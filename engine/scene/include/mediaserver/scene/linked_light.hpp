#pragma once
#include <mediaserver/scene/scene_schema.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/doc/document.hpp>
#include <array>
#include <cmath>

namespace mediaserver::scene {
inline constexpr std::array linked_light_fields{
    schema::color_r,schema::color_g,schema::color_b,schema::intensity,schema::range,
    schema::inner_cone_turns,schema::outer_cone_turns,schema::light_width,schema::light_height,
    schema::angular_radius_turns};
inline constexpr std::array linked_light_overrides{
    schema::color_r,schema::color_g,schema::color_b,schema::intensity,schema::range,
    schema::inner_cone_turns,schema::outer_cone_turns,schema::light_width,schema::light_height,
    schema::angular_radius_turns,schema::casts_shadow,schema::bake_indirect,
    schema::direction_x,schema::direction_y,schema::direction_z,schema::area_rotation_turns,
    schema::two_sided,schema::color_temperature_kelvin,schema::use_color_temperature,
    schema::shadow_tier};
// Input power is normalized photometry. Placement carries orientation; local
// dimensions are authored in metres and transformed once during link evaluation.
inline bool linked_light_values_valid(const doc::PropertyMap& p) {
    const auto number = [&](std::string_view key, double fallback) {
        const auto it=p.find(key); if (it==p.end()) return fallback;
        const auto* v=std::get_if<double>(&it->second); return v ? *v : NAN;
    };
    for (auto key : linked_light_fields) {
        const auto v=number(key,0); if (!std::isfinite(v) || v<0 || v>1e9) return false;
    }
    if (number(schema::range,10)<=0 || number(schema::range,10)>10000
        || number(schema::light_width,1)<0.001 || number(schema::light_width,1)>10000
        || number(schema::light_height,1)<0.001 || number(schema::light_height,1)>10000
        || number(schema::inner_cone_turns,0)>number(schema::outer_cone_turns,0.125)
        || number(schema::outer_cone_turns,0.125)>=0.249
        || number(schema::angular_radius_turns,0)>=0.5) return false;
    for (auto key : {schema::direction_x,schema::direction_y,schema::direction_z,schema::area_rotation_turns,schema::color_temperature_kelvin})
        if (!std::isfinite(number(key,0))) return false;
    for (auto key : {schema::casts_shadow,schema::bake_indirect,schema::two_sided,schema::use_color_temperature}) {
        if (const auto it=p.find(key); it!=p.end() && !std::holds_alternative<bool>(it->second)) return false;
    }
    return true;
}
} // namespace mediaserver::scene
