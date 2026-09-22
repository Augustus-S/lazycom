#pragma once

#include <lazycom/scheduler/task.hpp>

namespace lazycom::scheduler {

/**
 * @brief Pure single-owner state machine for one-shot and fixed-rate sends.
 *
 * Scheduler owns task state and immutable execution snapshots but creates no
 * thread, timer, queue, or serial resource. It is not synchronized; all calls
 * belong to one logical TX coordinator and supplied time points should be
 * monotonic and nondecreasing. Generation, request-sequence, and deadline
 * arithmetic report exhaustion instead of wrapping.
 */
class Scheduler {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  static_assert(Clock::is_steady);

  explicit Scheduler(TaskGeneration last_issued = {},
                     std::uint64_t first_request_sequence = 1U) noexcept;

  /**
   * @brief Starts a task or begins confirmed stop-and-replace.
   *
   * Interval zero is one-shot; periodic intervals are 10 through 86400000 ms.
   * Starting while Running requires confirmation, then returns the old
   * generation whose TX boundary must be stopped before replacement activates.
   * @param request Owned execution snapshot and interval.
   * @param now Current monotonic time.
   * @param replacement_confirmed Whether an active task may be replaced.
   */
  [[nodiscard]] TaskStartResult start(TaskRequest request, TimePoint now,
                                      bool replacement_confirmed = false);
  /** @brief Invalidates the running generation and suppresses new triggers. */
  [[nodiscard]] TaskInvalidationResult request_stop() noexcept;
  /** @brief Invalidates for disconnect and discards a pending replacement. */
  [[nodiscard]] TaskInvalidationResult invalidate_for_disconnect() noexcept;
  /** @brief Invalidates for shutdown and discards a pending replacement. */
  [[nodiscard]] TaskInvalidationResult invalidate_for_shutdown() noexcept;
  /**
   * @brief Applies an exact owner stop confirmation and activates replacement.
   * @return IgnoredStale without mutation for a nonmatching generation.
   */
  [[nodiscard]] StopConfirmationResult
  confirm_stopped(TaskGeneration generation, TimePoint now);

  /**
   * @brief Advances fixed-rate deadlines and records missed periods.
   * @param writer_busy Whether accepted TX occupies the writer at this
   * observation.
   * @note At most one trigger and one TX request can be outstanding.
   */
  void on_deadline(TimePoint now, bool writer_busy = false) noexcept;
  /**
   * @brief Applies a completed TX boundary and emits at most one scheduled
   * send.
   * @note manual_pending suppresses scheduled emission for this boundary.
   */
  [[nodiscard]] std::optional<ScheduledSend>
  on_tx_boundary(TimePoint now, const TxBoundary &boundary = {});

  [[nodiscard]] std::optional<TimePoint> next_deadline() const noexcept;
  /**
   * @brief Consumes the one-shot request to stop after internal exhaustion.
   */
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
