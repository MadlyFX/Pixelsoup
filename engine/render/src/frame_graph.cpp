#include <mediaserver/render/frame_graph.hpp>
#include <mediaserver/core/idle_report.hpp>
#include <new>

namespace mediaserver::render {
namespace { thread_local bool reference_recording{}; }
struct FrameGraph::Impl final {
    RenderGraph graph;
    RenderGraphExecutor executor;
    std::array<const rhi::Texture*, max_render_graph_resources> textures{};
    std::array<GraphAccess, max_render_graph_resources> accesses{};
    std::size_t texture_count{}, access_count{};
    FrameGraphStats stats{};
    bool failed{};

    void start_pass() noexcept { access_count = 0U; }
    bool access(const rhi::Texture* texture, GraphState state, GraphAccessMode mode) noexcept {
        if (!texture || !*texture || failed) return false;
        std::size_t i = 0U;
        while (i < texture_count && textures[i] != texture) ++i;
        if (i == texture_count) {
            auto resource = graph.add_resource({"texture", GraphResourceKind::Texture,
                mode != GraphAccessMode::Write, false});
            if (!resource) return false;
            textures[texture_count++] = texture;
            if (!executor.bind(resource.value(), const_cast<rhi::Texture&>(*texture))) return false;
        }
        GraphResource resource{static_cast<core::u16>(i + 1U)};
        for (std::size_t a = 0U; a < access_count; ++a) {
            if (accesses[a].resource == resource) {
                // Repeated material bindings are one read. Feedback between
                // SRV and RTV/UAV is refused rather than hidden by deduplication.
                return accesses[a].state == state && accesses[a].mode == mode;
            }
        }
        if (access_count == accesses.size()) return false;
        accesses[access_count++] = {resource, state, mode};
        return true;
    }
    bool sampled(std::span<const rhi::SampledTexture> sources) noexcept {
        for (const auto& source : sources)
            if (!access(source.texture, GraphState::ShaderRead, GraphAccessMode::Read)) return false;
        return true;
    }
    bool refuse(const char* reason = "invalid or over-capacity texture accesses") noexcept {
        failed = true; ++stats.refusals;
        static_cast<void>(core::report_idle_once("render graph", reason));
        return false;
    }
    template<class Body> bool run(rhi::CommandList& commands, core::RenderPass category,
                                GraphPassKind kind, Body&& body) noexcept {
        if (failed || !commands.device()) return refuse();
        const GraphPassDesc description{core::render_pass_names[static_cast<std::size_t>(category)], kind,
                GraphQueue::Graphics, {accesses.data(), access_count}};
        auto added = graph.add_pass(description);
        if (!added && added.error() == RenderGraphError::CapacityExceeded && graph.pass_count() != 0U) {
            // No work is dropped: finish one serial segment and continue with
            // its products imported. This preserves the renderer's existing
            // hundreds-of-transparent-layers bound without a growing graph.
            graph.discard_executed_passes();
            ++stats.segments;
            added = graph.add_pass(description);
        }
        if (!added) return refuse("pass declaration exceeds graph capacity");
        const auto plan = graph.compile();
        if (!plan) return refuse("pass reads an unwritten product or has invalid hazards");
        const GraphPassBody pass{[](rhi::CommandList&, void* context) {
            return (*static_cast<Body*>(context))();
        }, &body, category};
        if (!executor.execute(*commands.device(), commands, plan.value(),
                {&pass, 1U}, plan.value().passes().size() - 1U)) return refuse("pass execution or transition failed");
        stats.passes += executor.recorded_passes();
        stats.transitions += executor.recorded_transitions();
        return true;
    }
};
FrameGraph::FrameGraph() noexcept : impl_(new (std::nothrow) Impl) {}
FrameGraph::~FrameGraph() = default;
FrameGraph::FrameGraph(FrameGraph&&) noexcept = default;
FrameGraph& FrameGraph::operator=(FrameGraph&&) noexcept = default;
void FrameGraph::reference_recording_for_tests(bool enabled) noexcept { reference_recording = enabled; }
void FrameGraph::begin() noexcept {
    if (!impl_) return;
    impl_->graph.clear(); impl_->executor.reset();
    impl_->texture_count = impl_->access_count = 0U;
    impl_->stats.passes = impl_->stats.transitions = 0U; impl_->failed = false;
    impl_->stats.segments = 1U;
}
FrameGraphStats FrameGraph::stats() const noexcept { return impl_ ? impl_->stats : FrameGraphStats{0U, 0U, 1U}; }
bool FrameGraph::draw_pulled(rhi::CommandList& c, core::RenderPass p, rhi::Texture& color,
    rhi::Color clear_color, rhi::Texture& depth, float depth_clear, const rhi::PulledDraw& draw) noexcept {
    if (reference_recording || !c.device() || !c.device()->capabilities().explicit_barriers)
        return c.draw_pulled(color, clear_color, depth, depth_clear, draw);
    if (!impl_) return false;
    auto& g = *impl_; g.start_pass();
    const auto mode = draw.load ? GraphAccessMode::ReadWrite : GraphAccessMode::Write;
    if (!g.access(&color, GraphState::ColorTarget, mode) || !g.access(&depth, GraphState::DepthTarget, mode)
        || !g.sampled(draw.fragment_textures)
        || (draw.fragment_textures.empty() && draw.texture
            && !g.access(draw.texture, GraphState::ShaderRead, GraphAccessMode::Read))) return g.refuse();
    return g.run(c, p, GraphPassKind::Raster, [&]() { return c.draw_pulled(color, clear_color, depth, depth_clear, draw); });
}
bool FrameGraph::fullscreen(rhi::CommandList& c, core::RenderPass p, rhi::Texture& target,
    rhi::Color clear_color, const rhi::GraphicsPipeline& pipeline,
    std::span<const rhi::SampledTexture> sources, std::span<const core::u8> uniform, bool load) noexcept {
    if (reference_recording || !c.device() || !c.device()->capabilities().explicit_barriers)
        return c.draw_output_transform(target, clear_color, pipeline, sources, uniform, load);
    if (!impl_) return false;
    auto& g = *impl_; g.start_pass();
    if (!g.access(&target, GraphState::ColorTarget, load ? GraphAccessMode::ReadWrite : GraphAccessMode::Write)
        || !g.sampled(sources)) return g.refuse();
    return g.run(c, p, GraphPassKind::Composite, [&]() { return c.draw_output_transform(target, clear_color, pipeline, sources, uniform, load); });
}
bool FrameGraph::fullscreen(rhi::CommandList& c, core::RenderPass p, rhi::Texture& target,
    rhi::Color clear_color, const rhi::GraphicsPipeline& pipeline, const rhi::Texture& source,
    const rhi::Sampler& sampler, std::span<const core::u8> uniform, bool load) noexcept {
    const rhi::SampledTexture sampled{&source, &sampler};
    return fullscreen(c, p, target, clear_color, pipeline, {&sampled, 1U}, uniform, load);
}
bool FrameGraph::dispatch(rhi::CommandList& c, core::RenderPass p, const rhi::ComputeDispatch& work, bool preserve_storage) noexcept {
    if (reference_recording) return c.dispatch(work);
    if (!impl_) return false;
    auto& g = *impl_; g.start_pass();
    if (!g.sampled(work.sampled_textures)) return g.refuse();
    for (const auto& target : work.storage_textures)
        if (!g.access(target.texture, target.write ? GraphState::StorageWrite : GraphState::StorageRead,
                target.write ? (preserve_storage ? GraphAccessMode::ReadWrite : GraphAccessMode::Write) : GraphAccessMode::Read)) return g.refuse();
    return g.run(c, p, GraphPassKind::Compute, [&]() { return c.dispatch(work); });
}
bool FrameGraph::clear(rhi::CommandList& c, core::RenderPass p, rhi::Texture& target, rhi::Color color) noexcept {
    if (reference_recording || !c.device() || !c.device()->capabilities().explicit_barriers) return c.clear(target, color);
    if (!impl_) return false;
    auto& g = *impl_; g.start_pass();
    if (!g.access(&target, GraphState::ColorTarget, GraphAccessMode::Write)) return g.refuse();
    return g.run(c, p, GraphPassKind::Raster, [&]() { return c.clear(target, color); });
}
bool FrameGraph::copy(rhi::CommandList& c, core::RenderPass p, const rhi::Texture& source,
    rhi::ViewportRect rect, rhi::Texture& target) noexcept {
    if (reference_recording || !c.device() || !c.device()->capabilities().explicit_barriers) return c.copy_region(source, rect, target);
    if (!impl_) return false;
    auto& g = *impl_; g.start_pass();
    if (!g.access(&source, GraphState::CopySource, GraphAccessMode::Read)
        || !g.access(&target, GraphState::CopyDestination, GraphAccessMode::Write)) return g.refuse();
    return g.run(c, p, GraphPassKind::Copy, [&]() { return c.copy_region(source, rect, target); });
}
} // namespace mediaserver::render
