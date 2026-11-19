#include <mediaserver/render/projection_mesh_warp.hpp>

#include <mediaserver/core/idle_report.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/projection_mesh_warp_frag_dxil.h>
#include <mediaserver/shaders/projection_mesh_warp_vert_dxil.h>
#endif

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using Result = core::Result<ProjectionMeshWarp, RenderError>;

struct ShaderBlobs final {
    std::span<const core::u8> vertex;
    std::span<const core::u8> fragment;
    rhi::ShaderFormat format{rhi::ShaderFormat::Dxil};
    bool present{};
};

[[nodiscard]] ShaderBlobs shader_blobs() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return ShaderBlobs{
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_projection_mesh_warp_vert_dxil),
            sizeof(mediaserver_projection_mesh_warp_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_projection_mesh_warp_frag_dxil),
            sizeof(mediaserver_projection_mesh_warp_frag_dxil)},
        rhi::ShaderFormat::Dxil,
        true,
    };
#else
    return ShaderBlobs{};
#endif
}

[[nodiscard]] rhi::TextureDesc color_desc(core::u32 width,
                                          core::u32 height,
                                          rhi::TextureFormat format) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.color_target = true;
    desc.sampled = true;
    return desc;
}

[[nodiscard]] rhi::TextureDesc depth_desc(core::u32 width,
                                          core::u32 height) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Depth32Float;
    desc.depth_target = true;
    return desc;
}

template <typename T>
[[nodiscard]] std::span<const core::u8>
bytes_of(std::span<const T> values) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(values.data()),
        values.size_bytes()};
}

[[nodiscard]] bool valid_limits(ProjectionMeshWarpLimits limits) noexcept {
    constexpr auto maximum =
        static_cast<std::size_t>(std::numeric_limits<core::u32>::max());
    return limits.max_vertices != 0U && limits.max_indices != 0U
        && static_cast<std::size_t>(limits.max_vertices)
                * sizeof(output::MeshWarpVertex)
            <= maximum
        && static_cast<std::size_t>(limits.max_indices) * sizeof(core::u32)
            <= maximum;
}

struct BatchConstants final {
    core::u32 first_index{};
    core::u32 unused0{};
    core::u32 unused1{};
    core::u32 unused2{};
};

struct PassConstants final {
    core::u32 unused[4]{};
};

static_assert(sizeof(output::MeshWarpVertex) == 32U);
static_assert(sizeof(BatchConstants) == 16U);

} // namespace

bool ProjectionMeshWarp::shaders_available() noexcept {
    return shader_blobs().present;
}

ProjectionMeshWarp::ProjectionMeshWarp(
    ProjectionMeshWarpLimits limits, rhi::Texture color, rhi::Texture depth,
    rhi::GraphicsPipeline pipeline, rhi::Sampler sampler,
    rhi::StorageBuffer vertices, rhi::StorageBuffer indices) noexcept
    : limits_(limits), color_(std::move(color)), depth_(std::move(depth)),
      pipeline_(std::move(pipeline)), sampler_(std::move(sampler)),
      vertices_(std::move(vertices)), indices_(std::move(indices)) {}

Result ProjectionMeshWarp::create(rhi::Device& device, core::u32 width,
                                  core::u32 height,
                                  ProjectionMeshWarpLimits limits,
                                  rhi::TextureFormat format) {
    const ShaderBlobs blobs = shader_blobs();
    if (!blobs.present || !device.supports(blobs.format)) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }
    if (width == 0U || height == 0U || !valid_limits(limits)) {
        return Result::failure(RenderError::InvalidConfiguration);
    }

    auto color = device.create_texture(color_desc(width, height, format));
    auto depth = device.create_texture(depth_desc(width, height));
    auto vertices = device.create_storage_buffer(
        limits.max_vertices *
        static_cast<core::u32>(sizeof(output::MeshWarpVertex)));
    auto indices = device.create_storage_buffer(
        limits.max_indices * static_cast<core::u32>(sizeof(core::u32)));
    if (!color || !depth || !vertices || !indices) {
        return Result::failure(RenderError::AllocationFailed);
    }

    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = blobs.format;
    vertex.bytecode = blobs.vertex;
    vertex.storage_buffer_count = 2U;
    // Batch constants live at b1; the pulled draw vocabulary reserves b0 for
    // pass-wide data even though this shader needs none.
    vertex.uniform_buffer_count = 2U;

    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = blobs.format;
    fragment.bytecode = blobs.fragment;
    fragment.sampler_count = 1U;

    // draw_pulled always binds the warp's depth target, so the pipeline has to
    // declare one to match it (D3D12 checks the DSV format against the bound
    // view). The warp never occludes itself: no test and no writes.
    const rhi::DepthState depth_state{true, false, true, false};
    auto pipeline = device.create_pipeline(
        vertex, fragment, format, rhi::ColorBlend::Disabled, depth_state);
    if (!pipeline) {
        return Result::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }
    auto sampler = device.create_sampler(rhi::Filter::Linear);
    if (!sampler) {
        return Result::failure(RenderError::AllocationFailed);
    }

    return Result::success(ProjectionMeshWarp{
        limits, std::move(color).value(), std::move(depth).value(),
        std::move(pipeline).value(), std::move(sampler).value(),
        std::move(vertices).value(), std::move(indices).value()});
}

bool ProjectionMeshWarp::resize(rhi::Device& device, core::u32 width,
                                core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (color_.width() == width && color_.height() == height) {
        return true;
    }
    auto color = device.create_texture(
        color_desc(width, height, color_.desc().format));
    auto depth = device.create_texture(depth_desc(width, height));
    if (!color || !depth) {
        return false;
    }
    color_ = std::move(color).value();
    depth_ = std::move(depth).value();
    return true;
}

bool ProjectionMeshWarp::render(
    rhi::CommandList& commands, const rhi::Texture& source,
    const output::ProjectionMesh& mesh,
    const output::MeshPerspectiveView& view,
    const output::ProjectionMeshPlacement* placement,
    const output::MeshDestinationWindow* window) noexcept {
    constexpr const char* feature = "projection mesh warp";
    graph_.begin();
    if (!source || !source.desc().sampled) {
        static_cast<void>(core::report_idle_once(
            feature, "the rendered surface view is not a sampled texture"));
        return false;
    }
    if (mesh.vertices.size() == 0U
        || mesh.vertices.size() > limits_.max_vertices
        || mesh.indices.size() == 0U
        || mesh.indices.size() > limits_.max_indices) {
        static_cast<void>(core::report_idle_once(
            feature, "the mesh is empty or exceeds the warp's vertex limits"));
        return false;
    }
    auto resolved = window != nullptr
        ? output::mesh_warp_vertices(mesh, view, placement, *window)
        : placement != nullptr
            ? output::mesh_warp_vertices(mesh, view, *placement)
            : output::mesh_warp_vertices(mesh, view);
    if (!resolved) {
        static_cast<void>(core::report_idle_once(
            feature, "the mesh vertices could not be projected from the eye"));
        return false;
    }
    const std::vector<output::MeshWarpVertex>& warp = resolved.value();
    if (!commands.upload(vertices_, bytes_of(
                                      std::span<const output::MeshWarpVertex>{
                                          warp.data(), warp.size()}))
        || !commands.upload(indices_, bytes_of(
                                         std::span<const core::u32>{
                                             mesh.indices.data(),
                                             mesh.indices.size()}))) {
        static_cast<void>(core::report_idle_once(
            feature, "the GPU refused the mesh vertex or index upload"));
        return false;
    }

    const std::array<const rhi::StorageBuffer*, 2U> buffers{
        &vertices_, &indices_};
    const rhi::DrawBatch batch{
        static_cast<core::u32>(mesh.indices.size()), 1U};
    const BatchConstants constants{};
    // The vertex stage declares b0 for pass-wide data and b1 for the batch.
    // It reads nothing from b0, but a declared slot is always bound, and the
    // D3D12 backend refuses an empty record: without this every warp draw
    // failed and a UV-mesh output held its last picture.
    const PassConstants pass{};
    rhi::PulledDraw draw{};
    draw.pipeline = &pipeline_;
    draw.vertex_uniform = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&pass), sizeof(pass)};
    draw.vertex_buffers = buffers;
    draw.batch_uniforms = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&constants), sizeof(constants)};
    draw.batch_uniform_stride =
        static_cast<core::u32>(sizeof(constants));
    draw.batches = std::span<const rhi::DrawBatch>{&batch, 1U};
    draw.texture = &source;
    draw.sampler = &sampler_;
    if (!graph_.draw_pulled(commands, core::RenderPass::Output,
            color_, rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, depth_, 0.0F, draw)) {
        static_cast<void>(core::report_idle_once(
            feature, "the warp draw could not be recorded"));
        return false;
    }
    return true;
}

} // namespace mediaserver::render
