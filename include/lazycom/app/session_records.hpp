#pragma once
#include <deque>
#include <lazycom/base/error.hpp>
#include <lazycom/config/types.hpp>
#include <lazycom/framing/rx_framer.hpp>
#include <lazycom/model/session_sequencer.hpp>
#include <memory>

namespace lazycom::app {
enum class RecordDirection : std::uint8_t { Rx, Tx, System, Error };

struct VisibleRecord {
  std::uint64_t record_id{};
  std::uint64_t sequence{};
  RecordDirection direction{RecordDirection::System};
  std::string_view time_utc;
  std::span<const std::byte> payload;
  std::string_view message;
  std::optional<ErrorCode> error_code;
  std::optional<OperationId> operation_id;
  // Retains the immutable storage borrowed by the presentation fields above.
  model::SessionRecordPtr owner;
  std::size_t logical_bytes{};
  model::BudgetReservation reservation{};
};

struct DirectionFilter {
  bool rx{true};
  bool tx{true};
  bool system{true};
  bool error{true};
};

[[nodiscard]] bool direction_visible(RecordDirection direction,
                                     const DirectionFilter &filter) noexcept;
[[nodiscard]] std::string
project_record(const VisibleRecord &record,
               const config::ReceiveSettings &settings);

/** Main-thread owner of framing, stable record IDs, and record-granular
 * eviction. */
class SessionRecords {
public:
  explicit SessionRecords(model::GlobalMemoryBudget &budget)
      : budget_(budget) {}
  void start(const config::ReceiveSettings &settings,
             std::chrono::steady_clock::time_point observed_at,
             std::chrono::system_clock::time_point time_utc);
  void finish() noexcept;
  [[nodiscard]] bool active() const noexcept { return bool(framer_); }
  [[nodiscard]] std::vector<framing::RxFrame>
  push(std::span<const std::byte> bytes,
       std::chrono::steady_clock::time_point observed_at,
       std::chrono::system_clock::time_point time_utc);
  [[nodiscard]] std::vector<framing::RxFrame>
  on_idle(std::chrono::steady_clock::time_point now);
  [[nodiscard]] std::vector<framing::RxFrame> flush();
  [[nodiscard]] model::SequenceResult
  append(const config::ReceiveSettings &limits, SessionEventOrigin origin,
         RecordDirection direction, std::span<const std::byte> payload,
         std::string message, std::optional<ErrorCode> error_code,
         std::optional<OperationId> operation,
         std::chrono::steady_clock::time_point observed_at,
         std::chrono::system_clock::time_point time_utc,
         std::optional<config::SendMode> input_mode);
  [[nodiscard]] std::vector<std::uint64_t>
  search(std::string_view query, DirectionFilter filter,
         const config::ReceiveSettings &settings) const;
  [[nodiscard]] const std::deque<VisibleRecord> &visible() const noexcept {
    return visible_;
  }
  [[nodiscard]] std::uint64_t last_sequence() const noexcept {
    return sequencer_ ? sequencer_->next_seq() - 1U : 0U;
  }
  [[nodiscard]] const std::string &started_at_utc() const noexcept {
    return session_started_utc_;
  }
  [[nodiscard]] std::size_t display_bytes() const noexcept {
    return display_bytes_;
  }
  [[nodiscard]] std::uint64_t gap_records() const noexcept {
    return display_gap_records_;
  }
  void account_rx(std::size_t bytes) noexcept { rx_bytes_ += bytes; }
  void account_tx(std::size_t bytes) noexcept { tx_bytes_ += bytes; }
  [[nodiscard]] std::uint64_t rx_bytes() const noexcept { return rx_bytes_; }
  [[nodiscard]] std::uint64_t tx_bytes() const noexcept { return tx_bytes_; }
  void clear();

private:
  void evict_oldest();
  model::GlobalMemoryBudget &budget_;
  std::unique_ptr<model::SessionSequencer> sequencer_;
  std::unique_ptr<framing::RxFramer> framer_;
  std::deque<VisibleRecord> visible_;
  std::uint64_t next_record_id_{1U};
  std::chrono::steady_clock::time_point session_started_{};
  std::string session_started_utc_;
  std::size_t display_bytes_{};
  std::uint64_t display_gap_records_{};
  std::uint64_t rx_bytes_{};
  std::uint64_t tx_bytes_{};
};

} // namespace lazycom::app
