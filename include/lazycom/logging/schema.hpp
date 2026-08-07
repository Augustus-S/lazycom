#pragma once

/**
 * @file
 * @brief Bounded NDJSON session-log value model and codec.
 *
 * Value structs are not self-validating. Codec functions validate untrusted
 * input against physical-line, document, record-count, payload, metadata, and
 * message limits. These functions allocate and are not suitable for emergency
 * or fatal-signal paths.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <tl/expected.hpp>

namespace lazycom::logging {

inline constexpr std::uint32_t kSchemaMajor = 1;
inline constexpr std::uint32_t kSchemaMinor = 0;
inline constexpr std::size_t kMaxPayloadBytes = 1024U * 1024U;
inline constexpr std::size_t kMaxMessageBytes = 4096U;
inline constexpr std::size_t kMaxMetadataBytes = 4096U;
inline constexpr std::size_t kMaxPhysicalLineBytes = 2U * 1024U * 1024U;
inline constexpr std::size_t kMaxNdjsonDocumentBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kMaxNdjsonRecords = 100'000U;

struct SchemaVersion {
  std::uint32_t major{kSchemaMajor};
  std::uint32_t minor{kSchemaMinor};
  auto operator<=>(const SchemaVersion &) const = default;
};

enum class Direction { Rx, Tx, Sys, Err };
enum class PayloadEncoding { Utf8, Base64 };
enum class InputMode { Text, Hex };
enum class Parity { None, Odd, Even, Mark, Space };
enum class FlowControl { None, RtsCts, XonXoff };

using MetadataBytes = std::vector<std::byte>;

struct DeviceMetadata {
  MetadataBytes path;
  std::optional<MetadataBytes> manufacturer;
  std::optional<MetadataBytes> product;
  std::optional<MetadataBytes> serial_number;
  bool operator==(const DeviceMetadata &) const = default;
};

struct SerialSettings {
  std::uint32_t baud{115200};
  std::uint8_t data_bits{8};
  Parity parity{Parity::None};
  std::uint8_t stop_bits{1};
  FlowControl flow_control{FlowControl::None};
  bool operator==(const SerialSettings &) const = default;
};

struct Header {
  SchemaVersion version{};
  std::string started_at;
  std::string session_id;
  DeviceMetadata device;
  SerialSettings serial;
  bool operator==(const Header &) const = default;
};

struct Record {
  std::uint64_t seq{};
  std::string time_utc;
  std::uint64_t elapsed_ns{};
  Direction direction{Direction::Rx};

  /** RX/TX bytes. The encoder derives encoding and ignores its input value. */
  std::vector<std::byte> payload;
  PayloadEncoding encoding{PayloadEncoding::Utf8};
  std::optional<InputMode> input_mode;

  /** SYS/ERR safe text. ERR additionally requires a stable-shaped code. */
  std::string message;
  std::optional<std::string> code;
  bool operator==(const Record &) const = default;
};

enum class SchemaErrorCode {
  InvalidJson,
  InvalidType,
  MissingField,
  InvalidField,
  UnsupportedMajorVersion,
  InvalidUtf8,
  InvalidBase64,
  InvalidUtc,
  LimitExceeded,
  InvalidRecord,
  MissingHeader,
  UnexpectedHeader,
  IncompleteTail,
};

struct SchemaError {
  SchemaErrorCode code{SchemaErrorCode::InvalidField};
  std::size_t line{};
  std::string field;
  std::string detail;
  bool operator==(const SchemaError &) const = default;
};

template <class T> using SchemaResult = tl::expected<T, SchemaError>;

struct NdjsonDocument {
  Header header;
  std::vector<Record> records;
  bool incomplete_tail{};
  std::optional<SchemaError> tail_error;
};

/**
 * @brief Validates structural UTF-8 without terminal-safety filtering.
 * @note Valid controls, NUL, ESC, and bidi formatting code points remain valid
 * UTF-8 and require a separate safe-display projection.
 */
[[nodiscard]] bool is_strict_utf8(std::span<const std::byte> bytes) noexcept;
/**
 * @brief Validates `YYYY-MM-DDTHH:MM:SS[.fraction]Z` UTC timestamps.
 * @note Fractions contain one to nine digits; offsets and leap seconds are not
 * accepted.
 */
[[nodiscard]] bool is_valid_utc(std::string_view value) noexcept;

/**
 * @brief Produces bounded terminal-safe UTF-8 from untrusted message text.
 *
 * Invalid bytes, controls, DEL, and bidi formatting characters are escaped.
 * Output is truncated only at complete UTF-8 or escape boundaries.
 *
 * @warning This is a display-safety projection, not secret redaction and not a
 * reversible payload codec.
 */
[[nodiscard]] std::string sanitize_message(std::string_view message);

/**
 * @brief Encodes a validated header as one ASCII JSON line.
 * @return A line ending in exactly one LF, or a schema error.
 */
[[nodiscard]] SchemaResult<std::string>
encode_header_line(const Header &header);
/** @brief Encodes one validated record as an LF-terminated JSON line. */
[[nodiscard]] SchemaResult<std::string>
encode_record_line(const Record &record);
/**
 * @brief Encodes one header and a strictly increasing record sequence.
 * @return A complete bounded NDJSON document or a schema error.
 */
[[nodiscard]] SchemaResult<std::string>
encode_ndjson(const Header &header, std::span<const Record> records);

/**
 * @brief Decodes one optional-LF/CRLF header line.
 * @note Unknown fields and supported-minor additions are ignored; unknown major
 * versions are rejected.
 */
[[nodiscard]] SchemaResult<Header> decode_header_line(std::string_view line);
/** @brief Decodes one optional-LF/CRLF record line. */
[[nodiscard]] SchemaResult<Record> decode_record_line(std::string_view line);
/**
 * @brief Decodes a bounded NDJSON document with a leading header.
 *
 * A final unterminated line is reported as an incomplete tail and earlier
 * complete records are retained. Any malformed complete line rejects the
 * document.
 */
[[nodiscard]] SchemaResult<NdjsonDocument>
decode_ndjson(std::string_view input);

struct LogBarrier {
  std::uint64_t target_seq{};
  auto operator<=>(const LogBarrier &) const = default;
};

enum class BarrierState {
  WaitingForRecords,
  WaitingForFlush,
  FlushFailed,
  Confirmed,
  /** Worker-level failure; BarrierTracker itself never emits this state. */
  WriterFailed
};

struct BarrierResult {
  BarrierState state{BarrierState::WaitingForRecords};
  std::uint64_t target_seq{};
  std::uint64_t processed_through_seq{};
  std::uint64_t flush_attempted_through_seq{};
  std::uint64_t flushed_through_seq{};
  /** Session barriers describe write/flush, never fsync durability. */
  bool fsync_guaranteed{false};
};

/**
 * @brief Single-owner watermarks for processed records and user-space flushes.
 *
 * A processed watermark may advance over intentionally filtered records or
 * known gaps; it does not prove every intervening sequence was written.
 * Confirmation never implies fsync or physical-media durability.
 */
class BarrierTracker {
public:
  /** @return false for a regressing watermark; equal values are accepted. */
  [[nodiscard]] bool mark_processed_through(std::uint64_t seq) noexcept;
  /** @brief Records a flush attempt through the current processed watermark. */
  void mark_flush_result(bool succeeded) noexcept;
  /** @brief Evaluates a target against the current process/flush watermarks. */
  [[nodiscard]] BarrierResult check(LogBarrier barrier) const noexcept;

  [[nodiscard]] std::uint64_t processed_through_seq() const noexcept;
  [[nodiscard]] std::uint64_t flush_attempted_through_seq() const noexcept;
  [[nodiscard]] std::uint64_t flushed_through_seq() const noexcept;

private:
  std::uint64_t processed_through_seq_{};
  std::uint64_t flush_attempted_through_seq_{};
  std::uint64_t flushed_through_seq_{};
  bool has_successful_flush_{};
  bool has_flush_result_{};
  bool last_flush_succeeded_{};
};

} // namespace lazycom::logging
