#pragma once

#include <lazycom/app/signals.hpp>
#include <lazycom/app/state.hpp>
#include <lazycom/serial/backend.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace lazycom::serial {

struct ScanCompletion {
  OperationId operation_id{};
  ScanGeneration generation{};
  app::OperationOutcome outcome{app::OperationOutcome::Succeeded};
  std::vector<DeviceInfo> devices;
  std::optional<Error> error;
};

/**
 * @brief Persistent asynchronous device scanner with bounded outstanding work.
 *
 * The scanner owns one backend and one worker. At most one scan is active and
 * one is pending; a third request is rejected synchronously. Public methods are
 * synchronized, but callers should designate a single completion consumer.
 * Every admitted request receives one terminal completion during normal, error,
 * or stop settlement.
 *
 * @warning Destruction requests stop and joins without an internal deadline.
 * Owners requiring bounded shutdown must first enforce wait_until_stopped().
 */
class DeviceScanner final {
public:
  /**
   * @brief Consumes a backend and starts the scanner worker.
   * @param backend Non-null backend exclusively owned by the scanner.
   * @param wake_callback Optional coalesced UI notification callback.
   * @param wake_context Borrowed context valid through worker shutdown.
   * @return An owning scanner, or an Error for invalid dependencies/setup.
   */
  [[nodiscard]] static Result<std::unique_ptr<DeviceScanner>>
  create(std::unique_ptr<ISerialBackend> backend,
         UiWakeCallback wake_callback = nullptr, void *wake_context = nullptr);

  ~DeviceScanner();
  DeviceScanner(const DeviceScanner &) = delete;
  DeviceScanner &operator=(const DeviceScanner &) = delete;
  DeviceScanner(DeviceScanner &&) = delete;
  DeviceScanner &operator=(DeviceScanner &&) = delete;

  /**
   * @brief Attempts to admit one newer scan generation.
   * @return operation_id when request and completion capacity are reserved.
   * Success does not imply enumeration success.
   */
  [[nodiscard]] Result<OperationId> submit_scan(OperationId operation_id,
                                                ScanGeneration generation);
  /** @brief Consumes all ready completions in publication order. */
  [[nodiscard]] std::vector<ScanCompletion> drain_completions();
  /**
   * @brief Clears the coalesced wake bit and re-notifies if state remains
   * ready.
   */
  void acknowledge_ui_wakeup() noexcept;

  /** @brief Idempotently closes admission, cancels outstanding scans, and
   * wakes. */
  void request_stop() noexcept;
  /**
   * @brief Waits for AtReturnPoint until an absolute steady-clock deadline.
   * @note This neither requests stop nor joins.
   */
  [[nodiscard]] bool wait_until_stopped(
      std::chrono::steady_clock::time_point deadline) const noexcept;
  [[nodiscard]] std::optional<app::FatalSignal> fatal_signal() const noexcept;
  [[nodiscard]] std::optional<app::WorkerStoppedSignal>
  worker_stopped_signal() const noexcept;

private:
  struct Impl;
  explicit DeviceScanner(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace lazycom::serial
