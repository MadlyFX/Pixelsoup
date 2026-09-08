#include <mediaserver/render/gaussian_blur.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/gaussian_blur_frag_dxil.h>
#include <mediaserver/shaders/output_vert_dxil.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using CreateResult = core::Result<GaussianBlur, RenderError>;

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
                mediaserver_gaussian_blur_frag_dxil),
            sizeof(mediaserver_gaussian_blur_frag_dxil)},
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

struct alignas(16) BlurUniform final {
    float texel_x{};
    float texel_y{};
    float radius_px{};
    float sigma_px{};
    float half_taps{};
    float padding[3U]{};
};

static_assert(sizeof(BlurUniform) == 32U);

[[nodiscard]] std::span<const core::u8>
bytes(const BlurUniform& uniform) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&uniform), sizeof(uniform)};
}

} // namespace

GaussianKernel kernel_of(const scene::EffectSettings& settings) noexcept {
    const scene::EffectSettings valid = scene::clamped(settings);
    return GaussianKernel{valid.blur_taps, valid.blur_sigma_ratio,
                          valid.blur_passes};
}

bool GaussianBlur::shaders_available() noexcept {
    return shader_blobs().present;
}

GaussianBlur::GaussianBlur(rhi::Texture horizontal, rhi::Texture vertical,
                           rhi::GraphicsPipeline pipeline,
                           rhi::Sampler sampler) noexcept
    : horizontal_(std::move(horizontal)), vertical_(std::move(vertical)),
      pipeline_(std::move(pipeline)), sampler_(std::move(sampler)) {}

CreateResult GaussianBlur::create(rhi::Device& device, core::u32 width,
                                  core::u32 height,
                                  rhi::TextureFormat format) {
    const ShaderBlobs blobs = shader_blobs();
    if (!blobs.present || !device.supports(blobs.format)) {
        return CreateResult::failure(RenderError::ShaderFormatUnsupported);
    }
    if (width == 0U || height == 0U) {
        return CreateResult::failure(RenderError::InvalidConfiguration);
    }

    auto horizontal = device.create_texture(color_desc(width, height, format));
    if (!horizontal) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }
    auto vertical = device.create_texture(color_desc(width, height, format));
    if (!vertical) {
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
        vertex, fragment, format, false,
        rhi::DepthState{});
    if (!pipeline) {
        return CreateResult::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }
    auto sampler = device.create_sampler(rhi::Filter::Linear);
    if (!sampler) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }

    return CreateResult::success(GaussianBlur{
        std::move(horizontal).value(), std::move(vertical).value(),
        std::move(pipeline).value(), std::move(sampler).value()});
}

bool GaussianBlur::resize(rhi::Device& device, core::u32 width,
                          core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (horizontal_.width() == width && horizontal_.height() == height &&
        vertical_.width() == width && vertical_.height() == height) {
        return true;
    }

    const auto format = horizontal_.desc().format;
    auto horizontal = device.create_texture(color_desc(width, height, format));
    if (!horizontal) {
        return false;
    }
    auto vertical = device.create_texture(color_desc(width, height, format));
    if (!vertical) {
        return false;
    }
    horizontal_ = std::move(horizontal).value();
    vertical_ = std::move(vertical).value();
    return true;
}

GaussianBlurResult GaussianBlur::apply(rhi::CommandList& commands,
                                       const rhi::Texture& source,
                                       float radius_px,
                                       const GaussianKernel& kernel) noexcept {
    if (!source || !source.desc().sampled ||
        source.desc().format == rhi::TextureFormat::Depth32Float ||
        !std::isfinite(radius_px)) {
        return GaussianBlurResult::failure(RenderError::InvalidConfiguration);
    }
    if (radius_px <= 0.0F) {
        return GaussianBlurResult::success(&source);
    }
    if (source.width() != width() || source.height() != height()) {
        return GaussianBlurResult::failure(RenderError::InvalidConfiguration);
    }

    // The kernel arrives from a document, so it is re-clamped here rather than
    // trusted: the fragment stage's loop bound is not something a project file
    // gets to choose.
    scene::EffectSettings authored_settings{};
    authored_settings.blur_taps = kernel.taps;
    authored_settings.blur_sigma_ratio = kernel.sigma_ratio;
    authored_settings.blur_passes = kernel.passes;
    const scene::EffectSettings settings =
        scene::clamped(authored_settings);
    const float radius = std::min(radius_px, max_gaussian_blur_radius_px);
    const float half_taps =
        static_cast<float>((settings.blur_taps - 1U) / 2U);
    const float sigma = std::max(radius * settings.blur_sigma_ratio, 1.0e-4F);

    const rhi::Texture* input = &source;
    for (core::u32 pass = 0U; pass < settings.blur_passes; ++pass) {
        const BlurUniform horizontal_uniform{
            1.0F / static_cast<float>(width()), 0.0F, radius, sigma,
            half_taps, {}};
        if (!commands.draw_output_transform(
                horizontal_, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}, pipeline_,
                *input, sampler_, bytes(horizontal_uniform))) {
            return GaussianBlurResult::failure(RenderError::BackendFailure);
        }

        const BlurUniform vertical_uniform{
            0.0F, 1.0F / static_cast<float>(height()), radius, sigma,
            half_taps, {}};
        if (!commands.draw_output_transform(
                vertical_, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}, pipeline_,
                horizontal_, sampler_, bytes(vertical_uniform))) {
            return GaussianBlurResult::failure(RenderError::BackendFailure);
        }
        // A second pass reads the first pass's result. The two intermediates
        // alternate rather than aliasing: a pass never samples the target it
        // is writing.
        input = &vertical_;
    }
    return GaussianBlurResult::success(&vertical_);
}

} // namespace mediaserver::render
