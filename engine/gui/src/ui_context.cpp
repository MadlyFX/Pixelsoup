#include <mediaserver/gui/ui_context.hpp>

#include <mediaserver/gui/text.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace mediaserver::gui {
namespace {

constexpr UiId none = 0U;

// Left-aligned text, vertically centred in `bounds`, clipped to nothing here -
// the caller pushes a clip if the text can overflow.
void draw_label(
    UiDrawList& list,
    const FontAtlas& font,
    const Rect& bounds,
    float x,
    std::string_view text,
    UiColor color) {
    const float y = bounds.y + (bounds.height - font.line_height()) * 0.5F;
    static_cast<void>(draw_text(list, font, x, y, text, color));
}

// A number as a widget shows it: two decimals, which is the resolution an
// operator nudges a position or an opacity at. Written into a fixed buffer so
// drawing a frame of inspector fields allocates nothing.
struct Formatted final {
    std::array<char, 32U> buffer{};
    int length{};

    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view{buffer.data(), static_cast<std::size_t>(length)};
    }
};

[[nodiscard]] Formatted format_value(float value) noexcept {
    Formatted out{};
    out.length = std::snprintf(
        out.buffer.data(), out.buffer.size(), "%.2f", static_cast<double>(value));
    if (out.length < 0) {
        out.length = 0;
    }
    return out;
}

// A continuation byte is 10xxxxxx; a boundary is anything else. Walking by
// these rather than by bytes is what keeps an edit from splitting a code point
// in half, even though the ASCII atlas cannot draw a multi-byte one yet.
[[nodiscard]] bool is_continuation(char byte) noexcept {
    return (static_cast<core::u8>(byte) & 0xC0U) == 0x80U;
}

[[nodiscard]] std::size_t prev_boundary(
    const std::string& text, std::size_t index) noexcept {
    if (index == 0U) {
        return 0U;
    }
    --index;
    while (index > 0U && is_continuation(text[index])) {
        --index;
    }
    return index;
}

[[nodiscard]] std::size_t next_boundary(
    const std::string& text, std::size_t index) noexcept {
    if (index >= text.size()) {
        return text.size();
    }
    ++index;
    while (index < text.size() && is_continuation(text[index])) {
        ++index;
    }
    return index;
}

// The pixel width of `text` up to `index`.
[[nodiscard]] float width_to(
    const FontAtlas& font, const std::string& text, std::size_t index) {
    return font.measure(std::string_view{text}.substr(0U, index));
}

// The character boundary nearest `target_x`, so a click lands the caret between
// the two characters the operator aimed between rather than always after one.
[[nodiscard]] std::size_t caret_from_x(
    const FontAtlas& font,
    const std::string& text,
    float origin_x,
    float target_x) {
    float pen = origin_x;
    std::size_t best = 0U;
    float best_distance = std::fabs(target_x - pen);
    std::size_t index = 0U;
    while (index < text.size()) {
        const std::size_t next = next_boundary(text, index);
        for (std::size_t byte = index; byte < next; ++byte) {
            const Glyph* const g = font.glyph(text[byte]);
            if (g != nullptr) {
                pen += g->advance;
            }
        }
        index = next;
        const float distance = std::fabs(target_x - pen);
        if (distance < best_distance) {
            best_distance = distance;
            best = index;
        }
    }
    return best;
}

} // namespace

UiContext::UiContext(
    InputState& input,
    UiDrawList& list,
    const FontAtlas& font,
    Theme theme) noexcept
    : input_(input), list_(list), font_(font), theme_(theme) {
    // Solid fills sample the atlas's white texel; wire it once so panels and
    // widget bodies paint their exact colour.
    list_.set_white_texel(font_.white_u(), font_.white_v());
}

void UiContext::new_frame(double time_seconds) {
    hot_ = none;
    time_ = time_seconds;
    list_.reset();
}

void UiContext::push_clip(const Rect& bounds) {
    list_.push_clip(bounds.x, bounds.y, bounds.width, bounds.height);
}

void UiContext::pop_clip() {
    list_.pop_clip();
}

void UiContext::interact(
    UiId id,
    const Rect& bounds,
    bool& hovered_out,
    bool& pressed_out) noexcept {
    const bool over = bounds.contains(input_.mouse_x(), input_.mouse_y());
    // Hoverable only when nothing else has captured the pointer, so a drag that
    // wandered off one control does not light up another.
    const bool hovered = over && (active_ == none || active_ == id);
    if (hovered) {
        hot_ = id;
    }
    bool pressed = false;
    if (hot_ == id && active_ == none
        && input_.mouse_pressed(MouseButton::Left)) {
        active_ = id;
        pressed = true;
    }
    hovered_out = hovered;
    pressed_out = pressed;
}

Interaction UiContext::grab(UiId id, const Rect& bounds) noexcept {
    Interaction result{};
    interact(id, bounds, result.hovered, result.pressed);
    // Read before the release clears it, so a widget that follows the pointer
    // gets a final update at the position it was let go at.
    result.held = active_ == id;
    if (result.held && input_.mouse_released(MouseButton::Left)) {
        result.released = true;
        result.clicked = result.hovered;
        active_ = none;
    }
    return result;
}

void UiContext::panel(const Rect& bounds) {
    panel(bounds, theme_.panel);
}

void UiContext::panel(const Rect& bounds, UiColor color) {
    list_.add_rect(bounds.x, bounds.y, bounds.width, bounds.height, color);
}

void UiContext::label(const Rect& bounds, std::string_view text) {
    label(bounds, text, theme_.text);
}

void UiContext::label(const Rect& bounds, std::string_view text, UiColor color) {
    draw_label(list_, font_, bounds, bounds.x, text, color);
}

bool UiContext::button(UiId id, const Rect& bounds, std::string_view label) {
    const Interaction it = grab(id, bounds);
    const bool clicked = it.clicked;

    // Pressed-looking only while genuinely held: on the release frame it goes
    // back to the hover tint, which is what the eye expects of a click.
    const UiColor body = (it.held && !it.released)
        ? theme_.widget_active
        : (it.hovered ? theme_.widget_hover : theme_.widget);
    list_.add_rect(bounds.x, bounds.y, bounds.width, bounds.height, body);

    const float text_width = font_.measure(label);
    const float text_x = bounds.x + (bounds.width - text_width) * 0.5F;
    draw_label(list_, font_, bounds, text_x, label, theme_.text);
    return clicked;
}

bool UiContext::slider(
    UiId id, const Rect& bounds, float& value, float min, float max) {
    const Interaction it = grab(id, bounds);

    bool changed = false;
    if (it.held) {
        const float span = bounds.width > 0.0F ? bounds.width : 1.0F;
        const float t = std::clamp((input_.mouse_x() - bounds.x) / span, 0.0F, 1.0F);
        const float next = min + t * (max - min);
        if (next != value) {
            value = next;
            changed = true;
        }
    }

    list_.add_rect(bounds.x, bounds.y, bounds.width, bounds.height, theme_.track);
    const float fraction = max > min
        ? std::clamp((value - min) / (max - min), 0.0F, 1.0F)
        : 0.0F;
    // A knob marks the value; hover/active tints it so the grabbed control is
    // obvious.
    constexpr float knob_width = 6.0F;
    const float knob_x =
        bounds.x + fraction * bounds.width - knob_width * 0.5F;
    const UiColor knob = it.held
        ? theme_.accent
        : (it.hovered ? theme_.widget_hover : theme_.widget);
    list_.add_rect(knob_x, bounds.y, knob_width, bounds.height, knob);

    const Formatted text = format_value(value);
    const float text_x =
        bounds.x + (bounds.width - font_.measure(text.view())) * 0.5F;
    push_clip(bounds);
    draw_label(list_, font_, bounds, text_x, text.view(), theme_.text);
    pop_clip();
    return changed;
}

bool UiContext::drag_float(
    UiId id, const Rect& bounds, float& value, float speed) {
    const Interaction it = grab(id, bounds);

    bool changed = false;
    if (it.held) {
        const float dx = input_.mouse_dx();
        if (dx != 0.0F) {
            value += dx * speed;
            changed = true;
        }
    }

    const UiColor body = it.held
        ? theme_.widget_active
        : (it.hovered ? theme_.widget_hover : theme_.widget);
    list_.add_rect(bounds.x, bounds.y, bounds.width, bounds.height, body);

    // The number itself, centred: a drag field that does not show its value is
    // a rectangle the operator has to guess at.
    const Formatted text = format_value(value);
    const float text_x =
        bounds.x + (bounds.width - font_.measure(text.view())) * 0.5F;
    push_clip(bounds);
    draw_label(list_, font_, bounds, text_x, text.view(), theme_.text);
    pop_clip();
    return changed;
}

bool UiContext::checkbox(
    UiId id, const Rect& bounds, bool& value, std::string_view label) {
    const Interaction it = grab(id, bounds);

    bool toggled = false;
    if (it.clicked) {
        value = !value;
        toggled = true;
    }

    const float box = std::min(bounds.height, 18.0F);
    const Rect box_rect{
        bounds.x, bounds.y + (bounds.height - box) * 0.5F, box, box};
    const UiColor body = (it.held && !it.released)
        ? theme_.widget_active
        : (it.hovered ? theme_.widget_hover : theme_.widget);
    list_.add_rect(box_rect.x, box_rect.y, box, box, body);
    if (value) {
        const float pad = box * 0.25F;
        list_.add_rect(
            box_rect.x + pad,
            box_rect.y + pad,
            box - pad * 2.0F,
            box - pad * 2.0F,
            theme_.accent);
    }

    const float text_x = box_rect.x + box + theme_.padding;
    draw_label(list_, font_, bounds, text_x, label, theme_.text);
    return toggled;
}

void UiContext::take_focus(UiId id, const std::string& value) {
    focus_ = id;
    edit_original_ = value;
    blink_base_ = time_;
}

bool UiContext::edit_keys(std::string& value, bool& closed_out) {
    bool changed = false;
    closed_out = false;
    caret_ = std::min(caret_, value.size());

    for (const Key key : input_.keys()) {
        switch (key) {
        case Key::Backspace:
            if (caret_ > 0U) {
                const std::size_t from = prev_boundary(value, caret_);
                value.erase(from, caret_ - from);
                caret_ = from;
                changed = true;
            }
            break;
        case Key::Delete:
            if (caret_ < value.size()) {
                const std::size_t to = next_boundary(value, caret_);
                value.erase(caret_, to - caret_);
                changed = true;
            }
            break;
        case Key::Left:
            caret_ = prev_boundary(value, caret_);
            break;
        case Key::Right:
            caret_ = next_boundary(value, caret_);
            break;
        case Key::Home:
            caret_ = 0U;
            break;
        case Key::End:
            caret_ = value.size();
            break;
        case Key::Enter:
        case Key::Tab:
            closed_out = true;
            break;
        case Key::Escape:
            // Cancel: put back what the field held when the edit began.
            if (value != edit_original_) {
                value = edit_original_;
                changed = true;
            }
            closed_out = true;
            break;
        }
        if (closed_out) {
            break;
        }
    }

    if (!closed_out) {
        const std::string_view typed = input_.text();
        if (!typed.empty()) {
            value.insert(caret_, typed);
            caret_ += typed.size();
            changed = true;
        }
    }

    // Any keyboard activity holds the caret solid, so it never blinks out from
    // under someone mid-word.
    if (!input_.keys().empty() || !input_.text().empty()) {
        blink_base_ = time_;
    }
    return changed;
}

bool UiContext::text_field(UiId id, const Rect& bounds, std::string& value) {
    const Interaction it = grab(id, bounds);

    const float inset = theme_.padding;
    const Rect inner{
        bounds.x + inset,
        bounds.y,
        std::max(bounds.width - inset * 2.0F, 0.0F),
        bounds.height};

    bool changed = false;
    if (it.pressed) {
        if (focus_ != id) {
            take_focus(id, value);
        }
        caret_ = caret_from_x(font_, value, inner.x, input_.mouse_x());
        blink_base_ = time_;
    } else if (focus_ == id && input_.mouse_pressed(MouseButton::Left)
               && !it.hovered) {
        // A press anywhere else commits the edit, the way clicking off a field
        // does everywhere else.
        focus_ = none;
    }

    if (focus_ == id) {
        bool closed = false;
        changed = edit_keys(value, closed);
        if (closed) {
            focus_ = none;
        }
    }

    const bool focused = focus_ == id;
    // A focused field reads as an accent-framed inset well: the frame is the
    // outer rect showing through a one-pixel inset body.
    const UiColor frame = focused
        ? theme_.accent
        : (it.hovered ? theme_.widget_hover : theme_.widget);
    list_.add_rect(bounds.x, bounds.y, bounds.width, bounds.height, frame);
    list_.add_rect(
        bounds.x + 1.0F,
        bounds.y + 1.0F,
        std::max(bounds.width - 2.0F, 0.0F),
        std::max(bounds.height - 2.0F, 0.0F),
        theme_.track);

    const std::size_t caret = std::min(caret_, value.size());
    const float caret_x = focused ? width_to(font_, value, caret) : 0.0F;
    // Keep the caret in view by sliding the text left once it runs past the
    // right edge; an unfocused field always shows its start.
    const float scroll =
        focused ? std::max(caret_x - inner.width, 0.0F) : 0.0F;

    push_clip(inner);
    draw_label(list_, font_, bounds, inner.x - scroll, value, theme_.text);
    if (focused) {
        const bool visible = std::fmod(time_ - blink_base_, 1.0) < 0.5;
        if (visible) {
            const float height = font_.line_height();
            list_.add_rect(
                inner.x + caret_x - scroll,
                bounds.y + (bounds.height - height) * 0.5F,
                1.0F,
                height,
                theme_.text);
        }
    }
    pop_clip();
    return changed;
}

Split UiContext::split(
    UiId id,
    const Rect& bounds,
    float& fraction,
    Axis axis,
    float thickness) {
    const bool horizontal = axis == Axis::Row;
    const float extent = horizontal ? bounds.width : bounds.height;
    const float origin = horizontal ? bounds.x : bounds.y;

    fraction = clamp_split(bounds, fraction, axis);
    Split result = split_rects(bounds, fraction, axis, thickness);

    const Interaction it = grab(id, result.divider);
    if (it.held && extent > 0.0F) {
        const float pointer = horizontal ? input_.mouse_x() : input_.mouse_y();
        fraction = clamp_split(bounds, (pointer - origin) / extent, axis);
        result = split_rects(bounds, fraction, axis, thickness);
    }

    const UiColor body = it.held
        ? theme_.accent
        : (it.hovered ? theme_.widget_hover : theme_.divider);
    list_.add_rect(
        result.divider.x,
        result.divider.y,
        result.divider.width,
        result.divider.height,
        body);
    return result;
}

} // namespace mediaserver::gui
