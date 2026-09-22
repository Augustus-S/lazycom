#pragma once

#include <lazycom/base/ids.hpp>
#include <lazycom/config/types.hpp>

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lazycom::scheduler {

inline constexpr std::uint64_t kMinimumPeriodicIntervalMs = 10U;
inline constexpr std::uint64_t kMaximumPeriodicIntervalMs = 86'400'000U;

enum class PayloadParseStatus : std::uint8_t {
  Success,
  InvalidMode,
  InvalidText,
  InvalidHex,
  InvalidNewline,
  TooLarge,
};

struct PayloadParseResult {
  PayloadParseStatus status{PayloadParseStatus::Success};
  std::vector<std::byte> bytes;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == PayloadParseStatus::Success;
  }
};

/**
 * @brief Parses a bounded quick-send payload and appends its resolved newline.
 * @param mode TXT requires strict UTF-8; HEX uses whitespace-separated bytes.
 * @param content Untrusted slot content.
 * @param newline Requested newline, possibly Session.
 * @param session_newline Concrete newline used to resolve Session.
 * @return Complete owned bytes on success. Every failure returns no bytes.
 */
[[nodiscard]] PayloadParseResult
parse_payload(config::SendMode mode, std::string_view content,
              config::Newline newline,
              config::Newline session_newline = config::Newline::None);

struct QuickSendExecution {
  std::uint32_t slot_index{};
  std::string name;
  config::SendMode mode{config::SendMode::Txt};
  config::Newline newline{config::Newline::Session};
  config::Newline effective_newline{config::Newline::None};
  std::string note;
  std::vector<std::byte> bytes;
  auto operator<=>(const QuickSendExecution &) const = default;
};

enum class QuickSendBuildStatus : std::uint8_t {
  Success,
  SlotOutOfRange,
  EmptySlot,
  InvalidSlot,
  InvalidPayload,
};

struct QuickSendBuildResult {
  QuickSendBuildStatus status{QuickSendBuildStatus::Success};
  PayloadParseStatus payload_status{PayloadParseStatus::Success};
  std::optional<QuickSendExecution> execution;

  [[nodiscard]] explicit operator bool() const noexcept {
    return status == QuickSendBuildStatus::Success && execution.has_value();
  }
};

/**
 * @brief Builds an immutable execution snapshot for a persisted quick-send
 * slot.
 * @param slots Complete slot snapshot to inspect.
 * @param slot_index User-facing slot number in the range 1 through 20.
 * @param session_newline Concrete newline used by slots configured as Session.
 * @return A fully owned snapshot unaffected by later configuration edits, or a
 * shape/payload error.
 */
[[nodiscard]] QuickSendBuildResult
make_quick_send_execution(const config::QuickSendSnapshot &slots,
                          std::uint32_t slot_index,
                          config::Newline session_newline);

[[nodiscard]] constexpr bool
valid_interval(const std::uint64_t interval_ms) noexcept {
  return interval_ms == 0U || (interval_ms >= kMinimumPeriodicIntervalMs &&
                               interval_ms <= kMaximumPeriodicIntervalMs);
}

struct TaskRequest {
  QuickSendExecution execution;
  std::uint64_t interval_ms{};
};

enum class SchedulerState : std::uint8_t {
  Idle,
  Running,
  Stopping,
};

enum class TaskStartStatus : std::uint8_t {
  Started,
  ReplacementConfirmationRequired,
  ReplacementStopRequested,
  StopInProgress,
  InvalidInterval,
  InvalidExecution,
  GenerationOverflow,
  DeadlineOverflow,
};

struct TaskStartResult {
  TaskStartStatus status{TaskStartStatus::InvalidExecution};
  std::optional<TaskGeneration> started_generation;
  std::optional<TaskGeneration> generation_to_stop;
};

enum class TaskInvalidationStatus : std::uint8_t {
  StopRequested,
  AlreadyStopping,
  Idle,
};

struct TaskInvalidationResult {
  TaskInvalidationStatus status{TaskInvalidationStatus::Idle};
  std::optional<TaskGeneration> generation_to_stop;
};

enum class StopConfirmationStatus : std::uint8_t {
  Stopped,
  ReplacementStarted,
  IgnoredStale,
  GenerationOverflow,
  DeadlineOverflow,
};

struct StopConfirmationResult {
  StopConfirmationStatus status{StopConfirmationStatus::IgnoredStale};
  std::optional<TaskGeneration> started_generation;
};

struct ScheduledRequestToken {
  TaskGeneration generation{};
  std::uint64_t sequence{};
  auto operator<=>(const ScheduledRequestToken &) const = default;
};

struct ScheduledSend {
  ScheduledRequestToken token{};
  std::shared_ptr<const QuickSendExecution> execution;
};

enum class AutomaticStopReason : std::uint8_t {
  DeadlineOverflow,
  SequenceOverflow,
};

struct AutomaticStopRequest {
  TaskGeneration generation{};
  AutomaticStopReason reason{AutomaticStopReason::DeadlineOverflow};
  auto operator<=>(const AutomaticStopRequest &) const = default;
};

struct TxBoundary {
  std::optional<ScheduledRequestToken> completed_request;
  bool completed_successfully{};
  bool manual_pending{};
};

struct SchedulerSnapshot {
  SchedulerState state{SchedulerState::Idle};
  std::optional<TaskGeneration> generation;
  std::uint32_t slot_index{};
  std::uint64_t interval_ms{};
  std::uint64_t sent_count{};
  std::uint64_t missed_count{};
  bool trigger_pending{};
  bool outstanding{};
  bool operator==(const SchedulerSnapshot &) const = default;
};

} // namespace lazycom::scheduler
