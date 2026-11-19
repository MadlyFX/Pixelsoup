#include <mediaserver/editor/laser_pointer.hpp>
#include <mediaserver/editor/viewport_panel.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/gui/ui_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/show/pointer_overlay.hpp>

#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// The laser pointer: what L and the two buttons do to its marks, the picture
// the wall is sent, and the pointer driven through headless Program and Stage
// viewports. Nothing here is authored.
namespace {

namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;
namespace show = mediaserver::show;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define LASER_CHECK(...)                                                       \
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

[[nodiscard]] editor::LaserMark canvas_mark(float x, float y) {
    return editor::LaserMark{editor::LaserSpace::Canvas,
                             scene::Vec3{x, y, 0.0F}};
}

void l_and_the_buttons_decide_what_stays() {
    editor::LaserPointer laser;
    // Off until L is held: nothing to draw with.
    LASER_CHECK(!laser.begin_stroke(editor::LaserInk::Kept,
                                    canvas_mark(1.0F, 2.0F)));
    laser.point_at(canvas_mark(5.0F, 5.0F));
    LASER_CHECK(!laser.dot().has_value());

    laser.press();
    LASER_CHECK(laser.active());
    laser.point_at(canvas_mark(5.0F, 6.0F));
    LASER_CHECK(laser.dot().has_value() && near(laser.dot()->at.y, 6.0F));
    // The right button's line, then the left button's.
    LASER_CHECK(laser.begin_stroke(editor::LaserInk::Kept,
                                   canvas_mark(10.0F, 20.0F)));
    LASER_CHECK(laser.drawing());
    // One stroke at a time.
    LASER_CHECK(!laser.begin_stroke(editor::LaserInk::Fleeting,
                                    canvas_mark(0.0F, 0.0F)));
    LASER_CHECK(laser.extend(scene::Vec3{30.0F, 40.0F, 0.0F}));
    laser.end_stroke();
    LASER_CHECK(!laser.extend(scene::Vec3{31.0F, 41.0F, 0.0F}));
    LASER_CHECK(laser.begin_stroke(editor::LaserInk::Fleeting,
                                   canvas_mark(100.0F, 200.0F)));
    LASER_CHECK(laser.extend(scene::Vec3{110.0F, 210.0F, 0.0F}));
    LASER_CHECK(laser.extend(scene::Vec3{120.0F, 230.0F, 0.0F}));
    laser.end_stroke();
    LASER_CHECK(laser.strokes().size() == 2U);
    LASER_CHECK(laser.point_count() == 5U);

    // Letting go: the dot and the left button's line go, the right's stays.
    const auto before = laser.revision();
    laser.release();
    LASER_CHECK(laser.revision() != before);
    LASER_CHECK(!laser.active());
    LASER_CHECK(!laser.dot().has_value());
    LASER_CHECK(laser.strokes().size() == 1U);
    LASER_CHECK(laser.strokes().front().ink == editor::LaserInk::Kept);
    LASER_CHECK(laser.point_count() == 2U);
    LASER_CHECK(near(laser.strokes().front().points.back(),
                     scene::Vec3{30.0F, 40.0F, 0.0F}));

    // Pressing L again clears the kept line.
    laser.press();
    LASER_CHECK(laser.strokes().empty());
    LASER_CHECK(laser.point_count() == 0U);
}

void a_full_pointer_stops_growing() {
    editor::LaserPointer laser;
    laser.press();
    LASER_CHECK(laser.begin_stroke(editor::LaserInk::Fleeting,
                                   canvas_mark(0.0F, 0.0F)));
    std::size_t added = 1U;
    while (laser.extend(scene::Vec3{static_cast<float>(added), 0.0F, 0.0F})) {
        ++added;
    }
    LASER_CHECK(added == show::max_pointer_points);
    LASER_CHECK(laser.full());
    laser.end_stroke();
    LASER_CHECK(!laser.begin_stroke(editor::LaserInk::Kept,
                                    canvas_mark(1.0F, 1.0F)));
    // What it sends still fits a message.
    const scene::Scene view = box_scene();
    LASER_CHECK(show::valid(
        editor::wall_pointer_overlay(laser, view.camera, view.canvas)));
}

void the_wall_sees_canvas_pixels_while_l_is_held() {
    const scene::Scene view = box_scene();
    editor::LaserPointer laser;
    laser.press();
    // A Program line, as drawn.
    LASER_CHECK(laser.begin_stroke(editor::LaserInk::Fleeting,
                                   canvas_mark(100.0F, 200.0F)));
    LASER_CHECK(laser.extend(scene::Vec3{300.0F, 250.0F, 0.0F}));
    laser.end_stroke();
    // A Stage line over the box, carried through Program's camera.
    const scene::Vec3 a{1.0F, 1.0F, 1.0F};
    const scene::Vec3 b{-1.0F, 1.0F, 1.0F};
    LASER_CHECK(laser.begin_stroke(
        editor::LaserInk::Kept, editor::LaserMark{editor::LaserSpace::World, a}));
    LASER_CHECK(laser.extend(b));
    laser.end_stroke();
    laser.point_at(editor::LaserMark{editor::LaserSpace::World,
                                     scene::Vec3{0.0F, 1.0F, 0.0F}});

    const show::PointerOverlay wall =
        editor::wall_pointer_overlay(laser, view.camera, view.canvas);
    LASER_CHECK(show::valid(wall));
    LASER_CHECK(wall.stroke_ends.size() == 2U);
    LASER_CHECK(wall.stroke_ends[0] == 2U && wall.stroke_ends[1] == 4U);
    LASER_CHECK(near(wall.points[0].x, 100.0F) && near(wall.points[1].y, 250.0F));
    const auto pa = pixel_of(view, a);
    const auto pb = pixel_of(view, b);
    LASER_CHECK(near(wall.points[2].x, pa.x, 0.01F) &&
                near(wall.points[2].y, pa.y, 0.01F));
    LASER_CHECK(near(wall.points[3].x, pb.x, 0.01F) &&
                near(wall.points[3].y, pb.y, 0.01F));
    const auto dot = pixel_of(view, scene::Vec3{0.0F, 1.0F, 0.0F});
    LASER_CHECK(wall.dot.has_value() && near(wall.dot->x, dot.x, 0.01F));

    // Let go: the wall is cleared, though the kept line stays in the editor.
    laser.release();
    LASER_CHECK(laser.strokes().size() == 1U);
    LASER_CHECK(
        editor::wall_pointer_overlay(laser, view.camera, view.canvas).empty());
}

void a_line_behind_the_camera_breaks_on_the_wall() {
    const scene::Scene view = box_scene();
    editor::LaserPointer laser;
    laser.press();
    // In front, behind the eye (looking from (4,3,6) toward the origin),
    // then in front again.
    LASER_CHECK(laser.begin_stroke(
        editor::LaserInk::Fleeting,
        editor::LaserMark{editor::LaserSpace::World, scene::Vec3{0.5F, 0.0F, 0.0F}}));
    LASER_CHECK(laser.extend(scene::Vec3{-0.5F, 0.0F, 0.0F}));
    LASER_CHECK(laser.extend(scene::Vec3{8.0F, 6.0F, 12.0F}));
    LASER_CHECK(laser.extend(scene::Vec3{0.0F, 0.5F, 0.0F}));
    laser.end_stroke();
    const show::PointerOverlay wall =
        editor::wall_pointer_overlay(laser, view.camera, view.canvas);
    LASER_CHECK(show::valid(wall));
    LASER_CHECK(wall.points.size() == 3U);
    LASER_CHECK(wall.stroke_ends.size() == 2U);
    LASER_CHECK(wall.stroke_ends[0] == 2U && wall.stroke_ends[1] == 3U);
}

// A headless viewport and its input.
struct Harness final {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    rhi::Texture target;
    editor::ViewportPanel panel;
    scene::Scene view{box_scene()};
    editor::LaserPointer laser;
    gui::Rect bounds{0.0F, 0.0F, 1000.0F, 1000.0F};
    gui::Rect image{};

    explicit Harness(editor::ViewportRole role)
        : panel{gui::ui_id("laser.harness"), role} {}

    std::optional<editor::LaserMark> frame(float x, float y, bool left,
                                           bool right = false) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, left);
        input.set_mouse_button(gui::MouseButton::Right, right);
        ctx.new_frame(0.0);
        // Blocked as the shell blocks it while L is held: the view's own
        // tools stand down and only the pointer runs.
        const editor::ViewportFrame out =
            panel.draw(ctx, bounds, target, view, {}, {}, nullptr, &bounds);
        image = out.image;
        return panel.run_laser(ctx, laser, image, view, nullptr, true);
    }

    // The panel point showing canvas pixel (x, y).
    [[nodiscard]] gui::UiPoint at(float x, float y) const {
        return gui::UiPoint{
            image.x + x / static_cast<float>(view.canvas.width) * image.width,
            image.y + y / static_cast<float>(view.canvas.height) * image.height};
    }
};

void program_draws_in_canvas_pixels() {
    Harness h{editor::ViewportRole::Program};
    static_cast<void>(h.frame(-50.0F, -50.0F, false));
    LASER_CHECK(h.image.width > 0.0F);
    // Over the picture with L up: no dot, the system pointer as it was.
    auto start = h.at(480.0F, 270.0F);
    LASER_CHECK(!h.frame(start.x, start.y, false).has_value());
    LASER_CHECK(h.ctx.cursor() != gui::Cursor::Hidden);

    h.laser.press();
    const auto pointed = h.frame(start.x, start.y, false);
    LASER_CHECK(pointed.has_value());
    LASER_CHECK(pointed->space == editor::LaserSpace::Canvas);
    LASER_CHECK(near(pointed->at, scene::Vec3{480.0F, 270.0F, 0.0F}, 0.5F));
    LASER_CHECK(h.ctx.cursor() == gui::Cursor::Hidden);

    // A left drag draws the line that goes when L is let go.
    static_cast<void>(h.frame(start.x, start.y, true));
    LASER_CHECK(h.laser.drawing());
    const auto middle = h.at(960.0F, 540.0F);
    static_cast<void>(h.frame(middle.x, middle.y, true));
    // A hair's move adds nothing.
    static_cast<void>(h.frame(middle.x + 0.5F, middle.y, true));
    static_cast<void>(h.frame(middle.x + 0.5F, middle.y, false));
    LASER_CHECK(!h.laser.drawing());
    LASER_CHECK(h.laser.strokes().size() == 1U);
    const editor::LaserStroke& left = h.laser.strokes().front();
    LASER_CHECK(left.ink == editor::LaserInk::Fleeting);
    LASER_CHECK(left.space == editor::LaserSpace::Canvas);
    LASER_CHECK(left.points.size() == 2U);
    LASER_CHECK(near(left.points[1], scene::Vec3{960.0F, 540.0F, 0.0F}, 0.5F));

    // A right drag draws the one that stays.
    const auto low = h.at(200.0F, 900.0F);
    static_cast<void>(h.frame(low.x, low.y, false, true));
    const auto lower = h.at(400.0F, 950.0F);
    static_cast<void>(h.frame(lower.x, lower.y, false, true));
    static_cast<void>(h.frame(lower.x, lower.y, false, false));
    LASER_CHECK(h.laser.strokes().size() == 2U);
    LASER_CHECK(h.laser.strokes().back().ink == editor::LaserInk::Kept);

    h.laser.release();
    static_cast<void>(h.frame(lower.x, lower.y, false));
    LASER_CHECK(h.laser.strokes().size() == 1U);
    LASER_CHECK(h.laser.strokes().front().ink == editor::LaserInk::Kept);
    LASER_CHECK(h.ctx.cursor() != gui::Cursor::Hidden);
}

void the_stage_draws_on_the_surface_under_the_pointer() {
    Harness h{editor::ViewportRole::Stage};
    static_cast<void>(h.frame(-50.0F, -50.0F, false));
    // Triangles rather than bounds, as the shipping view has them once
    // built.
    static_cast<void>(h.panel.surface_at(h.view, 960.0F, 540.0F));
    LASER_CHECK(editor::detail::wait_for_viewport_surface_hierarchies_for_test(
        h.panel));
    h.laser.press();
    const scene::Vec3 face{0.4F, -0.5F, 1.0F};
    const auto face_pixel = pixel_of(h.view, face);
    const auto on_face = h.at(face_pixel.x, face_pixel.y);
    static_cast<void>(h.frame(on_face.x, on_face.y, false, true));
    LASER_CHECK(h.laser.drawing());
    // Off the box: the stage floor.
    const scene::Vec3 ground{-3.0F, 0.0F, 2.0F};
    const auto ground_pixel = pixel_of(h.view, ground);
    const auto on_ground = h.at(ground_pixel.x, ground_pixel.y);
    static_cast<void>(h.frame(on_ground.x, on_ground.y, false, true));
    static_cast<void>(h.frame(on_ground.x, on_ground.y, false, false));
    LASER_CHECK(h.laser.strokes().size() == 1U);
    const editor::LaserStroke& stroke = h.laser.strokes().front();
    LASER_CHECK(stroke.space == editor::LaserSpace::World);
    LASER_CHECK(stroke.ink == editor::LaserInk::Kept);
    LASER_CHECK(stroke.points.size() == 2U);
    LASER_CHECK(near(stroke.points[0], face, 0.02F));
    LASER_CHECK(near(stroke.points[1], ground, 0.02F));

    // The view moves; the mark stays on the box. Kept past the release.
    h.laser.release();
    h.view.camera.eye = scene::Vec3{-5.0F, 2.0F, 5.0F};
    static_cast<void>(h.frame(-50.0F, -50.0F, false));
    LASER_CHECK(h.laser.strokes().size() == 1U);
    LASER_CHECK(near(h.laser.strokes().front().points[0], face, 0.02F));
}

void a_warped_view_finds_the_canvas_pixel_under_the_pointer() {
    const scene::Scene view = box_scene();
    // A connector image that bends the canvas: a smooth, non-linear map, as
    // a curved surface's warp is.
    editor::ViewportProjection projection{};
    projection.canvas = scene::Canvas{800U, 600U};
    projection.project = [view](scene::Vec3 world) {
        scene::CanvasProjection p =
            scene::project_to_canvas(view.camera, view.canvas, world);
        const float u = p.x / 1920.0F;
        const float v = p.y / 1080.0F;
        p.x = 800.0F * (u + 0.08F * std::sin(3.0F * v));
        p.y = 600.0F * (v + 0.05F * u * u);
        return p;
    };
    const scene::Vec3 wanted{700.0F, 300.0F, 0.0F};
    const auto shown = editor::ViewportPanel::laser_canvas_to_view(
        view, &projection, wanted.x, wanted.y);
    LASER_CHECK(shown.has_value());
    const auto found = editor::ViewportPanel::laser_view_to_canvas(
        view, projection, shown->x, shown->y,
        scene::Vec3{960.0F, 540.0F, 0.0F});
    LASER_CHECK(found.has_value());
    LASER_CHECK(near(*found, wanted, 0.5F));

    // Without a projection a canvas pixel is shown where it is.
    const auto plain =
        editor::ViewportPanel::laser_canvas_to_view(view, nullptr, 12.0F, 34.0F);
    LASER_CHECK(plain.has_value() && near(plain->x, 12.0F) &&
                near(plain->y, 34.0F));
}

struct Case final {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const std::array cases{
        Case{"L and the buttons decide what stays",
             l_and_the_buttons_decide_what_stays},
        Case{"a full pointer stops growing", a_full_pointer_stops_growing},
        Case{"the wall sees canvas pixels while L is held",
             the_wall_sees_canvas_pixels_while_l_is_held},
        Case{"a line behind the camera breaks on the wall",
             a_line_behind_the_camera_breaks_on_the_wall},
        Case{"Program draws in canvas pixels", program_draws_in_canvas_pixels},
        Case{"the Stage draws on the surface under the pointer",
             the_stage_draws_on_the_surface_under_the_pointer},
        Case{"a warped view finds the canvas pixel under the pointer",
             a_warped_view_finds_the_canvas_pixel_under_the_pointer},
    };
    int failures = 0;
    for (const Case& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - static_cast<std::size_t>(failures)) << '/'
              << cases.size() << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
