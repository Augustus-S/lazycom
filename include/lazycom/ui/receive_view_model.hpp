#pragma once

#include <lazycom/app/session_records.hpp>

namespace lazycom::ui {

/** Coordinates are filtered before viewport truncation and live for one
 * operation. */
class ReceiveCoordinates {
public:
  ReceiveCoordinates(const std::deque<app::VisibleRecord> &records,
                     app::DirectionFilter filter);
  [[nodiscard]] bool empty() const noexcept { return indices_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return indices_.size(); }
  [[nodiscard]] std::size_t operator[](std::size_t position) const {
    return indices_[position];
  }
  [[nodiscard]] std::uint64_t id(std::size_t position) const;
  [[nodiscard]] std::optional<std::size_t> position(std::uint64_t id) const;
  [[nodiscard]] std::size_t nearest(std::uint64_t id) const;

private:
  const std::deque<app::VisibleRecord> &records_;
  std::vector<std::size_t> indices_;
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
  std::vector<std::uint64_t> matches_;
  std::size_t match_{};
};

} // namespace lazycom::ui
