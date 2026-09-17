#include <lazycom/base/error.hpp>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

TEST_CASE("published error codes retain their numeric values and identifiers",
          "[base]") {
  using lazycom::ErrorCode;
  struct PublishedCode {
    ErrorCode code;
    std::uint32_t value;
    std::string_view identifier;
  };
  CHECK(lazycom::error_descriptor(ErrorCode::SerialDeviceGone).domain ==
        "serial");
  constexpr PublishedCode expected[]{
      {ErrorCode::ValidationInvalidValue, 0x010001, "LC-VAL-1001"},
      {ErrorCode::SerialPermissionDenied, 0x020001, "LC-SER-2001"},
      {ErrorCode::SerialPortBusy, 0x020002, "LC-SER-2002"},
      {ErrorCode::SerialDeviceGone, 0x020003, "LC-SER-2003"},
      {ErrorCode::SerialUnsupported, 0x020004, "LC-SER-2004"},
      {ErrorCode::SerialOperationTimedOut, 0x020005, "LC-SER-2005"},
      {ErrorCode::SerialOperationCancelled, 0x020006, "LC-SER-2006"},
      {ErrorCode::ConfigParseFailed, 0x030001, "LC-CFG-3001"},
      {ErrorCode::ConfigSchemaInvalid, 0x030002, "LC-CFG-3002"},
      {ErrorCode::ConfigUnsafeFile, 0x030003, "LC-CFG-3003"},
      {ErrorCode::ConfigIoFailed, 0x030004, "LC-CFG-3004"},
      {ErrorCode::LoggingDiskFull, 0x040001, "LC-LOG-4001"},
      {ErrorCode::LoggingSchemaInvalid, 0x040002, "LC-LOG-4002"},
      {ErrorCode::DiagnosticsUnavailable, 0x050001, "LC-DIAG-5001"},
      {ErrorCode::InternalInvariantBroken, 0x090001, "LC-INT-9001"},
      {ErrorCode::InternalOutOfMemory, 0x090002, "LC-INT-9002"},
  };
  for (const auto &entry : expected) {
    CAPTURE(entry.identifier);
    CHECK(static_cast<std::uint32_t>(entry.code) == entry.value);
    CHECK(lazycom::error_descriptor(entry.code).identifier == entry.identifier);
  }
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
