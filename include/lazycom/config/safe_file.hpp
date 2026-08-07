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

/**
 * @brief Identity token used to detect replacement between read and write.
 *
 * The token combines filesystem identity, size, modification time, and a
 * content digest. Callers should retain the complete value returned by
 * read_safe_file() and supply it unchanged to a later atomic write.
 */
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

/**
 * @brief Reads a private configuration file and captures its identity.
 * @param path Absolute path to the file.
 * @param maximum_bytes Positive maximum accepted file size.
 * @return Owned file bytes and their identity. A confirmed missing file is a
 * successful result with empty bytes and identity.exists set to false.
 * @note Existing files must be current-user-owned regular files with mode 0600;
 * the final parent directory must be current-user-owned with mode 0700. Final
 * symlinks and files that change during the read are rejected.
 */
[[nodiscard]] Result<SafeFileContents>
read_safe_file(const std::filesystem::path &path, std::size_t maximum_bytes);

/** @brief Visibility and durability state of an atomic replacement. */
enum class CommitState {
  /** The target was not replaced. */
  NotCommitted,
  /** The target was replaced and parent-directory synchronization succeeded. */
  Committed,
  /** The target is visible, but crash durability could not be confirmed. */
  CommittedDurabilityUnknown,
};

struct AtomicWriteOutcome {
  CommitState state{CommitState::NotCommitted};
  std::optional<Error> error;
};

class AtomicWriteTransaction {
public:
  virtual ~AtomicWriteTransaction() = default;

  /** @brief Writes and synchronizes the complete temporary-file contents. */
  virtual Status stage(std::string_view bytes) = 0;
  /** @brief Makes the staged file visible at the target path. */
  virtual Status commit() = 0;
  /** @brief Synchronizes the parent directory after a successful commit. */
  virtual Status sync_parent_directory() = 0;
};

/**
 * @brief Factory interface for one optimistic atomic-write transaction.
 *
 * Implementations must clean uncommitted temporary state when a transaction is
 * destroyed. A returned transaction is used sequentially, not concurrently.
 */
class AtomicFileSystem {
public:
  virtual ~AtomicFileSystem() = default;

  virtual Result<std::unique_ptr<AtomicWriteTransaction>>
  begin_atomic_write(const std::filesystem::path &target,
                     std::size_t maximum_bytes,
                     const SafeFileIdentity &expected_identity) = 0;
};

/**
 * @brief Returns the process-wide Linux safe-file implementation.
 *
 * The implementation uses directory-relative no-follow operations, private
 * modes, an exclusive temporary file, one fixed backup, rename, and parent
 * directory synchronization.
 */
[[nodiscard]] AtomicFileSystem &linux_atomic_file_system() noexcept;

/**
 * @brief Replaces a file through the complete safe atomic-write protocol.
 * @param target Absolute target path.
 * @param bytes Complete replacement document.
 * @param maximum_bytes Positive maximum document size.
 * @param expected_identity Identity observed by the preceding safe read.
 * @param file_system Borrowed implementation used for this call only.
 * @return A tri-state outcome. CommittedDurabilityUnknown must be treated as a
 * committed replacement even though restart durability is uncertain.
 */
[[nodiscard]] AtomicWriteOutcome
write_file_atomically(const std::filesystem::path &target,
                      std::string_view bytes, std::size_t maximum_bytes,
                      const SafeFileIdentity &expected_identity,
                      AtomicFileSystem &file_system);

/** @overload Uses linux_atomic_file_system(). */
[[nodiscard]] AtomicWriteOutcome
write_file_atomically(const std::filesystem::path &target,
                      std::string_view bytes, std::size_t maximum_bytes,
                      const SafeFileIdentity &expected_identity);

} // namespace lazycom::config
