#pragma once
#include <mediaserver/render/graph_transient_pool.hpp>
#include <mediaserver/render/frame_graph.hpp>

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/bloom_chain.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/render/slice_apron.hpp>
#include <mediaserver/rhi/device.hpp>

#include <array>

// Bloom: the highlight spill a real lens adds, and the renderer already has the
// energy for.
//
// Advanced renders scene-linear and values above one survive the whole
// compositor, which means a practical light or a window onto a bright sky is
// carrying real highlight energy that currently has nowhere to go - it clips to
// a flat bright shape and reads as rendered rather than photographed. This is
// what turns it into light.
//
// Energy conserving, with no threshold. A hard threshold makes a light pop the
// moment it crosses one, which is a worse artefact than no bloom at all and one
// that a moving light will find repeatedly during a take.
//
// Where the levels sit is decided by `bloom_chain.hpp`, not here. This class
// owns the targets and records the dispatches; it refuses a plan that does not
// match the extent it was built for rather than filtering into the wrong grid.
namespace mediaserver::render {

struct BloomStats final {
    core::u64 chains{};
    core::u64 dispatches{};
    // Plans refused because they described a different extent from the targets.
    core::u64 mismatches{};

    bool operator==(const BloomStats&) const = default;
};

// The chain one frame should record, decided from the same inputs on the
// renderer and on the compositor so the two cannot disagree.
//
// A wall slice is planned on the authored canvas and the padded rectangle,
// which is what makes the chain node-independent. A frame rendered from an
// **explicit view** - an ICVFX plate, a projection-mapped surface - is planned
// on its own raster: it has no canvas rectangle to be aligned to, and planning
// it on the canvas refused every plate and every mapped output that authored
// bloom, because the canvas-sized plan never described the plate-sized
// targets. A view-driven surface is exact when one node renders all of it,
// which is the usual case for a plate; a surface raster partitioned across
// nodes has the same seam exposure a blur already has there, recorded in the
// status document.
[[nodiscard]] core::Result<BloomChainPlan, BloomChainError> bloom_plan_for(
    const scene::Scene& scene, const SceneRenderView* view,
    const PaddedSlice& padded, core::u32 target_width,
    core::u32 target_height) noexcept;

class Bloom final {
public:
    [[nodiscard]] static bool shaders_available() noexcept;

    // `output_format` is the format of level zero, the full-resolution result
    // `combine` publishes. Scene-linear RGBA16F everywhere it matters; a
    // compositor working in another format asks for its own so the frame can
    // be cropped out of the result without a conversion pass. The coarser
    // levels are always RGBA16F: an eight-bit intermediate would quantise the
    // very highlights the chain exists to spread.
    [[nodiscard]] static core::Result<Bloom, RenderError> create(
        rhi::Device& device, core::u32 width, core::u32 height,
        rhi::TextureFormat output_format =
            rhi::TextureFormat::Rgba16Float) noexcept;

    Bloom(const Bloom&) = delete;
    Bloom& operator=(const Bloom&) = delete;
    Bloom(Bloom&&) = default;
    Bloom& operator=(Bloom&&) = default;
    ~Bloom() = default;

    [[nodiscard]] core::u32 width() const noexcept {
        return level(0U).width();
    }
    [[nodiscard]] core::u32 height() const noexcept {
        return level(0U).height();
    }

    // The full-resolution result, ready to be added to the frame. Level zero of
    // the chain is never written by the downsample - the last upsample
    // establishes it - so this is the accumulated spill and nothing else.
    [[nodiscard]] const rhi::Texture& target() const noexcept {
        return level(0U);
    }

    [[nodiscard]] bool resize(rhi::Device& device, core::u32 width,
                              core::u32 height) noexcept;

    // Records the whole chain: down to the coarsest level, then back up adding
    // each level into the one below it.
    //
    // `source` is the scene colour at this node's *padded* extent, because the
    // padding is the part whose neighbourhood is trustworthy. Returns false and
    // counts a mismatch if the plan does not describe these targets.
    [[nodiscard]] bool record(rhi::CommandList& commands,
                              const rhi::Texture& source,
                              const BloomChainPlan& plan) noexcept;

    // Adds the accumulated spill back to the frame, at `strength`, and leaves
    // the result in `target()`.
    //
    // Separate from `record` because it reads the frame a second time and
    // writes the answer somewhere the caller then publishes; keeping it its own
    // step means the chain can be inspected on its own, which is what the halo
    // test does.
    [[nodiscard]] bool combine(rhi::CommandList& commands,
                               const rhi::Texture& source,
                               float strength) noexcept;

    [[nodiscard]] const BloomStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = {}; }
    [[nodiscard]] GraphTransientPoolStats transient_stats() const noexcept { return transients_.stats(); }

private:
    FrameGraph graph_;
    // One more than the deepest chain, because level zero is the full-
    // resolution accumulator rather than a downsampled level.
    static constexpr std::size_t slots = max_bloom_levels + 1U;

    Bloom(std::array<rhi::Texture, slots> levels,
          rhi::ComputePipeline downsample, rhi::ComputePipeline upsample,
          rhi::ComputePipeline combine, rhi::Sampler sampler,
          rhi::TextureFormat output_format) noexcept;

    GraphTransientPool transients_{{slots}};
    [[nodiscard]] rhi::Texture& level(std::size_t i) noexcept { return transients_.texture(i); }
    [[nodiscard]] const rhi::Texture& level(std::size_t i) const noexcept { return transients_.texture(i); }
    rhi::TextureFormat output_format_{rhi::TextureFormat::Rgba16Float};
    rhi::ComputePipeline downsample_;
    rhi::ComputePipeline upsample_;
    rhi::ComputePipeline combine_;
    rhi::Sampler sampler_;
    BloomStats stats_{};
};

} // namespace mediaserver::render
