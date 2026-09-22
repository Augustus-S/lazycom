#include <lazycom/scheduler/scheduler.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using lazycom::TaskGeneration;
using lazycom::config::Newline;
using lazycom::config::QuickSendSlot;
using lazycom::config::QuickSendSnapshot;
using lazycom::config::SendMode;
using namespace lazycom::scheduler;

static_assert(valid_interval(0U));
static_assert(!valid_interval(1U));
static_assert(!valid_interval(9U));
static_assert(valid_interval(10U));
static_assert(valid_interval(86'400'000U));
static_assert(!valid_interval(86'400'001U));

[[nodiscard]] Scheduler::TimePoint at(const std::int64_t milliseconds) {
  return Scheduler::TimePoint{} + std::chrono::milliseconds{milliseconds};
}

[[nodiscard]] QuickSendExecution execution(const std::uint32_t slot = 1U) {
  return QuickSendExecution{
      slot,          "slot", SendMode::Txt,   Newline::None,
      Newline::None, "",     {std::byte{'x'}}};
}

[[nodiscard]] TaskRequest request(const std::uint64_t interval_ms,
                                  const std::uint32_t slot = 1U) {
  return TaskRequest{execution(slot), interval_ms};
}

Scheduler running(std::uint64_t interval = 10U) {
  Scheduler scheduler;
  REQUIRE(scheduler.start(request(interval), at(0)).status ==
          TaskStartStatus::Started);
  return scheduler;
}
} // namespace

TEST_CASE("payload policy resolves suffixes and fails atomically",
          "[scheduler]") {
  struct Suffix {
    Newline policy;
    std::string_view expected;
  };
  for (const auto &[policy, expected] : {Suffix{Newline::None, "x"},
                                         {Newline::Lf, "x\n"},
                                         {Newline::Cr, "x\r"},
                                         {Newline::CrLf, "x\r\n"},
                                         {Newline::Session, "x\r\n"}}) {
    CAPTURE(policy);
    const auto parsed =
        parse_payload(SendMode::Txt, "x", policy, Newline::CrLf);
    REQUIRE(parsed);
    CHECK(std::string(reinterpret_cast<const char *>(parsed.bytes.data()),
                      parsed.bytes.size()) == expected);
  }
  const auto hex = parse_payload(SendMode::Hex, "41 54", Newline::Lf);
  REQUIRE(hex);
  CHECK(hex.bytes == std::vector<std::byte>{std::byte{'A'}, std::byte{'T'},
                                            std::byte{'\n'}});
  struct Rejected {
    SendMode mode;
    std::string text;
    Newline newline;
    PayloadParseStatus status;
  };
  const Rejected rejected[]{
      {SendMode::Hex, "0x0g", Newline::None, PayloadParseStatus::InvalidHex},
      {SendMode::Txt, "\xC0\x80", Newline::CrLf,
       PayloadParseStatus::InvalidText},
      {SendMode::Txt, "x", Newline::Session,
       PayloadParseStatus::InvalidNewline},
      {static_cast<SendMode>(99), "x", Newline::None,
       PayloadParseStatus::InvalidMode},
      {SendMode::Txt, std::string(lazycom::config::kMaximumPayloadBytes, 'x'),
       Newline::Lf, PayloadParseStatus::TooLarge},
      {SendMode::Txt,
       std::string(lazycom::config::kMaximumPayloadBytes, 'x') + '\xff',
       Newline::None, PayloadParseStatus::InvalidText},
  };
  for (const auto &input : rejected) {
    CAPTURE(input.mode, input.newline, input.status);
    const auto parsed =
        parse_payload(input.mode, input.text, input.newline, Newline::Session);
    CHECK(parsed.status == input.status);
    CHECK(parsed.bytes.empty());
  }
}

TEST_CASE("quick send owns its execution and validates the selected slot",
          "[scheduler]") {
  QuickSendSnapshot slots;
  QuickSendSlot slot;
  slot.index = 20U;
  slot.name = "query";
  slot.mode = SendMode::Hex;
  slot.content = "41 54";
  slot.newline = Newline::Session;
  slot.note = "snapshot";
  slots.slots[19] = slot;

  auto built = make_quick_send_execution(slots, 20U, Newline::Cr);
  REQUIRE(built);
  REQUIRE(built.execution->slot_index == 20U);
  REQUIRE(built.execution->effective_newline == Newline::Cr);
  REQUIRE(built.execution->bytes == std::vector<std::byte>{std::byte{'A'},
                                                           std::byte{'T'},
                                                           std::byte{'\r'}});

  slots.slots[19]->content = "00";
  REQUIRE(built.execution->bytes == std::vector<std::byte>{std::byte{'A'},
                                                           std::byte{'T'},
                                                           std::byte{'\r'}});
  REQUIRE(make_quick_send_execution(slots, 0U, Newline::None).status ==
          QuickSendBuildStatus::SlotOutOfRange);
  REQUIRE(make_quick_send_execution(slots, 1U, Newline::None).status ==
          QuickSendBuildStatus::EmptySlot);
}

TEST_CASE("scheduler validates intervals and retires a one-shot task",
          "[scheduler]") {
  Scheduler scheduler;
  REQUIRE(scheduler.start(request(9U), at(0)).status ==
          TaskStartStatus::InvalidInterval);
  REQUIRE(scheduler.snapshot().state == SchedulerState::Idle);
  const auto started = scheduler.start(request(0U), at(0));
  REQUIRE(started.status == TaskStartStatus::Started);
  REQUIRE_FALSE(scheduler.next_deadline());

  const auto send = scheduler.on_tx_boundary(at(0));
  REQUIRE(send);
  REQUIRE(send->token.generation == started.started_generation);
  REQUIRE_FALSE(scheduler.on_tx_boundary(at(0)));
  REQUIRE(scheduler.snapshot().outstanding);

  REQUIRE_FALSE(
      scheduler.on_tx_boundary(at(1), TxBoundary{send->token, true, false}));
  REQUIRE(scheduler.snapshot().state == SchedulerState::Idle);
  REQUIRE_FALSE(scheduler.accepts(send->token.generation));
}

TEST_CASE("fixed rate deadlines do not drift or accumulate requests",
          "[scheduler]") {
  auto scheduler = running();
  const auto first = scheduler.on_tx_boundary(at(0));
  REQUIRE(first);

  scheduler.on_deadline(at(10));
  scheduler.on_deadline(at(39));
  REQUIRE(scheduler.snapshot().missed_count == 3U);
  REQUIRE(scheduler.next_deadline() == at(40));
  REQUIRE_FALSE(
      scheduler.on_tx_boundary(at(39), TxBoundary{std::nullopt, false, false}));

  REQUIRE_FALSE(
      scheduler.on_tx_boundary(at(39), TxBoundary{first->token, true, false}));
  REQUIRE(scheduler.snapshot().sent_count == 1U);

  scheduler.on_deadline(at(75));
  REQUIRE(scheduler.snapshot().missed_count == 6U);
  REQUIRE(scheduler.next_deadline() == at(80));
  const auto second = scheduler.on_tx_boundary(at(75));
  REQUIRE(second);
  REQUIRE(second->token.sequence == 2U);

  REQUIRE_FALSE(scheduler.on_tx_boundary(
      at(76), TxBoundary{second->token, false, false}));
  REQUIRE(scheduler.snapshot().sent_count == 1U);
}

TEST_CASE("manual priority preserves only the initial pending trigger",
          "[scheduler]") {
  for (const auto [initial, explicit_deadline] :
       {std::pair{true, false}, std::pair{true, true},
        std::pair{false, false}}) {
    CAPTURE(initial, explicit_deadline);
    auto scheduler = running();
    if (!initial) {
      const auto first = scheduler.on_tx_boundary(at(0));
      REQUIRE(first);
      REQUIRE_FALSE(scheduler.on_tx_boundary(
          at(1), TxBoundary{first->token, true, false}));
    }
    if (explicit_deadline)
      scheduler.on_deadline(at(35));
    REQUIRE_FALSE(scheduler.on_tx_boundary(
        at(35), TxBoundary{std::nullopt, false, true}));
    CHECK(scheduler.snapshot().missed_count == 3U);
    CHECK(scheduler.snapshot().trigger_pending == initial);
    CHECK(scheduler.next_deadline() == at(40));
    const auto boundary = scheduler.on_tx_boundary(at(36));
    if (initial) {
      REQUIRE(boundary);
      CHECK(boundary->token.sequence == 1U);
      scheduler.on_deadline(at(40));
      CHECK_FALSE(scheduler.on_tx_boundary(
          at(41), TxBoundary{boundary->token, true, true}));
      CHECK_FALSE(scheduler.snapshot().trigger_pending);
    } else {
      CHECK_FALSE(boundary);
      const auto next = scheduler.on_tx_boundary(at(40));
      REQUIRE(next);
      CHECK(next->token.sequence == 2U);
    }
  }
}

TEST_CASE("replacement requires confirmation and rejects stale completion",
          "[scheduler]") {
  Scheduler scheduler;
  const auto old = scheduler.start(request(10U, 1U), at(0));
  REQUIRE(old.started_generation == TaskGeneration{1U});
  const auto old_send = scheduler.on_tx_boundary(at(0));
  REQUIRE(old_send);

  const auto unconfirmed = scheduler.start(request(20U, 2U), at(1));
  REQUIRE(unconfirmed.status ==
          TaskStartStatus::ReplacementConfirmationRequired);
  REQUIRE(scheduler.accepts(TaskGeneration{1U}));

  const auto replacing = scheduler.start(request(20U, 2U), at(1), true);
  REQUIRE(replacing.status == TaskStartStatus::ReplacementStopRequested);
  REQUIRE(replacing.generation_to_stop == TaskGeneration{1U});
  REQUIRE_FALSE(scheduler.accepts(TaskGeneration{1U}));
  REQUIRE_FALSE(scheduler.on_tx_boundary(
      at(2), TxBoundary{old_send->token, true, false}));

  REQUIRE(scheduler.confirm_stopped(TaskGeneration{99U}, at(3)).status ==
          StopConfirmationStatus::IgnoredStale);
  const auto confirmed = scheduler.confirm_stopped(TaskGeneration{1U}, at(3));
  REQUIRE(confirmed.status == StopConfirmationStatus::ReplacementStarted);
  REQUIRE(confirmed.started_generation == TaskGeneration{2U});

  const auto replacement = scheduler.on_tx_boundary(at(3));
  REQUIRE(replacement);
  REQUIRE(replacement->token.generation == TaskGeneration{2U});
  REQUIRE(replacement->execution->slot_index == 2U);

  REQUIRE_FALSE(scheduler.on_tx_boundary(
      at(4), TxBoundary{old_send->token, true, false}));
  REQUIRE(scheduler.snapshot().outstanding);
}

TEST_CASE("stop and disconnect invalidate generation until owner confirms",
          "[scheduler]") {
  Scheduler scheduler;
  REQUIRE(scheduler.start(request(10U), at(0)).started_generation ==
          TaskGeneration{1U});
  const auto send = scheduler.on_tx_boundary(at(0));
  REQUIRE(send);

  const auto stopping = scheduler.request_stop();
  REQUIRE(stopping.status == TaskInvalidationStatus::StopRequested);
  REQUIRE(stopping.generation_to_stop == TaskGeneration{1U});
  scheduler.on_deadline(at(100));
  REQUIRE_FALSE(
      scheduler.on_tx_boundary(at(100), TxBoundary{send->token, true, false}));
  REQUIRE(scheduler.snapshot().state == SchedulerState::Stopping);

  REQUIRE(scheduler.confirm_stopped(TaskGeneration{1U}, at(101)).status ==
          StopConfirmationStatus::Stopped);
  REQUIRE(scheduler.snapshot().state == SchedulerState::Idle);
  scheduler.on_deadline(at(1000));
  REQUIRE_FALSE(scheduler.on_tx_boundary(at(1000)));

  REQUIRE(scheduler.start(request(10U), at(2000)).started_generation ==
          TaskGeneration{2U});
  REQUIRE(scheduler.invalidate_for_disconnect().generation_to_stop ==
          TaskGeneration{2U});
  REQUIRE_FALSE(scheduler.accepts(TaskGeneration{2U}));
}

TEST_CASE("scheduler exhaustion fails activation or requests an explicit stop",
          "[scheduler]") {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  Scheduler exhausted{TaskGeneration{maximum}};
  CHECK(exhausted.start(request(10U), at(0)).status ==
        TaskStartStatus::GenerationOverflow);
  CHECK(exhausted.snapshot().state == SchedulerState::Idle);
  Scheduler rejected;
  CHECK(rejected.start(request(10U), Scheduler::TimePoint::max()).status ==
        TaskStartStatus::DeadlineOverflow);
  CHECK(rejected.snapshot().state == SchedulerState::Idle);
  const auto period =
      std::chrono::duration_cast<Scheduler::Clock::duration>(10ms);
  for (const auto scenario :
       {"last deadline", "full clock range", "sequence"}) {
    CAPTURE(scenario);
    const bool sequence = std::string_view{scenario} == "sequence";
    const bool full_range = std::string_view{scenario} == "full clock range";
    Scheduler scheduler{TaskGeneration{}, sequence ? maximum : 0U};
    const auto start = sequence     ? at(0)
                       : full_range ? Scheduler::TimePoint::min()
                                    : Scheduler::TimePoint::max() - period;
    const auto started = scheduler.start(request(10U), start);
    REQUIRE(started.status == TaskStartStatus::Started);
    if (sequence) {
      REQUIRE_FALSE(scheduler.on_tx_boundary(start));
    } else {
      if (!full_range)
        CHECK(scheduler.next_deadline() == Scheduler::TimePoint::max());
      scheduler.on_deadline(Scheduler::TimePoint::max());
      CHECK(scheduler.snapshot().missed_count > 0U);
    }
    CHECK(scheduler.snapshot().state == SchedulerState::Stopping);
    const auto stop = scheduler.take_automatic_stop_request();
    REQUIRE(stop);
    CHECK(stop->generation == started.started_generation);
    CHECK(stop->reason == (sequence ? AutomaticStopReason::SequenceOverflow
                                    : AutomaticStopReason::DeadlineOverflow));
    CHECK_FALSE(scheduler.take_automatic_stop_request());
    CHECK_FALSE(scheduler.accepts(*started.started_generation));
    CHECK(
        scheduler.confirm_stopped(stop->generation, Scheduler::TimePoint::max())
            .status == StopConfirmationStatus::Stopped);
    CHECK(scheduler.snapshot().state == SchedulerState::Idle);
  }
}
