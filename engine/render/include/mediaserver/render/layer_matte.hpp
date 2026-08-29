#pragma once

#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>

#include <span>
#include <vector>

namespace mediaserver::render {

[[nodiscard]] bool valid_matte_parameters(
    const compositor::MatteParameters& parameters) noexcept;

using MatteReferenceResult =
    core::Result<std::vector<rhi::Color>, RenderError>;

// Deterministic CPU reference. Both inputs are row-major RGBA coverage
// carriers. Source RGB must be valid premultiplied colour; matte RGB may be
// alpha-bearing out-of-gamut data so clamp_input has observable semantics.
[[nodiscard]] MatteReferenceResult matte_reference(
    std::span<const rhi::Color> source,
    std::span<const rhi::Color> matte,
    core::u32 width,
    core::u32 height,
    const compositor::MatteParameters& parameters);

struct MattedLayer final {
    const rhi::Texture* color{};
    // Exact source tested-depth provenance. The matte branch depth is not an
    // input to this processor and therefore cannot be propagated.
    const rhi::Texture* depth{};

    bool operator==(const MattedLayer&) const = default;
};

struct MatteStats final {
    core::u64 recorded_passes{};
    core::u64 processed_pixels{};

    bool operator==(const MatteStats&) const = default;
};

using LayerMatteResult = core::Result<MattedLayer, RenderError>;

class LayerMatte final {
  public:
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<LayerMatte, RenderError>
    create(rhi::Device& device, core::u32 width, core::u32 height);

    LayerMatte(const LayerMatte&) = delete;
    LayerMatte& operator=(const LayerMatte&) = delete;
    LayerMatte(LayerMatte&&) = default;
    LayerMatte& operator=(LayerMatte&&) = default;
    ~LayerMatte() = default;

    [[nodiscard]] core::u32 width() const noexcept { return output_a_.width(); }
    [[nodiscard]] core::u32 height() const noexcept {
        return output_a_.height();
    }

    [[nodiscard]] bool resize(rhi::Device& device, core::u32 width,
                              core::u32 height) noexcept;

    // Matte is never inferred to be identity from pixel values. Every valid
    // call records exactly one pass. A future graph-level enabled flag may
    // bypass the node before this processor is invoked.
    [[nodiscard]] LayerMatteResult apply(
        rhi::CommandList& commands,
        const rhi::Texture& source_color,
        const rhi::Texture& source_depth,
        const rhi::Texture& matte_color,
        const compositor::MatteParameters& parameters) noexcept;

    [[nodiscard]] const MatteStats& stats() const noexcept { return stats_; }
    void reset_stats() noexcept { stats_ = {}; }

  private:
    LayerMatte(rhi::Texture output_a, rhi::Texture output_b,
               rhi::GraphicsPipeline pipeline, rhi::Sampler sampler) noexcept;

    [[nodiscard]] rhi::Texture& output_for(
        const rhi::Texture& source, const rhi::Texture& matte) noexcept;

    rhi::Texture output_a_;
    rhi::Texture output_b_;
    rhi::GraphicsPipeline pipeline_;
    rhi::Sampler sampler_;
    MatteStats stats_{};
};

} // namespace mediaserver::render
