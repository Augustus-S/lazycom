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
  // This is the exact validated document passed to the atomic writer. Callers
  // use it to commit the matching in-memory snapshot for both committed states.
  std::string serialized_document;
};

struct SaveSubmission {
  SaveSubmitState state{SaveSubmitState::Invalid};
  std::future<SaveCompletion> completion;
  std::optional<Error> error;

  [[nodiscard]] bool accepted() const noexcept {
    return state == SaveSubmitState::Accepted;
  }
};

class PersistenceWorker {
public:
  explicit PersistenceWorker(
      PersistencePaths paths,
      AtomicFileSystem &file_system = linux_atomic_file_system());
  ~PersistenceWorker();

  PersistenceWorker(const PersistenceWorker &) = delete;
  PersistenceWorker &operator=(const PersistenceWorker &) = delete;
  PersistenceWorker(PersistenceWorker &&) = delete;
  PersistenceWorker &operator=(PersistenceWorker &&) = delete;

  [[nodiscard]] SaveSubmission
  save_config(const ConfigSnapshot &snapshot,
              const SafeFileIdentity &expected_identity,
              std::string_view preserved_document = {}, bool read_only = false);
  [[nodiscard]] SaveSubmission
  save_quick_send(const QuickSendSnapshot &snapshot,
                  const SafeFileIdentity &expected_identity,
                  std::string_view preserved_document = {},
                  bool read_only = false);
  [[nodiscard]] SaveSubmission
  save_state(const StateSnapshot &snapshot,
             const SafeFileIdentity &expected_identity,
             std::string_view preserved_document = {}, bool read_only = false);

  // Stops accepting saves. Accepted saves are drained in FIFO order.
  void request_stop() noexcept;
  [[nodiscard]] bool wait_until_stopped(
      std::chrono::steady_clock::time_point deadline) const noexcept;
  // Requests stop and joins after the worker reaches its return point.
  void shutdown();
  [[nodiscard]] bool active(PersistenceFile file) const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lazycom::config
