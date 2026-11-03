#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/gpu/device.hpp>
#include <ajfx/package.hpp>
#include <ajfx/result.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Kernel generation.
//
// Every emitter gets its own fused kernels: an update kernel that runs the
// whole update stack per particle in one pass, and a spawn kernel that runs
// the init stack for each newborn. Their source is generated from the module
// stacks and is deterministic text, so the same effect always compiles to the
// same bytecode with the same compiler. Compaction and bookkeeping kernels are
// shared by every effect.
namespace ajfx::gpu {

// Byte offsets of each attribute in a particle record; -1 for one the
// emitter's modules never touch, which then takes its default. Only what an
// emitter uses is stored.
struct ParticleLayout final {
    std::int32_t position{-1};
    std::int32_t velocity{-1};
    std::int32_t age{-1};
    std::int32_t lifetime{-1};
    std::int32_t spawn_index{-1};
    std::int32_t color{-1};
    std::int32_t base_color{-1};
    std::int32_t size{-1};
    std::int32_t base_size{-1};
    std::int32_t rotation{-1};
    std::int32_t orientation{-1};
    std::int32_t angular_velocity{-1};
    std::int32_t spin{-1};
    // Which kind of body each particle is, for an emitter that draws a model
    // per body kind (models_by_kind). Nothing else reads it, so nothing else
    // stores it.
    std::int32_t body_kind{-1};
    std::uint32_t stride{0U};

    friend bool operator==(const ParticleLayout&, const ParticleLayout&) = default;
};

[[nodiscard]] ParticleLayout particle_layout(const Emitter& emitter);

struct EmitterKernelSource final {
    std::string source; // entry points "update" and "spawn", and "shed" where `sheds`
    ParticleLayout layout;
    // Whether the emitter sheds particles from other emitters' particles
    // (SpawnFromParticles), so its source has a "shed" entry point too.
    bool sheds{false};
    // Floats of per-step module data the kernels read, in front of the birth ages.
    std::uint32_t module_floats{0U};
};

// HLSL for each emitter, in emitter order.
[[nodiscard]] std::vector<EmitterKernelSource> generate_kernel_sources(const Effect& effect);
// HLSL for the shared kernels: entry points "scan_blocks", "scan_totals",
// "scatter" and "finalize".
[[nodiscard]] std::string runtime_kernel_source();
// HLSL for the sprite renderer: entry points "sprite_vertex" and "sprite_pixel".
[[nodiscard]] std::string sprite_shader_source();
// HLSL for the depth sort: entry points "sort_keys", "sort_count", "sort_scan"
// and "sort_scatter".
[[nodiscard]] std::string sort_kernel_source();
// HLSL for the grid volumes' step: the entry points volume_kernel_names
// lists, in that order.
[[nodiscard]] std::string volume_kernel_source();
// HLSL for the reference volume raymarch: entry points "volume_vertex" and
// "volume_pixel".
[[nodiscard]] std::string volume_shader_source();

// The grid step's kernels, one dispatch each in this order (jacobi as many
// times as the volume asks): inject (with buoyancy), curl, confine, advect,
// divergence, jacobi, project (with dissipation).
inline constexpr std::string_view volume_kernel_names[] = {"volume_inject",     "volume_curl",   "volume_confine",
                                                           "volume_advect",     "volume_divergence",
                                                           "volume_jacobi",     "volume_project"};
inline constexpr std::size_t volume_kernel_count = 7U;

struct EmitterKernels final {
    std::vector<std::uint8_t> update;
    std::vector<std::uint8_t> spawn;
    // The births shed from other emitters' particles (SpawnFromParticles),
    // worked out on the GPU before the spawn kernel; empty for an emitter
    // that sheds from none.
    std::vector<std::uint8_t> shed;
};

struct KernelSet final {
    ShaderFormat format{ShaderFormat::Dxil};
    std::vector<std::uint8_t> scan_blocks;
    std::vector<std::uint8_t> scan_totals;
    std::vector<std::uint8_t> scatter;
    std::vector<std::uint8_t> finalize;
    std::vector<std::uint8_t> sort_keys;
    std::vector<std::uint8_t> sort_count;
    std::vector<std::uint8_t> sort_scan;
    std::vector<std::uint8_t> sort_scatter;
    std::vector<std::uint8_t> sprite_vertex;
    std::vector<std::uint8_t> sprite_pixel;
    std::vector<EmitterKernels> emitters;
    // The grid volumes' kernels (volume_kernel_names) and the raymarch's
    // shaders; empty for an effect without volumes.
    std::vector<std::uint8_t> volume[volume_kernel_count];
    std::vector<std::uint8_t> volume_vertex;
    std::vector<std::uint8_t> volume_pixel;
};

// Generates and compiles every kernel and shader an effect needs.
[[nodiscard]] Result<KernelSet> compile_kernels(const Effect& effect, ShaderCompiler& compiler);

// Identifies the generated source a kernel set was compiled from: the effect's
// kernels and this library's shared HLSL. Kernels carried in a package are
// used only when it matches, so a package from an older ajfx, or with an
// edited effect, never runs mismatched bytecode.
[[nodiscard]] std::uint64_t kernel_source_hash(const Effect& effect);

// Package entries holding the kernels, under "kernels/<format>/", for
// write_package's `extra` entries.
[[nodiscard]] std::vector<PackageEntry> kernel_package_entries(const Effect& effect, const KernelSet& kernels);

// The kernels a package carries for `format`, checked against the effect it
// holds. Fails with KernelsUnavailable when there are none for the format or
// they were compiled from different source.
[[nodiscard]] Result<KernelSet> read_package_kernels(std::span<const std::uint8_t> package, const Effect& effect,
                                                     ShaderFormat format);

} // namespace ajfx::gpu
