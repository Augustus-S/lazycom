#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace lazycom::model {

inline constexpr std::size_t kManagedMemoryLimit = 128U * 1024U * 1024U;
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

  const BudgetLimits limits;
  std::array<std::atomic_size_t, budget_category_count> used{};
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
 * Reservations atomically enforce each category limit. Their immutable limits
 * sum to at most 128 MiB, so no second aggregate admission counter is needed.
 * This type accounts only allocations that are explicitly reserved through it;
 * it is not an end-to-end process RSS limit.
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
   * @return A move-only reservation, or nullopt when the category limit
   * would be exceeded. A zero-byte reservation is valid.
   */
  [[nodiscard]] std::optional<BudgetReservation>
  try_reserve(BudgetCategory category, std::size_t bytes) noexcept;
  [[nodiscard]] std::size_t used(BudgetCategory category) const noexcept;
  // Observational sum; concurrent categories need not share one instant.
  [[nodiscard]] std::size_t total_used() const noexcept;
  [[nodiscard]] const BudgetLimits &limits() const noexcept;

private:
  std::shared_ptr<detail::BudgetState> state_;
};

} // namespace lazycom::model
