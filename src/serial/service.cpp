#include <lazycom/serial/service.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <stop_token>
#include <thread>
#include <utility>

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace lazycom::serial {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::size_t kMaximumQueueMessages = 65536U;
constexpr std::size_t kMaximumCommandBytes = 2U * 1024U * 1024U;
constexpr std::size_t kMaximumTxBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumRxBytes = 8U * 1024U * 1024U;
constexpr std::size_t kMaximumIoBudgetBytes = 1024U * 1024U;
constexpr auto kMinimumTimeout = std::chrono::milliseconds{100};
constexpr auto kMaximumTimeout = std::chrono::seconds{60};

[[nodiscard]] bool valid(const OperationId value) noexcept {
  return value.value != 0;
}

[[nodiscard]] bool valid(const ConnectionGeneration value) noexcept {
  return value.value != 0;
}

[[nodiscard]] bool valid(const SessionId value) noexcept {
  return value.value != 0;
}

[[nodiscard]] bool valid(const TaskGeneration value) noexcept {
  return value.value != 0;
}

[[nodiscard]] std::size_t mib_bytes(const std::uint32_t value) noexcept {
  return static_cast<std::size_t>(value) * 1024U * 1024U;
}

[[nodiscard]] std::size_t saturated_add(const std::size_t left,
                                        const std::size_t right) noexcept {
  if (right > std::numeric_limits<std::size_t>::max() - left) {
    return std::numeric_limits<std::size_t>::max();
  }
  return left + right;
}

[[nodiscard]] Error submission_error(const std::string_view detail,
                                     const Operation operation,
                                     const OperationId operation_id = {}) {
  return make_error(
      ErrorCode::ValidationInvalidValue, operation, detail, {}, std::nullopt,
      valid(operation_id) ? std::optional{operation_id} : std::nullopt);
}

[[nodiscard]] Error invariant_error(const std::string_view detail,
                                    const Operation operation) {
  return make_error(ErrorCode::InternalInvariantBroken, operation, detail);
}

[[nodiscard]] timespec relative_timeout(const Clock::time_point deadline,
                                        const Clock::time_point now) noexcept {
  if (deadline <= now) {
    return {};
  }
  const auto duration = deadline - now;
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(duration);
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(duration - seconds);
  return {.tv_sec = static_cast<time_t>(seconds.count()),
          .tv_nsec = static_cast<long>(nanoseconds.count())};
}

enum class MailboxKind : std::uint8_t {
  Connect,
  Tx,
  Disconnect,
  TaskStop,
};

struct CompletionSlot {
  OperationId operation_id{};
  MailboxKind kind{MailboxKind::Connect};
  bool reserved{};
  std::uint64_t completion_order{};
  std::optional<SerialCompletion> completion;
};

class CompletionMailbox final {
public:
  CompletionMailbox(const std::size_t normal, const std::size_t tx,
                    const std::size_t control)
      : normal_(normal), tx_(tx), control_(control) {}

  [[nodiscard]] bool reserve(const OperationId operation_id,
                             const app::OperationClass operation_class,
                             const MailboxKind kind) noexcept {
    if (!valid(operation_id) || find(operation_id) != nullptr) {
      return false;
    }
    auto &slots = select(operation_class);
    const auto free =
        std::find_if(slots.begin(), slots.end(),
                     [](const CompletionSlot &slot) { return !slot.reserved; });
    if (free == slots.end()) {
      return false;
    }
    free->operation_id = operation_id;
    free->kind = kind;
    free->reserved = true;
    free->completion_order = 0;
    free->completion.reset();
    return true;
  }

  void cancel(const OperationId operation_id) noexcept {
    if (auto *const slot = find(operation_id);
        slot != nullptr && !slot->completion.has_value()) {
      *slot = CompletionSlot{};
    }
  }

  [[nodiscard]] bool complete(SerialCompletion completion) noexcept {
    const OperationId id = std::visit(
        [](const auto &value) noexcept { return value.operation_id; },
        completion);
    auto *const slot = find(id);
    if (slot == nullptr || slot->completion.has_value() ||
        slot->kind != kind_of(completion)) {
      return false;
    }
    slot->completion.emplace(std::move(completion));
    slot->completion_order = ++last_completion_order_;
    return true;
  }

  [[nodiscard]] std::vector<SerialCompletion> drain() {
    struct Ready {
      std::uint64_t order;
      CompletionSlot *slot;
    };
    std::vector<Ready> ready;
    ready.reserve(completed_count());
    collect_ready(normal_, ready);
    collect_ready(tx_, ready);
    collect_ready(control_, ready);
    std::sort(ready.begin(), ready.end(),
              [](const Ready &left, const Ready &right) {
                return left.order < right.order;
              });
    std::vector<SerialCompletion> result;
    result.reserve(ready.size());
    for (const Ready &entry : ready) {
      result.push_back(std::move(*entry.slot->completion));
      *entry.slot = CompletionSlot{};
    }
    return result;
  }

  [[nodiscard]] bool has_completed() const noexcept {
    return completed_count() != 0;
  }

private:
  [[nodiscard]] static MailboxKind
  kind_of(const SerialCompletion &completion) noexcept {
    return std::visit(
        [](const auto &value) noexcept {
          using Value = std::remove_cvref_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, ConnectCompletion>) {
            return MailboxKind::Connect;
          } else if constexpr (std::is_same_v<Value, TxCompletion>) {
            return MailboxKind::Tx;
          } else if constexpr (std::is_same_v<Value, DisconnectCompletion>) {
            return MailboxKind::Disconnect;
          } else {
            return MailboxKind::TaskStop;
          }
        },
        completion);
  }

  [[nodiscard]] std::vector<CompletionSlot> &
  select(const app::OperationClass operation_class) noexcept {
    if (operation_class == app::OperationClass::Tx) {
      return tx_;
    }
    if (operation_class == app::OperationClass::Control) {
      return control_;
    }
    return normal_;
  }

  [[nodiscard]] CompletionSlot *find(const OperationId operation_id) noexcept {
    const auto in =
        [operation_id](std::vector<CompletionSlot> &slots) -> CompletionSlot * {
      const auto found = std::find_if(
          slots.begin(), slots.end(),
          [operation_id](const CompletionSlot &slot) {
            return slot.reserved && slot.operation_id == operation_id;
          });
      return found == slots.end() ? nullptr : &*found;
    };
    if (auto *const slot = in(normal_); slot != nullptr) {
      return slot;
    }
    if (auto *const slot = in(tx_); slot != nullptr) {
      return slot;
    }
    return in(control_);
  }

  [[nodiscard]] std::size_t completed_count() const noexcept {
    const auto count = [](const std::vector<CompletionSlot> &slots) {
      return static_cast<std::size_t>(std::count_if(
          slots.begin(), slots.end(), [](const CompletionSlot &slot) {
            return slot.completion.has_value();
          }));
    };
    return count(normal_) + count(tx_) + count(control_);
  }

  template <class Ready>
  static void collect_ready(std::vector<CompletionSlot> &slots,
                            std::vector<Ready> &ready) {
    for (CompletionSlot &slot : slots) {
      if (slot.completion) {
        ready.push_back(Ready{slot.completion_order, &slot});
      }
    }
  }

  std::vector<CompletionSlot> normal_;
  std::vector<CompletionSlot> tx_;
  std::vector<CompletionSlot> control_;
  std::uint64_t last_completion_order_{};
};

struct QueuedConnect {
  ConnectRequest request;
  Clock::time_point accepted_at;
  std::size_t charged_bytes{};
};

struct QueuedTx {
  TxRequest request;
  Clock::time_point accepted_at;
  std::size_t offset{};
  std::size_t charged_bytes{};
};

} // namespace

SerialServiceOptions SerialServiceOptions::from_config(
    const config::ConfigSnapshot &snapshot) noexcept {
  return {
      snapshot.queues.owner_command_max_messages,
      mib_bytes(snapshot.queues.owner_command_max_mib),
      snapshot.queues.tx_max_messages,
      mib_bytes(snapshot.queues.tx_max_mib),
      snapshot.queues.rx_ingress_max_blocks,
      mib_bytes(snapshot.queues.rx_ingress_max_mib),
      256U * 1024U,
      std::chrono::milliseconds{snapshot.timeouts.connect_ms},
      std::chrono::milliseconds{snapshot.timeouts.tx_ms},
      std::chrono::milliseconds{snapshot.timeouts.owner_stop_ms},
  };
}

struct SerialService::Impl {
  Impl(std::unique_ptr<ISerialBackend> serial_backend,
       const SerialServiceOptions service_options, const int event_fd,
       const UiWakeCallback callback, void *const callback_context)
      : backend(std::move(serial_backend)), options(service_options),
        wake_fd(event_fd),
        mailbox(options.command_max_messages, options.tx_max_messages, 3U),
        wake_callback(callback), wake_context(callback_context) {
    cleanup_events.resize(options.command_max_messages + 2U);
  }

  ~Impl() {
    request_stop();
    if (worker.joinable()) {
      worker.join();
    }
    if (wake_fd >= 0) {
      static_cast<void>(::close(wake_fd));
    }
  }

  void start() {
    worker = std::jthread(
        [this](const std::stop_token token) { thread_main(token); });
  }

  void request_stop() noexcept {
    stop_requested.store(true, std::memory_order_release);
    if (worker.joinable()) {
      worker.request_stop();
    }
    wake_owner();
  }

  void wake_owner() const noexcept {
    const std::uint64_t value = 1U;
    if (::write(wake_fd, &value, sizeof(value)) < 0 && errno != EAGAIN) {
      // The owner also observes stop_token; a broken eventfd is reported by
      // ppoll.
    }
  }

  void drain_wake_fd() const noexcept {
    std::uint64_t value = 0;
    while (::read(wake_fd, &value, sizeof(value)) ==
           static_cast<ssize_t>(sizeof(value))) {
    }
  }

  void notify_ui() noexcept {
    if (!ui_wakeup_pending.exchange(true, std::memory_order_acq_rel) &&
        wake_callback != nullptr) {
      wake_callback(wake_context);
    }
  }

  [[nodiscard]] bool complete(SerialCompletion value) noexcept {
    bool accepted = false;
    {
      std::lock_guard lock(mutex);
      accepted = mailbox.complete(std::move(value));
    }
    if (!accepted) {
      publish_fatal(app::FatalReason::InvariantBroken,
                    Operation::CoordinateFatal);
      return false;
    }
    notify_ui();
    return true;
  }

  void publish_fatal(const app::FatalReason reason,
                     const Operation operation) noexcept {
    const auto code = reason == app::FatalReason::OutOfMemory
                          ? ErrorCode::InternalOutOfMemory
                          : ErrorCode::InternalInvariantBroken;
    static_cast<void>(
        fatal.publish({code, operation, app::WorkerKind::Serial, reason,
                       app::SignalSourceLocation::current()}));
    notify_ui();
  }

  void publish_stopped(const app::WorkerExitReason reason) noexcept {
    stopped_signal = {app::WorkerKind::Serial,
                      app::WorkerLifecycle::AtReturnPoint, reason};
    stopped_ready.store(true, std::memory_order_release);
    stopped_cv.notify_all();
    notify_ui();
  }

  void thread_main(const std::stop_token token) noexcept {
    app::WorkerExitReason exit_reason = app::WorkerExitReason::Completed;
    try {
      const std::stop_callback stop_wakeup(token, [this] {
        stop_requested.store(true, std::memory_order_release);
        wake_owner();
      });
      owner_loop();
    } catch (const std::bad_alloc &) {
      emergency_close();
      publish_fatal(app::FatalReason::OutOfMemory, Operation::CoordinateFatal);
      exit_reason = app::WorkerExitReason::Fatal;
    } catch (...) {
      emergency_close();
      publish_fatal(app::FatalReason::UnknownException,
                    Operation::CoordinateFatal);
      exit_reason = app::WorkerExitReason::Fatal;
    }
    publish_stopped(exit_reason);
  }

  void emergency_close() noexcept {
    if (port_open) {
      try {
        static_cast<void>(backend->close());
      } catch (...) {
      }
      port_open = false;
      serial_fd = -1;
    }
  }

  void owner_loop() {
    while (!stop_requested.load(std::memory_order_acquire)) {
      process_controls();
      if (stop_requested.load(std::memory_order_acquire)) {
        break;
      }
      process_connect();
      process_controls();
      if (stop_requested.load(std::memory_order_acquire)) {
        break;
      }
      activate_tx();
      process_tx_deadline();

      std::array<pollfd, 2> wait_set{};
      nfds_t wait_size = 1U;
      wait_set[0] = {.fd = wake_fd, .events = POLLIN, .revents = 0};
      if (port_open) {
        short events = POLLIN;
        if (active_tx) {
          events = static_cast<short>(events | POLLOUT);
        }
        wait_set[1] = {.fd = serial_fd, .events = events, .revents = 0};
        wait_size = 2U;
      }

      const auto deadline = nearest_deadline();
      const auto now = Clock::now();
      std::optional<timespec> timeout;
      if (deadline) {
        timeout = relative_timeout(*deadline, now);
      }
      wait_calls.fetch_add(1U, std::memory_order_relaxed);
      const int ready = ::ppoll(wait_set.data(), wait_size,
                                timeout ? &*timeout : nullptr, nullptr);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        handle_fault(
            make_error(ErrorCode::InternalInvariantBroken,
                       Operation::ReadSerial, "serial owner ppoll failed",
                       std::error_code{errno, std::generic_category()}));
        continue;
      }
      if ((wait_set[0].revents & POLLIN) != 0) {
        drain_wake_fd();
      }
      if (!port_open || ready == 0) {
        continue;
      }

      const short serial_events = wait_set[1].revents;
      if ((serial_events & POLLIN) != 0) {
        read_ready();
      }
      if (port_open && (serial_events & POLLOUT) != 0) {
        write_ready();
      }
      if (port_open && (serial_events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        const auto detail = (serial_events & POLLNVAL) != 0
                                ? "serial wait handle became invalid"
                                : "serial device disconnected";
        handle_fault(make_error((serial_events & POLLNVAL) != 0
                                    ? ErrorCode::InternalInvariantBroken
                                    : ErrorCode::SerialDeviceGone,
                                Operation::ReadSerial, detail));
      } else if (port_open && serial_events != 0 &&
                 (serial_events & (POLLIN | POLLOUT)) == 0) {
        handle_fault(invariant_error("unknown serial poll event",
                                     Operation::ReadSerial));
      }
    }
    stop_cleanup();
  }

  [[nodiscard]] std::optional<Clock::time_point> nearest_deadline() const {
    if (active_tx) {
      return active_tx->accepted_at + options.tx_timeout;
    }
    return std::nullopt;
  }

  void process_connect() {
    std::optional<QueuedConnect> command;
    {
      std::lock_guard lock(mutex);
      if (connect_queue.empty() || port_open || connected) {
        return;
      }
      command.emplace(std::move(connect_queue.front()));
      connect_queue.pop_front();
    }

    const auto &request = command->request;
    if (Clock::now() - command->accepted_at >= options.connect_timeout) {
      finish_connect_failure(*command, app::OperationOutcome::TimedOut,
                             std::nullopt);
      return;
    }
    auto opened = backend->open(request.path, request.config);
    if (!opened) {
      finish_connect_failure(*command, app::OperationOutcome::Failed,
                             std::move(opened.error()));
      return;
    }
    port_open = true;
    const auto wait_handle = backend->native_wait_handle();
    if (!wait_handle) {
      Error error = wait_handle.error();
      static_cast<void>(backend->close());
      port_open = false;
      finish_connect_failure(*command, app::OperationOutcome::Failed,
                             std::move(error));
      return;
    }
    serial_fd = *wait_handle;

    std::optional<app::CancelConnectCommand> cancellation;
    {
      std::lock_guard lock(mutex);
      if (cancel_connect &&
          cancel_connect->generation == request.command.generation) {
        cancellation = std::exchange(cancel_connect, std::nullopt);
      }
    }
    if (cancellation || stop_requested.load(std::memory_order_acquire) ||
        Clock::now() - command->accepted_at >= options.connect_timeout) {
      const auto outcome =
          cancellation || stop_requested.load(std::memory_order_acquire)
              ? app::OperationOutcome::Cancelled
              : app::OperationOutcome::TimedOut;
      static_cast<void>(backend->close());
      port_open = false;
      serial_fd = -1;
      finish_connect_failure(*command, outcome, std::nullopt);
      if (cancellation) {
        finish_cancel(*cancellation, app::OperationOutcome::Succeeded);
      }
      return;
    }

    SessionId session;
    if (session_ids.issue(session) != IdIncrementResult::Advanced) {
      static_cast<void>(backend->close());
      port_open = false;
      serial_fd = -1;
      publish_fatal(app::FatalReason::InvariantBroken,
                    Operation::CoordinateFatal);
      finish_connect_failure(*command, app::OperationOutcome::Failed,
                             invariant_error("session identifier exhausted",
                                             Operation::OpenSerial));
      return;
    }
    {
      std::lock_guard lock(mutex);
      connected = true;
      connecting = false;
      disconnecting = false;
      generation = request.command.generation;
      session_id = session;
      release_normal_charge(*command);
    }
    static_cast<void>(complete(ConnectCompletion{
        request.command.operation_id, request.command.generation, session,
        app::OperationOutcome::Succeeded, std::nullopt}));
  }

  void finish_connect_failure(QueuedConnect &command,
                              const app::OperationOutcome outcome,
                              std::optional<Error> error) {
    {
      std::lock_guard lock(mutex);
      connecting = false;
      disconnecting = false;
      connected = false;
      generation.reset();
      session_id.reset();
      release_normal_charge(command);
    }
    static_cast<void>(
        complete(ConnectCompletion{command.request.command.operation_id,
                                   command.request.command.generation,
                                   std::nullopt, outcome, std::move(error)}));
  }

  void release_normal_charge(const QueuedConnect &command) noexcept {
    --command_messages;
    command_bytes -= command.charged_bytes;
  }

  void process_controls() {
    std::optional<app::CancelConnectCommand> cancellation;
    std::optional<app::DisconnectCommand> disconnection;
    std::optional<app::StopTaskCommand> task_stop;
    std::optional<QueuedConnect> cancelled_queued_connect;
    {
      std::lock_guard lock(mutex);
      if (cancel_connect) {
        const auto found =
            std::find_if(connect_queue.begin(), connect_queue.end(),
                         [&](const QueuedConnect &value) {
                           return value.request.command.generation ==
                                  cancel_connect->generation;
                         });
        if (found != connect_queue.end()) {
          cancelled_queued_connect.emplace(std::move(*found));
          connect_queue.erase(found);
          cancellation = std::exchange(cancel_connect, std::nullopt);
          connecting = false;
          disconnecting = false;
          generation.reset();
          release_normal_charge(*cancelled_queued_connect);
        } else if (!connecting) {
          cancellation = std::exchange(cancel_connect, std::nullopt);
        }
      }
      if (disconnect) {
        disconnection = std::exchange(disconnect, std::nullopt);
      }
      if (stop_task) {
        task_stop = std::exchange(stop_task, std::nullopt);
      }
    }
    if (cancelled_queued_connect) {
      static_cast<void>(complete(ConnectCompletion{
          cancelled_queued_connect->request.command.operation_id,
          cancelled_queued_connect->request.command.generation, std::nullopt,
          app::OperationOutcome::Cancelled, std::nullopt}));
    }
    if (cancellation) {
      finish_cancel(*cancellation, app::OperationOutcome::Succeeded);
    }
    if (task_stop) {
      stop_task_barrier(*task_stop);
    }
    if (disconnection) {
      disconnect_barrier(*disconnection);
    }
  }

  void stop_task_barrier(const app::StopTaskCommand &command) {
    std::deque<QueuedTx> cancelled;
    if (active_tx &&
        active_tx->request.command.task_generation == command.generation) {
      cancelled.push_back(std::move(*active_tx));
      active_tx.reset();
    }
    {
      std::lock_guard lock(mutex);
      for (auto iterator = tx_queue.begin(); iterator != tx_queue.end();) {
        if (iterator->request.command.task_generation == command.generation) {
          cancelled.push_back(std::move(*iterator));
          iterator = tx_queue.erase(iterator);
        } else {
          ++iterator;
        }
      }
    }
    for (auto &request : cancelled) {
      finish_tx(std::move(request), app::OperationOutcome::Cancelled,
                std::nullopt);
    }
    static_cast<void>(
        complete(TaskStopCompletion{command.operation_id, command.generation,
                                    app::OperationOutcome::Succeeded}));
  }

  void finish_cancel(const app::CancelConnectCommand &command,
                     const app::OperationOutcome outcome) {
    static_cast<void>(
        complete(DisconnectCompletion{command.operation_id, command.generation,
                                      std::nullopt, outcome, std::nullopt}));
  }

  void activate_tx() {
    if (active_tx || !port_open) {
      return;
    }
    std::optional<QueuedTx> expired;
    {
      std::lock_guard lock(mutex);
      while (!tx_queue.empty()) {
        if (Clock::now() - tx_queue.front().accepted_at < options.tx_timeout) {
          active_tx.emplace(std::move(tx_queue.front()));
          tx_queue.pop_front();
          break;
        }
        expired.emplace(std::move(tx_queue.front()));
        tx_queue.pop_front();
        break;
      }
    }
    if (expired) {
      finish_tx(std::move(*expired), app::OperationOutcome::TimedOut,
                std::nullopt);
    }
  }

  void process_tx_deadline() {
    if (active_tx &&
        Clock::now() - active_tx->accepted_at >= options.tx_timeout) {
      auto timed_out = std::move(*active_tx);
      active_tx.reset();
      finish_tx(std::move(timed_out), app::OperationOutcome::TimedOut,
                std::nullopt);
    }
  }

  [[nodiscard]] bool task_stop_requested(
      const std::optional<TaskGeneration> generation_to_check) const {
    if (!generation_to_check) {
      return false;
    }
    std::lock_guard lock(mutex);
    return stop_task && stop_task->generation == *generation_to_check;
  }

  void write_ready() {
    std::size_t budget = options.io_budget_bytes;
    while (active_tx && budget != 0U) {
      auto &request = *active_tx;
      if (task_stop_requested(request.request.command.task_generation)) {
        return;
      }
      const auto remaining =
          std::span<const std::byte>{request.request.payload}.subspan(
              request.offset);
      const auto attempt = remaining.first(std::min(remaining.size(), budget));
      auto written = backend->write_some(attempt);
      if (!written) {
        Error error = std::move(written.error());
        auto failed = std::move(request);
        active_tx.reset();
        finish_tx(std::move(failed), app::OperationOutcome::Failed,
                  std::move(error));
        handle_fault(make_error(ErrorCode::SerialDeviceGone,
                                Operation::WriteSerial,
                                "serial write fault closed the session"));
        return;
      }
      if (*written == 0U) {
        return;
      }
      if (*written > attempt.size()) {
        handle_fault(invariant_error("backend over-reported serial write",
                                     Operation::WriteSerial));
        return;
      }
      request.offset += *written;
      budget -= *written;
      if (request.offset == request.request.payload.size()) {
        auto completed = std::move(request);
        active_tx.reset();
        finish_tx(std::move(completed), app::OperationOutcome::Succeeded,
                  std::nullopt);
        activate_tx();
      }
    }
  }

  void finish_tx(QueuedTx request, const app::OperationOutcome outcome,
                 std::optional<Error> error) {
    bool data_accepted = true;
    if (request.offset != 0U) {
      SerialDataEvent event;
      event.generation = request.request.command.generation;
      event.session_id = request.request.command.session_id;
      event.kind = SerialDataKind::Tx;
      event.operation_id = request.request.command.operation_id;
      event.bytes.assign(request.request.payload.begin(),
                         request.request.payload.begin() +
                             static_cast<std::ptrdiff_t>(request.offset));
      data_accepted = enqueue_data(std::move(event));
    }
    {
      std::lock_guard lock(mutex);
      --tx_messages;
      tx_bytes -= request.charged_bytes;
    }
    static_cast<void>(complete(TxCompletion{
        request.request.command.operation_id,
        request.request.command.generation, request.request.command.session_id,
        request.offset, outcome, std::move(error)}));
    if (!data_accepted && port_open && !fault_in_progress) {
      handle_fault(make_error(ErrorCode::SerialDeviceGone,
                              Operation::WriteSerial,
                              "serial data event capacity exceeded"));
    }
  }

  void read_ready() {
    std::size_t budget = options.io_budget_bytes;
    while (port_open && budget != 0U) {
      const std::size_t request_size =
          std::min<std::size_t>(64U * 1024U, budget);
      std::array<std::byte, 64U * 1024U> scratch;
      auto amount =
          backend->read_some(std::span<std::byte>{scratch}.first(request_size));
      if (!amount) {
        handle_fault(std::move(amount.error()));
        return;
      }
      if (*amount == 0U) {
        return;
      }
      if (*amount > request_size) {
        handle_fault(invariant_error("backend over-reported serial read",
                                     Operation::ReadSerial));
        return;
      }
      budget -= *amount;
      SerialDataEvent event;
      {
        std::lock_guard lock(mutex);
        if (!generation || !session_id) {
          handle_fault_locked(
              invariant_error("serial bytes arrived without an active session",
                              Operation::ReadSerial));
          stop_requested.store(true, std::memory_order_release);
          return;
        }
        event.generation = *generation;
        event.session_id = *session_id;
      }
      event.kind = SerialDataKind::Rx;
      event.bytes.assign(scratch.begin(),
                         scratch.begin() +
                             static_cast<std::ptrdiff_t>(*amount));
      if (!enqueue_data(std::move(event))) {
        handle_fault(make_error(ErrorCode::SerialDeviceGone,
                                Operation::ReadSerial,
                                "RX ingress capacity exceeded"));
        return;
      }
    }
  }

  [[nodiscard]] bool enqueue_data(SerialDataEvent event) {
    bool accepted = false;
    {
      std::lock_guard lock(mutex);
      const std::size_t event_bytes =
          saturated_add(sizeof(SerialDataEvent), event.bytes.capacity());
      if (data_queue.size() < options.rx_max_chunks &&
          event_bytes <= options.rx_max_bytes -
                             std::min(data_bytes, options.rx_max_bytes)) {
        if (next_data_order == std::numeric_limits<std::uint64_t>::max()) {
          static_cast<void>(fatal.publish(
              {ErrorCode::InternalInvariantBroken, Operation::CoordinateFatal,
               app::WorkerKind::Serial, app::FatalReason::InvariantBroken,
               app::SignalSourceLocation::current()}));
          stop_requested.store(true, std::memory_order_release);
          return false;
        }
        event.owner_order = ++next_data_order;
        data_bytes += event_bytes;
        data_queue.push_back(std::move(event));
        accepted = true;
      } else {
        ++overflow.dropped_chunks;
        overflow.dropped_bytes += event.bytes.size();
        overflow.generation = event.generation;
        overflow.session_id = event.session_id;
        overflow_ready = true;
      }
    }
    notify_ui();
    return accepted;
  }

  void handle_fault_locked(Error error) {
    // Called only to publish the fixed fatal slot while mutex is held.
    static_cast<void>(
        fatal.publish({error.code, error.operation, app::WorkerKind::Serial,
                       app::FatalReason::InvariantBroken,
                       app::SignalSourceLocation::current()}));
  }

  void handle_fault(Error error) {
    if (!port_open) {
      return;
    }
    fault_in_progress = true;
    const auto fault_generation = generation;
    const auto fault_session = session_id;
    cancel_all_tx(app::OperationOutcome::Failed, error);
    const Status closed = backend->close();
    port_open = false;
    serial_fd = -1;
    Error cleanup_error = std::move(error);
    if (!closed) {
      cleanup_error = closed.error();
    }
    if (fault_generation && fault_session) {
      publish_cleanup(*fault_generation, *fault_session,
                      std::move(cleanup_error));
    }
    {
      std::lock_guard lock(mutex);
      connected = false;
      connecting = false;
      disconnecting = false;
      generation.reset();
      session_id.reset();
    }
    fault_in_progress = false;
  }

  void cancel_all_tx(const app::OperationOutcome outcome,
                     const std::optional<Error> &error = std::nullopt) {
    std::deque<QueuedTx> cancelled;
    if (active_tx) {
      cancelled.push_back(std::move(*active_tx));
      active_tx.reset();
    }
    {
      std::lock_guard lock(mutex);
      cancelled.insert(cancelled.end(),
                       std::make_move_iterator(tx_queue.begin()),
                       std::make_move_iterator(tx_queue.end()));
      tx_queue.clear();
    }
    for (QueuedTx &request : cancelled) {
      finish_tx(std::move(request), outcome, error);
    }
  }

  void disconnect_barrier(const app::DisconnectCommand &command) {
    const auto old_generation = generation;
    const auto old_session = session_id;
    cancel_all_tx(app::OperationOutcome::Cancelled);
    Status close_status;
    if (port_open) {
      close_status = backend->close();
      port_open = false;
      serial_fd = -1;
    }
    std::optional<Error> error;
    if (!close_status) {
      error = close_status.error();
    }
    if (old_generation && old_session) {
      publish_cleanup(*old_generation, *old_session, error);
    }
    {
      std::lock_guard lock(mutex);
      connected = false;
      connecting = false;
      disconnecting = false;
      generation.reset();
      session_id.reset();
    }
    static_cast<void>(complete(DisconnectCompletion{
        command.operation_id, command.generation, command.session_id,
        error ? app::OperationOutcome::Failed
              : app::OperationOutcome::Succeeded,
        std::move(error)}));
  }

  void publish_cleanup(const ConnectionGeneration event_generation,
                       const SessionId event_session,
                       std::optional<Error> error) {
    SerialDataEvent event;
    event.generation = event_generation;
    event.session_id = event_session;
    event.origin = app::SessionEventOrigin::Cleanup;
    event.kind = SerialDataKind::Cleanup;
    event.error = std::move(error);
    bool stored = false;
    {
      std::lock_guard lock(mutex);
      if (next_data_order == std::numeric_limits<std::uint64_t>::max()) {
        static_cast<void>(fatal.publish(
            {ErrorCode::InternalInvariantBroken, Operation::CoordinateFatal,
             app::WorkerKind::Serial, app::FatalReason::InvariantBroken,
             app::SignalSourceLocation::current()}));
        stop_requested.store(true, std::memory_order_release);
        return;
      }
      event.owner_order = ++next_data_order;
      const auto free = std::find_if(
          cleanup_events.begin(), cleanup_events.end(),
          [](const std::optional<SerialDataEvent> &slot) { return !slot; });
      if (free == cleanup_events.end()) {
        static_cast<void>(fatal.publish(
            {ErrorCode::InternalInvariantBroken, Operation::CoordinateFatal,
             app::WorkerKind::Serial, app::FatalReason::InvariantBroken,
             app::SignalSourceLocation::current()}));
      } else {
        free->emplace(std::move(event));
        stored = true;
      }
    }
    notify_ui();
    if (!stored) {
      stop_requested.store(true, std::memory_order_release);
    }
  }

  void stop_cleanup() {
    std::optional<QueuedConnect> pending_connect;
    std::optional<app::CancelConnectCommand> pending_cancel;
    std::optional<app::DisconnectCommand> pending_disconnect;
    std::optional<app::StopTaskCommand> pending_task_stop;
    {
      std::lock_guard lock(mutex);
      if (!connect_queue.empty()) {
        pending_connect.emplace(std::move(connect_queue.front()));
        connect_queue.pop_front();
        release_normal_charge(*pending_connect);
      }
      pending_cancel = std::exchange(cancel_connect, std::nullopt);
      pending_disconnect = std::exchange(disconnect, std::nullopt);
      pending_task_stop = std::exchange(stop_task, std::nullopt);
    }
    if (pending_connect) {
      static_cast<void>(complete(ConnectCompletion{
          pending_connect->request.command.operation_id,
          pending_connect->request.command.generation, std::nullopt,
          app::OperationOutcome::Cancelled, std::nullopt}));
    }
    cancel_all_tx(app::OperationOutcome::Cancelled);
    if (port_open) {
      const auto old_generation = generation;
      const auto old_session = session_id;
      const auto closed = backend->close();
      port_open = false;
      serial_fd = -1;
      if (old_generation && old_session) {
        publish_cleanup(*old_generation, *old_session,
                        closed ? std::nullopt
                               : std::optional<Error>{closed.error()});
      }
    }
    if (pending_cancel) {
      finish_cancel(*pending_cancel, app::OperationOutcome::Cancelled);
    }
    if (pending_disconnect) {
      static_cast<void>(complete(DisconnectCompletion{
          pending_disconnect->operation_id, pending_disconnect->generation,
          pending_disconnect->session_id, app::OperationOutcome::Cancelled,
          std::nullopt}));
    }
    if (pending_task_stop) {
      static_cast<void>(complete(TaskStopCompletion{
          pending_task_stop->operation_id, pending_task_stop->generation,
          app::OperationOutcome::Cancelled}));
    }
    std::lock_guard lock(mutex);
    connected = false;
    connecting = false;
    disconnecting = false;
    generation.reset();
    session_id.reset();
  }

  std::unique_ptr<ISerialBackend> backend;
  SerialServiceOptions options;
  int wake_fd{-1};
  mutable std::mutex mutex;
  std::deque<QueuedConnect> connect_queue;
  std::deque<QueuedTx> tx_queue;
  std::optional<QueuedTx> active_tx;
  std::optional<app::CancelConnectCommand> cancel_connect;
  std::optional<app::DisconnectCommand> disconnect;
  std::optional<app::StopTaskCommand> stop_task;
  std::deque<SerialDataEvent> data_queue;
  std::vector<std::optional<SerialDataEvent>> cleanup_events;
  CompletionMailbox mailbox;
  std::size_t command_messages{};
  std::size_t command_bytes{};
  std::size_t tx_messages{};
  std::size_t tx_bytes{};
  std::size_t data_bytes{};
  std::uint64_t next_data_order{};
  bool connected{};
  bool connecting{};
  bool disconnecting{};
  bool port_open{};
  bool fault_in_progress{};
  int serial_fd{-1};
  std::optional<ConnectionGeneration> generation;
  ConnectionGeneration last_generation{};
  std::optional<SessionId> session_id;
  IdSequence<SessionId> session_ids;
  SerialOverflowSignal overflow{};
  bool overflow_ready{};
  app::FatalSignalSlot fatal;
  app::WorkerStoppedSignal stopped_signal{};
  std::atomic_bool stopped_ready{false};
  mutable std::condition_variable stopped_cv;
  std::atomic_bool stop_requested{false};
  std::atomic_bool ui_wakeup_pending{false};
  std::atomic<std::uint64_t> wait_calls{0U};
  UiWakeCallback wake_callback{};
  void *wake_context{};
  std::jthread worker;
};

SerialService::SerialService(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

Result<std::unique_ptr<SerialService>> SerialService::create(
    std::unique_ptr<ISerialBackend> backend, const SerialServiceOptions options,
    const UiWakeCallback wake_callback, void *const wake_context) {
  if (!backend || options.command_max_messages == 0U ||
      options.command_max_bytes == 0U || options.tx_max_messages == 0U ||
      options.tx_max_bytes == 0U || options.rx_max_chunks == 0U ||
      options.rx_max_bytes == 0U || options.io_budget_bytes == 0U ||
      options.command_max_messages > kMaximumQueueMessages ||
      options.tx_max_messages > kMaximumQueueMessages ||
      options.rx_max_chunks > kMaximumQueueMessages ||
      options.command_max_bytes > kMaximumCommandBytes ||
      options.tx_max_bytes > kMaximumTxBytes ||
      options.rx_max_bytes > kMaximumRxBytes ||
      options.io_budget_bytes > kMaximumIoBudgetBytes ||
      options.connect_timeout < kMinimumTimeout ||
      options.connect_timeout > kMaximumTimeout ||
      options.tx_timeout < kMinimumTimeout ||
      options.tx_timeout > kMaximumTimeout ||
      options.owner_stop_timeout < kMinimumTimeout ||
      options.owner_stop_timeout > kMaximumTimeout) {
    return tl::unexpected(submission_error("invalid serial service options",
                                           Operation::ValidateConfig));
  }
  const int wake_fd = ::eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
  if (wake_fd < 0) {
    const int error = errno;
    return tl::unexpected(make_error(
        ErrorCode::InternalInvariantBroken, Operation::CoordinateFatal,
        "cannot create serial owner eventfd",
        std::error_code{error, std::generic_category()}));
  }
  bool caller_owns_wake_fd = true;
  try {
    auto impl = std::make_unique<Impl>(std::move(backend), options, wake_fd,
                                       wake_callback, wake_context);
    caller_owns_wake_fd = false;
    auto service =
        std::unique_ptr<SerialService>(new SerialService(std::move(impl)));
    service->impl_->start();
    return service;
  } catch (const std::bad_alloc &) {
    if (caller_owns_wake_fd) {
      static_cast<void>(::close(wake_fd));
    }
    return tl::unexpected(make_error(ErrorCode::InternalOutOfMemory,
                                     Operation::CoordinateFatal,
                                     "cannot allocate serial service"));
  } catch (const std::system_error &error) {
    if (caller_owns_wake_fd) {
      static_cast<void>(::close(wake_fd));
    }
    return tl::unexpected(make_error(
        ErrorCode::InternalInvariantBroken, Operation::CoordinateFatal,
        "cannot start serial owner", error.code()));
  }
}

SerialService::~SerialService() = default;

Result<OperationId> SerialService::submit_connect(ConnectRequest request) {
  const auto operation_id = request.command.operation_id;
  const std::size_t bytes = saturated_add(
      saturated_add(sizeof(QueuedConnect), request.path.requested.capacity()),
      request.path.canonical.capacity());
  std::lock_guard lock(impl_->mutex);
  if (!valid(operation_id) || !valid(request.command.generation)) {
    return tl::unexpected(submission_error(
        "invalid connect identifiers", Operation::OpenSerial, operation_id));
  }
  if (request.command.generation <= impl_->last_generation ||
      request.path.requested.empty() || request.path.requested.front() != '/' ||
      request.path.canonical.empty() || request.path.canonical.front() != '/' ||
      request.path.requested.find('\0') != std::string::npos ||
      request.path.canonical.find('\0') != std::string::npos) {
    return tl::unexpected(
        submission_error("stale generation or invalid absolute serial path",
                         Operation::OpenSerial, operation_id));
  }
  if (impl_->command_messages >= impl_->options.command_max_messages ||
      bytes > impl_->options.command_max_bytes -
                  std::min(impl_->command_bytes,
                           impl_->options.command_max_bytes)) {
    return tl::unexpected(submission_error(
        "serial command queue is full", Operation::OpenSerial, operation_id));
  }
  if (impl_->stop_requested.load(std::memory_order_acquire) ||
      impl_->connected || impl_->connecting || impl_->disconnecting) {
    return tl::unexpected(submission_error(
        "serial connection is not idle", Operation::OpenSerial, operation_id));
  }
  if (!impl_->mailbox.reserve(operation_id, app::OperationClass::Normal,
                              MailboxKind::Connect)) {
    return tl::unexpected(submission_error(
        "connect completion capacity is full or operation is duplicate",
        Operation::OpenSerial, operation_id));
  }
  try {
    impl_->connect_queue.push_back(
        QueuedConnect{std::move(request), Clock::now(), bytes});
  } catch (...) {
    impl_->mailbox.cancel(operation_id);
    return tl::unexpected(make_error(
        ErrorCode::InternalOutOfMemory, Operation::OpenSerial,
        "cannot enqueue connect operation", {}, std::nullopt, operation_id));
  }
  ++impl_->command_messages;
  impl_->command_bytes += bytes;
  impl_->connecting = true;
  impl_->last_generation =
      impl_->connect_queue.back().request.command.generation;
  impl_->generation = impl_->connect_queue.back().request.command.generation;
  impl_->wake_owner();
  return operation_id;
}

Result<OperationId> SerialService::submit_tx(TxRequest request) {
  const auto operation_id = request.command.operation_id;
  const std::size_t payload_size = request.payload.size();
  const std::size_t bytes =
      saturated_add(sizeof(QueuedTx), request.payload.capacity());
  std::lock_guard lock(impl_->mutex);
  if (!valid(operation_id) || !valid(request.command.generation) ||
      !valid(request.command.session_id) || payload_size == 0U ||
      payload_size > config::kMaximumPayloadBytes) {
    return tl::unexpected(submission_error(
        "invalid serial TX request", Operation::WriteSerial, operation_id));
  }
  if (!impl_->connected || impl_->disconnecting ||
      impl_->generation != request.command.generation ||
      impl_->session_id != request.command.session_id) {
    return tl::unexpected(submission_error("stale or disconnected TX session",
                                           Operation::WriteSerial,
                                           operation_id));
  }
  if (impl_->tx_messages >= impl_->options.tx_max_messages ||
      bytes > impl_->options.tx_max_bytes -
                  std::min(impl_->tx_bytes, impl_->options.tx_max_bytes)) {
    return tl::unexpected(submission_error(
        "serial TX queue is full", Operation::WriteSerial, operation_id));
  }
  if (!impl_->mailbox.reserve(operation_id, app::OperationClass::Tx,
                              MailboxKind::Tx)) {
    return tl::unexpected(submission_error(
        "TX completion capacity is full or operation is duplicate",
        Operation::WriteSerial, operation_id));
  }
  try {
    impl_->tx_queue.push_back(
        QueuedTx{std::move(request), Clock::now(), 0U, bytes});
  } catch (...) {
    impl_->mailbox.cancel(operation_id);
    return tl::unexpected(make_error(
        ErrorCode::InternalOutOfMemory, Operation::WriteSerial,
        "cannot enqueue TX operation", {}, std::nullopt, operation_id));
  }
  ++impl_->tx_messages;
  impl_->tx_bytes += bytes;
  impl_->wake_owner();
  return operation_id;
}

Result<SubmitStatus>
SerialService::request_cancel_connect(const app::CancelConnectCommand command) {
  std::lock_guard lock(impl_->mutex);
  if (!valid(command.operation_id) || !valid(command.generation)) {
    return tl::unexpected(submission_error("invalid cancel-connect identifiers",
                                           Operation::CloseSerial,
                                           command.operation_id));
  }
  if (impl_->cancel_connect) {
    return SubmitStatus::AlreadyPending;
  }
  if (!impl_->connecting || impl_->generation != command.generation) {
    return tl::unexpected(submission_error("no matching connection to cancel",
                                           Operation::CloseSerial,
                                           command.operation_id));
  }
  if (!impl_->mailbox.reserve(command.operation_id,
                              app::OperationClass::Control,
                              MailboxKind::Disconnect)) {
    return tl::unexpected(
        submission_error("cancel completion capacity is unavailable",
                         Operation::CloseSerial, command.operation_id));
  }
  impl_->cancel_connect = command;
  impl_->disconnecting = true;
  impl_->wake_owner();
  return SubmitStatus::Accepted;
}

Result<SubmitStatus>
SerialService::request_disconnect(const app::DisconnectCommand command) {
  std::lock_guard lock(impl_->mutex);
  if (!valid(command.operation_id) || !valid(command.generation) ||
      !command.session_id || !valid(*command.session_id)) {
    return tl::unexpected(submission_error("invalid disconnect identifiers",
                                           Operation::CloseSerial,
                                           command.operation_id));
  }
  if (impl_->disconnect) {
    return SubmitStatus::AlreadyPending;
  }
  if (!impl_->connected || impl_->generation != command.generation ||
      impl_->session_id != command.session_id) {
    return tl::unexpected(submission_error("no matching session to disconnect",
                                           Operation::CloseSerial,
                                           command.operation_id));
  }
  if (!impl_->mailbox.reserve(command.operation_id,
                              app::OperationClass::Control,
                              MailboxKind::Disconnect)) {
    return tl::unexpected(
        submission_error("disconnect completion capacity is unavailable",
                         Operation::CloseSerial, command.operation_id));
  }
  impl_->disconnect = command;
  impl_->disconnecting = true;
  impl_->wake_owner();
  return SubmitStatus::Accepted;
}

Result<SubmitStatus>
SerialService::request_stop_task(const app::StopTaskCommand command) {
  std::lock_guard lock(impl_->mutex);
  if (!valid(command.operation_id) || !valid(command.generation)) {
    return tl::unexpected(submission_error("invalid task-stop identifiers",
                                           Operation::WriteSerial,
                                           command.operation_id));
  }
  if (impl_->stop_task) {
    return SubmitStatus::AlreadyPending;
  }
  if (!impl_->mailbox.reserve(command.operation_id,
                              app::OperationClass::Control,
                              MailboxKind::TaskStop)) {
    return tl::unexpected(
        submission_error("task-stop completion capacity is unavailable",
                         Operation::WriteSerial, command.operation_id));
  }
  impl_->stop_task = command;
  impl_->wake_owner();
  return SubmitStatus::Accepted;
}

void SerialService::request_stop() noexcept { impl_->request_stop(); }

bool SerialService::request_stop_and_wait() noexcept {
  request_stop();
  return wait_until_stopped(Clock::now() + impl_->options.owner_stop_timeout);
}

bool SerialService::wait_until_stopped(
    const Clock::time_point deadline) const noexcept {
  if (impl_->stopped_ready.load(std::memory_order_acquire)) {
    return true;
  }
  try {
    std::unique_lock lock(impl_->mutex);
    return impl_->stopped_cv.wait_until(lock, deadline, [this] {
      return impl_->stopped_ready.load(std::memory_order_acquire);
    });
  } catch (...) {
    return false;
  }
}

std::vector<SerialDataEvent>
SerialService::drain_data(const std::size_t max_events) {
  std::vector<SerialDataEvent> result;
  if (max_events == 0U) {
    return result;
  }
  std::lock_guard lock(impl_->mutex);
  result.reserve(std::min(max_events, impl_->data_queue.size() +
                                          impl_->cleanup_events.size()));
  while (result.size() < max_events) {
    auto cleanup = impl_->cleanup_events.end();
    for (auto candidate = impl_->cleanup_events.begin();
         candidate != impl_->cleanup_events.end(); ++candidate) {
      if (*candidate && (cleanup == impl_->cleanup_events.end() ||
                         (*candidate)->owner_order < (*cleanup)->owner_order)) {
        cleanup = candidate;
      }
    }
    const bool take_data =
        !impl_->data_queue.empty() &&
        (cleanup == impl_->cleanup_events.end() ||
         impl_->data_queue.front().owner_order < (*cleanup)->owner_order);
    if (take_data) {
      impl_->data_bytes -= saturated_add(
          sizeof(SerialDataEvent), impl_->data_queue.front().bytes.capacity());
      result.push_back(std::move(impl_->data_queue.front()));
      impl_->data_queue.pop_front();
    } else if (cleanup != impl_->cleanup_events.end()) {
      result.push_back(std::move(**cleanup));
      cleanup->reset();
    } else {
      break;
    }
  }
  return result;
}

std::vector<SerialCompletion> SerialService::drain_completions() {
  std::lock_guard lock(impl_->mutex);
  return impl_->mailbox.drain();
}

void SerialService::acknowledge_ui_wakeup() noexcept {
  impl_->ui_wakeup_pending.store(false, std::memory_order_release);
  bool pending = false;
  {
    std::lock_guard lock(impl_->mutex);
    pending =
        !impl_->data_queue.empty() || impl_->mailbox.has_completed() ||
        std::any_of(impl_->cleanup_events.begin(), impl_->cleanup_events.end(),
                    [](const auto &slot) { return slot.has_value(); });
  }
  pending = pending || impl_->fatal.load().has_value() ||
            impl_->stopped_ready.load(std::memory_order_acquire);
  if (pending) {
    impl_->notify_ui();
  }
}

SerialConnectionSnapshot SerialService::connection_snapshot() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return {impl_->connected, impl_->connecting, impl_->disconnecting,
          impl_->generation, impl_->session_id};
}

std::optional<SerialOverflowSignal>
SerialService::overflow_signal() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->overflow_ready ? std::optional{impl_->overflow} : std::nullopt;
}

std::optional<app::FatalSignal> SerialService::fatal_signal() const noexcept {
  return impl_->fatal.load();
}

std::optional<app::WorkerStoppedSignal>
SerialService::worker_stopped_signal() const noexcept {
  if (!impl_->stopped_ready.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  return impl_->stopped_signal;
}

std::uint64_t SerialService::wait_count() const noexcept {
  return impl_->wait_calls.load(std::memory_order_relaxed);
}

std::size_t SerialService::queued_data_bytes() const noexcept {
  std::lock_guard lock(impl_->mutex);
  return impl_->data_bytes;
}

} // namespace lazycom::serial
