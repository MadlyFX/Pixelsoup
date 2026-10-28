#include <mediaserver/gui/ui_context.hpp>

#include <mediaserver/gui/color_picker.hpp>

#include <mediaserver/gui/shapes.hpp>
#include <mediaserver/gui/text.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace mediaserver::gui {
namespace {

constexpr UiId none = 0U;

// Room for every value eased at once on a busy screen - each hovered
// control, each visible timeline row - with plenty of slack for probing.
// A power of two, so an id's home slot is a mask.
constexpr std::size_t motion_slots = 2048U;
constexpr std::size_t motion_probe = 24U;

// Left-aligned text, vertically centred in `bounds`, clipped to nothing here -
// the caller pushes a clip if the text can overflow.
void draw_label(
    UiDrawList& list,
    const FontAtlas& font,
    const Rect& bounds,
    float x,
    std::string_view text,
    UiColor color,
    float size = 1.0F) {
    const float y = bounds.y + (bounds.height - font.line_height(size)) * 0.5F;
    static_cast<void>(draw_text(list, font, x, y, text, color, size));
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
                pen += g->advance / font.scale();
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

std::string sentence_case(std::string_view text) {
    std::string out{text};
    bool first = true;
    std::size_t index = 0U;
    while (index < out.size()) {
        const auto letter = [&out](std::size_t at) {
            return at < out.size() && ((out[at] >= 'A' && out[at] <= 'Z') ||
                                       (out[at] >= 'a' && out[at] <= 'z'));
        };
        if (!letter(index)) {
            ++index;
            continue;
        }
        std::size_t end = index;
        while (letter(end)) {
            ++end;
        }
        // Short all-capital words after the first are abbreviations.
        const bool abbreviation = !first && end - index <= 2U;
        if (!abbreviation) {
            for (std::size_t at = first ? index + 1U : index; at < end; ++at) {
                if (out[at] >= 'A' && out[at] <= 'Z') {
                    out[at] = static_cast<char>(out[at] - 'A' + 'a');
                }
            }
        }
        first = false;
        index = end;
    }
    return out;
}

UiContext::UiContext(
    InputState& input,
    UiDrawList& list,
    const FontAtlas& font,
    Theme theme) noexcept
    : input_(input), list_(list), font_(font), theme_(theme),
      motions_(motion_slots) {
    // Solid fills sample the atlas's white texel; wire it once so panels and
    // widget bodies paint their exact colour.
    list_.set_white_texel(font_.white_u(), font_.white_v());
}

void UiContext::new_frame(double time_seconds) {
    hot_ = none;
    press_consumed_ = false;
    cursor_ = Cursor::Arrow;
    // A stalled frame is not allowed to teleport everything to its target in
    // one step, nor a clock that went backwards to run motion in reverse.
    frame_dt_ = static_cast<float>(std::clamp(time_seconds - time_, 0.0, 0.1));
    ++frame_;
    motion_last_frame_ = motion_this_frame_;
    motion_this_frame_ = false;
    time_ = time_seconds;
    pointer_block_depth_ = 0U;
    interaction_clip_stack_.clear();
    // A capture whose widget was not drawn last frame - a wire removed by the
    // press that captured it, say - would never see its release and would
    // keep every other control from hovering or pressing. Once the button is
    // up it is let go.
    if (active_ != none && !active_seen_
        && !input_.mouse_down(MouseButton::Left)) {
        active_ = none;
    }
    active_seen_ = false;
    list_.reset();
    tooltip_text_.clear();
    dropdown_dismissed_ = false;
    if (color_.id != none) {
        // Escape puts back the colour the popover opened with; Enter keeps
        // the new one (unless a field inside is taking it); a press outside
        // the popover and its well keeps it too, and is spent closing it.
        color_.seen = false;
        // Only the popover's own fields can hold the keyboard while it is
        // open, and Enter in one of them commits what was typed instead.
        for (const Key key : input_.keys()) {
            if (key == Key::Escape) {
                color_.value = color_.original;
                color_.pending = true;
                color_.closing = true;
                focus_ = none;
            } else if (key == Key::Enter && focus_ == none) {
                color_.closing = true;
            }
        }
        if (input_.mouse_pressed(MouseButton::Left) &&
            !color_.bounds.contains(pointer_press_x(), pointer_press_y())) {
            color_.closing = true;
            press_consumed_ = true;
            dropdown_dismissed_ = true;
        }
    }
    dropdown_result_id_ = none;
    dropdown_seen_ = false;
    if (dropdown_id_ != none && !dropdown_options_.empty()) {
        const auto close = [&](bool accept) {
            if (accept) {
                dropdown_result_id_ = dropdown_id_;
                dropdown_result_ = dropdown_highlight_;
            }
            dropdown_id_ = none;
            dropdown_dismissed_ = true;
            active_ = none;
        };
        for (const Key key : input_.keys()) {
            if (key == Key::Escape || key == Key::Tab) { close(false); break; }
            if (key == Key::Enter) { close(true); break; }
            if (key == Key::Up && dropdown_highlight_ >= dropdown_options_.size()) {
                dropdown_highlight_ = dropdown_options_.size() - 1U;
            } else if (key == Key::Up && dropdown_highlight_ > 0U) {
                --dropdown_highlight_;
            }
            if (key == Key::Down && dropdown_highlight_ + 1U < dropdown_options_.size()) ++dropdown_highlight_;
        }
        if (dropdown_id_ != none) {
            const bool inside = dropdown_bounds_.contains(input_.mouse_x(), input_.mouse_y());
            const std::size_t visible = std::max(std::size_t{1}, static_cast<std::size_t>(dropdown_bounds_.height / menu_row_height));
            const std::size_t maximum = dropdown_options_.size() > visible ? dropdown_options_.size() - visible : 0U;
            if (inside && input_.wheel() != 0.0F) {
                const auto first = static_cast<int>(dropdown_first_) - static_cast<int>(input_.wheel() * 3.0F);
                dropdown_first_ = static_cast<std::size_t>(std::clamp(first, 0, static_cast<int>(maximum)));
            }
            if (inside && (input_.mouse_dx() != 0.0F || input_.mouse_dy() != 0.0F || input_.mouse_pressed(MouseButton::Left))) {
                dropdown_highlight_ = std::min(dropdown_first_ + static_cast<std::size_t>((input_.mouse_y() - dropdown_bounds_.y) / menu_row_height), dropdown_options_.size() - 1U);
            }
            if (input_.mouse_pressed(MouseButton::Left)) {
                press_consumed_ = true;
                if (dropdown_bounds_.contains(pointer_press_x(), pointer_press_y())) {
                    dropdown_pressed_ = std::min(dropdown_first_ + static_cast<std::size_t>(
                        (pointer_press_y() - dropdown_bounds_.y) / menu_row_height), dropdown_options_.size() - 1U);
                }
                else close(false);
            }
            // A second right click dismisses rather than stacking menus; the
            // operator right-clicks again for the menu of what is under it.
            if (dropdown_id_ != none && input_.mouse_pressed(MouseButton::Right)) {
                close(false);
            }
            if (dropdown_id_ != none && input_.mouse_released(MouseButton::Left)) {
                if (dropdown_bounds_.contains(pointer_release_x(), pointer_release_y())) {
                    const auto released = std::min(dropdown_first_ + static_cast<std::size_t>(
                        (pointer_release_y() - dropdown_bounds_.y) / menu_row_height), dropdown_options_.size() - 1U);
                    if (dropdown_pressed_ == released &&
                        (!input_.mouse_pressed(MouseButton::Left) ||
                         input_.mouse_release_follows_press(MouseButton::Left))) {
                        dropdown_highlight_ = released;
                        close(true);
                    }
                }
                dropdown_pressed_ = static_cast<std::size_t>(-1);
            }
            if (!input_.keys().empty()) {
                if (dropdown_highlight_ < dropdown_first_) dropdown_first_ = dropdown_highlight_;
                if (dropdown_highlight_ >= dropdown_first_ + visible) dropdown_first_ = dropdown_highlight_ - visible + 1U;
            }
        }
    }
}

void UiContext::push_clip(const Rect& bounds) {
    Rect clipped = bounds;
    if (!interaction_clip_stack_.empty()) {
        const Rect& parent = interaction_clip_stack_.back();
        const float left = std::max(parent.x, bounds.x);
        const float top = std::max(parent.y, bounds.y);
        const float right = std::min(
            parent.x + std::max(parent.width, 0.0F),
            bounds.x + std::max(bounds.width, 0.0F));
        const float bottom = std::min(
            parent.y + std::max(parent.height, 0.0F),
            bounds.y + std::max(bounds.height, 0.0F));
        clipped = Rect{
            left, top, std::max(right - left, 0.0F),
            std::max(bottom - top, 0.0F)};
    } else {
        clipped.width = std::max(clipped.width, 0.0F);
        clipped.height = std::max(clipped.height, 0.0F);
    }
    interaction_clip_stack_.push_back(clipped);
    list_.push_clip(bounds.x, bounds.y, bounds.width, bounds.height);
}

void UiContext::pop_clip() {
    if (!interaction_clip_stack_.empty()) {
        interaction_clip_stack_.pop_back();
    }
    list_.pop_clip();
}

void UiContext::interact(
    UiId id,
    const Rect& bounds,
    bool& hovered_out,
    bool& pressed_out) noexcept {
    const bool inside_clip = interaction_clip_stack_.empty()
        || interaction_clip_stack_.back().contains(
            input_.mouse_x(), input_.mouse_y());
    const bool over = !pointer_blocked() && inside_clip
        && bounds.contains(input_.mouse_x(), input_.mouse_y());
    // Hoverable only when nothing else has captured the pointer, so a drag that
    // wandered off one control does not light up another.
    const bool hovered = over && (active_ == none || active_ == id);
    if (hovered) {
        hot_ = id;
    }
    bool pressed = false;
    const bool press_inside_clip = interaction_clip_stack_.empty()
        || interaction_clip_stack_.back().contains(pointer_press_x(), pointer_press_y());
    if (!pointer_blocked() && press_inside_clip && active_ == none
        && !press_consumed_ && input_.mouse_pressed(MouseButton::Left)
        && bounds.contains(pointer_press_x(), pointer_press_y())) {
        active_ = id;
        press_consumed_ = true;
        pressed = true;
    }
    active_seen_ = active_seen_ || (active_ != none && active_ == id);
    hovered_out = hovered;
    pressed_out = pressed;
}

void UiContext::push_pointer_block() noexcept {
    ++pointer_block_depth_;
}

void UiContext::pop_pointer_block() noexcept {
    if (pointer_block_depth_ != 0U) {
        --pointer_block_depth_;
    }
}

Interaction UiContext::grab(UiId id, const Rect& bounds) noexcept {
    Interaction result{};
    interact(id, bounds, result.hovered, result.pressed);
    // Read before the release clears it, so a widget that follows the pointer
    // gets a final update at the position it was let go at.
    result.held = active_ == id;
    if (result.held && input_.mouse_released(MouseButton::Left) &&
        (!result.pressed || input_.mouse_release_follows_press(MouseButton::Left))) {
        result.released = true;
        result.clicked = !pointer_blocked()
            && bounds.contains(pointer_release_x(), pointer_release_y())
            && (interaction_clip_stack_.empty() || interaction_clip_stack_.back().contains(
                pointer_release_x(), pointer_release_y()));
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

void UiContext::outline(const Rect& bounds, UiColor color, float thickness) {
    add_outline(list_, bounds, thickness, color);
}

void UiContext::rounded(const Rect& bounds, UiColor color) {
    rounded(bounds, color, theme_.radius);
}

void UiContext::rounded(const Rect& bounds, UiColor color, float radius,
                        core::u8 corners) {
    list_.add_rounded_rect(bounds.x, bounds.y, bounds.width, bounds.height,
                           radius, color, corners);
}

void UiContext::rounded_outline(const Rect& bounds, UiColor color,
                                float radius, float thickness) {
    list_.add_rounded_outline(bounds.x, bounds.y, bounds.width, bounds.height,
                              radius, thickness, color);
}

void UiContext::glow(const Rect& bounds, UiColor color, float radius,
                     float spread) {
    if (theme_.glow <= 0.0F) {
        return;
    }
    list_.add_rounded_glow(bounds.x, bounds.y, bounds.width, bounds.height,
                           radius, spread, fade(color, color.a * theme_.glow));
}

void UiContext::shadow(const Rect& bounds, float radius, float spread) {
    list_.add_rounded_glow(bounds.x, bounds.y + 3.0F, bounds.width,
                           bounds.height, radius, spread,
                           UiColor{0.0F, 0.0F, 0.0F, 0.45F});
}

UiContext::Motion* UiContext::motion(UiId id, bool& fresh_out) noexcept {
    fresh_out = false;
    if (motions_.empty()) {
        return nullptr;
    }
    const std::size_t mask = motions_.size() - 1U;
    Motion* reusable = nullptr;
    for (std::size_t probe = 0U; probe < motion_probe; ++probe) {
        Motion& slot = motions_[(static_cast<std::size_t>(id) + probe) & mask];
        // Live means asked for this frame or the one before.
        const bool live = slot.id != none && slot.frame + 1U >= frame_;
        if (slot.id == id) {
            if (live) {
                return &slot;
            }
            reusable = &slot;
            break;
        }
        if (!live && reusable == nullptr) {
            reusable = &slot;
        }
    }
    if (reusable == nullptr) {
        return nullptr;
    }
    reusable->id = id;
    reusable->velocity = 0.0F;
    reusable->frame = frame_;
    fresh_out = true;
    return reusable;
}

float UiContext::ease(UiId id, float target, float speed) {
    bool fresh = false;
    Motion* const slot = motion(id, fresh);
    if (slot == nullptr) {
        return target;
    }
    const float rate = 22.0F * speed * theme_.motion;
    if (fresh || frame_dt_ <= 0.0F || rate <= 0.0F || !std::isfinite(slot->value)) {
        slot->value = target;
        slot->frame = frame_;
        return target;
    }
    if (slot->frame == frame_) {
        // Already stepped this frame by an earlier call.
        return slot->value;
    }
    slot->frame = frame_;
    slot->value += (target - slot->value) *
                   (1.0F - std::exp(-rate * frame_dt_));
    if (std::abs(target - slot->value) < 0.01F) {
        slot->value = target;
    } else {
        motion_this_frame_ = true;
    }
    return slot->value;
}

float UiContext::spring(UiId id, float target) {
    bool fresh = false;
    Motion* const slot = motion(id, fresh);
    if (slot == nullptr) {
        return target;
    }
    if (fresh || frame_dt_ <= 0.0F || theme_.motion <= 0.0F ||
        !std::isfinite(slot->value)) {
        slot->value = target;
        slot->velocity = 0.0F;
        slot->frame = frame_;
        return target;
    }
    if (slot->frame == frame_) {
        return slot->value;
    }
    slot->frame = frame_;
    // A lightly underdamped spring: it lands in about a fifth of a second
    // and overshoots by a few percent, enough to feel physical without
    // wobbling. Stepped at 240 Hz so a slow frame cannot destabilise it.
    const float stiffness = 520.0F * theme_.motion * theme_.motion;
    const float damping = 2.0F * std::sqrt(stiffness) * 0.6F;
    float remaining = frame_dt_;
    while (remaining > 0.0F) {
        const float step = std::min(remaining, 1.0F / 240.0F);
        const float force = (target - slot->value) * stiffness -
                            slot->velocity * damping;
        slot->velocity += force * step;
        slot->value += slot->velocity * step;
        remaining -= step;
    }
    if (std::abs(target - slot->value) < 0.001F &&
        std::abs(slot->velocity) < 0.01F) {
        slot->value = target;
        slot->velocity = 0.0F;
    } else {
        motion_this_frame_ = true;
    }
    return slot->value;
}

void UiContext::settle(UiId id, float value) {
    bool fresh = false;
    Motion* const slot = motion(id, fresh);
    if (slot == nullptr) {
        return;
    }
    slot->value = value;
    slot->velocity = 0.0F;
    // Leave the frame one behind, so this frame's ease() or spring() still
    // takes its step from here instead of treating the slot as done.
    slot->frame = frame_ - 1U;
}

void UiContext::label(const Rect& bounds, std::string_view text) {
    label(bounds, text, theme_.text);
}

void UiContext::label(const Rect& bounds, std::string_view text, UiColor color) {
    label(bounds, text, color, 1.0F);
}

void UiContext::label(
    const Rect& bounds, std::string_view text, UiColor color, float size) {
    if (bounds.width <= 0.0F || bounds.height <= 0.0F) return;
    list_.push_clip(bounds.x, bounds.y, std::max(bounds.width, 0.0F), std::max(bounds.height, 0.0F));
    if (font_.measure(text, size) > bounds.width) {
        // Never leave a half glyph at a dock edge. Keep the complete label on
        // hover, and shorten on UTF-8 boundaries for the visible line.
        constexpr std::string_view ellipsis{"..."};
        const float room = bounds.width - font_.measure(ellipsis, size);
        std::size_t end = 0U;
        float used = 0.0F;
        while (end < text.size()) {
            std::size_t next = end + 1U;
            while (next < text.size() && is_continuation(text[next])) ++next;
            const float advance = font_.measure(text.substr(end, next - end), size);
            if (used + advance > room) break;
            used += advance;
            end = next;
        }
        if (room >= 0.0F) {
            draw_label(list_, font_, bounds, bounds.x, text.substr(0U, end), color, size);
            draw_label(list_, font_, bounds, bounds.x + used, ellipsis, color, size);
        }
        tooltip(bounds, text);
    } else {
        draw_label(list_, font_, bounds, bounds.x, text, color, size);
    }
    list_.pop_clip();
}

void UiContext::label_centered(
    const Rect& bounds, std::string_view text, UiColor color) {
    const float text_width = font_.measure(text, 1.0F);
    if (text_width > bounds.width) {
        label(bounds, text, color);
        return;
    }
    if (bounds.height <= 0.0F) return;
    list_.push_clip(bounds.x, bounds.y, bounds.width, bounds.height);
    draw_label(list_, font_, bounds, bounds.x + (bounds.width - text_width) * 0.5F,
               text, color, 1.0F);
    list_.pop_clip();
}

void UiContext::tooltip(const Rect& bounds, std::string_view text) {
    if (!pointer_blocked() && !text.empty() && bounds.contains(pointer_x(), pointer_y()) &&
        (interaction_clip_stack_.empty() || interaction_clip_stack_.back().contains(pointer_x(), pointer_y()))) {
        tooltip_text_ = text;
    }
}

void UiContext::image(const Rect& bounds, const rhi::Texture& texture) {
    image(bounds, texture, UiColor{1.0F, 1.0F, 1.0F, 1.0F});
}

void UiContext::image(
    const Rect& bounds, const rhi::Texture& texture, UiColor tint) {
    list_.add_image(bounds.x, bounds.y, bounds.width, bounds.height, texture, tint);
}

Rect UiContext::image_fit(
    const Rect& bounds, const rhi::Texture& texture, float aspect) {
    if (bounds.width <= 0.0F || bounds.height <= 0.0F || aspect <= 0.0F) {
        return Rect{bounds.x, bounds.y, 0.0F, 0.0F};
    }
    // Fit inside, never crop: whichever axis runs out first sets the size, and
    // the bars fall either side of centre.
    float width = bounds.width;
    float height = width / aspect;
    if (height > bounds.height) {
        height = bounds.height;
        width = height * aspect;
    }
    const Rect placed{
        bounds.x + (bounds.width - width) * 0.5F,
        bounds.y + (bounds.height - height) * 0.5F,
        width,
        height};
    image(placed, texture);
    return placed;
}

bool UiContext::button(UiId id, const Rect& bounds, std::string_view label) {
    const Interaction it = grab(id, bounds);
    cursor_for(it, Cursor::Hand);
    const bool clicked = it.clicked;
    const bool pressed = it.held && !it.released;

    // A pill whose tint eases in under the pointer. Pressed-looking only
    // while genuinely held: on the release frame it goes back to the hover
    // tint, which is what the eye expects of a click, and it sinks a pixel
    // while held so the press is felt as well as seen.
    const float hover =
        ease(id ^ ui_id("button.hover"), it.hovered || it.held ? 1.0F : 0.0F);
    const UiColor body = pressed
        ? theme_.widget_active
        : blend(theme_.widget, theme_.widget_hover, hover);
    const Rect shape = pressed ? bounds.inflated(-1.0F, -1.0F) : bounds;
    rounded(shape, body, shape.height * 0.5F);

    const float text_width = font_.measure(label);
    const float text_x = bounds.x + (bounds.width - text_width) * 0.5F;
    draw_label(list_, font_, bounds, text_x, label, theme_.text);
    return clicked;
}

bool UiContext::slider(
    UiId id, const Rect& bounds, float& value, float min, float max) {
    const Interaction it = grab(id, bounds);
    cursor_for(it, Cursor::Hand);

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

    // A soft groove filled up to the value, and a round knob riding it.
    rounded(bounds, theme_.track, bounds.height * 0.5F);
    const float fraction = max > min
        ? std::clamp((value - min) / (max - min), 0.0F, 1.0F)
        : 0.0F;
    const float knob = std::min(bounds.height - 4.0F, 16.0F);
    const float travel = std::max(bounds.width - knob - 4.0F, 0.0F);
    const float knob_x = bounds.x + 2.0F + fraction * travel;
    const Rect filled{bounds.x, bounds.y,
                      knob_x - bounds.x + knob * 0.5F, bounds.height};
    rounded(filled, fade(theme_.accent, it.held ? 0.42F : 0.28F),
            bounds.height * 0.5F);
    const Rect knob_rect{knob_x, bounds.y + (bounds.height - knob) * 0.5F,
                         knob, knob};
    rounded(knob_rect, it.held || it.hovered ? theme_.text
                                             : blend(theme_.text_muted, theme_.text, 0.6F),
            knob * 0.5F);

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
    cursor_for(it, Cursor::ResizeHorizontal);

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
    rounded(bounds, body, theme_.radius_small);

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

    cursor_for(it, Cursor::Hand);
    const float box = std::min(bounds.height - 4.0F, 16.0F);
    const Rect box_rect{
        bounds.x, bounds.y + (bounds.height - box) * 0.5F, box, box};
    const UiColor body = (it.held && !it.released)
        ? theme_.widget_active
        : (it.hovered ? theme_.widget_hover : theme_.widget);
    // A soft square that fills with the accent when on, ticked in the
    // panel's own dark so the mark reads like a lit key.
    if (value) {
        rounded(box_rect, theme_.accent, theme_.radius_small);
        const std::array<UiPoint, 3U> tick{
            UiPoint{box_rect.x + box * 0.24F, box_rect.y + box * 0.52F},
            UiPoint{box_rect.x + box * 0.43F, box_rect.y + box * 0.71F},
            UiPoint{box_rect.x + box * 0.78F, box_rect.y + box * 0.31F}};
        list_.add_polyline(tick, 1.8F, theme_.background);
    } else {
        rounded(box_rect, body, theme_.radius_small);
        rounded_outline(box_rect, blend(theme_.border, theme_.text_muted, 0.5F),
                        theme_.radius_small);
    }

    const float text_x = box_rect.x + box + theme_.padding;
    list_.push_clip(bounds.x, bounds.y, bounds.width, bounds.height);
    draw_label(list_, font_, bounds, text_x, label, theme_.text);
    list_.pop_clip();
    return toggled;
}

bool UiContext::dropdown(UiId id, const Rect& bounds,
                         std::span<const std::string_view> options,
                         std::size_t& selected) {
    if (options.empty()) return false;
    bool changed = false;
    if (dropdown_result_id_ == id && dropdown_result_ < options.size()) {
        changed = selected != dropdown_result_;
        selected = dropdown_result_;
        dropdown_result_id_ = none;
    }
    const Interaction it = grab(id, bounds);
    if (it.clicked) {
        clear_focus();
        dropdown_id_ = id;
        dropdown_highlight_ = std::min(selected, options.size() - 1U);
        dropdown_first_ = dropdown_highlight_ > 7U ? dropdown_highlight_ - 7U : 0U;
        dropdown_pressed_ = static_cast<std::size_t>(-1);
    }
    if (dropdown_id_ == id) {
        dropdown_seen_ = true;
        dropdown_anchor_ = bounds;
        dropdown_options_.clear();
        dropdown_swatches_.clear();
        dropdown_checks_.clear();
        for (const auto option : options) dropdown_options_.emplace_back(option);
    }
    cursor_for(it, Cursor::Hand);
    const bool open = dropdown_id_ == id;
    rounded(bounds, it.hovered || open ? theme_.widget_hover : theme_.widget,
            theme_.radius_small);
    if (open) {
        rounded_outline(bounds, fade(theme_.accent, 0.7F), theme_.radius_small);
    }
    label({bounds.x + 8.0F, bounds.y, std::max(bounds.width - 27.0F, 0.0F), bounds.height},
          selected < options.size() ? options[selected] : std::string_view{"Mixed"});
    // A soft chevron, pointing up while the menu hangs open.
    const float x = bounds.right() - 11.0F;
    const float y = bounds.y + bounds.height * 0.5F;
    const float lift = open ? -1.0F : 1.0F;
    const std::array<UiPoint, 3U> chevron{
        UiPoint{x - 3.5F, y - 1.75F * lift}, UiPoint{x, y + 1.75F * lift},
        UiPoint{x + 3.5F, y - 1.75F * lift}};
    list_.add_polyline(chevron, 1.4F, theme_.text_muted);
    return changed;
}

namespace {

[[nodiscard]] UiColor clamped(UiColor color) noexcept {
    return UiColor{std::clamp(color.r, 0.0F, 1.0F), std::clamp(color.g, 0.0F, 1.0F),
                   std::clamp(color.b, 0.0F, 1.0F), color.a};
}

[[nodiscard]] int channel_byte(float value) noexcept {
    return static_cast<int>(std::lround(std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

[[nodiscard]] std::string hex_text(UiColor color) {
    std::array<char, 16U> text{};
    const int length = std::snprintf(text.data(), text.size(), "#%02X%02X%02X",
                                     channel_byte(color.r), channel_byte(color.g),
                                     channel_byte(color.b));
    return std::string{text.data(), static_cast<std::size_t>(std::max(length, 0))};
}

// "#RRGGBB", "RRGGBB" or "#RGB", any case; anything else is not a colour.
[[nodiscard]] std::optional<UiColor> parse_hex(std::string_view text,
                                               float alpha) noexcept {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1U);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1U);
    if (!text.empty() && text.front() == '#') text.remove_prefix(1U);
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::array<int, 3U> bytes{};
    if (text.size() == 6U) {
        for (std::size_t index = 0U; index < 3U; ++index) {
            const int high = digit(text[index * 2U]);
            const int low = digit(text[index * 2U + 1U]);
            if (high < 0 || low < 0) return std::nullopt;
            bytes[index] = high * 16 + low;
        }
    } else if (text.size() == 3U) {
        for (std::size_t index = 0U; index < 3U; ++index) {
            const int nibble = digit(text[index]);
            if (nibble < 0) return std::nullopt;
            bytes[index] = nibble * 17;
        }
    } else {
        return std::nullopt;
    }
    return UiColor{static_cast<float>(bytes[0]) / 255.0F,
                   static_cast<float>(bytes[1]) / 255.0F,
                   static_cast<float>(bytes[2]) / 255.0F, alpha};
}

// A whole number 0..255, or nothing.
[[nodiscard]] std::optional<float> parse_channel(std::string_view text) noexcept {
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1U);
    while (!text.empty() && text.back() == ' ') text.remove_suffix(1U);
    if (text.empty() || text.size() > 3U) return std::nullopt;
    int value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + (c - '0');
    }
    if (value > 255) return std::nullopt;
    return static_cast<float>(value) / 255.0F;
}

[[nodiscard]] bool same_rgb(UiColor a, UiColor b) noexcept {
    return std::abs(a.r - b.r) < 1.0e-6F && std::abs(a.g - b.g) < 1.0e-6F &&
           std::abs(a.b - b.b) < 1.0e-6F;
}

// The colours every wall operator reaches for: the primaries and secondaries
// of an LED, and white.
constexpr std::array<UiColor, 7U> preset_colors{{
    {1.0F, 0.0F, 0.0F, 1.0F},
    {0.0F, 1.0F, 0.0F, 1.0F},
    {0.0F, 0.0F, 1.0F, 1.0F},
    {0.0F, 1.0F, 1.0F, 1.0F},
    {1.0F, 0.0F, 1.0F, 1.0F},
    {1.0F, 1.0F, 0.0F, 1.0F},
    {1.0F, 1.0F, 1.0F, 1.0F},
}};

constexpr float popover_width = 264.0F;
constexpr float popover_height = 318.0F;

} // namespace

bool UiContext::color_well(UiId id, const Rect& bounds, UiColor& value) {
    bool changed = false;
    const bool open = color_.id != none && color_.id == id;
    if (open) {
        color_.seen = true;
        color_.anchor = bounds;
        if (color_.pending) {
            changed = !same_rgb(value, color_.value);
            value.r = color_.value.r;
            value.g = color_.value.g;
            value.b = color_.value.b;
            color_.pending = false;
        }
        if (color_.closing) {
            // Remembered for next time, newest first and never twice.
            if (!same_rgb(color_.original, value)) {
                std::size_t found = recent_count_;
                for (std::size_t index = 0U; index < recent_count_; ++index) {
                    if (same_rgb(recent_colors_[index], value)) found = index;
                }
                const std::size_t shift =
                    found < recent_count_ ? found
                                          : std::min(recent_count_, recent_colors_.size() - 1U);
                for (std::size_t index = shift; index > 0U; --index) {
                    recent_colors_[index] = recent_colors_[index - 1U];
                }
                recent_colors_[0U] = UiColor{value.r, value.g, value.b, 1.0F};
                if (found >= recent_count_) {
                    recent_count_ = std::min(recent_count_ + 1U, recent_colors_.size());
                }
            }
            focus_ = none;
            color_ = ColorEdit{};
        }
    }

    const Interaction it = grab(id, bounds);
    cursor_for(it, Cursor::Hand);
    if (it.clicked && color_.id == none) {
        clear_focus();
        color_ = ColorEdit{};
        color_.id = id;
        color_.anchor = bounds;
        color_.original = value;
        color_.value = value;
        color_.hue = rgb_to_hsv(value).hue;
        color_.seen = true;
    }

    // The swatch: the colour itself in a soft well, ringed while its
    // picker is open.
    const bool showing = color_.id == id;
    const float radius = std::min(theme_.radius_small + 1.0F, bounds.height * 0.5F);
    rounded(bounds, clamped(UiColor{value.r, value.g, value.b, 1.0F}), radius);
    rounded_outline(bounds,
                    showing ? theme_.accent
                            : (it.hovered ? fade(theme_.text, 0.55F)
                                          : fade(theme_.text, 0.18F)),
                    radius, showing ? 1.5F : 1.0F);
    tooltip(bounds, hex_text(value) + "  Click to pick a colour");
    return changed;
}

void UiContext::draw_color_popover(const Rect& window_bounds) {
    if (color_.id == none) {
        return;
    }
    if (!color_.seen) {
        // Its well was not drawn this frame: the owner is gone.
        color_ = ColorEdit{};
        return;
    }
    // Under the well, or above it where there is no room below; always
    // wholly inside the window.
    float x = std::clamp(color_.anchor.x, window_bounds.x + 8.0F,
                         std::max(window_bounds.x + 8.0F,
                                  window_bounds.right() - popover_width - 8.0F));
    float y = color_.anchor.bottom() + 8.0F;
    if (y + popover_height > window_bounds.bottom() - 8.0F) {
        y = color_.anchor.y - popover_height - 8.0F;
    }
    y = std::max(y, window_bounds.y + 8.0F);
    const Rect card{x, y, popover_width, popover_height};
    // Includes the well, so a press on it is not an outside press.
    color_.bounds = card;
    in_color_popover_ = true;

    shadow(card, theme_.radius_large, 22.0F);
    rounded(card, theme_.header, theme_.radius_large);
    rounded_outline(card, theme_.border, theme_.radius_large);

    constexpr float pad = 14.0F;
    Rect area{card.x + pad, card.y + pad, card.width - pad * 2.0F,
              card.height - pad * 2.0F};
    const auto take = [&area](float height) {
        const Rect row{area.x, area.y, area.width, height};
        area.y += height;
        area.height -= height;
        return row;
    };
    UiColor next = color_.value;
    bool edited = false;

    // Hue and saturation/value. A grey has no hue of its own, so the hue
    // the operator last chose is kept while the colour is grey.
    Hsv hsv = rgb_to_hsv(color_.value);
    if (hsv.saturation <= 1.0e-6F) {
        hsv.hue = color_.hue;
    }
    const Hsv before = hsv;
    const bool held = hsv_fields(*this, color_.id ^ ui_id("color.fields"),
                                 take(176.0F), hsv, theme_.header);
    color_.hue = hsv.hue;
    if (held && (hsv.hue != before.hue || hsv.saturation != before.saturation ||
                 hsv.value != before.value)) {
        const UiColor picked = hsv_to_rgb(hsv, color_.value.a);
        if (!same_rgb(picked, color_.value)) {
            next = picked;
            edited = true;
        }
    }
    static_cast<void>(take(12.0F));

    // Before and after, then the hex code.
    const Rect compare_row = take(30.0F);
    {
        const Rect before_swatch{compare_row.x, compare_row.y, 36.0F, compare_row.height};
        const Rect after_swatch{before_swatch.right(), compare_row.y, 36.0F,
                                compare_row.height};
        const Interaction back = grab(color_.id ^ ui_id("color.original"), before_swatch);
        cursor_for(back, Cursor::Hand);
        rounded(before_swatch, clamped(fade(color_.original, 1.0F)), theme_.radius,
                UiDrawList::corner_left);
        rounded(after_swatch, clamped(fade(next, 1.0F)), theme_.radius,
                UiDrawList::corner_right);
        rounded_outline(Rect{before_swatch.x, before_swatch.y, 72.0F, compare_row.height},
                        fade(theme_.text, 0.2F), theme_.radius);
        tooltip(before_swatch, "Before: click to go back to it");
        if (back.clicked && !same_rgb(color_.original, next)) {
            next = color_.original;
            edited = true;
        }
        const Rect hex_field{after_swatch.right() + 10.0F, compare_row.y,
                             compare_row.right() - after_swatch.right() - 10.0F,
                             compare_row.height};
        const UiId hex_id = color_.id ^ ui_id("color.hex");
        if (focus_ != hex_id) {
            color_.hex = hex_text(next);
        }
        if (text_field(hex_id, hex_field, color_.hex)) {
            if (const auto parsed = parse_hex(color_.hex, next.a)) {
                if (!same_rgb(*parsed, next)) {
                    next = *parsed;
                    edited = true;
                }
            }
        }
    }
    static_cast<void>(take(8.0F));

    // R, G and B as whole numbers, each marked with its own primary.
    {
        const Rect channel_row = take(30.0F);
        constexpr float gap = 8.0F;
        const float width = (channel_row.width - gap * 2.0F) / 3.0F;
        constexpr std::array<UiColor, 3U> marks{{
            {1.0F, 0.42F, 0.42F, 1.0F},
            {0.36F, 0.89F, 0.56F, 1.0F},
            {0.35F, 0.66F, 1.0F, 1.0F},
        }};
        for (std::size_t index = 0U; index < 3U; ++index) {
            const Rect field{channel_row.x + (width + gap) * static_cast<float>(index),
                             channel_row.y, width, channel_row.height};
            const UiId channel_id =
                (color_.id ^ ui_id("color.channel")) + index * 16U;
            float* const component =
                index == 0U ? &next.r : (index == 1U ? &next.g : &next.b);
            if (focus_ != channel_id) {
                color_.channels[index] = std::to_string(channel_byte(*component));
            }
            const Rect text{field.x + 14.0F, field.y, field.width - 14.0F, field.height};
            if (text_field(channel_id, text, color_.channels[index])) {
                if (const auto parsed = parse_channel(color_.channels[index])) {
                    if (std::abs(*parsed - *component) > 1.0e-6F) {
                        *component = *parsed;
                        edited = true;
                    }
                }
            }
            add_disc(list_, UiPoint{field.x + 5.0F, field.center_y()}, 3.5F,
                     marks[index], 12U);
        }
    }
    static_cast<void>(take(14.0F));

    // Recent colours, then the LED primaries.
    {
        const Rect swatch_row = take(22.0F);
        constexpr float size = 20.0F;
        const float step = (swatch_row.width - size) / 9.0F;
        std::array<UiColor, 10U> shown{};
        std::size_t count = 0U;
        for (std::size_t index = 0U; index < recent_count_ && count < 3U; ++index) {
            shown[count++] = recent_colors_[index];
        }
        for (const UiColor preset : preset_colors) {
            if (count < shown.size()) shown[count++] = preset;
        }
        for (std::size_t index = 0U; index < count; ++index) {
            const Rect chip{swatch_row.x + step * static_cast<float>(index),
                            swatch_row.y, size, size};
            const Interaction it = grab(
                (color_.id ^ ui_id("color.swatch")) + index * 32U, chip);
            cursor_for(it, Cursor::Hand);
            if (it.hovered) {
                glow(chip, shown[index], size * 0.5F, 6.0F);
            }
            rounded(chip, shown[index], size * 0.5F);
            rounded_outline(chip, fade(theme_.text, it.hovered ? 0.6F : 0.2F),
                            size * 0.5F);
            if (it.clicked && !same_rgb(shown[index], next)) {
                next = UiColor{shown[index].r, shown[index].g, shown[index].b,
                               next.a};
                edited = true;
            }
        }
    }

    color_.dragging = held;
    if (edited) {
        color_.value = next;
        color_.pending = true;
        const Hsv landed = rgb_to_hsv(next);
        if (landed.saturation > 1.0e-6F) {
            color_.hue = landed.hue;
        }
    }
    in_color_popover_ = false;
}

void UiContext::open_context_menu(UiId id, float x, float y) {
    clear_focus();
    dropdown_id_ = id;
    dropdown_anchor_ = Rect{x, y, 0.0F, 0.0F};
    // Nothing is highlighted until the pointer reaches an entry, so an Enter
    // struck by habit cannot pick the first one.
    dropdown_highlight_ = static_cast<std::size_t>(-1);
    dropdown_first_ = 0U;
    dropdown_pressed_ = static_cast<std::size_t>(-1);
    dropdown_options_.clear();
    dropdown_swatches_.clear();
    dropdown_checks_.clear();
    dropdown_seen_ = true;
}

std::optional<std::size_t> UiContext::context_menu(
    UiId id, std::span<const std::string_view> options) {
    return context_menu(id, options, std::span<const UiColor>{});
}

std::optional<std::size_t> UiContext::context_menu(
    UiId id, std::span<const std::string_view> options,
    std::span<const UiColor> swatches) {
    if (dropdown_result_id_ == id) {
        dropdown_result_id_ = none;
        if (dropdown_result_ < options.size()) {
            return dropdown_result_;
        }
        return std::nullopt;
    }
    if (dropdown_id_ == id) {
        if (options.empty()) {
            dropdown_id_ = none;
            return std::nullopt;
        }
        dropdown_seen_ = true;
        dropdown_options_.clear();
        for (const auto option : options) dropdown_options_.emplace_back(option);
        dropdown_swatches_.assign(swatches.begin(), swatches.end());
        dropdown_checks_.clear();
    }
    return std::nullopt;
}

std::optional<std::size_t> UiContext::context_menu(
    UiId id, std::span<const std::string_view> options,
    std::span<const bool> checked) {
    const auto chosen = context_menu(id, options, std::span<const UiColor>{});
    if (dropdown_id_ == id) {
        dropdown_checks_.assign(checked.begin(), checked.end());
    }
    return chosen;
}

void UiContext::draw_overlays(const Rect& window_bounds) {
    if (color_.id != none) {
        draw_color_popover(window_bounds);
        return;
    }
    if (dropdown_id_ == none) {
        if (tooltip_text_ != tooltip_previous_) { tooltip_previous_ = tooltip_text_; tooltip_since_ = time_; }
        if (tooltip_text_.empty() || time_ - tooltip_since_ < 0.6 || pointer_down() || typing()) return;
        const float max_width = std::max(std::min(520.0F, window_bounds.width - 24.0F), 1.0F);
        std::vector<std::string_view> lines;
        std::string_view remaining{tooltip_text_};
        float width = 1.0F;
        while (!remaining.empty()) {
            std::size_t count = 1U;
            while (count < remaining.size() && font_width(remaining.substr(0U, count + 1U)) < max_width) ++count;
            while (count < remaining.size() && is_continuation(remaining[count])) ++count;
            const auto line = remaining.substr(0U, count);
            lines.push_back(line); width = std::max(width, font_width(line));
            remaining.remove_prefix(count);
        }
        const float height = static_cast<float>(lines.size()) * line_height() + 12.0F;
        const float x = std::clamp(pointer_x() + 12.0F, window_bounds.x + 4.0F,
            std::max(window_bounds.x + 4.0F, window_bounds.right() - width - 20.0F));
        const float y = std::max(window_bounds.y + 4.0F,
            std::min(pointer_y() + 20.0F, window_bounds.bottom() - height - 4.0F));
        // A raised card: lighter than any panel and lifted by a shadow, so
        // it reads as floating over whatever it explains.
        const Rect card{x, y, width + 16.0F, height};
        shadow(card, theme_.radius, 12.0F);
        rounded(card, theme_.widget_hover, theme_.radius);
        rounded_outline(card, blend(theme_.widget_hover, theme_.text_muted, 0.35F), theme_.radius);
        for (std::size_t row = 0U; row < lines.size(); ++row) {
            draw_label(list_, font_, {x + 8.0F, y + 6.0F + static_cast<float>(row) * line_height(), width, line_height()}, x + 8.0F, lines[row], theme_.text);
        }
        return;
    }
    if (!dropdown_seen_) { dropdown_id_ = none; active_ = none; return; }
    const float height = std::min(static_cast<float>(std::min(dropdown_options_.size(), std::size_t{10})) * menu_row_height,
                                  std::max(menu_row_height, std::floor((window_bounds.height - 16.0F) / menu_row_height) * menu_row_height));
    float width = dropdown_anchor_.width;
    const float chip =
        dropdown_swatches_.empty() && dropdown_checks_.empty() ? 0.0F : 22.0F;
    for (const auto& option : dropdown_options_) width = std::max(width, font_width(option) + 40.0F + chip);
    width = std::min(width, std::max(window_bounds.width - 16.0F, 1.0F));
    const float x = std::clamp(dropdown_anchor_.x, window_bounds.x + 8.0F, std::max(window_bounds.x + 8.0F, window_bounds.right() - width - 8.0F));
    float y = dropdown_anchor_.bottom() + 6.0F;
    if (y + height > window_bounds.bottom() - 8.0F) y = dropdown_anchor_.y - height - 6.0F;
    y = std::max(y, window_bounds.y + 8.0F);
    dropdown_bounds_ = {x, y, width, height};
    // The rows sit on a floating card a few units larger than them, with
    // a shadow beneath, and the highlighted row is a soft capsule inside it.
    const Rect card = dropdown_bounds_.inflated(4.0F, 4.0F);
    shadow(card, theme_.radius_large, 18.0F);
    rounded(card, theme_.header, theme_.radius_large);
    rounded_outline(card, theme_.border, theme_.radius_large);
    const auto visible = static_cast<std::size_t>(height / menu_row_height);
    dropdown_first_ = std::min(dropdown_first_, dropdown_options_.size() > visible ? dropdown_options_.size() - visible : 0U);
    for (std::size_t row = 0U; row < visible && dropdown_first_ + row < dropdown_options_.size(); ++row) {
        const std::size_t index = dropdown_first_ + row;
        const Rect cell{x, y + static_cast<float>(row) * menu_row_height, width, menu_row_height};
        if (index == dropdown_highlight_) rounded(cell, theme_.selection, theme_.radius);
        if (index < dropdown_swatches_.size()) {
            const Rect swatch{x + 9.0F, cell.y + (menu_row_height - 14.0F) * 0.5F, 14.0F, 14.0F};
            const bool none_chip = dropdown_swatches_[index].a <= 0.0F;
            rounded(swatch, none_chip ? theme_.panel : dropdown_swatches_[index], 7.0F);
            rounded_outline(swatch, fade(theme_.text, none_chip ? 0.45F : 0.25F), 7.0F);
        }
        if (index < dropdown_checks_.size() && dropdown_checks_[index]) {
            // A tick, drawn: the atlas holds ASCII only.
            const float cx = x + 16.0F;
            const float cy = cell.y + menu_row_height * 0.5F;
            const std::array<UiPoint, 3U> tick{
                UiPoint{cx - 4.0F, cy}, UiPoint{cx - 1.0F, cy + 3.0F},
                UiPoint{cx + 4.5F, cy - 3.5F}};
            list_.add_polyline(tick, 1.8F, theme_.accent);
        }
        label({x + 10.0F + chip, cell.y, width - 20.0F - chip, cell.height}, dropdown_options_[index]);
    }
    if (dropdown_options_.size() > visible) {
        const float thumb = height * static_cast<float>(visible) / static_cast<float>(dropdown_options_.size());
        rounded({x + width - 2.0F, y + (height - thumb) * static_cast<float>(dropdown_first_) / static_cast<float>(dropdown_options_.size() - visible), 3.0F, thumb}, theme_.text_muted, 1.5F);
    }
}

void UiContext::take_focus(UiId id, const std::string& value) {
    focus_ = id;
    select_all_ = false;
    edit_original_ = value;
    blink_base_ = time_;
}

bool UiContext::edit_keys(std::string& value, bool& closed_out) {
    bool changed = false;
    closed_out = false;
    caret_ = std::min(caret_, value.size());

    for (const Key key : input_.keys()) {
        if (select_all_) {
            // A selection covering the whole value turns the first edit into
            // a replacement; a navigation key collapses it to a caret.
            if (key == Key::Backspace || key == Key::Delete) {
                if (!value.empty()) {
                    value.clear();
                    changed = true;
                }
                caret_ = 0U;
                select_all_ = false;
                continue;
            }
            if (key == Key::Left || key == Key::Home) {
                caret_ = 0U;
                select_all_ = false;
                continue;
            }
            if (key == Key::Right || key == Key::End) {
                caret_ = value.size();
                select_all_ = false;
                continue;
            }
        }
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
        case Key::Up:
        case Key::Down:
            // Compound numeric fields consume these as semantic nudges. A
            // plain text field deliberately leaves its contents unchanged.
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
        case Key::Undo:
        case Key::Redo:
        case Key::Copy:
        case Key::Paste:
        case Key::Split:
        case Key::Rename:
        case Key::ZoomIn:
        case Key::ZoomOut:
        case Key::ZoomReset:
        case Key::ClearLocation:
        case Key::ClearRotation:
        case Key::ClearScale:
        case Key::EasyEase:
            // Global shortcuts. A text field deliberately leaves document
            // history to its caller; Escape is the field-local way to abandon
            // typing.
            break;
        }
        if (closed_out) {
            break;
        }
    }

    if (!closed_out) {
        const std::string_view typed = input_.text();
        if (!typed.empty()) {
            if (select_all_) {
                value.clear();
                caret_ = 0U;
                select_all_ = false;
            }
            value.insert(caret_, typed);
            caret_ += typed.size();
            changed = true;
        }
    }
    if (closed_out) {
        select_all_ = false;
    }

    // Any keyboard activity holds the caret solid, so it never blinks out from
    // under someone mid-word.
    if (!input_.keys().empty() || !input_.text().empty()) {
        blink_base_ = time_;
    }
    return changed;
}

namespace {

// UTF-8 continuation bytes are not characters of their own.
[[nodiscard]] std::size_t characters_before(std::string_view value,
                                            std::size_t byte) noexcept {
    std::size_t count = 0U;
    for (std::size_t index = 0U; index < std::min(byte, value.size());
         ++index) {
        if ((static_cast<unsigned char>(value[index]) & 0xC0U) != 0x80U) {
            ++count;
        }
    }
    return count;
}

[[nodiscard]] std::size_t byte_of_character(std::string_view value,
                                            std::size_t character) noexcept {
    std::size_t seen = 0U;
    for (std::size_t index = 0U; index < value.size(); ++index) {
        if ((static_cast<unsigned char>(value[index]) & 0xC0U) != 0x80U) {
            if (seen == character) {
                return index;
            }
            ++seen;
        }
    }
    return value.size();
}

} // namespace

bool UiContext::text_field(UiId id, const Rect& bounds, std::string& value) {
    return edit_field(id, bounds, value, false);
}

bool UiContext::password_field(UiId id, const Rect& bounds,
                               std::string& value) {
    return edit_field(id, bounds, value, true);
}

bool UiContext::edit_field(UiId id, const Rect& bounds, std::string& value,
                           bool masked) {
    const Interaction it = grab(id, bounds);
    cursor_for(it, Cursor::Text);

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
        select_all_ = false;
        if (masked) {
            // The click lands on a '*'; the caret goes before the character
            // it stands for.
            const std::string shown(characters_before(value, value.size()),
                                    '*');
            caret_ = byte_of_character(
                value, caret_from_x(font_, shown, inner.x, pointer_press_x()));
        } else {
            caret_ = caret_from_x(font_, value, inner.x, pointer_press_x());
        }
        blink_base_ = time_;
    } else if (focus_ == id && input_.mouse_pressed(MouseButton::Left)
               && !bounds.contains(pointer_press_x(), pointer_press_y())) {
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
    // A soft well; the accent rings it, and a faint glow lifts it, while it
    // has the keyboard.
    const float radius = std::min(theme_.radius, bounds.height * 0.5F);
    if (focused) {
        glow(bounds, theme_.accent, radius, 6.0F);
    }
    rounded(bounds, theme_.track, radius);
    const UiColor frame = focused
        ? theme_.accent
        : (it.hovered ? theme_.widget_hover : theme_.widget);
    rounded_outline(bounds, frame, radius, focused ? 1.5F : 1.0F);

    const std::string masked_text =
        masked ? std::string(characters_before(value, value.size()), '*')
               : std::string{};
    const std::string& shown = masked ? masked_text : value;
    const std::size_t caret =
        masked ? characters_before(value, std::min(caret_, value.size()))
               : std::min(caret_, value.size());
    const float caret_x = focused ? width_to(font_, shown, caret) : 0.0F;
    // Keep the caret in view by sliding the text left once it runs past the
    // right edge; an unfocused field always shows its start.
    const float scroll =
        focused ? std::max(caret_x - inner.width, 0.0F) : 0.0F;

    push_clip(inner);
    if (focused && select_all_ && !shown.empty()) {
        const float height = font_.line_height() + 2.0F;
        list_.add_rect(inner.x - scroll - 1.0F,
                       bounds.y + (bounds.height - height) * 0.5F,
                       width_to(font_, shown, shown.size()) + 2.0F, height,
                       theme_.selection);
    }
    if (focused || masked)
        draw_label(list_, font_, bounds, inner.x - scroll, shown, theme_.text);
    else
        label({inner.x, bounds.y, inner.width, bounds.height}, shown, theme_.text);
    if (focused && !select_all_) {
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

void UiContext::focus_text_field(UiId id, const std::string& value) {
    take_focus(id, value);
    caret_ = value.size();
}

void UiContext::focus_text_field_selected(UiId id, const std::string& value) {
    take_focus(id, value);
    caret_ = value.size();
    select_all_ = true;
}

void UiContext::clear_focus() noexcept {
    focus_ = none;
    select_all_ = false;
}

bool UiContext::key_pressed(Key key) const noexcept {
    for (const Key struck : input_.keys()) {
        if (struck == key) {
            return true;
        }
    }
    return false;
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

    // The seam draws at its own thickness but answers a press a few units
    // either side of it, so a thin rule is still easy to take hold of.
    constexpr float reach = 4.0F;
    const Interaction it = grab(
        id, horizontal ? result.divider.inflated(reach, 0.0F)
                       : result.divider.inflated(0.0F, reach));
    cursor_for(it, horizontal ? Cursor::ResizeHorizontal
                              : Cursor::ResizeVertical);
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
