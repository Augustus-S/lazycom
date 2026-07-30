#pragma once

#include <lazycom/base/ids.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <system_error>

#include <tl/expected.hpp>

namespace lazycom {

inline constexpr std::size_t kMaxErrorDetailBytes = 512U;

enum class ErrorCode : std::uint32_t {
  ValidationInvalidValue = 0x010001,
  SerialPermissionDenied = 0x020001,
  SerialPortBusy = 0x020002,
  SerialDeviceGone = 0x020003,
  SerialUnsupported = 0x020004,
  ConfigParseFailed = 0x030001,
  ConfigSchemaInvalid = 0x030002,
  ConfigUnsafeFile = 0x030003,
  ConfigIoFailed = 0x030004,
  LoggingDiskFull = 0x040001,
  LoggingSchemaInvalid = 0x040002,
  DiagnosticsUnavailable = 0x050001,
  InternalInvariantBroken = 0x090001,
  InternalOutOfMemory = 0x090002,
};

enum class Operation : std::uint16_t {
  ValidateConfig,
  EnumerateDevices,
  OpenSerial,
  ConfigureSerial,
  ReadSerial,
  WriteSerial,
  CloseSerial,
  WriteSessionLog,
  SaveConfig,
  ParseConfig,
  ReadConfig,
  DecodeSessionLog,
  CoordinateFatal,
};

struct ErrorDescriptor {
  std::string_view identifier;
  std::string_view domain;
  std::string_view default_message;
};

struct Error {
  ErrorCode code;
  Operation operation;
  std::error_code cause;
  std::string detail;
  std::optional<SessionId> session_id;
  std::optional<OperationId> operation_id;
  std::source_location source;
};

template <class T> using Result = tl::expected<T, Error>;

using Status = Result<void>;

[[nodiscard]] const ErrorDescriptor &error_descriptor(ErrorCode code) noexcept;

[[nodiscard]] Error
make_error(ErrorCode code, Operation operation, std::string_view detail = {},
           std::error_code cause = {},
           std::optional<SessionId> session_id = std::nullopt,
           std::optional<OperationId> operation_id = std::nullopt,
           std::source_location source = std::source_location::current());

} // namespace lazycom
