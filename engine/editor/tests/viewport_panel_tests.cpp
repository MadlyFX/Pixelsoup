#include <mediaserver/editor/viewport_panel.hpp>
#include <mediaserver/editor/stage_view.hpp>
#include <mediaserver/editor/light_guides.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/render/model_cache.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <span>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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
        editor::ViewportFrame out = panel.draw(ctx, bounds, target, scene);
        // Menus (the depth chooser among them) draw last, as in the shell.
        ctx.draw_overlays(gui::Rect{0.0F, 0.0F, 2'000.0F, 2'000.0F});
        return out;
    }
};

const scene::SceneId mask_scene{1U};
const doc::NodeId rectangle_mask_node{20U};
const doc::NodeId ellipse_mask_node{21U};
const doc::NodeId bezier_mask_node{22U};
const doc::NodeId first_mask_point{100U};

[[nodiscard]] scene::MaskPoint make_mask_point(
    doc::NodeId source, mediaserver::core::i64 order, float x, float y,
    float in_x, float in_y, float out_x, float out_y) {
    scene::MaskPoint point{};
    point.source = source;
    point.scene = mask_scene;
    point.mask = bezier_mask_node;
    point.order = order;
    point.x = x;
    point.y = y;
    point.in_x = in_x;
    point.in_y = in_y;
    point.out_x = out_x;
    point.out_y = out_y;
    return point;
}

[[nodiscard]] std::vector<scene::MaskInstance> make_masks() {
    scene::MaskInstance rectangle{};
    rectangle.source = rectangle_mask_node;
    rectangle.scene = mask_scene;
    rectangle.name = "Frame";
    rectangle.target = clip_node;
    rectangle.kind = scene::MaskKind::Rectangle;
    rectangle.order = 0;
    rectangle.feather_canvas_px = 12.0F;
    rectangle.expansion_canvas_px = 4.0F;
    rectangle.bounds = scene::MaskBounds{0.10F, 0.15F, 0.90F, 0.85F};

    scene::MaskInstance ellipse{};
    ellipse.source = ellipse_mask_node;
    ellipse.scene = mask_scene;
    ellipse.name = "Face";
    ellipse.target = clip_node;
    ellipse.kind = scene::MaskKind::Ellipse;
    ellipse.order = 1;
    ellipse.bounds = scene::MaskBounds{0.25F, 0.20F, 0.75F, 0.80F};

    scene::MaskInstance bezier{};
    bezier.source = bezier_mask_node;
    bezier.scene = mask_scene;
    bezier.name = "Roto";
    bezier.target = clip_node;
    bezier.kind = scene::MaskKind::Bezier;
    bezier.order = 2;
    bezier.feather_canvas_px = 8.0F;
    bezier.expansion_canvas_px = -2.0F;
    bezier.points = {
        make_mask_point(first_mask_point, 0, 0.20F, 0.20F, 0.10F, 0.20F,
                        0.40F, 0.20F),
        make_mask_point(doc::NodeId{101U}, 1, 0.80F, 0.20F, 0.60F, 0.20F,
                        0.90F, 0.20F),
        make_mask_point(doc::NodeId{102U}, 2, 0.80F, 0.80F, 0.80F, 0.60F,
                        0.80F, 0.90F),
        make_mask_point(doc::NodeId{103U}, 3, 0.20F, 0.80F, 0.20F, 0.90F,
                        0.20F, 0.60F),
    };
    return {std::move(rectangle), std::move(ellipse), std::move(bezier)};
}

struct MaskHarness final {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    rhi::Texture target;
    editor::ViewportPanel panel{gui::ui_id("mask-stage"),
                                editor::ViewportRole::Stage};
    scene::Scene scene{make_scene()};
    std::vector<scene::MaskInstance> masks{make_masks()};
    editor::MasksPanelTarget mask_target{
        mask_scene, clip_node, scene::MaskCoordinateSpace::SourceLayer};
    editor::MasksPanelSelection selection{
        mask_scene, clip_node, bezier_mask_node, first_mask_point};
    std::optional<editor::MaskViewportSourceMapping> source_mapping{
        editor::MaskViewportSourceMapping{{
            editor::MaskViewportCanvasPoint{480.0F, 270.0F},
            editor::MaskViewportCanvasPoint{1440.0F, 270.0F},
            editor::MaskViewportCanvasPoint{1440.0F, 810.0F},
            editor::MaskViewportCanvasPoint{480.0F, 810.0F},
        }, scene::MaskCoordinateExtent{960.0F, 540.0F}}};
    bool pen_active{};
    bool coverage_visible{};
    bool authoring_enabled{true};

    editor::MaskViewportFrame frame(
        float x, float y, bool down,
        gui::KeyMods mods = gui::KeyMods::None,
        std::optional<gui::Key> key = std::nullopt) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        input.set_mods(mods);
        if (key.has_value()) {
            input.add_key(*key);
        }
        ctx.new_frame();
        editor::MaskViewportInput viewport_input{};
        viewport_input.target = mask_target;
        viewport_input.masks = std::span<const scene::MaskInstance>{masks};
        viewport_input.selection = selection;
        viewport_input.source_layer_mapping = source_mapping;
        viewport_input.pen_active = pen_active;
        viewport_input.coverage_visible = coverage_visible;
        viewport_input.authoring_enabled = authoring_enabled;
        editor::MaskViewportFrame result = panel.draw_masks(
            ctx, Harness::bounds, target, scene, viewport_input);
        if (result.selection_changed) {
            selection = result.selection;
        }
        return result;
    }

    [[nodiscard]] gui::UiPoint screen(float x, float y,
                                      const gui::Rect& image) const {
        const auto mapped = editor::mask_normalized_to_viewport(
            image, scene.canvas, mask_target.coordinate_space,
            source_mapping.has_value() ? &*source_mapping : nullptr, x, y);
        if (!mapped.has_value()) {
            throw std::runtime_error{"mask mapping unexpectedly unavailable"};
        }
        return *mapped;
    }
};

// Program Output is not the authored camera canvas: it is whatever raster the
// output map produces. This harness publishes the middle 960 columns of a
// 1920x1080 canvas as a 960x1080 output - the smallest projection that would
// silently mis-place every mask if the overlay resolved against scene.canvas.
struct ProgramMaskHarness final {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    rhi::Texture target;
    editor::ViewportPanel panel{gui::ui_id("mask-program"),
                                editor::ViewportRole::Program};
    scene::Scene scene{make_scene()};
    std::vector<scene::MaskInstance> masks{make_masks()};
    editor::MasksPanelTarget mask_target{
        mask_scene, clip_node, scene::MaskCoordinateSpace::SourceLayer};
    editor::MasksPanelSelection selection{
        mask_scene, clip_node, bezier_mask_node, first_mask_point};
    static constexpr scene::Canvas output_canvas{960U, 1080U};
    static constexpr scene::CanvasRect crop{480U, 0U, 960U, 1080U};
    // The clip quad's corners, projected through the crop rather than through
    // the camera - the same values the shell derives from the live projector.
    std::optional<editor::MaskViewportSourceMapping> source_mapping{
        editor::MaskViewportSourceMapping{{
            editor::MaskViewportCanvasPoint{0.0F, 270.0F},
            editor::MaskViewportCanvasPoint{960.0F, 270.0F},
            editor::MaskViewportCanvasPoint{960.0F, 810.0F},
            editor::MaskViewportCanvasPoint{0.0F, 810.0F},
        }, scene::MaskCoordinateExtent{960.0F, 540.0F}}};
    bool authoring_enabled{true};

    [[nodiscard]] editor::ViewportProjection projection() const {
        editor::ViewportProjection value{};
        value.canvas = output_canvas;
        value.forward = scene::normalize(scene.camera.target - scene.camera.eye);
        value.up = scene.camera.up;
        const scene::Scene& view = scene;
        value.project = [&view](scene::Vec3 point) {
            scene::CanvasProjection mapped =
                scene::project_to_canvas(view.camera, view.canvas, point);
            mapped.x -= static_cast<float>(crop.x);
            return mapped;
        };
        return value;
    }

    editor::MaskViewportFrame frame(float x, float y, bool down) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        ctx.new_frame();
        editor::MaskViewportInput viewport_input{};
        viewport_input.target = mask_target;
        viewport_input.masks = std::span<const scene::MaskInstance>{masks};
        viewport_input.selection = selection;
        viewport_input.source_layer_mapping = source_mapping;
        viewport_input.authoring_enabled = authoring_enabled;
        const editor::ViewportProjection output = projection();
        editor::MaskViewportFrame result =
            panel.draw_masks(ctx, Harness::bounds, target, scene,
                             viewport_input, nullptr, 60.0F, &output);
        if (result.selection_changed) {
            selection = result.selection;
        }
        return result;
    }

    [[nodiscard]] gui::UiPoint screen(float x, float y,
                                      const gui::Rect& image) const {
        const auto mapped = editor::mask_normalized_to_viewport(
            image, output_canvas, mask_target.coordinate_space,
            source_mapping.has_value() ? &*source_mapping : nullptr, x, y);
        if (!mapped.has_value()) {
            throw std::runtime_error{"program mask mapping unavailable"};
        }
        return *mapped;
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
    UI_CHECK(pressed.empty_canvas ==
             editor::EmptyCanvasInteraction::ClearSelection);
    // Nothing was picked, so nothing is being dragged either.
    UI_CHECK(!h.panel.dragging());
}

void stage_bare_canvas_explicitly_reserves_the_press_for_navigation() {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    rhi::Texture target;
    editor::ViewportPanel panel{gui::ui_id("stage.empty"),
                                editor::ViewportRole::Stage};
    scene::Scene empty = make_scene();
    empty.media.clear();

    const auto draw = [&](float x, float y, bool down) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        ctx.new_frame();
        return panel.draw(ctx, Harness::bounds, target, empty);
    };
    const editor::ViewportFrame placed = draw(0.0F, 0.0F, false);
    const editor::ViewportFrame pressed =
        draw(placed.image.x + 4.0F,
             placed.image.y + placed.image.height * 0.5F, true);

    UI_CHECK(pressed.pressed);
    UI_CHECK(!pressed.picked.has_value());
    UI_CHECK(pressed.empty_canvas == editor::EmptyCanvasInteraction::Navigate);
}

// Alt+left orbits in Unreal even with the pointer over geometry, and adding the
// left button to a right or middle drag pans. Neither may pick or drag the
// clip underneath; a plain press on the same pixel still does.
void stage_camera_chords_never_pick_what_is_underneath() {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    rhi::Texture target;
    editor::ViewportPanel panel{gui::ui_id("stage.chords"),
                                editor::ViewportRole::Stage};
    const scene::Scene clip_scene = make_scene();

    const auto draw = [&](float x, float y, bool left, gui::KeyMods mods,
                          gui::MouseButton held, bool hold) {
        input.new_frame();
        input.set_mods(mods);
        input.set_mouse_position(x, y);
        input.set_mouse_button(held, hold);
        input.set_mouse_button(gui::MouseButton::Left, left);
        ctx.new_frame();
        return panel.draw(ctx, Harness::bounds, target, clip_scene);
    };
    const editor::ViewportFrame placed = draw(
        0.0F, 0.0F, false, gui::KeyMods::None, gui::MouseButton::Right, false);
    const float x = placed.image.x + placed.image.width * 0.5F;
    const float y = placed.image.y + placed.image.height * 0.5F;

    const editor::ViewportFrame alt = draw(
        x, y, true, gui::KeyMods::Alt, gui::MouseButton::Right, false);
    UI_CHECK(alt.pressed);
    UI_CHECK(!alt.picked.has_value() && !alt.surface_picked.has_value());
    UI_CHECK(alt.empty_canvas == editor::EmptyCanvasInteraction::Navigate);
    UI_CHECK(!panel.dragging());
    static_cast<void>(draw(x, y, false, gui::KeyMods::None,
                           gui::MouseButton::Right, false));

    for (const gui::MouseButton held :
         {gui::MouseButton::Right, gui::MouseButton::Middle}) {
        static_cast<void>(draw(x, y, false, gui::KeyMods::None, held, true));
        const editor::ViewportFrame chord =
            draw(x, y, true, gui::KeyMods::None, held, true);
        UI_CHECK(chord.pressed && !chord.picked.has_value());
        UI_CHECK(chord.empty_canvas ==
                 editor::EmptyCanvasInteraction::Navigate);
        static_cast<void>(draw(x, y, false, gui::KeyMods::None, held, false));
    }

    const editor::ViewportFrame plain = draw(
        x, y, true, gui::KeyMods::None, gui::MouseButton::Right, false);
    UI_CHECK(plain.picked == clip_node);
}

// The world box F frames: a clip's unit quad through its world transform, and
// a node with only a position as that point.
void world_bounds_cover_the_selection_as_picked() {
    Harness h;
    scene::Scene framed = make_scene();
    framed.media.front().world =
        scene::multiply(scene::translation(scene::Vec3{3.0F, -1.0F, 2.0F}),
                        scene::scaling(scene::Vec3{4.0F, 2.0F, 1.0F}));
    scene::PointLight lamp{};
    lamp.source = other_node;
    lamp.position = scene::Vec3{-6.0F, 5.0F, 0.5F};
    framed.point_lights.push_back(lamp);

    const std::array<doc::NodeId, 1U> clip_only{clip_node};
    const auto clip_box = h.panel.world_bounds(framed, clip_only);
    UI_CHECK(clip_box.has_value());
    UI_CHECK(near(clip_box->minimum.x, 1.0F) && near(clip_box->maximum.x, 5.0F));
    UI_CHECK(near(clip_box->minimum.y, -2.0F) && near(clip_box->maximum.y, 0.0F));
    UI_CHECK(near(clip_box->minimum.z, 1.5F) && near(clip_box->maximum.z, 2.5F));

    const std::array<doc::NodeId, 2U> both{clip_node, other_node};
    const auto both_box = h.panel.world_bounds(framed, both);
    UI_CHECK(both_box.has_value());
    UI_CHECK(near(both_box->minimum.x, -6.0F) && near(both_box->maximum.y, 5.0F));
    UI_CHECK(near(both_box->minimum.z, 0.5F) && near(both_box->maximum.z, 2.5F));

    const std::array<doc::NodeId, 1U> missing{doc::NodeId{404U}};
    UI_CHECK(!h.panel.world_bounds(framed, missing).has_value());
}

void stage_navigation_follows_unreal_chords() {
    using editor::StageNavigation;
    const auto pick = [](bool left, bool middle, bool right, bool alt,
                         bool shift) {
        return editor::stage_navigation_for(
            editor::StageNavigationButtons{left, middle, right, alt, shift});
    };
    UI_CHECK(pick(false, false, false, false, false) == StageNavigation::None);
    UI_CHECK(pick(true, false, false, false, false) == StageNavigation::Walk);
    UI_CHECK(pick(false, false, true, false, false) == StageNavigation::Look);
    UI_CHECK(pick(false, true, false, false, false) == StageNavigation::Orbit);
    UI_CHECK(pick(true, false, true, false, false) == StageNavigation::Pan);
    UI_CHECK(pick(false, true, false, false, true) == StageNavigation::Track);
    UI_CHECK(pick(true, false, false, true, false) == StageNavigation::Orbit);
    UI_CHECK(pick(false, true, false, true, false) == StageNavigation::Track);
    UI_CHECK(pick(false, false, true, true, false) == StageNavigation::Dolly);
    UI_CHECK(pick(true, false, true, true, false) == StageNavigation::Dolly);
    // Alt alone, with nothing pressed on the Stage, moves nothing.
    UI_CHECK(pick(false, false, false, true, true) == StageNavigation::None);
}

[[nodiscard]] bool near_vec(scene::Vec3 a, scene::Vec3 b,
                            float tolerance = 0.001F) noexcept {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) &&
           near(a.z, b.z, tolerance);
}

void a_right_drag_looks_in_place_and_flies_with_wasd() {
    const editor::StageOrbit start{{2.0F, 1.0F, -3.0F}, 0.0F, 0.0F, 6.0F};
    const scene::Vec3 eye = editor::stage_eye(start);
    UI_CHECK(near_vec(eye, scene::Vec3{2.0F, 1.0F, 3.0F}));

    // Dragging right turns right: the eye stays and the view swings to +x.
    editor::StageNavigationStep look{};
    look.gesture = editor::StageNavigation::Look;
    look.dx = 50.0F;
    look.dy = -40.0F;
    const editor::StageOrbit turned = editor::navigate_stage(start, look);
    UI_CHECK(near_vec(editor::stage_eye(turned), eye));
    UI_CHECK(turned.target.x > eye.x);
    // Dragging up looks up.
    UI_CHECK(turned.target.y > eye.y);
    UI_CHECK(near(turned.distance, start.distance));

    // W flies along the view at the fly speed; D strafes right; Q sinks.
    editor::StageNavigationStep fly{};
    fly.gesture = editor::StageNavigation::Look;
    fly.seconds = 0.5F;
    fly.fly_speed = 3.0F;
    fly.move = scene::Vec3{0.0F, 0.0F, 1.0F};
    UI_CHECK(near_vec(editor::stage_eye(editor::navigate_stage(start, fly)),
                      scene::Vec3{2.0F, 1.0F, 1.5F}));
    fly.move = scene::Vec3{1.0F, 0.0F, 0.0F};
    UI_CHECK(near_vec(editor::stage_eye(editor::navigate_stage(start, fly)),
                      scene::Vec3{3.5F, 1.0F, 3.0F}));
    fly.move = scene::Vec3{0.0F, -1.0F, 0.0F};
    const editor::StageOrbit sunk = editor::navigate_stage(start, fly);
    UI_CHECK(near_vec(editor::stage_eye(sunk), scene::Vec3{2.0F, -0.5F, 3.0F}));
    UI_CHECK(near(sunk.yaw, start.yaw) && near(sunk.pitch, start.pitch));

    // Each wheel notch is a quarter faster or slower, within bounds.
    UI_CHECK(near(editor::stage_fly_speed_after(4.0F, 1.0F), 5.0F));
    UI_CHECK(near(editor::stage_fly_speed_after(4.0F, -2.0F), 2.56F));
    UI_CHECK(editor::stage_fly_speed_after(4.0F, 100.0F) ==
             editor::stage_maximum_fly_speed);
}

void orbit_pan_walk_and_dolly_move_the_view_as_unreal_does() {
    const editor::StageOrbit start{{1.0F, 0.5F, -2.0F}, 0.0F, 0.0F, 8.0F};
    editor::StageNavigationStep step{};

    // Orbit turns about the pivot at the same distance, and never flips over.
    step.gesture = editor::StageNavigation::Orbit;
    step.dx = 100.0F;
    step.dy = 10'000.0F;
    const editor::StageOrbit orbited = editor::navigate_stage(start, step);
    UI_CHECK(orbited.target == start.target);
    UI_CHECK(near(orbited.distance, start.distance));
    UI_CHECK(near(orbited.yaw, -0.15F));
    UI_CHECK(orbited.pitch == editor::stage_pitch_limit);

    // Pan keeps the view's direction and moves the camera with the mouse:
    // right and up. Track moves it the other way, so the scene follows.
    step = {};
    step.gesture = editor::StageNavigation::Pan;
    step.dx = 20.0F;
    step.dy = -10.0F;
    step.units_per_pixel = 0.05F;
    const editor::StageOrbit panned = editor::navigate_stage(start, step);
    UI_CHECK(near_vec(panned.target, scene::Vec3{2.0F, 1.0F, -2.0F}));
    UI_CHECK(panned.yaw == start.yaw && panned.distance == start.distance);
    step.gesture = editor::StageNavigation::Track;
    UI_CHECK(near_vec(editor::navigate_stage(start, step).target,
                      scene::Vec3{0.0F, 0.0F, -2.0F}));

    // A left drag walks along the ground, however far the view is pitched.
    editor::StageOrbit pitched = start;
    pitched.pitch = 0.1F;
    step = {};
    step.gesture = editor::StageNavigation::Walk;
    step.dy = -80.0F;
    step.fly_speed = 2.0F;
    const editor::StageOrbit walked = editor::navigate_stage(pitched, step);
    UI_CHECK(near_vec(walked.target, scene::Vec3{1.0F, 0.5F, -4.0F}));
    UI_CHECK(walked.pitch == pitched.pitch);

    // The wheel closes on the pivot, then carries it ahead rather than stop.
    const editor::StageOrbit closer = editor::stage_wheel_dolly(start, 2.0F);
    UI_CHECK(near(closer.distance, 6.4F) && closer.target == start.target);
    editor::StageOrbit near_pivot = start;
    near_pivot.distance = 0.6F;
    const editor::StageOrbit pushed =
        editor::stage_wheel_dolly(near_pivot, 3.0F);
    UI_CHECK(pushed.distance == editor::stage_minimum_distance);
    UI_CHECK(near(pushed.target.z, -2.2F));
    UI_CHECK(near_vec(editor::stage_eye(pushed),
                      editor::stage_eye(near_pivot) +
                          scene::Vec3{0.0F, 0.0F, -0.3F}));
    UI_CHECK(near(editor::stage_wheel_dolly(start, -1.0F).distance, 8.8F));

    // Alt+right: dragging right or up closes in, proportionally.
    step = {};
    step.gesture = editor::StageNavigation::Dolly;
    step.dx = 10.0F;
    step.dy = -15.0F;
    UI_CHECK(near(editor::navigate_stage(start, step).distance, 7.0F));
}

void f_frames_the_whole_box_from_the_current_direction() {
    const editor::StageOrbit start{{9.0F, 9.0F, 9.0F}, 0.2F, -0.05F, 30.0F};
    const editor::WorldBounds box{{-1.0F, 0.0F, 2.0F}, {3.0F, 4.0F, 6.0F}};
    const editor::StageOrbit framed = editor::stage_focus(start, box);
    UI_CHECK(near_vec(framed.target, scene::Vec3{1.0F, 2.0F, 4.0F}));
    UI_CHECK(framed.yaw == start.yaw && framed.pitch == start.pitch);
    // The bounding sphere fits inside the vertical field of view, without
    // standing off more than twice as far as it needs to.
    const float radius = std::sqrt(12.0F);
    const float half_view = editor::stage_field_of_view * 3.14159265F;
    UI_CHECK(std::asin(radius / framed.distance) < half_view);
    UI_CHECK(framed.distance < 2.0F * radius / std::sin(half_view));
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
    // The chooser is a context menu anchored 12 units below-right of the
    // press: it opens 2 units under the anchor with 24-unit rows.
    static_cast<void>(h.frame(mid_x, mid_y, false, 1.51));
    const float popup_x = mid_x + 28.0F;
    const float second_row_y = mid_y + 12.0F + 2.0F + 24.0F + 12.0F;
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

void a_long_press_over_the_selected_gizmo_still_offers_depths() {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    rhi::Texture target;
    editor::ViewportPanel panel{gui::ui_id("stage.deep-select"),
                                editor::ViewportRole::Stage};
    scene::Scene scn = make_scene();
    scene::MediaInstance nearer{};
    nearer.world =
        scene::multiply(scene::translation(scene::Vec3{0.0F, 0.0F, 2.0F}),
                        scene::scaling(scene::Vec3{2.0F, 2.0F, 1.0F}));
    nearer.source_node = other_node;
    scn.media.push_back(nearer);
    const std::array<doc::NodeId, 1U> selection{other_node};

    const auto draw = [&](float x, float y, bool down, double time) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        ctx.new_frame(time);
        return panel.draw(ctx, Harness::bounds, target, scn, selection,
                          other_node);
    };

    const editor::ViewportFrame placed = draw(0.0F, 0.0F, false, 0.0);
    const float centre_x = placed.image.x + placed.image.width * 0.5F;
    const float centre_y = placed.image.y + placed.image.height * 0.5F;

    // This point is both inside the two clips and on the selected item's +X
    // gizmo. Holding it must retain access to the depth chooser; moving it
    // remains the ordinary constrained gizmo drag.
    const editor::ViewportFrame pressed =
        draw(centre_x + 42.0F, centre_y, true, 1.0);
    UI_CHECK(pressed.picked == other_node);
    const editor::ViewportFrame held =
        draw(centre_x + 42.0F, centre_y, true, 1.5);
    UI_CHECK(held.deep_select_open);
    UI_CHECK(held.deep_candidate_count == 2U);
    UI_CHECK(!held.dragging);
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

void a_full_scatter_group_has_one_bounded_selection_outline() {
    Harness h;
    h.scene.media.clear();
    scene::ModelInstance model{};
    model.world=scene::identity(); model.normal=scene::identity();
    model.source_node=other_node; model.motion_source=doc::NodeId{100};
    model.batch_world=scene::identity();
    h.scene.models.assign(4096,model);
    const std::array<doc::NodeId,1> selection{other_node};
    h.input.new_frame(); h.ctx.new_frame();
    static_cast<void>(h.panel.draw(h.ctx,Harness::bounds,h.target,h.scene,selection,other_node));
    UI_CHECK(h.list.vertices().size()>24);
    UI_CHECK(h.list.vertices().size()<500);
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

    // A selected-outline pass admits the source but does not read it. Asset
    // bytes are decoded on the shared worker; this test-only seam makes
    // publication deterministic.
    h.input.new_frame();
    h.ctx.new_frame();
    const std::array<doc::NodeId, 1U> warm_selection{other_node};
    static_cast<void>(h.panel.draw(h.ctx, Harness::bounds, h.target, h.scene,
                                   warm_selection, other_node));
    UI_CHECK(mediaserver::render::detail::
                 wait_for_shared_model_cache_for_test());
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

void a_rotated_stage_gizmo_moves_along_the_displayed_local_axis() {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    rhi::Texture target;
    editor::ViewportPanel panel{gui::ui_id("rotated-stage"),
                                editor::ViewportRole::Stage};
    // Local orientation: the gizmo follows the element, not the world.
    panel.set_orientation(editor::TransformOrientation::Local);
    scene::Scene scn = make_scene();
    scn.media.front().world = scene::multiply(
        scene::rotation(0.0F, 0.0F, 0.125F),
        scene::scaling(scene::Vec3{2.0F, 2.0F, 1.0F}));
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
    const scene::Vec3 origin = scene::transform_point(
        scn.media.front().world, scene::Vec3{});
    const scene::Vec3 local_x = scene::normalize(
        scene::transform_point(scn.media.front().world,
                               scene::Vec3{1.0F, 0.0F, 0.0F}) -
        origin);
    const scene::CanvasProjection projected_origin =
        scene::project_to_canvas(scn.camera, scn.canvas, origin);
    const scene::CanvasProjection projected_x =
        scene::project_to_canvas(scn.camera, scn.canvas, origin + local_x);
    const float ui_dx =
        (projected_x.x - projected_origin.x) * placed.image.width /
        static_cast<float>(scn.canvas.width);
    const float ui_dy =
        (projected_x.y - projected_origin.y) * placed.image.height /
        static_cast<float>(scn.canvas.height);
    const float ui_length = std::sqrt(ui_dx * ui_dx + ui_dy * ui_dy);
    UI_CHECK(ui_length > 0.0F);
    const float direction_x = ui_dx / ui_length;
    const float direction_y = ui_dy / ui_length;
    const float centre_x = placed.image.x + placed.image.width * 0.5F;
    const float centre_y = placed.image.y + placed.image.height * 0.5F;

    const editor::ViewportFrame pressed =
        draw(centre_x + direction_x * 42.0F,
             centre_y + direction_y * 42.0F, true);
    UI_CHECK(pressed.picked == clip_node);
    const editor::ViewportFrame dragged =
        draw(centre_x + direction_x * 92.0F,
             centre_y + direction_y * 92.0F, true);
    UI_CHECK(dragged.dragging);
    UI_CHECK(dragged.gizmo_axis == editor::GizmoAxis::X);
    UI_CHECK(scene::dot(scene::normalize(dragged.translation), local_x) >
             0.999F);
    UI_CHECK(dragged.translation.x > 0.0F);
    UI_CHECK(dragged.translation.y < 0.0F);
}

// A Stage panel with the clip selected, driven the way the shell drives it:
// after each frame the shell hands the keyboard to a running keyboard
// transform.
struct StageTools final {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    rhi::Texture target;
    editor::ViewportPanel panel{gui::ui_id("stage-tools"),
                                editor::ViewportRole::Stage};
    scene::Scene scn = make_scene();
    std::array<doc::NodeId, 1U> selection{clip_node};
    float centre_x{};
    float centre_y{};

    StageTools() {
        const editor::ViewportFrame placed = frame(0.0F, 0.0F);
        centre_x = placed.image.x + placed.image.width * 0.5F;
        centre_y = placed.image.y + placed.image.height * 0.5F;
    }

    editor::ViewportFrame frame(float x, float y, bool down = false,
                                std::string_view text = {},
                                std::optional<gui::Key> key = {},
                                bool right = false,
                                gui::KeyMods mods = gui::KeyMods::None) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        input.set_mouse_button(gui::MouseButton::Right, right);
        input.set_mods(mods);
        if (!text.empty()) {
            input.add_text(text);
        }
        if (key.has_value()) {
            input.add_key(*key);
        }
        ctx.new_frame();
        const editor::ViewportFrame out = panel.draw(
            ctx, Harness::bounds, target, scn, selection, clip_node);
        ctx.set_navigating(panel.transforming());
        return out;
    }

    // A point at `radius` pixels from the gizmo centre, `degrees`
    // anticlockwise from screen right.
    [[nodiscard]] std::pair<float, float> around(float radius,
                                                 float degrees) const {
        const float radians = degrees * 3.14159265F / 180.0F;
        return {centre_x + radius * std::cos(radians),
                centre_y - radius * std::sin(radians)};
    }
};

[[nodiscard]] bool near(scene::Vec3 a, scene::Vec3 b, float tolerance) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) &&
           near(a.z, b.z, tolerance);
}

void the_move_gizmo_defaults_to_world_axes_like_blender() {
    StageTools h;
    UI_CHECK(h.panel.orientation() == editor::TransformOrientation::Global);
    UI_CHECK(h.panel.tool() == editor::TransformTool::Move);
    // Rolled an eighth of a turn, the clip's own X points up and right; the
    // global gizmo's X arrow still runs straight across the screen.
    h.scn.media.front().world = scene::multiply(
        scene::rotation(0.0F, 0.0F, 0.125F),
        scene::scaling(scene::Vec3{2.0F, 2.0F, 1.0F}));
    static_cast<void>(h.frame(h.centre_x + 42.0F, h.centre_y, true));
    const editor::ViewportFrame dragged =
        h.frame(h.centre_x + 92.0F, h.centre_y, true);
    UI_CHECK(dragged.gizmo_axis == editor::GizmoAxis::X);
    UI_CHECK(dragged.transform.has_value());
    UI_CHECK(dragged.transform->tool == editor::TransformTool::Move);
    UI_CHECK(dragged.translation.x > 0.0F);
    UI_CHECK(near(dragged.translation.y, 0.0F, 1.0e-5F));
    UI_CHECK(near(dragged.translation.z, 0.0F, 1.0e-5F));
}

void a_rotate_ring_turns_about_its_axis_as_the_pointer_circles() {
    StageTools h;
    h.panel.set_tool(editor::TransformTool::Rotate);
    // The Z ring faces this camera: a circle 0.92 of the 72-pixel gizmo.
    const auto [press_x, press_y] = h.around(72.0F * 0.92F, 45.0F);
    const editor::ViewportFrame pressed = h.frame(press_x, press_y, true);
    UI_CHECK(pressed.picked == clip_node);
    const auto [half_x, half_y] = h.around(72.0F * 0.92F, 90.0F);
    static_cast<void>(h.frame(half_x, half_y, true));
    const auto [end_x, end_y] = h.around(72.0F * 0.92F, 135.0F);
    const editor::ViewportFrame dragged = h.frame(end_x, end_y, true);
    UI_CHECK(dragged.dragging);
    UI_CHECK(dragged.gizmo_axis == editor::GizmoAxis::Z);
    UI_CHECK(dragged.transform.has_value());
    UI_CHECK(dragged.transform->tool == editor::TransformTool::Rotate);
    UI_CHECK(near(dragged.transform->axis, scene::Vec3{0.0F, 0.0F, 1.0F},
                  1.0e-5F));
    // A quarter anticlockwise on screen is a quarter turn about +Z, which
    // points at this camera. The pivot is the clip.
    UI_CHECK(near(dragged.transform->turns, 0.25F, 0.003F));
    UI_CHECK(near(dragged.transform->pivot, scene::Vec3{}, 1.0e-5F));
    const editor::ViewportFrame released = h.frame(end_x, end_y, false);
    UI_CHECK(released.released);

    // The outer ring turns about the view axis, whichever way the camera
    // looks: here also +Z, pointing back at the viewer.
    const auto [ring_x, ring_y] = h.around(86.0F, 0.0F);
    static_cast<void>(h.frame(ring_x, ring_y, true));
    const auto [up_x, up_y] = h.around(86.0F, -60.0F);
    const editor::ViewportFrame clockwise = h.frame(up_x, up_y, true);
    UI_CHECK(clockwise.gizmo_axis == editor::GizmoAxis::View);
    UI_CHECK(near(clockwise.transform->axis, scene::Vec3{0.0F, 0.0F, 1.0F},
                  1.0e-5F));
    UI_CHECK(near(clockwise.transform->turns, -60.0F / 360.0F, 0.003F));
}

void scale_handles_follow_the_pointer_and_the_ring_scales_uniformly() {
    StageTools h;
    h.panel.set_tool(editor::TransformTool::Scale);
    static_cast<void>(h.frame(h.centre_x + 60.0F, h.centre_y, true));
    const editor::ViewportFrame wider =
        h.frame(h.centre_x + 120.0F, h.centre_y, true);
    UI_CHECK(wider.gizmo_axis == editor::GizmoAxis::X);
    UI_CHECK(wider.transform.has_value());
    UI_CHECK(wider.transform->tool == editor::TransformTool::Scale);
    UI_CHECK(near(wider.transform->factors, scene::Vec3{2.0F, 1.0F, 1.0F},
                  1.0e-3F));
    static_cast<void>(h.frame(h.centre_x + 120.0F, h.centre_y, false));

    // On the diagonal, clear of the Y handle that runs up into the ring.
    const auto [ring_x, ring_y] = h.around(86.0F, 45.0F);
    static_cast<void>(h.frame(ring_x, ring_y, true));
    const auto [half_x, half_y] = h.around(43.0F, 45.0F);
    const editor::ViewportFrame smaller = h.frame(half_x, half_y, true);
    UI_CHECK(smaller.gizmo_axis == editor::GizmoAxis::View);
    UI_CHECK(near(smaller.transform->factors, scene::Vec3{0.5F, 0.5F, 0.5F},
                  1.0e-3F));
}

void snapping_rounds_a_gizmo_drag_and_ctrl_inverts_it() {
    StageTools h;
    h.panel.set_snapping(true);
    static_cast<void>(h.frame(h.centre_x + 42.0F, h.centre_y, true));
    const editor::ViewportFrame snapped =
        h.frame(h.centre_x + 92.0F, h.centre_y, true);
    // Fifty pixels is a little over half a metre at this depth: one step.
    UI_CHECK(snapped.translation.x == 1.0F);
    const editor::ViewportFrame free =
        h.frame(h.centre_x + 92.0F, h.centre_y, true, {}, {}, false,
                gui::KeyMods::Ctrl);
    UI_CHECK(free.translation.x > 0.3F && free.translation.x < 0.9F);
    static_cast<void>(h.frame(h.centre_x + 92.0F, h.centre_y, false));

    // Shift+Tab over the Stage flips the toggle, as in Blender.
    static_cast<void>(h.frame(h.centre_x, h.centre_y, false, {},
                              gui::Key::Tab, false, gui::KeyMods::Shift));
    UI_CHECK(!h.panel.snapping());
}

void g_x_and_a_typed_distance_move_exactly_and_enter_confirms() {
    StageTools h;
    const editor::ViewportFrame began =
        h.frame(h.centre_x + 10.0F, h.centre_y + 10.0F, false, "g");
    UI_CHECK(began.transform_began);
    UI_CHECK(began.keyboard_transform);
    UI_CHECK(h.panel.transforming());
    UI_CHECK(h.panel.tool() == editor::TransformTool::Move);
    UI_CHECK(began.transform.has_value());
    UI_CHECK(began.transform->translation == scene::Vec3{});

    // X constrains to world X: pointer travel up and down no longer counts.
    static_cast<void>(
        h.frame(h.centre_x + 10.0F, h.centre_y + 10.0F, false, "x"));
    const editor::ViewportFrame slid =
        h.frame(h.centre_x + 110.0F, h.centre_y + 60.0F);
    UI_CHECK(slid.transform->translation.x > 0.0F);
    UI_CHECK(slid.transform->translation.y == 0.0F);
    UI_CHECK(slid.transform->translation.z == 0.0F);
    // A typed value replaces the pointer's.
    const editor::ViewportFrame typed =
        h.frame(h.centre_x + 110.0F, h.centre_y + 60.0F, false, "-2.5");
    UI_CHECK(typed.transform->translation == (scene::Vec3{-2.5F, 0.0F, 0.0F}));
    const editor::ViewportFrame confirmed = h.frame(
        h.centre_x + 110.0F, h.centre_y + 60.0F, false, {}, gui::Key::Enter);
    UI_CHECK(confirmed.transform_ended);
    UI_CHECK(!confirmed.transform_cancelled);
    UI_CHECK(confirmed.transform->translation ==
             (scene::Vec3{-2.5F, 0.0F, 0.0F}));
    UI_CHECK(!h.panel.transforming());
    // Over and done: the next frame reports nothing.
    const editor::ViewportFrame after =
        h.frame(h.centre_x + 110.0F, h.centre_y + 60.0F);
    UI_CHECK(!after.transform.has_value());
}

void an_axis_key_twice_uses_the_elements_own_axis_then_frees_it() {
    StageTools h;
    h.scn.media.front().world = scene::multiply(
        scene::rotation(0.0F, 0.0F, 0.125F),
        scene::scaling(scene::Vec3{2.0F, 2.0F, 1.0F}));
    static_cast<void>(h.frame(h.centre_x, h.centre_y, false, "g"));
    static_cast<void>(h.frame(h.centre_x, h.centre_y, false, "x"));
    static_cast<void>(h.frame(h.centre_x, h.centre_y, false, "x"));
    const editor::ViewportFrame local =
        h.frame(h.centre_x, h.centre_y, false, "1");
    // Roll turns the clip's X down to the right on screen (see the rotated
    // gizmo test above); a typed metre goes that way.
    const float eighth = std::sqrt(0.5F);
    UI_CHECK(near(local.transform->translation,
                  scene::Vec3{eighth, -eighth, 0.0F}, 1.0e-4F));
    // Third press: unconstrained, the pointer moves on the view plane.
    static_cast<void>(h.frame(h.centre_x, h.centre_y, false, {},
                              gui::Key::Backspace));
    static_cast<void>(h.frame(h.centre_x, h.centre_y, false, "x"));
    const editor::ViewportFrame free =
        h.frame(h.centre_x + 40.0F, h.centre_y - 40.0F);
    UI_CHECK(free.transform->translation.x > 0.0F);
    UI_CHECK(free.transform->translation.y > 0.0F);
    UI_CHECK(near(free.transform->translation.z, 0.0F, 1.0e-5F));
}

void a_right_click_cancels_and_its_press_is_swallowed() {
    StageTools h;
    const auto [start_x, start_y] = h.around(100.0F, 0.0F);
    static_cast<void>(h.frame(start_x, start_y, false, "r"));
    UI_CHECK(h.panel.tool() == editor::TransformTool::Rotate);
    const auto [turn_x, turn_y] = h.around(100.0F, 30.0F);
    const editor::ViewportFrame turning = h.frame(turn_x, turn_y);
    UI_CHECK(near(turning.transform->turns, 30.0F / 360.0F, 0.002F));
    UI_CHECK(near(turning.transform->axis, scene::Vec3{0.0F, 0.0F, 1.0F},
                  1.0e-5F));
    const editor::ViewportFrame cancelled =
        h.frame(turn_x, turn_y, false, {}, {}, true);
    UI_CHECK(cancelled.transform_ended);
    UI_CHECK(cancelled.transform_cancelled);
    UI_CHECK(cancelled.transform->tool == editor::TransformTool::Rotate);
    UI_CHECK(cancelled.transform->turns == 0.0F);
    // The right button is still down: it belongs to the cancel, so the camera
    // must not start looking with it.
    UI_CHECK(cancelled.keyboard_transform);
    UI_CHECK(h.panel.transforming());
    static_cast<void>(h.frame(turn_x, turn_y));
    UI_CHECK(!h.panel.transforming());
}

void a_left_click_confirms_without_picking_and_s_shift_z_scales_a_plane() {
    StageTools h;
    static_cast<void>(
        h.frame(h.centre_x + 100.0F, h.centre_y, false, "s"));
    static_cast<void>(h.frame(h.centre_x + 100.0F, h.centre_y, false, "Z"));
    const editor::ViewportFrame scaled = h.frame(h.centre_x + 200.0F, h.centre_y);
    UI_CHECK(near(scaled.transform->factors, scene::Vec3{2.0F, 2.0F, 1.0F},
                  1.0e-3F));
    // Shift slows the pointer to a tenth: another 100 pixels adds 0.1.
    const editor::ViewportFrame fine =
        h.frame(h.centre_x + 300.0F, h.centre_y, false, {}, {}, false,
                gui::KeyMods::Shift);
    UI_CHECK(near(fine.transform->factors.x, 2.1F, 1.0e-3F));
    const editor::ViewportFrame confirmed =
        h.frame(h.centre_x + 300.0F, h.centre_y, true);
    UI_CHECK(confirmed.transform_ended);
    UI_CHECK(!confirmed.transform_cancelled);
    UI_CHECK(!confirmed.pressed);
    UI_CHECK(!confirmed.picked.has_value());
    // Held after the confirm, the button still picks nothing.
    const editor::ViewportFrame held =
        h.frame(h.centre_x + 300.0F, h.centre_y, true);
    UI_CHECK(!held.pressed && !held.dragging);
}

void keyboard_transforms_start_only_over_the_image_with_a_selection() {
    StageTools h;
    const editor::ViewportFrame outside = h.frame(5.0F, 5.0F, false, "g");
    UI_CHECK(!outside.transform_began);
    UI_CHECK(!h.panel.transforming());
    // With no primary there is nothing to transform.
    h.input.new_frame();
    h.input.set_mouse_position(h.centre_x, h.centre_y);
    h.input.add_text("g");
    h.ctx.new_frame();
    const editor::ViewportFrame unselected =
        h.panel.draw(h.ctx, Harness::bounds, h.target, h.scn, {}, {});
    UI_CHECK(!unselected.transform_began);
    // A program view never takes keyboard transforms.
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    editor::ViewportPanel program{gui::ui_id("program-tools")};
    input.new_frame();
    input.set_mouse_position(500.0F, 500.0F);
    input.add_text("g");
    ctx.new_frame();
    const std::array<doc::NodeId, 1U> selection{clip_node};
    const editor::ViewportFrame on_program = program.draw(
        ctx, Harness::bounds, h.target, h.scn, selection, clip_node);
    UI_CHECK(!on_program.transform_began);
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

class ViewportProjectionFixture final {
  public:
    ViewportProjectionFixture()
        : path_{std::filesystem::temp_directory_path() /
                "mediaserver_viewport_projection_refresh.obj"} {}

    ViewportProjectionFixture(const ViewportProjectionFixture&) = delete;
    ViewportProjectionFixture&
    operator=(const ViewportProjectionFixture&) = delete;

    ~ViewportProjectionFixture() {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove(path_, ignored));
    }

    [[nodiscard]] std::string write(std::string_view contents) const {
        std::ofstream file{path_, std::ios::trunc};
        file << contents;
        if (!file) {
            throw std::runtime_error{
                "could not write viewport projection fixture"};
        }
        return path_.string();
    }

  private:
    std::filesystem::path path_;
};

void an_explicit_same_path_surface_refresh_publishes_a_new_wire() {
    ViewportProjectionFixture fixture;
    const std::string source = fixture.write(
        "v -1 1 0\n"
        "v -1 -1 0\n"
        "v 1 1 0\n"
        "vt 0 1\n"
        "vt 0 0\n"
        "vt 1 1\n"
        "f 1/1 2/2 3/3\n");
    SurfaceHarness h;
    h.feed_map.surfaces[0U] =
        make_surface(output::ProjectionSurfaceShape::ImportedMesh);
    h.feed_map.surfaces[0U].mesh_source = source;

    static_cast<void>(h.frame(0.0F, 0.0F, false));
    UI_CHECK(editor::detail::wait_for_viewport_projection_surface_for_test(
        h.panel, surface_node));
    static_cast<void>(h.frame(0.0F, 0.0F, false));
    const std::size_t one_triangle = h.list.vertices().size();

    static_cast<void>(fixture.write(
        "v -1 1 0\n"
        "v -1 -1 0\n"
        "v 1 1 0\n"
        "v 1 -1 0\n"
        "vt 0 1\n"
        "vt 0 0\n"
        "vt 1 1\n"
        "vt 1 0\n"
        "f 1/1 2/2 3/3\n"
        "f 3/3 2/2 4/4\n"));
    static_cast<void>(h.frame(0.0F, 0.0F, false));
    UI_CHECK(h.list.vertices().size() == one_triangle);

    UI_CHECK(h.panel.refresh_projection_surface(surface_node));
    UI_CHECK(editor::detail::wait_for_viewport_projection_surface_for_test(
        h.panel, surface_node));
    static_cast<void>(h.frame(0.0F, 0.0F, false));
    UI_CHECK(h.list.vertices().size() > one_triangle);
}

void mask_mapping_preserves_source_and_authored_canvas_spaces() {
    constexpr gui::Rect image{10.0F, 20.0F, 800.0F, 450.0F};
    constexpr scene::Canvas canvas{1920U, 1080U};
    const auto authored = editor::mask_normalized_to_viewport(
        image, canvas, scene::MaskCoordinateSpace::AuthoredCanvas, nullptr,
        0.25F, 0.75F);
    UI_CHECK(authored.has_value());
    UI_CHECK(near(authored->x, 210.0F));
    UI_CHECK(near(authored->y, 357.5F));
    const auto authored_round_trip = editor::mask_viewport_to_normalized(
        image, canvas, scene::MaskCoordinateSpace::AuthoredCanvas, nullptr,
        authored->x, authored->y);
    UI_CHECK(authored_round_trip.has_value());
    UI_CHECK(near(authored_round_trip->x, 0.25F, 0.0001F));
    UI_CHECK(near(authored_round_trip->y, 0.75F, 0.0001F));

    const editor::MaskViewportSourceMapping source{{
        editor::MaskViewportCanvasPoint{200.0F, 100.0F},
        editor::MaskViewportCanvasPoint{1600.0F, 160.0F},
        editor::MaskViewportCanvasPoint{1460.0F, 980.0F},
        editor::MaskViewportCanvasPoint{260.0F, 900.0F},
    }};
    const auto mapped = editor::mask_normalized_to_viewport(
        image, canvas, scene::MaskCoordinateSpace::SourceLayer, &source,
        0.31F, 0.67F);
    UI_CHECK(mapped.has_value());
    const auto round_trip = editor::mask_viewport_to_normalized(
        image, canvas, scene::MaskCoordinateSpace::SourceLayer, &source,
        mapped->x, mapped->y);
    UI_CHECK(round_trip.has_value());
    UI_CHECK(near(round_trip->x, 0.31F, 0.0001F));
    UI_CHECK(near(round_trip->y, 0.67F, 0.0001F));

    UI_CHECK(!editor::mask_normalized_to_viewport(
                  image, canvas, scene::MaskCoordinateSpace::SourceLayer,
                  nullptr, 0.5F, 0.5F)
                  .has_value());
    const editor::MaskViewportSourceMapping degenerate{};
    UI_CHECK(!editor::mask_viewport_to_normalized(
                  image, canvas, scene::MaskCoordinateSpace::SourceLayer,
                  &degenerate, mapped->x, mapped->y)
                  .has_value());
}

void mask_mapping_follows_a_layer_seen_in_perspective() {
    // A full-canvas image, so viewport and canvas pixels coincide.
    constexpr gui::Rect image{0.0F, 0.0F, 1920.0F, 1080.0F};
    constexpr scene::Canvas canvas{1920U, 1080U};
    // A layer turned away from the camera: its far edge is shorter.
    const editor::MaskViewportSourceMapping turned{{
        editor::MaskViewportCanvasPoint{400.0F, 200.0F},
        editor::MaskViewportCanvasPoint{1500.0F, 350.0F},
        editor::MaskViewportCanvasPoint{1500.0F, 730.0F},
        editor::MaskViewportCanvasPoint{400.0F, 880.0F},
    }, scene::MaskCoordinateExtent{1100.0F, 530.0F}};
    // Under perspective the surface's centre is where its diagonals cross,
    // which lies toward the far edge; the corners' average does not.
    const auto centre = editor::mask_normalized_to_viewport(
        image, canvas, scene::MaskCoordinateSpace::SourceLayer, &turned, 0.5F,
        0.5F);
    UI_CHECK(centre.has_value());
    // Diagonal UL-BR falls 530 px over 1100 px and UR-BL rises by the same,
    // so they meet at y = 540, 340/530 of the way across.
    const float t = (540.0F - 200.0F) / 530.0F;
    UI_CHECK(near(centre->x, 400.0F + t * 1100.0F, 0.05F));
    UI_CHECK(near(centre->y, 540.0F, 0.05F));
    UI_CHECK(centre->x > 1000.0F); // the corners average to x = 950

    // A straight mask edge stays straight on screen.
    const auto top = editor::mask_normalized_to_viewport(
        image, canvas, scene::MaskCoordinateSpace::SourceLayer, &turned,
        0.25F, 0.0F);
    const auto bottom = editor::mask_normalized_to_viewport(
        image, canvas, scene::MaskCoordinateSpace::SourceLayer, &turned,
        0.25F, 1.0F);
    const auto middle = editor::mask_normalized_to_viewport(
        image, canvas, scene::MaskCoordinateSpace::SourceLayer, &turned,
        0.25F, 0.4F);
    UI_CHECK(top.has_value() && bottom.has_value() && middle.has_value());
    const float cross =
        (bottom->x - top->x) * (middle->y - top->y) -
        (bottom->y - top->y) * (middle->x - top->x);
    UI_CHECK(std::fabs(cross) < 1.0F);

    const auto round_trip = editor::mask_viewport_to_normalized(
        image, canvas, scene::MaskCoordinateSpace::SourceLayer, &turned,
        centre->x, centre->y);
    UI_CHECK(round_trip.has_value());
    UI_CHECK(near(round_trip->x, 0.5F, 0.0001F));
    UI_CHECK(near(round_trip->y, 0.5F, 0.0001F));
}

void mask_overlays_and_selection_stay_in_the_mask_domain() {
    MaskHarness h;
    const editor::MaskViewportFrame placed = h.frame(0.0F, 0.0F, false);
    UI_CHECK(placed.mapping_valid);
    UI_CHECK(h.list.vertices().size() > 100U);
    bool cyan = false;
    bool feather = false;
    bool expansion = false;
    for (const gui::UiVertex& vertex : h.list.vertices()) {
        cyan = cyan ||
               (vertex.r < 0.30F && vertex.g > 0.85F && vertex.b > 0.95F);
        feather = feather ||
                  (near(vertex.r, 0.26F, 0.001F) &&
                   near(vertex.g, 0.68F, 0.001F) &&
                   near(vertex.b, 1.0F, 0.001F));
        expansion = expansion ||
                    (near(vertex.r, 0.96F, 0.001F) &&
                     near(vertex.g, 0.56F, 0.001F) &&
                     near(vertex.b, 0.20F, 0.001F));
    }
    UI_CHECK(cyan);
    UI_CHECK(feather);
    UI_CHECK(expansion);

    const gui::UiPoint second_point = h.screen(0.80F, 0.20F, placed.image);
    const editor::MaskViewportFrame pressed =
        h.frame(second_point.x, second_point.y, true);
    UI_CHECK(pressed.hit == editor::MaskViewportHit::Point);
    UI_CHECK(pressed.selection_changed);
    UI_CHECK(pressed.selection.mask == bezier_mask_node);
    UI_CHECK(pressed.selection.point == doc::NodeId{101U});
    UI_CHECK(pressed.selection.mask != clip_node);
    UI_CHECK(!pressed.intent.has_value());
    UI_CHECK(!h.panel.dragging());
    const editor::MaskViewportFrame released =
        h.frame(second_point.x, second_point.y, false);
    UI_CHECK(!released.intent.has_value());
}

void mask_hit_priority_prefers_selected_handles_then_points_then_pen_path() {
    MaskHarness handle;
    handle.masks.back().points.front().out_x = 0.208F;
    const editor::MaskViewportFrame handle_placed =
        handle.frame(0.0F, 0.0F, false);
    const gui::UiPoint out_handle =
        handle.screen(0.208F, 0.20F, handle_placed.image);
    const editor::MaskViewportFrame handle_press =
        handle.frame(out_handle.x, out_handle.y, true);
    UI_CHECK(handle_press.hit == editor::MaskViewportHit::OutHandle);

    MaskHarness anchor;
    const editor::MaskViewportFrame anchor_placed =
        anchor.frame(0.0F, 0.0F, false);
    const gui::UiPoint point =
        anchor.screen(0.20F, 0.20F, anchor_placed.image);
    const editor::MaskViewportFrame point_press =
        anchor.frame(point.x, point.y, true);
    UI_CHECK(point_press.hit == editor::MaskViewportHit::Point);

    MaskHarness pen;
    pen.pen_active = true;
    const editor::MaskViewportFrame pen_placed =
        pen.frame(0.0F, 0.0F, false);
    const gui::UiPoint path = pen.screen(0.50F, 0.20F, pen_placed.image);
    const editor::MaskViewportFrame path_press =
        pen.frame(path.x, path.y, true);
    UI_CHECK(path_press.hit == editor::MaskViewportHit::Path);
    UI_CHECK(!path_press.intent.has_value());
    const editor::MaskViewportFrame path_release =
        pen.frame(path.x, path.y, false);
    UI_CHECK(path_release.intent.has_value());
    UI_CHECK(std::holds_alternative<editor::MaskViewportPointInsertIntent>(
        *path_release.intent));
    const auto& insert =
        std::get<editor::MaskViewportPointInsertIntent>(*path_release.intent);
    UI_CHECK(insert.scene == mask_scene);
    UI_CHECK(insert.target == clip_node);
    UI_CHECK(insert.mask == bezier_mask_node);
    UI_CHECK(insert.index == 1U);
    UI_CHECK(near(insert.geometry.x, 0.50F, 0.01F));
    UI_CHECK(near(insert.geometry.y, 0.20F, 0.01F));
}

void mask_point_and_handle_modifiers_are_deterministic() {
    MaskHarness point_drag;
    const editor::MaskViewportFrame placed =
        point_drag.frame(0.0F, 0.0F, false);
    const gui::UiPoint start =
        point_drag.screen(0.20F, 0.20F, placed.image);
    const gui::UiPoint end = point_drag.screen(0.32F, 0.27F, placed.image);
    static_cast<void>(point_drag.frame(start.x, start.y, true,
                                       gui::KeyMods::Shift));
    const editor::MaskViewportFrame preview = point_drag.frame(
        end.x, end.y, true, gui::KeyMods::Shift);
    UI_CHECK(preview.dragging);
    UI_CHECK(!preview.intent.has_value());
    const editor::MaskViewportFrame committed = point_drag.frame(
        end.x, end.y, false, gui::KeyMods::Shift);
    UI_CHECK(committed.intent.has_value());
    const auto& point_intent =
        std::get<editor::MaskViewportPointEditIntent>(*committed.intent);
    UI_CHECK(point_intent.point == first_mask_point);
    UI_CHECK(point_intent.patch.x.has_value());
    UI_CHECK(near(*point_intent.patch.x, 0.32F, 0.001F));
    UI_CHECK(!point_intent.patch.y.has_value());
    UI_CHECK(point_intent.patch.in_x.has_value());
    UI_CHECK(point_intent.patch.out_x.has_value());
    UI_CHECK(!point_intent.patch.in_y.has_value());
    UI_CHECK(!point_intent.patch.out_y.has_value());

    const auto drag_handle = [](bool break_tangent) {
        MaskHarness h;
        const editor::MaskViewportFrame handle_placed =
            h.frame(0.0F, 0.0F, false);
        const gui::UiPoint handle =
            h.screen(0.40F, 0.20F, handle_placed.image);
        const gui::UiPoint moved =
            h.screen(0.40F, 0.35F, handle_placed.image);
        const gui::KeyMods mods =
            break_tangent ? gui::KeyMods::Alt : gui::KeyMods::None;
        static_cast<void>(h.frame(handle.x, handle.y, true, mods));
        static_cast<void>(h.frame(moved.x, moved.y, true, mods));
        return h.frame(moved.x, moved.y, false, mods);
    };
    const editor::MaskViewportFrame mirrored = drag_handle(false);
    UI_CHECK(mirrored.intent.has_value());
    const auto& mirrored_edit =
        std::get<editor::MaskViewportPointEditIntent>(*mirrored.intent);
    UI_CHECK(mirrored_edit.patch.out_y.has_value());
    UI_CHECK(mirrored_edit.patch.in_x.has_value());
    UI_CHECK(mirrored_edit.patch.in_y.has_value());
    UI_CHECK(near(*mirrored_edit.patch.in_x, 0.0F, 0.001F));
    UI_CHECK(near(*mirrored_edit.patch.in_y, 0.05F, 0.001F));

    const editor::MaskViewportFrame broken = drag_handle(true);
    UI_CHECK(broken.intent.has_value());
    const auto& broken_edit =
        std::get<editor::MaskViewportPointEditIntent>(*broken.intent);
    UI_CHECK(broken_edit.patch.out_y.has_value());
    UI_CHECK(!broken_edit.patch.in_x.has_value());
    UI_CHECK(!broken_edit.patch.in_y.has_value());
}

void program_output_authors_masks_against_the_connector_image() {
    ProgramMaskHarness program;
    const editor::MaskViewportFrame placed = program.frame(0.0F, 0.0F, false);
    UI_CHECK(placed.object_selection_locked);
    // A Program-role panel used to refuse the mask domain outright; what
    // decides now is having a picture and a canvas to resolve against.
    UI_CHECK(placed.mapping_valid);
    UI_CHECK(placed.image.width > 0.0F);

    // The output raster is 960x1080, so its fitted image is taller than wide
    // where the 1920x1080 canvas letterboxed. Proving the panel used the
    // projection's canvas and not the scene's.
    UI_CHECK(placed.image.height > placed.image.width);

    const gui::UiPoint start = program.screen(0.20F, 0.20F, placed.image);
    const gui::UiPoint end = program.screen(0.34F, 0.28F, placed.image);
    static_cast<void>(program.frame(start.x, start.y, true));
    const editor::MaskViewportFrame dragging =
        program.frame(end.x, end.y, true);
    UI_CHECK(dragging.dragging);
    UI_CHECK(!dragging.intent.has_value());

    const editor::MaskViewportFrame committed =
        program.frame(end.x, end.y, false);
    UI_CHECK(committed.intent.has_value());
    const auto& edit =
        std::get<editor::MaskViewportPointEditIntent>(*committed.intent);
    UI_CHECK(edit.scene == mask_scene);
    UI_CHECK(edit.target == clip_node);
    UI_CHECK(edit.mask == bezier_mask_node);
    UI_CHECK(edit.point == first_mask_point);
    // The gesture lands in the mask's own normalized space, exactly as the
    // same drag would on Stage: the output crop is absorbed by the corners.
    UI_CHECK(edit.patch.x.has_value());
    UI_CHECK(edit.patch.y.has_value());
    UI_CHECK(near(*edit.patch.x, 0.34F, 0.002F));
    UI_CHECK(near(*edit.patch.y, 0.28F, 0.002F));

    // A locked target still draws the overlay and still refuses to author.
    ProgramMaskHarness locked;
    locked.authoring_enabled = false;
    const editor::MaskViewportFrame locked_placed =
        locked.frame(0.0F, 0.0F, false);
    const gui::UiPoint from = locked.screen(0.20F, 0.20F, locked_placed.image);
    const gui::UiPoint to = locked.screen(0.34F, 0.28F, locked_placed.image);
    static_cast<void>(locked.frame(from.x, from.y, true));
    static_cast<void>(locked.frame(to.x, to.y, true));
    UI_CHECK(!locked.frame(to.x, to.y, false).intent.has_value());
}

void mask_gestures_commit_once_and_cancel_or_noop_inertly() {
    MaskHarness translated;
    translated.masks.resize(1U);
    translated.selection = editor::MasksPanelSelection{
        mask_scene, clip_node, rectangle_mask_node, {}};
    const editor::MaskViewportFrame placed =
        translated.frame(0.0F, 0.0F, false);
    const gui::UiPoint start =
        translated.screen(0.50F, 0.50F, placed.image);
    const gui::UiPoint end = translated.screen(0.60F, 0.55F, placed.image);
    const editor::MaskViewportFrame pressed =
        translated.frame(start.x, start.y, true);
    UI_CHECK(pressed.hit == editor::MaskViewportHit::WholeMask);
    UI_CHECK(!pressed.intent.has_value());
    const editor::MaskViewportFrame held =
        translated.frame(end.x, end.y, true);
    UI_CHECK(held.dragging);
    UI_CHECK(!held.intent.has_value());
    const editor::MaskViewportFrame released =
        translated.frame(end.x, end.y, false);
    UI_CHECK(released.released);
    UI_CHECK(released.intent.has_value());
    const auto& translate =
        std::get<editor::MaskViewportTranslateIntent>(*released.intent);
    UI_CHECK(translate.mask == rectangle_mask_node);
    UI_CHECK(near(translate.delta_x, 0.10F, 0.001F));
    UI_CHECK(near(translate.delta_y, 0.05F, 0.001F));

    MaskHarness noop;
    noop.masks.resize(1U);
    noop.selection = editor::MasksPanelSelection{
        mask_scene, clip_node, rectangle_mask_node, {}};
    const editor::MaskViewportFrame noop_placed =
        noop.frame(0.0F, 0.0F, false);
    const gui::UiPoint noop_point =
        noop.screen(0.50F, 0.50F, noop_placed.image);
    static_cast<void>(noop.frame(noop_point.x, noop_point.y, true));
    const editor::MaskViewportFrame noop_release =
        noop.frame(noop_point.x, noop_point.y, false);
    UI_CHECK(!noop_release.intent.has_value());

    MaskHarness cancelled;
    cancelled.masks.resize(1U);
    cancelled.selection = editor::MasksPanelSelection{
        mask_scene, clip_node, rectangle_mask_node, {}};
    const editor::MaskViewportFrame cancel_placed =
        cancelled.frame(0.0F, 0.0F, false);
    const gui::UiPoint cancel_start =
        cancelled.screen(0.50F, 0.50F, cancel_placed.image);
    const gui::UiPoint cancel_end =
        cancelled.screen(0.60F, 0.55F, cancel_placed.image);
    static_cast<void>(cancelled.frame(cancel_start.x, cancel_start.y, true));
    static_cast<void>(cancelled.frame(cancel_end.x, cancel_end.y, true,
                                      gui::KeyMods::None,
                                      gui::Key::Escape));
    const editor::MaskViewportFrame cancel_release =
        cancelled.frame(cancel_end.x, cancel_end.y, false);
    UI_CHECK(cancel_release.cancelled);
    UI_CHECK(!cancel_release.intent.has_value());
}

void primitive_mask_handles_resize_and_rotate_on_canvas() {
    MaskHarness resized;
    resized.masks.resize(1U);
    resized.selection = editor::MasksPanelSelection{
        mask_scene, clip_node, rectangle_mask_node, {}};
    const editor::MaskViewportFrame placed =
        resized.frame(0.0F, 0.0F, false);
    const gui::UiPoint corner = resized.screen(0.10F, 0.15F, placed.image);
    const gui::UiPoint moved = resized.screen(0.20F, 0.25F, placed.image);
    const auto pressed = resized.frame(corner.x, corner.y, true);
    UI_CHECK(pressed.hit == editor::MaskViewportHit::ResizeTopLeft);
    UI_CHECK(resized.frame(moved.x, moved.y, true).dragging);
    const auto resize_commit = resized.frame(moved.x, moved.y, false);
    UI_CHECK(resize_commit.intent.has_value());
    const auto& resized_shape =
        std::get<editor::MaskViewportShapeEditIntent>(
            *resize_commit.intent);
    UI_CHECK(near(resized_shape.bounds.left, 0.20F, 0.002F));
    UI_CHECK(near(resized_shape.bounds.top, 0.25F, 0.002F));
    UI_CHECK(near(resized_shape.bounds.right, 0.80F, 0.002F));
    UI_CHECK(near(resized_shape.bounds.bottom, 0.75F, 0.002F));

    MaskHarness rotated;
    rotated.masks.resize(1U);
    rotated.selection = editor::MasksPanelSelection{
        mask_scene, clip_node, rectangle_mask_node, {}};
    const auto rotation_placed = rotated.frame(0.0F, 0.0F, false);
    const gui::UiPoint centre =
        rotated.screen(0.50F, 0.50F, rotation_placed.image);
    const gui::UiPoint top =
        rotated.screen(0.50F, 0.15F, rotation_placed.image);
    const float vx = top.x - centre.x;
    const float vy = top.y - centre.y;
    const float length = std::sqrt(vx * vx + vy * vy);
    const gui::UiPoint rotate_handle{
        top.x + vx / length * 24.0F,
        top.y + vy / length * 24.0F};
    const gui::UiPoint quarter_turn{
        centre.x - vy / length * (length + 24.0F),
        centre.y + vx / length * (length + 24.0F)};
    const auto rotate_pressed = rotated.frame(
        rotate_handle.x, rotate_handle.y, true);
    UI_CHECK(rotate_pressed.hit == editor::MaskViewportHit::Rotate);
    UI_CHECK(rotated.frame(quarter_turn.x, quarter_turn.y, true).dragging);
    const auto rotation_commit = rotated.frame(
        quarter_turn.x, quarter_turn.y, false);
    UI_CHECK(rotation_commit.intent.has_value());
    const auto& rotated_shape =
        std::get<editor::MaskViewportShapeEditIntent>(
            *rotation_commit.intent);
    UI_CHECK(near(rotated_shape.rotation_turns, 0.25F, 0.002F));
}

// The tracked camera and the plate are the two things an operator looks at to
// confirm ICVFX is doing what they think. Both are drawn in the same green so
// they read as one subject, and both must be absent when there is nothing
// truthful to draw.
// The two things an operator looks at to confirm ICVFX is doing what they
// think. They are drawn in the same green so they read as one subject, and
// each appears only where it means something: the camera's frustum is a thing
// in the scene, so it is Stage chrome; the plate is a region of the image, so
// it belongs on Program Output too.
void the_tracked_camera_and_plate_overlays_appear_only_when_live() {
    const auto count_green = [](const gui::UiDrawList& list) {
        std::size_t green = 0U;
        for (const gui::UiVertex& vertex : list.vertices()) {
            if (vertex.g > 0.75F && vertex.r < 0.45F && vertex.b < 0.70F) {
                ++green;
            }
        }
        return green;
    };
    const auto brightest_green = [](const gui::UiDrawList& list) {
        float alpha = 0.0F;
        for (const gui::UiVertex& vertex : list.vertices()) {
            if (vertex.g > 0.75F && vertex.r < 0.45F) {
                alpha = std::max(alpha, vertex.a);
            }
        }
        return alpha;
    };

    const auto tracked_scene = []() {
        scene::Scene value = make_scene();
        scene::TrackedCameraView view{};
        view.camera = doc::NodeId{11U};
        view.source = doc::NodeId{3U};
        view.authored = value.camera;
        view.icvfx.inner_frustum = true;
        view.pose.position = scene::Vec3{1.0F, 1.5F, 3.0F};
        view.pose.field_of_view = 0.06F;
        view.pose.aspect_ratio = 16.0F / 9.0F;
        view.pose.valid = true;
        value.tracked_cameras.push_back(view);
        return value;
    };

    // Stage, nothing tracked: no overlay at all. Naming the presence of a
    // thing and never the absence of one.
    Harness idle;
    editor::ViewportPanel stage_idle{gui::ui_id("stage.idle"),
                                     editor::ViewportRole::Stage};
    idle.input.new_frame();
    idle.ctx.new_frame();
    static_cast<void>(
        stage_idle.draw(idle.ctx, Harness::bounds, idle.target, idle.scene));
    UI_CHECK(count_green(idle.list) == 0U);

    // Stage, a live pose: the physical camera's frustum appears.
    Harness live;
    live.scene = tracked_scene();
    editor::ViewportPanel stage_live{gui::ui_id("stage.live"),
                                     editor::ViewportRole::Stage};
    live.input.new_frame();
    live.ctx.new_frame();
    static_cast<void>(
        stage_live.draw(live.ctx, Harness::bounds, live.target, live.scene));
    UI_CHECK(count_green(live.list) > 0U);

    // A held pose still draws - every node is holding the same one, and the
    // wall really is showing it - but dimmer, so it reads as held rather than
    // as live tracking.
    Harness held;
    held.scene = tracked_scene();
    held.scene.tracked_cameras.front().pose.held = true;
    editor::ViewportPanel stage_held{gui::ui_id("stage.held"),
                                     editor::ViewportRole::Stage};
    held.input.new_frame();
    held.ctx.new_frame();
    static_cast<void>(
        stage_held.draw(held.ctx, Harness::bounds, held.target, held.scene));
    UI_CHECK(count_green(held.list) > 0U);
    UI_CHECK(brightest_green(held.list) < brightest_green(live.list));

    // Program Output: no scene chrome, but the plate outline is exactly the
    // thing an operator is judging here, so it draws.
    Harness plated;
    plated.scene = tracked_scene();
    plated.scene.renderer_settings.icvfx.inner_frustum = true;
    const std::array<editor::ViewportPlate, 1U> plates{
        {{scene::CanvasRect{plated.scene.canvas.width / 4U,
                            plated.scene.canvas.height / 4U,
                            plated.scene.canvas.width / 2U,
                            plated.scene.canvas.height / 2U},
          16U}}};
    plated.input.new_frame();
    plated.ctx.new_frame();
    static_cast<void>(plated.panel.draw(
        plated.ctx, Harness::bounds, plated.target, plated.scene, {}, {},
        nullptr, nullptr, 60.0F, nullptr, doc::NodeId{}, nullptr,
        std::span<const editor::ViewportPlate>{plates}));
    UI_CHECK(count_green(plated.list) > 0U);

    // Two cameras put two outlines on the wall, so an operator can see both
    // plates and where they overlap rather than only whichever was first.
    Harness stacked;
    stacked.scene = tracked_scene();
    stacked.scene.renderer_settings.icvfx.inner_frustum = true;
    const std::array<editor::ViewportPlate, 2U> two{
        {{scene::CanvasRect{10U, 10U, 60U, 40U}, 8U},
         {scene::CanvasRect{200U, 120U, 60U, 40U}, 8U}}};
    stacked.input.new_frame();
    stacked.ctx.new_frame();
    static_cast<void>(stacked.panel.draw(
        stacked.ctx, Harness::bounds, stacked.target, stacked.scene, {}, {},
        nullptr, nullptr, 60.0F, nullptr, doc::NodeId{}, nullptr,
        std::span<const editor::ViewportPlate>{two}));
    UI_CHECK(count_green(stacked.list) > count_green(plated.list));

    // And without a plate, Program draws none of it.
    Harness unplated;
    unplated.scene = tracked_scene();
    unplated.input.new_frame();
    unplated.ctx.new_frame();
    static_cast<void>(unplated.panel.draw(unplated.ctx, Harness::bounds,
                                          unplated.target, unplated.scene));
    UI_CHECK(count_green(unplated.list) == 0U);
}

void coverage_toggle_is_a_typed_target_scoped_intent() {
    MaskHarness h;
    const editor::MaskViewportFrame placed = h.frame(0.0F, 0.0F, false);
    const float x = placed.image.x + placed.image.width - 55.0F;
    const float y = placed.image.y + 15.0F;
    const editor::MaskViewportFrame pressed = h.frame(x, y, true);
    UI_CHECK(!pressed.intent.has_value());
    const editor::MaskViewportFrame released = h.frame(x, y, false);
    UI_CHECK(released.intent.has_value());
    const auto& coverage =
        std::get<editor::MaskViewportCoverageIntent>(*released.intent);
    UI_CHECK(coverage.scene == mask_scene);
    UI_CHECK(coverage.target == clip_node);
    UI_CHECK(coverage.visible);
}

void light_sources_and_cutoffs_have_authored_world_dimensions() {
    scene::PointLight light;
    light.position = {3, 4, -2};
    light.kind = scene::LocalLightKind::Sphere;
    light.radius = 0.7F;
    light.range = 19.0F; // Previously silently capped at eight metres.
    for (const float radius : {0.7F, 1.4F}) {
        light.radius = radius;
        const auto guide = editor::light_guide(light, true);
        bool source = false, cutoff = false;
        for (const auto& edge : guide.segments()) {
            const float expected = edge.style == editor::LightGuideStyle::Range ? 19.0F : radius;
            UI_CHECK(near(scene::length(edge.b - light.position), expected));
            source |= edge.style == editor::LightGuideStyle::Source;
            cutoff |= edge.style == editor::LightGuideStyle::Range;
        }
        UI_CHECK(source && cutoff);
    }
    const auto unselected = editor::light_guide(light, false);
    UI_CHECK(!unselected.segments().empty());
    for (const auto& edge : unselected.segments()) {
        UI_CHECK(edge.style == editor::LightGuideStyle::Source);
    }
    light.kind = scene::LocalLightKind::Point;
    const auto point = editor::light_guide(light, true);
    UI_CHECK(!point.segments().empty());
    for (const auto& edge : point.segments()) {
        UI_CHECK(edge.style == editor::LightGuideStyle::Range);
        UI_CHECK(near(scene::length(edge.a - light.position), 19.0F));
    }
}

void rectangle_guides_follow_roll_aim_and_emitting_faces() {
    scene::PointLight light;
    light.kind = scene::LocalLightKind::Rectangle;
    light.position = {2, 3, 4};
    light.direction = {0, -5, 0};
    light.width = 4;
    light.height = 2;
    for (const float roll : {0.0F, 0.25F}) {
        light.area_rotation = roll;
        const auto guide = editor::light_guide(light, false);
        std::size_t source_edges = 0;
        for (const auto& edge : guide.segments()) {
            if (edge.style != editor::LightGuideStyle::Source) continue;
            ++source_edges;
            UI_CHECK(near(edge.a.y, 3.0F));
            UI_CHECK(near(std::abs(edge.a.x - 2), roll == 0 ? 2.0F : 1.0F));
            UI_CHECK(near(std::abs(edge.a.z - 4), roll == 0 ? 1.0F : 2.0F));
        }
        UI_CHECK(source_edges == 4U);
    }
    light.direction = {0, 0, 3};
    for (const bool two_sided : {false, true}) {
        light.two_sided = two_sided;
        const auto guide = editor::light_guide(light, false);
        bool forward = false, backward = false;
        for (const auto& edge : guide.segments()) {
            if (edge.style == editor::LightGuideStyle::Source) {
                UI_CHECK(near(edge.a.z, 4));
            }
            if (edge.style == editor::LightGuideStyle::Direction && edge.a == light.position) {
                forward |= edge.b.z > 4;
                backward |= edge.b.z < 4;
            }
        }
        UI_CHECK(forward && backward == two_sided);
    }
}

void spot_guides_show_inner_outer_angles_and_radial_cutoff() {
    scene::PointLight light;
    light.kind = scene::LocalLightKind::Spot;
    light.position = {2, 3, 4};
    light.direction = {0, -7, 0};
    light.range = 12;
    light.inner_cone = 1.0F / 12.0F; // 30 degree half-angle.
    light.outer_cone = 1.0F / 6.0F; // 60 degree half-angle.
    const auto guide = editor::light_guide(light, true);
    bool inner = false, outer = false;
    for (const auto& edge : guide.segments()) {
        if (edge.style != editor::LightGuideStyle::InnerCone &&
            edge.style != editor::LightGuideStyle::OuterCone) continue;
        const auto delta = edge.b - light.position;
        UI_CHECK(near(scene::length(delta), 12));
        if (edge.style == editor::LightGuideStyle::OuterCone) {
            UI_CHECK(near(-delta.y, 6));
            UI_CHECK(near(std::sqrt(delta.x * delta.x + delta.z * delta.z), std::sqrt(108.0F)));
            outer = true;
        } else {
            UI_CHECK(near(-delta.y, std::sqrt(108.0F)));
            UI_CHECK(near(std::sqrt(delta.x * delta.x + delta.z * delta.z), 6));
            inner = true;
        }
    }
    UI_CHECK(inner && outer);
    light.inner_cone = light.outer_cone = 0.249F;
    const auto wide = editor::light_guide(light, true);
    for (const auto& edge : wide.segments()) {
        UI_CHECK(edge.style != editor::LightGuideStyle::InnerCone);
        UI_CHECK(std::isfinite(edge.b.x) && std::isfinite(edge.b.y) && std::isfinite(edge.b.z));
        UI_CHECK(scene::length(edge.b - light.position) < 12.01F);
    }
    const auto sun = editor::directional_light_guide({2, 3, 4}, {0, 0, -8});
    std::size_t rays = 0;
    for (const auto& edge : sun.segments()) {
        UI_CHECK(edge.style == editor::LightGuideStyle::Direction);
        if (near(scene::length(edge.b - edge.a), 1.4F)) {
            ++rays;
            UI_CHECK(near(edge.b.x, edge.a.x) && near(edge.b.y, edge.a.y));
            UI_CHECK(near(edge.b.z, edge.a.z - 1.4F));
        }
    }
    UI_CHECK(rays == 3U);
}

void light_guides_clip_crossing_edges_before_projection() {
    const auto view = make_scene();
    const auto matrices = scene::view_for_slice(view.camera, view.canvas, scene::whole(view.canvas));
    const auto crossing = editor::project_light_guide_segment(
        {{0, 0, 0}, {0, 0, 10}, editor::LightGuideStyle::Source}, matrices, view.canvas);
    UI_CHECK(crossing.has_value());
    UI_CHECK(near((*crossing)[1].x, 960) && near((*crossing)[1].y, 540));
    UI_CHECK(near((*crossing)[1].depth, 1.0F));
    UI_CHECK(!editor::project_light_guide_segment(
        {{0, 0, 9}, {0, 0, 12}, editor::LightGuideStyle::Source}, matrices, view.canvas));
    const auto spanning = editor::project_light_guide_segment(
        {{-1000, 0, 0}, {1000, 0, 0}, editor::LightGuideStyle::Range}, matrices, view.canvas);
    UI_CHECK(spanning.has_value());
    UI_CHECK(near((*spanning)[0].x, 0, 0.1F));
    UI_CHECK(near((*spanning)[1].x, 1920, 0.1F));
}

void light_guides_respect_stage_selection_and_guides_toggle() {
    Harness h;
    h.scene.media.clear();
    scene::PointLight light;
    light.source = doc::NodeId{52};
    light.kind = scene::LocalLightKind::Sphere;
    light.radius = 0.75F;
    light.range = 3;
    h.scene.point_lights.push_back(light);
    editor::ViewportPanel stage{gui::ui_id("stage.lights"), editor::ViewportRole::Stage};
    const std::array<doc::NodeId, 1U> selection{light.source};
    const auto cutoff_vertices = [&] {
        return std::count_if(h.list.vertices().begin(), h.list.vertices().end(), [](const auto& vertex) {
            return near(vertex.r, 0.2F) && near(vertex.g, 0.92F) && near(vertex.b, 1) && near(vertex.a, 0.42F);
        });
    };
    const auto draw = [&](bool selected, float x = 0, bool down = false) {
        h.input.new_frame();
        h.input.set_mouse_position(x, 14);
        h.input.set_mouse_button(gui::MouseButton::Left, down);
        h.ctx.new_frame();
        static_cast<void>(stage.draw(h.ctx, Harness::bounds, h.target, h.scene,
            selected ? std::span<const doc::NodeId>{selection} : std::span<const doc::NodeId>{}, light.source));
    };
    draw(false);
    UI_CHECK(cutoff_vertices() == 0);
    draw(true);
    UI_CHECK(cutoff_vertices() > 0);
    draw(true, 350, true);
    draw(true, 350, false);
    UI_CHECK(!stage.overlays_visible());
    UI_CHECK(cutoff_vertices() == 0);
    h.input.new_frame();
    h.ctx.new_frame();
    static_cast<void>(h.panel.draw(h.ctx, Harness::bounds, h.target, h.scene, selection, light.source));
    UI_CHECK(cutoff_vertices() == 0); // Program never includes light guides.
}

void preview_resolution_controls_preserve_logical_picking() {
    Harness h;
    editor::ViewportPanel stage{gui::ui_id("scaled.stage"), editor::ViewportRole::Stage};
    const auto draw = [&] {
        const auto result = stage.draw(h.ctx, Harness::bounds, h.target, h.scene);
        h.ctx.draw_overlays(Harness::bounds);
        return result;
    };
    const auto click = [&](float x, float y, bool down) {
        h.input.new_frame();
        h.input.set_mouse_position(x, y);
        h.input.set_mouse_button(gui::MouseButton::Left, down);
        h.ctx.new_frame();
        return draw();
    };
    const auto full = click(500.0F, 500.0F, false);
    UI_CHECK(stage.resolution() == editor::PreviewResolution::Full);
    static_cast<void>(click(865.0F, 14.0F, true));
    static_cast<void>(click(865.0F, 14.0F, false));
    h.input.new_frame(); h.input.add_key(gui::Key::Down); h.ctx.new_frame();
    static_cast<void>(draw());
    h.input.new_frame(); h.input.add_key(gui::Key::Enter); h.ctx.new_frame();
    UI_CHECK(!draw().pressed);
    UI_CHECK(stage.resolution() == editor::PreviewResolution::Half);
    UI_CHECK(h.panel.resolution() == editor::PreviewResolution::Full);
    for (const auto resolution : {editor::PreviewResolution::Half, editor::PreviewResolution::Quarter}) {
        stage.set_resolution(resolution);
        const auto frame = click(500.0F, 500.0F, true);
        UI_CHECK(near(frame.image.x, full.image.x));
        UI_CHECK(near(frame.image.y, full.image.y));
        UI_CHECK(near(frame.image.width, full.image.width));
        UI_CHECK(near(frame.image.height, full.image.height));
        UI_CHECK(near(frame.pointer.x, full.pointer.x));
        UI_CHECK(near(frame.pointer.y, full.pointer.y));
        UI_CHECK(frame.picked == clip_node);
        UI_CHECK(h.scene.canvas.width == 1920U && h.scene.canvas.height == 1080U);
        static_cast<void>(click(500.0F, 500.0F, false));
    }
}

void stage_orbit_preserves_authored_exposure() {
    scene::Camera authored{};
    authored.exposure_compensation_ev = -4.0F;
    const editor::StageOrbit orbit{{1.0F, 2.0F, 3.0F}, 0.0F, 0.0F, 9.0F};
    const auto stage = editor::stage_camera_for(authored, orbit);
    UI_CHECK(stage.eye == scene::Vec3{1.0F, 2.0F, 12.0F});
    UI_CHECK(stage.target == orbit.target);
    UI_CHECK(stage.field_of_view == 0.14F);
    UI_CHECK(near(scene::camera_exposure_multiplier(stage), 0.0625F, 0.00001F));
    UI_CHECK(authored.eye == scene::Vec3{});

    // Exposure changes on a subsequent evaluated frame must be inherited,
    // including all physical controls rather than compensation alone.
    authored.aperture_f_number = 8.0F;
    authored.shutter_seconds = 1.0F / 120.0F;
    authored.iso = 800.0F;
    authored.exposure_compensation_ev = 2.0F;
    const auto next = editor::stage_camera_for(authored, orbit);
    UI_CHECK(next.eye == stage.eye && next.target == stage.target);
    UI_CHECK(next.aperture_f_number == authored.aperture_f_number);
    UI_CHECK(next.shutter_seconds == authored.shutter_seconds);
    UI_CHECK(next.iso == authored.iso);
    UI_CHECK(next.exposure_compensation_ev == authored.exposure_compensation_ev);
    UI_CHECK(scene::camera_exposure_multiplier(next) == scene::camera_exposure_multiplier(authored));

    const auto moved = editor::stage_camera_for(authored,
        editor::StageOrbit{orbit.target, 0.25F, 0.0F, 5.0F});
    UI_CHECK(near(moved.eye.x, 6.0F) && near(moved.eye.y, 2.0F) && near(moved.eye.z, 3.0F));
    UI_CHECK(scene::camera_exposure_multiplier(moved) == scene::camera_exposure_multiplier(next));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 60U> cases{{
    {"a full scatter group has one bounded selection outline",
     &a_full_scatter_group_has_one_bounded_selection_outline},
    {"stage orbit preserves authored exposure", &stage_orbit_preserves_authored_exposure},
    {"preview resolution controls preserve logical picking", &preview_resolution_controls_preserve_logical_picking},
    {"light sources and cutoffs have authored world dimensions", &light_sources_and_cutoffs_have_authored_world_dimensions},
    {"rectangle guides follow roll aim and emitting faces", &rectangle_guides_follow_roll_aim_and_emitting_faces},
    {"spot guides show inner outer angles and radial cutoff", &spot_guides_show_inner_outer_angles_and_radial_cutoff},
    {"light guides clip crossing edges before projection", &light_guides_clip_crossing_edges_before_projection},
    {"light guides respect Stage selection and Guides toggle", &light_guides_respect_stage_selection_and_guides_toggle},
    {"the render is letterboxed to the canvas aspect",
     &the_render_is_letterboxed_to_the_canvas_aspect},
    {"a pointer maps through the letterbox to canvas pixels",
     &a_pointer_maps_through_the_letterbox_to_canvas_pixels},
    {"clicking a clip picks the node that made it",
     &clicking_a_clip_picks_the_node_that_made_it},
    {"mapped Program pixels drive selection and direct manipulation",
     &mapped_program_pixels_drive_selection_and_direct_manipulation},
    {"clicking bare canvas picks nothing", &clicking_bare_canvas_picks_nothing},
    {"Stage bare canvas explicitly reserves the press for navigation",
     &stage_bare_canvas_explicitly_reserves_the_press_for_navigation},
    {"Stage camera chords never pick what is underneath",
     &stage_camera_chords_never_pick_what_is_underneath},
    {"world bounds cover the selection as picked",
     &world_bounds_cover_the_selection_as_picked},
    {"Stage navigation follows Unreal chords",
     &stage_navigation_follows_unreal_chords},
    {"a right drag looks in place and flies with WASD",
     &a_right_drag_looks_in_place_and_flies_with_wasd},
    {"orbit, pan, walk and dolly move the view as Unreal does",
     &orbit_pan_walk_and_dolly_move_the_view_as_unreal_does},
    {"F frames the whole box from the current direction",
     &f_frames_the_whole_box_from_the_current_direction},
    {"the front most clip wins an overlap",
     &the_front_most_clip_wins_an_overlap},
    {"a stationary long press offers every overlapping element",
     &a_stationary_long_press_offers_every_overlapping_element},
    {"moving during a long press keeps the normal drag",
     &moving_during_a_long_press_keeps_the_normal_drag},
    {"a long press over the selected gizmo still offers depths",
     &a_long_press_over_the_selected_gizmo_still_offers_depths},
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
    {"a rotated Stage gizmo moves along the displayed local axis",
     &a_rotated_stage_gizmo_moves_along_the_displayed_local_axis},
    {"the move gizmo defaults to world axes like blender",
     &the_move_gizmo_defaults_to_world_axes_like_blender},
    {"a rotate ring turns about its axis as the pointer circles",
     &a_rotate_ring_turns_about_its_axis_as_the_pointer_circles},
    {"scale handles follow the pointer and the ring scales uniformly",
     &scale_handles_follow_the_pointer_and_the_ring_scales_uniformly},
    {"snapping rounds a gizmo drag and ctrl inverts it",
     &snapping_rounds_a_gizmo_drag_and_ctrl_inverts_it},
    {"g x and a typed distance move exactly and enter confirms",
     &g_x_and_a_typed_distance_move_exactly_and_enter_confirms},
    {"an axis key twice uses the elements own axis then frees it",
     &an_axis_key_twice_uses_the_elements_own_axis_then_frees_it},
    {"a right click cancels and its press is swallowed",
     &a_right_click_cancels_and_its_press_is_swallowed},
    {"a left click confirms without picking and s shift z scales a plane",
     &a_left_click_confirms_without_picking_and_s_shift_z_scales_a_plane},
    {"keyboard transforms start only over the image with a selection",
     &keyboard_transforms_start_only_over_the_image_with_a_selection},
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
    {"an explicit same-path surface refresh publishes a new wire",
     &an_explicit_same_path_surface_refresh_publishes_a_new_wire},
    {"mask mapping preserves source and authored canvas spaces",
     &mask_mapping_preserves_source_and_authored_canvas_spaces},
    {"mask mapping follows a layer seen in perspective",
     &mask_mapping_follows_a_layer_seen_in_perspective},
    {"mask overlays and selection stay in the mask domain",
     &mask_overlays_and_selection_stay_in_the_mask_domain},
    {"mask hit priority prefers selected handles then points then pen path",
     &mask_hit_priority_prefers_selected_handles_then_points_then_pen_path},
    {"mask point and handle modifiers are deterministic",
     &mask_point_and_handle_modifiers_are_deterministic},
    {"mask gestures commit once and cancel or noop inertly",
     &mask_gestures_commit_once_and_cancel_or_noop_inertly},
    {"primitive mask handles resize and rotate on canvas",
     &primitive_mask_handles_resize_and_rotate_on_canvas},
    {"program output authors masks against the connector image",
     &program_output_authors_masks_against_the_connector_image},
    {"the tracked camera and plate overlays appear only when live",
     &the_tracked_camera_and_plate_overlays_appear_only_when_live},
    {"coverage toggle is a typed target scoped intent",
     &coverage_toggle_is_a_typed_target_scoped_intent},
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
