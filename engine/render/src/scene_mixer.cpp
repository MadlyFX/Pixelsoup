#include <mediaserver/render/scene_mixer.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/output_vert_dxil.h>
#include <mediaserver/shaders/scene_mix_frag_dxil.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using Result = core::Result<SceneMixer, RenderError>;

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
            reinterpret_cast<const core::u8*>(mediaserver_scene_mix_frag_dxil),
            sizeof(mediaserver_scene_mix_frag_dxil)},
        rhi::ShaderFormat::Dxil, true};
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

struct alignas(16) MixUniform final {
    float amount{};
    std::array<float, 3U> padding{};
};

static_assert(sizeof(MixUniform) == 16U);

[[nodiscard]] std::span<const core::u8> bytes(const MixUniform& value) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&value), sizeof(value)};
}

} // namespace

bool SceneMixer::shaders_available() noexcept { return shader_blobs().present; }

SceneMixer::SceneMixer(rhi::Texture color, rhi::GraphicsPipeline pipeline,
                       rhi::Sampler sampler) noexcept
    : color_(std::move(color)), pipeline_(std::move(pipeline)),
      sampler_(std::move(sampler)) {}

Result SceneMixer::create(rhi::Device& device, core::u32 width,
                          core::u32 height, rhi::TextureFormat format) {
    const ShaderBlobs blobs = shader_blobs();
    if (!blobs.present || !device.supports(blobs.format)) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }
    if (width == 0U || height == 0U) {
        return Result::failure(RenderError::InvalidConfiguration);
    }
    auto color = device.create_texture(color_desc(width, height, format));
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
    fragment.sampler_count = 2U;
    fragment.uniform_buffer_count = 1U;
    auto pipeline = device.create_pipeline(
        vertex, fragment, format, false,
        rhi::DepthState{});
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
    return Result::success(SceneMixer{std::move(color).value(),
                                      std::move(pipeline).value(),
                                      std::move(sampler).value()});
}

bool SceneMixer::resize(rhi::Device& device, core::u32 width,
                        core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (color_.width() == width && color_.height() == height) {
        return true;
    }
    auto color = device.create_texture(
        color_desc(width, height, color_.desc().format));
    if (!color) {
        return false;
    }
    color_ = std::move(color).value();
    return true;
}

bool SceneMixer::render(rhi::CommandList& commands,
                        const rhi::Texture& from,
                        const rhi::Texture& to,
                        float amount) noexcept {
    const MixUniform uniform{std::clamp(amount, 0.0F, 1.0F), {}};
    const std::array<rhi::SampledTexture, 2U> sources{{
        rhi::SampledTexture{&from, &sampler_},
        rhi::SampledTexture{&to, &sampler_},
    }};
    return commands.draw_output_transform(
        color_, rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, pipeline_, sources,
        bytes(uniform));
}

} // namespace mediaserver::render
