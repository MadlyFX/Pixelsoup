#include <mediaserver/editor/canvas_tool.hpp>
#include <mediaserver/editor/fill_handles.hpp>
#include <mediaserver/editor/transform_box.hpp>
#include <mediaserver/editor/transform_tool.hpp>
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
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

// The Program view as a 2D canvas, with no GPU and no window: which tool a
// key chooses, where the picture lands once it is zoomed and panned, which
// handle of a layer's box a pointer takes, and what a drag of one resolves
// to. What a drag then does to a document is the shell's, tested there.
namespace {

namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;
namespace doc = mediaserver::doc;

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

// A camera eight units back from a two-unit square clip at the origin, over
// a 16:9 canvas: the clip lands square in the middle of the picture.
[[nodiscard]] scene::Scene make_scene() {
    scene::Scene out{};
    out.canvas = scene::Canvas{1920U, 1080U};
    out.camera.eye = scene::Vec3{0.0F, 0.0F, 8.0F};
    out.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    out.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    scene::MediaInstance clip{};
    clip.world = scene::scaling(scene::Vec3{2.0F, 2.0F, 1.0F});
    clip.source_node = clip_node;
    out.media.push_back(clip);
    return out;
}

struct Pointer final {
    float x{};
    float y{};
    bool left{};
    bool middle{};
    float wheel{};
    gui::KeyMods mods{gui::KeyMods::None};
    std::optional<gui::Key> key{};
};

struct Harness final {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    rhi::Texture target;
    editor::ViewportPanel panel{gui::ui_id("program"),
                                editor::ViewportRole::Program};
    scene::Scene scene{make_scene()};
    std::vector<doc::NodeId> selection{clip_node};

    // Square, so the 16:9 picture has bars above and below it.
    static constexpr gui::Rect bounds{0.0F, 0.0F, 1000.0F, 1000.0F};

    editor::ViewportFrame frame(const Pointer& pointer) {
        input.new_frame();
        input.set_mouse_position(pointer.x, pointer.y);
        input.set_mouse_button(gui::MouseButton::Left, pointer.left);
        input.set_mouse_button(gui::MouseButton::Middle, pointer.middle);
        input.set_mods(pointer.mods);
        if (pointer.wheel != 0.0F) {
            input.add_wheel(pointer.wheel);
        }
        if (pointer.key.has_value()) {
            input.add_key(*pointer.key);
        }
        ctx.new_frame();
        return panel.draw(ctx, bounds, target, scene, selection,
                          selection.empty() ? doc::NodeId{} : selection[0U]);
    }

    editor::ViewportFrame at(float x, float y, bool down = false,
                             gui::KeyMods mods = gui::KeyMods::None) {
        return frame(Pointer{x, y, down, false, 0.0F, mods});
    }

    [[nodiscard]] editor::CanvasBoxMetrics metrics() {
        const auto found = panel.canvas_box_metrics(scene, clip_node);
        if (!found.has_value()) {
            throw std::runtime_error{"the clip has no box"};
        }
        return *found;
    }
};

// A point of the box as it stood, a share of the way across and down it. The
// test clip faces the camera squarely, so its box is a plain rectangle.
[[nodiscard]] gui::UiPoint on_box(const editor::TransformBox& box, float u,
                                  float v) noexcept {
    const gui::UiPoint& a = box.corners[0U];
    const gui::UiPoint& b = box.corners[1U];
    const gui::UiPoint& d = box.corners[3U];
    return gui::UiPoint{a.x + (b.x - a.x) * u + (d.x - a.x) * v,
                        a.y + (b.y - a.y) * u + (d.y - a.y) * v};
}

void each_tool_has_its_own_key_and_other_keys_choose_none() {
    UI_CHECK(editor::canvas_tool_for_shortcut("v") ==
             editor::CanvasTool::Select);
    UI_CHECK(editor::canvas_tool_for_shortcut("H") ==
             editor::CanvasTool::Hand);
    UI_CHECK(editor::canvas_tool_for_shortcut("z") ==
             editor::CanvasTool::Zoom);
    UI_CHECK(editor::canvas_tool_for_shortcut("r") ==
             editor::CanvasTool::Rectangle);
    UI_CHECK(editor::canvas_tool_for_shortcut("E") ==
             editor::CanvasTool::Ellipse);
    UI_CHECK(editor::canvas_tool_for_shortcut("p") ==
             editor::CanvasTool::Pen);
    UI_CHECK(editor::canvas_tool_for_shortcut("g") ==
             editor::CanvasTool::Gradient);
    UI_CHECK(!editor::draws_shape(editor::CanvasTool::Gradient));
    UI_CHECK(editor::draws_shape(editor::CanvasTool::Pen) &&
             editor::draws_shape(editor::CanvasTool::Ellipse) &&
             !editor::draws_shape(editor::CanvasTool::Select) &&
             !editor::draws_shape(editor::CanvasTool::Hand));
    UI_CHECK(!editor::canvas_tool_for_shortcut("q").has_value());
    UI_CHECK(!editor::canvas_tool_for_shortcut("").has_value());
    // No two tools share a key, and every tool names itself.
    const auto tools = editor::canvas_tools();
    for (std::size_t a = 0U; a < tools.size(); ++a) {
        UI_CHECK(!tools[a].name.empty() && !tools[a].hint.empty());
        UI_CHECK(&editor::canvas_tool(tools[a].tool) == &tools[a]);
        for (std::size_t b = a + 1U; b < tools.size(); ++b) {
            UI_CHECK(tools[a].shortcut != tools[b].shortcut);
            UI_CHECK(tools[a].tool != tools[b].tool);
        }
    }
    // The tools that move the view change nothing; the rest author.
    for (const editor::CanvasToolDescriptor& tool : tools) {
        const bool view_only = tool.tool == editor::CanvasTool::Hand ||
                               tool.tool == editor::CanvasTool::Zoom;
        UI_CHECK(tool.authors == !view_only);
    }
}

void the_palette_chooses_the_key_clicked_and_a_lock_refuses_select() {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    const gui::Rect palette{0.0F, 0.0F, editor::canvas_tool_palette_width,
                            400.0F};
    // Keys are 28 tall, 4 apart, from 4 below the top: Select, Hand, Zoom.
    const auto click = [&](float y, bool locked) {
        std::optional<editor::CanvasTool> chosen;
        for (const bool down : {false, true, false}) {
            input.new_frame();
            input.set_mouse_position(18.0F, y);
            input.set_mouse_button(gui::MouseButton::Left, down);
            ctx.new_frame();
            const auto result = editor::draw_canvas_tool_palette(
                ctx, gui::ui_id("palette"), palette,
                editor::CanvasTool::Select, locked);
            if (result.has_value()) {
                chosen = result;
            }
        }
        return chosen;
    };
    UI_CHECK(click(18.0F, false) == editor::CanvasTool::Select);
    UI_CHECK(click(50.0F, false) == editor::CanvasTool::Hand);
    UI_CHECK(click(82.0F, false) == editor::CanvasTool::Zoom);
    UI_CHECK(click(114.0F, false) == editor::CanvasTool::Rectangle);
    UI_CHECK(click(146.0F, false) == editor::CanvasTool::Ellipse);
    UI_CHECK(click(178.0F, false) == editor::CanvasTool::Pen);
    UI_CHECK(click(210.0F, false) == editor::CanvasTool::Gradient);
    UI_CHECK(!click(300.0F, false).has_value());
    // Locked, the tool that authors refuses; the ones that move the view
    // still answer.
    UI_CHECK(!click(18.0F, true).has_value());
    UI_CHECK(!click(146.0F, true).has_value());
    UI_CHECK(click(50.0F, true) == editor::CanvasTool::Hand);
}

void a_box_finds_its_handles_and_drops_the_edge_ones_when_small() {
    const std::array<gui::UiPoint, 4U> corners{
        gui::UiPoint{100.0F, 100.0F}, gui::UiPoint{300.0F, 100.0F},
        gui::UiPoint{300.0F, 200.0F}, gui::UiPoint{100.0F, 200.0F}};
    const editor::TransformBox box = editor::transform_box(corners, true);
    UI_CHECK(box.visible);
    UI_CHECK(near(box.centre.x, 200.0F) && near(box.centre.y, 150.0F));
    // The turn handle stands straight above the top edge's middle.
    UI_CHECK(near(box.rotate.x, 200.0F));
    UI_CHECK(near(box.rotate.y,
                  100.0F - editor::transform_box_stalk_pixels));

    using Handle = editor::TransformBoxHandle;
    UI_CHECK(editor::hit_transform_box(box, 101.0F, 99.0F) ==
             Handle::TopLeft);
    UI_CHECK(editor::hit_transform_box(box, 300.0F, 203.0F) ==
             Handle::BottomRight);
    UI_CHECK(editor::hit_transform_box(box, 200.0F, 200.0F) ==
             Handle::Bottom);
    UI_CHECK(editor::hit_transform_box(box, 100.0F, 150.0F) == Handle::Left);
    UI_CHECK(editor::hit_transform_box(box, box.rotate.x, box.rotate.y) ==
             Handle::Rotate);
    // The middle of the layer, and anywhere along an edge away from its
    // handle, is the layer's own: nothing of the box is hit.
    UI_CHECK(editor::hit_transform_box(box, 200.0F, 150.0F) == Handle::None);
    UI_CHECK(editor::hit_transform_box(box, 250.0F, 100.0F) == Handle::None);

    // Across the box from each handle is the one a resize holds still.
    UI_CHECK(editor::opposite(Handle::TopLeft) == Handle::BottomRight);
    UI_CHECK(editor::opposite(Handle::Right) == Handle::Left);
    UI_CHECK(editor::handle_point(Handle::TopRight) ==
             editor::BoxPoint{1.0F, 0.0F});
    UI_CHECK(editor::handle_point(Handle::Bottom) ==
             editor::BoxPoint{0.5F, 1.0F});

    // A side handle pulls along the screen axis it sits on.
    UI_CHECK(editor::handle_cursor(box, Handle::Right) ==
             gui::Cursor::ResizeHorizontal);
    UI_CHECK(editor::handle_cursor(box, Handle::Top) ==
             gui::Cursor::ResizeVertical);

    // Twenty pixels tall: the side handles would sit on the corners, so
    // only the top and bottom ones remain between them.
    const editor::TransformBox thin = editor::transform_box(
        {gui::UiPoint{100.0F, 100.0F}, gui::UiPoint{300.0F, 100.0F},
         gui::UiPoint{300.0F, 120.0F}, gui::UiPoint{100.0F, 120.0F}},
        true);
    UI_CHECK(editor::hit_transform_box(thin, 100.0F, 110.0F) == Handle::None);
    UI_CHECK(editor::hit_transform_box(thin, 200.0F, 120.0F) ==
             Handle::Bottom);

    // A box that only bounds something has corners and no sides.
    const editor::TransformBox bounding =
        editor::transform_box(corners, false);
    UI_CHECK(editor::hit_transform_box(bounding, 200.0F, 200.0F) ==
             Handle::None);
    UI_CHECK(editor::hit_transform_box(bounding, 300.0F, 200.0F) ==
             Handle::BottomRight);

    // No area on screen: nothing to draw or hit.
    const editor::TransformBox flat = editor::transform_box(
        {gui::UiPoint{100.0F, 100.0F}, gui::UiPoint{300.0F, 100.0F},
         gui::UiPoint{300.0F, 100.0F}, gui::UiPoint{100.0F, 100.0F}},
        true);
    UI_CHECK(!flat.visible);
    UI_CHECK(editor::hit_transform_box(flat, 100.0F, 100.0F) == Handle::None);
}

void a_resize_stretches_from_its_anchor_and_stops_short_of_inverting() {
    using Handle = editor::TransformBoxHandle;
    // The bottom-right corner pulled half a box right and a quarter down,
    // against the top-left corner.
    const editor::BoxScale corner = editor::resolve_box_scale(
        Handle::BottomRight, editor::BoxPoint{0.0F, 0.0F},
        editor::BoxPoint{0.5F, 0.25F}, false);
    UI_CHECK(near(corner.across, 1.5F) && near(corner.down, 1.25F));
    // The same pull against the middle counts double.
    const editor::BoxScale centred = editor::resolve_box_scale(
        Handle::BottomRight, editor::BoxPoint{0.5F, 0.5F},
        editor::BoxPoint{0.5F, 0.25F}, false);
    UI_CHECK(near(centred.across, 2.0F) && near(centred.down, 1.5F));
    // Kept in proportion, both sides take the larger change.
    const editor::BoxScale uniform = editor::resolve_box_scale(
        Handle::BottomRight, editor::BoxPoint{0.0F, 0.0F},
        editor::BoxPoint{0.5F, 0.25F}, true);
    UI_CHECK(near(uniform.across, 1.5F) && near(uniform.down, 1.5F));
    // The top-left corner pulled the same way shrinks the box instead.
    const editor::BoxScale shrink = editor::resolve_box_scale(
        Handle::TopLeft, editor::BoxPoint{1.0F, 1.0F},
        editor::BoxPoint{0.5F, 0.25F}, false);
    UI_CHECK(near(shrink.across, 0.5F) && near(shrink.down, 0.75F));
    // A side handle stretches one way however the pointer wanders.
    const editor::BoxScale side = editor::resolve_box_scale(
        Handle::Right, editor::BoxPoint{0.0F, 0.5F},
        editor::BoxPoint{0.3F, 0.9F}, true);
    UI_CHECK(near(side.across, 1.3F) && near(side.down, 1.0F));
    // Dragged past its anchor, a side stops at a sliver.
    const editor::BoxScale through = editor::resolve_box_scale(
        Handle::Right, editor::BoxPoint{0.0F, 0.5F},
        editor::BoxPoint{-3.0F, 0.0F}, false);
    UI_CHECK(near(through.across, editor::transform_box_minimum_scale,
                  1.0e-5F));
    // The turn handle resizes nothing.
    UI_CHECK(editor::resolve_box_scale(Handle::Rotate, {}, {2.0F, 2.0F},
                                       false) == editor::BoxScale{});
}

void the_selected_layer_has_a_box_on_program_and_none_elsewhere() {
    Harness h;
    const editor::ViewportFrame shown = h.at(10.0F, 10.0F);
    UI_CHECK(shown.box.visible && shown.box.sides);
    // The picture is 1000 wide in the middle of the square panel; the clip
    // is a square about its centre.
    UI_CHECK(near(shown.box.centre.x, 500.0F, 0.05F));
    UI_CHECK(near(shown.box.centre.y, 500.0F, 0.05F));
    const float width = shown.box.corners[1U].x - shown.box.corners[0U].x;
    const float height = shown.box.corners[3U].y - shown.box.corners[0U].y;
    UI_CHECK(width > 50.0F && near(width, height, 0.05F));
    // Top-left first: left of and above the middle.
    UI_CHECK(shown.box.corners[0U].x < 500.0F &&
             shown.box.corners[0U].y < 500.0F);

    // The options bar reads the same box in canvas pixels.
    const editor::CanvasBoxMetrics metrics = h.metrics();
    UI_CHECK(metrics.sides);
    UI_CHECK(near(metrics.x, 960.0F, 0.05F) && near(metrics.y, 540.0F, 0.05F));
    UI_CHECK(near(metrics.width, width * 1.92F, 0.1F));
    UI_CHECK(near(metrics.height, metrics.width, 0.05F));
    UI_CHECK(near(metrics.turns, 0.0F, 1.0e-4F));

    // Nothing selected: no box.
    h.selection.clear();
    UI_CHECK(!h.at(10.0F, 10.0F).box.visible);
    h.selection = {clip_node};

    // Another tool, a view that may not author, or the Stage: no box.
    h.panel.set_canvas_tool(editor::CanvasTool::Hand);
    UI_CHECK(!h.at(10.0F, 10.0F).box.visible);
    h.panel.set_canvas_tool(editor::CanvasTool::Select);
    h.panel.set_box_enabled(false);
    UI_CHECK(!h.at(10.0F, 10.0F).box.visible);
    h.panel.set_box_enabled(true);
    UI_CHECK(h.at(10.0F, 10.0F).box.visible);

    editor::ViewportPanel stage{gui::ui_id("stage"),
                                editor::ViewportRole::Stage};
    h.input.new_frame();
    h.ctx.new_frame();
    const editor::ViewportFrame on_stage = stage.draw(
        h.ctx, Harness::bounds, h.target, h.scene, h.selection, clip_node);
    UI_CHECK(!on_stage.box.visible);
}

void a_corner_drag_resizes_against_the_opposite_corner() {
    Harness h;
    const editor::TransformBox box = h.at(10.0F, 10.0F).box;
    const gui::UiPoint corner = on_box(box, 1.0F, 1.0F);
    const editor::ViewportFrame hover = h.at(corner.x, corner.y);
    UI_CHECK(hover.box_handle == editor::TransformBoxHandle::BottomRight);

    const editor::ViewportFrame pressed = h.at(corner.x, corner.y, true);
    UI_CHECK(pressed.pressed);
    UI_CHECK(pressed.picked == clip_node);
    UI_CHECK(!pressed.transform.has_value());

    // Half a box further right, a quarter further down.
    const gui::UiPoint to = on_box(box, 1.5F, 1.25F);
    const editor::ViewportFrame dragged = h.at(to.x, to.y, true);
    UI_CHECK(dragged.dragging && dragged.transform.has_value());
    const editor::TransformDelta& delta = *dragged.transform;
    UI_CHECK(delta.tool == editor::TransformTool::Scale);
    UI_CHECK(near(delta.factors.x, 1.5F, 0.005F));
    UI_CHECK(near(delta.factors.y, 1.25F, 0.005F));
    UI_CHECK(near(delta.factors.z, 1.0F));
    // The clip's own top-left corner in the world stays where it is.
    UI_CHECK(near(delta.pivot.x, -1.0F, 0.001F));
    UI_CHECK(near(delta.pivot.y, 1.0F, 0.001F));
    UI_CHECK(near(delta.pivot.z, 0.0F, 0.001F));
    // Across is the clip's X, down its Y, whichever way each points.
    UI_CHECK(near(std::fabs(delta.basis[0U].x), 1.0F));
    UI_CHECK(near(std::fabs(delta.basis[1U].y), 1.0F));

    // Alt resizes about the middle; Shift keeps the proportions.
    const editor::ViewportFrame centred =
        h.at(to.x, to.y, true, gui::KeyMods::Alt);
    UI_CHECK(near(centred.transform->factors.x, 2.0F, 0.01F));
    UI_CHECK(near(centred.transform->factors.y, 1.5F, 0.01F));
    UI_CHECK(near(centred.transform->pivot.x, 0.0F, 0.001F));
    UI_CHECK(near(centred.transform->pivot.y, 0.0F, 0.001F));
    const editor::ViewportFrame uniform =
        h.at(to.x, to.y, true, gui::KeyMods::Shift);
    UI_CHECK(near(uniform.transform->factors.x, 1.5F, 0.005F));
    UI_CHECK(near(uniform.transform->factors.y, 1.5F, 0.005F));

    const editor::ViewportFrame released = h.at(to.x, to.y, false);
    UI_CHECK(released.released);
    UI_CHECK(!h.panel.dragging());
    // Let go, the same pointer move resizes nothing.
    UI_CHECK(!h.at(to.x + 40.0F, to.y, false).transform.has_value());
}

void a_side_handle_stretches_one_way_against_the_far_side() {
    Harness h;
    const editor::TransformBox box = h.at(10.0F, 10.0F).box;
    const gui::UiPoint right = on_box(box, 1.0F, 0.5F);
    UI_CHECK(h.at(right.x, right.y, true).picked == clip_node);
    // Out by three tenths of the box, and well down it: only out counts.
    const gui::UiPoint to = on_box(box, 1.3F, 0.9F);
    const editor::ViewportFrame dragged = h.at(to.x, to.y, true);
    UI_CHECK(dragged.transform.has_value());
    UI_CHECK(near(dragged.transform->factors.x, 1.3F, 0.005F));
    UI_CHECK(near(dragged.transform->factors.y, 1.0F));
    // Held still: the middle of the clip's left side.
    UI_CHECK(near(dragged.transform->pivot.x, -1.0F, 0.001F));
    UI_CHECK(near(dragged.transform->pivot.y, 0.0F, 0.001F));
}

void the_turn_handle_turns_the_layer_the_way_the_pointer_goes() {
    Harness h;
    const editor::TransformBox box = h.at(10.0F, 10.0F).box;
    const float radius = box.centre.y - box.rotate.y;
    UI_CHECK(radius > 20.0F);
    UI_CHECK(h.at(box.rotate.x, box.rotate.y).box_handle ==
             editor::TransformBoxHandle::Rotate);
    UI_CHECK(h.at(box.rotate.x, box.rotate.y, true).picked == clip_node);

    // From straight above the middle round to its right: a quarter turn
    // clockwise as seen, in two steps.
    const float diagonal = radius * 0.70710678F;
    static_cast<void>(h.at(box.centre.x + diagonal, box.centre.y - diagonal,
                           true));
    const editor::ViewportFrame turned =
        h.at(box.centre.x + radius, box.centre.y, true);
    UI_CHECK(turned.transform.has_value());
    const editor::TransformDelta delta = *turned.transform;
    UI_CHECK(delta.tool == editor::TransformTool::Rotate);
    UI_CHECK(near(std::fabs(delta.turns), 0.25F, 0.002F));
    // About the clip's own face, through its middle.
    UI_CHECK(near(std::fabs(delta.axis.z), 1.0F));
    UI_CHECK(near(delta.pivot.x, 0.0F, 0.001F) &&
             near(delta.pivot.y, 0.0F, 0.001F));

    // Applied, the layer reads a quarter turn clockwise: the top edge now
    // runs down the picture.
    h.scene.media[0U].world =
        editor::transformed_world(h.scene.media[0U].world, delta);
    UI_CHECK(near(h.metrics().turns, 0.25F, 0.002F));

    // Shift lands on fifteen-degree steps.
    Harness snap;
    const editor::TransformBox other = snap.at(10.0F, 10.0F).box;
    static_cast<void>(snap.at(other.rotate.x, other.rotate.y, true));
    const float reach = other.centre.y - other.rotate.y;
    // About twenty degrees round.
    const editor::ViewportFrame stepped = snap.at(
        other.centre.x + reach * 0.34202F, other.centre.y - reach * 0.93969F,
        true, gui::KeyMods::Shift);
    UI_CHECK(stepped.transform.has_value());
    UI_CHECK(near(std::fabs(stepped.transform->turns), 15.0F / 360.0F,
                  1.0e-4F));
}

void typed_values_resolve_to_the_gesture_that_reaches_them() {
    // Each field's edit, applied to the clip, reads back as the value asked
    // for and leaves the others as they were.
    {
        Harness h;
        const editor::CanvasBoxMetrics before = h.metrics();
        const auto move = h.panel.canvas_box_edit(
            h.scene, clip_node, nullptr, editor::CanvasBoxField::X, 1234.5F);
        UI_CHECK(move.has_value() &&
                 move->tool == editor::TransformTool::Move);
        h.scene.media[0U].world = scene::multiply(
            scene::translation(move->translation), h.scene.media[0U].world);
        const editor::CanvasBoxMetrics after = h.metrics();
        UI_CHECK(near(after.x, 1234.5F, 0.05F));
        UI_CHECK(near(after.y, before.y, 0.05F));
        UI_CHECK(near(after.width, before.width, 0.05F));
    }
    {
        Harness h;
        const auto move = h.panel.canvas_box_edit(
            h.scene, clip_node, nullptr, editor::CanvasBoxField::Y, 211.0F);
        UI_CHECK(move.has_value());
        h.scene.media[0U].world = scene::multiply(
            scene::translation(move->translation), h.scene.media[0U].world);
        UI_CHECK(near(h.metrics().y, 211.0F, 0.05F));
        UI_CHECK(near(h.metrics().x, 960.0F, 0.05F));
    }
    {
        Harness h;
        const editor::CanvasBoxMetrics before = h.metrics();
        const auto widen = h.panel.canvas_box_edit(
            h.scene, clip_node, nullptr, editor::CanvasBoxField::Width,
            before.width * 1.75F);
        UI_CHECK(widen.has_value() &&
                 widen->tool == editor::TransformTool::Scale);
        h.scene.media[0U].world =
            editor::transformed_world(h.scene.media[0U].world, *widen);
        const editor::CanvasBoxMetrics after = h.metrics();
        UI_CHECK(near(after.width, before.width * 1.75F, 0.05F));
        UI_CHECK(near(after.height, before.height, 0.05F));
        UI_CHECK(near(after.x, before.x, 0.05F));
        // A size of nothing is refused rather than collapsing the layer.
        UI_CHECK(!h.panel
                      .canvas_box_edit(h.scene, clip_node, nullptr,
                                       editor::CanvasBoxField::Height, 0.0F)
                      .has_value());
    }
    {
        Harness h;
        const auto turn = h.panel.canvas_box_edit(
            h.scene, clip_node, nullptr, editor::CanvasBoxField::Rotation,
            0.125F);
        UI_CHECK(turn.has_value() &&
                 turn->tool == editor::TransformTool::Rotate);
        h.scene.media[0U].world =
            editor::transformed_world(h.scene.media[0U].world, *turn);
        UI_CHECK(near(h.metrics().turns, 0.125F, 0.001F));
        UI_CHECK(near(h.metrics().x, 960.0F, 0.05F));
    }
    // An element that is not in the scene has nothing to edit.
    Harness h;
    UI_CHECK(!h.panel.canvas_box_metrics(h.scene, doc::NodeId{99U})
                  .has_value());
    UI_CHECK(!h.panel
                  .canvas_box_edit(h.scene, doc::NodeId{99U}, nullptr,
                                   editor::CanvasBoxField::X, 10.0F)
                  .has_value());
}

void the_wheel_zooms_about_the_pointer_and_fit_puts_it_back() {
    Harness h;
    const editor::ViewportFrame fitted = h.at(600.0F, 450.0F);
    UI_CHECK(h.panel.canvas_fitted());
    // Letterboxed exactly as before: 1000 wide, 562.5 tall, centred.
    UI_CHECK(near(fitted.image.x, 0.0F) && near(fitted.image.y, 218.75F));
    UI_CHECK(near(fitted.image.width, 1000.0F) &&
             near(fitted.image.height, 562.5F));
    UI_CHECK(near(h.panel.canvas_scale(), 1000.0F / 1920.0F, 1.0e-4F));
    const editor::CanvasPoint under = fitted.pointer;

    // Two notches in: larger by 1.2 each, and the canvas pixel under the
    // pointer has not moved.
    const editor::ViewportFrame zoomed =
        h.frame(Pointer{600.0F, 450.0F, false, false, 2.0F});
    UI_CHECK(zoomed.navigated);
    UI_CHECK(!h.panel.canvas_fitted());
    UI_CHECK(near(zoomed.image.width, 1440.0F, 0.05F));
    UI_CHECK(near(zoomed.image.height, 810.0F, 0.05F));
    UI_CHECK(near(zoomed.pointer.x, under.x, 0.05F));
    UI_CHECK(near(zoomed.pointer.y, under.y, 0.05F));
    // The box follows the picture: still round the clip, now larger.
    UI_CHECK(near(zoomed.box.corners[1U].x - zoomed.box.corners[0U].x,
                  (fitted.box.corners[1U].x - fitted.box.corners[0U].x) *
                      1.44F,
                  0.05F));

    // Out never goes below a quarter of the fitted size.
    editor::ViewportFrame small{};
    for (int notch = 0; notch < 30; ++notch) {
        small = h.frame(Pointer{600.0F, 450.0F, false, false, -1.0F});
    }
    UI_CHECK(near(small.image.width, 250.0F, 0.05F));

    // A chosen scale is panel units per canvas pixel, about the middle.
    h.panel.set_canvas_scale(1.0F);
    const editor::ViewportFrame actual = h.at(600.0F, 450.0F);
    UI_CHECK(near(actual.image.width, 1920.0F, 0.05F));
    UI_CHECK(near(h.panel.canvas_scale(), 1.0F, 1.0e-4F));

    h.panel.fit_canvas();
    const editor::ViewportFrame again = h.at(600.0F, 450.0F);
    UI_CHECK(h.panel.canvas_fitted());
    UI_CHECK(again.image.x == fitted.image.x &&
             again.image.y == fitted.image.y &&
             again.image.width == fitted.image.width &&
             again.image.height == fitted.image.height);
}

void a_middle_drag_pans_with_any_tool_and_keeps_the_picture_in_reach() {
    Harness h;
    const gui::Rect before = h.at(500.0F, 500.0F).image;
    static_cast<void>(
        h.frame(Pointer{500.0F, 500.0F, false, true, 0.0F}));
    const editor::ViewportFrame panned =
        h.frame(Pointer{560.0F, 470.0F, false, true, 0.0F});
    UI_CHECK(panned.navigated);
    UI_CHECK(near(panned.image.x, before.x + 60.0F, 0.01F));
    UI_CHECK(near(panned.image.y, before.y - 30.0F, 0.01F));
    UI_CHECK(near(panned.image.width, before.width));
    // Nothing was picked or pressed by it.
    UI_CHECK(!panned.pressed && !panned.picked.has_value());

    // Let go, the pointer no longer carries the picture.
    const gui::Rect rest =
        h.frame(Pointer{700.0F, 700.0F, false, false, 0.0F}).image;
    UI_CHECK(near(rest.x, before.x + 60.0F, 0.01F));

    // Dragged far off, some of the picture stays in the panel.
    static_cast<void>(
        h.frame(Pointer{500.0F, 500.0F, false, true, 0.0F}));
    const gui::Rect far =
        h.frame(Pointer{9000.0F, 9000.0F, false, true, 0.0F}).image;
    UI_CHECK(far.x < Harness::bounds.right());
    UI_CHECK(far.y < Harness::bounds.bottom());
}

void the_hand_tool_pans_with_the_left_button_and_picks_nothing() {
    Harness h;
    h.panel.set_canvas_tool(editor::CanvasTool::Hand);
    const gui::Rect before = h.at(500.0F, 500.0F).image;
    const editor::ViewportFrame pressed = h.at(500.0F, 500.0F, true);
    // Over the clip, and still no pick.
    UI_CHECK(!pressed.pressed && !pressed.picked.has_value());
    const editor::ViewportFrame dragged = h.at(420.0F, 540.0F, true);
    UI_CHECK(dragged.navigated);
    UI_CHECK(!dragged.transform.has_value() && !h.panel.dragging());
    UI_CHECK(near(dragged.image.x, before.x - 80.0F, 0.01F));
    UI_CHECK(near(dragged.image.y, before.y + 40.0F, 0.01F));
}

void the_zoom_tool_doubles_on_a_click_and_alt_halves() {
    Harness h;
    h.panel.set_canvas_tool(editor::CanvasTool::Zoom);
    const editor::ViewportFrame before = h.at(300.0F, 400.0F);
    static_cast<void>(h.at(300.0F, 400.0F, true));
    const editor::ViewportFrame clicked = h.at(300.0F, 400.0F, false);
    UI_CHECK(near(clicked.image.width, before.image.width * 2.0F, 0.05F));
    UI_CHECK(near(clicked.pointer.x, before.pointer.x, 0.05F));
    UI_CHECK(near(clicked.pointer.y, before.pointer.y, 0.05F));
    UI_CHECK(!clicked.picked.has_value());

    static_cast<void>(h.at(300.0F, 400.0F, true, gui::KeyMods::Alt));
    const editor::ViewportFrame back =
        h.at(300.0F, 400.0F, false, gui::KeyMods::Alt);
    UI_CHECK(near(back.image.width, before.image.width, 0.05F));
}

void a_handle_in_the_bars_is_taken_and_a_bare_press_there_is_not() {
    Harness h;
    // Made tall enough that its box runs a hundred units past the picture's
    // top edge, into the bar above it.
    const editor::ViewportFrame first = h.at(10.0F, 10.0F);
    const float units_per_world =
        (first.box.corners[3U].y - first.box.corners[0U].y) / 2.0F;
    const float tall =
        (first.image.height * 0.5F + 100.0F) * 2.0F / units_per_world;
    h.scene.media[0U].world = scene::scaling(scene::Vec3{2.0F, tall, 1.0F});
    const editor::ViewportFrame shown = h.at(10.0F, 10.0F);
    const gui::UiPoint top = on_box(shown.box, 0.5F, 0.0F);
    UI_CHECK(top.y < shown.image.y);
    UI_CHECK(top.y > Harness::bounds.y);
    const editor::ViewportFrame pressed = h.at(top.x, top.y, true);
    UI_CHECK(pressed.pressed);
    UI_CHECK(pressed.picked == clip_node);
    UI_CHECK(pressed.box_handle == editor::TransformBoxHandle::Top);
    static_cast<void>(h.at(top.x, top.y, false));

    // Elsewhere in the bar, a press starts nothing.
    const editor::ViewportFrame bare = h.at(900.0F, 30.0F, true);
    UI_CHECK(!bare.pressed && !bare.picked.has_value());
    UI_CHECK(!h.panel.dragging());
}

void a_drag_on_the_layer_itself_still_moves_it() {
    Harness h;
    static_cast<void>(h.at(500.0F, 500.0F));
    const editor::ViewportFrame pressed = h.at(500.0F, 500.0F, true);
    UI_CHECK(pressed.picked == clip_node);
    UI_CHECK(pressed.box_handle == editor::TransformBoxHandle::None);
    const editor::ViewportFrame dragged = h.at(540.0F, 500.0F, true);
    UI_CHECK(dragged.transform.has_value());
    UI_CHECK(dragged.transform->tool == editor::TransformTool::Move);
    UI_CHECK(dragged.transform->translation.x > 0.0F);
    UI_CHECK(near(dragged.transform->translation.y, 0.0F, 1.0e-4F));
}

void a_shape_dragged_on_the_selected_layer_is_in_the_layers_own_space() {
    Harness h;
    h.panel.set_canvas_tool(editor::CanvasTool::Rectangle);
    const editor::ViewportFrame shown = h.at(10.0F, 10.0F);
    // The layer's outline is known to the tool, which offers no handles.
    UI_CHECK(shown.box.visible);
    UI_CHECK(shown.box_handle == editor::TransformBoxHandle::None);
    const gui::UiPoint from = on_box(shown.box, 0.2F, 0.3F);
    const gui::UiPoint to = on_box(shown.box, 0.7F, 0.8F);
    const editor::ViewportFrame pressed = h.at(from.x, from.y, true);
    // The press is the tool's: nothing is picked or moved by it.
    UI_CHECK(!pressed.pressed && !pressed.picked.has_value());
    const editor::ViewportFrame dragging = h.at(to.x, to.y, true);
    UI_CHECK(dragging.shape_drawing && !dragging.shape.has_value());
    UI_CHECK(!dragging.transform.has_value() && !h.panel.dragging());
    UI_CHECK(h.panel.shape_in_progress());
    const editor::ViewportFrame released = h.at(to.x, to.y, false);
    UI_CHECK(released.shape.has_value());
    const editor::CanvasShape& shape = *released.shape;
    UI_CHECK(shape.tool == editor::CanvasTool::Rectangle && shape.on_layer);
    UI_CHECK(near(shape.left, 0.2F, 0.002F) && near(shape.top, 0.3F, 0.002F));
    UI_CHECK(near(shape.right, 0.7F, 0.002F) &&
             near(shape.bottom, 0.8F, 0.002F));
    UI_CHECK(!h.panel.shape_in_progress());
    // Reported once.
    UI_CHECK(!h.at(to.x, to.y, false).shape.has_value());

    // Dragged up and to the left, it is the same box.
    h.panel.set_canvas_tool(editor::CanvasTool::Ellipse);
    static_cast<void>(h.at(to.x, to.y, true));
    static_cast<void>(h.at(from.x, from.y, true));
    const editor::ViewportFrame backwards = h.at(from.x, from.y, false);
    UI_CHECK(backwards.shape.has_value());
    UI_CHECK(backwards.shape->tool == editor::CanvasTool::Ellipse);
    UI_CHECK(near(backwards.shape->left, 0.2F, 0.002F) &&
             near(backwards.shape->bottom, 0.8F, 0.002F));
}

void a_shape_dragged_on_bare_picture_is_in_canvas_pixels() {
    Harness h;
    h.selection.clear();
    h.panel.set_canvas_tool(editor::CanvasTool::Ellipse);
    // The picture is 1000 wide for 1920 pixels, its top 218.75 down.
    static_cast<void>(h.at(100.0F, 300.0F, true));
    static_cast<void>(h.at(350.0F, 400.0F, true));
    const editor::ViewportFrame released = h.at(350.0F, 400.0F, false);
    UI_CHECK(released.shape.has_value());
    const editor::CanvasShape& shape = *released.shape;
    UI_CHECK(shape.tool == editor::CanvasTool::Ellipse && !shape.on_layer);
    UI_CHECK(near(shape.left, 192.0F, 0.05F));
    UI_CHECK(near(shape.top, (300.0F - 218.75F) * 1.92F, 0.05F));
    UI_CHECK(near(shape.right, 672.0F, 0.05F));
    UI_CHECK(near(shape.bottom, (400.0F - 218.75F) * 1.92F, 0.05F));

    // Shift keeps it as wide as it is tall, by the longer way dragged.
    static_cast<void>(h.at(100.0F, 300.0F, true));
    static_cast<void>(h.at(350.0F, 400.0F, true, gui::KeyMods::Shift));
    const editor::ViewportFrame square =
        h.at(350.0F, 400.0F, false, gui::KeyMods::Shift);
    UI_CHECK(square.shape.has_value());
    UI_CHECK(near(square.shape->right - square.shape->left, 480.0F, 0.05F));
    UI_CHECK(near(square.shape->bottom - square.shape->top, 480.0F, 0.05F));

    // Alt grows it from the press, which is then its middle.
    static_cast<void>(h.at(500.0F, 500.0F, true));
    static_cast<void>(h.at(550.0F, 520.0F, true, gui::KeyMods::Alt));
    const editor::ViewportFrame centred =
        h.at(550.0F, 520.0F, false, gui::KeyMods::Alt);
    UI_CHECK(centred.shape.has_value());
    UI_CHECK(near((centred.shape->left + centred.shape->right) * 0.5F, 960.0F,
                  0.05F));
    UI_CHECK(near(centred.shape->right - centred.shape->left, 192.0F, 0.05F));
    UI_CHECK(near(centred.shape->bottom - centred.shape->top, 76.8F, 0.05F));

    // A twitch is not a shape, and Esc drops one that is under way.
    static_cast<void>(h.at(500.0F, 500.0F, true));
    UI_CHECK(!h.at(502.0F, 501.0F, false).shape.has_value());
    static_cast<void>(h.at(500.0F, 500.0F, true));
    static_cast<void>(h.frame(Pointer{600.0F, 600.0F, true, false, 0.0F,
                                      gui::KeyMods::None, gui::Key::Escape}));
    UI_CHECK(!h.at(650.0F, 650.0F, false).shape.has_value());

    // Where a card may not be made, bare picture takes no shape at all.
    h.panel.set_card_drawing_enabled(false);
    static_cast<void>(h.at(100.0F, 300.0F, true));
    const editor::ViewportFrame refused = h.at(350.0F, 400.0F, true);
    UI_CHECK(!refused.shape_drawing);
    UI_CHECK(!h.at(350.0F, 400.0F, false).shape.has_value());
}

void the_pen_places_points_curves_them_and_closes_on_the_first() {
    Harness h;
    h.panel.set_canvas_tool(editor::CanvasTool::Pen);
    const editor::TransformBox box = h.at(10.0F, 10.0F).box;
    const auto click = [&](float u, float v) {
        const gui::UiPoint at = on_box(box, u, v);
        static_cast<void>(h.at(at.x, at.y, true));
        return h.at(at.x, at.y, false);
    };
    UI_CHECK(click(0.2F, 0.2F).shape_drawing);
    static_cast<void>(click(0.8F, 0.2F));
    // Two points cannot close: a click on the first only adds a third.
    UI_CHECK(h.panel.shape_in_progress());

    // The third is pressed and dragged on, which pulls its handles out: the
    // one ahead follows the pointer and the one behind mirrors it.
    const gui::UiPoint third = on_box(box, 0.5F, 0.8F);
    const gui::UiPoint pulled = on_box(box, 0.4F, 0.9F);
    static_cast<void>(h.at(third.x, third.y, true));
    static_cast<void>(h.at(pulled.x, pulled.y, true));
    UI_CHECK(!h.at(pulled.x, pulled.y, false).shape.has_value());

    // Back on the first point: the curve closes and is reported once.
    const gui::UiPoint first = on_box(box, 0.2F, 0.2F);
    const editor::ViewportFrame closed = h.at(first.x + 3.0F, first.y, true);
    UI_CHECK(closed.shape.has_value());
    const editor::CanvasShape& shape = *closed.shape;
    UI_CHECK(shape.tool == editor::CanvasTool::Pen && shape.on_layer);
    UI_CHECK(shape.points.size() == 3U);
    UI_CHECK(near(shape.points[0U].x, 0.2F, 0.002F) &&
             near(shape.points[0U].y, 0.2F, 0.002F));
    // A clicked point has no curve of its own: its handles sit on it.
    UI_CHECK(shape.points[1U].in_x == shape.points[1U].x &&
             shape.points[1U].out_y == shape.points[1U].y);
    UI_CHECK(near(shape.points[1U].x, 0.8F, 0.002F));
    UI_CHECK(near(shape.points[2U].x, 0.5F, 0.002F) &&
             near(shape.points[2U].y, 0.8F, 0.002F));
    UI_CHECK(near(shape.points[2U].out_x, 0.4F, 0.002F) &&
             near(shape.points[2U].out_y, 0.9F, 0.002F));
    UI_CHECK(near(shape.points[2U].in_x, 0.6F, 0.002F) &&
             near(shape.points[2U].in_y, 0.7F, 0.002F));
    UI_CHECK(!h.panel.shape_in_progress());
    static_cast<void>(h.at(first.x, first.y, false));

    // Backspace takes the last point back; Enter closes; Esc drops it all.
    static_cast<void>(click(0.1F, 0.1F));
    static_cast<void>(click(0.9F, 0.1F));
    static_cast<void>(click(0.9F, 0.9F));
    static_cast<void>(click(0.1F, 0.9F));
    static_cast<void>(h.frame(Pointer{500.0F, 500.0F, false, false, 0.0F,
                                      gui::KeyMods::None,
                                      gui::Key::Backspace}));
    const editor::ViewportFrame entered =
        h.frame(Pointer{500.0F, 500.0F, false, false, 0.0F,
                        gui::KeyMods::None, gui::Key::Enter});
    UI_CHECK(entered.shape.has_value());
    UI_CHECK(entered.shape->points.size() == 3U);
    UI_CHECK(near(entered.shape->points[2U].x, 0.9F, 0.002F) &&
             near(entered.shape->points[2U].y, 0.9F, 0.002F));

    static_cast<void>(click(0.1F, 0.1F));
    static_cast<void>(click(0.9F, 0.1F));
    static_cast<void>(click(0.9F, 0.9F));
    static_cast<void>(h.frame(Pointer{500.0F, 500.0F, false, false, 0.0F,
                                      gui::KeyMods::None, gui::Key::Escape}));
    UI_CHECK(!h.panel.shape_in_progress());
    UI_CHECK(!h.frame(Pointer{500.0F, 500.0F, false, false, 0.0F,
                              gui::KeyMods::None, gui::Key::Enter})
                  .shape.has_value());

    // Two points and Enter: nothing to close.
    static_cast<void>(click(0.1F, 0.1F));
    static_cast<void>(click(0.9F, 0.1F));
    UI_CHECK(!h.frame(Pointer{500.0F, 500.0F, false, false, 0.0F,
                              gui::KeyMods::None, gui::Key::Enter})
                  .shape.has_value());
    // Choosing another tool puts the part-drawn curve away.
    h.panel.set_canvas_tool(editor::CanvasTool::Select);
    static_cast<void>(h.at(10.0F, 10.0F));
    UI_CHECK(!h.panel.shape_in_progress());
}

void a_canvas_pixel_resolves_to_its_point_on_a_plane_facing_the_camera() {
    Harness h;
    const scene::Vec3 right{1.0F, 0.0F, 0.0F};
    const scene::Vec3 up{0.0F, 1.0F, 0.0F};
    // The middle of the picture is what the camera looks at.
    const auto middle = h.panel.canvas_point_on_plane(
        h.scene, nullptr, scene::Vec3{}, right, up, 960.0F, 540.0F);
    UI_CHECK(middle.has_value());
    UI_CHECK(near(middle->x, 0.0F, 1.0e-3F) && near(middle->y, 0.0F, 1.0e-3F) &&
             near(middle->z, 0.0F, 1.0e-3F));
    // Any other pixel comes back to itself through the camera, and up the
    // picture is up in the world.
    const auto corner = h.panel.canvas_point_on_plane(
        h.scene, nullptr, scene::Vec3{}, right, up, 1310.0F, 215.0F);
    UI_CHECK(corner.has_value());
    UI_CHECK(corner->x > 0.0F && corner->y > 0.0F &&
             near(corner->z, 0.0F, 1.0e-3F));
    const scene::CanvasProjection back =
        scene::project_to_canvas(h.scene.camera, h.scene.canvas, *corner);
    UI_CHECK(near(back.x, 1310.0F, 0.05F) && near(back.y, 215.0F, 0.05F));
}

void a_fills_handles_sit_where_the_renderer_puts_the_ramp() {
    // A surface four wide and two tall.
    constexpr float width = 4.0F;
    constexpr float height = 2.0F;
    scene::Fill fill{};
    // One colour has nowhere to put a handle.
    UI_CHECK(!editor::fill_handles(fill, width, height).visible);

    // Left to right across the whole surface, off-centre.
    fill.mode = scene::FillMode::Linear;
    fill.angle = 0.0F;
    fill.center_x = 0.4F;
    fill.center_y = 0.3F;
    fill.size = 1.0F;
    editor::FillHandles handles = editor::fill_handles(fill, width, height);
    UI_CHECK(handles.visible);
    UI_CHECK(handles.centre == editor::BoxPoint{0.4F, 0.3F});
    // A size of one is the surface's whole width: half of it either side.
    UI_CHECK(near(handles.start.u, -0.1F, 1.0e-5F) &&
             near(handles.start.v, 0.3F, 1.0e-5F));
    UI_CHECK(near(handles.end.u, 0.9F, 1.0e-5F) &&
             near(handles.end.v, 0.3F, 1.0e-5F));

    // A quarter turn runs top to bottom, by the height instead; at half
    // size the ends are a quarter of the height either side.
    fill.angle = 0.25F;
    fill.size = 0.5F;
    handles = editor::fill_handles(fill, width, height);
    UI_CHECK(near(handles.end.u, 0.4F, 1.0e-5F) &&
             near(handles.end.v, 0.55F, 1.0e-5F));
    UI_CHECK(near(handles.start.v, 0.05F, 1.0e-5F));

    // Radial: a size of one reaches the corners, so its rim is half the
    // diagonal out - sqrt(5) units, over a width of four.
    fill.mode = scene::FillMode::Radial;
    fill.size = 1.0F;
    handles = editor::fill_handles(fill, width, height);
    UI_CHECK(near(handles.end.u, 0.4F + std::sqrt(5.0F) / 4.0F, 1.0e-5F));
    UI_CHECK(near(handles.end.v, 0.3F, 1.0e-5F));
    UI_CHECK(handles.start == handles.centre);
}

void dragging_a_fills_handles_writes_the_value_that_puts_them_there() {
    constexpr float width = 4.0F;
    constexpr float height = 2.0F;
    scene::Fill fill{};
    fill.mode = scene::FillMode::Linear;
    fill.center_x = 0.5F;
    fill.center_y = 0.5F;

    // The end dragged to a point: the handle is then exactly there, the
    // centre where it was, whatever the direction.
    for (const editor::BoxPoint target :
         {editor::BoxPoint{0.9F, 0.5F}, editor::BoxPoint{0.5F, 0.1F},
          editor::BoxPoint{0.2F, 0.85F}, editor::BoxPoint{0.75F, 0.25F}}) {
        const scene::Fill dragged =
            editor::fill_with_end(fill, width, height, target);
        const editor::FillHandles handles =
            editor::fill_handles(dragged, width, height);
        UI_CHECK(near(handles.end.u, target.u, 1.0e-4F) &&
                 near(handles.end.v, target.v, 1.0e-4F));
        UI_CHECK(dragged.center_x == 0.5F && dragged.center_y == 0.5F);
        UI_CHECK(dragged.angle >= 0.0F && dragged.angle < 1.0F);
    }
    // Straight up is three quarters of a turn clockwise from rightwards.
    UI_CHECK(near(
        editor::fill_with_end(fill, width, height, {0.5F, 0.1F}).angle, 0.75F,
        1.0e-5F));

    // A line drawn afresh: colour A at its start, B at its end.
    const scene::Fill drawn = editor::fill_along(
        fill, width, height, {0.1F, 0.2F}, {0.6F, 0.9F});
    const editor::FillHandles line =
        editor::fill_handles(drawn, width, height);
    UI_CHECK(near(line.start.u, 0.1F, 1.0e-4F) &&
             near(line.start.v, 0.2F, 1.0e-4F));
    UI_CHECK(near(line.end.u, 0.6F, 1.0e-4F) &&
             near(line.end.v, 0.9F, 1.0e-4F));

    // The centre alone moves nothing else.
    const scene::Fill moved = editor::fill_with_centre(drawn, {0.3F, 0.7F});
    UI_CHECK(moved.center_x == 0.3F && moved.center_y == 0.7F);
    UI_CHECK(moved.angle == drawn.angle && moved.size == drawn.size);

    // Radial: the start is the centre and the end its rim.
    fill.mode = scene::FillMode::Radial;
    const scene::Fill round = editor::fill_along(
        fill, width, height, {0.25F, 0.5F}, {0.75F, 0.5F});
    UI_CHECK(round.center_x == 0.25F && round.center_y == 0.5F);
    // Two units out, over a half diagonal of sqrt(5).
    UI_CHECK(near(round.size, 2.0F / std::sqrt(5.0F), 1.0e-5F));
    UI_CHECK(near(editor::fill_handles(round, width, height).end.u, 0.75F,
                  1.0e-4F));

    // A single colour takes no drag, and a press that never moved keeps a
    // ramp of some length.
    scene::Fill solid{};
    UI_CHECK(editor::fill_with_end(solid, width, height, {0.9F, 0.9F}) ==
             solid);
    fill.mode = scene::FillMode::Linear;
    UI_CHECK(editor::fill_along(fill, width, height, {0.5F, 0.5F},
                                {0.5F, 0.5F})
                 .size == fill.size);
    UI_CHECK(editor::fill_with_end(fill, width, height, {40.0F, 0.5F}).size ==
             scene::max_fill_size);
}

void the_gradient_tool_drags_the_selected_cards_fill_and_nothing_else() {
    Harness h;
    // The clip gives way to a card of the same size with a gradient across
    // it from left to right.
    const doc::NodeId card{11U};
    h.scene.media.clear();
    scene::MeshInstance filled{};
    filled.source = card;
    filled.mesh = {scene::Primitive::Quad, 1U};
    filled.world = scene::scaling(scene::Vec3{2.0F, 2.0F, 1.0F});
    scene::Fill fill{};
    fill.mode = scene::FillMode::Linear;
    filled.material.fill = fill;
    h.scene.instances.push_back(filled);
    h.selection = {card};
    h.panel.set_canvas_tool(editor::CanvasTool::Gradient);

    const editor::TransformBox box = h.at(10.0F, 10.0F).box;
    UI_CHECK(box.visible);
    // The ramp's end is the middle of the right-hand side. Dragged to the
    // middle of the bottom, the ramp runs top to bottom.
    const gui::UiPoint end = on_box(box, 1.0F, 0.5F);
    const gui::UiPoint below = on_box(box, 0.5F, 1.0F);
    const editor::ViewportFrame pressed = h.at(end.x, end.y, true);
    UI_CHECK(!pressed.pressed && !pressed.picked.has_value());
    UI_CHECK(!pressed.fill.has_value());
    const editor::ViewportFrame turned = h.at(below.x, below.y, true);
    UI_CHECK(turned.fill.has_value());
    UI_CHECK(near(turned.fill->angle, 0.25F, 0.002F));
    UI_CHECK(near(turned.fill->size, 1.0F, 0.005F));
    UI_CHECK(turned.fill->center_x == 0.5F && turned.fill->center_y == 0.5F);
    UI_CHECK(!turned.transform.has_value() && !h.panel.dragging());
    static_cast<void>(h.at(below.x, below.y, false));

    // The middle, dragged: only the centre moves.
    const gui::UiPoint middle = on_box(box, 0.5F, 0.5F);
    const gui::UiPoint aside = on_box(box, 0.3F, 0.6F);
    static_cast<void>(h.at(middle.x, middle.y, true));
    const editor::ViewportFrame moved = h.at(aside.x, aside.y, true);
    UI_CHECK(moved.fill.has_value());
    UI_CHECK(near(moved.fill->center_x, 0.3F, 0.002F) &&
             near(moved.fill->center_y, 0.6F, 0.002F));
    UI_CHECK(moved.fill->angle == fill.angle && moved.fill->size == fill.size);
    static_cast<void>(h.at(aside.x, aside.y, false));

    // Anywhere else: a fresh line from the press to the pointer.
    const gui::UiPoint from = on_box(box, 0.2F, 0.2F);
    const gui::UiPoint to = on_box(box, 0.2F, 0.7F);
    static_cast<void>(h.at(from.x, from.y, true));
    const editor::ViewportFrame drawn = h.at(to.x, to.y, true);
    UI_CHECK(drawn.fill.has_value());
    UI_CHECK(near(drawn.fill->angle, 0.25F, 0.002F));
    UI_CHECK(near(drawn.fill->center_x, 0.2F, 0.002F) &&
             near(drawn.fill->center_y, 0.45F, 0.002F));
    UI_CHECK(near(drawn.fill->size, 0.5F, 0.005F));
    static_cast<void>(h.at(to.x, to.y, false));
    // Let go, nothing more is reported.
    UI_CHECK(!h.at(to.x + 30.0F, to.y, false).fill.has_value());

    // One colour, or a layer with no fill at all, takes no drag.
    h.scene.instances[0U].material.fill->mode = scene::FillMode::Solid;
    static_cast<void>(h.at(from.x, from.y, true));
    UI_CHECK(!h.at(to.x, to.y, true).fill.has_value());
    static_cast<void>(h.at(to.x, to.y, false));
    h.scene.instances[0U].material.fill.reset();
    static_cast<void>(h.at(from.x, from.y, true));
    const editor::ViewportFrame bare = h.at(to.x, to.y, true);
    UI_CHECK(!bare.fill.has_value() && !bare.transform.has_value());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr Case cases[] = {
    {"each tool has its own key and other keys choose none",
     &each_tool_has_its_own_key_and_other_keys_choose_none},
    {"the palette chooses the key clicked and a lock refuses select",
     &the_palette_chooses_the_key_clicked_and_a_lock_refuses_select},
    {"a box finds its handles and drops the edge ones when small",
     &a_box_finds_its_handles_and_drops_the_edge_ones_when_small},
    {"a resize stretches from its anchor and stops short of inverting",
     &a_resize_stretches_from_its_anchor_and_stops_short_of_inverting},
    {"the selected layer has a box on program and none elsewhere",
     &the_selected_layer_has_a_box_on_program_and_none_elsewhere},
    {"a corner drag resizes against the opposite corner",
     &a_corner_drag_resizes_against_the_opposite_corner},
    {"a side handle stretches one way against the far side",
     &a_side_handle_stretches_one_way_against_the_far_side},
    {"the turn handle turns the layer the way the pointer goes",
     &the_turn_handle_turns_the_layer_the_way_the_pointer_goes},
    {"typed values resolve to the gesture that reaches them",
     &typed_values_resolve_to_the_gesture_that_reaches_them},
    {"the wheel zooms about the pointer and fit puts it back",
     &the_wheel_zooms_about_the_pointer_and_fit_puts_it_back},
    {"a middle drag pans with any tool and keeps the picture in reach",
     &a_middle_drag_pans_with_any_tool_and_keeps_the_picture_in_reach},
    {"the hand tool pans with the left button and picks nothing",
     &the_hand_tool_pans_with_the_left_button_and_picks_nothing},
    {"the zoom tool doubles on a click and alt halves",
     &the_zoom_tool_doubles_on_a_click_and_alt_halves},
    {"a handle in the bars is taken and a bare press there is not",
     &a_handle_in_the_bars_is_taken_and_a_bare_press_there_is_not},
    {"a drag on the layer itself still moves it",
     &a_drag_on_the_layer_itself_still_moves_it},
    {"a shape dragged on the selected layer is in the layers own space",
     &a_shape_dragged_on_the_selected_layer_is_in_the_layers_own_space},
    {"a shape dragged on bare picture is in canvas pixels",
     &a_shape_dragged_on_bare_picture_is_in_canvas_pixels},
    {"the pen places points curves them and closes on the first",
     &the_pen_places_points_curves_them_and_closes_on_the_first},
    {"a canvas pixel resolves to its point on a plane facing the camera",
     &a_canvas_pixel_resolves_to_its_point_on_a_plane_facing_the_camera},
    {"a fills handles sit where the renderer puts the ramp",
     &a_fills_handles_sit_where_the_renderer_puts_the_ramp},
    {"dragging a fills handles writes the value that puts them there",
     &dragging_a_fills_handles_writes_the_value_that_puts_them_there},
    {"the gradient tool drags the selected cards fill and nothing else",
     &the_gradient_tool_drags_the_selected_cards_fill_and_nothing_else},
};

}  // namespace

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
    const std::size_t total = sizeof(cases) / sizeof(cases[0]);
    std::cout << (total - failures) << '/' << total << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
