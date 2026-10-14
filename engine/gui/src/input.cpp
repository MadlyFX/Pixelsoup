#include <mediaserver/gui/input.hpp>

namespace mediaserver::gui {

void InputState::new_frame() noexcept {
    pressed_.fill(false);
    released_.fill(false);
    prev_x_ = mouse_x_;
    prev_y_ = mouse_y_;
    wheel_ = 0.0F;
    motion_x_ = 0.0F;
    motion_y_ = 0.0F;
    text_.clear();
    keys_.clear();
}

void InputState::set_mouse_position(float x, float y) noexcept {
    if (pointer_locked_) {
        return;
    }
    mouse_x_ = x;
    mouse_y_ = y;
}

void InputState::set_mouse_button(MouseButton button, bool down) noexcept {
    const auto i = index(button);
    // A complete click can arrive between two UI refreshes. Preserve both
    // edges until the widgets consume the frame instead of losing the click
    // when its final button state is the same as the previous frame's.
    pressed_[i] = pressed_[i] || (down && !down_[i]);
    released_[i] = released_[i] || (!down && down_[i]);
    down_[i] = down;
}

void InputState::add_wheel(float delta) noexcept {
    wheel_ += delta;
}

void InputState::add_mouse_motion(float dx, float dy) noexcept {
    motion_x_ += dx;
    motion_y_ += dy;
}

void InputState::set_move_key(MoveKey key, bool down) noexcept {
    move_keys_[static_cast<std::size_t>(key)] = down;
}

void InputState::release_held() noexcept {
    for (const MouseButton button :
         {MouseButton::Left, MouseButton::Middle, MouseButton::Right}) {
        set_mouse_button(button, false);
    }
    move_keys_.fill(false);
    mods_ = KeyMods::None;
}

bool InputState::move_key_down(MoveKey key) const noexcept {
    return move_keys_[static_cast<std::size_t>(key)];
}

void InputState::add_text(std::string_view utf8) {
    text_.append(utf8);
}

void InputState::add_key(Key key) {
    keys_.push_back(key);
}

bool InputState::mouse_down(MouseButton button) const noexcept {
    return down_[index(button)];
}

bool InputState::mouse_pressed(MouseButton button) const noexcept {
    return pressed_[index(button)];
}

bool InputState::mouse_released(MouseButton button) const noexcept {
    return released_[index(button)];
}

} // namespace mediaserver::gui
