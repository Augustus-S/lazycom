#include <lazycom/config/schema.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

namespace {

[[nodiscard]] bool
has_path(const std::vector<lazycom::config::SchemaMessage> &messages,
         std::string_view path) {
  return std::any_of(
      messages.begin(), messages.end(),
      [path](const auto &message) { return message.path == path; });
}

void check_paths(const std::vector<lazycom::config::SchemaMessage> &messages,
                 std::initializer_list<std::string_view> paths) {
  for (const auto path : paths) {
    CAPTURE(path);
    CHECK(has_path(messages, path));
  }
}

} // namespace

namespace config = lazycom::config;
TEST_CASE("config defaults and enums obey the published schema", "[config]") {
  const auto defaults = config::parse_config_toml("version = 1\n");
  REQUIRE(defaults.accepted);
  REQUIRE_FALSE(defaults.read_only);
  REQUIRE(defaults.snapshot == config::ConfigSnapshot{});
  const auto canonical = config::serialize_config_toml(defaults.snapshot);
  REQUIRE(canonical);
  const auto round_trip = config::parse_config_toml(*canonical);
  REQUIRE(round_trip.accepted);
  CHECK(round_trip.snapshot == defaults.snapshot);
  const auto parsed = config::parse_config_toml(R"toml(version = 1
[serial.defaults]
parity = "EvEn"
flow_control = "RTS/CTS"
[send]
mode = "HEX"
newline = "CRLF"
[receive]
rx_view = "MiXeD"
tx_view = "HeX"
[ui]
background = "TrAnSpArEnT"
)toml");
  REQUIRE(parsed.accepted);
  CHECK(parsed.snapshot.ui.background == config::UiBackground::Transparent);
  const auto serialized = config::serialize_config_toml(parsed.snapshot);
  REQUIRE(serialized);
  for (const auto field :
       {"parity = 'even'", "flow_control = 'rts/cts'", "rx_view = 'mixed'",
        "tx_view = 'hex'", "background = 'transparent'", "mode = 'hex'",
        "newline = 'crlf'"}) {
    CAPTURE(field);
    CHECK(serialized->find(field) != std::string::npos);
  }
}

TEST_CASE("config rejects whole invalid candidates and reports every field",
          "[config]") {
  const std::array cases{
      std::pair{"version = [\n", "$syntax"},
      std::pair{"version = 2\n", "version"},
      std::pair{"version = 1\n[ui]\nbackground = 'solid'\n", "ui.background"},
      std::pair{"version = 1\n[serial.defaults]\nbaud = 12345\n",
                "serial.defaults.baud"},
      std::pair{"version = 1\n[logging]\ndirectory = \"/tmp\\u0000redirect\"\n",
                "logging.directory"}};
  for (const auto &[source, path] : cases) {
    CAPTURE(path);
    const auto result = config::parse_config_toml(source);
    CHECK_FALSE(result.accepted);
    CHECK(result.read_only);
    CHECK(has_path(result.errors, path));
  }
  const auto result = config::parse_config_toml(R"toml(version = 1
[serial.defaults]
baud = 0
data_bits = "eight"
parity = "broken"
[receive]
idle_gap_ms = 0
visible_buffer_mib = 49
[logging]
max_file_size_mib = 100
max_total_size_mib = 10
[queues]
tx_max_mib = 9
[timeouts]
connect_ms = 99
)toml");
  REQUIRE_FALSE(result.accepted);
  CHECK(result.snapshot == config::ConfigSnapshot{});
  check_paths(result.errors,
              {"serial.defaults.baud", "serial.defaults.data_bits",
               "serial.defaults.parity", "receive.idle_gap_ms",
               "receive.visible_buffer_mib", "logging.max_file_size_mib",
               "queues.tx_max_mib", "timeouts.connect_ms"});
  auto snapshot = config::ConfigSnapshot{};
  snapshot.logging.directory = std::string{"/tmp\0redirect", 13};
  CHECK(has_path(config::validate_config_snapshot(snapshot),
                 "logging.directory"));
}

TEST_CASE("config unknown keys and values survive a changed snapshot",
          "[config]") {
  const auto parsed = config::parse_config_toml(R"toml(version = 1
future_root = 42
[ui]
future_ui = "keep"
[send]
future_send = { enabled = true }
)toml");
  REQUIRE(parsed.accepted);
  auto changed = parsed.snapshot;
  changed.receive.rx_view = config::ReceiveView::Hex;
  const auto serialized =
      config::serialize_config_toml(changed, parsed.document);
  REQUIRE(serialized);
  const auto round_trip = config::parse_config_toml(*serialized);
  REQUIRE(round_trip.accepted);
  CHECK(round_trip.snapshot == changed);
  for (const auto path : {"future_root", "ui.future_ui", "send.future_send"}) {
    CAPTURE(path);
    CHECK(has_path(parsed.warnings, path));
    CHECK(has_path(round_trip.warnings, path));
  }
  for (const auto value :
       {"future_root = 42", "future_ui = 'keep'", "enabled = true"}) {
    CAPTURE(value);
    CHECK(serialized->find(value) != std::string::npos);
  }
}

TEST_CASE(
    "legacy settings migrate while explicit receive views take precedence",
    "[config]") {
  for (const auto &[keep, explicit_views] :
       {std::pair{true, false}, std::pair{false, false},
        std::pair{true, true}}) {
    CAPTURE(keep, explicit_views);
    const auto source =
        std::string{"version = 1\n[send]\nkeep_after_send = "} +
        (keep ? "true" : "false") +
        "\nfuture_send = 'keep'\n[receive]\nview = 'text'\n" +
        (explicit_views ? "rx_view = 'hex'\ntx_view = 'mixed'\n" : "");
    const auto parsed = config::parse_config_toml(source);
    REQUIRE(parsed.accepted);
    CHECK(
        parsed.snapshot.receive.rx_view ==
        (explicit_views ? config::ReceiveView::Hex : config::ReceiveView::Txt));
    CHECK(parsed.snapshot.receive.tx_view == (explicit_views
                                                  ? config::ReceiveView::Mixed
                                                  : config::ReceiveView::Txt));
    CHECK(has_path(parsed.warnings, "send.keep_after_send"));
    CHECK(has_path(parsed.warnings, "receive.view"));
    const auto serialized =
        config::serialize_config_toml(parsed.snapshot, parsed.document);
    REQUIRE(serialized);
    CHECK(serialized->find("keep_after_send") == std::string::npos);
    CHECK(serialized->find("view = 'text'") == std::string::npos);
    CHECK(serialized->find("future_send = 'keep'") != std::string::npos);
    const auto round_trip = config::parse_config_toml(*serialized);
    REQUIRE(round_trip.accepted);
    CHECK(round_trip.snapshot == parsed.snapshot);
  }
}

TEST_CASE("managed budgets count payload metadata and current TX plus draft",
          "[config]") {
  auto snapshot = config::ConfigSnapshot{};
  snapshot.receive.visible_buffer_mib = 49;
  snapshot.queues.log_max_mib = 17;
  snapshot.queues.rx_ingress_max_mib = 9;
  snapshot.queues.tx_max_mib = 9;
  snapshot.send.history_max_mib = 17;
  snapshot.queues.owner_command_max_mib = 17;
  const auto errors = config::validate_config_snapshot(snapshot);
  check_paths(errors, {"budget.ui_visible_mib", "budget.session_log_mib",
                       "budget.rx_ingress_mib", "budget.tx_mib",
                       "budget.send_history_mib",
                       "budget.owner_and_scratch_mib", "budget.total_mib"});
  snapshot = {};
  snapshot.queues.tx_max_mib = 8;
  CHECK(config::managed_memory_budget(snapshot).tx_mib > 8);
  CHECK(has_path(config::validate_config_snapshot(snapshot), "budget.tx_mib"));
  snapshot = {};
  snapshot.receive.visible_buffer_mib = 1;
  snapshot.receive.visible_max_records = 1000000;
  snapshot.queues.tx_max_mib = 1;
  snapshot.queues.tx_max_messages = 65536;
  snapshot.queues.owner_command_max_mib = 1;
  snapshot.queues.owner_command_max_messages = 65536;
  const auto metadata_errors = config::validate_config_snapshot(snapshot);
  check_paths(metadata_errors, {"budget.ui_visible_mib", "budget.tx_mib",
                                "budget.owner_and_scratch_mib"});
}

TEST_CASE("quick-send schema validates sparse empty and malformed slots",
          "[config]") {
  const auto sparse = config::parse_quick_send_toml(R"toml(version = 1
[[slots]]
index = 1
content = "AT+INFO"
[[slots]]
index = 20
mode = "hex"
content = "0x00 ff 7A"
newline = "crlf"
[[slots]]
content = "missing index"
)toml");
  REQUIRE(sparse.accepted);
  REQUIRE(sparse.snapshot.slots[0]);
  REQUIRE(sparse.snapshot.slots[19]);
  CHECK(sparse.snapshot.slots[19]->mode == config::SendMode::Hex);
  CHECK_FALSE(sparse.snapshot.slots[1]);
  const auto invalid = config::parse_quick_send_toml(R"toml(version = 1
[[slots]]
index = 2
mode = "hex"
content = "0x0g"
newline = "bad"
[[slots]]
index = 2
)toml");
  REQUIRE_FALSE(invalid.accepted);
  check_paths(invalid.errors,
              {"slots[0].content", "slots[0].newline", "slots[1].index"});
  std::string oversized = "version = 1\n";
  for (unsigned index = 1U; index <= 21U; ++index)
    oversized += "[[slots]]\nindex = " + std::to_string(index) + "\n";
  CHECK(has_path(config::parse_quick_send_toml(oversized).errors, "slots"));
}

TEST_CASE(
    "quick-send snapshot validation bounds text and aggregate model memory",
    "[config]") {
  config::QuickSendSnapshot snapshot;
  config::QuickSendSlot slot;
  slot.index = 1;
  slot.name.assign(65, 'n');
  slot.content = std::string{"\xC0\x80", 2};
  slot.note.assign(257, 'x');
  snapshot.slots[0] = slot;
  const auto errors = config::validate_quick_send_snapshot(snapshot);
  check_paths(errors, {"slots[0].name", "slots[0].content", "slots[0].note"});
  snapshot = {};
  for (std::uint32_t index = 0; index < 4; ++index) {
    slot = {};
    slot.index = index + 1U;
    slot.content.assign(config::kMaximumPayloadBytes, 'x');
    snapshot.slots[index] = slot;
  }
  CHECK(has_path(config::validate_quick_send_snapshot(snapshot),
                 "budget.model_and_search_mib"));
}

TEST_CASE("TOML parsers reject oversized documents before parsing",
          "[config]") {
  CHECK(has_path(config::parse_config_toml(
                     std::string(config::kConfigMaximumBytes + 1U, 'x'))
                     .errors,
                 "$document"));
  CHECK(has_path(config::parse_quick_send_toml(
                     std::string(config::kQuickSendMaximumBytes + 1U, 'x'))
                     .errors,
                 "$document"));
  CHECK(has_path(config::parse_state_toml(
                     std::string(config::kStateMaximumBytes + 1U, 'x'))
                     .errors,
                 "$document"));
}

TEST_CASE("quick-send full slots preserve unknown keys through repeated clear "
          "and refill",
          "[config]") {
  config::QuickSendSnapshot snapshot;
  for (std::uint32_t index = 0; index < snapshot.slots.size(); ++index) {
    config::QuickSendSlot slot;
    slot.index = index + 1U;
    slot.content = "payload";
    snapshot.slots[index] = slot;
  }
  const auto preserved = config::parse_quick_send_toml(
      "version = 1\nfuture_root = 'keep'\n[[slots]]\nindex = 1\nfuture_slot = "
      "7\n");
  REQUIRE(preserved.accepted);
  CHECK(has_path(preserved.warnings, "future_root"));
  CHECK(has_path(preserved.warnings, "slots[0].future_slot"));
  auto document =
      config::serialize_quick_send_toml(snapshot, preserved.document);
  REQUIRE(document);
  for (unsigned pass = 0; pass < 3; ++pass) {
    for (const bool filled : {false, true}) {
      CAPTURE(pass, filled);
      snapshot.slots[0] =
          filled ? std::optional{config::QuickSendSlot{}} : std::nullopt;
      if (filled) {
        snapshot.slots[0]->index = 1U;
        snapshot.slots[0]->content = "new payload";
      }
      document = config::serialize_quick_send_toml(snapshot, *document);
      REQUIRE(document);
      const auto loaded = config::parse_quick_send_toml(*document);
      REQUIRE(loaded.accepted);
      CHECK(loaded.snapshot == snapshot);
      CHECK(document->find("future_root = 'keep'") != std::string::npos);
      CHECK(document->find("future_slot = 7") != std::string::npos);
    }
  }
}

TEST_CASE("TOML serialization obeys next-load byte and combined memory limits",
          "[config]") {
  CHECK_FALSE(config::serialize_state_toml(
      {}, std::string(config::kStateMaximumBytes + 1U, 'x')));
  config::QuickSendSnapshot snapshot;
  config::QuickSendSlot slot;
  slot.index = 1;
  slot.content.assign(config::kMaximumPayloadBytes, '\0');
  snapshot.slots[0] = slot;
  CHECK_FALSE(config::serialize_quick_send_toml(snapshot));
  for (std::uint32_t index = 0; index < 3; ++index) {
    slot.index = index + 1U;
    slot.content.assign(index == 2U ? 512U * 1024U : 1024U * 1024U, 'x');
    snapshot.slots[index] = slot;
  }
  REQUIRE(config::validate_quick_send_snapshot(snapshot).empty());
  const auto preserved =
      "version = 1\nfuture_blob = '" + std::string(1024U * 1024U, 'x') + "'\n";
  REQUIRE(config::parse_quick_send_toml(preserved).accepted);
  CHECK_FALSE(config::serialize_quick_send_toml(snapshot, preserved));
}

TEST_CASE(
    "state validates intervals and visibility while preserving unknown keys",
    "[config]") {
  for (const auto interval : {0U, 10U, 86400000U}) {
    CAPTURE(interval);
    CHECK(config::parse_state_toml(
              "version = 1\nlast_quick_send_slot = 20\nlast_interval_ms = " +
              std::to_string(interval))
              .accepted);
  }
  const auto invalid = config::parse_state_toml(
      "version = 1\nlast_quick_send_slot = 21\nlast_interval_ms = 9\n");
  CHECK_FALSE(invalid.accepted);
  CHECK(invalid.read_only);
  CHECK(has_path(invalid.errors, "last_quick_send_slot"));
  CHECK(has_path(invalid.errors, "last_interval_ms"));
  CHECK_FALSE(
      config::parse_state_toml("version = 1\nlast_interval_ms = 86400001\n")
          .accepted);
  const auto parsed = config::parse_state_toml(
      "version = 1\nshow_rx = false\nshow_tx = true\nshow_system = "
      "false\nshow_error = true\nfuture_ui = true\n");
  REQUIRE(parsed.accepted);
  CHECK_FALSE(parsed.snapshot.show_rx);
  CHECK(parsed.snapshot.show_tx);
  CHECK_FALSE(parsed.snapshot.show_system);
  CHECK(parsed.snapshot.show_error);
  CHECK(has_path(parsed.warnings, "future_ui"));
  const auto serialized =
      config::serialize_state_toml(parsed.snapshot, parsed.document);
  REQUIRE(serialized);
  CHECK(serialized->find("future_ui = true") != std::string::npos);
  const auto round_trip = config::parse_state_toml(*serialized);
  REQUIRE(round_trip.accepted);
  CHECK(round_trip.snapshot == parsed.snapshot);
  const auto hidden = config::parse_state_toml(
      "version = 1\nshow_rx = false\nshow_tx = false\nshow_system = "
      "false\nshow_error = false\n");
  CHECK_FALSE(hidden.accepted);
  CHECK(hidden.read_only);
  CHECK(has_path(hidden.errors, "show_*"));
}
