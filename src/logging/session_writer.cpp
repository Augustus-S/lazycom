#include <lazycom/logging/session_writer.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <thread>
#include <tuple>
#include <unistd.h>
#include <utility>
#include <variant>

namespace lazycom::logging {
namespace {

constexpr std::string_view kLogPrefix = "lazycom-session-";
constexpr std::string_view kLogSuffix = ".ndjson";
constexpr std::string_view kLockName = ".lazycom-session.lock";

class FileDescriptor {
public:
  explicit FileDescriptor(int value = -1) noexcept : value_{value} {}
  ~FileDescriptor() {
    if (value_ >= 0) {
      static_cast<void>(::close(value_));
    }
  }
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept
      : value_{std::exchange(other.value_, -1)} {}
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      if (value_ >= 0) {
        static_cast<void>(::close(value_));
      }
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return value_; }
  [[nodiscard]] int release() noexcept { return std::exchange(value_, -1); }

private:
  int value_;
};

[[nodiscard]] Error log_error(std::string_view detail, int error_number = 0,
                              ErrorCode code = ErrorCode::LoggingDiskFull) {
  return make_error(
      code, Operation::WriteSessionLog, detail,
      error_number == 0
          ? std::error_code{}
          : std::error_code{error_number, std::generic_category()});
}

[[nodiscard]] Status log_failure(std::string_view detail, int error_number = 0,
                                 ErrorCode code = ErrorCode::LoggingDiskFull) {
  return tl::make_unexpected(log_error(detail, error_number, code));
}

[[nodiscard]] bool exact_file_mode(const struct stat &value) noexcept {
  return (value.st_mode & 07777) == (S_IRUSR | S_IWUSR);
}

[[nodiscard]] bool controlled_name(std::string_view name) noexcept {
  return name.size() > kLogPrefix.size() + kLogSuffix.size() &&
         name.starts_with(kLogPrefix) && name.ends_with(kLogSuffix);
}

[[nodiscard]] Status write_all(int descriptor, std::string_view bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto maximum =
        static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
    const auto amount = std::min(maximum, bytes.size() - offset);
    const auto count = ::write(descriptor, bytes.data() + offset, amount);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return log_failure("cannot write session log", errno);
    }
    if (count == 0) {
      return log_failure("session log write made no progress");
    }
    offset += static_cast<std::size_t>(count);
  }
  return {};
}

[[nodiscard]] std::string timestamp_component(std::string_view timestamp) {
  std::string result;
  result.reserve(timestamp.size());
  for (const char character : timestamp) {
    if ((character >= '0' && character <= '9') || character == 'T') {
      result.push_back(character);
    }
  }
  return result;
}

[[nodiscard]] Result<std::string> random_component() {
  std::array<unsigned char, 16> bytes{};
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count =
        ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return tl::make_unexpected(
          log_error("cannot obtain a random log filename", errno));
    }
    if (count == 0) {
      return tl::make_unexpected(
          log_error("random log filename generation made no progress"));
    }
    offset += static_cast<std::size_t>(count);
  }
  static constexpr std::string_view digits = "0123456789abcdef";
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (const auto byte : bytes) {
    result.push_back(digits[(byte >> 4U) & 0x0FU]);
    result.push_back(digits[byte & 0x0FU]);
  }
  return result;
}

struct Candidate {
  std::string name;
  std::string started_at;
  std::uint64_t size{};
  std::uint64_t device{};
  std::uint64_t inode{};
  std::int64_t mtime_seconds{};
  std::int64_t mtime_nanoseconds{};
  bool valid{};
  bool active{};
};

struct DirectoryInventory {
  std::uint64_t controlled_bytes{};
  std::size_t valid_files{};
  std::vector<Candidate> deletable;
};

struct DirectoryCloser {
  void operator()(DIR *directory) const noexcept {
    if (directory != nullptr) {
      static_cast<void>(::closedir(directory));
    }
  }
};

[[nodiscard]] bool same_identity(const struct stat &status,
                                 const Candidate &candidate) noexcept {
  return static_cast<std::uint64_t>(status.st_dev) == candidate.device &&
         static_cast<std::uint64_t>(status.st_ino) == candidate.inode &&
         status.st_size >= 0 &&
         static_cast<std::uint64_t>(status.st_size) == candidate.size &&
         static_cast<std::int64_t>(status.st_mtim.tv_sec) ==
             candidate.mtime_seconds &&
         static_cast<std::int64_t>(status.st_mtim.tv_nsec) ==
             candidate.mtime_nanoseconds;
}

class LinuxSessionLogFileSystem final : public SessionLogFileSystem {
public:
  ~LinuxSessionLogFileSystem() override { static_cast<void>(close()); }

  Status begin_session(const std::filesystem::path &directory,
                       std::string_view started_at,
                       std::string_view encoded_header,
                       const SessionLogQuotas &quotas) override {
    if (active_fd_.get() >= 0 || directory_fd_.get() >= 0) {
      return log_failure("session log filesystem is already active");
    }
    if (!directory.is_absolute() || directory.filename().empty() ||
        directory.native().find('\0') != std::string::npos) {
      return log_failure("session log directory must be an absolute path");
    }
    if (quotas.max_files == 0U || quotas.max_total_bytes == 0U ||
        quotas.max_file_bytes == 0U ||
        quotas.max_file_bytes > quotas.max_total_bytes ||
        encoded_header.empty() || !encoded_header.ends_with('\n') ||
        encoded_header.size() > quotas.max_file_bytes) {
      return log_failure("invalid session log limits or header");
    }

    const auto native = directory.native();
    std::error_code path_error;
    const bool directory_existed =
        std::filesystem::exists(directory, path_error);
    if (path_error) {
      return log_failure("cannot inspect session log directory",
                         path_error.value());
    }
    if (!directory_existed) {
      static_cast<void>(
          std::filesystem::create_directories(directory, path_error));
      if (path_error) {
        return log_failure("cannot create session log directory",
                           path_error.value());
      }
    }
    if (!directory_existed && ::chmod(native.c_str(), S_IRWXU) != 0) {
      return log_failure("cannot set session log directory mode 0700", errno);
    }
    const int directory_descriptor =
        ::open(native.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory_descriptor < 0) {
      return log_failure("cannot open session log directory", errno);
    }
    directory_fd_ = FileDescriptor{directory_descriptor};
    directory_path_ = directory;

    struct stat directory_status{};
    if (::fstat(directory_fd_.get(), &directory_status) != 0) {
      const int inspect_error = errno;
      reset_directory();
      return log_failure("cannot inspect session log directory", inspect_error);
    }
    if (!S_ISDIR(directory_status.st_mode) ||
        directory_status.st_uid != ::geteuid() ||
        ::faccessat(directory_fd_.get(), ".", W_OK | X_OK, AT_EACCESS) != 0) {
      const int inspect_error = errno;
      reset_directory();
      return log_failure(
          "session log directory must be user-owned, writable, and searchable",
          inspect_error);
    }

    const std::string lock_name{kLockName};
    bool created_lock = false;
    int lock_descriptor = ::openat(
        directory_fd_.get(), lock_name.c_str(),
        O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
    if (lock_descriptor >= 0) {
      created_lock = true;
    } else if (errno == EEXIST) {
      lock_descriptor = ::openat(directory_fd_.get(), lock_name.c_str(),
                                 O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    }
    if (lock_descriptor < 0) {
      const int open_error = errno;
      reset_directory();
      return log_failure("cannot open session log directory lock", open_error);
    }
    lock_fd_ = FileDescriptor{lock_descriptor};
    if (created_lock && ::fchmod(lock_fd_.get(), S_IRUSR | S_IWUSR) != 0) {
      const int mode_error = errno;
      reset_directory();
      return log_failure("cannot set session log lock mode 0600", mode_error);
    }
    struct stat lock_status{};
    if (::fstat(lock_fd_.get(), &lock_status) != 0 ||
        !S_ISREG(lock_status.st_mode) || lock_status.st_uid != ::geteuid() ||
        !exact_file_mode(lock_status)) {
      const int inspect_error = errno;
      reset_directory();
      return log_failure("session log lock must be owned and mode 0600",
                         inspect_error);
    }
    if (::flock(lock_fd_.get(), LOCK_EX | LOCK_NB) != 0) {
      const int lock_error = errno;
      reset_directory();
      return log_failure("session log directory is locked", lock_error);
    }

    quotas_ = quotas;
    started_at_ = std::string{started_at};
    header_ = std::string{encoded_header};
    auto capacity = ensure_capacity(1U, header_.size());
    if (!capacity) {
      reset_directory();
      return capacity;
    }
    auto created = create_active_file();
    if (!created) {
      reset_directory();
      return created;
    }
    return {};
  }

  Status append_line(std::string_view encoded_record) override {
    if (active_fd_.get() < 0 || encoded_record.empty() ||
        !encoded_record.ends_with('\n')) {
      return log_failure("session log append is not valid while inactive");
    }
    const auto amount = static_cast<std::uint64_t>(encoded_record.size());
    const auto header_size = static_cast<std::uint64_t>(header_.size());
    if (amount > quotas_.max_file_bytes - header_size) {
      return log_failure("one session record exceeds the file quota");
    }
    if (amount > quotas_.max_file_bytes - active_size_) {
      auto closed = close_active();
      if (!closed) {
        return closed;
      }
      auto capacity = ensure_capacity(1U, header_size + amount);
      if (!capacity) {
        return capacity;
      }
      auto created = create_active_file();
      if (!created) {
        return created;
      }
    } else {
      auto capacity = ensure_capacity(0U, amount);
      if (!capacity) {
        return capacity;
      }
    }
    auto written = write_all(active_fd_.get(), encoded_record);
    if (!written) {
      return written;
    }
    active_size_ += amount;
    return {};
  }

  Status flush() override {
    // append_line uses unbuffered write(2), so no process userspace bytes
    // remain.
    return active_fd_.get() >= 0 ? Status{} : Status{};
  }

  Status close() noexcept override {
    Status result;
    try {
      result = close_active();
    } catch (...) {
      result = log_failure("session log close failed");
    }
    reset_directory();
    return result;
  }

  [[nodiscard]] std::filesystem::path active_path() const override {
    return active_name_.empty() ? std::filesystem::path{}
                                : directory_path_ / active_name_;
  }

private:
  [[nodiscard]] Result<Candidate> inspect_log(const std::string &name,
                                              const struct stat &listed) const {
    Candidate candidate{
        name,
        {},
        listed.st_size < 0 ? 0U : static_cast<std::uint64_t>(listed.st_size),
        static_cast<std::uint64_t>(listed.st_dev),
        static_cast<std::uint64_t>(listed.st_ino),
        static_cast<std::int64_t>(listed.st_mtim.tv_sec),
        static_cast<std::int64_t>(listed.st_mtim.tv_nsec),
        false,
        name == active_name_};
    if (candidate.active) {
      candidate.valid = true;
      candidate.started_at = started_at_;
      return candidate;
    }
    if (!S_ISREG(listed.st_mode) || listed.st_uid != ::geteuid() ||
        !exact_file_mode(listed)) {
      return candidate;
    }
    const int descriptor = ::openat(directory_fd_.get(), name.c_str(),
                                    O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0) {
      return candidate;
    }
    FileDescriptor file{descriptor};
    struct stat opened{};
    if (::fstat(file.get(), &opened) != 0 ||
        !same_identity(opened, candidate)) {
      return candidate;
    }

    std::array<char, 16384> buffer{};
    std::string pending;
    bool header_seen = false;
    std::uint64_t last_seq = 0U;
    while (true) {
      const auto count = ::read(file.get(), buffer.data(), buffer.size());
      if (count < 0) {
        if (errno == EINTR) {
          continue;
        }
        return candidate;
      }
      if (count == 0) {
        break;
      }
      pending.append(buffer.data(), static_cast<std::size_t>(count));
      std::size_t consumed = 0;
      while (true) {
        const auto newline = pending.find('\n', consumed);
        if (newline == std::string::npos) {
          pending.erase(0, consumed);
          break;
        }
        const auto length = newline - consumed + 1U;
        if (length > kMaxPhysicalLineBytes) {
          return candidate;
        }
        const std::string_view line{pending.data() + consumed, length};
        if (!header_seen) {
          auto header = decode_header_line(line);
          if (!header) {
            return candidate;
          }
          candidate.started_at = std::move(header->started_at);
          header_seen = true;
        } else {
          auto record = decode_record_line(line);
          if (!record || record->seq <= last_seq) {
            return candidate;
          }
          last_seq = record->seq;
        }
        consumed = newline + 1U;
        if (consumed == pending.size()) {
          pending.clear();
          break;
        }
      }
      if (pending.size() > kMaxPhysicalLineBytes) {
        return candidate;
      }
    }
    if (!header_seen || !pending.empty()) {
      return candidate;
    }
    candidate.valid = true;
    return candidate;
  }

  [[nodiscard]] Result<DirectoryInventory> inventory() const {
    // dup(2) would share the directory stream offset, making every scan after
    // the first appear empty. Opening "." creates an independent description.
    const int duplicate =
        ::openat(directory_fd_.get(), ".",
                 O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (duplicate < 0) {
      return tl::make_unexpected(
          log_error("cannot duplicate session log directory", errno));
    }
    DIR *raw_directory = ::fdopendir(duplicate);
    if (raw_directory == nullptr) {
      const int open_error = errno;
      static_cast<void>(::close(duplicate));
      return tl::make_unexpected(
          log_error("cannot enumerate session log directory", open_error));
    }
    std::unique_ptr<DIR, DirectoryCloser> entries{raw_directory};
    DirectoryInventory result;
    errno = 0;
    while (const auto *entry = ::readdir(entries.get())) {
      const std::string name{entry->d_name};
      if (!controlled_name(name)) {
        continue;
      }
      struct stat status{};
      if (::fstatat(directory_fd_.get(), name.c_str(), &status,
                    AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) {
          continue;
        }
        return tl::make_unexpected(
            log_error("cannot inspect a session log quota entry", errno));
      }
      const auto size =
          status.st_size < 0 ? 0U : static_cast<std::uint64_t>(status.st_size);
      if (size >
          std::numeric_limits<std::uint64_t>::max() - result.controlled_bytes) {
        result.controlled_bytes = std::numeric_limits<std::uint64_t>::max();
      } else {
        result.controlled_bytes += size;
      }
      auto candidate = inspect_log(name, status);
      if (!candidate) {
        return tl::make_unexpected(candidate.error());
      }
      if (candidate->valid) {
        ++result.valid_files;
        if (!candidate->active) {
          result.deletable.push_back(std::move(*candidate));
        }
      }
      errno = 0;
    }
    if (errno != 0) {
      return tl::make_unexpected(
          log_error("cannot enumerate session log directory", errno));
    }
    std::ranges::sort(result.deletable,
                      [](const Candidate &left, const Candidate &right) {
                        return std::tie(left.started_at, left.mtime_seconds,
                                        left.mtime_nanoseconds, left.name) <
                               std::tie(right.started_at, right.mtime_seconds,
                                        right.mtime_nanoseconds, right.name);
                      });
    return result;
  }

  [[nodiscard]] Status ensure_capacity(std::size_t additional_files,
                                       std::uint64_t additional_bytes) {
    auto observed = inventory();
    if (!observed) {
      return tl::make_unexpected(observed.error());
    }
    auto files = observed->valid_files;
    auto bytes = observed->controlled_bytes;
    std::size_t index = 0;
    const auto exceeds = [&] {
      const bool file_limit = additional_files > quotas_.max_files ||
                              files > quotas_.max_files - additional_files;
      const bool byte_limit =
          additional_bytes > quotas_.max_total_bytes ||
          bytes > quotas_.max_total_bytes - additional_bytes;
      return file_limit || byte_limit;
    };
    while (exceeds()) {
      if (index >= observed->deletable.size()) {
        return log_failure(
            "session log quota cannot be met without deleting an active, "
            "unknown, or damaged file");
      }
      const auto &candidate = observed->deletable[index++];
      struct stat current{};
      if (::fstatat(directory_fd_.get(), candidate.name.c_str(), &current,
                    AT_SYMLINK_NOFOLLOW) != 0 ||
          !S_ISREG(current.st_mode) || current.st_uid != ::geteuid() ||
          !exact_file_mode(current) || !same_identity(current, candidate)) {
        return log_failure("session log changed while enforcing quota", errno);
      }
      if (::unlinkat(directory_fd_.get(), candidate.name.c_str(), 0) != 0) {
        return log_failure("cannot delete an old verified session log", errno);
      }
      --files;
      bytes = candidate.size > bytes ? 0U : bytes - candidate.size;
    }
    return {};
  }

  [[nodiscard]] Status create_active_file() {
    for (std::uint32_t attempt = 0; attempt < 128U; ++attempt) {
      auto random = random_component();
      if (!random) {
        return tl::make_unexpected(random.error());
      }
      active_name_ = std::string{kLogPrefix} +
                     timestamp_component(started_at_) + "-" + *random +
                     std::string{kLogSuffix};
      const int descriptor =
          ::openat(directory_fd_.get(), active_name_.c_str(),
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                   S_IRUSR | S_IWUSR);
      if (descriptor < 0) {
        const int open_error = errno;
        active_name_.clear();
        if (open_error == EEXIST) {
          continue;
        }
        return log_failure("cannot exclusively create a session log",
                           open_error);
      }
      active_fd_ = FileDescriptor{descriptor};
      if (::fchmod(active_fd_.get(), S_IRUSR | S_IWUSR) != 0) {
        return log_failure("cannot set session log mode 0600", errno);
      }
      auto written = write_all(active_fd_.get(), header_);
      if (!written) {
        return written;
      }
      active_size_ = header_.size();
      return {};
    }
    return log_failure("cannot allocate a unique session log filename");
  }

  [[nodiscard]] Status close_active() noexcept {
    if (active_fd_.get() < 0) {
      active_name_.clear();
      active_size_ = 0U;
      return {};
    }
    const int descriptor = active_fd_.release();
    active_name_.clear();
    active_size_ = 0U;
    if (::close(descriptor) != 0) {
      return log_failure("cannot close session log", errno);
    }
    return {};
  }

  void reset_directory() noexcept {
    active_fd_ = FileDescriptor{};
    active_name_.clear();
    active_size_ = 0U;
    if (lock_fd_.get() >= 0) {
      static_cast<void>(::flock(lock_fd_.get(), LOCK_UN));
    }
    lock_fd_ = FileDescriptor{};
    directory_fd_ = FileDescriptor{};
    directory_path_.clear();
    header_.clear();
    started_at_.clear();
  }

  FileDescriptor directory_fd_;
  FileDescriptor lock_fd_;
  FileDescriptor active_fd_;
  std::filesystem::path directory_path_;
  std::string active_name_;
  std::string started_at_;
  std::string header_;
  SessionLogQuotas quotas_;
  std::uint64_t active_size_{};
};

template <class T> [[nodiscard]] std::future<T> ready_future(T value) {
  std::promise<T> promise;
  auto future = promise.get_future();
  promise.set_value(std::move(value));
  return future;
}

[[nodiscard]] std::size_t batch_bytes(const std::vector<Record> &batch) {
  const auto maximum = std::numeric_limits<std::size_t>::max();
  if (batch.capacity() > (maximum - 128U) / sizeof(Record)) {
    return maximum;
  }
  std::size_t total = 128U + sizeof(Record) * batch.capacity();
  for (const auto &record : batch) {
    std::size_t add = record.payload.capacity();
    const std::array capacities{
        record.time_utc.capacity(), record.message.capacity(),
        record.code ? record.code->capacity() : std::size_t{0U}};
    for (const auto capacity : capacities) {
      if (capacity > maximum - add) {
        return maximum;
      }
      add += capacity;
    }
    if (add > maximum - total) {
      return std::numeric_limits<std::size_t>::max();
    }
    total += add;
  }
  return total;
}

[[nodiscard]] BarrierResult failed_barrier(std::uint64_t target,
                                           const BarrierTracker &tracker) {
  return {
      BarrierState::WriterFailed,      target,
      tracker.processed_through_seq(), tracker.flush_attempted_through_seq(),
      tracker.flushed_through_seq(),   false};
}

} // namespace

std::unique_ptr<SessionLogFileSystem> make_linux_session_log_file_system() {
  return std::make_unique<LinuxSessionLogFileSystem>();
}

struct SessionWriter::Impl {
  struct BatchItem {
    std::vector<Record> records;
    std::size_t bytes{};
  };
  struct StartItem {
    Header header;
    std::promise<SessionCommandResult> promise;
  };
  struct CloseItem {
    SessionLogState final_state{SessionLogState::Waiting};
    std::promise<SessionCommandResult> promise;
  };
  struct BarrierItem {
    std::uint64_t target{};
    std::promise<BarrierResult> promise;
  };
  struct ShutdownItem {
    std::promise<SessionCommandResult> promise;
  };
  using Item =
      std::variant<BatchItem, StartItem, CloseItem, BarrierItem, ShutdownItem>;

  struct PendingBarrier {
    std::uint64_t target{};
    std::promise<BarrierResult> promise;
  };

  Impl(SessionWriterOptions writer_options,
       std::unique_ptr<SessionLogFileSystem> writer_file_system)
      : options{std::move(writer_options)},
        file_system{std::move(writer_file_system)} {
    if (!file_system || options.queue_max_records == 0U ||
        options.queue_max_bytes == 0U || options.flush_batch_bytes == 0U ||
        options.flush_interval <= std::chrono::milliseconds::zero() ||
        options.quotas.max_files == 0U ||
        options.quotas.max_total_bytes == 0U ||
        options.quotas.max_file_bytes == 0U ||
        options.quotas.max_file_bytes > options.quotas.max_total_bytes) {
      throw std::invalid_argument{"invalid session writer options"};
    }
    worker = std::jthread{[this] { run(); }};
  }

  [[nodiscard]] SessionCommandResult command_result_locked() const {
    return {state.load(std::memory_order_acquire),
            processed.load(std::memory_order_acquire), terminal_error};
  }

  [[nodiscard]] SessionCommandResult command_result() const {
    std::scoped_lock lock{mutex};
    return command_result_locked();
  }

  void set_error(Error error) {
    {
      std::scoped_lock lock{mutex};
      if (!terminal_error) {
        terminal_error = std::move(error);
      }
      state.store(SessionLogState::Error, std::memory_order_release);
    }
    condition.notify_all();
  }

  void complete_failed_barriers() {
    for (auto &barrier : pending_barriers) {
      barrier.promise.set_value(failed_barrier(barrier.target, tracker));
    }
    pending_barriers.clear();
  }

  [[nodiscard]] bool flush_active() {
    if (!worker_active) {
      return true;
    }
    auto status = file_system->flush();
    tracker.mark_flush_result(static_cast<bool>(status));
    bytes_since_flush = 0U;
    next_flush = std::chrono::steady_clock::now() + options.flush_interval;
    if (!status) {
      set_error(status.error());
      complete_failed_barriers();
      static_cast<void>(file_system->close());
      worker_active = false;
      std::scoped_lock lock{mutex};
      active_file.clear();
      return false;
    }
    for (auto iterator = pending_barriers.begin();
         iterator != pending_barriers.end();) {
      if (iterator->target <= tracker.flushed_through_seq()) {
        iterator->promise.set_value(tracker.check({iterator->target}));
        iterator = pending_barriers.erase(iterator);
      } else {
        ++iterator;
      }
    }
    return true;
  }

  void observe_overload() {
    if (!overloaded.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    set_error(log_error("session log queue reached its hard limit"));
    complete_failed_barriers();
    if (worker_active) {
      static_cast<void>(file_system->close());
      worker_active = false;
      std::scoped_lock lock{mutex};
      active_file.clear();
    }
  }

  void handle(BatchItem item) {
    {
      std::scoped_lock lock{mutex};
      queued_record_count -= item.records.size();
      queued_byte_count -= item.bytes;
    }
    observe_overload();
    if (!worker_active ||
        state.load(std::memory_order_acquire) == SessionLogState::Error) {
      return;
    }
    for (const auto &record : item.records) {
      const bool included =
          (record.direction != Direction::Sys || options.include_system) &&
          (record.direction != Direction::Err || options.include_error);
      if (included) {
        auto encoded = encode_record_line(record);
        if (!encoded) {
          set_error(log_error(encoded.error().detail, 0,
                              ErrorCode::LoggingSchemaInvalid));
          complete_failed_barriers();
          static_cast<void>(file_system->close());
          worker_active = false;
          std::scoped_lock lock{mutex};
          active_file.clear();
          return;
        }
        auto status = file_system->append_line(*encoded);
        if (!status) {
          set_error(status.error());
          complete_failed_barriers();
          static_cast<void>(file_system->close());
          worker_active = false;
          std::scoped_lock lock{mutex};
          active_file.clear();
          return;
        }
        bytes_since_flush += encoded->size();
      }
      static_cast<void>(tracker.mark_processed_through(record.seq));
      processed.store(tracker.processed_through_seq(),
                      std::memory_order_release);
    }
    const bool barrier_ready = std::ranges::any_of(
        pending_barriers, [this](const PendingBarrier &barrier) {
          return barrier.target <= tracker.processed_through_seq();
        });
    if (bytes_since_flush >= options.flush_batch_bytes || barrier_ready ||
        std::chrono::steady_clock::now() >= next_flush) {
      static_cast<void>(flush_active());
    }
  }

  void handle(StartItem item) {
    observe_overload();
    if (state.load(std::memory_order_acquire) != SessionLogState::Waiting) {
      SessionCommandResult result;
      {
        std::scoped_lock lock{mutex};
        start_pending = false;
        result = command_result_locked();
      }
      item.promise.set_value(std::move(result));
      return;
    }
    auto encoded = encode_header_line(item.header);
    if (!encoded) {
      set_error(log_error(encoded.error().detail, 0,
                          ErrorCode::LoggingSchemaInvalid));
      {
        std::scoped_lock lock{mutex};
        start_pending = false;
      }
      item.promise.set_value(command_result());
      return;
    }
    auto status = file_system->begin_session(
        options.directory, item.header.started_at, *encoded, options.quotas);
    if (!status) {
      set_error(status.error());
      {
        std::scoped_lock lock{mutex};
        start_pending = false;
      }
      item.promise.set_value(command_result());
      return;
    }
    worker_active = true;
    tracker = BarrierTracker{};
    processed.store(0U, std::memory_order_release);
    bytes_since_flush = encoded->size();
    next_flush = std::chrono::steady_clock::now() + options.flush_interval;
    bool cancelled = false;
    {
      std::scoped_lock lock{mutex};
      start_pending = false;
      cancelled =
          state.load(std::memory_order_acquire) != SessionLogState::Waiting;
      if (!cancelled) {
        active_file = file_system->active_path();
        last_enqueued_seq = 0U;
        terminal_error.reset();
        state.store(SessionLogState::Recording, std::memory_order_release);
      }
    }
    if (cancelled) {
      static_cast<void>(file_system->close());
      worker_active = false;
    }
    item.promise.set_value(command_result());
  }

  void handle(CloseItem item) {
    observe_overload();
    if (worker_active) {
      static_cast<void>(flush_active());
      auto status = file_system->close();
      worker_active = false;
      if (!status) {
        set_error(status.error());
      }
    }
    complete_failed_barriers();
    {
      std::scoped_lock lock{mutex};
      close_pending = false;
      active_file.clear();
      if (state.load(std::memory_order_acquire) != SessionLogState::Error) {
        state.store(item.final_state, std::memory_order_release);
      } else if (item.final_state == SessionLogState::Off) {
        terminal_error.reset();
        state.store(SessionLogState::Off, std::memory_order_release);
      }
    }
    item.promise.set_value(command_result());
  }

  void handle(BarrierItem item) {
    observe_overload();
    if (!worker_active ||
        state.load(std::memory_order_acquire) == SessionLogState::Error) {
      item.promise.set_value(failed_barrier(item.target, tracker));
      return;
    }
    if (item.target <= tracker.processed_through_seq()) {
      static_cast<void>(flush_active());
      if (state.load(std::memory_order_acquire) == SessionLogState::Error) {
        item.promise.set_value(failed_barrier(item.target, tracker));
      } else {
        item.promise.set_value(tracker.check({item.target}));
      }
      return;
    }
    pending_barriers.push_back({item.target, std::move(item.promise)});
  }

  [[nodiscard]] bool handle(ShutdownItem item) {
    observe_overload();
    if (worker_active) {
      static_cast<void>(flush_active());
      auto status = file_system->close();
      worker_active = false;
      if (!status) {
        set_error(status.error());
      }
    }
    complete_failed_barriers();
    {
      std::scoped_lock lock{mutex};
      active_file.clear();
      if (state.load(std::memory_order_acquire) != SessionLogState::Error) {
        state.store(SessionLogState::Off, std::memory_order_release);
      }
    }
    item.promise.set_value(command_result());
    return true;
  }

  void run() {
    while (true) {
      std::optional<Item> item;
      {
        std::unique_lock lock{mutex};
        if (worker_active && bytes_since_flush != 0U) {
          condition.wait_until(lock, next_flush,
                               [this] { return !items.empty() || overloaded; });
        } else {
          condition.wait(lock, [this] { return !items.empty() || overloaded; });
        }
        if (!items.empty()) {
          item.emplace(std::move(items.front()));
          items.pop_front();
        }
      }
      observe_overload();
      if (!item) {
        if (worker_active && bytes_since_flush != 0U &&
            std::chrono::steady_clock::now() >= next_flush) {
          static_cast<void>(flush_active());
        }
        continue;
      }
      bool stop = false;
      std::visit(
          [this, &stop](auto value) mutable {
            using Value = decltype(value);
            if constexpr (std::is_same_v<Value, ShutdownItem>) {
              stop = handle(std::move(value));
            } else {
              handle(std::move(value));
            }
          },
          std::move(*item));
      if (stop) {
        return;
      }
    }
  }

  SessionWriterOptions options;
  std::unique_ptr<SessionLogFileSystem> file_system;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::deque<Item> items;
  std::vector<PendingBarrier> pending_barriers;
  std::atomic<SessionLogState> state{SessionLogState::Off};
  std::atomic<std::uint64_t> processed{};
  std::atomic<bool> overloaded{};
  std::jthread worker;
  BarrierTracker tracker;
  std::optional<Error> terminal_error;
  std::filesystem::path active_file;
  std::size_t queued_record_count{};
  std::size_t queued_byte_count{};
  std::size_t bytes_since_flush{};
  std::uint64_t last_enqueued_seq{};
  std::chrono::steady_clock::time_point next_flush{};
  bool worker_active{};
  bool stopping{};
  bool start_pending{};
  bool close_pending{};
};

SessionWriter::SessionWriter(SessionWriterOptions options,
                             std::unique_ptr<SessionLogFileSystem> file_system)
    : impl_{
          std::make_unique<Impl>(std::move(options), std::move(file_system))} {}

SessionWriter::~SessionWriter() {
  if (!impl_) {
    return;
  }
  auto completion = shutdown();
  completion.wait();
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
}

bool SessionWriter::enable() noexcept {
  std::scoped_lock lock{impl_->mutex};
  if (impl_->stopping) {
    return false;
  }
  if (impl_->close_pending) {
    return false;
  }
  const auto current = impl_->state.load(std::memory_order_acquire);
  if (current == SessionLogState::Error) {
    return false;
  }
  if (current == SessionLogState::Off) {
    impl_->terminal_error.reset();
    impl_->state.store(SessionLogState::Waiting, std::memory_order_release);
  }
  return true;
}

std::future<SessionCommandResult> SessionWriter::disable() {
  Impl::CloseItem item;
  auto future = item.promise.get_future();
  {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->stopping) {
      return ready_future(impl_->command_result_locked());
    }
    impl_->state.store(SessionLogState::Off, std::memory_order_release);
    impl_->close_pending = true;
    item.final_state = SessionLogState::Off;
    impl_->items.emplace_back(std::move(item));
  }
  impl_->condition.notify_one();
  return future;
}

std::future<SessionCommandResult> SessionWriter::start_session(Header header) {
  Impl::StartItem item{std::move(header), {}};
  auto future = item.promise.get_future();
  {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->stopping ||
        impl_->state.load(std::memory_order_acquire) !=
            SessionLogState::Waiting ||
        impl_->start_pending || impl_->close_pending) {
      return ready_future(impl_->command_result_locked());
    }
    impl_->start_pending = true;
    impl_->items.emplace_back(std::move(item));
  }
  impl_->condition.notify_one();
  return future;
}

std::future<SessionCommandResult> SessionWriter::end_session() {
  Impl::CloseItem item;
  auto future = item.promise.get_future();
  {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->stopping ||
        impl_->state.load(std::memory_order_acquire) !=
            SessionLogState::Recording ||
        impl_->close_pending) {
      return ready_future(impl_->command_result_locked());
    }
    impl_->state.store(SessionLogState::Waiting, std::memory_order_release);
    impl_->close_pending = true;
    item.final_state = SessionLogState::Waiting;
    impl_->items.emplace_back(std::move(item));
  }
  impl_->condition.notify_one();
  return future;
}

EnqueueResult SessionWriter::try_enqueue(std::vector<Record> batch) noexcept {
  if (batch.empty()) {
    return EnqueueResult::EmptyBatch;
  }
  try {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->stopping) {
      return EnqueueResult::Stopping;
    }
    if (impl_->state.load(std::memory_order_acquire) !=
        SessionLogState::Recording) {
      return EnqueueResult::NotRecording;
    }
    auto previous = impl_->last_enqueued_seq;
    for (const auto &record : batch) {
      if (record.seq == 0U || record.seq <= previous) {
        return EnqueueResult::InvalidBatch;
      }
      previous = record.seq;
    }
    const auto bytes = batch_bytes(batch);
    if (batch.size() > impl_->options.queue_max_records ||
        impl_->queued_record_count >
            impl_->options.queue_max_records - batch.size() ||
        bytes > impl_->options.queue_max_bytes ||
        impl_->queued_byte_count > impl_->options.queue_max_bytes - bytes) {
      impl_->overloaded.store(true, std::memory_order_release);
      impl_->state.store(SessionLogState::Error, std::memory_order_release);
      impl_->condition.notify_one();
      return EnqueueResult::QueueFull;
    }
    impl_->last_enqueued_seq = previous;
    impl_->queued_record_count += batch.size();
    impl_->queued_byte_count += bytes;
    impl_->items.emplace_back(Impl::BatchItem{std::move(batch), bytes});
    impl_->condition.notify_one();
    return EnqueueResult::Accepted;
  } catch (...) {
    impl_->overloaded.store(true, std::memory_order_release);
    impl_->state.store(SessionLogState::Error, std::memory_order_release);
    impl_->condition.notify_one();
    return EnqueueResult::QueueFull;
  }
}

std::future<BarrierResult> SessionWriter::barrier(std::uint64_t target_seq) {
  Impl::BarrierItem item{target_seq, {}};
  auto future = item.promise.get_future();
  {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->stopping) {
      const auto processed = impl_->processed.load(std::memory_order_acquire);
      return ready_future(BarrierResult{BarrierState::WriterFailed, target_seq,
                                        processed, processed, processed,
                                        false});
    }
    impl_->items.emplace_back(std::move(item));
  }
  impl_->condition.notify_one();
  return future;
}

std::future<SessionCommandResult> SessionWriter::shutdown() {
  Impl::ShutdownItem item;
  auto future = item.promise.get_future();
  {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->stopping) {
      return ready_future(impl_->command_result_locked());
    }
    impl_->stopping = true;
    impl_->items.emplace_back(std::move(item));
  }
  impl_->condition.notify_one();
  return future;
}

SessionLogState SessionWriter::state() const noexcept {
  return impl_->state.load(std::memory_order_acquire);
}

std::uint64_t SessionWriter::processed_through_seq() const noexcept {
  return impl_->processed.load(std::memory_order_acquire);
}

std::size_t SessionWriter::queued_records() const noexcept {
  std::scoped_lock lock{impl_->mutex};
  return impl_->queued_record_count;
}

std::size_t SessionWriter::queued_bytes() const noexcept {
  std::scoped_lock lock{impl_->mutex};
  return impl_->queued_byte_count;
}

std::filesystem::path SessionWriter::active_path() const {
  std::scoped_lock lock{impl_->mutex};
  return impl_->active_file;
}

} // namespace lazycom::logging
