#pragma once

#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/gaussian_blur.hpp>
#include <mediaserver/render/layer_keyer.hpp>
#include <mediaserver/render/layer_matte.hpp>
#include <mediaserver/render/mask_raster.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>
#include <mediaserver/render/spatial_blur.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>
#include <string>
#include <vector>

namespace mediaserver::render {

// This executor accepts an image tree under Output built from SceneLayer,
// Matte, Background, Composite, LED Car Process and Merge. Effect and
// RadialArray inputs remain attached through typed ports. By default, source
// drawables absent from that Output path form an implicit bottom stack;
// OutputParameters::replace_scene explicitly suppresses it.
//
// A Composite's layers land in order on whatever is below them, as does an
// LED Car Process's plates. A Merge is an image of its own: its BASE and
// LAYERS are composited into a transparent intermediate target, which then
// lands as one image. So does a Composite or LED Car Process that a Merge
// lands as one of its LAYERS. Intermediate targets stack at most
// max_compositor_group_depth deep.
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

// The graph's own nesting limit, plus one: a Scene Layer whose drawable
// carries a blend mode of its own is isolated once more before a Merge lands
// it, which the graph cannot know about until the scene is evaluated.
inline constexpr core::u32 max_compositor_group_depth =
    compositor::max_merge_nesting + 1U;

// A generated image, composited straight onto the accumulator: no renderer,
// no depth, no target of its own.
struct CompositorBackgroundPass final {
    doc::NodeId graph_node{};
    compositor::BackgroundParameters parameters{};

    bool operator==(const CompositorBackgroundPass&) const = default;
};

enum class CompositorStepKind : core::u8 {
    // Render plan.layers[index] and land it on the open accumulator.
    Layer,
    // Generate plan.backgrounds[index] and land it on the open accumulator.
    Background,
    // Open a transparent accumulator; the steps that follow land on it.
    BeginGroup,
    // Close the open accumulator and land it, as one image, on the one it
    // was opened over.
    EndGroup,
};

// A Merge with nothing wired to LAYERS: the group it closes is blended
// against this constant before it lands, inside the group's own coverage.
struct CompositorConstantBlend final {
    compositor::MergeBlendMode blend{compositor::MergeBlendMode::Multiply};
    float value{1.0F};
    float opacity{1.0F};

    bool operator==(const CompositorConstantBlend&) const = default;
};

struct CompositorStep final {
    CompositorStepKind kind{CompositorStepKind::Layer};
    core::u32 index{};
    // How the image lands on the accumulator below it. BeginGroup ignores
    // both.
    compositor::MergeBlendMode blend{compositor::MergeBlendMode::Normal};
    float opacity{1.0F};
    // EndGroup only.
    std::optional<CompositorConstantBlend> constant;
    // The graph node this step realises, for diagnostics; zero for the
    // implicit pass-through stack.
    doc::NodeId graph_node{};

    bool operator==(const CompositorStep&) const = default;
};

// The image a Matte reads its coverage from: a Scene Layer rendered in
// isolation through its own filters and mask, or a generated Background.
struct CompositorMatteBranch final {
    doc::NodeId graph_node{};
    doc::NodeId scene_node{};
    // As CompositorLayerPass::scene_nodes: a Track's on-air clips.
    std::vector<doc::NodeId> scene_nodes;
    float blur_radius_px{};
    std::vector<scene::SpatialEffectOperation> spatial_effects;
    std::optional<scene::MaskProgram> mask_program;
    // Present when the matte is a Background; the scene fields are unused.
    std::optional<compositor::BackgroundParameters> background;

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
    std::vector<CompositorBackgroundPass> backgrounds;
    // The order images are produced and landed. Empty lands every layer, in
    // order, with its own blend - the plan the scene alone makes. Layer steps
    // name layers in ascending order, each exactly once.
    std::vector<CompositorStep> steps;
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
    // Intermediate images opened by Merges, and Background images generated.
    core::u32 merge_groups{};
    core::u32 background_passes{};
    // The deepest intermediate stack this frame opened.
    core::u32 group_depth{};
    // Frames whose background was the authored environment rather than the
    // flat clear. Counted because "the sky is missing" and "the sky is drawn
    // but looks flat" are different faults with the same symptom.
    core::u64 environment_backgrounds{};
    // Atmosphere and bloom, applied here to the finished composite rather than
    // inside each layer's render. Refusals are counted for the reason the
    // renderer counts its own: a pass present on one node and absent on its
    // neighbour is a seam down the middle of the wall.
    core::u64 fog_volumes{};
    core::u64 fog_refusals{};
    core::u64 bloom_chains{};
    core::u64 bloom_refusals{};
    core::u64 plate_dof_passes{};
    core::u64 plate_dof_refusals{};

    bool operator==(const CompositorSceneStats&) const = default;
};

using CompositorSceneResult = core::Result<const rhi::Texture*, RenderError>;

class CompositorSceneExecutor final {
  public:
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<CompositorSceneExecutor, RenderError>
    create(rhi::Device& device, core::u32 width, core::u32 height,
           const RendererLimits& limits = RendererLimits{},
           MediaCacheConfig media_cache = MediaCacheConfig{},
           rhi::TextureFormat working_format =
               rhi::TextureFormat::Rgba8Unorm);

    CompositorSceneExecutor(const CompositorSceneExecutor&) = delete;
    CompositorSceneExecutor& operator=(const CompositorSceneExecutor&) = delete;
    CompositorSceneExecutor(CompositorSceneExecutor&&) = default;
    CompositorSceneExecutor& operator=(CompositorSceneExecutor&&) = default;
    ~CompositorSceneExecutor() = default;

    // The extent this executor presents, which is the authored slice's extent
    // and is not the extent it renders at.
    //
    // Its internal targets carry the slice apron - the neighbouring pixels a
    // blur or any other neighbourhood pass needs to produce the same answer at
    // a node boundary that a full-canvas render would - and are therefore
    // larger whenever a scene authors one. The apron is cropped away once, at
    // this boundary, so nothing downstream sees or needs to know about it.
    [[nodiscard]] core::u32 width() const noexcept { return output_.width(); }
    [[nodiscard]] core::u32 height() const noexcept { return output_.height(); }
    // What the internals are actually sized to. Equal to width()/height() when
    // no apron is authored, which is the ordinary case and costs nothing.
    [[nodiscard]] core::u32 padded_width() const noexcept {
        return composite_a_.width();
    }
    [[nodiscard]] core::u32 padded_height() const noexcept {
        return composite_a_.height();
    }
    // The most recent successful execute result. Before the first execute it
    // names the initial accumulator target; prefer execute().value() when
    // recording and use this accessor for presenter-style handoff afterward.
    [[nodiscard]] const rhi::Texture& target() const noexcept {
        return output_;
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

    [[nodiscard]] core::u64 temporal_restarts() const noexcept { return temporal_restarts_; }
    [[nodiscard]] core::u64 temporal_restart_frame() const noexcept { return temporal_restart_frame_; }
    [[nodiscard]] doc::Version temporal_document_version() const noexcept { return temporal_document_version_; }

    [[nodiscard]] const CompositorSceneStats& stats() const noexcept {
        return stats_;
    }
    [[nodiscard]] const RendererReadiness& renderer_readiness()
        const noexcept {
        return last_readiness_;
    }
    // Digest of the exact authored RendererSettings associated with
    // renderer_readiness(). A stale GPU slot may leave both values unchanged;
    // callers compare the digest before treating the readiness as current.
    [[nodiscard]] core::u64 renderer_profile_digest() const noexcept {
        return last_renderer_profile_digest_;
    }
    // Which authored passes ran on the last frame this executor recorded.
    //
    // Carried out of the renderer rather than re-derived here: only the
    // renderer knows whether a pass refused, and a second implementation of
    // that judgement would eventually disagree with the first.
    [[nodiscard]] const RendererPassStatus& renderer_passes() const noexcept {
        return last_passes_;
    }

    void observe_completed_timing(const core::FrameTiming& timing) noexcept {
        for (auto& renderer : layer_renderers_) renderer.observe_completed_timing(timing);
    }
    [[nodiscard]] bool scene_linear_output() const noexcept {
        return last_scene_linear_;
    }

    // Snapshot of the GPU media residency used by the latest execution. Layers
    // share exact source frames; CPU decode residency is process-wide and
    // reported separately by media_decode_stats().
    [[nodiscard]] MediaCacheStats media_cache_stats() const noexcept;
    [[nodiscard]] ModelCacheStats model_cache_stats() const noexcept;
    // Explicit operator/control-path refresh. Decode remains asynchronous and
    // every layer renderer observes the one shared source generation.
    [[nodiscard]] bool refresh_model_source(
        const std::string& source) noexcept;

  private:
    CompositorSceneExecutor(rhi::Device& device, RendererLimits limits,
                            MediaCacheConfig media_cache,
                            std::vector<HybridSceneRenderer> layer_renderers,
                            GaussianBlur blur, SpatialBlur spatial_blur,
                            LayerKeyer keyer, MaskRaster mask_raster,
                            LayerMatte matte,
                            rhi::Texture composite_a,
                             rhi::Texture composite_b,
                             rhi::Texture output,
                             rhi::GraphicsPipeline composite_pipeline,
                             rhi::GraphicsPipeline merge_pipeline,
                             rhi::Sampler sampler,
                             rhi::TextureFormat working_format) noexcept;

    // Reallocates everything that carries the apron to `width` x `height`,
    // leaving the presented output extent alone. Transactional: a failed
    // allocation leaves the previous set usable, because a half-resized
    // compositor would draw one layer at the wrong extent.
    [[nodiscard]] bool reserve_padded(core::u32 width,
                                      core::u32 height) noexcept;

    // The scene-level effects at the padded extent. Their absence is not a
    // failure here - a device without compute has none - but an authored pass
    // is then refused at execution rather than skipped.
    void create_effects(core::u32 width, core::u32 height) noexcept;

    [[nodiscard]] CompositorSceneResult
    execute_plan(rhi::CommandList& commands, const CompositorScenePlan& plan,
                 scene::CanvasRect slice, rhi::Color initial_clear,
                 const std::optional<SceneRenderView>& view =
                     std::nullopt,
                 MediaCache* shared_media_cache = nullptr) noexcept;

    FrameGraph graph_;
    rhi::Device* device_{};
    RendererLimits limits_{};
    // Each layer recorded into one command list needs distinct upload buffers;
    // otherwise a later SceneRenderer upload could replace an earlier pass's
    // staging data before submission. The pool grows to the graph's stable
    // layer count and is capped by max_compositor_scene_layers.
    std::vector<HybridSceneRenderer> layer_renderers_;
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
    // The cropped result, at the authored slice's extent. Separate from the
    // accumulators because they carry the apron and this does not.
    rhi::Texture output_;
    // Atmosphere and bloom, owned here because they apply to the composite:
    // atmosphere is light scattered by the air between the camera and whatever
    // it can see, not a property of one layer, and applying it inside each
    // layer's render fogged a shared background once per layer. Both are
    // sized to the padded extent with everything else that carries the apron.
    std::optional<VolumetricFog> fog_;
    std::optional<Bloom> bloom_;
    std::optional<ObjectMotionBlur> motion_blur_;
    std::optional<PlateDepthOfField> plate_dof_;
    // The nearest surface any layer drew, for the atmosphere to resolve
    // against. Merged as each layer lands. A storage-writable R32F rather than
    // a depth attachment, because a compute merge cannot write one of those.
    rhi::Texture scene_depth_;
    std::optional<rhi::ComputePipeline> depth_merge_;
    rhi::GraphicsPipeline composite_pipeline_;
    // Merge blend modes, opacity, a constant, and generated Backgrounds. The
    // four blends composite_pipeline_ also has go through it instead, so a
    // graph without a Merge renders exactly as it always did.
    rhi::GraphicsPipeline merge_pipeline_;
    // Intermediate accumulators, a ping-pong pair per open group level, at
    // the padded extent. Allocated the first frame a depth is needed and
    // dropped whenever the padded extent changes.
    std::vector<rhi::Texture> group_targets_;
    // A Matte's coverage image, held apart from the filters' and keyers' own
    // targets so filtering the source cannot overwrite it, and a transparent
    // target a Background matte is generated over. Padded extent; allocated
    // the first frame a Matte runs and dropped with the padded extent.
    rhi::Texture matte_image_;
    rhi::Texture matte_blank_;
    rhi::Sampler sampler_;
    rhi::TextureFormat working_format_{rhi::TextureFormat::Rgba8Unorm};
    CompositorSceneStats stats_{};
    MediaCacheStats latest_media_cache_stats_{};
    RendererReadiness last_readiness_{};
    // Adapter-local bytes the last added layer renderer took, and the padded
    // extent it was built at. Zero until one has been built; a different
    // extent costs a different amount, so it is measured again there.
    core::u64 layer_renderer_bytes_{};
    core::u32 layer_renderer_bytes_width_{};
    core::u32 layer_renderer_bytes_height_{};
    RendererPassStatus last_passes_{};
    core::u64 last_renderer_profile_digest_{};
    doc::Version temporal_document_version_{};
    core::u64 temporal_restart_baseline_{};
    core::u64 temporal_restarts_{};
    core::u64 temporal_restart_frame_{};
    bool output_is_a_{true};
    bool last_scene_linear_{};
};

} // namespace mediaserver::render
