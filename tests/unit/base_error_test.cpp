#include <lazycom/base/error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <set>
#include <string>

TEST_CASE("error registry exposes stable identifiers", "[base]") {
  const auto &descriptor =
      lazycom::error_descriptor(lazycom::ErrorCode::SerialDeviceGone);

  REQUIRE(descriptor.identifier == "LC-SER-2003");
  REQUIRE(descriptor.domain == "serial");
}

TEST_CASE("published error codes retain unique numeric values and identifiers",
          "[base]") {
  constexpr std::array codes{
      lazycom::ErrorCode::ValidationInvalidValue,
      lazycom::ErrorCode::SerialPermissionDenied,
      lazycom::ErrorCode::SerialPortBusy,
      lazycom::ErrorCode::SerialDeviceGone,
      lazycom::ErrorCode::SerialUnsupported,
      lazycom::ErrorCode::SerialOperationTimedOut,
      lazycom::ErrorCode::SerialOperationCancelled,
      lazycom::ErrorCode::ConfigParseFailed,
      lazycom::ErrorCode::ConfigSchemaInvalid,
      lazycom::ErrorCode::ConfigUnsafeFile,
      lazycom::ErrorCode::ConfigIoFailed,
      lazycom::ErrorCode::LoggingDiskFull,
      lazycom::ErrorCode::LoggingSchemaInvalid,
      lazycom::ErrorCode::DiagnosticsUnavailable,
      lazycom::ErrorCode::InternalInvariantBroken,
      lazycom::ErrorCode::InternalOutOfMemory,
  };
  std::set<std::uint32_t> values;
  std::set<std::string_view> identifiers;
  for (const auto code : codes) {
    values.insert(static_cast<std::uint32_t>(code));
    identifiers.insert(lazycom::error_descriptor(code).identifier);
  }
  REQUIRE(values.size() == codes.size());
  REQUIRE(identifiers.size() == codes.size());
}

TEST_CASE("serial operation terminal errors have stable identifiers",
          "[base][serial]") {
  CHECK(lazycom::error_descriptor(lazycom::ErrorCode::SerialOperationTimedOut)
            .identifier == "LC-SER-2005");
  CHECK(lazycom::error_descriptor(lazycom::ErrorCode::SerialOperationCancelled)
            .identifier == "LC-SER-2006");
}

TEST_CASE("error captures operation context", "[base]") {
  auto error = lazycom::make_error(
      lazycom::ErrorCode::SerialPortBusy, lazycom::Operation::OpenSerial,
      "test port", {}, lazycom::SessionId{7}, lazycom::OperationId{11});

  REQUIRE(error.detail == "test port");
  REQUIRE(error.session_id->value == 7);
  REQUIRE(error.operation_id->value == 11);
}

TEST_CASE("error detail is bounded UTF-8 safe and control visible", "[base]") {
  std::string detail{"line\n"};
  detail.push_back('\x1B');
  detail.append("\xF0\x9F\x98\x80");
  detail.push_back(static_cast<char>(0xFF));

  const auto error =
      lazycom::make_error(lazycom::ErrorCode::InternalInvariantBroken,
                          lazycom::Operation::CoordinateFatal, detail);
  REQUIRE(error.detail == "line\\x0A\\x1B\xF0\x9F\x98\x80\\xFF");

  std::string boundary(lazycom::kMaxErrorDetailBytes - 1U, 'a');
  boundary.append("\xC2\xA2");
  const auto truncated =
      lazycom::make_error(lazycom::ErrorCode::ConfigParseFailed,
                          lazycom::Operation::ParseConfig, boundary);
  REQUIRE(truncated.detail.size() == lazycom::kMaxErrorDetailBytes - 1U);
  REQUIRE(truncated.detail.back() == 'a');

  const auto bounded = lazycom::make_error(
      lazycom::ErrorCode::ConfigParseFailed, lazycom::Operation::ParseConfig,
      std::string(lazycom::kMaxErrorDetailBytes + 100U, 'x'));
  REQUIRE(bounded.detail.size() == lazycom::kMaxErrorDetailBytes);
}
