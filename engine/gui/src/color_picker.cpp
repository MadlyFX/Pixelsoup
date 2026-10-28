#include <mediaserver/gui/color_picker.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace mediaserver::gui {
namespace {

struct Hsv final {
    float hue{};
    float saturation{};
    float value{};
};

[[nodiscard]] Hsv rgb_to_hsv(UiColor color) noexcept {
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

[[nodiscard]] UiColor hsv_to_rgb(Hsv value, float alpha) noexcept {
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

[[nodiscard]] bool different(UiColor left, UiColor right) noexcept {
    return std::abs(left.r - right.r) > 1.0e-6F ||
           std::abs(left.g - right.g) > 1.0e-6F ||
           std::abs(left.b - right.b) > 1.0e-6F;
}

} // namespace

ColorPickerFrame ColorPicker::draw(UiContext& ctx, UiId id,
                                   const Rect& bounds, UiColor color) {
    ColorPickerFrame frame{color};
    if (bounds.width <= 1.0F || bounds.height <= 22.0F) {
        return frame;
    }

    constexpr float hue_height = 14.0F;
    constexpr float gap = 6.0F;
    const Rect saturation_value{
        bounds.x, bounds.y, bounds.width,
        std::max(bounds.height - hue_height - gap, 1.0F)};
    const Rect hue{bounds.x, saturation_value.bottom() + gap,
                   bounds.width, hue_height};

    Hsv hsv = rgb_to_hsv(color);
    const auto remembered = remembered_hues_.try_emplace(id, hsv.hue).first;
    if (hsv.saturation > 1.0e-6F) {
        remembered->second = hsv.hue;
    } else {
        hsv.hue = remembered->second;
    }
    constexpr std::size_t saturation_steps = 20U;
    constexpr std::size_t value_steps = 12U;
    const float cell_width =
        saturation_value.width / static_cast<float>(saturation_steps);
    const float cell_height =
        saturation_value.height / static_cast<float>(value_steps);
    for (std::size_t row = 0U; row < value_steps; ++row) {
        const float value = 1.0F -
            (static_cast<float>(row) + 0.5F) /
                static_cast<float>(value_steps);
        for (std::size_t column = 0U; column < saturation_steps; ++column) {
            const float saturation =
                (static_cast<float>(column) + 0.5F) /
                static_cast<float>(saturation_steps);
            ctx.panel(Rect{saturation_value.x +
                               static_cast<float>(column) * cell_width,
                           saturation_value.y +
                               static_cast<float>(row) * cell_height,
                           cell_width + 0.5F, cell_height + 0.5F},
                      hsv_to_rgb(Hsv{hsv.hue, saturation, value}, 1.0F));
        }
    }

    constexpr std::size_t hue_steps = 48U;
    const float hue_width = hue.width / static_cast<float>(hue_steps);
    for (std::size_t step = 0U; step < hue_steps; ++step) {
        const float value = (static_cast<float>(step) + 0.5F) /
                            static_cast<float>(hue_steps);
        ctx.panel(Rect{hue.x + static_cast<float>(step) * hue_width, hue.y,
                       hue_width + 0.5F, hue.height},
                  hsv_to_rgb(Hsv{value, 1.0F, 1.0F}, 1.0F));
    }

    const Interaction saturation_value_interaction =
        ctx.grab(id ^ ui_id("color-picker.saturation-value"),
                 saturation_value);
    const Interaction hue_interaction =
        ctx.grab(id ^ ui_id("color-picker.hue"), hue);
    if (hue_interaction.held) {
        hsv.hue = std::clamp((ctx.pointer_x() - hue.x) /
                                 std::max(hue.width, 1.0F),
                             0.0F, 1.0F);
        remembered->second = hsv.hue;
    }
    if (saturation_value_interaction.held) {
        hsv.saturation = std::clamp(
            (ctx.pointer_x() - saturation_value.x) /
                std::max(saturation_value.width, 1.0F),
            0.0F, 1.0F);
        hsv.value = 1.0F - std::clamp(
            (ctx.pointer_y() - saturation_value.y) /
                std::max(saturation_value.height, 1.0F),
            0.0F, 1.0F);
    }

    frame.editing = saturation_value_interaction.held || hue_interaction.held;
    if (frame.editing) {
        frame.color = hsv_to_rgb(hsv, color.a);
        frame.changed = different(frame.color, color);
    }

    // The markers are deliberately high contrast against both black and the
    // fully saturated corners. Their positions always reflect the value the
    // widget returned on this frame, including direct manipulation.
    const float marker_x =
        saturation_value.x + hsv.saturation * saturation_value.width;
    const float marker_y =
        saturation_value.y + (1.0F - hsv.value) * saturation_value.height;
    constexpr float marker_radius = 5.0F;
    ctx.draw_list().add_line(marker_x - marker_radius, marker_y,
                             marker_x + marker_radius, marker_y, 2.0F,
                             ctx.theme().track);
    ctx.draw_list().add_line(marker_x, marker_y - marker_radius, marker_x,
                             marker_y + marker_radius, 2.0F,
                             ctx.theme().track);
    ctx.draw_list().add_line(marker_x - marker_radius + 1.0F, marker_y,
                             marker_x + marker_radius - 1.0F, marker_y, 1.0F,
                             ctx.theme().text);
    ctx.draw_list().add_line(marker_x, marker_y - marker_radius + 1.0F,
                             marker_x, marker_y + marker_radius - 1.0F, 1.0F,
                             ctx.theme().text);

    const float hue_x = hue.x + hsv.hue * hue.width;
    ctx.draw_list().add_line(hue_x, hue.y - 2.0F, hue_x,
                             hue.bottom() + 2.0F, 3.0F,
                             ctx.theme().track);
    ctx.draw_list().add_line(hue_x, hue.y - 1.0F, hue_x,
                             hue.bottom() + 1.0F, 1.0F,
                             ctx.theme().text);
    return frame;
}

} // namespace mediaserver::gui
