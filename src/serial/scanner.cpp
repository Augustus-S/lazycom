#include <lazycom/serial/scanner.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <new>
#include <stdexcept>
#include <system_error>
#include <thread>
#include <utility>

namespace lazycom::serial {
namespace {

struct ScanRequest {
  OperationId operation_id{};
  ScanGeneration generation{};
};

struct CompletionSlot {
  OperationId operation_id{};
  bool reserved{};
  std::uint64_t completion_order{};
  std::optional<ScanCompletion> completion;
};

[[nodiscard]] Error scan_submission_error(const std::string_view detail,
                                          const OperationId operation_id) {
  return make_error(ErrorCode::ValidationInvalidValue,
                    Operation::EnumerateDevices, detail, {}, std::nullopt,
                    operation_id.value == 0U ? std::nullopt
                                             : std::optional{operation_id});
}

} // namespace

struct DeviceScanner::Impl {
  Impl(std::unique_ptr<ISerialBackend> scanner_backend,
       const UiWakeCallback callback, void *const callback_context)
      : backend(std::move(scanner_backend)), wake_callback(callback),
        wake_context(callback_context) {}

  ~Impl() {
    request_stop();
    if (worker.joinable()) {
      worker.join();
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
    condition.notify_all();
  }

  void notify_ui() noexcept {
    if (!ui_wakeup_pending.exchange(true, std::memory_order_acq_rel) &&
        wake_callback != nullptr) {
      wake_callback(wake_context);
    }
  }

  [[nodiscard]] CompletionSlot *find_slot(const OperationId operation_id) {
    const auto found = std::find_if(
        slots.begin(), slots.end(), [operation_id](const CompletionSlot &slot) {
          return slot.reserved && slot.operation_id == operation_id;
        });
    return found == slots.end() ? nullptr : &*found;
  }

  void thread_main(const std::stop_token token) noexcept {
    app::WorkerExitReason reason = app::WorkerExitReason::Completed;
    try {
      while (!token.stop_requested() &&
             !stop_requested.load(std::memory_order_acquire)) {
        std::optional<ScanRequest> request;
        {
          std::unique_lock lock(mutex);
          condition.wait(lock, [&] {
            return token.stop_requested() ||
                   stop_requested.load(std::memory_order_acquire) ||
                   !requests.empty();
          });
          if (token.stop_requested() ||
              stop_requested.load(std::memory_order_acquire)) {
            break;
          }
          request = requests.front();
          requests.pop_front();
        }

        auto devices = backend->enumerate();
        ScanCompletion completion;
        completion.operation_id = request->operation_id;
        completion.generation = request->generation;
        if (devices) {
          completion.devices = std::move(*devices);
        } else {
          completion.outcome = app::OperationOutcome::Failed;
          completion.error = std::move(devices.error());
        }
        {
          std::lock_guard lock(mutex);
          auto *const slot = find_slot(request->operation_id);
          if (slot == nullptr || slot->completion) {
            throw std::logic_error("scanner completion slot invariant");
          }
          slot->completion.emplace(std::move(completion));
          slot->completion_order = ++last_completion_order;
        }
        notify_ui();
      }
    } catch (const std::bad_alloc &) {
      static_cast<void>(fatal.publish(
          {ErrorCode::InternalOutOfMemory, Operation::EnumerateDevices,
           app::WorkerKind::Scanner, app::FatalReason::OutOfMemory,
           app::SignalSourceLocation::current()}));
      reason = app::WorkerExitReason::Fatal;
    } catch (...) {
      static_cast<void>(fatal.publish(
          {ErrorCode::InternalInvariantBroken, Operation::EnumerateDevices,
           app::WorkerKind::Scanner, app::FatalReason::UnknownException,
           app::SignalSourceLocation::current()}));
      reason = app::WorkerExitReason::Fatal;
    }

    std::deque<ScanRequest> cancelled;
    {
      std::lock_guard lock(mutex);
      cancelled.swap(requests);
      for (const ScanRequest &request : cancelled) {
        auto *const slot = find_slot(request.operation_id);
        if (slot != nullptr && !slot->completion) {
          slot->completion.emplace(
              ScanCompletion{request.operation_id,
                             request.generation,
                             app::OperationOutcome::Cancelled,
                             {},
                             std::nullopt});
          slot->completion_order = ++last_completion_order;
        }
      }
    }
    stopped = {app::WorkerKind::Scanner, app::WorkerLifecycle::AtReturnPoint,
               reason};
    stopped_ready.store(true, std::memory_order_release);
    condition.notify_all();
    notify_ui();
  }

  std::unique_ptr<ISerialBackend> backend;
  mutable std::mutex mutex;
  mutable std::condition_variable condition;
  std::deque<ScanRequest> requests;
  std::array<CompletionSlot, 2> slots{};
  ScanGeneration last_generation{};
  std::uint64_t last_completion_order{};
  std::atomic_bool stop_requested{false};
  std::atomic_bool stopped_ready{false};
  std::atomic_bool ui_wakeup_pending{false};
  app::FatalSignalSlot fatal;
  app::WorkerStoppedSignal stopped{};
  UiWakeCallback wake_callback{};
  void *wake_context{};
  std::jthread worker;
};

DeviceScanner::DeviceScanner(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

Result<std::unique_ptr<DeviceScanner>>
DeviceScanner::create(std::unique_ptr<ISerialBackend> backend,
                      const UiWakeCallback wake_callback,
                      void *const wake_context) {
  if (!backend) {
    return tl::unexpected(scan_submission_error("scanner backend is null", {}));
  }
  try {
    auto scanner =
        std::unique_ptr<DeviceScanner>(new DeviceScanner(std::make_unique<Impl>(
            std::move(backend), wake_callback, wake_context)));
    scanner->impl_->start();
    return scanner;
  } catch (const std::bad_alloc &) {
    return tl::unexpected(make_error(ErrorCode::InternalOutOfMemory,
                                     Operation::EnumerateDevices,
                                     "cannot allocate device scanner"));
  } catch (const std::system_error &error) {
    return tl::unexpected(make_error(
        ErrorCode::InternalInvariantBroken, Operation::EnumerateDevices,
        "cannot start device scanner", error.code()));
  }
}

DeviceScanner::~DeviceScanner() = default;

Result<OperationId>
DeviceScanner::submit_scan(const OperationId operation_id,
                           const ScanGeneration generation) {
  std::lock_guard lock(impl_->mutex);
  if (operation_id.value == 0U || generation.value == 0U ||
      generation <= impl_->last_generation) {
    return tl::unexpected(scan_submission_error(
        "invalid or stale scan identifiers", operation_id));
  }
  if (impl_->stop_requested.load(std::memory_order_acquire)) {
    return tl::unexpected(
        scan_submission_error("device scanner is stopping", operation_id));
  }
  if (impl_->find_slot(operation_id) != nullptr) {
    return tl::unexpected(
        scan_submission_error("duplicate scan operation", operation_id));
  }
  const auto free =
      std::find_if(impl_->slots.begin(), impl_->slots.end(),
                   [](const CompletionSlot &slot) { return !slot.reserved; });
  if (free == impl_->slots.end()) {
    return tl::unexpected(
        scan_submission_error("device scanner is busy", operation_id));
  }
  free->operation_id = operation_id;
  free->reserved = true;
  try {
    impl_->requests.push_back({operation_id, generation});
  } catch (...) {
    *free = CompletionSlot{};
    return tl::unexpected(make_error(
        ErrorCode::InternalOutOfMemory, Operation::EnumerateDevices,
        "cannot enqueue device scan", {}, std::nullopt, operation_id));
  }
  impl_->last_generation = generation;
  impl_->condition.notify_one();
  return operation_id;
}

std::vector<ScanCompletion> DeviceScanner::drain_completions() {
  std::lock_guard lock(impl_->mutex);
  std::vector<ScanCompletion> result;
  std::array<CompletionSlot *, 2> ready{};
  std::size_t ready_count = 0U;
  for (CompletionSlot &slot : impl_->slots) {
    if (slot.completion) {
      ready[ready_count++] = &slot;
    }
  }
  if (ready_count == 2U &&
      ready[1]->completion_order < ready[0]->completion_order) {
    std::swap(ready[0], ready[1]);
  }
  result.reserve(ready_count);
  for (std::size_t index = 0U; index < ready_count; ++index) {
    result.push_back(std::move(*ready[index]->completion));
    *ready[index] = CompletionSlot{};
  }
  return result;
}

void DeviceScanner::acknowledge_ui_wakeup() noexcept {
  impl_->ui_wakeup_pending.store(false, std::memory_order_release);
  bool pending = false;
  {
    std::lock_guard lock(impl_->mutex);
    pending = std::any_of(
        impl_->slots.begin(), impl_->slots.end(),
        [](const CompletionSlot &slot) { return slot.completion.has_value(); });
  }
  if (pending || impl_->fatal.load() ||
      impl_->stopped_ready.load(std::memory_order_acquire)) {
    impl_->notify_ui();
  }
}

void DeviceScanner::request_stop() noexcept { impl_->request_stop(); }

bool DeviceScanner::wait_until_stopped(
    const std::chrono::steady_clock::time_point deadline) const noexcept {
  try {
    std::unique_lock lock(impl_->mutex);
    return impl_->condition.wait_until(lock, deadline, [this] {
      return impl_->stopped_ready.load(std::memory_order_acquire);
    });
  } catch (...) {
    return false;
  }
}

std::optional<app::FatalSignal> DeviceScanner::fatal_signal() const noexcept {
  return impl_->fatal.load();
}

std::optional<app::WorkerStoppedSignal>
DeviceScanner::worker_stopped_signal() const noexcept {
  if (!impl_->stopped_ready.load(std::memory_order_acquire)) {
    return std::nullopt;
  }
  return impl_->stopped;
}

} // namespace lazycom::serial
