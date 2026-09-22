#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace lazycom {

inline constexpr std::size_t kMaxSafeMessageBytes = 4096U;

/**
 * @brief Validates structural UTF-8 without terminal-safety filtering.
 * @note Valid controls, NUL, ESC, and bidi formatting code points remain valid
 * UTF-8 and require a separate safe-display projection.
 */
[[nodiscard]] bool is_strict_utf8(std::span<const std::byte> bytes) noexcept;
/**
 * @brief Validates `YYYY-MM-DDTHH:MM:SS[.fraction]Z` UTC timestamps.
 * @note Fractions contain one to nine digits; offsets and leap seconds are not
 * accepted.
 */
[[nodiscard]] bool is_valid_utc(std::string_view value) noexcept;

/**
 * @brief Produces bounded terminal-safe UTF-8 from untrusted message text.
 *
 * Invalid bytes, controls, DEL, and bidi formatting characters are escaped.
 * Output is truncated only at complete UTF-8 or escape boundaries.
 *
 * @warning This is a display-safety projection, not secret redaction and not a
 * reversible payload codec.
 */
[[nodiscard]] std::string
sanitize_message(std::string_view message,
                 std::size_t maximum_bytes = kMaxSafeMessageBytes);

[[nodiscard]] bool is_strict_utf8(std::string_view value) noexcept;

} // namespace lazycom
