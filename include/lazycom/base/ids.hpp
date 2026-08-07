#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <limits>

namespace lazycom {

/** @brief Generation of a connection attempt, distinct from a session ID. */
struct ConnectionGeneration {
  std::uint64_t value{};
  auto operator<=>(const ConnectionGeneration &) const = default;
};

/** @brief Identity of one successfully opened serial session. */
struct SessionId {
  std::uint64_t value{};
  auto operator<=>(const SessionId &) const = default;
};

/** @brief Identity of one admitted asynchronous operation. */
struct OperationId {
  std::uint64_t value{};
  auto operator<=>(const OperationId &) const = default;
};

/** @brief Generation of one scheduled-send task lifetime. */
struct TaskGeneration {
  std::uint64_t value{};
  auto operator<=>(const TaskGeneration &) const = default;
};

/** @brief Generation used to reject stale device-scan results. */
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

/**
 * @brief Advances a strong ID without allowing unsigned wraparound.
 * @param id ID to advance.
 * @return Advanced on success, or Overflow when id is already UINT64_MAX.
 * @post On overflow, id is unchanged.
 * @note ID value zero is conventionally unissued; this helper does not enforce
 * that convention.
 */
template <StrongMonotonicId Id>
[[nodiscard]] constexpr IdIncrementResult increment_id(Id &id) noexcept {
  if (id.value == std::numeric_limits<std::uint64_t>::max()) {
    return IdIncrementResult::Overflow;
  }
  ++id.value;
  return IdIncrementResult::Advanced;
}

/**
 * @brief Issues monotonically increasing values in one strong-ID domain.
 *
 * The sequence is not synchronized. All mutations must be confined to one
 * owner or externally synchronized. Overflow is reported and never wraps.
 */
template <StrongMonotonicId Id> class IdSequence {
public:
  constexpr IdSequence() noexcept = default;
  explicit constexpr IdSequence(const Id last_issued) noexcept
      : last_issued_(last_issued) {}

  /**
   * @brief Issues the successor of the last issued value.
   * @param issued Receives the new ID only when Advanced is returned.
   * @return Advanced or Overflow.
   * @post On overflow, both the sequence and issued are unchanged.
   */
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
