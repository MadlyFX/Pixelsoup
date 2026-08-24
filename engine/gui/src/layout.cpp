#include <mediaserver/gui/layout.hpp>

#include <algorithm>

namespace mediaserver::gui {
namespace {

// Row is the horizontal axis and Column the vertical one, so a node's main axis
// names which of its width/height the children stack along.
[[nodiscard]] Axis cross_of(Axis axis) noexcept {
    return axis == Axis::Row ? Axis::Column : Axis::Row;
}

[[nodiscard]] const Size& size_on(const LayoutStyle& style, Axis axis) noexcept {
    return axis == Axis::Row ? style.width : style.height;
}

} // namespace

void LayoutTree::reset() noexcept {
    // Capacity is kept: a steady-state frame rebuilds the same tree and should
    // not touch the allocator to do it.
    nodes_.clear();
    open_.clear();
    root_ = no_node;
}

NodeId LayoutTree::begin(const LayoutStyle& style) {
    const auto id = static_cast<NodeId>(nodes_.size());
    nodes_.push_back(Node{style, no_node, no_node, no_node, 0.0F, 0.0F, Rect{}});

    // A node opened with nothing above it is the root. A caller that opens a
    // second one at the top level gets it appended under the first rather than
    // silently dropped - one tree per frame is the contract.
    NodeId parent = no_node;
    if (!open_.empty()) {
        parent = open_.back();
    } else if (root_ != no_node && root_ != id) {
        parent = root_;
    } else {
        root_ = id;
    }

    if (parent != no_node) {
        Node& owner = nodes_[parent];
        if (owner.last_child == no_node) {
            owner.first_child = id;
        } else {
            nodes_[owner.last_child].next_sibling = id;
        }
        owner.last_child = id;
    }

    open_.push_back(id);
    return id;
}

void LayoutTree::end() noexcept {
    if (!open_.empty()) {
        open_.pop_back();
    }
}

NodeId LayoutTree::leaf(const LayoutStyle& style) {
    const NodeId id = begin(style);
    end();
    return id;
}

float LayoutTree::intrinsic(NodeId id, Axis axis) const noexcept {
    const Node& node = nodes_[id];
    const Size& size = size_on(node.style, axis);
    const float fit = axis == Axis::Row ? node.fit_width : node.fit_height;
    switch (size.mode) {
    case SizeMode::Fixed:
        return size.value;
    case SizeMode::Fit:
        return std::max(fit, size.value);
    case SizeMode::Grow:
    case SizeMode::Percent:
        // Both are resolved against the parent later; what they report here is
        // the floor they will not be squeezed below, which is their content.
        return fit;
    }
    return fit;
}

void LayoutTree::measure(NodeId id) noexcept {
    Node& node = nodes_[id];
    const Axis main = node.style.axis;
    const Axis cross = cross_of(main);

    float main_extent = 0.0F;
    float cross_extent = 0.0F;
    core::u32 count = 0U;
    for (NodeId child = node.first_child; child != no_node;
         child = nodes_[child].next_sibling) {
        measure(child);
        ++count;
        main_extent += intrinsic(child, main);
        cross_extent = std::max(cross_extent, intrinsic(child, cross));
    }
    if (count > 1U) {
        main_extent += node.style.gap * static_cast<float>(count - 1U);
    }

    const float width = main == Axis::Row ? main_extent : cross_extent;
    const float height = main == Axis::Row ? cross_extent : main_extent;
    const Padding& pad = node.style.padding;
    node.fit_width = width + pad.left + pad.right;
    node.fit_height = height + pad.top + pad.bottom;
}

void LayoutTree::arrange(NodeId id) noexcept {
    const Rect box = content(id);
    const LayoutStyle style = nodes_[id].style;
    const Axis main = style.axis;
    const Axis cross = cross_of(main);
    const bool horizontal = main == Axis::Row;
    const float box_main = horizontal ? box.width : box.height;
    const float box_cross = horizontal ? box.height : box.width;

    // First walk: how much main-axis space the non-growing children claim, and
    // the floors and weights of the growing ones. The sibling list is walked
    // twice rather than cached, so arranging a node allocates nothing.
    core::u32 count = 0U;
    float claimed = 0.0F;
    float weight_total = 0.0F;
    for (NodeId child = nodes_[id].first_child; child != no_node;
         child = nodes_[child].next_sibling) {
        ++count;
        const Size& size = size_on(nodes_[child].style, main);
        if (size.mode == SizeMode::Grow) {
            weight_total += std::max(size.value, 0.0F);
            claimed += intrinsic(child, main);
        } else if (size.mode == SizeMode::Percent) {
            claimed += size.value * box_main;
        } else {
            claimed += intrinsic(child, main);
        }
    }
    if (count == 0U) {
        return;
    }
    const float gaps = style.gap * static_cast<float>(count - 1U);
    // Children that do not fit keep their floors and overflow; a clip rect is
    // what hides the excess, so there is nothing to fail on here.
    const float spare = std::max(box_main - claimed - gaps, 0.0F);

    // Second walk: hand out the sizes and place the children.
    float pen = horizontal ? box.x : box.y;
    for (NodeId child = nodes_[id].first_child; child != no_node;
         child = nodes_[child].next_sibling) {
        const LayoutStyle& child_style = nodes_[child].style;

        const Size& main_size = size_on(child_style, main);
        float extent = 0.0F;
        switch (main_size.mode) {
        case SizeMode::Grow:
            extent = intrinsic(child, main);
            if (weight_total > 0.0F) {
                extent +=
                    spare * (std::max(main_size.value, 0.0F) / weight_total);
            }
            break;
        case SizeMode::Percent:
            extent = main_size.value * box_main;
            break;
        default:
            extent = intrinsic(child, main);
            break;
        }

        const Size& cross_size = size_on(child_style, cross);
        float thickness = 0.0F;
        switch (cross_size.mode) {
        case SizeMode::Fixed:
            thickness = cross_size.value;
            break;
        case SizeMode::Percent:
            thickness = cross_size.value * box_cross;
            break;
        case SizeMode::Grow:
            // Nothing to share on the cross axis, so growing means filling.
            thickness = box_cross;
            break;
        case SizeMode::Fit:
            thickness = style.align == Align::Stretch
                ? box_cross
                : intrinsic(child, cross);
            break;
        }

        // A child with a size of its own is placed by the alignment; under
        // Stretch it was already widened, so it sits at the start.
        float offset = 0.0F;
        switch (style.align) {
        case Align::Center:
            offset = (box_cross - thickness) * 0.5F;
            break;
        case Align::End:
            offset = box_cross - thickness;
            break;
        case Align::Start:
        case Align::Stretch:
            offset = 0.0F;
            break;
        }
        const float cross_origin = (horizontal ? box.y : box.x) + offset;

        nodes_[child].rect = horizontal
            ? Rect{pen, cross_origin, extent, thickness}
            : Rect{cross_origin, pen, thickness, extent};
        pen += extent + style.gap;

        arrange(child);
    }
}

void LayoutTree::solve(const Rect& viewport) noexcept {
    if (root_ == no_node) {
        return;
    }
    measure(root_);
    // The root takes the viewport as given: there is nothing above it to size
    // against, so its own width and height styles have no meaning.
    nodes_[root_].rect = viewport;
    arrange(root_);
}

Rect LayoutTree::rect(NodeId id) const noexcept {
    if (id >= nodes_.size()) {
        return Rect{};
    }
    return nodes_[id].rect;
}

Rect LayoutTree::content(NodeId id) const noexcept {
    if (id >= nodes_.size()) {
        return Rect{};
    }
    const Node& node = nodes_[id];
    const Padding& pad = node.style.padding;
    return Rect{
        node.rect.x + pad.left,
        node.rect.y + pad.top,
        std::max(node.rect.width - pad.left - pad.right, 0.0F),
        std::max(node.rect.height - pad.top - pad.bottom, 0.0F)};
}

} // namespace mediaserver::gui
