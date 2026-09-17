#include <lazycom/serial/backend.hpp>

#include <catch2/catch_test_macros.hpp>
#include <libserialport.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <dirent.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace {

using namespace std::chrono_literals;

class UniqueFd {
public:
  explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}

  ~UniqueFd() { reset(); }

  UniqueFd(const UniqueFd &) = delete;
  UniqueFd &operator=(const UniqueFd &) = delete;

  UniqueFd(UniqueFd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

  UniqueFd &operator=(UniqueFd &&other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.fd_, -1));
    }
    return *this;
  }

  [[nodiscard]] int get() const noexcept { return fd_; }

  void reset(int fd = -1) noexcept {
    if (fd_ >= 0) {
      static_cast<void>(::close(fd_));
    }
    fd_ = fd;
  }

private:
  int fd_;
};

class PortGuard {
public:
  explicit PortGuard(sp_port *port) noexcept : port_(port) {}

  ~PortGuard() {
    if (open_) {
      static_cast<void>(sp_close(port_));
    }
    if (port_ != nullptr) {
      sp_free_port(port_);
    }
  }

  PortGuard(const PortGuard &) = delete;
  PortGuard &operator=(const PortGuard &) = delete;

  [[nodiscard]] sp_port *get() const noexcept { return port_; }

  void mark_open() noexcept { open_ = true; }

private:
  sp_port *port_;
  bool open_{false};
};

int wait_for(pollfd *fds, nfds_t count, std::chrono::milliseconds timeout) {
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(timeout);
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - seconds);
  const timespec limit{
      .tv_sec = static_cast<time_t>(seconds.count()),
      .tv_nsec = static_cast<long>(nanoseconds.count()),
  };
  return ::ppoll(fds, count, &limit, nullptr);
}

void drain_eventfd(int fd) {
  std::uint64_t value = 0;
  while (::read(fd, &value, sizeof(value)) ==
         static_cast<ssize_t>(sizeof(value))) {
  }
  REQUIRE(errno == EAGAIN);
}

[[nodiscard]] std::size_t count_open_fds() {
  DIR *const directory = ::opendir("/proc/self/fd");
  if (directory == nullptr) {
    throw std::runtime_error("cannot inspect /proc/self/fd");
  }
  std::size_t count = 0U;
  while (const dirent *const entry = ::readdir(directory)) {
    const std::string_view name{entry->d_name};
    if (name != "." && name != "..") {
      ++count;
    }
  }
  if (::closedir(directory) != 0) {
    throw std::runtime_error("cannot close /proc/self/fd");
  }
  return count;
}

} // namespace

TEST_CASE("libserialport native fd works with ppoll and eventfd",
          "[integration][serial][linux]") {
  UniqueFd master{::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)};
  REQUIRE(master.get() >= 0);
  REQUIRE(::grantpt(master.get()) == 0);
  REQUIRE(::unlockpt(master.get()) == 0);

  std::array<char, 256> slave_path{};
  REQUIRE(::ptsname_r(master.get(), slave_path.data(), slave_path.size()) == 0);

  sp_port *raw_port = nullptr;
  REQUIRE(sp_get_port_by_name(slave_path.data(), &raw_port) == SP_OK);
  PortGuard port{raw_port};
  REQUIRE(sp_open(port.get(), SP_MODE_READ_WRITE) == SP_OK);
  port.mark_open();
  REQUIRE(sp_set_baudrate(port.get(), 115200) == SP_OK);
  REQUIRE(sp_set_bits(port.get(), 8) == SP_OK);
  REQUIRE(sp_set_parity(port.get(), SP_PARITY_NONE) == SP_OK);
  REQUIRE(sp_set_stopbits(port.get(), 1) == SP_OK);
  REQUIRE(sp_set_flowcontrol(port.get(), SP_FLOWCONTROL_NONE) == SP_OK);

  int serial_fd = -1;
  REQUIRE(sp_get_port_handle(port.get(), &serial_fd) == SP_OK);
  REQUIRE(serial_fd >= 0);

  struct stat serial_stat{};
  REQUIRE(::fstat(serial_fd, &serial_stat) == 0);
  REQUIRE(S_ISCHR(serial_stat.st_mode));

  UniqueFd wake_fd{::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
  REQUIRE(wake_fd.get() >= 0);

  SECTION("idle wait blocks and eventfd wakes it") {
    std::array<pollfd, 2> wait_set{{
        {.fd = serial_fd, .events = POLLIN, .revents = 0},
        {.fd = wake_fd.get(), .events = POLLIN, .revents = 0},
    }};
    REQUIRE(wait_for(wait_set.data(), wait_set.size(), 30ms) == 0);

    std::atomic_bool signal_succeeded{false};
    std::jthread signaler([fd = wake_fd.get(), &signal_succeeded] {
      std::this_thread::sleep_for(20ms);
      const std::uint64_t value = 1;
      signal_succeeded.store(::write(fd, &value, sizeof(value)) ==
                                 static_cast<ssize_t>(sizeof(value)),
                             std::memory_order_relaxed);
    });

    REQUIRE(wait_for(wait_set.data(), wait_set.size(), 1s) == 1);
    signaler.join();
    REQUIRE(signal_succeeded.load(std::memory_order_relaxed));
    REQUIRE((wait_set[1].revents & POLLIN) != 0);
    REQUIRE(wait_set[0].revents == 0);
    drain_eventfd(wake_fd.get());
  }

  SECTION("serial data uses libserialport nonblocking APIs") {
    constexpr std::array<std::byte, 4> inbound{
        std::byte{0x00}, std::byte{0x41}, std::byte{0x80}, std::byte{0xff}};
    REQUIRE(::write(master.get(), inbound.data(), inbound.size()) ==
            static_cast<ssize_t>(inbound.size()));

    pollfd readable{.fd = serial_fd, .events = POLLIN, .revents = 0};
    REQUIRE(wait_for(&readable, 1, 1s) == 1);
    REQUIRE((readable.revents & POLLIN) != 0);

    std::array<std::byte, inbound.size()> received{};
    const int read_result =
        sp_nonblocking_read(port.get(), received.data(), received.size());
    REQUIRE(read_result >= 0);
    REQUIRE(static_cast<std::size_t>(read_result) == received.size());
    REQUIRE(received == inbound);

    constexpr std::array<std::byte, 4> outbound{
        std::byte{0xfe}, std::byte{0x42}, std::byte{0x00}, std::byte{0x7f}};
    pollfd writable{.fd = serial_fd, .events = POLLOUT, .revents = 0};
    REQUIRE(wait_for(&writable, 1, 1s) == 1);
    REQUIRE((writable.revents & POLLOUT) != 0);
    const int write_result =
        sp_nonblocking_write(port.get(), outbound.data(), outbound.size());
    REQUIRE(write_result >= 0);
    REQUIRE(static_cast<std::size_t>(write_result) == outbound.size());

    pollfd peer_readable{.fd = master.get(), .events = POLLIN, .revents = 0};
    REQUIRE(wait_for(&peer_readable, 1, 1s) == 1);
    std::array<std::byte, outbound.size()> peer_received{};
    REQUIRE(::read(master.get(), peer_received.data(), peer_received.size()) ==
            static_cast<ssize_t>(peer_received.size()));
    REQUIRE(peer_received == outbound);
  }

  SECTION(
      "blocked TX returns partial writes and EAGAIN without hiding wakeups") {
    const std::array<std::byte, 64 * 1024> chunk{};
    bool saw_partial_write = false;
    bool saw_eagain = false;

    for (std::size_t attempt = 0; attempt < 512; ++attempt) {
      const int written =
          sp_nonblocking_write(port.get(), chunk.data(), chunk.size());
      REQUIRE(written >= 0);
      if (written == 0) {
        saw_eagain = true;
        break;
      }
      if (static_cast<std::size_t>(written) < chunk.size()) {
        saw_partial_write = true;
      }
    }
    REQUIRE(saw_partial_write);
    REQUIRE(saw_eagain);

    std::atomic_bool signal_succeeded{false};
    std::jthread signaler([fd = wake_fd.get(), &signal_succeeded] {
      std::this_thread::sleep_for(20ms);
      const std::uint64_t value = 1;
      signal_succeeded.store(::write(fd, &value, sizeof(value)) ==
                                 static_cast<ssize_t>(sizeof(value)),
                             std::memory_order_relaxed);
    });

    std::array<pollfd, 2> blocked_wait_set{{
        {.fd = serial_fd, .events = POLLIN | POLLOUT, .revents = 0},
        {.fd = wake_fd.get(), .events = POLLIN, .revents = 0},
    }};
    const auto wake_deadline = std::chrono::steady_clock::now() + 1s;
    bool wake_observed = false;
    while (!wake_observed) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= wake_deadline) {
        break;
      }
      const auto timeout =
          std::chrono::ceil<std::chrono::milliseconds>(wake_deadline - now);
      const auto ready =
          wait_for(blocked_wait_set.data(), blocked_wait_set.size(), timeout);
      if (ready < 0 && errno == EINTR) {
        continue;
      }
      REQUIRE(ready >= 0);
      if (ready == 0) {
        break;
      }
      REQUIRE((blocked_wait_set[0].revents & (POLLERR | POLLHUP | POLLNVAL)) ==
              0);
      wake_observed = (blocked_wait_set[1].revents & POLLIN) != 0;
      if (!wake_observed && (blocked_wait_set[0].revents & POLLOUT) != 0) {
        // PTY buffering may restore readiness after the first EAGAIN even
        // before the peer reads. Refill it while still observing the wake fd.
        const int written =
            sp_nonblocking_write(port.get(), chunk.data(), chunk.size());
        REQUIRE(written >= 0);
      }
    }
    signaler.join();
    REQUIRE(signal_succeeded.load(std::memory_order_relaxed));
    REQUIRE(wake_observed);
    drain_eventfd(wake_fd.get());

    std::array<std::byte, 64 * 1024> drain_buffer{};
    while (::read(master.get(), drain_buffer.data(), drain_buffer.size()) > 0) {
    }
    REQUIRE(errno == EAGAIN);

    pollfd recovered{.fd = serial_fd, .events = POLLOUT, .revents = 0};
    REQUIRE(wait_for(&recovered, 1, 1s) == 1);
    REQUIRE((recovered.revents & POLLOUT) != 0);
  }

  SECTION("closing the PTY peer reports hangup") {
    master.reset();
    pollfd disconnected{.fd = serial_fd, .events = POLLIN, .revents = 0};
    REQUIRE(wait_for(&disconnected, 1, 1s) == 1);
    REQUIRE((disconnected.revents & (POLLHUP | POLLERR)) != 0);
  }
}

TEST_CASE("serial permission probe reports effective access",
          "[integration][serial][linux][permission]") {
  UniqueFd master{::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)};
  REQUIRE(master.get() >= 0);
  REQUIRE(::grantpt(master.get()) == 0);
  REQUIRE(::unlockpt(master.get()) == 0);

  std::array<char, 256> slave_path{};
  REQUIRE(::ptsname_r(master.get(), slave_path.data(), slave_path.size()) == 0);
  const auto allowed =
      lazycom::serial::inspect_device_permission(slave_path.data());
  REQUIRE(allowed);
  REQUIRE(allowed->access == lazycom::serial::DeviceAccess::Allowed);

  if (::geteuid() != 0U) {
    REQUIRE(::chmod(slave_path.data(), 0000) == 0);
    const auto denied =
        lazycom::serial::inspect_device_permission(slave_path.data());
    REQUIRE(denied);
    CHECK(denied->access == lazycom::serial::DeviceAccess::PermissionDenied);
    REQUIRE(::chmod(slave_path.data(), S_IRUSR | S_IWUSR) == 0);
  }
}

TEST_CASE("busy libserialport open does not leak its temporary fd",
          "[integration][serial][linux]") {
  UniqueFd master{::posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)};
  REQUIRE(master.get() >= 0);
  REQUIRE(::grantpt(master.get()) == 0);
  REQUIRE(::unlockpt(master.get()) == 0);

  std::array<char, 256> slave_path{};
  REQUIRE(::ptsname_r(master.get(), slave_path.data(), slave_path.size()) == 0);
  UniqueFd lock_holder{
      ::open(slave_path.data(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC)};
  REQUIRE(lock_holder.get() >= 0);
  REQUIRE(::flock(lock_holder.get(), LOCK_EX | LOCK_NB) == 0);

  sp_port *raw_port = nullptr;
  REQUIRE(sp_get_port_by_name(slave_path.data(), &raw_port) == SP_OK);
  PortGuard port{raw_port};
  const auto before = count_open_fds();
  for (std::size_t attempt = 0U; attempt < 16U; ++attempt) {
    REQUIRE(sp_open(port.get(), SP_MODE_READ_WRITE) == SP_ERR_FAIL);
    const int open_error = sp_last_error_code();
    REQUIRE((open_error == EAGAIN || open_error == EWOULDBLOCK));
    int failed_handle = 0;
    REQUIRE(sp_get_port_handle(port.get(), &failed_handle) == SP_OK);
    REQUIRE(failed_handle == -1);
  }
  REQUIRE(count_open_fds() == before);
}
