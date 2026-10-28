#include <mediaserver/gui/dock.hpp>

#include <mediaserver/gui/rect_cut.hpp>

#include <algorithm>
#include <cmath>
#include <set>

namespace mediaserver::gui {
namespace {

// Identities for the parts of the dock chrome. They are mixed with a constant
// so a seam and a panel that happen to share a number cannot fight over the
// pointer.
[[nodiscard]] UiId seam_id(DockId node) noexcept {
    return ui_id("dock.seam") ^ (static_cast<UiId>(node) * 0x9E3779B97F4A7C15ULL);
}

[[nodiscard]] UiId tab_id(PanelId panel) noexcept {
    return panel ^ ui_id("dock.tab");
}

[[nodiscard]] UiId tab_close_id(PanelId panel) noexcept {
    return panel ^ ui_id("dock.tab.close");
}

[[nodiscard]] UiId title_id(DockId node) noexcept {
    return ui_id("dock.title") ^ (static_cast<UiId>(node) * 0xD1B54A32D192ED03ULL);
}

[[nodiscard]] UiId resize_id(DockId node) noexcept {
    return ui_id("dock.resize") ^
           (static_cast<UiId>(node) * 0x94D049BB133111EBULL);
}

[[nodiscard]] UiId frame_close_id(DockId node) noexcept {
    return ui_id("dock.frame.close") ^
           (static_cast<UiId>(node) * 0xDB4F0B9175AE2165ULL);
}

// Far enough that the operator meant to drag rather than click.
constexpr float drag_threshold = 5.0F;
constexpr float tab_close_extent = 20.0F;
constexpr float floating_grip_size = 18.0F;
// The band along a floating frame's right and bottom edges that resizes it.
constexpr float floating_edge_reach = 5.0F;

[[nodiscard]] UiId resize_edge_id(DockId node, bool right) noexcept {
    return ui_id(right ? "dock.resize.right" : "dock.resize.bottom") ^
           (static_cast<UiId>(node) * 0x94D049BB133111EBULL);
}

// One-unit outline, drawn as four rules so it never covers what it frames.
[[nodiscard]] UiColor mixed(UiColor a, UiColor b, float t) noexcept {
    return UiColor{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                   a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

[[nodiscard]] Rect floating_title_bounds(
    const DockLayout::Floating& frame, const DockMetrics& metrics) noexcept {
    Rect rest = frame.frame;
    return cut_top(rest, metrics.title_bar);
}

// A square in the title bar's right end, 3px in from its edges.
[[nodiscard]] Rect floating_close_bounds(
    const DockLayout::Floating& frame, const DockMetrics& metrics) noexcept {
    const Rect corner = inset(floating_title_bounds(frame, metrics), 3.0F);
    Rect lane = corner;
    return cut_right(lane, corner.height);
}

// The title bar left of the close button: the frame's move handle, and
// where its name goes.
[[nodiscard]] Rect floating_title_lane(
    const DockLayout::Floating& frame, const DockMetrics& metrics) noexcept {
    const Rect title = floating_title_bounds(frame, metrics);
    return Rect{title.x, title.y,
                std::max(floating_close_bounds(frame, metrics).x - title.x, 0.0F),
                title.height};
}

[[nodiscard]] Rect floating_resize_bounds(
    const DockLayout::Floating& frame) noexcept {
    Rect corner = frame.frame;
    corner = cut_bottom(corner, floating_grip_size);
    return cut_right(corner, floating_grip_size);
}

[[nodiscard]] Rect floating_right_edge(
    const DockLayout::Floating& frame) noexcept {
    return Rect{frame.frame.x + frame.frame.width - floating_edge_reach,
                frame.frame.y,
                floating_edge_reach * 2.0F,
                std::max(frame.frame.height - floating_grip_size, 0.0F)};
}

[[nodiscard]] Rect floating_bottom_edge(
    const DockLayout::Floating& frame) noexcept {
    return Rect{frame.frame.x,
                frame.frame.y + frame.frame.height - floating_edge_reach,
                std::max(frame.frame.width - floating_grip_size, 0.0F),
                floating_edge_reach * 2.0F};
}

} // namespace

DockSide drop_side(const Rect& node, float x, float y, float edge) noexcept {
    const float band_x = node.width * edge;
    const float band_y = node.height * edge;
    if (band_x <= 0.0F || band_y <= 0.0F) {
        return DockSide::Center;
    }

    // Each edge is scored against its own band, so the closest edge in
    // proportional terms wins a corner rather than the widest one.
    const float scores[] = {
        (x - node.x) / band_x,
        (node.x + node.width - x) / band_x,
        (y - node.y) / band_y,
        (node.y + node.height - y) / band_y,
    };
    constexpr DockSide sides[] = {
        DockSide::Left, DockSide::Right, DockSide::Top, DockSide::Bottom};

    DockSide best = DockSide::Center;
    float best_score = 1.0F;
    for (std::size_t i = 0U; i < 4U; ++i) {
        if (scores[i] >= 0.0F && scores[i] < best_score) {
            best_score = scores[i];
            best = sides[i];
        }
    }
    return best;
}

Rect drop_preview(const Rect& node, DockSide side) noexcept {
    Rect half = node;
    switch (side) {
    case DockSide::Left:
        return cut_left(half, node.width * 0.5F);
    case DockSide::Right:
        return cut_right(half, node.width * 0.5F);
    case DockSide::Top:
        return cut_top(half, node.height * 0.5F);
    case DockSide::Bottom:
        return cut_bottom(half, node.height * 0.5F);
    case DockSide::Center:
        break;
    }
    return node;
}

// ---------------------------------------------------------------- DockLayout

void DockLayout::register_panel(PanelId panel, std::string_view title) {
    if (panel != PanelId{}) {
        titles_[panel] = std::string{title};
    }
}

DockLayout::State DockLayout::snapshot() const {
    State state;
    std::vector<DockId> compact(nodes_.size(), no_dock);
    state.nodes.reserve(nodes_.size() - free_.size());
    for (std::size_t index = 0U; index < nodes_.size(); ++index) {
        if (!nodes_[index].used) {
            continue;
        }
        compact[index] = static_cast<DockId>(state.nodes.size());
        state.nodes.push_back(State::Node{});
    }

    for (std::size_t index = 0U; index < nodes_.size(); ++index) {
        const Node& source = nodes_[index];
        if (!source.used) {
            continue;
        }
        State::Node& target = state.nodes[compact[index]];
        target.split = source.split;
        target.axis = source.axis;
        target.fraction = source.fraction;
        target.first = source.first == no_dock ? no_dock : compact[source.first];
        target.second =
            source.second == no_dock ? no_dock : compact[source.second];
        target.tabs = source.tabs;
        target.active = active_of(static_cast<DockId>(index));
    }

    state.root = root_ == no_dock ? no_dock : compact[root_];
    state.floating.reserve(floating_.size());
    for (const Floating& frame : floating_) {
        state.floating.push_back(State::Floating{compact[frame.node], frame.frame});
    }

    for (const auto& [panel, unused_title] : titles_) {
        static_cast<void>(unused_title);
        if (leaf_of(panel) != no_dock) {
            continue;
        }
        State::Closed closed{};
        closed.panel = panel;
        if (const auto found = restore_.find(panel); found != restore_.end()) {
            closed.has_anchor = found->second.has_anchor;
            closed.anchor = found->second.anchor;
            closed.side = found->second.side;
            closed.fraction = found->second.fraction;
            closed.floating = found->second.floating;
            closed.frame = found->second.frame;
        }
        state.closed.push_back(closed);
    }
    return state;
}

bool DockLayout::restore(const State& state) {
    constexpr std::size_t maximum_nodes = 4'096U;
    constexpr std::size_t maximum_tabs = 4'096U;
    if (state.nodes.size() > maximum_nodes ||
        state.floating.size() > maximum_nodes ||
        state.closed.size() > maximum_tabs ||
        (state.root != no_dock && state.root >= state.nodes.size())) {
        return false;
    }

    const auto valid_side = [](DockSide side) noexcept {
        return side == DockSide::Left || side == DockSide::Right ||
            side == DockSide::Top || side == DockSide::Bottom ||
            side == DockSide::Center;
    };
    const auto valid_rect = [](const Rect& rect) noexcept {
        return std::isfinite(rect.x) && std::isfinite(rect.y) &&
            std::isfinite(rect.width) && std::isfinite(rect.height) &&
            rect.width > 0.0F && rect.height > 0.0F;
    };

    std::vector<std::size_t> parents(state.nodes.size(), 0U);
    std::set<PanelId> represented_panels;
    std::size_t tab_count{};
    for (const State::Node& node : state.nodes) {
        if (node.split) {
            if ((node.axis != Axis::Row && node.axis != Axis::Column) ||
                !std::isfinite(node.fraction) || node.fraction < 0.0F ||
                node.fraction > 1.0F || node.first >= state.nodes.size() ||
                node.second >= state.nodes.size() || node.first == node.second ||
                !node.tabs.empty() || node.active != PanelId{}) {
                return false;
            }
            ++parents[node.first];
            ++parents[node.second];
            continue;
        }
        if (node.first != no_dock || node.second != no_dock ||
            node.tabs.empty() || node.tabs.size() > maximum_tabs - tab_count) {
            return false;
        }
        tab_count += node.tabs.size();
        bool active_found = false;
        for (const PanelId panel : node.tabs) {
            if (panel == PanelId{} || !titles_.contains(panel) ||
                !represented_panels.insert(panel).second) {
                return false;
            }
            active_found = active_found || panel == node.active;
        }
        if (!active_found) {
            return false;
        }
    }

    std::vector<DockId> roots;
    roots.reserve(state.floating.size() + 1U);
    if (state.root != no_dock) {
        roots.push_back(state.root);
    }
    for (const State::Floating& frame : state.floating) {
        if (frame.node >= state.nodes.size() || !valid_rect(frame.frame) ||
            std::find(roots.begin(), roots.end(), frame.node) != roots.end()) {
            return false;
        }
        roots.push_back(frame.node);
    }

    for (const DockId root : roots) {
        if (parents[root] != 0U) {
            return false;
        }
    }
    std::vector<bool> visited(state.nodes.size(), false);
    std::vector<DockId> pending = roots;
    while (!pending.empty()) {
        const DockId current = pending.back();
        pending.pop_back();
        if (visited[current]) {
            return false;
        }
        visited[current] = true;
        const State::Node& node = state.nodes[current];
        if (node.split) {
            pending.push_back(node.first);
            pending.push_back(node.second);
        }
    }
    if (std::find(visited.begin(), visited.end(), false) != visited.end()) {
        return false;
    }

    for (std::size_t index = 0U; index < parents.size(); ++index) {
        const bool top_level =
            std::find(roots.begin(), roots.end(), static_cast<DockId>(index)) !=
            roots.end();
        if (parents[index] != (top_level ? 0U : 1U)) {
            return false;
        }
    }

    std::map<PanelId, Restore> restored_closed;
    for (const State::Closed& closed : state.closed) {
        if (closed.panel == PanelId{} || !titles_.contains(closed.panel) ||
            !represented_panels.insert(closed.panel).second ||
            !valid_side(closed.side) || !std::isfinite(closed.fraction) ||
            closed.fraction < 0.0F || closed.fraction > 1.0F ||
            (closed.has_anchor &&
             (closed.anchor == PanelId{} || !titles_.contains(closed.anchor))) ||
            (closed.floating && !valid_rect(closed.frame))) {
            return false;
        }
        restored_closed.emplace(
            closed.panel,
            Restore{closed.has_anchor, closed.anchor, closed.side,
                    closed.fraction, closed.floating, closed.frame});
    }

    // Preserve panels introduced by this build that were open in its default
    // layout but absent from an older snapshot. Newly registered closed panels
    // stay closed.
    std::vector<PanelId> new_default_panels;
    for (const Node& node : nodes_) {
        if (!node.used || node.split) {
            continue;
        }
        for (const PanelId panel : node.tabs) {
            if (!represented_panels.contains(panel)) {
                new_default_panels.push_back(panel);
            }
        }
    }

    DockLayout candidate;
    candidate.titles_ = titles_;
    candidate.nodes_.resize(state.nodes.size());
    for (std::size_t index = 0U; index < state.nodes.size(); ++index) {
        const State::Node& source = state.nodes[index];
        Node& target = candidate.nodes_[index];
        target.used = true;
        target.split = source.split;
        target.axis = source.axis;
        target.fraction = source.fraction;
        target.first = source.first;
        target.second = source.second;
        target.tabs = source.tabs;
        if (!source.split) {
            const auto active =
                std::find(target.tabs.begin(), target.tabs.end(), source.active);
            target.active = static_cast<std::size_t>(active - target.tabs.begin());
        }
    }
    for (std::size_t index = 0U; index < candidate.nodes_.size(); ++index) {
        Node& node = candidate.nodes_[index];
        if (node.split) {
            candidate.nodes_[node.first].parent = static_cast<DockId>(index);
            candidate.nodes_[node.second].parent = static_cast<DockId>(index);
        }
    }
    candidate.root_ = state.root;
    candidate.floating_.reserve(state.floating.size());
    for (const State::Floating& frame : state.floating) {
        candidate.floating_.push_back(Floating{frame.node, frame.frame});
    }
    candidate.restore_ = std::move(restored_closed);

    for (const PanelId panel : new_default_panels) {
        static_cast<void>(candidate.add(panel, candidate.title_of(panel)));
    }
    *this = std::move(candidate);
    return true;
}

DockId DockLayout::allocate() {
    if (!free_.empty()) {
        const DockId id = free_.back();
        free_.pop_back();
        nodes_[id] = Node{};
        nodes_[id].used = true;
        return id;
    }
    nodes_.push_back(Node{});
    nodes_.back().used = true;
    return static_cast<DockId>(nodes_.size() - 1U);
}

DockId DockLayout::allocate_leaf(PanelId panel) {
    const DockId id = allocate();
    nodes_[id].tabs.push_back(panel);
    nodes_[id].active = 0U;
    return id;
}

void DockLayout::release(DockId id) noexcept {
    if (id >= nodes_.size()) {
        return;
    }
    nodes_[id].used = false;
    nodes_[id].tabs.clear();
    nodes_[id].first = no_dock;
    nodes_[id].second = no_dock;
    nodes_[id].parent = no_dock;
    free_.push_back(id);
}

bool DockLayout::valid(DockId id) const noexcept {
    return id < nodes_.size() && nodes_[id].used;
}

bool DockLayout::is_split(DockId id) const noexcept {
    return valid(id) && nodes_[id].split;
}

void DockLayout::replace_slot(DockId old_node, DockId replacement) noexcept {
    if (!valid(old_node) || !valid(replacement)) {
        return;
    }
    const DockId parent = nodes_[old_node].parent;
    nodes_[replacement].parent = parent;

    if (parent == no_dock) {
        // A root: either the docked tree's, or a floating frame's.
        if (root_ == old_node) {
            root_ = replacement;
            return;
        }
        for (Floating& frame : floating_) {
            if (frame.node == old_node) {
                frame.node = replacement;
                return;
            }
        }
        return;
    }

    Node& owner = nodes_[parent];
    if (owner.first == old_node) {
        owner.first = replacement;
    } else if (owner.second == old_node) {
        owner.second = replacement;
    }
}

void DockLayout::collapse(DockId leaf) noexcept {
    if (!valid(leaf)) {
        return;
    }
    const DockId parent = nodes_[leaf].parent;
    if (parent == no_dock) {
        if (root_ == leaf) {
            root_ = no_dock;
        } else {
            const auto found = std::find_if(
                floating_.begin(), floating_.end(), [leaf](const Floating& f) {
                    return f.node == leaf;
                });
            if (found != floating_.end()) {
                floating_.erase(found);
            }
        }
        release(leaf);
        return;
    }

    // The sibling takes the split's place, and both the empty leaf and the
    // split that held it go away.
    const DockId sibling =
        nodes_[parent].first == leaf ? nodes_[parent].second : nodes_[parent].first;
    replace_slot(parent, sibling);
    release(leaf);
    release(parent);
}

DockId DockLayout::first_leaf(DockId id) const noexcept {
    while (valid(id) && nodes_[id].split) {
        id = nodes_[id].first;
    }
    return valid(id) ? id : no_dock;
}

DockId DockLayout::tree_root(DockId id) const noexcept {
    if (!valid(id)) {
        return no_dock;
    }
    while (nodes_[id].parent != no_dock && valid(nodes_[id].parent)) {
        id = nodes_[id].parent;
    }
    return id;
}

const DockLayout::Floating*
DockLayout::floating_for(DockId id) const noexcept {
    const DockId owner = tree_root(id);
    const auto found = std::find_if(
        floating_.begin(), floating_.end(), [owner](const Floating& frame) {
            return frame.node == owner;
        });
    return found == floating_.end() ? nullptr : &*found;
}

DockId DockLayout::place(
    PanelId panel, DockId target, DockSide side, float fraction) {
    // Nothing to dock against: the panel becomes the tree.
    if (root_ == no_dock && floating_.empty()) {
        root_ = allocate_leaf(panel);
        return root_;
    }
    if (!valid(target)) {
        target = root_ != no_dock ? root_ : floating_.front().node;
    }

    if (side == DockSide::Center) {
        const DockId leaf = nodes_[target].split ? first_leaf(target) : target;
        if (!valid(leaf)) {
            return no_dock;
        }
        nodes_[leaf].tabs.push_back(panel);
        nodes_[leaf].active = nodes_[leaf].tabs.size() - 1U;
        return leaf;
    }

    // Both allocations first: they can reallocate the node storage, so no
    // reference may be held across them.
    const DockId leaf = allocate_leaf(panel);
    const DockId split = allocate();

    const bool leading = side == DockSide::Left || side == DockSide::Top;
    nodes_[split].split = true;
    nodes_[split].axis =
        (side == DockSide::Left || side == DockSide::Right) ? Axis::Row : Axis::Column;
    nodes_[split].first = leading ? leaf : target;
    nodes_[split].second = leading ? target : leaf;
    nodes_[split].fraction = leading ? fraction : 1.0F - fraction;

    replace_slot(target, split);
    nodes_[target].parent = split;
    nodes_[leaf].parent = split;
    return leaf;
}

DockId DockLayout::add(PanelId panel, std::string_view title) {
    register_panel(panel, title);
    restore_.erase(panel);
    const DockId existing = leaf_of(panel);
    if (existing != no_dock) {
        set_active(existing, panel);
        return existing;
    }
    if (root_ == no_dock) {
        root_ = allocate_leaf(panel);
        return root_;
    }
    const DockId leaf = first_leaf(root_);
    if (!valid(leaf)) {
        return no_dock;
    }
    nodes_[leaf].tabs.push_back(panel);
    return leaf;
}

DockId DockLayout::dock(
    PanelId panel,
    std::string_view title,
    DockId target,
    DockSide side,
    float fraction) {
    register_panel(panel, title);
    restore_.erase(panel);
    const DockId existing = leaf_of(panel);
    if (existing != no_dock) {
        set_active(existing, panel);
        return existing;
    }
    return place(panel, target, side, fraction);
}

DockId DockLayout::move(
    PanelId panel, DockId target, DockSide side, float fraction) {
    const DockId from = leaf_of(panel);
    if (from == no_dock) {
        return no_dock;
    }
    // Dropped back where it started, with nothing to rearrange.
    if (from == target
        && (side == DockSide::Center || nodes_[from].tabs.size() <= 1U)) {
        return from;
    }
    remove(panel);
    return place(panel, target, side, fraction);
}

DockId DockLayout::detach(PanelId panel, const Rect& frame) {
    if (leaf_of(panel) == no_dock) {
        return no_dock;
    }
    remove(panel);
    const DockId leaf = allocate_leaf(panel);
    floating_.push_back(Floating{leaf, frame});
    return leaf;
}

void DockLayout::remove(PanelId panel) {
    const DockId leaf = leaf_of(panel);
    if (leaf == no_dock) {
        return;
    }
    auto& tabs = nodes_[leaf].tabs;
    const auto found = std::find(tabs.begin(), tabs.end(), panel);
    if (found == tabs.end()) {
        return;
    }
    const auto index = static_cast<std::size_t>(found - tabs.begin());
    tabs.erase(found);

    if (tabs.empty()) {
        collapse(leaf);
        return;
    }
    // Keep the same tab showing where possible: a removal before the active one
    // shifts it down, and one at the end pulls it back into range.
    if (index < nodes_[leaf].active) {
        --nodes_[leaf].active;
    }
    if (nodes_[leaf].active >= tabs.size()) {
        nodes_[leaf].active = tabs.size() - 1U;
    }
}

bool DockLayout::is_open(PanelId panel) const noexcept {
    return leaf_of(panel) != no_dock;
}

bool DockLayout::has_saved_placement(PanelId panel) const noexcept {
    return restore_.contains(panel);
}

DockId DockLayout::show(PanelId panel) {
    DockId leaf = leaf_of(panel);
    if (leaf != no_dock) {
        set_active(leaf, panel);
        const DockId owner = tree_root(leaf);
        if (floating_for(leaf) != nullptr) {
            raise(owner);
        }
        return leaf;
    }
    if (!titles_.contains(panel)) {
        return no_dock;
    }

    const auto saved = restore_.find(panel);
    if (saved != restore_.end() && saved->second.has_anchor) {
        const DockId target = leaf_of(saved->second.anchor);
        if (target != no_dock) {
            leaf = place(panel, target, saved->second.side,
                         saved->second.fraction);
        }
    }
    if (leaf == no_dock && saved != restore_.end() &&
        saved->second.floating) {
        leaf = allocate_leaf(panel);
        floating_.push_back(Floating{leaf, saved->second.frame});
    }
    if (leaf == no_dock) {
        if (root_ == no_dock) {
            root_ = allocate_leaf(panel);
            leaf = root_;
        } else {
            leaf = place(panel, first_leaf(root_), DockSide::Center, 0.5F);
        }
    }
    restore_.erase(panel);
    return leaf;
}

void DockLayout::show_group(std::span<const PanelId> panels) {
    std::vector<PanelId> pending;
    pending.reserve(panels.size());
    for (const PanelId panel : panels) {
        if (is_open(panel) || !titles_.contains(panel) ||
            std::find(pending.begin(), pending.end(), panel) != pending.end()) {
            continue;
        }
        pending.push_back(panel);
    }

    while (!pending.empty()) {
        bool opened = false;
        for (auto panel = pending.begin(); panel != pending.end();) {
            const auto saved = restore_.find(*panel);
            const bool waits_for_group_anchor =
                saved != restore_.end() && saved->second.has_anchor &&
                !is_open(saved->second.anchor) &&
                std::find(pending.begin(), pending.end(),
                          saved->second.anchor) != pending.end();
            if (waits_for_group_anchor) {
                ++panel;
                continue;
            }
            static_cast<void>(show(*panel));
            panel = pending.erase(panel);
            opened = true;
        }

        // A malformed saved cycle cannot make group opening hang. restore()
        // rejects cycles in persisted trees, but close/show are public model
        // operations and this keeps the lifecycle API defensive on its own.
        if (!opened) {
            static_cast<void>(show(pending.front()));
            pending.erase(pending.begin());
        }
    }
}

void DockLayout::close(PanelId panel) {
    const DockId leaf = leaf_of(panel);
    if (leaf == no_dock) {
        return;
    }

    Restore saved{};
    if (const Floating* const frame = floating_for(leaf); frame != nullptr) {
        saved.floating = true;
        saved.frame = frame->frame;
    }

    const Node& node = nodes_[leaf];
    if (node.tabs.size() > 1U) {
        const auto found = std::find(node.tabs.begin(), node.tabs.end(), panel);
        const std::size_t index =
            static_cast<std::size_t>(found - node.tabs.begin());
        const std::size_t anchor_index =
            index + 1U < node.tabs.size() ? index + 1U : index - 1U;
        saved.has_anchor = true;
        saved.anchor = node.tabs[anchor_index];
        saved.side = DockSide::Center;
    } else if (node.parent != no_dock && is_split(node.parent)) {
        const Node& parent = nodes_[node.parent];
        const bool leading = parent.first == leaf;
        const DockId sibling = leading ? parent.second : parent.first;
        const DockId anchor_leaf = first_leaf(sibling);
        const PanelId anchor = active_of(anchor_leaf);
        if (anchor != PanelId{}) {
            saved.has_anchor = true;
            saved.anchor = anchor;
            saved.side = parent.axis == Axis::Row
                             ? (leading ? DockSide::Left : DockSide::Right)
                             : (leading ? DockSide::Top : DockSide::Bottom);
            saved.fraction = leading ? parent.fraction : 1.0F - parent.fraction;
        }
    }

    restore_[panel] = saved;
    remove(panel);
}

void DockLayout::solve_node(DockId id, const Rect& rect) {
    if (!valid(id)) {
        return;
    }
    nodes_[id].rect = rect;
    if (!nodes_[id].split) {
        leaves_.push_back(id);
        return;
    }

    seams_.push_back(id);
    nodes_[id].fraction = clamp_split(rect, nodes_[id].fraction, nodes_[id].axis);
    const Split panes =
        split_rects(rect, nodes_[id].fraction, nodes_[id].axis, metrics_.divider);
    const DockId first = nodes_[id].first;
    const DockId second = nodes_[id].second;
    solve_node(first, panes.first);
    solve_node(second, panes.second);
}

void DockLayout::solve(const Rect& area, const DockMetrics& metrics) {
    metrics_ = metrics;
    leaves_.clear();
    seams_.clear();

    if (root_ != no_dock) {
        solve_node(root_, area);
    }
    // A frame can never be lost off the edge: a window made smaller, or a
    // layout saved at another interface scale, pulls it back inside the area
    // with its title bar reachable.
    if (area.width > 0.0F && area.height > 0.0F) {
        for (Floating& frame : floating_) {
            Rect& placed = frame.frame;
            placed.width = std::min(placed.width, area.width);
            placed.height = std::min(placed.height, area.height);
            placed.x = std::clamp(placed.x, area.x,
                                  area.x + area.width - placed.width);
            placed.y = std::clamp(placed.y, area.y,
                                  area.y + area.height - placed.height);
        }
    }
    // Floating frames after the docked tree, in stacking order, so the leaf
    // lists come out back to front for both painting and hit testing.
    for (const Floating& frame : floating_) {
        const Rect below{
            frame.frame.x,
            frame.frame.y + metrics.title_bar,
            frame.frame.width,
            std::max(frame.frame.height - metrics.title_bar, 0.0F)};
        solve_node(frame.node, below);
    }
}

DockId DockLayout::leaf_of(PanelId panel) const noexcept {
    for (std::size_t i = 0U; i < nodes_.size(); ++i) {
        const Node& node = nodes_[i];
        if (!node.used || node.split) {
            continue;
        }
        if (std::find(node.tabs.begin(), node.tabs.end(), panel) != node.tabs.end()) {
            return static_cast<DockId>(i);
        }
    }
    return no_dock;
}

DockId DockLayout::leaf_at(float x, float y) const noexcept {
    // Back to front in the list means front to back here: a floating frame
    // takes the drop before the docked panel underneath it.
    for (auto it = leaves_.rbegin(); it != leaves_.rend(); ++it) {
        if (nodes_[*it].rect.contains(x, y)) {
            return *it;
        }
    }
    return no_dock;
}

Rect DockLayout::rect_of(DockId id) const noexcept {
    return valid(id) ? nodes_[id].rect : Rect{};
}

Rect DockLayout::content_of(DockId leaf) const noexcept {
    if (!valid(leaf) || nodes_[leaf].split) {
        return Rect{};
    }
    Rect content = nodes_[leaf].rect;
    static_cast<void>(cut_top(content, metrics_.tab_bar));
    return content;
}

std::span<const PanelId> DockLayout::tabs_of(DockId leaf) const noexcept {
    if (!valid(leaf) || nodes_[leaf].split) {
        return {};
    }
    return nodes_[leaf].tabs;
}

PanelId DockLayout::active_of(DockId leaf) const noexcept {
    if (!valid(leaf) || nodes_[leaf].split || nodes_[leaf].tabs.empty()) {
        return PanelId{};
    }
    return nodes_[leaf].tabs[std::min(nodes_[leaf].active, nodes_[leaf].tabs.size() - 1U)];
}

void DockLayout::set_active(DockId leaf, PanelId panel) noexcept {
    if (!valid(leaf) || nodes_[leaf].split) {
        return;
    }
    const auto& tabs = nodes_[leaf].tabs;
    const auto found = std::find(tabs.begin(), tabs.end(), panel);
    if (found != tabs.end()) {
        nodes_[leaf].active = static_cast<std::size_t>(found - tabs.begin());
    }
}

std::string_view DockLayout::title_of(PanelId panel) const noexcept {
    const auto found = titles_.find(panel);
    return found == titles_.end() ? std::string_view{} : std::string_view{found->second};
}

std::span<const DockId> DockLayout::leaves() const noexcept {
    return leaves_;
}

std::span<const DockId> DockLayout::seams() const noexcept {
    return seams_;
}

DockId DockLayout::first_child(DockId split) const noexcept {
    return is_split(split) ? nodes_[split].first : no_dock;
}

DockId DockLayout::second_child(DockId split) const noexcept {
    return is_split(split) ? nodes_[split].second : no_dock;
}

Axis DockLayout::axis_of(DockId split) const noexcept {
    return is_split(split) ? nodes_[split].axis : Axis::Row;
}

float DockLayout::fraction_of(DockId split) const noexcept {
    return is_split(split) ? nodes_[split].fraction : 0.5F;
}

void DockLayout::set_fraction(DockId split, float fraction) noexcept {
    if (is_split(split)) {
        nodes_[split].fraction = fraction;
    }
}

std::span<const DockLayout::Floating> DockLayout::floating() const noexcept {
    return floating_;
}

DockId DockLayout::floating_at(float x, float y) const noexcept {
    for (auto it = floating_.rbegin(); it != floating_.rend(); ++it) {
        if (it->frame.contains(x, y)) {
            return it->node;
        }
    }
    return no_dock;
}

void DockLayout::move_floating(DockId node, float dx, float dy) noexcept {
    for (Floating& frame : floating_) {
        if (frame.node == node) {
            frame.frame.x += dx;
            frame.frame.y += dy;
            return;
        }
    }
}

void DockLayout::resize_floating(DockId node, float dw, float dh,
                                 float min_width,
                                 float min_height) noexcept {
    for (Floating& frame : floating_) {
        if (frame.node == node) {
            frame.frame.width = std::max(frame.frame.width + dw, min_width);
            frame.frame.height = std::max(frame.frame.height + dh, min_height);
            return;
        }
    }
}

void DockLayout::raise(DockId node) noexcept {
    const auto found = std::find_if(
        floating_.begin(), floating_.end(), [node](const Floating& f) {
            return f.node == node;
        });
    if (found != floating_.end() && found + 1 != floating_.end()) {
        std::rotate(found, found + 1, floating_.end());
    }
}

// ------------------------------------------------------------------ DockHost

void DockHost::run_seam(UiContext& ctx, DockId seam) {
    const Rect bounds = layout_->rect_of(seam);
    const Axis axis = layout_->axis_of(seam);
    const Split panes = split_rects(
        bounds, layout_->fraction_of(seam), axis, metrics_.divider);

    const bool horizontal = axis == Axis::Row;
    const Rect hit = horizontal
        ? panes.divider.inflated(metrics_.seam_reach, 0.0F)
        : panes.divider.inflated(0.0F, metrics_.seam_reach);
    const Interaction it = ctx.grab(seam_id(seam), hit);
    ctx.cursor_for(it, horizontal ? Cursor::ResizeHorizontal
                                  : Cursor::ResizeVertical);
    if (!it.held) {
        return;
    }
    const float extent = horizontal ? bounds.width : bounds.height;
    if (extent <= 0.0F) {
        return;
    }
    const float origin = horizontal ? bounds.x : bounds.y;
    const float pointer = horizontal ? ctx.pointer_x() : ctx.pointer_y();
    layout_->set_fraction(
        seam, clamp_split(bounds, (pointer - origin) / extent, axis));
}

void DockHost::run_seams(UiContext& ctx, DockId root) {
    if (!layout_->valid(root) || !layout_->is_split(root)) {
        return;
    }
    run_seams(ctx, layout_->first_child(root));
    run_seams(ctx, layout_->second_child(root));
    run_seam(ctx, root);
}

void DockHost::draw_seam(UiContext& ctx, DockId split) const {
    const Split panes = split_rects(
        layout_->rect_of(split),
        layout_->fraction_of(split),
        layout_->axis_of(split),
        metrics_.divider);
    const UiId id = seam_id(split);
    // At rest the seam is the background showing between two framed panels.
    // Under the pointer it lights a rule down its middle, and while dragged
    // the rule takes the accent, so the operator sees exactly what they hold.
    ctx.panel(panes.divider, ctx.theme().background);
    const bool held = ctx.active() == id;
    if (!held && ctx.hot() != id) {
        return;
    }
    const bool horizontal = layout_->axis_of(split) == Axis::Row;
    const float rule = 2.0F;
    const Rect line = horizontal
        ? Rect{panes.divider.x + (panes.divider.width - rule) * 0.5F,
               panes.divider.y, rule, panes.divider.height}
        : Rect{panes.divider.x,
               panes.divider.y + (panes.divider.height - rule) * 0.5F,
               panes.divider.width, rule};
    ctx.panel(line, held ? ctx.theme().accent
                         : mixed(ctx.theme().border, ctx.theme().accent, 0.5F));
}

void DockHost::draw_tree(
    UiContext& ctx, DockId node, const PanelDrawer& draw_panel) {
    if (!layout_->valid(node)) {
        return;
    }
    if (!layout_->is_split(node)) {
        draw_leaf(ctx, node, draw_panel);
        return;
    }
    draw_tree(ctx, layout_->first_child(node), draw_panel);
    draw_tree(ctx, layout_->second_child(node), draw_panel);
    // After its children, so the seam is never buried under what it divides.
    draw_seam(ctx, node);
}

void DockHost::draw_leaf(
    UiContext& ctx, DockId leaf, const PanelDrawer& draw_panel) {
    const Rect bounds = layout_->rect_of(leaf);
    const auto tabs = layout_->tabs_of(leaf);
    if (tabs.empty()) {
        return;
    }

    const Theme& theme = ctx.theme();
    ctx.panel(bounds, theme.panel);

    // The strip is a shade darker than the panel it heads, so a row of tabs
    // reads as the panel's title bar; the showing tab is cut from the panel's
    // own colour and joins it.
    Rect body = bounds;
    const Rect strip = cut_top(body, metrics_.tab_bar);
    const UiColor strip_color = mixed(theme.background, theme.panel, 0.55F);
    ctx.panel(strip, strip_color);
    {
        Rect rule = strip;
        ctx.panel(cut_bottom(rule, 1.0F), theme.border);
    }

    const PanelId active = layout_->active_of(leaf);
    ctx.push_clip(strip);
    float pen = bounds.x;
    for (const PanelId panel : tabs) {
        const std::string_view title = layout_->title_of(panel);
        // Room for the label, breathing space, and a close affordance that does
        // not overlap the tab's drag target.
        const float width = ctx.font_width(title) + theme.padding * 3.0F +
                            tab_close_extent;
        const Rect tab{pen, strip.y, width, strip.height};
        if (tab.x >= strip.x + strip.width) {
            // Run off the end of the strip: clipped away, so it must not be
            // clickable either. Scrolling the strip is a later refinement.
            pen += width;
            continue;
        }

        // The close mark only exists on the showing tab and the one under the
        // pointer. Six crosses in a row are noise, and an invisible one must
        // not take a click either.
        const bool is_active = panel == active;
        const bool tab_pointed = tab.contains(ctx.pointer_x(), ctx.pointer_y()) &&
                                 !ctx.pointer_blocked();
        const bool closable = is_active || tab_pointed ||
                              ctx.active() == tab_close_id(panel);
        // The close mark's target at the tab's right end; the rest of the
        // tab, and 2px under the mark, drags it.
        Rect close_lane = tab;
        const Rect close_bounds =
            inset(cut_right(close_lane, tab_close_extent),
                  Padding{0.0F, 3.0F, 3.0F, 3.0F});
        const Interaction close = closable
            ? ctx.grab(tab_close_id(panel), close_bounds) : Interaction{};
        if (close.clicked) {
            close_panel_pending_ = panel;
        }

        const Rect drag_bounds =
            inset(tab, Padding{0.0F, 0.0F, tab_close_extent - 2.0F, 0.0F});
        const Interaction it = ctx.grab(tab_id(panel), drag_bounds);
        if (it.pressed) {
            layout_->set_active(leaf, panel);
            press_x_ = ctx.pointer_x();
            press_y_ = ctx.pointer_y();
        }
        if (it.held && !drag_active_ && dragging_ == PanelId{}) {
            const float travel = std::fabs(ctx.pointer_x() - press_x_)
                + std::fabs(ctx.pointer_y() - press_y_);
            if (travel > drag_threshold) {
                drag_active_ = true;
                dragging_ = panel;
            }
        }
        if (it.released && drag_active_ && dragging_ == panel) {
            drop_pending_ = true;
        }

        const UiColor body_color = is_active
            ? theme.panel
            : (it.hovered ? mixed(strip_color, theme.widget_hover, 0.6F)
                          : strip_color);
        // An inactive tab stops a pixel short, leaving the strip's rule.
        ctx.panel(is_active ? tab : inset(tab, Padding{0.0F, 0.0F, 0.0F, 1.0F}),
                  body_color);
        if (is_active) {
            // An accent rule along the top marks the showing tab - readable at
            // a glance in a dark room - and the tab opens into the panel
            // below it rather than sitting on a line.
            Rect edges = tab;
            Rect top = tab;
            ctx.panel(cut_top(top, 2.0F), theme.accent);
            ctx.panel(cut_right(edges, 1.0F), theme.border);
            if (tab.x > bounds.x) {
                ctx.panel(cut_left(edges, 1.0F), theme.border);
            }
        }
        // The title, a pixel low to sit on the tab's centre line.
        Rect title_cell = inset(
            tab, Padding{theme.padding * 1.5F, 0.0F, tab_close_extent, 0.0F});
        title_cell.y += 1.0F;
        ctx.label(title_cell, title, is_active ? theme.text : theme.text_muted);
        if (closable) {
            if (close.hovered || close.held) {
                ctx.panel(close_bounds.inflated(-2.0F, -2.0F),
                          close.held ? theme.widget_active : theme.widget_hover);
            }
            const UiColor close_color =
                close.held ? theme.warning
                           : (close.hovered ? theme.text : theme.text_muted);
            const float cx = close_bounds.x + close_bounds.width * 0.5F;
            const float cy = close_bounds.y + close_bounds.height * 0.5F;
            constexpr float arm = 3.5F;
            ctx.draw_list().add_line(cx - arm, cy - arm, cx + arm, cy + arm,
                                     1.2F, close_color);
            ctx.draw_list().add_line(cx + arm, cy - arm, cx - arm, cy + arm,
                                     1.2F, close_color);
        }
        pen += width;
    }
    ctx.pop_clip();

    // The panel itself, straight after its own chrome and clipped to the space
    // it owns, so a frame and everything inside it lands as one layer.
    const Rect content = layout_->content_of(leaf);
    if (draw_panel && active != PanelId{} && content.width > 0.0F
        && content.height > 0.0F) {
        ctx.push_clip(content);
        draw_panel(active, content);
        ctx.pop_clip();
    }
    // Last, over the panel's own edge pixels: every leaf is framed, so two
    // dark panels side by side always read as two.
    ctx.outline(bounds, theme.border);
}

void DockHost::run_floating_chrome(UiContext& ctx) {
    DockId node = no_dock;
    for (const DockLayout::Floating& frame : layout_->floating()) {
        if (ctx.active() == frame_close_id(frame.node) ||
            ctx.active() == title_id(frame.node) ||
            ctx.active() == resize_id(frame.node) ||
            ctx.active() == resize_edge_id(frame.node, true) ||
            ctx.active() == resize_edge_id(frame.node, false)) {
            node = frame.node;
            break;
        }
    }
    if (node == no_dock) {
        node = layout_->floating_at(ctx.pointer_x(), ctx.pointer_y());
    }
    if (node == no_dock) {
        return;
    }

    const auto found = std::find_if(
        layout_->floating().begin(), layout_->floating().end(),
        [node](const DockLayout::Floating& frame) {
            return frame.node == node;
        });
    if (found == layout_->floating().end()) {
        return;
    }

    const Rect close_bounds = floating_close_bounds(*found, metrics_);
    const Rect drag_title = floating_title_lane(*found, metrics_);
    const Rect resize_bounds = floating_resize_bounds(*found);

    const Interaction close = ctx.grab(frame_close_id(node), close_bounds);
    const Interaction resize = ctx.grab(resize_id(node), resize_bounds);
    const Interaction right =
        ctx.grab(resize_edge_id(node, true), floating_right_edge(*found));
    const Interaction bottom =
        ctx.grab(resize_edge_id(node, false), floating_bottom_edge(*found));
    const Interaction move = ctx.grab(title_id(node), drag_title);
    ctx.cursor_for(resize, Cursor::ResizeDiagonal);
    ctx.cursor_for(right, Cursor::ResizeHorizontal);
    ctx.cursor_for(bottom, Cursor::ResizeVertical);
    ctx.cursor_for(move, Cursor::Move);
    bool raise_to_front = close.pressed || resize.pressed || move.pressed ||
                          right.pressed || bottom.pressed;

    if (close.clicked) {
        close_frame_pending_ = node;
    }
    if (resize.held && !resize.pressed) {
        layout_->resize_floating(node, ctx.pointer_dx(), ctx.pointer_dy());
    }
    if (right.held && !right.pressed) {
        layout_->resize_floating(node, ctx.pointer_dx(), 0.0F);
    }
    if (bottom.held && !bottom.pressed) {
        layout_->resize_floating(node, 0.0F, ctx.pointer_dy());
    }
    if (move.held && !move.pressed) {
        layout_->move_floating(node, ctx.pointer_dx(), ctx.pointer_dy());
    }
    if (raise_to_front) {
        layout_->raise(node);
    }
}

void DockHost::draw_floating(UiContext& ctx, const PanelDrawer& draw_panel,
                             DockId input_frame) {
    for (const DockLayout::Floating& frame : layout_->floating()) {
        const Rect title = floating_title_bounds(frame, metrics_);
        // A soft drop shadow and a lit frame, so a floating panel reads as
        // sitting above the docked ones rather than punched into them.
        for (int ring = 3; ring >= 1; --ring) {
            // Each ring spreads 2px further, the shadow falling down-right.
            Rect shadow = inset(frame.frame, -static_cast<float>(ring) * 2.0F);
            shadow.x += 2.0F;
            shadow.y += 4.0F;
            ctx.panel(shadow, UiColor{0.0F, 0.0F, 0.0F, 0.14F});
        }
        ctx.panel(frame.frame, ctx.theme().background);
        ctx.panel(title, mixed(ctx.theme().header, ctx.theme().widget_hover, 0.4F));
        const Rect close_bounds = floating_close_bounds(frame, metrics_);
        // Named after the panel showing in it - unless a lone tab right below
        // already says so, when the bar is simply the handle (the pointer
        // turns to a move cursor over it).
        const bool lone_tab = !layout_->is_split(frame.node) &&
                              layout_->tabs_of(frame.node).size() == 1U;
        if (!lone_tab) {
            ctx.label(inset(floating_title_lane(frame, metrics_),
                            Padding{8.0F, 0.0F, 4.0F, 0.0F}),
                      layout_->title_of(layout_->active_of(frame.node)),
                      ctx.theme().text_muted);
        }

        const bool frontmost =
            layout_->floating_at(ctx.pointer_x(), ctx.pointer_y()) == frame.node;
        const bool close_held = ctx.active() == frame_close_id(frame.node);
        const bool close_hovered =
            frontmost && close_bounds.contains(ctx.pointer_x(), ctx.pointer_y());
        const UiColor close_color =
            close_held ? ctx.theme().warning
                       : (close_hovered ? ctx.theme().text
                                        : ctx.theme().text_muted);
        // A cross from corner to corner, 4px in.
        const Rect mark = inset(close_bounds, 4.0F);
        ctx.draw_list().add_line(mark.x, mark.y, mark.right(), mark.bottom(),
                                 1.3F, close_color);
        ctx.draw_list().add_line(mark.right(), mark.y, mark.x, mark.bottom(),
                                 1.3F, close_color);

        const Rect grip = floating_resize_bounds(frame);

        // This frame's whole tree, immediately, so it stacks above the docked
        // panels it overlaps instead of only its border doing so.
        const bool covered = input_frame != no_dock && frame.node != input_frame;
        if (covered) {
            ctx.push_pointer_block();
        }
        draw_tree(ctx, frame.node, draw_panel);
        if (covered) {
            ctx.pop_pointer_block();
        }

        const bool resize_held = ctx.active() == resize_id(frame.node);
        const bool resize_hovered =
            frontmost && grip.contains(ctx.pointer_x(), ctx.pointer_y());
        const UiColor grip_color = resize_held
            ? ctx.theme().accent
            : (resize_hovered ? ctx.theme().text : ctx.theme().text_muted);
        ctx.draw_list().add_line(grip.x + 6.0F, grip.y + grip.height - 3.0F,
                                 grip.x + grip.width - 3.0F,
                                 grip.y + 6.0F, 1.0F, grip_color);
        ctx.draw_list().add_line(grip.x + 10.0F, grip.y + grip.height - 3.0F,
                                 grip.x + grip.width - 3.0F,
                                 grip.y + 10.0F, 1.0F, grip_color);
        ctx.draw_list().add_line(grip.x + 14.0F, grip.y + grip.height - 3.0F,
                                 grip.x + grip.width - 3.0F,
                                 grip.y + 14.0F, 1.0F, grip_color);
        const bool edge_held =
            ctx.active() == resize_edge_id(frame.node, true) ||
            ctx.active() == resize_edge_id(frame.node, false);
        ctx.outline(frame.frame,
                    edge_held ? ctx.theme().accent
                          : mixed(ctx.theme().border, ctx.theme().text_muted, 0.35F));
    }
}

void DockHost::close_tree(DockId node) {
    std::vector<PanelId> panels;
    std::vector<DockId> pending{node};
    while (!pending.empty()) {
        const DockId current = pending.back();
        pending.pop_back();
        if (!layout_->valid(current)) {
            continue;
        }
        if (layout_->is_split(current)) {
            pending.push_back(layout_->first_child(current));
            pending.push_back(layout_->second_child(current));
            continue;
        }
        const auto tabs = layout_->tabs_of(current);
        panels.insert(panels.end(), tabs.begin(), tabs.end());
    }
    for (const PanelId panel : panels) {
        layout_->close(panel);
    }
}

void DockHost::finish_drag() {
    if (drop_target_ != no_dock) {
        static_cast<void>(layout_->move(dragging_, drop_target_, drop_side_));
    } else {
        // Let go over nothing: the panel becomes a floating frame under the
        // pointer, grabbed near the middle of its title bar.
        constexpr float width = 360.0F;
        constexpr float height = 260.0F;
        layout_->detach(
            dragging_,
            Rect{
                pointer_x_ - width * 0.5F,
                pointer_y_ - metrics_.title_bar * 0.5F,
                width,
                height});
    }
    drag_active_ = false;
    dragging_ = PanelId{};
    drop_target_ = no_dock;
}

void DockHost::update(
    UiContext& ctx,
    const Rect& area,
    const PanelDrawer& draw_panel,
    const DockMetrics& metrics) {
    metrics_ = metrics;
    area_ = area;
    pointer_x_ = ctx.pointer_x();
    pointer_y_ = ctx.pointer_y();

    // Floating chrome owns the highest input layer, matching the order it is
    // painted in. Claim it before docked panel callbacks can capture the same
    // press, then solve again so move/resize feedback has no one-frame lag.
    layout_->solve(area, metrics_);
    run_floating_chrome(ctx);
    layout_->solve(area, metrics_);

    // Only the frontmost root under the pointer participates in hit testing.
    // A seam already holding capture remains its owner even if the pointer
    // leaves that root while dragging.
    const DockId input_frame =
        layout_->floating_at(ctx.pointer_x(), ctx.pointer_y());
    DockId active_seam = no_dock;
    for (const DockId seam : layout_->seams()) {
        if (ctx.active() == seam_id(seam)) {
            active_seam = seam;
            break;
        }
    }

    // Let seams move against the current geometry and solve once more so this
    // frame already shows the new division.
    if (active_seam != no_dock) {
        run_seam(ctx, active_seam);
    } else {
        run_seams(ctx, input_frame != no_dock ? input_frame : layout_->root());
    }
    layout_->solve(area, metrics_);

    // Where the panel would land if it were let go now, resolved before the tab
    // strips run so a release this frame drops where the pointer actually is
    // rather than where it was on the last one. Over nothing means detaching.
    if (drag_active_) {
        drop_target_ = layout_->leaf_at(pointer_x_, pointer_y_);
        drop_side_ = drop_target_ != no_dock
            ? drop_side(
                  layout_->rect_of(drop_target_),
                  pointer_x_,
                  pointer_y_,
                  metrics_.edge)
            : DockSide::Center;
    }

    // The docked tree first, then the floating frames over it, each complete
    // before the next begins.
    if (input_frame != no_dock) {
        ctx.push_pointer_block();
    }
    draw_tree(ctx, layout_->root(), draw_panel);
    if (input_frame != no_dock) {
        ctx.pop_pointer_block();
    }
    draw_floating(ctx, draw_panel, input_frame);

    // Only now, with the walk over the solved tree finished.
    if (drop_pending_) {
        drop_pending_ = false;
        finish_drag();
    }
    if (close_frame_pending_ != no_dock) {
        close_tree(close_frame_pending_);
        close_frame_pending_ = no_dock;
    }
    if (close_panel_pending_ != PanelId{}) {
        layout_->close(close_panel_pending_);
        close_panel_pending_ = PanelId{};
    }
}

bool DockHost::visible(PanelId panel) const noexcept {
    const DockId leaf = layout_->leaf_of(panel);
    return leaf != no_dock && layout_->active_of(leaf) == panel;
}

Rect DockHost::content_of(PanelId panel) const noexcept {
    const DockId leaf = layout_->leaf_of(panel);
    if (leaf == no_dock || layout_->active_of(leaf) != panel) {
        return Rect{};
    }
    return layout_->content_of(leaf);
}

void DockHost::overlay(UiContext& ctx) {
    if (!drag_active_) {
        return;
    }
    const UiColor accent = ctx.theme().accent;

    if (drop_target_ != no_dock) {
        const Rect preview =
            drop_preview(layout_->rect_of(drop_target_), drop_side_);
        ctx.panel(preview, UiColor{accent.r, accent.g, accent.b, 0.28F});
    }

    // The tab itself, riding under the pointer, so the drag has something the
    // eye can follow between the panel it left and the one it is over.
    const std::string_view title = layout_->title_of(dragging_);
    const float width = ctx.font_width(title) + ctx.theme().padding * 3.0F;
    const Rect ghost{
        ctx.pointer_x() - width * 0.5F,
        ctx.pointer_y() - metrics_.tab_bar * 0.5F,
        width,
        metrics_.tab_bar};
    ctx.panel(ghost, UiColor{accent.r, accent.g, accent.b, 0.85F});
    ctx.label(
        Rect{ghost.x + ctx.theme().padding * 1.5F, ghost.y, ghost.width, ghost.height},
        title,
        ctx.theme().text);
}

} // namespace mediaserver::gui
