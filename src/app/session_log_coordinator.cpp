#include "messages.hpp"
#include <algorithm>
#include <lazycom/app/session_log_coordinator.hpp>
#include <stdexcept>
#include <utility>

namespace lazycom::app {
namespace {
using namespace std::chrono_literals;
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
log_record_bytes(const model::SessionRecordPtr &record) noexcept {
  return record->logical_bytes() + 128U;
}

} // namespace

SessionLogCoordinator::SessionLogCoordinator(
    const config::ConfigSnapshot &configuration,
    std::filesystem::path default_directory, model::GlobalMemoryBudget budget)
    : memory_budget_{std::move(budget)},
      writer_{std::make_unique<logging::SessionWriter>(
          writer_options(configuration, default_directory),
          logging::make_linux_session_log_file_system(), memory_budget_)},
      default_log_directory_{std::move(default_directory)} {
  if (configuration.logging.default_enabled && writer_->enable()) {
    static_cast<void>(log_state_.enable(false));
  }
}

void SessionLogCoordinator::session_opened(LogSessionOwner owner,
                                           logging::Header header) {
  session_header_ = std::move(header);
  session_owner_ = owner;
  closed_log_owner_.reset();
}

bool SessionLogCoordinator::log_rollover_pending(
    const LogContext &context) const noexcept {
  return restart_log_owner_ && context.connection.generation() &&
         context.connection.session_id() &&
         *restart_log_owner_ ==
             LogSessionOwner{*context.connection.generation(),
                             *context.connection.session_id()};
}

void SessionLogCoordinator::begin_log_session(const LogContext &context) {
  const bool finishing_rollover = log_rollover_pending(context);
  if (!(context.allowed && !fatal_) ||
      log_state_.state() != LogState::Waiting || log_command_ ||
      writer_reconfigure_pending_ || writer_stop_deadline_ ||
      (context.shutting_down && !finishing_rollover) ||
      (context.connection.state() != ConnectionState::Connected &&
       !(finishing_rollover &&
         context.connection.state() == ConnectionState::Disconnecting)) ||
      !context.connection.generation() || !context.connection.session_id() ||
      !session_header_) {
    return;
  }
  const LogSessionOwner owner{*context.connection.generation(),
                              *context.connection.session_id()};
  if (session_owner_ != owner) {
    record_fatal({ErrorCode::InternalInvariantBroken,
                  Operation::CoordinateFatal, WorkerKind::SessionLog,
                  FatalReason::InvariantBroken,
                  SignalSourceLocation::current()});
    return;
  }
  log_command_.emplace(PendingLogCommand{
      writer_->start_session(*session_header_), PendingLogCommand::Kind::Start,
      owner, context.cleanup == owner,
      std::chrono::steady_clock::now() +
          std::chrono::milliseconds{context.config.timeouts.log_barrier_ms}});
  restart_log_owner_.reset();
}

void SessionLogCoordinator::end_log_session(
    const LogContext &context, const ConnectionGeneration generation,
    const SessionId session_id) {
  const LogSessionOwner owner{generation, session_id};
  if (closed_log_owner_ == owner) {
    return;
  }
  if (log_command_) {
    if (log_command_->kind == PendingLogCommand::Kind::Start &&
        log_command_->owner == owner) {
      log_command_->close_after_start = true;
    }
    if (!log_command_->owner) {
      log_command_->owner = owner;
    }
    return;
  }
  if (log_rollover_pending(context) && writer_reconfigure_pending_ &&
      log_state_.state() == LogState::Waiting) {
    reconfigure_log_writer_if_inactive(context);
    return;
  }
  if (writer_stop_deadline_) {
    const auto now = std::chrono::steady_clock::now();
    if (!writer_->wait_until_stopped(now)) {
      if (now >= *writer_stop_deadline_) {
        abort_after_shutdown_timeout();
      }
      return;
    }
    if (const auto fatal = writer_->fatal_signal()) {
      record_fatal(*fatal);
      return;
    }
  }
  if (!writer_stop_deadline_ && (log_state_.state() == LogState::Recording ||
                                 log_state_.state() == LogState::Error)) {
    log_command_.emplace(PendingLogCommand{
        log_state_.state() == LogState::Error ? writer_->disable()
                                              : writer_->end_session(),
        PendingLogCommand::Kind::End, owner, false,
        std::chrono::steady_clock::now() +
            std::chrono::milliseconds{context.config.timeouts.log_barrier_ms}});
    static_cast<void>(log_state_.connection_closed());
  } else if (context.cleanup == owner) {
    if (!log_backlog_.empty()) {
      log_backlog_.clear();
      log_backlog_bytes_ = 0U;
      static_cast<void>(log_state_.fail());
      set_notice("Session ended during log rollover; pending records could not "
                 "be logged");
    }
    closed_log_owner_ = owner;
  }
}

void SessionLogCoordinator::start_log_rollover_if_needed(
    const LogContext &context) {
  if (!restart_log_owner_ || !writer_reconfigure_pending_ || log_command_ ||
      log_state_.state() != LogState::Recording ||
      (context.connection.state() != ConnectionState::Connected &&
       context.connection.state() != ConnectionState::Disconnecting) ||
      !context.connection.generation() || !context.connection.session_id() ||
      *restart_log_owner_ !=
          LogSessionOwner{*context.connection.generation(),
                          *context.connection.session_id()}) {
    return;
  }
  end_log_session(context, restart_log_owner_->generation,
                  restart_log_owner_->session_id);
}

void SessionLogCoordinator::process_log_commands(const LogContext &context) {
  if (const auto fatal = writer_->fatal_signal()) {
    record_fatal(*fatal);
    return;
  }
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
  if (const auto fatal = writer_->fatal_signal()) {
    record_fatal(*fatal);
    return;
  }
  const auto kind = log_command_->kind;
  const auto owner = log_command_->owner;
  const bool close_after_start = log_command_->close_after_start;
  log_command_.reset();
  const bool failed = result.error ||
                      result.state == logging::SessionLogState::Error ||
                      (kind == PendingLogCommand::Kind::Start &&
                       result.state != logging::SessionLogState::Recording) ||
                      (kind != PendingLogCommand::Kind::Start &&
                       result.state == logging::SessionLogState::Recording);
  if (failed) {
    static_cast<void>(log_state_.fail());
    log_backlog_.clear();
    log_backlog_bytes_ = 0U;
    restart_log_owner_.reset();
    set_notice(result.error ? error_text(*result.error) : "Session log failed");
    // A rejected command can return a ready future before any close occurred.
    // Stop drains accepted work and proves cleanup at the worker return point.
    if (!writer_stop_deadline_) {
      writer_stop_deadline_ =
          std::chrono::steady_clock::now() +
          std::chrono::milliseconds{context.config.timeouts.log_barrier_ms};
      writer_->request_stop();
    }
    writer_reconfigure_pending_ = true;
  } else if (kind == PendingLogCommand::Kind::Start) {
    const bool owner_is_current =
        owner &&
        (context.connection.state() == ConnectionState::Connected ||
         context.connection.state() == ConnectionState::Disconnecting) &&
        context.connection.generation() && context.connection.session_id() &&
        *owner == LogSessionOwner{*context.connection.generation(),
                                  *context.connection.session_id()};
    if (!close_after_start && owner_is_current &&
        log_state_.state() != LogState::Error) {
      static_cast<void>(log_state_.connection_opened());
    }
    std::deque<PendingLogRecord> retained;
    std::size_t retained_bytes = 0U;
    bool queue_failed = false;
    while (!log_backlog_.empty()) {
      auto pending = std::move(log_backlog_.front());
      log_backlog_.pop_front();
      const auto pending_bytes = log_record_bytes(pending.record);
      log_backlog_bytes_ -= pending_bytes;
      if (!owner || pending.owner != *owner) {
        retained_bytes += pending_bytes;
        retained.push_back(std::move(pending));
        continue;
      }
      if (writer_->try_enqueue(std::move(pending.record)) !=
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
    if (close_after_start || !owner_is_current || queue_failed ||
        log_state_.state() == LogState::Error) {
      log_command_.emplace(PendingLogCommand{
          log_state_.state() == LogState::Error ? writer_->disable()
                                                : writer_->end_session(),
          PendingLogCommand::Kind::End, owner, false,
          std::chrono::steady_clock::now() +
              std::chrono::milliseconds{
                  context.config.timeouts.log_barrier_ms}});
    }
  } else if (kind == PendingLogCommand::Kind::End) {
    // end_session() moved the application to WAITING at submission time.
  } else if (result.state == logging::SessionLogState::Waiting) {
    // Observe flush/close failure before resetting the worker's error state.
    log_command_.emplace(PendingLogCommand{
        writer_->disable(), PendingLogCommand::Kind::Disable, owner, false,
        std::chrono::steady_clock::now() +
            std::chrono::milliseconds{context.config.timeouts.log_barrier_ms}});
  } else {
    static_cast<void>(log_state_.disable());
  }
  if (!failed && kind != PendingLogCommand::Kind::Start && !log_command_ &&
      owner && context.cleanup == owner && !log_rollover_pending(context)) {
    if (!log_backlog_.empty()) {
      log_backlog_.clear();
      log_backlog_bytes_ = 0U;
      static_cast<void>(log_state_.fail());
      set_notice("Session ended during log rollover; pending records could not "
                 "be logged");
    }
    closed_log_owner_ = owner;
  } else if (context.cleanup && !log_command_) {
    end_log_session(context, context.cleanup->generation,
                    context.cleanup->session_id);
  }

  reconfigure_log_writer_if_inactive(context);
  if (!context.shutting_down) {
    start_log_rollover_if_needed(context);
    if (!log_command_ && log_state_.state() == LogState::Waiting &&
        context.connection.state() == ConnectionState::Connected) {
      if (!writer_reconfigure_pending_) {
        restart_log_owner_.reset();
        begin_log_session(context);
      }
    }
  }
}

void SessionLogCoordinator::reconfigure_log_writer_if_inactive(
    const LogContext &context) {
  const bool finishing_rollover = log_rollover_pending(context);
  if (!writer_reconfigure_pending_ || log_command_ ||
      (context.shutting_down && !finishing_rollover) ||
      !(context.allowed && !fatal_) ||
      log_state_.state() == LogState::Recording) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (!writer_stop_deadline_) {
    if (context.connection.state() == ConnectionState::Disconnecting &&
        !finishing_rollover) {
      return;
    }
    writer_stop_deadline_ =
        now + std::chrono::milliseconds{context.config.timeouts.log_barrier_ms};
    writer_->request_stop();
  }
  if (!writer_->wait_until_stopped(now)) {
    if (now >= *writer_stop_deadline_) {
      abort_after_shutdown_timeout();
    }
    return;
  }
  if (const auto fatal = writer_->fatal_signal()) {
    record_fatal(*fatal);
    return;
  }
  if (writer_->state() == logging::SessionLogState::Error &&
      log_state_.state() != LogState::Error) {
    if (log_state_.state() == LogState::Off) {
      static_cast<void>(log_state_.enable(false));
    }
    static_cast<void>(log_state_.fail());
    set_notice("Session log failed while stopping the previous writer");
  }
  if (context.connection.state() == ConnectionState::Disconnecting &&
      !finishing_rollover) {
    if (context.cleanup) {
      end_log_session(context, context.cleanup->generation,
                      context.cleanup->session_id);
    }
    return;
  }
  try {
    // The old worker is at its nonblocking return point before a new one
    // exists.
    writer_ = std::make_unique<logging::SessionWriter>(
        writer_options(context.config, default_log_directory_),
        logging::make_linux_session_log_file_system(), memory_budget_);
    writer_stop_deadline_.reset();
    writer_reconfigure_pending_ = false;
    if (log_state_.state() == LogState::Waiting && !writer_->enable()) {
      static_cast<void>(log_state_.fail());
      set_notice("Replacement log writer could not enter WAITING");
    }

    if (log_state_.state() != LogState::Waiting) {
      restart_log_owner_.reset();
    }
    begin_log_session(context);
  } catch (const std::bad_alloc &) {
    record_fatal({ErrorCode::InternalOutOfMemory, Operation::CoordinateFatal,
                  WorkerKind::SessionLog, FatalReason::OutOfMemory,
                  SignalSourceLocation::current()});
  } catch (const std::exception &exception) {
    writer_reconfigure_pending_ = false;
    if (log_state_.state() == LogState::Off) {
      static_cast<void>(log_state_.enable(false));
    }
    static_cast<void>(log_state_.fail());
    log_backlog_.clear();
    log_backlog_bytes_ = 0U;
    restart_log_owner_.reset();

    set_notice(std::string{"Cannot apply logging settings: "} +
               exception.what());
  }
}

void SessionLogCoordinator::toggle_log(const LogContext &context) {
  if (!(context.allowed && !fatal_)) {
    set_notice("Application is stopping; logging command rejected");
    return;
  }
  if (context.connection.state() == ConnectionState::Disconnecting) {
    set_notice("Logging changes are locked while disconnecting");
    return;
  }
  if (writer_stop_deadline_ && !writer_reconfigure_pending_ &&
      log_state_.state() == LogState::Error) {
    static_cast<void>(log_state_.disable());

    writer_reconfigure_pending_ = true;
    reconfigure_log_writer_if_inactive(context);
    return;
  }
  if (log_command_ || writer_stop_deadline_) {
    set_notice("A log state transition is still in progress", false);
    return;
  }
  if (log_state_.state() == LogState::Error) {
    log_backlog_.clear();
    log_backlog_bytes_ = 0U;
    log_command_.emplace(PendingLogCommand{
        writer_->disable(), PendingLogCommand::Kind::Disable, std::nullopt,
        false,
        std::chrono::steady_clock::now() +
            std::chrono::milliseconds{context.config.timeouts.log_barrier_ms}});
    set_notice("Resetting log error to OFF...", false);
    return;
  }
  if (log_state_.state() == LogState::Off) {
    static_cast<void>(log_state_.enable(false));
    if (!writer_->enable()) {
      static_cast<void>(log_state_.fail());
      set_notice("Log writer could not enter WAITING");
      return;
    }

    if (context.connection.state() == ConnectionState::Connected) {
      begin_log_session(context);
    }
    set_notice(context.connection.state() == ConnectionState::Connected
                   ? "Starting session logging..."
                   : "Session logging is WAITING for a connection",
               false);
    return;
  }
  log_backlog_.clear();
  log_backlog_bytes_ = 0U;
  restart_log_owner_.reset();
  log_command_.emplace(PendingLogCommand{
      log_state_.state() == LogState::Recording ? writer_->end_session()
                                                : writer_->disable(),
      PendingLogCommand::Kind::Disable, std::nullopt, false,
      std::chrono::steady_clock::now() +
          std::chrono::milliseconds{context.config.timeouts.log_barrier_ms}});
  set_notice("Stopping session logging...", false);
}

void SessionLogCoordinator::enqueue(model::SessionRecordPtr log_record,
                                    const LogContext &context) {
  const bool disabling =
      log_command_ && log_command_->kind == PendingLogCommand::Kind::Disable;
  if (log_state_.state() == LogState::Recording && !disabling) {
    const auto result = writer_->try_enqueue(std::move(log_record));
    if (result != logging::EnqueueResult::Accepted) {
      static_cast<void>(log_state_.fail());
      set_notice("Session log stopped because its queue rejected a record");
    }
  } else if (!disabling && log_state_.state() == LogState::Waiting &&
             context.connection.generation() &&
             context.connection.session_id() &&
             (context.connection.state() == ConnectionState::Connected ||
              context.connection.state() == ConnectionState::Disconnecting ||
              context.connection.state() == ConnectionState::Error)) {
    const auto record_bytes = log_record_bytes(log_record);
    const auto log_maximum_bytes =
        static_cast<std::size_t>(context.config.queues.log_max_mib) * 1024U *
        1024U;
    auto reservation = memory_budget_.try_reserve(
        model::BudgetCategory::SessionLog, sizeof(PendingLogRecord) + 128U);
    if (reservation &&
        log_backlog_.size() < context.config.queues.log_max_messages &&
        record_bytes <= log_maximum_bytes -
                            std::min(log_backlog_bytes_, log_maximum_bytes)) {
      log_backlog_bytes_ += record_bytes;
      log_backlog_.push_back(
          {{*context.connection.generation(), *context.connection.session_id()},
           std::move(log_record),
           std::move(*reservation)});
    } else {
      static_cast<void>(log_state_.fail());
      log_backlog_.clear();
      log_backlog_bytes_ = 0U;
      restart_log_owner_.reset();
      set_notice("Session log stopped because its rollover backlog is full");
    }
  }
}

void SessionLogCoordinator::record_overflow() {
  if (log_state_.state() != LogState::Recording &&
      log_state_.state() != LogState::Waiting) {
    return;
  }
  writer_->fail_overload();
  static_cast<void>(log_state_.fail());
  log_backlog_.clear();
  log_backlog_bytes_ = 0U;
  restart_log_owner_.reset();
  set_notice("Session log stopped because the record memory limit was reached");
}

void SessionLogCoordinator::observe_worker() {
  if (const auto fatal = writer_->fatal_signal()) {
    record_fatal(*fatal);
    return;
  }
  if (writer_->state() == logging::SessionLogState::Error &&
      log_state_.state() != LogState::Error) {
    if (log_state_.state() == LogState::Off) {
      static_cast<void>(log_state_.enable(false));
    }
    static_cast<void>(log_state_.fail());
    log_backlog_.clear();
    log_backlog_bytes_ = 0U;
    restart_log_owner_.reset();

    set_notice("Session log stopped after an asynchronous writer failure");
  }
}

bool SessionLogCoordinator::apply_settings(const LogContext &context,
                                           bool rotate_now) {
  writer_reconfigure_pending_ = true;
  if (!log_rollover_pending(context)) {
    restart_log_owner_.reset();
  }
  if (rotate_now) {
    if (log_command_ && log_command_->kind == PendingLogCommand::Kind::Start &&
        log_command_->owner) {
      restart_log_owner_ = log_command_->owner;
    } else if (log_state_.state() == LogState::Recording &&
               context.connection.generation() &&
               context.connection.session_id()) {
      restart_log_owner_ = LogSessionOwner{*context.connection.generation(),
                                           *context.connection.session_id()};
    }
  }
  reconfigure_log_writer_if_inactive(context);
  const bool rotates_active_file = rotate_now && restart_log_owner_.has_value();
  if (rotate_now) {
    start_log_rollover_if_needed(context);
  }
  return rotates_active_file;
}

LogState SessionLogCoordinator::state() const noexcept {
  return log_state_.state();
}
std::size_t SessionLogCoordinator::pending_records() const noexcept {
  return writer_->queued_records() + log_backlog_.size();
}
bool SessionLogCoordinator::command_pending() const noexcept {
  return log_command_.has_value();
}
bool SessionLogCoordinator::closed(LogSessionOwner owner) const noexcept {
  return closed_log_owner_ == owner;
}
std::optional<FatalSignal>
SessionLogCoordinator::fatal_signal() const noexcept {
  return fatal_ ? fatal_ : writer_->fatal_signal();
}
void SessionLogCoordinator::record_fatal(FatalSignal signal) noexcept {
  if (!fatal_) {
    fatal_ = signal;
  }
}
void SessionLogCoordinator::set_notice(std::string message, bool error) {
  if (error || !notice_ || !notice_->error) {
    notice_ = LogNotice{std::move(message), error};
  }
}
std::optional<LogNotice> SessionLogCoordinator::take_notice() {
  return std::exchange(notice_, std::nullopt);
}
void SessionLogCoordinator::request_stop(
    std::chrono::milliseconds timeout) noexcept {
  if (!writer_stop_deadline_) {
    writer_stop_deadline_ = std::chrono::steady_clock::now() + timeout;
  }
  writer_->request_stop();
}
std::optional<std::chrono::steady_clock::time_point>
SessionLogCoordinator::stop_deadline() const noexcept {
  return writer_stop_deadline_;
}
bool SessionLogCoordinator::wait_until_stopped(
    std::chrono::steady_clock::time_point deadline) const noexcept {
  return writer_->wait_until_stopped(deadline);
}
bool SessionLogCoordinator::wait_command_until(
    std::chrono::steady_clock::time_point deadline) const {
  return !log_command_ || log_command_->completion.wait_until(deadline) ==
                              std::future_status::ready;
}
void SessionLogCoordinator::finish_writer(std::uint64_t last_sequence,
                                          std::chrono::milliseconds timeout) {
  if (writer_->state() == logging::SessionLogState::Recording) {
    auto barrier = writer_->barrier(last_sequence);
    if (barrier.wait_for(timeout) != std::future_status::ready) {
      abort_after_shutdown_timeout();
    }
    auto ended = writer_->end_session();
    if (ended.wait_for(timeout) != std::future_status::ready) {
      abort_after_shutdown_timeout();
    }
  }
  request_stop(timeout);
  if (!wait_until_stopped(*writer_stop_deadline_)) {
    abort_after_shutdown_timeout();
  }
}

} // namespace lazycom::app
