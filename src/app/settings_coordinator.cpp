#include <lazycom/app/settings_coordinator.hpp>

namespace lazycom::app {
namespace {

Error save_error(std::string_view detail) {
  return make_error(ErrorCode::ValidationInvalidValue, Operation::SaveConfig,
                    detail);
}

} // namespace

SettingsCoordinator::SettingsCoordinator(config::PersistencePaths paths,
                                         config::ConfigLoadResult configuration,
                                         config::QuickSendLoadResult quick_send,
                                         config::StateLoadResult preferences)
    : configuration_{std::move(configuration)},
      quick_send_{std::move(quick_send)}, preferences_{std::move(preferences)},
      worker_{std::move(paths)} {}

Result<SaveAdmission>
SettingsCoordinator::save_config(const config::ConfigSnapshot &snapshot) {
  if (configuration_.completion) {
    configuration_.dirty = true;
    return SaveAdmission::Queued;
  }
  auto submitted =
      worker_.save_config(snapshot, configuration_.identity,
                          configuration_.document, configuration_.read_only);
  configuration_.dirty = submitted.state == config::SaveSubmitState::Busy;
  if (!submitted.accepted()) {
    return tl::unexpected(
        submitted.error.value_or(save_error("Config save is busy")));
  }
  configuration_.completion = std::move(submitted.completion);
  return SaveAdmission::Pending;
}

Status SettingsCoordinator::save_state(const config::StateSnapshot &snapshot) {
  if (preferences_.completion) {
    preferences_.dirty = true;
    return {};
  }
  auto submitted =
      worker_.save_state(snapshot, preferences_.identity, preferences_.document,
                         preferences_.read_only);
  preferences_.dirty = submitted.state == config::SaveSubmitState::Busy;
  if (!submitted.accepted()) {
    return tl::unexpected(
        submitted.error.value_or(save_error("State save is busy")));
  }
  preferences_.completion = std::move(submitted.completion);
  return {};
}

Status
SettingsCoordinator::save_quick_send(config::QuickSendSnapshot candidate) {
  if (quick_send_.completion) {
    return tl::unexpected(save_error("quick-send save is busy"));
  }
  auto submitted =
      worker_.save_quick_send(candidate, quick_send_.identity,
                              quick_send_.document, quick_send_.read_only);
  if (!submitted.accepted()) {
    return tl::unexpected(
        submitted.error.value_or(save_error("quick-send save is busy")));
  }
  staged_quick_send_ = std::move(candidate);
  quick_send_.completion = std::move(submitted.completion);
  quick_save_failed_ = false;
  return {};
}

std::optional<SettingsCompletion>
SettingsCoordinator::take_completion(DocumentState &document,
                                     const config::PersistenceFile file,
                                     std::optional<FatalSignal> &fatal) {
  if (fatal || !document.completion ||
      document.completion->wait_for(std::chrono::milliseconds::zero()) !=
          std::future_status::ready) {
    return std::nullopt;
  }
  auto completed = document.completion->get();
  document.completion.reset();
  const bool committed =
      completed.outcome.state != config::CommitState::NotCommitted;
  if (completed.file != file ||
      (committed && (!completed.outcome.committed_identity ||
                     !completed.outcome.committed_identity->exists))) {
    fatal = FatalSignal{ErrorCode::InternalInvariantBroken,
                        Operation::CoordinateFatal, WorkerKind::Persistence,
                        FatalReason::InvariantBroken,
                        SignalSourceLocation::current()};
    return std::nullopt;
  }
  SettingsCompletion result{file, std::move(completed.outcome), document.dirty,
                            std::nullopt};
  if (committed) {
    document.identity = *result.outcome.committed_identity;
    document.document = std::move(completed.serialized_document);
  }
  if (file == config::PersistenceFile::QuickSend) {
    quick_save_failed_ = !committed;
    if (committed) {
      result.committed_quick_send = std::move(staged_quick_send_);
    }
    staged_quick_send_.reset();
  }
  return result;
}

SettingsPoll SettingsCoordinator::poll() {
  SettingsPoll result;
  result.completions = {
      take_completion(configuration_, config::PersistenceFile::Config,
                      result.fatal),
      take_completion(quick_send_, config::PersistenceFile::QuickSend,
                      result.fatal),
      take_completion(preferences_, config::PersistenceFile::State,
                      result.fatal)};
  return result;
}

bool SettingsCoordinator::config_retry_pending() const noexcept {
  return configuration_.dirty && !configuration_.completion;
}
bool SettingsCoordinator::state_retry_pending() const noexcept {
  return preferences_.dirty && !preferences_.completion;
}
bool SettingsCoordinator::quick_save_pending() const noexcept {
  return quick_send_.completion.has_value();
}
bool SettingsCoordinator::quick_save_failed() const noexcept {
  return quick_save_failed_;
}
bool SettingsCoordinator::pending() const noexcept {
  return configuration_.dirty || preferences_.dirty ||
         configuration_.completion || quick_send_.completion ||
         preferences_.completion;
}
SaveWait SettingsCoordinator::wait_next(
    const std::chrono::steady_clock::time_point deadline) const {
  for (const auto *document : {&configuration_, &quick_send_, &preferences_}) {
    if (document->completion) {
      return document->completion->wait_until(deadline) ==
                     std::future_status::ready
                 ? SaveWait::Ready
                 : SaveWait::TimedOut;
    }
  }
  return SaveWait::NothingPending;
}
void SettingsCoordinator::request_stop() noexcept { worker_.request_stop(); }
bool SettingsCoordinator::wait_until_stopped(
    const std::chrono::steady_clock::time_point deadline) const noexcept {
  return worker_.wait_until_stopped(deadline);
}
void SettingsCoordinator::shutdown() { worker_.shutdown(); }

} // namespace lazycom::app
