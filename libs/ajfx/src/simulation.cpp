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
        }
        ++kept;
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

// How colliders meet an emitter's particles when it has a RigidBody.
struct RigidContact final {
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
    std::optional<RigidContact> rigid{};
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

} // namespace

Simulation::Simulation(Effect effect, std::uint32_t instance_seed, FieldSet fields)
    : effect_{std::move(effect)},
      instance_seed_{instance_seed},
      step_seconds_{1.0F / static_cast<float>(effect_.simulation_rate)},
      fields_{std::move(fields)} {
    reset();
}

Result<Simulation> Simulation::create(Effect effect, std::uint32_t instance_seed, FieldSet fields) {
    Result<bool> valid = validate(effect);
    if (!valid) {
        return valid.error();
    }
    if (Result<bool> held = check_fields(effect, fields); !held) {
        return held.error();
    }
    return Simulation{std::move(effect), instance_seed, std::move(fields)};
}

void Simulation::reset() {
    state_ = SimulationState{};
    state_.emitters.resize(effect_.emitters.size());
    for (std::size_t i = 0U; i < effect_.emitters.size(); ++i) {
        state_.emitters[i].rate_carry.assign(detail::rate_module_count(effect_.emitters[i]), 0.0);
    }
}

void Simulation::step(const ParameterProvider* parameters) {
    detail::resolve_values(effect_, parameters, state_.tick, values_);
    for (std::size_t i = 0U; i < effect_.emitters.size(); ++i) {
        step_emitter(i, values_);
    }
    ++state_.tick;
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
                RigidContact{rigid->contact_scale, step.in(rigid->rest_speed), step.in(rigid->roundness)};
        }
    }

    // Age, and retire particles whose life is over.
    for (float& age : s.age) {
        age += step.dt;
    }
    compact(s, [&s](std::size_t i) { return !(s.age[i] < s.lifetime[i]); });

    // Forces, integration, then constraints and appearance.
    step.last = s.alive();
    const auto run = [&step](const auto& module) { apply(module, step); };
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

    // Births, each with the age it has reached by the end of the step.
    const std::size_t first_new = s.alive();
    const auto birth = [&s, &emitter](double age) {
        if (s.alive() < emitter.capacity) {
            append(s, static_cast<float>(age));
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
            simulation.reset();
        }
    }
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
