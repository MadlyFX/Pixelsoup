#include <mediaserver/gui/dock.hpp>

#include <algorithm>
#include <cmath>

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

[[nodiscard]] UiId title_id(DockId node) noexcept {
    return ui_id("dock.title") ^ (static_cast<UiId>(node) * 0xD1B54A32D192ED03ULL);
}

// Far enough that the operator meant to drag rather than click.
constexpr float drag_threshold = 5.0F;

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
    const float half_w = node.width * 0.5F;
    const float half_h = node.height * 0.5F;
    switch (side) {
    case DockSide::Left:
        return Rect{node.x, node.y, half_w, node.height};
    case DockSide::Right:
        return Rect{node.x + half_w, node.y, half_w, node.height};
    case DockSide::Top:
        return Rect{node.x, node.y, node.width, half_h};
    case DockSide::Bottom:
        return Rect{node.x, node.y + half_h, node.width, half_h};
    case DockSide::Center:
        break;
    }
    return node;
}

// ---------------------------------------------------------------- DockLayout

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
    titles_[panel] = std::string{title};
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
    titles_[panel] = std::string{title};
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
    const Rect& rect = nodes_[leaf].rect;
    return Rect{
        rect.x,
        rect.y + metrics_.tab_bar,
        rect.width,
        std::max(rect.height - metrics_.tab_bar, 0.0F)};
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

void DockHost::run_seams(UiContext& ctx) {
    for (const DockId seam : layout_->seams()) {
        const Rect bounds = layout_->rect_of(seam);
        const Axis axis = layout_->axis_of(seam);
        const Split panes = split_rects(
            bounds, layout_->fraction_of(seam), axis, metrics_.divider);

        const Interaction it = ctx.grab(seam_id(seam), panes.divider);
        if (!it.held) {
            continue;
        }
        const bool horizontal = axis == Axis::Row;
        const float extent = horizontal ? bounds.width : bounds.height;
        if (extent <= 0.0F) {
            continue;
        }
        const float origin = horizontal ? bounds.x : bounds.y;
        const float pointer = horizontal ? ctx.pointer_x() : ctx.pointer_y();
        layout_->set_fraction(
            seam, clamp_split(bounds, (pointer - origin) / extent, axis));
    }
}

void DockHost::draw_seam(UiContext& ctx, DockId split) const {
    const Split panes = split_rects(
        layout_->rect_of(split),
        layout_->fraction_of(split),
        layout_->axis_of(split),
        metrics_.divider);
    const UiId id = seam_id(split);
    const UiColor body = ctx.active() == id
        ? ctx.theme().accent
        : (ctx.hot() == id ? ctx.theme().widget_hover : ctx.theme().divider);
    ctx.panel(panes.divider, body);
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

    const Rect strip{bounds.x, bounds.y, bounds.width, metrics_.tab_bar};
    ctx.panel(strip, UiColor{0.10F, 0.10F, 0.12F, 1.0F});

    const PanelId active = layout_->active_of(leaf);
    ctx.push_clip(strip);
    float pen = bounds.x;
    for (const PanelId panel : tabs) {
        const std::string_view title = layout_->title_of(panel);
        // Room for the label plus a little breathing space either side.
        const float width = ctx.font_width(title) + theme.padding * 3.0F;
        const Rect tab{pen, strip.y, width, strip.height};
        if (tab.x >= strip.x + strip.width) {
            // Run off the end of the strip: clipped away, so it must not be
            // clickable either. Scrolling the strip is a later refinement.
            pen += width;
            continue;
        }

        const Interaction it = ctx.grab(tab_id(panel), tab);
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

        const bool is_active = panel == active;
        const UiColor body = is_active
            ? theme.panel
            : (it.hovered ? theme.widget_hover : UiColor{0.13F, 0.13F, 0.15F, 1.0F});
        ctx.panel(tab, body);
        if (is_active) {
            // An accent underline marks the showing tab, the way an editor's
            // does - readable at a glance in a dark room.
            ctx.panel(
                Rect{tab.x, tab.y + tab.height - 2.0F, tab.width, 2.0F},
                theme.accent);
        }
        ctx.label(
            Rect{tab.x + theme.padding * 1.5F, tab.y, tab.width, tab.height},
            title,
            is_active ? theme.text : theme.text_muted);
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
}

void DockHost::draw_floating(UiContext& ctx, const PanelDrawer& draw_panel) {
    DockId raise_to_front = no_dock;
    float move_dx = 0.0F;
    float move_dy = 0.0F;
    DockId move_node = no_dock;

    for (const DockLayout::Floating& frame : layout_->floating()) {
        const Rect title{
            frame.frame.x,
            frame.frame.y,
            frame.frame.width,
            metrics_.title_bar};
        // A dark surround, so a floating frame reads as sitting above the
        // docked panels rather than punched into them.
        ctx.panel(frame.frame, UiColor{0.04F, 0.04F, 0.05F, 1.0F});
        ctx.panel(title, UiColor{0.16F, 0.16F, 0.19F, 1.0F});
        // Named after the panel showing in it. A frame that has been split
        // holds several and so names none: its tab strips say what is inside.
        ctx.label(
            Rect{title.x + 8.0F, title.y, title.width, title.height},
            layout_->title_of(layout_->active_of(frame.node)),
            ctx.theme().text_muted);

        const Interaction it = ctx.grab(title_id(frame.node), title);
        if (it.pressed) {
            raise_to_front = frame.node;
        }
        if (it.held) {
            move_node = frame.node;
            move_dx = ctx.pointer_dx();
            move_dy = ctx.pointer_dy();
        }

        // This frame's whole tree, immediately, so it stacks above the docked
        // panels it overlaps instead of only its border doing so.
        draw_tree(ctx, frame.node, draw_panel);
    }

    // Applied after the walk: both reorder or move the very list being iterated.
    if (move_node != no_dock) {
        layout_->move_floating(move_node, move_dx, move_dy);
    }
    if (raise_to_front != no_dock) {
        layout_->raise(raise_to_front);
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

    // Solve, let the seams be dragged, then solve again so this frame already
    // shows the new division rather than lagging it by one.
    layout_->solve(area, metrics_);
    run_seams(ctx);
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
    draw_tree(ctx, layout_->root(), draw_panel);
    draw_floating(ctx, draw_panel);

    // Only now, with the walk over the solved tree finished.
    if (drop_pending_) {
        drop_pending_ = false;
        finish_drag();
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
