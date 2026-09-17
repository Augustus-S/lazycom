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

[[nodiscard]] inline logging::Header log_header() {
  logging::Header value;
  value.started_at = "2026-07-22T04:30:00.001Z";
  value.session_id = "a1b2c3";
  value.device.path = bytes("/dev/ttyUSB0");
  return value;
}

[[nodiscard]] inline logging::Record
log_record(std::uint64_t seq, logging::Direction direction,
           std::vector<std::byte> payload) {
  logging::Record value;
  value.seq = seq;
  value.time_utc = "2026-07-22T04:30:03.442Z";
  value.elapsed_ns = 3'441'000'000ULL;
  value.direction = direction;
  value.payload = std::move(payload);
  return value;
}

} // namespace lazycom::test
