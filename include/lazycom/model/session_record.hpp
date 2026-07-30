#pragma once

#include <lazycom/base/ids.hpp>
#include <lazycom/logging/schema.hpp>
#include <lazycom/model/memory_budget.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace lazycom::model {

struct RecordTime {
  std::string time_utc;
  std::uint64_t elapsed_ns{};
  bool operator==(const RecordTime &) const = default;
};

struct RecordDraft {
  logging::Direction direction{logging::Direction::Rx};
  RecordTime time;
  ByteSlice payload;
  std::optional<logging::InputMode> input_mode;
  std::string message;
  std::optional<std::string> code;
  std::optional<OperationId> operation_id;
};

class SessionRecord {
public:
  [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }
  [[nodiscard]] logging::Direction direction() const noexcept {
    return direction_;
  }
  [[nodiscard]] const RecordTime &time() const noexcept { return time_; }
  [[nodiscard]] const ByteSlice &payload() const noexcept { return payload_; }
  [[nodiscard]] const std::optional<logging::InputMode> &input_mode() const
      noexcept {
    return input_mode_;
  }
  [[nodiscard]] const std::string &message() const noexcept { return message_; }
  [[nodiscard]] const std::optional<std::string> &code() const noexcept {
    return code_;
  }
  [[nodiscard]] const std::optional<OperationId> &operation_id() const noexcept {
    return operation_id_;
  }
  [[nodiscard]] std::size_t logical_bytes() const noexcept;
  [[nodiscard]] logging::Record to_log_record() const;

private:
  friend class SessionRecordBatch;
  SessionRecord(std::uint64_t seq, RecordDraft draft);

  std::uint64_t seq_{};
  logging::Direction direction_{logging::Direction::Rx};
  RecordTime time_;
  ByteSlice payload_;
  std::optional<logging::InputMode> input_mode_;
  std::string message_;
  std::optional<std::string> code_;
  std::optional<OperationId> operation_id_;
};

class SessionRecordBatch;
using SessionRecordBatchPtr = std::shared_ptr<const SessionRecordBatch>;

class SessionRecordBatch {
public:
  [[nodiscard]] SessionId session_id() const noexcept { return session_id_; }
  [[nodiscard]] std::span<const SessionRecord> records() const noexcept {
    return records_;
  }
  [[nodiscard]] std::uint64_t first_seq() const noexcept;
  [[nodiscard]] std::uint64_t last_seq() const noexcept;
  [[nodiscard]] std::size_t logical_bytes() const noexcept {
    return logical_bytes_;
  }
  [[nodiscard]] std::size_t metadata_budget_bytes() const noexcept {
    return reservation_.bytes();
  }

private:
  friend class SessionSequencer;
  SessionRecordBatch(SessionId session_id, std::uint64_t first_seq,
                     std::vector<RecordDraft> drafts,
                     BudgetReservation reservation);

  SessionId session_id_{};
  std::vector<SessionRecord> records_;
  std::size_t logical_bytes_{};
  BudgetReservation reservation_;
};

[[nodiscard]] bool valid_record_draft(const RecordDraft &draft) noexcept;
[[nodiscard]] std::size_t
estimate_batch_metadata(std::span<const RecordDraft> drafts) noexcept;

} // namespace lazycom::model
