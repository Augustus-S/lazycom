#include <lazycom/app/signals.hpp>
#include <lazycom/base/ids.hpp>
#include <lazycom/diagnostics/diagnostics.hpp>

#include <support/fake_clock.hpp>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

using namespace lazycom;
using namespace lazycom::app;

TEST_CASE("strong identifiers advance without wrapping", "[ids]") {
  ConnectionGeneration generation{41};
  REQUIRE(increment_id(generation) == IdIncrementResult::Advanced);
  REQUIRE(generation.value == 42);

  OperationId maximum{std::numeric_limits<std::uint64_t>::max()};
  REQUIRE(increment_id(maximum) == IdIncrementResult::Overflow);
  REQUIRE(maximum.value == std::numeric_limits<std::uint64_t>::max());

  IdSequence<TaskGeneration> sequence{
      TaskGeneration{std::numeric_limits<std::uint64_t>::max()}};
  TaskGeneration issued{9};
  REQUIRE(sequence.issue(issued) == IdIncrementResult::Overflow);
  REQUIRE(issued == TaskGeneration{9});
}

TEST_CASE("fake clock advances deterministically without sleeping",
          "[support]") {
  lazycom::test::FakeClock clock;
  const auto start = clock.now();
  clock.advance(std::chrono::milliseconds{125});
  REQUIRE(clock.now() - start == std::chrono::milliseconds{125});
}

TEST_CASE("fatal and stopped signals are fixed trivial values", "[signals]") {
  STATIC_REQUIRE(std::is_trivially_copyable_v<FatalSignal>);
  STATIC_REQUIRE(std::is_trivially_copyable_v<WorkerStoppedSignal>);
  STATIC_REQUIRE(sizeof(FatalSignal) <= 64);
  STATIC_REQUIRE(noexcept(lazycom::diagnostics::emergency_write()));
  STATIC_REQUIRE(noexcept(lazycom::diagnostics::install_terminate_handler()));
}

TEST_CASE("first fatal signal wins without using a dynamic queue",
          "[signals]") {
  FatalSignalSlot slot;
  const FatalSignal first{ErrorCode::InternalOutOfMemory,
                          Operation::SaveConfig,
                          WorkerKind::Persistence,
                          FatalReason::OutOfMemory,
                          {}};
  const FatalSignal second{ErrorCode::InternalInvariantBroken,
                           Operation::ReadSerial,
                           WorkerKind::Serial,
                           FatalReason::InvariantBroken,
                           {}};

  REQUIRE(slot.publish(first));
  REQUIRE_FALSE(slot.publish(second));
  REQUIRE(slot.load());
  REQUIRE(slot.load()->worker == WorkerKind::Persistence);
  REQUIRE(slot.load()->code == ErrorCode::InternalOutOfMemory);
  REQUIRE(slot.additional_count() == 1);
}

TEST_CASE("concurrent fatal publishers have exactly one winner", "[signals]") {
  constexpr std::size_t publisher_count = 16U;
  FatalSignalSlot slot;
  std::barrier start{static_cast<std::ptrdiff_t>(publisher_count)};
  std::atomic<std::size_t> winners{0U};
  std::vector<std::jthread> publishers;
  publishers.reserve(publisher_count);
  for (std::size_t index = 0U; index < publisher_count; ++index) {
    publishers.emplace_back([&, index] {
      start.arrive_and_wait();
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
  publishers.clear();

  REQUIRE(winners.load(std::memory_order_relaxed) == 1U);
  REQUIRE(slot.load().has_value());
  REQUIRE(slot.load()->source.line < publisher_count);
  REQUIRE(slot.additional_count() == publisher_count - 1U);
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
