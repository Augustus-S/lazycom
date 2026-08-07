#pragma once

#include <lazycom/base/error.hpp>
#include <lazycom/logging/schema.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lazycom::logging {

enum class SessionLogState { Off, Waiting, Recording, Error };

/** @brief File-count and encoded-byte limits for one logging directory. */
struct SessionLogQuotas {
  std::size_t max_files{100U};
  std::uint64_t max_total_bytes{1024ULL * 1024ULL * 1024ULL};
  std::uint64_t max_file_bytes{64ULL * 1024ULL * 1024ULL};
  auto operator<=>(const SessionLogQuotas &) const = default;
};

/**
 * @brief Bounded queue, flush, filtering, and filesystem options.
 *
 * All numeric limits and flush_interval must be positive, and max_file_bytes
 * must not exceed max_total_bytes. Invalid options cause SessionWriter
 * construction to throw std::invalid_argument.
 */
struct SessionWriterOptions {
  std::filesystem::path directory;
  SessionLogQuotas quotas;
  std::size_t queue_max_records{4096U};
  std::size_t queue_max_bytes{8U * 1024U * 1024U};
  std::size_t flush_batch_bytes{256U * 1024U};
  std::chrono::milliseconds flush_interval{1000};
  bool include_system{true};
  bool include_error{true};
};

/**
 * @brief Worker-confined filesystem seam for session-log storage.
 *
 * Calls are serialized on the writer thread. begin_session() and append_line()
 * receive complete LF-terminated schema lines. flush() releases implementation
 * buffering but does not imply fsync. close() must be idempotent and reusable.
 */
class SessionLogFileSystem {
public:
  virtual ~SessionLogFileSystem() = default;

  virtual Status begin_session(const std::filesystem::path &directory,
                               std::string_view started_at,
                               std::string_view encoded_header,
                               const SessionLogQuotas &quotas) = 0;
  virtual Status append_line(std::string_view encoded_record) = 0;
  virtual Status flush() = 0;
  virtual Status close() noexcept = 0;
  [[nodiscard]] virtual std::filesystem::path active_path() const = 0;
};

/**
 * @brief Creates the Linux private, no-follow session-log implementation.
 *
 * Directories and files use private ownership/modes. Quota cleanup deletes only
 * closed, identity-stable, successfully decoded supported logs; active,
 * damaged, symlinked, or otherwise unverifiable entries are retained.
 */
[[nodiscard]] std::unique_ptr<SessionLogFileSystem>
make_linux_session_log_file_system();

enum class EnqueueResult {
  Accepted,
  NotRecording,
  EmptyBatch,
  InvalidBatch,
  QueueFull,
  Stopping,
};

struct SessionCommandResult {
  SessionLogState state{SessionLogState::Off};
  std::uint64_t processed_through_seq{};
  std::optional<Error> error;
};

/**
 * @brief Asynchronous bounded session-log writer with one filesystem worker.
 *
 * Submission and snapshot operations are synchronized; destruction must not
 * race callers. Off rejects producers, Waiting is enabled without an active
 * file, Recording accepts records, and Error rejects later records after a
 * terminal queue/schema/filesystem failure. The stopping gate is separate from
 * SessionLogState.
 *
 * Queue byte accounting is a conservative in-memory charge; file quotas use
 * actual encoded NDJSON bytes. Filtering SYS/ERR lines still advances processed
 * and barrier watermarks.
 */
class SessionWriter {
public:
  /**
   * @brief Validates options and starts the filesystem worker.
   * @param options Bounded logging and directory policy.
   * @param file_system Exclusively owned implementation used for the complete
   * worker lifetime.
   */
  explicit SessionWriter(SessionWriterOptions options,
                         std::unique_ptr<SessionLogFileSystem> file_system =
                             make_linux_session_log_file_system());
  ~SessionWriter();

  SessionWriter(const SessionWriter &) = delete;
  SessionWriter &operator=(const SessionWriter &) = delete;
  SessionWriter(SessionWriter &&) = delete;
  SessionWriter &operator=(SessionWriter &&) = delete;

  /**
   * @brief Enables logging without opening a session file.
   * @return true for Off to Waiting and idempotent enabled states; false while
   * stopping, closing, or in Error.
   */
  [[nodiscard]] bool enable() noexcept;
  /**
   * @brief Immediately closes producer admission, then flushes and closes.
   * @return A future completed after records ordered before the command and the
   * final close have been processed.
   */
  [[nodiscard]] std::future<SessionCommandResult> disable();

  /**
   * @brief Safely creates a new file and writes its session header.
   * @param header Untrusted header value validated and encoded by the worker.
   * @return A future whose result is Recording only after file creation and
   * header append succeed, or Error after a validation/quota/filesystem
   * failure.
   * @pre Visible state is Waiting.
   */
  [[nodiscard]] std::future<SessionCommandResult> start_session(Header header);
  /**
   * @brief Stops the active file while leaving logging enabled.
   * @return A future completed after earlier records, flush, and close.
   * @post Visible Recording state changes to Waiting before worker completion,
   * preventing later producer admission.
   */
  [[nodiscard]] std::future<SessionCommandResult> end_session();

  /**
   * @brief Attempts to enqueue an owned, strictly increasing record batch.
   * @return Immediate admission status without waiting for filesystem I/O or
   * queue capacity. QueueFull transitions the writer to Error and prevents
   * later records from being written.
   * @note Full schema validation occurs on the worker after admission.
   */
  [[nodiscard]] EnqueueResult try_enqueue(std::vector<Record> batch) noexcept;

  /**
   * @brief Requests a user-space flush through an inclusive sequence watermark.
   * @param target_seq Highest sequence that must be processed before flush.
   * @note Confirmation includes filtered records but never guarantees fsync or
   * physical-media durability.
   */
  [[nodiscard]] std::future<BarrierResult> barrier(std::uint64_t target_seq);
  /**
   * @brief Permanently stops admission and drains the worker to its return
   * point.
   * @warning Destruction waits for the worker without an internal deadline.
   * Deadline-sensitive owners must enforce wait_until_stopped() before destroy.
   */
  [[nodiscard]] std::future<SessionCommandResult> shutdown();
  /**
   * @brief Waits for the worker return point without requesting stop or
   * joining.
   * @param deadline Absolute steady-clock deadline.
   */
  [[nodiscard]] bool wait_until_stopped(
      std::chrono::steady_clock::time_point deadline) const noexcept;

  [[nodiscard]] SessionLogState state() const noexcept;
  [[nodiscard]] std::uint64_t processed_through_seq() const noexcept;
  [[nodiscard]] std::size_t queued_records() const noexcept;
  [[nodiscard]] std::size_t queued_bytes() const noexcept;
  [[nodiscard]] std::filesystem::path active_path() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lazycom::logging
