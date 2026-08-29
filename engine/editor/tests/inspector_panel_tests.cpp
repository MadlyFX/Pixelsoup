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
// A track's SCREEN section: TRANSFORM ends with its key note at 170, the
// Delete row runs 178..200, then the section header 204..228 and its two rows.
// The raster row carries two fields rather than three, so its columns are 93.2
// wide from x = 99.6.
constexpr float screen_raster_y = 243.0F;
constexpr float screen_aspect_y = 269.0F;
constexpr float raster_width_x = 145.0F;
constexpr float raster_height_x = 243.0F;

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
    core::u64 show_frame{50U};

    static constexpr gui::Rect bounds{0.0F, 0.0F, 300.0F, 600.0F};

    editor::InspectorFrame frame(float x, float y, bool down) {
        input.new_frame();
        input.set_mouse_position(x, y);
        input.set_mouse_button(gui::MouseButton::Left, down);
        ctx.new_frame();
        return panel.draw(ctx, bounds, &element, 0U, keyframes, show_frame,
                          effects);
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
        return panel.draw(ctx, bounds, &element, 0U, keyframes, show_frame,
                          effects);
    }

    editor::InspectorFrame nudge(gui::Key key,
                                 gui::KeyMods mods = gui::KeyMods::None) {
        input.new_frame();
        input.set_mouse_button(gui::MouseButton::Left, false);
        input.set_mods(mods);
        input.add_key(key);
        ctx.new_frame();
        return panel.draw(ctx, bounds, &element, 0U, keyframes, show_frame,
                          effects);
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

constexpr std::array<Case, 26U> cases{{
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
    {"an animated effect displays and keys its evaluated value",
     &an_animated_effect_displays_and_keys_its_evaluated_value},
    {"Saturation amount clamps and its filled key erases",
     &saturation_amount_clamps_and_its_filled_key_erases},
    {"Film Grain amount uses its typed value and keyframe channel",
     &film_grain_amount_uses_its_typed_value_and_keyframe_channel},
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
