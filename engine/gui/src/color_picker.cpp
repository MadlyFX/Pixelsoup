#include <mediaserver/gui/color_picker.hpp>

#include <mediaserver/gui/shapes.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace mediaserver::gui {
namespace {

[[nodiscard]] bool different(UiColor left, UiColor right) noexcept {
    return std::abs(left.r - right.r) > 1.0e-6F ||
           std::abs(left.g - right.g) > 1.0e-6F ||
           std::abs(left.b - right.b) > 1.0e-6F;
}

// A marker that reads on any colour under it: a white ring inside a dark one.
void draw_marker(UiDrawList& list, UiPoint centre, float radius) {
    add_ring(list, centre, radius + 1.5F, 1.5F, UiColor{0.0F, 0.0F, 0.0F, 0.55F});
    add_ring(list, centre, radius, 2.0F, UiColor{1.0F, 1.0F, 1.0F, 1.0F});
}

} // namespace

Hsv rgb_to_hsv(UiColor color) noexcept {
    const float red = std::clamp(color.r, 0.0F, 1.0F);
    const float green = std::clamp(color.g, 0.0F, 1.0F);
    const float blue = std::clamp(color.b, 0.0F, 1.0F);
    const float maximum = std::max({red, green, blue});
    const float minimum = std::min({red, green, blue});
    const float chroma = maximum - minimum;

    Hsv result{};
    result.value = maximum;
    result.saturation = maximum > 0.0F ? chroma / maximum : 0.0F;
    if (chroma <= 1.0e-7F) {
        return result;
    }
    if (maximum == red) {
        result.hue = (green - blue) / chroma / 6.0F;
    } else if (maximum == green) {
        result.hue = ((blue - red) / chroma + 2.0F) / 6.0F;
    } else {
        result.hue = ((red - green) / chroma + 4.0F) / 6.0F;
    }
    result.hue -= std::floor(result.hue);
    return result;
}

UiColor hsv_to_rgb(Hsv value, float alpha) noexcept {
    const float hue = value.hue - std::floor(value.hue);
    const float saturation = std::clamp(value.saturation, 0.0F, 1.0F);
    const float brightness = std::clamp(value.value, 0.0F, 1.0F);
    const float scaled = hue * 6.0F;
    const int sector = static_cast<int>(std::floor(scaled)) % 6;
    const float fraction = scaled - std::floor(scaled);
    const float low = brightness * (1.0F - saturation);
    const float falling = brightness * (1.0F - saturation * fraction);
    const float rising =
        brightness * (1.0F - saturation * (1.0F - fraction));
    switch (sector) {
    case 0:
        return UiColor{brightness, rising, low, alpha};
    case 1:
        return UiColor{falling, brightness, low, alpha};
    case 2:
        return UiColor{low, brightness, rising, alpha};
    case 3:
        return UiColor{low, falling, brightness, alpha};
    case 4:
        return UiColor{rising, low, brightness, alpha};
    default:
        return UiColor{brightness, low, falling, alpha};
    }
}

bool hsv_fields(UiContext& ctx, UiId id, const Rect& bounds, Hsv& hsv,
                UiColor ground) {
    if (bounds.width <= 1.0F || bounds.height <= 22.0F) {
        return false;
    }
    constexpr float hue_height = 14.0F;
    constexpr float gap = 6.0F;
    const Rect field{bounds.x, bounds.y, bounds.width,
                     std::max(bounds.height - hue_height - gap, 1.0F)};
    const Rect rail{bounds.x, field.bottom() + gap, bounds.width, hue_height};

    const Interaction field_it =
        ctx.grab(id ^ ui_id("color-picker.saturation-value"), field);
    const Interaction rail_it = ctx.grab(id ^ ui_id("color-picker.hue"), rail);
    ctx.cursor_for(field_it, Cursor::Hand);
    ctx.cursor_for(rail_it, Cursor::Hand);
    if (rail_it.held) {
        hsv.hue = std::clamp(
            (ctx.pointer_x() - rail.x) / std::max(rail.width, 1.0F), 0.0F, 1.0F);
    }
    if (field_it.held) {
        hsv.saturation = std::clamp(
            (ctx.pointer_x() - field.x) / std::max(field.width, 1.0F), 0.0F, 1.0F);
        hsv.value = 1.0F - std::clamp(
            (ctx.pointer_y() - field.y) / std::max(field.height, 1.0F), 0.0F, 1.0F);
    }

    // The field: white to the pure hue across, then a fade to black down -
    // two smooth gradients rather than a grid of cells.
    UiDrawList& list = ctx.draw_list();
    const UiColor white{1.0F, 1.0F, 1.0F, 1.0F};
    const UiColor pure = hsv_to_rgb(Hsv{hsv.hue, 1.0F, 1.0F}, 1.0F);
    list.add_gradient_rect(field.x, field.y, field.width, field.height, white,
                           pure, pure, white);
    const UiColor clear{0.0F, 0.0F, 0.0F, 0.0F};
    const UiColor black{0.0F, 0.0F, 0.0F, 1.0F};
    list.add_gradient_rect(field.x, field.y, field.width, field.height, clear,
                           clear, black, black);
    const float radius = ctx.theme().radius;
    list.add_corner_mask(field.x, field.y, field.width, field.height, radius,
                         ground);

    // The rail: the hue circle unrolled, in six even blends.
    for (std::size_t step = 0U; step < 6U; ++step) {
        const float from = static_cast<float>(step) / 6.0F;
        const float to = static_cast<float>(step + 1U) / 6.0F;
        const UiColor left = hsv_to_rgb(Hsv{from, 1.0F, 1.0F}, 1.0F);
        const UiColor right = hsv_to_rgb(Hsv{to, 1.0F, 1.0F}, 1.0F);
        // Each blend overlaps the next by half a unit so no seam shows; the
        // last stops exactly at the rail's end.
        const float x = rail.x + rail.width * from;
        const float end = step == 5U ? rail.right()
                                     : rail.x + rail.width * to + 0.5F;
        list.add_gradient_rect(x, rail.y, end - x, rail.height, left, right,
                               right, left);
    }
    list.add_corner_mask(rail.x, rail.y, rail.width, rail.height,
                         rail.height * 0.5F, ground);

    // Markers sit where the value returned this frame is.
    draw_marker(list,
                UiPoint{field.x + hsv.saturation * field.width,
                        field.y + (1.0F - hsv.value) * field.height},
                field_it.held ? 7.0F : 6.0F);
    const float knob_x = rail.x + std::clamp(hsv.hue, 0.0F, 1.0F) * rail.width;
    add_disc(list, UiPoint{knob_x, rail.center_y()}, rail.height * 0.5F + 1.0F,
             pure, 16U);
    draw_marker(list, UiPoint{knob_x, rail.center_y()},
                rail.height * 0.5F + (rail_it.held ? 1.0F : 0.0F));
    return field_it.held || rail_it.held;
}

ColorPickerFrame ColorPicker::draw(UiContext& ctx, UiId id,
                                   const Rect& bounds, UiColor color) {
    ColorPickerFrame frame{color};
    if (bounds.width <= 1.0F || bounds.height <= 22.0F) {
        return frame;
    }
    Hsv hsv = rgb_to_hsv(color);
    const auto remembered = remembered_hues_.try_emplace(id, hsv.hue).first;
    if (hsv.saturation > 1.0e-6F) {
        remembered->second = hsv.hue;
    } else {
        hsv.hue = remembered->second;
    }
    frame.editing = hsv_fields(ctx, id, bounds, hsv, ctx.theme().panel);
    remembered->second = hsv.hue;
    if (frame.editing) {
        frame.color = hsv_to_rgb(hsv, color.a);
        frame.changed = different(frame.color, color);
    }
    return frame;
}

} // namespace mediaserver::gui
