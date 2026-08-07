#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <tl/expected.hpp>

namespace lazycom::encoding {

inline constexpr std::size_t kMaxTxBytes = 1024U * 1024U;

enum class TxParseErrorCode { InvalidUtf8, InvalidHex, LimitExceeded };

struct TxParseError {
  TxParseErrorCode code{TxParseErrorCode::InvalidHex};
  std::size_t offset{};
  std::string detail;
  bool operator==(const TxParseError &) const = default;
};

using ParsedTxBytes = tl::expected<std::vector<std::byte>, TxParseError>;

namespace detail {

[[nodiscard]] inline int hex_value(const char value) noexcept {
  if (value >= '0' && value <= '9') {
    return value - '0';
  }
  if (value >= 'a' && value <= 'f') {
    return value - 'a' + 10;
  }
  if (value >= 'A' && value <= 'F') {
    return value - 'A' + 10;
  }
  return -1;
}

[[nodiscard]] inline bool hex_separator(const char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

[[nodiscard]] inline TxParseError parse_error(const TxParseErrorCode code,
                                              const std::size_t offset,
                                              std::string detail) {
  return TxParseError{code, offset, std::move(detail)};
}

} // namespace detail

[[nodiscard]] inline bool is_strict_utf8(const std::string_view text) noexcept {
  std::size_t index = 0U;
  while (index < text.size()) {
    const auto first = static_cast<unsigned char>(text[index]);
    if (first <= 0x7FU) {
      ++index;
      continue;
    }

    std::size_t continuation_count = 0U;
    std::uint32_t code_point = 0U;
    std::uint32_t minimum = 0U;
    if ((first & 0xE0U) == 0xC0U) {
      continuation_count = 1U;
      code_point = first & 0x1FU;
      minimum = 0x80U;
    } else if ((first & 0xF0U) == 0xE0U) {
      continuation_count = 2U;
      code_point = first & 0x0FU;
      minimum = 0x800U;
    } else if ((first & 0xF8U) == 0xF0U) {
      continuation_count = 3U;
      code_point = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (continuation_count > text.size() - index - 1U) {
      return false;
    }
    for (std::size_t offset = 1U; offset <= continuation_count; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (continuation & 0x3FU);
    }
    if (code_point < minimum || code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
      return false;
    }
    index += continuation_count + 1U;
  }
  return true;
}

[[nodiscard]] inline ParsedTxBytes
parse_text(const std::string_view text,
           const std::size_t maximum = kMaxTxBytes) {
  if (text.size() > maximum) {
    return tl::unexpected(
        detail::parse_error(TxParseErrorCode::LimitExceeded, maximum,
                            "TXT payload exceeds its byte limit"));
  }
  if (!is_strict_utf8(text)) {
    return tl::unexpected(detail::parse_error(
        TxParseErrorCode::InvalidUtf8, 0U, "TXT payload is not strict UTF-8"));
  }
  const auto bytes = std::span<const std::byte>{
      reinterpret_cast<const std::byte *>(text.data()), text.size()};
  return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

[[nodiscard]] inline ParsedTxBytes
parse_hex(const std::string_view text,
          const std::size_t maximum = kMaxTxBytes) {
  std::vector<std::byte> output;
  output.reserve(std::min(text.size() / 2U, maximum));
  std::size_t offset = 0U;
  while (offset < text.size()) {
    while (offset < text.size() && detail::hex_separator(text[offset])) {
      ++offset;
    }
    if (offset == text.size()) {
      break;
    }
    const auto token_start = offset;
    while (offset < text.size() && !detail::hex_separator(text[offset])) {
      ++offset;
    }
    auto token = text.substr(token_start, offset - token_start);
    if (token.size() == 4U && token[0] == '0' &&
        (token[1] == 'x' || token[1] == 'X')) {
      token.remove_prefix(2U);
    }
    if (token.size() != 2U || detail::hex_value(token[0]) < 0 ||
        detail::hex_value(token[1]) < 0) {
      return tl::unexpected(detail::parse_error(TxParseErrorCode::InvalidHex,
                                                token_start,
                                                "HEX tokens must be one byte"));
    }
    if (output.size() == maximum) {
      return tl::unexpected(
          detail::parse_error(TxParseErrorCode::LimitExceeded, token_start,
                              "HEX payload exceeds its byte limit"));
    }
    const auto value = static_cast<unsigned int>(
        detail::hex_value(token[0]) * 16 + detail::hex_value(token[1]));
    output.push_back(static_cast<std::byte>(value));
  }
  return output;
}

} // namespace lazycom::encoding
