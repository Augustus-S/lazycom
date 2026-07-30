#include <lazycom/ui/tui.hpp>

#include <lazycom/config/schema.hpp>
#include <lazycom/logging/schema.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace lazycom::ui {
namespace {

using namespace ftxui;
using namespace std::chrono_literals;

namespace theme {
const Color base = Color::RGB(35U, 33U, 54U);
const Color surface = Color::RGB(42U, 39U, 63U);
const Color overlay = Color::RGB(57U, 53U, 82U);
const Color text = Color::RGB(224U, 222U, 244U);
const Color muted = Color::RGB(110U, 106U, 134U);
const Color subtle = Color::RGB(144U, 140U, 170U);
const Color love = Color::RGB(235U, 111U, 146U);
const Color gold = Color::RGB(246U, 193U, 119U);
const Color rose = Color::RGB(234U, 154U, 151U);
const Color pine = Color::RGB(62U, 143U, 176U);
const Color foam = Color::RGB(156U, 207U, 216U);
const Color iris = Color::RGB(196U, 167U, 231U);
const Color success = Color::RGB(156U, 207U, 124U);
const Color shortcut_text = Color::RGB(255U, 255U, 255U);
} // namespace theme

[[nodiscard]] bool key_is(std::string_view key, std::string_view expected) {
  return key == expected;
}

[[nodiscard]] std::string event_key(const Event &event) {
  if (event == Event::F1) {
    return "F1";
  }
  if (event == Event::F3) {
    return "F3";
  }
  if (event == Event::F5) {
    return "F5";
  }
  if (event == Event::Escape) {
    return "Esc";
  }
  if (event == Event::Return) {
    return "Enter";
  }
  if (event == Event::ArrowUp) {
    return "Up";
  }
  if (event == Event::ArrowDown) {
    return "Down";
  }
  if (event == Event::ArrowLeft) {
    return "Left";
  }
  if (event == Event::ArrowRight) {
    return "Right";
  }
  if (event == Event::PageUp) {
    return "PgUp";
  }
  if (event == Event::PageDown) {
    return "PgDn";
  }
  if (event == Event::Home) {
    return "Home";
  }
  if (event == Event::End) {
    return "End";
  }
  if (event == Event::Backspace) {
    return "Backspace";
  }
  if (event == Event::Delete) {
    return "Delete";
  }
  if (event == Event::Tab) {
    return "Tab";
  }
  if (event == Event::TabReverse) {
    return "Shift+Tab";
  }
  if (event == Event::CtrlL) {
    return "Ctrl+L";
  }
  if (event == Event::AltN) {
    return "Alt+N";
  }
  if (event == Event::Special("\x1b[1;2R")) {
    return "Shift+F3";
  }
  if (event.input() == "\x1b\r" || event.input() == "\x1b\n") {
    return "Alt+Enter";
  }
  if (event.input() == "\x1b[A") {
    return "Up";
  }
  if (event.input() == "\x1b[B") {
    return "Down";
  }
  if (event.input() == "\x1b[1;3A") {
    return "Alt+Up";
  }
  if (event.input() == "\x1b[1;3B") {
    return "Alt+Down";
  }
  if (event.is_character()) {
    return event.character();
  }
  return event.input();
}

[[nodiscard]] std::string connection_text(app::ConnectionState state) {
  switch (state) {
  case app::ConnectionState::Disconnected:
    return "Disconnected";
  case app::ConnectionState::Connecting:
    return "Connecting";
  case app::ConnectionState::Connected:
    return "Connected";
  case app::ConnectionState::Disconnecting:
    return "Disconnecting";
  case app::ConnectionState::Error:
    return "Error";
  }
  return "Error";
}

[[nodiscard]] std::string interaction_text(app::InteractionState state) {
  switch (state) {
  case app::InteractionState::Normal:
    return "Normal";
  case app::InteractionState::SendEdit:
    return "Input";
  case app::InteractionState::ReceiveBrowse:
    return "Receive";
  }
  return "Normal";
}

[[nodiscard]] std::string log_text(app::LogState state) {
  switch (state) {
  case app::LogState::Off:
    return "OFF";
  case app::LogState::Waiting:
    return "WAITING";
  case app::LogState::Recording:
    return "REC";
  case app::LogState::Error:
    return "ERROR";
  }
  return "ERROR";
}

[[nodiscard]] std::string uppercase_ascii(const std::string_view value) {
  std::string result{value};
  for (char &character : result) {
    if (character >= 'a' && character <= 'z') {
      character = static_cast<char>(character - 'a' + 'A');
    }
  }
  return result;
}

[[nodiscard]] std::string abbreviate_left(const std::string_view value,
                                          const std::size_t max_size) {
  if (value.size() <= max_size) {
    return std::string{value};
  }
  if (max_size <= 3U) {
    return std::string{value.substr(value.size() - max_size)};
  }
  return "..." + std::string{value.substr(value.size() - (max_size - 3U))};
}

[[nodiscard]] char parity_letter(const config::Parity parity) noexcept {
  switch (parity) {
  case config::Parity::None:
    return 'N';
  case config::Parity::Odd:
    return 'O';
  case config::Parity::Even:
    return 'E';
  case config::Parity::Mark:
    return 'M';
  case config::Parity::Space:
    return 'S';
  }
  return '?';
}

[[nodiscard]] std::string flow_text(const config::FlowControl flow) {
  switch (flow) {
  case config::FlowControl::None:
    return "None";
  case config::FlowControl::RtsCts:
    return "RTS/CTS";
  case config::FlowControl::XonXoff:
    return "XON/XOFF";
  }
  return "Unknown";
}

[[nodiscard]] Element divider() { return text(" | ") | color(theme::muted); }

[[nodiscard]] Element shortcut(const std::string_view key) {
  return text("<" + std::string{key} + ">") | color(theme::shortcut_text) |
         bold;
}

[[nodiscard]] std::string base64_encode(const std::string_view input) {
  static constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string output;
  output.reserve(((input.size() + 2U) / 3U) * 4U);
  for (std::size_t index = 0U; index < input.size(); index += 3U) {
    const auto first = static_cast<unsigned char>(input[index]);
    const auto second = index + 1U < input.size()
                            ? static_cast<unsigned char>(input[index + 1U])
                            : 0U;
    const auto third = index + 2U < input.size()
                           ? static_cast<unsigned char>(input[index + 2U])
                           : 0U;
    const std::uint32_t packed = (static_cast<std::uint32_t>(first) << 16U) |
                                 (static_cast<std::uint32_t>(second) << 8U) |
                                 static_cast<std::uint32_t>(third);
    output.push_back(alphabet[(packed >> 18U) & 0x3FU]);
    output.push_back(alphabet[(packed >> 12U) & 0x3FU]);
    output.push_back(
        index + 1U < input.size() ? alphabet[(packed >> 6U) & 0x3FU] : '=');
    output.push_back(index + 2U < input.size() ? alphabet[packed & 0x3FU]
                                               : '=');
  }
  return output;
}

[[nodiscard]] Element status_field(const std::string_view label,
                                   const std::string_view key,
                                   const std::string_view value,
                                   const Color value_color) {
  Elements parts;
  parts.push_back(text("[") | color(theme::subtle));
  parts.push_back(text(std::string{label}) | color(theme::pine));
  if (!key.empty()) {
    parts.push_back(shortcut(key));
  }
  parts.push_back(text(":") | color(theme::subtle));
  parts.push_back(text(std::string{value}) | color(value_color));
  parts.push_back(text("]") | color(theme::subtle));
  return hbox(std::move(parts));
}

[[nodiscard]] bool visible(const app::VisibleRecord &record,
                           const app::DirectionFilter &filter) {
  switch (record.direction) {
  case app::RecordDirection::Rx:
    return filter.rx;
  case app::RecordDirection::Tx:
    return filter.tx;
  case app::RecordDirection::System:
    return filter.system;
  case app::RecordDirection::Error:
    return filter.error;
  }
  return false;
}

[[nodiscard]] std::string status_error(const Error &error) {
  return std::string{error_descriptor(error.code).identifier} + ": " +
         logging::sanitize_message(error.detail);
}

} // namespace

RoutedAction route_key(const RouteInput &input) noexcept {
  if (key_is(input.key, "F1")) {
    return RoutedAction::Help;
  }
  if (input.mode == RouteMode::Help) {
    return key_is(input.key, "Esc") || key_is(input.key, "?")
               ? RoutedAction::Escape
               : RoutedAction::None;
  }
  if (input.mode == RouteMode::ErrorDialog) {
    return key_is(input.key, "Esc") || key_is(input.key, "Enter")
               ? RoutedAction::Escape
               : RoutedAction::None;
  }
  if (input.mode == RouteMode::Confirm) {
    if (key_is(input.key, "Esc")) {
      return RoutedAction::Escape;
    }
    if (key_is(input.key, "Tab")) {
      return RoutedAction::FocusNext;
    }
    if (key_is(input.key, "Shift+Tab")) {
      return RoutedAction::FocusPrevious;
    }
    if (key_is(input.key, "Right")) {
      return RoutedAction::SelectNext;
    }
    if (key_is(input.key, "Left")) {
      return RoutedAction::SelectPrevious;
    }
    return key_is(input.key, "Enter") ? RoutedAction::Apply
                                      : RoutedAction::None;
  }
  if (input.mode == RouteMode::Modal) {
    if (key_is(input.key, "Esc")) {
      return RoutedAction::Escape;
    }
    if (key_is(input.key, "F5") && input.device_modal) {
      return RoutedAction::Scan;
    }
    if (key_is(input.key, "Tab")) {
      return RoutedAction::FocusNext;
    }
    if (key_is(input.key, "Shift+Tab")) {
      return RoutedAction::FocusPrevious;
    }
    return key_is(input.key, "Enter") ? RoutedAction::Apply
                                      : RoutedAction::None;
  }
  if (input.mode == RouteMode::Search) {
    if (key_is(input.key, "Esc")) {
      return RoutedAction::Escape;
    }
    if (key_is(input.key, "End")) {
      return RoutedAction::End;
    }
    if (key_is(input.key, "F3")) {
      return RoutedAction::SearchNext;
    }
    if (key_is(input.key, "Shift+F3")) {
      return RoutedAction::SearchPrevious;
    }
    if (key_is(input.key, "Tab")) {
      return RoutedAction::FocusNext;
    }
    if (key_is(input.key, "Shift+Tab")) {
      return RoutedAction::FocusPrevious;
    }
    if (input.search_filter_focused &&
        (key_is(input.key, "Right") || key_is(input.key, " "))) {
      return RoutedAction::SelectNext;
    }
    if (input.search_filter_focused && key_is(input.key, "Left")) {
      return RoutedAction::SelectPrevious;
    }
    return key_is(input.key, "Enter") ? RoutedAction::Apply
                                      : RoutedAction::None;
  }
  if (input.mode == RouteMode::SendEdit) {
    if (key_is(input.key, "Esc")) {
      return RoutedAction::Escape;
    }
    if (key_is(input.key, "Enter")) {
      return RoutedAction::Submit;
    }
    if (key_is(input.key, "Alt+Enter")) {
      return RoutedAction::InsertNewline;
    }
    if (key_is(input.key, "Alt+Up")) {
      return RoutedAction::HistoryPrevious;
    }
    if (key_is(input.key, "Alt+Down")) {
      return RoutedAction::HistoryNext;
    }
    return RoutedAction::None;
  }
  if (input.mode == RouteMode::ReceiveBrowse) {
    if (key_is(input.key, "Esc")) {
      return RoutedAction::Escape;
    }
    if (key_is(input.key, "Up")) {
      return RoutedAction::ScrollUp;
    }
    if (key_is(input.key, "Down")) {
      return RoutedAction::ScrollDown;
    }
    if (key_is(input.key, "PgUp")) {
      return RoutedAction::PageUp;
    }
    if (key_is(input.key, "PgDn")) {
      return RoutedAction::PageDown;
    }
    if (key_is(input.key, "Home")) {
      return RoutedAction::Home;
    }
    if (key_is(input.key, "End")) {
      return RoutedAction::End;
    }
    if (key_is(input.key, "j") || key_is(input.key, "k") ||
        key_is(input.key, "g") || key_is(input.key, "G") ||
        key_is(input.key, "y") ||
        (input.key.size() == 1U && input.key.front() >= '0' &&
         input.key.front() <= '9')) {
      return RoutedAction::None;
    }
    if (key_is(input.key, "i")) {
      return RoutedAction::Edit;
    }
    if (key_is(input.key, "q")) {
      return RoutedAction::Quit;
    }
    return RoutedAction::None;
  }
  if (key_is(input.key, "?")) {
    return RoutedAction::Help;
  }
  if (key_is(input.key, "q")) {
    return RoutedAction::Quit;
  }
  if (key_is(input.key, "C")) {
    return RoutedAction::Connection;
  }
  if (key_is(input.key, "R")) {
    return RoutedAction::Browse;
  }
  if (key_is(input.key, "i")) {
    return RoutedAction::Edit;
  }
  if (key_is(input.key, "E")) {
    return RoutedAction::CommandPalette;
  }
  if (key_is(input.key, "X")) {
    return RoutedAction::Clear;
  }
  if (key_is(input.key, "Ctrl+L")) {
    return RoutedAction::Redraw;
  }
  if (key_is(input.key, " ")) {
    return RoutedAction::Pause;
  }
  if (key_is(input.key, "/")) {
    return RoutedAction::Search;
  }
  if (key_is(input.key, "F5")) {
    return RoutedAction::Scan;
  }
  if (key_is(input.key, "P")) {
    return RoutedAction::Port;
  }
  if (key_is(input.key, "B")) {
    return RoutedAction::Baud;
  }
  if (key_is(input.key, "D")) {
    return RoutedAction::DataFormat;
  }
  if (key_is(input.key, "N")) {
    return RoutedAction::Newline;
  }
  if (key_is(input.key, "V")) {
    return RoutedAction::View;
  }
  if (key_is(input.key, "H")) {
    return RoutedAction::SendMode;
  }
  if (key_is(input.key, "G")) {
    return RoutedAction::LoggingConfig;
  }
  if (key_is(input.key, "F")) {
    return RoutedAction::QuickConfig;
  }
  if (key_is(input.key, "g")) {
    return RoutedAction::ToggleLog;
  }
  if (key_is(input.key, "y")) {
    return RoutedAction::CopySelection;
  }
  if (key_is(input.key, "f") && input.connected) {
    return RoutedAction::QuickExecute;
  }
  return RoutedAction::None;
}

ReceiveVimResult
parse_receive_vim_command(const std::string_view command) noexcept {
  constexpr std::size_t maximum_count = 1'000'000U;
  if (command == "g" || command == "y") {
    return {ReceiveVimAction::Pending, 1U};
  }
  if (command == "gg") {
    return {ReceiveVimAction::First, 1U};
  }
  if (command == "G") {
    return {ReceiveVimAction::Last, 1U};
  }
  if (command == "yy") {
    return {ReceiveVimAction::YankLine, 1U};
  }
  if (command.empty()) {
    return {};
  }

  const char final = command.back();
  const bool movement = final == 'j' || final == 'k';
  const auto digits =
      movement ? command.substr(0U, command.size() - 1U) : command;
  if (digits.empty()) {
    return movement ? ReceiveVimResult{final == 'j' ? ReceiveVimAction::MoveDown
                                                    : ReceiveVimAction::MoveUp,
                                       1U}
                    : ReceiveVimResult{};
  }
  if (digits.front() == '0' ||
      !std::ranges::all_of(digits, [](const char value) {
        return value >= '0' && value <= '9';
      })) {
    return {};
  }
  std::size_t count{};
  const auto parsed =
      std::from_chars(digits.data(), digits.data() + digits.size(), count);
  if (parsed.ptr != digits.data() + digits.size() ||
      (parsed.ec != std::errc{} &&
       parsed.ec != std::errc::result_out_of_range)) {
    return {};
  }
  count = parsed.ec == std::errc::result_out_of_range
              ? maximum_count
              : std::min(count, maximum_count);
  if (!movement) {
    return {ReceiveVimAction::Pending, count};
  }
  return {final == 'j' ? ReceiveVimAction::MoveDown : ReceiveVimAction::MoveUp,
          count};
}

struct Tui::Impl {
  ScreenInteractive screen = ScreenInteractive::Fullscreen();
  std::unique_ptr<app::Application> application;
  std::string draft_input_value;
  int draft_cursor{};
  Component draft_input;
  RouteMode mode{RouteMode::Normal};
  RouteMode mode_before_help{RouteMode::Normal};
  std::string modal_kind;
  std::string modal_title;
  std::string edit_value;
  std::size_t edit_cursor{};
  std::vector<std::string> modal_options;
  std::string modal_parent;
  std::size_t selected_device{};
  config::SerialDefaults serial_candidate;
  config::ReceiveView rx_view_candidate{config::ReceiveView::Txt};
  config::ReceiveView tx_view_candidate{config::ReceiveView::Txt};
  app::DirectionFilter filter_candidate;
  config::LoggingSettings logging_candidate;
  std::size_t scroll_from_bottom{};
  std::string search_query;
  std::vector<std::uint64_t> search_matches;
  std::size_t search_match{};
  enum class SearchFocus : std::uint8_t { Query, Direction };
  enum class SearchDirection : std::uint8_t { All, Rx, Tx, System, Error };
  SearchFocus search_focus{SearchFocus::Query};
  SearchDirection search_direction{SearchDirection::All};
  enum class ConfirmAction : std::uint8_t {
    None,
    Quit,
    Clear,
    DeleteQuickSlot,
    ReplaceQuickTask,
  };
  ConfirmAction confirm_action{ConfirmAction::None};
  RouteMode confirm_return_mode{RouteMode::Normal};
  RouteMode alert_return_mode{RouteMode::Normal};
  bool confirm_accept{};
  std::uint32_t pending_quick_slot{};
  std::uint64_t pending_quick_interval{};
  bool awaiting_quick_save{};
  bool connect_after_port{};
  bool exit_ok{true};
  std::optional<std::uint64_t> receive_cursor;
  std::string receive_vim_pending;
  std::string selected_text;
  std::string ui_notice;

  Impl() {
    InputOption option;
    option.multiline = true;
    option.cursor_position = &draft_cursor;
    option.on_change = [this] {
      if (!application) {
        return;
      }
      application->set_draft(draft_input_value);
      const auto &stored = application->snapshot().draft;
      if (stored != draft_input_value) {
        draft_input_value = stored;
        draft_cursor =
            std::min(draft_cursor, static_cast<int>(draft_input_value.size()));
      }
    };
    option.transform = [this](InputState state) {
      if (state.is_placeholder && mode == RouteMode::SendEdit) {
        return text(" ") | focusCursorBarBlinking | color(theme::text);
      }
      return std::move(state.element) | color(theme::text);
    };
    draft_input = Input(&draft_input_value, "", std::move(option));
  }

  static void wake(void *context) noexcept {
    if (context != nullptr) {
      static_cast<ScreenInteractive *>(context)->PostEvent(Event::Custom);
    }
  }

  [[nodiscard]] RouteMode current_route_mode() const noexcept { return mode; }

  void sync_draft_from_application() {
    draft_input_value = application->snapshot().draft;
    const auto max_cursor =
        static_cast<std::size_t>(std::numeric_limits<int>::max());
    draft_cursor =
        static_cast<int>(std::min(draft_input_value.size(), max_cursor));
  }

  [[nodiscard]] bool transparent_background() const noexcept {
    return application->snapshot().config.ui.background ==
           config::UiBackground::Transparent;
  }

  [[nodiscard]] Element panel_background(Element element,
                                         const Color background) const {
    return transparent_background() ? std::move(element)
                                    : std::move(element) | bgcolor(background);
  }

  void copy_text(const std::string_view value, const std::string_view label) {
    constexpr std::size_t maximum = 1024U * 1024U;
    if (value.empty()) {
      return;
    }
    if (value.size() > maximum) {
      set_notice(std::string{label} + " exceeds the 1 MiB clipboard limit");
      return;
    }
    std::cout << "\x1b]52;c;" << base64_encode(value) << '\a' << std::flush;
    set_notice("Copied " + std::to_string(value.size()) + " byte(s) from " +
               std::string{label});
  }

  void copy_selection() { copy_text(selected_text, "the selection"); }

  [[nodiscard]] std::vector<std::size_t> receive_visible_indices() const {
    const auto &state = application->snapshot();
    std::vector<std::size_t> indices;
    indices.reserve(state.records.size());
    for (std::size_t index = 0U; index < state.records.size(); ++index) {
      if (visible(state.records[index], state.filter)) {
        indices.push_back(index);
      }
    }
    return indices;
  }

  void set_receive_cursor(const std::vector<std::size_t> &indices,
                          const std::size_t position) {
    if (indices.empty()) {
      receive_cursor.reset();
      return;
    }
    const auto selected = indices[std::min(position, indices.size() - 1U)];
    const auto &records = application->snapshot().records;
    receive_cursor = records[selected].record_id;
    const auto visible_position = static_cast<std::size_t>(
        std::distance(indices.begin(), std::ranges::find(indices, selected)));
    scroll_from_bottom = indices.size() - visible_position - 1U;
  }

  void normalize_receive_cursor(const bool prefer_latest = false) {
    const auto indices = receive_visible_indices();
    if (indices.empty()) {
      receive_cursor.reset();
      receive_vim_pending.clear();
      return;
    }
    const auto &records = application->snapshot().records;
    if (receive_cursor) {
      const auto retained =
          std::ranges::find_if(indices, [&](const auto index) {
            return records[index].record_id == *receive_cursor;
          });
      if (retained != indices.end()) {
        const auto position =
            static_cast<std::size_t>(std::distance(indices.begin(), retained));
        scroll_from_bottom = indices.size() - position - 1U;
        return;
      }
      const auto next = std::ranges::find_if(indices, [&](const auto index) {
        return records[index].record_id > *receive_cursor;
      });
      set_receive_cursor(indices, next == indices.end()
                                      ? indices.size() - 1U
                                      : static_cast<std::size_t>(std::distance(
                                            indices.begin(), next)));
      return;
    }
    if (prefer_latest) {
      set_receive_cursor(indices, indices.size() - 1U);
      return;
    }
    const auto end = indices.size() > scroll_from_bottom
                         ? indices.size() - scroll_from_bottom
                         : 0U;
    set_receive_cursor(indices, end == 0U ? 0U : end - 1U);
  }

  void move_receive_cursor(const bool down, const std::size_t count) {
    normalize_receive_cursor(true);
    const auto indices = receive_visible_indices();
    if (!receive_cursor || indices.empty()) {
      return;
    }
    const auto &records = application->snapshot().records;
    const auto current = std::ranges::find_if(indices, [&](const auto index) {
      return records[index].record_id == *receive_cursor;
    });
    if (current == indices.end()) {
      return;
    }
    const auto position =
        static_cast<std::size_t>(std::distance(indices.begin(), current));
    const auto target =
        down ? std::min(indices.size() - 1U,
                        position + std::min(count, indices.size()))
             : position - std::min(position, count);
    set_receive_cursor(indices, target);
  }

  void move_receive_edge(const bool latest) {
    const auto indices = receive_visible_indices();
    if (!indices.empty()) {
      set_receive_cursor(indices, latest ? indices.size() - 1U : 0U);
    }
  }

  void copy_receive_record() {
    if (!receive_cursor) {
      return;
    }
    const auto &records = application->snapshot().records;
    const auto current = std::ranges::find(records, *receive_cursor,
                                           &app::VisibleRecord::record_id);
    if (current != records.end()) {
      copy_text(application->render_record(*current), "the current record");
    }
  }

  [[nodiscard]] bool handle_receive_vim_key(const std::string_view key) {
    const bool command_key =
        key.size() == 1U &&
        (key.front() == 'j' || key.front() == 'k' || key.front() == 'g' ||
         key.front() == 'G' || key.front() == 'y' ||
         (key.front() >= '0' && key.front() <= '9'));
    if (receive_vim_pending.empty() && !command_key) {
      return false;
    }
    const auto command = receive_vim_pending + std::string{key};
    const auto parsed = parse_receive_vim_command(command);
    if (parsed.action == ReceiveVimAction::Pending) {
      receive_vim_pending = command.front() >= '1' && command.front() <= '9' &&
                                    parsed.count == 1'000'000U
                                ? std::to_string(parsed.count)
                                : command;
      return true;
    }
    receive_vim_pending.clear();
    switch (parsed.action) {
    case ReceiveVimAction::MoveDown:
      move_receive_cursor(true, parsed.count);
      break;
    case ReceiveVimAction::MoveUp:
      move_receive_cursor(false, parsed.count);
      break;
    case ReceiveVimAction::First:
      move_receive_edge(false);
      break;
    case ReceiveVimAction::Last:
      move_receive_edge(true);
      break;
    case ReceiveVimAction::YankLine:
      copy_receive_record();
      break;
    case ReceiveVimAction::Pending:
    case ReceiveVimAction::Invalid:
      break;
    }
    return true;
  }

  void set_notice(std::string message) { ui_notice = std::move(message); }

  void open_help() {
    if (mode == RouteMode::Help) {
      mode = mode_before_help;
      return;
    }
    mode_before_help = mode;
    mode = RouteMode::Help;
  }

  void show_alert_if_needed() {
    if (!application->snapshot().alert || mode == RouteMode::ErrorDialog ||
        mode == RouteMode::Help) {
      return;
    }
    alert_return_mode = mode;
    mode = RouteMode::ErrorDialog;
  }

  void dismiss_alert() {
    application->dismiss_alert();
    mode = alert_return_mode;
    if (mode == RouteMode::ErrorDialog) {
      mode = RouteMode::Normal;
    }
  }

  void open_modal(std::string kind, std::string title, std::string value) {
    modal_kind = std::move(kind);
    modal_title = std::move(title);
    edit_value = std::move(value);
    edit_cursor = edit_value.size();
    selected_device = 0U;
    modal_options.clear();
    modal_parent.clear();
    mode = RouteMode::Modal;
  }

  void open_choice(std::string kind, std::string title,
                   std::vector<std::string> options,
                   const std::size_t selected = 0U, std::string parent = {}) {
    modal_kind = std::move(kind);
    modal_title = std::move(title);
    modal_options = std::move(options);
    selected_device = modal_options.empty()
                          ? 0U
                          : std::min(selected, modal_options.size() - 1U);
    modal_parent = std::move(parent);
    edit_value.clear();
    edit_cursor = 0U;
    mode = RouteMode::Modal;
  }

  [[nodiscard]] std::string logging_value() const {
    return logging_candidate.directory + "|" +
           std::to_string(logging_candidate.max_files) + "|" +
           std::to_string(logging_candidate.max_total_size_mib) + "|" +
           std::to_string(logging_candidate.max_file_size_mib);
  }

  void open_data_root() {
    open_choice(
        "D", "Serial Settings",
        {"Data bits: " + std::to_string(serial_candidate.data_bits),
         "Stop bits: " + std::to_string(serial_candidate.stop_bits),
         "Parity: " + std::string{config::to_string(serial_candidate.parity)},
         "Flow: " +
             std::string{config::to_string(serial_candidate.flow_control)},
         "Apply"});
  }

  void open_view_root() {
    const auto setting = [](const bool enabled,
                            const config::ReceiveView view) {
      return enabled ? uppercase_ascii(config::to_string(view))
                     : std::string{"HIDDEN"};
    };
    open_choice(
        "V", "Receive View",
        {"RX: " + setting(filter_candidate.rx, rx_view_candidate),
         "TX: " + setting(filter_candidate.tx, tx_view_candidate),
         std::string{"SYS: "} + (filter_candidate.system ? "TXT" : "HIDDEN"),
         std::string{"ERR: "} + (filter_candidate.error ? "TXT" : "HIDDEN"),
         "Apply"});
  }

  void open_logging_root() {
    open_choice(
        "G", "Session Log Settings",
        {"Directory: " + (logging_candidate.directory.empty()
                              ? std::string{"<XDG default>"}
                              : logging_candidate.directory),
         "Maximum files: " + std::to_string(logging_candidate.max_files),
         "Maximum total size: " +
             std::to_string(logging_candidate.max_total_size_mib) + " MiB",
         "Maximum file size: " +
             std::to_string(logging_candidate.max_file_size_mib) + " MiB",
         "Save for Next Session", "Save and Rotate Now", "Cancel"});
  }

  void close_overlay() {
    mode = RouteMode::Normal;
    application->set_interaction(app::InteractionState::Normal);
    confirm_action = ConfirmAction::None;
    awaiting_quick_save = false;
    connect_after_port = false;
  }

  void open_confirm(const ConfirmAction action, const RouteMode return_mode) {
    confirm_action = action;
    confirm_return_mode = return_mode;
    confirm_accept = false;
    mode = RouteMode::Confirm;
  }

  void cancel_confirm() {
    mode = confirm_return_mode;
    confirm_action = ConfirmAction::None;
    confirm_accept = false;
    if (mode == RouteMode::Normal) {
      application->set_interaction(app::InteractionState::Normal);
    }
  }

  [[nodiscard]] static bool parse_quick_request(const std::string_view value,
                                                std::uint32_t &slot,
                                                std::uint64_t &interval) {
    const auto separator = value.find('|');
    if (separator == std::string_view::npos) {
      return false;
    }
    const auto first =
        std::from_chars(value.data(), value.data() + separator, slot);
    const auto second = std::from_chars(value.data() + separator + 1U,
                                        value.data() + value.size(), interval);
    return first.ec == std::errc{} && first.ptr == value.data() + separator &&
           second.ec == std::errc{} &&
           second.ptr == value.data() + value.size();
  }

  [[nodiscard]] static std::optional<std::uint32_t>
  quick_slot_from_editor(const std::string_view value) {
    const auto separator = value.find('|');
    const auto field = value.substr(0U, separator);
    std::uint32_t slot{};
    const auto parsed =
        std::from_chars(field.data(), field.data() + field.size(), slot);
    if (parsed.ec != std::errc{} || parsed.ptr != field.data() + field.size() ||
        slot < 1U || slot > 20U) {
      return std::nullopt;
    }
    return slot;
  }

  [[nodiscard]] app::DirectionFilter search_direction_filter() const noexcept {
    app::DirectionFilter filter{false, false, false, false};
    switch (search_direction) {
    case SearchDirection::All:
      return {true, true, true, true};
    case SearchDirection::Rx:
      filter.rx = true;
      break;
    case SearchDirection::Tx:
      filter.tx = true;
      break;
    case SearchDirection::System:
      filter.system = true;
      break;
    case SearchDirection::Error:
      filter.error = true;
      break;
    }
    return filter;
  }

  [[nodiscard]] std::string_view search_direction_text() const noexcept {
    switch (search_direction) {
    case SearchDirection::All:
      return "All";
    case SearchDirection::Rx:
      return "RX";
    case SearchDirection::Tx:
      return "TX";
    case SearchDirection::System:
      return "SYS";
    case SearchDirection::Error:
      return "ERR";
    }
    return "All";
  }

  void run_search() {
    search_matches =
        application->search(search_query, search_direction_filter());
    search_match = 0U;
    if (!search_matches.empty()) {
      static_cast<void>(scroll_to_search_match());
    }
  }

  [[nodiscard]] bool scroll_to_search_match() {
    if (search_match >= search_matches.size()) {
      return false;
    }
    const auto &records = application->snapshot().records;
    const auto found = std::ranges::find(records, search_matches[search_match],
                                         &app::VisibleRecord::record_id);
    if (found == records.end()) {
      return false;
    }
    const auto filter = search_direction_filter();
    scroll_from_bottom = static_cast<std::size_t>(std::ranges::count_if(
        std::ranges::subrange(std::next(found), records.end()),
        [&filter](const auto &record) { return visible(record, filter); }));
    return true;
  }

  void prune_search_matches() {
    if (search_matches.empty()) {
      return;
    }
    const auto &records = application->snapshot().records;
    if (records.empty()) {
      search_matches.clear();
      search_match = 0U;
      return;
    }
    const auto selected =
        search_matches[std::min(search_match, search_matches.size() - 1U)];
    const auto first = records.front().record_id;
    const auto last = records.back().record_id;
    std::erase_if(search_matches, [first, last](const std::uint64_t sequence) {
      return sequence < first || sequence > last;
    });
    if (search_matches.empty()) {
      search_match = 0U;
      return;
    }
    const auto retained = std::ranges::find(search_matches, selected);
    if (retained == search_matches.end()) {
      search_match = 0U;
      static_cast<void>(scroll_to_search_match());
    } else {
      search_match = static_cast<std::size_t>(
          std::distance(search_matches.begin(), retained));
    }
  }

  void cycle_search_direction(const bool forward) {
    constexpr auto count = static_cast<std::uint8_t>(SearchDirection::Error) +
                           static_cast<std::uint8_t>(1U);
    const auto current = static_cast<std::uint8_t>(search_direction);
    const auto next =
        forward ? static_cast<std::uint8_t>((current + 1U) % count)
                : static_cast<std::uint8_t>((current + count - 1U) % count);
    search_direction = static_cast<SearchDirection>(next);
    if (!search_query.empty()) {
      run_search();
    }
  }

  void open_configuration(RoutedAction action) {
    const auto &state = application->snapshot();
    switch (action) {
    case RoutedAction::Port: {
      application->request_scan();
      open_choice("P", "Serial Device (F5 refresh)", {"Scanning..."});
      break;
    }
    case RoutedAction::Baud: {
      std::vector<std::string> options;
      options.reserve(config::kBaudPresets.size());
      std::size_t selected = 0U;
      for (std::size_t index = 0U; index < config::kBaudPresets.size();
           ++index) {
        options.push_back(std::to_string(config::kBaudPresets[index]));
        if (config::kBaudPresets[index] == state.config.serial.baud) {
          selected = index;
        }
      }
      open_choice("B", "Baud Rate", std::move(options), selected);
      break;
    }
    case RoutedAction::DataFormat:
      serial_candidate = state.config.serial;
      open_data_root();
      break;
    case RoutedAction::Newline:
      open_choice("N", "NewLine Suffix",
                  {"None", "LF  (0A)", "CR  (0D)", "CRLF  (0D 0A)"},
                  static_cast<std::size_t>(state.config.send.newline));
      break;
    case RoutedAction::View:
      rx_view_candidate = state.config.receive.rx_view;
      tx_view_candidate = state.config.receive.tx_view;
      filter_candidate = state.filter;
      open_view_root();
      break;
    case RoutedAction::SendMode:
      open_choice("H", "Input Type", {"TXT", "HEX"},
                  state.config.send.mode == config::SendMode::Txt ? 0U : 1U);
      break;
    case RoutedAction::LoggingConfig:
      logging_candidate = state.config.logging;
      if (logging_candidate.directory.empty()) {
        logging_candidate.directory = state.effective_log_directory;
      }
      open_logging_root();
      break;
    case RoutedAction::QuickConfig: {
      const auto slot_index = state.preferences.last_quick_send_slot;
      const auto &slot = state.quick_send.slots[slot_index - 1U];
      if (slot) {
        open_modal("F", "F  index|name|mode|content|newline|note",
                   std::to_string(slot_index) + "|" + slot->name + "|" +
                       std::string{config::to_string(slot->mode)} + "|" +
                       slot->content + "|" +
                       std::string{config::to_string(slot->newline)} + "|" +
                       slot->note);
      } else {
        open_modal("F", "F  index|name|mode|content|newline|note",
                   std::to_string(slot_index) + "||txt||session|");
      }
      break;
    }
    case RoutedAction::QuickExecute:
      open_modal("f",
                 state.task.state == scheduler::SchedulerState::Idle
                     ? "f  slot|interval_ms (0 or 10..86400000)"
                     : "f  slot|interval_ms to replace; 'stop' stops the task",
                 std::to_string(state.preferences.last_quick_send_slot) + "|" +
                     std::to_string(state.preferences.last_interval_ms));
      break;
    case RoutedAction::CommandPalette:
      open_modal("E", "E  Command: scan/connect/disconnect/help/stop-task", "");
      break;
    default:
      break;
    }
  }

  void apply_modal() {
    Status result;
    if (modal_kind == "P") {
      const auto &devices = application->snapshot().devices;
      if (application->snapshot().scanning) {
        result = tl::unexpected(make_error(
            ErrorCode::ValidationInvalidValue, Operation::ValidateConfig,
            "wait for the current device scan to finish"));
      } else if (devices.empty() || selected_device >= devices.size()) {
        result = tl::unexpected(make_error(
            ErrorCode::ValidationInvalidValue, Operation::ValidateConfig,
            "no scanned serial device selected"));
      } else if (devices[selected_device].permission.access ==
                 serial::DeviceAccess::PermissionDenied) {
        application->publish_permission_alert(
            devices[selected_device].path, devices[selected_device].permission);
        show_alert_if_needed();
        return;
      } else {
        result = application->apply_port(devices[selected_device].path);
      }
    } else if (modal_kind == "B") {
      result = application->apply_baud(modal_options[selected_device]);
    } else if (modal_kind == "D") {
      if (selected_device == 0U) {
        open_choice("D-data", "Data Bits", {"5", "6", "7", "8"},
                    static_cast<std::size_t>(serial_candidate.data_bits - 5),
                    "D");
        return;
      }
      if (selected_device == 1U) {
        open_choice("D-stop", "Stop Bits", {"1", "2"},
                    static_cast<std::size_t>(serial_candidate.stop_bits - 1),
                    "D");
        return;
      }
      if (selected_device == 2U) {
        open_choice("D-parity", "Parity",
                    {"None", "Odd", "Even", "Mark", "Space"},
                    static_cast<std::size_t>(serial_candidate.parity), "D");
        return;
      }
      if (selected_device == 3U) {
        open_choice("D-flow", "Flow Control", {"None", "RTS/CTS", "XON/XOFF"},
                    static_cast<std::size_t>(serial_candidate.flow_control),
                    "D");
        return;
      }
      result = application->apply_data_format(
          std::to_string(serial_candidate.data_bits) + "," +
          std::to_string(serial_candidate.stop_bits) + "," +
          std::string{config::to_string(serial_candidate.parity)} + "," +
          std::string{config::to_string(serial_candidate.flow_control)});
    } else if (modal_kind == "D-data") {
      serial_candidate.data_bits =
          static_cast<std::int32_t>(selected_device + 5U);
      open_data_root();
      return;
    } else if (modal_kind == "D-stop") {
      serial_candidate.stop_bits =
          static_cast<std::int32_t>(selected_device + 1U);
      open_data_root();
      return;
    } else if (modal_kind == "D-parity") {
      serial_candidate.parity = static_cast<config::Parity>(selected_device);
      open_data_root();
      return;
    } else if (modal_kind == "D-flow") {
      serial_candidate.flow_control =
          static_cast<config::FlowControl>(selected_device);
      open_data_root();
      return;
    } else if (modal_kind == "N") {
      static constexpr std::array<std::string_view, 4> values{"none", "lf",
                                                              "cr", "crlf"};
      result = application->apply_newline(values[selected_device]);
    } else if (modal_kind == "V") {
      if (selected_device == 0U) {
        open_choice("V-rx", "RX View", {"Hidden", "TXT", "HEX", "MIXED"},
                    filter_candidate.rx
                        ? static_cast<std::size_t>(rx_view_candidate) + 1U
                        : 0U,
                    "V");
        return;
      }
      if (selected_device == 1U) {
        open_choice("V-tx", "TX View", {"Hidden", "TXT", "HEX", "MIXED"},
                    filter_candidate.tx
                        ? static_cast<std::size_t>(tx_view_candidate) + 1U
                        : 0U,
                    "V");
        return;
      }
      if (selected_device == 2U) {
        open_choice("V-sys", "SYS Records", {"Hidden", "TXT"},
                    filter_candidate.system ? 1U : 0U, "V");
        return;
      }
      if (selected_device == 3U) {
        open_choice("V-err", "ERR Records", {"Hidden", "TXT"},
                    filter_candidate.error ? 1U : 0U, "V");
        return;
      }
      result = application->apply_view(
          std::string{config::to_string(rx_view_candidate)} + "," +
          std::string{config::to_string(tx_view_candidate)} + "," +
          (filter_candidate.rx ? "1" : "0") + "," +
          (filter_candidate.tx ? "1" : "0") + "," +
          (filter_candidate.system ? "1" : "0") + "," +
          (filter_candidate.error ? "1" : "0"));
    } else if (modal_kind == "V-rx" || modal_kind == "V-tx") {
      const bool enabled = selected_device != 0U;
      if (modal_kind == "V-rx") {
        filter_candidate.rx = enabled;
        if (enabled) {
          rx_view_candidate =
              static_cast<config::ReceiveView>(selected_device - 1U);
        }
      } else {
        filter_candidate.tx = enabled;
        if (enabled) {
          tx_view_candidate =
              static_cast<config::ReceiveView>(selected_device - 1U);
        }
      }
      open_view_root();
      return;
    } else if (modal_kind == "V-sys" || modal_kind == "V-err") {
      if (modal_kind == "V-sys") {
        filter_candidate.system = selected_device != 0U;
      } else {
        filter_candidate.error = selected_device != 0U;
      }
      open_view_root();
      return;
    } else if (modal_kind == "H") {
      result =
          application->apply_send_mode(selected_device == 0U ? "txt" : "hex");
    } else if (modal_kind == "G") {
      if (selected_device <= 3U) {
        static constexpr std::array<std::string_view, 4> kinds{
            "G-dir", "G-files", "G-total", "G-file"};
        static constexpr std::array<std::string_view, 4> titles{
            "Log Directory", "Maximum Files", "Maximum Total Size (MiB)",
            "Maximum File Size (MiB)"};
        const std::array<std::string, 4> values{
            logging_candidate.directory,
            std::to_string(logging_candidate.max_files),
            std::to_string(logging_candidate.max_total_size_mib),
            std::to_string(logging_candidate.max_file_size_mib)};
        open_modal(std::string{kinds[selected_device]},
                   std::string{titles[selected_device]},
                   values[selected_device]);
        modal_parent = "G";
        return;
      }
      if (selected_device == 6U) {
        close_overlay();
        return;
      }
      result = application->apply_logging(logging_value(),
                                          selected_device == 4U
                                              ? app::LogApplyPolicy::NextSession
                                              : app::LogApplyPolicy::RotateNow);
    } else if (modal_kind == "G-dir" || modal_kind == "G-files" ||
               modal_kind == "G-total" || modal_kind == "G-file") {
      auto candidate = logging_candidate;
      if (modal_kind == "G-dir") {
        candidate.directory = edit_value;
      } else {
        std::uint32_t parsed{};
        const auto result_parse = std::from_chars(
            edit_value.data(), edit_value.data() + edit_value.size(), parsed);
        if (result_parse.ec != std::errc{} ||
            result_parse.ptr != edit_value.data() + edit_value.size()) {
          result = tl::unexpected(make_error(
              ErrorCode::ValidationInvalidValue, Operation::ValidateConfig,
              "the logging value must be an unsigned integer"));
        } else if (modal_kind == "G-files") {
          candidate.max_files = parsed;
        } else if (modal_kind == "G-total") {
          candidate.max_total_size_mib = parsed;
        } else {
          candidate.max_file_size_mib = parsed;
        }
      }
      if (result) {
        const auto value = candidate.directory + "|" +
                           std::to_string(candidate.max_files) + "|" +
                           std::to_string(candidate.max_total_size_mib) + "|" +
                           std::to_string(candidate.max_file_size_mib);
        result = application->validate_logging(value);
      }
      if (result) {
        logging_candidate = std::move(candidate);
        open_logging_root();
        return;
      }
    } else if (modal_kind == "F") {
      result = application->apply_quick_slot(edit_value);
    } else if (modal_kind == "f") {
      if (edit_value == "stop") {
        application->stop_quick_task();
        close_overlay();
        return;
      }
      std::uint32_t slot{};
      std::uint64_t interval{};
      if (!parse_quick_request(edit_value, slot, interval)) {
        result = tl::unexpected(make_error(
            ErrorCode::ValidationInvalidValue, Operation::ValidateConfig,
            "quick execution format is slot|interval_ms"));
      } else {
        const auto task_state = application->snapshot().task.state;
        if (task_state == scheduler::SchedulerState::Running) {
          pending_quick_slot = slot;
          pending_quick_interval = interval;
          open_confirm(ConfirmAction::ReplaceQuickTask, RouteMode::Modal);
          return;
        }
        result = application->execute_quick(slot, interval);
      }
    } else if (modal_kind == "E") {
      if (edit_value == "scan") {
        application->request_scan();
      } else if (edit_value == "connect" || edit_value == "disconnect") {
        application->connection_control();
      } else if (edit_value == "help") {
        close_overlay();
        open_help();
        return;
      } else if (edit_value == "stop-task") {
        application->stop_quick_task();
      } else {
        result = tl::unexpected(make_error(ErrorCode::ValidationInvalidValue,
                                           Operation::ValidateConfig,
                                           "unknown command"));
      }
    }
    if (!result) {
      if (!application->snapshot().alert) {
        application->publish_alert({result.error().code,
                                    "Invalid Configuration",
                                    status_error(result.error())});
      }
      show_alert_if_needed();
      return;
    }
    if (modal_kind == "F") {
      awaiting_quick_save = true;
      modal_title = "F  Saving quick-send slot...";
      return;
    }
    const bool should_connect = modal_kind == "P" && connect_after_port;
    connect_after_port = false;
    close_overlay();
    if (should_connect) {
      application->connection_control();
    }
  }

  void edit_text(const Event &event) {
    std::string value = edit_value;
    edit_cursor = std::min(edit_cursor, value.size());
    if (event == Event::Backspace) {
      if (edit_cursor != 0U) {
        value.erase(edit_cursor - 1U, 1U);
        --edit_cursor;
      }
    } else if (event == Event::Delete) {
      if (edit_cursor < value.size()) {
        value.erase(edit_cursor, 1U);
      }
    } else if (event == Event::ArrowLeft) {
      if (edit_cursor != 0U) {
        --edit_cursor;
      }
    } else if (event == Event::ArrowRight) {
      if (edit_cursor < value.size()) {
        ++edit_cursor;
      }
    } else if (event.is_character()) {
      const auto text = event.character();
      value.insert(edit_cursor, text);
      edit_cursor += text.size();
    }
    edit_value = std::move(value);
  }

  void scroll(RoutedAction action) {
    const auto count = mode == RouteMode::Search
                           ? application->snapshot().records.size()
                           : receive_visible_indices().size();
    switch (action) {
    case RoutedAction::ScrollUp:
      scroll_from_bottom = std::min(count, scroll_from_bottom + 1U);
      break;
    case RoutedAction::ScrollDown:
      if (scroll_from_bottom != 0U) {
        --scroll_from_bottom;
      }
      break;
    case RoutedAction::PageUp:
      scroll_from_bottom = std::min(count, scroll_from_bottom + 10U);
      break;
    case RoutedAction::PageDown:
      scroll_from_bottom =
          scroll_from_bottom > 10U ? scroll_from_bottom - 10U : 0U;
      break;
    case RoutedAction::Home:
      scroll_from_bottom = count;
      break;
    case RoutedAction::End:
      scroll_from_bottom = 0U;
      break;
    default:
      break;
    }
  }

  bool handle(Event event) {
    if (event == Event::Custom) {
      const auto old_count = receive_visible_indices().size();
      application->tick();
      show_alert_if_needed();
      if (mode == RouteMode::Modal && modal_kind == "P") {
        const auto &devices = application->snapshot().devices;
        modal_options.clear();
        if (application->snapshot().scanning) {
          modal_options.push_back("Scanning...");
        } else {
          for (const auto &device : devices) {
            modal_options.push_back(
                device.path + "  " + device.description +
                (device.permission.access ==
                         serial::DeviceAccess::PermissionDenied
                     ? "  [Permission denied]"
                     : ""));
          }
          if (modal_options.empty()) {
            modal_options.push_back("No scanned serial devices");
          }
        }
        selected_device = std::min(selected_device, modal_options.size() - 1U);
      }
      if (mode == RouteMode::Search) {
        prune_search_matches();
      }
      if (awaiting_quick_save &&
          !application->snapshot().quick_send_save_pending) {
        awaiting_quick_save = false;
        if (application->snapshot().quick_send_save_failed) {
          modal_title = "F  Save failed; edit is preserved";
        } else {
          close_overlay();
        }
      }
      const auto new_count = receive_visible_indices().size();
      if (mode == RouteMode::ReceiveBrowse) {
        normalize_receive_cursor();
      } else if (new_count > old_count &&
                 (scroll_from_bottom != 0U ||
                  application->snapshot().manual_pause)) {
        scroll_from_bottom =
            std::min(new_count, scroll_from_bottom + (new_count - old_count));
      }
      scroll_from_bottom = std::min(scroll_from_bottom, new_count);
      return false;
    }
    if (event == Event::CtrlC) {
      if (mode == RouteMode::ReceiveBrowse && !receive_vim_pending.empty()) {
        receive_vim_pending.clear();
        return true;
      }
      if (mode == RouteMode::Normal || mode == RouteMode::ReceiveBrowse) {
        copy_selection();
      }
      return true;
    }
    if (event.is_mouse() &&
        (mode == RouteMode::Normal || mode == RouteMode::ReceiveBrowse)) {
      if (mode == RouteMode::ReceiveBrowse && !receive_vim_pending.empty()) {
        receive_vim_pending.clear();
        return true;
      }
      if (event.mouse().button == Mouse::WheelUp) {
        if (mode == RouteMode::ReceiveBrowse) {
          move_receive_cursor(false, 1U);
          return true;
        }
        scroll(RoutedAction::ScrollUp);
        return false;
      }
      if (event.mouse().button == Mouse::WheelDown) {
        if (mode == RouteMode::ReceiveBrowse) {
          move_receive_cursor(true, 1U);
          return true;
        }
        scroll(RoutedAction::ScrollDown);
        return false;
      }
    }
    const auto key = event_key(event);
    const auto connected =
        application->snapshot().connection == app::ConnectionState::Connected;
    const auto action = route_key(
        {mode, key, connected, mode == RouteMode::Modal && modal_kind == "P",
         mode == RouteMode::Search && search_focus == SearchFocus::Direction});
    if (action == RoutedAction::Help) {
      open_help();
      return true;
    }
    if (mode == RouteMode::Help) {
      if (action == RoutedAction::Escape) {
        open_help();
      }
      return true;
    }
    if (mode == RouteMode::ErrorDialog) {
      if (action == RoutedAction::Escape) {
        dismiss_alert();
      }
      return true;
    }
    if (mode == RouteMode::Confirm) {
      if (action == RoutedAction::Escape) {
        cancel_confirm();
      } else if (action == RoutedAction::FocusNext ||
                 action == RoutedAction::FocusPrevious) {
        confirm_accept = !confirm_accept;
      } else if (action == RoutedAction::SelectNext) {
        confirm_accept = true;
      } else if (action == RoutedAction::SelectPrevious) {
        confirm_accept = false;
      } else if (action == RoutedAction::Apply) {
        if (!confirm_accept) {
          cancel_confirm();
        } else if (confirm_action == ConfirmAction::Clear) {
          application->clear_records();
          close_overlay();
        } else if (confirm_action == ConfirmAction::Quit) {
          exit_ok = application->shutdown();
          screen.Exit();
        } else if (confirm_action == ConfirmAction::DeleteQuickSlot) {
          const auto value = std::to_string(pending_quick_slot) + "|||||";
          const auto result = application->apply_quick_slot(value);
          if (!result) {
            mode = RouteMode::Modal;
            confirm_action = ConfirmAction::None;
            edit_value = status_error(result.error());
            edit_cursor = edit_value.size();
          } else {
            mode = RouteMode::Modal;
            confirm_action = ConfirmAction::None;
            confirm_accept = false;
            awaiting_quick_save = true;
            modal_title = "F  Deleting quick-send slot...";
          }
        } else if (confirm_action == ConfirmAction::ReplaceQuickTask) {
          const auto result = application->execute_quick(
              pending_quick_slot, pending_quick_interval, true);
          if (!result) {
            mode = RouteMode::Modal;
            confirm_action = ConfirmAction::None;
            edit_value = status_error(result.error());
            edit_cursor = edit_value.size();
          } else {
            close_overlay();
          }
        }
      }
      return true;
    }
    if (mode == RouteMode::Modal) {
      if (awaiting_quick_save) {
        return true;
      }
      if (action == RoutedAction::Escape) {
        if (modal_parent == "D") {
          open_data_root();
        } else if (modal_parent == "V") {
          open_view_root();
        } else if (modal_parent == "G") {
          open_logging_root();
        } else {
          close_overlay();
        }
      } else if (action == RoutedAction::Scan) {
        application->request_scan();
        modal_options = {"Scanning..."};
        selected_device = 0U;
      } else if (action == RoutedAction::Apply) {
        apply_modal();
      } else if (!modal_options.empty() &&
                 (action == RoutedAction::FocusNext ||
                  action == RoutedAction::FocusPrevious)) {
        if (action == RoutedAction::FocusPrevious && selected_device != 0U) {
          --selected_device;
        } else if (action == RoutedAction::FocusNext &&
                   selected_device + 1U < modal_options.size()) {
          ++selected_device;
        }
      } else if (modal_kind == "F" && event == Event::Delete) {
        const auto slot = quick_slot_from_editor(edit_value);
        if (!slot) {
          edit_value = "Select a valid slot number before deleting";
          edit_cursor = edit_value.size();
        } else if (!application->snapshot().quick_send.slots[*slot - 1U]) {
          edit_value = "The selected quick-send slot is already empty";
          edit_cursor = edit_value.size();
        } else {
          pending_quick_slot = *slot;
          open_confirm(ConfirmAction::DeleteQuickSlot, RouteMode::Modal);
        }
      } else if (!modal_options.empty() &&
                 (event == Event::ArrowUp || event == Event::ArrowDown)) {
        if (event == Event::ArrowUp && selected_device != 0U) {
          --selected_device;
        } else if (event == Event::ArrowDown &&
                   selected_device + 1U < modal_options.size()) {
          ++selected_device;
        }
      } else if (modal_options.empty()) {
        edit_text(event);
      }
      return true;
    }
    if (mode == RouteMode::Search) {
      if (action == RoutedAction::Escape) {
        close_overlay();
      } else if (action == RoutedAction::End) {
        scroll_from_bottom = 0U;
        close_overlay();
      } else if (action == RoutedAction::Apply) {
        run_search();
      } else if (action == RoutedAction::FocusNext ||
                 action == RoutedAction::FocusPrevious) {
        search_focus = search_focus == SearchFocus::Query
                           ? SearchFocus::Direction
                           : SearchFocus::Query;
      } else if (action == RoutedAction::SelectNext ||
                 action == RoutedAction::SelectPrevious) {
        cycle_search_direction(action == RoutedAction::SelectNext);
      } else if (!search_matches.empty() &&
                 (action == RoutedAction::SearchNext ||
                  action == RoutedAction::SearchPrevious)) {
        if (action == RoutedAction::SearchNext) {
          search_match = (search_match + 1U) % search_matches.size();
        } else {
          search_match = search_match == 0U ? search_matches.size() - 1U
                                            : search_match - 1U;
        }
        if (!scroll_to_search_match()) {
          prune_search_matches();
          static_cast<void>(scroll_to_search_match());
        }
      } else if (search_focus == SearchFocus::Query) {
        edit_value = search_query;
        edit_text(event);
        search_query = edit_value;
      }
      return true;
    }
    if (mode == RouteMode::SendEdit) {
      if (action == RoutedAction::Escape) {
        close_overlay();
      } else if (action == RoutedAction::Submit) {
        application->submit_draft();
        sync_draft_from_application();
      } else if (action == RoutedAction::InsertNewline) {
        const auto cursor = static_cast<std::size_t>(std::max(draft_cursor, 0));
        const auto position = std::min(cursor, draft_input_value.size());
        draft_input_value.insert(position, "\n");
        draft_cursor = static_cast<int>(position + 1U);
        application->set_draft(draft_input_value);
        draft_input_value = application->snapshot().draft;
        draft_cursor =
            std::min(draft_cursor, static_cast<int>(draft_input_value.size()));
      } else if (action == RoutedAction::HistoryPrevious) {
        application->history_previous();
        sync_draft_from_application();
      } else if (action == RoutedAction::HistoryNext) {
        application->history_next();
        sync_draft_from_application();
      } else {
        return false;
      }
      return true;
    }
    if (mode == RouteMode::ReceiveBrowse) {
      if (action == RoutedAction::Escape) {
        receive_vim_pending.clear();
      } else if (event.is_character() && handle_receive_vim_key(key)) {
        return true;
      } else if (!receive_vim_pending.empty()) {
        receive_vim_pending.clear();
        return true;
      }
      if (action == RoutedAction::ScrollUp) {
        move_receive_cursor(false, 1U);
        return true;
      }
      if (action == RoutedAction::ScrollDown) {
        move_receive_cursor(true, 1U);
        return true;
      }
      if (action == RoutedAction::PageUp) {
        move_receive_cursor(false, 10U);
        return true;
      }
      if (action == RoutedAction::PageDown) {
        move_receive_cursor(true, 10U);
        return true;
      }
      if (action == RoutedAction::Home) {
        move_receive_edge(false);
        return true;
      }
      if (action == RoutedAction::End) {
        move_receive_edge(true);
        return true;
      }
    }

    switch (action) {
    case RoutedAction::Quit:
      if (application->has_exit_risk()) {
        open_confirm(ConfirmAction::Quit, RouteMode::Normal);
      } else {
        exit_ok = application->shutdown();
        screen.Exit();
      }
      break;
    case RoutedAction::Connection:
      if (application->snapshot().connection ==
              app::ConnectionState::Disconnected &&
          application->snapshot().device_path.empty()) {
        connect_after_port = true;
        open_configuration(RoutedAction::Port);
      } else {
        application->connection_control();
      }
      break;
    case RoutedAction::Browse:
      mode = RouteMode::ReceiveBrowse;
      application->set_interaction(app::InteractionState::ReceiveBrowse);
      receive_vim_pending.clear();
      normalize_receive_cursor();
      break;
    case RoutedAction::Edit:
      mode = RouteMode::SendEdit;
      application->set_interaction(app::InteractionState::SendEdit);
      sync_draft_from_application();
      draft_input->TakeFocus();
      break;
    case RoutedAction::Clear:
      open_confirm(ConfirmAction::Clear, RouteMode::Normal);
      break;
    case RoutedAction::Pause:
      application->toggle_pause();
      break;
    case RoutedAction::Search:
      mode = RouteMode::Search;
      search_query.clear();
      search_matches.clear();
      search_focus = SearchFocus::Query;
      search_direction = SearchDirection::All;
      edit_cursor = 0U;
      break;
    case RoutedAction::Scan:
      application->request_scan();
      break;
    case RoutedAction::ToggleLog:
      application->toggle_log();
      break;
    case RoutedAction::CopySelection:
      copy_selection();
      break;
    case RoutedAction::Redraw:
      screen.PostEvent(Event::Custom);
      break;
    case RoutedAction::ScrollUp:
    case RoutedAction::ScrollDown:
    case RoutedAction::PageUp:
    case RoutedAction::PageDown:
    case RoutedAction::Home:
    case RoutedAction::End:
      scroll(action);
      return false;
    case RoutedAction::Port:
    case RoutedAction::Baud:
    case RoutedAction::DataFormat:
    case RoutedAction::Newline:
    case RoutedAction::View:
    case RoutedAction::SendMode:
    case RoutedAction::LoggingConfig:
    case RoutedAction::QuickConfig:
    case RoutedAction::QuickExecute:
    case RoutedAction::CommandPalette:
      open_configuration(action);
      break;
    case RoutedAction::Escape:
      close_overlay();
      break;
    default:
      break;
    }
    show_alert_if_needed();
    if (event.is_mouse() &&
        (mode == RouteMode::Normal || mode == RouteMode::ReceiveBrowse)) {
      return false;
    }
    return true;
  }

  [[nodiscard]] Element records_element() const {
    const auto &state = application->snapshot();
    Elements rows;
    if (state.display_gap_records != 0U) {
      rows.push_back(
          text("... GAP: " + std::to_string(state.display_gap_records) +
               " evicted record(s) ...") |
          color(theme::gold));
    }
    std::vector<std::size_t> indices;
    if (mode == RouteMode::Search) {
      const auto filter = search_direction_filter();
      for (std::size_t index = 0U; index < state.records.size(); ++index) {
        if (visible(state.records[index], filter)) {
          indices.push_back(index);
        }
      }
    } else {
      indices = receive_visible_indices();
    }
    std::size_t end = indices.size() > scroll_from_bottom
                          ? indices.size() - scroll_from_bottom
                          : 0U;
    std::size_t begin = end > 200U ? end - 200U : 0U;
    if (mode == RouteMode::ReceiveBrowse && receive_cursor) {
      const auto current = std::ranges::find_if(indices, [&](const auto index) {
        return state.records[index].record_id == *receive_cursor;
      });
      if (current != indices.end()) {
        const auto position =
            static_cast<std::size_t>(std::distance(indices.begin(), current));
        if (position < begin || position >= end) {
          end = position + 1U;
          begin = end > 200U ? end - 200U : 0U;
        }
      }
    }
    for (std::size_t position = begin; position < end; ++position) {
      const auto index = indices[position];
      const auto &record = state.records[index];
      const bool current = mode == RouteMode::ReceiveBrowse && receive_cursor &&
                           record.record_id == *receive_cursor;
      auto row = text((mode == RouteMode::ReceiveBrowse
                           ? std::string{current ? "> " : "  "}
                           : std::string{}) +
                      application->render_record(record));
      if (record.direction == app::RecordDirection::Rx) {
        row |= color(theme::foam);
      } else if (record.direction == app::RecordDirection::Tx) {
        row |= color(theme::rose);
      } else if (record.direction == app::RecordDirection::Error) {
        row |= color(theme::love);
      } else {
        row |= color(theme::gold);
      }
      if (current) {
        row |= bold;
        row |= inverted;
        row |= focus;
      }
      rows.push_back(std::move(row));
    }
    if (rows.empty()) {
      rows.push_back(text(mode == RouteMode::ReceiveBrowse
                              ? "> [No visible records; cursor is empty]"
                              : "No session records. F5 scans, P selects a "
                                "port, C connects.") |
                     color(theme::muted));
    }
    return vbox(std::move(rows)) | yframe | flex;
  }

  [[nodiscard]] Element receive_element() const {
    const bool focused = mode == RouteMode::ReceiveBrowse;
    Elements title;
    if (focused) {
      title.push_back(text(" Receive [Browse] ") | color(theme::iris) | bold);
      title.push_back(shortcut("j/k"));
      title.push_back(text(" Move ") | color(theme::subtle));
      title.push_back(shortcut("gg/G"));
      title.push_back(text(" Edge ") | color(theme::subtle));
      title.push_back(shortcut("yy"));
      title.push_back(text(" Copy ") | color(theme::subtle));
      title.push_back(shortcut("Esc"));
      title.push_back(text(" Normal ") | color(theme::subtle));
    } else {
      title.push_back(shortcut("R"));
      title.push_back(text(" Receive ") | color(theme::subtle));
    }
    auto panel = panel_background(
        window(hbox(std::move(title)), records_element()), theme::surface);
    panel |= color(focused ? theme::iris : theme::subtle);
    return panel | flex;
  }

  [[nodiscard]] Element link_element(const app::ConnectionState state) const {
    auto value = text(connection_text(state));
    switch (state) {
    case app::ConnectionState::Disconnected:
      value |= color(theme::love);
      break;
    case app::ConnectionState::Connecting:
    case app::ConnectionState::Disconnecting:
      value |= color(theme::gold);
      break;
    case app::ConnectionState::Connected:
      value |= color(theme::success);
      break;
    case app::ConnectionState::Error:
      value |= color(theme::love) | bold | inverted;
      break;
    }
    return hbox({text("Link") | color(theme::subtle), shortcut("C"),
                 text(":") | color(theme::subtle), std::move(value)});
  }

  [[nodiscard]] Element log_element(const app::LogState state) const {
    Color value_color = theme::muted;
    if (state == app::LogState::Waiting) {
      value_color = theme::gold;
    } else if (state == app::LogState::Recording) {
      value_color = theme::success;
    } else if (state == app::LogState::Error) {
      value_color = theme::love;
    }
    auto field = status_field("Log", "g/G", log_text(state), value_color);
    if (state == app::LogState::Error) {
      field |= bold;
    }
    return field;
  }

  [[nodiscard]] Element status_element() const {
    const auto &state = application->snapshot();
    const auto terminal_width = std::max(Terminal::Size().dimx, 0);
    const auto port = state.device_path.empty()
                          ? std::string{"Unset"}
                          : abbreviate_left(state.device_path, 18U);
    const auto port_config = state.active_port_config.value_or(
        serial::PortConfig::from_defaults(state.config.serial));
    const auto serial_value = std::to_string(port_config.data_bits) +
                              parity_letter(port_config.parity) +
                              std::to_string(port_config.stop_bits) +
                              " Flow:" + flow_text(port_config.flow_control);
    std::string visible_directions;
    const auto add_direction =
        [&visible_directions](const std::string_view name, const bool enabled) {
          if (!enabled) {
            return;
          }
          if (!visible_directions.empty()) {
            visible_directions += "+";
          }
          visible_directions += name;
        };
    add_direction("RX", state.filter.rx);
    add_direction("TX", state.filter.tx);
    add_direction("SYS", state.filter.system);
    add_direction("ERR", state.filter.error);

    Elements fields;
    fields.push_back(text("Status:") | color(theme::subtle));
    fields.push_back(text(interaction_text(state.interaction)) |
                     color(theme::iris) | bold);
    fields.push_back(divider());
    fields.push_back(link_element(state.connection));
    if (terminal_width >= 54) {
      fields.push_back(divider());
      const bool locked =
          state.connection != app::ConnectionState::Disconnected;
      fields.push_back(text(locked ? "HW:LOCKED" : "HW:UNLOCKED") |
                       color(locked ? theme::gold : theme::success));
    }
    if (terminal_width >= 84) {
      fields.push_back(divider());
      fields.push_back(status_field(
          "Port", "P", port,
          state.device_path.empty() ? theme::love : theme::success));
    }
    if (terminal_width >= 104) {
      fields.push_back(divider());
      fields.push_back(status_field(
          "Baud", "B", std::to_string(port_config.baud), theme::success));
    }
    if (terminal_width >= 135) {
      fields.push_back(divider());
      fields.push_back(
          status_field("Serial", "D", serial_value, theme::success));
    }
    if (terminal_width >= 156) {
      fields.push_back(divider());
      fields.push_back(log_element(state.log));
    }
    if (terminal_width >= 176) {
      fields.push_back(divider());
      fields.push_back(status_field(
          "NewLine", "N",
          uppercase_ascii(config::to_string(state.config.send.newline)),
          theme::success));
    }
    if (terminal_width >= 222) {
      fields.push_back(divider());
      fields.push_back(status_field(
          "View", "V",
          "RX:" +
              uppercase_ascii(config::to_string(state.config.receive.rx_view)) +
              " TX:" +
              uppercase_ascii(config::to_string(state.config.receive.tx_view)) +
              "/" + visible_directions,
          theme::success));
    }
    if (terminal_width >= 242) {
      fields.push_back(divider());
      fields.push_back(status_field(
          "InputType", "H",
          uppercase_ascii(config::to_string(state.config.send.mode)),
          theme::success));
    }
    return hbox(std::move(fields)) | size(HEIGHT, EQUAL, 1);
  }

  [[nodiscard]] Element draft_element() const {
    const auto &state = application->snapshot();
    const auto send_mode =
        uppercase_ascii(config::to_string(state.config.send.mode));
    const bool focused = mode == RouteMode::SendEdit;
    Elements title;
    if (focused) {
      title.push_back(text(" Input [" + send_mode + "] ") |
                      color(theme::success) | bold);
      title.push_back(shortcut("Enter"));
      title.push_back(text(" Send ") | color(theme::subtle));
      if (state.config.send.mode == config::SendMode::Txt) {
        title.push_back(shortcut("Alt+Enter"));
        title.push_back(text(" Newline ") | color(theme::subtle));
      }
      title.push_back(shortcut("Alt+Up/Down"));
      title.push_back(text(" History ") | color(theme::subtle));
      title.push_back(shortcut("Esc"));
      title.push_back(text(" Normal ") | color(theme::subtle));
    } else {
      title.push_back(shortcut("i"));
      title.push_back(text(" Input [" + send_mode + "] ") |
                      color(theme::subtle));
    }

    Element body;
    if (focused) {
      body = draft_input->Render() | color(theme::text);
    } else if (state.draft.empty()) {
      body = paragraph("[Use <i> to input.]") | color(theme::muted);
    } else {
      body = paragraph(state.draft) | color(theme::text);
    }
    auto panel =
        panel_background(window(hbox(std::move(title)),
                                std::move(body) | size(HEIGHT, LESS_THAN, 5)),
                         theme::surface);
    panel |= color(focused ? theme::success : theme::subtle);
    return panel;
  }

  [[nodiscard]] Element metrics_element() const {
    const auto &state = application->snapshot();
    const auto task =
        state.task.state == scheduler::SchedulerState::Idle
            ? "TASK:OFF"
            : "TASK:F" + std::to_string(state.task.slot_index) + " @" +
                  std::to_string(state.task.interval_ms) +
                  "ms sent=" + std::to_string(state.task.sent_count) +
                  " missed=" + std::to_string(state.task.missed_count);
    const auto metrics =
        "RX:" + std::to_string(state.rx_bytes) +
        " TX:" + std::to_string(state.tx_bytes) +
        " | MEM:" + std::to_string(state.display_bytes / 1024U) +
        "KiB | TXQ:" + std::to_string(state.tx_pending) +
        " RXQ:" + std::to_string(state.rx_ingress_bytes / 1024U) + "KiB" +
        " LOGQ:" + std::to_string(state.log_pending) + " | " + task +
        (state.manual_pause ? " | PAUSED" : "") +
        (scroll_from_bottom != 0U ? " | AWAY" : "") +
        (mode == RouteMode::Search ? " | SEARCH" : "");
    return panel_background(text(metrics) | color(theme::text),
                            theme::overlay) |
           size(HEIGHT, EQUAL, 1);
  }

  [[nodiscard]] Element actions_element() const {
    Elements actions;
    const auto width = std::max(Terminal::Size().dimx, 0);
    const auto add_action = [&actions](const std::string_view key,
                                       const std::string_view label) {
      if (!actions.empty()) {
        actions.push_back(divider());
      }
      actions.push_back(shortcut(key));
      actions.push_back(text(" " + std::string{label}) | color(theme::text));
    };

    if (mode == RouteMode::SendEdit) {
      add_action("Enter", "Send");
      add_action("Esc", "Normal");
      if (width >= 70 &&
          application->snapshot().config.send.mode == config::SendMode::Txt) {
        add_action("Alt+Enter", "Newline");
      }
      if (width >= 100) {
        add_action("Alt+Up/Down", "History");
      }
    } else if (mode == RouteMode::ReceiveBrowse) {
      add_action("j/k", "Move");
      add_action("q", "Quit");
      if (width >= 60) {
        add_action("gg/G", "Edge");
        add_action("yy", "Copy");
      }
      if (width >= 90) {
        add_action("i", "Input");
        add_action("Esc", "Normal");
      }
      if (width >= 120) {
        add_action("[count]j/k", "Move N");
        add_action("PgUp/PgDn", "Page");
      }
    } else {
      add_action("i", "Input");
      add_action("q", "Quit");
      if (width >= 60) {
        add_action("R", "Browse");
        add_action("/", "Search");
      }
      if (width >= 90) {
        add_action("F5", "Scan");
        add_action("E", "Commands");
      }
      if (width >= 120) {
        add_action("?/F1", "Help");
        add_action("y", "Copy");
      }
    }
    return panel_background(hbox(std::move(actions)), theme::overlay) |
           size(HEIGHT, EQUAL, 1);
  }

  [[nodiscard]] Element main_element() const {
    const auto &state = application->snapshot();
    const bool compact = Terminal::Size().dimy < 16;
    Elements content;
    content.push_back(
        hbox({text(" LazyCom ") | color(theme::rose) | bold, filler()}));
    if (!compact) {
      content.push_back(separator() | color(theme::subtle));
    }
    content.push_back(status_element());
    if (!compact) {
      content.push_back(separator() | color(theme::subtle));
    }
    content.push_back(receive_element());
    content.push_back(draft_element());
    if (!state.configuration_notice.empty()) {
      content.push_back(text(state.configuration_notice) | color(theme::gold));
    }
    content.push_back(text(ui_notice.empty() ? state.notice : ui_notice) |
                      color(theme::muted));
    content.push_back(metrics_element());
    content.push_back(actions_element());
    auto root = panel_background(
        vbox(std::move(content)) | border | color(theme::text), theme::base);
    if (transparent_background()) {
      return root | selectionStyle([](Pixel &pixel) {
               pixel.inverted = true;
               pixel.underlined_double = true;
             });
    }
    return root | selectionBackgroundColor(theme::iris) |
           selectionForegroundColor(theme::base);
  }

  [[nodiscard]] Element overlay_element() const {
    if (mode == RouteMode::Help) {
      return window(text(" LazyCom shortcuts ") | bold,
                    vbox({text("Normal: C connect/cancel/disconnect, i send "
                               "edit, R browse"),
                          text("P/B/D/N/V/H/G/F configuration, g logging, f "
                               "quick send"),
                          text("F5 scan, Space pause, / search, X clear, E "
                               "commands, y copy, q quit"),
                          text("Copy: y or forwarded Ctrl+C; Ctrl+Shift+C is "
                               "handled by the terminal"),
                          text("Send: Enter send, Alt+Enter newline, "
                               "Alt+Up/Down history"),
                          text("Receive: j/k, [count]j/k, gg/G, yy; arrows and "
                               "pages remain available"),
                          text("SYS is lifecycle TXT; ERR is error TXT; "
                               "HW:LOCKED protects serial settings"),
                          text("Search: Enter query, F3 next, Shift+F3 "
                               "previous, Tab direction, End latest"),
                          text("F1 toggles this help over every context; Esc "
                               "restores")})) |
             size(WIDTH, LESS_THAN, 78) | center;
    }
    if (mode == RouteMode::Confirm) {
      std::string message;
      std::string action_label;
      if (confirm_action == ConfirmAction::Quit) {
        message = "Stop tasks, disconnect, flush logs, and quit?";
        action_label = "Quit";
      } else if (confirm_action == ConfirmAction::Clear) {
        message = "Clear " +
                  std::to_string(application->snapshot().records.size()) +
                  " in-memory record(s)? Log files are kept.";
        action_label = "Clear records";
      } else if (confirm_action == ConfirmAction::DeleteQuickSlot) {
        message = "Delete quick-send slot " +
                  std::to_string(pending_quick_slot) + "?";
        action_label = "Delete slot";
      } else {
        message = "Stop the active task and replace it with slot " +
                  std::to_string(pending_quick_slot) + " at " +
                  std::to_string(pending_quick_interval) + " ms?";
        action_label = "Replace task";
      }
      auto cancel = text("[ Cancel ]");
      auto accept = text("[ " + action_label + " ]");
      if (confirm_accept) {
        accept |= inverted;
      } else {
        cancel |= inverted;
      }
      return window(
                 text(" Confirm ") | bold,
                 vbox({paragraph(message), separator(),
                       hbox({std::move(cancel), text("  "), std::move(accept)}),
                       text("Tab/Left/Right: choose   Enter: apply   Esc: "
                            "cancel") |
                           dim})) |
             size(WIDTH, LESS_THAN, 64) | center;
    }
    if (mode == RouteMode::ErrorDialog) {
      const auto &alert = application->snapshot().alert;
      if (!alert) {
        return text("");
      }
      const auto identifier = error_descriptor(alert->code).identifier;
      return window(text(" " + alert->title + " ") | color(theme::love) | bold,
                    vbox({text(std::string{identifier}) | color(theme::love) |
                              bold,
                          separator() | color(theme::love),
                          paragraph(alert->message) | color(theme::love),
                          separator() | color(theme::love),
                          text("Enter/Esc: close") |
                              color(theme::shortcut_text) | bold})) |
             color(theme::love) | size(WIDTH, LESS_THAN, 76) |
             size(HEIGHT, LESS_THAN, 20) | center;
    }
    if (mode == RouteMode::Search) {
      auto query =
          text(std::string{search_focus == SearchFocus::Query ? "> " : "  "} +
               "Query: " + search_query);
      auto direction = text(
          std::string{search_focus == SearchFocus::Direction ? "> " : "  "} +
          "Direction: " + std::string{search_direction_text()});
      if (search_focus == SearchFocus::Query) {
        query |= inverted;
      } else {
        direction |= inverted;
      }
      return window(text(" Search ") | bold,
                    vbox({std::move(query), std::move(direction),
                          text("Matches: " +
                               std::to_string(search_matches.size())),
                          text("Tab focus | Left/Right/Space direction | "
                               "Enter search | F3/Shift+F3 navigate | End "
                               "latest | Esc close") |
                              dim})) |
             size(WIDTH, LESS_THAN, 76) | center;
    }
    if (mode == RouteMode::Modal) {
      Elements body;
      if (modal_options.empty()) {
        body.push_back(
            paragraph(edit_value.empty() ? "[Input please.]" : edit_value) |
            inverted);
      } else {
        if (modal_kind == "P" && application->snapshot().scanning) {
          body.push_back(text("Scanning...") | color(theme::gold));
        }
        const auto begin = selected_device > 8U ? selected_device - 8U : 0U;
        const auto end = std::min(modal_options.size(), begin + 18U);
        for (std::size_t index = begin; index < end; ++index) {
          auto item = text((index == selected_device ? "> " : "  ") +
                           modal_options[index]);
          if (modal_kind == "P" &&
              index < application->snapshot().devices.size() &&
              application->snapshot().devices[index].permission.access ==
                  serial::DeviceAccess::PermissionDenied) {
            item |= color(theme::love);
          }
          if (index == selected_device) {
            item |= bold;
            item |= inverted;
          }
          body.push_back(std::move(item));
        }
      }
      body.push_back(separator());
      body.push_back(text("Enter apply | Esc cancel | F1 help") | dim);
      return window(text(" " + modal_title + " ") | bold,
                    vbox(std::move(body))) |
             size(WIDTH, LESS_THAN, 88) | size(HEIGHT, LESS_THAN, 20) | center;
    }
    return text("");
  }

  [[nodiscard]] Element render() const {
    auto base = main_element();
    if (mode == RouteMode::Normal || mode == RouteMode::SendEdit ||
        mode == RouteMode::ReceiveBrowse) {
      return base;
    }
    return dbox(
        {std::move(base),
         panel_background(overlay_element() | clear_under | color(theme::text),
                          theme::overlay)});
  }
};

Tui::Tui() : impl_(std::make_unique<Impl>()) {}
Tui::~Tui() = default;

int Tui::run() {
  auto created = app::Application::create_default(&Impl::wake, &impl_->screen);
  if (!created) {
    throw std::runtime_error(status_error(created.error()));
  }
  impl_->application = std::move(*created);
  impl_->sync_draft_from_application();
  auto active_input = Maybe(impl_->draft_input, [this] {
    return impl_->mode == RouteMode::SendEdit;
  });
  auto component =
      Renderer(std::move(active_input), [this] { return impl_->render(); });
  component = CatchEvent(std::move(component),
                         [this](Event event) { return impl_->handle(event); });
  impl_->screen.ForceHandleCtrlC(false);
  impl_->screen.SelectionChange(
      [this] { impl_->selected_text = impl_->screen.GetSelection(); });
  std::jthread ticker([this](const std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::this_thread::sleep_for(25ms);
      if (!stop.stop_requested()) {
        impl_->screen.PostEvent(Event::Custom);
      }
    }
  });
  impl_->screen.Loop(std::move(component));
  ticker.request_stop();
  if (!impl_->application->snapshot().shutting_down) {
    impl_->exit_ok = impl_->application->shutdown();
  }
  return impl_->exit_ok ? 0 : 1;
}

} // namespace lazycom::ui
