// SceneRenderer::render: one view of a scene recorded into a command list.
// The driver, the Frame it runs, and the phases that prepare the recording:
// the view, the planar mirror, texture streaming, lighting assets, the
// environment and motion history.

#include "scene_renderer_frame.hpp"

#include <mediaserver/core/idle_report.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <utility>

namespace mediaserver::render {

using namespace scene_detail;

bool SceneRenderer::render(
    rhi::CommandList& commands,
    const scene::Scene& scene,
    scene::CanvasRect slice) {
    return render(commands, scene, slice, SceneRenderOptions{});
}

bool SceneRenderer::render(
    rhi::CommandList& commands,
    const scene::Scene& scene,
    scene::CanvasRect slice,
    const SceneRenderOptions& options) {
    media_cache_.begin_frame();
    struct EndMediaFrame final {
        MediaCache* cache{};
        ~EndMediaFrame() { cache->end_frame(); }
    } end_media_frame{&media_cache_};
    return render(commands, scene, slice, options, media_cache_);
}

// One recording of the environment as a background, into whichever target owns
// it. `render` draws it into its own colour target; the compositor hands in the
// accumulator its layers will be composited over.
//
// It owns the clear: the pass clears colour and depth as any first pass does
// and fills every pixel, so the colour handed in is overwritten rather than
// blended against.
int SceneRenderer::Frame::ies_slot_for(const scene::PointLight& light) const noexcept {
    if (light.ies_source.empty()) return -1;
    if (scene::area_light(light.kind)) return -2;
    for (core::u32 i = 0; i < ies_count; ++i)
        if (ies_sources[i] == light.ies_source && ies_available[i]) return static_cast<int>(i);
    return -2;
}

std::size_t SceneRenderer::Frame::held_instances() const noexcept {
    return static_cast<std::size_t>(renderer.stats_.instances)
        + renderer.composite_draws_.size() + renderer.depth_only_media_.size();
}

GpuInstance SceneRenderer::Frame::instance_record(
    const scene::MeshInstance& instance,
    core::u32 first_effect,
    core::u32 effect_count,
    bool depth_only,
    DecodedPixelLayout media_layout,
    bool has_normal_map,
    bool has_occlusion_map,
    bool normal_bc5, bool coat_bc5, doc::NodeId motion_source) const {
    const auto previous = motion_transforms == nullptr
        ? static_cast<const PreviousTransform*>(nullptr)
        : [&]() noexcept -> const PreviousTransform* {
              const auto key = motion_source.value() ? motion_source : instance.source;
              const auto found = std::lower_bound(
                  motion_transforms->begin(),
                  motion_transforms->end(), key,
                  [](const PreviousTransform& held,doc::NodeId wanted) noexcept {
                      return held.source < wanted;
                  });
              return found == motion_transforms->end() || found->source != key
                  ? nullptr
                  : &*found;
          }();
    const scene::Mat4* prior =
        previous == nullptr ? nullptr : &previous->world;
    auto record = to_gpu(
        instance, first_effect, effect_count, depth_only,
        media_layout, prior);
    for (const auto& animated:renderer.animated_instances_) if (animated.source==instance.source) {
        record.pose[0]=animated.offset; record.pose[1]=animated.count;
        record.pose[2]=animated.skin_base; record.pose[3]=animated.first_vertex;
        record.wind[0]=animated.wind.x; record.wind[1]=animated.wind.y; record.wind[2]=animated.wind.z;
        record.wind[3]=animated.minimum_y;
        record.previous_wind[0]=animated.previous_wind.x; record.previous_wind[1]=animated.previous_wind.y; record.previous_wind[2]=animated.previous_wind.z;
        record.previous_wind[3]=animated.inverse_height;
        break;
    }
    record.alpha[3] = options.reflection_pass ? -1.0F :
        (renderer.planar_active_ && instance.source == scene.environment.planar.receiver ? 1.0F : 0.0F);

    // A bound fallback is a descriptor-valid absence, not authored
    // material data. Zeroing these two strengths makes that absence
    // mathematically exact even though RGBA8 cannot encode 0.5 in the
    // normal map's X/Y channels exactly.
    if (!has_normal_map) {
        record.material_properties[0U] = 0.0F;
    }
    if (!has_occlusion_map) {
        record.material_properties[1U] = 0.0F;
    }
    record.material_properties[2U] = normal_bc5 ? 1.0F : 0.0F;
    record.material_properties[3U] = coat_bc5 ? 1.0F : 0.0F;
    return record;
}

void SceneRenderer::Frame::append_material_textures(
    const std::array<const rhi::Texture*, 14U>& textures,
    const std::array<ModelSampler, 14U>& samplers) {
    std::array<rhi::SampledTexture, advanced_fragment_textures> bindings{};
    renderer.bind_material_textures(bindings, textures, samplers,
        scene.renderer_settings.texture_anisotropy, renderer.material_fallback_);
    const auto count = renderer.scene_linear() ? advanced_fragment_textures : basic_fragment_textures;
    renderer.batch_textures_.insert(renderer.batch_textures_.end(), bindings.begin(), bindings.begin() + count);
    if (renderer.scene_linear()) renderer.rsm_flux_textures_.push_back(bindings[0]);
}

bool SceneRenderer::Frame::inline_range_for(
    doc::NodeId source,
    const scene::EffectStack& effects,
    InlineProgramRange& range,
    const std::optional<SurfaceFillProgram>& fill) {
    if (source.value() != 0U) {
        const auto found = std::find_if(
            renderer.inline_program_cache_.begin(),
            renderer.inline_program_cache_.end(),
            [source](const InlineProgramCacheEntry& entry) noexcept {
                return entry.source == source;
            });
        if (found != renderer.inline_program_cache_.end()) {
            range = found->range;
            return true;
        }
    }

    CompiledInlineRange compiled{};
    if (!compile_inline_program(
            effects,
            scene.effect_settings,
            renderer.limits_.max_inline_effect_ops,
            renderer.limits_.max_inline_effect_parameter_vectors,
            renderer.limits_.max_inline_curve_luts,
            curve_luts_used,
            renderer.inline_effect_op_scratch_,
            renderer.inline_effect_parameter_scratch_,
            compiled,
            fill)) {
        return false;
    }
    range = InlineProgramRange{compiled.first, compiled.count};
    if (source.value() != 0U) {
        renderer.inline_program_cache_.push_back(
            InlineProgramCacheEntry{source, range});
    }
    return true;
}

bool SceneRenderer::Frame::queue_composite(
    const MeshRange& range,
    const scene::MeshInstance& instance,
    ResolvedMediaFrame media,
    const std::array<const rhi::Texture*, 14U>& material_textures,
    float depth,
    doc::NodeId source,
    std::optional<core::u64> timeline_order,
    std::optional<SiblingOrder> sibling_order,
    std::array<ModelSampler, 14U> samplers,
    ModelBounds bounds) {
    if (held_instances() >= renderer.limits_.max_instances) {
        ++renderer.stats_.dropped_instances;
        return false;
    }
    CompositeDraw draw{};
    draw.range = range;
    draw.instance = instance;
    draw.material_textures = material_textures;
    draw.samplers = samplers;
    draw.media = media;
    draw.blend = valid_blend(instance.style.blend);
    draw.depth = depth;
    draw.source_order = timeline_order.value_or(source.value());
    if (sibling_order.has_value()) {
        draw.sibling_ordered = true;
        draw.sibling_parent = sibling_order->parent;
        draw.sibling_order = sibling_order->order;
    }
    draw.bounds = bounds;
    draw.ordinal = next_ordinal;
    ++next_ordinal;
    renderer.composite_draws_.push_back(draw);
    return true;
}

bool SceneRenderer::Frame::camera_visible(const scene::Mat4& world,
                                          const ModelBounds& bounds) const {
    // A free Stage or physical-wall projection can see outside the authored
    // canvas. Scene has no replicated wall planes, so explicit views retain
    // all geometry; LOD still uses the authored camera on every surface.
    return !scene.renderer_settings.frustum_culling || options.view.has_value()
        || frusta->visible(world, bounds);
}

void SceneRenderer::Frame::selected(doc::NodeId source, bool model,
                                    bool visible, core::u32 lod) {
    renderer.geometry_selection_.push_back({source, model, visible, lod});
    if (visible) ++renderer.stats_.visible_geometry; else ++renderer.stats_.culled_geometry;
}

bool SceneRenderer::render(
    rhi::CommandList& commands,
    const scene::Scene& scene,
    scene::CanvasRect slice,
    const SceneRenderOptions& options,
    MediaCache& media_cache) {
    // The geometry is shared with every renderer on this device.
    const auto geometry_held = geometry_->lock();
    stats_ = RenderStats{};
    graph_.begin();
    struct PublishGraph { FrameGraph& graph; RenderStats& stats;
        ~PublishGraph() { stats.graph = graph.stats(); }
    } publish_graph{graph_, stats_};
    Frame f{*this, commands, scene, slice, options, media_cache};
    return begin_recording(f)
        && resolve_view(f)
        && record_planar_reflection(f)
        && prepare_texture_streaming(f)
        && upload_material_fallbacks(f)
        && prepare_light_assets(f)
        && prepare_baked_lighting(f)
        && prepare_environment(f)
        && begin_history(f)
        && gather_instances(f)
        && gather_models(f)
        && build_opaque_batches(f)
        && gather_media(f)
        && order_composites(f)
        && pack_lights(f)
        && prepare_view_constants(f)
        && fit_shadow_cascades(f)
        && prepare_scene_constants(f)
        && prepare_bindings(f)
        && record_shadow_maps(f)
        && record_auxiliary_passes(f)
        && record_lit_pass(f)
        && record_composites(f)
        && record_particles_and_coverage(f)
        && commit_history(f);
}

bool SceneRenderer::begin_recording(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    auto& slice = f.slice;
    auto& options = f.options;
    auto& media_cache = f.media_cache;
    for (const auto& light : scene.directional_lights)
        if (!scene::supported_light_unit(light)) return false;
    for (const auto& light : scene.point_lights)
        if (!scene::supported_light_unit(light)) return false;

    authored_asset_generation_ = scene::asset_generation_key(options.residency_scene ? *options.residency_scene : scene);
    if(!media_cache.adopt_generation(authored_asset_generation_,(options.residency_scene ? *options.residency_scene : scene).media)) return false;
    stats_.model_texture_bytes = texture_streamer_ ? texture_streamer_->stats().allocated_bytes : model_texture_bytes_;
    geometry_selection_.clear();
    const std::size_t requested_instances = scene.instances.size() + scene.models.size() + scene.media.size();
    if (requested_instances > limits_.max_instances) {
        stats_.dropped_instances = static_cast<core::u32>(std::min<std::size_t>(
            requested_instances, std::numeric_limits<core::u32>::max()));
        return false;
    }
    if (!resolve_recording_transactions(commands)) {
        return false;
    }
    // The command list, rather than a show-frame/timer key, is the safe local
    // publication boundary. Every renderer and mapped view recorded into this
    // list reuses this exact epoch even if the worker completes in between.
    const core::u64 model_publication_fence = scene.models.empty() &&
        (!options.residency_scene || options.residency_scene->models.empty())
        ? 0U
        : commands.latch_asset_publication_epoch(
              model_cache_.publication_epoch());

    if (!scene::valid(scene.camera) ||
        options.history_slot >= view_histories_.size() ||
        (!options.view.has_value() && !scene::contains(scene.canvas, slice))) {
        return false;
    }
    f.model_publication_fence = model_publication_fence;
    return true;
}

bool SceneRenderer::resolve_view(Frame& f) {
    auto& scene = f.scene;
    auto& slice = f.slice;
    auto& options = f.options;
    SceneRenderView render_view{};
    if (!resolve_render_view(scene, slice, options, render_view)) {
        return false;
    }
    // An editor view's shading aid. Wireframe needs its own pipelines, which
    // only a renderer built for an editor view has: refused, not drawn lit.
    const ViewShading shading = render_view.shading;
    const bool wireframe = shading == ViewShading::Wireframe;
    if (wireframe && (!wireframe_pipeline_ || !media_wireframe_pipeline_)) {
        static_cast<void>(core::report_idle_once(
            "view shading",
            "wireframe needs a renderer built for an editor view"));
        return false;
    }
    if ((shading == ViewShading::UvChecker
         || shading == ViewShading::WorldNormals
         || render_view.model_part.has_value())
        && !limits_.view_shading) {
        static_cast<void>(core::report_idle_once(
            "view shading",
            "UV checker, world normals and part isolation need a renderer "
            "built for an editor view"));
        return false;
    }
    f.render_view = render_view;
    f.shading = shading;
    f.wireframe = wireframe;
    return true;
}

bool SceneRenderer::record_planar_reflection(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    auto& slice = f.slice;
    auto& options = f.options;
    auto& media_cache = f.media_cache;
    auto& render_view = f.render_view;
    auto& shading = f.shading;
    planar_active_ = false;
    planar_texture_ = nullptr;
    // Views that draw no light need no mirror either.
    if (!options.reflection_pass && scene.environment.planar.enabled
        && view_shading_lit(shading)) {
        const auto& p = scene.environment.planar;
        const auto& full = options.residency_scene ? *options.residency_scene : scene;
        const bool receiver = p.receiver != doc::NodeId{} &&
            (std::any_of(full.instances.begin(), full.instances.end(), [&](const auto& v) { return v.source == p.receiver; }) ||
             std::any_of(full.models.begin(), full.models.end(), [&](const auto& v) { return v.source_node == p.receiver; }));
        if (!scene_linear() || (!planar_renderer_ && !options.shared_planar) || !receiver) {
            ++stats_.planar_refusals;
            stats_.reflection_reason = !scene_linear() ? "Planar reflections require Advanced" : !receiver ? "Choose a planar receiver object" : "Only one planar reflection pass is supported";
            static_cast<void>(core::report_idle_once("Planar reflection", stats_.reflection_reason.data()));
        } else if (options.shared_planar) {
            planar_active_ = true;
            planar_texture_ = options.shared_planar;
        } else {
            const auto mirror = scene::reflection_matrix(p);
            auto mirrored = render_view;
            mirrored.matrices.view = scene::multiply(render_view.matrices.view, mirror);
            mirrored.matrices.view_projection = scene::multiply(render_view.matrices.view_projection, mirror);
            mirrored.canvas_view_projection = scene::multiply(render_view.canvas_view_projection, mirror);
            mirrored.eye = scene::transform_point(mirror, render_view.eye);
            const auto direction = scene::transform(mirror, {render_view.forward.x, render_view.forward.y, render_view.forward.z, 0});
            mirrored.forward = {direction.x, direction.y, direction.z};
            SceneRenderOptions reflection_options{};
            reflection_options.reflection_pass = true;
            reflection_options.full_canvas_view = !options.view.has_value();
            reflection_options.residency_scene = &full;
            reflection_options.view = mirrored;
            reflection_options.excluded_source = p.receiver;
            reflection_options.history = SceneHistoryAction::Reset;
            const auto n = p.normal * (scene::dot(p.normal, render_view.eye - p.point) >= 0 ? 1.0F : -1.0F);
            reflection_options.clip_plane = {n.x, n.y, n.z, -scene::dot(n, p.point) - 0.0005F};
            if (!planar_renderer_->render(commands, full, slice, reflection_options, media_cache)) return false;
            planar_active_ = true;
            planar_texture_ = &planar_renderer_->target();
            ++stats_.planar_passes;
        }
    }
    return true;
}

bool SceneRenderer::prepare_texture_streaming(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    auto& options = f.options;
    auto& model_publication_fence = f.model_publication_fence;
    if (texture_streamer_ && (!scene.models.empty() ||
        (options.residency_scene && !options.residency_scene->models.empty()) ||
        texture_streamer_->stats().allocated_bytes != 0U)) {
        const auto& full = options.residency_scene ? *options.residency_scene : scene;
        texture_requests_.clear();
        bool complete = true;
        for (const auto models : {std::span<const scene::ModelInstance>{full.models},
                                  std::span<const scene::ModelInstance>{full.publication_retained_models}})
        for (const auto& model : models) {
            if (model.source.empty()) continue;
            // One observation per textured source in this recording. A later
            // try-lock miss for another instance cannot mix refresh generations.
            const auto prior = std::find_if(texture_requests_.begin(), texture_requests_.end(),
                [&](const auto& request) { return request.source == model.source; });
            const auto observed = prior == texture_requests_.end()
                ? model_cache_.observe(model.source, model_publication_fence, authored_asset_generation_)
                : ModelCacheObservation{};
            auto asset = prior == texture_requests_.end() ? observed.asset : prior->asset;
            auto generation = prior == texture_requests_.end() ? observed.generation : prior->generation;
            if (!asset) {
                const auto held = resident_models_.find(model.source);
                if (held != resident_models_.end()) {
                    asset = held->second.decoded_asset;
                    generation = held->second.generation;
                }
            }
            if (!asset) {
                complete = false;
                if (observed.state == ModelCacheState::Failed) ++stats_.models_failed;
                else ++stats_.models_pending;
                continue;
            }
            const float pixels = projected_model_pixels(full, model.world, asset->bounds);
            for (core::u32 i = 0U; i < asset->textures.size(); ++i) {
                if (!asset->textures[i].valid()) continue;
                auto found = std::find_if(texture_requests_.begin(), texture_requests_.end(),
                    [&](const auto& request) { return request.source == model.source && request.texture == i; });
                if (found != texture_requests_.end()) {
                    found->projected_pixels = std::max(found->projected_pixels, pixels);
                    continue;
                }
                if (texture_requests_.size() == texture_requests_.capacity()) {
                    stats_.texture_streaming.state = TextureStreamState::CapacityRefused;
                    return false;
                }
                texture_requests_.push_back({model.source, asset, generation, i, pixels, full.show_frame});
            }
        }
        if (!complete) {
            stats_.texture_streaming = texture_streamer_->stats();
            stats_.texture_streaming.state = TextureStreamState::Loading;
            return false; // No partial full-scene plan; the output owner holds.
        }
        const auto settings = scene::clamped(full.renderer_settings);
        const auto budget = std::min(limits_.max_model_texture_bytes,
            static_cast<core::u64>(settings.texture_budget_mib) * 1024ULL * 1024ULL);
        const bool prepared = texture_streamer_->prepare(*device_, commands, texture_requests_,
            budget, settings.texture_streaming, settings.texture_mip_bias);
        stats_.texture_streaming = texture_streamer_->stats();
        stats_.model_texture_bytes = stats_.texture_streaming.allocated_bytes;
        stats_.upload_bytes += stats_.texture_streaming.upload_bytes;
        if (!prepared) return false;
    }
    return true;
}

bool SceneRenderer::upload_material_fallbacks(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    // Decided here, before anything is bound.
    //
    // The per-batch descriptor arrays are assembled during scene traversal,
    // which happens long before the occlusion dispatch, so a flag set by that
    // dispatch would bind last frame's answer. The dispatch runs under exactly
    // this condition and a failure returns, so the binding and the recording
    // cannot disagree.
    occlusion_recorded_ = occlusion_.has_value()
        && mode_ == SceneRendererMode::AdvancedLinear
        && scene.renderer_settings.occlusion_radius > 0.0F;

    if (!material_fallback_uploaded_) {
        constexpr std::array<core::u8, 4U> white{255U, 255U, 255U, 255U};
        constexpr std::array<core::u8, 4U> positive_z{
            128U, 128U, 255U, 255U};
        // Fixed setup texture: opaque grey with a dark X, sampled through the
        // ordinary media pass so transforms, depth and opacity stay identical.
        std::array<core::u8, 64U * 64U * 4U> offline_pixels{};
        for (core::u32 y = 0U; y < 64U; ++y) {
            for (core::u32 x = 0U; x < 64U; ++x) {
                const bool cross = std::abs(static_cast<int>(x) - static_cast<int>(y)) <= 2 ||
                    std::abs(static_cast<int>(x + y) - 63) <= 2;
                const core::u8 shade = cross ? 40U : 128U;
                const auto offset = (y * 64U + x) * 4U;
                offline_pixels[offset] = shade;
                offline_pixels[offset + 1U] = shade;
                offline_pixels[offset + 2U] = shade;
                offline_pixels[offset + 3U] = 255U;
            }
        }
        const auto recording = commands.recording_token();
        if (!recording) {
            return false;
        }
        if (!commands.upload(material_fallback_, white)
            || !commands.upload(normal_fallback_, positive_z)
            || !commands.upload(offline_media_, offline_pixels)) {
            return false;
        }
        material_fallback_uploaded_ = true;
        material_fallback_upload_recording_ = recording;
        stats_.upload_bytes += white.size() + positive_z.size() + offline_pixels.size();
    }
    return true;
}

bool SceneRenderer::prepare_light_assets(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    auto& options = f.options;
    auto& model_publication_fence = f.model_publication_fence;
    // The environment, adopted at a generation boundary.
    //
    // Observed rather than loaded: the cache reads and decodes on its own
    // worker, and this never waits for it. A scene that authored no map takes
    // the early exit and costs nothing; a map still decoding leaves the
    // previous environment - or the hemisphere - in place for this frame
    // rather than flashing.
    //
    // Prefiltering is recorded into this same command list, so the maps a
    // frame lights from are the ones its own recording produced.
    const auto& complete_scene = options.residency_scene ? *options.residency_scene : scene;
    stats_.assets = {scene::asset_set_digest(complete_scene), true, 0};
    stats_.ltc_ready = ltc_asset_.prepare(commands);
    stats_.upload_bytes += ltc_asset_.upload_bytes();
    if (!stats_.ltc_ready) {
        stats_.assets.ready = false;
        static_cast<void>(core::report_idle_once("area lighting", "LTC table upload unavailable"));
        return false;
    }
    for (const auto& light : complete_scene.point_lights) if (scene::area_light(light.kind)) {
        ++stats_.area_lights;
        if (light.casts_shadow) {
            ++stats_.area_shadow_requests;
            stats_.assets.ready = false;
            static_cast<void>(core::report_idle_once("area-light shadows", "area-light shadow maps are not implemented"));
        }
    }
    if (!sky_pipeline_ && std::any_of(complete_scene.directional_lights.begin(), complete_scene.directional_lights.end(),
        [](const auto& light) { return light.angular_radius > 0.0F && light.intensity > 0.0F; })) {
        static_cast<void>(core::report_idle_once("sun disc", "Advanced sky pipeline is unavailable"));
    }
    for (const auto& model : complete_scene.models) {
        const auto observed = model_cache_.observe(model.source, model_publication_fence, authored_asset_generation_);
        stats_.assets.ready = stats_.assets.ready && observed.ready() && !observed.stale_ready;
    }
    // The complete authored scene determines atlas slots, independent of camera,
    // culling, slice, or compositor isolation. Strings are views into this snapshot.
    std::array<std::string_view, max_ies_profiles> ies_sources{};
    std::array<bool, max_ies_profiles> ies_available{};
    core::u32 ies_count = 0;
    bool ies_dirty = !ies_uploaded_;
    if (ies_upload_recording_) {
        const auto outcome = ies_upload_recording_.outcome();
        if (outcome == rhi::CommandRecordingOutcome::Canceled || outcome == rhi::CommandRecordingOutcome::Unavailable)
            ies_dirty = true;
        if (outcome != rhi::CommandRecordingOutcome::Pending) ies_upload_recording_ = {};
    }
    const auto ies_fence = commands.latch_asset_publication_epoch(ies_cache_.publication_epoch(), rhi::AssetPublicationDomain::Ies);
    for (const auto& light : complete_scene.point_lights) {
        if (light.ies_source.empty()) continue;
        if (scene::area_light(light.kind)) {
            ++stats_.ies_refused;
            stats_.assets.ready = false;
            static_cast<void>(core::report_idle_once("IES lighting", "IES profiles require point or spot emission"));
            continue;
        }
        bool duplicate = false;
        for (core::u32 i = 0; i < ies_count; ++i) if (ies_sources[i] == light.ies_source) duplicate = true;
        if (duplicate) continue;
        if (ies_count == max_ies_profiles || scene::area_light(light.kind)) {
            ++stats_.ies_refused;
            stats_.assets.ready = false;
            static_cast<void>(core::report_idle_once("IES lighting", "profile atlas capacity or unsupported area light"));
            continue;
        }
        const auto slot = ies_count++;
        ies_sources[slot] = light.ies_source;
        const auto observed = conformance_ies_
            ? IesObservation{conformance_ies_, IesCacheState::Ready}
            : ies_cache_.observe(light.ies_source, ies_fence, authored_asset_generation_);
        if (observed.profile && observed.state == IesCacheState::Ready) {
            ies_available[slot] = true;
            ++stats_.ies_ready;
            if (ies_profiles_[slot] != observed.profile) {
                ies_profiles_[slot] = observed.profile;
                std::copy(observed.profile->values.begin(), observed.profile->values.end(),
                    ies_atlas_scratch_.begin() + static_cast<std::ptrdiff_t>(slot * ies_profile_texels));
                ies_dirty = true;
            }
        } else {
            stats_.assets.ready = false;
            if (observed.state == IesCacheState::Failed || observed.state == IesCacheState::Capacity) {
                ++stats_.ies_failed;
                stats_.ies_error = observed.error;
                static_cast<void>(core::report_idle_once("IES lighting", ies_error_label(observed.error)));
            } else {
                ++stats_.ies_pending;
                static_cast<void>(core::report_idle_once("IES lighting", "profile loading or publication pending"));
            }
        }
    }
    if (ies_dirty) {
        ies_uploaded_ = false;
        const auto token = commands.recording_token();
        const auto bytes = std::span<const core::u8>{reinterpret_cast<const core::u8*>(ies_atlas_scratch_.data()),
            ies_atlas_scratch_.size() * sizeof(float)};
        if (!token || !commands.upload(ies_atlas_, bytes)) return false;
        ies_uploaded_ = true;
        ies_upload_recording_ = token;
        stats_.upload_bytes += bytes.size();
    }
    f.ies_sources = ies_sources;
    f.ies_available = ies_available;
    f.ies_count = ies_count;
    return true;
}

bool SceneRenderer::prepare_baked_lighting(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    const auto& baked = scene.environment.baked;
    const bool reflections_authored = std::any_of(baked.reflections.begin(), baked.reflections.end(), [](const auto& p) { return p.enabled; });
    stats_.baked_ready = !baked.enabled && !reflections_authored;
    active_reflections_ = {};
    if ((baked.enabled || reflections_authored) && mode_ == SceneRendererMode::AdvancedLinear) {
        const auto observed = probe_cache_.observe(baked.source, baked.content,
            commands.latch_asset_publication_epoch(probe_cache_.publication_epoch(), rhi::AssetPublicationDomain::Probes), authored_asset_generation_);
        stats_.baked_state = observed.state;
        if (observed.asset && observed.state == ProbeCacheState::Ready) {
            if (probe_upload_recording_) {
                const auto outcome = probe_upload_recording_.outcome();
                if (outcome == rhi::CommandRecordingOutcome::Canceled || outcome == rhi::CommandRecordingOutcome::Unavailable)
                    probes_uploaded_ = false;
                if (outcome != rhi::CommandRecordingOutcome::Pending) probe_upload_recording_ = {};
            }
            if (probe_asset_ != observed.asset || !probes_uploaded_) {
                const auto data = std::span<const core::u8>{reinterpret_cast<const core::u8*>(observed.asset->probes.data()), observed.asset->probes.size() * sizeof(BakedProbe)};
                if (!commands.upload(probe_buffer_, data)) return false;
                probe_asset_ = observed.asset;
                probes_uploaded_ = true;
                probe_upload_recording_ = commands.recording_token();
                stats_.upload_bytes += data.size();
            }
            stats_.baked_ready = true;
            stats_.baked_probes = static_cast<core::u32>(probe_asset_->probes.size());
            if (reflection_upload_recording_) {
                const auto outcome = reflection_upload_recording_.outcome();
                if (outcome == rhi::CommandRecordingOutcome::Canceled || outcome == rhi::CommandRecordingOutcome::Unavailable) reflection_asset_.reset();
                if (outcome != rhi::CommandRecordingOutcome::Pending) reflection_upload_recording_ = {};
            }
            if (reflections_authored && reflection_asset_ != observed.asset) {
                reflection_asset_.reset();
                for (std::size_t i = 0; i < observed.asset->reflections.size(); ++i) {
                    const auto& pixels = observed.asset->reflections[i];
                    if (pixels.empty()) continue;
                    const auto data = std::span<const core::u8>{reinterpret_cast<const core::u8*>(pixels.data()), pixels.size() * sizeof(scene::Vec4)};
                    if (!commands.upload(reflection_sources_[i], data) || !reflection_filters_[i]->prepare(commands) ||
                        !reflection_filters_[i]->record(commands, reflection_sources_[i], 0)) return false;
                }
                reflection_asset_ = observed.asset;
                reflection_upload_recording_ = commands.recording_token();
            }
            // Asset slots are canonical node order. Look up authored influence
            // by identity; changing slot order cannot change the reduction.
            for (std::size_t i = 0; i < observed.asset->grid.reflections.size(); ++i) {
                const auto& captured = observed.asset->grid.reflections[i];
                if (!captured.enabled) continue;
                for (const auto& p : baked.reflections) {
                    if (!p.enabled || p.source != captured.source || p.position != captured.position) continue;
                    active_reflections_[i] = p; ++stats_.reflection_probes; break;
                }
            }
            const auto requested = static_cast<core::u32>(std::count_if(baked.reflections.begin(), baked.reflections.end(), [](const auto& p) { return p.enabled; }));
            if (requested != stats_.reflection_probes) {
                stats_.reflection_refusals = requested - std::min(requested, stats_.reflection_probes);
                stats_.baked_ready = false;
                stats_.reflection_reason = "Reflection capture missing or moved; rebake lighting";
                static_cast<void>(core::report_idle_once("Reflection probes", stats_.reflection_reason.data()));
            }

        }
    }
    f.reflections_authored = reflections_authored;
    return true;
}

bool SceneRenderer::prepare_environment(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    auto& reflections_authored = f.reflections_authored;
    if (!prepare_physical_environment(commands, scene)) return false;
    if (!scene.environment.physical_sky.enabled && environment_.has_value() && !embedded_environment_) {

        const EnvironmentCacheObservation observed =
            environment_cache_.observe(
                scene.environment.map_source,
                commands.latch_asset_publication_epoch(
                    environment_cache_.publication_epoch(), rhi::AssetPublicationDomain::Environments), authored_asset_generation_);
        // Rotation joins the test because it is baked into the cube rather
        // than applied per frame: without it the dial moves the authored value
        // and nothing re-records, so the environment never turns.
        stats_.assets.ready = stats_.assets.ready && (scene.environment.map_source.empty() || (observed.ready() && observed.state == EnvironmentCacheState::Ready));
        const bool changed = observed.generation != environment_generation_
            || scene.environment.map_source != environment_source_
            || scene.environment.map_rotation != environment_rotation_;
        if (scene.environment.map_source.empty()) {
            environment_generation_ = 0U;
            environment_source_.clear();
        } else if (changed && observed.ready()) {
            // A fresh equirect upload and the whole prefilter chain, once.
            // Everything downstream of here is per-frame constant.
            if (!environment_->prepare(commands)) {
                return false;
            }
            if (!adopt_environment(commands, *observed.frame,
                                   scene.environment.map_rotation)) {
                return false;
            }
            environment_generation_ = observed.generation;
            environment_rotation_ = scene.environment.map_rotation;
            try {
                environment_source_ = scene.environment.map_source;
            } catch (const std::bad_alloc&) {
                environment_generation_ = 0U;
                return false;
            }
        }
    }

    if (scene_linear() && (reflections_authored || planar_active_) && environment_ && !environment_->prepare(commands)) return false;
    if (reflections_authored && stats_.reflection_probes == 0 && stats_.reflection_reason.empty()) {
        stats_.reflection_reason = !scene_linear() ? "Reflection probes require Advanced" : "Reflection asset loading, missing or failed";
        static_cast<void>(core::report_idle_once("Reflection probes", stats_.reflection_reason.data()));
    }
    return true;
}

bool SceneRenderer::begin_history(Frame& f) {
    auto& scene = f.scene;
    auto& options = f.options;
    auto& render_view = f.render_view;
    instance_scratch_.clear();
    opaque_submissions_.clear();
    reordered_instances_.clear();
    opaque_visibility_.clear();
    camera_batch_uniforms_.clear();
    camera_batches_.clear();
    camera_batch_textures_.clear();
    light_scratch_.clear();
    inline_effect_op_scratch_.clear();
    inline_effect_parameter_scratch_.clear();
    inline_program_cache_.clear();
    batch_uniforms_.clear();
    batches_.clear();
    batch_textures_.clear();
    rsm_flux_textures_.clear();
    camera_coverage_textures_.clear();
    composite_draws_.clear();
    depth_only_media_.clear();
    next_transforms_.clear();
    DirectionalShadowBounds shadow_bounds{};
    // Transform history is independent of an output's camera or slice. Build
    // it from the complete evaluated scene, not only the items visible in this
    // view, so sequential output maps can safely share the two retained scene
    // generations while keeping their view-projection histories separate.
    const auto remember_transform =
        [&](doc::NodeId source, const scene::Mat4& world) noexcept {
            if (source.value() != 0U
                && next_transforms_.size() < limits_.max_instances) {
                next_transforms_.push_back(PreviousTransform{source, world});
            }
        };
    for (const auto& instance : scene.instances) {
        remember_transform(instance.source, instance.world);
    }
    for (const auto& model : scene.models) {
        if (options.excluded_source != doc::NodeId{} && model.source_node == options.excluded_source) continue;
        remember_transform(model.motion_source.value() ? model.motion_source : model.source_node, model.world);
    }
    for (const auto& media : scene.media) {
        remember_transform(media.source_node, media.world);
    }
    std::sort(next_transforms_.begin(),next_transforms_.end(),
        [](const PreviousTransform& a,const PreviousTransform& b) { return a.source < b.source; });

    ViewHistoryState& view_history =
        view_histories_[options.history_slot];
    const bool can_advance_history = view_history.valid
        && view_history.previous_show_rate == scene.show_rate
        && view_history.previous_scene == scene.id
        && view_history.previous_show_frame
            != std::numeric_limits<core::u64>::max()
        && scene.show_frame == view_history.previous_show_frame + 1U;
    const bool can_hold_history = view_history.valid
        && view_history.previous_show_rate == scene.show_rate
        && view_history.previous_scene == scene.id
        && scene.show_frame == view_history.previous_show_frame;
    const auto transforms_for_show_frame =
        [&](core::u64 show_frame) noexcept
            -> const std::vector<PreviousTransform>* {
            if (!history_valid_ || previous_scene_ != scene.id) {
                return nullptr;
            }
            if (previous_show_frame_ == show_frame) {
                return &previous_transforms_;
            }
            if (motion_source_show_frame_ == show_frame) {
                return &motion_source_transforms_;
            }
            return nullptr;
        };
    const std::vector<PreviousTransform>* motion_transforms{};
    scene::Mat4 motion_view_projection =
        render_view.canvas_view_projection;
    bool advance_history{};
    bool hold_history{};
    switch (options.history) {
    case SceneHistoryAction::Automatic:
        if (can_hold_history) {
            motion_transforms = transforms_for_show_frame(
                view_history.motion_transform_show_frame);
            if (motion_transforms != nullptr) {
                motion_view_projection =
                    view_history.motion_source_view_projection;
                hold_history = true;
            }
        } else if (can_advance_history) {
            motion_transforms = transforms_for_show_frame(
                view_history.previous_show_frame);
            if (motion_transforms != nullptr) {
                motion_view_projection =
                    view_history.previous_view_projection;
                advance_history = true;
            }
        }
        break;
    case SceneHistoryAction::Initialize:
    case SceneHistoryAction::Reset:
        break;
    case SceneHistoryAction::Advance:
        // An explicit Advance is the facade's published contract. Refuse a
        // disagreement rather than recording reset velocity under that label.
        if (!can_advance_history) {
            return false;
        }
        motion_transforms = transforms_for_show_frame(
            view_history.previous_show_frame);
        if (motion_transforms == nullptr) {
            return false;
        }
        motion_view_projection = view_history.previous_view_projection;
        advance_history = true;
        break;
    case SceneHistoryAction::Hold:
        if (!can_hold_history) {
            return false;
        }
        motion_transforms = transforms_for_show_frame(
            view_history.motion_transform_show_frame);
        if (motion_transforms == nullptr) {
            return false;
        }
        motion_view_projection =
            view_history.motion_source_view_projection;
        hold_history = true;
        break;
    }
    f.shadow_bounds = shadow_bounds;
    f.view_history = &view_history;
    f.motion_transforms = motion_transforms;
    f.motion_view_projection = motion_view_projection;
    f.advance_history = advance_history;
    f.hold_history = hold_history;
    return true;
}

} // namespace mediaserver::render
