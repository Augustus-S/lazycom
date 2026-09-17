#include <lazycom/scheduler/scheduler.hpp>

#include <lazycom/encoding/tx.hpp>

#include <limits>
#include <type_traits>
#include <utility>

namespace lazycom::scheduler {
namespace {

[[nodiscard]] std::optional<config::Newline>
resolve_newline(const config::Newline policy,
                const config::Newline session_newline) noexcept {
  const auto resolved =
      policy == config::Newline::Session ? session_newline : policy;
  switch (resolved) {
  case config::Newline::None:
  case config::Newline::Lf:
  case config::Newline::Cr:
  case config::Newline::CrLf:
    return resolved;
  case config::Newline::Session:
    return std::nullopt;
  }
  return std::nullopt;
}

[[nodiscard]] std::size_t newline_size(const config::Newline newline) noexcept {
  switch (newline) {
  case config::Newline::None:
    return 0U;
  case config::Newline::Lf:
  case config::Newline::Cr:
    return 1U;
  case config::Newline::CrLf:
    return 2U;
  case config::Newline::Session:
    return 0U;
  }
  return 0U;
}

void append_newline(std::vector<std::byte> &bytes,
                    const config::Newline newline) {
  switch (newline) {
  case config::Newline::None:
    break;
  case config::Newline::Lf:
    bytes.push_back(std::byte{'\n'});
    break;
  case config::Newline::Cr:
    bytes.push_back(std::byte{'\r'});
    break;
  case config::Newline::CrLf:
    bytes.push_back(std::byte{'\r'});
    bytes.push_back(std::byte{'\n'});
    break;
  case config::Newline::Session:
    break;
  }
}

[[nodiscard]] bool valid_mode(const config::SendMode mode) noexcept {
  return mode == config::SendMode::Txt || mode == config::SendMode::Hex;
}

[[nodiscard]] bool
valid_newline_policy(const config::Newline newline) noexcept {
  switch (newline) {
  case config::Newline::None:
  case config::Newline::Lf:
  case config::Newline::Cr:
  case config::Newline::CrLf:
  case config::Newline::Session:
    return true;
  }
  return false;
}

[[nodiscard]] std::uint64_t saturated_add(const std::uint64_t left,
                                          const std::uint64_t right) noexcept {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  return right > maximum - left ? maximum : left + right;
}

[[nodiscard]] PayloadParseStatus
parse_status(const encoding::TxParseErrorCode code) noexcept {
  switch (code) {
  case encoding::TxParseErrorCode::InvalidUtf8:
    return PayloadParseStatus::InvalidText;
  case encoding::TxParseErrorCode::InvalidHex:
    return PayloadParseStatus::InvalidHex;
  case encoding::TxParseErrorCode::LimitExceeded:
    return PayloadParseStatus::TooLarge;
  }
  return PayloadParseStatus::InvalidMode;
}

[[nodiscard]] bool try_add_duration(const Scheduler::TimePoint start,
                                    const Scheduler::Clock::duration duration,
                                    Scheduler::TimePoint &result) noexcept {
  if (duration < Scheduler::Clock::duration::zero() ||
      start > Scheduler::TimePoint::max() - duration) {
    return false;
  }
  result = start + duration;
  return true;
}

} // namespace

PayloadParseResult parse_payload(const config::SendMode mode,
                                 const std::string_view content,
                                 const config::Newline newline,
                                 const config::Newline session_newline) {
  PayloadParseResult result;
  if (!valid_mode(mode)) {
    result.status = PayloadParseStatus::InvalidMode;
    return result;
  }
  const auto effective_newline = resolve_newline(newline, session_newline);
  if (!effective_newline) {
    result.status = PayloadParseStatus::InvalidNewline;
    return result;
  }
  const auto suffix_size = newline_size(*effective_newline);

  const auto maximum = config::kMaximumPayloadBytes - suffix_size;
  encoding::ParsedTxBytes parsed;
  if (mode == config::SendMode::Txt) {
    // Preserve scheduler's invalid-text-before-size error precedence.
    if (!encoding::is_strict_utf8(content)) {
      result.status = PayloadParseStatus::InvalidText;
      return result;
    }
    parsed = encoding::parse_text(content, maximum);
  } else {
    parsed = encoding::parse_hex(content, maximum);
  }
  if (!parsed) {
    result.status = parse_status(parsed.error().code);
    return result;
  }

  result.bytes = std::move(*parsed);
  append_newline(result.bytes, *effective_newline);
  return result;
}

QuickSendBuildResult
make_quick_send_execution(const config::QuickSendSnapshot &slots,
                          const std::uint32_t slot_index,
                          const config::Newline session_newline) {
  QuickSendBuildResult result;
  if (slot_index < 1U || slot_index > slots.slots.size()) {
    result.status = QuickSendBuildStatus::SlotOutOfRange;
    return result;
  }
  const auto &optional_slot = slots.slots[slot_index - 1U];
  if (!optional_slot) {
    result.status = QuickSendBuildStatus::EmptySlot;
    return result;
  }
  const auto &slot = *optional_slot;
  if (slot.index != slot_index) {
    result.status = QuickSendBuildStatus::InvalidSlot;
    return result;
  }

  auto parsed =
      parse_payload(slot.mode, slot.content, slot.newline, session_newline);
  result.payload_status = parsed.status;
  if (!parsed) {
    result.status = QuickSendBuildStatus::InvalidPayload;
    return result;
  }
  const auto effective_newline = resolve_newline(slot.newline, session_newline);
  if (!effective_newline) {
    result.status = QuickSendBuildStatus::InvalidPayload;
    result.payload_status = PayloadParseStatus::InvalidNewline;
    return result;
  }

  result.execution = QuickSendExecution{slot_index,
                                        slot.name,
                                        slot.mode,
                                        slot.newline,
                                        *effective_newline,
                                        slot.note,
                                        std::move(parsed.bytes)};
  return result;
}

Scheduler::Scheduler(const TaskGeneration last_issued,
                     const std::uint64_t first_request_sequence) noexcept
    : generations_(last_issued),
      first_request_sequence_(
          first_request_sequence == 0U ? 1U : first_request_sequence),
      next_sequence_(first_request_sequence_) {}

bool Scheduler::valid_execution(
    const QuickSendExecution &execution) const noexcept {
  return execution.slot_index >= 1U && execution.slot_index <= 20U &&
         valid_mode(execution.mode) &&
         valid_newline_policy(execution.newline) &&
         execution.effective_newline != config::Newline::Session &&
         resolve_newline(execution.effective_newline, config::Newline::None)
             .has_value() &&
         execution.bytes.size() <= config::kMaximumPayloadBytes;
}

TaskStartResult Scheduler::activate(TaskRequest request, const TimePoint now) {
  std::optional<TimePoint> deadline;
  if (request.interval_ms != 0U) {
    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::milliseconds{request.interval_ms});
    TimePoint candidate;
    if (period <= Clock::duration::zero() ||
        !try_add_duration(now, period, candidate)) {
      return {TaskStartStatus::DeadlineOverflow, std::nullopt, std::nullopt};
    }
    deadline = candidate;
  }
  auto execution =
      std::make_shared<const QuickSendExecution>(std::move(request.execution));
  TaskGeneration issued;
  if (generations_.issue(issued) == IdIncrementResult::Overflow) {
    return {TaskStartStatus::GenerationOverflow, std::nullopt, std::nullopt};
  }
  state_ = SchedulerState::Running;
  generation_ = issued;
  execution_ = std::move(execution);
  interval_ms_ = request.interval_ms;
  next_deadline_ = deadline;
  outstanding_.reset();
  automatic_stop_request_.reset();
  next_sequence_ = first_request_sequence_;
  sent_count_ = 0U;
  missed_count_ = 0U;
  trigger_pending_ = true;
  return {TaskStartStatus::Started, issued, std::nullopt};
}

TaskStartResult Scheduler::start(TaskRequest request, const TimePoint now,
                                 const bool replacement_confirmed) {
  if (!valid_interval(request.interval_ms)) {
    return {TaskStartStatus::InvalidInterval, std::nullopt, std::nullopt};
  }
  if (!valid_execution(request.execution)) {
    return {TaskStartStatus::InvalidExecution, std::nullopt, std::nullopt};
  }
  if (state_ == SchedulerState::Stopping) {
    return {TaskStartStatus::StopInProgress, std::nullopt, generation_};
  }
  if (state_ == SchedulerState::Running) {
    if (!replacement_confirmed) {
      return {TaskStartStatus::ReplacementConfirmationRequired, std::nullopt,
              generation_};
    }
    pending_replacement_ = std::move(request);
    const auto invalidated = invalidate(false);
    return {TaskStartStatus::ReplacementStopRequested, std::nullopt,
            invalidated.generation_to_stop};
  }

  return activate(std::move(request), now);
}

TaskInvalidationResult
Scheduler::invalidate(const bool discard_replacement) noexcept {
  if (discard_replacement) {
    pending_replacement_.reset();
  }
  if (state_ == SchedulerState::Idle) {
    return {TaskInvalidationStatus::Idle, std::nullopt};
  }
  if (state_ == SchedulerState::Stopping) {
    return {TaskInvalidationStatus::AlreadyStopping, generation_};
  }
  state_ = SchedulerState::Stopping;
  trigger_pending_ = false;
  next_deadline_.reset();
  automatic_stop_request_.reset();
  return {TaskInvalidationStatus::StopRequested, generation_};
}

TaskInvalidationResult Scheduler::request_stop() noexcept {
  return invalidate(true);
}

TaskInvalidationResult Scheduler::invalidate_for_disconnect() noexcept {
  return invalidate(true);
}

TaskInvalidationResult Scheduler::invalidate_for_shutdown() noexcept {
  return invalidate(true);
}

StopConfirmationResult
Scheduler::confirm_stopped(const TaskGeneration generation,
                           const TimePoint now) {
  if (state_ != SchedulerState::Stopping || generation_ != generation) {
    return {StopConfirmationStatus::IgnoredStale, std::nullopt};
  }

  state_ = SchedulerState::Idle;
  generation_.reset();
  execution_.reset();
  interval_ms_ = 0U;
  next_deadline_.reset();
  outstanding_.reset();
  automatic_stop_request_.reset();
  trigger_pending_ = false;
  sent_count_ = 0U;
  missed_count_ = 0U;

  if (!pending_replacement_) {
    return {StopConfirmationStatus::Stopped, std::nullopt};
  }
  auto replacement = std::move(*pending_replacement_);
  pending_replacement_.reset();
  const auto started = activate(std::move(replacement), now);
  if (started.status == TaskStartStatus::GenerationOverflow) {
    return {StopConfirmationStatus::GenerationOverflow, std::nullopt};
  }
  if (started.status == TaskStartStatus::DeadlineOverflow) {
    return {StopConfirmationStatus::DeadlineOverflow, std::nullopt};
  }
  return {StopConfirmationStatus::ReplacementStarted,
          started.started_generation};
}

void Scheduler::add_missed(const std::uint64_t count) noexcept {
  missed_count_ = saturated_add(missed_count_, count);
}

void Scheduler::on_deadline(const TimePoint now,
                            const bool writer_busy) noexcept {
  if (state_ != SchedulerState::Running || !next_deadline_ ||
      now < *next_deadline_) {
    return;
  }

  using DurationRep = Clock::duration::rep;
  using UnsignedRep = std::make_unsigned_t<DurationRep>;
  static_assert(std::is_integral_v<DurationRep>);
  static_assert(std::numeric_limits<UnsignedRep>::digits <=
                std::numeric_limits<std::uint64_t>::digits);

  const auto period = std::chrono::duration_cast<Clock::duration>(
      std::chrono::milliseconds{interval_ms_});
  const auto period_ticks = static_cast<UnsignedRep>(period.count());
  const auto now_ticks =
      static_cast<UnsignedRep>(now.time_since_epoch().count());
  const auto deadline_ticks =
      static_cast<UnsignedRep>(next_deadline_->time_since_epoch().count());
  const auto elapsed_ticks = now_ticks - deadline_ticks;
  const auto elapsed_periods =
      static_cast<std::uint64_t>(elapsed_ticks / period_ticks);
  const auto due_count =
      elapsed_periods == std::numeric_limits<std::uint64_t>::max()
          ? elapsed_periods
          : elapsed_periods + 1U;

  const auto phase_ticks = elapsed_ticks % period_ticks;
  const auto until_next_ticks =
      phase_ticks == 0U ? period_ticks : period_ticks - phase_ticks;
  const auto until_next =
      Clock::duration{static_cast<DurationRep>(until_next_ticks)};
  TimePoint next;
  if (!try_add_duration(now, until_next, next)) {
    add_missed(due_count);
    begin_automatic_stop(AutomaticStopReason::DeadlineOverflow);
    return;
  }
  next_deadline_ = next;

  if (trigger_pending_ || outstanding_ || writer_busy) {
    add_missed(due_count);
    return;
  }
  trigger_pending_ = true;
  add_missed(due_count - 1U);
}

void Scheduler::begin_automatic_stop(
    const AutomaticStopReason reason) noexcept {
  if (state_ != SchedulerState::Running || !generation_) {
    return;
  }
  state_ = SchedulerState::Stopping;
  trigger_pending_ = false;
  next_deadline_.reset();
  automatic_stop_request_ = AutomaticStopRequest{*generation_, reason};
}

std::optional<ScheduledSend> Scheduler::emit_pending() {
  if (state_ != SchedulerState::Running || !generation_ || !execution_ ||
      !trigger_pending_ || outstanding_) {
    return std::nullopt;
  }
  if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    begin_automatic_stop(AutomaticStopReason::SequenceOverflow);
    return std::nullopt;
  }
  const ScheduledRequestToken token{*generation_, next_sequence_};
  ++next_sequence_;
  trigger_pending_ = false;
  outstanding_ = token;
  return ScheduledSend{token, execution_};
}

std::optional<ScheduledSend>
Scheduler::on_tx_boundary(const TimePoint now, const TxBoundary &boundary) {
  // A deadline reached while the previous request was active is missed, even
  // when its completion is observed at this same owner boundary.
  on_deadline(now, boundary.manual_pending);

  if (state_ != SchedulerState::Running) {
    return std::nullopt;
  }
  if (boundary.completed_request &&
      outstanding_ == boundary.completed_request) {
    outstanding_.reset();
    if (boundary.completed_successfully) {
      sent_count_ = saturated_add(sent_count_, 1U);
    }
    if (interval_ms_ == 0U) {
      state_ = SchedulerState::Idle;
      generation_.reset();
      execution_.reset();
      return std::nullopt;
    }
  }
  if (boundary.manual_pending) {
    return std::nullopt;
  }
  return emit_pending();
}

std::optional<Scheduler::TimePoint> Scheduler::next_deadline() const noexcept {
  return state_ == SchedulerState::Running ? next_deadline_ : std::nullopt;
}

std::optional<AutomaticStopRequest>
Scheduler::take_automatic_stop_request() noexcept {
  return std::exchange(automatic_stop_request_, std::nullopt);
}

SchedulerSnapshot Scheduler::snapshot() const noexcept {
  SchedulerSnapshot result;
  result.state = state_;
  result.generation = generation_;
  result.interval_ms = interval_ms_;
  result.sent_count = sent_count_;
  result.missed_count = missed_count_;
  result.trigger_pending = trigger_pending_;
  result.outstanding = outstanding_.has_value();
  if (execution_) {
    result.slot_index = execution_->slot_index;
  }
  return result;
}

bool Scheduler::accepts(const TaskGeneration generation) const noexcept {
  return state_ == SchedulerState::Running && generation_ == generation;
}

} // namespace lazycom::scheduler
