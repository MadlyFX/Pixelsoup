#include <algorithm>
#include <cmath>
#include <limits>
#include <mediaserver/render/geometry_visibility.hpp>
#include <mediaserver/render/slice_apron.hpp>

namespace mediaserver::render {
namespace {
bool finite(scene::Vec3 p) noexcept {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}
bool corners(const scene::Mat4& world, const ModelBounds& bounds,
             std::array<scene::Vec3, 8U>& result) noexcept {
    if (!finite(bounds.minimum) || !finite(bounds.maximum) ||
        bounds.minimum.x > bounds.maximum.x ||
        bounds.minimum.y > bounds.maximum.y ||
        bounds.minimum.z > bounds.maximum.z)
        return false;
    for (core::u32 i = 0U; i < 8U; ++i) {
        result[i] = scene::transform_point(
            world, {(i & 1U) ? bounds.maximum.x : bounds.minimum.x,
                    (i & 2U) ? bounds.maximum.y : bounds.minimum.y,
                    (i & 4U) ? bounds.maximum.z : bounds.minimum.z});
        if (!finite(result[i]))
            return false;
    }
    return true;
}
} // namespace

GeometryFrusta::GeometryFrusta(const scene::Scene& frame) noexcept {
    // One extra pixel covers any temporal jitter and raster roundoff. The
    // apron comes from the whole scene, including isolated spatial effects.
    const float apron =
        static_cast<float>(apron_requirements(frame).maximum()) + 1.0F;
    const auto add = [&](const scene::Camera& camera, float margin) {
        if (!scene::valid(camera) || !scene::valid(frame.canvas) ||
            count_ >= planes_.size()) {
            conservative_ = true;
            return;
        }
        const auto vp = scene::view_for_slice(
                            camera, frame.canvas,
                            {0U, 0U, frame.canvas.width, frame.canvas.height})
                            .view_projection;
        const auto row = [&](core::u32 r) {
            return scene::Vec4{vp.m[r], vp.m[4U + r], vp.m[8U + r],
                               vp.m[12U + r]};
        };
        const auto x = row(0U), y = row(1U), z = row(2U), w = row(3U);
        const float sx = 1.0F +
                         2.0F * apron / static_cast<float>(frame.canvas.width) +
                         margin;
        const float sy =
            1.0F + 2.0F * apron / static_cast<float>(frame.canvas.height) +
            margin;
        auto& p = planes_[count_++];
        p = {{{w.x * sx + x.x, w.y * sx + x.y, w.z * sx + x.z, w.w * sx + x.w},
              {w.x * sx - x.x, w.y * sx - x.y, w.z * sx - x.z, w.w * sx - x.w},
              {w.x * sy + y.x, w.y * sy + y.y, w.z * sy + y.z, w.w * sy + y.w},
              {w.x * sy - y.x, w.y * sy - y.y, w.z * sy - y.z, w.w * sy - y.w},
              z,
              {w.x - z.x, w.y - z.y, w.z - z.z, w.w - z.w}}};
        for (auto& plane : p) {
            const float length = std::sqrt(
                plane.x * plane.x + plane.y * plane.y + plane.z * plane.z);
            if (!(length > 0.0F) || !std::isfinite(length)) {
                conservative_ = true;
                continue;
            }
            plane = {plane.x / length, plane.y / length, plane.z / length,
                     plane.w / length};
        }
    };
    add(frame.camera, 0.0F);
    if (frame.renderer_settings.icvfx.inner_frustum) {
        for (const auto& tracked : frame.tracked_cameras) {
            if (tracked.drawing()) {
                // The scene does not carry the physical wall rectangle. Its
                // overscanned off-axis plate can exceed the ideal lens cone;
                // retain the full set until those replicated volumes are
                // supplied.
                conservative_ = true;
                add(scene::tracked_view_camera(tracked.authored, tracked.pose,
                                               tracked.encoders),
                    2.0F * static_cast<float>(tracked.icvfx.margin_px) /
                        static_cast<float>(
                            std::min(frame.canvas.width, frame.canvas.height)));
            }
        }
    }
}

bool GeometryFrusta::visible(const scene::Mat4& world,
                             const ModelBounds& bounds) const noexcept {
    std::array<scene::Vec3, 8U> points{};
    if (conservative_ || count_ == 0U || !corners(world, bounds, points))
        return true;
    const auto center = (points[0U] + points[7U]) * 0.5F;
    float radius_squared = 0.0F;
    for (const auto p : points) {
        const auto delta = p - center;
        radius_squared = std::max(radius_squared, scene::dot(delta, delta));
    }
    const float radius = std::sqrt(radius_squared);
    for (core::u32 i = 0U; i < count_; ++i) {
        bool outside = false;
        for (const auto p : planes_[i]) {
            const auto distance = [&](scene::Vec3 v) {
                return p.x * v.x + p.y * v.y + p.z * v.z + p.w;
            };
            // A small outward tolerance prevents boundary roundoff becoming a
            // false negative. Non-finite arithmetic is conservative as well.
            if (distance(center) < -radius - 0.001F) {
                outside = true;
                break;
            }
            if (distance(center) <= radius + 0.001F) {
                bool all_outside = true;
                for (const auto v : points)
                    all_outside = all_outside && distance(v) < -0.001F;
                if (all_outside) {
                    outside = true;
                    break;
                }
            }
        }
        if (!outside)
            return true;
    }
    return false;
}

float projected_model_pixels(const scene::Scene& frame,
                             const scene::Mat4& world,
                             const ModelBounds& bounds) noexcept {
    constexpr float finest = std::numeric_limits<float>::infinity();
    std::array<scene::Vec3, 8U> points{};
    if (!scene::valid(frame.canvas) || !scene::valid(frame.camera) ||
        !corners(world, bounds, points))
        return finest;
    const auto vp =
        scene::view_for_slice(frame.camera, frame.canvas,
                              {0U, 0U, frame.canvas.width, frame.canvas.height})
            .view_projection;
    float min_x = finest, min_y = finest, max_x = -finest, max_y = -finest;
    for (auto p : points) {
        const auto clip = scene::transform(vp, {p.x, p.y, p.z, 1.0F});
        if (!(clip.w > frame.camera.near_plane) || !std::isfinite(clip.w))
            return finest;
        const float x = clip.x / clip.w, y = clip.y / clip.w;
        if (!std::isfinite(x) || !std::isfinite(y))
            return finest;
        min_x = std::min(min_x, x);
        max_x = std::max(max_x, x);
        min_y = std::min(min_y, y);
        max_y = std::max(max_y, y);
    }
    return std::max(
        (max_x - min_x) * 0.5F * static_cast<float>(frame.canvas.width),
        (max_y - min_y) * 0.5F * static_cast<float>(frame.canvas.height));
}

core::u32 select_model_lod(const scene::Scene& frame, const scene::Mat4& world,
                           const ModelBounds& bounds,
                           std::span<const ModelLod> levels) noexcept {
    if (!frame.renderer_settings.model_lod)
        return 0U;
    const float pixels = projected_model_pixels(frame, world, bounds);
    core::u32 selected = 0U;
    for (core::u32 i = 1U; i < levels.size(); ++i) {
        if (pixels <= levels[i].max_pixels)
            selected = i;
    }
    return selected;
}
} // namespace mediaserver::render
