// SceneRenderer construction: the pipelines and targets it creates, and
// resizing them.

#include "scene_renderer_internal.hpp"

namespace mediaserver::render {

using namespace scene_detail;

bool SceneRenderer::shaders_available() noexcept {
    return shader_blobs().present;
}

void SceneRenderer::order_composites_by_planes(
    const SceneRenderView& view) noexcept {
    const auto count = composite_draws_.size();
    if (count < 2U) return;
    const bool orthographic = scene::orthographic(view.matrices.projection);
    const auto forward =
        scene::normalize(view.forward, scene::Vec3{0.0F, 0.0F, -1.0F});

    layer_volumes_.clear();
    for (const auto& draw : composite_draws_) {
        LayerVolume volume{};
        volume.nearest = std::numeric_limits<float>::infinity();
        volume.farthest = -std::numeric_limits<float>::infinity();
        const auto& bounds = draw.bounds;
        for (std::size_t corner = 0U; corner < volume.corners.size(); ++corner) {
            const scene::Vec3 local{
                (corner & 1U) != 0U ? bounds.maximum.x : bounds.minimum.x,
                (corner & 2U) != 0U ? bounds.maximum.y : bounds.minimum.y,
                (corner & 4U) != 0U ? bounds.maximum.z : bounds.minimum.z};
            volume.corners[corner] =
                scene::transform_point(draw.instance.world, local);
            const float depth =
                scene::dot(volume.corners[corner] - view.eye, forward);
            volume.nearest = std::min(volume.nearest, depth);
            volume.farthest = std::max(volume.farthest, depth);
        }
        // No thickness along one local axis: the surface lies in the plane
        // through the corners spanned by the other two.
        for (std::size_t axis = 0U; axis < 3U; ++axis) {
            if (component(bounds.maximum, axis) != component(bounds.minimum, axis)) {
                continue;
            }
            const std::size_t u = std::size_t{1} << ((axis + 1U) % 3U);
            const std::size_t v = std::size_t{1} << ((axis + 2U) % 3U);
            const auto origin = volume.corners[0U];
            const auto normal = scene::cross(
                volume.corners[u] - origin, volume.corners[v] - origin);
            const float area = scene::length(normal);
            if (!(area > 1.0e-12F)) break;
            volume.normal = normal * (1.0F / area);
            volume.offset = scene::dot(volume.normal, origin);
            volume.viewer_side = orthographic
                ? -scene::dot(volume.normal, forward)
                : scene::dot(volume.normal, view.eye) - volume.offset;
            // Seen edge on, a surface has no side the viewer is on.
            volume.planar = std::isfinite(volume.viewer_side)
                && std::abs(volume.viewer_side) > 1.0e-6F;
            break;
        }
        layer_volumes_.push_back(volume);
    }

    // Whether every corner of `points` is on one side of `plane`: +1 the
    // viewer's side, -1 the far side. Points within the tolerance count as
    // on the plane, so coplanar layers are on neither side and keep their
    // composite order.
    const auto wholly = [](const LayerVolume& plane,
                           const LayerVolume& points, float side) {
        const float tolerance = 1.0e-4F + 1.0e-5F * std::abs(plane.offset);
        const float facing = plane.viewer_side > 0.0F ? side : -side;
        bool off_plane = false;
        for (const auto& corner : points.corners) {
            const float distance =
                (scene::dot(plane.normal, corner) - plane.offset) * facing;
            if (!(distance >= -tolerance)) return false;
            off_plane = off_plane || distance > tolerance;
        }
        return off_plane;
    };
    // Whether `under` has to be drawn before `over`.
    const auto beneath = [&](const LayerVolume& under, const LayerVolume& over) {
        // Apart in depth, the centre order already draws the farther first.
        if (over.farthest < under.nearest || under.farthest < over.nearest) {
            return false;
        }
        return (under.planar && wholly(under, over, 1.0F))
            || (over.planar && wholly(over, under, -1.0F));
    };

    // Each flat layer against every other layer, a pair of flat ones once,
    // within a fixed budget of pairs and relations per recording.
    layer_edges_.clear();
    std::size_t pairs_left = layer_pair_budget;
    for (std::size_t first = 0U; first < count; ++first) {
        if (!layer_volumes_[first].planar) continue;
        if (pairs_left < count || layer_edges_.size() + count
                > layer_edges_.capacity()) {
            // Out of budget: this flat layer and the ones after it keep the
            // centre order against everything.
            for (std::size_t rest = first; rest < count; ++rest) {
                if (layer_volumes_[rest].planar) ++stats_.unordered_layers;
            }
            break;
        }
        pairs_left -= count;
        const auto& a = layer_volumes_[first];
        for (std::size_t second = 0U; second < count; ++second) {
            const auto& b = layer_volumes_[second];
            if (second == first || (b.planar && second < first)) continue;
            const bool a_under = beneath(a, b);
            // Both at once only for two that cross: leave those to the sort.
            if (a_under == beneath(b, a)) continue;
            const auto from = static_cast<core::u32>(a_under ? first : second);
            const auto to = static_cast<core::u32>(a_under ? second : first);
            layer_edges_.emplace_back(from, to);
        }
    }
    if (layer_edges_.empty()) return;

    // Kahn's algorithm, always taking the earliest ready layer in the sorted
    // order, so layers with no relation keep exactly the order they had.
    std::sort(layer_edges_.begin(), layer_edges_.end());
    layer_edge_offsets_.assign(count + 1U, 0U);
    layer_in_degree_.assign(count, 0U);
    for (const auto& [from, to] : layer_edges_) {
        ++layer_edge_offsets_[from + 1U];
        ++layer_in_degree_[to];
    }
    for (std::size_t index = 0U; index < count; ++index) {
        layer_edge_offsets_[index + 1U] += layer_edge_offsets_[index];
    }
    layer_placed_.assign(count, 0U);
    layer_ready_.clear();
    layer_order_.clear();
    const auto earliest_first = std::greater<core::u32>{};
    const auto make_ready = [&](core::u32 index) {
        layer_ready_.push_back(index);
        std::push_heap(layer_ready_.begin(), layer_ready_.end(), earliest_first);
    };
    for (std::size_t index = 0U; index < count; ++index) {
        if (layer_in_degree_[index] == 0U) {
            make_ready(static_cast<core::u32>(index));
        }
    }
    std::size_t unplaced = 0U;
    while (layer_order_.size() < count) {
        if (layer_ready_.empty()) {
            // A cycle: release the earliest layer still waiting on it.
            while (layer_placed_[unplaced] != 0U) ++unplaced;
            make_ready(static_cast<core::u32>(unplaced));
        }
        std::pop_heap(layer_ready_.begin(), layer_ready_.end(), earliest_first);
        const auto next = layer_ready_.back();
        layer_ready_.pop_back();
        if (layer_placed_[next] != 0U) continue;
        layer_placed_[next] = 1U;
        layer_order_.push_back(next);
        for (auto edge = layer_edge_offsets_[next];
             edge < layer_edge_offsets_[next + 1U]; ++edge) {
            const auto to = layer_edges_[edge].second;
            if (layer_placed_[to] == 0U && layer_in_degree_[to] > 0U
                && --layer_in_degree_[to] == 0U) {
                make_ready(to);
            }
        }
    }

    // Apply the order in place, one permutation cycle at a time.
    for (std::size_t start = 0U; start < count; ++start) {
        if (layer_order_[start] == start) continue;
        CompositeDraw held = std::move(composite_draws_[start]);
        std::size_t at = start;
        for (;;) {
            const std::size_t from = layer_order_[at];
            layer_order_[at] = static_cast<core::u32>(at);
            if (from == start) {
                composite_draws_[at] = std::move(held);
                break;
            }
            composite_draws_[at] = std::move(composite_draws_[from]);
            at = from;
        }
    }
}

void SceneRenderer::bind_material_textures(std::span<rhi::SampledTexture> into,
    const std::array<const rhi::Texture*, 14U>& textures,
    const std::array<ModelSampler, 14U>& samplers, core::u32 anisotropy,
    const rhi::Texture& background) noexcept {
    for (std::size_t i = 0U; i < textures.size(); ++i)
        into[i] = {textures[i], &material_samplers_[material_sampler_index(samplers[i], anisotropy)]};
    if (!scene_linear()) {
        into[14] = {&background, &media_sampler_};
        into[15] = {&ies_atlas_, &media_sampler_};
        return;
    }
    // Fourteen material samplers plus linear-clamp and nearest-clamp fit s0-s15.
    // Other advanced textures share the appropriate clamp sampler in HLSL.
    const EnvironmentMaps maps = environment_ ? environment_->maps() : EnvironmentMaps{};
    into[14] = {maps.specular ? maps.specular : &environment_cube_fallback_, &environment_sampler_};
    bind_shadow_products(into);
    into[21] = {maps.irradiance ? maps.irradiance : &environment_fallback_, &media_sampler_};
    into[22] = {environment_ && environment_->brdf() ? environment_->brdf() : &environment_fallback_, &environment_sampler_};
    into[23] = {occlusion_recorded_ && occlusion_ ? &occlusion_->target() : &material_fallback_, &media_sampler_};
    into[25] = {&background, &media_sampler_};
    into[26] = {&auxiliary_depth_, &shadow_sampler_};
    into[27] = {&ies_atlas_, &shadow_sampler_};
    for (std::size_t i = 0; i < reflection_filters_.size(); ++i)
        into[28 + i] = {active_reflections_[i].enabled ? reflection_filters_[i]->maps().specular : &environment_cube_fallback_, &environment_sampler_};
    into[32] = {planar_texture_ ? planar_texture_ : &material_fallback_, &media_sampler_};

}

void SceneRenderer::bind_shadow_products(std::span<rhi::SampledTexture> into) const noexcept {
    // Lanes past the cascades a frame draws are never read, but are bound.
    for (std::size_t i = 0U; i < scene::max_shadow_cascades; ++i) {
        const auto& product = cascades_[i] ? cascades_[i]
            : cascade_homes_[std::min(i, cascade_homes_.size() - 1U)];
        into[15U + i] = {&product->textures[0U], &shadow_sampler_};
    }
    const auto& rsm = rsm_ ? rsm_ : rsm_home_;
    into[19] = {&rsm->textures[0U], &shadow_sampler_};
    into[20] = {&rsm->textures[1U], &shadow_sampler_};
    into[24] = {&(atlas_ ? atlas_ : atlas_home_)->textures[0U], &shadow_sampler_};
}

SceneRenderer::SceneRenderer(
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
    rhi::Sampler coverage_sampler) noexcept
    : device_(&device),
      limits_(limits),
      color_(std::move(color)),
      depth_(std::move(depth)),
      pipeline_(std::move(pipeline)),
      blended_pipelines_(std::move(blended_pipelines)),
      media_blended_pipelines_(std::move(media_blended_pipelines)),
      media_depth_pipeline_(std::move(media_depth_pipeline)),
      blended_depth_pipeline_(std::move(blended_depth_pipeline)),
      media_sampler_(std::move(media_sampler)),
      material_fallback_(std::move(material_fallback)),
      normal_fallback_(std::move(normal_fallback)),
      geometry_(std::move(geometry)),
      instance_buffer_(std::move(instances)),
      light_buffer_(std::move(lights)),
      inline_effect_op_buffer_(std::move(inline_effect_ops)),
      inline_effect_parameter_buffer_(std::move(inline_effect_parameters)),
      mode_(mode), velocity_(std::move(velocity)),
      object_id_(std::move(object_id)), normal_(std::move(normal)),
      coverage_(std::move(coverage)),
      auxiliary_depth_(std::move(auxiliary_depth)),
      velocity_pipeline_(std::move(velocity_pipeline)),
      object_id_pipeline_(std::move(object_id_pipeline)),
      normal_pipeline_(std::move(normal_pipeline)),
      media_normal_pipeline_(std::move(media_normal_pipeline)),
      media_velocity_pipeline_(std::move(media_velocity_pipeline)),
      media_object_id_pipeline_(std::move(media_object_id_pipeline)),
      coverage_pipeline_(std::move(coverage_pipeline)),
      coverage_sampler_(std::move(coverage_sampler)) {}

Result SceneRenderer::create(
    rhi::Device& device,
    core::u32 width,
    core::u32 height,
    const RendererLimits& limits,
    SceneRendererMode mode) {
    const auto blobs = shader_blobs();
    if (!blobs.present) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }
    if (width == 0U || height == 0U || limits.max_instances == 0U
        || limits.max_history_slots == 0U
        || limits.max_model_refresh_generations == 0U
        || limits.max_model_textures == 0U
        || limits.max_model_texture_bytes == 0U
        || limits.max_vertices == 0U || limits.max_indices == 0U
        || limits.max_skin_vertices == 0U || limits.geometry_upload_bytes == 0U
        || limits.texture_upload_bytes == 0U
        || limits.max_lights == 0U || limits.max_inline_effect_ops == 0U
        || limits.directional_shadow_resolution == 0U
        || limits.directional_shadow_resolution > 4'096U
        || limits.max_directional_shadow_cascades == 0U
        || limits.max_directional_shadow_cascades > scene::max_shadow_cascades
        || limits.max_inline_curve_luts == 0U
        || limits.max_inline_effect_parameter_vectors == 0U) {
        return Result::failure(RenderError::InvalidConfiguration);
    }
    if (!device.supports(blobs.format)) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }
    const bool advanced = mode == SceneRendererMode::AdvancedLinear;
    if (advanced && !device.capabilities().advanced_raster()) {
        return Result::failure(RenderError::CapabilityUnsupported);
    }
    const auto color_format = advanced ? rhi::TextureFormat::Rgba16Float
                                       : rhi::TextureFormat::Rgba8Unorm;

    auto color = device.create_texture(color_desc(width, height, color_format));
    auto transmission_background = device.create_texture(color_desc(width, height, color_format));
    if (!transmission_background) return Result::failure(RenderError::AllocationFailed);
    if (!color) {
        return Result::failure(RenderError::AllocationFailed);
    }
    auto depth = device.create_texture(depth_desc(width, height));
    if (!depth) {
        return Result::failure(RenderError::AllocationFailed);
    }

    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = blobs.format;
    vertex.bytecode = blobs.vertex;
    // Vertices, indices, instances, and the parallel tangent stream; two
    // uniform slots, the view and the per-batch offsets.
    vertex.storage_buffer_count = 6U;
    vertex.uniform_buffer_count = 2U;

    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = blobs.format;
    fragment.bytecode = advanced ? blobs.linear_fragment : blobs.fragment;
    // Five material maps, then the fixed cascade set and the two reflective-
    // shadow-map attributes. The cascade slots are declared whether or not a
    // show authored that many, so one pipeline serves every authored count and
    // the binding shape never depends on document state.
    // ... and then the three image-based-lighting inputs, declared whether or
    // not a show authored an environment for the same reason the cascade slots
    // are: one pipeline serves every authored state, and the binding shape
    // never depends on document content.
    fragment.sampler_count =
        advanced ? 16U : basic_fragment_textures;
    fragment.sampled_texture_count = advanced ? advanced_fragment_textures : basic_fragment_textures;
    fragment.storage_buffer_count = advanced ? 7U : 4U;
    fragment.uniform_buffer_count = 1U;

    rhi::DepthState depth_state{};
    depth_state.enabled = true;
    depth_state.write = true;
    // Reversed depth: the scene layer's projection puts the near plane at one.
    depth_state.greater = true;

    auto pipeline = device.create_pipeline(
        vertex,
        fragment,
        color_format,
        rhi::ColorBlend::Disabled,
        depth_state);
    if (!pipeline) {
        return Result::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }

    // Media uses the same pulled vertex stage and binds two source slots. RGBA
    // clips use the first and ignore the fallback second slot; planar video
    // binds luma and chroma directly and converts in this fragment stage. It always
    // goes through a compositing pipeline because source pixels may have alpha.
    rhi::ShaderDesc media_fragment{};
    media_fragment.stage = rhi::ShaderStage::Fragment;
    media_fragment.format = blobs.format;
    media_fragment.bytecode = advanced ? blobs.media_linear_fragment
                                       : blobs.media_fragment;
    media_fragment.sampler_count = 2U;
    media_fragment.storage_buffer_count = 2U;
    // The media stage reads the same scene constants as the lit stage, for the
    // one field both have to agree about: the project's brightness curve.
    media_fragment.uniform_buffer_count = 1U;

    rhi::DepthState composite_depth = depth_state;
    composite_depth.write = false;
    constexpr std::array<scene::BlendMode, 4U> blend_modes{
        scene::BlendMode::Normal,
        scene::BlendMode::Add,
        scene::BlendMode::Multiply,
        scene::BlendMode::Screen};
    std::array<rhi::GraphicsPipeline, 4U> blended_pipelines{};
    std::array<rhi::GraphicsPipeline, 4U> media_blended_pipelines{};
    for (std::size_t index = 0U; index < blend_modes.size(); ++index) {
        auto blended = device.create_pipeline(
            vertex,
            fragment,
            color_format,
            to_rhi_blend(blend_modes[index], advanced),
            composite_depth);
        if (!blended) {
            return Result::failure(
                blended.error() == rhi::RhiError::ShaderFormatUnsupported
                    ? RenderError::ShaderFormatUnsupported
                    : RenderError::BackendFailure);
        }
        blended_pipelines[index] = std::move(blended).value();

        auto media_blended = device.create_pipeline(
            vertex,
            media_fragment,
            color_format,
            to_rhi_blend(blend_modes[index], advanced),
            composite_depth);
        if (!media_blended) {
            return Result::failure(
                media_blended.error() == rhi::RhiError::ShaderFormatUnsupported
                    ? RenderError::ShaderFormatUnsupported
                    : RenderError::BackendFailure);
        }
        media_blended_pipelines[index] = std::move(media_blended).value();
    }
    // A clip that occludes an isolated layer needs depth without colour. The
    // ordinary source-over factors leave the destination untouched at the zero
    // alpha the depth-only branch of the media stage returns, so this is the
    // blended pipeline with depth writes turned back on.
    auto media_depth_pipeline = device.create_pipeline(
        vertex,
        media_fragment,
        color_format,
        advanced ? rhi::ColorBlend::PremultipliedNormal
                 : rhi::ColorBlend::Normal,
        depth_state);
    if (!media_depth_pipeline) {
        return Result::failure(
            media_depth_pipeline.error() ==
                    rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }
    // The lit counterpart, for the depth pass a blended surface makes of its
    // own opaque pixels before the layers are composited.
    auto blended_depth_pipeline = device.create_pipeline(
        vertex,
        fragment,
        color_format,
        advanced ? rhi::ColorBlend::PremultipliedNormal
                 : rhi::ColorBlend::Normal,
        depth_state);
    if (!blended_depth_pipeline) {
        return Result::failure(
            blended_depth_pipeline.error() ==
                    rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }

    rhi::Texture velocity;
    rhi::Texture object_velocity;
    rhi::Texture object_id;
    rhi::Texture normal;
    rhi::Texture coverage;
    rhi::Texture auxiliary_depth;
    rhi::GraphicsPipeline velocity_pipeline;
    rhi::GraphicsPipeline normal_pipeline;
    rhi::GraphicsPipeline media_normal_pipeline;
    rhi::GraphicsPipeline object_id_pipeline;
    rhi::GraphicsPipeline media_velocity_pipeline;
    rhi::GraphicsPipeline media_object_id_pipeline;
    rhi::GraphicsPipeline coverage_pipeline;
    rhi::Sampler coverage_sampler;
    std::vector<rhi::Texture> directional_shadow;
    rhi::Texture shadow_depth;
    rhi::GraphicsPipeline shadow_pipeline;
    rhi::Sampler shadow_sampler;
    rhi::Texture rsm_normal;
    rhi::Texture rsm_flux;
    rhi::GraphicsPipeline rsm_normal_pipeline;
    rhi::GraphicsPipeline auxiliary_pipeline;
    rhi::GraphicsPipeline media_auxiliary_pipeline;
    rhi::GraphicsPipeline rsm_flux_pipeline;
    if (advanced) {
        auto created_velocity = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::Rgba32Float));
        auto created_object_velocity = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::Rgba32Float));
        if (!created_object_velocity) return Result::failure(RenderError::AllocationFailed);
        object_velocity = std::move(created_object_velocity).value();
        auto created_object_id = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::R32Uint));
        auto created_normal = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::Rg16Float));
        auto created_coverage = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::R32Float));
        auto created_auxiliary_depth =
            device.create_texture(depth_desc(width, height, true));
        // One map per allocated cascade. They are created here, once, so an
        // authored cascade change is a different draw count over resources
        // that already exist rather than an allocation during recording.
        bool cascades_created = true;
        try {
            directional_shadow.reserve(
                limits.max_directional_shadow_cascades);
        } catch (const std::bad_alloc&) {
            return Result::failure(RenderError::AllocationFailed);
        }
        for (core::u32 cascade = 0U;
             cascade < limits.max_directional_shadow_cascades; ++cascade) {
            auto created_cascade = device.create_texture(color_desc(
                limits.directional_shadow_resolution,
                limits.directional_shadow_resolution,
                rhi::TextureFormat::R32Float));
            if (!created_cascade) {
                cascades_created = false;
                break;
            }
            directional_shadow.push_back(std::move(created_cascade).value());
        }
        auto created_shadow_depth = device.create_texture(depth_desc(
            limits.directional_shadow_resolution,
            limits.directional_shadow_resolution));
        auto created_rsm_normal = device.create_texture(color_desc(
            limits.directional_shadow_resolution,
            limits.directional_shadow_resolution,
            rhi::TextureFormat::Rgba16Float));
        auto created_rsm_flux = device.create_texture(color_desc(
            limits.directional_shadow_resolution,
            limits.directional_shadow_resolution,
            rhi::TextureFormat::Rgba16Float));
        if (!created_velocity || !created_object_id || !created_normal
            || !created_coverage
            || !created_auxiliary_depth || !cascades_created
            || !created_shadow_depth || !created_rsm_normal
            || !created_rsm_flux) {
            return Result::failure(RenderError::AllocationFailed);
        }

        rhi::ShaderDesc object_fragment{};
        object_fragment.stage = rhi::ShaderStage::Fragment;
        object_fragment.format = blobs.format;
        object_fragment.sampler_count = 1U;
        object_fragment.uniform_buffer_count = 1U;
        object_fragment.bytecode = blobs.object_id_fragment;
        auto created_object_pipeline = device.create_pipeline(
            vertex, object_fragment, rhi::TextureFormat::R32Uint,
            rhi::ColorBlend::Disabled, depth_state);

        rhi::ShaderDesc velocity_fragment{};
        velocity_fragment.stage = rhi::ShaderStage::Fragment;
        velocity_fragment.format = blobs.format;
        velocity_fragment.sampler_count = 1U;
        velocity_fragment.uniform_buffer_count = 1U;
        velocity_fragment.bytecode = blobs.velocity_fragment;
        auto motion_vertex=vertex;
        motion_vertex.bytecode=blobs.motion_vertex;
        auto created_velocity_pipeline = device.create_pipeline(
            motion_vertex, velocity_fragment, rhi::TextureFormat::Rgba32Float,
            rhi::ColorBlend::Disabled, depth_state);

        rhi::ShaderDesc media_object_fragment{};
        media_object_fragment.stage = rhi::ShaderStage::Fragment;
        media_object_fragment.format = blobs.format;
        media_object_fragment.bytecode = blobs.media_object_id_fragment;
        media_object_fragment.sampler_count = 2U;
        auto created_media_object_pipeline = device.create_pipeline(
            vertex, media_object_fragment, rhi::TextureFormat::R32Uint,
            rhi::ColorBlend::Disabled, depth_state);

        rhi::ShaderDesc media_velocity_fragment{};
        media_velocity_fragment.stage = rhi::ShaderStage::Fragment;
        media_velocity_fragment.format = blobs.format;
        media_velocity_fragment.bytecode = blobs.media_velocity_fragment;
        media_velocity_fragment.sampler_count = 2U;
        auto created_media_velocity_pipeline = device.create_pipeline(
            vertex, media_velocity_fragment, rhi::TextureFormat::Rgba32Float,
            rhi::ColorBlend::Disabled, depth_state);

        rhi::ShaderDesc normal_fragment{};
        normal_fragment.stage = rhi::ShaderStage::Fragment;
        normal_fragment.format = blobs.format;
        normal_fragment.sampler_count = 1U;
        normal_fragment.uniform_buffer_count = 1U;
        normal_fragment.bytecode = blobs.normal_fragment;
        auto created_normal_pipeline = device.create_pipeline(
            vertex, normal_fragment, rhi::TextureFormat::Rg16Float,
            rhi::ColorBlend::Disabled, depth_state);

        rhi::ShaderDesc media_normal_fragment{};
        media_normal_fragment.stage = rhi::ShaderStage::Fragment;
        media_normal_fragment.format = blobs.format;
        media_normal_fragment.bytecode = blobs.media_normal_fragment;
        media_normal_fragment.sampler_count = 2U;
        auto created_media_normal_pipeline = device.create_pipeline(
            vertex, media_normal_fragment, rhi::TextureFormat::Rg16Float,
            rhi::ColorBlend::Disabled, depth_state);

        // The three products above in one pass where the device writes
        // several targets at once. Same vertex stages as the single-target
        // passes: motion for opaque geometry, the plain stage for media.
        if (device.capabilities().multiple_render_targets) {
            constexpr std::array<rhi::TextureFormat, 3U> auxiliary_formats{
                rhi::TextureFormat::R32Uint, rhi::TextureFormat::Rgba32Float,
                rhi::TextureFormat::Rg16Float};
            rhi::ShaderDesc auxiliary_fragment = velocity_fragment;
            auxiliary_fragment.bytecode = blobs.auxiliary_fragment;
            auto created_auxiliary = device.create_pipeline(
                motion_vertex, auxiliary_fragment, auxiliary_formats,
                rhi::ColorBlend::Disabled, depth_state);
            rhi::ShaderDesc media_auxiliary_fragment = media_velocity_fragment;
            media_auxiliary_fragment.bytecode = blobs.media_auxiliary_fragment;
            auto created_media_auxiliary = device.create_pipeline(
                vertex, media_auxiliary_fragment, auxiliary_formats,
                rhi::ColorBlend::Disabled, depth_state);
            if (!created_auxiliary || !created_media_auxiliary) {
                return Result::failure(RenderError::BackendFailure);
            }
            auxiliary_pipeline = std::move(created_auxiliary).value();
            media_auxiliary_pipeline = std::move(created_media_auxiliary).value();
        }

        rhi::ShaderDesc coverage_vertex{};
        coverage_vertex.stage = rhi::ShaderStage::Vertex;
        coverage_vertex.format = blobs.format;
        coverage_vertex.bytecode = blobs.coverage_vertex;
        rhi::ShaderDesc coverage_fragment{};
        coverage_fragment.stage = rhi::ShaderStage::Fragment;
        coverage_fragment.format = blobs.format;
        coverage_fragment.bytecode = blobs.coverage_fragment;
        coverage_fragment.sampler_count = 1U;
        auto created_coverage_pipeline = device.create_pipeline(
            coverage_vertex, coverage_fragment, rhi::TextureFormat::R32Float,
            rhi::ColorBlend::Disabled);
        auto created_coverage_sampler =
            device.create_sampler(rhi::Filter::Nearest);
        rhi::ShaderDesc shadow_fragment{};
        shadow_fragment.stage = rhi::ShaderStage::Fragment;
        shadow_fragment.format = blobs.format;
        shadow_fragment.sampler_count = 1U;
        shadow_fragment.uniform_buffer_count = 1U;
        shadow_fragment.bytecode = blobs.shadow_fragment;
        auto created_shadow_pipeline = device.create_pipeline(
            vertex, shadow_fragment, rhi::TextureFormat::R32Float,
            rhi::ColorBlend::Disabled, depth_state);
        rhi::ShaderDesc rsm_normal_fragment{};
        rsm_normal_fragment.stage = rhi::ShaderStage::Fragment;
        rsm_normal_fragment.format = blobs.format;
        rsm_normal_fragment.sampler_count = 1U;
        rsm_normal_fragment.uniform_buffer_count = 1U;
        rsm_normal_fragment.bytecode = blobs.rsm_normal_fragment;
        auto created_rsm_normal_pipeline = device.create_pipeline(
            vertex, rsm_normal_fragment, rhi::TextureFormat::Rgba16Float,
            rhi::ColorBlend::Disabled, depth_state);
        rhi::ShaderDesc rsm_flux_fragment{};
        rsm_flux_fragment.stage = rhi::ShaderStage::Fragment;
        rsm_flux_fragment.format = blobs.format;
        rsm_flux_fragment.bytecode = blobs.rsm_flux_fragment;
        rsm_flux_fragment.sampler_count = 1U;
        rsm_flux_fragment.uniform_buffer_count = 1U;
        auto created_rsm_flux_pipeline = device.create_pipeline(
            vertex, rsm_flux_fragment, rhi::TextureFormat::Rgba16Float,
            rhi::ColorBlend::Disabled, depth_state);
        auto created_shadow_sampler =
            device.create_sampler(rhi::Filter::Nearest);
        if (!created_object_pipeline || !created_velocity_pipeline
            || !created_media_object_pipeline
            || !created_media_velocity_pipeline
            || !created_normal_pipeline || !created_media_normal_pipeline
            || !created_coverage_pipeline || !created_coverage_sampler
            || !created_shadow_pipeline || !created_shadow_sampler
            || !created_rsm_normal_pipeline
            || !created_rsm_flux_pipeline) {
            return Result::failure(RenderError::BackendFailure);
        }
        velocity = std::move(created_velocity).value();
        object_id = std::move(created_object_id).value();
        normal = std::move(created_normal).value();
        coverage = std::move(created_coverage).value();
        auxiliary_depth = std::move(created_auxiliary_depth).value();
        object_id_pipeline = std::move(created_object_pipeline).value();
        velocity_pipeline = std::move(created_velocity_pipeline).value();
        normal_pipeline = std::move(created_normal_pipeline).value();
        media_normal_pipeline =
            std::move(created_media_normal_pipeline).value();
        media_object_id_pipeline =
            std::move(created_media_object_pipeline).value();
        media_velocity_pipeline =
            std::move(created_media_velocity_pipeline).value();
        coverage_pipeline = std::move(created_coverage_pipeline).value();
        coverage_sampler = std::move(created_coverage_sampler).value();
        shadow_depth = std::move(created_shadow_depth).value();
        shadow_pipeline = std::move(created_shadow_pipeline).value();
        shadow_sampler = std::move(created_shadow_sampler).value();
        rsm_normal = std::move(created_rsm_normal).value();
        rsm_flux = std::move(created_rsm_flux).value();
        rsm_normal_pipeline =
            std::move(created_rsm_normal_pipeline).value();
        rsm_flux_pipeline =
            std::move(created_rsm_flux_pipeline).value();
    }

    // Image-based lighting, Advanced only. A device without compute keeps the
    // hemisphere rather than failing: this is an image-quality addition, not a
    // capability the renderer's contract depends on, and refusing to construct
    // over it would take a working Advanced renderer off a machine that had
    // one.
    std::optional<EnvironmentPrefilter> environment;
    rhi::Texture environment_fallback;
    rhi::Texture environment_cube_fallback;
    rhi::Sampler environment_sampler;
    {
        // Bound whether or not an environment exists, so the descriptor table
        // has one shape for every scene. Never sampled: the shader selects on
        // the intensity constant.
        rhi::TextureDesc cube_desc = sampled_desc(1U, 1U);
        cube_desc.array_layers = 6U;
        cube_desc.cube = true;
        auto created_fallback = device.create_texture(sampled_desc(1U, 1U));
        auto created_cube = device.create_texture(cube_desc);
        auto created_sampler = device.create_sampler(rhi::Filter::Linear);
        if (!created_fallback || !created_cube || !created_sampler) {
            return Result::failure(RenderError::AllocationFailed);
        }
        environment_fallback = std::move(created_fallback).value();
        environment_cube_fallback = std::move(created_cube).value();
        environment_sampler = std::move(created_sampler).value();
        if (advanced && EnvironmentPrefilter::shaders_available()
            && device.capabilities().compute) {
            auto created_environment = EnvironmentPrefilter::create(device);
            if (created_environment) {
                environment.emplace(std::move(created_environment).value());
            }
        }
    }

    // The Advanced sky also draws analytic sun discs with the flat background,
    // so it can operate even on a device without environment compute support.
    rhi::GraphicsPipeline sky_pipeline;
    if (advanced && blobs.sky_present) {
        rhi::ShaderDesc sky_vertex{};
        sky_vertex.stage = rhi::ShaderStage::Vertex;
        sky_vertex.format = blobs.format;
        sky_vertex.bytecode = blobs.fullscreen_vertex;
        rhi::ShaderDesc sky_fragment{};
        sky_fragment.stage = rhi::ShaderStage::Fragment;
        sky_fragment.format = blobs.format;
        sky_fragment.bytecode = blobs.sky_fragment;
        sky_fragment.sampler_count = 1U;
        sky_fragment.uniform_buffer_count = 1U;
        auto created_sky = device.create_pipeline(
            sky_vertex, sky_fragment, color_format, rhi::ColorBlend::Disabled,
            rhi::DepthState{});
        if (created_sky) {
            sky_pipeline = std::move(created_sky).value();
        } else {
            // No sky is a flat background, not a failed renderer.
            environment.reset();
        }
    }
    // Linear, so a clip at one resolution resamples cleanly onto a quad of any
    // size on screen.
    auto media_sampler = device.create_sampler(rhi::Filter::Linear);
    if (!media_sampler) {
        return Result::failure(RenderError::AllocationFailed);
    }
    auto material_fallback = device.create_texture(sampled_desc(1U, 1U));
    auto normal_fallback = device.create_texture(sampled_desc(1U, 1U));
    auto offline_media = device.create_texture(sampled_desc(64U, 64U));
    auto ies_desc = sampled_desc(ies_vertical_samples, ies_horizontal_samples * max_ies_profiles);
    ies_desc.format = rhi::TextureFormat::R32Float;
    auto ies_atlas = device.create_texture(ies_desc);
    if (!ies_atlas) return Result::failure(RenderError::AllocationFailed);
    if (!material_fallback || !normal_fallback || !offline_media) {
        return Result::failure(RenderError::AllocationFailed);
    }

    static_assert(sizeof(scene::MeshVertex) == GeometryArena::vertex_bytes);
    static_assert(sizeof(scene::Vec4) == GeometryArena::tangent_bytes);
    static_assert(sizeof(GpuSkin) == GeometryArena::skin_bytes);
    GeometryArenaLimits geometry_limits{};
    geometry_limits.max_vertices = limits.max_vertices;
    geometry_limits.max_indices = limits.max_indices;
    geometry_limits.max_skins = std::min(limits.max_skin_vertices, limits.max_vertices);
    geometry_limits.initial_vertices = std::min(limits.initial_vertices, limits.max_vertices);
    geometry_limits.initial_indices = std::min(limits.initial_indices, limits.max_indices);
    auto geometry = GeometryResidency::for_device(device, geometry_limits);
    auto instances = device.create_storage_buffer(
        limits.max_instances * static_cast<core::u32>(sizeof(GpuInstance)));
    auto lights = device.create_storage_buffer(
        limits.max_lights * static_cast<core::u32>(sizeof(GpuLight)));
    auto inline_effect_ops = device.create_storage_buffer(
        limits.max_inline_effect_ops
        * static_cast<core::u32>(sizeof(GpuInlineEffectOp)));
    auto inline_effect_parameters = device.create_storage_buffer(
        limits.max_inline_effect_parameter_vectors
        * static_cast<core::u32>(sizeof(GpuInlineEffectParameter)));
    if (!geometry || !instances || !lights
        || !inline_effect_ops || !inline_effect_parameters) {
        return Result::failure(RenderError::AllocationFailed);
    }

    SceneRenderer renderer{
        device,
        limits,
        std::move(color).value(),
        std::move(depth).value(),
        std::move(pipeline).value(),
        std::move(blended_pipelines),
        std::move(media_blended_pipelines),
        std::move(media_depth_pipeline).value(),
        std::move(blended_depth_pipeline).value(),
        std::move(media_sampler).value(),
        std::move(material_fallback).value(),
        std::move(normal_fallback).value(),
        std::move(geometry).value(),
        std::move(instances).value(),
        std::move(lights).value(),
        std::move(inline_effect_ops).value(),
        std::move(inline_effect_parameters).value(),
        mode,
        std::move(velocity),
        std::move(object_id),
        std::move(normal),
        std::move(coverage),
        std::move(auxiliary_depth),
        std::move(velocity_pipeline),
        std::move(object_id_pipeline),
        std::move(normal_pipeline),
        std::move(media_velocity_pipeline),
        std::move(media_object_id_pipeline),
        std::move(media_normal_pipeline),
        std::move(coverage_pipeline),
        std::move(coverage_sampler)};
    auto pose_buffer=device.create_storage_buffer(max_frame_pose_bindings*static_cast<core::u32>(sizeof(GpuPose)));
    if (!pose_buffer) return Result::failure(RenderError::AllocationFailed);
    renderer.pose_buffer_=std::move(pose_buffer).value();
    renderer.pose_scratch_=std::make_unique<ModelPoseScratch>();
    renderer.current_pose_.resize(max_model_pose_bindings);
    renderer.previous_pose_.resize(max_model_pose_bindings);
    renderer.pose_upload_.reserve(max_frame_pose_bindings*sizeof(GpuPose));
    renderer.animated_instances_.reserve(limits.max_instances);
    // Occlusion, where the device can run it. Constructed rather than
    // refused when absent: a scene that authors no radius never dispatches,
    // and a device without compute keeps a working Advanced renderer.
    if (mode == SceneRendererMode::AdvancedLinear
        && AmbientOcclusion::shaders_available()
        && device.capabilities().compute
        && device.capabilities().storage_images) {
        auto created_occlusion =
            AmbientOcclusion::create(device, width, height);
        if (created_occlusion) {
            renderer.occlusion_.emplace(std::move(created_occlusion).value());
            auto temporal_ao = TemporalResolve::create(device, width, height,
                TemporalResolveLimits{limits.max_temporal_history_targets},
                rhi::TextureFormat::R32Float);
            if (temporal_ao) {
                renderer.temporal_occlusion_.emplace(std::move(temporal_ao).value());
            }
        }
    }
    // Particles, where the device can simulate them. Like occlusion, a device
    // that cannot keeps a working renderer; a scene that places an effect is
    // then refused readiness with the reason.
    renderer.adopt_particle_runtime(device, color_format, mode, limits.particles);
    // The local-light shadow atlas, and the light table that indexes it.
    // Allocated once at construction like the cascades: every node runs the
    // same build and therefore reserves the same bytes whatever a show
    // authors.
    if (mode == SceneRendererMode::AdvancedLinear) {
        renderer.shadow_atlas_limits_ = limits.shadow_atlas;
        auto atlas = device.create_texture(color_desc(
            limits.shadow_atlas.extent, limits.shadow_atlas.extent,
            rhi::TextureFormat::R32Float));
        auto atlas_depth = device.create_texture(depth_desc(
            limits.shadow_atlas.extent, limits.shadow_atlas.extent));
        auto probes = device.create_storage_buffer(scene::max_baked_probes * static_cast<core::u32>(sizeof(BakedProbe)));
        if (!probes) return Result::failure(RenderError::AllocationFailed);
        renderer.probe_buffer_ = std::move(probes).value();
        auto shadows = device.create_storage_buffer(
            static_cast<core::u32>(scene::max_scene_point_lights
                                   * sizeof(GpuLightShadow)));
        if (!atlas || !atlas_depth || !shadows) {
            return Result::failure(RenderError::AllocationFailed);
        }
        {
            const auto held = renderer.geometry_->lock();
            renderer.atlas_home_ = renderer.geometry_->adopt_shadow(
                {std::move(atlas).value(), rhi::Texture{}});
        }
        if (!renderer.atlas_home_) {
            return Result::failure(RenderError::AllocationFailed);
        }
        renderer.shadow_atlas_depth_ = std::move(atlas_depth).value();
        renderer.light_shadow_buffer_ = std::move(shadows).value();
        auto particle_lights = device.create_writable_buffer(max_particle_lights * 128U);
        if (!particle_lights) {
            return Result::failure(RenderError::AllocationFailed);
        }
        renderer.particle_light_buffer_ = std::move(particle_lights).value();
        try {
            renderer.light_shadow_scratch_.reserve(
                static_cast<std::size_t>(scene::max_scene_point_lights)
                * sizeof(GpuLightShadow));
        } catch (const std::bad_alloc&) {
            return Result::failure(RenderError::AllocationFailed);
        }
    }
    if (advanced) {
        // Registered with the shared geometry, so another renderer drawing
        // the same maps samples these instead.
        const auto held = renderer.geometry_->lock();
        try {
            renderer.cascade_homes_.reserve(directional_shadow.size());
        } catch (const std::bad_alloc&) {
            return Result::failure(RenderError::AllocationFailed);
        }
        for (auto& cascade : directional_shadow) {
            auto adopted = renderer.geometry_->adopt_shadow({std::move(cascade), rhi::Texture{}});
            if (!adopted) {
                return Result::failure(RenderError::AllocationFailed);
            }
            renderer.cascade_homes_.push_back(std::move(adopted));
        }
        renderer.rsm_home_ = renderer.geometry_->adopt_shadow({std::move(rsm_normal), std::move(rsm_flux)});
        if (!renderer.rsm_home_) {
            return Result::failure(RenderError::AllocationFailed);
        }
    }
    renderer.shadow_depth_ = std::move(shadow_depth);
    renderer.shadow_pipeline_ = std::move(shadow_pipeline);
    renderer.transmission_background_ = std::move(transmission_background).value();
    renderer.shadow_sampler_ = std::move(shadow_sampler);
    renderer.rsm_normal_pipeline_ = std::move(rsm_normal_pipeline);
    renderer.auxiliary_pipeline_ = std::move(auxiliary_pipeline);
    renderer.media_auxiliary_pipeline_ = std::move(media_auxiliary_pipeline);
    if (limits.view_shading && device.capabilities().wireframe_fill) {
        // The lit and media colour passes, rasterizing edges. Depth still
        // tests and writes, so an edge behind another is drawn after it in
        // the same pass; the colour pass clears its own depth, so the edges
        // are tested only against each other and every one shows.
        const rhi::RasterState edges{rhi::FillMode::Wireframe};
        auto wireframe = device.create_pipeline(
            vertex, fragment, color_format, rhi::ColorBlend::Disabled,
            depth_state, edges);
        auto media_wireframe = device.create_pipeline(
            vertex, media_fragment, color_format,
            advanced ? rhi::ColorBlend::PremultipliedNormal
                     : rhi::ColorBlend::Normal,
            depth_state, edges);
        if (!wireframe || !media_wireframe) {
            return Result::failure(RenderError::BackendFailure);
        }
        renderer.wireframe_pipeline_ = std::move(wireframe).value();
        renderer.media_wireframe_pipeline_ =
            std::move(media_wireframe).value();
    }
    renderer.rsm_flux_pipeline_ = std::move(rsm_flux_pipeline);
    renderer.environment_ = std::move(environment);
    renderer.offline_media_ = std::move(offline_media).value();
    renderer.environment_fallback_ = std::move(environment_fallback);
    renderer.environment_cube_fallback_ =
        std::move(environment_cube_fallback);
    renderer.environment_sampler_ = std::move(environment_sampler);
    if (!renderer.ltc_asset_.create(device)) return Result::failure(RenderError::AllocationFailed);
    renderer.ies_atlas_ = std::move(ies_atlas).value();
    renderer.ies_atlas_scratch_.resize(ies_profile_texels * max_ies_profiles);
    renderer.sky_pipeline_ = std::move(sky_pipeline);
    for (std::size_t index = 0U; index < material_sampler_count; ++index) {
        auto sampler = device.create_sampler(material_sampler_desc(index));
        if (!sampler) return Result::failure(RenderError::AllocationFailed);
        renderer.material_samplers_[index] = std::move(sampler).value();
    }

    // Every CPU arena is reserved to its fixed cap during renderer creation.
    // Asynchronous model publication may happen while the operator is on air;
    // integrating completed geometry must not turn into a large vector growth
    // allocation on that recording's frame path.
    try {
    renderer.instance_scratch_.reserve(
        static_cast<std::size_t>(limits.max_instances) * sizeof(GpuInstance));
    renderer.light_scratch_.reserve(
        static_cast<std::size_t>(limits.max_lights) * sizeof(GpuLight));
    renderer.inline_effect_op_scratch_.reserve(
        static_cast<std::size_t>(limits.max_inline_effect_ops)
        * sizeof(GpuInlineEffectOp));
    renderer.inline_effect_parameter_scratch_.reserve(
        static_cast<std::size_t>(limits.max_inline_effect_parameter_vectors)
        * sizeof(GpuInlineEffectParameter));
    renderer.inline_program_cache_.reserve(limits.max_instances);
    renderer.batch_uniforms_.reserve(
        static_cast<std::size_t>(limits.max_instances) * sizeof(BatchConstants));
    renderer.batches_.reserve(limits.max_instances);
    renderer.opaque_submissions_.reserve(limits.max_instances);
    renderer.reordered_instances_.reserve(static_cast<std::size_t>(limits.max_instances) * sizeof(GpuInstance));
    renderer.geometry_selection_.reserve(limits.max_instances);
    if (advanced) {
        // Model textures are shared by every renderer on the device.
        auto textures = TextureResidency::for_device(device);
        if (!textures) return Result::failure(RenderError::AllocationFailed);
        renderer.texture_streamer_.emplace(limits.max_model_textures, std::move(textures).value());
        renderer.texture_requests_.reserve(limits.max_model_textures);
    }
    renderer.opaque_visibility_.reserve(limits.max_instances);
    renderer.camera_batch_uniforms_.reserve(static_cast<std::size_t>(limits.max_instances) * sizeof(BatchConstants));
    renderer.camera_pulled_batch_uniforms_.reserve(static_cast<std::size_t>(limits.max_instances) * sizeof(BatchConstants));
    renderer.casters_.batches.reserve(limits.max_instances);
    renderer.casters_.uniforms.reserve(static_cast<std::size_t>(limits.max_instances) * sizeof(BatchConstants));
    renderer.casters_.textures.reserve(limits.max_instances);
    renderer.instance_digests_.reserve(limits.max_instances);
    renderer.camera_batches_.reserve(limits.max_instances);
    renderer.camera_batch_textures_.reserve(static_cast<std::size_t>(limits.max_instances) *
        (mode == SceneRendererMode::AdvancedLinear ? advanced_fragment_textures : basic_fragment_textures));
    // Must match fragment_textures_per_batch exactly. A short reservation is
    // not a wrong image, it is a vector growth on the frame path - which is
    // the thing this arena exists to prevent.
    renderer.batch_textures_.reserve(
        static_cast<std::size_t>(limits.max_instances)
        * (advanced ? advanced_fragment_textures
                    : basic_fragment_textures));
    renderer.rsm_flux_textures_.reserve(limits.max_instances);
    renderer.camera_coverage_textures_.reserve(limits.max_instances);
    renderer.composite_draws_.reserve(limits.max_instances);
    renderer.layer_volumes_.reserve(limits.max_instances);
    renderer.layer_edges_.reserve(
        static_cast<std::size_t>(limits.max_instances) * layer_edges_per_layer);
    renderer.layer_edge_offsets_.reserve(
        static_cast<std::size_t>(limits.max_instances) + 1U);
    renderer.layer_in_degree_.reserve(limits.max_instances);
    renderer.layer_ready_.reserve(limits.max_instances);
    renderer.layer_order_.reserve(limits.max_instances);
    renderer.layer_placed_.reserve(limits.max_instances);
    renderer.depth_only_media_.reserve(scene::max_scene_media);
    // Every scene renderer names its device as a candidate for video decode,
    // for as long as it exists. A scene is the only thing that samples media,
    // so the live renderers are the complete set the decoder has to agree
    // with - and one that failed to build above never counted.
    renderer.video_decode_offer_ = offer_video_decode_device(device);
    renderer.retired_models_.reserve(
        limits.max_model_refresh_generations);
    renderer.pending_model_integrations_.reserve(
        limits.max_model_refresh_generations);
    renderer.object_velocity_ = std::move(object_velocity);
    renderer.previous_transforms_.reserve(limits.max_instances);
    renderer.motion_source_transforms_.reserve(limits.max_instances);
    renderer.next_transforms_.reserve(limits.max_instances);
    renderer.view_histories_.resize(limits.max_history_slots);
    } catch (const std::bad_alloc&) {
        return Result::failure(RenderError::AllocationFailed);
    }
    if (advanced) {
        for (std::size_t i = 0; i < renderer.reflection_filters_.size(); ++i) {
            auto filter = EnvironmentPrefilter::create(device);
            auto source = device.create_texture(color_desc(reflection_capture_width, reflection_capture_height, rhi::TextureFormat::Rgba32Float));
            if (!filter || !source) return Result::failure(RenderError::AllocationFailed);
            renderer.reflection_filters_[i] = std::move(filter).value();
            renderer.reflection_sources_[i] = std::move(source).value();
        }
        // Not built here. The mirror is a second complete renderer at full
        // extent - it very nearly doubles this one - and a scene with no
        // reflective surface never draws a pixel through it. `ensure_planar`
        // builds it when one is authored.
        renderer.planar_buildable_ = limits.planar_reflections;
    }
    return Result::success(std::move(renderer));
}

void SceneRenderer::ensure_planar() noexcept {
    if (planar_renderer_ || !planar_buildable_ || device_ == nullptr) {
        return;
    }
    // One attempt. This is the same bargain the Advanced renderer strikes
    // above it: a renderer that cannot build its mirror now will not manage
    // it on the next frame, and retrying would put the construction of a
    // whole nested renderer on the frame path for the life of the show.
    planar_buildable_ = false;
    auto nested_limits = limits_;
    nested_limits.planar_reflections = false;
    auto planar = SceneRenderer::create(*device_, color_.width(),
                                        color_.height(), nested_limits, mode_);
    if (!planar) {
        return;
    }
    try {
        planar_renderer_ =
            std::make_unique<SceneRenderer>(std::move(planar).value());
    } catch (const std::bad_alloc&) {
        planar_renderer_.reset();
    }
}

bool SceneRenderer::resize(
    rhi::Device& device,
    core::u32 width,
    core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (color_.width() == width && color_.height() == height) {
        return true;
    }
    const bool advanced = mode_ == SceneRendererMode::AdvancedLinear;
    auto color = device.create_texture(color_desc(
        width, height, advanced ? rhi::TextureFormat::Rgba16Float
                                : rhi::TextureFormat::Rgba8Unorm));
    if (!color) {
        return false;
    }
    auto depth = device.create_texture(depth_desc(width, height));
    if (!depth) {
        return false;
    }
    rhi::Texture velocity;
    rhi::Texture object_velocity;
    rhi::Texture object_id;
    rhi::Texture normal;
    rhi::Texture coverage;
    rhi::Texture auxiliary_depth;
    if (advanced) {
        auto created_velocity = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::Rgba32Float));
        auto created_object_velocity = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::Rgba32Float));
        if (!created_object_velocity) return false;
        object_velocity = std::move(created_object_velocity).value();
        auto created_object_id = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::R32Uint));
        auto created_normal = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::Rg16Float));
        auto created_coverage = device.create_texture(
            color_desc(width, height, rhi::TextureFormat::R32Float));
        auto created_auxiliary_depth =
            device.create_texture(depth_desc(width, height, true));
        if (!created_velocity || !created_object_id || !created_normal
            || !created_coverage || !created_auxiliary_depth) {
            return false;
        }
        velocity = std::move(created_velocity).value();
        object_id = std::move(created_object_id).value();
        normal = std::move(created_normal).value();
        coverage = std::move(created_coverage).value();
        auxiliary_depth = std::move(created_auxiliary_depth).value();
    }
    if (planar_renderer_ && !planar_renderer_->resize(device, width, height)) return false;
    // Both replaced together or neither: a colour target that outlived its
    // depth buffer would fail every subsequent draw.
    auto transmission_background = device.create_texture(color_desc(width, height, color_.desc().format));
    if (!transmission_background) return false;
    transmission_background_ = std::move(transmission_background).value();
    color_ = std::move(color).value();
    depth_ = std::move(depth).value();
    if (advanced) {
        velocity_ = std::move(velocity);
        object_velocity_ = std::move(object_velocity);
        object_id_ = std::move(object_id);
        normal_ = std::move(normal);
        coverage_ = std::move(coverage);
        auxiliary_depth_ = std::move(auxiliary_depth);
    }
    history_valid_ = false;
    motion_source_show_frame_ = 0U;
    previous_transforms_.clear();
    motion_source_transforms_.clear();
    next_transforms_.clear();
    std::fill(
        view_histories_.begin(), view_histories_.end(),
        ViewHistoryState{});
    return true;
}

} // namespace mediaserver::render
