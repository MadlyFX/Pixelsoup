// SceneRenderer::render, passes: auxiliary products and occlusion, the lit
// pass, composited layers, particles, coverage, and committing history.

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

bool SceneRenderer::record_auxiliary_passes(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    auto& slice = f.slice;
    auto& options = f.options;
    auto& render_view = f.render_view;
    auto& indexed = f.indexed;
    auto& raster_viewport = f.raster_viewport;
    auto& view_constants = f.view_constants;
    auto& renderer_settings = f.renderer_settings;
    auto& vertex_bindings = f.vertex_bindings;
    auto& batch_indices = f.batch_indices;
    auto& coverage_sampling = f.coverage_sampling;
    if (mode_ == SceneRendererMode::AdvancedLinear && !options.reflection_pass) {
        // Motion of a deforming triangle is solved from all three of its
        // vertices, which the vertex stage finds by its position in the index
        // stream: such a frame draws the motion products pulled.
        const bool pulled_motion = indexed && !animated_instances_.empty();
        if (pulled_motion) {
            camera_pulled_batch_uniforms_.assign(
                camera_batch_uniforms_.begin(), camera_batch_uniforms_.end());
            for (std::size_t offset = 0U; offset < camera_pulled_batch_uniforms_.size();
                 offset += sizeof(BatchConstants)) {
                BatchConstants constants{};
                std::memcpy(&constants, camera_pulled_batch_uniforms_.data() + offset, sizeof(constants));
                constants.indexed = 0U;
                std::memcpy(camera_pulled_batch_uniforms_.data() + offset, &constants, sizeof(constants));
            }
        }
        const auto render_auxiliary =
            [&](rhi::Texture& target,
                const rhi::GraphicsPipeline& auxiliary_pipeline,
                const rhi::GraphicsPipeline& media_auxiliary_pipeline,
                bool object_only = false, bool motion = false,
                std::span<rhi::Texture* const> additional_targets = {}) {
                // A merged pass is timed as Identity, its first product.
                const auto category = &target == &object_id_ ? core::RenderPass::Identity
                    : &target == &object_velocity_ ? core::RenderPass::ObjectVelocity
                    : &target == &normal_ ? core::RenderPass::Normals : core::RenderPass::Velocity;
                ViewConstants auxiliary_view = view_constants;
                if (object_only) {
                    // Both endpoints use this frame's camera and jitter. Only
                    // the retained previous object transform can create motion.
                    std::copy(std::begin(view_constants.canvas_view_projection),
                        std::end(view_constants.canvas_view_projection),
                        std::begin(auxiliary_view.previous_view_projection));
                }
                rhi::PulledDraw auxiliary{};
                auxiliary.pipeline = &auxiliary_pipeline;
                auxiliary.vertex_buffers = vertex_bindings;
                auxiliary.vertex_uniform = bytes_of(auxiliary_view);
        auxiliary.raster_viewport = raster_viewport;
                const bool pulled = motion && pulled_motion;
                auxiliary.batch_uniforms = pulled
                    ? std::span<const core::u8>{camera_pulled_batch_uniforms_}
                    : std::span<const core::u8>{camera_batch_uniforms_};
                auxiliary.batch_uniform_stride =
                    static_cast<core::u32>(sizeof(BatchConstants));
                auxiliary.batches = camera_batches_;
                auxiliary.index_buffer = pulled ? nullptr : batch_indices;
                auxiliary.fragment_uniform = bytes_of(coverage_sampling);
                auxiliary.fragment_textures = camera_coverage_textures_;
                auxiliary.fragment_textures_per_batch = 1U;
                auxiliary.additional_targets = additional_targets;
                // This is the public Advanced depth product after the velocity
                // replay. Preserve it even for a scene with no composites.
                auxiliary.store_depth = true;
                if (!graph_.draw_pulled(commands, category,
                        target, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}, auxiliary_depth_, 0.0F,
                        auxiliary)) {
                    return false;
                }

                // Depth-only media is part of the colour pass's tested
                // visibility but owns no categorical product in this isolated
                // layer. Its media auxiliary shader alpha-tests the sampled
                // coverage, returns zero, and still writes depth where it
                // genuinely occludes later layers.
                for (const auto& occluder : depth_only_media_) {
                    const rhi::DrawBatch batch{
                        occluder.range.index_count, 1U};
                    const BatchConstants offsets{
                        occluder.range.first_index,
                        occluder.first_instance,
                        0U,
                        0U};
                    const std::array<rhi::SampledTexture, 2U> sources{{
                        rhi::SampledTexture{
                            occluder.media.primary, &media_sampler_},
                        rhi::SampledTexture{
                            occluder.media.chroma != nullptr
                                ? occluder.media.chroma
                                : &material_fallback_,
                            &media_sampler_},
                    }};
                    rhi::PulledDraw layer{};
                    layer.pipeline = &media_auxiliary_pipeline;
                    layer.vertex_buffers = vertex_bindings;
                    layer.vertex_uniform = bytes_of(auxiliary_view);
        layer.raster_viewport = raster_viewport;
                    layer.additional_targets = additional_targets;
                    layer.fragment_textures = sources;
                    layer.batch_uniforms = bytes_of(offsets);
                    layer.batch_uniform_stride =
                        static_cast<core::u32>(sizeof(BatchConstants));
                    layer.batches =
                        std::span<const rhi::DrawBatch>{&batch, 1U};
                    layer.load = true;
                    layer.store_depth = true;
                    if (!graph_.draw_pulled(commands, category,
                            target, rhi::Color{}, auxiliary_depth_, 0.0F,
                            layer)) {
                        return false;
                    }
                }
                for (std::size_t index = 0U;
                     index < composite_draws_.size(); ++index) {
                    const auto& composite = composite_draws_[index];
                    const rhi::DrawBatch batch{
                        composite.range.index_count, 1U};
                    const BatchConstants offsets{
                        composite.range.first_index,
                        composite.first_instance,
                        0U,
                        0U};
                    rhi::PulledDraw layer{};
                    layer.pipeline = composite.media
                        ? &media_auxiliary_pipeline
                        : &auxiliary_pipeline;
                    layer.additional_targets = additional_targets;
                    layer.vertex_buffers = vertex_bindings;
                    layer.vertex_uniform = bytes_of(auxiliary_view);
        layer.raster_viewport = raster_viewport;
                    std::array<rhi::SampledTexture, 2U> media_sources{};
                    const rhi::SampledTexture coverage_source{
                        composite.material_textures[0],
                        &material_samplers_[material_sampler_index(composite.samplers[0], renderer_settings.texture_anisotropy)]};
                    if (!composite.media) {
                        layer.fragment_textures = {&coverage_source, 1U};
                        layer.fragment_uniform = bytes_of(coverage_sampling);
                    }
                    if (composite.media) {
                        media_sources = std::array<rhi::SampledTexture, 2U>{{
                            rhi::SampledTexture{
                                composite.media.primary, &media_sampler_},
                            rhi::SampledTexture{
                                composite.media.chroma != nullptr
                                    ? composite.media.chroma
                                    : &material_fallback_,
                                &media_sampler_},
                        }};
                        layer.fragment_textures = media_sources;
                    }
                    layer.batch_uniforms = bytes_of(offsets);
                    layer.batch_uniform_stride =
                        static_cast<core::u32>(sizeof(BatchConstants));
                    layer.batches =
                        std::span<const rhi::DrawBatch>{&batch, 1U};
                    layer.load = true;
                    layer.store_depth = true;
                    if (!graph_.draw_pulled(commands, category,
                            target, rhi::Color{}, auxiliary_depth_, 0.0F,
                            layer)) {
                        return false;
                    }
                }
                return true;
            };
        // Depth, identity, motion and normals, all before colour.
        //
        // They used to run after it, which was fine while nothing in the
        // colour pass read them. Occlusion does: it needs this frame's depth
        // and normals, and the lit pass needs this frame's occlusion. Each
        // call already clears and rewrites the same auxiliary depth, so which
        // one runs first does not change what the depth ends up holding.
        //
        // A device with multiple render targets writes identity, motion and
        // normals in one pass: the same rasterization, coverage and
        // equations, one geometry replay instead of three.
        const bool merged_auxiliary = auxiliary_pipeline_ && media_auxiliary_pipeline_;
        rhi::Texture* const merged_targets[]{&velocity_, &normal_};
        if ((renderer_settings.motion_blur && renderer_settings.motion_blur_radius > 0U
                && !render_auxiliary(object_velocity_, velocity_pipeline_, media_velocity_pipeline_, true, true))
            || (merged_auxiliary
                ? !render_auxiliary(object_id_, auxiliary_pipeline_, media_auxiliary_pipeline_,
                      false, true, merged_targets)
                : (!render_auxiliary(
                        object_id_, object_id_pipeline_, media_object_id_pipeline_)
                    || !render_auxiliary(
                        velocity_, velocity_pipeline_, media_velocity_pipeline_, false, true)
                    || !render_auxiliary(
                        normal_, normal_pipeline_, media_normal_pipeline_)))) {
            return false;
        }
        // Occlusion, from the depth and normals the replay above just wrote.
        //
        // The slice origin travels with it so the sample kernel is rotated by
        // the absolute canvas pixel rather than by a node-local index: two
        // nodes asked about the same wall pixel must rotate identically, or a
        // crease that crosses their seam is sampled two different ways and the
        // seam appears in the occlusion.
        if (occlusion_recorded_) {
            if (!occlusion_->resize(*device_, color_.width(),
                                    color_.height())) {
                return false;
            }
            if (!occlusion_->record(
                    commands, auxiliary_depth_, normal_,
                    scene::invert(render_view.matrices.view_projection), slice,
                    scene.show_frame,
                    scene.renderer_settings.occlusion_radius,
                    scene.renderer_settings.occlusion_strength)) {
                return false;
            }
            if (scene::antialiasing_for(scene.renderer_settings)
                == scene::AntialiasingMode::Temporal) {
                if (!temporal_occlusion_ || !temporal_occlusion_->resize(
                        *device_, color_.width(), color_.height())) return false;
                HistoryDisposition disposition = HistoryDisposition::Initialize;
                switch (options.history) {
                case SceneHistoryAction::Advance: disposition = HistoryDisposition::Advance; break;
                case SceneHistoryAction::Hold: disposition = HistoryDisposition::Hold; break;
                case SceneHistoryAction::Reset: disposition = HistoryDisposition::Reset; break;
                default: break;
                }
                const TemporalResolve::Sources sources{
                    &occlusion_->target(), &velocity_, &object_id_,
                    options.view ? color_.width() : scene.canvas.width,
                    options.view ? color_.height() : scene.canvas.height,
                    options.view
                        ? orthographic_history_depth_tolerance(
                              options.view->matrices.projection,
                              color_.height())
                        : 0.0F};
                auto resolved = temporal_occlusion_->apply(*device_, commands,
                    options.history_slot, disposition, sources);
                if (!resolved) return false;
                // Descriptor arrays already point at this stable AO texture.
                if (resolved.value() != &occlusion_->target()
                    && !graph_.copy(commands, core::RenderPass::Lighting, *resolved.value(),
                        {0U, 0U, color_.width(), color_.height()},
                        occlusion_->target())) return false;
            }
        }
    }
    return true;
}

bool SceneRenderer::record_lit_pass(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    auto& slice = f.slice;
    auto& options = f.options;
    auto& render_view = f.render_view;
    auto& wireframe = f.wireframe;
    auto& raster_viewport = f.raster_viewport;
    auto& view_constants = f.view_constants;
    auto& scene_constants = f.scene_constants;
    auto& vertex_bindings = f.vertex_bindings;
    auto& batch_indices = f.batch_indices;
    auto& lit_fragment_bindings = f.lit_fragment_bindings;
    if (options.reflection_pass) scene_constants.ground_exposure[3] = 1.0F;

    rhi::PulledDraw draw{};
    draw.pipeline = wireframe ? &wireframe_pipeline_ : &pipeline_;
    draw.vertex_buffers = vertex_bindings;
    draw.fragment_buffers = lit_fragment_bindings;
    draw.vertex_uniform = bytes_of(view_constants);
        draw.raster_viewport = raster_viewport;
    draw.fragment_uniform = bytes_of(scene_constants);
    draw.batch_uniforms = camera_batch_uniforms_;
    draw.batch_uniform_stride = static_cast<core::u32>(sizeof(BatchConstants));
    draw.batches = camera_batches_;
    draw.index_buffer = batch_indices;
    draw.fragment_textures = camera_batch_textures_;
    draw.fragment_textures_per_batch =
        mode_ == SceneRendererMode::AdvancedLinear
        ? advanced_fragment_textures
        : basic_fragment_textures;
    draw.store_depth =
        !composite_draws_.empty() || !depth_only_media_.empty() || draws_particles();

    // The ordinary clear is the sky, so an empty scene still reads as a frame.
    // Isolated compositor layers instead retain transparent uncovered pixels.
    // Depth clears to zero because the projection is reversed.
    const rhi::Color clear = options.transparent_background
        ? rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}
        : rhi::Color{
              scene.environment.sky_color.x
                  * scene_constants.ground_exposure[3U],
              scene.environment.sky_color.y
                  * scene_constants.ground_exposure[3U],
              scene.environment.sky_color.z
                  * scene_constants.ground_exposure[3U],
              1.0F};
    // The environment as the background, before anything is drawn over it.
    //
    // It owns the clear when it runs: the pass clears colour and depth as any
    // first pass does, fills every pixel with the environment, and the lit pass
    // then loads instead of clearing. A scene with no map skips all of this and
    // the lit pass clears exactly as it always did, which is what keeps the
    // no-environment output bit for bit unchanged.
    // An isolated compositor layer must stay transparent where it drew
    // nothing, so it gets no sky: filling one with a background makes every
    // layer opaque and the composite shows only whichever happened to be last.
    // The environment still lights that layer - only the background is
    // withheld, and the compositor owns its own.
    const bool draw_sky =
        !options.transparent_background && background_resident(scene);
    if (draw_sky
        && !record_sky(commands, color_, clear, render_view, scene,
                       scene_constants.ground_exposure[3U], slice, options.view.has_value() && !options.full_canvas_view)) {
        return false;
    }
    draw.load = draw_sky;
    if (!graph_.draw_pulled(commands, core::RenderPass::Lighting, color_, clear, depth_, 0.0F, draw)) {
        return false;
    }
    f.clear = clear;
    return true;
}

bool SceneRenderer::record_composites(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    auto& wireframe = f.wireframe;
    auto& raster_viewport = f.raster_viewport;
    auto& view_constants = f.view_constants;
    auto& scene_constants = f.scene_constants;
    auto& vertex_bindings = f.vertex_bindings;
    auto& lit_fragment_bindings = f.lit_fragment_bindings;
    auto& media_fragment_bindings = f.media_fragment_bindings;
    auto& clear = f.clear;
    // Clips that occlude this pass, one depth-writing alpha-tested pass each.
    // They run before any composited layer so every later draw is tested
    // against the complete depth of the frame rather than part of it.
    for (std::size_t index = 0U; index < depth_only_media_.size(); ++index) {
        const auto& occluder = depth_only_media_[index];
        const rhi::DrawBatch occluder_batch{occluder.range.index_count, 1U};
        const BatchConstants occluder_offsets{
            occluder.range.first_index, occluder.first_instance, 0U, 0U};
        rhi::PulledDraw occluder_draw{};
        occluder_draw.pipeline =
            wireframe ? &media_wireframe_pipeline_ : &media_depth_pipeline_;
        occluder_draw.vertex_buffers = vertex_bindings;
        occluder_draw.vertex_uniform = bytes_of(view_constants);
        occluder_draw.raster_viewport = raster_viewport;
        const std::array<rhi::SampledTexture, 2U> occluder_sources{{
            rhi::SampledTexture{occluder.media.primary, &media_sampler_},
            rhi::SampledTexture{
                occluder.media.chroma != nullptr
                    ? occluder.media.chroma
                    : &material_fallback_,
                &media_sampler_},
        }};
        occluder_draw.fragment_textures = occluder_sources;
        occluder_draw.fragment_buffers = media_fragment_bindings;
        occluder_draw.fragment_uniform = bytes_of(scene_constants);
        occluder_draw.batch_uniforms = bytes_of(occluder_offsets);
        occluder_draw.batch_uniform_stride =
            static_cast<core::u32>(sizeof(BatchConstants));
        occluder_draw.batches =
            std::span<const rhi::DrawBatch>{&occluder_batch, 1U};
        occluder_draw.load = true;
        occluder_draw.store_depth = index + 1U < depth_only_media_.size() ||
            !composite_draws_.empty() || draws_particles();
        if (!graph_.draw_pulled(commands, core::RenderPass::Media,
                color_, clear, depth_, 0.0F, occluder_draw)) {
            return false;
        }
    }

    const bool needs_transmission = std::any_of(composite_draws_.begin(), composite_draws_.end(),
        [](const auto& item) { return !item.media && item.instance.material.transmission > 0.0F; });
    if (needs_transmission && !graph_.copy(commands, core::RenderPass::Lighting, color_,
        {0U, 0U, color_.width(), color_.height()}, transmission_background_)) return false;

    // Destination-reading layers are intentionally one batch per pass. That
    // preserves their sorted order while allowing every layer to choose its
    // own fixed-function blend equation and, for media, its own texture.
    // `depth_pass` records the layer instead into depth alone, for its fully
    // opaque pixels.
    const auto record_composite = [&](const CompositeDraw& composite,
                                      bool depth_pass, bool store_depth) {
        const rhi::DrawBatch composite_batch{
            composite.range.index_count, 1U};
        const BatchConstants composite_offsets{
            composite.range.first_index,
            composite.first_instance,
            0U,
            depth_pass ? 1U : 0U};
        rhi::PulledDraw composite_draw{};
        const auto pipeline_index = blend_index(composite.blend);
        composite_draw.pipeline = depth_pass
            ? (composite.media ? &media_depth_pipeline_
                               : &blended_depth_pipeline_)
            : wireframe
            ? (composite.media ? &media_wireframe_pipeline_
                               : &wireframe_pipeline_)
            : composite.media
            ? &media_blended_pipelines_[pipeline_index]
            : &blended_pipelines_[pipeline_index];
        composite_draw.vertex_buffers = vertex_bindings;
        composite_draw.vertex_uniform = bytes_of(view_constants);
        composite_draw.raster_viewport = raster_viewport;
        // The same set the lit pass binds, because this uses the same
        // pipeline. A composited layer that bound fewer would be a different
        // count than its own root signature declares - which is a refused draw
        // with no explanation, and is exactly what happened the last time a
        // binding was added and this array was missed.
        constexpr std::size_t material_binding_count =
            advanced_fragment_textures;
        std::array<rhi::SampledTexture, material_binding_count>
            material_bindings{};
        std::array<rhi::SampledTexture, 2U> media_bindings{};
        if (!composite.media) {
            bind_material_textures(material_bindings, composite.material_textures,
                composite.samplers, scene.renderer_settings.texture_anisotropy,
                composite.instance.material.transmission > 0.0F
                    ? transmission_background_ : material_fallback_);
            composite_draw.fragment_textures = scene_linear()
                ? std::span<const rhi::SampledTexture>{material_bindings}
                : std::span<const rhi::SampledTexture>{
                      material_bindings.data(), basic_fragment_textures};
            composite_draw.fragment_buffers = lit_fragment_bindings;
            composite_draw.fragment_uniform = bytes_of(scene_constants);
        } else {
            media_bindings = std::array<rhi::SampledTexture, 2U>{{
                rhi::SampledTexture{
                    composite.media.primary, &media_sampler_},
                rhi::SampledTexture{
                    composite.media.chroma != nullptr
                        ? composite.media.chroma
                        : &material_fallback_,
                    &media_sampler_},
            }};
            composite_draw.fragment_textures = media_bindings;
            composite_draw.fragment_buffers = media_fragment_bindings;
            composite_draw.fragment_uniform = bytes_of(scene_constants);
        }
        composite_draw.batch_uniforms = bytes_of(composite_offsets);
        composite_draw.batch_uniform_stride =
            static_cast<core::u32>(sizeof(BatchConstants));
        composite_draw.batches =
            std::span<const rhi::DrawBatch>{&composite_batch, 1U};
        composite_draw.load = true;
        composite_draw.store_depth = store_depth;
        return graph_.draw_pulled(commands, core::RenderPass::Media,
            color_, clear, depth_, 0.0F, composite_draw);
    };
    // Layers write no depth when composited, so two that cross would each be
    // drawn whole over the other in sorted order. First, every layer that a
    // later one could pass behind writes the depth of its fully opaque pixels,
    // so the later one is hidden there. Only Normal layers hide what is
    // behind them; the last layer has nothing after it to hide; a layer
    // below full opacity has no opaque pixels; and wireframe draws edges.
    for (std::size_t index = 0U;
         !wireframe && index + 1U < composite_draws_.size(); ++index) {
        const auto& composite = composite_draws_[index];
        if (composite.blend != scene::BlendMode::Normal
            || opacity(composite.instance.style) < 1.0F) {
            continue;
        }
        if (!record_composite(composite, true, true)) return false;
    }
    for (std::size_t index = 0U; index < composite_draws_.size(); ++index) {
        if (!record_composite(composite_draws_[index], false,
                index + 1U < composite_draws_.size() || draws_particles())) {
            return false;
        }
    }
    return true;
}

bool SceneRenderer::record_particles_and_coverage(Frame& f) {
    auto& commands = f.commands;
    auto& scene = f.scene;
    auto& options = f.options;
    auto& render_view = f.render_view;
    auto& view_history = *f.view_history;
    auto& motion_view_projection = f.motion_view_projection;
    auto& advance_history = f.advance_history;
    auto& raster_viewport = f.raster_viewport;
    auto& view_constants = f.view_constants;
    auto& particle_lighting = f.particle_lighting;
    ParticleTargets particle_targets{};
    particle_targets.raster_viewport = raster_viewport;
    particle_targets.lighting = &particle_lighting;
    // The velocity target holds this frame's motion only where the auxiliary
    // pass ran: Advanced, and not a reflection capture.
    const bool scene_motion = mode_ == SceneRendererMode::AdvancedLinear && !options.reflection_pass;
    if (scene_motion) {
        particle_targets.scene_depth = &velocity_;
    }
    if (scene_motion && scene::antialiasing_for(scene.renderer_settings) == scene::AntialiasingMode::Temporal) {
        // Sprite motion for the temporal resolve, between the same two
        // endpoints as the scene's own motion: the previous frame's canvas
        // matrix, and where particles were then by their velocity.
        particle_targets.velocity = &velocity_;
        particle_targets.canvas_view_projection = render_view.canvas_view_projection;
        particle_targets.previous_view_projection = motion_view_projection;
        particle_targets.motion_valid = view_constants.temporal_parameters[0U] > 0.5F;
        const core::u64 previous_frame = advance_history ? view_history.previous_show_frame
                                                         : view_history.motion_transform_show_frame;
        if (particle_targets.motion_valid && scene.show_frame > previous_frame && scene.show_rate.numerator != 0U) {
            particle_targets.motion_seconds =
                static_cast<float>(static_cast<double>(scene.show_frame - previous_frame) *
                                   scene.show_rate.denominator / scene.show_rate.numerator);
        }
    }
    if (!record_particles(commands,
            raster_viewport.empty()
                ? render_view.matrices
                : scene::ViewMatrices{render_view.matrices.view, render_view.matrices.projection,
                                      render_view.canvas_view_projection},
            particle_targets)) {
        return false;
    }

    if (mode_ == SceneRendererMode::AdvancedLinear) {
        // Coverage samples the colour target, so this one
        // genuinely has to follow the colour pass.
        const std::array<rhi::SampledTexture, 1U> coverage_sources{{
            rhi::SampledTexture{&color_, &coverage_sampler_},
        }};
        if (!graph_.fullscreen(commands, core::RenderPass::Coverage,
                coverage_, rhi::Color{}, coverage_pipeline_, coverage_sources,
                {}, false)) {
            return false;
        }
    }
    return true;
}

bool SceneRenderer::commit_history(Frame& f) {
    auto& scene = f.scene;
    auto& render_view = f.render_view;
    auto& view_history = *f.view_history;
    auto& advance_history = f.advance_history;
    auto& hold_history = f.hold_history;
    if (!hold_history) {
        const bool same_global_frame = history_valid_
            && previous_scene_ == scene.id
            && previous_show_frame_ == scene.show_frame;
        const bool same_global_transforms = same_global_frame
            && previous_transforms_ == next_transforms_;
        if (!same_global_frame) {
            const bool sequential_global_frame = history_valid_
                && previous_scene_ == scene.id
                && previous_show_frame_
                    != std::numeric_limits<core::u64>::max()
                && scene.show_frame == previous_show_frame_ + 1U;
            if (sequential_global_frame) {
                // Preserve N while committing N+1. A later mapped view for
                // N+1 still needs N even though another view recorded first.
                motion_source_transforms_.swap(previous_transforms_);
                motion_source_show_frame_ = previous_show_frame_;
                previous_transforms_.swap(next_transforms_);
            } else {
                // A discontinuity has no valid motion source. Current==prior
                // keeps Initialize/Reset velocity exactly zero and retryable.
                previous_transforms_.swap(next_transforms_);
                motion_source_transforms_.assign(
                    previous_transforms_.begin(), previous_transforms_.end());
                motion_source_show_frame_ = scene.show_frame;
            }
            previous_scene_ = scene.id;
            previous_show_frame_ = scene.show_frame;
            history_valid_ = true;
        } else if (!same_global_transforms) {
            // A same-frame visual edit is a reset. Replace both retained
            // generations so no view can accidentally reuse pre-edit motion.
            previous_transforms_.swap(next_transforms_);
            motion_source_transforms_.assign(
                previous_transforms_.begin(), previous_transforms_.end());
            motion_source_show_frame_ = scene.show_frame;
        }

        if (advance_history) {
            view_history.motion_source_view_projection =
                view_history.previous_view_projection;
            view_history.motion_transform_show_frame =
                view_history.previous_show_frame;
        } else {
            view_history.motion_source_view_projection =
                render_view.canvas_view_projection;
            view_history.motion_transform_show_frame = scene.show_frame;
        }
        // The canvas-wide matrix, because this is what the *next* frame reads
        // as its previous endpoint and both endpoints have to live in the same
        // node-independent space.
        view_history.previous_view_projection =
            render_view.canvas_view_projection;
        view_history.previous_scene = scene.id;
        view_history.previous_show_frame = scene.show_frame;
        view_history.previous_show_rate = scene.show_rate;
        view_history.valid = true;
    }
    return true;
}

} // namespace mediaserver::render
