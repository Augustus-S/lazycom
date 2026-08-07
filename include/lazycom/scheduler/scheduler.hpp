#pragma once

#include <lazycom/base/ids.hpp>
#include <lazycom/config/schema.hpp>

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

// Session is resolved before bytes are returned. Any failure returns no bytes.
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

// slot_index is the user-facing 1..20 index. The result owns a snapshot and is
// therefore unaffected by later edits to the persisted slots.
[[nodiscard]] QuickSendBuildResult
make_quick_send_execution(const config::QuickSendSnapshot &slots,
                          std::uint32_t slot_index,
                          config::Newline session_newline);

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
};

class Scheduler {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  static_assert(Clock::is_steady);

  explicit Scheduler(TaskGeneration last_issued = {},
                     std::uint64_t first_request_sequence = 1U) noexcept;

  [[nodiscard]] static constexpr bool
  valid_interval(const std::uint64_t interval_ms) noexcept {
    return interval_ms == 0U || (interval_ms >= kMinimumPeriodicIntervalMs &&
                                 interval_ms <= kMaximumPeriodicIntervalMs);
  }

  [[nodiscard]] TaskStartResult start(TaskRequest request, TimePoint now,
                                      bool replacement_confirmed = false);
  [[nodiscard]] TaskInvalidationResult request_stop() noexcept;
  [[nodiscard]] TaskInvalidationResult invalidate_for_disconnect() noexcept;
  [[nodiscard]] TaskInvalidationResult invalidate_for_shutdown() noexcept;
  [[nodiscard]] StopConfirmationResult
  confirm_stopped(TaskGeneration generation, TimePoint now);

  // The owner calls on_deadline after its monotonic wait expires, then calls
  // on_tx_boundary whenever no logical TX request is being partially written.
  void on_deadline(TimePoint now) noexcept;
  [[nodiscard]] std::optional<ScheduledSend>
  on_tx_boundary(TimePoint now, const TxBoundary &boundary = {});

  [[nodiscard]] std::optional<TimePoint> next_deadline() const noexcept;
  // Returned once when an internal exhaustion condition requires the owner to
  // stop this generation at its TX boundary.
  [[nodiscard]] std::optional<AutomaticStopRequest>
  take_automatic_stop_request() noexcept;
  [[nodiscard]] SchedulerSnapshot snapshot() const noexcept;
  [[nodiscard]] bool accepts(TaskGeneration generation) const noexcept;

private:
  [[nodiscard]] bool
  valid_execution(const QuickSendExecution &execution) const noexcept;
  [[nodiscard]] TaskStartResult activate(TaskRequest request, TimePoint now);
  [[nodiscard]] TaskInvalidationResult
  invalidate(bool discard_replacement) noexcept;
  [[nodiscard]] std::optional<ScheduledSend> emit_pending();
  void begin_automatic_stop(AutomaticStopReason reason) noexcept;
  void add_missed(std::uint64_t count) noexcept;

  IdSequence<TaskGeneration> generations_;
  SchedulerState state_{SchedulerState::Idle};
  std::optional<TaskGeneration> generation_;
  std::shared_ptr<const QuickSendExecution> execution_;
  std::uint64_t interval_ms_{};
  std::optional<TimePoint> next_deadline_;
  std::optional<ScheduledRequestToken> outstanding_;
  std::optional<TaskRequest> pending_replacement_;
  std::optional<AutomaticStopRequest> automatic_stop_request_;
  std::uint64_t first_request_sequence_{1U};
  std::uint64_t next_sequence_{1U};
  std::uint64_t sent_count_{};
  std::uint64_t missed_count_{};
  bool trigger_pending_{};
};

} // namespace lazycom::scheduler
