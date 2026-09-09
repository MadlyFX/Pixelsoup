#include <mediaserver/editor/light_guides.hpp>

#include <mediaserver/scene/lighting.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>

namespace mediaserver::editor {
namespace {
using scene::Vec3;
constexpr std::size_t circle_segments = 48U;

void segment(LightGuide& guide, Vec3 a, Vec3 b, LightGuideStyle style) noexcept {
    assert(guide.count < guide.storage.size());
    guide.storage[guide.count++] = {a, b, style};
}

void ring(LightGuide& guide, Vec3 centre, Vec3 u, Vec3 v, float radius,
          LightGuideStyle style) noexcept {
    Vec3 previous = centre + u * radius;
    for (std::size_t i = 1U; i <= circle_segments; ++i) {
        const float turns = static_cast<float>(i) / circle_segments;
        const Vec3 next = centre + u * (radius * scene::deterministic::cos_turns(turns))
                                + v * (radius * scene::deterministic::sin_turns(turns));
        // Dashes distinguish a distance cutoff from a physical emitter edge.
        if (style != LightGuideStyle::Range || i % 2U == 1U) {
            segment(guide, previous, next, style);
        }
        previous = next;
    }
}

void sphere(LightGuide& guide, Vec3 centre, float radius,
            LightGuideStyle style) noexcept {
    ring(guide, centre, {1, 0, 0}, {0, 1, 0}, radius, style);
    ring(guide, centre, {1, 0, 0}, {0, 0, 1}, radius, style);
    ring(guide, centre, {0, 1, 0}, {0, 0, 1}, radius, style);
}

void arrow(LightGuide& guide, Vec3 origin, Vec3 axis, Vec3 u, Vec3 v,
           float length) noexcept {
    const Vec3 tip = origin + axis * length;
    const Vec3 neck = tip - axis * (length * 0.22F);
    segment(guide, origin, tip, LightGuideStyle::Direction);
    for (const Vec3 side : {u, -u, v, -v}) {
        segment(guide, tip, neck + side * (length * 0.1F),
                LightGuideStyle::Direction);
    }
}

void cone(LightGuide& guide, Vec3 origin, Vec3 axis, Vec3 u, Vec3 v,
          float distance, scene::Turns angle, LightGuideStyle style) noexcept {
    // Range is radial distance, not axial depth. This stays finite even for
    // wide cones and makes every rim point lie on the renderer's cutoff sphere.
    const Vec3 centre = origin + axis * (distance * scene::deterministic::cos_turns(angle));
    const float radius = distance * scene::deterministic::sin_turns(angle);
    ring(guide, centre, u, v, radius, style);
    for (const Vec3 side : {u, -u, v, -v}) {
        segment(guide, origin, centre + side * radius, style);
    }
}
} // namespace

LightGuide light_guide(const scene::PointLight& light, bool selected) noexcept {
    LightGuide guide;
    const Vec3 axis = scene::normalize(light.direction, {0, -1, 0});
    const Vec3 u = scene::area_light_tangent(light);
    const Vec3 v = scene::cross(axis, u);
    switch (light.kind) {
    case scene::LocalLightKind::Sphere:
        sphere(guide, light.position, light.radius, LightGuideStyle::Source);
        if (selected) {
            segment(guide, light.position, light.position + Vec3{light.radius, 0, 0},
                    LightGuideStyle::Source);
        }
        break;
    case scene::LocalLightKind::Rectangle: {
        const Vec3 half_u = u * (light.width * 0.5F);
        const Vec3 half_v = v * (light.height * 0.5F);
        const std::array<Vec3, 4U> corners{
            light.position - half_u - half_v, light.position + half_u - half_v,
            light.position + half_u + half_v, light.position - half_u + half_v};
        for (std::size_t i = 0; i < corners.size(); ++i) {
            segment(guide, corners[i], corners[(i + 1U) % corners.size()],
                    LightGuideStyle::Source);
        }
        const float arrow_length = std::clamp(std::max(light.width, light.height) * 0.4F,
                                              0.35F, 1.5F);
        arrow(guide, light.position, axis, u, v, arrow_length);
        if (light.two_sided) {
            arrow(guide, light.position, -axis, u, v, arrow_length);
        }
        break;
    }
    case scene::LocalLightKind::Spot: {
        const float distance = selected ? light.range : std::min(light.range, 1.25F);
        cone(guide, light.position, axis, u, v, distance, light.outer_cone,
             LightGuideStyle::OuterCone);
        if (selected && light.inner_cone != light.outer_cone) {
            cone(guide, light.position, axis, u, v, distance, light.inner_cone,
                 LightGuideStyle::InnerCone);
        }
        if (selected) {
            // Four meridians show the curved radial cutoff across the cone.
            for (const Vec3 side : {u, -u, v, -v}) {
                Vec3 previous = light.position + axis * distance;
                for (std::size_t i = 1U; i <= 16U; ++i) {
                    const float angle = light.outer_cone * static_cast<float>(i) / 16.0F;
                    const Vec3 next = light.position
                        + axis * (distance * scene::deterministic::cos_turns(angle))
                        + side * (distance * scene::deterministic::sin_turns(angle));
                    if (i % 2U == 1U) segment(guide, previous, next, LightGuideStyle::Range);
                    previous = next;
                }
            }
        }
        arrow(guide, light.position, axis, u, v, std::min(distance, 1.0F));
        break;
    }
    case scene::LocalLightKind::Point:
        break;
    }
    if (selected && light.kind != scene::LocalLightKind::Spot) {
        sphere(guide, light.position, light.range, LightGuideStyle::Range);
    }
    return guide;
}

LightGuide directional_light_guide(Vec3 anchor, Vec3 direction) noexcept {
    scene::PointLight basis;
    basis.direction = direction;
    const Vec3 axis = scene::normalize(direction, {0, -1, 0});
    const Vec3 u = scene::area_light_tangent(basis);
    const Vec3 v = scene::cross(axis, u);
    LightGuide guide;
    // Parallel arrows communicate a sun's parallel rays, with no range cutoff.
    for (const Vec3 offset : {Vec3{}, u * 0.3F, -u * 0.3F}) {
        arrow(guide, anchor + offset, axis, u, v, 1.4F);
    }
    return guide;
}

std::optional<std::array<scene::CanvasProjection, 2U>> project_light_guide_segment(
    const LightGuideSegment& edge, const scene::ViewMatrices& matrices,
    scene::Canvas canvas) noexcept {
    const auto a = scene::transform(matrices.view_projection, {edge.a.x, edge.a.y, edge.a.z, 1});
    const auto b = scene::transform(matrices.view_projection, {edge.b.x, edge.b.y, edge.b.z, 1});
    const std::array<float, 6U> fa{a.w + a.x, a.w - a.x, a.w + a.y, a.w - a.y, a.z, a.w - a.z};
    const std::array<float, 6U> fb{b.w + b.x, b.w - b.x, b.w + b.y, b.w - b.y, b.z, b.w - b.z};
    float first = 0.0F, last = 1.0F;
    for (std::size_t i = 0; i < fa.size(); ++i) {
        if (!std::isfinite(fa[i]) || !std::isfinite(fb[i])) return std::nullopt;
        if (fa[i] < 0 && fb[i] < 0) return std::nullopt;
        if (fa[i] < 0) first = std::max(first, fa[i] / (fa[i] - fb[i]));
        if (fb[i] < 0) last = std::min(last, fa[i] / (fa[i] - fb[i]));
    }
    if (first > last) return std::nullopt;
    std::array<scene::CanvasProjection, 2U> points{};
    for (std::size_t i = 0; i < points.size(); ++i) {
        const float t = i == 0 ? first : last;
        const float w = a.w + (b.w - a.w) * t;
        if (!(w > 0)) return std::nullopt;
        points[i] = {
            std::clamp(0.5F + 0.5F * (a.x + (b.x - a.x) * t) / w, 0.0F, 1.0F) * canvas.width,
            std::clamp(0.5F - 0.5F * (a.y + (b.y - a.y) * t) / w, 0.0F, 1.0F) * canvas.height,
            (a.z + (b.z - a.z) * t) / w, true};
    }
    return points;
}
} // namespace mediaserver::editor
