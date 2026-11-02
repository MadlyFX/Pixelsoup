#pragma once

#include "../step_inputs.hpp"

#include <ajfx/effect.hpp>
#include <ajfx/field.hpp>
#include <ajfx/math.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <map>
#include <numbers>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

// Per-step module data for the GPU kernels: each module's id, then its inputs
// resolved for the step, plus anything the CPU reference derives once per
// step (unit axes, dt products, drag factors), computed here exactly as the
// reference computes it. The order of the floats is the order the module's
// function in shaders/ajfx_particles.hlsli reads them. Ids are data rather
// than kernel source, so giving a module a new id (and so new random draws)
// never needs the kernels compiled again.
namespace ajfx::detail {

// Where an emitter's distance fields sit in its fields buffer (in floats).
struct FieldLayout final {
    std::map<std::string, std::uint32_t, std::less<>> offsets;
    const FieldSet* fields{nullptr};
};

struct PackContext final {
    const Inputs& in;
    float dt;
    double start; // seconds at the start of the step
    // None when only the data's size matters (kernel generation).
    const FieldLayout* fields{nullptr};
    // The bodies this step's BodyCollision modules meet, by the emitter they
    // name. None leaves every collider empty, which is what kernel
    // generation wants: the data's size is the same either way.
    const std::map<std::string, std::vector<BodyCollider>, std::less<>>* bodies{nullptr};
};

class Packer final {
public:
    explicit Packer(std::vector<float>& out) : out_{out} {}

    void f(float value) { out_.push_back(value); }
    void u(std::uint32_t value) { out_.push_back(std::bit_cast<float>(value)); }
    void v3(Vec3 value) {
        f(value.x);
        f(value.y);
        f(value.z);
    }
    void c4(Color value) {
        f(value.r);
        f(value.g);
        f(value.b);
        f(value.a);
    }

private:
    std::vector<float>& out_;
};

inline Vec3 normalized(Vec3 v) {
    const float length_squared = dot(v, v);
    return length_squared > 0.0F ? v * (1.0F / std::sqrt(length_squared)) : Vec3{};
}

inline void pack(const Lifetime& m, const PackContext& c, Packer& p) {
    p.f(c.in(m.min));
    p.f(c.in(m.max));
}

inline void pack(const ShapeLocation& m, const PackContext& c, Packer& p) {
    p.u(static_cast<std::uint32_t>(m.shape));
    p.v3(c.in(m.center));
    p.f(c.in(m.radius_min));
    p.f(c.in(m.radius_max));
    p.v3(m.size);
    p.u(m.surface_only ? 1U : 0U);
}

inline void pack(const Velocity& m, const PackContext& c, Packer& p) {
    Vec3 axis = normalized(c.in(m.direction));
    if (dot(axis, axis) == 0.0F) {
        axis = {0.0F, 1.0F, 0.0F};
    }
    const Vec3 helper = std::fabs(axis.y) < 0.99F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
    const Vec3 t1 = normalized(cross(helper, axis));
    const Vec3 t2 = cross(axis, t1);
    p.v3(axis);
    p.v3(t1);
    p.v3(t2);
    p.f(std::cos(c.in(m.spread) * (std::numbers::pi_v<float> / 180.0F)));
    p.f(c.in(m.speed_min));
    p.f(c.in(m.speed_max));
    p.f(std::cos(c.in(m.spread_min) * (std::numbers::pi_v<float> / 180.0F)));
}

inline void pack(const InitColor& m, const PackContext& c, Packer& p) {
    p.c4(c.in(m.a));
    p.c4(c.in(m.b));
    p.f(c.in(m.brightness_min));
    p.f(c.in(m.brightness_max));
}

inline void pack(const InitSize& m, const PackContext& c, Packer& p) {
    p.f(c.in(m.min));
    p.f(c.in(m.max));
}

inline void pack(const InitRotation& m, const PackContext& c, Packer& p) {
    p.f(c.in(m.min));
    p.f(c.in(m.max));
    p.f(std::numbers::pi_v<float> / 180.0F);
}

inline void pack(const InitTumble& m, const PackContext& c, Packer& p) {
    p.f(c.in(m.speed_min));
    p.f(c.in(m.speed_max));
    p.f(std::numbers::pi_v<float> / 180.0F);
    p.u(m.random_orientation ? 1U : 0U);
}

// A curve's keys as the shaders' ajfx_curve reads them: the count, then
// (t, value) per key.
inline void pack_curve(const std::vector<CurveKey>& keys, Packer& p) {
    p.u(static_cast<std::uint32_t>(keys.size()));
    for (const CurveKey& key : keys) {
        p.f(key.t);
        p.f(key.value);
    }
}

inline void pack(const InitSpin& m, const PackContext& c, Packer& p) {
    p.f(c.in(m.speed_min));
    p.f(c.in(m.speed_max));
    p.f(std::numbers::pi_v<float> / 180.0F);
    p.u(m.both_ways ? 1U : 0U);
    p.f(1.0F / (1.0F + c.in(m.drag) * c.dt));
}

// A force's per-particle factor range, as ajfx_force_scale reads it.
template <class Force>
void pack_scale(const Force& m, const PackContext& c, Packer& p) {
    p.f(c.in(m.scale_min));
    p.f(c.in(m.scale_max));
}

inline void pack(const Gravity& m, const PackContext& c, Packer& p) {
    p.v3(c.in(m.acceleration) * c.dt);
    pack_scale(m, c, p);
    pack_curve(m.over_life, p);
}

inline void pack(const Drag& m, const PackContext& c, Packer& p) {
    const float rate = c.in(m.coefficient) * c.dt;
    p.f(1.0F / (1.0F + rate));
    p.f(rate);
    p.u(c.in(m.scale_min) == 1.0F && c.in(m.scale_max) == 1.0F ? 0U : 1U);
    pack_scale(m, c, p);
}

inline void pack(const CurlNoise& m, const PackContext& c, Packer& p) {
    p.f(c.in(m.frequency));
    p.f(c.in(m.strength) * c.dt);
    p.f(static_cast<float>(c.start) * c.in(m.evolution));
    p.u(m.seed);
    p.v3(c.in(m.drift) * static_cast<float>(c.start));
    pack_scale(m, c, p);
    pack_curve(m.over_life, p);
}

inline void pack(const Vortex& m, const PackContext& c, Packer& p) {
    p.v3(normalized(c.in(m.axis)));
    p.v3(c.in(m.center));
    p.f(c.in(m.strength) * c.dt);
    pack_scale(m, c, p);
}

inline void pack(const Attractor& m, const PackContext& c, Packer& p) {
    p.v3(c.in(m.position));
    p.f(c.in(m.strength));
    p.f(c.in(m.radius));
    p.f(c.dt);
    pack_scale(m, c, p);
}

inline void pack(const Converge& m, const PackContext& c, Packer& p) {
    p.v3(normalized(c.in(m.axis)));
    p.v3(c.in(m.center));
    p.f(c.in(m.strength) * c.dt);
    p.f(c.in(m.height));
    pack_scale(m, c, p);
}

inline void pack(const PlaneCollision& m, const PackContext& c, Packer& p) {
    p.v3(normalized(m.normal));
    p.f(m.offset);
    p.f(c.in(m.restitution));
    p.f(1.0F - c.in(m.friction));
    p.u(m.kill ? 1U : 0U);
}

inline void pack(const SphereCollision& m, const PackContext& c, Packer& p) {
    p.v3(c.in(m.center));
    p.f(c.in(m.radius));
    p.f(c.in(m.restitution));
    p.f(1.0F - c.in(m.friction));
    p.u(m.inside ? 1U : 0U);
    p.u(m.kill ? 1U : 0U);
}

// [restitution, 1 - friction, kill, count, then nine floats a body:
//  position, orientation, half size, 1 for a box]. The bodies travel in the
//  module data, which is written every step, so they are where the emitter
//  they belong to has just put them.
inline void pack(const BodyCollision& m, const PackContext& c, Packer& p) {
    p.f(c.in(m.restitution));
    p.f(1.0F - c.in(m.friction));
    p.u(m.kill ? 1U : 0U);
    const std::vector<BodyCollider>* bodies = nullptr;
    if (c.bodies != nullptr) {
        const auto found = c.bodies->find(m.emitter);
        if (found != c.bodies->end()) {
            bodies = &found->second;
        }
    }
    const std::size_t count = bodies != nullptr ? bodies->size() : 0U;
    p.u(static_cast<std::uint32_t>(count));
    // Room for every body the named emitter may hold, so the data is the
    // same size every step whatever is alive, and the same size the kernel
    // was generated for; the count says how much of it to read.
    std::uint32_t room = 0U;
    for (const Emitter& other : c.in.effect.emitters) {
        if (other.name == m.emitter) {
            room = std::min(other.capacity, max_body_colliders);
            break;
        }
    }
    for (std::size_t i = 0U; i < room; ++i) {
        const BodyCollider body = i < count ? (*bodies)[i] : BodyCollider{};
        p.v3(body.position);
        p.f(body.orientation.x);
        p.f(body.orientation.y);
        p.f(body.orientation.z);
        p.f(body.orientation.w);
        p.f(body.half);
        p.f(body.box ? 1.0F : 0.0F);
    }
}

inline void pack(const SdfCollision& m, const PackContext& c, Packer& p) {
    const DistanceField* field = nullptr;
    std::uint32_t at = 0U;
    if (c.fields != nullptr && c.fields->fields != nullptr) {
        const auto found = c.fields->fields->find(m.field);
        const auto offset = c.fields->offsets.find(m.field);
        if (found != c.fields->fields->end() && offset != c.fields->offsets.end()) {
            field = &found->second;
            at = offset->second;
        }
    }
    p.u(at);
    p.u(field != nullptr ? field->nx : 2U);
    p.u(field != nullptr ? field->ny : 2U);
    p.u(field != nullptr ? field->nz : 2U);
    p.v3(field != nullptr ? field->origin : Vec3{});
    p.f(field != nullptr ? field->spacing : 1.0F);
    p.v3(c.in(m.offset));
    p.f(c.in(m.restitution));
    p.f(1.0F - c.in(m.friction));
    p.u(m.inside ? 1U : 0U);
    p.u(m.kill ? 1U : 0U);
}

inline void pack(const BoxCollision& m, const PackContext& c, Packer& p) {
    p.v3(c.in(m.center));
    p.v3(m.half_size);
    p.f(c.in(m.restitution));
    p.f(1.0F - c.in(m.friction));
    p.u(m.inside ? 1U : 0U);
    p.u(m.kill ? 1U : 0U);
}

inline void pack(const RigidBody& m, const PackContext& c, Packer& p) {
    p.f(1.0F / (1.0F + c.in(m.angular_drag) * c.dt));
    p.f(0.5F * c.dt);
    p.f(m.contact_scale);
    p.f(c.in(m.rest_speed));
    p.f(c.in(m.roundness));
}

inline void pack(const ColorOverLife& m, const PackContext&, Packer& p) {
    p.u(static_cast<std::uint32_t>(m.keys.size()));
    for (const ColorKey& key : m.keys) {
        p.f(key.t);
        p.c4(key.value);
    }
}

inline void pack(const SizeOverLife& m, const PackContext&, Packer& p) {
    pack_curve(m.keys, p);
}

// Rigid physics emitters run on the CPU; their kernels read no module data.
inline void pack(const RigidPhysics&, const PackContext&, Packer&) {}

// Breaking happens in the rigid physics world, on the CPU, so it too leaves
// nothing for a kernel to read.
inline void pack(const Break&, const PackContext&, Packer&) {}

// So does holding bodies to each other.
inline void pack(const Glue&, const PackContext&, Packer&) {}

// One module: its id, then its data.
template <class Module>
void pack_module(const Module& module, const PackContext& context, Packer& p) {
    p.u(module.id);
    pack(module, context, p);
}

// The init stack, then the update stack, in order.
inline void pack_emitter(const Emitter& emitter, const PackContext& context, std::vector<float>& out) {
    Packer packer{out};
    const auto visit = [&](const auto& module) { pack_module(module, context, packer); };
    for (const InitModule& module : emitter.init) {
        std::visit(visit, module);
    }
    for (const UpdateModule& module : emitter.update) {
        std::visit(visit, module);
    }
}

// Module data floats for an emitter. The count does not depend on parameter
// values, so the kernels can bake each module's offset.
inline std::uint32_t module_float_count(const Effect& effect, const Emitter& emitter) {
    std::vector<ParameterValue> values;
    resolve_values(effect, nullptr, 0U, values);
    const Inputs in{effect, values};
    std::vector<float> out;
    pack_emitter(emitter, PackContext{in, 1.0F / static_cast<float>(effect.simulation_rate), 0.0}, out);
    return static_cast<std::uint32_t>(out.size());
}

} // namespace ajfx::detail
