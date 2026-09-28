#include <lazycom/logging/session_writer.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>

#include "../platform/file_descriptor.hpp"
#include "session_error.hpp"

namespace lazycom::logging {
namespace {

using platform::FileDescriptor;

constexpr std::string_view kLogPrefix = "lazycom-session-";
constexpr std::string_view kLogSuffix = ".ndjson";
constexpr std::string_view kLockName = ".lazycom-session.lock";

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

struct DirectoryStamp {
  std::uint64_t device{};
  std::uint64_t inode{};
  std::int64_t mtime_seconds{};
  std::int64_t mtime_nanoseconds{};
  std::int64_t ctime_seconds{};
  std::int64_t ctime_nanoseconds{};
  auto operator<=>(const DirectoryStamp &) const = default;
};

struct DirectoryCloser {
  void operator()(DIR *directory) const noexcept {
    if (directory != nullptr) {
      static_cast<void>(::closedir(directory));
    }
  }
};

[[nodiscard]] Result<FileDescriptor>
open_or_create_log_directory(const std::filesystem::path &path) {
  const auto native = path.native();
  if (native.find('\0') != std::string::npos || !path.is_absolute() ||
      path == path.root_path() || path.filename().empty()) {
    return tl::make_unexpected(log_error("invalid session log directory path"));
  }
  const int root = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (root < 0) {
    return tl::make_unexpected(
        log_error("cannot open filesystem root for session logs", errno));
  }
  FileDescriptor current{root};
  const auto relative = path.relative_path();
  for (auto iterator = relative.begin(); iterator != relative.end();
       ++iterator) {
    const auto component = iterator->native();
    if (component.empty() || component == "." || component == "..") {
      return tl::make_unexpected(
          log_error("invalid session log directory component"));
    }
    const bool final = std::next(iterator) == relative.end();
    bool created = false;
    int descriptor = ::openat(current.get(), component.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 && errno == ENOENT) {
      if (::mkdirat(current.get(), component.c_str(), S_IRWXU) != 0) {
        return tl::make_unexpected(
            log_error("cannot create session log directory", errno));
      }
      created = true;
      if (::fchmodat(current.get(), component.c_str(), S_IRWXU,
                     AT_SYMLINK_NOFOLLOW) != 0) {
        return tl::make_unexpected(
            log_error("cannot set session log directory mode 0700", errno));
      }
      descriptor = ::openat(current.get(), component.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (descriptor < 0) {
      return tl::make_unexpected(
          log_error("cannot open session log directory component", errno));
    }
    FileDescriptor next{descriptor};
    if (created && ::fchmod(next.get(), S_IRWXU) != 0) {
      return tl::make_unexpected(
          log_error("cannot set session log directory mode 0700", errno));
    }
    struct stat status{};
    if (::fstat(next.get(), &status) != 0 || !S_ISDIR(status.st_mode)) {
      return tl::make_unexpected(
          log_error("cannot inspect session log directory component", errno));
    }
    if (created &&
        (status.st_uid != ::geteuid() || (status.st_mode & 07777) != S_IRWXU)) {
      return tl::make_unexpected(log_error(
          "new session log directory must be user-owned and mode 0700"));
    }
    if (final && (status.st_uid != ::geteuid() ||
                  (status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
                  ::faccessat(next.get(), ".", W_OK | X_OK, AT_EACCESS) != 0)) {
      return tl::make_unexpected(log_error(
          "session log directory must be user-owned, private from writes, "
          "writable, and searchable",
          errno));
    }
    current = std::move(next);
  }
  return current;
}

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

    auto opened_directory = open_or_create_log_directory(directory);
    if (!opened_directory) {
      return tl::make_unexpected(opened_directory.error());
    }
    directory_fd_ = std::move(*opened_directory);
    directory_path_ = directory;

    struct stat directory_status{};
    if (::fstat(directory_fd_.get(), &directory_status) != 0) {
      const int inspect_error = errno;
      reset_directory();
      return log_failure("cannot inspect session log directory", inspect_error);
    }
    if (!S_ISDIR(directory_status.st_mode) ||
        directory_status.st_uid != ::geteuid() ||
        (directory_status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
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

    const int watch_descriptor = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (watch_descriptor < 0) {
      const int watch_error = errno;
      reset_directory();
      return log_failure("cannot monitor session log directory", watch_error);
    }
    inventory_watch_fd_ = FileDescriptor{watch_descriptor};
    const auto descriptor_path =
        "/proc/self/fd/" + std::to_string(directory_fd_.get());
    constexpr std::uint32_t watch_mask =
        IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE |
        IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF;
    if (::inotify_add_watch(inventory_watch_fd_.get(), descriptor_path.c_str(),
                            watch_mask) < 0) {
      const int watch_error = errno;
      reset_directory();
      return log_failure("cannot watch session log directory", watch_error);
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
    auto refreshed = refresh_inventory();
    if (!refreshed) {
      return refreshed;
    }
    if (amount > quotas_.max_file_bytes - active_size_) {
      auto closed = close_active();
      if (!closed) {
        return closed;
      }
      inventory_cached_ = false;
      auto capacity = ensure_capacity(1U, header_size + amount);
      if (!capacity) {
        return capacity;
      }
      auto created = create_active_file();
      if (!created) {
        return created;
      }
    } else {
      auto capacity = enforce_capacity(0U, amount);
      if (!capacity) {
        return capacity;
      }
    }
    auto written = write_all(active_fd_.get(), encoded_record);
    if (!written) {
      return written;
    }
    active_size_ += amount;
    if (inventory_cached_) {
      inventory_cache_.controlled_bytes += amount;
    }
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
      ++result.valid_files;
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

  [[nodiscard]] Result<DirectoryStamp> directory_stamp() const {
    struct stat status{};
    if (::fstat(directory_fd_.get(), &status) != 0) {
      return tl::make_unexpected(
          log_error("cannot inspect session log directory state", errno));
    }
    return DirectoryStamp{static_cast<std::uint64_t>(status.st_dev),
                          static_cast<std::uint64_t>(status.st_ino),
                          static_cast<std::int64_t>(status.st_mtim.tv_sec),
                          static_cast<std::int64_t>(status.st_mtim.tv_nsec),
                          static_cast<std::int64_t>(status.st_ctim.tv_sec),
                          static_cast<std::int64_t>(status.st_ctim.tv_nsec)};
  }

  [[nodiscard]] Result<bool> inventory_watch_changed() const {
    bool changed = false;
    alignas(struct inotify_event) std::array<char, 4096> events{};
    while (true) {
      const auto count =
          ::read(inventory_watch_fd_.get(), events.data(), events.size());
      if (count > 0) {
        std::size_t offset = 0U;
        const auto available = static_cast<std::size_t>(count);
        while (offset <= available &&
               available - offset >= sizeof(inotify_event)) {
          const auto *event = reinterpret_cast<const inotify_event *>(
              events.data() + static_cast<std::ptrdiff_t>(offset));
          if ((event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED)) !=
              0U) {
            return tl::make_unexpected(log_error(
                "session log directory was moved, deleted, or unwatched"));
          }
          changed = true;
          const auto event_size = sizeof(inotify_event) + event->len;
          if (event_size > available - offset) {
            return tl::make_unexpected(
                log_error("invalid session log directory monitor event"));
          }
          offset += event_size;
        }
        changed = true;
        continue;
      }
      if (count == 0 ||
          (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
        return changed;
      }
      if (errno == EINTR) {
        continue;
      }
      return tl::make_unexpected(
          log_error("cannot read session log directory monitor", errno));
    }
  }

  [[nodiscard]] Status synchronize_active_file() {
    if (active_fd_.get() < 0) {
      return {};
    }
    struct stat opened{};
    struct stat named{};
    if (::fstat(active_fd_.get(), &opened) != 0 ||
        ::fstatat(directory_fd_.get(), active_name_.c_str(), &named,
                  AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(opened.st_mode) || !S_ISREG(named.st_mode) ||
        opened.st_uid != ::geteuid() || named.st_uid != ::geteuid() ||
        !exact_file_mode(opened) || !exact_file_mode(named) ||
        opened.st_dev != named.st_dev || opened.st_ino != named.st_ino ||
        opened.st_size < 0 || opened.st_size != named.st_size) {
      return log_failure("active session log changed unexpectedly", errno);
    }
    const auto size = static_cast<std::uint64_t>(opened.st_size);
    if (size < header_.size() || size > quotas_.max_file_bytes) {
      return log_failure("active session log size is outside its quota");
    }
    active_size_ = size;
    return {};
  }

  [[nodiscard]] Status refresh_inventory() {
    auto before = directory_stamp();
    if (!before) {
      return tl::make_unexpected(before.error());
    }
    auto watch_changed = inventory_watch_changed();
    if (!watch_changed) {
      return tl::make_unexpected(watch_changed.error());
    }
    if (inventory_cached_ && *before == inventory_stamp_ && !*watch_changed) {
      return {};
    }
    for (std::uint32_t attempt = 0U; attempt < 3U; ++attempt) {
      auto observed = inventory();
      if (!observed) {
        return tl::make_unexpected(observed.error());
      }
      auto after = directory_stamp();
      if (!after) {
        return tl::make_unexpected(after.error());
      }
      watch_changed = inventory_watch_changed();
      if (!watch_changed) {
        return tl::make_unexpected(watch_changed.error());
      }
      if (*before == *after && !*watch_changed) {
        auto synchronized = synchronize_active_file();
        if (!synchronized) {
          return synchronized;
        }
        inventory_cache_ = std::move(*observed);
        inventory_stamp_ = *after;
        inventory_cached_ = true;
        return {};
      }
      before = std::move(after);
    }
    return log_failure("session log directory kept changing during inventory");
  }

  void update_inventory_stamp() noexcept {
    auto stamp = directory_stamp();
    if (!stamp) {
      inventory_cached_ = false;
      return;
    }
    inventory_stamp_ = *stamp;
  }

  [[nodiscard]] Status ensure_capacity(std::size_t additional_files,
                                       std::uint64_t additional_bytes) {
    auto refreshed = refresh_inventory();
    if (!refreshed) {
      return refreshed;
    }
    return enforce_capacity(additional_files, additional_bytes);
  }

  // Requires a successful inventory refresh for the current active file.
  [[nodiscard]] Status enforce_capacity(std::size_t additional_files,
                                        std::uint64_t additional_bytes) {
    auto &observed = inventory_cache_;
    std::size_t index = 0;
    const auto exceeds = [&] {
      const bool file_limit =
          additional_files > quotas_.max_files ||
          observed.valid_files > quotas_.max_files - additional_files;
      const bool byte_limit = additional_bytes > quotas_.max_total_bytes ||
                              observed.controlled_bytes >
                                  quotas_.max_total_bytes - additional_bytes;
      return file_limit || byte_limit;
    };
    while (exceeds()) {
      if (index >= observed.deletable.size()) {
        return log_failure(
            "session log quota cannot be met without deleting an active, "
            "unknown, or damaged file");
      }
      const auto &candidate = observed.deletable[index++];
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
      --observed.valid_files;
      observed.controlled_bytes =
          candidate.size > observed.controlled_bytes
              ? 0U
              : observed.controlled_bytes - candidate.size;
    }
    observed.deletable.erase(observed.deletable.begin(),
                             observed.deletable.begin() +
                                 static_cast<std::ptrdiff_t>(index));
    if (index != 0U) {
      update_inventory_stamp();
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
        const int mode_error = errno;
        auto cleanup = abandon_active_file();
        if (!cleanup) {
          return cleanup;
        }
        return log_failure("cannot set session log mode 0600", mode_error);
      }
      auto written = write_all(active_fd_.get(), header_);
      if (!written) {
        auto cleanup = abandon_active_file();
        if (!cleanup) {
          return cleanup;
        }
        return written;
      }
      active_size_ = header_.size();
      if (inventory_cached_) {
        inventory_cache_.controlled_bytes += active_size_;
        ++inventory_cache_.valid_files;
        update_inventory_stamp();
      }
      return {};
    }
    return log_failure("cannot allocate a unique session log filename");
  }

  [[nodiscard]] Status abandon_active_file() noexcept {
    int unlink_result = -1;
    if (!active_name_.empty()) {
      do {
        unlink_result =
            ::unlinkat(directory_fd_.get(), active_name_.c_str(), 0);
      } while (unlink_result != 0 && errno == EINTR);
    }
    const int unlink_error = errno;
    active_fd_ = FileDescriptor{};
    active_name_.clear();
    active_size_ = 0U;
    inventory_cached_ = false;
    if (unlink_result != 0) {
      return log_failure("cannot remove failed session log creation",
                         unlink_error);
    }
    return {};
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
    inventory_watch_fd_ = FileDescriptor{};
    lock_fd_ = FileDescriptor{};
    directory_fd_ = FileDescriptor{};
    directory_path_.clear();
    header_.clear();
    started_at_.clear();
    inventory_cache_ = {};
    inventory_stamp_ = {};
    inventory_cached_ = false;
  }

  FileDescriptor directory_fd_;
  FileDescriptor lock_fd_;
  FileDescriptor inventory_watch_fd_;
  FileDescriptor active_fd_;
  std::filesystem::path directory_path_;
  std::string active_name_;
  std::string started_at_;
  std::string header_;
  SessionLogQuotas quotas_;
  std::uint64_t active_size_{};
  DirectoryInventory inventory_cache_;
  DirectoryStamp inventory_stamp_;
  bool inventory_cached_{};
};

} // namespace

std::unique_ptr<SessionLogFileSystem> make_linux_session_log_file_system() {
  return std::make_unique<LinuxSessionLogFileSystem>();
}

Status validate_session_log_directory(std::string_view directory) {
  const auto directory_error = [](std::string_view detail) {
    return make_error(ErrorCode::ValidationInvalidValue,
                      Operation::ValidateConfig, detail);
  };
  if (!directory.empty() && directory.front() != '/') {
    return tl::unexpected(directory_error("invalid logging configuration"));
  }
  if (!directory.empty()) {
    const std::string path{directory};
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0) {
      return tl::unexpected(directory_error("log directory does not exist"));
    }
    if (S_ISLNK(status.st_mode)) {
      return tl::unexpected(
          directory_error("log directory must not be a symbolic link"));
    }
    if (!S_ISDIR(status.st_mode)) {
      return tl::unexpected(directory_error("log path is not a directory"));
    }
    if (status.st_uid != ::geteuid()) {
      return tl::unexpected(
          directory_error("log directory must be owned by the current user"));
    }
    if (::faccessat(AT_FDCWD, path.c_str(), W_OK | X_OK, AT_EACCESS) != 0) {
      return tl::unexpected(
          directory_error("log directory is not writable and searchable"));
    }
  }
  return {};
}

} // namespace lazycom::logging
