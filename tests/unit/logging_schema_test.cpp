#include <lazycom/logging/schema.hpp>

#include <support/logging_builders.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

using lazycom::logging::Direction;
using lazycom::logging::Header;
using lazycom::logging::Record;

[[nodiscard]] Header valid_header() {
  return lazycom::test::LogHeaderBuilder{}.build();
}

[[nodiscard]] Record payload_record(std::uint64_t seq, Direction direction,
                                    std::vector<std::byte> payload) {
  return lazycom::test::LogRecordBuilder{seq, direction, std::move(payload)}
      .build();
}

[[nodiscard]] std::string replace_once(std::string input, std::string_view from,
                                       std::string_view to) {
  const auto position = input.find(from);
  REQUIRE(position != std::string::npos);
  input.replace(position, from.size(), to);
  return input;
}

} // namespace

TEST_CASE("NDJSON v1 header round trips safe and binary metadata",
          "[logging][schema]") {
  auto header = valid_header();
  header.device.manufacturer = lazycom::test::bytes("LazyCom");
  header.device.product =
      std::vector<std::byte>{std::byte{0xFF}, std::byte{0x00}};

  const auto encoded = lazycom::logging::encode_header_line(header);
  REQUIRE(encoded);
  REQUIRE(encoded->ends_with('\n'));
  REQUIRE(encoded->find("\"major\":1") != std::string::npos);
  REQUIRE(encoded->find("\"minor\":0") != std::string::npos);
  REQUIRE(encoded->find("\"product_base64\":\"/wA=\"") != std::string::npos);
  REQUIRE(encoded->find(static_cast<char>(0xFF)) == std::string::npos);

  const auto decoded = lazycom::logging::decode_header_line(*encoded);
  REQUIRE(decoded);
  REQUIRE(*decoded == header);
}

TEST_CASE("strict UTF-8 payload including NUL and ESC remains byte exact",
          "[logging][schema]") {
  const std::vector<std::byte> payload{
      std::byte{'A'},  std::byte{0x00}, std::byte{0x1B}, std::byte{0xE4},
      std::byte{0xB8}, std::byte{0xAD}, std::byte{'\n'}};
  const auto original = payload_record(1U, Direction::Rx, payload);

  const auto encoded = lazycom::logging::encode_record_line(original);
  REQUIRE(encoded);
  REQUIRE(encoded->find("\"encoding\":\"utf8\"") != std::string::npos);
  REQUIRE(encoded->find("\\u0000") != std::string::npos);
  REQUIRE(encoded->find("\\u001b") != std::string::npos);
  REQUIRE(std::count(encoded->begin(), encoded->end(), '\n') == 1);

  const auto decoded = lazycom::logging::decode_record_line(*encoded);
  REQUIRE(decoded);
  REQUIRE(decoded->payload == payload);
  REQUIRE(decoded->encoding == lazycom::logging::PayloadEncoding::Utf8);
}

TEST_CASE("all octets use base64 when the byte stream is not strict UTF-8",
          "[logging][schema]") {
  std::vector<std::byte> payload;
  payload.reserve(256U);
  for (unsigned int value = 0; value <= 255U; ++value) {
    payload.push_back(static_cast<std::byte>(value));
  }
  const auto encoded = lazycom::logging::encode_record_line(
      payload_record(2U, Direction::Tx, payload));
  REQUIRE(encoded);
  REQUIRE(encoded->find("\"encoding\":\"base64\"") != std::string::npos);

  const auto decoded = lazycom::logging::decode_record_line(*encoded);
  REQUIRE(decoded);
  REQUIRE(decoded->payload == payload);
  REQUIRE(decoded->encoding == lazycom::logging::PayloadEncoding::Base64);
}

TEST_CASE("representative malformed UTF-8 is rejected by the strict validator",
          "[logging][utf8]") {
  const std::array malformed{
      std::vector<std::byte>{std::byte{0x80}},
      std::vector<std::byte>{std::byte{0xC0}, std::byte{0x80}},
      std::vector<std::byte>{std::byte{0xE0}, std::byte{0x80}, std::byte{0x80}},
      std::vector<std::byte>{std::byte{0xED}, std::byte{0xA0}, std::byte{0x80}},
      std::vector<std::byte>{std::byte{0xF4}, std::byte{0x90}, std::byte{0x80},
                             std::byte{0x80}},
      std::vector<std::byte>{std::byte{0xE2}, std::byte{0x82}}};
  for (const auto &value : malformed) {
    REQUIRE_FALSE(lazycom::logging::is_strict_utf8(value));
  }
  REQUIRE(lazycom::logging::is_strict_utf8(
      lazycom::test::bytes("\xF0\x9F\x98\x80")));
}

TEST_CASE("base64 decoder is reversible and rejects non-canonical input",
          "[logging][schema]") {
  const auto valid = lazycom::logging::decode_record_line(
      R"({"type":"record","seq":1,"time_utc":"2026-07-22T04:30:03Z","elapsed_ns":0,"direction":"RX","encoding":"base64","content":"AaD/fg=="})");
  REQUIRE(valid);
  REQUIRE(valid->payload ==
          std::vector<std::byte>{std::byte{0x01}, std::byte{0xA0},
                                 std::byte{0xFF}, std::byte{0x7E}});

  const auto noncanonical = lazycom::logging::decode_record_line(
      R"({"type":"record","seq":1,"time_utc":"2026-07-22T04:30:03Z","elapsed_ns":0,"direction":"RX","encoding":"base64","content":"/x=="})");
  REQUIRE_FALSE(noncanonical);
  REQUIRE(noncanonical.error().code ==
          lazycom::logging::SchemaErrorCode::InvalidBase64);

  const auto noncanonical_encoding = lazycom::logging::decode_record_line(
      R"({"type":"record","seq":1,"time_utc":"2026-07-22T04:30:03Z","elapsed_ns":0,"direction":"RX","encoding":"base64","content":"QQ=="})");
  REQUIRE_FALSE(noncanonical_encoding);
  REQUIRE(noncanonical_encoding.error().code ==
          lazycom::logging::SchemaErrorCode::InvalidRecord);
}

TEST_CASE("record schema enforces seq UTC direction and stable ERR code",
          "[logging][schema]") {
  auto record = payload_record(0U, Direction::Rx, {});
  REQUIRE_FALSE(lazycom::logging::encode_record_line(record));

  record.seq = 1U;
  record.elapsed_ns = std::numeric_limits<std::uint64_t>::max();
  REQUIRE(lazycom::logging::encode_record_line(record));
  record.time_utc = "2026-02-29T00:00:00Z";
  REQUIRE_FALSE(lazycom::logging::encode_record_line(record));

  Record error_record;
  error_record.seq = 2U;
  error_record.time_utc = "2024-02-29T23:59:59.123456789Z";
  error_record.direction = Direction::Err;
  error_record.message = std::string{"bad\0\x1B", 5U};
  error_record.code = "LC-SER-2003";
  const auto encoded_error = lazycom::logging::encode_record_line(error_record);
  REQUIRE(encoded_error);
  REQUIRE(encoded_error->find(R"(bad\\x00\\x1B)") != std::string::npos);
  const auto decoded_error =
      lazycom::logging::decode_record_line(*encoded_error);
  REQUIRE(decoded_error);
  REQUIRE(decoded_error->message == R"(bad\x00\x1B)");
  REQUIRE(decoded_error->code == "LC-SER-2003");

  error_record.code = "SERIAL-2003";
  REQUIRE_FALSE(lazycom::logging::encode_record_line(error_record));

  Record system_record;
  system_record.seq = 3U;
  system_record.time_utc = "2024-02-29T23:59:59Z";
  system_record.direction = Direction::Sys;
  system_record.message = "Session opened";
  const auto encoded_system =
      lazycom::logging::encode_record_line(system_record);
  REQUIRE(encoded_system);
  const auto decoded_system =
      lazycom::logging::decode_record_line(*encoded_system);
  REQUIRE(decoded_system);
  REQUIRE(decoded_system->direction == Direction::Sys);

  const auto negative_elapsed = lazycom::logging::decode_record_line(
      R"({"type":"record","seq":1,"time_utc":"2024-02-29T23:59:59Z","elapsed_ns":-1,"direction":"SYS","message":"safe"})");
  REQUIRE_FALSE(negative_elapsed);
}

TEST_CASE("UTC validation checks syntax ranges calendar dates and UTC marker",
          "[logging][schema]") {
  REQUIRE(lazycom::logging::is_valid_utc("2024-02-29T00:00:00Z"));
  REQUIRE(lazycom::logging::is_valid_utc("2026-07-22T04:30:00.001Z"));
  REQUIRE_FALSE(lazycom::logging::is_valid_utc("2023-02-29T00:00:00Z"));
  REQUIRE_FALSE(lazycom::logging::is_valid_utc("2024-01-01T24:00:00Z"));
  REQUIRE_FALSE(lazycom::logging::is_valid_utc("2024-01-01T00:00:60Z"));
  REQUIRE_FALSE(lazycom::logging::is_valid_utc("2024-01-01T00:00:00+00:00"));
  REQUIRE_FALSE(lazycom::logging::is_valid_utc("2024-01-01T00:00:00.Z"));
}

TEST_CASE("unknown major is rejected while unknown minor fields are ignored",
          "[logging][schema]") {
  const auto encoded = lazycom::logging::encode_header_line(valid_header());
  REQUIRE(encoded);

  const auto unknown_major =
      replace_once(*encoded, "\"major\":1", "\"major\":2");
  const auto rejected = lazycom::logging::decode_header_line(unknown_major);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code ==
          lazycom::logging::SchemaErrorCode::UnsupportedMajorVersion);

  auto future_minor = replace_once(*encoded, "\"minor\":0",
                                   "\"minor\":7,\"future_version_data\":true");
  future_minor =
      replace_once(future_minor, "\"session_id\":\"a1b2c3\"",
                   "\"future_top_level\":{},\"session_id\":\"a1b2c3\"");
  const auto accepted = lazycom::logging::decode_header_line(future_minor);
  REQUIRE(accepted);
  REQUIRE(accepted->version.major == 1U);
  REQUIRE(accepted->version.minor == 7U);
}

TEST_CASE("line decoders reject internal line breaks and accept one CRLF",
          "[logging][schema]") {
  const auto encoded = lazycom::logging::encode_header_line(valid_header());
  REQUIRE(encoded);

  auto multiline = *encoded;
  multiline.insert(1U, 1U, '\n');
  REQUIRE_FALSE(lazycom::logging::decode_header_line(multiline));

  auto internal_cr = *encoded;
  internal_cr.insert(1U, 1U, '\r');
  REQUIRE_FALSE(lazycom::logging::decode_header_line(internal_cr));

  auto crlf = encoded->substr(0U, encoded->size() - 1U);
  crlf += "\r\n";
  REQUIRE(lazycom::logging::decode_header_line(crlf));
  REQUIRE_FALSE(lazycom::logging::decode_header_line(*encoded + "\n"));
}

TEST_CASE("untrusted NDJSON limits apply before payload decoding",
          "[logging][schema]") {
  const std::string oversized_line(lazycom::logging::kMaxPhysicalLineBytes + 1U,
                                   ' ');
  const auto line_result = lazycom::logging::decode_record_line(oversized_line);
  REQUIRE_FALSE(line_result);
  REQUIRE(line_result.error().code ==
          lazycom::logging::SchemaErrorCode::LimitExceeded);

  const auto maximum_base64_bytes =
      (lazycom::logging::kMaxPayloadBytes / 3U) * 4U +
      (lazycom::logging::kMaxPayloadBytes % 3U == 0U ? 0U : 4U);
  std::string oversized_base64 =
      R"({"type":"record","seq":1,"time_utc":"2026-07-22T04:30:03Z","elapsed_ns":0,"direction":"RX","encoding":"base64","content":")";
  oversized_base64.append(maximum_base64_bytes + 4U, 'A');
  oversized_base64 += R"("})";
  const auto base64_result =
      lazycom::logging::decode_record_line(oversized_base64);
  REQUIRE_FALSE(base64_result);
  REQUIRE(base64_result.error().code ==
          lazycom::logging::SchemaErrorCode::LimitExceeded);

  const std::string oversized_document(
      lazycom::logging::kMaxNdjsonDocumentBytes + 1U, 'x');
  const auto document_result =
      lazycom::logging::decode_ndjson(oversized_document);
  REQUIRE_FALSE(document_result);
  REQUIRE(document_result.error().code ==
          lazycom::logging::SchemaErrorCode::LimitExceeded);
}

TEST_CASE("NDJSON document enforces its record count limit",
          "[logging][schema]") {
  const auto header = lazycom::logging::encode_header_line(valid_header());
  REQUIRE(header);
  std::string document = *header;
  document.reserve(lazycom::logging::kMaxNdjsonDocumentBytes);
  for (std::size_t index = 1U;
       index <= lazycom::logging::kMaxNdjsonRecords + 1U; ++index) {
    document += R"({"type":"record","seq":)";
    document += std::to_string(index);
    document +=
        R"(,"time_utc":"2026-07-22T04:30:03Z","elapsed_ns":0,"direction":"SYS","message":""})";
    document.push_back('\n');
  }
  REQUIRE(document.size() < lazycom::logging::kMaxNdjsonDocumentBytes);

  const auto result = lazycom::logging::decode_ndjson(document);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code ==
          lazycom::logging::SchemaErrorCode::LimitExceeded);
}

TEST_CASE("truncated final line is reported and prior complete records survive",
          "[logging][schema]") {
  const auto record =
      payload_record(1U, Direction::Rx, lazycom::test::bytes("READY\r\n"));
  const std::array records{record};
  const auto complete =
      lazycom::logging::encode_ndjson(valid_header(), records);
  REQUIRE(complete);

  const auto decoded = lazycom::logging::decode_ndjson(
      *complete + R"({"type":"record","seq":2)");
  REQUIRE(decoded);
  REQUIRE(decoded->incomplete_tail);
  REQUIRE(decoded->tail_error);
  REQUIRE(decoded->tail_error->code ==
          lazycom::logging::SchemaErrorCode::IncompleteTail);
  REQUIRE(decoded->records.size() == 1U);
  REQUIRE(decoded->records.front().payload ==
          lazycom::test::bytes("READY\r\n"));
}

TEST_CASE("any unterminated tail is ignored even when it is valid JSON",
          "[logging][schema]") {
  const auto complete = lazycom::logging::encode_ndjson(valid_header(), {});
  REQUIRE(complete);

  const auto decoded = lazycom::logging::decode_ndjson(
      *complete +
      R"({"type":"record","seq":1,"time_utc":"2026-07-22T04:30:03Z","elapsed_ns":0,"direction":"SYS","message":"complete JSON without LF"})");
  REQUIRE(decoded);
  REQUIRE(decoded->incomplete_tail);
  REQUIRE(decoded->records.empty());
}

TEST_CASE("each logical record occupies one escaped NDJSON line",
          "[logging][schema]") {
  auto first = payload_record(1U, Direction::Rx, lazycom::test::bytes("a\nb"));
  auto second = payload_record(2U, Direction::Tx, lazycom::test::bytes("c\rd"));
  const std::array records{first, second};
  const auto encoded = lazycom::logging::encode_ndjson(valid_header(), records);
  REQUIRE(encoded);
  REQUIRE(std::count(encoded->begin(), encoded->end(), '\n') == 3);
  REQUIRE(encoded->find("a\\nb") != std::string::npos);
  REQUIRE(encoded->find("c\\rd") != std::string::npos);

  second.seq = 1U;
  const std::array duplicate_records{first, second};
  REQUIRE_FALSE(
      lazycom::logging::encode_ndjson(valid_header(), duplicate_records));
}

TEST_CASE("BarrierTracker confirms only a flush covering the target",
          "[logging][barrier]") {
  lazycom::logging::BarrierTracker tracker;
  const lazycom::logging::LogBarrier barrier{5U};

  REQUIRE(tracker.check(barrier).state ==
          lazycom::logging::BarrierState::WaitingForRecords);
  REQUIRE(tracker.mark_processed_through(4U));
  tracker.mark_flush_result(true);
  REQUIRE(tracker.check(barrier).state ==
          lazycom::logging::BarrierState::WaitingForRecords);

  REQUIRE(tracker.mark_processed_through(5U));
  REQUIRE(tracker.check(barrier).state ==
          lazycom::logging::BarrierState::WaitingForFlush);
  tracker.mark_flush_result(false);
  REQUIRE(tracker.check(barrier).state ==
          lazycom::logging::BarrierState::FlushFailed);
  tracker.mark_flush_result(true);

  const auto confirmation = tracker.check(barrier);
  REQUIRE(confirmation.state == lazycom::logging::BarrierState::Confirmed);
  REQUIRE(confirmation.processed_through_seq == 5U);
  REQUIRE(confirmation.flushed_through_seq == 5U);
  REQUIRE_FALSE(confirmation.fsync_guaranteed);
  REQUIRE_FALSE(tracker.mark_processed_through(4U));
}

TEST_CASE("flush failure only fails barriers it attempted to cover",
          "[logging][barrier]") {
  lazycom::logging::BarrierTracker tracker;
  REQUIRE(tracker.mark_processed_through(4U));
  tracker.mark_flush_result(false);
  REQUIRE(tracker.flush_attempted_through_seq() == 4U);

  REQUIRE(tracker.mark_processed_through(5U));
  const auto waiting = tracker.check({5U});
  REQUIRE(waiting.state == lazycom::logging::BarrierState::WaitingForFlush);
  REQUIRE(waiting.flush_attempted_through_seq == 4U);

  tracker.mark_flush_result(true);
  REQUIRE(tracker.check({5U}).state ==
          lazycom::logging::BarrierState::Confirmed);
}
