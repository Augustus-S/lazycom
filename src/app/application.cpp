#include <lazycom/app/application.hpp>

#include <lazycom/diagnostics/diagnostics.hpp>
#include <lazycom/logging/schema.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lazycom::app {
namespace {

using namespace std::chrono_literals;

[[noreturn]] void abort_after_shutdown_timeout() noexcept {
  diagnostics::emergency_write();
  std::abort();
}

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

[[nodiscard]] std::string utc_now() {
  const auto now = std::chrono::system_clock::now();
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now.time_since_epoch()) %
      1000;
  const std::time_t raw = std::chrono::system_clock::to_time_t(now);
  std::tm value{};
  if (::gmtime_r(&raw, &value) == nullptr) {
    return "1970-01-01T00:00:00.000Z";
  }
  std::array<char, 32> output{};
  const auto length =
      std::strftime(output.data(), output.size(), "%Y-%m-%dT%H:%M:%S", &value);
  if (length == 0U) {
    return "1970-01-01T00:00:00.000Z";
  }
  return std::string{output.data(), length} + "." +
         (milliseconds.count() < 100 ? "0" : "") +
         (milliseconds.count() < 10 ? "0" : "") +
         std::to_string(milliseconds.count()) + "Z";
}

[[nodiscard]] std::vector<std::string_view> split(std::string_view value,
                                                  char delimiter) {
  std::vector<std::string_view> fields;
  while (true) {
    const auto position = value.find(delimiter);
    fields.push_back(value.substr(0U, position));
    if (position == std::string_view::npos) {
      return fields;
    }
    value.remove_prefix(position + 1U);
  }
}

template <class Integer>
[[nodiscard]] std::optional<Integer> parse_integer(std::string_view value) {
  Integer result{};
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] std::optional<config::Parity>
parse_parity(std::string_view value) {
  if (value == "none") {
    return config::Parity::None;
  }
  if (value == "odd") {
    return config::Parity::Odd;
  }
  if (value == "even") {
    return config::Parity::Even;
  }
  if (value == "mark") {
    return config::Parity::Mark;
  }
  if (value == "space") {
    return config::Parity::Space;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<config::FlowControl>
parse_flow(std::string_view value) {
  if (value == "none") {
    return config::FlowControl::None;
  }
  if (value == "rtscts") {
    return config::FlowControl::RtsCts;
  }
  if (value == "xonxoff") {
    return config::FlowControl::XonXoff;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<config::Newline>
parse_newline(std::string_view value, bool allow_session = false) {
  if (value == "none") {
    return config::Newline::None;
  }
  if (value == "lf") {
    return config::Newline::Lf;
  }
  if (value == "cr") {
    return config::Newline::Cr;
  }
  if (value == "crlf") {
    return config::Newline::CrLf;
  }
  if (allow_session && value == "session") {
    return config::Newline::Session;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<config::ReceiveView>
parse_receive_view(std::string_view value) {
  if (value == "txt") {
    return config::ReceiveView::Txt;
  }
  if (value == "hex") {
    return config::ReceiveView::Hex;
  }
  if (value == "mixed") {
    return config::ReceiveView::Mixed;
  }
  return std::nullopt;
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

[[nodiscard]] std::string error_text(const Error &error) {
  const auto &descriptor = error_descriptor(error.code);
  std::string result{descriptor.identifier};
  result += ": ";
  result += descriptor.default_message;
  if (!error.detail.empty()) {
    result += " (";
    result += logging::sanitize_message(error.detail);
    result += ")";
  }
  return result;
}

[[nodiscard]] logging::Direction log_direction(RecordDirection direction) {
  switch (direction) {
  case RecordDirection::Rx:
    return logging::Direction::Rx;
  case RecordDirection::Tx:
    return logging::Direction::Tx;
  case RecordDirection::System:
    return logging::Direction::Sys;
  case RecordDirection::Error:
    return logging::Direction::Err;
  }
  return logging::Direction::Err;
}

[[nodiscard]] logging::SessionWriterOptions
writer_options(const config::ConfigSnapshot &snapshot,
               const std::filesystem::path &default_directory) {
  const auto &settings = snapshot.logging;
  logging::SessionWriterOptions options;
  options.directory = settings.directory.empty()
                          ? default_directory
                          : std::filesystem::path{settings.directory};
  options.quotas = {
      settings.max_files,
      static_cast<std::uint64_t>(settings.max_total_size_mib) * 1024U * 1024U,
      static_cast<std::uint64_t>(settings.max_file_size_mib) * 1024U * 1024U};
  options.queue_max_records = snapshot.queues.log_max_messages;
  options.queue_max_bytes =
      static_cast<std::size_t>(snapshot.queues.log_max_mib) * 1024U * 1024U;
  options.flush_interval =
      std::chrono::milliseconds{settings.flush_interval_ms};
  options.include_system = settings.include_system;
  options.include_error = settings.include_error;
  return options;
}

[[nodiscard]] std::size_t
log_record_bytes(const logging::Record &record) noexcept {
  std::size_t result = sizeof(logging::Record) + record.time_utc.capacity() +
                       record.payload.capacity() + record.message.capacity();
  if (record.code) {
    result += record.code->capacity();
  }
  return result;
}

[[nodiscard]] bool direction_visible(const RecordDirection direction,
                                     const DirectionFilter &filter) noexcept {
  switch (direction) {
  case RecordDirection::Rx:
    return filter.rx;
  case RecordDirection::Tx:
    return filter.tx;
  case RecordDirection::System:
    return filter.system;
  case RecordDirection::Error:
    return filter.error;
  }
  return false;
}

[[nodiscard]] Result<config::ConfigSnapshot>
logging_candidate(const config::ConfigSnapshot &current,
                  const std::string_view value) {
  const auto fields = split(value, '|');
  if (fields.size() != 4U) {
    return tl::unexpected(application_error(
        "logging format is directory|max_files|max_total_mib|max_file_mib"));
  }
  auto candidate = current;
  candidate.logging.directory = std::string{fields[0]};
  const auto files = parse_integer<std::uint32_t>(fields[1]);
  const auto total = parse_integer<std::uint32_t>(fields[2]);
  const auto file = parse_integer<std::uint32_t>(fields[3]);
  if (!files || !total || !file ||
      (!fields[0].empty() && fields[0].front() != '/')) {
    return tl::unexpected(application_error("invalid logging configuration"));
  }
  if (!fields[0].empty()) {
    const std::string path{fields[0]};
    struct stat status{};
    if (::lstat(path.c_str(), &status) != 0) {
      return tl::unexpected(application_error("log directory does not exist"));
    }
    if (S_ISLNK(status.st_mode)) {
      return tl::unexpected(
          application_error("log directory must not be a symbolic link"));
    }
    if (!S_ISDIR(status.st_mode)) {
      return tl::unexpected(application_error("log path is not a directory"));
    }
    if (status.st_uid != ::geteuid()) {
      return tl::unexpected(
          application_error("log directory must be owned by the current user"));
    }
    if (::faccessat(AT_FDCWD, path.c_str(), W_OK | X_OK, AT_EACCESS) != 0) {
      return tl::unexpected(
          application_error("log directory is not writable and searchable"));
    }
  }
  candidate.logging.max_files = *files;
  candidate.logging.max_total_size_mib = *total;
  candidate.logging.max_file_size_mib = *file;
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
    auto writer = std::make_unique<logging::SessionWriter>(
        writer_options(loaded->config.snapshot, loaded->default_log_directory));
    return std::unique_ptr<Application>(new Application(
        std::move(dependencies), std::move(*loaded), std::move(*serial_service),
        std::move(*scanner), std::move(writer)));
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

Application::Application(ApplicationDependencies, LoadedConfiguration loaded,
                         std::unique_ptr<serial::SerialService> serial_service,
                         std::unique_ptr<serial::DeviceScanner> scanner,
                         std::unique_ptr<logging::SessionWriter> writer)
    : serial_(std::move(serial_service)), scanner_(std::move(scanner)),
      writer_(std::move(writer)), paths_(std::move(loaded.paths)),
      config_load_(std::move(loaded.config)),
      quick_load_(std::move(loaded.quick)),
      state_load_(std::move(loaded.state)),
      default_log_directory_(std::move(loaded.default_log_directory)) {
  snapshot_.config = config_load_.snapshot;
  snapshot_.quick_send = quick_load_.snapshot;
  snapshot_.preferences = state_load_.snapshot;
  snapshot_.filter = {
      snapshot_.preferences.show_rx, snapshot_.preferences.show_tx,
      snapshot_.preferences.show_system, snapshot_.preferences.show_error};
  snapshot_.config_read_only = config_load_.read_only;
  snapshot_.quick_send_read_only = quick_load_.read_only;
  snapshot_.state_read_only = state_load_.read_only;
  snapshot_.configuration_notice = std::move(loaded.notice);
  snapshot_.effective_log_directory = snapshot_.config.logging.directory.empty()
                                          ? default_log_directory_.string()
                                          : snapshot_.config.logging.directory;
  persistence_ = std::make_unique<config::PersistenceWorker>(paths_);
  static_cast<void>(workers_.mark_running(WorkerKind::Serial));
  static_cast<void>(workers_.mark_running(WorkerKind::Scanner));
  static_cast<void>(workers_.mark_running(WorkerKind::SessionLog));
  static_cast<void>(workers_.mark_running(WorkerKind::Persistence));
  if (snapshot_.config.logging.default_enabled && writer_->enable()) {
    static_cast<void>(log_state_.enable(false));
  }
  snapshot_.log = log_state_.state();
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
  if (operation_ids_.issue(result) != IdIncrementResult::Advanced) {
    throw std::overflow_error("operation identifier exhausted");
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

void Application::set_notice(std::string message) {
  snapshot_.notice = logging::sanitize_message(message);
}

bool Application::operations_allowed() const noexcept {
  return !stopped_ && fatal_guard_.state() == ProcessLifecycle::Running;
}

Status Application::operation_rejected() const {
  return tl::unexpected(
      application_error("application is stopping after a fatal worker failure",
                        Operation::CoordinateFatal));
}

void Application::publish_permission_alert(
    const std::string_view path, const serial::DevicePermission &permission) {
  std::ostringstream message;
  message << "Cannot open " << logging::sanitize_message(path) << ".\n\n"
          << "Effective UID: " << static_cast<std::uint64_t>(::geteuid())
          << "\n"
          << "Owner UID: " << permission.owner_uid << "\n"
          << "Group: " << logging::sanitize_message(permission.group_name)
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
  alert.title = logging::sanitize_message(alert.title);
  alert.message = logging::sanitize_message(alert.message);
  snapshot_.alert = std::move(alert);
}

void Application::request_scan() {
  if (!operations_allowed()) {
    set_notice("Application is stopping; device scan rejected");
    return;
  }
  try {
    const auto operation = issue_operation();
    const auto generation = issue_scan_generation();
    const auto submitted = scanner_->submit_scan(operation, generation);
    if (!submitted) {
      set_notice(error_text(submitted.error()));
      return;
    }
    latest_scan_generation_ = generation;
    snapshot_.scanning = true;
    set_notice("Scanning serial devices...");
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
  snapshot_.device_path = logging::sanitize_message(device_path_);
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
    set_notice("Connecting to serial device...");
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
    auto submitted = serial_->request_disconnect(command);
    if (!submitted) {
      set_notice(error_text(submitted.error()));
      return;
    }
    if (*submitted == serial::SubmitStatus::AlreadyPending) {
      set_notice("A disconnect request is already pending");
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
    set_notice("Disconnecting serial session...");
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
    auto submitted = serial_->request_cancel_connect(command);
    if (!submitted) {
      const auto owner = serial_->connection_snapshot();
      if (owner.connected && owner.generation == command.generation) {
        // The connect committed at the owner between the UI snapshot and this
        // submission. Keep local state unchanged until its completion installs
        // the session, then submit a real disconnect for the user's intent.
        cancel_race_disconnect_pending_ = true;
        set_notice("Connection completed while cancellation raced; waiting for "
                   "its session boundary...");
        return;
      }
      set_notice(error_text(submitted.error()));
      return;
    }
    if (*submitted == serial::SubmitStatus::AlreadyPending) {
      set_notice("A connection cancellation is already pending");
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
    set_notice("Cancelling connection...");
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
  if (log_command_) {
    connect_after_log_ = true;
    set_notice("Finishing the previous session log before reconnecting...");
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
      set_notice("Pending connection cancelled");
    } else {
      set_notice("Disconnect requires an active link");
    }
    break;
  case ConnectionState::Disconnecting:
    set_notice("Disconnect is already in progress");
    break;
  }
}

void Application::connection_control() {
  if (connection_.state() == ConnectionState::Disconnected) {
    if (connect_after_log_) {
      connect_after_log_ = false;
      set_notice("Pending connection cancelled");
    } else {
      connect();
    }
  } else {
    disconnect();
  }
}

void Application::set_interaction(const InteractionState state) noexcept {
  static_cast<void>(interaction_.enter(state));
  snapshot_.interaction = interaction_.state();
}

void Application::set_draft(std::string draft) {
  if (!operations_allowed()) {
    set_notice("Application is stopping; draft edit rejected");
    return;
  }
  if (draft.size() > snapshot_.config.send.max_draft_bytes) {
    draft.resize(snapshot_.config.send.max_draft_bytes);
    set_notice("Draft reached its configured byte limit");
  }
  snapshot_.draft = std::move(draft);
  history_index_.reset();
}

bool Application::submit_tx(
    std::vector<std::byte> payload,
    const std::optional<TaskGeneration> task_generation,
    const std::optional<scheduler::ScheduledRequestToken> scheduled_token) {
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
  try {
    const auto operation = issue_operation();
    const SendCommand command{operation, *connection_.generation(),
                              *connection_.session_id(), task_generation};
    if (!connection_.accepts(command)) {
      set_notice("TX rejected by the active session guard");
      return false;
    }
    auto submitted = serial_->submit_tx({command, std::move(payload)});
    if (!submitted) {
      set_notice(error_text(submitted.error()));
      return false;
    }
    if (scheduled_token) {
      scheduled_operations_.emplace(operation.value, *scheduled_token);
    }
    ++active_tx_count_;
    snapshot_.tx_pending = active_tx_count_;
    return true;
  } catch (const std::exception &exception) {
    set_notice(exception.what());
    return false;
  }
}

void Application::submit_draft() {
  if (!operations_allowed()) {
    set_notice("Application is stopping; TX rejected");
    return;
  }
  if (connection_.state() != ConnectionState::Connected) {
    set_notice("Not connected; draft was kept");
    return;
  }
  auto parsed =
      scheduler::parse_payload(snapshot_.config.send.mode, snapshot_.draft,
                               snapshot_.config.send.newline);
  if (!parsed) {
    set_notice("Draft is not valid for the selected TXT/HEX mode");
    return;
  }
  if (!submit_tx(std::move(parsed.bytes), std::nullopt)) {
    return;
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
  set_notice("TX accepted by serial owner");
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

void Application::enqueue_frames(std::vector<framing::RxFrame> frames,
                                 const SessionEventOrigin origin) {
  for (auto &frame : frames) {
    snapshot_.rx_bytes += frame.bytes.size();
    enqueue_record(RecordDirection::Rx, frame.bytes, {}, std::nullopt,
                   std::nullopt, origin);
  }
}

void Application::enqueue_record(const RecordDirection direction,
                                 const std::span<const std::byte> payload,
                                 std::string message,
                                 const std::optional<ErrorCode> error_code,
                                 const std::optional<OperationId> operation,
                                 const SessionEventOrigin origin) {
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
  if (next_record_id_ == std::numeric_limits<std::uint64_t>::max() ||
      next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    set_notice("Session record identifier exhausted");
    return;
  }
  VisibleRecord visible;
  visible.record_id = next_record_id_++;
  visible.sequence = next_sequence_++;
  visible.direction = direction;
  visible.time_utc = utc_now();
  visible.payload.assign(payload.begin(), payload.end());
  visible.message = logging::sanitize_message(message);
  visible.error_code = error_code;
  visible.operation_id = operation;

  const std::size_t logical_bytes = sizeof(VisibleRecord) +
                                    visible.payload.capacity() +
                                    visible.message.capacity();
  const auto maximum_bytes =
      static_cast<std::size_t>(snapshot_.config.receive.visible_buffer_mib) *
      1024U * 1024U;
  while (!snapshot_.records.empty() &&
         (snapshot_.records.size() >=
              snapshot_.config.receive.visible_max_records ||
          snapshot_.display_bytes >
              maximum_bytes - std::min(maximum_bytes, logical_bytes))) {
    const auto &front = snapshot_.records.front();
    snapshot_.display_bytes -= sizeof(VisibleRecord) +
                               front.payload.capacity() +
                               front.message.capacity();
    snapshot_.records.pop_front();
    ++snapshot_.display_gap_records;
  }
  snapshot_.display_bytes += logical_bytes;
  snapshot_.records.push_back(visible);

  logging::Record log_record;
  log_record.seq = visible.sequence;
  log_record.time_utc = visible.time_utc;
  log_record.elapsed_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - session_started_)
          .count());
  log_record.direction = log_direction(direction);
  log_record.payload = visible.payload;
  log_record.message = visible.message;
  if (direction == RecordDirection::Tx) {
    log_record.input_mode = snapshot_.config.send.mode == config::SendMode::Txt
                                ? logging::InputMode::Text
                                : logging::InputMode::Hex;
  }
  if (direction == RecordDirection::Error) {
    log_record.code =
        std::string{error_descriptor(
                        error_code.value_or(ErrorCode::InternalInvariantBroken))
                        .identifier};
  }
  const bool disabling =
      log_command_ && log_command_->kind == PendingLogCommand::Kind::Disable;
  if (log_state_.state() == LogState::Recording && !disabling) {
    const auto result = writer_->try_enqueue({std::move(log_record)});
    if (result != logging::EnqueueResult::Accepted) {
      static_cast<void>(log_state_.fail());
      set_notice("Session log stopped because its queue rejected a record");
    }
  } else if (!disabling && log_state_.state() == LogState::Waiting &&
             connection_.generation() && connection_.session_id() &&
             (connection_.state() == ConnectionState::Connected ||
              connection_.state() == ConnectionState::Disconnecting)) {
    const auto record_bytes = log_record_bytes(log_record);
    const auto log_maximum_bytes =
        static_cast<std::size_t>(snapshot_.config.queues.log_max_mib) * 1024U *
        1024U;
    if (log_backlog_.size() < snapshot_.config.queues.log_max_messages &&
        record_bytes <= log_maximum_bytes -
                            std::min(log_backlog_bytes_, log_maximum_bytes)) {
      log_backlog_bytes_ += record_bytes;
      log_backlog_.push_back(
          {{*connection_.generation(), *connection_.session_id()},
           std::move(log_record)});
    } else {
      static_cast<void>(log_state_.fail());
      log_backlog_.clear();
      log_backlog_bytes_ = 0U;
      restart_log_owner_.reset();
      set_notice("Session log stopped because its rollover backlog is full");
    }
  }
  snapshot_.log = log_state_.state();
  snapshot_.log_pending = writer_->queued_records() + log_backlog_.size();
}

void Application::process_serial_data() {
  // The owner may publish bytes immediately after its connect completion. Keep
  // them queued until the application has installed the matching session.
  if (connection_.state() == ConnectionState::Connecting &&
      !connection_.session_id()) {
    return;
  }
  while (true) {
    auto events = serial_->drain_data(512U);
    if (events.empty()) {
      break;
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
        if (connection_.accepts(guard) && framer_) {
          enqueue_frames(
              framer_->push(event.bytes, std::chrono::steady_clock::now()),
              event.origin);
        }
      } else if (event.kind == serial::SerialDataKind::Tx) {
        const SessionDataEvent guard{event.generation, event.session_id,
                                     event.origin, SessionEventKind::Tx};
        if (connection_.accepts(guard)) {
          snapshot_.tx_bytes += event.bytes.size();
          enqueue_record(RecordDirection::Tx, event.bytes, {}, std::nullopt,
                         event.operation_id, event.origin);
        }
      } else if (event.kind == serial::SerialDataKind::Error) {
        const SessionDataEvent guard{event.generation, event.session_id,
                                     event.origin, SessionEventKind::Error};
        if (event.error && connection_.accepts(guard)) {
          enqueue_record(RecordDirection::Error, {}, error_text(*event.error),
                         event.error->code,
                         event.error->operation_id ? event.error->operation_id
                                                   : event.operation_id,
                         event.origin);
        }
      } else {
        processed_cleanup_ =
            LogSessionOwner{event.generation, event.session_id};
        bool owner_fault_cleanup = false;
        if (connection_.state() == ConnectionState::Connected) {
          owner_fault_cleanup =
              connection_.session_fault({event.generation, event.session_id}) ==
              StateChange::Applied;
        }
        if (framer_) {
          enqueue_frames(framer_->flush(), SessionEventOrigin::Cleanup);
        }
        if (event.error) {
          enqueue_record(RecordDirection::Error, {}, error_text(*event.error),
                         event.error->code, event.error->operation_id,
                         SessionEventOrigin::Cleanup);
        } else {
          enqueue_record(RecordDirection::System, {}, "Serial session closed",
                         std::nullopt, std::nullopt,
                         SessionEventOrigin::Cleanup);
        }
        if (owner_fault_cleanup) {
          const auto invalidated = scheduler_.invalidate_for_disconnect();
          if (invalidated.generation_to_stop) {
            static_cast<void>(
                scheduler_.confirm_stopped(*invalidated.generation_to_stop,
                                           std::chrono::steady_clock::now()));
            snapshot_.task = scheduler_.snapshot();
          }
          try {
            const DisconnectCommand cleanup{issue_operation(), event.generation,
                                            event.session_id};
            if (connection_.begin_disconnect(cleanup) == StateChange::Applied) {
              end_log_session(event.generation, event.session_id);
              static_cast<void>(connection_.disconnect_completed(
                  {cleanup.operation_id, cleanup.generation, cleanup.session_id,
                   OperationOutcome::Failed}));
              framer_.reset();
              connected_port_config_.reset();
              snapshot_.active_port_config.reset();
              set_interaction(InteractionState::Normal);
            }
          } catch (const std::exception &exception) {
            set_notice(exception.what());
          }
        }
      }
    }
    // A disconnect closes the owner before publishing its completion, so a
    // bounded drain is finite and must reach the cleanup marker. Connected RX
    // remains budgeted to one batch per UI tick.
    if (event_count < 512U ||
        connection_.state() == ConnectionState::Connected) {
      break;
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
                              : "Connection was cancelled");
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
    auto submitted = serial_->request_disconnect(command);
    if (!submitted) {
      set_notice(error_text(submitted.error()));
      return;
    }
    if (*submitted == serial::SubmitStatus::AlreadyPending) {
      set_notice("Racing connection cleanup is already pending");
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
    set_notice("Connection completed while cancellation raced; closing it...");
  } catch (const std::exception &exception) {
    set_notice(exception.what());
  }
}

void Application::process_serial_completions() {
  auto completions = serial_->drain_completions();
  const auto log_boundary_ready = [this](const ConnectionGeneration generation,
                                         const SessionId session_id) {
    return !log_command_ || !log_command_->owner ||
           *log_command_->owner != LogSessionOwner{generation, session_id};
  };
  for (std::size_t index = deferred_disconnect_completions_.size(); index != 0U;
       --index) {
    auto value = std::move(deferred_disconnect_completions_.front());
    deferred_disconnect_completions_.pop_front();
    const bool cleanup_ready =
        !value.session_id ||
        (processed_cleanup_ &&
         *processed_cleanup_ ==
             LogSessionOwner{value.generation, *value.session_id} &&
         log_boundary_ready(value.generation, *value.session_id));
    if (cleanup_ready) {
      completions.emplace_back(std::move(value));
    } else {
      deferred_disconnect_completions_.push_back(std::move(value));
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
                session_started_ = std::chrono::steady_clock::now();
                next_sequence_ = 1U;
                framer_ =
                    std::make_unique<framing::RxFramer>(framing::RxFramerConfig{
                        std::chrono::milliseconds{
                            snapshot_.config.receive.idle_gap_ms},
                        snapshot_.config.receive.max_frame_bytes});
                set_interaction(InteractionState::Normal);
                begin_log_session();
                if (connection_.state() == ConnectionState::Connected &&
                    !cancel_race_disconnect_pending_) {
                  set_notice("Serial connection established");
                  enqueue_record(RecordDirection::System, {},
                                 "Serial connection established");
                } else {
                  cancel_race_disconnect_pending_ = true;
                  retry_cancel_race_disconnect();
                }
              }
            } else {
              finish_failed_connection(value);
            }
          } else if constexpr (std::is_same_v<Value, serial::TxCompletion>) {
            if (active_tx_count_ != 0U) {
              --active_tx_count_;
            }
            const auto scheduled =
                scheduled_operations_.find(value.operation_id.value);
            if (scheduled != scheduled_operations_.end()) {
              const auto token = scheduled->second;
              scheduled_operations_.erase(scheduled);
              if (scheduler_.snapshot().state !=
                  scheduler::SchedulerState::Stopping) {
                scheduler::TxBoundary boundary;
                boundary.completed_request = token;
                boundary.completed_successfully =
                    value.outcome == OperationOutcome::Succeeded;
                if (const auto next = scheduler_.on_tx_boundary(
                        std::chrono::steady_clock::now(), boundary)) {
                  static_cast<void>(submit_tx(next->execution->bytes,
                                              next->token.generation,
                                              next->token));
                }
              }
            }
          } else if constexpr (std::is_same_v<Value,
                                              serial::DisconnectCompletion>) {
            if (value.session_id &&
                (!processed_cleanup_ ||
                 *processed_cleanup_ !=
                     LogSessionOwner{value.generation, *value.session_id} ||
                 !log_boundary_ready(value.generation, *value.session_id))) {
              deferred_disconnect_completions_.push_back(std::move(value));
              return;
            }
            const auto changed = connection_.disconnect_completed(
                {value.operation_id, value.generation, value.session_id,
                 value.outcome});
            if (changed == StateChange::Applied) {
              if (value.session_id) {
                end_log_session(value.generation, *value.session_id);
              }
              const auto invalidated = scheduler_.invalidate_for_disconnect();
              if (invalidated.generation_to_stop) {
                static_cast<void>(scheduler_.confirm_stopped(
                    *invalidated.generation_to_stop,
                    std::chrono::steady_clock::now()));
                snapshot_.task = scheduler_.snapshot();
              }
              processed_cleanup_.reset();
              cancel_race_disconnect_pending_ = false;
              framer_.reset();
              connected_port_config_.reset();
              snapshot_.active_port_config.reset();
              set_interaction(InteractionState::Normal);
              set_notice(value.outcome == OperationOutcome::Succeeded
                             ? "Serial session disconnected"
                             : "Serial session closed with an error");
            }
          } else {
            const auto stopped = scheduler_.confirm_stopped(
                value.generation, std::chrono::steady_clock::now());
            if (stopped.status ==
                scheduler::StopConfirmationStatus::ReplacementStarted) {
              set_notice("Old quick-send task stopped; replacement started");
              process_scheduler();
            } else if (stopped.status ==
                       scheduler::StopConfirmationStatus::Stopped) {
              set_notice("Quick-send task stopped");
            } else if (stopped.status ==
                       scheduler::StopConfirmationStatus::GenerationOverflow) {
              set_notice("Quick-send replacement generation was exhausted");
            }
            snapshot_.task = scheduler_.snapshot();
          }
        },
        completion);
  }
  snapshot_.connection = connection_.state();
  snapshot_.tx_pending = active_tx_count_;
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
                 " serial device(s)");
    } else if (completion.error) {
      set_notice(error_text(*completion.error));
    }
    startup_scan_alert_pending_ = false;
  }
}

void Application::begin_log_session() {
  if (log_state_.state() != LogState::Waiting || log_command_ ||
      (connection_.state() != ConnectionState::Connected &&
       connection_.state() != ConnectionState::Disconnecting) ||
      !connection_.generation() || !connection_.session_id() ||
      !connected_port_config_) {
    return;
  }
  const LogSessionOwner owner{*connection_.generation(),
                              *connection_.session_id()};
  logging::Header header;
  header.started_at = utc_now();
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
  log_command_.emplace(PendingLogCommand{
      writer_->start_session(std::move(header)), PendingLogCommand::Kind::Start,
      owner, false,
      std::chrono::steady_clock::now() +
          std::chrono::milliseconds{snapshot_.config.timeouts.log_barrier_ms}});
}

void Application::end_log_session(const ConnectionGeneration generation,
                                  const SessionId session_id) {
  const LogSessionOwner owner{generation, session_id};
  if (log_command_) {
    if (log_command_->kind == PendingLogCommand::Kind::Start &&
        log_command_->owner == owner) {
      log_command_->close_after_start = true;
    }
    return;
  }
  if (log_state_.state() == LogState::Recording) {
    log_command_.emplace(PendingLogCommand{
        writer_->end_session(), PendingLogCommand::Kind::End, owner, false,
        std::chrono::steady_clock::now() +
            std::chrono::milliseconds{
                snapshot_.config.timeouts.log_barrier_ms}});
    static_cast<void>(log_state_.connection_closed());
    snapshot_.log = log_state_.state();
  }
}

void Application::start_log_rollover_if_needed() {
  if (!restart_log_owner_ || !writer_reconfigure_pending_ || log_command_ ||
      log_state_.state() != LogState::Recording ||
      connection_.state() != ConnectionState::Connected ||
      !connection_.generation() || !connection_.session_id() ||
      *restart_log_owner_ != LogSessionOwner{*connection_.generation(),
                                             *connection_.session_id()}) {
    return;
  }
  end_log_session(restart_log_owner_->generation,
                  restart_log_owner_->session_id);
}

void Application::process_log_commands() {
  if (!log_command_) {
    return;
  }
  if (log_command_->completion.wait_for(0ms) != std::future_status::ready) {
    if (std::chrono::steady_clock::now() >= log_command_->deadline) {
      abort_after_shutdown_timeout();
    }
    return;
  }
  const auto result = log_command_->completion.get();
  const auto kind = log_command_->kind;
  const auto owner = log_command_->owner;
  const bool close_after_start = log_command_->close_after_start;
  log_command_.reset();
  if (result.error || result.state == logging::SessionLogState::Error) {
    static_cast<void>(log_state_.fail());
    log_backlog_.clear();
    log_backlog_bytes_ = 0U;
    restart_log_owner_.reset();
    set_notice(result.error ? error_text(*result.error) : "Session log failed");
  } else if (kind == PendingLogCommand::Kind::Start) {
    const bool owner_is_current =
        owner &&
        (connection_.state() == ConnectionState::Connected ||
         connection_.state() == ConnectionState::Disconnecting) &&
        connection_.generation() && connection_.session_id() &&
        *owner == LogSessionOwner{*connection_.generation(),
                                  *connection_.session_id()};
    if (!close_after_start && owner_is_current) {
      static_cast<void>(log_state_.connection_opened());
    }
    std::deque<PendingLogRecord> retained;
    std::size_t retained_bytes = 0U;
    bool queue_failed = false;
    while (!log_backlog_.empty()) {
      std::vector<logging::Record> batch;
      batch.reserve(std::min<std::size_t>(log_backlog_.size(), 64U));
      while (!log_backlog_.empty() && batch.size() < 64U) {
        auto pending = std::move(log_backlog_.front());
        log_backlog_.pop_front();
        const auto pending_bytes = log_record_bytes(pending.record);
        log_backlog_bytes_ -= pending_bytes;
        if (owner && pending.owner == *owner) {
          batch.push_back(std::move(pending.record));
        } else {
          retained_bytes += pending_bytes;
          retained.push_back(std::move(pending));
        }
      }
      if (!batch.empty() && writer_->try_enqueue(std::move(batch)) !=
                                logging::EnqueueResult::Accepted) {
        static_cast<void>(log_state_.fail());
        log_backlog_.clear();
        log_backlog_bytes_ = 0U;
        queue_failed = true;
        set_notice("Session log queue became full");
        break;
      }
    }
    if (!queue_failed) {
      log_backlog_ = std::move(retained);
      log_backlog_bytes_ = retained_bytes;
    }
    if (close_after_start || !owner_is_current) {
      log_command_.emplace(PendingLogCommand{
          writer_->end_session(), PendingLogCommand::Kind::End, owner, false,
          std::chrono::steady_clock::now() +
              std::chrono::milliseconds{
                  snapshot_.config.timeouts.log_barrier_ms}});
    }
  } else if (kind == PendingLogCommand::Kind::End) {
    // end_session() moved the application to WAITING at submission time.
  } else {
    static_cast<void>(log_state_.disable());
  }
  snapshot_.log = log_state_.state();
  if (!snapshot_.shutting_down) {
    reconfigure_log_writer_if_inactive();
    start_log_rollover_if_needed();
    if (!log_command_ && log_state_.state() == LogState::Waiting &&
        (connection_.state() == ConnectionState::Connected ||
         connection_.state() == ConnectionState::Disconnecting)) {
      if (!writer_reconfigure_pending_) {
        restart_log_owner_.reset();
        begin_log_session();
      }
    }
    if (connect_after_log_ && !log_command_ &&
        connection_.state() == ConnectionState::Disconnected) {
      connect_after_log_ = false;
      start_connection();
    }
  }
}

void Application::reconfigure_log_writer_if_inactive() {
  if (!writer_reconfigure_pending_ || log_command_ ||
      log_state_.state() == LogState::Recording) {
    return;
  }
  try {
    auto replacement = std::make_unique<logging::SessionWriter>(
        writer_options(snapshot_.config, default_log_directory_));
    if (log_state_.state() == LogState::Waiting && !replacement->enable()) {
      throw std::runtime_error(
          "replacement log writer could not enter WAITING");
    }
    auto stopped = writer_->shutdown();
    if (stopped.wait_for(std::chrono::milliseconds{
            snapshot_.config.timeouts.log_barrier_ms}) !=
        std::future_status::ready) {
      abort_after_shutdown_timeout();
    }
    writer_ = std::move(replacement);
    writer_reconfigure_pending_ = false;
    snapshot_.log_pending = log_backlog_.size();
  } catch (const std::exception &exception) {
    writer_reconfigure_pending_ = false;
    static_cast<void>(log_state_.fail());
    log_backlog_.clear();
    log_backlog_bytes_ = 0U;
    restart_log_owner_.reset();
    snapshot_.log = log_state_.state();
    set_notice(std::string{"Cannot apply logging settings: "} +
               exception.what());
  }
}

void Application::toggle_log() {
  if (!operations_allowed()) {
    set_notice("Application is stopping; logging command rejected");
    return;
  }
  if (log_command_) {
    set_notice("A log state transition is still in progress");
    return;
  }
  if (log_state_.state() == LogState::Error) {
    log_backlog_.clear();
    log_backlog_bytes_ = 0U;
    log_command_.emplace(
        PendingLogCommand{writer_->disable(), PendingLogCommand::Kind::Disable,
                          std::nullopt, false,
                          std::chrono::steady_clock::now() +
                              std::chrono::milliseconds{
                                  snapshot_.config.timeouts.log_barrier_ms}});
    set_notice("Resetting log error to OFF...");
    return;
  }
  if (log_state_.state() == LogState::Off) {
    if (!writer_->enable()) {
      set_notice("Log writer could not enter WAITING");
      return;
    }
    static_cast<void>(log_state_.enable(false));
    snapshot_.log = log_state_.state();
    if (connection_.state() == ConnectionState::Connected) {
      begin_log_session();
    }
    set_notice(connection_.state() == ConnectionState::Connected
                   ? "Starting session logging..."
                   : "Session logging is WAITING for a connection");
    return;
  }
  log_backlog_.clear();
  log_backlog_bytes_ = 0U;
  restart_log_owner_.reset();
  log_command_.emplace(PendingLogCommand{
      writer_->disable(), PendingLogCommand::Kind::Disable, std::nullopt, false,
      std::chrono::steady_clock::now() +
          std::chrono::milliseconds{snapshot_.config.timeouts.log_barrier_ms}});
  set_notice("Stopping session logging...");
}

void Application::process_scheduler() {
  if (!operations_allowed()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  scheduler_.on_deadline(now);
  if (const auto automatic_stop = scheduler_.take_automatic_stop_request()) {
    const auto reason = automatic_stop->reason ==
                                scheduler::AutomaticStopReason::DeadlineOverflow
                            ? "Quick-send stopped after deadline overflow"
                            : "Quick-send stopped after sequence exhaustion";
    if (!request_quick_task_stop(automatic_stop->generation, reason)) {
      enter_fatal_stopping({ErrorCode::InternalInvariantBroken,
                            Operation::CoordinateFatal, WorkerKind::Serial,
                            FatalReason::InvariantBroken,
                            SignalSourceLocation::current()});
      return;
    }
  }
  if (connection_.state() == ConnectionState::Connected) {
    if (const auto send = scheduler_.on_tx_boundary(now)) {
      if (!submit_tx(send->execution->bytes, send->token.generation,
                     send->token)) {
        scheduler::TxBoundary failed;
        failed.completed_request = send->token;
        failed.completed_successfully = false;
        static_cast<void>(scheduler_.on_tx_boundary(now, failed));
      }
    }
  }
  snapshot_.task = scheduler_.snapshot();
}

bool Application::request_quick_task_stop(const TaskGeneration generation,
                                          const std::string_view notice) {
  if (!operations_allowed()) {
    set_notice("Application is stopping; task stop rejected");
    return false;
  }
  try {
    const StopTaskCommand command{issue_operation(), generation};
    auto submitted = serial_->request_stop_task(command);
    if (!submitted) {
      set_notice(error_text(submitted.error()));
      return false;
    }
    if (*submitted == serial::SubmitStatus::AlreadyPending) {
      set_notice("A quick-send stop is already pending");
      return false;
    }
    set_notice(std::string{notice});
    return true;
  } catch (const std::exception &exception) {
    set_notice(exception.what());
    return false;
  }
}

void Application::tick() {
  if (stopped_) {
    return;
  }
  serial_->acknowledge_ui_wakeup();
  scanner_->acknowledge_ui_wakeup();
  process_serial_data();
  process_serial_completions();
  process_scan_completions();
  if (framer_ && connection_.state() == ConnectionState::Connected) {
    enqueue_frames(framer_->on_idle(std::chrono::steady_clock::now()),
                   SessionEventOrigin::Normal);
  }
  process_log_commands();
  process_save_completions();
  process_scheduler();
  update_worker_state();
  snapshot_.log_pending = writer_->queued_records() + log_backlog_.size();
  snapshot_.rx_ingress_bytes = serial_->queued_data_bytes();
}

void Application::update_worker_state() {
  if (writer_->state() == logging::SessionLogState::Error &&
      log_state_.state() != LogState::Error) {
    static_cast<void>(log_state_.fail());
    log_backlog_.clear();
    log_backlog_bytes_ = 0U;
    restart_log_owner_.reset();
    snapshot_.log = log_state_.state();
    set_notice("Session log stopped after an asynchronous writer failure");
  }
  if (const auto stopped = serial_->worker_stopped_signal()) {
    static_cast<void>(workers_.mark_at_return_point(*stopped));
  }
  if (const auto stopped = scanner_->worker_stopped_signal()) {
    static_cast<void>(workers_.mark_at_return_point(*stopped));
  }
  if (const auto fatal = serial_->fatal_signal()) {
    enter_fatal_stopping(*fatal);
  }
  if (const auto fatal = scanner_->fatal_signal()) {
    enter_fatal_stopping(*fatal);
  }
  if (const auto overflow = serial_->overflow_signal();
      overflow && connection_.session_id() == overflow->session_id) {
    set_notice("RX ingress overflow; session is being closed");
  }
}

void Application::enter_fatal_stopping(const FatalSignal &signal) noexcept {
  if (fatal_guard_.enter_fatal_stopping() != StateChange::Applied) {
    return;
  }
  snapshot_.fatal_stopping = true;
  snapshot_.shutting_down = true;
  static_cast<void>(scheduler_.invalidate_for_shutdown());
  snapshot_.task = scheduler_.snapshot();
  serial_->request_stop();
  scanner_->request_stop();
  persistence_->request_stop();
  try {
    fatal_log_shutdown_ = writer_->shutdown();
    set_notice("Fatal worker signal: " +
               std::string{error_descriptor(signal.code).identifier});
  } catch (...) {
    snapshot_.notice = "Fatal worker signal; emergency shutdown requested";
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
  snapshot_.records.clear();
  snapshot_.display_bytes = 0U;
  snapshot_.display_gap_records = 0U;
  set_notice("In-memory session records cleared; log files were not changed");
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
  snapshot_.device_path = logging::sanitize_message(device_path_);
  set_notice("Serial device selected");
  return {};
}

Status Application::apply_baud(const std::string_view value) {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  if (connection_.state() != ConnectionState::Disconnected) {
    return tl::unexpected(
        application_error("hardware is locked while connected"));
  }
  const auto baud = parse_integer<std::int32_t>(value);
  if (!baud || *baud < 1) {
    return tl::unexpected(
        application_error("baud must be one of the supported presets"));
  }
  auto candidate = snapshot_.config;
  candidate.serial.baud = *baud;
  if (const auto errors = config::validate_config_snapshot(candidate);
      !errors.empty()) {
    return tl::unexpected(application_error(errors.front().message));
  }
  snapshot_.config = candidate;
  save_config();
  return {};
}

Status Application::apply_data_format(const std::string_view value) {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  if (connection_.state() != ConnectionState::Disconnected) {
    return tl::unexpected(
        application_error("hardware is locked while connected"));
  }
  const auto fields = split(value, ',');
  if (fields.size() != 4U) {
    return tl::unexpected(
        application_error("format is data_bits,stop_bits,parity,flow"));
  }
  const auto data = parse_integer<std::int32_t>(fields[0]);
  const auto stop = parse_integer<std::int32_t>(fields[1]);
  const auto parity = parse_parity(fields[2]);
  const auto flow = parse_flow(fields[3]);
  if (!data || !stop || !parity || !flow) {
    return tl::unexpected(application_error("invalid serial format value"));
  }
  auto candidate = snapshot_.config;
  candidate.serial.data_bits = *data;
  candidate.serial.stop_bits = *stop;
  candidate.serial.parity = *parity;
  candidate.serial.flow_control = *flow;
  if (const auto errors = config::validate_config_snapshot(candidate);
      !errors.empty()) {
    return tl::unexpected(application_error(errors.front().message));
  }
  snapshot_.config = candidate;
  save_config();
  return {};
}

Status Application::apply_newline(const std::string_view value) {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  const auto newline = parse_newline(value);
  if (!newline) {
    return tl::unexpected(application_error("newline must be none/lf/cr/crlf"));
  }
  snapshot_.config.send.newline = *newline;
  save_config();
  return {};
}

Status Application::apply_view(const std::string_view value) {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  const auto fields = split(value, ',');
  if (fields.size() != 6U) {
    return tl::unexpected(application_error(
        "view must be rx_mode,tx_mode plus four direction filters"));
  }
  const auto rx_view = parse_receive_view(fields[0]);
  const auto tx_view = parse_receive_view(fields[1]);
  if (!rx_view || !tx_view) {
    return tl::unexpected(
        application_error("RX/TX view must be txt/hex/mixed"));
  }
  auto candidate = snapshot_.config;
  candidate.receive.rx_view = *rx_view;
  candidate.receive.tx_view = *tx_view;
  if (std::ranges::any_of(fields | std::views::drop(2),
                          [](const std::string_view field) {
                            return field != "0" && field != "1";
                          })) {
    return tl::unexpected(application_error("view filters must be 0 or 1"));
  }
  const auto enabled = [](const std::string_view field) {
    return field == "1";
  };
  const DirectionFilter filter{enabled(fields[2]), enabled(fields[3]),
                               enabled(fields[4]), enabled(fields[5])};
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
  save_config();
  save_state();
  return {};
}

Status Application::apply_send_mode(const std::string_view value) {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  if (value == "txt") {
    snapshot_.config.send.mode = config::SendMode::Txt;
  } else if (value == "hex") {
    snapshot_.config.send.mode = config::SendMode::Hex;
  } else {
    return tl::unexpected(application_error("send mode must be txt or hex"));
  }
  save_config();
  return {};
}

Status Application::apply_logging(const std::string_view value,
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
  save_config();
  writer_reconfigure_pending_ = true;
  restart_log_owner_.reset();
  if (policy == LogApplyPolicy::RotateNow) {
    if (log_command_ && log_command_->kind == PendingLogCommand::Kind::Start &&
        log_command_->owner) {
      restart_log_owner_ = log_command_->owner;
    } else if (log_state_.state() == LogState::Recording &&
               connection_.generation() && connection_.session_id()) {
      restart_log_owner_ =
          LogSessionOwner{*connection_.generation(), *connection_.session_id()};
    }
  }
  reconfigure_log_writer_if_inactive();
  const bool rotates_active_file =
      policy == LogApplyPolicy::RotateNow && restart_log_owner_.has_value();
  if (policy == LogApplyPolicy::RotateNow) {
    start_log_rollover_if_needed();
  }
  set_notice(rotates_active_file
                 ? "Logging settings saved; active recording will rotate"
                 : "Logging settings saved for the next recording");
  return {};
}

Status Application::validate_logging(const std::string_view value) const {
  auto candidate = logging_candidate(snapshot_.config, value);
  if (!candidate) {
    return tl::unexpected(candidate.error());
  }
  return {};
}

Status Application::apply_quick_slot(const std::string_view value) {
  if (!operations_allowed()) {
    return operation_rejected();
  }
  const auto fields = split(value, '|');
  if (fields.size() != 6U) {
    return tl::unexpected(application_error(
        "slot format is index|name|mode|content|newline|note"));
  }
  const auto index = parse_integer<std::uint32_t>(fields[0]);
  if (!index || *index < 1U || *index > 20U) {
    return tl::unexpected(application_error("invalid quick-send slot"));
  }
  auto candidate = snapshot_.quick_send;
  const bool clear = std::ranges::all_of(
      fields | std::views::drop(1),
      [](const std::string_view field) { return field.empty(); });
  if (clear) {
    candidate.slots[*index - 1U].reset();
  } else {
    const auto newline = parse_newline(fields[4], true);
    if (!newline || (fields[2] != "txt" && fields[2] != "hex")) {
      return tl::unexpected(application_error("invalid quick-send slot"));
    }
    config::QuickSendSlot slot;
    slot.index = *index;
    slot.name = std::string{fields[1]};
    slot.mode =
        fields[2] == "txt" ? config::SendMode::Txt : config::SendMode::Hex;
    slot.content = std::string{fields[3]};
    slot.newline = *newline;
    slot.note = std::string{fields[5]};
    candidate.slots[*index - 1U] = std::move(slot);
  }
  if (const auto errors = config::validate_quick_send_snapshot(candidate);
      !errors.empty()) {
    return tl::unexpected(application_error(errors.front().message));
  }
  auto submission = persistence_->save_quick_send(
      candidate, quick_load_.file_identity, quick_load_.document,
      snapshot_.quick_send_read_only);
  if (!submission.accepted()) {
    return tl::unexpected(submission.error.value_or(
        application_error("quick-send save is busy", Operation::SaveConfig)));
  }
  pending_quick_send_ = std::move(candidate);
  save_completions_[1] = std::move(submission.completion);
  snapshot_.quick_send_save_pending = true;
  snapshot_.quick_send_save_failed = false;
  set_notice("Saving quick-send slot...");
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
  const auto now = std::chrono::steady_clock::now();
  const auto scheduler_state = scheduler_.snapshot();
  if (scheduler_state.state == scheduler::SchedulerState::Running &&
      !replacement_confirmed) {
    return tl::unexpected(
        application_error("replacing the active task requires confirmation"));
  }
  if (scheduler_state.state == scheduler::SchedulerState::Stopping) {
    return tl::unexpected(
        application_error("the active task is still stopping"));
  }
  if (!scheduler::Scheduler::valid_interval(interval_ms)) {
    return tl::unexpected(application_error("invalid interval or task state"));
  }
  if (scheduler_state.state == scheduler::SchedulerState::Running) {
    if (!scheduler_state.generation ||
        !request_quick_task_stop(
            *scheduler_state.generation,
            "Stopping the old task before starting its replacement...")) {
      return tl::unexpected(
          application_error("could not submit the task replacement stop"));
    }
  }
  auto started = scheduler_.start(
      {std::move(*execution.execution), interval_ms}, now,
      scheduler_state.state == scheduler::SchedulerState::Running);
  if (started.status ==
      scheduler::TaskStartStatus::ReplacementConfirmationRequired) {
    return tl::unexpected(
        application_error("replacing the active task requires confirmation"));
  }
  if (started.status != scheduler::TaskStartStatus::Started &&
      started.status != scheduler::TaskStartStatus::ReplacementStopRequested) {
    return tl::unexpected(application_error("invalid interval or task state"));
  }
  snapshot_.preferences.last_quick_send_slot = slot;
  snapshot_.preferences.last_interval_ms =
      static_cast<std::uint32_t>(interval_ms);
  save_state();
  if (started.status == scheduler::TaskStartStatus::ReplacementStopRequested &&
      started.generation_to_stop) {
    snapshot_.task = scheduler_.snapshot();
    return {};
  }
  process_scheduler();
  set_notice(started.status ==
                     scheduler::TaskStartStatus::ReplacementStopRequested
                 ? "Quick-send replacement started"
             : interval_ms == 0U ? "Quick send submitted"
                                 : "Periodic quick-send task started");
  return {};
}

void Application::stop_quick_task() {
  if (!operations_allowed()) {
    set_notice("Application is stopping; task stop rejected");
    return;
  }
  const auto current = scheduler_.snapshot();
  if (current.state == scheduler::SchedulerState::Running &&
      current.generation &&
      request_quick_task_stop(
          *current.generation,
          "Stopping quick-send task at the serial owner boundary...")) {
    static_cast<void>(scheduler_.request_stop());
  } else if (current.state == scheduler::SchedulerState::Stopping) {
    set_notice("A quick-send stop is already pending");
  }
  snapshot_.task = scheduler_.snapshot();
}

void Application::save_config() {
  if (!operations_allowed()) {
    return;
  }
  if (save_completions_[0]) {
    config_save_dirty_ = true;
    set_notice("Configuration save queued behind the active save");
    return;
  }
  auto submission = persistence_->save_config(
      snapshot_.config, config_load_.file_identity, config_load_.document,
      snapshot_.config_read_only);
  if (!submission.accepted()) {
    config_save_dirty_ = submission.state == config::SaveSubmitState::Busy;
    set_notice(submission.error ? error_text(*submission.error)
                                : "Config save is busy");
  } else {
    config_save_dirty_ = false;
    save_completions_[0] = std::move(submission.completion);
  }
}

void Application::save_state() {
  if (!operations_allowed()) {
    return;
  }
  if (save_completions_[2]) {
    state_save_dirty_ = true;
    return;
  }
  auto submission =
      persistence_->save_state(snapshot_.preferences, state_load_.file_identity,
                               state_load_.document, snapshot_.state_read_only);
  if (!submission.accepted()) {
    state_save_dirty_ = submission.state == config::SaveSubmitState::Busy;
    if (submission.error) {
      set_notice(error_text(*submission.error));
    }
  } else {
    state_save_dirty_ = false;
    save_completions_[2] = std::move(submission.completion);
  }
}

void Application::process_save_completions() {
  for (std::size_t index = 0U; index < save_completions_.size(); ++index) {
    auto &pending = save_completions_[index];
    if (!pending || pending->wait_for(0ms) != std::future_status::ready) {
      continue;
    }
    auto completion = pending->get();
    pending.reset();
    if (index == 1U) {
      snapshot_.quick_send_save_pending = false;
    }
    if (completion.outcome.state == config::CommitState::NotCommitted) {
      if (index == 1U) {
        pending_quick_send_.reset();
        snapshot_.quick_send_save_failed = true;
      }
      if (completion.outcome.error) {
        set_notice(error_text(*completion.outcome.error));
      }
      continue;
    }
    if (index == 0U) {
      config_load_ = config::load_config_toml(paths_.config);
      config_load_.document = std::move(completion.serialized_document);
    } else if (index == 1U) {
      quick_load_ = config::load_quick_send_toml(paths_.quick_send);
      quick_load_.document = std::move(completion.serialized_document);
      if (pending_quick_send_) {
        snapshot_.quick_send = std::move(*pending_quick_send_);
        pending_quick_send_.reset();
      }
      snapshot_.quick_send_save_failed = false;
    } else {
      state_load_ = config::load_state_toml(paths_.state);
      state_load_.document = std::move(completion.serialized_document);
    }
    if (completion.outcome.state ==
        config::CommitState::CommittedDurabilityUnknown) {
      set_notice(
          "Configuration committed, but directory durability is unknown");
    }
  }
  if (config_save_dirty_ && !save_completions_[0]) {
    save_config();
  }
  if (state_save_dirty_ && !save_completions_[2]) {
    save_state();
  }
}

std::string Application::render_record(const VisibleRecord &record) const {
  std::string prefix;
  switch (record.direction) {
  case RecordDirection::Rx:
    prefix = "<- RX ";
    break;
  case RecordDirection::Tx:
    prefix = "-> TX ";
    break;
  case RecordDirection::System:
    prefix = "!  SYS ";
    break;
  case RecordDirection::Error:
    prefix = "x  ERR ";
    break;
  }
  prefix += "[" + record.time_utc + "] ";
  if (record.direction == RecordDirection::System ||
      record.direction == RecordDirection::Error) {
    return prefix + logging::sanitize_message(record.message);
  }
  const auto view = record.direction == RecordDirection::Tx
                        ? snapshot_.config.receive.tx_view
                        : snapshot_.config.receive.rx_view;
  encoding::DisplayMode mode = encoding::DisplayMode::Text;
  if (view == config::ReceiveView::Hex) {
    mode = encoding::DisplayMode::Hex;
  } else if (view == config::ReceiveView::Mixed) {
    mode = encoding::DisplayMode::Mixed;
  }
  return prefix + encoding::render(record.payload, mode);
}

std::vector<std::uint64_t>
Application::search(const std::string_view query,
                    const DirectionFilter filter) const {
  std::vector<std::uint64_t> result;
  if (query.empty()) {
    return result;
  }
  for (std::size_t index = 0U;
       index < snapshot_.records.size() && result.size() < 10'000U; ++index) {
    if (direction_visible(snapshot_.records[index].direction, filter) &&
        render_record(snapshot_.records[index]).find(query) !=
            std::string::npos) {
      result.push_back(snapshot_.records[index].record_id);
    }
  }
  return result;
}

bool Application::has_exit_risk() const noexcept {
  return connection_.state() != ConnectionState::Disconnected ||
         log_state_.state() != LogState::Off ||
         scheduler_.snapshot().state != scheduler::SchedulerState::Idle ||
         !snapshot_.draft.empty();
}

bool Application::shutdown() noexcept {
  if (stopped_) {
    return fatal_guard_.state() != ProcessLifecycle::FatalStopping;
  }
  snapshot_.shutting_down = true;
  connect_after_log_ = false;
  const bool fatal = fatal_guard_.state() == ProcessLifecycle::FatalStopping;
  const auto owner_timeout =
      std::chrono::milliseconds{snapshot_.config.timeouts.owner_stop_ms};
  const auto log_timeout =
      std::chrono::milliseconds{snapshot_.config.timeouts.log_barrier_ms};
  try {
    if (fatal) {
      serial_->request_stop();
      scanner_->request_stop();
      if (std::this_thread::get_id() != fatal_guard_.owner_thread()) {
        return false;
      }
      const auto deadline = std::chrono::steady_clock::now() + owner_timeout;
      const bool serial_stopped = serial_->wait_until_stopped(deadline);
      const bool scanner_stopped = scanner_->wait_until_stopped(deadline);
      persistence_->request_stop();
      const bool persistence_stopped =
          persistence_->wait_until_stopped(deadline);
      if (!fatal_log_shutdown_) {
        fatal_log_shutdown_ = writer_->shutdown();
      }
      const bool log_completion_ready =
          fatal_log_shutdown_->wait_for(log_timeout) ==
          std::future_status::ready;
      const bool log_stopped =
          log_completion_ready &&
          writer_->wait_until_stopped(std::chrono::steady_clock::now() +
                                      log_timeout);
      if (!serial_stopped || !scanner_stopped || !persistence_stopped ||
          !log_stopped) {
        abort_after_shutdown_timeout();
      }
      if (const auto signal = serial_->worker_stopped_signal()) {
        static_cast<void>(workers_.mark_at_return_point(*signal));
      }
      if (const auto signal = scanner_->worker_stopped_signal()) {
        static_cast<void>(workers_.mark_at_return_point(*signal));
      }
      static_cast<void>(workers_.mark_at_return_point(
          {WorkerKind::SessionLog, WorkerLifecycle::AtReturnPoint,
           WorkerExitReason::Fatal}));
      persistence_->shutdown();
      static_cast<void>(workers_.mark_at_return_point(
          {WorkerKind::Persistence, WorkerLifecycle::AtReturnPoint,
           WorkerExitReason::Completed}));
      stopped_ = true;
      return false;
    }
    stop_quick_task();
    const auto drain_log_commands = [this, log_timeout] {
      const auto deadline = std::chrono::steady_clock::now() + log_timeout;
      while (log_command_) {
        if (log_command_->completion.wait_until(deadline) !=
            std::future_status::ready) {
          abort_after_shutdown_timeout();
        }
        process_log_commands();
      }
    };
    drain_log_commands();
    start_log_rollover_if_needed();
    drain_log_commands();
    reconfigure_log_writer_if_inactive();
    if ((connection_.state() == ConnectionState::Connected ||
         connection_.state() == ConnectionState::Disconnecting) &&
        log_state_.state() == LogState::Waiting && !log_command_) {
      begin_log_session();
      drain_log_commands();
    }
    if (connection_.state() == ConnectionState::Connecting) {
      request_cancel_connect();
    } else if (connection_.state() == ConnectionState::Connected ||
               connection_.state() == ConnectionState::Error) {
      request_disconnect();
    }
    if (connection_.state() != ConnectionState::Disconnected) {
      const auto deadline = std::chrono::steady_clock::now() + owner_timeout;
      while (connection_.state() != ConnectionState::Disconnected &&
             std::chrono::steady_clock::now() < deadline) {
        process_serial_data();
        process_serial_completions();
        std::this_thread::sleep_for(5ms);
      }
      if (connection_.state() != ConnectionState::Disconnected) {
        abort_after_shutdown_timeout();
      }
    }
    drain_log_commands();
    if (writer_->state() == logging::SessionLogState::Recording) {
      auto barrier =
          writer_->barrier(next_sequence_ == 0U ? 0U : next_sequence_ - 1U);
      if (barrier.wait_for(log_timeout) != std::future_status::ready) {
        abort_after_shutdown_timeout();
      }
      auto ended = writer_->end_session();
      if (ended.wait_for(log_timeout) != std::future_status::ready) {
        abort_after_shutdown_timeout();
      }
    }
    auto writer_stopped = writer_->shutdown();
    if (writer_stopped.wait_for(log_timeout) != std::future_status::ready ||
        !writer_->wait_until_stopped(std::chrono::steady_clock::now() +
                                     log_timeout)) {
      abort_after_shutdown_timeout();
    }
    static_cast<void>(workers_.mark_at_return_point(
        {WorkerKind::SessionLog, WorkerLifecycle::AtReturnPoint,
         WorkerExitReason::Completed}));
    scanner_->request_stop();
    if (!scanner_->wait_until_stopped(std::chrono::steady_clock::now() +
                                      owner_timeout)) {
      abort_after_shutdown_timeout();
    }
    if (const auto signal = scanner_->worker_stopped_signal()) {
      static_cast<void>(workers_.mark_at_return_point(*signal));
    }
    serial_->request_stop();
    if (!serial_->wait_until_stopped(std::chrono::steady_clock::now() +
                                     owner_timeout)) {
      abort_after_shutdown_timeout();
    }
    if (const auto signal = serial_->worker_stopped_signal()) {
      static_cast<void>(workers_.mark_at_return_point(*signal));
    }
    const auto persistence_deadline =
        std::chrono::steady_clock::now() + owner_timeout;
    while (config_save_dirty_ || state_save_dirty_ ||
           std::ranges::any_of(save_completions_, [](const auto &completion) {
             return completion.has_value();
           })) {
      bool waited = false;
      for (auto &completion : save_completions_) {
        if (completion) {
          if (completion->wait_until(persistence_deadline) !=
              std::future_status::ready) {
            abort_after_shutdown_timeout();
          }
          waited = true;
          break;
        }
      }
      process_save_completions();
      if (!waited && std::chrono::steady_clock::now() >= persistence_deadline) {
        abort_after_shutdown_timeout();
      }
    }
    persistence_->request_stop();
    if (!persistence_->wait_until_stopped(std::chrono::steady_clock::now() +
                                          owner_timeout)) {
      abort_after_shutdown_timeout();
    }
    persistence_->shutdown();
    static_cast<void>(workers_.mark_at_return_point(
        {WorkerKind::Persistence, WorkerLifecycle::AtReturnPoint,
         WorkerExitReason::Completed}));
    stopped_ = true;
    return true;
  } catch (...) {
    abort_after_shutdown_timeout();
  }
}

} // namespace lazycom::app
