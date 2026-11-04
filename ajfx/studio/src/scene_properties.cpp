#include "scene_properties.hpp"

#include <cmath>
#include <type_traits>

namespace ajfx::studio {
namespace {
template <class T> Value encode(const T &v) {
    if constexpr (std::is_same_v<T, Vec3>) {
        return vec3(v.x, v.y, v.z);
    } else if constexpr (std::is_same_v<T, Range>) {
        return range(v.min, v.max);
    } else if constexpr (std::is_same_v<T, Color>) {
        return color(v.r, v.g, v.b, v.a);
    } else if constexpr (std::is_same_v<T, std::string>) {
        return choice(v);
    } else if constexpr (std::is_same_v<T, bool>) {
        return toggle(v);
    } else {
        return number(static_cast<float>(v));
    }
}
template <class T> T decode(const Value &v) {
    if constexpr (std::is_same_v<T, Vec3>) {
        return {v.x, v.y, v.z};
    } else if constexpr (std::is_same_v<T, Range>) {
        return {v.x, v.y};
    } else if constexpr (std::is_same_v<T, Color>) {
        return {v.x, v.y, v.z, v.w};
    } else if constexpr (std::is_same_v<T, std::string>) {
        return v.text;
    } else if constexpr (std::is_same_v<T, bool>) {
        return v.x != 0.0F;
    } else {
        return static_cast<T>(v.x);
    }
}

template <class Owner, class T>
SceneProperty property(std::string_view id, std::string_view label, std::string_view section, T Owner::*member,
                       float lo = 0.0F, float hi = 100.0F, std::string_view unit = {},
                       std::initializer_list<std::string_view> options = {}) {
    ControlDef control;
    control.id = id;
    control.label = label;
    control.fallback = encode(Owner{}.*member);
    control.min = control.hard_min = lo;
    control.max = control.hard_max = hi;
    control.unit = unit;
    if constexpr (std::is_same_v<T, Vec3>) {
        control.type = ControlType::Vec3;
    } else if constexpr (std::is_same_v<T, Range>) {
        control.type = ControlType::Range;
    } else if constexpr (std::is_same_v<T, Color>) {
        control.type = ControlType::Color;
    } else if constexpr (std::is_same_v<T, std::string>) {
        control.type = ControlType::File;
        control.files = FileKind::Model;
    } else if constexpr (std::is_same_v<T, bool>) {
        control.type = ControlType::Toggle;
    } else if constexpr (std::is_enum_v<T>) {
        control.type = ControlType::Choice;
        // Values are ordinal, serialized as numbers; the UI resolves labels.
        for (const std::string_view option : options) {
            control.choices.push_back(ChoiceDef{option, option});
        }
    } else {
        control.type = ControlType::Number;
        if constexpr (std::is_integral_v<T>) {
            control.decimals = 0;
        }
    }
    const auto read = [member](const SceneItem &item) { return encode(std::get<Owner>(item.data).*member); };
    const auto write = [member](SceneItem &item, const Value &v) { std::get<Owner>(item.data).*member = decode<T>(v); };
    return SceneProperty{section, std::move(control), read, write};
}

const std::vector<SceneProperty> &object_properties() {
    static const std::vector<SceneProperty> properties = [] {
        std::vector<SceneProperty> out{
            property("position", "Position", "Transform", &Object::position, -10000, 10000, "m"),
            property("rotation", "Rotation", "Transform", &Object::rotation, -36000, 36000, "deg"),
            property("size", "Longest side", "Transform", &Object::size, 0.01F, 1000, "m"),
            property("mesh", "Model", "Geometry", &Object::mesh),
            property("distribution", "Distribution", "Geometry", &Object::distribution, 0, 2, {},
                     {"Single", "Grid", "Pile"}),
            property("count", "Instance count", "Geometry", &Object::count, 1, 20000),
            property("width", "Width", "Geometry", &Object::width, 0.01F, 1000, "m"),
            property("depth", "Depth", "Geometry", &Object::depth, 0.01F, 1000, "m"),
            property("spacing", "Spacing", "Geometry", &Object::spacing, 0.5F, 4, "x"),
            property("variation", "Position variation", "Geometry", &Object::variation, 0, 1),
            property("tint", "Color", "Surface", &Object::tint, 0, 64),
            property("motion", "Motion", "Physics", &Object::motion, 0, 2, {}, {"Static", "Dynamic", "Animated"}),
            property("collider", "Collider", "Physics", &Object::collider, 0, 2, {}, {"Convex hull", "Sphere", "Box"}),
            property("density", "Density", "Physics", &Object::density, 1, 100000, "kg/m^3"),
            property("friction", "Friction", "Physics", &Object::friction, 0, 1),
            property("restitution", "Restitution", "Physics", &Object::restitution, 0, 1),
            property("velocity", "Initial velocity", "Physics", &Object::velocity, -10000, 10000, "m/s"),
            property("release", "Release time", "Physics", &Object::release, 0, 3600, "s"),
            property("move_to", "Move to", "Animation", &Object::move_to, -10000, 10000, "m"),
            property("turn_to", "Rotate to", "Animation", &Object::turn_to, -36000, 36000, "deg"),
            property("move_at", "Start time", "Animation", &Object::move_at, 0, 3600, "s"),
            property("move_over", "Duration", "Animation", &Object::move_over, 0, 3600, "s"),
            property("fracture", "Enabled", "Fracture", &Object::fracture),
            property("fragments", "Fragment count", "Fracture", &Object::fragments, 2, 64),
            property("fracture_speed", "Impact threshold", "Fracture", &Object::fracture_speed, 0.1F, 1000, "m/s")};
        for (SceneProperty &p : out) {
            if (p.control.id == "tint") {
                p.control.timing = ParameterTiming::Playback;
            }
        }
        return out;
    }();
    return properties;
}
const std::vector<SceneProperty> &system_properties() {
    static const std::vector<SceneProperty> properties = [] {
        std::vector<SceneProperty> out{
            property("position", "Position", "Initial state / Sources", &ParticleSystem::position, -10000, 10000, "m"),
            property("radius", "Radius", "Initial state / Sources", &ParticleSystem::radius, 0, 1000, "m"),
            property("persistent", "Persistent population", "Initial state / Sources", &ParticleSystem::persistent),
            property("lifetime", "Lifetime", "Initial state / Sources", &ParticleSystem::lifetime, 0.01F, 3600, "s"),
            property("direction", "Direction", "Motion", &ParticleSystem::direction, -10000, 10000, "m"),
            property("speed", "Speed", "Motion", &ParticleSystem::speed, 0, 10000, "m/s"),
            property("spread", "Velocity spread", "Motion", &ParticleSystem::spread, 0, 180, "deg"),
            property("gravity", "Gravity share", "Motion", &ParticleSystem::gravity, 0, 10, "x"),
            property("tint", "Color", "Appearance", &ParticleSystem::tint, 0, 64),
            property("brightness", "Brightness", "Appearance", &ParticleSystem::brightness, 0, 64, "x"),
            property("size", "Size", "Appearance", &ParticleSystem::size, 0.001F, 100, "m"),
            property("spin", "Spin", "Appearance", &ParticleSystem::spin, 0, 7200, "deg/s"),
            property("look", "Renderer", "Appearance", &ParticleSystem::look, 0, 2, {}, {"Glow", "Smoke", "Solid"}),
            property("lighting", "Light", "Appearance", &ParticleSystem::lighting, 0, 1, {},
                     {"Glows on its own", "Lit by the scene"}),
            property("curvature", "Rounded", "Appearance", &ParticleSystem::curvature, 0, 1),
            property("wrap", "Light wraps", "Appearance", &ParticleSystem::wrap, 0, 1),
            property("ambient", "Takes ambient", "Appearance", &ParticleSystem::ambient, 0, 16, "x"),
            property("shadowed", "Takes shadows", "Appearance", &ParticleSystem::shadowed)};
        out.push_back(property("collide", "Collide with objects", "Interactions", &ParticleSystem::collide));
        for (SceneProperty &p : out) {
            if (p.control.id == "tint" || p.control.id == "look") {
                p.control.timing = ParameterTiming::Playback;
            }
        }
        return out;
    }();
    return properties;
}
const std::vector<SceneProperty> &force_properties() {
    static const std::vector<SceneProperty> properties = [] {
        std::vector<SceneProperty> out{
            property("kind", "Kind", "Force", &Force::kind, 0, 4, {}, {"Wind", "Vortex", "Attract", "Draw in", "Blast"}),
            property("position", "Position", "Force", &Force::position, -10000, 10000, "m"),
            property("strength", "Strength", "Force", &Force::strength, -1000, 1000, "m/s^2"),
            property("heading", "Heading", "Force", &Force::heading, 0, 360, "deg"),
            property("axis", "Axis", "Force", &Force::axis, 0, 1, {}, {"Vertical", "Horizontal"}),
            property("reach", "Reach", "Force", &Force::reach, 0, 1000, "m"),
            property("height", "Fades by", "Force", &Force::height, 0, 1000, "m"),
            property("power", "Power", "Force", &Force::power, 0, 1000, "m/s"),
            property("radius", "Radius", "Force", &Force::radius, 0.01F, 1000, "m"),
            property("time", "Goes off at", "Force", &Force::time, 0, 3600, "s"),
            property("all_systems", "Every particle system", "Targets", &Force::all_systems)};
        for (SceneProperty &p : out) {
            if (p.control.id == "heading" || p.control.id == "time") {
                p.control.decimals = p.control.id == "heading" ? 0 : 2;
            }
        }
        return out;
    }();
    return properties;
}
// A smoke grid, in the artist's words. The runtime's own names for the
// fluid settings are in scene.hpp beside each field. Look settings are
// read when the smoke is drawn, never by the simulation, so they keep a
// motion cache.
const std::vector<SceneProperty> &smoke_grid_properties() {
    static const std::vector<SceneProperty> properties = [] {
        std::vector<SceneProperty> out{
            property("position", "Position", "Transform", &SmokeGrid::position, -10000, 10000, "m"),
            property("size", "Size", "Transform", &SmokeGrid::size, 0.02F, 1000, "m"),
            property("cell", "Cell size", "Smoke", &SmokeGrid::cell, 0.01F, 100, "m"),
            property("rises", "Rises", "Smoke", &SmokeGrid::rises, 0, 100),
            property("sinks", "Sinks", "Smoke", &SmokeGrid::sinks, 0, 100),
            property("swirl", "Swirl", "Smoke", &SmokeGrid::swirl, 0, 100),
            property("fades", "Fades", "Smoke", &SmokeGrid::fades, 0, 100, "/s"),
            property("calms", "Calms", "Smoke", &SmokeGrid::calms, 0, 100, "/s"),
            property("solve", "Solve quality", "Smoke", &SmokeGrid::solve, 1,
                     static_cast<float>(max_volume_pressure_iterations)),
            property("thickness", "Thickness", "Look", &SmokeGrid::thickness, 0, 100),
            property("color", "Color", "Look", &SmokeGrid::color, 0, 1),
            property("brightness", "Brightness", "Look", &SmokeGrid::brightness, 0, 1)};
        for (SceneProperty &p : out) {
            if (p.section == "Look") {
                p.control.timing = ParameterTiming::Playback;
            }
        }
        return out;
    }();
    return properties;
}
} // namespace

std::span<const SceneProperty> scene_properties(const SceneItem &item) {
    if (std::holds_alternative<Object>(item.data)) {
        return object_properties();
    }
    if (std::holds_alternative<ParticleSystem>(item.data)) {
        return system_properties();
    }
    if (std::holds_alternative<Force>(item.data)) {
        return force_properties();
    }
    if (std::holds_alternative<SmokeGrid>(item.data)) {
        return smoke_grid_properties();
    }
    return {};
}

std::string validate_scene_properties(const SceneItem &item) {
    for (const SceneProperty &property : scene_properties(item)) {
        const Value v = property.read(item);
        const ControlDef &c = property.control;
        if (c.type == ControlType::File) {
            continue;
        }
        const int count = c.type == ControlType::Vec3    ? 3
                          : c.type == ControlType::Color ? 4
                          : c.type == ControlType::Range ? 2
                                                         : 1;
        const float values[] = {v.x, v.y, v.z, v.w};
        for (int k = 0; k < count; ++k) {
            if (!std::isfinite(values[k]) || values[k] < c.hard_min || values[k] > c.hard_max) {
                return item.name + ": " + std::string{property.section} + " / " + std::string{c.label} +
                       " is outside its supported range";
            }
        }
        if (c.type == ControlType::Color && v.w > 1.0F) {
            return item.name + ": opacity must be between 0 and 1";
        }
    }
    return {};
}
} // namespace ajfx::studio
