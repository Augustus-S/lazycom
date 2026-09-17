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

/** @brief Hard queue, I/O-budget, and monotonic-timeout settings for the owner.
 */
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
  config::SendMode input_mode{config::SendMode::Txt};
};

struct ConnectCompletion {
  OperationId operation_id{};
  ConnectionGeneration generation{};
  std::optional<SessionId> session_id;
  app::OperationOutcome outcome{app::OperationOutcome::Succeeded};
  std::optional<Error> error;
  /** @brief Paired owner observations of successful session establishment. */
  std::chrono::steady_clock::time_point observed_at{};
  std::chrono::system_clock::time_point time_utc{};
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
  /** @brief RX read, last positive TX write, or lifecycle observation time. */
  std::chrono::steady_clock::time_point observed_at{};
  std::chrono::system_clock::time_point time_utc{};
  std::optional<config::SendMode> input_mode{};
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

/**
 * @brief Asynchronous single-owner serial service with bounded data and
 * commands.
 *
 * The service owns one backend, eventfd, and persistent owner thread. Only that
 * thread accesses the active port or borrowed native wait handle. Public
 * submission, draining, stop, wait, and snapshot methods are synchronized;
 * callers should designate one consumer for each drain channel.
 *
 * Successful submission means request and terminal-completion capacity were
 * reserved, not that the serial operation succeeded. Completion and ordered
 * data are separate channels and may be drained at different times.
 *
 * @warning Destruction requests stop and joins without an internal deadline. A
 * blocking backend call cannot be interrupted by the service timeout.
 */
class SerialService final {
public:
  /**
   * @brief Validates options, consumes the backend, and starts the owner
   * thread.
   * @param backend Non-null backend exclusively owned by the serial owner.
   * @param options Bounded queues, I/O budget, and monotonic timeouts.
   * @param wake_callback Optional coalesced UI notification callback.
   * @param wake_context Borrowed context valid through worker shutdown.
   */
  [[nodiscard]] static Result<std::unique_ptr<SerialService>>
  create(std::unique_ptr<ISerialBackend> backend,
         SerialServiceOptions options = {},
         UiWakeCallback wake_callback = nullptr, void *wake_context = nullptr);

  ~SerialService();
  SerialService(const SerialService &) = delete;
  SerialService &operator=(const SerialService &) = delete;
  SerialService(SerialService &&) = delete;
  SerialService &operator=(SerialService &&) = delete;

  /**
   * @brief Admits a newer connection generation while the owner is idle.
   * @return The request operation ID after command, completion, and final
   * cleanup capacity are reserved.
   */
  [[nodiscard]] Result<OperationId> submit_connect(ConnectRequest request);
  /**
   * @brief Admits a nonempty, at-most-1-MiB TX for the exact active session.
   *
   * Requests do not interleave bytes. Pending manual TX takes priority over
   * scheduled TX at request boundaries. Terminal data contains the exact
   * accepted prefix, followed by an ERR for partial
   * failure/cancellation/timeout.
   */
  [[nodiscard]] Result<OperationId> submit_tx(TxRequest request);
  /**
   * @brief Requests cancellation through the dedicated connect-control slot.
   * @return Accepted when this operation owns a future completion, or
   * AlreadyPending when an earlier control request remains authoritative.
   */
  [[nodiscard]] Result<SubmitStatus>
  request_cancel_connect(app::CancelConnectCommand command);
  /** @brief Requests ordered close and final Cleanup for the exact session. */
  [[nodiscard]] Result<SubmitStatus>
  request_disconnect(app::DisconnectCommand command);
  /**
   * @brief Cancels matching scheduled TX and establishes a generation barrier.
   * @post After successful terminal completion, no matching write may occur.
   */
  [[nodiscard]] Result<SubmitStatus>
  request_stop_task(app::StopTaskCommand command);

  /**
   * @brief Idempotently closes admission and settles all accepted operations.
   *
   * An active port is closed by the owner, and its final Cleanup data is
   * published before the worker reaches AtReturnPoint.
   */
  void request_stop() noexcept;
  /** @brief Requests stop and waits for the configured owner-stop timeout. */
  [[nodiscard]] bool request_stop_and_wait() noexcept;
  /**
   * @brief Waits for AtReturnPoint until an absolute steady-clock deadline.
   * @note This function neither requests stop nor joins.
   */
  [[nodiscard]] bool wait_until_stopped(
      std::chrono::steady_clock::time_point deadline) const noexcept;

  /**
   * @brief Consumes at most max_events events in global owner_order.
   * @note Cleanup is the final ordered data event for its session.
   */
  [[nodiscard]] std::vector<SerialDataEvent> drain_data(std::size_t max_events);
  /** @brief Consumes all ready terminal completions in publication order. */
  [[nodiscard]] std::vector<SerialCompletion> drain_completions();
  /** @brief Acknowledges and safely rearms the coalesced UI wake notification.
   */
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
