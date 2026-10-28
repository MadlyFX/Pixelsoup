#include <mediaserver/gui/shapes.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace mediaserver::gui {

UiPoint on_circle(UiPoint centre, float radius, float angle) noexcept {
    return UiPoint{centre.x + std::cos(angle) * radius,
                   centre.y + std::sin(angle) * radius};
}

void add_disc(UiDrawList& list, UiPoint centre, float radius, UiColor color,
              std::size_t sides) {
    constexpr float tau = 6.28318530718F;
    constexpr std::size_t most_sides = 64U;
    std::array<UiPoint, most_sides> points{};
    const std::size_t count = std::min(sides, most_sides);
    for (std::size_t index = 0U; index < count; ++index) {
        points[index] = on_circle(
            centre, radius,
            tau * static_cast<float>(index) / static_cast<float>(count));
    }
    list.add_convex_poly(std::span<const UiPoint>{points.data(), count}, color);
}

void add_arc(
    UiDrawList& list,
    UiPoint centre,
    float radius,
    float start,
    float sweep,
    float thickness,
    UiColor color,
    std::size_t segments) {
    if (segments == 0U) {
        return;
    }
    // Small enough for every glyph and handle to stay off the heap.
    constexpr std::size_t inline_points = 65U;
    std::array<UiPoint, inline_points> fixed{};
    std::vector<UiPoint> grown;
    const std::size_t count = segments + 1U;
    UiPoint* points = fixed.data();
    if (count > inline_points) {
        grown.resize(count);
        points = grown.data();
    }
    for (std::size_t index = 0U; index < count; ++index) {
        points[index] = on_circle(
            centre, radius,
            start + sweep * static_cast<float>(index) / static_cast<float>(segments));
    }
    list.add_polyline(std::span<const UiPoint>{points, count}, thickness, color);
}

void add_ring(
    UiDrawList& list,
    UiPoint centre,
    float radius,
    float thickness,
    UiColor color,
    std::size_t segments) {
    add_arc(list, centre, radius, 0.0F, 6.28318530718F, thickness, color, segments);
}

void add_arrowhead(
    UiDrawList& list,
    UiPoint tip,
    UiPoint direction,
    float length,
    float half_width,
    UiColor color) {
    const float magnitude = std::hypot(direction.x, direction.y);
    if (magnitude <= 0.0F) {
        return;
    }
    // Along the arrow, and across it.
    const float ax = direction.x / magnitude;
    const float ay = direction.y / magnitude;
    const float base_x = tip.x - ax * length;
    const float base_y = tip.y - ay * length;
    list.add_triangle(tip.x, tip.y, base_x - ay * half_width,
                      base_y + ax * half_width, base_x + ay * half_width,
                      base_y - ax * half_width, color);
}

void add_diamond(UiDrawList& list, UiPoint centre, float radius, UiColor color) {
    const std::array<UiPoint, 4U> points{{
        {centre.x, centre.y - radius},
        {centre.x + radius, centre.y},
        {centre.x, centre.y + radius},
        {centre.x - radius, centre.y},
    }};
    list.add_convex_poly(points, color);
}

void add_outline(UiDrawList& list, const Rect& bounds, float thickness, UiColor color) {
    if (bounds.width <= 0.0F || bounds.height <= 0.0F || thickness <= 0.0F) {
        return;
    }
    list.add_rect(bounds.x, bounds.y, bounds.width, thickness, color);
    list.add_rect(bounds.x, bounds.bottom() - thickness, bounds.width, thickness,
                  color);
    list.add_rect(bounds.x, bounds.y, thickness, bounds.height, color);
    list.add_rect(bounds.right() - thickness, bounds.y, thickness, bounds.height,
                  color);
}

} // namespace mediaserver::gui
