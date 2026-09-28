#include <lazycom/base/text.hpp>
#include <lazycom/ui/receive_view_model.hpp>
#include <lazycom/ui/tui.hpp>

#include <lazycom/app/application.hpp>

#include "modal_state.hpp"
#include <lazycom/config/schema.hpp>

#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/terminal.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <tuple>
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
    std::size_t begin = value.size() - max_size;
    while (begin < value.size() &&
           (static_cast<unsigned char>(value[begin]) & 0xC0U) == 0x80U) {
      ++begin;
    }
    return std::string{value.substr(begin)};
  }
  std::size_t begin = value.size() - (max_size - 3U);
  while (begin < value.size() &&
         (static_cast<unsigned char>(value[begin]) & 0xC0U) == 0x80U) {
    ++begin;
  }
  return "..." + std::string{value.substr(begin)};
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

[[nodiscard]] std::string status_error(const Error &error) {
  return std::string{error_descriptor(error.code).identifier} + ": " +
         lazycom::sanitize_message(error.detail);
}

[[nodiscard]] bool continuation_byte(const char value) noexcept {
  return (static_cast<unsigned char>(value) & 0xC0U) == 0x80U;
}

[[nodiscard]] std::size_t
utf8_boundary_at_or_before(const std::string_view value,
                           std::size_t position) noexcept {
  position = std::min(position, value.size());
  while (position != 0U && position < value.size() &&
         continuation_byte(value[position])) {
    --position;
  }
  return position;
}

[[nodiscard]] bool strict_utf8(const std::string_view value) noexcept {
  return lazycom::is_strict_utf8(
      std::as_bytes(std::span{value.data(), value.size()}));
}

constexpr std::size_t kSearchMaximumBytes = 4096U;
constexpr std::size_t kCommandMaximumBytes = 64U;
constexpr std::size_t kNumericMaximumBytes = 20U;
constexpr std::size_t kPathMaximumBytes = 4096U;
constexpr std::size_t kGeneralModalMaximumBytes = 4096U;
constexpr std::string_view kPasteBegin = "\x1b[200~";
constexpr std::string_view kPasteEnd = "\x1b[201~";

void set_bracketed_paste_mode(const bool enabled) noexcept {
  static constexpr std::string_view enable = "\x1b[?2004h";
  static constexpr std::string_view disable = "\x1b[?2004l";
  const auto sequence = enabled ? enable : disable;
  static_cast<void>(std::fwrite(sequence.data(), 1U, sequence.size(), stdout));
  static_cast<void>(std::fflush(stdout));
}

class BracketedPasteModeGuard {
public:
  BracketedPasteModeGuard() noexcept { set_bracketed_paste_mode(true); }
  ~BracketedPasteModeGuard() noexcept { set_bracketed_paste_mode(false); }
  BracketedPasteModeGuard(const BracketedPasteModeGuard &) = delete;
  BracketedPasteModeGuard &operator=(const BracketedPasteModeGuard &) = delete;
};

} // namespace

void BracketedPaste::start(const RouteMode mode, const std::uint64_t context_id,
                           const std::size_t available_bytes,
                           const bool accepts_text) {
  active_ = true;
  rejected_ = !accepts_text;
  accepts_text_ = accepts_text;
  mode_ = mode;
  context_id_ = context_id;
  available_bytes_ = available_bytes;
  text_.clear();
}

PasteConsumeResult
BracketedPaste::consume(const std::string_view text, const bool is_character,
                        const bool is_end, const RouteMode current_mode,
                        const std::uint64_t current_context_id) {
  if (!active_) {
    return PasteConsumeResult::Rejected;
  }
  if (current_mode != mode_ || current_context_id != context_id_) {
    rejected_ = true;
  }
  if (is_end) {
    active_ = false;
    if (rejected_) {
      text_.clear();
      return PasteConsumeResult::Rejected;
    }
    return PasteConsumeResult::Completed;
  }
  if (!accepts_text_ || !is_character || !strict_utf8(text) ||
      text.size() >
          available_bytes_ - std::min(available_bytes_, text_.size())) {
    rejected_ = true;
    return PasteConsumeResult::Consumed;
  }
  if (!rejected_) {
    text_.append(text);
  }
  return PasteConsumeResult::Consumed;
}

std::string BracketedPaste::take_text() {
  auto result = std::move(text_);
  text_.clear();
  return result;
}

PasteConsumeResult consume_paste_event(BracketedPaste &paste,
                                       const Event &event,
                                       const RouteMode current_mode,
                                       const std::uint64_t current_context_id) {
  const bool end = event.input() == kPasteEnd;
  if (event.is_character()) {
    return paste.consume(event.input(), true, end, current_mode,
                         current_context_id);
  }
  if (event == Event::Return) {
    return paste.consume("\n", true, end, current_mode, current_context_id);
  }
  if (event == Event::Tab) {
    return paste.consume("\t", true, end, current_mode, current_context_id);
  }
  return paste.consume({}, false, end, current_mode, current_context_id);
}

bool edit_utf8_text(std::string &value, std::size_t &cursor,
                    const Utf8EditAction action,
                    const std::string_view insertion,
                    const std::size_t maximum_bytes) {
  cursor = utf8_boundary_at_or_before(value, cursor);
  switch (action) {
  case Utf8EditAction::Insert:
    if (insertion.empty() || !strict_utf8(insertion) ||
        insertion.size() >
            maximum_bytes - std::min(value.size(), maximum_bytes)) {
      return false;
    }
    value.insert(cursor, insertion);
    cursor += insertion.size();
    return true;
  case Utf8EditAction::Backspace: {
    if (cursor == 0U) {
      return false;
    }
    std::size_t previous = cursor - 1U;
    while (previous != 0U && continuation_byte(value[previous])) {
      --previous;
    }
    value.erase(previous, cursor - previous);
    cursor = previous;
    return true;
  }
  case Utf8EditAction::Delete: {
    if (cursor >= value.size()) {
      return false;
    }
    std::size_t next = cursor + 1U;
    while (next < value.size() && continuation_byte(value[next])) {
      ++next;
    }
    value.erase(cursor, next - cursor);
    return true;
  }
  case Utf8EditAction::MoveLeft:
    if (cursor == 0U) {
      return false;
    }
    --cursor;
    while (cursor != 0U && continuation_byte(value[cursor])) {
      --cursor;
    }
    return true;
  case Utf8EditAction::MoveRight:
    if (cursor >= value.size()) {
      return false;
    }
    ++cursor;
    while (cursor < value.size() && continuation_byte(value[cursor])) {
      ++cursor;
    }
    return true;
  }
  return false;
}

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
  mutable model::GlobalMemoryBudget memory_budget;
  std::unique_ptr<app::Application> application;
  mutable std::optional<ReceiveCoordinates> coordinates;
  struct CachedRecord {
    std::uint64_t id{};
    config::ReceiveView view{config::ReceiveView::Txt};
    model::BudgetReservation reservation;
    Element line;
  };
  mutable model::BudgetReservation cache_storage;
  mutable std::unique_ptr<std::array<CachedRecord, 200>> record_cache;
  mutable std::size_t cache_bytes{};
  mutable std::size_t cache_slot{};
  std::string draft_input_value;
  int draft_cursor{};
  Component draft_input;
  RouteMode mode{RouteMode::Normal};
  struct OverlayFrame {
    RouteMode mode;
    app::InteractionState interaction;
    std::uint64_t modal_id;
  };
  std::array<OverlayFrame, 3> overlay_stack{};
  std::size_t overlay_depth{};
  ModalState modal;
  std::uint64_t quick_save_modal_id{};
  std::optional<std::tuple<std::uint64_t, bool, std::uint64_t>> port_projection;
  std::size_t help_line{};
  ReceiveViewModel receive_view;
  TextEditState search_editor;
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
  bool confirm_accept{};
  std::uint32_t pending_quick_slot{};
  std::uint64_t pending_quick_interval{};
  bool awaiting_quick_save{};
  bool connect_after_port{};
  bool exit_ok{true};
  std::string receive_vim_pending;
  std::string selected_text;
  std::uint32_t quick_slot{1U};
  std::uint64_t quick_interval{};
  std::string quick_preview;
  std::string quick_preview_summary;
  std::size_t preview_offset{};
  bool preserve_selection{};
  BracketedPaste paste;
  std::atomic_bool update_pending{};

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

  void post_update() noexcept {
    if (update_pending.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    try {
      // FTXUI closures do not invalidate the frame. Publish a redraw only
      // after advancing state on the UI thread and observing a visible change.
      screen.Post([this] {
        update_pending.store(false, std::memory_order_release);
        if (advance()) {
          screen.PostEvent(Event::Custom);
        }
      });
    } catch (...) {
      update_pending.store(false, std::memory_order_release);
    }
  }

  static void wake(void *context) noexcept {
    if (context != nullptr) {
      static_cast<Impl *>(context)->post_update();
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
      set_notice(std::string{label} + " exceeds the 1 MiB clipboard limit",
                 true);
      return;
    }
    std::cout << "\x1b]52;c;" << base64_encode(value) << '\a' << std::flush;
    set_notice("Copied " + std::to_string(value.size()) + " byte(s) from " +
               std::string{label});
  }

  void copy_selection() { copy_text(selected_text, "the selection"); }

  [[nodiscard]] const ReceiveCoordinates &
  filtered_coordinates(app::DirectionFilter filter) const {
    if (!coordinates) {
      coordinates.emplace(application->snapshot().records, filter,
                          memory_budget);
    } else {
      coordinates->refresh(filter);
    }
    return *coordinates;
  }

  [[nodiscard]] const ReceiveCoordinates &receive_visible_indices() const {
    return filtered_coordinates(application->snapshot().filter);
  }

  [[nodiscard]] bool search_visible() const {
    if (mode == RouteMode::Search) {
      return true;
    }
    return std::any_of(
        overlay_stack.begin(),
        overlay_stack.begin() + static_cast<std::ptrdiff_t>(overlay_depth),
        [](const auto &frame) { return frame.mode == RouteMode::Search; });
  }

  [[nodiscard]] const ReceiveCoordinates &view_indices() const {
    return filtered_coordinates(search_visible()
                                    ? search_direction_filter()
                                    : application->snapshot().filter);
  }
  void normalize_viewport(const bool follow = false) {
    receive_view.normalize_viewport(view_indices(), follow);
  }
  void normalize_receive_cursor(const bool prefer_latest = false) {
    receive_view.normalize_cursor(receive_visible_indices(), prefer_latest);
    if (!receive_view.cursor()) {
      receive_vim_pending.clear();
    }
  }
  void move_receive_cursor(const bool down, const std::size_t count) {
    receive_view.move(receive_visible_indices(), down, count);
    if (!receive_view.cursor()) {
      receive_vim_pending.clear();
    }
  }
  void move_receive_edge(const bool latest) {
    const auto &visible = receive_visible_indices();
    if (!visible.empty()) {
      receive_view.select(visible, latest ? visible.size() - 1U : 0U);
    }
  }

  void copy_receive_record() {
    if (!receive_view.cursor()) {
      return;
    }
    const auto &records = application->snapshot().records;
    const auto current = std::ranges::lower_bound(
        records, *receive_view.cursor(), {}, &app::VisibleRecord::record_id);
    if (current != records.end() &&
        current->record_id == *receive_view.cursor()) {
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
      preserve_selection = true;
      break;
    case ReceiveVimAction::MoveUp:
      move_receive_cursor(false, parsed.count);
      preserve_selection = true;
      break;
    case ReceiveVimAction::First:
      move_receive_edge(false);
      preserve_selection = true;
      break;
    case ReceiveVimAction::Last:
      move_receive_edge(true);
      preserve_selection = true;
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

  void set_notice(std::string message, const bool error = false) {
    application->publish_notice(std::move(message), error);
  }

  void push_overlay(const RouteMode next) {
    if (overlay_depth == overlay_stack.size()) {
      set_notice("Cannot nest another overlay", true);
      return;
    }
    // Suspended fields and Input components stay in place and do not receive
    // input.
    overlay_stack[overlay_depth++] = {mode, application->snapshot().interaction,
                                      modal.id};
    mode = next;
  }

  void pop_overlay() {
    if (overlay_depth == 0U) {
      close_overlay();
      return;
    }
    const auto frame = overlay_stack[--overlay_depth];
    mode = frame.mode;
    application->set_interaction(frame.interaction);
    normalize_viewport();
  }

  void open_help() {
    if (mode == RouteMode::Help) {
      pop_overlay();
      return;
    }
    help_line = 0U;
    push_overlay(RouteMode::Help);
  }

  void show_alert_if_needed() {
    if (!application->snapshot().alert || mode == RouteMode::ErrorDialog ||
        mode == RouteMode::Help) {
      return;
    }
    push_overlay(RouteMode::ErrorDialog);
  }

  void dismiss_alert() {
    application->dismiss_alert();
    pop_overlay();
  }

  void open_modal(ModalKind kind, std::string title, std::string value) {
    ++modal.id;
    modal.kind = std::move(kind);
    modal.title = std::move(title);
    modal.editor.value = std::move(value);
    const auto maximum = modal_input_limit();
    if (modal.editor.value.size() > maximum) {
      modal.editor.value.resize(
          utf8_boundary_at_or_before(modal.editor.value, maximum));
    }
    modal.editor.cursor = modal.editor.value.size();
    modal.selected = 0U;
    modal.options.clear();
    modal.parent = ModalKind::None;
    mode = RouteMode::Modal;
  }

  [[nodiscard]] std::size_t modal_input_limit() const noexcept {
    if (modal.kind == ModalKind::CommandPalette ||
        modal.kind == ModalKind::QuickExecuteSlots) {
      return kCommandMaximumBytes;
    }
    if (modal.kind == ModalKind::LogFiles ||
        modal.kind == ModalKind::LogTotalSize ||
        modal.kind == ModalKind::LogFileSize) {
      return kNumericMaximumBytes;
    }
    if (modal.kind == ModalKind::LogDirectory) {
      return kPathMaximumBytes;
    }
    if (modal.kind == ModalKind::QuickContent) {
      return config::kQuickSendMaximumBytes;
    }
    if (modal.kind == ModalKind::QuickName) {
      return 64U;
    }
    if (modal.kind == ModalKind::QuickNote) {
      return 256U;
    }
    if (modal.kind == ModalKind::QuickInterval) {
      return kNumericMaximumBytes;
    }
    return kGeneralModalMaximumBytes;
  }

  void open_choice(ModalKind kind, std::string title,
                   std::vector<std::string> options,
                   const std::size_t selected = 0U,
                   ModalKind parent = ModalKind::None) {
    ++modal.id;
    modal.kind = std::move(kind);
    modal.title = std::move(title);
    modal.options = std::move(options);
    modal.selected = modal.options.empty()
                         ? 0U
                         : std::min(selected, modal.options.size() - 1U);
    modal.parent = std::move(parent);
    modal.editor.value.clear();
    modal.editor.cursor = 0U;
    mode = RouteMode::Modal;
  }

  void open_data_root() {
    open_choice(
        ModalKind::DataFormat, "Serial Settings",
        {"Data bits: " + std::to_string(modal.serial().data_bits),
         "Stop bits: " + std::to_string(modal.serial().stop_bits),
         "Parity: " + std::string{config::to_string(modal.serial().parity)},
         "Flow: " + std::string{config::to_string(modal.serial().flow_control)},
         "Apply"});
  }

  void open_view_root() {
    const auto setting = [](const bool enabled,
                            const config::ReceiveView view) {
      return enabled ? uppercase_ascii(config::to_string(view))
                     : std::string{"HIDDEN"};
    };
    open_choice(
        ModalKind::View, "Receive View",
        {"RX: " + setting(modal.view().filter.rx, modal.view().rx_view),
         "TX: " + setting(modal.view().filter.tx, modal.view().tx_view),
         std::string{"SYS: "} + (modal.view().filter.system ? "TXT" : "HIDDEN"),
         std::string{"ERR: "} + (modal.view().filter.error ? "TXT" : "HIDDEN"),
         "Apply"});
  }

  void open_logging_root() {
    open_choice(ModalKind::Logging, "Session Log Settings",
                {"Directory: " + (modal.logging().directory.empty()
                                      ? std::string{"<XDG default>"}
                                      : modal.logging().directory),
                 "Maximum files: " + std::to_string(modal.logging().max_files),
                 "Maximum total size: " +
                     std::to_string(modal.logging().max_total_size_mib) +
                     " MiB",
                 "Maximum file size: " +
                     std::to_string(modal.logging().max_file_size_mib) + " MiB",
                 "Save for Next Session", "Save and Rotate Now", "Cancel"});
  }

  void close_overlay() {
    receive_view.set_matches({});
    overlay_depth = 0U;
    mode = RouteMode::Normal;
    application->set_interaction(app::InteractionState::Normal);
    confirm_action = ConfirmAction::None;
    awaiting_quick_save = false;
    connect_after_port = false;
    normalize_viewport();
  }

  void open_confirm(const ConfirmAction action, const RouteMode return_mode) {
    confirm_action = action;
    confirm_accept = false;
    mode = return_mode;
    push_overlay(RouteMode::Confirm);
  }

  void cancel_confirm() {
    pop_overlay();
    confirm_action = ConfirmAction::None;
    confirm_accept = false;
    if (mode == RouteMode::Normal) {
      application->set_interaction(app::InteractionState::Normal);
    }
  }

  [[nodiscard]] static std::string short_value(const std::string_view value) {
    const auto end = utf8_boundary_at_or_before(
        value, std::min(value.size(), std::size_t{64U}));
    return lazycom::sanitize_message(value.substr(0U, end)) +
           (end < value.size() ? "..." : "");
  }

  void open_quick_slots(const bool edit) {
    std::vector<std::string> options;
    for (std::size_t index = 0U;
         index < application->snapshot().quick_send.slots.size(); ++index) {
      const auto &slot = application->snapshot().quick_send.slots[index];
      options.push_back(
          std::to_string(index + 1U) + "  " +
          (slot ? short_value(slot->name) + " [" +
                      uppercase_ascii(config::to_string(slot->mode)) + "] " +
                      short_value(slot->content)
                : "[Input please.]"));
    }
    open_choice(edit ? ModalKind::QuickSlots : ModalKind::QuickExecuteSlots,
                edit ? "Quick Slots: Enter edit / Delete remove"
                     : "Quick Send: select a slot",
                std::move(options), quick_slot - 1U);
  }

  void open_quick_editor() {
    open_choice(
        ModalKind::QuickEdit, "Edit Slot " + std::to_string(quick_slot),
        {"Name: " + short_value(modal.quick().name),
         "Mode: " + uppercase_ascii(config::to_string(modal.quick().mode)),
         "Content: " + short_value(modal.quick().content),
         "NewLine: " + std::string{config::to_string(modal.quick().newline)},
         "Note: " + short_value(modal.quick().note), "Preview bytes", "Save",
         "Cancel"});
  }

  bool prepare_quick_preview(const config::QuickSendSlot &slot) {
    const auto newline = application->snapshot().config.send.newline;
    auto parsed = scheduler::parse_payload(slot.mode, slot.content,
                                           slot.newline, newline);
    if (!parsed) {
      set_notice(
          "Quick slot payload is invalid or exceeds 1 MiB including its suffix",
          true);
      return false;
    }
    quick_preview =
        encoding::render(parsed.bytes, encoding::DisplayMode::Mixed);
    quick_preview_summary =
        uppercase_ascii(config::to_string(slot.mode)) + " | NewLine:" +
        uppercase_ascii(config::to_string(
            slot.newline == config::Newline::Session ? newline
                                                     : slot.newline)) +
        " | " + std::to_string(parsed.bytes.size()) + " byte(s)";
    preview_offset = 0U;
    return true;
  }

  void open_quick_run() {
    open_choice(ModalKind::QuickRun,
                "Execute Slot " + std::to_string(quick_slot),
                {"Interval: " + std::to_string(quick_interval) + " ms",
                 "Full payload preview",
                 quick_interval == 0U ? "Send once" : "Start periodic task",
                 "Stop active task", "Cancel"});
  }

  [[nodiscard]] std::size_t preview_page_end(const std::size_t offset,
                                             Elements *rows = nullptr) const {
    const auto width =
        static_cast<std::size_t>(std::clamp(Terminal::Size().dimx - 8, 4, 78));
    const auto height = std::clamp(Terminal::Size().dimy - 9, 1, 12);
    auto cursor = offset;
    for (int line = 0; line < height && cursor < quick_preview.size(); ++line) {
      const auto end = utf8_boundary_at_or_before(
          quick_preview, std::min(quick_preview.size(), cursor + width));
      if (rows) {
        rows->push_back(text(quick_preview.substr(cursor, end - cursor)));
      }
      cursor = end;
    }
    return cursor;
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
    const auto &state = application->snapshot();
    if (!receive_view.begin_search(
            search_editor.value, search_direction_filter(),
            state.config.receive, state.records, memory_budget)) {
      set_notice("Not enough memory to search retained records", true);
      return;
    }
    receive_view.advance_search(state.records);
    static_cast<void>(scroll_to_search_match());
  }
  [[nodiscard]] bool scroll_to_search_match() {
    return receive_view.scroll_to_match(view_indices());
  }
  void prune_search_matches() {
    receive_view.prune_matches(application->snapshot().records, view_indices());
  }

  void cycle_search_direction(const bool forward) {
    constexpr auto count = static_cast<std::uint8_t>(SearchDirection::Error) +
                           static_cast<std::uint8_t>(1U);
    const auto current = static_cast<std::uint8_t>(search_direction);
    const auto next =
        forward ? static_cast<std::uint8_t>((current + 1U) % count)
                : static_cast<std::uint8_t>((current + count - 1U) % count);
    search_direction = static_cast<SearchDirection>(next);
    run_search();
  }

  void open_configuration(RoutedAction action) {
    const auto &state = application->snapshot();
    switch (action) {
    case RoutedAction::Port: {
      application->request_scan();
      open_choice(ModalKind::Port, "Serial Device (F5 refresh)",
                  {"Scanning..."});
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
      open_choice(ModalKind::Baud, "Baud Rate", std::move(options), selected);
      break;
    }
    case RoutedAction::DataFormat:
      modal.candidate = app::DataFormatCandidate{
          state.config.serial.data_bits, state.config.serial.stop_bits,
          state.config.serial.parity, state.config.serial.flow_control};
      open_data_root();
      break;
    case RoutedAction::Newline:
      open_choice(ModalKind::Newline, "NewLine Suffix",
                  {"None", "LF  (0A)", "CR  (0D)", "CRLF  (0D 0A)"},
                  static_cast<std::size_t>(state.config.send.newline));
      break;
    case RoutedAction::View:
      modal.candidate =
          app::ViewCandidate{state.config.receive.rx_view,
                             state.config.receive.tx_view, state.filter};
      open_view_root();
      break;
    case RoutedAction::SendMode:
      open_choice(ModalKind::SendMode, "Input Type", {"TXT", "HEX"},
                  state.config.send.mode == config::SendMode::Txt ? 0U : 1U);
      break;
    case RoutedAction::LoggingConfig:
      modal.candidate = app::LogSettingsCandidate{
          state.config.logging.directory, state.config.logging.max_files,
          state.config.logging.max_total_size_mib,
          state.config.logging.max_file_size_mib};
      if (modal.logging().directory.empty()) {
        modal.logging().directory = state.effective_log_directory;
      }
      open_logging_root();
      break;
    case RoutedAction::QuickConfig: {
      quick_slot = state.preferences.last_quick_send_slot;
      open_quick_slots(true);
      break;
    }
    case RoutedAction::QuickExecute:
      quick_slot = state.preferences.last_quick_send_slot;
      quick_interval = state.preferences.last_interval_ms;
      open_quick_slots(false);
      break;
    case RoutedAction::CommandPalette:
      open_modal(ModalKind::CommandPalette,
                 "E  Command: scan/connect/disconnect/help/stop-task", "");
      break;
    default:
      break;
    }
  }

  void apply_modal() {
    Status result;
    if (modal.kind == ModalKind::Port) {
      const auto &devices = application->snapshot().devices;
      if (application->snapshot().scanning) {
        result = tl::unexpected(make_error(
            ErrorCode::ValidationInvalidValue, Operation::ValidateConfig,
            "wait for the current device scan to finish"));
      } else if (devices.empty() || modal.selected >= devices.size()) {
        result = tl::unexpected(make_error(
            ErrorCode::ValidationInvalidValue, Operation::ValidateConfig,
            "no scanned serial device selected"));
      } else if (devices[modal.selected].permission.access ==
                 serial::DeviceAccess::PermissionDenied) {
        application->publish_permission_alert(
            devices[modal.selected].path, devices[modal.selected].permission);
        show_alert_if_needed();
        return;
      } else {
        result = application->apply_port(devices[modal.selected].path);
      }
    } else if (modal.kind == ModalKind::Baud) {
      result = application->apply_baud(config::kBaudPresets[modal.selected]);
    } else if (modal.kind == ModalKind::DataFormat) {
      if (modal.selected == 0U) {
        open_choice(ModalKind::DataBits, "Data Bits", {"5", "6", "7", "8"},
                    static_cast<std::size_t>(modal.serial().data_bits - 5),
                    ModalKind::DataFormat);
        return;
      }
      if (modal.selected == 1U) {
        open_choice(ModalKind::StopBits, "Stop Bits", {"1", "2"},
                    static_cast<std::size_t>(modal.serial().stop_bits - 1),
                    ModalKind::DataFormat);
        return;
      }
      if (modal.selected == 2U) {
        open_choice(ModalKind::Parity, "Parity",
                    {"None", "Odd", "Even", "Mark", "Space"},
                    static_cast<std::size_t>(modal.serial().parity),
                    ModalKind::DataFormat);
        return;
      }
      if (modal.selected == 3U) {
        open_choice(ModalKind::FlowControl, "Flow Control",
                    {"None", "RTS/CTS", "XON/XOFF"},
                    static_cast<std::size_t>(modal.serial().flow_control),
                    ModalKind::DataFormat);
        return;
      }
      result = application->apply_data_format(
          {modal.serial().data_bits, modal.serial().stop_bits,
           modal.serial().parity, modal.serial().flow_control});
    } else if (modal.kind == ModalKind::DataBits) {
      modal.serial().data_bits = static_cast<std::int32_t>(modal.selected + 5U);
      open_data_root();
      return;
    } else if (modal.kind == ModalKind::StopBits) {
      modal.serial().stop_bits = static_cast<std::int32_t>(modal.selected + 1U);
      open_data_root();
      return;
    } else if (modal.kind == ModalKind::Parity) {
      modal.serial().parity = static_cast<config::Parity>(modal.selected);
      open_data_root();
      return;
    } else if (modal.kind == ModalKind::FlowControl) {
      modal.serial().flow_control =
          static_cast<config::FlowControl>(modal.selected);
      open_data_root();
      return;
    } else if (modal.kind == ModalKind::Newline) {
      static constexpr std::array values{
          config::Newline::None, config::Newline::Lf, config::Newline::Cr,
          config::Newline::CrLf};
      result = application->apply_newline(values[modal.selected]);
    } else if (modal.kind == ModalKind::View) {
      if (modal.selected == 0U) {
        open_choice(ModalKind::RxView, "RX View",
                    {"Hidden", "TXT", "HEX", "MIXED"},
                    modal.view().filter.rx
                        ? static_cast<std::size_t>(modal.view().rx_view) + 1U
                        : 0U,
                    ModalKind::View);
        return;
      }
      if (modal.selected == 1U) {
        open_choice(ModalKind::TxView, "TX View",
                    {"Hidden", "TXT", "HEX", "MIXED"},
                    modal.view().filter.tx
                        ? static_cast<std::size_t>(modal.view().tx_view) + 1U
                        : 0U,
                    ModalKind::View);
        return;
      }
      if (modal.selected == 2U) {
        open_choice(ModalKind::SystemVisibility, "SYS Records",
                    {"Hidden", "TXT"}, modal.view().filter.system ? 1U : 0U,
                    ModalKind::View);
        return;
      }
      if (modal.selected == 3U) {
        open_choice(ModalKind::ErrorVisibility, "ERR Records",
                    {"Hidden", "TXT"}, modal.view().filter.error ? 1U : 0U,
                    ModalKind::View);
        return;
      }
      result = application->apply_view(
          {modal.view().rx_view, modal.view().tx_view, modal.view().filter});
    } else if (modal.kind == ModalKind::RxView ||
               modal.kind == ModalKind::TxView) {
      const bool enabled = modal.selected != 0U;
      if (modal.kind == ModalKind::RxView) {
        modal.view().filter.rx = enabled;
        if (enabled) {
          modal.view().rx_view =
              static_cast<config::ReceiveView>(modal.selected - 1U);
        }
      } else {
        modal.view().filter.tx = enabled;
        if (enabled) {
          modal.view().tx_view =
              static_cast<config::ReceiveView>(modal.selected - 1U);
        }
      }
      open_view_root();
      return;
    } else if (modal.kind == ModalKind::SystemVisibility ||
               modal.kind == ModalKind::ErrorVisibility) {
      if (modal.kind == ModalKind::SystemVisibility) {
        modal.view().filter.system = modal.selected != 0U;
      } else {
        modal.view().filter.error = modal.selected != 0U;
      }
      open_view_root();
      return;
    } else if (modal.kind == ModalKind::SendMode) {
      result = application->apply_send_mode(
          modal.selected == 0U ? config::SendMode::Txt : config::SendMode::Hex);
    } else if (modal.kind == ModalKind::Logging) {
      if (modal.selected <= 3U) {
        static constexpr std::array<ModalKind, 4> kinds{
            ModalKind::LogDirectory, ModalKind::LogFiles,
            ModalKind::LogTotalSize, ModalKind::LogFileSize};
        static constexpr std::array<std::string_view, 4> titles{
            "Log Directory", "Maximum Files", "Maximum Total Size (MiB)",
            "Maximum File Size (MiB)"};
        const std::array<std::string, 4> values{
            modal.logging().directory,
            std::to_string(modal.logging().max_files),
            std::to_string(modal.logging().max_total_size_mib),
            std::to_string(modal.logging().max_file_size_mib)};
        open_modal(kinds[modal.selected], std::string{titles[modal.selected]},
                   values[modal.selected]);
        modal.parent = ModalKind::Logging;
        return;
      }
      if (modal.selected == 6U) {
        close_overlay();
        return;
      }
      result = application->apply_logging(modal.logging(),
                                          modal.selected == 4U
                                              ? app::LogApplyPolicy::NextSession
                                              : app::LogApplyPolicy::RotateNow);
    } else if (modal.kind == ModalKind::LogDirectory ||
               modal.kind == ModalKind::LogFiles ||
               modal.kind == ModalKind::LogTotalSize ||
               modal.kind == ModalKind::LogFileSize) {
      auto candidate = modal.logging();
      if (modal.kind == ModalKind::LogDirectory) {
        candidate.directory = modal.editor.value;
      } else {
        std::uint32_t parsed{};
        const auto result_parse = std::from_chars(
            modal.editor.value.data(),
            modal.editor.value.data() + modal.editor.value.size(), parsed);
        if (result_parse.ec != std::errc{} ||
            result_parse.ptr !=
                modal.editor.value.data() + modal.editor.value.size()) {
          result = tl::unexpected(make_error(
              ErrorCode::ValidationInvalidValue, Operation::ValidateConfig,
              "the logging value must be an unsigned integer"));
        } else if (modal.kind == ModalKind::LogFiles) {
          candidate.max_files = parsed;
        } else if (modal.kind == ModalKind::LogTotalSize) {
          candidate.max_total_size_mib = parsed;
        } else {
          candidate.max_file_size_mib = parsed;
        }
      }
      if (result) {
        result = application->validate_logging(candidate);
      }
      if (result) {
        modal.logging() = std::move(candidate);
        open_logging_root();
        return;
      }
    } else if (modal.kind == ModalKind::QuickSlots ||
               modal.kind == ModalKind::QuickExecuteSlots) {
      quick_slot = static_cast<std::uint32_t>(modal.selected + 1U);
      const auto &slot =
          application->snapshot().quick_send.slots[modal.selected];
      if (modal.kind == ModalKind::QuickSlots) {
        modal.candidate = slot.value_or(config::QuickSendSlot{});
        modal.quick().index = quick_slot;
        open_quick_editor();
      } else if (slot && prepare_quick_preview(*slot)) {
        open_quick_run();
      } else if (!slot) {
        set_notice("The selected quick-send slot is empty", true);
      }
      return;
    } else if (modal.kind == ModalKind::QuickEdit) {
      if (modal.selected == 0U || modal.selected == 2U ||
          modal.selected == 4U) {
        const auto field = modal.selected;
        open_modal(field == 0U   ? ModalKind::QuickName
                   : field == 2U ? ModalKind::QuickContent
                                 : ModalKind::QuickNote,
                   field == 2U ? "Content: Alt+Enter inserts a newline"
                               : "Edit slot field",
                   field == 0U   ? modal.quick().name
                   : field == 2U ? modal.quick().content
                                 : modal.quick().note);
        modal.parent = ModalKind::QuickEdit;
      } else if (modal.selected == 1U) {
        open_choice(ModalKind::QuickMode, "Slot input mode", {"TXT", "HEX"},
                    modal.quick().mode == config::SendMode::Txt ? 0U : 1U,
                    ModalKind::QuickEdit);
      } else if (modal.selected == 3U) {
        open_choice(ModalKind::QuickNewline, "Slot suffix",
                    {"None", "LF", "CR", "CRLF", "Session"},
                    static_cast<std::size_t>(modal.quick().newline),
                    ModalKind::QuickEdit);
      } else if (modal.selected == 5U) {
        if (prepare_quick_preview(modal.quick())) {
          open_modal(ModalKind::QuickPreview, "Payload preview", "");
          modal.parent = ModalKind::QuickEdit;
        }
      } else if (modal.selected == 6U) {
        result = application->apply_quick_slot(quick_slot, modal.quick());
        if (result) {
          awaiting_quick_save = true;
          quick_save_modal_id = modal.id;
          modal.title = "Saving quick-send slot...";
          return;
        }
      } else {
        open_quick_slots(true);
      }
      if (result) {
        return;
      }
    } else if (modal.kind == ModalKind::QuickName ||
               modal.kind == ModalKind::QuickContent ||
               modal.kind == ModalKind::QuickNote) {
      if (modal.kind == ModalKind::QuickName) {
        modal.quick().name = modal.editor.value;
      } else if (modal.kind == ModalKind::QuickContent) {
        modal.quick().content = modal.editor.value;
      } else {
        modal.quick().note = modal.editor.value;
      }
      open_quick_editor();
      return;
    } else if (modal.kind == ModalKind::QuickMode ||
               modal.kind == ModalKind::QuickNewline) {
      if (modal.kind == ModalKind::QuickMode) {
        modal.quick().mode = modal.selected == 0U ? config::SendMode::Txt
                                                  : config::SendMode::Hex;
      } else {
        modal.quick().newline = static_cast<config::Newline>(modal.selected);
      }
      open_quick_editor();
      return;
    } else if (modal.kind == ModalKind::QuickInterval) {
      std::uint64_t interval{};
      const auto parsed = std::from_chars(
          modal.editor.value.data(),
          modal.editor.value.data() + modal.editor.value.size(), interval);
      if (parsed.ec != std::errc{} ||
          parsed.ptr != modal.editor.value.data() + modal.editor.value.size() ||
          (interval != 0U &&
           (interval < scheduler::kMinimumPeriodicIntervalMs ||
            interval > scheduler::kMaximumPeriodicIntervalMs))) {
        set_notice("Interval must be 0 or 10..86400000 ms", true);
      } else {
        quick_interval = interval;
        open_quick_run();
      }
      return;
    } else if (modal.kind == ModalKind::QuickRun) {
      if (modal.selected == 0U) {
        open_modal(ModalKind::QuickInterval,
                   "Interval in ms: 0 or 10..86400000",
                   std::to_string(quick_interval));
        modal.parent = ModalKind::QuickRun;
        return;
      }
      if (modal.selected == 1U) {
        preview_offset = 0U;
        open_modal(ModalKind::QuickPreview, "Payload preview", "");
        modal.parent = ModalKind::QuickRun;
        return;
      }
      if (modal.selected == 3U) {
        application->stop_quick_task();
        close_overlay();
        return;
      }
      if (modal.selected == 4U) {
        close_overlay();
        return;
      }
      if (application->snapshot().task.state ==
          scheduler::SchedulerState::Running) {
        pending_quick_slot = quick_slot;
        pending_quick_interval = quick_interval;
        open_confirm(ConfirmAction::ReplaceQuickTask, RouteMode::Modal);
        return;
      }
      result = application->execute_quick(quick_slot, quick_interval);
    } else if (modal.kind == ModalKind::CommandPalette) {
      if (modal.editor.value == "scan") {
        application->request_scan();
      } else if (modal.editor.value == "connect") {
        if (application->snapshot().connection !=
            app::ConnectionState::Disconnected) {
          result = tl::unexpected(make_error(
              ErrorCode::ValidationInvalidValue, Operation::ValidateConfig,
              "connect requires a disconnected link"));
        } else {
          application->connect();
        }
      } else if (modal.editor.value == "disconnect") {
        if (application->snapshot().connection ==
            app::ConnectionState::Disconnected) {
          result = tl::unexpected(make_error(
              ErrorCode::ValidationInvalidValue, Operation::ValidateConfig,
              "disconnect requires an active link"));
        } else {
          application->disconnect();
        }
      } else if (modal.editor.value == "help") {
        close_overlay();
        open_help();
        return;
      } else if (modal.editor.value == "stop-task") {
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
    const bool should_connect =
        modal.kind == ModalKind::Port && connect_after_port;
    connect_after_port = false;
    close_overlay();
    if (should_connect) {
      application->connection_control();
    }
  }

  void edit_text(TextEditState &editor, const Event &event,
                 const std::size_t maximum_bytes) {
    auto &edit_value = editor.value;
    auto &edit_cursor = editor.cursor;
    edit_cursor = utf8_boundary_at_or_before(edit_value, edit_cursor);
    if (event == Event::Backspace) {
      static_cast<void>(edit_utf8_text(edit_value, edit_cursor,
                                       Utf8EditAction::Backspace, {},
                                       maximum_bytes));
    } else if (event == Event::Delete) {
      static_cast<void>(edit_utf8_text(
          edit_value, edit_cursor, Utf8EditAction::Delete, {}, maximum_bytes));
    } else if (event == Event::ArrowLeft) {
      static_cast<void>(edit_utf8_text(edit_value, edit_cursor,
                                       Utf8EditAction::MoveLeft, {},
                                       maximum_bytes));
    } else if (event == Event::ArrowRight) {
      static_cast<void>(edit_utf8_text(edit_value, edit_cursor,
                                       Utf8EditAction::MoveRight, {},
                                       maximum_bytes));
    } else if (event.is_character()) {
      static_cast<void>(edit_utf8_text(edit_value, edit_cursor,
                                       Utf8EditAction::Insert,
                                       event.character(), maximum_bytes));
    }
  }

  [[nodiscard]] std::uint64_t paste_context_id() const noexcept {
    return mode == RouteMode::Modal ? modal.id : 0U;
  }

  void start_paste() {
    std::size_t available{};
    bool accepts_text = false;
    if (mode == RouteMode::SendEdit) {
      const auto maximum = static_cast<std::size_t>(
          application->snapshot().config.send.max_draft_bytes);
      available = maximum - std::min(maximum, draft_input_value.size());
      accepts_text = true;
    } else if (mode == RouteMode::Search &&
               search_focus == SearchFocus::Query) {
      available = kSearchMaximumBytes -
                  std::min(kSearchMaximumBytes, search_editor.value.size());
      accepts_text = true;
    } else if (mode == RouteMode::Modal && modal.options.empty() &&
               modal.kind != ModalKind::QuickPreview) {
      const auto maximum = modal_input_limit();
      available = maximum - std::min(maximum, modal.editor.value.size());
      accepts_text = true;
    }
    paste.start(mode, paste_context_id(), available, accepts_text);
  }

  void apply_paste(std::string text) {
    if (text.empty()) {
      return;
    }
    if (mode == RouteMode::SendEdit) {
      auto cursor = static_cast<std::size_t>(std::max(draft_cursor, 0));
      const auto maximum = static_cast<std::size_t>(
          application->snapshot().config.send.max_draft_bytes);
      if (edit_utf8_text(draft_input_value, cursor, Utf8EditAction::Insert,
                         text, maximum)) {
        draft_cursor = static_cast<int>(std::min(
            cursor, static_cast<std::size_t>(std::numeric_limits<int>::max())));
        application->set_draft(draft_input_value);
        draft_input_value = application->snapshot().draft;
        draft_cursor = std::min(
            draft_cursor,
            static_cast<int>(std::min(
                draft_input_value.size(),
                static_cast<std::size_t>(std::numeric_limits<int>::max()))));
      }
      return;
    }
    if (mode == RouteMode::Search && search_focus == SearchFocus::Query) {
      static_cast<void>(
          edit_utf8_text(search_editor.value, search_editor.cursor,
                         Utf8EditAction::Insert, text, kSearchMaximumBytes));
      return;
    }
    if (mode == RouteMode::Modal && modal.options.empty() &&
        modal.kind != ModalKind::QuickPreview) {
      static_cast<void>(edit_utf8_text(modal.editor.value, modal.editor.cursor,
                                       Utf8EditAction::Insert, text,
                                       modal_input_limit()));
    }
  }

  void scroll(RoutedAction action) {
    const auto &indices = view_indices();
    receive_view.normalize_viewport(indices);
    if (indices.empty()) {
      return;
    }
    auto position = indices.nearest(*receive_view.anchor());
    switch (action) {
    case RoutedAction::ScrollUp:
      position -= std::min(position, std::size_t{1U});
      break;
    case RoutedAction::ScrollDown:
      position = std::min(indices.size() - 1U, position + 1U);
      break;
    case RoutedAction::PageUp:
      position -= std::min(position, std::size_t{10U});
      break;
    case RoutedAction::PageDown:
      position = std::min(indices.size() - 1U, position + 10U);
      break;
    case RoutedAction::Home:
      position = 0U;
      break;
    case RoutedAction::End:
      position = indices.size() - 1U;
      break;
    default:
      break;
    }
    receive_view.anchor(indices, position);
  }

  [[nodiscard]] auto render_stamp() const {
    const auto &state = application->snapshot();
    return std::tuple{
        state.connection,
        state.interaction,
        state.log,
        state.task,
        state.rx_bytes,
        state.tx_bytes,
        state.display_bytes,
        state.display_gap_records,
        state.tx_pending,
        state.rx_ingress_bytes,
        state.log_pending,
        state.notice_revision,
        state.scanning,
        state.quick_send_save_pending,
        state.quick_send_save_failed,
        state.manual_pause,
        state.shutting_down,
        state.records.size(),
        state.records.empty() ? 0U : state.records.back().record_id,
        state.alert.has_value(),
        state.alert ? state.alert->title : std::string{},
        state.alert ? state.alert->message : std::string{},
        state.alert ? state.alert->code : ErrorCode::ValidationInvalidValue,
        mode,
        port_projection,
        awaiting_quick_save,
        modal.id,
        modal.selected,
        receive_view.anchor(),
        receive_view.cursor(),
        receive_view.match_count(),
        receive_view.searching()};
  }

  bool advance() {
    const auto before = render_stamp();
    const bool follow = receive_view.at_bottom() &&
                        !application->snapshot().manual_pause &&
                        !search_visible() &&
                        application->snapshot().interaction !=
                            app::InteractionState::ReceiveBrowse;
    application->tick();
    if (application->snapshot().fatal_stopping) {
      exit_ok = false;
      screen.Exit();
      return false;
    }
    show_alert_if_needed();
    const auto next_port_projection =
        std::tuple{modal.id, application->snapshot().scanning,
                   application->snapshot().notice_revision};
    if (mode == RouteMode::Modal && modal.kind == ModalKind::Port &&
        port_projection != next_port_projection) {
      port_projection = next_port_projection;
      const auto &devices = application->snapshot().devices;
      modal.options.clear();
      if (application->snapshot().scanning) {
        modal.options.push_back("Scanning...");
      } else {
        for (const auto &device : devices) {
          modal.options.push_back(
              device.path + "  " + device.description +
              (device.permission.access ==
                       serial::DeviceAccess::PermissionDenied
                   ? "  [Permission denied]"
                   : ""));
        }
        if (modal.options.empty()) {
          modal.options.push_back("No scanned serial devices");
        }
      }
      modal.selected = std::min(modal.selected, modal.options.size() - 1U);
    }
    if (search_visible()) {
      prune_search_matches();
      const bool had_matches = receive_view.match_count() != 0U;
      receive_view.advance_search(application->snapshot().records);
      if (!had_matches && receive_view.match_count() != 0U) {
        static_cast<void>(scroll_to_search_match());
      }
    }
    if (awaiting_quick_save &&
        !application->snapshot().quick_send_save_pending) {
      awaiting_quick_save = false;
      if (application->snapshot().quick_send_save_failed) {
        modal.title = "F  Save failed; edit is preserved";
      } else {
        if (mode == RouteMode::Modal && modal.id == quick_save_modal_id) {
          close_overlay();
        } else {
          for (std::size_t index = 0U; index < overlay_depth; ++index) {
            auto &frame = overlay_stack[index];
            if (frame.mode == RouteMode::Modal &&
                frame.modal_id == quick_save_modal_id) {
              frame.mode = RouteMode::Normal;
              frame.interaction = app::InteractionState::Normal;
            }
          }
        }
      }
    }
    if (mode == RouteMode::ReceiveBrowse) {
      normalize_receive_cursor();
    } else {
      normalize_viewport(follow);
    }
    return before != render_stamp();
  }

  bool handle(Event event) {
    preserve_selection = false;
    if (event.input() == kPasteBegin) {
      if (paste.active()) {
        static_cast<void>(
            paste.consume({}, false, false, mode, paste_context_id()));
      } else {
        start_paste();
      }
      return true;
    }
    if (event == Event::Custom) {
      return false;
    }
    if (paste.active()) {
      const auto result =
          consume_paste_event(paste, event, mode, paste_context_id());
      if (result == PasteConsumeResult::Completed) {
        apply_paste(paste.take_text());
      } else if (result == PasteConsumeResult::Rejected) {
        set_notice("Paste rejected; input was not changed", true);
      }
      return true;
    }
    if (event == Event::CtrlZ) {
      screen.Post(screen.WithRestoredIO([] {
        set_bracketed_paste_mode(false);
        static_cast<void>(std::raise(SIGTSTP));
        set_bracketed_paste_mode(true);
      }));
      return true;
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
          return false;
        }
        scroll(RoutedAction::ScrollUp);
        return false;
      }
      if (event.mouse().button == Mouse::WheelDown) {
        if (mode == RouteMode::ReceiveBrowse) {
          move_receive_cursor(true, 1U);
          return false;
        }
        scroll(RoutedAction::ScrollDown);
        return false;
      }
    }
    const auto key = event_key(event);
    if (event == Event::Escape && mode == RouteMode::Normal &&
        !application->snapshot().notice.empty()) {
      application->dismiss_notice();
      return true;
    }
    const auto connected =
        application->snapshot().connection == app::ConnectionState::Connected;
    const auto action = route_key(
        {mode, key, connected,
         mode == RouteMode::Modal && modal.kind == ModalKind::Port,
         mode == RouteMode::Search && search_focus == SearchFocus::Direction});
    if (action == RoutedAction::Help) {
      open_help();
      return true;
    }
    if (mode == RouteMode::Help) {
      if (action == RoutedAction::Escape) {
        open_help();
      } else if (event == Event::ArrowDown || event == Event::PageDown ||
                 (event.is_mouse() &&
                  event.mouse().button == Mouse::WheelDown)) {
        help_line = std::min(help_line + 1U, std::size_t{8U});
      } else if (event == Event::ArrowUp || event == Event::PageUp ||
                 (event.is_mouse() && event.mouse().button == Mouse::WheelUp)) {
        help_line -= std::min(help_line, std::size_t{1U});
      } else if (event == Event::Home) {
        help_line = 0U;
      } else if (event == Event::End) {
        help_line = 8U;
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
          const auto result =
              application->apply_quick_slot(pending_quick_slot, std::nullopt);
          cancel_confirm();
          if (!result) {
            set_notice(status_error(result.error()), true);
          } else {
            awaiting_quick_save = true;
            quick_save_modal_id = modal.id;
            modal.title = "F  Deleting quick-send slot...";
          }
        } else if (confirm_action == ConfirmAction::ReplaceQuickTask) {
          const auto result = application->execute_quick(
              pending_quick_slot, pending_quick_interval, true);
          if (!result) {
            cancel_confirm();
            set_notice(status_error(result.error()), true);
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
        if (modal.parent == ModalKind::DataFormat) {
          open_data_root();
        } else if (modal.parent == ModalKind::View) {
          open_view_root();
        } else if (modal.parent == ModalKind::Logging) {
          open_logging_root();
        } else if (modal.parent == ModalKind::QuickEdit) {
          open_quick_editor();
        } else if (modal.parent == ModalKind::QuickRun) {
          open_quick_run();
        } else if (modal.kind == ModalKind::QuickEdit ||
                   modal.kind == ModalKind::QuickRun) {
          open_quick_slots(modal.kind == ModalKind::QuickEdit);
        } else {
          close_overlay();
        }
      } else if (modal.kind == ModalKind::QuickPreview) {
        if (event == Event::PageDown || event == Event::ArrowDown ||
            (event.is_mouse() && event.mouse().button == Mouse::WheelDown)) {
          const auto next = preview_page_end(preview_offset);
          if (next < quick_preview.size()) {
            preview_offset = next;
          }
        } else if (event == Event::PageUp || event == Event::ArrowUp ||
                   (event.is_mouse() &&
                    event.mouse().button == Mouse::WheelUp)) {
          const auto width = static_cast<std::size_t>(
              std::clamp(Terminal::Size().dimx - 8, 4, 78));
          const auto height = static_cast<std::size_t>(
              std::clamp(Terminal::Size().dimy - 9, 1, 12));
          preview_offset = utf8_boundary_at_or_before(
              quick_preview,
              preview_offset - std::min(preview_offset, width * height));
        } else if (event == Event::Home) {
          preview_offset = 0U;
        }
      } else if (action == RoutedAction::Scan) {
        application->request_scan();
        modal.options = {"Scanning..."};
        modal.selected = 0U;
      } else if (action == RoutedAction::Apply) {
        apply_modal();
      } else if (!modal.options.empty() &&
                 (action == RoutedAction::FocusNext ||
                  action == RoutedAction::FocusPrevious)) {
        if (action == RoutedAction::FocusPrevious && modal.selected != 0U) {
          --modal.selected;
        } else if (action == RoutedAction::FocusNext &&
                   modal.selected + 1U < modal.options.size()) {
          ++modal.selected;
        }
      } else if (modal.kind == ModalKind::QuickSlots &&
                 event == Event::Delete) {
        const auto slot = static_cast<std::uint32_t>(modal.selected + 1U);
        if (!application->snapshot().quick_send.slots[slot - 1U]) {
          set_notice("The selected quick-send slot is already empty", true);
        } else {
          pending_quick_slot = slot;
          open_confirm(ConfirmAction::DeleteQuickSlot, RouteMode::Modal);
        }
      } else if (!modal.options.empty() &&
                 (event == Event::Home || event == Event::End)) {
        modal.selected = event == Event::Home ? 0U : modal.options.size() - 1U;
      } else if (!modal.options.empty() &&
                 (event == Event::ArrowUp || event == Event::ArrowDown)) {
        if (event == Event::ArrowUp && modal.selected != 0U) {
          --modal.selected;
        } else if (event == Event::ArrowDown &&
                   modal.selected + 1U < modal.options.size()) {
          ++modal.selected;
        }
      } else if (modal.options.empty()) {
        if (modal.kind == ModalKind::QuickContent && key == "Alt+Enter") {
          static_cast<void>(edit_utf8_text(
              modal.editor.value, modal.editor.cursor, Utf8EditAction::Insert,
              "\n", modal_input_limit()));
        } else if (event == Event::Home) {
          modal.editor.cursor = 0U;
        } else if (event == Event::End) {
          modal.editor.cursor = modal.editor.value.size();
        } else {
          edit_text(modal.editor, event, modal_input_limit());
        }
      }
      return true;
    }
    if (mode == RouteMode::Search) {
      if (action == RoutedAction::Escape) {
        close_overlay();
      } else if (action == RoutedAction::End) {
        close_overlay();
        normalize_viewport(true);
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
      } else if (receive_view.match_count() != 0U &&
                 (action == RoutedAction::SearchNext ||
                  action == RoutedAction::SearchPrevious)) {
        receive_view.next_match(action == RoutedAction::SearchPrevious);
        if (!scroll_to_search_match()) {
          prune_search_matches();
          static_cast<void>(scroll_to_search_match());
        }
      } else if (search_focus == SearchFocus::Query) {
        edit_text(search_editor, event, kSearchMaximumBytes);
      }
      return true;
    }
    if (mode == RouteMode::SendEdit) {
      if (action == RoutedAction::Escape) {
        close_overlay();
      } else if (action == RoutedAction::Submit) {
        if (application->submit_draft()) {
          sync_draft_from_application();
          // Let Input clear its private selection after a successful admission.
          static_cast<void>(draft_input->OnEvent(Event::Home));
        }
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
        return !preserve_selection;
      } else if (!receive_vim_pending.empty()) {
        receive_vim_pending.clear();
        return true;
      }
      if (action == RoutedAction::ScrollUp) {
        move_receive_cursor(false, 1U);
        return false;
      }
      if (action == RoutedAction::ScrollDown) {
        move_receive_cursor(true, 1U);
        return false;
      }
      if (action == RoutedAction::PageUp) {
        move_receive_cursor(false, 10U);
        return false;
      }
      if (action == RoutedAction::PageDown) {
        move_receive_cursor(true, 10U);
        return false;
      }
      if (action == RoutedAction::Home) {
        move_receive_edge(false);
        return false;
      }
      if (action == RoutedAction::End) {
        move_receive_edge(true);
        return false;
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
      search_editor.value.clear();
      receive_view.set_matches({});
      search_focus = SearchFocus::Query;
      search_direction = SearchDirection::All;
      search_editor.cursor = 0U;
      normalize_viewport();
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
      post_update();
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

  [[nodiscard]] Element record_element(const app::VisibleRecord &record) const {
    const auto &settings = application->snapshot().config.receive;
    const auto view = record.direction == app::RecordDirection::Rx
                          ? settings.rx_view
                      : record.direction == app::RecordDirection::Tx
                          ? settings.tx_view
                          : config::ReceiveView::Txt;
    if (record_cache) {
      for (const auto &entry : *record_cache) {
        if (entry.line && entry.id == record.record_id && entry.view == view) {
          return entry.line;
        }
      }
    }
    auto rendered = application->render_record(record);
    const auto bytes = rendered.capacity() + 512U;
    constexpr std::size_t maximum_cache_bytes = 2U * 1024U * 1024U;
    constexpr auto storage_bytes = sizeof(std::array<CachedRecord, 200>);
    if (bytes > maximum_cache_bytes - storage_bytes) {
      return text(std::move(rendered));
    }
    if (!record_cache) {
      auto reserved = memory_budget.try_reserve(
          model::BudgetCategory::UiRecords, storage_bytes);
      if (!reserved) {
        return text(std::move(rendered));
      }
      record_cache = std::make_unique<std::array<CachedRecord, 200>>();
      cache_storage = std::move(*reserved);
      cache_bytes = storage_bytes;
    }
    for (std::size_t attempt = 0U; attempt < record_cache->size(); ++attempt) {
      auto &entry = (*record_cache)[cache_slot];
      entry.line.reset();
      cache_bytes -= entry.reservation.bytes();
      entry.reservation.reset();
      if (cache_bytes <= maximum_cache_bytes - bytes) {
        auto reserved =
            memory_budget.try_reserve(model::BudgetCategory::UiRecords, bytes);
        if (!reserved) {
          return text(std::move(rendered));
        }
        entry.id = record.record_id;
        entry.view = view;
        entry.reservation = std::move(*reserved);
        entry.line = text(std::move(rendered));
        cache_bytes += bytes;
        cache_slot = (cache_slot + 1U) % record_cache->size();
        return entry.line;
      }
      cache_slot = (cache_slot + 1U) % record_cache->size();
    }
    return text(std::move(rendered));
  }

  [[nodiscard]] Element records_element(std::size_t maximum_rows) const {
    const auto &state = application->snapshot();
    if (state.records.empty()) {
      record_cache.reset();
      cache_storage.reset();
      cache_bytes = 0U;
      cache_slot = 0U;
    } else if (record_cache) {
      for (auto &entry : *record_cache) {
        if (entry.line && (entry.id < state.records.front().record_id ||
                           entry.id > state.records.back().record_id)) {
          entry.line.reset();
          cache_bytes -= entry.reservation.bytes();
          entry.reservation.reset();
        }
      }
    }
    Elements rows;
    if (state.display_gap_records != 0U) {
      maximum_rows -= std::min(maximum_rows, std::size_t{1U});
      rows.push_back(
          text("... GAP: " + std::to_string(state.display_gap_records) +
               " evicted record(s) ...") |
          color(theme::gold));
    }
    const auto &indices = view_indices();
    const auto [begin, end] = receive_view.viewport(
        indices, maximum_rows, mode == RouteMode::ReceiveBrowse);
    for (std::size_t position = begin; position < end; ++position) {
      const auto index = indices[position];
      const auto &record = state.records[index];
      const bool current = mode == RouteMode::ReceiveBrowse &&
                           receive_view.cursor() &&
                           record.record_id == *receive_view.cursor();
      auto row = record_element(record);
      if (mode == RouteMode::ReceiveBrowse) {
        row = hbox({text(current ? "> " : "  "), std::move(row)});
      }
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
      }
      if (position + 1U == end) {
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

  [[nodiscard]] Element receive_element(std::size_t maximum_rows) const {
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
        window(hbox(std::move(title)), records_element(maximum_rows)),
        theme::surface);
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
    int used = 0;
    bool full = false;
    const auto add_field = [&](Element field) {
      field->ComputeRequirement();
      const auto needed = field->requirement().min_x + (fields.empty() ? 0 : 3);
      if (full || needed > std::max(terminal_width - 2 - used, 0)) {
        full = true;
        return;
      }
      if (!fields.empty()) {
        fields.push_back(divider());
      }
      used += needed;
      fields.push_back(std::move(field));
    };
    if (terminal_width < 70) {
      add_field(text("Link:" + connection_text(state.connection)) |
                color(state.connection == app::ConnectionState::Connected
                          ? theme::success
                          : theme::gold));
      add_field(
          text("Log:" + log_text(state.log)) |
          color(state.log == app::LogState::Error ? theme::love : theme::text) |
          bold);
    } else {
      add_field(link_element(state.connection));
      add_field(log_element(state.log));
    }
    add_field(text("Status:" + interaction_text(state.interaction)) |
              color(theme::iris) | bold);
    const bool locked = state.connection != app::ConnectionState::Disconnected;
    add_field(text(locked ? "HW:LOCKED" : "HW:UNLOCKED") |
              color(locked ? theme::gold : theme::success));
    add_field(
        status_field("Port", "P", port,
                     state.device_path.empty() ? theme::love : theme::success));
    add_field(status_field("Baud", "B", std::to_string(port_config.baud),
                           theme::success));
    add_field(status_field("Serial", "D", serial_value, theme::success));
    add_field(status_field(
        "NewLine", "N",
        uppercase_ascii(config::to_string(state.config.send.newline)),
        theme::success));
    add_field(status_field(
        "View", "V",
        "RX:" +
            uppercase_ascii(config::to_string(state.config.receive.rx_view)) +
            " TX:" +
            uppercase_ascii(config::to_string(state.config.receive.tx_view)) +
            "/" + visible_directions,
        theme::success));
    add_field(
        status_field("InputType", "H",
                     uppercase_ascii(config::to_string(state.config.send.mode)),
                     theme::success));
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
        (!receive_view.at_bottom() ? " | AWAY" : "") +
        (search_visible() ? " | SEARCH" : "");
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

    if (mode == RouteMode::Help) {
      add_action("Esc/F1", "Return");
      add_action("Up/Down", "Read");
    } else if (mode == RouteMode::ErrorDialog) {
      add_action("Enter/Esc", "Close");
      add_action("F1", "Help");
    } else if (mode == RouteMode::Confirm) {
      add_action("Esc", "Cancel");
      add_action("Tab", "Choose");
      add_action("Enter", "Apply");
    } else if (mode == RouteMode::Search) {
      add_action("Esc", "Return");
      add_action("Enter", "Search");
      if (width >= 70) {
        add_action("F3", "Next");
      }
    } else if (mode == RouteMode::Modal) {
      add_action("Esc", "Back");
      add_action(modal.kind == ModalKind::QuickPreview ? "PgUp/PgDn" : "Enter",
                 modal.kind == ModalKind::QuickPreview ? "Read" : "Select");
      if (width >= 70) {
        add_action("Tab", "Focus");
      }
    } else if (mode == RouteMode::SendEdit) {
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
    const auto height = Terminal::Size().dimy;
    const bool compact = height < 16;
    auto draft = draft_element();
    draft->ComputeRequirement();
    const auto fixed_rows = 9 + (compact ? 0 : 2) + draft->requirement().min_y +
                            (state.configuration_notice.empty() ? 0 : 1);
    const auto receive_rows =
        static_cast<std::size_t>(std::clamp(height - fixed_rows, 1, 200));
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
    content.push_back(receive_element(receive_rows));
    content.push_back(std::move(draft));
    if (!state.configuration_notice.empty()) {
      content.push_back(text(state.configuration_notice) | color(theme::gold));
    }
    content.push_back(
        text(state.notice +
             (state.notice_error ? " [Esc in Normal: dismiss]" : "")) |
        color(state.notice_error ? theme::love : theme::muted));
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
      const std::array<std::string, 9> sections{
          "Normal: C connect/cancel/disconnect, i input, R browse. Esc "
          "dismisses a notice.",
          "P/B/D/N/V/H/G/F configure; g logging; f quick send. Quick slots: "
          "arrows/Home/End select, Enter open.",
          "F5 scan; Space pause; / search; X clear; E commands; q quit. Help "
          "never sends data.",
          "Normal y copies selection; Receive yy copies current record. "
          "Forwarded Ctrl+C copies selection. Ctrl+Shift+C belongs to the "
          "terminal; OSC 52 must be supported.",
          "Input: Enter send; Alt+Enter newline; Alt+Up/Down history; Esc "
          "Normal. Rejected sends preserve the draft and cursor.",
          "Receive: j/k, [count]j/k, gg/G, yy; arrows, pages and wheel. Esc "
          "Normal. Returning to the bottom does not cancel manual pause.",
          "SYS is lifecycle text; ERR is error text. HW:LOCKED protects serial "
          "settings. Log:ERROR means logging stopped, not serial failure.",
          "Search: Enter query; F3/Shift+F3 next/previous; Tab direction; End "
          "latest; Esc nearest normally visible record.",
          "F1/Esc restores the source context. Quick payload preview uses "
          "pages; interval 0 sends once, 10..86400000 ms is best effort, not "
          "real time."};
      Elements body;
      for (std::size_t index = 0U; index < sections.size(); ++index) {
        auto section = paragraph(sections[index]);
        if (index == help_line) {
          section |= focus;
          section |= bold;
        }
        body.push_back(std::move(section));
        body.push_back(separator());
      }
      return window(text(" Help " + std::to_string(help_line + 1U) + "/9 ") |
                        bold,
                    vbox({vbox(std::move(body)) | yframe | flex,
                          hbox({shortcut("Up/Down"), text(" Read  "),
                                shortcut("F1/Esc"), text(" Return")})})) |
             size(WIDTH, LESS_THAN,
                  std::min(78, std::max(Terminal::Size().dimx - 4, 1))) |
             size(HEIGHT, EQUAL,
                  std::min(20, std::max(Terminal::Size().dimy - 2, 1))) |
             center;
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
               "Query: " + lazycom::sanitize_message(search_editor.value));
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
                               std::to_string(receive_view.match_count()) +
                               (receive_view.searching() ? " (searching...)"
                                                         : "")),
                          text("Tab focus | Left/Right/Space direction | "
                               "Enter search | F3/Shift+F3 navigate | End "
                               "latest | Esc close") |
                              dim})) |
             size(WIDTH, LESS_THAN, 76) | center;
    }
    if (mode == RouteMode::Modal) {
      Elements body;
      if (modal.kind == ModalKind::QuickPreview) {
        body.push_back(text(quick_preview_summary) | color(theme::gold));
        const auto end = preview_page_end(preview_offset, &body);
        body.push_back(text("Preview " + std::to_string(preview_offset) + ".." +
                            std::to_string(end) + "/" +
                            std::to_string(quick_preview.size())) |
                       dim);
        body.push_back(text("PgUp/PgDn read | Home first | Esc back") | bold);
      } else if (modal.options.empty()) {
        const auto cursor =
            utf8_boundary_at_or_before(modal.editor.value, modal.editor.cursor);
        const auto begin = utf8_boundary_at_or_before(
            modal.editor.value, cursor - std::min(cursor, std::size_t{64U}));
        const auto end = utf8_boundary_at_or_before(
            modal.editor.value,
            std::min(modal.editor.value.size(), cursor + 64U));
        body.push_back(
            paragraph((begin == 0U ? "" : "...") +
                      lazycom::sanitize_message(
                          std::string_view{modal.editor.value}.substr(
                              begin, cursor - begin)) +
                      "|" +
                      lazycom::sanitize_message(
                          std::string_view{modal.editor.value}.substr(
                              cursor, end - cursor)) +
                      (end == modal.editor.value.size() ? "" : "...")) |
            inverted);
        body.push_back(text("Cursor " + std::to_string(cursor) + "/" +
                            std::to_string(modal.editor.value.size()) +
                            " bytes") |
                       dim);
      } else {
        if (modal.kind == ModalKind::QuickRun) {
          body.push_back(text(quick_preview_summary) | color(theme::gold));
          body.push_back(paragraph(short_value(quick_preview)));
        }
        if (modal.kind == ModalKind::Port && application->snapshot().scanning) {
          body.push_back(text("Scanning...") | color(theme::gold));
        }
        const auto begin = modal.selected > 8U ? modal.selected - 8U : 0U;
        const auto end = std::min(modal.options.size(), begin + 18U);
        for (std::size_t index = begin; index < end; ++index) {
          auto item = text((index == modal.selected ? "> " : "  ") +
                           lazycom::sanitize_message(modal.options[index]));
          if (modal.kind == ModalKind::Port &&
              index < application->snapshot().devices.size() &&
              application->snapshot().devices[index].permission.access ==
                  serial::DeviceAccess::PermissionDenied) {
            item |= color(theme::love);
          }
          if (index == modal.selected) {
            item |= bold;
            item |= inverted;
            item |= focus;
          }
          body.push_back(std::move(item));
        }
      }
      body.push_back(separator());
      if (modal.kind != ModalKind::QuickPreview) {
        body.push_back(text("Enter apply | Esc back | F1 help") | dim);
      }
      return window(text(" " + modal.title + " ") | bold,
                    vbox(std::move(body)) | yframe) |
             size(WIDTH, LESS_THAN,
                  std::min(88, std::max(Terminal::Size().dimx - 4, 1))) |
             size(HEIGHT, LESS_THAN,
                  std::min(20, std::max(Terminal::Size().dimy - 2, 1))) |
             center;
    }
    return text("");
  }

  [[nodiscard]] Element render() const {
    if (Terminal::Size().dimx < 40 || Terminal::Size().dimy < 12) {
      return panel_background(
          paragraph(
              "Resize terminal to at least 40x12; serial activity continues.") |
              color(theme::gold),
          theme::base);
    }
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
  auto created = app::Application::create_default(&Impl::wake, impl_.get(),
                                                  impl_->memory_budget);
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
  impl_->screen.ForceHandleCtrlZ(false);
  impl_->screen.SelectionChange(
      [this] { impl_->selected_text = impl_->screen.GetSelection(); });
  std::jthread ticker([this](const std::stop_token stop) {
    try {
      while (!stop.stop_requested()) {
        std::this_thread::sleep_for(25ms);
        if (!stop.stop_requested()) {
          impl_->post_update();
        }
      }
    } catch (...) {
      // A failed ticker must not escape the thread and call std::terminate.
    }
  });
  BracketedPasteModeGuard paste_mode;
  impl_->screen.Loop(std::move(component));
  ticker.request_stop();
  if (!impl_->application->snapshot().shutting_down ||
      impl_->application->snapshot().fatal_stopping) {
    impl_->exit_ok = impl_->application->shutdown();
  }
  return impl_->exit_ok ? 0 : 1;
}

} // namespace lazycom::ui
