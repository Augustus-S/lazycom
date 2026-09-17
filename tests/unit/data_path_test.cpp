#include <lazycom/encoding/display.hpp>
#include <lazycom/encoding/tx.hpp>
#include <lazycom/framing/rx_framer.hpp>
#include <lazycom/model/data_path.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lazycom::app::SessionEventOrigin;
using lazycom::framing::ObservationClock;
using lazycom::logging::Direction;
using lazycom::model::BudgetCategory;
using namespace lazycom::model;
using lazycom::SessionId;
using lazycom::framing::RxFramer;

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view text) {
  const auto source = std::span<const std::byte>{
      reinterpret_cast<const std::byte *>(text.data()), text.size()};
  return {source.begin(), source.end()};
}

[[nodiscard]] std::vector<std::byte>
join(const std::vector<lazycom::framing::RxFrame> &frames) {
  std::vector<std::byte> output;
  for (const auto &frame : frames) {
    output.insert(output.end(), frame.bytes.begin(), frame.bytes.end());
  }
  return output;
}

void append(std::vector<lazycom::framing::RxFrame> &destination,
            std::vector<lazycom::framing::RxFrame> source) {
  destination.insert(destination.end(), std::make_move_iterator(source.begin()),
                     std::make_move_iterator(source.end()));
}

class ScriptedSink final : public IRecordSink {
public:
  std::deque<QueuePushResult> script;
  std::vector<SinkEnvelope> accepted;
  std::size_t calls{};

  [[nodiscard]] QueuePushResult push(const SinkEnvelope &envelope,
                                     const std::size_t) override {
    ++calls;
    const auto result =
        script.empty() ? QueuePushResult::Accepted : script.front();
    if (!script.empty()) {
      script.pop_front();
    }
    if (result == QueuePushResult::Accepted) {
      accepted.push_back(envelope);
    }
    return result;
  }
};

[[nodiscard]] RecordDraft rx_draft(const SharedPayloadPtr &payload) {
  RecordDraft draft;
  draft.direction = Direction::Rx;
  draft.time = {"2026-07-26T00:00:00Z", 1U};
  draft.payload = ByteSlice{payload};
  return draft;
}

} // namespace

TEST_CASE("RX framing preserves delimiter idle and size boundaries",
          "[data-path][framing]") {
  const auto now = ObservationClock::time_point{};

  SECTION("CRLF is joined across reads") {
    RxFramer framer{{50ms, 64U}};
    REQUIRE(framer.push(bytes("abc\r"), now).empty());
    const auto frames = framer.push(bytes("\ndef\n"), now + 1ms);
    REQUIRE(frames.size() == 2U);
    REQUIRE(frames[0].bytes == bytes("abc\r\n"));
    REQUIRE(frames[1].bytes == bytes("def\n"));
  }

  SECTION("pending CR is emitted before a non-LF byte") {
    RxFramer framer{{50ms, 64U}};
    REQUIRE(framer.push(bytes("a\r"), now).empty());
    const auto frames = framer.push(bytes("b"), now + 1ms);
    REQUIRE(frames.size() == 1U);
    REQUIRE(frames[0].bytes == bytes("a\r"));
    REQUIRE(framer.flush()[0].bytes == bytes("b"));
  }

  SECTION("idle and disconnect flush pending and tail data") {
    RxFramer framer{{50ms, 64U}};
    REQUIRE(framer.push(bytes("tail\r"), now).empty());
    REQUIRE(framer.on_idle(now + 49ms).empty());
    const auto idle = framer.on_idle(now + 50ms);
    REQUIRE(idle.size() == 1U);
    REQUIRE(idle[0].bytes == bytes("tail\r"));
    REQUIRE(framer.push(bytes("last"), now + 51ms).empty());
    REQUIRE(framer.flush()[0].bytes == bytes("last"));
  }

  SECTION("maximum frame wins over CRLF") {
    RxFramer framer{{50ms, 2U}};
    auto frames = framer.push(bytes("A\r\n"), now);
    REQUIRE(frames.size() == 2U);
    REQUIRE(frames[0].bytes == bytes("A\r"));
    REQUIRE(frames[1].bytes == bytes("\n"));
  }
  SECTION("idle checks across extreme observation times") {
    RxFramer framer{{50ms, 64U}};
    REQUIRE(
        framer.push(bytes("old"), ObservationClock::time_point::min()).empty());
    const auto frames =
        framer.push(bytes("new"), ObservationClock::time_point::max());
    REQUIRE(frames.size() == 1U);
    REQUIRE(frames[0].bytes == bytes("old"));
    REQUIRE(framer.flush()[0].bytes == bytes("new"));

    REQUIRE(framer.push(bytes("tail"), ObservationClock::time_point::max())
                .empty());
    REQUIRE(framer.on_idle(ObservationClock::time_point::min()).empty());
    REQUIRE(framer.flush()[0].bytes == bytes("tail"));
  }
}

TEST_CASE("RX framing reconstructs every chunk split and seeded random stream",
          "[data-path][framing][property]") {
  SECTION("all input chunk boundaries preserve the exact RX stream") {
    const auto input = bytes("A\r\nB\rC\nD\r\n");
    const auto now = ObservationClock::time_point{};
    const auto boundary_count = input.size() - 1U;
    const auto combinations = std::uint64_t{1U} << boundary_count;

    for (std::size_t maximum = 1U; maximum <= input.size(); ++maximum) {
      for (std::uint64_t mask = 0U; mask < combinations; ++mask) {
        RxFramer framer{{50ms, maximum}};
        std::vector<lazycom::framing::RxFrame> frames;
        std::size_t start = 0U;
        for (std::size_t boundary = 0U; boundary < boundary_count; ++boundary) {
          if ((mask & (std::uint64_t{1U} << boundary)) != 0U) {
            append(frames,
                   framer.push(std::span<const std::byte>{input}.subspan(
                                   start, boundary + 1U - start),
                               now + std::chrono::milliseconds{boundary}));
            start = boundary + 1U;
          }
        }
        append(frames,
               framer.push(std::span<const std::byte>{input}.subspan(start),
                           now + 20ms));
        append(frames, framer.flush());
        CAPTURE(maximum, mask);
        REQUIRE(join(frames) == input);
        REQUIRE(std::ranges::all_of(frames, [maximum](const auto &frame) {
          return !frame.bytes.empty() && frame.bytes.size() <= maximum;
        }));
      }
    }
  }
  SECTION("random RX streams reconstruct without loss") {
    std::mt19937 generator{0x4C415A59U};
    const auto now = ObservationClock::time_point{};
    for (std::size_t iteration = 0U; iteration < 200U; ++iteration) {
      const auto length = static_cast<std::size_t>(generator() % 1024U);
      const auto maximum = static_cast<std::size_t>(generator() % 64U) + 1U;
      std::vector<std::byte> input(length);
      for (auto &byte : input) {
        byte = static_cast<std::byte>(generator() % 256U);
      }
      RxFramer framer{{50ms, maximum}};
      std::vector<lazycom::framing::RxFrame> frames;
      std::size_t offset = 0U;
      while (offset < input.size()) {
        const auto available = input.size() - offset;
        const auto count = std::min(
            available, static_cast<std::size_t>(generator() % 31U) + 1U);
        append(frames, framer.push(std::span<const std::byte>{input}.subspan(
                                       offset, count),
                                   now + std::chrono::milliseconds{offset}));
        offset += count;
      }
      append(frames, framer.flush());
      CAPTURE(iteration, length, maximum);
      REQUIRE(join(frames) == input);
    }
  }
}

TEST_CASE("RX delimiter floods retain only small frame capacities",
          "[data-path][framing]") {
  constexpr std::size_t maximum = 65'536U;
  std::string input;
  input.reserve(4096U);
  for (std::size_t index = 0U; index < 2048U; ++index) {
    input.append("\r\n");
  }

  RxFramer framer{{50ms, maximum}};
  const auto frames = framer.push(bytes(input), ObservationClock::time_point{});
  REQUIRE(frames.size() == 2048U);
  std::size_t total_capacity = 0U;
  for (const auto &frame : frames) {
    REQUIRE(frame.bytes == bytes("\r\n"));
    REQUIRE(frame.bytes.capacity() < maximum);
    total_capacity += frame.bytes.capacity();
  }
  REQUIRE(total_capacity < maximum);
}

TEST_CASE("encoding projects safe text and parses complete TX payloads",
          "[data-path][encoding]") {
  std::array<std::byte, 256U> all{};
  for (std::size_t value = 0U; value < all.size(); ++value) {
    all[value] = static_cast<std::byte>(value);
  }
  const auto text = lazycom::encoding::render_text(all);
  REQUIRE(text.find('\0') == std::string::npos);
  REQUIRE(text.find('\x1B') == std::string::npos);
  REQUIRE(text.find('\r') == std::string::npos);
  REQUIRE(text.find('\n') == std::string::npos);
  REQUIRE(lazycom::encoding::render_hex(all).size() == 256U * 3U - 1U);

  lazycom::encoding::SafeUtf8Display display;
  REQUIRE(display.append(std::array{std::byte{0xE2}, std::byte{0x82}}).empty());
  REQUIRE(display.has_incomplete_sequence());
  REQUIRE(display.append(std::array{std::byte{0xAC}}) == "\xE2\x82\xAC");
  REQUIRE(display.finish_frame().empty());

  lazycom::encoding::SafeUtf8Display incomplete;
  REQUIRE(incomplete.append(std::array{std::byte{0xF0}}).empty());
  REQUIRE(incomplete.finish_frame() == "\\xF0");

  lazycom::encoding::SafeUtf8Display invalid_continuation;
  REQUIRE(invalid_continuation.append(
              std::array{std::byte{0xE2}, std::byte{'A'}}) == "\\xE2A");
  REQUIRE(invalid_continuation.finish_frame().empty());

  const auto bidi =
      std::array{std::byte{0xE2}, std::byte{0x80}, std::byte{0xAE}};
  REQUIRE(lazycom::encoding::render_text(bidi) == "\\u{202E}");
  REQUIRE(lazycom::encoding::render_text(bytes("\\")) == "\\\\");
  REQUIRE(lazycom::encoding::render(
              bytes("A"), lazycom::encoding::DisplayMode::Mixed) == "A | 41");
  const auto parsed_text = lazycom::encoding::parse_text("ready\r\n");
  REQUIRE(parsed_text);
  REQUIRE(*parsed_text == bytes("ready\r\n"));
  REQUIRE_FALSE(
      lazycom::encoding::parse_text(std::string_view{"\xC0\xAF", 2U}));

  const auto hex = lazycom::encoding::parse_hex("0x00 FF\t7e\r\nA5");
  REQUIRE(hex);
  REQUIRE(*hex == std::vector<std::byte>{std::byte{0x00}, std::byte{0xFF},
                                         std::byte{0x7E}, std::byte{0xA5}});
  for (const std::string_view malformed :
       {"0", "0001", "0x", "0x0", "gg", "01\v02", "0xx1", "01,02", "00 1 02"}) {
    CAPTURE(malformed);
    const auto parsed = lazycom::encoding::parse_hex(malformed);
    REQUIRE_FALSE(parsed);
    CHECK(parsed.error().offset == (malformed == "00 1 02" ? 3U : 0U));
  }
}

TEST_CASE(
    "shared payload and batch budgets follow ownership and reject overcommit",
    "[data-path][budget]") {
  SECTION(
      "payload reservations count overhead once and release on last owner") {
    for (const std::size_t size : {0U, 1U, 7U}) {
      CAPTURE(size);
      GlobalMemoryBudget budget;
      const auto charge = kSharedPayloadFixedBudgetBytes + size;
      {
        const auto payload = make_shared_payload(budget, BudgetCategory::Tx,
                                                 bytes(std::string(size, 'x')));
        REQUIRE(payload);
        CHECK(payload->bytes().size() == size);
        CHECK(payload->budget_bytes() == charge);
        {
          const auto copy = payload;
          CHECK(budget.used(BudgetCategory::Tx) == charge);
        }
        CHECK(budget.used(BudgetCategory::Tx) == charge);
        const auto distinct =
            make_shared_payload(budget, BudgetCategory::Tx, bytes("x"));
        REQUIRE(distinct);
        CHECK(budget.used(BudgetCategory::Tx) ==
              charge + kSharedPayloadFixedBudgetBytes + 1U);
      }
      CHECK(budget.total_used() == 0U);
    }
  }
  SECTION("budget rejects before construction and returns all reservations") {
    auto limits = BudgetLimits::defaults();
    const auto limit = kSharedPayloadFixedBudgetBytes + 3U;
    limits.category[static_cast<std::size_t>(BudgetCategory::RxIngress)] =
        limit;
    GlobalMemoryBudget budget{limits};
    REQUIRE_FALSE(
        make_shared_payload(budget, BudgetCategory::RxIngress, bytes("four")));
    REQUIRE(budget.total_used() == 0U);
    {
      auto reservation = budget.try_reserve(BudgetCategory::RxIngress, limit);
      REQUIRE(reservation);
      REQUIRE_FALSE(budget.try_reserve(BudgetCategory::RxIngress, 1U));
    }
    REQUIRE(budget.total_used() == 0U);
  }
  SECTION(
      "immutable batch returns metadata budget after every sink releases it") {
    GlobalMemoryBudget budget;
    const auto payload =
        make_shared_payload(budget, BudgetCategory::RxIngress, bytes("x"));
    ScriptedSink ui;
    ScriptedSink log;
    SessionSequencer sequencer{SessionId{1U}, budget, &ui, &log};
    auto result = sequencer.submit(SessionId{1U}, SessionEventOrigin::Normal,
                                   {rx_draft(payload)});
    REQUIRE(result.status == SequenceStatus::Accepted);
    REQUIRE(budget.used(BudgetCategory::OwnerScratch) > 0U);
    result = {};
    ui.accepted.clear();
    REQUIRE(budget.used(BudgetCategory::OwnerScratch) > 0U);
    log.accepted.clear();
    REQUIRE(budget.used(BudgetCategory::OwnerScratch) == 0U);
  }
}

TEST_CASE("record draft validation rejects out-of-range enums",
          "[data-path][model]") {
  GlobalMemoryBudget budget;
  const auto payload =
      make_shared_payload(budget, BudgetCategory::RxIngress, bytes("x"));

  auto draft = rx_draft(payload);
  draft.direction = static_cast<Direction>(std::numeric_limits<int>::max());
  REQUIRE_FALSE(valid_record_draft(draft));

  draft = rx_draft(payload);
  draft.direction = Direction::Tx;
  draft.input_mode =
      static_cast<lazycom::logging::InputMode>(std::numeric_limits<int>::max());
  REQUIRE_FALSE(valid_record_draft(draft));

  draft.input_mode = lazycom::logging::InputMode::Text;
  REQUIRE(valid_record_draft(draft));
}

TEST_CASE("sequencer isolates sink loss and keeps session and gap identities",
          "[data-path][sequencer]") {
  SECTION("sequencer validates identity and isolates full sinks with gaps") {
    GlobalMemoryBudget budget;
    const auto payload =
        make_shared_payload(budget, BudgetCategory::RxIngress, bytes("x"));
    ScriptedSink ui;
    ScriptedSink log;
    ui.script = {QueuePushResult::Full, QueuePushResult::Accepted};
    SessionSequencer sequencer{SessionId{7U}, budget, &ui, &log};

    auto wrong = sequencer.submit(SessionId{8U}, SessionEventOrigin::Normal,
                                  {rx_draft(payload)});
    REQUIRE(wrong.status == SequenceStatus::WrongSession);
    REQUIRE(sequencer.next_seq() == 1U);

    const auto first =
        sequencer.submit(SessionId{7U}, SessionEventOrigin::Normal,
                         {rx_draft(payload), rx_draft(payload)});
    REQUIRE(first.status == SequenceStatus::Accepted);
    REQUIRE(first.first_seq == 1U);
    REQUIRE(first.last_seq == 2U);
    REQUIRE(first.ui.result == QueuePushResult::Full);
    REQUIRE(first.ui.gap == SequenceGap{1U, 2U});
    REQUIRE(first.log.result == QueuePushResult::Accepted);
    REQUIRE(log.accepted.size() == 1U);

    const auto second = sequencer.submit(
        SessionId{7U}, SessionEventOrigin::Normal, {rx_draft(payload)});
    REQUIRE(second.first_seq == 3U);
    REQUIRE(second.ui.result == QueuePushResult::Accepted);
    REQUIRE(second.ui.gap == SequenceGap{1U, 2U});
    REQUIRE(ui.accepted[0].gap_before == SequenceGap{1U, 2U});
    REQUIRE(log.calls == 2U);

    REQUIRE(sequencer.begin_cleanup(SessionId{7U}));
    REQUIRE(sequencer
                .submit(SessionId{7U}, SessionEventOrigin::Normal,
                        {rx_draft(payload)})
                .status == SequenceStatus::InvalidOrigin);
    REQUIRE(sequencer
                .submit(SessionId{7U}, SessionEventOrigin::Cleanup,
                        {rx_draft(payload)})
                .first_seq == 4U);
  }
  SECTION("UI sink evicts oldest batches and preserves a sequence gap") {
    GlobalMemoryBudget budget;
    const auto payload =
        make_shared_payload(budget, BudgetCategory::RxIngress, bytes("x"));
    EvictingUiRecordSink ui{2U, 64U * 1024U};
    ScriptedSink log;
    SessionSequencer sequencer{SessionId{9U}, budget, &ui, &log};
    for (std::size_t index = 0U; index < 3U; ++index) {
      REQUIRE(sequencer
                  .submit(SessionId{9U}, SessionEventOrigin::Normal,
                          {rx_draft(payload)})
                  .ui.result == QueuePushResult::Accepted);
    }
    REQUIRE(ui.record_count() == 2U);
    const auto first_visible = ui.try_pop();
    REQUIRE(first_visible);
    REQUIRE(first_visible->batch->first_seq() == 2U);
    REQUIRE(first_visible->gap_before == SequenceGap{1U, 1U});
    REQUIRE(log.calls == 3U);
  }
}

TEST_CASE("TX terminal helper records only accepted prefix then termination",
          "[data-path][tx]") {
  GlobalMemoryBudget budget;
  const auto payload =
      make_shared_payload(budget, BudgetCategory::Tx, bytes("abcdef"));
  TxOperation operation{lazycom::OperationId{42U},
                        lazycom::ConnectionGeneration{2U},
                        SessionId{7U},
                        payload,
                        lazycom::logging::InputMode::Text,
                        std::chrono::steady_clock::time_point{} + 1s};

  REQUIRE(operation.accept(2U, {"2026-07-26T00:00:01Z", 10U}));
  REQUIRE(operation.accept(1U, {"2026-07-26T00:00:02Z", 20U}));
  REQUIRE_FALSE(operation.accept(4U, {"ignored", 0U}));
  const auto records = operation.terminal_records(
      TxTermination::TimedOut, {"2026-07-26T00:00:03Z", 30U}, "LC-SER-2004",
      "deadline exceeded");
  REQUIRE(records);
  REQUIRE(records->size() == 2U);
  REQUIRE((*records)[0].direction == Direction::Tx);
  REQUIRE(std::ranges::equal((*records)[0].payload.bytes(),
                             payload->bytes().first(3U)));
  REQUIRE((*records)[0].time.elapsed_ns == 20U);
  REQUIRE((*records)[1].direction == Direction::Err);
  REQUIRE((*records)[1].operation_id == lazycom::OperationId{42U});
  REQUIRE((*records)[1].message.find("operation_id=42") != std::string::npos);

  TxOperation untouched{lazycom::OperationId{43U},
                        lazycom::ConnectionGeneration{2U},
                        SessionId{7U},
                        payload,
                        lazycom::logging::InputMode::Hex,
                        std::chrono::steady_clock::time_point{}};
  const auto failed = untouched.terminal_records(TxTermination::Failed,
                                                 {"2026-07-26T00:00:03Z", 30U});
  REQUIRE(failed);
  REQUIRE(failed->size() == 1U);
  REQUIRE((*failed)[0].direction == Direction::Err);
}

TEST_CASE("bounded queue reports accepted full and stopped",
          "[data-path][queue]") {
  BoundedQueue<int> queue{2U, 3U};
  REQUIRE(queue.push(1, 2U) == QueuePushResult::Accepted);
  REQUIRE(queue.push(2, 2U) == QueuePushResult::Full);
  REQUIRE(queue.push(3, 1U) == QueuePushResult::Accepted);
  REQUIRE(queue.push(4, 0U) == QueuePushResult::Full);
  const auto first = queue.try_pop();
  REQUIRE(first);
  REQUIRE(first->value == 1);
  REQUIRE(queue.logical_bytes() == 1U);
  queue.stop();
  REQUIRE(queue.push(5, 0U) == QueuePushResult::Stopped);
  REQUIRE(queue.wait_pop()->value == 3);
  REQUIRE_FALSE(queue.wait_pop());
}
