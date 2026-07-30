#pragma once

#include <lazycom/base/error.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace lazycom::config {

struct SafeFileIdentity {
  bool exists{};
  std::uint64_t device{};
  std::uint64_t inode{};
  std::uint64_t size{};
  std::int64_t mtime_seconds{};
  std::int64_t mtime_nanoseconds{};
  std::array<std::uint8_t, 32> content_digest{};
  auto operator<=>(const SafeFileIdentity &) const = default;
};

struct SafeFileContents {
  std::string bytes;
  SafeFileIdentity identity;
};

[[nodiscard]] Result<SafeFileContents>
read_safe_file(const std::filesystem::path &path, std::size_t maximum_bytes);

enum class CommitState {
  NotCommitted,
  Committed,
  CommittedDurabilityUnknown,
};

struct AtomicWriteOutcome {
  CommitState state{CommitState::NotCommitted};
  std::optional<Error> error;
};

class AtomicWriteTransaction {
public:
  virtual ~AtomicWriteTransaction() = default;

  virtual Status stage(std::string_view bytes) = 0;
  virtual Status commit() = 0;
  virtual Status sync_parent_directory() = 0;
};

class AtomicFileSystem {
public:
  virtual ~AtomicFileSystem() = default;

  virtual Result<std::unique_ptr<AtomicWriteTransaction>>
  begin_atomic_write(const std::filesystem::path &target,
                     std::size_t maximum_bytes,
                     const SafeFileIdentity &expected_identity) = 0;
};

[[nodiscard]] AtomicFileSystem &linux_atomic_file_system() noexcept;

[[nodiscard]] AtomicWriteOutcome
write_file_atomically(const std::filesystem::path &target,
                      std::string_view bytes, std::size_t maximum_bytes,
                      const SafeFileIdentity &expected_identity,
                      AtomicFileSystem &file_system);

[[nodiscard]] AtomicWriteOutcome
write_file_atomically(const std::filesystem::path &target,
                      std::string_view bytes, std::size_t maximum_bytes,
                      const SafeFileIdentity &expected_identity);

} // namespace lazycom::config
