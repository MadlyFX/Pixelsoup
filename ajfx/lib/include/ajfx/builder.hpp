#pragma once

#include <ajfx/effect.hpp>

#include <cstdint>
#include <deque>
#include <string>

// Building effects in code:
//
//     ajfx::EffectBuilder builder{"embers"};
//     builder.float_parameter("rate", 200.0F, 0.0F, 1000.0F);
//     builder.emitter("sparks")
//         .capacity(4096)
//         .spawn(ajfx::SpawnRate{.rate = ajfx::bind("rate")})
//         .init(ajfx::Lifetime{.min = 1.0F, .max = 2.0F})
//         .init(ajfx::Velocity{.speed_min = 2.0F, .speed_max = 4.0F, .spread = 25.0F})
//         .update(ajfx::Gravity{})
//         .update(ajfx::CurlNoise{.strength = 2.0F});
//     ajfx::Result<ajfx::Effect> effect = builder.build();
//
// Modules added without an id get the next free one in their emitter.
namespace ajfx {

class EmitterBuilder final {
public:
    explicit EmitterBuilder(std::string name);

    EmitterBuilder& seed(std::uint32_t seed);
    EmitterBuilder& capacity(std::uint32_t capacity);
    EmitterBuilder& spawn(SpawnModule module);
    EmitterBuilder& init(InitModule module);
    EmitterBuilder& update(UpdateModule module);
    EmitterBuilder& renderer(SpriteRenderer renderer);

    [[nodiscard]] const Emitter& emitter() const noexcept { return emitter_; }

private:
    Emitter emitter_;
    ModuleId last_id_{0U};
};

class EffectBuilder final {
public:
    explicit EffectBuilder(std::string name);

    EffectBuilder& simulation_rate(std::uint32_t steps_per_second);
    EffectBuilder& float_parameter(std::string name, float default_value, float min, float max);
    EffectBuilder& vec3_parameter(std::string name, Vec3 default_value, float min, float max);
    EffectBuilder& color_parameter(std::string name, Color default_value, float min = 0.0F, float max = 1.0F);

    // The emitter with this name, added if it is new. The reference stays
    // valid for the builder's lifetime.
    EmitterBuilder& emitter(std::string name);

    // The finished effect, validated.
    [[nodiscard]] Result<Effect> build() const;

private:
    Effect effect_;
    std::deque<EmitterBuilder> emitters_;
};

} // namespace ajfx
