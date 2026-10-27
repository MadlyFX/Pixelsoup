#include <mediaserver/scene/lighting.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>

#include <mediaserver/core/idle_report.hpp>

#include <limits>
#include <new>

namespace mediaserver::render {
namespace {

// Why a pass did not run, in the operator's terms. Literals, because the
// status carries them by pointer and the idle report dedups by address.
constexpr const char* bloom_refusal_unaligned =
    "slice does not start on the canvas's 64-pixel bloom grid";
constexpr const char* bloom_refusal_targets =
    "no bloom targets at this extent on this device";
// An orthographic view's eye stands far behind what it shows, so the air
// between would be integrated as though the camera really were out there.
constexpr const char* fog_idle_orthographic =
    "atmosphere is not drawn in an orthographic view";
constexpr const char* fog_refusal_targets =
    "no atmosphere targets at this extent on this device";

[[nodiscard]] constexpr core::u64 bit(RendererCapability value) noexcept {
    return static_cast<core::u64>(value);
}

[[nodiscard]] core::u64 missing_capabilities(
    const scene::RendererSettings& settings,
    const rhi::DeviceCapabilities& capabilities) noexcept {
    core::u64 missing{};
    if (!capabilities.offscreen) {
        missing |= bit(RendererCapability::Offscreen);
    }
    if (!capabilities.rgba16_float) {
        missing |= bit(RendererCapability::Rgba16Float);
    }
    if (!capabilities.sampled_depth) {
        missing |= bit(RendererCapability::SampledDepth);
    }
    if (!capabilities.integer_targets) {
        missing |= bit(RendererCapability::IntegerTargets);
    }
    if (!capabilities.advanced_scene_raster) {
        missing |= bit(
            RendererCapability::AdvancedSceneRasterImplementation);
    }
    if (settings.ray_tracing || settings.ddgi || settings.restir) {
        if (!capabilities.acceleration_structures) {
            missing |= bit(RendererCapability::AccelerationStructures);
        }
        if (!capabilities.ray_queries) {
            missing |= bit(RendererCapability::RayQueries);
        }
    }
    return missing;
}

[[nodiscard]] bool requested_future_features(
    const scene::RendererSettings& settings) noexcept {
    // `taau` is deliberately absent. It used to sit here because there was no
    // resolve to run; now there is, so temporal antialiasing is a capability
    // this renderer satisfies or refuses on its own terms rather than an
    // authored request for work that does not exist.
    return settings.ray_tracing || settings.ddgi || settings.restir ||
        settings.denoising;
}

[[nodiscard]] core::u64 requested_view_digest(
    const SceneRenderOptions& options) noexcept {
    if (!options.view.has_value()) {
        return 0U;
    }
    return options.view->history_identity;
}

[[nodiscard]] SceneHistoryAction scene_history_action(
    HistoryDisposition disposition) noexcept {
    switch (disposition) {
    case HistoryDisposition::Initialize:
        return SceneHistoryAction::Initialize;
    case HistoryDisposition::Advance:
        return SceneHistoryAction::Advance;
    case HistoryDisposition::Hold:
        return SceneHistoryAction::Hold;
    case HistoryDisposition::Reset:
        return SceneHistoryAction::Reset;
    }
    return SceneHistoryAction::Reset;
}

// Stable owner identity used to find a slot when some content of HistoryKey
// changed and therefore needs a Reset. Mapped views use their persistent map-
// node identity (including maps which partition one physical surface), then
// explicit non-mapped surfaces, while ordinary offscreen callers fall back to
// their authored slice and extent.
[[nodiscard]] bool same_history_owner(
    const HistoryKey& left,
    const HistoryKey& right) noexcept {
    if (left.surface != right.surface) {
        return false;
    }
    if (left.view_digest != 0U || right.view_digest != 0U) {
        return left.view_digest == right.view_digest;
    }
    if (left.surface_id != 0U || right.surface_id != 0U) {
        return left.surface_id == right.surface_id;
    }
    return left.canvas == right.canvas
        && left.slice == right.slice
        && left.width == right.width
        && left.height == right.height;
}

} // namespace

std::string_view RendererReadiness::headline() const noexcept {
    switch (code) {
    case RendererReadinessCode::Ready:
        return selected == scene::RendererBackend::Advanced
            ? "ADVANCED RENDERER READY"
            : "BASIC RENDERER READY";
    case RendererReadinessCode::BasicFallback:
        return "BASIC RENDERER FALLBACK";
    case RendererReadinessCode::MissingCapabilities:
        return "ADVANCED GPU CAPABILITIES MISSING";
    case RendererReadinessCode::RequestedFeaturesUnavailable:
        return "REQUESTED ADVANCED FEATURES UNAVAILABLE";
    case RendererReadinessCode::AdvancedImplementationUnavailable:
        return "ADVANCED RENDERER UNAVAILABLE";
    case RendererReadinessCode::InvalidPolicy:
        return "RENDERER POLICY INVALID";
    case RendererReadinessCode::OrthographicCamera:
        return "ORTHOGRAPHIC CAMERA NEEDS A STAGE VIEW";
    }
    return "RENDERER UNAVAILABLE";
}

RendererReadiness readiness_for_camera(RendererReadiness readiness,
                                       const scene::Camera& camera) noexcept {
    if (scene::orthographic(camera)) {
        readiness.code = RendererReadinessCode::OrthographicCamera;
        readiness.satisfied = false;
        readiness.fallback = false;
    }
    return readiness;
}

RendererReadiness evaluate_renderer_readiness(
    const scene::RendererSettings& source,
    const rhi::DeviceCapabilities& capabilities,
    bool advanced_implementation_available) noexcept {
    const scene::RendererSettings settings = scene::clamped(source);
    RendererReadiness result{};
    result.requested = settings.backend;
    result.selected = settings.backend;
    if (settings.backend == scene::RendererBackend::Basic) {
        return result;
    }

    result.missing_capabilities =
        missing_capabilities(settings, capabilities);
    // Future feature carriers remain replicated and fail closed, but they are
    // not evidence that this GPU is deficient: their passes and callable RHI
    // surface do not exist yet. Report that authored fact before hardware
    // capability diagnostics so an operator is not sent hunting for a new
    // adapter to solve a software boundary.
    if (requested_future_features(settings)) {
        result.code = RendererReadinessCode::RequestedFeaturesUnavailable;
    } else if (result.missing_capabilities != 0U) {
        result.code = RendererReadinessCode::MissingCapabilities;
    } else if (advanced_implementation_available) {
        result.code = RendererReadinessCode::Ready;
    } else {
        result.code = RendererReadinessCode::AdvancedImplementationUnavailable;
    }
    if (result.code == RendererReadinessCode::Ready) {
        result.satisfied = true;
    } else if (!settings.required) {
        result.code = RendererReadinessCode::BasicFallback;
        result.selected = scene::RendererBackend::Basic;
        result.satisfied = true;
        result.fallback = true;
    } else {
        result.satisfied = false;
    }
    return result;
}

core::Result<HybridSceneRenderer, RenderError> HybridSceneRenderer::create(
    rhi::Device& device,
    core::u32 width,
    core::u32 height,
    const RendererLimits& limits,
    SceneEffectOwner scene_effect_owner) {
    using Result = core::Result<HybridSceneRenderer, RenderError>;
    auto basic = SceneRenderer::create(device, width, height, limits);
    if (!basic) {
        return Result::failure(basic.error());
    }
    const core::u64 records = static_cast<core::u64>(limits.max_instances) +
        scene::max_scene_media + scene::max_scene_models +
        scene::max_scene_directional_lights + scene::max_scene_point_lights;
    if (records > std::numeric_limits<core::u32>::max()) {
        return Result::failure(RenderError::InvalidConfiguration);
    }
    auto bridge = SceneBridge::create(static_cast<core::u32>(records));
    if (!bridge) {
        return Result::failure(
            bridge.error() == SceneBridgeError::AllocationFailed
                ? RenderError::AllocationFailed
                : RenderError::InvalidConfiguration);
    }
    std::vector<HistorySlot> history_slots;
    try {
        history_slots.resize(limits.max_history_slots);
    } catch (const std::bad_alloc&) {
        return Result::failure(RenderError::AllocationFailed);
    }
    // Basic only, here. The Advanced renderer and every pass that feeds off
    // it allocate at construction and hold that memory for the renderer's
    // life - full-extent targets, the cascade maps, the shadow atlas, the
    // temporal history - and a show authored on Basic draws no pixel through
    // any of it. Building them anyway cost a node its card: four of these
    // renderers stand behind two outputs, and on six gigabytes that is the
    // difference between media that fits and media that does not.
    //
    // Nothing above the renderer has to know. `readiness` builds on demand,
    // so a scene that authors Advanced is answered with Advanced whether or
    // not this is the first frame to ask it.
    HybridSceneRenderer renderer{
        device, std::move(basic).value(), std::nullopt,
        std::move(bridge).value(), std::move(history_slots)};
    renderer.scene_effect_owner_ = scene_effect_owner;
    renderer.advanced_buildable_ = device.capabilities().advanced_raster();
    renderer.diagnostics_.scene_record_capacity =
        static_cast<core::u32>(records);
    renderer.diagnostics_.history_slot_capacity =
        limits.max_history_slots;
    return Result::success(std::move(renderer));
}

void HybridSceneRenderer::ensure_advanced() noexcept {
    if (advanced_.has_value() || !advanced_buildable_ || device_ == nullptr) {
        return;
    }
    // One attempt, whatever comes of it. A device that advertises advanced
    // raster and then cannot build the renderer will not do better a frame
    // later, and retrying would leave an allocation attempt on the frame path
    // for as long as the show runs.
    advanced_buildable_ = false;
    const core::u32 width = basic_.width();
    const core::u32 height = basic_.height();
    const RendererLimits limits = basic_.limits();
    auto created = SceneRenderer::create(
        *device_, width, height, limits, SceneRendererMode::AdvancedLinear);
    if (!created) {
        return;
    }
    advanced_.emplace(std::move(created).value());
    diagnostics_.advanced_resident = true;
    // The resolve, only where there is an Advanced renderer to feed it. Its
    // absence is not an error here: a build without the shader, or a device
    // without advanced raster, simply refuses an authored temporal request
    // later rather than failing to construct a renderer that works fine
    // without one.
    if (TemporalResolve::shaders_available()) {
        TemporalResolveLimits temporal_limits{};
        temporal_limits.max_targets = limits.max_temporal_history_targets;
        auto created_temporal =
            TemporalResolve::create(*device_, width, height, temporal_limits);
        if (created_temporal) {
            temporal_.emplace(std::move(created_temporal).value());
        }
    }
    if (SpatialAntialias::shaders_available()) {
        auto created_spatial =
            SpatialAntialias::create(*device_, width, height);
        if (created_spatial) {
            spatial_.emplace(std::move(created_spatial).value());
        }
    }
    if (scene_effect_owner_ != SceneEffectOwner::Renderer) {
        return;
    }
    auto motion = ObjectMotionBlur::create(*device_, width, height);
    if (motion) {
        motion_blur_.emplace(std::move(motion).value());
    }
    // Bloom, on the same terms as the resolve above: present when there is an
    // Advanced renderer and the shaders to drive it, absent without complaint
    // otherwise, and refusing an authored request later rather than now.
    if (Bloom::shaders_available()) {
        auto created_bloom = Bloom::create(*device_, width, height);
        if (created_bloom) {
            bloom_.emplace(std::move(created_bloom).value());
        }
    }
    if (VolumetricFog::shaders_available()) {
        auto created_fog =
            VolumetricFog::create(*device_, width, height, FroxelGridLimits{});
        if (created_fog) {
            fog_.emplace(std::move(created_fog).value());
        }
    }
}

void HybridSceneRenderer::prepare(
    const scene::RendererSettings& settings) noexcept {
    if (scene::clamped(settings).backend == scene::RendererBackend::Advanced) {
        ensure_advanced();
    }
}

void HybridSceneRenderer::prepare(const scene::Scene& value) noexcept {
    prepare(value.renderer_settings);
    // A sky is drawn through the Advanced renderer whichever backend the show
    // selected - the compositor owns the environment behind its layers and
    // reaches for `record_background` without consulting the selection. So a
    // Basic show that authored one still needs the Advanced renderer built,
    // and only a scene with no sky to draw may go without it. Deciding this
    // from the authored fields rather than from residency is deliberate: an
    // image that has not finished loading must not change what gets built.
    if (!value.environment.map_source.empty()
        || value.environment.physical_sky.enabled) {
        ensure_advanced();
    }
    // The planar mirror is a second complete renderer inside the Advanced
    // one, so it waits for a scene that authored a reflection. A planar pass
    // is refused outright on Basic - it needs a scene-linear renderer - so a
    // show that selected Basic is not given one even where a sky above has
    // just built the Advanced renderer for its own reasons.
    if (value.environment.planar.enabled && advanced_.has_value()
        && scene::clamped(value.renderer_settings).backend
               == scene::RendererBackend::Advanced) {
        advanced_->ensure_planar();
    }
    diagnostics_.planar_resident =
        advanced_.has_value() && advanced_->planar_resident();
}

RenderFrameRequest HybridSceneRenderer::convenience_request(
    const scene::Scene& value,
    scene::CanvasRect slice,
    const SceneRenderOptions& options,
    core::u32 output_width,
    core::u32 output_height) const noexcept {
    RenderFrameRequest request{};
    request.scene = &value;
    request.slice = slice;
    // Zero means this renderer's own extent, which is right for a caller that
    // did not pad its targets. An owner that did must say so, because the
    // padded target extent is not what the render represents and the history
    // key below is keyed on what it represents.
    request.output_width = output_width != 0U ? output_width : width();
    request.output_height = output_height != 0U ? output_height : height();
    request.options = options;
    request.history.scene = value.id;
    request.history.document_version = value.document_version;
    request.history.temporal_document_digest = value.temporal_document_digest;
    request.history.surface = HistorySurface::Offscreen;
    request.history.canvas = value.canvas;
    request.history.slice = slice;
    request.history.width = request.output_width;
    request.history.height = request.output_height;
    request.history.camera_source = value.camera_source;
    request.history.camera_digest = camera_history_digest(value.camera);
    request.history.view_digest = requested_view_digest(options);
    request.history.profile_digest =
        renderer_profile_digest(value.renderer_settings);
    if (!value.models.empty()) request.history.motion_rate = value.show_rate;
    if (value.renderer_settings.motion_blur && value.renderer_settings.motion_blur_radius > 0U) {
        request.history.motion_rate = value.show_rate;
        request.history.object_shutter = object_shutter_fraction(value, options.view ? &*options.view : nullptr);
    }
    return request;
}

RendererReadiness HybridSceneRenderer::readiness(
    const scene::RendererSettings& settings) noexcept {
    // Build before answering, so the answer describes what this renderer can
    // actually do rather than what it happens to have built so far. Asking
    // whether Advanced is available is the moment the owner commits to it.
    prepare(settings);
    auto result = evaluate_renderer_readiness(
        settings,
        device_ != nullptr ? device_->capabilities()
                           : rhi::DeviceCapabilities{},
        advanced_.has_value());
    if (result.selected == scene::RendererBackend::Advanced
        && scene_effect_owner_ == SceneEffectOwner::Renderer
        && settings.motion_blur && settings.motion_blur_radius > 0U && !motion_blur_) {
        result.code = RendererReadinessCode::RequestedFeaturesUnavailable;
        result.satisfied = false;
    }
    return result;
}

RenderProductsResult HybridSceneRenderer::render_frame(
    rhi::CommandList& commands,
    const RenderFrameRequest& request,
    MediaCache* shared_media_cache) noexcept {
    const auto record_begin = std::chrono::steady_clock::now();
    const auto upload_begin = commands.upload_bytes();
    struct RecordTiming { RendererDiagnostics& diagnostics; rhi::CommandList& commands;
        std::chrono::steady_clock::time_point begin; core::u64 upload_begin;
        ~RecordTiming() {
            diagnostics.cpu_record_ns = static_cast<core::u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - begin).count());
            diagnostics.upload_bytes = commands.upload_bytes() - upload_begin;
        }
    } record_timing{diagnostics_, commands, record_begin, upload_begin};
    ++diagnostics_.frames_requested;
    // A view-driven request renders its own raster, which need not fit inside
    // the canvas: a plate at twice the wall's density or a mapped window wider
    // than the canvas are both legitimate. Only a canvas slice has to be one.
    if (device_ == nullptr || request.scene == nullptr ||
        (scene_effect_owner_ == SceneEffectOwner::Compositor && !request.defer_scene_effects) ||
        request.output_width == 0U || request.output_height == 0U ||
        (!request.options.view.has_value()
         && !scene::contains(request.scene->canvas, request.slice)) ||
        request.history.scene != request.scene->id ||
        request.history.document_version != request.scene->document_version ||
        request.history.temporal_document_digest != request.scene->temporal_document_digest ||
        request.history.canvas != request.scene->canvas ||
        request.history.slice != request.slice ||
        request.history.width != request.output_width ||
        request.history.height != request.output_height ||
        request.history.camera_source != request.scene->camera_source ||
        request.history.camera_digest !=
            camera_history_digest(request.scene->camera) ||
        request.history.view_digest != requested_view_digest(request.options) ||
        request.history.profile_digest !=
            renderer_profile_digest(request.scene->renderer_settings)) {
        ++diagnostics_.invalid_requests;
        return RenderProductsResult::failure(
            RenderError::InvalidConfiguration);
    }

    const RendererReadiness readiness = readiness_for_camera(
        this->readiness(request.scene->renderer_settings),
        request.scene->camera);
    const bool wants_motion = request.scene->renderer_settings.motion_blur
        && request.scene->renderer_settings.motion_blur_radius > 0U;
    const float shutter = object_shutter_fraction(*request.scene,
        request.options.view ? &*request.options.view : nullptr);
    if (((wants_motion || !request.scene->models.empty()) && request.history.motion_rate != request.scene->show_rate)
        || (wants_motion && request.history.object_shutter != shutter)) {
        ++diagnostics_.invalid_requests;
        return RenderProductsResult::failure(RenderError::InvalidConfiguration);
    }
    diagnostics_.last_readiness = readiness;
    if (!readiness.satisfied) {
        ++diagnostics_.capability_refusals;
        last_products_ = RenderProducts{};
        last_products_.readiness = readiness;
        return RenderProductsResult::failure(
            RenderError::CapabilityUnsupported);
    }

    const auto supported_units = [](const auto& lights) {
        return std::all_of(lights.begin(), lights.end(), [](const auto& light) {
            return scene::supported_light_unit(light);
        });
    };
    if (!supported_units(request.scene->directional_lights)
        || !supported_units(request.scene->point_lights)) {
        ++diagnostics_.invalid_requests;
        return RenderProductsResult::failure(RenderError::InvalidConfiguration);
    }
    const auto bridge = bridge_.prepare(*request.scene);
    if (!bridge) {
        ++diagnostics_.resource_refusals;
        return RenderProductsResult::failure(
            bridge.error() == SceneBridgeError::AllocationFailed
                ? RenderError::AllocationFailed
                : RenderError::InvalidConfiguration);
    }
    // A view-driven render resolves one asymmetric viewpoint itself and never
    // derives a projection from the slice, so expanding the slice would
    // enlarge the target without widening what is drawn into it.
    const auto padded = request.padded.has_value()
        ? core::Result<PaddedSlice, SliceApronError>::success(*request.padded)
        : request.options.view.has_value()
        ? core::Result<PaddedSlice, SliceApronError>::success(
              unpadded(request.slice))
        : padded_slice_for(
              *request.scene, request.slice, request.output_width,
              request.output_height,
              readiness.selected == scene::RendererBackend::Advanced);
    if (!padded) {
        // An apron past the authored budget is refused, never quietly reduced.
        // A reduced filter on one node and a full one on its neighbour is the
        // seam this whole path exists to prevent, and it would be invisible
        // until it was on camera.
        ++diagnostics_.resource_refusals;
        return RenderProductsResult::failure(
            RenderError::InvalidConfiguration);
    }
    // The targets must already be the padded extent. They are sized by whoever
    // owns this renderer, from this same function on the same inputs, so a
    // mismatch means the owner did not ask - refuse rather than render the
    // padded rectangle squashed into an unpadded target, which would look
    // almost right and be wrong everywhere.
    const auto required = padded_target_extent(
        padded.value(), request.slice, request.output_width,
        request.output_height);
    if (width() != required.width || height() != required.height) {
        ++diagnostics_.resource_refusals;
        return RenderProductsResult::failure(
            RenderError::InvalidConfiguration);
    }

    SceneRenderer& selected =
        readiness.selected == scene::RendererBackend::Advanced
        && advanced_.has_value()
        ? *advanced_
        : basic_;

    // Prefer the exact temporal key. If an authored property changed, reuse
    // the same output owner's slot so TemporalHistory reports Reset rather
    // than manufacturing a new surface. Only a genuinely new owner consumes
    // an empty slot or deterministically evicts the least recently used one.
    std::size_t history_slot = history_slots_.size();
    std::size_t owner_slot = history_slots_.size();
    std::size_t empty_slot = history_slots_.size();
    std::size_t lru_slot = 0U;
    core::u64 lru_stamp = std::numeric_limits<core::u64>::max();
    for (std::size_t index = 0U; index < history_slots_.size(); ++index) {
        const HistorySlot& candidate = history_slots_[index];
        if (!candidate.occupied) {
            if (empty_slot == history_slots_.size()) {
                empty_slot = index;
            }
            continue;
        }
        if (candidate.key == request.history) {
            history_slot = index;
            break;
        }
        if (same_history_owner(candidate.key, request.history)
            && (owner_slot == history_slots_.size()
                || candidate.last_used
                    > history_slots_[owner_slot].last_used)) {
            owner_slot = index;
        }
        if (candidate.last_used < lru_stamp) {
            lru_stamp = candidate.last_used;
            lru_slot = index;
        }
    }
    if (history_slot == history_slots_.size()) {
        history_slot = owner_slot != history_slots_.size()
            ? owner_slot
            : (empty_slot != history_slots_.size() ? empty_slot : lru_slot);
    }
    const bool slot_was_occupied = history_slots_[history_slot].occupied;
    const bool evicting = slot_was_occupied
        && history_slots_[history_slot].key != request.history
        && !same_history_owner(
            history_slots_[history_slot].key, request.history);
    HistorySlot pending_history = history_slots_[history_slot];
    if (!pending_history.occupied || evicting) {
        pending_history = HistorySlot{};
        pending_history.occupied = true;
    }
    pending_history.key = request.history;
    const HistoryDecision history_decision = pending_history.history.observe(
        request.history, request.scene->show_frame,
        bridge.value().topology_digest, bridge.value().visual_digest);
    SceneRenderOptions record_options = request.options;
    // This value is owned by the facade. A caller may use SceneRenderOptions
    // directly with SceneRenderer's Automatic mode, but it cannot override the
    // decision published for a Hybrid render.
    record_options.history =
        scene_history_action(history_decision.disposition);
    record_options.history_slot = static_cast<core::u32>(history_slot);

    // Temporal antialiasing, decided once for this recording.
    //
    // Refused rather than silently skipped when the resolve is missing. A node
    // quietly rendering without it while its neighbour accumulates is a
    // visible difference along the edge they share, and the whole point of
    // failing closed is that a cohort disagreeing about a pass is worse than a
    // cohort that will not start.
    const scene::AntialiasingMode antialiasing =
        scene::antialiasing_for(request.scene->renderer_settings);
    const bool temporal =
        antialiasing == scene::AntialiasingMode::Temporal
        && readiness.selected == scene::RendererBackend::Advanced;
    const bool spatial =
        antialiasing == scene::AntialiasingMode::Spatial
        && readiness.selected == scene::RendererBackend::Advanced;
    if ((temporal && !temporal_.has_value())
        || (spatial && !spatial_.has_value())) {
        ++diagnostics_.temporal_refusals;
        last_products_ = RenderProducts{};
        last_products_.readiness = readiness;
        return RenderProductsResult::failure(
            RenderError::CapabilityUnsupported);
    }
    if (temporal) {
        // From the show frame and the authored period alone. A hold repeats
        // the show frame, so it reuses its offset without a branch here.
        record_options.jitter = scene::jitter_for_frame(
            request.scene->show_frame,
            scene::clamped(request.scene->renderer_settings)
                .taa_sequence_length);
    }
    // The padded rectangle, not the authored slice. Everything downstream of
    // this renderer that reads neighbouring pixels needs the apron present;
    // the crop back to the authored slice happens once, after the last such
    // pass, and is published on the products.
    const scene::CanvasRect render_rect = padded.value().render;
    const bool recorded = shared_media_cache == nullptr
        ? selected.render(commands, *request.scene, render_rect,
                          record_options)
        : selected.render(commands, *request.scene, render_rect,
                          record_options, *shared_media_cache);
    if (!recorded) {
        last_products_.passes.geometry = selected.stats();
        ++diagnostics_.backend_failures;
        return RenderProductsResult::failure(RenderError::BackendFailure);
    }
    if (readiness.selected == scene::RendererBackend::Advanced
        && !selected.complete_products()) {
        ++diagnostics_.backend_failures;
        last_products_ = RenderProducts{};
        last_advanced_ = false;
        return RenderProductsResult::failure(RenderError::BackendFailure);
    }

    if (request.conformance) {
        const auto& tap = *request.conformance;
        using P = core::ConformancePass;
        for (core::u32 i = 0U; i < selected.stats().directional_shadow_cascades; ++i)
            if (!tap.record(commands, P::Cascades, selected.cascade_target(i)))
                return RenderProductsResult::failure(RenderError::BackendFailure);
        if (!tap.record(commands, P::ShadowAtlas, selected.shadow_atlas_target())
            || !tap.record(commands, P::Depth, selected.product_depth_target())
            || !tap.record(commands, P::Identity, selected.object_id_target())
            || !tap.record(commands, P::Normals, selected.normal_target())
            || !tap.record(commands, P::Velocity, selected.velocity_target())
            || !tap.record(commands, P::ObjectVelocity, selected.object_velocity_target())
            || !tap.record(commands, P::Coverage, selected.coverage_target())
            || !selected.occlusion_target()
            || !tap.record(commands, P::Occlusion, *selected.occlusion_target())
            || !tap.record(commands, P::Lighting, selected.target()))
            return RenderProductsResult::failure(RenderError::BackendFailure);
    }
    bridge_.commit();

    last_products_ = RenderProducts{};
    last_products_.color = &selected.target();

    // Volumetric fog, before the antialiasing resolve and before bloom.
    //
    // Atmosphere is part of the image rather than a filter on it: it is light
    // scattered toward the lens by the air the set sits in, so it belongs in
    // the frame the resolve then integrates and the chain then spills. Applying
    // it afterwards would antialias the set and then lay haze over the result,
    // which reads as a wash rather than as depth - and would bloom the fog
    // rather than the lights inside it.
    bool fog_ran = false;
    const scene::Environment atmosphere = request.scene->environment;
    // Skipped entirely when the owner applies atmosphere and bloom to its
    // composite instead; see `RenderFrameRequest::defer_scene_effects`.
    const bool defer = request.defer_scene_effects;
    const bool orthographic_view = request.options.view.has_value()
        && scene::orthographic(request.options.view->matrices.projection);
    if (!defer && readiness.selected == scene::RendererBackend::Advanced
        && atmosphere.fog_density > 0.0F && orthographic_view) {
        static_cast<void>(
            core::report_idle_once("atmosphere", fog_idle_orthographic));
    } else if (!defer && readiness.selected == scene::RendererBackend::Advanced
        && atmosphere.fog_density > 0.0F) {
        // The frustum this frame was actually rendered from and the raster it
        // fills, decided by `fog_frame_for` - the one derivation the renderer
        // and the compositor share, so an ICVFX plate or a mapped surface is
        // fogged through its own view rather than the authored camera's.
        const FogFrame frame = fog_frame_for(
            *request.scene,
            request.options.view.has_value() ? &*request.options.view
                                             : nullptr,
            padded.value(), last_products_.color->width(),
            last_products_.color->height());
        if (fog_.has_value()
            && fog_->record(commands, *last_products_.color,
                            selected.product_depth_target(),
                            selected.shadow_atlas_target(),
                            selected.light_storage(),
                            selected.light_shadow_storage(),
                            selected.light_storage_count(), *request.scene,
                            frame.view_projection, frame.grid_canvas,
                            frame.render, frame.clip_planes,
                            selected.fog_environment(), &selected.ies_atlas())) {
            last_products_.color = &fog_->target();
            fog_ran = true;
            diagnostics_.fog_volumes = fog_->stats().volumes;
        } else {
            // Authored and not delivered. Refused rather than skipped, for the
            // reason every refusal on this path exists: a node quietly dropping
            // the atmosphere while its neighbour renders it puts a hard edge
            // down the middle of every deep shot. The reason is published so
            // an operator can tell this refusal from a switch that is off.
            ++diagnostics_.fog_refusals;
            static_cast<void>(
                core::report_idle_once("atmosphere", fog_refusal_targets));
            last_products_ = RenderProducts{};
            last_products_.readiness = readiness;
            last_products_.passes.authored =
                scene::clamped(request.scene->renderer_settings).backend;
            last_products_.passes.selected = readiness.selected;
            last_products_.passes.fog = PassState::Refused;
            last_products_.passes.fog_refusal = fog_refusal_targets;
            return RenderProductsResult::failure(
                RenderError::InvalidConfiguration);
        }
    }

    if (request.conformance && !request.conformance->record(commands, core::ConformancePass::Fog, *last_products_.color))
        return RenderProductsResult::failure(RenderError::BackendFailure);
    if (temporal) {
        // The resolve runs over the whole padded target. Its neighbourhood
        // clamp can reach one pixel past the rendered rectangle, which is
        // undefined - and which the apron exists to absorb, because the crop
        // that follows discards it.
        const TemporalResolve::Sources sources{
            last_products_.color, &selected.velocity_target(),
            &selected.object_id_target(),
            request.options.view ? selected.width() : request.scene->canvas.width,
            request.options.view ? selected.height() : request.scene->canvas.height,
            request.options.view
                ? orthographic_history_depth_tolerance(
                      request.options.view->matrices.projection,
                      selected.height())
                : 0.0F};
        auto resolved = temporal_->apply(
            *device_, commands, record_options.history_slot,
            history_decision.disposition, sources);
        if (!resolved) {
            ++diagnostics_.temporal_refusals;
            last_products_ = RenderProducts{};
            last_products_.readiness = readiness;
            return RenderProductsResult::failure(resolved.error());
        }
        last_products_.color = resolved.value();
        diagnostics_.temporal_resolves = temporal_->stats().resolves;
        diagnostics_.temporal_restarts = temporal_->stats().restarts
            + selected.occlusion_temporal_restarts();
    }
    if (request.conformance && !request.conformance->record(commands, core::ConformancePass::Temporal, *last_products_.color))
        return RenderProductsResult::failure(RenderError::BackendFailure);
    if (spatial) {
        // The other branch of the same authored choice, never both: one
        // softens edges by looking sideways and the other by looking backward,
        // and running them in series would be two blurs rather than one.
        auto filtered = spatial_->apply(commands, *last_products_.color);
        if (!filtered) {
            ++diagnostics_.temporal_refusals;
            last_products_ = RenderProducts{};
            last_products_.readiness = readiness;
            return RenderProductsResult::failure(filtered.error());
        }
        last_products_.color = filtered.value();
        diagnostics_.spatial_passes = spatial_->stats().passes;
    }

    bool motion_ran = false;
    if (!defer && readiness.selected == scene::RendererBackend::Advanced && wants_motion) {
        const float scale_x = request.options.view ? static_cast<float>(selected.width())
            : static_cast<float>(request.scene->canvas.width) * selected.width() / render_rect.width;
        const float scale_y = request.options.view ? static_cast<float>(selected.height())
            : static_cast<float>(request.scene->canvas.height) * selected.height() / render_rect.height;
        const auto blurred = motion_blur_->apply(commands, *last_products_.color,
            selected.object_velocity_target(), shutter,
            scene::clamped(request.scene->renderer_settings).motion_blur_radius, scale_x, scale_y);
        if (!blurred) {
            ++diagnostics_.motion_blur_refusals;
            last_products_.passes.motion_blur = PassState::Refused;
            last_products_.passes.motion_blur_reason = "object motion reconstruction failed";
            return RenderProductsResult::failure(blurred.error());
        }
        last_products_.color = blurred.value();
        motion_ran = true;
        if (shutter > 0.0F) ++diagnostics_.motion_blur_passes;
    }

    if (request.conformance && !request.conformance->record(commands, core::ConformancePass::MotionBlur, *last_products_.color))
        return RenderProductsResult::failure(RenderError::BackendFailure);
    // Bloom. After the antialiasing resolve, because spill belongs on the
    // resolved image rather than on one frame's jittered sample of it; before
    // anything crops, because the chain reads a neighbourhood and every pass
    // that does lives inside the padded region.
    //
    // It writes into the chain's own full-resolution level rather than over the
    // frame, so the colour the temporal resolve published - and stored as its
    // history - is untouched. A history that had bloom folded into it would
    // bloom its own bloom, a little more every frame.
    const scene::RendererSettings bloom_settings =
        scene::clamped(request.scene->renderer_settings);
    bool bloom_ran = false;
    if (!defer && readiness.selected == scene::RendererBackend::Advanced
        && bloom_settings.bloom_strength > 0.0F) {
        const auto plan = bloom_plan_for(
            *request.scene,
            request.options.view.has_value() ? &*request.options.view
                                             : nullptr,
            padded.value(), last_products_.color->width(),
            last_products_.color->height());
        const bool has_chain = plan.has_value() && !plan.value().levels.empty();
        // Every refusal below names its reason, because from the wall a
        // refused bloom and a switched-off one are the same picture and only
        // one of them has something the operator can fix.
        const auto refuse = [&](const char* reason) {
            ++diagnostics_.bloom_refusals;
            static_cast<void>(core::report_idle_once("bloom", reason));
            last_products_ = RenderProducts{};
            last_products_.readiness = readiness;
            last_products_.passes.authored = bloom_settings.backend;
            last_products_.passes.selected = readiness.selected;
            last_products_.passes.bloom = PassState::Refused;
            last_products_.passes.bloom_refusal = reason;
            return RenderProductsResult::failure(
                RenderError::InvalidConfiguration);
        };
        if (has_chain) {
            if (bloom_.has_value()
                && bloom_->record(commands, *last_products_.color,
                                  plan.value())
                && bloom_->combine(commands, *last_products_.color,
                                   bloom_settings.bloom_strength)) {
                last_products_.color = &bloom_->target();
                bloom_ran = true;
                diagnostics_.bloom_chains = bloom_->stats().chains;
            } else {
                // Authored and not delivered. Refused rather than skipped: a
                // node that quietly drops bloom while its neighbour applies it
                // puts a visible edge down the middle of the wall, and that is
                // worse than a frame that does not appear at all.
                return refuse(bloom_refusal_targets);
            }
        } else if (!plan) {
            // The rectangle is off the canvas grid the chain is partitioned
            // on. Nothing downstream can repair that, so it is a refusal - and
            // the one refusal here with an actionable cause, so it says so.
            return refuse(plan.error() == BloomChainError::Unaligned
                              ? bloom_refusal_unaligned
                              : bloom_refusal_targets);
        } else {
            // A canvas too small to halve has no chain to build and no bloom
            // to give. Not a refusal: every node reaches the same conclusion
            // about the same canvas, so there is no seam to protect against -
            // and the pass ran, in the only sense a chain of nothing can.
            bloom_ran = true;
        }
    }
    // Isolated-layer processing needs the exact depth used by the colour pass
    // and accesses depth_target() directly. The public Advanced product is the
    // separately reconstructed nearest visible surface, including media alpha
    // coverage; Basic has no separate auxiliary depth.
    last_products_.depth = &selected.product_depth_target();
    if (readiness.selected == scene::RendererBackend::Advanced) {
        last_products_.velocity = &selected.velocity_target();
        if (wants_motion) last_products_.object_velocity = &selected.object_velocity_target();
        last_products_.object_id = &selected.object_id_target();
        last_products_.normal = &selected.normal_target();
        last_products_.coverage = &selected.coverage_target();
        last_products_.occlusion = selected.occlusion_target();
    }
    last_products_.padded = padded.value();
    last_products_.crop = padded.value().crop;
    // Commit only after the command list accepted every draw. A failed record
    // must be retryable as the same temporal step rather than consuming it.
    if (history_use_stamp_ == std::numeric_limits<core::u64>::max()) {
        // This is unreachable on any practical show, but keeping rollover
        // defined preserves deterministic eviction even under synthetic
        // stress. Halving is bounded, allocation-free, and guarantees room.
        for (HistorySlot& slot : history_slots_) {
            slot.last_used >>= 1U;
        }
        history_use_stamp_ >>= 1U;
    }
    pending_history.last_used = ++history_use_stamp_;
    history_slots_[history_slot] = pending_history;
    if (!slot_was_occupied) {
        ++history_slot_count_;
        diagnostics_.history_slots = history_slot_count_;
    }
    if (evicting) {
        ++diagnostics_.history_slot_evictions;
    }
    last_products_.history = history_decision;
    last_products_.readiness = readiness;

    // What actually ran, as opposed to what was asked for.
    //
    // Reported because the two are not the same thing and nothing on screen
    // distinguishes them: an authored pass the backend cannot run, and one that
    // ran and did nothing visible, produce the same frame. Every pass here has
    // been silently absent at some point in this renderer's life.
    {
        const scene::RendererSettings authored =
            scene::clamped(request.scene->renderer_settings);
        const bool advanced =
            readiness.selected == scene::RendererBackend::Advanced;
        const auto state = [advanced](bool wanted, bool ran) {
            if (!wanted) {
                return PassState::NotAuthored;
            }
            if (!advanced) {
                return PassState::BackendUnsupported;
            }
            return ran ? PassState::Active : PassState::Refused;
        };
        RendererPassStatus passes{};
        passes.geometry = selected.stats();
        passes.authored = authored.backend;
        passes.selected = readiness.selected;
        passes.temporal_antialiasing =
            state(scene::antialiasing_for(authored)
                      == scene::AntialiasingMode::Temporal
                      || (authored.taau
                          && authored.backend
                              == scene::RendererBackend::Advanced),
                  temporal);
        passes.spatial_antialiasing =
            state(authored.spatial_antialiasing, spatial);
        passes.motion_blur = state(!defer && wants_motion, motion_ran);
        passes.shadows = state(authored.shadow_cascades > 0U, advanced);
        passes.bounced_light = state(authored.gi_strength > 0.0F, advanced);
        passes.occlusion = state(authored.occlusion_radius > 0.0F,
                                 last_products_.occlusion != nullptr);
        // Deferred passes are the owner's to report: this renderer neither
        // ran nor refused them, and saying either would be a claim the owner
        // then has to overwrite.
        passes.bloom = defer ? PassState::NotAuthored
                             : state(authored.bloom_strength > 0.0F, bloom_ran);
        passes.fog = defer ? PassState::NotAuthored
                           : state(atmosphere.fog_density > 0.0F, fog_ran);
        if (!defer && advanced && atmosphere.fog_density > 0.0F
            && orthographic_view) {
            passes.fog = PassState::Idle;
            passes.fog_refusal = fog_idle_orthographic;
        }
        last_products_.passes = passes;
    }

    last_products_.scene_linear = selected.scene_linear();
    last_advanced_ = selected.scene_linear();
    ++diagnostics_.frames_rendered;
    if (readiness.selected == scene::RendererBackend::Advanced) {
        ++diagnostics_.advanced_frames;
    } else {
        ++diagnostics_.basic_frames;
    }
    if (readiness.fallback) {
        ++diagnostics_.fallback_frames;
    }
    switch (last_products_.history.disposition) {
    case HistoryDisposition::Initialize:
        ++diagnostics_.history_initializes;
        break;
    case HistoryDisposition::Advance:
        ++diagnostics_.history_advances;
        break;
    case HistoryDisposition::Hold:
        ++diagnostics_.history_holds;
        break;
    case HistoryDisposition::Reset:
        ++diagnostics_.history_resets;
        break;
    }
    diagnostics_.profile_digest = request.history.profile_digest;
    diagnostics_.transient_textures = (bloom_ ? bloom_->transient_stats().textures : 0U)
        + (fog_ ? fog_->transient_stats().textures : 0U);
    diagnostics_.transient_refusals = (bloom_ ? bloom_->transient_stats().refusals : 0U)
        + (fog_ ? fog_->transient_stats().refusals : 0U);
    diagnostics_.graph = selected.stats().graph;
    diagnostics_.last_bridge = bridge.value();
    diagnostics_.scene_records = bridge.value().records;
    diagnostics_.last_padded = padded.value();
    return RenderProductsResult::success(last_products_);
}

bool HybridSceneRenderer::render(
    rhi::CommandList& commands,
    const scene::Scene& value,
    scene::CanvasRect slice) {
    return render(commands, value, slice, SceneRenderOptions{});
}

bool HybridSceneRenderer::render(
    rhi::CommandList& commands,
    const scene::Scene& value,
    scene::CanvasRect slice,
    const SceneRenderOptions& options) {
    return render_frame(
               commands, convenience_request(value, slice, options), nullptr)
        .has_value();
}

bool HybridSceneRenderer::render(
    rhi::CommandList& commands,
    const scene::Scene& value,
    scene::CanvasRect slice,
    const SceneRenderOptions& options,
    MediaCache& media_cache) {
    return render_frame(
               commands, convenience_request(value, slice, options),
               &media_cache)
        .has_value();
}

bool HybridSceneRenderer::resize(
    rhi::Device& device,
    core::u32 width,
    core::u32 height) noexcept {
    if (device_ == nullptr || device.native_handle() != device_->native_handle()) {
        return false;
    }
    if (!basic_.resize(device, width, height)) {
        return false;
    }
    if (advanced_.has_value()
        && !advanced_->resize(device, width, height)) {
        return false;
    }
    if (bloom_.has_value() && !bloom_->resize(device, width, height)) {
        return false;
    }
    if (fog_.has_value() && !fog_->resize(device, width, height)) {
        return false;
    }
    // The antialiasing passes too. They were left out when bloom's resize was
    // added, and the omission was unreachable only because this renderer
    // refuses a request whose padded extent differs from its own - a trap for
    // whoever next made a pass resizable, now closed.
    if (temporal_.has_value() && !temporal_->resize(device, width, height)) {
        return false;
    }
    if (spatial_.has_value() && !spatial_->resize(device, width, height)) {
        return false;
    }
    if (motion_blur_ && !motion_blur_->resize(device, width, height)) return false;
    for (HistorySlot& slot : history_slots_) {
        slot = HistorySlot{};
    }
    history_use_stamp_ = 0U;
    history_slot_count_ = 0U;
    diagnostics_.history_slots = 0U;
    last_products_ = RenderProducts{};
    last_advanced_ = false;
    return true;
}

} // namespace mediaserver::render
