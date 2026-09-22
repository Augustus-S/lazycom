#include <lazycom/app/state.hpp>

namespace lazycom::app {

namespace {

[[nodiscard]] bool valid(const OperationId id) noexcept {
  return id.value != 0;
}
[[nodiscard]] bool valid(const ConnectionGeneration id) noexcept {
  return id.value != 0;
}
[[nodiscard]] bool valid(const SessionId id) noexcept { return id.value != 0; }

} // namespace

bool ConnectionStateMachine::matches_generation(
    const ConnectionGeneration generation) const noexcept {
  return generation_.has_value() && *generation_ == generation;
}

bool ConnectionStateMachine::matches_session(
    const std::optional<SessionId> &session) const noexcept {
  return session_id_ == session;
}

StateChange
ConnectionStateMachine::begin_connect(const ConnectCommand &command) noexcept {
  if (state_ != ConnectionState::Disconnected) {
    return StateChange::InvalidTransition;
  }
  if (!valid(command.operation_id) || !valid(command.generation) ||
      command.generation <= last_generation_) {
    return StateChange::IgnoredStale;
  }
  state_ = ConnectionState::Connecting;
  last_generation_ = command.generation;
  generation_ = command.generation;
  session_id_.reset();
  connect_operation_ = command.operation_id;
  cleanup_operation_.reset();
  return StateChange::Applied;
}

StateChange ConnectionStateMachine::connect_succeeded(
    const ConnectSucceeded &event) noexcept {
  if (connect_operation_ != event.operation_id) {
    return StateChange::IgnoredStale;
  }
  if (!matches_generation(event.generation) || !valid(event.session_id)) {
    return StateChange::InvalidTransition;
  }
  if (state_ != ConnectionState::Connecting &&
      state_ != ConnectionState::Disconnecting) {
    return StateChange::InvalidTransition;
  }
  session_id_ = event.session_id;
  connect_operation_.reset();
  if (state_ == ConnectionState::Connecting) {
    state_ = ConnectionState::Connected;
  }
  return StateChange::Applied;
}

StateChange
ConnectionStateMachine::connect_failed(const ConnectFailed &event) noexcept {
  if (connect_operation_ != event.operation_id) {
    return StateChange::IgnoredStale;
  }
  if (!matches_generation(event.generation)) {
    return StateChange::InvalidTransition;
  }
  if (state_ != ConnectionState::Connecting &&
      state_ != ConnectionState::Disconnecting) {
    return StateChange::InvalidTransition;
  }
  connect_operation_.reset();
  if (state_ == ConnectionState::Connecting) {
    state_ = ConnectionState::Error;
  }
  return StateChange::Applied;
}

StateChange ConnectionStateMachine::cancel_connect(
    const CancelConnectCommand &command) noexcept {
  if (!matches_generation(command.generation)) {
    return StateChange::IgnoredStale;
  }
  if (state_ != ConnectionState::Connecting || !valid(command.operation_id)) {
    return StateChange::InvalidTransition;
  }
  state_ = ConnectionState::Disconnecting;
  cleanup_operation_ = command.operation_id;
  return StateChange::Applied;
}

StateChange ConnectionStateMachine::begin_disconnect(
    const DisconnectCommand &command) noexcept {
  if (!matches_generation(command.generation) ||
      !matches_session(command.session_id)) {
    return StateChange::IgnoredStale;
  }
  if ((state_ != ConnectionState::Connected &&
       state_ != ConnectionState::Error &&
       !(state_ == ConnectionState::Disconnecting && session_id_)) ||
      !valid(command.operation_id)) {
    return StateChange::InvalidTransition;
  }
  state_ = ConnectionState::Disconnecting;
  cleanup_operation_ = command.operation_id;
  return StateChange::Applied;
}

StateChange
ConnectionStateMachine::session_fault(const SessionFaultEvent &event) noexcept {
  if (!matches_generation(event.generation) ||
      !matches_session(event.session_id)) {
    return StateChange::IgnoredStale;
  }
  if (state_ != ConnectionState::Connected) {
    return StateChange::InvalidTransition;
  }
  state_ = ConnectionState::Error;
  return StateChange::Applied;
}

StateChange ConnectionStateMachine::disconnect_completed(
    const DisconnectCompleted &event) noexcept {
  if (cleanup_operation_ != event.operation_id) {
    return StateChange::IgnoredStale;
  }
  if (state_ != ConnectionState::Disconnecting ||
      !matches_generation(event.generation) ||
      !matches_session(event.session_id)) {
    return StateChange::InvalidTransition;
  }
  state_ = ConnectionState::Disconnected;
  generation_.reset();
  session_id_.reset();
  connect_operation_.reset();
  cleanup_operation_.reset();
  return StateChange::Applied;
}

bool ConnectionStateMachine::accepts(
    const SendCommand &command) const noexcept {
  return state_ == ConnectionState::Connected && valid(command.operation_id) &&
         matches_generation(command.generation) &&
         matches_session(command.session_id);
}

bool ConnectionStateMachine::accepts(
    const SessionDataEvent &event) const noexcept {
  if (!matches_generation(event.generation) ||
      !matches_session(event.session_id)) {
    return false;
  }
  if (event.origin == SessionEventOrigin::Normal) {
    // The owner cleanup marker is the disconnect boundary. Normal events
    // ordered before it remain part of this session while disconnecting.
    return state_ == ConnectionState::Connected ||
           state_ == ConnectionState::Disconnecting;
  }
  return state_ == ConnectionState::Disconnecting;
}

bool LogStateMachine::transition_allowed(const LogState from,
                                         const LogState to) noexcept {
  switch (from) {
  case LogState::Off:
    return to == LogState::Waiting || to == LogState::Recording;
  case LogState::Waiting:
    return to == LogState::Recording || to == LogState::Off ||
           to == LogState::Error;
  case LogState::Recording:
    return to == LogState::Waiting || to == LogState::Off ||
           to == LogState::Error;
  case LogState::Error:
    return to == LogState::Off;
  }
  return false;
}

StateChange LogStateMachine::transition(const LogState next) noexcept {
  if (state_ == next) {
    return StateChange::NoChange;
  }
  if (!transition_allowed(state_, next)) {
    return StateChange::InvalidTransition;
  }
  state_ = next;
  return StateChange::Applied;
}

StateChange LogStateMachine::enable(const bool connected) noexcept {
  if (state_ != LogState::Off) {
    return StateChange::InvalidTransition;
  }
  return transition(connected ? LogState::Recording : LogState::Waiting);
}

StateChange LogStateMachine::disable() noexcept {
  if (state_ == LogState::Off) {
    return StateChange::NoChange;
  }
  return transition(LogState::Off);
}

StateChange LogStateMachine::connection_opened() noexcept {
  if (state_ != LogState::Waiting) {
    return state_ == LogState::Recording ? StateChange::NoChange
                                         : StateChange::InvalidTransition;
  }
  return transition(LogState::Recording);
}

StateChange LogStateMachine::connection_closed() noexcept {
  if (state_ != LogState::Recording) {
    return state_ == LogState::Waiting ? StateChange::NoChange
                                       : StateChange::InvalidTransition;
  }
  return transition(LogState::Waiting);
}

StateChange LogStateMachine::fail() noexcept {
  return transition(LogState::Error);
}

} // namespace lazycom::app
