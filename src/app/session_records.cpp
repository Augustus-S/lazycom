#include <array>
#include <ctime>
#include <lazycom/app/session_records.hpp>
#include <lazycom/base/text.hpp>
#include <lazycom/encoding/display.hpp>
#include <limits>

namespace lazycom::app {
namespace {
[[nodiscard]] std::string
format_utc(std::chrono::system_clock::time_point now) {
  const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
  const auto milliseconds =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - seconds);
  const std::time_t raw = std::chrono::system_clock::to_time_t(seconds);
  std::tm value{};
  if (::gmtime_r(&raw, &value) == nullptr) {
    return "1970-01-01T00:00:00.000Z";
  }
  std::array<char, 32> output{};
  const auto length =
      std::strftime(output.data(), output.size(), "%Y-%m-%dT%H:%M:%S", &value);
  if (length == 0U) {
    return "1970-01-01T00:00:00.000Z";
  }
  return std::string{output.data(), length} + "." +
         (milliseconds.count() < 100 ? "0" : "") +
         (milliseconds.count() < 10 ? "0" : "") +
         std::to_string(milliseconds.count()) + "Z";
}

[[nodiscard]] model::Direction model_direction(RecordDirection direction) {
  switch (direction) {
  case RecordDirection::Rx:
    return model::Direction::Rx;
  case RecordDirection::Tx:
    return model::Direction::Tx;
  case RecordDirection::System:
    return model::Direction::Sys;
  case RecordDirection::Error:
    return model::Direction::Err;
  }
  return model::Direction::Err;
}

} // namespace

[[nodiscard]] bool direction_visible(const RecordDirection direction,
                                     const DirectionFilter &filter) noexcept {
  switch (direction) {
  case RecordDirection::Rx:
    return filter.rx;
  case RecordDirection::Tx:
    return filter.tx;
  case RecordDirection::System:
    return filter.system;
  case RecordDirection::Error:
    return filter.error;
  }
  return false;
}

void SessionRecords::start(const config::ReceiveSettings &settings,
                           std::chrono::steady_clock::time_point observed_at,
                           std::chrono::system_clock::time_point time_utc) {
  session_started_ = observed_at;
  session_started_utc_ = format_utc(time_utc);
  sequencer_ = std::make_unique<model::SessionSequencer>(budget_);
  framer_ = std::make_unique<framing::RxFramer>(
      framing::RxFramerConfig{std::chrono::milliseconds{settings.idle_gap_ms},
                              settings.max_frame_bytes});
}
void SessionRecords::finish() noexcept {
  framer_.reset();
  if (sequencer_)
    static_cast<void>(sequencer_->close());
}
std::vector<framing::RxFrame>
SessionRecords::push(std::span<const std::byte> bytes,
                     std::chrono::steady_clock::time_point observed_at,
                     std::chrono::system_clock::time_point time_utc) {
  return framer_ ? framer_->push(bytes, observed_at, time_utc)
                 : std::vector<framing::RxFrame>{};
}
std::vector<framing::RxFrame>
SessionRecords::on_idle(std::chrono::steady_clock::time_point now) {
  return framer_ ? framer_->on_idle(now) : std::vector<framing::RxFrame>{};
}
std::vector<framing::RxFrame> SessionRecords::flush() {
  return framer_ ? framer_->flush() : std::vector<framing::RxFrame>{};
}
model::SequenceResult SessionRecords::append(
    const config::ReceiveSettings &limits, const SessionEventOrigin origin,
    const RecordDirection direction, std::span<const std::byte> payload,
    std::string message, std::optional<ErrorCode> error_code,
    std::optional<OperationId> operation,
    std::chrono::steady_clock::time_point observed_at,
    std::chrono::system_clock::time_point time_utc,
    std::optional<config::SendMode> input_mode) {
  model::SequenceResult result;
  if (!sequencer_ || !framer_)
    return result;
  if (next_record_id_ == std::numeric_limits<std::uint64_t>::max()) {
    result.status = model::SequenceStatus::SequenceOverflow;
    return result;
  }
  if (origin == SessionEventOrigin::Cleanup) {
    static_cast<void>(sequencer_->begin_cleanup());
  }
  model::RecordDraft draft;
  draft.direction = model_direction(direction);
  draft.time = {format_utc(time_utc),
                observed_at <= session_started_
                    ? 0U
                    : static_cast<std::uint64_t>(
                          std::chrono::duration_cast<std::chrono::nanoseconds>(
                              observed_at - session_started_)
                              .count())};
  draft.message = std::move(message);
  draft.operation_id = operation;
  if (direction == RecordDirection::Tx) {
    draft.input_mode = input_mode == config::SendMode::Txt
                           ? model::InputMode::Text
                           : model::InputMode::Hex;
  }
  if (direction == RecordDirection::Error) {
    draft.code =
        std::string{error_descriptor(
                        error_code.value_or(ErrorCode::InternalInvariantBroken))
                        .identifier};
  }
  draft.payload = payload;
  result = sequencer_->submit(origin, draft);
  while (result.status == model::SequenceStatus::BudgetExhausted &&
         !visible_.empty()) {
    evict_oldest();
    result = sequencer_->submit(origin, draft);
  }
  if (result.status == model::SequenceStatus::BudgetExhausted) {
    ++display_gap_records_;
  }
  if (result.status != model::SequenceStatus::Accepted)
    return result;
  const auto &record = *result.record;
  VisibleRecord visible{
      next_record_id_++, record.seq(),
      direction,         record.time().time_utc,
      record.payload(),  record.message(),
      error_code,        operation,
      result.record,     sizeof(VisibleRecord) + record.logical_bytes()};
  const auto maximum_bytes =
      static_cast<std::size_t>(limits.visible_buffer_mib) * 1024U * 1024U;
  if (visible.logical_bytes > maximum_bytes) {
    ++display_gap_records_;
  } else {
    while (!visible_.empty() &&
           (visible_.size() >= limits.visible_max_records ||
            display_bytes_ > maximum_bytes - visible.logical_bytes)) {
      evict_oldest();
    }
    auto handle = budget_.try_reserve(model::BudgetCategory::UiRecords,
                                      sizeof(VisibleRecord) + 128U);
    while (!handle && !visible_.empty()) {
      evict_oldest();
      handle = budget_.try_reserve(model::BudgetCategory::UiRecords,
                                   sizeof(VisibleRecord) + 128U);
    }
    if (handle) {
      visible.reservation = std::move(*handle);
      visible_.push_back(std::move(visible));
      display_bytes_ += visible_.back().logical_bytes;
    } else {
      ++display_gap_records_;
    }
  }
  return result;
}

void SessionRecords::evict_oldest() {
  display_bytes_ -= visible_.front().logical_bytes;
  visible_.pop_front();
  ++display_gap_records_;
}

void SessionRecords::clear() {
  visible_.clear();
  display_bytes_ = 0U;
  display_gap_records_ = 0U;
}

std::string project_record(const VisibleRecord &record,
                           const config::ReceiveSettings &settings) {
  std::string prefix;
  switch (record.direction) {
  case RecordDirection::Rx:
    prefix = "<- RX ";
    break;
  case RecordDirection::Tx:
    prefix = "-> TX ";
    break;
  case RecordDirection::System:
    prefix = "!  SYS ";
    break;
  case RecordDirection::Error:
    prefix = "x  ERR ";
    break;
  }
  prefix += "[";
  prefix += record.time_utc;
  prefix += "] ";
  if (record.direction == RecordDirection::System ||
      record.direction == RecordDirection::Error) {
    return prefix + lazycom::sanitize_message(record.message);
  }
  const auto view = record.direction == RecordDirection::Tx ? settings.tx_view
                                                            : settings.rx_view;
  encoding::DisplayMode mode = encoding::DisplayMode::Text;
  if (view == config::ReceiveView::Hex) {
    mode = encoding::DisplayMode::Hex;
  } else if (view == config::ReceiveView::Mixed) {
    mode = encoding::DisplayMode::Mixed;
  }
  return prefix + encoding::render(record.payload, mode);
}

std::vector<std::uint64_t>
SessionRecords::search(const std::string_view query,
                       const DirectionFilter filter,
                       const config::ReceiveSettings &settings) const {
  std::vector<std::uint64_t> result;
  if (query.empty()) {
    return result;
  }
  for (std::size_t index = 0U;
       index < visible_.size() && result.size() < 10'000U; ++index) {
    if (direction_visible(visible_[index].direction, filter) &&
        project_record(visible_[index], settings).find(query) !=
            std::string::npos) {
      result.push_back(visible_[index].record_id);
    }
  }
  return result;
}

} // namespace lazycom::app
