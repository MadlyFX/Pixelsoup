#pragma once

#include <mediaserver/core/types.hpp>

#include <vector>

// The layout solver the bespoke UI lays its panels out with: a flexbox-like
// pass over a tree of boxes that turns a declarative description into pixel
// rectangles. The architecture plan names Clay and Yoga as references rather
// than dependencies, and this is that solver - small enough to read in one
// sitting, and owing them the model: a main axis, a cross axis, and children
// that are fixed, shrink-wrapped, or share what is left over.
//
// It is deliberately separate from the widget layer. Widgets take an explicit
// Rect and know nothing about how it was produced, so interaction logic and
// layout can be tested - and changed - independently. A caller may skip the
// solver entirely and pass rectangles it computed itself.
//
// Solving is two passes. The first walks bottom-up and gives every node the
// size it would take if left alone (its children's extent, plus padding). The
// second walks top-down handing out real sizes: fixed and percentage children
// take theirs, shrink-wrapped children take their first-pass size, and whatever
// main-axis space remains is split among the growing children by weight.
//
// Overflow is not an error. When children do not fit, they keep their minimum
// sizes and run past the parent's edge; the draw list's clip stack is what
// hides the excess, exactly as it would for a scrolled panel.
namespace mediaserver::gui {

// A pixel rectangle with its origin at the top left - the currency between the
// solver, the widgets, and the draw list. It lives here because layout is what
// produces rectangles; everything else downstream only consumes them.
struct Rect final {
    float x{};
    float y{};
    float width{};
    float height{};

    [[nodiscard]] bool contains(float px, float py) const noexcept {
        return px >= x && px < x + width && py >= y && py < y + height;
    }

    // Grown by `dx` left and right and `dy` above and below: how a thin mark -
    // a seam, a trim edge, a key - gets a hit area larger than what it draws.
    [[nodiscard]] Rect inflated(float dx, float dy) const noexcept {
        return Rect{x - dx, y - dy, width + dx * 2.0F, height + dy * 2.0F};
    }

    // The far edges and the middle, so a caller anchoring to them says which
    // edge it means instead of re-adding the extent.
    [[nodiscard]] constexpr float right() const noexcept { return x + width; }
    [[nodiscard]] constexpr float bottom() const noexcept { return y + height; }
    [[nodiscard]] constexpr float center_x() const noexcept {
        return x + width * 0.5F;
    }
    [[nodiscard]] constexpr float center_y() const noexcept {
        return y + height * 0.5F;
    }
};

// The direction children are stacked in. The other axis is the cross axis.
enum class Axis : core::u8 {
    Row,
    Column,
};

// How a node sizes itself on one axis.
enum class SizeMode : core::u8 {
    // Exactly `value` pixels.
    Fixed,
    // Shrink-wraps the children, never below `value` (a minimum, usually zero).
    Fit,
    // Takes a share of the main-axis space left after the others are placed,
    // weighted by `value`. On the cross axis there is nothing to share, so a
    // growing node simply fills its parent.
    Grow,
    // `value` as a fraction (0..1) of the parent's content box on this axis.
    Percent,
};

struct Size final {
    SizeMode mode{SizeMode::Fit};
    float value{};

    [[nodiscard]] static constexpr Size fixed(float pixels) noexcept {
        return Size{SizeMode::Fixed, pixels};
    }
    [[nodiscard]] static constexpr Size fit(float minimum = 0.0F) noexcept {
        return Size{SizeMode::Fit, minimum};
    }
    [[nodiscard]] static constexpr Size grow(float weight = 1.0F) noexcept {
        return Size{SizeMode::Grow, weight};
    }
    [[nodiscard]] static constexpr Size percent(float fraction) noexcept {
        return Size{SizeMode::Percent, fraction};
    }
};

// Where children sit on the cross axis within their parent's content box.
enum class Align : core::u8 {
    Start,
    Center,
    End,
    // Shrink-wrapped children are widened to fill the cross axis. A child with
    // an explicit cross size keeps it and is aligned to the start.
    Stretch,
};

struct Padding final {
    float left{};
    float top{};
    float right{};
    float bottom{};

    [[nodiscard]] static constexpr Padding all(float amount) noexcept {
        return Padding{amount, amount, amount, amount};
    }
};

struct LayoutStyle final {
    Axis axis{Axis::Column};
    Size width{};
    Size height{};
    Padding padding{};
    // Space inserted between children along the main axis.
    float gap{};
    Align align{Align::Stretch};
};

// The two panes a seam divides a rectangle into, plus the seam itself.
struct Split final {
    Rect first;
    Rect second;
    Rect divider;
};

// A seam keeps `min_pane` pixels on either side of it, so neither pane can be
// dragged away to nothing. A rectangle too small to hold two of them pins the
// seam to the middle, which is the only place left for it.
[[nodiscard]] float clamp_split(
    const Rect& bounds, float fraction, Axis axis, float min_pane = 24.0F) noexcept;

// Where a seam at `fraction` puts its divider, and what is left on either side.
// The split() widget and the dock tree both resolve their seams through this,
// so a docked edge and a plain one divide space identically.
[[nodiscard]] Split split_rects(
    const Rect& bounds, float fraction, Axis axis, float thickness) noexcept;

using NodeId = core::u32;

// The tree of boxes for one frame. Built by nesting begin()/end() calls, solved
// once, then read back by node id. Rebuild it each frame: it is a scratch
// structure, and reset() keeps the storage so a steady-state frame allocates
// nothing.
class LayoutTree final {
public:
    void reset() noexcept;

    // Opens a node as a child of the currently open one (or as the root, when
    // none is open). Must be matched by end().
    NodeId begin(const LayoutStyle& style);
    void end() noexcept;

    // A node with no children - a widget's box.
    NodeId leaf(const LayoutStyle& style);

    // Assigns the root `viewport` and resolves every node beneath it. Sizes on
    // the root's own style are ignored: the viewport is what it is.
    void solve(const Rect& viewport) noexcept;

    // The solved box. Meaningless before solve(); an unknown id gives an empty
    // rectangle rather than undefined behaviour.
    [[nodiscard]] Rect rect(NodeId id) const noexcept;
    // The solved box inset by the node's padding - where its children live, and
    // the right rectangle to clip a panel's contents to.
    [[nodiscard]] Rect content(NodeId id) const noexcept;

    [[nodiscard]] core::u32 node_count() const noexcept {
        return static_cast<core::u32>(nodes_.size());
    }

private:
    static constexpr NodeId no_node = ~NodeId{0U};

    struct Node final {
        LayoutStyle style{};
        NodeId first_child{no_node};
        NodeId last_child{no_node};
        NodeId next_sibling{no_node};
        // The size this node takes when nothing pushes on it: its children's
        // extent plus padding, floored by a Fit minimum. Pass one's output.
        float fit_width{};
        float fit_height{};
        Rect rect{};
    };

    // Pass one, for the subtree rooted at `id`.
    void measure(NodeId id) noexcept;
    // Pass two, for the children of `id`, whose own rect is already assigned.
    void arrange(NodeId id) noexcept;

    // The size `id` asks for on one axis before any growing is shared out.
    [[nodiscard]] float intrinsic(NodeId id, Axis axis) const noexcept;

    std::vector<Node> nodes_;
    std::vector<NodeId> open_;
    NodeId root_{no_node};
};

} // namespace mediaserver::gui
