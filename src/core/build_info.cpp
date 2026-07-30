#include <lazycom/core/build_info.hpp>

namespace lazycom {

std::string_view version() noexcept {
  return LAZYCOM_VERSION;
}

}  // namespace lazycom
