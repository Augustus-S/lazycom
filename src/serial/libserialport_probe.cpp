#include <lazycom/serial/libserialport_probe.hpp>

#include <libserialport.h>

namespace lazycom::serial {
namespace detail {
void ensure_libserialport_initialized() noexcept;
}

LibserialportVersion libserialport_version() noexcept {
  detail::ensure_libserialport_initialized();
  const auto *text = sp_get_package_version_string();
  return {
      sp_get_major_package_version(),
      sp_get_minor_package_version(),
      sp_get_micro_package_version(),
      text == nullptr ? std::string_view{} : std::string_view{text},
  };
}

bool libserialport_version_supported() noexcept {
  const auto current = libserialport_version();
  return current.major > 0 || (current.major == 0 && current.minor > 1) ||
         (current.major == 0 && current.minor == 1 && current.micro >= 2);
}

} // namespace lazycom::serial
