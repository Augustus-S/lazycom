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

/**
 * @brief Nonblocking sink interface used during sequenced fan-out.
 *
 * envelope is borrowed for the call. A sink that retains it must copy the
 * shared batch pointer and gap value. Implementations must not re-enter the
 * originating SessionSequencer because delivery occurs under its mutex.
 */
class IRecordSink {
public:
  virtual ~IRecordSink() = default;
  [[nodiscard]] virtual QueuePushResult push(const SinkEnvelope &envelope,
                                             std::size_t logical_bytes) = 0;
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
  /**
   * @brief Creates a sequencer for one session and two optional borrowed sinks.
   * @param session_id Session accepted by submit() and lifecycle operations.
   * @param budget Borrowed budget that must outlive all sequencer calls.
   * @param ui_sink Optional borrowed UI sink.
   * @param log_sink Optional borrowed logging sink.
   * @param batch_category Category charged for batch metadata.
   */
  SessionSequencer(
      SessionId session_id, GlobalMemoryBudget &budget, IRecordSink *ui_sink,
      IRecordSink *log_sink,
      BudgetCategory batch_category = BudgetCategory::OwnerScratch);

  /**
   * @brief Validates, sequences, constructs, and independently fans out a
   * batch.
   * @return Admission status, contiguous sequence range, immutable batch, and
   * per-sink delivery results. Sequence acceptance remains successful even when
   * a sink is full or stopped; that sink receives a gap on a later success.
   * @note Active phase accepts Normal origin and cleanup phase accepts Cleanup
   * origin. Rejection consumes no sequence numbers.
   */
  [[nodiscard]] SequenceResult submit(SessionId session_id,
                                      app::SessionEventOrigin origin,
                                      std::vector<RecordDraft> drafts);
  /** @brief Enters cleanup exactly once for the matching active session. */
  [[nodiscard]] bool begin_cleanup(SessionId session_id) noexcept;
  /** @brief Permanently closes the matching active or cleanup session. */
  [[nodiscard]] bool close(SessionId session_id) noexcept;
  [[nodiscard]] std::uint64_t next_seq() const noexcept;

private:
  enum class Phase { Active, Cleanup, Closed };

  [[nodiscard]] SinkDelivery deliver(IRecordSink *sink,
                                     std::optional<SequenceGap> &pending,
                                     const SessionRecordBatchPtr &batch);
  static void extend_gap(std::optional<SequenceGap> &gap, std::uint64_t first,
                         std::uint64_t last) noexcept;

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
