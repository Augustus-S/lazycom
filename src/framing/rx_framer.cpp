#include <lazycom/framing/rx_framer.hpp>

#include <stdexcept>
#include <utility>

namespace lazycom::framing {

RxFramer::RxFramer(const RxFramerConfig config) : config_(config) {
  if (config_.idle_gap.count() < 0 || config_.max_frame_bytes == 0U) {
    throw std::invalid_argument{"invalid RX framer configuration"};
  }
  bytes_.reserve(config_.max_frame_bytes);
}

void RxFramer::emit(std::vector<RxFrame> &output) {
  if (bytes_.empty()) {
    pending_cr_ = false;
    return;
  }
  output.push_back(
      RxFrame{std::move(bytes_), first_byte_observed_at_});
  bytes_.clear();
  bytes_.reserve(config_.max_frame_bytes);
  pending_cr_ = false;
}

void RxFramer::consume(const std::byte byte,
                       const ObservationClock::time_point observed_at,
                       std::vector<RxFrame> &output) {
  if (pending_cr_) {
    if (byte == std::byte{0x0A}) {
      bytes_.push_back(byte);
      if (bytes_.size() == config_.max_frame_bytes) {
        emit(output);
      } else {
        emit(output);
      }
      return;
    }
    emit(output);
  }

  if (bytes_.empty()) {
    first_byte_observed_at_ = observed_at;
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
               const ObservationClock::time_point observed_at) {
  std::vector<RxFrame> output;
  if (bytes.empty()) {
    return output;
  }

  if (has_last_read_ && !bytes_.empty() &&
      observed_at - last_read_observed_at_ >= config_.idle_gap) {
    emit(output);
  }
  for (const auto byte : bytes) {
    consume(byte, observed_at, output);
  }
  last_read_observed_at_ = observed_at;
  has_last_read_ = true;
  return output;
}

std::vector<RxFrame>
RxFramer::on_idle(const ObservationClock::time_point now) {
  std::vector<RxFrame> output;
  if (has_last_read_ && !bytes_.empty() &&
      now - last_read_observed_at_ >= config_.idle_gap) {
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
