#pragma once

#include <lazycom/app/state.hpp>

#include <cstdint>
#include <type_traits>
#include <variant>

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

/**
 * @brief Terminal value for one admitted asynchronous application operation.
 *
 * The carrying mailbox, not this variant, provides reliable delivery and must
 * reserve capacity before admission. Consumers match lifecycle results by
 * operation ID and must ignore stale terminal events without mutating a newer
 * generation or session.
 */
using CompletionEvent =
    std::variant<ConnectSucceeded, ConnectFailed, SendCompleted,
                 DisconnectCompleted, TaskStopped, ScanCompleted, SaveCompleted,
                 LogBarrierCompleted>;

static_assert(std::is_trivially_copyable_v<CompletionEvent>);

/** @brief Returns the operation identity shared by every completion variant. */
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

/**
 * @brief Classifies a completion independently of its success/failure variant.
 * @note ConnectSucceeded and ConnectFailed both map to CompletionKind::Connect.
 */
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

} // namespace lazycom::app
