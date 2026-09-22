#include <lazycom/base/error.hpp>
#include <lazycom/base/text.hpp>

#include <algorithm>
#include <array>
#include <string_view>

namespace lazycom {
namespace {

struct ErrorRegistryEntry {
  ErrorCode code;
  ErrorDescriptor descriptor;
};

constexpr std::array registry{
    ErrorRegistryEntry{ErrorCode::ValidationInvalidValue,
                       {"LC-VAL-1001", "validation", "Invalid value"}},
    ErrorRegistryEntry{ErrorCode::SerialPermissionDenied,
                       {"LC-SER-2001", "serial", "Permission denied"}},
    ErrorRegistryEntry{ErrorCode::SerialPortBusy,
                       {"LC-SER-2002", "serial", "Serial port is busy"}},
    ErrorRegistryEntry{ErrorCode::SerialDeviceGone,
                       {"LC-SER-2003", "serial", "Serial device disappeared"}},
    ErrorRegistryEntry{ErrorCode::SerialUnsupported,
                       {"LC-SER-2004", "serial", "Serial setting unsupported"}},
    ErrorRegistryEntry{ErrorCode::SerialOperationTimedOut,
                       {"LC-SER-2005", "serial", "Serial operation timed out"}},
    ErrorRegistryEntry{
        ErrorCode::SerialOperationCancelled,
        {"LC-SER-2006", "serial", "Serial operation was cancelled"}},
    ErrorRegistryEntry{ErrorCode::ConfigParseFailed,
                       {"LC-CFG-3001", "config", "Configuration parse failed"}},
    ErrorRegistryEntry{
        ErrorCode::ConfigSchemaInvalid,
        {"LC-CFG-3002", "config", "Configuration schema is invalid"}},
    ErrorRegistryEntry{
        ErrorCode::ConfigUnsafeFile,
        {"LC-CFG-3003", "config", "Configuration file is unsafe"}},
    ErrorRegistryEntry{ErrorCode::ConfigIoFailed,
                       {"LC-CFG-3004", "config", "Configuration I/O failed"}},
    ErrorRegistryEntry{ErrorCode::LoggingDiskFull,
                       {"LC-LOG-4001", "logging", "Session log disk is full"}},
    ErrorRegistryEntry{
        ErrorCode::LoggingSchemaInvalid,
        {"LC-LOG-4002", "logging", "Session log schema is invalid"}},
    ErrorRegistryEntry{
        ErrorCode::DiagnosticsUnavailable,
        {"LC-DIAG-5001", "diagnostics", "Diagnostics unavailable"}},
    ErrorRegistryEntry{
        ErrorCode::InternalInvariantBroken,
        {"LC-INT-9001", "internal", "Internal invariant broken"}},
    ErrorRegistryEntry{ErrorCode::InternalOutOfMemory,
                       {"LC-INT-9002", "internal", "Memory allocation failed"}},
};

constexpr ErrorDescriptor unknown_descriptor{"LC-INT-9999", "internal",
                                             "Unknown internal error"};

} // namespace

const ErrorDescriptor &error_descriptor(const ErrorCode code) noexcept {
  for (const auto &entry : registry) {
    if (entry.code == code) {
      return entry.descriptor;
    }
  }
  return unknown_descriptor;
}

Error make_error(const ErrorCode code, const Operation operation,
                 const std::string_view detail, std::error_code cause,
                 const std::optional<SessionId> session_id,
                 const std::optional<OperationId> operation_id,
                 const std::source_location source) {
  return Error{
      code,       operation,
      cause,      sanitize_message(detail, kMaxErrorDetailBytes),
      session_id, operation_id,
      source,
  };
}

} // namespace lazycom
