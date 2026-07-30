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
using lazycom::model::QueuePushResult;

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
  destination.insert(destination.end(),
                     std::make_move_iterator(source.begin()),
                     std::make_move_iterator(source.end()));
}

class ScriptedSink final : public lazycom::model::IRecordSink {
public:
  std::deque<QueuePushResult> script;
  std::vector<lazycom::model::SinkEnvelope> accepted;
  std::size_t calls{};

  [[nodiscard]] QueuePushResult
  push(const lazycom::model::SinkEnvelope &envelope,
       const std::size_t) override {
    ++calls;
    const auto result = script.empty() ? QueuePushResult::Accepted
                                       : script.front();
    if (!script.empty()) {
      script.pop_front();
    }
    if (result == QueuePushResult::Accepted) {
      accepted.push_back(envelope);
    }
    return result;
  }
};

[[nodiscard]] lazycom::model::RecordDraft
rx_draft(const lazycom::model::SharedPayloadPtr &payload) {
  lazycom::model::RecordDraft draft;
  draft.direction = Direction::Rx;
  draft.time = {"2026-07-26T00:00:00Z", 1U};
  draft.payload = lazycom::model::ByteSlice{payload};
  return draft;
}

} // namespace

TEST_CASE("RX framer handles delimiters pending CR idle and maximum priority",
          "[data-path][framing]") {
  const auto now = ObservationClock::time_point{};

  SECTION("CRLF is joined across reads") {
    lazycom::framing::RxFramer framer{{50ms, 64U}};
    REQUIRE(framer.push(bytes("abc\r"), now).empty());
    const auto frames = framer.push(bytes("\ndef\n"), now + 1ms);
    REQUIRE(frames.size() == 2U);
    REQUIRE(frames[0].bytes == bytes("abc\r\n"));
    REQUIRE(frames[1].bytes == bytes("def\n"));
  }

  SECTION("pending CR is emitted before a non-LF byte") {
    lazycom::framing::RxFramer framer{{50ms, 64U}};
    REQUIRE(framer.push(bytes("a\r"), now).empty());
    const auto frames = framer.push(bytes("b"), now + 1ms);
    REQUIRE(frames.size() == 1U);
    REQUIRE(frames[0].bytes == bytes("a\r"));
    REQUIRE(framer.flush()[0].bytes == bytes("b"));
  }

  SECTION("idle and disconnect flush pending and tail data") {
    lazycom::framing::RxFramer framer{{50ms, 64U}};
    REQUIRE(framer.push(bytes("tail\r"), now).empty());
    REQUIRE(framer.on_idle(now + 49ms).empty());
    const auto idle = framer.on_idle(now + 50ms);
    REQUIRE(idle.size() == 1U);
    REQUIRE(idle[0].bytes == bytes("tail\r"));
    REQUIRE(framer.push(bytes("last"), now + 51ms).empty());
    REQUIRE(framer.flush()[0].bytes == bytes("last"));
  }

  SECTION("maximum frame wins over CRLF") {
    lazycom::framing::RxFramer framer{{50ms, 2U}};
    auto frames = framer.push(bytes("A\r\n"), now);
    REQUIRE(frames.size() == 2U);
    REQUIRE(frames[0].bytes == bytes("A\r"));
    REQUIRE(frames[1].bytes == bytes("\n"));
  }
}

TEST_CASE("all input chunk boundaries preserve the exact RX stream",
          "[data-path][framing][property]") {
  const auto input = bytes("A\r\nB\rC\nD\r\n");
  const auto now = ObservationClock::time_point{};
  const auto boundary_count = input.size() - 1U;
  const auto combinations = std::uint64_t{1U} << boundary_count;

  for (std::size_t maximum = 1U; maximum <= input.size(); ++maximum) {
    for (std::uint64_t mask = 0U; mask < combinations; ++mask) {
      lazycom::framing::RxFramer framer{{50ms, maximum}};
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
      append(frames, framer.push(std::span<const std::byte>{input}.subspan(start),
                                 now + 20ms));
      append(frames, framer.flush());
      REQUIRE(join(frames) == input);
      REQUIRE(std::ranges::all_of(frames, [maximum](const auto &frame) {
        return !frame.bytes.empty() && frame.bytes.size() <= maximum;
      }));
    }
  }
}

TEST_CASE("random RX streams reconstruct without loss",
          "[data-path][framing][property]") {
  std::mt19937 generator{0x4C415A59U};
  const auto now = ObservationClock::time_point{};
  for (std::size_t iteration = 0U; iteration < 200U; ++iteration) {
    const auto length = static_cast<std::size_t>(generator() % 1024U);
    const auto maximum =
        static_cast<std::size_t>(generator() % 64U) + 1U;
    std::vector<std::byte> input(length);
    for (auto &byte : input) {
      byte = static_cast<std::byte>(generator() % 256U);
    }
    lazycom::framing::RxFramer framer{{50ms, maximum}};
    std::vector<lazycom::framing::RxFrame> frames;
    std::size_t offset = 0U;
    while (offset < input.size()) {
      const auto available = input.size() - offset;
      const auto count = std::min(
          available, static_cast<std::size_t>(generator() % 31U) + 1U);
      append(frames,
             framer.push(std::span<const std::byte>{input}.subspan(offset, count),
                         now + std::chrono::milliseconds{offset}));
      offset += count;
    }
    append(frames, framer.flush());
    REQUIRE(join(frames) == input);
  }
}

TEST_CASE("safe display covers every octet and incremental UTF-8",
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
  REQUIRE(display.append(
              std::array{std::byte{0xE2}, std::byte{0x82}})
              .empty());
  REQUIRE(display.has_incomplete_sequence());
  REQUIRE(display.append(std::array{std::byte{0xAC}}) == "\xE2\x82\xAC");
  REQUIRE(display.finish_frame().empty());

  lazycom::encoding::SafeUtf8Display incomplete;
  REQUIRE(incomplete.append(std::array{std::byte{0xF0}}).empty());
  REQUIRE(incomplete.finish_frame() == "\\xF0");

  lazycom::encoding::SafeUtf8Display invalid_continuation;
  REQUIRE(invalid_continuation.append(
              std::array{std::byte{0xE2}, std::byte{'A'}}) ==
          "\\xE2A");
  REQUIRE(invalid_continuation.finish_frame().empty());

  const auto bidi = std::array{std::byte{0xE2}, std::byte{0x80},
                               std::byte{0xAE}};
  REQUIRE(lazycom::encoding::render_text(bidi) == "\\u{202E}");
  REQUIRE(lazycom::encoding::render_text(bytes("\\")) == "\\\\");
  REQUIRE(lazycom::encoding::render(bytes("A"),
                                    lazycom::encoding::DisplayMode::Mixed) ==
          "A | 41");
}

TEST_CASE("TXT and HEX parsers reject partial or malformed input",
          "[data-path][tx]") {
  const auto text = lazycom::encoding::parse_text("ready\r\n");
  REQUIRE(text);
  REQUIRE(*text == bytes("ready\r\n"));
  REQUIRE_FALSE(lazycom::encoding::parse_text(
      std::string_view{"\xC0\xAF", 2U}));

  const auto hex = lazycom::encoding::parse_hex("0x00 FF\t7e\r\nA5");
  REQUIRE(hex);
  REQUIRE(*hex ==
          std::vector<std::byte>{std::byte{0x00}, std::byte{0xFF},
                                 std::byte{0x7E}, std::byte{0xA5}});
  REQUIRE_FALSE(lazycom::encoding::parse_hex("00 1 02"));
  REQUIRE_FALSE(lazycom::encoding::parse_hex("0011"));
}

TEST_CASE("shared payload budget is charged once and released once",
          "[data-path][budget]") {
  lazycom::model::GlobalMemoryBudget budget;
  {
    const auto payload = lazycom::model::make_shared_payload(
        budget, BudgetCategory::RxIngress, bytes("payload"));
    REQUIRE(payload);
    REQUIRE(budget.used(BudgetCategory::RxIngress) == 7U);
    {
      const auto first_sink = payload;
      const auto second_sink = payload;
      REQUIRE(first_sink == second_sink);
      REQUIRE(budget.used(BudgetCategory::RxIngress) == 7U);
    }
  }
  REQUIRE(budget.used(BudgetCategory::RxIngress) == 0U);
  REQUIRE(budget.total_used() == 0U);
}

TEST_CASE("budget rejects before construction and returns all reservations",
          "[data-path][budget]") {
  auto limits = lazycom::model::BudgetLimits::defaults();
  limits.category[static_cast<std::size_t>(BudgetCategory::RxIngress)] = 3U;
  lazycom::model::GlobalMemoryBudget budget{limits};
  REQUIRE_FALSE(lazycom::model::make_shared_payload(
      budget, BudgetCategory::RxIngress, bytes("four")));
  REQUIRE(budget.total_used() == 0U);
  {
    auto reservation = budget.try_reserve(BudgetCategory::RxIngress, 3U);
    REQUIRE(reservation);
    REQUIRE_FALSE(budget.try_reserve(BudgetCategory::RxIngress, 1U));
  }
  REQUIRE(budget.total_used() == 0U);
}

TEST_CASE("immutable batch returns metadata budget after every sink releases it",
          "[data-path][budget]") {
  lazycom::model::GlobalMemoryBudget budget;
  const auto payload = lazycom::model::make_shared_payload(
      budget, BudgetCategory::RxIngress, bytes("x"));
  ScriptedSink ui;
  ScriptedSink log;
  lazycom::model::SessionSequencer sequencer{lazycom::SessionId{1U}, budget,
                                             &ui, &log};
  auto result = sequencer.submit(lazycom::SessionId{1U},
                                 SessionEventOrigin::Normal,
                                 {rx_draft(payload)});
  REQUIRE(result.status == lazycom::model::SequenceStatus::Accepted);
  REQUIRE(budget.used(BudgetCategory::OwnerScratch) > 0U);
  result = {};
  ui.accepted.clear();
  REQUIRE(budget.used(BudgetCategory::OwnerScratch) > 0U);
  log.accepted.clear();
  REQUIRE(budget.used(BudgetCategory::OwnerScratch) == 0U);
}

TEST_CASE("sequencer validates identity and isolates full sinks with gaps",
          "[data-path][sequencer]") {
  lazycom::model::GlobalMemoryBudget budget;
  const auto payload = lazycom::model::make_shared_payload(
      budget, BudgetCategory::RxIngress, bytes("x"));
  ScriptedSink ui;
  ScriptedSink log;
  ui.script = {QueuePushResult::Full, QueuePushResult::Accepted};
  lazycom::model::SessionSequencer sequencer{lazycom::SessionId{7U}, budget,
                                             &ui, &log};

  auto wrong = sequencer.submit(lazycom::SessionId{8U},
                                SessionEventOrigin::Normal,
                                {rx_draft(payload)});
  REQUIRE(wrong.status == lazycom::model::SequenceStatus::WrongSession);
  REQUIRE(sequencer.next_seq() == 1U);

  const auto first = sequencer.submit(lazycom::SessionId{7U},
                                      SessionEventOrigin::Normal,
                                      {rx_draft(payload), rx_draft(payload)});
  REQUIRE(first.status == lazycom::model::SequenceStatus::Accepted);
  REQUIRE(first.first_seq == 1U);
  REQUIRE(first.last_seq == 2U);
  REQUIRE(first.ui.result == QueuePushResult::Full);
  REQUIRE(first.ui.gap == lazycom::model::SequenceGap{1U, 2U});
  REQUIRE(first.log.result == QueuePushResult::Accepted);
  REQUIRE(log.accepted.size() == 1U);

  const auto second = sequencer.submit(lazycom::SessionId{7U},
                                       SessionEventOrigin::Normal,
                                       {rx_draft(payload)});
  REQUIRE(second.first_seq == 3U);
  REQUIRE(second.ui.result == QueuePushResult::Accepted);
  REQUIRE(second.ui.gap == lazycom::model::SequenceGap{1U, 2U});
  REQUIRE(ui.accepted[0].gap_before ==
          lazycom::model::SequenceGap{1U, 2U});
  REQUIRE(log.calls == 2U);

  REQUIRE(sequencer.begin_cleanup(lazycom::SessionId{7U}));
  REQUIRE(sequencer.submit(lazycom::SessionId{7U},
                           SessionEventOrigin::Normal, {rx_draft(payload)})
              .status == lazycom::model::SequenceStatus::InvalidOrigin);
  REQUIRE(sequencer.submit(lazycom::SessionId{7U},
                           SessionEventOrigin::Cleanup, {rx_draft(payload)})
              .first_seq == 4U);
}

TEST_CASE("UI sink evicts oldest batches and preserves a sequence gap",
          "[data-path][sink]") {
  lazycom::model::GlobalMemoryBudget budget;
  const auto payload = lazycom::model::make_shared_payload(
      budget, BudgetCategory::RxIngress, bytes("x"));
  lazycom::model::EvictingUiRecordSink ui{2U, 64U * 1024U};
  ScriptedSink log;
  lazycom::model::SessionSequencer sequencer{lazycom::SessionId{9U}, budget,
                                             &ui, &log};
  for (std::size_t index = 0U; index < 3U; ++index) {
    REQUIRE(sequencer.submit(lazycom::SessionId{9U},
                             SessionEventOrigin::Normal,
                             {rx_draft(payload)})
                .ui.result == QueuePushResult::Accepted);
  }
  REQUIRE(ui.record_count() == 2U);
  const auto first_visible = ui.try_pop();
  REQUIRE(first_visible);
  REQUIRE(first_visible->batch->first_seq() == 2U);
  REQUIRE(first_visible->gap_before ==
          lazycom::model::SequenceGap{1U, 1U});
  REQUIRE(log.calls == 3U);
}

TEST_CASE("TX terminal helper records only accepted prefix then termination",
          "[data-path][tx]") {
  lazycom::model::GlobalMemoryBudget budget;
  const auto payload = lazycom::model::make_shared_payload(
      budget, BudgetCategory::Tx, bytes("abcdef"));
  lazycom::model::TxOperation operation{
      lazycom::OperationId{42U}, lazycom::ConnectionGeneration{2U},
      lazycom::SessionId{7U}, payload, lazycom::logging::InputMode::Text,
      std::chrono::steady_clock::time_point{} + 1s};

  REQUIRE(operation.accept(2U, {"2026-07-26T00:00:01Z", 10U}));
  REQUIRE(operation.accept(1U, {"2026-07-26T00:00:02Z", 20U}));
  REQUIRE_FALSE(operation.accept(4U, {"ignored", 0U}));
  const auto records = operation.terminal_records(
      lazycom::model::TxTermination::TimedOut,
      {"2026-07-26T00:00:03Z", 30U}, "LC-SER-2004", "deadline exceeded");
  REQUIRE(records);
  REQUIRE(records->size() == 2U);
  REQUIRE((*records)[0].direction == Direction::Tx);
  REQUIRE(std::ranges::equal(
      (*records)[0].payload.bytes(), payload->bytes().first(3U)));
  REQUIRE((*records)[0].time.elapsed_ns == 20U);
  REQUIRE((*records)[1].direction == Direction::Err);
  REQUIRE((*records)[1].operation_id == lazycom::OperationId{42U});
  REQUIRE((*records)[1].message.find("operation_id=42") != std::string::npos);

  lazycom::model::TxOperation untouched{
      lazycom::OperationId{43U}, lazycom::ConnectionGeneration{2U},
      lazycom::SessionId{7U}, payload, lazycom::logging::InputMode::Hex,
      std::chrono::steady_clock::time_point{}};
  const auto failed = untouched.terminal_records(
      lazycom::model::TxTermination::Failed,
      {"2026-07-26T00:00:03Z", 30U});
  REQUIRE(failed);
  REQUIRE(failed->size() == 1U);
  REQUIRE((*failed)[0].direction == Direction::Err);
}

TEST_CASE("bounded queue reports accepted full and stopped",
          "[data-path][queue]") {
  lazycom::model::BoundedQueue<int> queue{2U, 3U};
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
