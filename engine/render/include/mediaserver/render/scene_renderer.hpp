#pragma once
#include <mediaserver/render/frame_graph.hpp>

#include <mediaserver/render/plate_depth_of_field.hpp>

#include <mediaserver/render/material_sampling.hpp>

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/geometry_arena.hpp>
#include <mediaserver/render/geometry_residency.hpp>
#include <mediaserver/render/media_cache.hpp>
#include <mediaserver/render/model_cache.hpp>
#include <mediaserver/render/model_animation.hpp>
#include <mediaserver/render/texture_streaming.hpp>
#include <mediaserver/render/model_loader.hpp>
#include <mediaserver/render/geometry_visibility.hpp>
#include <mediaserver/render/ambient_occlusion.hpp>
#include <mediaserver/render/temporal_resolve.hpp>
#include <mediaserver/render/shadow_atlas.hpp>
#include <mediaserver/render/environment_cache.hpp>
#include <mediaserver/render/ies_profile.hpp>
#include <mediaserver/render/particle_package_cache.hpp>
#include <mediaserver/render/particle_pass.hpp>
#include <mediaserver/render/ltc_asset.hpp>
#include <mediaserver/render/baked_lighting.hpp>
#include <mediaserver/render/environment_prefilter.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/scene.hpp>

#include <algorithm>
#include <array>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
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
// Camera submissions are culled against the full authored canvas. Each shadow
// map draws the casters its own projection reaches, and is kept from an
// earlier frame while everything that draw reads is unchanged; a renderer on
// the same device whose draw would read the same samples that map rather than
// drawing its own, and model textures are resident once per device. Shared
// mesh/model parts instance in stable asset/part/node order, and authored LOD
// chains select without history. Batched opaque passes draw indexed where the
// device can. Occlusion culling and GPU-driven indirect submission remain
// deferred.
namespace mediaserver::render {

// Fixed capacities. The renderer allocates once at construction and never
// again, so a scene past one of these is refused for the frame and counted
// rather than absorbed into an allocation on the frame path.
struct RendererLimits final {
    // Internal construction bound: a reflection capture cannot recurse.
    bool planar_reflections{true};
    // Builds the wireframe pipelines an editor view's ViewShading::Wireframe
    // draws with. Off for every output node, which only ever renders Lit.
    bool view_shading{};
    core::u32 max_instances{scene::max_scene_instances};
    // One tiny CPU history slot per independently mapped output/view. The
    // default covers the replicated feed-map ceiling without duplicating GPU
    // render targets or storage buffers for every map.
    core::u32 max_history_slots{256U};
    // Surfaces that may hold a temporal colour history at once. Far smaller
    // than the CPU slot count above because each one is two full-extent
    // targets rather than a few dozen bytes; Program, Preview, Stage and a
    // mapped view is the shape of a real show.
    core::u32 max_temporal_history_targets{4U};
    // Same-source model refreshes append immutable geometry and retain the old
    // GPU textures so no command list can outlive a resource it referenced.
    // Once this bounded retirement pool is full, the last complete GPU model
    // remains live until renderer reconstruction.
    core::u32 max_model_refresh_generations{64U};
    // Total GPU texture envelope. Advanced divides this into a working set
    // and transactional replacement headroom; Basic retains full uploads.
    // Pending and fence-retired textures count against the same envelope.
    // Advanced textures are resident once per device (TextureResidency):
    // one another renderer made counts against this envelope all the same.
    core::u64 max_model_texture_bytes{
        static_cast<core::u64>(scene::max_texture_budget_mib) * 1024ULL * 1024ULL};
    core::u32 max_model_textures{4096U};
    // The geometry arena's largest size, in elements. It lives in device
    // memory, starts small and grows only when content needs it, and holds
    // every resident LOD and refresh generation still in use. These fit a
    // ~30M-triangle scene with room for refreshes. Skin records exist only
    // for skinned meshes. Renderers on one device with equal arena limits
    // share one arena (GeometryResidency), so a mesh several of them draw
    // is resident once.
    core::u32 max_vertices{48U << 20U};
    core::u32 max_indices{144U << 20U};
    core::u32 max_skin_vertices{4U << 20U};
    // Where the arena starts; it doubles as content needs.
    core::u32 initial_vertices{1U << 18U};
    core::u32 initial_indices{3U << 18U};
    // Geometry bytes one renderer streams into its arena per recording. A
    // larger model integrates over several frames and reports pending until
    // all of it has landed.
    core::u64 geometry_upload_bytes{48ULL << 20U};
    // Model texture bytes Basic uploads per recording, within what the
    // command list's upload pages still hold. A model with more integrates
    // over several frames and reports pending until all of it has landed.
    // Advanced streams its textures through TextureResidency instead.
    core::u64 texture_upload_bytes{16ULL << 20U};
    core::u32 max_lights{
        scene::max_scene_directional_lights + scene::max_scene_point_lights};
    // Per-cascade resolution of the directional shadow products shared by
    // every output slice. This is a deployment memory bound, not a look
    // control: every node in a cohort runs the same build and therefore
    // allocates the same maps. How many of them a show actually uses is
    // authored in `scene::RendererSettings::shadow_cascades`.
    core::u32 directional_shadow_resolution{1'024U};
    // The local-light shadow atlas. One texture partitioned into tiles rather
    // than a map per light: sixty four separate targets is sixty four
    // allocations and sixty four bindings, and a single atlas is what makes a
    // bounded budget expressible at all. Every node in a cohort runs the same
    // build and therefore partitions the same atlas.
    ShadowAtlasLimits shadow_atlas{};
    // Cascade maps allocated once at construction. The authored cascade count
    // is bounded by this, so a shadow product can never be created while a
    // frame is being recorded.
    core::u32 max_directional_shadow_cascades{scene::max_shadow_cascades};
    // Inline effects are uploaded as one bounded program shared by all draw
    // types. Two legacy scalar controls per drawable plus every first-class
    // scene/graph effect fit without reserving fixed parameter lanes in every
    // instance record.
    core::u32 max_inline_effect_ops{
        scene::max_scene_effects + scene::max_scene_instances * 2U + 1'024U};
    // A scene may contribute 512 first-class effects and a graph up to 1024
    // nodes. Curves consumes one fixed LUT slot; this explicit cap keeps a
    // hostile document from turning the frame path into an unbounded upload.
    core::u32 max_inline_curve_luts{scene::max_scene_effects + 1'024U};
    core::u32 max_inline_effect_parameter_vectors{
        (scene::max_scene_effects + scene::max_scene_instances * 2U + 1'024U)
            * 8U
        + (scene::max_scene_effects + 1'024U) * scene::curve_lut_entries};

    bool operator==(const RendererLimits&) const = default;
};

// What one frame cost and what it left out. Reported because a renderer that
// quietly drops geometry looks exactly like a scene that is missing it.
struct RenderStats final {
    FrameGraphStats graph{};
    core::u32 reflection_probes{}, reflection_refusals{}, planar_passes{}, planar_refusals{};
    core::u32 physical_sky_updates{};
    std::string_view reflection_reason{};
    core::AssetSetReadiness assets{};
    bool baked_ready{true};
    ProbeCacheState baked_state{ProbeCacheState::Unseen};
    core::u32 baked_probes{};
    core::u32 batches{};
    core::u32 instances{};
    // Media clips drawn this frame, each in its own textured pass.
    core::u32 media{};
    core::u32 offline_media{};
    // Media quads proved to be wholly outside this render view. They are
    // intentionally not resolved through MediaCache, so an off-camera video
    // does not consume a decoder merely because it exists in the scene.
    core::u32 culled_media{};
    core::u32 culled_geometry{};
    core::u32 visible_geometry{};
    core::u32 camera_instances{};
    core::u32 camera_triangles{};
    core::u32 instanced_batches{};
    core::u32 lod_models{};
    core::u32 lod_reductions{};
    core::u32 triangles{};
    core::u32 meshes_resident{};
    core::u32 models_pending{};
    core::u32 models_failed{};
    core::u32 models_stale{};
    core::u32 model_integration_attempts{};
    core::u32 model_integrations{};
    core::u32 model_integration_refusals{};
    core::u64 model_texture_bytes{};
    TextureStreamingStats texture_streaming{};
    // Instances drawn only to prime depth for this pass. They emit no colour
    // and no alpha, so they are counted separately from the drawables the pass
    // is actually responsible for.
    core::u32 depth_only_instances{};
    // Flat blended layers (screens, cards) this recording had no budget left
    // to order by which side of the others they are on. They keep the
    // centre-distance order, which can draw a farther layer over a nearer one.
    core::u32 unordered_layers{};
    // Instances not drawn because the mesh arena had no room for their
    // geometry, or because the instance array was full.
    core::u32 dropped_instances{};
    core::u32 dropped_lights{};
    // Local lights holding a shadow atlas tile this frame, and the ones that
    // asked and did not get one. The refusal count is the important half: the
    // same lights are refused on every node, so the wall stays consistent, but
    // a practical that silently stopped casting is an hour of an operator's
    // day if nothing says so.
    core::u32 shadow_tiles{};
    core::u32 shadow_lights_refused{};
    core::u32 directional_shadow_casters{};
    // Caster instances drawn into shadow maps this frame, over every cascade
    // and atlas tile. A caster wholly outside a map's projection is left out
    // of that map, which cannot change it.
    core::u32 shadow_caster_instances{};
    // Shadow products (cascades, the reflective-shadow-map pair, the local
    // atlas) this recording sampled without drawing: kept from an earlier
    // frame because nothing they are drawn from changed, or drawn by another
    // renderer on the device whose draw read exactly the same.
    core::u32 shadow_maps_reused{};
    // The part of those another renderer drew.
    core::u32 shadow_maps_shared{};
    std::array<scene::Mat4, scene::max_shadow_cascades> directional_shadow_fit{};
    // Cascades actually rendered this frame. Zero when the show authored no
    // directional shadows, when no directional light exists, or when the
    // light fit could not be built; it is never reduced to fit a budget.
    core::u32 directional_shadow_cascades{};
    bool directional_shadow_rendered{};
    bool directional_gi_rendered{};
    core::u32 contact_shadow_lights{};
    core::u32 inline_effect_ops{};
    // True on a frame that re-uploaded the mesh arena, which happens when a
    // mesh the scene references was not yet resident. Zero on a settled show.
    bool geometry_uploaded{};
    // Device memory of the geometry arena this renderer draws from, and the
    // part of it holding meshes. Shared by every renderer on the device with
    // the same arena limits, so these count it once for all of them.
    core::u64 geometry_capacity_bytes{};
    core::u64 geometry_used_bytes{};
    core::u64 upload_bytes{};
    core::u32 area_lights{}, area_shadow_requests{};
    bool ltc_ready{};
    core::u64 ltc_generation{};
    core::u32 ies_ready{}, ies_pending{}, ies_failed{}, ies_refused{};
    IesError ies_error{IesError::None};
    // Particle effects drawn this view, and the ones holding readiness back:
    // a package still loading or a new instance waiting its turn, a package
    // or instance that failed, a renderer that cannot run them, and a seek
    // further than one frame's step budget still catching up. The reason for
    // the first failure or refusal, if any.
    core::u32 particles_drawn{}, particles_pending{}, particles_failed{};
    core::u32 particles_unsupported{}, particles_behind{};
    std::string particle_issue;

    bool operator==(const RenderStats&) const = default;
};

// How an editor view shades the scene. Lit is the picture the show renders
// and the only shading an output ever uses; the others are authoring aids for
// the operator's own Stage view and never enter a document or a digest.
enum class ViewShading : core::u8 {
    Lit,
    // Base colour and emission as they are, with no lighting.
    Unlit,
    // Every surface a neutral grey dielectric under the scene's own lights
    // and shadows; clips are flat grey cards.
    LightsOnly,
    // Triangle edges only, every edge visible through the others. Needs a
    // renderer built with RendererLimits::view_shading.
    Wireframe,
    // Each surface's first texture coordinates as an eight-by-eight checker,
    // red rising with U and green with V, so a stretched, flipped or missing
    // mapping shows. Unlit. Needs RendererLimits::view_shading.
    UvChecker,
    // The shaded world-space normal, normal map included, as a colour: x, y
    // and z from -1..1 to 0..1 on screen. Unlit. Needs
    // RendererLimits::view_shading.
    WorldNormals,
};

// Whether a view shading draws the scene's light: Lit and Lights only do;
// the others show a property of the surface instead, so atmosphere and
// mirrors have nothing to add to them.
[[nodiscard]] constexpr bool view_shading_lit(ViewShading shading) noexcept {
    return shading == ViewShading::Lit || shading == ViewShading::LightsOnly;
}

struct SceneClipPlanes final {
    float near_plane{};
    float far_plane{};
    bool operator==(const SceneClipPlanes&) const = default;
};

// A fully resolved render viewpoint. Output projection policies construct this
// value without teaching the renderer about documents, wall surfaces, or feed
// routing. Eye and forward accompany the matrices because lighting and
// translucent sorting must use the same physical viewpoint as projection.
struct SceneRenderView final {
    scene::ViewMatrices matrices{};
    scene::Vec3 eye{};
    scene::Vec3 forward{0.0F, 0.0F, -1.0F};
    // Stable calibration/mapping identity, not the animated matrices above.
    // A document revision resets a calibration edit; camera motion remains a
    // sequential view change and therefore produces motion vectors.
    core::u64 history_identity{};
    // The same camera and jitter projected over the *whole* canvas rather than
    // this slice of it. Motion vectors are expressed against it so every node
    // describes a displacement with the same number; `matrices` above is what
    // actually rasterises. This is derived rather than authored:
    // `resolve_render_view` overwrites whatever a caller supplying an explicit
    // view left here, and callers brace-initialise the fields above positionally.
    scene::Mat4 canvas_view_projection{};
    // Near/far of an override whose depth range differs from the authored
    // camera (the free Stage viewport, for example). Empty uses the authored
    // range, as mapped and tracked views historically do.
    std::optional<SceneClipPlanes> clip_planes;
    // The compositor widens this projection by its filter support and crops
    // once at the end. History identity continues to describe the visible crop.
    bool guard_band{};
    // Tracked plates use their own physical shutter instead of the outer camera.
    std::optional<float> shutter_seconds;
    // Present only for an ICVFX plate, including when its lens data is missing.
    std::optional<PlateLens> plate_lens;
    // An editor view's shading aid; every output view is Lit.
    ViewShading shading{ViewShading::Lit};
    // An editor view's isolation aid: draw only this part of every model, by
    // its index among the package's full-detail parts, at full detail. Empty
    // draws every part; every output view leaves it empty.
    std::optional<core::u32> model_part;

    bool operator==(const SceneRenderView&) const = default;
};

// How the owning surface's already-decided temporal state applies to this
// recording. Direct SceneRenderer callers retain Automatic; the
// HybridSceneRenderer supplies one of the explicit actions so the velocity
// product cannot disagree with its published HistoryDecision.
// Applied to whichever projection this recording resolves. Zero is no jitter
// and reproduces an untemporal frame exactly, so a caller that never sets it
// is unchanged down to the bit.
//
// Owned by the facade for a Hybrid render, like `history` below: deriving it
// twice from the show frame would be two chances to disagree with the history
// decision it has to match.
enum class SceneHistoryAction : core::u8 {
    Automatic,
    Initialize,
    Advance,
    Hold,
    Reset,
};

struct SceneRenderOptions final {
    const rhi::Texture* shared_planar{};
    bool reflection_pass{};
    bool full_canvas_view{};
    scene::Vec4 clip_plane{};
    doc::NodeId excluded_source{};
    // Isolated compositor layers need uncovered pixels to remain transparent.
    // The ordinary scene path keeps its historical opaque sky clear.
    bool transparent_background{};
    // Complete authored scene before compositor isolation. Streaming must
    // budget every texture even when this draw only owns a subset of layers.
    const scene::Scene* residency_scene{};
    // Empty keeps the original camera/canvas-slice projection. A physical
    // output surface supplies an explicit asymmetric view here.
    std::optional<SceneRenderView> view;
    // Sub-pixel offset for temporal antialiasing, in canvas pixels. A
    // canvas-slice projection rebuilds its window around it; an explicit view
    // has only a matrix left and is offset in its own target pixels.
    scene::SubpixelJitter jitter{};
    // Document nodes drawn into this pass for their depth alone: geometry that
    // belongs to a *different* compositor layer but stands in front of this
    // one. They write depth and emit nothing, so the layer this pass owns is
    // occluded exactly as it would be in a single-pass render, and the flat
    // premultiplied composite that follows cannot float it over its occluders.
    //
    // Ascending document-id order. Sorted rather than merely unique because
    // the frame path looks each drawable up by binary search.
    std::span<const doc::NodeId> depth_only_sources;
    SceneHistoryAction history{SceneHistoryAction::Automatic};
    // Selected by HybridSceneRenderer from the complete HistoryKey. Direct
    // SceneRenderer callers use slot zero. The slot stores only temporal CPU
    // state; all GPU targets remain reusable between sequential mapped views.
    core::u32 history_slot{};

    [[nodiscard]] bool depth_only(doc::NodeId source) const noexcept {
        return std::binary_search(depth_only_sources.begin(),
                                  depth_only_sources.end(), source);
    }

    // Written out because a span compares as a reference, not as a sequence,
    // and two option sets naming the same occluders are the same options.
    [[nodiscard]] bool
    operator==(const SceneRenderOptions& other) const noexcept {
        return transparent_background == other.transparent_background &&
            shared_planar == other.shared_planar && reflection_pass == other.reflection_pass && full_canvas_view == other.full_canvas_view && clip_plane == other.clip_plane &&
            excluded_source == other.excluded_source &&
            residency_scene == other.residency_scene &&
            view == other.view &&
            history == other.history &&
            history_slot == other.history_slot &&
            std::equal(depth_only_sources.begin(), depth_only_sources.end(),
                       other.depth_only_sources.begin(),
                       other.depth_only_sources.end());
    }
};

enum class SceneRendererMode : core::u8 {
    BasicDisplay,
    AdvancedLinear,
};

class SceneRenderer final {
    friend class ConformanceHarness;
public:
    // Fails with ShaderFormatUnsupported on a build with no scene bytecode,
    // which is a normal outcome on a machine without a shader compiler.
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<SceneRenderer, RenderError> create(
        rhi::Device& device,
        core::u32 width,
        core::u32 height,
        const RendererLimits& limits = RendererLimits{},
        SceneRendererMode mode = SceneRendererMode::BasicDisplay);

    SceneRenderer(const SceneRenderer&) = delete;
    SceneRenderer& operator=(const SceneRenderer&) = delete;
    SceneRenderer(SceneRenderer&&) = default;
    SceneRenderer& operator=(SceneRenderer&&) = default;
    ~SceneRenderer() = default;

    // The colour target this renderer draws into. Sampled and presentable; the
    // caller blits it to a swapchain or reads it back.
    [[nodiscard]] rhi::Texture& target() noexcept { return color_; }
    [[nodiscard]] const rhi::Texture* planar_target() const noexcept { return planar_texture_; }
    // Build the planar mirror, if these limits allow one and none exists yet.
    // Call it from the scene's adoption and never from a recording: the
    // mirror is a second complete SceneRenderer at this renderer's extent.
    void ensure_planar() noexcept;
    // Whether that mirror exists. A scene that authors no planar reflection
    // leaves it absent, which is the point of building it on demand.
    [[nodiscard]] bool planar_resident() const noexcept {
        return planar_renderer_ != nullptr;
    }
    // Exact depth tested while producing target(). Isolated colour processors
    // borrow this texture as a pair so subsequent keyed operations retain the
    // original per-fragment scene-depth relationship.
    [[nodiscard]] rhi::Texture& depth_target() noexcept { return depth_; }
    // The Advanced renderer reconstructs the nearest visible raster surface
    // while producing object ID and velocity. Unlike the colour-pass depth,
    // this includes composited media only where its sampled premultiplied
    // coverage is nonzero. Basic has no separate product and returns the depth
    // it tested while drawing colour.
    [[nodiscard]] rhi::Texture& product_depth_target() noexcept {
        return scene_linear() ? auxiliary_depth_ : depth_;
    }
    [[nodiscard]] core::u64 occlusion_temporal_restarts() const noexcept {
        return temporal_occlusion_ ? temporal_occlusion_->stats().restarts : 0U;
    }

    // RGBA32F: UV motion in xy; current and expected previous reversed Z in zw.
    [[nodiscard]] rhi::Texture& velocity_target() noexcept {
        return velocity_;
    }
    [[nodiscard]] rhi::Texture& object_velocity_target() noexcept {
        return object_velocity_;
    }
    [[nodiscard]] rhi::Texture& object_id_target() noexcept {
        return object_id_;
    }
    // Octahedral world normals, for occlusion now and screen-space
    // reflections later. Written by the same auxiliary replay as identity and
    // motion, under the same coverage rules.
    [[nodiscard]] rhi::Texture& normal_target() noexcept { return normal_; }
    // The lights this frame uploaded, and the atlas tiles the casting ones
    // got, so a pass outside the lit draw can evaluate the same lights against
    // the same shadows. Volumetric fog needs exactly that: a practical has to
    // light the air the way it lights the surfaces, and be occluded by the same
    // wall.
    [[nodiscard]] const rhi::Texture* fog_environment() const noexcept {
        return environment_ && environment_generation_ != 0U
            ? environment_->maps().irradiance : nullptr;
    }
    [[nodiscard]] const rhi::StorageBuffer& light_storage() const noexcept {
        return light_buffer_;
    }
    [[nodiscard]] const rhi::StorageBuffer& light_shadow_storage()
        const noexcept {
        return light_shadow_buffer_;
    }
    [[nodiscard]] core::u32 light_storage_count() const noexcept {
        return uploaded_light_count_;
    }

    // The maps the last recording sampled, which another renderer on this
    // device may have drawn.
    [[nodiscard]] const rhi::Texture& cascade_target(core::u32 index) const noexcept {
        return (cascades_[index] ? cascades_[index] : cascade_homes_[index])->textures[0U];
    }
    [[nodiscard]] rhi::Texture& shadow_atlas_target() noexcept {
        return atlas_ ? atlas_->textures[0U]
            : atlas_home_ ? atlas_home_->textures[0U] : absent_atlas_;
    }
    // Screen-space occlusion, present only when the device can run compute and
    // the scene authored a radius. Absent is not an error - it is a scene that
    // did not ask, or a device that cannot.
    // Null unless this recording actually produced one. The texture outlives
    // a frame, so publishing it whenever it exists would hand a consumer last
    // frame's occlusion for a scene that has since turned occlusion off.
    [[nodiscard]] rhi::Texture* occlusion_target() noexcept {
        return occlusion_recorded_ && occlusion_.has_value()
            ? &occlusion_->target()
            : nullptr;
    }
    [[nodiscard]] rhi::Texture& coverage_target() noexcept {
        return coverage_;
    }
    [[nodiscard]] bool scene_linear() const noexcept {
        return mode_ == SceneRendererMode::AdvancedLinear;
    }
    [[nodiscard]] bool complete_products() const noexcept {
        return scene_linear() && velocity_ && object_id_ && normal_
            && coverage_
            && auxiliary_depth_ && shadow_cascades_available() != 0U
            && shadow_depth_ && shadow_pipeline_ && shadow_sampler_
            && rsm_home_ && rsm_normal_pipeline_
            && rsm_flux_pipeline_;
    }
    // Cascade maps this renderer allocated. The authored count is bounded by
    // it, which is deterministic across a cohort because the bound is a build
    // constant rather than a per-machine observation. `RenderStats` reports
    // the count actually rendered so a lowered deployment bound is visible.
    [[nodiscard]] core::u32 shadow_cascades_available() const noexcept {
        return static_cast<core::u32>(cascade_homes_.size());
    }
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

    // Records through a cache whose frame lifetime is owned by the caller.
    // This lets several compositor layers and editor viewports reuse one GPU
    // upload of an identical (source, frame) while keeping their render targets
    // independent. The caller must bracket every command-list frame with the
    // cache's begin_frame()/end_frame().
    [[nodiscard]] bool render(
        rhi::CommandList& commands,
        const scene::Scene& scene,
        scene::CanvasRect slice,
        const SceneRenderOptions& options,
        MediaCache& media_cache);

    // Draws the environment as the background of a target this renderer does
    // not own, for a compositor whose layers are all transparent by contract
    // and so may none of them draw a sky of their own.
    //
    // Returns false when there is no environment resident, which is the signal
    // to keep whatever clear the caller would otherwise have used - a scene
    // with no map is unchanged, down to the bit.
    [[nodiscard]] bool record_background(
        rhi::CommandList& commands, rhi::Texture& target,
        const scene::Scene& scene, scene::CanvasRect slice,
        const SceneRenderOptions& options) noexcept;

    // Whether an environment is prefiltered and ready to be drawn as one.
    [[nodiscard]] const rhi::Texture& ies_atlas() const noexcept { return ies_atlas_; }
    [[nodiscard]] bool background_resident(const scene::Scene& scene) const noexcept;

    [[nodiscard]] const RenderStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const MediaCacheStats& media_cache_stats() const noexcept {
        return media_cache_.stats();
    }
    [[nodiscard]] std::span<const GeometrySelection> geometry_selection() const noexcept {
        return geometry_selection_;
    }

    [[nodiscard]] core::u32 resident_texture_mip(std::string_view source, core::u32 index) const noexcept {
        return texture_streamer_ ? texture_streamer_->first_mip(source, index) : 0U;
    }
    [[nodiscard]] ModelCacheStats model_cache_stats() const noexcept {
        return model_cache_.stats();
    }
    // Operator/control path only. Advances the process-shared decode
    // generation without reading the source or joining an in-flight decode.
    [[nodiscard]] bool refresh_model_source(
        const std::string& source) noexcept {
        return model_cache_.refresh(source);
    }
    [[nodiscard]] const RendererLimits& limits() const noexcept { return limits_; }

private:
    FrameGraph graph_;
    // Where one mesh lives in the shared index arena. Vertices are addressed
    // absolutely, so a batch needs no base-vertex fixup.
    struct MeshRange final {
        core::u32 first_index{};
        core::u32 index_count{};
    };

    // A procedural mesh (cube, plane, clip quad), resident in the shared
    // residency for its whole life.
    struct ResidentMesh final {
        MeshRange range{};
        GeometryResidency::Entry* entry{};
    };

    struct ResidentModelPart final {
        MeshRange range{};
        scene::Material material{};
        bool has_material{};
        std::optional<core::u32> base_color_texture;
        std::optional<core::u32> metallic_roughness_texture;
        std::optional<core::u32> normal_texture;
        std::optional<core::u32> occlusion_texture;
        std::optional<core::u32> emissive_texture;
        std::optional<core::u32> clearcoat_texture;
        std::optional<core::u32> clearcoat_roughness_texture;
        std::optional<core::u32> clearcoat_normal_texture;
        std::optional<core::u32> sheen_color_texture;
        std::optional<core::u32> sheen_roughness_texture;
        std::optional<core::u32> transmission_texture;
        std::optional<core::u32> specular_texture;
        std::optional<core::u32> specular_color_texture;
        std::optional<core::u32> anisotropy_texture;
        std::array<ModelSampler, 14U> samplers{};

    };

    // A model is uploaded once but retained as several draw ranges so every
    // source material can supply its own PBR factors. Bounds stay resident for
    // the future culling/picking path rather than needing a second file parse.
    struct ResidentModel final {
        std::vector<ResidentModelPart> parts;
        std::vector<rhi::Texture> textures;
        // Keeps this exact immutable CPU generation alive while its GPU
        // resources may be needed by another renderer/view in the same
        // recording. Cache turnover therefore cannot erase the only bytes a
        // cold sibling renderer needs behind an already-latched epoch.
        std::shared_ptr<const ModelAsset> decoded_asset;
        ModelBounds bounds{};
        core::u64 generation{};
        // GPU integration failures are sticky for the decoded generation.
        // Repeated frames retain stale geometry without retrying allocations,
        // arena growth, or uploads. Explicit ModelCache refresh advances it.
        core::u64 blocked_generation{};
        core::u64 texture_bytes{};
        // The arena slots this model's mesh (every part and LOD) occupies,
        // shared with any other renderer that integrated the same decoded
        // generation. Empty only for a refusal record.
        GeometryResidency::Handle geometry;
        // Recordings that drew this model or wrote its geometry and are not
        // yet submitted. A retired model's slots are reused only once none
        // remain; later copies then follow its last draw on the queue.
        mutable std::vector<rhi::CommandRecordingToken> uses;
        mutable bool untracked_use{};
    };

    struct OpaqueSubmission final {
        MeshRange range{};
        std::array<const rhi::Texture*, 14U> textures{};
        std::string_view asset;
        scene::MeshKey mesh{};
        doc::NodeId source{};
        core::u32 part{};
        core::u32 original_slot{};
        bool visible{};
        std::array<ModelSampler, 14U> samplers{};
        // Where the instance is, for testing it against shadow projections.
        scene::Mat4 world{};
        ModelBounds bounds{};
    };

    // One shadow view's casters: the opaque batches cut down to the runs of
    // instances its projection can reach.
    struct CasterList final {
        std::vector<rhi::DrawBatch> batches;
        std::vector<core::u8> uniforms;
        std::vector<rhi::SampledTexture> textures;
        core::u32 instances{};
    };

    struct PendingModelIntegration final {
        // std::map node addresses are stable across insertion and SceneRenderer
        // move transfers the nodes. The active value remains untouched until
        // submission, so a canceled replacement always preserves stale GPU
        // geometry.
        ResidentModel* active{};
        ResidentModel candidate;
        std::shared_ptr<const ModelAsset> decoded_asset;
        // The recording the candidate is drawable in; empty while its
        // geometry or textures are still streaming or after a canceled
        // recording.
        rhi::CommandRecordingToken recording;
        core::u64 generation{};
        bool blocked{};
        // Basic texture upload progress over (texture, mip, row), and the
        // recordings that carried each run of it. Rows land once their
        // recording is submitted; a canceled run is uploaded again.
        struct TextureWrite final {
            rhi::CommandRecordingToken recording;
            core::u64 begin{};
        };
        core::u64 texture_next{};
        std::vector<TextureWrite> texture_writes;
    };

    // What remains of a per-recording upload limit. Use restarts at zero
    // with each recording.
    struct RecordingBudget final {
        rhi::CommandRecordingToken recording;
        core::u64 used{};

        [[nodiscard]] core::u64 remaining(rhi::CommandList& commands, core::u64 limit);
    };

    enum class TextureUpload : core::u8 { Drawable, Pending, Failed };

    // A drawable that needs destination colour: translucent Normal, Add,
    // Multiply, Screen, and every media texture (whose pixels may carry alpha).
    // Collected without consuming an instance slot, sorted deterministically,
    // then appended to the instance arena in draw order.
    struct CompositeDraw final {
        MeshRange range{};
        scene::MeshInstance instance{};
        // Base color, packed metallic-roughness, tangent normal, occlusion,
        // and emissive. This order is the fragment descriptor contract.
        std::array<const rhi::Texture*, 14U> material_textures{};
        std::array<ModelSampler, 14U> samplers{};
        ResolvedMediaFrame media;
        scene::BlendMode blend{scene::BlendMode::Normal};
        float depth{};
        core::u64 source_order{};
        // Object/model siblings use their authored group_order when their
        // camera depths tie. The flag distinguishes top-level siblings (an
        // intentionally empty parent) from draws with no sibling domain.
        bool sibling_ordered{};
        std::optional<doc::NodeId> sibling_parent;
        core::i64 sibling_order{};
        core::u32 ordinal{};
        core::u32 first_instance{};
        core::u32 first_effect{};
        core::u32 effect_count{};
        // Where the geometry lies before `instance.world`; zero thickness
        // along an axis marks a flat surface, which other layers can then be
        // ordered against.
        ModelBounds bounds{
            scene::Vec3{-0.5F, -0.5F, -0.5F}, scene::Vec3{0.5F, 0.5F, 0.5F}};
    };

    // A composited layer's world-space extent and, for a flat one, its plane,
    // for ordering layers by which side of each other they are on.
    struct LayerVolume final {
        std::array<scene::Vec3, 8U> corners{};
        scene::Vec3 normal{};
        float offset{};
        // Signed: which side of the plane the viewer is on.
        float viewer_side{};
        float nearest{};
        float farthest{};
        bool planar{};
    };

    // Refines the centre-distance order of composite_draws_: a layer lying
    // wholly on the viewer's side of a flat layer's plane is drawn after it,
    // and a layer wholly behind one is drawn before it. Pairs with no such
    // relation - coplanar ones, or two that cross - keep the sorted order,
    // and so does any cycle.
    void order_composites_by_planes(const SceneRenderView& view) noexcept;

    struct InlineProgramRange final {
        core::u32 first{};
        core::u32 count{};
    };

    struct InlineProgramCacheEntry final {
        doc::NodeId source{};
        InlineProgramRange range{};
    };

    void bind_material_textures(std::span<rhi::SampledTexture> into,
        const std::array<const rhi::Texture*, 14U>& textures,
        const std::array<ModelSampler, 14U>& samplers, core::u32 anisotropy,
        const rhi::Texture& background) noexcept;
    // The Advanced descriptor lanes of the shadow products this recording
    // samples, decided only after the batches were assembled.
    void bind_shadow_products(std::span<rhi::SampledTexture> into) const noexcept;

    [[nodiscard]] bool prepare_physical_environment(
        rhi::CommandList& commands, const scene::Scene& scene) noexcept;

    SceneRenderer(
        rhi::Device& device,
        RendererLimits limits,
        rhi::Texture color,
        rhi::Texture depth,
        rhi::GraphicsPipeline pipeline,
        std::array<rhi::GraphicsPipeline, 4U> blended_pipelines,
        std::array<rhi::GraphicsPipeline, 4U> media_blended_pipelines,
        rhi::GraphicsPipeline media_depth_pipeline,
        rhi::GraphicsPipeline blended_depth_pipeline,
        rhi::Sampler media_sampler,
        rhi::Texture material_fallback,
        rhi::Texture normal_fallback,
        std::shared_ptr<GeometryResidency> geometry,
        rhi::StorageBuffer instances,
        rhi::StorageBuffer lights,
        rhi::StorageBuffer inline_effect_ops,
        rhi::StorageBuffer inline_effect_parameters,
        SceneRendererMode mode,
        rhi::Texture velocity,
        rhi::Texture object_id,
        rhi::Texture normal,
        rhi::Texture coverage,
        rhi::Texture auxiliary_depth,
        rhi::GraphicsPipeline velocity_pipeline,
        rhi::GraphicsPipeline object_id_pipeline,
        rhi::GraphicsPipeline normal_pipeline,
        rhi::GraphicsPipeline media_velocity_pipeline,
        rhi::GraphicsPipeline media_object_id_pipeline,
        rhi::GraphicsPipeline media_normal_pipeline,
        rhi::GraphicsPipeline coverage_pipeline,
        rhi::Sampler coverage_sampler) noexcept;

    // Generates and uploads a procedural mesh if it is not already resident.
    // Null when the arena has no room or the upload cannot be recorded, which
    // the caller turns into dropped instances rather than a failed frame.
    [[nodiscard]] const MeshRange* ensure_resident(
        scene::MeshKey key, rhi::CommandList& commands);

    // Nonblocking observation of asynchronously decoded CPU residency. GPU
    // integration is bounded record-time work; no file bytes, parsing,
    // tangent generation, or image decode occurs here.
    [[nodiscard]] const ResidentModel* ensure_model_resident(
        const std::string& source,
        rhi::CommandList& commands,
        core::u64 publication_fence);
    [[nodiscard]] const ResidentModel* integrate_model(
        const std::string& source,
        rhi::CommandList& commands,
        core::u64 publication_fence);

    // Reconciles every transaction, including sources absent from this scene.
    // Submitted candidates promote. Canceled candidates keep their existing
    // GPU allocations, mesh ranges, and immutable CPU bytes so the next
    // authored observation can re-record only their texture uploads.
    [[nodiscard]] bool resolve_recording_transactions(
        rhi::CommandList& commands);

    // Streams an entry's geometry up to this renderer's per-recording budget,
    // or, for `complete_now`, all of it. True when the mesh may be drawn in
    // this recording.
    [[nodiscard]] bool stream_geometry(
        GeometryResidency::Entry& entry, rhi::CommandList& commands, bool complete_now);
    // Uploads the rows of a Basic candidate's textures this recording can
    // carry: up to `texture_upload_bytes`, and only what fits the command
    // list's remaining upload pages. Drawable once every row has landed or
    // was written earlier in this recording; Failed only when an upload the
    // list had room for was refused.
    [[nodiscard]] TextureUpload stream_model_textures(
        PendingModelIntegration& integration, rhi::CommandList& commands);
    void note_use(const ResidentModel& model, rhi::CommandList& commands);
    // True while a model's candidate is still streaming its geometry or
    // textures: the model is loading, not refused.
    [[nodiscard]] bool streaming_geometry(const std::string& source) const noexcept;
    // Releases retired models no outstanding recording uses: their arena
    // slots, textures and retirement slots.
    void reclaim_retired_models() noexcept;
    // Brings every started effect's simulation to this show frame (within the
    // step budget) and lists the ones to draw; counts the rest into stats_
    // and readiness.
    void prepare_particles(rhi::CommandList& commands, const scene::Scene& scene,
                           const SceneRenderOptions& options);
    [[nodiscard]] bool record_particles(rhi::CommandList& commands, const scene::ViewMatrices& view,
                                        rhi::RasterViewport raster_viewport);

    // Borrowed. The device this renderer was created from, needed on the frame
    // path to create clip textures. It outlives the renderer by construction -
    // the presenter owns both and destroys the renderer first.
    rhi::Device* device_{};
    RendererLimits limits_{};
    rhi::Texture color_;
    rhi::Texture transmission_background_;
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
    // Blended layers write no depth, so two that cross would be ordered whole
    // by their centres. Before compositing, each one except the last draws
    // its fully opaque pixels into depth through these (lit, then media's
    // depth pipeline above), so a later layer is hidden where it passes
    // behind an earlier one. The pass is biased like a depth-only occluder,
    // leaving each layer's own plane to the composite order.
    rhi::GraphicsPipeline blended_depth_pipeline_;
    // ViewShading::Wireframe's colour passes, built only for an editor view
    // (RendererLimits::view_shading) on a device with wireframe_fill.
    rhi::GraphicsPipeline wireframe_pipeline_;
    rhi::GraphicsPipeline media_wireframe_pipeline_;
    rhi::Sampler media_sampler_;
    std::array<rhi::Sampler, material_sampler_count> material_samplers_;
    // One white RGBA texel is neutral for base colour, packed
    // metallic-roughness, occlusion, and emissive. A separate +Z normal texel
    // makes the fifth descriptor valid even when no normal map is authored.
    rhi::Texture material_fallback_;
    rhi::Texture normal_fallback_;
    rhi::Texture offline_media_;
    bool material_fallback_uploaded_{};
    MediaCache media_cache_;
    // Declared before every map holding entries of it, so those are released
    // into it before it goes.
    std::shared_ptr<GeometryResidency> geometry_;
    rhi::StorageBuffer instance_buffer_;
    rhi::StorageBuffer pose_buffer_;
    struct AnimatedInstance final {
        doc::NodeId source{};
        core::u32 offset{};
        core::u32 count{};
        scene::Vec3 wind{};
        scene::Vec3 previous_wind{};
        float minimum_y{};
        float inverse_height{};
        // Skin records are a separate arena range: a vertex's record is
        // skin_base + (vertex - first_vertex).
        core::u32 skin_base{};
        core::u32 first_vertex{};
    };
    std::vector<AnimatedInstance> animated_instances_;
    std::unique_ptr<ModelPoseScratch> pose_scratch_;
    std::vector<scene::Mat4> current_pose_;
    std::vector<scene::Mat4> previous_pose_;
    std::vector<core::u8> pose_upload_;
    rhi::StorageBuffer light_buffer_;
    LtcAsset ltc_asset_;
    IesCache ies_cache_;
    std::shared_ptr<const IesProfile> conformance_ies_;
    rhi::Texture ies_atlas_;
    std::vector<float> ies_atlas_scratch_;
    std::array<std::shared_ptr<const IesProfile>, max_ies_profiles> ies_profiles_{};
    bool ies_uploaded_{};
    rhi::CommandRecordingToken ies_upload_recording_;
    // How many rows of it this frame filled. The buffer is sized to the
    // authored maximum and only partly written, so a consumer reading it needs
    // to be told where the lights stop.
    core::u32 uploaded_light_count_{};
    rhi::StorageBuffer inline_effect_op_buffer_;
    rhi::StorageBuffer inline_effect_parameter_buffer_;
    SceneRendererMode mode_{SceneRendererMode::BasicDisplay};
    rhi::Texture velocity_;
    rhi::Texture object_velocity_;
    rhi::Texture object_id_;
    rhi::Texture normal_;
    std::optional<AmbientOcclusion> occlusion_;
    // Particle effects, on an Advanced renderer whose device can run them.
    // The pass is declared before the slots so it outlives their instances.
    struct ParticleSlot final {
        std::shared_ptr<const ParticlePackage> package;
        core::u32 seed{};
        std::optional<ParticleInstance> instance;
        // Why this package and seed could not be instanced; sticky until
        // either changes.
        std::string error;
    };
    std::unique_ptr<ParticlePass> particle_pass_;
    std::string particles_unsupported_;
    std::unique_ptr<ParticlePackageCache> particle_packages_;
    std::unordered_map<doc::NodeId, ParticleSlot> particle_slots_;
    std::vector<std::pair<const ParticleInstance*, scene::Mat4>> particle_draws_;
    std::optional<TemporalResolve> temporal_occlusion_;
    ShadowAtlasLimits shadow_atlas_limits_{};
    ShadowAtlasPlan shadow_atlas_plan_{};
    rhi::Texture shadow_atlas_depth_;
    rhi::StorageBuffer light_shadow_buffer_;
    std::vector<core::u8> light_shadow_scratch_;
    bool occlusion_recorded_{};
    rhi::Texture coverage_;
    rhi::Texture auxiliary_depth_;
    rhi::GraphicsPipeline velocity_pipeline_;
    rhi::GraphicsPipeline normal_pipeline_;
    rhi::GraphicsPipeline media_normal_pipeline_;
    rhi::GraphicsPipeline object_id_pipeline_;
    // Media variants sample texture alpha and authored edge coverage before
    // writing their categorical products or the shared auxiliary depth.
    rhi::GraphicsPipeline media_velocity_pipeline_;
    rhi::GraphicsPipeline media_object_id_pipeline_;
    rhi::GraphicsPipeline coverage_pipeline_;
    rhi::Sampler coverage_sampler_;
    // Identity, motion and normals written together in one pass, on a device
    // with multiple render targets. Empty elsewhere, where the single-target
    // pipelines above draw them one pass each.
    rhi::GraphicsPipeline auxiliary_pipeline_;
    rhi::GraphicsPipeline media_auxiliary_pipeline_;
    // First advanced raster visibility product: cascaded, output-slice-
    // independent directional shadow maps encoded as reversed depth in R32F.
    // They are rendered before the lit pass and sampled there with
    // deterministic bounded PCSS for a finite sun, fixed-kernel PCF otherwise.
    // The last cascade a frame uses is always
    // the full-scene fit, so it is also the reflective-shadow-map depth.
    //
    // Each shadow product - a cascade, the reflective-shadow-map pair, the
    // local-light atlas - is this renderer's own, allocated at construction
    // and drawn only by it, but registered with the shared geometry so a
    // renderer whose draw would read the same samples it instead of drawing
    // its own. The bound handles name what the last recording sampled.
    std::vector<GeometryResidency::ShadowHandle> cascade_homes_;
    std::array<GeometryResidency::ShadowHandle, scene::max_shadow_cascades> cascades_{};
    rhi::Texture shadow_depth_;
    rhi::GraphicsPipeline shadow_pipeline_;
    rhi::Sampler shadow_sampler_;
    // Reflective-shadow-map attributes share the exact full-scene light fit.
    // Depth reconstructs VPL positions, while these products carry geometric
    // normal and first-directional-light diffuse flux for a fixed sample set.
    GeometryResidency::ShadowHandle rsm_home_;
    GeometryResidency::ShadowHandle rsm_;
    GeometryResidency::ShadowHandle atlas_home_;
    GeometryResidency::ShadowHandle atlas_;
    // What Basic, which has no atlas, reports as one.
    rhi::Texture absent_atlas_;
    rhi::GraphicsPipeline rsm_normal_pipeline_;
    rhi::GraphicsPipeline rsm_flux_pipeline_;
    // Image-based lighting. The prefilter owns the cubemap chain, the
    // coefficients and the split-sum table; the fallbacks exist so the
    // descriptor table has one shape whether or not a scene authored an
    // environment, and are never read - the shader selects on the intensity
    // constant rather than on which texture happens to be bound.
    // Uploads one decoded map into a source texture sized for it and records
    // the prefilter chain. Allocating here is the one place this renderer
    // does so outside construction, and it happens on an authored change
    // rather than per frame - the same footing as a window resize.
    [[nodiscard]] bool adopt_environment(
        rhi::CommandList& commands, const DecodedFrame& frame,
        scene::Turns rotation) noexcept;

    // One recording of the environment as a background, into whichever target
    // owns it. It owns the clear and fills every pixel.
    [[nodiscard]] bool record_sky(
        rhi::CommandList& commands, rhi::Texture& target,
        rhi::Color clear, const SceneRenderView& render_view,
        const scene::Scene& scene, float exposure, scene::CanvasRect slice, bool explicit_view) noexcept;

    std::optional<EnvironmentPrefilter> environment_;
    rhi::Texture environment_source_texture_;
    rhi::Texture environment_fallback_;
    rhi::Texture environment_cube_fallback_;
    rhi::Sampler environment_sampler_;
    // The environment drawn as the background. Present only when the prefilter
    // is, and used only when a map is resident: a scene with none keeps the
    // flat sky clear it always had.
    rhi::GraphicsPipeline sky_pipeline_;
    EnvironmentCache environment_cache_;
    std::array<std::optional<EnvironmentPrefilter>, scene::max_reflection_probes> reflection_filters_;
    std::array<rhi::Texture, scene::max_reflection_probes> reflection_sources_;
    std::array<scene::ReflectionProbe, scene::max_reflection_probes> active_reflections_{};
    std::shared_ptr<const ProbeAsset> reflection_asset_;
    rhi::CommandRecordingToken reflection_upload_recording_;
    std::unique_ptr<SceneRenderer> planar_renderer_;
    // These limits allow a mirror and none has been built yet. Cleared by the
    // one attempt `ensure_planar` makes, so a failure is not retried per frame.
    bool planar_buildable_{};
    bool planar_active_{};
    const rhi::Texture* planar_texture_{};
    // Generation of the map currently prefiltered. Zero means none, which is
    // what makes a scene with no environment cost nothing here.
    core::u64 environment_generation_{};
    bool physical_environment_{};
    scene::PhysicalSky physical_environment_settings_{};
    float physical_environment_scale_{};
    // Set only by the compiled diagnostic fixture; never consult the asset cache.
    bool embedded_environment_{};
    std::string environment_source_;
    // The rotation baked into the resident cube. Rotation is applied during
    // the equirect-to-cube pass so every downstream pass sees one already
    // oriented environment, which means turning the dial has to re-record the
    // chain - it is not a per-frame constant like intensity is.
    scene::Turns environment_rotation_{};

    struct PreviousTransform final {
        doc::NodeId source{};
        scene::Mat4 world{};

        bool operator==(const PreviousTransform&) const = default;
    };
    struct ViewHistoryState final {
        scene::Mat4 previous_view_projection{};
        scene::Mat4 motion_source_view_projection{};
        doc::NodeId previous_scene{};
        core::u64 previous_show_frame{};
        scene::MediaFrameRate previous_show_rate{};
        // Which of the two retained transform generations produced the
        // velocity recorded for previous_show_frame.
        core::u64 motion_transform_show_frame{};
        bool valid{};
    };
    std::vector<ViewHistoryState> view_histories_;
    std::vector<PreviousTransform> previous_transforms_;
    // The transform/view state that produced the most recently committed
    // frame's velocity. Retaining it lets a repeated show frame re-record an
    // identical velocity product without advancing temporal state.
    std::vector<PreviousTransform> motion_source_transforms_;
    std::vector<PreviousTransform> next_transforms_;
    doc::NodeId previous_scene_{};
    core::u64 previous_show_frame_{};
    core::u64 motion_source_show_frame_{};
    bool history_valid_{};

    // Procedural meshes this renderer has drawn, resident in the shared
    // geometry. The per-frame arenas below are scratch that is refilled and
    // uploaded every frame.
    std::map<scene::MeshKey, ResidentMesh> resident_;
    // Imported model geometry in the same arenas, keyed by source path.
    std::map<std::string, ResidentModel> resident_models_;
    // Refresh replacement moves old texture handles here and appends new mesh
    // ranges, so no frame-path operation destroys or mutates resources an
    // earlier draw or submitted recording may still reference.
    std::vector<ResidentModel> retired_models_;
    std::vector<PendingModelIntegration> pending_model_integrations_;
    core::u64 model_texture_bytes_{};
    RecordingBudget geometry_budget_;
    RecordingBudget texture_budget_;
    std::vector<core::u8> instance_scratch_;
    std::vector<core::u8> light_scratch_;
    std::vector<core::u8> inline_effect_op_scratch_;
    std::vector<core::u8> inline_effect_parameter_scratch_;
    std::vector<InlineProgramCacheEntry> inline_program_cache_;
    std::vector<OpaqueSubmission> opaque_submissions_;
    std::vector<core::u8> reordered_instances_;
    std::vector<GeometrySelection> geometry_selection_;
    std::vector<core::u8> opaque_visibility_;
    CasterList casters_;
    // A digest of the part of each opaque instance record a caster draw
    // reads, in slot order, for this frame.
    std::vector<core::u64> instance_digests_;
    std::vector<core::u8> camera_batch_uniforms_;
    // camera_batch_uniforms_ marked pulled, for motion products of a frame
    // with deforming instances.
    std::vector<core::u8> camera_pulled_batch_uniforms_;
    std::vector<rhi::DrawBatch> camera_batches_;
    std::vector<rhi::SampledTexture> camera_batch_textures_;
    std::vector<core::u8> batch_uniforms_;
    std::vector<rhi::DrawBatch> batches_;
    std::vector<rhi::SampledTexture> batch_textures_;
    std::vector<rhi::SampledTexture> rsm_flux_textures_;
    std::vector<rhi::SampledTexture> camera_coverage_textures_;
    std::vector<CompositeDraw> composite_draws_;
    // Clips drawn for depth only, in document order. Kept apart from the
    // composited clips because each needs its own depth-writing pass before
    // any destination-reading draw runs.
    std::vector<CompositeDraw> depth_only_media_;
    // order_composites_by_planes scratch, sized once for max_instances.
    std::vector<LayerVolume> layer_volumes_;
    std::vector<std::pair<core::u32, core::u32>> layer_edges_;
    std::vector<core::u32> layer_edge_offsets_;
    std::vector<core::u32> layer_in_degree_;
    std::vector<core::u32> layer_ready_;
    std::vector<core::u32> layer_order_;
    std::vector<core::u8> layer_placed_;
    ModelCache model_cache_;
    core::u64 authored_asset_generation_{};
    std::optional<TextureStreamer> texture_streamer_;
    std::vector<TextureRequest> texture_requests_;
    ProbeCache probe_cache_;
    rhi::StorageBuffer probe_buffer_;
    std::shared_ptr<const ProbeAsset> probe_asset_;
    rhi::CommandRecordingToken probe_upload_recording_;
    bool probes_uploaded_{};
    rhi::CommandRecordingToken material_fallback_upload_recording_;

    RenderStats stats_{};
};

} // namespace mediaserver::render
