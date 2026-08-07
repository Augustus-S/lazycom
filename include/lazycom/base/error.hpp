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

/**
 * @brief Stable machine-readable error categories.
 *
 * Each value has a stable external identifier returned by error_descriptor().
 * Published values must not be reused for a different meaning.
 */
enum class ErrorCode : std::uint32_t {
  ValidationInvalidValue = 0x010001,
  SerialPermissionDenied = 0x020001,
  SerialPortBusy = 0x020002,
  SerialDeviceGone = 0x020003,
  SerialUnsupported = 0x020004,
  SerialOperationTimedOut = 0x020005,
  SerialOperationCancelled = 0x020006,
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

/**
 * @brief Bounded context for an expected application failure.
 *
 * Recovery policy is intentionally not encoded here. The layer handling the
 * error decides whether it fails one operation, closes a session, disables a
 * subsystem, or terminates the process.
 *
 * @warning detail produced by make_error() is safe for terminal display, but it
 * is not a redaction mechanism. Direct aggregate construction bypasses that
 * sanitization. Callers must not supply payloads, credentials, or other secrets
 * as error detail.
 */
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

/**
 * @brief Returns the stable descriptor registered for an error code.
 * @return A reference to process-lifetime immutable storage. An unregistered
 * value maps to the generic internal-error descriptor.
 */
[[nodiscard]] const ErrorDescriptor &error_descriptor(ErrorCode code) noexcept;

/**
 * @brief Constructs an error with bounded, terminal-safe dynamic detail.
 * @param code Stable application error category.
 * @param operation Operation that observed the failure.
 * @param detail Untrusted diagnostic context. Invalid UTF-8, control bytes, and
 * bidirectional formatting controls are escaped, and output is limited to
 * kMaxErrorDetailBytes.
 * @param cause Optional underlying system or library error.
 * @param session_id Session associated with the failure, when applicable.
 * @param operation_id Asynchronous operation associated with the failure, when
 * applicable.
 * @param source Call site at which the error is adapted.
 * @return An owning Error value.
 * @warning Sanitization makes detail display-safe but does not remove secrets.
 */
[[nodiscard]] Error
make_error(ErrorCode code, Operation operation, std::string_view detail = {},
           std::error_code cause = {},
           std::optional<SessionId> session_id = std::nullopt,
           std::optional<OperationId> operation_id = std::nullopt,
           std::source_location source = std::source_location::current());

} // namespace lazycom
