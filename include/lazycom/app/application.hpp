#pragma once

#include <lazycom/app/completion.hpp>
#include <lazycom/config/persistence.hpp>
#include <lazycom/encoding/display.hpp>
#include <lazycom/framing/rx_framer.hpp>
#include <lazycom/logging/session_writer.hpp>
#include <lazycom/scheduler/scheduler.hpp>
#include <lazycom/serial/scanner.hpp>
#include <lazycom/serial/service.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lazycom::app {

enum class RecordDirection : std::uint8_t { Rx, Tx, System, Error };

struct VisibleRecord {
  std::uint64_t record_id{};
  std::uint64_t sequence{};
  RecordDirection direction{RecordDirection::System};
  std::string time_utc;
  std::vector<std::byte> payload;
  std::string message;
  std::optional<ErrorCode> error_code;
  std::optional<OperationId> operation_id;
};

struct DirectionFilter {
  bool rx{true};
  bool tx{true};
  bool system{true};
  bool error{true};
};

struct UserAlert {
  ErrorCode code{ErrorCode::ValidationInvalidValue};
  std::string title;
  std::string message;
};

enum class LogApplyPolicy : std::uint8_t { NextSession, RotateNow };

struct ApplicationSnapshot {
  ConnectionState connection{ConnectionState::Disconnected};
  InteractionState interaction{InteractionState::Normal};
  LogState log{LogState::Off};
  config::ConfigSnapshot config;
  config::QuickSendSnapshot quick_send;
  config::StateSnapshot preferences;
  DirectionFilter filter;
  std::optional<serial::PortConfig> active_port_config;
  std::string device_path;
  std::string draft;
  std::string notice;
  std::string configuration_notice;
  std::vector<serial::DeviceInfo> devices;
  std::deque<VisibleRecord> records;
  scheduler::SchedulerSnapshot task;
  std::uint64_t rx_bytes{};
  std::uint64_t tx_bytes{};
  std::uint64_t display_gap_records{};
  std::size_t display_bytes{};
  std::size_t tx_pending{};
  std::size_t rx_ingress_bytes{};
  std::size_t log_pending{};
  std::string effective_log_directory;
  bool scanning{};
  bool config_read_only{};
  bool quick_send_read_only{};
  bool state_read_only{};
  bool quick_send_save_pending{};
  bool quick_send_save_failed{};
  bool manual_pause{};
  bool shutting_down{};
  bool fatal_stopping{};
  std::optional<UserAlert> alert;
};

struct ApplicationDependencies {
  std::unique_ptr<serial::ISerialBackend> serial_backend;
  std::unique_ptr<serial::ISerialBackend> scanner_backend;
  serial::UiWakeCallback wake_callback{};
  void *wake_context{};
  std::optional<config::PersistencePaths> paths;
  std::optional<std::filesystem::path> log_directory;
};

class Application final {
public:
  [[nodiscard]] static Result<std::unique_ptr<Application>>
  create(ApplicationDependencies dependencies);
  [[nodiscard]] static Result<std::unique_ptr<Application>>
  create_default(serial::UiWakeCallback wake_callback = nullptr,
                 void *wake_context = nullptr);

  ~Application();
  Application(const Application &) = delete;
  Application &operator=(const Application &) = delete;
  Application(Application &&) = delete;
  Application &operator=(Application &&) = delete;

  [[nodiscard]] const ApplicationSnapshot &snapshot() const noexcept;

  void tick();
  void request_scan();
  void set_device_path(std::string path);
  void connect();
  void disconnect();
  void connection_control();
  void set_interaction(InteractionState state) noexcept;
  void set_draft(std::string draft);
  void submit_draft();
  void history_previous();
  void history_next();
  void toggle_log();
  void toggle_pause() noexcept;
  void clear_records();
  void dismiss_alert() noexcept;
  void publish_alert(UserAlert alert);
  void publish_permission_alert(std::string_view path,
                                const serial::DevicePermission &permission);

  [[nodiscard]] Status apply_port(std::string_view value);
  [[nodiscard]] Status apply_baud(std::string_view value);
  [[nodiscard]] Status apply_data_format(std::string_view value);
  [[nodiscard]] Status apply_newline(std::string_view value);
  [[nodiscard]] Status apply_view(std::string_view value);
  [[nodiscard]] Status apply_send_mode(std::string_view value);
  [[nodiscard]] Status
  apply_logging(std::string_view value,
                LogApplyPolicy policy = LogApplyPolicy::RotateNow);
  [[nodiscard]] Status validate_logging(std::string_view value) const;
  [[nodiscard]] Status apply_quick_slot(std::string_view value);
  [[nodiscard]] Status execute_quick(std::uint32_t slot,
                                     std::uint64_t interval_ms,
                                     bool replacement_confirmed = false);
  void stop_quick_task();

  [[nodiscard]] std::vector<std::uint64_t>
  search(std::string_view query, DirectionFilter filter = {}) const;
  [[nodiscard]] std::string render_record(const VisibleRecord &record) const;
  [[nodiscard]] bool has_exit_risk() const noexcept;
  [[nodiscard]] bool shutdown() noexcept;

private:
  struct LoadedConfiguration;
  Application(ApplicationDependencies dependencies, LoadedConfiguration loaded,
              std::unique_ptr<serial::SerialService> serial_service,
              std::unique_ptr<serial::DeviceScanner> scanner,
              std::unique_ptr<logging::SessionWriter> writer);

  [[nodiscard]] static Result<LoadedConfiguration>
  load_configuration(const std::optional<config::PersistencePaths> &paths,
                     const std::optional<std::filesystem::path> &log_directory);
  [[nodiscard]] OperationId issue_operation();
  [[nodiscard]] ConnectionGeneration issue_connection_generation();
  [[nodiscard]] ScanGeneration issue_scan_generation();
  void set_notice(std::string message);
  [[nodiscard]] bool operations_allowed() const noexcept;
  [[nodiscard]] Status operation_rejected() const;
  void start_connection();
  void finish_failed_connection(const serial::ConnectCompletion &completion);
  void request_disconnect();
  void request_cancel_connect();
  void retry_cancel_race_disconnect();
  void process_serial_data();
  void process_serial_completions();
  void process_scan_completions();
  void process_log_commands();
  void process_save_completions();
  void process_scheduler();
  [[nodiscard]] bool request_quick_task_stop(TaskGeneration generation,
                                             std::string_view notice);
  void begin_log_session();
  void end_log_session(ConnectionGeneration generation, SessionId session_id);
  void start_log_rollover_if_needed();
  void enqueue_record(RecordDirection direction,
                      std::span<const std::byte> payload,
                      std::string message = {},
                      std::optional<ErrorCode> error_code = std::nullopt,
                      std::optional<OperationId> operation = std::nullopt,
                      SessionEventOrigin origin = SessionEventOrigin::Normal);
  void enqueue_frames(std::vector<framing::RxFrame> frames,
                      SessionEventOrigin origin);
  [[nodiscard]] bool submit_tx(std::vector<std::byte> payload,
                               std::optional<TaskGeneration> task_generation,
                               std::optional<scheduler::ScheduledRequestToken>
                                   scheduled_token = std::nullopt);
  void save_config();
  void save_state();
  void update_worker_state();
  void enter_fatal_stopping(const FatalSignal &signal) noexcept;
  void reconfigure_log_writer_if_inactive();

  struct LogSessionOwner {
    ConnectionGeneration generation{};
    SessionId session_id{};
    auto operator<=>(const LogSessionOwner &) const = default;
  };

  struct PendingLogCommand {
    std::future<logging::SessionCommandResult> completion;
    enum class Kind : std::uint8_t { Start, End, Disable } kind{Kind::Start};
    std::optional<LogSessionOwner> owner;
    bool close_after_start{};
    std::chrono::steady_clock::time_point deadline;
  };

  struct PendingLogRecord {
    LogSessionOwner owner;
    logging::Record record;
  };

  ApplicationSnapshot snapshot_;
  ConnectionStateMachine connection_;
  InteractionStateMachine interaction_;
  LogStateMachine log_state_;
  IdSequence<OperationId> operation_ids_;
  IdSequence<ConnectionGeneration> connection_ids_;
  IdSequence<ScanGeneration> scan_ids_;
  std::unique_ptr<serial::SerialService> serial_;
  std::unique_ptr<serial::DeviceScanner> scanner_;
  std::unique_ptr<logging::SessionWriter> writer_;
  std::unique_ptr<config::PersistenceWorker> persistence_;
  config::PersistencePaths paths_;
  config::ConfigLoadResult config_load_;
  config::QuickSendLoadResult quick_load_;
  config::StateLoadResult state_load_;
  std::filesystem::path default_log_directory_;
  std::string device_path_;
  std::optional<ScanGeneration> latest_scan_generation_;
  std::optional<serial::PortConfig> connected_port_config_;
  std::unique_ptr<framing::RxFramer> framer_;
  scheduler::Scheduler scheduler_;
  std::unordered_map<std::uint64_t, scheduler::ScheduledRequestToken>
      scheduled_operations_;
  std::deque<std::string> history_;
  std::size_t history_bytes_{};
  std::optional<std::size_t> history_index_;
  std::string history_sentinel_;
  std::uint64_t next_record_id_{1U};
  std::uint64_t next_sequence_{1U};
  std::chrono::steady_clock::time_point session_started_{};
  std::optional<PendingLogCommand> log_command_;
  std::deque<PendingLogRecord> log_backlog_;
  std::size_t log_backlog_bytes_{};
  std::optional<LogSessionOwner> restart_log_owner_;
  std::optional<LogSessionOwner> processed_cleanup_;
  std::deque<serial::DisconnectCompletion> deferred_disconnect_completions_;
  std::array<std::optional<std::future<config::SaveCompletion>>, 3>
      save_completions_;
  std::optional<config::QuickSendSnapshot> pending_quick_send_;
  std::size_t active_tx_count_{};
  bool manual_pause_{};
  bool startup_scan_alert_pending_{true};
  bool config_save_dirty_{};
  bool state_save_dirty_{};
  bool writer_reconfigure_pending_{};
  bool connect_after_log_{};
  bool cancel_race_disconnect_pending_{};
  MainThreadFatalGuard fatal_guard_;
  WorkerLifecycleRegistry workers_;
  std::optional<std::future<logging::SessionCommandResult>> fatal_log_shutdown_;
  bool stopped_{};
};

} // namespace lazycom::app
