#pragma once

#include <lazycom/base/error.hpp>
#include <lazycom/config/schema.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lazycom::serial {

/**
 * @brief Nonblocking worker-to-UI wake notification.
 *
 * The callback may run on a worker thread and is coalesced rather than issued
 * once per event. It must be noexcept, return promptly, and only post a UI
 * event. context remains caller-owned until the service is stopped and
 * destroyed.
 */
using UiWakeCallback = void (*)(void *) noexcept;

enum class DeviceTransport : std::uint8_t {
  Native,
  Usb,
  Bluetooth,
  Unknown,
};

enum class DeviceAccess : std::uint8_t { Unknown, Allowed, PermissionDenied };

struct DevicePermission {
  DeviceAccess access{DeviceAccess::Unknown};
  std::uint32_t owner_uid{};
  std::uint32_t owner_gid{};
  std::uint32_t mode{};
  std::string group_name;
  auto operator<=>(const DevicePermission &) const = default;
};

struct DeviceIdentity {
  std::uint64_t device{};
  std::uint64_t inode{};
  std::uint64_t special_device{};
  auto operator<=>(const DeviceIdentity &) const = default;
};

struct DevicePath {
  std::string requested;
  std::string canonical;
  DeviceIdentity identity{};
  auto operator<=>(const DevicePath &) const = default;
};

struct DeviceInfo {
  std::string path;
  std::string description;
  DeviceTransport transport{DeviceTransport::Unknown};
  std::optional<std::uint16_t> usb_vendor_id;
  std::optional<std::uint16_t> usb_product_id;
  std::string usb_manufacturer;
  std::string usb_product;
  std::string usb_serial;
  std::string bluetooth_address;
  std::optional<DeviceIdentity> identity;
  DevicePermission permission;
  auto operator<=>(const DeviceInfo &) const = default;
};

struct PortConfig {
  std::int32_t baud{115200};
  std::int32_t data_bits{8};
  std::int32_t stop_bits{1};
  config::Parity parity{config::Parity::None};
  config::FlowControl flow_control{config::FlowControl::None};
  auto operator<=>(const PortConfig &) const = default;

  [[nodiscard]] static PortConfig
  from_defaults(const config::SerialDefaults &defaults) noexcept;
};

/**
 * @brief Resolves and snapshots an absolute character-device path.
 * @return The requested path, canonical target, and `(st_dev, st_ino, st_rdev)`
 * identity, or an Error.
 * @note The result is a time-of-check snapshot. A later open must revalidate
 * the final object.
 */
[[nodiscard]] Result<DevicePath>
inspect_device_path(std::string_view absolute_path);
/**
 * @brief Inspects effective read/write access and diagnostic ownership
 * metadata.
 * @return PermissionDenied as a successful DevicePermission state; path or
 * metadata inspection failures return Error.
 * @note This is diagnostic preflight, not authorization for a later open.
 */
[[nodiscard]] Result<DevicePermission>
inspect_device_permission(std::string_view absolute_path);

/**
 * @brief Synchronous serial-library adapter confined to one worker owner.
 *
 * Backend instances are not required to be thread-safe. enumerate() must return
 * fully owned values. Once open, native_wait_handle() is borrowed for readiness
 * waiting and identity checks only; callers must not read, write, configure, or
 * close it directly. All I/O remains nonblocking through this interface.
 */
class ISerialBackend {
public:
  virtual ~ISerialBackend() = default;

  /** @brief Enumerates ports into fully owned device values. */
  virtual Result<std::vector<DeviceInfo>> enumerate() = 0;
  /**
   * @brief Opens, revalidates, and completely configures one inspected path.
   * @post Failure leaves the backend closed.
   */
  virtual Status open(const DevicePath &path, const PortConfig &config) = 0;
  /**
   * @brief Returns a borrowed readiness descriptor valid only while open.
   */
  virtual Result<int> native_wait_handle() const = 0;
  /** @brief Performs one nonblocking read without retaining the destination. */
  virtual Result<std::size_t> read_some(std::span<std::byte> destination) = 0;
  /**
   * @brief Performs one nonblocking write without retaining the source.
   * @note A positive result means accepted by the OS, not physically delivered.
   */
  virtual Result<std::size_t> write_some(std::span<const std::byte> source) = 0;
  /**
   * @brief Releases the port and invalidates the borrowed wait descriptor.
   * @note The operation is idempotent; resources are released even when close
   * reports an error.
   */
  virtual Status close() = 0;
};

/**
 * @brief libserialport implementation that exclusively owns any active port.
 *
 * No sp_port pointer or owning native descriptor leaves the serial module.
 * Destruction performs best-effort close and releases the library object.
 */
class LibserialportBackend final : public ISerialBackend {
public:
  LibserialportBackend();
  ~LibserialportBackend() override;
  LibserialportBackend(const LibserialportBackend &) = delete;
  LibserialportBackend &operator=(const LibserialportBackend &) = delete;
  LibserialportBackend(LibserialportBackend &&) = delete;
  LibserialportBackend &operator=(LibserialportBackend &&) = delete;

  Result<std::vector<DeviceInfo>> enumerate() override;
  Status open(const DevicePath &path, const PortConfig &config) override;
  Result<int> native_wait_handle() const override;
  Result<std::size_t> read_some(std::span<std::byte> destination) override;
  Result<std::size_t> write_some(std::span<const std::byte> source) override;
  Status close() override;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lazycom::serial
