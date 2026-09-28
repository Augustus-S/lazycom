#include <lazycom/ui/receive_view_model.hpp>

#include <algorithm>
#include <chrono>
#include <ranges>

namespace lazycom::ui {

ReceiveCoordinates::ReceiveCoordinates(
    const std::deque<app::VisibleRecord> &records, app::DirectionFilter filter,
    model::GlobalMemoryBudget budget)
    : records_{records}, budget_{std::move(budget)} {
  refresh(filter);
}

void ReceiveCoordinates::refresh(app::DirectionFilter filter) {
  const auto first = records_.empty() ? 0U : records_.front().record_id;
  const auto last = records_.empty() ? 0U : records_.back().record_id;
  if (initialized_ && filter == filter_ && first == first_id_ &&
      last == last_id_ && records_.size() == record_count_) {
    return;
  }
  bool append_only = initialized_ && indices_ && filter == filter_ &&
                     first == first_id_ && last >= last_id_ &&
                     records_.size() >= record_count_;
  unfiltered_ = filter.rx && filter.tx && filter.system && filter.error;
  if (unfiltered_ || records_.empty()) {
    indices_.reset();
    reservation_.reset();
    capacity_ = 0U;
  } else if (records_.size() > capacity_) {
    indices_.reset();
    reservation_.reset();
    capacity_ = 0U;
    append_only = false;
    constexpr std::size_t maximum_cached_records = 1'000'000U;
    if (records_.size() <= maximum_cached_records) {
      const auto capacity = std::min(
          maximum_cached_records, records_.size() + records_.size() / 2U + 1U);
      auto reserved = budget_.try_reserve(model::BudgetCategory::UiRecords,
                                          capacity * sizeof(std::size_t));
      if (reserved) {
        auto storage = std::make_unique_for_overwrite<std::size_t[]>(capacity);
        indices_ = std::move(storage);
        reservation_ = std::move(*reserved);
        capacity_ = capacity;
      }
    }
  }
  const auto begin = append_only ? record_count_ : 0U;
  if (!append_only) {
    size_ = 0U;
  }
  if (unfiltered_) {
    size_ = records_.size();
  } else {
    for (auto index = begin; index < records_.size(); ++index) {
      if (app::direction_visible(records_[index].direction, filter)) {
        if (indices_) {
          indices_[size_] = index;
        }
        ++size_;
      }
    }
  }
  filter_ = filter;
  first_id_ = first;
  last_id_ = last;
  record_count_ = records_.size();
  scan_position_ = 0U;
  scan_index_ = 0U;
  initialized_ = true;
}

std::size_t ReceiveCoordinates::operator[](std::size_t position) const {
  if (unfiltered_) {
    return position;
  }
  if (indices_) {
    return indices_[position];
  }
  // Under budget pressure, scan without allocating or hiding records. Keep
  // forward viewport traversal linear even when no index can be admitted.
  if (position < scan_position_) {
    scan_position_ = 0U;
    scan_index_ = 0U;
  }
  while (scan_index_ < records_.size()) {
    if (app::direction_visible(records_[scan_index_].direction, filter_)) {
      if (scan_position_ == position) {
        return scan_index_;
      }
      ++scan_position_;
    }
    ++scan_index_;
  }
  return records_.size();
}

std::uint64_t ReceiveCoordinates::id(std::size_t position) const {
  return records_[(*this)[position]].record_id;
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
  std::size_t begin = 0U;
  auto end = size();
  while (begin < end) {
    const auto middle = begin + (end - begin) / 2U;
    if (id(middle) < target) {
      begin = middle + 1U;
    } else {
      end = middle;
    }
  }
  if (begin == 0U) {
    return 0U;
  }
  if (begin == size()) {
    return size() - 1U;
  }
  return target - id(begin - 1U) < id(begin) - target ? begin - 1U : begin;
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
  std::string{}.swap(query_);
  search_reservation_.reset();
  searching_ = false;
  match_ = 0U;
}

bool ReceiveViewModel::begin_search(
    std::string_view query, app::DirectionFilter filter,
    const config::ReceiveSettings &settings,
    const std::deque<app::VisibleRecord> &records,
    model::GlobalMemoryBudget &budget) {
  set_matches({});
  if (query.empty() || records.empty()) {
    return true;
  }
  constexpr std::size_t maximum_matches = 10'000U;
  if (query.size() > 4096U) {
    return false;
  }
  auto reserved = budget.try_reserve(
      model::BudgetCategory::Model,
      2U * (maximum_matches * sizeof(std::uint64_t) + query.size() + 1U));
  if (!reserved) {
    return false;
  }
  search_reservation_ = std::move(*reserved);
  matches_.reserve(maximum_matches);
  query_ = query;
  search_filter_ = filter;
  search_settings_ = settings;
  search_next_ = records.front().record_id;
  search_last_ = records.back().record_id;
  searching_ = true;
  return true;
}

void ReceiveViewModel::advance_search(
    const std::deque<app::VisibleRecord> &records) {
  if (!searching_) {
    return;
  }
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds{2};
  auto record = std::ranges::lower_bound(records, search_next_, {},
                                         &app::VisibleRecord::record_id);
  std::size_t visited = 0U;
  std::size_t bytes = 0U;
  while (record != records.end() && record->record_id <= search_last_) {
    const auto record_bytes = record->payload.size() + record->message.size();
    if (visited != 0U && record_bytes > 512U * 1024U - bytes) {
      return;
    }
    if (app::direction_visible(record->direction, search_filter_) &&
        app::project_record(*record, search_settings_).find(query_) !=
            std::string::npos) {
      matches_.push_back(record->record_id);
    }
    if (record->record_id == search_last_ || matches_.size() == 10'000U) {
      searching_ = false;
      return;
    }
    search_next_ = record->record_id + 1U;
    bytes += record_bytes;
    ++record;
    if (++visited == 256U || bytes >= 512U * 1024U ||
        std::chrono::steady_clock::now() >= deadline) {
      return;
    }
  }
  searching_ = false;
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
  const auto first = records.front().record_id;
  const auto last = records.back().record_id;
  if (matches_.front() >= first && matches_.back() <= last) {
    return;
  }
  const auto selected = matches_[std::min(match_, matches_.size() - 1U)];
  const auto end = std::ranges::upper_bound(matches_, last);
  matches_.erase(end, matches_.end());
  const auto begin = std::ranges::lower_bound(matches_, first);
  matches_.erase(matches_.begin(), begin);
  if (matches_.empty()) {
    match_ = 0U;
    return;
  }
  const auto retained = std::ranges::lower_bound(matches_, selected);
  if (retained == matches_.end() || *retained != selected) {
    match_ = 0U;
    static_cast<void>(scroll_to_match(coordinates));
  } else {
    match_ =
        static_cast<std::size_t>(std::distance(matches_.begin(), retained));
  }
}

} // namespace lazycom::ui
