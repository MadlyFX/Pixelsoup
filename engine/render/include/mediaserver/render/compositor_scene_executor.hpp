#pragma once

#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/gaussian_blur.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <vector>

namespace mediaserver::render {

// This first executor slice intentionally accepts one flat image stack:
// Output <- SceneLayer, or Output <- Composite <- SceneLayer(s). Effect and
// RadialArray inputs remain attached to each SceneLayer through their typed
// ports. By default, source drawables absent from that Output path form an
// implicit bottom stack; OutputParameters::replace_scene explicitly suppresses
// it. Nested image composites need retained intermediate textures and are
// rejected rather than silently flattened into different blend semantics.
enum class CompositorScenePlanError : core::u8 {
    InvalidGraph,
    UnsupportedTopology,
    LayersOverCapacity,
    MissingSceneLayer,
    InconsistentLayerStyle,
    AllocationFailed,
};

// Each simultaneously recorded layer currently owns distinct renderer upload
// resources. Keep that honest first implementation bounded; a future shared
// immutable geometry arena can raise this without changing the graph API.
inline constexpr core::u32 max_compositor_scene_layers = 16U;

struct CompositorLayerPass final {
    doc::NodeId graph_node{};
    doc::NodeId scene_node{};
    // Scene-only execution groups consecutive ordinary layers into one
    // renderer pass. Empty means this pass contains only scene_node; graph
    // plans remain source-compatible single-layer passes.
    std::vector<doc::NodeId> scene_nodes;
    scene::BlendMode blend{scene::BlendMode::Normal};
    float blur_radius_px{};
    // Drawables belonging to *other* passes that this one renders for depth
    // alone. Isolating a layer costs it the shared depth buffer, and a flat
    // premultiplied composite has no way to put it back: without this, adding
    // a brightness or blur node to an object lifted that object in front of
    // everything else in the frame. Ascending document-id order.
    //
    // Only crisply opaque geometry qualifies. A translucent or additively
    // blended drawable does not hide what is behind it, and a blurred one has
    // no hard silhouette to occlude with, so neither may write depth on
    // another layer's behalf.
    std::vector<doc::NodeId> depth_only_nodes;

    bool operator==(const CompositorLayerPass&) const = default;
};

struct CompositorScenePlan final {
    // Brightness and RadialArray are already folded here by compositor::apply.
    // The executor derives an isolated Scene from this value for each pass.
    scene::Scene adjusted_scene;
    // Bottom-to-top order. Default graph plans begin with implicit pass-through
    // layers, then append the explicit Composite input edge order.
    std::vector<CompositorLayerPass> layers;
    // True only for an authored Output that explicitly opts out of scene
    // pass-through. Execution then starts transparent, preserving the graph's
    // full-image-replacement contract.
    bool replaces_scene{};

    bool operator==(const CompositorScenePlan&) const = default;
};

[[nodiscard]] core::Result<CompositorScenePlan, CompositorScenePlanError>
plan_compositor_scene(const compositor::CompositorGraph& graph,
                      const scene::Scene& scene);

// Builds the implicit compositor stack used when no authored graph exists.
// The evaluated Scene has no separate z-index, so document node identity is
// its deterministic bottom-to-top layer order. Consecutive Normal/no-blur
// sources are kept together; only layers that need a spatial pass or
// destination-dependent blend are isolated.
[[nodiscard]] core::Result<CompositorScenePlan, CompositorScenePlanError>
plan_scene_effects(const scene::Scene& scene);

struct CompositorSceneStats final {
    core::u32 layers{};
    core::u32 blurred_layers{};
    core::u32 composite_passes{};

    bool operator==(const CompositorSceneStats&) const = default;
};

using CompositorSceneResult = core::Result<const rhi::Texture*, RenderError>;

class CompositorSceneExecutor final {
  public:
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<CompositorSceneExecutor, RenderError>
    create(rhi::Device& device, core::u32 width, core::u32 height,
           const RendererLimits& limits = RendererLimits{});

    CompositorSceneExecutor(const CompositorSceneExecutor&) = delete;
    CompositorSceneExecutor& operator=(const CompositorSceneExecutor&) = delete;
    CompositorSceneExecutor(CompositorSceneExecutor&&) = default;
    CompositorSceneExecutor& operator=(CompositorSceneExecutor&&) = default;
    ~CompositorSceneExecutor() = default;

    [[nodiscard]] core::u32 width() const noexcept {
        return composite_a_.width();
    }
    [[nodiscard]] core::u32 height() const noexcept {
        return composite_a_.height();
    }
    // The most recent successful execute result. Before the first execute it
    // names the initial accumulator target; prefer execute().value() when
    // recording and use this accessor for presenter-style handoff afterward.
    [[nodiscard]] const rhi::Texture& target() const noexcept {
        return output_is_a_ ? composite_a_ : composite_b_;
    }

    [[nodiscard]] bool resize(rhi::Device& device, core::u32 width,
                              core::u32 height) noexcept;

    // Renders graph-managed scene nodes to transparent offscreen layers,
    // applies spatial blur, and composites them over the implicit pass-through
    // stack unless Output explicitly replaces the scene. No completed
    // whole-scene texture is ever passed to GaussianBlur.
    [[nodiscard]] CompositorSceneResult
    execute(rhi::CommandList& commands,
            const compositor::CompositorGraph& graph, const scene::Scene& scene,
            scene::CanvasRect slice) noexcept;

    // Physical-output projection. The graph/effect contract is identical, but
    // every isolated layer uses one already-resolved asymmetric viewpoint.
    [[nodiscard]] CompositorSceneResult
    execute(rhi::CommandList& commands,
            const compositor::CompositorGraph& graph,
            const scene::Scene& scene,
            const SceneRenderView& view) noexcept;

    // Executes the first-class LayerStyle effect stacks already folded into
    // an evaluated Scene. This is the normal path when no compositor graph is
    // authored; unlike graph execution, it includes every scene drawable and
    // preserves the ordinary opaque scene background.
    [[nodiscard]] CompositorSceneResult
    execute(rhi::CommandList& commands, const scene::Scene& scene,
            scene::CanvasRect slice) noexcept;

    [[nodiscard]] CompositorSceneResult
    execute(rhi::CommandList& commands, const scene::Scene& scene,
            const SceneRenderView& view) noexcept;

    [[nodiscard]] const CompositorSceneStats& stats() const noexcept {
        return stats_;
    }

    // Aggregate of the per-layer GPU media residencies used by the latest
    // execution. CPU decode residency is process-wide and reported separately
    // by media_decode_stats().
    [[nodiscard]] MediaCacheStats media_cache_stats() const noexcept;

  private:
    CompositorSceneExecutor(rhi::Device& device, RendererLimits limits,
                            std::vector<SceneRenderer> layer_renderers,
                            GaussianBlur blur, rhi::Texture composite_a,
                            rhi::Texture composite_b,
                            rhi::GraphicsPipeline composite_pipeline,
                            rhi::Sampler sampler) noexcept;

    [[nodiscard]] CompositorSceneResult
    execute_plan(rhi::CommandList& commands, const CompositorScenePlan& plan,
                 scene::CanvasRect slice, rhi::Color initial_clear,
                 const std::optional<SceneRenderView>& view =
                     std::nullopt) noexcept;

    rhi::Device* device_{};
    RendererLimits limits_{};
    // Each layer recorded into one command list needs distinct upload buffers;
    // otherwise a later SceneRenderer upload could replace an earlier pass's
    // staging data before submission. The pool grows to the graph's stable
    // layer count and is capped by max_compositor_scene_layers.
    std::vector<SceneRenderer> layer_renderers_;
    GaussianBlur blur_;
    rhi::Texture composite_a_;
    rhi::Texture composite_b_;
    rhi::GraphicsPipeline composite_pipeline_;
    rhi::Sampler sampler_;
    CompositorSceneStats stats_{};
    bool output_is_a_{true};
};

} // namespace mediaserver::render
