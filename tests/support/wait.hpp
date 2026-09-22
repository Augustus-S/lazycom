#pragma once

#include <chrono>
#include <thread>

namespace lazycom::test {

inline bool wait_until(const auto &predicate,
                       std::chrono::milliseconds timeout = std::chrono::seconds{
                           1}) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline)
      return false;
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  }
  return true;
}

} // namespace lazycom::test
