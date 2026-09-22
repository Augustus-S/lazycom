#pragma once

#include <lazycom/base/worker_signals.hpp>
#include <lazycom/config/persistence.hpp>

namespace lazycom::app {

enum class SaveAdmission { Pending, Queued };
enum class SaveWait { NothingPending, Ready, TimedOut };

struct SettingsCompletion {
  config::PersistenceFile file;
  config::AtomicWriteOutcome outcome;
  bool superseded{};
  std::optional<config::QuickSendSnapshot> committed_quick_send;
};

struct SettingsPoll {
  std::array<std::optional<SettingsCompletion>, 3> completions;
  std::optional<FatalSignal> fatal;
};

/** Owns committed document identities and the three distinct save policies. */
class SettingsCoordinator {
public:
  SettingsCoordinator(config::PersistencePaths paths,
                      config::ConfigLoadResult configuration,
                      config::QuickSendLoadResult quick_send,
                      config::StateLoadResult preferences);

  [[nodiscard]] Result<SaveAdmission>
  save_config(const config::ConfigSnapshot &snapshot);
  [[nodiscard]] Status save_state(const config::StateSnapshot &snapshot);
  [[nodiscard]] Status save_quick_send(config::QuickSendSnapshot candidate);
  [[nodiscard]] SettingsPoll poll();

  [[nodiscard]] bool config_retry_pending() const noexcept;
  [[nodiscard]] bool state_retry_pending() const noexcept;
  [[nodiscard]] bool quick_save_pending() const noexcept;
  [[nodiscard]] bool quick_save_failed() const noexcept;
  [[nodiscard]] bool pending() const noexcept;
  [[nodiscard]] SaveWait
  wait_next(std::chrono::steady_clock::time_point deadline) const;
  void request_stop() noexcept;
  [[nodiscard]] bool wait_until_stopped(
      std::chrono::steady_clock::time_point deadline) const noexcept;
  void shutdown();

private:
  struct DocumentState {
    template <class Snapshot>
    explicit DocumentState(config::SnapshotLoadResult<Snapshot> loaded)
        : identity{loaded.file_identity}, document{std::move(loaded.document)},
          read_only{loaded.read_only} {}

    config::SafeFileIdentity identity;
    std::string document;
    bool read_only{};
    std::optional<std::future<config::SaveCompletion>> completion;
    bool dirty{};
  };

  [[nodiscard]] std::optional<SettingsCompletion>
  take_completion(DocumentState &document, config::PersistenceFile file,
                  std::optional<FatalSignal> &fatal);

  DocumentState configuration_;
  DocumentState quick_send_;
  DocumentState preferences_;
  std::optional<config::QuickSendSnapshot> staged_quick_send_;
  bool quick_save_failed_{};
  config::PersistenceWorker worker_;
};

} // namespace lazycom::app
