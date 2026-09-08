#include <mediaserver/render/layer_matte.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/matte_frag_dxil.h>
#include <mediaserver/shaders/output_vert_dxil.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

constexpr float rec709_red = 0.2126F;
constexpr float rec709_green = 0.7152F;
constexpr float rec709_blue = 0.0722F;
constexpr float alpha_epsilon = 1.0e-8F;

using CreateResult = core::Result<LayerMatte, RenderError>;

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
            reinterpret_cast<const core::u8*>(mediaserver_matte_frag_dxil),
            sizeof(mediaserver_matte_frag_dxil)},
        rhi::ShaderFormat::Dxil,
        true};
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

[[nodiscard]] bool in_unit(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

[[nodiscard]] bool valid_pixel(const rhi::Color& value) noexcept {
    return in_unit(value.r) && in_unit(value.g) && in_unit(value.b) &&
        in_unit(value.a);
}

[[nodiscard]] bool valid_source_pixel(const rhi::Color& value) noexcept {
    return valid_pixel(value) && value.r <= value.a + 1.0e-6F &&
        value.g <= value.a + 1.0e-6F && value.b <= value.a + 1.0e-6F;
}

[[nodiscard]] bool valid_reference_inputs(
    std::span<const rhi::Color> source,
    std::span<const rhi::Color> matte,
    core::u32 width,
    core::u32 height) noexcept {
    if (width == 0U || height == 0U ||
        static_cast<std::size_t>(width) >
            std::numeric_limits<std::size_t>::max() /
                static_cast<std::size_t>(height)) {
        return false;
    }
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    return source.size() == pixels && matte.size() == pixels &&
        std::all_of(source.begin(), source.end(), valid_source_pixel) &&
        std::all_of(matte.begin(), matte.end(), valid_pixel);
}

[[nodiscard]] float saturate(float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] float matte_coverage(
    const rhi::Color& pixel,
    const compositor::MatteParameters& parameters) noexcept {
    float coverage = pixel.a;
    if (parameters.mode == compositor::MatteMode::Luma) {
        float red{};
        float green{};
        float blue{};
        if (pixel.a > alpha_epsilon) {
            const float inverse_alpha = 1.0F / pixel.a;
            red = pixel.r * inverse_alpha;
            green = pixel.g * inverse_alpha;
            blue = pixel.b * inverse_alpha;
            if (parameters.clamp_input) {
                red = saturate(red);
                green = saturate(green);
                blue = saturate(blue);
            }
        }
        coverage = saturate(
            (rec709_red * red + rec709_green * green + rec709_blue * blue) *
            pixel.a);
    } else {
        coverage = saturate(coverage);
    }
    return parameters.invert ? 1.0F - coverage : coverage;
}

[[nodiscard]] bool valid_texture_pair(
    const rhi::Texture& source_color,
    const rhi::Texture& source_depth,
    const rhi::Texture& matte_color,
    core::u32 width,
    core::u32 height) noexcept {
    const auto valid_color = [width, height](const rhi::Texture& texture) {
        return texture && texture.desc().sampled &&
            texture.desc().format != rhi::TextureFormat::Depth32Float &&
            texture.width() == width && texture.height() == height;
    };
    return valid_color(source_color) && valid_color(matte_color) &&
        source_depth &&
        source_depth.desc().format == rhi::TextureFormat::Depth32Float &&
        source_depth.desc().depth_target && source_depth.width() == width &&
        source_depth.height() == height && &source_color != &matte_color;
}

struct alignas(16) MatteUniform final {
    core::u32 mode{};
    core::u32 invert{};
    core::u32 clamp_input{};
    core::u32 padding{};
};
static_assert(sizeof(MatteUniform) == 16U);

[[nodiscard]] std::span<const core::u8>
bytes(const MatteUniform& value) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&value), sizeof(value)};
}

} // namespace

bool valid_matte_parameters(
    const compositor::MatteParameters& parameters) noexcept {
    return parameters.mode == compositor::MatteMode::Alpha ||
        parameters.mode == compositor::MatteMode::Luma;
}

MatteReferenceResult matte_reference(
    std::span<const rhi::Color> source,
    std::span<const rhi::Color> matte,
    core::u32 width,
    core::u32 height,
    const compositor::MatteParameters& parameters) {
    if (!valid_matte_parameters(parameters) ||
        !valid_reference_inputs(source, matte, width, height)) {
        return MatteReferenceResult::failure(
            RenderError::InvalidConfiguration);
    }

    std::vector<rhi::Color> result(source.size());
    for (std::size_t index = 0U; index < source.size(); ++index) {
        const rhi::Color& source_pixel = source[index];
        const float coverage = matte_coverage(matte[index], parameters);
        const float final_alpha = source_pixel.a * coverage;
        if (source_pixel.a <= alpha_epsilon || final_alpha <= 0.0F) {
            result[index] = rhi::Color{};
            continue;
        }
        const float scale = final_alpha / source_pixel.a;
        result[index] = rhi::Color{source_pixel.r * scale,
                                   source_pixel.g * scale,
                                   source_pixel.b * scale,
                                   final_alpha};
    }
    return MatteReferenceResult::success(std::move(result));
}

bool LayerMatte::shaders_available() noexcept {
    return shader_blobs().present;
}

LayerMatte::LayerMatte(rhi::Texture output_a, rhi::Texture output_b,
                       rhi::GraphicsPipeline pipeline,
                       rhi::Sampler sampler) noexcept
    : output_a_(std::move(output_a)), output_b_(std::move(output_b)),
      pipeline_(std::move(pipeline)), sampler_(std::move(sampler)) {}

CreateResult LayerMatte::create(rhi::Device& device, core::u32 width,
                                core::u32 height,
                                rhi::TextureFormat format) {
    const ShaderBlobs blobs = shader_blobs();
    if (!blobs.present || !device.supports(blobs.format)) {
        return CreateResult::failure(RenderError::ShaderFormatUnsupported);
    }
    if (width == 0U || height == 0U) {
        return CreateResult::failure(RenderError::InvalidConfiguration);
    }
    auto output_a = device.create_texture(color_desc(width, height, format));
    auto output_b = device.create_texture(color_desc(width, height, format));
    if (!output_a || !output_b) {
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
    fragment.sampler_count = 2U;
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
    auto sampler = device.create_sampler(rhi::Filter::Nearest);
    if (!sampler) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }
    return CreateResult::success(LayerMatte{
        std::move(output_a).value(), std::move(output_b).value(),
        std::move(pipeline).value(), std::move(sampler).value()});
}

bool LayerMatte::resize(rhi::Device& device, core::u32 width,
                        core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (this->width() == width && this->height() == height) {
        return true;
    }
    const auto format = output_a_.desc().format;
    auto output_a = device.create_texture(color_desc(width, height, format));
    auto output_b = device.create_texture(color_desc(width, height, format));
    if (!output_a || !output_b) {
        return false;
    }
    output_a_ = std::move(output_a).value();
    output_b_ = std::move(output_b).value();
    return true;
}

rhi::Texture& LayerMatte::output_for(
    const rhi::Texture& source, const rhi::Texture& matte) noexcept {
    return &source == &output_a_ || &matte == &output_a_ ? output_b_
                                                         : output_a_;
}

LayerMatteResult LayerMatte::apply(
    rhi::CommandList& commands,
    const rhi::Texture& source_color,
    const rhi::Texture& source_depth,
    const rhi::Texture& matte_color,
    const compositor::MatteParameters& parameters) noexcept {
    if (!valid_matte_parameters(parameters) ||
        !valid_texture_pair(source_color, source_depth, matte_color, width(),
                            height())) {
        return LayerMatteResult::failure(RenderError::InvalidConfiguration);
    }
    rhi::Texture& output = output_for(source_color, matte_color);
    const MatteUniform uniform{
        static_cast<core::u32>(parameters.mode),
        parameters.invert ? 1U : 0U,
        parameters.clamp_input ? 1U : 0U,
        0U};
    const std::array<rhi::SampledTexture, 2U> inputs{{
        rhi::SampledTexture{&source_color, &sampler_},
        rhi::SampledTexture{&matte_color, &sampler_},
    }};
    if (!commands.draw_output_transform(
            output, rhi::Color{}, pipeline_, inputs, bytes(uniform))) {
        return LayerMatteResult::failure(RenderError::BackendFailure);
    }
    ++stats_.recorded_passes;
    stats_.processed_pixels +=
        static_cast<core::u64>(width()) * static_cast<core::u64>(height());
    return LayerMatteResult::success(MattedLayer{&output, &source_depth});
}

} // namespace mediaserver::render
