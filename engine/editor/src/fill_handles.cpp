#include <mediaserver/editor/fill_handles.hpp>

#include <algorithm>
#include <cmath>

namespace mediaserver::editor {

namespace {

constexpr float tau = 6.28318530717958647692F;
// The shortest ramp or radius a drag makes, as a size: a press that never
// moved leaves a visible fill rather than a hard edge of no width.
constexpr float minimum_size = 0.01F;

[[nodiscard]] float clamped_centre(float value) noexcept {
    return std::clamp(value, -scene::max_fill_center, scene::max_fill_center);
}

[[nodiscard]] float clamped_size(float value) noexcept {
    return std::isfinite(value)
               ? std::clamp(value, minimum_size, scene::max_fill_size)
               : minimum_size;
}

// A linear ramp's full length for a size of one: the surface's extent along
// the direction (x, y).
[[nodiscard]] float extent_along(float x, float y, float width,
                                 float height) noexcept {
    return std::abs(x) * width + std::abs(y) * height;
}

[[nodiscard]] float half_diagonal(float width, float height) noexcept {
    return 0.5F * std::sqrt(width * width + height * height);
}

}  // namespace

FillHandles fill_handles(const scene::Fill& fill, float width,
                         float height) noexcept {
    FillHandles handles{};
    if (fill.mode == scene::FillMode::Solid || !(width > 0.0F) ||
        !(height > 0.0F)) {
        return handles;
    }
    handles.visible = true;
    handles.centre = BoxPoint{fill.center_x, fill.center_y};
    if (fill.mode == scene::FillMode::Linear) {
        const float x = std::cos(fill.angle * tau);
        const float y = std::sin(fill.angle * tau);
        const float half = extent_along(x, y, width, height) * fill.size * 0.5F;
        handles.start = BoxPoint{fill.center_x - x * half / width,
                                 fill.center_y - y * half / height};
        handles.end = BoxPoint{fill.center_x + x * half / width,
                               fill.center_y + y * half / height};
    } else {
        handles.start = handles.centre;
        handles.end = BoxPoint{
            fill.center_x + half_diagonal(width, height) * fill.size / width,
            fill.center_y};
    }
    return handles;
}

scene::Fill fill_with_centre(scene::Fill fill, BoxPoint centre) noexcept {
    if (std::isfinite(centre.u) && std::isfinite(centre.v)) {
        fill.center_x = clamped_centre(centre.u);
        fill.center_y = clamped_centre(centre.v);
    }
    return fill;
}

scene::Fill fill_with_end(scene::Fill fill, float width, float height,
                          BoxPoint end) noexcept {
    if (fill.mode == scene::FillMode::Solid || !(width > 0.0F) ||
        !(height > 0.0F) || !std::isfinite(end.u) || !std::isfinite(end.v)) {
        return fill;
    }
    const float dx = (end.u - fill.center_x) * width;
    const float dy = (end.v - fill.center_y) * height;
    const float reach = std::hypot(dx, dy);
    if (fill.mode == scene::FillMode::Radial) {
        fill.size = clamped_size(reach / half_diagonal(width, height));
        return fill;
    }
    if (reach <= 0.0F) {
        return fill;
    }
    const float x = dx / reach;
    const float y = dy / reach;
    float turns = std::atan2(y, x) / tau;
    if (turns < 0.0F) {
        turns += 1.0F;
    }
    fill.angle = turns;
    fill.size =
        clamped_size(2.0F * reach / extent_along(x, y, width, height));
    return fill;
}

scene::Fill fill_along(scene::Fill fill, float width, float height,
                       BoxPoint from, BoxPoint to) noexcept {
    if (fill.mode == scene::FillMode::Solid) {
        return fill;
    }
    if (fill.mode == scene::FillMode::Radial) {
        return fill_with_end(fill_with_centre(fill, from), width, height, to);
    }
    // The ramp's middle is the line's middle, and its far end the line's.
    fill = fill_with_centre(
        fill, BoxPoint{(from.u + to.u) * 0.5F, (from.v + to.v) * 0.5F});
    return fill_with_end(fill, width, height, to);
}

}  // namespace mediaserver::editor
