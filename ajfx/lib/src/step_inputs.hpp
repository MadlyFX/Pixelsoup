#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

inline std::size_t rate_module_count(const Emitter& emitter) {
    return static_cast<std::size_t>(std::count_if(emitter.spawn.begin(), emitter.spawn.end(), [](const SpawnModule& m) {
        return std::holds_alternative<SpawnRate>(m);
    }));
}

// Calls `birth(age)` for every particle the emitter's spawn stack produces in
// the step [start, end), in stack order, with the age each has reached by the
// end of the step. `rate_carry` holds one fractional count per SpawnRate.
template <class Birth>
void for_each_birth(const Emitter& emitter, const Inputs& in, std::vector<double>& rate_carry, double start, double end,
                    double steps_per_second, Birth&& birth) {
    std::size_t rate_slot = 0U;
    for (const SpawnModule& module : emitter.spawn) {
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
                birth((1.0 - fraction) / steps_per_second);
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
                        birth(end - at);
                    }
                }
                if (!repeats) {
                    break;
                }
            }
        }
    }
}

} // namespace ajfx::detail
