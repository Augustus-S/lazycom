#pragma once

#include <chrono>
#include <cstddef>
#include <span>
#include <vector>

namespace lazycom::framing {

using ObservationClock = std::chrono::steady_clock;

struct RxFrame {
  std::vector<std::byte> bytes;
  ObservationClock::time_point first_byte_observed_at{};
  std::chrono::system_clock::time_point first_byte_observed_utc{};
  bool operator==(const RxFrame &) const = default;
};

struct RxFramerConfig {
  std::chrono::milliseconds idle_gap{50};
  std::size_t max_frame_bytes{65'536U};
};

/**
 * @brief Owner-confined incremental RX delimiter and idle-gap framer.
 *
 * LF, CR, and CRLF terminate frames and remain in emitted bytes. The configured
 * byte limit is enforced immediately, so no frame exceeds max_frame_bytes. The
 * class retains input bytes but never retains caller spans.
 *
 * Instances are not synchronized and should remain confined to the serial data
 * owner.
 */
class RxFramer {
public:
  /**
   * @brief Constructs a framer with class-level limits.
   * @throws std::invalid_argument if idle_gap is negative or unrepresentable,
   * or max_frame_bytes is zero.
   * @note Product configuration applies narrower validated ranges.
   */
  explicit RxFramer(RxFramerConfig config = {});

  /**
   * @brief Appends bytes observed at one monotonic time point.
   * @param bytes Bytes to copy into the framer.
   * @param observed_at Observation time shared by this chunk.
   * @param observed_utc Paired UTC observation; defaults to epoch for models.
   * @return Frames completed by a prior idle gap, delimiters, or size limits.
   * Empty input is a no-op and does not trigger idle processing.
   */
  [[nodiscard]] std::vector<RxFrame>
  push(std::span<const std::byte> bytes,
       ObservationClock::time_point observed_at,
       std::chrono::system_clock::time_point observed_utc = {});
  /**
   * @brief Emits a pending frame when the inclusive idle deadline has elapsed.
   */
  [[nodiscard]] std::vector<RxFrame> on_idle(ObservationClock::time_point now);
  /** @brief Emits all pending bytes and resets read timing state. */
  [[nodiscard]] std::vector<RxFrame> flush();

  [[nodiscard]] bool empty() const noexcept { return bytes_.empty(); }
  [[nodiscard]] bool has_pending_cr() const noexcept { return pending_cr_; }
  [[nodiscard]] std::size_t buffered_bytes() const noexcept {
    return bytes_.size();
  }
  [[nodiscard]] std::chrono::milliseconds idle_gap() const noexcept {
    return config_.idle_gap;
  }
  [[nodiscard]] std::size_t max_frame_bytes() const noexcept {
    return config_.max_frame_bytes;
  }

private:
  void emit(std::vector<RxFrame> &output);
  void consume(std::byte byte, ObservationClock::time_point observed_at,
               std::chrono::system_clock::time_point observed_utc,
               std::vector<RxFrame> &output);

  RxFramerConfig config_;
  std::vector<std::byte> bytes_;
  ObservationClock::time_point first_byte_observed_at_{};
  std::chrono::system_clock::time_point first_byte_observed_utc_{};
  ObservationClock::time_point last_read_observed_at_{};
  bool has_last_read_{};
  bool pending_cr_{};
};

} // namespace lazycom::framing
