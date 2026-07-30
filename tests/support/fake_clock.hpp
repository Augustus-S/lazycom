#pragma once

#include <chrono>

namespace lazycom::test {

class FakeClock {
public:
  using clock = std::chrono::steady_clock;
  using duration = clock::duration;
  using time_point = clock::time_point;

  [[nodiscard]] time_point now() const noexcept { return now_; }

  void advance(const duration amount) noexcept { now_ += amount; }
  void set(const time_point value) noexcept { now_ = value; }

private:
  time_point now_{};
};

} // namespace lazycom::test
