#pragma once

#include <mediaserver/core/types.hpp>
#include <array>
#include <algorithm>
#include <string_view>

namespace mediaserver::core {

// Bump whenever the compiled fixture, pass ordering or digest encoding changes.
inline constexpr u32 render_conformance_version = 5U;
enum class ConformanceState : u8 { Unknown, Running, Passed, Failed };
enum class ConformancePass : u8 {
    Environment, Irradiance, Specular, Brdf, Cascades, ShadowAtlas,
    Depth, Identity, Normals, Velocity, ObjectVelocity, Coverage, Occlusion,
    Lighting, Fog, Temporal, MotionBlur, Bloom, Display, Dither, PlateDof,
    PhysicalSky, PhysicalIrradiance, PhysicalSpecular, Count
};
inline constexpr std::size_t conformance_pass_count = static_cast<std::size_t>(ConformancePass::Count);
inline constexpr std::array<std::string_view, conformance_pass_count> conformance_pass_names{
    "environment", "irradiance", "specular", "BRDF LUT", "cascades", "shadow atlas",
    "depth", "object identity", "normals", "velocity", "object velocity", "coverage",
    "occlusion", "lighting", "fog", "temporal resolve", "object motion blur", "bloom",
    "display LUT", "output dither", "plate depth of field",
    "physical sky scattering", "physical sky irradiance", "physical sky specular"};

struct RenderAdapterIdentity final {
    std::array<char, 128U> name{};
    u64 driver_version{}; // DXGI CheckInterfaceSupport, four 16-bit components.
    u32 vendor{};
    u32 device{};
    bool operator==(const RenderAdapterIdentity&) const = default;
};

struct RenderConformance final {
    ConformanceState state{};
    u32 version{};
    u64 request{}; // Director's explicit check generation, zero for startup.
    RenderAdapterIdentity adapter{};
    std::array<u8, 32U> shader_bundle{}; // SHA-256 of compiled bytecode bundle.
    std::array<u64, conformance_pass_count> passes{};
    ConformancePass failed_pass{ConformancePass::Count};
    bool strict_float{};
    bool operator==(const RenderConformance&) const = default;

    [[nodiscard]] bool complete() const noexcept {
        return state == ConformanceState::Passed && version == render_conformance_version
            && adapter.name[0] != '\0' && adapter.driver_version != 0U
            && std::any_of(shader_bundle.begin(), shader_bundle.end(), [](u8 v) { return v != 0U; })
            && std::all_of(passes.begin(), passes.end(), [](u64 v) { return v != 0U; });
    }
};

enum class ConformanceMismatch : u8 { None, Pending, Failed, Version, Bundle, Adapter, Pass };
struct ConformanceComparison final {
    ConformanceMismatch reason{};
    ConformancePass pass{ConformancePass::Count};
};

[[nodiscard]] inline std::string_view conformance_detail(ConformanceComparison difference) noexcept {
    const auto index = static_cast<std::size_t>(difference.pass);
    if (index < conformance_pass_count) return conformance_pass_names[index];
    switch (difference.reason) {
    case ConformanceMismatch::None: return "matching pass bytes";
    case ConformanceMismatch::Pending: return "awaiting current evidence";
    case ConformanceMismatch::Failed: return "device or fixture setup";
    case ConformanceMismatch::Version: return "fixture version";
    case ConformanceMismatch::Bundle: return "shader bundle / floating point contract";
    case ConformanceMismatch::Adapter: return "adapter / driver version";
    case ConformanceMismatch::Pass: return "pass bytes";
    }
    return "unknown evidence";
}

// Compare bytes first so the operator sees the earliest divergent pass even
// when it was a different shader/driver that caused that divergence.
[[nodiscard]] inline ConformanceComparison compare_render_conformance(
    const RenderConformance& reference, const RenderConformance& observed) noexcept {
    if (reference.state == ConformanceState::Failed || observed.state == ConformanceState::Failed)
        return {ConformanceMismatch::Failed, observed.state == ConformanceState::Failed ? observed.failed_pass : reference.failed_pass};
    if (reference.state == ConformanceState::Passed && observed.state == ConformanceState::Passed
        && (reference.version != render_conformance_version || observed.version != render_conformance_version))
        return {ConformanceMismatch::Version};
    if (!reference.complete() || !observed.complete() || reference.request != observed.request)
        return {ConformanceMismatch::Pending};
    for (std::size_t i = 0; i < conformance_pass_count; ++i)
        if (reference.passes[i] != observed.passes[i])
            return {ConformanceMismatch::Pass, static_cast<ConformancePass>(i)};
    if (reference.shader_bundle != observed.shader_bundle || reference.strict_float != observed.strict_float)
        return {ConformanceMismatch::Bundle};
    if (reference.adapter != observed.adapter) return {ConformanceMismatch::Adapter};
    return {};
}
} // namespace mediaserver::core
