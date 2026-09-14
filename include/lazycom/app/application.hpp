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
  std::uint64_t notice_revision{};
  bool notice_error{};
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

/**
 * @brief Main-thread application coordinator and UI-facing command facade.
 *
 * Construction, all member calls, shutdown, and destruction are confined to
 * the UI/main thread. Worker threads may invoke only the supplied wake callback
 * and never mutate Application state directly. A successful instance owns its
 * serial, scanner, logging, and persistence workers; wake_context remains
 * caller-owned until all workers are stopped and destroyed.
 *
 * snapshot() exposes borrowed state that may change after any command or tick.
 */
class Application final {
public:
  /**
   * @brief Loads configuration and starts all application workers.
   * @param dependencies Consumed dependencies. Serial and scanner backends must
   * be distinct non-null instances.
   * @return An owning application, or an expected construction Error.
   * @note Success does not imply the asynchronously requested startup scan or
   * initial logging session has succeeded.
   */
  [[nodiscard]] static Result<std::unique_ptr<Application>>
  create(ApplicationDependencies dependencies);
  /**
   * @brief Creates independent default libserialport backends and the app.
   * @param wake_callback Optional worker-safe coalesced UI wake callback.
   * @param wake_context Borrowed callback context valid through shutdown.
   * @throws std::bad_alloc if backend allocation fails before error adaptation.
   */
  [[nodiscard]] static Result<std::unique_ptr<Application>>
  create_default(serial::UiWakeCallback wake_callback = nullptr,
                 void *wake_context = nullptr);

  ~Application();
  Application(const Application &) = delete;
  Application &operator=(const Application &) = delete;
  Application(Application &&) = delete;
  Application &operator=(Application &&) = delete;

  /** @return Borrowed view-model state valid until Application destruction. */
  [[nodiscard]] const ApplicationSnapshot &snapshot() const noexcept;

  /**
   * @brief Advances worker completions, serial data, framing, logging, saves,
   * and scheduled-send state.
   * @note Main-thread only and non-reentrant. Normal progress does not wait for
   * worker I/O; fatal deadline handling may abort the process.
   */
  void tick();
  /** @brief Attempts to admit an asynchronous device scan. */
  void request_scan();
  /**
   * @brief Sets a device path through the internal/test seam while
   * disconnected.
   * @warning Product UI selection must use apply_port() and the latest scan.
   */
  void set_device_path(std::string path);
  /** @brief Attempts to admit a connection using a captured hardware snapshot.
   */
  void connect();
  /** @brief Cancels Connecting or begins cleanup of the active session. */
  void disconnect();
  /** @brief Dispatches connect, cancel, or disconnect for the current state. */
  void connection_control();
  void set_interaction(InteractionState state) noexcept;
  /**
   * @brief Replaces the editable draft, truncating it to the configured bytes.
   * @note Byte truncation does not preserve a UTF-8 code-point boundary.
   */
  void set_draft(std::string draft);
  /**
   * @brief Parses and attempts to admit the complete draft for transmission.
   * @note Validation or admission failure preserves the draft. Serial-service
   * admission records history and clears it immediately, before terminal TX.
   * @return true only when serial-service admission succeeded, not terminal TX.
   */
  bool submit_draft();
  void history_previous();
  void history_next();
  /** @brief Starts the asynchronous log-state transition for the current state.
   */
  void toggle_log();
  void toggle_pause() noexcept;
  void clear_records();
  void dismiss_alert() noexcept;
  void publish_alert(UserAlert alert);
  /** @brief Publishes a safe notice; successes never replace an error and
   * expire after three seconds. New errors replace earlier errors. */
  void publish_notice(std::string message, bool error = false);
  /** @brief Acknowledges the current notice without allocation. */
  void dismiss_notice() noexcept;
  void publish_permission_alert(std::string_view path,
                                const serial::DevicePermission &permission);

  /**
   * @brief Applies a path from the latest scan after identity and permission
   * reinspection. Hardware settings are mutable only while disconnected.
   */
  [[nodiscard]] Status apply_port(std::string_view value);
  /** @brief Applies a preset baud and asynchronously persists the snapshot. */
  [[nodiscard]] Status apply_baud(std::string_view value);
  /** @brief Applies a complete preset data-bits/parity/stop/flow format. */
  [[nodiscard]] Status apply_data_format(std::string_view value);
  /** @brief Applies the runtime TX newline setting. */
  [[nodiscard]] Status apply_newline(std::string_view value);
  /** @brief Applies RX/TX display modes and direction visibility together. */
  [[nodiscard]] Status apply_view(std::string_view value);
  /** @brief Applies TXT or HEX draft interpretation. */
  [[nodiscard]] Status apply_send_mode(std::string_view value);
  /**
   * @brief Applies the complete logging-settings candidate.
   * @note Status success means validation and save admission, not durable
   * commit. Rejection of persistence does not undo runtime settings.
   * NextSession preserves an active file; RotateNow closes and restarts it.
   */
  [[nodiscard]] Status
  apply_logging(std::string_view value,
                LogApplyPolicy policy = LogApplyPolicy::RotateNow);
  /** @brief Validates a complete logging-settings candidate without applying
   * it. */
  [[nodiscard]] Status validate_logging(std::string_view value) const;
  /**
   * @brief Validates and asynchronously saves one complete quick-send slot.
   * @param index User-facing slot number, 1 through 20.
   * @param slot Complete candidate with matching index, or nullopt to delete.
   * @note Visible quick-send state changes only after a committed completion.
   */
  [[nodiscard]] Status
  apply_quick_slot(std::uint32_t index,
                   std::optional<config::QuickSendSlot> slot);
  /**
   * @brief Starts one-shot or periodic execution from an immutable slot
   * snapshot.
   * @param slot User-facing quick-send slot in the range 1 through 20.
   * @param interval_ms Zero for one-shot, otherwise 10 through 86400000.
   * @param replacement_confirmed Whether an active task may enter stop/replace.
   */
  [[nodiscard]] Status execute_quick(std::uint32_t slot,
                                     std::uint64_t interval_ms,
                                     bool replacement_confirmed = false);
  /** @brief Requests the owner stop barrier for the active task generation. */
  void stop_quick_task();

  /**
   * @brief Finds stable record IDs whose current safe rendering contains query.
   * @param query Case-sensitive substring to find.
   * @param filter Directions eligible for matching.
   * @return At most 10000 IDs in record order.
   */
  [[nodiscard]] std::vector<std::uint64_t>
  search(std::string_view query, DirectionFilter filter = {}) const;
  /** @brief Renders one record through its terminal-safe direction projection.
   */
  [[nodiscard]] std::string render_record(const VisibleRecord &record) const;
  /** @brief Reports whether current user state warrants exit confirmation. */
  [[nodiscard]] bool has_exit_risk() const noexcept;
  /**
   * @brief Performs idempotent deadline-coordinated application shutdown.
   * @return true after graceful nonfatal shutdown, false after fatal
   * coordination.
   * @warning A worker that misses its shutdown deadline causes std::abort().
   */
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
  void set_notice(std::string message, bool error = true);
  [[nodiscard]] bool operations_allowed() const noexcept;
  [[nodiscard]] Status operation_rejected() const;
  void start_connection();
  void finish_failed_connection(const serial::ConnectCompletion &completion);
  void request_disconnect();
  void request_cancel_connect();
  void retry_cancel_race_disconnect();
  bool process_serial_data();
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
  void
  enqueue_record(RecordDirection direction, std::span<const std::byte> payload,
                 std::string message = {},
                 std::optional<ErrorCode> error_code = std::nullopt,
                 std::optional<OperationId> operation = std::nullopt,
                 SessionEventOrigin origin = SessionEventOrigin::Normal,
                 std::chrono::steady_clock::time_point observed_at =
                     std::chrono::steady_clock::now(),
                 std::chrono::system_clock::time_point time_utc =
                     std::chrono::system_clock::now(),
                 std::optional<config::SendMode> input_mode = std::nullopt);
  void enqueue_frames(std::vector<framing::RxFrame> frames,
                      SessionEventOrigin origin);
  [[nodiscard]] bool
  submit_tx(std::vector<std::byte> payload, config::SendMode input_mode,
            std::optional<TaskGeneration> task_generation,
            std::optional<scheduler::ScheduledRequestToken> scheduled_token =
                std::nullopt);
  Status save_config();
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
  std::chrono::system_clock::time_point session_started_utc_{};
  std::optional<std::chrono::steady_clock::time_point> notice_deadline_;
  std::string fatal_notice_{
      "Fatal worker signal; emergency shutdown requested"};
  std::optional<std::chrono::steady_clock::time_point> writer_stop_deadline_;
  std::optional<PendingLogCommand> log_command_;
  std::deque<PendingLogRecord> log_backlog_;
  std::size_t log_backlog_bytes_{};
  std::optional<LogSessionOwner> restart_log_owner_;
  std::optional<LogSessionOwner> processed_cleanup_;
  std::optional<LogSessionOwner> closed_log_owner_;
  std::optional<serial::DisconnectCompletion> deferred_disconnect_completion_;
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
  bool stopped_{};
};

} // namespace lazycom::app
