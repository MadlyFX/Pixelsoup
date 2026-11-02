#pragma once

#include <ajfx/modules.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <type_traits>
#include <variant>
#include <vector>

// Curves over a particle's life, shared by the CPU reference and the cache
// (cache.cpp), which works a cached particle's colour and size out again from
// its age rather than storing them: both must run this exact code for the
// two to agree to the bit.
namespace ajfx::detail {

[[nodiscard]] inline float mix(float a, float b, float t) {
    return a + (b - a) * t;
}

[[nodiscard]] inline Color times(Color a, Color b) {
    return {a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a};
}

template <class Key>
[[nodiscard]] auto sample(const std::vector<Key>& keys, float t) {
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

[[nodiscard]] inline float normalized_age(const EmitterState& s, std::size_t i) {
    return std::clamp(s.age[i] / s.lifetime[i], 0.0F, 1.0F);
}

// Whether particle i is in `scope` (Scope): its spawn source and, for a
// body, its kind.
[[nodiscard]] inline bool scoped(const Scope& scope, const EmitterState& s, std::size_t i) {
    return scope.empty() || in_scope(scope, s.spawn_index[i], i < s.body_kind.size() ? s.body_kind[i] : 0U);
}

inline void color_over_life(const ColorOverLife& m, EmitterState& s, std::size_t first, std::size_t last) {
    for (std::size_t i = first; i < last; ++i) {
        if (scoped(m.only, s, i)) {
            s.color[i] = times(s.base_color[i], sample(m.keys, normalized_age(s, i)));
        }
    }
}

inline void size_over_life(const SizeOverLife& m, EmitterState& s, std::size_t first, std::size_t last) {
    for (std::size_t i = first; i < last; ++i) {
        if (scoped(m.only, s, i)) {
            s.size[i] = s.base_size[i] * sample(m.keys, normalized_age(s, i));
        }
    }
}

// A heat's glow at `age` (Heat): the black-body-like ramp of start *
// 2^(-age / half_life), times the glow. Operation for operation as ajfx_heat
// (shaders/ajfx_particles.hlsli) works it out.
[[nodiscard]] inline Color heat_glow(const Heat& m, float age) {
    const float heat = m.start * std::exp2(-(age * (1.0F / m.half_life)));
    const float t = std::clamp(heat, 0.0F, 1.0F);
    return Color{std::clamp(3.0F * t, 0.0F, 1.0F) * m.glow, std::clamp(3.0F * t - 1.0F, 0.0F, 1.0F) * m.glow,
                 std::clamp(3.0F * t - 2.0F, 0.0F, 1.0F) * m.glow, 0.0F};
}

[[nodiscard]] inline bool glows(const Emitter& emitter) {
    return std::any_of(emitter.update.begin(), emitter.update.end(),
                       [](const UpdateModule& module) { return std::holds_alternative<Heat>(module); });
}

// The colour of a glowing emitter's particles [first, last): their colour
// over life worked out afresh from their base colour, and then every heat in
// scope added on top, so a glow is never added to last step's glow. Nothing
// for an emitter without a Heat module, whose colour this leaves exactly as
// it was.
inline void glow(const Emitter& emitter, EmitterState& s, std::size_t first, std::size_t last) {
    if (!glows(emitter)) {
        return;
    }
    for (std::size_t i = first; i < last; ++i) {
        s.color[i] = s.base_color[i];
    }
    for (const UpdateModule& module : emitter.update) {
        if (const auto* color = std::get_if<ColorOverLife>(&module)) {
            color_over_life(*color, s, first, last);
        }
    }
    for (const UpdateModule& module : emitter.update) {
        const auto* heat = std::get_if<Heat>(&module);
        if (heat == nullptr) {
            continue;
        }
        for (std::size_t i = first; i < last; ++i) {
            if (scoped(heat->only, s, i)) {
                const Color add = heat_glow(*heat, s.age[i]);
                s.color[i] = Color{s.color[i].r + add.r, s.color[i].g + add.g, s.color[i].b + add.b, s.color[i].a};
            }
        }
    }
}

} // namespace ajfx::detail
