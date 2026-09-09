#pragma once

#include <mediaserver/scene/scene.hpp>

namespace mediaserver::scene {

[[nodiscard]] constexpr bool area_light(LocalLightKind kind) noexcept {
    return kind == LocalLightKind::Rectangle || kind == LocalLightKind::Sphere;
}
// Rectangle local X: world X projected onto the emission plane, rolled by
// authored turns; world Z is the fallback. Independent of every camera.
[[nodiscard]] Vec3 area_light_tangent(const PointLight& light) noexcept;
[[nodiscard]] float emitting_area(const PointLight& light) noexcept;
struct AreaLightSample final { Vec3 position, normal; float area; };
inline constexpr core::u32 area_light_sample_count = 64;
[[nodiscard]] AreaLightSample area_light_sample(const PointLight& light, core::u32 index) noexcept;

// Linear Rec.709, Planckian chromaticity adapted to neutral 6500 K. Unit
// luminance preserves photometric intensity; negative out-of-gamut channels
// are clipped. The table and arithmetic are identical on every node.
[[nodiscard]] Vec3 light_temperature_rgb(float kelvin) noexcept;

struct LightEmission final {
    Vec3 color;
    float intensity{};
};

// Unitless is an exact passthrough. Directional photometry uses lux; point
// and spot photometry use candela or lumens. Nits require an area emitter.
[[nodiscard]] bool supported_light_unit(const DirectionalLight& light) noexcept;
[[nodiscard]] bool supported_light_unit(const PointLight& light) noexcept;
[[nodiscard]] LightEmission light_emission(
    const DirectionalLight& light, float photometric_scale) noexcept;
[[nodiscard]] LightEmission light_emission(
    const PointLight& light, float photometric_scale) noexcept;

// Half-angles in turns. Integrates the smoothstep cone over solid angle,
// including its penumbra, so a lumen value denotes total emitted flux.
[[nodiscard]] float spot_solid_angle(Turns inner, Turns outer) noexcept;
[[nodiscard]] float spot_cone_attenuation(
    const PointLight& light, float alignment) noexcept;

} // namespace mediaserver::scene
