#pragma once

#include "../body_grid.hpp"
#include "../step_inputs.hpp"

#include <ajfx/effect.hpp>
#include <ajfx/field.hpp>
#include <ajfx/math.hpp>

#include <algorithm>
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

// Where an emitter's distance fields sit in its fields buffer (in floats),
// and after them, where the bodies of each emitter its BodyCollision and
// Wake modules name are written every step (body_region_floats each).
struct FieldLayout final {
    std::map<std::string, std::uint32_t, std::less<>> offsets;
    const FieldSet* fields{nullptr};
    std::map<std::string, std::uint32_t, std::less<>> body_offsets;
};

// Floats one emitter's bodies take in a fields buffer, for an emitter of
// `capacity`: each cell's first body (one more entry than cells), the body
// indices by cell, then each body's record.
inline std::uint64_t body_region_floats(std::uint32_t capacity) {
    return std::uint64_t{body_grid_cells} + 1U + std::uint64_t{capacity} * (1U + body_record_floats);
}

struct PackContext final {
    const Inputs& in;
    float dt;
    double start; // seconds at the start of the step
    // None when only the data's size matters (kernel generation).
    const FieldLayout* fields{nullptr};
    // The bodies this step's BodyCollision and Wake modules meet, by the
    // emitter they name, in their grids. None leaves every one empty, which
    // is what kernel generation wants: the data's size is the same either
    // way.
    const BodyGrids* bodies{nullptr};
    // The step being taken, which a blast goes off in or not. Kernel
    // generation leaves it at 0: only the data's size matters there.
    std::uint64_t tick{0U};
};

class Packer final {
public:
    explicit Packer(std::vector<float>& out) : out_{out} {}

    void f(float value) { out_.push_back(value); }
    // Where the next value goes, and a value already written there changed.
    [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }
    void set(std::size_t at, float value) { out_[at] = value; }
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

// [fires this step, center.xyz, power, radius, scale_min, scale_max]. Whether
// the blast goes off is decided here, on the CPU, from the step, so the
// kernel only throws when told to.
inline void pack(const Shockwave& m, const PackContext& c, Packer& p) {
    const Blast* blast = find_blast(c.in.effect, m.blast);
    const bool fires = blast != nullptr && blast_tick(*blast, c.in.effect.simulation_rate) == c.tick;
    p.u(fires ? 1U : 0U);
    p.v3(blast != nullptr ? blast->center : Vec3{});
    p.f(blast != nullptr ? blast->power : 0.0F);
    p.f(blast != nullptr ? blast->radius : 1.0F);
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

// Where the named emitter's bodies are this step, as ajfx_cell_span and
// ajfx_next_body read it, in 13 floats: [records at, order at, starts at (all in floats of
// the fields buffer), count, usable, origin.xyz, 1 / cell, nx, ny, nz,
// bound]. A count of 0 where there are no bodies to meet, or no buffer to
// find them in (kernel generation).
inline const BodyGrid* pack_grid(const std::string& name, const PackContext& c, Packer& p) {
    const BodyGrid* grid = nullptr;
    std::uint32_t at = 0U;
    if (c.bodies != nullptr && c.fields != nullptr) {
        const auto found = c.bodies->find(name);
        const auto offset = c.fields->body_offsets.find(name);
        if (found != c.bodies->end() && offset != c.fields->body_offsets.end()) {
            grid = &found->second;
            at = offset->second;
        }
    }
    std::uint32_t capacity = 0U;
    for (const Emitter& other : c.in.effect.emitters) {
        if (other.name == name) {
            capacity = other.capacity;
            break;
        }
    }
    const std::uint32_t order_at = at + body_grid_cells + 1U;
    p.u(order_at + capacity);
    p.u(order_at);
    p.u(at);
    p.u(grid != nullptr ? static_cast<std::uint32_t>(grid->bodies.size()) : 0U);
    p.u(grid != nullptr && grid->usable ? 1U : 0U);
    p.v3(grid != nullptr ? grid->origin : Vec3{});
    p.f(grid != nullptr ? grid->inv_cell : 1.0F);
    p.u(grid != nullptr ? grid->nx : 0U);
    p.u(grid != nullptr ? grid->ny : 0U);
    p.u(grid != nullptr ? grid->nz : 0U);
    p.f(grid != nullptr ? grid->bound : 0.0F);
    return grid;
}

// [restitution, 1 - friction, kill, grid (pack_grid)]. The bodies travel in
// the fields buffer, written every step, so they are where the emitter they
// belong to has just put them.
inline void pack(const BodyCollision& m, const PackContext& c, Packer& p) {
    p.f(c.in(m.restitution));
    p.f(1.0F - c.in(m.friction));
    p.u(m.kill ? 1U : 0U);
    static_cast<void>(pack_grid(m.emitter, c, p));
}

// [every source, 8 words of one bit per spawn source, every kind, 8 words of
// one bit per body kind]: which bodies a wake follows, as
// ajfx_body_in_scope reads it and in_scope decides it.
inline void pack_body_scope(const Scope& scope, Packer& p) {
    for (const std::vector<std::uint32_t>* list : {&scope.sources, &scope.kinds}) {
        std::uint32_t words[8]{};
        for (const std::uint32_t entry : *list) {
            if (entry < 256U) {
                words[entry >> 5U] |= 1U << (entry & 31U);
            }
        }
        p.u(list->empty() ? 1U : 0U);
        for (const std::uint32_t word : words) {
            p.u(word);
        }
    }
}

// [strength * dt, radius, min_speed squared, scale_min, scale_max, the
// farthest any wake reaches (radius times the largest half size), bodies
// scope (18), grid (pack_grid)], as ajfx_wake reads them.
inline void pack(const Wake& m, const PackContext& c, Packer& p) {
    p.f(c.in(m.strength) * c.dt);
    p.f(m.radius);
    p.f(m.min_speed * m.min_speed);
    pack_scale(m, c, p);
    const std::size_t reach_at = p.size();
    p.f(0.0F);
    pack_body_scope(m.bodies, p);
    if (const BodyGrid* grid = pack_grid(m.emitter, c, p)) {
        p.set(reach_at, m.radius * grid->half);
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

// [start, 1 / half_life, glow], as ajfx_heat reads them and detail::heat_glow
// works with them.
inline void pack(const Heat& m, const PackContext&, Packer& p) {
    p.f(m.start);
    p.f(1.0F / m.half_life);
    p.f(m.glow);
}

// Floats a module's scope takes in its data: whether it is every particle,
// then one bit per spawn source (piece_spawn_source the last).
inline constexpr std::uint32_t scope_floats = 9U;

// [every particle, 8 words of one bit per spawn source], as ajfx_in_scope
// reads them. A scope's kinds never reach a kernel: only a rigid physics
// emitter's particles are bodies, and the CPU simulates those.
inline void pack_scope(const Scope& scope, Packer& p) {
    std::uint32_t words[8]{};
    for (const std::uint32_t source : scope.sources) {
        if (source < 256U) {
            words[source >> 5U] |= 1U << (source & 31U);
        }
    }
    p.u(scope.sources.empty() ? 1U : 0U);
    for (const std::uint32_t word : words) {
        p.u(word);
    }
}

// Rigid physics emitters run on the CPU; their kernels read no module data.
inline void pack(const RigidPhysics&, const PackContext&, Packer&) {}

// Breaking happens in the rigid physics world, on the CPU, so it too leaves
// nothing for a kernel to read.
inline void pack(const Break&, const PackContext&, Packer&) {}

// So does holding bodies to each other.
inline void pack(const Glue&, const PackContext&, Packer&) {}

// One module: its id, then its scope where it has one (scope_floats), then
// its data.
template <class Module>
void pack_module(const Module& module, const PackContext& context, Packer& p) {
    p.u(module.id);
    if constexpr (requires { module.only; }) {
        pack_scope(module.only, p);
    }
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

// Whether an emitter bears particles from contacts or moving bodies, whose
// newborns then start where those put them: the spawn kernel reads each
// one's start (birth_origin_floats) after the ages and spawn indices.
inline bool has_birth_origins(const Emitter& emitter) {
    return std::any_of(emitter.spawn.begin(), emitter.spawn.end(), [](const SpawnModule& module) {
        return std::holds_alternative<SpawnFromContacts>(module) || std::holds_alternative<SpawnFromBodies>(module);
    });
}

// [position.xyz, velocity.xyz, 1 where it has one] a birth.
inline constexpr std::uint32_t birth_origin_floats = 7U;

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
