#pragma once

#include <cstddef>

namespace lazycom::model {
enum class Direction { Rx, Tx, Sys, Err };
enum class InputMode { Text, Hex };
inline constexpr std::size_t kMaxPayloadBytes = 1024U * 1024U;
inline constexpr std::size_t kMaxMessageBytes = 4096U;
} // namespace lazycom::model
