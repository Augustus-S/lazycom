#include <lazycom/model/memory_budget.hpp>

#include <limits>
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
  state_->total.fetch_sub(bytes_, std::memory_order_acq_rel);
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
  if (!reserve_atomic(state_->total, kManagedMemoryLimit, bytes)) {
    return std::nullopt;
  }
  if (!reserve_atomic(state_->used[category_index],
                      state_->limits.category[category_index], bytes)) {
    state_->total.fetch_sub(bytes, std::memory_order_acq_rel);
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
  return state_->total.load(std::memory_order_acquire);
}

const BudgetLimits &GlobalMemoryBudget::limits() const noexcept {
  return state_->limits;
}

SharedPayload::SharedPayload(const std::span<const std::byte> bytes,
                             BudgetReservation reservation)
    : bytes_(bytes.begin(), bytes.end()), reservation_(std::move(reservation)) {
}

SharedPayloadPtr make_shared_payload(GlobalMemoryBudget &budget,
                                     const BudgetCategory category,
                                     const std::span<const std::byte> bytes) {
  if (bytes.size() > std::numeric_limits<std::size_t>::max() -
                         kSharedPayloadFixedBudgetBytes) {
    return {};
  }
  const auto budget_bytes = bytes.size() + kSharedPayloadFixedBudgetBytes;
  auto reservation = budget.try_reserve(category, budget_bytes);
  if (!reservation) {
    return {};
  }
  return SharedPayloadPtr{new SharedPayload{bytes, std::move(*reservation)}};
}

ByteSlice::ByteSlice(SharedPayloadPtr payload) : payload_(std::move(payload)) {
  if (payload_) {
    length_ = payload_->bytes().size();
  }
}

ByteSlice::ByteSlice(SharedPayloadPtr payload, const std::size_t offset,
                     const std::size_t length)
    : payload_(std::move(payload)), offset_(offset), length_(length) {
  if (!payload_ || offset_ > payload_->bytes().size() ||
      length_ > payload_->bytes().size() - offset_) {
    throw std::out_of_range{"payload slice is out of range"};
  }
}

std::span<const std::byte> ByteSlice::bytes() const noexcept {
  if (!payload_) {
    return {};
  }
  return payload_->bytes().subspan(offset_, length_);
}

} // namespace lazycom::model
