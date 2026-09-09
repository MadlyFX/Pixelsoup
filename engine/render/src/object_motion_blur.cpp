#include <mediaserver/render/object_motion_blur.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/output_vert_dxil.h>
#include <mediaserver/shaders/object_motion_dilate_frag_dxil.h>
#include <mediaserver/shaders/object_motion_blur_frag_dxil.h>
#include <mediaserver/shaders/object_motion_merge_comp_dxil.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>

namespace mediaserver::render {
namespace {
struct Constants final {
    float extent[4]{}; // width, height, radius, vertical dilation
    float motion[4]{}; // UV-to-output-pixel scale, shutter fraction, unused
};
std::span<const core::u8> bytes(const Constants& value) noexcept {
    return {reinterpret_cast<const core::u8*>(&value), sizeof(value)};
}
rhi::TextureDesc texture_desc(core::u32 width, core::u32 height,
    rhi::TextureFormat format, bool storage = false) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width; desc.height = height; desc.format = format;
    desc.color_target = true; desc.sampled = true;
    desc.storage_read = storage; desc.storage_write = storage;
    return desc;
}
} // namespace

float object_shutter_fraction(const scene::Scene& frame, const SceneRenderView* view) noexcept {
    const float seconds = view && view->shutter_seconds
        ? *view->shutter_seconds : frame.camera.shutter_seconds;
    if (!std::isfinite(seconds) || seconds <= 0.0F
        || frame.show_rate.numerator == 0U || frame.show_rate.denominator == 0U) return 0.0F;
    return static_cast<float>(std::clamp(static_cast<double>(seconds)
        * frame.show_rate.numerator / frame.show_rate.denominator, 0.0, 1.0));
}

core::Result<ObjectMotionBlur, RenderError> ObjectMotionBlur::create(
    rhi::Device& device, core::u32 width, core::u32 height,
    rhi::TextureFormat format, bool merge_layers) noexcept {
    using Result = core::Result<ObjectMotionBlur, RenderError>;
#if !defined(MEDIASERVER_HAS_DXIL_SHADERS)
    static_cast<void>(device); static_cast<void>(width); static_cast<void>(height);
    static_cast<void>(format); static_cast<void>(merge_layers);
    return Result::failure(RenderError::ShaderFormatUnsupported);
#else
    if (width == 0U || height == 0U) return Result::failure(RenderError::InvalidConfiguration);
    ObjectMotionBlur result;
    auto horizontal = device.create_texture(texture_desc(width, height, rhi::TextureFormat::Rgba32Float));
    auto neighborhood = device.create_texture(texture_desc(width, height, rhi::TextureFormat::Rgba32Float));
    auto output = device.create_texture(texture_desc(width, height, format));
    auto sampler = device.create_sampler(rhi::Filter::Nearest);
    if (!horizontal || !neighborhood || !output || !sampler) return Result::failure(RenderError::AllocationFailed);
    result.horizontal_ = std::move(horizontal).value();
    result.neighborhood_ = std::move(neighborhood).value();
    result.output_ = std::move(output).value();
    result.sampler_ = std::move(sampler).value();
    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.bytecode = {reinterpret_cast<const core::u8*>(mediaserver_output_vert_dxil), sizeof(mediaserver_output_vert_dxil)};
    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.sampler_count = 1U; fragment.uniform_buffer_count = 1U;
    fragment.bytecode = {reinterpret_cast<const core::u8*>(mediaserver_object_motion_dilate_frag_dxil), sizeof(mediaserver_object_motion_dilate_frag_dxil)};
    auto dilation = device.create_pipeline(vertex, fragment, rhi::TextureFormat::Rgba32Float, false, {});
    fragment.sampler_count = 3U;
    fragment.bytecode = {reinterpret_cast<const core::u8*>(mediaserver_object_motion_blur_frag_dxil), sizeof(mediaserver_object_motion_blur_frag_dxil)};
    auto resolve = device.create_pipeline(vertex, fragment, format, false, {});
    if (!dilation || !resolve) return Result::failure(RenderError::BackendFailure);
    result.dilation_pipeline_ = std::move(dilation).value();
    result.resolve_pipeline_ = std::move(resolve).value();
    if (merge_layers) {
        auto merged = device.create_texture(texture_desc(width, height, rhi::TextureFormat::Rgba32Float, true));
        rhi::ShaderDesc merge{};
        merge.stage = rhi::ShaderStage::Compute;
        merge.sampler_count = 1U; merge.storage_texture_count = 1U; merge.uniform_buffer_count = 1U;
        merge.bytecode = {reinterpret_cast<const core::u8*>(mediaserver_object_motion_merge_comp_dxil), sizeof(mediaserver_object_motion_merge_comp_dxil)};
        auto pipeline = device.create_compute_pipeline(merge);
        if (!merged || !pipeline) return Result::failure(RenderError::BackendFailure);
        result.merged_ = std::move(merged).value();
        result.merge_pipeline_ = std::move(pipeline).value();
    }
    return Result::success(std::move(result));
#endif
}

bool ObjectMotionBlur::resize(rhi::Device& device, core::u32 width, core::u32 height) noexcept {
    if (output_.width() == width && output_.height() == height) return true;
    auto next = create(device, width, height, output_.desc().format, static_cast<bool>(merged_));
    if (!next) return false;
    *this = std::move(next).value();
    return true;
}

bool ObjectMotionBlur::begin(rhi::CommandList& commands) noexcept {
    graph_.begin();
    return merged_ && graph_.clear(commands, core::RenderPass::MotionBlur, merged_, {0, 0, 0, 0});
}

bool ObjectMotionBlur::merge(rhi::CommandList& commands, const rhi::Texture& motion) noexcept {
    if (!merged_ || motion.width() != merged_.width() || motion.height() != merged_.height()
        || motion.desc().format != rhi::TextureFormat::Rgba32Float) return false;
    const Constants constants{{static_cast<float>(motion.width()), static_cast<float>(motion.height()), 0, 0}, {}};
    const std::array inputs{rhi::SampledTexture{&motion, &sampler_}};
    const std::array targets{rhi::StorageTexture{&merged_, 0U, 0U, true}};
    rhi::ComputeDispatch work{};
    work.pipeline = &merge_pipeline_; work.sampled_textures = inputs;
    work.storage_textures = targets; work.uniform = bytes(constants);
    work.groups_x = (motion.width() + 7U) / 8U; work.groups_y = (motion.height() + 7U) / 8U;
    return graph_.dispatch(commands, core::RenderPass::MotionBlur, work);
}

core::Result<const rhi::Texture*, RenderError> ObjectMotionBlur::apply(
    rhi::CommandList& commands, const rhi::Texture& color, const rhi::Texture& motion,
    float shutter_fraction, core::u32 radius, float motion_width, float motion_height) noexcept {
    graph_.begin();
    using Result = core::Result<const rhi::Texture*, RenderError>;
    if (!std::isfinite(shutter_fraction) || shutter_fraction < 0 || shutter_fraction > 1
        || radius > scene::max_motion_blur_radius || !std::isfinite(motion_width)
        || !std::isfinite(motion_height) || motion_width <= 0 || motion_height <= 0)
        return Result::failure(RenderError::InvalidConfiguration);
    if (radius == 0U || shutter_fraction == 0.0F) return Result::success(&color);
    if (color.width() != output_.width() || color.height() != output_.height()
        || color.desc().format != output_.desc().format || motion.width() != color.width()
        || motion.height() != color.height() || motion.desc().format != rhi::TextureFormat::Rgba32Float)
        return Result::failure(RenderError::InvalidConfiguration);
    Constants constants{{static_cast<float>(color.width()), static_cast<float>(color.height()),
        static_cast<float>(radius), 0}, {motion_width, motion_height, shutter_fraction, 0}};
    const std::array first{rhi::SampledTexture{&motion, &sampler_}};
    if (!graph_.fullscreen(commands, core::RenderPass::MotionBlur, horizontal_, {0, 0, 0, 0}, dilation_pipeline_, first, bytes(constants)))
        return Result::failure(RenderError::BackendFailure);
    constants.extent[3] = 1;
    const std::array second{rhi::SampledTexture{&horizontal_, &sampler_}};
    if (!graph_.fullscreen(commands, core::RenderPass::MotionBlur, neighborhood_, {0, 0, 0, 0}, dilation_pipeline_, second, bytes(constants)))
        return Result::failure(RenderError::BackendFailure);
    const std::array inputs{rhi::SampledTexture{&color, &sampler_}, rhi::SampledTexture{&motion, &sampler_},
        rhi::SampledTexture{&neighborhood_, &sampler_}};
    if (!graph_.fullscreen(commands, core::RenderPass::MotionBlur, output_, {0, 0, 0, 0}, resolve_pipeline_, inputs, bytes(constants)))
        return Result::failure(RenderError::BackendFailure);
    return Result::success(&output_);
}
} // namespace mediaserver::render
