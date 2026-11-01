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
    // Lifetime's id and 2 floats, then the spin's id and data.
    AJFX_CHECK(contains(s, "ajfx_spin(p, 4u, ajfx_u32(3u));"));
    AJFX_CHECK(contains(s, "ajfx_spin_turn(p, 4u);"));
    AJFX_CHECK(s.find("p.position += p.velocity * ajfx_dt;") < s.find("ajfx_spin_turn(p"));
    AJFX_CHECK(s.find("ajfx_spin_turn(p") < s.find("ajfx_plane_collision(p"));
    // Newborns turn through the part of the step they lived.
    AJFX_CHECK(contains(s, "p.rotation += p.spin * p.age;"));
}

void module_calls_carry_their_data_offsets_and_ids() {
    const std::vector<gpu::EmitterKernelSource> sources = gpu::generate_kernel_sources(presets::sparks());
    AJFX_CHECK(sources.size() == 1U);
    const std::string& s = sources[0].source;
    // Each module's data is its id, then its packed floats, and offsets are
    // the running total: lifetime 2, shape 10, velocity 13, colour 10, size
    // 2, gravity 5 and its curve (1 + 2 per key), drag 5, plane 7,
    // colour-over-life 1 + 5 per key. A call reads its id from the data.
    AJFX_CHECK(contains(s, "ajfx_lifetime(p, 1u, ajfx_u32(0u));"));
    AJFX_CHECK(contains(s, "ajfx_shape_location(p, 4u, ajfx_u32(3u));"));
    AJFX_CHECK(contains(s, "ajfx_velocity(p, 15u, ajfx_u32(14u));"));
    AJFX_CHECK(contains(s, "ajfx_color(p, 29u, ajfx_u32(28u));"));
    AJFX_CHECK(contains(s, "ajfx_size(p, 40u, ajfx_u32(39u));"));
    AJFX_CHECK(contains(s, "ajfx_gravity(p, 43u, ajfx_u32(42u));"));
    AJFX_CHECK(contains(s, "ajfx_drag(p, 54u, ajfx_u32(53u));"));
    AJFX_CHECK(contains(s, "ajfx_plane_collision(p, 60u, ajfx_u32(59u));"));
    AJFX_CHECK(contains(s, "ajfx_color_over_life(p, 68u, ajfx_u32(67u));"));
    AJFX_CHECK(sources[0].module_floats == 79U);
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
        {"generation_is_deterministic", generation_is_deterministic},
        {"compiled_bytecode_is_reproducible", compiled_bytecode_is_reproducible},
        {"kernels_travel_in_packages_and_go_stale_with_their_source",
         kernels_travel_in_packages_and_go_stale_with_their_source},
        {"compile_errors_are_reported", compile_errors_are_reported},
    };
    return ajfx::test::run(cases);
}
