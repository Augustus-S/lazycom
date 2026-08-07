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

class GlobalMemoryBudget {
public:
  explicit GlobalMemoryBudget(BudgetLimits limits = BudgetLimits::defaults());

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

// The reservation is acquired before payload or shared ownership allocation.
[[nodiscard]] SharedPayloadPtr
make_shared_payload(GlobalMemoryBudget &budget, BudgetCategory category,
                    std::span<const std::byte> bytes);

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
