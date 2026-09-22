#pragma once

#include <lazycom/model/session_record.hpp>
#include <lazycom/model/session_types.hpp>

#include <cstdint>

namespace lazycom::model {

enum class SequenceStatus {
  Accepted,
  InvalidOrigin,
  InvalidRecord,
  SequenceOverflow,
  BudgetExhausted,
  Closed,
};

struct SequenceResult {
  SequenceStatus status{SequenceStatus::Closed};
  SessionRecordPtr record;
};

class SessionSequencer {
public:
  // Main-thread admission only; immutable results may cross threads.
  explicit SessionSequencer(GlobalMemoryBudget &budget) : budget_(budget) {}

  // Budget rejection leaves the draft intact for eviction and retry.
  [[nodiscard]] SequenceResult submit(SessionEventOrigin origin,
                                      RecordDraft &draft);
  /** @brief Enters cleanup exactly once for the active session. */
  [[nodiscard]] bool begin_cleanup() noexcept;
  /** @brief Permanently closes the active or cleanup session. */
  [[nodiscard]] bool close() noexcept;
  [[nodiscard]] std::uint64_t next_seq() const noexcept;

private:
  enum class Phase { Active, Cleanup, Closed };

  GlobalMemoryBudget &budget_;
  std::uint64_t next_seq_{1U};
  Phase phase_{Phase::Active};
};

} // namespace lazycom::model
