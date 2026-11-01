#include <ajfx/simulation.hpp>

#include <ajfx/random.hpp>

#include "noise.hpp"
#include "gpu/pack.hpp"
#include "step_inputs.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace ajfx {
namespace {

// Size of a particle with no size module, in world units.
constexpr float default_size = 0.1F;

using detail::Inputs;
using detail::normalized;

float mix(float a, float b, float t) {
    return a + (b - a) * t;
}

Color times(Color a, Color b) {
    return {a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a};
}

template <class Key>
auto sample(const std::vector<Key>& keys, float t) {
    const auto next = std::find_if(keys.begin(), keys.end(), [t](const Key& key) { return key.t >= t; });
    if (next == keys.begin()) {
        return keys.front().value;
    }
    if (next == keys.end()) {
        return keys.back().value;
    }
    const Key& a = *(next - 1);
    const Key& b = *next;
    const float span = b.t - a.t;
    const float f = span > 0.0F ? (t - a.t) / span : 1.0F;
    if constexpr (std::is_same_v<Key, ColorKey>) {
        return Color{mix(a.value.r, b.value.r, f), mix(a.value.g, b.value.g, f), mix(a.value.b, b.value.b, f),
                     mix(a.value.a, b.value.a, f)};
    } else {
        return mix(a.value, b.value, f);
    }
}

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
        }
        ++kept;
    }
    if (bodies) {
        s.body.resize(kept);
    }
    if (kinds) {
        s.body_kind.resize(kept);
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

float normalized_age(const EmitterState& s, std::size_t i) {
    return std::clamp(s.age[i] / s.lifetime[i], 0.0F, 1.0F);
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
    std::optional<BodyResponse> rigid{};
};

// ------------------------------------------------------------ init modules

void apply(const Lifetime& m, Step& step) {
    const float lo = step.in(m.min);
    const float hi = step.in(m.max);
    for (std::size_t i = step.first; i < step.last; ++i) {
        step.s.lifetime[i] = mix(lo, hi, step.random(step.s.spawn_index[i], m.id, 0U));
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
        step.s.base_size[i] = mix(lo, hi, step.random(step.s.spawn_index[i], m.id, 0U));
    }
}

void apply(const InitRotation& m, Step& step) {
    const float lo = step.in(m.min);
    const float hi = step.in(m.max);
    constexpr float radians = std::numbers::pi_v<float> / 180.0F;
    for (std::size_t i = step.first; i < step.last; ++i) {
        step.s.rotation[i] = mix(lo, hi, step.random(step.s.spawn_index[i], m.id, 0U)) * radians;
    }
}

void apply(const InitTumble& m, Step& step) {
    const float lo = step.in(m.speed_min);
    const float hi = step.in(m.speed_max);
    constexpr float radians = std::numbers::pi_v<float> / 180.0F;
    constexpr float two_pi = 2.0F * std::numbers::pi_v<float>;
    for (std::size_t i = step.first; i < step.last; ++i) {
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

// ------------------------------------------------------------ constraints and appearance

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
    for (std::size_t i = step.first; i < step.last; ++i) {
        const Vec3 d = step.s.position[i] - center;
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
    for (std::size_t i = step.first; i < step.last; ++i) {
        step.s.color[i] = times(step.s.base_color[i], sample(m.keys, normalized_age(step.s, i)));
    }
}

void apply(const SizeOverLife& m, Step& step) {
    for (std::size_t i = step.first; i < step.last; ++i) {
        step.s.size[i] = step.s.base_size[i] * sample(m.keys, normalized_age(step.s, i));
    }
}

// The engine moves rigid physics particles (see move_rigid).
void apply(const RigidPhysics&, Step&) {}

// A body comes apart after the world has stepped (see break_bodies).
void apply(const Break&, Step&) {}

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
    for (const UpdateModule& module : emitter.update) {
        if (const auto* color = std::get_if<ColorOverLife>(&module)) {
            apply(*color, step);
        }
    }
}

// Bodies for the newborns [first, alive), in birth order, each in the lowest
// slot no older particle holds; asleep if `asleep`.
void add_bodies(EmitterState& s, std::size_t first, std::uint32_t capacity, RigidWorld& world, bool asleep = false) {
    std::vector<bool> used(capacity, false);
    for (std::size_t i = 0U; i < first; ++i) {
        used[s.body[i]] = true;
    }
    s.body.resize(s.alive());
    s.body_kind.resize(s.alive());
    std::uint32_t next = 0U;
    for (std::size_t i = first; i < s.alive(); ++i) {
        while (used[next]) {
            ++next;
        }
        used[next] = true;
        s.body[i] = next;
        world.add(next, s.body_kind[i], s.size[i],
                  RigidBodyState{s.position[i], s.orientation[i], s.velocity[i], s.angular_velocity[i]}, asleep);
    }
}

// The step a pile is let go at: the first that starts at or after its
// release. 0 lets it go at once.
std::uint64_t release_tick(const SpawnPile& pile, std::uint32_t simulation_rate) {
    return static_cast<std::uint64_t>(std::ceil(static_cast<double>(pile.release) * simulation_rate));
}

// How far a body of one kind at size 1 reaches across, however it is turned:
// the diameter of the ball it turns within.
float body_reach(const RigidWorldDesc& desc, std::uint32_t kind) {
    const RigidWorldDesc::Body& body = desc.bodies[std::min<std::size_t>(kind, desc.bodies.size() - 1U)];
    switch (body.shape) {
    case RigidWorldDesc::Shape::Sphere:
        return 1.0F;
    case RigidWorldDesc::Shape::Box:
        return std::numbers::sqrt3_v<float>;
    case RigidWorldDesc::Shape::Hull: {
        float farthest = 0.0F;
        for (const Vec3& point : body.hull_points) {
            farthest = std::max(farthest, dot(point, point));
        }
        return 2.0F * std::sqrt(farthest);
    }
    }
    return 1.0F;
}

// Where `count` bodies spaced `d` apart stack, bottom first, relative to the
// middle of the pile's base.
std::vector<Vec3> pile_lattice(const SpawnPile& m, std::size_t count, float d) {
    std::vector<Vec3> points;
    if (count == 0U) {
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
        s.velocity[i] = Vec3{};
        s.angular_velocity[i] = Vec3{};
        s.spin[i] = 0.0F;
    }
}

void append(EmitterState& s, float age) {
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
    s.spawn_index.push_back(static_cast<std::uint32_t>(s.spawned));
}

// A body that was hit hard enough comes apart. Called after the world has
// stepped, so the contacts it reports are this step's, and before the step's
// births, so every piece takes its slot in birth order.
// - Breaks are taken in birth order, the order the emitter does everything
//   else in, and each body's pieces in the order they are listed.
// - A body breaks only if every one of its pieces fits the emitter's
//   capacity. One that does not stays whole and its pieces are counted as
//   refused, so a body is never half broken.
void break_bodies(Step& step, const Emitter& emitter, const RigidPhysics& physics, RigidWorld& world) {
    EmitterState& s = step.s;
    const std::size_t kinds = physics.bodies.size() + 1U;
    std::vector<const Break*> breaks(kinds, nullptr);
    bool any = false;
    for (const UpdateModule& module : emitter.update) {
        if (const auto* breaking = std::get_if<Break>(&module); breaking != nullptr && breaking->body < kinds) {
            breaks[breaking->body] = breaking;
            any = true;
        }
    }
    if (!any || s.alive() == 0U) {
        return;
    }
    // The hardest this step hit each body, by its slot. Only contacts over
    // the lowest threshold were collected at all, so this is a short list.
    std::vector<float> hit(emitter.capacity, 0.0F);
    for (const RigidContact& contact : world.contacts()) {
        if (contact.a_is == RigidSide::Body && contact.a < hit.size()) {
            hit[contact.a] = std::max(hit[contact.a], contact.speed);
        }
        if (contact.b_is == RigidSide::Body && contact.b < hit.size()) {
            hit[contact.b] = std::max(hit[contact.b], contact.speed);
        }
    }
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
        if (by == nullptr || hit[s.body[i]] < by->threshold) {
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
    // The broken leave their world oldest first, as the dead do.
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        if (gone[i]) {
            world.remove(s.body[i]);
        }
    }
    compact(s, [&gone](std::size_t i) { return gone[i]; });
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
            append(s, parent.age);
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
                                             float density, float friction, float restitution,
                                             const MeshSet& meshes) {
    RigidWorldDesc::Body body;
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
                                        float step_seconds, const std::optional<ContactOptions>& contacts) {
    RigidWorldDesc desc;
    desc.capacity = emitter.capacity;
    desc.step_seconds = step_seconds;
    if (contacts) {
        desc.report_contacts = true;
        desc.contact_min_speed = contacts->min_speed;
        desc.contact_limit = contacts->limit;
    }
    // A Break module needs the contacts to decide what broke, so the world
    // reports them whether the host asked or not, down to the gentlest
    // threshold that breaks anything.
    for (const UpdateModule& module : emitter.update) {
        if (const auto* breaking = std::get_if<Break>(&module)) {
            desc.contact_min_speed =
                desc.report_contacts ? std::min(desc.contact_min_speed, breaking->threshold) : breaking->threshold;
            desc.contact_limit = std::max(desc.contact_limit, emitter.capacity);
            desc.report_contacts = true;
        }
    }
    desc.angular_damping = physics.angular_damping;
    desc.allow_sleep = physics.allow_sleep;
    // Kind 0 is the emitter's own shape and material, then the kinds it
    // lists, in order, so a spawn module's `body` indexes straight into this.
    desc.bodies.clear();
    Result<RigidWorldDesc::Body> own = rigid_body_desc(emitter, physics.shape, {}, physics.density, physics.friction,
                                                       physics.restitution, meshes);
    if (!own) {
        return own.error();
    }
    desc.bodies.push_back(std::move(own).value());
    for (const RigidBodyKind& kind : physics.bodies) {
        Result<RigidWorldDesc::Body> body = rigid_body_desc(emitter, kind.shape, kind.mesh, kind.density,
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
            const auto add = [&](Vec3 center, Vec3 half) {
                desc.statics.push_back(RigidStatic{.kind = RigidStatic::Kind::Box,
                                                   .center = center,
                                                   .half_size = half,
                                                   .friction = box->friction.value,
                                                   .restitution = box->restitution.value});
            };
            if (!box->inside) {
                add(c, h);
            } else {
                // Six walls around the inside, as thick as the box is large.
                const float t = std::max({h.x, h.y, h.z});
                for (const float side : {-1.0F, 1.0F}) {
                    add(c + Vec3{side * (h.x + t), 0.0F, 0.0F}, Vec3{t, h.y + 2.0F * t, h.z + 2.0F * t});
                    add(c + Vec3{0.0F, side * (h.y + t), 0.0F}, Vec3{h.x + 2.0F * t, t, h.z + 2.0F * t});
                    add(c + Vec3{0.0F, 0.0F, side * (h.z + t)}, Vec3{h.x + 2.0F * t, h.y + 2.0F * t, t});
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
      fields_{std::move(fields)} {
    rigid_.resize(effect_.emitters.size());
    has_piles_ = std::any_of(effect_.emitters.begin(), effect_.emitters.end(), [](const Emitter& emitter) {
        return std::any_of(emitter.spawn.begin(), emitter.spawn.end(),
                           [](const SpawnModule& m) { return std::holds_alternative<SpawnPile>(m); });
    });
    has_props_ = std::any_of(effect_.emitters.begin(), effect_.emitters.end(), [](const Emitter& emitter) {
        const RigidPhysics* physics = rigid_physics(emitter);
        return physics != nullptr && !physics->props.empty();
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
    std::vector<RigidEmitter> rigid(effect.emitters.size());
    const float step_seconds = 1.0F / static_cast<float>(effect.simulation_rate);
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        const Emitter& emitter = effect.emitters[i];
        const RigidPhysics* physics = rigid_physics(emitter);
        if (physics == nullptr) {
            continue;
        }
        if (options.rigid == nullptr) {
            return Error{ErrorCode::RigidSolverMissing,
                         "emitter " + quoted(emitter.name) +
                             " has rigid physics, and no rigid solver was given to run it (ajfx::rigid has one)"};
        }
        Result<RigidWorldDesc> desc =
            rigid_world_desc(emitter, *physics, options.meshes, step_seconds, options.contacts);
        if (!desc) {
            return desc.error();
        }
        Result<std::unique_ptr<RigidWorld>> world = options.rigid->create_world(desc.value());
        if (!world) {
            return Error{ErrorCode::RigidSolverFailure,
                         "emitter " + quoted(emitter.name) + "'s rigid physics world: " + world.error().message};
        }
        rigid[i] = RigidEmitter{std::move(desc).value(), std::move(world).value()};
    }
    Simulation simulation{std::move(effect), instance_seed, std::move(options.fields)};
    simulation.solver_ = options.rigid;
    simulation.rigid_ = std::move(rigid);
    // The piles and props need their worlds, which the constructor did not
    // have yet.
    if (simulation.has_piles_ || simulation.has_props_) {
        simulation.reset();
    }
    return simulation;
}

void Simulation::reset(const ParameterProvider* parameters) {
    std::vector<ParameterValue> values;
    detail::resolve_values(effect_, parameters, 0U, values);
    start(values);
}

void Simulation::prepare(const ParameterProvider* parameters) {
    if (!has_piles_ || state_.tick != 0U) {
        return;
    }
    std::vector<ParameterValue> values;
    detail::resolve_values(effect_, parameters, 0U, values);
    if (values != pile_values_) {
        start(values);
    }
}

void Simulation::start(std::span<const ParameterValue> values) {
    state_ = SimulationState{};
    state_.emitters.resize(effect_.emitters.size());
    for (std::size_t i = 0U; i < effect_.emitters.size(); ++i) {
        state_.emitters[i].rate_carry.assign(detail::rate_module_count(effect_.emitters[i]), 0.0);
    }
    rebuild_worlds();
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
    if (has_piles_) {
        for (std::size_t i = 0U; i < effect_.emitters.size(); ++i) {
            prespawn(i, values);
        }
        solver_state_stale_ = std::any_of(rigid_.begin(), rigid_.end(),
                                          [](const RigidEmitter& r) { return r.world != nullptr; });
    }
}

// Each pile's bodies, born at age 0 before step 0 and set up by the init
// modules as any newborn, then put on the pile still and, until released,
// asleep.
void Simulation::prespawn(std::size_t index, std::span<const ParameterValue> values) {
    const Emitter& emitter = effect_.emitters[index];
    EmitterState& s = state_.emitters[index];
    RigidWorld* world = rigid_[index].world.get();
    if (world == nullptr) {
        return;
    }
    Step step{s, Inputs{effect_, values}, Random{hash_mix(instance_seed_, emitter.seed)}, step_seconds_, 0.0, 0.0,
              0U, 0U, &fields_};
    const auto run = [&step](const auto& module) { apply(module, step); };
    for (const SpawnModule& spawn : emitter.spawn) {
        const auto* pile = std::get_if<SpawnPile>(&spawn);
        if (pile == nullptr) {
            continue;
        }
        // A pile is stacked on the reach of the kind it spawns.
        const float reach = body_reach(rigid_[index].desc, pile->body);
        step.first = s.alive();
        for (std::uint32_t c = 0U; c < pile->count; ++c) {
            if (s.alive() < emitter.capacity) {
                append(s, 0.0F);
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
        place_pile(*pile, step, reach);
        add_bodies(s, step.first, emitter.capacity, *world, release_tick(*pile, effect_.simulation_rate) > 0U);
        // Step 0 shows the poses the engine holds (it keeps its own
        // normalized turn), so a held body never moves by a bit.
        for (std::size_t i = step.first; i < step.last; ++i) {
            const RigidBodyState body = world->body(s.body[i]);
            s.position[i] = body.position;
            s.orientation[i] = body.orientation;
        }
    }
}

bool Simulation::held(std::size_t index, std::uint32_t spawn_index) const {
    std::uint64_t first = 0U;
    for (const SpawnModule& spawn : effect_.emitters[index].spawn) {
        if (const auto* pile = std::get_if<SpawnPile>(&spawn)) {
            if (spawn_index >= first && spawn_index < first + pile->count) {
                return state_.tick < release_tick(*pile, effect_.simulation_rate);
            }
            first += pile->count;
        }
    }
    return false;
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
        for (std::size_t p = 0U; p < s.alive() && p < s.body.size(); ++p) {
            rigid.world->add(s.body[p], p < s.body_kind.size() ? s.body_kind[p] : 0U, s.size[p],
                             RigidBodyState{s.position[p], s.orientation[p], s.velocity[p], s.angular_velocity[p]},
                             held(i, s.spawn_index[p]));
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
    rebuild_worlds();
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

void Simulation::step(const ParameterProvider* parameters) {
    detail::resolve_values(effect_, parameters, state_.tick, values_);
    if (has_piles_ && state_.tick == 0U && values_ != pile_values_) {
        start(values_);
    }
    for (std::size_t i = 0U; i < effect_.emitters.size(); ++i) {
        step_emitter(i, values_);
    }
    ++state_.tick;
    solver_state_stale_ = solver_state_stale_ || std::any_of(rigid_.begin(), rigid_.end(), [](const RigidEmitter& r) {
                                                     return r.world != nullptr;
                                                 });
}

void Simulation::advance_to(std::uint64_t target, const ParameterProvider* parameters) {
    while (state_.tick < target) {
        step(parameters);
    }
}

void Simulation::step_emitter(std::size_t index, std::span<const ParameterValue> values) {
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
    for (const UpdateModule& module : emitter.update) {
        if (const auto* rigid = std::get_if<RigidBody>(&module)) {
            step.rigid =
                BodyResponse{rigid->contact_scale, step.in(rigid->rest_speed), step.in(rigid->roundness)};
        }
    }

    // Age, and retire particles whose life is over. A rigid physics body
    // leaves its world with its particle, oldest first.
    for (float& age : s.age) {
        age += step.dt;
    }
    RigidWorld* world = rigid_[index].world.get();
    if (world != nullptr) {
        for (std::size_t i = 0U; i < s.alive(); ++i) {
            if (!(s.age[i] < s.lifetime[i])) {
                world->remove(s.body[i]);
            }
        }
    }
    compact(s, [&s](std::size_t i) { return !(s.age[i] < s.lifetime[i]); });

    step.last = s.alive();
    const auto run = [&step](const auto& module) { apply(module, step); };
    if (world != nullptr) {
        // Piles let go this step wake, in slot order.
        std::uint64_t first = 0U;
        std::vector<std::uint32_t> waking;
        for (const SpawnModule& spawn : emitter.spawn) {
            if (const auto* pile = std::get_if<SpawnPile>(&spawn)) {
                const std::uint64_t release = release_tick(*pile, effect_.simulation_rate);
                if (release > 0U && release == state_.tick) {
                    for (std::size_t i = 0U; i < s.alive(); ++i) {
                        if (s.spawn_index[i] >= first && s.spawn_index[i] < first + pile->count) {
                            waking.push_back(s.body[i]);
                        }
                    }
                }
                first += pile->count;
            }
        }
        if (!waking.empty()) {
            std::sort(waking.begin(), waking.end());
            world->wake(waking);
        }
        const RigidPhysics& physics = *rigid_physics(emitter);
        move_rigid(step, emitter, physics, *world);
        break_bodies(step, emitter, physics, *world);
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
        compact(s, [&s](std::size_t i) { return !(s.age[i] < s.lifetime[i]); });
    }

    // Births, each with the age it has reached by the end of the step.
    const std::size_t first_new = s.alive();
    const auto birth = [&s, &emitter, world](double age, std::uint32_t kind) {
        if (s.alive() < emitter.capacity) {
            append(s, static_cast<float>(age));
            if (world != nullptr) {
                s.body_kind.push_back(kind);
            }
        } else {
            ++s.refused;
        }
        ++s.spawned;
    };
    detail::for_each_birth(emitter, step.in, s.rate_carry, step.start, step.end, rate, birth);

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
    if (world != nullptr) {
        add_bodies(s, step.first, emitter.capacity, *world);
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

void CheckpointRing::seek(Simulation& simulation, std::uint64_t target, const ParameterProvider* parameters) {
    const SimulationState* best = nullptr;
    for (const SimulationState& checkpoint : checkpoints_) {
        if (checkpoint.tick <= target && (best == nullptr || checkpoint.tick > best->tick)) {
            best = &checkpoint;
        }
    }
    const bool keep_current = simulation.tick() <= target && (best == nullptr || simulation.tick() >= best->tick);
    if (!keep_current) {
        if (best != nullptr) {
            simulation.restore(*best);
        } else {
            simulation.reset(parameters);
        }
    }
    simulation.prepare(parameters);
    while (true) {
        if (simulation.tick() % interval_ == 0U) {
            record(simulation);
        }
        if (simulation.tick() >= target) {
            break;
        }
        simulation.step(parameters);
    }
}

} // namespace ajfx
