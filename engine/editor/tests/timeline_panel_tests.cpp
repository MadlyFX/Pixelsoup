#include <mediaserver/editor/editor_shell.hpp>
#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The bespoke timeline's proof, with no window and no GPU. Its contract is the
// requests it reports to the shell, so synthetic pointer frames can exercise
// the whole interaction: transport, range, selection, ruler scrub, keying, and
// a retime drag that commits exactly once on release.
namespace {

namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace gui = mediaserver::gui;
namespace output = mediaserver::output;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define UI_CHECK(...)                                                          \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

const doc::NodeId object_node{5U};
const doc::NodeId clip_node{7U};
const doc::NodeId surface_node{90U};

[[nodiscard]] editor::ElementInfo make_object() {
    editor::ElementInfo out{};
    out.node = object_node;
    out.kind = editor::ElementKind::Object;
    out.position = scene::Vec3{1.0F, 2.0F, 3.0F};
    out.rotation = scene::Vec3{0.1F, 0.2F, 0.3F};
    out.scale = scene::Vec3{1.0F, 1.5F, 2.0F};
    return out;
}

// Placed at frame 20 for forty frames. At the harness's 1000px width the
// graph starts at x=260 and holds 7.4 pixels per frame, so the clip's bar runs
// x=408..704 on the second layer row, y=81..97.
[[nodiscard]] editor::ElementInfo make_clip() {
    editor::ElementInfo out{};
    out.node = clip_node;
    out.kind = editor::ElementKind::Media;
    out.sequence_count = 240U;
    out.timing = scene::ClipTiming{20, 40, 0, 0, 0};
    return out;
}

[[nodiscard]] output::ProjectionSurface make_surface() {
    output::ProjectionSurface surface{};
    surface.node = surface_node;
    surface.name = "Curved LED";
    surface.geometry.center = scene::Vec3{4.0F, 5.0F, 6.0F};
    surface.geometry.right = scene::Vec3{0.0F, 0.0F, -1.0F};
    surface.geometry.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    surface.geometry.width = 8.0F;
    surface.geometry.height = 4.5F;
    surface.geometry.raster = scene::Canvas{1'920U, 1'080U};
    return surface;
}

struct Harness final {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    editor::TimelinePanel panel{gui::ui_id("timeline")};
    editor::EditorTimeline timeline{50U, 50U, 0U, 100U, false, false, false};
    std::vector<editor::ElementInfo> elements{make_object(), make_clip()};
    std::vector<scene::ChannelKeyframes> keys{
        scene::ChannelKeyframes{
            object_node, scene::TransformChannel::PositionX, {10, 20, 50}},
        scene::ChannelKeyframes{
            object_node, scene::TransformChannel::PositionY, {25, 50}},
    };
    std::vector<doc::NodeId> selection;
    doc::NodeId primary{};
    std::vector<output::ProjectionSurface> surfaces;
    doc::NodeId selected_surface{};

    static constexpr gui::Rect bounds{0.0F, 0.0F, 1000.0F, 400.0F};

    editor::TimelinePanelFrame frame(float x, float y, bool down,
                                     gui::KeyMods mods = gui::KeyMods::None,
                                     float wheel = 0.0F,
                                     std::string_view text = {}) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        input.set_mods(mods);
        if (wheel != 0.0F) {
            input.add_wheel(wheel);
        }
        if (!text.empty()) {
            input.add_text(text);
        }
        ctx.new_frame();
        return panel.draw(ctx, bounds, timeline, keys, elements, selection,
                          primary, {}, surfaces, selected_surface);
    }

    editor::TimelinePanelFrame click(float x, float y,
                                     gui::KeyMods mods = gui::KeyMods::None) {
        static_cast<void>(frame(x, y, false, mods));
        static_cast<void>(frame(x, y, true, mods));
        return frame(x, y, false, mods);
    }

    editor::TimelinePanelFrame key(gui::Key pressed) {
        input.new_frame();
        input.set_mouse_button(gui::MouseButton::Left, false);
        input.set_mouse_button(gui::MouseButton::Middle, false);
        input.set_mods(gui::KeyMods::None);
        input.add_key(pressed);
        ctx.new_frame();
        return panel.draw(ctx, bounds, timeline, keys, elements, selection,
                          primary, {}, surfaces, selected_surface);
    }

    editor::TimelinePanelFrame middle(float x, float y, bool down) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, false);
        input.set_mouse_button(gui::MouseButton::Middle, down);
        input.set_mods(gui::KeyMods::None);
        ctx.new_frame();
        return panel.draw(ctx, bounds, timeline, keys, elements, selection,
                          primary, {}, surfaces, selected_surface);
    }

    void open_position_components() {
        // Layer row is y=56..78. Its twirl is left of the layer name.
        static_cast<void>(click(10.0F, 67.0F));
        // Position becomes the next row, y=78..100; indent puts its twirl at
        // x=26.
        static_cast<void>(click(25.0F, 89.0F));
        static_cast<void>(frame(0.0F, 0.0F, false));
    }
};

void transport_controls_report_their_action() {
    Harness h;
    // The six compact transport buttons begin at x=6. PLAY occupies 90..128.
    const editor::TimelinePanelFrame played = h.click(105.0F, 17.0F);
    UI_CHECK(played.transport == editor::TransportRequest::TogglePlay);

    const editor::TimelinePanelFrame start = h.click(16.0F, 17.0F);
    UI_CHECK(start.transport == editor::TransportRequest::GoToStart);
}

void the_ruler_scrubs_and_clamps_to_the_show_range() {
    Harness h;
    // At 1000px wide the list is 260px and the 0..100 range fits the 740px
    // graph. One quarter across is frame 25.
    static_cast<void>(h.frame(445.0F, 45.0F, false));
    const editor::TimelinePanelFrame pressed = h.frame(445.0F, 45.0F, true);
    UI_CHECK(pressed.seek_to_frame.has_value());
    UI_CHECK(pressed.seek_to_frame.value() == 25U);

    // Capture survives leaving the ruler and clamps at OUT.
    const editor::TimelinePanelFrame outside = h.frame(1400.0F, 45.0F, true);
    UI_CHECK(outside.seek_to_frame.has_value());
    UI_CHECK(outside.seek_to_frame.value() == 100U);
}

void a_layer_row_selects_and_modifier_click_toggles() {
    Harness h;
    const editor::TimelinePanelFrame selected = h.click(80.0F, 67.0F);
    UI_CHECK(selected.selection.has_value());
    UI_CHECK(selected.selection->node == object_node);
    UI_CHECK(!selected.selection->toggle);

    const editor::TimelinePanelFrame toggled =
        h.click(80.0F, 67.0F, gui::KeyMods::Ctrl);
    UI_CHECK(toggled.selection.has_value());
    UI_CHECK(toggled.selection->node == object_node);
    UI_CHECK(toggled.selection->toggle);
}

void physical_surface_rows_author_position_and_yaw_keys() {
    Harness h;
    h.elements.clear();
    h.keys.clear();
    h.surfaces.push_back(make_surface());
    static_cast<void>(h.frame(0.0F, 0.0F, false));

    const editor::TimelinePanelFrame selected = h.click(80.0F, 67.0F);
    UI_CHECK(selected.selection.has_value());
    UI_CHECK(selected.selection->node == surface_node);
    UI_CHECK(selected.selection->surface);

    // Surface layer y=56..78. Open it to reveal Position then Rotation.
    static_cast<void>(h.click(10.0F, 67.0F));
    const editor::TimelinePanelFrame position = h.click(248.0F, 89.0F);
    UI_CHECK(position.keys.size() == 3U);
    UI_CHECK(position.keys[0].target == surface_node);
    UI_CHECK(position.keys[0].component ==
             scene::PropertyChannel::PositionX);
    UI_CHECK(position.keys[0].value == 4.0);
    UI_CHECK(position.keys[1].component ==
             scene::PropertyChannel::PositionY);
    UI_CHECK(position.keys[1].value == 5.0);
    UI_CHECK(position.keys[2].component ==
             scene::PropertyChannel::PositionZ);
    UI_CHECK(position.keys[2].value == 6.0);

    // A physical surface exposes only the horizontal Rotation/Yaw channel.
    const editor::TimelinePanelFrame yaw = h.click(248.0F, 111.0F);
    UI_CHECK(yaw.keys.size() == 1U);
    UI_CHECK(yaw.keys[0].target == surface_node);
    UI_CHECK(yaw.keys[0].component ==
             scene::PropertyChannel::RotationYaw);
    UI_CHECK(std::abs(yaw.keys[0].value - 0.25) < 1.0e-6);
}

void surface_transform_edits_key_only_animated_components() {
    editor::FeedSurfaceEditRequest request{};
    request.node = surface_node;
    request.name = "Curved LED";
    request.surface = make_surface().geometry;
    request.gesture = 77U;
    request.transform_components =
        editor::FeedSurfaceTransform::PositionX |
        editor::FeedSurfaceTransform::PositionY |
        editor::FeedSurfaceTransform::Yaw;

    std::vector<scene::ChannelKeyframes> channels{
        scene::ChannelKeyframes{
            surface_node, scene::PropertyChannel::PositionX, {0}, {4.0},
            {scene::KeyInterpolation::Linear}},
        scene::ChannelKeyframes{
            surface_node, scene::PropertyChannel::RotationYaw, {0}, {0.25},
            {scene::KeyInterpolation::Linear}},
    };
    const editor::RoutedSurfaceEdit routed =
        editor::route_surface_transform_edit(request, channels, 42U);

    UI_CHECK(routed.keys.size() == 2U);
    UI_CHECK(routed.keys[0] == editor::KeyIntent{
                                   surface_node,
                                   scene::PropertyChannel::PositionX,
                                   42U,
                                   4.0,
                                   false,
                                   77U});
    UI_CHECK(routed.keys[1].component ==
             scene::PropertyChannel::RotationYaw);
    UI_CHECK(std::abs(routed.keys[1].value - 0.25) < 1.0e-6);
    UI_CHECK(routed.keys[1].gesture == 77U);
    UI_CHECK(routed.base.has_value());
    UI_CHECK(routed.base->gesture == 77U);
    UI_CHECK(editor::has_transform(
        routed.base->transform_components,
        editor::FeedSurfaceTransform::PositionY));
    UI_CHECK(!editor::has_transform(
        routed.base->transform_components,
        editor::FeedSurfaceTransform::PositionX));
    UI_CHECK(!editor::has_transform(
        routed.base->transform_components, editor::FeedSurfaceTransform::Yaw));

    request.transform_components = editor::FeedSurfaceTransform::None;
    const editor::RoutedSurfaceEdit calibration =
        editor::route_surface_transform_edit(request, channels, 42U);
    UI_CHECK(calibration.keys.empty());
    UI_CHECK(calibration.base.has_value());
    UI_CHECK(calibration.base->transform_components ==
             editor::FeedSurfaceTransform::None);
}

void timeline_search_filters_scene_layers_and_surfaces() {
    Harness h;
    h.surfaces.push_back(make_surface());

    // The search well occupies the fixed left side of the ruler, matching the
    // Scene panel's case-insensitive text-field behavior.
    static_cast<void>(h.click(80.0F, 45.0F));
    const editor::TimelinePanelFrame typed =
        h.frame(80.0F, 45.0F, false, gui::KeyMods::None, 0.0F, "cUrVeD");
    UI_CHECK(typed.editing);
    UI_CHECK(h.panel.search() == "cUrVeD");
    static_cast<void>(h.frame(80.0F, 45.0F, false));

    // The filtered surface becomes the first row; object/clip rows are gone.
    const editor::TimelinePanelFrame selected = h.click(80.0F, 67.0F);
    UI_CHECK(selected.selection.has_value());
    UI_CHECK(selected.selection->node == surface_node);
    UI_CHECK(selected.selection->surface);
}

void clear_emits_every_layer_channel() {
    Harness h;
    h.selection = {object_node};
    h.primary = object_node;
    static_cast<void>(h.frame(0.0F, 0.0F, false));

    // Clear remains a distinct operation after SNAP / FIT / KEYS.
    const editor::TimelinePanelFrame cleared = h.click(535.0F, 17.0F);
    UI_CHECK(cleared.keys.size() == 10U);
    for (std::size_t index = 0U; index < cleared.keys.size(); ++index) {
        UI_CHECK(cleared.keys[index].target == object_node);
        UI_CHECK(cleared.keys[index].clear);
        UI_CHECK(cleared.keys[index].component ==
                 static_cast<scene::TransformChannel>(index));
    }
}

void range_controls_report_loop_and_exact_bounds() {
    Harness h;
    static_cast<void>(h.frame(0.0F, 0.0F, false));

    // Loop is right aligned at x=910..994.
    const editor::TimelinePanelFrame looped = h.click(930.0F, 17.0F);
    UI_CHECK(looped.range.has_value());
    UI_CHECK(looped.range->first == 0U);
    UI_CHECK(looped.range->last == 100U);
    UI_CHECK(looped.range->loop);

    // IN is a NumberField at x=738..802, one frame per drag pixel.
    static_cast<void>(h.frame(760.0F, 17.0F, false));
    static_cast<void>(h.frame(760.0F, 17.0F, true));
    const editor::TimelinePanelFrame dragged = h.frame(780.0F, 17.0F, true);
    UI_CHECK(dragged.range.has_value());
    UI_CHECK(dragged.range->first == 20U);
    UI_CHECK(dragged.range->last == 100U);
    UI_CHECK(dragged.editing);
}

void show_rate_menu_reports_exact_common_rates() {
    constexpr std::array<editor::ShowRateRequest, 8U> expected{{
        {24'000U, 1'001U},
        {24U, 1U},
        {25U, 1U},
        {30'000U, 1'001U},
        {30U, 1U},
        {50U, 1U},
        {60'000U, 1'001U},
        {60U, 1U},
    }};

    Harness h;
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        // Keep the selected entry different from the current rate so every
        // exact rational is observable as an authored request.
        h.timeline.rate_numerator = expected[index].numerator == 50U
            && expected[index].denominator == 1U
            ? 24U
            : 50U;
        h.timeline.rate_denominator = 1U;

        const editor::TimelinePanelFrame opened = h.click(670.0F, 17.0F);
        UI_CHECK(!opened.show_rate.has_value());
        UI_CHECK(opened.editing);

        const editor::TimelinePanelFrame selected =
            h.click(670.0F, 46.0F + 21.0F * static_cast<float>(index));
        UI_CHECK(selected.show_rate.has_value());
        UI_CHECK(selected.show_rate.value() == expected[index]);
        UI_CHECK(!selected.seek_to_frame.has_value());
    }

    // Choosing the rate already in force simply dismisses the popup.
    h.timeline.rate_numerator = 24U;
    h.timeline.rate_denominator = 1U;
    static_cast<void>(h.click(670.0F, 17.0F));
    const editor::TimelinePanelFrame unchanged = h.click(670.0F, 67.0F);
    UI_CHECK(!unchanged.show_rate.has_value());
}

void a_key_drag_retimes_once_on_release() {
    Harness h;
    static_cast<void>(h.frame(0.0F, 0.0F, false));
    h.open_position_components();

    // Position X is y=100..122. Frame 10 is 74px into the fitted graph:
    // x=260 + 10*7.4 = 334. Dragging 37px lands exactly at frame 15.
    static_cast<void>(h.frame(334.0F, 111.0F, false));
    const editor::TimelinePanelFrame pressed = h.frame(334.0F, 111.0F, true);
    UI_CHECK(pressed.key_moves.empty());

    const editor::TimelinePanelFrame held = h.frame(371.0F, 111.0F, true);
    UI_CHECK(held.key_moves.empty());

    const editor::TimelinePanelFrame released = h.frame(371.0F, 111.0F, false);
    UI_CHECK(released.key_moves.size() == 1U);
    UI_CHECK(released.key_moves[0].target == object_node);
    UI_CHECK(released.key_moves[0].component ==
             scene::TransformChannel::PositionX);
    UI_CHECK(released.key_moves[0].from_frame == 10);
    UI_CHECK(released.key_moves[0].to_frame == 15);

    const editor::TimelinePanelFrame after = h.frame(371.0F, 111.0F, false);
    UI_CHECK(after.key_moves.empty());
}

void modifier_clicks_build_and_trim_a_key_selection() {
    Harness h;
    static_cast<void>(h.frame(0.0F, 0.0F, false));
    h.open_position_components();

    static_cast<void>(h.click(334.0F, 111.0F));
    UI_CHECK(h.panel.selected_key_count() == 1U);
    static_cast<void>(h.click(408.0F, 111.0F, gui::KeyMods::Shift));
    UI_CHECK(h.panel.selected_key_count() == 2U);
    static_cast<void>(h.click(334.0F, 111.0F, gui::KeyMods::Ctrl));
    UI_CHECK(h.panel.selected_key_count() == 1U);
}

void property_diamonds_key_groups_and_individual_channels() {
    Harness group;
    static_cast<void>(group.frame(0.0F, 0.0F, false));
    group.open_position_components();

    // The row-local diamond is at the right edge of the layer column.
    const editor::TimelinePanelFrame group_key = group.click(248.0F, 89.0F);
    UI_CHECK(group_key.keys.size() == 3U);
    UI_CHECK(group_key.keys[0].component ==
             scene::TransformChannel::PositionX);
    UI_CHECK(group_key.keys[0].frame == group.timeline.show_frame);
    UI_CHECK(group_key.keys[0].value == 1.0);
    UI_CHECK(group_key.keys[1].component ==
             scene::TransformChannel::PositionY);
    UI_CHECK(group_key.keys[2].component ==
             scene::TransformChannel::PositionZ);
    UI_CHECK(!group_key.keys[0].erase);
    UI_CHECK(!group_key.keys[1].erase);
    UI_CHECK(!group_key.keys[2].erase);

    Harness component;
    static_cast<void>(component.frame(0.0F, 0.0F, false));
    component.open_position_components();
    const editor::TimelinePanelFrame component_key =
        component.click(248.0F, 111.0F);
    UI_CHECK(component_key.keys.size() == 1U);
    UI_CHECK(component_key.keys[0].component ==
             scene::TransformChannel::PositionX);
    UI_CHECK(component_key.keys[0].value == 1.0);
    UI_CHECK(component_key.keys[0].erase);
}

void a_marquee_selects_keys_and_the_group_retimes_together() {
    Harness h;
    static_cast<void>(h.frame(0.0F, 0.0F, false));
    h.open_position_components();

    // Drag a box around Position X's frame-10 and frame-20 diamonds.
    static_cast<void>(h.frame(320.0F, 105.0F, false));
    static_cast<void>(h.frame(320.0F, 105.0F, true));
    static_cast<void>(h.frame(415.0F, 116.0F, true));
    static_cast<void>(h.frame(415.0F, 116.0F, false));
    UI_CHECK(h.panel.selected_key_count() == 2U);

    // Grabbing either selected diamond moves the complete selection by five.
    static_cast<void>(h.frame(334.0F, 111.0F, false));
    static_cast<void>(h.frame(334.0F, 111.0F, true));
    static_cast<void>(h.frame(371.0F, 111.0F, true));
    const editor::TimelinePanelFrame moved =
        h.frame(371.0F, 111.0F, false);
    UI_CHECK(moved.key_moves.size() == 2U);
    UI_CHECK(moved.key_moves[0].from_frame == 20);
    UI_CHECK(moved.key_moves[0].to_frame == 25);
    UI_CHECK(moved.key_moves[1].from_frame == 10);
    UI_CHECK(moved.key_moves[1].to_frame == 15);
}

void key_drags_snap_to_nearby_channel_keys() {
    Harness h;
    static_cast<void>(h.frame(0.0F, 0.0F, false));
    h.open_position_components();

    // Pointer frame 24 is within eight pixels of Position Y's frame-25 key.
    static_cast<void>(h.frame(334.0F, 111.0F, false));
    static_cast<void>(h.frame(334.0F, 111.0F, true));
    static_cast<void>(h.frame(440.0F, 111.0F, true));
    const editor::TimelinePanelFrame snapped =
        h.frame(440.0F, 111.0F, false);
    UI_CHECK(snapped.key_moves.size() == 1U);
    UI_CHECK(snapped.key_moves[0].from_frame == 10);
    UI_CHECK(snapped.key_moves[0].to_frame == 25);
}

void delete_erases_selected_keys_not_the_whole_channel() {
    Harness h;
    static_cast<void>(h.frame(0.0F, 0.0F, false));
    h.open_position_components();
    static_cast<void>(h.click(334.0F, 111.0F));

    const editor::TimelinePanelFrame erased = h.key(gui::Key::Delete);
    UI_CHECK(erased.consume_delete);
    UI_CHECK(erased.keys.size() == 1U);
    UI_CHECK(erased.keys[0].target == object_node);
    UI_CHECK(erased.keys[0].component ==
             scene::TransformChannel::PositionX);
    UI_CHECK(erased.keys[0].frame == 10U);
    UI_CHECK(erased.keys[0].erase);
    UI_CHECK(!erased.keys[0].clear);
    UI_CHECK(h.panel.selected_key_count() == 0U);
}

void middle_mouse_pans_time_without_seeking() {
    Harness h;
    static_cast<void>(h.frame(500.0F, 200.0F, false));
    static_cast<void>(h.middle(500.0F, 200.0F, false));
    static_cast<void>(h.middle(500.0F, 200.0F, true));
    const editor::TimelinePanelFrame panned = h.middle(400.0F, 200.0F, true);
    UI_CHECK(!panned.seek_to_frame.has_value());
    static_cast<void>(h.middle(400.0F, 200.0F, false));

    // The same ruler x was frame 50 before the 100px leftward pan; it now
    // addresses roughly frame 64.
    const editor::TimelinePanelFrame scrubbed = h.click(630.0F, 45.0F);
    UI_CHECK(scrubbed.seek_to_frame.has_value());
    UI_CHECK(scrubbed.seek_to_frame.value() == 64U);
}

void clicking_an_empty_lane_scrubs_instead_of_moving_a_key() {
    Harness h;
    static_cast<void>(h.frame(0.0F, 0.0F, false));
    h.open_position_components();

    // Frame 40 has no Position X key.
    const editor::TimelinePanelFrame pressed = h.frame(556.0F, 111.0F, true);
    UI_CHECK(pressed.key_moves.empty());
    UI_CHECK(pressed.seek_to_frame.has_value());
    UI_CHECK(pressed.seek_to_frame.value() == 40U);
}

void a_project_replacement_clears_stale_keys_and_refits_the_new_range() {
    Harness h;
    static_cast<void>(h.frame(0.0F, 0.0F, false));
    h.open_position_components();
    static_cast<void>(h.click(334.0F, 111.0F));
    UI_CHECK(h.panel.selected_key_count() == 1U);

    // Deliberately zoom the old 0..100 project and preserve a user preference.
    static_cast<void>(h.frame(630.0F, 200.0F, false, gui::KeyMods::None, 4.0F));
    static_cast<void>(h.click(370.0F, 17.0F));
    UI_CHECK(!h.panel.snapping_enabled());

    h.timeline = editor::EditorTimeline{
        1'500U, 1'500U, 1'000U, 2'000U, false, false, false};
    static_cast<void>(h.frame(0.0F, 0.0F, false));
    const editor::TimelinePanelFrame still_zoomed = h.click(630.0F, 45.0F);
    UI_CHECK(still_zoomed.seek_to_frame.has_value());
    UI_CHECK(still_zoomed.seek_to_frame.value() != 1'500U);

    // EditorShell calls this only when EditorProjectState::generation changes,
    // which happens after a replacement has successfully committed.
    h.panel.reset_project_state();
    UI_CHECK(h.panel.selected_key_count() == 0U);
    UI_CHECK(!h.panel.snapping_enabled());
    static_cast<void>(h.frame(0.0F, 0.0F, false));
    const editor::TimelinePanelFrame fitted = h.click(630.0F, 45.0F);
    UI_CHECK(fitted.seek_to_frame.has_value());
    UI_CHECK(fitted.seek_to_frame.value() == 1'500U);
}

void a_panel_with_no_room_reports_nothing() {
    Harness h;
    h.input.new_frame();
    h.ctx.new_frame();
    const editor::TimelinePanelFrame frame =
        h.panel.draw(h.ctx, gui::Rect{0.0F, 0.0F, 0.0F, 0.0F}, h.timeline,
                     h.keys, h.elements, h.selection, h.primary);
    UI_CHECK(!frame.seek_to_frame.has_value());
    UI_CHECK(frame.keys.empty());
    UI_CHECK(frame.key_moves.empty());
    UI_CHECK(!frame.selection.has_value());
}


void dragging_a_clip_body_moves_the_placement() {
    Harness h;
    static_cast<void>(h.frame(500.0F, 90.0F, false));
    // The press claims the bar rather than scrubbing the playhead.
    const editor::TimelinePanelFrame pressed = h.frame(500.0F, 90.0F, true);
    UI_CHECK(!pressed.seek_to_frame.has_value());
    UI_CHECK(pressed.selection.has_value());
    UI_CHECK(pressed.selection->node == clip_node);
    UI_CHECK(h.panel.dragging_clip());

    const editor::TimelinePanelFrame moved = h.frame(574.0F, 90.0F, true);
    UI_CHECK(moved.clip_edits.size() == 1U);
    UI_CHECK(moved.clip_edits.front().clip == clip_node);
    UI_CHECK(moved.clip_edits.front().timing ==
             (scene::ClipTiming{30, 40, 0, 0, 0}));
    UI_CHECK(!moved.seek_to_frame.has_value());

    // Release ends the gesture, and nothing further is authored.
    const editor::TimelinePanelFrame released = h.frame(574.0F, 90.0F, false);
    UI_CHECK(released.clip_edits.empty());
    UI_CHECK(!h.panel.dragging_clip());
}

void dragging_a_clip_edge_trims_without_moving_the_rest() {
    Harness h;
    static_cast<void>(h.frame(410.0F, 90.0F, false));
    static_cast<void>(h.frame(410.0F, 90.0F, true));
    const editor::TimelinePanelFrame trimmed = h.frame(447.0F, 90.0F, true);
    UI_CHECK(trimmed.clip_edits.size() == 1U);
    // The out point is untouched and the source slides with the in point, so
    // the frames that survive still show what they showed.
    UI_CHECK(trimmed.clip_edits.front().timing ==
             (scene::ClipTiming{25, 35, 5, 0, 0}));
    static_cast<void>(h.frame(447.0F, 90.0F, false));

    // The other end changes only the duration.
    static_cast<void>(h.frame(700.0F, 90.0F, false));
    static_cast<void>(h.frame(700.0F, 90.0F, true));
    const editor::TimelinePanelFrame shortened = h.frame(663.0F, 90.0F, true);
    UI_CHECK(shortened.clip_edits.size() == 1U);
    UI_CHECK(shortened.clip_edits.front().timing ==
             (scene::ClipTiming{20, 35, 0, 0, 0}));
}

void a_clip_fade_handle_sets_the_head_ramp() {
    Harness h;
    static_cast<void>(h.frame(409.0F, 83.0F, false));
    static_cast<void>(h.frame(409.0F, 83.0F, true));
    const editor::TimelinePanelFrame faded = h.frame(446.0F, 83.0F, true);
    UI_CHECK(faded.clip_edits.size() == 1U);
    UI_CHECK(faded.clip_edits.front().timing ==
             (scene::ClipTiming{20, 40, 0, 5, 0}));
}

void a_clip_move_snaps_to_the_playhead() {
    Harness h;
    // Playhead is frame 50, so a body drag that lands the head two frames
    // short of it is pulled onto the cut.
    static_cast<void>(h.frame(500.0F, 90.0F, false));
    static_cast<void>(h.frame(500.0F, 90.0F, true));
    const editor::TimelinePanelFrame snapped = h.frame(707.0F, 90.0F, true);
    UI_CHECK(snapped.clip_edits.size() == 1U);
    UI_CHECK(snapped.clip_edits.front().timing.start == 50);
}

void splitting_reports_the_selected_clip_at_the_playhead() {
    Harness h;
    h.selection.push_back(clip_node);
    h.primary = clip_node;
    // SPLIT sits after SNAP, FIT, and the curve toggle, at x=502..556.
    const editor::TimelinePanelFrame split = h.click(529.0F, 17.0F);
    UI_CHECK(split.clip_splits.size() == 1U);
    UI_CHECK(split.clip_splits.front().clip == clip_node);
    UI_CHECK(split.clip_splits.front().frame == 50U);

    // With the playhead outside the clip there is nothing to cut and the
    // button is not offered at all.
    Harness outside;
    outside.selection.push_back(clip_node);
    outside.primary = clip_node;
    outside.timeline.show_frame = 90U;
    const editor::TimelinePanelFrame none = outside.click(529.0F, 17.0F);
    UI_CHECK(none.clip_splits.empty());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 24U> cases{{
    {"transport controls report their action",
     &transport_controls_report_their_action},
    {"dragging a clip body moves the placement",
     &dragging_a_clip_body_moves_the_placement},
    {"dragging a clip edge trims without moving the rest",
     &dragging_a_clip_edge_trims_without_moving_the_rest},
    {"a clip fade handle sets the head ramp",
     &a_clip_fade_handle_sets_the_head_ramp},
    {"a clip move snaps to the playhead",
     &a_clip_move_snaps_to_the_playhead},
    {"splitting reports the selected clip at the playhead",
     &splitting_reports_the_selected_clip_at_the_playhead},
    {"the ruler scrubs and clamps to the show range",
     &the_ruler_scrubs_and_clamps_to_the_show_range},
    {"a layer row selects and modifier click toggles",
     &a_layer_row_selects_and_modifier_click_toggles},
    {"physical surface rows author position and yaw keys",
     &physical_surface_rows_author_position_and_yaw_keys},
    {"surface transform edits key only animated components",
     &surface_transform_edits_key_only_animated_components},
    {"timeline search filters scene layers and surfaces",
     &timeline_search_filters_scene_layers_and_surfaces},
    {"clear emits every layer channel", &clear_emits_every_layer_channel},
    {"range controls report loop and exact bounds",
     &range_controls_report_loop_and_exact_bounds},
    {"show rate menu reports exact common rates",
     &show_rate_menu_reports_exact_common_rates},
    {"a key drag retimes once on release", &a_key_drag_retimes_once_on_release},
    {"modifier clicks build and trim a key selection",
     &modifier_clicks_build_and_trim_a_key_selection},
    {"property diamonds key groups and individual channels",
     &property_diamonds_key_groups_and_individual_channels},
    {"a marquee selects keys and the group retimes together",
     &a_marquee_selects_keys_and_the_group_retimes_together},
    {"key drags snap to nearby channel keys",
     &key_drags_snap_to_nearby_channel_keys},
    {"delete erases selected keys not the whole channel",
     &delete_erases_selected_keys_not_the_whole_channel},
    {"middle mouse pans time without seeking",
     &middle_mouse_pans_time_without_seeking},
    {"clicking an empty lane scrubs instead of moving a key",
     &clicking_an_empty_lane_scrubs_instead_of_moving_a_key},
    {"a project replacement clears stale keys and refits the new range",
     &a_project_replacement_clears_stale_keys_and_refits_the_new_range},
    {"a panel with no room reports nothing",
     &a_panel_with_no_room_reports_nothing},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& entry : cases) {
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
