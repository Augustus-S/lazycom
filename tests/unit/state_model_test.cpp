#include <lazycom/app/state.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <thread>

using namespace lazycom;
using namespace lazycom::app;

TEST_CASE("connection lifecycle rejects stale identities through disconnect "
          "and reconnect",
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
  REQUIRE(state.begin_disconnect({OperationId{2}, ConnectionGeneration{1},
                                  SessionId{10}}) == StateChange::Applied);

  REQUIRE(state.session_id() == SessionId{10});
  REQUIRE(state.accepts({ConnectionGeneration{1}, SessionId{10},
                         SessionEventOrigin::Normal, SessionEventKind::Rx}));
  REQUIRE(
      state.accepts({ConnectionGeneration{1}, SessionId{10},
                     SessionEventOrigin::Cleanup, SessionEventKind::System}));
  REQUIRE_FALSE(
      state.accepts({ConnectionGeneration{1}, SessionId{19},
                     SessionEventOrigin::Cleanup, SessionEventKind::Rx}));
  REQUIRE(state.disconnect_completed(
              {OperationId{99}, ConnectionGeneration{1}, SessionId{10},
               OperationOutcome::Succeeded}) == StateChange::IgnoredStale);
  REQUIRE(state.session_id() == SessionId{10});
  REQUIRE(state.disconnect_completed(
              {OperationId{2}, ConnectionGeneration{1}, SessionId{10},
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
      state.accepts({ConnectionGeneration{1}, SessionId{10},
                     SessionEventOrigin::Normal, SessionEventKind::Rx}));
  REQUIRE_FALSE(
      state.accepts({ConnectionGeneration{2}, SessionId{10},
                     SessionEventOrigin::Normal, SessionEventKind::Rx}));
  REQUIRE(state.accepts({ConnectionGeneration{2}, SessionId{21},
                         SessionEventOrigin::Normal, SessionEventKind::Rx}));
}

TEST_CASE("connect failures and cancellation converge through matching cleanup",
          "[state]") {
  ConnectionStateMachine state;
  REQUIRE(state.begin_connect({OperationId{1}, ConnectionGeneration{4}}) ==
          StateChange::Applied);
  SECTION("connection errors must clean up through disconnecting") {
    REQUIRE(state.connect_failed({OperationId{1}, ConnectionGeneration{4}}) ==
            StateChange::Applied);
    REQUIRE(state.state() == ConnectionState::Error);
    REQUIRE(state.begin_connect({OperationId{2}, ConnectionGeneration{5}}) ==
            StateChange::InvalidTransition);
    REQUIRE(state.begin_disconnect({OperationId{2}, ConnectionGeneration{4},
                                    std::nullopt}) == StateChange::Applied);
    REQUIRE(state.disconnect_completed(
                {OperationId{2}, ConnectionGeneration{4}, std::nullopt,
                 OperationOutcome::Succeeded}) == StateChange::Applied);
  }
  SECTION(
      "cancelled connect retains a racing successful session until cleanup") {
    REQUIRE(state.cancel_connect({OperationId{2}, ConnectionGeneration{4}}) ==
            StateChange::Applied);
    REQUIRE(state.connect_operation() == OperationId{1});
    REQUIRE(state.cleanup_operation() == OperationId{2});

    REQUIRE(state.connect_succeeded({OperationId{1}, ConnectionGeneration{4},
                                     SessionId{30}}) == StateChange::Applied);
    REQUIRE(state.state() == ConnectionState::Disconnecting);
    REQUIRE(state.session_id() == SessionId{30});
    REQUIRE_FALSE(state.connect_operation().has_value());
    REQUIRE(
        state.accepts({ConnectionGeneration{4}, SessionId{30},
                       SessionEventOrigin::Cleanup, SessionEventKind::System}));

    REQUIRE(state.disconnect_completed({OperationId{2}, ConnectionGeneration{4},
                                        std::nullopt,
                                        OperationOutcome::Succeeded}) ==
            StateChange::InvalidTransition);
    REQUIRE(state.begin_disconnect({OperationId{3}, ConnectionGeneration{4},
                                    SessionId{30}}) == StateChange::Applied);
    REQUIRE(state.cleanup_operation() == OperationId{3});
    REQUIRE(state.disconnect_completed(
                {OperationId{2}, ConnectionGeneration{4}, SessionId{30},
                 OperationOutcome::Succeeded}) == StateChange::IgnoredStale);
    REQUIRE(state.disconnect_completed(
                {OperationId{3}, ConnectionGeneration{4}, SessionId{30},
                 OperationOutcome::Succeeded}) == StateChange::Applied);
    REQUIRE(state.state() == ConnectionState::Disconnected);
  }
  SECTION("cancelled connect failure still waits for matching cleanup") {
    REQUIRE(state.cancel_connect({OperationId{2}, ConnectionGeneration{4}}) ==
            StateChange::Applied);
    REQUIRE(state.connect_failed({OperationId{1}, ConnectionGeneration{4}}) ==
            StateChange::Applied);
    REQUIRE(state.state() == ConnectionState::Disconnecting);
    REQUIRE_FALSE(state.session_id().has_value());
    REQUIRE(state.disconnect_completed({OperationId{2}, ConnectionGeneration{3},
                                        std::nullopt,
                                        OperationOutcome::Succeeded}) ==
            StateChange::InvalidTransition);
    REQUIRE(state.disconnect_completed(
                {OperationId{2}, ConnectionGeneration{4}, std::nullopt,
                 OperationOutcome::Succeeded}) == StateChange::Applied);
  }
}
