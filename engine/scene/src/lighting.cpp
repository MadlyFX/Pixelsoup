#include <mediaserver/scene/lighting.hpp>

#include <algorithm>
#include <array>
#include <cmath>

namespace mediaserver::scene {
namespace {
constexpr float pi = 3.14159265358979323846F;
struct Chromaticity { float x; float y; };
constexpr std::array<Chromaticity, 391U> temperatures{{
#include "light_temperature_xy.inc"
}};

Vec3 linear_rgb(Chromaticity xy) noexcept {
    const float x = xy.x / xy.y;
    const float z = (1.0F - xy.x - xy.y) / xy.y;
    return {3.2406F * x - 1.5372F - 0.4986F * z,
            -0.9689F * x + 1.8758F + 0.0415F * z,
            0.0557F * x - 0.2040F + 1.0570F * z};
}

template<class Light>
Vec3 light_color(const Light& light) noexcept {
    if (!light.use_color_temperature || light.color_temperature_kelvin == 6500.0F)
        return light.color;
    const Vec3 tint = light_temperature_rgb(light.color_temperature_kelvin);
    return {light.color.x * tint.x, light.color.y * tint.y, light.color.z * tint.z};
}
} // namespace

Vec3 light_temperature_rgb(float kelvin) noexcept {
    if (!std::isfinite(kelvin) || kelvin == 6500.0F) return {1.0F, 1.0F, 1.0F};
    const float offset = (std::clamp(kelvin, 1000.0F, 40000.0F) - 1000.0F) / 100.0F;
    const auto index = std::min(static_cast<std::size_t>(offset), temperatures.size() - 2U);
    const float weight = offset - static_cast<float>(index);
    const auto a = temperatures[index], b = temperatures[index + 1U];
    const Vec3 rgb = linear_rgb({a.x + (b.x - a.x) * weight, a.y + (b.y - a.y) * weight});
    const Vec3 white = linear_rgb(temperatures[55U]);
    const Vec3 adapted{std::max(rgb.x / white.x, 0.0F),
                       std::max(rgb.y / white.y, 0.0F),
                       std::max(rgb.z / white.z, 0.0F)};
    const float luminance = 0.2126F * adapted.x + 0.7152F * adapted.y + 0.0722F * adapted.z;
    return adapted * (1.0F / luminance);
}

bool supported_light_unit(const DirectionalLight& light) noexcept {
    return light.unit == LightUnit::Unitless || light.unit == LightUnit::Lux;
}

bool supported_light_unit(const PointLight& light) noexcept {
    return light.unit == LightUnit::Unitless || light.unit == LightUnit::Lumens
        || (area_light(light.kind) ? light.unit == LightUnit::Nits : light.unit == LightUnit::Candela);
}

float spot_solid_angle(Turns inner, Turns outer) noexcept {
    // Integral of smoothstep over its [outer cosine, inner cosine] interval
    // is half that interval; the inner cap contributes at full intensity.
    return 2.0F * pi * std::max(1.0F - 0.5F * (
        deterministic::cos_turns(inner) + deterministic::cos_turns(outer)), 1e-6F);
}

float spot_cone_attenuation(const PointLight& light, float alignment) noexcept {
    const float inner = deterministic::cos_turns(light.inner_cone);
    const float outer = deterministic::cos_turns(light.outer_cone);
    // Preserve legacy unitless cones exactly; photometric hard cones include
    // their inner boundary and avoid smoothstep(a,a,x)'s undefined division.
    if (light.unit != LightUnit::Unitless && inner == outer)
        return alignment >= inner ? 1.0F : 0.0F;
    const float interval = light.unit == LightUnit::Unitless
        ? std::max(inner - outer, 1e-4F) : inner - outer;
    const float cone = std::clamp((alignment - outer) / interval, 0.0F, 1.0F);
    return light.unit == LightUnit::Unitless ? cone * cone : cone * cone * (3.0F - 2.0F * cone);
}

LightEmission light_emission(const DirectionalLight& light, float scale) noexcept {
    return {light_color(light), light.unit == LightUnit::Unitless
        ? light.intensity : light.intensity * scale};
}

LightEmission light_emission(const PointLight& light, float scale) noexcept {
    float intensity = light.intensity;
    if (area_light(light.kind)) {
        if (light.unit == LightUnit::Lumens) intensity /= pi * emitting_area(light);
    } else if (light.unit == LightUnit::Lumens) {
        intensity /= light.kind == LocalLightKind::Spot
            ? spot_solid_angle(light.inner_cone, light.outer_cone) : 4.0F * pi;
    }
    if (light.unit != LightUnit::Unitless) intensity *= scale;
    return {light_color(light), intensity};
}
Vec3 area_light_tangent(const PointLight& light) noexcept {
    const Vec3 axis = normalize(light.direction, {0, -1, 0});
    const Vec3 reference = std::abs(axis.x) < 0.9F ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
    const Vec3 u = normalize(reference - axis * dot(reference, axis));
    return u * deterministic::cos_turns(light.area_rotation)
        + cross(axis, u) * deterministic::sin_turns(light.area_rotation);
}
AreaLightSample area_light_sample(const PointLight& light, core::u32 index) noexcept {
    constexpr std::array<Vec3, area_light_sample_count> directions{{
#include "area_directions.inc"
    }};
    index %= area_light_sample_count;
    if (light.kind == LocalLightKind::Sphere) {
        const Vec3 n = directions[index];
        return {light.position + n * light.radius, n, 4 * pi * light.radius * light.radius / 64};
    }
    const Vec3 n = normalize(light.direction, {0,-1,0});
    const Vec3 u = area_light_tangent(light), v = cross(n,u);
    const float x = (static_cast<float>(index % 8) + 0.5F) / 8 - 0.5F;
    const float y = (static_cast<float>(index / 8) + 0.5F) / 8 - 0.5F;
    return {light.position + u * (light.width * x) + v * (light.height * y), n, light.width * light.height / 64};
}
float emitting_area(const PointLight& light) noexcept {
    if (light.kind == LocalLightKind::Sphere) return 4.0F * pi * light.radius * light.radius;
    return light.width * light.height * (light.two_sided ? 2.0F : 1.0F);
}
} // namespace mediaserver::scene
