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

[[nodiscard]] Result<DevicePath>
inspect_device_path(std::string_view absolute_path);
[[nodiscard]] Result<DevicePermission>
inspect_device_permission(std::string_view absolute_path);

class ISerialBackend {
public:
  virtual ~ISerialBackend() = default;

  virtual Result<std::vector<DeviceInfo>> enumerate() = 0;
  virtual Status open(const DevicePath &path, const PortConfig &config) = 0;
  virtual Result<int> native_wait_handle() const = 0;
  virtual Result<std::size_t> read_some(std::span<std::byte> destination) = 0;
  virtual Result<std::size_t> write_some(std::span<const std::byte> source) = 0;
  virtual Status close() = 0;
};

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
