#include <lazycom/base/text.hpp>
#include <lazycom/model/session_record.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace lazycom::model {
namespace {

[[nodiscard]] std::size_t saturating_add(const std::size_t left,
                                         const std::size_t right) noexcept {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return std::numeric_limits<std::size_t>::max();
  }
  return left + right;
}

[[nodiscard]] std::size_t
saturating_multiply(const std::size_t value,
                    const std::size_t multiplier) noexcept {
  if (value > std::numeric_limits<std::size_t>::max() / multiplier) {
    return std::numeric_limits<std::size_t>::max();
  }
  return value * multiplier;
}

[[nodiscard]] bool stable_error_code(const std::string &value) noexcept {
  if (!value.starts_with("LC-") || value.size() < 11U || value.size() > 32U) {
    return false;
  }
  const auto separator = value.find('-', 3U);
  if (separator == std::string::npos || separator < 6U ||
      separator + 5U != value.size()) {
    return false;
  }
  for (std::size_t index = 3U; index < separator; ++index) {
    if (value[index] < 'A' || value[index] > 'Z') {
      return false;
    }
  }
  for (std::size_t index = separator + 1U; index < value.size(); ++index) {
    if (value[index] < '0' || value[index] > '9') {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool valid_direction(const Direction direction) noexcept {
  switch (direction) {
  case Direction::Rx:
  case Direction::Tx:
  case Direction::Sys:
  case Direction::Err:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid_input_mode(const InputMode mode) noexcept {
  switch (mode) {
  case InputMode::Text:
  case InputMode::Hex:
    return true;
  }
  return false;
}

} // namespace

SessionRecord::SessionRecord(const std::uint64_t seq, RecordDraft draft,
                             BudgetReservation reservation)
    : reservation_(std::move(reservation)), seq_(seq),
      direction_(draft.direction), time_(std::move(draft.time)),
      payload_(draft.payload.begin(), draft.payload.end()),
      input_mode_(draft.input_mode),
      message_(lazycom::sanitize_message(draft.message)),
      code_(std::move(draft.code)), operation_id_(draft.operation_id) {}

std::size_t SessionRecord::logical_bytes() const noexcept {
  auto result = sizeof(SessionRecord);
  result = saturating_add(result, payload_.capacity());
  result = saturating_add(result, time_.time_utc.capacity());
  result = saturating_add(result, message_.capacity());
  if (code_) {
    result = saturating_add(result, code_->capacity());
  }
  return result;
}

bool valid_record_draft(const RecordDraft &draft) noexcept {
  if (!lazycom::is_valid_utc(draft.time.time_utc) ||
      !valid_direction(draft.direction) ||
      (draft.input_mode && !valid_input_mode(*draft.input_mode))) {
    return false;
  }
  const bool payload_direction =
      draft.direction == Direction::Rx || draft.direction == Direction::Tx;
  if (payload_direction) {
    return draft.payload.size() <= kMaxPayloadBytes && draft.message.empty() &&
           !draft.code &&
           (!draft.input_mode || draft.direction == Direction::Tx);
  }
  if (!draft.payload.empty() || draft.input_mode) {
    return false;
  }
  if (draft.direction == Direction::Err) {
    return draft.code && stable_error_code(*draft.code);
  }
  return draft.direction == Direction::Sys && !draft.code;
}

std::size_t estimate_record_memory(const RecordDraft &draft) noexcept {
  // Covers the shared object/control block and dynamic allocation metadata.
  auto result =
      saturating_add(sizeof(SessionRecord) + 128U, draft.payload.size());
  result = saturating_add(result, draft.time.time_utc.capacity());
  if (draft.direction == Direction::Sys || draft.direction == Direction::Err) {
    const auto maximum_output = std::min(
        kMaxMessageBytes, saturating_multiply(draft.message.size(), 4U));
    result = saturating_add(result, saturating_multiply(maximum_output, 2U));
  }
  if (draft.code)
    result = saturating_add(result, draft.code->capacity());
  return result;
}

} // namespace lazycom::model
