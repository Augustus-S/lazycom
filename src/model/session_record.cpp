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

[[nodiscard]] bool
valid_direction(const logging::Direction direction) noexcept {
  switch (direction) {
  case logging::Direction::Rx:
  case logging::Direction::Tx:
  case logging::Direction::Sys:
  case logging::Direction::Err:
    return true;
  }
  return false;
}

[[nodiscard]] bool valid_input_mode(const logging::InputMode mode) noexcept {
  switch (mode) {
  case logging::InputMode::Text:
  case logging::InputMode::Hex:
    return true;
  }
  return false;
}

} // namespace

SessionRecord::SessionRecord(const std::uint64_t seq, RecordDraft draft)
    : seq_(seq), direction_(draft.direction), time_(std::move(draft.time)),
      payload_(std::move(draft.payload)), input_mode_(draft.input_mode),
      message_(logging::sanitize_message(draft.message)),
      code_(std::move(draft.code)), operation_id_(draft.operation_id) {}

std::size_t SessionRecord::logical_bytes() const noexcept {
  auto result = sizeof(SessionRecord);
  result = saturating_add(result, payload_.size());
  result = saturating_add(result, message_.capacity());
  if (code_) {
    result = saturating_add(result, code_->capacity());
  }
  return result;
}

logging::Record SessionRecord::to_log_record() const {
  logging::Record output;
  output.seq = seq_;
  output.time_utc = time_.time_utc;
  output.elapsed_ns = time_.elapsed_ns;
  output.direction = direction_;
  output.input_mode = input_mode_;
  if (direction_ == logging::Direction::Rx ||
      direction_ == logging::Direction::Tx) {
    const auto source = payload_.bytes();
    output.payload.assign(source.begin(), source.end());
  } else {
    output.message = message_;
    output.code = code_;
  }
  return output;
}

SessionRecordBatch::SessionRecordBatch(const SessionId session_id,
                                       const std::uint64_t first_seq,
                                       std::vector<RecordDraft> drafts,
                                       BudgetReservation reservation)
    : session_id_(session_id), reservation_(std::move(reservation)) {
  records_.reserve(drafts.size());
  auto seq = first_seq;
  for (auto &draft : drafts) {
    records_.push_back(SessionRecord{seq, std::move(draft)});
    logical_bytes_ =
        saturating_add(logical_bytes_, records_.back().logical_bytes());
    ++seq;
  }
}

std::uint64_t SessionRecordBatch::first_seq() const noexcept {
  return records_.empty() ? 0U : records_.front().seq();
}

std::uint64_t SessionRecordBatch::last_seq() const noexcept {
  return records_.empty() ? 0U : records_.back().seq();
}

bool valid_record_draft(const RecordDraft &draft) noexcept {
  if (!logging::is_valid_utc(draft.time.time_utc) ||
      !valid_direction(draft.direction) ||
      (draft.input_mode && !valid_input_mode(*draft.input_mode))) {
    return false;
  }
  const bool payload_direction = draft.direction == logging::Direction::Rx ||
                                 draft.direction == logging::Direction::Tx;
  if (payload_direction) {
    return draft.payload.size() <= logging::kMaxPayloadBytes &&
           draft.message.empty() && !draft.code &&
           (!draft.input_mode || draft.direction == logging::Direction::Tx);
  }
  if (!draft.payload.empty() || draft.input_mode) {
    return false;
  }
  if (draft.direction == logging::Direction::Err) {
    return draft.code && stable_error_code(*draft.code);
  }
  return draft.direction == logging::Direction::Sys && !draft.code;
}

std::size_t
estimate_batch_metadata(const std::span<const RecordDraft> drafts) noexcept {
  auto result = sizeof(SessionRecordBatch);
  if (drafts.size() > (std::numeric_limits<std::size_t>::max() - result) /
                          sizeof(SessionRecord)) {
    return std::numeric_limits<std::size_t>::max();
  }
  result += drafts.size() * sizeof(SessionRecord);
  for (const auto &draft : drafts) {
    result = saturating_add(result, draft.time.time_utc.capacity());
    if (draft.direction == logging::Direction::Sys ||
        draft.direction == logging::Direction::Err) {
      result = saturating_add(result, draft.message.capacity());
      const auto maximum_output =
          std::min(logging::kMaxMessageBytes,
                   saturating_multiply(draft.message.size(), 4U));
      result = saturating_add(result, saturating_multiply(maximum_output, 2U));
    }
    if (draft.code) {
      result = saturating_add(result, draft.code->capacity());
    }
  }
  return result;
}

} // namespace lazycom::model
