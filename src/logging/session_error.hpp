#pragma once

#include <lazycom/base/error.hpp>

namespace lazycom::logging {

[[nodiscard]] inline Error
log_error(std::string_view detail, int error_number = 0,
          ErrorCode code = ErrorCode::LoggingDiskFull) {
  return make_error(
      code, Operation::WriteSessionLog, detail,
      error_number == 0
          ? std::error_code{}
          : std::error_code{error_number, std::generic_category()});
}

} // namespace lazycom::logging
