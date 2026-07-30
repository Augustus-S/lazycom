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

class DeviceScanner final {
public:
  [[nodiscard]] static Result<std::unique_ptr<DeviceScanner>>
  create(std::unique_ptr<ISerialBackend> backend,
         UiWakeCallback wake_callback = nullptr, void *wake_context = nullptr);

  ~DeviceScanner();
  DeviceScanner(const DeviceScanner &) = delete;
  DeviceScanner &operator=(const DeviceScanner &) = delete;
  DeviceScanner(DeviceScanner &&) = delete;
  DeviceScanner &operator=(DeviceScanner &&) = delete;

  [[nodiscard]] Result<OperationId> submit_scan(OperationId operation_id,
                                                ScanGeneration generation);
  [[nodiscard]] std::vector<ScanCompletion> drain_completions();
  void acknowledge_ui_wakeup() noexcept;

  void request_stop() noexcept;
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
