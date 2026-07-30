#include <lazycom/diagnostics/diagnostics.hpp>

#if defined(LAZYCOM_DIAGNOSTICS_ENABLED)
#include <spdlog/version.h>
#endif

namespace lazycom::diagnostics {

bool compiled_in() noexcept {
#if defined(LAZYCOM_DIAGNOSTICS_ENABLED)
  return true;
#else
  return false;
#endif
}

int backend_version() noexcept {
#if defined(LAZYCOM_DIAGNOSTICS_ENABLED)
  return SPDLOG_VERSION;
#else
  return 0;
#endif
}

}  // namespace lazycom::diagnostics
