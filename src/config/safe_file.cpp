#include <lazycom/config/safe_file.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace lazycom::config {
namespace {

constexpr std::string_view kTransactionLockName = ".lazycom-config.lock";

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

[[nodiscard]] Error file_error(Operation operation, std::string detail,
                               int error_number = 0) {
  return make_error(
      ErrorCode::ConfigIoFailed, operation, std::move(detail),
      error_number == 0
          ? std::error_code{}
          : std::error_code{error_number, std::generic_category()});
}

template <class T>
[[nodiscard]] Result<T> failure(Operation operation, std::string detail,
                                int error_number = 0) {
  return tl::make_unexpected(
      file_error(operation, std::move(detail), error_number));
}

[[nodiscard]] Status status_failure(std::string detail, int error_number = 0) {
  return tl::make_unexpected(
      file_error(Operation::SaveConfig, std::move(detail), error_number));
}

[[nodiscard]] bool
has_directory_permissions(const struct stat &status) noexcept {
  return (status.st_mode & 07777) == (S_IRWXU);
}

[[nodiscard]] bool has_file_permissions(const struct stat &status) noexcept {
  return (status.st_mode & 07777) == (S_IRUSR | S_IWUSR);
}

[[nodiscard]] int fsync_retry(int descriptor) noexcept;

[[nodiscard]] Result<FileDescriptor>
open_private_directory(const std::filesystem::path &path, Operation operation,
                       bool missing_is_empty) {
  const auto native = path.native();
  const int descriptor =
      ::open(native.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (missing_is_empty && errno == ENOENT) {
      return FileDescriptor{};
    }
    return failure<FileDescriptor>(
        operation, "cannot open configuration directory", errno);
  }

  FileDescriptor result{descriptor};
  struct stat status{};
  if (::fstat(result.get(), &status) != 0) {
    return failure<FileDescriptor>(
        operation, "cannot inspect configuration directory", errno);
  }
  if (!S_ISDIR(status.st_mode)) {
    return failure<FileDescriptor>(operation,
                                   "configuration parent is not a directory");
  }
  if (status.st_uid != ::geteuid()) {
    return failure<FileDescriptor>(
        operation, "configuration directory is not owned by current euid");
  }
  if (!has_directory_permissions(status)) {
    return failure<FileDescriptor>(
        operation, "configuration directory permissions must be 0700");
  }
  return result;
}

[[nodiscard]] Result<FileDescriptor>
open_or_create_private_directory(const std::filesystem::path &path,
                                 Operation operation) {
  const auto native = path.native();
  if (native.find('\0') != std::string::npos || !path.is_absolute() ||
      path == path.root_path() || path.filename().empty()) {
    return failure<FileDescriptor>(operation,
                                   "invalid configuration directory path");
  }
  const int root_descriptor =
      ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (root_descriptor < 0) {
    return failure<FileDescriptor>(
        operation, "cannot open filesystem root for configuration", errno);
  }
  FileDescriptor current{root_descriptor};
  auto relative = path.relative_path();
  for (auto iterator = relative.begin(); iterator != relative.end();
       ++iterator) {
    const auto component = iterator->native();
    if (component.empty() || component == "." || component == "..") {
      return failure<FileDescriptor>(operation,
                                     "invalid configuration path component");
    }
    const bool final = std::next(iterator) == relative.end();
    bool created = false;
    int descriptor = ::openat(current.get(), component.c_str(),
                              O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0 && errno == ENOENT) {
      if (::mkdirat(current.get(), component.c_str(), S_IRWXU) != 0) {
        return failure<FileDescriptor>(
            operation, "cannot create configuration directory", errno);
      }
      created = true;
      if (::fchmodat(current.get(), component.c_str(), S_IRWXU,
                     AT_SYMLINK_NOFOLLOW) != 0) {
        return failure<FileDescriptor>(
            operation, "cannot set configuration directory mode 0700", errno);
      }
      if (fsync_retry(current.get()) != 0) {
        return failure<FileDescriptor>(
            operation, "cannot fsync parent of new configuration directory",
            errno);
      }
      descriptor = ::openat(current.get(), component.c_str(),
                            O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    }
    if (descriptor < 0) {
      return failure<FileDescriptor>(
          operation, "cannot open configuration directory", errno);
    }
    FileDescriptor next{descriptor};
    struct stat status{};
    if (::fstat(next.get(), &status) != 0) {
      return failure<FileDescriptor>(
          operation, "cannot inspect configuration directory", errno);
    }
    if (!S_ISDIR(status.st_mode)) {
      return failure<FileDescriptor>(
          operation, "configuration path component is not a directory");
    }
    if ((created || final) && status.st_uid != ::geteuid()) {
      return failure<FileDescriptor>(
          operation, "configuration directory is not owned by current euid");
    }
    if ((created || final) && !has_directory_permissions(status)) {
      return failure<FileDescriptor>(
          operation, "configuration directory permissions must be 0700");
    }
    current = std::move(next);
  }
  return current;
}

[[nodiscard]] bool valid_target_path(const std::filesystem::path &target) {
  const auto native = target.native();
  if (native.find('\0') != std::string::npos || !target.is_absolute() ||
      target.filename().empty() || target.filename() == "." ||
      target.filename() == "..") {
    return false;
  }
  return target.filename() == target.filename().filename();
}

[[nodiscard]] Status validate_existing_target(int directory_fd,
                                              const std::string &name) {
  struct stat status{};
  if (::fstatat(directory_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) !=
      0) {
    if (errno == ENOENT) {
      return {};
    }
    return status_failure("cannot inspect atomic write target", errno);
  }
  if (!S_ISREG(status.st_mode)) {
    return status_failure("atomic write target is not a regular file");
  }
  if (status.st_uid != ::geteuid()) {
    return status_failure("atomic write target is not owned by current euid");
  }
  if (!has_file_permissions(status)) {
    return status_failure("atomic write target permissions must be 0600");
  }
  return {};
}

[[nodiscard]] Result<FileDescriptor>
lock_configuration_directory(int directory_fd) {
  const std::string name{kTransactionLockName};
  bool created = false;
  int descriptor = ::openat(directory_fd, name.c_str(),
                            O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                            S_IRUSR | S_IWUSR);
  if (descriptor >= 0) {
    created = true;
  } else if (errno == EEXIST) {
    descriptor =
        ::openat(directory_fd, name.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  }
  if (descriptor < 0) {
    return failure<FileDescriptor>(Operation::SaveConfig,
                                   "cannot open configuration transaction lock",
                                   errno);
  }
  FileDescriptor lock{descriptor};
  if (created && ::fchmod(lock.get(), S_IRUSR | S_IWUSR) != 0) {
    const int mode_error = errno;
    static_cast<void>(::unlinkat(directory_fd, name.c_str(), 0));
    return failure<FileDescriptor>(Operation::SaveConfig,
                                   "cannot set transaction lock mode 0600",
                                   mode_error);
  }
  struct stat status{};
  if (::fstat(lock.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != ::geteuid() || !has_file_permissions(status)) {
    const int inspect_error = errno;
    return failure<FileDescriptor>(
        Operation::SaveConfig,
        "configuration transaction lock must be owned and mode 0600",
        inspect_error);
  }
  if (::flock(lock.get(), LOCK_EX | LOCK_NB) != 0) {
    return failure<FileDescriptor>(Operation::SaveConfig,
                                   "configuration directory is busy", errno);
  }
  return lock;
}

class Sha256 {
public:
  void update(const std::string_view bytes) noexcept {
    for (const char raw_byte : bytes) {
      const auto byte = static_cast<std::uint8_t>(raw_byte);
      buffer_[buffer_size_++] = byte;
      ++total_bytes_;
      if (buffer_size_ == buffer_.size()) {
        transform(buffer_.data());
        buffer_size_ = 0;
      }
    }
  }

  [[nodiscard]] std::array<std::uint8_t, 32> finish() noexcept {
    const auto bit_count = total_bytes_ * 8U;
    buffer_[buffer_size_++] = 0x80U;
    if (buffer_size_ > 56U) {
      std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
                buffer_.end(), 0U);
      transform(buffer_.data());
      buffer_size_ = 0;
    }
    std::fill(buffer_.begin() + static_cast<std::ptrdiff_t>(buffer_size_),
              buffer_.begin() + 56, 0U);
    for (std::size_t index = 0; index < 8U; ++index) {
      buffer_[63U - index] =
          static_cast<std::uint8_t>(bit_count >> (index * 8U));
    }
    transform(buffer_.data());

    std::array<std::uint8_t, 32> result{};
    for (std::size_t index = 0; index < state_.size(); ++index) {
      result[index * 4U] = static_cast<std::uint8_t>(state_[index] >> 24U);
      result[index * 4U + 1U] = static_cast<std::uint8_t>(state_[index] >> 16U);
      result[index * 4U + 2U] = static_cast<std::uint8_t>(state_[index] >> 8U);
      result[index * 4U + 3U] = static_cast<std::uint8_t>(state_[index]);
    }
    return result;
  }

private:
  [[nodiscard]] static constexpr std::uint32_t
  choose(const std::uint32_t x, const std::uint32_t y,
         const std::uint32_t z) noexcept {
    return (x & y) ^ (~x & z);
  }

  [[nodiscard]] static constexpr std::uint32_t
  majority(const std::uint32_t x, const std::uint32_t y,
           const std::uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
  }

  void transform(const std::uint8_t *block) noexcept {
    static constexpr std::array<std::uint32_t, 64> constants{
        0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
        0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
        0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
        0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
        0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
        0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
        0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
        0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
        0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
        0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
        0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
        0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
        0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0; index < 16U; ++index) {
      const auto offset = index * 4U;
      words[index] = (static_cast<std::uint32_t>(block[offset]) << 24U) |
                     (static_cast<std::uint32_t>(block[offset + 1U]) << 16U) |
                     (static_cast<std::uint32_t>(block[offset + 2U]) << 8U) |
                     static_cast<std::uint32_t>(block[offset + 3U]);
    }
    for (std::size_t index = 16U; index < words.size(); ++index) {
      const auto s0 = std::rotr(words[index - 15U], 7) ^
                      std::rotr(words[index - 15U], 18) ^
                      (words[index - 15U] >> 3U);
      const auto s1 = std::rotr(words[index - 2U], 17) ^
                      std::rotr(words[index - 2U], 19) ^
                      (words[index - 2U] >> 10U);
      words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
    }

    auto a = state_[0];
    auto b = state_[1];
    auto c = state_[2];
    auto d = state_[3];
    auto e = state_[4];
    auto f = state_[5];
    auto g = state_[6];
    auto h = state_[7];
    for (std::size_t index = 0; index < words.size(); ++index) {
      const auto sum1 = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
      const auto temporary1 =
          h + sum1 + choose(e, f, g) + constants[index] + words[index];
      const auto sum0 = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
      const auto temporary2 = sum0 + majority(a, b, c);
      h = g;
      g = f;
      f = e;
      e = d + temporary1;
      d = c;
      c = b;
      b = a;
      a = temporary1 + temporary2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::array<std::uint32_t, 8> state_{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U,
                                      0xa54ff53aU, 0x510e527fU, 0x9b05688cU,
                                      0x1f83d9abU, 0x5be0cd19U};
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffer_size_{};
  std::uint64_t total_bytes_{};
};

[[nodiscard]] bool same_file_version(const struct stat &left,
                                     const struct stat &right) noexcept {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
         left.st_size == right.st_size &&
         left.st_mtim.tv_sec == right.st_mtim.tv_sec &&
         left.st_mtim.tv_nsec == right.st_mtim.tv_nsec;
}

struct ObservedFile {
  std::string bytes;
  SafeFileIdentity identity;
};

[[nodiscard]] Result<ObservedFile>
read_open_file(int descriptor, std::size_t maximum_bytes, Operation operation) {
  struct stat before{};
  if (::fstat(descriptor, &before) != 0) {
    return failure<ObservedFile>(operation, "cannot inspect configuration file",
                                 errno);
  }
  if (!S_ISREG(before.st_mode)) {
    return failure<ObservedFile>(operation,
                                 "configuration path is not a regular file");
  }
  if (before.st_uid != ::geteuid()) {
    return failure<ObservedFile>(
        operation, "configuration file is not owned by current euid");
  }
  if (!has_file_permissions(before)) {
    return failure<ObservedFile>(operation,
                                 "configuration file permissions must be 0600");
  }
  if (before.st_size < 0 ||
      static_cast<std::uintmax_t>(before.st_size) > maximum_bytes) {
    return failure<ObservedFile>(operation,
                                 "configuration file exceeds its maximum size");
  }

  ObservedFile result;
  result.bytes.reserve(static_cast<std::size_t>(before.st_size));
  Sha256 digest;
  std::array<char, 16384> buffer{};
  while (true) {
    const auto count = ::read(descriptor, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return failure<ObservedFile>(operation, "cannot read configuration file",
                                   errno);
    }
    if (count == 0) {
      break;
    }
    const auto amount = static_cast<std::size_t>(count);
    if (amount > maximum_bytes - result.bytes.size()) {
      return failure<ObservedFile>(
          operation, "configuration file grew beyond its maximum size");
    }
    const std::string_view chunk{buffer.data(), amount};
    digest.update(chunk);
    result.bytes.append(chunk);
  }

  struct stat after{};
  if (::fstat(descriptor, &after) != 0) {
    return failure<ObservedFile>(operation,
                                 "cannot re-inspect configuration file", errno);
  }
  if (!same_file_version(before, after) || !S_ISREG(after.st_mode) ||
      after.st_uid != ::geteuid() || !has_file_permissions(after) ||
      after.st_size < 0 ||
      static_cast<std::uintmax_t>(after.st_size) != result.bytes.size()) {
    return failure<ObservedFile>(operation,
                                 "configuration file changed while being read");
  }
  result.identity = {true,
                     static_cast<std::uint64_t>(after.st_dev),
                     static_cast<std::uint64_t>(after.st_ino),
                     static_cast<std::uint64_t>(after.st_size),
                     static_cast<std::int64_t>(after.st_mtim.tv_sec),
                     static_cast<std::int64_t>(after.st_mtim.tv_nsec),
                     digest.finish()};
  return result;
}

[[nodiscard]] Result<ObservedFile> observe_target(int directory_fd,
                                                  const std::string &name,
                                                  std::size_t maximum_bytes,
                                                  Operation operation) {
  const int descriptor =
      ::openat(directory_fd, name.c_str(),
               O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      return ObservedFile{};
    }
    return failure<ObservedFile>(
        operation, "cannot open configuration file without following links",
        errno);
  }
  FileDescriptor file{descriptor};
  return read_open_file(file.get(), maximum_bytes, operation);
}

[[nodiscard]] Status
validate_expected_target(int directory_fd, const std::string &name,
                         std::size_t maximum_bytes,
                         const SafeFileIdentity &expected_identity) {
  auto observed =
      observe_target(directory_fd, name, maximum_bytes, Operation::SaveConfig);
  if (!observed) {
    return tl::make_unexpected(observed.error());
  }
  if (observed->identity != expected_identity) {
    return status_failure("configuration file changed since it was loaded");
  }
  return {};
}

[[nodiscard]] int fsync_retry(int descriptor) noexcept {
  int result = -1;
  do {
    result = ::fsync(descriptor);
  } while (result != 0 && errno == EINTR);
  return result;
}

[[nodiscard]] int rename_noreplace(int directory_fd, const std::string &source,
                                   const std::string &target) noexcept {
#ifdef SYS_renameat2
  return static_cast<int>(::syscall(SYS_renameat2, directory_fd, source.c_str(),
                                    directory_fd, target.c_str(), 1U));
#else
  static_cast<void>(directory_fd);
  static_cast<void>(source);
  static_cast<void>(target);
  errno = ENOTSUP;
  return -1;
#endif
}

[[nodiscard]] std::string temporary_name(const std::string &target) {
  static std::atomic<std::uint64_t> sequence{0};
  const auto id = sequence.fetch_add(1, std::memory_order_relaxed);
  return "." + target + ".tmp." +
         std::to_string(static_cast<unsigned long long>(::getpid())) + "." +
         std::to_string(id);
}

[[nodiscard]] Status write_all(int descriptor, std::string_view bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto remaining = bytes.size() - offset;
    const auto bounded =
        std::min(remaining,
                 static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
    const auto written = ::write(descriptor, bytes.data() + offset, bounded);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return status_failure("cannot write atomic temporary", errno);
    }
    if (written == 0) {
      return status_failure("atomic write made no progress");
    }
    offset += static_cast<std::size_t>(written);
  }
  return {};
}

class LinuxAtomicWriteTransaction final : public AtomicWriteTransaction {
public:
  LinuxAtomicWriteTransaction(FileDescriptor directory, FileDescriptor lock,
                              std::string target, std::size_t maximum_bytes,
                              SafeFileIdentity expected_identity)
      : directory_{std::move(directory)}, lock_{std::move(lock)},
        target_{std::move(target)}, maximum_bytes_{maximum_bytes},
        expected_identity_{std::move(expected_identity)} {}

  ~LinuxAtomicWriteTransaction() override {
    if (!temporary_.empty()) {
      static_cast<void>(::unlinkat(directory_.get(), temporary_.c_str(), 0));
    }
  }

  Status stage(std::string_view bytes) override {
    if (!temporary_.empty() || committed_) {
      return status_failure("atomic write transaction was already staged");
    }
    if (bytes.size() > maximum_bytes_) {
      return status_failure("new configuration exceeds its maximum size");
    }

    for (std::uint32_t attempt = 0; attempt < 128; ++attempt) {
      temporary_ = temporary_name(target_);
      const int descriptor =
          ::openat(directory_.get(), temporary_.c_str(),
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                   S_IRUSR | S_IWUSR);
      if (descriptor >= 0) {
        temporary_fd_ = FileDescriptor{descriptor};
        if (::fchmod(temporary_fd_.get(), S_IRUSR | S_IWUSR) != 0) {
          return status_failure("cannot set atomic write temporary permissions",
                                errno);
        }
        break;
      }
      const int open_error = errno;
      temporary_.clear();
      if (open_error != EEXIST) {
        return status_failure("cannot create atomic write temporary",
                              open_error);
      }
    }
    if (temporary_fd_.get() < 0) {
      return status_failure("cannot allocate a unique atomic write temporary");
    }

    const auto write_status = write_all(temporary_fd_.get(), bytes);
    if (!write_status) {
      return write_status;
    }

    if (fsync_retry(temporary_fd_.get()) != 0) {
      return status_failure("cannot fsync atomic write temporary", errno);
    }
    struct stat staged_status{};
    if (::fstat(temporary_fd_.get(), &staged_status) != 0) {
      return status_failure("cannot inspect atomic write temporary", errno);
    }
    if (!S_ISREG(staged_status.st_mode) ||
        staged_status.st_uid != ::geteuid() ||
        !has_file_permissions(staged_status) || staged_status.st_size < 0 ||
        static_cast<std::uintmax_t>(staged_status.st_size) != bytes.size()) {
      return status_failure("atomic write temporary changed while staging");
    }
    Sha256 digest;
    digest.update(bytes);
    staged_identity_ = {
        true,
        static_cast<std::uint64_t>(staged_status.st_dev),
        static_cast<std::uint64_t>(staged_status.st_ino),
        static_cast<std::uint64_t>(staged_status.st_size),
        static_cast<std::int64_t>(staged_status.st_mtim.tv_sec),
        static_cast<std::int64_t>(staged_status.st_mtim.tv_nsec),
        digest.finish()};
    const int descriptor = temporary_fd_.release();
    if (::close(descriptor) != 0) {
      return status_failure("cannot close atomic write temporary", errno);
    }
    return {};
  }

  Result<SafeFileIdentity> commit() override {
    if (temporary_.empty() || temporary_fd_.get() >= 0 || committed_) {
      return failure<SafeFileIdentity>(
          Operation::SaveConfig,
          "atomic write transaction is not ready to commit");
    }
    // Build the backup only from the identity observed by the caller, then
    // recheck after backup I/O so an external replacement cannot be overwritten
    // without another optimistic-concurrency check immediately before rename.
    const auto initial_identity_status = validate_expected_target(
        directory_.get(), target_, maximum_bytes_, expected_identity_);
    if (!initial_identity_status) {
      return tl::make_unexpected(initial_identity_status.error());
    }
    const auto backup_status = backup_existing_target();
    if (!backup_status) {
      return tl::make_unexpected(backup_status.error());
    }
    const auto final_identity_status = validate_expected_target(
        directory_.get(), target_, maximum_bytes_, expected_identity_);
    if (!final_identity_status) {
      return tl::make_unexpected(final_identity_status.error());
    }
    const int rename_result =
        expected_identity_.exists
            ? ::renameat(directory_.get(), temporary_.c_str(), directory_.get(),
                         target_.c_str())
            : rename_noreplace(directory_.get(), temporary_, target_);
    if (rename_result != 0) {
      return failure<SafeFileIdentity>(
          Operation::SaveConfig, "cannot rename atomic write temporary", errno);
    }
    temporary_.clear();
    committed_ = true;
    return staged_identity_;
  }

  Status sync_parent_directory() override {
    if (!committed_) {
      return status_failure("cannot sync an uncommitted atomic write");
    }
    if (fsync_retry(directory_.get()) != 0) {
      return status_failure("cannot fsync configuration directory", errno);
    }
    return {};
  }

private:
  [[nodiscard]] Status backup_existing_target() {
    if (!expected_identity_.exists) {
      return {};
    }
    auto source = observe_target(directory_.get(), target_, maximum_bytes_,
                                 Operation::SaveConfig);
    if (!source) {
      return tl::make_unexpected(source.error());
    }
    if (source->identity != expected_identity_) {
      return status_failure("configuration backup source changed since load");
    }

    const std::string backup = target_ + ".bak";
    const auto backup_target_status =
        validate_existing_target(directory_.get(), backup);
    if (!backup_target_status) {
      return backup_target_status;
    }

    std::string temporary;
    FileDescriptor destination;
    for (std::uint32_t attempt = 0; attempt < 128; ++attempt) {
      temporary = temporary_name(backup);
      const int descriptor =
          ::openat(directory_.get(), temporary.c_str(),
                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                   S_IRUSR | S_IWUSR);
      if (descriptor >= 0) {
        destination = FileDescriptor{descriptor};
        break;
      }
      const int open_error = errno;
      temporary.clear();
      if (open_error != EEXIST) {
        return status_failure("cannot create configuration backup", open_error);
      }
    }
    if (destination.get() < 0) {
      return status_failure("cannot allocate a configuration backup temporary");
    }

    const auto cleanup = [&] {
      if (!temporary.empty()) {
        static_cast<void>(::unlinkat(directory_.get(), temporary.c_str(), 0));
      }
    };
    if (source->bytes.size() > maximum_bytes_) {
      cleanup();
      return status_failure("configuration backup exceeds its maximum size");
    }
    const auto copy_status = write_all(destination.get(), source->bytes);
    if (!copy_status) {
      cleanup();
      return copy_status;
    }
    if (::fchmod(destination.get(), S_IRUSR | S_IWUSR) != 0 ||
        fsync_retry(destination.get()) != 0) {
      const int sync_error = errno;
      cleanup();
      return status_failure("cannot sync configuration backup", sync_error);
    }
    const int descriptor = destination.release();
    if (::close(descriptor) != 0) {
      const int close_error = errno;
      cleanup();
      return status_failure("cannot close configuration backup", close_error);
    }
    if (::renameat(directory_.get(), temporary.c_str(), directory_.get(),
                   backup.c_str()) != 0) {
      const int rename_error = errno;
      cleanup();
      return status_failure("cannot replace configuration backup",
                            rename_error);
    }
    temporary.clear();
    if (fsync_retry(directory_.get()) != 0) {
      return status_failure("cannot sync configuration backup directory",
                            errno);
    }
    return {};
  }

  FileDescriptor directory_;
  FileDescriptor lock_;
  std::string target_;
  std::string temporary_;
  FileDescriptor temporary_fd_;
  std::size_t maximum_bytes_;
  SafeFileIdentity expected_identity_;
  SafeFileIdentity staged_identity_;
  bool committed_{};
};

class LinuxAtomicFileSystem final : public AtomicFileSystem {
public:
  Result<std::unique_ptr<AtomicWriteTransaction>>
  begin_atomic_write(const std::filesystem::path &target,
                     std::size_t maximum_bytes,
                     const SafeFileIdentity &expected_identity) override {
    if (maximum_bytes == 0) {
      return failure<std::unique_ptr<AtomicWriteTransaction>>(
          Operation::SaveConfig, "maximum file size must be positive");
    }
    if (!valid_target_path(target)) {
      return failure<std::unique_ptr<AtomicWriteTransaction>>(
          Operation::SaveConfig,
          "atomic write target must be an absolute file path");
    }
    if (target.filename() == kTransactionLockName) {
      return failure<std::unique_ptr<AtomicWriteTransaction>>(
          Operation::SaveConfig,
          "atomic write target conflicts with the transaction lock");
    }
    auto directory = open_or_create_private_directory(target.parent_path(),
                                                      Operation::SaveConfig);
    if (!directory) {
      return tl::make_unexpected(directory.error());
    }
    auto lock = lock_configuration_directory(directory->get());
    if (!lock) {
      return tl::make_unexpected(lock.error());
    }
    const auto name = target.filename().string();
    const auto identity_status = validate_expected_target(
        directory->get(), name, maximum_bytes, expected_identity);
    if (!identity_status) {
      return tl::make_unexpected(identity_status.error());
    }
    return std::make_unique<LinuxAtomicWriteTransaction>(
        std::move(*directory), std::move(*lock), name, maximum_bytes,
        expected_identity);
  }
};

} // namespace

Result<SafeFileContents> read_safe_file(const std::filesystem::path &path,
                                        std::size_t maximum_bytes) {
  try {
    if (maximum_bytes == 0) {
      return failure<SafeFileContents>(Operation::ValidateConfig,
                                       "maximum file size must be positive");
    }
    if (!valid_target_path(path)) {
      return failure<SafeFileContents>(
          Operation::ValidateConfig,
          "configuration path must be an absolute file path");
    }

    auto directory = open_private_directory(path.parent_path(),
                                            Operation::ValidateConfig, true);
    if (!directory) {
      return tl::make_unexpected(directory.error());
    }
    if (directory->get() < 0) {
      return SafeFileContents{};
    }

    const auto name = path.filename().string();
    auto observed = observe_target(directory->get(), name, maximum_bytes,
                                   Operation::ValidateConfig);
    if (!observed) {
      return tl::make_unexpected(observed.error());
    }
    return SafeFileContents{std::move(observed->bytes),
                            std::move(observed->identity)};
  } catch (const std::exception &exception) {
    return failure<SafeFileContents>(
        Operation::ValidateConfig,
        std::string{"safe configuration read failed: "} + exception.what());
  } catch (...) {
    return failure<SafeFileContents>(Operation::ValidateConfig,
                                     "safe configuration read failed");
  }
}

AtomicFileSystem &linux_atomic_file_system() noexcept {
  static LinuxAtomicFileSystem instance;
  return instance;
}

AtomicWriteOutcome
write_file_atomically(const std::filesystem::path &target,
                      std::string_view bytes, std::size_t maximum_bytes,
                      const SafeFileIdentity &expected_identity,
                      AtomicFileSystem &file_system) {
  // A committed identity survives directory-sync failure and remains tied to
  // this replacement even if another writer subsequently changes the path.
  std::optional<SafeFileIdentity> committed_identity;
  try {
    if (maximum_bytes == 0 || bytes.size() > maximum_bytes) {
      return {CommitState::NotCommitted,
              file_error(Operation::SaveConfig,
                         maximum_bytes == 0
                             ? "maximum file size must be positive"
                             : "new configuration exceeds its maximum size")};
    }
    auto transaction = file_system.begin_atomic_write(target, maximum_bytes,
                                                      expected_identity);
    if (!transaction) {
      return {CommitState::NotCommitted, transaction.error()};
    }
    auto status = (*transaction)->stage(bytes);
    if (!status) {
      return {CommitState::NotCommitted, status.error()};
    }
    auto committed = (*transaction)->commit();
    if (!committed) {
      return {CommitState::NotCommitted, committed.error()};
    }
    committed_identity = *committed;
    status = (*transaction)->sync_parent_directory();
    if (!status) {
      return {CommitState::CommittedDurabilityUnknown, status.error(),
              committed_identity};
    }
    return {CommitState::Committed, std::nullopt, committed_identity};
  } catch (const std::exception &exception) {
    return {committed_identity ? CommitState::CommittedDurabilityUnknown
                               : CommitState::NotCommitted,
            file_error(Operation::SaveConfig,
                       std::string{"atomic configuration write failed: "} +
                           exception.what()),
            committed_identity};
  } catch (...) {
    return {
        committed_identity ? CommitState::CommittedDurabilityUnknown
                           : CommitState::NotCommitted,
        file_error(Operation::SaveConfig, "atomic configuration write failed"),
        committed_identity};
  }
}

AtomicWriteOutcome
write_file_atomically(const std::filesystem::path &target,
                      std::string_view bytes, std::size_t maximum_bytes,
                      const SafeFileIdentity &expected_identity) {
  return write_file_atomically(target, bytes, maximum_bytes, expected_identity,
                               linux_atomic_file_system());
}

} // namespace lazycom::config
