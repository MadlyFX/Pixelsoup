#include <mediaserver/render/scene_renderer.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/scene_frag_dxil.h>
#include <mediaserver/shaders/scene_media_frag_dxil.h>
#include <mediaserver/shaders/scene_vert_dxil.h>
#endif

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
};

static_assert(sizeof(GpuInstance) == 144U);
static_assert(alignof(GpuInstance) == 4U);

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
    return record;
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
    rhi::GraphicsPipeline media_pipeline,
    rhi::Sampler media_sampler,
    rhi::StorageBuffer vertices,
    rhi::StorageBuffer indices,
    rhi::StorageBuffer instances,
    rhi::StorageBuffer lights) noexcept
    : device_(&device),
      limits_(limits),
      color_(std::move(color)),
      depth_(std::move(depth)),
      pipeline_(std::move(pipeline)),
      media_pipeline_(std::move(media_pipeline)),
      media_sampler_(std::move(media_sampler)),
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
        // Opaque. Sorted transparency is a later pass with its own ordering
        // rules, not a blend state bolted onto this one.
        false,
        depth_state);
    if (!pipeline) {
        return Result::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }

    // The media pass: the same pulled vertex stage, a fragment stage that
    // samples the clip, and the same reversed depth so clips occlude and are
    // occluded by scene geometry.
    rhi::ShaderDesc media_fragment{};
    media_fragment.stage = rhi::ShaderStage::Fragment;
    media_fragment.format = blobs.format;
    media_fragment.bytecode = blobs.media_fragment;
    media_fragment.sampler_count = 1U;

    auto media_pipeline = device.create_pipeline(
        vertex,
        media_fragment,
        rhi::TextureFormat::Rgba8Unorm,
        false,
        depth_state);
    if (!media_pipeline) {
        return Result::failure(
            media_pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }
    // Linear, so a clip at one resolution resamples cleanly onto a quad of any
    // size on screen.
    auto media_sampler = device.create_sampler(rhi::Filter::Linear);
    if (!media_sampler) {
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
        std::move(media_pipeline).value(),
        std::move(media_sampler).value(),
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
    renderer.media_draws_.reserve(scene::max_scene_media);
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

bool SceneRenderer::ensure_resident(scene::MeshKey key) {
    const auto normal_key = scene::normalized(key);
    if (resident_.find(normal_key) != resident_.end()) {
        return true;
    }
    if (!scene::build_mesh(normal_key, mesh_scratch_)) {
        return false;
    }

    const auto vertex_stride = sizeof(scene::MeshVertex);
    const auto resident_vertices =
        static_cast<core::u64>(vertex_arena_.size() / vertex_stride);
    const auto resident_indices =
        static_cast<core::u64>(index_arena_.size() / sizeof(core::u32));

    if (resident_vertices + mesh_scratch_.vertices.size() > limits_.max_vertices
        || resident_indices + mesh_scratch_.indices.size() > limits_.max_indices) {
        // Out of arena. Reported through `dropped_instances` rather than as a
        // failed frame: one over-tessellated object should not black out an
        // output that is otherwise correct.
        return false;
    }

    const auto vertex_offset = vertex_arena_.size();
    vertex_arena_.resize(
        vertex_offset + mesh_scratch_.vertices.size() * vertex_stride);
    std::memcpy(
        vertex_arena_.data() + vertex_offset,
        mesh_scratch_.vertices.data(),
        mesh_scratch_.vertices.size() * vertex_stride);

    const auto first_index = static_cast<core::u32>(resident_indices);
    const auto base_vertex = static_cast<core::u32>(resident_vertices);
    const auto index_offset = index_arena_.size();
    index_arena_.resize(
        index_offset + mesh_scratch_.indices.size() * sizeof(core::u32));
    auto* const written =
        reinterpret_cast<core::u32*>(index_arena_.data() + index_offset);
    for (std::size_t position = 0U; position < mesh_scratch_.indices.size();
         ++position) {
        // Indices are made absolute on the way in, so a batch is a range in
        // the index arena and nothing else - no base-vertex argument, and no
        // per-backend disagreement about whether one is applied.
        written[position] = base_vertex + mesh_scratch_.indices[position];
    }

    resident_.emplace(
        normal_key,
        MeshRange{first_index, static_cast<core::u32>(mesh_scratch_.indices.size())});
    geometry_dirty_ = true;
    return true;
}

bool SceneRenderer::render(
    rhi::CommandList& commands,
    const scene::Scene& scene,
    scene::CanvasRect slice) {
    stats_ = RenderStats{};

    if (!scene::contains(scene.canvas, slice) || !scene::valid(scene.camera)) {
        return false;
    }

    instance_scratch_.clear();
    light_scratch_.clear();
    batch_uniforms_.clear();
    batches_.clear();

    // One batch per run of instances sharing a mesh. The scene arrives sorted
    // by mesh key, so the runs are already contiguous and this is one pass.
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
        append(
            batch_uniforms_,
            BatchConstants{open_range->first_index, open_first_instance, 0U, 0U});
        stats_.triangles += (open_range->index_count / 3U) * open_count;
        open_range = nullptr;
        open_count = 0U;
    };

    for (const auto& instance : scene.instances) {
        if (stats_.instances >= limits_.max_instances) {
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
    stats_.batches = static_cast<core::u32>(batches_.size());

    // Media clips are textured quads drawn after the lit geometry, each in its
    // own pass. Their instances append to the same buffer, after the scene's,
    // and their textures are resolved - and uploaded - here on the frame path.
    media_draws_.clear();
    for (const auto& clip : scene.media) {
        const auto key = scene::normalized(clip.mesh);
        if (!ensure_resident(key)) {
            continue;
        }
        const auto found = resident_.find(key);
        if (found == resident_.end() || found->second.index_count == 0U) {
            continue;
        }
        const auto slot = static_cast<core::u32>(
            instance_scratch_.size() / sizeof(GpuInstance));
        if (slot >= limits_.max_instances) {
            continue;
        }
        const scene::MeshInstance proxy{
            clip.mesh,
            clip.world,
            clip.normal,
            scene::Material{},
            clip.source_node};
        append(instance_scratch_, to_gpu(proxy));
        const auto* const texture = media_cache_.resolve(
            *device_, commands, clip.source, clip.frame_index);
        if (texture == nullptr) {
            // The file could not be loaded: drop the instance just appended and
            // show nothing for this clip rather than fail the whole frame.
            instance_scratch_.resize(
                instance_scratch_.size() - sizeof(GpuInstance));
            continue;
        }
        media_draws_.push_back(MediaDraw{
            found->second.first_index,
            found->second.index_count,
            slot,
            texture});
    }
    stats_.media = static_cast<core::u32>(media_draws_.size());
    stats_.meshes_resident = static_cast<core::u32>(resident_.size());

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

    const auto view = scene::view_for_slice(scene.camera, scene.canvas, slice);
    ViewConstants view_constants{};
    for (core::u32 index = 0U; index < 16U; ++index) {
        view_constants.view_projection[index] = view.view_projection.m[index];
    }

    SceneConstants scene_constants{};
    scene_constants.eye_position[0U] = scene.camera.eye.x;
    scene_constants.eye_position[1U] = scene.camera.eye.y;
    scene_constants.eye_position[2U] = scene.camera.eye.z;
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
    // Keep the depth buffer only when a media pass will depth-test against it.
    draw.store_depth = !media_draws_.empty();

    // The clear is the sky, so a frame with nothing in it still reads as a
    // frame rather than as a dead output. Depth clears to zero because the
    // projection is reversed.
    const rhi::Color sky{
        scene.environment.sky_color.x,
        scene.environment.sky_color.y,
        scene.environment.sky_color.z,
        1.0F};
    if (!commands.draw_pulled(color_, sky, depth_, 0.0F, draw)) {
        return false;
    }

    // Each clip composites over the lit scene in its own pass, sharing the view
    // and depth-testing against the geometry the first pass wrote.
    for (const auto& media : media_draws_) {
        const rhi::DrawBatch media_batch{media.index_count, 1U};
        const BatchConstants media_offsets{
            media.first_index, media.first_instance, 0U, 0U};
        rhi::PulledDraw media_draw{};
        media_draw.pipeline = &media_pipeline_;
        media_draw.vertex_buffers = vertex_bindings;
        media_draw.vertex_uniform = bytes_of(view_constants);
        media_draw.batch_uniforms = bytes_of(media_offsets);
        media_draw.batch_uniform_stride =
            static_cast<core::u32>(sizeof(BatchConstants));
        media_draw.batches = std::span<const rhi::DrawBatch>{&media_batch, 1U};
        media_draw.texture = media.texture;
        media_draw.sampler = &media_sampler_;
        media_draw.load = true;
        media_draw.store_depth = true;
        if (!commands.draw_pulled(color_, sky, depth_, 0.0F, media_draw)) {
            return false;
        }
    }
    return true;
}

} // namespace mediaserver::render
