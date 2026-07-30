#pragma once

#include <cstddef>
#include <string>
#include <string_view>
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

[[nodiscard]] ParsedTxBytes parse_text(std::string_view text,
                                       std::size_t maximum = kMaxTxBytes);
[[nodiscard]] ParsedTxBytes parse_hex(std::string_view text,
                                      std::size_t maximum = kMaxTxBytes);

} // namespace lazycom::encoding
