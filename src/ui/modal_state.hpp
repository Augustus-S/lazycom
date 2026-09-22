#pragma once

#include <lazycom/app/application.hpp>
#include <variant>

namespace lazycom::ui {

enum class ModalKind {
  None,
  Port,
  Baud,
  DataFormat,
  DataBits,
  StopBits,
  Parity,
  FlowControl,
  Newline,
  View,
  RxView,
  TxView,
  SystemVisibility,
  ErrorVisibility,
  SendMode,
  Logging,
  LogDirectory,
  LogFiles,
  LogTotalSize,
  LogFileSize,
  QuickSlots,
  QuickExecuteSlots,
  QuickEdit,
  QuickName,
  QuickMode,
  QuickContent,
  QuickNewline,
  QuickNote,
  QuickInterval,
  QuickRun,
  QuickPreview,
  CommandPalette
};

struct TextEditState {
  std::string value;
  std::size_t cursor{};
};

struct ModalState {
  std::uint64_t id{};
  ModalKind kind{ModalKind::None};
  ModalKind parent{ModalKind::None};
  std::string title;
  TextEditState editor;
  std::vector<std::string> options;
  std::size_t selected{};
  std::variant<std::monostate, app::DataFormatCandidate, app::ViewCandidate,
               app::LogSettingsCandidate, config::QuickSendSlot>
      candidate;

  app::DataFormatCandidate &serial() {
    return std::get<app::DataFormatCandidate>(candidate);
  }
  app::ViewCandidate &view() { return std::get<app::ViewCandidate>(candidate); }
  app::LogSettingsCandidate &logging() {
    return std::get<app::LogSettingsCandidate>(candidate);
  }
  config::QuickSendSlot &quick() {
    return std::get<config::QuickSendSlot>(candidate);
  }
};

} // namespace lazycom::ui
