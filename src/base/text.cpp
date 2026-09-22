#include <lazycom/base/text.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace lazycom {
namespace {

[[nodiscard]] unsigned int octet(std::byte value) noexcept {
  return std::to_integer<unsigned int>(value);
}

[[nodiscard]] std::span<const std::byte>
as_bytes(std::string_view value) noexcept {
  return {reinterpret_cast<const std::byte *>(value.data()), value.size()};
}

[[nodiscard]] bool is_leap_year(unsigned int year) noexcept {
  return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
}

[[nodiscard]] unsigned int parse_digits(std::string_view value,
                                        std::size_t offset,
                                        std::size_t count) noexcept {
  unsigned int result = 0;
  for (std::size_t index = offset; index < offset + count; ++index) {
    if (value[index] < '0' || value[index] > '9') {
      return std::numeric_limits<unsigned int>::max();
    }
    result = result * 10U + static_cast<unsigned int>(value[index] - '0');
  }
  return result;
}

[[nodiscard]] bool append_visible(std::string &output, std::string_view value,
                                  std::size_t maximum_bytes) {
  if (value.size() > maximum_bytes - output.size()) {
    return false;
  }
  output.append(value);
  return true;
}

[[nodiscard]] std::size_t utf8_sequence_length(unsigned int first) noexcept {
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

[[nodiscard]] std::uint32_t decode_code_point(std::string_view value,
                                              std::size_t length) noexcept {
  const auto first = static_cast<unsigned char>(value[0]);
  if (length == 1U) {
    return first;
  }
  std::uint32_t result = first & (0x7FU >> length);
  for (std::size_t index = 1U; index < length; ++index) {
    result =
        (result << 6U) | (static_cast<unsigned char>(value[index]) & 0x3FU);
  }
  return result;
}

[[nodiscard]] bool unsafe_code_point(std::uint32_t value) noexcept {
  return value <= 0x1FU || (value >= 0x7FU && value <= 0x9FU) ||
         value == 0x061CU || value == 0x200EU || value == 0x200FU ||
         (value >= 0x202AU && value <= 0x202EU) ||
         (value >= 0x2066U && value <= 0x2069U);
}

[[nodiscard]] std::string hex_escape(unsigned int value) {
  static constexpr std::string_view digits = "0123456789ABCDEF";
  std::string escaped{"\\x00"};
  escaped[2] = digits[(value >> 4U) & 0x0FU];
  escaped[3] = digits[value & 0x0FU];
  return escaped;
}

[[nodiscard]] std::string unicode_escape(std::uint32_t value) {
  static constexpr std::string_view digits = "0123456789ABCDEF";
  std::string escaped{"\\u{0000}"};
  for (std::size_t index = 0; index < 4U; ++index) {
    escaped[6U - index] = digits[value & 0x0FU];
    value >>= 4U;
  }
  return escaped;
}

} // namespace

bool is_strict_utf8(std::span<const std::byte> bytes) noexcept {
  std::size_t index = 0;
  while (index < bytes.size()) {
    const auto first = octet(bytes[index]);
    if (first <= 0x7FU) {
      ++index;
      continue;
    }
    const auto length = utf8_sequence_length(first);
    if (length == 0U || length > bytes.size() - index) {
      return false;
    }
    for (std::size_t continuation = 1U; continuation < length; ++continuation) {
      const auto value = octet(bytes[index + continuation]);
      if (value < 0x80U || value > 0xBFU) {
        return false;
      }
    }
    const auto second = octet(bytes[index + 1U]);
    if ((first == 0xE0U && second < 0xA0U) ||
        (first == 0xEDU && second > 0x9FU) ||
        (first == 0xF0U && second < 0x90U) ||
        (first == 0xF4U && second > 0x8FU)) {
      return false;
    }
    index += length;
  }
  return true;
}

bool is_valid_utc(std::string_view value) noexcept {
  if (value.size() < 20U || value.size() > 30U || value[4] != '-' ||
      value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
      value[16] != ':' || value.back() != 'Z') {
    return false;
  }
  if (value.size() != 20U) {
    if (value.size() < 22U || value[19] != '.') {
      return false;
    }
    for (std::size_t index = 20U; index + 1U < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') {
        return false;
      }
    }
  }
  const auto year = parse_digits(value, 0U, 4U);
  const auto month = parse_digits(value, 5U, 2U);
  const auto day = parse_digits(value, 8U, 2U);
  const auto hour = parse_digits(value, 11U, 2U);
  const auto minute = parse_digits(value, 14U, 2U);
  const auto second = parse_digits(value, 17U, 2U);
  if (year == 0U || month == 0U || month > 12U || hour > 23U || minute > 59U ||
      second > 59U) {
    return false;
  }
  static constexpr std::array<unsigned int, 12> days_per_month{
      31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
  auto days = days_per_month[month - 1U];
  if (month == 2U && is_leap_year(year)) {
    ++days;
  }
  return day > 0U && day <= days;
}

std::string sanitize_message(std::string_view message,
                             const std::size_t maximum_bytes) {
  std::string output;
  output.reserve(std::min(message.size(), maximum_bytes));
  std::size_t index = 0;
  while (index < message.size() && output.size() < maximum_bytes) {
    const auto first = static_cast<unsigned char>(message[index]);
    const auto length = utf8_sequence_length(first);
    if (length == 0U || length > message.size() - index ||
        !is_strict_utf8(as_bytes(message.substr(index, length)))) {
      if (!append_visible(output, hex_escape(first), maximum_bytes)) {
        break;
      }
      ++index;
      continue;
    }
    const auto point = decode_code_point(message.substr(index, length), length);
    if (unsafe_code_point(point)) {
      const auto escaped =
          point <= 0xFFU ? hex_escape(point) : unicode_escape(point);
      if (!append_visible(output, escaped, maximum_bytes)) {
        break;
      }
    } else if (!append_visible(output, message.substr(index, length),
                               maximum_bytes)) {
      break;
    }
    index += length;
  }
  return output;
}

bool is_strict_utf8(const std::string_view value) noexcept {
  return is_strict_utf8(as_bytes(value));
}

} // namespace lazycom
