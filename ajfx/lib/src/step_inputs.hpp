#pragma once

#include <ajfx/actions.hpp>
#include <ajfx/effect.hpp>
#include <ajfx/random.hpp>
#include <ajfx/rigid_solver.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// What the CPU and GPU simulators share per step: parameter values, resolved
// module inputs and the birth schedule. Both run this exact code, so the
// GPU's particles are born at the same steps, with the same ages and spawn
// indices, as the reference's.
namespace ajfx::detail {

inline std::size_t parameter_index(const Effect& effect, std::string_view name) {
    for (std::size_t i = 0U; i < effect.parameters.size(); ++i) {
        if (effect.parameters[i].name == name) {
            return i;
        }
    }
    return 0U; // unreachable for a validated effect
}

// Defaults, then the provider's values, clamped to each range.
inline void resolve_values(const Effect& effect, const ParameterProvider* provider, std::uint64_t tick,
                           std::vector<ParameterValue>& values) {
    values.resize(effect.parameters.size());
    for (std::size_t i = 0U; i < values.size(); ++i) {
        values[i] = effect.parameters[i].default_value;
    }
    if (provider != nullptr) {
        provider->values_at(tick, values);
    }
    for (std::size_t i = 0U; i < values.size(); ++i) {
        for (float& component : values[i]) {
            component = std::clamp(component, effect.parameters[i].min, effect.parameters[i].max);
        }
    }
}

// Module inputs resolved against one step's parameter values.
struct Inputs final {
    const Effect& effect;
    std::span<const ParameterValue> values;

    [[nodiscard]] const ParameterValue& bound(const std::string& name) const {
        return values[parameter_index(effect, name)];
    }
    [[nodiscard]] float operator()(const FloatInput& in) const {
        return in.bound() ? bound(in.parameter)[0] : in.value;
    }
    [[nodiscard]] Vec3 operator()(const Vec3Input& in) const {
        if (!in.bound()) {
            return in.value;
        }
        const ParameterValue& v = bound(in.parameter);
        return {v[0], v[1], v[2]};
    }
    [[nodiscard]] Color operator()(const ColorInput& in) const {
        if (!in.bound()) {
            return in.value;
        }
        const ParameterValue& v = bound(in.parameter);
        return {v[0], v[1], v[2], v[3]};
    }
};

// One birth count per spawn module, then one for the pieces broken bodies
// become (EmitterState::spawn_counts).
inline std::size_t spawn_count_slots(const Emitter& emitter) {
    return emitter.spawn.size() + 1U;
}

// The next spawn index from `source`, counting the birth.
inline std::uint32_t next_spawn_index(std::vector<std::uint64_t>& counts, std::uint32_t source) {
    const std::size_t slot = source == piece_spawn_source ? counts.size() - 1U : source;
    return spawn_index_of(source, counts[slot]++);
}

inline std::size_t rate_module_count(const Emitter& emitter) {
    return static_cast<std::size_t>(std::count_if(emitter.spawn.begin(), emitter.spawn.end(), [](const SpawnModule& m) {
        return std::holds_alternative<SpawnRate>(m);
    }));
}

// ------------------------------------------------------------ contacts and bodies

// What the effect itself reads of a world's contacts (Break,
// SpawnFromContacts): those at `slowest` or faster, and of them the hardest
// `limit` (contact_harder), in the order the world gave them
// (contact_before). The world collects at least this (its floor is no higher
// and its limit no lower, and past its limit it drops the softest), so the
// answer is the same whatever slower or further contacts a host also asked
// for, on the CPU, through a RigidRunner or on the GPU.
inline std::vector<RigidContact> effect_contacts(std::span<const RigidContact> world, float slowest,
                                                 std::uint32_t limit) {
    std::vector<RigidContact> out;
    for (const RigidContact& contact : world) {
        if (contact.speed >= slowest) {
            out.push_back(contact);
        }
    }
    if (out.size() > limit) {
        std::sort(out.begin(), out.end(), contact_harder);
        out.resize(limit);
        std::sort(out.begin(), out.end(), contact_before);
    }
    return out;
}

// The most contacts of an emitter the effect reads a step (see
// max_rigid_contacts).
inline std::uint32_t effect_contact_limit(const Emitter& emitter) {
    return std::max(emitter.capacity, max_rigid_contacts);
}

// A rigid physics emitter as the emitters bearing particles from it find it
// in a step: its bodies where this step left them, and the contacts the
// effect reads of that step (effect_contacts).
struct BodySource final {
    static constexpr std::uint32_t none_held = std::numeric_limits<std::uint32_t>::max();

    const EmitterState* bodies{nullptr};
    const RigidPhysics* physics{nullptr};
    std::vector<RigidContact> contacts;
    // Each world slot's place among `bodies`, or none_held.
    std::vector<std::uint32_t> particle_of_slot;
};

using BodySources = std::map<std::string, BodySource, std::less<>>;

inline BodySource body_source(const RigidPhysics& physics, const EmitterState& bodies,
                              std::vector<RigidContact> contacts, std::uint32_t capacity) {
    BodySource source;
    source.bodies = &bodies;
    source.physics = &physics;
    source.contacts = std::move(contacts);
    source.particle_of_slot.assign(capacity, BodySource::none_held);
    for (std::size_t i = 0U; i < bodies.alive() && i < bodies.body.size(); ++i) {
        if (bodies.body[i] < capacity) {
            source.particle_of_slot[bodies.body[i]] = static_cast<std::uint32_t>(i);
        }
    }
    return source;
}

// A particle emitter as the emitters shedding particles from it
// (SpawnFromParticles) find it in a step: its particles where this step left
// them, newborns included. By the name of the emitter.
using ParticleSources = std::map<std::string, const EmitterState*, std::less<>>;

// Where a particle born of a contact, a moving body, a moving particle or
// an action starts, before its init modules: added to the position and
// velocity they give it. A body born of an action (`exact`) takes the pose
// and motion as they are instead, whatever its init modules say, at `size`
// when that is above zero.
struct BirthOrigin final {
    Vec3 position{};
    Vec3 velocity{};
    bool exact{false};
    Quat orientation{};
    Vec3 angular_velocity{};
    float size{0.0F};
    // A body asleep until this tick (0 for one awake from birth).
    std::uint64_t held_until{0U};
};

// What births need beyond the spawn stack: the step, the emitter's seed
// (hash_mix(instance seed, emitter seed)), the bodies it bears from and the
// particles it sheds from. The GPU simulation leaves `particles` null: its
// kernels shed those births themselves (see gpu/kernels.cpp), and
// for_each_birth then skips SpawnFromParticles.
struct BirthContext final {
    std::uint64_t tick{0U};
    std::uint32_t seed{0U};
    float dt{0.0F};
    const BodySources* bodies{nullptr};
    const ParticleSources* particles{nullptr};
    // The actions the step applies (SpawnActions), the emitter's name they
    // are kept under, and how many more of its action-born particles may be
    // alive (max_alive less those alive now; a particle emitter, which keeps
    // no such count, passes max_per_step). Rigid physics emitters bear their
    // tick-0 spawns before the first step (Simulation::start), so the step
    // from 0 skips them (`skip_spawns`).
    const ActionProvider* actions{nullptr};
    std::string_view emitter;
    std::uint32_t action_room{0U};
    bool skip_spawns{false};
    // Whether the emitter has rigid physics: its action births are exact.
    bool rigid{false};
};

inline Vec3 unit_or_zero(Vec3 v) {
    const float length_squared = dot(v, v);
    return length_squared > 0.0F ? v * (1.0F / std::sqrt(length_squared)) : Vec3{};
}

// Two unit axes across `axis`, as Velocity picks them.
inline void across(Vec3 axis, Vec3& t1, Vec3& t2) {
    const Vec3 helper = std::fabs(axis.y) < 0.99F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
    t1 = unit_or_zero(cross(helper, axis));
    t2 = cross(axis, t1);
}

inline Vec3 sphere_point(float u, float v) {
    const float z = 2.0F * u - 1.0F;
    const float r = std::sqrt(std::max(0.0F, 1.0F - z * z));
    const float phi = 2.0F * std::numbers::pi_v<float> * v;
    return {r * std::cos(phi), r * std::sin(phi), z};
}

// `v` turned a random way within `spread` degrees of itself, its length
// kept.
inline Vec3 scatter(Vec3 v, float spread, float u1, float u2) {
    const float length = std::sqrt(dot(v, v));
    if (!(length > 0.0F) || !(spread > 0.0F)) {
        return v;
    }
    const Vec3 axis = v * (1.0F / length);
    Vec3 t1{};
    Vec3 t2{};
    across(axis, t1, t2);
    const float cos_spread = std::cos(spread * (std::numbers::pi_v<float> / 180.0F));
    const float cos_theta = 1.0F - u1 * (1.0F - cos_spread);
    const float sin_theta = std::sqrt(std::max(0.0F, 1.0F - cos_theta * cos_theta));
    const float phi = 2.0F * std::numbers::pi_v<float> * u2;
    return (t1 * (sin_theta * std::cos(phi)) + t2 * (sin_theta * std::sin(phi)) + axis * cos_theta) * length;
}

// Streams of the per-step counts, beside a module's per-particle draws.
inline constexpr std::uint32_t contact_count_stream = 0x8000c047U;
inline constexpr std::uint32_t body_count_stream = 0x8000b0d1U;
inline constexpr std::uint32_t particle_count_stream = 0x8000a71cU;

inline std::uint32_t step_key(std::uint64_t tick) {
    return hash_mix(static_cast<std::uint32_t>(tick), static_cast<std::uint32_t>(tick >> 32U));
}

// Whether one side of a contact is a body of `source` in `scope`.
inline bool side_in_scope(const BodySource& source, const Scope& scope, RigidSide side, std::uint32_t slot) {
    if (side != RigidSide::Body || slot >= source.particle_of_slot.size()) {
        return false;
    }
    const std::uint32_t i = source.particle_of_slot[slot];
    if (i == BodySource::none_held) {
        return false;
    }
    const EmitterState& s = *source.bodies;
    return in_scope(scope, s.spawn_index[i], i < s.body_kind.size() ? s.body_kind[i] : 0U);
}

// How many of `wanted` (a mean) a roll makes: down or up, so the mean holds.
inline std::uint32_t rounded_count(float wanted, float roll, std::uint32_t room) {
    const double count = std::floor(static_cast<double>(std::max(wanted, 0.0F)) + static_cast<double>(roll));
    return count >= static_cast<double>(room) ? room : static_cast<std::uint32_t>(count);
}

template <class Birth>
void contact_births(const SpawnFromContacts& m, std::uint32_t source, const BirthContext& context,
                    std::vector<std::uint64_t>& counts, Birth& birth) {
    if (context.bodies == nullptr) {
        return;
    }
    const auto found = context.bodies->find(m.emitter);
    if (found == context.bodies->end()) {
        return;
    }
    const BodySource& from = found->second;
    const std::uint32_t key = step_key(context.tick);
    std::uint32_t made = 0U;
    for (std::size_t k = 0U; k < from.contacts.size() && made < m.max_per_step; ++k) {
        const RigidContact& hit = from.contacts[k];
        if (hit.speed < m.min_speed || (m.new_only && !hit.began)) {
            continue;
        }
        if (!m.bodies.empty() && !side_in_scope(from, m.bodies, hit.a_is, hit.a) &&
            !side_in_scope(from, m.bodies, hit.b_is, hit.b)) {
            continue;
        }
        const float roll = particle_unit(context.seed, hash_mix(key, static_cast<std::uint32_t>(k)),
                                         hash_mix(m.id, contact_count_stream));
        const std::uint32_t count = rounded_count(m.per_hit + m.per_speed * hit.speed, roll, m.max_per_step - made);
        // Away from what was hit: the normal turned to point up, so dust
        // leaves a floor rather than burrowing into it.
        const Vec3 up = hit.normal.y < 0.0F ? hit.normal * -1.0F : hit.normal;
        Vec3 t1{};
        Vec3 t2{};
        across(up, t1, t2);
        for (std::uint32_t n = 0U; n < count; ++n) {
            const std::uint32_t index = next_spawn_index(counts, source);
            const auto u = [&](std::uint32_t draw) { return particle_unit(context.seed, index, hash_mix(m.id, draw)); };
            const float phi = 2.0F * std::numbers::pi_v<float> * u(0U);
            const Vec3 along = t1 * std::cos(phi) + t2 * std::sin(phi);
            const Vec3 thrown = up * (m.along_normal * hit.speed) + along * (m.along_surface * hit.speed);
            const BirthOrigin origin{hit.point, scatter(thrown, m.spread, u(1U), u(2U))};
            birth(0.0, 0U, source, index, &origin);
            ++made;
        }
    }
}

// How far out a body's surface is: half its size, times its kind's scale.
inline float body_radius(const RigidPhysics& physics, const EmitterState& s, std::size_t i) {
    const std::uint32_t kind = i < s.body_kind.size() ? s.body_kind[i] : 0U;
    const float scale = kind > 0U && kind <= physics.bodies.size() ? physics.bodies[kind - 1U].scale : 1.0F;
    return 0.5F * s.size[i] * scale;
}

template <class Birth>
void body_births(const SpawnFromBodies& m, std::uint32_t source, const BirthContext& context,
                 std::vector<std::uint64_t>& counts, Birth& birth) {
    if (context.bodies == nullptr) {
        return;
    }
    const auto found = context.bodies->find(m.emitter);
    if (found == context.bodies->end() || found->second.bodies == nullptr) {
        return;
    }
    const BodySource& from = found->second;
    const EmitterState& s = *from.bodies;
    const std::uint32_t key = step_key(context.tick);
    std::uint32_t made = 0U;
    for (std::size_t i = 0U; i < s.alive() && made < m.max_per_step; ++i) {
        if (!m.bodies.empty() && !in_scope(m.bodies, s.spawn_index[i], i < s.body_kind.size() ? s.body_kind[i] : 0U)) {
            continue;
        }
        const Vec3 v = s.velocity[i];
        const float speed = std::sqrt(dot(v, v));
        if (speed < m.min_speed) {
            continue;
        }
        const float roll =
            particle_unit(context.seed, hash_mix(key, s.spawn_index[i]), hash_mix(m.id, body_count_stream));
        const std::uint32_t count =
            rounded_count((m.rate + m.per_speed * speed) * context.dt, roll, m.max_per_step - made);
        const float reach = from.physics != nullptr ? body_radius(*from.physics, s, i) * m.surface : 0.0F;
        for (std::uint32_t n = 0U; n < count; ++n) {
            // Spread along the step's stretch of the path, the first born
            // the oldest, so a trail is continuous however fast the body.
            const double age = static_cast<double>(context.dt) *
                               (1.0 - (static_cast<double>(n) + 0.5) / static_cast<double>(count));
            const std::uint32_t index = next_spawn_index(counts, source);
            const auto u = [&](std::uint32_t draw) { return particle_unit(context.seed, index, hash_mix(m.id, draw)); };
            const Vec3 at = s.position[i] - v * static_cast<float>(age) + sphere_point(u(0U), u(1U)) * reach;
            const BirthOrigin origin{at, v * m.inherit};
            birth(age, 0U, source, index, &origin);
            ++made;
        }
    }
}

// How many of `wanted` (a mean) a roll makes, in single precision: the GPU
// works this out too (ajfx_shed_count in gpu/kernels.cpp), operation for
// operation, so both must stay in float.
inline std::uint32_t rounded_count_f(float wanted, float roll, std::uint32_t room) {
    const float count = std::floor(wanted + roll);
    return count >= static_cast<float>(room) ? room : static_cast<std::uint32_t>(count);
}

// The births a SpawnFromParticles sheds in one step, as the GPU kernel sheds
// them: every live particle of the source, in its order, in float
// arithmetic throughout. `made` counts the module's births this step.
template <class Birth>
void particle_births(const SpawnFromParticles& m, std::uint32_t source, const BirthContext& context,
                     std::vector<std::uint64_t>& counts, Birth& birth) {
    if (context.particles == nullptr) {
        return;
    }
    const auto found = context.particles->find(m.emitter);
    if (found == context.particles->end() || found->second == nullptr) {
        return;
    }
    const EmitterState& s = *found->second;
    const std::uint32_t key = step_key(context.tick);
    const float min_speed_squared = m.min_speed * m.min_speed;
    std::uint32_t made = 0U;
    for (std::size_t i = 0U; i < s.alive() && made < m.max_per_step; ++i) {
        if (!m.particles.empty() && !in_scope(m.particles, s.spawn_index[i], 0U)) {
            continue;
        }
        const Vec3 v = s.velocity[i];
        const float speed_squared = dot(v, v);
        if (speed_squared < min_speed_squared) {
            continue;
        }
        const float speed = std::sqrt(speed_squared);
        const float wanted = (m.rate + m.per_speed * speed) * context.dt;
        const float roll =
            particle_unit(context.seed, hash_mix(key, s.spawn_index[i]), hash_mix(m.id, particle_count_stream));
        const std::uint32_t count = rounded_count_f(wanted, roll, m.max_per_step - made);
        for (std::uint32_t n = 0U; n < count; ++n) {
            // Spread along the step's stretch of the path, the first born
            // the oldest, so a trail is continuous however fast the particle.
            const float age =
                context.dt * (1.0F - (static_cast<float>(n) + 0.5F) / static_cast<float>(count));
            const std::uint32_t index = next_spawn_index(counts, source);
            const BirthOrigin origin{s.position[i] - v * age, v * m.inherit};
            birth(static_cast<double>(age), 0U, source, index, &origin);
            ++made;
        }
    }
}

// A Spawn action as a birth origin: a body takes it exactly, a particle
// starts from it.
inline BirthOrigin action_origin(const Action& action, std::uint64_t tick, bool rigid) {
    BirthOrigin origin;
    origin.position = action.position;
    origin.velocity = action.velocity;
    origin.exact = rigid;
    origin.orientation = action.orientation;
    origin.angular_velocity = action.angular_velocity;
    origin.size = action.size;
    origin.held_until = rigid && action.release > tick ? action.release : 0U;
    return origin;
}

// The births a SpawnActions module makes in one step: every Spawn action
// stamped with the step's tick, in the provider's order, each with the
// action's serial as its count. Refused past the module's cap for the step
// and past the room the context gives, and counted so. A serial too large
// for a spawn index is refused too.
template <class Birth>
void action_births(const SpawnActions& m, std::uint32_t source, const BirthContext& context, Birth& birth) {
    if (context.actions == nullptr || context.skip_spawns) {
        return;
    }
    std::uint32_t made = 0U;
    for (const Action& action : context.actions->actions_at(context.emitter, context.tick)) {
        if (action.kind != ActionKind::Spawn) {
            continue;
        }
        const BirthOrigin origin = action_origin(action, context.tick, context.rigid);
        if (made >= m.max_per_step || made >= context.action_room || action.serial > spawn_count_mask) {
            // Refused, as a birth past capacity is: the birth callback counts
            // it without room for it.
            birth(0.0, action.body, source, spawn_index_of(source, action.serial), &origin, true);
            continue;
        }
        birth(0.0, action.body, source, spawn_index_of(source, action.serial), &origin, false);
        ++made;
    }
}

// Calls `birth(age, body_kind, source, spawn_index, origin)` for every
// particle the emitter's spawn stack produces in the step [start, end), in
// stack order, with the age each has reached by the end of the step, the
// kind of body its spawn module asked for, that module's place in the stack
// (its spawn source, see spawn_index_of), its spawn index (counted in
// `counts`, EmitterState::spawn_counts) and, for a particle born of a contact,
// a moving body or a moving particle, where it starts (null for the rest).
// `rate_carry` holds one fractional count per SpawnRate. The particles
// SpawnFromParticles sheds come last, in stack order among themselves: the
// GPU sheds those in a kernel after the births it was handed. Action
// births (SpawnActions) come with `refused` set when the module or the
// context has no room for them: they are counted, not made.
template <class Birth>
void for_each_birth(const Emitter& emitter, const Inputs& in, std::vector<double>& rate_carry,
                    std::vector<std::uint64_t>& counts, double start, double end, double steps_per_second,
                    const BirthContext& context, Birth&& born) {
    std::size_t rate_slot = 0U;
    const BirthOrigin* const none = nullptr;
    const auto birth = [&born](double age, std::uint32_t kind, std::uint32_t source, std::uint32_t spawn_index,
                               const BirthOrigin* origin, bool refused = false) {
        born(age, kind, source, spawn_index, origin, refused);
    };
    for (std::size_t slot = 0U; slot < emitter.spawn.size(); ++slot) {
        const SpawnModule& module = emitter.spawn[slot];
        const auto source = static_cast<std::uint32_t>(slot);
        if (const SpawnRate* spawn_rate = std::get_if<SpawnRate>(&module)) {
            double& carry = rate_carry[rate_slot++];
            const double births = std::max(0.0, static_cast<double>(in(spawn_rate->rate))) / steps_per_second;
            if (births <= 0.0) {
                continue;
            }
            const double total = carry + births;
            const double count = std::floor(total);
            // The m-th birth happens when the running total crosses m + 1.
            for (double m = 0.0; m < count; m += 1.0) {
                const double fraction = (m + 1.0 - carry) / births;
                birth((1.0 - fraction) / steps_per_second, spawn_rate->body, source,
                      next_spawn_index(counts, source), none);
            }
            carry = total - count;
        } else if (const SpawnBurst* burst = std::get_if<SpawnBurst>(&module)) {
            const double time = burst->time;
            const double interval = burst->interval;
            const bool repeats = interval > 0.0 && burst->cycles != 1U;
            std::uint64_t k = 0U;
            if (repeats && time < start) {
                k = static_cast<std::uint64_t>(std::ceil((start - time) / interval));
                // Correct the division's rounding in either direction.
                while (k > 0U && time + static_cast<double>(k - 1U) * interval >= start) {
                    --k;
                }
                while (time + static_cast<double>(k) * interval < start) {
                    ++k;
                }
            }
            for (;; ++k) {
                if (k > 0U && (!repeats || (burst->cycles != 0U && k >= burst->cycles))) {
                    break;
                }
                const double at = time + static_cast<double>(k) * interval;
                if (at >= end) {
                    break;
                }
                if (at >= start) {
                    for (std::uint32_t c = 0U; c < burst->count; ++c) {
                        birth(end - at, burst->body, source, next_spawn_index(counts, source), none);
                    }
                }
                if (!repeats) {
                    break;
                }
            }
        } else if (const SpawnFromContacts* contacts = std::get_if<SpawnFromContacts>(&module)) {
            contact_births(*contacts, source, context, counts, birth);
        } else if (const SpawnFromBodies* bodies = std::get_if<SpawnFromBodies>(&module)) {
            body_births(*bodies, source, context, counts, birth);
        } else if (const SpawnActions* actions = std::get_if<SpawnActions>(&module)) {
            action_births(*actions, source, context, birth);
        }
    }
    for (std::size_t slot = 0U; slot < emitter.spawn.size(); ++slot) {
        if (const SpawnFromParticles* shed = std::get_if<SpawnFromParticles>(&emitter.spawn[slot])) {
            particle_births(*shed, static_cast<std::uint32_t>(slot), context, counts, birth);
        }
    }
}

} // namespace ajfx::detail
