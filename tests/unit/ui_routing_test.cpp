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

TEST_CASE("F1 is the only unconditional application route") {
  REQUIRE(route_key({RouteMode::Modal, "F1", false, false, false}) ==
          RoutedAction::Help);
  REQUIRE(route_key({RouteMode::Modal, "Q", false, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::Modal, "q", false, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::Search, "C", true, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::Search, "q", true, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::SendEdit, "g", true, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::SendEdit, "q", true, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::Confirm, "q", true, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::Help, "q", true, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::ErrorDialog, "q", true, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::ErrorDialog, "Enter", true, false, false}) ==
          RoutedAction::Escape);
}

TEST_CASE("normal shortcuts preserve case and connection guards") {
  REQUIRE(route_key({RouteMode::Normal, "C", false, false, false}) ==
          RoutedAction::Connection);
  REQUIRE(route_key({RouteMode::Normal, "g", false, false, false}) ==
          RoutedAction::ToggleLog);
  REQUIRE(route_key({RouteMode::Normal, "G", false, false, false}) ==
          RoutedAction::LoggingConfig);
  REQUIRE(route_key({RouteMode::Normal, "f", false, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::Normal, "f", true, false, false}) ==
          RoutedAction::QuickExecute);
  REQUIRE(route_key({RouteMode::Normal, "S", true, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::Normal, "q", true, false, false}) ==
          RoutedAction::Quit);
  REQUIRE(route_key({RouteMode::Normal, "Q", true, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::Normal, "y", true, false, false}) ==
          RoutedAction::CopySelection);
}

TEST_CASE("editing browsing and search have contextual escape semantics") {
  REQUIRE(route_key({RouteMode::SendEdit, "Enter", true, false, false}) ==
          RoutedAction::Submit);
  REQUIRE(route_key({RouteMode::SendEdit, "Alt+Enter", true, false, false}) ==
          RoutedAction::InsertNewline);
  REQUIRE(route_key({RouteMode::ReceiveBrowse, "PgUp", true, false, false}) ==
          RoutedAction::PageUp);
  REQUIRE(route_key({RouteMode::ReceiveBrowse, "i", true, false, false}) ==
          RoutedAction::Edit);
  REQUIRE(route_key({RouteMode::ReceiveBrowse, "q", true, false, false}) ==
          RoutedAction::Quit);
  REQUIRE(route_key({RouteMode::ReceiveBrowse, "C", true, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::ReceiveBrowse, "Q", true, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::ReceiveBrowse, "y", true, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::Search, "F3", true, false, false}) ==
          RoutedAction::SearchNext);
  REQUIRE(route_key({RouteMode::Search, "End", true, false, false}) ==
          RoutedAction::End);
  REQUIRE(route_key({RouteMode::Modal, "F5", false, true, false}) ==
          RoutedAction::Scan);
  REQUIRE(route_key({RouteMode::Modal, "F5", false, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::Modal, "Tab", false, false, false}) ==
          RoutedAction::FocusNext);
  REQUIRE(route_key({RouteMode::Modal, "Shift+Tab", false, false, false}) ==
          RoutedAction::FocusPrevious);
}

TEST_CASE("receive Vim commands parse bounded contextual sequences") {
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

TEST_CASE("search direction and destructive confirmation are contextual") {
  REQUIRE(route_key({RouteMode::Search, "Tab", true, false, false}) ==
          RoutedAction::FocusNext);
  REQUIRE(route_key({RouteMode::Search, "Right", true, false, false}) ==
          RoutedAction::None);
  REQUIRE(route_key({RouteMode::Search, "Right", true, false, true}) ==
          RoutedAction::SelectNext);
  REQUIRE(route_key({RouteMode::Search, " ", true, false, true}) ==
          RoutedAction::SelectNext);
  REQUIRE(route_key({RouteMode::Confirm, "Right", true, false, false}) ==
          RoutedAction::SelectNext);
  REQUIRE(route_key({RouteMode::Confirm, "Left", true, false, false}) ==
          RoutedAction::SelectPrevious);
  REQUIRE(route_key({RouteMode::Confirm, "Enter", true, false, false}) ==
          RoutedAction::Apply);
}

TEST_CASE("bounded text editing moves and deletes on UTF-8 boundaries") {
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

TEST_CASE("bracketed paste collects text without routing embedded commands") {
  BracketedPaste paste;
  paste.start(RouteMode::SendEdit, 7U, 64U, true);

  CHECK(paste.consume("A", true, false, RouteMode::SendEdit, 7U) ==
        PasteConsumeResult::Consumed);
  CHECK(paste.consume("T", true, false, RouteMode::SendEdit, 7U) ==
        PasteConsumeResult::Consumed);
  CHECK(paste.consume("\n", true, false, RouteMode::SendEdit, 7U) ==
        PasteConsumeResult::Consumed);
  CHECK(paste.consume("NEXT", true, false, RouteMode::SendEdit, 7U) ==
        PasteConsumeResult::Consumed);
  CHECK(paste.consume({}, false, true, RouteMode::SendEdit, 7U) ==
        PasteConsumeResult::Completed);
  CHECK(paste.take_text() == "AT\nNEXT");
  CHECK_FALSE(paste.active());
}

TEST_CASE("bracketed paste rejects unsafe input without leaking later keys") {
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

  SECTION("context changes reject the whole paste") {
    paste.start(RouteMode::Modal, 9U, 16U, true);
    CHECK(paste.consume("old", true, false, RouteMode::Modal, 9U) ==
          PasteConsumeResult::Consumed);
    CHECK(paste.consume("new", true, false, RouteMode::Modal, 10U) ==
          PasteConsumeResult::Consumed);
    CHECK(paste.consume({}, false, true, RouteMode::Modal, 10U) ==
          PasteConsumeResult::Rejected);
    CHECK(paste.take_text().empty());
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

TEST_CASE("bracketed paste adapts owned FTXUI event input safely") {
  BracketedPaste paste;
  paste.start(RouteMode::SendEdit, 4U, 16U, true);

  CHECK(consume_paste_event(paste, ftxui::Event::Character("\xE4\xB8\xAD"),
                            RouteMode::SendEdit,
                            4U) == PasteConsumeResult::Consumed);
  CHECK(consume_paste_event(paste, ftxui::Event::Return, RouteMode::SendEdit,
                            4U) == PasteConsumeResult::Consumed);
  CHECK(consume_paste_event(paste, ftxui::Event::Special("\x1b[201~"),
                            RouteMode::SendEdit,
                            4U) == PasteConsumeResult::Completed);
  CHECK(paste.take_text() == "\xE4\xB8\xAD\n");
}

TEST_CASE("custom events can advance context without becoming paste data") {
  BracketedPaste paste;
  paste.start(RouteMode::Modal, 10U, 16U, true);
  CHECK(consume_paste_event(paste, ftxui::Event::Character("old"),
                            RouteMode::Modal,
                            10U) == PasteConsumeResult::Consumed);

  // Tui handles Custom before the paste branch. A context change made by that
  // tick is observed by the next payload event and rejects the whole paste.
  CHECK(consume_paste_event(paste, ftxui::Event::Character("new"),
                            RouteMode::Modal,
                            11U) == PasteConsumeResult::Consumed);
  CHECK(consume_paste_event(paste, ftxui::Event::Special("\x1b[201~"),
                            RouteMode::Modal,
                            11U) == PasteConsumeResult::Rejected);
  CHECK(paste.take_text().empty());
}
