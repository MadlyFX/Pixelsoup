#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <cstddef>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Docking: the arrangement of panels an operator rearranges, and the drag that
// rearranges it.
//
// The model is a binary tree. A leaf holds one or more panels as tabs, with one
// of them active; a split divides its rectangle between two children along an
// axis, at a fraction the operator drags. Docking a panel to a leaf's edge
// replaces that leaf with a split holding both; docking to its middle adds a
// tab. Removing the last panel from a leaf collapses it, and its sibling takes
// the vanished split's place, so the tree never keeps a node with nothing in it.
//
// A detached panel is the same tree rooted in a floating frame instead of the
// main area, which is what lets a floating window be split and tabbed exactly
// like a docked one, and lets a panel be dragged back without a second model.
// The frames float within the main window for now; giving one its own operating
// system window needs a second swapchain, which is a step of its own.
//
// DockLayout is the model and nothing else - no drawing, no input, and so
// testable head-on. DockHost is the surface that draws it and runs the drag.
namespace mediaserver::gui {

using PanelId = UiId;
using DockId = core::u32;
inline constexpr DockId no_dock = ~DockId{0U};

// Where a panel lands relative to the node it is dropped on. The four edges
// split that node; the middle joins it as a tab.
enum class DockSide : core::u8 {
    Left,
    Right,
    Top,
    Bottom,
    Center,
};

struct DockMetrics final {
    // The seam between two docked nodes, and the grab handle for resizing it.
    float divider{6.0F};
    // The tab strip along the top of every leaf.
    float tab_bar{24.0F};
    // The drag handle along the top of a floating frame.
    float title_bar{22.0F};
    // How far in from an edge a drop still counts as splitting rather than
    // tabbing, as a fraction of the node's width or height.
    float edge{0.25F};
};

// Which drop a pointer at (x, y) over `node` is asking for. Edges are judged
// against their own band rather than in pixels, so a wide short panel does not
// have its left edge swallow its top one.
[[nodiscard]] DockSide drop_side(
    const Rect& node, float x, float y, float edge = 0.25F) noexcept;

// The area that drop would take, for the highlight shown before letting go.
[[nodiscard]] Rect drop_preview(const Rect& node, DockSide side) noexcept;

class DockLayout final {
public:
    // A detached panel tree and the frame it floats in. The frame includes the
    // title bar; the tree is solved into what is left below it.
    struct Floating final {
        DockId node{no_dock};
        Rect frame{};
    };

    // Adds a panel to the layout: the first one becomes the whole tree, and
    // later ones join the first leaf as tabs. Use dock() to place one
    // deliberately.
    DockId add(PanelId panel, std::string_view title);

    // Introduces `panel` and places it against `target`.
    DockId dock(
        PanelId panel,
        std::string_view title,
        DockId target,
        DockSide side,
        float fraction = 0.5F);

    // Moves a panel already in the layout. This is what a completed drag does.
    // Dropping a panel back where it came from is a no-op rather than a
    // needless rebuild.
    DockId move(
        PanelId panel, DockId target, DockSide side, float fraction = 0.5F);

    // Detaches `panel` into a floating frame.
    DockId detach(PanelId panel, const Rect& frame);

    // Takes the panel out of the layout, collapsing whatever it empties.
    void remove(PanelId panel);

    // Resolves every node's rectangle. Call once a frame before reading any of
    // the queries below.
    void solve(const Rect& area, const DockMetrics& metrics);

    [[nodiscard]] bool valid(DockId id) const noexcept;
    [[nodiscard]] bool is_split(DockId id) const noexcept;
    [[nodiscard]] DockId root() const noexcept { return root_; }

    [[nodiscard]] DockId leaf_of(PanelId panel) const noexcept;
    // The topmost leaf under the point - floating frames before docked ones.
    [[nodiscard]] DockId leaf_at(float x, float y) const noexcept;
    [[nodiscard]] Rect rect_of(DockId id) const noexcept;
    // A leaf's rectangle below its tab strip: where the panel's own drawing goes.
    [[nodiscard]] Rect content_of(DockId leaf) const noexcept;

    [[nodiscard]] std::span<const PanelId> tabs_of(DockId leaf) const noexcept;
    [[nodiscard]] PanelId active_of(DockId leaf) const noexcept;
    void set_active(DockId leaf, PanelId panel) noexcept;
    [[nodiscard]] std::string_view title_of(PanelId panel) const noexcept;

    // Solved, in painting order: docked leaves first, then floating ones from
    // back to front.
    [[nodiscard]] std::span<const DockId> leaves() const noexcept;
    [[nodiscard]] std::span<const DockId> seams() const noexcept;
    // The two halves of a split, so a caller can walk one root at a time and
    // paint it in its own order rather than from the flat lists above.
    [[nodiscard]] DockId first_child(DockId split) const noexcept;
    [[nodiscard]] DockId second_child(DockId split) const noexcept;
    [[nodiscard]] Axis axis_of(DockId split) const noexcept;
    [[nodiscard]] float fraction_of(DockId split) const noexcept;
    void set_fraction(DockId split, float fraction) noexcept;

    [[nodiscard]] std::span<const Floating> floating() const noexcept;
    // The topmost floating frame containing the point, or no_dock.
    [[nodiscard]] DockId floating_at(float x, float y) const noexcept;
    void move_floating(DockId node, float dx, float dy) noexcept;
    // Brings a floating frame to the front of the stack.
    void raise(DockId node) noexcept;

private:
    struct Node final {
        bool used{};
        bool split{};
        Axis axis{Axis::Row};
        float fraction{0.5F};
        DockId first{no_dock};
        DockId second{no_dock};
        DockId parent{no_dock};
        std::vector<PanelId> tabs;
        std::size_t active{};
        Rect rect{};
    };

    [[nodiscard]] DockId allocate();
    [[nodiscard]] DockId allocate_leaf(PanelId panel);
    void release(DockId id) noexcept;

    // Puts `replacement` wherever `old_node` sat - a parent's child slot, the
    // root, or a floating frame - and reparents it to match.
    void replace_slot(DockId old_node, DockId replacement) noexcept;
    // Drops an emptied leaf and lifts its sibling into the split's place.
    void collapse(DockId leaf) noexcept;
    [[nodiscard]] DockId first_leaf(DockId id) const noexcept;
    [[nodiscard]] DockId place(
        PanelId panel, DockId target, DockSide side, float fraction);
    void solve_node(DockId id, const Rect& rect);

    std::vector<Node> nodes_;
    std::vector<DockId> free_;
    DockId root_{no_dock};
    std::vector<Floating> floating_;
    std::map<PanelId, std::string> titles_;

    DockMetrics metrics_{};
    std::vector<DockId> leaves_;
    std::vector<DockId> seams_;
};

// Draws a DockLayout and runs the interaction that rearranges it: seams the
// operator drags to resize, tab strips they click to switch and drag to move,
// and floating frames they drag by the title bar.
//
// A panel's own drawing is a callback rather than something the caller does
// afterwards, because painting order is the whole difference between a
// floating frame that sits above the docked panels it covers and one that gets
// scribbled over by them. Each node's chrome and its panel are painted
// together, one root at a time. The callback is handed the rectangle the panel
// owns, already clipped to it.
//
// A frame is update() and then overlay() - the drag feedback last, since a drop
// highlight painted over by panel contents would tell the operator nothing.
class DockHost final {
public:
    // Draws one panel into the rectangle it has been given.
    using PanelDrawer = std::function<void(PanelId, const Rect&)>;

    explicit DockHost(DockLayout& layout) noexcept : layout_(&layout) {}

    void update(
        UiContext& ctx,
        const Rect& area,
        const PanelDrawer& draw_panel,
        const DockMetrics& metrics = DockMetrics{});

    // True when this panel is the active tab of its leaf, and so has a
    // rectangle to draw into this frame.
    [[nodiscard]] bool visible(PanelId panel) const noexcept;
    [[nodiscard]] Rect content_of(PanelId panel) const noexcept;

    void overlay(UiContext& ctx);

    [[nodiscard]] bool dragging() const noexcept { return drag_active_; }
    [[nodiscard]] PanelId dragged_panel() const noexcept { return dragging_; }

private:
    void run_seams(UiContext& ctx);
    // Paints one root: every node beneath it, each seam over the two panes it
    // divides, and each panel over its own chrome.
    void draw_tree(UiContext& ctx, DockId node, const PanelDrawer& draw_panel);
    void draw_seam(UiContext& ctx, DockId split) const;
    void draw_leaf(UiContext& ctx, DockId leaf, const PanelDrawer& draw_panel);
    void draw_floating(UiContext& ctx, const PanelDrawer& draw_panel);
    void finish_drag();

    DockLayout* layout_{};
    DockMetrics metrics_{};
    Rect area_{};

    // The panel being dragged, and where the press began - a drag only starts
    // once the pointer has travelled far enough that it cannot be a click.
    PanelId dragging_{};
    bool drag_active_{};
    float press_x_{};
    float press_y_{};
    float pointer_x_{};
    float pointer_y_{};

    DockId drop_target_{no_dock};
    DockSide drop_side_{DockSide::Center};
    // The release is recorded while walking the leaves and acted on after,
    // because rearranging the tree mid-walk would pull it apart underneath.
    bool drop_pending_{};
};

} // namespace mediaserver::gui
