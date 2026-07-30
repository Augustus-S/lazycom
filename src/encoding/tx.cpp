#include <lazycom/encoding/tx.hpp>

#include <lazycom/logging/schema.hpp>

#include <algorithm>
#include <limits>
#include <span>

namespace lazycom::encoding {
namespace {

[[nodiscard]] int hex_value(const char value) noexcept {
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

[[nodiscard]] bool separator(const char value) noexcept {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

[[nodiscard]] TxParseError error(const TxParseErrorCode code,
                                 const std::size_t offset,
                                 std::string detail) {
  return TxParseError{code, offset, std::move(detail)};
}

} // namespace

ParsedTxBytes parse_text(const std::string_view text,
                         const std::size_t maximum) {
  if (text.size() > maximum) {
    return tl::unexpected(error(TxParseErrorCode::LimitExceeded, maximum,
                                "TXT payload exceeds its byte limit"));
  }
  const auto bytes = std::span<const std::byte>{
      reinterpret_cast<const std::byte *>(text.data()), text.size()};
  if (!logging::is_strict_utf8(bytes)) {
    return tl::unexpected(error(TxParseErrorCode::InvalidUtf8, 0U,
                                "TXT payload is not strict UTF-8"));
  }
  return std::vector<std::byte>{bytes.begin(), bytes.end()};
}

ParsedTxBytes parse_hex(const std::string_view text,
                        const std::size_t maximum) {
  std::vector<std::byte> output;
  output.reserve(std::min(text.size() / 2U, maximum));
  std::size_t offset = 0U;
  while (offset < text.size()) {
    while (offset < text.size() && separator(text[offset])) {
      ++offset;
    }
    if (offset == text.size()) {
      break;
    }
    const auto token_start = offset;
    while (offset < text.size() && !separator(text[offset])) {
      ++offset;
    }
    auto token = text.substr(token_start, offset - token_start);
    if (token.size() == 4U && token[0] == '0' &&
        (token[1] == 'x' || token[1] == 'X')) {
      token.remove_prefix(2U);
    }
    if (token.size() != 2U || hex_value(token[0]) < 0 ||
        hex_value(token[1]) < 0) {
      return tl::unexpected(error(TxParseErrorCode::InvalidHex, token_start,
                                  "HEX tokens must be one byte"));
    }
    if (output.size() == maximum) {
      return tl::unexpected(error(TxParseErrorCode::LimitExceeded, token_start,
                                  "HEX payload exceeds its byte limit"));
    }
    const auto value = static_cast<unsigned int>(hex_value(token[0]) * 16 +
                                                  hex_value(token[1]));
    output.push_back(static_cast<std::byte>(value));
  }
  return output;
}

} // namespace lazycom::encoding
