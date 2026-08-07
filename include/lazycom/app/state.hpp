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

/**
 * @brief Single-owner connection lifecycle with strong stale-event filtering.
 *
 * The state machine is not synchronized. Commands and events must carry
 * nonzero identities issued by their owning coordinator; value structs do not
 * enforce that invariant themselves. A connection generation is consumed when
 * begin_connect() succeeds and is never reused, including after failure.
 */
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

  /** @brief Begins a newer generation from Disconnected. */
  [[nodiscard]] StateChange
  begin_connect(const ConnectCommand &command) noexcept;
  /**
   * @brief Installs a session for the matching connect operation.
   * @note A matching success may arrive during cancellation cleanup; callers
   * must continue that cleanup rather than exposing a new connection.
   */
  [[nodiscard]] StateChange
  connect_succeeded(const ConnectSucceeded &event) noexcept;
  /** @brief Applies failure only to the matching connect operation/generation.
   */
  [[nodiscard]] StateChange connect_failed(const ConnectFailed &event) noexcept;
  /** @brief Moves a matching Connecting generation to Disconnecting cleanup. */
  [[nodiscard]] StateChange
  cancel_connect(const CancelConnectCommand &command) noexcept;
  /** @brief Begins cleanup for the exact current generation and session. */
  [[nodiscard]] StateChange
  begin_disconnect(const DisconnectCommand &command) noexcept;
  [[nodiscard]] StateChange
  session_fault(const SessionFaultEvent &event) noexcept;
  [[nodiscard]] StateChange
  disconnect_completed(const DisconnectCompleted &event) noexcept;

  /** @return true only for a nonzero TX operation matching the active session.
   */
  [[nodiscard]] bool accepts(const SendCommand &command) const noexcept;
  /**
   * @return true when generation, session, origin, and lifecycle permit the
   * event. Cleanup-origin events are accepted only while Disconnecting.
   */
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

/** @brief Single-owner interaction-state transitions for the primary panels. */
class InteractionStateMachine {
public:
  [[nodiscard]] InteractionState state() const noexcept { return state_; }
  [[nodiscard]] StateChange enter(InteractionState state) noexcept;
  void reset_to_normal() noexcept { state_ = InteractionState::Normal; }

private:
  InteractionState state_{InteractionState::Normal};
};

/**
 * @brief Single-owner Off/Waiting/Recording/Error session-log state machine.
 *
 * enable(false) enters Waiting, enable(true) enters Recording, and disable()
 * returns every non-Off state to Off. Connection events only move between
 * Waiting and Recording.
 */
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

/**
 * @brief Coordinator-owned registry of worker return-point lifecycles.
 * @pre WorkerKind arguments are valid enumerators.
 * @note The registry records NotStarted to Running to AtReturnPoint; it does
 * not request stop or join any thread.
 */
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
  /** @brief Captures the constructing thread as the sole fatal coordinator. */
  MainThreadFatalGuard() noexcept;

  [[nodiscard]] ProcessLifecycle state() const noexcept { return state_; }
  [[nodiscard]] std::thread::id owner_thread() const noexcept { return owner_; }
  /**
   * @brief Idempotently enters FatalStopping on the captured owner thread.
   * @return Unauthorized without mutation when called from another thread.
   */
  [[nodiscard]] StateChange enter_fatal_stopping() noexcept;
  [[nodiscard]] StateChange
  enter_fatal_stopping_from(std::thread::id caller) noexcept;

private:
  std::thread::id owner_;
  ProcessLifecycle state_{ProcessLifecycle::Running};
};

} // namespace lazycom::app
