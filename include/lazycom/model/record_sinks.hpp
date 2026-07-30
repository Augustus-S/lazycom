#pragma once

#include <lazycom/model/session_sequencer.hpp>

#include <algorithm>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

namespace lazycom::model {

class BoundedRecordSink final : public IRecordSink {
public:
  BoundedRecordSink(std::size_t max_batches, std::size_t max_logical_bytes)
      : queue_(max_batches, max_logical_bytes) {}

  [[nodiscard]] QueuePushResult
  push(const SinkEnvelope &envelope,
       const std::size_t logical_bytes) override {
    return queue_.push(envelope, logical_bytes);
  }
  [[nodiscard]] std::optional<BoundedQueue<SinkEnvelope>::Value> try_pop() {
    return queue_.try_pop();
  }
  [[nodiscard]] std::optional<BoundedQueue<SinkEnvelope>::Value> wait_pop() {
    return queue_.wait_pop();
  }
  void stop() noexcept { queue_.stop(); }

private:
  BoundedQueue<SinkEnvelope> queue_;
};

// UI overload keeps newest data. Whole immutable batches are evicted, and the
// removed sequence range is attached to the earliest remaining batch.
class EvictingUiRecordSink final : public IRecordSink {
public:
  EvictingUiRecordSink(const std::size_t max_records,
                       const std::size_t max_logical_bytes)
      : max_records_(max_records), max_logical_bytes_(max_logical_bytes) {}

  [[nodiscard]] QueuePushResult
  push(const SinkEnvelope &envelope,
       const std::size_t) override {
    if (!envelope.batch) {
      return QueuePushResult::Full;
    }
    SinkEnvelope stored = envelope;
    const auto incoming_records = stored.batch->records().size();
    const auto logical_bytes = stored.batch->logical_bytes();
    std::lock_guard lock{mutex_};
    if (stopped_) {
      return QueuePushResult::Stopped;
    }
    if (incoming_records > max_records_ ||
        logical_bytes > max_logical_bytes_) {
      return QueuePushResult::Full;
    }
    while (!queue_.empty() &&
           (records_ > max_records_ - incoming_records ||
            bytes_ > max_logical_bytes_ - logical_bytes)) {
      auto evicted_gap = queue_.front().gap_before;
      merge(evicted_gap, SequenceGap{queue_.front().batch->first_seq(),
                                     queue_.front().batch->last_seq()});
      records_ -= queue_.front().batch->records().size();
      bytes_ -= queue_.front().batch->logical_bytes();
      queue_.pop_front();
      if (queue_.empty()) {
        merge(stored.gap_before, evicted_gap);
      } else {
        merge(queue_.front().gap_before, evicted_gap);
      }
    }
    queue_.push_back(std::move(stored));
    records_ += incoming_records;
    bytes_ += logical_bytes;
    return QueuePushResult::Accepted;
  }

  [[nodiscard]] std::optional<SinkEnvelope> try_pop() {
    std::lock_guard lock{mutex_};
    if (queue_.empty()) {
      return std::nullopt;
    }
    auto result = std::move(queue_.front());
    queue_.pop_front();
    records_ -= result.batch->records().size();
    bytes_ -= result.batch->logical_bytes();
    return result;
  }

  void stop() noexcept {
    std::lock_guard lock{mutex_};
    stopped_ = true;
  }
  [[nodiscard]] std::size_t record_count() const noexcept {
    std::lock_guard lock{mutex_};
    return records_;
  }
  [[nodiscard]] std::size_t logical_bytes() const noexcept {
    std::lock_guard lock{mutex_};
    return bytes_;
  }

private:
  static void merge(std::optional<SequenceGap> &destination,
                    const std::optional<SequenceGap> &source) noexcept {
    if (!source) {
      return;
    }
    if (!destination) {
      destination = source;
      return;
    }
    destination->first = std::min(destination->first, source->first);
    destination->last = std::max(destination->last, source->last);
  }

  static void merge(std::optional<SequenceGap> &destination,
                    const SequenceGap source) noexcept {
    merge(destination, std::optional<SequenceGap>{source});
  }

  const std::size_t max_records_;
  const std::size_t max_logical_bytes_;
  mutable std::mutex mutex_;
  std::deque<SinkEnvelope> queue_;
  std::size_t records_{};
  std::size_t bytes_{};
  bool stopped_{};
};

} // namespace lazycom::model
