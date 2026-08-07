#include <lazycom/base/error.hpp>

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

[[nodiscard]] constexpr std::size_t
utf8_sequence_length(const unsigned int first) noexcept {
  if (first <= 0x7FU) {
    return 1U;
  }
  if (first >= 0xC2U && first <= 0xDFU) {
    return 2U;
  }
  if (first >= 0xE0U && first <= 0xEFU) {
    return 3U;
  }
  if (first >= 0xF0U && first <= 0xF4U) {
    return 4U;
  }
  return 0U;
}

[[nodiscard]] bool valid_utf8_sequence(const std::string_view value,
                                       const std::size_t offset,
                                       const std::size_t length) noexcept {
  if (length == 0U || length > value.size() - offset) {
    return false;
  }
  const auto first = static_cast<unsigned char>(value[offset]);
  for (std::size_t index = 1U; index < length; ++index) {
    const auto continuation = static_cast<unsigned char>(value[offset + index]);
    if (continuation < 0x80U || continuation > 0xBFU) {
      return false;
    }
  }
  if (length >= 3U) {
    const auto second = static_cast<unsigned char>(value[offset + 1U]);
    if ((first == 0xE0U && second < 0xA0U) ||
        (first == 0xEDU && second > 0x9FU) ||
        (first == 0xF0U && second < 0x90U) ||
        (first == 0xF4U && second > 0x8FU)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::uint32_t
decode_code_point(const std::string_view value, const std::size_t offset,
                  const std::size_t length) noexcept {
  const auto first = static_cast<unsigned char>(value[offset]);
  if (length == 1U) {
    return first;
  }
  std::uint32_t result = first & (0x7FU >> length);
  for (std::size_t index = 1U; index < length; ++index) {
    result = (result << 6U) |
             (static_cast<unsigned char>(value[offset + index]) & 0x3FU);
  }
  return result;
}

[[nodiscard]] constexpr bool
unsafe_code_point(const std::uint32_t value) noexcept {
  return value <= 0x1FU || (value >= 0x7FU && value <= 0x9FU) ||
         value == 0x061CU || value == 0x200EU || value == 0x200FU ||
         (value >= 0x202AU && value <= 0x202EU) ||
         (value >= 0x2066U && value <= 0x2069U);
}

[[nodiscard]] std::string hex_escape(const unsigned int value) {
  static constexpr std::string_view digits = "0123456789ABCDEF";
  std::string result{"\\x00"};
  result[2] = digits[(value >> 4U) & 0x0FU];
  result[3] = digits[value & 0x0FU];
  return result;
}

[[nodiscard]] std::string unicode_escape(std::uint32_t value) {
  static constexpr std::string_view digits = "0123456789ABCDEF";
  std::string result{"\\u{0000}"};
  for (std::size_t index = 0; index < 4U; ++index) {
    result[6U - index] = digits[value & 0x0FU];
    value >>= 4U;
  }
  return result;
}

[[nodiscard]] std::string sanitize_error_detail(const std::string_view detail) {
  std::string result;
  result.reserve(std::min(detail.size(), kMaxErrorDetailBytes));
  const auto append = [&result](const std::string_view value) {
    if (value.size() > kMaxErrorDetailBytes - result.size()) {
      return false;
    }
    result.append(value);
    return true;
  };

  std::size_t offset = 0U;
  while (offset < detail.size() && result.size() < kMaxErrorDetailBytes) {
    const auto first = static_cast<unsigned char>(detail[offset]);
    const auto length = utf8_sequence_length(first);
    if (!valid_utf8_sequence(detail, offset, length)) {
      if (!append(hex_escape(first))) {
        break;
      }
      ++offset;
      continue;
    }
    const auto point = decode_code_point(detail, offset, length);
    if (unsafe_code_point(point)) {
      const auto escaped =
          point <= 0xFFU ? hex_escape(point) : unicode_escape(point);
      if (!append(escaped)) {
        break;
      }
    } else if (!append(detail.substr(offset, length))) {
      break;
    }
    offset += length;
  }
  return result;
}

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
      code,       operation,    cause,  sanitize_error_detail(detail),
      session_id, operation_id, source,
  };
}

} // namespace lazycom
