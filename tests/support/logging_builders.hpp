#pragma once

#include <lazycom/logging/schema.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <vector>

namespace lazycom::test {

[[nodiscard]] inline std::vector<std::byte>
bytes(const std::string_view value) {
  const auto *begin = reinterpret_cast<const std::byte *>(value.data());
  return {begin, begin + value.size()};
}

class LogHeaderBuilder {
public:
  LogHeaderBuilder() {
    value_.started_at = "2026-07-22T04:30:00.001Z";
    value_.session_id = "a1b2c3";
    value_.device.path = bytes("/dev/ttyUSB0");
  }

  [[nodiscard]] logging::Header build() const { return value_; }

private:
  logging::Header value_;
};

class LogRecordBuilder {
public:
  LogRecordBuilder(const std::uint64_t seq, const logging::Direction direction,
                   std::vector<std::byte> payload) {
    value_.seq = seq;
    value_.time_utc = "2026-07-22T04:30:03.442Z";
    value_.elapsed_ns = 3'441'000'000ULL;
    value_.direction = direction;
    value_.payload = std::move(payload);
  }

  [[nodiscard]] logging::Record build() && { return std::move(value_); }

private:
  logging::Record value_;
};

} // namespace lazycom::test
