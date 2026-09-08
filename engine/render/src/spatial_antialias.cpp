#include <mediaserver/render/spatial_antialias.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/output_vert_dxil.h>
#include <mediaserver/shaders/spatial_antialias_frag_dxil.h>
#endif

#include <array>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using CreateResult = core::Result<SpatialAntialias, RenderError>;
using ApplyResult = core::Result<const rhi::Texture*, RenderError>;

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
            reinterpret_cast<const core::u8*>(mediaserver_output_vert_dxil),
            sizeof(mediaserver_output_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(
                mediaserver_spatial_antialias_frag_dxil),
            sizeof(mediaserver_spatial_antialias_frag_dxil)},
        rhi::ShaderFormat::Dxil, true};
#else
    return ShaderBlobs{};
#endif
}

[[nodiscard]] rhi::TextureDesc target_desc(core::u32 width,
                                           core::u32 height) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba16Float;
    desc.color_target = true;
    desc.sampled = true;
    return desc;
}

struct SpatialConstants final {
    float parameters[4U]{};
};

[[nodiscard]] std::span<const core::u8> bytes_of(
    const SpatialConstants& value) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&value), sizeof(value)};
}

} // namespace

bool SpatialAntialias::shaders_available() noexcept {
    return shader_blobs().present;
}

SpatialAntialias::SpatialAntialias(
    rhi::Texture output, rhi::GraphicsPipeline pipeline,
    rhi::Sampler sampler) noexcept
    : output_(std::move(output)), pipeline_(std::move(pipeline)),
      sampler_(std::move(sampler)) {}

CreateResult SpatialAntialias::create(
    rhi::Device& device, core::u32 width, core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return CreateResult::failure(RenderError::InvalidConfiguration);
    }
    const ShaderBlobs blobs = shader_blobs();
    if (!blobs.present) {
        return CreateResult::failure(RenderError::ShaderFormatUnsupported);
    }

    auto output = device.create_texture(target_desc(width, height));
    if (!output) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }

    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = blobs.format;
    vertex.bytecode = blobs.vertex;

    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = blobs.format;
    fragment.bytecode = blobs.fragment;
    fragment.sampler_count = 1U;
    fragment.uniform_buffer_count = 1U;

    auto pipeline = device.create_pipeline(
        vertex, fragment, rhi::TextureFormat::Rgba16Float, false,
        rhi::DepthState{});
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

    return CreateResult::success(SpatialAntialias{
        std::move(output).value(), std::move(pipeline).value(),
        std::move(sampler).value()});
}

bool SpatialAntialias::resize(
    rhi::Device& device, core::u32 width, core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (output_.width() == width && output_.height() == height) {
        return true;
    }
    auto output = device.create_texture(target_desc(width, height));
    if (!output) {
        return false;
    }
    output_ = std::move(output).value();
    return true;
}

ApplyResult SpatialAntialias::apply(
    rhi::CommandList& commands, const rhi::Texture& source) noexcept {
    if (source.width() != output_.width()
        || source.height() != output_.height()) {
        return ApplyResult::failure(RenderError::InvalidConfiguration);
    }

    SpatialConstants constants{};
    constants.parameters[0U] = static_cast<float>(output_.width());
    constants.parameters[1U] = static_cast<float>(output_.height());
    constants.parameters[2U] = spatial_edge_threshold;
    constants.parameters[3U] = spatial_edge_floor;

    const std::array<rhi::SampledTexture, 1U> inputs{
        rhi::SampledTexture{&source, &sampler_}};
    if (!commands.draw_output_transform(
            output_, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}, pipeline_, inputs,
            bytes_of(constants))) {
        return ApplyResult::failure(RenderError::BackendFailure);
    }
    ++stats_.passes;
    return ApplyResult::success(&output_);
}

} // namespace mediaserver::render
