#include <lazycom/logging/session_writer.hpp>

#include "session_error.hpp"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <variant>

namespace lazycom::logging {
namespace {

constexpr std::size_t kMaxQueuedControlCommands = 256U;
constexpr std::size_t kMaxPendingBarrierPromises = 256U;
constexpr std::size_t kMaxDisableWaiters = 256U;

template <class T> [[nodiscard]] std::future<T> ready_future(T value) {
  std::promise<T> promise;
  auto future = promise.get_future();
  promise.set_value(std::move(value));
  return future;
}

[[nodiscard]] BarrierResult failed_barrier(std::uint64_t target,
                                           const BarrierTracker &tracker) {
  return {
      BarrierState::WriterFailed,      target,
      tracker.processed_through_seq(), tracker.flush_attempted_through_seq(),
      tracker.flushed_through_seq(),   false};
}

} // namespace

struct SessionWriter::Impl {
  struct RecordItem {
    model::SessionRecordPtr record;
    std::size_t bytes{};
    model::BudgetReservation reservation;
  };
  struct StartItem {
    Header header;
    std::promise<SessionCommandResult> promise;
  };
  struct CloseItem {
    SessionLogState final_state{SessionLogState::Waiting};
    std::vector<std::promise<SessionCommandResult>> promises;
  };
  struct BarrierItem {
    std::uint64_t target{};
    std::promise<BarrierResult> promise;
  };
  struct ShutdownItem {
    std::promise<SessionCommandResult> promise;
  };
  using Item =
      std::variant<RecordItem, StartItem, CloseItem, BarrierItem, ShutdownItem>;

  struct PendingBarrier {
    std::uint64_t target{};
    std::vector<std::promise<BarrierResult>> promises;
  };

  Impl(SessionWriterOptions writer_options,
       std::unique_ptr<SessionLogFileSystem> writer_file_system,
       model::GlobalMemoryBudget budget)
      : memory_budget{std::move(budget)}, options{std::move(writer_options)},
        file_system{std::move(writer_file_system)} {
    if (!file_system || options.queue_max_records == 0U ||
        options.queue_max_bytes == 0U || options.flush_batch_bytes == 0U ||
        options.flush_interval <= std::chrono::milliseconds::zero() ||
        options.quotas.max_files == 0U ||
        options.quotas.max_total_bytes == 0U ||
        options.quotas.max_file_bytes == 0U ||
        options.quotas.max_file_bytes > options.quotas.max_total_bytes) {
      throw std::invalid_argument{"invalid session writer options"};
    }
    worker = std::jthread{[this] {
      run();
      stopped_ready.store(true, std::memory_order_release);
      condition.notify_all();
    }};
  }

  [[nodiscard]] SessionCommandResult command_result_locked() const {
    return {state.load(std::memory_order_acquire),
            processed.load(std::memory_order_acquire), terminal_error};
  }

  [[nodiscard]] SessionCommandResult command_result() const {
    std::scoped_lock lock{mutex};
    return command_result_locked();
  }

  template <class T>
  static void fulfill(std::promise<T> &promise, T value) noexcept {
    try {
      promise.set_value(std::move(value));
    } catch (...) {
    }
  }

  [[nodiscard]] SessionCommandResult failed_command_result() const noexcept {
    return {SessionLogState::Error, processed.load(std::memory_order_acquire),
            std::nullopt};
  }

  void set_error(Error error) {
    {
      std::scoped_lock lock{mutex};
      if (!terminal_error) {
        terminal_error = std::move(error);
      }
      state.store(SessionLogState::Error, std::memory_order_release);
    }
    condition.notify_all();
  }

  void complete_failed_barriers() {
    for (auto &barrier : pending_barriers) {
      for (auto &promise : barrier.promises) {
        fulfill(promise, failed_barrier(barrier.target, tracker));
      }
    }
    pending_barriers.clear();
    pending_barrier_promises = 0U;
  }

  [[nodiscard]] bool flush_active() {
    if (!worker_active) {
      return true;
    }
    auto status = file_system->flush();
    tracker.mark_flush_result(static_cast<bool>(status));
    bytes_since_flush = 0U;
    next_flush = std::chrono::steady_clock::now() + options.flush_interval;
    if (!status) {
      set_error(status.error());
      complete_failed_barriers();
      static_cast<void>(file_system->close());
      worker_active = false;
      std::scoped_lock lock{mutex};
      active_file.clear();
      return false;
    }
    for (auto iterator = pending_barriers.begin();
         iterator != pending_barriers.end();) {
      if (iterator->target <= tracker.flushed_through_seq()) {
        const auto result = tracker.check({iterator->target});
        for (auto &promise : iterator->promises) {
          fulfill(promise, result);
        }
        pending_barrier_promises -= iterator->promises.size();
        iterator = pending_barriers.erase(iterator);
      } else {
        ++iterator;
      }
    }
    return true;
  }

  void observe_overload() {
    if (!overloaded.exchange(false, std::memory_order_acq_rel)) {
      return;
    }
    set_error(log_error("session log queue reached its hard limit"));
    complete_failed_barriers();
    if (worker_active) {
      static_cast<void>(file_system->close());
      worker_active = false;
      std::scoped_lock lock{mutex};
      active_file.clear();
    }
  }

  void handle(RecordItem &item) {
    observe_overload();
    if (!worker_active ||
        state.load(std::memory_order_acquire) == SessionLogState::Error) {
      return;
    }
    {
      const auto &record = *item.record;
      const bool included =
          (record.direction() != Direction::Sys || options.include_system) &&
          (record.direction() != Direction::Err || options.include_error);
      if (included) {
        auto encoded = encode_record_line(record);
        if (!encoded) {
          set_error(log_error(encoded.error().detail, 0,
                              ErrorCode::LoggingSchemaInvalid));
          complete_failed_barriers();
          static_cast<void>(file_system->close());
          worker_active = false;
          std::scoped_lock lock{mutex};
          active_file.clear();
          return;
        }
        auto status = file_system->append_line(*encoded);
        if (!status) {
          set_error(status.error());
          complete_failed_barriers();
          static_cast<void>(file_system->close());
          worker_active = false;
          std::scoped_lock lock{mutex};
          active_file.clear();
          return;
        }
        bytes_since_flush += encoded->size();
      }
      static_cast<void>(tracker.mark_processed_through(record.seq()));
      processed.store(tracker.processed_through_seq(),
                      std::memory_order_release);
    }
    const bool barrier_ready = std::ranges::any_of(
        pending_barriers, [this](const PendingBarrier &barrier) {
          return barrier.target <= tracker.processed_through_seq();
        });
    if (bytes_since_flush >= options.flush_batch_bytes || barrier_ready ||
        std::chrono::steady_clock::now() >= next_flush) {
      static_cast<void>(flush_active());
    }
  }

  void handle(StartItem &item) {
    observe_overload();
    if (state.load(std::memory_order_acquire) != SessionLogState::Waiting) {
      SessionCommandResult result;
      {
        std::scoped_lock lock{mutex};
        start_pending = false;
        result = command_result_locked();
      }
      fulfill(item.promise, std::move(result));
      return;
    }
    auto encoded = encode_header_line(item.header);
    if (!encoded) {
      set_error(log_error(encoded.error().detail, 0,
                          ErrorCode::LoggingSchemaInvalid));
      {
        std::scoped_lock lock{mutex};
        start_pending = false;
      }
      fulfill(item.promise, command_result());
      return;
    }
    auto status = file_system->begin_session(
        options.directory, item.header.started_at, *encoded, options.quotas);
    if (!status) {
      set_error(status.error());
      {
        std::scoped_lock lock{mutex};
        start_pending = false;
      }
      fulfill(item.promise, command_result());
      return;
    }
    worker_active = true;
    tracker = BarrierTracker{};
    processed.store(0U, std::memory_order_release);
    bytes_since_flush = encoded->size();
    next_flush = std::chrono::steady_clock::now() + options.flush_interval;
    auto path = file_system->active_path();
    bool cancelled = false;
    {
      std::scoped_lock lock{mutex};
      start_pending = false;
      cancelled =
          state.load(std::memory_order_acquire) != SessionLogState::Waiting;
      if (!cancelled) {
        active_file = std::move(path);
        last_enqueued_seq = 0U;
        terminal_error.reset();
        state.store(SessionLogState::Recording, std::memory_order_release);
      }
    }
    if (cancelled) {
      static_cast<void>(file_system->close());
      worker_active = false;
    }
    fulfill(item.promise, command_result());
  }

  void handle(CloseItem &item) {
    observe_overload();
    if (worker_active) {
      static_cast<void>(flush_active());
      auto status = file_system->close();
      worker_active = false;
      if (!status) {
        set_error(status.error());
      }
    }
    complete_failed_barriers();
    std::vector<std::promise<SessionCommandResult>> disable_completions;
    {
      std::scoped_lock lock{mutex};
      active_file.clear();
      if (state.load(std::memory_order_acquire) != SessionLogState::Error) {
        state.store(item.final_state, std::memory_order_release);
      } else if (item.final_state == SessionLogState::Off) {
        terminal_error.reset();
        state.store(SessionLogState::Off, std::memory_order_release);
      }
      if (item.final_state == SessionLogState::Off) {
        disable_pending = false;
        close_pending = false;
        disable_completions.swap(disable_waiters);
      } else {
        close_pending = disable_pending;
      }
    }
    const auto result = command_result();
    for (auto &promise : item.promises) {
      fulfill(promise, result);
    }
    for (auto &promise : disable_completions) {
      fulfill(promise, result);
    }
  }

  void handle(BarrierItem &item) {
    observe_overload();
    if (!worker_active ||
        state.load(std::memory_order_acquire) == SessionLogState::Error) {
      fulfill(item.promise, failed_barrier(item.target, tracker));
      return;
    }
    if (item.target <= tracker.processed_through_seq()) {
      static_cast<void>(flush_active());
      if (state.load(std::memory_order_acquire) == SessionLogState::Error) {
        fulfill(item.promise, failed_barrier(item.target, tracker));
      } else {
        fulfill(item.promise, tracker.check({item.target}));
      }
      return;
    }
    if (pending_barrier_promises >= kMaxPendingBarrierPromises) {
      fulfill(item.promise, failed_barrier(item.target, tracker));
      return;
    }
    const auto matching = std::ranges::find_if(
        pending_barriers, [&item](const PendingBarrier &barrier) {
          return barrier.target == item.target;
        });
    if (matching != pending_barriers.end()) {
      matching->promises.push_back(std::move(item.promise));
    } else {
      PendingBarrier pending;
      pending.target = item.target;
      pending.promises.push_back(std::move(item.promise));
      pending_barriers.push_back(std::move(pending));
    }
    ++pending_barrier_promises;
  }

  void finish_stop() {
    observe_overload();
    if (worker_active) {
      static_cast<void>(flush_active());
      auto status = file_system->close();
      worker_active = false;
      if (!status) {
        set_error(status.error());
      }
    }
    complete_failed_barriers();
    {
      std::scoped_lock lock{mutex};
      active_file.clear();
      if (state.load(std::memory_order_acquire) != SessionLogState::Error) {
        state.store(SessionLogState::Off, std::memory_order_release);
      }
    }
  }

  void publish_fatal(FatalReason reason) noexcept {
    static_cast<void>(
        fatal.publish({reason == FatalReason::OutOfMemory
                           ? ErrorCode::InternalOutOfMemory
                           : ErrorCode::InternalInvariantBroken,
                       Operation::WriteSessionLog, WorkerKind::SessionLog,
                       reason, SignalSourceLocation::current()}));
  }

  void fail_worker(Item *current) noexcept {
    state.store(SessionLogState::Error, std::memory_order_release);
    // The virtual close contract is noexcept, not allocation-free. Leave the
    // filesystem owned by Impl on fatal paths; the owner enforces its deadline.
    if (!fatal.load()) {
      static_cast<void>(file_system->close());
    }
    worker_active = false;
    const auto command = failed_command_result();
    const auto complete_item = [this, &command](Item &item) noexcept {
      std::visit(
          [this, &command](auto &value) noexcept {
            using Value = std::remove_cvref_t<decltype(value)>;
            if constexpr (std::is_same_v<Value, StartItem>) {
              fulfill(value.promise, command);
            } else if constexpr (std::is_same_v<Value, CloseItem>) {
              for (auto &promise : value.promises) {
                fulfill(promise, command);
              }
            } else if constexpr (std::is_same_v<Value, BarrierItem>) {
              fulfill(value.promise, failed_barrier(value.target, tracker));
            } else if constexpr (std::is_same_v<Value, ShutdownItem>) {
              fulfill(value.promise, command);
            }
          },
          item);
    };
    if (current != nullptr) {
      complete_item(*current);
    }
    std::scoped_lock lock{mutex};
    stopping = true;
    worker_failed = true;
    start_pending = false;
    close_pending = false;
    disable_pending = false;
    active_file.clear();
    for (auto &item : items) {
      complete_item(item);
    }
    items.clear();
    for (auto &barrier : pending_barriers) {
      for (auto &promise : barrier.promises) {
        fulfill(promise, failed_barrier(barrier.target, tracker));
      }
    }
    pending_barriers.clear();
    for (auto &promise : disable_waiters) {
      fulfill(promise, command);
    }
    disable_waiters.clear();
    queued_control_count = 0U;
    pending_barrier_promises = 0U;
    queued_record_count = 0U;
    queued_byte_count = 0U;
    condition.notify_all();
  }

  void run() noexcept {
    std::optional<Item> item;
    try {
      while (true) {
        item.reset();
        bool stop_when_empty = false;
        {
          std::unique_lock lock{mutex};
          if (worker_active && bytes_since_flush != 0U) {
            condition.wait_until(lock, next_flush, [this] {
              return stopping || !items.empty() || overloaded;
            });
          } else {
            condition.wait(lock, [this] {
              return stopping || !items.empty() || overloaded;
            });
          }
          if (!items.empty()) {
            item.emplace(std::move(items.front()));
            if (const auto *record = std::get_if<RecordItem>(&*item)) {
              --queued_record_count;
              queued_byte_count -= record->bytes;
            } else {
              --queued_control_count;
            }
            items.pop_front();
          } else {
            stop_when_empty = stopping;
          }
        }
        observe_overload();
        if (!item) {
          if (stop_when_empty) {
            finish_stop();
            break;
          }
          if (worker_active && bytes_since_flush != 0U &&
              std::chrono::steady_clock::now() >= next_flush) {
            static_cast<void>(flush_active());
          }
          continue;
        }
        bool stop = false;
        std::visit(
            [this, &stop](auto &value) {
              using Value = std::remove_cvref_t<decltype(value)>;
              if constexpr (std::is_same_v<Value, ShutdownItem>) {
                finish_stop();
                fulfill(value.promise, command_result());
                stop = true;
              } else {
                handle(value);
              }
            },
            *item);
        if (stop) {
          break;
        }
        item.reset();
      }
    } catch (const std::bad_alloc &) {
      publish_fatal(FatalReason::OutOfMemory);
      fail_worker(item ? &*item : nullptr);
    } catch (const std::logic_error &) {
      publish_fatal(FatalReason::InvariantBroken);
      fail_worker(item ? &*item : nullptr);
    } catch (const std::exception &) {
      try {
        set_error(log_error("session log filesystem exception"));
      } catch (const std::bad_alloc &) {
        publish_fatal(FatalReason::OutOfMemory);
      } catch (...) {
        publish_fatal(FatalReason::ErrorAdaptationFailed);
      }
      fail_worker(item ? &*item : nullptr);
    } catch (...) {
      publish_fatal(FatalReason::UnknownException);
      fail_worker(item ? &*item : nullptr);
    }
  }

  model::GlobalMemoryBudget memory_budget;
  SessionWriterOptions options;
  std::unique_ptr<SessionLogFileSystem> file_system;
  mutable std::mutex mutex;
  std::condition_variable condition;
  std::deque<Item> items;
  std::vector<PendingBarrier> pending_barriers;
  std::vector<std::promise<SessionCommandResult>> disable_waiters;
  std::atomic<SessionLogState> state{SessionLogState::Off};
  std::atomic<std::uint64_t> processed{};
  std::atomic<bool> overloaded{};
  std::jthread worker;
  BarrierTracker tracker;
  std::optional<Error> terminal_error;
  std::filesystem::path active_file;
  std::size_t queued_record_count{};
  std::size_t queued_byte_count{};
  std::size_t queued_control_count{};
  std::size_t pending_barrier_promises{};
  std::size_t bytes_since_flush{};
  std::uint64_t last_enqueued_seq{};
  std::chrono::steady_clock::time_point next_flush{};
  bool worker_active{};
  bool stopping{};
  bool worker_failed{};
  bool start_pending{};
  bool close_pending{};
  bool disable_pending{};
  std::atomic<bool> stopped_ready{};
  FatalSignalSlot fatal;
};

SessionWriter::SessionWriter(SessionWriterOptions options,
                             std::unique_ptr<SessionLogFileSystem> file_system,
                             model::GlobalMemoryBudget budget)
    : impl_{std::make_unique<Impl>(std::move(options), std::move(file_system),
                                   std::move(budget))} {}

SessionWriter::~SessionWriter() {
  if (!impl_) {
    return;
  }
  request_stop();
  static_cast<void>(
      wait_until_stopped(std::chrono::steady_clock::time_point::max()));
  if (impl_->worker.joinable()) {
    impl_->worker.join();
  }
}

bool SessionWriter::enable() noexcept {
  std::scoped_lock lock{impl_->mutex};
  if (impl_->stopping) {
    return false;
  }
  if (impl_->close_pending) {
    return false;
  }
  const auto current = impl_->state.load(std::memory_order_acquire);
  if (current == SessionLogState::Error) {
    return false;
  }
  if (current == SessionLogState::Off) {
    impl_->terminal_error.reset();
    impl_->state.store(SessionLogState::Waiting, std::memory_order_release);
  }
  return true;
}

std::future<SessionCommandResult> SessionWriter::disable() {
  std::promise<SessionCommandResult> promise;
  auto future = promise.get_future();
  Impl::CloseItem item;
  bool queued = false;
  {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->stopping || impl_->worker_failed) {
      return ready_future(impl_->command_result_locked());
    }
    if (impl_->state.load(std::memory_order_acquire) == SessionLogState::Off &&
        !impl_->disable_pending && !impl_->close_pending) {
      return ready_future(impl_->command_result_locked());
    }
    if (impl_->disable_waiters.size() >= kMaxDisableWaiters ||
        (!impl_->disable_pending &&
         impl_->queued_control_count >= kMaxQueuedControlCommands)) {
      auto result = impl_->command_result_locked();
      result.error = log_error("session log control capacity is full");
      return ready_future(std::move(result));
    }
    impl_->disable_waiters.push_back(std::move(promise));
    // Close the producer gate before publishing the asynchronous close command;
    // records admitted before this store remain ordered ahead of that command.
    impl_->state.store(SessionLogState::Off, std::memory_order_release);
    impl_->close_pending = true;
    if (!impl_->disable_pending) {
      impl_->disable_pending = true;
      item.final_state = SessionLogState::Off;
      impl_->items.emplace_back(std::move(item));
      ++impl_->queued_control_count;
      queued = true;
    }
  }
  if (queued) {
    impl_->condition.notify_one();
  }
  return future;
}

std::future<SessionCommandResult> SessionWriter::start_session(Header header) {
  Impl::StartItem item{std::move(header), {}};
  auto future = item.promise.get_future();
  {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->stopping || impl_->worker_failed ||
        impl_->state.load(std::memory_order_acquire) !=
            SessionLogState::Waiting ||
        impl_->start_pending || impl_->close_pending ||
        impl_->queued_control_count >= kMaxQueuedControlCommands) {
      return ready_future(impl_->command_result_locked());
    }
    impl_->start_pending = true;
    impl_->items.emplace_back(std::move(item));
    ++impl_->queued_control_count;
  }
  impl_->condition.notify_one();
  return future;
}

std::future<SessionCommandResult> SessionWriter::end_session() {
  Impl::CloseItem item;
  item.promises.emplace_back();
  auto future = item.promises.front().get_future();
  {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->stopping || impl_->worker_failed ||
        impl_->state.load(std::memory_order_acquire) !=
            SessionLogState::Recording ||
        impl_->close_pending ||
        impl_->queued_control_count >= kMaxQueuedControlCommands) {
      return ready_future(impl_->command_result_locked());
    }
    // Waiting is visible immediately so no producer can enqueue behind the
    // close command while the worker drains the preceding records.
    impl_->state.store(SessionLogState::Waiting, std::memory_order_release);
    impl_->close_pending = true;
    item.final_state = SessionLogState::Waiting;
    impl_->items.emplace_back(std::move(item));
    ++impl_->queued_control_count;
  }
  impl_->condition.notify_one();
  return future;
}

EnqueueResult
SessionWriter::try_enqueue(model::SessionRecordPtr record) noexcept {
  if (!record) {
    return EnqueueResult::EmptyRecord;
  }
  try {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->stopping) {
      return EnqueueResult::Stopping;
    }
    if (impl_->state.load(std::memory_order_acquire) !=
        SessionLogState::Recording) {
      return EnqueueResult::NotRecording;
    }
    if (record->seq() == 0U || record->seq() <= impl_->last_enqueued_seq) {
      return EnqueueResult::InvalidRecord;
    }
    const auto bytes =
        record->logical_bytes() + sizeof(Impl::RecordItem) + 128U;
    auto reservation = impl_->memory_budget.try_reserve(
        model::BudgetCategory::SessionLog, sizeof(Impl::Item) + 128U);
    if (!reservation ||
        impl_->queued_record_count >= impl_->options.queue_max_records ||
        bytes > impl_->options.queue_max_bytes ||
        impl_->queued_byte_count > impl_->options.queue_max_bytes - bytes) {
      impl_->overloaded.store(true, std::memory_order_release);
      impl_->state.store(SessionLogState::Error, std::memory_order_release);
      impl_->condition.notify_one();
      return EnqueueResult::QueueFull;
    }
    impl_->last_enqueued_seq = record->seq();
    ++impl_->queued_record_count;
    impl_->queued_byte_count += bytes;
    impl_->items.emplace_back(
        Impl::RecordItem{std::move(record), bytes, std::move(*reservation)});
    impl_->condition.notify_one();
    return EnqueueResult::Accepted;
  } catch (...) {
    impl_->overloaded.store(true, std::memory_order_release);
    impl_->state.store(SessionLogState::Error, std::memory_order_release);
    impl_->condition.notify_one();
    return EnqueueResult::QueueFull;
  }
}

void SessionWriter::fail_overload() noexcept {
  {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->stopping ||
        impl_->state.load(std::memory_order_acquire) == SessionLogState::Off) {
      return;
    }
    impl_->overloaded.store(true, std::memory_order_release);
    impl_->state.store(SessionLogState::Error, std::memory_order_release);
  }
  impl_->condition.notify_one();
}

std::future<BarrierResult> SessionWriter::barrier(std::uint64_t target_seq) {
  Impl::BarrierItem item{target_seq, {}};
  auto future = item.promise.get_future();
  {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->stopping || impl_->worker_failed ||
        impl_->queued_control_count >= kMaxQueuedControlCommands) {
      const auto processed = impl_->processed.load(std::memory_order_acquire);
      return ready_future(BarrierResult{BarrierState::WriterFailed, target_seq,
                                        processed, processed, processed,
                                        false});
    }
    impl_->items.emplace_back(std::move(item));
    ++impl_->queued_control_count;
  }
  impl_->condition.notify_one();
  return future;
}

std::future<SessionCommandResult> SessionWriter::shutdown() {
  Impl::ShutdownItem item;
  auto future = item.promise.get_future();
  {
    std::scoped_lock lock{impl_->mutex};
    if (impl_->stopping || impl_->worker_failed) {
      return ready_future(impl_->command_result_locked());
    }
    impl_->items.emplace_back(std::move(item));
    impl_->stopping = true;
    ++impl_->queued_control_count;
  }
  impl_->condition.notify_one();
  return future;
}

void SessionWriter::request_stop() noexcept {
  {
    // Use the wait predicate's mutex so notification cannot be lost between
    // checking the predicate and sleeping. Never called while holding it.
    std::scoped_lock lock{impl_->mutex};
    impl_->stopping = true;
  }
  impl_->condition.notify_all();
}

std::optional<FatalSignal> SessionWriter::fatal_signal() const noexcept {
  return impl_->fatal.load();
}

bool SessionWriter::wait_until_stopped(
    const std::chrono::steady_clock::time_point deadline) const noexcept {
  while (!impl_->stopped_ready.load(std::memory_order_acquire)) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return false;
    }
    std::this_thread::sleep_until(
        std::min(deadline, now + std::chrono::milliseconds{1}));
  }
  return true;
}

SessionLogState SessionWriter::state() const noexcept {
  return impl_->state.load(std::memory_order_acquire);
}

std::uint64_t SessionWriter::processed_through_seq() const noexcept {
  return impl_->processed.load(std::memory_order_acquire);
}

std::size_t SessionWriter::queued_records() const noexcept {
  std::scoped_lock lock{impl_->mutex};
  return impl_->queued_record_count;
}

std::size_t SessionWriter::queued_bytes() const noexcept {
  std::scoped_lock lock{impl_->mutex};
  return impl_->queued_byte_count;
}

std::filesystem::path SessionWriter::active_path() const {
  std::scoped_lock lock{impl_->mutex};
  return impl_->active_file;
}

} // namespace lazycom::logging
