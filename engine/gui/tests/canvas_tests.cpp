#include <mediaserver/gui/canvas_view.hpp>
#include <mediaserver/gui/shapes.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/gui/ui_renderer.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

// What a canvas draws by points rather than boxes: the pan/zoom view a node
// graph maps through, the shared shapes, and the icon glyphs. The view is
// arithmetic; the shapes and glyphs are asserted by the vertices they emit,
// so a glyph drawn about the wrong origin or a shape on the wrong side of its
// point fails without a GPU.
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

[[nodiscard]] bool near(float a, float b, float tolerance = 0.01F) noexcept {
    return std::fabs(a - b) < tolerance;
}

[[nodiscard]] bool same_rect(
    const gui::Rect& r, float x, float y, float w, float h) noexcept {
    return near(r.x, x) && near(r.y, y) && near(r.width, w)
        && near(r.height, h);
}

// The extent of the vertices emitted since `first`.
[[nodiscard]] gui::Rect extent(const gui::UiDrawList& list, std::size_t first) {
    float min_x = 1.0e9F, min_y = 1.0e9F, max_x = -1.0e9F, max_y = -1.0e9F;
    const auto& vertices = list.vertices();
    for (std::size_t index = first; index < vertices.size(); ++index) {
        min_x = std::min(min_x, vertices[index].x);
        min_y = std::min(min_y, vertices[index].y);
        max_x = std::max(max_x, vertices[index].x);
        max_y = std::max(max_y, vertices[index].y);
    }
    return gui::Rect{min_x, min_y, max_x - min_x, max_y - min_y};
}

[[nodiscard]] gui::UiDrawList fresh_list() {
    gui::UiDrawList list;
    list.reset();
    list.push_clip(0.0F, 0.0F, 1000.0F, 1000.0F);
    return list;
}

// A view panned and zoomed by values unlike its defaults, over a canvas that
// does not start at the origin, so a mapping that forgot either shows.
constexpr gui::Rect canvas{40.0F, 30.0F, 600.0F, 400.0F};

void the_view_maps_graph_to_screen_and_back() {
    const gui::CanvasView view{25.0F, -15.0F, 1.5F};
    const gui::UiPoint at = view.to_screen(canvas, 100.0F, 80.0F);
    UI_CHECK(near(at.x, 40.0F + 25.0F + 150.0F));
    UI_CHECK(near(at.y, 30.0F - 15.0F + 120.0F));
    const gui::UiPoint back = view.to_graph(canvas, at.x, at.y);
    UI_CHECK(near(back.x, 100.0F) && near(back.y, 80.0F));

    UI_CHECK(same_rect(view.to_screen(canvas, gui::Rect{100.0F, 80.0F, 20.0F, 10.0F}),
                       at.x, at.y, 30.0F, 15.0F));
    // The canvas shows from the graph point at its top left, its size over
    // the zoom.
    UI_CHECK(same_rect(view.visible(canvas), -25.0F / 1.5F, 15.0F / 1.5F,
                       400.0F, 400.0F / 1.5F));

    const gui::CanvasView none{10.0F, 10.0F, 0.0F};
    UI_CHECK(near(none.to_graph(canvas, 200.0F, 200.0F).x, 0.0F));
    UI_CHECK(same_rect(none.visible(canvas), 0.0F, 0.0F, 0.0F, 0.0F));
}

void zooming_keeps_the_anchor_still_and_centring_centres() {
    gui::CanvasView view{25.0F, -15.0F, 1.5F};
    const gui::UiPoint held = view.to_graph(canvas, 310.0F, 220.0F);
    view.zoom_about(canvas, 310.0F, 220.0F, 0.6F);
    UI_CHECK(near(view.zoom, 0.6F));
    const gui::UiPoint still = view.to_screen(canvas, held.x, held.y);
    UI_CHECK(near(still.x, 310.0F) && near(still.y, 220.0F));

    view.centre_on(canvas, -50.0F, 70.0F);
    const gui::UiPoint centre = view.to_screen(canvas, -50.0F, 70.0F);
    UI_CHECK(near(centre.x, canvas.center_x()) && near(centre.y, canvas.center_y()));
    UI_CHECK(near(view.zoom, 0.6F));
}

void a_disc_and_a_ring_surround_their_centre() {
    gui::UiDrawList list = fresh_list();
    gui::add_disc(list, gui::UiPoint{200.0F, 100.0F}, 5.0F, gui::UiColor{});
    // Twelve sides, fanned from the first: ten triangles.
    UI_CHECK(list.vertices().size() == 30U);
    const gui::Rect disc = extent(list, 0U);
    UI_CHECK(near(disc.x, 195.0F) && near(disc.right(), 205.0F));
    UI_CHECK(near(disc.center_y(), 100.0F, 0.1F));
    // More sides, more triangles in the fan.
    const std::size_t sided = list.vertices().size();
    gui::add_disc(list, gui::UiPoint{200.0F, 100.0F}, 5.0F, gui::UiColor{}, 14U);
    UI_CHECK(list.vertices().size() - sided == 36U);

    const std::size_t before = list.vertices().size();
    gui::add_ring(list, gui::UiPoint{200.0F, 100.0F}, 8.0F, 2.0F, gui::UiColor{});
    const gui::Rect ring = extent(list, before);
    UI_CHECK(ring.width > 16.0F && ring.width < 22.0F);
    UI_CHECK(near(ring.center_x(), 200.0F, 0.2F) && near(ring.center_y(), 100.0F, 0.2F));
}

void an_arc_runs_from_its_start_clockwise() {
    gui::UiDrawList list = fresh_list();
    // A quarter from +x round to +y: the lower right of the circle only.
    gui::add_arc(list, gui::UiPoint{100.0F, 100.0F}, 20.0F, 0.0F, 1.5707963F, 2.0F,
                 gui::UiColor{}, 8U);
    const gui::Rect quarter = extent(list, 0U);
    UI_CHECK(quarter.x > 95.0F && quarter.y > 95.0F);
    UI_CHECK(quarter.right() > 119.0F && quarter.bottom() > 119.0F);

    const std::size_t before = list.vertices().size();
    gui::add_arc(list, gui::UiPoint{100.0F, 100.0F}, 20.0F, 0.0F, 1.0F, 2.0F,
                 gui::UiColor{}, 0U);
    UI_CHECK(list.vertices().size() == before);
}

void an_arrowhead_points_along_its_direction() {
    gui::UiDrawList list = fresh_list();
    gui::add_arrowhead(list, gui::UiPoint{100.0F, 50.0F}, gui::UiPoint{0.0F, 3.0F},
                       10.0F, 4.0F, gui::UiColor{});
    UI_CHECK(list.vertices().size() == 3U);
    // Pointing down: the tip is the lowest point, the base 10 above it and
    // 4 either side.
    UI_CHECK(same_rect(extent(list, 0U), 96.0F, 40.0F, 8.0F, 10.0F));
    UI_CHECK(near(list.vertices()[0].x, 100.0F) && near(list.vertices()[0].y, 50.0F));

    gui::add_arrowhead(list, gui::UiPoint{100.0F, 50.0F}, gui::UiPoint{}, 10.0F, 4.0F,
                       gui::UiColor{});
    UI_CHECK(list.vertices().size() == 3U);
}

// Every icon draws something, about the point it is given: inside a square
// a little larger than the 14px it is designed for, and not lopsided.
void every_icon_draws_about_its_centre() {
    for (int value = 0; value <= static_cast<int>(gui::Icon::PlayRange); ++value) {
        gui::UiDrawList list = fresh_list();
        gui::draw_icon(list, static_cast<gui::Icon>(value), 300.0F, 200.0F,
                       gui::UiColor{1.0F, 1.0F, 1.0F, 1.0F});
        UI_CHECK(!list.vertices().empty());
        const gui::Rect drawn = extent(list, 0U);
        const std::string which = "icon " + std::to_string(value);
        check(drawn.x >= 291.0F && drawn.right() <= 309.0F && drawn.y >= 191.0F
                  && drawn.bottom() <= 209.0F,
              which.c_str(), __LINE__);
        // Minus is a bar: wide and, by design, flat.
        const bool flat = static_cast<gui::Icon>(value) == gui::Icon::Minus;
        check(drawn.width >= 5.0F && (flat || drawn.height >= 5.0F),
              which.c_str(), __LINE__);
    }
    // Drawn at twice the size, the same glyph covers twice the extent about
    // the same centre. The plus is whole units, so pixel snapping leaves it.
    gui::UiDrawList once = fresh_list();
    gui::draw_icon(once, gui::Icon::Plus, 300.0F, 200.0F, gui::UiColor{});
    gui::UiDrawList twice = fresh_list();
    gui::draw_icon(twice, gui::Icon::Plus, 300.0F, 200.0F, gui::UiColor{}, 2.0F);
    UI_CHECK(same_rect(extent(once, 0U), 295.0F, 195.0F, 10.0F, 10.0F));
    UI_CHECK(same_rect(extent(twice, 0U), 290.0F, 190.0F, 20.0F, 20.0F));
}

} // namespace

int main() {
    using Case = void (*)();
    constexpr struct {
        const char* name;
        Case run;
    } cases[] = {
        {"the_view_maps_graph_to_screen_and_back", the_view_maps_graph_to_screen_and_back},
        {"zooming_keeps_the_anchor_still_and_centring_centres",
         zooming_keeps_the_anchor_still_and_centring_centres},
        {"a_disc_and_a_ring_surround_their_centre", a_disc_and_a_ring_surround_their_centre},
        {"an_arc_runs_from_its_start_clockwise", an_arc_runs_from_its_start_clockwise},
        {"an_arrowhead_points_along_its_direction", an_arrowhead_points_along_its_direction},
        {"every_icon_draws_about_its_centre", every_icon_draws_about_its_centre},
    };

    std::size_t failures = 0U;
    for (const auto& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0U ? 0 : 1;
}
