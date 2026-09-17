#include <lazycom/app/signals.hpp>
#include <lazycom/base/ids.hpp>
#include <lazycom/diagnostics/diagnostics.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdint>
#include <latch>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

using namespace lazycom;
using namespace lazycom::app;

static_assert([] {
  ConnectionGeneration generation{41};
  OperationId maximum{std::numeric_limits<std::uint64_t>::max()};
  IdSequence<TaskGeneration> sequence{TaskGeneration{maximum.value}};
  TaskGeneration issued{9};
  return increment_id(generation) == IdIncrementResult::Advanced &&
         generation.value == 42 &&
         increment_id(maximum) == IdIncrementResult::Overflow &&
         maximum.value == std::numeric_limits<std::uint64_t>::max() &&
         sequence.issue(issued) == IdIncrementResult::Overflow &&
         issued == TaskGeneration{9};
}());
static_assert(std::is_trivially_copyable_v<FatalSignal>);
static_assert(std::is_trivially_copyable_v<WorkerStoppedSignal>);
static_assert(sizeof(FatalSignal) <= 64);
static_assert(noexcept(lazycom::diagnostics::emergency_write()));
static_assert(noexcept(lazycom::diagnostics::install_terminate_handler()));

TEST_CASE("concurrent and later fatal publishers preserve exactly one winner",
          "[signals]") {
  constexpr std::size_t publisher_count = 16U;
  FatalSignalSlot slot;
  std::latch start{1};
  std::atomic<std::size_t> winners{0U};
  std::vector<std::jthread> publishers;
  publishers.reserve(publisher_count);
  {
    // Release already-created threads even if a later thread cannot be created.
    struct ReleaseStart {
      std::latch &start;
      ~ReleaseStart() { start.count_down(); }
    } release{start};
    for (std::size_t index = 0U; index < publisher_count; ++index) {
      publishers.emplace_back([&, index] {
        start.wait();
        const auto source =
            SignalSourceLocation{"concurrent", "publisher",
                                 static_cast<std::uint_least32_t>(index), 0U};
        if (slot.publish({ErrorCode::InternalInvariantBroken,
                          Operation::CoordinateFatal, WorkerKind::Serial,
                          FatalReason::InvariantBroken, source})) {
          winners.fetch_add(1U, std::memory_order_relaxed);
        }
      });
    }
  }
  publishers.clear();

  REQUIRE(winners.load(std::memory_order_relaxed) == 1U);
  REQUIRE(slot.load().has_value());
  REQUIRE(slot.load()->source.line < publisher_count);
  REQUIRE(slot.additional_count() == publisher_count - 1U);
  CHECK_FALSE(slot.publish({ErrorCode::InternalOutOfMemory,
                            Operation::SaveConfig,
                            WorkerKind::Persistence,
                            FatalReason::OutOfMemory,
                            {}}));
  CHECK(slot.load()->worker == WorkerKind::Serial);
  CHECK(slot.load()->code == ErrorCode::InternalInvariantBroken);
  CHECK(slot.additional_count() == publisher_count);
}

TEST_CASE("worker trampoline separates normal exceptions from fatal failures",
          "[signals]") {
  const auto normal = run_worker_trampoline(
      WorkerKind::Scanner, Operation::EnumerateDevices,
      [] { throw std::runtime_error("enumeration failed"); },
      ErrorCode::DiagnosticsUnavailable);
  REQUIRE(normal.has_error());
  REQUIRE_FALSE(normal.is_fatal());
  REQUIRE(normal.error->detail == "enumeration failed");
  REQUIRE(normal.stopped.reason == WorkerExitReason::RecoverableError);

  const auto oom =
      run_worker_trampoline(WorkerKind::Persistence, Operation::SaveConfig,
                            [] { throw std::bad_alloc{}; });
  REQUIRE(oom.is_fatal());
  REQUIRE_FALSE(oom.has_error());
  REQUIRE(oom.fatal->reason == FatalReason::OutOfMemory);
  REQUIRE(oom.fatal->code == ErrorCode::InternalOutOfMemory);

  const auto unknown = run_worker_trampoline(
      WorkerKind::Serial, Operation::ReadSerial, [] { throw 7; });
  REQUIRE(unknown.is_fatal());
  REQUIRE(unknown.fatal->reason == FatalReason::UnknownException);

  const auto completed = run_worker_trampoline(
      WorkerKind::Diagnostics, Operation::WriteSessionLog, [] {});
  REQUIRE_FALSE(completed.is_fatal());
  REQUIRE_FALSE(completed.has_error());
  REQUIRE(completed.stopped.reason == WorkerExitReason::Completed);
}
