#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace lazycom::model {

inline constexpr std::size_t kManagedMemoryLimit = 128U * 1024U * 1024U;
// Conservatively covers SharedPayload, its vector object/allocation metadata,
// and the separate shared_ptr control block allocation.
inline constexpr std::size_t kSharedPayloadFixedBudgetBytes = 128U;

enum class BudgetCategory : std::uint8_t {
  UiRecords,
  SessionLog,
  RxIngress,
  Tx,
  TxHistory,
  Diagnostics,
  OwnerScratch,
  Model,
  Count,
};

inline constexpr std::size_t budget_category_count =
    static_cast<std::size_t>(BudgetCategory::Count);

struct BudgetLimits {
  std::array<std::size_t, budget_category_count> category{};

  [[nodiscard]] static constexpr BudgetLimits defaults() noexcept {
    constexpr std::size_t mib = 1024U * 1024U;
    return BudgetLimits{{48U * mib, 16U * mib, 8U * mib, 8U * mib, 16U * mib,
                         8U * mib, 16U * mib, 8U * mib}};
  }
};

namespace detail {
struct BudgetState {
  explicit BudgetState(BudgetLimits configured) : limits(configured) {}

  BudgetLimits limits;
  std::array<std::atomic_size_t, budget_category_count> used{};
  std::atomic_size_t total{};
};
} // namespace detail

class BudgetReservation {
public:
  BudgetReservation() noexcept = default;
  BudgetReservation(const BudgetReservation &) = delete;
  BudgetReservation &operator=(const BudgetReservation &) = delete;
  BudgetReservation(BudgetReservation &&other) noexcept;
  BudgetReservation &operator=(BudgetReservation &&other) noexcept;
  ~BudgetReservation();

  [[nodiscard]] std::size_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] BudgetCategory category() const noexcept { return category_; }
  [[nodiscard]] explicit operator bool() const noexcept {
    return static_cast<bool>(state_);
  }
  void reset() noexcept;

private:
  friend class GlobalMemoryBudget;
  BudgetReservation(std::shared_ptr<detail::BudgetState> state,
                    BudgetCategory category, std::size_t bytes) noexcept;

  std::shared_ptr<detail::BudgetState> state_;
  BudgetCategory category_{BudgetCategory::Model};
  std::size_t bytes_{};
};

/**
 * @brief Shared, thread-safe accounting for bounded managed-memory categories.
 *
 * Reservations atomically enforce both the selected category limit and the
 * fixed 128 MiB aggregate limit. This type accounts only allocations that are
 * explicitly reserved through it; it is not an end-to-end process RSS limit.
 */
class GlobalMemoryBudget {
public:
  /**
   * @throws std::invalid_argument if any category or their sum exceeds the
   * managed-memory hard limit.
   */
  explicit GlobalMemoryBudget(BudgetLimits limits = BudgetLimits::defaults());

  /**
   * @brief Attempts to reserve bytes until the returned token is released.
   * @return A move-only reservation, or nullopt when a category or total limit
   * would be exceeded. A zero-byte reservation is valid.
   */
  [[nodiscard]] std::optional<BudgetReservation>
  try_reserve(BudgetCategory category, std::size_t bytes) noexcept;
  [[nodiscard]] std::size_t used(BudgetCategory category) const noexcept;
  [[nodiscard]] std::size_t total_used() const noexcept;
  [[nodiscard]] const BudgetLimits &limits() const noexcept;

private:
  std::shared_ptr<detail::BudgetState> state_;
};

class SharedPayload;
using SharedPayloadPtr = std::shared_ptr<const SharedPayload>;

/** @brief Immutable payload whose budget charge follows shared ownership. */
class SharedPayload {
public:
  [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
    return bytes_;
  }
  [[nodiscard]] std::size_t budget_bytes() const noexcept {
    return reservation_.bytes();
  }

private:
  friend SharedPayloadPtr make_shared_payload(GlobalMemoryBudget &,
                                              BudgetCategory,
                                              std::span<const std::byte>);
  SharedPayload(std::span<const std::byte> bytes,
                BudgetReservation reservation);

  std::vector<std::byte> bytes_;
  BudgetReservation reservation_;
};

static_assert(kSharedPayloadFixedBudgetBytes >=
              sizeof(SharedPayload) + 4U * sizeof(void *));

/**
 * @brief Copies bytes into an immutable shared payload after reserving budget.
 * @return Shared ownership, or an empty pointer when size arithmetic or budget
 * admission fails.
 * @note The reservation is acquired before payload and control-block
 * allocation. Standard allocation failure may still throw.
 */
[[nodiscard]] SharedPayloadPtr
make_shared_payload(GlobalMemoryBudget &budget, BudgetCategory category,
                    std::span<const std::byte> bytes);

/**
 * @brief Immutable range retaining ownership of its underlying shared payload.
 * @throws std::out_of_range if an explicit range is outside a non-null payload,
 * or if an explicit range is constructed from a null payload.
 */
class ByteSlice {
public:
  ByteSlice() noexcept = default;
  explicit ByteSlice(SharedPayloadPtr payload);
  ByteSlice(SharedPayloadPtr payload, std::size_t offset, std::size_t length);

  [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
  [[nodiscard]] const SharedPayloadPtr &payload() const noexcept {
    return payload_;
  }
  [[nodiscard]] std::size_t offset() const noexcept { return offset_; }
  [[nodiscard]] std::size_t size() const noexcept { return length_; }
  [[nodiscard]] bool empty() const noexcept { return length_ == 0U; }

private:
  SharedPayloadPtr payload_;
  std::size_t offset_{};
  std::size_t length_{};
};

} // namespace lazycom::model
