#include <mediaserver/ui/gpu_output_target.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/quad_frag_dxil.h>
#include <mediaserver/shaders/quad_vert_dxil.h>
#endif

#include <new>
#include <span>
#include <utility>

namespace mediaserver::ui {
namespace {

// The instance record the vertex stage indexes. Both sides describe the same
// 48 bytes, and a mismatch would be a silently wrong frame rather than a
// compile error, so it is asserted here.
static_assert(sizeof(show::PatternQuad) == 48U);
static_assert(alignof(show::PatternQuad) == 4U);

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
            reinterpret_cast<const core::u8*>(mediaserver_quad_vert_dxil),
            sizeof(mediaserver_quad_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_quad_frag_dxil),
            sizeof(mediaserver_quad_frag_dxil)},
        rhi::ShaderFormat::Dxil,
        true,
    };
#else
    // No shader compiler was available when this was built, so there is no GPU
    // path to offer. Saying so plainly is the whole point of the flag.
    return ShaderBlobs{};
#endif
}

} // namespace

bool GpuOutputTarget::shaders_available() noexcept {
    return shader_blobs().present;
}

GpuOutputTarget::GpuOutputTarget(
    rhi::Texture target,
    rhi::Texture atlas,
    rhi::Sampler sampler,
    rhi::GraphicsPipeline pipeline,
    rhi::StorageBuffer instances) noexcept
    : target_(std::move(target)),
      atlas_(std::move(atlas)),
      sampler_(std::move(sampler)),
      pipeline_(std::move(pipeline)),
      instances_(std::move(instances)) {}

core::Result<GpuOutputTarget, rhi::RhiError> GpuOutputTarget::create(
    rhi::Device& device,
    core::u32 width,
    core::u32 height) {
    using Result = core::Result<GpuOutputTarget, rhi::RhiError>;

    const auto blobs = shader_blobs();
    if (!blobs.present) {
        return Result::failure(rhi::RhiError::ShaderFormatUnsupported);
    }
    if (width == 0U || height == 0U) {
        return Result::failure(rhi::RhiError::InvalidConfiguration);
    }

    rhi::TextureDesc colour{};
    colour.width = width;
    colour.height = height;
    colour.format = rhi::TextureFormat::Rgba8Unorm;
    colour.color_target = true;
    colour.sampled = true;
    auto target = device.create_texture(colour);
    if (!target) {
        return Result::failure(target.error());
    }

    rhi::TextureDesc coverage{};
    coverage.width = show::atlas_width;
    coverage.height = show::atlas_height;
    coverage.format = rhi::TextureFormat::Rgba8Unorm;
    coverage.sampled = true;
    auto atlas = device.create_texture(coverage);
    if (!atlas) {
        return Result::failure(atlas.error());
    }

    // Nearest, because the atlas stores coverage on an exact texel grid and an
    // integer upscale must land one texel per pixel rather than smear.
    auto sampler = device.create_sampler(rhi::Filter::Nearest);
    if (!sampler) {
        return Result::failure(sampler.error());
    }

    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = blobs.format;
    vertex.bytecode = blobs.vertex;
    vertex.storage_buffer_count = 1U;
    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = blobs.format;
    fragment.bytecode = blobs.fragment;
    fragment.sampler_count = 1U;

    auto pipeline = device.create_pipeline(
        vertex, fragment, rhi::TextureFormat::Rgba8Unorm, true);
    if (!pipeline) {
        return Result::failure(pipeline.error());
    }

    auto instances = device.create_storage_buffer(static_cast<core::u32>(
        sizeof(show::PatternQuad) * show::max_pattern_quads));
    if (!instances) {
        return Result::failure(instances.error());
    }

    GpuOutputTarget created{
        std::move(target).value(),
        std::move(atlas).value(),
        std::move(sampler).value(),
        std::move(pipeline).value(),
        std::move(instances).value(),
    };
    created.quads_.reset(
        new (std::nothrow) std::array<show::PatternQuad, show::max_pattern_quads>{});
    if (created.quads_ == nullptr) {
        return Result::failure(rhi::RhiError::AllocationFailed);
    }

    // The atlas is static, so it is uploaded exactly once here. This is the
    // only pixel data the GPU path ever sends.
    std::array<core::u8, show::atlas_rgba_bytes> pixels{};
    if (!show::write_glyph_atlas(pixels)) {
        return Result::failure(rhi::RhiError::InvalidConfiguration);
    }
    auto acquired = device.acquire_commands();
    if (!acquired) {
        return Result::failure(acquired.error());
    }
    auto commands = std::move(acquired).value();
    if (!commands.upload(created.atlas_, pixels)) {
        return Result::failure(rhi::RhiError::BackendFailure);
    }
    auto submitted = device.submit(std::move(commands));
    if (!submitted) {
        return Result::failure(submitted.error());
    }
    if (!std::move(submitted).value().wait()) {
        return Result::failure(rhi::RhiError::BackendFailure);
    }
    return Result::success(std::move(created));
}

bool GpuOutputTarget::record(
    rhi::CommandList& commands,
    const show::TestPatternInput& frame) noexcept {
    if (quads_ == nullptr) {
        return false;
    }

    const auto described = show::build_pattern_quads(
        frame, target_.width(), target_.height(), *quads_);
    if (described.truncated) {
        // A partial frame on one output and a whole one on another is exactly
        // the divergence this pattern exists to reveal, so it is a failure
        // rather than a frame with pieces missing.
        return false;
    }
    last_instances_ = described.count;

    const std::span<const core::u8> instance_bytes{
        reinterpret_cast<const core::u8*>(quads_->data()),
        described.count * sizeof(show::PatternQuad)};
    if (!commands.upload(instances_, instance_bytes)) {
        return false;
    }
    return commands.draw_instanced_quads(
        target_,
        rhi::Color{0.0F, 0.0F, 0.0F, 1.0F},
        pipeline_,
        instances_,
        static_cast<core::u32>(described.count),
        atlas_,
        sampler_);
}

show::PresenterReadback GpuOutputTarget::readback(rhi::Device& device) noexcept {
    const auto width = target_.width();
    const auto height = target_.height();
    const auto bytes = static_cast<core::u32>(width) * height * 4U;

    auto buffer = device.create_readback(bytes);
    if (!buffer) {
        return {};
    }
    auto acquired = device.acquire_commands();
    if (!acquired) {
        return {};
    }
    auto commands = std::move(acquired).value();
    if (!commands.download(target_, buffer.value())) {
        return {};
    }
    auto submitted = device.submit(std::move(commands));
    if (!submitted || !std::move(submitted).value().wait()) {
        return {};
    }

    const auto pixels = buffer.value().map();
    if (pixels.size() < bytes) {
        return {};
    }
    const show::FrameBufferView view{
        std::span<core::u8>{const_cast<core::u8*>(pixels.data()), pixels.size()},
        width,
        height,
        width * 4U,
        show::PixelFormat::Rgba8888,
    };
    const show::PresenterReadback report{
        width, height, show::frame_buffer_digest(view)};
    buffer.value().unmap();
    return report;
}

} // namespace mediaserver::ui
