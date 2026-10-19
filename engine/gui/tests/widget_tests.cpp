#include <mediaserver/gui/color_picker.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

// The widget layer's proof, with no GPU and no window. Each case drives
// synthetic input through the shared hot/active model and asserts the
// interaction outcome - a click needs a press then a release over the control, a
// drag keeps its capture off the control, a slider maps the pointer to its
// range. The draw list is exercised too, but it is the logic that is under test:
// a fontless atlas stands in, since glyph shape is irrelevant here.
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

[[nodiscard]] bool near(float a, float b) noexcept {
    return std::fabs(a - b) < 0.01F;
}

// A frame: snapshot input, apply this frame's pointer state, reset the context's
// per-frame state. Widgets are then called by the case.
void begin_frame(
    gui::InputState& input,
    gui::UiContext& ctx,
    float x,
    float y,
    bool left_down) {
    input.new_frame();
    input.set_mouse_position(x, y);
    input.set_mouse_button(gui::MouseButton::Left, left_down);
    ctx.new_frame();
}

void a_click_is_a_press_then_a_release_over_the_button() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("go");
    const gui::Rect r{10.0F, 10.0F, 100.0F, 30.0F};

    // Press over the button: no click yet, but it captures the pointer.
    begin_frame(input, ctx, 50.0F, 20.0F, true);
    UI_CHECK(!ctx.button(id, r, "Go"));
    UI_CHECK(ctx.active() == id);

    // Release over it: the click fires and the capture is released.
    begin_frame(input, ctx, 50.0F, 20.0F, false);
    UI_CHECK(ctx.button(id, r, "Go"));
    UI_CHECK(ctx.active() == 0U);

    // The button painted at least its body.
    UI_CHECK(!list.vertices().empty());

    // Playback can delay a UI refresh until both native events are queued.
    // A short click must still activate the transport exactly once.
    input.new_frame();
    input.set_mouse_button(gui::MouseButton::Left, true);
    input.set_mouse_button(gui::MouseButton::Left, false);
    ctx.new_frame();
    UI_CHECK(ctx.button(id, r, "Go"));
    UI_CHECK(ctx.active() == 0U);
    begin_frame(input, ctx, 50.0F, 20.0F, false);
    UI_CHECK(!ctx.button(id, r, "Go"));
}

void releasing_off_the_button_is_not_a_click() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("go");
    const gui::Rect r{10.0F, 10.0F, 100.0F, 30.0F};

    begin_frame(input, ctx, 50.0F, 20.0F, true);
    UI_CHECK(!ctx.button(id, r, "Go"));
    UI_CHECK(ctx.active() == id);

    // Slide off and release: no click, capture cleared.
    begin_frame(input, ctx, 300.0F, 300.0F, false);
    UI_CHECK(!ctx.button(id, r, "Go"));
    UI_CHECK(ctx.active() == 0U);
}

void input_keeps_each_buttons_first_press_position() {
    gui::InputState input;
    input.new_frame();
    input.set_mouse_position(12.0F, 24.0F);
    input.set_mouse_button(gui::MouseButton::Left, true);
    input.set_mouse_position(32.0F, 48.0F);
    input.set_mouse_button(gui::MouseButton::Right, true);
    input.set_mouse_position(72.0F, 96.0F);
    input.set_mouse_button(gui::MouseButton::Left, true);
    input.set_mouse_button(gui::MouseButton::Left, false);
    input.set_mouse_button(gui::MouseButton::Left, true);
    UI_CHECK(input.mouse_pressed(gui::MouseButton::Left));
    UI_CHECK(input.mouse_released(gui::MouseButton::Left));
    UI_CHECK(near(input.mouse_press_x(gui::MouseButton::Left), 12.0F));
    UI_CHECK(near(input.mouse_press_y(gui::MouseButton::Left), 24.0F));
    UI_CHECK(near(input.mouse_press_x(gui::MouseButton::Right), 32.0F));
    UI_CHECK(near(input.mouse_press_y(gui::MouseButton::Right), 48.0F));
    UI_CHECK(near(input.mouse_x(), 72.0F));
    input.release_held();
    input.new_frame();
    UI_CHECK(!input.mouse_pressed(gui::MouseButton::Left));
    input.set_mouse_position(120.0F, 140.0F);
    input.set_mouse_button(gui::MouseButton::Left, true);
    UI_CHECK(near(input.mouse_press_x(gui::MouseButton::Left), 120.0F));
    UI_CHECK(near(input.mouse_press_y(gui::MouseButton::Left), 140.0F));
}

void batched_motion_captures_the_press_origin_once() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::Rect origin{10.0F, 10.0F, 80.0F, 30.0F};
    const gui::Rect destination{150.0F, 10.0F, 80.0F, 30.0F};
    input.new_frame();
    input.set_mouse_position(40.0F, 20.0F);
    input.set_mouse_button(gui::MouseButton::Left, true);
    input.set_mouse_position(180.0F, 20.0F);
    input.set_mouse_button(gui::MouseButton::Left, false);
    ctx.new_frame();
    UI_CHECK(near(ctx.pointer_press_x(), 40.0F));
    UI_CHECK(near(ctx.pointer_x(), 180.0F));
    // The latest hovered control must not steal a press that began elsewhere,
    // even when it is visited before the actual press target.
    const auto other = ctx.grab(1U, destination);
    UI_CHECK(other.hovered && !other.pressed && !other.released);
    const auto source = ctx.grab(2U, origin);
    UI_CHECK(source.pressed && source.held && source.released);
    UI_CHECK(!source.hovered && !source.clicked);
    UI_CHECK(ctx.active() == 0U);
    const auto duplicate = ctx.grab(3U, origin);
    UI_CHECK(!duplicate.pressed && !duplicate.released);

    // A complete click in one event batch also activates only one of two
    // overlapping controls, despite release clearing the active ID.
    input.new_frame();
    input.set_mouse_position(40.0F, 20.0F);
    input.set_mouse_button(gui::MouseButton::Left, true);
    input.set_mouse_button(gui::MouseButton::Left, false);
    ctx.new_frame();
    UI_CHECK(ctx.button(2U, origin, "First"));
    UI_CHECK(!ctx.button(3U, origin, "Second"));
}

void batched_press_clipping_uses_the_press_origin() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::Rect content{0.0F, 0.0F, 200.0F, 100.0F};
    const gui::Rect clip{0.0F, 0.0F, 80.0F, 50.0F};
    input.new_frame();
    input.set_mouse_position(40.0F, 20.0F);
    input.set_mouse_button(gui::MouseButton::Left, true);
    input.set_mouse_position(120.0F, 20.0F);
    ctx.new_frame();
    ctx.push_clip(clip);
    const auto from_visible = ctx.grab(1U, content);
    UI_CHECK(from_visible.pressed && from_visible.held && !from_visible.hovered);
    ctx.pop_clip();
    begin_frame(input, ctx, 120.0F, 20.0F, false);
    UI_CHECK(ctx.grab(1U, content).released);

    input.new_frame();
    input.set_mouse_position(120.0F, 20.0F);
    input.set_mouse_button(gui::MouseButton::Left, true);
    input.set_mouse_position(40.0F, 20.0F);
    ctx.new_frame();
    ctx.push_clip(clip);
    const auto from_hidden = ctx.grab(1U, content);
    UI_CHECK(from_hidden.hovered && !from_hidden.pressed && !from_hidden.held);
    ctx.pop_clip();
}

void motion_after_release_does_not_change_the_click_endpoint() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::Rect bounds{10.0F, 10.0F, 80.0F, 30.0F};
    input.new_frame();
    input.set_mouse_position(40.0F, 20.0F);
    input.set_mouse_button(gui::MouseButton::Left, true);
    input.set_mouse_position(60.0F, 25.0F);
    input.set_mouse_button(gui::MouseButton::Left, false);
    input.set_mouse_position(180.0F, 90.0F);
    ctx.new_frame();
    const auto clicked = ctx.grab(1U, bounds);
    UI_CHECK(clicked.pressed && clicked.released && clicked.clicked);
    UI_CHECK(!clicked.hovered);
    UI_CHECK(near(ctx.pointer_release_x(), 60.0F));
    UI_CHECK(near(ctx.pointer_release_y(), 25.0F));
    UI_CHECK(near(ctx.pointer_release_x() - ctx.pointer_press_x(), 20.0F));

    // Conversely, a release outside cannot become a click by moving back in.
    input.new_frame();
    input.set_mouse_position(40.0F, 20.0F);
    input.set_mouse_button(gui::MouseButton::Left, true);
    input.set_mouse_position(180.0F, 90.0F);
    input.set_mouse_button(gui::MouseButton::Left, false);
    input.set_mouse_position(40.0F, 20.0F);
    ctx.new_frame();
    const auto canceled = ctx.grab(1U, bounds);
    UI_CHECK(canceled.pressed && canceled.released && !canceled.clicked);
    UI_CHECK(canceled.hovered);
}

void a_prior_release_does_not_end_a_new_capture() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::Rect first{10.0F, 10.0F, 80.0F, 30.0F};
    const gui::Rect second{150.0F, 10.0F, 80.0F, 30.0F};
    begin_frame(input, ctx, 40.0F, 20.0F, true);
    UI_CHECK(ctx.grab(1U, first).pressed);
    input.new_frame();
    input.set_mouse_position(60.0F, 20.0F);
    input.set_mouse_button(gui::MouseButton::Left, false);
    input.set_mouse_position(180.0F, 20.0F);
    input.set_mouse_button(gui::MouseButton::Left, true);
    ctx.new_frame();
    UI_CHECK(!input.mouse_release_follows_press(gui::MouseButton::Left));
    UI_CHECK(ctx.grab(1U, first).clicked);
    const auto next = ctx.grab(2U, second);
    UI_CHECK(next.pressed && next.held && !next.released);
    UI_CHECK(ctx.active() == 2U);
    begin_frame(input, ctx, 190.0F, 20.0F, false);
    UI_CHECK(ctx.grab(2U, second).clicked);
}

void a_press_outside_never_activates() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("go");
    const gui::Rect r{10.0F, 10.0F, 100.0F, 30.0F};

    begin_frame(input, ctx, 300.0F, 300.0F, true);
    UI_CHECK(!ctx.button(id, r, "Go"));
    UI_CHECK(ctx.active() == 0U);
    UI_CHECK(ctx.hot() != id);
}

void a_slider_maps_the_pointer_to_its_range() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("amount");
    const gui::Rect r{0.0F, 0.0F, 100.0F, 12.0F};

    float value = 0.0F;
    // Press at a quarter across: value lands a quarter through the range.
    begin_frame(input, ctx, 25.0F, 6.0F, true);
    UI_CHECK(ctx.slider(id, r, value, 0.0F, 10.0F));
    UI_CHECK(near(value, 2.5F));

    // Drag past the right edge: clamped to the max.
    begin_frame(input, ctx, 250.0F, 6.0F, true);
    UI_CHECK(ctx.slider(id, r, value, 0.0F, 10.0F));
    UI_CHECK(near(value, 10.0F));

    // Drag past the left edge: clamped to the min.
    begin_frame(input, ctx, -50.0F, 6.0F, true);
    UI_CHECK(ctx.slider(id, r, value, 0.0F, 10.0F));
    UI_CHECK(near(value, 0.0F));
}

void a_slider_drag_keeps_its_capture_off_the_track() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("amount");
    const gui::Rect r{0.0F, 0.0F, 100.0F, 12.0F};

    float value = 0.0F;
    begin_frame(input, ctx, 50.0F, 6.0F, true);
    UI_CHECK(ctx.slider(id, r, value, 0.0F, 10.0F));
    UI_CHECK(ctx.active() == id);

    // Pointer leaves the track vertically but the button is still held: the
    // slider keeps tracking, and its value follows the pointer's x.
    begin_frame(input, ctx, 100.0F, 400.0F, true);
    UI_CHECK(ctx.slider(id, r, value, 0.0F, 10.0F));
    UI_CHECK(ctx.active() == id);
    UI_CHECK(near(value, 10.0F));

    begin_frame(input, ctx, 100.0F, 400.0F, false);
    static_cast<void>(ctx.slider(id, r, value, 0.0F, 10.0F));
    UI_CHECK(ctx.active() == 0U);
}

void a_drag_field_moves_by_pointer_delta() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("x");
    const gui::Rect r{0.0F, 0.0F, 100.0F, 20.0F};

    float value = 1.0F;
    // Move over it first, so the press frame carries no stray delta.
    begin_frame(input, ctx, 50.0F, 10.0F, false);
    UI_CHECK(!ctx.drag_float(id, r, value, 0.5F));

    // Press: no motion, no change.
    begin_frame(input, ctx, 50.0F, 10.0F, true);
    UI_CHECK(!ctx.drag_float(id, r, value, 0.5F));
    UI_CHECK(near(value, 1.0F));

    // Drag right by 10px at speed 0.5: value climbs by 5.
    begin_frame(input, ctx, 60.0F, 10.0F, true);
    UI_CHECK(ctx.drag_float(id, r, value, 0.5F));
    UI_CHECK(near(value, 6.0F));

    begin_frame(input, ctx, 60.0F, 10.0F, false);
    static_cast<void>(ctx.drag_float(id, r, value, 0.5F));
    UI_CHECK(ctx.active() == 0U);
}

void a_checkbox_toggles_on_release() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("visible");
    const gui::Rect r{0.0F, 0.0F, 150.0F, 20.0F};

    bool value = false;
    begin_frame(input, ctx, 8.0F, 10.0F, true);
    UI_CHECK(!ctx.checkbox(id, r, value, "Visible"));
    UI_CHECK(!value);

    begin_frame(input, ctx, 8.0F, 10.0F, false);
    UI_CHECK(ctx.checkbox(id, r, value, "Visible"));
    UI_CHECK(value);

    // A second click turns it back off.
    begin_frame(input, ctx, 8.0F, 10.0F, true);
    static_cast<void>(ctx.checkbox(id, r, value, "Visible"));
    begin_frame(input, ctx, 8.0F, 10.0F, false);
    UI_CHECK(ctx.checkbox(id, r, value, "Visible"));
    UI_CHECK(!value);
}

void hovering_sets_hot_without_capturing() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("go");
    const gui::Rect r{10.0F, 10.0F, 100.0F, 30.0F};

    begin_frame(input, ctx, 50.0F, 20.0F, false);
    static_cast<void>(ctx.button(id, r, "Go"));
    UI_CHECK(ctx.hot() == id);
    UI_CHECK(ctx.active() == 0U);

    // Move away: hot clears next frame.
    begin_frame(input, ctx, 300.0F, 300.0F, false);
    static_cast<void>(ctx.button(id, r, "Go"));
    UI_CHECK(ctx.hot() != id);
}

// Focuses `field` by pressing and releasing over it, and leaves the caret where
// a click put it. The fontless atlas measures every string as zero wide, so a
// click always lands the caret at the start - which is why the cases below move
// it with Home/End rather than by clicking, and why glyph metrics are the
// render suite's business, not this one's.
void focus_field(
    gui::InputState& input,
    gui::UiContext& ctx,
    gui::UiId id,
    const gui::Rect& bounds,
    std::string& value) {
    begin_frame(input, ctx, bounds.x + 2.0F, bounds.y + 2.0F, true);
    static_cast<void>(ctx.text_field(id, bounds, value));
    begin_frame(input, ctx, bounds.x + 2.0F, bounds.y + 2.0F, false);
    static_cast<void>(ctx.text_field(id, bounds, value));
}

void a_click_focuses_a_text_field_and_typing_inserts() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("name");
    const gui::Rect r{0.0F, 0.0F, 200.0F, 24.0F};

    std::string value = "ab";
    focus_field(input, ctx, id, r, value);
    UI_CHECK(ctx.focus() == id);
    UI_CHECK(ctx.caret() == 0U);

    // End moves the caret without changing the text.
    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_key(gui::Key::End);
    UI_CHECK(!ctx.text_field(id, r, value));
    UI_CHECK(ctx.caret() == 2U);

    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_text("c");
    UI_CHECK(ctx.text_field(id, r, value));
    UI_CHECK(value == "abc");
    UI_CHECK(ctx.caret() == 3U);
}

// A password field edits exactly as a text field does - a click focuses it,
// End and typing work on the real text, a multi-byte character stays whole -
// while it draws one '*' per character.
void a_password_field_edits_the_real_text() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("password");
    const gui::Rect r{0.0F, 0.0F, 200.0F, 24.0F};

    std::string value = "p\xc3\xa9";
    begin_frame(input, ctx, r.x + 2.0F, r.y + 2.0F, true);
    static_cast<void>(ctx.password_field(id, r, value));
    begin_frame(input, ctx, r.x + 2.0F, r.y + 2.0F, false);
    static_cast<void>(ctx.password_field(id, r, value));
    UI_CHECK(ctx.focus() == id);

    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_key(gui::Key::End);
    UI_CHECK(!ctx.password_field(id, r, value));
    UI_CHECK(ctx.caret() == value.size());

    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_text("x");
    UI_CHECK(ctx.password_field(id, r, value));
    UI_CHECK(value == "p\xc3\xa9x");

    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_key(gui::Key::Backspace);
    input.add_key(gui::Key::Backspace);
    UI_CHECK(ctx.password_field(id, r, value));
    UI_CHECK(value == "p");
}

// A paste lands in the focused field.
//
// The clipboard reaches this layer as ordinary typed text - the window host
// reads it when it sees the chord and pushes it through the same path a
// keystroke takes - so the field inserts a whole path at the caret without
// knowing what a clipboard is. Before that, Key::Paste was passed through as a
// global shortcut and a path field could only be typed into by hand, which for
// an absolute path is not an authoring surface at all.
void a_paste_inserts_at_the_caret() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("path");
    const gui::Rect r{0.0F, 0.0F, 200.0F, 24.0F};

    std::string value;
    focus_field(input, ctx, id, r, value);
    UI_CHECK(ctx.focus() == id);

    // The whole string arrives at once, and the caret ends past it rather than
    // in the middle of what was just inserted.
    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_key(gui::Key::Paste);
    input.add_text("C:/stage/studio.hdr");
    UI_CHECK(ctx.text_field(id, r, value));
    UI_CHECK(value == "C:/stage/studio.hdr");
    UI_CHECK(ctx.caret() == value.size());

    // And a second paste inserts at the caret rather than replacing.
    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_key(gui::Key::Home);
    UI_CHECK(!ctx.text_field(id, r, value));
    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_key(gui::Key::Paste);
    input.add_text("D:/");
    UI_CHECK(ctx.text_field(id, r, value));
    UI_CHECK(value == "D:/C:/stage/studio.hdr");
    UI_CHECK(ctx.caret() == 3U);
}

void an_unfocused_field_ignores_the_keyboard() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("name");
    const gui::Rect r{0.0F, 0.0F, 200.0F, 24.0F};

    std::string value = "ab";
    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_text("zzz");
    input.add_key(gui::Key::Backspace);
    UI_CHECK(!ctx.text_field(id, r, value));
    UI_CHECK(value == "ab");
}

void backspace_and_delete_cut_either_side_of_the_caret() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("name");
    const gui::Rect r{0.0F, 0.0F, 200.0F, 24.0F};

    std::string value = "abc";
    focus_field(input, ctx, id, r, value);

    // Caret at the end: Backspace takes the character before it.
    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_key(gui::Key::End);
    input.add_key(gui::Key::Backspace);
    UI_CHECK(ctx.text_field(id, r, value));
    UI_CHECK(value == "ab");

    // Caret at the start: Delete takes the character after it.
    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_key(gui::Key::Home);
    input.add_key(gui::Key::Delete);
    UI_CHECK(ctx.text_field(id, r, value));
    UI_CHECK(value == "b");

    // And neither runs off the end of the string.
    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_key(gui::Key::Backspace);
    input.add_key(gui::Key::Backspace);
    static_cast<void>(ctx.text_field(id, r, value));
    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_key(gui::Key::Delete);
    static_cast<void>(ctx.text_field(id, r, value));
    UI_CHECK(value.empty());
    UI_CHECK(ctx.caret() == 0U);
}

void the_caret_steps_over_whole_utf8_characters() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("name");
    const gui::Rect r{0.0F, 0.0F, 200.0F, 24.0F};

    // "a" then a two-byte e-acute: four bytes, three characters.
    std::string value = "a\xC3\xA9z";
    focus_field(input, ctx, id, r, value);

    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_key(gui::Key::Right);
    input.add_key(gui::Key::Right);
    UI_CHECK(!ctx.text_field(id, r, value));
    // Two characters in is three bytes in, not two.
    UI_CHECK(ctx.caret() == 3U);

    // Backspace takes the whole character rather than half of it.
    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_key(gui::Key::Backspace);
    UI_CHECK(ctx.text_field(id, r, value));
    UI_CHECK(value == "az");
}

void enter_commits_and_escape_restores() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("name");
    const gui::Rect r{0.0F, 0.0F, 200.0F, 24.0F};

    std::string value = "abc";
    focus_field(input, ctx, id, r, value);
    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_key(gui::Key::Enter);
    static_cast<void>(ctx.text_field(id, r, value));
    UI_CHECK(ctx.focus() == 0U);
    UI_CHECK(value == "abc");

    // Edit, then cancel: the field goes back to what it held when the edit
    // began, not to whatever was typed.
    focus_field(input, ctx, id, r, value);
    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_text("XY");
    UI_CHECK(ctx.text_field(id, r, value));
    UI_CHECK(value == "XYabc");

    begin_frame(input, ctx, 2.0F, 2.0F, false);
    input.add_key(gui::Key::Escape);
    UI_CHECK(ctx.text_field(id, r, value));
    UI_CHECK(value == "abc");
    UI_CHECK(ctx.focus() == 0U);
}

void pressing_elsewhere_commits_the_edit() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("name");
    const gui::Rect r{0.0F, 0.0F, 200.0F, 24.0F};

    std::string value = "abc";
    focus_field(input, ctx, id, r, value);
    UI_CHECK(ctx.focus() == id);

    // A press far away ends the edit and keeps what was typed.
    begin_frame(input, ctx, 600.0F, 600.0F, true);
    static_cast<void>(ctx.text_field(id, r, value));
    UI_CHECK(ctx.focus() == 0U);
    UI_CHECK(value == "abc");
}

void a_splitter_drag_moves_the_seam_and_both_panes() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("main split");
    const gui::Rect bounds{0.0F, 0.0F, 1000.0F, 600.0F};

    float fraction = 0.5F;
    // Grab the divider, which sits centred on the seam.
    begin_frame(input, ctx, 500.0F, 300.0F, true);
    gui::Split panes = ctx.split(id, bounds, fraction, gui::Axis::Row);
    UI_CHECK(ctx.active() == id);
    UI_CHECK(near(panes.divider.x, 497.0F));

    // Drag left: the fraction follows the pointer and both panes resize with it.
    begin_frame(input, ctx, 300.0F, 300.0F, true);
    panes = ctx.split(id, bounds, fraction, gui::Axis::Row);
    UI_CHECK(near(fraction, 0.3F));
    UI_CHECK(near(panes.first.width, 297.0F));
    UI_CHECK(near(panes.second.x, 303.0F));
    UI_CHECK(near(panes.second.width, 697.0F));
    UI_CHECK(near(panes.first.height, 600.0F));

    begin_frame(input, ctx, 300.0F, 300.0F, false);
    panes = ctx.split(id, bounds, fraction, gui::Axis::Row);
    UI_CHECK(ctx.active() == 0U);
}

void a_column_splitter_divides_top_from_bottom() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("timeline split");
    const gui::Rect bounds{0.0F, 0.0F, 800.0F, 1000.0F};

    float fraction = 0.5F;
    begin_frame(input, ctx, 400.0F, 500.0F, true);
    static_cast<void>(ctx.split(id, bounds, fraction, gui::Axis::Column));

    begin_frame(input, ctx, 400.0F, 700.0F, true);
    const gui::Split panes = ctx.split(id, bounds, fraction, gui::Axis::Column);
    UI_CHECK(near(fraction, 0.7F));
    UI_CHECK(near(panes.first.height, 697.0F));
    UI_CHECK(near(panes.second.y, 703.0F));
    UI_CHECK(near(panes.first.width, 800.0F));
}

void a_splitter_never_lets_a_pane_vanish() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("main split");
    const gui::Rect bounds{0.0F, 0.0F, 1000.0F, 600.0F};

    float fraction = 0.5F;
    begin_frame(input, ctx, 500.0F, 300.0F, true);
    static_cast<void>(ctx.split(id, bounds, fraction, gui::Axis::Row));

    // Drag far past the left edge: the seam stops at the minimum pane width.
    begin_frame(input, ctx, -400.0F, 300.0F, true);
    gui::Split panes = ctx.split(id, bounds, fraction, gui::Axis::Row);
    UI_CHECK(near(fraction, 0.024F));
    UI_CHECK(panes.first.width > 0.0F);

    // And symmetrically past the right.
    begin_frame(input, ctx, 4000.0F, 300.0F, true);
    panes = ctx.split(id, bounds, fraction, gui::Axis::Row);
    UI_CHECK(near(fraction, 0.976F));
    UI_CHECK(panes.second.width > 0.0F);
}

void a_clip_prevents_interaction_with_hidden_content() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("scrolled row");
    const gui::Rect row{10.0F, 70.0F, 100.0F, 24.0F};
    const gui::Rect clip{0.0F, 0.0F, 120.0F, 50.0F};

    // The pointer is inside the row's unscrolled geometry but outside the
    // visible clip. It must not become hot or capture the press.
    begin_frame(input, ctx, 40.0F, 80.0F, true);
    ctx.push_clip(clip);
    const gui::Interaction interaction = ctx.grab(id, row);
    ctx.pop_clip();
    UI_CHECK(!interaction.hovered);
    UI_CHECK(!interaction.pressed);
    UI_CHECK(ctx.hot() != id);
    UI_CHECK(ctx.active() == 0U);
}

void a_clipped_control_keeps_an_existing_drag_capture() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("dragged row");
    const gui::Rect row{10.0F, 10.0F, 100.0F, 24.0F};
    const gui::Rect clip{0.0F, 0.0F, 120.0F, 50.0F};

    begin_frame(input, ctx, 40.0F, 20.0F, true);
    ctx.push_clip(clip);
    const gui::Interaction pressed = ctx.grab(id, row);
    ctx.pop_clip();
    UI_CHECK(pressed.pressed);
    UI_CHECK(ctx.active() == id);

    // Releasing outside the panel still reaches the control that owns capture,
    // which is required for drag-and-drop between docked panels.
    begin_frame(input, ctx, 300.0F, 300.0F, false);
    ctx.push_clip(clip);
    const gui::Interaction released = ctx.grab(id, row);
    ctx.pop_clip();
    UI_CHECK(released.held);
    UI_CHECK(released.released);
    UI_CHECK(!released.clicked);
    UI_CHECK(ctx.active() == 0U);
}

void the_color_picker_maps_hue_and_saturation_value_directly() {
    constexpr gui::Rect bounds{0.0F, 0.0F, 200.0F, 120.0F};
    constexpr gui::UiColor green{0.0F, 1.0F, 0.0F, 1.0F};

    gui::InputState hue_input;
    gui::UiDrawList hue_list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext hue_ctx{hue_input, hue_list, font};
    gui::ColorPicker hue_picker;
    begin_frame(hue_input, hue_ctx, bounds.width * (2.0F / 3.0F), 113.0F,
                true);
    const gui::ColorPickerFrame blue = hue_picker.draw(
        hue_ctx, gui::ui_id("hue picker"), bounds, green);
    UI_CHECK(blue.changed);
    UI_CHECK(blue.editing);
    UI_CHECK(near(blue.color.r, 0.0F));
    UI_CHECK(near(blue.color.g, 0.0F));
    UI_CHECK(near(blue.color.b, 1.0F));
    UI_CHECK(!hue_list.vertices().empty());

    gui::InputState field_input;
    gui::UiDrawList field_list;
    gui::UiContext field_ctx{field_input, field_list, font};
    gui::ColorPicker field_picker;
    // The field is 100px high. Its centre means saturation=.5, value=.5;
    // retaining green hue produces RGB (.25, .5, .25).
    begin_frame(field_input, field_ctx, 100.0F, 50.0F, true);
    const gui::ColorPickerFrame muted = field_picker.draw(
        field_ctx, gui::ui_id("field picker"), bounds, green);
    UI_CHECK(muted.changed);
    UI_CHECK(near(muted.color.r, 0.25F));
    UI_CHECK(near(muted.color.g, 0.5F));
    UI_CHECK(near(muted.color.b, 0.25F));

    gui::InputState gray_input;
    gui::UiDrawList gray_list;
    gui::UiContext gray_ctx{gray_input, gray_list, font};
    gui::ColorPicker gray_picker;
    const gui::UiId gray_id = gui::ui_id("gray picker");
    constexpr gui::UiColor gray{0.5F, 0.5F, 0.5F, 1.0F};
    begin_frame(gray_input, gray_ctx, bounds.width * (2.0F / 3.0F), 113.0F,
                true);
    const gui::ColorPickerFrame hue_only =
        gray_picker.draw(gray_ctx, gray_id, bounds, gray);
    UI_CHECK(hue_only.editing);
    UI_CHECK(!hue_only.changed);
    begin_frame(gray_input, gray_ctx, bounds.width * (2.0F / 3.0F), 113.0F,
                false);
    static_cast<void>(gray_picker.draw(gray_ctx, gray_id, bounds, gray));
    begin_frame(gray_input, gray_ctx, 199.0F, 50.0F, true);
    const gui::ColorPickerFrame saturated =
        gray_picker.draw(gray_ctx, gray_id, bounds, gray);
    UI_CHECK(saturated.changed);
    UI_CHECK(near(saturated.color.r, 0.0F));
    UI_CHECK(near(saturated.color.g, 0.0F));
    UI_CHECK(near(saturated.color.b, 0.5F));
}

void dropdown_selection_is_direct_and_blocks_the_workspace() {
    gui::InputState input; gui::UiDrawList list; const auto font=gui::FontAtlas::empty();gui::UiContext ctx{input,list,font};
    std::size_t selected=0;
    constexpr std::array<std::string_view,4> options{{"Point","Spot","Rectangle","Sphere"}};
    const gui::Rect field{10,10,110,22}, window{0,0,240,200};
    const auto draw=[&] {const bool changed=ctx.dropdown(50,field,options,selected);ctx.draw_overlays(window);return changed;};
    begin_frame(input,ctx,30,20,true);UI_CHECK(!draw());
    begin_frame(input,ctx,30,20,false);UI_CHECK(!draw());UI_CHECK(ctx.typing());
    // The menu overlaps another control; its click must never leak through.
    begin_frame(input,ctx,30,118,true);UI_CHECK(!ctx.button(51,{0,30,200,150},"Underlying"));UI_CHECK(!draw());
    begin_frame(input,ctx,30,118,false);UI_CHECK(!ctx.button(51,{0,30,200,150},"Underlying"));UI_CHECK(draw());UI_CHECK(selected==3);
    // Outside dismissal consumes its press and release.
    begin_frame(input,ctx,30,20,true);static_cast<void>(draw());
    begin_frame(input,ctx,30,20,false);static_cast<void>(draw());
    begin_frame(input,ctx,200,180,true);UI_CHECK(!ctx.button(51,{180,160,50,35},"Outside"));UI_CHECK(!draw());
    begin_frame(input,ctx,200,180,false);UI_CHECK(!ctx.button(51,{180,160,50,35},"Outside"));UI_CHECK(!draw());UI_CHECK(selected==3);
}

void dropdown_keyboard_selection_escape_and_disappearing_owner() {
    gui::InputState input;gui::UiDrawList list;const auto font=gui::FontAtlas::empty();gui::UiContext ctx{input,list,font};
    std::size_t selected=0;constexpr std::array<std::string_view,3> options{{"Y up","Z up","X up"}};
    const gui::Rect field{10,170,100,22},window{0,0,240,200};
    const auto draw=[&] {const bool changed=ctx.dropdown(60,field,options,selected);ctx.draw_overlays(window);return changed;};
    begin_frame(input,ctx,30,180,true);static_cast<void>(draw());
    begin_frame(input,ctx,30,180,false);static_cast<void>(draw());
    input.new_frame();input.add_key(gui::Key::Down);ctx.new_frame();UI_CHECK(!draw());
    input.new_frame();input.add_key(gui::Key::Enter);ctx.new_frame();UI_CHECK(draw());UI_CHECK(selected==1);
    begin_frame(input,ctx,30,180,true);static_cast<void>(draw());
    begin_frame(input,ctx,30,180,false);static_cast<void>(draw());
    input.new_frame();input.add_key(gui::Key::Down);ctx.new_frame();static_cast<void>(draw());
    input.new_frame();input.add_key(gui::Key::Escape);ctx.new_frame();UI_CHECK(!draw());UI_CHECK(selected==1);
    begin_frame(input,ctx,30,180,true);static_cast<void>(draw());
    begin_frame(input,ctx,30,180,false);static_cast<void>(draw());
    input.new_frame();ctx.new_frame();ctx.draw_overlays(window);UI_CHECK(!ctx.pointer_blocked());
}

void a_context_menu_opens_at_a_point_and_reports_one_choice() {
    gui::InputState input;gui::UiDrawList list;const auto font=gui::FontAtlas::empty();gui::UiContext ctx{input,list,font};
    constexpr std::array<std::string_view,3> options{{"Add Marker","Add Hold Cue","Add Loop Cue"}};
    const gui::Rect window{0,0,400,300};
    std::optional<std::size_t> chosen;
    const auto draw=[&] {chosen=ctx.context_menu(70,options);ctx.draw_overlays(window);};
    // A right press is the only thing that asks for it.
    input.new_frame();input.set_mouse_position(100,50);input.set_mouse_button(gui::MouseButton::Right,true);ctx.new_frame();
    UI_CHECK(ctx.secondary_pointer_pressed());
    ctx.open_context_menu(70,ctx.pointer_x(),ctx.pointer_y());draw();
    UI_CHECK(ctx.menu_open(70));UI_CHECK(ctx.typing());UI_CHECK(!chosen);
    input.new_frame();input.set_mouse_button(gui::MouseButton::Right,false);ctx.new_frame();draw();
    // Enter with nothing under the pointer picks nothing.
    input.new_frame();input.add_key(gui::Key::Enter);ctx.new_frame();draw();UI_CHECK(!chosen);
    UI_CHECK(!ctx.menu_open(70));
    // Open again; the second entry is 24px below the anchor's first.
    input.new_frame();input.set_mouse_position(100,50);input.set_mouse_button(gui::MouseButton::Right,true);ctx.new_frame();
    ctx.open_context_menu(70,100,50);draw();
    input.new_frame();input.set_mouse_button(gui::MouseButton::Right,false);ctx.new_frame();draw();
    begin_frame(input,ctx,120,52+24+12,true);UI_CHECK(!ctx.button(71,{0,0,400,300},"Underneath"));draw();UI_CHECK(!chosen);
    begin_frame(input,ctx,120,52+24+12,false);UI_CHECK(!ctx.button(71,{0,0,400,300},"Underneath"));draw();
    UI_CHECK(chosen==std::optional<std::size_t>{1U});UI_CHECK(!ctx.menu_open(70));
    // Once: the next frame reports nothing.
    begin_frame(input,ctx,120,88,false);draw();UI_CHECK(!chosen);
    // An owner that stops asking takes its menu with it.
    ctx.open_context_menu(70,100,50);draw();
    input.new_frame();ctx.new_frame();ctx.draw_overlays(window);UI_CHECK(!ctx.menu_open(70));
}

void shortcut_text_stands_down_while_anything_is_typing() {
    gui::InputState input;gui::UiDrawList list;const auto font=gui::FontAtlas::empty();gui::UiContext ctx{input,list,font};
    input.new_frame();input.add_text("m");ctx.new_frame();
    UI_CHECK(ctx.shortcut_text()=="m");
    std::string value;
    ctx.focus_text_field(80,value);
    input.new_frame();input.add_text("m");ctx.new_frame();
    UI_CHECK(ctx.shortcut_text().empty());
}

// A flown camera owns W A S D Q E: the letters stop being shortcuts while it
// flies, and a field with the keyboard takes them back as typing.
void a_flying_camera_takes_movement_keys_from_shortcuts() {
    gui::InputState input;gui::UiDrawList list;const auto font=gui::FontAtlas::empty();gui::UiContext ctx{input,list,font};
    input.new_frame();input.add_text("q");input.set_move_key(gui::MoveKey::Q,true);ctx.new_frame();
    UI_CHECK(ctx.shortcut_text()=="q");UI_CHECK(ctx.move_key_down(gui::MoveKey::Q));
    ctx.set_navigating(true);
    input.new_frame();input.add_text("q");ctx.new_frame();
    UI_CHECK(ctx.typing());UI_CHECK(ctx.shortcut_text().empty());UI_CHECK(ctx.move_key_down(gui::MoveKey::Q));
    ctx.set_navigating(false);
    std::string value;
    ctx.focus_text_field(81,value);
    input.new_frame();ctx.new_frame();
    UI_CHECK(!ctx.move_key_down(gui::MoveKey::Q));
}

// A dense display draws the same logical layout with more pixels. The list
// multiplies as it emits, so every vertex and clip edge lands at the physical
// position, and rectangles stay on whole pixels.
void a_scaled_list_emits_physical_pixels() {
    gui::UiDrawList list;
    list.set_scale(1.5F);
    list.push_clip(10.0F, 10.0F, 100.0F, 50.0F);
    list.add_rect(20.0F, 20.0F, 40.0F, 10.0F, gui::UiColor{1.0F, 1.0F, 1.0F, 1.0F});
    list.pop_clip();
    UI_CHECK(list.batches().size() == 1U);
    const gui::UiBatch& batch = list.batches().front();
    UI_CHECK(near(batch.clip_min_x, 15.0F) && near(batch.clip_min_y, 15.0F));
    UI_CHECK(near(batch.clip_max_x, 165.0F) && near(batch.clip_max_y, 90.0F));
    const auto& vertices = list.vertices();
    UI_CHECK(vertices.size() == 6U);
    UI_CHECK(near(vertices[0].x, 30.0F) && near(vertices[0].y, 30.0F));
    UI_CHECK(near(vertices[2].x, 90.0F) && near(vertices[2].y, 45.0F));
    // reset() clears the frame but not the display's scale.
    list.reset();
    UI_CHECK(near(list.scale(), 1.5F));
}

// A one-unit rule must not come out one pixel here and two there at 150%;
// thin spans keep one width wherever they fall, and empty ones stay empty.
void thin_rules_keep_one_width_at_fractional_scale() {
    gui::UiDrawList list;
    list.set_scale(1.5F);
    const gui::UiColor white{1.0F, 1.0F, 1.0F, 1.0F};
    for (int index = 0; index < 4; ++index) {
        list.add_rect(static_cast<float>(index) * 3.0F + 0.5F, 0.0F, 1.0F,
                      10.0F, white);
    }
    const auto& vertices = list.vertices();
    UI_CHECK(vertices.size() == 24U);
    for (std::size_t quad = 0U; quad < 4U; ++quad) {
        const float left = vertices[quad * 6U].x;
        const float right = vertices[quad * 6U + 1U].x;
        UI_CHECK(near(right - left, 2.0F));
        UI_CHECK(near(left, std::round(left)));
    }
    list.add_rect(50.0F, 0.0F, 0.0F, 10.0F, white);
    UI_CHECK(list.vertices().size() == 24U);
}

// Opening a field with its value selected - how a rename starts - lets the
// first keystroke replace the old name, and an arrow key keep it instead.
void a_selected_field_replaces_its_value_on_typing() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("rename");
    const gui::Rect field{0.0F, 0.0F, 200.0F, 20.0F};
    std::string value{"Box 12"};
    ctx.focus_text_field_selected(id, value);
    input.new_frame();
    input.add_text("Floor");
    ctx.new_frame();
    UI_CHECK(ctx.text_field(id, field, value));
    UI_CHECK(value == "Floor");

    std::string kept{"Riser"};
    ctx.focus_text_field_selected(id, kept);
    input.new_frame();
    input.add_key(gui::Key::End);
    input.add_text(" 2");
    ctx.new_frame();
    static_cast<void>(ctx.text_field(id, field, kept));
    UI_CHECK(kept == "Riser 2");

    std::string cleared{"Wall"};
    ctx.focus_text_field_selected(id, cleared);
    input.new_frame();
    input.add_key(gui::Key::Backspace);
    ctx.new_frame();
    static_cast<void>(ctx.text_field(id, field, cleared));
    UI_CHECK(cleared.empty());
}

// A divider answers a press several units either side of the line it draws,
// and asks for the resize cursor while the pointer is over it.
void a_splitter_takes_presses_beside_its_line() {
    gui::InputState input;
    gui::UiDrawList list;
    const gui::FontAtlas font = gui::FontAtlas::empty();
    gui::UiContext ctx{input, list, font};
    const gui::UiId id = gui::ui_id("seam");
    const gui::Rect bounds{0.0F, 0.0F, 400.0F, 100.0F};
    float fraction = 0.5F;
    // The 2-unit line is at x=199..201; x=204 is outside it but within reach.
    begin_frame(input, ctx, 204.0F, 50.0F, false);
    static_cast<void>(ctx.split(id, bounds, fraction, gui::Axis::Row, 2.0F));
    UI_CHECK(ctx.cursor() == gui::Cursor::ResizeHorizontal);
    begin_frame(input, ctx, 204.0F, 50.0F, true);
    static_cast<void>(ctx.split(id, bounds, fraction, gui::Axis::Row, 2.0F));
    begin_frame(input, ctx, 300.0F, 50.0F, true);
    static_cast<void>(ctx.split(id, bounds, fraction, gui::Axis::Row, 2.0F));
    UI_CHECK(near(fraction, 0.75F));
    begin_frame(input, ctx, 300.0F, 50.0F, false);
    static_cast<void>(ctx.split(id, bounds, fraction, gui::Axis::Row, 2.0F));
    begin_frame(input, ctx, 50.0F, 50.0F, false);
    UI_CHECK(ctx.cursor() == gui::Cursor::Arrow);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 36U> cases{{
    {"motion after release does not change the click endpoint", &motion_after_release_does_not_change_the_click_endpoint},
    {"a prior release does not end a new capture", &a_prior_release_does_not_end_a_new_capture},
    {"input keeps each button's first press position", &input_keeps_each_buttons_first_press_position},
    {"batched motion captures the press origin once", &batched_motion_captures_the_press_origin_once},
    {"batched press clipping uses the press origin", &batched_press_clipping_uses_the_press_origin},
    {"a scaled list emits physical pixels", &a_scaled_list_emits_physical_pixels},
    {"thin rules keep one width at fractional scale",
     &thin_rules_keep_one_width_at_fractional_scale},
    {"a selected field replaces its value on typing",
     &a_selected_field_replaces_its_value_on_typing},
    {"a splitter takes presses beside its line",
     &a_splitter_takes_presses_beside_its_line},
    {"dropdown selection owns pointer input", &dropdown_selection_is_direct_and_blocks_the_workspace},
    {"dropdown keyboard and dismissal", &dropdown_keyboard_selection_escape_and_disappearing_owner},
    {"a context menu opens at a point and reports one choice", &a_context_menu_opens_at_a_point_and_reports_one_choice},
    {"shortcut text stands down while anything is typing", &shortcut_text_stands_down_while_anything_is_typing},
    {"a flying camera takes movement keys from shortcuts", &a_flying_camera_takes_movement_keys_from_shortcuts},
    {"a paste inserts at the caret", &a_paste_inserts_at_the_caret},
    {"a click is a press then a release over the button",
     &a_click_is_a_press_then_a_release_over_the_button},
    {"releasing off the button is not a click",
     &releasing_off_the_button_is_not_a_click},
    {"a press outside never activates", &a_press_outside_never_activates},
    {"a slider maps the pointer to its range",
     &a_slider_maps_the_pointer_to_its_range},
    {"a slider drag keeps its capture off the track",
     &a_slider_drag_keeps_its_capture_off_the_track},
    {"a drag field moves by pointer delta",
     &a_drag_field_moves_by_pointer_delta},
    {"a checkbox toggles on release", &a_checkbox_toggles_on_release},
    {"hovering sets hot without capturing",
     &hovering_sets_hot_without_capturing},
    {"a click focuses a text field and typing inserts",
     &a_click_focuses_a_text_field_and_typing_inserts},
    {"a password field edits the real text",
     &a_password_field_edits_the_real_text},
    {"an unfocused field ignores the keyboard",
     &an_unfocused_field_ignores_the_keyboard},
    {"backspace and delete cut either side of the caret",
     &backspace_and_delete_cut_either_side_of_the_caret},
    {"the caret steps over whole utf8 characters",
     &the_caret_steps_over_whole_utf8_characters},
    {"enter commits and escape restores", &enter_commits_and_escape_restores},
    {"pressing elsewhere commits the edit",
     &pressing_elsewhere_commits_the_edit},
    {"a splitter drag moves the seam and both panes",
     &a_splitter_drag_moves_the_seam_and_both_panes},
    {"a column splitter divides top from bottom",
     &a_column_splitter_divides_top_from_bottom},
    {"a splitter never lets a pane vanish",
     &a_splitter_never_lets_a_pane_vanish},
    {"a clip prevents interaction with hidden content",
     &a_clip_prevents_interaction_with_hidden_content},
    {"a clipped control keeps an existing drag capture",
     &a_clipped_control_keeps_an_existing_drag_capture},
    {"the color picker maps hue and saturation value directly",
     &the_color_picker_maps_hue_and_saturation_value_directly},
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
