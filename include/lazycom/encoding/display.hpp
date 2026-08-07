#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace lazycom::encoding {

enum class DisplayMode { Text, Hex, Mixed };

/**
 * @brief Incrementally projects arbitrary bytes to terminal-safe UTF-8.
 *
 * Complete safe UTF-8 is preserved. Invalid bytes and C0/C1 controls are made
 * visible as byte escapes, bidirectional formatting controls are escaped, and
 * literal backslashes are doubled. A potentially valid incomplete UTF-8 suffix
 * is retained between append() calls.
 *
 * Instances are mutable and not safe for concurrent use.
 */
class SafeUtf8Display {
public:
  /**
   * @brief Projects complete text available after appending a byte chunk.
   * @return Only newly completed projected text; an incomplete UTF-8 suffix may
   * remain buffered.
   */
  [[nodiscard]] std::string append(std::span<const std::byte> bytes);
  /**
   * @brief Escapes any incomplete suffix and resets frame-local state.
   * @return Projected text for the pending suffix, or an empty string.
   */
  [[nodiscard]] std::string finish_frame();
  [[nodiscard]] bool has_incomplete_sequence() const noexcept {
    return !pending_.empty();
  }

private:
  std::vector<std::byte> pending_;
};

/** @brief Performs a one-shot terminal-safe text projection. */
[[nodiscard]] std::string render_text(std::span<const std::byte> bytes);
/** @brief Renders uppercase two-digit bytes separated by ASCII spaces. */
[[nodiscard]] std::string render_hex(std::span<const std::byte> bytes);
/**
 * @brief Renders bytes in text, hexadecimal, or combined form.
 * @note Mixed output has the exact form `<safe text> | <hex>`.
 */
[[nodiscard]] std::string render(std::span<const std::byte> bytes,
                                 DisplayMode mode);

} // namespace lazycom::encoding
