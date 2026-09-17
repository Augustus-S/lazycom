#include <catch2/catch_test_macros.hpp>
#include <lazycom/serial/backend.hpp>
#include <lazycom/serial/service.hpp>

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
using namespace lazycom::app;
using namespace lazycom::serial;

class UniqueFd final {
public:
  explicit UniqueFd(int fd) noexcept : fd_{fd} {}
  ~UniqueFd() { reset(); }
  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;
  int get() const noexcept { return fd_; }
  void reset() noexcept {
    if (fd_ >= 0)
      static_cast<void>(::close(fd_));
    fd_ = -1;
  }

private:
  int fd_;
};
bool wait_until(const auto &predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}
struct PtyOwner {
  PtyOwner() {
    REQUIRE(master.get() >= 0);
    REQUIRE(::grantpt(master.get()) == 0);
    REQUIRE(::unlockpt(master.get()) == 0);
    std::array<char, 256> path{};
    REQUIRE(::ptsname_r(master.get(), path.data(), path.size()) == 0);
    auto inspected = inspect_device_path(path.data());
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
    std::vector<SerialCompletion> result;
    REQUIRE(wait_until([&] {
      auto batch = service->drain_completions();
      result.insert(result.end(), std::make_move_iterator(batch.begin()),
                    std::make_move_iterator(batch.end()));
      return result.size() >= count;
    }));
    REQUIRE(result.size() == count);
    return result;
  }
  ConnectCompletion connected() {
    const auto results = completions(1U);
    REQUIRE(std::holds_alternative<ConnectCompletion>(results[0]));
    auto result = std::get<ConnectCompletion>(results[0]);
    REQUIRE(result.outcome == OperationOutcome::Succeeded);
    REQUIRE(result.session_id);
    return result;
  }
  UniqueFd master{::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)};
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
  SECTION("an active connection exchanges bytes without idle spinning") {
    const auto connected = test.connected();
    const auto idle = test.service->wait_count();
    std::this_thread::sleep_for(50ms);
    CHECK(test.service->wait_count() - idle <= 2U);
    const std::vector<std::byte> inbound{std::byte{0x00}, std::byte{0x41},
                                         std::byte{0x80}, std::byte{0xff}};
    REQUIRE(::write(test.master.get(), inbound.data(), inbound.size()) ==
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
          ::read(test.master.get(), peer_received.data() + offset,
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
      test.master.reset();
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
