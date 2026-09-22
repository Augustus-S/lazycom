#include <lazycom/ui/receive_view_model.hpp>
#include <lazycom/ui/tui.hpp>

#include <catch2/catch_test_macros.hpp>
#include <ftxui/component/event.hpp>

using lazycom::ui::BracketedPaste;
using lazycom::ui::consume_paste_event;
using lazycom::ui::edit_utf8_text;
using lazycom::ui::parse_receive_vim_command;
using lazycom::ui::PasteConsumeResult;
using lazycom::ui::ReceiveVimAction;
using lazycom::ui::route_key;
using lazycom::ui::RoutedAction;
using lazycom::ui::RouteInput;
using lazycom::ui::RouteMode;
using lazycom::ui::Utf8EditAction;

TEST_CASE(
    "receive coordinates retain stable IDs through filtering and eviction",
    "[ui]") {
  using namespace lazycom;
  std::deque<app::VisibleRecord> records;
  for (std::uint64_t id = 1U; id <= 6U; ++id) {
    app::VisibleRecord record;
    record.record_id = id;
    record.sequence = id <= 3U ? id : id - 3U;
    record.direction =
        id % 2U == 0U ? app::RecordDirection::Tx : app::RecordDirection::Rx;
    records.push_back(std::move(record));
  }
  const app::DirectionFilter rx_only{true, false, false, false};
  ui::ReceiveViewModel view;
  {
    const ui::ReceiveCoordinates coordinates{records, rx_only};
    REQUIRE(coordinates.size() == 3U);
    view.normalize_cursor(coordinates, true);
    CHECK(view.cursor() == 5U);
    const auto [begin, end] = view.viewport(coordinates, 2U, true);
    CHECK(coordinates.id(begin) == 3U);
    CHECK(end == 3U);
    view.move(coordinates, false, 1U);
    CHECK(view.cursor() == 3U);
    CHECK_FALSE(view.at_bottom());
    view.set_matches({1U, 3U, 5U});
    REQUIRE(view.scroll_to_match(coordinates));
    CHECK(view.anchor() == 1U);
  }
  while (records.front().record_id < 4U) {
    records.pop_front();
  }
  {
    const ui::ReceiveCoordinates coordinates{records, rx_only};
    view.normalize_cursor(coordinates);
    CHECK(view.cursor() == 5U);
    CHECK(view.at_bottom());
    view.prune_matches(records, coordinates);
    CHECK(view.match_count() == 1U);
    CHECK(view.anchor() == 5U);
    view.next_match(true);
    REQUIRE(view.scroll_to_match(coordinates));
  }
  records.clear();
  const ui::ReceiveCoordinates empty{records, rx_only};
  view.normalize_cursor(empty);
  view.prune_matches(records, empty);
  CHECK_FALSE(view.cursor());
  CHECK_FALSE(view.anchor());
  CHECK(view.match_count() == 0U);
}

TEST_CASE("key routing respects mode priority and connection guards", "[ui]") {
  struct Route {
    RouteInput input;
    RoutedAction expected;
  };
  const Route routes[]{
      {{RouteMode::Modal, "F1", false, false, false}, RoutedAction::Help},
      {{RouteMode::Modal, "Q", false, false, false}, RoutedAction::None},
      {{RouteMode::Modal, "q", false, false, false}, RoutedAction::None},
      {{RouteMode::Search, "C", true, false, false}, RoutedAction::None},
      {{RouteMode::Search, "q", true, false, false}, RoutedAction::None},
      {{RouteMode::SendEdit, "g", true, false, false}, RoutedAction::None},
      {{RouteMode::SendEdit, "q", true, false, false}, RoutedAction::None},
      {{RouteMode::Confirm, "q", true, false, false}, RoutedAction::None},
      {{RouteMode::Help, "q", true, false, false}, RoutedAction::None},
      {{RouteMode::ErrorDialog, "q", true, false, false}, RoutedAction::None},
      {{RouteMode::ErrorDialog, "Enter", true, false, false},
       RoutedAction::Escape},
      {{RouteMode::Normal, "C", false, false, false}, RoutedAction::Connection},
      {{RouteMode::Normal, "g", false, false, false}, RoutedAction::ToggleLog},
      {{RouteMode::Normal, "G", false, false, false},
       RoutedAction::LoggingConfig},
      {{RouteMode::Normal, "f", false, false, false}, RoutedAction::None},
      {{RouteMode::Normal, "f", true, false, false},
       RoutedAction::QuickExecute},
      {{RouteMode::Normal, "S", true, false, false}, RoutedAction::None},
      {{RouteMode::Normal, "q", true, false, false}, RoutedAction::Quit},
      {{RouteMode::Normal, "Q", true, false, false}, RoutedAction::None},
      {{RouteMode::Normal, "y", true, false, false},
       RoutedAction::CopySelection},
      {{RouteMode::SendEdit, "Enter", true, false, false},
       RoutedAction::Submit},
      {{RouteMode::SendEdit, "Alt+Enter", true, false, false},
       RoutedAction::InsertNewline},
      {{RouteMode::ReceiveBrowse, "PgUp", true, false, false},
       RoutedAction::PageUp},
      {{RouteMode::ReceiveBrowse, "i", true, false, false}, RoutedAction::Edit},
      {{RouteMode::ReceiveBrowse, "q", true, false, false}, RoutedAction::Quit},
      {{RouteMode::ReceiveBrowse, "C", true, false, false}, RoutedAction::None},
      {{RouteMode::ReceiveBrowse, "Q", true, false, false}, RoutedAction::None},
      {{RouteMode::ReceiveBrowse, "y", true, false, false}, RoutedAction::None},
      {{RouteMode::Search, "F3", true, false, false}, RoutedAction::SearchNext},
      {{RouteMode::Search, "End", true, false, false}, RoutedAction::End},
      {{RouteMode::Modal, "F5", false, true, false}, RoutedAction::Scan},
      {{RouteMode::Modal, "F5", false, false, false}, RoutedAction::None},
      {{RouteMode::Modal, "Tab", false, false, false}, RoutedAction::FocusNext},
      {{RouteMode::Modal, "Shift+Tab", false, false, false},
       RoutedAction::FocusPrevious},
      {{RouteMode::Search, "Tab", true, false, false}, RoutedAction::FocusNext},
      {{RouteMode::Search, "Right", true, false, false}, RoutedAction::None},
      {{RouteMode::Search, "Right", true, false, true},
       RoutedAction::SelectNext},
      {{RouteMode::Search, " ", true, false, true}, RoutedAction::SelectNext},
      {{RouteMode::Confirm, "Right", true, false, false},
       RoutedAction::SelectNext},
      {{RouteMode::Confirm, "Left", true, false, false},
       RoutedAction::SelectPrevious},
      {{RouteMode::Confirm, "Enter", true, false, false}, RoutedAction::Apply},
  };
  for (const auto &[input, expected] : routes) {
    CAPTURE(input.mode, input.key, input.connected);
    CHECK(route_key(input) == expected);
  }
}

TEST_CASE("receive Vim commands parse bounded contextual sequences", "[ui]") {
  CHECK(parse_receive_vim_command("j").action == ReceiveVimAction::MoveDown);
  CHECK(parse_receive_vim_command("k").action == ReceiveVimAction::MoveUp);
  CHECK(parse_receive_vim_command("10j").count == 10U);
  CHECK(parse_receive_vim_command("10j").action == ReceiveVimAction::MoveDown);
  CHECK(parse_receive_vim_command("11k").count == 11U);
  CHECK(parse_receive_vim_command("g").action == ReceiveVimAction::Pending);
  CHECK(parse_receive_vim_command("gg").action == ReceiveVimAction::First);
  CHECK(parse_receive_vim_command("G").action == ReceiveVimAction::Last);
  CHECK(parse_receive_vim_command("y").action == ReceiveVimAction::Pending);
  CHECK(parse_receive_vim_command("yy").action == ReceiveVimAction::YankLine);
  CHECK(parse_receive_vim_command("10yy").action == ReceiveVimAction::Invalid);
  CHECK(parse_receive_vim_command("0j").action == ReceiveVimAction::Invalid);
  CHECK(parse_receive_vim_command("1000001j").action ==
        ReceiveVimAction::MoveDown);
  CHECK(parse_receive_vim_command("1000001j").count == 1'000'000U);
}

TEST_CASE("bounded text editing moves and deletes on UTF-8 boundaries",
          "[ui]") {
  std::string value = "A\xE4\xB8\xAD\xF0\x9F\x99\x82";
  std::size_t cursor = value.size();
  REQUIRE(edit_utf8_text(value, cursor, Utf8EditAction::MoveLeft, {}, 16U));
  CHECK(cursor == 4U);
  REQUIRE(edit_utf8_text(value, cursor, Utf8EditAction::Backspace, {}, 16U));
  CHECK(value == "A\xF0\x9F\x99\x82");
  CHECK(cursor == 1U);
  REQUIRE(edit_utf8_text(value, cursor, Utf8EditAction::Delete, {}, 16U));
  CHECK(value == "A");

  REQUIRE(edit_utf8_text(value, cursor, Utf8EditAction::Insert, "\xE4\xB8\xAD",
                         4U));
  CHECK(value == "A\xE4\xB8\xAD");
  CHECK_FALSE(edit_utf8_text(value, cursor, Utf8EditAction::Insert, "x", 4U));
  CHECK_FALSE(
      edit_utf8_text(value, cursor, Utf8EditAction::Insert, "\xE4\xB8", 16U));
}

TEST_CASE("bracketed paste rejects unsafe input atomically", "[ui]") {
  BracketedPaste paste;
  SECTION("non-text contexts consume everything through the end marker") {
    paste.start(RouteMode::Normal, 1U, 0U, false);
    CHECK(paste.consume("i", true, false, RouteMode::Normal, 1U) ==
          PasteConsumeResult::Consumed);
    CHECK(paste.consume("q", true, false, RouteMode::Normal, 1U) ==
          PasteConsumeResult::Consumed);
    CHECK(paste.consume({}, false, true, RouteMode::Normal, 1U) ==
          PasteConsumeResult::Rejected);
  }

  SECTION("invalid UTF-8 and overflow reject atomically") {
    paste.start(RouteMode::SendEdit, 2U, 2U, true);
    CHECK(paste.consume("A", true, false, RouteMode::SendEdit, 2U) ==
          PasteConsumeResult::Consumed);
    CHECK(paste.consume("\xE4\xB8\xAD", true, false, RouteMode::SendEdit, 2U) ==
          PasteConsumeResult::Consumed);
    CHECK(paste.consume("q", true, false, RouteMode::SendEdit, 2U) ==
          PasteConsumeResult::Consumed);
    CHECK(paste.consume({}, false, true, RouteMode::SendEdit, 2U) ==
          PasteConsumeResult::Rejected);
    CHECK(paste.take_text().empty());

    paste.start(RouteMode::Search, 3U, 16U, true);
    CHECK(paste.consume(std::string_view{"\xE4\xB8", 2U}, true, false,
                        RouteMode::Search, 3U) == PasteConsumeResult::Consumed);
    CHECK(paste.consume({}, false, true, RouteMode::Search, 3U) ==
          PasteConsumeResult::Rejected);
  }
}

TEST_CASE(
    "bracketed paste adapts owned FTXUI events and detects context changes",
    "[ui]") {
  for (const bool changed_context : {false, true}) {
    CAPTURE(changed_context);
    BracketedPaste paste;
    paste.start(RouteMode::SendEdit, 4U, 16U, true);
    CHECK(consume_paste_event(paste, ftxui::Event::Character("\xE4\xB8\xAD"),
                              RouteMode::SendEdit,
                              4U) == PasteConsumeResult::Consumed);
    const auto context = changed_context ? 5U : 4U;
    CHECK(consume_paste_event(paste, ftxui::Event::Return, RouteMode::SendEdit,
                              context) == PasteConsumeResult::Consumed);
    CHECK(consume_paste_event(paste, ftxui::Event::Character("NEXT"),
                              RouteMode::SendEdit,
                              context) == PasteConsumeResult::Consumed);
    CHECK(consume_paste_event(paste, ftxui::Event::Special("\x1b[201~"),
                              RouteMode::SendEdit, context) ==
          (changed_context ? PasteConsumeResult::Rejected
                           : PasteConsumeResult::Completed));
    CHECK_FALSE(paste.active());
    CHECK(paste.take_text() == (changed_context ? "" : "\xE4\xB8\xAD\nNEXT"));
  }
}
