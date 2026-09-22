#include <lazycom/model/memory_budget.hpp>

#include <stdexcept>
#include <utility>

namespace lazycom::model {
namespace {

[[nodiscard]] constexpr std::size_t index(const BudgetCategory category) {
  return static_cast<std::size_t>(category);
}

[[nodiscard]] bool reserve_atomic(std::atomic_size_t &used,
                                  const std::size_t limit,
                                  const std::size_t bytes) noexcept {
  auto current = used.load(std::memory_order_relaxed);
  while (bytes <= limit && current <= limit - bytes) {
    if (used.compare_exchange_weak(current, current + bytes,
                                   std::memory_order_acq_rel,
                                   std::memory_order_relaxed)) {
      return true;
    }
  }
  return false;
}

} // namespace

BudgetReservation::BudgetReservation(std::shared_ptr<detail::BudgetState> state,
                                     const BudgetCategory category,
                                     const std::size_t bytes) noexcept
    : state_(std::move(state)), category_(category), bytes_(bytes) {}

BudgetReservation::BudgetReservation(BudgetReservation &&other) noexcept
    : state_(std::move(other.state_)), category_(other.category_),
      bytes_(other.bytes_) {
  other.bytes_ = 0U;
}

BudgetReservation &
BudgetReservation::operator=(BudgetReservation &&other) noexcept {
  if (this != &other) {
    reset();
    state_ = std::move(other.state_);
    category_ = other.category_;
    bytes_ = other.bytes_;
    other.bytes_ = 0U;
  }
  return *this;
}

BudgetReservation::~BudgetReservation() { reset(); }

void BudgetReservation::reset() noexcept {
  if (!state_) {
    return;
  }
  state_->used[index(category_)].fetch_sub(bytes_, std::memory_order_acq_rel);
  state_.reset();
  bytes_ = 0U;
}

GlobalMemoryBudget::GlobalMemoryBudget(const BudgetLimits limits) {
  std::size_t sum = 0U;
  for (const auto limit : limits.category) {
    if (limit > kManagedMemoryLimit || sum > kManagedMemoryLimit - limit) {
      throw std::invalid_argument{"managed memory limits exceed 128 MiB"};
    }
    sum += limit;
  }
  state_ = std::make_shared<detail::BudgetState>(limits);
}

std::optional<BudgetReservation>
GlobalMemoryBudget::try_reserve(const BudgetCategory category,
                                const std::size_t bytes) noexcept {
  const auto category_index = index(category);
  if (category_index >= budget_category_count) {
    return std::nullopt;
  }
  if (!reserve_atomic(state_->used[category_index],
                      state_->limits.category[category_index], bytes)) {
    return std::nullopt;
  }
  return BudgetReservation{state_, category, bytes};
}

std::size_t
GlobalMemoryBudget::used(const BudgetCategory category) const noexcept {
  const auto category_index = index(category);
  if (category_index >= budget_category_count) {
    return 0U;
  }
  return state_->used[category_index].load(std::memory_order_acquire);
}

std::size_t GlobalMemoryBudget::total_used() const noexcept {
  std::size_t total = 0U;
  for (const auto &used : state_->used)
    total += used.load(std::memory_order_acquire);
  return total;
}

const BudgetLimits &GlobalMemoryBudget::limits() const noexcept {
  return state_->limits;
}

} // namespace lazycom::model
