#pragma once

#include <lazycom/app/state.hpp>
#include <lazycom/model/bounded_queue.hpp>
#include <lazycom/model/session_record.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace lazycom::model {

struct SequenceGap {
  std::uint64_t first{};
  std::uint64_t last{};
  auto operator<=>(const SequenceGap &) const = default;
};

struct SinkEnvelope {
  SessionRecordBatchPtr batch;
  std::optional<SequenceGap> gap_before;
};

class IRecordSink {
public:
  virtual ~IRecordSink() = default;
  [[nodiscard]] virtual QueuePushResult
  push(const SinkEnvelope &envelope, std::size_t logical_bytes) = 0;
};

enum class SequenceStatus {
  Accepted,
  WrongSession,
  InvalidOrigin,
  InvalidRecord,
  EmptyBatch,
  SequenceOverflow,
  BudgetExhausted,
  Closed,
};

struct SinkDelivery {
  QueuePushResult result{QueuePushResult::Stopped};
  // On Accepted this gap was delivered with the batch; otherwise it remains
  // pending for this sink.
  std::optional<SequenceGap> gap;
};

struct SequenceResult {
  SequenceStatus status{SequenceStatus::Closed};
  std::uint64_t first_seq{};
  std::uint64_t last_seq{};
  SessionRecordBatchPtr batch;
  SinkDelivery ui;
  SinkDelivery log;
};

class SessionSequencer {
public:
  SessionSequencer(SessionId session_id, GlobalMemoryBudget &budget,
                   IRecordSink *ui_sink, IRecordSink *log_sink,
                   BudgetCategory batch_category =
                       BudgetCategory::OwnerScratch);

  [[nodiscard]] SequenceResult
  submit(SessionId session_id, app::SessionEventOrigin origin,
         std::vector<RecordDraft> drafts);
  [[nodiscard]] bool begin_cleanup(SessionId session_id) noexcept;
  [[nodiscard]] bool close(SessionId session_id) noexcept;
  [[nodiscard]] std::uint64_t next_seq() const noexcept;

private:
  enum class Phase { Active, Cleanup, Closed };

  [[nodiscard]] SinkDelivery deliver(IRecordSink *sink,
                                     std::optional<SequenceGap> &pending,
                                     const SessionRecordBatchPtr &batch);
  static void extend_gap(std::optional<SequenceGap> &gap,
                         std::uint64_t first, std::uint64_t last) noexcept;

  SessionId session_id_{};
  GlobalMemoryBudget &budget_;
  IRecordSink *ui_sink_{};
  IRecordSink *log_sink_{};
  BudgetCategory batch_category_{BudgetCategory::OwnerScratch};
  mutable std::mutex mutex_;
  std::uint64_t next_seq_{1U};
  Phase phase_{Phase::Active};
  std::optional<SequenceGap> ui_gap_;
  std::optional<SequenceGap> log_gap_;
};

} // namespace lazycom::model
