#pragma once

#include <lazycom/base/text.hpp>

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

/** Maximum decoded payload accepted by the default TX parsers. */
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

/**
 * @brief Converts strict UTF-8 text to its exact byte representation.
 * @param text Text to validate and copy.
 * @param maximum Maximum encoded byte count.
 * @return Owned bytes, or a parse error. No newline or display escaping is
 * applied.
 */
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

/**
 * @brief Parses whitespace-separated one-byte hexadecimal tokens.
 * @param text Tokens in NN or 0xNN form, separated by space, tab, CR, or LF.
 * @param maximum Maximum decoded byte count.
 * @return The complete decoded payload, or an error at the source token offset.
 * No partial output is returned on failure.
 */
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
