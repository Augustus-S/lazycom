#pragma once

#include <lazycom/base/ids.hpp>

#include <cstdint>
#include <optional>

namespace lazycom {

enum class SessionEventOrigin : std::uint8_t {
  Normal,
  Cleanup,
};

enum class OperationOutcome : std::uint8_t {
  Succeeded,
  Failed,
  Cancelled,
  TimedOut,
};

struct ConnectCommand {
  OperationId operation_id{};
  ConnectionGeneration generation{};
};

struct CancelConnectCommand {
  OperationId operation_id{};
  ConnectionGeneration generation{};
};

struct DisconnectCommand {
  OperationId operation_id{};
  ConnectionGeneration generation{};
  std::optional<SessionId> session_id;
};

struct SendCommand {
  OperationId operation_id{};
  ConnectionGeneration generation{};
  SessionId session_id{};
  std::optional<TaskGeneration> task_generation;
};

struct StopTaskCommand {
  OperationId operation_id{};
  std::optional<TaskGeneration> generation;
  std::optional<OperationId> starting_operation{};
};

} // namespace lazycom
