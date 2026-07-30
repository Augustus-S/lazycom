#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <limits>

namespace lazycom {

struct ConnectionGeneration {
  std::uint64_t value{};
  auto operator<=>(const ConnectionGeneration &) const = default;
};

struct SessionId {
  std::uint64_t value{};
  auto operator<=>(const SessionId &) const = default;
};

struct OperationId {
  std::uint64_t value{};
  auto operator<=>(const OperationId &) const = default;
};

struct TaskGeneration {
  std::uint64_t value{};
  auto operator<=>(const TaskGeneration &) const = default;
};

struct ScanGeneration {
  std::uint64_t value{};
  auto operator<=>(const ScanGeneration &) const = default;
};

enum class IdIncrementResult : std::uint8_t {
  Advanced,
  Overflow,
};

template <class Id>
concept StrongMonotonicId = requires(Id id) {
  { id.value } -> std::same_as<std::uint64_t &>;
};

template <StrongMonotonicId Id>
[[nodiscard]] constexpr IdIncrementResult increment_id(Id &id) noexcept {
  if (id.value == std::numeric_limits<std::uint64_t>::max()) {
    return IdIncrementResult::Overflow;
  }
  ++id.value;
  return IdIncrementResult::Advanced;
}

template <StrongMonotonicId Id> class IdSequence {
public:
  constexpr IdSequence() noexcept = default;
  explicit constexpr IdSequence(const Id last_issued) noexcept
      : last_issued_(last_issued) {}

  [[nodiscard]] constexpr IdIncrementResult issue(Id &issued) noexcept {
    const auto result = increment_id(last_issued_);
    if (result == IdIncrementResult::Advanced) {
      issued = last_issued_;
    }
    return result;
  }

  [[nodiscard]] constexpr Id last_issued() const noexcept {
    return last_issued_;
  }

private:
  Id last_issued_{};
};

} // namespace lazycom
