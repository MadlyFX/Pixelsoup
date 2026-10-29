#include "check.hpp"

#include <ajfx/d3d12.hpp>
#include <ajfx/gpu/kernels.hpp>
#include <ajfx/presets.hpp>

#include <iostream>
#include <string>

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
}

void module_calls_carry_their_data_offsets_and_ids() {
    const std::vector<gpu::EmitterKernelSource> sources = gpu::generate_kernel_sources(presets::sparks());
    AJFX_CHECK(sources.size() == 1U);
    const std::string& s = sources[0].source;
    // Offsets are the running total of each module's packed floats:
    // lifetime 2, shape 9, velocity 12, colour 8, size 2, gravity 3, drag 1,
    // plane 7, colour-over-life 1 + 5 per key.
    AJFX_CHECK(contains(s, "ajfx_lifetime(p, 0u, 2u);"));
    AJFX_CHECK(contains(s, "ajfx_shape_location(p, 2u, 3u);"));
    AJFX_CHECK(contains(s, "ajfx_velocity(p, 11u, 4u);"));
    AJFX_CHECK(contains(s, "ajfx_color(p, 23u, 5u);"));
    AJFX_CHECK(contains(s, "ajfx_size(p, 31u, 6u);"));
    AJFX_CHECK(contains(s, "ajfx_gravity(p, 33u, 7u);"));
    AJFX_CHECK(contains(s, "ajfx_drag(p, 36u, 8u);"));
    AJFX_CHECK(contains(s, "ajfx_plane_collision(p, 37u, 9u);"));
    AJFX_CHECK(contains(s, "ajfx_color_over_life(p, 44u, 10u);"));
    AJFX_CHECK(sources[0].module_floats == 55U);
    // Forces run before integration, constraints after.
    AJFX_CHECK(s.find("ajfx_drag(p") < s.find("p.position += p.velocity * ajfx_dt;"));
    AJFX_CHECK(s.find("p.position += p.velocity * ajfx_dt;") < s.find("ajfx_plane_collision(p"));
    AJFX_CHECK(contains(s, "void update(") && contains(s, "void spawn("));
    AJFX_CHECK(!contains(s, "#include"));
}

void generation_is_deterministic() {
    for (const Effect& effect : {presets::sparks(), presets::smoke(), presets::confetti()}) {
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

void compile_errors_are_reported() {
    Result<std::unique_ptr<gpu::ShaderCompiler>> compiler = d3d12::create_shader_compiler();
    if (!compiler) {
        return;
    }
    Result<std::vector<std::uint8_t>> broken =
        compiler.value()->compile_compute("[numthreads(1,1,1)] void main() { undefined_thing(); }", "main", "broken");
    AJFX_CHECK(!broken.ok() && broken.error().code == ErrorCode::ShaderCompilation);
    AJFX_CHECK(contains(broken.error().message, "undefined_thing"));
}

} // namespace

int main() {
    const ajfx::test::Case cases[] = {
        {"layouts_store_only_what_modules_use", layouts_store_only_what_modules_use},
        {"module_calls_carry_their_data_offsets_and_ids", module_calls_carry_their_data_offsets_and_ids},
        {"generation_is_deterministic", generation_is_deterministic},
        {"compiled_bytecode_is_reproducible", compiled_bytecode_is_reproducible},
        {"compile_errors_are_reported", compile_errors_are_reported},
    };
    return ajfx::test::run(cases);
}
