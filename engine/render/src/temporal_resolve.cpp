#include <mediaserver/render/temporal_resolve.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/output_vert_dxil.h>
#include <mediaserver/shaders/taa_resolve_frag_dxil.h>
#endif

#include <array>
#include <limits>
#include <span>
#include <utility>

namespace mediaserver::render {
namespace {

using CreateResult = core::Result<TemporalResolve, RenderError>;
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
                mediaserver_taa_resolve_frag_dxil),
            sizeof(mediaserver_taa_resolve_frag_dxil)},
        rhi::ShaderFormat::Dxil, true};
#else
    return ShaderBlobs{};
#endif
}

[[nodiscard]] rhi::TextureDesc target_desc(
    core::u32 width, core::u32 height, rhi::TextureFormat format) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.color_target = true;
    desc.sampled = true;
    return desc;
}

// The resolve's constant buffer. Thirty-two bytes, matching taa_resolve.frag.
struct TaaConstants final {
    float parameters[4U]{};
    float canvas[4U]{};
};

[[nodiscard]] std::span<const core::u8> bytes_of(
    const TaaConstants& value) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&value), sizeof(value)};
}

} // namespace

bool TemporalResolve::shaders_available() noexcept {
    return shader_blobs().present;
}

TemporalResolve::TemporalResolve(
    rhi::Texture output, rhi::GraphicsPipeline pipeline, rhi::Sampler sampler,
    std::vector<Entry> entries) noexcept
    : output_(std::move(output)), pipeline_(std::move(pipeline)),
      sampler_(std::move(sampler)), entries_(std::move(entries)) {}

CreateResult TemporalResolve::create(
    rhi::Device& device, core::u32 width, core::u32 height,
    TemporalResolveLimits limits, rhi::TextureFormat format) noexcept {
    if (width == 0U || height == 0U
        || (format != rhi::TextureFormat::Rgba16Float
            && format != rhi::TextureFormat::R32Float)) {
        return CreateResult::failure(RenderError::InvalidConfiguration);
    }
    const ShaderBlobs blobs = shader_blobs();
    if (!blobs.present) {
        return CreateResult::failure(RenderError::ShaderFormatUnsupported);
    }

    auto output = device.create_texture(
        target_desc(width, height, format));
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
    // Current colour/motion/id and matching previous colour/id/motion-depth.
    fragment.sampler_count = 6U;
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

    std::vector<Entry> entries;
    const core::u32 count = limits.max_targets == 0U ? 1U : limits.max_targets;
    try {
        entries.resize(count);
    } catch (const std::bad_alloc&) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }

    return CreateResult::success(TemporalResolve{
        std::move(output).value(), std::move(pipeline).value(),
        std::move(sampler).value(), std::move(entries)});
}

bool TemporalResolve::resize(
    rhi::Device& device, core::u32 width, core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (output_.width() == width && output_.height() == height) {
        return true;
    }
    auto output = device.create_texture(
        target_desc(width, height, output_.desc().format));
    if (!output) {
        return false;
    }
    output_ = std::move(output).value();
    // Every history goes. A resize is a discontinuity and `TemporalHistory`
    // already reports Reset for one, so nothing that would have been used is
    // being discarded here.
    for (Entry& entry : entries_) {
        entry = Entry{};
    }
    return true;
}

void TemporalResolve::forget(core::u32 slot) noexcept {
    for (Entry& entry : entries_) {
        if (entry.occupied && entry.slot == slot) {
            entry = Entry{};
        }
    }
}

TemporalResolve::Entry* TemporalResolve::claim(
    rhi::Device& device, core::u32 slot) noexcept {
    Entry* empty = nullptr;
    Entry* least_recent = nullptr;
    for (Entry& entry : entries_) {
        if (entry.occupied && entry.slot == slot) {
            return &entry;
        }
        if (!entry.occupied) {
            if (empty == nullptr) {
                empty = &entry;
            }
            continue;
        }
        if (least_recent == nullptr
            || entry.last_used < least_recent->last_used) {
            least_recent = &entry;
        }
    }

    Entry* const target = empty != nullptr ? empty : least_recent;
    if (target == nullptr) {
        return nullptr;
    }
    if (target->occupied) {
        ++stats_.evictions;
    }
    // Reuse the textures when they already exist at the right extent: an
    // eviction is a change of owner, not a reason to reallocate.
    if (!target->color || !target->object_id || !target->motion_depth) {
        auto color = device.create_texture(target_desc(
            output_.width(), output_.height(),
            output_.desc().format));
        auto object_id = device.create_texture(target_desc(
            output_.width(), output_.height(), rhi::TextureFormat::R32Uint));
        auto motion_depth = device.create_texture(target_desc(
            output_.width(), output_.height(), rhi::TextureFormat::Rgba32Float));
        if (!color || !object_id || !motion_depth) {
            ++stats_.allocation_failures;
            return nullptr;
        }
        target->color = std::move(color).value();
        target->object_id = std::move(object_id).value();
        target->motion_depth = std::move(motion_depth).value();
        ++stats_.targets_allocated;
    }
    target->occupied = true;
    target->slot = slot;
    target->primed = false;
    return target;
}

bool TemporalResolve::store(
    rhi::CommandList& commands, Entry& entry, const rhi::Texture& color,
    const rhi::Texture& object_id, const rhi::Texture& motion_depth) noexcept {
    const rhi::ViewportRect whole{0U, 0U, output_.width(), output_.height()};
    const auto recording = commands.recording_token();
    if (!recording) return false;
    if (!graph_.copy(commands, core::RenderPass::Temporal, color, whole, entry.color)
        || !graph_.copy(commands, core::RenderPass::Temporal, object_id, whole, entry.object_id)
        || !graph_.copy(commands, core::RenderPass::Temporal, motion_depth, whole, entry.motion_depth)) {
        return false;
    }
    entry.recording = recording;
    entry.primed = true;
    return true;
}

ApplyResult TemporalResolve::apply(
    rhi::Device& device, rhi::CommandList& commands, core::u32 slot,
    HistoryDisposition disposition, const Sources& sources) noexcept {
    graph_.begin();
    if (sources.color == nullptr || sources.velocity == nullptr
        || sources.object_id == nullptr) {
        return ApplyResult::failure(RenderError::InvalidConfiguration);
    }
    if (sources.color->width() != output_.width()
        || sources.color->height() != output_.height()
        || sources.color->desc().format != output_.desc().format
        || sources.velocity->width() != output_.width()
        || sources.velocity->height() != output_.height()
        || sources.velocity->desc().format != rhi::TextureFormat::Rgba32Float
        || sources.object_id->width() != output_.width()
        || sources.object_id->height() != output_.height()
        || sources.object_id->desc().format != rhi::TextureFormat::R32Uint
        || sources.canvas_width == 0U || sources.canvas_height == 0U) {
        return ApplyResult::failure(RenderError::InvalidConfiguration);
    }

    Entry* const entry = claim(device, slot);
    if (entry == nullptr) {
        return ApplyResult::failure(RenderError::AllocationFailed);
    }

    // An abandoned recording never produced usable GPU history. Re-prime on
    // retry rather than returning uninitialized memory for a CPU-side Hold.
    if (entry->recording.outcome() == rhi::CommandRecordingOutcome::Canceled) {
        entry->primed = false;
    }

    // The disposition table. Every row is here, in one place, because this
    // mapping is the reason the temporal contract exists and scattering it
    // into conditionals is how two of its rows end up meaning the same thing.
    //
    // A Hold returns the previous resolve untouched and accumulates nothing: a
    // repeated show frame is not a new temporal sample, and treating it as one
    // would let a paused timeline converge on an image a running one never
    // reaches.
    if (disposition == HistoryDisposition::Hold && entry->primed) {
        entry->last_used = ++use_stamp_;
        ++stats_.holds;
        return ApplyResult::success(&entry->color);
    }

    const bool blending = disposition == HistoryDisposition::Advance
        && entry->primed;
    if (!blending) {
        // Initialize, Reset, a Hold with nothing held, and an Advance whose
        // history was evicted all take this path: publish the current frame
        // and make it the history.
        if (disposition == HistoryDisposition::Advance) {
            ++stats_.restarts;
        } else {
            ++stats_.initializes;
        }
        if (!store(commands, *entry, *sources.color, *sources.object_id, *sources.velocity)) {
            return ApplyResult::failure(RenderError::BackendFailure);
        }
        entry->last_used = ++use_stamp_;
        return ApplyResult::success(sources.color);
    }

    TaaConstants constants{};
    constants.parameters[0U] = static_cast<float>(output_.width());
    constants.parameters[1U] = static_cast<float>(output_.height());
    constants.parameters[2U] = temporal_history_weight;
    // Pixel extent of the projection that produced motion: the shared canvas
    // for ordinary slices, or the padded image for an explicit tracked view.
    constants.canvas[0U] = static_cast<float>(sources.canvas_width);
    constants.canvas[1U] = static_cast<float>(sources.canvas_height);

    const std::array<rhi::SampledTexture, 6U> inputs{{
        rhi::SampledTexture{sources.color, &sampler_},
        rhi::SampledTexture{sources.velocity, &sampler_},
        rhi::SampledTexture{sources.object_id, &sampler_},
        rhi::SampledTexture{&entry->color, &sampler_},
        rhi::SampledTexture{&entry->object_id, &sampler_},
        rhi::SampledTexture{&entry->motion_depth, &sampler_},
    }};
    if (!graph_.fullscreen(commands, core::RenderPass::Temporal,
            output_, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}, pipeline_, inputs,
            bytes_of(constants))) {
        return ApplyResult::failure(RenderError::BackendFailure);
    }
    // The resolved frame becomes the history, together with the object ids it
    // was resolved against, so next frame's rejection compares like with like.
    if (!store(commands, *entry, output_, *sources.object_id, *sources.velocity)) {
        return ApplyResult::failure(RenderError::BackendFailure);
    }
    entry->last_used = ++use_stamp_;
    ++stats_.resolves;
    return ApplyResult::success(&output_);
}

} // namespace mediaserver::render
