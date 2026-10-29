#include <ajfx/simulation.hpp>

#include <ajfx/random.hpp>

#include "noise.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <type_traits>
#include <utility>
#include <variant>

namespace ajfx {
namespace {

// Size of a particle with no size module, in world units.
constexpr float default_size = 0.1F;

std::size_t parameter_index(const Effect& effect, std::string_view name) {
    for (std::size_t i = 0U; i < effect.parameters.size(); ++i) {
        if (effect.parameters[i].name == name) {
            return i;
        }
    }
    return 0U; // unreachable for a validated effect
}

// Module inputs resolved against this step's parameter values.
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

float mix(float a, float b, float t) {
    return a + (b - a) * t;
}

Vec3 normalized(Vec3 v) {
    const float length_squared = dot(v, v);
    return length_squared > 0.0F ? v * (1.0F / std::sqrt(length_squared)) : Vec3{};
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
    s.spawn_index.resize(kept);
}

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
    const float radius = step.in(m.radius);
    const Vec3 size = m.size;
    for (std::size_t i = step.first; i < step.last; ++i) {
        const std::uint32_t index = step.s.spawn_index[i];
        const auto u = [&](std::uint32_t draw) { return step.random(index, m.id, draw); };
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
    const float lo = step.in(m.speed_min);
    const float hi = step.in(m.speed_max);
    for (std::size_t i = step.first; i < step.last; ++i) {
        const std::uint32_t index = step.s.spawn_index[i];
        const float cos_theta = 1.0F - step.random(index, m.id, 0U) * (1.0F - cos_spread);
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
    for (std::size_t i = step.first; i < step.last; ++i) {
        const float t = step.random(step.s.spawn_index[i], m.id, 0U);
        step.s.base_color[i] = {mix(a.r, b.r, t), mix(a.g, b.g, t), mix(a.b, b.b, t), mix(a.a, b.a, t)};
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

// ------------------------------------------------------------ forces

constexpr bool is_force(const UpdateModule& module) {
    return !std::holds_alternative<PlaneCollision>(module) && !std::holds_alternative<ColorOverLife>(module) &&
           !std::holds_alternative<SizeOverLife>(module);
}

void apply(const Gravity& m, Step& step) {
    const Vec3 dv = step.in(m.acceleration) * step.dt;
    for (std::size_t i = step.first; i < step.last; ++i) {
        step.s.velocity[i] += dv;
    }
}

void apply(const Drag& m, Step& step) {
    const float keep = 1.0F / (1.0F + step.in(m.coefficient) * step.dt);
    for (std::size_t i = step.first; i < step.last; ++i) {
        step.s.velocity[i] = step.s.velocity[i] * keep;
    }
}

void apply(const CurlNoise& m, Step& step) {
    const float frequency = step.in(m.frequency);
    const float scale = step.in(m.strength) * step.dt;
    const Vec3 scroll{0.0F, 0.0F, static_cast<float>(step.start) * step.in(m.evolution)};
    for (std::size_t i = step.first; i < step.last; ++i) {
        step.s.velocity[i] += detail::curl_noise(step.s.position[i] * frequency + scroll, m.seed) * scale;
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
            step.s.velocity[i] += cross(axis, radial) * (scale / length);
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
        step.s.velocity[i] += d * (strength * falloff * step.dt / distance);
    }
}

// ------------------------------------------------------------ constraints and appearance

void apply(const PlaneCollision& m, Step& step) {
    const Vec3 n = normalized(m.normal);
    const float restitution = step.in(m.restitution);
    const float slide = 1.0F - step.in(m.friction);
    for (std::size_t i = step.first; i < step.last; ++i) {
        const float distance = dot(n, step.s.position[i]) - m.offset;
        if (distance >= 0.0F) {
            continue;
        }
        if (m.kill) {
            // Removed by the compaction that follows the update stack.
            step.s.age[i] = std::numeric_limits<float>::infinity();
            continue;
        }
        step.s.position[i] -= n * distance;
        const float into = dot(step.s.velocity[i], n);
        if (into < 0.0F) {
            const Vec3 normal = n * into;
            step.s.velocity[i] = (step.s.velocity[i] - normal) * slide - normal * restitution;
        }
    }
}

float normalized_age(const EmitterState& s, std::size_t i) {
    return std::clamp(s.age[i] / s.lifetime[i], 0.0F, 1.0F);
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
    s.spawn_index.push_back(static_cast<std::uint32_t>(s.spawned));
}

} // namespace

Simulation::Simulation(Effect effect, std::uint32_t instance_seed)
    : effect_{std::move(effect)},
      instance_seed_{instance_seed},
      step_seconds_{1.0F / static_cast<float>(effect_.simulation_rate)} {
    reset();
}

Result<Simulation> Simulation::create(Effect effect, std::uint32_t instance_seed) {
    Result<bool> valid = validate(effect);
    if (!valid) {
        return valid.error();
    }
    return Simulation{std::move(effect), instance_seed};
}

void Simulation::reset() {
    state_ = SimulationState{};
    state_.emitters.resize(effect_.emitters.size());
    for (std::size_t i = 0U; i < effect_.emitters.size(); ++i) {
        const auto& spawn = effect_.emitters[i].spawn;
        const auto rates = std::count_if(spawn.begin(), spawn.end(),
                                         [](const SpawnModule& m) { return std::holds_alternative<SpawnRate>(m); });
        state_.emitters[i].rate_carry.assign(static_cast<std::size_t>(rates), 0.0);
    }
}

void Simulation::step(const ParameterProvider* parameters) {
    values_.resize(effect_.parameters.size());
    for (std::size_t i = 0U; i < values_.size(); ++i) {
        values_[i] = effect_.parameters[i].default_value;
    }
    if (parameters != nullptr) {
        parameters->values_at(state_.tick, values_);
    }
    for (std::size_t i = 0U; i < values_.size(); ++i) {
        for (float& component : values_[i]) {
            component = std::clamp(component, effect_.parameters[i].min, effect_.parameters[i].max);
        }
    }
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
              0U};

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
    std::size_t rate_slot = 0U;
    for (const SpawnModule& module : emitter.spawn) {
        if (const SpawnRate* spawn_rate = std::get_if<SpawnRate>(&module)) {
            double& carry = s.rate_carry[rate_slot++];
            const double births = std::max(0.0, static_cast<double>(step.in(spawn_rate->rate))) / rate;
            if (births <= 0.0) {
                continue;
            }
            const double total = carry + births;
            const double count = std::floor(total);
            // The m-th birth happens when the running total crosses m + 1.
            for (double m = 0.0; m < count; m += 1.0) {
                const double fraction = (m + 1.0 - carry) / births;
                birth((1.0 - fraction) / rate);
            }
            carry = total - count;
        } else if (const SpawnBurst* burst = std::get_if<SpawnBurst>(&module)) {
            const double time = burst->time;
            const double interval = burst->interval;
            const bool repeats = interval > 0.0 && burst->cycles != 1U;
            std::uint64_t k = 0U;
            if (repeats && time < step.start) {
                k = static_cast<std::uint64_t>(std::ceil((step.start - time) / interval));
                // Correct the division's rounding in either direction.
                while (k > 0U && time + static_cast<double>(k - 1U) * interval >= step.start) {
                    --k;
                }
                while (time + static_cast<double>(k) * interval < step.start) {
                    ++k;
                }
            }
            for (;; ++k) {
                if (k > 0U && (!repeats || (burst->cycles != 0U && k >= burst->cycles))) {
                    break;
                }
                const double at = time + static_cast<double>(k) * interval;
                if (at >= step.end) {
                    break;
                }
                if (at >= step.start) {
                    for (std::uint32_t c = 0U; c < burst->count; ++c) {
                        birth(step.end - at);
                    }
                }
                if (!repeats) {
                    break;
                }
            }
        }
    }

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
