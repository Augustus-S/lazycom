#pragma once

#include <lazycom/app/signals.hpp>
#include <lazycom/base/ids.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <variant>

namespace lazycom::app {

enum class StateChange : std::uint8_t {
  Applied,
  NoChange,
  IgnoredStale,
  InvalidTransition,
  CapacityExceeded,
  Unauthorized,
};

enum class ConnectionState : std::uint8_t {
  Disconnected,
  Connecting,
  Connected,
  Error,
  Disconnecting,
};

enum class InteractionState : std::uint8_t {
  Normal,
  SendEdit,
  ReceiveBrowse,
};

enum class OverlayKind : std::uint8_t {
  None,
  Search,
  Modal,
  Confirm,
  ErrorDialog,
  Help,
  CommandPalette,
};

enum class LogState : std::uint8_t {
  Off,
  Waiting,
  Recording,
  Error,
};

enum class SessionEventOrigin : std::uint8_t {
  Normal,
  Cleanup,
};

enum class SessionEventKind : std::uint8_t {
  Rx,
  Tx,
  System,
  Error,
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
  TaskGeneration generation{};
};

struct ScanCommand {
  OperationId operation_id{};
  ScanGeneration scan_generation{};
};

struct SaveCommand {
  OperationId operation_id{};
};

struct LogBarrierCommand {
  OperationId operation_id{};
  SessionId session_id{};
  std::uint64_t target_sequence{};
};

using AppCommand = std::variant<ConnectCommand, CancelConnectCommand,
                                DisconnectCommand, SendCommand, StopTaskCommand,
                                ScanCommand, SaveCommand, LogBarrierCommand>;

struct ConnectSucceeded {
  OperationId operation_id{};
  ConnectionGeneration generation{};
  SessionId session_id{};
};

struct ConnectFailed {
  OperationId operation_id{};
  ConnectionGeneration generation{};
};

struct DisconnectCompleted {
  OperationId operation_id{};
  ConnectionGeneration generation{};
  std::optional<SessionId> session_id;
  OperationOutcome outcome{OperationOutcome::Succeeded};
};

struct SessionDataEvent {
  ConnectionGeneration generation{};
  SessionId session_id{};
  SessionEventOrigin origin{SessionEventOrigin::Normal};
  SessionEventKind kind{SessionEventKind::Rx};
};

struct SessionFaultEvent {
  ConnectionGeneration generation{};
  SessionId session_id{};
};

using AppEvent =
    std::variant<ConnectSucceeded, ConnectFailed, DisconnectCompleted,
                 SessionDataEvent, SessionFaultEvent, FatalSignal,
                 WorkerStoppedSignal>;

class ConnectionStateMachine {
public:
  [[nodiscard]] ConnectionState state() const noexcept { return state_; }
  [[nodiscard]] std::optional<ConnectionGeneration>
  generation() const noexcept {
    return generation_;
  }
  [[nodiscard]] std::optional<SessionId> session_id() const noexcept {
    return session_id_;
  }
  [[nodiscard]] std::optional<OperationId> connect_operation() const noexcept {
    return connect_operation_;
  }
  [[nodiscard]] std::optional<OperationId> cleanup_operation() const noexcept {
    return cleanup_operation_;
  }

  [[nodiscard]] StateChange
  begin_connect(const ConnectCommand &command) noexcept;
  [[nodiscard]] StateChange
  connect_succeeded(const ConnectSucceeded &event) noexcept;
  [[nodiscard]] StateChange connect_failed(const ConnectFailed &event) noexcept;
  [[nodiscard]] StateChange
  cancel_connect(const CancelConnectCommand &command) noexcept;
  [[nodiscard]] StateChange
  begin_disconnect(const DisconnectCommand &command) noexcept;
  [[nodiscard]] StateChange
  session_fault(const SessionFaultEvent &event) noexcept;
  [[nodiscard]] StateChange
  disconnect_completed(const DisconnectCompleted &event) noexcept;

  [[nodiscard]] bool accepts(const SendCommand &command) const noexcept;
  [[nodiscard]] bool accepts(const SessionDataEvent &event) const noexcept;

  [[nodiscard]] static bool transition_allowed(ConnectionState from,
                                               ConnectionState to) noexcept;

private:
  [[nodiscard]] bool
  matches_generation(ConnectionGeneration generation) const noexcept;
  [[nodiscard]] bool
  matches_session(const std::optional<SessionId> &session) const noexcept;

  ConnectionState state_{ConnectionState::Disconnected};
  ConnectionGeneration last_generation_{};
  std::optional<ConnectionGeneration> generation_;
  std::optional<SessionId> session_id_;
  std::optional<OperationId> connect_operation_;
  std::optional<OperationId> cleanup_operation_;
};

class InteractionStateMachine {
public:
  [[nodiscard]] InteractionState state() const noexcept { return state_; }
  [[nodiscard]] StateChange enter(InteractionState state) noexcept;
  void reset_to_normal() noexcept { state_ = InteractionState::Normal; }

private:
  InteractionState state_{InteractionState::Normal};
};

struct OverlayFrame {
  OverlayKind kind{OverlayKind::None};
  std::uint32_t focus{};
  std::array<std::uint64_t, 4> field_state{};
  InteractionState interaction{InteractionState::Normal};
  auto operator<=>(const OverlayFrame &) const = default;
};

struct OverlayCloseResult {
  StateChange change{StateChange::InvalidTransition};
  InteractionState interaction{InteractionState::Normal};
};

class OverlayStack {
public:
  static constexpr std::size_t capacity = 8;

  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
  [[nodiscard]] const OverlayFrame *active() const noexcept;
  [[nodiscard]] StateChange open(const OverlayFrame &frame) noexcept;
  [[nodiscard]] OverlayCloseResult
  toggle_help(const OverlayFrame &frame) noexcept;
  [[nodiscard]] StateChange update_active(const OverlayFrame &frame) noexcept;
  [[nodiscard]] OverlayCloseResult close_top() noexcept;

private:
  std::array<OverlayFrame, capacity> frames_{};
  std::size_t size_{};
};

class LogStateMachine {
public:
  [[nodiscard]] LogState state() const noexcept { return state_; }
  [[nodiscard]] StateChange transition(LogState next) noexcept;
  [[nodiscard]] StateChange enable(bool connected) noexcept;
  [[nodiscard]] StateChange disable() noexcept;
  [[nodiscard]] StateChange connection_opened() noexcept;
  [[nodiscard]] StateChange connection_closed() noexcept;
  [[nodiscard]] StateChange fail() noexcept;

  [[nodiscard]] static bool transition_allowed(LogState from,
                                               LogState to) noexcept;

private:
  LogState state_{LogState::Off};
};

class WorkerLifecycleRegistry {
public:
  [[nodiscard]] WorkerLifecycle state(WorkerKind worker) const noexcept;
  [[nodiscard]] StateChange mark_running(WorkerKind worker) noexcept;
  [[nodiscard]] StateChange
  mark_at_return_point(const WorkerStoppedSignal &signal) noexcept;
  [[nodiscard]] bool should_wait_for(WorkerKind worker) const noexcept;
  [[nodiscard]] std::size_t running_count() const noexcept;

private:
  [[nodiscard]] static constexpr std::size_t
  index(const WorkerKind worker) noexcept {
    return static_cast<std::size_t>(worker);
  }

  std::array<WorkerLifecycle, worker_kind_count> states_{};
};

enum class ProcessLifecycle : std::uint8_t {
  Running,
  FatalStopping,
};

class MainThreadFatalGuard {
public:
  MainThreadFatalGuard() noexcept;

  [[nodiscard]] ProcessLifecycle state() const noexcept { return state_; }
  [[nodiscard]] std::thread::id owner_thread() const noexcept { return owner_; }
  [[nodiscard]] StateChange enter_fatal_stopping() noexcept;
  [[nodiscard]] StateChange
  enter_fatal_stopping_from(std::thread::id caller) noexcept;

private:
  std::thread::id owner_;
  ProcessLifecycle state_{ProcessLifecycle::Running};
};

} // namespace lazycom::app
