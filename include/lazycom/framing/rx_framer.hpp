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
  bool operator==(const RxFrame &) const = default;
};

struct RxFramerConfig {
  std::chrono::milliseconds idle_gap{50};
  std::size_t max_frame_bytes{65'536U};
};

class RxFramer {
public:
  explicit RxFramer(RxFramerConfig config = {});

  [[nodiscard]] std::vector<RxFrame>
  push(std::span<const std::byte> bytes,
       ObservationClock::time_point observed_at);
  [[nodiscard]] std::vector<RxFrame>
  on_idle(ObservationClock::time_point now);
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
               std::vector<RxFrame> &output);

  RxFramerConfig config_;
  std::vector<std::byte> bytes_;
  ObservationClock::time_point first_byte_observed_at_{};
  ObservationClock::time_point last_read_observed_at_{};
  bool has_last_read_{};
  bool pending_cr_{};
};

} // namespace lazycom::framing
