#pragma once
#include <cstdlib>
#include <lazycom/base/error.hpp>
#include <lazycom/base/text.hpp>
#include <lazycom/diagnostics/diagnostics.hpp>

namespace lazycom::app {
[[noreturn]] inline void abort_after_shutdown_timeout() noexcept {
  diagnostics::emergency_write();
  std::abort();
}

[[nodiscard]] inline std::string error_text(const Error &error) {
  const auto &descriptor = error_descriptor(error.code);
  std::string result{descriptor.identifier};
  result += ": ";
  result += descriptor.default_message;
  if (!error.detail.empty()) {
    result += " (";
    result += lazycom::sanitize_message(error.detail);
    result += ")";
  }
  return result;
}

} // namespace lazycom::app
