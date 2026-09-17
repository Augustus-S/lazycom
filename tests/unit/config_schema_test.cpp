#include <lazycom/config/schema.hpp>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

namespace {

constexpr std::string_view kPlanConfig = R"toml(
version = 1

[ui]
background = "rose-pine"

[serial.defaults]
baud = 115200
data_bits = 8
stop_bits = 1
parity = "none"
flow_control = "none"

[send]
mode = "txt"
newline = "none"
max_draft_bytes = 1048576
history_max_entries = 1000
history_max_mib = 8

[receive]
rx_view = "txt"
tx_view = "txt"
idle_gap_ms = 50
max_frame_bytes = 65536
visible_buffer_mib = 32
visible_max_records = 100000

[logging]
default_enabled = false
directory = ""
max_files = 100
max_total_size_mib = 1024
max_file_size_mib = 64
flush_interval_ms = 1000
include_system = true
include_error = true

[queues]
tx_max_messages = 256
tx_max_mib = 4
owner_command_max_messages = 256
owner_command_max_mib = 1
rx_ingress_max_blocks = 4096
rx_ingress_max_mib = 4
log_max_messages = 4096
log_max_mib = 8

[timeouts]
connect_ms = 5000
tx_ms = 5000
owner_stop_ms = 5000
log_barrier_ms = 5000
)toml";

[[nodiscard]] bool
has_path(const std::vector<lazycom::config::SchemaMessage> &messages,
         std::string_view path) {
  return std::any_of(
      messages.begin(), messages.end(),
      [path](const auto &message) { return message.path == path; });
}

} // namespace

TEST_CASE("Plan config example produces the complete default snapshot",
          "[config]") {
  const auto result = lazycom::config::parse_config_toml(kPlanConfig);

  REQUIRE(result.accepted);
  REQUIRE_FALSE(result.read_only);
  REQUIRE(result.errors.empty());
  REQUIRE(result.snapshot == lazycom::config::ConfigSnapshot{});
  REQUIRE(result.snapshot.ui.background ==
          lazycom::config::UiBackground::RosePine);
  REQUIRE(lazycom::config::managed_memory_budget(result.snapshot).total_mib() <=
          128U);

  const auto serialized =
      lazycom::config::serialize_config_toml(result.snapshot, result.document);
  REQUIRE(serialized);
  const auto round_trip = lazycom::config::parse_config_toml(*serialized);
  REQUIRE(round_trip.accepted);
  REQUIRE(round_trip.snapshot == result.snapshot);
}

TEST_CASE("configuration scopes lock only the hardware connection snapshot",
          "[config]") {
  using lazycom::config::ConfigurationField;
  using lazycom::config::ConfigurationScope;

  STATIC_REQUIRE(
      lazycom::config::configuration_scope(ConfigurationField::Baud) ==
      ConfigurationScope::HardwareConnectionSnapshot);
  STATIC_REQUIRE_FALSE(lazycom::config::mutable_while_connected(
      ConfigurationField::FlowControl));
  STATIC_REQUIRE(
      lazycom::config::mutable_while_connected(ConfigurationField::SendMode));
  STATIC_REQUIRE(lazycom::config::mutable_while_connected(
      ConfigurationField::ReceiveView));
  STATIC_REQUIRE(lazycom::config::mutable_while_connected(
      ConfigurationField::QuickSendSlots));
}

TEST_CASE("serial baud accepts only the published presets", "[config]") {
  const auto invalid = lazycom::config::parse_config_toml(R"toml(
version = 1
[serial.defaults]
baud = 12345
)toml");
  REQUIRE_FALSE(invalid.accepted);
  REQUIRE(has_path(invalid.errors, "serial.defaults.baud"));

  for (const auto baud : lazycom::config::kBaudPresets) {
    auto candidate = lazycom::config::ConfigSnapshot{};
    candidate.serial.baud = baud;
    CHECK(lazycom::config::validate_config_snapshot(candidate).empty());
  }
}

TEST_CASE("config schema collects errors and does not partially commit",
          "[config]") {
  const auto result = lazycom::config::parse_config_toml(R"toml(
version = 1
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
  REQUIRE(result.snapshot == lazycom::config::ConfigSnapshot{});
  REQUIRE(has_path(result.errors, "serial.defaults.baud"));
  REQUIRE(has_path(result.errors, "serial.defaults.data_bits"));
  REQUIRE(has_path(result.errors, "serial.defaults.parity"));
  REQUIRE(has_path(result.errors, "receive.idle_gap_ms"));
  REQUIRE(has_path(result.errors, "receive.visible_buffer_mib"));
  REQUIRE(has_path(result.errors, "logging.max_file_size_mib"));
  REQUIRE(has_path(result.errors, "queues.tx_max_mib"));
  REQUIRE(has_path(result.errors, "timeouts.connect_ms"));
}

TEST_CASE("config enums are case insensitive and serialize lowercase",
          "[config]") {
  const auto parsed = lazycom::config::parse_config_toml(R"toml(
version = 1
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
  REQUIRE(parsed.snapshot.ui.background ==
          lazycom::config::UiBackground::Transparent);

  const auto serialized =
      lazycom::config::serialize_config_toml(parsed.snapshot);
  REQUIRE(serialized);
  REQUIRE(serialized->find("parity = 'even'") != std::string::npos);
  REQUIRE(serialized->find("flow_control = 'rts/cts'") != std::string::npos);
  REQUIRE(serialized->find("rx_view = 'mixed'") != std::string::npos);
  REQUIRE(serialized->find("tx_view = 'hex'") != std::string::npos);
  REQUIRE(serialized->find("background = 'transparent'") != std::string::npos);
}

TEST_CASE("UI background defaults when its table is absent", "[config]") {
  const auto parsed = lazycom::config::parse_config_toml("version = 1\n");

  REQUIRE(parsed.accepted);
  REQUIRE(parsed.snapshot.ui.background ==
          lazycom::config::UiBackground::RosePine);
}

TEST_CASE("invalid UI background is rejected", "[config]") {
  const auto parsed = lazycom::config::parse_config_toml(
      "version = 1\n[ui]\nbackground = \"solid\"\n");

  REQUIRE_FALSE(parsed.accepted);
  REQUIRE(has_path(parsed.errors, "ui.background"));
}

TEST_CASE("unknown config keys warn and survive a rewrite", "[config]") {
  const std::string source = R"toml(
version = 1
future_root = 42
[ui]
background = "ROSE-PINE"
future_ui = "keep"
[send]
mode = "txt"
future_send = { enabled = true }
)toml";
  const auto parsed = lazycom::config::parse_config_toml(source);
  REQUIRE(parsed.accepted);
  REQUIRE(has_path(parsed.warnings, "future_root"));
  REQUIRE(has_path(parsed.warnings, "ui.future_ui"));
  REQUIRE(has_path(parsed.warnings, "send.future_send"));

  auto changed = parsed.snapshot;
  changed.receive.rx_view = lazycom::config::ReceiveView::Hex;
  const auto serialized =
      lazycom::config::serialize_config_toml(changed, parsed.document);
  REQUIRE(serialized);
  REQUIRE(serialized->find("future_root = 42") != std::string::npos);
  REQUIRE(serialized->find("background = 'rose-pine'") != std::string::npos);
  REQUIRE(serialized->find("future_ui = 'keep'") != std::string::npos);
  REQUIRE(serialized->find("future_send") != std::string::npos);

  const auto round_trip = lazycom::config::parse_config_toml(*serialized);
  REQUIRE(round_trip.accepted);
  REQUIRE(round_trip.snapshot.receive.rx_view ==
          lazycom::config::ReceiveView::Hex);
  REQUIRE(round_trip.snapshot.ui.background ==
          lazycom::config::UiBackground::RosePine);
  REQUIRE(has_path(round_trip.warnings, "future_root"));
  REQUIRE(has_path(round_trip.warnings, "ui.future_ui"));
}

TEST_CASE("legacy send and receive settings migrate on rewrite", "[config]") {
  const std::string source = R"toml(
version = 1
[send]
keep_after_send = true
future_send = "keep"
[receive]
view = "text"
)toml";
  const auto parsed = lazycom::config::parse_config_toml(source);
  REQUIRE(parsed.accepted);
  REQUIRE(parsed.snapshot.receive.rx_view == lazycom::config::ReceiveView::Txt);
  REQUIRE(parsed.snapshot.receive.tx_view == lazycom::config::ReceiveView::Txt);
  REQUIRE(has_path(parsed.warnings, "send.keep_after_send"));
  REQUIRE(has_path(parsed.warnings, "receive.view"));

  const auto serialized =
      lazycom::config::serialize_config_toml(parsed.snapshot, parsed.document);
  REQUIRE(serialized);
  CHECK(serialized->find("keep_after_send") == std::string::npos);
  CHECK(serialized->find("view = 'text'") == std::string::npos);
  CHECK(serialized->find("rx_view = 'txt'") != std::string::npos);
  CHECK(serialized->find("tx_view = 'txt'") != std::string::npos);
  CHECK(serialized->find("future_send = 'keep'") != std::string::npos);
}

TEST_CASE("new receive views override the legacy view", "[config]") {
  const auto parsed = lazycom::config::parse_config_toml(R"toml(
version = 1
[receive]
view = "text"
rx_view = "hex"
tx_view = "mixed"
)toml");
  REQUIRE(parsed.accepted);
  CHECK(parsed.snapshot.receive.rx_view == lazycom::config::ReceiveView::Hex);
  CHECK(parsed.snapshot.receive.tx_view == lazycom::config::ReceiveView::Mixed);
  CHECK(has_path(parsed.warnings, "receive.view"));
}

TEST_CASE("legacy false keep-after-send is also ignored", "[config]") {
  const auto parsed = lazycom::config::parse_config_toml(
      "version = 1\n[send]\nkeep_after_send = false\n");
  REQUIRE(parsed.accepted);
  CHECK(has_path(parsed.warnings, "send.keep_after_send"));
  const auto serialized =
      lazycom::config::serialize_config_toml(parsed.snapshot, parsed.document);
  REQUIRE(serialized);
  CHECK(serialized->find("keep_after_send") == std::string::npos);
}

TEST_CASE("syntax failure enables read-only protection", "[config]") {
  const auto result = lazycom::config::parse_config_toml("version = [\n");

  REQUIRE_FALSE(result.accepted);
  REQUIRE(result.read_only);
  REQUIRE(has_path(result.errors, "$syntax"));
}

TEST_CASE("unknown major versions are rejected", "[config]") {
  const auto result = lazycom::config::parse_config_toml("version = 2\n");
  REQUIRE_FALSE(result.accepted);
  REQUIRE(result.read_only);
  REQUIRE(has_path(result.errors, "version"));
}

TEST_CASE("config hard limits include all eight managed budget classes",
          "[config]") {
  auto snapshot = lazycom::config::ConfigSnapshot{};
  snapshot.receive.visible_buffer_mib = 49;
  snapshot.queues.log_max_mib = 17;
  snapshot.queues.rx_ingress_max_mib = 9;
  snapshot.queues.tx_max_mib = 9;
  snapshot.send.history_max_mib = 17;
  snapshot.queues.owner_command_max_mib = 17;

  const auto errors = lazycom::config::validate_config_snapshot(snapshot);
  REQUIRE(has_path(errors, "budget.ui_visible_mib"));
  REQUIRE(has_path(errors, "budget.session_log_mib"));
  REQUIRE(has_path(errors, "budget.rx_ingress_mib"));
  REQUIRE(has_path(errors, "budget.tx_mib"));
  REQUIRE(has_path(errors, "budget.send_history_mib"));
  REQUIRE(has_path(errors, "budget.owner_and_scratch_mib"));
  REQUIRE(has_path(errors, "budget.total_mib"));
}

TEST_CASE("TX queue current request and draft share the eight MiB category",
          "[config]") {
  auto snapshot = lazycom::config::ConfigSnapshot{};
  snapshot.queues.tx_max_mib = 8;

  const auto budget = lazycom::config::managed_memory_budget(snapshot);
  REQUIRE(budget.tx_mib > 8);
  REQUIRE(has_path(lazycom::config::validate_config_snapshot(snapshot),
                   "budget.tx_mib"));
}

TEST_CASE("managed budget includes count-based metadata", "[config]") {
  auto snapshot = lazycom::config::ConfigSnapshot{};
  snapshot.receive.visible_buffer_mib = 1;
  snapshot.receive.visible_max_records = 1000000;
  snapshot.queues.tx_max_mib = 1;
  snapshot.queues.tx_max_messages = 65536;
  snapshot.queues.owner_command_max_mib = 1;
  snapshot.queues.owner_command_max_messages = 65536;

  const auto errors = lazycom::config::validate_config_snapshot(snapshot);
  REQUIRE(has_path(errors, "budget.ui_visible_mib"));
  REQUIRE(has_path(errors, "budget.tx_mib"));
  REQUIRE(has_path(errors, "budget.owner_and_scratch_mib"));
}

TEST_CASE("quick-send parses sparse slots and validates decoded data",
          "[config]") {
  const auto result = lazycom::config::parse_quick_send_toml(R"toml(
version = 1
[[slots]]
index = 1
name = "Query"
mode = "txt"
content = "AT+INFO"
newline = "session"
note = ""
[[slots]]
index = 20
mode = "hex"
content = "0x00 ff 7A"
newline = "crlf"
)toml");

  REQUIRE(result.accepted);
  REQUIRE(result.snapshot.slots[0]);
  REQUIRE(result.snapshot.slots[19]);
  REQUIRE(result.snapshot.slots[19]->mode == lazycom::config::SendMode::Hex);
}

TEST_CASE("quick-send collects duplicate enum and HEX errors", "[config]") {
  const auto result = lazycom::config::parse_quick_send_toml(R"toml(
version = 1
[[slots]]
index = 2
name = "ok"
mode = "hex"
content = "0x0g"
newline = "bad"
[[slots]]
index = 2
)toml");

  REQUIRE_FALSE(result.accepted);
  REQUIRE(has_path(result.errors, "slots[0].content"));
  REQUIRE(has_path(result.errors, "slots[0].newline"));
  REQUIRE(has_path(result.errors, "slots[1].index"));
}

TEST_CASE("quick-send accepts empty slots and contains at most twenty entries",
          "[config]") {
  const auto missing = lazycom::config::parse_quick_send_toml(R"toml(
version = 1
[[slots]]
content = "missing index"
)toml");
  REQUIRE(missing.accepted);
  REQUIRE(missing.errors.empty());
  REQUIRE_FALSE(missing.snapshot.slots[0].has_value());

  std::string oversized = "version = 1\n";
  for (std::size_t index = 1U; index <= 21U; ++index) {
    oversized += "[[slots]]\nindex = " + std::to_string(index) + "\n";
  }
  const auto too_many = lazycom::config::parse_quick_send_toml(oversized);
  REQUIRE_FALSE(too_many.accepted);
  REQUIRE(has_path(too_many.errors, "slots"));
}

TEST_CASE("quick-send programmatic validation enforces UTF-8 and byte limits",
          "[config]") {
  lazycom::config::QuickSendSnapshot snapshot;
  lazycom::config::QuickSendSlot slot;
  slot.index = 1;
  slot.name = std::string(65, 'n');
  slot.mode = lazycom::config::SendMode::Txt;
  slot.content = std::string{"\xC0\x80", 2};
  slot.note = std::string(257, 'x');
  snapshot.slots[0] = slot;

  const auto errors = lazycom::config::validate_quick_send_snapshot(snapshot);
  REQUIRE(has_path(errors, "slots[0].name"));
  REQUIRE(has_path(errors, "slots[0].content"));
  REQUIRE(has_path(errors, "slots[0].note"));
}

TEST_CASE("quick-send aggregate model memory is bounded", "[config]") {
  lazycom::config::QuickSendSnapshot snapshot;
  for (std::size_t index = 0; index < 4; ++index) {
    lazycom::config::QuickSendSlot slot;
    slot.index = static_cast<std::uint32_t>(index + 1U);
    slot.content.assign(lazycom::config::kMaximumPayloadBytes, 'x');
    snapshot.slots[index] = std::move(slot);
  }

  const auto errors = lazycom::config::validate_quick_send_snapshot(snapshot);
  REQUIRE(has_path(errors, "budget.model_and_search_mib"));
}

TEST_CASE("serialization refuses documents beyond their next-load limit",
          "[config]") {
  const std::string oversized_state(lazycom::config::kStateMaximumBytes + 1U,
                                    'x');
  REQUIRE_FALSE(lazycom::config::serialize_state_toml(
      lazycom::config::StateSnapshot{}, oversized_state));

  lazycom::config::QuickSendSnapshot quick_send;
  lazycom::config::QuickSendSlot slot;
  slot.index = 1;
  slot.content.assign(lazycom::config::kMaximumPayloadBytes, '\0');
  quick_send.slots[0] = std::move(slot);
  REQUIRE_FALSE(lazycom::config::serialize_quick_send_toml(quick_send));
}

TEST_CASE("public TOML parsers reject oversized documents before parsing",
          "[config]") {
  const std::string config(lazycom::config::kConfigMaximumBytes + 1U, 'x');
  const std::string quick_send(lazycom::config::kQuickSendMaximumBytes + 1U,
                               'x');
  const std::string state(lazycom::config::kStateMaximumBytes + 1U, 'x');
  REQUIRE(
      has_path(lazycom::config::parse_config_toml(config).errors, "$document"));
  REQUIRE(has_path(lazycom::config::parse_quick_send_toml(quick_send).errors,
                   "$document"));
  REQUIRE(
      has_path(lazycom::config::parse_state_toml(state).errors, "$document"));
}

TEST_CASE("quick-send unknown keys survive a rewrite", "[config]") {
  const auto parsed = lazycom::config::parse_quick_send_toml(R"toml(
version = 1
future_root = "keep"
[[slots]]
index = 1
content = "ping"
future_slot = 7
)toml");
  REQUIRE(parsed.accepted);
  REQUIRE(has_path(parsed.warnings, "future_root"));
  REQUIRE(has_path(parsed.warnings, "slots[0].future_slot"));

  const auto serialized = lazycom::config::serialize_quick_send_toml(
      parsed.snapshot, parsed.document);
  REQUIRE(serialized);
  REQUIRE(serialized->find("future_root") != std::string::npos);
  REQUIRE(serialized->find("future_slot") != std::string::npos);
  const auto round_trip = lazycom::config::parse_quick_send_toml(*serialized);
  REQUIRE(round_trip.accepted);
  REQUIRE(round_trip.snapshot.slots[0]->content == "ping");
}

TEST_CASE("quick-send full slots survive repeated clear and refill",
          "[config]") {
  lazycom::config::QuickSendSnapshot snapshot;
  for (std::size_t position = 0; position < snapshot.slots.size(); ++position) {
    lazycom::config::QuickSendSlot slot;
    slot.index = static_cast<std::uint32_t>(position + 1U);
    slot.content = "payload";
    snapshot.slots[position] = std::move(slot);
  }
  auto document = lazycom::config::serialize_quick_send_toml(
      snapshot, "version = 1\n[[slots]]\nindex = 1\nfuture_slot = 7\n");
  REQUIRE(document);
  for (std::uint32_t pass = 0U; pass < 3U; ++pass) {
    CAPTURE(pass);
    snapshot.slots[0].reset();
    document = lazycom::config::serialize_quick_send_toml(snapshot, *document);
    REQUIRE(document);
    auto loaded = lazycom::config::parse_quick_send_toml(*document);
    REQUIRE(loaded.accepted);
    CHECK(loaded.snapshot == snapshot);
    CHECK(document->find("future_slot = 7") != std::string::npos);

    lazycom::config::QuickSendSlot slot;
    slot.index = 1U;
    slot.content = "new payload";
    snapshot.slots[0] = std::move(slot);
    document = lazycom::config::serialize_quick_send_toml(snapshot, *document);
    REQUIRE(document);
    loaded = lazycom::config::parse_quick_send_toml(*document);
    REQUIRE(loaded.accepted);
    CHECK(loaded.snapshot == snapshot);
    CHECK(document->find("future_slot = 7") != std::string::npos);
  }
}

TEST_CASE("quick-send serialization enforces the next load's combined budget",
          "[config]") {
  lazycom::config::QuickSendSnapshot snapshot;
  for (std::uint32_t index = 0U; index < 3U; ++index) {
    lazycom::config::QuickSendSlot slot;
    slot.index = index + 1U;
    slot.content.assign(index == 2U ? 512U * 1024U : 1024U * 1024U, 'x');
    snapshot.slots[index] = std::move(slot);
  }
  REQUIRE(lazycom::config::validate_quick_send_snapshot(snapshot).empty());
  const std::string preserved =
      "version = 1\nfuture_blob = '" + std::string(1024U * 1024U, 'x') + "'\n";
  REQUIRE(lazycom::config::parse_quick_send_toml(preserved).accepted);
  REQUIRE_FALSE(
      lazycom::config::serialize_quick_send_toml(snapshot, preserved));
}

TEST_CASE("state accepts zero or 10ms through one day only", "[config]") {
  REQUIRE(lazycom::config::parse_state_toml(
              "version = 1\nlast_quick_send_slot = 20\nlast_interval_ms = 0\n")
              .accepted);
  REQUIRE(lazycom::config::parse_state_toml(
              "version = 1\nlast_quick_send_slot = 1\nlast_interval_ms = 10\n")
              .accepted);
  const auto invalid = lazycom::config::parse_state_toml(
      "version = 1\nlast_quick_send_slot = 21\nlast_interval_ms = 9\n");
  REQUIRE_FALSE(invalid.accepted);
  REQUIRE(invalid.read_only);
  REQUIRE(has_path(invalid.errors, "last_quick_send_slot"));
  REQUIRE(has_path(invalid.errors, "last_interval_ms"));
}

TEST_CASE("state persists receive visibility preferences", "[config]") {
  const auto parsed = lazycom::config::parse_state_toml(R"toml(
version = 1
show_rx = false
show_tx = true
show_system = false
show_error = true
)toml");
  REQUIRE(parsed.accepted);
  CHECK_FALSE(parsed.snapshot.show_rx);
  CHECK(parsed.snapshot.show_tx);
  CHECK_FALSE(parsed.snapshot.show_system);
  CHECK(parsed.snapshot.show_error);
  const auto serialized =
      lazycom::config::serialize_state_toml(parsed.snapshot, parsed.document);
  REQUIRE(serialized);
  const auto round_trip = lazycom::config::parse_state_toml(*serialized);
  REQUIRE(round_trip.accepted);
  CHECK(round_trip.snapshot == parsed.snapshot);
}

TEST_CASE("state rejects hiding every receive record type", "[config]") {
  const auto parsed = lazycom::config::parse_state_toml(R"toml(
version = 1
show_rx = false
show_tx = false
show_system = false
show_error = false
)toml");
  REQUIRE_FALSE(parsed.accepted);
  REQUIRE(parsed.read_only);
  REQUIRE(has_path(parsed.errors, "show_*"));
}

TEST_CASE("logging directory rejects embedded NUL", "[config]") {
  const auto parsed = lazycom::config::parse_config_toml(
      "version = 1\n[logging]\ndirectory = \"/tmp\\u0000redirect\"\n");
  REQUIRE_FALSE(parsed.accepted);
  REQUIRE(has_path(parsed.errors, "logging.directory"));

  auto snapshot = lazycom::config::ConfigSnapshot{};
  snapshot.logging.directory = std::string{"/tmp\0redirect", 13};
  REQUIRE(has_path(lazycom::config::validate_config_snapshot(snapshot),
                   "logging.directory"));
}

TEST_CASE("state unknown keys warn and survive a rewrite", "[config]") {
  const auto parsed = lazycom::config::parse_state_toml(
      "version = 1\nlast_interval_ms = 0\nfuture_ui = true\n");
  REQUIRE(parsed.accepted);
  REQUIRE(has_path(parsed.warnings, "future_ui"));

  const auto serialized =
      lazycom::config::serialize_state_toml(parsed.snapshot, parsed.document);
  REQUIRE(serialized);
  REQUIRE(serialized->find("future_ui = true") != std::string::npos);
  REQUIRE(lazycom::config::parse_state_toml(*serialized).accepted);
}
