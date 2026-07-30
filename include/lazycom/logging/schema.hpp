#pragma once

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

  // RX/TX use payload. The codec always derives encoding from the bytes.
  std::vector<std::byte> payload;
  PayloadEncoding encoding{PayloadEncoding::Utf8};
  std::optional<InputMode> input_mode;

  // SYS/ERR use message. ERR additionally requires code.
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

[[nodiscard]] bool is_strict_utf8(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] bool is_valid_utc(std::string_view value) noexcept;

// Produces valid UTF-8 with controls and bidi formatting characters made
// visible.
[[nodiscard]] std::string sanitize_message(std::string_view message);

// Encoded lines include exactly one trailing LF.
[[nodiscard]] SchemaResult<std::string>
encode_header_line(const Header &header);
[[nodiscard]] SchemaResult<std::string>
encode_record_line(const Record &record);
[[nodiscard]] SchemaResult<std::string>
encode_ndjson(const Header &header, std::span<const Record> records);

[[nodiscard]] SchemaResult<Header> decode_header_line(std::string_view line);
[[nodiscard]] SchemaResult<Record> decode_record_line(std::string_view line);
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
  // Worker-level terminal result. BarrierTracker itself never emits this and
  // retains its existing pure processed/flush behavior.
  WriterFailed
};

struct BarrierResult {
  BarrierState state{BarrierState::WaitingForRecords};
  std::uint64_t target_seq{};
  std::uint64_t processed_through_seq{};
  std::uint64_t flush_attempted_through_seq{};
  std::uint64_t flushed_through_seq{};
  // Session log barriers only describe write/flush, never fsync durability.
  bool fsync_guaranteed{false};
};

class BarrierTracker {
public:
  [[nodiscard]] bool mark_processed_through(std::uint64_t seq) noexcept;
  void mark_flush_result(bool succeeded) noexcept;
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
