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
#include <optional>
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
    std::unique_lock lock(mutex_);
    enumerate_entered_ = true;
    condition_.notify_all();
    condition_.wait(lock,
                    [this] { return !block_enumerate_ || release_enumerate_; });
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
    std::unique_lock lock(mutex_);
    ++write_calls_;
    condition_.notify_all();
    condition_.wait(lock, [this] { return !block_writes_ || release_writes_; });
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
    std::unique_lock lock(mutex_);
    close_entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this] { return !block_close_ || release_close_; });
    open_ = false;
    ++close_calls_;
    condition_.notify_all();
    return {};
  }

  void block_open() {
    std::lock_guard lock(mutex_);
    block_open_ = true;
  }

  void block_enumerate() {
    std::lock_guard lock(mutex_);
    block_enumerate_ = true;
  }

  [[nodiscard]] bool
  wait_for_enumerate_entry(const std::chrono::milliseconds limit) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, limit,
                               [this] { return enumerate_entered_; });
  }

  void release_enumerate() {
    std::lock_guard lock(mutex_);
    release_enumerate_ = true;
    condition_.notify_all();
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

  void block_writes() {
    std::lock_guard lock(mutex_);
    block_writes_ = true;
  }

  void release_writes() {
    std::lock_guard lock(mutex_);
    release_writes_ = true;
    condition_.notify_all();
  }

  void block_close() {
    std::lock_guard lock(mutex_);
    block_close_ = true;
  }

  [[nodiscard]] bool
  wait_for_close_entry(const std::chrono::milliseconds limit) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, limit, [this] { return close_entered_; });
  }

  void release_close() {
    std::lock_guard lock(mutex_);
    release_close_ = true;
    condition_.notify_all();
  }

  void resume_writes() {
    std::lock_guard lock(mutex_);
    stall_writes_ = false;
    stall_after_steps_ = false;
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

  void inject_rx(const std::span<const std::byte> bytes) const {
    const auto amount = ::write(peer_fd_, bytes.data(), bytes.size());
    if (amount != static_cast<ssize_t>(bytes.size())) {
      throw std::system_error(errno, std::generic_category(),
                              "fake RX injection failed");
    }
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
  bool block_enumerate_{};
  bool release_enumerate_{};
  bool enumerate_entered_{};
  bool open_{};
  std::optional<Error> open_error_;
  std::deque<std::ptrdiff_t> write_steps_;
  bool block_writes_{};
  bool release_writes_{};
  bool block_close_{};
  bool release_close_{};
  bool close_entered_{};
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

[[nodiscard]] std::size_t retained_event_bytes(const SerialDataEvent &event) {
  std::size_t result = sizeof(SerialDataEvent) + event.bytes.capacity();
  if (event.error) {
    result += event.error->detail.capacity();
  }
  return result;
}

[[nodiscard]] std::size_t
retained_event_bytes(const std::vector<SerialDataEvent> &events) {
  std::size_t result = 0U;
  for (const auto &event : events) {
    result += retained_event_bytes(event);
  }
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

TEST_CASE("stop during open rejects new work and settles the accepted connect",
          "[serial][owner]") {
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend);
  backend->block_open();
  REQUIRE(service->submit_connect(
      {{OperationId{1}, ConnectionGeneration{1}}, fake_path(), {}}));
  REQUIRE(backend->wait_for_open_entry(1s));

  service->request_stop();
  REQUIRE_FALSE(service->submit_connect(
      {{OperationId{2}, ConnectionGeneration{2}}, fake_path(), {}}));
  REQUIRE_FALSE(service->request_cancel_connect(
      {OperationId{3}, ConnectionGeneration{1}}));
  backend->release_open();
  REQUIRE(service->wait_until_stopped(std::chrono::steady_clock::now() + 1s));

  const auto completions = service->drain_completions();
  REQUIRE(completions.size() == 1U);
  REQUIRE(std::holds_alternative<ConnectCompletion>(completions.front()));
  REQUIRE(std::get<ConnectCompletion>(completions.front()).outcome ==
          OperationOutcome::Cancelled);
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

TEST_CASE("scanner stop settles an active request and cannot lose its wakeup",
          "[serial][scanner]") {
  auto backend = std::make_unique<FakeSerialBackend>();
  auto *const fake = backend.get();
  fake->block_enumerate();
  auto scanner_result = DeviceScanner::create(std::move(backend));
  REQUIRE(scanner_result);
  auto scanner = std::move(*scanner_result);
  REQUIRE(scanner->submit_scan(OperationId{1}, ScanGeneration{1}));
  REQUIRE(fake->wait_for_enumerate_entry(1s));

  scanner->request_stop();
  REQUIRE_FALSE(scanner->submit_scan(OperationId{2}, ScanGeneration{2}));
  fake->release_enumerate();
  REQUIRE(scanner->wait_until_stopped(std::chrono::steady_clock::now() + 1s));
  const auto completions = scanner->drain_completions();
  REQUIRE(completions.size() == 1U);
  REQUIRE(completions.front().operation_id == OperationId{1});
  REQUIRE(completions.front().outcome == OperationOutcome::Cancelled);
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

TEST_CASE("disconnect wake prevents writes after the current backend call",
          "[serial][owner]") {
  SerialServiceOptions options;
  options.tx_max_messages = 1U;
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend, options);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);
  backend->set_write_steps({1, 64});
  backend->block_writes();

  REQUIRE(service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              {std::byte{1}, std::byte{2}, std::byte{3}}}));
  REQUIRE(backend->wait_for_write_calls(1U, 1s));
  REQUIRE(service->request_disconnect(
      {OperationId{3}, ConnectionGeneration{1}, connected.session_id}));
  backend->release_writes();

  const auto completions = wait_completions(*service, 2U);
  REQUIRE(completions.size() == 2U);
  REQUIRE(std::holds_alternative<TxCompletion>(completions[0]));
  REQUIRE(std::get<TxCompletion>(completions[0]).outcome ==
          OperationOutcome::Cancelled);
  REQUIRE(std::get<TxCompletion>(completions[0]).accepted_bytes == 1U);
  REQUIRE(std::holds_alternative<DisconnectCompletion>(completions[1]));
  REQUIRE(backend->written().size() == 1U);

  const auto queued_bytes = service->queued_data_bytes();
  const auto events = service->drain_data(8U);
  REQUIRE(events.size() == 3U);
  REQUIRE(events[0].kind == SerialDataKind::Tx);
  REQUIRE(events[1].kind == SerialDataKind::Error);
  REQUIRE(events[2].kind == SerialDataKind::Cleanup);
  REQUIRE(events[0].owner_order < events[1].owner_order);
  REQUIRE(events[1].owner_order < events[2].owner_order);
  REQUIRE(queued_bytes == retained_event_bytes(events));
  REQUIRE(service->queued_data_bytes() == 0U);
}

TEST_CASE("successful TX events survive saturated data ingress",
          "[serial][owner]") {
  SerialServiceOptions options;
  options.command_max_messages = 1U;
  options.tx_max_messages = 2U;
  options.rx_max_chunks = 1U;
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend, options);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);

  const std::vector<std::byte> rx{std::byte{'R'}};
  backend->inject_rx(rx);
  REQUIRE(wait_until([&] { return service->queued_data_bytes() != 0U; }));
  const auto saturated_bytes = service->queued_data_bytes();

  const std::vector<std::byte> first{std::byte{'A'}};
  REQUIRE(service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              first}));
  const auto first_completion = wait_completions(*service, 1U);
  REQUIRE(first_completion.size() == 1U);
  REQUIRE(std::get<TxCompletion>(first_completion.front()).outcome ==
          OperationOutcome::Succeeded);
  REQUIRE(wait_until([&] {
    return service->queued_data_bytes() > saturated_bytes ||
           !service->connection_snapshot().connected;
  }));
  REQUIRE(service->connection_snapshot().connected);
  REQUIRE_FALSE(service->overflow_signal());

  const std::vector<std::byte> second{std::byte{'B'}};
  REQUIRE(service->submit_tx({{OperationId{3}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              second}));
  const auto second_completion = wait_completions(*service, 1U);
  REQUIRE(second_completion.size() == 1U);
  REQUIRE(std::get<TxCompletion>(second_completion.front()).outcome ==
          OperationOutcome::Succeeded);

  const auto events = service->drain_data(8U);
  REQUIRE(events.size() == 3U);
  REQUIRE(events[0].kind == SerialDataKind::Rx);
  REQUIRE(events[0].bytes == rx);
  REQUIRE(events[1].kind == SerialDataKind::Tx);
  REQUIRE(events[1].operation_id == OperationId{2});
  REQUIRE(events[1].bytes == first);
  REQUIRE(events[2].kind == SerialDataKind::Tx);
  REQUIRE(events[2].operation_id == OperationId{3});
  REQUIRE(events[2].bytes == second);
  REQUIRE(events[0].owner_order < events[1].owner_order);
  REQUIRE(events[1].owner_order < events[2].owner_order);
  REQUIRE_FALSE(service->fatal_signal());
}

TEST_CASE("completed TX retains admission until terminal data is drained",
          "[serial][owner]") {
  SerialServiceOptions options;
  options.command_max_messages = 1U;
  options.tx_max_messages = 1U;
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend, options);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);

  REQUIRE(service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              {std::byte{'A'}}}));
  const auto completion = wait_completions(*service, 1U);
  REQUIRE(completion.size() == 1U);
  REQUIRE(std::get<TxCompletion>(completion.front()).outcome ==
          OperationOutcome::Succeeded);

  // The configured reserved pool has five slots. More attempts than that must
  // remain bounded by the one retained TX admission rather than fill the pool.
  for (std::uint64_t operation = 3U; operation <= 8U; ++operation) {
    const auto rejected =
        service->submit_tx({{OperationId{operation}, ConnectionGeneration{1},
                             *connected.session_id, std::nullopt},
                            {std::byte{'B'}}});
    REQUIRE_FALSE(rejected);
    REQUIRE(rejected.error().detail == "serial TX queue is full");
  }
  REQUIRE_FALSE(service->fatal_signal());
  REQUIRE(service->connection_snapshot().connected);

  const auto retained = service->drain_data(8U);
  REQUIRE(retained.size() == 1U);
  REQUIRE(retained.front().kind == SerialDataKind::Tx);
  REQUIRE(service->queued_data_bytes() == 0U);

  REQUIRE(service->submit_tx({{OperationId{9}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              {std::byte{'C'}}}));
  REQUIRE(wait_completions(*service, 1U).size() == 1U);
  REQUIRE(service->drain_data(8U).size() == 1U);
  REQUIRE_FALSE(service->fatal_signal());
}

TEST_CASE("cleanup admission remains reserved until cleanup is drained",
          "[serial][owner]") {
  SerialServiceOptions options;
  options.command_max_messages = 1U;
  options.tx_max_messages = 1U;
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend, options);

  for (std::uint64_t generation = 1U; generation <= 3U; ++generation) {
    const auto operation = generation * 2U - 1U;
    const auto connected = connect(*service, operation, generation);
    REQUIRE(connected.session_id);
    REQUIRE(service->request_disconnect({OperationId{operation + 1U},
                                         ConnectionGeneration{generation},
                                         connected.session_id}));
    const auto completion = wait_completions(*service, 1U);
    REQUIRE(completion.size() == 1U);
    REQUIRE(std::holds_alternative<DisconnectCompletion>(completion.front()));
  }

  const auto rejected = service->submit_connect(
      {{OperationId{7}, ConnectionGeneration{4}}, fake_path(), {}});
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().detail == "serial cleanup capacity is full");
  REQUIRE_FALSE(service->fatal_signal());

  const auto cleanup = service->drain_data(8U);
  REQUIRE(cleanup.size() == 3U);
  REQUIRE(std::all_of(cleanup.begin(), cleanup.end(), [](const auto &event) {
    return event.kind == SerialDataKind::Cleanup;
  }));
  REQUIRE(cleanup[0].owner_order < cleanup[1].owner_order);
  REQUIRE(cleanup[1].owner_order < cleanup[2].owner_order);
  REQUIRE(service->queued_data_bytes() == 0U);

  const auto connected = connect(*service, 8U, 4U);
  REQUIRE(connected.session_id);
  REQUIRE_FALSE(service->fatal_signal());
}

TEST_CASE("retained TX bytes gate admission without consuming RX capacity",
          "[serial][owner]") {
  SerialServiceOptions options;
  options.tx_max_messages = 8U;
  options.tx_max_bytes = 1024U * 1024U;
  options.rx_max_chunks = 1U;
  options.rx_max_bytes = 4096U;
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend, options);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);

  const std::vector<std::byte> payload(700U * 1024U, std::byte{'T'});
  REQUIRE(service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              payload}));
  REQUIRE(wait_completions(*service, 1U).size() == 1U);

  const auto rejected =
      service->submit_tx({{OperationId{3}, ConnectionGeneration{1},
                           *connected.session_id, std::nullopt},
                          payload});
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().detail == "serial TX queue is full");

  const auto before_rx = service->queued_data_bytes();
  REQUIRE(before_rx > options.rx_max_bytes);
  const std::vector<std::byte> rx{std::byte{'R'}};
  backend->inject_rx(rx);
  REQUIRE(wait_until([&] { return service->queued_data_bytes() > before_rx; }));
  REQUIRE(service->connection_snapshot().connected);
  REQUIRE_FALSE(service->overflow_signal());

  const auto retained = service->drain_data(8U);
  REQUIRE(retained.size() == 2U);
  REQUIRE(retained[0].kind == SerialDataKind::Tx);
  REQUIRE(retained[1].kind == SerialDataKind::Rx);
  REQUIRE(retained_event_bytes(retained) ==
          before_rx + retained_event_bytes(retained[1]));
  REQUIRE(service->queued_data_bytes() == 0U);

  REQUIRE(service->submit_tx({{OperationId{3}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              payload}));
  REQUIRE(wait_completions(*service, 1U).size() == 1U);
  REQUIRE(service->drain_data(8U).size() == 1U);
}

TEST_CASE("TX timeout activates the next queued request with a fresh deadline",
          "[serial][owner]") {
  SerialServiceOptions options;
  options.tx_timeout = 100ms;
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend, options);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);
  backend->write_prefix_then_stall(1);

  REQUIRE(service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              {std::byte{1}, std::byte{2}}}));
  REQUIRE(service->submit_tx({{OperationId{3}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              {std::byte{2}}}));
  const auto first = wait_completions(*service, 1U);
  REQUIRE(first.size() == 1U);
  REQUIRE(std::get<TxCompletion>(first.front()).operation_id == OperationId{2});
  REQUIRE(std::get<TxCompletion>(first.front()).outcome ==
          OperationOutcome::TimedOut);
  REQUIRE(std::get<TxCompletion>(first.front()).error);
  REQUIRE(std::get<TxCompletion>(first.front()).error->code ==
          ErrorCode::SerialOperationTimedOut);
  const auto timed_out_events = service->drain_data(8U);
  REQUIRE(timed_out_events.size() == 2U);
  REQUIRE(timed_out_events[0].kind == SerialDataKind::Tx);
  REQUIRE(timed_out_events[0].bytes.size() == 1U);
  REQUIRE(timed_out_events[1].kind == SerialDataKind::Error);
  REQUIRE(timed_out_events[1].error);
  REQUIRE(timed_out_events[1].error->code ==
          ErrorCode::SerialOperationTimedOut);
  REQUIRE(timed_out_events[0].owner_order < timed_out_events[1].owner_order);

  std::this_thread::sleep_for(60ms);
  backend->resume_writes();
  const auto second = wait_completions(*service, 1U);
  REQUIRE(second.size() == 1U);
  REQUIRE(std::get<TxCompletion>(second.front()).operation_id ==
          OperationId{3});
  REQUIRE(std::get<TxCompletion>(second.front()).outcome ==
          OperationOutcome::Succeeded);
}

TEST_CASE("zero-byte TX writes use bounded retry backoff", "[serial][owner]") {
  SerialServiceOptions options;
  options.tx_timeout = 500ms;
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend, options);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);
  backend->stall_writes();
  REQUIRE(service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              {std::byte{1}}}));
  REQUIRE(backend->wait_for_write_calls(1U, 1s));

  const auto before = service->wait_count();
  std::this_thread::sleep_for(50ms);
  const auto waits = service->wait_count() - before;
  REQUIRE(waits >= 3U);
  REQUIRE(waits <= 20U);
}

TEST_CASE("stop closes every producer and settles all accepted TX",
          "[serial][owner]") {
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);
  backend->stall_writes();
  for (std::uint64_t operation = 2U; operation <= 4U; ++operation) {
    REQUIRE(service->submit_tx(
        {{OperationId{operation}, ConnectionGeneration{1},
          *connected.session_id, std::nullopt},
         {std::byte{static_cast<unsigned char>(operation)}}}));
  }
  REQUIRE(backend->wait_for_write_calls(1U, 1s));
  const auto accepted_control =
      service->request_stop_task({OperationId{5}, TaskGeneration{9}});
  REQUIRE(accepted_control);
  REQUIRE(*accepted_control == SubmitStatus::Accepted);

  service->request_stop();
  REQUIRE_FALSE(service->submit_tx({{OperationId{6}, ConnectionGeneration{1},
                                     *connected.session_id, std::nullopt},
                                    {std::byte{5}}}));
  REQUIRE_FALSE(service->request_disconnect(
      {OperationId{7}, ConnectionGeneration{1}, connected.session_id}));
  REQUIRE_FALSE(
      service->request_stop_task({OperationId{8}, TaskGeneration{1}}));
  REQUIRE_FALSE(service->request_cancel_connect(
      {OperationId{9}, ConnectionGeneration{1}}));
  REQUIRE(service->wait_until_stopped(std::chrono::steady_clock::now() + 1s));

  const auto completions = wait_completions(*service, 4U);
  REQUIRE(completions.size() == 4U);
  REQUIRE(std::count_if(completions.begin(), completions.end(),
                        [](const SerialCompletion &completion) {
                          return std::holds_alternative<TxCompletion>(
                                     completion) &&
                                 std::get<TxCompletion>(completion).outcome ==
                                     OperationOutcome::Cancelled;
                        }) == 3);
  REQUIRE(std::count_if(completions.begin(), completions.end(),
                        [](const SerialCompletion &completion) {
                          return std::holds_alternative<TaskStopCompletion>(
                              completion);
                        }) == 1);
}

TEST_CASE("owner stop prevents writes after the current backend call",
          "[serial][owner]") {
  SerialServiceOptions options;
  options.tx_max_messages = 1U;
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend, options);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);
  backend->set_write_steps({1, 64});
  backend->block_writes();

  REQUIRE(service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              {std::byte{1}, std::byte{2}, std::byte{3}}}));
  REQUIRE(backend->wait_for_write_calls(1U, 1s));
  service->request_stop();
  backend->release_writes();
  REQUIRE(service->wait_until_stopped(std::chrono::steady_clock::now() + 1s));

  const auto completions = wait_completions(*service, 1U);
  REQUIRE(completions.size() == 1U);
  const auto &tx = std::get<TxCompletion>(completions.front());
  REQUIRE(tx.outcome == OperationOutcome::Cancelled);
  REQUIRE(tx.accepted_bytes == 1U);
  REQUIRE(backend->written().size() == 1U);
  const auto events = service->drain_data(8U);
  REQUIRE(events.size() == 3U);
  REQUIRE(events[0].kind == SerialDataKind::Tx);
  REQUIRE(events[1].kind == SerialDataKind::Error);
  REQUIRE(events[2].kind == SerialDataKind::Cleanup);
}

TEST_CASE("pending task stop rejects racing TX for the same generation",
          "[serial][owner][scheduler]") {
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);
  backend->stall_writes();
  backend->block_writes();

  REQUIRE(service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                               *connected.session_id, TaskGeneration{7}},
                              {std::byte{1}}}));
  REQUIRE(backend->wait_for_write_calls(1U, 1s));
  REQUIRE(service->request_stop_task({OperationId{3}, TaskGeneration{7}}));

  const auto raced =
      service->submit_tx({{OperationId{4}, ConnectionGeneration{1},
                           *connected.session_id, TaskGeneration{7}},
                          {std::byte{2}}});
  REQUIRE_FALSE(raced);
  REQUIRE(raced.error().detail == "serial task is stopping");

  backend->release_writes();
  const auto completions = wait_completions(*service, 2U);
  REQUIRE(completions.size() == 2U);
  REQUIRE(std::holds_alternative<TxCompletion>(completions[0]));
  REQUIRE(std::get<TxCompletion>(completions[0]).outcome ==
          OperationOutcome::Cancelled);
  REQUIRE(std::holds_alternative<TaskStopCompletion>(completions[1]));
  REQUIRE_FALSE(service->fatal_signal());
}

TEST_CASE("task stop cancels the matching partial write before confirmation",
          "[serial][owner][scheduler]") {
  SerialServiceOptions options;
  options.tx_max_messages = 1U;
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend, options);
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
  REQUIRE(std::get<TxCompletion>(completions[0]).error);
  REQUIRE(std::get<TxCompletion>(completions[0]).error->code ==
          ErrorCode::SerialOperationCancelled);
  REQUIRE(std::holds_alternative<TaskStopCompletion>(completions[1]));
  REQUIRE(std::get<TaskStopCompletion>(completions[1]).generation ==
          TaskGeneration{7});
  REQUIRE(backend->written().size() == 1U);
  const auto queued_bytes = service->queued_data_bytes();
  const auto prefix = service->drain_data(1U);
  REQUIRE(prefix.size() == 1U);
  REQUIRE(prefix[0].kind == SerialDataKind::Tx);
  const auto rejected =
      service->submit_tx({{OperationId{4}, ConnectionGeneration{1},
                           *connected.session_id, std::nullopt},
                          {std::byte{4}}});
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().detail == "serial TX queue is full");

  const auto terminal = service->drain_data(1U);
  REQUIRE(terminal.size() == 1U);
  REQUIRE(terminal[0].kind == SerialDataKind::Error);
  REQUIRE(terminal[0].error);
  REQUIRE(terminal[0].error->code == ErrorCode::SerialOperationCancelled);
  REQUIRE(prefix[0].owner_order < terminal[0].owner_order);
  REQUIRE(queued_bytes ==
          retained_event_bytes(prefix) + retained_event_bytes(terminal));
  REQUIRE(service->queued_data_bytes() == 0U);

  backend->resume_writes();
  REQUIRE(service->submit_tx({{OperationId{5}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              {std::byte{5}}}));
  REQUIRE(wait_completions(*service, 1U).size() == 1U);
  REQUIRE(service->drain_data(1U).size() == 1U);
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
  REQUIRE(wait_until([&] {
    const auto snapshot = service->connection_snapshot();
    return !snapshot.connected && !snapshot.disconnecting;
  }));
  const auto queued_bytes = service->queued_data_bytes();
  const auto data = service->drain_data(8U);
  REQUIRE(
      std::any_of(data.begin(), data.end(), [](const SerialDataEvent &event) {
        return event.kind == SerialDataKind::Cleanup && event.error.has_value();
      }));
  REQUIRE(queued_bytes == retained_event_bytes(data));
  REQUIRE(service->queued_data_bytes() == 0U);
}

TEST_CASE("fault transition closes TX admission before backend close",
          "[serial][owner]") {
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);
  backend->set_write_steps({-1});
  backend->block_writes();
  REQUIRE(service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              {std::byte{1}}}));
  REQUIRE(backend->wait_for_write_calls(1U, 1s));

  backend->block_close();
  backend->release_writes();
  const bool close_entered = backend->wait_for_close_entry(1s);
  const auto raced =
      service->submit_tx({{OperationId{3}, ConnectionGeneration{1},
                           *connected.session_id, std::nullopt},
                          {std::byte{2}}});
  backend->release_close();

  REQUIRE(close_entered);
  REQUIRE_FALSE(raced);
  REQUIRE(raced.error().detail == "stale or disconnected TX session");
  REQUIRE(wait_until([&] {
    const auto snapshot = service->connection_snapshot();
    return !snapshot.connected && !snapshot.disconnecting;
  }));
  const auto completions = wait_completions(*service, 1U);
  REQUIRE(completions.size() == 1U);
  REQUIRE(std::get<TxCompletion>(completions.front()).operation_id ==
          OperationId{2});
  REQUIRE(std::get<TxCompletion>(completions.front()).outcome ==
          OperationOutcome::Failed);
  const auto events = service->drain_data(8U);
  REQUIRE(events.size() == 2U);
  REQUIRE(events[0].kind == SerialDataKind::Error);
  REQUIRE(events[1].kind == SerialDataKind::Cleanup);
  REQUIRE(events[0].owner_order < events[1].owner_order);
  REQUIRE_FALSE(service->fatal_signal());
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
  REQUIRE(wait_until([&] {
    const auto snapshot = service->connection_snapshot();
    return !snapshot.connected && !snapshot.disconnecting;
  }));
  const auto events = service->drain_data(8U);
  REQUIRE(events.size() == 3U);
  REQUIRE(events.front().kind == SerialDataKind::Tx);
  REQUIRE(events.front().bytes.size() == 2U);
  REQUIRE(events[1].kind == SerialDataKind::Error);
  REQUIRE(events[1].error);
  REQUIRE(events[1].error->code == ErrorCode::SerialDeviceGone);
  REQUIRE(events[1].error->operation_id == OperationId{2});
  REQUIRE(events.back().kind == SerialDataKind::Cleanup);
  REQUIRE(events.front().owner_order < events.back().owner_order);
  REQUIRE(events[1].owner_order < events.back().owner_order);
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

TEST_CASE("manual TX wins at the next complete request boundary",
          "[serial][owner][scheduler]") {
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);
  backend->set_write_steps({1, 64, 64, 64});
  backend->block_writes();

  REQUIRE(service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              {std::byte{'A'}, std::byte{'a'}}}));
  const bool started = backend->wait_for_write_calls(1U, 1s);
  if (!started) {
    backend->release_writes();
  }
  REQUIRE(started);
  const auto scheduled =
      service->submit_tx({{OperationId{3}, ConnectionGeneration{1},
                           *connected.session_id, TaskGeneration{7}},
                          {std::byte{'Q'}, std::byte{'q'}}});
  const auto manual =
      service->submit_tx({{OperationId{4}, ConnectionGeneration{1},
                           *connected.session_id, std::nullopt},
                          {std::byte{'B'}, std::byte{'b'}}});

  backend->release_writes();
  REQUIRE(scheduled);
  REQUIRE(manual);
  const auto completions = wait_completions(*service, 3U);
  REQUIRE(completions.size() == 3U);
  CHECK(std::get<TxCompletion>(completions[0]).operation_id == OperationId{2});
  CHECK(std::get<TxCompletion>(completions[1]).operation_id == OperationId{4});
  CHECK(std::get<TxCompletion>(completions[2]).operation_id == OperationId{3});
  const std::vector<std::byte> expected{std::byte{'A'}, std::byte{'a'},
                                        std::byte{'B'}, std::byte{'b'},
                                        std::byte{'Q'}, std::byte{'q'}};
  CHECK(backend->written() == expected);
  CHECK_FALSE(service->fatal_signal());
}

TEST_CASE("TX expiry stops further writes after a late positive partial write",
          "[serial][owner]") {
  SerialServiceOptions options;
  options.tx_timeout = 100ms;
  FakeSerialBackend *backend = nullptr;
  auto service = make_service(backend, options);
  const auto connected = connect(*service);
  REQUIRE(connected.session_id);
  backend->set_write_steps({1, 64});
  backend->block_writes();

  REQUIRE(service->submit_tx({{OperationId{2}, ConnectionGeneration{1},
                               *connected.session_id, std::nullopt},
                              {std::byte{1}, std::byte{2}, std::byte{3}}}));
  const bool started = backend->wait_for_write_calls(1U, 1s);
  if (started) {
    std::this_thread::sleep_for(150ms);
  }
  backend->release_writes();
  REQUIRE(started);

  const auto completions = wait_completions(*service, 1U);
  REQUIRE(completions.size() == 1U);
  const auto &completion = std::get<TxCompletion>(completions.front());
  CHECK(completion.outcome == OperationOutcome::TimedOut);
  CHECK(completion.accepted_bytes == 1U);
  REQUIRE(completion.error);
  CHECK(completion.error->code == ErrorCode::SerialOperationTimedOut);
  CHECK(backend->written() == std::vector<std::byte>{std::byte{1}});

  const auto events = service->drain_data(8U);
  REQUIRE(events.size() == 2U);
  CHECK(events[0].kind == SerialDataKind::Tx);
  CHECK(events[0].bytes == std::vector<std::byte>{std::byte{1}});
  CHECK(events[1].kind == SerialDataKind::Error);
  REQUIRE(events[1].error);
  CHECK(events[1].error->code == ErrorCode::SerialOperationTimedOut);
  CHECK(events[1].error->operation_id == OperationId{2});
  CHECK(events[0].owner_order < events[1].owner_order);
  CHECK_FALSE(service->fatal_signal());
}
