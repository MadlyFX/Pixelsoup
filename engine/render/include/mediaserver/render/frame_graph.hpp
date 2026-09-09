#pragma once
#include <mediaserver/render/render_graph_executor.hpp>
#include <memory>

namespace mediaserver::render {

struct FrameGraphStats final {
    core::u64 passes{}, transitions{}, refusals{};
    core::u64 segments{};
    bool operator==(const FrameGraphStats&) const = default;
};

// A surface's bounded graph, compiled in declaration order as passes arrive.
// Each appended prefix is validated; only its new suffix executes. Pass bodies
// therefore borrow live stack uniforms without allocating or retaining them.
// This deliberately supports neither reordering nor aliasing across a prefix.
// Full pass/access arrays start another serial segment, importing already
// executed products while preserving texture identities and the resource bound.
// Components (fog, bloom, temporal resolve) own separate bounded subgraphs.
class FrameGraph final {
public:
    FrameGraph() noexcept;
    ~FrameGraph();
    FrameGraph(FrameGraph&&) noexcept;
    FrameGraph& operator=(FrameGraph&&) noexcept;
    FrameGraph(const FrameGraph&) = delete;
    FrameGraph& operator=(const FrameGraph&) = delete;
    void begin() noexcept;
    [[nodiscard]] FrameGraphStats stats() const noexcept;
    [[nodiscard]] bool draw_pulled(rhi::CommandList&, core::RenderPass,
        rhi::Texture&, rhi::Color, rhi::Texture&, float, const rhi::PulledDraw&) noexcept;
    [[nodiscard]] bool fullscreen(rhi::CommandList&, core::RenderPass,
        rhi::Texture&, rhi::Color, const rhi::GraphicsPipeline&,
        std::span<const rhi::SampledTexture>, std::span<const core::u8>, bool load = false) noexcept;
    [[nodiscard]] bool fullscreen(rhi::CommandList&, core::RenderPass,
        rhi::Texture&, rhi::Color, const rhi::GraphicsPipeline&, const rhi::Texture&,
        const rhi::Sampler&, std::span<const core::u8>, bool load = false) noexcept;
    [[nodiscard]] bool dispatch(rhi::CommandList&, core::RenderPass, const rhi::ComputeDispatch&, bool preserve_storage = false) noexcept;
    [[nodiscard]] bool clear(rhi::CommandList&, core::RenderPass, rhi::Texture&, rhi::Color) noexcept;
    [[nodiscard]] bool copy(rhi::CommandList&, core::RenderPass, const rhi::Texture&,
        rhi::ViewportRect, rhi::Texture&) noexcept;
    // Test-only reference recorder. The production default always executes the
    // graph. Thread-local so independent render tests cannot change each other.
    static void reference_recording_for_tests(bool enabled) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace mediaserver::render
