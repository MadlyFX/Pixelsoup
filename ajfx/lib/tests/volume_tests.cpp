#include "check.hpp"

#include <ajfx/builder.hpp>
#include <ajfx/cache.hpp>
#include <ajfx/package.hpp>
#include <ajfx/simulation.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace ajfx;

template <class T>
bool fails_with(const Result<T>& result, ErrorCode code, const std::string& fragment) {
    return !result.ok() && result.error().code == code && result.error().message.find(fragment) != std::string::npos;
}

Simulation make(const Effect& effect, std::uint32_t seed = 0U) {
    Result<Simulation> simulation = Simulation::create(effect, seed);
    AJFX_CHECK(simulation.ok());
    return std::move(simulation).value();
}

// The air above a fire: a grid two units across and three high, standing on
// y = 0, with every setting away from its default.
Volume air(float vorticity = 0.0F) {
    return Volume{.name = "air",
                  .origin = Vec3{-1.0F, 0.0F, -1.0F},
                  .cell_size = 0.125F,
                  .nx = 16U,
                  .ny = 24U,
                  .nz = 16U,
                  .buoyancy = 2.0F,
                  .weight = 0.1F,
                  .vorticity = vorticity,
                  .pressure_iterations = 12U,
                  .density_dissipation = 0.2F,
                  .temperature_dissipation = 0.5F,
                  .velocity_dissipation = 0.1F,
                  .extinction = 1.5F,
                  .albedo = 0.7F,
                  .color = Vec3{0.9F, 0.8F, 0.7F}};
}

// A fire feeding the air with smoke, heat and a little of its own rise, and
// motes above it carried by the air. Every module is one the GPU reproduces
// to the bit (no cones, spheres or curves), so the grids may be compared
// exactly.
Effect plume(float vorticity = 0.0F, bool motes_first = false) {
    EffectBuilder builder{"plume"};
    builder.volume(air(vorticity));
    const auto fire = [&builder] {
        builder.emitter("fire")
            .capacity(2000U)
            .seed(3U)
            .spawn(SpawnRate{.rate = 300.0F})
            .init(Lifetime{.min = 0.4F, .max = 0.8F})
            .init(ShapeLocation{.shape = Shape::Box, .center = Vec3{0.0F, 0.2F, 0.0F}, .size = Vec3{0.5F, 0.1F, 0.5F}})
            .init(Velocity{.direction = Vec3{0.0F, 1.0F, 0.0F}, .speed_min = 0.5F, .speed_max = 1.0F})
            .update(VolumeSource{.volume = "air", .density = 4.0F, .temperature = 3.0F, .velocity = 0.5F});
    };
    const auto motes = [&builder] {
        builder.emitter("motes")
            .capacity(512U)
            .seed(5U)
            .spawn(SpawnBurst{.count = 256U})
            .init(Lifetime{.min = 10.0F, .max = 10.0F})
            .init(ShapeLocation{.shape = Shape::Box, .center = Vec3{0.0F, 1.5F, 0.0F}, .size = Vec3{1.6F, 1.0F, 1.6F}})
            .update(VolumeAdvect{.volume = "air", .strength = 6.0F, .scale_min = 0.5F, .scale_max = 1.5F});
    };
    if (motes_first) {
        motes();
        fire();
    } else {
        fire();
        motes();
    }
    Result<Effect> effect = builder.build();
    if (!effect) {
        throw std::runtime_error{effect.error().message};
    }
    return std::move(effect).value();
}

float total(const std::vector<float>& values) {
    float sum = 0.0F;
    for (const float v : values) {
        sum += v;
    }
    return sum;
}

// The density-weighted height of the grid's smoke, in cells.
float smoke_height(const VolumeState& v) {
    float weight = 0.0F;
    float moment = 0.0F;
    for (std::uint32_t z = 0U; z < v.nz; ++z) {
        for (std::uint32_t y = 0U; y < v.ny; ++y) {
            for (std::uint32_t x = 0U; x < v.nx; ++x) {
                const float d = v.density[(z * v.ny + y) * v.nx + x];
                weight += d;
                moment += d * static_cast<float>(y);
            }
        }
    }
    return weight > 0.0F ? moment / weight : 0.0F;
}

void grids_are_budgeted_and_named() {
    const auto with = [](Volume volume) {
        EffectBuilder b{"b"};
        b.volume(std::move(volume));
        b.emitter("e").spawn(SpawnBurst{}).init(Lifetime{});
        return b.build();
    };
    // Past the cap, with the memory it would have taken.
    Volume huge = air();
    huge.nx = huge.ny = huge.nz = 128U;
    AJFX_CHECK(fails_with(with(huge), ErrorCode::BudgetExceeded,
                          "volume 'air' has 2097152 cells (128x128x128), more than the 1048576 one grid may hold; its "
                          "state alone would need 40 MB"));
    Volume wide = air();
    wide.nx = 300U;
    AJFX_CHECK(fails_with(with(wide), ErrorCode::InvalidEffect, "must be at most 256 cells"));
    Volume thin = air();
    thin.nz = 1U;
    AJFX_CHECK(fails_with(with(thin), ErrorCode::InvalidEffect, "at least 2"));
    Volume flat = air();
    flat.cell_size = 0.0F;
    AJFX_CHECK(fails_with(with(flat), ErrorCode::InvalidEffect, "field 'cell_size' must be positive"));
    Volume slow = air();
    slow.pressure_iterations = 1000U;
    AJFX_CHECK(fails_with(with(slow), ErrorCode::InvalidEffect, "field 'pressure_iterations' must be at most 256"));
    Volume unnamed = air();
    unnamed.name = "Air/1";
    AJFX_CHECK(fails_with(with(unnamed), ErrorCode::InvalidEffect, "a volume's name must be"));
    // Under the cap, the largest grid is accepted.
    Volume largest = air();
    largest.nx = 256U;
    largest.ny = 64U;
    largest.nz = 64U;
    AJFX_CHECK(with(largest).ok());

    EffectBuilder twice{"twice"};
    twice.volume(air()).volume(air());
    twice.emitter("e").spawn(SpawnBurst{}).init(Lifetime{});
    AJFX_CHECK(fails_with(twice.build(), ErrorCode::InvalidEffect, "volume 'air' is declared twice"));

    // The modules name a grid the effect lists.
    EffectBuilder orphan{"orphan"};
    orphan.volume(air());
    orphan.emitter("e").spawn(SpawnBurst{}).init(Lifetime{}).update(VolumeSource{.volume = "smoke"});
    AJFX_CHECK(fails_with(orphan.build(), ErrorCode::InvalidEffect,
                          "module 'volume_source' #3 field 'volume' names volume 'smoke', which this effect has none of"));
    EffectBuilder lost{"lost"};
    lost.emitter("e").spawn(SpawnBurst{}).init(Lifetime{}).update(VolumeAdvect{.volume = "air"});
    AJFX_CHECK(fails_with(lost.build(), ErrorCode::InvalidEffect, "field 'volume' names volume 'air'"));
    // Bodies are moved by their engine, not by a grid.
    EffectBuilder rigid{"rigid"};
    rigid.volume(air());
    rigid.emitter("e").spawn(SpawnBurst{}).init(Lifetime{}).update(RigidPhysics{}).update(VolumeAdvect{.volume = "air"});
    AJFX_CHECK(fails_with(rigid.build(), ErrorCode::InvalidEffect, "cannot act on rigid physics particles"));
    // The deposits run after integration; the carrying is a force.
    AJFX_CHECK(!is_force(UpdateModule{VolumeSource{}}) && is_force(UpdateModule{VolumeAdvect{}}));
}

void fire_feeds_the_grid_and_the_grid_rises() {
    const Effect effect = plume();
    Simulation simulation = make(effect, 2U);
    simulation.advance_to(30U);
    const VolumeState early = simulation.volume(0);
    AJFX_CHECK(early.nx == 16U && early.ny == 24U && early.nz == 16U && early.cells() == 16U * 24U * 16U);
    AJFX_CHECK(total(early.density) > 0.0F && total(early.temperature) > 0.0F);
    const float low = smoke_height(early);
    simulation.advance_to(150U);
    const VolumeState later = simulation.volume(0);
    // Hot smoke rises: its centre climbs, and the air above the fire moves
    // up. Every number stays finite under the dissipation.
    AJFX_CHECK(smoke_height(later) > low + 1.0F);
    float fastest_up = 0.0F;
    for (const Vec3& v : later.velocity) {
        AJFX_CHECK(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z));
        fastest_up = std::max(fastest_up, v.y);
    }
    AJFX_CHECK(fastest_up > 0.5F);
    for (const float d : later.density) {
        AJFX_CHECK(std::isfinite(d) && d >= 0.0F);
    }
    // The motes are carried: those over the fire have risen and move up.
    const EmitterState& motes = simulation.emitter(1);
    AJFX_CHECK(motes.alive() == 256U);
    std::size_t carried = 0U;
    for (std::size_t i = 0U; i < motes.alive(); ++i) {
        carried += motes.velocity[i].y > 0.05F ? 1U : 0U;
    }
    AJFX_CHECK(carried > 20U);
    // Deposits land only in the grid: the fire's births are in it, so every
    // step adds smoke, and nothing is refused.
    AJFX_CHECK(simulation.emitter(0).refused == 0U && simulation.emitter(0).alive() > 100U);
}

void reruns_are_bit_identical_and_seeds_differ() {
    const Effect effect = plume(0.4F);
    Simulation a = make(effect, 7U);
    Simulation b = make(effect, 7U);
    Simulation c = make(effect, 8U);
    a.advance_to(120U);
    b.advance_to(120U);
    c.advance_to(120U);
    AJFX_CHECK(a.state() == b.state());
    AJFX_CHECK(a.volume(0).density != c.volume(0).density);
    AJFX_CHECK(a.state().volumes.size() == 1U);
}

void deposits_sum_the_same_in_any_order() {
    // The same fire and motes listed either way round: the fire's deposits
    // are summed as integers, so the grid, and the motes it carries, are the
    // same to the bit whichever emitter steps first.
    Simulation first = make(plume(0.0F, false), 4U);
    Simulation second = make(plume(0.0F, true), 4U);
    first.advance_to(100U);
    second.advance_to(100U);
    AJFX_CHECK(first.volume(0) == second.volume(0));
    AJFX_CHECK(first.emitter(1) == second.emitter(0));
    AJFX_CHECK(total(first.volume(0).density) > 0.0F);
}

void seeking_lands_on_the_stepped_grid() {
    const Effect effect = plume(0.3F);
    Simulation seeker = make(effect, 9U);
    CheckpointRing ring{40U, 6U};
    for (const std::uint64_t target : {200U, 90U, 170U, 131U, 0U, 60U}) {
        ring.seek(seeker, target);
        Simulation stepped = make(effect, 9U);
        stepped.advance_to(target);
        AJFX_CHECK(seeker.tick() == target);
        AJFX_CHECK(seeker.state() == stepped.state());
        AJFX_CHECK(seeker.volume(0) == stepped.volume(0));
    }
}

void saved_state_carries_the_grids() {
    const Effect effect = plume(0.3F);
    Simulation simulation = make(effect, 1U);
    simulation.advance_to(75U);
    const SimulationState& state = simulation.state();
    const std::vector<std::uint8_t> bytes = write_state(state);
    AJFX_CHECK(write_state(state) == bytes);
    const Result<SimulationState> back = read_state(bytes);
    AJFX_CHECK(back.ok() && back.value() == state);
    AJFX_CHECK(back.value().volumes.size() == 1U && back.value().volumes[0].cells() == 16U * 24U * 16U);
    // Restored into a fresh simulation it carries on exactly.
    Simulation restored = make(effect, 1U);
    restored.restore(back.value());
    simulation.advance_to(140U);
    restored.advance_to(140U);
    AJFX_CHECK(restored.state() == simulation.state());
    // Damaged or cut short, refused.
    std::vector<std::uint8_t> truncated(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() - 100U));
    AJFX_CHECK(fails_with(read_state(truncated), ErrorCode::MalformedCache, "checksum"));
    // A restore from a state without grids starts them empty rather than
    // stepping a grid of no cells.
    SimulationState none = state;
    none.volumes.clear();
    restored.restore(none);
    AJFX_CHECK(restored.volume(0).cells() == 16U * 24U * 16U && total(restored.volume(0).density) == 0.0F);
    restored.step();
}

void vorticity_confinement_stirs_the_flow() {
    Simulation calm = make(plume(0.0F), 3U);
    Simulation stirred = make(plume(0.6F), 3U);
    calm.advance_to(120U);
    stirred.advance_to(120U);
    AJFX_CHECK(calm.volume(0).velocity != stirred.volume(0).velocity);
    for (const Vec3& v : stirred.volume(0).velocity) {
        AJFX_CHECK(std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.y) < 100.0F);
    }
}

void grids_round_trip_through_packages() {
    const Effect effect = plume(0.25F);
    const Result<std::vector<std::uint8_t>> first = write_package(effect);
    AJFX_CHECK(first.ok());
    const Result<Effect> read = read_package(first.value());
    AJFX_CHECK(read.ok() && read.value() == effect);
    AJFX_CHECK(read.value().volumes.size() == 1U && read.value().volumes[0] == air(0.25F));
    const Result<std::vector<std::uint8_t>> second = write_package(read.value());
    AJFX_CHECK(second.ok() && second.value() == first.value());
    const VolumeSource& source = std::get<VolumeSource>(read.value().emitters[0].update[0]);
    AJFX_CHECK(source.volume == "air" && source.density.value == 4.0F && source.temperature.value == 3.0F &&
               source.velocity.value == 0.5F);
    const VolumeAdvect& advect = std::get<VolumeAdvect>(read.value().emitters[1].update[0]);
    AJFX_CHECK(advect.volume == "air" && advect.strength.value == 6.0F && advect.scale_min.value == 0.5F);
    // An effect without volumes says nothing of them.
    EffectBuilder plain{"plain"};
    plain.emitter("e").spawn(SpawnBurst{}).init(Lifetime{});
    AJFX_CHECK(effect_to_json(plain.build().value()).find("volumes") == std::string::npos);
    // A misspelt field of a volume is reported with its path.
    AJFX_CHECK(fails_with(effect_from_json(R"({"format": 9, "name": "x", "volumes": [{"name": "a", "cel_size": 2}]})"),
                          ErrorCode::MalformedPackage, "volumes[0] has unknown field 'cel_size'"));
    AJFX_CHECK(fails_with(effect_from_json(R"({"format": 9, "name": "x", "volumes": {"name": "a"}})"),
                          ErrorCode::MalformedPackage, "volumes must be an array of volumes"));
    // A volume in the JSON with nothing but a name takes the defaults.
    const Result<Effect> defaults = effect_from_json(R"({"format": 9, "name": "x", "volumes": [{"name": "a"}]})");
    AJFX_CHECK(defaults.ok() && defaults.value().volumes.size() == 1U && defaults.value().volumes[0].nx == 16U);
}

void caches_keep_grids_out_of_cached_emitters() {
    // The fire feeds the grid, so it cannot be cached: a cache keeps
    // particles, not what they deposit.
    const Effect effect = plume();
    BakeSettings settings;
    settings.frames = 10U;
    settings.modes = {CacheMode::Particles, CacheMode::Live};
    AJFX_CHECK(fails_with(bake(effect, 1U, SimulationOptions{}, settings), ErrorCode::InvalidEffect,
                          "emitter 'fire' feeds volume 'air', so it cannot be cached"));
    // The motes depend on the air, and the air on the fire: the motes'
    // digest changes with the fire's.
    Effect hotter = effect;
    std::get<VolumeSource>(hotter.emitters[0].update[0]).temperature = 5.0F;
    AJFX_CHECK(emitter_digest(effect, 1U) != emitter_digest(hotter, 1U));
    // And with the grid's own settings, but not with how it is drawn.
    Effect thicker = effect;
    thicker.volumes[0].buoyancy = 3.0F;
    AJFX_CHECK(emitter_digest(effect, 1U) != emitter_digest(thicker, 1U));
    Effect tinted = effect;
    tinted.volumes[0].color = Vec3{0.1F, 0.2F, 0.3F};
    tinted.volumes[0].albedo = 0.1F;
    AJFX_CHECK(emitter_digest(effect, 1U) == emitter_digest(tinted, 1U));
}

} // namespace

int main() {
    const ajfx::test::Case cases[] = {
        {"grids_are_budgeted_and_named", grids_are_budgeted_and_named},
        {"fire_feeds_the_grid_and_the_grid_rises", fire_feeds_the_grid_and_the_grid_rises},
        {"reruns_are_bit_identical_and_seeds_differ", reruns_are_bit_identical_and_seeds_differ},
        {"deposits_sum_the_same_in_any_order", deposits_sum_the_same_in_any_order},
        {"seeking_lands_on_the_stepped_grid", seeking_lands_on_the_stepped_grid},
        {"saved_state_carries_the_grids", saved_state_carries_the_grids},
        {"vorticity_confinement_stirs_the_flow", vorticity_confinement_stirs_the_flow},
        {"grids_round_trip_through_packages", grids_round_trip_through_packages},
        {"caches_keep_grids_out_of_cached_emitters", caches_keep_grids_out_of_cached_emitters},
    };
    return ajfx::test::run(cases);
}
