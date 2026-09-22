#include <catch2/catch_test_macros.hpp>
#include <lazycom/serial/backend.hpp>
#include <lazycom/serial/service.hpp>
#include <support/serial.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <fcntl.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace lazycom;
using namespace lazycom::serial;

bool wait_until(const auto &predicate) {
  return test::wait_until(predicate, 2s);
}
struct PtyOwner {
  PtyOwner() {
    auto inspected = inspect_device_path(pty.slave_path.data());
    REQUIRE(inspected);
    auto created =
        SerialService::create(std::make_unique<LibserialportBackend>());
    REQUIRE(created);
    service = std::move(*created);
    REQUIRE(service->submit_connect(
        {{OperationId{1}, ConnectionGeneration{1}}, *inspected, {}}));
  }
  ~PtyOwner() {
    service->request_stop();
    if (!service->wait_until_stopped(std::chrono::steady_clock::now() + 2s))
      std::terminate();
  }
  std::vector<SerialCompletion> completions(std::size_t count) {
    return test::collect_completions(*service, count, 2s);
  }
  ConnectCompletion connected() {
    const auto results = completions(1U);
    REQUIRE(std::holds_alternative<ConnectCompletion>(results[0]));
    auto result = std::get<ConnectCompletion>(results[0]);
    REQUIRE(result.outcome == OperationOutcome::Succeeded);
    REQUIRE(result.session_id);
    return result;
  }
  test::Pty pty;
  std::unique_ptr<SerialService> service;
};
} // namespace

TEST_CASE("real PTY owner exchanges bytes and settles connection lifecycle",
          "[integration][serial][pty]") {
  PtyOwner test;
  SECTION("a queued connection can be cancelled or has already connected") {
    const auto cancelled = test.service->request_cancel_connect(
        {OperationId{2}, ConnectionGeneration{1}});
    if (cancelled) {
      const auto results = test.completions(2U);
      REQUIRE(std::holds_alternative<ConnectCompletion>(results[0]));
      CHECK(std::get<ConnectCompletion>(results[0]).outcome ==
            OperationOutcome::Cancelled);
      REQUIRE(std::holds_alternative<DisconnectCompletion>(results[1]));
    } else {
      const auto connected = test.connected();
      REQUIRE(test.service->request_disconnect(
          {OperationId{3}, ConnectionGeneration{1}, connected.session_id}));
      REQUIRE(std::holds_alternative<DisconnectCompletion>(
          test.completions(1U)[0]));
    }
  }
  SECTION(
      "periodic deadlines run without a UI tick and stop is a write barrier") {
    const auto connected = test.connected();
    scheduler::QuickSendExecution execution{1U,
                                            "probe",
                                            config::SendMode::Txt,
                                            config::Newline::None,
                                            config::Newline::None,
                                            "",
                                            {std::byte{'Q'}}};
    REQUIRE(test.service->submit_task({OperationId{2},
                                       connected.generation,
                                       *connected.session_id,
                                       std::nullopt,
                                       {std::move(execution), 10U}}));
    REQUIRE(wait_until(
        [&] { return test.service->task_snapshot().task.sent_count >= 3U; }));
    const auto task = test.service->task_snapshot().task;
    REQUIRE(task.generation);
    CHECK(task.sent_count >= 3U);
    OperationId stop;
    REQUIRE(test.service->issue_operation(stop) == IdIncrementResult::Advanced);
    REQUIRE(test.service->request_stop_task({stop, task.generation}));
    bool stopped = false;
    std::vector<OperationId> operations;
    REQUIRE(wait_until([&] {
      for (const auto &completion : test.service->drain_completions()) {
        if (const auto *tx = std::get_if<TxCompletion>(&completion)) {
          CHECK((tx->outcome == OperationOutcome::Succeeded ||
                 tx->outcome == OperationOutcome::Cancelled));
          CHECK(std::find(operations.begin(), operations.end(),
                          tx->operation_id) == operations.end());
          operations.push_back(tx->operation_id);
        } else if (const auto *done =
                       std::get_if<TaskStopCompletion>(&completion)) {
          CHECK(done->operation_id == stop);
          stopped = true;
        } else {
          CHECK(std::get<TaskStartCompletion>(completion).outcome ==
                OperationOutcome::Succeeded);
        }
      }
      return stopped;
    }));
    std::array<std::byte, 256> output{};
    const auto amount =
        ::read(test.pty.master.get(), output.data(), output.size());
    REQUIRE(amount >= 3);
    CHECK(std::all_of(output.begin(), output.begin() + amount,
                      [](auto value) { return value == std::byte{'Q'}; }));
    std::this_thread::sleep_for(40ms);
    CHECK(::read(test.pty.master.get(), output.data(), output.size()) == -1);
    CHECK(errno == EAGAIN);
    CHECK(test.service->task_snapshot().task.state ==
          scheduler::SchedulerState::Idle);
    CHECK_FALSE(test.service->fatal_signal());
  }
  SECTION("an active connection exchanges bytes without idle spinning") {
    const auto connected = test.connected();
    const auto idle = test.service->wait_count();
    std::this_thread::sleep_for(50ms);
    CHECK(test.service->wait_count() - idle <= 2U);
    const std::vector<std::byte> inbound{std::byte{0x00}, std::byte{0x41},
                                         std::byte{0x80}, std::byte{0xff}};
    REQUIRE(::write(test.pty.master.get(), inbound.data(), inbound.size()) ==
            static_cast<ssize_t>(inbound.size()));
    std::vector<std::byte> received;
    REQUIRE(wait_until([&] {
      for (const auto &event : test.service->drain_data(16U)) {
        REQUIRE(event.kind == SerialDataKind::Rx);
        received.insert(received.end(), event.bytes.begin(), event.bytes.end());
      }
      return received.size() >= inbound.size();
    }));
    CHECK(received == inbound);
    const std::vector<std::byte> outbound{std::byte{0xfe}, std::byte{0x42},
                                          std::byte{0x00}, std::byte{0x7f}};
    REQUIRE(test.service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                                      *connected.session_id, std::nullopt},
                                     outbound}));
    std::array<std::byte, 4> peer_received{};
    std::size_t offset = 0U;
    REQUIRE(wait_until([&] {
      const auto amount =
          ::read(test.pty.master.get(), peer_received.data() + offset,
                 peer_received.size() - offset);
      if (amount > 0)
        offset += static_cast<std::size_t>(amount);
      return offset == peer_received.size();
    }));
    CHECK(std::equal(peer_received.begin(), peer_received.end(),
                     outbound.begin()));
    const auto tx = test.completions(1U);
    REQUIRE(std::holds_alternative<TxCompletion>(tx[0]));
    CHECK(std::get<TxCompletion>(tx[0]).outcome == OperationOutcome::Succeeded);
    bool hangup = false;
    SECTION("normal disconnect") {
      REQUIRE(test.service->request_disconnect(
          {OperationId{3}, ConnectionGeneration{1}, connected.session_id}));
      REQUIRE(std::holds_alternative<DisconnectCompletion>(
          test.completions(1U)[0]));
    }
    SECTION("peer hangup") {
      test.pty.master.reset();
      hangup = true;
    }
    REQUIRE(wait_until([&] {
      const auto state = test.service->connection_snapshot();
      return !state.connected && !state.disconnecting;
    }));
    const auto events = test.service->drain_data(16U);
    REQUIRE(
        std::any_of(events.begin(), events.end(), [hangup](const auto &event) {
          return event.kind == SerialDataKind::Cleanup &&
                 event.origin == SessionEventOrigin::Cleanup &&
                 (!hangup || event.error.has_value());
        }));
  }
}
