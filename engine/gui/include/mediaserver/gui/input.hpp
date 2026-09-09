#pragma once

#include <mediaserver/core/types.hpp>

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The per-frame input the bespoke UI reacts to, kept independent of any window
// toolkit. The window host (GUI-2b) translates SDL events into these setters;
// the widget layer reads the queries. Splitting it this way keeps the widget
// logic headless-testable: a test drives synthetic input with no window at all.
//
// Edges (pressed / released this frame, mouse delta, typed text) are derived
// against the previous frame, so a widget can ask "was this clicked" rather than
// tracking history itself. Call new_frame() once at the top of each frame,
// before applying that frame's events.
namespace mediaserver::gui {

enum class MouseButton : core::u8 {
    Left,
    Middle,
    Right,
};

// Modifier keys held this frame, as a bitmask.
enum class KeyMods : core::u8 {
    None = 0U,
    Shift = 1U << 0U,
    Ctrl = 1U << 1U,
    Alt = 1U << 2U,
};

[[nodiscard]] constexpr KeyMods operator|(KeyMods a, KeyMods b) noexcept {
    return static_cast<KeyMods>(
        static_cast<core::u8>(a) | static_cast<core::u8>(b));
}
[[nodiscard]] constexpr bool has_mod(KeyMods set, KeyMods flag) noexcept {
    return (static_cast<core::u8>(set) & static_cast<core::u8>(flag)) != 0U;
}

// Non-text keys the UI acts on directly (text fields, navigation). Printable
// characters arrive through text(), not here.
enum class Key : core::u8 {
    Backspace,
    Delete,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    Enter,
    Tab,
    Escape,
    // Layout-independent editor shortcuts translated by the window host. Kept
    // distinct from printable text so a focused field can ignore them and let
    // its caller decide whether document history navigation is currently legal.
    Undo,
    Redo,
    Copy,
    Paste,
};

class InputState final {
public:
    // Snapshots the pointer and clears per-frame button edges, wheel, typed
    // text and key presses. Call before applying the frame's events.
    void new_frame() noexcept;

    // --- Event application: the window host calls these as events arrive. ---
    void set_mouse_position(float x, float y) noexcept;
    void set_mouse_button(MouseButton button, bool down) noexcept;
    void add_wheel(float delta) noexcept;
    void add_text(std::string_view utf8);
    void add_key(Key key);
    void set_mods(KeyMods mods) noexcept { mods_ = mods; }

    // --- Queries: the widget layer reads these. ---
    [[nodiscard]] float mouse_x() const noexcept { return mouse_x_; }
    [[nodiscard]] float mouse_y() const noexcept { return mouse_y_; }
    [[nodiscard]] float mouse_dx() const noexcept { return mouse_x_ - prev_x_; }
    [[nodiscard]] float mouse_dy() const noexcept { return mouse_y_ - prev_y_; }
    [[nodiscard]] bool mouse_down(MouseButton button) const noexcept;
    // Went down this frame (a click's leading edge).
    [[nodiscard]] bool mouse_pressed(MouseButton button) const noexcept;
    // Went up this frame (a click's trailing edge).
    [[nodiscard]] bool mouse_released(MouseButton button) const noexcept;
    [[nodiscard]] float wheel() const noexcept { return wheel_; }
    [[nodiscard]] std::string_view text() const noexcept { return text_; }
    [[nodiscard]] std::span<const Key> keys() const noexcept { return keys_; }
    [[nodiscard]] KeyMods mods() const noexcept { return mods_; }

private:
    static constexpr std::size_t button_count = 3U;
    [[nodiscard]] static std::size_t index(MouseButton button) noexcept {
        return static_cast<std::size_t>(button);
    }

    float mouse_x_{};
    float mouse_y_{};
    float prev_x_{};
    float prev_y_{};
    std::array<bool, button_count> down_{};
    std::array<bool, button_count> pressed_{};
    std::array<bool, button_count> released_{};
    float wheel_{};
    std::string text_;
    std::vector<Key> keys_;
    KeyMods mods_{KeyMods::None};
};

} // namespace mediaserver::gui
