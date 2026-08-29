#include <mediaserver/render/output_transform.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/output_frag_dxil.h>
#include <mediaserver/shaders/output_vert_dxil.h>
#endif

#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using Result = core::Result<OutputTransform, RenderError>;

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
            reinterpret_cast<const core::u8*>(mediaserver_output_frag_dxil),
            sizeof(mediaserver_output_frag_dxil)},
        rhi::ShaderFormat::Dxil,
        true,
    };
#else
    // No shader compiler was available when this was built, so there is no
    // output path to offer.
    return ShaderBlobs{};
#endif
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

struct alignas(16) OutputUniform final {
    float viewport_x{};
    float viewport_y{};
    float viewport_width{1.0F};
    float viewport_height{1.0F};
};

static_assert(sizeof(OutputUniform) == 16U);

[[nodiscard]] std::span<const core::u8>
bytes(const OutputUniform& uniform) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&uniform), sizeof(uniform)};
}

} // namespace

bool OutputTransform::shaders_available() noexcept {
    return shader_blobs().present;
}

OutputTransform::OutputTransform(
    rhi::Texture color,
    rhi::GraphicsPipeline pipeline,
    rhi::Sampler sampler) noexcept
    : color_(std::move(color)),
      pipeline_(std::move(pipeline)),
      sampler_(std::move(sampler)) {}

Result OutputTransform::create(
    rhi::Device& device,
    core::u32 width,
    core::u32 height) {
    const auto blobs = shader_blobs();
    if (!blobs.present) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }
    if (width == 0U || height == 0U) {
        return Result::failure(RenderError::InvalidConfiguration);
    }
    if (!device.supports(blobs.format)) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }

    auto color = device.create_texture(color_desc(width, height));
    if (!color) {
        return Result::failure(RenderError::AllocationFailed);
    }

    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = blobs.format;
    vertex.bytecode = blobs.vertex;
    // The fullscreen triangle reads nothing: its three corners come from
    // SV_VertexID, so there are no storage buffers and no uniforms here.

    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = blobs.format;
    fragment.bytecode = blobs.fragment;
    fragment.sampler_count = 1U;
    fragment.uniform_buffer_count = 1U;

    auto pipeline = device.create_pipeline(
        vertex,
        fragment,
        rhi::TextureFormat::Rgba8Unorm,
        // Opaque: the transform writes premultiplied colour and owns the whole
        // target, so there is nothing here to blend against.
        false,
        rhi::DepthState{});
    if (!pipeline) {
        return Result::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }

    // Linear, so a source rendered at one resolution resamples cleanly onto an
    // output raster of another. The seam's samplers are clamp-to-edge, which
    // keeps the fade from wrapping the far edge's pixels in at the near one.
    auto sampler = device.create_sampler(rhi::Filter::Linear);
    if (!sampler) {
        return Result::failure(RenderError::AllocationFailed);
    }

    return Result::success(OutputTransform{
        std::move(color).value(),
        std::move(pipeline).value(),
        std::move(sampler).value()});
}

bool OutputTransform::resize(
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
    color_ = std::move(color).value();
    return true;
}

bool OutputTransform::render(
    rhi::CommandList& commands,
    const rhi::Texture& source,
    scene::CanvasRect viewport) {
    return clear(commands) && composite(commands, source, viewport);
}

bool OutputTransform::clear(rhi::CommandList& commands) noexcept {
    return commands.clear(color_, rhi::Color{0.0F, 0.0F, 0.0F, 1.0F});
}

bool OutputTransform::composite(
    rhi::CommandList& commands,
    const rhi::Texture& source,
    scene::CanvasRect viewport) {
    const scene::Canvas window{width(), height()};
    if (!scene::contains(window, viewport)) {
        return false;
    }
    const OutputUniform uniform{
        static_cast<float>(viewport.x) / static_cast<float>(width()),
        static_cast<float>(viewport.y) / static_cast<float>(height()),
        static_cast<float>(viewport.width) / static_cast<float>(width()),
        static_cast<float>(viewport.height) / static_cast<float>(height())};
    // The target is loaded so other viewports in this window survive. The
    // fragment shader discards outside this viewport.
    return commands.draw_output_transform(
        color_,
        rhi::Color{0.0F, 0.0F, 0.0F, 1.0F},
        pipeline_,
        source,
        sampler_,
        bytes(uniform),
        true);
}

} // namespace mediaserver::render
