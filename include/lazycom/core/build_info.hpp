#pragma once

#include <string_view>

namespace lazycom {

/**
 * @brief Returns the project version embedded by the build system.
 * @return A view into process-lifetime static storage.
 * @note This is the application version, not a configuration or log schema
 * version.
 */
[[nodiscard]] std::string_view version() noexcept;

} // namespace lazycom
