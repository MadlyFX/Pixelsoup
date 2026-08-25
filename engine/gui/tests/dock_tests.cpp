#include <mediaserver/gui/dock.hpp>
#include <mediaserver/gui/layout.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

// The dock tree's proof. Rearranging panels is where a docking system goes
// wrong quietly - a collapse that orphans a subtree, a drop that loses a panel,
// a tab index left pointing past the end - and none of that needs a pointer or
// a pixel to provoke. Every case here drives the model directly.
//
// The geometry cases matter as much as the structural ones: a leaf's rectangle
// is what a panel draws into and what a drop is judged against, so a seam that
// divides its area wrongly misplaces both.
namespace {

namespace gui = mediaserver::gui;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define UI_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] bool near(float a, float b) noexcept {
    return std::fabs(a - b) < 0.01F;
}

const gui::PanelId viewport = gui::ui_id("viewport");
const gui::PanelId inspector = gui::ui_id("inspector");
const gui::PanelId timeline = gui::ui_id("timeline");
const gui::PanelId assets = gui::ui_id("assets");

constexpr gui::DockMetrics metrics{
    .divider = 6.0F, .tab_bar = 24.0F, .title_bar = 22.0F, .edge = 0.25F};

constexpr gui::Rect area{0.0F, 0.0F, 1000.0F, 600.0F};

void the_first_panel_becomes_the_whole_area() {
    gui::DockLayout dock;
    const gui::DockId leaf = dock.add(viewport, "Viewport");
    dock.solve(area, metrics);

    UI_CHECK(dock.root() == leaf);
    UI_CHECK(dock.leaves().size() == 1U);
    UI_CHECK(near(dock.rect_of(leaf).width, 1000.0F));
    UI_CHECK(near(dock.rect_of(leaf).height, 600.0F));
    // The content sits below the tab strip.
    UI_CHECK(near(dock.content_of(leaf).y, 24.0F));
    UI_CHECK(near(dock.content_of(leaf).height, 576.0F));
}

void docking_to_an_edge_splits_the_target() {
    gui::DockLayout dock;
    const gui::DockId left = dock.add(viewport, "Viewport");
    const gui::DockId right =
        dock.dock(inspector, "Inspector", left, gui::DockSide::Right);
    dock.solve(area, metrics);

    UI_CHECK(dock.leaves().size() == 2U);
    UI_CHECK(dock.seams().size() == 1U);
    // A root leaf that gets a neighbour is no longer the root: the new split is.
    UI_CHECK(dock.root() != left);
    UI_CHECK(dock.is_split(dock.root()));
    UI_CHECK(dock.axis_of(dock.root()) == gui::Axis::Row);

    // Half each, less the seam between them.
    UI_CHECK(near(dock.rect_of(left).x, 0.0F));
    UI_CHECK(near(dock.rect_of(left).width, 497.0F));
    UI_CHECK(near(dock.rect_of(right).x, 503.0F));
    UI_CHECK(near(dock.rect_of(right).width, 497.0F));
}

void docking_to_the_leading_edge_puts_the_newcomer_first() {
    gui::DockLayout dock;
    const gui::DockId original = dock.add(viewport, "Viewport");
    const gui::DockId added =
        dock.dock(inspector, "Inspector", original, gui::DockSide::Left);
    dock.solve(area, metrics);

    UI_CHECK(near(dock.rect_of(added).x, 0.0F));
    UI_CHECK(dock.rect_of(original).x > dock.rect_of(added).x);
}

void a_top_dock_splits_along_the_other_axis() {
    gui::DockLayout dock;
    const gui::DockId body = dock.add(viewport, "Viewport");
    const gui::DockId strip =
        dock.dock(timeline, "Timeline", body, gui::DockSide::Bottom, 0.7F);
    dock.solve(area, metrics);

    UI_CHECK(dock.axis_of(dock.root()) == gui::Axis::Column);
    // A 0.7 fraction given to the trailing side leaves 0.3 above it.
    UI_CHECK(near(dock.rect_of(body).height, 177.0F));
    UI_CHECK(near(dock.rect_of(strip).y, 183.0F));
}

void docking_to_the_middle_adds_a_tab() {
    gui::DockLayout dock;
    const gui::DockId leaf = dock.add(viewport, "Viewport");
    const gui::DockId same =
        dock.dock(inspector, "Inspector", leaf, gui::DockSide::Center);
    dock.solve(area, metrics);

    UI_CHECK(same == leaf);
    UI_CHECK(dock.leaves().size() == 1U);
    UI_CHECK(dock.tabs_of(leaf).size() == 2U);
    // A panel joining a leaf comes to the front.
    UI_CHECK(dock.active_of(leaf) == inspector);
}

void removing_the_last_tab_collapses_the_leaf_and_its_seam() {
    gui::DockLayout dock;
    const gui::DockId left = dock.add(viewport, "Viewport");
    const gui::DockId right =
        dock.dock(inspector, "Inspector", left, gui::DockSide::Right);
    UI_CHECK(dock.is_split(dock.root()));

    dock.remove(inspector);
    dock.solve(area, metrics);

    // The survivor is lifted into the vanished split's place and takes it all.
    UI_CHECK(dock.root() == left);
    UI_CHECK(!dock.is_split(dock.root()));
    UI_CHECK(dock.leaves().size() == 1U);
    UI_CHECK(dock.seams().empty());
    UI_CHECK(near(dock.rect_of(left).width, 1000.0F));
    UI_CHECK(dock.leaf_of(inspector) == gui::no_dock);
    UI_CHECK(!dock.valid(right));
}

void a_collapse_deep_in_the_tree_keeps_the_rest_intact() {
    gui::DockLayout dock;
    const gui::DockId main = dock.add(viewport, "Viewport");
    const gui::DockId side =
        dock.dock(inspector, "Inspector", main, gui::DockSide::Right);
    const gui::DockId under =
        dock.dock(assets, "Assets", side, gui::DockSide::Bottom);
    const gui::DockId strip =
        dock.dock(timeline, "Timeline", dock.root(), gui::DockSide::Bottom);
    dock.solve(area, metrics);
    UI_CHECK(dock.leaves().size() == 4U);

    // Take out one half of the nested split; its sibling should take over that
    // whole region, and everything above should be untouched.
    const gui::Rect before = dock.rect_of(side);
    const gui::Rect under_before = dock.rect_of(under);
    dock.remove(assets);
    dock.solve(area, metrics);

    UI_CHECK(dock.leaves().size() == 3U);
    UI_CHECK(dock.leaf_of(viewport) == main);
    UI_CHECK(dock.leaf_of(timeline) == strip);
    UI_CHECK(near(dock.rect_of(side).y, before.y));
    UI_CHECK(near(
        dock.rect_of(side).height,
        before.height + under_before.height + metrics.divider));
}

void removing_the_only_panel_empties_the_layout() {
    gui::DockLayout dock;
    dock.add(viewport, "Viewport");
    dock.remove(viewport);
    dock.solve(area, metrics);

    UI_CHECK(dock.root() == gui::no_dock);
    UI_CHECK(dock.leaves().empty());
    UI_CHECK(dock.leaf_of(viewport) == gui::no_dock);

    // And the layout is still usable afterwards.
    const gui::DockId again = dock.add(inspector, "Inspector");
    dock.solve(area, metrics);
    UI_CHECK(dock.root() == again);
    UI_CHECK(near(dock.rect_of(again).width, 1000.0F));
}

void removing_a_tab_keeps_the_right_one_showing() {
    gui::DockLayout dock;
    const gui::DockId leaf = dock.add(viewport, "Viewport");
    dock.dock(inspector, "Inspector", leaf, gui::DockSide::Center);
    dock.dock(timeline, "Timeline", leaf, gui::DockSide::Center);
    dock.set_active(leaf, timeline);

    // Removing a tab before the active one must not shift what is showing.
    dock.remove(inspector);
    UI_CHECK(dock.tabs_of(leaf).size() == 2U);
    UI_CHECK(dock.active_of(leaf) == timeline);

    // Removing the active one falls back rather than pointing past the end.
    dock.remove(timeline);
    UI_CHECK(dock.tabs_of(leaf).size() == 1U);
    UI_CHECK(dock.active_of(leaf) == viewport);
}

void a_move_relocates_a_panel_without_duplicating_it() {
    gui::DockLayout dock;
    const gui::DockId left = dock.add(viewport, "Viewport");
    const gui::DockId right =
        dock.dock(inspector, "Inspector", left, gui::DockSide::Right);
    dock.dock(timeline, "Timeline", right, gui::DockSide::Center);
    dock.solve(area, metrics);
    UI_CHECK(dock.tabs_of(right).size() == 2U);

    // Drag the timeline out of the right-hand tabs and onto the bottom of the
    // left pane.
    const gui::DockId landed = dock.move(timeline, left, gui::DockSide::Bottom);
    dock.solve(area, metrics);

    UI_CHECK(dock.leaf_of(timeline) == landed);
    UI_CHECK(landed != right);
    UI_CHECK(dock.tabs_of(right).size() == 1U);
    UI_CHECK(dock.tabs_of(landed).size() == 1U);
    UI_CHECK(dock.leaves().size() == 3U);
    // It sits under the viewport, which it split.
    UI_CHECK(dock.rect_of(landed).y > dock.rect_of(left).y);
}

void dropping_a_panel_back_where_it_started_changes_nothing() {
    gui::DockLayout dock;
    const gui::DockId leaf = dock.add(viewport, "Viewport");
    dock.solve(area, metrics);

    const gui::DockId same = dock.move(viewport, leaf, gui::DockSide::Center);
    dock.solve(area, metrics);
    UI_CHECK(same == leaf);
    UI_CHECK(dock.leaves().size() == 1U);

    // The lone panel of a leaf dropped on that leaf's own edge would otherwise
    // split a node against itself, so it is refused as a no-op too.
    const gui::DockId still = dock.move(viewport, leaf, gui::DockSide::Right);
    dock.solve(area, metrics);
    UI_CHECK(still == leaf);
    UI_CHECK(dock.leaves().size() == 1U);
    UI_CHECK(dock.seams().empty());
}

void a_tab_dragged_to_its_own_leafs_edge_splits_it() {
    gui::DockLayout dock;
    const gui::DockId leaf = dock.add(viewport, "Viewport");
    dock.dock(inspector, "Inspector", leaf, gui::DockSide::Center);
    dock.solve(area, metrics);
    UI_CHECK(dock.leaves().size() == 1U);

    // Two tabs, one dragged to the right edge of their shared leaf: that is a
    // genuine split, unlike the single-tab case above.
    const gui::DockId landed = dock.move(inspector, leaf, gui::DockSide::Right);
    dock.solve(area, metrics);

    UI_CHECK(landed != leaf);
    UI_CHECK(dock.leaves().size() == 2U);
    UI_CHECK(dock.tabs_of(leaf).size() == 1U);
    UI_CHECK(dock.rect_of(landed).x > dock.rect_of(leaf).x);
}

void detaching_lifts_a_panel_into_a_floating_frame() {
    gui::DockLayout dock;
    const gui::DockId left = dock.add(viewport, "Viewport");
    dock.dock(inspector, "Inspector", left, gui::DockSide::Right);

    const gui::Rect frame{300.0F, 200.0F, 360.0F, 260.0F};
    const gui::DockId floated = dock.detach(inspector, frame);
    dock.solve(area, metrics);

    UI_CHECK(dock.floating().size() == 1U);
    UI_CHECK(dock.floating()[0].node == floated);
    // The docked tree collapsed back to the one panel left in it.
    UI_CHECK(dock.root() == left);
    UI_CHECK(near(dock.rect_of(left).width, 1000.0F));
    // The floating tree is solved below its title bar.
    UI_CHECK(near(dock.rect_of(floated).y, 222.0F));
    UI_CHECK(near(dock.rect_of(floated).height, 238.0F));
    UI_CHECK(near(dock.rect_of(floated).x, 300.0F));
}

void a_floating_frame_can_be_moved_and_docked_again() {
    gui::DockLayout dock;
    const gui::DockId left = dock.add(viewport, "Viewport");
    dock.dock(inspector, "Inspector", left, gui::DockSide::Right);
    const gui::DockId floated =
        dock.detach(inspector, gui::Rect{300.0F, 200.0F, 360.0F, 260.0F});

    dock.move_floating(floated, 40.0F, -30.0F);
    dock.solve(area, metrics);
    UI_CHECK(near(dock.floating()[0].frame.x, 340.0F));
    UI_CHECK(near(dock.floating()[0].frame.y, 170.0F));

    // Dragged back onto the docked panel: the frame goes away with it.
    dock.move(inspector, left, gui::DockSide::Center);
    dock.solve(area, metrics);
    UI_CHECK(dock.floating().empty());
    UI_CHECK(dock.leaf_of(inspector) == left);
    UI_CHECK(dock.tabs_of(left).size() == 2U);
}

void a_floating_frame_takes_the_drop_before_the_panel_under_it() {
    gui::DockLayout dock;
    const gui::DockId left = dock.add(viewport, "Viewport");
    dock.dock(inspector, "Inspector", left, gui::DockSide::Right);
    const gui::DockId floated =
        dock.detach(inspector, gui::Rect{100.0F, 100.0F, 360.0F, 260.0F});
    dock.solve(area, metrics);

    // A point inside the frame belongs to the frame, not the docked panel it
    // covers; a point outside it falls through.
    UI_CHECK(dock.leaf_at(200.0F, 200.0F) == floated);
    UI_CHECK(dock.leaf_at(800.0F, 500.0F) == left);
    UI_CHECK(dock.floating_at(200.0F, 110.0F) == floated);
    UI_CHECK(dock.floating_at(800.0F, 500.0F) == gui::no_dock);
}

void raising_puts_a_frame_in_front() {
    gui::DockLayout dock;
    dock.add(viewport, "Viewport");
    dock.dock(inspector, "Inspector", dock.root(), gui::DockSide::Right);
    dock.dock(assets, "Assets", dock.root(), gui::DockSide::Bottom);

    const gui::Rect overlap{100.0F, 100.0F, 300.0F, 200.0F};
    const gui::DockId first = dock.detach(inspector, overlap);
    const gui::DockId second = dock.detach(assets, overlap);
    dock.solve(area, metrics);

    // The later frame is on top, so it takes a point both cover.
    UI_CHECK(dock.leaf_at(200.0F, 200.0F) == second);

    dock.raise(first);
    dock.solve(area, metrics);
    UI_CHECK(dock.leaf_at(200.0F, 200.0F) == first);
    UI_CHECK(dock.floating().size() == 2U);
}

void the_drop_side_follows_the_pointer_within_a_panel() {
    const gui::Rect node{0.0F, 0.0F, 400.0F, 400.0F};

    UI_CHECK(gui::drop_side(node, 20.0F, 200.0F) == gui::DockSide::Left);
    UI_CHECK(gui::drop_side(node, 380.0F, 200.0F) == gui::DockSide::Right);
    UI_CHECK(gui::drop_side(node, 200.0F, 20.0F) == gui::DockSide::Top);
    UI_CHECK(gui::drop_side(node, 200.0F, 380.0F) == gui::DockSide::Bottom);
    UI_CHECK(gui::drop_side(node, 200.0F, 200.0F) == gui::DockSide::Center);

    // A corner goes to whichever edge it is proportionally nearer, not to
    // whichever happens to be tested first.
    UI_CHECK(gui::drop_side(node, 10.0F, 30.0F) == gui::DockSide::Left);
    UI_CHECK(gui::drop_side(node, 30.0F, 10.0F) == gui::DockSide::Top);
}

void a_wide_panel_does_not_let_its_long_edge_swallow_the_short_one() {
    // 1000 x 100: the left band is 250px, the top band only 25px. Judged in
    // pixels the left edge would win almost everywhere along the top.
    const gui::Rect node{0.0F, 0.0F, 1000.0F, 100.0F};
    UI_CHECK(gui::drop_side(node, 120.0F, 8.0F) == gui::DockSide::Top);
    UI_CHECK(gui::drop_side(node, 40.0F, 50.0F) == gui::DockSide::Left);
}

void the_drop_preview_marks_the_half_a_panel_would_take() {
    const gui::Rect node{100.0F, 100.0F, 400.0F, 200.0F};

    const gui::Rect left = gui::drop_preview(node, gui::DockSide::Left);
    UI_CHECK(near(left.x, 100.0F) && near(left.width, 200.0F));
    UI_CHECK(near(left.height, 200.0F));

    const gui::Rect bottom = gui::drop_preview(node, gui::DockSide::Bottom);
    UI_CHECK(near(bottom.y, 200.0F) && near(bottom.height, 100.0F));

    // Tabbing highlights the whole panel, because that is what it joins.
    const gui::Rect centre = gui::drop_preview(node, gui::DockSide::Center);
    UI_CHECK(near(centre.width, 400.0F) && near(centre.height, 200.0F));
}

void a_seam_divides_its_own_area_not_the_window() {
    gui::DockLayout dock;
    const gui::DockId main = dock.add(viewport, "Viewport");
    const gui::DockId strip =
        dock.dock(timeline, "Timeline", main, gui::DockSide::Bottom, 0.5F);
    const gui::DockId side =
        dock.dock(inspector, "Inspector", main, gui::DockSide::Right, 0.5F);
    dock.solve(area, metrics);

    // The inner seam splits only the upper band, so both panes it makes are
    // that band's height rather than the window's.
    const gui::Rect upper = dock.rect_of(main);
    const gui::Rect right = dock.rect_of(side);
    UI_CHECK(near(upper.height, right.height));
    UI_CHECK(upper.height < 600.0F);
    UI_CHECK(near(upper.height + metrics.divider + dock.rect_of(strip).height, 600.0F));
}

void panels_keep_their_titles() {
    gui::DockLayout dock;
    const gui::DockId leaf = dock.add(viewport, "Viewport");
    dock.dock(timeline, "Timeline", leaf, gui::DockSide::Center);

    UI_CHECK(dock.title_of(viewport) == "Viewport");
    UI_CHECK(dock.title_of(timeline) == "Timeline");
    UI_CHECK(dock.title_of(gui::ui_id("nothing")).empty());
}

// --- The host: the drag that rearranges the tree ---------------------------
//
// The model above is exercised by calling its operations. What is left is the
// path from a pointer to those operations - press a tab, travel far enough for
// it to be a drag rather than a click, and let go somewhere - and that runs
// perfectly well with a synthetic InputState and no window at all.
//
// The atlas is fontless, so every tab measures the same minimum width. That is
// not a limitation here: what is under test is which tab a press lands on and
// what the release does with it, not how wide a word is.
struct Harness final {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    gui::DockLayout dock;
    gui::DockHost host{dock};
    // The panels the last frame asked for, in the order it asked - which is the
    // order they are painted in.
    std::vector<gui::PanelId> drawn;

    // The dock area deliberately starts below the top of the window, so a drop
    // above it lands on nothing - which is what detaching a panel looks like.
    static constexpr gui::Rect area{0.0F, 40.0F, 1000.0F, 560.0F};

    void frame(float x, float y, bool down) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        ctx.new_frame();
        drawn.clear();
        host.update(
            ctx,
            area,
            [this](gui::PanelId panel, const gui::Rect&) { drawn.push_back(panel); },
            metrics);
        host.overlay(ctx);
    }

    // Press on a tab, travel to the target, and let go there.
    void drag(float from_x, float from_y, float to_x, float to_y) {
        frame(from_x, from_y, true);
        frame(to_x, to_y, true);
        frame(to_x, to_y, false);
    }
};

// The x of the nth tab's middle in a leaf whose strip starts at `left`.
[[nodiscard]] float tab_centre(
    const gui::UiContext& ctx, float left, std::size_t index) {
    const float width = ctx.font_width(std::string_view{}) + ctx.theme().padding * 3.0F;
    return left + width * (static_cast<float>(index) + 0.5F);
}

void clicking_a_tab_brings_it_forward() {
    Harness h;
    const gui::DockId leaf = h.dock.add(inspector, "Inspector");
    h.dock.dock(assets, "Assets", leaf, gui::DockSide::Center);
    h.dock.set_active(leaf, inspector);
    h.frame(0.0F, 0.0F, false);

    const float second = tab_centre(h.ctx, h.area.x, 1U);
    h.frame(second, h.area.y + 12.0F, true);
    h.frame(second, h.area.y + 12.0F, false);

    UI_CHECK(h.dock.active_of(leaf) == assets);
    // A click is not a drag: the tree is untouched.
    UI_CHECK(h.dock.leaves().size() == 1U);
    UI_CHECK(!h.host.dragging());
}

void dragging_a_tab_onto_another_panels_edge_redocks_it() {
    Harness h;
    const gui::DockId left = h.dock.add(viewport, "Viewport");
    const gui::DockId right =
        h.dock.dock(inspector, "Inspector", left, gui::DockSide::Right);
    h.frame(0.0F, 0.0F, false);
    UI_CHECK(h.dock.leaves().size() == 2U);

    // Grab the viewport's tab and let go against the right edge of the
    // inspector, which should put it beyond the inspector rather than tab it.
    h.drag(
        tab_centre(h.ctx, h.area.x, 0U),
        h.area.y + 12.0F,
        960.0F,
        h.area.y + 280.0F);

    UI_CHECK(h.dock.leaves().size() == 2U);
    UI_CHECK(h.dock.tabs_of(right).size() == 1U);
    const gui::DockId moved = h.dock.leaf_of(viewport);
    UI_CHECK(moved != gui::no_dock);
    UI_CHECK(moved != right);
    UI_CHECK(!h.host.dragging());

    // The drop lands at the end of the frame it was released in, so the new
    // arrangement has rectangles only once the next frame has solved it.
    h.frame(0.0F, 0.0F, false);
    UI_CHECK(h.dock.rect_of(moved).x > h.dock.rect_of(right).x);
    UI_CHECK(near(h.dock.rect_of(right).x, 0.0F));
}

void dragging_a_tab_into_another_panels_middle_tabs_it() {
    Harness h;
    const gui::DockId left = h.dock.add(viewport, "Viewport");
    const gui::DockId right =
        h.dock.dock(inspector, "Inspector", left, gui::DockSide::Right);
    h.frame(0.0F, 0.0F, false);

    // The middle of the inspector is a tab drop, not a split.
    h.drag(
        tab_centre(h.ctx, h.area.x, 0U),
        h.area.y + 12.0F,
        750.0F,
        h.area.y + 280.0F);

    UI_CHECK(h.dock.leaf_of(viewport) == right);
    UI_CHECK(h.dock.tabs_of(right).size() == 2U);
    // The whole area belongs to the surviving leaf now.
    h.frame(0.0F, 0.0F, false);
    UI_CHECK(h.dock.leaves().size() == 1U);
    UI_CHECK(near(h.dock.rect_of(right).width, 1000.0F));
}

void dragging_a_tab_off_the_dock_area_detaches_it() {
    Harness h;
    const gui::DockId left = h.dock.add(viewport, "Viewport");
    h.dock.dock(inspector, "Inspector", left, gui::DockSide::Right);
    h.frame(0.0F, 0.0F, false);
    UI_CHECK(h.dock.floating().empty());

    // Let go above the dock area, where there is no panel to land on.
    h.drag(tab_centre(h.ctx, h.area.x, 0U), h.area.y + 12.0F, 500.0F, 10.0F);

    UI_CHECK(h.dock.floating().size() == 1U);
    const gui::DockId floated = h.dock.leaf_of(viewport);
    UI_CHECK(floated == h.dock.floating()[0].node);
    // The frame is centred under where it was let go.
    UI_CHECK(near(h.dock.floating()[0].frame.x, 500.0F - 180.0F));
    // And the docked tree collapsed to the panel left behind.
    UI_CHECK(h.dock.leaf_of(inspector) == h.dock.root());
}

void a_press_that_does_not_travel_is_never_a_drag() {
    Harness h;
    const gui::DockId left = h.dock.add(viewport, "Viewport");
    const gui::DockId right =
        h.dock.dock(inspector, "Inspector", left, gui::DockSide::Right);
    h.frame(0.0F, 0.0F, false);

    // Two pixels of jitter is a click, not an intent to re-dock.
    const float x = tab_centre(h.ctx, h.area.x, 0U);
    h.frame(x, h.area.y + 12.0F, true);
    h.frame(x + 2.0F, h.area.y + 12.0F, true);
    h.frame(x + 2.0F, h.area.y + 12.0F, false);

    UI_CHECK(h.dock.leaf_of(viewport) == left);
    UI_CHECK(h.dock.tabs_of(right).size() == 1U);
    UI_CHECK(h.dock.leaves().size() == 2U);
}

void dragging_a_seam_resizes_the_panels_either_side() {
    Harness h;
    const gui::DockId left = h.dock.add(viewport, "Viewport");
    const gui::DockId right =
        h.dock.dock(inspector, "Inspector", left, gui::DockSide::Right);
    h.frame(0.0F, 0.0F, false);
    UI_CHECK(near(h.dock.rect_of(left).width, 497.0F));

    // The seam sits on the midline; drag it to three tenths across.
    h.frame(500.0F, h.area.y + 300.0F, true);
    h.frame(300.0F, h.area.y + 300.0F, true);

    UI_CHECK(near(h.dock.rect_of(left).width, 297.0F));
    UI_CHECK(near(h.dock.rect_of(right).x, 303.0F));
    UI_CHECK(near(h.dock.rect_of(right).width, 697.0F));

    h.frame(300.0F, h.area.y + 300.0F, false);
    UI_CHECK(near(h.dock.rect_of(left).width, 297.0F));
}

void a_panel_is_only_drawn_when_its_tab_is_showing() {
    Harness h;
    const gui::DockId leaf = h.dock.add(inspector, "Inspector");
    h.dock.dock(assets, "Assets", leaf, gui::DockSide::Center);
    h.dock.set_active(leaf, inspector);
    h.frame(0.0F, 0.0F, false);

    UI_CHECK(h.host.visible(inspector));
    UI_CHECK(!h.host.visible(assets));
    UI_CHECK(near(h.host.content_of(inspector).height, 560.0F - metrics.tab_bar));
    UI_CHECK(near(h.host.content_of(assets).width, 0.0F));

    h.dock.set_active(leaf, assets);
    h.frame(0.0F, 0.0F, false);
    UI_CHECK(h.host.visible(assets));
    UI_CHECK(!h.host.visible(inspector));
}

void a_floating_panel_is_painted_over_the_docked_ones() {
    Harness h;
    const gui::DockId left = h.dock.add(viewport, "Viewport");
    h.dock.dock(inspector, "Inspector", left, gui::DockSide::Right);
    h.dock.dock(timeline, "Timeline", h.dock.root(), gui::DockSide::Bottom);
    h.frame(0.0F, 0.0F, false);
    UI_CHECK(h.drawn.size() == 3U);

    // Detached over the middle of the window, so it covers docked panels.
    h.dock.detach(inspector, gui::Rect{200.0F, 200.0F, 360.0F, 260.0F});
    h.frame(0.0F, 0.0F, false);

    UI_CHECK(h.drawn.size() == 3U);
    // Last means on top: a docked panel drawn afterwards would paint straight
    // over the frame floating above it.
    UI_CHECK(h.drawn.back() == inspector);
    UI_CHECK(h.drawn[0] != inspector && h.drawn[1] != inspector);
}

void a_hidden_tab_is_never_asked_to_draw() {
    Harness h;
    const gui::DockId leaf = h.dock.add(inspector, "Inspector");
    h.dock.dock(assets, "Assets", leaf, gui::DockSide::Center);
    h.dock.set_active(leaf, inspector);

    h.frame(0.0F, 0.0F, false);
    UI_CHECK(h.drawn.size() == 1U);
    UI_CHECK(h.drawn[0] == inspector);

    h.dock.set_active(leaf, assets);
    h.frame(0.0F, 0.0F, false);
    UI_CHECK(h.drawn.size() == 1U);
    UI_CHECK(h.drawn[0] == assets);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 30U> cases{{
    {"the first panel becomes the whole area",
     &the_first_panel_becomes_the_whole_area},
    {"docking to an edge splits the target",
     &docking_to_an_edge_splits_the_target},
    {"docking to the leading edge puts the newcomer first",
     &docking_to_the_leading_edge_puts_the_newcomer_first},
    {"a top dock splits along the other axis",
     &a_top_dock_splits_along_the_other_axis},
    {"docking to the middle adds a tab", &docking_to_the_middle_adds_a_tab},
    {"removing the last tab collapses the leaf and its seam",
     &removing_the_last_tab_collapses_the_leaf_and_its_seam},
    {"a collapse deep in the tree keeps the rest intact",
     &a_collapse_deep_in_the_tree_keeps_the_rest_intact},
    {"removing the only panel empties the layout",
     &removing_the_only_panel_empties_the_layout},
    {"removing a tab keeps the right one showing",
     &removing_a_tab_keeps_the_right_one_showing},
    {"a move relocates a panel without duplicating it",
     &a_move_relocates_a_panel_without_duplicating_it},
    {"dropping a panel back where it started changes nothing",
     &dropping_a_panel_back_where_it_started_changes_nothing},
    {"a tab dragged to its own leafs edge splits it",
     &a_tab_dragged_to_its_own_leafs_edge_splits_it},
    {"detaching lifts a panel into a floating frame",
     &detaching_lifts_a_panel_into_a_floating_frame},
    {"a floating frame can be moved and docked again",
     &a_floating_frame_can_be_moved_and_docked_again},
    {"a floating frame takes the drop before the panel under it",
     &a_floating_frame_takes_the_drop_before_the_panel_under_it},
    {"raising puts a frame in front", &raising_puts_a_frame_in_front},
    {"the drop side follows the pointer within a panel",
     &the_drop_side_follows_the_pointer_within_a_panel},
    {"a wide panel does not let its long edge swallow the short one",
     &a_wide_panel_does_not_let_its_long_edge_swallow_the_short_one},
    {"the drop preview marks the half a panel would take",
     &the_drop_preview_marks_the_half_a_panel_would_take},
    {"a seam divides its own area not the window",
     &a_seam_divides_its_own_area_not_the_window},
    {"panels keep their titles", &panels_keep_their_titles},
    {"clicking a tab brings it forward", &clicking_a_tab_brings_it_forward},
    {"dragging a tab onto another panels edge redocks it",
     &dragging_a_tab_onto_another_panels_edge_redocks_it},
    {"dragging a tab into another panels middle tabs it",
     &dragging_a_tab_into_another_panels_middle_tabs_it},
    {"dragging a tab off the dock area detaches it",
     &dragging_a_tab_off_the_dock_area_detaches_it},
    {"a press that does not travel is never a drag",
     &a_press_that_does_not_travel_is_never_a_drag},
    {"dragging a seam resizes the panels either side",
     &dragging_a_seam_resizes_the_panels_either_side},
    {"a panel is only drawn when its tab is showing",
     &a_panel_is_only_drawn_when_its_tab_is_showing},
    {"a floating panel is painted over the docked ones",
     &a_floating_panel_is_painted_over_the_docked_ones},
    {"a hidden tab is never asked to draw",
     &a_hidden_tab_is_never_asked_to_draw},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
