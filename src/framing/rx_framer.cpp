#include <lazycom/framing/rx_framer.hpp>

#include <chrono>
#include <stdexcept>
#include <utility>

namespace lazycom::framing {
namespace {

[[nodiscard]] bool
idle_gap_elapsed(const ObservationClock::time_point now,
                 const ObservationClock::time_point last_observed,
                 const std::chrono::milliseconds idle_gap) noexcept {
  if (now < last_observed) {
    return false;
  }
  const auto gap =
      std::chrono::duration_cast<ObservationClock::duration>(idle_gap);
  if (last_observed > ObservationClock::time_point::max() - gap) {
    return false;
  }
  return now >= last_observed + gap;
}

} // namespace

RxFramer::RxFramer(const RxFramerConfig config) : config_(config) {
  const auto maximum_idle_gap =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          ObservationClock::duration::max());
  if (config_.idle_gap.count() < 0 || config_.idle_gap > maximum_idle_gap ||
      config_.max_frame_bytes == 0U) {
    throw std::invalid_argument{"invalid RX framer configuration"};
  }
}

void RxFramer::emit(std::vector<RxFrame> &output) {
  if (bytes_.empty()) {
    pending_cr_ = false;
    return;
  }
  output.push_back(RxFrame{std::move(bytes_), first_byte_observed_at_,
                           first_byte_observed_utc_});
  bytes_ = std::vector<std::byte>{};
  pending_cr_ = false;
}

void RxFramer::consume(const std::byte byte,
                       const ObservationClock::time_point observed_at,
                       const std::chrono::system_clock::time_point observed_utc,
                       std::vector<RxFrame> &output) {
  if (pending_cr_) {
    if (byte == std::byte{0x0A}) {
      bytes_.push_back(byte);
      emit(output);
      return;
    }
    emit(output);
  }

  if (bytes_.empty()) {
    first_byte_observed_at_ = observed_at;
    first_byte_observed_utc_ = observed_utc;
  }
  bytes_.push_back(byte);

  // The size limit wins over delimiter lookahead, including CRLF lookahead.
  if (bytes_.size() == config_.max_frame_bytes) {
    emit(output);
    return;
  }
  if (byte == std::byte{0x0A}) {
    emit(output);
  } else if (byte == std::byte{0x0D}) {
    pending_cr_ = true;
  }
}

std::vector<RxFrame>
RxFramer::push(const std::span<const std::byte> bytes,
               const ObservationClock::time_point observed_at,
               const std::chrono::system_clock::time_point observed_utc) {
  std::vector<RxFrame> output;
  if (bytes.empty()) {
    return output;
  }

  if (has_last_read_ && !bytes_.empty() &&
      idle_gap_elapsed(observed_at, last_read_observed_at_, config_.idle_gap)) {
    emit(output);
  }
  for (const auto byte : bytes) {
    consume(byte, observed_at, observed_utc, output);
  }
  last_read_observed_at_ = observed_at;
  has_last_read_ = true;
  return output;
}

std::vector<RxFrame> RxFramer::on_idle(const ObservationClock::time_point now) {
  std::vector<RxFrame> output;
  if (has_last_read_ && !bytes_.empty() &&
      idle_gap_elapsed(now, last_read_observed_at_, config_.idle_gap)) {
    emit(output);
  }
  return output;
}

std::vector<RxFrame> RxFramer::flush() {
  std::vector<RxFrame> output;
  emit(output);
  has_last_read_ = false;
  return output;
}

} // namespace lazycom::framing
