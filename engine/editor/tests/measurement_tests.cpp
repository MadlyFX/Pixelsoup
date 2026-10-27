#include <mediaserver/editor/measurement.hpp>
#include <mediaserver/editor/stage_view.hpp>
#include <mediaserver/editor/viewport_panel.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/gui/ui_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The Stage's Measure tool: the reading and its wording, how clicks build a
// measurement or a path, what a point snaps to on real geometry, and the tool
// driven through a headless Stage viewport. Nothing here touches a document:
// a measurement is never authored.
namespace {

namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define MEASURE_CHECK(...)                                                     \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] bool near(float a, float b, float tolerance = 1.0e-3F) noexcept {
    return std::fabs(a - b) < tolerance;
}

[[nodiscard]] bool near(scene::Vec3 a, scene::Vec3 b,
                        float tolerance = 1.0e-3F) noexcept {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) &&
           near(a.z, b.z, tolerance);
}

const doc::NodeId box_node{12U};

// A two-metre box standing at the origin, seen from above and to the side.
[[nodiscard]] scene::Scene box_scene() {
    scene::Scene out{};
    out.canvas = scene::Canvas{1920U, 1080U};
    out.camera.eye = scene::Vec3{4.0F, 3.0F, 6.0F};
    out.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    out.camera.far_plane = 100.0F;
    scene::MeshInstance box{};
    box.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    box.world = scene::scaling(scene::Vec3{2.0F, 2.0F, 2.0F});
    box.source = box_node;
    out.instances.push_back(box);
    return out;
}

[[nodiscard]] scene::CanvasProjection pixel_of(const scene::Scene& view,
                                               scene::Vec3 world) {
    const auto projected =
        scene::project_to_canvas(view.camera, view.canvas, world);
    check(projected.visible, "point in view", __LINE__);
    return projected;
}

[[nodiscard]] editor::MeasurePoint point_at(float x, float y, float z) {
    return editor::MeasurePoint{scene::Vec3{x, y, z},
                                editor::MeasureSnap::Vertex, {}};
}

void a_reading_has_its_length_axis_parts_and_slope() {
    const editor::MeasureReading flat =
        editor::measure(scene::Vec3{1.0F, 0.5F, 0.0F}, scene::Vec3{4.0F, 0.5F, 4.0F});
    MEASURE_CHECK(near(flat.length, 5.0F));
    MEASURE_CHECK(near(flat.delta, scene::Vec3{3.0F, 0.0F, 4.0F}));
    MEASURE_CHECK(near(flat.slope_degrees, 0.0F));
    const editor::MeasureReading ramp =
        editor::measure(scene::Vec3{}, scene::Vec3{0.0F, 2.0F, -2.0F});
    MEASURE_CHECK(near(ramp.slope_degrees, 45.0F));
    const editor::MeasureReading down =
        editor::measure(scene::Vec3{0.0F, 3.0F, 0.0F}, scene::Vec3{});
    MEASURE_CHECK(near(down.slope_degrees, -90.0F));
    MEASURE_CHECK(near(editor::measure(scene::Vec3{}, scene::Vec3{}).slope_degrees,
                       0.0F));

    MEASURE_CHECK(editor::format_metres(3.21449F) == "3.214 m");
    MEASURE_CHECK(editor::format_metres(0.0F) == "0.000 m");
    editor::MeasureReading reading{};
    reading.delta = scene::Vec3{1.2F, -0.0001F, -2.981F};
    reading.slope_degrees = -0.01F;
    MEASURE_CHECK(editor::format_reading_detail(reading) ==
                  "X 1.200  Y 0.000  Z -2.981  slope 0.0 deg");
    reading.delta.y = -0.25F;
    reading.slope_degrees = -4.44F;
    MEASURE_CHECK(editor::format_reading_detail(reading) ==
                  "X 1.200  Y -0.250  Z -2.981  slope -4.4 deg");
}

void clicks_make_a_measurement_and_shift_clicks_a_path() {
    editor::MeasureChain chain;
    chain.click(point_at(0.0F, 0.0F, 0.0F), false);
    MEASURE_CHECK(chain.points.size() == 1U && chain.open);
    chain.click(point_at(3.0F, 0.0F, 0.0F), false);
    // The second click finishes it.
    MEASURE_CHECK(chain.points.size() == 2U && !chain.open);
    // The next starts again.
    chain.click(point_at(0.0F, 1.0F, 0.0F), false);
    MEASURE_CHECK(chain.points.size() == 1U && chain.open);
    MEASURE_CHECK(near(chain.points.front().position.y, 1.0F));
    // Shift adds without finishing, so a path keeps following the pointer.
    chain.click(point_at(0.0F, 1.0F, 2.0F), true);
    chain.click(point_at(3.0F, 1.0F, 2.0F), true);
    MEASURE_CHECK(chain.points.size() == 3U && chain.open);
    chain.click(point_at(3.0F, 5.0F, 2.0F), false);
    MEASURE_CHECK(chain.points.size() == 4U && !chain.open);
    MEASURE_CHECK(near(editor::path_length(chain.points), 9.0F));
    chain.clear();
    MEASURE_CHECK(chain.points.empty() && !chain.open);
}

void points_snap_to_vertices_then_midpoints_then_the_surface() {
    const scene::Scene view = box_scene();
    editor::ViewportPanel panel{gui::ui_id("measure.stage"),
                                editor::ViewportRole::Stage};
    // Three canvas pixels off the box's near top corner.
    const auto corner = pixel_of(view, scene::Vec3{1.0F, 1.0F, 1.0F});
    const auto first = panel.measure_point_at(view, corner.x + 3.0F,
                                              corner.y + 3.0F, 12.0F);
    MEASURE_CHECK(first.has_value());
    // The box's hierarchy is still being built: the surface stands in.
    MEASURE_CHECK(first->snap == editor::MeasureSnap::Surface);
    MEASURE_CHECK(first->element == box_node);
    MEASURE_CHECK(editor::detail::wait_for_viewport_surface_hierarchies_for_test(
        panel));

    const auto vertex = panel.measure_point_at(view, corner.x + 3.0F,
                                               corner.y + 3.0F, 12.0F);
    MEASURE_CHECK(vertex.has_value());
    MEASURE_CHECK(vertex->snap == editor::MeasureSnap::Vertex);
    MEASURE_CHECK(near(vertex->position, scene::Vec3{1.0F, 1.0F, 1.0F}));

    // Out of a vertex's reach but near the middle of the top front edge.
    const auto edge = pixel_of(view, scene::Vec3{0.05F, 1.0F, 1.0F});
    const auto midpoint =
        panel.measure_point_at(view, edge.x, edge.y + 2.0F, 12.0F);
    MEASURE_CHECK(midpoint.has_value());
    MEASURE_CHECK(midpoint->snap == editor::MeasureSnap::EdgeMidpoint);
    MEASURE_CHECK(near(midpoint->position, scene::Vec3{0.0F, 1.0F, 1.0F}));

    // Mid-face, away from every vertex and midpoint: the surface itself.
    const auto face = pixel_of(view, scene::Vec3{0.4F, -0.5F, 1.0F});
    const auto surface = panel.measure_point_at(view, face.x, face.y, 12.0F);
    MEASURE_CHECK(surface.has_value());
    MEASURE_CHECK(surface->snap == editor::MeasureSnap::Surface);
    MEASURE_CHECK(near(surface->position, scene::Vec3{0.4F, -0.5F, 1.0F}, 0.01F));

    // Off the box, the stage floor.
    const auto ground = pixel_of(view, scene::Vec3{-3.0F, 0.0F, 2.0F});
    const auto floor = panel.measure_point_at(view, ground.x, ground.y, 12.0F);
    MEASURE_CHECK(floor.has_value());
    MEASURE_CHECK(floor->snap == editor::MeasureSnap::Floor);
    MEASURE_CHECK(near(floor->position, scene::Vec3{-3.0F, 0.0F, 2.0F}, 0.01F));
}

struct Harness final {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    rhi::Texture target;
    editor::ViewportPanel panel{gui::ui_id("measure.harness"),
                                editor::ViewportRole::Stage};
    scene::Scene view{box_scene()};
    std::vector<doc::NodeId> selection{box_node};
    doc::NodeId primary{box_node};
    gui::Rect image{};

    gui::Rect bounds{0.0F, 0.0F, 1000.0F, 1000.0F};

    editor::ViewportFrame frame(float x, float y, bool down,
                                std::string_view text = {},
                                bool escape = false, bool shift = false,
                                gui::KeyMods mods = gui::KeyMods::None) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        input.set_mods(shift ? mods | gui::KeyMods::Shift : mods);
        if (!text.empty()) {
            input.add_text(text);
        }
        if (escape) {
            input.add_key(gui::Key::Escape);
        }
        ctx.new_frame(0.0);
        editor::ViewportFrame out =
            panel.draw(ctx, bounds, target, view, selection, primary);
        ctx.draw_overlays(gui::Rect{0.0F, 0.0F, 2'000.0F, 2'000.0F});
        image = out.image;
        return out;
    }

    editor::ViewportFrame middle(float x, float y, bool down,
                                 std::string_view text = {}) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Middle, down);
        input.set_mods(gui::KeyMods::None);
        if (!text.empty()) {
            input.add_text(text);
        }
        ctx.new_frame(0.0);
        editor::ViewportFrame out =
            panel.draw(ctx, bounds, target, view, selection, primary);
        image = out.image;
        return out;
    }

    // The interface position of a world point in the letterboxed image.
    [[nodiscard]] gui::UiPoint at(scene::Vec3 world) const {
        const auto projected = pixel_of(view, world);
        return gui::UiPoint{
            image.x + projected.x / static_cast<float>(view.canvas.width) *
                          image.width,
            image.y + projected.y / static_cast<float>(view.canvas.height) *
                          image.height};
    }

    // Two pixels in from the point towards the box's middle, so a click by a
    // corner lands on the box rather than past its silhouette.
    editor::ViewportFrame click(scene::Vec3 world, bool shift = false) {
        const gui::UiPoint p = at(world);
        const gui::UiPoint middle = at(scene::Vec3{});
        const float dx = middle.x - p.x;
        const float dy = middle.y - p.y;
        const float reach = std::sqrt(dx * dx + dy * dy);
        const float x = p.x + (reach > 0.0F ? dx / reach * 2.0F : 0.0F);
        const float y = p.y + (reach > 0.0F ? dy / reach * 2.0F : 0.0F);
        static_cast<void>(frame(x, y, false, {}, false, shift));
        const auto pressed = frame(x, y, true, {}, false, shift);
        static_cast<void>(frame(x, y, false, {}, false, shift));
        return pressed;
    }
};

void the_stage_measures_between_clicked_vertices_without_selecting() {
    Harness h;
    const float middle_x = 500.0F;
    const float middle_y = 500.0F;
    static_cast<void>(h.frame(middle_x, middle_y, false));
    MEASURE_CHECK(!h.panel.measuring());
    // M over the Stage picks up the tool.
    static_cast<void>(h.frame(middle_x, middle_y, false, "m"));
    MEASURE_CHECK(h.panel.measuring());
    // Hovering builds the box's hierarchy; wait for it so vertices snap.
    static_cast<void>(h.frame(h.at(scene::Vec3{1.0F, 1.0F, 1.0F}).x,
                              h.at(scene::Vec3{1.0F, 1.0F, 1.0F}).y, false));
    MEASURE_CHECK(editor::detail::wait_for_viewport_surface_hierarchies_for_test(
        h.panel));

    const editor::ViewportFrame first = h.click(scene::Vec3{1.0F, 1.0F, 1.0F});
    // A measuring click neither picks nor hands the drag to the camera.
    MEASURE_CHECK(!first.picked.has_value());
    MEASURE_CHECK(first.empty_canvas == editor::EmptyCanvasInteraction::None);
    MEASURE_CHECK(!first.dragging && !first.transform.has_value());
    MEASURE_CHECK(h.panel.measurement().points.size() == 1U);

    static_cast<void>(h.click(scene::Vec3{-1.0F, 1.0F, 1.0F}));
    const editor::MeasureChain& done = h.panel.measurement();
    MEASURE_CHECK(done.points.size() == 2U && !done.open);
    MEASURE_CHECK(done.points[0].snap == editor::MeasureSnap::Vertex);
    MEASURE_CHECK(done.points[1].snap == editor::MeasureSnap::Vertex);
    MEASURE_CHECK(near(editor::measure(done.points[0].position,
                                       done.points[1].position).length,
                       2.0F));

    // Shift+clicks measure a path down the box's edge and along its foot.
    static_cast<void>(h.click(scene::Vec3{1.0F, 1.0F, 1.0F}));
    static_cast<void>(h.click(scene::Vec3{1.0F, -1.0F, 1.0F}, true));
    static_cast<void>(h.click(scene::Vec3{1.0F, -1.0F, -1.0F}, true));
    MEASURE_CHECK(h.panel.measurement().points.size() == 3U);
    MEASURE_CHECK(near(editor::path_length(h.panel.measurement().points), 4.0F));

    // Escape clears, then leaves the tool.
    static_cast<void>(h.frame(middle_x, middle_y, false, {}, true));
    MEASURE_CHECK(h.panel.measurement().points.empty());
    MEASURE_CHECK(h.panel.measuring());
    static_cast<void>(h.frame(middle_x, middle_y, false, {}, true));
    MEASURE_CHECK(!h.panel.measuring());

    // Out of the tool, the same click picks the box again.
    const auto picked = h.click(scene::Vec3{0.3F, 0.2F, 1.0F});
    MEASURE_CHECK(picked.picked.has_value() && *picked.picked == box_node);
}

const doc::NodeId crate_node{30U};

// A half-metre crate on the floor beside the box, selected for dragging.
[[nodiscard]] Harness& with_crate(Harness& h) {
    scene::MeshInstance crate{};
    crate.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    crate.world = scene::multiply(
        scene::translation(scene::Vec3{3.0F, 0.25F, 0.0F}),
        scene::scaling(scene::Vec3{0.5F, 0.5F, 0.5F}));
    crate.source = crate_node;
    h.view.instances.push_back(crate);
    h.selection = {crate_node};
    h.primary = crate_node;
    return h;
}

void surface_snapping_rests_a_dragged_element_on_what_is_under_the_pointer() {
    Harness h;
    static_cast<void>(with_crate(h));
    h.panel.set_surface_snapping(true);
    // Unselected, so the press takes the crate's body rather than a gizmo
    // handle drawn over it.
    h.selection.clear();
    h.primary = {};
    static_cast<void>(h.frame(500.0F, 500.0F, false));
    const gui::UiPoint grab = h.at(scene::Vec3{3.0F, 0.25F, 0.25F});
    const gui::UiPoint onto = h.at(scene::Vec3{0.2F, 1.0F, 0.3F});
    static_cast<void>(h.frame(grab.x, grab.y, false));
    const auto pressed = h.frame(grab.x, grab.y, true);
    MEASURE_CHECK(pressed.picked.has_value() && *pressed.picked == crate_node);
    const auto dragged = h.frame(onto.x, onto.y, true);
    MEASURE_CHECK(dragged.transform.has_value());
    // The crate's foot lands on the box top where the pointer is.
    const scene::Vec3 landed =
        scene::Vec3{3.0F, 0.25F, 0.0F} + dragged.transform->translation;
    MEASURE_CHECK(near(landed, scene::Vec3{0.2F, 1.25F, 0.3F}, 0.01F));
    static_cast<void>(h.frame(onto.x, onto.y, false));

    // Off, the same drag stays at the depth it was grabbed at.
    h.panel.set_surface_snapping(false);
    static_cast<void>(h.frame(grab.x, grab.y, false));
    static_cast<void>(h.frame(grab.x, grab.y, true));
    const auto free = h.frame(onto.x, onto.y, true);
    MEASURE_CHECK(free.transform.has_value());
    MEASURE_CHECK(!near(free.transform->translation.y, 1.0F, 0.05F));
    static_cast<void>(h.frame(onto.x, onto.y, false));
}

void holding_v_puts_the_dragged_vertex_on_the_vertex_under_the_pointer() {
    Harness h;
    static_cast<void>(with_crate(h));
    static_cast<void>(h.frame(500.0F, 500.0F, false));
    // Let the box's hierarchy build, as hovering it would.
    static_cast<void>(h.panel.measure_point_at(h.view, 0.0F, 0.0F, 1.0F));
    const auto box_corner = pixel_of(h.view, scene::Vec3{1.0F, 1.0F, 1.0F});
    static_cast<void>(h.panel.measure_point_at(h.view, box_corner.x,
                                               box_corner.y, 12.0F));
    MEASURE_CHECK(editor::detail::wait_for_viewport_surface_hierarchies_for_test(
        h.panel));

    // Grab the crate just inside its top front right corner.
    const gui::UiPoint corner = h.at(scene::Vec3{3.25F, 0.5F, 0.25F});
    const gui::UiPoint middle = h.at(scene::Vec3{3.0F, 0.25F, 0.0F});
    const float grab_x = corner.x + (middle.x - corner.x) * 0.06F;
    const float grab_y = corner.y + (middle.y - corner.y) * 0.06F;
    static_cast<void>(h.frame(grab_x, grab_y, false));
    MEASURE_CHECK(h.frame(grab_x, grab_y, true).picked.has_value());
    h.input.set_move_key(gui::MoveKey::V, true);
    const gui::UiPoint onto = h.at(scene::Vec3{1.0F, 1.0F, 1.0F});
    const auto snapped = h.frame(onto.x + 4.0F, onto.y + 3.0F, true);
    MEASURE_CHECK(snapped.transform.has_value());
    MEASURE_CHECK(near(snapped.transform->translation,
                       scene::Vec3{1.0F, 1.0F, 1.0F} -
                           scene::Vec3{3.25F, 0.5F, 0.25F}));
    h.input.set_move_key(gui::MoveKey::V, false);
    static_cast<void>(h.frame(onto.x, onto.y, false));
}

void ctrl_alt_drag_boxes_a_selection_and_alt_on_a_gizmo_duplicates() {
    Harness h;
    static_cast<void>(with_crate(h));
    h.selection.clear();
    h.primary = {};
    static_cast<void>(h.frame(500.0F, 500.0F, false));
    const gui::UiPoint crate = h.at(scene::Vec3{3.0F, 0.25F, 0.0F});
    const auto boxed = [&](bool shift) {
        const gui::KeyMods chord = gui::KeyMods::Ctrl | gui::KeyMods::Alt;
        const auto pressed = h.frame(crate.x - 10.0F, crate.y - 10.0F, true, {},
                                     false, shift, chord);
        // A box never picks, and never hands the drag to the camera.
        check(pressed.pressed, "box press registers", __LINE__);
        check(!pressed.picked.has_value() &&
                  pressed.empty_canvas == editor::EmptyCanvasInteraction::None,
              "box press", __LINE__);
        static_cast<void>(h.frame(crate.x + 10.0F, crate.y + 10.0F, true, {},
                                  false, shift, chord));
        return h.frame(crate.x + 10.0F, crate.y + 10.0F, false, {}, false,
                       shift, chord);
    };
    const auto replaced = boxed(false);
    MEASURE_CHECK(replaced.marquee.has_value());
    MEASURE_CHECK((*replaced.marquee == std::vector<doc::NodeId>{crate_node}));
    MEASURE_CHECK(!replaced.marquee_adds);
    MEASURE_CHECK(boxed(true).marquee_adds);

    // Alt on a gizmo handle duplicates what it moves; Alt elsewhere orbits.
    h.selection = {box_node};
    h.primary = box_node;
    // Partway along the X arrow: the gizmo's middle is left to body drags.
    const gui::UiPoint origin = h.at(scene::Vec3{});
    const gui::UiPoint toward = h.at(scene::Vec3{0.1F, 0.0F, 0.0F});
    const float run = std::sqrt((toward.x - origin.x) * (toward.x - origin.x) +
                                (toward.y - origin.y) * (toward.y - origin.y));
    const gui::UiPoint pivot{origin.x + (toward.x - origin.x) / run * 45.0F,
                             origin.y + (toward.y - origin.y) / run * 45.0F};
    static_cast<void>(h.frame(pivot.x, pivot.y, false));
    const auto alt_gizmo = h.frame(pivot.x, pivot.y, true, {}, false, false,
                                   gui::KeyMods::Alt);
    MEASURE_CHECK(alt_gizmo.duplicate);
    MEASURE_CHECK(alt_gizmo.picked.has_value() && *alt_gizmo.picked == box_node);
    static_cast<void>(h.frame(pivot.x, pivot.y, false));
    const auto plain = h.frame(pivot.x, pivot.y, true);
    MEASURE_CHECK(!plain.duplicate);
    static_cast<void>(h.frame(pivot.x, pivot.y, false));
    const float sky_x = h.image.x + 20.0F;
    const float sky_y = h.image.y + 20.0F;
    static_cast<void>(h.frame(sky_x, sky_y, false));
    const auto alt_canvas = h.frame(sky_x, sky_y, true, {}, false, false,
                                    gui::KeyMods::Alt);
    MEASURE_CHECK(!alt_canvas.duplicate);
    MEASURE_CHECK(alt_canvas.empty_canvas ==
                  editor::EmptyCanvasInteraction::Navigate);
    static_cast<void>(h.frame(sky_x, sky_y, false));
}

void a_middle_drag_from_the_gizmo_centre_moves_the_pivot_to_a_vertex() {
    Harness h;
    static_cast<void>(h.frame(500.0F, 500.0F, false));
    static_cast<void>(h.panel.measure_point_at(
        h.view, 960.0F, 540.0F, 12.0F));
    MEASURE_CHECK(editor::detail::wait_for_viewport_surface_hierarchies_for_test(
        h.panel));
    const gui::UiPoint centre = h.at(scene::Vec3{});
    const gui::UiPoint corner = h.at(scene::Vec3{1.0F, 1.0F, 1.0F});
    static_cast<void>(h.frame(centre.x, centre.y, false));
    const auto pressed = h.middle(centre.x, centre.y, true);
    MEASURE_CHECK(pressed.pivot_editing);
    static_cast<void>(h.middle(corner.x + 3.0F, corner.y + 4.0F, true));
    static_cast<void>(h.middle(corner.x + 3.0F, corner.y + 4.0F, false));
    MEASURE_CHECK(h.panel.pivot_override().has_value());
    MEASURE_CHECK(near(*h.panel.pivot_override(), scene::Vec3{1.0F, 1.0F, 1.0F}));

    // R now turns the selection about that corner.
    const auto turning = h.frame(500.0F, 500.0F, false, "r");
    MEASURE_CHECK(turning.transform_began && turning.transform.has_value());
    MEASURE_CHECK(near(turning.transform->pivot, scene::Vec3{1.0F, 1.0F, 1.0F}));
    static_cast<void>(h.frame(500.0F, 500.0F, false, {}, true));

    // A middle click on the moved gizmo's centre puts the pivot back.
    static_cast<void>(h.middle(corner.x, corner.y, true));
    static_cast<void>(h.middle(corner.x, corner.y, false));
    MEASURE_CHECK(!h.panel.pivot_override().has_value());

    // And a moved pivot is forgotten when the selection changes.
    static_cast<void>(h.middle(centre.x, centre.y, true));
    static_cast<void>(h.middle(corner.x + 3.0F, corner.y + 4.0F, true));
    static_cast<void>(h.middle(corner.x + 3.0F, corner.y + 4.0F, false));
    MEASURE_CHECK(h.panel.pivot_override().has_value());
    h.selection.clear();
    h.primary = {};
    static_cast<void>(h.frame(500.0F, 500.0F, false));
    MEASURE_CHECK(!h.panel.pivot_override().has_value());
}

void the_bounds_overlay_outlines_every_element_only_when_on() {
    Harness h;
    static_cast<void>(with_crate(h));
    h.selection.clear();
    h.primary = {};
    const auto outline_vertices = [&] {
        return std::count_if(h.list.vertices().begin(), h.list.vertices().end(),
                             [](const auto& vertex) {
                                 return near(vertex.r, 0.45F) &&
                                        near(vertex.g, 0.78F) &&
                                        near(vertex.b, 0.86F) &&
                                        near(vertex.a, 0.55F);
                             });
    };
    static_cast<void>(h.frame(500.0F, 500.0F, false));
    MEASURE_CHECK(outline_vertices() == 0);
    h.panel.set_bounds_visible(true);
    static_cast<void>(h.frame(500.0F, 500.0F, false));
    // Twelve edges a box, each a quad, for the box and the crate.
    MEASURE_CHECK(outline_vertices() >= 2 * 12 * 4);
}

void a_narrow_stage_keeps_its_display_toggles_in_a_show_menu() {
    Harness h;
    h.bounds = gui::Rect{0.0F, 0.0F, 800.0F, 700.0F};
    static_cast<void>(h.frame(400.0F, 400.0F, false));
    MEASURE_CHECK(h.panel.grid_visible());
    // SHOW sits after MOVE..MEASURE, GLOBAL, SNAP, the steps and SURFACE.
    const float show_x = 7.0F + 50.0F + 60.0F + 52.0F + 68.0F + 6.0F + 60.0F +
                         48.0F + 128.0F + 68.0F + 6.0F + 26.0F;
    static_cast<void>(h.frame(show_x, 14.0F, true));
    static_cast<void>(h.frame(show_x, 14.0F, false));
    // The first entry, just under the button.
    static_cast<void>(h.frame(show_x, 38.0F, false));
    static_cast<void>(h.frame(show_x, 38.0F, true));
    static_cast<void>(h.frame(show_x, 38.0F, false));
    MEASURE_CHECK(!h.panel.grid_visible());
}

// Each of the Stage's four views - Top, Front, Side and Perspective - finds
// the surface under a pixel through its own camera, and its Measure point
// snaps to the vertex there. The orthographic views' rays are parallel, so a
// pixel near a corner must still reach that corner's face, not the eye.
void every_stage_view_picks_and_snaps_through_its_own_camera() {
    scene::Scene view = box_scene();
    view.canvas = scene::Canvas{1200U, 800U};
    editor::ViewportPanel panel{gui::ui_id("measure.views"),
                                editor::ViewportRole::Stage};
    // Build the box's hierarchy first; until then only bounds stand in.
    static_cast<void>(panel.measure_point_at(view, 600.0F, 400.0F, 12.0F));
    MEASURE_CHECK(editor::detail::wait_for_viewport_surface_hierarchies_for_test(
        panel));

    editor::StageOrbit orbit{{0.3F, 0.2F, -0.1F}, 0.1F, 0.06F, 7.0F};
    struct Expected final {
        editor::StageProjection projection;
        // A face this view sees, a point on it away from every vertex and
        // midpoint, and one of its corners.
        scene::Vec3 normal;
        scene::Vec3 face;
        scene::Vec3 corner;
    };
    constexpr std::array<Expected, 4U> views{{
        {editor::StageProjection::Top, {0.0F, 1.0F, 0.0F},
         {0.4F, 1.0F, -0.3F}, {1.0F, 1.0F, 1.0F}},
        {editor::StageProjection::Front, {0.0F, 0.0F, 1.0F},
         {0.4F, -0.3F, 1.0F}, {-1.0F, 1.0F, 1.0F}},
        {editor::StageProjection::Side, {1.0F, 0.0F, 0.0F},
         {1.0F, -0.3F, 0.4F}, {1.0F, -1.0F, -1.0F}},
        {editor::StageProjection::Perspective, {0.0F, 1.0F, 0.0F},
         {0.4F, 1.0F, -0.3F}, {1.0F, 1.0F, 1.0F}},
    }};
    for (const Expected& expected : views) {
        orbit.projection = expected.projection;
        view.camera = editor::stage_camera_for(scene::Camera{}, orbit);
        MEASURE_CHECK(scene::orthographic(view.camera) ==
                      editor::stage_orthographic(expected.projection));

        const auto face = pixel_of(view, expected.face);
        const auto hit = panel.surface_at(view, face.x, face.y);
        MEASURE_CHECK(hit.has_value() && hit->node == box_node);
        MEASURE_CHECK(near(hit->position, expected.face, 0.01F));
        MEASURE_CHECK(near(hit->normal, expected.normal, 0.01F));

        // A pixel just inside the face near its corner: the vertex.
        const scene::Vec3 centre = expected.normal;
        const auto inside = pixel_of(
            view, expected.corner + (centre - expected.corner) * 0.04F);
        const auto point =
            panel.measure_point_at(view, inside.x, inside.y, 12.0F);
        MEASURE_CHECK(point.has_value());
        MEASURE_CHECK(point->snap == editor::MeasureSnap::Vertex);
        MEASURE_CHECK(point->element == box_node);
        // In Top the bottom corner beneath it lands on the same pixel; the
        // one on the face under the pointer is the one taken.
        MEASURE_CHECK(near(point->position, expected.corner));
    }
}

struct Case final {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const std::array cases{
        Case{"a reading has its length, axis parts and slope",
             a_reading_has_its_length_axis_parts_and_slope},
        Case{"clicks make a measurement and shift-clicks a path",
             clicks_make_a_measurement_and_shift_clicks_a_path},
        Case{"points snap to vertices, then midpoints, then the surface",
             points_snap_to_vertices_then_midpoints_then_the_surface},
        Case{"the Stage measures between clicked vertices without selecting",
             the_stage_measures_between_clicked_vertices_without_selecting},
        Case{"surface snapping rests a dragged element on what is under the pointer",
             surface_snapping_rests_a_dragged_element_on_what_is_under_the_pointer},
        Case{"holding V puts the dragged vertex on the vertex under the pointer",
             holding_v_puts_the_dragged_vertex_on_the_vertex_under_the_pointer},
        Case{"Ctrl+Alt drag boxes a selection and Alt on a gizmo duplicates",
             ctrl_alt_drag_boxes_a_selection_and_alt_on_a_gizmo_duplicates},
        Case{"a middle drag from the gizmo centre moves the pivot to a vertex",
             a_middle_drag_from_the_gizmo_centre_moves_the_pivot_to_a_vertex},
        Case{"the bounds overlay outlines every element only when on",
             the_bounds_overlay_outlines_every_element_only_when_on},
        Case{"a narrow Stage keeps its display toggles in a SHOW menu",
             a_narrow_stage_keeps_its_display_toggles_in_a_show_menu},
        Case{"every Stage view picks and snaps through its own camera",
             every_stage_view_picks_and_snaps_through_its_own_camera},
    };
    int failures = 0;
    for (const Case& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
