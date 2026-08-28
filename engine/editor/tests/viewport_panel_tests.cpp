#include <mediaserver/editor/viewport_panel.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

// The ported viewport's proof, with no GPU and no window.
//
// The panel draws an engine texture, but nothing here renders: the draw list
// only records which texture a quad samples, so an unbacked handle is enough to
// exercise every line of the interaction. What is under test is the part that
// would be wrong silently - where a click lands in canvas pixels once the image
// has been letterboxed, which clip that picks, and what world translation a
// drag across the panel resolves to.
namespace {

namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;
namespace doc = mediaserver::doc;
namespace output = mediaserver::output;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define UI_CHECK(...)                                                          \
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

    editor::ViewportFrame frame(float x, float y, bool down,
                                double time_seconds = 0.0) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        ctx.new_frame(time_seconds);
        return panel.draw(ctx, bounds, target, scene);
    }
};

const doc::NodeId surface_node{70U};

[[nodiscard]] output::ProjectionSurface make_surface(
    output::ProjectionSurfaceShape shape =
        output::ProjectionSurfaceShape::Planar) {
    output::ProjectionSurface surface{};
    surface.node = surface_node;
    surface.name = "Main LED Wall";
    surface.geometry.center = scene::Vec3{0.0F, 0.0F, 0.0F};
    surface.geometry.right = scene::Vec3{1.0F, 0.0F, 0.0F};
    surface.geometry.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    surface.geometry.width = 4.0F;
    surface.geometry.height = 2.0F;
    surface.geometry.raster = scene::Canvas{3840U, 1080U};
    surface.shape = shape;
    surface.horizontal_arc_turns = 0.20F;
    surface.mesh_columns = 18U;
    surface.mesh_scale = 2.5F;
    return surface;
}

struct SurfaceHarness final {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    rhi::Texture target;
    editor::ViewportPanel panel{gui::ui_id("surface-stage"),
                                editor::ViewportRole::Stage};
    scene::Scene scene{make_scene()};
    output::FeedMap feed_map;

    SurfaceHarness() {
        scene.media.clear();
        feed_map.surfaces.push_back(make_surface());
    }

    editor::ViewportFrame frame(float x, float y, bool down,
                                doc::NodeId selected = {}) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        ctx.new_frame();
        return panel.draw(ctx, Harness::bounds, target, scene, {}, {}, nullptr,
                          nullptr, 60.0F, &feed_map, selected);
    }

    [[nodiscard]] gui::UiPoint screen(scene::Vec3 world,
                                      const gui::Rect& image) const {
        const scene::CanvasProjection projected =
            scene::project_to_canvas(scene.camera, scene.canvas, world);
        return gui::UiPoint{
            image.x + projected.x * image.width /
                          static_cast<float>(scene.canvas.width),
            image.y + projected.y * image.height /
                          static_cast<float>(scene.canvas.height)};
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
    const editor::CanvasPoint centre =
        editor::to_canvas(image, h.scene.canvas, image.x + image.width * 0.5F,
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

void mapped_program_pixels_drive_selection_and_direct_manipulation() {
    Harness h;
    editor::ViewportProjection projection{};
    projection.canvas = h.scene.canvas;
    projection.forward = scene::Vec3{0.0F, 0.0F, -1.0F};
    projection.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    projection.project = [&h](scene::Vec3 point) {
        scene::CanvasProjection result =
            scene::project_to_canvas(h.scene.camera, h.scene.canvas, point);
        result.x += 400.0F;
        return result;
    };
    const auto draw = [&](float x, float y, bool down) {
        h.input.new_frame();
        h.input.set_mouse_position(x, y);
        h.input.set_mouse_button(gui::MouseButton::Left, down);
        h.ctx.new_frame();
        return h.panel.draw(h.ctx, Harness::bounds, h.target, h.scene, {}, {},
                            nullptr, nullptr, 60.0F, nullptr, {}, &projection);
    };

    const editor::ViewportFrame placed = draw(0.0F, 0.0F, false);
    const float mapped_x = placed.image.x +
                           (1'360.0F / 1'920.0F) * placed.image.width;
    const float mapped_y = placed.image.y + placed.image.height * 0.5F;
    const editor::ViewportFrame pressed = draw(mapped_x, mapped_y, true);
    UI_CHECK(pressed.picked == clip_node);

    const editor::ViewportFrame dragged =
        draw(mapped_x + 50.0F, mapped_y, true);
    UI_CHECK(dragged.dragging);
    UI_CHECK(dragged.translation.x > 0.0F);
    UI_CHECK(near(dragged.translation.y, 0.0F));
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
    nearer.world =
        scene::multiply(scene::translation(scene::Vec3{0.0F, 0.0F, 2.0F}),
                        scene::scaling(scene::Vec3{2.0F, 2.0F, 1.0F}));
    nearer.source_node = other_node;
    h.scene.media.push_back(nearer);

    const editor::ViewportFrame placed = h.frame(0.0F, 0.0F, false);
    const editor::ViewportFrame pressed =
        h.frame(placed.image.x + placed.image.width * 0.5F,
                placed.image.y + placed.image.height * 0.5F, true);

    UI_CHECK(pressed.picked.has_value());
    UI_CHECK(pressed.picked.value() == other_node);
}

void a_stationary_long_press_offers_every_overlapping_element() {
    Harness h;
    scene::MediaInstance nearer{};
    nearer.world =
        scene::multiply(scene::translation(scene::Vec3{0.0F, 0.0F, 2.0F}),
                        scene::scaling(scene::Vec3{2.0F, 2.0F, 1.0F}));
    nearer.source_node = other_node;
    h.scene.media.push_back(nearer);

    const editor::ViewportFrame placed = h.frame(0.0F, 0.0F, false);
    const float mid_x = placed.image.x + placed.image.width * 0.5F;
    const float mid_y = placed.image.y + placed.image.height * 0.5F;

    const editor::ViewportFrame pressed = h.frame(mid_x, mid_y, true, 1.0);
    UI_CHECK(pressed.picked == other_node);
    const editor::ViewportFrame held = h.frame(mid_x, mid_y, true, 1.5);
    UI_CHECK(held.deep_select_open);
    UI_CHECK(held.deep_candidate_count == 2U);
    UI_CHECK(!held.dragging);

    // Release the original viewport capture, then choose the second depth row.
    static_cast<void>(h.frame(mid_x, mid_y, false, 1.51));
    const float popup_x = mid_x + 28.0F;
    const float second_row_y = mid_y + 12.0F + 25.0F + 34.0F;
    static_cast<void>(h.frame(popup_x, second_row_y, true, 1.6));
    const editor::ViewportFrame chosen =
        h.frame(popup_x, second_row_y, false, 1.61);
    UI_CHECK(chosen.pressed);
    UI_CHECK(chosen.picked == clip_node);
    UI_CHECK(!chosen.deep_select_open);
}

void moving_during_a_long_press_keeps_the_normal_drag() {
    Harness h;
    scene::MediaInstance nearer{};
    nearer.world =
        scene::multiply(scene::translation(scene::Vec3{0.0F, 0.0F, 2.0F}),
                        scene::scaling(scene::Vec3{2.0F, 2.0F, 1.0F}));
    nearer.source_node = other_node;
    h.scene.media.push_back(nearer);

    const editor::ViewportFrame placed = h.frame(0.0F, 0.0F, false);
    const float mid_x = placed.image.x + placed.image.width * 0.5F;
    const float mid_y = placed.image.y + placed.image.height * 0.5F;
    static_cast<void>(h.frame(mid_x, mid_y, true, 2.0));
    const editor::ViewportFrame moved =
        h.frame(mid_x + 20.0F, mid_y, true, 2.5);
    UI_CHECK(moved.dragging);
    UI_CHECK(!moved.deep_select_open);
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
    const editor::ViewportFrame frame = h.panel.draw(
        h.ctx, gui::Rect{0.0F, 0.0F, 0.0F, 0.0F}, h.target, h.scene);

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

void clicking_a_procedural_object_selects_it() {
    Harness h;
    h.scene.media.clear();
    scene::MeshInstance object{};
    object.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    object.world = scene::scaling(scene::Vec3{2.0F, 2.0F, 2.0F});
    object.source = other_node;
    h.scene.instances.push_back(object);

    const editor::ViewportFrame placed = h.frame(0.0F, 0.0F, false);
    const editor::ViewportFrame pressed =
        h.frame(placed.image.x + placed.image.width * 0.5F,
                placed.image.y + placed.image.height * 0.5F, true);
    UI_CHECK(pressed.picked.has_value());
    UI_CHECK(pressed.picked.value() == other_node);
}

void a_selected_object_draws_a_cyan_outline_over_the_render() {
    Harness h;
    h.input.new_frame();
    h.ctx.new_frame();
    const std::array<doc::NodeId, 1U> selection{clip_node};
    static_cast<void>(h.panel.draw(h.ctx, Harness::bounds, h.target, h.scene,
                                   selection, clip_node));

    // The image itself is six vertices. The selected quad adds two passes over
    // four edges (dark separation plus cyan), so the outline must materially
    // extend the draw list and contain the bright cyan primary colour.
    UI_CHECK(h.list.vertices().size() > 6U);
    bool cyan = false;
    for (const gui::UiVertex& vertex : h.list.vertices()) {
        cyan =
            cyan || (vertex.g > 0.85F && vertex.b > 0.90F && vertex.r < 0.30F);
    }
    UI_CHECK(cyan);
}

class OffsetModelFixture final {
  public:
    OffsetModelFixture()
        : path_{std::filesystem::temp_directory_path() /
                "mediaserver_viewport_offset_bounds.obj"} {
        std::ofstream file{path_, std::ios::trunc};
        file << "o OffsetBounds\n"
                "v 1.0 -0.4 -0.2\n"
                "v 2.0 -0.4 -0.2\n"
                "v 2.0 0.4 -0.2\n"
                "v 1.0 0.4 -0.2\n"
                "f 1 2 3\n"
                "f 1 3 4\n";
        if (!file) {
            throw std::runtime_error{"could not write viewport model fixture"};
        }
    }

    ~OffsetModelFixture() {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove(path_, ignored));
    }

    [[nodiscard]] std::string source() const { return path_.string(); }

  private:
    std::filesystem::path path_;
};

void imported_model_bounds_drive_picking_and_the_selected_outline() {
    OffsetModelFixture fixture;
    Harness h;
    h.scene.media.clear();
    scene::ModelInstance model{};
    model.world = scene::identity();
    model.source = fixture.source();
    model.source_node = other_node;
    h.scene.models.push_back(model);

    const editor::ViewportFrame placed = h.frame(0.0F, 0.0F, false);
    const auto projected = scene::project_to_canvas(
        h.scene.camera, h.scene.canvas, scene::Vec3{1.5F, 0.0F, 0.0F});
    UI_CHECK(projected.visible);
    const float pointer_x =
        placed.image.x + projected.x * placed.image.width /
                             static_cast<float>(h.scene.canvas.width);
    const float pointer_y =
        placed.image.y + projected.y * placed.image.height /
                             static_cast<float>(h.scene.canvas.height);

    // The imported AABB is x=[1,2], entirely outside the legacy unit cube.
    const editor::ViewportFrame pressed = h.frame(pointer_x, pointer_y, true);
    UI_CHECK(pressed.picked.has_value());
    UI_CHECK(pressed.picked.value() == other_node);

    h.input.new_frame();
    h.input.set_mouse_position(pointer_x, pointer_y);
    h.input.set_mouse_button(gui::MouseButton::Left, false);
    h.ctx.new_frame();
    const std::array<doc::NodeId, 1U> selection{other_node};
    const editor::ViewportFrame outlined = h.panel.draw(
        h.ctx, Harness::bounds, h.target, h.scene, selection, other_node);

    float minimum_cyan_x = std::numeric_limits<float>::infinity();
    for (const gui::UiVertex& vertex : h.list.vertices()) {
        if (vertex.g > 0.85F && vertex.b > 0.90F && vertex.r < 0.30F) {
            minimum_cyan_x = std::min(minimum_cyan_x, vertex.x);
        }
    }
    UI_CHECK(std::isfinite(minimum_cyan_x));
    UI_CHECK(minimum_cyan_x > outlined.image.x + outlined.image.width * 0.5F);
}

void the_stage_x_gizmo_constrains_a_drag_to_world_x() {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    rhi::Texture target;
    editor::ViewportPanel panel{gui::ui_id("stage"),
                                editor::ViewportRole::Stage};
    scene::Scene scn = make_scene();
    const std::array<doc::NodeId, 1U> selection{clip_node};

    const auto draw = [&](float x, float y, bool down) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        ctx.new_frame();
        return panel.draw(ctx, Harness::bounds, target, scn, selection,
                          clip_node);
    };

    const editor::ViewportFrame placed = draw(0.0F, 0.0F, false);
    const float centre_x = placed.image.x + placed.image.width * 0.5F;
    const float centre_y = placed.image.y + placed.image.height * 0.5F;

    // The front camera projects +X horizontally right. Press midway along that
    // arrow, then continue right: the handle wins over the clip underneath.
    const editor::ViewportFrame pressed =
        draw(centre_x + 42.0F, centre_y, true);
    UI_CHECK(pressed.picked.has_value());
    UI_CHECK(pressed.picked.value() == clip_node);
    const editor::ViewportFrame dragged =
        draw(centre_x + 92.0F, centre_y, true);
    UI_CHECK(dragged.dragging);
    UI_CHECK(dragged.gizmo_axis == editor::GizmoAxis::X);
    UI_CHECK(dragged.translation.x > 0.0F);
    UI_CHECK(near(dragged.translation.y, 0.0F, 0.001F));
    UI_CHECK(near(dragged.translation.z, 0.0F, 0.001F));
}

void a_planar_surface_is_pickable_and_direct_drag_preserves_calibration() {
    SurfaceHarness h;
    h.feed_map.surfaces[0U].mesh_source = "preserve-this-source.obj";
    const editor::ViewportFrame placed = h.frame(0.0F, 0.0F, false);
    const auto corners = output::surface_corners(
        h.feed_map.surfaces[0U].geometry,
        scene::whole(h.feed_map.surfaces[0U].geometry.raster));
    UI_CHECK(corners.has_value());
    const scene::Vec3 top_middle =
        (corners.value().upper_left + corners.value().upper_right) * 0.5F;
    const gui::UiPoint pointer = h.screen(top_middle, placed.image);

    const editor::ViewportFrame pressed =
        h.frame(pointer.x, pointer.y, true);
    UI_CHECK(pressed.surface_picked == surface_node);
    UI_CHECK(!pressed.picked.has_value());

    const editor::ViewportFrame dragged =
        h.frame(pointer.x + 60.0F, pointer.y, true, surface_node);
    UI_CHECK(dragged.surface_dragging);
    UI_CHECK(dragged.surface_edit.has_value());
    const output::ProjectionSurface& edited = *dragged.surface_edit;
    UI_CHECK(edited.node == surface_node);
    UI_CHECK(edited.name == "Main LED Wall");
    UI_CHECK(edited.shape == output::ProjectionSurfaceShape::Planar);
    UI_CHECK(near(edited.horizontal_arc_turns, 0.20F));
    UI_CHECK(edited.mesh_columns == 18U);
    UI_CHECK(edited.mesh_source == "preserve-this-source.obj");
    UI_CHECK(near(edited.mesh_scale, 2.5F));
    UI_CHECK(edited.geometry.center.x > 0.0F);
    UI_CHECK(near(edited.geometry.center.y, 0.0F));
    UI_CHECK((edited.geometry.raster == scene::Canvas{3840U, 1080U}));
}

void a_locked_surface_is_visible_but_not_pickable_or_manipulable() {
    SurfaceHarness h;
    h.feed_map.surfaces[0U].locked = true;
    const editor::ViewportFrame placed =
        h.frame(0.0F, 0.0F, false, surface_node);
    const auto corners = output::surface_corners(
        h.feed_map.surfaces[0U].geometry,
        scene::whole(h.feed_map.surfaces[0U].geometry.raster));
    UI_CHECK(corners.has_value());
    const scene::Vec3 top_middle =
        (corners.value().upper_left + corners.value().upper_right) * 0.5F;
    const gui::UiPoint pointer = h.screen(top_middle, placed.image);

    const editor::ViewportFrame pressed =
        h.frame(pointer.x, pointer.y, true, surface_node);
    UI_CHECK(!pressed.surface_picked.has_value());
    UI_CHECK(!pressed.surface_dragging);
    UI_CHECK(!pressed.surface_edit.has_value());
}

void the_exact_horizontal_arc_wire_is_pickable() {
    SurfaceHarness h;
    h.feed_map.surfaces[0U] =
        make_surface(output::ProjectionSurfaceShape::HorizontalArc);
    const editor::ViewportFrame placed = h.frame(0.0F, 0.0F, false);
    const auto mesh = output::horizontal_arc_mesh(
        h.feed_map.surfaces[0U].geometry,
        h.feed_map.surfaces[0U].horizontal_arc_turns,
        h.feed_map.surfaces[0U].mesh_columns);
    UI_CHECK(mesh.has_value());
    UI_CHECK(mesh.value().vertices.size() >= 8U);
    const scene::Vec3 edge_middle =
        (mesh.value().vertices[4U].position +
         mesh.value().vertices[6U].position) *
        0.5F;
    const gui::UiPoint pointer = h.screen(edge_middle, placed.image);
    const editor::ViewportFrame pressed =
        h.frame(pointer.x, pointer.y, true);
    UI_CHECK(pressed.surface_picked == surface_node);
}

void the_surface_yaw_ring_rotates_about_stage_up_and_preserves_the_surface() {
    SurfaceHarness h;
    const editor::ViewportFrame placed =
        h.frame(0.0F, 0.0F, false, surface_node);
    const gui::UiPoint centre =
        h.screen(h.feed_map.surfaces[0U].geometry.center, placed.image);
    constexpr float ring_diagonal = 34.648F;
    const float ring_x = centre.x - ring_diagonal;
    const float ring_y = centre.y + ring_diagonal;

    const editor::ViewportFrame pressed =
        h.frame(ring_x, ring_y, true, surface_node);
    UI_CHECK(pressed.surface_picked == surface_node);
    const editor::ViewportFrame dragged =
        h.frame(ring_x + 36.0F, ring_y, true, surface_node);
    UI_CHECK(dragged.surface_dragging);
    UI_CHECK(dragged.gizmo_axis == editor::GizmoAxis::Yaw);
    UI_CHECK(dragged.surface_edit.has_value());
    const output::ProjectionSurface& edited = *dragged.surface_edit;
    UI_CHECK(near(edited.geometry.center.x, 0.0F));
    UI_CHECK(near(edited.geometry.center.y, 0.0F));
    UI_CHECK(near(edited.geometry.center.z, 0.0F));
    UI_CHECK(edited.geometry.right.x < 1.0F);
    UI_CHECK(edited.geometry.right.z < 0.0F);
    UI_CHECK(near(edited.geometry.up.x, 0.0F));
    UI_CHECK(near(edited.geometry.up.y, 1.0F));
    UI_CHECK(near(edited.geometry.up.z, 0.0F));
    UI_CHECK(edited.name == "Main LED Wall");
    UI_CHECK((edited.geometry.raster == scene::Canvas{3840U, 1080U}));
}

void an_unavailable_imported_mesh_draws_honest_calibration_bounds() {
    SurfaceHarness h;
    h.feed_map.surfaces[0U] =
        make_surface(output::ProjectionSurfaceShape::ImportedMesh);
    h.feed_map.surfaces[0U].mesh_source =
        "this-projection-mesh-does-not-exist.obj";
    static_cast<void>(h.frame(0.0F, 0.0F, false, surface_node));

    bool amber = false;
    for (const gui::UiVertex& vertex : h.list.vertices()) {
        amber = amber || (vertex.r > 0.95F && vertex.g > 0.55F &&
                          vertex.g < 0.70F && vertex.b < 0.25F);
    }
    UI_CHECK(amber);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 23U> cases{{
    {"the render is letterboxed to the canvas aspect",
     &the_render_is_letterboxed_to_the_canvas_aspect},
    {"a pointer maps through the letterbox to canvas pixels",
     &a_pointer_maps_through_the_letterbox_to_canvas_pixels},
    {"clicking a clip picks the node that made it",
     &clicking_a_clip_picks_the_node_that_made_it},
    {"mapped Program pixels drive selection and direct manipulation",
     &mapped_program_pixels_drive_selection_and_direct_manipulation},
    {"clicking bare canvas picks nothing", &clicking_bare_canvas_picks_nothing},
    {"the front most clip wins an overlap",
     &the_front_most_clip_wins_an_overlap},
    {"a stationary long press offers every overlapping element",
     &a_stationary_long_press_offers_every_overlapping_element},
    {"moving during a long press keeps the normal drag",
     &moving_during_a_long_press_keeps_the_normal_drag},
    {"a drag reports a world translation from where it began",
     &a_drag_reports_a_world_translation_from_where_it_began},
    {"a downward drag moves the clip down",
     &a_downward_drag_moves_the_clip_down},
    {"a drag keeps its grip when the pointer leaves the panel",
     &a_drag_keeps_its_grip_when_the_pointer_leaves_the_panel},
    {"a press outside the image starts nothing",
     &a_press_outside_the_image_starts_nothing},
    {"a panel with no room reports nothing",
     &a_panel_with_no_room_reports_nothing},
    {"the image is recorded against its own texture",
     &the_image_is_recorded_against_its_own_texture},
    {"clicking a procedural object selects it",
     &clicking_a_procedural_object_selects_it},
    {"a selected object draws a cyan outline over the render",
     &a_selected_object_draws_a_cyan_outline_over_the_render},
    {"imported model bounds drive picking and the selected outline",
     &imported_model_bounds_drive_picking_and_the_selected_outline},
    {"the stage x gizmo constrains a drag to world x",
     &the_stage_x_gizmo_constrains_a_drag_to_world_x},
    {"a planar surface is pickable and direct drag preserves calibration",
     &a_planar_surface_is_pickable_and_direct_drag_preserves_calibration},
    {"a locked surface is visible but not pickable or manipulable",
     &a_locked_surface_is_visible_but_not_pickable_or_manipulable},
    {"the exact horizontal arc wire is pickable",
     &the_exact_horizontal_arc_wire_is_pickable},
    {"the surface yaw ring rotates about stage up and preserves the surface",
     &the_surface_yaw_ring_rotates_about_stage_up_and_preserves_the_surface},
    {"an unavailable imported mesh draws honest calibration bounds",
     &an_unavailable_imported_mesh_draws_honest_calibration_bounds},
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
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
