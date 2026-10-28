#include <mediaserver/render/inspection_view.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/inspection_view_frag_dxil.h>
#include <mediaserver/shaders/output_vert_dxil.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using Result = core::Result<InspectionView, RenderError>;

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
                mediaserver_inspection_view_frag_dxil),
            sizeof(mediaserver_inspection_view_frag_dxil)},
        rhi::ShaderFormat::Dxil, true};
#else
    return ShaderBlobs{};
#endif
}

[[nodiscard]] rhi::TextureDesc color_desc(core::u32 width,
                                          core::u32 height) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.color_target = true;
    desc.sampled = true;
    return desc;
}

struct alignas(16) InspectionUniform final {
    core::u32 channel{};
    float gain{1.0F};
    std::array<float, 2U> padding{};
};

static_assert(sizeof(InspectionUniform) == 16U);

[[nodiscard]] std::span<const core::u8>
bytes(const InspectionUniform& value) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&value), sizeof(value)};
}

} // namespace

bool InspectionView::shaders_available() noexcept {
    return shader_blobs().present;
}

InspectionView::InspectionView(rhi::Texture color,
                               rhi::GraphicsPipeline pipeline,
                               rhi::Sampler sampler) noexcept
    : color_(std::move(color)), pipeline_(std::move(pipeline)),
      sampler_(std::move(sampler)) {}

Result InspectionView::create(rhi::Device& device, core::u32 width,
                              core::u32 height) {
    const ShaderBlobs blobs = shader_blobs();
    if (!blobs.present || !device.supports(blobs.format)) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }
    if (width == 0U || height == 0U) {
        return Result::failure(RenderError::InvalidConfiguration);
    }
    auto color = device.create_texture(color_desc(width, height));
    if (!color) {
        return Result::failure(RenderError::AllocationFailed);
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
        vertex, fragment, rhi::TextureFormat::Rgba8Unorm, false,
        rhi::DepthState{});
    if (!pipeline) {
        return Result::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }
    // Same-size input and output: nearest reads exactly the pixel under each
    // output pixel, so the pass never softens what it inspects.
    auto sampler = device.create_sampler(rhi::Filter::Nearest);
    if (!sampler) {
        return Result::failure(RenderError::AllocationFailed);
    }
    return Result::success(InspectionView{std::move(color).value(),
                                          std::move(pipeline).value(),
                                          std::move(sampler).value()});
}

bool InspectionView::resize(rhi::Device& device, core::u32 width,
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
    color_ = std::move(color).value();
    return true;
}

bool InspectionView::render(rhi::CommandList& commands,
                            const rhi::Texture& image,
                            InspectionChannel channel, float stops) noexcept {
    graph_.begin();
    const float clamped =
        std::isfinite(stops) ? std::clamp(stops, -16.0F, 16.0F) : 0.0F;
    const InspectionUniform uniform{static_cast<core::u32>(channel),
                                    std::exp2(clamped), {}};
    const std::array<rhi::SampledTexture, 1U> sources{{
        rhi::SampledTexture{&image, &sampler_},
    }};
    return graph_.fullscreen(commands, core::RenderPass::Composite, color_,
                             rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, pipeline_,
                             sources, bytes(uniform));
}

} // namespace mediaserver::render
