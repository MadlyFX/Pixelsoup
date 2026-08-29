#include <mediaserver/render/layer_keyer.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/chroma_key_compose_frag_dxil.h>
#include <mediaserver/shaders/chroma_key_matte_frag_dxil.h>
#include <mediaserver/shaders/key_morphology_frag_dxil.h>
#include <mediaserver/shaders/luma_key_compose_frag_dxil.h>
#include <mediaserver/shaders/luma_key_matte_frag_dxil.h>
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
constexpr float rec709_cb_scale = 1.8556F;
constexpr float rec709_cr_scale = 1.5748F;

using CreateResult = core::Result<LayerKeyer, RenderError>;

struct ShaderBlobs final {
    std::span<const core::u8> vertex;
    std::span<const core::u8> chroma_matte;
    std::span<const core::u8> luma_matte;
    std::span<const core::u8> morphology;
    std::span<const core::u8> chroma_compose;
    std::span<const core::u8> luma_compose;
    rhi::ShaderFormat format{rhi::ShaderFormat::Dxil};
    bool present{};
};

[[nodiscard]] ShaderBlobs shader_blobs() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return ShaderBlobs{
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_output_vert_dxil),
            sizeof(mediaserver_output_vert_dxil)},
        std::span<const core::u8>{reinterpret_cast<const core::u8*>(
                                     mediaserver_chroma_key_matte_frag_dxil),
                                 sizeof(mediaserver_chroma_key_matte_frag_dxil)},
        std::span<const core::u8>{reinterpret_cast<const core::u8*>(
                                     mediaserver_luma_key_matte_frag_dxil),
                                 sizeof(mediaserver_luma_key_matte_frag_dxil)},
        std::span<const core::u8>{reinterpret_cast<const core::u8*>(
                                     mediaserver_key_morphology_frag_dxil),
                                 sizeof(mediaserver_key_morphology_frag_dxil)},
        std::span<const core::u8>{reinterpret_cast<const core::u8*>(
                                     mediaserver_chroma_key_compose_frag_dxil),
                                 sizeof(
                                     mediaserver_chroma_key_compose_frag_dxil)},
        std::span<const core::u8>{reinterpret_cast<const core::u8*>(
                                     mediaserver_luma_key_compose_frag_dxil),
                                 sizeof(mediaserver_luma_key_compose_frag_dxil)},
        rhi::ShaderFormat::Dxil,
        true,
    };
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

[[nodiscard]] core::Result<rhi::GraphicsPipeline, RenderError>
create_pipeline(rhi::Device& device, const ShaderBlobs& blobs,
                std::span<const core::u8> fragment_bytecode,
                core::u32 sampler_count) {
    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = blobs.format;
    vertex.bytecode = blobs.vertex;

    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = blobs.format;
    fragment.bytecode = fragment_bytecode;
    fragment.sampler_count = sampler_count;
    fragment.uniform_buffer_count = 1U;

    auto pipeline = device.create_pipeline(
        vertex, fragment, rhi::TextureFormat::Rgba8Unorm, false,
        rhi::DepthState{});
    if (!pipeline) {
        return core::Result<rhi::GraphicsPipeline, RenderError>::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }
    return core::Result<rhi::GraphicsPipeline, RenderError>::success(
        std::move(pipeline).value());
}

struct alignas(16) ChromaMatteUniform final {
    float key_r{};
    float key_g{};
    float key_b{};
    float tolerance{};
    float softness{};
    std::array<float, 3U> padding{};
};
static_assert(sizeof(ChromaMatteUniform) == 32U);

struct alignas(16) LumaMatteUniform final {
    float low_threshold{};
    float high_threshold{};
    float low_softness{};
    float high_softness{};
    core::u32 invert{};
    std::array<core::u32, 3U> padding{};
};
static_assert(sizeof(LumaMatteUniform) == 32U);

struct alignas(16) MorphologyUniform final {
    float texel_x{};
    float texel_y{};
    core::u32 radius_px{};
    core::u32 dilate{};
};
static_assert(sizeof(MorphologyUniform) == 16U);

struct alignas(16) ChromaComposeUniform final {
    float key_r{};
    float key_g{};
    float key_b{};
    float spill_suppression{};
    float mix{};
    std::array<float, 3U> padding{};
};
static_assert(sizeof(ChromaComposeUniform) == 32U);

struct alignas(16) LumaComposeUniform final {
    float mix{};
    std::array<float, 3U> padding{};
};
static_assert(sizeof(LumaComposeUniform) == 16U);

template <typename Value>
[[nodiscard]] std::span<const core::u8> bytes(const Value& value) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&value), sizeof(value)};
}

[[nodiscard]] bool in_unit(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

[[nodiscard]] bool valid_color(const rhi::Color& value) noexcept {
    return in_unit(value.r) && in_unit(value.g) && in_unit(value.b) &&
        in_unit(value.a);
}

[[nodiscard]] bool valid_source(const rhi::Texture& color,
                                const rhi::Texture& depth, core::u32 width,
                                core::u32 height) noexcept {
    return color && color.desc().sampled &&
        color.desc().format != rhi::TextureFormat::Depth32Float &&
        color.width() == width && color.height() == height && depth &&
        depth.desc().format == rhi::TextureFormat::Depth32Float &&
        depth.desc().depth_target && depth.width() == width &&
        depth.height() == height;
}

[[nodiscard]] bool valid_reference_source(
    std::span<const rhi::Color> source, core::u32 width,
    core::u32 height) noexcept {
    if (width == 0U || height == 0U ||
        static_cast<std::size_t>(width) >
            std::numeric_limits<std::size_t>::max() /
                static_cast<std::size_t>(height) ||
        source.size() != static_cast<std::size_t>(width) * height) {
        return false;
    }
    return std::all_of(source.begin(), source.end(), [](const rhi::Color& pixel) {
        return valid_color(pixel) && pixel.r <= pixel.a + 1.0e-6F &&
            pixel.g <= pixel.a + 1.0e-6F && pixel.b <= pixel.a + 1.0e-6F;
    });
}

[[nodiscard]] float saturate(float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

struct CbCr final {
    float cb{};
    float cr{};
};

[[nodiscard]] float luminance(float r, float g, float b) noexcept {
    return rec709_red * r + rec709_green * g + rec709_blue * b;
}

[[nodiscard]] CbCr cbcr(float r, float g, float b) noexcept {
    const float y = luminance(r, g, b);
    return CbCr{(b - y) / rec709_cb_scale, (r - y) / rec709_cr_scale};
}

[[nodiscard]] rhi::Color straight(const rhi::Color& premultiplied) noexcept {
    if (premultiplied.a <= 0.0F) {
        return rhi::Color{0.0F, 0.0F, 0.0F, 0.0F};
    }
    const float inverse_alpha = 1.0F / premultiplied.a;
    return rhi::Color{saturate(premultiplied.r * inverse_alpha),
                      saturate(premultiplied.g * inverse_alpha),
                      saturate(premultiplied.b * inverse_alpha),
                      premultiplied.a};
}

[[nodiscard]] float smooth_step(float low, float high,
                                float value) noexcept {
    const float t = saturate((value - low) / (high - low));
    return t * t * (3.0F - 2.0F * t);
}

[[nodiscard]] float chroma_matte(const rhi::Color& source,
                                 const ChromaKeyParameters& parameters) noexcept {
    if (source.a <= 0.0F) {
        return 0.0F;
    }
    const rhi::Color rgb = straight(source);
    const CbCr sample = cbcr(rgb.r, rgb.g, rgb.b);
    const CbCr key = cbcr(parameters.key_color.r, parameters.key_color.g,
                          parameters.key_color.b);
    const float delta_cb = sample.cb - key.cb;
    const float delta_cr = sample.cr - key.cr;
    const float distance =
        std::sqrt(delta_cb * delta_cb + delta_cr * delta_cr);
    if (parameters.softness == 0.0F) {
        return distance <= parameters.tolerance ? 0.0F : 1.0F;
    }
    return smooth_step(parameters.tolerance,
                       parameters.tolerance + parameters.softness, distance);
}

[[nodiscard]] float luma_matte(const rhi::Color& source,
                               const LumaKeyParameters& parameters) noexcept {
    const rhi::Color rgb = straight(source);
    const float y = luminance(rgb.r, rgb.g, rgb.b);
    const float low = parameters.low_softness == 0.0F
        ? (y >= parameters.low_threshold ? 1.0F : 0.0F)
        : smooth_step(parameters.low_threshold - parameters.low_softness,
                      parameters.low_threshold, y);
    const float high = parameters.high_softness == 0.0F
        ? (y <= parameters.high_threshold ? 1.0F : 0.0F)
        : 1.0F - smooth_step(parameters.high_threshold,
                             parameters.high_threshold +
                                 parameters.high_softness,
                             y);
    const float coverage = std::min(low, high);
    return parameters.invert ? 1.0F - coverage : coverage;
}

void morphology_axis(std::span<const float> source, std::span<float> target,
                     core::u32 width, core::u32 height, core::i32 radius,
                     bool horizontal, bool dilate) {
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            float selected = dilate ? 0.0F : 1.0F;
            for (core::i32 offset = -radius; offset <= radius; ++offset) {
                const core::i64 sample_x = static_cast<core::i64>(x) +
                    (horizontal ? offset : 0);
                const core::i64 sample_y = static_cast<core::i64>(y) +
                    (horizontal ? 0 : offset);
                const float value = sample_x < 0 || sample_y < 0 ||
                        sample_x >= static_cast<core::i64>(width) ||
                        sample_y >= static_cast<core::i64>(height)
                    ? 0.0F
                    : source[static_cast<std::size_t>(sample_y) * width +
                             static_cast<std::size_t>(sample_x)];
                selected = dilate ? std::max(selected, value)
                                   : std::min(selected, value);
            }
            target[static_cast<std::size_t>(y) * width + x] = selected;
        }
    }
}

void apply_morphology(std::vector<float>& matte, core::u32 width,
                      core::u32 height, core::i32 signed_radius) {
    if (signed_radius == 0) {
        return;
    }
    const core::i32 radius = signed_radius < 0 ? -signed_radius : signed_radius;
    const bool dilate = signed_radius > 0;
    std::vector<float> temporary(matte.size());
    morphology_axis(matte, temporary, width, height, radius, true, dilate);
    morphology_axis(temporary, matte, width, height, radius, false, dilate);
}

[[nodiscard]] rhi::Color suppress_spill(
    const rhi::Color& source, float matte,
    const ChromaKeyParameters& parameters) noexcept {
    const CbCr key = cbcr(parameters.key_color.r, parameters.key_color.g,
                          parameters.key_color.b);
    const float key_length = std::sqrt(key.cb * key.cb + key.cr * key.cr);
    if (key_length <= 1.0e-8F || source.a <= 0.0F) {
        return straight(source);
    }
    rhi::Color rgb = straight(source);
    const float y = luminance(rgb.r, rgb.g, rgb.b);
    CbCr sample = cbcr(rgb.r, rgb.g, rgb.b);
    const float direction_cb = key.cb / key_length;
    const float direction_cr = key.cr / key_length;
    const float projection =
        std::max(sample.cb * direction_cb + sample.cr * direction_cr, 0.0F);
    const float amount = parameters.spill_suppression * parameters.mix *
        (1.0F - matte);
    sample.cb -= direction_cb * projection * amount;
    sample.cr -= direction_cr * projection * amount;
    const float red = y + rec709_cr_scale * sample.cr;
    const float blue = y + rec709_cb_scale * sample.cb;
    const float green =
        (y - rec709_red * red - rec709_blue * blue) / rec709_green;
    rgb.r = saturate(red);
    rgb.g = saturate(green);
    rgb.b = saturate(blue);
    return rgb;
}

[[nodiscard]] rhi::Color compose(const rhi::Color& source,
                                 const rhi::Color& straight_rgb, float matte,
                                 float mix) noexcept {
    const float coverage = 1.0F + (matte - 1.0F) * mix;
    const float alpha = saturate(source.a * coverage);
    return rhi::Color{saturate(straight_rgb.r * alpha),
                      saturate(straight_rgb.g * alpha),
                      saturate(straight_rgb.b * alpha), alpha};
}

[[nodiscard]] core::Result<rhi::Texture, RenderError>
create_color(rhi::Device& device, core::u32 width, core::u32 height) {
    auto texture = device.create_texture(color_desc(width, height));
    if (!texture) {
        return core::Result<rhi::Texture, RenderError>::failure(
            RenderError::AllocationFailed);
    }
    return core::Result<rhi::Texture, RenderError>::success(
        std::move(texture).value());
}

} // namespace

bool valid_chroma_key(const ChromaKeyParameters& parameters) noexcept {
    return valid_color(parameters.key_color) && in_unit(parameters.tolerance) &&
        in_unit(parameters.softness) &&
        parameters.morphology_px >= -max_key_morphology_radius_px &&
        parameters.morphology_px <= max_key_morphology_radius_px &&
        in_unit(parameters.spill_suppression) && in_unit(parameters.mix);
}

bool valid_luma_key(const LumaKeyParameters& parameters) noexcept {
    return in_unit(parameters.low_threshold) &&
        in_unit(parameters.high_threshold) &&
        parameters.low_threshold <= parameters.high_threshold &&
        in_unit(parameters.low_softness) &&
        in_unit(parameters.high_softness) && in_unit(parameters.mix);
}

bool chroma_key_is_identity(
    const ChromaKeyParameters& parameters) noexcept {
    return std::isfinite(parameters.mix) && parameters.mix == 0.0F;
}

bool luma_key_is_identity(const LumaKeyParameters& parameters) noexcept {
    return std::isfinite(parameters.mix) && parameters.mix == 0.0F;
}

KeyBoundsExpansion chroma_key_bounds(
    const ChromaKeyParameters& parameters) noexcept {
    static_cast<void>(parameters);
    // Final coverage is sourceA * lerp(1, matte, mix). Dilation can restore a
    // keyed pixel that already had source coverage, but cannot manufacture
    // coverage or edge colour outside the layer.
    return {};
}

KeyReferenceResult chroma_key_reference(
    std::span<const rhi::Color> source, core::u32 width, core::u32 height,
    const ChromaKeyParameters& parameters) {
    if (!valid_reference_source(source, width, height) ||
        !in_unit(parameters.mix)) {
        return KeyReferenceResult::failure(RenderError::InvalidConfiguration);
    }
    if (parameters.mix == 0.0F) {
        return KeyReferenceResult::success(
            std::vector<rhi::Color>{source.begin(), source.end()});
    }
    if (!valid_chroma_key(parameters)) {
        return KeyReferenceResult::failure(RenderError::InvalidConfiguration);
    }

    std::vector<float> matte(source.size());
    std::transform(source.begin(), source.end(), matte.begin(),
                   [&parameters](const rhi::Color& pixel) {
                       return chroma_matte(pixel, parameters);
                   });
    apply_morphology(matte, width, height, parameters.morphology_px);

    std::vector<rhi::Color> result(source.size());
    for (std::size_t index = 0U; index < source.size(); ++index) {
        result[index] = compose(
            source[index], suppress_spill(source[index], matte[index],
                                          parameters),
            matte[index], parameters.mix);
    }
    return KeyReferenceResult::success(std::move(result));
}

KeyReferenceResult luma_key_reference(
    std::span<const rhi::Color> source, core::u32 width, core::u32 height,
    const LumaKeyParameters& parameters) {
    if (!valid_reference_source(source, width, height) ||
        !in_unit(parameters.mix)) {
        return KeyReferenceResult::failure(RenderError::InvalidConfiguration);
    }
    if (parameters.mix == 0.0F) {
        return KeyReferenceResult::success(
            std::vector<rhi::Color>{source.begin(), source.end()});
    }
    if (!valid_luma_key(parameters)) {
        return KeyReferenceResult::failure(RenderError::InvalidConfiguration);
    }

    std::vector<rhi::Color> result(source.size());
    for (std::size_t index = 0U; index < source.size(); ++index) {
        const float matte = luma_matte(source[index], parameters);
        result[index] = compose(source[index], straight(source[index]), matte,
                                parameters.mix);
    }
    return KeyReferenceResult::success(std::move(result));
}

bool LayerKeyer::shaders_available() noexcept {
    return shader_blobs().present;
}

LayerKeyer::LayerKeyer(
    rhi::Texture matte_a, rhi::Texture matte_b, rhi::Texture output_a,
    rhi::Texture output_b, rhi::GraphicsPipeline chroma_matte_pipeline,
    rhi::GraphicsPipeline luma_matte_pipeline,
    rhi::GraphicsPipeline morphology_pipeline,
    rhi::GraphicsPipeline chroma_compose_pipeline,
    rhi::GraphicsPipeline luma_compose_pipeline, rhi::Sampler sampler) noexcept
    : matte_a_(std::move(matte_a)), matte_b_(std::move(matte_b)),
      output_a_(std::move(output_a)), output_b_(std::move(output_b)),
      chroma_matte_pipeline_(std::move(chroma_matte_pipeline)),
      luma_matte_pipeline_(std::move(luma_matte_pipeline)),
      morphology_pipeline_(std::move(morphology_pipeline)),
      chroma_compose_pipeline_(std::move(chroma_compose_pipeline)),
      luma_compose_pipeline_(std::move(luma_compose_pipeline)),
      sampler_(std::move(sampler)) {}

CreateResult LayerKeyer::create(rhi::Device& device, core::u32 width,
                                core::u32 height) {
    const ShaderBlobs blobs = shader_blobs();
    if (!blobs.present || !device.supports(blobs.format)) {
        return CreateResult::failure(RenderError::ShaderFormatUnsupported);
    }
    if (width == 0U || height == 0U) {
        return CreateResult::failure(RenderError::InvalidConfiguration);
    }

    auto matte_a = create_color(device, width, height);
    auto matte_b = create_color(device, width, height);
    auto output_a = create_color(device, width, height);
    auto output_b = create_color(device, width, height);
    if (!matte_a || !matte_b || !output_a || !output_b) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }
    auto chroma_matte =
        create_pipeline(device, blobs, blobs.chroma_matte, 1U);
    auto luma_matte = create_pipeline(device, blobs, blobs.luma_matte, 1U);
    auto morphology = create_pipeline(device, blobs, blobs.morphology, 1U);
    auto chroma_compose =
        create_pipeline(device, blobs, blobs.chroma_compose, 2U);
    auto luma_compose =
        create_pipeline(device, blobs, blobs.luma_compose, 2U);
    if (!chroma_matte || !luma_matte || !morphology || !chroma_compose ||
        !luma_compose) {
        const RenderError error = !chroma_matte
            ? chroma_matte.error()
            : !luma_matte
            ? luma_matte.error()
            : !morphology
            ? morphology.error()
            : !chroma_compose ? chroma_compose.error()
                              : luma_compose.error();
        return CreateResult::failure(error);
    }
    auto sampler = device.create_sampler(rhi::Filter::Nearest);
    if (!sampler) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }

    return CreateResult::success(LayerKeyer{
        std::move(matte_a).value(), std::move(matte_b).value(),
        std::move(output_a).value(), std::move(output_b).value(),
        std::move(chroma_matte).value(), std::move(luma_matte).value(),
        std::move(morphology).value(), std::move(chroma_compose).value(),
        std::move(luma_compose).value(), std::move(sampler).value()});
}

bool LayerKeyer::resize(rhi::Device& device, core::u32 width,
                        core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (this->width() == width && this->height() == height) {
        return true;
    }
    auto matte_a = create_color(device, width, height);
    auto matte_b = create_color(device, width, height);
    auto output_a = create_color(device, width, height);
    auto output_b = create_color(device, width, height);
    if (!matte_a || !matte_b || !output_a || !output_b) {
        return false;
    }
    matte_a_ = std::move(matte_a).value();
    matte_b_ = std::move(matte_b).value();
    output_a_ = std::move(output_a).value();
    output_b_ = std::move(output_b).value();
    return true;
}

rhi::Texture& LayerKeyer::output_for(const rhi::Texture& source) noexcept {
    return &source == &output_a_ ? output_b_ : output_a_;
}

void LayerKeyer::record_pass() noexcept {
    ++stats_.recorded_passes;
    stats_.processed_pixels +=
        static_cast<core::u64>(width()) * static_cast<core::u64>(height());
}

LayerKeyerResult LayerKeyer::apply_chroma(
    rhi::CommandList& commands, const rhi::Texture& source_color,
    const rhi::Texture& source_depth,
    const ChromaKeyParameters& parameters) noexcept {
    if (!valid_source(source_color, source_depth, width(), height()) ||
        !in_unit(parameters.mix)) {
        return LayerKeyerResult::failure(RenderError::InvalidConfiguration);
    }
    if (parameters.mix == 0.0F) {
        return LayerKeyerResult::success(
            KeyedLayer{&source_color, &source_depth});
    }
    if (!valid_chroma_key(parameters)) {
        return LayerKeyerResult::failure(RenderError::InvalidConfiguration);
    }

    const ChromaMatteUniform matte_uniform{
        parameters.key_color.r, parameters.key_color.g,
        parameters.key_color.b, parameters.tolerance, parameters.softness, {}};
    if (!commands.draw_output_transform(
            matte_a_, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F},
            chroma_matte_pipeline_, source_color, sampler_,
            bytes(matte_uniform))) {
        return LayerKeyerResult::failure(RenderError::BackendFailure);
    }
    record_pass();

    const rhi::Texture* matte = &matte_a_;
    if (parameters.morphology_px != 0) {
        const core::u32 radius = static_cast<core::u32>(
            parameters.morphology_px < 0 ? -parameters.morphology_px
                                         : parameters.morphology_px);
        const core::u32 dilate = parameters.morphology_px > 0 ? 1U : 0U;
        const MorphologyUniform horizontal{
            1.0F / static_cast<float>(width()), 0.0F, radius, dilate};
        if (!commands.draw_output_transform(
                matte_b_, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F},
                morphology_pipeline_, matte_a_, sampler_, bytes(horizontal))) {
            return LayerKeyerResult::failure(RenderError::BackendFailure);
        }
        record_pass();
        const MorphologyUniform vertical{
            0.0F, 1.0F / static_cast<float>(height()), radius, dilate};
        if (!commands.draw_output_transform(
                matte_a_, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F},
                morphology_pipeline_, matte_b_, sampler_, bytes(vertical))) {
            return LayerKeyerResult::failure(RenderError::BackendFailure);
        }
        record_pass();
        matte = &matte_a_;
    }

    rhi::Texture& output = output_for(source_color);
    const ChromaComposeUniform compose_uniform{
        parameters.key_color.r, parameters.key_color.g,
        parameters.key_color.b, parameters.spill_suppression, parameters.mix,
        {}};
    const std::array<rhi::SampledTexture, 2U> sources{{
        rhi::SampledTexture{&source_color, &sampler_},
        rhi::SampledTexture{matte, &sampler_},
    }};
    if (!commands.draw_output_transform(
            output, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F},
            chroma_compose_pipeline_, sources, bytes(compose_uniform))) {
        return LayerKeyerResult::failure(RenderError::BackendFailure);
    }
    record_pass();
    return LayerKeyerResult::success(KeyedLayer{&output, &source_depth});
}

LayerKeyerResult LayerKeyer::apply_luma(
    rhi::CommandList& commands, const rhi::Texture& source_color,
    const rhi::Texture& source_depth,
    const LumaKeyParameters& parameters) noexcept {
    if (!valid_source(source_color, source_depth, width(), height()) ||
        !in_unit(parameters.mix)) {
        return LayerKeyerResult::failure(RenderError::InvalidConfiguration);
    }
    if (parameters.mix == 0.0F) {
        return LayerKeyerResult::success(
            KeyedLayer{&source_color, &source_depth});
    }
    if (!valid_luma_key(parameters)) {
        return LayerKeyerResult::failure(RenderError::InvalidConfiguration);
    }

    const LumaMatteUniform matte_uniform{
        parameters.low_threshold, parameters.high_threshold,
        parameters.low_softness, parameters.high_softness,
        parameters.invert ? 1U : 0U, {}};
    if (!commands.draw_output_transform(
            matte_a_, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F},
            luma_matte_pipeline_, source_color, sampler_, bytes(matte_uniform))) {
        return LayerKeyerResult::failure(RenderError::BackendFailure);
    }
    record_pass();

    rhi::Texture& output = output_for(source_color);
    const LumaComposeUniform compose_uniform{parameters.mix, {}};
    const std::array<rhi::SampledTexture, 2U> sources{{
        rhi::SampledTexture{&source_color, &sampler_},
        rhi::SampledTexture{&matte_a_, &sampler_},
    }};
    if (!commands.draw_output_transform(
            output, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F},
            luma_compose_pipeline_, sources, bytes(compose_uniform))) {
        return LayerKeyerResult::failure(RenderError::BackendFailure);
    }
    record_pass();
    return LayerKeyerResult::success(KeyedLayer{&output, &source_depth});
}

} // namespace mediaserver::render
