#include <mediaserver/render/ambient_occlusion.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/ambient_occlusion_comp_dxil.h>
#endif

#include <array>
#include <cstring>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using CreateResult = core::Result<AmbientOcclusion, RenderError>;

[[nodiscard]] bool shader_present() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return true;
#else
    return false;
#endif
}

[[nodiscard]] std::span<const core::u8> shader_bytecode() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(
            mediaserver_ambient_occlusion_comp_dxil),
        sizeof(mediaserver_ambient_occlusion_comp_dxil)};
#else
    return {};
#endif
}

[[nodiscard]] rhi::TextureDesc occlusion_desc(core::u32 width,
                                              core::u32 height) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::R32Float;
    desc.sampled = true;
    desc.storage_write = true;
    return desc;
}

// Matches `numthreads(8, 8, 1)` in the shader. Changing one without the other
// either leaves a strip of the target unwritten or dispatches groups that do
// nothing, so they are named together here.
constexpr core::u32 occlusion_group_size = 8U;

[[nodiscard]] core::u32 groups_for(core::u32 extent) noexcept {
    return (extent + occlusion_group_size - 1U) / occlusion_group_size;
}

struct OcclusionConstants final {
    float inverse_view_projection[16U]{};
    float extent[4U]{};
    float parameters[4U]{};
};

[[nodiscard]] std::span<const core::u8> bytes_of(
    const OcclusionConstants& value) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&value), sizeof(value)};
}

} // namespace

bool AmbientOcclusion::shaders_available() noexcept {
    return shader_present();
}

AmbientOcclusion::AmbientOcclusion(
    rhi::Texture occlusion, rhi::ComputePipeline pipeline,
    rhi::Sampler sampler) noexcept
    : occlusion_(std::move(occlusion)), pipeline_(std::move(pipeline)),
      sampler_(std::move(sampler)) {}

CreateResult AmbientOcclusion::create(
    rhi::Device& device, core::u32 width, core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return CreateResult::failure(RenderError::InvalidConfiguration);
    }
    if (!shader_present()) {
        return CreateResult::failure(RenderError::ShaderFormatUnsupported);
    }
    if (!device.capabilities().compute
        || !device.capabilities().storage_images) {
        return CreateResult::failure(RenderError::CapabilityUnsupported);
    }

    auto occlusion = device.create_texture(occlusion_desc(width, height));
    if (!occlusion) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }

    rhi::ShaderDesc compute{};
    compute.stage = rhi::ShaderStage::Compute;
    compute.format = rhi::ShaderFormat::Dxil;
    compute.bytecode = shader_bytecode();
    compute.sampler_count = 2U;
    compute.storage_texture_count = 1U;
    compute.uniform_buffer_count = 1U;
    auto pipeline = device.create_compute_pipeline(compute);
    if (!pipeline) {
        return CreateResult::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }

    auto sampler = device.create_sampler(rhi::Filter::Nearest);
    if (!sampler) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }

    return CreateResult::success(AmbientOcclusion{
        std::move(occlusion).value(), std::move(pipeline).value(),
        std::move(sampler).value()});
}

bool AmbientOcclusion::resize(
    rhi::Device& device, core::u32 width, core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (occlusion_.width() == width && occlusion_.height() == height) {
        return true;
    }
    auto occlusion = device.create_texture(occlusion_desc(width, height));
    if (!occlusion) {
        return false;
    }
    occlusion_ = std::move(occlusion).value();
    return true;
}

bool AmbientOcclusion::record(
    rhi::CommandList& commands, const rhi::Texture& depth,
    const rhi::Texture& normal, const scene::Mat4& inverse_view_projection,
    scene::CanvasRect slice, core::u64 show_frame, float radius,
    float strength) noexcept {
    graph_.begin();
    if (depth.width() != occlusion_.width()
        || depth.height() != occlusion_.height()
        || normal.width() != occlusion_.width()
        || normal.height() != occlusion_.height()) {
        return false;
    }

    OcclusionConstants constants{};
    for (core::u32 index = 0U; index < 16U; ++index) {
        constants.inverse_view_projection[index] =
            inverse_view_projection.m[index];
    }
    constants.extent[0U] = static_cast<float>(occlusion_.width());
    constants.extent[1U] = static_cast<float>(occlusion_.height());
    // Absolute canvas origin, so the kernel rotation below is a property of
    // the wall pixel rather than of which node happens to own it.
    constants.extent[2U] = static_cast<float>(slice.x);
    constants.extent[3U] = static_cast<float>(slice.y);
    constants.parameters[0U] = radius;
    constants.parameters[1U] = strength;
    // The show frame in two halves, because the constant buffer is floats and
    // a 64-bit frame does not survive one. The shader reads them back as bits,
    // never as numbers, so no precision is lost on the way.
    const auto low = static_cast<core::u32>(show_frame & 0xFFFF'FFFFULL);
    const auto high = static_cast<core::u32>(show_frame >> 32U);
    std::memcpy(&constants.parameters[2U], &low, sizeof(low));
    std::memcpy(&constants.parameters[3U], &high, sizeof(high));

    const std::array<rhi::SampledTexture, 2U> sources{{
        rhi::SampledTexture{&depth, &sampler_},
        rhi::SampledTexture{&normal, &sampler_},
    }};
    const std::array<rhi::StorageTexture, 1U> targets{
        rhi::StorageTexture{&occlusion_, 0U, 0U, true}};
    rhi::ComputeDispatch work{};
    work.pipeline = &pipeline_;
    work.sampled_textures = sources;
    work.storage_textures = targets;
    work.uniform = bytes_of(constants);
    work.groups_x = groups_for(occlusion_.width());
    work.groups_y = groups_for(occlusion_.height());
    if (!graph_.dispatch(commands, core::RenderPass::Occlusion, work)) {
        return false;
    }
    ++stats_.dispatches;
    return true;
}

} // namespace mediaserver::render
