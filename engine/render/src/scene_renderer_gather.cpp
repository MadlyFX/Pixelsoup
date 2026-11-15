// SceneRenderer::render, gathering: instances, models and media into opaque
// batches and sorted composite layers.

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

bool SceneRenderer::gather_instances(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    auto& options = f.options;
    auto& media_cache = f.media_cache;
    auto& render_view = f.render_view;
    auto& shadow_bounds = f.shadow_bounds;
    auto& fallback_material_textures = f.fallback_material_textures;
    f.fallback_material_textures = {
        &material_fallback_, &material_fallback_, &normal_fallback_,
        &material_fallback_, &material_fallback_,
        &material_fallback_, &material_fallback_, &normal_fallback_,
        &material_fallback_, &material_fallback_, &material_fallback_,
        &material_fallback_, &material_fallback_, &normal_fallback_};
    f.frusta.emplace(scene);
    animated_instances_.clear();
    pose_upload_.clear();

    for (const auto& instance : scene.instances) {
        if (options.excluded_source != doc::NodeId{} && instance.source == options.excluded_source) continue;
        if (opacity(instance.style) <= 0.0F) {
            continue;
        }
        if (f.held_instances() >= limits_.max_instances) {
            ++stats_.dropped_instances;
            return false;
        }
        const bool visible = f.camera_visible(instance.world, {{-0.5F,-0.5F,-0.5F},{0.5F,0.5F,0.5F}});
        f.selected(instance.source, false, visible, 0U);
        const auto key = scene::normalized(instance.mesh);
        const MeshRange* const mesh_range = ensure_resident(key, commands);
        if (mesh_range == nullptr || mesh_range->index_count == 0U) {
            ++stats_.dropped_instances;
            continue;
        }

        auto instance_textures = fallback_material_textures;
        const std::array<std::string_view, 5U> texture_sources{
            instance.material.base_color_texture,
            instance.material.metallic_roughness_texture,
            instance.material.normal_texture,
            instance.material.occlusion_texture,
            instance.material.emissive_texture};
        const bool has_generator_texture = std::any_of(
            texture_sources.begin(), texture_sources.end(),
            [](std::string_view source) { return !source.empty(); });
        for (std::size_t texture_index = 0U;
             texture_index < texture_sources.size(); ++texture_index) {
            if (texture_sources[texture_index].empty()) continue;
            const auto texture = media_cache.resolve(
                *device_, commands,
                std::string{texture_sources[texture_index]}, 0U, {},
                instance.source.value());
            if (texture) instance_textures[texture_index] = texture.primary;
        }

        // Occluders join the batched depth-writing pass whatever their own
        // compositing style says, because the only thing wanted from them is
        // the depth their silhouette would have written.
        const bool instance_depth_only = options.depth_only(instance.source);
        if (!instance_depth_only && (has_generator_texture
            || needs_compositing(instance.style)
            || instance.material.transmission > 0.0F || instance.material.alpha_mode == scene::MaterialAlphaMode::Blend)) {
            if (!visible) continue;
            f.queue_composite(
                *mesh_range,
                instance,
                ResolvedMediaFrame{},
                instance_textures,
                camera_depth(instance.world, scene::Vec3{}, render_view.eye,
                             render_view.forward),
                instance.source,
                std::nullopt,
                Frame::SiblingOrder{instance.parent_group, instance.group_order},
                {},
                primitive_bounds(key.primitive));
            continue;
        }

        InlineProgramRange effect_range{};
        if (!instance_depth_only
            && !f.inline_range_for(
                instance.source, instance.style.effects, effect_range,
                surface_fill_program(instance))) {
            return false;
        }
        append(
            instance_scratch_,
            f.instance_record(
                instance,
                effect_range.first,
                effect_range.count,
                instance_depth_only));
        expand_directional_shadow_bounds(
            shadow_bounds, instance.world,
            scene::Vec3{-0.5F, -0.5F, -0.5F},
            scene::Vec3{0.5F, 0.5F, 0.5F});
        opaque_submissions_.push_back({*mesh_range, fallback_material_textures,
            {}, key, instance.source, 0U, stats_.instances, visible, {}, instance.world,
            ModelBounds{{-0.5F, -0.5F, -0.5F}, {0.5F, 0.5F, 0.5F}}});
        ++stats_.instances;
        if (instance_depth_only) {
            ++stats_.depth_only_instances;
        }
    }
    return true;
}

bool SceneRenderer::gather_models(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    auto& options = f.options;
    auto& model_publication_fence = f.model_publication_fence;
    auto& render_view = f.render_view;
    auto& shadow_bounds = f.shadow_bounds;
    auto& view_history = *f.view_history;
    auto& advance_history = f.advance_history;
    auto& hold_history = f.hold_history;
    // Imported models upload once per asset while retaining a batch per source
    // material part. Their transformed asset-bounds centre supplies one stable
    // sort depth for every part of an instance.
    for (const auto& model : scene.models) {
        if (options.excluded_source != doc::NodeId{} && model.source_node == options.excluded_source) continue;
        if (opacity(model.style) <= 0.0F) {
            continue;
        }
        const std::string& model_source = model.source;
        const ResidentModel* const resident = model_source.empty()
            ? nullptr
            : ensure_model_resident(
                model_source, commands, model_publication_fence);
        if (resident == nullptr) {
            ++stats_.dropped_instances;
            // A cold model can be omitted only while pending uploads - its
            // textures, or its geometry still streaming into the arena - keep
            // assets unready, so the presenter submits this recording and
            // holds the picture. Ready textures must never turn a geometry
            // refusal into a successfully presented hole.
            if (texture_streamer_ && stats_.texture_streaming.state !=
                    TextureStreamState::Loading && !streaming_geometry(model_source)) return false;
            continue;
        }
        if (texture_streamer_ &&
            (!texture_streamer_->contains(model_source, resident->generation, *resident->decoded_asset) ||
             stats_.model_integration_refusals != 0U)) return false;
        if (resident->parts.empty()) {
            ++stats_.dropped_instances;
            continue;
        }
        const auto& asset=*resident->decoded_asset;
        const bool deforming=!asset.pose_bindings.empty() || model.animation.wind!=scene::Vec3{};
        ModelBounds animated_bounds=resident->bounds;
        if (deforming) {
            const auto count=static_cast<core::u32>(asset.pose_bindings.size());
            const auto offset=static_cast<core::u32>(pose_upload_.size()/sizeof(GpuPose));
            if (animated_instances_.size()==animated_instances_.capacity() || count>max_frame_pose_bindings-offset) {
                stats_.assets.ready=false; ++stats_.dropped_instances;
                static_cast<void>(core::report_idle_once("Model animation","Frame joint palette capacity exceeded")); return false;
            }
            const auto previous_frame=advance_history ? view_history.previous_show_frame
                : hold_history ? view_history.motion_transform_show_frame : scene.show_frame;
            if (!evaluate_model_pose(asset,model.animation,scene.show_frame,scene.show_rate,*pose_scratch_,current_pose_)
                || !evaluate_model_pose(asset,model.animation,previous_frame,scene.show_rate,*pose_scratch_,previous_pose_)) {
                stats_.assets.ready=false;
                static_cast<void>(core::report_idle_once("Model animation","Invalid animation clip or nonfinite pose")); return false;
            }
            for (core::u32 j=0; j<count; ++j) append(pose_upload_,GpuPose{current_pose_[j],previous_pose_[j]});
            const auto wind=model_wind(model.animation,scene.show_frame);
            const auto prior_wind=model_wind(model.animation,previous_frame);
            const float height=asset.bounds.maximum.y-asset.bounds.minimum.y;
            animated_instances_.push_back({model.source_node,offset,count,wind,prior_wind,asset.bounds.minimum.y,height>1e-8F?1.0F/height:0.0F,
                resident->geometry->span.first_skin,resident->geometry->span.first_vertex});
            // Wind is applied in baked model space before skinning; expand the
            // bind bounds first, then transform every corner by every binding.
            auto expanded=asset.bounds;
            const scene::Vec3 extent{std::abs(wind.x),std::abs(wind.y),std::abs(wind.z)};
            expanded.minimum=expanded.minimum-extent; expanded.maximum=expanded.maximum+extent;
            animated_bounds=model_pose_bounds(asset,std::span{current_pose_}.first(count),wind);
            // Conservative skin motion is independent of the view. Dynamic
            // models keep LOD zero; external animated LODs fail ingest.
            if (count && wind!=scene::Vec3{}) {
                for (core::u32 j=0;j<count;++j) for (unsigned corner=0;corner<8;++corner) {
                    const auto p=scene::transform_point(current_pose_[j],{(corner&1U)?expanded.maximum.x:expanded.minimum.x,
                        (corner&2U)?expanded.maximum.y:expanded.minimum.y,(corner&4U)?expanded.maximum.z:expanded.minimum.z});
                    animated_bounds.minimum={std::min(animated_bounds.minimum.x,p.x),std::min(animated_bounds.minimum.y,p.y),std::min(animated_bounds.minimum.z,p.z)};
                    animated_bounds.maximum={std::max(animated_bounds.maximum.x,p.x),std::max(animated_bounds.maximum.y,p.y),std::max(animated_bounds.maximum.z,p.z)};
                }
            }
        }
        const bool visible = f.camera_visible(model.world, animated_bounds);
        const auto& levels = resident->decoded_asset->lods;
        // An isolated part is named among the full-detail parts, so an
        // isolating view stays at full detail.
        const core::u32 lod = deforming || render_view.model_part.has_value()
            ? 0U : select_model_lod(scene, model.world, resident->bounds, levels);
        f.selected(model.source_node, true, visible, lod);
        if (levels.size() > 1U) ++stats_.lod_models;
        if (lod > 0U) ++stats_.lod_reductions;
        const bool model_depth_only = options.depth_only(model.source_node);
        const auto local_center =
            (animated_bounds.minimum + animated_bounds.maximum) * 0.5F;
        const float depth = camera_depth(model.world, local_center,
                                         render_view.eye,
                                         render_view.forward);
        const auto texture_for = [&](std::optional<core::u32> index)
            -> const rhi::Texture* {
            if (texture_streamer_ && index) {
                if (const auto* texture = texture_streamer_->find(model_source, resident->generation, *index))
                    return texture;
                return &material_fallback_;
            }
            if (index && *index < resident->textures.size()) {
                const auto& texture = resident->textures[*index];
                if (texture) {
                    return &texture;
                }
            }
            return &material_fallback_;
        };
        const ModelLod level = levels.empty()
            ? ModelLod{0U, static_cast<core::u32>(resident->parts.size()), 0.0F}
            : levels[lod];
        // Fit/count the base caster set. A LOD with fewer material runs must
        // not move a cascade or change its caster count.
        if (model_depth_only || !needs_compositing(model.style)) {
            const core::u32 base_parts = levels.empty()
                ? static_cast<core::u32>(resident->parts.size()) : levels.front().part_count;
            for (core::u32 i = 0U; i < base_parts; ++i) {
                expand_directional_shadow_bounds(shadow_bounds, model.world,
                    animated_bounds.minimum, animated_bounds.maximum);
            }
        }
        for (core::u32 part_index = level.first_part;
             part_index < level.first_part + level.part_count; ++part_index) {
            if (render_view.model_part.has_value()
                && part_index - level.first_part != *render_view.model_part) {
                continue;
            }
            const auto& part = resident->parts[part_index];
            if (f.held_instances() >= limits_.max_instances) {
                ++stats_.dropped_instances;
                return false;
            }
            if (part.range.index_count == 0U) {
                ++stats_.dropped_instances;
                continue;
            }

            scene::MeshInstance proxy{};
            proxy.world = model.world;
            proxy.normal = model.normal;
            // Source-assigned material factors are authoritative. The authored
            // model material is used only for an unassigned primitive; an
            // explicit future override therefore needs its own state bit rather
            // than making today's default gray erase imported appearance.
            proxy.material = scene::model_part_material(model,
                part.has_material ? part.material : model.material);
            proxy.source = model.source_node;
            proxy.style = model.style;
            const std::array<const rhi::Texture*, 14U> material_textures{
                texture_for(part.base_color_texture),
                texture_for(part.metallic_roughness_texture),
                part.normal_texture.has_value()
                    ? texture_for(part.normal_texture)
                    : &normal_fallback_,
                texture_for(part.occlusion_texture),
                texture_for(part.emissive_texture),
                texture_for(part.clearcoat_texture),
                texture_for(part.clearcoat_roughness_texture),
                part.clearcoat_normal_texture ? texture_for(part.clearcoat_normal_texture) : &normal_fallback_,
                texture_for(part.sheen_color_texture),
                texture_for(part.sheen_roughness_texture),
                texture_for(part.transmission_texture),
                texture_for(part.specular_texture),
                texture_for(part.specular_color_texture),
                part.anisotropy_texture ? texture_for(part.anisotropy_texture) : &normal_fallback_};
            if (!part.clearcoat_normal_texture) proxy.material.clearcoat_normal_scale = 0.0F;
            const bool has_normal_map =
                part.normal_texture.has_value()
                && material_textures[2U] != &material_fallback_
                && material_textures[2U] != &normal_fallback_;
            const bool has_occlusion_map =
                part.occlusion_texture.has_value()
                && material_textures[3U] != &material_fallback_;

            if (!model_depth_only && (needs_compositing(model.style)
                || proxy.material.transmission > 0.0F || proxy.material.alpha_mode == scene::MaterialAlphaMode::Blend)) {
                if (!visible) continue;
                f.queue_composite(
                    part.range,
                    proxy,
                    ResolvedMediaFrame{},
                    material_textures,
                    depth,
                    model.source_node,
                    std::nullopt,
                    Frame::SiblingOrder{model.parent_group, model.group_order}, part.samplers,
                    animated_bounds);
                continue;
            }

            const auto slot = stats_.instances;
            InlineProgramRange effect_range{};
            if (!model_depth_only
                && !f.inline_range_for(
                    model.source_node, model.style.effects, effect_range)) {
                return false;
            }
            append(
                instance_scratch_,
                f.instance_record(
                    proxy,
                    effect_range.first,
                    effect_range.count,
                    model_depth_only,
                    DecodedPixelLayout::Rgba8,
                    has_normal_map,
                    has_occlusion_map,
                    material_textures[2U]->desc().format == rhi::TextureFormat::Bc5Unorm,
                    material_textures[7U]->desc().format == rhi::TextureFormat::Bc5Unorm, model.motion_source));
            opaque_submissions_.push_back({part.range, material_textures,
                model.source, {}, model.source_node, part_index, slot, visible, part.samplers,
                model.world, animated_bounds});
            ++stats_.instances;
            if (model_depth_only) {
                ++stats_.depth_only_instances;
            }
        }
    }
    return true;
}

bool SceneRenderer::build_opaque_batches(Frame& f) {
    auto& scene = f.scene;
    std::sort(opaque_submissions_.begin(), opaque_submissions_.end(),
        [](const OpaqueSubmission& a, const OpaqueSubmission& b) {
            if (a.asset != b.asset) return a.asset < b.asset;
            if (a.mesh != b.mesh) return a.mesh < b.mesh;
            if (a.part != b.part) return a.part < b.part;
            if (a.source != b.source) return a.source < b.source;
            return a.original_slot < b.original_slot;
        });
    for (const auto& submission : opaque_submissions_) {
        const auto offset = static_cast<std::size_t>(submission.original_slot) * sizeof(GpuInstance);
        reordered_instances_.insert(reordered_instances_.end(),
            instance_scratch_.data() + offset, instance_scratch_.data() + offset + sizeof(GpuInstance));
        opaque_visibility_.push_back(submission.visible ? 1U : 0U);
    }
    instance_scratch_.swap(reordered_instances_);
    // Batched opaque geometry is drawn indexed where the device can, so a
    // vertex shared by several triangles is shaded once rather than once per
    // triangle. Draws of the same triangles produce the same picture either way.
    const bool indexed = device_->capabilities().indexed_geometry;
    const auto draw_batch = [&](const MeshRange& range, core::u32 instances) noexcept {
        return rhi::DrawBatch{range.index_count, instances, indexed ? range.first_index : 0U};
    };
    const auto batch_constants = [&](const MeshRange& range, core::u32 first_instance) noexcept {
        return BatchConstants{range.first_index, first_instance, indexed ? 1U : 0U, 0U};
    };
    for (core::u32 first = 0U; first < opaque_submissions_.size();) {
        const auto& head = opaque_submissions_[first];
        core::u32 end = first + 1U;
        if (scene.renderer_settings.geometry_instancing) {
            while (end < opaque_submissions_.size()) {
                const auto& next = opaque_submissions_[end];
                if (head.asset != next.asset || head.mesh != next.mesh || head.part != next.part
                    || head.range.first_index != next.range.first_index
                    || head.range.index_count != next.range.index_count || head.textures != next.textures
                    || head.samplers != next.samplers) break;
                ++end;
            }
        }
        batches_.push_back(draw_batch(head.range, end - first));
        append(batch_uniforms_, batch_constants(head.range, first));
        const auto texture_begin = batch_textures_.size();
        f.append_material_textures(head.textures, head.samplers);
        stats_.triangles += (head.range.index_count / 3U) * (end - first);
        for (core::u32 cursor = first; cursor < end;) {
            if (opaque_visibility_[cursor] == 0U) { ++cursor; continue; }
            const core::u32 begin = cursor++;
            while (cursor < end && opaque_visibility_[cursor] != 0U) ++cursor;
            camera_batches_.push_back(draw_batch(head.range, cursor - begin));
            append(camera_batch_uniforms_, batch_constants(head.range, begin));
            camera_batch_textures_.insert(camera_batch_textures_.end(),
                batch_textures_.begin() + static_cast<std::ptrdiff_t>(texture_begin), batch_textures_.end());
            if (scene_linear()) camera_coverage_textures_.push_back(batch_textures_[texture_begin]);
            stats_.camera_instances += cursor - begin;
            stats_.camera_triangles += (head.range.index_count / 3U) * (cursor - begin);
            if (cursor - begin > 1U) ++stats_.instanced_batches;
        }
        first = end;
    }
    std::sort(geometry_selection_.begin(), geometry_selection_.end(),
        [](const GeometrySelection& a, const GeometrySelection& b) {
            if (a.model != b.model) return a.model < b.model;
            return a.source < b.source;
        });
    f.indexed = indexed;
    return true;
}

bool SceneRenderer::gather_media(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    auto& options = f.options;
    auto& media_cache = f.media_cache;
    auto& render_view = f.render_view;
    auto& fallback_material_textures = f.fallback_material_textures;
    auto& next_ordinal = f.next_ordinal;
    // A media frame can contain alpha even when its layer opacity is one, so it
    // always joins the compositing queue. That is the only honest treatment
    // without asset metadata guaranteeing every texel is opaque.
    for (const auto& clip : scene.media) {
        if (opacity(clip.style) <= 0.0F) {
            continue;
        }
        if (clip.mesh.primitive == scene::Primitive::Quad
            && media_quad_outside_view(
                clip.world, render_view.matrices.view_projection,
                clip.style.effects, scene.effect_settings)) {
            ++stats_.culled_media;
            continue;
        }
        if (f.held_instances() >= limits_.max_instances) {
            ++stats_.dropped_instances;
            return false;
        }
        const auto key = scene::normalized(clip.mesh);
        const MeshRange* const mesh_range = ensure_resident(key, commands);
        if (mesh_range == nullptr || mesh_range->index_count == 0U) {
            continue;
        }
        // A half-resolution clip decodes as a source of its own; a full one
        // keeps its path and allocates nothing here.
        const std::string half_source =
            clip.resolution == scene::MediaResolution::Half
            ? media_decode_source(clip) : std::string{};
        const std::string& media_source =
            half_source.empty() ? clip.source : half_source;
        ResolvedMediaFrame media_frame = clip.offline || media_source.empty()
            ? ResolvedMediaFrame{} : media_cache.resolve(
            *device_, commands, media_source, clip.frame_index,
            MediaLoopWindow{clip.loop_first, clip.loop_count},
            clip.stream_id(), clip.asset.value());
        if (!media_frame) {
            media_frame = ResolvedMediaFrame{&offline_media_};
        }

        scene::MeshInstance proxy{};
        proxy.mesh = clip.mesh;
        proxy.world = clip.world;
        proxy.normal = clip.normal;
        proxy.source = clip.source_node;
        proxy.style = clip.style;
        if (options.depth_only(clip.source_node)) {
            CompositeDraw draw{};
            draw.range = *mesh_range;
            draw.instance = proxy;
            draw.media = media_frame;
            draw.ordinal = next_ordinal;
            ++next_ordinal;
            depth_only_media_.push_back(draw);
            continue;
        }
        f.queue_composite(
            *mesh_range,
            proxy,
            media_frame,
            fallback_material_textures,
            camera_depth(clip.world, scene::Vec3{}, render_view.eye,
                         render_view.forward),
            clip.source_node,
            clip.compositing_order,
            clip.timeline_lane == doc::NodeId{}
                ? std::optional<Frame::SiblingOrder>{Frame::SiblingOrder{
                      clip.parent_group, clip.group_order}}
                : std::nullopt,
            {},
            primitive_bounds(key.primitive));
    }
    // Clips that are about to cut in. Named after the drawing pass, so a frame
    // the show needs now is always queued ahead of a frame it needs shortly.
    for (const auto& upcoming : scene.media_prewarm) {
        if (upcoming.source.empty()) {
            continue;
        }
        media_cache.prewarm(
            *device_, commands,
            media_decode_source(upcoming.source, upcoming.resolution),
            upcoming.frame_index,
            MediaLoopWindow{upcoming.loop_first, upcoming.loop_count},
            upcoming.stream_id());
    }
    return true;
}

bool SceneRenderer::order_composites(Frame& f) {
    auto& render_view = f.render_view;
    // Back-to-front for colour correctness, then the layer's own order and
    // the collection ordinal for a total order. The latter two make equal-depth
    // layers deterministic even though procedural instances arrived grouped
    // by mesh rather than by document order. Media clips carry their lane and
    // clip order in place of a node id, so clips sharing one surface - equal
    // depth to the bit - stack the way the timeline stacks them, while clips on
    // surfaces at different depths occlude by where they actually are. Depth
    // has to stay the primary key: these draws do not write depth, so drawing
    // order is the only thing deciding which one survives.
    std::sort(
        composite_draws_.begin(),
        composite_draws_.end(),
        [](const CompositeDraw& left, const CompositeDraw& right) noexcept {
            if (left.depth != right.depth) {
                return left.depth > right.depth;
            }
            if (left.sibling_ordered && right.sibling_ordered
                && left.sibling_parent == right.sibling_parent
                && left.sibling_order != right.sibling_order) {
                return left.sibling_order < right.sibling_order;
            }
            if (left.source_order != right.source_order) {
                return left.source_order < right.source_order;
            }
            return left.ordinal < right.ordinal;
        });
    // Centres alone misorder a layer standing in front of a large surface
    // whose centre is nearer the camera than its own.
    order_composites_by_planes(render_view);
    // Depth-only clips are recorded before every destination-reading draw, so
    // their instance slots are claimed first and in document order.
    for (auto& draw : depth_only_media_) {
        draw.first_instance = stats_.instances;
        append(
            instance_scratch_,
            f.instance_record(
                draw.instance, 0U, 0U, true, draw.media.layout));
        stats_.triangles += draw.range.index_count / 3U;
        ++stats_.instances;
        ++stats_.depth_only_instances;
    }
    for (auto& draw : composite_draws_) {
        draw.first_instance = stats_.instances;
        InlineProgramRange effect_range{};
        if (!f.inline_range_for(
                draw.instance.source,
                draw.instance.style.effects,
                effect_range,
                surface_fill_program(draw.instance))) {
            return false;
        }
        draw.first_effect = effect_range.first;
        draw.effect_count = effect_range.count;
        append(
            instance_scratch_,
            f.instance_record(
                draw.instance, draw.first_effect, draw.effect_count, false,
                draw.media.layout,
                !draw.media
                    && draw.material_textures[2U] != &normal_fallback_,
                !draw.media
                    && draw.material_textures[3U] != &material_fallback_,
                !draw.media && draw.material_textures[2U]->desc().format == rhi::TextureFormat::Bc5Unorm,
                !draw.media && draw.material_textures[7U]->desc().format == rhi::TextureFormat::Bc5Unorm));
        stats_.triangles += draw.range.index_count / 3U;
        ++stats_.instances;
        if (draw.media) {
            if (draw.media.primary == &offline_media_) ++stats_.offline_media;
            else ++stats_.media;
        }
    }
    return true;
}

} // namespace mediaserver::render
