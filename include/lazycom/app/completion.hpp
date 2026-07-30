#pragma once

#include <lazycom/app/state.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <variant>
#include <vector>

namespace lazycom::app {

struct SendCompleted {
  OperationId operation_id{};
  ConnectionGeneration generation{};
  SessionId session_id{};
  OperationOutcome outcome{OperationOutcome::Succeeded};
};

struct TaskStopped {
  OperationId operation_id{};
  TaskGeneration generation{};
  OperationOutcome outcome{OperationOutcome::Succeeded};
};

struct ScanCompleted {
  OperationId operation_id{};
  ScanGeneration scan_generation{};
  OperationOutcome outcome{OperationOutcome::Succeeded};
};

struct SaveCompleted {
  OperationId operation_id{};
  OperationOutcome outcome{OperationOutcome::Succeeded};
};

struct LogBarrierCompleted {
  OperationId operation_id{};
  SessionId session_id{};
  std::uint64_t processed_through_sequence{};
  OperationOutcome outcome{OperationOutcome::Succeeded};
};

using CompletionEvent =
    std::variant<ConnectSucceeded, ConnectFailed, SendCompleted,
                 DisconnectCompleted, TaskStopped, ScanCompleted, SaveCompleted,
                 LogBarrierCompleted>;

static_assert(std::is_trivially_copyable_v<CompletionEvent>);

[[nodiscard]] inline OperationId
operation_id(const CompletionEvent &event) noexcept {
  return std::visit(
      [](const auto &value) noexcept { return value.operation_id; }, event);
}

enum class OperationClass : std::uint8_t {
  Normal,
  Tx,
  Control,
};

enum class CompletionKind : std::uint8_t {
  Connect,
  Send,
  Disconnect,
  TaskStop,
  Scan,
  Save,
  LogBarrier,
};

[[nodiscard]] inline CompletionKind
completion_kind(const CompletionEvent &event) noexcept {
  return std::visit(
      [](const auto &value) noexcept {
        using Value = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, ConnectSucceeded> ||
                      std::is_same_v<Value, ConnectFailed>) {
          return CompletionKind::Connect;
        } else if constexpr (std::is_same_v<Value, SendCompleted>) {
          return CompletionKind::Send;
        } else if constexpr (std::is_same_v<Value, DisconnectCompleted>) {
          return CompletionKind::Disconnect;
        } else if constexpr (std::is_same_v<Value, TaskStopped>) {
          return CompletionKind::TaskStop;
        } else if constexpr (std::is_same_v<Value, ScanCompleted>) {
          return CompletionKind::Scan;
        } else if constexpr (std::is_same_v<Value, SaveCompleted>) {
          return CompletionKind::Save;
        } else {
          return CompletionKind::LogBarrier;
        }
      },
      event);
}

struct CompletionCapacities {
  std::size_t normal{};
  std::size_t tx{};
  std::size_t control{};
};

enum class ReserveCompletionResult : std::uint8_t {
  Reserved,
  Duplicate,
  Full,
  InvalidOperation,
};

enum class CompleteOperationResult : std::uint8_t {
  Completed,
  DuplicateIgnored,
  UnknownIgnored,
  UnexpectedKind,
};

class CompletionTracker {
public:
  explicit CompletionTracker(const CompletionCapacities capacities)
      : normal_slots_(capacities.normal), tx_slots_(capacities.tx),
        control_slots_(capacities.control) {}

  [[nodiscard]] std::size_t
  capacity(OperationClass operation_class) const noexcept;
  [[nodiscard]] std::size_t capacity() const noexcept;
  [[nodiscard]] std::size_t reserved_count() const noexcept;
  [[nodiscard]] std::size_t completed_count() const noexcept;

  [[nodiscard]] ReserveCompletionResult
  reserve(OperationId operation, OperationClass operation_class,
          CompletionKind expected_kind) noexcept;
  [[nodiscard]] bool cancel_reservation(OperationId operation) noexcept;
  [[nodiscard]] CompleteOperationResult
  complete(const CompletionEvent &event) noexcept;
  [[nodiscard]] const CompletionEvent *
  peek(OperationId operation) const noexcept;
  [[nodiscard]] std::optional<CompletionEvent>
  consume(OperationId operation) noexcept;

private:
  enum class SlotState : std::uint8_t {
    Free,
    Reserved,
    Completed,
  };

  struct Slot {
    OperationId operation_id{};
    CompletionKind expected_kind{CompletionKind::Connect};
    SlotState state{SlotState::Free};
    std::optional<CompletionEvent> completion;
  };

  [[nodiscard]] Slot *find(OperationId operation) noexcept;
  [[nodiscard]] const Slot *find(OperationId operation) const noexcept;
  [[nodiscard]] std::vector<Slot> &
  slots(OperationClass operation_class) noexcept;
  [[nodiscard]] const std::vector<Slot> &
  slots(OperationClass operation_class) const noexcept;

  std::vector<Slot> normal_slots_;
  std::vector<Slot> tx_slots_;
  std::vector<Slot> control_slots_;
};

} // namespace lazycom::app
