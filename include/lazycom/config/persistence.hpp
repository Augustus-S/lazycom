#pragma once

#include <lazycom/config/safe_file.hpp>
#include <lazycom/config/schema.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace lazycom::config {

enum class PersistenceFile { Config, QuickSend, State };
enum class SaveSubmitState { Accepted, Busy, ReadOnly, Stopping, Invalid };

struct PersistencePaths {
  std::filesystem::path config;
  std::filesystem::path quick_send;
  std::filesystem::path state;
};

struct SaveCompletion {
  PersistenceFile file{PersistenceFile::Config};
  AtomicWriteOutcome outcome;
  /**
   * Exact validated document passed to the atomic writer. Callers use it to
   * commit the matching in-memory snapshot and retain
   * outcome.committed_identity for both committed states, without reloading the
   * target path.
   */
  std::string serialized_document;
};

/**
 * @brief Synchronous admission result for an asynchronous save.
 *
 * completion is valid only when state is Accepted. Every other state carries
 * an immediate error and does not admit filesystem work.
 */
struct SaveSubmission {
  SaveSubmitState state{SaveSubmitState::Invalid};
  std::future<SaveCompletion> completion;
  std::optional<Error> error;

  [[nodiscard]] bool accepted() const noexcept {
    return state == SaveSubmitState::Accepted;
  }
};

/**
 * @brief Serializes and persists configuration documents on one worker thread.
 *
 * At most one request per PersistenceFile may be outstanding. Accepted
 * requests are executed FIFO. The injected AtomicFileSystem is borrowed and
 * must outlive this worker.
 *
 * @warning Destruction and shutdown must not race other member calls.
 */
class PersistenceWorker {
public:
  /**
   * @brief Starts the persistence worker.
   * @param paths Target paths for the three managed documents.
   * @param file_system Borrowed filesystem implementation that must outlive the
   * worker.
   */
  explicit PersistenceWorker(
      PersistencePaths paths,
      AtomicFileSystem &file_system = linux_atomic_file_system());
  ~PersistenceWorker();

  PersistenceWorker(const PersistenceWorker &) = delete;
  PersistenceWorker &operator=(const PersistenceWorker &) = delete;
  PersistenceWorker(PersistenceWorker &&) = delete;
  PersistenceWorker &operator=(PersistenceWorker &&) = delete;

  /**
   * @brief Validates, serializes, and attempts to admit a config save.
   * @return Accepted with a future, or an immediate rejection with an error.
   * @note Accepted means queued, not committed.
   */
  [[nodiscard]] SaveSubmission
  save_config(const ConfigSnapshot &snapshot,
              const SafeFileIdentity &expected_identity,
              std::string_view preserved_document = {}, bool read_only = false);
  /** @brief Equivalent save operation for quick-send configuration. */
  [[nodiscard]] SaveSubmission
  save_quick_send(const QuickSendSnapshot &snapshot,
                  const SafeFileIdentity &expected_identity,
                  std::string_view preserved_document = {},
                  bool read_only = false);
  /** @brief Equivalent save operation for UI state. */
  [[nodiscard]] SaveSubmission
  save_state(const StateSnapshot &snapshot,
             const SafeFileIdentity &expected_identity,
             std::string_view preserved_document = {}, bool read_only = false);

  /** @brief Permanently stops admission and drains accepted saves FIFO. */
  void request_stop() noexcept;
  /**
   * @brief Waits for the worker to reach its nonblocking return point.
   * @param deadline Absolute steady-clock deadline.
   * @return true if the return point was reached before the deadline.
   * @note This function neither requests stop nor joins the thread.
   */
  [[nodiscard]] bool wait_until_stopped(
      std::chrono::steady_clock::time_point deadline) const noexcept;
  /**
   * @brief Requests stop and joins the worker.
   * @warning This call has no internal deadline. Deadline-sensitive callers
   * must first obtain a successful bounded wait.
   */
  void shutdown();
  [[nodiscard]] bool active(PersistenceFile file) const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lazycom::config
