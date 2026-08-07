#include <lazycom/serial/backend.hpp>
#include <lazycom/serial/service.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using namespace lazycom;
using namespace lazycom::app;
using namespace lazycom::serial;

class UniqueFd final {
public:
  explicit UniqueFd(const int fd = -1) noexcept : fd_(fd) {}
  ~UniqueFd() { reset(); }
  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;
  UniqueFd(UniqueFd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.fd_, -1));
    }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return fd_; }
  void reset(const int fd = -1) noexcept {
    if (fd_ >= 0) {
      static_cast<void>(::close(fd_));
    }
    fd_ = fd;
  }

private:
  int fd_;
};

struct Pty {
  UniqueFd master;
  DevicePath slave;
};

[[nodiscard]] Pty make_pty() {
  UniqueFd master{::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)};
  if (master.get() < 0 || ::grantpt(master.get()) != 0 ||
      ::unlockpt(master.get()) != 0) {
    throw std::runtime_error("cannot create PTY");
  }
  std::array<char, 256> path{};
  if (::ptsname_r(master.get(), path.data(), path.size()) != 0) {
    throw std::runtime_error("cannot obtain PTY slave path");
  }
  auto inspected = inspect_device_path(path.data());
  if (!inspected) {
    throw std::runtime_error(inspected.error().detail);
  }
  return {std::move(master), std::move(*inspected)};
}

[[nodiscard]] std::unique_ptr<SerialService> make_service() {
  auto created =
      SerialService::create(std::make_unique<LibserialportBackend>());
  if (!created) {
    throw std::runtime_error(created.error().detail);
  }
  return std::move(*created);
}

[[nodiscard]] bool wait_until(const auto &predicate,
                              const std::chrono::milliseconds timeout = 2s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

[[nodiscard]] std::optional<ConnectCompletion>
wait_connect(SerialService &service) {
  std::optional<ConnectCompletion> result;
  static_cast<void>(wait_until([&] {
    for (auto &completion : service.drain_completions()) {
      if (auto *const connected = std::get_if<ConnectCompletion>(&completion)) {
        result = std::move(*connected);
      }
    }
    return result.has_value();
  }));
  return result;
}

} // namespace

TEST_CASE("real PTY owner exchanges bytes and remains idle without spinning",
          "[integration][serial][pty]") {
  auto pty = make_pty();
  auto service = make_service();
  REQUIRE(service->submit_connect(
      {{OperationId{1}, ConnectionGeneration{1}}, pty.slave, {}}));
  const auto connected = wait_connect(*service);
  REQUIRE(connected);
  REQUIRE(connected->outcome == OperationOutcome::Succeeded);
  REQUIRE(connected->session_id);

  const auto before_idle = service->wait_count();
  std::this_thread::sleep_for(50ms);
  const auto after_idle = service->wait_count();
  REQUIRE(after_idle - before_idle <= 2U);

  constexpr std::array<std::byte, 4> inbound{std::byte{0x00}, std::byte{0x41},
                                             std::byte{0x80}, std::byte{0xff}};
  REQUIRE(::write(pty.master.get(), inbound.data(), inbound.size()) ==
          static_cast<ssize_t>(inbound.size()));
  std::optional<SerialDataEvent> received;
  REQUIRE(wait_until([&] {
    for (auto &event : service->drain_data(16U)) {
      if (event.kind == SerialDataKind::Rx) {
        received = std::move(event);
      }
    }
    return received.has_value();
  }));
  REQUIRE(received->bytes ==
          std::vector<std::byte>(inbound.begin(), inbound.end()));

  const std::vector<std::byte> outbound{std::byte{0xfe}, std::byte{0x42},
                                        std::byte{0x00}, std::byte{0x7f}};
  REQUIRE(service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                               *connected->session_id, std::nullopt},
                              outbound}));
  std::array<std::byte, 4> peer_received{};
  REQUIRE(wait_until([&] {
    const auto amount =
        ::read(pty.master.get(), peer_received.data(), peer_received.size());
    return amount == static_cast<ssize_t>(peer_received.size());
  }));
  REQUIRE(
      std::equal(peer_received.begin(), peer_received.end(), outbound.begin()));

  REQUIRE(service->request_disconnect(
      {OperationId{3}, ConnectionGeneration{1}, connected->session_id}));
  bool disconnected = false;
  REQUIRE(wait_until([&] {
    for (const auto &completion : service->drain_completions()) {
      disconnected = disconnected ||
                     std::holds_alternative<DisconnectCompletion>(completion);
    }
    return disconnected;
  }));
  const auto cleanup = service->drain_data(16U);
  REQUIRE(std::any_of(cleanup.begin(), cleanup.end(),
                      [](const SerialDataEvent &event) {
                        return event.kind == SerialDataKind::Cleanup &&
                               event.origin == SessionEventOrigin::Cleanup;
                      }));
}

TEST_CASE("real PTY queued connection can be cancelled",
          "[integration][serial][pty]") {
  auto pty = make_pty();
  auto service = make_service();
  REQUIRE(service->submit_connect(
      {{OperationId{10}, ConnectionGeneration{10}}, pty.slave, {}}));
  const auto cancellation = service->request_cancel_connect(
      {OperationId{11}, ConnectionGeneration{10}});
  if (!cancellation) {
    const auto connected = wait_connect(*service);
    REQUIRE(connected);
    REQUIRE(connected->session_id);
    REQUIRE(service->request_disconnect(
        {OperationId{12}, ConnectionGeneration{10}, connected->session_id}));
  } else {
    std::vector<SerialCompletion> completions;
    REQUIRE(wait_until([&] {
      auto batch = service->drain_completions();
      completions.insert(completions.end(),
                         std::make_move_iterator(batch.begin()),
                         std::make_move_iterator(batch.end()));
      return completions.size() >= 2U;
    }));
    REQUIRE(std::any_of(completions.begin(), completions.end(),
                        [](const SerialCompletion &value) {
                          return std::holds_alternative<ConnectCompletion>(
                                     value) &&
                                 std::get<ConnectCompletion>(value).outcome ==
                                     OperationOutcome::Cancelled;
                        }));
  }
}

TEST_CASE("real PTY hangup closes the owner session",
          "[integration][serial][pty]") {
  auto pty = make_pty();
  auto service = make_service();
  REQUIRE(service->submit_connect(
      {{OperationId{20}, ConnectionGeneration{20}}, pty.slave, {}}));
  const auto connected = wait_connect(*service);
  REQUIRE(connected);
  REQUIRE(connected->session_id);

  pty.master.reset();
  REQUIRE(wait_until([&] {
    const auto snapshot = service->connection_snapshot();
    return !snapshot.connected && !snapshot.disconnecting;
  }));
  const auto events = service->drain_data(16U);
  REQUIRE(std::any_of(
      events.begin(), events.end(), [](const SerialDataEvent &event) {
        return event.kind == SerialDataKind::Cleanup && event.error.has_value();
      }));
}
