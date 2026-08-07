#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace lazycom::model {

enum class QueuePushResult { Accepted, Full, Stopped };

/**
 * @brief Thread-safe FIFO with hard message and caller-defined byte limits.
 *
 * logical_bytes is an accounting value supplied by the caller; it is not
 * sizeof(T) and does not reserve from GlobalMemoryBudget. Operations never wait
 * for capacity, although allocation and mutex acquisition may block or throw.
 * stop() is permanent and leaves already queued values available for draining.
 */
template <class T> class BoundedQueue {
public:
  struct Value {
    T value;
    std::size_t logical_bytes{};
  };

  BoundedQueue(const std::size_t max_messages, const std::size_t max_bytes)
      : max_messages_(max_messages), max_bytes_(max_bytes) {}

  /**
   * @brief Attempts to append one value without waiting for queue capacity.
   * @param value Value passed by value and consumed even when admission fails.
   * @param logical_bytes Stable byte charge retained until the value is popped.
   * @return Accepted, Full, or Stopped.
   */
  [[nodiscard]] QueuePushResult push(T value, const std::size_t logical_bytes) {
    std::lock_guard lock{mutex_};
    if (stopped_) {
      return QueuePushResult::Stopped;
    }
    if (queue_.size() >= max_messages_ || logical_bytes > max_bytes_ ||
        bytes_ > max_bytes_ - logical_bytes) {
      return QueuePushResult::Full;
    }
    queue_.push_back(Value{std::move(value), logical_bytes});
    bytes_ += logical_bytes;
    available_.notify_one();
    return QueuePushResult::Accepted;
  }

  [[nodiscard]] std::optional<Value> try_pop() {
    std::lock_guard lock{mutex_};
    return pop_locked();
  }

  /**
   * @brief Waits until a value is available or the stopped queue is drained.
   * @return The next FIFO value, or nullopt after stop and complete draining.
   */
  [[nodiscard]] std::optional<Value> wait_pop() {
    std::unique_lock lock{mutex_};
    available_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
    return pop_locked();
  }

  /** @brief Permanently rejects new pushes and wakes all waiting consumers. */
  void stop() noexcept {
    {
      std::lock_guard lock{mutex_};
      stopped_ = true;
    }
    available_.notify_all();
  }

  [[nodiscard]] bool stopped() const noexcept {
    std::lock_guard lock{mutex_};
    return stopped_;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    std::lock_guard lock{mutex_};
    return queue_.size();
  }

  [[nodiscard]] std::size_t logical_bytes() const noexcept {
    std::lock_guard lock{mutex_};
    return bytes_;
  }

private:
  [[nodiscard]] std::optional<Value> pop_locked() {
    if (queue_.empty()) {
      return std::nullopt;
    }
    auto result = std::move(queue_.front());
    queue_.pop_front();
    bytes_ -= result.logical_bytes;
    return result;
  }

  const std::size_t max_messages_;
  const std::size_t max_bytes_;
  mutable std::mutex mutex_;
  std::condition_variable available_;
  std::deque<Value> queue_;
  std::size_t bytes_{};
  bool stopped_{};
};

} // namespace lazycom::model
