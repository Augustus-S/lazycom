#include <lazycom/model/session_sequencer.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace lazycom::model {

SessionSequencer::SessionSequencer(const SessionId session_id,
                                   GlobalMemoryBudget &budget,
                                   IRecordSink *const ui_sink,
                                   IRecordSink *const log_sink,
                                   const BudgetCategory batch_category)
    : session_id_(session_id), budget_(budget), ui_sink_(ui_sink),
      log_sink_(log_sink), batch_category_(batch_category) {}

void SessionSequencer::extend_gap(std::optional<SequenceGap> &gap,
                                  const std::uint64_t first,
                                  const std::uint64_t last) noexcept {
  if (!gap) {
    gap = SequenceGap{first, last};
    return;
  }
  gap->first = std::min(gap->first, first);
  gap->last = std::max(gap->last, last);
}

SinkDelivery
SessionSequencer::deliver(IRecordSink *const sink,
                          std::optional<SequenceGap> &pending,
                          const SessionRecordBatchPtr &batch) {
  if (sink == nullptr) {
    return SinkDelivery{QueuePushResult::Stopped, pending};
  }
  const auto attached_gap = pending;
  const auto result = sink->push(SinkEnvelope{batch, attached_gap},
                                 batch->logical_bytes());
  if (result == QueuePushResult::Accepted) {
    pending.reset();
    return SinkDelivery{result, attached_gap};
  }
  extend_gap(pending, batch->first_seq(), batch->last_seq());
  return SinkDelivery{result, pending};
}

SequenceResult
SessionSequencer::submit(const SessionId session_id,
                         const app::SessionEventOrigin origin,
                         std::vector<RecordDraft> drafts) {
  std::lock_guard lock{mutex_};
  SequenceResult result;
  if (session_id != session_id_) {
    result.status = SequenceStatus::WrongSession;
    return result;
  }
  if (phase_ == Phase::Closed) {
    result.status = SequenceStatus::Closed;
    return result;
  }
  const bool origin_allowed =
      (phase_ == Phase::Active && origin == app::SessionEventOrigin::Normal) ||
      (phase_ == Phase::Cleanup && origin == app::SessionEventOrigin::Cleanup);
  if (!origin_allowed) {
    result.status = SequenceStatus::InvalidOrigin;
    return result;
  }
  if (drafts.empty()) {
    result.status = SequenceStatus::EmptyBatch;
    return result;
  }
  if (!std::ranges::all_of(drafts, valid_record_draft)) {
    result.status = SequenceStatus::InvalidRecord;
    return result;
  }
  if (drafts.size() >
      std::numeric_limits<std::uint64_t>::max() - next_seq_) {
    result.status = SequenceStatus::SequenceOverflow;
    return result;
  }

  const auto metadata = estimate_batch_metadata(drafts);
  if (metadata > std::numeric_limits<std::size_t>::max() - 64U) {
    result.status = SequenceStatus::BudgetExhausted;
    return result;
  }
  auto reservation = budget_.try_reserve(batch_category_, metadata + 64U);
  if (!reservation) {
    result.status = SequenceStatus::BudgetExhausted;
    return result;
  }
  const auto first = next_seq_;
  auto mutable_batch = std::shared_ptr<SessionRecordBatch>{
      new SessionRecordBatch{session_id_, first, std::move(drafts),
                             std::move(*reservation)}};
  const SessionRecordBatchPtr batch = std::move(mutable_batch);
  next_seq_ = batch->last_seq() + 1U;

  result.status = SequenceStatus::Accepted;
  result.first_seq = batch->first_seq();
  result.last_seq = batch->last_seq();
  result.batch = batch;
  // A full or stopped sink never prevents the other sink from being tried.
  result.ui = deliver(ui_sink_, ui_gap_, batch);
  result.log = deliver(log_sink_, log_gap_, batch);
  return result;
}

bool SessionSequencer::begin_cleanup(const SessionId session_id) noexcept {
  std::lock_guard lock{mutex_};
  if (session_id != session_id_ || phase_ != Phase::Active) {
    return false;
  }
  phase_ = Phase::Cleanup;
  return true;
}

bool SessionSequencer::close(const SessionId session_id) noexcept {
  std::lock_guard lock{mutex_};
  if (session_id != session_id_ || phase_ == Phase::Closed) {
    return false;
  }
  phase_ = Phase::Closed;
  return true;
}

std::uint64_t SessionSequencer::next_seq() const noexcept {
  std::lock_guard lock{mutex_};
  return next_seq_;
}

} // namespace lazycom::model
