#pragma once

#include <lazycom/base/ids.hpp>
#include <lazycom/model/memory_budget.hpp>
#include <lazycom/model/record_types.hpp>
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

// Payload is borrowed only until admission finishes.
struct RecordDraft {
  Direction direction{Direction::Rx};
  RecordTime time;
  std::span<const std::byte> payload;
  std::optional<InputMode> input_mode;
  std::string message;
  std::optional<std::string> code;
  std::optional<OperationId> operation_id;
};

// One immutable record owns both bytes and their budget until its last reader.
class SessionRecord {
public:
  // The caller reserves estimate_record_memory(draft) before construction.
  SessionRecord(std::uint64_t seq, RecordDraft draft,
                BudgetReservation reservation);
  [[nodiscard]] std::uint64_t seq() const noexcept { return seq_; }
  [[nodiscard]] Direction direction() const noexcept { return direction_; }
  [[nodiscard]] const RecordTime &time() const noexcept { return time_; }
  [[nodiscard]] std::span<const std::byte> payload() const noexcept {
    return payload_;
  }
  [[nodiscard]] const std::optional<InputMode> &input_mode() const noexcept {
    return input_mode_;
  }
  [[nodiscard]] const std::string &message() const noexcept { return message_; }
  [[nodiscard]] const std::optional<std::string> &code() const noexcept {
    return code_;
  }
  [[nodiscard]] const std::optional<OperationId> &
  operation_id() const noexcept {
    return operation_id_;
  }
  [[nodiscard]] std::size_t logical_bytes() const noexcept;

private:
  BudgetReservation reservation_;
  std::uint64_t seq_{};
  Direction direction_{Direction::Rx};
  RecordTime time_;
  std::vector<std::byte> payload_;
  std::optional<InputMode> input_mode_;
  std::string message_;
  std::optional<std::string> code_;
  std::optional<OperationId> operation_id_;
};
using SessionRecordPtr = std::shared_ptr<const SessionRecord>;

[[nodiscard]] bool valid_record_draft(const RecordDraft &draft) noexcept;
[[nodiscard]] std::size_t
estimate_record_memory(const RecordDraft &draft) noexcept;

} // namespace lazycom::model
