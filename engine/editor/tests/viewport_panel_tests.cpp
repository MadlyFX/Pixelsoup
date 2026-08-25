#include <mediaserver/editor/viewport_panel.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

// The ported viewport's proof, with no GPU and no window.
//
// The panel draws an engine texture, but nothing here renders: the draw list
// only records which texture a quad samples, so an unbacked handle is enough to
// exercise every line of the interaction. What is under test is the part that
// would be wrong silently - where a click lands in canvas pixels once the image
// has been letterboxed, which clip that picks, and what world translation a drag
// across the panel resolves to.
namespace {

namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;
namespace doc = mediaserver::doc;

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

const doc::NodeId clip_node{7U};
const doc::NodeId other_node{9U};

// A camera looking down -z at the origin from eight units back, and a canvas
// that is a plain 16:9. Enough for a clip at the origin to project to the
// middle of the image.
[[nodiscard]] scene::Scene make_scene() {
    scene::Scene out{};
    out.canvas = scene::Canvas{1920U, 1080U};
    out.camera.eye = scene::Vec3{0.0F, 0.0F, 8.0F};
    out.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    out.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};

    scene::MediaInstance clip{};
    // A two-unit quad at the origin, which covers the middle of the view.
    clip.world = scene::scaling(scene::Vec3{2.0F, 2.0F, 1.0F});
    clip.source_node = clip_node;
    out.media.push_back(clip);
    return out;
}

// Everything one headless frame needs. The texture is an empty handle: the draw
// list stores the pointer and never touches it without a renderer.
struct Harness final {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    rhi::Texture target;
    editor::ViewportPanel panel{gui::ui_id("wall")};
    scene::Scene scene{make_scene()};

    // A 1000x1000 panel, so a 16:9 canvas letterboxes with bars top and bottom
    // and the mapping has something non-trivial to get right.
    static constexpr gui::Rect bounds{0.0F, 0.0F, 1000.0F, 1000.0F};

    editor::ViewportFrame frame(float x, float y, bool down) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        ctx.new_frame();
        return panel.draw(ctx, bounds, target, scene);
    }
};

void the_render_is_letterboxed_to_the_canvas_aspect() {
    Harness h;
    const editor::ViewportFrame frame = h.frame(0.0F, 0.0F, false);

    // 16:9 inside a square: full width, and centred vertically.
    UI_CHECK(near(frame.image.width, 1000.0F));
    UI_CHECK(near(frame.image.height, 562.5F));
    UI_CHECK(near(frame.image.x, 0.0F));
    UI_CHECK(near(frame.image.y, (1000.0F - 562.5F) * 0.5F));
}

void a_pointer_maps_through_the_letterbox_to_canvas_pixels() {
    Harness h;
    const editor::ViewportFrame placed = h.frame(0.0F, 0.0F, false);
    const gui::Rect image = placed.image;

    // The middle of the image is the middle of the canvas.
    const editor::CanvasPoint centre = editor::to_canvas(
        image, h.scene.canvas, image.x + image.width * 0.5F,
        image.y + image.height * 0.5F);
    UI_CHECK(centre.inside);
    UI_CHECK(near(centre.x, 960.0F));
    UI_CHECK(near(centre.y, 540.0F));

    // The top-left corner of the image is the canvas origin.
    const editor::CanvasPoint corner =
        editor::to_canvas(image, h.scene.canvas, image.x, image.y);
    UI_CHECK(corner.inside);
    UI_CHECK(near(corner.x, 0.0F));
    UI_CHECK(near(corner.y, 0.0F));

    // A point in the letterbox bar is outside the image, even though it is
    // inside the panel - clicking a bar must not pick anything.
    const editor::CanvasPoint bar =
        editor::to_canvas(image, h.scene.canvas, 500.0F, 10.0F);
    UI_CHECK(!bar.inside);
}

void clicking_a_clip_picks_the_node_that_made_it() {
    Harness h;
    const editor::ViewportFrame placed = h.frame(0.0F, 0.0F, false);
    const gui::Rect image = placed.image;
    const float mid_x = image.x + image.width * 0.5F;
    const float mid_y = image.y + image.height * 0.5F;

    const editor::ViewportFrame pressed = h.frame(mid_x, mid_y, true);
    UI_CHECK(pressed.pressed);
    UI_CHECK(pressed.picked.has_value());
    UI_CHECK(pressed.picked.value() == clip_node);
}

void clicking_bare_canvas_picks_nothing() {
    Harness h;
    const editor::ViewportFrame placed = h.frame(0.0F, 0.0F, false);
    const gui::Rect image = placed.image;

    // Just inside the image's left edge, well clear of the clip at the centre.
    const editor::ViewportFrame pressed =
        h.frame(image.x + 4.0F, image.y + image.height * 0.5F, true);
    UI_CHECK(pressed.pressed);
    UI_CHECK(!pressed.picked.has_value());
    // Nothing was picked, so nothing is being dragged either.
    UI_CHECK(!h.panel.dragging());
}

void the_front_most_clip_wins_an_overlap() {
    Harness h;
    // A second, nearer clip over the first.
    scene::MediaInstance nearer{};
    nearer.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.0F, 2.0F}),
        scene::scaling(scene::Vec3{2.0F, 2.0F, 1.0F}));
    nearer.source_node = other_node;
    h.scene.media.push_back(nearer);

    const editor::ViewportFrame placed = h.frame(0.0F, 0.0F, false);
    const editor::ViewportFrame pressed = h.frame(
        placed.image.x + placed.image.width * 0.5F,
        placed.image.y + placed.image.height * 0.5F,
        true);

    UI_CHECK(pressed.picked.has_value());
    UI_CHECK(pressed.picked.value() == other_node);
}

void a_drag_reports_a_world_translation_from_where_it_began() {
    Harness h;
    const editor::ViewportFrame placed = h.frame(0.0F, 0.0F, false);
    const float mid_x = placed.image.x + placed.image.width * 0.5F;
    const float mid_y = placed.image.y + placed.image.height * 0.5F;

    // Press on the clip: a press alone moves nothing.
    const editor::ViewportFrame pressed = h.frame(mid_x, mid_y, true);
    UI_CHECK(pressed.picked.has_value());
    UI_CHECK(!pressed.dragging);

    // Drag right: the clip moves along the camera's right axis, which for a
    // camera looking down -z with +y up is world +x, and nowhere else.
    const editor::ViewportFrame dragged = h.frame(mid_x + 100.0F, mid_y, true);
    UI_CHECK(dragged.dragging);
    UI_CHECK(dragged.translation.x > 0.0F);
    UI_CHECK(near(dragged.translation.y, 0.0F));
    UI_CHECK(near(dragged.translation.z, 0.0F));

    // Twice as far is twice the translation - the delta is measured from the
    // press, so it cannot drift with the frame rate.
    const editor::ViewportFrame further = h.frame(mid_x + 200.0F, mid_y, true);
    UI_CHECK(near(further.translation.x, dragged.translation.x * 2.0F, 0.001F));

    // Back to where it started: no net movement, however it got there.
    const editor::ViewportFrame back = h.frame(mid_x, mid_y, true);
    UI_CHECK(!back.dragging);

    const editor::ViewportFrame let_go = h.frame(mid_x, mid_y, false);
    UI_CHECK(let_go.released);
    UI_CHECK(!h.panel.dragging());
}

void a_downward_drag_moves_the_clip_down() {
    Harness h;
    const editor::ViewportFrame placed = h.frame(0.0F, 0.0F, false);
    const float mid_x = placed.image.x + placed.image.width * 0.5F;
    const float mid_y = placed.image.y + placed.image.height * 0.5F;

    static_cast<void>(h.frame(mid_x, mid_y, true));
    const editor::ViewportFrame dragged = h.frame(mid_x, mid_y + 100.0F, true);

    UI_CHECK(dragged.dragging);
    // Canvas y runs down, world y runs up.
    UI_CHECK(dragged.translation.y < 0.0F);
    UI_CHECK(near(dragged.translation.x, 0.0F));
}

void a_drag_keeps_its_grip_when_the_pointer_leaves_the_panel() {
    Harness h;
    const editor::ViewportFrame placed = h.frame(0.0F, 0.0F, false);
    const float mid_x = placed.image.x + placed.image.width * 0.5F;
    const float mid_y = placed.image.y + placed.image.height * 0.5F;

    static_cast<void>(h.frame(mid_x, mid_y, true));
    // Off the panel entirely, button still down: the drag continues, because a
    // manipulation the operator has not let go of is still theirs.
    const editor::ViewportFrame outside = h.frame(4000.0F, mid_y, true);
    UI_CHECK(h.panel.dragging());
    UI_CHECK(outside.dragging);
    UI_CHECK(outside.translation.x > 0.0F);
}

void a_press_outside_the_image_starts_nothing() {
    Harness h;
    static_cast<void>(h.frame(0.0F, 0.0F, false));

    // In the letterbox bar at the top of the panel.
    const editor::ViewportFrame pressed = h.frame(500.0F, 10.0F, true);
    UI_CHECK(!pressed.pressed);
    UI_CHECK(!pressed.picked.has_value());
    UI_CHECK(!h.panel.dragging());
}

void a_panel_with_no_room_reports_nothing() {
    Harness h;
    h.ctx.new_frame();
    const editor::ViewportFrame frame =
        h.panel.draw(h.ctx, gui::Rect{0.0F, 0.0F, 0.0F, 0.0F}, h.target, h.scene);

    UI_CHECK(near(frame.image.width, 0.0F));
    UI_CHECK(!frame.hovered);
    UI_CHECK(!frame.pressed);
}

void the_image_is_recorded_against_its_own_texture() {
    Harness h;
    static_cast<void>(h.frame(0.0F, 0.0F, false));

    // One batch, and it samples the panel's target rather than the atlas -
    // which is what makes a viewport a zero-copy texture in the widget tree.
    UI_CHECK(!h.list.batches().empty());
    UI_CHECK(h.list.batches().front().texture == &h.target);
    UI_CHECK(h.list.vertices().size() == 6U);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 11U> cases{{
    {"the render is letterboxed to the canvas aspect",
     &the_render_is_letterboxed_to_the_canvas_aspect},
    {"a pointer maps through the letterbox to canvas pixels",
     &a_pointer_maps_through_the_letterbox_to_canvas_pixels},
    {"clicking a clip picks the node that made it",
     &clicking_a_clip_picks_the_node_that_made_it},
    {"clicking bare canvas picks nothing", &clicking_bare_canvas_picks_nothing},
    {"the front most clip wins an overlap", &the_front_most_clip_wins_an_overlap},
    {"a drag reports a world translation from where it began",
     &a_drag_reports_a_world_translation_from_where_it_began},
    {"a downward drag moves the clip down", &a_downward_drag_moves_the_clip_down},
    {"a drag keeps its grip when the pointer leaves the panel",
     &a_drag_keeps_its_grip_when_the_pointer_leaves_the_panel},
    {"a press outside the image starts nothing",
     &a_press_outside_the_image_starts_nothing},
    {"a panel with no room reports nothing", &a_panel_with_no_room_reports_nothing},
    {"the image is recorded against its own texture",
     &the_image_is_recorded_against_its_own_texture},
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
