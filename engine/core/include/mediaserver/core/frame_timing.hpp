#pragma once

#include <mediaserver/core/types.hpp>
#include <algorithm>
#include <array>
#include <string_view>

namespace mediaserver::core {

// Stable wire IDs. Measurements never participate in scene/history digests.
enum class RenderPass : u8 {
    Other, Sky, Shadows, ShadowAttributes, Depth, Normals, Identity,
    Velocity, ObjectVelocity, Coverage, Occlusion, OcclusionHistory,
    Lighting, Media, FogScatter, FogIntegrate, FogComposite,
    Temporal, Spatial, MotionBlur, DepthOfField, BloomDown, BloomUp,
    BloomComposite, Composite, Output, Environment, Count
};
inline constexpr std::size_t render_pass_count = static_cast<std::size_t>(RenderPass::Count);
inline constexpr std::array<std::string_view, render_pass_count> render_pass_names{
    "Other", "Sky", "Shadows", "Shadow attributes", "Depth", "Normals", "Identity",
    "Velocity", "Object velocity", "Coverage", "Occlusion", "Occlusion history",
    "Lighting", "Media", "Fog scatter", "Fog integrate", "Fog composite",
    "Temporal AA", "Spatial AA", "Object motion blur", "Plate depth of field",
    "Bloom downsample", "Bloom upsample", "Bloom composite", "Composite", "Output", "Environment"
};

struct FrameTiming final {
    u64 sequence{};             // Local submission serial, not a show-frame clock.
    u64 profile_digest{};       // Settings actually used by this submission.
    u64 cpu_record_ns{};
    u64 gpu_ns{};
    u64 upload_bytes{};         // Bytes reserved in the recording's upload arena.
    u64 present_ns{};           // CPU time inside native Present/barrier Present.
    u64 presents{}, presents_in_sync{}, flips_in_sync{};
    u64 query_overflows{};
    std::array<u64, render_pass_count> pass_ns{};
    bool gpu_valid{};           // Set only after the owning fence completes.

    [[nodiscard]] bool over_budget(u32 budget_us) const noexcept {
        const u64 budget = static_cast<u64>(budget_us) * 1'000U;
        return budget != 0U && (cpu_record_ns > budget || (gpu_valid && gpu_ns > budget));
    }
    [[nodiscard]] RenderPass slowest_pass() const noexcept {
        return static_cast<RenderPass>(std::max_element(pass_ns.begin(), pass_ns.end()) - pass_ns.begin());
    }
    bool operator==(const FrameTiming&) const = default;
};

} // namespace mediaserver::core
