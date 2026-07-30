#include <lazycom/encoding/display.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace lazycom::encoding {
namespace {

[[nodiscard]] unsigned int octet(const std::byte value) noexcept {
  return std::to_integer<unsigned int>(value);
}

[[nodiscard]] std::size_t sequence_length(const unsigned int first) noexcept {
  if (first <= 0x7FU) {
    return 1U;
  }
  if (first >= 0xC2U && first <= 0xDFU) {
    return 2U;
  }
  if (first >= 0xE0U && first <= 0xEFU) {
    return 3U;
  }
  if (first >= 0xF0U && first <= 0xF4U) {
    return 4U;
  }
  return 0U;
}

[[nodiscard]] bool valid_sequence(const std::span<const std::byte> bytes,
                                  const std::size_t length) noexcept {
  for (std::size_t index = 1U; index < length; ++index) {
    const auto value = octet(bytes[index]);
    if (value < 0x80U || value > 0xBFU) {
      return false;
    }
  }
  const auto first = octet(bytes[0]);
  if (length >= 2U) {
    const auto second = octet(bytes[1]);
    if ((first == 0xE0U && second < 0xA0U) ||
        (first == 0xEDU && second > 0x9FU) ||
        (first == 0xF0U && second < 0x90U) ||
        (first == 0xF4U && second > 0x8FU)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool invalid_prefix(const std::span<const std::byte> bytes,
                                  const std::size_t length) noexcept {
  const auto available = std::min(bytes.size(), length);
  for (std::size_t index = 1U; index < available; ++index) {
    const auto value = octet(bytes[index]);
    if (value < 0x80U || value > 0xBFU) {
      return true;
    }
  }
  if (available >= 2U) {
    const auto first = octet(bytes[0]);
    const auto second = octet(bytes[1]);
    return (first == 0xE0U && second < 0xA0U) ||
           (first == 0xEDU && second > 0x9FU) ||
           (first == 0xF0U && second < 0x90U) ||
           (first == 0xF4U && second > 0x8FU);
  }
  return false;
}

[[nodiscard]] std::uint32_t
decode(const std::span<const std::byte> bytes) noexcept {
  const auto length = bytes.size();
  auto value = static_cast<std::uint32_t>(octet(bytes[0]));
  if (length == 1U) {
    return value;
  }
  value &= static_cast<std::uint32_t>(0x7FU >> length);
  for (std::size_t index = 1U; index < length; ++index) {
    value = (value << 6U) |
            static_cast<std::uint32_t>(octet(bytes[index]) & 0x3FU);
  }
  return value;
}

[[nodiscard]] bool is_bidi_control(const std::uint32_t point) noexcept {
  return point == 0x061CU || point == 0x200EU || point == 0x200FU ||
         (point >= 0x202AU && point <= 0x202EU) ||
         (point >= 0x2066U && point <= 0x2069U);
}

[[nodiscard]] bool is_control(const std::uint32_t point) noexcept {
  return point <= 0x1FU || (point >= 0x7FU && point <= 0x9FU);
}

void append_hex_escape(std::string &output, const unsigned int value) {
  static constexpr std::string_view digits = "0123456789ABCDEF";
  output.append("\\x00");
  output[output.size() - 2U] = digits[(value >> 4U) & 0x0FU];
  output[output.size() - 1U] = digits[value & 0x0FU];
}

void append_unicode_escape(std::string &output, std::uint32_t value) {
  static constexpr std::string_view digits = "0123456789ABCDEF";
  std::array<char, 6> reversed{};
  std::size_t count = 0U;
  do {
    reversed[count] = digits[value & 0x0FU];
    ++count;
    value >>= 4U;
  } while (value != 0U);
  output.append("\\u{");
  while (count != 0U) {
    --count;
    output.push_back(reversed[count]);
  }
  output.push_back('}');
}

void append_visible(std::string &output,
                    const std::span<const std::byte> sequence) {
  const auto point = decode(sequence);
  if (is_bidi_control(point)) {
    append_unicode_escape(output, point);
    return;
  }
  if (is_control(point)) {
    for (const auto byte : sequence) {
      append_hex_escape(output, octet(byte));
    }
    return;
  }
  if (point == static_cast<std::uint32_t>('\\')) {
    output.append("\\\\");
    return;
  }
  output.append(reinterpret_cast<const char *>(sequence.data()),
                sequence.size());
}

} // namespace

std::string SafeUtf8Display::append(const std::span<const std::byte> bytes) {
  pending_.insert(pending_.end(), bytes.begin(), bytes.end());
  std::string output;
  std::size_t consumed = 0U;
  while (consumed < pending_.size()) {
    const auto length = sequence_length(octet(pending_[consumed]));
    if (length == 0U) {
      append_hex_escape(output, octet(pending_[consumed]));
      ++consumed;
      continue;
    }
    const auto remaining =
        std::span<const std::byte>{pending_}.subspan(consumed);
    if (invalid_prefix(remaining, length)) {
      append_hex_escape(output, octet(pending_[consumed]));
      ++consumed;
      continue;
    }
    if (length > pending_.size() - consumed) {
      break;
    }
    const auto sequence = remaining.first(length);
    if (!valid_sequence(sequence, length)) {
      append_hex_escape(output, octet(pending_[consumed]));
      ++consumed;
      continue;
    }
    append_visible(output, sequence);
    consumed += length;
  }
  pending_.erase(pending_.begin(),
                 pending_.begin() + static_cast<std::ptrdiff_t>(consumed));
  return output;
}

std::string SafeUtf8Display::finish_frame() {
  std::string output;
  for (const auto byte : pending_) {
    append_hex_escape(output, octet(byte));
  }
  pending_.clear();
  return output;
}

std::string render_text(const std::span<const std::byte> bytes) {
  SafeUtf8Display display;
  auto output = display.append(bytes);
  output += display.finish_frame();
  return output;
}

std::string render_hex(const std::span<const std::byte> bytes) {
  static constexpr std::string_view digits = "0123456789ABCDEF";
  std::string output;
  if (!bytes.empty()) {
    output.reserve(bytes.size() * 3U - 1U);
  }
  for (std::size_t index = 0U; index < bytes.size(); ++index) {
    if (index != 0U) {
      output.push_back(' ');
    }
    const auto value = octet(bytes[index]);
    output.push_back(digits[(value >> 4U) & 0x0FU]);
    output.push_back(digits[value & 0x0FU]);
  }
  return output;
}

std::string render(const std::span<const std::byte> bytes,
                   const DisplayMode mode) {
  switch (mode) {
  case DisplayMode::Text:
    return render_text(bytes);
  case DisplayMode::Hex:
    return render_hex(bytes);
  case DisplayMode::Mixed:
    return render_text(bytes) + " | " + render_hex(bytes);
  }
  return {};
}

} // namespace lazycom::encoding
