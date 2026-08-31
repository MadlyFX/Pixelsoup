#pragma once

#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/gaussian_blur.hpp>
#include <mediaserver/render/layer_keyer.hpp>
#include <mediaserver/render/layer_matte.hpp>
#include <mediaserver/render/mask_raster.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/render/spatial_blur.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>
#include <vector>

namespace mediaserver::render {

// This executor accepts one flat image stack: Output <- SceneLayer,
// Output <- Composite <- SceneLayer(s), or Output <- LED Car Process <- any
// subset of its nine optional SceneLayers. Effect and RadialArray inputs remain
// attached through typed ports. By default, source drawables absent from that Output path form an
// implicit bottom stack; OutputParameters::replace_scene explicitly suppresses
// it. Nested image composites need retained intermediate textures and are
// rejected rather than silently flattened into different blend semantics.
enum class CompositorScenePlanError : core::u8 {
    InvalidGraph,
    // The authored effect order crosses execution domains in a sequence the
    // fixed renderer cannot represent without silently changing semantics.
    InvalidConfiguration,
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

struct CompositorMatteBranch final {
    doc::NodeId graph_node{};
    doc::NodeId scene_node{};
    float blur_radius_px{};
    std::vector<scene::SpatialEffectOperation> spatial_effects;
    std::optional<scene::MaskProgram> mask_program;

    bool operator==(const CompositorMatteBranch&) const = default;
};

struct CompositorLayerPass final {
    doc::NodeId graph_node{};
    doc::NodeId scene_node{};
    // Scene-only execution groups every ordinary layer into one shared-depth
    // renderer pass. Empty means this pass contains only scene_node. A graph
    // Scene Layer naming a Track lists that Track's currently on-air clips
    // here; an empty Track therefore remains a valid transparent pass.
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
    // Exact authored isolated-layer program. Empty preserves hand-built plans
    // that use blur_radius_px as the legacy one-Gaussian form.
    std::vector<scene::SpatialEffectOperation> spatial_effects;
    // One active, valid SourceLayer program for this exact drawable source.
    // Empty is identity. AuthoredCanvas group programs cannot enter a flat
    // pass and are rejected by planning until groups have a retained isolated
    // composite of their own.
    std::optional<scene::MaskProgram> mask_program;
    // A Matte node consumes this distinct isolated SceneLayer branch as
    // coverage. It is never appended to the output stack and its tested depth
    // is deliberately absent from the processor contract.
    std::optional<CompositorMatteBranch> matte_branch;
    std::optional<compositor::MatteParameters> matte;

    bool operator==(const CompositorLayerPass&) const = default;
};

struct CompositorScenePlan final {
    // Inline effects and RadialArray are already folded here by
    // compositor::apply; Blur remains an explicit layer-pass radius. The
    // executor derives an isolated Scene from this value for each pass.
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
// Normal/no-blur sources stay together in one shared-depth base pass. Only
// layers that need a spatial pass or destination-dependent blend are isolated;
// those layers are ordered back-to-front by camera-space depth, with document
// node identity as the deterministic equal-depth tie breaker.
[[nodiscard]] core::Result<CompositorScenePlan, CompositorScenePlanError>
plan_scene_effects(const scene::Scene& scene);

struct CompositorSceneStats final {
    core::u32 layers{};
    core::u32 blurred_layers{};
    core::u32 box_blurred_layers{};
    core::u32 directional_blurred_layers{};
    core::u32 chroma_keyed_layers{};
    core::u32 luma_keyed_layers{};
    core::u32 masked_layers{};
    core::u32 matted_layers{};
    core::u32 composite_passes{};
    core::u64 keyer_passes{};
    core::u64 keyer_pixels{};
    core::u64 mask_passes{};
    core::u64 mask_pixels{};
    core::u64 matte_passes{};
    core::u64 matte_pixels{};

    bool operator==(const CompositorSceneStats&) const = default;
};

using CompositorSceneResult = core::Result<const rhi::Texture*, RenderError>;

class CompositorSceneExecutor final {
  public:
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<CompositorSceneExecutor, RenderError>
    create(rhi::Device& device, core::u32 width, core::u32 height,
           const RendererLimits& limits = RendererLimits{},
           MediaCacheConfig media_cache = MediaCacheConfig{});

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
    // whole-scene texture is ever passed to a spatial-filter processor.
    [[nodiscard]] CompositorSceneResult
    execute(rhi::CommandList& commands,
            const compositor::CompositorGraph& graph, const scene::Scene& scene,
            scene::CanvasRect slice,
            MediaCache* shared_media_cache = nullptr) noexcept;

    // Physical-output projection. The graph/effect contract is identical, but
    // every isolated layer uses one already-resolved asymmetric viewpoint.
    [[nodiscard]] CompositorSceneResult
    execute(rhi::CommandList& commands,
            const compositor::CompositorGraph& graph,
            const scene::Scene& scene,
            const SceneRenderView& view,
            MediaCache* shared_media_cache = nullptr) noexcept;

    // Executes the first-class LayerStyle effect stacks already folded into
    // an evaluated Scene. This is the normal path when no compositor graph is
    // authored; unlike graph execution, it includes every scene drawable and
    // preserves the ordinary opaque scene background.
    [[nodiscard]] CompositorSceneResult
    execute(rhi::CommandList& commands, const scene::Scene& scene,
            scene::CanvasRect slice,
            MediaCache* shared_media_cache = nullptr) noexcept;

    [[nodiscard]] CompositorSceneResult
    execute(rhi::CommandList& commands, const scene::Scene& scene,
            const SceneRenderView& view,
            MediaCache* shared_media_cache = nullptr) noexcept;

    [[nodiscard]] const CompositorSceneStats& stats() const noexcept {
        return stats_;
    }

    // Snapshot of the GPU media residency used by the latest execution. Layers
    // share exact source frames; CPU decode residency is process-wide and
    // reported separately by media_decode_stats().
    [[nodiscard]] MediaCacheStats media_cache_stats() const noexcept;

  private:
    CompositorSceneExecutor(rhi::Device& device, RendererLimits limits,
                            MediaCacheConfig media_cache,
                            std::vector<SceneRenderer> layer_renderers,
                            GaussianBlur blur, SpatialBlur spatial_blur,
                            LayerKeyer keyer, MaskRaster mask_raster,
                            LayerMatte matte,
                            rhi::Texture composite_a,
                            rhi::Texture composite_b,
                            rhi::GraphicsPipeline composite_pipeline,
                            rhi::Sampler sampler) noexcept;

    [[nodiscard]] CompositorSceneResult
    execute_plan(rhi::CommandList& commands, const CompositorScenePlan& plan,
                 scene::CanvasRect slice, rhi::Color initial_clear,
                 const std::optional<SceneRenderView>& view =
                     std::nullopt,
                 MediaCache* shared_media_cache = nullptr) noexcept;

    rhi::Device* device_{};
    RendererLimits limits_{};
    // Each layer recorded into one command list needs distinct upload buffers;
    // otherwise a later SceneRenderer upload could replace an earlier pass's
    // staging data before submission. The pool grows to the graph's stable
    // layer count and is capped by max_compositor_scene_layers.
    std::vector<SceneRenderer> layer_renderers_;
    // GPU media inputs do not: immutable source textures are safe to sample
    // from every isolated layer. An editor recording Program and Stage into one
    // command list can supply a wider shared cache; other callers use this one.
    MediaCache media_cache_;
    GaussianBlur blur_;
    SpatialBlur spatial_blur_;
    LayerKeyer keyer_;
    MaskRaster mask_raster_;
    LayerMatte matte_;
    rhi::Texture composite_a_;
    rhi::Texture composite_b_;
    rhi::GraphicsPipeline composite_pipeline_;
    rhi::Sampler sampler_;
    CompositorSceneStats stats_{};
    MediaCacheStats latest_media_cache_stats_{};
    bool output_is_a_{true};
};

} // namespace mediaserver::render
