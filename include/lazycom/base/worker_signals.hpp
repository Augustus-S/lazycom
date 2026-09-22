#pragma once

#include <lazycom/base/error.hpp>

#include <atomic>
#include <cstdint>
#include <optional>
#include <source_location>

namespace lazycom {

enum class WorkerKind : std::uint8_t {
  Serial,
  SessionLog,
  Scanner,
  Persistence,
  Diagnostics,
};

enum class WorkerLifecycle : std::uint8_t {
  NotStarted,
  Running,
  AtReturnPoint,
};

enum class FatalReason : std::uint8_t {
  OutOfMemory,
  UnknownException,
  ErrorAdaptationFailed,
  InvariantBroken,
};

struct SignalSourceLocation {
  const char *file_name{};
  const char *function_name{};
  std::uint_least32_t line{};
  std::uint_least32_t column{};

  [[nodiscard]] static constexpr SignalSourceLocation
  current(const std::source_location source =
              std::source_location::current()) noexcept {
    return {source.file_name(), source.function_name(), source.line(),
            source.column()};
  }
};

/**
 * @brief Fixed-size emergency notification published by a failed worker.
 *
 * Publishing this value requires no dynamic allocation or normal queue. The
 * source string pointers normally refer to static source-location storage; any
 * custom pointers must outlive all observations of the signal.
 */
struct FatalSignal {
  ErrorCode code{ErrorCode::InternalInvariantBroken};
  Operation operation{Operation::ValidateConfig};
  WorkerKind worker{WorkerKind::Serial};
  FatalReason reason{FatalReason::InvariantBroken};
  SignalSourceLocation source{};
};

/**
 * @brief Lock-free first-writer-wins storage for one worker fatal signal.
 *
 * publish(), load(), and additional_count() may be called concurrently while
 * the slot remains alive. The slot has no reset operation. Later publishers do
 * not replace the first signal and only increment the additional count.
 */
class FatalSignalSlot {
public:
  FatalSignalSlot() noexcept = default;
  FatalSignalSlot(const FatalSignalSlot &) = delete;
  FatalSignalSlot &operator=(const FatalSignalSlot &) = delete;

  /** @return true only for the signal that changes the slot to ready. */
  [[nodiscard]] bool publish(FatalSignal signal) noexcept {
    std::uint8_t expected = empty;
    if (!state_.compare_exchange_strong(expected, writing,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
      additional_count_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    signal_ = signal;
    state_.store(ready, std::memory_order_release);
    return true;
  }

  /** @return The first signal after publication completes, otherwise nullopt.
   */
  [[nodiscard]] std::optional<FatalSignal> load() const noexcept {
    if (state_.load(std::memory_order_acquire) != ready) {
      return std::nullopt;
    }
    return signal_;
  }

  [[nodiscard]] std::uint32_t additional_count() const noexcept {
    return additional_count_.load(std::memory_order_relaxed);
  }

private:
  static constexpr std::uint8_t empty = 0;
  static constexpr std::uint8_t writing = 1;
  static constexpr std::uint8_t ready = 2;

  std::atomic<std::uint8_t> state_{empty};
  std::atomic<std::uint32_t> additional_count_{0};
  FatalSignal signal_{};
};

enum class WorkerExitReason : std::uint8_t {
  Completed,
  RecoverableError,
  Fatal,
};

struct WorkerStoppedSignal {
  WorkerKind worker{WorkerKind::Serial};
  WorkerLifecycle lifecycle{WorkerLifecycle::AtReturnPoint};
  WorkerExitReason reason{WorkerExitReason::Completed};
};

static_assert(std::is_trivially_copyable_v<SignalSourceLocation>);
static_assert(std::is_trivially_copyable_v<FatalSignal>);
static_assert(sizeof(FatalSignal) <= 64);
static_assert(std::is_trivially_copyable_v<WorkerStoppedSignal>);

} // namespace lazycom
