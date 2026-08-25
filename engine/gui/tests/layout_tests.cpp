#include <mediaserver/gui/layout.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

// The layout solver's proof. Each case builds a small tree, solves it against a
// viewport, and asserts the pixel rectangles that come out - the whole unit is
// arithmetic, so there is nothing to mock and nothing to skip.
//
// The cases are the properties the widget layer depends on: fixed children keep
// their size, growing children share what is left in proportion to their
// weight, shrink-wrapped parents take their children's extent, padding and gap
// come out of the content box rather than the border box, and children that do
// not fit overflow rather than being crushed.
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

[[nodiscard]] bool same_rect(
    const gui::Rect& r, float x, float y, float w, float h) noexcept {
    return near(r.x, x) && near(r.y, y) && near(r.width, w)
        && near(r.height, h);
}

void growing_children_share_the_row_evenly() {
    gui::LayoutTree tree;
    tree.begin(gui::LayoutStyle{.axis = gui::Axis::Row});
    const gui::NodeId a = tree.leaf(
        gui::LayoutStyle{.width = gui::Size::grow(), .height = gui::Size::grow()});
    const gui::NodeId b = tree.leaf(
        gui::LayoutStyle{.width = gui::Size::grow(), .height = gui::Size::grow()});
    const gui::NodeId c = tree.leaf(
        gui::LayoutStyle{.width = gui::Size::grow(), .height = gui::Size::grow()});
    tree.end();
    tree.solve(gui::Rect{0.0F, 0.0F, 900.0F, 600.0F});

    UI_CHECK(same_rect(tree.rect(a), 0.0F, 0.0F, 300.0F, 600.0F));
    UI_CHECK(same_rect(tree.rect(b), 300.0F, 0.0F, 300.0F, 600.0F));
    UI_CHECK(same_rect(tree.rect(c), 600.0F, 0.0F, 300.0F, 600.0F));
}

void a_fixed_sidebar_keeps_its_width_and_the_rest_grows() {
    gui::LayoutTree tree;
    tree.begin(gui::LayoutStyle{.axis = gui::Axis::Row});
    const gui::NodeId sidebar = tree.leaf(gui::LayoutStyle{
        .width = gui::Size::fixed(200.0F), .height = gui::Size::grow()});
    const gui::NodeId viewport = tree.leaf(
        gui::LayoutStyle{.width = gui::Size::grow(), .height = gui::Size::grow()});
    tree.end();
    tree.solve(gui::Rect{0.0F, 0.0F, 1000.0F, 600.0F});

    UI_CHECK(same_rect(tree.rect(sidebar), 0.0F, 0.0F, 200.0F, 600.0F));
    UI_CHECK(same_rect(tree.rect(viewport), 200.0F, 0.0F, 800.0F, 600.0F));
}

void weight_decides_how_the_spare_space_splits() {
    gui::LayoutTree tree;
    tree.begin(gui::LayoutStyle{.axis = gui::Axis::Row});
    const gui::NodeId thin = tree.leaf(gui::LayoutStyle{
        .width = gui::Size::grow(1.0F), .height = gui::Size::grow()});
    const gui::NodeId wide = tree.leaf(gui::LayoutStyle{
        .width = gui::Size::grow(3.0F), .height = gui::Size::grow()});
    tree.end();
    tree.solve(gui::Rect{0.0F, 0.0F, 1000.0F, 400.0F});

    UI_CHECK(near(tree.rect(thin).width, 250.0F));
    UI_CHECK(near(tree.rect(wide).width, 750.0F));
    UI_CHECK(near(tree.rect(wide).x, 250.0F));
}

void padding_and_gap_come_out_of_the_content_box() {
    gui::LayoutTree tree;
    tree.begin(gui::LayoutStyle{
        .axis = gui::Axis::Row,
        .padding = gui::Padding::all(10.0F),
        .gap = 8.0F});
    const gui::NodeId a = tree.leaf(
        gui::LayoutStyle{.width = gui::Size::grow(), .height = gui::Size::grow()});
    const gui::NodeId b = tree.leaf(
        gui::LayoutStyle{.width = gui::Size::grow(), .height = gui::Size::grow()});
    tree.end();
    tree.solve(gui::Rect{0.0F, 0.0F, 1000.0F, 600.0F});

    // 1000 wide less 20 of padding and 8 of gap leaves 972 to halve.
    UI_CHECK(same_rect(tree.rect(a), 10.0F, 10.0F, 486.0F, 580.0F));
    UI_CHECK(same_rect(tree.rect(b), 504.0F, 10.0F, 486.0F, 580.0F));
}

void a_fitting_parent_takes_its_children_extent() {
    gui::LayoutTree tree;
    // Start alignment, so the fitting column is not stretched to fill instead.
    tree.begin(
        gui::LayoutStyle{.axis = gui::Axis::Row, .align = gui::Align::Start});
    const gui::NodeId column = tree.begin(gui::LayoutStyle{
        .axis = gui::Axis::Column,
        .width = gui::Size::fixed(120.0F),
        .height = gui::Size::fit(),
        .padding = gui::Padding::all(5.0F),
        .gap = 4.0F});
    tree.leaf(gui::LayoutStyle{.height = gui::Size::fixed(30.0F)});
    tree.leaf(gui::LayoutStyle{.height = gui::Size::fixed(30.0F)});
    tree.leaf(gui::LayoutStyle{.height = gui::Size::fixed(30.0F)});
    tree.end();
    tree.end();
    tree.solve(gui::Rect{0.0F, 0.0F, 800.0F, 600.0F});

    // Three 30px rows, two 4px gaps, and 5px of padding on each edge.
    UI_CHECK(near(tree.rect(column).height, 108.0F));
    UI_CHECK(near(tree.rect(column).width, 120.0F));
}

void a_percentage_resolves_against_the_parent_content_box() {
    gui::LayoutTree tree;
    tree.begin(gui::LayoutStyle{
        .axis = gui::Axis::Row, .padding = gui::Padding::all(50.0F)});
    const gui::NodeId quarter = tree.leaf(gui::LayoutStyle{
        .width = gui::Size::percent(0.25F), .height = gui::Size::grow()});
    tree.end();
    tree.solve(gui::Rect{0.0F, 0.0F, 1000.0F, 400.0F});

    // The content box is 900 wide, not 1000: padding is taken off first.
    UI_CHECK(near(tree.rect(quarter).width, 225.0F));
    UI_CHECK(near(tree.rect(quarter).x, 50.0F));
}

void alignment_places_a_child_on_the_cross_axis() {
    const auto solve_with = [](gui::Align align) {
        gui::LayoutTree tree;
        tree.begin(gui::LayoutStyle{.axis = gui::Axis::Row, .align = align});
        const gui::NodeId child = tree.leaf(gui::LayoutStyle{
            .width = gui::Size::fixed(100.0F),
            .height = gui::Size::fixed(40.0F)});
        tree.end();
        tree.solve(gui::Rect{0.0F, 0.0F, 400.0F, 600.0F});
        return tree.rect(child);
    };

    UI_CHECK(near(solve_with(gui::Align::Start).y, 0.0F));
    UI_CHECK(near(solve_with(gui::Align::Center).y, 280.0F));
    UI_CHECK(near(solve_with(gui::Align::End).y, 560.0F));
    // A child with a size of its own keeps it under Stretch and sits at the
    // start; only a shrink-wrapped one is widened.
    UI_CHECK(near(solve_with(gui::Align::Stretch).y, 0.0F));
    UI_CHECK(near(solve_with(gui::Align::Stretch).height, 40.0F));
}

void stretch_widens_a_shrink_wrapped_child() {
    gui::LayoutTree tree;
    tree.begin(
        gui::LayoutStyle{.axis = gui::Axis::Row, .align = gui::Align::Stretch});
    const gui::NodeId child = tree.leaf(gui::LayoutStyle{
        .width = gui::Size::fixed(100.0F), .height = gui::Size::fit()});
    tree.end();
    tree.solve(gui::Rect{0.0F, 0.0F, 400.0F, 600.0F});

    UI_CHECK(near(tree.rect(child).height, 600.0F));
}

void a_nested_column_divides_the_panel_it_was_given() {
    gui::LayoutTree tree;
    tree.begin(gui::LayoutStyle{.axis = gui::Axis::Row});
    const gui::NodeId sidebar = tree.begin(gui::LayoutStyle{
        .axis = gui::Axis::Column,
        .width = gui::Size::fixed(240.0F),
        .height = gui::Size::grow()});
    const gui::NodeId top = tree.leaf(
        gui::LayoutStyle{.width = gui::Size::grow(), .height = gui::Size::grow()});
    const gui::NodeId bottom = tree.leaf(
        gui::LayoutStyle{.width = gui::Size::grow(), .height = gui::Size::grow()});
    tree.end();
    tree.leaf(
        gui::LayoutStyle{.width = gui::Size::grow(), .height = gui::Size::grow()});
    tree.end();
    tree.solve(gui::Rect{0.0F, 0.0F, 1000.0F, 600.0F});

    UI_CHECK(same_rect(tree.rect(sidebar), 0.0F, 0.0F, 240.0F, 600.0F));
    UI_CHECK(same_rect(tree.rect(top), 0.0F, 0.0F, 240.0F, 300.0F));
    UI_CHECK(same_rect(tree.rect(bottom), 0.0F, 300.0F, 240.0F, 300.0F));
}

void children_that_do_not_fit_overflow_rather_than_shrink() {
    gui::LayoutTree tree;
    tree.begin(gui::LayoutStyle{.axis = gui::Axis::Row});
    const gui::NodeId a = tree.leaf(gui::LayoutStyle{
        .width = gui::Size::fixed(80.0F), .height = gui::Size::grow()});
    const gui::NodeId b = tree.leaf(gui::LayoutStyle{
        .width = gui::Size::fixed(80.0F), .height = gui::Size::grow()});
    tree.end();
    tree.solve(gui::Rect{0.0F, 0.0F, 100.0F, 50.0F});

    // Both keep their width; the second runs past the edge, for the clip stack
    // to deal with rather than the solver.
    UI_CHECK(near(tree.rect(a).width, 80.0F));
    UI_CHECK(near(tree.rect(b).width, 80.0F));
    UI_CHECK(near(tree.rect(b).x, 80.0F));
}

void a_growing_child_is_never_squeezed_below_its_content() {
    gui::LayoutTree tree;
    tree.begin(gui::LayoutStyle{.axis = gui::Axis::Row});
    // A growing box whose own children need 300px: there is no spare space to
    // share, so it falls back to that floor instead of collapsing.
    const gui::NodeId box = tree.begin(gui::LayoutStyle{
        .axis = gui::Axis::Row,
        .width = gui::Size::grow(),
        .height = gui::Size::grow()});
    tree.leaf(gui::LayoutStyle{.width = gui::Size::fixed(300.0F)});
    tree.end();
    tree.leaf(gui::LayoutStyle{.width = gui::Size::fixed(400.0F)});
    tree.end();
    tree.solve(gui::Rect{0.0F, 0.0F, 400.0F, 100.0F});

    UI_CHECK(near(tree.rect(box).width, 300.0F));
}

void content_is_the_box_inside_the_padding() {
    gui::LayoutTree tree;
    const gui::NodeId root = tree.begin(gui::LayoutStyle{
        .axis = gui::Axis::Column,
        .padding = gui::Padding{4.0F, 8.0F, 12.0F, 16.0F}});
    tree.end();
    tree.solve(gui::Rect{0.0F, 0.0F, 100.0F, 100.0F});

    UI_CHECK(same_rect(tree.rect(root), 0.0F, 0.0F, 100.0F, 100.0F));
    UI_CHECK(same_rect(tree.content(root), 4.0F, 8.0F, 84.0F, 76.0F));
}

void reset_clears_the_tree_for_the_next_frame() {
    gui::LayoutTree tree;
    tree.begin(gui::LayoutStyle{.axis = gui::Axis::Row});
    tree.leaf(gui::LayoutStyle{.width = gui::Size::grow()});
    tree.end();
    UI_CHECK(tree.node_count() == 2U);

    tree.reset();
    UI_CHECK(tree.node_count() == 0U);

    // A rebuilt tree solves on its own terms, with ids starting over.
    const gui::NodeId only = tree.begin(gui::LayoutStyle{.axis = gui::Axis::Row});
    tree.end();
    tree.solve(gui::Rect{10.0F, 20.0F, 30.0F, 40.0F});
    UI_CHECK(same_rect(tree.rect(only), 10.0F, 20.0F, 30.0F, 40.0F));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 13U> cases{{
    {"growing children share the row evenly",
     &growing_children_share_the_row_evenly},
    {"a fixed sidebar keeps its width and the rest grows",
     &a_fixed_sidebar_keeps_its_width_and_the_rest_grows},
    {"weight decides how the spare space splits",
     &weight_decides_how_the_spare_space_splits},
    {"padding and gap come out of the content box",
     &padding_and_gap_come_out_of_the_content_box},
    {"a fitting parent takes its children extent",
     &a_fitting_parent_takes_its_children_extent},
    {"a percentage resolves against the parent content box",
     &a_percentage_resolves_against_the_parent_content_box},
    {"alignment places a child on the cross axis",
     &alignment_places_a_child_on_the_cross_axis},
    {"stretch widens a shrink wrapped child",
     &stretch_widens_a_shrink_wrapped_child},
    {"a nested column divides the panel it was given",
     &a_nested_column_divides_the_panel_it_was_given},
    {"children that do not fit overflow rather than shrink",
     &children_that_do_not_fit_overflow_rather_than_shrink},
    {"a growing child is never squeezed below its content",
     &a_growing_child_is_never_squeezed_below_its_content},
    {"content is the box inside the padding",
     &content_is_the_box_inside_the_padding},
    {"reset clears the tree for the next frame",
     &reset_clears_the_tree_for_the_next_frame},
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
