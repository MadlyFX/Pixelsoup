#include <mediaserver/render/scene_renderer.hpp>

#include <mediaserver/render/model_loader.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/scene_frag_dxil.h>
#include <mediaserver/shaders/scene_media_frag_dxil.h>
#include <mediaserver/shaders/scene_vert_dxil.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using Result = core::Result<SceneRenderer, RenderError>;

// ---------------------------------------------------------------------------
// The GPU-side records. Every one of these is described twice - here and in
// the HLSL - and a mismatch would be a silently wrong picture rather than a
// compile error, so each carries a size assertion and each field comment names
// its counterpart.

// scene.vert.hlsl: struct Vertex.
static_assert(sizeof(scene::MeshVertex) == 32U);

// scene.vert.hlsl: struct Instance. Matrices are stored as explicit columns on
// both sides, so nothing here transposes anything.
struct GpuInstance final {
    float world[16U]{};  // four columns of the object-to-world transform
    float normal[12U]{}; // three columns of the inverse-transpose basis
    float base_color[3U]{};
    float roughness{};
    float emissive[3U]{};
    float metallic{};
    float layer_style[4U]{}; // opacity, brightness, BlendMode, reserved
};

static_assert(sizeof(GpuInstance) == 160U);
static_assert(alignof(GpuInstance) == 4U);
static_assert(static_cast<core::u8>(scene::BlendMode::Normal) == 0U);
static_assert(static_cast<core::u8>(scene::BlendMode::Add) == 1U);
static_assert(static_cast<core::u8>(scene::BlendMode::Multiply) == 2U);
static_assert(static_cast<core::u8>(scene::BlendMode::Screen) == 3U);

// scene.frag.hlsl: struct Light.
struct GpuLight final {
    float vector_kind[4U]{}; // xyz direction or position, w selects which
    float color_intensity[4U]{};
    float params[4U]{}; // x: range
};

static_assert(sizeof(GpuLight) == 48U);

// scene.vert.hlsl: cbuffer ViewConstants.
struct ViewConstants final {
    float view_projection[16U]{};
};

// scene.vert.hlsl: cbuffer BatchConstants.
struct BatchConstants final {
    core::u32 first_index{};
    core::u32 first_instance{};
    core::u32 reserved0{};
    core::u32 reserved1{};
};

static_assert(sizeof(BatchConstants) == 16U);

// scene.frag.hlsl: cbuffer SceneConstants.
struct SceneConstants final {
    float eye_position[4U]{};
    float sky_ambient[4U]{};
    float ground_exposure[4U]{};
    core::u32 counts[4U]{};
};

static_assert(sizeof(SceneConstants) == 64U);

// ---------------------------------------------------------------------------

struct ShaderBlobs final {
    std::span<const core::u8> vertex;
    std::span<const core::u8> fragment;
    std::span<const core::u8> media_fragment;
    rhi::ShaderFormat format{rhi::ShaderFormat::Dxil};
    bool present{};
};

[[nodiscard]] ShaderBlobs shader_blobs() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return ShaderBlobs{
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_vert_dxil),
            sizeof(mediaserver_scene_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_frag_dxil),
            sizeof(mediaserver_scene_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_scene_media_frag_dxil),
            sizeof(mediaserver_scene_media_frag_dxil)},
        rhi::ShaderFormat::Dxil,
        true,
    };
#else
    // No shader compiler was available when this was built, so there is no 3D
    // path to offer. Saying so plainly is the whole point of the flag.
    return ShaderBlobs{};
#endif
}

template <typename T>
void append(std::vector<core::u8>& into, const T& value) {
    const auto offset = into.size();
    into.resize(offset + sizeof(T));
    std::memcpy(into.data() + offset, &value, sizeof(T));
}

template <typename T>
[[nodiscard]] std::span<const core::u8> bytes_of(const T& value) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&value), sizeof(T)};
}

[[nodiscard]] rhi::TextureDesc color_desc(core::u32 width, core::u32 height) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.color_target = true;
    // Sampled as well as drawn into, because presentation blits from it and a
    // readback test reads it.
    desc.sampled = true;
    return desc;
}

[[nodiscard]] rhi::TextureDesc sampled_desc(
    core::u32 width,
    core::u32 height) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    return desc;
}

[[nodiscard]] rhi::TextureDesc depth_desc(core::u32 width, core::u32 height) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Depth32Float;
    desc.depth_target = true;
    return desc;
}

[[nodiscard]] GpuInstance to_gpu(const scene::MeshInstance& instance) noexcept {
    GpuInstance record{};
    for (core::u32 index = 0U; index < 16U; ++index) {
        record.world[index] = instance.world.m[index];
    }
    // The first twelve elements of a column-major 4x4 are exactly its first
    // three columns, and the fourth element of each is zero for a basis, so
    // this is a copy rather than a gather.
    for (core::u32 index = 0U; index < 12U; ++index) {
        record.normal[index] = instance.normal.m[index];
    }
    record.base_color[0U] = instance.material.base_color.x;
    record.base_color[1U] = instance.material.base_color.y;
    record.base_color[2U] = instance.material.base_color.z;
    record.roughness = instance.material.roughness;
    record.emissive[0U] = instance.material.emissive.x;
    record.emissive[1U] = instance.material.emissive.y;
    record.emissive[2U] = instance.material.emissive.z;
    record.metallic = instance.material.metallic;
    record.layer_style[0U] = std::isfinite(instance.style.opacity)
        ? std::clamp(instance.style.opacity, 0.0F, 1.0F)
        : 1.0F;
    const float folded_brightness = instance.style.effects.brightness_enabled
        ? instance.style.effects.brightness
        : 1.0F;
    // The scene has already folded ordered brightness nodes by multiplication;
    // retain that result rather than clamping it back to one effect's range.
    record.layer_style[1U] = std::isfinite(folded_brightness)
        ? std::max(folded_brightness, 0.0F)
        : 1.0F;
    switch (instance.style.blend) {
    case scene::BlendMode::Normal:
    case scene::BlendMode::Add:
    case scene::BlendMode::Multiply:
    case scene::BlendMode::Screen:
        record.layer_style[2U] = static_cast<float>(
            static_cast<core::u8>(instance.style.blend));
        break;
    default:
        record.layer_style[2U] = 0.0F;
        break;
    }
    // Blur is intentionally absent. It needs an offscreen source and a
    // separable compositor pass; treating a radius as a pointwise colour tweak
    // would claim an effect the renderer did not perform.
    return record;
}

[[nodiscard]] scene::BlendMode valid_blend(scene::BlendMode blend) noexcept {
    switch (blend) {
    case scene::BlendMode::Normal:
    case scene::BlendMode::Add:
    case scene::BlendMode::Multiply:
    case scene::BlendMode::Screen:
        return blend;
    }
    return scene::BlendMode::Normal;
}

[[nodiscard]] float opacity(const scene::LayerStyle& style) noexcept {
    return std::isfinite(style.opacity)
        ? std::clamp(style.opacity, 0.0F, 1.0F)
        : 1.0F;
}

[[nodiscard]] bool needs_compositing(const scene::LayerStyle& style) noexcept {
    return opacity(style) < 1.0F
        || valid_blend(style.blend) != scene::BlendMode::Normal;
}

[[nodiscard]] std::size_t blend_index(scene::BlendMode blend) noexcept {
    return static_cast<std::size_t>(
        static_cast<core::u8>(valid_blend(blend)));
}

[[nodiscard]] rhi::ColorBlend to_rhi_blend(scene::BlendMode blend) noexcept {
    switch (valid_blend(blend)) {
    case scene::BlendMode::Add:
        return rhi::ColorBlend::Add;
    case scene::BlendMode::Multiply:
        return rhi::ColorBlend::Multiply;
    case scene::BlendMode::Screen:
        return rhi::ColorBlend::Screen;
    case scene::BlendMode::Normal:
        break;
    }
    return rhi::ColorBlend::Normal;
}

[[nodiscard]] float camera_depth(
    const scene::Mat4& world,
    scene::Vec3 local_center,
    scene::Vec3 eye,
    scene::Vec3 forward) noexcept {
    forward = scene::normalize(forward, scene::Vec3{0.0F, 0.0F, -1.0F});
    const float result = scene::dot(
        scene::transform_point(world, local_center) - eye, forward);
    return std::isfinite(result) ? result : 0.0F;
}

[[nodiscard]] bool finite(scene::Vec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y)
        && std::isfinite(value.z);
}

[[nodiscard]] bool finite(const scene::Mat4& value) noexcept {
    return std::all_of(value.m.begin(), value.m.end(), [](float element) {
        return std::isfinite(element);
    });
}

} // namespace

bool SceneRenderer::shaders_available() noexcept {
    return shader_blobs().present;
}

SceneRenderer::SceneRenderer(
    rhi::Device& device,
    RendererLimits limits,
    rhi::Texture color,
    rhi::Texture depth,
    rhi::GraphicsPipeline pipeline,
    std::array<rhi::GraphicsPipeline, 4U> blended_pipelines,
    std::array<rhi::GraphicsPipeline, 4U> media_blended_pipelines,
    rhi::Sampler media_sampler,
    rhi::Texture material_fallback,
    rhi::StorageBuffer vertices,
    rhi::StorageBuffer indices,
    rhi::StorageBuffer instances,
    rhi::StorageBuffer lights) noexcept
    : device_(&device),
      limits_(limits),
      color_(std::move(color)),
      depth_(std::move(depth)),
      pipeline_(std::move(pipeline)),
      blended_pipelines_(std::move(blended_pipelines)),
      media_blended_pipelines_(std::move(media_blended_pipelines)),
      media_sampler_(std::move(media_sampler)),
      material_fallback_(std::move(material_fallback)),
      vertex_buffer_(std::move(vertices)),
      index_buffer_(std::move(indices)),
      instance_buffer_(std::move(instances)),
      light_buffer_(std::move(lights)) {}

Result SceneRenderer::create(
    rhi::Device& device,
    core::u32 width,
    core::u32 height,
    const RendererLimits& limits) {
    const auto blobs = shader_blobs();
    if (!blobs.present) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }
    if (width == 0U || height == 0U || limits.max_instances == 0U
        || limits.max_vertices == 0U || limits.max_indices == 0U
        || limits.max_lights == 0U) {
        return Result::failure(RenderError::InvalidConfiguration);
    }
    if (!device.supports(blobs.format)) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }

    auto color = device.create_texture(color_desc(width, height));
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
    // Vertices, indices, and instances; two uniform slots, the view and the
    // per-batch offsets.
    vertex.storage_buffer_count = 3U;
    vertex.uniform_buffer_count = 2U;

    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = blobs.format;
    fragment.bytecode = blobs.fragment;
    fragment.sampler_count = 3U;
    fragment.storage_buffer_count = 1U;
    fragment.uniform_buffer_count = 1U;

    rhi::DepthState depth_state{};
    depth_state.enabled = true;
    depth_state.write = true;
    // Reversed depth: the scene layer's projection puts the near plane at one.
    depth_state.greater = true;

    auto pipeline = device.create_pipeline(
        vertex,
        fragment,
        rhi::TextureFormat::Rgba8Unorm,
        rhi::ColorBlend::Disabled,
        depth_state);
    if (!pipeline) {
        return Result::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }

    // Media uses the same pulled vertex stage and samples one clip. It always
    // goes through a compositing pipeline because source pixels may have alpha
    // even when the layer's authored opacity is one.
    rhi::ShaderDesc media_fragment{};
    media_fragment.stage = rhi::ShaderStage::Fragment;
    media_fragment.format = blobs.format;
    media_fragment.bytecode = blobs.media_fragment;
    media_fragment.sampler_count = 1U;

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
            rhi::TextureFormat::Rgba8Unorm,
            to_rhi_blend(blend_modes[index]),
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
            rhi::TextureFormat::Rgba8Unorm,
            to_rhi_blend(blend_modes[index]),
            composite_depth);
        if (!media_blended) {
            return Result::failure(
                media_blended.error() == rhi::RhiError::ShaderFormatUnsupported
                    ? RenderError::ShaderFormatUnsupported
                    : RenderError::BackendFailure);
        }
        media_blended_pipelines[index] = std::move(media_blended).value();
    }
    // Linear, so a clip at one resolution resamples cleanly onto a quad of any
    // size on screen.
    auto media_sampler = device.create_sampler(rhi::Filter::Linear);
    if (!media_sampler) {
        return Result::failure(RenderError::AllocationFailed);
    }
    auto material_fallback = device.create_texture(sampled_desc(1U, 1U));
    if (!material_fallback) {
        return Result::failure(RenderError::AllocationFailed);
    }

    auto vertices = device.create_storage_buffer(
        limits.max_vertices * static_cast<core::u32>(sizeof(scene::MeshVertex)));
    auto indices = device.create_storage_buffer(
        limits.max_indices * static_cast<core::u32>(sizeof(core::u32)));
    auto instances = device.create_storage_buffer(
        limits.max_instances * static_cast<core::u32>(sizeof(GpuInstance)));
    auto lights = device.create_storage_buffer(
        limits.max_lights * static_cast<core::u32>(sizeof(GpuLight)));
    if (!vertices || !indices || !instances || !lights) {
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
        std::move(media_sampler).value(),
        std::move(material_fallback).value(),
        std::move(vertices).value(),
        std::move(indices).value(),
        std::move(instances).value(),
        std::move(lights).value()};

    // Every per-frame arena is reserved to its cap here so that the frame path
    // never grows one. The mesh arenas are not: they grow as geometry becomes
    // resident, which happens on a document change rather than on a frame.
    renderer.instance_scratch_.reserve(
        static_cast<std::size_t>(limits.max_instances) * sizeof(GpuInstance));
    renderer.light_scratch_.reserve(
        static_cast<std::size_t>(limits.max_lights) * sizeof(GpuLight));
    renderer.batch_uniforms_.reserve(
        static_cast<std::size_t>(limits.max_instances) * sizeof(BatchConstants));
    renderer.batches_.reserve(limits.max_instances);
    renderer.batch_textures_.reserve(
        static_cast<std::size_t>(limits.max_instances) * 3U);
    renderer.composite_draws_.reserve(limits.max_instances);
    return Result::success(std::move(renderer));
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
    auto color = device.create_texture(color_desc(width, height));
    if (!color) {
        return false;
    }
    auto depth = device.create_texture(depth_desc(width, height));
    if (!depth) {
        return false;
    }
    // Both replaced together or neither: a colour target that outlived its
    // depth buffer would fail every subsequent draw.
    color_ = std::move(color).value();
    depth_ = std::move(depth).value();
    return true;
}

bool SceneRenderer::append_mesh(
    const scene::MeshData& mesh,
    MeshRange& range) {
    const auto vertex_stride = sizeof(scene::MeshVertex);
    const auto resident_vertices =
        static_cast<core::u64>(vertex_arena_.size() / vertex_stride);
    const auto resident_indices =
        static_cast<core::u64>(index_arena_.size() / sizeof(core::u32));

    if (resident_vertices + mesh.vertices.size() > limits_.max_vertices
        || resident_indices + mesh.indices.size() > limits_.max_indices) {
        // Out of arena. Reported through `dropped_instances` rather than as a
        // failed frame: one over-tessellated object or heavy model should not
        // black out an output that is otherwise correct.
        return false;
    }

    const auto vertex_offset = vertex_arena_.size();
    vertex_arena_.resize(
        vertex_offset + mesh.vertices.size() * vertex_stride);
    std::memcpy(
        vertex_arena_.data() + vertex_offset,
        mesh.vertices.data(),
        mesh.vertices.size() * vertex_stride);

    const auto first_index = static_cast<core::u32>(resident_indices);
    const auto base_vertex = static_cast<core::u32>(resident_vertices);
    const auto index_offset = index_arena_.size();
    index_arena_.resize(
        index_offset + mesh.indices.size() * sizeof(core::u32));
    auto* const written =
        reinterpret_cast<core::u32*>(index_arena_.data() + index_offset);
    for (std::size_t position = 0U; position < mesh.indices.size();
         ++position) {
        // Indices are made absolute on the way in, so a batch is a range in
        // the index arena and nothing else - no base-vertex argument, and no
        // per-backend disagreement about whether one is applied.
        written[position] = base_vertex + mesh.indices[position];
    }

    range = MeshRange{
        first_index, static_cast<core::u32>(mesh.indices.size())};
    geometry_dirty_ = true;
    return true;
}

bool SceneRenderer::ensure_resident(scene::MeshKey key) {
    const auto normal_key = scene::normalized(key);
    if (resident_.find(normal_key) != resident_.end()) {
        return true;
    }
    if (!scene::build_mesh(normal_key, mesh_scratch_)) {
        return false;
    }
    MeshRange range{};
    if (!append_mesh(mesh_scratch_, range)) {
        return false;
    }
    resident_.emplace(normal_key, range);
    return true;
}

bool SceneRenderer::ensure_model_resident(
    const std::string& source,
    rhi::CommandList& commands) {
    if (resident_models_.find(source) != resident_models_.end()) {
        return true;
    }
    if (!load_model(source, model_scratch_)) {
        return false;
    }
    MeshRange asset_range{};
    if (!append_mesh(model_scratch_.mesh, asset_range)) {
        return false;
    }
    ResidentModel resident{};
    resident.bounds = model_scratch_.bounds;
    resident.textures.reserve(model_scratch_.textures.size());
    for (const ModelTexture& source_texture : model_scratch_.textures) {
        rhi::Texture texture{};
        if (source_texture.valid()) {
            auto created = device_->create_texture(sampled_desc(
                source_texture.width, source_texture.height));
            if (created) {
                texture = std::move(created).value();
                if (commands.upload(texture, source_texture.rgba)) {
                    stats_.upload_bytes += source_texture.rgba.size();
                } else {
                    texture = rhi::Texture{};
                }
            }
        }
        resident.textures.push_back(std::move(texture));
    }
    resident.parts.reserve(model_scratch_.parts.size());
    for (const auto& source_part : model_scratch_.parts) {
        resident.parts.push_back(ResidentModelPart{
            MeshRange{
                asset_range.first_index + source_part.first_index,
            source_part.index_count},
            source_part.material,
            source_part.has_material,
            source_part.base_color_texture,
            source_part.metallic_roughness_texture,
            source_part.emissive_texture});
    }
    resident_models_.emplace(source, std::move(resident));
    return true;
}

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
    stats_ = RenderStats{};

    if (!scene::valid(scene.camera) ||
        (!options.view.has_value() && !scene::contains(scene.canvas, slice))) {
        return false;
    }

    SceneRenderView render_view{};
    if (options.view.has_value()) {
        render_view = *options.view;
        if (!finite(render_view.eye) || !finite(render_view.forward) ||
            !(scene::length(render_view.forward) > 1.0e-20F) ||
            !finite(render_view.matrices.view) ||
            !finite(render_view.matrices.projection) ||
            !finite(render_view.matrices.view_projection)) {
            return false;
        }
        render_view.forward = scene::normalize(render_view.forward);
    } else {
        render_view.matrices =
            scene::view_for_slice(scene.camera, scene.canvas, slice);
        render_view.eye = scene.camera.eye;
        render_view.forward = scene::normalize(
            scene.camera.target - scene.camera.eye,
            scene::Vec3{0.0F, 0.0F, -1.0F});
    }

    if (!material_fallback_uploaded_) {
        constexpr std::array<core::u8, 4U> white{255U, 255U, 255U, 255U};
        if (!commands.upload(material_fallback_, white)) {
            return false;
        }
        material_fallback_uploaded_ = true;
        stats_.upload_bytes += white.size();
    }

    instance_scratch_.clear();
    light_scratch_.clear();
    batch_uniforms_.clear();
    batches_.clear();
    batch_textures_.clear();
    composite_draws_.clear();

    core::u32 next_ordinal = 0U;
    const auto held_instances = [&]() noexcept {
        return static_cast<std::size_t>(stats_.instances)
            + composite_draws_.size();
    };
    const std::array<const rhi::Texture*, 3U> fallback_material_textures{
        &material_fallback_, &material_fallback_, &material_fallback_};
    const auto append_material_textures =
        [&](const std::array<const rhi::Texture*, 3U>& textures) {
        for (const rhi::Texture* const texture : textures) {
            batch_textures_.push_back(
                rhi::SampledTexture{texture, &media_sampler_});
        }
    };
    const auto queue_composite = [&](const MeshRange& range,
                                     const scene::MeshInstance& instance,
                                     const rhi::Texture* texture,
                                     const std::array<const rhi::Texture*, 3U>&
                                         material_textures,
                                     float depth,
                                     doc::NodeId source) {
        if (held_instances() >= limits_.max_instances) {
            ++stats_.dropped_instances;
            return false;
        }
        CompositeDraw draw{};
        draw.range = range;
        draw.instance = instance;
        draw.material_textures = material_textures;
        draw.texture = texture;
        draw.blend = valid_blend(instance.style.blend);
        draw.depth = depth;
        draw.source_order = source.value();
        draw.ordinal = next_ordinal;
        ++next_ordinal;
        composite_draws_.push_back(draw);
        return true;
    };

    // Opaque instances retain the mesh-grouped fast path. Composited instances
    // are only queued here, so they do not interrupt the contiguous opaque
    // instance slots even when the evaluated scene interleaves the two.
    const MeshRange* open_range = nullptr;
    core::u32 open_first_instance = 0U;
    core::u32 open_count = 0U;
    scene::MeshKey open_key{};

    const auto close_batch = [&]() {
        if (open_range == nullptr || open_count == 0U) {
            return;
        }
        batches_.push_back(
            rhi::DrawBatch{open_range->index_count, open_count});
        append_material_textures(fallback_material_textures);
        append(
            batch_uniforms_,
            BatchConstants{open_range->first_index, open_first_instance, 0U, 0U});
        stats_.triangles += (open_range->index_count / 3U) * open_count;
        open_range = nullptr;
        open_count = 0U;
    };

    for (const auto& instance : scene.instances) {
        if (opacity(instance.style) <= 0.0F) {
            continue;
        }
        if (held_instances() >= limits_.max_instances) {
            ++stats_.dropped_instances;
            continue;
        }
        const auto key = scene::normalized(instance.mesh);
        if (!ensure_resident(key)) {
            ++stats_.dropped_instances;
            continue;
        }
        const auto found = resident_.find(key);
        if (found == resident_.end() || found->second.index_count == 0U) {
            ++stats_.dropped_instances;
            continue;
        }

        if (needs_compositing(instance.style)) {
            queue_composite(
                found->second,
                instance,
                nullptr,
                fallback_material_textures,
                camera_depth(instance.world, scene::Vec3{}, render_view.eye,
                             render_view.forward),
                instance.source);
            continue;
        }

        if (open_range == nullptr || !(key == open_key)) {
            close_batch();
            open_key = key;
            open_range = &found->second;
            open_first_instance = stats_.instances;
        }
        append(instance_scratch_, to_gpu(instance));
        ++open_count;
        ++stats_.instances;
    }
    close_batch();

    // Imported models upload once per asset while retaining a batch per source
    // material part. Their transformed asset-bounds centre supplies one stable
    // sort depth for every part of an instance.
    for (const auto& model : scene.models) {
        if (opacity(model.style) <= 0.0F) {
            continue;
        }
        const std::string& model_source = model.render_source();
        if (model_source.empty() ||
            !ensure_model_resident(model_source, commands)) {
            ++stats_.dropped_instances;
            continue;
        }
        const auto found = resident_models_.find(model_source);
        if (found == resident_models_.end() || found->second.parts.empty()) {
            ++stats_.dropped_instances;
            continue;
        }
        const auto local_center =
            (found->second.bounds.minimum + found->second.bounds.maximum) * 0.5F;
        const float depth = camera_depth(model.world, local_center,
                                         render_view.eye,
                                         render_view.forward);
        const auto texture_for = [&](std::optional<core::u32> index)
            -> const rhi::Texture* {
            if (index && *index < found->second.textures.size()) {
                const auto& texture = found->second.textures[*index];
                if (texture) {
                    return &texture;
                }
            }
            return &material_fallback_;
        };
        for (const auto& part : found->second.parts) {
            if (held_instances() >= limits_.max_instances) {
                ++stats_.dropped_instances;
                continue;
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
            proxy.material = part.has_material ? part.material : model.material;
            proxy.source = model.source_node;
            proxy.style = model.style;
            const std::array<const rhi::Texture*, 3U> material_textures{
                texture_for(part.base_color_texture),
                texture_for(part.metallic_roughness_texture),
                texture_for(part.emissive_texture)};

            if (needs_compositing(model.style)) {
                queue_composite(
                    part.range,
                    proxy,
                    nullptr,
                    material_textures,
                    depth,
                    model.source_node);
                continue;
            }

            const auto slot = stats_.instances;
            append(instance_scratch_, to_gpu(proxy));
            batches_.push_back(rhi::DrawBatch{part.range.index_count, 1U});
            append_material_textures(material_textures);
            append(
                batch_uniforms_,
                BatchConstants{part.range.first_index, slot, 0U, 0U});
            stats_.triangles += part.range.index_count / 3U;
            ++stats_.instances;
        }
    }

    // A media frame can contain alpha even when its layer opacity is one, so it
    // always joins the compositing queue. That is the only honest treatment
    // without asset metadata guaranteeing every texel is opaque.
    media_cache_.begin_frame();
    for (const auto& clip : scene.media) {
        if (opacity(clip.style) <= 0.0F) {
            continue;
        }
        if (held_instances() >= limits_.max_instances) {
            ++stats_.dropped_instances;
            continue;
        }
        const auto key = scene::normalized(clip.mesh);
        if (!ensure_resident(key)) {
            continue;
        }
        const auto found = resident_.find(key);
        if (found == resident_.end() || found->second.index_count == 0U) {
            continue;
        }
        const std::string& media_source = clip.render_source();
        if (media_source.empty()) {
            continue;
        }
        const auto* const texture = media_cache_.resolve(
            *device_, commands, media_source, clip.frame_index);
        if (texture == nullptr) {
            // The file could not be loaded: show nothing for this clip rather
            // than failing the whole frame.
            continue;
        }

        scene::MeshInstance proxy{};
        proxy.mesh = clip.mesh;
        proxy.world = clip.world;
        proxy.normal = clip.normal;
        proxy.source = clip.source_node;
        proxy.style = clip.style;
        queue_composite(
            found->second,
            proxy,
            texture,
            fallback_material_textures,
            camera_depth(clip.world, scene::Vec3{}, render_view.eye,
                         render_view.forward),
            clip.source_node);
    }
    media_cache_.end_frame();

    // Back-to-front for colour correctness, then document identity and the
    // collection ordinal for a total order. The latter two make equal-depth
    // layers deterministic even though procedural instances arrived grouped
    // by mesh rather than by document order.
    std::sort(
        composite_draws_.begin(),
        composite_draws_.end(),
        [](const CompositeDraw& left, const CompositeDraw& right) noexcept {
            if (left.depth != right.depth) {
                return left.depth > right.depth;
            }
            if (left.source_order != right.source_order) {
                return left.source_order < right.source_order;
            }
            return left.ordinal < right.ordinal;
        });
    for (auto& draw : composite_draws_) {
        draw.first_instance = stats_.instances;
        append(instance_scratch_, to_gpu(draw.instance));
        stats_.triangles += draw.range.index_count / 3U;
        ++stats_.instances;
        if (draw.texture != nullptr) {
            ++stats_.media;
        }
    }

    stats_.batches = static_cast<core::u32>(
        batches_.size() + composite_draws_.size());
    stats_.meshes_resident = static_cast<core::u32>(
        resident_.size() + resident_models_.size());

    const auto push_light = [&](const float vector[3U],
                                float kind,
                                scene::Vec3 color,
                                float intensity,
                                float range) {
        const auto held = static_cast<core::u32>(light_scratch_.size() / sizeof(GpuLight));
        if (held >= limits_.max_lights) {
            ++stats_.dropped_lights;
            return;
        }
        GpuLight record{};
        record.vector_kind[0U] = vector[0U];
        record.vector_kind[1U] = vector[1U];
        record.vector_kind[2U] = vector[2U];
        record.vector_kind[3U] = kind;
        record.color_intensity[0U] = color.x;
        record.color_intensity[1U] = color.y;
        record.color_intensity[2U] = color.z;
        record.color_intensity[3U] = intensity;
        record.params[0U] = range;
        append(light_scratch_, record);
    };

    for (const auto& light : scene.directional_lights) {
        const float vector[3U]{light.direction.x, light.direction.y, light.direction.z};
        push_light(vector, 0.0F, light.color, light.intensity, 0.0F);
    }
    for (const auto& light : scene.point_lights) {
        const float vector[3U]{light.position.x, light.position.y, light.position.z};
        push_light(vector, 1.0F, light.color, light.intensity, light.range);
    }

    if (geometry_dirty_) {
        if (!commands.upload(vertex_buffer_, vertex_arena_)
            || !commands.upload(index_buffer_, index_arena_)) {
            return false;
        }
        geometry_dirty_ = false;
        stats_.geometry_uploaded = true;
        stats_.upload_bytes += vertex_arena_.size() + index_arena_.size();
    }
    if (!commands.upload(instance_buffer_, instance_scratch_)
        || !commands.upload(light_buffer_, light_scratch_)) {
        return false;
    }
    stats_.upload_bytes += instance_scratch_.size() + light_scratch_.size();

    ViewConstants view_constants{};
    for (core::u32 index = 0U; index < 16U; ++index) {
        view_constants.view_projection[index] =
            render_view.matrices.view_projection.m[index];
    }

    SceneConstants scene_constants{};
    scene_constants.eye_position[0U] = render_view.eye.x;
    scene_constants.eye_position[1U] = render_view.eye.y;
    scene_constants.eye_position[2U] = render_view.eye.z;
    scene_constants.eye_position[3U] = 1.0F;
    scene_constants.sky_ambient[0U] = scene.environment.sky_color.x;
    scene_constants.sky_ambient[1U] = scene.environment.sky_color.y;
    scene_constants.sky_ambient[2U] = scene.environment.sky_color.z;
    scene_constants.sky_ambient[3U] = scene.environment.ambient;
    scene_constants.ground_exposure[0U] = scene.environment.ground_color.x;
    scene_constants.ground_exposure[1U] = scene.environment.ground_color.y;
    scene_constants.ground_exposure[2U] = scene.environment.ground_color.z;
    // Exposure is fixed at one until there is a camera model to derive it
    // from. It belongs with lumens and shutter angle, not with the surround.
    scene_constants.ground_exposure[3U] = 1.0F;
    scene_constants.counts[0U] =
        static_cast<core::u32>(light_scratch_.size() / sizeof(GpuLight));

    const rhi::StorageBuffer* const vertex_bindings[]{
        &vertex_buffer_, &index_buffer_, &instance_buffer_};
    const rhi::StorageBuffer* const fragment_bindings[]{&light_buffer_};

    rhi::PulledDraw draw{};
    draw.pipeline = &pipeline_;
    draw.vertex_buffers = vertex_bindings;
    draw.fragment_buffers = fragment_bindings;
    draw.vertex_uniform = bytes_of(view_constants);
    draw.fragment_uniform = bytes_of(scene_constants);
    draw.batch_uniforms = batch_uniforms_;
    draw.batch_uniform_stride = static_cast<core::u32>(sizeof(BatchConstants));
    draw.batches = batches_;
    draw.fragment_textures = batch_textures_;
    draw.fragment_textures_per_batch = 3U;
    draw.store_depth = !composite_draws_.empty();

    // The ordinary clear is the sky, so an empty scene still reads as a frame.
    // Isolated compositor layers instead retain transparent uncovered pixels.
    // Depth clears to zero because the projection is reversed.
    const rhi::Color clear = options.transparent_background
        ? rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}
        : rhi::Color{
              scene.environment.sky_color.x,
              scene.environment.sky_color.y,
              scene.environment.sky_color.z,
              1.0F};
    if (!commands.draw_pulled(color_, clear, depth_, 0.0F, draw)) {
        return false;
    }

    // Destination-reading layers are intentionally one batch per pass. That
    // preserves their sorted order while allowing every layer to choose its
    // own fixed-function blend equation and, for media, its own texture.
    for (std::size_t index = 0U; index < composite_draws_.size(); ++index) {
        const auto& composite = composite_draws_[index];
        const rhi::DrawBatch composite_batch{
            composite.range.index_count, 1U};
        const BatchConstants composite_offsets{
            composite.range.first_index,
            composite.first_instance,
            0U,
            0U};
        rhi::PulledDraw composite_draw{};
        const auto pipeline_index = blend_index(composite.blend);
        composite_draw.pipeline = composite.texture != nullptr
            ? &media_blended_pipelines_[pipeline_index]
            : &blended_pipelines_[pipeline_index];
        composite_draw.vertex_buffers = vertex_bindings;
        composite_draw.vertex_uniform = bytes_of(view_constants);
        std::array<rhi::SampledTexture, 3U> material_bindings{};
        if (composite.texture == nullptr) {
            material_bindings = std::array<rhi::SampledTexture, 3U>{
                rhi::SampledTexture{
                    composite.material_textures[0U], &media_sampler_},
                rhi::SampledTexture{
                    composite.material_textures[1U], &media_sampler_},
                rhi::SampledTexture{
                    composite.material_textures[2U], &media_sampler_}};
            composite_draw.fragment_textures = material_bindings;
            composite_draw.fragment_buffers = fragment_bindings;
            composite_draw.fragment_uniform = bytes_of(scene_constants);
        } else {
            composite_draw.texture = composite.texture;
            composite_draw.sampler = &media_sampler_;
        }
        composite_draw.batch_uniforms = bytes_of(composite_offsets);
        composite_draw.batch_uniform_stride =
            static_cast<core::u32>(sizeof(BatchConstants));
        composite_draw.batches =
            std::span<const rhi::DrawBatch>{&composite_batch, 1U};
        composite_draw.load = true;
        composite_draw.store_depth = index + 1U < composite_draws_.size();
        if (!commands.draw_pulled(
                color_, clear, depth_, 0.0F, composite_draw)) {
            return false;
        }
    }
    return true;
}

} // namespace mediaserver::render
