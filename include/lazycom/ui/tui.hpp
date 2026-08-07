#pragma once

#include <lazycom/app/application.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace lazycom::ui {

enum class RouteMode : std::uint8_t {
  Normal,
  SendEdit,
  ReceiveBrowse,
  Search,
  Modal,
  Help,
  Confirm,
  ErrorDialog,
};

enum class RoutedAction : std::uint8_t {
  None,
  Help,
  Quit,
  Connection,
  Browse,
  Edit,
  CommandPalette,
  Clear,
  Redraw,
  Pause,
  Search,
  Scan,
  Port,
  Baud,
  DataFormat,
  Newline,
  View,
  SendMode,
  LoggingConfig,
  QuickConfig,
  ToggleLog,
  CopySelection,
  QuickExecute,
  Submit,
  InsertNewline,
  HistoryPrevious,
  HistoryNext,
  Escape,
  ScrollUp,
  ScrollDown,
  PageUp,
  PageDown,
  Home,
  End,
  SearchNext,
  SearchPrevious,
  FocusNext,
  FocusPrevious,
  SelectNext,
  SelectPrevious,
  Apply,
};

enum class ReceiveVimAction : std::uint8_t {
  Pending,
  MoveDown,
  MoveUp,
  First,
  Last,
  YankLine,
  Invalid,
};

enum class Utf8EditAction : std::uint8_t {
  Insert,
  Backspace,
  Delete,
  MoveLeft,
  MoveRight,
};

struct ReceiveVimResult {
  ReceiveVimAction action{ReceiveVimAction::Invalid};
  std::size_t count{1U};
};

struct RouteInput {
  RouteMode mode{RouteMode::Normal};
  std::string key;
  bool connected{};
  bool device_modal{};
  bool search_filter_focused{};
};

[[nodiscard]] RoutedAction route_key(const RouteInput &input) noexcept;
[[nodiscard]] ReceiveVimResult
parse_receive_vim_command(std::string_view command) noexcept;
[[nodiscard]] bool edit_utf8_text(std::string &value, std::size_t &cursor,
                                  Utf8EditAction action,
                                  std::string_view insertion,
                                  std::size_t maximum_bytes);

class Tui final {
public:
  Tui();
  ~Tui();
  Tui(const Tui &) = delete;
  Tui &operator=(const Tui &) = delete;

  [[nodiscard]] int run();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lazycom::ui
