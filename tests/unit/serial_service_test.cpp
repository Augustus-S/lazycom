#include <lazycom/serial/scanner.hpp>
#include <lazycom/serial/service.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using namespace lazycom;
using namespace lazycom::app;
using namespace lazycom::serial;

class FakeSerialBackend final : public ISerialBackend {
public:
  FakeSerialBackend() {
    int descriptors[2]{-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                     descriptors) != 0) {
      throw std::system_error(errno, std::generic_category(), "socketpair");
    }
    owner_fd_ = descriptors[0];
    peer_fd_ = descriptors[1];
  }

  ~FakeSerialBackend() override {
    if (owner_fd_ >= 0) {
      static_cast<void>(::close(owner_fd_));
    }
    if (peer_fd_ >= 0) {
      static_cast<void>(::close(peer_fd_));
    }
  }

  Result<std::vector<DeviceInfo>> enumerate() override {
    DeviceInfo device;
    device.path = "/dev/fake";
    device.description = "fake serial device";
    device.transport = DeviceTransport::Native;
    return std::vector<DeviceInfo>{std::move(device)};
  }

  Status open(const DevicePath &, const PortConfig &) override {
    std::unique_lock lock(mutex_);
    open_entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this] { return !block_open_ || release_open_; });
    if (open_error_) {
      return tl::unexpected(*open_error_);
    }
    open_ = true;
    return {};
  }

  Result<int> native_wait_handle() const override { return owner_fd_; }

  Result<std::size_t> read_some(std::span<std::byte> destination) override {
    const auto amount =
        ::read(owner_fd_, destination.data(), destination.size());
    if (amount >= 0) {
      return static_cast<std::size_t>(amount);
    }
    if (errno == EAGAIN) {
      return 0U;
    }
    return tl::unexpected(make_error(
        ErrorCode::SerialDeviceGone, Operation::ReadSerial, "fake read failed",
        std::error_code{errno, std::generic_category()}));
  }

  Result<std::size_t>
  write_some(const std::span<const std::byte> source) override {
    std::lock_guard lock(mutex_);
    ++write_calls_;
    condition_.notify_all();
    std::ptrdiff_t step = static_cast<std::ptrdiff_t>(source.size());
    if (stall_writes_) {
      step = 0;
    } else if (!write_steps_.empty()) {
      step = write_steps_.front();
      write_steps_.pop_front();
      if (write_steps_.empty() && stall_after_steps_) {
        stall_writes_ = true;
      }
    }
    if (step < 0) {
      return tl::unexpected(make_error(ErrorCode::SerialDeviceGone,
                                       Operation::WriteSerial,
                                       "injected device disappearance"));
    }
    const auto requested = static_cast<std::size_t>(step);
    const auto amount = std::min(requested, source.size());
    written_.insert(written_.end(), source.begin(),
                    source.begin() + static_cast<std::ptrdiff_t>(amount));
    return amount;
  }

  Status close() override {
    std::lock_guard lock(mutex_);
    open_ = false;
    ++close_calls_;
    condition_.notify_all();
    return {};
  }

  void block_open() {
    std::lock_guard lock(mutex_);
    block_open_ = true;
  }

  [[nodiscard]] bool
  wait_for_open_entry(const std::chrono::milliseconds limit) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, limit, [this] { return open_entered_; });
  }

  void release_open() {
    std::lock_guard lock(mutex_);
    release_open_ = true;
    condition_.notify_all();
  }

  void set_write_steps(std::initializer_list<std::ptrdiff_t> steps) {
    std::lock_guard lock(mutex_);
    write_steps_.assign(steps);
  }

  void stall_writes() {
    std::lock_guard lock(mutex_);
    stall_writes_ = true;
  }

  void write_prefix_then_stall(const std::ptrdiff_t prefix) {
    std::lock_guard lock(mutex_);
    write_steps_ = {prefix};
    stall_after_steps_ = true;
  }

  [[nodiscard]] bool
  wait_for_write_calls(const std::size_t count,
                       const std::chrono::milliseconds limit) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, limit,
                               [this, count] { return write_calls_ >= count; });
  }

  [[nodiscard]] std::vector<std::byte> written() const {
    std::lock_guard lock(mutex_);
    return written_;
  }

  void disappear() {
    std::lock_guard lock(mutex_);
    if (peer_fd_ >= 0) {
      static_cast<void>(::close(std::exchange(peer_fd_, -1)));
    }
  }

private:
  int owner_fd_{-1};
  int peer_fd_{-1};
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  bool block_open_{};
  bool release_open_{};
  bool open_entered_{};
  bool open_{};
  std::optional<Error> open_error_;
  std::deque<std::ptrdiff_t> write_steps_;
  bool stall_writes_{};
  bool stall_after_steps_{};
  std::vector<std::byte> written_;
  std::size_t write_calls_{};
  std::size_t close_calls_{};
};

[[nodiscard]] DevicePath fake_path() {
  return {.requested = "/dev/fake",
          .canonical = "/dev/fake",
          .identity = {.device = 1U, .inode = 2U, .special_device = 3U}};
}

[[nodiscard]] std::unique_ptr<SerialService>
make_service(FakeSerialBackend *&backend,
             const SerialServiceOptions &options = {}) {
  auto fake = std::make_unique<FakeSerialBackend>();
  backend = fake.get();
  auto created = SerialService::create(std::move(fake), options);
  if (!created) {
    throw std::runtime_error(created.error().detail);
  }
  return std::move(*created);
}

[[nodiscard]] bool wait_until(const auto &predicate,
                              const std::chrono::milliseconds timeout = 1s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(1ms);
  }
  return predicate();
}

[[nodiscard]] std::vector<SerialCompletion>
wait_completions(SerialService &service, const std::size_t count) {
  std::vector<SerialCompletion> result;
  static_cast<void>(wait_until([&] {
    auto current = service.drain_completions();
    result.insert(result.end(), std::make_move_iterator(current.begin()),
                  std::make_move_iterator(current.end()));
    return result.size() >= count;
  }));
  return result;
}

[[nodiscard]] ConnectCompletion connect(SerialService &service,
                                        const std::uint64_t operation = 1U,
                                        const std::uint64_t generation = 1U) {
  auto accepted = service.submit_connect(
      {{OperationId{operation}, ConnectionGeneration{generation}},
       fake_path(),
       {}});
  if (!accepted) {
    throw std::runtime_error(accepted.error().detail);
  }
  const auto completions = wait_completions(service, 1U);
  if (completions.empty() ||
      !std::holds_alternative<ConnectCompletion>(completions.front())) {
    throw std::runtime_error("connect completion was not published");
  }
  return std::get<ConnectCompletion>(completions.front());
}

} // namespace

TEST_CASE("normal queue saturation cannot block connect cancellation",
          "[serial][owner]") {
  SerialServiceOptions options;
  options.command_max_messages = 1U;
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend, options);
  backend->block_open();

  REQUIRE(service->submit_connect(
      {{OperationId{1}, ConnectionGeneration{1}}, fake_path(), {}}));
  REQUIRE(backend->wait_for_open_entry(1s));
  const auto rejected = service->submit_connect(
      {{OperationId{2}, ConnectionGeneration{2}}, fake_path(), {}});
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().detail == "serial command queue is full");

  REQUIRE(service->request_cancel_connect(
      {OperationId{3}, ConnectionGeneration{1}}));
  backend->release_open();
  const auto completions = wait_completions(*service, 2U);
  REQUIRE(completions.size() == 2U);
  const auto connect_result =
      std::find_if(completions.begin(), completions.end(),
                   [](const SerialCompletion &value) {
                     return std::holds_alternative<ConnectCompletion>(value);
                   });
  REQUIRE(connect_result != completions.end());
  REQUIRE(std::get<ConnectCompletion>(*connect_result).outcome ==
          OperationOutcome::Cancelled);
  REQUIRE(std::any_of(completions.begin(), completions.end(),
                      [](const SerialCompletion &value) {
                        return std::holds_alternative<DisconnectCompletion>(
                            value);
                      }));
}

TEST_CASE("persistent scanner keeps value results and one bounded pending scan",
          "[serial][scanner]") {
  auto backend = std::make_unique<FakeSerialBackend>();
  auto scanner_result = DeviceScanner::create(std::move(backend));
  REQUIRE(scanner_result);
  auto scanner = std::move(*scanner_result);
  REQUIRE(scanner->submit_scan(OperationId{1}, ScanGeneration{1}));
  REQUIRE(scanner->submit_scan(OperationId{2}, ScanGeneration{2}));
  REQUIRE_FALSE(scanner->submit_scan(OperationId{3}, ScanGeneration{3}));

  std::vector<ScanCompletion> completions;
  REQUIRE(wait_until([&] {
    auto batch = scanner->drain_completions();
    completions.insert(completions.end(),
                       std::make_move_iterator(batch.begin()),
                       std::make_move_iterator(batch.end()));
    return completions.size() == 2U;
  }));
  REQUIRE(completions[0].generation == ScanGeneration{1});
  REQUIRE(completions[1].generation == ScanGeneration{2});
  REQUIRE(completions[0].devices.front().path == "/dev/fake");
  scanner->request_stop();
  REQUIRE(scanner->wait_until_stopped(std::chrono::steady_clock::now() + 1s));
  REQUIRE(scanner->worker_stopped_signal());
}

TEST_CASE("TX writes are partial nonblocking and never interleave requests",
          "[serial][owner]") {
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);
  backend->set_write_steps({2, 0, 1, 64, 1, 64});

  const std::vector<std::byte> first{std::byte{'a'}, std::byte{'b'},
                                     std::byte{'c'}, std::byte{'d'}};
  const std::vector<std::byte> second{std::byte{'X'}, std::byte{'Y'}};
  REQUIRE(service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              first}));
  REQUIRE(service->submit_tx({{OperationId{3}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              second}));
  const auto completions = wait_completions(*service, 2U);
  REQUIRE(completions.size() == 2U);
  REQUIRE(std::all_of(completions.begin(), completions.end(),
                      [](const SerialCompletion &value) {
                        return std::get<TxCompletion>(value).outcome ==
                               OperationOutcome::Succeeded;
                      }));
  auto expected = first;
  expected.insert(expected.end(), second.begin(), second.end());
  REQUIRE(backend->written() == expected);
}

TEST_CASE("TX saturation still permits reliable disconnect control",
          "[serial][owner]") {
  SerialServiceOptions options;
  options.tx_max_messages = 1U;
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend, options);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);
  backend->stall_writes();

  REQUIRE(service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              {std::byte{1}, std::byte{2}}}));
  REQUIRE(backend->wait_for_write_calls(1U, 1s));
  REQUIRE_FALSE(service->submit_tx({{OperationId{3}, ConnectionGeneration{1},
                                     *connected.session_id, std::nullopt},
                                    {std::byte{3}}}));
  REQUIRE(service->request_disconnect(
      {OperationId{4}, ConnectionGeneration{1}, connected.session_id}));

  const auto completions = wait_completions(*service, 2U);
  REQUIRE(completions.size() == 2U);
  REQUIRE(std::any_of(completions.begin(), completions.end(),
                      [](const SerialCompletion &value) {
                        return std::holds_alternative<TxCompletion>(value) &&
                               std::get<TxCompletion>(value).outcome ==
                                   OperationOutcome::Cancelled;
                      }));
  REQUIRE(std::any_of(completions.begin(), completions.end(),
                      [](const SerialCompletion &value) {
                        return std::holds_alternative<DisconnectCompletion>(
                            value);
                      }));
  const auto data = service->drain_data(8U);
  REQUIRE(
      std::any_of(data.begin(), data.end(), [](const SerialDataEvent &event) {
        return event.kind == SerialDataKind::Cleanup &&
               event.origin == SessionEventOrigin::Cleanup;
      }));
}

TEST_CASE("task stop cancels the matching partial write before confirmation",
          "[serial][owner][scheduler]") {
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);
  backend->write_prefix_then_stall(1);

  REQUIRE(service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                               *connected.session_id, TaskGeneration{7}},
                              {std::byte{1}, std::byte{2}, std::byte{3}}}));
  REQUIRE(backend->wait_for_write_calls(1U, 1s));
  REQUIRE(service->request_stop_task({OperationId{3}, TaskGeneration{7}}));

  const auto completions = wait_completions(*service, 2U);
  REQUIRE(completions.size() == 2U);
  REQUIRE(std::holds_alternative<TxCompletion>(completions[0]));
  REQUIRE(std::get<TxCompletion>(completions[0]).outcome ==
          OperationOutcome::Cancelled);
  REQUIRE(std::get<TxCompletion>(completions[0]).accepted_bytes == 1U);
  REQUIRE(std::holds_alternative<TaskStopCompletion>(completions[1]));
  REQUIRE(std::get<TaskStopCompletion>(completions[1]).generation ==
          TaskGeneration{7});
  REQUIRE(backend->written().size() == 1U);
}

TEST_CASE("late sessions are rejected and device loss performs cleanup",
          "[serial][owner]") {
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);

  REQUIRE_FALSE(service->submit_tx(
      {{OperationId{2}, ConnectionGeneration{1},
        SessionId{connected.session_id->value + 1U}, std::nullopt},
       {std::byte{1}}}));
  backend->disappear();
  REQUIRE(
      wait_until([&] { return !service->connection_snapshot().connected; }));
  const auto data = service->drain_data(8U);
  REQUIRE(
      std::any_of(data.begin(), data.end(), [](const SerialDataEvent &event) {
        return event.kind == SerialDataKind::Cleanup && event.error.has_value();
      }));
}

TEST_CASE("device loss after a partial write reports the accepted prefix",
          "[serial][owner]") {
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);
  backend->set_write_steps({2, -1});
  REQUIRE(service->submit_tx(
      {{OperationId{2}, ConnectionGeneration{1}, *connected.session_id,
        std::nullopt},
       {std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}}));

  const auto completions = wait_completions(*service, 1U);
  REQUIRE(completions.size() == 1U);
  const auto &tx = std::get<TxCompletion>(completions.front());
  REQUIRE(tx.outcome == OperationOutcome::Failed);
  REQUIRE(tx.accepted_bytes == 2U);
  REQUIRE(tx.error);
  REQUIRE(
      wait_until([&] { return !service->connection_snapshot().connected; }));
  const auto events = service->drain_data(8U);
  REQUIRE(events.size() >= 2U);
  REQUIRE(events.front().kind == SerialDataKind::Tx);
  REQUIRE(events.front().bytes.size() == 2U);
  REQUIRE(events.back().kind == SerialDataKind::Cleanup);
  REQUIRE(events.front().owner_order < events.back().owner_order);
}

TEST_CASE("owner stop wakes ppoll and publishes its fixed lifecycle slot",
          "[serial][owner]") {
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend);
  static_cast<void>(backend);
  std::this_thread::sleep_for(20ms);
  service->request_stop();
  REQUIRE(service->wait_until_stopped(std::chrono::steady_clock::now() + 1s));
  REQUIRE(service->worker_stopped_signal());
  REQUIRE(service->worker_stopped_signal()->worker == WorkerKind::Serial);
  REQUIRE(service->worker_stopped_signal()->lifecycle ==
          WorkerLifecycle::AtReturnPoint);
  REQUIRE_FALSE(service->fatal_signal());
}
