#pragma once

#include <string_view>

namespace lazycom::serial {

struct LibserialportVersion {
  int major;
  int minor;
  int micro;
  std::string_view text;
};

[[nodiscard]] LibserialportVersion libserialport_version() noexcept;
[[nodiscard]] bool libserialport_version_supported() noexcept;

} // namespace lazycom::serial
