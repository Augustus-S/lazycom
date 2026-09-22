#include <lazycom/app/application.hpp>

#include "messages.hpp"
#include <lazycom/base/text.hpp>

#include <lazycom/diagnostics/diagnostics.hpp>
#include <lazycom/logging/schema.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include <unistd.h>

namespace lazycom::app {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] Error
application_error(std::string_view detail,
                  Operation operation = Operation::ValidateConfig) {
  return make_error(ErrorCode::ValidationInvalidValue, operation, detail);
}

[[nodiscard]] std::filesystem::path
absolute_environment_path(const char *name,
                          const std::filesystem::path &fallback) {
  const char *const value = std::getenv(name);
  const std::filesystem::path selected = value != nullptr && *value != '\0'
                                             ? std::filesystem::path{value}
                                             : fallback;
  if (!selected.is_absolute()) {
    throw std::runtime_error(std::string{name} + " must be an absolute path");
  }
  return selected;
}

[[nodiscard]] std::filesystem::path home_directory() {
  const char *const home = std::getenv("HOME");
  if (home == nullptr || *home == '\0' ||
      !std::filesystem::path{home}.is_absolute()) {
    throw std::runtime_error("HOME must name an absolute directory");
  }
  return std::filesystem::path{home};
}

[[nodiscard]] logging::Parity to_log_parity(config::Parity value) noexcept {
  switch (value) {
  case config::Parity::None:
    return logging::Parity::None;
  case config::Parity::Odd:
    return logging::Parity::Odd;
  case config::Parity::Even:
    return logging::Parity::Even;
  case config::Parity::Mark:
    return logging::Parity::Mark;
  case config::Parity::Space:
    return logging::Parity::Space;
  }
  return logging::Parity::None;
}

[[nodiscard]] logging::FlowControl
to_log_flow(config::FlowControl value) noexcept {
  switch (value) {
  case config::FlowControl::None:
    return logging::FlowControl::None;
  case config::FlowControl::RtsCts:
    return logging::FlowControl::RtsCts;
  case config::FlowControl::XonXoff:
    return logging::FlowControl::XonXoff;
  }
  return logging::FlowControl::None;
}

[[nodiscard]] Result<config::ConfigSnapshot>
logging_candidate(const config::ConfigSnapshot &current,
                  const LogSettingsCandidate &value) {
  auto candidate = current;
  candidate.logging.directory = value.directory;
  if (auto checked = logging::validate_session_log_directory(value.directory);
      !checked) {
    return tl::unexpected(std::move(checked.error()));
  }
  candidate.logging.max_files = value.max_files;
  candidate.logging.max_total_size_mib = value.max_total_size_mib;
  candidate.logging.max_file_size_mib = value.max_file_size_mib;
  if (const auto errors = config::validate_config_snapshot(candidate);
      !errors.empty()) {
    return tl::unexpected(application_error(errors.front().message));
  }
  return candidate;
}

} // namespace

struct Application::LoadedConfiguration {
  config::PersistencePaths paths;
  config::ConfigLoadResult config;
  config::QuickSendLoadResult quick;
  config::StateLoadResult state;
  std::filesystem::path default_log_directory;
  std::string notice;
};

Result<Application::LoadedConfiguration> Application::load_configuration(
    const std::optional<config::PersistencePaths> &provided_paths,
    const std::optional<std::filesystem::path> &provided_log_directory) {
  try {
    const auto home = home_directory();
    const auto config_root =
        absolute_environment_path("XDG_CONFIG_HOME", home / ".config");
    const auto state_root =
        absolute_environment_path("XDG_STATE_HOME", home / ".local" / "state");
    LoadedConfiguration loaded;
    loaded.paths = provided_paths.value_or(
        config::PersistencePaths{config_root / "lazycom" / "config.toml",
                                 config_root / "lazycom" / "quick_send.toml",
                                 state_root / "lazycom" / "state.toml"});
    if (!loaded.paths.config.is_absolute() ||
        !loaded.paths.quick_send.is_absolute() ||
        !loaded.paths.state.is_absolute()) {
      return tl::unexpected(
          application_error("configuration paths must be absolute"));
    }
    loaded.config = config::load_config_toml(loaded.paths.config);
    loaded.quick = config::load_quick_send_toml(loaded.paths.quick_send);
    loaded.state = config::load_state_toml(loaded.paths.state);
    loaded.default_log_directory =
        provided_log_directory.value_or(state_root / "lazycom" / "logs");
    const auto effective_log_directory =
        loaded.config.snapshot.logging.directory.empty()
            ? loaded.default_log_directory
            : std::filesystem::path{loaded.config.snapshot.logging.directory};
    if (!effective_log_directory.is_absolute()) {
      return tl::unexpected(
          application_error("log directory must be absolute"));
    }
    const auto append_load_notice = [&loaded](std::string_view name,
                                              const auto &result) {
      if (!result.errors.empty()) {
        if (!loaded.notice.empty()) {
          loaded.notice += " | ";
        }
        loaded.notice +=
            std::string{name} + " rejected; read-only defaults active";
      }
    };
    append_load_notice("config.toml", loaded.config);
    append_load_notice("quick_send.toml", loaded.quick);
    append_load_notice("state.toml", loaded.state);
    return loaded;
  } catch (const std::exception &exception) {
    return tl::unexpected(
        application_error(exception.what(), Operation::ReadConfig));
  }
}

Result<std::unique_ptr<Application>>
Application::create(ApplicationDependencies dependencies) {
  if (!dependencies.serial_backend || !dependencies.scanner_backend) {
    return tl::unexpected(
        application_error("both serial backends are required"));
  }
  auto loaded =
      load_configuration(dependencies.paths, dependencies.log_directory);
  if (!loaded) {
    return tl::unexpected(std::move(loaded.error()));
  }
  auto serial_service = serial::SerialService::create(
      std::move(dependencies.serial_backend),
      serial::SerialServiceOptions::from_config(loaded->config.snapshot),
      dependencies.wake_callback, dependencies.wake_context);
  if (!serial_service) {
    return tl::unexpected(std::move(serial_service.error()));
  }
  auto scanner = serial::DeviceScanner::create(
      std::move(dependencies.scanner_backend), dependencies.wake_callback,
      dependencies.wake_context);
  if (!scanner) {
    return tl::unexpected(std::move(scanner.error()));
  }
  try {
    auto logs = std::make_unique<SessionLogCoordinator>(
        loaded->config.snapshot, loaded->default_log_directory,
        dependencies.memory_budget);
    return std::unique_ptr<Application>(new Application(
        std::move(dependencies), std::move(*loaded), std::move(*serial_service),
        std::move(*scanner), std::move(logs)));
  } catch (const std::exception &exception) {
    return tl::unexpected(
        application_error(exception.what(), Operation::WriteSessionLog));
  }
}

Result<std::unique_ptr<Application>>
Application::create_default(const serial::UiWakeCallback wake_callback,
                            void *const wake_context) {
  ApplicationDependencies dependencies;
  dependencies.serial_backend =
      std::make_unique<serial::LibserialportBackend>();
  dependencies.scanner_backend =
      std::make_unique<serial::LibserialportBackend>();
  dependencies.wake_callback = wake_callback;
  dependencies.wake_context = wake_context;
  return create(std::move(dependencies));
}

Application::Application(ApplicationDependencies dependencies,
                         LoadedConfiguration loaded,
                         std::unique_ptr<serial::SerialService> serial_service,
                         std::unique_ptr<serial::DeviceScanner> scanner,
                         std::unique_ptr<SessionLogCoordinator> logs)
    : memory_budget_{dependencies.memory_budget}, snapshot_{records_.visible()},
      serial_(std::move(serial_service)), scanner_(std::move(scanner)),
      logs_(std::move(logs)),
      default_log_directory_(std::move(loaded.default_log_directory)) {
  snapshot_.config = loaded.config.snapshot;
  snapshot_.quick_send = loaded.quick.snapshot;
  snapshot_.preferences = loaded.state.snapshot;
  snapshot_.filter = {
      snapshot_.preferences.show_rx, snapshot_.preferences.show_tx,
      snapshot_.preferences.show_system, snapshot_.preferences.show_error};
  snapshot_.config_read_only = loaded.config.read_only;
  snapshot_.quick_send_read_only = loaded.quick.read_only;
  snapshot_.state_read_only = loaded.state.read_only;
  snapshot_.configuration_notice = std::move(loaded.notice);
  snapshot_.effective_log_directory = snapshot_.config.logging.directory.empty()
                                          ? default_log_directory_.string()
                                          : snapshot_.config.logging.directory;
  settings_ = std::make_unique<SettingsCoordinator>(
      std::move(loaded.paths), std::move(loaded.config),
      std::move(loaded.quick), std::move(loaded.state));
  snapshot_.log = logs_->state();
  request_scan();
}

Application::~Application() {
  if (!shutdown() && !stopped_) {
    abort_after_shutdown_timeout();
  }
}

const ApplicationSnapshot &Application::snapshot() const noexcept {
  return snapshot_;
}

OperationId Application::issue_operation() {
  OperationId result;
  if (serial_->issue_operation(result) != IdIncrementResult::Advanced) {
    enter_fatal_stopping(*serial_->fatal_signal());
    return {};
  }
  return result;
}

ConnectionGeneration Application::issue_connection_generation() {
  ConnectionGeneration result;
  if (connection_ids_.issue(result) != IdIncrementResult::Advanced) {
    throw std::overflow_error("connection generation exhausted");
  }
  return result;
}

ScanGeneration Application::issue_scan_generation() {
  ScanGeneration result;
  if (scan_ids_.issue(result) != IdIncrementResult::Advanced) {
    throw std::overflow_error("scan generation exhausted");
  }
  return result;
}

void Application::set_notice(std::string message, const bool error) {
  if (!error && snapshot_.notice_error) {
    return;
  }
  snapshot_.notice = lazycom::sanitize_message(message);
  snapshot_.notice_error = error && !snapshot_.notice.empty();
  if (snapshot_.notice_revision == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("notice revision exhausted");
  }
  ++snapshot_.notice_revision;
  notice_deadline_ = snapshot_.notice_error || snapshot_.notice.empty()
                         ? std::nullopt
                         : std::optional{std::chrono::steady_clock::now() + 3s};
}

void Application::publish_notice(std::string message, const bool error) {
  set_notice(std::move(message), error);
}

void Application::dismiss_notice() noexcept {
  snapshot_.notice.clear();
  snapshot_.notice_error = false;
  notice_deadline_.reset();
  if (snapshot_.notice_revision != std::numeric_limits<std::uint64_t>::max()) {
    ++snapshot_.notice_revision;
  }
}

bool Application::operations_allowed() const noexcept {
  return !stopped_ && !snapshot_.fatal_stopping;
}

Status Application::operation_rejected() const {
  return tl::unexpected(
      application_error("application is stopping after a fatal worker failure",
                        Operation::CoordinateFatal));
}

void Application::publish_permission_alert(
    const std::string_view path, const serial::DevicePermission &permission) {
  std::ostringstream message;
  message << "Cannot open " << lazycom::sanitize_message(path) << ".\n\n"
          << "Effective UID: " << static_cast<std::uint64_t>(::geteuid())
          << "\n"
          << "Owner UID: " << permission.owner_uid << "\n"
          << "Group: " << lazycom::sanitize_message(permission.group_name)
          << " (" << permission.owner_gid << ")\n"
          << "Mode: 0" << std::oct << permission.mode << std::dec << "\n\n"
          << "The current user cannot read and write this device. Check the "
             "dialout/uucp device group and sign in again after membership "
             "changes. If needed, ask an administrator to inspect udev rules "
             "and whether another service is using the device.";
  snapshot_.alert = UserAlert{ErrorCode::SerialPermissionDenied,
                              "Serial Permission Denied", message.str()};
}

void Application::dismiss_alert() noexcept { snapshot_.alert.reset(); }

void Application::publish_alert(UserAlert alert) {
  alert.title = lazycom::sanitize_message(alert.title);
  alert.message = lazycom::sanitize_message(alert.message);
  snapshot_.alert = std::move(alert);
}

void Application::request_scan() {
  if (!operations_allowed()) {
    set_notice("Application is stopping; device scan rejected");
    return;
  }
  try {
    const auto operation = issue_operation();
    if (operation.value == 0U) {
      return;
    }
    const auto generation = issue_scan_generation();
    const auto submitted = scanner_->submit_scan(operation, generation);
    if (!submitted) {
      set_notice(error_text(submitted.error()));
      return;
    }
    latest_scan_generation_ = generation;
    snapshot_.scanning = true;
    set_notice("Scanning serial devices...", false);
  } catch (const std::exception &exception) {
    set_notice(exception.what());
  }
}

void Application::set_device_path(std::string path) {
  if (!operations_allowed()) {
    set_notice("Application is stopping; device selection rejected");
    return;
  }
  if (connection_.state() != ConnectionState::Disconnected) {
    set_notice("Hardware settings are locked while the link is active");
    return;
  }
  device_path_ = std::move(path);
  snapshot_.device_path = lazycom::sanitize_message(device_path_);
}

void Application::start_connection() {
  if (!operations_allowed()) {
    set_notice("Application is stopping; connection rejected");
    return;
  }
  if (device_path_.empty()) {
    request_scan();
    set_notice("Select a scanned serial device");
    return;
  }
  auto path = serial::inspect_device_path(device_path_);
  if (!path) {
    set_notice(error_text(path.error()));
    return;
  }
  const auto permission = serial::inspect_device_permission(path->canonical);
  if (!permission) {
    set_notice(error_text(permission.error()));
    return;
  }
  if (permission->access == serial::DeviceAccess::PermissionDenied) {
    publish_permission_alert(path->canonical, *permission);
    set_notice("Permission denied for the selected serial device");
    return;
  }
  try {
    const auto command =
        ConnectCommand{issue_operation(), issue_connection_generation()};
    if (command.operation_id.value == 0U) {
      return;
    }
    if (connection_.begin_connect(command) != StateChange::Applied) {
      set_notice("Cannot start this connection generation");
      return;
    }
    const auto port =
        serial::PortConfig::from_defaults(snapshot_.config.serial);
    auto submitted = serial_->submit_connect({command, std::move(*path), port});
    if (!submitted) {
      const ConnectFailed failed{command.operation_id, command.generation};
      static_cast<void>(connection_.connect_failed(failed));
      const DisconnectCommand cleanup{issue_operation(), command.generation,
                                      std::nullopt};
      if (cleanup.operation_id.value == 0U) {
        return;
      }
      static_cast<void>(connection_.begin_disconnect(cleanup));
      static_cast<void>(connection_.disconnect_completed(
          {cleanup.operation_id, cleanup.generation, std::nullopt,
           OperationOutcome::Failed}));
      set_notice(error_text(submitted.error()));
      snapshot_.connection = connection_.state();
      return;
    }
    connected_port_config_ = port;
    snapshot_.active_port_config = port;
    snapshot_.connection = connection_.state();
    set_notice("Connecting to serial device...", false);
  } catch (const std::exception &exception) {
    set_notice(exception.what());
  }
}

void Application::request_disconnect() {
  if (!connection_.generation() || !connection_.session_id()) {
    return;
  }
  try {
    const DisconnectCommand command{
        issue_operation(), *connection_.generation(), connection_.session_id()};
    if (command.operation_id.value == 0U) {
      return;
    }
    auto submitted = serial_->request_disconnect(command);
    if (!submitted) {
      set_notice(error_text(submitted.error()));
      return;
    }
    if (*submitted == serial::SubmitStatus::AlreadyPending) {
      set_notice("A disconnect request is already pending", false);
      return;
    }
    if (connection_.begin_disconnect(command) != StateChange::Applied) {
      enter_fatal_stopping({ErrorCode::InternalInvariantBroken,
                            Operation::CoordinateFatal, WorkerKind::Serial,
                            FatalReason::InvariantBroken,
                            SignalSourceLocation::current()});
      return;
    }
    stop_quick_task();
    set_notice("Disconnecting serial session...", false);
    snapshot_.connection = connection_.state();
  } catch (const std::exception &exception) {
    set_notice(exception.what());
  }
}

void Application::request_cancel_connect() {
  if (!connection_.generation()) {
    return;
  }
  try {
    const CancelConnectCommand command{issue_operation(),
                                       *connection_.generation()};
    if (command.operation_id.value == 0U) {
      return;
    }
    auto submitted = serial_->request_cancel_connect(command);
    if (!submitted) {
      const auto owner = serial_->connection_snapshot();
      if (owner.connected && owner.generation == command.generation) {
        // The connect committed at the owner between the UI snapshot and this
        // submission. Keep local state unchanged until its completion installs
        // the session, then submit a real disconnect for the user's intent.
        cancel_race_disconnect_pending_ = true;
        set_notice("Connection completed while cancellation raced; waiting for "
                   "its session boundary...",
                   false);
        return;
      }
      set_notice(error_text(submitted.error()));
      return;
    }
    if (*submitted == serial::SubmitStatus::AlreadyPending) {
      set_notice("A connection cancellation is already pending", false);
      return;
    }
    if (connection_.cancel_connect(command) != StateChange::Applied) {
      enter_fatal_stopping({ErrorCode::InternalInvariantBroken,
                            Operation::CoordinateFatal, WorkerKind::Serial,
                            FatalReason::InvariantBroken,
                            SignalSourceLocation::current()});
      return;
    }
    snapshot_.connection = connection_.state();
    set_notice("Cancelling connection...", false);
  } catch (const std::exception &exception) {
    set_notice(exception.what());
  }
}

void Application::connect() {
  if (!operations_allowed()) {
    set_notice("Application is stopping; connection rejected");
    return;
  }
  if (connection_.state() != ConnectionState::Disconnected) {
    set_notice("Connect requires a disconnected link");
    return;
  }
  if (logs_->command_pending()) {
    connect_after_log_ = true;
    set_notice("Finishing the previous session log before reconnecting...",
               false);
    return;
  }
  start_connection();
}

void Application::disconnect() {
  if (!operations_allowed()) {
    set_notice("Application is stopping; disconnect rejected");
    return;
  }
  switch (connection_.state()) {
  case ConnectionState::Connecting:
    request_cancel_connect();
    break;
  case ConnectionState::Connected:
  case ConnectionState::Error:
    request_disconnect();
    break;
  case ConnectionState::Disconnected:
    if (connect_after_log_) {
      connect_after_log_ = false;
      set_notice("Pending connection cancelled", false);
    } else {
      set_notice("Disconnect requires an active link");
    }
    break;
  case ConnectionState::Disconnecting:
    set_notice("Disconnect is already in progress", false);
    break;
  }
}

void Application::connection_control() {
  if (connection_.state() == ConnectionState::Disconnected) {
    if (connect_after_log_) {
      connect_after_log_ = false;
      set_notice("Pending connection cancelled", false);
    } else {
      connect();
    }
  } else {
    disconnect();
  }
}

void Application::set_interaction(const InteractionState state) noexcept {
  snapshot_.interaction = state;
}

void Application::set_draft(std::string draft) {
  if (!operations_allowed()) {
    set_notice("Application is stopping; draft edit rejected");
    return;
  }
  if (draft.size() > snapshot_.config.send.max_draft_bytes) {
    set_notice("Draft reached its configured byte limit");
    return;
  }
  if (!lazycom::is_strict_utf8(
          std::as_bytes(std::span{draft.data(), draft.size()}))) {
    set_notice("Draft must contain valid UTF-8 text");
    return;
  }
  snapshot_.draft = std::move(draft);
  history_index_.reset();
}

bool Application::submit_tx(std::vector<std::byte> payload,
                            const config::SendMode input_mode) {
  if (!operations_allowed()) {
    set_notice("Application is stopping; TX rejected");
    return false;
  }
  if (connection_.state() != ConnectionState::Connected ||
      !connection_.generation() || !connection_.session_id()) {
    set_notice("Not connected; draft was kept");
    return false;
  }
  if (payload.empty()) {
    set_notice("Nothing to send");
    return false;
  }
  OperationId operation{};
  try {
    operation = issue_operation();
    if (operation.value == 0U) {
      return false;
    }
    const SendCommand command{operation, *connection_.generation(),
                              *connection_.session_id(), std::nullopt};
    if (!connection_.accepts(command)) {
      set_notice("TX rejected by the active session guard");
      return false;
    }
    auto submitted =
        serial_->submit_tx({command, std::move(payload), input_mode});
    if (!submitted) {
      set_notice(error_text(submitted.error()));
      return false;
    }
    snapshot_.tx_pending = serial_->pending_tx_requests();
    return true;
  } catch (const std::exception &exception) {
    set_notice(exception.what());
    return false;
  }
}

bool Application::submit_draft() {
  if (!operations_allowed()) {
    set_notice("Application is stopping; TX rejected");
    return false;
  }
  if (connection_.state() != ConnectionState::Connected) {
    set_notice("Not connected; draft was kept");
    return false;
  }
  auto parsed =
      scheduler::parse_payload(snapshot_.config.send.mode, snapshot_.draft,
                               snapshot_.config.send.newline);
  if (!parsed) {
    set_notice("Draft is not valid for the selected TXT/HEX mode");
    return false;
  }
  if (!submit_tx(std::move(parsed.bytes), snapshot_.config.send.mode)) {
    return false;
  }
  if (history_.empty() || history_.back() != snapshot_.draft) {
    history_.push_back(snapshot_.draft);
    history_bytes_ += sizeof(std::string) + history_.back().capacity();
    const auto maximum_bytes =
        static_cast<std::size_t>(snapshot_.config.send.history_max_mib) *
        1024U * 1024U;
    while (!history_.empty() &&
           (history_.size() > snapshot_.config.send.history_max_entries ||
            history_bytes_ > maximum_bytes)) {
      history_bytes_ -= sizeof(std::string) + history_.front().capacity();
      history_.pop_front();
    }
  }
  snapshot_.draft.clear();
  history_index_.reset();
  set_notice("TX accepted by serial owner", false);
  return true;
}

void Application::history_previous() {
  if (!operations_allowed()) {
    set_notice("Application is stopping; history navigation rejected");
    return;
  }
  if (history_.empty()) {
    return;
  }
  if (!history_index_) {
    history_sentinel_ = snapshot_.draft;
    history_index_ = history_.size() - 1U;
  } else if (*history_index_ != 0U) {
    --*history_index_;
  }
  snapshot_.draft = history_[*history_index_];
}

void Application::history_next() {
  if (!operations_allowed()) {
    set_notice("Application is stopping; history navigation rejected");
    return;
  }
  if (!history_index_) {
    return;
  }
  if (*history_index_ + 1U < history_.size()) {
    ++*history_index_;
    snapshot_.draft = history_[*history_index_];
  } else {
    snapshot_.draft = history_sentinel_;
    history_index_.reset();
  }
}

void Application::synchronize_records() noexcept {
  snapshot_.rx_bytes = records_.rx_bytes();
  snapshot_.tx_bytes = records_.tx_bytes();
  snapshot_.display_bytes = records_.display_bytes();
  snapshot_.display_gap_records = records_.gap_records();
}

void Application::enqueue_frames(std::vector<framing::RxFrame> frames,
                                 const SessionEventOrigin origin) {
  for (auto &frame : frames) {
    records_.account_rx(frame.bytes.size());
    enqueue_record(RecordDirection::Rx, frame.bytes, {}, std::nullopt,
                   std::nullopt, origin, frame.first_byte_observed_at,
                   frame.first_byte_observed_utc);
  }
}

void Application::enqueue_record(
    const RecordDirection direction, const std::span<const std::byte> payload,
    std::string message, const std::optional<ErrorCode> error_code,
    const std::optional<OperationId> operation, const SessionEventOrigin origin,
    const std::chrono::steady_clock::time_point observed_at,
    const std::chrono::system_clock::time_point time_utc,
    const std::optional<config::SendMode> input_mode) {
  const bool normal = origin == SessionEventOrigin::Normal;
  const bool late_cleanup_error =
      !normal && direction == RecordDirection::Error &&
      connection_.state() == ConnectionState::Disconnected;
  if ((normal && connection_.state() != ConnectionState::Connected &&
       connection_.state() != ConnectionState::Disconnecting) ||
      (!normal && connection_.state() != ConnectionState::Disconnecting &&
       connection_.state() != ConnectionState::Error && !late_cleanup_error)) {
    return;
  }
  if (direction == RecordDirection::Tx && !input_mode) {
    enter_fatal_stopping({ErrorCode::InternalInvariantBroken,
                          Operation::CoordinateFatal, WorkerKind::Serial,
                          FatalReason::InvariantBroken,
                          SignalSourceLocation::current()});
    return;
  }
  auto record = records_.append(snapshot_.config.receive, origin, direction,
                                payload, std::move(message), error_code,
                                operation, observed_at, time_utc, input_mode);
  synchronize_records();
  if (record.status != model::SequenceStatus::Accepted) {
    if (late_cleanup_error && record.status == model::SequenceStatus::Closed) {
      set_notice("Serial cleanup failed after the session closed");
      return;
    }
    if (record.status == model::SequenceStatus::BudgetExhausted) {
      logs_->record_overflow();
      synchronize_log_state();
      if (direction == RecordDirection::Rx &&
          connection_.state() == ConnectionState::Connected) {
        request_disconnect();
      }
      set_notice(
          "Record memory limit reached; received data may be incomplete");
      return;
    }
    enter_fatal_stopping({ErrorCode::InternalInvariantBroken,
                          Operation::CoordinateFatal, WorkerKind::Serial,
                          FatalReason::InvariantBroken,
                          SignalSourceLocation::current()});
    return;
  }
  logs_->enqueue(std::move(record.record), log_context());
  synchronize_log_state();
}

bool Application::process_serial_data() {
  // The owner may publish bytes immediately after its connect completion. Keep
  // them queued until the application has installed the matching session.
  if (!connection_.session_id() &&
      (connection_.state() == ConnectionState::Connecting ||
       connection_.state() == ConnectionState::Disconnecting)) {
    return false;
  }
  while (true) {
    auto events = serial_->drain_data(512U);
    if (events.empty()) {
      return true;
    }
    const auto event_count = events.size();
    for (auto &event : events) {
      if (!connection_.generation() || !connection_.session_id() ||
          event.generation != *connection_.generation() ||
          event.session_id != *connection_.session_id()) {
        continue;
      }
      if (event.kind == serial::SerialDataKind::Rx) {
        const SessionDataEvent guard{event.generation, event.session_id,
                                     event.origin, SessionEventKind::Rx};
        if (connection_.accepts(guard) && records_.active()) {
          enqueue_frames(
              records_.push(event.bytes, event.observed_at, event.time_utc),
              event.origin);
        }
      } else if (event.kind == serial::SerialDataKind::Tx) {
        const SessionDataEvent guard{event.generation, event.session_id,
                                     event.origin, SessionEventKind::Tx};
        if (connection_.accepts(guard)) {
          records_.account_tx(event.bytes.size());
          enqueue_record(RecordDirection::Tx, event.bytes, {}, std::nullopt,
                         event.operation_id, event.origin, event.observed_at,
                         event.time_utc, event.input_mode);
        }
      } else if (event.kind == serial::SerialDataKind::Error) {
        const SessionDataEvent guard{event.generation, event.session_id,
                                     event.origin, SessionEventKind::Error};
        if (event.error && connection_.accepts(guard)) {
          enqueue_record(RecordDirection::Error, {}, error_text(*event.error),
                         event.error->code,
                         event.error->operation_id ? event.error->operation_id
                                                   : event.operation_id,
                         event.origin, event.observed_at, event.time_utc);
        }
      } else {
        if (processed_cleanup_ ==
            LogSessionOwner{event.generation, event.session_id}) {
          continue;
        }
        processed_cleanup_ =
            LogSessionOwner{event.generation, event.session_id};
        bool owner_fault_cleanup = false;
        if (connection_.state() == ConnectionState::Connected) {
          owner_fault_cleanup =
              connection_.session_fault({event.generation, event.session_id}) ==
              StateChange::Applied;
        }
        if (records_.active()) {
          enqueue_frames(records_.flush(), SessionEventOrigin::Cleanup);
        }
        if (event.error) {
          enqueue_record(RecordDirection::Error, {}, error_text(*event.error),
                         event.error->code, event.error->operation_id,
                         SessionEventOrigin::Cleanup, event.observed_at,
                         event.time_utc);
        } else {
          enqueue_record(RecordDirection::System, {}, "Serial session closed",
                         std::nullopt, std::nullopt,
                         SessionEventOrigin::Cleanup, event.observed_at,
                         event.time_utc);
        }
        if (owner_fault_cleanup) {
          try {
            const DisconnectCommand cleanup{issue_operation(), event.generation,
                                            event.session_id};
            if (cleanup.operation_id.value == 0U) {
              return false;
            }
            if (connection_.begin_disconnect(cleanup) == StateChange::Applied) {
              deferred_disconnect_completion_ = serial::DisconnectCompletion{
                  cleanup.operation_id, cleanup.generation, cleanup.session_id,
                  OperationOutcome::Failed, event.error};
            }
          } catch (const std::exception &exception) {
            set_notice(exception.what());
          }
        }
        end_log_session(event.generation, event.session_id);
      }
    }
    // A disconnect closes the owner before publishing its completion, so a
    // bounded drain is finite and must reach the cleanup marker. Connected RX
    // remains budgeted to one batch per UI tick.
    if (event_count < 512U ||
        connection_.state() == ConnectionState::Connected) {
      return false;
    }
  }
}

void Application::finish_failed_connection(
    const serial::ConnectCompletion &completion) {
  const auto changed = connection_.connect_failed(
      {completion.operation_id, completion.generation});
  if (changed != StateChange::Applied) {
    return;
  }
  try {
    if (connection_.state() == ConnectionState::Disconnecting &&
        connection_.cleanup_operation()) {
      // A submitted cancel owns the transition until its service completion.
    } else {
      const DisconnectCommand cleanup{issue_operation(), completion.generation,
                                      std::nullopt};
      if (cleanup.operation_id.value == 0U) {
        return;
      }
      static_cast<void>(connection_.begin_disconnect(cleanup));
      static_cast<void>(connection_.disconnect_completed(
          {cleanup.operation_id, cleanup.generation, std::nullopt,
           completion.outcome}));
    }
  } catch (const std::exception &exception) {
    set_notice(exception.what());
  }
  connected_port_config_.reset();
  snapshot_.active_port_config.reset();
  if (completion.error &&
      completion.error->code == ErrorCode::SerialPermissionDenied) {
    const auto permission = serial::inspect_device_permission(device_path_);
    if (permission) {
      publish_permission_alert(device_path_, *permission);
    } else {
      snapshot_.alert =
          UserAlert{ErrorCode::SerialPermissionDenied,
                    "Serial Permission Denied", error_text(*completion.error)};
    }
  }
  set_notice(completion.error ? error_text(*completion.error)
                              : "Connection was cancelled",
             completion.error.has_value());
}

void Application::retry_cancel_race_disconnect() {
  if (!cancel_race_disconnect_pending_ ||
      (connection_.state() != ConnectionState::Connected &&
       connection_.state() != ConnectionState::Disconnecting) ||
      !connection_.generation() || !connection_.session_id()) {
    return;
  }
  try {
    const DisconnectCommand command{
        issue_operation(), *connection_.generation(), connection_.session_id()};
    if (command.operation_id.value == 0U) {
      return;
    }
    auto submitted = serial_->request_disconnect(command);
    if (!submitted) {
      set_notice(error_text(submitted.error()));
      return;
    }
    if (*submitted == serial::SubmitStatus::AlreadyPending) {
      set_notice("Racing connection cleanup is already pending", false);
      return;
    }
    if (connection_.begin_disconnect(command) != StateChange::Applied) {
      enter_fatal_stopping({ErrorCode::InternalInvariantBroken,
                            Operation::CoordinateFatal, WorkerKind::Serial,
                            FatalReason::InvariantBroken,
                            SignalSourceLocation::current()});
      return;
    }
    cancel_race_disconnect_pending_ = false;
    set_notice("Connection completed while cancellation raced; closing it...",
               false);
  } catch (const std::exception &exception) {
    set_notice(exception.what());
  }
}

void Application::process_serial_completions() {
  auto completions = serial_->drain_completions();
  const auto log_boundary_ready = [this](const ConnectionGeneration generation,
                                         const SessionId session_id) {
    return logs_->closed({generation, session_id});
  };
  // Completion and data use independent channels. Hold disconnect completion
  // until this session's ordered Cleanup event and its log close boundary have
  // both been processed, so session identity cannot be cleared prematurely.
  if (deferred_disconnect_completion_) {
    const auto &value = *deferred_disconnect_completion_;
    const bool cleanup_ready =
        !value.session_id ||
        (processed_cleanup_ &&
         *processed_cleanup_ ==
             LogSessionOwner{value.generation, *value.session_id} &&
         log_boundary_ready(value.generation, *value.session_id));
    if (cleanup_ready) {
      completions.emplace_back(std::move(*deferred_disconnect_completion_));
      deferred_disconnect_completion_.reset();
    }
  }
  for (auto &completion : completions) {
    std::visit(
        [this, &log_boundary_ready](auto &value) {
          using Value = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, serial::ConnectCompletion>) {
            if (value.outcome == OperationOutcome::Succeeded &&
                value.session_id) {
              const auto changed = connection_.connect_succeeded(
                  {value.operation_id, value.generation, *value.session_id});
              if (changed == StateChange::Applied) {
                records_.start(snapshot_.config.receive, value.observed_at,
                               value.time_utc);
                processed_cleanup_.reset();
                capture_log_session();
                set_interaction(InteractionState::Normal);
                begin_log_session();
                if (connection_.state() == ConnectionState::Connected &&
                    !cancel_race_disconnect_pending_) {
                  set_notice("Serial connection established", false);
                  enqueue_record(RecordDirection::System, {},
                                 "Serial connection established", std::nullopt,
                                 std::nullopt, SessionEventOrigin::Normal,
                                 value.observed_at, value.time_utc);
                } else {
                  cancel_race_disconnect_pending_ = true;
                  retry_cancel_race_disconnect();
                }
              }
            } else {
              finish_failed_connection(value);
            }
          } else if constexpr (std::is_same_v<Value, serial::TxCompletion>) {
            // Owner completion releases admission; ordered data carries payload
            // and errors.
          } else if constexpr (std::is_same_v<Value,
                                              serial::DisconnectCompletion>) {
            if (connection_.generation() != value.generation ||
                connection_.session_id() != value.session_id ||
                connection_.cleanup_operation() != value.operation_id) {
              return;
            }
            if (value.session_id &&
                (!processed_cleanup_ ||
                 *processed_cleanup_ !=
                     LogSessionOwner{value.generation, *value.session_id} ||
                 !log_boundary_ready(value.generation, *value.session_id))) {
              deferred_disconnect_completion_ = std::move(value);
              return;
            }
            const auto changed = connection_.disconnect_completed(
                {value.operation_id, value.generation, value.session_id,
                 value.outcome});
            if (changed == StateChange::Applied) {
              processed_cleanup_.reset();
              cancel_race_disconnect_pending_ = false;
              records_.finish();
              connected_port_config_.reset();
              snapshot_.active_port_config.reset();
              set_interaction(InteractionState::Normal);
              if (logs_->state() != LogState::Error ||
                  !snapshot_.notice_error) {
                set_notice(value.error ? error_text(*value.error)
                           : value.outcome == OperationOutcome::Succeeded
                               ? "Serial session disconnected"
                               : "Serial session closed with an error",
                           value.error.has_value() ||
                               value.outcome != OperationOutcome::Succeeded);
              }
            }
          } else if constexpr (std::is_same_v<Value,
                                              serial::TaskStartCompletion>) {
            if (!pending_task_start_ ||
                pending_task_start_->operation_id != value.operation_id) {
              return;
            }
            const auto intent = *pending_task_start_;
            pending_task_start_.reset();
            if (value.outcome == OperationOutcome::Succeeded &&
                connection_.generation() == value.generation &&
                connection_.session_id() == value.session_id) {
              snapshot_.preferences.last_quick_send_slot = intent.slot;
              snapshot_.preferences.last_interval_ms =
                  static_cast<std::uint32_t>(intent.interval_ms);
              save_state();
              set_notice(intent.interval_ms == 0U
                             ? "Quick send submitted"
                             : "Periodic quick-send task started",
                         false);
            } else if (value.outcome == OperationOutcome::Failed) {
              set_notice("Quick-send task could not start; the active task or "
                         "session changed");
            }
          } else if constexpr (std::is_same_v<Value,
                                              serial::TaskStopCompletion>) {
            if (pending_task_stop_ == value.operation_id) {
              pending_task_stop_.reset();
              if (value.outcome == OperationOutcome::Succeeded) {
                set_notice("Quick-send task stopped", false);
              }
            }
          }
        },
        completion);
  }
  snapshot_.connection = connection_.state();
  snapshot_.tx_pending = serial_->pending_tx_requests();
  retry_cancel_race_disconnect();
}

void Application::process_scan_completions() {
  for (auto &completion : scanner_->drain_completions()) {
    if (!latest_scan_generation_ ||
        completion.generation != *latest_scan_generation_) {
      continue;
    }
    snapshot_.scanning = false;
    if (completion.outcome == OperationOutcome::Succeeded) {
      snapshot_.devices = std::move(completion.devices);
      if (startup_scan_alert_pending_ && !snapshot_.devices.empty() &&
          std::ranges::all_of(snapshot_.devices, [](const auto &device) {
            return device.permission.access ==
                   serial::DeviceAccess::PermissionDenied;
          })) {
        publish_permission_alert(snapshot_.devices.front().path,
                                 snapshot_.devices.front().permission);
      }
      set_notice("Found " + std::to_string(snapshot_.devices.size()) +
                     " serial device(s)",
                 false);
    } else if (completion.error) {
      set_notice(error_text(*completion.error));
    }
    startup_scan_alert_pending_ = false;
  }
}

LogContext Application::log_context() const noexcept {
  return {connection_, snapshot_.config, processed_cleanup_,
          snapshot_.shutting_down, operations_allowed()};
}
void Application::synchronize_log_state() {
  snapshot_.log = logs_->state();
  snapshot_.log_pending = logs_->pending_records();
  if (const auto fatal = logs_->fatal_signal()) {
    enter_fatal_stopping(*fatal);
    return;
  }
  if (auto notice = logs_->take_notice()) {
    set_notice(std::move(notice->message), notice->error);
  }
}
bool Application::log_rollover_pending() const noexcept {
  return logs_->log_rollover_pending(log_context());
}
void Application::begin_log_session() {
  logs_->begin_log_session(log_context());
  synchronize_log_state();
}
void Application::start_log_rollover_if_needed() {
  logs_->start_log_rollover_if_needed(log_context());
  synchronize_log_state();
}
void Application::process_log_commands() {
  logs_->process_log_commands(log_context());
  synchronize_log_state();
  if (operations_allowed() && !snapshot_.shutting_down && connect_after_log_ &&
      !logs_->command_pending() &&
      connection_.state() == ConnectionState::Disconnected) {
    connect_after_log_ = false;
    start_connection();
  }
}
void Application::reconfigure_log_writer_if_inactive() {
  logs_->reconfigure_log_writer_if_inactive(log_context());
  synchronize_log_state();
}
void Application::toggle_log() {
  logs_->toggle_log(log_context());
  synchronize_log_state();
}
void Application::end_log_session(ConnectionGeneration generation,
                                  SessionId session_id) {
  logs_->end_log_session(log_context(), generation, session_id);
  synchronize_log_state();
}
void Application::capture_log_session() {
  logging::Header header;
  header.started_at = records_.started_at_utc();
  header.session_id = std::to_string(connection_.session_id()->value);
  for (const char value : device_path_) {
    header.device.path.push_back(static_cast<std::byte>(value));
  }
  const auto &port = *connected_port_config_;
  header.serial = {static_cast<std::uint32_t>(port.baud),
                   static_cast<std::uint8_t>(port.data_bits),
                   to_log_parity(port.parity),
                   static_cast<std::uint8_t>(port.stop_bits),
                   to_log_flow(port.flow_control)};
  logs_->session_opened({*connection_.generation(), *connection_.session_id()},
                        std::move(header));
}

void Application::update_task_projection() {
  const auto current = serial_->task_snapshot();
  snapshot_.task = current.task;
  snapshot_.tx_pending = serial_->pending_tx_requests();
  if (current.notice_revision != task_notice_revision_) {
    task_notice_revision_ = current.notice_revision;
    switch (current.notice) {
    case serial::TaskNotice::AdmissionRejected:
      set_notice(
          "Quick-send TX could not reserve queue or completion capacity");
      break;
    case serial::TaskNotice::DeadlineOverflow:
      set_notice("Quick-send stopped after deadline overflow");
      break;
    case serial::TaskNotice::SequenceOverflow:
      set_notice("Quick-send stopped after sequence exhaustion");
      break;
    case serial::TaskNotice::None:
      break;
    }
  }
}

void Application::tick() {
  if (stopped_) {
    return;
  }
  serial_->acknowledge_ui_wakeup();
  scanner_->acknowledge_ui_wakeup();
  update_worker_state();
  if (!operations_allowed()) {
    return;
  }
  const auto idle_observed_at = std::chrono::steady_clock::now();
  const bool drained = process_serial_data();
  process_serial_completions();
  if (!operations_allowed()) {
    return;
  }
  process_scan_completions();
  // Only an actually empty drain permits idle, never a short/full event batch.
  // Use the pre-drain cutoff so time spent processing cannot manufacture idle.
  if (drained && serial_->queued_data_bytes() == 0U && records_.active() &&
      connection_.state() == ConnectionState::Connected) {
    enqueue_frames(records_.on_idle(idle_observed_at),
                   SessionEventOrigin::Normal);
  }
  process_log_commands();
  reconfigure_log_writer_if_inactive();
  if (!operations_allowed()) {
    return;
  }
  process_save_completions();
  update_task_projection();
  update_worker_state();
  snapshot_.log_pending = logs_->pending_records();
  snapshot_.rx_ingress_bytes = serial_->queued_data_bytes();
  if (notice_deadline_ &&
      std::chrono::steady_clock::now() >= *notice_deadline_) {
    set_notice({}, false);
  }
}

void Application::update_worker_state() {
  logs_->observe_worker();
  synchronize_log_state();
  if (!operations_allowed()) {
    return;
  }
  if (const auto fatal = serial_->fatal_signal()) {
    enter_fatal_stopping(*fatal);
    return;
  }
  if (const auto fatal = scanner_->fatal_signal()) {
    enter_fatal_stopping(*fatal);
    return;
  }
  if (const auto overflow = serial_->overflow_signal();
      overflow && connection_.session_id() == overflow->session_id) {
    set_notice("RX ingress overflow; session is being closed");
  }
}

void Application::enter_fatal_stopping(const FatalSignal &) noexcept {
  if (snapshot_.fatal_stopping || std::this_thread::get_id() != main_thread_) {
    return;
  }
  snapshot_.fatal_stopping = true;
  snapshot_.shutting_down = true;
  serial_->request_stop();
  scanner_->request_stop();
  settings_->request_stop();
  logs_->request_stop(
      std::chrono::milliseconds{snapshot_.config.timeouts.log_barrier_ms});
  // Fatal coordination must not allocate even when the triggering failure is
  // OOM.
  notice_deadline_.reset();
  snapshot_.notice.swap(fatal_notice_);
  snapshot_.notice_error = true;
  if (snapshot_.notice_revision != std::numeric_limits<std::uint64_t>::max()) {
    ++snapshot_.notice_revision;
  }
}

void Application::toggle_pause() noexcept {
  if (!operations_allowed()) {
    return;
  }
  manual_pause_ = !manual_pause_;
  snapshot_.manual_pause = manual_pause_;
}

void Application::clear_records() {
  if (!operations_allowed()) {
    set_notice("Application is stopping; clear rejected");
    return;
  }
  records_.clear();
  synchronize_records();
  set_notice("In-memory session records cleared; log files were not changed",
             false);
}

Status Application::apply_port(const std::string_view value) {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  if (connection_.state() != ConnectionState::Disconnected) {
    return tl::unexpected(
        application_error("hardware is locked while connected"));
  }
  if (value.empty() || value.front() != '/') {
    return tl::unexpected(application_error("device path must be absolute"));
  }
  if (snapshot_.scanning) {
    return tl::unexpected(
        application_error("wait for the current device scan to finish"));
  }
  const auto selected =
      std::ranges::find(snapshot_.devices, value, &serial::DeviceInfo::path);
  if (selected == snapshot_.devices.end()) {
    return tl::unexpected(
        application_error("device must come from the latest completed scan"));
  }
  auto path = serial::inspect_device_path(value);
  if (!path) {
    return tl::unexpected(std::move(path.error()));
  }
  auto permission = serial::inspect_device_permission(path->canonical);
  if (!permission) {
    return tl::unexpected(std::move(permission.error()));
  }
  if (permission->access == serial::DeviceAccess::PermissionDenied) {
    publish_permission_alert(path->canonical, *permission);
    return tl::unexpected(
        make_error(ErrorCode::SerialPermissionDenied, Operation::OpenSerial,
                   "selected serial device is not accessible"));
  }
  device_path_ = std::string{value};
  snapshot_.device_path = lazycom::sanitize_message(device_path_);
  set_notice("Serial device selected", false);
  return {};
}

Status Application::apply_baud(const std::int32_t value) {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  if (connection_.state() != ConnectionState::Disconnected) {
    return tl::unexpected(
        application_error("hardware is locked while connected"));
  }
  if (value < 1) {
    return tl::unexpected(
        application_error("baud must be one of the supported presets"));
  }
  auto candidate = snapshot_.config;
  candidate.serial.baud = value;
  if (const auto errors = config::validate_config_snapshot(candidate);
      !errors.empty()) {
    return tl::unexpected(application_error(errors.front().message));
  }
  snapshot_.config = candidate;
  return save_config();
}

Status Application::apply_data_format(const DataFormatCandidate &value) {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  if (connection_.state() != ConnectionState::Disconnected) {
    return tl::unexpected(
        application_error("hardware is locked while connected"));
  }
  auto candidate = snapshot_.config;
  candidate.serial.data_bits = value.data_bits;
  candidate.serial.stop_bits = value.stop_bits;
  candidate.serial.parity = value.parity;
  candidate.serial.flow_control = value.flow_control;
  if (const auto errors = config::validate_config_snapshot(candidate);
      !errors.empty()) {
    return tl::unexpected(application_error(errors.front().message));
  }
  snapshot_.config = candidate;
  return save_config();
}

Status Application::apply_newline(const config::Newline value) {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  if (value == config::Newline::Session || config::to_string(value).empty()) {
    return tl::unexpected(application_error("newline must be none/lf/cr/crlf"));
  }
  snapshot_.config.send.newline = value;
  return save_config();
}

Status Application::apply_view(const ViewCandidate &value) {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  if (config::to_string(value.rx_view).empty() ||
      config::to_string(value.tx_view).empty()) {
    return tl::unexpected(
        application_error("RX/TX view must be txt/hex/mixed"));
  }
  auto candidate = snapshot_.config;
  candidate.receive.rx_view = value.rx_view;
  candidate.receive.tx_view = value.tx_view;
  const auto &filter = value.filter;
  if (!filter.rx && !filter.tx && !filter.system && !filter.error) {
    return tl::unexpected(
        application_error("at least one receive direction must remain on"));
  }
  snapshot_.config = std::move(candidate);
  snapshot_.filter = filter;
  snapshot_.preferences.show_rx = filter.rx;
  snapshot_.preferences.show_tx = filter.tx;
  snapshot_.preferences.show_system = filter.system;
  snapshot_.preferences.show_error = filter.error;
  auto saved = save_config();
  save_state();
  return saved;
}

Status Application::apply_send_mode(const config::SendMode value) {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  if (config::to_string(value).empty()) {
    return tl::unexpected(application_error("send mode must be txt or hex"));
  }
  snapshot_.config.send.mode = value;
  return save_config();
}

Status Application::apply_logging(const LogSettingsCandidate &value,
                                  const LogApplyPolicy policy) {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  auto candidate = logging_candidate(snapshot_.config, value);
  if (!candidate) {
    return tl::unexpected(candidate.error());
  }
  snapshot_.config = *candidate;
  snapshot_.effective_log_directory = snapshot_.config.logging.directory.empty()
                                          ? default_log_directory_.string()
                                          : snapshot_.config.logging.directory;
  auto saved = save_config();
  const bool rotates_active_file =
      logs_->apply_settings(log_context(), policy == LogApplyPolicy::RotateNow);
  synchronize_log_state();
  if (saved) {
    set_notice(
        rotates_active_file
            ? "Logging settings applied; save pending; recording will rotate"
            : "Logging settings applied for the next recording; save pending",
        false);
  }
  return saved;
}

Status Application::validate_logging(const LogSettingsCandidate &value) const {
  auto candidate = logging_candidate(snapshot_.config, value);
  if (!candidate) {
    return tl::unexpected(candidate.error());
  }
  return {};
}

Status
Application::apply_quick_slot(const std::uint32_t index,
                              std::optional<config::QuickSendSlot> slot) {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  if (index < 1U || index > 20U || (slot && slot->index != index)) {
    return tl::unexpected(application_error("invalid quick-send slot"));
  }
  if (settings_->quick_save_pending()) {
    return tl::unexpected(
        application_error("quick-send save is busy", Operation::SaveConfig));
  }
  auto candidate = snapshot_.quick_send;
  candidate.slots[index - 1U] = std::move(slot);
  if (const auto errors = config::validate_quick_send_snapshot(candidate);
      !errors.empty()) {
    return tl::unexpected(application_error(errors.front().message));
  }
  if (auto saved = settings_->save_quick_send(std::move(candidate)); !saved) {
    return saved;
  }
  snapshot_.quick_send_save_pending = true;
  snapshot_.quick_send_save_failed = false;
  set_notice("Saving quick-send slot...", false);
  return {};
}

Status Application::execute_quick(const std::uint32_t slot,
                                  const std::uint64_t interval_ms,
                                  const bool replacement_confirmed) {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  if (connection_.state() != ConnectionState::Connected) {
    return tl::unexpected(
        application_error("quick send requires a connection"));
  }
  auto execution = scheduler::make_quick_send_execution(
      snapshot_.quick_send, slot, snapshot_.config.send.newline);
  if (!execution || !execution.execution) {
    return tl::unexpected(
        application_error("quick-send slot is empty or invalid"));
  }
  if (pending_task_start_ || pending_task_stop_) {
    return tl::unexpected(application_error("a task command is still pending"));
  }
  const auto current = serial_->task_snapshot().task;
  if (current.state == scheduler::SchedulerState::Running &&
      !replacement_confirmed) {
    return tl::unexpected(
        application_error("replacing the active task requires confirmation"));
  }
  if (current.state == scheduler::SchedulerState::Stopping ||
      !scheduler::valid_interval(interval_ms)) {
    return tl::unexpected(application_error("invalid interval or task state"));
  }
  const auto operation = issue_operation();
  if (operation.value == 0U) {
    return operation_rejected();
  }
  auto submitted =
      serial_->submit_task({operation,
                            *connection_.generation(),
                            *connection_.session_id(),
                            current.generation,
                            {std::move(*execution.execution), interval_ms}});
  if (!submitted) {
    return tl::unexpected(std::move(submitted.error()));
  }
  pending_task_start_ = PendingTaskStart{operation, slot, interval_ms};
  set_notice("Quick-send task command accepted", false);
  return {};
}

void Application::stop_quick_task() {
  if (!operations_allowed()) {
    set_notice("Application is stopping; task stop rejected");
    return;
  }
  if (pending_task_stop_) {
    set_notice("A quick-send stop is already pending", false);
    return;
  }
  const auto current = serial_->task_snapshot().task;
  if (!current.generation && !pending_task_start_) {
    return;
  }
  const auto operation = issue_operation();
  if (operation.value == 0U) {
    return;
  }
  const auto submitted = serial_->request_stop_task(
      {operation, current.generation,
       pending_task_start_ ? std::optional{pending_task_start_->operation_id}
                           : std::nullopt});
  if (!submitted) {
    set_notice(error_text(submitted.error()));
    return;
  }
  if (*submitted == serial::SubmitStatus::Accepted) {
    pending_task_stop_ = operation;
    set_notice("Stopping quick-send task at the serial owner boundary...",
               false);
  }
}

Status Application::save_config() {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  auto saved = settings_->save_config(snapshot_.config);
  if (!saved) {
    set_notice("Settings applied for this process, but not saved: " +
               error_text(saved.error()));
    return tl::unexpected(std::move(saved.error()));
  }
  set_notice(*saved == SaveAdmission::Queued
                 ? "Settings applied; latest configuration save queued"
                 : "Settings applied; configuration save pending",
             false);
  return {};
}

void Application::save_state() {
  if (!operations_allowed()) {
    return;
  }
  if (auto saved = settings_->save_state(snapshot_.preferences); !saved) {
    set_notice(error_text(saved.error()));
  }
}

void Application::process_save_completions() {
  auto results = settings_->poll();
  snapshot_.quick_send_save_pending = settings_->quick_save_pending();
  snapshot_.quick_send_save_failed = settings_->quick_save_failed();
  for (auto &pending : results.completions) {
    if (!pending) {
      continue;
    }
    auto &completion = *pending;
    const bool superseded = completion.superseded;
    const std::string file =
        completion.file == config::PersistenceFile::Config ? "config.toml"
        : completion.file == config::PersistenceFile::QuickSend
            ? "quick_send.toml"
            : "state.toml";
    if (completion.outcome.state == config::CommitState::NotCommitted) {
      set_notice((superseded ? "Earlier " : "") + file +
                 " snapshot was not saved" +
                 (completion.outcome.error
                      ? ": " + error_text(*completion.outcome.error)
                      : ""));
      continue;
    }
    if (completion.committed_quick_send) {
      snapshot_.quick_send = std::move(*completion.committed_quick_send);
    }
    if (completion.outcome.state ==
        config::CommitState::CommittedDurabilityUnknown) {
      set_notice((superseded ? "Earlier " : "") + file +
                 " snapshot committed, but directory durability is unknown");
    } else if (!superseded) {
      set_notice(file + " saved", false);
    }
  }
  if (results.fatal) {
    enter_fatal_stopping(*results.fatal);
    return;
  }
  if (settings_->config_retry_pending()) {
    static_cast<void>(save_config());
  }
  if (settings_->state_retry_pending()) {
    save_state();
  }
}

std::string Application::render_record(const VisibleRecord &record) const {
  return project_record(record, snapshot_.config.receive);
}
std::vector<std::uint64_t> Application::search(std::string_view query,
                                               DirectionFilter filter) const {
  return records_.search(query, filter, snapshot_.config.receive);
}

bool Application::has_exit_risk() const noexcept {
  return connection_.state() != ConnectionState::Disconnected ||
         logs_->state() != LogState::Off ||
         (serial_->task_snapshot().task.state !=
              scheduler::SchedulerState::Idle ||
          pending_task_start_ || pending_task_stop_) ||
         !snapshot_.draft.empty();
}

bool Application::shutdown() noexcept {
  if (stopped_) {
    return !snapshot_.fatal_stopping;
  }
  snapshot_.shutting_down = true;
  connect_after_log_ = false;
  const auto owner_timeout =
      std::chrono::milliseconds{snapshot_.config.timeouts.owner_stop_ms};
  const auto log_timeout =
      std::chrono::milliseconds{snapshot_.config.timeouts.log_barrier_ms};
  try {
    update_worker_state();
    const bool fatal = snapshot_.fatal_stopping;
    if (fatal) {
      serial_->request_stop();
      scanner_->request_stop();
      settings_->request_stop();
      logs_->request_stop(log_timeout);
      if (std::this_thread::get_id() != main_thread_) {
        return false;
      }
      const auto deadline = std::chrono::steady_clock::now() + owner_timeout;
      const auto log_deadline = logs_->stop_deadline().value_or(
          std::chrono::steady_clock::now() + log_timeout);
      const bool serial_stopped = serial_->wait_until_stopped(deadline);
      const bool scanner_stopped = scanner_->wait_until_stopped(deadline);
      const bool persistence_stopped = settings_->wait_until_stopped(deadline);
      const bool log_stopped = logs_->wait_until_stopped(log_deadline);
      if (!serial_stopped || !scanner_stopped || !persistence_stopped ||
          !log_stopped) {
        abort_after_shutdown_timeout();
      }

      settings_->shutdown();
      stopped_ = true;
      return false;
    }
    stop_quick_task();
    const auto drain_log_commands = [this, log_timeout] {
      const auto deadline = std::chrono::steady_clock::now() + log_timeout;
      while (logs_->command_pending() || log_rollover_pending()) {
        if (!logs_->command_pending()) {
          start_log_rollover_if_needed();
          reconfigure_log_writer_if_inactive();
          if (!operations_allowed()) {
            return;
          }
          if (!logs_->command_pending()) {
            if (std::chrono::steady_clock::now() >= deadline) {
              abort_after_shutdown_timeout();
            }
            std::this_thread::sleep_for(1ms);
            continue;
          }
        }
        if (!logs_->wait_command_until(deadline)) {
          abort_after_shutdown_timeout();
        }
        process_log_commands();
        if (!operations_allowed()) {
          return;
        }
      }
    };
    drain_log_commands();
    if (!operations_allowed()) {
      return shutdown();
    }
    if (connection_.state() == ConnectionState::Connecting) {
      request_cancel_connect();
    } else if (connection_.state() == ConnectionState::Connected ||
               connection_.state() == ConnectionState::Error) {
      request_disconnect();
    }
    if (connection_.state() != ConnectionState::Disconnected) {
      const auto owner_deadline =
          std::chrono::steady_clock::now() + owner_timeout;
      const auto deadline = owner_deadline + log_timeout;
      while (connection_.state() != ConnectionState::Disconnected &&
             std::chrono::steady_clock::now() < deadline) {
        process_serial_data();
        process_serial_completions();
        if (processed_cleanup_ && !logs_->command_pending()) {
          end_log_session(processed_cleanup_->generation,
                          processed_cleanup_->session_id);
        }
        process_log_commands();
        if (!operations_allowed()) {
          return shutdown();
        }
        if (logs_->stop_deadline() &&
            std::chrono::steady_clock::now() >= *logs_->stop_deadline() &&
            !logs_->wait_until_stopped(std::chrono::steady_clock::now())) {
          abort_after_shutdown_timeout();
        }
        if (!processed_cleanup_ &&
            connection_.state() != ConnectionState::Disconnected &&
            std::chrono::steady_clock::now() >= owner_deadline) {
          abort_after_shutdown_timeout();
        }
        std::this_thread::sleep_for(5ms);
      }
      if (connection_.state() != ConnectionState::Disconnected) {
        abort_after_shutdown_timeout();
      }
    }
    drain_log_commands();
    if (!operations_allowed()) {
      return shutdown();
    }
    logs_->finish_writer(records_.last_sequence(), log_timeout);
    if (const auto fatal_signal = logs_->fatal_signal()) {
      enter_fatal_stopping(*fatal_signal);
      return shutdown();
    }

    scanner_->request_stop();
    if (!scanner_->wait_until_stopped(std::chrono::steady_clock::now() +
                                      owner_timeout)) {
      abort_after_shutdown_timeout();
    }
    serial_->request_stop();
    if (!serial_->wait_until_stopped(std::chrono::steady_clock::now() +
                                     owner_timeout)) {
      abort_after_shutdown_timeout();
    }
    const auto persistence_deadline =
        std::chrono::steady_clock::now() + owner_timeout;
    while (settings_->pending()) {
      const auto waited = settings_->wait_next(persistence_deadline);
      if (waited == SaveWait::TimedOut) {
        abort_after_shutdown_timeout();
      }
      process_save_completions();
      if (!operations_allowed()) {
        return shutdown();
      }
      if (waited == SaveWait::NothingPending &&
          std::chrono::steady_clock::now() >= persistence_deadline) {
        abort_after_shutdown_timeout();
      }
    }
    settings_->request_stop();
    if (!settings_->wait_until_stopped(std::chrono::steady_clock::now() +
                                       owner_timeout)) {
      abort_after_shutdown_timeout();
    }
    settings_->shutdown();

    stopped_ = true;
    return true;
  } catch (...) {
    abort_after_shutdown_timeout();
  }
}

} // namespace lazycom::app
