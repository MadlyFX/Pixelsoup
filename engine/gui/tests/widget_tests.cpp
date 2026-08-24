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

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 17U> cases{{
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
