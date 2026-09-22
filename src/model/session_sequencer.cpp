#include <lazycom/model/session_sequencer.hpp>

#include <limits>
#include <utility>

namespace lazycom::model {

SequenceResult SessionSequencer::submit(SessionEventOrigin origin,
                                        RecordDraft &draft) {
  SequenceResult result;
  if (phase_ == Phase::Closed) {
    result.status = SequenceStatus::Closed;
    return result;
  }
  const bool origin_allowed =
      (phase_ == Phase::Active && origin == SessionEventOrigin::Normal) ||
      (phase_ == Phase::Cleanup && origin == SessionEventOrigin::Cleanup);
  if (!origin_allowed) {
    result.status = SequenceStatus::InvalidOrigin;
    return result;
  }
  if (!valid_record_draft(draft)) {
    result.status = SequenceStatus::InvalidRecord;
    return result;
  }
  if (next_seq_ == std::numeric_limits<std::uint64_t>::max()) {
    result.status = SequenceStatus::SequenceOverflow;
    return result;
  }

  auto reservation = budget_.try_reserve(BudgetCategory::UiRecords,
                                         estimate_record_memory(draft));
  if (!reservation) {
    result.status = SequenceStatus::BudgetExhausted;
    return result;
  }
  result.record = std::make_shared<SessionRecord>(next_seq_, std::move(draft),
                                                  std::move(*reservation));
  ++next_seq_;
  result.status = SequenceStatus::Accepted;
  return result;
}

bool SessionSequencer::begin_cleanup() noexcept {
  if (phase_ != Phase::Active) {
    return false;
  }
  phase_ = Phase::Cleanup;
  return true;
}

bool SessionSequencer::close() noexcept {
  if (phase_ == Phase::Closed) {
    return false;
  }
  phase_ = Phase::Closed;
  return true;
}

std::uint64_t SessionSequencer::next_seq() const noexcept { return next_seq_; }

} // namespace lazycom::model
