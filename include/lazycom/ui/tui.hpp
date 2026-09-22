#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ftxui {
class Event;
}

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

enum class PasteConsumeResult : std::uint8_t {
  Consumed,
  Completed,
  Rejected,
};

/** Collects one terminal bracketed paste without routing its contents as keys.
 */
class BracketedPaste {
public:
  void start(RouteMode mode, std::uint64_t context_id,
             std::size_t available_bytes, bool accepts_text);
  [[nodiscard]] PasteConsumeResult consume(std::string_view text,
                                           bool is_character, bool is_end,
                                           RouteMode current_mode,
                                           std::uint64_t current_context_id);
  [[nodiscard]] bool active() const noexcept { return active_; }
  [[nodiscard]] std::string take_text();

private:
  bool active_{};
  bool rejected_{};
  bool accepts_text_{};
  RouteMode mode_{RouteMode::Normal};
  std::uint64_t context_id_{};
  std::size_t available_bytes_{};
  std::string text_;
};

[[nodiscard]] PasteConsumeResult
consume_paste_event(BracketedPaste &paste, const ftxui::Event &event,
                    RouteMode current_mode, std::uint64_t current_context_id);

/**
 * @brief Applies pure context-sensitive routing to one canonical key name.
 *
 * F1 is the only unconditional application action. Overlay and editor modes
 * suppress Normal actions. None may mean unbound, consumed by context, or left
 * for a child component; callers must not fall through from a modal context to
 * Normal routing. Business-state guards remain the Application's
 * responsibility.
 */
[[nodiscard]] RoutedAction route_key(const RouteInput &input) noexcept;
/**
 * @brief Parses one complete or pending ReceiveBrowse Vim-style command.
 *
 * Accepted commands are j, k, positive count plus j/k, gg, G, and yy. Counts
 * clamp to 1000000; a lone g, y, or positive count is Pending. Invalid input is
 * intended to be consumed while clearing the caller's pending prefix.
 */
[[nodiscard]] ReceiveVimResult
parse_receive_vim_command(std::string_view command) noexcept;
/**
 * @brief Applies one code-point-aware edit to strict UTF-8 text.
 * @param value Mutable strict UTF-8 value.
 * @param cursor In/out byte offset; normalized to a code-point boundary even
 * when the requested edit is rejected.
 * @param action Editing operation to apply.
 * @param insertion Strict UTF-8 insertion used only for Insert.
 * @param maximum_bytes Maximum resulting byte count.
 * @return true only when value or cursor performs the requested edit/movement.
 * @note This validates UTF-8 structure but does not make control characters or
 * bidi formatting safe for terminal display.
 */
[[nodiscard]] bool edit_utf8_text(std::string &value, std::size_t &cursor,
                                  Utf8EditAction action,
                                  std::string_view insertion,
                                  std::size_t maximum_bytes);

/**
 * @brief Main-thread owner of the fullscreen FTXUI application loop.
 *
 * Construct, run, and destroy on the same process main thread. run() is
 * blocking and intended for one invocation. Only the loop thread mutates
 * components or application state; workers and the ticker may only use FTXUI's
 * thread-safe PostEvent entry point.
 */
class Tui final {
public:
  Tui();
  ~Tui();
  Tui(const Tui &) = delete;
  Tui &operator=(const Tui &) = delete;

  /**
   * @return 0 after graceful shutdown, or 1 after controlled failed/fatal
   * shutdown.
   * @warning A shutdown deadline failure may abort rather than return.
   */
  [[nodiscard]] int run();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace lazycom::ui
