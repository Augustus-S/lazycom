#pragma once

#include <lazycom/app/completion.hpp>
#include <lazycom/app/signals.hpp>
#include <lazycom/serial/backend.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace lazycom::serial {

struct SerialServiceOptions {
  std::size_t command_max_messages{256};
  std::size_t command_max_bytes{1024U * 1024U};
  std::size_t tx_max_messages{256};
  std::size_t tx_max_bytes{4U * 1024U * 1024U};
  std::size_t rx_max_chunks{4096};
  std::size_t rx_max_bytes{4U * 1024U * 1024U};
  std::size_t io_budget_bytes{256U * 1024U};
  std::chrono::milliseconds connect_timeout{5000};
  std::chrono::milliseconds tx_timeout{5000};
  std::chrono::milliseconds owner_stop_timeout{5000};

  [[nodiscard]] static SerialServiceOptions
  from_config(const config::ConfigSnapshot &snapshot) noexcept;
};

struct ConnectRequest {
  app::ConnectCommand command;
  DevicePath path;
  PortConfig config;
};

struct TxRequest {
  app::SendCommand command;
  std::vector<std::byte> payload;
};

struct ConnectCompletion {
  OperationId operation_id{};
  ConnectionGeneration generation{};
  std::optional<SessionId> session_id;
  app::OperationOutcome outcome{app::OperationOutcome::Succeeded};
  std::optional<Error> error;
};

struct TxCompletion {
  OperationId operation_id{};
  ConnectionGeneration generation{};
  SessionId session_id{};
  std::size_t accepted_bytes{};
  app::OperationOutcome outcome{app::OperationOutcome::Succeeded};
  std::optional<Error> error;
};

struct DisconnectCompletion {
  OperationId operation_id{};
  ConnectionGeneration generation{};
  std::optional<SessionId> session_id;
  app::OperationOutcome outcome{app::OperationOutcome::Succeeded};
  std::optional<Error> error;
};

struct TaskStopCompletion {
  OperationId operation_id{};
  TaskGeneration generation{};
  app::OperationOutcome outcome{app::OperationOutcome::Succeeded};
};

using SerialCompletion = std::variant<ConnectCompletion, TxCompletion,
                                      DisconnectCompletion, TaskStopCompletion>;

enum class SerialDataKind : std::uint8_t {
  Rx,
  Tx,
  Error,
  Cleanup,
};

struct SerialDataEvent {
  std::uint64_t owner_order{};
  ConnectionGeneration generation{};
  SessionId session_id{};
  app::SessionEventOrigin origin{app::SessionEventOrigin::Normal};
  SerialDataKind kind{SerialDataKind::Rx};
  std::optional<OperationId> operation_id;
  std::vector<std::byte> bytes;
  std::optional<Error> error;
};

struct SerialOverflowSignal {
  ConnectionGeneration generation{};
  SessionId session_id{};
  std::uint64_t dropped_chunks{};
  std::uint64_t dropped_bytes{};
};

struct SerialConnectionSnapshot {
  bool connected{};
  bool connecting{};
  bool disconnecting{};
  std::optional<ConnectionGeneration> generation;
  std::optional<SessionId> session_id;
};

enum class SubmitStatus : std::uint8_t {
  Accepted,
  AlreadyPending,
};

class SerialService final {
public:
  [[nodiscard]] static Result<std::unique_ptr<SerialService>>
  create(std::unique_ptr<ISerialBackend> backend,
         SerialServiceOptions options = {},
         UiWakeCallback wake_callback = nullptr, void *wake_context = nullptr);

  ~SerialService();
  SerialService(const SerialService &) = delete;
  SerialService &operator=(const SerialService &) = delete;
  SerialService(SerialService &&) = delete;
  SerialService &operator=(SerialService &&) = delete;

  [[nodiscard]] Result<OperationId> submit_connect(ConnectRequest request);
  [[nodiscard]] Result<OperationId> submit_tx(TxRequest request);
  [[nodiscard]] Result<SubmitStatus>
  request_cancel_connect(app::CancelConnectCommand command);
  [[nodiscard]] Result<SubmitStatus>
  request_disconnect(app::DisconnectCommand command);
  [[nodiscard]] Result<SubmitStatus>
  request_stop_task(app::StopTaskCommand command);

  void request_stop() noexcept;
  [[nodiscard]] bool request_stop_and_wait() noexcept;
  [[nodiscard]] bool wait_until_stopped(
      std::chrono::steady_clock::time_point deadline) const noexcept;

  [[nodiscard]] std::vector<SerialDataEvent> drain_data(std::size_t max_events);
  [[nodiscard]] std::vector<SerialCompletion> drain_completions();
  void acknowledge_ui_wakeup() noexcept;

  [[nodiscard]] SerialConnectionSnapshot connection_snapshot() const noexcept;
  [[nodiscard]] std::optional<SerialOverflowSignal>
  overflow_signal() const noexcept;
  [[nodiscard]] std::optional<app::FatalSignal> fatal_signal() const noexcept;
  [[nodiscard]] std::optional<app::WorkerStoppedSignal>
  worker_stopped_signal() const noexcept;
  [[nodiscard]] std::uint64_t wait_count() const noexcept;
  [[nodiscard]] std::size_t queued_data_bytes() const noexcept;

private:
  struct Impl;
  explicit SerialService(std::unique_ptr<Impl> impl) noexcept;
  std::unique_ptr<Impl> impl_;
};

} // namespace lazycom::serial
