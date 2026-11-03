#include "check.hpp"

#include <ajfx/d3d12.hpp>
#include <ajfx/gpu/kernels.hpp>
#include <ajfx/package.hpp>
#include <ajfx/presets.hpp>

#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace {

using namespace ajfx;

bool contains(const std::string& text, const std::string& fragment) {
    return text.find(fragment) != std::string::npos;
}

void layouts_store_only_what_modules_use() {
    // sparks: velocity, colour with colour-over-life, size without size-over-life.
    const gpu::ParticleLayout sparks = gpu::particle_layout(presets::sparks().emitters[0]);
    AJFX_CHECK(sparks.position == 0 && sparks.velocity == 12 && sparks.age == 24 && sparks.lifetime == 28 &&
               sparks.spawn_index == 32 && sparks.color == 36 && sparks.base_color == 52 && sparks.size == 68);
    AJFX_CHECK(sparks.base_size == -1 && sparks.rotation == -1);
    AJFX_CHECK(sparks.stride == 72U);

    // A bare emitter stores position, age, lifetime and spawn index only.
    Emitter bare;
    bare.init.push_back(Lifetime{.id = 1U});
    const gpu::ParticleLayout minimal = gpu::particle_layout(bare);
    AJFX_CHECK(minimal.velocity == -1 && minimal.color == -1 && minimal.size == -1);
    AJFX_CHECK(minimal.stride == 24U);

    // A spinning sprite stores its turn and its spin.
    Emitter spinning = bare;
    spinning.init.push_back(InitSpin{.id = 2U});
    const gpu::ParticleLayout turning = gpu::particle_layout(spinning);
    AJFX_CHECK(turning.rotation == 24 && turning.spin == 28 && turning.stride == 32U);
}

void spin_turns_where_position_integrates() {
    Effect effect;
    effect.name = "spin";
    Emitter& emitter = effect.emitters.emplace_back();
    emitter.name = "e";
    emitter.init = {Lifetime{.id = 1U}, InitSpin{.id = 2U}};
    emitter.update = {Gravity{.id = 3U}, PlaneCollision{.id = 4U}};
    const std::string s = gpu::generate_kernel_sources(effect)[0].source;
    // Lifetime's id, its scope's 9 floats and its 2, then the spin's id and
    // data.
    AJFX_CHECK(contains(s, "ajfx_spin(p, 13u, ajfx_u32(12u));"));
    AJFX_CHECK(contains(s, "ajfx_spin_turn(p, 13u);"));
    AJFX_CHECK(s.find("p.position += p.velocity * ajfx_dt;") < s.find("ajfx_spin_turn(p"));
    AJFX_CHECK(s.find("ajfx_spin_turn(p") < s.find("ajfx_plane_collision(p"));
    // Newborns turn through the part of the step they lived.
    AJFX_CHECK(contains(s, "p.rotation += p.spin * p.age;"));
}

void module_calls_carry_their_data_offsets_and_ids() {
    const std::vector<gpu::EmitterKernelSource> sources = gpu::generate_kernel_sources(presets::sparks());
    AJFX_CHECK(sources.size() == 1U);
    const std::string& s = sources[0].source;
    // Each module's data is its id, then its scope (9 floats) where it has
    // one, then its packed floats, and offsets are the running total:
    // lifetime 2, shape 10, velocity 13, colour 10, size 2, gravity 5 and its
    // curve (1 + 2 per key), drag 5, plane 7, colour-over-life 1 + 5 per key.
    // A call reads its id from the data, and a scoped one asks its scope.
    AJFX_CHECK(contains(s, "if (ajfx_in_scope(p, 1u)) {"));
    AJFX_CHECK(contains(s, "ajfx_lifetime(p, 10u, ajfx_u32(0u));"));
    AJFX_CHECK(contains(s, "ajfx_shape_location(p, 22u, ajfx_u32(12u));"));
    AJFX_CHECK(contains(s, "ajfx_velocity(p, 42u, ajfx_u32(32u));"));
    AJFX_CHECK(contains(s, "ajfx_color(p, 65u, ajfx_u32(55u));"));
    AJFX_CHECK(contains(s, "ajfx_size(p, 85u, ajfx_u32(75u));"));
    AJFX_CHECK(contains(s, "ajfx_gravity(p, 88u, ajfx_u32(87u));"));
    AJFX_CHECK(contains(s, "ajfx_drag(p, 99u, ajfx_u32(98u));"));
    AJFX_CHECK(contains(s, "ajfx_plane_collision(p, 105u, ajfx_u32(104u));"));
    AJFX_CHECK(contains(s, "if (ajfx_in_scope(p, 113u)) {"));
    AJFX_CHECK(contains(s, "ajfx_color_over_life(p, 122u, ajfx_u32(112u));"));
    AJFX_CHECK(sources[0].module_floats == 133U);
    // Ids are data, not source: new ids (a seed, a copied block) keep the
    // same kernels.
    Effect renumbered = presets::sparks();
    for (InitModule& module : renumbered.emitters[0].init) {
        std::visit([](auto& m) { m.id += 1000U; }, module);
    }
    AJFX_CHECK(gpu::generate_kernel_sources(renumbered)[0].source == s);
    AJFX_CHECK(gpu::kernel_source_hash(renumbered) == gpu::kernel_source_hash(presets::sparks()));
    // Forces run before integration, constraints after.
    AJFX_CHECK(s.find("ajfx_drag(p") < s.find("p.position += p.velocity * ajfx_dt;"));
    AJFX_CHECK(s.find("p.position += p.velocity * ajfx_dt;") < s.find("ajfx_plane_collision(p"));
    AJFX_CHECK(contains(s, "void update(") && contains(s, "void spawn("));
    AJFX_CHECK(!contains(s, "#include"));
}

void heat_and_births_from_bodies_reach_the_kernels() {
    Effect effect;
    effect.name = "glow";
    Emitter& rocks = effect.emitters.emplace_back();
    rocks.name = "rocks";
    rocks.update = {RigidPhysics{.id = 1U}};
    Emitter& sparks = effect.emitters.emplace_back();
    sparks.name = "sparks";
    sparks.spawn = {SpawnFromContacts{.id = 1U, .emitter = "rocks"}};
    sparks.init = {Lifetime{.id = 2U}};
    sparks.update = {Heat{.id = 3U}, ColorOverLife{.id = 4U}};
    const std::string s = gpu::generate_kernel_sources(effect)[1].source;
    // The glow is worked out afresh from the base colour after the colour
    // over life, in the update and for newborns, wherever Heat stands.
    const std::size_t reset = s.find("p.color = p.base_color;\n        if (ajfx_in_scope");
    AJFX_CHECK(reset != std::string::npos);
    AJFX_CHECK(s.find("ajfx_color_over_life(p", reset) < s.find("ajfx_heat(p", reset));
    AJFX_CHECK(s.find("ajfx_heat(p") > s.find("ajfx_color_over_life(p"));
    // Newborns start where their contact put them.
    AJFX_CHECK(contains(s, "uint origin = ajfx_ages_offset + 2u * ajfx_capacity + b * 7u;"));
    // Heat keeps the base colour; the layout stores it.
    const gpu::ParticleLayout layout = gpu::particle_layout(sparks);
    AJFX_CHECK(layout.color >= 0 && layout.base_color >= 0 && layout.velocity >= 0);
}


void trails_from_particles_get_a_shed_kernel() {
    Effect effect;
    effect.name = "trail";
    Emitter& embers = effect.emitters.emplace_back();
    embers.name = "embers";
    embers.spawn = {SpawnBurst{.id = 1U, .count = 4U}};
    embers.init = {Lifetime{.id = 2U}, Velocity{.id = 3U}};
    Emitter& smoke = effect.emitters.emplace_back();
    smoke.name = "smoke";
    smoke.capacity = 8192U;
    smoke.spawn = {SpawnRate{.id = 1U}, SpawnFromParticles{.id = 2U, .emitter = "embers"}};
    smoke.init = {Lifetime{.id = 3U}};
    AJFX_CHECK(validate(effect).ok());
    const std::vector<gpu::EmitterKernelSource> sources = gpu::generate_kernel_sources(effect);
    AJFX_CHECK(!sources[0].sheds && !contains(sources[0].source, "void shed("));
    AJFX_CHECK(sources[1].sheds && contains(sources[1].source, "void shed("));
    const std::string& s = sources[1].source;
    // The module's data follows the init stack (one lifetime: id, scope, two
    // floats), its slot is baked, and the source's record layout is read
    // directly: position first, then velocity, with the spawn index after
    // age and lifetime.
    AJFX_CHECK(contains(s, "o = 13u;") && contains(s, "id = ajfx_u32(12u);") && contains(s, "slot = 1u;"));
    // (The emitters moved when the second was added, so by index.)
    const gpu::ParticleLayout source = gpu::particle_layout(effect.emitters[0]);
    AJFX_CHECK(contains(s, "source_stride = " + std::to_string(source.stride) + "u;"));
    AJFX_CHECK(contains(s, "at_velocity = " + std::to_string(source.velocity) + "u;"));
    AJFX_CHECK(contains(s, "at_spawn_index = " + std::to_string(source.spawn_index) + "u;"));
    // Births shed on the GPU count with the ones the CPU scheduled.
    AJFX_CHECK(contains(s, "if (b >= ajfx_births() ||"));
    AJFX_CHECK(sources[1].module_floats == 12U + 1U + 14U);
    // The shed kernel travels in the package beside the others, for the
    // emitter that has one only.
    gpu::KernelSet set;
    set.emitters.resize(2U);
    std::vector<PackageEntry> entries = gpu::kernel_package_entries(effect, set);
    std::size_t sheds = 0U;
    for (const PackageEntry& entry : entries) {
        sheds += entry.name.ends_with("/shed") ? 1U : 0U;
        AJFX_CHECK(!entry.name.ends_with("emitter/0/shed"));
    }
    AJFX_CHECK(sheds == 1U);
}

void generation_is_deterministic() {
    for (const Effect& effect : {presets::sparks(), presets::smoke(), presets::confetti(), presets::debris()}) {
        const auto a = gpu::generate_kernel_sources(effect);
        const auto b = gpu::generate_kernel_sources(effect);
        AJFX_CHECK(a.size() == b.size());
        for (std::size_t i = 0U; i < a.size(); ++i) {
            AJFX_CHECK(a[i].source == b[i].source);
        }
    }
    const std::string runtime = gpu::runtime_kernel_source();
    AJFX_CHECK(runtime == gpu::runtime_kernel_source());
    for (const char* entry : {"void scan_blocks(", "void scan_totals(", "void scatter(", "void finalize("}) {
        AJFX_CHECK(contains(runtime, entry));
    }
    AJFX_CHECK(!contains(runtime, "#include"));
}

// Needs dxcompiler.dll; the rest of the suite does not.
void compiled_bytecode_is_reproducible() {
    Result<std::unique_ptr<gpu::ShaderCompiler>> compiler = d3d12::create_shader_compiler();
    if (!compiler) {
        std::cout << "  (no compiler: " << compiler.error().message << ")\n";
        return;
    }
    for (const Effect& effect : {presets::sparks(), presets::smoke()}) {
        Result<gpu::KernelSet> a = gpu::compile_kernels(effect, *compiler.value());
        if (!a) {
            std::cout << a.error().message << '\n';
        }
        AJFX_CHECK(a.ok());
        Result<gpu::KernelSet> b = gpu::compile_kernels(effect, *compiler.value());
        AJFX_CHECK(b.ok());
        AJFX_CHECK(a.value().emitters[0].update == b.value().emitters[0].update);
        AJFX_CHECK(a.value().emitters[0].spawn == b.value().emitters[0].spawn);
        AJFX_CHECK(a.value().scan_blocks == b.value().scan_blocks);
        AJFX_CHECK(!a.value().emitters[0].update.empty());
    }
}

void kernels_travel_in_packages_and_go_stale_with_their_source() {
    Result<std::unique_ptr<gpu::ShaderCompiler>> compiler = d3d12::create_shader_compiler();
    if (!compiler) {
        return;
    }
    const Effect effect = presets::smoke();
    const gpu::KernelSet kernels = gpu::compile_kernels(effect, *compiler.value()).value();
    const std::vector<std::uint8_t> package =
        write_package(effect, gpu::kernel_package_entries(effect, kernels)).value();

    // The effect still reads, and the kernels come back byte for byte.
    AJFX_CHECK(read_package(package).value() == effect);
    const Result<gpu::KernelSet> read = gpu::read_package_kernels(package, effect, gpu::ShaderFormat::Dxil);
    AJFX_CHECK(read.ok());
    AJFX_CHECK(read.value().scan_blocks == kernels.scan_blocks && read.value().finalize == kernels.finalize);
    AJFX_CHECK(read.value().sprite_vertex == kernels.sprite_vertex && read.value().sprite_pixel == kernels.sprite_pixel);
    AJFX_CHECK(read.value().emitters.size() == 1U && read.value().emitters[0].update == kernels.emitters[0].update &&
               read.value().emitters[0].spawn == kernels.emitters[0].spawn);

    // Kernels compiled for one effect are refused for another.
    Effect edited = effect;
    std::get<Drag>(edited.emitters[0].update[2]).coefficient = 0.9F; // data, not code: same kernels
    AJFX_CHECK(gpu::read_package_kernels(package, edited, gpu::ShaderFormat::Dxil).ok());
    edited.emitters[0].update.pop_back(); // a module removed: different code
    const Result<gpu::KernelSet> stale = gpu::read_package_kernels(package, edited, gpu::ShaderFormat::Dxil);
    AJFX_CHECK(!stale.ok() && stale.error().code == ErrorCode::KernelsUnavailable);
    AJFX_CHECK(contains(stale.error().message, "different source"));

    // A package written without kernels says so.
    const Result<gpu::KernelSet> none =
        gpu::read_package_kernels(write_package(effect).value(), effect, gpu::ShaderFormat::Dxil);
    AJFX_CHECK(!none.ok() && contains(none.error().message, "has none"));
    // And effect.json cannot be smuggled in as an extra entry.
    AJFX_CHECK(!write_package(effect, {{"effect.json", {1U}}}).ok());
}

void compile_errors_are_reported() {
    Result<std::unique_ptr<gpu::ShaderCompiler>> compiler = d3d12::create_shader_compiler();
    if (!compiler) {
        return;
    }
    Result<std::vector<std::uint8_t>> broken =
        compiler.value()->compile("[numthreads(1,1,1)] void main() { undefined_thing(); }", "main",
                                  gpu::ShaderStage::Compute, "broken");
    AJFX_CHECK(!broken.ok() && broken.error().code == ErrorCode::ShaderCompilation);
    AJFX_CHECK(contains(broken.error().message, "undefined_thing"));
}

} // namespace

int main() {
    const ajfx::test::Case cases[] = {
        {"layouts_store_only_what_modules_use", layouts_store_only_what_modules_use},
        {"module_calls_carry_their_data_offsets_and_ids", module_calls_carry_their_data_offsets_and_ids},
        {"spin_turns_where_position_integrates", spin_turns_where_position_integrates},
        {"heat_and_births_from_bodies_reach_the_kernels", heat_and_births_from_bodies_reach_the_kernels},
        {"trails_from_particles_get_a_shed_kernel", trails_from_particles_get_a_shed_kernel},
        {"generation_is_deterministic", generation_is_deterministic},
        {"compiled_bytecode_is_reproducible", compiled_bytecode_is_reproducible},
        {"kernels_travel_in_packages_and_go_stale_with_their_source",
         kernels_travel_in_packages_and_go_stale_with_their_source},
        {"compile_errors_are_reported", compile_errors_are_reported},
    };
    return ajfx::test::run(cases);
}
