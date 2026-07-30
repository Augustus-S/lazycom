#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

namespace lazycom::encoding {

enum class DisplayMode { Text, Hex, Mixed };

class SafeUtf8Display {
public:
  [[nodiscard]] std::string append(std::span<const std::byte> bytes);
  [[nodiscard]] std::string finish_frame();
  [[nodiscard]] bool has_incomplete_sequence() const noexcept {
    return !pending_.empty();
  }

private:
  std::vector<std::byte> pending_;
};

[[nodiscard]] std::string render_text(std::span<const std::byte> bytes);
[[nodiscard]] std::string render_hex(std::span<const std::byte> bytes);
[[nodiscard]] std::string render(std::span<const std::byte> bytes,
                                 DisplayMode mode);

} // namespace lazycom::encoding
