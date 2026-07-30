#include <lazycom/scheduler/scheduler.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace lazycom::scheduler {
namespace {

[[nodiscard]] bool is_hex_digit(const char value) noexcept {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f') ||
         (value >= 'A' && value <= 'F');
}

[[nodiscard]] unsigned int hex_value(const char value) noexcept {
  if (value >= '0' && value <= '9') {
    return static_cast<unsigned int>(value - '0');
  }
  if (value >= 'a' && value <= 'f') {
    return static_cast<unsigned int>(value - 'a') + 10U;
  }
  return static_cast<unsigned int>(value - 'A') + 10U;
}

[[nodiscard]] bool is_hex_space(const char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

[[nodiscard]] bool is_valid_utf8(const std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto first = static_cast<unsigned char>(text[index]);
    if (first <= 0x7FU) {
      ++index;
      continue;
    }

    std::size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if ((first & 0xE0U) == 0xC0U) {
      continuation_count = 1;
      code_point = first & 0x1FU;
      minimum = 0x80U;
    } else if ((first & 0xF0U) == 0xE0U) {
      continuation_count = 2;
      code_point = first & 0x0FU;
      minimum = 0x800U;
    } else if ((first & 0xF8U) == 0xF0U) {
      continuation_count = 3;
      code_point = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (continuation_count > text.size() - index - 1U) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (continuation & 0x3FU);
    }
    if (code_point < minimum || code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
      return false;
    }
    index += continuation_count + 1U;
  }
  return true;
}

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

  if (mode == config::SendMode::Txt) {
    if (!is_valid_utf8(content)) {
      result.status = PayloadParseStatus::InvalidText;
      return result;
    }
    if (content.size() > config::kMaximumPayloadBytes - suffix_size) {
      result.status = PayloadParseStatus::TooLarge;
      return result;
    }
    result.bytes.reserve(content.size() + suffix_size);
    for (const char value : content) {
      result.bytes.push_back(static_cast<std::byte>(value));
    }
  } else {
    std::size_t index = 0;
    result.bytes.reserve(
        std::min(content.size() / 2U, config::kMaximumPayloadBytes));
    while (true) {
      while (index < content.size() && is_hex_space(content[index])) {
        ++index;
      }
      if (index == content.size()) {
        break;
      }
      if (index + 1U < content.size() && content[index] == '0' &&
          (content[index + 1U] == 'x' || content[index + 1U] == 'X')) {
        index += 2U;
      }
      if (index + 1U >= content.size() || !is_hex_digit(content[index]) ||
          !is_hex_digit(content[index + 1U])) {
        result.bytes.clear();
        result.status = PayloadParseStatus::InvalidHex;
        return result;
      }
      if (result.bytes.size() >= config::kMaximumPayloadBytes - suffix_size) {
        result.bytes.clear();
        result.status = PayloadParseStatus::TooLarge;
        return result;
      }
      const auto value =
          (hex_value(content[index]) << 4U) | hex_value(content[index + 1U]);
      result.bytes.push_back(static_cast<std::byte>(value));
      index += 2U;
      if (index < content.size() && !is_hex_space(content[index])) {
        result.bytes.clear();
        result.status = PayloadParseStatus::InvalidHex;
        return result;
      }
    }
  }

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

Scheduler::Scheduler(const TaskGeneration last_issued) noexcept
    : generations_(last_issued) {}

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

std::optional<TaskGeneration> Scheduler::activate(TaskRequest request,
                                                  const TimePoint now) {
  auto execution =
      std::make_shared<const QuickSendExecution>(std::move(request.execution));
  TaskGeneration issued;
  if (generations_.issue(issued) == IdIncrementResult::Overflow) {
    return std::nullopt;
  }
  state_ = SchedulerState::Running;
  generation_ = issued;
  execution_ = std::move(execution);
  interval_ms_ = request.interval_ms;
  next_deadline_.reset();
  if (interval_ms_ != 0U) {
    const auto period = std::chrono::milliseconds{interval_ms_};
    if (now <= Scheduler::TimePoint::max() - period) {
      next_deadline_ = now + period;
    }
  }
  outstanding_.reset();
  next_sequence_ = 1U;
  sent_count_ = 0U;
  missed_count_ = 0U;
  trigger_pending_ = true;
  return issued;
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

  const auto generation = activate(std::move(request), now);
  if (!generation) {
    return {TaskStartStatus::GenerationOverflow, std::nullopt, std::nullopt};
  }
  return {TaskStartStatus::Started, generation, std::nullopt};
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
  trigger_pending_ = false;
  sent_count_ = 0U;
  missed_count_ = 0U;

  if (!pending_replacement_) {
    return {StopConfirmationStatus::Stopped, std::nullopt};
  }
  auto replacement = std::move(*pending_replacement_);
  pending_replacement_.reset();
  const auto started = activate(std::move(replacement), now);
  if (!started) {
    return {StopConfirmationStatus::GenerationOverflow, std::nullopt};
  }
  return {StopConfirmationStatus::ReplacementStarted, started};
}

void Scheduler::add_missed(const std::uint64_t count) noexcept {
  missed_count_ = saturated_add(missed_count_, count);
}

void Scheduler::on_deadline(const TimePoint now) noexcept {
  if (state_ != SchedulerState::Running || !next_deadline_ ||
      now < *next_deadline_) {
    return;
  }

  const auto period = std::chrono::milliseconds{interval_ms_};
  const auto elapsed = now - *next_deadline_;
  const auto elapsed_periods = elapsed / period;
  const auto due_count = static_cast<std::uint64_t>(elapsed_periods) + 1U;

  const auto phase = elapsed % period;
  const auto until_next =
      phase == Clock::duration::zero() ? period : period - phase;
  if (now <= TimePoint::max() - until_next) {
    next_deadline_ = now + until_next;
  } else {
    next_deadline_.reset();
  }

  if (trigger_pending_ || outstanding_) {
    add_missed(due_count);
    return;
  }
  trigger_pending_ = true;
  add_missed(due_count - 1U);
}

std::optional<ScheduledSend> Scheduler::emit_pending() {
  if (state_ != SchedulerState::Running || !generation_ || !execution_ ||
      !trigger_pending_ || outstanding_) {
    return std::nullopt;
  }
  if (next_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
    trigger_pending_ = false;
    state_ = SchedulerState::Stopping;
    next_deadline_.reset();
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
  on_deadline(now);

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
