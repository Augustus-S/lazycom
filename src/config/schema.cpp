#include <lazycom/config/schema.hpp>

#include <lazycom/config/safe_file.hpp>

#include <toml++/toml.hpp>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>

namespace lazycom::config {
namespace {

constexpr std::uint32_t kQueueMaximumCount = 65536;
constexpr std::uint32_t kTimeoutMinimumMs = 100;
constexpr std::uint32_t kTimeoutMaximumMs = 60000;
constexpr std::uint64_t kMebibyte = 1024U * 1024U;
constexpr std::uint64_t kVisibleRecordOverheadBytes = 96U;
constexpr std::uint64_t kLogQueueNodeOverheadBytes = 128U;
constexpr std::uint64_t kRxBlockOverheadBytes = 64U;
constexpr std::uint64_t kTxQueueNodeOverheadBytes = 128U;
constexpr std::uint64_t kHistoryEntryOverheadBytes = 128U;
constexpr std::uint64_t kOwnerCommandOverheadBytes = 256U;
constexpr std::uint64_t kOwnerFixedScratchBytes = 2U * kMebibyte;
constexpr std::uint64_t kDiagnosticsBudgetBytes = 8U * kMebibyte;
constexpr std::uint64_t kQuickSendSlotOverheadBytes = 256U;
constexpr std::uint64_t kQuickSendSnapshotOverheadBytes = 512U;
constexpr std::uint64_t kQuickSendModelLimitBytes = 6U * kMebibyte;
constexpr std::uint64_t kModelCategoryLimitBytes = 8U * kMebibyte;
constexpr std::uint64_t kSearchAndControlBytes =
    (10000U * sizeof(std::uint64_t)) + (512U * 1024U);
constexpr std::uint64_t kConfigStateModelBytes = 256U * 1024U;

[[nodiscard]] constexpr std::uint64_t
saturated_add(const std::uint64_t left, const std::uint64_t right) noexcept {
  return right > std::numeric_limits<std::uint64_t>::max() - left
             ? std::numeric_limits<std::uint64_t>::max()
             : left + right;
}

[[nodiscard]] constexpr std::uint64_t
saturated_multiply(const std::uint64_t left,
                   const std::uint64_t right) noexcept {
  return left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left
             ? std::numeric_limits<std::uint64_t>::max()
             : left * right;
}

[[nodiscard]] constexpr std::uint64_t
mib_bytes(const std::uint32_t value) noexcept {
  return saturated_multiply(value, kMebibyte);
}

[[nodiscard]] constexpr std::uint32_t
bytes_to_mib(const std::uint64_t bytes) noexcept {
  if (bytes == std::numeric_limits<std::uint64_t>::max()) {
    return std::numeric_limits<std::uint32_t>::max();
  }
  const auto rounded = saturated_add(bytes, kMebibyte - 1U) / kMebibyte;
  return static_cast<std::uint32_t>(std::min<std::uint64_t>(
      rounded, std::numeric_limits<std::uint32_t>::max()));
}

void add_error(std::vector<SchemaMessage> &errors, std::string path,
               std::string message) {
  errors.push_back({std::move(path), std::move(message)});
}

[[nodiscard]] std::string ascii_lower(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (const char character : text) {
    result.push_back(character >= 'A' && character <= 'Z'
                         ? static_cast<char>(character + ('a' - 'A'))
                         : character);
  }
  return result;
}

[[nodiscard]] bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  while (index < text.size()) {
    const auto first = static_cast<unsigned char>(text[index]);
    if (first <= 0x7FU) {
      ++index;
      continue;
    }

    std::size_t continuation_count = 0;
    std::uint32_t code_point = 0;
    std::uint32_t minimum = 0;
    if ((first & 0xE0U) == 0xC0U) {
      continuation_count = 1;
      code_point = first & 0x1FU;
      minimum = 0x80U;
    } else if ((first & 0xF0U) == 0xE0U) {
      continuation_count = 2;
      code_point = first & 0x0FU;
      minimum = 0x800U;
    } else if ((first & 0xF8U) == 0xF0U) {
      continuation_count = 3;
      code_point = first & 0x07U;
      minimum = 0x10000U;
    } else {
      return false;
    }
    if (continuation_count > text.size() - index - 1U) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const auto continuation =
          static_cast<unsigned char>(text[index + offset]);
      if ((continuation & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6U) | (continuation & 0x3FU);
    }
    if (code_point < minimum || code_point > 0x10FFFFU ||
        (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
      return false;
    }
    index += continuation_count + 1U;
  }
  return true;
}

[[nodiscard]] bool is_hex_digit(char character) noexcept {
  return (character >= '0' && character <= '9') ||
         (character >= 'a' && character <= 'f') ||
         (character >= 'A' && character <= 'F');
}

[[nodiscard]] bool is_hex_space(char character) noexcept {
  return character == ' ' || character == '\t' || character == '\r' ||
         character == '\n';
}

[[nodiscard]] std::optional<std::size_t>
decoded_hex_size(std::string_view text) noexcept {
  std::size_t index = 0;
  std::size_t result = 0;
  while (true) {
    while (index < text.size() && is_hex_space(text[index])) {
      ++index;
    }
    if (index == text.size()) {
      return result;
    }
    if (index + 1U < text.size() && text[index] == '0' &&
        (text[index + 1U] == 'x' || text[index + 1U] == 'X')) {
      index += 2U;
    }
    if (index + 1U >= text.size() || !is_hex_digit(text[index]) ||
        !is_hex_digit(text[index + 1U])) {
      return std::nullopt;
    }
    index += 2U;
    ++result;
    if (index < text.size() && !is_hex_space(text[index])) {
      return std::nullopt;
    }
  }
}

[[nodiscard]] std::uint64_t
quick_send_memory_bytes(const QuickSendSnapshot &snapshot) noexcept {
  auto total = kQuickSendSnapshotOverheadBytes;
  for (const auto &optional_slot : snapshot.slots) {
    if (!optional_slot) {
      continue;
    }
    const auto &slot = *optional_slot;
    total = saturated_add(total, kQuickSendSlotOverheadBytes);
    total = saturated_add(total, slot.name.capacity());
    total = saturated_add(total, slot.note.capacity());
    total = saturated_add(total, slot.content.capacity());
    if (slot.mode == SendMode::Txt) {
      total = saturated_add(total, slot.content.size());
    } else if (slot.mode == SendMode::Hex) {
      const auto decoded = decoded_hex_size(slot.content);
      if (decoded) {
        total = saturated_add(total, *decoded);
      }
    }
  }
  return total;
}

template <class Integer>
void validate_range(std::vector<SchemaMessage> &errors, std::string path,
                    Integer value, Integer minimum, Integer maximum) {
  if (value < minimum || value > maximum) {
    add_error(errors, std::move(path),
              "must be in range " + std::to_string(minimum) + ".." +
                  std::to_string(maximum));
  }
}

void validate_timeout(std::vector<SchemaMessage> &errors, std::string path,
                      std::uint32_t value) {
  validate_range(errors, std::move(path), value, kTimeoutMinimumMs,
                 kTimeoutMaximumMs);
}

[[nodiscard]] const toml::table *
optional_table(const toml::table &parent, std::string_view key,
               std::string path, std::vector<SchemaMessage> &errors) {
  const auto *node = parent.get(key);
  if (node == nullptr) {
    return nullptr;
  }
  const auto *table = node->as_table();
  if (table == nullptr) {
    add_error(errors, std::move(path), "must be a table");
  }
  return table;
}

template <class Integer>
void read_integer(const toml::table *table, std::string_view key,
                  std::string path, std::int64_t minimum, std::int64_t maximum,
                  Integer &destination, std::vector<SchemaMessage> &errors) {
  if (table == nullptr) {
    return;
  }
  const auto *node = table->get(key);
  if (node == nullptr) {
    return;
  }
  const auto value = node->value<std::int64_t>();
  if (!value) {
    add_error(errors, std::move(path), "must be an integer");
    return;
  }
  if (*value < minimum || *value > maximum) {
    add_error(errors, std::move(path),
              "must be in range " + std::to_string(minimum) + ".." +
                  std::to_string(maximum));
    return;
  }
  destination = static_cast<Integer>(*value);
}

void read_bool(const toml::table *table, std::string_view key, std::string path,
               bool &destination, std::vector<SchemaMessage> &errors) {
  if (table == nullptr) {
    return;
  }
  const auto *node = table->get(key);
  if (node == nullptr) {
    return;
  }
  const auto value = node->value<bool>();
  if (!value) {
    add_error(errors, std::move(path), "must be a boolean");
    return;
  }
  destination = *value;
}

void read_string(const toml::table *table, std::string_view key,
                 std::string path, std::string &destination,
                 std::vector<SchemaMessage> &errors) {
  if (table == nullptr) {
    return;
  }
  const auto *node = table->get(key);
  if (node == nullptr) {
    return;
  }
  const auto value = node->value<std::string>();
  if (!value) {
    add_error(errors, std::move(path), "must be a string");
    return;
  }
  destination = *value;
}

template <class Enum, class Parser>
void read_enum(const toml::table *table, std::string_view key, std::string path,
               std::string allowed, Enum &destination,
               std::vector<SchemaMessage> &errors, Parser parser) {
  if (table == nullptr) {
    return;
  }
  const auto *node = table->get(key);
  if (node == nullptr) {
    return;
  }
  const auto value = node->value<std::string>();
  if (!value) {
    add_error(errors, std::move(path), "must be a string enum");
    return;
  }
  const auto parsed = parser(ascii_lower(*value));
  if (!parsed) {
    add_error(errors, std::move(path), "must be one of " + allowed);
    return;
  }
  destination = *parsed;
}

[[nodiscard]] std::optional<Parity> parse_parity(std::string_view value) {
  if (value == "none")
    return Parity::None;
  if (value == "odd")
    return Parity::Odd;
  if (value == "even")
    return Parity::Even;
  if (value == "mark")
    return Parity::Mark;
  if (value == "space")
    return Parity::Space;
  return std::nullopt;
}

[[nodiscard]] std::optional<FlowControl>
parse_flow_control(std::string_view value) {
  if (value == "none")
    return FlowControl::None;
  if (value == "rts/cts")
    return FlowControl::RtsCts;
  if (value == "xon/xoff")
    return FlowControl::XonXoff;
  return std::nullopt;
}

[[nodiscard]] std::optional<SendMode> parse_send_mode(std::string_view value) {
  if (value == "txt")
    return SendMode::Txt;
  if (value == "hex")
    return SendMode::Hex;
  return std::nullopt;
}

[[nodiscard]] std::optional<Newline> parse_newline(std::string_view value) {
  if (value == "none")
    return Newline::None;
  if (value == "lf")
    return Newline::Lf;
  if (value == "cr")
    return Newline::Cr;
  if (value == "crlf")
    return Newline::CrLf;
  if (value == "session")
    return Newline::Session;
  return std::nullopt;
}

[[nodiscard]] std::optional<ReceiveView>
parse_receive_view(std::string_view value) {
  if (value == "txt")
    return ReceiveView::Txt;
  if (value == "hex")
    return ReceiveView::Hex;
  if (value == "mixed")
    return ReceiveView::Mixed;
  return std::nullopt;
}

[[nodiscard]] std::optional<ReceiveView>
parse_legacy_receive_view(std::string_view value) {
  if (value == "text" || value == "txt")
    return ReceiveView::Txt;
  return parse_receive_view(value);
}

void warn_unknown_keys(const toml::table *table,
                       std::initializer_list<std::string_view> known,
                       std::string_view prefix,
                       std::vector<SchemaMessage> &warnings) {
  if (table == nullptr) {
    return;
  }
  for (const auto &[key, unused] : *table) {
    static_cast<void>(unused);
    const auto name = key.str();
    if (std::find(known.begin(), known.end(), name) == known.end()) {
      warnings.push_back({prefix.empty()
                              ? std::string{name}
                              : std::string{prefix} + "." + std::string{name},
                          "unknown key is preserved"});
    }
  }
}

void read_version(const toml::table &table, std::int64_t &version,
                  std::vector<SchemaMessage> &errors) {
  const auto *node = table.get("version");
  if (node == nullptr) {
    add_error(errors, "version", "is required");
    return;
  }
  const auto value = node->value<std::int64_t>();
  if (!value) {
    add_error(errors, "version", "must be an integer");
    return;
  }
  version = *value;
  if (version != kSchemaVersion) {
    add_error(errors, "version", "unsupported major version");
  }
}

[[nodiscard]] ConfigLoadResult parse_config_table(toml::table table,
                                                  std::string document) {
  ConfigLoadResult result;
  result.document = std::move(document);
  ConfigSnapshot candidate;
  read_version(table, candidate.version, result.errors);

  const auto *serial = optional_table(table, "serial", "serial", result.errors);
  const auto *serial_defaults =
      serial == nullptr ? nullptr
                        : optional_table(*serial, "defaults", "serial.defaults",
                                         result.errors);
  const auto *send = optional_table(table, "send", "send", result.errors);
  const auto *receive =
      optional_table(table, "receive", "receive", result.errors);
  const auto *ui = optional_table(table, "ui", "ui", result.errors);
  const auto *logging =
      optional_table(table, "logging", "logging", result.errors);
  const auto *queues = optional_table(table, "queues", "queues", result.errors);
  const auto *timeouts =
      optional_table(table, "timeouts", "timeouts", result.errors);

  read_integer(serial_defaults, "baud", "serial.defaults.baud", 1, INT_MAX,
               candidate.serial.baud, result.errors);
  read_integer(serial_defaults, "data_bits", "serial.defaults.data_bits", 5, 8,
               candidate.serial.data_bits, result.errors);
  read_integer(serial_defaults, "stop_bits", "serial.defaults.stop_bits", 1, 2,
               candidate.serial.stop_bits, result.errors);
  read_enum(serial_defaults, "parity", "serial.defaults.parity",
            "none, odd, even, mark, space", candidate.serial.parity,
            result.errors, parse_parity);
  read_enum(serial_defaults, "flow_control", "serial.defaults.flow_control",
            "none, rts/cts, xon/xoff", candidate.serial.flow_control,
            result.errors, parse_flow_control);

  read_enum(send, "mode", "send.mode", "txt, hex", candidate.send.mode,
            result.errors, parse_send_mode);
  read_enum(send, "newline", "send.newline", "none, lf, cr, crlf",
            candidate.send.newline, result.errors,
            [](std::string_view value) -> std::optional<Newline> {
              const auto parsed = parse_newline(value);
              return parsed && *parsed != Newline::Session ? parsed
                                                           : std::nullopt;
            });
  read_integer(send, "max_draft_bytes", "send.max_draft_bytes", 1,
               static_cast<std::int64_t>(kMaximumPayloadBytes),
               candidate.send.max_draft_bytes, result.errors);
  read_integer(send, "history_max_entries", "send.history_max_entries", 1,
               10000, candidate.send.history_max_entries, result.errors);
  read_integer(send, "history_max_mib", "send.history_max_mib", 1, 16,
               candidate.send.history_max_mib, result.errors);

  read_enum(receive, "rx_view", "receive.rx_view", "txt, hex, mixed",
            candidate.receive.rx_view, result.errors, parse_receive_view);
  read_enum(receive, "tx_view", "receive.tx_view", "txt, hex, mixed",
            candidate.receive.tx_view, result.errors, parse_receive_view);
  const bool has_legacy_view = receive != nullptr && receive->contains("view");
  if (has_legacy_view) {
    const bool has_rx_view = receive->contains("rx_view");
    const bool has_tx_view = receive->contains("tx_view");
    if (!has_rx_view || !has_tx_view) {
      ReceiveView legacy_view{ReceiveView::Txt};
      read_enum(receive, "view", "receive.view", "text, hex, mixed",
                legacy_view, result.errors, parse_legacy_receive_view);
      if (!has_rx_view) {
        candidate.receive.rx_view = legacy_view;
      }
      if (!has_tx_view) {
        candidate.receive.tx_view = legacy_view;
      }
    }
    result.warnings.push_back(
        {"receive.view", has_rx_view && has_tx_view
                             ? "legacy key is ignored and will be removed"
                             : "legacy key is migrated and will be removed"});
  }
  if (send != nullptr && send->contains("keep_after_send")) {
    result.warnings.push_back(
        {"send.keep_after_send",
         "deprecated key is ignored and will be removed"});
  }
  read_integer(receive, "idle_gap_ms", "receive.idle_gap_ms", 1, 60000,
               candidate.receive.idle_gap_ms, result.errors);
  read_integer(receive, "max_frame_bytes", "receive.max_frame_bytes", 256,
               65536, candidate.receive.max_frame_bytes, result.errors);
  read_integer(receive, "visible_buffer_mib", "receive.visible_buffer_mib", 1,
               48, candidate.receive.visible_buffer_mib, result.errors);
  read_integer(receive, "visible_max_records", "receive.visible_max_records",
               1000, 1000000, candidate.receive.visible_max_records,
               result.errors);

  read_enum(ui, "background", "ui.background", "rose-pine, transparent",
            candidate.ui.background, result.errors, parse_ui_background);

  read_bool(logging, "default_enabled", "logging.default_enabled",
            candidate.logging.default_enabled, result.errors);
  read_string(logging, "directory", "logging.directory",
              candidate.logging.directory, result.errors);
  read_integer(logging, "max_files", "logging.max_files", 1, 10000,
               candidate.logging.max_files, result.errors);
  read_integer(logging, "max_total_size_mib", "logging.max_total_size_mib", 1,
               65536, candidate.logging.max_total_size_mib, result.errors);
  read_integer(logging, "max_file_size_mib", "logging.max_file_size_mib", 1,
               1024, candidate.logging.max_file_size_mib, result.errors);
  read_integer(logging, "flush_interval_ms", "logging.flush_interval_ms", 1,
               60000, candidate.logging.flush_interval_ms, result.errors);
  read_bool(logging, "include_system", "logging.include_system",
            candidate.logging.include_system, result.errors);
  read_bool(logging, "include_error", "logging.include_error",
            candidate.logging.include_error, result.errors);

  read_integer(queues, "tx_max_messages", "queues.tx_max_messages", 1,
               kQueueMaximumCount, candidate.queues.tx_max_messages,
               result.errors);
  read_integer(queues, "tx_max_mib", "queues.tx_max_mib", 1, 8,
               candidate.queues.tx_max_mib, result.errors);
  read_integer(queues, "owner_command_max_messages",
               "queues.owner_command_max_messages", 1, kQueueMaximumCount,
               candidate.queues.owner_command_max_messages, result.errors);
  read_integer(queues, "owner_command_max_mib", "queues.owner_command_max_mib",
               1, 2, candidate.queues.owner_command_max_mib, result.errors);
  read_integer(queues, "rx_ingress_max_blocks", "queues.rx_ingress_max_blocks",
               1, kQueueMaximumCount, candidate.queues.rx_ingress_max_blocks,
               result.errors);
  read_integer(queues, "rx_ingress_max_mib", "queues.rx_ingress_max_mib", 1, 8,
               candidate.queues.rx_ingress_max_mib, result.errors);
  read_integer(queues, "log_max_messages", "queues.log_max_messages", 1,
               kQueueMaximumCount, candidate.queues.log_max_messages,
               result.errors);
  read_integer(queues, "log_max_mib", "queues.log_max_mib", 1, 16,
               candidate.queues.log_max_mib, result.errors);

  read_integer(timeouts, "connect_ms", "timeouts.connect_ms", kTimeoutMinimumMs,
               kTimeoutMaximumMs, candidate.timeouts.connect_ms, result.errors);
  read_integer(timeouts, "tx_ms", "timeouts.tx_ms", kTimeoutMinimumMs,
               kTimeoutMaximumMs, candidate.timeouts.tx_ms, result.errors);
  read_integer(timeouts, "owner_stop_ms", "timeouts.owner_stop_ms",
               kTimeoutMinimumMs, kTimeoutMaximumMs,
               candidate.timeouts.owner_stop_ms, result.errors);
  read_integer(timeouts, "log_barrier_ms", "timeouts.log_barrier_ms",
               kTimeoutMinimumMs, kTimeoutMaximumMs,
               candidate.timeouts.log_barrier_ms, result.errors);

  warn_unknown_keys(&table,
                    {"version", "serial", "send", "receive", "ui", "logging",
                     "queues", "timeouts"},
                    "", result.warnings);
  warn_unknown_keys(serial, {"defaults"}, "serial", result.warnings);
  warn_unknown_keys(
      serial_defaults,
      {"baud", "data_bits", "stop_bits", "parity", "flow_control"},
      "serial.defaults", result.warnings);
  warn_unknown_keys(send,
                    {"mode", "newline", "keep_after_send", "max_draft_bytes",
                     "history_max_entries", "history_max_mib"},
                    "send", result.warnings);
  warn_unknown_keys(receive,
                    {"view", "rx_view", "tx_view", "idle_gap_ms",
                     "max_frame_bytes", "visible_buffer_mib",
                     "visible_max_records"},
                    "receive", result.warnings);
  warn_unknown_keys(ui, {"background"}, "ui", result.warnings);
  warn_unknown_keys(logging,
                    {"default_enabled", "directory", "max_files",
                     "max_total_size_mib", "max_file_size_mib",
                     "flush_interval_ms", "include_system", "include_error"},
                    "logging", result.warnings);
  warn_unknown_keys(queues,
                    {"tx_max_messages", "tx_max_mib",
                     "owner_command_max_messages", "owner_command_max_mib",
                     "rx_ingress_max_blocks", "rx_ingress_max_mib",
                     "log_max_messages", "log_max_mib"},
                    "queues", result.warnings);
  warn_unknown_keys(timeouts,
                    {"connect_ms", "tx_ms", "owner_stop_ms", "log_barrier_ms"},
                    "timeouts", result.warnings);

  auto validation = validate_config_snapshot(candidate);
  result.errors.insert(result.errors.end(),
                       std::make_move_iterator(validation.begin()),
                       std::make_move_iterator(validation.end()));
  if (result.errors.empty()) {
    result.snapshot = std::move(candidate);
    result.accepted = true;
  } else {
    result.read_only = true;
  }
  return result;
}

void validate_quick_slot(const QuickSendSlot &slot, std::string_view base,
                         std::vector<SchemaMessage> &errors) {
  const auto path = [base](std::string_view field) {
    return std::string{base} + "." + std::string{field};
  };
  if (slot.name.size() > 64U) {
    add_error(errors, path("name"), "must not exceed 64 UTF-8 bytes");
  }
  if (!is_valid_utf8(slot.name)) {
    add_error(errors, path("name"), "must be valid UTF-8");
  }
  if (slot.note.size() > 256U) {
    add_error(errors, path("note"), "must not exceed 256 UTF-8 bytes");
  }
  if (!is_valid_utf8(slot.note)) {
    add_error(errors, path("note"), "must be valid UTF-8");
  }
  if (slot.newline != Newline::Session && slot.newline != Newline::None &&
      slot.newline != Newline::Lf && slot.newline != Newline::Cr &&
      slot.newline != Newline::CrLf) {
    add_error(errors, path("newline"), "is invalid");
  }
  if (slot.mode == SendMode::Txt) {
    if (!is_valid_utf8(slot.content)) {
      add_error(errors, path("content"), "TXT content must be valid UTF-8");
    }
    if (slot.content.size() > kMaximumPayloadBytes) {
      add_error(errors, path("content"), "decoded content exceeds 1 MiB");
    }
  } else if (slot.mode == SendMode::Hex) {
    const auto size = decoded_hex_size(slot.content);
    if (!size) {
      add_error(errors, path("content"), "must be valid HEX bytes");
    } else if (*size > kMaximumPayloadBytes) {
      add_error(errors, path("content"), "decoded content exceeds 1 MiB");
    }
  } else {
    add_error(errors, path("mode"), "is invalid");
  }
}

[[nodiscard]] QuickSendLoadResult parse_quick_send_table(toml::table table,
                                                         std::string document) {
  QuickSendLoadResult result;
  result.document = std::move(document);
  QuickSendSnapshot candidate;
  read_version(table, candidate.version, result.errors);
  warn_unknown_keys(&table, {"version", "slots"}, "", result.warnings);

  const auto *slots_node = table.get("slots");
  const auto *slots = slots_node == nullptr ? nullptr : slots_node->as_array();
  if (slots_node != nullptr && slots == nullptr) {
    add_error(result.errors, "slots", "must be an array of tables");
  }
  std::array<bool, 20> occupied{};
  if (slots != nullptr) {
    if (slots->size() > candidate.slots.size()) {
      add_error(result.errors, "slots", "must not contain more than 20 slots");
    }
    const auto slots_to_parse = std::min(slots->size(), candidate.slots.size());
    for (std::size_t position = 0; position < slots_to_parse; ++position) {
      const auto base = "slots[" + std::to_string(position) + "]";
      const auto *slot_table = slots->get(position)->as_table();
      if (slot_table == nullptr) {
        add_error(result.errors, base, "must be a table");
        continue;
      }
      warn_unknown_keys(slot_table,
                        {"index", "name", "mode", "content", "newline", "note"},
                        base, result.warnings);
      const auto *index_node = slot_table->get("index");
      if (index_node == nullptr) {
        // A table without an index represents an intentionally empty slot.
        continue;
      }
      const auto index_value = index_node->value<std::int64_t>();
      if (!index_value) {
        add_error(result.errors, base + ".index", "must be an integer");
        continue;
      }
      if (*index_value < 1 || *index_value > 20) {
        add_error(result.errors, base + ".index", "must be in range 1..20");
        continue;
      }
      const auto slot_index = static_cast<std::size_t>(*index_value - 1);
      QuickSendSlot slot;
      slot.index = static_cast<std::uint32_t>(*index_value);
      read_string(slot_table, "name", base + ".name", slot.name, result.errors);
      read_enum(slot_table, "mode", base + ".mode", "txt, hex", slot.mode,
                result.errors, parse_send_mode);
      read_string(slot_table, "content", base + ".content", slot.content,
                  result.errors);
      read_enum(slot_table, "newline", base + ".newline",
                "session, none, lf, cr, crlf", slot.newline, result.errors,
                parse_newline);
      read_string(slot_table, "note", base + ".note", slot.note, result.errors);
      validate_quick_slot(slot, base, result.errors);
      if (occupied[slot_index]) {
        add_error(result.errors, base + ".index", "must not be duplicated");
      } else {
        occupied[slot_index] = true;
        candidate.slots[slot_index] = std::move(slot);
      }
    }
  }
  const auto snapshot_bytes = quick_send_memory_bytes(candidate);
  if (snapshot_bytes > kQuickSendModelLimitBytes) {
    add_error(result.errors, "budget.model_and_search_mib",
              "quick-send snapshot exceeds its model memory reservation");
  }
  if (saturated_add(result.document.size(), snapshot_bytes) >
      kModelCategoryLimitBytes) {
    add_error(result.errors, "$document",
              "preserved document and quick-send snapshot exceed 8 MiB");
  }
  if (result.errors.empty()) {
    result.snapshot = std::move(candidate);
    result.accepted = true;
  } else {
    result.read_only = true;
  }
  return result;
}

[[nodiscard]] StateLoadResult parse_state_table(toml::table table,
                                                std::string document) {
  StateLoadResult result;
  result.document = std::move(document);
  StateSnapshot candidate;
  read_version(table, candidate.version, result.errors);
  read_integer(&table, "last_quick_send_slot", "last_quick_send_slot", 1, 20,
               candidate.last_quick_send_slot, result.errors);
  read_integer(&table, "last_interval_ms", "last_interval_ms", 0, 86400000,
               candidate.last_interval_ms, result.errors);
  read_bool(&table, "show_rx", "show_rx", candidate.show_rx, result.errors);
  read_bool(&table, "show_tx", "show_tx", candidate.show_tx, result.errors);
  read_bool(&table, "show_system", "show_system", candidate.show_system,
            result.errors);
  read_bool(&table, "show_error", "show_error", candidate.show_error,
            result.errors);
  warn_unknown_keys(&table,
                    {"version", "last_quick_send_slot", "last_interval_ms",
                     "show_rx", "show_tx", "show_system", "show_error"},
                    "", result.warnings);
  auto validation = validate_state_snapshot(candidate);
  result.errors.insert(result.errors.end(),
                       std::make_move_iterator(validation.begin()),
                       std::make_move_iterator(validation.end()));
  if (result.errors.empty()) {
    result.snapshot = candidate;
    result.accepted = true;
  } else {
    result.read_only = true;
  }
  return result;
}

template <class ResultType, class Parser>
[[nodiscard]] ResultType parse_document(std::string_view document,
                                        std::size_t maximum_bytes,
                                        Parser parser) {
  if (document.size() > maximum_bytes) {
    ResultType result;
    result.read_only = true;
    add_error(result.errors, "$document",
              "TOML document exceeds its maximum size");
    return result;
  }
  try {
    auto table = toml::parse(document);
    return parser(std::move(table), std::string{document});
  } catch (const toml::parse_error &error) {
    ResultType result;
    result.document = std::string{document};
    result.read_only = true;
    const auto &source = error.source();
    add_error(result.errors, "$syntax",
              std::string{error.description()} + " at line " +
                  std::to_string(source.begin.line) + ", column " +
                  std::to_string(source.begin.column));
    return result;
  } catch (const std::exception &error) {
    ResultType result;
    result.document = std::string{document};
    result.read_only = true;
    add_error(result.errors, "$parse", error.what());
    return result;
  } catch (...) {
    ResultType result;
    result.document = std::string{document};
    result.read_only = true;
    add_error(result.errors, "$parse", "TOML parser failed");
    return result;
  }
}

template <class ResultType, class Parser>
[[nodiscard]] ResultType load_document(const std::filesystem::path &path,
                                       std::size_t maximum_bytes,
                                       Parser parser) {
  auto file = read_safe_file(path, maximum_bytes);
  if (!file) {
    ResultType result;
    result.file_exists = true;
    result.read_only = true;
    add_error(result.errors, "$file", file.error().detail);
    return result;
  }
  if (!file->identity.exists) {
    ResultType result;
    result.file_exists = false;
    result.accepted = true;
    result.file_identity = file->identity;
    return result;
  }
  auto result = parser(file->bytes);
  result.file_exists = true;
  result.file_identity = file->identity;
  return result;
}

[[nodiscard]] toml::table &ensure_table(toml::table &parent,
                                        std::string_view key) {
  if (auto *table = parent[key].as_table()) {
    return *table;
  }
  parent.insert_or_assign(key, toml::table{});
  return *parent[key].as_table();
}

[[nodiscard]] Result<std::string>
serialization_failure(std::string detail,
                      ErrorCode code = ErrorCode::ConfigParseFailed) {
  return tl::make_unexpected(
      make_error(code, Operation::SaveConfig, std::move(detail)));
}

[[nodiscard]] Result<toml::table> preserved_table(std::string_view document,
                                                  std::size_t maximum_bytes) {
  if (document.size() > maximum_bytes) {
    return tl::make_unexpected(
        make_error(ErrorCode::ConfigIoFailed, Operation::SaveConfig,
                   "preserved TOML exceeds its maximum size"));
  }
  try {
    return document.empty() ? toml::table{} : toml::parse(document);
  } catch (const toml::parse_error &error) {
    return tl::make_unexpected(
        make_error(ErrorCode::ConfigParseFailed, Operation::SaveConfig,
                   std::string{"cannot rewrite invalid TOML: "} +
                       std::string{error.description()}));
  } catch (const std::exception &error) {
    return tl::make_unexpected(make_error(ErrorCode::ConfigParseFailed,
                                          Operation::SaveConfig, error.what()));
  } catch (...) {
    return tl::make_unexpected(make_error(ErrorCode::ConfigParseFailed,
                                          Operation::SaveConfig,
                                          "cannot parse preserved TOML"));
  }
}

[[nodiscard]] Result<std::string> format_table(toml::table table,
                                               std::size_t maximum_bytes) {
  try {
    std::ostringstream stream;
    stream << table;
    stream << '\n';
    auto result = stream.str();
    if (result.size() > maximum_bytes) {
      return serialization_failure("serialized TOML exceeds its maximum size",
                                   ErrorCode::ConfigIoFailed);
    }
    return result;
  } catch (const std::exception &error) {
    return serialization_failure(error.what());
  } catch (...) {
    return serialization_failure("cannot format TOML");
  }
}

[[nodiscard]] std::string
validation_detail(const std::vector<SchemaMessage> &errors) {
  std::string result = "snapshot validation failed";
  for (const auto &error : errors) {
    result += "; " + error.path + ": " + error.message;
  }
  return result;
}

} // namespace

std::uint64_t ManagedMemoryBudget::total_mib() const noexcept {
  return static_cast<std::uint64_t>(ui_visible_mib) + session_log_mib +
         rx_ingress_mib + tx_mib + send_history_mib + diagnostics_mib +
         owner_and_scratch_mib + model_and_search_mib;
}

ManagedMemoryBudget
managed_memory_budget(const ConfigSnapshot &snapshot) noexcept {
  const auto ui_bytes =
      saturated_add(mib_bytes(snapshot.receive.visible_buffer_mib),
                    saturated_multiply(snapshot.receive.visible_max_records,
                                       kVisibleRecordOverheadBytes));
  const auto log_bytes =
      saturated_add(mib_bytes(snapshot.queues.log_max_mib),
                    saturated_multiply(snapshot.queues.log_max_messages,
                                       kLogQueueNodeOverheadBytes));
  const auto rx_bytes =
      saturated_add(mib_bytes(snapshot.queues.rx_ingress_max_mib),
                    saturated_multiply(snapshot.queues.rx_ingress_max_blocks,
                                       kRxBlockOverheadBytes));
  auto tx_bytes = saturated_add(mib_bytes(snapshot.queues.tx_max_mib),
                                kMaximumPayloadBytes);
  tx_bytes = saturated_add(tx_bytes, snapshot.send.max_draft_bytes);
  tx_bytes = saturated_add(tx_bytes,
                           saturated_multiply(snapshot.queues.tx_max_messages,
                                              kTxQueueNodeOverheadBytes));
  const auto history_bytes =
      saturated_add(mib_bytes(snapshot.send.history_max_mib),
                    saturated_multiply(snapshot.send.history_max_entries,
                                       kHistoryEntryOverheadBytes));
  auto owner_bytes =
      saturated_add(mib_bytes(snapshot.queues.owner_command_max_mib),
                    snapshot.receive.max_frame_bytes);
  owner_bytes = saturated_add(owner_bytes, kOwnerFixedScratchBytes);
  owner_bytes = saturated_add(
      owner_bytes,
      saturated_multiply(snapshot.queues.owner_command_max_messages,
                         kOwnerCommandOverheadBytes));
  const auto model_bytes = saturated_add(
      kQuickSendModelLimitBytes,
      saturated_add(kSearchAndControlBytes, kConfigStateModelBytes));
  return {
      bytes_to_mib(ui_bytes),      bytes_to_mib(log_bytes),
      bytes_to_mib(rx_bytes),      bytes_to_mib(tx_bytes),
      bytes_to_mib(history_bytes), bytes_to_mib(kDiagnosticsBudgetBytes),
      bytes_to_mib(owner_bytes),   bytes_to_mib(model_bytes),
  };
}

std::vector<SchemaMessage>
validate_config_snapshot(const ConfigSnapshot &snapshot) {
  std::vector<SchemaMessage> errors;
  if (snapshot.version != kSchemaVersion) {
    add_error(errors, "version", "unsupported major version");
  }
  if (std::ranges::find(kBaudPresets, snapshot.serial.baud) ==
      kBaudPresets.end()) {
    add_error(errors, "serial.defaults.baud", "is not a supported preset");
  }
  validate_range(errors, "serial.defaults.data_bits", snapshot.serial.data_bits,
                 5, 8);
  validate_range(errors, "serial.defaults.stop_bits", snapshot.serial.stop_bits,
                 1, 2);
  if (to_string(snapshot.serial.parity).empty()) {
    add_error(errors, "serial.defaults.parity", "is invalid");
  }
  if (to_string(snapshot.serial.flow_control).empty()) {
    add_error(errors, "serial.defaults.flow_control", "is invalid");
  }
  if (to_string(snapshot.send.mode).empty()) {
    add_error(errors, "send.mode", "is invalid");
  }
  if (snapshot.send.newline == Newline::Session) {
    add_error(errors, "send.newline", "session is only valid for quick send");
  } else if (to_string(snapshot.send.newline).empty()) {
    add_error(errors, "send.newline", "is invalid");
  }
  if (to_string(snapshot.receive.rx_view).empty()) {
    add_error(errors, "receive.rx_view", "is invalid");
  }
  if (to_string(snapshot.receive.tx_view).empty()) {
    add_error(errors, "receive.tx_view", "is invalid");
  }
  if (to_string(snapshot.ui.background).empty()) {
    add_error(errors, "ui.background", "is invalid");
  }
  validate_range(errors, "send.max_draft_bytes", snapshot.send.max_draft_bytes,
                 1U, static_cast<std::uint32_t>(kMaximumPayloadBytes));
  validate_range(errors, "send.history_max_entries",
                 snapshot.send.history_max_entries, 1U, 10000U);
  validate_range(errors, "send.history_max_mib", snapshot.send.history_max_mib,
                 1U, 16U);
  validate_range(errors, "receive.idle_gap_ms", snapshot.receive.idle_gap_ms,
                 1U, 60000U);
  validate_range(errors, "receive.max_frame_bytes",
                 snapshot.receive.max_frame_bytes, 256U, 65536U);
  validate_range(errors, "receive.visible_buffer_mib",
                 snapshot.receive.visible_buffer_mib, 1U, 48U);
  validate_range(errors, "receive.visible_max_records",
                 snapshot.receive.visible_max_records, 1000U, 1000000U);
  validate_range(errors, "logging.max_files", snapshot.logging.max_files, 1U,
                 10000U);
  validate_range(errors, "logging.max_total_size_mib",
                 snapshot.logging.max_total_size_mib, 1U, 65536U);
  validate_range(errors, "logging.max_file_size_mib",
                 snapshot.logging.max_file_size_mib, 1U, 1024U);
  validate_range(errors, "logging.flush_interval_ms",
                 snapshot.logging.flush_interval_ms, 1U, 60000U);
  if (snapshot.logging.directory.find('\0') != std::string::npos) {
    add_error(errors, "logging.directory", "must not contain NUL");
  } else if (!snapshot.logging.directory.empty() &&
             !std::filesystem::path{snapshot.logging.directory}.is_absolute()) {
    add_error(errors, "logging.directory", "must be empty or absolute");
  }
  if (snapshot.logging.max_file_size_mib >
      snapshot.logging.max_total_size_mib) {
    add_error(errors, "logging.max_file_size_mib",
              "must not exceed logging.max_total_size_mib");
  }

  validate_range(errors, "queues.tx_max_messages",
                 snapshot.queues.tx_max_messages, 1U, kQueueMaximumCount);
  validate_range(errors, "queues.tx_max_mib", snapshot.queues.tx_max_mib, 1U,
                 8U);
  validate_range(errors, "queues.owner_command_max_messages",
                 snapshot.queues.owner_command_max_messages, 1U,
                 kQueueMaximumCount);
  validate_range(errors, "queues.owner_command_max_mib",
                 snapshot.queues.owner_command_max_mib, 1U, 2U);
  validate_range(errors, "queues.rx_ingress_max_blocks",
                 snapshot.queues.rx_ingress_max_blocks, 1U, kQueueMaximumCount);
  validate_range(errors, "queues.rx_ingress_max_mib",
                 snapshot.queues.rx_ingress_max_mib, 1U, 8U);
  validate_range(errors, "queues.log_max_messages",
                 snapshot.queues.log_max_messages, 1U, kQueueMaximumCount);
  validate_range(errors, "queues.log_max_mib", snapshot.queues.log_max_mib, 1U,
                 16U);
  validate_timeout(errors, "timeouts.connect_ms", snapshot.timeouts.connect_ms);
  validate_timeout(errors, "timeouts.tx_ms", snapshot.timeouts.tx_ms);
  validate_timeout(errors, "timeouts.owner_stop_ms",
                   snapshot.timeouts.owner_stop_ms);
  validate_timeout(errors, "timeouts.log_barrier_ms",
                   snapshot.timeouts.log_barrier_ms);

  const auto budget = managed_memory_budget(snapshot);
  const std::array<std::pair<std::string_view, std::uint32_t>, 8> values{{
      {"budget.ui_visible_mib", budget.ui_visible_mib},
      {"budget.session_log_mib", budget.session_log_mib},
      {"budget.rx_ingress_mib", budget.rx_ingress_mib},
      {"budget.tx_mib", budget.tx_mib},
      {"budget.send_history_mib", budget.send_history_mib},
      {"budget.diagnostics_mib", budget.diagnostics_mib},
      {"budget.owner_and_scratch_mib", budget.owner_and_scratch_mib},
      {"budget.model_and_search_mib", budget.model_and_search_mib},
  }};
  constexpr std::array<std::uint32_t, 8> limits{48, 16, 8, 8, 16, 8, 16, 8};
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (values[index].second > limits[index]) {
      add_error(errors, std::string{values[index].first},
                "exceeds category hard limit of " +
                    std::to_string(limits[index]) + " MiB");
    }
  }
  if (budget.total_mib() > 128U) {
    add_error(errors, "budget.total_mib", "must not exceed 128 MiB");
  }
  return errors;
}

std::vector<SchemaMessage>
validate_quick_send_snapshot(const QuickSendSnapshot &snapshot) {
  std::vector<SchemaMessage> errors;
  if (snapshot.version != kSchemaVersion) {
    add_error(errors, "version", "unsupported major version");
  }
  std::array<bool, 20> occupied{};
  for (std::size_t position = 0; position < snapshot.slots.size(); ++position) {
    if (!snapshot.slots[position]) {
      continue;
    }
    const auto &slot = *snapshot.slots[position];
    const auto base = "slots[" + std::to_string(position) + "]";
    if (slot.index < 1 || slot.index > 20) {
      add_error(errors, base + ".index", "must be in range 1..20");
    } else {
      const auto logical_index = static_cast<std::size_t>(slot.index - 1U);
      if (occupied[logical_index]) {
        add_error(errors, base + ".index", "must not be duplicated");
      }
      occupied[logical_index] = true;
      if (logical_index != position) {
        add_error(errors, base + ".index", "must match its snapshot slot");
      }
    }
    validate_quick_slot(slot, base, errors);
  }
  if (quick_send_memory_bytes(snapshot) > kQuickSendModelLimitBytes) {
    add_error(errors, "budget.model_and_search_mib",
              "quick-send snapshot exceeds its model memory reservation");
  }
  return errors;
}

std::vector<SchemaMessage>
validate_state_snapshot(const StateSnapshot &snapshot) {
  std::vector<SchemaMessage> errors;
  if (snapshot.version != kSchemaVersion) {
    add_error(errors, "version", "unsupported major version");
  }
  validate_range(errors, "last_quick_send_slot", snapshot.last_quick_send_slot,
                 1U, 20U);
  if (snapshot.last_interval_ms != 0 &&
      (snapshot.last_interval_ms < 10U ||
       snapshot.last_interval_ms > 86400000U)) {
    add_error(errors, "last_interval_ms", "must be 0 or in range 10..86400000");
  }
  if (!snapshot.show_rx && !snapshot.show_tx && !snapshot.show_system &&
      !snapshot.show_error) {
    add_error(errors, "show_*", "at least one record type must remain visible");
  }
  return errors;
}

ConfigLoadResult parse_config_toml(std::string_view document) {
  return parse_document<ConfigLoadResult>(document, kConfigMaximumBytes,
                                          parse_config_table);
}

QuickSendLoadResult parse_quick_send_toml(std::string_view document) {
  return parse_document<QuickSendLoadResult>(document, kQuickSendMaximumBytes,
                                             parse_quick_send_table);
}

StateLoadResult parse_state_toml(std::string_view document) {
  return parse_document<StateLoadResult>(document, kStateMaximumBytes,
                                         parse_state_table);
}

ConfigLoadResult load_config_toml(const std::filesystem::path &path) {
  return load_document<ConfigLoadResult>(path, kConfigMaximumBytes,
                                         parse_config_toml);
}

QuickSendLoadResult load_quick_send_toml(const std::filesystem::path &path) {
  return load_document<QuickSendLoadResult>(path, kQuickSendMaximumBytes,
                                            parse_quick_send_toml);
}

StateLoadResult load_state_toml(const std::filesystem::path &path) {
  return load_document<StateLoadResult>(path, kStateMaximumBytes,
                                        parse_state_toml);
}

Result<std::string> serialize_config_toml(const ConfigSnapshot &snapshot,
                                          std::string_view preserved_document) {
  const auto errors = validate_config_snapshot(snapshot);
  if (!errors.empty()) {
    return serialization_failure(validation_detail(errors),
                                 ErrorCode::ValidationInvalidValue);
  }
  auto parsed = preserved_table(preserved_document, kConfigMaximumBytes);
  if (!parsed) {
    return tl::make_unexpected(parsed.error());
  }
  try {
    auto table = std::move(*parsed);
    table.insert_or_assign("version", snapshot.version);
    auto &serial = ensure_table(table, "serial");
    auto &serial_defaults = ensure_table(serial, "defaults");
    serial_defaults.insert_or_assign("baud", snapshot.serial.baud);
    serial_defaults.insert_or_assign("data_bits", snapshot.serial.data_bits);
    serial_defaults.insert_or_assign("stop_bits", snapshot.serial.stop_bits);
    serial_defaults.insert_or_assign(
        "parity", std::string{to_string(snapshot.serial.parity)});
    serial_defaults.insert_or_assign(
        "flow_control", std::string{to_string(snapshot.serial.flow_control)});

    auto &send = ensure_table(table, "send");
    send.insert_or_assign("mode", std::string{to_string(snapshot.send.mode)});
    send.insert_or_assign("newline",
                          std::string{to_string(snapshot.send.newline)});
    static_cast<void>(send.erase("keep_after_send"));
    send.insert_or_assign("max_draft_bytes", snapshot.send.max_draft_bytes);
    send.insert_or_assign("history_max_entries",
                          snapshot.send.history_max_entries);
    send.insert_or_assign("history_max_mib", snapshot.send.history_max_mib);

    auto &receive = ensure_table(table, "receive");
    static_cast<void>(receive.erase("view"));
    receive.insert_or_assign("rx_view",
                             std::string{to_string(snapshot.receive.rx_view)});
    receive.insert_or_assign("tx_view",
                             std::string{to_string(snapshot.receive.tx_view)});
    receive.insert_or_assign("idle_gap_ms", snapshot.receive.idle_gap_ms);
    receive.insert_or_assign("max_frame_bytes",
                             snapshot.receive.max_frame_bytes);
    receive.insert_or_assign("visible_buffer_mib",
                             snapshot.receive.visible_buffer_mib);
    receive.insert_or_assign("visible_max_records",
                             snapshot.receive.visible_max_records);

    auto &ui = ensure_table(table, "ui");
    ui.insert_or_assign("background",
                        std::string{to_string(snapshot.ui.background)});

    auto &logging = ensure_table(table, "logging");
    logging.insert_or_assign("default_enabled",
                             snapshot.logging.default_enabled);
    logging.insert_or_assign("directory", snapshot.logging.directory);
    logging.insert_or_assign("max_files", snapshot.logging.max_files);
    logging.insert_or_assign("max_total_size_mib",
                             snapshot.logging.max_total_size_mib);
    logging.insert_or_assign("max_file_size_mib",
                             snapshot.logging.max_file_size_mib);
    logging.insert_or_assign("flush_interval_ms",
                             snapshot.logging.flush_interval_ms);
    logging.insert_or_assign("include_system", snapshot.logging.include_system);
    logging.insert_or_assign("include_error", snapshot.logging.include_error);

    auto &queues = ensure_table(table, "queues");
    queues.insert_or_assign("tx_max_messages", snapshot.queues.tx_max_messages);
    queues.insert_or_assign("tx_max_mib", snapshot.queues.tx_max_mib);
    queues.insert_or_assign("owner_command_max_messages",
                            snapshot.queues.owner_command_max_messages);
    queues.insert_or_assign("owner_command_max_mib",
                            snapshot.queues.owner_command_max_mib);
    queues.insert_or_assign("rx_ingress_max_blocks",
                            snapshot.queues.rx_ingress_max_blocks);
    queues.insert_or_assign("rx_ingress_max_mib",
                            snapshot.queues.rx_ingress_max_mib);
    queues.insert_or_assign("log_max_messages",
                            snapshot.queues.log_max_messages);
    queues.insert_or_assign("log_max_mib", snapshot.queues.log_max_mib);

    auto &timeouts = ensure_table(table, "timeouts");
    timeouts.insert_or_assign("connect_ms", snapshot.timeouts.connect_ms);
    timeouts.insert_or_assign("tx_ms", snapshot.timeouts.tx_ms);
    timeouts.insert_or_assign("owner_stop_ms", snapshot.timeouts.owner_stop_ms);
    timeouts.insert_or_assign("log_barrier_ms",
                              snapshot.timeouts.log_barrier_ms);
    return format_table(std::move(table), kConfigMaximumBytes);
  } catch (const std::exception &error) {
    return serialization_failure(error.what());
  } catch (...) {
    return serialization_failure("cannot construct config TOML");
  }
}

Result<std::string>
serialize_quick_send_toml(const QuickSendSnapshot &snapshot,
                          std::string_view preserved_document) {
  const auto errors = validate_quick_send_snapshot(snapshot);
  if (!errors.empty()) {
    return serialization_failure(validation_detail(errors),
                                 ErrorCode::ValidationInvalidValue);
  }
  auto parsed = preserved_table(preserved_document, kQuickSendMaximumBytes);
  if (!parsed) {
    return tl::make_unexpected(parsed.error());
  }
  try {
    auto table = std::move(*parsed);
    table.insert_or_assign("version", snapshot.version);
    toml::array *slots = table["slots"].as_array();
    if (slots == nullptr) {
      table.insert_or_assign("slots", toml::array{});
      slots = table["slots"].as_array();
    }

    std::array<toml::table *, 20> existing{};
    for (auto &node : *slots) {
      auto *slot_table = node.as_table();
      if (slot_table == nullptr) {
        continue;
      }
      const auto index = (*slot_table)["index"].value<std::int64_t>();
      if (index && *index >= 1 && *index <= 20 &&
          existing[static_cast<std::size_t>(*index - 1)] == nullptr) {
        existing[static_cast<std::size_t>(*index - 1)] = slot_table;
      }
    }

    for (std::size_t position = 0; position < snapshot.slots.size();
         ++position) {
      const auto &slot = snapshot.slots[position];
      auto *slot_table = existing[position];
      if (!slot) {
        if (slot_table != nullptr) {
          slot_table->erase("index");
          slot_table->erase("name");
          slot_table->erase("mode");
          slot_table->erase("content");
          slot_table->erase("newline");
          slot_table->erase("note");
        }
        continue;
      }
      if (slot_table == nullptr) {
        for (auto &node : *slots) {
          auto *empty = node.as_table();
          if (empty != nullptr && !empty->contains("index")) {
            slot_table = empty;
            break;
          }
        }
        if (slot_table == nullptr) {
          slots->push_back(toml::table{});
          slot_table = slots->back().as_table();
        }
      }
      slot_table->insert_or_assign("index", slot->index);
      slot_table->insert_or_assign("name", slot->name);
      slot_table->insert_or_assign("mode", std::string{to_string(slot->mode)});
      slot_table->insert_or_assign("content", slot->content);
      slot_table->insert_or_assign("newline",
                                   std::string{to_string(slot->newline)});
      slot_table->insert_or_assign("note", slot->note);
    }
    auto serialized = format_table(std::move(table), kQuickSendMaximumBytes);
    if (!serialized) {
      return serialized;
    }
    const auto round_trip = parse_quick_send_toml(*serialized);
    if (!round_trip.accepted) {
      return serialization_failure(validation_detail(round_trip.errors),
                                   ErrorCode::ValidationInvalidValue);
    }
    return serialized;
  } catch (const std::exception &error) {
    return serialization_failure(error.what());
  } catch (...) {
    return serialization_failure("cannot construct quick-send TOML");
  }
}

Result<std::string> serialize_state_toml(const StateSnapshot &snapshot,
                                         std::string_view preserved_document) {
  const auto errors = validate_state_snapshot(snapshot);
  if (!errors.empty()) {
    return serialization_failure(validation_detail(errors),
                                 ErrorCode::ValidationInvalidValue);
  }
  auto parsed = preserved_table(preserved_document, kStateMaximumBytes);
  if (!parsed) {
    return tl::make_unexpected(parsed.error());
  }
  try {
    auto table = std::move(*parsed);
    table.insert_or_assign("version", snapshot.version);
    table.insert_or_assign("last_quick_send_slot",
                           snapshot.last_quick_send_slot);
    table.insert_or_assign("last_interval_ms", snapshot.last_interval_ms);
    table.insert_or_assign("show_rx", snapshot.show_rx);
    table.insert_or_assign("show_tx", snapshot.show_tx);
    table.insert_or_assign("show_system", snapshot.show_system);
    table.insert_or_assign("show_error", snapshot.show_error);
    return format_table(std::move(table), kStateMaximumBytes);
  } catch (const std::exception &error) {
    return serialization_failure(error.what());
  } catch (...) {
    return serialization_failure("cannot construct state TOML");
  }
}

std::string_view to_string(Parity value) noexcept {
  switch (value) {
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
  return "";
}

std::string_view to_string(FlowControl value) noexcept {
  switch (value) {
  case FlowControl::None:
    return "none";
  case FlowControl::RtsCts:
    return "rts/cts";
  case FlowControl::XonXoff:
    return "xon/xoff";
  }
  return "";
}

std::string_view to_string(SendMode value) noexcept {
  switch (value) {
  case SendMode::Txt:
    return "txt";
  case SendMode::Hex:
    return "hex";
  }
  return "";
}

std::string_view to_string(Newline value) noexcept {
  switch (value) {
  case Newline::None:
    return "none";
  case Newline::Lf:
    return "lf";
  case Newline::Cr:
    return "cr";
  case Newline::CrLf:
    return "crlf";
  case Newline::Session:
    return "session";
  }
  return "";
}

std::string_view to_string(ReceiveView value) noexcept {
  switch (value) {
  case ReceiveView::Txt:
    return "txt";
  case ReceiveView::Hex:
    return "hex";
  case ReceiveView::Mixed:
    return "mixed";
  }
  return "";
}

std::optional<UiBackground> parse_ui_background(std::string_view value) {
  const auto normalized = ascii_lower(value);
  if (normalized == "rose-pine")
    return UiBackground::RosePine;
  if (normalized == "transparent")
    return UiBackground::Transparent;
  return std::nullopt;
}

std::string_view to_string(UiBackground value) noexcept {
  switch (value) {
  case UiBackground::RosePine:
    return "rose-pine";
  case UiBackground::Transparent:
    return "transparent";
  }
  return "";
}

} // namespace lazycom::config
