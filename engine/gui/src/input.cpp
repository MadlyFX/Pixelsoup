#include <mediaserver/gui/input.hpp>

namespace mediaserver::gui {

void InputState::new_frame() noexcept {
    prev_down_ = down_;
    prev_x_ = mouse_x_;
    prev_y_ = mouse_y_;
    wheel_ = 0.0F;
    text_.clear();
    keys_.clear();
}

void InputState::set_mouse_position(float x, float y) noexcept {
    mouse_x_ = x;
    mouse_y_ = y;
}

void InputState::set_mouse_button(MouseButton button, bool down) noexcept {
    down_[index(button)] = down;
}

void InputState::add_wheel(float delta) noexcept {
    wheel_ += delta;
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
    const auto i = index(button);
    return down_[i] && !prev_down_[i];
}

bool InputState::mouse_released(MouseButton button) const noexcept {
    const auto i = index(button);
    return !down_[i] && prev_down_[i];
}

} // namespace mediaserver::gui
