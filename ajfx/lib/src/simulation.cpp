#include <ajfx/simulation.hpp>

#include <ajfx/cache.hpp>
#include <ajfx/random.hpp>

#include "body_grid.hpp"
#include "curves.hpp"
#include "noise.hpp"
#include "gpu/pack.hpp"
#include "step_inputs.hpp"
#include "volume.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace ajfx {

// One grid volume's step: its constants for this step's length, the deposits
// the emitters make into it before it steps, and its scratch buffers.
struct Simulation::VolumeWork final {
    detail::VolumeStep step;
    detail::VolumeDeposits deposits;
    detail::VolumeScratch scratch;
};

namespace {

// Size of a particle with no size module, in world units.
constexpr float default_size = 0.1F;

using detail::Inputs;
using detail::normalized;

using detail::mix;
using detail::normalized_age;
using detail::sample;
using detail::times;

// A random value in [0, 1) for one particle, keyed by the module instance
// and which of its random draws this is.
struct Random final {
    std::uint32_t seed;

    [[nodiscard]] float operator()(std::uint32_t spawn_index, ModuleId module, std::uint32_t draw) const {
        return particle_unit(seed, spawn_index, hash_mix(module, draw));
    }
};

// Removes the particles `dead` selects, keeping the rest in order.
template <class Dead>
void compact(EmitterState& s, Dead dead) {
    const std::size_t count = s.alive();
    const bool bodies = s.body.size() == count && count > 0U;
    const bool kinds = s.body_kind.size() == count && count > 0U;
    const bool holds = s.held_until.size() == count && count > 0U;
    std::size_t kept = 0U;
    for (std::size_t i = 0U; i < count; ++i) {
        if (dead(i)) {
            continue;
        }
        if (kept != i) {
            s.position[kept] = s.position[i];
            s.velocity[kept] = s.velocity[i];
            s.age[kept] = s.age[i];
            s.lifetime[kept] = s.lifetime[i];
            s.base_color[kept] = s.base_color[i];
            s.color[kept] = s.color[i];
            s.base_size[kept] = s.base_size[i];
            s.size[kept] = s.size[i];
            s.rotation[kept] = s.rotation[i];
            s.orientation[kept] = s.orientation[i];
            s.angular_velocity[kept] = s.angular_velocity[i];
            s.spin[kept] = s.spin[i];
            s.spawn_index[kept] = s.spawn_index[i];
            if (bodies) {
                s.body[kept] = s.body[i];
            }
            if (kinds) {
                s.body_kind[kept] = s.body_kind[i];
            }
            if (holds) {
                s.held_until[kept] = s.held_until[i];
            }
        }
        ++kept;
    }
    if (bodies) {
        s.body.resize(kept);
    }
    if (kinds) {
        s.body_kind.resize(kept);
    }
    if (holds) {
        s.held_until.resize(kept);
    }
    s.position.resize(kept);
    s.velocity.resize(kept);
    s.age.resize(kept);
    s.lifetime.resize(kept);
    s.base_color.resize(kept);
    s.color.resize(kept);
    s.base_size.resize(kept);
    s.size.resize(kept);
    s.rotation.resize(kept);
    s.orientation.resize(kept);
    s.angular_velocity.resize(kept);
    s.spin.resize(kept);
    s.spawn_index.resize(kept);
}

// How colliders meet an emitter's particles when it has a RigidBody. Not a
// RigidContact (rigid_solver.hpp), which is what a physics engine's bodies
// were found hitting: these are the settings a collider responds by.
struct BodyResponse final {
    float contact_scale;
    float rest_speed;
    float roundness;
};

// Per-step context shared by the module implementations.
struct Step final {
    EmitterState& s;
    Inputs in;
    Random random;
    float dt;
    double start;  // seconds at the start of the step
    double end;    // seconds at the end of the step
    std::size_t first;
    std::size_t last;
    const FieldSet* fields{nullptr};
    // The bodies this emitter's BodyCollision and Wake modules meet, by the
    // emitter they name, sorted into their grids. Gathered before the step's
    // forces, so every body is where the other emitter's physics put it this
    // step.
    const detail::BodyGrids* bodies{nullptr};
    std::optional<BodyResponse> rigid{};
    // The step being taken, which a blast goes off in or not.
    std::uint64_t tick{0U};
    // The grid volumes as the step began (what VolumeAdvect reads) and
    // their work, where VolumeSource deposits; in Effect::volumes order.
    const std::vector<VolumeState>* volumes{nullptr};
    std::vector<Simulation::VolumeWork>* volume_work{nullptr};
};

// ------------------------------------------------------------ init modules

void apply(const Lifetime& m, Step& step) {
    const float lo = step.in(m.min);
    const float hi = step.in(m.max);
    for (std::size_t i = step.first; i < step.last; ++i) {
        if (detail::scoped(m.only, step.s, i)) {
            step.s.lifetime[i] = mix(lo, hi, step.random(step.s.spawn_index[i], m.id, 0U));
        }
    }
}

Vec3 unit_sphere(float u, float v) {
    const float z = 2.0F * u - 1.0F;
    const float r = std::sqrt(std::max(0.0F, 1.0F - z * z));
    const float phi = 2.0F * std::numbers::pi_v<float> * v;
    return {r * std::cos(phi), r * std::sin(phi), z};
}

void apply(const ShapeLocation& m, Step& step) {
    const Vec3 center = step.in(m.center);
    const float radius_min = step.in(m.radius_min);
    const float radius_max = step.in(m.radius_max);
    const Vec3 size = m.size;
    for (std::size_t i = step.first; i < step.last; ++i) {
        if (!detail::scoped(m.only, step.s, i)) {
            continue;
        }
        const std::uint32_t index = step.s.spawn_index[i];
        const auto u = [&](std::uint32_t draw) { return step.random(index, m.id, draw); };
        // Equal ends give exactly that radius, whatever the draw.
        const float radius = mix(radius_min, radius_max, u(5U));
        Vec3 offset{};
        switch (m.shape) {
        case Shape::Point:
            break;
        case Shape::Sphere: {
            const float r = m.surface_only ? radius : radius * std::cbrt(u(2U));
            offset = unit_sphere(u(0U), u(1U)) * r;
            break;
        }
        case Shape::Disc: {
            const float r = m.surface_only ? radius : radius * std::sqrt(u(1U));
            const float phi = 2.0F * std::numbers::pi_v<float> * u(0U);
            offset = {r * std::cos(phi), 0.0F, r * std::sin(phi)};
            break;
        }
        case Shape::Box: {
            offset = {(u(0U) - 0.5F) * size.x, (u(1U) - 0.5F) * size.y, (u(2U) - 0.5F) * size.z};
            if (m.surface_only) {
                // Pick a face pair in proportion to its area, then push the
                // point onto one face of the pair.
                const float yz = size.y * size.z;
                const float xz = size.x * size.z;
                const float xy = size.x * size.y;
                const float pick = u(3U) * (yz + xz + xy);
                const float side = u(4U) < 0.5F ? -0.5F : 0.5F;
                if (pick < yz) {
                    offset.x = side * size.x;
                } else if (pick < yz + xz) {
                    offset.y = side * size.y;
                } else {
                    offset.z = side * size.z;
                }
            }
            break;
        }
        }
        step.s.position[i] = center + offset;
    }
}

void apply(const Velocity& m, Step& step) {
    Vec3 axis = normalized(step.in(m.direction));
    if (dot(axis, axis) == 0.0F) {
        axis = {0.0F, 1.0F, 0.0F};
    }
    const Vec3 helper = std::fabs(axis.y) < 0.99F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
    const Vec3 t1 = normalized(cross(helper, axis));
    const Vec3 t2 = cross(axis, t1);
    const float cos_spread = std::cos(step.in(m.spread) * (std::numbers::pi_v<float> / 180.0F));
    const float cos_inner = std::cos(step.in(m.spread_min) * (std::numbers::pi_v<float> / 180.0F));
    const float lo = step.in(m.speed_min);
    const float hi = step.in(m.speed_max);
    for (std::size_t i = step.first; i < step.last; ++i) {
        if (!detail::scoped(m.only, step.s, i)) {
            continue;
        }
        const std::uint32_t index = step.s.spawn_index[i];
        const float cos_theta = cos_inner - step.random(index, m.id, 0U) * (cos_inner - cos_spread);
        const float sin_theta = std::sqrt(std::max(0.0F, 1.0F - cos_theta * cos_theta));
        const float phi = 2.0F * std::numbers::pi_v<float> * step.random(index, m.id, 1U);
        const Vec3 direction =
            t1 * (sin_theta * std::cos(phi)) + t2 * (sin_theta * std::sin(phi)) + axis * cos_theta;
        step.s.velocity[i] = direction * mix(lo, hi, step.random(index, m.id, 2U));
    }
}

void apply(const InitColor& m, Step& step) {
    const Color a = step.in(m.a);
    const Color b = step.in(m.b);
    const float lo = step.in(m.brightness_min);
    const float hi = step.in(m.brightness_max);
    for (std::size_t i = step.first; i < step.last; ++i) {
        if (!detail::scoped(m.only, step.s, i)) {
            continue;
        }
        const std::uint32_t index = step.s.spawn_index[i];
        const float t = step.random(index, m.id, 0U);
        const float k = mix(lo, hi, step.random(index, m.id, 1U));
        step.s.base_color[i] = {mix(a.r, b.r, t) * k, mix(a.g, b.g, t) * k, mix(a.b, b.b, t) * k, mix(a.a, b.a, t)};
    }
}

void apply(const InitSize& m, Step& step) {
    const float lo = step.in(m.min);
    const float hi = step.in(m.max);
    for (std::size_t i = step.first; i < step.last; ++i) {
        if (detail::scoped(m.only, step.s, i)) {
            step.s.base_size[i] = mix(lo, hi, step.random(step.s.spawn_index[i], m.id, 0U));
        }
    }
}

void apply(const InitRotation& m, Step& step) {
    const float lo = step.in(m.min);
    const float hi = step.in(m.max);
    constexpr float radians = std::numbers::pi_v<float> / 180.0F;
    for (std::size_t i = step.first; i < step.last; ++i) {
        if (detail::scoped(m.only, step.s, i)) {
            step.s.rotation[i] = mix(lo, hi, step.random(step.s.spawn_index[i], m.id, 0U)) * radians;
        }
    }
}

void apply(const InitTumble& m, Step& step) {
    const float lo = step.in(m.speed_min);
    const float hi = step.in(m.speed_max);
    constexpr float radians = std::numbers::pi_v<float> / 180.0F;
    constexpr float two_pi = 2.0F * std::numbers::pi_v<float>;
    for (std::size_t i = step.first; i < step.last; ++i) {
        if (!detail::scoped(m.only, step.s, i)) {
            continue;
        }
        const std::uint32_t index = step.s.spawn_index[i];
        const auto u = [&](std::uint32_t draw) { return step.random(index, m.id, draw); };
        if (m.random_orientation) {
            // Shoemake's uniform rotation from three draws.
            const float u1 = u(0U);
            const float a = std::sqrt(1.0F - u1);
            const float b = std::sqrt(u1);
            const float t2 = two_pi * u(1U);
            const float t3 = two_pi * u(2U);
            step.s.orientation[i] = Quat{a * std::sin(t2), a * std::cos(t2), b * std::sin(t3), b * std::cos(t3)};
        }
        step.s.angular_velocity[i] = unit_sphere(u(3U), u(4U)) * (mix(lo, hi, u(5U)) * radians);
    }
}

void apply(const InitSpin& m, Step& step) {
    const float lo = step.in(m.speed_min);
    const float hi = step.in(m.speed_max);
    constexpr float radians = std::numbers::pi_v<float> / 180.0F;
    for (std::size_t i = step.first; i < step.last; ++i) {
        const std::uint32_t index = step.s.spawn_index[i];
        const float speed = mix(lo, hi, step.random(index, m.id, 0U)) * radians;
        step.s.spin[i] = m.both_ways && step.random(index, m.id, 1U) < 0.5F ? -speed : speed;
    }
}

// Turns sprites by their spin where position is integrated.
void turn(const InitSpin& m, Step& step) {
    const float keep = 1.0F / (1.0F + step.in(m.drag) * step.dt);
    for (std::size_t i = step.first; i < step.last; ++i) {
        step.s.spin[i] = step.s.spin[i] * keep;
        step.s.rotation[i] += step.s.spin[i] * step.dt;
    }
}

// ------------------------------------------------------------ forces

// A force module's factor for particle i (see the forces in modules.hpp).
template <class Force>
float force_scale(const Force& m, const Step& step, std::size_t i) {
    return mix(step.in(m.scale_min), step.in(m.scale_max), step.random(step.s.spawn_index[i], m.id, 0U));
}

void apply(const Gravity& m, Step& step) {
    const Vec3 dv = step.in(m.acceleration) * step.dt;
    for (std::size_t i = step.first; i < step.last; ++i) {
        step.s.velocity[i] += dv * (sample(m.over_life, normalized_age(step.s, i)) * force_scale(m, step, i));
    }
}

void apply(const Drag& m, Step& step) {
    const float rate = step.in(m.coefficient) * step.dt;
    const float keep = 1.0F / (1.0F + rate);
    // Without a spread every particle keeps the same share, worked out once.
    const bool varied = !(step.in(m.scale_min) == 1.0F && step.in(m.scale_max) == 1.0F);
    for (std::size_t i = step.first; i < step.last; ++i) {
        step.s.velocity[i] = step.s.velocity[i] * (varied ? 1.0F / (1.0F + rate * force_scale(m, step, i)) : keep);
    }
}

void apply(const CurlNoise& m, Step& step) {
    const float frequency = step.in(m.frequency);
    const float scale = step.in(m.strength) * step.dt;
    const Vec3 scroll{0.0F, 0.0F, static_cast<float>(step.start) * step.in(m.evolution)};
    const Vec3 travelled = step.in(m.drift) * static_cast<float>(step.start);
    for (std::size_t i = step.first; i < step.last; ++i) {
        const Vec3 q = (step.s.position[i] - travelled) * frequency + scroll;
        step.s.velocity[i] += detail::curl_noise(q, m.seed) *
                              (scale * sample(m.over_life, normalized_age(step.s, i)) * force_scale(m, step, i));
    }
}

void apply(const Vortex& m, Step& step) {
    const Vec3 axis = normalized(step.in(m.axis));
    const Vec3 center = step.in(m.center);
    const float scale = step.in(m.strength) * step.dt;
    for (std::size_t i = step.first; i < step.last; ++i) {
        const Vec3 r = step.s.position[i] - center;
        const Vec3 radial = r - axis * dot(r, axis);
        const float length = std::sqrt(dot(radial, radial));
        if (length > 1.0e-6F) {
            step.s.velocity[i] += cross(axis, radial) * (scale * force_scale(m, step, i) / length);
        }
    }
}

void apply(const Attractor& m, Step& step) {
    const Vec3 target = step.in(m.position);
    const float strength = step.in(m.strength);
    const float radius = step.in(m.radius);
    for (std::size_t i = step.first; i < step.last; ++i) {
        const Vec3 d = target - step.s.position[i];
        const float distance = std::sqrt(dot(d, d));
        if (distance <= 1.0e-6F) {
            continue;
        }
        const float falloff = radius > 0.0F ? std::max(0.0F, 1.0F - distance / radius) : 1.0F;
        step.s.velocity[i] += d * (strength * force_scale(m, step, i) * falloff * step.dt / distance);
    }
}

void apply(const Converge& m, Step& step) {
    const Vec3 axis = normalized(step.in(m.axis));
    const Vec3 center = step.in(m.center);
    const float scale = step.in(m.strength) * step.dt;
    const float height = step.in(m.height);
    for (std::size_t i = step.first; i < step.last; ++i) {
        const Vec3 r = step.s.position[i] - center;
        const float along = dot(r, axis);
        const Vec3 radial = r - axis * along;
        const float falloff = height > 0.0F ? std::clamp(1.0F - along / height, 0.0F, 1.0F) : 1.0F;
        step.s.velocity[i] -= radial * (scale * force_scale(m, step, i) * falloff);
    }
}

// Throws particles out from the blast in the step it goes off (Shockwave),
// as ajfx_shockwave does on the GPU.
void apply(const Shockwave& m, Step& step) {
    const Blast* blast = find_blast(step.in.effect, m.blast);
    if (blast == nullptr || step.tick != blast_tick(*blast, step.in.effect.simulation_rate)) {
        return;
    }
    for (std::size_t i = step.first; i < step.last; ++i) {
        const BlastPush push = blast_push(*blast, step.s.position[i]);
        // Beyond the radius nothing is touched, not even by a zero.
        if (push.direction == Vec3{}) {
            continue;
        }
        step.s.velocity[i] += push.direction * (push.speed * force_scale(m, step, i));
    }
}

// Carries particles with the grid's velocity (VolumeAdvect), as
// ajfx_volume_advect does on the GPU: each takes at most all of the way to
// the velocity the grid holds at it.
void apply(const VolumeAdvect& m, Step& step) {
    const std::optional<std::size_t> index = volume_index(step.in.effect, m.volume);
    if (!index || step.volumes == nullptr || step.volume_work == nullptr) {
        return;
    }
    const detail::VolumeStep& grid = (*step.volume_work)[*index].step;
    const VolumeState& state = (*step.volumes)[*index];
    const float rate = step.in(m.strength) * step.dt;
    for (std::size_t i = step.first; i < step.last; ++i) {
        const std::optional<Vec3> carried = detail::sample_volume_velocity(grid, state, step.s.position[i]);
        if (!carried) {
            continue;
        }
        const float t = std::min(rate * force_scale(m, step, i), 1.0F);
        const Vec3 v = step.s.velocity[i];
        step.s.velocity[i] = v + (*carried - v) * t;
    }
}

// ------------------------------------------------------------ constraints and appearance

// Deposits into the grid's cells (VolumeSource), as ajfx_volume_source does
// on the GPU: each particle in the grid adds its amounts, quantised, to the
// sums of the cell it is in.
void apply(const VolumeSource& m, Step& step) {
    const std::optional<std::size_t> index = volume_index(step.in.effect, m.volume);
    if (!index || step.volume_work == nullptr) {
        return;
    }
    Simulation::VolumeWork& work = (*step.volume_work)[*index];
    const float density = step.in(m.density) * step.dt;
    const float temperature = step.in(m.temperature) * step.dt;
    const float velocity = step.in(m.velocity) * step.dt;
    for (std::size_t i = step.first; i < step.last; ++i) {
        const std::optional<std::uint32_t> cell = detail::volume_cell(work.step, step.s.position[i]);
        if (!cell) {
            continue;
        }
        const float over_life = sample(m.over_life, normalized_age(step.s, i));
        const Vec3 v = step.s.velocity[i];
        detail::deposit(work.deposits, *cell, density * over_life, temperature * over_life,
                        Vec3{v.x * (velocity * over_life), v.y * (velocity * over_life), v.z * (velocity * over_life)});
    }
}

// A rigid particle meets a collider as a ball (see RigidBody).
void respond_rigid(Step& step, std::size_t i, float distance, Vec3 n, float restitution, float slide, bool kill) {
    EmitterState& s = step.s;
    const float round = step.rigid->roundness;
    const float radius = s.size[i] * step.rigid->contact_scale;
    const float gap = distance - radius;
    if (gap >= 0.0F) {
        return;
    }
    if (kill) {
        s.age[i] = std::numeric_limits<float>::infinity();
        return;
    }
    s.position[i] -= n * gap;
    const float into = dot(s.velocity[i], n);
    if (!(into < 0.0F)) {
        return;
    }
    const float bounce = -into < step.rigid->rest_speed ? 0.0F : restitution;
    const float push = -into * (1.0F + bounce);
    s.velocity[i] += n * push;
    // A piece that is not round cannot keep rolling or twisting on the
    // collider.
    s.angular_velocity[i] = s.angular_velocity[i] * round;
    // How fast the ball's surface slips where it touches.
    const Vec3 arm = n * -radius;
    const Vec3 contact = s.velocity[i] + cross(s.angular_velocity[i], arm);
    const Vec3 slip = contact - n * dot(contact, n);
    // Stopping the slip takes `share` of it off the centre's velocity and
    // turns the ball by the rest: 2/7 and 5/7 for a solid ball, all of it
    // off the velocity for a flat chip. Friction takes at most
    // friction / (1 - friction) of the push, so full friction grips however
    // lightly the piece touches.
    const float turn = 2.5F * round;
    float share = 1.0F / (1.0F + turn);
    const float speed = std::sqrt(dot(slip, slip));
    const float grip = push * (1.0F - slide);
    if (share * speed * slide > grip) {
        share = grip / (speed * slide);
    }
    s.velocity[i] -= slip * share;
    if (radius > 1.0e-6F) {
        s.angular_velocity[i] -= cross(arm, slip) * (share * turn / (radius * radius));
    }
}

// Every collider's response, given particle i's signed distance (negative
// where it is not allowed to be) and the unit direction back to where it is.
void respond(Step& step, std::size_t i, float distance, Vec3 n, float restitution, float slide, bool kill) {
    if (step.rigid) {
        respond_rigid(step, i, distance, n, restitution, slide, kill);
        return;
    }
    if (distance >= 0.0F) {
        return;
    }
    if (kill) {
        // Removed by the compaction that follows the update stack.
        step.s.age[i] = std::numeric_limits<float>::infinity();
        return;
    }
    step.s.position[i] -= n * distance;
    const float into = dot(step.s.velocity[i], n);
    if (into < 0.0F) {
        const Vec3 normal = n * into;
        step.s.velocity[i] = (step.s.velocity[i] - normal) * slide - normal * restitution;
    }
}

void apply(const PlaneCollision& m, Step& step) {
    const Vec3 n = normalized(m.normal);
    const float restitution = step.in(m.restitution);
    const float slide = 1.0F - step.in(m.friction);
    for (std::size_t i = step.first; i < step.last; ++i) {
        respond(step, i, dot(n, step.s.position[i]) - m.offset, n, restitution, slide, m.kill);
    }
}

void apply(const SphereCollision& m, Step& step) {
    const Vec3 center = step.in(m.center);
    const float radius = step.in(m.radius);
    const float restitution = step.in(m.restitution);
    const float slide = 1.0F - step.in(m.friction);
    for (std::size_t i = step.first; i < step.last; ++i) {
        const Vec3 d = step.s.position[i] - center;
        const float length = std::sqrt(dot(d, d));
        // At the very centre, out is up.
        const Vec3 out = length > 1.0e-6F ? d * (1.0F / length) : Vec3{0.0F, 1.0F, 0.0F};
        if (m.inside) {
            respond(step, i, radius - length, out * -1.0F, restitution, slide, m.kill);
        } else {
            respond(step, i, length - radius, out, restitution, slide, m.kill);
        }
    }
}

// A particle kept out of one body: the ball or box `body` describes.
void meet_body(const BodyCollider& body, Step& step, std::size_t i, float restitution, float slide, bool kill) {
    const Vec3 d = step.s.position[i] - body.position;
    if (body.box) {
        // In the body's own frame, where the box is axis aligned.
        const Quat back{-body.orientation.x, -body.orientation.y, -body.orientation.z, body.orientation.w};
        const Vec3 local = rotate(back, d);
        const Vec3 over{std::fabs(local.x) - body.half, std::fabs(local.y) - body.half,
                        std::fabs(local.z) - body.half};
        if (over.x >= 0.0F || over.y >= 0.0F || over.z >= 0.0F) {
            // Outside: the nearest point on the box is the way out.
            const Vec3 clamped{std::clamp(local.x, -body.half, body.half), std::clamp(local.y, -body.half, body.half),
                               std::clamp(local.z, -body.half, body.half)};
            const Vec3 away = local - clamped;
            const float length = std::sqrt(dot(away, away));
            if (length > 1.0e-6F) {
                respond(step, i, length, rotate(body.orientation, away * (1.0F / length)), restitution, slide, kill);
            }
            return;
        }
        // Inside: out through the face it is nearest, x before y before z on
        // a tie, as BoxCollision does.
        Vec3 out{over.x >= over.y && over.x >= over.z ? (local.x < 0.0F ? -1.0F : 1.0F) : 0.0F, 0.0F, 0.0F};
        float distance = over.x;
        if (over.y > over.x && over.y >= over.z) {
            out = Vec3{0.0F, local.y < 0.0F ? -1.0F : 1.0F, 0.0F};
            distance = over.y;
        } else if (over.z > over.x && over.z > over.y) {
            out = Vec3{0.0F, 0.0F, local.z < 0.0F ? -1.0F : 1.0F};
            distance = over.z;
        }
        respond(step, i, distance, rotate(body.orientation, out), restitution, slide, kill);
        return;
    }
    const float length = std::sqrt(dot(d, d));
    const Vec3 out = length > 1.0e-6F ? d * (1.0F / length) : Vec3{0.0F, 1.0F, 0.0F};
    respond(step, i, length - body.half, out, restitution, slide, kill);
}

// A particle kept out of each body in turn, in body order, the nearest
// surface winning where two overlap, because each response pushes it out of
// the one it is in and the next sees where it ended up. The grid finds the
// bodies it may be touching (detail::meet_bodies); a body it is not touching
// does nothing, so that is meeting every body in turn, to the bit.
void apply(const BodyCollision& m, Step& step) {
    if (step.bodies == nullptr) {
        return;
    }
    const auto found = step.bodies->find(m.emitter);
    if (found == step.bodies->end() || found->second.bodies.empty()) {
        return;
    }
    const detail::BodyGrid& grid = found->second;
    const float restitution = step.in(m.restitution);
    const float slide = 1.0F - step.in(m.friction);
    for (std::size_t i = step.first; i < step.last; ++i) {
        // A rigid particle reaches out by its contact radius, as
        // respond_rigid works it out.
        const float extra = step.rigid ? step.s.size[i] * step.rigid->contact_scale : 0.0F;
        detail::meet_bodies(
            grid, [&step, i] { return step.s.position[i]; }, extra,
            [&](const BodyCollider& body) { meet_body(body, step, i, restitution, slide, m.kill); });
    }
}

// Pulls particles towards the velocity of the strongest moving body's wake
// at them (Wake), as ajfx_wake does on the GPU.
void apply(const Wake& m, Step& step) {
    if (step.bodies == nullptr) {
        return;
    }
    const auto found = step.bodies->find(m.emitter);
    if (found == step.bodies->end() || found->second.bodies.empty()) {
        return;
    }
    const detail::BodyGrid& grid = found->second;
    const float rate = step.in(m.strength) * step.dt;
    for (std::size_t i = step.first; i < step.last; ++i) {
        const detail::WakePick pick = detail::strongest_wake(grid, m, step.s.position[i]);
        if (pick.body == detail::no_body) {
            continue;
        }
        const float k = rate * force_scale(m, step, i) * pick.weight;
        const float t = k / (1.0F + k);
        const Vec3 v = step.s.velocity[i];
        step.s.velocity[i] = v + (grid.bodies[pick.body].velocity - v) * t;
    }
}

void apply(const SdfCollision& m, Step& step) {
    const DistanceField& field = step.fields->find(m.field)->second;
    const Vec3 offset = step.in(m.offset);
    const float restitution = step.in(m.restitution);
    const float slide = 1.0F - step.in(m.friction);
    for (std::size_t i = step.first; i < step.last; ++i) {
        const std::optional<FieldSample> sample = sample_field(field, step.s.position[i] - offset);
        if (!sample) {
            continue;
        }
        const Vec3 g = sample->gradient;
        const float length = std::sqrt(dot(g, g));
        const Vec3 out = length > 1.0e-6F ? g * (1.0F / length) : Vec3{0.0F, 1.0F, 0.0F};
        if (m.inside) {
            respond(step, i, -sample->distance, out * -1.0F, restitution, slide, m.kill);
        } else {
            respond(step, i, sample->distance, out, restitution, slide, m.kill);
        }
    }
}

void apply(const BoxCollision& m, Step& step) {
    const Vec3 center = step.in(m.center);
    const Vec3 h = m.half_size;
    const float restitution = step.in(m.restitution);
    const float slide = 1.0F - step.in(m.friction);
    // Worked in the box's own frame: into it by the inverse turn, the way
    // out back by the turn (an unturned box turns nothing, to the bit).
    const Quat turn = prop_orientation(m.turn);
    const Quat back{-turn.x, -turn.y, -turn.z, turn.w};
    for (std::size_t i = step.first; i < step.last; ++i) {
        const Vec3 d = rotate(back, step.s.position[i] - center);
        const Vec3 sign{d.x < 0.0F ? -1.0F : 1.0F, d.y < 0.0F ? -1.0F : 1.0F, d.z < 0.0F ? -1.0F : 1.0F};
        // How far past each face pair: positive outside it.
        const Vec3 q{std::fabs(d.x) - h.x, std::fabs(d.y) - h.y, std::fabs(d.z) - h.z};
        const Vec3 over{std::max(q.x, 0.0F), std::max(q.y, 0.0F), std::max(q.z, 0.0F)};
        const float outside = std::sqrt(dot(over, over));
        // The signed distance from the box and the way out of it: from
        // outside, from the nearest point on the box; from inside, through
        // the nearest face.
        Vec3 n{};
        float distance = outside;
        if (outside > 0.0F) {
            n = Vec3{sign.x * over.x / outside, sign.y * over.y / outside, sign.z * over.z / outside};
        } else {
            n = Vec3{sign.x, 0.0F, 0.0F};
            distance = q.x;
            if (q.y > distance) {
                n = Vec3{0.0F, sign.y, 0.0F};
                distance = q.y;
            }
            if (q.z > distance) {
                n = Vec3{0.0F, 0.0F, sign.z};
                distance = q.z;
            }
        }
        n = rotate(turn, n);
        if (m.inside) {
            respond(step, i, -distance, n * -1.0F, restitution, slide, m.kill);
        } else {
            respond(step, i, distance, n, restitution, slide, m.kill);
        }
    }
}

void apply(const RigidBody& m, Step& step) {
    const float keep = 1.0F / (1.0F + step.in(m.angular_drag) * step.dt);
    const float half = 0.5F * step.dt;
    for (std::size_t i = step.first; i < step.last; ++i) {
        const Vec3 w = step.s.angular_velocity[i] * keep;
        step.s.angular_velocity[i] = w;
        // q += dt/2 (w, 0) q, then back to unit length.
        const Quat q = step.s.orientation[i];
        const float x = q.x + half * (w.x * q.w + w.y * q.z - w.z * q.y);
        const float y = q.y + half * (w.y * q.w + w.z * q.x - w.x * q.z);
        const float z = q.z + half * (w.z * q.w + w.x * q.y - w.y * q.x);
        const float qw = q.w - half * (w.x * q.x + w.y * q.y + w.z * q.z);
        const float inverse = 1.0F / std::sqrt(x * x + y * y + z * z + qw * qw);
        step.s.orientation[i] = Quat{x * inverse, y * inverse, z * inverse, qw * inverse};
    }
}

void apply(const ColorOverLife& m, Step& step) {
    detail::color_over_life(m, step.s, step.first, step.last);
}

void apply(const SizeOverLife& m, Step& step) {
    detail::size_over_life(m, step.s, step.first, step.last);
}

// The engine moves rigid physics particles (see move_rigid).
void apply(const RigidPhysics&, Step&) {}

// A body comes apart after the world has stepped (see break_bodies).
void apply(const Break&, Step&) {}

// Joins are made as bodies are born and broken by the world (see glue_bodies).
void apply(const Glue&, Step&) {}

// A glow is worked out afresh with the colour, after every colour over life
// (see detail::glow).
void apply(const Heat&, Step&) {}

// Where a prop stands at `seconds`: a driven prop moves from its pose to the
// one it is given, over its window; every other prop stands where it was put.
void prop_pose_at(const RigidProp& prop, float seconds, Vec3& position, Quat& orientation) {
    position = prop.position;
    orientation = prop_orientation(prop.turn);
    if (prop.motion != PropMotion::Driven) {
        return;
    }
    float t = 1.0F;
    if (prop.move_over > 0.0F) {
        t = std::clamp((seconds - prop.move_from) / prop.move_over, 0.0F, 1.0F);
    } else if (seconds < prop.move_from) {
        t = 0.0F;
    }
    // Smooth at both ends, so a tip-over starts and stops without a jolt.
    const float e = t * t * (3.0F - 2.0F * t);
    position = prop.position + (prop.to_position - prop.position) * e;
    orientation = prop_orientation(prop.turn + (prop.to_turn - prop.turn) * e);
}

// The joins the world holds, in its order, which is the order they must be
// made again in to rebuild it.
void take_joins(EmitterState& s, const RigidWorld& world) {
    const std::span<const RigidJoin> held = world.joins();
    s.joins.assign(held.begin(), held.end());
}

// A rigid physics emitter's motion for one step, between retiring the dead
// and giving birth: the dead leave the world oldest first, each driven prop
// is told where to stand at the end of the step, the world steps under the
// step's gravity, and every particle and prop takes its body's pose; then
// colour over life, as for any emitter.
void move_rigid(Step& step, const Emitter& emitter, const RigidPhysics& physics, RigidWorld& world) {
    EmitterState& s = step.s;
    Vec3 gravity{};
    for (const UpdateModule& module : emitter.update) {
        if (const auto* g = std::get_if<Gravity>(&module)) {
            gravity += step.in(g->acceleration);
        }
    }
    // Where the move has reached by the end of the step, so the prop shoves
    // what is in its way over the step rather than jumping through it.
    for (std::size_t i = 0U; i < physics.props.size(); ++i) {
        if (physics.props[i].motion != PropMotion::Driven) {
            continue;
        }
        Vec3 position{};
        Quat orientation{};
        prop_pose_at(physics.props[i], static_cast<float>(step.end), position, orientation);
        world.set_prop_pose(i, position, orientation);
    }
    world.step(gravity);
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        const RigidBodyState body = world.body(s.body[i]);
        s.position[i] = body.position;
        s.orientation[i] = body.orientation;
        s.velocity[i] = body.velocity;
        s.angular_velocity[i] = body.angular_velocity;
    }
    s.props.resize(physics.props.size());
    for (std::size_t i = 0U; i < physics.props.size(); ++i) {
        s.props[i] = world.prop(i);
    }
    // What the step pulled apart is no longer held, and what is left is in
    // the order the world now keeps it.
    take_joins(s, world);
    for (const UpdateModule& module : emitter.update) {
        if (const auto* color = std::get_if<ColorOverLife>(&module)) {
            apply(*color, step);
        }
    }
    detail::glow(emitter, s, step.first, step.last);
}

// Bodies for the newborns [first, alive), in birth order, each in the lowest
// slot no older particle holds; asleep if `asleep`, or if an action put the
// body down to wait (held_until).
void add_bodies(EmitterState& s, std::size_t first, std::uint32_t capacity, RigidWorld& world, bool asleep = false) {
    std::vector<bool> used(capacity, false);
    for (std::size_t i = 0U; i < first; ++i) {
        used[s.body[i]] = true;
    }
    s.body.resize(s.alive());
    s.body_kind.resize(s.alive());
    s.held_until.resize(s.alive(), 0U);
    std::uint32_t next = 0U;
    for (std::size_t i = first; i < s.alive(); ++i) {
        while (used[next]) {
            ++next;
        }
        used[next] = true;
        s.body[i] = next;
        world.add(next, s.body_kind[i], s.size[i],
                  RigidBodyState{s.position[i], s.orientation[i], s.velocity[i], s.angular_velocity[i]},
                  asleep || s.held_until[i] != 0U);
    }
}

// The particle of `s` whose spawn index is `target`, if one is alive.
std::optional<std::size_t> find_target(const EmitterState& s, std::uint32_t target) {
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        if (s.spawn_index[i] == target) {
            return i;
        }
    }
    return std::nullopt;
}

// `v` no longer than `limit`.
Vec3 clamp_length(Vec3 v, float limit) {
    const float length_squared = dot(v, v);
    if (limit <= 0.0F || length_squared <= limit * limit) {
        return v;
    }
    return v * (limit / std::sqrt(length_squared));
}

// A Steer action on body `i`, before the world steps: its velocity is
// changed towards the one that reaches the target in 1 / gain seconds, and
// its spin towards the one that turns it there in the same time, neither
// faster than max_speed. The world keeps the body, so what it meets stops
// it as it would stop anything.
void steer_body(const Action& action, const EmitterState& s, std::size_t i, RigidWorld& world) {
    const Vec3 wanted = clamp_length((action.position - s.position[i]) * action.gain, action.max_speed);
    const Vec3 change = wanted - s.velocity[i];
    const Quat c = s.orientation[i];
    Quat d = times(action.orientation, Quat{-c.x, -c.y, -c.z, c.w});
    if (d.w < 0.0F) {
        d = Quat{-d.x, -d.y, -d.z, -d.w};
    }
    const float sin_half = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const float angle = 2.0F * std::atan2(sin_half, d.w);
    const Vec3 axis = sin_half > 1.0e-6F ? Vec3{d.x, d.y, d.z} * (1.0F / sin_half) : Vec3{};
    const Vec3 spin = clamp_length(axis * (angle * action.gain), action.max_speed);
    world.push(RigidSide::Body, s.body[i], change, spin - s.angular_velocity[i]);
}

// The Push and Steer actions of the step on a rigid physics emitter's
// bodies, in the provider's order, before the world steps so the step
// carries them.
void push_bodies(std::span<const Action> actions, const EmitterState& s, RigidWorld& world) {
    for (const Action& action : actions) {
        if (action.kind != ActionKind::Push && action.kind != ActionKind::Steer) {
            continue;
        }
        const std::optional<std::size_t> i = find_target(s, action.target);
        if (!i) {
            continue;
        }
        if (action.kind == ActionKind::Push) {
            world.push(RigidSide::Body, s.body[*i], action.push, action.twist);
        } else {
            // A hand keeps what it holds awake: a body held still in the air
            // would otherwise be put to sleep by the engine and hang there
            // when let go.
            const std::uint32_t slot = s.body[*i];
            world.wake(std::span<const std::uint32_t>{&slot, 1U});
            steer_body(action, s, *i, world);
        }
    }
}

// Remove actions end a particle's life where it stands: it is retired as
// one whose life ran out, in this step.
void remove_targets(std::span<const Action> actions, EmitterState& s) {
    for (const Action& action : actions) {
        if (action.kind != ActionKind::Remove) {
            continue;
        }
        if (const std::optional<std::size_t> i = find_target(s, action.target)) {
            s.lifetime[*i] = s.age[*i];
        }
    }
}

// The action-born particles of `s` alive now (those of source `source`).
std::uint32_t action_born(const EmitterState& s, std::uint32_t source) {
    std::uint32_t count = 0U;
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        if (spawn_source(s.spawn_index[i]) == source) {
            ++count;
        }
    }
    return count;
}

// The SpawnActions module of the emitter and its place in the spawn stack,
// if it has one.
std::optional<std::pair<const SpawnActions*, std::uint32_t>> action_module(const Emitter& emitter) {
    for (std::size_t slot = 0U; slot < emitter.spawn.size(); ++slot) {
        if (const auto* actions = std::get_if<SpawnActions>(&emitter.spawn[slot])) {
            return std::pair{actions, static_cast<std::uint32_t>(slot)};
        }
    }
    return std::nullopt;
}

// Where a newborn born of an origin starts: a body born of an action takes
// the pose and motion exactly, at the action's size when it gives one;
// anything else starts from the origin, offset and thrown by what its init
// modules gave it.
void take_origin(EmitterState& s, std::size_t i, const detail::BirthOrigin& origin) {
    if (origin.exact) {
        s.position[i] = origin.position;
        s.velocity[i] = origin.velocity;
        s.orientation[i] = origin.orientation;
        s.angular_velocity[i] = origin.angular_velocity;
        if (origin.size > 0.0F) {
            s.base_size[i] = origin.size;
            s.size[i] = origin.size;
        }
    } else {
        s.position[i] = origin.position + s.position[i];
        s.velocity[i] = s.velocity[i] + origin.velocity;
    }
}

const Glue* glue_module(const Emitter& emitter) {
    for (const UpdateModule& module : emitter.update) {
        if (const auto* glue = std::get_if<Glue>(&module)) {
            return glue;
        }
    }
    return nullptr;
}

// Joins between the bodies born in [first, alive) whose surfaces are within
// `range` of each other, in birth order, lowest slot first in each pair.
// Only the newborns are considered: a wall is built at once, and what lands
// on it afterwards is not part of it.
void glue_bodies(EmitterState& s, std::size_t first, const Glue& glue, RigidWorld& world) {
    for (std::size_t i = first; i < s.alive(); ++i) {
        for (std::size_t j = i + 1U; j < s.alive(); ++j) {
            if (s.joins.size() >= max_rigid_joins) {
                return;
            }
            const Vec3 d = s.position[j] - s.position[i];
            // Their surfaces, taking each as the ball its size describes:
            // close enough to touch, plus the range the join reaches.
            const float gap = std::sqrt(dot(d, d)) - 0.5F * (s.size[i] + s.size[j]);
            if (gap > glue.range) {
                continue;
            }
            const bool first_is_a = s.body[i] < s.body[j];
            const std::size_t lower = first_is_a ? i : j;
            const std::size_t upper = first_is_a ? j : i;
            const RigidJoin join = rest_of(
                s.body[lower], s.body[upper],
                RigidBodyState{s.position[lower], s.orientation[lower], s.velocity[lower],
                               s.angular_velocity[lower]},
                RigidBodyState{s.position[upper], s.orientation[upper], s.velocity[upper],
                               s.angular_velocity[upper]});
            world.glue(join, glue.strength);
            take_joins(s, world);
        }
    }
}

// Drops every join the slot is in: a body about to leave takes its joins
// with it.
void unglue_body(EmitterState& s, std::uint32_t slot, RigidWorld& world) {
    for (const RigidJoin& join : std::vector<RigidJoin>{s.joins}) {
        if (join.a == slot || join.b == slot) {
            world.unglue(join);
        }
    }
    take_joins(s, world);
}

// The step a pile is let go at: the first that starts at or after its
// release. 0 lets it go at once.
std::uint64_t release_tick(const SpawnPile& pile, std::uint32_t simulation_rate) {
    return static_cast<std::uint64_t>(std::ceil(static_cast<double>(pile.release) * simulation_rate));
}

// How far a body of one kind at size 1 reaches across, however it is turned:
// the diameter of the ball it turns within. The kind's own scale is in it, so
// a pile of a kind bigger than the particle's size is spaced for the bodies
// it will actually hold.
float body_reach(const RigidWorldDesc& desc, std::uint32_t kind) {
    const RigidWorldDesc::Body& body = desc.bodies[std::min<std::size_t>(kind, desc.bodies.size() - 1U)];
    const float scale = body.scale;
    switch (body.shape) {
    case RigidWorldDesc::Shape::Sphere:
        return scale;
    case RigidWorldDesc::Shape::Box:
        return scale * std::numbers::sqrt3_v<float>;
    case RigidWorldDesc::Shape::Hull: {
        float farthest = 0.0F;
        for (const Vec3& point : body.hull_points) {
            farthest = std::max(farthest, dot(point, point));
        }
        return scale * 2.0F * std::sqrt(farthest);
    }
    }
    return scale;
}

// Where `count` bodies spaced `d` apart stack, bottom first, relative to the
// middle of the pile's base; a single body's middle is the point itself.
std::vector<Vec3> pile_lattice(const SpawnPile& m, std::size_t count, float d) {
    std::vector<Vec3> points;
    if (count == 0U) {
        return points;
    }
    if (m.shape == PileShape::Single) {
        points.assign(count, Vec3{});
        return points;
    }
    if (m.shape == PileShape::Box) {
        const auto across = [d](float extent) {
            return std::max<std::size_t>(1U, static_cast<std::size_t>(std::floor(extent / d + 1.0e-4F)));
        };
        const std::size_t nx = across(m.width);
        const std::size_t nz = across(m.depth);
        const float x0 = -0.5F * static_cast<float>(nx - 1U) * d;
        const float z0 = -0.5F * static_cast<float>(nz - 1U) * d;
        for (std::size_t n = 0U; n < count; ++n) {
            const std::size_t layer = n / (nx * nz);
            const std::size_t row = (n / nx) % nz;
            const std::size_t column = n % nx;
            points.push_back(Vec3{x0 + static_cast<float>(column) * d,
                                  0.5F * d + static_cast<float>(layer) * d,
                                  z0 + static_cast<float>(row) * d});
        }
        return points;
    }
    // A heap: staggered layers, each body over a gap in the layer below,
    // taken in the order nested cones of the heap's slope reach them, so the
    // heap keeps its slope at every count.
    const float h = d * std::numbers::sqrt2_v<float> * 0.5F;
    const float rise = std::tan(m.slope * (std::numbers::pi_v<float> / 180.0F));
    struct Candidate final {
        float key;
        std::int32_t layer;
        std::int32_t row;
        std::int32_t column;
        Vec3 at;
    };
    // A cone holding `count` lattice cells, from which the heap is taken.
    const double cells = static_cast<double>(count) * d * d * h;
    double height = std::cbrt(3.0 * cells * rise * rise / std::numbers::pi);
    std::vector<Candidate> candidates;
    while (candidates.size() < count) {
        height *= 1.5;
        candidates.clear();
        const auto layers = static_cast<std::int32_t>(std::ceil((height + d) / h));
        const auto reach = static_cast<std::int32_t>(std::ceil((height / rise + d) / d)) + 1;
        for (std::int32_t layer = 0; layer < layers; ++layer) {
            const float offset = (layer % 2 == 0) ? 0.0F : 0.5F * d;
            const float y = 0.5F * d + static_cast<float>(layer) * h;
            for (std::int32_t row = -reach; row <= reach; ++row) {
                for (std::int32_t column = -reach; column <= reach; ++column) {
                    const float x = static_cast<float>(column) * d + offset;
                    const float z = static_cast<float>(row) * d + offset;
                    const float key = y + std::sqrt(x * x + z * z) * rise;
                    if (key <= static_cast<float>(height) + 0.5F * d) {
                        candidates.push_back(Candidate{key, layer, row, column, Vec3{x, y, z}});
                    }
                }
            }
        }
    }
    const auto before = [](const Candidate& a, const Candidate& b) {
        if (a.key != b.key) {
            return a.key < b.key;
        }
        if (a.layer != b.layer) {
            return a.layer < b.layer;
        }
        return a.row != b.row ? a.row < b.row : a.column < b.column;
    };
    std::partial_sort(candidates.begin(), candidates.begin() + static_cast<std::ptrdiff_t>(count), candidates.end(),
                      before);
    for (std::size_t n = 0U; n < count; ++n) {
        points.push_back(candidates[n].at);
    }
    return points;
}

// Puts the pile's newborns [first, last) on its lattice, still: the largest
// of them sets the spacing, so none overlaps however it is turned.
void place_pile(const SpawnPile& m, Step& step, float reach) {
    EmitterState& s = step.s;
    float largest = 0.0F;
    for (std::size_t i = step.first; i < step.last; ++i) {
        largest = std::max(largest, s.size[i]);
    }
    const float d = std::max(reach * largest * m.spacing, 1.0e-3F);
    const std::vector<Vec3> lattice = pile_lattice(m, step.last - step.first, d);
    for (std::size_t i = step.first; i < step.last; ++i) {
        const std::uint32_t index = s.spawn_index[i];
        const float jx = (step.random(index, m.id, 0U) - 0.5F) * m.jitter * d;
        const float jz = (step.random(index, m.id, 1U) - 0.5F) * m.jitter * d;
        s.position[i] = m.center + lattice[i - step.first] + Vec3{jx, 0.0F, jz};
        s.velocity[i] = m.initial_velocity;
        if (m.turn != Vec3{}) { s.orientation[i] = times(prop_orientation(m.turn), s.orientation[i]); }
        s.angular_velocity[i] = Vec3{};
        s.spin[i] = 0.0F;
    }
}

void append(EmitterState& s, float age, std::uint32_t spawn_index) {
    s.position.push_back({});
    s.velocity.push_back({});
    s.age.push_back(age);
    s.lifetime.push_back(1.0F);
    s.base_color.push_back({});
    s.color.push_back({});
    s.base_size.push_back(default_size);
    s.size.push_back(default_size);
    s.rotation.push_back(0.0F);
    s.orientation.push_back({});
    s.angular_velocity.push_back({});
    s.spin.push_back(0.0F);
    s.spawn_index.push_back(spawn_index);
}

// The Break module of each kind of body, by kind; none where a kind does not
// break.
std::vector<const Break*> breaks_by_kind(const Emitter& emitter, const RigidPhysics& physics) {
    const std::size_t kinds = physics.bodies.size() + 1U;
    std::vector<const Break*> breaks(kinds, nullptr);
    for (const UpdateModule& module : emitter.update) {
        if (const auto* breaking = std::get_if<Break>(&module); breaking != nullptr && breaking->body < kinds) {
            breaks[breaking->body] = breaking;
        }
    }
    return breaks;
}

// Bodies come apart into their pieces: every body whose kind breaks and for
// which `breaks_now(i, by)` holds, `i` being its place among the particles as
// they stand when this is called and `by` its kind's Break.
// - Breaks are taken in birth order, the order the emitter does everything
//   else in, and each body's pieces in the order they are listed.
// - A body breaks only if every one of its pieces fits the emitter's
//   capacity. One that does not stays whole and its pieces are counted as
//   refused, so a body is never half broken.
// - Each piece is a birth from the pieces' own source (piece_spawn_source),
//   so breaking never moves another module's spawn indices.
template <class BreaksNow>
void shatter(Step& step, const Emitter& emitter, RigidWorld& world, const std::vector<const Break*>& breaks,
             BreaksNow&& breaks_now) {
    EmitterState& s = step.s;
    const std::size_t kinds = breaks.size();
    // What the parent leaves its pieces, kept before compaction moves it.
    struct Broken final {
        Vec3 position;
        Quat orientation;
        Vec3 velocity;
        float size;
        float age;
        float lifetime;
        Color base_color;
        Color color;
        std::uint32_t spawn_index;
        const Break* by;
    };
    std::vector<Broken> broken;
    std::vector<bool> gone(s.alive(), false);
    std::size_t spare = emitter.capacity - s.alive();
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        const Break* by = breaks[std::min<std::size_t>(s.body_kind[i], kinds - 1U)];
        if (by == nullptr || !breaks_now(i, *by)) {
            continue;
        }
        // The body's own place comes free, so its pieces have one more than
        // the emitter has spare.
        if (by->pieces.size() > spare + 1U) {
            s.refused += by->pieces.size();
            continue;
        }
        spare = spare + 1U - by->pieces.size();
        gone[i] = true;
        broken.push_back(Broken{s.position[i], s.orientation[i], s.velocity[i], s.size[i], s.age[i], s.lifetime[i],
                                s.base_color[i], s.color[i], s.spawn_index[i], by});
    }
    if (broken.empty()) {
        return;
    }
    // The broken leave their world oldest first, as the dead do, and the
    // joins that held them go with them: the crack is where they were.
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        if (gone[i]) {
            unglue_body(s, s.body[i], world);
            world.remove(s.body[i]);
        }
    }
    compact(s, [&gone](std::size_t i) { return gone[i]; });
    if (s.spawn_counts.size() < detail::spawn_count_slots(emitter)) {
        s.spawn_counts.resize(detail::spawn_count_slots(emitter), 0U);
    }
    constexpr float radians = std::numbers::pi_v<float> / 180.0F;
    const std::size_t first_piece = s.alive();
    for (const Broken& parent : broken) {
        for (std::size_t k = 0U; k < parent.by->pieces.size(); ++k) {
            const BreakPiece& piece = parent.by->pieces[k];
            // Hashed from the parent and which piece this is, so a seek finds
            // the same break however it got here.
            const auto u = [&](std::uint32_t draw) {
                return step.random(parent.spawn_index, parent.by->id,
                                   static_cast<std::uint32_t>(k) * 8U + draw);
            };
            append(s, parent.age, detail::next_spawn_index(s.spawn_counts, piece_spawn_source));
            const std::size_t i = s.alive() - 1U;
            s.body_kind.push_back(piece.body);
            // Where the piece sat in the thing that broke, and the way out
            // from its middle, which is the way the piece is thrown.
            const Vec3 out = rotate(parent.orientation, piece.offset * parent.size);
            const float reach = std::sqrt(dot(out, out));
            const Vec3 away = reach > 1.0e-6F ? out * (1.0F / reach) : unit_sphere(u(0U), u(1U));
            s.position[i] = parent.position + out;
            s.orientation[i] = times(parent.orientation, prop_orientation(piece.turn));
            s.velocity[i] = parent.velocity + away * (parent.by->scatter * mix(0.5F, 1.0F, u(2U)));
            s.angular_velocity[i] =
                unit_sphere(u(3U), u(4U)) * (parent.by->spin * mix(0.5F, 1.0F, u(5U)) * radians);
            s.base_size[i] = parent.size * piece.size;
            s.size[i] = s.base_size[i];
            s.lifetime[i] = parent.lifetime;
            s.base_color[i] = parent.base_color;
            s.color[i] = parent.color;
            ++s.spawned;
        }
    }
    add_bodies(s, first_piece, emitter.capacity, world);
}

// A body that was hit hard enough comes apart. Called after the world has
// stepped, so the contacts it reports are this step's, and before the step's
// births, so every piece takes its slot in birth order.
void break_bodies(Step& step, const Emitter& emitter, const RigidPhysics& physics, RigidWorld& world,
                  std::span<const RigidContact> contacts) {
    EmitterState& s = step.s;
    const std::vector<const Break*> breaks = breaks_by_kind(emitter, physics);
    if (s.alive() == 0U || std::none_of(breaks.begin(), breaks.end(), [](const Break* b) { return b != nullptr; })) {
        return;
    }
    // The hardest this step hit each body, by its slot, from what the
    // effect reads of the step's contacts: none slower than the gentlest
    // threshold, so this is a short list.
    std::vector<float> hit(emitter.capacity, 0.0F);
    for (const RigidContact& contact : contacts) {
        if (contact.a_is == RigidSide::Body && contact.a < hit.size()) {
            hit[contact.a] = std::max(hit[contact.a], contact.speed);
        }
        if (contact.b_is == RigidSide::Body && contact.b < hit.size()) {
            hit[contact.b] = std::max(hit[contact.b], contact.speed);
        }
    }
    shatter(step, emitter, world, breaks,
            [&](std::size_t i, const Break& by) { return hit[s.body[i]] >= by.threshold; });
}

// The blasts a rigid physics emitter's Shockwave modules answer, in the step
// each goes off and in stack order. Called before the world steps, so the
// step carries the throw:
// - first the bodies of a kind that breaks when blasted come apart, those
//   thrown at its `blast_threshold` or harder, so their pieces are thrown
//   from where each of them sits;
// - then every body is thrown by the blast where it stands, times its own
//   factor (Shockwave::scale_min to scale_max), in birth order;
// - then every loose prop, in the order the module lists them, as the blast
//   is: a prop has no factor of its own. Fixed and driven props are not
//   moved by anything.
void blast_bodies(Step& step, const Emitter& emitter, const RigidPhysics& physics, RigidWorld& world) {
    EmitterState& s = step.s;
    const Effect& effect = step.in.effect;
    for (const UpdateModule& module : emitter.update) {
        const auto* wave = std::get_if<Shockwave>(&module);
        if (wave == nullptr) {
            continue;
        }
        const Blast* blast = find_blast(effect, wave->blast);
        if (blast == nullptr || step.tick != blast_tick(*blast, effect.simulation_rate)) {
            continue;
        }
        // How hard the body in place i is thrown: the blast where it stands,
        // times its own factor.
        const auto thrown = [&](std::size_t i) {
            return blast_push(*blast, s.position[i]).speed * force_scale(*wave, step, i);
        };
        const std::vector<const Break*> breaks = breaks_by_kind(emitter, physics);
        if (s.alive() != 0U && std::any_of(breaks.begin(), breaks.end(), [](const Break* b) {
                return b != nullptr && b->blast_threshold > 0.0F;
            })) {
            shatter(step, emitter, world, breaks, [&](std::size_t i, const Break& by) {
                return by.blast_threshold > 0.0F && thrown(i) >= by.blast_threshold;
            });
        }
        for (std::size_t i = 0U; i < s.alive(); ++i) {
            const BlastPush push = blast_push(*blast, s.position[i]);
            if (push.direction == Vec3{}) {
                continue;
            }
            world.push(RigidSide::Body, s.body[i], push.direction * (push.speed * force_scale(*wave, step, i)));
        }
        for (std::size_t p = 0U; p < physics.props.size() && p < s.props.size(); ++p) {
            if (physics.props[p].motion != PropMotion::Loose) {
                continue;
            }
            const BlastPush push = blast_push(*blast, s.props[p].position);
            if (push.direction == Vec3{}) {
                continue;
            }
            world.push(RigidSide::Prop, static_cast<std::uint32_t>(p), push.direction * push.speed);
        }
    }
}

// The Blast actions of the step on a rigid physics emitter, in the
// provider's order, after its Shockwave blasts: as blast_bodies does for an
// effect's blast, the bodies thrown at least as hard as their kind's
// blast_threshold come apart first, then every body and every loose prop is
// thrown. An action's blast has no per-body factor: each body takes the
// blast's speed where it stands.
void blast_actions(Step& step, const Emitter& emitter, const RigidPhysics& physics, RigidWorld& world,
                   std::span<const Action> actions) {
    EmitterState& s = step.s;
    for (const Action& action : actions) {
        if (action.kind != ActionKind::Blast || !(action.radius > 0.0F) || !(action.power > 0.0F)) {
            continue;
        }
        const Blast blast{.name = {}, .center = action.position, .power = action.power, .radius = action.radius};
        const std::vector<const Break*> breaks = breaks_by_kind(emitter, physics);
        if (s.alive() != 0U && std::any_of(breaks.begin(), breaks.end(), [](const Break* b) {
                return b != nullptr && b->blast_threshold > 0.0F;
            })) {
            shatter(step, emitter, world, breaks, [&](std::size_t i, const Break& by) {
                return by.blast_threshold > 0.0F && blast_push(blast, s.position[i]).speed >= by.blast_threshold;
            });
        }
        for (std::size_t i = 0U; i < s.alive(); ++i) {
            const BlastPush push = blast_push(blast, s.position[i]);
            if (push.direction == Vec3{}) {
                continue;
            }
            world.push(RigidSide::Body, s.body[i], push.direction * push.speed);
        }
        for (std::size_t p = 0U; p < physics.props.size() && p < s.props.size(); ++p) {
            if (physics.props[p].motion != PropMotion::Loose) {
                continue;
            }
            const BlastPush push = blast_push(blast, s.props[p].position);
            if (push.direction == Vec3{}) {
                continue;
            }
            world.push(RigidSide::Prop, static_cast<std::uint32_t>(p), push.direction * push.speed);
        }
    }
}

std::string quoted(std::string_view text) {
    return "'" + std::string{text} + "'";
}

RigidPropDesc::Shape solver_shape(PropShape shape) {
    switch (shape) {
    case PropShape::Mesh:
        return RigidPropDesc::Shape::Mesh;
    case PropShape::Hull:
        return RigidPropDesc::Shape::Hull;
    case PropShape::Sphere:
        return RigidPropDesc::Shape::Sphere;
    case PropShape::Box:
        return RigidPropDesc::Shape::Box;
    case PropShape::Cylinder:
        return RigidPropDesc::Shape::Cylinder;
    case PropShape::HollowBox:
        return RigidPropDesc::Shape::HollowBox;
    case PropShape::HollowCylinder:
        return RigidPropDesc::Shape::HollowCylinder;
    case PropShape::Cone:
        return RigidPropDesc::Shape::Cone;
    case PropShape::Pipe:
        return RigidPropDesc::Shape::Pipe;
    }
    return RigidPropDesc::Shape::Hull;
}

RigidPropDesc::Motion solver_motion(PropMotion motion) {
    switch (motion) {
    case PropMotion::Fixed:
        return RigidPropDesc::Motion::Fixed;
    case PropMotion::Driven:
        return RigidPropDesc::Motion::Driven;
    case PropMotion::Loose:
        return RigidPropDesc::Motion::Loose;
    }
    return RigidPropDesc::Motion::Fixed;
}

// One kind of body in the solver's terms: the emitter's own, or one of the
// kinds rigid physics lists. An empty `mesh` wraps the renderer's own model,
// as the emitter's shape does; a kind that names one wraps that.
Result<RigidWorldDesc::Body> rigid_body_desc(const Emitter& emitter, RigidShape shape, const std::string& mesh,
                                             float scale, float density, float friction, float restitution,
                                             const MeshSet& meshes) {
    RigidWorldDesc::Body body;
    body.scale = scale;
    body.density = density;
    body.friction = friction;
    body.restitution = restitution;
    if (shape == RigidShape::Auto) {
        shape = emitter.renderer.geometry == Geometry::Mesh || !mesh.empty() ? RigidShape::Hull : RigidShape::Sphere;
    }
    switch (shape) {
    case RigidShape::Auto:
    case RigidShape::Hull: {
        const std::string& name = mesh.empty() ? emitter.renderer.mesh : mesh;
        const auto found = meshes.find(name);
        if (found == meshes.end()) {
            return Error{ErrorCode::MissingMesh, "emitter " + quoted(emitter.name) +
                                                     " has rigid physics wrapping mesh " + quoted(name) +
                                                     ", which was not given"};
        }
        body.shape = RigidWorldDesc::Shape::Hull;
        for (const MeshVertex& vertex : found->second.vertices) {
            body.hull_points.push_back(vertex.position);
        }
        break;
    }
    case RigidShape::Sphere:
        body.shape = RigidWorldDesc::Shape::Sphere;
        break;
    case RigidShape::Box:
        body.shape = RigidWorldDesc::Shape::Box;
        break;
    }
    return body;
}

// The world a rigid physics emitter's bodies live in: its kinds of body, its
// damping, its colliders as statics, and the artist's props.
Result<RigidWorldDesc> rigid_world_desc(const Emitter& emitter, const RigidPhysics& physics, const MeshSet& meshes,
                                        float step_seconds, const std::optional<ContactOptions>& contacts,
                                        std::optional<float> slowest) {
    RigidWorldDesc desc;
    desc.capacity = emitter.capacity;
    desc.step_seconds = step_seconds;
    if (contacts) {
        desc.report_contacts = true;
        desc.contact_min_speed = contacts->min_speed;
        desc.contact_limit = contacts->limit;
    }
    // What the effect reads of the contacts (a Break deciding what broke, a
    // SpawnFromContacts bearing particles) the world reports whether the
    // host asked or not, down to the slowest it reads and at least as many
    // as it reads, so what it reads is the same whatever the host asked.
    if (slowest) {
        desc.contact_min_speed = desc.report_contacts ? std::min(desc.contact_min_speed, *slowest) : *slowest;
        desc.contact_limit =
            desc.report_contacts ? std::max(desc.contact_limit, detail::effect_contact_limit(emitter))
                                 : detail::effect_contact_limit(emitter);
        desc.report_contacts = true;
    }
    desc.angular_damping = physics.angular_damping;
    desc.allow_sleep = physics.allow_sleep;
    // Kind 0 is the emitter's own shape and material, then the kinds it
    // lists, in order, so a spawn module's `body` indexes straight into this.
    desc.bodies.clear();
    // The emitter's own kind is the particle's size exactly: its scale is
    // what every other kind is measured against.
    Result<RigidWorldDesc::Body> own = rigid_body_desc(emitter, physics.shape, {}, 1.0F, physics.density,
                                                       physics.friction, physics.restitution, meshes);
    if (!own) {
        return own.error();
    }
    desc.bodies.push_back(std::move(own).value());
    for (const RigidBodyKind& kind : physics.bodies) {
        Result<RigidWorldDesc::Body> body = rigid_body_desc(emitter, kind.shape, kind.mesh, kind.scale, kind.density,
                                                            kind.friction, kind.restitution, meshes);
        if (!body) {
            return body.error();
        }
        desc.bodies.push_back(std::move(body).value());
    }
    for (const UpdateModule& module : emitter.update) {
        // Validation refused bound inputs here, so the values are the inputs.
        if (const auto* drag = std::get_if<Drag>(&module)) {
            desc.linear_damping += drag->coefficient.value;
        } else if (const auto* plane = std::get_if<PlaneCollision>(&module)) {
            const Vec3 n = detail::normalized(plane->normal);
            desc.statics.push_back(RigidStatic{.kind = RigidStatic::Kind::Plane,
                                               .center = n * plane->offset,
                                               .normal = n,
                                               .friction = plane->friction.value,
                                               .restitution = plane->restitution.value});
        } else if (const auto* sphere = std::get_if<SphereCollision>(&module)) {
            desc.statics.push_back(RigidStatic{.kind = RigidStatic::Kind::Sphere,
                                               .center = sphere->center.value,
                                               .radius = sphere->radius.value,
                                               .friction = sphere->friction.value,
                                               .restitution = sphere->restitution.value});
        } else if (const auto* box = std::get_if<BoxCollision>(&module)) {
            const Vec3 c = box->center.value;
            const Vec3 h = box->half_size;
            const Quat turn = prop_orientation(box->turn);
            // `offset` is in the box's own frame, so the walls of a turned
            // box turn with it.
            const auto add = [&](Vec3 offset, Vec3 half) {
                desc.statics.push_back(RigidStatic{.kind = RigidStatic::Kind::Box,
                                                   .center = c + rotate(turn, offset),
                                                   .half_size = half,
                                                   .turn = turn,
                                                   .friction = box->friction.value,
                                                   .restitution = box->restitution.value});
            };
            if (!box->inside) {
                add(Vec3{}, h);
            } else {
                // Six walls around the inside, as thick as the box is large.
                const float t = std::max({h.x, h.y, h.z});
                for (const float side : {-1.0F, 1.0F}) {
                    add(Vec3{side * (h.x + t), 0.0F, 0.0F}, Vec3{t, h.y + 2.0F * t, h.z + 2.0F * t});
                    add(Vec3{0.0F, side * (h.y + t), 0.0F}, Vec3{h.x + 2.0F * t, t, h.z + 2.0F * t});
                    add(Vec3{0.0F, 0.0F, side * (h.z + t)}, Vec3{h.x + 2.0F * t, h.y + 2.0F * t, t});
                }
            }
        }
    }
    // The props, in the order the module lists them: the solver creates them
    // in that order, so their bodies are a pure function of the effect.
    for (const RigidProp& prop : physics.props) {
        RigidPropDesc out;
        out.shape = solver_shape(prop.shape);
        out.motion = solver_motion(prop.motion);
        out.half_size = prop.half_size;
        out.radius = prop.radius;
        out.height = prop.height;
        out.wall = prop.wall;
        out.segments = prop.segments;
        out.size = prop.size;
        out.density = prop.density;
        out.friction = prop.friction;
        out.restitution = prop.restitution;
        prop_pose_at(prop, 0.0F, out.position, out.orientation);
        if (prop.shape == PropShape::Mesh || prop.shape == PropShape::Hull) {
            const auto mesh = meshes.find(prop.mesh);
            if (mesh == meshes.end()) {
                return Error{ErrorCode::MissingMesh, "emitter " + quoted(emitter.name) + " has a prop shaped by mesh " +
                                                         quoted(prop.mesh) + ", which was not given"};
            }
            for (const MeshVertex& vertex : mesh->second.vertices) {
                out.points.push_back(vertex.position);
            }
            if (prop.shape == PropShape::Mesh) {
                out.indices = mesh->second.indices;
            }
        }
        desc.props.push_back(std::move(out));
    }
    return desc;
}

} // namespace

Simulation::Simulation(Effect effect, std::uint32_t instance_seed, FieldSet fields)
    : effect_{std::move(effect)},
      instance_seed_{instance_seed},
      step_seconds_{1.0F / static_cast<float>(effect_.simulation_rate)},
      order_{step_order(effect_)},
      fields_{std::move(fields)} {
    rigid_.resize(effect_.emitters.size());
    volume_work_.resize(effect_.volumes.size());
    for (std::size_t i = 0U; i < effect_.volumes.size(); ++i) {
        volume_work_[i].step = detail::volume_step(effect_.volumes[i], step_seconds_);
        volume_work_[i].deposits.sums.assign(static_cast<std::size_t>(volume_work_[i].step.cells) * 5U, 0);
    }
    has_piles_ = std::any_of(effect_.emitters.begin(), effect_.emitters.end(), [](const Emitter& emitter) {
        return std::any_of(emitter.spawn.begin(), emitter.spawn.end(),
                           [](const SpawnModule& m) { return std::holds_alternative<SpawnPile>(m); });
    });
    has_props_ = std::any_of(effect_.emitters.begin(), effect_.emitters.end(), [](const Emitter& emitter) {
        const RigidPhysics* physics = rigid_physics(emitter);
        return physics != nullptr && !physics->props.empty();
    });
    has_placed_ = std::any_of(effect_.emitters.begin(), effect_.emitters.end(), [](const Emitter& emitter) {
        return rigid_physics(emitter) != nullptr && action_module(emitter).has_value();
    });
    reset();
}

Simulation::Simulation(Simulation&&) noexcept = default;
Simulation& Simulation::operator=(Simulation&&) noexcept = default;
Simulation::~Simulation() = default;

Result<Simulation> Simulation::create(Effect effect, std::uint32_t instance_seed, FieldSet fields) {
    return create(std::move(effect), instance_seed, SimulationOptions{.fields = std::move(fields)});
}

Result<Simulation> Simulation::create(Effect effect, std::uint32_t instance_seed, SimulationOptions options) {
    Result<bool> valid = validate(effect);
    if (!valid) {
        return valid.error();
    }
    if (Result<bool> held = check_fields(effect, options.fields); !held) {
        return held.error();
    }
    const Cache* cache = options.cache.get();
    if (cache != nullptr && !(cache->effect() == effect)) {
        return Error{ErrorCode::StaleCache, "the cache was opened for another effect than " + quoted(effect.name)};
    }
    // A live emitter bearing particles from a cached one would have nothing
    // to bear them from: a cache holds bodies, not their contacts or their
    // velocities.
    if (cache != nullptr) {
        for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
            if (cache->mode(i) != CacheMode::Live) {
                // A cache keeps an emitter's particles, not what they fed a
                // grid: the grid would go unfed where the cache plays.
                for (const UpdateModule& module : effect.emitters[i].update) {
                    if (const auto* source = std::get_if<VolumeSource>(&module)) {
                        return Error{ErrorCode::StaleCache,
                                     "emitter " + quoted(effect.emitters[i].name) + " module " +
                                         quoted(VolumeSource::kind) + " feeds volume " + quoted(source->volume) +
                                         ", and the cache holds the emitter; a cache keeps particles, not what "
                                         "they deposit, so leave this emitter live"};
                    }
                }
                continue;
            }
            for (const SpawnModule& module : effect.emitters[i].spawn) {
                const std::string* named = nullptr;
                std::string_view kind;
                if (const auto* contacts = std::get_if<SpawnFromContacts>(&module)) {
                    named = &contacts->emitter;
                    kind = SpawnFromContacts::kind;
                } else if (const auto* bodies = std::get_if<SpawnFromBodies>(&module)) {
                    named = &bodies->emitter;
                    kind = SpawnFromBodies::kind;
                } else if (const auto* shed = std::get_if<SpawnFromParticles>(&module)) {
                    named = &shed->emitter;
                    kind = SpawnFromParticles::kind;
                }
                for (std::size_t other = 0U; named != nullptr && other < effect.emitters.size(); ++other) {
                    if (effect.emitters[other].name == *named && cache->mode(other) != CacheMode::Live) {
                        return Error{ErrorCode::StaleCache,
                                     "emitter " + quoted(effect.emitters[i].name) + " module " + quoted(kind) +
                                         " field 'emitter' bears particles from " + quoted(*named) +
                                         ", which the cache holds; a cache keeps bodies and particles, not their "
                                         "contacts or speeds, so cache this emitter too or leave that one live"};
                    }
                }
            }
            // A wake follows the bodies' speeds, which a cache does not keep.
            for (const UpdateModule& module : effect.emitters[i].update) {
                const auto* stirred = std::get_if<Wake>(&module);
                for (std::size_t other = 0U; stirred != nullptr && other < effect.emitters.size(); ++other) {
                    if (effect.emitters[other].name == stirred->emitter && cache->mode(other) != CacheMode::Live) {
                        return Error{ErrorCode::StaleCache,
                                     "emitter " + quoted(effect.emitters[i].name) + " module " +
                                         quoted(Wake::kind) + " field 'emitter' is stirred by " +
                                         quoted(stirred->emitter) +
                                         ", which the cache holds; a cache keeps bodies, not their speeds, so cache "
                                         "this emitter too or leave that one live"};
                    }
                }
            }
        }
    }
    std::vector<RigidEmitter> rigid(effect.emitters.size());
    const float step_seconds = 1.0F / static_cast<float>(effect.simulation_rate);
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        const Emitter& emitter = effect.emitters[i];
        const RigidPhysics* physics = rigid_physics(emitter);
        // A cached emitter is read, not simulated: it has no world.
        if (physics == nullptr || (cache != nullptr && cache->mode(i) != CacheMode::Live)) {
            continue;
        }
        // What this effect reads of its contacts, and what a larger effect
        // this one is a part of reads.
        std::optional<float> slowest = contact_floor(effect, i);
        if (i < options.contact_floors.size() && options.contact_floors[i]) {
            slowest = slowest ? std::min(*slowest, *options.contact_floors[i]) : *options.contact_floors[i];
        }
        if (options.rigid == nullptr) {
            return Error{ErrorCode::RigidSolverMissing,
                         "emitter " + quoted(emitter.name) +
                             " has rigid physics, and no rigid solver was given to run it (ajfx::rigid has one)"};
        }
        Result<RigidWorldDesc> desc =
            rigid_world_desc(emitter, *physics, options.meshes, step_seconds, options.contacts, slowest);
        if (!desc) {
            return desc.error();
        }
        Result<std::unique_ptr<RigidWorld>> world = options.rigid->create_world(desc.value());
        if (!world) {
            return Error{ErrorCode::RigidSolverFailure,
                         "emitter " + quoted(emitter.name) + "'s rigid physics world: " + world.error().message};
        }
        rigid[i] = RigidEmitter{std::move(desc).value(), std::move(world).value(), slowest, {}};
    }
    Simulation simulation{std::move(effect), instance_seed, std::move(options.fields)};
    simulation.solver_ = options.rigid;
    simulation.body_broadphase_ = options.body_broadphase;
    simulation.rigid_ = std::move(rigid);
    if (cache != nullptr) {
        simulation.cache_ = std::move(options.cache);
        simulation.pinned_.resize(simulation.effect_.parameters.size());
        for (std::size_t i = 0U; i < simulation.effect_.parameters.size(); ++i) {
            for (const BakeValue& value : simulation.cache_->identity().bake) {
                if (value.name == simulation.effect_.parameters[i].name) {
                    simulation.pinned_[i] = value.value;
                }
            }
        }
    }
    // The piles and props need their worlds, and cached emitters their
    // cache, which the constructor did not have yet.
    if (simulation.has_piles_ || simulation.has_props_ || simulation.cache_) {
        simulation.reset();
    }
    return simulation;
}

void Simulation::resolve(const ParameterProvider* parameters, std::uint64_t tick,
                         std::vector<ParameterValue>& values) const {
    detail::resolve_values(effect_, parameters, tick, values);
    for (std::size_t i = 0U; i < pinned_.size() && i < values.size(); ++i) {
        if (pinned_[i]) {
            values[i] = *pinned_[i];
        }
    }
}

bool Simulation::cached(std::size_t index) const noexcept {
    return cache_ != nullptr && cache_->mode(index) != CacheMode::Live;
}

void Simulation::load_cached() {
    if (!cache_) {
        return;
    }
    for (std::size_t i = 0U; i < effect_.emitters.size(); ++i) {
        if (cached(i)) {
            state_.emitters[i] = cache_->emitter_at(i, state_.tick);
        }
    }
}

bool Simulation::beyond_cache() const noexcept {
    return cache_ != nullptr && cache_->caches_anything() && state_.tick >= cache_->frames();
}

std::optional<SimulationState> Simulation::cached_checkpoint(std::uint64_t tick) const {
    if (!cache_) {
        return std::nullopt;
    }
    std::optional<SimulationState> state = cache_->checkpoint_before(tick);
    if (state && state->emitters.size() != effect_.emitters.size()) {
        return std::nullopt;
    }
    return state;
}

std::vector<ParameterValue> parameter_values(const Effect& effect, const ParameterProvider* provider,
                                             std::uint64_t tick) {
    std::vector<ParameterValue> values;
    detail::resolve_values(effect, provider, tick, values);
    return values;
}

DrawAdjustment draw_adjustment(const Effect& effect, std::size_t emitter, std::span<const ParameterValue> values) {
    if (emitter >= effect.emitters.size()) {
        return {};
    }
    std::vector<ParameterValue> resolved;
    if (values.size() != effect.parameters.size()) {
        detail::resolve_values(effect, nullptr, 0U, resolved);
        std::copy_n(values.begin(), std::min(values.size(), resolved.size()), resolved.begin());
        values = resolved;
    }
    const Inputs in{effect, values};
    const SpriteRenderer& renderer = effect.emitters[emitter].renderer;
    const Color tint = in(renderer.tint);
    const float brightness = in(renderer.brightness);
    return DrawAdjustment{Color{tint.r * brightness, tint.g * brightness, tint.b * brightness,
                                tint.a * in(renderer.opacity)},
                          in(renderer.size_scale)};
}

void adjust_for_drawing(const DrawAdjustment& adjustment, EmitterState& particles) {
    if (adjustment == DrawAdjustment{}) {
        return;
    }
    for (Color& color : particles.color) {
        color = times(color, adjustment.color);
    }
    for (float& size : particles.size) {
        size *= adjustment.size;
    }
}

void Simulation::reset(const ParameterProvider* parameters, const ActionProvider* actions) {
    std::vector<ParameterValue> values;
    resolve(parameters, 0U, values);
    start(values, actions);
}

void Simulation::prepare(const ParameterProvider* parameters, const ActionProvider* actions) {
    if (state_.tick != 0U || (!has_piles_ && !has_placed_)) {
        return;
    }
    std::vector<ParameterValue> values;
    resolve(parameters, 0U, values);
    if ((has_piles_ && values != pile_values_) || (has_placed_ && placed_digest(actions) != placed_)) {
        start(values, actions);
    }
}

std::uint64_t Simulation::placed_digest(const ActionProvider* actions) const {
    std::uint64_t hash = 0xcbf29ce484222325ULL;
    if (actions == nullptr) {
        return hash;
    }
    for (const Emitter& emitter : effect_.emitters) {
        if (rigid_physics(emitter) != nullptr && action_module(emitter)) {
            hash = action_digest(actions->actions_at(emitter.name, 0U), hash);
        }
    }
    return hash;
}

void Simulation::start(std::span<const ParameterValue> values, const ActionProvider* actions) {
    state_ = SimulationState{};
    state_.emitters.resize(effect_.emitters.size());
    for (std::size_t i = 0U; i < effect_.emitters.size(); ++i) {
        state_.emitters[i].rate_carry.assign(detail::rate_module_count(effect_.emitters[i]), 0.0);
        state_.emitters[i].spawn_counts.assign(detail::spawn_count_slots(effect_.emitters[i]), 0U);
    }
    state_.volumes.clear();
    for (std::size_t i = 0U; i < effect_.volumes.size(); ++i) {
        state_.volumes.push_back(detail::empty_volume(effect_.volumes[i]));
        volume_work_[i].deposits.clear();
    }
    rebuild_worlds();
    for (RigidEmitter& r : rigid_) {
        r.read.clear();
    }
    solver_state_stale_ = false;
    // The props stand in their worlds from before the first step, whatever
    // else the emitter does, so a paused effect at the start shows them.
    // Reading a pose changes nothing, so the solver state stays fresh.
    for (std::size_t i = 0U; i < rigid_.size(); ++i) {
        const RigidWorld* world = rigid_[i].world.get();
        if (world == nullptr) {
            continue;
        }
        EmitterState& s = state_.emitters[i];
        s.props.resize(rigid_[i].desc.props.size());
        for (std::size_t p = 0U; p < s.props.size(); ++p) {
            s.props[p] = world->prop(p);
        }
    }
    pile_values_.assign(values.begin(), values.end());
    placed_ = placed_digest(actions);
    if (has_piles_ || has_placed_) {
        for (std::size_t i = 0U; i < effect_.emitters.size(); ++i) {
            prespawn(i, values, actions);
        }
        solver_state_stale_ = std::any_of(rigid_.begin(), rigid_.end(),
                                          [](const RigidEmitter& r) { return r.world != nullptr; });
    }
    load_cached();
}

// Each pile's bodies, born at age 0 before step 0 and set up by the init
// modules as any newborn, then put on the pile still and, until released,
// asleep.
void Simulation::prespawn(std::size_t index, std::span<const ParameterValue> values, const ActionProvider* actions) {
    const Emitter& emitter = effect_.emitters[index];
    EmitterState& s = state_.emitters[index];
    RigidWorld* world = rigid_[index].world.get();
    if (world == nullptr) {
        return;
    }
    Step step{s, Inputs{effect_, values}, Random{hash_mix(instance_seed_, emitter.seed)}, step_seconds_, 0.0, 0.0,
              0U, 0U, &fields_};
    const auto run = [&step](const auto& module) { apply(module, step); };
    for (std::size_t slot = 0U; slot < emitter.spawn.size(); ++slot) {
        const auto* pile = std::get_if<SpawnPile>(&emitter.spawn[slot]);
        if (pile == nullptr) {
            continue;
        }
        // A pile is stacked on the reach of the kind it spawns.
        const float reach = body_reach(rigid_[index].desc, pile->body);
        step.first = s.alive();
        for (std::uint32_t c = 0U; c < pile->count; ++c) {
            const std::uint32_t spawn_index =
                detail::next_spawn_index(s.spawn_counts, static_cast<std::uint32_t>(slot));
            if (s.alive() < emitter.capacity) {
                append(s, 0.0F, spawn_index);
                s.body_kind.push_back(pile->body);
            } else {
                ++s.refused;
            }
            ++s.spawned;
        }
        step.last = s.alive();
        for (const InitModule& module : emitter.init) {
            std::visit(run, module);
        }
        for (std::size_t i = step.first; i < step.last; ++i) {
            s.color[i] = s.base_color[i];
            s.size[i] = s.base_size[i];
        }
        for (const UpdateModule& module : emitter.update) {
            if (std::holds_alternative<ColorOverLife>(module)) {
                std::visit(run, module);
            }
        }
        detail::glow(emitter, s, step.first, step.last);
        place_pile(*pile, step, reach);
        add_bodies(s, step.first, emitter.capacity, *world, release_tick(*pile, effect_.simulation_rate) > 0U);
        if (const Glue* glue = glue_module(emitter)) {
            glue_bodies(s, step.first, *glue, *world);
        }
        // Step 0 shows the poses the engine holds (it keeps its own
        // normalized turn), so a held body never moves by a bit.
        for (std::size_t i = step.first; i < step.last; ++i) {
            const RigidBodyState body = world->body(s.body[i]);
            s.position[i] = body.position;
            s.orientation[i] = body.orientation;
        }
    }
    // The bodies actions place at tick 0 stand before the first step, each
    // set up as any newborn and then put exactly where its action says,
    // asleep until its release. They are joined to nothing (Glue holds
    // bodies born together in a step; these are placed one by one).
    const auto placing = action_module(emitter);
    if (actions == nullptr || !placing) {
        return;
    }
    const auto& [module, source] = *placing;
    std::uint32_t made = 0U;
    for (const Action& action : actions->actions_at(emitter.name, 0U)) {
        if (action.kind != ActionKind::Spawn) {
            continue;
        }
        ++s.spawned;
        if (made >= module->max_alive || action.serial > spawn_count_mask || s.alive() >= emitter.capacity) {
            ++s.refused;
            continue;
        }
        const detail::BirthOrigin origin = detail::action_origin(action, 0U, true);
        step.first = s.alive();
        append(s, 0.0F, spawn_index_of(source, action.serial));
        s.body_kind.push_back(action.body);
        s.held_until.push_back(origin.held_until);
        step.last = s.alive();
        for (const InitModule& init : emitter.init) {
            std::visit(run, init);
        }
        take_origin(s, step.first, origin);
        s.color[step.first] = s.base_color[step.first];
        s.size[step.first] = s.base_size[step.first];
        for (const UpdateModule& over_life : emitter.update) {
            if (std::holds_alternative<ColorOverLife>(over_life)) {
                std::visit(run, over_life);
            }
        }
        detail::glow(emitter, s, step.first, step.last);
        add_bodies(s, step.first, emitter.capacity, *world);
        const RigidBodyState body = world->body(s.body[step.first]);
        s.position[step.first] = body.position;
        s.orientation[step.first] = body.orientation;
        ++made;
    }
}

bool Simulation::held(std::size_t index, std::size_t i) const {
    const EmitterState& s = state_.emitters[index];
    if (i < s.held_until.size() && state_.tick < s.held_until[i]) {
        return true;
    }
    const std::vector<SpawnModule>& spawn = effect_.emitters[index].spawn;
    const std::uint32_t source = spawn_source(s.spawn_index[i]);
    if (source >= spawn.size()) {
        return false;
    }
    const auto* pile = std::get_if<SpawnPile>(&spawn[source]);
    return pile != nullptr && state_.tick < release_tick(*pile, effect_.simulation_rate);
}

void Simulation::rebuild_worlds() {
    for (std::size_t i = 0U; i < rigid_.size(); ++i) {
        RigidEmitter& rigid = rigid_[i];
        if (solver_ == nullptr || !rigid.world) {
            continue;
        }
        // The solver made this world once from the same description.
        Result<std::unique_ptr<RigidWorld>> world = solver_->create_world(rigid.desc);
        if (!world) {
            continue;
        }
        rigid.world = std::move(world).value();
        const EmitterState& s = state_.emitters[i];
        const Glue* glue = glue_module(effect_.emitters[i]);
        for (std::size_t p = 0U; p < s.alive() && p < s.body.size(); ++p) {
            rigid.world->add(s.body[p], p < s.body_kind.size() ? s.body_kind[p] : 0U, s.size[p],
                             RigidBodyState{s.position[p], s.orientation[p], s.velocity[p], s.angular_velocity[p]},
                             held(i, p));
        }
        // The joins the state holds, made again in the order they were, so
        // a restored world holds them as the saved one did. The solver
        // state that follows was written against exactly these.
        if (glue != nullptr) {
            for (const RigidJoin& join : s.joins) {
                rigid.world->glue(join, glue->strength);
            }
        }
    }
}

const SimulationState& Simulation::state() const {
    if (solver_state_stale_) {
        for (std::size_t i = 0U; i < rigid_.size(); ++i) {
            if (rigid_[i].world) {
                state_.emitters[i].solver_state = rigid_[i].world->save();
            }
        }
        solver_state_stale_ = false;
    }
    return state_;
}

std::vector<BodyCollider> body_colliders(const RigidPhysics& physics, const EmitterState& state,
                                         Geometry geometry) {
    std::vector<BodyCollider> out;
    out.reserve(state.alive());
    for (std::size_t i = 0U; i < state.alive(); ++i) {
        const std::size_t kind = i < state.body_kind.size() ? state.body_kind[i] : 0U;
        RigidShape shape = physics.shape;
        // The kind's own size as well, so a particle meets the body the
        // engine built rather than the one the emitter's size describes.
        float scale = 1.0F;
        if (kind > 0U && kind <= physics.bodies.size()) {
            shape = physics.bodies[kind - 1U].shape;
            scale = physics.bodies[kind - 1U].scale;
        }
        if (shape == RigidShape::Auto) {
            shape = geometry == Geometry::Mesh ? RigidShape::Hull : RigidShape::Sphere;
        }
        // A hull is met as the ball its size describes: its model's own
        // shape is the engine's business, not a particle's.
        out.push_back(BodyCollider{state.position[i], state.orientation[i], 0.5F * state.size[i] * scale,
                                   shape == RigidShape::Box, state.velocity[i], state.spawn_index[i],
                                   static_cast<std::uint32_t>(kind)});
    }
    return out;
}

std::span<const RigidContact> Simulation::contacts(std::size_t index) const {
    if (index >= rigid_.size() || !rigid_[index].world) {
        return {};
    }
    return rigid_[index].world->contacts();
}

std::uint32_t Simulation::contacts_dropped(std::size_t index) const {
    if (index >= rigid_.size() || !rigid_[index].world) {
        return 0U;
    }
    return rigid_[index].world->contacts_dropped();
}

void Simulation::restore(const SimulationState& state) {
    state_ = state;
    // A state from another effect's grids, or none, starts them empty: the
    // step needs grids of the volumes' own sizes.
    if (state_.volumes.size() != effect_.volumes.size()) {
        state_.volumes.clear();
    }
    for (std::size_t i = 0U; i < effect_.volumes.size(); ++i) {
        const detail::VolumeStep& grid = volume_work_[i].step;
        if (i >= state_.volumes.size()) {
            state_.volumes.push_back(detail::empty_volume(effect_.volumes[i]));
        } else if (state_.volumes[i].nx != grid.nx || state_.volumes[i].ny != grid.ny ||
                   state_.volumes[i].nz != grid.nz || state_.volumes[i].density.size() != grid.cells ||
                   state_.volumes[i].temperature.size() != grid.cells ||
                   state_.volumes[i].velocity.size() != grid.cells) {
            state_.volumes[i] = detail::empty_volume(effect_.volumes[i]);
        }
        volume_work_[i].deposits.clear();
    }
    rebuild_worlds();
    for (RigidEmitter& r : rigid_) {
        r.read.clear();
    }
    bool stale = false;
    for (std::size_t i = 0U; i < rigid_.size(); ++i) {
        if (!rigid_[i].world) {
            continue;
        }
        const std::vector<std::uint8_t>& bytes = state_.emitters[i].solver_state;
        if (bytes.empty() || !rigid_[i].world->restore(bytes)) {
            // Only bodies' poses to go on: what state() returns next will say
            // so, rather than repeat bytes this world does not hold.
            stale = true;
        }
    }
    solver_state_stale_ = stale;
}

void Simulation::step(const ParameterProvider* parameters, const ActionProvider* actions) {
    resolve(parameters, state_.tick, values_);
    if (state_.tick == 0U &&
        ((has_piles_ && values_ != pile_values_) || (has_placed_ && placed_digest(actions) != placed_))) {
        start(values_, actions);
    }
    // Rigid physics emitters step first, all of them, then the rest, each
    // after the emitters it sheds particles from (step_order). A particle
    // that meets a body has to meet it where this step left it, one shed by
    // a particle where this step left that particle, and which emitters
    // those are must not depend on the order they happen to be listed in: so
    // the order is this one, fixed, declared and the same on every node,
    // rather than "whatever is listed before me". Rigid emitters cannot meet
    // each other's bodies (check_rigid_physics refuses a collider there), so
    // the order within the first pass settles nothing and is simply the
    // effect's own.
    for (const std::size_t i : order_) {
        // A cached emitter is read where it would have been stepped, so a
        // live emitter meets its bodies where this step left them.
        if (cached(i)) {
            cache_->advance(i, state_.tick + 1U, state_.emitters[i]);
        } else {
            step_emitter(i, values_, actions);
        }
    }
    // Then every grid, in the effect's order, on what this step deposited:
    // the particles read the grids as the step began, so this order is the
    // GPU's too.
    for (std::size_t i = 0U; i < volume_work_.size(); ++i) {
        detail::step_volume(volume_work_[i].step, volume_work_[i].deposits, state_.volumes[i],
                            volume_work_[i].scratch);
    }
    ++state_.tick;
    solver_state_stale_ = solver_state_stale_ || std::any_of(rigid_.begin(), rigid_.end(), [](const RigidEmitter& r) {
                                                     return r.world != nullptr;
                                                 });
}

void Simulation::advance_to(std::uint64_t target, const ParameterProvider* parameters,
                            const ActionProvider* actions) {
    while (state_.tick < target) {
        step(parameters, actions);
    }
}

void Simulation::step_emitter(std::size_t index, std::span<const ParameterValue> values,
                              const ActionProvider* actions) {
    const Emitter& emitter = effect_.emitters[index];
    EmitterState& s = state_.emitters[index];
    const double rate = static_cast<double>(effect_.simulation_rate);
    Step step{s,
              Inputs{effect_, values},
              Random{hash_mix(instance_seed_, emitter.seed)},
              step_seconds_,
              static_cast<double>(state_.tick) / rate,
              static_cast<double>(state_.tick + 1U) / rate,
              0U,
              0U,
              &fields_};
    step.tick = state_.tick;
    step.volumes = &state_.volumes;
    step.volume_work = &volume_work_;
    if (s.spawn_counts.size() != detail::spawn_count_slots(emitter)) {
        s.spawn_counts.resize(detail::spawn_count_slots(emitter), 0U);
    }
    for (const UpdateModule& module : emitter.update) {
        if (const auto* rigid = std::get_if<RigidBody>(&module)) {
            step.rigid =
                BodyResponse{rigid->contact_scale, step.in(rigid->rest_speed), step.in(rigid->roundness)};
        }
    }

    // The step's actions, if the emitter takes any (SpawnActions). What is
    // removed goes with the dead; the rest act before the world steps, and
    // the spawns are born with the step's births.
    const std::optional<std::pair<const SpawnActions*, std::uint32_t>> acting = action_module(emitter);
    const std::span<const Action> acts =
        acting && actions != nullptr ? actions->actions_at(emitter.name, state_.tick) : std::span<const Action>{};
    RigidWorld* world = rigid_[index].world.get();
    if (world != nullptr) {
        remove_targets(acts, s);
    }

    // Age, and retire particles whose life is over. A rigid physics body
    // leaves its world with its particle, oldest first.
    for (float& age : s.age) {
        age += step.dt;
    }
    if (world != nullptr) {
        for (std::size_t i = 0U; i < s.alive(); ++i) {
            if (!(s.age[i] < s.lifetime[i])) {
                unglue_body(s, s.body[i], *world);
                world->remove(s.body[i]);
            }
        }
    }
    compact(s, [&s](std::size_t i) { return !(s.age[i] < s.lifetime[i]); });

    step.last = s.alive();
    // The bodies this emitter's colliders and wakes name, as they stand now,
    // sorted into their grids. Every rigid physics emitter steps first, so
    // their physics has already moved this step.
    detail::BodyGrids bodies;
    for (const UpdateModule& module : emitter.update) {
        const std::string* named = nullptr;
        if (const auto* meets = std::get_if<BodyCollision>(&module)) {
            named = &meets->emitter;
        } else if (const auto* stirred = std::get_if<Wake>(&module)) {
            named = &stirred->emitter;
        }
        if (named == nullptr || bodies.contains(*named)) {
            continue;
        }
        for (std::size_t other = 0U; other < effect_.emitters.size(); ++other) {
            const RigidPhysics* physics = rigid_physics(effect_.emitters[other]);
            if (physics == nullptr || effect_.emitters[other].name != *named) {
                continue;
            }
            bodies.emplace(*named, detail::make_body_grid(
                                       body_colliders(*physics, state_.emitters[other],
                                                      effect_.emitters[other].renderer.geometry),
                                       body_broadphase_));
            break;
        }
    }
    if (!bodies.empty()) {
        step.bodies = &bodies;
    }
    const auto run = [&step](const auto& module) { apply(module, step); };
    if (world != nullptr) {
        // Piles let go this step wake, in slot order. A pile's bodies are
        // the births of its own module (spawn_source).
        std::vector<std::uint32_t> waking;
        for (std::size_t i = 0U; i < s.alive(); ++i) {
            const std::uint32_t source = spawn_source(s.spawn_index[i]);
            if (source >= emitter.spawn.size()) {
                continue;
            }
            if (const auto* pile = std::get_if<SpawnPile>(&emitter.spawn[source])) {
                const std::uint64_t release = release_tick(*pile, effect_.simulation_rate);
                if (release > 0U && release == state_.tick) {
                    waking.push_back(s.body[i]);
                }
            }
        }
        // So do the bodies actions put down to wait until this tick.
        for (std::size_t i = 0U; i < s.alive() && i < s.held_until.size(); ++i) {
            if (s.held_until[i] != 0U && s.held_until[i] == state_.tick) {
                waking.push_back(s.body[i]);
            }
        }
        if (!waking.empty()) {
            std::sort(waking.begin(), waking.end());
            waking.erase(std::unique(waking.begin(), waking.end()), waking.end());
            world->wake(waking);
        }
        const RigidPhysics& physics = *rigid_physics(emitter);
        // Hands and blasts act before the world steps, so the step carries
        // them: the actions first, then the blasts.
        push_bodies(acts, s, *world);
        blast_bodies(step, emitter, physics, *world);
        blast_actions(step, emitter, physics, *world, acts);
        step.last = s.alive();
        move_rigid(step, emitter, physics, *world);
        // What the effect reads of the step's contacts, for its own breaks
        // and for the emitters bearing particles from them, which step after
        // this one.
        RigidEmitter& rigid = rigid_[index];
        rigid.read.clear();
        if (rigid.floor) {
            rigid.read =
                detail::effect_contacts(world->contacts(), *rigid.floor, detail::effect_contact_limit(emitter));
        }
        break_bodies(step, emitter, physics, *world, rigid.read);
    } else {
        // Forces, integration, then constraints and appearance.
        for (const UpdateModule& module : emitter.update) {
            if (is_force(module)) {
                std::visit(run, module);
            }
        }
        for (std::size_t i = 0U; i < s.alive(); ++i) {
            s.position[i] += s.velocity[i] * step.dt;
        }
        const InitSpin* spin = nullptr;
        for (const InitModule& module : emitter.init) {
            if (const auto* found = std::get_if<InitSpin>(&module)) {
                spin = found;
            }
        }
        if (spin != nullptr) {
            turn(*spin, step);
        }
        for (const UpdateModule& module : emitter.update) {
            if (!is_force(module)) {
                std::visit(run, module);
            }
        }
        detail::glow(emitter, s, step.first, step.last);
        compact(s, [&s](std::size_t i) { return !(s.age[i] < s.lifetime[i]); });
    }

    // The bodies and contacts the spawn modules bear particles from, as the
    // rigid physics emitters (which have stepped) left them.
    detail::BodySources sources;
    for (const SpawnModule& module : emitter.spawn) {
        const std::string* named = nullptr;
        if (const auto* contacts = std::get_if<SpawnFromContacts>(&module)) {
            named = &contacts->emitter;
        } else if (const auto* shed = std::get_if<SpawnFromBodies>(&module)) {
            named = &shed->emitter;
        }
        if (named == nullptr || sources.contains(*named)) {
            continue;
        }
        for (std::size_t other = 0U; other < effect_.emitters.size(); ++other) {
            const RigidPhysics* physics = rigid_physics(effect_.emitters[other]);
            if (physics == nullptr || effect_.emitters[other].name != *named) {
                continue;
            }
            sources.emplace(*named, detail::body_source(*physics, state_.emitters[other], rigid_[other].read,
                                                        effect_.emitters[other].capacity));
            break;
        }
    }
    // The particles the spawn modules shed from, as the emitters named (which
    // have stepped, step_order) left them.
    detail::ParticleSources shed_from;
    for (const SpawnModule& module : emitter.spawn) {
        const auto* shed = std::get_if<SpawnFromParticles>(&module);
        if (shed == nullptr || shed_from.contains(shed->emitter)) {
            continue;
        }
        for (std::size_t other = 0U; other < effect_.emitters.size(); ++other) {
            if (other != index && effect_.emitters[other].name == shed->emitter) {
                shed_from.emplace(shed->emitter, &state_.emitters[other]);
                break;
            }
        }
    }

    // Births, each with the age it has reached by the end of the step, and
    // where a contact or a body puts it, if one does.
    const std::size_t first_new = s.alive();
    std::vector<std::optional<detail::BirthOrigin>> origins;
    const auto birth = [&s, &emitter, &origins, world](double age, std::uint32_t kind, std::uint32_t,
                                                       std::uint32_t spawn_index,
                                                       const detail::BirthOrigin* origin, bool refused) {
        if (!refused && s.alive() < emitter.capacity) {
            append(s, static_cast<float>(age), spawn_index);
            if (world != nullptr) {
                s.body_kind.push_back(kind);
                s.held_until.push_back(origin != nullptr ? origin->held_until : 0U);
            }
            origins.push_back(origin != nullptr ? std::optional<detail::BirthOrigin>{*origin} : std::nullopt);
        } else {
            ++s.refused;
        }
        ++s.spawned;
    };
    detail::BirthContext context{state_.tick, step.random.seed, step.dt, &sources, &shed_from};
    if (acting) {
        context.actions = actions;
        context.emitter = emitter.name;
        context.rigid = world != nullptr;
        // A rigid emitter keeps at most max_alive action-born bodies; a
        // particle emitter's room is its step's cap.
        const std::uint32_t born = world != nullptr ? action_born(s, acting->second) : 0U;
        context.action_room = world != nullptr
                                  ? (born < acting->first->max_alive ? acting->first->max_alive - born : 0U)
                                  : acting->first->max_per_step;
        context.skip_spawns = world != nullptr && state_.tick == 0U;
    }
    detail::for_each_birth(emitter, step.in, s.rate_carry, s.spawn_counts, step.start, step.end, rate, context,
                           birth);

    // Initialise the newborns, carry them through the part of the step they
    // lived, then give them their appearance at that age.
    step.first = first_new;
    step.last = s.alive();
    if (step.first == step.last) {
        return;
    }
    for (const InitModule& module : emitter.init) {
        std::visit(run, module);
    }
    // A particle born of a contact or a body starts there: its init
    // modules' position is an offset from it, their velocity added to what
    // it was given. As ajfx's spawn kernel does it.
    for (std::size_t i = step.first; i < step.last; ++i) {
        if (const std::optional<detail::BirthOrigin>& origin = origins[i - step.first]) {
            take_origin(s, i, *origin);
        }
    }
    for (std::size_t i = step.first; i < step.last; ++i) {
        s.color[i] = s.base_color[i];
        s.size[i] = s.base_size[i];
        s.position[i] += s.velocity[i] * s.age[i];
        s.rotation[i] += s.spin[i] * s.age[i];
    }
    for (const UpdateModule& module : emitter.update) {
        if (std::holds_alternative<ColorOverLife>(module) || std::holds_alternative<SizeOverLife>(module)) {
            std::visit(run, module);
        }
    }
    detail::glow(emitter, s, step.first, step.last);
    if (world != nullptr) {
        add_bodies(s, step.first, emitter.capacity, *world);
        if (const Glue* glue = glue_module(emitter)) {
            glue_bodies(s, step.first, *glue, *world);
        }
    }
}

CheckpointRing::CheckpointRing(std::uint64_t interval, std::size_t capacity)
    : interval_{std::max<std::uint64_t>(interval, 1U)}, capacity_{std::max<std::size_t>(capacity, 1U)} {}

void CheckpointRing::record(const Simulation& simulation) {
    const std::uint64_t tick = simulation.tick();
    for (const SimulationState& checkpoint : checkpoints_) {
        if (checkpoint.tick == tick) {
            return;
        }
    }
    if (checkpoints_.size() == capacity_) {
        checkpoints_.erase(checkpoints_.begin());
    }
    checkpoints_.push_back(simulation.state());
}

void CheckpointRing::clear_from(std::uint64_t tick) noexcept {
    std::erase_if(checkpoints_, [tick](const SimulationState& c) { return c.tick > tick || tick == 0U; });
    stale_from_ = stale_from_ ? std::min(*stale_from_, tick) : tick;
}

void CheckpointRing::seek(Simulation& simulation, std::uint64_t target, const ParameterProvider* parameters,
                          const ActionProvider* actions) {
    const SimulationState* best = nullptr;
    for (const SimulationState& checkpoint : checkpoints_) {
        if (checkpoint.tick <= target && (best == nullptr || checkpoint.tick > best->tick)) {
            best = &checkpoint;
        }
    }
    // A cache's checkpoints are as good as the ring's, and a far seek finds
    // one there that the ring never passed.
    std::optional<std::uint64_t> cached_tick;
    if (simulation.cache() != nullptr) {
        cached_tick = simulation.cache()->checkpoint_tick(target);
        if (cached_tick && best != nullptr && best->tick >= *cached_tick) {
            cached_tick.reset();
        }
    }
    const bool any = best != nullptr || cached_tick.has_value();
    const std::uint64_t best_tick = cached_tick ? *cached_tick : (best != nullptr ? best->tick : 0U);
    // Where it stands is of the old performance if it is past the tick the
    // actions changed at (or that tick was 0, which placed its bodies).
    const bool current_stale = stale_from_ && (simulation.tick() > *stale_from_ || *stale_from_ == 0U);
    stale_from_.reset();
    const bool keep_current =
        !current_stale && simulation.tick() <= target && (!any || simulation.tick() >= best_tick);
    if (!keep_current) {
        std::optional<SimulationState> from_cache;
        if (cached_tick) {
            from_cache = simulation.cached_checkpoint(target);
        }
        if (from_cache) {
            simulation.restore(*from_cache);
        } else if (best != nullptr) {
            simulation.restore(*best);
        } else {
            simulation.reset(parameters, actions);
        }
    }
    simulation.prepare(parameters, actions);
    while (true) {
        if (simulation.tick() % interval_ == 0U) {
            record(simulation);
        }
        if (simulation.tick() >= target) {
            break;
        }
        simulation.step(parameters, actions);
    }
}

} // namespace ajfx
