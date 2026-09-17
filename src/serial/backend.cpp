#include <lazycom/serial/backend.hpp>
#include <lazycom/serial/libserialport_probe.hpp>

#include <libserialport.h>

#include <cerrno>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <grp.h>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace lazycom::serial {
namespace detail {
void ensure_libserialport_initialized() noexcept;
}
namespace {

void silent_sp_debug_handler(const char *, ...) {
  const int saved_errno = errno;
  errno = saved_errno;
}

void initialize_libserialport() noexcept {
  const int saved_errno = errno;
  static const bool initialized = [] {
    sp_set_debug_handler(silent_sp_debug_handler);
    return true;
  }();
  static_cast<void>(initialized);
  errno = saved_errno;
}

[[nodiscard]] DeviceIdentity identity_from(const struct stat &value) noexcept {
  return {
      static_cast<std::uint64_t>(value.st_dev),
      static_cast<std::uint64_t>(value.st_ino),
      static_cast<std::uint64_t>(value.st_rdev),
  };
}

[[nodiscard]] std::string group_name(const gid_t group_id) {
  long requested = ::sysconf(_SC_GETGR_R_SIZE_MAX);
  if (requested < 1024L) {
    requested = 1024L;
  }
  const auto size = static_cast<std::size_t>(std::min(requested, 65536L));
  std::vector<char> buffer(size);
  struct group value{};
  struct group *result = nullptr;
  if (::getgrgid_r(group_id, &value, buffer.data(), buffer.size(), &result) ==
          0 &&
      result != nullptr && result->gr_name != nullptr) {
    return result->gr_name;
  }
  return std::to_string(static_cast<std::uint64_t>(group_id));
}

[[nodiscard]] ErrorCode serial_error_code(const int os_error) noexcept {
  switch (os_error) {
  case EACCES:
  case EPERM:
    return ErrorCode::SerialPermissionDenied;
  case EBUSY:
  case EAGAIN:
    return ErrorCode::SerialPortBusy;
  case ENODEV:
  case ENOENT:
  case EIO:
  case ENXIO:
    return ErrorCode::SerialDeviceGone;
  default:
    return ErrorCode::SerialDeviceGone;
  }
}

[[nodiscard]] Error adapt_sp_error(const int result, const Operation operation,
                                   const std::string_view context) {
  int os_error = 0;
  std::string message;
  if (result == SP_ERR_FAIL) {
    // These must be the first libserialport calls after the failing call.
    os_error = sp_last_error_code();
    char *const raw_message = sp_last_error_message();
    if (raw_message != nullptr) {
      message.assign(raw_message);
      sp_free_error_message(raw_message);
    }
  }

  ErrorCode code = ErrorCode::SerialDeviceGone;
  if (result == SP_ERR_ARG) {
    code = ErrorCode::ValidationInvalidValue;
  } else if (result == SP_ERR_SUPP) {
    code = ErrorCode::SerialUnsupported;
  } else if (result == SP_ERR_MEM) {
    code = ErrorCode::InternalOutOfMemory;
  } else if (result == SP_ERR_FAIL) {
    code = serial_error_code(os_error);
  }

  std::string detail{context};
  if (!message.empty()) {
    detail.append(": ");
    detail.append(message);
  } else if (result == SP_ERR_SUPP) {
    detail.append(": unsupported by libserialport or the device");
  } else if (result == SP_ERR_MEM) {
    detail.append(": libserialport allocation failed");
  } else if (result == SP_ERR_ARG) {
    detail.append(": libserialport rejected an argument");
  }
  return make_error(code, operation, detail,
                    os_error == 0
                        ? std::error_code{}
                        : std::error_code{os_error, std::generic_category()});
}

[[nodiscard]] Error posix_error(const int error, const Operation operation,
                                const std::string_view detail) {
  return make_error(serial_error_code(error), operation, detail,
                    std::error_code{error, std::generic_category()});
}

[[nodiscard]] std::string copied(const char *const value) {
  return value == nullptr ? std::string{} : std::string{value};
}

[[nodiscard]] DeviceTransport
transport_from(const sp_transport value) noexcept {
  switch (value) {
  case SP_TRANSPORT_NATIVE:
    return DeviceTransport::Native;
  case SP_TRANSPORT_USB:
    return DeviceTransport::Usb;
  case SP_TRANSPORT_BLUETOOTH:
    return DeviceTransport::Bluetooth;
  }
  return DeviceTransport::Unknown;
}

[[nodiscard]] Result<sp_parity>
parity_from(const config::Parity parity) noexcept {
  switch (parity) {
  case config::Parity::None:
    return SP_PARITY_NONE;
  case config::Parity::Odd:
    return SP_PARITY_ODD;
  case config::Parity::Even:
    return SP_PARITY_EVEN;
  case config::Parity::Mark:
    return SP_PARITY_MARK;
  case config::Parity::Space:
    return SP_PARITY_SPACE;
  }
  return tl::unexpected(make_error(ErrorCode::ValidationInvalidValue,
                                   Operation::ConfigureSerial,
                                   "unknown parity value"));
}

[[nodiscard]] Result<sp_flowcontrol>
flow_from(const config::FlowControl flow) noexcept {
  switch (flow) {
  case config::FlowControl::None:
    return SP_FLOWCONTROL_NONE;
  case config::FlowControl::RtsCts:
    return SP_FLOWCONTROL_RTSCTS;
  case config::FlowControl::XonXoff:
    return SP_FLOWCONTROL_XONXOFF;
  }
  return tl::unexpected(make_error(ErrorCode::ValidationInvalidValue,
                                   Operation::ConfigureSerial,
                                   "unknown flow-control value"));
}

class PortList final {
public:
  explicit PortList(sp_port **ports) noexcept : ports_(ports) {}
  ~PortList() {
    if (ports_ != nullptr) {
      sp_free_port_list(ports_);
    }
  }
  PortList(const PortList &) = delete;
  PortList &operator=(const PortList &) = delete;
  [[nodiscard]] sp_port **get() const noexcept { return ports_; }

private:
  sp_port **ports_;
};

class PortConfigGuard final {
public:
  explicit PortConfigGuard(sp_port_config *config) noexcept : config_(config) {}
  ~PortConfigGuard() {
    if (config_ != nullptr) {
      sp_free_config(config_);
    }
  }
  PortConfigGuard(const PortConfigGuard &) = delete;
  PortConfigGuard &operator=(const PortConfigGuard &) = delete;
  [[nodiscard]] sp_port_config *get() const noexcept { return config_; }

private:
  sp_port_config *config_;
};

void discard_sp_error(const enum sp_return result) noexcept {
  if (result != SP_ERR_FAIL) {
    return;
  }
  static_cast<void>(sp_last_error_code());
  char *const message = sp_last_error_message();
  if (message != nullptr) {
    sp_free_error_message(message);
  }
}

class OpenPortGuard final {
public:
  explicit OpenPortGuard(sp_port *port) noexcept : port_(port) {}
  ~OpenPortGuard() {
    if (open_) {
      discard_sp_error(sp_close(port_));
    }
    if (port_ != nullptr) {
      sp_free_port(port_);
    }
  }
  OpenPortGuard(const OpenPortGuard &) = delete;
  OpenPortGuard &operator=(const OpenPortGuard &) = delete;

  [[nodiscard]] sp_port *get() const noexcept { return port_; }
  void mark_open() noexcept { open_ = true; }
  [[nodiscard]] sp_port *release() noexcept {
    open_ = false;
    return std::exchange(port_, nullptr);
  }

private:
  sp_port *port_{};
  bool open_{};
};

} // namespace

void detail::ensure_libserialport_initialized() noexcept {
  initialize_libserialport();
}

PortConfig
PortConfig::from_defaults(const config::SerialDefaults &defaults) noexcept {
  return {defaults.baud, defaults.data_bits, defaults.stop_bits,
          defaults.parity, defaults.flow_control};
}

Result<DevicePath> inspect_device_path(const std::string_view absolute_path) {
  if (absolute_path.empty() || absolute_path.front() != '/' ||
      absolute_path.find('\0') != std::string_view::npos) {
    return tl::unexpected(make_error(ErrorCode::ValidationInvalidValue,
                                     Operation::OpenSerial,
                                     "serial device path must be absolute"));
  }

  const std::string requested{absolute_path};
  errno = 0;
  std::unique_ptr<char, decltype(&std::free)> resolved{
      ::realpath(requested.c_str(), nullptr), &std::free};
  if (resolved == nullptr) {
    const int error = errno;
    return tl::unexpected(posix_error(error, Operation::OpenSerial,
                                      "cannot resolve device path"));
  }
  std::string canonical{resolved.get()};

  struct stat value{};
  if (::stat(canonical.c_str(), &value) != 0) {
    const int error = errno;
    return tl::unexpected(posix_error(error, Operation::OpenSerial,
                                      "cannot inspect device path"));
  }
  if (!S_ISCHR(value.st_mode)) {
    return tl::unexpected(
        make_error(ErrorCode::ValidationInvalidValue, Operation::OpenSerial,
                   "serial device path is not a character device"));
  }
  return DevicePath{requested, std::move(canonical), identity_from(value)};
}

Result<DevicePermission>
inspect_device_permission(const std::string_view absolute_path) {
  const auto inspected = inspect_device_path(absolute_path);
  if (!inspected) {
    return tl::unexpected(inspected.error());
  }
  struct stat value{};
  if (::stat(inspected->canonical.c_str(), &value) != 0) {
    const int error = errno;
    return tl::unexpected(posix_error(error, Operation::OpenSerial,
                                      "cannot inspect serial permissions"));
  }
  DevicePermission permission;
  permission.owner_uid = static_cast<std::uint32_t>(value.st_uid);
  permission.owner_gid = static_cast<std::uint32_t>(value.st_gid);
  permission.mode = static_cast<std::uint32_t>(value.st_mode & 0777);
  permission.group_name = group_name(value.st_gid);
  if (::faccessat(AT_FDCWD, inspected->canonical.c_str(), R_OK | W_OK,
                  AT_EACCESS) == 0) {
    permission.access = DeviceAccess::Allowed;
    return permission;
  }
  const int error = errno;
  if (error == EACCES || error == EPERM) {
    permission.access = DeviceAccess::PermissionDenied;
    return permission;
  }
  return tl::unexpected(posix_error(error, Operation::OpenSerial,
                                    "cannot check serial permissions"));
}

struct LibserialportBackend::Impl {
  sp_port *port{};
  int wait_fd{-1};
  bool open{};
};

LibserialportBackend::LibserialportBackend() : impl_(std::make_unique<Impl>()) {
  detail::ensure_libserialport_initialized();
}

LibserialportBackend::~LibserialportBackend() {
  if (impl_->open) {
    discard_sp_error(sp_close(impl_->port));
  }
  if (impl_->port != nullptr) {
    sp_free_port(impl_->port);
  }
}

Result<std::vector<DeviceInfo>> LibserialportBackend::enumerate() {
  sp_port **raw_ports = nullptr;
  const auto result = sp_list_ports(&raw_ports);
  if (result != SP_OK) {
    return tl::unexpected(adapt_sp_error(result, Operation::EnumerateDevices,
                                         "port enumeration failed"));
  }
  PortList ports{raw_ports};
  std::vector<DeviceInfo> devices;
  for (sp_port **entry = ports.get(); entry != nullptr && *entry != nullptr;
       ++entry) {
    DeviceInfo device;
    device.path = copied(sp_get_port_name(*entry));
    device.description = copied(sp_get_port_description(*entry));
    const auto transport = sp_get_port_transport(*entry);
    device.transport = transport_from(transport);
    if (transport == SP_TRANSPORT_USB) {
      int vendor = 0;
      int product = 0;
      const auto id_result = sp_get_port_usb_vid_pid(*entry, &vendor, &product);
      if (id_result == SP_ERR_FAIL) {
        return tl::unexpected(
            adapt_sp_error(id_result, Operation::EnumerateDevices,
                           "cannot copy USB serial device identifiers"));
      }
      if (id_result == SP_OK && vendor >= 0 && product >= 0 &&
          vendor <= UINT16_MAX && product <= UINT16_MAX) {
        device.usb_vendor_id = static_cast<std::uint16_t>(vendor);
        device.usb_product_id = static_cast<std::uint16_t>(product);
      }
      device.usb_manufacturer = copied(sp_get_port_usb_manufacturer(*entry));
      device.usb_product = copied(sp_get_port_usb_product(*entry));
      device.usb_serial = copied(sp_get_port_usb_serial(*entry));
    } else if (transport == SP_TRANSPORT_BLUETOOTH) {
      device.bluetooth_address = copied(sp_get_port_bluetooth_address(*entry));
    }

    const auto inspected = inspect_device_path(device.path);
    if (inspected) {
      device.identity = inspected->identity;
    }
    const auto permission = inspect_device_permission(device.path);
    if (permission) {
      device.permission = *permission;
    }
    devices.push_back(std::move(device));
  }
  return devices;
}

Status LibserialportBackend::open(const DevicePath &path,
                                  const PortConfig &configuration) {
  if (impl_->port != nullptr || impl_->open) {
    return tl::unexpected(make_error(ErrorCode::SerialPortBusy,
                                     Operation::OpenSerial,
                                     "backend already owns an open port"));
  }
  if (configuration.baud < 1 || configuration.baud > INT_MAX ||
      configuration.data_bits < 5 || configuration.data_bits > 8 ||
      (configuration.stop_bits != 1 && configuration.stop_bits != 2)) {
    return tl::unexpected(make_error(ErrorCode::ValidationInvalidValue,
                                     Operation::ConfigureSerial,
                                     "invalid serial port configuration"));
  }
  const auto parity = parity_from(configuration.parity);
  if (!parity) {
    return tl::unexpected(parity.error());
  }
  const auto flow = flow_from(configuration.flow_control);
  if (!flow) {
    return tl::unexpected(flow.error());
  }

  const auto current_path = inspect_device_path(path.requested);
  if (!current_path) {
    return tl::unexpected(current_path.error());
  }
  if (current_path->identity != path.identity) {
    return tl::unexpected(make_error(ErrorCode::SerialDeviceGone,
                                     Operation::OpenSerial,
                                     "device identity changed before open"));
  }

  sp_port *port = nullptr;
  auto result = sp_get_port_by_name(path.requested.c_str(), &port);
  if (result != SP_OK) {
    return tl::unexpected(adapt_sp_error(result, Operation::OpenSerial,
                                         "cannot create port handle"));
  }
  OpenPortGuard port_guard{port};

  result = sp_open(port_guard.get(), SP_MODE_READ_WRITE);
  if (result != SP_OK) {
    return tl::unexpected(adapt_sp_error(result, Operation::OpenSerial,
                                         "cannot open serial port"));
  }
  port_guard.mark_open();

  int native_fd = -1;
  result = sp_get_port_handle(port_guard.get(), &native_fd);
  if (result != SP_OK) {
    return tl::unexpected(adapt_sp_error(result, Operation::OpenSerial,
                                         "cannot obtain native serial handle"));
  }
  struct stat opened_stat{};
  if (::fstat(native_fd, &opened_stat) != 0) {
    const int error_number = errno;
    return tl::unexpected(posix_error(error_number, Operation::OpenSerial,
                                      "cannot inspect opened serial handle"));
  }
  if (!S_ISCHR(opened_stat.st_mode) ||
      identity_from(opened_stat) != path.identity) {
    return tl::unexpected(
        make_error(ErrorCode::SerialDeviceGone, Operation::OpenSerial,
                   "opened serial device identity does not match selection"));
  }

  sp_port_config *raw_config = nullptr;
  result = sp_new_config(&raw_config);
  if (result != SP_OK) {
    return tl::unexpected(
        adapt_sp_error(result, Operation::ConfigureSerial,
                       "cannot allocate complete port configuration"));
  }
  PortConfigGuard config_guard{raw_config};
  const auto set_field = [&](const enum sp_return field_result,
                             const std::string_view field) -> Status {
    if (field_result == SP_OK) {
      return {};
    }
    return tl::unexpected(
        adapt_sp_error(field_result, Operation::ConfigureSerial, field));
  };
  Status status =
      set_field(sp_set_config_baudrate(config_guard.get(), configuration.baud),
                "invalid baud rate");
  if (status) {
    status = set_field(
        sp_set_config_bits(config_guard.get(), configuration.data_bits),
        "invalid data bits");
  }
  if (status) {
    status = set_field(sp_set_config_parity(config_guard.get(), *parity),
                       "invalid parity");
  }
  if (status) {
    status = set_field(
        sp_set_config_stopbits(config_guard.get(), configuration.stop_bits),
        "invalid stop bits");
  }
  if (status) {
    status = set_field(sp_set_config_flowcontrol(config_guard.get(), *flow),
                       "invalid flow control");
  }
  if (status) {
    result = sp_set_config(port_guard.get(), config_guard.get());
    if (result != SP_OK) {
      status = tl::unexpected(
          adapt_sp_error(result, Operation::ConfigureSerial,
                         "cannot apply complete port configuration"));
    }
  }
  if (!status) {
    return tl::unexpected(std::move(status.error()));
  }
  impl_->port = port_guard.release();
  impl_->open = true;
  impl_->wait_fd = native_fd;
  return {};
}

Result<int> LibserialportBackend::native_wait_handle() const {
  if (!impl_->open || impl_->wait_fd < 0) {
    return tl::unexpected(
        make_error(ErrorCode::InternalInvariantBroken, Operation::OpenSerial,
                   "serial wait handle requested while closed"));
  }
  return impl_->wait_fd;
}

Result<std::size_t>
LibserialportBackend::read_some(const std::span<std::byte> destination) {
  if (!impl_->open) {
    return tl::unexpected(make_error(ErrorCode::InternalInvariantBroken,
                                     Operation::ReadSerial,
                                     "serial read requested while closed"));
  }
  const int result =
      sp_nonblocking_read(impl_->port, destination.data(), destination.size());
  if (result < 0) {
    return tl::unexpected(adapt_sp_error(result, Operation::ReadSerial,
                                         "nonblocking serial read failed"));
  }
  return static_cast<std::size_t>(result);
}

Result<std::size_t>
LibserialportBackend::write_some(const std::span<const std::byte> source) {
  if (!impl_->open) {
    return tl::unexpected(make_error(ErrorCode::InternalInvariantBroken,
                                     Operation::WriteSerial,
                                     "serial write requested while closed"));
  }
  const int result =
      sp_nonblocking_write(impl_->port, source.data(), source.size());
  if (result < 0) {
    return tl::unexpected(adapt_sp_error(result, Operation::WriteSerial,
                                         "nonblocking serial write failed"));
  }
  return static_cast<std::size_t>(result);
}

Status LibserialportBackend::close() {
  if (impl_->port == nullptr) {
    impl_->open = false;
    impl_->wait_fd = -1;
    return {};
  }

  std::optional<Error> close_error;
  if (impl_->open) {
    const auto result = sp_close(impl_->port);
    if (result != SP_OK) {
      close_error =
          adapt_sp_error(result, Operation::CloseSerial, "serial close failed");
    }
  }
  impl_->open = false;
  impl_->wait_fd = -1;
  sp_free_port(std::exchange(impl_->port, nullptr));
  if (close_error) {
    return tl::unexpected(std::move(*close_error));
  }
  return {};
}

} // namespace lazycom::serial
