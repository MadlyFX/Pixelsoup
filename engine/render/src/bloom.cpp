#include <mediaserver/render/bloom.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/bloom_combine_comp_dxil.h>
#include <mediaserver/shaders/bloom_downsample_comp_dxil.h>
#include <mediaserver/shaders/bloom_upsample_comp_dxil.h>
#endif

#include <algorithm>
#include <array>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using CreateResult = core::Result<Bloom, RenderError>;

[[nodiscard]] bool shaders_present() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return true;
#else
    return false;
#endif
}

[[nodiscard]] std::span<const core::u8> downsample_bytecode() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(
            mediaserver_bloom_downsample_comp_dxil),
        sizeof(mediaserver_bloom_downsample_comp_dxil)};
#else
    return {};
#endif
}

[[nodiscard]] std::span<const core::u8> combine_bytecode() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_bloom_combine_comp_dxil),
        sizeof(mediaserver_bloom_combine_comp_dxil)};
#else
    return {};
#endif
}

[[nodiscard]] std::span<const core::u8> upsample_bytecode() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(
            mediaserver_bloom_upsample_comp_dxil),
        sizeof(mediaserver_bloom_upsample_comp_dxil)};
#else
    return {};
#endif
}

// Scene-linear, like everything else on this path. Bloom is an addition of
// light to light; an eight-bit intermediate would quantise the very highlights
// it exists to spread.
[[nodiscard]] rhi::TextureDesc level_desc(
    core::u32 width, core::u32 height,
    rhi::TextureFormat format = rhi::TextureFormat::Rgba16Float) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.sampled = true;
    desc.storage_read = true;
    desc.storage_write = true;
    // Also a colour target, because a compositor that ends its frame on this
    // texture copies out of it - and a copy source that was never declared one
    // is refused by the backend, which surfaces as a backend failure at the
    // crop rather than anywhere near the pass that produced it.
    desc.color_target = true;
    return desc;
}

// Matches `numthreads(8, 8, 1)` in both shaders. Named here so a change to one
// is visibly a change to the other.
constexpr core::u32 bloom_group_size = 8U;

[[nodiscard]] core::u32 groups_for(core::u32 extent) noexcept {
    return (extent + bloom_group_size - 1U) / bloom_group_size;
}

// The same halving the chain planner uses, and it has to stay the same: a level
// that rounds differently here from there is a level the plan describes and the
// dispatch does not fill.
[[nodiscard]] core::u32 halve(core::u32 value) noexcept {
    return value == 0U ? 0U : (value + 1U) / 2U;
}

struct BloomConstants final {
    float extent[4U]{};
    float parameters[4U]{};
};

[[nodiscard]] std::span<const core::u8> bytes_of(
    const BloomConstants& value) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&value), sizeof(value)};
}

[[nodiscard]] core::Result<rhi::ComputePipeline, RenderError> make_pipeline(
    rhi::Device& device, std::span<const core::u8> bytecode) noexcept {
    using Result = core::Result<rhi::ComputePipeline, RenderError>;
    rhi::ShaderDesc compute{};
    compute.stage = rhi::ShaderStage::Compute;
    compute.format = rhi::ShaderFormat::Dxil;
    compute.bytecode = bytecode;
    compute.sampler_count = 1U;
    compute.storage_texture_count = 1U;
    compute.uniform_buffer_count = 1U;
    auto pipeline = device.create_compute_pipeline(compute);
    if (!pipeline) {
        return Result::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }
    return Result::success(std::move(pipeline).value());
}

} // namespace

bool Bloom::shaders_available() noexcept { return shaders_present(); }

core::Result<BloomChainPlan, BloomChainError> bloom_plan_for(
    const scene::Scene& scene, const SceneRenderView* view,
    const PaddedSlice& padded, core::u32 target_width,
    core::u32 target_height) noexcept {
    if (view != nullptr) {
        // The view's own raster is the grid. It has no canvas rectangle to sit
        // on, and the plate or surface it fills is rendered at its own
        // density, so the canvas's blocks describe nothing about it.
        const scene::Canvas raster{target_width, target_height};
        return plan_bloom_chain(raster, scene::whole(raster));
    }
    return plan_bloom_chain(scene.canvas, padded.render);
}

Bloom::Bloom(std::array<rhi::Texture, slots> levels,
             rhi::ComputePipeline downsample, rhi::ComputePipeline upsample,
             rhi::ComputePipeline combine, rhi::Sampler sampler,
             rhi::TextureFormat output_format) noexcept
    : output_format_(output_format),
      downsample_(std::move(downsample)),
      upsample_(std::move(upsample)), combine_(std::move(combine)),
      sampler_(std::move(sampler)) {
    for (auto& texture : levels) static_cast<void>(transients_.adopt(std::move(texture)));
    transients_.seal();
}

CreateResult Bloom::create(rhi::Device& device, core::u32 width,
                           core::u32 height,
                           rhi::TextureFormat output_format) noexcept {
    if (width == 0U || height == 0U) {
        return CreateResult::failure(RenderError::InvalidConfiguration);
    }
    if (!shaders_present()) {
        return CreateResult::failure(RenderError::ShaderFormatUnsupported);
    }
    if (!device.capabilities().compute
        || !device.capabilities().storage_images) {
        return CreateResult::failure(RenderError::CapabilityUnsupported);
    }

    // Every slot is allocated even when a canvas takes a shorter chain. The
    // deepest levels are a few hundred texels between them, and the alternative
    // is reallocating whenever the authored canvas changes shape - which is a
    // frame-time allocation on a path that must not have one.
    std::array<rhi::Texture, slots> levels{};
    core::u32 level_width = width;
    core::u32 level_height = height;
    for (std::size_t level = 0U; level < slots; ++level) {
        // Level zero is the published result and takes the caller's format;
        // the coarser levels stay scene-linear RGBA16F whatever it is.
        auto created = device.create_texture(level_desc(
            std::max(level_width, 1U), std::max(level_height, 1U),
            level == 0U ? output_format : rhi::TextureFormat::Rgba16Float));
        if (!created) {
            return CreateResult::failure(RenderError::AllocationFailed);
        }
        levels[level] = std::move(created).value();
        level_width = halve(level_width);
        level_height = halve(level_height);
    }

    auto downsample = make_pipeline(device, downsample_bytecode());
    if (!downsample) {
        return CreateResult::failure(downsample.error());
    }
    auto upsample = make_pipeline(device, upsample_bytecode());
    if (!upsample) {
        return CreateResult::failure(upsample.error());
    }
    auto combine = make_pipeline(device, combine_bytecode());
    if (!combine) {
        return CreateResult::failure(combine.error());
    }

    // Nearest, and never used: every tap in both shaders is a Load. The
    // sampler exists because the pipeline's table declares one.
    auto sampler = device.create_sampler(rhi::Filter::Nearest);
    if (!sampler) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }

    return CreateResult::success(Bloom{
        std::move(levels), std::move(downsample).value(),
        std::move(upsample).value(), std::move(combine).value(),
        std::move(sampler).value(), output_format});
}

bool Bloom::resize(rhi::Device& device, core::u32 width,
                   core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (this->level(0U).width() == width && this->level(0U).height() == height) {
        return true;
    }
    std::array<rhi::Texture, slots> levels{};
    core::u32 level_width = width;
    core::u32 level_height = height;
    for (std::size_t level = 0U; level < slots; ++level) {
        auto created = device.create_texture(level_desc(
            std::max(level_width, 1U), std::max(level_height, 1U),
            level == 0U ? output_format_ : rhi::TextureFormat::Rgba16Float));
        if (!created) {
            return false;
        }
        levels[level] = std::move(created).value();
        level_width = halve(level_width);
        level_height = halve(level_height);
    }
    GraphTransientPool replacement{{slots}};
    for (auto& texture : levels) if (!replacement.adopt(std::move(texture))) return false;
    replacement.seal();
    transients_ = std::move(replacement);
    return true;
}

bool Bloom::record(rhi::CommandList& commands, const rhi::Texture& source,
                   const BloomChainPlan& plan) noexcept {
    graph_.begin();
    transients_.begin_frame();
    for (std::size_t i = 0U; i < slots; ++i)
        if (!commands.device() || !transients_.acquire(*commands.device(), this->level(i).desc())) return false;
    if (plan.levels.empty()) {
        // A canvas too small to halve usefully. Nothing to record, and nothing
        // wrong either - the caller skips the combine because the strength it
        // would apply has no chain behind it.
        return true;
    }
    const core::u32 depth = plan.depth();
    if (depth == 0U || depth >= slots) {
        ++stats_.mismatches;
        return false;
    }
    // The plan describes a rectangle; these targets were built for one. If they
    // disagree the chain would filter into the wrong grid, which is exactly the
    // failure this whole file is arranged to prevent, so it is refused rather
    // than scaled.
    if (plan.levels[0U].width != this->level(0U).width()
        || plan.levels[0U].height != this->level(0U).height()
        || source.width() != this->level(0U).width()
        || source.height() != this->level(0U).height()) {
        ++stats_.mismatches;
        return false;
    }

    const auto dispatch = [&](const rhi::ComputePipeline& pipeline,
                              const rhi::Texture& from, std::size_t into,
                              bool accumulate) {
        BloomConstants constants{};
        constants.extent[0U] = static_cast<float>(this->level(into).width());
        constants.extent[1U] = static_cast<float>(this->level(into).height());
        constants.extent[2U] = static_cast<float>(from.width());
        constants.extent[3U] = static_cast<float>(from.height());
        constants.parameters[1U] = accumulate ? 1.0F : 0.0F;

        const std::array<rhi::SampledTexture, 1U> sources{
            rhi::SampledTexture{&from, &sampler_}};
        const std::array<rhi::StorageTexture, 1U> targets{
            rhi::StorageTexture{&this->level(into), 0U, 0U, true}};
        rhi::ComputeDispatch work{};
        work.pipeline = &pipeline;
        work.sampled_textures = sources;
        work.storage_textures = targets;
        work.uniform = bytes_of(constants);
        work.groups_x = groups_for(this->level(into).width());
        work.groups_y = groups_for(this->level(into).height());
        return graph_.dispatch(commands, &pipeline == &downsample_ ? core::RenderPass::BloomDown : core::RenderPass::BloomUp, work, accumulate);
    };

    // Down. The first level reads the scene colour directly rather than a copy
    // of it: level zero of the chain is the accumulator the walk back up
    // establishes, and prefiltering into it would be a full-resolution pass
    // that only ever copied.
    for (core::u32 level = 1U; level <= depth; ++level) {
        const rhi::Texture& from =
            level == 1U ? source : this->level(level - 1U);
        if (!dispatch(downsample_, from, level, false)) {
            return false;
        }
        ++stats_.dispatches;
    }

    // Up. Every level but the last accumulates into what the downsample left
    // there; level zero holds nothing, so its write establishes rather than
    // adds.
    for (core::u32 level = depth; level-- > 0U;) {
        if (!dispatch(upsample_, this->level(level + 1U), level, level != 0U)) {
            return false;
        }
        ++stats_.dispatches;
    }

    ++stats_.chains;
    return true;
}

bool Bloom::combine(rhi::CommandList& commands, const rhi::Texture& source,
                    float strength) noexcept {
    if (source.width() != this->level(0U).width()
        || source.height() != this->level(0U).height()) {
        ++stats_.mismatches;
        return false;
    }
    BloomConstants constants{};
    constants.extent[0U] = static_cast<float>(this->level(0U).width());
    constants.extent[1U] = static_cast<float>(this->level(0U).height());
    constants.extent[2U] = constants.extent[0U];
    constants.extent[3U] = constants.extent[1U];
    constants.parameters[0U] = strength;

    const std::array<rhi::SampledTexture, 1U> sources{
        rhi::SampledTexture{&source, &sampler_}};
    const std::array<rhi::StorageTexture, 1U> targets{
        rhi::StorageTexture{&this->level(0U), 0U, 0U, true}};
    rhi::ComputeDispatch work{};
    work.pipeline = &combine_;
    work.sampled_textures = sources;
    work.storage_textures = targets;
    work.uniform = bytes_of(constants);
    work.groups_x = groups_for(this->level(0U).width());
    work.groups_y = groups_for(this->level(0U).height());
    if (!graph_.dispatch(commands, core::RenderPass::BloomComposite, work, true)) {
        return false;
    }
    ++stats_.dispatches;
    return true;
}

} // namespace mediaserver::render
