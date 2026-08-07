#pragma once

namespace lazycom::diagnostics {

[[nodiscard]] bool compiled_in() noexcept;
[[nodiscard]] int backend_version() noexcept;
void emergency_write() noexcept;
void install_terminate_handler() noexcept;

} // namespace lazycom::diagnostics
