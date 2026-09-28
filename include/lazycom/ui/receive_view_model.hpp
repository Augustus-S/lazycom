#pragma once

#include <lazycom/app/session_records.hpp>

namespace lazycom::ui {

/** Main-thread filtered coordinates. Refresh after changing the borrowed
 * record deque and before using its coordinates. */
class ReceiveCoordinates {
public:
  ReceiveCoordinates(
      const std::deque<app::VisibleRecord> &records,
      app::DirectionFilter filter,
      model::GlobalMemoryBudget budget = model::GlobalMemoryBudget{});
  void refresh(app::DirectionFilter filter);
  [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t operator[](std::size_t position) const;
  [[nodiscard]] std::uint64_t id(std::size_t position) const;
  [[nodiscard]] std::optional<std::size_t> position(std::uint64_t id) const;
  [[nodiscard]] std::size_t nearest(std::uint64_t id) const;

private:
  const std::deque<app::VisibleRecord> &records_;
  model::GlobalMemoryBudget budget_;
  model::BudgetReservation reservation_;
  std::unique_ptr<std::size_t[]> indices_;
  std::size_t capacity_{};
  std::size_t size_{};
  std::size_t record_count_{};
  std::uint64_t first_id_{};
  std::uint64_t last_id_{};
  app::DirectionFilter filter_{};
  bool initialized_{};
  bool unfiltered_{};
  mutable std::size_t scan_position_{};
  mutable std::size_t scan_index_{};
};

class ReceiveViewModel {
public:
  void normalize_viewport(const ReceiveCoordinates &coordinates,
                          bool follow = false);
  void normalize_cursor(const ReceiveCoordinates &coordinates,
                        bool prefer_latest = false);
  void select(const ReceiveCoordinates &coordinates, std::size_t position);
  void move(const ReceiveCoordinates &coordinates, bool down,
            std::size_t count);
  void anchor(const ReceiveCoordinates &coordinates, std::size_t position);
  [[nodiscard]] std::pair<std::size_t, std::size_t>
  viewport(const ReceiveCoordinates &coordinates, std::size_t maximum_rows,
           bool browsing) const;
  [[nodiscard]] std::optional<std::uint64_t> cursor() const noexcept {
    return cursor_;
  }
  [[nodiscard]] std::optional<std::uint64_t> anchor() const noexcept {
    return anchor_;
  }
  [[nodiscard]] bool at_bottom() const noexcept { return at_bottom_; }

  void set_matches(std::vector<std::uint64_t> matches);
  /** Captures the query, display modes and current record-ID upper bound.
   * Returns false if its bounded storage cannot be admitted. */
  [[nodiscard]] bool begin_search(std::string_view query,
                                  app::DirectionFilter filter,
                                  const config::ReceiveSettings &settings,
                                  const std::deque<app::VisibleRecord> &records,
                                  model::GlobalMemoryBudget &budget);
  /** Advances at most 256 records, yielding at record boundaries at 512 KiB
   * or 2 ms. One record may exceed either threshold. Appends are excluded. */
  void advance_search(const std::deque<app::VisibleRecord> &records);
  [[nodiscard]] bool searching() const noexcept { return searching_; }
  void next_match(bool backwards);
  void prune_matches(const std::deque<app::VisibleRecord> &records,
                     const ReceiveCoordinates &coordinates);
  [[nodiscard]] bool scroll_to_match(const ReceiveCoordinates &coordinates);
  [[nodiscard]] std::size_t match_count() const noexcept {
    return matches_.size();
  }

private:
  std::optional<std::uint64_t> anchor_;
  std::optional<std::uint64_t> cursor_;
  bool at_bottom_{true};
  model::BudgetReservation search_reservation_;
  std::vector<std::uint64_t> matches_;
  std::size_t match_{};
  std::string query_;
  app::DirectionFilter search_filter_;
  config::ReceiveSettings search_settings_;
  std::uint64_t search_next_{};
  std::uint64_t search_last_{};
  bool searching_{};
};

} // namespace lazycom::ui
