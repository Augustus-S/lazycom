#pragma once

#include <lazycom/base/error.hpp>

#include <atomic>
#include <cstdint>
#include <exception>
#include <new>
#include <optional>
#include <source_location>
#include <string_view>
#include <type_traits>
#include <utility>

namespace lazycom::app {

enum class WorkerKind : std::uint8_t {
  Serial,
  SessionLog,
  Scanner,
  Persistence,
  Diagnostics,
};

inline constexpr std::size_t worker_kind_count = 5;

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

struct FatalSignal {
  ErrorCode code{ErrorCode::InternalInvariantBroken};
  Operation operation{Operation::ValidateConfig};
  WorkerKind worker{WorkerKind::Serial};
  FatalReason reason{FatalReason::InvariantBroken};
  SignalSourceLocation source{};
};

class FatalSignalSlot {
public:
  FatalSignalSlot() noexcept = default;
  FatalSignalSlot(const FatalSignalSlot &) = delete;
  FatalSignalSlot &operator=(const FatalSignalSlot &) = delete;

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

struct WorkerTrampolineResult {
  WorkerStoppedSignal stopped{};
  std::optional<Error> error;
  std::optional<FatalSignal> fatal;

  [[nodiscard]] bool is_fatal() const noexcept { return fatal.has_value(); }
  [[nodiscard]] bool has_error() const noexcept { return error.has_value(); }
};

namespace detail {

[[nodiscard]] inline WorkerTrampolineResult
fatal_worker_result(const WorkerKind worker, const Operation operation,
                    const FatalReason reason,
                    const SignalSourceLocation source) noexcept {
  WorkerTrampolineResult result;
  result.stopped = {worker, WorkerLifecycle::AtReturnPoint,
                    WorkerExitReason::Fatal};
  const auto code = reason == FatalReason::OutOfMemory
                        ? ErrorCode::InternalOutOfMemory
                        : ErrorCode::InternalInvariantBroken;
  result.fatal = FatalSignal{code, operation, worker, reason, source};
  return result;
}

} // namespace detail

template <class WorkerBody>
[[nodiscard]] WorkerTrampolineResult run_worker_trampoline(
    const WorkerKind worker, const Operation operation,
    WorkerBody &&worker_body,
    const ErrorCode exception_code = ErrorCode::InternalInvariantBroken,
    const std::source_location source =
        std::source_location::current()) noexcept {
  const auto signal_source = SignalSourceLocation::current(source);
  try {
    std::forward<WorkerBody>(worker_body)();
    WorkerTrampolineResult result;
    result.stopped = {worker, WorkerLifecycle::AtReturnPoint,
                      WorkerExitReason::Completed};
    return result;
  } catch (const std::bad_alloc &) {
    return detail::fatal_worker_result(worker, operation,
                                       FatalReason::OutOfMemory, signal_source);
  } catch (const std::exception &exception) {
    try {
      WorkerTrampolineResult result;
      result.stopped = {worker, WorkerLifecycle::AtReturnPoint,
                        WorkerExitReason::RecoverableError};
      const char *const message = exception.what();
      const std::string_view detail = message == nullptr
                                          ? std::string_view{"std::exception"}
                                          : std::string_view{message};
      result.error = make_error(exception_code, operation, detail, {},
                                std::nullopt, std::nullopt, source);
      return result;
    } catch (const std::bad_alloc &) {
      return detail::fatal_worker_result(
          worker, operation, FatalReason::OutOfMemory, signal_source);
    } catch (...) {
      return detail::fatal_worker_result(
          worker, operation, FatalReason::ErrorAdaptationFailed, signal_source);
    }
  } catch (...) {
    return detail::fatal_worker_result(
        worker, operation, FatalReason::UnknownException, signal_source);
  }
}

} // namespace lazycom::app
