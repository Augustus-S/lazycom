#include <lazycom/encoding/display.hpp>
#include <lazycom/encoding/tx.hpp>
#include <lazycom/framing/rx_framer.hpp>
#include <lazycom/model/memory_budget.hpp>

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
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using lazycom::framing::ObservationClock;
using lazycom::model::BudgetCategory;
using namespace lazycom::model;
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

TEST_CASE("RX framing reconstructs seeded random streams",
          "[data-path][framing][property]") {
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
      const auto count =
          std::min(available, static_cast<std::size_t>(generator() % 31U) + 1U);
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

TEST_CASE("memory budget rejects overcommit and releases moved reservations",
          "[data-path][budget]") {
  auto limits = BudgetLimits::defaults();
  limits.category[static_cast<std::size_t>(BudgetCategory::UiRecords)] = 3U;
  GlobalMemoryBudget budget{limits};
  {
    auto first = budget.try_reserve(BudgetCategory::UiRecords, 3U);
    REQUIRE(first);
    REQUIRE_FALSE(budget.try_reserve(BudgetCategory::UiRecords, 1U));
    auto second = std::move(*first);
    CHECK(budget.total_used() == 3U);
    second.reset();
  }
  CHECK(budget.total_used() == 0U);
  limits = BudgetLimits::defaults();
  ++limits.category[0];
  REQUIRE_THROWS_AS(GlobalMemoryBudget{limits}, std::invalid_argument);
}
