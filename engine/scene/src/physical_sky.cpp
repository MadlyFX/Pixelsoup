#include <mediaserver/scene/scene.hpp>
#include <algorithm>
#include <cmath>

namespace mediaserver::scene {
namespace {
constexpr float pi = 3.14159265358979323846F;
constexpr float earth = 6360.0F; // kilometres; matches physical_sky.comp.hlsl
constexpr float atmosphere = 6460.0F;
constexpr Vec3 beta{0.005802F, 0.013558F, 0.033100F};
constexpr Vec3 observer{0, earth + 0.001F, 0};
float top_distance(Vec3 p, Vec3 d) noexcept {
    const float b = dot(p, d);
    return -b + std::sqrt(std::max(b*b - dot(p,p) + atmosphere*atmosphere, 0.0F));
}
Vec3 extinction(float rayleigh, float mie, float haze) noexcept {
    return {std::exp(-beta.x*rayleigh - 0.00444F*haze*mie),
            std::exp(-beta.y*rayleigh - 0.00444F*haze*mie),
            std::exp(-beta.z*rayleigh - 0.00444F*haze*mie)};
}
Vec3 sunlight(Vec3 p, Vec3 d, float haze) noexcept {
    const float b = dot(p,d);
    if (b < 0 && b*b - dot(p,p) + earth*earth > 0) return {};
    const float step = top_distance(p,d) / 16.0F;
    float r = 0, m = 0;
    for (unsigned i=0; i<16; ++i) {
        const auto q = p + d*((static_cast<float>(i)+0.5F)*step);
        const float h = std::max(std::sqrt(dot(q,q))-earth,0.0F);
        r += std::exp(-h/8.0F)*step;
        m += std::exp(-h/1.2F)*step;
    }
    return extinction(r,m,haze);
}
Vec3 product(Vec3 a, Vec3 b) noexcept { return {a.x*b.x,a.y*b.y,a.z*b.z}; }
}
PhysicalSky clamped(const PhysicalSky& value) noexcept {
    PhysicalSky r = value;
    const PhysicalSky defaults{};
    const auto bounded = [](float v, float lo, float hi, float fallback) {
        return std::isfinite(v) ? std::clamp(v,lo,hi) : fallback;
    };
    r.time_of_day = bounded(r.time_of_day,0,24,defaults.time_of_day);
    r.latitude = bounded(r.latitude,-89,89,defaults.latitude);
    r.north_rotation = bounded(r.north_rotation,-1,1,0);
    r.haze = bounded(r.haze,0,8,defaults.haze);
    r.solar_illuminance = bounded(r.solar_illuminance,0,200000,defaults.solar_illuminance);
    r.angular_radius = bounded(r.angular_radius,0.00005F,0.025F,defaults.angular_radius);
    r.ground_albedo = bounded(r.ground_albedo,0,1,defaults.ground_albedo);
    return r;
}
Vec3 physical_sun_direction(const PhysicalSky& value) noexcept {
    const auto s = clamped(value);
    const float hour = (s.time_of_day-12.0F)/24.0F;
    // The scene math uses deterministic trigonometric polynomials.
    const Vec3 noon = rotate_around({0,1,0},{1,0,0},s.latitude/360.0F);
    const Vec3 axis = rotate_around({0,0,1},{1,0,0},s.latitude/360.0F);
    return normalize(rotate_around(rotate_around(noon,axis,hour),{0,1,0},s.north_rotation),{0,1,0});
}
Vec3 physical_sun_illuminance(const PhysicalSky& value) noexcept {
    const auto s = clamped(value);
    const auto d = physical_sun_direction(s);
    return d.y > 0 ? sunlight(observer,d,s.haze)*s.solar_illuminance : Vec3{};
}
DirectionalLight physical_sun(const PhysicalSky& value, doc::NodeId source) noexcept {
    const auto s = clamped(value);
    DirectionalLight light{};
    light.direction = physical_sun_direction(s)*-1.0F;
    light.color = physical_sun_illuminance(s);
    light.intensity = s.enabled ? 1.0F : 0.0F;
    light.unit = LightUnit::Lux;
    light.source = source;
    light.locked = true;
    light.angular_radius = s.angular_radius;
    return light;
}
Vec3 physical_sky_radiance(const PhysicalSky& value, Vec3 direction) noexcept {
    const auto s = clamped(value);
    const auto sun = physical_sun_direction(s);
    const auto d = normalize(direction,{0,1,0});
    if (d.y < 0) return physical_sun_illuminance(s)*(std::max(sun.y,0.0F)*s.ground_albedo/pi);
    const float step = top_distance(observer,d)/32.0F;
    const float mu = dot(d,sun);
    const float rayleigh_phase = 3.0F*(1+mu*mu)/(16*pi);
    constexpr float g = 0.8F;
    const float denom = std::max(1+g*g-2*g*mu,0.001F);
    const float mie_phase = (1-g*g)/(4*pi*denom*std::sqrt(denom));
    float optical_r=0, optical_m=0;
    Vec3 sum{};
    for (unsigned i=0; i<32; ++i) {
        const auto p = observer+d*((static_cast<float>(i)+0.5F)*step);
        const float h = std::max(std::sqrt(dot(p,p))-earth,0.0F);
        const float r=std::exp(-h/8.0F), m=std::exp(-h/1.2F);
        const auto trans = product(extinction(optical_r+r*step*0.5F,optical_m+m*step*0.5F,s.haze),sunlight(p,sun,s.haze));
        const float mie = 0.003996F*s.haze*m*mie_phase;
        sum = sum + product(trans,beta*(r*rayleigh_phase)+Vec3{mie,mie,mie})*step;
        optical_r += r*step; optical_m += m*step;
    }
    return sum*s.solar_illuminance;
}
} // namespace mediaserver::scene
