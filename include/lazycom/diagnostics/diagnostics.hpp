#pragma once

namespace lazycom::diagnostics {

[[nodiscard]] bool compiled_in() noexcept;
[[nodiscard]] int backend_version() noexcept;

}  // namespace lazycom::diagnostics
