#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

namespace mediaserver::editor {

// The semantic action behind a selection click. Modifier interpretation lives
// at the panel boundary because only a panel knows whether its rows have a
// meaningful visible order.
enum class SelectionAction : core::u8 {
  Replace,
  Toggle,
  Range,
  Clear,
};

// Shift owns the ordered-range gesture when the surface supplies an order;
// Ctrl remains an exact one-item toggle everywhere else. Ctrl+Shift therefore
// has deterministic Shift/range semantics instead of accidentally toggling the
// range endpoint.
[[nodiscard]] constexpr SelectionAction
selection_action(bool ctrl, bool shift, bool ordered_range) noexcept {
  if (shift && ordered_range) {
    return SelectionAction::Range;
  }
  if (ctrl) {
    return SelectionAction::Toggle;
  }
  return SelectionAction::Replace;
}

// Editor-local scene-node selection. Membership is always sorted and unique,
// while primary remains an independent, selected node for Inspector focus and
// the brighter row/outline treatment. A private range anchor lets repeated
// Shift clicks resize a range without making click order part of membership.
class NodeSelection final {
public:
  [[nodiscard]] bool empty() const noexcept { return members_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return members_.size(); }
  [[nodiscard]] std::span<const doc::NodeId> members() const noexcept {
    return members_;
  }
  [[nodiscard]] doc::NodeId primary() const noexcept { return primary_; }

  [[nodiscard]] bool contains(doc::NodeId node) const noexcept {
    return std::binary_search(members_.begin(), members_.end(), node);
  }

  void clear() noexcept {
    members_.clear();
    primary_ = {};
    range_anchor_ = {};
  }

  void replace(doc::NodeId node) {
    if (node == doc::NodeId{}) {
      clear();
      return;
    }
    members_.assign(1U, node);
    primary_ = node;
    range_anchor_ = node;
  }

  void set_primary(doc::NodeId node) noexcept {
    if (!contains(node)) {
      return;
    }
    primary_ = node;
    range_anchor_ = node;
  }

  void toggle(doc::NodeId node) {
    if (node == doc::NodeId{}) {
      return;
    }
    const auto found = std::lower_bound(members_.begin(), members_.end(), node);
    if (found == members_.end() || *found != node) {
      members_.insert(found, node);
      primary_ = node;
      range_anchor_ = node;
      return;
    }

    const bool removed_primary = primary_ == node;
    const bool removed_anchor = range_anchor_ == node;
    members_.erase(found);
    if (members_.empty()) {
      primary_ = {};
      range_anchor_ = {};
      return;
    }
    if (removed_primary) {
      primary_ = members_.front();
    }
    if (removed_anchor) {
      range_anchor_ = primary_;
    }
  }

  void select_range(doc::NodeId node,
                    std::span<const doc::NodeId> visible_order) {
    if (node == doc::NodeId{}) {
      clear();
      return;
    }
    const auto target =
        std::find(visible_order.begin(), visible_order.end(), node);
    auto anchor =
        std::find(visible_order.begin(), visible_order.end(), range_anchor_);
    if (anchor == visible_order.end()) {
      anchor = std::find(visible_order.begin(), visible_order.end(), primary_);
    }
    if (target == visible_order.end() || anchor == visible_order.end()) {
      replace(node);
      return;
    }

    const auto first = std::min(anchor, target);
    const auto last = std::max(anchor, target);
    std::vector<doc::NodeId> next;
    next.reserve(static_cast<std::size_t>(std::distance(first, last)) + 1U);
    for (auto item = first; item != std::next(last); ++item) {
      if (*item != doc::NodeId{}) {
        next.push_back(*item);
      }
    }
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    if (next.empty()) {
      replace(node);
      return;
    }
    members_ = std::move(next);
    primary_ = node;
    if (!contains(range_anchor_)) {
      range_anchor_ = node;
    }
  }

  void apply(SelectionAction action, doc::NodeId node = {},
             std::span<const doc::NodeId> visible_order = {}) {
    switch (action) {
    case SelectionAction::Replace:
      replace(node);
      return;
    case SelectionAction::Toggle:
      toggle(node);
      return;
    case SelectionAction::Range:
      select_range(node, visible_order);
      return;
    case SelectionAction::Clear:
      clear();
      return;
    }
  }

  template <typename Predicate> bool erase_if(Predicate &&predicate) {
    const std::size_t before = members_.size();
    members_.erase(
        std::remove_if(members_.begin(), members_.end(),
                       std::forward<Predicate>(predicate)),
        members_.end());
    if (members_.empty()) {
      primary_ = {};
      range_anchor_ = {};
    } else {
      if (!contains(primary_)) {
        primary_ = members_.front();
      }
      if (!contains(range_anchor_)) {
        range_anchor_ = primary_;
      }
    }
    return members_.size() != before;
  }

private:
  std::vector<doc::NodeId> members_;
  doc::NodeId primary_{};
  doc::NodeId range_anchor_{};
};

} // namespace mediaserver::editor
