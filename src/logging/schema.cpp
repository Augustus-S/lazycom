#include <lazycom/logging/schema.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <exception>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace lazycom::logging {
namespace {

using Json = nlohmann::json;

constexpr std::size_t kMaxJsonDepth = 16U;
constexpr std::size_t kMaxJsonStructuralEvents = 256U;

class JsonComplexityError final : public std::exception {
public:
  [[nodiscard]] const char *what() const noexcept override {
    return "JSON nesting or structural complexity exceeds its limit";
  }
};

[[nodiscard]] SchemaError error(SchemaErrorCode code, std::string field,
                                std::string detail, std::size_t line = 0) {
  return SchemaError{code, line, std::move(field), std::move(detail)};
}

[[nodiscard]] unsigned int octet(std::byte value) noexcept {
  return std::to_integer<unsigned int>(value);
}

[[nodiscard]] std::span<const std::byte>
as_bytes(std::string_view value) noexcept {
  return {reinterpret_cast<const std::byte *>(value.data()), value.size()};
}

[[nodiscard]] std::string bytes_to_string(std::span<const std::byte> bytes) {
  return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

[[nodiscard]] bool has_ascii_session_id(std::string_view value) noexcept {
  if (value.empty() || value.size() > 128U) {
    return false;
  }
  return std::ranges::all_of(value, [](char character) {
    const auto byte = static_cast<unsigned char>(character);
    return (byte >= static_cast<unsigned char>('a') &&
            byte <= static_cast<unsigned char>('z')) ||
           (byte >= static_cast<unsigned char>('A') &&
            byte <= static_cast<unsigned char>('Z')) ||
           (byte >= static_cast<unsigned char>('0') &&
            byte <= static_cast<unsigned char>('9')) ||
           character == '-' || character == '_' || character == '.';
  });
}

[[nodiscard]] bool is_stable_code(std::string_view value) noexcept {
  if (!value.starts_with("LC-") || value.size() < 11U || value.size() > 32U) {
    return false;
  }
  const auto separator = value.find('-', 3U);
  if (separator == std::string_view::npos || separator < 6U ||
      separator + 5U != value.size()) {
    return false;
  }
  for (std::size_t index = 3U; index < separator; ++index) {
    if (value[index] < 'A' || value[index] > 'Z') {
      return false;
    }
  }
  for (std::size_t index = separator + 1U; index < value.size(); ++index) {
    if (value[index] < '0' || value[index] > '9') {
      return false;
    }
  }
  return true;
}

[[nodiscard]] constexpr std::string_view
direction_name(Direction direction) noexcept {
  switch (direction) {
  case Direction::Rx:
    return "RX";
  case Direction::Tx:
    return "TX";
  case Direction::Sys:
    return "SYS";
  case Direction::Err:
    return "ERR";
  }
  return {};
}

[[nodiscard]] std::optional<Direction>
parse_direction(std::string_view value) noexcept {
  if (value == "RX") {
    return Direction::Rx;
  }
  if (value == "TX") {
    return Direction::Tx;
  }
  if (value == "SYS") {
    return Direction::Sys;
  }
  if (value == "ERR") {
    return Direction::Err;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr std::string_view parity_name(Parity parity) noexcept {
  switch (parity) {
  case Parity::None:
    return "none";
  case Parity::Odd:
    return "odd";
  case Parity::Even:
    return "even";
  case Parity::Mark:
    return "mark";
  case Parity::Space:
    return "space";
  }
  return {};
}

[[nodiscard]] std::optional<Parity>
parse_parity(std::string_view value) noexcept {
  if (value == "none") {
    return Parity::None;
  }
  if (value == "odd") {
    return Parity::Odd;
  }
  if (value == "even") {
    return Parity::Even;
  }
  if (value == "mark") {
    return Parity::Mark;
  }
  if (value == "space") {
    return Parity::Space;
  }
  return std::nullopt;
}

[[nodiscard]] constexpr std::string_view flow_name(FlowControl flow) noexcept {
  switch (flow) {
  case FlowControl::None:
    return "none";
  case FlowControl::RtsCts:
    return "rts/cts";
  case FlowControl::XonXoff:
    return "xon/xoff";
  }
  return {};
}

[[nodiscard]] std::optional<FlowControl>
parse_flow(std::string_view value) noexcept {
  if (value == "none") {
    return FlowControl::None;
  }
  if (value == "rts/cts") {
    return FlowControl::RtsCts;
  }
  if (value == "xon/xoff") {
    return FlowControl::XonXoff;
  }
  return std::nullopt;
}

[[nodiscard]] std::string encode_base64(std::span<const std::byte> bytes) {
  static constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve(((bytes.size() + 2U) / 3U) * 4U);
  for (std::size_t index = 0; index < bytes.size(); index += 3U) {
    const auto first = octet(bytes[index]);
    const auto second =
        index + 1U < bytes.size() ? octet(bytes[index + 1U]) : 0U;
    const auto third =
        index + 2U < bytes.size() ? octet(bytes[index + 2U]) : 0U;
    const auto value = (first << 16U) | (second << 8U) | third;
    output.push_back(alphabet[(value >> 18U) & 0x3FU]);
    output.push_back(alphabet[(value >> 12U) & 0x3FU]);
    output.push_back(index + 1U < bytes.size() ? alphabet[(value >> 6U) & 0x3FU]
                                               : '=');
    output.push_back(index + 2U < bytes.size() ? alphabet[value & 0x3FU] : '=');
  }
  return output;
}

[[nodiscard]] int base64_value(char character) noexcept {
  if (character >= 'A' && character <= 'Z') {
    return character - 'A';
  }
  if (character >= 'a' && character <= 'z') {
    return character - 'a' + 26;
  }
  if (character >= '0' && character <= '9') {
    return character - '0' + 52;
  }
  if (character == '+') {
    return 62;
  }
  if (character == '/') {
    return 63;
  }
  return -1;
}

[[nodiscard]] SchemaResult<std::vector<std::byte>>
decode_base64(std::string_view input, std::string field,
              const std::size_t maximum_decoded_bytes) {
  const auto maximum_encoded_bytes =
      (maximum_decoded_bytes / 3U) * 4U +
      (maximum_decoded_bytes % 3U == 0U ? 0U : 4U);
  if (input.size() > maximum_encoded_bytes) {
    return tl::unexpected(error(SchemaErrorCode::LimitExceeded,
                                std::move(field),
                                "base64 value exceeds its decoded byte limit"));
  }
  if (input.size() % 4U != 0U) {
    return tl::unexpected(error(SchemaErrorCode::InvalidBase64,
                                std::move(field),
                                "base64 length is not a multiple of four"));
  }
  std::vector<std::byte> output;
  output.reserve((input.size() / 4U) * 3U);
  for (std::size_t index = 0; index < input.size(); index += 4U) {
    const bool final_group = index + 4U == input.size();
    const auto first = base64_value(input[index]);
    const auto second = base64_value(input[index + 1U]);
    const bool third_padding = input[index + 2U] == '=';
    const bool fourth_padding = input[index + 3U] == '=';
    const auto third = third_padding ? 0 : base64_value(input[index + 2U]);
    const auto fourth = fourth_padding ? 0 : base64_value(input[index + 3U]);
    if (first < 0 || second < 0 || third < 0 || fourth < 0 ||
        (third_padding && !fourth_padding) ||
        (!final_group && (third_padding || fourth_padding))) {
      return tl::unexpected(
          error(SchemaErrorCode::InvalidBase64, std::move(field),
                "base64 contains invalid characters or padding"));
    }
    if ((third_padding && (second & 0x0F) != 0) ||
        (fourth_padding && !third_padding && (third & 0x03) != 0)) {
      return tl::unexpected(error(SchemaErrorCode::InvalidBase64,
                                  std::move(field),
                                  "base64 has non-canonical trailing bits"));
    }
    const auto value = (static_cast<unsigned int>(first) << 18U) |
                       (static_cast<unsigned int>(second) << 12U) |
                       (static_cast<unsigned int>(third) << 6U) |
                       static_cast<unsigned int>(fourth);
    output.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
    if (!third_padding) {
      output.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    }
    if (!fourth_padding) {
      output.push_back(static_cast<std::byte>(value & 0xFFU));
    }
  }
  return output;
}

void put_metadata(Json &object, std::string_view name,
                  std::span<const std::byte> bytes) {
  if (is_strict_utf8(bytes)) {
    object[std::string{name}] = bytes_to_string(bytes);
  } else {
    object[std::string{name} + "_base64"] = encode_base64(bytes);
  }
}

[[nodiscard]] SchemaResult<MetadataBytes>
get_metadata(const Json &object, std::string_view name, bool required) {
  const auto text_name = std::string{name};
  const auto base64_name = text_name + "_base64";
  const bool has_text = object.contains(text_name);
  const bool has_base64 = object.contains(base64_name);
  if (has_text && has_base64) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidField, text_name,
              "text and base64 metadata forms are mutually exclusive"));
  }
  if (!has_text && !has_base64) {
    if (required) {
      return tl::unexpected(error(SchemaErrorCode::MissingField, text_name,
                                  "required metadata is missing"));
    }
    return MetadataBytes{};
  }
  if (has_text) {
    const auto &value = object.at(text_name);
    if (!value.is_string()) {
      return tl::unexpected(error(SchemaErrorCode::InvalidType, text_name,
                                  "metadata must be a string"));
    }
    const auto text = value.get<std::string>();
    const auto bytes = as_bytes(text);
    if (!is_strict_utf8(bytes)) {
      return tl::unexpected(error(SchemaErrorCode::InvalidUtf8, text_name,
                                  "metadata text is not strict UTF-8"));
    }
    if (bytes.size() > kMaxMetadataBytes) {
      return tl::unexpected(error(SchemaErrorCode::LimitExceeded, text_name,
                                  "metadata exceeds its byte limit"));
    }
    return MetadataBytes{bytes.begin(), bytes.end()};
  }
  const auto &value = object.at(base64_name);
  if (!value.is_string()) {
    return tl::unexpected(error(SchemaErrorCode::InvalidType, base64_name,
                                "base64 metadata must be a string"));
  }
  auto decoded = decode_base64(value.get_ref<const std::string &>(),
                               base64_name, kMaxMetadataBytes);
  if (!decoded) {
    return tl::unexpected(std::move(decoded.error()));
  }
  if (decoded->size() > kMaxMetadataBytes) {
    return tl::unexpected(error(SchemaErrorCode::LimitExceeded, base64_name,
                                "metadata exceeds its byte limit"));
  }
  if (is_strict_utf8(*decoded)) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidField, base64_name,
              "strict UTF-8 metadata must use its text field"));
  }
  return decoded;
}

[[nodiscard]] SchemaResult<Json> parse_json_line(std::string_view input) {
  if (input.size() > kMaxPhysicalLineBytes) {
    return tl::unexpected(error(SchemaErrorCode::LimitExceeded, {},
                                "NDJSON line exceeds its physical byte limit"));
  }
  if (input.ends_with('\n')) {
    input.remove_suffix(1U);
    if (input.ends_with('\r')) {
      input.remove_suffix(1U);
    }
  }
  if (input.find('\n') != std::string_view::npos ||
      input.find('\r') != std::string_view::npos) {
    return tl::unexpected(error(SchemaErrorCode::InvalidJson, {},
                                "NDJSON line contains an internal line break"));
  }
  if (input.empty()) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidJson, {}, "NDJSON line is empty"));
  }
  try {
    std::size_t structural_events = 0U;
    Json::parser_callback_t complexity_guard =
        [&structural_events](const int depth, const Json::parse_event_t event,
                             Json &) {
          if (depth < 0 || static_cast<std::size_t>(depth) > kMaxJsonDepth) {
            throw JsonComplexityError{};
          }
          if (event != Json::parse_event_t::object_end &&
              event != Json::parse_event_t::array_end &&
              ++structural_events > kMaxJsonStructuralEvents) {
            throw JsonComplexityError{};
          }
          return true;
        };
    auto document =
        Json::parse(input.begin(), input.end(), complexity_guard, true);
    if (!document.is_object()) {
      return tl::unexpected(error(SchemaErrorCode::InvalidType, {},
                                  "NDJSON line must be a JSON object"));
    }
    return document;
  } catch (const JsonComplexityError &exception) {
    return tl::unexpected(
        error(SchemaErrorCode::LimitExceeded, {}, exception.what()));
  } catch (const std::bad_alloc &) {
    return tl::unexpected(error(SchemaErrorCode::LimitExceeded, {},
                                "JSON parsing exhausted available memory"));
  } catch (const Json::exception &exception) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidJson, {}, exception.what()));
  } catch (const std::exception &exception) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidJson, {}, exception.what()));
  }
}

[[nodiscard]] SchemaResult<std::string>
required_string(const Json &object, std::string_view field) {
  const auto name = std::string{field};
  if (!object.contains(name)) {
    return tl::unexpected(error(SchemaErrorCode::MissingField, name,
                                "required field is missing"));
  }
  if (!object.at(name).is_string()) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidType, name, "field must be a string"));
  }
  return object.at(name).get<std::string>();
}

[[nodiscard]] SchemaResult<std::uint64_t>
required_uint64(const Json &object, std::string_view field) {
  const auto name = std::string{field};
  if (!object.contains(name)) {
    return tl::unexpected(error(SchemaErrorCode::MissingField, name,
                                "required field is missing"));
  }
  if (!object.at(name).is_number_unsigned()) {
    return tl::unexpected(error(SchemaErrorCode::InvalidType, name,
                                "field must be an unsigned integer"));
  }
  return object.at(name).get<std::uint64_t>();
}

[[nodiscard]] SchemaResult<std::uint32_t>
required_uint32(const Json &object, std::string_view field) {
  auto value = required_uint64(object, field);
  if (!value) {
    return tl::unexpected(std::move(value.error()));
  }
  if (*value > std::numeric_limits<std::uint32_t>::max()) {
    return tl::unexpected(error(SchemaErrorCode::InvalidField,
                                std::string{field}, "integer is out of range"));
  }
  return static_cast<std::uint32_t>(*value);
}

[[nodiscard]] bool is_leap_year(unsigned int year) noexcept {
  return year % 4U == 0U && (year % 100U != 0U || year % 400U == 0U);
}

[[nodiscard]] unsigned int parse_digits(std::string_view value,
                                        std::size_t offset,
                                        std::size_t count) noexcept {
  unsigned int result = 0;
  for (std::size_t index = offset; index < offset + count; ++index) {
    if (value[index] < '0' || value[index] > '9') {
      return std::numeric_limits<unsigned int>::max();
    }
    result = result * 10U + static_cast<unsigned int>(value[index] - '0');
  }
  return result;
}

[[nodiscard]] bool append_visible(std::string &output, std::string_view value) {
  if (value.size() > kMaxMessageBytes - output.size()) {
    return false;
  }
  output.append(value);
  return true;
}

[[nodiscard]] std::size_t utf8_sequence_length(unsigned int first) noexcept {
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

[[nodiscard]] std::uint32_t decode_code_point(std::string_view value,
                                              std::size_t length) noexcept {
  const auto first = static_cast<unsigned char>(value[0]);
  if (length == 1U) {
    return first;
  }
  std::uint32_t result = first & (0x7FU >> length);
  for (std::size_t index = 1U; index < length; ++index) {
    result =
        (result << 6U) | (static_cast<unsigned char>(value[index]) & 0x3FU);
  }
  return result;
}

[[nodiscard]] bool unsafe_code_point(std::uint32_t value) noexcept {
  return value <= 0x1FU || (value >= 0x7FU && value <= 0x9FU) ||
         value == 0x061CU || value == 0x200EU || value == 0x200FU ||
         (value >= 0x202AU && value <= 0x202EU) ||
         (value >= 0x2066U && value <= 0x2069U);
}

[[nodiscard]] std::string hex_escape(unsigned int value) {
  static constexpr std::string_view digits = "0123456789ABCDEF";
  std::string escaped{"\\x00"};
  escaped[2] = digits[(value >> 4U) & 0x0FU];
  escaped[3] = digits[value & 0x0FU];
  return escaped;
}

[[nodiscard]] std::string unicode_escape(std::uint32_t value) {
  static constexpr std::string_view digits = "0123456789ABCDEF";
  std::string escaped{"\\u{0000}"};
  for (std::size_t index = 0; index < 4U; ++index) {
    escaped[6U - index] = digits[value & 0x0FU];
    value >>= 4U;
  }
  return escaped;
}

[[nodiscard]] SchemaResult<Json> make_header_json(const Header &header) {
  if (header.version.major != kSchemaMajor ||
      header.version.minor != kSchemaMinor) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidField, "version",
              "the encoder only emits LazyCom log version 1.0"));
  }
  if (!is_valid_utc(header.started_at)) {
    return tl::unexpected(error(SchemaErrorCode::InvalidUtc, "started_at",
                                "invalid UTC timestamp"));
  }
  if (!has_ascii_session_id(header.session_id)) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidField, "session_id",
              "session ID must use 1..128 safe ASCII characters"));
  }
  if (header.device.path.size() > kMaxMetadataBytes ||
      (header.device.manufacturer &&
       header.device.manufacturer->size() > kMaxMetadataBytes) ||
      (header.device.product &&
       header.device.product->size() > kMaxMetadataBytes) ||
      (header.device.serial_number &&
       header.device.serial_number->size() > kMaxMetadataBytes)) {
    return tl::unexpected(error(SchemaErrorCode::LimitExceeded, "device",
                                "device metadata exceeds its byte limit"));
  }
  if (header.serial.baud == 0U ||
      header.serial.baud > static_cast<std::uint32_t>(INT_MAX) ||
      header.serial.data_bits < 5U || header.serial.data_bits > 8U ||
      (header.serial.stop_bits != 1U && header.serial.stop_bits != 2U) ||
      parity_name(header.serial.parity).empty() ||
      flow_name(header.serial.flow_control).empty()) {
    return tl::unexpected(error(SchemaErrorCode::InvalidField, "serial",
                                "serial settings are out of range"));
  }

  Json device = Json::object();
  put_metadata(device, "path", header.device.path);
  if (header.device.manufacturer) {
    put_metadata(device, "manufacturer", *header.device.manufacturer);
  }
  if (header.device.product) {
    put_metadata(device, "product", *header.device.product);
  }
  if (header.device.serial_number) {
    put_metadata(device, "serial_number", *header.device.serial_number);
  }

  return Json{
      {"type", "header"},
      {"format", "lazycom-log"},
      {"version",
       {{"major", header.version.major}, {"minor", header.version.minor}}},
      {"started_at", header.started_at},
      {"session_id", header.session_id},
      {"device", std::move(device)},
      {"serial",
       {{"baud", header.serial.baud},
        {"data_bits", header.serial.data_bits},
        {"parity", parity_name(header.serial.parity)},
        {"stop_bits", header.serial.stop_bits},
        {"flow_control", flow_name(header.serial.flow_control)}}}};
}

[[nodiscard]] SchemaResult<Json> make_record_json(const Record &record) {
  if (record.seq == 0U) {
    return tl::unexpected(error(SchemaErrorCode::InvalidRecord, "seq",
                                "record sequence must be positive"));
  }
  if (!is_valid_utc(record.time_utc)) {
    return tl::unexpected(error(SchemaErrorCode::InvalidUtc, "time_utc",
                                "invalid UTC timestamp"));
  }
  if (direction_name(record.direction).empty()) {
    return tl::unexpected(error(SchemaErrorCode::InvalidRecord, "direction",
                                "unknown record direction"));
  }

  Json object{{"type", "record"},
              {"seq", record.seq},
              {"time_utc", record.time_utc},
              {"elapsed_ns", record.elapsed_ns},
              {"direction", direction_name(record.direction)}};
  if (record.direction == Direction::Rx || record.direction == Direction::Tx) {
    if (record.payload.size() > kMaxPayloadBytes) {
      return tl::unexpected(error(SchemaErrorCode::LimitExceeded, "content",
                                  "payload exceeds its byte limit"));
    }
    if (!record.message.empty() || record.code) {
      return tl::unexpected(
          error(SchemaErrorCode::InvalidRecord, "message",
                "RX/TX records cannot contain message or code"));
    }
    const bool utf8 = is_strict_utf8(record.payload);
    object["encoding"] = utf8 ? "utf8" : "base64";
    object["content"] =
        utf8 ? bytes_to_string(record.payload) : encode_base64(record.payload);
    if (record.input_mode) {
      if (record.direction != Direction::Tx) {
        return tl::unexpected(error(SchemaErrorCode::InvalidRecord,
                                    "input_mode",
                                    "input_mode is only valid for TX records"));
      }
      if (*record.input_mode == InputMode::Text) {
        object["input_mode"] = "txt";
      } else if (*record.input_mode == InputMode::Hex) {
        object["input_mode"] = "hex";
      } else {
        return tl::unexpected(error(SchemaErrorCode::InvalidRecord,
                                    "input_mode", "unknown input mode"));
      }
    }
  } else {
    if (!record.payload.empty() || record.input_mode) {
      return tl::unexpected(
          error(SchemaErrorCode::InvalidRecord, "content",
                "SYS/ERR records cannot contain payload or input_mode"));
    }
    object["message"] = sanitize_message(record.message);
    if (record.direction == Direction::Err) {
      if (!record.code || !is_stable_code(*record.code)) {
        return tl::unexpected(
            error(SchemaErrorCode::InvalidRecord, "code",
                  "ERR records require an LC-DOMAIN-NNNN stable code"));
      }
      object["code"] = *record.code;
    } else if (record.code) {
      return tl::unexpected(error(SchemaErrorCode::InvalidRecord, "code",
                                  "SYS records cannot contain an error code"));
    }
  }
  return object;
}

[[nodiscard]] std::string dump_line(const Json &object) {
  // ensure_ascii keeps the physical NDJSON line safe from bidi/control
  // injection.
  return object.dump(-1, ' ', true, Json::error_handler_t::strict) + '\n';
}

[[nodiscard]] SchemaResult<std::string> line_type(const Json &object) {
  return required_string(object, "type");
}

[[nodiscard]] SchemaResult<Header> decode_header_json(const Json &object) {
  auto type = line_type(object);
  if (!type) {
    return tl::unexpected(std::move(type.error()));
  }
  if (*type != "header") {
    return tl::unexpected(error(SchemaErrorCode::InvalidField, "type",
                                "expected a header object"));
  }
  auto format = required_string(object, "format");
  if (!format) {
    return tl::unexpected(std::move(format.error()));
  }
  if (*format != "lazycom-log") {
    return tl::unexpected(error(SchemaErrorCode::InvalidField, "format",
                                "unsupported log format"));
  }
  if (!object.contains("version")) {
    return tl::unexpected(error(SchemaErrorCode::MissingField, "version",
                                "required field is missing"));
  }
  const auto &version_object = object.at("version");
  if (!version_object.is_object()) {
    return tl::unexpected(error(SchemaErrorCode::InvalidType, "version",
                                "version must be an object"));
  }
  auto major = required_uint32(version_object, "major");
  auto minor = required_uint32(version_object, "minor");
  if (!major) {
    return tl::unexpected(std::move(major.error()));
  }
  if (!minor) {
    return tl::unexpected(std::move(minor.error()));
  }
  if (*major != kSchemaMajor) {
    return tl::unexpected(error(SchemaErrorCode::UnsupportedMajorVersion,
                                "version.major",
                                "unsupported log schema major version"));
  }
  auto started_at = required_string(object, "started_at");
  auto session_id = required_string(object, "session_id");
  if (!started_at) {
    return tl::unexpected(std::move(started_at.error()));
  }
  if (!is_valid_utc(*started_at)) {
    return tl::unexpected(error(SchemaErrorCode::InvalidUtc, "started_at",
                                "invalid UTC timestamp"));
  }
  if (!session_id) {
    return tl::unexpected(std::move(session_id.error()));
  }
  if (!has_ascii_session_id(*session_id)) {
    return tl::unexpected(error(SchemaErrorCode::InvalidField, "session_id",
                                "unsafe session ID"));
  }
  if (!object.contains("device") || !object.at("device").is_object()) {
    return tl::unexpected(error(SchemaErrorCode::InvalidType, "device",
                                "device must be an object"));
  }
  if (!object.contains("serial") || !object.at("serial").is_object()) {
    return tl::unexpected(error(SchemaErrorCode::InvalidType, "serial",
                                "serial must be an object"));
  }
  const auto &device_object = object.at("device");
  auto path = get_metadata(device_object, "path", true);
  auto manufacturer = get_metadata(device_object, "manufacturer", false);
  auto product = get_metadata(device_object, "product", false);
  auto serial_number = get_metadata(device_object, "serial_number", false);
  if (!path) {
    return tl::unexpected(std::move(path.error()));
  }
  if (!manufacturer) {
    return tl::unexpected(std::move(manufacturer.error()));
  }
  if (!product) {
    return tl::unexpected(std::move(product.error()));
  }
  if (!serial_number) {
    return tl::unexpected(std::move(serial_number.error()));
  }

  const auto &serial_object = object.at("serial");
  auto baud = required_uint32(serial_object, "baud");
  auto data_bits = required_uint32(serial_object, "data_bits");
  auto stop_bits = required_uint32(serial_object, "stop_bits");
  auto parity_text = required_string(serial_object, "parity");
  auto flow_text = required_string(serial_object, "flow_control");
  if (!baud) {
    return tl::unexpected(std::move(baud.error()));
  }
  if (!data_bits) {
    return tl::unexpected(std::move(data_bits.error()));
  }
  if (!stop_bits) {
    return tl::unexpected(std::move(stop_bits.error()));
  }
  if (!parity_text) {
    return tl::unexpected(std::move(parity_text.error()));
  }
  if (!flow_text) {
    return tl::unexpected(std::move(flow_text.error()));
  }
  const auto parity = parse_parity(*parity_text);
  const auto flow = parse_flow(*flow_text);
  if (*baud == 0U || *baud > static_cast<std::uint32_t>(INT_MAX) ||
      *data_bits < 5U || *data_bits > 8U ||
      (*stop_bits != 1U && *stop_bits != 2U) || !parity || !flow) {
    return tl::unexpected(error(SchemaErrorCode::InvalidField, "serial",
                                "serial settings are out of range"));
  }

  Header header;
  header.version = SchemaVersion{*major, *minor};
  header.started_at = std::move(*started_at);
  header.session_id = std::move(*session_id);
  header.device.path = std::move(*path);
  if (device_object.contains("manufacturer") ||
      device_object.contains("manufacturer_base64")) {
    header.device.manufacturer = std::move(*manufacturer);
  }
  if (device_object.contains("product") ||
      device_object.contains("product_base64")) {
    header.device.product = std::move(*product);
  }
  if (device_object.contains("serial_number") ||
      device_object.contains("serial_number_base64")) {
    header.device.serial_number = std::move(*serial_number);
  }
  header.serial =
      SerialSettings{*baud, static_cast<std::uint8_t>(*data_bits), *parity,
                     static_cast<std::uint8_t>(*stop_bits), *flow};
  return header;
}

[[nodiscard]] SchemaResult<Record> decode_record_json(const Json &object) {
  auto type = line_type(object);
  if (!type) {
    return tl::unexpected(std::move(type.error()));
  }
  if (*type != "record") {
    return tl::unexpected(error(SchemaErrorCode::InvalidField, "type",
                                "expected a record object"));
  }
  auto seq = required_uint64(object, "seq");
  auto time_utc = required_string(object, "time_utc");
  auto elapsed_ns = required_uint64(object, "elapsed_ns");
  auto direction_text = required_string(object, "direction");
  if (!seq) {
    return tl::unexpected(std::move(seq.error()));
  }
  if (*seq == 0U) {
    return tl::unexpected(error(SchemaErrorCode::InvalidRecord, "seq",
                                "record sequence must be positive"));
  }
  if (!time_utc) {
    return tl::unexpected(std::move(time_utc.error()));
  }
  if (!is_valid_utc(*time_utc)) {
    return tl::unexpected(error(SchemaErrorCode::InvalidUtc, "time_utc",
                                "invalid UTC timestamp"));
  }
  if (!elapsed_ns) {
    return tl::unexpected(std::move(elapsed_ns.error()));
  }
  if (!direction_text) {
    return tl::unexpected(std::move(direction_text.error()));
  }
  const auto direction = parse_direction(*direction_text);
  if (!direction) {
    return tl::unexpected(error(SchemaErrorCode::InvalidField, "direction",
                                "unknown record direction"));
  }

  Record record;
  record.seq = *seq;
  record.time_utc = std::move(*time_utc);
  record.elapsed_ns = *elapsed_ns;
  record.direction = *direction;
  if (*direction == Direction::Rx || *direction == Direction::Tx) {
    auto encoding = required_string(object, "encoding");
    auto content = required_string(object, "content");
    if (!encoding) {
      return tl::unexpected(std::move(encoding.error()));
    }
    if (!content) {
      return tl::unexpected(std::move(content.error()));
    }
    if (*encoding == "utf8") {
      const auto bytes = as_bytes(*content);
      if (!is_strict_utf8(bytes)) {
        return tl::unexpected(error(SchemaErrorCode::InvalidUtf8, "content",
                                    "content is not strict UTF-8"));
      }
      record.payload.assign(bytes.begin(), bytes.end());
      if (bytes_to_string(record.payload) != *content) {
        return tl::unexpected(error(SchemaErrorCode::InvalidUtf8, "content",
                                    "UTF-8 did not round trip byte-for-byte"));
      }
      record.encoding = PayloadEncoding::Utf8;
    } else if (*encoding == "base64") {
      auto decoded = decode_base64(*content, "content", kMaxPayloadBytes);
      if (!decoded) {
        return tl::unexpected(std::move(decoded.error()));
      }
      if (is_strict_utf8(*decoded)) {
        auto utf8_object = object;
        utf8_object["encoding"] = "utf8";
        utf8_object["content"] = bytes_to_string(*decoded);
        if (dump_line(utf8_object).size() <= kMaxPhysicalLineBytes) {
          return tl::unexpected(
              error(SchemaErrorCode::InvalidRecord, "encoding",
                    "strict UTF-8 payload must use utf8 encoding unless "
                    "escaping would exceed the physical line limit"));
        }
      }
      record.payload = std::move(*decoded);
      record.encoding = PayloadEncoding::Base64;
    } else {
      return tl::unexpected(error(SchemaErrorCode::InvalidField, "encoding",
                                  "unknown payload encoding"));
    }
    if (record.payload.size() > kMaxPayloadBytes) {
      return tl::unexpected(error(SchemaErrorCode::LimitExceeded, "content",
                                  "payload exceeds its byte limit"));
    }
    if (object.contains("message") || object.contains("code")) {
      return tl::unexpected(error(SchemaErrorCode::InvalidRecord, "message",
                                  "RX/TX cannot contain message or code"));
    }
    if (object.contains("input_mode")) {
      if (*direction != Direction::Tx || !object.at("input_mode").is_string()) {
        return tl::unexpected(error(SchemaErrorCode::InvalidRecord,
                                    "input_mode",
                                    "input_mode is only valid for TX"));
      }
      const auto mode = object.at("input_mode").get<std::string>();
      if (mode == "txt") {
        record.input_mode = InputMode::Text;
      } else if (mode == "hex") {
        record.input_mode = InputMode::Hex;
      } else {
        return tl::unexpected(error(SchemaErrorCode::InvalidField, "input_mode",
                                    "unknown input mode"));
      }
    }
  } else {
    auto message = required_string(object, "message");
    if (!message) {
      return tl::unexpected(std::move(message.error()));
    }
    if (*message != sanitize_message(*message)) {
      return tl::unexpected(error(SchemaErrorCode::InvalidRecord, "message",
                                  "message is not bounded safe text"));
    }
    record.message = std::move(*message);
    if (object.contains("content") || object.contains("encoding") ||
        object.contains("input_mode")) {
      return tl::unexpected(error(SchemaErrorCode::InvalidRecord, "content",
                                  "SYS/ERR cannot contain payload fields"));
    }
    if (*direction == Direction::Err) {
      auto code = required_string(object, "code");
      if (!code) {
        return tl::unexpected(std::move(code.error()));
      }
      if (!is_stable_code(*code)) {
        return tl::unexpected(error(SchemaErrorCode::InvalidRecord, "code",
                                    "invalid stable error code"));
      }
      record.code = std::move(*code);
    } else if (object.contains("code")) {
      return tl::unexpected(error(SchemaErrorCode::InvalidRecord, "code",
                                  "SYS cannot contain an error code"));
    }
  }
  return record;
}

} // namespace

bool is_strict_utf8(std::span<const std::byte> bytes) noexcept {
  std::size_t index = 0;
  while (index < bytes.size()) {
    const auto first = octet(bytes[index]);
    if (first <= 0x7FU) {
      ++index;
      continue;
    }
    const auto length = utf8_sequence_length(first);
    if (length == 0U || length > bytes.size() - index) {
      return false;
    }
    for (std::size_t continuation = 1U; continuation < length; ++continuation) {
      const auto value = octet(bytes[index + continuation]);
      if (value < 0x80U || value > 0xBFU) {
        return false;
      }
    }
    const auto second = octet(bytes[index + 1U]);
    if ((first == 0xE0U && second < 0xA0U) ||
        (first == 0xEDU && second > 0x9FU) ||
        (first == 0xF0U && second < 0x90U) ||
        (first == 0xF4U && second > 0x8FU)) {
      return false;
    }
    index += length;
  }
  return true;
}

bool is_valid_utc(std::string_view value) noexcept {
  if (value.size() < 20U || value.size() > 30U || value[4] != '-' ||
      value[7] != '-' || value[10] != 'T' || value[13] != ':' ||
      value[16] != ':' || value.back() != 'Z') {
    return false;
  }
  if (value.size() != 20U) {
    if (value.size() < 22U || value[19] != '.') {
      return false;
    }
    for (std::size_t index = 20U; index + 1U < value.size(); ++index) {
      if (value[index] < '0' || value[index] > '9') {
        return false;
      }
    }
  }
  const auto year = parse_digits(value, 0U, 4U);
  const auto month = parse_digits(value, 5U, 2U);
  const auto day = parse_digits(value, 8U, 2U);
  const auto hour = parse_digits(value, 11U, 2U);
  const auto minute = parse_digits(value, 14U, 2U);
  const auto second = parse_digits(value, 17U, 2U);
  if (year == 0U || month == 0U || month > 12U || hour > 23U || minute > 59U ||
      second > 59U) {
    return false;
  }
  static constexpr std::array<unsigned int, 12> days_per_month{
      31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};
  auto days = days_per_month[month - 1U];
  if (month == 2U && is_leap_year(year)) {
    ++days;
  }
  return day > 0U && day <= days;
}

std::string sanitize_message(std::string_view message) {
  std::string output;
  output.reserve(std::min(message.size(), kMaxMessageBytes));
  std::size_t index = 0;
  while (index < message.size() && output.size() < kMaxMessageBytes) {
    const auto first = static_cast<unsigned char>(message[index]);
    const auto length = utf8_sequence_length(first);
    if (length == 0U || length > message.size() - index ||
        !is_strict_utf8(as_bytes(message.substr(index, length)))) {
      if (!append_visible(output, hex_escape(first))) {
        break;
      }
      ++index;
      continue;
    }
    const auto point = decode_code_point(message.substr(index, length), length);
    if (unsafe_code_point(point)) {
      const auto escaped =
          point <= 0xFFU ? hex_escape(point) : unicode_escape(point);
      if (!append_visible(output, escaped)) {
        break;
      }
    } else if (!append_visible(output, message.substr(index, length))) {
      break;
    }
    index += length;
  }
  return output;
}

SchemaResult<std::string> encode_header_line(const Header &header) {
  try {
    auto object = make_header_json(header);
    if (!object) {
      return tl::unexpected(std::move(object.error()));
    }
    auto line = dump_line(*object);
    if (line.size() > kMaxPhysicalLineBytes) {
      return tl::unexpected(error(SchemaErrorCode::LimitExceeded, {},
                                  "encoded header exceeds the physical line "
                                  "byte limit"));
    }
    return line;
  } catch (const std::bad_alloc &) {
    return tl::unexpected(error(SchemaErrorCode::LimitExceeded, {},
                                "header encoding exhausted available memory"));
  } catch (const Json::exception &exception) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidUtf8, {}, exception.what()));
  } catch (const std::exception &exception) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidField, {}, exception.what()));
  }
}

SchemaResult<std::string> encode_record_line(const Record &record) {
  try {
    auto object = make_record_json(record);
    if (!object) {
      return tl::unexpected(std::move(object.error()));
    }
    auto line = dump_line(*object);
    if (line.size() > kMaxPhysicalLineBytes &&
        (record.direction == Direction::Rx ||
         record.direction == Direction::Tx) &&
        is_strict_utf8(record.payload)) {
      (*object)["encoding"] = "base64";
      (*object)["content"] = encode_base64(record.payload);
      line = dump_line(*object);
    }
    if (line.size() > kMaxPhysicalLineBytes) {
      return tl::unexpected(error(SchemaErrorCode::LimitExceeded, {},
                                  "encoded record exceeds the physical line "
                                  "byte limit"));
    }
    return line;
  } catch (const std::bad_alloc &) {
    return tl::unexpected(error(SchemaErrorCode::LimitExceeded, {},
                                "record encoding exhausted available memory"));
  } catch (const Json::exception &exception) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidUtf8, {}, exception.what()));
  } catch (const std::exception &exception) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidField, {}, exception.what()));
  }
}

SchemaResult<std::string> encode_ndjson(const Header &header,
                                        std::span<const Record> records) {
  try {
    if (records.size() > kMaxNdjsonRecords) {
      return tl::unexpected(error(SchemaErrorCode::LimitExceeded, {},
                                  "NDJSON document exceeds its record limit"));
    }
    auto encoded_header = encode_header_line(header);
    if (!encoded_header) {
      return tl::unexpected(std::move(encoded_header.error()));
    }
    if (encoded_header->size() > kMaxNdjsonDocumentBytes) {
      return tl::unexpected(error(SchemaErrorCode::LimitExceeded, {},
                                  "NDJSON document exceeds its byte limit"));
    }
    std::string output = std::move(*encoded_header);
    std::uint64_t last_seq = 0U;
    for (const auto &record : records) {
      if (record.seq <= last_seq) {
        return tl::unexpected(
            error(SchemaErrorCode::InvalidRecord, "seq",
                  "record sequences must be strictly increasing"));
      }
      auto encoded_record = encode_record_line(record);
      if (!encoded_record) {
        return tl::unexpected(std::move(encoded_record.error()));
      }
      if (encoded_record->size() > kMaxNdjsonDocumentBytes - output.size()) {
        return tl::unexpected(error(SchemaErrorCode::LimitExceeded, {},
                                    "NDJSON document exceeds its byte limit"));
      }
      output.append(*encoded_record);
      last_seq = record.seq;
    }
    return output;
  } catch (const std::bad_alloc &) {
    return tl::unexpected(error(SchemaErrorCode::LimitExceeded, {},
                                "NDJSON encoding exhausted available memory"));
  } catch (const std::exception &exception) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidField, {}, exception.what()));
  }
}

SchemaResult<Header> decode_header_line(std::string_view line) {
  try {
    auto object = parse_json_line(line);
    if (!object) {
      return tl::unexpected(std::move(object.error()));
    }
    return decode_header_json(*object);
  } catch (const std::bad_alloc &) {
    return tl::unexpected(error(SchemaErrorCode::LimitExceeded, {},
                                "header decoding exhausted available memory"));
  } catch (const std::exception &exception) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidJson, {}, exception.what()));
  }
}

SchemaResult<Record> decode_record_line(std::string_view line) {
  try {
    auto object = parse_json_line(line);
    if (!object) {
      return tl::unexpected(std::move(object.error()));
    }
    return decode_record_json(*object);
  } catch (const std::bad_alloc &) {
    return tl::unexpected(error(SchemaErrorCode::LimitExceeded, {},
                                "record decoding exhausted available memory"));
  } catch (const std::exception &exception) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidJson, {}, exception.what()));
  }
}

SchemaResult<NdjsonDocument> decode_ndjson(std::string_view input) {
  try {
    if (input.size() > kMaxNdjsonDocumentBytes) {
      return tl::unexpected(error(SchemaErrorCode::LimitExceeded, {},
                                  "NDJSON document exceeds its byte limit"));
    }
    NdjsonDocument result;
    bool has_header = false;
    std::uint64_t last_seq = 0U;
    std::size_t line_number = 1U;
    std::size_t offset = 0U;
    while (offset < input.size()) {
      const auto newline = input.find('\n', offset);
      const bool unterminated = newline == std::string_view::npos;
      if (unterminated) {
        if (!has_header) {
          return tl::unexpected(error(SchemaErrorCode::MissingHeader, {},
                                      "no complete header line was found",
                                      line_number));
        }
        result.incomplete_tail = true;
        result.tail_error =
            error(SchemaErrorCode::IncompleteTail, {},
                  "incomplete final NDJSON line was ignored", line_number);
        break;
      }
      const auto line = unterminated
                            ? input.substr(offset)
                            : input.substr(offset, newline - offset + 1U);
      auto object = parse_json_line(line);
      if (!object) {
        object.error().line = line_number;
        return tl::unexpected(std::move(object.error()));
      }
      auto type = line_type(*object);
      if (!type) {
        type.error().line = line_number;
        return tl::unexpected(std::move(type.error()));
      }
      if (!has_header) {
        if (*type != "header") {
          return tl::unexpected(
              error(SchemaErrorCode::MissingHeader, "type",
                    "the first complete line must be a header", line_number));
        }
        auto header = decode_header_json(*object);
        if (!header) {
          header.error().line = line_number;
          return tl::unexpected(std::move(header.error()));
        }
        result.header = std::move(*header);
        has_header = true;
      } else {
        if (*type == "header") {
          return tl::unexpected(
              error(SchemaErrorCode::UnexpectedHeader, "type",
                    "a log may contain only one leading header", line_number));
        }
        if (result.records.size() >= kMaxNdjsonRecords) {
          return tl::unexpected(
              error(SchemaErrorCode::LimitExceeded, {},
                    "NDJSON document exceeds its record limit", line_number));
        }
        auto record = decode_record_json(*object);
        if (!record) {
          record.error().line = line_number;
          return tl::unexpected(std::move(record.error()));
        }
        if (record->seq <= last_seq) {
          return tl::unexpected(error(
              SchemaErrorCode::InvalidRecord, "seq",
              "record sequences must be strictly increasing", line_number));
        }
        last_seq = record->seq;
        result.records.push_back(std::move(*record));
      }
      offset = newline + 1U;
      ++line_number;
    }
    if (!has_header) {
      return tl::unexpected(error(SchemaErrorCode::MissingHeader, {},
                                  "no complete header line was found"));
    }
    return result;
  } catch (const std::bad_alloc &) {
    return tl::unexpected(error(SchemaErrorCode::LimitExceeded, {},
                                "NDJSON decoding exhausted available memory"));
  } catch (const std::exception &exception) {
    return tl::unexpected(
        error(SchemaErrorCode::InvalidJson, {}, exception.what()));
  }
}

bool BarrierTracker::mark_processed_through(std::uint64_t seq) noexcept {
  if (seq < processed_through_seq_) {
    return false;
  }
  processed_through_seq_ = seq;
  return true;
}

void BarrierTracker::mark_flush_result(bool succeeded) noexcept {
  has_flush_result_ = true;
  last_flush_succeeded_ = succeeded;
  flush_attempted_through_seq_ = processed_through_seq_;
  if (succeeded) {
    has_successful_flush_ = true;
    flushed_through_seq_ =
        std::max(flushed_through_seq_, processed_through_seq_);
  }
}

BarrierResult BarrierTracker::check(LogBarrier barrier) const noexcept {
  BarrierResult result{BarrierState::WaitingForRecords,
                       barrier.target_seq,
                       processed_through_seq_,
                       flush_attempted_through_seq_,
                       flushed_through_seq_,
                       false};
  if (has_successful_flush_ && flushed_through_seq_ >= barrier.target_seq) {
    result.state = BarrierState::Confirmed;
  } else if (processed_through_seq_ < barrier.target_seq) {
    result.state = BarrierState::WaitingForRecords;
  } else if (has_flush_result_ && !last_flush_succeeded_ &&
             flush_attempted_through_seq_ >= barrier.target_seq) {
    result.state = BarrierState::FlushFailed;
  } else {
    result.state = BarrierState::WaitingForFlush;
  }
  return result;
}

std::uint64_t BarrierTracker::processed_through_seq() const noexcept {
  return processed_through_seq_;
}

std::uint64_t BarrierTracker::flush_attempted_through_seq() const noexcept {
  return flush_attempted_through_seq_;
}

std::uint64_t BarrierTracker::flushed_through_seq() const noexcept {
  return flushed_through_seq_;
}

} // namespace lazycom::logging
