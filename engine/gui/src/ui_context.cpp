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
    pointer_block_depth_ = 0U;
    interaction_clip_stack_.clear();
    list_.reset();
    tooltip_text_.clear();
    dropdown_dismissed_ = false;
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
            const std::size_t visible = std::max(std::size_t{1}, static_cast<std::size_t>(dropdown_bounds_.height / 24.0F));
            const std::size_t maximum = dropdown_options_.size() > visible ? dropdown_options_.size() - visible : 0U;
            if (inside && input_.wheel() != 0.0F) {
                const auto first = static_cast<int>(dropdown_first_) - static_cast<int>(input_.wheel() * 3.0F);
                dropdown_first_ = static_cast<std::size_t>(std::clamp(first, 0, static_cast<int>(maximum)));
            }
            if (inside && (input_.mouse_dx() != 0.0F || input_.mouse_dy() != 0.0F || input_.mouse_pressed(MouseButton::Left))) {
                dropdown_highlight_ = std::min(dropdown_first_ + static_cast<std::size_t>((input_.mouse_y() - dropdown_bounds_.y) / 24.0F), dropdown_options_.size() - 1U);
            }
            if (input_.mouse_pressed(MouseButton::Left)) {
                if (inside) dropdown_pressed_ = dropdown_highlight_;
                else close(false);
            }
            // A second right click dismisses rather than stacking menus; the
            // operator right-clicks again for the menu of what is under it.
            if (dropdown_id_ != none && input_.mouse_pressed(MouseButton::Right)) {
                close(false);
            }
            if (dropdown_id_ != none && input_.mouse_released(MouseButton::Left)) {
                if (inside && dropdown_pressed_ == dropdown_highlight_) close(true);
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
    if (hot_ == id && active_ == none
        && input_.mouse_pressed(MouseButton::Left)) {
        active_ = id;
        pressed = true;
    }
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

void draw_eye_glyph(
    UiDrawList& list, float center_x, float center_y, bool open, UiColor color) {
    constexpr float half = 6.0F;
    constexpr float lid = 3.5F;
    constexpr std::size_t steps = 6U;
    constexpr float pi = 3.14159265F;
    const auto bow = [](float t) noexcept {
        return lid * std::sin(t * pi);
    };
    if (!open) {
        // A closed lid is the lower arc alone, with two lashes. An eye that is
        // not looking is not an eye with a line through it.
        for (std::size_t index = 0U; index < steps; ++index) {
            const float t0 = static_cast<float>(index) / static_cast<float>(steps);
            const float t1 =
                static_cast<float>(index + 1U) / static_cast<float>(steps);
            list.add_line(
                center_x - half + t0 * half * 2.0F,
                center_y + bow(t0),
                center_x - half + t1 * half * 2.0F,
                center_y + bow(t1),
                1.0F,
                color);
        }
        list.add_line(
            center_x - half, center_y + 1.0F, center_x - half - 2.0F,
            center_y + 3.0F, 1.0F, color);
        list.add_line(
            center_x + half, center_y + 1.0F, center_x + half + 2.0F,
            center_y + 3.0F, 1.0F, color);
        return;
    }
    for (std::size_t index = 0U; index < steps; ++index) {
        const float t0 = static_cast<float>(index) / static_cast<float>(steps);
        const float t1 = static_cast<float>(index + 1U) / static_cast<float>(steps);
        const float x0 = center_x - half + t0 * half * 2.0F;
        const float x1 = center_x - half + t1 * half * 2.0F;
        list.add_line(x0, center_y - bow(t0), x1, center_y - bow(t1), 1.0F, color);
        list.add_line(x0, center_y + bow(t0), x1, center_y + bow(t1), 1.0F, color);
    }
    list.add_rect(center_x - 1.5F, center_y - 1.5F, 3.0F, 3.0F, color);
}

void draw_padlock_glyph(
    UiDrawList& list,
    float center_x,
    float center_y,
    bool closed,
    UiColor color) {
    const float body_top = center_y - 1.0F;
    // An open lock lifts its shackle clear of the body and shifts it aside, so
    // the two states differ in silhouette and not only in fill.
    const float shackle_x = closed ? center_x : center_x + 2.0F;
    list.add_rect(shackle_x - 3.5F, body_top - 5.0F, 1.0F, 5.0F, color);
    list.add_rect(
        shackle_x + 2.5F, body_top - 5.0F, 1.0F, closed ? 5.0F : 3.0F, color);
    list.add_rect(shackle_x - 3.5F, body_top - 6.0F, 7.0F, 1.0F, color);
    if (closed) {
        list.add_rect(center_x - 5.0F, body_top, 10.0F, 8.0F, color);
        return;
    }
    list.add_rect(center_x - 5.0F, body_top, 10.0F, 1.0F, color);
    list.add_rect(center_x - 5.0F, body_top + 7.0F, 10.0F, 1.0F, color);
    list.add_rect(center_x - 5.0F, body_top, 1.0F, 8.0F, color);
    list.add_rect(center_x + 4.0F, body_top, 1.0F, 8.0F, color);
}

void draw_eyedropper_glyph(
    UiDrawList& list, float center_x, float center_y, UiColor color) {
    // Two parallel strokes make the glass tube; the short diagonal at the
    // upper-right is its squeeze bulb and the converging lower-left strokes
    // form the sampling tip. This stays legible at the Inspector's 18px row.
    list.add_line(center_x + 2.0F, center_y - 5.0F,
                  center_x - 4.0F, center_y + 1.0F, 2.0F, color);
    list.add_line(center_x + 5.0F, center_y - 2.0F,
                  center_x - 1.0F, center_y + 4.0F, 2.0F, color);
    list.add_line(center_x + 1.0F, center_y - 6.0F,
                  center_x + 6.0F, center_y - 1.0F, 2.0F, color);
    list.add_line(center_x - 4.0F, center_y + 1.0F,
                  center_x - 6.0F, center_y + 6.0F, 1.5F, color);
    list.add_line(center_x - 1.0F, center_y + 4.0F,
                  center_x - 6.0F, center_y + 6.0F, 1.5F, color);
    list.add_rect(center_x - 7.0F, center_y + 6.0F, 2.0F, 2.0F, color);
}

void draw_icon(
    UiDrawList& list,
    Icon icon,
    float center_x,
    float center_y,
    UiColor color) {
    const float x = center_x;
    const float y = center_y;
    const auto right = [&](float tip, float half) {
        list.add_triangle(tip - half * 1.6F, y - half, tip, y,
                          tip - half * 1.6F, y + half, color);
    };
    const auto left = [&](float tip, float half) {
        list.add_triangle(tip + half * 1.6F, y - half, tip + half * 1.6F,
                          y + half, tip, y, color);
    };
    switch (icon) {
    case Icon::Play:
        right(x + 5.0F, 5.5F);
        return;
    case Icon::Pause:
        list.add_rect(x - 4.0F, y - 5.0F, 3.0F, 10.0F, color);
        list.add_rect(x + 1.0F, y - 5.0F, 3.0F, 10.0F, color);
        return;
    case Icon::StepBack:
        left(x - 4.0F, 4.5F);
        list.add_rect(x + 3.0F, y - 4.5F, 2.0F, 9.0F, color);
        return;
    case Icon::StepForward:
        list.add_rect(x - 5.0F, y - 4.5F, 2.0F, 9.0F, color);
        right(x + 4.0F, 4.5F);
        return;
    case Icon::ToStart:
        list.add_rect(x - 6.0F, y - 4.5F, 2.0F, 9.0F, color);
        left(x - 4.0F, 4.5F);
        left(x + 1.0F, 4.5F);
        return;
    case Icon::ToEnd:
        right(x - 1.0F, 4.5F);
        right(x + 4.0F, 4.5F);
        list.add_rect(x + 4.0F, y - 4.5F, 2.0F, 9.0F, color);
        return;
    case Icon::Loop: {
        // Three quarters of a ring with its arrowhead: the round trip.
        constexpr std::size_t steps = 9U;
        constexpr float radius = 5.0F;
        constexpr float start = 0.6F;
        constexpr float sweep = 4.8F;
        for (std::size_t index = 0U; index < steps; ++index) {
            const float a0 = start + sweep * static_cast<float>(index)
                / static_cast<float>(steps);
            const float a1 = start + sweep * static_cast<float>(index + 1U)
                / static_cast<float>(steps);
            list.add_line(x + std::cos(a0) * radius, y + std::sin(a0) * radius,
                          x + std::cos(a1) * radius, y + std::sin(a1) * radius,
                          1.5F, color);
        }
        const float end = start + sweep;
        const float ex = x + std::cos(end) * radius;
        const float ey = y + std::sin(end) * radius;
        list.add_triangle(ex - 3.0F, ey - 3.0F, ex + 3.0F, ey - 1.0F,
                          ex - 1.0F, ey + 3.0F, color);
        return;
    }
    case Icon::Snap:
        // A horseshoe magnet, legs up.
        list.add_rect(x - 5.0F, y - 5.0F, 3.0F, 6.0F, color);
        list.add_rect(x + 2.0F, y - 5.0F, 3.0F, 6.0F, color);
        list.add_line(x - 3.5F, y + 1.0F, x - 2.0F, y + 4.0F, 3.0F, color);
        list.add_line(x - 2.0F, y + 4.0F, x + 2.0F, y + 4.0F, 3.0F, color);
        list.add_line(x + 2.0F, y + 4.0F, x + 3.5F, y + 1.0F, 3.0F, color);
        return;
    case Icon::Marker:
        list.add_rect(x - 4.0F, y - 6.0F, 1.5F, 12.0F, color);
        list.add_triangle(x - 3.0F, y - 6.0F, x + 5.0F, y - 3.0F, x - 3.0F,
                          y, color);
        return;
    case Icon::Hold: {
        std::array<UiPoint, 8U> points{};
        for (std::size_t index = 0U; index < points.size(); ++index) {
            const float angle = 0.3927F + 0.7854F * static_cast<float>(index);
            points[index] =
                UiPoint{x + std::cos(angle) * 6.0F, y + std::sin(angle) * 6.0F};
        }
        list.add_convex_poly(points, color);
        return;
    }
    case Icon::Select:
        list.add_triangle(x - 4.0F, y - 6.0F, x - 4.0F, y + 4.0F, x + 4.0F,
                          y + 1.0F, color);
        list.add_line(x - 1.0F, y + 1.0F, x + 2.0F, y + 6.0F, 2.0F, color);
        return;
    case Icon::Ripple:
        // An edit point that pushes everything after it along.
        list.add_rect(x - 4.0F, y - 6.0F, 1.5F, 12.0F, color);
        list.add_rect(x - 7.0F, y - 6.0F, 3.0F, 1.5F, color);
        list.add_rect(x - 7.0F, y + 4.5F, 3.0F, 1.5F, color);
        list.add_rect(x - 1.0F, y - 0.75F, 5.0F, 1.5F, color);
        right(x + 7.0F, 3.0F);
        return;
    case Icon::Roll:
        // The shared edit point between two clips, moved either way.
        list.add_rect(x - 2.0F, y - 6.0F, 1.5F, 12.0F, color);
        list.add_rect(x + 0.5F, y - 6.0F, 1.5F, 12.0F, color);
        left(x - 7.0F, 3.0F);
        right(x + 7.0F, 3.0F);
        return;
    case Icon::Slip:
        // The source moves inside a clip that stays put.
        list.add_rect(x - 7.0F, y - 5.0F, 14.0F, 1.0F, color);
        list.add_rect(x - 7.0F, y + 4.0F, 14.0F, 1.0F, color);
        list.add_rect(x - 7.0F, y - 5.0F, 1.0F, 10.0F, color);
        list.add_rect(x + 6.0F, y - 5.0F, 1.0F, 10.0F, color);
        list.add_rect(x - 2.0F, y - 0.5F, 4.0F, 1.0F, color);
        left(x - 5.0F, 2.5F);
        right(x + 5.0F, 2.5F);
        return;
    case Icon::Slide:
        // The clip moves between neighbours that stay put.
        list.add_rect(x - 3.0F, y - 4.0F, 6.0F, 8.0F, color);
        left(x - 7.5F, 3.0F);
        right(x + 7.5F, 3.0F);
        return;
    case Icon::Razor:
        // A blade crossing a clip.
        list.add_line(x - 5.0F, y + 6.0F, x + 4.0F, y - 6.0F, 2.0F, color);
        list.add_line(x - 1.0F, y + 6.0F, x + 7.0F, y - 4.0F, 1.0F, color);
        list.add_line(x - 7.0F, y - 1.0F, x + 7.0F, y - 1.0F, 1.0F, color);
        return;
    case Icon::Fit:
        list.add_rect(x - 7.0F, y - 5.0F, 1.5F, 10.0F, color);
        list.add_rect(x + 5.5F, y - 5.0F, 1.5F, 10.0F, color);
        list.add_rect(x - 3.0F, y - 0.75F, 6.0F, 1.5F, color);
        left(x - 5.0F, 2.5F);
        right(x + 5.0F, 2.5F);
        return;
    case Icon::Plus:
        list.add_rect(x - 5.0F, y - 1.0F, 10.0F, 2.0F, color);
        list.add_rect(x - 1.0F, y - 5.0F, 2.0F, 10.0F, color);
        return;
    }
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
    list_.push_clip(bounds.x, bounds.y, std::max(bounds.width, 0.0F), std::max(bounds.height, 0.0F));
    draw_label(list_, font_, bounds, bounds.x, text, color);
    list_.pop_clip();
    if (font_width(text) > bounds.width) tooltip(bounds, text);
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

    const float box = std::min(bounds.height - 4.0F, 14.0F);
    const Rect box_rect{
        bounds.x, bounds.y + (bounds.height - box) * 0.5F, box, box};
    const UiColor body = (it.held && !it.released)
        ? theme_.widget_active
        : (it.hovered ? theme_.widget_hover : theme_.widget);
    list_.add_rect(box_rect.x, box_rect.y, box, box, theme_.text_muted);
    list_.add_rect(box_rect.x + 1.0F, box_rect.y + 1.0F, box - 2.0F, box - 2.0F, value ? theme_.accent : body);
    if (value) {
        list_.add_line(box_rect.x + box * 0.22F, box_rect.y + box * 0.51F,
                       box_rect.x + box * 0.43F, box_rect.y + box * 0.72F, 1.7F, theme_.text);
        list_.add_line(box_rect.x + box * 0.43F, box_rect.y + box * 0.72F,
                       box_rect.x + box * 0.80F, box_rect.y + box * 0.28F, 1.7F, theme_.text);
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
        for (const auto option : options) dropdown_options_.emplace_back(option);
    }
    panel(bounds, it.hovered ? theme_.widget_hover : theme_.track);
    list_.add_rect(bounds.x, bounds.y + bounds.height - 1.0F, bounds.width, 1.0F,
                   dropdown_id_ == id ? theme_.accent : theme_.widget);
    label({bounds.x + 6.0F, bounds.y, std::max(bounds.width - 25.0F, 0.0F), bounds.height},
          selected < options.size() ? options[selected] : std::string_view{"Mixed"});
    const float x = bounds.x + bounds.width - 10.0F;
    const float y = bounds.y + bounds.height * 0.5F;
    list_.add_line(x - 3.0F, y - 1.5F, x, y + 1.5F, 1.0F, theme_.text_muted);
    list_.add_line(x, y + 1.5F, x + 3.0F, y - 1.5F, 1.0F, theme_.text_muted);
    return changed;
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
    dropdown_seen_ = true;
}

std::optional<std::size_t> UiContext::context_menu(
    UiId id, std::span<const std::string_view> options) {
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
    }
    return std::nullopt;
}

void UiContext::draw_overlays(const Rect& window_bounds) {
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
            std::max(window_bounds.x + 4.0F, window_bounds.x + window_bounds.width - width - 20.0F));
        const float y = std::max(window_bounds.y + 4.0F,
            std::min(pointer_y() + 20.0F, window_bounds.y + window_bounds.height - height - 4.0F));
        panel({x - 1.0F, y - 1.0F, width + 14.0F, height + 2.0F}, theme_.widget_hover);
        panel({x, y, width + 12.0F, height}, theme_.panel);
        for (std::size_t row = 0U; row < lines.size(); ++row) {
            draw_label(list_, font_, {x + 6.0F, y + 6.0F + static_cast<float>(row) * line_height(), width, line_height()}, x + 6.0F, lines[row], theme_.text);
        }
        return;
    }
    if (!dropdown_seen_) { dropdown_id_ = none; active_ = none; return; }
    const float height = std::min(static_cast<float>(std::min(dropdown_options_.size(), std::size_t{10})) * 24.0F,
                                  std::max(24.0F, std::floor((window_bounds.height - 8.0F) / 24.0F) * 24.0F));
    float width = dropdown_anchor_.width;
    for (const auto& option : dropdown_options_) width = std::max(width, font_width(option) + 36.0F);
    width = std::min(width, std::max(window_bounds.width - 8.0F, 1.0F));
    const float x = std::clamp(dropdown_anchor_.x, window_bounds.x + 4.0F, std::max(window_bounds.x + 4.0F, window_bounds.x + window_bounds.width - width - 4.0F));
    float y = dropdown_anchor_.y + dropdown_anchor_.height + 2.0F;
    if (y + height > window_bounds.y + window_bounds.height - 4.0F) y = dropdown_anchor_.y - height - 2.0F;
    y = std::max(y, window_bounds.y + 4.0F);
    dropdown_bounds_ = {x, y, width, height};
    panel({x - 1.0F, y - 1.0F, width + 2.0F, height + 2.0F}, theme_.widget_hover);
    panel(dropdown_bounds_, theme_.panel);
    const auto visible = static_cast<std::size_t>(height / 24.0F);
    dropdown_first_ = std::min(dropdown_first_, dropdown_options_.size() > visible ? dropdown_options_.size() - visible : 0U);
    for (std::size_t row = 0U; row < visible && dropdown_first_ + row < dropdown_options_.size(); ++row) {
        const std::size_t index = dropdown_first_ + row;
        const Rect cell{x, y + static_cast<float>(row) * 24.0F, width, 24.0F};
        if (index == dropdown_highlight_) panel(cell, theme_.selection);
        label({x + 8.0F, cell.y, width - 16.0F, cell.height}, dropdown_options_[index]);
    }
    if (dropdown_options_.size() > visible) {
        const float thumb = height * static_cast<float>(visible) / static_cast<float>(dropdown_options_.size());
        panel({x + width - 3.0F, y + (height - thumb) * static_cast<float>(dropdown_first_) / static_cast<float>(dropdown_options_.size() - visible), 2.0F, thumb}, theme_.text_muted);
    }
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

void UiContext::focus_text_field(UiId id, const std::string& value) {
    take_focus(id, value);
    caret_ = value.size();
}

void UiContext::clear_focus() noexcept {
    focus_ = none;
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
