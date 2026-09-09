#include <mediaserver/editor/inspector_panel.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// The Inspector's proof, with no GPU and no window.
//
// Nothing here renders: the panel's output is a list of property edits, and the
// draw list only records rectangles. What is under test is the behaviour that
// would be wrong silently. Chiefly the scrub: the document a field shows is
// several frames behind a live drag, so a field that re-based on the value it
// was handed each frame would crawl at a fraction of the pointer's speed, and
// the bug would look like "the mouse feels heavy" rather than like a fault.
//
// The layout is arithmetic, so the suite computes where the fields land rather
// than hunting for them: a 300-wide panel puts the transform's rows at known
// offsets, and the constants below are those offsets.
namespace {

namespace editor = mediaserver::editor;
namespace core = mediaserver::core;
namespace gui = mediaserver::gui;
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

[[nodiscard]] bool near(double a, double b, double tolerance = 1.0e-5) noexcept {
    return std::abs(a - b) < tolerance;
}

const doc::NodeId subject{5U};

// Where the panel puts things, given a 300x600 panel at the origin and an empty
// font (every string measures zero, so nothing shifts with the glyph metrics).
// Header 10..40, section 44..68, then 22-pixel rows four pixels apart.
constexpr float section_y = 50.0F;
constexpr float position_y = 80.0F;
constexpr float rotation_y = 106.0F;
constexpr float scale_y = 132.0F;
// The three fields of a row: label column 89.6 wide, then three 60.8-wide
// columns four pixels apart, starting at x = 99.6.
constexpr float field_x0 = 110.0F;
constexpr float field_x1 = 175.0F;
constexpr float field_x2 = 240.0F;
// A light's rows: swatch 72..86, then RGB, Intensity, Position, Range.
constexpr float light_rgb_y = 100.0F;
constexpr float light_intensity_y = 126.0F;
// The light rows run at a fixed pitch: RGB, Intensity, Position, Range, and
// then the spot controls. Derived rather than written out so a row added above
// moves them together instead of silently shifting the clicks onto a
// neighbour.
constexpr float light_row_pitch = 26.0F;
constexpr float light_position_y = light_intensity_y + 3.0F * light_row_pitch;
constexpr float light_range_y = light_position_y + light_row_pitch;
// A track's SCREEN section: TRANSFORM ends with its key note at 170, the
// Delete row runs 178..200, then the section header 204..228 and its two rows.
// The raster row carries two fields rather than three, so its columns are 93.2
// wide from x = 99.6.
constexpr float screen_raster_y = 243.0F;
constexpr float screen_aspect_y = 269.0F;
constexpr float raster_width_x = 145.0F;
constexpr float raster_height_x = 243.0F;

// A camera's sections: CAMERA is header 44..68 then Position, Target, and FOV,
// the Delete row runs 158..180, and TRACKING's header is 184..208. Its first
// row is None; each authored feed is one 22-pixel row four pixels below.
constexpr float camera_position_y = 83.0F;
constexpr float camera_fov_y = 135.0F;
constexpr float tracking_none_y = 301.0F;
constexpr float tracking_first_source_y = 327.0F;
constexpr float tracking_second_source_y = 353.0F;
// The bound feed's own rows follow the list and the "+ New feed" button:
// State, Protocol, Port, Camera ID, then the three lens rows.
constexpr float tracker_state_y = 353.0F;
constexpr float tracker_protocol_y = 379.0F;
constexpr float tracker_port_y = 405.0F;
constexpr float well_x = 150.0F;

const doc::NodeId feed{42U};
const doc::NodeId other_feed{43U};

[[nodiscard]] editor::ElementInfo make_camera(doc::NodeId bound = {}) {
    editor::ElementInfo info{};
    info.node = subject;
    info.kind = editor::ElementKind::Camera;
    info.tracking_source = bound;
    return info;
}

[[nodiscard]] std::vector<scene::TrackingSource> two_feeds() {
    scene::TrackingSource first{};
    first.node = feed;
    first.name = "Stage Tracker";
    first.settings.enabled = true;
    first.settings.port = 6301U;

    scene::TrackingSource second{};
    second.node = other_feed;
    second.name = "Spare";
    return std::vector<scene::TrackingSource>{first, second};
}

[[nodiscard]] editor::ElementInfo make_track() {
    editor::ElementInfo info{};
    info.node = subject;
    info.kind = editor::ElementKind::Track;
    info.display_name = "Front Left";
    info.scale = scene::Vec3{4.0F, 2.25F, 1.0F};
    return info;
}

// A clip on that track. Its own transform still exists in the document; it is
// simply not what the clip renders with while it is on a screen.
[[nodiscard]] editor::ElementInfo make_clip_on_screen() {
    editor::ElementInfo info{};
    info.node = subject;
    info.kind = editor::ElementKind::Media;
    info.timeline_lane = doc::NodeId{9U};
    info.screen_name = "Front Left";
    info.position = scene::Vec3{3.0F, 0.0F, 0.0F};
    return info;
}

[[nodiscard]] editor::ElementInfo make_object() {
    editor::ElementInfo info{};
    info.node = subject;
    info.kind = editor::ElementKind::Object;
    return info;
}

[[nodiscard]] editor::ElementInfo make_point_light() {
    editor::ElementInfo info{};
    info.node = subject;
    info.kind = editor::ElementKind::PointLight;
    info.color = scene::Vec3{1.0F, 1.0F, 1.0F};
    info.intensity = 2.0F;
    info.range = 10.0F;
    return info;
}

// Everything one headless frame needs.
struct Harness final {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    editor::InspectorPanel panel{gui::ui_id("inspector")};
    editor::ElementInfo element{make_object()};
    std::vector<scene::ChannelKeyframes> keyframes;
    std::vector<scene::EffectInstance> effects;
    std::vector<scene::TrackingSource> sources;
    core::u64 show_frame{50U};

    static constexpr gui::Rect bounds{0.0F, 0.0F, 300.0F, 600.0F};

    editor::InspectorFrame frame(
        float x, float y, bool down,
        const gui::Rect& draw_bounds = bounds) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        ctx.new_frame();
        auto result = panel.draw(ctx, draw_bounds, &element, 0U, keyframes, show_frame,
                          effects, sources);
        ctx.draw_overlays(draw_bounds);
        return result;
    }

    // A frame with no pointer movement, carrying typed text and keys - what the
    // field being edited from the keyboard sees.
    editor::InspectorFrame type(std::string_view text, bool enter, bool escape) {
        input.new_frame();
        input.set_mouse_button(gui::MouseButton::Left, false);
        if (!text.empty()) {
            input.add_text(text);
        }
        if (enter) {
            input.add_key(gui::Key::Enter);
        }
        if (escape) {
            input.add_key(gui::Key::Escape);
        }
        ctx.new_frame();
        auto result = panel.draw(ctx, bounds, &element, 0U, keyframes, show_frame,
                          effects, sources);
        ctx.draw_overlays(bounds);
        return result;
    }

    editor::InspectorFrame nudge(gui::Key key,
                                 gui::KeyMods mods = gui::KeyMods::None) {
        input.new_frame();
        input.set_mouse_button(gui::MouseButton::Left, false);
        input.set_mods(mods);
        input.add_key(key);
        ctx.new_frame();
        auto result = panel.draw(ctx, bounds, &element, 0U, keyframes, show_frame,
                          effects, sources);
        ctx.draw_overlays(bounds);
        return result;
    }

    editor::InspectorFrame choose(float y, std::size_t from, std::size_t to) {
        click(well_x,y);
        while(from<to) {static_cast<void>(nudge(gui::Key::Down));++from;}
        while(from>to) {static_cast<void>(nudge(gui::Key::Up));--from;}
        return type("",true,false);
    }

    // Press at a point, without moving: the click that opens a field for typing.
    void click(float x, float y) {
        static_cast<void>(frame(x, y, false));
        static_cast<void>(frame(x, y, true));
        static_cast<void>(frame(x, y, false));
    }
};

// The same headless surface, fed by the canonical selection overload instead
// of the compatibility single-element overload.
struct MultiHarness final {
    gui::InputState input;
    gui::UiDrawList list;
    gui::FontAtlas font{gui::FontAtlas::empty()};
    gui::UiContext ctx{input, list, font};
    editor::InspectorPanel panel{gui::ui_id("multi-inspector")};
    std::vector<editor::ElementInfo> elements;
    doc::NodeId primary{};
    std::vector<scene::ChannelKeyframes> keyframes;
    std::vector<scene::EffectInstance> effects;
    core::u64 show_frame{50U};

    editor::InspectorFrame frame(float x, float y, bool down) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        ctx.new_frame();
        return panel.draw(ctx, Harness::bounds,
                          std::span<const editor::ElementInfo>{elements},
                          primary, keyframes, show_frame, effects);
    }

    editor::InspectorFrame type(std::string_view text, bool enter,
                                bool escape) {
        input.new_frame();
        input.set_mouse_button(gui::MouseButton::Left, false);
        if (!text.empty()) {
            input.add_text(text);
        }
        if (enter) {
            input.add_key(gui::Key::Enter);
        }
        if (escape) {
            input.add_key(gui::Key::Escape);
        }
        ctx.new_frame();
        return panel.draw(ctx, Harness::bounds,
                          std::span<const editor::ElementInfo>{elements},
                          primary, keyframes, show_frame, effects);
    }

    void click(float x, float y) {
        static_cast<void>(frame(x, y, false));
        static_cast<void>(frame(x, y, true));
        static_cast<void>(frame(x, y, false));
    }
};

void a_scrub_emits_the_component_the_field_belongs_to() {
    Harness h;
    static_cast<void>(h.frame(field_x0, position_y, false));
    static_cast<void>(h.frame(field_x0, position_y, true));

    const editor::InspectorFrame dragged =
        h.frame(field_x0 + 50.0F, position_y, true);

    UI_CHECK(dragged.component_edits.size() == 1U);
    const editor::ComponentEdit& edit = dragged.component_edits.front();
    UI_CHECK(edit.node == subject);
    UI_CHECK(edit.component == scene::TransformChannel::PositionX);
    // 50 pixels at the position speed of 0.01 units per pixel.
    UI_CHECK(near(edit.value, 0.5));
    UI_CHECK(dragged.editing);
}

void a_scrub_measures_from_where_it_began_not_from_the_document() {
    Harness h;
    static_cast<void>(h.frame(field_x0, position_y, false));
    static_cast<void>(h.frame(field_x0, position_y, true));

    const editor::InspectorFrame first =
        h.frame(field_x0 + 50.0F, position_y, true);
    UI_CHECK(near(first.component_edits.front().value, 0.5));

    // The document has not caught up - the element still reads zero, exactly as
    // it would several frames into a live drag. The field must keep tracking the
    // pointer from where the press landed rather than restarting from zero.
    const editor::InspectorFrame second =
        h.frame(field_x0 + 100.0F, position_y, true);
    UI_CHECK(second.component_edits.size() == 1U);
    UI_CHECK(near(second.component_edits.front().value, 1.0));

    // And back: a round trip nets out at the value it started from.
    const editor::InspectorFrame back = h.frame(field_x0, position_y, true);
    UI_CHECK(back.component_edits.size() == 1U);
    UI_CHECK(near(back.component_edits.front().value, 0.0));
}

void each_row_and_column_maps_to_its_own_channel() {
    struct Expectation final {
        float x;
        float y;
        scene::TransformChannel component;
    };
    const std::array<Expectation, 5U> expectations{{
        {field_x1, position_y, scene::TransformChannel::PositionY},
        {field_x2, position_y, scene::TransformChannel::PositionZ},
        {field_x0, rotation_y, scene::TransformChannel::RotationYaw},
        {field_x2, rotation_y, scene::TransformChannel::RotationRoll},
        {field_x1, scale_y, scene::TransformChannel::ScaleY},
    }};

    for (const Expectation& expectation : expectations) {
        Harness h;
        static_cast<void>(h.frame(expectation.x, expectation.y, false));
        static_cast<void>(h.frame(expectation.x, expectation.y, true));
        const editor::InspectorFrame dragged =
            h.frame(expectation.x + 40.0F, expectation.y, true);

        UI_CHECK(dragged.component_edits.size() == 1U);
        UI_CHECK(
            dragged.component_edits.front().component == expectation.component);
    }
}

void a_press_that_never_travels_commits_nothing() {
    Harness h;
    static_cast<void>(h.frame(field_x0, position_y, false));
    static_cast<void>(h.frame(field_x0, position_y, true));
    // A single pixel of jitter is not a scrub.
    const editor::InspectorFrame nudged =
        h.frame(field_x0 + 1.0F, position_y, true);
    UI_CHECK(nudged.component_edits.empty());

    const editor::InspectorFrame released =
        h.frame(field_x0 + 1.0F, position_y, false);
    UI_CHECK(released.component_edits.empty());
}

void a_click_opens_the_field_and_typing_commits_an_exact_value() {
    Harness h;
    h.click(field_x0, position_y);

    // The field now has the keyboard, and typing goes into it rather than into
    // the shell's shortcuts.
    const editor::InspectorFrame typed = h.type("2.5", false, false);
    UI_CHECK(typed.component_edits.empty());
    UI_CHECK(h.ctx.typing());

    const editor::InspectorFrame committed = h.type("", true, false);
    UI_CHECK(committed.component_edits.size() == 1U);
    UI_CHECK(
        committed.component_edits.front().component
        == scene::TransformChannel::PositionX);
    UI_CHECK(near(committed.component_edits.front().value, 2.5));
    UI_CHECK(!h.ctx.typing());
}

void escape_abandons_a_typed_edit() {
    Harness h;
    h.click(field_x0, position_y);
    static_cast<void>(h.type("9.75", false, false));

    const editor::InspectorFrame cancelled = h.type("", false, true);
    UI_CHECK(cancelled.component_edits.empty());
    UI_CHECK(!h.ctx.typing());
}

void arrow_nudges_report_semantics_and_track_through_document_latency() {
    Harness h;
    h.click(field_x0, position_y);

    const editor::InspectorFrame first = h.nudge(gui::Key::Up);
    UI_CHECK(first.component_edits.size() == 1U);
    UI_CHECK(near(first.component_edits.front().value, 0.01));
    UI_CHECK(first.numeric_nudge.has_value());
    UI_CHECK(first.numeric_nudge->focus != gui::UiId{});
    UI_CHECK(first.numeric_nudge->property != 0U);
    UI_CHECK(first.numeric_nudge->direction ==
             editor::NumberFieldNudgeDirection::Increase);
    UI_CHECK(first.numeric_nudge->modifiers == gui::KeyMods::None);

    // The supplied element intentionally remains at zero. Repeats still use
    // the local accumulated value rather than crawling on the lagging model.
    const editor::InspectorFrame second = h.nudge(gui::Key::Up);
    UI_CHECK(second.component_edits.size() == 1U);
    UI_CHECK(near(second.component_edits.front().value, 0.02));
    UI_CHECK(second.numeric_nudge->focus == first.numeric_nudge->focus);
    UI_CHECK(second.numeric_nudge->property == first.numeric_nudge->property);

    const editor::InspectorFrame coarse =
        h.nudge(gui::Key::Up, gui::KeyMods::Shift);
    UI_CHECK(near(coarse.component_edits.front().value, 0.12));
    UI_CHECK(coarse.numeric_nudge->modifiers == gui::KeyMods::Shift);

    const editor::InspectorFrame reverse =
        h.nudge(gui::Key::Down, gui::KeyMods::Shift);
    UI_CHECK(near(reverse.component_edits.front().value, 0.02));
    UI_CHECK(reverse.numeric_nudge->direction ==
             editor::NumberFieldNudgeDirection::Decrease);
}

void the_transform_diamond_keys_the_group_at_the_playhead() {
    Harness h;
    h.element.position = scene::Vec3{1.0F, 2.0F, 3.0F};

    // The Position diamond occupies the end of the label column, immediately
    // before the X field.
    static_cast<void>(h.frame(90.0F, position_y, false));
    static_cast<void>(h.frame(90.0F, position_y, true));
    const editor::InspectorFrame keyed = h.frame(90.0F, position_y, false);

    UI_CHECK(keyed.component_edits.empty());
    UI_CHECK(keyed.key_edits.size() == 3U);
    UI_CHECK(keyed.key_edits[0] == editor::InspectorKeyEdit{
                                         subject,
                                         scene::TransformChannel::PositionX,
                                         1.0,
                                         false});
    UI_CHECK(keyed.key_edits[1].component ==
             scene::TransformChannel::PositionY);
    UI_CHECK(near(keyed.key_edits[1].value, 2.0));
    UI_CHECK(keyed.key_edits[2].component ==
             scene::TransformChannel::PositionZ);
    UI_CHECK(near(keyed.key_edits[2].value, 3.0));

    h.keyframes = {
        scene::ChannelKeyframes{
            subject, scene::TransformChannel::PositionX, {50}},
        scene::ChannelKeyframes{
            subject, scene::TransformChannel::PositionY, {50}},
        scene::ChannelKeyframes{
            subject, scene::TransformChannel::PositionZ, {50}},
    };
    static_cast<void>(h.frame(90.0F, position_y, true));
    const editor::InspectorFrame erased = h.frame(90.0F, position_y, false);
    UI_CHECK(erased.key_edits.size() == 3U);
    UI_CHECK(erased.key_edits[0].erase);
    UI_CHECK(erased.key_edits[1].erase);
    UI_CHECK(erased.key_edits[2].erase);
}

void an_animated_property_displays_and_keys_its_evaluated_value() {
    Harness h;
    h.element.position = scene::Vec3{1.0F, 2.0F, 3.0F};
    h.keyframes = {scene::ChannelKeyframes{
        subject,
        scene::TransformChannel::PositionX,
        {0, 100},
        {2.0, 8.0},
        {scene::KeyInterpolation::Linear, scene::KeyInterpolation::Linear}}};

    // At frame 50 the visible X value is 5.0. Capturing the Position group is
    // an observable proof that the same evaluated value feeding the number
    // field is used by the Inspector, rather than the authored base value 1.0.
    static_cast<void>(h.frame(90.0F, position_y, false));
    static_cast<void>(h.frame(90.0F, position_y, true));
    const editor::InspectorFrame keyed = h.frame(90.0F, position_y, false);

    UI_CHECK(keyed.key_edits.size() == 3U);
    UI_CHECK(keyed.key_edits[0].component ==
             scene::TransformChannel::PositionX);
    UI_CHECK(near(keyed.key_edits[0].value, 5.0));
    UI_CHECK(near(keyed.key_edits[1].value, 2.0));
    UI_CHECK(near(keyed.key_edits[2].value, 3.0));
}

void a_light_edits_its_own_named_properties() {
    Harness h;
    h.element = make_point_light();

    static_cast<void>(h.frame(field_x0, light_rgb_y, false));
    static_cast<void>(h.frame(field_x0, light_rgb_y, true));
    const editor::InspectorFrame red =
        h.frame(field_x0 + 20.0F, light_rgb_y, true);

    UI_CHECK(red.component_edits.empty());
    UI_CHECK(red.named_edits.size() == 1U);
    UI_CHECK(red.named_edits.front().node == subject);
    UI_CHECK(red.named_edits.front().property == scene::schema::color_r);

    // And a scalar field below it, to prove the light rows are not all colour.
    Harness intensity;
    intensity.element = make_point_light();
    static_cast<void>(intensity.frame(field_x0, light_intensity_y, false));
    static_cast<void>(intensity.frame(field_x0, light_intensity_y, true));
    const editor::InspectorFrame brighter =
        intensity.frame(field_x0 + 20.0F, light_intensity_y, true);
    UI_CHECK(brighter.named_edits.size() == 1U);
    UI_CHECK(brighter.named_edits.front().property == scene::schema::intensity);
}

void a_collapsed_section_takes_its_fields_with_it() {
    Harness h;
    // Twirl the transform shut.
    static_cast<void>(h.frame(100.0F, section_y, false));
    static_cast<void>(h.frame(100.0F, section_y, true));
    static_cast<void>(h.frame(100.0F, section_y, false));

    // Where Position X was, there is now nothing to grab.
    static_cast<void>(h.frame(field_x1, position_y, false));
    static_cast<void>(h.frame(field_x1, position_y, true));
    const editor::InspectorFrame dragged =
        h.frame(field_x1 + 50.0F, position_y, true);
    UI_CHECK(dragged.component_edits.empty());
    UI_CHECK(!dragged.editing);
}

void an_empty_selection_offers_nothing_to_edit() {
    Harness h;
    h.input.new_frame();
    h.ctx.new_frame();
    const editor::InspectorFrame frame =
        h.panel.draw(h.ctx, Harness::bounds, nullptr);

    UI_CHECK(frame.component_edits.empty());
    UI_CHECK(frame.named_edits.empty());
    UI_CHECK(frame.effect_deletes.empty());
    UI_CHECK(!frame.delete_requested);
    UI_CHECK(!frame.editing);
}

void an_empty_selection_ends_an_edit_in_progress() {
    Harness h;
    h.click(field_x0, position_y);
    UI_CHECK(h.ctx.typing());

    // The operator deleted the element out from under the open field. The panel
    // has to give the keyboard back, or the next Delete keystroke would be
    // swallowed by a field for a node that no longer exists.
    h.input.new_frame();
    h.ctx.new_frame();
    static_cast<void>(h.panel.draw(h.ctx, Harness::bounds, nullptr));
    UI_CHECK(!h.ctx.typing());
}

void the_delete_button_asks_the_shell_to_remove_the_selection() {
    Harness h;
    // The button sits below the transform rows: header, section, three rows, the
    // rotation note, then a gap.
    constexpr float delete_x = 40.0F;
    constexpr float delete_y = 190.0F;
    static_cast<void>(h.frame(delete_x, delete_y, false));
    static_cast<void>(h.frame(delete_x, delete_y, true));
    const editor::InspectorFrame released =
        h.frame(delete_x, delete_y, false);

    UI_CHECK(released.delete_requested);
}

void an_effect_remove_button_requests_true_node_deletion() {
    Harness h;
    const doc::NodeId effect_node{80U};
    h.effects.push_back(scene::EffectInstance{
        effect_node, subject, scene::EffectKind::Saturation, 0, true, 2.0F});

    // The unmistakable REMOVE control is at the right of the effect's title
    // row. It reports the first-class effect identity, not an enabled=false
    // property edit, so the authoring boundary can erase it and its channels.
    constexpr float remove_x = 250.0F;
    constexpr float remove_y = 470.0F;
    static_cast<void>(h.frame(remove_x, remove_y, false));
    static_cast<void>(h.frame(remove_x, remove_y, true));
    const editor::InspectorFrame released =
        h.frame(remove_x, remove_y, false);

    UI_CHECK(released.effect_deletes.size() == 1U);
    UI_CHECK(released.effect_deletes.front() == effect_node);
    UI_CHECK(released.named_edits.empty());
    UI_CHECK(!released.delete_requested);
}

void an_effect_eye_switches_it_off_without_deleting_it() {
    Harness h;
    const doc::NodeId effect_node{82U};
    h.effects.push_back(scene::EffectInstance{
        effect_node, subject, scene::EffectKind::Saturation, 0, true, 2.0F});

    // The eye is at the left of the effect's title row, where a layer list puts
    // it. It authors the document's own enabled flag, so the effect, its
    // parameters, and its key channels all survive being switched off.
    constexpr float eye_x = 15.0F;
    constexpr float eye_y = 470.0F;
    static_cast<void>(h.frame(eye_x, eye_y, false));
    static_cast<void>(h.frame(eye_x, eye_y, true));
    const editor::InspectorFrame off = h.frame(eye_x, eye_y, false);

    UI_CHECK(off.named_edits.size() == 1U);
    UI_CHECK(off.named_edits.front().node == effect_node);
    UI_CHECK(off.named_edits.front().property ==
             scene::schema::effect_enabled);
    UI_CHECK(off.named_edits.front().value == 0.0);
    UI_CHECK(off.effect_deletes.empty());

    // A disabled effect keeps its row and its controls; the same eye turns it
    // back on.
    h.effects.front().enabled = false;
    static_cast<void>(h.frame(eye_x, eye_y, false));
    static_cast<void>(h.frame(eye_x, eye_y, true));
    const editor::InspectorFrame on = h.frame(eye_x, eye_y, false);
    UI_CHECK(on.named_edits.size() == 1U);
    UI_CHECK(on.named_edits.front().value == 1.0);
    UI_CHECK(on.effect_deletes.empty());
}

void an_animated_effect_displays_and_keys_its_evaluated_value() {
    Harness h;
    const doc::NodeId effect_node{81U};
    h.effects.push_back(scene::EffectInstance{
        effect_node, subject, scene::EffectKind::Blur, 0, true, 1.0F});
    h.keyframes = {scene::ChannelKeyframes{
        effect_node,
        scene::TransformChannel::BlurRadius,
        {0, 100},
        {2.0, 8.0},
        {scene::KeyInterpolation::Linear, scene::KeyInterpolation::Linear}}};

    // The Blur radius row follows its title row near the bottom of the panel.
    // At frame 50 its evaluated radius is 5, even though the base is 1.
    constexpr float effect_key_x = 90.0F;
    constexpr float effect_value_y = 493.0F;
    static_cast<void>(h.frame(effect_key_x, effect_value_y, false));
    static_cast<void>(h.frame(effect_key_x, effect_value_y, true));
    const editor::InspectorFrame keyed =
        h.frame(effect_key_x, effect_value_y, false);

    UI_CHECK(keyed.key_edits.size() == 1U);
    UI_CHECK(keyed.key_edits.front().node == effect_node);
    UI_CHECK(keyed.key_edits.front().component ==
             scene::TransformChannel::BlurRadius);
    UI_CHECK(near(keyed.key_edits.front().value, 5.0));
    UI_CHECK(!keyed.key_edits.front().erase);

    Harness scrub;
    scrub.effects = h.effects;
    scrub.keyframes = h.keyframes;
    constexpr float effect_field_x = 150.0F;
    static_cast<void>(scrub.frame(effect_field_x, effect_value_y, false));
    static_cast<void>(scrub.frame(effect_field_x, effect_value_y, true));
    const editor::InspectorFrame changed =
        scrub.frame(effect_field_x + 10.0F, effect_value_y, true);
    UI_CHECK(changed.component_edits.size() == 1U);
    UI_CHECK(changed.component_edits.front().node == effect_node);
    // Blur scrubs at 0.1 per pixel from the evaluated value 5, not base 1.
    UI_CHECK(near(changed.component_edits.front().value, 6.0));
}

void saturation_amount_clamps_and_its_filled_key_erases() {
    const doc::NodeId effect_node{82U};
    Harness keyed_harness;
    keyed_harness.effects.push_back(scene::EffectInstance{
        effect_node, subject, scene::EffectKind::Saturation, 0, true, 1.0F});
    keyed_harness.keyframes = {scene::ChannelKeyframes{
        effect_node,
        scene::PropertyChannel::Saturation,
        {50},
        {3.5},
        {scene::KeyInterpolation::Hold}}};

    constexpr float effect_key_x = 90.0F;
    constexpr float effect_field_x = 150.0F;
    constexpr float effect_value_y = 493.0F;
    static_cast<void>(
        keyed_harness.frame(effect_key_x, effect_value_y, false));
    static_cast<void>(
        keyed_harness.frame(effect_key_x, effect_value_y, true));
    const editor::InspectorFrame erased =
        keyed_harness.frame(effect_key_x, effect_value_y, false);
    UI_CHECK(erased.key_edits.size() == 1U);
    UI_CHECK(erased.key_edits.front().node == effect_node);
    UI_CHECK(erased.key_edits.front().component ==
             scene::PropertyChannel::Saturation);
    UI_CHECK(near(erased.key_edits.front().value, 3.5));
    UI_CHECK(erased.key_edits.front().erase);

    Harness upper;
    upper.effects = keyed_harness.effects;
    upper.keyframes = keyed_harness.keyframes;
    static_cast<void>(upper.frame(effect_field_x, effect_value_y, false));
    static_cast<void>(upper.frame(effect_field_x, effect_value_y, true));
    const editor::InspectorFrame raised =
        upper.frame(effect_field_x + 100.0F, effect_value_y, true);
    UI_CHECK(raised.component_edits.size() == 1U);
    UI_CHECK(raised.component_edits.front().component ==
             scene::PropertyChannel::Saturation);
    UI_CHECK(near(raised.component_edits.front().value, 4.0));

    Harness lower;
    lower.effects = keyed_harness.effects;
    lower.keyframes = keyed_harness.keyframes;
    static_cast<void>(lower.frame(effect_field_x, effect_value_y, false));
    static_cast<void>(lower.frame(effect_field_x, effect_value_y, true));
    const editor::InspectorFrame lowered =
        lower.frame(effect_field_x - 400.0F, effect_value_y, true);
    UI_CHECK(lowered.component_edits.size() == 1U);
    UI_CHECK(lowered.component_edits.front().component ==
             scene::PropertyChannel::Saturation);
    UI_CHECK(near(lowered.component_edits.front().value, 0.0));
}

void film_grain_amount_uses_its_typed_value_and_keyframe_channel() {
    const doc::NodeId effect_node{83U};
    Harness keyed;
    scene::EffectInstance grain{};
    grain.source = effect_node;
    grain.target = subject;
    grain.kind = scene::EffectKind::FilmGrain;
    grain.film_grain.amount = 0.25F;
    // Deliberately leave the generic scalar at its default to prove that the
    // Inspector projects the typed Film Grain amount.
    keyed.effects.push_back(grain);

    constexpr float effect_key_x = 90.0F;
    constexpr float effect_field_x = 150.0F;
    constexpr float effect_value_y = 493.0F;
    static_cast<void>(keyed.frame(effect_key_x, effect_value_y, false));
    static_cast<void>(keyed.frame(effect_key_x, effect_value_y, true));
    const editor::InspectorFrame request =
        keyed.frame(effect_key_x, effect_value_y, false);
    UI_CHECK(request.key_edits.size() == 1U);
    UI_CHECK(request.key_edits.front().node == effect_node);
    UI_CHECK(request.key_edits.front().component ==
             scene::PropertyChannel::FilmGrainAmount);
    UI_CHECK(near(request.key_edits.front().value, 0.25));

    Harness scrub;
    scrub.effects = keyed.effects;
    static_cast<void>(scrub.frame(effect_field_x, effect_value_y, false));
    static_cast<void>(scrub.frame(effect_field_x, effect_value_y, true));
    const editor::InspectorFrame changed =
        scrub.frame(effect_field_x + 100.0F, effect_value_y, true);
    UI_CHECK(changed.component_edits.size() == 1U);
    UI_CHECK(changed.component_edits.front().component ==
             scene::PropertyChannel::FilmGrainAmount);
    UI_CHECK(near(changed.component_edits.front().value, 0.75));
}

void structured_effect_notes_open_their_dedicated_tabs() {
    Harness color;
    scene::EffectInstance levels{};
    levels.source = doc::NodeId{84U};
    levels.target = subject;
    levels.kind = scene::EffectKind::Levels;
    color.effects.push_back(levels);
    constexpr float open_x = 250.0F;
    constexpr float color_open_y = 505.0F;
    static_cast<void>(color.frame(open_x, color_open_y, false));
    static_cast<void>(color.frame(open_x, color_open_y, true));
    const editor::InspectorFrame color_opened =
        color.frame(open_x, color_open_y, false);
    UI_CHECK(color_opened.panel_request ==
             editor::InspectorPanelRequest::Color);

    Harness texture;
    scene::EffectInstance grain{};
    grain.source = doc::NodeId{85U};
    grain.target = subject;
    grain.kind = scene::EffectKind::FilmGrain;
    grain.film_grain.amount = 0.25F;
    texture.effects.push_back(grain);
    constexpr float texture_open_y = 520.0F;
    static_cast<void>(texture.frame(open_x, texture_open_y, false));
    static_cast<void>(texture.frame(open_x, texture_open_y, true));
    const editor::InspectorFrame texture_opened =
        texture.frame(open_x, texture_open_y, false);
    UI_CHECK(texture_opened.panel_request ==
             editor::InspectorPanelRequest::Texture);
}

void chroma_key_exposes_color_tolerance_softness_edge_and_spill() {
    scene::EffectInstance key{};
    key.source = doc::NodeId{86U};
    key.target = subject;
    key.kind = scene::EffectKind::ChromaKey;
    key.chroma_key.key_color = scene::Vec3{0.2F, 0.7F, 0.1F};
    key.chroma_key.tolerance = 0.2F;
    key.chroma_key.softness = 0.3F;
    key.chroma_key.morphology_px = -2;
    key.chroma_key.spill_suppression = 0.4F;

    Harness color;
    color.effects.push_back(key);
    constexpr gui::Rect tall{0.0F, 0.0F, 300.0F, 900.0F};
    constexpr float key_rgb_y = 515.0F;
    static_cast<void>(color.frame(field_x0, key_rgb_y, false, tall));
    static_cast<void>(color.frame(field_x0, key_rgb_y, true, tall));
    const editor::InspectorFrame changed =
        color.frame(field_x0 + 20.0F, key_rgb_y, true, tall);
    UI_CHECK(changed.named_edits.size() == 1U);
    UI_CHECK(changed.named_edits.front().node == key.source);
    UI_CHECK(changed.named_edits.front().property ==
             scene::schema::chroma_key_color_r);
    UI_CHECK(near(changed.named_edits.front().value, 0.3));

    Harness edge;
    edge.effects.push_back(key);
    constexpr float edge_y = 593.0F;
    static_cast<void>(edge.frame(150.0F, edge_y, false, tall));
    static_cast<void>(edge.frame(150.0F, edge_y, true, tall));
    const editor::InspectorFrame grown =
        edge.frame(155.0F, edge_y, true, tall);
    UI_CHECK(grown.named_edits.size() == 1U);
    UI_CHECK(grown.named_edits.front().property ==
             scene::schema::chroma_key_morphology_px);
    UI_CHECK(near(grown.named_edits.front().value, 3.0));

    Harness eyedropper;
    eyedropper.effects.push_back(key);
    // The compact icon button sits immediately beside the colour well.
    constexpr float pick_view_x = 279.0F;
    constexpr float pick_view_y = 492.0F;
    static_cast<void>(eyedropper.frame(pick_view_x, pick_view_y, false, tall));
    static_cast<void>(eyedropper.frame(pick_view_x, pick_view_y, true, tall));
    const editor::InspectorFrame requested =
        eyedropper.frame(pick_view_x, pick_view_y, false, tall);
    UI_CHECK(requested.color_sample_request.has_value());
    UI_CHECK(requested.color_sample_request->node == key.source);
    UI_CHECK(requested.color_sample_request->properties[0] ==
             scene::schema::chroma_key_color_r);
    UI_CHECK(requested.color_sample_request->properties[1] ==
             scene::schema::chroma_key_color_g);
    UI_CHECK(requested.color_sample_request->properties[2] ==
             scene::schema::chroma_key_color_b);
}

void luma_key_exposes_threshold_softness_invert_and_mix() {
    Harness h;
    scene::EffectInstance key{};
    key.source = doc::NodeId{87U};
    key.target = subject;
    key.kind = scene::EffectKind::LumaKey;
    key.luma_key.low_threshold = 0.2F;
    key.luma_key.high_threshold = 0.8F;
    key.luma_key.low_softness = 0.1F;
    key.luma_key.high_softness = 0.15F;
    h.effects.push_back(key);
    constexpr gui::Rect tall{0.0F, 0.0F, 300.0F, 720.0F};
    constexpr float invert_y = 597.0F;
    static_cast<void>(h.frame(180.0F, invert_y, false, tall));
    static_cast<void>(h.frame(180.0F, invert_y, true, tall));
    const editor::InspectorFrame inverted =
        h.frame(180.0F, invert_y, false, tall);
    UI_CHECK(inverted.named_edits.size() == 1U);
    UI_CHECK(inverted.named_edits.front().node == key.source);
    UI_CHECK(inverted.named_edits.front().property ==
             scene::schema::luma_key_invert);
    UI_CHECK(near(inverted.named_edits.front().value, 1.0));
}

void multi_selection_reports_equal_values_and_mixed_values_truthfully() {
    MultiHarness equal;
    editor::ElementInfo first = make_object();
    first.position.x = 3.0F;
    editor::ElementInfo second = make_object();
    second.node = doc::NodeId{6U};
    second.position.x = 3.0F;
    equal.elements = {first, second};
    equal.primary = first.node;

    const editor::InspectorFrame common =
        equal.frame(0.0F, 0.0F, false);
    UI_CHECK(common.mixed_fields == 0U);

    equal.elements[1].position.x = 8.0F;
    const editor::InspectorFrame mixed =
        equal.frame(0.0F, 0.0F, false);
    // Position X is the only disagreeing editable value. NumberField receives
    // this state and draws the literal Mixed instead of either node's number.
    UI_CHECK(mixed.mixed_fields == 1U);
    UI_CHECK(mixed.component_edits.empty());

    MultiHarness content;
    first.kind = editor::ElementKind::Media;
    first.position.x = 0.0F;
    first.source = "first.mov";
    first.sequence_start = 1U;
    second.kind = editor::ElementKind::Media;
    second.position.x = 0.0F;
    second.source = "second.mov";
    second.sequence_start = 2U;
    content.elements = {first, second};
    content.primary = first.node;
    const editor::InspectorFrame content_mixed =
        content.frame(0.0F, 0.0F, false);
    UI_CHECK(content_mixed.mixed_fields == 2U);
}

void a_typed_absolute_value_fans_out_even_when_it_matches_the_primary() {
    MultiHarness h;
    editor::ElementInfo first = make_object();
    first.position.x = 2.5F;
    editor::ElementInfo second = make_object();
    second.node = doc::NodeId{6U};
    second.position.x = 9.0F;
    h.elements = {first, second};
    h.primary = first.node;

    h.click(field_x0, position_y);
    static_cast<void>(h.type("2.5", false, false));
    const editor::InspectorFrame committed = h.type("", true, false);

    UI_CHECK(committed.component_edits.size() == 2U);
    UI_CHECK(committed.component_edits[0] == editor::ComponentEdit{
                                                  first.node,
                                                  scene::TransformChannel::PositionX,
                                                  2.5});
    UI_CHECK(committed.component_edits[1] == editor::ComponentEdit{
                                                  second.node,
                                                  scene::TransformChannel::PositionX,
                                                  2.5});
}

void a_multi_scrub_applies_one_delta_to_each_captured_start() {
    MultiHarness h;
    editor::ElementInfo first = make_object();
    first.position.x = 1.0F;
    editor::ElementInfo second = make_object();
    second.node = doc::NodeId{6U};
    second.position.x = 10.0F;
    h.elements = {first, second};
    h.primary = first.node;

    static_cast<void>(h.frame(field_x0, position_y, false));
    static_cast<void>(h.frame(field_x0, position_y, true));
    const editor::InspectorFrame first_move =
        h.frame(field_x0 + 50.0F, position_y, true);
    UI_CHECK(first_move.component_edits.size() == 2U);
    UI_CHECK(near(first_move.component_edits[0].value, 1.5));
    UI_CHECK(near(first_move.component_edits[1].value, 10.5));

    // Even a wildly different document snapshot cannot rebase either member
    // while this pointer gesture is still live.
    h.elements[0].position.x = 100.0F;
    h.elements[1].position.x = 200.0F;
    const editor::InspectorFrame second_move =
        h.frame(field_x0 + 100.0F, position_y, true);
    UI_CHECK(second_move.component_edits.size() == 2U);
    UI_CHECK(near(second_move.component_edits[0].value, 2.0));
    UI_CHECK(near(second_move.component_edits[1].value, 11.0));
}

void locked_and_incompatible_targets_are_listed_and_skipped() {
    MultiHarness h;
    editor::ElementInfo object = make_object();
    object.position.x = 1.0F;
    editor::ElementInfo media = make_object();
    media.node = doc::NodeId{6U};
    media.kind = editor::ElementKind::Media;
    media.position.x = 4.0F;
    editor::ElementInfo locked = make_object();
    locked.node = doc::NodeId{7U};
    locked.locked = true;
    editor::ElementInfo light = make_point_light();
    light.node = doc::NodeId{8U};
    h.elements = {object, media, locked, light};
    h.primary = object.node;

    static_cast<void>(h.frame(field_x0, position_y, false));
    static_cast<void>(h.frame(field_x0, position_y, true));
    const editor::InspectorFrame changed =
        h.frame(field_x0 + 10.0F, position_y, true);

    UI_CHECK(changed.component_edits.size() == 2U);
    UI_CHECK(changed.component_edits[0].node == object.node);
    UI_CHECK(near(changed.component_edits[0].value, 1.1));
    UI_CHECK(changed.component_edits[1].node == media.node);
    UI_CHECK(near(changed.component_edits[1].value, 4.1));
    UI_CHECK(changed.skipped_locked ==
             std::vector<doc::NodeId>{locked.node});
    UI_CHECK(changed.skipped_incompatible ==
             std::vector<doc::NodeId>{light.node});
}

void named_and_effect_fields_share_the_multi_edit_contract() {
    constexpr float roughness_y = 287.0F;
    MultiHarness material;
    editor::ElementInfo first = make_object();
    first.material.roughness = 0.2F;
    editor::ElementInfo second = make_object();
    second.node = doc::NodeId{6U};
    second.kind = editor::ElementKind::Model;
    second.material.roughness = 0.7F;
    material.elements = {first, second};
    material.primary = first.node;
    static_cast<void>(material.frame(150.0F, roughness_y, false));
    static_cast<void>(material.frame(150.0F, roughness_y, true));
    const editor::InspectorFrame material_edit =
        material.frame(170.0F, roughness_y, true);
    UI_CHECK(material_edit.named_edits.size() == 2U);
    UI_CHECK(material_edit.named_edits[0].property == scene::schema::roughness);
    UI_CHECK(near(material_edit.named_edits[0].value, 0.3));
    UI_CHECK(near(material_edit.named_edits[1].value, 0.8));

    constexpr float effect_value_y = 493.0F;
    MultiHarness effect;
    effect.elements = {first, second};
    effect.primary = first.node;
    effect.effects = {
        scene::EffectInstance{doc::NodeId{80U}, first.node,
                              scene::EffectKind::Brightness, 0, true, 2.0F},
        scene::EffectInstance{doc::NodeId{81U}, second.node,
                              scene::EffectKind::Brightness, 0, true, 3.0F}};
    static_cast<void>(effect.frame(150.0F, effect_value_y, false));
    static_cast<void>(effect.frame(150.0F, effect_value_y, true));
    const editor::InspectorFrame effect_edit =
        effect.frame(160.0F, effect_value_y, true);
    UI_CHECK(effect_edit.component_edits.size() == 2U);
    UI_CHECK(effect_edit.component_edits[0].node == doc::NodeId{80U});
    UI_CHECK(near(effect_edit.component_edits[0].value, 2.1));
    UI_CHECK(effect_edit.component_edits[1].node == doc::NodeId{81U});
    UI_CHECK(near(effect_edit.component_edits[1].value, 3.1));
}

struct Case final {
    const char* name;
    void (*function)();
};

void a_track_inspects_the_screen_it_owns() {
    Harness h;
    h.element = make_track();

    // The raster is whole pixels and scrubs one pixel per pixel.
    static_cast<void>(h.frame(raster_width_x, screen_raster_y, false));
    static_cast<void>(h.frame(raster_width_x, screen_raster_y, true));
    const editor::InspectorFrame dragged =
        h.frame(raster_width_x + 40.0F, screen_raster_y, true);
    UI_CHECK(dragged.named_edits.size() == 1U);
    UI_CHECK(dragged.named_edits.front().node == subject);
    UI_CHECK(dragged.named_edits.front().property
             == scene::schema::lane_raster_width);
    UI_CHECK(near(dragged.named_edits.front().value, 1'960.0));

    // Half a pixel of travel is no pixels, not half a pixel: a raster that
    // rounded at the document boundary instead would show 1920.4 in the field.
    Harness fractional;
    fractional.element = make_track();
    static_cast<void>(
        fractional.frame(raster_height_x, screen_raster_y, false));
    static_cast<void>(fractional.frame(raster_height_x, screen_raster_y, true));
    const editor::InspectorFrame nudged =
        fractional.frame(raster_height_x + 0.4F, screen_raster_y, true);
    UI_CHECK(nudged.named_edits.empty()
             || near(nudged.named_edits.front().value, 1'080.0));
}

void the_aspect_lock_is_one_control_on_the_track() {
    Harness h;
    h.element = make_track();
    static_cast<void>(h.frame(raster_width_x, screen_aspect_y, false));
    static_cast<void>(h.frame(raster_width_x, screen_aspect_y, true));
    const editor::InspectorFrame clicked =
        h.frame(raster_width_x, screen_aspect_y, false);
    UI_CHECK(clicked.named_edits.size() == 1U);
    UI_CHECK(clicked.named_edits.front().property
             == scene::schema::lane_aspect_locked);
    // It reports the state being moved to, so a locked screen opens.
    UI_CHECK(near(clicked.named_edits.front().value, 0.0));
}

void a_clip_on_a_screen_offers_no_transform_of_its_own() {
    Harness h;
    h.element = make_clip_on_screen();

    // Where a transform field would be there is a line naming the screen
    // instead. Scrubbing it moves nothing, because the clip does not have a
    // place in the scene - the track it plays on does.
    static_cast<void>(h.frame(field_x0, position_y, false));
    static_cast<void>(h.frame(field_x0, position_y, true));
    const editor::InspectorFrame dragged =
        h.frame(field_x0 + 50.0F, position_y, true);
    UI_CHECK(dragged.component_edits.empty());

    // A clip outside every track still has one, and it still works.
    Harness loose;
    loose.element = make_clip_on_screen();
    loose.element.timeline_lane = doc::NodeId{};
    loose.element.screen_name.clear();
    static_cast<void>(loose.frame(field_x0, position_y, false));
    static_cast<void>(loose.frame(field_x0, position_y, true));
    const editor::InspectorFrame moved =
        loose.frame(field_x0 + 50.0F, position_y, true);
    UI_CHECK(moved.component_edits.size() == 1U);
    UI_CHECK(moved.component_edits.front().component
             == scene::TransformChannel::PositionX);
}

// A camera is placed and aimed. It reads none of the transform properties
// every other element uses, so offering them would be nine fields that move
// nothing - the same reason a clip on a screen is shown none.
void a_camera_is_placed_and_aimed_rather_than_transformed() {
    Harness h;
    h.element = make_camera();
    h.element.position = scene::Vec3{0.0F, 1.6F, 3.0F};

    h.click(field_x0, camera_position_y);
    static_cast<void>(h.type("4.5", false, false));
    const editor::InspectorFrame moved = h.type("", true, false);
    UI_CHECK(moved.component_edits.empty());
    UI_CHECK(moved.named_edits.size() == 1U);
    UI_CHECK(moved.named_edits.front().property == scene::schema::eye_x);
    UI_CHECK(near(moved.named_edits.front().value, 4.5));

    // An angle is a fraction of a turn, and a camera that sees nothing is not
    // a camera: the field itself refuses zero rather than the document.
    Harness angle;
    angle.element = make_camera();
    angle.click(field_x0, camera_fov_y);
    static_cast<void>(angle.type("0", false, false));
    const editor::InspectorFrame narrowed = angle.type("", true, false);
    UI_CHECK(narrowed.named_edits.size() == 1U);
    UI_CHECK(narrowed.named_edits.front().property
             == scene::schema::fov_turns);
    UI_CHECK(narrowed.named_edits.front().value > 0.0);
    Harness shutter;
    shutter.element = make_camera();
    shutter.element.shutter_seconds = 0.03125F;
    shutter.click(field_x0, camera_fov_y + 26.0F);
    static_cast<void>(shutter.type("0.010416667", false, false));
    const auto exposure = shutter.type("", true, false);
    UI_CHECK(exposure.named_edits.size() == 1U);
    UI_CHECK(exposure.named_edits.front().property == scene::schema::shutter_seconds);
    UI_CHECK(std::abs(exposure.named_edits.front().value - 0.010416667) < 1.0e-9);
    for (bool focus : {true, false}) {
        Harness lens; lens.element = make_camera();
        lens.click(field_x0, camera_fov_y + (focus ? 52.0F : 78.0F));
        static_cast<void>(lens.type(focus ? "4.75" : "1.8", false, false));
        const auto edited = lens.type("", true, false);
        UI_CHECK(edited.named_edits.size() == 1U);
        UI_CHECK(edited.named_edits[0].property == (focus ? scene::schema::focus_distance_m : scene::schema::aperture_f_number));
        UI_CHECK(std::abs(edited.named_edits[0].value - (focus ? 4.75 : 1.8)) < 1e-8);
    }
}

void a_camera_binds_the_feed_the_operator_picks() {
    Harness h;h.element=make_camera();h.sources=two_feeds();
    auto first=h.choose(tracking_none_y,0,1);
    UI_CHECK(first.named_edits.size()==1 && first.named_edits[0].node==subject);
    UI_CHECK(first.named_edits[0].property==scene::schema::camera_tracking_source && first.named_edits[0].value==static_cast<double>(feed.value()));
    auto second=h.choose(tracking_none_y,0,2);
    UI_CHECK(second.named_edits.size()==1 && second.named_edits[0].value==static_cast<double>(other_feed.value()));
}

void the_chosen_feed_is_the_only_row_that_authors_nothing() {
    Harness h;h.element=make_camera(feed);h.sources=two_feeds();
    UI_CHECK(h.choose(tracking_none_y,1,1).named_edits.empty());
    const auto none=h.choose(tracking_none_y,1,0);
    UI_CHECK(none.named_edits.size()==1 && none.named_edits[0].property==scene::schema::camera_tracking_source && none.named_edits[0].value==0.0);
}

// The bound feed's own settings sit under the camera that uses it, and the
// edits are addressed to the feed's node while the selection stays the camera.
// That is the whole point of putting them here: an operator setting a port is
// looking at the camera it feeds, not hunting for a separate object.
void a_bound_feed_is_configured_where_it_is_chosen() {
    Harness state;
    state.element = make_camera(feed);
    state.sources = two_feeds();
    static_cast<void>(state.frame(well_x, tracker_state_y, false));
    static_cast<void>(state.frame(well_x, tracker_state_y, true));
    const editor::InspectorFrame listening =
        state.frame(well_x, tracker_state_y, false);
    UI_CHECK(listening.component_edits.empty());
    UI_CHECK(listening.named_edits.size() == 1U);
    UI_CHECK(listening.named_edits.front().node == feed);
    UI_CHECK(listening.named_edits.front().property
             == scene::schema::tracking_enabled);
    // The first feed is authored on, so this switches it off.
    UI_CHECK(near(listening.named_edits.front().value, 0.0));

    Harness protocol;
    protocol.element = make_camera(feed);
    protocol.sources = two_feeds();
    const editor::InspectorFrame swapped = protocol.choose(tracker_protocol_y,0,1);
    UI_CHECK(swapped.named_edits.size() == 1U);
    UI_CHECK(swapped.named_edits.front().node == feed);
    UI_CHECK(swapped.named_edits.front().property
             == scene::schema::tracking_protocol);
    UI_CHECK(near(swapped.named_edits.front().value, 1.0));

    // An unbound camera offers no feed settings at all, so the rows below the
    // list are the camera's own: nothing here can edit a feed that is not
    // bound, and nothing addresses an edit to a node the camera does not name.
    Harness unbound;
    unbound.element = make_camera();
    unbound.sources = two_feeds();
    static_cast<void>(unbound.frame(well_x, tracker_state_y, false));
    static_cast<void>(unbound.frame(well_x, tracker_state_y, true));
    const editor::InspectorFrame nothing =
        unbound.frame(well_x, tracker_state_y, false);
    for (const editor::NamedEdit& edit : nothing.named_edits) {
        UI_CHECK(edit.node == subject);
        UI_CHECK(edit.property != scene::schema::tracking_enabled);
        UI_CHECK(edit.property != scene::schema::tracking_protocol);
    }
}

void a_port_is_a_whole_number_inside_its_own_range() {
    Harness h;
    h.element = make_camera(feed);
    h.sources = two_feeds();
    h.click(field_x0, tracker_port_y);
    static_cast<void>(h.type("70000.5", false, false));
    const editor::InspectorFrame typed = h.type("", true, false);
    UI_CHECK(typed.named_edits.size() == 1U);
    UI_CHECK(typed.named_edits.front().node == feed);
    UI_CHECK(typed.named_edits.front().property
             == scene::schema::tracking_port);
    UI_CHECK(near(typed.named_edits.front().value, 65535.0));
}

// A spot light can be authored from the Inspector, and its controls name the
// properties the parser actually reads.
//
// This test exists because the feature shipped without it. The scene model,
// the parser, the shader and the shadow atlas all understood spots, and there
// was still no way to make one - no Add entry, no kind selector, no cone rows.
// Every test upstream of this built its spot by filling in a C++ struct, which
// is precisely the step an operator cannot take.
//
// The property *names* are the assertion that matters. An edit named something
// the parser does not read is a control that moves and does nothing, which is
// the failure this whole area kept producing.
void a_spot_is_authorable_from_the_inspector() {
    // The spot rows follow Range at the shared pitch: Kind, Aim, the two cone
    // angles, then the shadow controls.
    constexpr float kind_y = light_range_y + light_row_pitch;
    constexpr float shadow_y = kind_y + light_row_pitch * 4.0F;
    constexpr float tier_y = shadow_y + light_row_pitch;
    // The right-hand end of the field area, which is the last option of a row.
    constexpr float last_option_x = field_x1 + 60.0F;

    // Presses a button row and returns the frame the release produced.
    const auto press = [](Harness& harness, float x, float y) {
        static_cast<void>(harness.frame(x, y, false));
        static_cast<void>(harness.frame(x, y, true));
        return harness.frame(x, y, false);
    };

    // A point light offers the kind switch, and choosing SPOT is a property
    // edit on the same node - point and spot are one node type with a
    // different `kind`, so switching must not require a different element.
    Harness point;
    point.element = make_point_light();
    point.element.local_kind = scene::LocalLightKind::Point;
    const editor::InspectorFrame switched = point.choose(kind_y,0,1);
    UI_CHECK(switched.named_edits.size() == 1U);
    UI_CHECK(switched.named_edits.front().node == subject);
    UI_CHECK(switched.named_edits.front().property
             == scene::schema::light_kind);
    // One rather than zero: the server turns the index into the kind name, so
    // a spot must not be the falsy option.
    UI_CHECK(switched.named_edits.front().value == 1.0);

    // A point light draws no shadow controls. Other contextual controls may
    // occupy those coordinates, but must never author shadow properties.
    Harness bare;
    bare.element = make_point_light();
    bare.element.local_kind = scene::LocalLightKind::Point;
    for (const float y : {shadow_y, tier_y}) {
        for (const auto& edit : press(bare, last_option_x, y).named_edits) {
            UI_CHECK(edit.property != scene::schema::casts_shadow && edit.property != scene::schema::shadow_tier);
        }
    }

    // A spot draws them, and they name the properties the parser reads. An
    // edit named anything else is a control that moves and does nothing, which
    // is the failure this whole area kept producing.
    Harness spot;
    spot.element = make_point_light();
    spot.element.local_kind = scene::LocalLightKind::Spot;
    spot.element.casts_shadow = false;
    const editor::InspectorFrame cast = press(spot, last_option_x, shadow_y);
    UI_CHECK(cast.named_edits.size() == 1U);
    UI_CHECK(cast.named_edits.front().property == scene::schema::casts_shadow);
    UI_CHECK(cast.named_edits.front().value == 1.0);

    // The tier appears only once the light actually casts: a resolution for a
    // shadow that is switched off is a control with nothing behind it.
    Harness quiet;
    quiet.element = make_point_light();
    quiet.element.local_kind = scene::LocalLightKind::Spot;
    quiet.element.casts_shadow = false;
    UI_CHECK(press(quiet, last_option_x, tier_y).named_edits.empty());

    Harness casting;
    casting.element = make_point_light();
    casting.element.local_kind = scene::LocalLightKind::Spot;
    casting.element.casts_shadow = true;
    casting.element.shadow_tier = scene::ShadowTier::Medium;
    const editor::InspectorFrame tier = casting.choose(tier_y,1,2);
    UI_CHECK(tier.named_edits.size() == 1U);
    UI_CHECK(tier.named_edits.front().property == scene::schema::shadow_tier);
    // The rightmost of three is Large, and the server maps two onto it.
    UI_CHECK(tier.named_edits.front().value == 2.0);

    // Choosing the option already active emits nothing, so a click that
    // changes nothing does not author an undo step.
    Harness already;
    already.element = make_point_light();
    already.element.local_kind = scene::LocalLightKind::Spot;
    already.element.casts_shadow = true;
    already.element.shadow_tier = scene::ShadowTier::Large;
    UI_CHECK(already.choose(tier_y,2,2).named_edits.empty());
}

void area_lights_are_authorable_from_the_inspector() {
    constexpr float kind_y=light_range_y+light_row_pitch;
    const auto press=[](Harness& h,float x,float y) {
        static_cast<void>(h.frame(x,y,false));static_cast<void>(h.frame(x,y,true));return h.frame(x,y,false);
    };
    for (unsigned kind=2;kind<4;++kind) {
        Harness h;h.element=make_point_light();
        const auto chosen=h.choose(kind_y,0,kind);
        UI_CHECK(chosen.named_edits.size()==1 && chosen.named_edits[0].property==scene::schema::light_kind);
        UI_CHECK(chosen.named_edits[0].value==kind);
    }
    for (unsigned field=0;field<4;++field) {
        Harness h;h.element=make_point_light();
        h.element.local_kind=field==3?scene::LocalLightKind::Sphere:scene::LocalLightKind::Rectangle;
        h.element.light_width=3.2F;h.element.light_height=1.7F;h.element.area_rotation=-0.1F;h.element.light_radius=0.8F;
        const float y=kind_y+light_row_pitch*static_cast<float>(field==3?1:field+1);
        h.click(140,y);h.click(140,y);static_cast<void>(h.type("0.65",false,false));const auto edited=h.type("",true,false);
        UI_CHECK(edited.named_edits.size()==1);
        UI_CHECK(edited.named_edits[0].property==(field==0?scene::schema::light_width:field==1?scene::schema::light_height:field==2?scene::schema::area_rotation_turns:scene::schema::light_radius));
        UI_CHECK(near(edited.named_edits[0].value,0.65));
    }
    Harness h;h.element=make_point_light();h.element.local_kind=scene::LocalLightKind::Rectangle;
    const auto sided=press(h,250,kind_y+4*light_row_pitch);
    UI_CHECK(sided.named_edits.size()==1 && sided.named_edits[0].property==scene::schema::two_sided && sided.named_edits[0].value==1);
    const auto units=h.choose(light_intensity_y+light_row_pitch,0,2);
    UI_CHECK(units.named_edits.size()==1 && units.named_edits[0].property==scene::schema::light_unit && units.named_edits[0].value==4);
}

void ies_profile_path_apply_clear_and_rotation() {
    const auto press = [](Harness& h, float x, float y) {
        static_cast<void>(h.frame(x, y, false));
        static_cast<void>(h.frame(x, y, true));
        return h.frame(x, y, false);
    };
    Harness h; h.element = make_point_light();
    h.click(130.0F, 304.0F);
    static_cast<void>(h.type("C:/fixtures/practical.ies", false, false));
    static_cast<void>(h.type("", true, false));
    auto applied = press(h, 70.0F, 332.0F);
    UI_CHECK(applied.text_edits.size() == 1);
    UI_CHECK(applied.text_edits[0].node == subject);
    UI_CHECK(applied.text_edits[0].property == scene::schema::ies_source);
    UI_CHECK(applied.text_edits[0].value == "C:/fixtures/practical.ies");
    h.element.ies_source = applied.text_edits[0].value;
    auto clear = press(h, 225.0F, 332.0F);
    UI_CHECK(clear.text_edits.size() == 1 && clear.text_edits[0].value.empty());
    h.click(150.0F, 355.0F);
    static_cast<void>(h.type("0.25", false, false));
    auto rotation = h.type("", true, false);
    UI_CHECK(rotation.named_edits.size() == 1);
    UI_CHECK(rotation.named_edits[0].property == scene::schema::ies_rotation_turns);
    UI_CHECK(rotation.named_edits[0].value == 0.25);
}

void photometric_controls_and_spot_fields_author_real_values() {
    const auto press = [](Harness& h, float x, float y) {
        static_cast<void>(h.frame(x, y, false));
        static_cast<void>(h.frame(x, y, true));
        return h.frame(x, y, false);
    };
    Harness h; h.element = make_point_light();
    const float units_y = light_intensity_y + light_row_pitch;
    auto edit = h.choose(units_y,0,1);
    UI_CHECK(edit.named_edits.size() == 1U);
    UI_CHECK(edit.named_edits[0].property == scene::schema::light_unit);
    UI_CHECK(edit.named_edits[0].value == static_cast<double>(scene::LightUnit::Lumens));
    edit = h.choose(units_y,0,2);
    UI_CHECK(edit.named_edits.size() == 1U);
    UI_CHECK(edit.named_edits[0].value == static_cast<double>(scene::LightUnit::Candela));
    h.element.kind = editor::ElementKind::DirectionalLight;
    edit = h.choose(units_y,0,1);
    UI_CHECK(edit.named_edits.size() == 1U);
    UI_CHECK(edit.named_edits[0].value == static_cast<double>(scene::LightUnit::Lux));
    edit = press(h, 250.0F, units_y + light_row_pitch);
    UI_CHECK(edit.named_edits.size() == 1U);
    UI_CHECK(edit.named_edits[0].property == scene::schema::use_color_temperature);
    UI_CHECK(edit.named_edits[0].value == 1);
    h.element.use_color_temperature = true;
    h.click(field_x0, units_y + 2 * light_row_pitch);
    static_cast<void>(h.type("3200", false, false));
    edit = h.type("", true, false);
    UI_CHECK(edit.named_edits.size() == 1U);
    UI_CHECK(edit.named_edits[0].property == scene::schema::color_temperature_kelvin);
    UI_CHECK(edit.named_edits[0].value == 3200);
    h.click(field_x0, units_y + 4 * light_row_pitch);
    static_cast<void>(h.type("0.003", false, false));
    edit = h.type("", true, false);
    UI_CHECK(edit.named_edits.size() == 1U);
    UI_CHECK(edit.named_edits[0].property == scene::schema::angular_radius_turns);
    UI_CHECK(std::abs(edit.named_edits[0].value - 0.003) < 1.0e-6);
    for (unsigned field = 0; field < 3; ++field) {
        Harness spot; spot.element = make_point_light();
        spot.element.local_kind = scene::LocalLightKind::Spot;
        const float y = light_range_y + light_row_pitch * (2.0F + static_cast<float>(field));
        spot.click(field_x0, y);
        static_cast<void>(spot.type(field == 0 ? "0.5" : "0.08", false, false));
        edit = spot.type("", true, false);
        UI_CHECK(edit.named_edits.size() == 1U);
        UI_CHECK(edit.named_edits[0].property == (field == 0 ? scene::schema::direction_x
            : field == 1 ? scene::schema::inner_cone_turns : scene::schema::outer_cone_turns));
    }
}

void model_animation_buttons_preserve_exact_start_frame() {
    bool enabled=false,loop=false,start=false;
    constexpr core::u64 exact_frame=9007199254740993ULL;
    const gui::Rect tall{0,0,300,1800};
    // Discover clickable rows through actual emitted edits, independent of
    // the expanded material/content sections above this model-only control.
    for (float y=10;y<1700 && !(enabled && loop && start);y+=4) {
        Harness h; h.element.kind=editor::ElementKind::Model; h.show_frame=exact_frame;
        static_cast<void>(h.frame(150,y,false,tall)); static_cast<void>(h.frame(150,y,true,tall));
        const auto result=h.frame(150,y,false,tall);
        for (const auto& edit:result.named_edits) {
            if (edit.property==scene::schema::animation_enabled) { UI_CHECK(edit.value==0 && edit.node==subject); enabled=true; }
            if (edit.property==scene::schema::animation_loop) { UI_CHECK(edit.value==0 && edit.node==subject); loop=true; }
        }
        for (const auto& edit:result.text_edits) if (edit.property==scene::schema::animation_start_frame && edit.value!="0") {
            UI_CHECK(edit.value=="9007199254740993" && edit.node==subject); start=true;
        }
    }
    UI_CHECK(enabled && loop && start);
}

constexpr std::array<Case, 40U> cases{{
    {"model animation controls preserve the exact show frame", &model_animation_buttons_preserve_exact_start_frame},
    {"area light shapes, dimensions, units and sides", &area_lights_are_authorable_from_the_inspector},
    {"IES path, apply, clear and rotation", &ies_profile_path_apply_clear_and_rotation},
    {"photometric controls and spot fields author real values", &photometric_controls_and_spot_fields_author_real_values},
    {"a camera is placed and aimed rather than transformed",
     &a_camera_is_placed_and_aimed_rather_than_transformed},
    {"a camera binds the feed the operator picks",
     &a_camera_binds_the_feed_the_operator_picks},
    {"the chosen feed is the only row that authors nothing",
     &the_chosen_feed_is_the_only_row_that_authors_nothing},
    {"a bound feed is configured where it is chosen",
     &a_bound_feed_is_configured_where_it_is_chosen},
    {"a port is a whole number inside its own range",
     &a_port_is_a_whole_number_inside_its_own_range},
    {"a track inspects the screen it owns",
     &a_track_inspects_the_screen_it_owns},
    {"the aspect lock is one control on the track",
     &the_aspect_lock_is_one_control_on_the_track},
    {"a clip on a screen offers no transform of its own",
     &a_clip_on_a_screen_offers_no_transform_of_its_own},
    {"a scrub emits the component the field belongs to",
     &a_scrub_emits_the_component_the_field_belongs_to},
    {"a scrub measures from where it began not from the document",
     &a_scrub_measures_from_where_it_began_not_from_the_document},
    {"each row and column maps to its own channel",
     &each_row_and_column_maps_to_its_own_channel},
    {"a press that never travels commits nothing",
     &a_press_that_never_travels_commits_nothing},
    {"a click opens the field and typing commits an exact value",
     &a_click_opens_the_field_and_typing_commits_an_exact_value},
    {"escape abandons a typed edit", &escape_abandons_a_typed_edit},
    {"arrow nudges report semantics and track through document latency",
     &arrow_nudges_report_semantics_and_track_through_document_latency},
    {"the transform diamond keys the group at the playhead",
     &the_transform_diamond_keys_the_group_at_the_playhead},
    {"an animated property displays and keys its evaluated value",
     &an_animated_property_displays_and_keys_its_evaluated_value},
    {"a light edits its own named properties",
     &a_light_edits_its_own_named_properties},
    {"a spot is authorable from the inspector",
     &a_spot_is_authorable_from_the_inspector},
    {"a collapsed section takes its fields with it",
     &a_collapsed_section_takes_its_fields_with_it},
    {"an empty selection offers nothing to edit",
     &an_empty_selection_offers_nothing_to_edit},
    {"an empty selection ends an edit in progress",
     &an_empty_selection_ends_an_edit_in_progress},
    {"the delete button asks the shell to remove the selection",
     &the_delete_button_asks_the_shell_to_remove_the_selection},
    {"an effect remove button requests true node deletion",
     &an_effect_remove_button_requests_true_node_deletion},
    {"an effect eye switches it off without deleting it",
     &an_effect_eye_switches_it_off_without_deleting_it},
    {"an animated effect displays and keys its evaluated value",
     &an_animated_effect_displays_and_keys_its_evaluated_value},
    {"Saturation amount clamps and its filled key erases",
     &saturation_amount_clamps_and_its_filled_key_erases},
    {"Film Grain amount uses its typed value and keyframe channel",
     &film_grain_amount_uses_its_typed_value_and_keyframe_channel},
    {"structured effect notes open their dedicated tabs",
     &structured_effect_notes_open_their_dedicated_tabs},
    {"Chroma Key exposes color tolerance softness edge and spill",
     &chroma_key_exposes_color_tolerance_softness_edge_and_spill},
    {"Luma Key exposes threshold softness invert and Mix",
     &luma_key_exposes_threshold_softness_invert_and_mix},
    {"multi-selection reports equal values and Mixed truthfully",
     &multi_selection_reports_equal_values_and_mixed_values_truthfully},
    {"a typed absolute value fans out even when it matches the primary",
     &a_typed_absolute_value_fans_out_even_when_it_matches_the_primary},
    {"a multi scrub applies one delta to each captured start",
     &a_multi_scrub_applies_one_delta_to_each_captured_start},
    {"locked and incompatible targets are listed and skipped",
     &locked_and_incompatible_targets_are_listed_and_skipped},
    {"named and effect fields share the multi-edit contract",
     &named_and_effect_fields_share_the_multi_edit_contract},
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
