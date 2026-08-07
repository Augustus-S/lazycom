#include <lazycom/scheduler/scheduler.hpp>

#include <support/fake_clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lazycom::TaskGeneration;
using lazycom::config::Newline;
using lazycom::config::QuickSendSlot;
using lazycom::config::QuickSendSnapshot;
using lazycom::config::SendMode;
using namespace lazycom::scheduler;

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

} // namespace

TEST_CASE("TXT and HEX parsing is atomic and appends resolved newlines",
          "[scheduler]") {
  const auto text =
      parse_payload(SendMode::Txt, "A\nB", Newline::Session, Newline::CrLf);
  REQUIRE(text);
  REQUIRE(text.bytes == std::vector<std::byte>{std::byte{'A'}, std::byte{'\n'},
                                               std::byte{'B'}, std::byte{'\r'},
                                               std::byte{'\n'}});

  const auto hex =
      parse_payload(SendMode::Hex, " 0x00\tFF\r0X7a\n 01 ", Newline::Lf);
  REQUIRE(hex);
  REQUIRE(hex.bytes == std::vector<std::byte>{std::byte{0x00}, std::byte{0xFF},
                                              std::byte{0x7A}, std::byte{0x01},
                                              std::byte{'\n'}});

  for (const std::string malformed :
       {"0", "0001", "0x", "0x0", "gg", "01\v02", "0xx1", "01,02"}) {
    const auto parsed = parse_payload(SendMode::Hex, malformed, Newline::None);
    REQUIRE_FALSE(parsed);
    REQUIRE(parsed.status == PayloadParseStatus::InvalidHex);
    REQUIRE(parsed.bytes.empty());
  }
}

TEST_CASE("payload failures always have zero output", "[scheduler]") {
  const auto invalid_text =
      parse_payload(SendMode::Txt, std::string{"\xC0\x80", 2}, Newline::CrLf);
  REQUIRE(invalid_text.status == PayloadParseStatus::InvalidText);
  REQUIRE(invalid_text.bytes.empty());

  const auto unresolved =
      parse_payload(SendMode::Txt, "x", Newline::Session, Newline::Session);
  REQUIRE(unresolved.status == PayloadParseStatus::InvalidNewline);
  REQUIRE(unresolved.bytes.empty());

  const auto invalid_mode = parse_payload(static_cast<SendMode>(99), "x",
                                          Newline::None, Newline::None);
  REQUIRE(invalid_mode.status == PayloadParseStatus::InvalidMode);
  REQUIRE(invalid_mode.bytes.empty());

  const std::string maximum(lazycom::config::kMaximumPayloadBytes, 'x');
  const auto too_large =
      parse_payload(SendMode::Txt, maximum, Newline::Lf, Newline::None);
  REQUIRE(too_large.status == PayloadParseStatus::TooLarge);
  REQUIRE(too_large.bytes.empty());

  std::string oversized_invalid(lazycom::config::kMaximumPayloadBytes + 1U,
                                'x');
  oversized_invalid.back() = static_cast<char>(0xFF);
  const auto invalid_before_size = parse_payload(
      SendMode::Txt, oversized_invalid, Newline::None, Newline::None);
  REQUIRE(invalid_before_size.status == PayloadParseStatus::InvalidText);
  REQUIRE(invalid_before_size.bytes.empty());
}

TEST_CASE("every newline policy resolves to its exact byte suffix",
          "[scheduler]") {
  const auto none =
      parse_payload(SendMode::Txt, "x", Newline::None, Newline::CrLf);
  const auto lf = parse_payload(SendMode::Txt, "x", Newline::Lf);
  const auto cr = parse_payload(SendMode::Txt, "x", Newline::Cr);
  const auto crlf = parse_payload(SendMode::Txt, "x", Newline::CrLf);
  const auto session =
      parse_payload(SendMode::Txt, "x", Newline::Session, Newline::Lf);

  REQUIRE(none.bytes == std::vector<std::byte>{std::byte{'x'}});
  REQUIRE(lf.bytes == std::vector<std::byte>{std::byte{'x'}, std::byte{'\n'}});
  REQUIRE(cr.bytes == std::vector<std::byte>{std::byte{'x'}, std::byte{'\r'}});
  REQUIRE(crlf.bytes == std::vector<std::byte>{std::byte{'x'}, std::byte{'\r'},
                                               std::byte{'\n'}});
  REQUIRE(session.bytes ==
          std::vector<std::byte>{std::byte{'x'}, std::byte{'\n'}});
}

TEST_CASE("quick send builds an owned execution from one of twenty slots",
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

TEST_CASE("scheduler accepts only zero or 10ms through one day",
          "[scheduler]") {
  STATIC_REQUIRE(Scheduler::valid_interval(0U));
  STATIC_REQUIRE_FALSE(Scheduler::valid_interval(1U));
  STATIC_REQUIRE_FALSE(Scheduler::valid_interval(9U));
  STATIC_REQUIRE(Scheduler::valid_interval(10U));
  STATIC_REQUIRE(Scheduler::valid_interval(86'400'000U));
  STATIC_REQUIRE_FALSE(Scheduler::valid_interval(86'400'001U));

  Scheduler scheduler;
  REQUIRE(scheduler.start(request(9U), at(0)).status ==
          TaskStartStatus::InvalidInterval);
  REQUIRE(scheduler.snapshot().state == SchedulerState::Idle);
}

TEST_CASE("zero interval emits immediately once at a TX boundary",
          "[scheduler]") {
  Scheduler scheduler;
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

TEST_CASE("busy immediate send waits for one boundary and misses later ticks",
          "[scheduler]") {
  lazycom::test::FakeClock clock;
  Scheduler scheduler;
  const auto started = scheduler.start(request(10U), clock.now());
  REQUIRE(started.status == TaskStartStatus::Started);

  clock.advance(35ms);
  scheduler.on_deadline(clock.now());
  REQUIRE(scheduler.snapshot().missed_count == 3U);
  REQUIRE(scheduler.next_deadline() == at(40));

  const auto send = scheduler.on_tx_boundary(clock.now());
  REQUIRE(send);
  REQUIRE(send->token.sequence == 1U);
  clock.advance(1ms);
  REQUIRE_FALSE(scheduler.on_tx_boundary(clock.now()));
}

TEST_CASE("fixed rate deadlines do not drift or accumulate requests",
          "[scheduler]") {
  Scheduler scheduler;
  REQUIRE(scheduler.start(request(10U), at(0)).status ==
          TaskStartStatus::Started);
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

TEST_CASE("manual requests win only when the writer reaches a boundary",
          "[scheduler]") {
  Scheduler scheduler;
  REQUIRE(scheduler.start(request(10U), at(0)).status ==
          TaskStartStatus::Started);

  REQUIRE_FALSE(
      scheduler.on_tx_boundary(at(0), TxBoundary{std::nullopt, false, true}));
  REQUIRE(scheduler.snapshot().trigger_pending);
  const auto first = scheduler.on_tx_boundary(at(1));
  REQUIRE(first);

  scheduler.on_deadline(at(10));
  REQUIRE(scheduler.snapshot().missed_count == 1U);
  REQUIRE_FALSE(
      scheduler.on_tx_boundary(at(11), TxBoundary{first->token, true, true}));
  REQUIRE_FALSE(scheduler.snapshot().trigger_pending);
}

TEST_CASE("replacement requires confirmation and old generation stops first",
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

TEST_CASE("generation overflow rejects activation without wrapping",
          "[scheduler]") {
  Scheduler scheduler{
      TaskGeneration{std::numeric_limits<std::uint64_t>::max()}};
  REQUIRE(scheduler.start(request(10U), at(0)).status ==
          TaskStartStatus::GenerationOverflow);
  REQUIRE(scheduler.snapshot().state == SchedulerState::Idle);
}

TEST_CASE("unrepresentable scheduler deadlines fail or request a stop",
          "[scheduler]") {
  Scheduler scheduler;
  const auto rejected =
      scheduler.start(request(10U), Scheduler::TimePoint::max());
  REQUIRE(rejected.status == TaskStartStatus::DeadlineOverflow);
  REQUIRE(scheduler.snapshot().state == SchedulerState::Idle);

  const auto period = std::chrono::duration_cast<Scheduler::Clock::duration>(
      std::chrono::milliseconds{10});
  const auto started =
      scheduler.start(request(10U), Scheduler::TimePoint::max() - period);
  REQUIRE(started.status == TaskStartStatus::Started);
  REQUIRE(scheduler.next_deadline() == Scheduler::TimePoint::max());

  scheduler.on_deadline(Scheduler::TimePoint::max());
  REQUIRE(scheduler.snapshot().state == SchedulerState::Stopping);
  const auto stop = scheduler.take_automatic_stop_request();
  REQUIRE(stop);
  REQUIRE(stop->generation == started.started_generation);
  REQUIRE(stop->reason == AutomaticStopReason::DeadlineOverflow);
  REQUIRE_FALSE(scheduler.take_automatic_stop_request());
  REQUIRE(scheduler
              .confirm_stopped(*started.started_generation,
                               Scheduler::TimePoint::max())
              .status == StopConfirmationStatus::Stopped);
  REQUIRE(scheduler.snapshot().state == SchedulerState::Idle);
}

TEST_CASE("deadline accounting is safe across the full clock range",
          "[scheduler]") {
  Scheduler scheduler;
  const auto started =
      scheduler.start(request(10U), Scheduler::TimePoint::min());
  REQUIRE(started.status == TaskStartStatus::Started);

  scheduler.on_deadline(Scheduler::TimePoint::max());
  const auto snapshot = scheduler.snapshot();
  REQUIRE(snapshot.state == SchedulerState::Stopping);
  REQUIRE(snapshot.missed_count > 0U);
  const auto stop = scheduler.take_automatic_stop_request();
  REQUIRE(stop);
  REQUIRE(stop->reason == AutomaticStopReason::DeadlineOverflow);
}

TEST_CASE("request sequence exhaustion emits an explicit stop request",
          "[scheduler]") {
  Scheduler scheduler{TaskGeneration{},
                      std::numeric_limits<std::uint64_t>::max()};
  const auto started = scheduler.start(request(10U), at(0));
  REQUIRE(started.status == TaskStartStatus::Started);
  REQUIRE_FALSE(scheduler.on_tx_boundary(at(0)));
  REQUIRE(scheduler.snapshot().state == SchedulerState::Stopping);

  const auto stop = scheduler.take_automatic_stop_request();
  REQUIRE(stop);
  REQUIRE(stop->generation == started.started_generation);
  REQUIRE(stop->reason == AutomaticStopReason::SequenceOverflow);
  REQUIRE_FALSE(scheduler.accepts(*started.started_generation));
  REQUIRE(
      scheduler.confirm_stopped(*started.started_generation, at(1)).status ==
      StopConfirmationStatus::Stopped);
}
