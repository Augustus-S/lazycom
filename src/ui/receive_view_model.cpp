#include <lazycom/ui/receive_view_model.hpp>

#include <algorithm>
#include <ranges>

namespace lazycom::ui {

ReceiveCoordinates::ReceiveCoordinates(
    const std::deque<app::VisibleRecord> &records, app::DirectionFilter filter)
    : records_{records} {
  indices_.reserve(records.size());
  for (std::size_t index = 0U; index < records.size(); ++index) {
    if (app::direction_visible(records[index].direction, filter)) {
      indices_.push_back(index);
    }
  }
}
std::uint64_t ReceiveCoordinates::id(std::size_t position) const {
  return records_[indices_[position]].record_id;
}
std::optional<std::size_t>
ReceiveCoordinates::position(std::uint64_t target) const {
  if (empty()) {
    return std::nullopt;
  }
  const auto found = nearest(target);
  return id(found) == target ? std::optional{found} : std::nullopt;
}
std::size_t ReceiveCoordinates::nearest(std::uint64_t target) const {
  const auto next =
      std::ranges::lower_bound(indices_, target, {}, [this](std::size_t index) {
        return records_[index].record_id;
      });
  if (next == indices_.begin()) {
    return 0U;
  }
  if (next == indices_.end()) {
    return size() - 1U;
  }
  const auto found =
      static_cast<std::size_t>(std::distance(indices_.begin(), next));
  return target - id(found - 1U) < id(found) - target ? found - 1U : found;
}

void ReceiveViewModel::normalize_viewport(const ReceiveCoordinates &coordinates,
                                          bool follow) {
  if (coordinates.empty()) {
    anchor_.reset();
    at_bottom_ = true;
    return;
  }
  anchor(coordinates, follow || !anchor_ ? coordinates.size() - 1U
                                         : coordinates.nearest(*anchor_));
}
void ReceiveViewModel::select(const ReceiveCoordinates &coordinates,
                              std::size_t position) {
  if (coordinates.empty()) {
    cursor_.reset();
    return;
  }
  position = std::min(position, coordinates.size() - 1U);
  cursor_ = coordinates.id(position);
  anchor(coordinates, position);
}
void ReceiveViewModel::anchor(const ReceiveCoordinates &coordinates,
                              std::size_t position) {
  if (coordinates.empty()) {
    anchor_.reset();
    at_bottom_ = true;
    return;
  }
  position = std::min(position, coordinates.size() - 1U);
  anchor_ = coordinates.id(position);
  at_bottom_ = position + 1U == coordinates.size();
}
void ReceiveViewModel::normalize_cursor(const ReceiveCoordinates &coordinates,
                                        bool prefer_latest) {
  if (coordinates.empty()) {
    cursor_.reset();
    anchor_.reset();
    at_bottom_ = true;
    return;
  }
  if (cursor_) {
    select(coordinates, coordinates.nearest(*cursor_));
  } else if (prefer_latest || !anchor_) {
    select(coordinates, coordinates.size() - 1U);
  } else {
    select(coordinates, coordinates.nearest(*anchor_));
  }
}
void ReceiveViewModel::move(const ReceiveCoordinates &coordinates, bool down,
                            std::size_t count) {
  normalize_cursor(coordinates, true);
  if (!cursor_) {
    return;
  }
  const auto position = coordinates.nearest(*cursor_);
  select(coordinates,
         down ? std::min(coordinates.size() - 1U,
                         position + std::min(count, coordinates.size()))
              : position - std::min(position, count));
}
std::pair<std::size_t, std::size_t>
ReceiveViewModel::viewport(const ReceiveCoordinates &coordinates,
                           std::size_t maximum_rows, bool browsing) const {
  auto end = coordinates.empty() ? 0U
             : anchor_           ? coordinates.nearest(*anchor_) + 1U
                                 : coordinates.size();
  auto begin = end - std::min(end, maximum_rows);
  if (browsing && cursor_) {
    if (const auto position = coordinates.position(*cursor_);
        position && (*position < begin || *position >= end)) {
      end = *position + 1U;
      begin = end - std::min(end, maximum_rows);
    }
  }
  return {begin, end};
}

void ReceiveViewModel::set_matches(std::vector<std::uint64_t> matches) {
  matches_ = std::move(matches);
  match_ = 0U;
}
void ReceiveViewModel::next_match(bool backwards) {
  if (matches_.empty()) {
    return;
  }
  match_ = backwards ? (match_ == 0U ? matches_.size() - 1U : match_ - 1U)
                     : (match_ + 1U) % matches_.size();
}
bool ReceiveViewModel::scroll_to_match(const ReceiveCoordinates &coordinates) {
  if (match_ >= matches_.size()) {
    return false;
  }
  const auto position = coordinates.position(matches_[match_]);
  if (!position) {
    return false;
  }
  anchor(coordinates, *position);
  return true;
}
void ReceiveViewModel::prune_matches(
    const std::deque<app::VisibleRecord> &records,
    const ReceiveCoordinates &coordinates) {
  if (matches_.empty()) {
    return;
  }
  if (records.empty()) {
    set_matches({});
    return;
  }
  const auto selected = matches_[std::min(match_, matches_.size() - 1U)];
  const auto first = records.front().record_id;
  const auto last = records.back().record_id;
  std::erase_if(matches_, [first, last](std::uint64_t id) {
    return id < first || id > last;
  });
  if (matches_.empty()) {
    match_ = 0U;
    return;
  }
  const auto retained = std::ranges::find(matches_, selected);
  if (retained == matches_.end()) {
    match_ = 0U;
    static_cast<void>(scroll_to_match(coordinates));
  } else {
    match_ =
        static_cast<std::size_t>(std::distance(matches_.begin(), retained));
  }
}

} // namespace lazycom::ui
