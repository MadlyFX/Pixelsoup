// SceneRenderer::render, lighting: packed lights, view and scene constants,
// shadow cascades and the shadow maps drawn or reused for them.

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

bool SceneRenderer::pack_lights(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    stats_.batches = static_cast<core::u32>(
        camera_batches_.size() + depth_only_media_.size() + composite_draws_.size());
    const auto ready_models = static_cast<std::size_t>(std::count_if(
        resident_models_.begin(), resident_models_.end(),
        [](const auto& model) noexcept {
            return model.second.generation != 0U;
        })) + static_cast<std::size_t>(std::count_if(
            pending_model_integrations_.begin(),
            pending_model_integrations_.end(),
            [](const PendingModelIntegration& pending) noexcept {
                return pending.active != nullptr
                    && pending.active->generation == 0U
                    && pending.candidate.generation != 0U;
            }));
    stats_.meshes_resident = static_cast<core::u32>(
        resident_.size() + ready_models);
    stats_.geometry_capacity_bytes = geometry_->arena().capacity_bytes();
    stats_.geometry_used_bytes = geometry_->arena().used_bytes();

    const auto push_light = [&](const float vector[3U],
                                GpuLightKind kind,
                                scene::Vec3 color,
                                float intensity,
                                float range,
                                const scene::PointLight* spot = nullptr) {
        const auto held = static_cast<core::u32>(light_scratch_.size() / sizeof(GpuLight));
        if (held >= limits_.max_lights) {
            ++stats_.dropped_lights;
            return;
        }
        GpuLight record{};
        record.vector[0U] = vector[0U];
        record.vector[1U] = vector[1U];
        record.vector[2U] = vector[2U];
        record.kind = kind;
        record.color_intensity[0U] = color.x;
        record.color_intensity[1U] = color.y;
        record.color_intensity[2U] = color.z;
        record.color_intensity[3U] = intensity;
        record.params[0U] = range;
        if (spot != nullptr) {
            const auto slot = f.ies_slot_for(*spot);
            record.ies[3] = slot < 0 ? static_cast<float>(slot) : static_cast<float>(slot) * static_cast<float>(ies_horizontal_samples);
            if (slot >= 0) {
                const auto tangent = ies_tangent(*spot);
                const auto axis = scene::normalize(spot->direction, {0, -1, 0});
                record.ies[0] = tangent.x; record.ies[1] = tangent.y; record.ies[2] = tangent.z;
                record.ies_axis[0] = axis.x; record.ies_axis[1] = axis.y; record.ies_axis[2] = axis.z;
            }
        }
        if (spot != nullptr && scene::area_light(spot->kind)) {
            const auto axis = scene::normalize(spot->direction, {0, -1, 0});
            const auto u = scene::area_light_tangent(*spot);
            const auto v = scene::cross(axis, u);
            record.spot[0] = axis.x; record.spot[1] = axis.y; record.spot[2] = axis.z;
            record.spot[3] = spot->two_sided ? 1.0F : 0.0F;
            record.area_u[0] = u.x; record.area_u[1] = u.y; record.area_u[2] = u.z;
            record.area_u[3] = spot->kind == scene::LocalLightKind::Sphere ? spot->radius : spot->width * 0.5F;
            record.area_v[0] = v.x; record.area_v[1] = v.y; record.area_v[2] = v.z;
            record.area_v[3] = spot->height * 0.5F;
        }
        if (spot != nullptr && spot->kind == scene::LocalLightKind::Spot) {
            // Deterministic trig, because two nodes comparing a dot product
            // against differently-rounded cosines would put the edge of the
            // cone in different places on the same wall.
            record.params[1U] =
                scene::deterministic::cos_turns(spot->outer_cone);
            record.params[2U] =
                scene::deterministic::cos_turns(spot->inner_cone);
            const scene::Vec3 axis = scene::normalize(
                spot->direction, scene::Vec3{0.0F, -1.0F, 0.0F});
            record.spot[0U] = axis.x;
            record.spot[1U] = axis.y;
            record.spot[2U] = axis.z;
            record.spot[3U] = spot->unit == scene::LightUnit::Unitless ? 0.0F : 1.0F;
            // Which row of the shadow table describes this light, or -1. An
            // index rather than a flag, because the table only holds the
            // lights that got a tile and is far shorter than the light list.
            record.params[3U] = -1.0F;
            for (std::size_t tile = 0U;
                 tile < shadow_atlas_plan_.tiles.size(); ++tile) {
                if (shadow_atlas_plan_.tiles[tile].light == spot->source) {
                    record.params[3U] = static_cast<float>(tile);
                    break;
                }
            }
        } else {
            record.params[3U] = -1.0F;
        }
        append(light_scratch_, record);
    };

    // The shadow atlas partition for this frame, decided before any light is
    // packed so each one can carry its own tile index.
    //
    // Derived from the authored lights alone - no camera enters it - so every
    // node reaches the same assignment for the same scene. See shadow_atlas.hpp
    // for why a distance-ranked policy would be a seam.
    shadow_atlas_plan_ =
        plan_shadow_atlas(scene.point_lights, shadow_atlas_limits_);
    stats_.shadow_tiles =
        static_cast<core::u32>(shadow_atlas_plan_.tiles.size());
    stats_.shadow_lights_refused = shadow_atlas_plan_.refused;

    // One row per tile, built here rather than while rasterizing, so the
    // matrix a tile is drawn with and the matrix the lit pass reads back are
    // the same value and cannot drift.
    light_shadow_scratch_.clear();
    for (const ShadowTile& tile : shadow_atlas_plan_.tiles) {
        const scene::PointLight* source = nullptr;
        for (const auto& candidate : scene.point_lights) {
            if (candidate.source == tile.light) {
                source = &candidate;
                break;
            }
        }
        GpuLightShadow record{};
        if (source != nullptr) {
            // Near plane from the light's own range rather than a constant: a
            // practical two metres from a wall and one lighting a whole set
            // need very different depth precision, and the range is the only
            // authored quantity that says which this is.
            const float far_plane = std::max(source->range, 0.01F);
            const float near_plane = std::max(far_plane * 0.005F, 0.01F);
            const scene::ViewMatrices light_view = scene::spot_light_view(
                source->position, source->direction, source->outer_cone,
                near_plane, far_plane);
            for (core::u32 index = 0U; index < 16U; ++index) {
                record.view_projection[index] =
                    light_view.view_projection.m[index];
            }
        }
        record.tile[0U] = static_cast<float>(tile.x);
        record.tile[1U] = static_cast<float>(tile.y);
        record.tile[2U] = static_cast<float>(tile.extent);
        record.tile[3U] = 1.0F / static_cast<float>(tile.extent);
        append(light_shadow_scratch_, record);
    }
    if (light_shadow_scratch_.empty()) {
        // A scene with no casting spot still has to leave the buffer with an
        // address, because the lit pipeline binds it whatever the scene holds.
        // An empty upload leaves it with none and the lit draw is refused -
        // which is a completely black frame for a scene that never asked for a
        // shadow in the first place.
        append(light_shadow_scratch_, GpuLightShadow{});
    }

    for (const auto& light : scene.directional_lights) {
        const float vector[3U]{light.direction.x, light.direction.y, light.direction.z};
        const auto emission = scene::light_emission(light, scene.renderer_settings.photometric_scale);
        // The finite solar disc is already in the physical sky's specular IBL.
        // Reserve the unused directional range lane to avoid adding a second,
        // point-source specular sun on top of it. Diffuse keeps cascade shadows.
        const bool physical_sun = physical_environment_ && &light == &scene.directional_lights.front();
        push_light(vector, GpuLightKind::Directional, emission.color, emission.intensity, physical_sun ? -1.0F : 0.0F);
    }
    for (const auto& light : scene.point_lights) {
        const float vector[3U]{light.position.x, light.position.y, light.position.z};
        const auto slot = f.ies_slot_for(light);
        const auto emission = ies_light_emission(light, scene.renderer_settings.photometric_scale,
            slot >= 0 ? ies_profiles_[static_cast<std::size_t>(slot)].get() : nullptr);
        const auto kind = light.kind == scene::LocalLightKind::Spot ? GpuLightKind::Spot
            : light.kind == scene::LocalLightKind::Rectangle ? GpuLightKind::Rectangle
            : light.kind == scene::LocalLightKind::Sphere ? GpuLightKind::Sphere : GpuLightKind::Point;
        push_light(vector, kind,
                   emission.color, emission.intensity, light.range, &light);
    }


    // The shadow table only exists on the Advanced path, which is the only
    // one that binds it. Basic has no atlas, no table and no buffer, and
    // uploading into a resource it never allocated fails the whole frame.
    if (light_shadow_buffer_
        && !commands.upload(light_shadow_buffer_, light_shadow_scratch_)) {
        return false;
    }
    if (!commands.upload(pose_buffer_,pose_upload_)
        || !commands.upload(instance_buffer_, instance_scratch_)
        || !commands.upload(light_buffer_, light_scratch_)
        || !commands.upload(
            inline_effect_op_buffer_, inline_effect_op_scratch_)
        || !commands.upload(
            inline_effect_parameter_buffer_,
            inline_effect_parameter_scratch_)) {
        return false;
    }
    stats_.inline_effect_ops = static_cast<core::u32>(
        inline_effect_op_scratch_.size() / sizeof(GpuInlineEffectOp));
    stats_.upload_bytes += pose_upload_.size() + instance_scratch_.size() + light_scratch_.size()
        + inline_effect_op_scratch_.size()
        + inline_effect_parameter_scratch_.size();
    return true;
}

bool SceneRenderer::prepare_view_constants(Frame& f) {
    auto& scene = f.scene;
    auto& slice = f.slice;
    auto& options = f.options;
    auto& render_view = f.render_view;
    auto& view_history = *f.view_history;
    auto& motion_view_projection = f.motion_view_projection;
    auto& advance_history = f.advance_history;
    auto& hold_history = f.hold_history;
    // Rasterize the same full-canvas triangles on every node. Only the
    // viewport origin changes for a slice; clipping and interpolation retain
    // their common projection, including with sharp anisotropic highlights.
    rhi::RasterViewport raster_viewport{};
    if (!options.view.has_value() || options.full_canvas_view) {
        const float scale_x = static_cast<float>(color_.width()) / static_cast<float>(slice.width);
        const float scale_y = static_cast<float>(color_.height()) / static_cast<float>(slice.height);
        raster_viewport = {-static_cast<float>(slice.x) * scale_x,
            -static_cast<float>(slice.y) * scale_y,
            static_cast<float>(scene.canvas.width) * scale_x,
            static_cast<float>(scene.canvas.height) * scale_y};
        // The authored canvas may be 65,535 pixels wide, beyond the native
        // viewport bound. Keep rendering its small node targets with their
        // slice projection; lighting still reconstructs in canvas coordinates.
        if (!raster_viewport.valid()) raster_viewport = {};
    }
    ViewConstants view_constants{};
    view_constants.motion_viewport[0]=raster_viewport.empty()?0.0F:raster_viewport.x;
    view_constants.motion_viewport[1]=raster_viewport.empty()?0.0F:raster_viewport.y;
    view_constants.motion_viewport[2]=1.0F/(raster_viewport.empty()?static_cast<float>(color_.width()):raster_viewport.width);
    view_constants.motion_viewport[3]=1.0F/(raster_viewport.empty()?static_cast<float>(color_.height()):raster_viewport.height);
    view_constants.temporal_parameters[0U] = advance_history
        || (hold_history && view_history.motion_transform_show_frame != scene.show_frame)
        ? 1.0F : 0.0F;
    for (core::u32 index = 0U; index < 16U; ++index) {
        view_constants.view_projection[index] = raster_viewport.empty()
            ? render_view.matrices.view_projection.m[index]
            : render_view.canvas_view_projection.m[index];
        view_constants.previous_view_projection[index] =
            motion_view_projection.m[index];
        view_constants.canvas_view_projection[index] =
            render_view.canvas_view_projection.m[index];
    }
    f.raster_viewport = raster_viewport;
    f.view_constants = view_constants;
    return true;
}

bool SceneRenderer::fit_shadow_cascades(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    auto& options = f.options;
    auto& shadow_bounds = f.shadow_bounds;
    // Directional shadow cascades. The authored count is replicated policy;
    // the last cascade is always the full-scene fit, so one cascade is exactly
    // the single whole-scene map this renderer had before cascades existed and
    // the reflective-shadow-map pass keeps its unchanged projection.
    const scene::RendererSettings renderer_settings =
        scene::clamped(scene.renderer_settings);
    std::array<scene::Mat4, scene::max_shadow_cascades>
        cascade_view_projection{};
    for (scene::Mat4& matrix : cascade_view_projection) {
        matrix = scene::identity();
    }
    std::array<float, scene::max_shadow_cascades> cascade_texel_depth{};
    scene::Mat4 shadow_inverse_view_projection = scene::identity();
    core::u32 cascade_count = 0U;
    if (mode_ == SceneRendererMode::AdvancedLinear
        && !scene.directional_lights.empty()
        && renderer_settings.shadow_cascades != 0U
        && !cascade_homes_.empty()) {
        const core::u32 requested = std::min(
            renderer_settings.shadow_cascades,
            static_cast<core::u32>(cascade_homes_.size()));
        // The far cascade is fitted to every caster, exactly as the single map
        // was, so it always covers the scene however the camera is pointed.
        if (directional_shadow_matrix(
                shadow_bounds, scene.directional_lights.front(),
                limits_.directional_shadow_resolution,
                cascade_view_projection[requested - 1U],
                shadow_inverse_view_projection,
                cascade_texel_depth[requested - 1U])) {
            cascade_count = requested;
            // Nearer cascades subdivide the authored full-canvas camera, never
            // this node's slice, so two nodes place the same world point in the
            // same cascade and read the same texels for it.
            const scene::FrustumWindow window = scene::window_for_slice(
                scene.camera, scene.canvas, scene::whole(scene.canvas));
            const float near_distance =
                std::max(window.near_plane, 0.0001F);
            const float far_distance = cascade_far_distance(
                scene.camera, shadow_bounds, near_distance,
                window.far_plane);
            for (core::u32 cascade = 0U; cascade + 1U < requested;
                 ++cascade) {
                CascadeVolume volume{};
                if (!camera_cascade_volume(
                        scene.camera, window,
                        cascade_split(near_distance, far_distance, cascade,
                                      requested),
                        cascade_split(near_distance, far_distance,
                                      cascade + 1U, requested),
                        volume)
                    || !cascade_shadow_matrix(
                        shadow_bounds, volume,
                        scene.directional_lights.front(),
                        limits_.directional_shadow_resolution,
                        cascade_view_projection[cascade],
                        cascade_texel_depth[cascade])) {
                    // A cascade that cannot be fitted is not silently skipped:
                    // dropping it would leave a hole a neighbouring node might
                    // not have. Fall back to the full-scene map alone, which
                    // every node derives identically.
                    cascade_view_projection[0U] =
                        cascade_view_projection[requested - 1U];
                    cascade_texel_depth[0U] =
                        cascade_texel_depth[requested - 1U];
                    cascade_count = 1U;
                    break;
                }
            }
        }
    }
    const bool directional_shadow_enabled = cascade_count != 0U;
    const bool directional_gi_enabled = directional_shadow_enabled
        && renderer_settings.gi_strength > 0.0F
        && !scene.environment.baked.enabled;
    prepare_particles(commands, scene, options);
    stats_.assets.ready = stats_.assets.ready && stats_.planar_refusals == 0 && (options.shared_planar || !planar_active_ || planar_renderer_->stats().assets.ready) && stats_.baked_ready && stats_.models_pending == 0 && stats_.models_failed == 0 && stats_.models_stale == 0 && stats_.model_integration_refusals == 0 && stats_.texture_streaming.state == TextureStreamState::Ready;
    stats_.directional_shadow_rendered = directional_shadow_enabled;
    stats_.directional_shadow_cascades = cascade_count;
    stats_.directional_shadow_fit = cascade_view_projection;
    stats_.directional_shadow_casters = directional_shadow_enabled
        ? shadow_bounds.casters
        : 0U;
    stats_.directional_gi_rendered = directional_gi_enabled;
    f.renderer_settings = renderer_settings;
    f.cascade_view_projection = cascade_view_projection;
    f.cascade_texel_depth = cascade_texel_depth;
    f.shadow_inverse_view_projection = shadow_inverse_view_projection;
    f.cascade_count = cascade_count;
    f.directional_shadow_enabled = directional_shadow_enabled;
    f.directional_gi_enabled = directional_gi_enabled;
    return true;
}

bool SceneRenderer::prepare_scene_constants(Frame& f) {
    auto& scene = f.scene;
    auto& slice = f.slice;
    auto& options = f.options;
    auto& render_view = f.render_view;
    auto& shading = f.shading;
    auto& view_constants = f.view_constants;
    auto& renderer_settings = f.renderer_settings;
    auto& cascade_view_projection = f.cascade_view_projection;
    auto& cascade_texel_depth = f.cascade_texel_depth;
    auto& shadow_inverse_view_projection = f.shadow_inverse_view_projection;
    auto& cascade_count = f.cascade_count;
    auto& directional_gi_enabled = f.directional_gi_enabled;
    const auto& baked = f.scene.environment.baked;
    SceneConstants scene_constants{};
    view_constants.clip_plane[0] = options.clip_plane.x; view_constants.clip_plane[1] = options.clip_plane.y;
    view_constants.clip_plane[2] = options.clip_plane.z; view_constants.clip_plane[3] = options.clip_plane.w;
    for (std::size_t i = 0; i < active_reflections_.size(); ++i) {
        const auto& p = active_reflections_[i]; if (!p.enabled) continue;
        scene_constants.reflection_position[i][0] = p.position.x; scene_constants.reflection_position[i][1] = p.position.y; scene_constants.reflection_position[i][2] = p.position.z;
        scene_constants.reflection_position[i][3] = p.sphere ? 1.0F : 0.0F;
        scene_constants.reflection_extent[i][0] = p.extent.x; scene_constants.reflection_extent[i][1] = p.extent.y; scene_constants.reflection_extent[i][2] = p.extent.z;
        scene_constants.reflection_parameters[i][0] = p.blend;
        scene_constants.reflection_parameters[i][1] = p.intensity;
        scene_constants.reflection_parameters[i][2] = static_cast<float>(reflection_filters_[i]->limits().specular_mips);
        scene_constants.reflection_parameters[i][3] = 1;
    }
    scene_constants.planar_parameters[0] = planar_active_ ? scene.environment.planar.strength : 0;
    scene_constants.planar_parameters[1] = static_cast<float>(stats_.reflection_probes);

    scene_constants.texture_sampling[0] = scene.renderer_settings.texture_lod_bias;
    scene_constants.shadow_quality[0] = renderer_settings.shadow_blend_band;
    scene_constants.shadow_quality[1] = renderer_settings.shadow_filter_radius;
    const float sun_radius = scene.directional_lights.empty() ? 0.0F
        : std::clamp(scene.directional_lights.front().angular_radius, 0.0F, 0.249F);
    scene_constants.shadow_quality[2] = scene::deterministic::sin_turns(sun_radius)
        / scene::deterministic::cos_turns(sun_radius);
    scene_constants.contact_shadow[0] = renderer_settings.contact_shadow_length;
    scene_constants.contact_shadow[1] = renderer_settings.contact_shadow_thickness;
    scene_constants.contact_shadow[2] = 32.0F;
    // An orthographic view has no eye point to look from: every pixel sees
    // along the view. w = 0 carries that direction, toward the viewer, in
    // place of a position, and the shading reads it as the view vector.
    if (scene::orthographic(render_view.matrices.projection)) {
        scene_constants.eye_position[0U] = -render_view.forward.x;
        scene_constants.eye_position[1U] = -render_view.forward.y;
        scene_constants.eye_position[2U] = -render_view.forward.z;
        scene_constants.eye_position[3U] = 0.0F;
    } else {
        scene_constants.eye_position[0U] = render_view.eye.x;
        scene_constants.eye_position[1U] = render_view.eye.y;
        scene_constants.eye_position[2U] = render_view.eye.z;
        scene_constants.eye_position[3U] = 1.0F;
    }
    scene_constants.view_mode[0U] = static_cast<float>(shading);
    // The default fill, which goes out once the scene has a light.
    const scene::AmbientFill fill = scene::ambient_fill(scene);
    scene_constants.sky_ambient[0U] = fill.sky.x;
    scene_constants.sky_ambient[1U] = fill.sky.y;
    scene_constants.sky_ambient[2U] = fill.sky.z;
    scene_constants.sky_ambient[3U] = fill.strength;
    scene_constants.ground_exposure[0U] = fill.ground.x;
    scene_constants.ground_exposure[1U] = fill.ground.y;
    scene_constants.ground_exposure[2U] = fill.ground.z;
    // Basic remains the regression/reference renderer. Advanced consumes the
    // replicated physical camera controls, normalized so the existing default
    // camera is neutral while each authored stop retains its exact ratio.
    scene_constants.ground_exposure[3U] =
        mode_ == SceneRendererMode::AdvancedLinear
        ? scene::camera_exposure_multiplier(scene.camera)
        : 1.0F;
    scene_constants.counts[0U] =
        static_cast<core::u32>(light_scratch_.size() / sizeof(GpuLight));
    // Published for passes outside the lit draw. The buffer is sized to the
    // authored maximum and only partly written, so a consumer needs telling
    // where the lights stop.
    uploaded_light_count_ = scene_constants.counts[0U];
    scene_constants.particle_lights[0U] = particle_lights_used_;
    if (scene_linear() && renderer_settings.contact_shadow_length > 0.0F
        && renderer_settings.contact_shadow_thickness > 0.0F) {
        for (core::u32 i = 0U; i < uploaded_light_count_; ++i) {
            GpuLight light{};
            std::memcpy(&light, light_scratch_.data() + static_cast<std::size_t>(i) * sizeof(light), sizeof(light));
            if ((i == 0U && light.kind == GpuLightKind::Directional)
                || (light.kind == GpuLightKind::Spot && light.params[3U] >= 0.0F))
                ++stats_.contact_shadow_lights;
        }
        if (stats_.contact_shadow_lights == 0U)
            static_cast<void>(core::report_idle_once("contact shadows", "no directional light or allocated shadow-casting spot"));
    } else if (renderer_settings.contact_shadow_length > 0.0F) {
        static_cast<void>(core::report_idle_once("contact shadows", scene_linear()
            ? "contact thickness is zero" : "Advanced renderer is not selected"));
    } else if (scene_linear()) {
        static_cast<void>(core::report_idle_once("contact shadows", "contact length is zero"));
    }
    scene_constants.counts[1U] =
        scene.effect_settings.brightness_curve == scene::BrightnessCurve::Gain
            ? 1U
            : 0U;
    scene_constants.grain_context[0U] = slice.x;
    scene_constants.grain_context[1U] = slice.y;
    scene_constants.grain_context[2U] =
        static_cast<core::u32>(scene.show_frame & 0xFFFF'FFFFULL);
    scene_constants.grain_context[3U] =
        static_cast<core::u32>(scene.show_frame >> 32U);
    scene_constants.grain_extent[0U] = slice.width;
    scene_constants.grain_extent[1U] = slice.height;
    scene_constants.grain_extent[2U] = width();
    scene_constants.grain_extent[3U] = height();
    const auto lighting_inverse = scene::invert(render_view.canvas_view_projection);
    for (core::u32 index = 0U; index < 16U; ++index) {
        scene_constants.lighting_inverse_view_projection[index] = lighting_inverse.m[index];
        scene_constants.lighting_view_projection[index] = render_view.canvas_view_projection.m[index];
    }
    scene_constants.lighting_canvas[0U] = static_cast<float>(options.view && !options.full_canvas_view ? slice.width : scene.canvas.width);
    scene_constants.lighting_canvas[1U] = static_cast<float>(options.view && !options.full_canvas_view ? slice.height : scene.canvas.height);
    scene_constants.lighting_canvas[2U] = options.view && !options.full_canvas_view ? 0.0F : static_cast<float>(slice.x);
    scene_constants.lighting_canvas[3U] = options.view && !options.full_canvas_view ? 0.0F : static_cast<float>(slice.y);
    for (core::u32 cascade = 0U; cascade < scene::max_shadow_cascades;
         ++cascade) {
        for (core::u32 index = 0U; index < 16U; ++index) {
            scene_constants.shadow_view_projection[cascade][index] =
                cascade_view_projection[cascade].m[index];
        }
        scene_constants.shadow_cascade_texel_depth[cascade] =
            cascade_texel_depth[cascade];
    }
    for (core::u32 index = 0U; index < 16U; ++index) {
        scene_constants.shadow_inverse_view_projection[index] =
            shadow_inverse_view_projection.m[index];
    }
    const float reciprocal_shadow = 1.0F
        / static_cast<float>(limits_.directional_shadow_resolution);
    scene_constants.shadow_parameters[0U] = reciprocal_shadow;
    scene_constants.shadow_parameters[1U] = reciprocal_shadow;
    // In shadow texels, so each cascade scales them by its own texel depth.
    scene_constants.shadow_parameters[2U] = 1.5F;
    scene_constants.shadow_parameters[3U] = 6.0F;
    if (baked.enabled && stats_.baked_ready && probe_asset_) {
        const auto& grid = probe_asset_->grid;
        scene_constants.baked_minimum[0] = grid.minimum.x;
        scene_constants.baked_minimum[1] = grid.minimum.y;
        scene_constants.baked_minimum[2] = grid.minimum.z;
        scene_constants.baked_maximum[0] = grid.maximum.x;
        scene_constants.baked_maximum[1] = grid.maximum.y;
        scene_constants.baked_maximum[2] = grid.maximum.z;
        scene_constants.baked_dimensions[0] = static_cast<float>(grid.count_x);
        scene_constants.baked_dimensions[1] = static_cast<float>(grid.count_y);
        scene_constants.baked_dimensions[2] = static_cast<float>(grid.count_z);
        scene_constants.baked_dimensions[3] = baked.strength;
    }
    scene_constants.gi_parameters[0U] = 0.15F;
    scene_constants.gi_parameters[1U] = directional_gi_enabled
        ? renderer_settings.gi_strength
        : 0.0F;
    scene_constants.gi_parameters[2U] = 0.0025F;
    // The environment, or nothing. Zero intensity is what selects the
    // hemisphere fallback in the shader, so a scene with no authored map takes
    // exactly the path it took before any of this existed.
    if (environment_.has_value() && environment_->maps().complete()
        && environment_generation_ != 0U) {
        scene_constants.environment_parameters[0U] =
            physical_environment_ ? 1.0F : std::max(scene.environment.map_intensity, 0.0F);
        scene_constants.environment_parameters[1U] =
            static_cast<float>(environment_->limits().specular_mips);
    }
    scene_constants.counts[2U] = cascade_count;
    // Directional lights are packed before local lights; the first authored
    // directional light therefore has the stable packed index zero.
    scene_constants.counts[3U] = 0U;
    // What lit particles light by: the same records, hemisphere, exposure
    // and cascades the lit pass reads, so they sit with the surfaces.
    ParticleLighting particle_lighting{};
    particle_lighting.lights = &light_buffer_;
    particle_lighting.light_count = scene_constants.counts[0U];
    if (particle_light_buffer_ && particle_lights_used_ != 0U) {
        particle_lighting.particle_lights = &particle_light_buffer_;
        particle_lighting.particle_light_count = particle_lights_used_;
    }
    std::copy_n(scene_constants.sky_ambient, 4U, particle_lighting.sky_ambient.begin());
    std::copy_n(scene_constants.ground_exposure, 4U, particle_lighting.ground_exposure.begin());
    particle_lighting.cascade_count = cascade_count;
    for (core::u32 cascade = 0U; cascade < cascade_count; ++cascade) {
        particle_lighting.cascades[cascade] = &cascade_target(cascade);
        particle_lighting.cascade_view_projection[cascade] = cascade_view_projection[cascade];
        particle_lighting.cascade_texel_depth[cascade] = cascade_texel_depth[cascade];
    }
    f.scene_constants = scene_constants;
    f.particle_lighting = particle_lighting;
    return true;
}

bool SceneRenderer::prepare_bindings(Frame& f) {
    f.vertex_bindings = {
        &geometry_->buffer(GeometryStream::Vertices), &geometry_->buffer(GeometryStream::Indices), &instance_buffer_,
        &geometry_->buffer(GeometryStream::Tangents), &geometry_->buffer(GeometryStream::Skins), &pose_buffer_};
    f.batch_indices =
        f.indexed ? &geometry_->buffer(GeometryStream::Indices) : nullptr;
    // The shadow table is the fourth, and only the Advanced pipeline declares
    // it. Bound as a span rather than a fixed array so the count follows the
    // pipeline: Basic binding four against a signature that declares three is
    // a refused draw, and the refusal says nothing about which count was
    // wrong.
    f.lit_fragment_buffers = {
        &light_buffer_,
        &inline_effect_op_buffer_,
        &inline_effect_parameter_buffer_,
        &ltc_asset_.buffer(), &light_shadow_buffer_, &probe_buffer_, &particle_light_buffer_};
    f.lit_fragment_bindings = std::span<const rhi::StorageBuffer* const>{
        f.lit_fragment_buffers.data(),
        mode_ == SceneRendererMode::AdvancedLinear ? 7U : 4U};
    f.media_fragment_bindings = {
        &inline_effect_op_buffer_, &inline_effect_parameter_buffer_};

    f.coverage_sampling = {f.renderer_settings.texture_lod_bias, 0.0F, 0.0F, 0.0F};
    return true;
}

bool SceneRenderer::record_shadow_maps(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    auto& cascade_view_projection = f.cascade_view_projection;
    auto& cascade_count = f.cascade_count;
    auto& directional_shadow_enabled = f.directional_shadow_enabled;
    auto& directional_gi_enabled = f.directional_gi_enabled;
    auto& vertex_bindings = f.vertex_bindings;
    auto& batch_indices = f.batch_indices;
    auto& coverage_sampling = f.coverage_sampling;
    // Shadow maps: each drawn from the casters its projection can reach, and
    // kept from an earlier frame while nothing its draw reads has changed -
    // the projection, the caster instance records and joint palettes, the
    // geometry arena, the alpha textures. That is the usual state of a set
    // under a fixed sun: the full-scene cascade and the reflective shadow map
    // depend on no camera, so they stay put while the camera moves, and a
    // held camera keeps every cascade.
    //
    // The same digest finds a map another renderer on this device drew. Every
    // view of one scene - Program, its transition target, the stage view, a
    // mapped output - fits its cascades to the authored camera rather than to
    // its own view, so the first of them recorded draws the maps and the rest
    // sample them.
    //
    // Neither choice can make two nodes disagree. A caster is left out of a
    // map only when it lies wholly beyond one of the map's side planes, where
    // it would have drawn nothing; and a reused map holds exactly the texels
    // the same draw would write again.
    const bool atlas_wanted = !shadow_atlas_plan_.tiles.empty() && atlas_home_;
    const rhi::CommandRecordingToken shadow_recording =
        directional_shadow_enabled || atlas_wanted ? commands.recording_token() : rhi::CommandRecordingToken{};
    const TextureResidency* const texture_residency =
        texture_streamer_ ? texture_streamer_->residency().get() : nullptr;
    std::unique_lock<std::recursive_mutex> textures_held;
    if (directional_shadow_enabled || atlas_wanted) {
        if (texture_streamer_ && texture_streamer_->residency()) {
            textures_held = texture_streamer_->residency()->lock();
        }
        instance_digests_.clear();
        for (std::size_t slot = 0U; slot < opaque_submissions_.size(); ++slot) {
            GpuInstance instance{};
            std::memcpy(&instance, instance_scratch_.data() + slot * sizeof(GpuInstance),
                        sizeof(instance));
            // Digested as a caster draw reads it. The previous transform, the
            // reflection/receiver flag and the inline program range feed only
            // camera products, and differ between views of one scene.
            std::fill(std::begin(instance.previous_world), std::end(instance.previous_world), 0.0F);
            instance.alpha[3U] = 0.0F;
            std::fill(std::begin(instance.effect_range), std::end(instance.effect_range), 0U);
            core::u64 digest = digest_bytes(0U, bytes_of(instance));
            if (instance.pose[1] != 0U) {
                // A skinned caster moves with its joint palette, which its
                // record only points at. A caster draw reads the current poses.
                const std::size_t poses = pose_upload_.size() / sizeof(GpuPose);
                const std::size_t first = instance.pose[0];
                const std::size_t count = instance.pose[1];
                if (first <= poses && count <= poses - first) {
                    for (std::size_t pose = first; pose < first + count; ++pose) {
                        digest = digest_bytes(digest, std::span<const core::u8>{
                            pose_upload_.data() + pose * sizeof(GpuPose) + offsetof(GpuPose, current),
                            sizeof(scene::Mat4)});
                    }
                } else {
                    digest = mix_digest(digest, unmatched_digest());
                }
            }
            instance_digests_.push_back(digest);
        }
    }
    // A coverage texture by what it holds rather than by its object: this
    // renderer's white fallback by a constant, a model texture by the serial
    // its residency gave it, which no other texture has had. Anything else
    // matches nothing.
    const auto coverage_identity = [&](const rhi::SampledTexture& bound) noexcept {
        core::u64 texture = 0U;
        if (bound.texture == &material_fallback_) {
            texture = 0x5748'4954'45ULL;
        } else if (texture_residency != nullptr) {
            texture = texture_residency->serial(bound.texture);
        }
        if (texture == 0U) {
            texture = unmatched_digest();
        }
        const auto* const samplers = material_samplers_.data();
        const core::u64 sampler = bound.sampler >= samplers
                && bound.sampler < samplers + material_samplers_.size()
            ? static_cast<core::u64>(bound.sampler - samplers)
            : unmatched_digest();
        return mix_digest(texture, sampler);
    };
    // Fills casters_ with the opaque instances `view_projection` can reach,
    // in the batch order every map uses, and returns a digest of what drawing
    // them reads.
    const core::u64 caster_scope = mix_digest(geometry_->epoch(),
        std::bit_cast<core::u32>(coverage_sampling[0U]));
    const auto gather_casters = [&](const scene::Mat4& view_projection) -> core::u64 {
        casters_.batches.clear();
        casters_.uniforms.clear();
        casters_.textures.clear();
        casters_.instances = 0U;
        core::u64 key = digest_bytes(caster_scope, std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(view_projection.m.data()), sizeof(view_projection.m)});
        for (std::size_t index = 0U; index < batches_.size(); ++index) {
            BatchConstants head{};
            std::memcpy(&head, batch_uniforms_.data() + index * sizeof(BatchConstants), sizeof(head));
            const rhi::DrawBatch& batch = batches_[index];
            const rhi::SampledTexture& coverage_texture = rsm_flux_textures_[index];
            const core::u32 end = head.first_instance + batch.instance_count;
            core::u32 run_begin = 0U;
            bool in_run = false;
            for (core::u32 slot = head.first_instance; slot <= end; ++slot) {
                const bool reached = slot < end && !outside_shadow_projection(view_projection,
                    opaque_submissions_[slot].world, opaque_submissions_[slot].bounds);
                if (reached && !in_run) {
                    run_begin = slot;
                    in_run = true;
                } else if (!reached && in_run) {
                    in_run = false;
                    casters_.batches.push_back({batch.vertex_count, slot - run_begin, batch.first_index});
                    BatchConstants constants = head;
                    constants.first_instance = run_begin;
                    append(casters_.uniforms, constants);
                    casters_.textures.push_back(coverage_texture);
                    casters_.instances += slot - run_begin;
                    key = mix_digest(key, (static_cast<core::u64>(head.first_index) << 32U) | batch.vertex_count);
                    key = mix_digest(key, (static_cast<core::u64>(run_begin) << 32U) | (slot - run_begin));
                    key = mix_digest(key, coverage_identity(coverage_texture));
                    for (core::u32 drawn = run_begin; drawn < slot; ++drawn) {
                        key = mix_digest(key, instance_digests_[drawn]);
                    }
                }
            }
        }
        return key;
    };
    // A product's key also names its kind and size, so a cascade never
    // stands in for an atlas or a map of another resolution.
    const auto product_key = [](core::u64 key, core::u64 kind, core::u32 extent) noexcept {
        return mix_digest(mix_digest(key, kind), extent);
    };
    // Chooses what this recording samples for `key`: what it sampled last
    // time, or what another renderer drew, while either is current. Otherwise
    // binds this renderer's own product, cleared of its key until the caller
    // has drawn it, and returns false. Its own other products are never taken
    // for a slot: one of them may yet be redrawn in this recording.
    const auto sample = [&](GeometryResidency::ShadowHandle& bound,
                            const GeometryResidency::ShadowHandle& home, core::u64 key) {
        const auto owned = [&](const GeometryResidency::ShadowHandle& product) noexcept {
            return product == rsm_home_ || product == atlas_home_
                || std::find(cascade_homes_.begin(), cascade_homes_.end(), product)
                    != cascade_homes_.end();
        };
        if (!bound || !GeometryResidency::shadow_current(*bound, key, shadow_recording)) {
            bound = geometry_->find_shadow(key, shadow_recording);
        }
        if (bound && bound != home && owned(bound)) {
            bound.reset();
        }
        if (!bound) {
            bound = home;
            home->key = 0U;
            home->recording = {};
            home->submitted = false;
            return false;
        }
        ++stats_.shadow_maps_reused;
        if (bound != home) {
            ++stats_.shadow_maps_shared;
        }
        return true;
    };
    const auto drawn = [&](GeometryResidency::ShadowProduct& product, core::u64 key) noexcept {
        product.key = key;
        product.recording = shadow_recording;
        product.submitted = false;
    };
    const auto caster_draw = [&](const rhi::GraphicsPipeline& pipeline) noexcept {
        rhi::PulledDraw caster{};
        caster.pipeline = &pipeline;
        caster.fragment_uniform = bytes_of(coverage_sampling);
        caster.fragment_textures = casters_.textures;
        caster.fragment_textures_per_batch = 1U;
        caster.vertex_buffers = vertex_bindings;
        caster.batch_uniforms = casters_.uniforms;
        caster.batch_uniform_stride = static_cast<core::u32>(sizeof(BatchConstants));
        caster.batches = casters_.batches;
        caster.index_buffer = batch_indices;
        return caster;
    };
    const core::u32 shadow_extent = limits_.directional_shadow_resolution;
    for (core::u32 cascade = cascade_count; cascade < scene::max_shadow_cascades; ++cascade) {
        cascades_[cascade].reset();
    }
    if (directional_shadow_enabled) {
        ViewConstants shadow_view{};
        const auto view_from = [&](const scene::Mat4& matrix) noexcept {
            for (core::u32 index = 0U; index < 16U; ++index) {
                shadow_view.view_projection[index] = matrix.m[index];
                shadow_view.previous_view_projection[index] = matrix.m[index];
                shadow_view.canvas_view_projection[index] = matrix.m[index];
            }
        };
        // The full-scene fit comes last, so after this loop casters_ holds
        // its casters for the reflective-shadow-map passes.
        core::u64 full_scene_key{};
        for (core::u32 cascade = 0U; cascade < cascade_count; ++cascade) {
            full_scene_key = gather_casters(cascade_view_projection[cascade]);
            const core::u64 key = product_key(full_scene_key, 0x435343ULL, shadow_extent);
            if (sample(cascades_[cascade], cascade_homes_[cascade], key)) {
                continue;
            }
            view_from(cascade_view_projection[cascade]);
            rhi::PulledDraw shadow_draw = caster_draw(shadow_pipeline_);
            shadow_draw.vertex_uniform = bytes_of(shadow_view);
            if (!graph_.draw_pulled(commands, core::RenderPass::Shadows,
                    cascade_homes_[cascade]->textures[0U],
                    rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, shadow_depth_, 0.0F,
                    shadow_draw)) {
                return false;
            }
            stats_.shadow_caster_instances += casters_.instances;
            drawn(*cascade_homes_[cascade], key);
        }
        if (directional_gi_enabled) {
            const auto& light = scene.directional_lights.front();
            const auto emission = scene::light_emission(light, scene.renderer_settings.photometric_scale);
            RsmFluxConstants flux_constants{};
            flux_constants.direction_intensity[0U] = light.direction.x;
            flux_constants.direction_intensity[1U] = light.direction.y;
            flux_constants.direction_intensity[2U] = light.direction.z;
            flux_constants.direction_intensity[3U] = emission.intensity;
            flux_constants.light_color[0U] = emission.color.x;
            flux_constants.light_color[1U] = emission.color.y;
            flux_constants.light_color[2U] = emission.color.z;
            flux_constants.light_color[3U] = scene.renderer_settings.texture_lod_bias;
            const core::u64 rsm_key = product_key(digest_bytes(
                mix_digest(full_scene_key, 0x52534DULL), bytes_of(flux_constants)),
                0x52534DULL, shadow_extent);
            if (!sample(rsm_, rsm_home_, rsm_key)) {
                // The reflective-shadow-map attributes belong to the
                // full-scene fit, which is the last cascade.
                view_from(cascade_view_projection[cascade_count - 1U]);
                rhi::PulledDraw rsm_draw = caster_draw(rsm_normal_pipeline_);
                rsm_draw.vertex_uniform = bytes_of(shadow_view);
                if (!graph_.draw_pulled(commands, core::RenderPass::ShadowAttributes,
                        rsm_home_->textures[0U], rhi::Color{}, shadow_depth_, 0.0F,
                        rsm_draw)) {
                    return false;
                }
                rsm_draw.pipeline = &rsm_flux_pipeline_;
                rsm_draw.fragment_uniform = bytes_of(flux_constants);
                if (!graph_.draw_pulled(commands, core::RenderPass::ShadowAttributes,
                        rsm_home_->textures[1U], rhi::Color{}, shadow_depth_, 0.0F,
                        rsm_draw)) {
                    return false;
                }
                drawn(*rsm_home_, rsm_key);
            }
        } else {
            rsm_.reset();
        }
    } else {
        rsm_.reset();
    }

    // The local-light shadow atlas: one tile per shadow-casting spot.
    //
    // Outside the directional block on purpose. A set lit only by practicals
    // has no directional light and no cascades at all, and that is exactly the
    // scene this feature exists for - nesting it under the sun would mean the
    // interior sets that need local shadows most were the ones that never got
    // them.
    //
    // The atlas is cleared once and every tile then loads, because a clear is
    // not viewport-scoped: clearing per tile would wipe the tiles already
    // drawn. Each draw is confined to its own rectangle by the viewport, so
    // two lights cannot write each other's map. For the same reason the atlas
    // is reused or redrawn whole.
    //
    // Each tile draws the casters its spot's projection reaches, under the
    // same rule as the cascades.
    if (atlas_wanted) {
        const auto tile_projection = [&](std::size_t index) noexcept {
            GpuLightShadow record{};
            std::memcpy(&record,
                        light_shadow_scratch_.data()
                            + index * sizeof(GpuLightShadow),
                        sizeof(record));
            scene::Mat4 matrix{};
            for (core::u32 lane = 0U; lane < 16U; ++lane) {
                matrix.m[lane] = record.view_projection[lane];
            }
            return matrix;
        };
        core::u64 atlas_key = mix_digest(0x41544CULL, shadow_atlas_plan_.tiles.size());
        for (std::size_t index = 0U; index < shadow_atlas_plan_.tiles.size(); ++index) {
            const ShadowTile& tile = shadow_atlas_plan_.tiles[index];
            atlas_key = mix_digest(atlas_key, gather_casters(tile_projection(index)));
            atlas_key = mix_digest(atlas_key, (static_cast<core::u64>(tile.x) << 32U) | tile.y);
            atlas_key = mix_digest(atlas_key, tile.extent);
        }
        atlas_key = product_key(atlas_key, 0x41544CULL, shadow_atlas_limits_.extent);
        if (!sample(atlas_, atlas_home_, atlas_key)) {
            rhi::Texture& atlas = atlas_home_->textures[0U];
            if (!graph_.clear(commands, core::RenderPass::Shadows, atlas,
                                rhi::Color{0.0F, 0.0F, 0.0F, 1.0F})) {
                return false;
            }
            ViewConstants tile_view{};
            for (std::size_t index = 0U;
                 index < shadow_atlas_plan_.tiles.size(); ++index) {
                const ShadowTile& tile = shadow_atlas_plan_.tiles[index];
                const scene::Mat4 projection = tile_projection(index);
                static_cast<void>(gather_casters(projection));
                for (core::u32 lane = 0U; lane < 16U; ++lane) {
                    tile_view.view_projection[lane] = projection.m[lane];
                    tile_view.previous_view_projection[lane] = projection.m[lane];
                    tile_view.canvas_view_projection[lane] = projection.m[lane];
                }
                rhi::PulledDraw tile_draw = caster_draw(shadow_pipeline_);
                tile_draw.vertex_uniform = bytes_of(tile_view);
                tile_draw.load = true;
                tile_draw.viewport =
                    rhi::ViewportRect{tile.x, tile.y, tile.extent, tile.extent};
                if (!graph_.draw_pulled(commands, core::RenderPass::Shadows,
                        atlas, rhi::Color{0.0F, 0.0F, 0.0F, 1.0F},
                        shadow_atlas_depth_, 0.0F, tile_draw)) {
                    return false;
                }
                stats_.shadow_caster_instances += casters_.instances;
            }
            drawn(*atlas_home_, atlas_key);
        }
    } else {
        atlas_.reset();
    }
    textures_held = {};
    // The batches were assembled before it was known whose maps this
    // recording samples.
    if (scene_linear()) {
        const std::span<rhi::SampledTexture> batch_textures{camera_batch_textures_};
        for (std::size_t first = 0U; first + advanced_fragment_textures <= batch_textures.size();
             first += advanced_fragment_textures) {
            bind_shadow_products(batch_textures.subspan(first, advanced_fragment_textures));
        }
    }
    return true;
}

} // namespace mediaserver::render
