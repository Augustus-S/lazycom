#pragma once

/**
 * @file
 * @brief Bounded TOML configuration models, validation, loading, and writing.
 *
 * Snapshot structs are plain values and are not self-validating. Call the
 * matching validator before applying a programmatically constructed snapshot.
 * Parsers reject an entire invalid candidate rather than merging defaults into
 * it, and serializers validate before producing output.
 */

#include <lazycom/base/error.hpp>
#include <lazycom/config/safe_file.hpp>

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lazycom::config {

inline constexpr std::int64_t kSchemaVersion = 1;
inline constexpr std::size_t kConfigMaximumBytes = 1024U * 1024U;
inline constexpr std::size_t kQuickSendMaximumBytes = 4U * 1024U * 1024U;
inline constexpr std::size_t kStateMaximumBytes = 64U * 1024U;
inline constexpr std::size_t kMaximumPayloadBytes = 1024U * 1024U;
inline constexpr std::array<std::int32_t, 18> kBaudPresets{
    300,    600,    1200,   2400,   4800,   9600,   19200,   38400,   57600,
    115200, 230400, 250000, 460800, 500000, 921600, 1000000, 1500000, 2000000};

enum class Parity { None, Odd, Even, Mark, Space };
enum class FlowControl { None, RtsCts, XonXoff };
enum class SendMode { Txt, Hex };
enum class Newline { None, Lf, Cr, CrLf, Session };
enum class ReceiveView { Txt, Hex, Mixed };
enum class UiBackground { RosePine, Transparent };

enum class ConfigurationScope {
  HardwareConnectionSnapshot,
  RuntimeSend,
  DisplayOnly,
  GlobalDefault,
};

enum class ConfigurationField {
  Device,
  Baud,
  DataBits,
  StopBits,
  Parity,
  FlowControl,
  SendMode,
  Newline,
  ReceiveView,
  DirectionFilter,
  LoggingDefaults,
  QuickSendSlots,
  NewSessionDefaults,
};

[[nodiscard]] constexpr ConfigurationScope
configuration_scope(const ConfigurationField field) noexcept {
  switch (field) {
  case ConfigurationField::Device:
  case ConfigurationField::Baud:
  case ConfigurationField::DataBits:
  case ConfigurationField::StopBits:
  case ConfigurationField::Parity:
  case ConfigurationField::FlowControl:
    return ConfigurationScope::HardwareConnectionSnapshot;
  case ConfigurationField::SendMode:
  case ConfigurationField::Newline:
    return ConfigurationScope::RuntimeSend;
  case ConfigurationField::ReceiveView:
  case ConfigurationField::DirectionFilter:
    return ConfigurationScope::DisplayOnly;
  case ConfigurationField::LoggingDefaults:
  case ConfigurationField::QuickSendSlots:
  case ConfigurationField::NewSessionDefaults:
    return ConfigurationScope::GlobalDefault;
  }
  return ConfigurationScope::GlobalDefault;
}

[[nodiscard]] constexpr bool
mutable_while_connected(const ConfigurationField field) noexcept {
  return configuration_scope(field) !=
         ConfigurationScope::HardwareConnectionSnapshot;
}

struct SerialDefaults {
  std::int32_t baud{115200};
  std::int32_t data_bits{8};
  std::int32_t stop_bits{1};
  Parity parity{Parity::None};
  FlowControl flow_control{FlowControl::None};
  auto operator<=>(const SerialDefaults &) const = default;
};

struct SendSettings {
  SendMode mode{SendMode::Txt};
  Newline newline{Newline::None};
  std::uint32_t max_draft_bytes{1048576};
  std::uint32_t history_max_entries{1000};
  std::uint32_t history_max_mib{8};
  auto operator<=>(const SendSettings &) const = default;
};

struct ReceiveSettings {
  ReceiveView rx_view{ReceiveView::Txt};
  ReceiveView tx_view{ReceiveView::Txt};
  std::uint32_t idle_gap_ms{50};
  std::uint32_t max_frame_bytes{65536};
  std::uint32_t visible_buffer_mib{32};
  std::uint32_t visible_max_records{100000};
  auto operator<=>(const ReceiveSettings &) const = default;
};

struct UiSettings {
  UiBackground background{UiBackground::RosePine};
  auto operator<=>(const UiSettings &) const = default;
};

struct LoggingSettings {
  bool default_enabled{false};
  std::string directory;
  std::uint32_t max_files{100};
  std::uint32_t max_total_size_mib{1024};
  std::uint32_t max_file_size_mib{64};
  std::uint32_t flush_interval_ms{1000};
  bool include_system{true};
  bool include_error{true};
  auto operator<=>(const LoggingSettings &) const = default;
};

struct QueueSettings {
  std::uint32_t tx_max_messages{256};
  std::uint32_t tx_max_mib{4};
  std::uint32_t owner_command_max_messages{256};
  std::uint32_t owner_command_max_mib{1};
  std::uint32_t rx_ingress_max_blocks{4096};
  std::uint32_t rx_ingress_max_mib{4};
  std::uint32_t log_max_messages{4096};
  std::uint32_t log_max_mib{8};
  auto operator<=>(const QueueSettings &) const = default;
};

struct TimeoutSettings {
  std::uint32_t connect_ms{5000};
  std::uint32_t tx_ms{5000};
  std::uint32_t owner_stop_ms{5000};
  std::uint32_t log_barrier_ms{5000};
  auto operator<=>(const TimeoutSettings &) const = default;
};

struct ManagedMemoryBudget {
  std::uint32_t ui_visible_mib{};
  std::uint32_t session_log_mib{};
  std::uint32_t rx_ingress_mib{};
  std::uint32_t tx_mib{};
  std::uint32_t send_history_mib{};
  std::uint32_t diagnostics_mib{};
  std::uint32_t owner_and_scratch_mib{};
  std::uint32_t model_and_search_mib{};

  [[nodiscard]] std::uint64_t total_mib() const noexcept;
  auto operator<=>(const ManagedMemoryBudget &) const = default;
};

/** @brief Complete validated candidate for config.toml. */
struct ConfigSnapshot {
  std::int64_t version{kSchemaVersion};
  SerialDefaults serial;
  SendSettings send;
  ReceiveSettings receive;
  UiSettings ui;
  LoggingSettings logging;
  QueueSettings queues;
  TimeoutSettings timeouts;
  auto operator<=>(const ConfigSnapshot &) const = default;
};

struct QuickSendSlot {
  std::uint32_t index{};
  std::string name;
  SendMode mode{SendMode::Txt};
  std::string content;
  Newline newline{Newline::Session};
  std::string note;
  auto operator<=>(const QuickSendSlot &) const = default;
};

struct QuickSendSnapshot {
  std::int64_t version{kSchemaVersion};
  std::array<std::optional<QuickSendSlot>, 20> slots{};
  auto operator<=>(const QuickSendSnapshot &) const = default;
};

struct StateSnapshot {
  std::int64_t version{kSchemaVersion};
  std::uint32_t last_quick_send_slot{1};
  std::uint32_t last_interval_ms{0};
  bool show_rx{true};
  bool show_tx{true};
  bool show_system{true};
  bool show_error{true};
  auto operator<=>(const StateSnapshot &) const = default;
};

struct SchemaMessage {
  std::string path;
  std::string message;
  auto operator<=>(const SchemaMessage &) const = default;
};

/**
 * @brief Result of parsing or safely loading one complete snapshot.
 *
 * accepted is true only when snapshot represents the complete accepted
 * candidate. A rejected document returns the built-in default snapshot with
 * read_only set, preserving the current configuration from partial updates.
 * document and all diagnostic values own their storage.
 */
template <class Snapshot> struct SnapshotLoadResult {
  Snapshot snapshot{};
  bool accepted{};
  bool file_exists{true};
  bool read_only{};
  SafeFileIdentity file_identity;
  std::string document;
  std::vector<SchemaMessage> errors;
  std::vector<SchemaMessage> warnings;
};

using ConfigLoadResult = SnapshotLoadResult<ConfigSnapshot>;
using QuickSendLoadResult = SnapshotLoadResult<QuickSendSnapshot>;
using StateLoadResult = SnapshotLoadResult<StateSnapshot>;

/**
 * @brief Calculates the conservative managed-memory charge of a configuration.
 * @note The calculation saturates and rounds categories upward; it does not
 * validate the snapshot or claim an end-to-end process RSS bound.
 */
[[nodiscard]] ManagedMemoryBudget
managed_memory_budget(const ConfigSnapshot &snapshot) noexcept;
/**
 * @brief Validates all config schema, range, and memory-combination rules.
 * @return All discovered errors. An empty result means the snapshot is valid.
 */
[[nodiscard]] std::vector<SchemaMessage>
validate_config_snapshot(const ConfigSnapshot &snapshot);
/** @brief Validates quick-send slot shape, payloads, and aggregate limits. */
[[nodiscard]] std::vector<SchemaMessage>
validate_quick_send_snapshot(const QuickSendSnapshot &snapshot);
/** @brief Validates persisted UI state and its visibility invariant. */
[[nodiscard]] std::vector<SchemaMessage>
validate_state_snapshot(const StateSnapshot &snapshot);

/**
 * @brief Parses a complete config.toml candidate from owned-or-borrowed text.
 * @return An accepted complete snapshot, or a read-only default snapshot with
 * errors. Syntax, schema, range, version, and resource failures reject the
 * whole candidate. Unknown keys are warnings and can be preserved on write by
 * passing result.document to serialize_config_toml().
 */
[[nodiscard]] ConfigLoadResult parse_config_toml(std::string_view document);
/** @brief Parses a complete, bounded quick_send.toml candidate. */
[[nodiscard]] QuickSendLoadResult
parse_quick_send_toml(std::string_view document);
/** @brief Parses a complete, bounded state.toml candidate. */
[[nodiscard]] StateLoadResult parse_state_toml(std::string_view document);

/**
 * @brief Safely reads and parses config.toml.
 * @return A missing file as an accepted default with file_exists false; unsafe,
 * unreadable, or invalid files produce a rejected read-only result.
 */
[[nodiscard]] ConfigLoadResult
load_config_toml(const std::filesystem::path &path);
/** @brief Safely reads and parses quick_send.toml. */
[[nodiscard]] QuickSendLoadResult
load_quick_send_toml(const std::filesystem::path &path);
/** @brief Safely reads and parses state.toml. */
[[nodiscard]] StateLoadResult
load_state_toml(const std::filesystem::path &path);

/**
 * @brief Validates and serializes a config snapshot as normalized TOML.
 * @param snapshot Complete candidate to serialize.
 * @param preserved_document Previously parsed document whose unknown values
 * should be retained. Comments and original layout are not preserved.
 * @return Canonical TOML ending in one LF, or an Error.
 */
[[nodiscard]] Result<std::string>
serialize_config_toml(const ConfigSnapshot &snapshot,
                      std::string_view preserved_document = {});
/** @brief Validates and serializes a quick-send snapshot as normalized TOML. */
[[nodiscard]] Result<std::string>
serialize_quick_send_toml(const QuickSendSnapshot &snapshot,
                          std::string_view preserved_document = {});
/** @brief Validates and serializes a UI-state snapshot as normalized TOML. */
[[nodiscard]] Result<std::string>
serialize_state_toml(const StateSnapshot &snapshot,
                     std::string_view preserved_document = {});

[[nodiscard]] std::string_view to_string(Parity value) noexcept;
[[nodiscard]] std::string_view to_string(FlowControl value) noexcept;
[[nodiscard]] std::string_view to_string(SendMode value) noexcept;
[[nodiscard]] std::string_view to_string(Newline value) noexcept;
[[nodiscard]] std::string_view to_string(ReceiveView value) noexcept;
[[nodiscard]] std::optional<UiBackground>
parse_ui_background(std::string_view value);
[[nodiscard]] std::string_view to_string(UiBackground value) noexcept;

} // namespace lazycom::config
