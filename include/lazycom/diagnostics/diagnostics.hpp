#pragma once

namespace lazycom::diagnostics {

/**
 * @brief Reports whether the spdlog dependency probe was compiled in.
 * @note This does not report runtime diagnostics enablement, sink health, or
 * file creation. The planned diagnostics service is not represented here.
 */
[[nodiscard]] bool compiled_in() noexcept;
/** @return Compile-time spdlog version, or zero when compiled out. */
[[nodiscard]] int backend_version() noexcept;
/**
 * @brief Best-effort direct write of one fixed emergency line to standard
 * error.
 *
 * The function does not allocate, format caller data, enqueue, lock, or create
 * files. Write failure and a partial write are intentionally ignored.
 */
void emergency_write() noexcept;
/**
 * @brief Installs the process-wide minimal terminate handler.
 *
 * On termination the handler invokes emergency_write() and exits immediately;
 * it does not unwind, flush workers, run destructors, or preserve the previous
 * handler. Install once before worker startup.
 */
void install_terminate_handler() noexcept;

} // namespace lazycom::diagnostics
