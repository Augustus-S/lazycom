#include <lazycom/app/state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <thread>

using namespace lazycom;
using namespace lazycom::app;

TEST_CASE("connection state machine rejects illegal and stale transitions",
          "[state]") {
  ConnectionStateMachine state;

  REQUIRE(state.begin_disconnect({OperationId{1}, ConnectionGeneration{1},
                                  std::nullopt}) == StateChange::IgnoredStale);
  REQUIRE(state.begin_connect({OperationId{1}, ConnectionGeneration{1}}) ==
          StateChange::Applied);
  REQUIRE(state.begin_connect({OperationId{2}, ConnectionGeneration{2}}) ==
          StateChange::InvalidTransition);
  REQUIRE(state.connect_succeeded(
              {OperationId{1}, ConnectionGeneration{0}, SessionId{10}}) ==
          StateChange::InvalidTransition);
  REQUIRE(state.connect_succeeded(
              {OperationId{9}, ConnectionGeneration{1}, SessionId{10}}) ==
          StateChange::IgnoredStale);
  REQUIRE(state.connect_succeeded({OperationId{1}, ConnectionGeneration{1},
                                   SessionId{10}}) == StateChange::Applied);
  REQUIRE(state.state() == ConnectionState::Connected);

  REQUIRE_FALSE(state.accepts(
      SendCommand{OperationId{3}, ConnectionGeneration{0}, SessionId{10}, {}}));
  REQUIRE_FALSE(state.accepts(
      SendCommand{OperationId{3}, ConnectionGeneration{1}, SessionId{11}, {}}));
  REQUIRE(state.accepts(
      SendCommand{OperationId{3}, ConnectionGeneration{1}, SessionId{10}, {}}));
}

TEST_CASE("disconnect keeps its session until matching completion", "[state]") {
  ConnectionStateMachine state;
  REQUIRE(state.begin_connect({OperationId{1}, ConnectionGeneration{1}}) ==
          StateChange::Applied);
  REQUIRE(state.connect_succeeded({OperationId{1}, ConnectionGeneration{1},
                                   SessionId{20}}) == StateChange::Applied);
  REQUIRE(state.begin_disconnect({OperationId{2}, ConnectionGeneration{1},
                                  SessionId{20}}) == StateChange::Applied);

  REQUIRE(state.session_id() == SessionId{20});
  REQUIRE(state.accepts({ConnectionGeneration{1}, SessionId{20},
                         SessionEventOrigin::Normal, SessionEventKind::Rx}));
  REQUIRE(
      state.accepts({ConnectionGeneration{1}, SessionId{20},
                     SessionEventOrigin::Cleanup, SessionEventKind::System}));
  REQUIRE_FALSE(
      state.accepts({ConnectionGeneration{1}, SessionId{19},
                     SessionEventOrigin::Cleanup, SessionEventKind::Rx}));
  REQUIRE(state.disconnect_completed(
              {OperationId{99}, ConnectionGeneration{1}, SessionId{20},
               OperationOutcome::Succeeded}) == StateChange::IgnoredStale);
  REQUIRE(state.session_id() == SessionId{20});
  REQUIRE(state.disconnect_completed(
              {OperationId{2}, ConnectionGeneration{1}, SessionId{20},
               OperationOutcome::Succeeded}) == StateChange::Applied);
  REQUIRE_FALSE(state.session_id().has_value());
  REQUIRE(state.state() == ConnectionState::Disconnected);

  REQUIRE(state.begin_connect({OperationId{3}, ConnectionGeneration{1}}) ==
          StateChange::IgnoredStale);
  REQUIRE(state.begin_connect({OperationId{3}, ConnectionGeneration{2}}) ==
          StateChange::Applied);
  REQUIRE(state.connect_succeeded({OperationId{3}, ConnectionGeneration{2},
                                   SessionId{21}}) == StateChange::Applied);
  REQUIRE_FALSE(
      state.accepts({ConnectionGeneration{1}, SessionId{20},
                     SessionEventOrigin::Normal, SessionEventKind::Rx}));
  REQUIRE_FALSE(
      state.accepts({ConnectionGeneration{2}, SessionId{20},
                     SessionEventOrigin::Normal, SessionEventKind::Rx}));
  REQUIRE(state.accepts({ConnectionGeneration{2}, SessionId{21},
                         SessionEventOrigin::Normal, SessionEventKind::Rx}));
}

TEST_CASE("connection errors must clean up through disconnecting", "[state]") {
  ConnectionStateMachine state;
  REQUIRE(state.begin_connect({OperationId{1}, ConnectionGeneration{1}}) ==
          StateChange::Applied);
  REQUIRE(state.connect_failed({OperationId{1}, ConnectionGeneration{1}}) ==
          StateChange::Applied);
  REQUIRE(state.state() == ConnectionState::Error);
  REQUIRE(state.begin_connect({OperationId{2}, ConnectionGeneration{2}}) ==
          StateChange::InvalidTransition);
  REQUIRE(state.begin_disconnect({OperationId{2}, ConnectionGeneration{1},
                                  std::nullopt}) == StateChange::Applied);
  REQUIRE(state.disconnect_completed(
              {OperationId{2}, ConnectionGeneration{1}, std::nullopt,
               OperationOutcome::Succeeded}) == StateChange::Applied);
}

TEST_CASE("cancelled connect retains a racing successful session until cleanup",
          "[state]") {
  ConnectionStateMachine state;
  REQUIRE(state.begin_connect({OperationId{1}, ConnectionGeneration{1}}) ==
          StateChange::Applied);
  REQUIRE(state.cancel_connect({OperationId{2}, ConnectionGeneration{1}}) ==
          StateChange::Applied);
  REQUIRE(state.connect_operation() == OperationId{1});
  REQUIRE(state.cleanup_operation() == OperationId{2});

  REQUIRE(state.connect_succeeded({OperationId{1}, ConnectionGeneration{1},
                                   SessionId{30}}) == StateChange::Applied);
  REQUIRE(state.state() == ConnectionState::Disconnecting);
  REQUIRE(state.session_id() == SessionId{30});
  REQUIRE_FALSE(state.connect_operation().has_value());
  REQUIRE(
      state.accepts({ConnectionGeneration{1}, SessionId{30},
                     SessionEventOrigin::Cleanup, SessionEventKind::System}));

  REQUIRE(state.disconnect_completed(
              {OperationId{2}, ConnectionGeneration{1}, std::nullopt,
               OperationOutcome::Succeeded}) == StateChange::InvalidTransition);
  REQUIRE(state.begin_disconnect({OperationId{3}, ConnectionGeneration{1},
                                  SessionId{30}}) == StateChange::Applied);
  REQUIRE(state.cleanup_operation() == OperationId{3});
  REQUIRE(state.disconnect_completed(
              {OperationId{2}, ConnectionGeneration{1}, SessionId{30},
               OperationOutcome::Succeeded}) == StateChange::IgnoredStale);
  REQUIRE(state.disconnect_completed(
              {OperationId{3}, ConnectionGeneration{1}, SessionId{30},
               OperationOutcome::Succeeded}) == StateChange::Applied);
  REQUIRE(state.state() == ConnectionState::Disconnected);
}

TEST_CASE("cancelled connect failure still waits for matching cleanup",
          "[state]") {
  ConnectionStateMachine state;
  REQUIRE(state.begin_connect({OperationId{10}, ConnectionGeneration{4}}) ==
          StateChange::Applied);
  REQUIRE(state.cancel_connect({OperationId{11}, ConnectionGeneration{4}}) ==
          StateChange::Applied);
  REQUIRE(state.connect_failed({OperationId{10}, ConnectionGeneration{4}}) ==
          StateChange::Applied);
  REQUIRE(state.state() == ConnectionState::Disconnecting);
  REQUIRE_FALSE(state.session_id().has_value());
  REQUIRE(state.disconnect_completed(
              {OperationId{11}, ConnectionGeneration{3}, std::nullopt,
               OperationOutcome::Succeeded}) == StateChange::InvalidTransition);
  REQUIRE(state.disconnect_completed(
              {OperationId{11}, ConnectionGeneration{4}, std::nullopt,
               OperationOutcome::Succeeded}) == StateChange::Applied);
}

TEST_CASE("interaction and logging states are independent strict machines",
          "[state]") {
  InteractionStateMachine interaction;
  LogStateMachine logging;
  REQUIRE(interaction.enter(InteractionState::SendEdit) ==
          StateChange::Applied);
  REQUIRE(logging.connection_opened() == StateChange::InvalidTransition);
  REQUIRE(logging.enable(false) == StateChange::Applied);
  REQUIRE(logging.state() == LogState::Waiting);
  REQUIRE(logging.connection_opened() == StateChange::Applied);
  REQUIRE(logging.fail() == StateChange::Applied);
  REQUIRE(logging.enable(true) == StateChange::InvalidTransition);
  REQUIRE(logging.disable() == StateChange::Applied);
  REQUIRE(logging.enable(true) == StateChange::Applied);
  REQUIRE(logging.connection_closed() == StateChange::Applied);
  REQUIRE(interaction.state() == InteractionState::SendEdit);
}

TEST_CASE("only the creating thread identity may enter fatal stopping",
          "[state]") {
  MainThreadFatalGuard guard;
  REQUIRE(guard.enter_fatal_stopping_from(std::thread::id{}) ==
          StateChange::Unauthorized);
  REQUIRE(guard.state() == ProcessLifecycle::Running);
  REQUIRE(guard.enter_fatal_stopping() == StateChange::Applied);
  REQUIRE(guard.enter_fatal_stopping() == StateChange::NoChange);
}

TEST_CASE("all five workers follow the fixed lifecycle", "[state]") {
  WorkerLifecycleRegistry workers;
  constexpr std::array kinds{WorkerKind::Serial, WorkerKind::SessionLog,
                             WorkerKind::Scanner, WorkerKind::Persistence,
                             WorkerKind::Diagnostics};

  for (const auto kind : kinds) {
    REQUIRE(workers.state(kind) == WorkerLifecycle::NotStarted);
    REQUIRE_FALSE(workers.should_wait_for(kind));
    REQUIRE(workers.mark_running(kind) == StateChange::Applied);
  }
  REQUIRE(workers.running_count() == kinds.size());
  for (const auto kind : kinds) {
    REQUIRE(workers.mark_at_return_point({kind, WorkerLifecycle::AtReturnPoint,
                                          WorkerExitReason::Completed}) ==
            StateChange::Applied);
    REQUIRE_FALSE(workers.should_wait_for(kind));
  }
  REQUIRE(workers.running_count() == 0);
  REQUIRE(workers.mark_at_return_point(
              {WorkerKind::Serial, WorkerLifecycle::AtReturnPoint,
               WorkerExitReason::Completed}) == StateChange::NoChange);
}
