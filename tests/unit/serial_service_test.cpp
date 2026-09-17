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
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using namespace lazycom;
using namespace lazycom::app;
using namespace lazycom::serial;

class Gate {
public:
  void block() {
    std::lock_guard lock(mutex_);
    blocked_ = true;
  }
  void enter() {
    std::unique_lock lock(mutex_);
    entered_ = true;
    condition_.notify_all();
    condition_.wait(lock, [this] { return !blocked_; });
  }
  bool wait() {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, 1s, [this] { return entered_; });
  }
  void release() {
    std::lock_guard lock(mutex_);
    blocked_ = false;
    condition_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool blocked_{}, entered_{};
};

class FakeSerialBackend final : public ISerialBackend {
public:
  FakeSerialBackend() {
    int descriptors[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0,
                     descriptors) != 0) {
      throw std::system_error(errno, std::generic_category(), "socketpair");
    }
    owner_fd_ = descriptors[0];
    peer_fd_ = descriptors[1];
  }
  ~FakeSerialBackend() override {
    static_cast<void>(::close(owner_fd_));
    if (peer_fd_ >= 0)
      static_cast<void>(::close(peer_fd_));
  }
  Result<std::vector<DeviceInfo>> enumerate() override {
    enumerate_gate.enter();
    DeviceInfo device;
    device.path = "/dev/fake";
    device.description = "fake serial device";
    device.transport = DeviceTransport::Native;
    return std::vector<DeviceInfo>{std::move(device)};
  }
  Status open(const DevicePath &, const PortConfig &) override {
    open_gate.enter();
    return {};
  }
  Result<int> native_wait_handle() const override { return owner_fd_; }
  Result<std::size_t> read_some(std::span<std::byte> destination) override {
    const auto amount =
        ::read(owner_fd_, destination.data(), destination.size());
    if (amount >= 0)
      return static_cast<std::size_t>(amount);
    if (errno == EAGAIN)
      return 0U;
    return tl::unexpected(make_error(
        ErrorCode::SerialDeviceGone, Operation::ReadSerial, "fake read failed",
        std::error_code{errno, std::generic_category()}));
  }
  Result<std::size_t> write_some(std::span<const std::byte> source) override {
    write_gate.enter();
    std::lock_guard lock(mutex_);
    auto step = static_cast<std::ptrdiff_t>(source.size());
    if (stall_)
      step = 0;
    else if (!steps_.empty()) {
      step = steps_.front();
      steps_.pop_front();
      if (steps_.empty() && stall_after_steps_)
        stall_ = true;
    }
    if (step < 0)
      return tl::unexpected(make_error(ErrorCode::SerialDeviceGone,
                                       Operation::WriteSerial,
                                       "injected device disappearance"));
    const auto amount = std::min(static_cast<std::size_t>(step), source.size());
    written_.insert(written_.end(), source.begin(),
                    source.begin() + static_cast<std::ptrdiff_t>(amount));
    return amount;
  }
  Status close() override {
    close_gate.enter();
    return {};
  }
  void set_write_steps(std::initializer_list<std::ptrdiff_t> steps) {
    std::lock_guard lock(mutex_);
    steps_.assign(steps);
  }
  void stall_writes() {
    std::lock_guard lock(mutex_);
    stall_ = true;
  }
  void resume_writes() {
    std::lock_guard lock(mutex_);
    stall_ = stall_after_steps_ = false;
  }
  void write_prefix_then_stall(std::ptrdiff_t prefix) {
    std::lock_guard lock(mutex_);
    steps_ = {prefix};
    stall_after_steps_ = true;
  }
  std::vector<std::byte> written() const {
    std::lock_guard lock(mutex_);
    return written_;
  }
  void inject_rx(std::span<const std::byte> bytes) const {
    if (::write(peer_fd_, bytes.data(), bytes.size()) !=
        static_cast<ssize_t>(bytes.size())) {
      throw std::runtime_error("fake RX injection failed");
    }
  }
  void disappear() { static_cast<void>(::close(std::exchange(peer_fd_, -1))); }
  void release_all() {
    open_gate.release();
    enumerate_gate.release();
    write_gate.release();
    close_gate.release();
  }
  Gate open_gate, enumerate_gate, write_gate, close_gate;

private:
  int owner_fd_{-1}, peer_fd_{-1};
  mutable std::mutex mutex_;
  std::deque<std::ptrdiff_t> steps_;
  bool stall_{}, stall_after_steps_{};
  std::vector<std::byte> written_;
};

DevicePath fake_path() {
  return {.requested = "/dev/fake",
          .canonical = "/dev/fake",
          .identity = {.device = 1U, .inode = 2U, .special_device = 3U}};
}
std::vector<std::byte> bytes(std::string_view text) {
  std::vector<std::byte> result;
  for (const auto value : text)
    result.push_back(static_cast<std::byte>(value));
  return result;
}
bool wait_until(const auto &predicate, std::chrono::milliseconds timeout = 1s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}
std::size_t retained_event_bytes(const SerialDataEvent &event) {
  return sizeof(SerialDataEvent) + event.bytes.capacity() +
         (event.error ? event.error->detail.capacity() : 0U);
}
std::size_t retained_event_bytes(const std::vector<SerialDataEvent> &events) {
  std::size_t result = 0U;
  for (const auto &event : events) {
    result += retained_event_bytes(event);
  }
  return result;
}
const TxCompletion &tx_result(const SerialCompletion &value, std::uint64_t id,
                              OperationOutcome outcome) {
  REQUIRE(std::holds_alternative<TxCompletion>(value));
  const auto &tx = std::get<TxCompletion>(value);
  CHECK(tx.operation_id == OperationId{id});
  CHECK(tx.outcome == outcome);
  return tx;
}

struct SerialFixture {
  explicit SerialFixture(SerialServiceOptions options = {}) {
    auto fake = std::make_unique<FakeSerialBackend>();
    backend = fake.get();
    auto created = SerialService::create(std::move(fake), options);
    REQUIRE(created);
    service = std::move(*created);
  }
  ~SerialFixture() {
    backend->release_all();
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
  void connect(std::uint64_t operation = 1U, std::uint64_t generation = 1U) {
    REQUIRE(service->submit_connect(
        {{OperationId{operation}, ConnectionGeneration{generation}},
         fake_path(),
         {}}));
    const auto result = completions(1U);
    REQUIRE(std::holds_alternative<ConnectCompletion>(result.front()));
    connected = std::get<ConnectCompletion>(result.front());
    REQUIRE(connected.outcome == OperationOutcome::Succeeded);
    REQUIRE(connected.session_id);
  }
  Result<OperationId> send(std::uint64_t id, std::vector<std::byte> payload,
                           std::optional<TaskGeneration> task = std::nullopt) {
    return service->submit_tx(
        {{OperationId{id}, connected.generation, *connected.session_id, task},
         std::move(payload)});
  }
  auto disconnect(std::uint64_t id) {
    return service->request_disconnect(
        {OperationId{id}, connected.generation, connected.session_id});
  }
  std::vector<SerialDataEvent> drain_events(std::size_t count) {
    const auto queued = service->queued_data_bytes();
    auto result = service->drain_data(8U);
    REQUIRE(result.size() == count);
    CHECK(queued == retained_event_bytes(result));
    CHECK(service->queued_data_bytes() == 0U);
    for (std::size_t i = 1U; i < result.size(); ++i)
      CHECK(result[i - 1U].owner_order < result[i].owner_order);
    return result;
  }
  FakeSerialBackend *backend{};
  std::unique_ptr<SerialService> service;
  ConnectCompletion connected;
};
} // namespace

TEST_CASE("open cancellation and stop settle accepted work under saturation",
          "[serial][owner]") {
  SerialServiceOptions options;
  options.command_max_messages = 1U;
  SerialFixture test{options};
  test.backend->open_gate.block();
  REQUIRE(test.service->submit_connect(
      {{OperationId{1}, ConnectionGeneration{1}}, fake_path(), {}}));
  REQUIRE(test.backend->open_gate.wait());
  const auto rejected = test.service->submit_connect(
      {{OperationId{2}, ConnectionGeneration{2}}, fake_path(), {}});
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().detail == "serial command queue is full");
  std::size_t count = 1U;
  SECTION("cancel has a reserved control completion") {
    REQUIRE(test.service->request_cancel_connect(
        {OperationId{3}, ConnectionGeneration{1}}));
    count = 2U;
  }
  SECTION("stop closes all connect producers") {
    test.service->request_stop();
    REQUIRE_FALSE(test.service->submit_connect(
        {{OperationId{4}, ConnectionGeneration{2}}, fake_path(), {}}));
    REQUIRE_FALSE(test.service->request_cancel_connect(
        {OperationId{3}, ConnectionGeneration{1}}));
  }
  test.backend->open_gate.release();
  const auto result = test.completions(count);
  REQUIRE(std::holds_alternative<ConnectCompletion>(result[0]));
  CHECK(std::get<ConnectCompletion>(result[0]).outcome ==
        OperationOutcome::Cancelled);
  if (count == 2U)
    REQUIRE(std::holds_alternative<DisconnectCompletion>(result[1]));
  else
    REQUIRE(test.service->wait_until_stopped(std::chrono::steady_clock::now() +
                                             1s));
}

TEST_CASE("scanner bounds pending work and settles active work during stop",
          "[serial][scanner]") {
  auto fake = std::make_unique<FakeSerialBackend>();
  auto *backend = fake.get();
  backend->enumerate_gate.block();
  auto created = DeviceScanner::create(std::move(fake));
  REQUIRE(created);
  auto scanner = std::move(*created);
  struct Cleanup {
    FakeSerialBackend &backend;
    DeviceScanner &scanner;
    ~Cleanup() {
      backend.release_all();
      scanner.request_stop();
      if (!scanner.wait_until_stopped(std::chrono::steady_clock::now() + 2s))
        std::terminate();
    }
  } cleanup{*backend, *scanner};
  REQUIRE(scanner->submit_scan(OperationId{1}, ScanGeneration{1}));
  REQUIRE(backend->enumerate_gate.wait());
  std::size_t count = 1U;
  SECTION("one pending request is retained and the third is rejected") {
    REQUIRE(scanner->submit_scan(OperationId{2}, ScanGeneration{2}));
    REQUIRE_FALSE(scanner->submit_scan(OperationId{3}, ScanGeneration{3}));
    count = 2U;
  }
  SECTION("stop rejects new work before enumerate returns") {
    scanner->request_stop();
    REQUIRE_FALSE(scanner->submit_scan(OperationId{2}, ScanGeneration{2}));
  }
  backend->enumerate_gate.release();
  std::vector<ScanCompletion> results;
  REQUIRE(wait_until([&] {
    auto batch = scanner->drain_completions();
    results.insert(results.end(), std::make_move_iterator(batch.begin()),
                   std::make_move_iterator(batch.end()));
    return results.size() >= count;
  }));
  REQUIRE(results.size() == count);
  CHECK(results[0].operation_id == OperationId{1});
  CHECK(results[0].generation == ScanGeneration{1});
  if (count == 1U)
    CHECK(results[0].outcome == OperationOutcome::Cancelled);
  else {
    CHECK(results[1].generation == ScanGeneration{2});
    REQUIRE_FALSE(results[0].devices.empty());
    CHECK(results[0].devices.front().path == "/dev/fake");
  }
  scanner->request_stop();
  REQUIRE(scanner->wait_until_stopped(std::chrono::steady_clock::now() + 1s));
  REQUIRE(scanner->worker_stopped_signal());
}

TEST_CASE("partial TX never interleaves and manual TX wins the next boundary",
          "[serial][owner][scheduler]") {
  SerialFixture test;
  test.connect();
  test.backend->set_write_steps({2, 0, 1, 64, 1, 64, 1, 64});
  test.backend->write_gate.block();
  REQUIRE(test.send(2U, bytes("Abcd")));
  REQUIRE(test.backend->write_gate.wait());
  REQUIRE(test.send(3U, bytes("Qq"), TaskGeneration{7}));
  REQUIRE(test.send(4U, bytes("Bb")));
  test.backend->write_gate.release();
  const auto result = test.completions(3U);
  tx_result(result[0], 2U, OperationOutcome::Succeeded);
  tx_result(result[1], 4U, OperationOutcome::Succeeded);
  tx_result(result[2], 3U, OperationOutcome::Succeeded);
  CHECK(test.backend->written() == bytes("AbcdBbQq"));
  CHECK_FALSE(test.service->fatal_signal());
}

TEST_CASE(
    "disconnect and stop interrupt partial TX despite saturated admission",
    "[serial][owner]") {
  SerialServiceOptions options;
  options.tx_max_messages = 1U;
  SerialFixture test{options};
  test.connect();
  test.backend->set_write_steps({1, 64});
  test.backend->write_gate.block();
  REQUIRE(test.send(2U, bytes("ABC")));
  REQUIRE(test.backend->write_gate.wait());
  REQUIRE_FALSE(test.send(3U, bytes("D")));
  std::size_t count = 1U;
  SECTION("disconnect during the backend call") {
    REQUIRE(test.disconnect(4U));
    count = 2U;
  }
  SECTION("owner stop during the backend call") {
    test.service->request_stop();
  }
  test.backend->write_gate.release();
  const auto result = test.completions(count);
  CHECK(tx_result(result[0], 2U, OperationOutcome::Cancelled).accepted_bytes ==
        1U);
  if (count == 2U)
    REQUIRE(std::holds_alternative<DisconnectCompletion>(result[1]));
  else
    REQUIRE(test.service->wait_until_stopped(std::chrono::steady_clock::now() +
                                             1s));
  CHECK(test.backend->written() == bytes("A"));
  const auto events = test.drain_events(3U);
  CHECK(events[0].kind == SerialDataKind::Tx);
  CHECK(events[1].kind == SerialDataKind::Error);
  CHECK(events[2].kind == SerialDataKind::Cleanup);
  CHECK(events[2].origin == SessionEventOrigin::Cleanup);
}

TEST_CASE("successful TX events survive saturated RX ingress",
          "[serial][owner]") {
  SerialServiceOptions options;
  options.command_max_messages = 1U;
  options.tx_max_messages = 2U;
  options.rx_max_chunks = 1U;
  SerialFixture test{options};
  test.connect();
  test.backend->inject_rx(bytes("R"));
  REQUIRE(wait_until([&] { return test.service->queued_data_bytes() != 0U; }));
  const auto saturated = test.service->queued_data_bytes();
  REQUIRE(test.send(2U, bytes("A")));
  tx_result(test.completions(1U)[0], 2U, OperationOutcome::Succeeded);
  REQUIRE(wait_until([&] {
    return test.service->queued_data_bytes() > saturated ||
           !test.service->connection_snapshot().connected;
  }));
  REQUIRE(test.service->connection_snapshot().connected);
  REQUIRE(test.send(3U, bytes("B")));
  tx_result(test.completions(1U)[0], 3U, OperationOutcome::Succeeded);
  const auto events = test.drain_events(3U);
  CHECK(events[0].kind == SerialDataKind::Rx);
  CHECK(events[0].bytes == bytes("R"));
  for (std::size_t i = 1U; i <= 2U; ++i) {
    CHECK(events[i].kind == SerialDataKind::Tx);
    CHECK(events[i].operation_id == OperationId{i + 1U});
    CHECK(events[i].bytes == bytes(i == 1U ? "A" : "B"));
  }
  CHECK_FALSE(test.service->overflow_signal());
  CHECK_FALSE(test.service->fatal_signal());
}

TEST_CASE("retained terminal data keeps its admission and releases it on drain",
          "[serial][owner]") {
  SerialServiceOptions options;
  options.command_max_messages = 1U;
  options.tx_max_messages = 1U;
  SECTION("TX message reservation survives completion consumption") {
    SerialFixture test{options};
    test.connect();
    REQUIRE(test.send(2U, bytes("A")));
    tx_result(test.completions(1U)[0], 2U, OperationOutcome::Succeeded);
    // Exceed the five reserved slots without consuming the retained data.
    for (std::uint64_t id = 3U; id <= 8U; ++id) {
      const auto rejected = test.send(id, bytes("B"));
      REQUIRE_FALSE(rejected);
      CHECK(rejected.error().detail == "serial TX queue is full");
    }
    CHECK(test.service->connection_snapshot().connected);
    CHECK(test.drain_events(1U)[0].kind == SerialDataKind::Tx);
    REQUIRE(test.send(9U, bytes("C")));
    tx_result(test.completions(1U)[0], 9U, OperationOutcome::Succeeded);
    test.drain_events(1U);
    CHECK_FALSE(test.service->fatal_signal());
  }
  SECTION("cleanup reservations survive across sessions") {
    SerialFixture test{options};
    for (std::uint64_t generation = 1U; generation <= 3U; ++generation) {
      test.connect(generation * 2U - 1U, generation);
      REQUIRE(test.disconnect(generation * 2U));
      REQUIRE(std::holds_alternative<DisconnectCompletion>(
          test.completions(1U)[0]));
    }
    const auto rejected = test.service->submit_connect(
        {{OperationId{7}, ConnectionGeneration{4}}, fake_path(), {}});
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().detail == "serial cleanup capacity is full");
    for (const auto &event : test.drain_events(3U))
      CHECK(event.kind == SerialDataKind::Cleanup);
    test.connect(8U, 4U);
    CHECK_FALSE(test.service->fatal_signal());
  }
  SECTION("TX byte reservations cannot consume the independent RX quota") {
    options.tx_max_messages = 8U;
    options.tx_max_bytes = 1024U * 1024U;
    options.rx_max_chunks = 1U;
    options.rx_max_bytes = 4096U;
    SerialFixture test{options};
    test.connect();
    const std::vector<std::byte> payload(700U * 1024U, std::byte{'T'});
    REQUIRE(test.send(2U, payload));
    test.completions(1U);
    const auto rejected = test.send(3U, payload);
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().detail == "serial TX queue is full");
    const auto before = test.service->queued_data_bytes();
    REQUIRE(before > options.rx_max_bytes);
    test.backend->inject_rx(bytes("R"));
    REQUIRE(
        wait_until([&] { return test.service->queued_data_bytes() > before; }));
    REQUIRE(test.service->connection_snapshot().connected);
    CHECK_FALSE(test.service->overflow_signal());
    const auto events = test.drain_events(2U);
    CHECK(events[0].kind == SerialDataKind::Tx);
    CHECK(events[1].kind == SerialDataKind::Rx);
    CHECK(retained_event_bytes(events[0]) == before);
    REQUIRE(test.send(3U, payload));
    test.completions(1U);
    test.drain_events(1U);
  }
}

TEST_CASE(
    "TX deadlines stop partial writes and restart only for the next request",
    "[serial][owner]") {
  SerialServiceOptions options;
  options.tx_timeout = 100ms;
  SerialFixture test{options};
  test.connect();
  bool queued_next = false;
  SECTION(
      "zero progress times out and the queued request gets a fresh deadline") {
    test.backend->write_prefix_then_stall(1);
    REQUIRE(test.send(2U, bytes("AB")));
    REQUIRE(test.send(3U, bytes("B")));
    queued_next = true;
  }
  SECTION("a positive partial write returns after its deadline") {
    test.backend->set_write_steps({1, 64});
    test.backend->write_gate.block();
    REQUIRE(test.send(2U, bytes("ABC")));
    REQUIRE(test.backend->write_gate.wait());
    std::this_thread::sleep_for(150ms);
    test.backend->write_gate.release();
  }
  const auto first = test.completions(1U);
  const auto &tx = tx_result(first[0], 2U, OperationOutcome::TimedOut);
  CHECK(tx.accepted_bytes == 1U);
  REQUIRE(tx.error);
  CHECK(tx.error->code == ErrorCode::SerialOperationTimedOut);
  CHECK(test.backend->written() == bytes("A"));
  const auto events = test.drain_events(2U);
  CHECK(events[0].kind == SerialDataKind::Tx);
  CHECK(events[0].bytes == bytes("A"));
  CHECK(events[1].kind == SerialDataKind::Error);
  REQUIRE(events[1].error);
  CHECK(events[1].error->code == ErrorCode::SerialOperationTimedOut);
  CHECK(events[1].error->operation_id == OperationId{2});
  if (queued_next) {
    std::this_thread::sleep_for(60ms);
    test.backend->resume_writes();
    tx_result(test.completions(1U)[0], 3U, OperationOutcome::Succeeded);
  }
  CHECK_FALSE(test.service->fatal_signal());
}

TEST_CASE("zero-byte TX writes use bounded retry backoff", "[serial][owner]") {
  SerialServiceOptions options;
  options.tx_timeout = 500ms;
  SerialFixture test{options};
  test.connect();
  test.backend->stall_writes();
  REQUIRE(test.send(2U, bytes("A")));
  REQUIRE(test.backend->write_gate.wait());
  const auto before = test.service->wait_count();
  std::this_thread::sleep_for(50ms);
  const auto waits = test.service->wait_count() - before;
  CHECK(waits >= 3U);
  CHECK(waits <= 20U);
}

TEST_CASE("owner stop settles producers and publishes its fixed lifecycle slot",
          "[serial][owner]") {
  SerialFixture test;
  SECTION("idle ppoll is woken") { std::this_thread::sleep_for(20ms); }
  SECTION("accepted TX and task control all complete") {
    test.connect();
    test.backend->stall_writes();
    for (std::uint64_t id = 2U; id <= 4U; ++id)
      REQUIRE(test.send(id, bytes("A")));
    REQUIRE(test.backend->write_gate.wait());
    const auto control =
        test.service->request_stop_task({OperationId{5}, TaskGeneration{9}});
    REQUIRE(control);
    REQUIRE(*control == SubmitStatus::Accepted);
    test.service->request_stop();
    REQUIRE_FALSE(test.send(6U, bytes("B")));
    REQUIRE_FALSE(test.disconnect(7U));
    REQUIRE_FALSE(
        test.service->request_stop_task({OperationId{8}, TaskGeneration{1}}));
    REQUIRE_FALSE(test.service->request_cancel_connect(
        {OperationId{9}, ConnectionGeneration{1}}));
    const auto results = test.completions(4U);
    for (std::uint64_t id = 2U; id <= 4U; ++id) {
      CHECK(std::count_if(results.begin(), results.end(),
                          [id](const auto &value) {
                            const auto *tx = std::get_if<TxCompletion>(&value);
                            return tx && tx->operation_id == OperationId{id} &&
                                   tx->outcome == OperationOutcome::Cancelled;
                          }) == 1);
    }
    CHECK(std::count_if(results.begin(), results.end(), [](const auto &value) {
            return std::holds_alternative<TaskStopCompletion>(value);
          }) == 1);
  }
  test.service->request_stop();
  REQUIRE(
      test.service->wait_until_stopped(std::chrono::steady_clock::now() + 1s));
  const auto signal = test.service->worker_stopped_signal();
  REQUIRE(signal);
  CHECK(signal->worker == WorkerKind::Serial);
  CHECK(signal->lifecycle == WorkerLifecycle::AtReturnPoint);
  CHECK_FALSE(test.service->fatal_signal());
}

TEST_CASE(
    "task stop closes admission and confirms only after partial TX settles",
    "[serial][owner][scheduler]") {
  SerialServiceOptions options;
  options.tx_max_messages = 1U;
  SerialFixture test{options};
  test.connect();
  test.backend->write_prefix_then_stall(1);
  bool blocked = false;
  SECTION("stop arrives before the backend call returns") {
    test.backend->write_gate.block();
    blocked = true;
  }
  SECTION("stop arrives after the backend accepted a prefix") {}
  REQUIRE(test.send(2U, bytes("ABC"), TaskGeneration{7}));
  REQUIRE(test.backend->write_gate.wait());
  if (!blocked)
    REQUIRE(wait_until([&] { return test.backend->written() == bytes("A"); }));
  REQUIRE(test.service->request_stop_task({OperationId{3}, TaskGeneration{7}}));
  if (blocked) {
    const auto raced = test.send(4U, bytes("B"), TaskGeneration{7});
    REQUIRE_FALSE(raced);
    CHECK(raced.error().detail == "serial task is stopping");
  }
  test.backend->write_gate.release();
  const auto results = test.completions(2U);
  const auto &tx = tx_result(results[0], 2U, OperationOutcome::Cancelled);
  CHECK(tx.accepted_bytes == 1U);
  REQUIRE(tx.error);
  CHECK(tx.error->code == ErrorCode::SerialOperationCancelled);
  REQUIRE(std::holds_alternative<TaskStopCompletion>(results[1]));
  CHECK(std::get<TaskStopCompletion>(results[1]).generation ==
        TaskGeneration{7});
  CHECK(test.backend->written() == bytes("A"));
  const auto queued = test.service->queued_data_bytes();
  const auto prefix = test.service->drain_data(1U);
  REQUIRE(prefix.size() == 1U);
  CHECK(prefix[0].kind == SerialDataKind::Tx);
  const auto rejected = test.send(5U, bytes("D"));
  REQUIRE_FALSE(rejected);
  CHECK(rejected.error().detail == "serial TX queue is full");
  const auto terminal = test.service->drain_data(1U);
  REQUIRE(terminal.size() == 1U);
  CHECK(terminal[0].kind == SerialDataKind::Error);
  REQUIRE(terminal[0].error);
  CHECK(terminal[0].error->code == ErrorCode::SerialOperationCancelled);
  CHECK(prefix[0].owner_order < terminal[0].owner_order);
  CHECK(queued ==
        retained_event_bytes(prefix) + retained_event_bytes(terminal));
  CHECK(test.service->queued_data_bytes() == 0U);
  test.backend->resume_writes();
  REQUIRE(test.send(6U, bytes("E")));
  test.completions(1U);
  test.drain_events(1U);
  CHECK_FALSE(test.service->fatal_signal());
}

TEST_CASE("late sessions are rejected and device loss performs cleanup",
          "[serial][owner]") {
  SerialFixture test;
  test.connect();
  REQUIRE_FALSE(test.service->submit_tx(
      {{OperationId{2}, test.connected.generation,
        SessionId{test.connected.session_id->value + 1U}, std::nullopt},
       bytes("A")}));
  test.backend->disappear();
  REQUIRE(wait_until([&] {
    const auto state = test.service->connection_snapshot();
    return !state.connected && !state.disconnecting;
  }));
  const auto events = test.drain_events(1U);
  CHECK(events[0].kind == SerialDataKind::Cleanup);
  REQUIRE(events[0].error);
}

TEST_CASE("fault transition closes TX admission before backend close",
          "[serial][owner]") {
  SerialFixture test;
  test.connect();
  test.backend->set_write_steps({-1});
  test.backend->write_gate.block();
  REQUIRE(test.send(2U, bytes("A")));
  REQUIRE(test.backend->write_gate.wait());
  test.backend->close_gate.block();
  test.backend->write_gate.release();
  REQUIRE(test.backend->close_gate.wait());
  const auto raced = test.send(3U, bytes("B"));
  REQUIRE_FALSE(raced);
  CHECK(raced.error().detail == "stale or disconnected TX session");
  test.backend->close_gate.release();
  REQUIRE(wait_until([&] {
    const auto state = test.service->connection_snapshot();
    return !state.connected && !state.disconnecting;
  }));
  tx_result(test.completions(1U)[0], 2U, OperationOutcome::Failed);
  const auto events = test.drain_events(2U);
  CHECK(events[0].kind == SerialDataKind::Error);
  CHECK(events[1].kind == SerialDataKind::Cleanup);
  CHECK_FALSE(test.service->fatal_signal());
}

TEST_CASE("device loss after a partial write reports the accepted prefix",
          "[serial][owner]") {
  SerialFixture test;
  test.connect();
  test.backend->set_write_steps({2, -1});
  REQUIRE(test.send(2U, bytes("ABCD")));
  const auto result = test.completions(1U);
  const auto &tx = tx_result(result[0], 2U, OperationOutcome::Failed);
  CHECK(tx.accepted_bytes == 2U);
  REQUIRE(tx.error);
  REQUIRE(wait_until([&] {
    const auto state = test.service->connection_snapshot();
    return !state.connected && !state.disconnecting;
  }));
  const auto events = test.drain_events(3U);
  CHECK(events[0].kind == SerialDataKind::Tx);
  CHECK(events[0].bytes == bytes("AB"));
  CHECK(events[1].kind == SerialDataKind::Error);
  REQUIRE(events[1].error);
  CHECK(events[1].error->code == ErrorCode::SerialDeviceGone);
  CHECK(events[1].error->operation_id == OperationId{2});
  CHECK(events[2].kind == SerialDataKind::Cleanup);
}
