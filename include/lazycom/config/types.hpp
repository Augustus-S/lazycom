#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
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

[[nodiscard]] std::string_view to_string(Parity value) noexcept;
[[nodiscard]] std::string_view to_string(FlowControl value) noexcept;
[[nodiscard]] std::string_view to_string(SendMode value) noexcept;
[[nodiscard]] std::string_view to_string(Newline value) noexcept;
[[nodiscard]] std::string_view to_string(ReceiveView value) noexcept;
[[nodiscard]] std::optional<UiBackground>
parse_ui_background(std::string_view value);
[[nodiscard]] std::string_view to_string(UiBackground value) noexcept;

} // namespace lazycom::config
