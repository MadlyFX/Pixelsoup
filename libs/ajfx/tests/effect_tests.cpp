#include "check.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/effect.hpp>
#include <ajfx/presets.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <variant>

namespace {

using namespace ajfx;

// A small valid effect the failure cases each break in one way.
EffectBuilder valid_builder() {
    EffectBuilder builder{"test"};
    builder.float_parameter("rate", 50.0F, 0.0F, 100.0F);
    builder.emitter("main")
        .capacity(1000U)
        .spawn(SpawnRate{.rate = bind("rate")})
        .init(Lifetime{.min = 1.0F, .max = 2.0F})
        .update(Gravity{});
    return builder;
}

bool fails_with(const Result<Effect>& result, ErrorCode code, const std::string& fragment) {
    return !result.ok() && result.error().code == code && result.error().message.find(fragment) != std::string::npos;
}

void builder_produces_a_validated_effect_with_ids() {
    const Result<Effect> effect = valid_builder().build();
    AJFX_CHECK(effect.ok());
    const Emitter& emitter = effect.value().emitters.at(0);
    AJFX_CHECK(std::get<SpawnRate>(emitter.spawn[0]).id == 1U);
    AJFX_CHECK(std::get<Lifetime>(emitter.init[0]).id == 2U);
    AJFX_CHECK(std::get<Gravity>(emitter.update[0]).id == 3U);
    AJFX_CHECK(std::get<SpawnRate>(emitter.spawn[0]).rate.parameter == "rate");
}

void builder_keeps_explicit_ids_and_continues_after_them() {
    EffectBuilder builder{"ids"};
    builder.emitter("main")
        .spawn(SpawnBurst{.id = 40U})
        .init(Lifetime{})
        .init(InitSize{.id = 7U});
    const Result<Effect> effect = builder.build();
    AJFX_CHECK(effect.ok());
    const Emitter& emitter = effect.value().emitters[0];
    AJFX_CHECK(std::get<SpawnBurst>(emitter.spawn[0]).id == 40U);
    AJFX_CHECK(std::get<Lifetime>(emitter.init[0]).id == 41U);
    AJFX_CHECK(std::get<InitSize>(emitter.init[1]).id == 7U);
}

void unknown_and_mistyped_bindings_are_refused() {
    EffectBuilder unknown = valid_builder();
    unknown.emitter("second").init(InitSize{.min = bind("missing")});
    AJFX_CHECK(fails_with(unknown.build(), ErrorCode::UnknownParameter, "'missing'"));

    EffectBuilder mistyped = valid_builder();
    mistyped.emitter("second").update(Gravity{.acceleration = bind("rate")});
    AJFX_CHECK(fails_with(mistyped.build(), ErrorCode::ParameterTypeMismatch, "'acceleration'"));
}

void structural_problems_are_refused_with_the_place_named() {
    Effect duplicate = valid_builder().build().value();
    duplicate.emitters.push_back(duplicate.emitters[0]);
    const Result<bool> twice = validate(duplicate);
    AJFX_CHECK(!twice.ok() && twice.error().message.find("'main' is declared twice") != std::string::npos);

    EffectBuilder no_lifetime{"x"};
    no_lifetime.emitter("e").spawn(SpawnRate{});
    AJFX_CHECK(fails_with(no_lifetime.build(), ErrorCode::InvalidEffect, "no 'lifetime' module"));

    EffectBuilder zero_capacity = valid_builder();
    zero_capacity.emitter("empty").capacity(0U);
    AJFX_CHECK(fails_with(zero_capacity.build(), ErrorCode::InvalidEffect, "capacity"));

    EffectBuilder not_finite = valid_builder();
    not_finite.emitter("nan").init(InitSize{.min = std::numeric_limits<float>::quiet_NaN()});
    AJFX_CHECK(fails_with(not_finite.build(), ErrorCode::InvalidEffect, "'min' is not a finite number"));

    EffectBuilder bad_keys = valid_builder();
    bad_keys.emitter("keys").update(SizeOverLife{.keys = {{0.5F, 1.0F}, {0.2F, 2.0F}}});
    AJFX_CHECK(fails_with(bad_keys.build(), ErrorCode::InvalidEffect, "'keys'"));

    EffectBuilder bad_default{"x"};
    bad_default.float_parameter("p", 5.0F, 0.0F, 1.0F);
    AJFX_CHECK(fails_with(bad_default.build(), ErrorCode::InvalidEffect, "default is outside its range"));

    EffectBuilder wide_spread = valid_builder();
    wide_spread.emitter("cone").init(Velocity{.spread = 190.0F});
    AJFX_CHECK(fails_with(wide_spread.build(), ErrorCode::InvalidEffect, "'spread'"));

    EffectBuilder slow{"x"};
    slow.simulation_rate(0U);
    AJFX_CHECK(fails_with(slow.build(), ErrorCode::InvalidEffect, "simulation rate"));

    EffectBuilder unturned = valid_builder();
    unturned.emitter("chips").init(InitTumble{});
    AJFX_CHECK(fails_with(unturned.build(), ErrorCode::InvalidEffect, "no 'rigid_body' or 'rigid_physics' module"));

    EffectBuilder twice_rigid = valid_builder();
    twice_rigid.emitter("chips").update(RigidBody{}).update(RigidBody{});
    AJFX_CHECK(fails_with(twice_rigid.build(), ErrorCode::InvalidEffect, "more than one 'rigid_body'"));

    EffectBuilder sinking = valid_builder();
    sinking.emitter("chips").update(RigidBody{.rest_speed = -1.0F});
    AJFX_CHECK(fails_with(sinking.build(), ErrorCode::InvalidEffect, "'rest_speed'"));

    EffectBuilder aimed = valid_builder();
    aimed.emitter("chips")
        .update(RigidBody{})
        .renderer(SpriteRenderer{.facing = Facing::Velocity, .geometry = Geometry::Mesh, .mesh = "chip"});
    AJFX_CHECK(fails_with(aimed.build(), ErrorCode::InvalidEffect, "no velocity facing"));
}

void budget_uses_the_top_of_each_bound_range() {
    // rate up to 100/s, lifetime up to 2 s: ceil(200) + 2 = 202.
    const Result<Effect> effect = valid_builder().build();
    AJFX_CHECK(particle_bound(effect.value(), effect.value().emitters[0]) == 202U);

    EffectBuilder tight{"tight"};
    tight.float_parameter("rate", 50.0F, 0.0F, 100.0F);
    tight.emitter("main")
        .capacity(201U)
        .spawn(SpawnRate{.rate = bind("rate")})
        .init(Lifetime{.min = 1.0F, .max = 2.0F});
    AJFX_CHECK(fails_with(tight.build(), ErrorCode::BudgetExceeded, "up to 202 particles alive but its capacity is 201"));

    tight.emitter("main").capacity(202U);
    AJFX_CHECK(tight.build().ok());
}

void repeating_bursts_count_overlapping_lives() {
    EffectBuilder builder{"bursts"};
    builder.emitter("b")
        .capacity(100000U)
        .spawn(SpawnBurst{.time = 0.0F, .count = 100U, .interval = 0.5F, .cycles = 0U})
        .spawn(SpawnBurst{.time = 1.0F, .count = 7U})
        .spawn(SpawnBurst{.time = 0.0F, .count = 9U, .interval = 0.1F, .cycles = 3U})
        .init(Lifetime{.min = 1.2F, .max = 1.2F});
    const Result<Effect> effect = builder.build();
    AJFX_CHECK(effect.ok());
    // floor(1.2 / 0.5) + 1 = 3 bursts of 100; one of 7; three of 9 (capped by cycles).
    AJFX_CHECK(particle_bound(effect.value(), effect.value().emitters[0]) == 300U + 7U + 27U);
}

void attributes_follow_the_modules() {
    EffectBuilder builder{"attributes"};
    builder.emitter("plain").init(Lifetime{});
    builder.emitter("rich")
        .init(Lifetime{})
        .init(InitColor{})
        .init(InitRotation{})
        .update(SizeOverLife{})
        .update(Drag{});
    builder.emitter("facing").renderer(SpriteRenderer{.facing = Facing::Velocity});
    builder.emitter("rigid").update(RigidBody{});
    const Result<Effect> effect = builder.build();
    AJFX_CHECK(effect.ok());

    const std::uint32_t plain = required_attributes(effect.value().emitters[0]);
    AJFX_CHECK(has_attribute(plain, Attribute::Position) && has_attribute(plain, Attribute::Age) &&
               has_attribute(plain, Attribute::Lifetime));
    AJFX_CHECK(!has_attribute(plain, Attribute::Velocity) && !has_attribute(plain, Attribute::Color) &&
               !has_attribute(plain, Attribute::Size) && !has_attribute(plain, Attribute::Rotation));

    const std::uint32_t rich = required_attributes(effect.value().emitters[1]);
    AJFX_CHECK(has_attribute(rich, Attribute::Color) && has_attribute(rich, Attribute::Rotation) &&
               has_attribute(rich, Attribute::Size) && has_attribute(rich, Attribute::Velocity));

    AJFX_CHECK(has_attribute(required_attributes(effect.value().emitters[2]), Attribute::Velocity));
    AJFX_CHECK(!has_attribute(rich, Attribute::Orientation) && !has_attribute(rich, Attribute::AngularVelocity));

    const std::uint32_t rigid = required_attributes(effect.value().emitters[3]);
    AJFX_CHECK(has_attribute(rigid, Attribute::Orientation) && has_attribute(rigid, Attribute::AngularVelocity) &&
               has_attribute(rigid, Attribute::Velocity));
}

void host_frames_map_onto_steps_exactly() {
    AJFX_CHECK(tick_at(24U, 24U, 1U, 60U) == 60U);
    AJFX_CHECK(tick_at(1U, 24U, 1U, 60U) == 2U);   // 2.5 steps, floored
    AJFX_CHECK(tick_at(25U, 24U, 1U, 60U) == 62U); // 62.5
    // 23.976 fps: 24000 frames are 1001 seconds.
    AJFX_CHECK(tick_at(24000U, 24000U, 1001U, 60U) == 60060U);
    AJFX_CHECK(tick_at(24001U, 24000U, 1001U, 60U) == 60062U); // 60062.5025
    // A day of 60 fps frames at 240 steps per second.
    AJFX_CHECK(tick_at(5184000U, 60U, 1U, 240U) == 20736000U);
}

void presets_are_valid() {
    for (const Effect& effect : {presets::sparks(), presets::smoke(), presets::confetti(), presets::debris()}) {
        AJFX_CHECK(!effect.name.empty());
        AJFX_CHECK(validate(effect).ok());
        AJFX_CHECK(!effect.parameters.empty());
    }
}

} // namespace

int main() {
    const ajfx::test::Case cases[] = {
        {"builder_produces_a_validated_effect_with_ids", builder_produces_a_validated_effect_with_ids},
        {"builder_keeps_explicit_ids_and_continues_after_them", builder_keeps_explicit_ids_and_continues_after_them},
        {"unknown_and_mistyped_bindings_are_refused", unknown_and_mistyped_bindings_are_refused},
        {"structural_problems_are_refused_with_the_place_named",
         structural_problems_are_refused_with_the_place_named},
        {"budget_uses_the_top_of_each_bound_range", budget_uses_the_top_of_each_bound_range},
        {"repeating_bursts_count_overlapping_lives", repeating_bursts_count_overlapping_lives},
        {"attributes_follow_the_modules", attributes_follow_the_modules},
        {"host_frames_map_onto_steps_exactly", host_frames_map_onto_steps_exactly},
        {"presets_are_valid", presets_are_valid},
    };
    return ajfx::test::run(cases);
}
