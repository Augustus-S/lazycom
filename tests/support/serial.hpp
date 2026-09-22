#pragma once

#include <catch2/catch_test_macros.hpp>
#include <lazycom/serial/service.hpp>
#include <support/wait.hpp>

#include <array>
#include <cstdlib>
#include <fcntl.h>
#include <iterator>
#include <unistd.h>
#include <vector>

namespace lazycom::test {

class UniqueFd {
public:
  explicit UniqueFd(int fd) noexcept : fd_{fd} {}
  ~UniqueFd() { reset(); }
  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;
  [[nodiscard]] int get() const noexcept { return fd_; }
  void reset() noexcept {
    if (fd_ >= 0)
      static_cast<void>(::close(fd_));
    fd_ = -1;
  }

private:
  int fd_;
};

struct Pty {
  UniqueFd master{::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)};
  std::array<char, 256> slave_path{};
  Pty() {
    REQUIRE(master.get() >= 0);
    REQUIRE(::grantpt(master.get()) == 0);
    REQUIRE(::unlockpt(master.get()) == 0);
    REQUIRE(::ptsname_r(master.get(), slave_path.data(), slave_path.size()) ==
            0);
  }
};

inline std::vector<serial::SerialCompletion> collect_completions(
    serial::SerialService &service, std::size_t count,
    std::chrono::milliseconds timeout = std::chrono::seconds{1}) {
  std::vector<serial::SerialCompletion> result;
  REQUIRE(wait_until(
      [&] {
        auto batch = service.drain_completions();
        result.insert(result.end(), std::make_move_iterator(batch.begin()),
                      std::make_move_iterator(batch.end()));
        return result.size() >= count;
      },
      timeout));
  REQUIRE(result.size() == count);
  return result;
}

} // namespace lazycom::test
