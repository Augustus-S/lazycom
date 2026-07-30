#include <lazycom/app/application.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/eventfd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

class FakeBackend final : public lazycom::serial::ISerialBackend {
public:
  explicit FakeBackend(std::vector<lazycom::serial::DeviceInfo> devices = {})
      : devices_(std::move(devices)), wait_fd_(::eventfd(0U, EFD_NONBLOCK)) {
    if (wait_fd_ < 0) {
      throw std::runtime_error("cannot create fake serial eventfd");
    }
  }

  ~FakeBackend() override { static_cast<void>(::close(wait_fd_)); }

  lazycom::Result<std::vector<lazycom::serial::DeviceInfo>>
  enumerate() override {
    return devices_;
  }
  lazycom::Status open(const lazycom::serial::DevicePath &,
                       const lazycom::serial::PortConfig &) override {
    if (!open_rx_.empty()) {
      inject_rx(open_rx_);
    }
    return {};
  }
  lazycom::Result<int> native_wait_handle() const override { return wait_fd_; }
  lazycom::Result<std::size_t>
  read_some(const std::span<std::byte> destination) override {
    std::lock_guard lock(mutex_);
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
    const auto amount = std::min(destination.size(), pending_rx_.size());
    std::copy_n(pending_rx_.begin(), amount, destination.begin());
    pending_rx_.erase(pending_rx_.begin(),
                      pending_rx_.begin() +
                          static_cast<std::ptrdiff_t>(amount));
    std::uint64_t wake_count{};
    static_cast<void>(::read(wait_fd_, &wake_count, sizeof(wake_count)));
    return amount;
  }
  lazycom::Result<std::size_t>
  write_some(std::span<const std::byte> source) override {
    return source.size();
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

  void inject_rx_on_open(std::string bytes) { open_rx_ = std::move(bytes); }

private:
  std::vector<lazycom::serial::DeviceInfo> devices_;
  int wait_fd_;
  std::mutex mutex_;
  std::vector<std::byte> pending_rx_;
  std::string open_rx_;
  bool fail_read_{};
};

[[nodiscard]] lazycom::config::PersistencePaths test_paths() {
  static std::uint32_t sequence{};
  ++sequence;
  const auto root = std::filesystem::temp_directory_path() /
                    ("lazycom-application-unit-" +
                     std::to_string(static_cast<long long>(::getpid())) + "-" +
                     std::to_string(sequence));
  return {root / "config.toml", root / "quick_send.toml", root / "state.toml"};
}

} // namespace

TEST_CASE("application loads defaults and merges scanner completion") {
  lazycom::serial::DeviceInfo fake_device;
  fake_device.path = "/dev/ttyFAKE0";
  fake_device.description = "fake";
  lazycom::app::ApplicationDependencies dependencies;
  dependencies.serial_backend = std::make_unique<FakeBackend>();
  dependencies.scanner_backend = std::make_unique<FakeBackend>(
      std::vector<lazycom::serial::DeviceInfo>{std::move(fake_device)});
  dependencies.paths = test_paths();
  dependencies.log_directory =
      std::filesystem::temp_directory_path() / "lazycom-application-logs";

  auto created = lazycom::app::Application::create(std::move(dependencies));
  REQUIRE(created);
  auto &application = **created;
  for (int attempt = 0; attempt < 100 && application.snapshot().scanning;
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.snapshot().config.serial.baud == 115200);
  REQUIRE(application.snapshot().devices.size() == 1U);
  REQUIRE(application.snapshot().devices.front().path == "/dev/ttyFAKE0");
  REQUIRE(application.apply_baud("230400"));
  REQUIRE(application.snapshot().config.serial.baud == 230400);
  REQUIRE(application.apply_data_format("8,1,none,none"));
  REQUIRE_FALSE(application.apply_baud("0"));
  REQUIRE(application.shutdown());
}

TEST_CASE("application restores receive visibility from state") {
  const auto paths = test_paths();
  const auto root = paths.config.parent_path();
  REQUIRE(std::filesystem::create_directory(root));
  REQUIRE(::chmod(root.c_str(), S_IRWXU) == 0);
  {
    std::ofstream output(paths.state);
    REQUIRE(output);
    output << "version = 1\nshow_rx = false\nshow_tx = true\n"
              "show_system = false\nshow_error = true\n";
  }
  REQUIRE(::chmod(paths.state.c_str(), S_IRUSR | S_IWUSR) == 0);

  lazycom::app::ApplicationDependencies dependencies;
  dependencies.serial_backend = std::make_unique<FakeBackend>();
  dependencies.scanner_backend = std::make_unique<FakeBackend>();
  dependencies.paths = paths;
  auto created = lazycom::app::Application::create(std::move(dependencies));
  REQUIRE(created);
  const auto &filter = (*created)->snapshot().filter;
  CHECK_FALSE(filter.rx);
  CHECK(filter.tx);
  CHECK_FALSE(filter.system);
  CHECK(filter.error);
  REQUIRE((*created)->shutdown());
  created->reset();
  std::filesystem::remove_all(root);
}

TEST_CASE("disconnected draft is retained and cannot create TX") {
  lazycom::app::ApplicationDependencies dependencies;
  dependencies.serial_backend = std::make_unique<FakeBackend>();
  dependencies.scanner_backend = std::make_unique<FakeBackend>();
  dependencies.paths = test_paths();
  dependencies.log_directory =
      std::filesystem::temp_directory_path() / "lazycom-application-logs";
  auto created = lazycom::app::Application::create(std::move(dependencies));
  REQUIRE(created);
  auto &application = **created;
  application.set_draft("AT+INFO");
  application.submit_draft();
  REQUIRE(application.snapshot().draft == "AT+INFO");
  REQUIRE(application.snapshot().tx_bytes == 0U);
  REQUIRE(application.snapshot().notice.find("Not connected") !=
          std::string::npos);
  REQUIRE(application.shutdown());
}

TEST_CASE("port selection accepts only the latest scan result") {
  lazycom::serial::DeviceInfo device;
  device.path = "/dev/null";
  device.description = "test character device";
  lazycom::app::ApplicationDependencies dependencies;
  dependencies.serial_backend = std::make_unique<FakeBackend>();
  dependencies.scanner_backend = std::make_unique<FakeBackend>(
      std::vector<lazycom::serial::DeviceInfo>{std::move(device)});
  dependencies.paths = test_paths();
  auto created = lazycom::app::Application::create(std::move(dependencies));
  REQUIRE(created);
  auto &application = **created;
  for (int attempt = 0; attempt < 100 && application.snapshot().scanning;
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.apply_port("/dev/null"));
  CHECK_FALSE(application.apply_port("/dev/zero"));
  REQUIRE(application.shutdown());
}

TEST_CASE("receive data published with connect completion is retained") {
  auto serial = std::make_unique<FakeBackend>();
  serial->inject_rx_on_open("ready\n");
  lazycom::app::ApplicationDependencies dependencies;
  dependencies.serial_backend = std::move(serial);
  dependencies.scanner_backend = std::make_unique<FakeBackend>();
  dependencies.paths = test_paths();
  auto created = lazycom::app::Application::create(std::move(dependencies));
  REQUIRE(created);
  auto &application = **created;
  application.set_device_path("/dev/null");
  application.connection_control();
  for (int attempt = 0;
       attempt < 200 && std::ranges::none_of(
                            application.snapshot().records,
                            [](const auto &record) {
                              return record.direction ==
                                     lazycom::app::RecordDirection::Rx;
                            });
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  CHECK(std::ranges::any_of(
      application.snapshot().records, [](const auto &record) {
        return record.direction == lazycom::app::RecordDirection::Rx;
      }));
  REQUIRE(application.shutdown());
}

TEST_CASE("send history enforces its configured byte limit") {
  const auto paths = test_paths();
  const auto root = paths.config.parent_path();
  REQUIRE(std::filesystem::create_directory(root));
  REQUIRE(::chmod(root.c_str(), S_IRWXU) == 0);
  {
    std::ofstream output(paths.config);
    REQUIRE(output);
    output << "version = 1\n[send]\nhistory_max_mib = 1\n";
  }
  REQUIRE(::chmod(paths.config.c_str(), S_IRUSR | S_IWUSR) == 0);

  lazycom::app::ApplicationDependencies dependencies;
  dependencies.serial_backend = std::make_unique<FakeBackend>();
  dependencies.scanner_backend = std::make_unique<FakeBackend>();
  dependencies.paths = paths;
  auto created = lazycom::app::Application::create(std::move(dependencies));
  REQUIRE(created);
  auto &application = **created;
  application.set_device_path("/dev/null");
  application.connection_control();
  for (int attempt = 0;
       attempt < 100 && application.snapshot().connection !=
                            lazycom::app::ConnectionState::Connected;
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.snapshot().connection ==
          lazycom::app::ConnectionState::Connected);

  for (const char marker : {'A', 'B'}) {
    application.set_draft(std::string(600'000U, marker));
    application.submit_draft();
    CHECK(application.snapshot().draft.empty());
    for (int attempt = 0;
         attempt < 200 && application.snapshot().tx_pending != 0U; ++attempt) {
      application.tick();
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(application.snapshot().tx_pending == 0U);
  }
  application.set_draft("sentinel");
  application.history_previous();
  REQUIRE(application.snapshot().draft.front() == 'B');
  application.history_previous();
  CHECK(application.snapshot().draft.front() == 'B');

  REQUIRE(application.shutdown());
  created->reset();
  std::filesystem::remove_all(root);
}

TEST_CASE("rapid config edits persist the latest combined snapshot") {
  const auto paths = test_paths();
  lazycom::app::ApplicationDependencies dependencies;
  dependencies.serial_backend = std::make_unique<FakeBackend>();
  dependencies.scanner_backend = std::make_unique<FakeBackend>();
  dependencies.paths = paths;
  auto created = lazycom::app::Application::create(std::move(dependencies));
  REQUIRE(created);
  auto &application = **created;

  REQUIRE(application.apply_baud("230400"));
  REQUIRE(application.apply_data_format("7,2,even,none"));
  REQUIRE(application.apply_newline("crlf"));
  bool persisted = false;
  for (int attempt = 0; attempt < 200 && !persisted; ++attempt) {
    application.tick();
    const auto loaded = lazycom::config::load_config_toml(paths.config);
    persisted =
        loaded.accepted && loaded.snapshot.serial.baud == 230400 &&
        loaded.snapshot.serial.data_bits == 7 &&
        loaded.snapshot.serial.stop_bits == 2 &&
        loaded.snapshot.serial.parity == lazycom::config::Parity::Even &&
        loaded.snapshot.send.newline == lazycom::config::Newline::CrLf;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  CHECK(persisted);
  REQUIRE(application.shutdown());
  created->reset();
  std::filesystem::remove_all(paths.config.parent_path());
}

TEST_CASE("startup scan publishes one alert when every device is denied") {
  lazycom::serial::DeviceInfo denied;
  denied.path = "/dev/ttyDENIED0";
  denied.description = "denied";
  denied.permission.access = lazycom::serial::DeviceAccess::PermissionDenied;
  denied.permission.owner_uid = 0U;
  denied.permission.owner_gid = 18U;
  denied.permission.mode = 0660U;
  denied.permission.group_name = "dialout";

  lazycom::app::ApplicationDependencies dependencies;
  dependencies.serial_backend = std::make_unique<FakeBackend>();
  dependencies.scanner_backend = std::make_unique<FakeBackend>(
      std::vector<lazycom::serial::DeviceInfo>{std::move(denied)});
  dependencies.paths = test_paths();
  auto created = lazycom::app::Application::create(std::move(dependencies));
  REQUIRE(created);
  auto &application = **created;

  for (int attempt = 0; attempt < 100 && !application.snapshot().alert;
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.snapshot().alert);
  CHECK(application.snapshot().alert->code ==
        lazycom::ErrorCode::SerialPermissionDenied);
  CHECK(application.snapshot().alert->message.find("dialout") !=
        std::string::npos);
  application.dismiss_alert();
  CHECK_FALSE(application.snapshot().alert);
  application.request_scan();
  for (int attempt = 0; attempt < 100 && application.snapshot().scanning;
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  CHECK_FALSE(application.snapshot().alert);
  REQUIRE(application.shutdown());
}

TEST_CASE("logging defaults rebuild an inactive writer") {
  lazycom::app::ApplicationDependencies dependencies;
  dependencies.serial_backend = std::make_unique<FakeBackend>();
  dependencies.scanner_backend = std::make_unique<FakeBackend>();
  dependencies.paths = test_paths();
  dependencies.log_directory = std::filesystem::temp_directory_path() /
                               "lazycom-application-reconfigured-logs";
  const auto default_logs = *dependencies.log_directory;
  auto created = lazycom::app::Application::create(std::move(dependencies));
  REQUIRE(created);
  auto &application = **created;

  REQUIRE(application.apply_logging("|25|128|16"));
  REQUIRE(application.snapshot().config.logging.directory.empty());
  CHECK(application.snapshot().effective_log_directory ==
        default_logs.string());
  REQUIRE(application.snapshot().config.logging.max_files == 25U);
  REQUIRE(application.snapshot().config.logging.max_total_size_mib == 128U);
  REQUIRE(application.snapshot().config.logging.max_file_size_mib == 16U);
  REQUIRE_FALSE(application.apply_logging("relative|25|128|16"));

  application.toggle_log();
  REQUIRE(application.snapshot().log == lazycom::app::LogState::Waiting);
  application.toggle_log();
  for (int attempt = 0; attempt < 100 && application.snapshot().log !=
                                             lazycom::app::LogState::Off;
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.snapshot().log == lazycom::app::LogState::Off);
  REQUIRE(application.shutdown());
}

TEST_CASE("logging settings roll an active file onto the new snapshot") {
  const auto paths = test_paths();
  const auto root = paths.config.parent_path();
  REQUIRE(std::filesystem::create_directory(root));
  REQUIRE(::chmod(root.c_str(), S_IRWXU) == 0);
  const auto old_logs = root / "old-logs";
  const auto new_logs = root / "new-logs";

  lazycom::app::ApplicationDependencies dependencies;
  dependencies.serial_backend = std::make_unique<FakeBackend>();
  dependencies.scanner_backend = std::make_unique<FakeBackend>();
  dependencies.paths = paths;
  dependencies.log_directory = old_logs;
  auto created = lazycom::app::Application::create(std::move(dependencies));
  REQUIRE(created);
  auto &application = **created;

  application.set_device_path("/dev/null");
  application.connection_control();
  for (int attempt = 0;
       attempt < 100 && application.snapshot().connection !=
                            lazycom::app::ConnectionState::Connected;
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.snapshot().connection ==
          lazycom::app::ConnectionState::Connected);

  application.toggle_log();
  for (int attempt = 0; attempt < 100 && application.snapshot().log !=
                                             lazycom::app::LogState::Recording;
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.snapshot().log == lazycom::app::LogState::Recording);
  REQUIRE(std::filesystem::exists(old_logs));

  REQUIRE(std::filesystem::create_directory(new_logs));
  REQUIRE(::chmod(new_logs.c_str(), S_IRWXU) == 0);
  REQUIRE(application.apply_logging(new_logs.string() + "|25|128|16"));
  for (int attempt = 0;
       attempt < 200 &&
       (application.snapshot().log != lazycom::app::LogState::Recording ||
        !std::filesystem::exists(new_logs));
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.snapshot().log == lazycom::app::LogState::Recording);
  REQUIRE(std::filesystem::exists(new_logs));
  const auto session_file_count = [](const std::filesystem::path &directory) {
    return static_cast<std::size_t>(std::ranges::count_if(
        std::filesystem::directory_iterator{directory}, [](const auto &entry) {
          return entry.path().filename().string().starts_with(
              "lazycom-session-");
        }));
  };
  REQUIRE(session_file_count(old_logs) == 1U);
  REQUIRE(session_file_count(new_logs) == 1U);
  REQUIRE(application.shutdown());
  created->reset();
  std::filesystem::remove_all(root);
}

TEST_CASE("logging settings require an existing safe directory") {
  const auto paths = test_paths();
  const auto root = paths.config.parent_path();
  REQUIRE(std::filesystem::create_directory(root));
  REQUIRE(::chmod(root.c_str(), S_IRWXU) == 0);

  lazycom::app::ApplicationDependencies dependencies;
  dependencies.serial_backend = std::make_unique<FakeBackend>();
  dependencies.scanner_backend = std::make_unique<FakeBackend>();
  dependencies.paths = paths;
  dependencies.log_directory = root / "default-logs";
  auto created = lazycom::app::Application::create(std::move(dependencies));
  REQUIRE(created);
  auto &application = **created;

  const auto missing = root / "missing";
  REQUIRE_FALSE(application.validate_logging(missing.string() + "|25|128|16"));
  REQUIRE_FALSE(application.apply_logging(missing.string() + "|25|128|16"));

  const auto regular_file = root / "not-a-directory";
  {
    std::ofstream output(regular_file);
    REQUIRE(output);
  }
  REQUIRE(::chmod(regular_file.c_str(), S_IRUSR | S_IWUSR) == 0);
  REQUIRE_FALSE(
      application.validate_logging(regular_file.string() + "|25|128|16"));

  const auto directory = root / "logs";
  REQUIRE(std::filesystem::create_directory(directory));
  REQUIRE(::chmod(directory.c_str(), S_IRWXU) == 0);
  REQUIRE(application.validate_logging(directory.string() + "|25|128|16"));
  REQUIRE(application.apply_logging(directory.string() + "|25|128|16",
                                    lazycom::app::LogApplyPolicy::NextSession));
  CHECK(application.snapshot().config.logging.directory == directory.string());

  REQUIRE(application.shutdown());
  created->reset();
  std::filesystem::remove_all(root);
}

TEST_CASE("application exposes the active serial configuration snapshot") {
  const auto paths = test_paths();
  lazycom::app::ApplicationDependencies dependencies;
  dependencies.serial_backend = std::make_unique<FakeBackend>();
  dependencies.scanner_backend = std::make_unique<FakeBackend>();
  dependencies.paths = paths;
  dependencies.log_directory = paths.config.parent_path() / "logs";

  auto created = lazycom::app::Application::create(std::move(dependencies));
  REQUIRE(created);
  auto &application = **created;

  REQUIRE_FALSE(application.snapshot().active_port_config);
  application.set_device_path("/dev/null");
  REQUIRE(application.apply_baud("230400"));
  REQUIRE(application.apply_data_format("7,2,even,none"));

  application.connection_control();
  REQUIRE(application.snapshot().active_port_config);
  CHECK(application.snapshot().active_port_config->baud == 230400);
  CHECK(application.snapshot().active_port_config->data_bits == 7);
  CHECK(application.snapshot().active_port_config->stop_bits == 2);
  CHECK(application.snapshot().active_port_config->parity ==
        lazycom::config::Parity::Even);

  for (int attempt = 0;
       attempt < 100 && application.snapshot().connection !=
                            lazycom::app::ConnectionState::Connected;
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.snapshot().connection ==
          lazycom::app::ConnectionState::Connected);
  REQUIRE(application.snapshot().active_port_config);

  application.connection_control();
  REQUIRE(application.snapshot().active_port_config);
  for (int attempt = 0;
       attempt < 100 && application.snapshot().connection !=
                            lazycom::app::ConnectionState::Disconnected;
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.snapshot().connection ==
          lazycom::app::ConnectionState::Disconnected);
  REQUIRE_FALSE(application.snapshot().active_port_config);
  REQUIRE(application.shutdown());
  created->reset();
  std::filesystem::remove_all(paths.config.parent_path());
}

TEST_CASE("quick-send deletion changes memory only after persistence commits") {
  lazycom::app::ApplicationDependencies dependencies;
  dependencies.serial_backend = std::make_unique<FakeBackend>();
  dependencies.scanner_backend = std::make_unique<FakeBackend>();
  dependencies.paths = test_paths();
  dependencies.log_directory =
      std::filesystem::temp_directory_path() / "lazycom-application-logs";
  auto created = lazycom::app::Application::create(std::move(dependencies));
  REQUIRE(created);
  auto &application = **created;

  REQUIRE(application.apply_quick_slot("3|Ping|txt|AT|crlf|probe"));
  REQUIRE_FALSE(application.snapshot().quick_send.slots[2U]);
  for (int attempt = 0;
       attempt < 100 && !application.snapshot().quick_send.slots[2U];
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.snapshot().quick_send.slots[2U]);
  REQUIRE(application.snapshot().quick_send.slots[2U]->content == "AT");

  REQUIRE(application.apply_quick_slot("3|||||"));
  REQUIRE(application.snapshot().quick_send.slots[2U]);
  for (int attempt = 0;
       attempt < 100 && application.snapshot().quick_send.slots[2U];
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE_FALSE(application.snapshot().quick_send.slots[2U]);
  REQUIRE(application.shutdown());
}

TEST_CASE("search direction is independent from the display filter") {
  auto serial = std::make_unique<FakeBackend>();
  auto *const serial_backend = serial.get();
  lazycom::app::ApplicationDependencies dependencies;
  dependencies.serial_backend = std::move(serial);
  dependencies.scanner_backend = std::make_unique<FakeBackend>();
  const auto paths = test_paths();
  dependencies.paths = paths;
  dependencies.log_directory =
      std::filesystem::temp_directory_path() / "lazycom-application-logs";
  auto created = lazycom::app::Application::create(std::move(dependencies));
  REQUIRE(created);
  auto &application = **created;

  application.set_device_path("/dev/null");
  application.connection_control();
  for (int attempt = 0;
       attempt < 100 && application.snapshot().connection !=
                            lazycom::app::ConnectionState::Connected;
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.snapshot().connection ==
          lazycom::app::ConnectionState::Connected);

  serial_backend->inject_rx("needle\n");
  for (int attempt = 0;
       attempt < 200 && std::ranges::none_of(
                            application.snapshot().records,
                            [](const auto &record) {
                              return record.direction ==
                                     lazycom::app::RecordDirection::Rx;
                            });
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.apply_view("txt,hex,0,1,1,1"));
  REQUIRE_FALSE(application.apply_view("txt,hex,0,0,0,0"));
  const lazycom::app::DirectionFilter rx_only{true, false, false, false};
  const lazycom::app::DirectionFilter tx_only{false, true, false, false};
  const auto rx_matches = application.search("needle", rx_only);
  REQUIRE(rx_matches.size() == 1U);
  const auto rx_record = std::ranges::find_if(
      application.snapshot().records, [](const auto &record) {
        return record.direction == lazycom::app::RecordDirection::Rx;
      });
  REQUIRE(rx_record != application.snapshot().records.end());
  REQUIRE(rx_matches.front() == rx_record->record_id);
  REQUIRE(application.search("needle", tx_only).empty());

  REQUIRE(application.apply_view("hex,mixed,1,0,1,0"));
  CHECK(application.render_record(*rx_record).find("6E 65 65 64 6C 65") !=
        std::string::npos);
  lazycom::app::VisibleRecord tx_record;
  tx_record.direction = lazycom::app::RecordDirection::Tx;
  tx_record.payload = {std::byte{0x41}};
  CHECK(application.render_record(tx_record).find("A | 41") !=
        std::string::npos);
  lazycom::app::VisibleRecord system_record;
  system_record.direction = lazycom::app::RecordDirection::System;
  system_record.message = "connected";
  CHECK(application.render_record(system_record).ends_with("connected"));
  REQUIRE(application.shutdown());
  created->reset();
  const auto persisted_state = lazycom::config::load_state_toml(paths.state);
  REQUIRE(persisted_state.accepted);
  CHECK(persisted_state.snapshot.show_rx);
  CHECK_FALSE(persisted_state.snapshot.show_tx);
  CHECK(persisted_state.snapshot.show_system);
  CHECK_FALSE(persisted_state.snapshot.show_error);
  std::filesystem::remove_all(paths.config.parent_path());
}

TEST_CASE(
    "unexpected serial loss invalidates a periodic task before reconnect") {
  auto serial = std::make_unique<FakeBackend>();
  auto *const serial_backend = serial.get();
  lazycom::app::ApplicationDependencies dependencies;
  dependencies.serial_backend = std::move(serial);
  dependencies.scanner_backend = std::make_unique<FakeBackend>();
  const auto paths = test_paths();
  dependencies.paths = paths;
  dependencies.log_directory =
      std::filesystem::temp_directory_path() / "lazycom-application-logs";
  auto created = lazycom::app::Application::create(std::move(dependencies));
  REQUIRE(created);
  auto &application = **created;

  REQUIRE(application.apply_quick_slot("1|Probe|txt|AT|none|"));
  for (int attempt = 0;
       attempt < 100 && !application.snapshot().quick_send.slots[0U];
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.snapshot().quick_send.slots[0U]);
  application.set_device_path("/dev/null");
  application.connection_control();
  for (int attempt = 0;
       attempt < 100 && application.snapshot().connection !=
                            lazycom::app::ConnectionState::Connected;
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.execute_quick(1U, 1000U));
  REQUIRE(application.snapshot().task.state ==
          lazycom::scheduler::SchedulerState::Running);

  serial_backend->fail_next_read();
  for (int attempt = 0;
       attempt < 200 && (application.snapshot().connection !=
                             lazycom::app::ConnectionState::Disconnected ||
                         application.snapshot().task.state !=
                             lazycom::scheduler::SchedulerState::Idle);
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.snapshot().connection ==
          lazycom::app::ConnectionState::Disconnected);
  REQUIRE(application.snapshot().task.state ==
          lazycom::scheduler::SchedulerState::Idle);

  application.connection_control();
  for (int attempt = 0;
       attempt < 100 && application.snapshot().connection !=
                            lazycom::app::ConnectionState::Connected;
       ++attempt) {
    application.tick();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  REQUIRE(application.snapshot().connection ==
          lazycom::app::ConnectionState::Connected);
  REQUIRE(application.snapshot().task.state ==
          lazycom::scheduler::SchedulerState::Idle);
  for (std::size_t index = 1U; index < application.snapshot().records.size();
       ++index) {
    REQUIRE(application.snapshot().records[index - 1U].record_id <
            application.snapshot().records[index].record_id);
  }
  REQUIRE(application.shutdown());
  created->reset();
  std::filesystem::remove_all(paths.config.parent_path());
}
