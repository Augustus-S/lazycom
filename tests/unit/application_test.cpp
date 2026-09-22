#include <lazycom/app/application.hpp>

#include <catch2/catch_test_macros.hpp>
#include <support/temporary_directory.hpp>
#include <support/wait.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <utility>
#include <vector>

#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace app = lazycom::app;
namespace config = lazycom::config;
namespace logging = lazycom::logging;
namespace serial = lazycom::serial;

namespace {
using namespace std::chrono_literals;
using lazycom::test::wait_until;

class FakeBackend final : public serial::ISerialBackend {
public:
  explicit FakeBackend(std::vector<serial::DeviceInfo> devices = {},
                       const bool block_enumerate = false)
      : devices_(std::move(devices)), block_enumerate_(block_enumerate),
        wait_fd_(::eventfd(0U, EFD_NONBLOCK)) {
    if (wait_fd_ < 0) {
      throw std::runtime_error("cannot create fake serial eventfd");
    }
  }

  ~FakeBackend() override { static_cast<void>(::close(wait_fd_)); }

  lazycom::Result<std::vector<serial::DeviceInfo>> enumerate() override {
    enumerate_entered_.store(true, std::memory_order_release);
    if (block_enumerate_) {
      std::unique_lock lock(mutex_);
      enumerate_condition_.wait(lock, [] { return false; });
    }
    return devices_;
  }
  lazycom::Status open(const serial::DevicePath &,
                       const serial::PortConfig &) override {
    if (!open_rx_.empty()) {
      inject_rx(open_rx_);
    }
    opened_.store(true, std::memory_order_release);
    return {};
  }
  lazycom::Result<int> native_wait_handle() const override { return wait_fd_; }
  lazycom::Result<std::size_t>
  read_some(const std::span<std::byte> destination) override {
    std::lock_guard lock(mutex_);
    if (throw_read_) {
      throw_read_ = false;
      throw std::runtime_error("injected fatal read exception");
    }
    if (fail_read_) {
      fail_read_ = false;
      std::uint64_t wake_count{};
      static_cast<void>(::read(wait_fd_, &wake_count, sizeof(wake_count)));
      return tl::unexpected(lazycom::make_error(
          lazycom::ErrorCode::SerialDeviceGone, lazycom::Operation::ReadSerial,
          "injected application read failure"));
    }
    if (pending_rx_.empty()) {
      return std::size_t{0U};
    }
    const auto amount =
        std::min({destination.size(), pending_rx_.size(), maximum_read_size_});
    std::copy_n(pending_rx_.begin(), amount, destination.begin());
    pending_rx_.erase(pending_rx_.begin(),
                      pending_rx_.begin() +
                          static_cast<std::ptrdiff_t>(amount));
    std::uint64_t wake_count{};
    static_cast<void>(::read(wait_fd_, &wake_count, sizeof(wake_count)));
    read_bytes_count_.fetch_add(amount, std::memory_order_release);
    return amount;
  }
  lazycom::Result<std::size_t>
  write_some(std::span<const std::byte> source) override {
    std::lock_guard lock(mutex_);
    const auto remaining =
        write_limit_ > written_bytes_ ? write_limit_ - written_bytes_ : 0U;
    if (remaining == 0U && fail_after_write_limit_) {
      fail_after_write_limit_ = false;
      return tl::unexpected(lazycom::make_error(
          lazycom::ErrorCode::SerialDeviceGone, lazycom::Operation::WriteSerial,
          "injected application partial TX failure"));
    }
    const auto amount = std::min(source.size(), remaining);
    written_bytes_ += amount;
    written_payload_.append(reinterpret_cast<const char *>(source.data()),
                            amount);
    return amount;
  }
  lazycom::Status close() override { return {}; }

  void inject_rx(const std::string_view bytes) {
    {
      std::lock_guard lock(mutex_);
      for (const char value : bytes) {
        pending_rx_.push_back(static_cast<std::byte>(value));
      }
    }
    const std::uint64_t wake = 1U;
    static_cast<void>(::write(wait_fd_, &wake, sizeof(wake)));
  }

  void fail_next_read() {
    {
      std::lock_guard lock(mutex_);
      fail_read_ = true;
    }
    const std::uint64_t wake = 1U;
    static_cast<void>(::write(wait_fd_, &wake, sizeof(wake)));
  }

  void throw_next_read() {
    {
      std::lock_guard lock(mutex_);
      throw_read_ = true;
    }
    const std::uint64_t wake = 1U;
    static_cast<void>(::write(wait_fd_, &wake, sizeof(wake)));
  }

  void fail_after_write_limit(const std::size_t size) {
    std::lock_guard lock(mutex_);
    write_limit_ = size;
    written_bytes_ = 0U;
    fail_after_write_limit_ = true;
  }

  void set_maximum_read_size(const std::size_t size) {
    std::lock_guard lock(mutex_);
    maximum_read_size_ = size;
  }

  void set_write_limit(const std::size_t size) {
    std::lock_guard lock(mutex_);
    write_limit_ = size;
    written_bytes_ = 0U;
  }

  [[nodiscard]] std::size_t read_bytes() const noexcept {
    return read_bytes_count_.load(std::memory_order_acquire);
  }

  [[nodiscard]] std::size_t written_bytes() const {
    std::lock_guard lock(mutex_);
    return written_bytes_;
  }

  [[nodiscard]] std::string written_payload() const {
    std::lock_guard lock(mutex_);
    return written_payload_;
  }

  [[nodiscard]] bool opened() const noexcept {
    return opened_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool enumerate_entered() const noexcept {
    return enumerate_entered_.load(std::memory_order_acquire);
  }

  void inject_rx_on_open(std::string bytes) { open_rx_ = std::move(bytes); }

private:
  std::vector<serial::DeviceInfo> devices_;
  bool block_enumerate_{};
  int wait_fd_;
  mutable std::mutex mutex_;
  std::condition_variable enumerate_condition_;
  std::vector<std::byte> pending_rx_;
  std::string open_rx_;
  std::size_t maximum_read_size_{std::numeric_limits<std::size_t>::max()};
  std::size_t write_limit_{std::numeric_limits<std::size_t>::max()};
  std::size_t written_bytes_{};
  std::string written_payload_;
  std::atomic<std::size_t> read_bytes_count_{};
  std::atomic_bool opened_{};
  std::atomic_bool enumerate_entered_{};
  bool fail_read_{};
  bool throw_read_{};
  bool fail_after_write_limit_{};
};

template <typename Predicate>
bool tick_until(app::Application &application, Predicate predicate,
                const int attempts = 500) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    if (predicate()) {
      return true;
    }
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return predicate();
}

struct ApplicationHarness {
  lazycom::test::TemporaryDirectory directory;
  config::PersistencePaths paths{directory.path() / "config.toml",
                                 directory.path() / "quick_send.toml",
                                 directory.path() / "state.toml"};
  std::unique_ptr<app::Application> application;
  bool shutdown_attempted{};

  ApplicationHarness(
      std::unique_ptr<serial::ISerialBackend> serial =
          std::make_unique<FakeBackend>(),
      std::unique_ptr<serial::ISerialBackend> scanner =
          std::make_unique<FakeBackend>(),
      const std::string_view log_subdirectory = "logs",
      const std::function<void(const config::PersistencePaths &)> &prepare = {},
      lazycom::model::GlobalMemoryBudget budget =
          lazycom::model::GlobalMemoryBudget{}) {
    if (prepare) {
      prepare(paths);
    }
    app::ApplicationDependencies dependencies;
    dependencies.serial_backend = std::move(serial);
    dependencies.scanner_backend = std::move(scanner);
    dependencies.paths = paths;
    dependencies.log_directory = directory.path() / log_subdirectory;
    dependencies.memory_budget = std::move(budget);
    auto created = app::Application::create(std::move(dependencies));
    if (!created) {
      throw std::runtime_error("cannot create application test fixture");
    }
    application = std::move(*created);
  }

  ~ApplicationHarness() {
    if (application && !shutdown_attempted) {
      static_cast<void>(application->shutdown());
    }
    application.reset();
  }

  [[nodiscard]] app::Application &app() const { return *application; }

  [[nodiscard]] bool shutdown() {
    shutdown_attempted = true;
    return application->shutdown();
  }

  [[nodiscard]] bool connect() {
    application->set_device_path("/dev/null");
    application->connect();
    return tick_until(*application, [&] {
      return application->snapshot().connection ==
             app::ConnectionState::Connected;
    });
  }

  [[nodiscard]] bool start_logging() {
    application->toggle_log();
    return tick_until(*application, [&] {
      return application->snapshot().log == app::LogState::Recording;
    });
  }
};

auto read_session(const std::filesystem::path &directory) {
  std::filesystem::path path;
  for (const auto &entry : std::filesystem::directory_iterator{directory}) {
    if (!entry.path().filename().string().starts_with("lazycom-session-"))
      continue;
    REQUIRE(path.empty());
    path = entry.path();
  }
  REQUIRE_FALSE(path.empty());
  std::ifstream input{path, std::ios::binary};
  REQUIRE(input);
  const std::string text{std::istreambuf_iterator<char>{input},
                         std::istreambuf_iterator<char>{}};
  auto document = logging::decode_ndjson(text);
  REQUIRE(document);
  return document;
}

void write_private(const std::filesystem::path &path, std::string_view text) {
  std::ofstream output{path};
  REQUIRE(output);
  output << text;
  output.close();
  REQUIRE(output);
  REQUIRE(::chmod(path.c_str(), S_IRUSR | S_IWUSR) == 0);
}
} // namespace

TEST_CASE("application restores startup state and integrates device scans",
          "[application]") {
  SECTION("application loads defaults and merges scanner completion") {
    serial::DeviceInfo fake_device;
    fake_device.path = "/dev/ttyFAKE0";
    fake_device.description = "fake";
    ApplicationHarness harness{
        std::make_unique<FakeBackend>(),
        std::make_unique<FakeBackend>(
            std::vector<serial::DeviceInfo>{std::move(fake_device)})};
    auto &application = harness.app();
    REQUIRE(tick_until(application,
                       [&] { return !application.snapshot().scanning; }));
    REQUIRE(application.snapshot().config.serial.baud == 115200);
    REQUIRE(application.snapshot().devices.size() == 1U);
    REQUIRE(application.snapshot().devices.front().path == "/dev/ttyFAKE0");
    REQUIRE(application.apply_baud(230400));
    REQUIRE(application.snapshot().config.serial.baud == 230400);
    REQUIRE(application.apply_data_format(
        {8, 1, config::Parity::None, config::FlowControl::None}));
    REQUIRE_FALSE(application.apply_baud(0));
    REQUIRE_FALSE(application.apply_newline(config::Newline::Session));
    REQUIRE_FALSE(application.apply_newline(static_cast<config::Newline>(99)));
    REQUIRE_FALSE(
        application.apply_send_mode(static_cast<config::SendMode>(99)));
    REQUIRE_FALSE(application.apply_data_format(
        {8, 1, static_cast<config::Parity>(99), config::FlowControl::None}));
    REQUIRE(harness.shutdown());
  }
  SECTION("application restores receive visibility from state") {
    ApplicationHarness harness{
        std::make_unique<FakeBackend>(), std::make_unique<FakeBackend>(),
        "logs", [](const auto &paths) {
          write_private(paths.state,
                        "version = 1\nshow_rx = false\nshow_tx = "
                        "true\nshow_system = false\nshow_error = true\n");
        }};
    auto &application = harness.app();
    const auto &filter = application.snapshot().filter;
    CHECK_FALSE(filter.rx);
    CHECK(filter.tx);
    CHECK_FALSE(filter.system);
    CHECK(filter.error);
    REQUIRE(harness.shutdown());
  }
  SECTION("port selection accepts only the latest scan result") {
    serial::DeviceInfo device;
    device.path = "/dev/null";
    device.description = "test character device";
    ApplicationHarness harness{
        std::make_unique<FakeBackend>(),
        std::make_unique<FakeBackend>(
            std::vector<serial::DeviceInfo>{std::move(device)})};
    auto &application = harness.app();
    REQUIRE(tick_until(application,
                       [&] { return !application.snapshot().scanning; }));
    REQUIRE(application.apply_port("/dev/null"));
    CHECK_FALSE(application.apply_port("/dev/zero"));
    REQUIRE(harness.shutdown());
  }
  SECTION("startup scan publishes one alert when every device is denied") {
    serial::DeviceInfo denied;
    denied.path = "/dev/ttyDENIED0";
    denied.description = "denied";
    denied.permission.access = serial::DeviceAccess::PermissionDenied;
    denied.permission.owner_uid = 0U;
    denied.permission.owner_gid = 18U;
    denied.permission.mode = 0660U;
    denied.permission.group_name = "dialout";

    ApplicationHarness harness{
        std::make_unique<FakeBackend>(),
        std::make_unique<FakeBackend>(
            std::vector<serial::DeviceInfo>{std::move(denied)})};
    auto &application = harness.app();
    REQUIRE(tick_until(
        application, [&] { return bool(application.snapshot().alert); }, 100));
    CHECK(application.snapshot().alert->code ==
          lazycom::ErrorCode::SerialPermissionDenied);
    CHECK(application.snapshot().alert->message.find("dialout") !=
          std::string::npos);
    application.dismiss_alert();
    CHECK_FALSE(application.snapshot().alert);
    application.request_scan();
    REQUIRE(tick_until(
        application, [&] { return !(application.snapshot().scanning); }, 100));
    CHECK_FALSE(application.snapshot().alert);
    REQUIRE(harness.shutdown());
  }
}

TEST_CASE(
    "draft admission rejects disconnected oversized and invalid UTF-8 input",
    "[application]") {
  SECTION("disconnected draft is retained and cannot create TX") {
    ApplicationHarness harness;
    auto &application = harness.app();
    application.set_draft("AT+INFO");
    application.submit_draft();
    REQUIRE(application.snapshot().draft == "AT+INFO");
    REQUIRE(application.snapshot().tx_bytes == 0U);
    REQUIRE(application.snapshot().notice.find("Not connected") !=
            std::string::npos);
    REQUIRE(harness.shutdown());
  }
  SECTION("draft admission preserves valid UTF-8 and the previous contents") {
    ApplicationHarness harness{
        std::make_unique<FakeBackend>(), std::make_unique<FakeBackend>(),
        "logs", [](const auto &paths) {
          write_private(paths.config,
                        "version = 1\n[send]\nmax_draft_bytes = 2\n");
        }};
    auto &application = harness.app();
    application.set_draft("A");
    application.set_draft("A\xE4\xB8\xAD");
    CHECK(application.snapshot().draft == "A");
    application.set_draft(std::string(1U, '\xE4'));
    CHECK(application.snapshot().draft == "A");
    application.set_draft("AB");
    CHECK(application.snapshot().draft == "AB");
    REQUIRE(harness.shutdown());
  }
}

TEST_CASE("connection completion and cancellation preserve session data and "
          "hardware state",
          "[application]") {
  SECTION("receive data published with connect completion is retained") {
    auto serial = std::make_unique<FakeBackend>();
    serial->inject_rx_on_open("ready\n");
    ApplicationHarness harness{std::move(serial)};
    auto &application = harness.app();
    application.set_device_path("/dev/null");
    application.connection_control();
    REQUIRE(tick_until(
        application,
        [&] {
          return !(std::ranges::none_of(
              application.snapshot().records, [](const auto &record) {
                return record.direction == app::RecordDirection::Rx;
              }));
        },
        200));
    REQUIRE(harness.shutdown());
  }
  SECTION("application exposes the active serial configuration snapshot") {
    ApplicationHarness harness;
    auto &application = harness.app();
    REQUIRE_FALSE(application.snapshot().active_port_config);
    application.set_device_path("/dev/null");
    REQUIRE(application.apply_baud(230400));
    REQUIRE(application.apply_data_format(
        {7, 2, config::Parity::Even, config::FlowControl::None}));

    application.connection_control();
    REQUIRE(application.snapshot().active_port_config);
    CHECK(application.snapshot().active_port_config->baud == 230400);
    CHECK(application.snapshot().active_port_config->data_bits == 7);
    CHECK(application.snapshot().active_port_config->stop_bits == 2);
    CHECK(application.snapshot().active_port_config->parity ==
          config::Parity::Even);

    REQUIRE(tick_until(
        application,
        [&] {
          return application.snapshot().connection ==
                 app::ConnectionState::Connected;
        },
        100));
    REQUIRE(application.snapshot().active_port_config);

    application.connection_control();
    REQUIRE(application.snapshot().active_port_config);
    REQUIRE(tick_until(
        application,
        [&] {
          return application.snapshot().connection ==
                 app::ConnectionState::Disconnected;
        },
        100));
    REQUIRE_FALSE(application.snapshot().active_port_config);
    REQUIRE(harness.shutdown());
  }
  SECTION(
      "cancel intent converges when owner connect wins the submission race") {
    auto serial = std::make_unique<FakeBackend>();
    auto *const backend = serial.get();
    backend->inject_rx_on_open("race");
    ApplicationHarness harness{std::move(serial)};
    auto &application = harness.app();
    const auto log_directory = harness.directory.path() / "logs";

    application.toggle_log();
    REQUIRE(application.snapshot().log == app::LogState::Waiting);
    application.set_device_path("/dev/null");
    application.connect();
    REQUIRE(application.snapshot().connection ==
            app::ConnectionState::Connecting);
    REQUIRE(wait_until([&] { return backend->opened(); }, 200ms));
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
    application.disconnect();
    REQUIRE(tick_until(application, [&] {
      return application.snapshot().connection ==
             app::ConnectionState::Disconnected;
    }));
    CHECK_FALSE(application.snapshot().active_port_config);
    REQUIRE(harness.shutdown());
    const auto decoded = read_session(log_directory);
    CHECK(std::ranges::any_of(decoded->records, [](const auto &record) {
      return record.direction == logging::Direction::Rx &&
             record.payload.size() == 4U;
    }));
  }
}

TEST_CASE(
    "inactive logging settings validate directories and rebuild the writer",
    "[application]") {
  SECTION("logging defaults rebuild an inactive writer") {
    ApplicationHarness harness;
    auto &application = harness.app();
    const auto default_logs = harness.directory.path() / "logs";
    REQUIRE(application.apply_logging({{}, 25U, 128U, 16U}));
    REQUIRE(application.snapshot().config.logging.directory.empty());
    CHECK(application.snapshot().effective_log_directory ==
          default_logs.string());
    REQUIRE(application.snapshot().config.logging.max_files == 25U);
    REQUIRE(application.snapshot().config.logging.max_total_size_mib == 128U);
    REQUIRE(application.snapshot().config.logging.max_file_size_mib == 16U);
    REQUIRE_FALSE(application.apply_logging({"relative", 25U, 128U, 16U}));

    REQUIRE(tick_until(application, [&] {
      if (application.snapshot().log == app::LogState::Off)
        application.toggle_log();
      return application.snapshot().log == app::LogState::Waiting;
    }));
    application.toggle_log();
    REQUIRE(tick_until(application, [&] {
      return application.snapshot().log == app::LogState::Off;
    }));
    REQUIRE(harness.shutdown());
  }
  SECTION("logging settings require an existing safe directory") {
    ApplicationHarness harness{std::make_unique<FakeBackend>(),
                               std::make_unique<FakeBackend>(), "default-logs"};
    auto &application = harness.app();
    const auto root = harness.directory.path();
    const auto missing = root / "missing";
    REQUIRE_FALSE(
        application.validate_logging({missing.string(), 25U, 128U, 16U}));
    REQUIRE_FALSE(
        application.apply_logging({missing.string(), 25U, 128U, 16U}));

    const auto regular_file = root / "not-a-directory";
    {
      std::ofstream output(regular_file);
      REQUIRE(output);
    }
    REQUIRE(::chmod(regular_file.c_str(), S_IRUSR | S_IWUSR) == 0);
    REQUIRE_FALSE(
        application.validate_logging({regular_file.string(), 25U, 128U, 16U}));

    const auto directory = root / "logs|session";
    REQUIRE(std::filesystem::create_directory(directory));
    REQUIRE(::chmod(directory.c_str(), S_IRWXU) == 0);
    REQUIRE(application.validate_logging({directory.string(), 25U, 128U, 16U}));
    REQUIRE(application.apply_logging({directory.string(), 25U, 128U, 16U},
                                      app::LogApplyPolicy::NextSession));
    CHECK(application.snapshot().config.logging.directory ==
          directory.string());

    REQUIRE(harness.shutdown());
  }
}

TEST_CASE(
    "configuration persistence serializes snapshots and gates visible changes",
    "[application]") {
  SECTION("rapid config edits persist the latest combined snapshot") {
    ApplicationHarness harness;
    auto &application = harness.app();

    REQUIRE(application.apply_baud(230400));
    REQUIRE(application.apply_data_format(
        {7, 2, config::Parity::Even, config::FlowControl::None}));
    REQUIRE(application.apply_newline(config::Newline::CrLf));
    CHECK(tick_until(
        application,
        [&] {
          const auto loaded = config::load_config_toml(harness.paths.config);
          return loaded.accepted && loaded.snapshot.serial.baud == 230400 &&
                 loaded.snapshot.serial.data_bits == 7 &&
                 loaded.snapshot.serial.stop_bits == 2 &&
                 loaded.snapshot.serial.parity == config::Parity::Even &&
                 loaded.snapshot.send.newline == config::Newline::CrLf;
        },
        200));
    REQUIRE(harness.shutdown());
  }
  SECTION("quick-send deletion changes memory only after persistence commits") {
    ApplicationHarness harness;
    auto &application = harness.app();

    const config::QuickSendSlot slot{
        3U,     "Ping", config::SendMode::Txt, "AT", config::Newline::CrLf,
        "probe"};
    REQUIRE(application.apply_quick_slot(3U, slot));
    REQUIRE_FALSE(application.snapshot().quick_send.slots[2U]);
    REQUIRE(tick_until(application, [&] {
      return application.snapshot().quick_send.slots[2U].has_value();
    }));
    REQUIRE(application.snapshot().quick_send.slots[2U] == slot);

    REQUIRE(application.apply_quick_slot(3U, std::nullopt));
    REQUIRE(application.snapshot().quick_send.slots[2U]);
    REQUIRE(tick_until(application, [&] {
      return !application.snapshot().quick_send.slots[2U].has_value();
    }));
    REQUIRE(harness.shutdown());
  }
  SECTION(
      "save completion never adopts an external replacement as its baseline") {
    using namespace std::chrono_literals;
    ApplicationHarness harness;
    auto &application = harness.app();
    const auto &paths = harness.paths;
    REQUIRE(application.apply_baud(230400));
    REQUIRE(wait_until(
        [&] {
          const auto loaded = config::load_config_toml(paths.config);
          return loaded.accepted && loaded.snapshot.serial.baud == 230400;
        },
        2s));
    const std::string external = "[invalid syntax\n";
    {
      std::ofstream output{paths.config};
      output << external;
    }
    for (int attempt = 0; attempt < 50; ++attempt) {
      application.tick();
      std::this_thread::sleep_for(1ms);
    }
    REQUIRE(application.apply_baud(460800));
    REQUIRE(harness.shutdown());
    const auto retained = config::read_safe_file(paths.config, 1024U);
    REQUIRE(retained);
    CHECK(retained->bytes == external);
  }
}

TEST_CASE("pending log starts converge through reconnect and shutdown",
          "[application]") {
  SECTION("pending log start closes before logging a reconnected session") {
    ApplicationHarness harness;
    auto &application = harness.app();
    REQUIRE(harness.connect());
    application.toggle_log();
    application.disconnect();
    REQUIRE(tick_until(
        application,
        [&] {
          return application.snapshot().connection ==
                 app::ConnectionState::Disconnected;
        },
        500));
    application.connect();
    CHECK(tick_until(application, [&] {
      return application.snapshot().connection ==
                 app::ConnectionState::Connected &&
             application.snapshot().log == app::LogState::Recording;
    }));
    REQUIRE(harness.shutdown());
  }
  SECTION("shutdown persists records queued behind a pending log start") {
    ApplicationHarness harness;
    auto &application = harness.app();
    const auto log_directory = harness.directory.path() / "logs";
    REQUIRE(harness.connect());
    application.toggle_log();
    REQUIRE(harness.shutdown());

    const auto decoded = read_session(log_directory);
    CHECK(std::ranges::any_of(decoded->records, [](const auto &record) {
      return record.direction == logging::Direction::Sys &&
             record.message == "Serial session closed";
    }));
  }
}

TEST_CASE("send history enforces its configured byte limit", "[application]") {
  ApplicationHarness harness{
      std::make_unique<FakeBackend>(), std::make_unique<FakeBackend>(), "logs",
      [](const auto &paths) {
        write_private(paths.config,
                      "version = 1\n[send]\nhistory_max_mib = 1\n");
      }};
  auto &application = harness.app();
  REQUIRE(harness.connect());
  for (const char marker : {'A', 'B'}) {
    application.set_draft(std::string(600'000U, marker));
    application.submit_draft();
    CHECK(application.snapshot().draft.empty());
    REQUIRE(tick_until(
        application, [&] { return application.snapshot().tx_pending == 0U; },
        200));
  }
  application.set_draft("sentinel");
  application.history_previous();
  REQUIRE(application.snapshot().draft.front() == 'B');
  application.history_previous();
  CHECK(application.snapshot().draft.front() == 'B');

  REQUIRE(harness.shutdown());
}

TEST_CASE("search direction is independent from the display filter",
          "[application]") {
  auto serial = std::make_unique<FakeBackend>();
  auto *const serial_backend = serial.get();
  ApplicationHarness harness{std::move(serial)};
  auto &application = harness.app();
  const auto &paths = harness.paths;
  REQUIRE(harness.connect());
  serial_backend->inject_rx("needle\n");
  REQUIRE(tick_until(
      application,
      [&] {
        return !(std::ranges::none_of(
            application.snapshot().records, [](const auto &record) {
              return record.direction == app::RecordDirection::Rx;
            }));
      },
      200));
  REQUIRE(application.apply_view({config::ReceiveView::Txt,
                                  config::ReceiveView::Hex,
                                  {false, true, true, true}}));
  REQUIRE_FALSE(application.apply_view({config::ReceiveView::Txt,
                                        config::ReceiveView::Hex,
                                        {false, false, false, false}}));
  const app::DirectionFilter rx_only{true, false, false, false};
  const app::DirectionFilter tx_only{false, true, false, false};
  const auto rx_matches = application.search("needle", rx_only);
  REQUIRE(rx_matches.size() == 1U);
  const auto rx_record = std::ranges::find_if(
      application.snapshot().records, [](const auto &record) {
        return record.direction == app::RecordDirection::Rx;
      });
  REQUIRE(rx_record != application.snapshot().records.end());
  REQUIRE(rx_matches.front() == rx_record->record_id);
  REQUIRE(application.search("needle", tx_only).empty());

  REQUIRE(application.apply_view({config::ReceiveView::Hex,
                                  config::ReceiveView::Mixed,
                                  {true, false, true, false}}));
  CHECK(application.render_record(*rx_record).find("6E 65 65 64 6C 65") !=
        std::string::npos);
  app::VisibleRecord tx_record;
  tx_record.direction = app::RecordDirection::Tx;
  const std::array tx_payload{std::byte{0x41}};
  tx_record.payload = tx_payload;
  CHECK(application.render_record(tx_record).find("A | 41") !=
        std::string::npos);
  app::VisibleRecord system_record;
  system_record.direction = app::RecordDirection::System;
  system_record.message = "connected";
  CHECK(application.render_record(system_record).ends_with("connected"));
  REQUIRE(harness.shutdown());
  const auto persisted_state = config::load_state_toml(paths.state);
  REQUIRE(persisted_state.accepted);
  CHECK(persisted_state.snapshot.show_rx);
  CHECK_FALSE(persisted_state.snapshot.show_tx);
  CHECK(persisted_state.snapshot.show_system);
  CHECK_FALSE(persisted_state.snapshot.show_error);
}

TEST_CASE("shutdown completes an active log rotation before disconnecting",
          "[application][logging][shutdown]") {
  auto backend = std::make_unique<FakeBackend>();
  auto *const serial_backend = backend.get();
  ApplicationHarness harness{std::move(backend),
                             std::make_unique<FakeBackend>(), "old-logs"};
  auto &application = harness.app();
  const auto new_logs = harness.directory.path() / "new-logs";
  REQUIRE(harness.connect());
  application.toggle_log();
  bool disconnect_before_shutdown = false;
  SECTION("rotation begins after the initial session start completes") {
    REQUIRE(tick_until(
        application,
        [&] { return application.snapshot().log == app::LogState::Recording; },
        200));
  }
  SECTION("rotation is requested while the initial start is still pending") {
    REQUIRE(application.snapshot().log == app::LogState::Waiting);
  }
  SECTION("disconnect precedes shutdown while the initial start is pending") {
    REQUIRE(application.snapshot().log == app::LogState::Waiting);
    disconnect_before_shutdown = true;
  }

  REQUIRE(std::filesystem::create_directory(new_logs));
  REQUIRE(::chmod(new_logs.c_str(), S_IRWXU) == 0);
  REQUIRE(application.apply_logging({new_logs.string(), 25U, 128U, 16U}));
  serial_backend->inject_rx("last RX\n");
  REQUIRE(
      wait_until([&] { return serial_backend->read_bytes() == 8U; }, 500ms));
  if (disconnect_before_shutdown) {
    application.disconnect();
    REQUIRE(application.snapshot().connection ==
            app::ConnectionState::Disconnecting);
  }
  REQUIRE(harness.shutdown());

  const auto decoded = read_session(new_logs);
  CHECK(std::ranges::any_of(decoded->records, [](const auto &record) {
    return record.direction == logging::Direction::Rx &&
           record.payload.size() == 8U;
  }));
  CHECK(std::ranges::any_of(decoded->records, [](const auto &record) {
    return record.direction == logging::Direction::Sys &&
           record.message == "Serial session closed";
  }));
}

TEST_CASE("unexpected serial loss invalidates a periodic task before reconnect",
          "[application]") {
  auto serial = std::make_unique<FakeBackend>();
  auto *const serial_backend = serial.get();
  ApplicationHarness harness{std::move(serial)};
  auto &application = harness.app();

  REQUIRE(application.apply_quick_slot(
      1U, config::QuickSendSlot{1U, "Probe", config::SendMode::Txt, "AT",
                                config::Newline::None, ""}));
  REQUIRE(tick_until(application, [&] {
    return application.snapshot().quick_send.slots[0U].has_value();
  }));
  REQUIRE(harness.connect());
  REQUIRE(application.execute_quick(1U, 1000U));
  REQUIRE(tick_until(application, [&] {
    return application.snapshot().task.state ==
           lazycom::scheduler::SchedulerState::Running;
  }));

  serial_backend->fail_next_read();
  REQUIRE(tick_until(application, [&] {
    return application.snapshot().connection ==
               app::ConnectionState::Disconnected &&
           application.snapshot().task.state ==
               lazycom::scheduler::SchedulerState::Idle;
  }));
  REQUIRE(harness.connect());
  REQUIRE(application.snapshot().task.state ==
          lazycom::scheduler::SchedulerState::Idle);
  for (std::size_t index = 1U; index < application.snapshot().records.size();
       ++index) {
    REQUIRE(application.snapshot().records[index - 1U].record_id <
            application.snapshot().records[index].record_id);
  }
  REQUIRE(harness.shutdown());
}

TEST_CASE("disconnect drains more than 512 ordered RX events through cleanup",
          "[application]") {
  auto serial = std::make_unique<FakeBackend>();
  auto *const backend = serial.get();
  backend->set_maximum_read_size(1U);
  ApplicationHarness harness{std::move(serial)};
  auto &application = harness.app();
  REQUIRE(harness.connect());

  const std::string payload(600U, 'r');
  backend->inject_rx(payload);
  REQUIRE(wait_until([&] { return backend->read_bytes() == payload.size(); },
                     500ms));
  application.disconnect();
  REQUIRE(tick_until(application, [&] {
    return application.snapshot().connection ==
           app::ConnectionState::Disconnected;
  }));

  CHECK(application.snapshot().rx_bytes == payload.size());
  std::size_t received{};
  for (const auto &record : application.snapshot().records) {
    if (record.direction == app::RecordDirection::Rx) {
      received += record.payload.size();
    }
  }
  CHECK(received == payload.size());
  REQUIRE(harness.shutdown());
}

TEST_CASE("disconnect retains the written prefix of a partial TX",
          "[application]") {
  auto serial = std::make_unique<FakeBackend>();
  auto *const backend = serial.get();
  backend->set_write_limit(7U);
  ApplicationHarness harness{std::move(serial)};
  auto &application = harness.app();
  REQUIRE(harness.connect());
  application.set_draft(std::string(1024U, 'T'));
  application.submit_draft();
  REQUIRE(wait_until([&] { return backend->written_bytes() == 7U; }, 200ms));
  application.disconnect();
  REQUIRE(tick_until(application, [&] {
    return application.snapshot().connection ==
           app::ConnectionState::Disconnected;
  }));

  CHECK(application.snapshot().tx_bytes == 7U);
  CHECK(std::ranges::any_of(
      application.snapshot().records, [](const auto &record) {
        return record.direction == app::RecordDirection::Tx &&
               record.payload.size() == 7U;
      }));
  REQUIRE(harness.shutdown());
}

TEST_CASE("TX errors retain their code and operation identifier",
          "[application]") {
  auto serial = std::make_unique<FakeBackend>();
  auto *const backend = serial.get();
  ApplicationHarness harness{std::move(serial)};
  auto &application = harness.app();
  const auto log_directory = harness.paths.config.parent_path() / "logs";
  REQUIRE(std::filesystem::create_directory(log_directory));
  REQUIRE(::chmod(log_directory.c_str(), S_IRWXU) == 0);
  REQUIRE(application.apply_logging({log_directory.string(), 25U, 128U, 16U}));
  REQUIRE(tick_until(application, [&] {
    return application.snapshot().effective_log_directory ==
           log_directory.string();
  }));
  REQUIRE(harness.connect());
  REQUIRE(harness.start_logging());
  backend->fail_after_write_limit(3U);
  application.set_draft("failure");
  application.submit_draft();
  REQUIRE(tick_until(application, [&] {
    return application.snapshot().connection ==
           app::ConnectionState::Disconnected;
  }));
  const auto tx = std::ranges::find_if(
      application.snapshot().records, [](const auto &record) {
        return record.direction == app::RecordDirection::Tx &&
               record.payload.size() == 3U;
      });
  const auto error = std::ranges::find_if(
      application.snapshot().records, [](const auto &record) {
        return record.direction == app::RecordDirection::Error &&
               record.operation_id.has_value();
      });
  REQUIRE(tx != application.snapshot().records.end());
  REQUIRE(error != application.snapshot().records.end());
  CHECK(tx->sequence < error->sequence);
  CHECK(error->error_code == lazycom::ErrorCode::SerialDeviceGone);
  CHECK(error->operation_id->value != 0U);
  CHECK(std::ranges::count_if(
            application.snapshot().records, [&](const auto &record) {
              return record.direction == app::RecordDirection::Error &&
                     record.operation_id == error->operation_id;
            }) == 1);
  REQUIRE(harness.shutdown());
  const auto decoded = read_session(log_directory);
  const auto logged_tx =
      std::ranges::find_if(decoded->records, [](const auto &record) {
        return record.direction == logging::Direction::Tx &&
               record.payload.size() == 3U;
      });
  const auto logged_error =
      std::ranges::find_if(decoded->records, [](const auto &record) {
        return record.direction == logging::Direction::Err &&
               record.code == "LC-SER-2003";
      });
  REQUIRE(logged_tx != decoded->records.end());
  REQUIRE(logged_error != decoded->records.end());
  CHECK(logged_tx->seq < logged_error->seq);
  CHECK(std::ranges::count_if(decoded->records, [](const auto &record) {
          return record.direction == logging::Direction::Err &&
                 record.code == "LC-SER-2003";
        }) == 1);
}

TEST_CASE("record budget exhaustion stops logging and RX without a fatal error",
          "[application][logging][budget]") {
  auto limits = lazycom::model::BudgetLimits::defaults();
  limits.category[static_cast<std::size_t>(
      lazycom::model::BudgetCategory::UiRecords)] = 8U * 1024U;
  lazycom::model::GlobalMemoryBudget budget{limits};
  auto serial = std::make_unique<FakeBackend>();
  auto *backend = serial.get();
  ApplicationHarness harness{
      std::move(serial), std::make_unique<FakeBackend>(), "logs", {}, budget};
  auto &application = harness.app();
  REQUIRE(harness.connect());
  REQUIRE(harness.start_logging());
  backend->inject_rx(std::string(8U * 1024U, 'x') + "\n");
  REQUIRE(tick_until(application, [&] {
    return application.snapshot().connection ==
           app::ConnectionState::Disconnected;
  }));
  CHECK_FALSE(application.snapshot().fatal_stopping);
  CHECK(application.snapshot().log == app::LogState::Error);
  CHECK(application.snapshot().display_gap_records > 0U);
  REQUIRE(harness.shutdown());
  harness.application.reset();
  CHECK(budget.total_used() == 0U);
}

TEST_CASE("fatal worker signal stops accepting operations and fails shutdown",
          "[application][shutdown]") {
  auto serial = std::make_unique<FakeBackend>();
  auto *const backend = serial.get();
  ApplicationHarness harness{std::move(serial)};
  auto &application = harness.app();
  REQUIRE(harness.connect());
  backend->throw_next_read();
  REQUIRE(tick_until(
      application, [&] { return bool(application.snapshot().fatal_stopping); },
      500));
  REQUIRE(application.snapshot().fatal_stopping);
  REQUIRE(application.snapshot().shutting_down);
  CHECK_FALSE(application.apply_newline(config::Newline::Lf));
  const auto connection_before_rejected_operation =
      application.snapshot().connection;
  application.disconnect();
  CHECK(application.snapshot().connection ==
        connection_before_rejected_operation);
  CHECK_FALSE(harness.shutdown());
}

TEST_CASE("normal shutdown aborts instead of joining a timed-out scanner",
          "[application][shutdown]") {
  lazycom::test::TemporaryDirectory temporary;
  const auto root = temporary.path() / "app";
  const config::PersistencePaths paths{
      root / "config.toml", root / "quick_send.toml", root / "state.toml"};
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    static_cast<void>(std::signal(SIGABRT, SIG_DFL));
    if (!std::filesystem::create_directory(root) ||
        ::chmod(root.c_str(), S_IRWXU) != 0) {
      ::_exit(10);
    }
    {
      std::ofstream output{paths.config};
      if (!output) {
        ::_exit(11);
      }
      output << "version = 1\n[timeouts]\nowner_stop_ms = 100\n"
                "log_barrier_ms = 100\n";
    }
    if (::chmod(paths.config.c_str(), S_IRUSR | S_IWUSR) != 0) {
      ::_exit(12);
    }
    auto scanner =
        std::make_unique<FakeBackend>(std::vector<serial::DeviceInfo>{}, true);
    auto *const scanner_backend = scanner.get();
    app::ApplicationDependencies dependencies;
    dependencies.serial_backend = std::make_unique<FakeBackend>();
    dependencies.scanner_backend = std::move(scanner);
    dependencies.paths = paths;
    auto created = app::Application::create(std::move(dependencies));
    if (!created) {
      ::_exit(13);
    }
    if (!wait_until([&] { return scanner_backend->enumerate_entered(); })) {
      ::_exit(14);
    }
    static_cast<void>((*created)->shutdown());
    ::_exit(15);
  }

  int status = 0;
  bool exited = false;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{3};
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = ::waitpid(child, &status, WNOHANG);
    if (result == child) {
      exited = true;
      break;
    }
    REQUIRE(result >= 0);
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
  }
  if (!exited) {
    static_cast<void>(::kill(child, SIGKILL));
    static_cast<void>(::waitpid(child, &status, 0));
  }
  REQUIRE(exited);
  REQUIRE(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGABRT);
}

TEST_CASE("busy manual sending keeps periodic requests behind all manual work",
          "[application]") {
  using namespace std::chrono_literals;
  auto backend = std::make_unique<FakeBackend>();
  auto *const serial_backend = backend.get();
  ApplicationHarness harness{std::move(backend)};
  auto &application = harness.app();
  REQUIRE(application.apply_quick_slot(
      1U, config::QuickSendSlot{1U, "Quick", config::SendMode::Txt, "Q",
                                config::Newline::None, ""}));
  REQUIRE(tick_until(
      application,
      [&] { return bool(application.snapshot().quick_send.slots[0]); }, 500));
  REQUIRE(application.snapshot().quick_send.slots[0]);
  REQUIRE(harness.connect());
  serial_backend->set_write_limit(0U);
  application.set_draft("A");
  REQUIRE(application.submit_draft());
  REQUIRE(application.execute_quick(1U, 1000U));
  CHECK(application.snapshot().tx_pending == 1U);
  application.set_draft("B");
  REQUIRE(application.submit_draft());
  serial_backend->set_write_limit(100U);
  REQUIRE(tick_until(
      application,
      [&] { return !(serial_backend->written_payload().size() < 3U); }, 500));
  CHECK(serial_backend->written_payload() == "ABQ");
  REQUIRE(harness.shutdown());
}

TEST_CASE("repeated logging updates preserve a pending rotation and its tail",
          "[application][logging]") {
  using namespace std::chrono_literals;
  using app::ConnectionState;
  using app::LogApplyPolicy;
  using app::LogState;
  auto backend = std::make_unique<FakeBackend>();
  auto *const serial_backend = backend.get();
  ApplicationHarness harness{std::move(backend)};
  auto &application = harness.app();
  const auto root = harness.paths.config.parent_path();
  REQUIRE(std::filesystem::exists(root));
  REQUIRE(::chmod(root.c_str(), S_IRWXU) == 0);
  const auto old_logs = root / "old-logs";
  const auto next_logs = root / "next-logs";
  const auto final_logs = root / "final-logs";
  for (const auto &directory : {old_logs, next_logs, final_logs}) {
    REQUIRE(std::filesystem::create_directory(directory));
    REQUIRE(::chmod(directory.c_str(), S_IRWXU) == 0);
  }
  REQUIRE(application.apply_logging({old_logs.string(), 25U, 128U, 16U}));
  REQUIRE(harness.connect());
  REQUIRE(harness.start_logging());
  REQUIRE(application.apply_logging({next_logs.string(), 25U, 128U, 16U}));

  enum class CloseKind { Disconnect, DeviceFault, Shutdown };
  auto close_kind = CloseKind::Disconnect;
  auto second_policy = LogApplyPolicy::RotateNow;
  bool first_rotation_completed = false;
  SECTION("the first rotation completes before the second update") {
    REQUIRE(tick_until(application, [&] {
      return application.snapshot().log == LogState::Recording;
    }));
    first_rotation_completed = true;
  }
  SECTION("another immediate rotation followed by disconnect") {}
  SECTION("next-session settings cannot cancel an accepted rotation") {
    second_policy = LogApplyPolicy::NextSession;
  }
  SECTION("another immediate rotation followed by device loss") {
    close_kind = CloseKind::DeviceFault;
  }
  SECTION("another immediate rotation followed by shutdown") {
    close_kind = CloseKind::Shutdown;
  }
  REQUIRE(application.apply_logging({final_logs.string(), 25U, 128U, 16U},
                                    second_policy));
  serial_backend->inject_rx("after twice\n");
  REQUIRE(
      wait_until([&] { return serial_backend->read_bytes() == 12U; }, 500ms));
  if (close_kind == CloseKind::Shutdown) {
    REQUIRE(harness.shutdown());
  } else {
    if (close_kind == CloseKind::DeviceFault) {
      serial_backend->fail_next_read();
    } else {
      application.disconnect();
    }
    REQUIRE(tick_until(application, [&] {
      return application.snapshot().connection == ConnectionState::Disconnected;
    }));
    CHECK(application.snapshot().log == LogState::Waiting);
    REQUIRE(harness.shutdown());
  }
  CHECK_FALSE(read_session(old_logs)->incomplete_tail);
  if (first_rotation_completed)
    CHECK_FALSE(read_session(next_logs)->incomplete_tail);
  const auto decoded = read_session(final_logs);
  CHECK(std::ranges::any_of(decoded->records, [](const auto &record) {
    return record.direction == logging::Direction::Rx &&
           record.payload.size() == 12U;
  }));
  CHECK(std::ranges::any_of(decoded->records, [close_kind](const auto &record) {
    return close_kind == CloseKind::DeviceFault
               ? record.direction == logging::Direction::Err
               : record.direction == logging::Direction::Sys &&
                     record.message == "Serial session closed";
  }));
}
