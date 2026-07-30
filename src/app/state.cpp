#include <lazycom/app/completion.hpp>
#include <lazycom/app/state.hpp>

#include <algorithm>

namespace lazycom::app {

namespace {

[[nodiscard]] bool valid(const OperationId id) noexcept {
  return id.value != 0;
}
[[nodiscard]] bool valid(const ConnectionGeneration id) noexcept {
  return id.value != 0;
}
[[nodiscard]] bool valid(const SessionId id) noexcept { return id.value != 0; }

[[nodiscard]] bool overlay_can_cover(const OverlayKind next,
                                     const OverlayKind active) noexcept {
  if (next == OverlayKind::Help) {
    return active == OverlayKind::Search || active == OverlayKind::Modal ||
           active == OverlayKind::Confirm || active == OverlayKind::ErrorDialog;
  }
  return next == OverlayKind::CommandPalette && active == OverlayKind::Search;
}

} // namespace

bool ConnectionStateMachine::transition_allowed(
    const ConnectionState from, const ConnectionState to) noexcept {
  switch (from) {
  case ConnectionState::Disconnected:
    return to == ConnectionState::Connecting;
  case ConnectionState::Connecting:
    return to == ConnectionState::Connected || to == ConnectionState::Error ||
           to == ConnectionState::Disconnecting;
  case ConnectionState::Connected:
    return to == ConnectionState::Disconnecting || to == ConnectionState::Error;
  case ConnectionState::Error:
    return to == ConnectionState::Disconnecting;
  case ConnectionState::Disconnecting:
    return to == ConnectionState::Disconnected;
  }
  return false;
}

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
       state_ != ConnectionState::Error) ||
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
    return state_ == ConnectionState::Connected;
  }
  return state_ == ConnectionState::Disconnecting;
}

StateChange
InteractionStateMachine::enter(const InteractionState state) noexcept {
  if (state_ == state) {
    return StateChange::NoChange;
  }
  state_ = state;
  return StateChange::Applied;
}

const OverlayFrame *OverlayStack::active() const noexcept {
  return empty() ? nullptr : &frames_[size_ - 1];
}

StateChange OverlayStack::open(const OverlayFrame &frame) noexcept {
  if (frame.kind == OverlayKind::None) {
    return StateChange::InvalidTransition;
  }
  if (!empty() && !overlay_can_cover(frame.kind, active()->kind)) {
    return StateChange::InvalidTransition;
  }
  if (size_ == capacity) {
    return StateChange::CapacityExceeded;
  }
  frames_[size_] = frame;
  ++size_;
  return StateChange::Applied;
}

OverlayCloseResult
OverlayStack::toggle_help(const OverlayFrame &frame) noexcept {
  if (frame.kind != OverlayKind::Help) {
    return {};
  }
  if (!empty() && active()->kind == OverlayKind::Help) {
    return close_top();
  }
  const auto change = open(frame);
  return {change, frame.interaction};
}

StateChange OverlayStack::update_active(const OverlayFrame &frame) noexcept {
  if (empty() || frame.kind != frames_[size_ - 1].kind ||
      frame.kind == OverlayKind::None) {
    return StateChange::InvalidTransition;
  }
  frames_[size_ - 1] = frame;
  return StateChange::Applied;
}

OverlayCloseResult OverlayStack::close_top() noexcept {
  if (empty()) {
    return {};
  }
  const auto closed = frames_[size_ - 1];
  --size_;

  InteractionState restored = closed.interaction;
  if (!empty()) {
    restored = frames_[size_ - 1].interaction;
  } else if (closed.kind == OverlayKind::Modal ||
             closed.kind == OverlayKind::Confirm) {
    restored = InteractionState::Normal;
  }
  return {StateChange::Applied, restored};
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

WorkerLifecycle
WorkerLifecycleRegistry::state(const WorkerKind worker) const noexcept {
  return states_[index(worker)];
}

StateChange
WorkerLifecycleRegistry::mark_running(const WorkerKind worker) noexcept {
  auto &lifecycle = states_[index(worker)];
  if (lifecycle != WorkerLifecycle::NotStarted) {
    return StateChange::InvalidTransition;
  }
  lifecycle = WorkerLifecycle::Running;
  return StateChange::Applied;
}

StateChange WorkerLifecycleRegistry::mark_at_return_point(
    const WorkerStoppedSignal &signal) noexcept {
  if (signal.lifecycle != WorkerLifecycle::AtReturnPoint) {
    return StateChange::InvalidTransition;
  }
  auto &lifecycle = states_[index(signal.worker)];
  if (lifecycle == WorkerLifecycle::AtReturnPoint) {
    return StateChange::NoChange;
  }
  if (lifecycle != WorkerLifecycle::Running) {
    return StateChange::InvalidTransition;
  }
  lifecycle = WorkerLifecycle::AtReturnPoint;
  return StateChange::Applied;
}

bool WorkerLifecycleRegistry::should_wait_for(
    const WorkerKind worker) const noexcept {
  return state(worker) == WorkerLifecycle::Running;
}

std::size_t WorkerLifecycleRegistry::running_count() const noexcept {
  return static_cast<std::size_t>(
      std::count(states_.begin(), states_.end(), WorkerLifecycle::Running));
}

MainThreadFatalGuard::MainThreadFatalGuard() noexcept
    : owner_(std::this_thread::get_id()) {}

StateChange MainThreadFatalGuard::enter_fatal_stopping() noexcept {
  return enter_fatal_stopping_from(std::this_thread::get_id());
}

StateChange MainThreadFatalGuard::enter_fatal_stopping_from(
    const std::thread::id caller) noexcept {
  if (caller != owner_) {
    return StateChange::Unauthorized;
  }
  if (state_ == ProcessLifecycle::FatalStopping) {
    return StateChange::NoChange;
  }
  state_ = ProcessLifecycle::FatalStopping;
  return StateChange::Applied;
}

std::size_t CompletionTracker::reserved_count() const noexcept {
  const auto count = [](const std::vector<Slot> &slots) {
    return static_cast<std::size_t>(
        std::count_if(slots.begin(), slots.end(), [](const Slot &slot) {
          return slot.state != SlotState::Free;
        }));
  };
  return count(normal_slots_) + count(tx_slots_) + count(control_slots_);
}

std::size_t CompletionTracker::completed_count() const noexcept {
  const auto count = [](const std::vector<Slot> &slots) {
    return static_cast<std::size_t>(
        std::count_if(slots.begin(), slots.end(), [](const Slot &slot) {
          return slot.state == SlotState::Completed;
        }));
  };
  return count(normal_slots_) + count(tx_slots_) + count(control_slots_);
}

std::size_t CompletionTracker::capacity(
    const OperationClass operation_class) const noexcept {
  return slots(operation_class).size();
}

std::size_t CompletionTracker::capacity() const noexcept {
  return normal_slots_.size() + tx_slots_.size() + control_slots_.size();
}

std::vector<CompletionTracker::Slot> &
CompletionTracker::slots(const OperationClass operation_class) noexcept {
  switch (operation_class) {
  case OperationClass::Normal:
    return normal_slots_;
  case OperationClass::Tx:
    return tx_slots_;
  case OperationClass::Control:
    return control_slots_;
  }
  return normal_slots_;
}

const std::vector<CompletionTracker::Slot> &
CompletionTracker::slots(const OperationClass operation_class) const noexcept {
  switch (operation_class) {
  case OperationClass::Normal:
    return normal_slots_;
  case OperationClass::Tx:
    return tx_slots_;
  case OperationClass::Control:
    return control_slots_;
  }
  return normal_slots_;
}

CompletionTracker::Slot *
CompletionTracker::find(const OperationId operation) noexcept {
  for (const auto operation_class :
       {OperationClass::Normal, OperationClass::Tx, OperationClass::Control}) {
    auto &class_slots = slots(operation_class);
    const auto found = std::find_if(class_slots.begin(), class_slots.end(),
                                    [operation](const Slot &slot) {
                                      return slot.state != SlotState::Free &&
                                             slot.operation_id == operation;
                                    });
    if (found != class_slots.end()) {
      return &*found;
    }
  }
  return nullptr;
}

const CompletionTracker::Slot *
CompletionTracker::find(const OperationId operation) const noexcept {
  for (const auto operation_class :
       {OperationClass::Normal, OperationClass::Tx, OperationClass::Control}) {
    const auto &class_slots = slots(operation_class);
    const auto found = std::find_if(class_slots.begin(), class_slots.end(),
                                    [operation](const Slot &slot) {
                                      return slot.state != SlotState::Free &&
                                             slot.operation_id == operation;
                                    });
    if (found != class_slots.end()) {
      return &*found;
    }
  }
  return nullptr;
}

ReserveCompletionResult
CompletionTracker::reserve(const OperationId operation,
                           const OperationClass operation_class,
                           const CompletionKind expected_kind) noexcept {
  if (!valid(operation)) {
    return ReserveCompletionResult::InvalidOperation;
  }
  if (find(operation) != nullptr) {
    return ReserveCompletionResult::Duplicate;
  }
  auto &class_slots = slots(operation_class);
  const auto free_slot = std::find_if(
      class_slots.begin(), class_slots.end(),
      [](const Slot &slot) { return slot.state == SlotState::Free; });
  if (free_slot == class_slots.end()) {
    return ReserveCompletionResult::Full;
  }
  free_slot->operation_id = operation;
  free_slot->expected_kind = expected_kind;
  free_slot->state = SlotState::Reserved;
  free_slot->completion.reset();
  return ReserveCompletionResult::Reserved;
}

bool CompletionTracker::cancel_reservation(
    const OperationId operation) noexcept {
  auto *const slot = find(operation);
  if (slot == nullptr || slot->state != SlotState::Reserved) {
    return false;
  }
  *slot = Slot{};
  return true;
}

CompleteOperationResult
CompletionTracker::complete(const CompletionEvent &event) noexcept {
  auto *const slot = find(operation_id(event));
  if (slot == nullptr) {
    return CompleteOperationResult::UnknownIgnored;
  }
  if (slot->expected_kind != completion_kind(event)) {
    return CompleteOperationResult::UnexpectedKind;
  }
  if (slot->state == SlotState::Completed) {
    return CompleteOperationResult::DuplicateIgnored;
  }
  slot->completion = event;
  slot->state = SlotState::Completed;
  return CompleteOperationResult::Completed;
}

const CompletionEvent *
CompletionTracker::peek(const OperationId operation) const noexcept {
  const auto *const slot = find(operation);
  if (slot == nullptr || slot->state != SlotState::Completed) {
    return nullptr;
  }
  return &*slot->completion;
}

std::optional<CompletionEvent>
CompletionTracker::consume(const OperationId operation) noexcept {
  auto *const slot = find(operation);
  if (slot == nullptr || slot->state != SlotState::Completed) {
    return std::nullopt;
  }
  auto event = std::move(slot->completion);
  *slot = Slot{};
  return event;
}

} // namespace lazycom::app
