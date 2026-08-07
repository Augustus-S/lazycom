#pragma once

#include <string_view>

namespace lazycom::serial {

struct LibserialportVersion {
  int major;
  int minor;
  int micro;
  std::string_view text;
};

/**
 * @brief Returns the linked libserialport package version.
 * @return Numeric components and a view into library-owned static storage.
 */
[[nodiscard]] LibserialportVersion libserialport_version() noexcept;
/** @return true exactly when the package version is at least 0.1.2. */
[[nodiscard]] bool libserialport_version_supported() noexcept;

} // namespace lazycom::serial
