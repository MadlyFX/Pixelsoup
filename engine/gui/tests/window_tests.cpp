#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/window.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// The window host's proof, with no window. Event translation is the part of
// windowing that can be wrong quietly - a button that maps to the wrong index,
// a key that never reaches a text field, an edge that reads against the wrong
// frame - so the suite synthesizes SDL events and asserts what the widget layer
// would have seen.
//
// The one case that does touch a display is tolerant by design: a machine with
// no GPU or no video driver reports a skip rather than a failure, exactly as
// the render suite does.
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

[[nodiscard]] SDL_Event motion_event(float x, float y) noexcept {
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_MOTION;
    event.motion.x = x;
    event.motion.y = y;
    return event;
}

[[nodiscard]] SDL_Event button_event(
    Uint8 button, bool down, float x, float y) noexcept {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
    event.button.button = button;
    event.button.down = down;
    event.button.x = x;
    event.button.y = y;
    return event;
}

[[nodiscard]] SDL_Event key_event(
    SDL_Keycode code, bool down, SDL_Keymod mod = SDL_KMOD_NONE) noexcept {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.key = code;
    event.key.mod = mod;
    event.key.down = down;
    return event;
}

[[nodiscard]] SDL_Event text_event(const char* text) noexcept {
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = text;
    return event;
}

void motion_moves_the_pointer() {
    gui::InputState input;
    input.new_frame();
    const SDL_Event event = motion_event(120.0F, 48.0F);
    UI_CHECK(gui::translate_event(&event, input));

    UI_CHECK(near(input.mouse_x(), 120.0F));
    UI_CHECK(near(input.mouse_y(), 48.0F));
}

void a_button_event_carries_its_own_position() {
    gui::InputState input;
    input.new_frame();
    // No motion event first: a click that arrives cold must still land where it
    // was aimed, or the first press of a session hits the wrong widget.
    const SDL_Event event = button_event(SDL_BUTTON_LEFT, true, 300.0F, 200.0F);
    UI_CHECK(gui::translate_event(&event, input));

    UI_CHECK(near(input.mouse_x(), 300.0F));
    UI_CHECK(near(input.mouse_y(), 200.0F));
    UI_CHECK(input.mouse_down(gui::MouseButton::Left));
    UI_CHECK(input.mouse_pressed(gui::MouseButton::Left));
}

void the_three_buttons_map_to_their_own_indices() {
    gui::InputState input;
    input.new_frame();
    const SDL_Event right = button_event(SDL_BUTTON_RIGHT, true, 0.0F, 0.0F);
    UI_CHECK(gui::translate_event(&right, input));

    UI_CHECK(input.mouse_down(gui::MouseButton::Right));
    UI_CHECK(!input.mouse_down(gui::MouseButton::Left));
    UI_CHECK(!input.mouse_down(gui::MouseButton::Middle));

    const SDL_Event middle = button_event(SDL_BUTTON_MIDDLE, true, 0.0F, 0.0F);
    UI_CHECK(gui::translate_event(&middle, input));
    UI_CHECK(input.mouse_down(gui::MouseButton::Middle));
    UI_CHECK(input.mouse_down(gui::MouseButton::Right));
}

void press_and_release_read_as_edges_of_their_own_frames() {
    gui::InputState input;

    input.new_frame();
    const SDL_Event down = button_event(SDL_BUTTON_LEFT, true, 10.0F, 10.0F);
    UI_CHECK(gui::translate_event(&down, input));
    UI_CHECK(input.mouse_pressed(gui::MouseButton::Left));
    UI_CHECK(!input.mouse_released(gui::MouseButton::Left));

    // Held: the press edge is spent, and the button is merely down.
    input.new_frame();
    UI_CHECK(!input.mouse_pressed(gui::MouseButton::Left));
    UI_CHECK(input.mouse_down(gui::MouseButton::Left));

    input.new_frame();
    const SDL_Event up = button_event(SDL_BUTTON_LEFT, false, 10.0F, 10.0F);
    UI_CHECK(gui::translate_event(&up, input));
    UI_CHECK(input.mouse_released(gui::MouseButton::Left));
    UI_CHECK(!input.mouse_down(gui::MouseButton::Left));
}

void the_wheel_accumulates_within_a_frame() {
    gui::InputState input;
    input.new_frame();
    SDL_Event event{};
    event.type = SDL_EVENT_MOUSE_WHEEL;
    event.wheel.y = 1.5F;
    UI_CHECK(gui::translate_event(&event, input));
    UI_CHECK(gui::translate_event(&event, input));
    UI_CHECK(near(input.wheel(), 3.0F));

    // And clears with the frame, so a stopped wheel does not keep scrolling.
    input.new_frame();
    UI_CHECK(near(input.wheel(), 0.0F));
}

void typed_text_arrives_as_text_not_as_keys() {
    gui::InputState input;
    input.new_frame();
    const SDL_Event first = text_event("He");
    const SDL_Event second = text_event("llo");
    UI_CHECK(gui::translate_event(&first, input));
    UI_CHECK(gui::translate_event(&second, input));

    UI_CHECK(input.text() == "Hello");
    UI_CHECK(input.keys().empty());
}

void the_editing_keys_are_translated() {
    gui::InputState input;
    input.new_frame();
    const std::array<SDL_Event, 11U> events{{
        key_event(SDLK_BACKSPACE, true),
        key_event(SDLK_DELETE, true),
        key_event(SDLK_LEFT, true),
        key_event(SDLK_RIGHT, true),
        key_event(SDLK_UP, true),
        key_event(SDLK_DOWN, true),
        key_event(SDLK_HOME, true),
        key_event(SDLK_END, true),
        key_event(SDLK_RETURN, true),
        key_event(SDLK_TAB, true),
        key_event(SDLK_ESCAPE, true),
    }};
    for (const SDL_Event& event : events) {
        UI_CHECK(gui::translate_event(&event, input));
    }

    const auto keys = input.keys();
    UI_CHECK(keys.size() == 11U);
    UI_CHECK(keys[0] == gui::Key::Backspace);
    UI_CHECK(keys[1] == gui::Key::Delete);
    UI_CHECK(keys[2] == gui::Key::Left);
    UI_CHECK(keys[3] == gui::Key::Right);
    UI_CHECK(keys[4] == gui::Key::Up);
    UI_CHECK(keys[5] == gui::Key::Down);
    UI_CHECK(keys[6] == gui::Key::Home);
    UI_CHECK(keys[7] == gui::Key::End);
    UI_CHECK(keys[8] == gui::Key::Enter);
    UI_CHECK(keys[9] == gui::Key::Tab);
    UI_CHECK(keys[10] == gui::Key::Escape);
}

void ctrl_z_is_translated_as_document_undo() {
    gui::InputState input;
    input.new_frame();
    const SDL_Event event = key_event(SDLK_Z, true, SDL_KMOD_LCTRL);
    UI_CHECK(gui::translate_event(&event, input));

    const auto keys = input.keys();
    UI_CHECK(keys.size() == 1U);
    UI_CHECK(keys.front() == gui::Key::Undo);
    UI_CHECK(has_mod(input.mods(), gui::KeyMods::Ctrl));
}

void standard_redo_chords_are_translated_as_document_redo() {
    gui::InputState input;
    input.new_frame();
    const SDL_Event shifted_z = key_event(
        SDLK_Z, true,
        static_cast<SDL_Keymod>(SDL_KMOD_LCTRL | SDL_KMOD_LSHIFT));
    UI_CHECK(gui::translate_event(&shifted_z, input));

    auto keys = input.keys();
    UI_CHECK(keys.size() == 1U);
    UI_CHECK(keys.front() == gui::Key::Redo);
    UI_CHECK(has_mod(input.mods(), gui::KeyMods::Ctrl));
    UI_CHECK(has_mod(input.mods(), gui::KeyMods::Shift));

    input.new_frame();
    const SDL_Event ctrl_y = key_event(SDLK_Y, true, SDL_KMOD_LCTRL);
    UI_CHECK(gui::translate_event(&ctrl_y, input));

    keys = input.keys();
    UI_CHECK(keys.size() == 1U);
    UI_CHECK(keys.front() == gui::Key::Redo);
    UI_CHECK(has_mod(input.mods(), gui::KeyMods::Ctrl));
    UI_CHECK(!has_mod(input.mods(), gui::KeyMods::Shift));
}

void standard_clipboard_chords_are_semantic_keys() {
    gui::InputState input;
    input.new_frame();
    const SDL_Event copy = key_event(SDLK_C, true, SDL_KMOD_LCTRL);
    const SDL_Event paste = key_event(SDLK_V, true, SDL_KMOD_LCTRL);
    UI_CHECK(gui::translate_event(&copy, input));
    UI_CHECK(gui::translate_event(&paste, input));

    const auto keys = input.keys();
    UI_CHECK(keys.size() == 2U);
    UI_CHECK(keys[0] == gui::Key::Copy);
    UI_CHECK(keys[1] == gui::Key::Paste);
    UI_CHECK(has_mod(input.mods(), gui::KeyMods::Ctrl));
}

void a_printable_key_is_left_to_text_input() {
    gui::InputState input;
    input.new_frame();
    // The letter comes back as a text event instead; translating it here too
    // would type every character twice.
    const SDL_Event letter = key_event(SDLK_A, true);
    const SDL_Event function_key = key_event(SDLK_F5, true);
    UI_CHECK(gui::translate_event(&letter, input));
    UI_CHECK(gui::translate_event(&function_key, input));

    UI_CHECK(input.keys().empty());
}

void modifiers_ride_along_with_key_events() {
    gui::InputState input;
    input.new_frame();
    const SDL_Event event = key_event(
        SDLK_LEFT, true, static_cast<SDL_Keymod>(SDL_KMOD_LSHIFT | SDL_KMOD_LCTRL));
    UI_CHECK(gui::translate_event(&event, input));

    UI_CHECK(has_mod(input.mods(), gui::KeyMods::Shift));
    UI_CHECK(has_mod(input.mods(), gui::KeyMods::Ctrl));
    UI_CHECK(!has_mod(input.mods(), gui::KeyMods::Alt));

    // Releasing them clears the set, so a stale modifier cannot stick.
    input.new_frame();
    const SDL_Event release = key_event(SDLK_LSHIFT, false, SDL_KMOD_NONE);
    UI_CHECK(gui::translate_event(&release, input));
    UI_CHECK(!has_mod(input.mods(), gui::KeyMods::Shift));
}

void quit_and_close_ask_the_pump_to_stop() {
    gui::InputState input;
    input.new_frame();

    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    UI_CHECK(!gui::translate_event(&quit, input));

    SDL_Event close{};
    close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    UI_CHECK(!gui::translate_event(&close, input));

    // Everything else keeps the loop running.
    const SDL_Event motion = motion_event(1.0F, 1.0F);
    UI_CHECK(gui::translate_event(&motion, input));
    UI_CHECK(gui::translate_event(nullptr, input));
}

void an_unhandled_event_changes_nothing() {
    gui::InputState input;
    input.new_frame();
    const SDL_Event motion = motion_event(50.0F, 60.0F);
    UI_CHECK(gui::translate_event(&motion, input));

    SDL_Event other{};
    other.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
    UI_CHECK(gui::translate_event(&other, input));

    UI_CHECK(near(input.mouse_x(), 50.0F));
    UI_CHECK(near(input.mouse_y(), 60.0F));
    UI_CHECK(input.text().empty());
    UI_CHECK(input.keys().empty());
}

// The only case that wants a display. A machine without one - or without a GPU
// the RHI can drive - is a skip, not a failure: the same contract the render
// suite runs under.
void a_window_opens_when_the_platform_has_one() {
    gui::WindowConfig config{};
    config.title = "gui window smoke";
    config.width = 320U;
    config.height = 240U;

    auto created = gui::UiWindow::create(config);
    if (!created) {
        std::cout << "      (skipped: no window on this machine)\n";
        return;
    }
    auto window = std::move(created).value();

    UI_CHECK(window.width() > 0U);
    UI_CHECK(window.height() > 0U);
    UI_CHECK(window.time_seconds() >= 0.0);

    const gui::FontAtlas atlas = gui::FontAtlas::empty();
    UI_CHECK(window.set_atlas(atlas));

    // The editor records Program/Stage and UI presentation as two independent
    // submissions per frame. It must have another frame's offscreen context
    // available without waiting when those two are still in flight.
    const std::size_t expected_contexts =
        window.device().capabilities().backend
                == mediaserver::rhi::BackendKind::D3d12
            ? 4U
            : 3U;
    std::vector<mediaserver::rhi::CommandList> contexts;
    contexts.reserve(expected_contexts);
    for (std::size_t index = 0U; index < expected_contexts; ++index) {
        auto acquired = window.device().acquire_commands();
        UI_CHECK(acquired);
        contexts.emplace_back(std::move(acquired).value());
    }
    UI_CHECK(!window.device().acquire_commands());
    contexts.clear();

    // Pumping an empty queue must leave the window open and reset the frame.
    gui::InputState input;
    UI_CHECK(window.pump(input));
    UI_CHECK(input.text().empty());

    // Now the whole path, which nothing above can stand in for: events go into
    // the real SDL queue, the pump drains it, and the widget layer sees them.
    SDL_Event motion = motion_event(64.0F, 32.0F);
    SDL_Event press = button_event(SDL_BUTTON_LEFT, true, 64.0F, 32.0F);
    UI_CHECK(SDL_PushEvent(&motion));
    UI_CHECK(SDL_PushEvent(&press));
    UI_CHECK(window.pump(input));
    UI_CHECK(near(input.mouse_x(), 64.0F));
    UI_CHECK(near(input.mouse_y(), 32.0F));
    UI_CHECK(input.mouse_pressed(gui::MouseButton::Left));

    // The next pump starts a new frame, so the press edge is spent even though
    // the button is still held.
    UI_CHECK(window.pump(input));
    UI_CHECK(!input.mouse_pressed(gui::MouseButton::Left));
    UI_CHECK(input.mouse_down(gui::MouseButton::Left));

    // A quit latches the wrapper, but the native window is still alive so an
    // unsaved-work confirmation can cancel that request.
    SDL_Event quit{};
    quit.type = SDL_EVENT_QUIT;
    UI_CHECK(SDL_PushEvent(&quit));
    UI_CHECK(!window.pump(input));
    window.cancel_close_request();
    UI_CHECK(window.pump(input));

    UI_CHECK(SDL_PushEvent(&quit));
    UI_CHECK(!window.pump(input));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 15U> cases{{
    {"motion moves the pointer", &motion_moves_the_pointer},
    {"a button event carries its own position",
     &a_button_event_carries_its_own_position},
    {"the three buttons map to their own indices",
     &the_three_buttons_map_to_their_own_indices},
    {"press and release read as edges of their own frames",
     &press_and_release_read_as_edges_of_their_own_frames},
    {"the wheel accumulates within a frame",
     &the_wheel_accumulates_within_a_frame},
    {"typed text arrives as text not as keys",
     &typed_text_arrives_as_text_not_as_keys},
    {"the editing keys are translated", &the_editing_keys_are_translated},
    {"Ctrl+Z is translated as document undo",
     &ctrl_z_is_translated_as_document_undo},
    {"standard redo chords are translated as document redo",
     &standard_redo_chords_are_translated_as_document_redo},
    {"standard clipboard chords are semantic keys",
     &standard_clipboard_chords_are_semantic_keys},
    {"a printable key is left to text input",
     &a_printable_key_is_left_to_text_input},
    {"modifiers ride along with key events",
     &modifiers_ride_along_with_key_events},
    {"quit and close ask the pump to stop",
     &quit_and_close_ask_the_pump_to_stop},
    {"an unhandled event changes nothing", &an_unhandled_event_changes_nothing},
    {"a window opens when the platform has one",
     &a_window_opens_when_the_platform_has_one},
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
