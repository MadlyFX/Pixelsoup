#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/media_cache.hpp>
#include <mediaserver/render/model_loader.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

// The renderer: a `scene::Scene` and a rectangle of its canvas in, one
// rendered colour target out.
//
// This is the second half of the seam the implementation plan's §6.4 calls the
// critical one. The scene layer never learns that a GPU exists; this layer
// never reads a document. Everything crossing between them is a `Scene`, which
// is a value with no pointers into anything that could change underneath it.
//
// What it does per frame, in order:
//
//  1. Resolves every mesh key the scene references through a cache, generating
//     any that are missing. Geometry is procedural, so a cache miss costs a
//     generator call rather than a file read or a network fetch, and a miss
//     resolves identically on every node.
//  2. Writes one instance record per drawable. Opaque geometry remains batched
//     by mesh; composited geometry is sorted back-to-front with stable source
//     and insertion tie-breakers.
//  3. Narrows the camera to this node's rectangle - `scene::view_for_slice` -
//     and uploads the resulting matrix.
//  4. One depth-writing opaque pass followed by depth-tested, non-writing
//     compositing passes using each layer's blend equation.
//
// The camera is never adjusted for the node. That is the whole design: a node
// renders a sub-frustum of the one camera the cohort shares, so two nodes'
// outputs abut exactly rather than approximately, and neither node holds any
// state the other does not.
//
// What is deliberately absent, so that its absence is a decision rather than
// an oversight: frustum culling (the scenes are small and a wrong cull is
// worse than a slow frame), instancing beyond per-mesh batching, shadows, and
// any global illumination. Each of those has a place in the plan, and each of
// them lands behind this same interface.
namespace mediaserver::render {

// Fixed capacities. The renderer allocates once at construction and never
// again, so a scene past one of these is refused for the frame and counted
// rather than absorbed into an allocation on the frame path.
struct RendererLimits final {
    core::u32 max_instances{scene::max_scene_instances};
    // Enough for a few hundred primitives at a reasonable tessellation, or a
    // handful at a high one.
    core::u32 max_vertices{1U << 20U};
    core::u32 max_indices{1U << 21U};
    core::u32 max_lights{
        scene::max_scene_directional_lights + scene::max_scene_point_lights};

    bool operator==(const RendererLimits&) const = default;
};

// What one frame cost and what it left out. Reported because a renderer that
// quietly drops geometry looks exactly like a scene that is missing it.
struct RenderStats final {
    core::u32 batches{};
    core::u32 instances{};
    // Media clips drawn this frame, each in its own textured pass.
    core::u32 media{};
    core::u32 triangles{};
    core::u32 meshes_resident{};
    // Instances drawn only to prime depth for this pass. They emit no colour
    // and no alpha, so they are counted separately from the drawables the pass
    // is actually responsible for.
    core::u32 depth_only_instances{};
    // Instances not drawn because the mesh arena had no room for their
    // geometry, or because the instance array was full.
    core::u32 dropped_instances{};
    core::u32 dropped_lights{};
    // True on a frame that re-uploaded the mesh arena, which happens when a
    // mesh the scene references was not yet resident. Zero on a settled show.
    bool geometry_uploaded{};
    core::u64 upload_bytes{};

    bool operator==(const RenderStats&) const = default;
};

// A fully resolved render viewpoint. Output projection policies construct this
// value without teaching the renderer about documents, wall surfaces, or feed
// routing. Eye and forward accompany the matrices because lighting and
// translucent sorting must use the same physical viewpoint as projection.
struct SceneRenderView final {
    scene::ViewMatrices matrices{};
    scene::Vec3 eye{};
    scene::Vec3 forward{0.0F, 0.0F, -1.0F};

    bool operator==(const SceneRenderView&) const = default;
};

struct SceneRenderOptions final {
    // Isolated compositor layers need uncovered pixels to remain transparent.
    // The ordinary scene path keeps its historical opaque sky clear.
    bool transparent_background{};
    // Empty keeps the original camera/canvas-slice projection. A physical
    // output surface supplies an explicit asymmetric view here.
    std::optional<SceneRenderView> view;
    // Document nodes drawn into this pass for their depth alone: geometry that
    // belongs to a *different* compositor layer but stands in front of this
    // one. They write depth and emit nothing, so the layer this pass owns is
    // occluded exactly as it would be in a single-pass render, and the flat
    // premultiplied composite that follows cannot float it over its occluders.
    //
    // Ascending document-id order. Sorted rather than merely unique because
    // the frame path looks each drawable up by binary search.
    std::span<const doc::NodeId> depth_only_sources;

    [[nodiscard]] bool depth_only(doc::NodeId source) const noexcept {
        return std::binary_search(depth_only_sources.begin(),
                                  depth_only_sources.end(), source);
    }

    // Written out because a span compares as a reference, not as a sequence,
    // and two option sets naming the same occluders are the same options.
    [[nodiscard]] bool
    operator==(const SceneRenderOptions& other) const noexcept {
        return transparent_background == other.transparent_background &&
            view == other.view &&
            std::equal(depth_only_sources.begin(), depth_only_sources.end(),
                       other.depth_only_sources.begin(),
                       other.depth_only_sources.end());
    }
};

class SceneRenderer final {
public:
    // Fails with ShaderFormatUnsupported on a build with no scene bytecode,
    // which is a normal outcome on a machine without a shader compiler.
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<SceneRenderer, RenderError> create(
        rhi::Device& device,
        core::u32 width,
        core::u32 height,
        const RendererLimits& limits = RendererLimits{});

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;
    SceneRenderer(SceneRenderer&&) = default;
    SceneRenderer& operator=(SceneRenderer&&) = default;
    ~SceneRenderer() = default;

    // The colour target this renderer draws into. Sampled and presentable; the
    // caller blits it to a swapchain or reads it back.
    [[nodiscard]] rhi::Texture& target() noexcept { return color_; }
    [[nodiscard]] core::u32 width() const noexcept { return color_.width(); }
    [[nodiscard]] core::u32 height() const noexcept { return color_.height(); }

    // Reallocates the colour and depth targets. A no-op when the size is
    // unchanged, so it is safe to call every frame from a resizable window.
    [[nodiscard]] bool resize(
        rhi::Device& device,
        core::u32 width,
        core::u32 height) noexcept;

    // Records one frame. `slice` is this node's rectangle of `scene.canvas`;
    // pass `scene::whole(scene.canvas)` to render the entire image on one
    // machine.
    //
    // A slice the canvas does not contain is refused rather than clamped: an
    // output mapped outside its canvas is a feed-map error, and rendering
    // something plausible would hide it.
    [[nodiscard]] bool render(
        rhi::CommandList& commands,
        const scene::Scene& scene,
        scene::CanvasRect slice);

    // Same scene draw, but with options needed by offscreen compositor layers.
    // Kept as an overload so every existing caller retains the opaque-sky
    // contract without changing source or behaviour.
    [[nodiscard]] bool render(
        rhi::CommandList& commands,
        const scene::Scene& scene,
        scene::CanvasRect slice,
        const SceneRenderOptions& options);

    [[nodiscard]] const RenderStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const MediaCacheStats& media_cache_stats() const noexcept {
        return media_cache_.stats();
    }
    [[nodiscard]] const RendererLimits& limits() const noexcept { return limits_; }

private:
    // Where one mesh lives in the shared index arena. Vertices are addressed
    // absolutely, so a batch needs no base-vertex fixup.
    struct MeshRange final {
        core::u32 first_index{};
        core::u32 index_count{};
    };

    struct ResidentModelPart final {
        MeshRange range{};
        scene::Material material{};
        bool has_material{};
        std::optional<core::u32> base_color_texture;
        std::optional<core::u32> metallic_roughness_texture;
        std::optional<core::u32> emissive_texture;
    };

    // A model is uploaded once but retained as several draw ranges so every
    // source material can supply its own PBR factors. Bounds stay resident for
    // the future culling/picking path rather than needing a second file parse.
    struct ResidentModel final {
        std::vector<ResidentModelPart> parts;
        std::vector<rhi::Texture> textures;
        ModelBounds bounds{};
    };

    // A drawable that needs destination colour: translucent Normal, Add,
    // Multiply, Screen, and every media texture (whose pixels may carry alpha).
    // Collected without consuming an instance slot, sorted deterministically,
    // then appended to the instance arena in draw order.
    struct CompositeDraw final {
        MeshRange range{};
        scene::MeshInstance instance{};
        std::array<const rhi::Texture*, 3U> material_textures{};
        const rhi::Texture* texture{};
        scene::BlendMode blend{scene::BlendMode::Normal};
        float depth{};
        core::u64 source_order{};
        core::u32 ordinal{};
        core::u32 first_instance{};
    };

    SceneRenderer(
        rhi::Device& device,
        RendererLimits limits,
        rhi::Texture color,
        rhi::Texture depth,
        rhi::GraphicsPipeline pipeline,
        std::array<rhi::GraphicsPipeline, 4U> blended_pipelines,
        std::array<rhi::GraphicsPipeline, 4U> media_blended_pipelines,
        rhi::GraphicsPipeline media_depth_pipeline,
        rhi::Sampler media_sampler,
        rhi::Texture material_fallback,
        rhi::StorageBuffer vertices,
        rhi::StorageBuffer indices,
        rhi::StorageBuffer instances,
        rhi::StorageBuffer lights) noexcept;

    // Generates and appends a mesh if it is not already resident. False when
    // the arena has no room, which the caller turns into dropped instances
    // rather than a failed frame.
    [[nodiscard]] bool ensure_resident(scene::MeshKey key);

    // Loads and appends an imported model's geometry if not already resident,
    // keyed by its source path. False when the file cannot be loaded or the
    // arena has no room; the caller draws nothing for that model.
    [[nodiscard]] bool ensure_model_resident(
        const std::string& source,
        rhi::CommandList& commands);

    // Appends one mesh to the vertex/index arenas, returning its range. False
    // when the arena is full. Shared by procedural and imported residency.
    [[nodiscard]] bool append_mesh(
        const scene::MeshData& mesh,
        MeshRange& range);

    // Borrowed. The device this renderer was created from, needed on the frame
    // path to create clip textures. It outlives the renderer by construction -
    // the presenter owns both and destroys the renderer first.
    rhi::Device* device_{};
    RendererLimits limits_{};
    rhi::Texture color_;
    rhi::Texture depth_;
    // Opaque lit geometry writes depth. Every destination-reading mode uses a
    // matching non-depth-writing pipeline from these arrays, indexed by the
    // scene BlendMode order (Normal, Add, Multiply, Screen).
    rhi::GraphicsPipeline pipeline_;
    std::array<rhi::GraphicsPipeline, 4U> blended_pipelines_;
    std::array<rhi::GraphicsPipeline, 4U> media_blended_pipelines_;
    // A clip standing in front of an isolated layer has to occlude it, and a
    // clip cannot join the batched opaque pass because it needs its own
    // texture. This pipeline writes depth, alpha-tests the clip, and composes
    // a zero-alpha colour, so it primes depth without touching the image.
    rhi::GraphicsPipeline media_depth_pipeline_;
    rhi::Sampler media_sampler_;
    // One white RGBA texel is neutral for all three multiplicative maps:
    // base colour, packed metallic-roughness, and emissive.
    rhi::Texture material_fallback_;
    bool material_fallback_uploaded_{};
    MediaCache media_cache_;
    rhi::StorageBuffer vertex_buffer_;
    rhi::StorageBuffer index_buffer_;
    rhi::StorageBuffer instance_buffer_;
    rhi::StorageBuffer light_buffer_;

    // CPU-side arenas. The mesh arenas are the authoritative copy and are
    // re-uploaded whole whenever they grow; the per-frame arenas are scratch
    // that is refilled and uploaded every frame.
    std::map<scene::MeshKey, MeshRange> resident_;
    // Imported model geometry in the same arenas, keyed by source path.
    std::map<std::string, ResidentModel> resident_models_;
    std::vector<core::u8> vertex_arena_;
    std::vector<core::u8> index_arena_;
    std::vector<core::u8> instance_scratch_;
    std::vector<core::u8> light_scratch_;
    std::vector<core::u8> batch_uniforms_;
    std::vector<rhi::DrawBatch> batches_;
    std::vector<rhi::SampledTexture> batch_textures_;
    std::vector<CompositeDraw> composite_draws_;
    // Clips drawn for depth only, in document order. Kept apart from the
    // composited clips because each needs its own depth-writing pass before
    // any destination-reading draw runs.
    std::vector<CompositeDraw> depth_only_media_;
    scene::MeshData mesh_scratch_;
    ModelAsset model_scratch_;
    bool geometry_dirty_{};

    RenderStats stats_{};
};

} // namespace mediaserver::render
