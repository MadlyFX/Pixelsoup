#pragma once

#include <mediaserver/render/object_motion_blur.hpp>
#include <mediaserver/render/render_conformance.hpp>

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/render/scene_bridge.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/render/slice_apron.hpp>
#include <mediaserver/render/bloom.hpp>
#include <mediaserver/render/volumetric_fog.hpp>
#include <mediaserver/render/spatial_antialias.hpp>
#include <mediaserver/render/temporal_history.hpp>
#include <mediaserver/render/temporal_resolve.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>

#include <string>
#include <string_view>
#include <optional>
#include <utility>
#include <vector>

namespace mediaserver::render {

enum class RendererReadinessCode : core::u8 {
    Ready,
    BasicFallback,
    MissingCapabilities,
    RequestedFeaturesUnavailable,
    AdvancedImplementationUnavailable,
    InvalidPolicy,
};

enum class RendererCapability : core::u64 {
    Rgba16Float = 1ULL << 0U,
    IntegerTargets = 1ULL << 1U,
    StorageImages = 1ULL << 2U,
    Compute = 1ULL << 3U,
    Bindless = 1ULL << 4U,
    ExplicitBarriers = 1ULL << 5U,
    IndirectDraw = 1ULL << 6U,
    AccelerationStructures = 1ULL << 7U,
    RayQueries = 1ULL << 8U,
    Offscreen = 1ULL << 9U,
    SampledDepth = 1ULL << 10U,
    AdvancedSceneRasterImplementation = 1ULL << 11U,
};

struct RendererReadiness final {
    RendererReadinessCode code{RendererReadinessCode::Ready};
    scene::RendererBackend requested{scene::RendererBackend::Basic};
    scene::RendererBackend selected{scene::RendererBackend::Basic};
    core::u64 missing_capabilities{};
    bool satisfied{true};
    bool fallback{};

    bool operator==(const RendererReadiness&) const = default;
    [[nodiscard]] std::string_view headline() const noexcept;
};

[[nodiscard]] RendererReadiness evaluate_renderer_readiness(
    const scene::RendererSettings& settings,
    const rhi::DeviceCapabilities& capabilities,
    bool advanced_implementation_available = false) noexcept;

struct RenderFrameRequest final {
    const ConformanceCapture* conformance{};
    const scene::Scene* scene{};
    scene::CanvasRect slice{};
    // Leave atmosphere and bloom to the owner.
    //
    // The compositor applies both to its finished composite: atmosphere is
    // light scattered by the air between the camera and whatever it can see,
    // not a property of one layer, and applying it inside each layer's render
    // fogged a shared background once per layer. Object shutter reconstruction
    // also needs the completed composite. A renderer told this skips these
    // passes and reports them as not authored; the owner that deferred
    // them is the one that knows what became of them, and reports that.
    bool defer_scene_effects{};
    // The extent this render *represents*, which is not necessarily the extent
    // of the targets it draws into.
    //
    // An owner that sized the targets for a slice apron passes the unpadded
    // extent here, because that is what decides whether the apron applies at
    // all: it does only at a one-to-one canvas-to-output pitch. Reading the
    // target extent instead would be circular - the targets are already padded,
    // so the pitch would never look one-to-one and the apron would switch
    // itself off exactly where it had been asked for.
    core::u32 output_width{};
    core::u32 output_height{};
    // The padding to render, when the owner has already decided it.
    //
    // An owner that isolates layers - the compositor does, one renderer per
    // layer - hands each renderer a scene whose effects have been lifted out
    // into the pass that will apply them. A renderer deriving the apron from
    // what it was given would compute zero for a layer that is about to be
    // blurred, and then draw too few pixels for the blur to be correct at the
    // node boundary. Only whoever can see the whole frame can decide this.
    //
    // Absent means this renderer decides, which is right for a caller that
    // hands it the scene as authored.
    std::optional<PaddedSlice> padded{};
    HistoryKey history{};
    SceneRenderOptions options{};
};

// Products remain on the creating device. Basic intentionally leaves advanced
// auxiliaries null; an advanced implementation is not ready until it supplies
// every declared product rather than manufacturing plausible placeholders.
// Whether an authored pass actually ran on the frame just published.
//
// This exists because an authored pass that never reaches the screen looks
// exactly like a pass that does nothing, and the difference is not something an
// operator can see by staring harder. Bloom shipped authored, replicated,
// digested and dispatched, and was thrown away by the compositor for reading a
// texture written before it ran - and so, for far longer, was every frame of
// temporal and spatial antialiasing. Nothing anywhere said so.
enum class PassState : core::u8 {
    // Nothing asked for it. Not a fault, and must not read as one.
    NotAuthored,
    // Authored and it ran on the last frame.
    Active,
    // Authored, and the backend that was *selected* cannot run it. Distinct
    // from a refusal: the pass is fine, the project is on Basic.
    BackendUnsupported,
    // Authored, the backend can run it, and it did not - a refused chain, a
    // missing shader, a slice off the grid. The one state that is a fault.
    Refused,
    // Authored but inapplicable to this view, with an explicit reason.
    Idle,
};

struct RendererPassStatus final {
    RenderStats geometry{};
    // What the project asked for, and what the renderer chose. A project may
    // author Advanced and be handed Basic without anything failing, which is
    // the silent case every state below hangs off.
    scene::RendererBackend authored{scene::RendererBackend::Basic};
    scene::RendererBackend selected{scene::RendererBackend::Basic};

    PassState temporal_antialiasing{};
    PassState spatial_antialiasing{};
    PassState shadows{};
    PassState bounced_light{};
    PassState occlusion{};
    PassState bloom{};
    PassState fog{};
    PassState plate_dof{};
    std::string_view plate_dof_reason{};
    PassState motion_blur{};
    std::string_view motion_blur_reason{};
    // Why a refused pass refused, in words an operator can act on, or empty
    // when it did not refuse. A counter says that bloom stopped; only this
    // says "this slice must start on a 64-pixel boundary", which is the
    // difference between a fault and a fix. String literals, so the status
    // stays a trivially copyable value that compares by content.
    std::string_view bloom_refusal{};
    std::string_view fog_refusal{};

    // True when Advanced was authored and Basic was selected. Every
    // `BackendUnsupported` below has this as its cause, and saying it once is
    // more use than saying it six times.
    [[nodiscard]] bool fell_back() const noexcept {
        return authored == scene::RendererBackend::Advanced
            && selected != scene::RendererBackend::Advanced;
    }

    bool operator==(const RendererPassStatus&) const = default;
};

struct RenderProducts final {
    const rhi::Texture* color{};
    const rhi::Texture* depth{};
    const rhi::Texture* velocity{};
    const rhi::Texture* object_velocity{};
    const rhi::Texture* object_id{};
    // Octahedral world normals. Ambient occlusion needs them and depth
    // reconstruction is wrong exactly where occlusion matters most - at a
    // depth discontinuity the derivative spans two surfaces and belongs to
    // neither.
    const rhi::Texture* normal{};
    const rhi::Texture* coverage{};
    // Screen-space occlusion, null when the scene authored no radius or the
    // device cannot run the pass.
    const rhi::Texture* occlusion{};
    // What was rendered, and where the authored slice sits inside it.
    //
    // `padded.render` is in canvas pixels; `crop` is the same rectangle in
    // *output* pixels, which is what a copy out of this renderer's target
    // needs. Under the one-to-one pitch the apron requires, the two carry the
    // same numbers - but they are different spaces, and a consumer reaching for
    // the canvas rectangle to index a texture would be right by accident.
    //
    // An unpadded frame leaves `crop` at the whole target, so a consumer that
    // always crops is correct whether or not an apron was applied.
    PaddedSlice padded{};
    scene::CanvasRect crop{};
    HistoryDecision history{};
    RendererReadiness readiness{};
    RendererPassStatus passes{};
    bool scene_linear{};
};

// Frame-path diagnostics are counters and the last already-computed answers.
// Reading them performs no query, allocation, timer access, or device wait.
struct RendererDiagnostics final {
    core::FrameTiming timing{}; // Last completed host submission, including this surface.
    core::u64 cpu_record_ns{}, upload_bytes{}; // This surface's latest recording.
    core::u64 profile_digest{};
    core::u64 transient_textures{}, transient_refusals{};
    FrameGraphStats graph{};
    core::u64 frames_requested{};
    core::u64 frames_rendered{};
    core::u64 basic_frames{};
    core::u64 advanced_frames{};
    core::u64 fallback_frames{};
    core::u64 capability_refusals{};
    core::u64 invalid_requests{};
    core::u64 resource_refusals{};
    core::u64 backend_failures{};
    core::u64 history_initializes{};
    core::u64 history_advances{};
    core::u64 history_holds{};
    core::u64 history_resets{};
    // A fixed-capacity slot was reused for a different output/view identity.
    // Selection is deterministic least-recently-used and performs no frame-
    // path allocation.
    core::u64 history_slot_evictions{};
    // Temporal antialiasing. `temporal_restarts` is the one to watch on a
    // wall: it counts frames that had to begin accumulating again because the
    // surface's colour history had been evicted, and a node restarting while
    // its neighbour accumulates is a difference at their shared edge.
    core::u64 temporal_resolves{};
    core::u64 temporal_restarts{};
    core::u64 temporal_refusals{};
    core::u64 spatial_passes{};
    core::u64 motion_blur_passes{};
    core::u64 motion_blur_refusals{};
    core::u64 bloom_chains{};
    core::u64 fog_volumes{};
    // Frames that authored atmosphere and did not get it. Counted for the
    // same reason as the bloom refusals beside it: fog present on one node
    // and absent on its neighbour is a seam down every deep shot.
    core::u64 fog_refusals{};
    // Frames that authored bloom and did not get it, because the chain the
    // canvas asks for needs more apron than the show allows or because this
    // node's rectangle does not sit on the canvas grid. Counted rather than
    // silently shortened: a shorter chain here than on the node next door is
    // two different blurs meeting in the middle of a wall.
    core::u64 bloom_refusals{};
    core::u32 history_slots{};
    core::u32 history_slot_capacity{};
    core::u32 scene_records{};
    core::u32 scene_record_capacity{};
    SceneBridgeUpdate last_bridge{};
    RendererReadiness last_readiness{};
    PaddedSlice last_padded{};

    bool operator==(const RendererDiagnostics&) const = default;
};

using RenderProductsResult = core::Result<RenderProducts, RenderError>;

// Host-owned renderer facade. It is the only product-facing selection point:
// Basic remains the SDL path. Advanced raster is a separate scene-linear
// implementation and is capability-gated; options whose passes have not
// landed yet continue to fail closed rather than being silently omitted.
class HybridSceneRenderer final {
    friend class ConformanceHarness;
public:
    [[nodiscard]] static bool shaders_available() noexcept {
        return SceneRenderer::shaders_available();
    }

    [[nodiscard]] static core::Result<HybridSceneRenderer, RenderError> create(
        rhi::Device& device,
        core::u32 width,
        core::u32 height,
        const RendererLimits& limits = RendererLimits{});

    HybridSceneRenderer(const HybridSceneRenderer&) = delete;
    HybridSceneRenderer& operator=(const HybridSceneRenderer&) = delete;
    HybridSceneRenderer(HybridSceneRenderer&&) = default;
    HybridSceneRenderer& operator=(HybridSceneRenderer&&) = default;
    ~HybridSceneRenderer() = default;

    // The request the convenience overloads below build. Public because an
    // owner that pads this renderer's targets must fill in the unpadded output
    // extent itself, and reconstructing the rest of the request by hand would
    // be a second definition of the history key.
    [[nodiscard]] RenderFrameRequest convenience_request(
        const scene::Scene& scene,
        scene::CanvasRect slice,
        const SceneRenderOptions& options,
        core::u32 output_width = 0U,
        core::u32 output_height = 0U) const noexcept;

    [[nodiscard]] RenderProductsResult render_frame(
        rhi::CommandList& commands,
        const RenderFrameRequest& request,
        MediaCache* shared_media_cache = nullptr) noexcept;

    // Compatibility surface used while compositor call sites migrate to the
    // complete request/products API.
    [[nodiscard]] bool render(
        rhi::CommandList& commands,
        const scene::Scene& scene,
        scene::CanvasRect slice);
    [[nodiscard]] bool render(
        rhi::CommandList& commands,
        const scene::Scene& scene,
        scene::CanvasRect slice,
        const SceneRenderOptions& options);
    [[nodiscard]] bool render(
        rhi::CommandList& commands,
        const scene::Scene& scene,
        scene::CanvasRect slice,
        const SceneRenderOptions& options,
        MediaCache& media_cache);

    [[nodiscard]] bool resize(
        rhi::Device& device,
        core::u32 width,
        core::u32 height) noexcept;

    // Draws the environment behind a compositor's layers. Only the Advanced
    // renderer has one to draw; Basic is the reference path and keeps the flat
    // clear it always had. `last_advanced_` is deliberately not consulted - it
    // describes the previous frame, and the resident-environment test inside
    // is the honest question anyway.
    [[nodiscard]] bool record_background(
        rhi::CommandList& commands, rhi::Texture& target,
        const scene::Scene& scene, scene::CanvasRect slice,
        const SceneRenderOptions& options) noexcept {
        return advanced_.has_value()
            && advanced_->record_background(commands, target, scene, slice,
                                            options);
    }

    [[nodiscard]] const rhi::Texture* planar_target() const noexcept { return advanced_ ? advanced_->planar_target() : nullptr; }
    [[nodiscard]] rhi::Texture& target() noexcept {
        return last_advanced_ && advanced_.has_value()
            ? advanced_->target()
            : basic_.target();
    }
    [[nodiscard]] rhi::Texture& depth_target() noexcept {
        return last_advanced_ && advanced_.has_value()
            ? advanced_->depth_target()
            : basic_.depth_target();
    }
    // Forwarded so a compositor can gather one depth per layer and apply the
    // scene-level effects - atmosphere and bloom - to the composite instead of
    // inside each layer, where an alpha-weighted `over` would discard them.
    [[nodiscard]] rhi::Texture& product_depth_target() noexcept {
        return last_advanced_ && advanced_.has_value()
            ? advanced_->product_depth_target()
            : basic_.product_depth_target();
    }
    [[nodiscard]] rhi::Texture& shadow_atlas_target() noexcept {
        return last_advanced_ && advanced_.has_value()
            ? advanced_->shadow_atlas_target()
            : basic_.shadow_atlas_target();
    }
    [[nodiscard]] const rhi::Texture& ies_atlas() const noexcept { return last_advanced_ && advanced_ ? advanced_->ies_atlas() : basic_.ies_atlas(); }
    [[nodiscard]] const rhi::Texture* fog_environment() const noexcept {
        return advanced_ ? advanced_->fog_environment() : basic_.fog_environment();
    }
    [[nodiscard]] const rhi::StorageBuffer& light_storage() const noexcept {
        return last_advanced_ && advanced_.has_value()
            ? advanced_->light_storage()
            : basic_.light_storage();
    }
    [[nodiscard]] const rhi::StorageBuffer& light_shadow_storage()
        const noexcept {
        return last_advanced_ && advanced_.has_value()
            ? advanced_->light_shadow_storage()
            : basic_.light_shadow_storage();
    }
    [[nodiscard]] core::u32 light_storage_count() const noexcept {
        return last_advanced_ && advanced_.has_value()
            ? advanced_->light_storage_count()
            : basic_.light_storage_count();
    }
    [[nodiscard]] core::u32 width() const noexcept { return basic_.width(); }
    [[nodiscard]] core::u32 height() const noexcept { return basic_.height(); }
    [[nodiscard]] const RenderStats& stats() const noexcept {
        return last_advanced_ && advanced_.has_value()
            ? advanced_->stats()
            : basic_.stats();
    }
    [[nodiscard]] MediaCacheStats media_cache_stats() const noexcept {
        return last_advanced_ && advanced_.has_value()
            ? advanced_->media_cache_stats()
            : basic_.media_cache_stats();
    }
    [[nodiscard]] ModelCacheStats model_cache_stats() const noexcept {
        return basic_.model_cache_stats();
    }
    // All SceneRenderers use the same process cache, so advance the source
    // generation exactly once through Basic rather than once per backend.
    [[nodiscard]] bool refresh_model_source(
        const std::string& source) noexcept {
        return basic_.refresh_model_source(source);
    }
    [[nodiscard]] const RendererLimits& limits() const noexcept {
        return basic_.limits();
    }
    [[nodiscard]] const RenderProducts& last_products() const noexcept {
        return last_products_;
    }
    [[nodiscard]] const RendererDiagnostics& diagnostics() const noexcept {
        return diagnostics_;
    }
    void observe_completed_timing(const core::FrameTiming& timing) noexcept {
        if (timing.profile_digest == diagnostics_.profile_digest) diagnostics_.timing = timing;
    }
    // Pure, already-known selection result for this device and policy. Used
    // by an empty compositor plan, which still has to enforce required
    // Advanced and choose the correct colour domain without inventing a draw.
    [[nodiscard]] RendererReadiness readiness(
        const scene::RendererSettings& settings) const noexcept;

private:
    struct HistorySlot final {
        HistoryKey key{};
        TemporalHistory history{};
        core::u64 last_used{};
        bool occupied{};
    };

    HybridSceneRenderer(
        rhi::Device& device,
        SceneRenderer basic,
        std::optional<SceneRenderer> advanced,
        SceneBridge bridge,
        std::vector<HistorySlot> history_slots) noexcept
        : device_(&device), basic_(std::move(basic)),
          advanced_(std::move(advanced)),
          bridge_(std::move(bridge)),
          history_slots_(std::move(history_slots)) {}

    rhi::Device* device_{};
    SceneRenderer basic_;
    std::optional<SceneRenderer> advanced_;
    // Present only when the Advanced renderer and the resolve shader both are.
    // Its absence is why an authored temporal request can still be refused.
    std::optional<TemporalResolve> temporal_;
    std::optional<SpatialAntialias> spatial_;
    std::optional<ObjectMotionBlur> motion_blur_;
    std::optional<Bloom> bloom_;
    std::optional<VolumetricFog> fog_;
    bool last_advanced_{};
    SceneBridge bridge_;
    std::vector<HistorySlot> history_slots_;
    core::u64 history_use_stamp_{};
    core::u32 history_slot_count_{};
    RenderProducts last_products_{};
    RendererDiagnostics diagnostics_{};
};

} // namespace mediaserver::render
