#pragma once
#include <mediaserver/render/frame_graph.hpp>

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>

// The spatial half of step 08's antialiasing.
//
// One fullscreen edge-directed blend, holding no state between frames. That
// is the entire reason it exists next to the temporal resolve: it cannot ghost,
// cannot smear, and cannot disagree with a neighbouring node about anything
// beyond the pixels it was handed. It resolves thin geometry far less well,
// which is the trade, and the trade is authored.
namespace mediaserver::render {

struct SpatialAntialiasStats final {
    core::u64 passes{};

    bool operator==(const SpatialAntialiasStats&) const = default;
};

class SpatialAntialias final {
public:
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<SpatialAntialias, RenderError> create(
        rhi::Device& device, core::u32 width, core::u32 height) noexcept;

    SpatialAntialias(const SpatialAntialias&) = delete;
    SpatialAntialias& operator=(const SpatialAntialias&) = delete;
    SpatialAntialias(SpatialAntialias&&) = default;
    SpatialAntialias& operator=(SpatialAntialias&&) = default;
    ~SpatialAntialias() = default;

    [[nodiscard]] core::u32 width() const noexcept { return output_.width(); }
    [[nodiscard]] core::u32 height() const noexcept { return output_.height(); }

    [[nodiscard]] bool resize(rhi::Device& device, core::u32 width,
                              core::u32 height) noexcept;

    [[nodiscard]] core::Result<const rhi::Texture*, RenderError> apply(
        rhi::CommandList& commands, const rhi::Texture& source) noexcept;

    [[nodiscard]] const SpatialAntialiasStats& stats() const noexcept {
        return stats_;
    }
    void reset_stats() noexcept { stats_ = {}; }

private:
    FrameGraph graph_;
    SpatialAntialias(rhi::Texture output, rhi::GraphicsPipeline pipeline,
                     rhi::Sampler sampler) noexcept;

    rhi::Texture output_;
    rhi::GraphicsPipeline pipeline_;
    rhi::Sampler sampler_;
    SpatialAntialiasStats stats_{};
};

// Relative contrast that counts as an edge, and the absolute floor below which
// nothing does. The floor is what keeps the pass off flat areas where the only
// variation is quantisation - without it a wall picks up a uniform softness
// that reads as a renderer fault rather than as antialiasing.
inline constexpr float spatial_edge_threshold = 0.125F;
inline constexpr float spatial_edge_floor = 0.0312F;

} // namespace mediaserver::render
