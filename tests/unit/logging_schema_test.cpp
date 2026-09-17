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

using namespace lazycom::logging;
using lazycom::test::bytes;

using lazycom::test::log_header;
using lazycom::test::log_record;

[[nodiscard]] std::string replace_once(std::string input, std::string_view from,
                                       std::string_view to) {
  const auto position = input.find(from);
  REQUIRE(position != std::string::npos);
  input.replace(position, from.size(), to);
  return input;
}

void check_payload(const std::vector<std::byte> &payload,
                   PayloadEncoding encoding) {
  const auto encoded =
      encode_record_line(log_record(1U, Direction::Rx, payload));
  REQUIRE(encoded);
  CHECK(encoded->size() <= kMaxPhysicalLineBytes);
  CHECK(std::count(encoded->begin(), encoded->end(), '\n') == 1);
  const auto decoded = decode_record_line(*encoded);
  REQUIRE(decoded);
  CHECK(decoded->payload == payload);
  CHECK(decoded->encoding == encoding);
}
} // namespace

TEST_CASE("NDJSON header preserves metadata and version compatibility",
          "[logging][schema]") {
  auto header = log_header();
  header.device.manufacturer = bytes("LazyCom");
  header.device.product =
      std::vector<std::byte>{std::byte{0xFF}, std::byte{0x00}};

  const auto encoded = encode_header_line(header);
  REQUIRE(encoded);
  REQUIRE(encoded->ends_with('\n'));
  REQUIRE(encoded->find("\"major\":1") != std::string::npos);
  REQUIRE(encoded->find("\"minor\":0") != std::string::npos);
  REQUIRE(encoded->find("\"product_base64\":\"/wA=\"") != std::string::npos);
  REQUIRE(encoded->find(static_cast<char>(0xFF)) == std::string::npos);

  const auto decoded = decode_header_line(*encoded);
  REQUIRE(decoded);
  REQUIRE(*decoded == header);
  const auto unknown_major =
      replace_once(*encoded, "\"major\":1", "\"major\":2");
  const auto rejected = decode_header_line(unknown_major);
  REQUIRE_FALSE(rejected);
  REQUIRE(rejected.error().code == SchemaErrorCode::UnsupportedMajorVersion);

  auto future_minor = replace_once(*encoded, "\"minor\":0",
                                   "\"minor\":7,\"future_version_data\":true");
  future_minor =
      replace_once(future_minor, "\"session_id\":\"a1b2c3\"",
                   "\"future_top_level\":{},\"session_id\":\"a1b2c3\"");
  const auto accepted = decode_header_line(future_minor);
  REQUIRE(accepted);
  REQUIRE(accepted->version.major == 1U);
  REQUIRE(accepted->version.minor == 7U);
}

TEST_CASE("NDJSON payload encoding is byte exact bounded and canonical",
          "[logging][schema]") {
  check_payload(bytes(std::string_view{"A\0\x1b\xe4\xb8\xad\n", 7U}),
                PayloadEncoding::Utf8);
  check_payload(std::vector<std::byte>(kMaxPayloadBytes, std::byte{0}),
                PayloadEncoding::Base64);
  std::vector<std::byte> all;
  for (unsigned value = 0; value < 256U; ++value)
    all.push_back(static_cast<std::byte>(value));
  check_payload(all, PayloadEncoding::Base64);
  for (const std::string_view malformed :
       {"\x80", "\xc0\x80", "\xe0\x80\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80",
        "\xe2\x82"}) {
    CAPTURE(malformed);
    CHECK_FALSE(is_strict_utf8(bytes(malformed)));
  }
  CHECK(is_strict_utf8(bytes("\xf0\x9f\x98\x80")));
  const auto valid = decode_record_line(
      R"({"type":"record","seq":1,"time_utc":"2026-07-22T04:30:03Z","elapsed_ns":0,"direction":"RX","encoding":"base64","content":"AaD/fg=="})");
  REQUIRE(valid);
  REQUIRE(valid->payload ==
          std::vector<std::byte>{std::byte{0x01}, std::byte{0xA0},
                                 std::byte{0xFF}, std::byte{0x7E}});

  const auto noncanonical = decode_record_line(
      R"({"type":"record","seq":1,"time_utc":"2026-07-22T04:30:03Z","elapsed_ns":0,"direction":"RX","encoding":"base64","content":"/x=="})");
  REQUIRE_FALSE(noncanonical);
  REQUIRE(noncanonical.error().code == SchemaErrorCode::InvalidBase64);

  const auto noncanonical_encoding = decode_record_line(
      R"({"type":"record","seq":1,"time_utc":"2026-07-22T04:30:03Z","elapsed_ns":0,"direction":"RX","encoding":"base64","content":"QQ=="})");
  REQUIRE_FALSE(noncanonical_encoding);
  REQUIRE(noncanonical_encoding.error().code == SchemaErrorCode::InvalidRecord);
}

TEST_CASE("NDJSON record fields and safe messages obey their schema",
          "[logging][schema]") {
  auto record = log_record(0U, Direction::Rx, {});
  REQUIRE_FALSE(encode_record_line(record));

  record.seq = 1U;
  record.elapsed_ns = std::numeric_limits<std::uint64_t>::max();
  REQUIRE(encode_record_line(record));
  record.time_utc = "2026-02-29T00:00:00Z";
  REQUIRE_FALSE(encode_record_line(record));

  Record error_record;
  error_record.seq = 2U;
  error_record.time_utc = "2024-02-29T23:59:59.123456789Z";
  error_record.direction = Direction::Err;
  error_record.message = std::string{"bad\0\x1B", 5U};
  error_record.code = "LC-SER-2003";
  const auto encoded_error = encode_record_line(error_record);
  REQUIRE(encoded_error);
  REQUIRE(encoded_error->find(R"(bad\\x00\\x1B)") != std::string::npos);
  const auto decoded_error = decode_record_line(*encoded_error);
  REQUIRE(decoded_error);
  REQUIRE(decoded_error->message == R"(bad\x00\x1B)");
  REQUIRE(decoded_error->code == "LC-SER-2003");

  error_record.code = "SERIAL-2003";
  REQUIRE_FALSE(encode_record_line(error_record));

  Record system_record;
  system_record.seq = 3U;
  system_record.time_utc = "2024-02-29T23:59:59Z";
  system_record.direction = Direction::Sys;
  system_record.message = "Session opened";
  const auto encoded_system = encode_record_line(system_record);
  REQUIRE(encoded_system);
  const auto decoded_system = decode_record_line(*encoded_system);
  REQUIRE(decoded_system);
  REQUIRE(decoded_system->direction == Direction::Sys);

  const auto negative_elapsed = decode_record_line(
      R"({"type":"record","seq":1,"time_utc":"2024-02-29T23:59:59Z","elapsed_ns":-1,"direction":"SYS","message":"safe"})");
  REQUIRE_FALSE(negative_elapsed);
  REQUIRE(is_valid_utc("2024-02-29T00:00:00Z"));
  REQUIRE(is_valid_utc("2026-07-22T04:30:00.001Z"));
  REQUIRE_FALSE(is_valid_utc("2023-02-29T00:00:00Z"));
  REQUIRE_FALSE(is_valid_utc("2024-01-01T24:00:00Z"));
  REQUIRE_FALSE(is_valid_utc("2024-01-01T00:00:60Z"));
  REQUIRE_FALSE(is_valid_utc("2024-01-01T00:00:00+00:00"));
  REQUIRE_FALSE(is_valid_utc("2024-01-01T00:00:00.Z"));
}

TEST_CASE("NDJSON physical lines preserve complete records and ignore "
          "unterminated tails",
          "[logging][schema]") {
  const auto header_line = encode_header_line(log_header());
  REQUIRE(header_line);

  auto multiline = *header_line;
  multiline.insert(1U, 1U, '\n');
  REQUIRE_FALSE(decode_header_line(multiline));

  auto internal_cr = *header_line;
  internal_cr.insert(1U, 1U, '\r');
  REQUIRE_FALSE(decode_header_line(internal_cr));

  auto crlf = header_line->substr(0U, header_line->size() - 1U);
  crlf += "\r\n";
  REQUIRE(decode_header_line(crlf));
  REQUIRE_FALSE(decode_header_line(*header_line + "\n"));
  auto first = log_record(1U, Direction::Rx, bytes("a\nb"));
  auto second = log_record(2U, Direction::Tx, bytes("c\rd"));
  const std::array records{first, second};
  const auto encoded = encode_ndjson(log_header(), records);
  REQUIRE(encoded);
  REQUIRE(std::count(encoded->begin(), encoded->end(), '\n') == 3);
  REQUIRE(encoded->find("a\\nb") != std::string::npos);
  REQUIRE(encoded->find("c\\rd") != std::string::npos);

  second.seq = 1U;
  const std::array duplicate_records{first, second};
  REQUIRE_FALSE(encode_ndjson(log_header(), duplicate_records));
  for (
      const std::string_view tail :
      {R"({"type":"record","seq":3)",
       R"({"type":"record","seq":3,"time_utc":"2026-07-22T04:30:03Z","elapsed_ns":0,"direction":"SYS","message":"complete JSON without LF"})"}) {
    CAPTURE(tail);
    const auto decoded = decode_ndjson(*encoded + std::string(tail));
    REQUIRE(decoded);
    CHECK(decoded->incomplete_tail);
    REQUIRE(decoded->tail_error);
    CHECK(decoded->tail_error->code == SchemaErrorCode::IncompleteTail);
    REQUIRE(decoded->records.size() == 2U);
    CHECK(decoded->records[0].payload == first.payload);
    CHECK(decoded->records[1].payload == second.payload);
  }
}

TEST_CASE("untrusted NDJSON bounds bytes nesting and structural complexity",
          "[logging][schema]") {
  const std::string oversized_line(kMaxPhysicalLineBytes + 1U, ' ');
  const auto line_result = decode_record_line(oversized_line);
  REQUIRE_FALSE(line_result);
  REQUIRE(line_result.error().code == SchemaErrorCode::LimitExceeded);

  const auto maximum_base64_bytes =
      (kMaxPayloadBytes / 3U) * 4U + (kMaxPayloadBytes % 3U == 0U ? 0U : 4U);
  std::string oversized_base64 =
      R"({"type":"record","seq":1,"time_utc":"2026-07-22T04:30:03Z","elapsed_ns":0,"direction":"RX","encoding":"base64","content":")";
  oversized_base64.append(maximum_base64_bytes + 4U, 'A');
  oversized_base64 += R"("})";
  const auto base64_result = decode_record_line(oversized_base64);
  REQUIRE_FALSE(base64_result);
  REQUIRE(base64_result.error().code == SchemaErrorCode::LimitExceeded);

  const std::string oversized_document(kMaxNdjsonDocumentBytes + 1U, 'x');
  const auto document_result = decode_ndjson(oversized_document);
  REQUIRE_FALSE(document_result);
  REQUIRE(document_result.error().code == SchemaErrorCode::LimitExceeded);
  std::string deeply_nested =
      R"({"type":"record","seq":1,"time_utc":"2026-07-22T04:30:03Z","elapsed_ns":0,"direction":"SYS","message":"safe","future":)";
  deeply_nested.append(20U, '[');
  deeply_nested += '0';
  deeply_nested.append(20U, ']');
  deeply_nested += '}';
  const auto deep = decode_record_line(deeply_nested);
  REQUIRE_FALSE(deep);
  REQUIRE(deep.error().code == SchemaErrorCode::LimitExceeded);

  std::string wide =
      R"({"type":"record","seq":1,"time_utc":"2026-07-22T04:30:03Z","elapsed_ns":0,"direction":"SYS","message":"safe")";
  for (std::size_t index = 0U; index < 300U; ++index) {
    wide += ",\"future_" + std::to_string(index) + "\":0";
  }
  wide += '}';
  const auto complex = decode_record_line(wide);
  REQUIRE_FALSE(complex);
  REQUIRE(complex.error().code == SchemaErrorCode::LimitExceeded);
}

TEST_CASE("NDJSON encoder enforces record and accumulated byte limits",
          "[logging][schema][extended]") {
  std::vector<Record> too_many(kMaxNdjsonRecords + 1U);
  const auto count_result = encode_ndjson(log_header(), too_many);
  REQUIRE_FALSE(count_result);
  REQUIRE(count_result.error().code == SchemaErrorCode::LimitExceeded);

  std::vector<Record> records;
  records.reserve(13U);
  for (std::uint64_t seq = 1U; seq <= 13U; ++seq) {
    records.push_back(
        log_record(seq, Direction::Rx,
                   std::vector<std::byte>(kMaxPayloadBytes, std::byte{0x00})));
  }
  const auto byte_result = encode_ndjson(log_header(), records);
  REQUIRE_FALSE(byte_result);
  REQUIRE(byte_result.error().code == SchemaErrorCode::LimitExceeded);
}

TEST_CASE("NDJSON document enforces its record count limit",
          "[logging][schema][extended]") {
  const auto header = encode_header_line(log_header());
  REQUIRE(header);
  std::string document = *header;
  document.reserve(kMaxNdjsonDocumentBytes);
  for (std::size_t index = 1U; index <= kMaxNdjsonRecords + 1U; ++index) {
    document += R"({"type":"record","seq":)";
    document += std::to_string(index);
    document +=
        R"(,"time_utc":"2026-07-22T04:30:03Z","elapsed_ns":0,"direction":"SYS","message":""})";
    document.push_back('\n');
  }
  REQUIRE(document.size() < kMaxNdjsonDocumentBytes);

  const auto result = decode_ndjson(document);
  REQUIRE_FALSE(result);
  REQUIRE(result.error().code == SchemaErrorCode::LimitExceeded);
}

TEST_CASE("barriers require a flush covering their own target",
          "[logging][barrier]") {
  for (const bool first_flush_succeeds : {true, false}) {
    CAPTURE(first_flush_succeeds);
    BarrierTracker tracker;
    const LogBarrier barrier{5U};

    REQUIRE(tracker.check(barrier).state == BarrierState::WaitingForRecords);
    REQUIRE(tracker.mark_processed_through(4U));
    tracker.mark_flush_result(first_flush_succeeds);
    REQUIRE(tracker.check(barrier).state == BarrierState::WaitingForRecords);

    REQUIRE(tracker.mark_processed_through(5U));
    CHECK(tracker.check(barrier).flush_attempted_through_seq == 4U);
    REQUIRE(tracker.check(barrier).state == BarrierState::WaitingForFlush);
    tracker.mark_flush_result(false);
    REQUIRE(tracker.check(barrier).state == BarrierState::FlushFailed);
    tracker.mark_flush_result(true);

    const auto confirmation = tracker.check(barrier);
    REQUIRE(confirmation.state == BarrierState::Confirmed);
    REQUIRE(confirmation.processed_through_seq == 5U);
    REQUIRE(confirmation.flushed_through_seq == 5U);
    REQUIRE_FALSE(confirmation.fsync_guaranteed);
    REQUIRE_FALSE(tracker.mark_processed_through(4U));
  }
}
