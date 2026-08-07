#include <lazycom/diagnostics/diagnostics.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <exception>

#include <unistd.h>

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

void emergency_write() noexcept {
  static constexpr char message[] =
      "LazyCom fatal: unhandled process termination\n";
  std::size_t offset = 0U;
  while (offset < sizeof(message) - 1U) {
    const auto written =
        ::write(STDERR_FILENO, message + offset, sizeof(message) - 1U - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
    } else if (written < 0 && errno == EINTR) {
      continue;
    } else {
      break;
    }
  }
}

void install_terminate_handler() noexcept {
  std::set_terminate([]() noexcept {
    emergency_write();
    std::_Exit(EXIT_FAILURE);
  });
}

} // namespace lazycom::diagnostics
