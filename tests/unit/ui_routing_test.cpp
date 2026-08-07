#include <lazycom/ui/tui.hpp>

#include <catch2/catch_test_macros.hpp>

using lazycom::ui::edit_utf8_text;
using lazycom::ui::parse_receive_vim_command;
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
