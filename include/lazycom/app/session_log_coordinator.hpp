#pragma once
#include <deque>
#include <lazycom/app/state.hpp>
#include <lazycom/config/types.hpp>
#include <lazycom/logging/session_writer.hpp>

namespace lazycom::app {
struct LogSessionOwner {
  ConnectionGeneration generation{};
  SessionId session_id{};
  auto operator<=>(const LogSessionOwner &) const = default;
};

struct LogContext {
  const ConnectionStateMachine &connection;
  const config::ConfigSnapshot &config;
  std::optional<LogSessionOwner> cleanup;
  bool shutting_down{};
  bool allowed{};
};
struct LogNotice {
  std::string message;
  bool error{};
};

/** Owns writer lifecycle, rollover records, and the session file-close
 * boundary. */
class SessionLogCoordinator {
public:
  SessionLogCoordinator(const config::ConfigSnapshot &configuration,
                        std::filesystem::path default_directory,
                        model::GlobalMemoryBudget budget);
  void session_opened(LogSessionOwner owner, logging::Header header);
  [[nodiscard]] bool
  log_rollover_pending(const LogContext &context) const noexcept;
  void begin_log_session(const LogContext &context);
  void end_log_session(const LogContext &context,
                       ConnectionGeneration generation, SessionId session_id);
  void start_log_rollover_if_needed(const LogContext &context);
  void process_log_commands(const LogContext &context);
  void reconfigure_log_writer_if_inactive(const LogContext &context);
  void toggle_log(const LogContext &context);
  void enqueue(model::SessionRecordPtr record, const LogContext &context);
  void record_overflow();
  void observe_worker();
  [[nodiscard]] bool apply_settings(const LogContext &context, bool rotate_now);
  [[nodiscard]] LogState state() const noexcept;
  [[nodiscard]] std::size_t pending_records() const noexcept;
  [[nodiscard]] bool command_pending() const noexcept;
  [[nodiscard]] bool closed(LogSessionOwner owner) const noexcept;
  [[nodiscard]] std::optional<LogNotice> take_notice();
  [[nodiscard]] std::optional<FatalSignal> fatal_signal() const noexcept;
  void request_stop(std::chrono::milliseconds timeout) noexcept;
  [[nodiscard]] std::optional<std::chrono::steady_clock::time_point>
  stop_deadline() const noexcept;
  [[nodiscard]] bool wait_until_stopped(
      std::chrono::steady_clock::time_point deadline) const noexcept;
  [[nodiscard]] bool
  wait_command_until(std::chrono::steady_clock::time_point deadline) const;
  void finish_writer(std::uint64_t last_sequence,
                     std::chrono::milliseconds timeout);

private:
  void set_notice(std::string message, bool error = true);
  void record_fatal(FatalSignal signal) noexcept;
  struct PendingLogCommand {
    std::future<logging::SessionCommandResult> completion;
    enum class Kind : std::uint8_t { Start, End, Disable } kind{Kind::Start};
    std::optional<LogSessionOwner> owner;
    bool close_after_start{};
    std::chrono::steady_clock::time_point deadline;
  };

  struct PendingLogRecord {
    LogSessionOwner owner;
    model::SessionRecordPtr record;
    model::BudgetReservation reservation;
  };

  model::GlobalMemoryBudget memory_budget_;
  std::unique_ptr<logging::SessionWriter> writer_;
  std::filesystem::path default_log_directory_;
  LogStateMachine log_state_;
  std::optional<logging::Header> session_header_;
  std::optional<LogSessionOwner> session_owner_;
  std::optional<std::chrono::steady_clock::time_point> writer_stop_deadline_;
  std::optional<PendingLogCommand> log_command_;
  std::deque<PendingLogRecord> log_backlog_;
  std::size_t log_backlog_bytes_{};
  std::optional<LogSessionOwner> restart_log_owner_;
  std::optional<LogSessionOwner> closed_log_owner_;
  bool writer_reconfigure_pending_{};
  std::optional<LogNotice> notice_;
  std::optional<FatalSignal> fatal_;
};
} // namespace lazycom::app
