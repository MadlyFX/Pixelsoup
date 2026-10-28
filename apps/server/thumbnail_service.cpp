#include "thumbnail_service.hpp"

#include <mediaserver/assets/asset_path.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/assets/proxy_frame_map.hpp>
#include <mediaserver/render/media_source.hpp>
#include <mediaserver/rhi/device.hpp>

// These are the repository's existing header-only image utility seams. Keep
// their implementation local/static so the renderer remains the sole owner of
// stb_image decoding and no symbols leak across static-library boundaries.
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb_image_resize2.h>
#include <stb_image_write.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace mediaserver::server {
namespace {

[[nodiscard]] ThumbnailServiceError service_error(
    ThumbnailServiceErrorCode code) noexcept {
    return ThumbnailServiceError{code};
}

[[nodiscard]] bool expected_rgba_bytes(core::u32 width, core::u32 height,
                                       std::size_t& result) noexcept {
    const core::u64 pixels = static_cast<core::u64>(width) * height;
    if (pixels > std::numeric_limits<std::size_t>::max() / 4U) {
        return false;
    }
    result = static_cast<std::size_t>(pixels * 4U);
    return true;
}

[[nodiscard]] ThumbnailServiceConfig normalized_config(
    ThumbnailServiceConfig config) noexcept {
    config.worker_count = std::clamp(
        config.worker_count, 1U, maximum_thumbnail_workers);
    config.maximum_pending_requests = std::clamp(
        config.maximum_pending_requests, 1U,
        maximum_thumbnail_pending_requests);
    config.maximum_prefetch_requests = std::min(
        config.maximum_prefetch_requests, config.maximum_pending_requests);
    const core::u32 minimum_snapshots = config.maximum_pending_requests +
        config.worker_count;
    config.maximum_snapshots = std::clamp(
        config.maximum_snapshots, minimum_snapshots,
        maximum_thumbnail_snapshots);
    return config;
}

class RendererThumbnailDecoder final : public ThumbnailDecoder {
public:
    RendererThumbnailDecoder() : source_(render::make_media_source()) {}

    core::u32 frame_count(const std::string& source) override {
        return render::probe_frame_count(source);
    }

    ThumbnailRgbaFrame decode(const std::string& source,
                              core::u32 source_ordinal,
                              std::stop_token stop) override {
        if (stop.stop_requested()) {
            return {};
        }
        render::DecodedFrame decoded = source_->decode(source, source_ordinal);
        if (stop.stop_requested() || !decoded.valid() ||
            decoded.format != rhi::TextureFormat::Rgba8Unorm) {
            return {};
        }
        return ThumbnailRgbaFrame{
            decoded.width,
            decoded.height,
            std::move(decoded.pixels),
            0U,
            1U,
            1U,
        };
    }

private:
    std::unique_ptr<render::MediaSource> source_;
};

[[nodiscard]] std::optional<ThumbnailRgbaFrame> rotated_frame(
    ThumbnailRgbaFrame frame, std::stop_token stop) {
    const core::u8 turns = frame.clockwise_quarter_turns;
    if (turns == 0U) {
        return frame;
    }
    const core::u32 output_width = (turns % 2U) != 0U
        ? frame.height
        : frame.width;
    const core::u32 output_height = (turns % 2U) != 0U
        ? frame.width
        : frame.height;
    std::size_t output_bytes = 0U;
    if (!expected_rgba_bytes(output_width, output_height, output_bytes)) {
        return std::nullopt;
    }
    std::vector<core::u8> output(output_bytes);
    for (core::u32 y = 0U; y < frame.height; ++y) {
        if (stop.stop_requested()) {
            return std::nullopt;
        }
        for (core::u32 x = 0U; x < frame.width; ++x) {
            core::u32 destination_x = x;
            core::u32 destination_y = y;
            switch (turns) {
            case 1U:
                destination_x = frame.height - 1U - y;
                destination_y = x;
                break;
            case 2U:
                destination_x = frame.width - 1U - x;
                destination_y = frame.height - 1U - y;
                break;
            case 3U:
                destination_x = y;
                destination_y = frame.width - 1U - x;
                break;
            default:
                return std::nullopt;
            }
            const std::size_t source_offset =
                (static_cast<std::size_t>(y) * frame.width + x) * 4U;
            const std::size_t destination_offset =
                (static_cast<std::size_t>(destination_y) * output_width +
                 destination_x) * 4U;
            std::copy_n(frame.pixels.begin() +
                            static_cast<std::ptrdiff_t>(source_offset),
                        4U,
                        output.begin() +
                            static_cast<std::ptrdiff_t>(destination_offset));
        }
    }
    frame.width = output_width;
    frame.height = output_height;
    frame.pixels = std::move(output);
    frame.clockwise_quarter_turns = 0U;
    if ((turns % 2U) != 0U) {
        std::swap(frame.pixel_aspect_numerator,
                  frame.pixel_aspect_denominator);
    }
    return frame;
}

[[nodiscard]] core::u32 rounded_ratio(core::u64 numerator,
                                      core::u64 denominator) noexcept {
    return static_cast<core::u32>(
        (numerator + denominator / 2U) / denominator);
}

struct PngWriteContext final {
    std::vector<core::u8> bytes;
    bool failed{};
};

void png_write(void* opaque, void* data, int size) noexcept {
    auto& context = *static_cast<PngWriteContext*>(opaque);
    if (context.failed || data == nullptr || size <= 0) {
        return;
    }
    try {
        const auto* first = static_cast<const core::u8*>(data);
        context.bytes.insert(context.bytes.end(), first, first + size);
    } catch (...) {
        context.failed = true;
    }
}

struct FittedRgba final {
    core::u32 width{};
    core::u32 height{};
    std::vector<core::u8> pixels;
};

[[nodiscard]] std::optional<FittedRgba> fit_frame(
    ThumbnailRgbaFrame frame, core::u32 target_width,
    core::u32 target_height, std::stop_token stop) {
    if (!frame.valid() || stop.stop_requested() ||
        target_width == 0U || target_height == 0U ||
        target_width > static_cast<core::u32>(
                            std::numeric_limits<int>::max()) ||
        target_height > static_cast<core::u32>(
                             std::numeric_limits<int>::max())) {
        return std::nullopt;
    }
    std::optional<ThumbnailRgbaFrame> rotated = rotated_frame(
        std::move(frame), stop);
    if (!rotated.has_value() || stop.stop_requested()) {
        return std::nullopt;
    }
    frame = std::move(*rotated);
    if (frame.width > static_cast<core::u32>(
                          std::numeric_limits<int>::max() / 4) ||
        frame.height > static_cast<core::u32>(
                           std::numeric_limits<int>::max())) {
        return std::nullopt;
    }

    // The validation cap keeps all three-factor scale products inside u64
    // while admitting every ordinary rational sample-aspect value.
    constexpr core::u32 maximum_pixel_aspect_component = 1'000'000U;
    if (frame.pixel_aspect_numerator == 0U ||
        frame.pixel_aspect_denominator == 0U ||
        frame.pixel_aspect_numerator > maximum_pixel_aspect_component ||
        frame.pixel_aspect_denominator > maximum_pixel_aspect_component) {
        return std::nullopt;
    }

    const core::u64 display_numerator =
        static_cast<core::u64>(frame.width) *
        frame.pixel_aspect_numerator;
    const core::u64 display_denominator =
        static_cast<core::u64>(frame.height) *
        frame.pixel_aspect_denominator;
    const auto display_aspect = assets::make_presentation_time(
        display_numerator, display_denominator);
    const auto target_aspect = assets::make_presentation_time(
        target_width, target_height);
    if (!display_aspect || !target_aspect) {
        return std::nullopt;
    }

    core::u32 scaled_width = target_width;
    core::u32 scaled_height = target_height;
    if (assets::compare_presentation_time(display_aspect.value(),
                                          target_aspect.value()) > 0) {
        const core::u64 numerator =
            static_cast<core::u64>(target_width) * frame.height *
            frame.pixel_aspect_denominator;
        const core::u64 denominator =
            static_cast<core::u64>(frame.width) *
            frame.pixel_aspect_numerator;
        scaled_height = std::clamp(rounded_ratio(numerator, denominator),
                                   1U, target_height);
    } else {
        const core::u64 numerator =
            static_cast<core::u64>(target_height) * frame.width *
            frame.pixel_aspect_numerator;
        const core::u64 denominator =
            static_cast<core::u64>(frame.height) *
            frame.pixel_aspect_denominator;
        scaled_width = std::clamp(rounded_ratio(numerator, denominator),
                                  1U, target_width);
    }

    std::size_t resized_bytes = 0U;
    std::size_t canvas_bytes = 0U;
    if (!expected_rgba_bytes(scaled_width, scaled_height, resized_bytes) ||
        !expected_rgba_bytes(target_width, target_height, canvas_bytes)) {
        return std::nullopt;
    }
    std::vector<core::u8> resized(resized_bytes);
    if (stbir_resize_uint8_srgb(
            frame.pixels.data(), static_cast<int>(frame.width),
            static_cast<int>(frame.height),
            static_cast<int>(frame.width * 4U), resized.data(),
            static_cast<int>(scaled_width), static_cast<int>(scaled_height),
            static_cast<int>(scaled_width * 4U), STBIR_RGBA) == nullptr ||
        stop.stop_requested()) {
        return std::nullopt;
    }

    std::vector<core::u8> canvas(canvas_bytes, 0U);
    const core::u32 offset_x = (target_width - scaled_width) / 2U;
    const core::u32 offset_y = (target_height - scaled_height) / 2U;
    for (core::u32 row = 0U; row < scaled_height; ++row) {
        if (stop.stop_requested()) {
            return std::nullopt;
        }
        const std::size_t source_offset =
            static_cast<std::size_t>(row) * scaled_width * 4U;
        const std::size_t destination_offset =
            (static_cast<std::size_t>(row + offset_y) * target_width +
             offset_x) * 4U;
        std::copy_n(resized.begin() +
                        static_cast<std::ptrdiff_t>(source_offset),
                    static_cast<std::size_t>(scaled_width) * 4U,
                    canvas.begin() +
                        static_cast<std::ptrdiff_t>(destination_offset));
    }

    return FittedRgba{target_width, target_height, std::move(canvas)};
}

[[nodiscard]] std::optional<std::vector<core::u8>> encode_png(
    core::u32 width, core::u32 height, std::span<const core::u8> pixels,
    std::stop_token stop) {
    std::size_t expected = 0U;
    if (stop.stop_requested() ||
        width > static_cast<core::u32>(std::numeric_limits<int>::max() / 4) ||
        height > static_cast<core::u32>(std::numeric_limits<int>::max()) ||
        !expected_rgba_bytes(width, height, expected) ||
        pixels.size() != expected) {
        return std::nullopt;
    }
    PngWriteContext output;
    const int encoded = stbi_write_png_to_func(
        &png_write, &output, static_cast<int>(width),
        static_cast<int>(height), 4, pixels.data(),
        static_cast<int>(width * 4U));
    if (encoded == 0 || output.failed || output.bytes.size() < 24U ||
        stop.stop_requested()) {
        return std::nullopt;
    }
    constexpr std::array<core::u8, 8U> png_signature{{
        0x89U, 'P', 'N', 'G', 0x0dU, 0x0aU, 0x1aU, 0x0aU,
    }};
    if (!std::equal(png_signature.begin(), png_signature.end(),
                    output.bytes.begin())) {
        return std::nullopt;
    }
    return std::move(output.bytes);
}

[[nodiscard]] std::optional<std::vector<core::u8>> encode_thumbnail(
    ThumbnailRgbaFrame frame, const assets::DerivativeProfile& profile,
    std::stop_token stop) {
    auto fitted = fit_frame(std::move(frame), profile.width, profile.height,
                            stop);
    if (!fitted.has_value()) {
        return std::nullopt;
    }
    return encode_png(fitted->width, fitted->height, fitted->pixels, stop);
}

[[nodiscard]] std::optional<std::vector<core::u8>> encode_filmstrip(
    std::vector<ThumbnailRgbaFrame> frames,
    const assets::DerivativeProfile& profile, std::stop_token stop) {
    if (profile.sample_count == 0U ||
        frames.size() != profile.sample_count ||
        profile.width > std::numeric_limits<core::u32>::max() /
                            profile.sample_count) {
        return std::nullopt;
    }
    const core::u32 atlas_width = profile.width * profile.sample_count;
    std::size_t atlas_bytes = 0U;
    if (!expected_rgba_bytes(atlas_width, profile.height, atlas_bytes)) {
        return std::nullopt;
    }
    std::vector<core::u8> atlas(atlas_bytes, 0U);
    for (std::size_t tile_index = 0U; tile_index < frames.size();
         ++tile_index) {
        if (stop.stop_requested()) {
            return std::nullopt;
        }
        auto tile = fit_frame(std::move(frames[tile_index]), profile.width,
                              profile.height, stop);
        if (!tile.has_value()) {
            return std::nullopt;
        }
        const std::size_t tile_x = tile_index * profile.width;
        for (core::u32 row = 0U; row < profile.height; ++row) {
            const std::size_t source_offset =
                static_cast<std::size_t>(row) * profile.width * 4U;
            const std::size_t destination_offset =
                (static_cast<std::size_t>(row) * atlas_width + tile_x) * 4U;
            std::copy_n(
                tile->pixels.begin() +
                    static_cast<std::ptrdiff_t>(source_offset),
                static_cast<std::size_t>(profile.width) * 4U,
                atlas.begin() +
                    static_cast<std::ptrdiff_t>(destination_offset));
        }
    }
    return encode_png(atlas_width, profile.height, atlas, stop);
}

[[nodiscard]] bool terminal(const assets::DerivativeState state) noexcept {
    return state == assets::DerivativeState::Ready ||
        state == assets::DerivativeState::Stale ||
        state == assets::DerivativeState::Failed ||
        state == assets::DerivativeState::Cancelled;
}

} // namespace

bool ThumbnailRgbaFrame::valid() const noexcept {
    std::size_t expected = 0U;
    return width != 0U && height != 0U && clockwise_quarter_turns < 4U &&
        pixel_aspect_numerator != 0U && pixel_aspect_denominator != 0U &&
        expected_rgba_bytes(width, height, expected) &&
        pixels.size() == expected;
}

ThumbnailDecoderFactory default_thumbnail_decoder_factory() {
    return [] { return std::make_unique<RendererThumbnailDecoder>(); };
}

struct ThumbnailService::Impl final {
    struct Job final {
        assets::DerivativeKey key;
        assets::MediaAsset asset;
        // A model job is drawn by the editor rather than decoded here: it
        // waits for take_model_renders, then for its picture.
        std::optional<assets::ModelAsset> model;
        bool waiting{};
        std::vector<core::u8> rendered;
        assets::DerivativeState state{assets::DerivativeState::Missing};
        ThumbnailRequestPriority priority{ThumbnailRequestPriority::Visible};
        ThumbnailFailureCode failure{ThumbnailFailureCode::None};
        std::optional<assets::DerivativeCacheErrorCode> cache_error;
        std::optional<core::u64> source_ordinal;
        std::vector<core::u64> source_ordinals;
        core::u64 attempted_frames{};
        core::u64 revision{};
        core::u64 stamp{};
        std::stop_source cancellation;
        bool enqueued{};
        bool in_flight{};
    };

    Impl(assets::DerivativeCache value, ThumbnailServiceConfig requested_config,
         ThumbnailDecoderFactory factory)
        : cache(std::move(value)), config(normalized_config(requested_config)),
          decoder_factory(std::move(factory)) {
        if (!decoder_factory) {
            decoder_factory = default_thumbnail_decoder_factory();
        }
        workers.reserve(config.worker_count);
        try {
            for (core::u32 index = 0U; index < config.worker_count; ++index) {
                static_cast<void>(index);
                workers.emplace_back([this] { worker_loop(); });
            }
        } catch (...) {
            shutdown();
            throw;
        }
    }

    ~Impl() { shutdown(); }

    void shutdown() noexcept {
        {
            std::lock_guard lock{mutex};
            if (!stopping) {
                stopping = true;
                for (auto& pair : jobs) {
                    Job& job = *pair.second;
                    job.cancellation.request_stop();
                    if (job.enqueued) {
                        release_queue_slot_locked(job);
                        job.state = assets::DerivativeState::Cancelled;
                        ++job.revision;
                    }
                }
            }
        }
        ready.notify_all();
        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers.clear();
    }

    [[nodiscard]] ThumbnailSnapshot snapshot_locked(const Job& job) const {
        return ThumbnailSnapshot{
            job.key,
            job.state,
            job.priority,
            job.failure,
            job.cache_error,
            job.source_ordinal,
            job.attempted_frames,
            job.revision,
            assets::derivative_cache_path(cache.root(), job.key),
            job.source_ordinals,
        };
    }

    void set_state_locked(Job& job, assets::DerivativeState state,
                          ThumbnailFailureCode failure =
                              ThumbnailFailureCode::None) noexcept {
        job.state = state;
        job.failure = failure;
        job.cache_error.reset();
        job.stamp = ++clock;
        ++job.revision;
    }

    void release_queue_slot_locked(Job& job) noexcept {
        if (!job.enqueued) {
            return;
        }
        job.enqueued = false;
        --queued_count;
        if (job.priority == ThumbnailRequestPriority::Prefetch) {
            --prefetch_count;
        }
    }

    void cancel_locked(Job& job,
                       ThumbnailFailureCode failure =
                           ThumbnailFailureCode::None) noexcept {
        job.cancellation.request_stop();
        release_queue_slot_locked(job);
        if (job.waiting) {
            job.waiting = false;
            --waiting_count;
        }
        if (job.state == assets::DerivativeState::Queued ||
            job.state == assets::DerivativeState::Generating ||
            (job.state == assets::DerivativeState::Stale && job.in_flight)) {
            set_state_locked(job, assets::DerivativeState::Cancelled, failure);
        }
    }

    [[nodiscard]] bool evict_prefetch_locked() noexcept {
        while (!prefetch.empty()) {
            std::shared_ptr<Job> candidate = std::move(prefetch.front());
            prefetch.pop_front();
            if (!candidate->enqueued ||
                candidate->priority != ThumbnailRequestPriority::Prefetch) {
                continue;
            }
            cancel_locked(*candidate, ThumbnailFailureCode::QueueCapacity);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool enqueue_locked(
        const std::shared_ptr<Job>& job) noexcept {
        if (job->priority == ThumbnailRequestPriority::Prefetch &&
            prefetch_count >= config.maximum_prefetch_requests) {
            set_state_locked(*job, assets::DerivativeState::Cancelled,
                             ThumbnailFailureCode::QueueCapacity);
            return false;
        }
        if (queued_count >= config.maximum_pending_requests) {
            if (job->priority == ThumbnailRequestPriority::Prefetch ||
                !evict_prefetch_locked()) {
                set_state_locked(*job, assets::DerivativeState::Cancelled,
                                 ThumbnailFailureCode::QueueCapacity);
                return false;
            }
        }
        job->enqueued = true;
        ++queued_count;
        if (job->priority == ThumbnailRequestPriority::Visible) {
            visible.push_back(job);
        } else {
            ++prefetch_count;
            prefetch.push_back(job);
        }
        ready.notify_one();
        return true;
    }

    [[nodiscard]] std::shared_ptr<Job> take_locked() noexcept {
        auto take_from = [this](std::deque<std::shared_ptr<Job>>& queue) {
            while (!queue.empty()) {
                std::shared_ptr<Job> job = std::move(queue.front());
                queue.pop_front();
                if (!job->enqueued || job->cancellation.stop_requested()) {
                    continue;
                }
                release_queue_slot_locked(*job);
                return job;
            }
            return std::shared_ptr<Job>{};
        };
        if (std::shared_ptr<Job> job = take_from(visible)) {
            return job;
        }
        return take_from(prefetch);
    }

    void prune_locked() {
        while (jobs.size() > config.maximum_snapshots) {
            auto oldest = jobs.end();
            for (auto candidate = jobs.begin(); candidate != jobs.end();
                 ++candidate) {
                const Job& job = *candidate->second;
                if (job.enqueued || job.in_flight || !terminal(job.state)) {
                    continue;
                }
                if (oldest == jobs.end() ||
                    job.stamp < oldest->second->stamp) {
                    oldest = candidate;
                }
            }
            if (oldest == jobs.end()) {
                return;
            }
            jobs.erase(oldest);
        }
    }

    void finish(const std::shared_ptr<Job>& job,
                assets::DerivativeState state,
                ThumbnailFailureCode failure = ThumbnailFailureCode::None,
                std::optional<assets::DerivativeCacheErrorCode> cache_error =
                    std::nullopt) noexcept {
        std::lock_guard lock{mutex};
        job->in_flight = false;
        if (job->state == assets::DerivativeState::Cancelled ||
            job->cancellation.stop_requested()) {
            if (job->state != assets::DerivativeState::Cancelled) {
                set_state_locked(*job, assets::DerivativeState::Cancelled);
            }
        } else {
            set_state_locked(*job, state, failure);
            job->cache_error = cache_error;
        }
        prune_locked();
    }

    // A model's picture came back from the editor: encode it, check the
    // file is still the revision it was drawn from, and publish.
    void process_model(const std::shared_ptr<Job>& job) {
        const std::stop_token stop = job->cancellation.get_token();
        const assets::DerivativeProfile* profile =
            assets::derivative_profile(job->key.profile);
        std::vector<core::u8> pixels = std::move(job->rendered);
        job->rendered.clear();
        std::size_t expected = 0U;
        if (profile == nullptr ||
            !expected_rgba_bytes(profile->width, profile->height, expected) ||
            pixels.size() != expected) {
            finish(job, assets::DerivativeState::Failed,
                   ThumbnailFailureCode::NoValidFrame);
            return;
        }
        std::optional<std::vector<core::u8>> encoded;
        try {
            encoded = encode_png(profile->width, profile->height, pixels, stop);
        } catch (...) {
            encoded.reset();
        }
        if (stop.stop_requested()) {
            finish(job, assets::DerivativeState::Cancelled);
            return;
        }
        if (!encoded.has_value()) {
            finish(job, assets::DerivativeState::Failed,
                   ThumbnailFailureCode::EncodeFailed);
            return;
        }
        const auto fingerprint =
            assets::source_fingerprint(job->model->source, stop);
        if (!fingerprint) {
            if (fingerprint.error().code ==
                assets::ContentFingerprintErrorCode::Cancelled) {
                finish(job, assets::DerivativeState::Cancelled);
            } else {
                finish(job, assets::DerivativeState::Failed,
                       ThumbnailFailureCode::SourceUnavailable);
            }
            return;
        }
        if (!job->model->fingerprint.has_value() ||
            fingerprint.value() != *job->model->fingerprint) {
            finish(job, assets::DerivativeState::Stale,
                   ThumbnailFailureCode::FingerprintMismatch);
            return;
        }
        const auto published = cache.publish(job->key, *encoded, false, stop);
        if (!published) {
            finish(job, assets::DerivativeState::Failed,
                   ThumbnailFailureCode::CacheFailure,
                   published.error().code);
            return;
        }
        if (published.value().state == assets::DerivativeState::Cancelled) {
            finish(job, assets::DerivativeState::Cancelled);
            return;
        }
        finish(job,
               published.value().state == assets::DerivativeState::Ready
                   ? assets::DerivativeState::Ready
                   : assets::DerivativeState::Failed,
               published.value().state == assets::DerivativeState::Ready
                   ? ThumbnailFailureCode::None
                   : ThumbnailFailureCode::CacheFailure);
    }

    void process(const std::shared_ptr<Job>& job, ThumbnailDecoder* decoder) {
        if (job->model.has_value()) {
            process_model(job);
            return;
        }
        const std::stop_token stop = job->cancellation.get_token();
        if (decoder == nullptr) {
            finish(job, assets::DerivativeState::Failed,
                   ThumbnailFailureCode::InvalidFrame);
            return;
        }
        const auto actual_fingerprint = assets::source_fingerprint(
            job->asset.source, stop);
        if (!actual_fingerprint) {
            if (actual_fingerprint.error().code ==
                assets::ContentFingerprintErrorCode::Cancelled) {
                finish(job, assets::DerivativeState::Cancelled);
            } else {
                finish(job, assets::DerivativeState::Failed,
                       ThumbnailFailureCode::SourceUnavailable);
            }
            return;
        }
        if (!job->asset.fingerprint.has_value() ||
            actual_fingerprint.value() != *job->asset.fingerprint) {
            finish(job, assets::DerivativeState::Stale,
                   ThumbnailFailureCode::FingerprintMismatch);
            return;
        }
        if (stop.stop_requested()) {
            finish(job, assets::DerivativeState::Cancelled);
            return;
        }

        core::u32 frame_count = job->asset.frame_count.value_or(0U);
        if (frame_count == 0U) {
            frame_count = decoder->frame_count(job->asset.source);
        }
        if (frame_count == 0U) {
            finish(job, assets::DerivativeState::Failed,
                   ThumbnailFailureCode::NoValidFrame);
            return;
        }

        const assets::DerivativeProfile* profile = assets::derivative_profile(
            job->key.profile);
        const bool is_thumbnail = profile != nullptr &&
            profile->id ==
                assets::DerivativeProfileId::Thumbnail320x180Srgb;
        const bool is_filmstrip = profile != nullptr &&
            profile->id ==
                assets::DerivativeProfileId::Filmstrip8x160x90Srgb;
        if (!is_thumbnail && !is_filmstrip) {
            finish(job, assets::DerivativeState::Failed,
                   ThumbnailFailureCode::EncodeFailed);
            return;
        }

        ThumbnailRgbaFrame decoded;
        std::vector<ThumbnailRgbaFrame> filmstrip_frames;
        std::vector<core::u64> selected_ordinals;
        if (is_thumbnail) {
            bool found = false;
            for (core::u64 attempt = 0U; attempt < frame_count; ++attempt) {
                if (stop.stop_requested()) {
                    finish(job, assets::DerivativeState::Cancelled);
                    return;
                }
                const std::optional<core::u64> ordinal =
                    assets::thumbnail_probe_ordinal(frame_count, attempt);
                if (!ordinal.has_value() ||
                    *ordinal > std::numeric_limits<core::u32>::max()) {
                    break;
                }
                {
                    std::lock_guard lock{mutex};
                    job->source_ordinal = *ordinal;
                    job->attempted_frames = attempt + 1U;
                    job->stamp = ++clock;
                    ++job->revision;
                }
                decoded = decoder->decode(
                    job->asset.source, static_cast<core::u32>(*ordinal), stop);
                if (decoded.valid()) {
                    selected_ordinals.push_back(*ordinal);
                    found = true;
                    break;
                }
            }
            if (!found) {
                finish(job, assets::DerivativeState::Failed,
                       ThumbnailFailureCode::NoValidFrame);
                return;
            }
        } else {
            const auto ordinals = assets::filmstrip_source_ordinals(
                frame_count);
            if (!ordinals.has_value()) {
                finish(job, assets::DerivativeState::Failed,
                       ThumbnailFailureCode::NoValidFrame);
                return;
            }
            filmstrip_frames.reserve(ordinals->size());
            selected_ordinals.reserve(ordinals->size());
            for (core::u64 ordinal : *ordinals) {
                if (stop.stop_requested()) {
                    finish(job, assets::DerivativeState::Cancelled);
                    return;
                }
                if (ordinal > std::numeric_limits<core::u32>::max()) {
                    finish(job, assets::DerivativeState::Failed,
                           ThumbnailFailureCode::NoValidFrame);
                    return;
                }
                ThumbnailRgbaFrame frame = decoder->decode(
                    job->asset.source, static_cast<core::u32>(ordinal), stop);
                if (!frame.valid()) {
                    finish(job, assets::DerivativeState::Failed,
                           ThumbnailFailureCode::NoValidFrame);
                    return;
                }
                selected_ordinals.push_back(ordinal);
                filmstrip_frames.push_back(std::move(frame));
                {
                    std::lock_guard lock{mutex};
                    job->source_ordinal = ordinal;
                    job->attempted_frames = selected_ordinals.size();
                    job->stamp = ++clock;
                    ++job->revision;
                }
            }
        }
        if (stop.stop_requested()) {
            finish(job, assets::DerivativeState::Cancelled);
            return;
        }
        {
            std::lock_guard lock{mutex};
            job->source_ordinals = selected_ordinals;
            if (!selected_ordinals.empty()) {
                job->source_ordinal = selected_ordinals.front();
            }
            job->stamp = ++clock;
            ++job->revision;
        }

        std::optional<std::vector<core::u8>> encoded;
        try {
            encoded = is_thumbnail
                ? encode_thumbnail(std::move(decoded), *profile, stop)
                : encode_filmstrip(std::move(filmstrip_frames), *profile,
                                   stop);
        } catch (...) {
            encoded.reset();
        }
        if (stop.stop_requested()) {
            finish(job, assets::DerivativeState::Cancelled);
            return;
        }
        if (!encoded.has_value()) {
            finish(job, assets::DerivativeState::Failed,
                   ThumbnailFailureCode::EncodeFailed);
            return;
        }

        // Decode opens the source through the renderer seam after the first
        // metadata check. Re-check immediately before publication so a file
        // resized or replaced while decoding is not committed under an older
        // live revision key.
        const auto final_fingerprint = assets::source_fingerprint(
            job->asset.source, stop);
        if (!final_fingerprint) {
            if (final_fingerprint.error().code ==
                assets::ContentFingerprintErrorCode::Cancelled) {
                finish(job, assets::DerivativeState::Cancelled);
            } else {
                finish(job, assets::DerivativeState::Failed,
                       ThumbnailFailureCode::SourceUnavailable);
            }
            return;
        }
        if (final_fingerprint.value() != *job->asset.fingerprint) {
            finish(job, assets::DerivativeState::Stale,
                   ThumbnailFailureCode::FingerprintMismatch);
            return;
        }

        const auto published = cache.publish(job->key, *encoded, false, stop);
        if (!published) {
            finish(job, assets::DerivativeState::Failed,
                   ThumbnailFailureCode::CacheFailure,
                   published.error().code);
            return;
        }
        if (published.value().state == assets::DerivativeState::Cancelled) {
            finish(job, assets::DerivativeState::Cancelled);
            return;
        }
        if (published.value().state != assets::DerivativeState::Ready) {
            finish(job, assets::DerivativeState::Failed,
                   ThumbnailFailureCode::CacheFailure);
            return;
        }
        finish(job, assets::DerivativeState::Ready);
    }

    void worker_loop() noexcept {
        std::unique_ptr<ThumbnailDecoder> decoder;
        try {
            decoder = decoder_factory();
        } catch (...) {
            decoder.reset();
        }
        while (true) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock lock{mutex};
                ready.wait(lock, [this] {
                    return stopping || queued_count != 0U;
                });
                if (stopping) {
                    return;
                }
                job = take_locked();
                if (!job) {
                    continue;
                }
                job->in_flight = true;
                set_state_locked(*job, assets::DerivativeState::Generating);
            }
            try {
                process(job, decoder.get());
            } catch (...) {
                finish(job, assets::DerivativeState::Failed,
                       ThumbnailFailureCode::InvalidFrame);
            }
        }
    }

    assets::DerivativeCache cache;
    ThumbnailServiceConfig config;
    ThumbnailDecoderFactory decoder_factory;
    mutable std::mutex mutex;
    std::condition_variable ready;
    bool stopping{};
    std::vector<std::thread> workers;
    std::deque<std::shared_ptr<Job>> visible;
    std::deque<std::shared_ptr<Job>> prefetch;
    // Model jobs waiting for the editor to draw them, visible first.
    std::deque<std::shared_ptr<Job>> visible_renders;
    std::deque<std::shared_ptr<Job>> prefetch_renders;
    core::u32 waiting_count{};
    std::unordered_map<std::string, std::shared_ptr<Job>> jobs;
    core::u32 queued_count{};
    core::u32 prefetch_count{};
    core::u64 clock{};
};

ThumbnailService::ThumbnailService(assets::DerivativeCache cache,
                                   ThumbnailServiceConfig config)
    : ThumbnailService(std::move(cache), config,
                       default_thumbnail_decoder_factory()) {}

ThumbnailService::ThumbnailService(
    assets::DerivativeCache cache, ThumbnailServiceConfig config,
    ThumbnailDecoderFactory decoder_factory)
    : implementation_(std::make_unique<Impl>(
          std::move(cache), config, std::move(decoder_factory))) {}

ThumbnailService::~ThumbnailService() = default;

ThumbnailRequestResult ThumbnailService::request(
    const assets::MediaAsset& asset,
    ThumbnailRequestPriority priority) {
    return request_for_profile(
        asset, assets::DerivativeProfileId::Thumbnail320x180Srgb, priority);
}

ThumbnailRequestResult ThumbnailService::request_filmstrip(
    const assets::MediaAsset& asset,
    ThumbnailRequestPriority priority) {
    return request_for_profile(
        asset, assets::DerivativeProfileId::Filmstrip8x160x90Srgb, priority);
}

ThumbnailRequestResult ThumbnailService::request_for_profile(
    const assets::MediaAsset& asset,
    assets::DerivativeProfileId profile_id,
    ThumbnailRequestPriority priority) {
    try {
        if (!asset.id || !assets::valid_asset_source(asset.source)) {
            return ThumbnailRequestResult::failure(
                service_error(ThumbnailServiceErrorCode::InvalidAsset));
        }
        if (!asset.fingerprint.has_value()) {
            return ThumbnailRequestResult::failure(
                service_error(ThumbnailServiceErrorCode::MissingFingerprint));
        }
        const assets::DerivativeProfile* profile = assets::derivative_profile(
            profile_id);
        if (profile == nullptr ||
            (profile_id !=
                 assets::DerivativeProfileId::Thumbnail320x180Srgb &&
             profile_id !=
                 assets::DerivativeProfileId::Filmstrip8x160x90Srgb)) {
            return ThumbnailRequestResult::failure(
                service_error(ThumbnailServiceErrorCode::InvalidAsset));
        }
        const assets::DerivativeKey derivative_key =
            assets::make_derivative_key(asset.id, *asset.fingerprint, *profile);
        const std::string encoded_key =
            assets::derivative_key_string(derivative_key);

        {
            std::lock_guard lock{implementation_->mutex};
            const auto found = implementation_->jobs.find(encoded_key);
            if (found != implementation_->jobs.end() &&
                (found->second->enqueued || found->second->in_flight)) {
                if (priority == ThumbnailRequestPriority::Visible &&
                    found->second->priority ==
                        ThumbnailRequestPriority::Prefetch) {
                    found->second->priority = priority;
                    found->second->stamp = ++implementation_->clock;
                    ++found->second->revision;
                    // Leave the stale pointer in the prefetch deque and add
                    // the same job to the visible queue. Whichever deque sees
                    // it after the visible pop observes enqueued=false.
                    if (found->second->enqueued) {
                        --implementation_->prefetch_count;
                        implementation_->visible.push_back(found->second);
                    }
                }
                return ThumbnailRequestResult::success(
                    implementation_->snapshot_locked(*found->second));
            }
        }

        const auto cached = implementation_->cache.inspect(derivative_key);
        auto job = std::make_shared<Impl::Job>();
        job->key = derivative_key;
        job->asset = asset;
        job->priority = priority;
        ThumbnailSnapshot returned;
        {
            std::lock_guard lock{implementation_->mutex};
            const auto raced = implementation_->jobs.find(encoded_key);
            if (raced != implementation_->jobs.end() &&
                (raced->second->enqueued || raced->second->in_flight)) {
                return ThumbnailRequestResult::success(
                    implementation_->snapshot_locked(*raced->second));
            }
            job->stamp = ++implementation_->clock;
            if (!cached) {
                implementation_->set_state_locked(
                    *job, assets::DerivativeState::Failed,
                    ThumbnailFailureCode::CacheFailure);
                job->cache_error = cached.error().code;
            } else if (cached.value().state ==
                       assets::DerivativeState::Ready) {
                implementation_->set_state_locked(
                    *job, assets::DerivativeState::Ready);
            } else if (cached.value().state ==
                       assets::DerivativeState::Failed) {
                implementation_->set_state_locked(
                    *job, assets::DerivativeState::Failed,
                    ThumbnailFailureCode::CacheFailure);
            } else {
                const bool was_stale = cached.value().state ==
                    assets::DerivativeState::Stale;
                implementation_->set_state_locked(
                    *job, was_stale ? assets::DerivativeState::Stale
                                    : assets::DerivativeState::Queued,
                    was_stale ? ThumbnailFailureCode::CacheFailure
                              : ThumbnailFailureCode::None);
                returned = implementation_->snapshot_locked(*job);
                if (was_stale) {
                    implementation_->set_state_locked(
                        *job, assets::DerivativeState::Queued);
                }
                static_cast<void>(implementation_->enqueue_locked(job));
            }
            implementation_->jobs.insert_or_assign(encoded_key, job);
            implementation_->prune_locked();
            if (returned.state == assets::DerivativeState::Stale) {
                return ThumbnailRequestResult::success(std::move(returned));
            }
            returned = implementation_->snapshot_locked(*job);
        }
        return ThumbnailRequestResult::success(std::move(returned));
    } catch (const std::bad_alloc&) {
        return ThumbnailRequestResult::failure(
            service_error(ThumbnailServiceErrorCode::AllocationFailed));
    }
}

ThumbnailRequestResult ThumbnailService::request_model(
    const assets::ModelAsset& asset, ThumbnailRequestPriority priority) {
    try {
        if (!asset.id || !assets::valid_asset_source(asset.source)) {
            return ThumbnailRequestResult::failure(
                service_error(ThumbnailServiceErrorCode::InvalidAsset));
        }
        if (!asset.fingerprint.has_value()) {
            return ThumbnailRequestResult::failure(
                service_error(ThumbnailServiceErrorCode::MissingFingerprint));
        }
        const assets::DerivativeProfile* profile = assets::derivative_profile(
            assets::DerivativeProfileId::ModelTurntable320x180Srgb);
        if (profile == nullptr) {
            return ThumbnailRequestResult::failure(
                service_error(ThumbnailServiceErrorCode::InvalidAsset));
        }
        const assets::DerivativeKey derivative_key =
            assets::make_derivative_key(asset.id, *asset.fingerprint, *profile);
        const std::string encoded_key =
            assets::derivative_key_string(derivative_key);
        {
            std::lock_guard lock{implementation_->mutex};
            const auto found = implementation_->jobs.find(encoded_key);
            if (found != implementation_->jobs.end() &&
                (found->second->waiting || found->second->enqueued ||
                 found->second->in_flight)) {
                return ThumbnailRequestResult::success(
                    implementation_->snapshot_locked(*found->second));
            }
        }
        const auto cached = implementation_->cache.inspect(derivative_key);
        auto job = std::make_shared<Impl::Job>();
        job->key = derivative_key;
        job->model = asset;
        job->priority = priority;
        std::lock_guard lock{implementation_->mutex};
        const auto raced = implementation_->jobs.find(encoded_key);
        if (raced != implementation_->jobs.end() &&
            (raced->second->waiting || raced->second->enqueued ||
             raced->second->in_flight)) {
            return ThumbnailRequestResult::success(
                implementation_->snapshot_locked(*raced->second));
        }
        job->stamp = ++implementation_->clock;
        if (!cached) {
            implementation_->set_state_locked(
                *job, assets::DerivativeState::Failed,
                ThumbnailFailureCode::CacheFailure);
            job->cache_error = cached.error().code;
        } else if (cached.value().state == assets::DerivativeState::Ready) {
            implementation_->set_state_locked(*job,
                                              assets::DerivativeState::Ready);
        } else if (implementation_->waiting_count >=
                   implementation_->config.maximum_pending_requests) {
            implementation_->set_state_locked(
                *job, assets::DerivativeState::Cancelled,
                ThumbnailFailureCode::QueueCapacity);
        } else {
            implementation_->set_state_locked(*job,
                                              assets::DerivativeState::Queued);
            job->waiting = true;
            ++implementation_->waiting_count;
            (priority == ThumbnailRequestPriority::Visible
                 ? implementation_->visible_renders
                 : implementation_->prefetch_renders)
                .push_back(job);
        }
        implementation_->jobs.insert_or_assign(encoded_key, job);
        implementation_->prune_locked();
        return ThumbnailRequestResult::success(
            implementation_->snapshot_locked(*job));
    } catch (const std::bad_alloc&) {
        return ThumbnailRequestResult::failure(
            service_error(ThumbnailServiceErrorCode::AllocationFailed));
    }
}

std::vector<ModelThumbnailRender>
ThumbnailService::take_model_renders(core::u32 limit) {
    std::vector<ModelThumbnailRender> taken;
    try {
        std::lock_guard lock{implementation_->mutex};
        for (auto* queue : {&implementation_->visible_renders,
                            &implementation_->prefetch_renders}) {
            while (taken.size() < limit && !queue->empty()) {
                std::shared_ptr<Impl::Job> job = std::move(queue->front());
                queue->pop_front();
                if (!job->waiting || job->cancellation.stop_requested()) {
                    continue;
                }
                job->waiting = false;
                --implementation_->waiting_count;
                job->in_flight = true;
                implementation_->set_state_locked(
                    *job, assets::DerivativeState::Generating);
                taken.push_back(ModelThumbnailRender{job->key, job->model->source});
            }
        }
    } catch (const std::bad_alloc&) {
    }
    return taken;
}

void ThumbnailService::complete_model_render(const assets::DerivativeKey& key,
                                             std::vector<core::u8> rgba) {
    try {
        const std::string encoded = assets::derivative_key_string(key);
        std::lock_guard lock{implementation_->mutex};
        const auto found = implementation_->jobs.find(encoded);
        if (found == implementation_->jobs.end() || found->second->key != key) {
            return;
        }
        const std::shared_ptr<Impl::Job>& job = found->second;
        if (!job->model.has_value() || !job->in_flight || job->enqueued) {
            return;
        }
        job->in_flight = false;
        if (job->cancellation.stop_requested() ||
            job->state != assets::DerivativeState::Generating) {
            if (job->state != assets::DerivativeState::Cancelled) {
                implementation_->set_state_locked(
                    *job, assets::DerivativeState::Cancelled);
            }
            return;
        }
        job->rendered = std::move(rgba);
        job->priority = ThumbnailRequestPriority::Visible;
        static_cast<void>(implementation_->enqueue_locked(job));
    } catch (...) {
    }
}

std::optional<ThumbnailSnapshot> ThumbnailService::snapshot(
    const assets::DerivativeKey& key) {
    try {
        const std::string encoded = assets::derivative_key_string(key);
        std::lock_guard lock{implementation_->mutex};
        const auto found = implementation_->jobs.find(encoded);
        if (found == implementation_->jobs.end() || found->second->key != key) {
            return std::nullopt;
        }
        found->second->stamp = ++implementation_->clock;
        return implementation_->snapshot_locked(*found->second);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<ThumbnailSnapshot> ThumbnailService::cancel(
    const assets::DerivativeKey& key) {
    try {
        const std::string encoded = assets::derivative_key_string(key);
        std::lock_guard lock{implementation_->mutex};
        const auto found = implementation_->jobs.find(encoded);
        if (found == implementation_->jobs.end() || found->second->key != key) {
            return std::nullopt;
        }
        implementation_->cancel_locked(*found->second);
        return implementation_->snapshot_locked(*found->second);
    } catch (...) {
        return std::nullopt;
    }
}

core::Result<assets::DerivativeCacheLoad, assets::DerivativeCacheError>
ThumbnailService::load(const assets::DerivativeKey& key) noexcept {
    return implementation_->cache.load(
        key, maximum_thumbnail_payload_bytes);
}

void ThumbnailService::retain(
    std::span<const assets::DerivativeKey> needed) {
    std::unordered_set<std::string> retained;
    retained.reserve(needed.size());
    for (const assets::DerivativeKey& key : needed) {
        retained.insert(assets::derivative_key_string(key));
    }
    std::lock_guard lock{implementation_->mutex};
    for (auto& pair : implementation_->jobs) {
        Impl::Job& job = *pair.second;
        if ((job.enqueued || job.in_flight || job.waiting) &&
            retained.find(pair.first) == retained.end()) {
            implementation_->cancel_locked(job);
        }
    }
}

ThumbnailServiceStats ThumbnailService::stats() const noexcept {
    ThumbnailServiceStats result{};
    std::lock_guard lock{implementation_->mutex};
    for (const auto& pair : implementation_->jobs) {
        switch (pair.second->state) {
        case assets::DerivativeState::Queued:
            ++result.queued;
            break;
        case assets::DerivativeState::Generating:
            ++result.generating;
            break;
        case assets::DerivativeState::Ready:
            ++result.ready;
            break;
        case assets::DerivativeState::Stale:
            ++result.stale;
            break;
        case assets::DerivativeState::Failed:
            ++result.failed;
            break;
        case assets::DerivativeState::Cancelled:
            ++result.cancelled;
            break;
        case assets::DerivativeState::Missing:
            break;
        }
    }
    result.workers = implementation_->config.worker_count;
    result.pending_limit = implementation_->config.maximum_pending_requests;
    result.prefetch_limit = implementation_->config.maximum_prefetch_requests;
    return result;
}

} // namespace mediaserver::server
