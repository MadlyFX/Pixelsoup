#include "proxy_generation_service.hpp"
#include "proxy_generation_worker.hpp"

#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/assets/proxy_policy.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace app = mediaserver::app;
namespace assets = mediaserver::assets;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace fs = std::filesystem;
namespace server = mediaserver::server;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} +
                                 std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define PROXY_CHECK(...)                                                      \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<core::u64> sequence{};
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = fs::temp_directory_path() /
            ("mediaserver_proxy_service_" + std::to_string(stamp) + "_" +
             std::to_string(sequence.fetch_add(1U)));
        std::error_code filesystem_error;
        if (!fs::create_directory(path_, filesystem_error) ||
            filesystem_error) {
            throw std::runtime_error{"could not create temporary directory"};
        }
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        static_cast<void>(fs::remove_all(path_, ignored));
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    [[nodiscard]] const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

[[nodiscard]] std::vector<core::u8> bytes(std::string_view value) {
    return std::vector<core::u8>{value.begin(), value.end()};
}

void write_file(const fs::path& path, std::span<const core::u8> content) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(content.data()),
                 static_cast<std::streamsize>(content.size()));
    output.close();
    if (!output) {
        throw std::runtime_error{"could not write fixture"};
    }
}

[[nodiscard]] assets::ContentFingerprint fingerprint(const fs::path& path) {
    const auto result = assets::source_fingerprint(path.string());
    if (!result) {
        throw std::runtime_error{"could not fingerprint fixture"};
    }
    return result.value();
}

[[nodiscard]] assets::DerivativeCache open_cache(const fs::path& root) {
    auto opened = assets::DerivativeCache::open(
        {root, 16U * 1'024U * 1'024U});
    if (!opened) {
        throw std::runtime_error{"could not open derivative cache"};
    }
    return std::move(opened).value();
}

[[nodiscard]] assets::MediaAsset asset(const fs::path& source,
                                       core::u64 id = 41U) {
    return assets::MediaAsset{
        doc::NodeId{id},
        "Proxy source",
        source.string(),
        assets::PixelDimensions{1'920U, 1'080U},
        3U,
        assets::MediaFrameRate{30'000U, 1'001U},
        fingerprint(source),
        assets::DerivativeProfileId::Proxy720pCfrH264,
        assets::ProxyPlaybackPolicy::PreferProxy,
    };
}

// What a backend reports for a source of `source_frames` frames: the whole
// file, or the run of it a segment covers.
[[nodiscard]] server::ProxyBackendOutput backend_output(
    const assets::ContentFingerprint& consumed_fingerprint,
    core::u64 source_frames = 3U,
    std::optional<server::ProxySourceRange> range = std::nullopt) {
    const assets::MediaFrameRate rate{30'000U, 1'001U};
    const server::ProxySourceRange covered =
        range.value_or(server::ProxySourceRange{0U, source_frames});
    auto map = assets::make_identity_proxy_frame_map(covered.count, rate,
                                                     covered.first);
    PROXY_CHECK(map.has_value());
    return server::ProxyBackendOutput{
        consumed_fingerprint,
        server::ProxySourceMetadata{
            assets::PixelDimensions{1'920U, 1'080U},
            source_frames,
            rate,
            1U,
            1U,
            1U,
            assets::DerivativeColorPrimaries::Rec709,
            assets::DerivativeTransfer::Rec709,
            assets::DerivativeMatrix::Rec709,
            assets::DerivativeColorRange::Limited,
            true,
        },
        server::ProxyEncodedMetadata{
            assets::PixelDimensions{1'280U, 720U},
            covered.count,
            rate,
            1U,
            1U,
            1U,
            assets::DerivativeColorPrimaries::Rec709,
            assets::DerivativeTransfer::Rec709,
            assets::DerivativeMatrix::Rec709,
            assets::DerivativeColorRange::Limited,
            assets::DerivativeContainer::Mp4,
            assets::DerivativeCodec::H264,
            assets::DerivativePixelFormat::Yuv420p,
            assets::DerivativeAudioLayout::None,
            assets::DerivativeAudioCodec::None,
            0U,
            0U,
        },
        std::move(map).value(),
    };
}

struct BackendState final {
    std::atomic<core::u32> calls{};
    std::vector<core::u8> payload{bytes("opaque-test-backend-output")};
    std::optional<server::ProxyBackendError> failure;
    std::optional<server::ProxyBackendOutput> output;
    fs::path mutate_source;
    std::vector<core::u8> replacement_source;
    bool omit_output_file{};
    bool wait_for_cancellation{};
    std::mutex mutex;
    std::condition_variable condition;
    bool entered{};
    doc::NodeId requested_asset{};
    std::optional<assets::DerivativeKey> requested_key;
    fs::path requested_output;
    std::optional<bool> requested_yield;
    std::optional<server::ProxySourceRange> requested_range;
    // The bytes of each file a join request named, in order.
    std::vector<std::vector<core::u8>> joined;
    core::u64 source_frames{3U};
    // Every request's range in order (empty for a whole file or a join),
    // how many joins were asked for, and each piece handed over.
    std::vector<std::optional<server::ProxySourceRange>> ranges;
    core::u32 joins{};
    std::vector<server::ProxySourceRange> pieces;
    // Runs before each piece is handed over.
    std::function<void(core::u32 piece)> on_piece;
};

class ControlledBackend final : public server::ProxyEncoderBackend {
public:
    explicit ControlledBackend(std::shared_ptr<BackendState> state)
        : state_(std::move(state)) {}

    server::ProxyBackendResult encode(
        const server::ProxyEncodeRequest& request,
        std::stop_token stop) override {
        ++state_->calls;
        {
            std::scoped_lock lock{state_->mutex};
            state_->entered = true;
            state_->requested_asset = request.asset;
            state_->requested_key = request.key;
            state_->requested_output = request.output_file;
            state_->requested_yield = request.yield_to_playback;
            state_->requested_range = request.range;
            state_->ranges.push_back(request.range);
            if (!request.join_segments.empty()) {
                ++state_->joins;
            }
            state_->joined.clear();
            for (const fs::path& segment : request.join_segments) {
                std::ifstream input{segment, std::ios::binary};
                state_->joined.emplace_back(
                    std::istreambuf_iterator<char>{input},
                    std::istreambuf_iterator<char>{});
            }
        }
        state_->condition.notify_all();

        if (request.progress) {
            request.progress(1U, 3U);
            request.progress(3U, 3U);
        }

        // A run hands over one piece per segment, each named like a
        // segment encoded alone, and stops when the caller refuses one.
        if (request.piece_frames != 0U && request.range.has_value()) {
            const core::u64 end = request.range->first + request.range->count;
            std::optional<server::ProxyBackendOutput> last;
            for (core::u32 piece = 0U;; ++piece) {
                const core::u64 first =
                    request.range->first +
                    static_cast<core::u64>(piece) * request.piece_frames;
                if (first >= end) {
                    break;
                }
                const server::ProxySourceRange covered{
                    first, std::min(request.piece_frames, end - first)};
                write_file(server::proxy_piece_path(request.output_file,
                                                    piece),
                           bytes("segment-" + std::to_string(first)));
                {
                    std::scoped_lock lock{state_->mutex};
                    state_->pieces.push_back(covered);
                }
                if (state_->on_piece) {
                    state_->on_piece(piece);
                }
                last = backend_output(request.source_fingerprint,
                                      state_->source_frames, covered);
                if (!request.piece_done(piece, *last) &&
                    covered.first + covered.count < end) {
                    return server::ProxyBackendResult::failure(
                        server::ProxyBackendError{
                            server::ProxyBackendErrorCode::Cancelled, {}});
                }
            }
            return server::ProxyBackendResult::success(std::move(*last));
        }
        if (!state_->omit_output_file) {
            // A segment's payload names its first frame, so a join can be
            // checked for order.
            write_file(request.output_file,
                       request.range.has_value()
                           ? bytes("segment-" +
                                   std::to_string(request.range->first))
                           : state_->payload);
        }
        if (state_->wait_for_cancellation) {
            std::stop_callback wake{stop, [this] {
                state_->condition.notify_all();
            }};
            std::unique_lock lock{state_->mutex};
            state_->condition.wait(lock,
                                   [&] { return stop.stop_requested(); });
            return server::ProxyBackendResult::failure(
                server::ProxyBackendError{
                    server::ProxyBackendErrorCode::Cancelled, {}});
        }
        if (state_->failure.has_value()) {
            return server::ProxyBackendResult::failure(*state_->failure);
        }
        if (!state_->mutate_source.empty()) {
            write_file(state_->mutate_source, state_->replacement_source);
        }
        server::ProxyBackendOutput output = state_->output.has_value()
            ? *state_->output
            : backend_output(request.source_fingerprint,
                             state_->source_frames, request.range);
        return server::ProxyBackendResult::success(std::move(output));
    }

private:
    std::shared_ptr<BackendState> state_;
};

[[nodiscard]] bool work_files_exist(const fs::path& cache_root) {
    const fs::path work = cache_root / ".proxy-work";
    std::error_code filesystem_error;
    if (!fs::exists(work, filesystem_error) || filesystem_error) {
        return false;
    }
    fs::recursive_directory_iterator iterator{
        work, fs::directory_options::skip_permission_denied,
        filesystem_error};
    const fs::recursive_directory_iterator end;
    while (!filesystem_error && iterator != end) {
        if (iterator->is_regular_file(filesystem_error) &&
            !filesystem_error) {
            return true;
        }
        filesystem_error.clear();
        iterator.increment(filesystem_error);
    }
    return false;
}

void unavailable_default_fails_closed_without_publishing() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "source.mov";
    write_file(source, bytes("source-revision"));
    const assets::MediaAsset media = asset(source);
    const assets::MediaAsset unchanged = media;
    server::ProxyGenerationService service{
        open_cache(temporary.path() / "cache")};

    const auto result = service.generate(media);
    PROXY_CHECK(!result);
    PROXY_CHECK(result.error().code ==
                server::ProxyGenerationErrorCode::BackendUnavailable);
    PROXY_CHECK(result.error().backend_error.has_value());
    PROXY_CHECK(result.error().backend_error->code ==
                server::ProxyBackendErrorCode::Unavailable);
    PROXY_CHECK(media == unchanged);
    const auto key = assets::requested_proxy_key(media);
    PROXY_CHECK(key.has_value());
    const auto cached = service.inspect(*key);
    PROXY_CHECK(cached.has_value());
    PROXY_CHECK(cached.value().state == assets::DerivativeState::Missing);
    PROXY_CHECK(!work_files_exist(service.cache_root()));
}

void successful_backend_publishes_keyed_payload_and_sidecar() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "fractional.mov";
    write_file(source, bytes("fractional-rate-source"));
    const assets::MediaAsset media = asset(source);
    const assets::MediaAsset unchanged = media;
    const auto expected_key = assets::requested_proxy_key(media);
    PROXY_CHECK(expected_key.has_value());
    const fs::path cache_root = temporary.path() / "cache";
    const auto state = std::make_shared<BackendState>();
    const std::vector<core::u8> expected_payload = state->payload;
    {
        server::ProxyGenerationService service{
            open_cache(cache_root),
            std::make_unique<ControlledBackend>(state),
            {1U * 1'024U * 1'024U, 100U},
        };
        const auto generated = service.generate(media);
        PROXY_CHECK(generated.has_value());
        PROXY_CHECK(!generated.value().cache_hit);
        PROXY_CHECK(state->requested_yield == false);
        PROXY_CHECK(generated.value().asset == media.id);
        PROXY_CHECK(generated.value().key == *expected_key);
        PROXY_CHECK(generated.value().key.asset == media.id);
        PROXY_CHECK(generated.value().encoded_metadata.frame_rate ==
                    assets::MediaFrameRate{30'000U, 1'001U});
        PROXY_CHECK(generated.value().encoded_metadata.clockwise_quarter_turns ==
                    1U);
        PROXY_CHECK(generated.value().frame_map.proxy_frame_rate ==
                    assets::MediaFrameRate{30'000U, 1'001U});
        PROXY_CHECK(generated.value().frame_map.source_time_by_proxy_frame.size() ==
                    3U);
        PROXY_CHECK(generated.value().cache_entry.state ==
                    assets::DerivativeState::Ready);
        PROXY_CHECK(media == unchanged);
        PROXY_CHECK(state->calls.load() == 1U);
        PROXY_CHECK(state->requested_asset == media.id);
        PROXY_CHECK(state->requested_key == expected_key);
        PROXY_CHECK(state->requested_output.filename().string().find(
                        "fractional.mov") == std::string::npos);

        const auto loaded = service.load(*expected_key);
        PROXY_CHECK(loaded.has_value());
        PROXY_CHECK(loaded.value().receipt.key == *expected_key);
        PROXY_CHECK(loaded.value().receipt.asset == media.id);
        PROXY_CHECK(loaded.value().receipt.frame_map ==
                    generated.value().frame_map);
        PROXY_CHECK(loaded.value().encoded_payload == expected_payload);
        PROXY_CHECK(!work_files_exist(service.cache_root()));
    }

    std::error_code remove_error;
    PROXY_CHECK(fs::remove(source, remove_error));
    PROXY_CHECK(!remove_error);
    server::ProxyGenerationService restarted{open_cache(cache_root)};
    const auto cached = restarted.generate(media);
    PROXY_CHECK(cached.has_value());
    PROXY_CHECK(cached.value().cache_hit);
    PROXY_CHECK(cached.value().asset == media.id);
    PROXY_CHECK(cached.value().key == *expected_key);
}

void source_revision_must_match_before_during_and_after_encode() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "revision.mov";
    write_file(source, bytes("expected-source"));
    assets::MediaAsset mismatched = asset(source, 51U);
    const auto wrong_bytes = bytes("different-revision");
    mismatched.fingerprint = assets::ContentFingerprint{
        static_cast<core::u64>(wrong_bytes.size()),
        assets::sha256_digest(wrong_bytes),
    };
    auto state = std::make_shared<BackendState>();
    server::ProxyGenerationService service{
        open_cache(temporary.path() / "cache-a"),
        std::make_unique<ControlledBackend>(state)};
    const auto before = service.generate(mismatched);
    PROXY_CHECK(!before);
    PROXY_CHECK(before.error().code ==
                server::ProxyGenerationErrorCode::FingerprintMismatch);
    PROXY_CHECK(state->calls.load() == 0U);

    const fs::path source_changed = temporary.path() / "changed.mov";
    write_file(source_changed, bytes("initial-source"));
    const assets::MediaAsset changing = asset(source_changed, 52U);
    state = std::make_shared<BackendState>();
    state->mutate_source = source_changed;
    state->replacement_source = bytes("changed-during-encode");
    server::ProxyGenerationService changing_service{
        open_cache(temporary.path() / "cache-b"),
        std::make_unique<ControlledBackend>(state)};
    const auto during = changing_service.generate(changing);
    PROXY_CHECK(!during);
    PROXY_CHECK(during.error().code ==
                server::ProxyGenerationErrorCode::FingerprintMismatch);

    const fs::path consumed = temporary.path() / "consumed.mov";
    write_file(consumed, bytes("consumed-source"));
    const assets::MediaAsset consumed_asset = asset(consumed, 53U);
    state = std::make_shared<BackendState>();
    state->output = backend_output(consumed_asset.fingerprint.value());
    state->output->encoded_source_fingerprint.sha256.bytes[0U] ^= 0xffU;
    server::ProxyGenerationService consumed_service{
        open_cache(temporary.path() / "cache-c"),
        std::make_unique<ControlledBackend>(state)};
    const auto consumed_result = consumed_service.generate(consumed_asset);
    PROXY_CHECK(!consumed_result);
    PROXY_CHECK(consumed_result.error().code ==
                server::ProxyGenerationErrorCode::FingerprintMismatch);
}

void metadata_and_frame_map_validation_rejects_semantic_drift() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "metadata.mov";
    write_file(source, bytes("metadata-source"));
    const assets::MediaAsset media = asset(source, 61U);
    const auto state = std::make_shared<BackendState>();
    state->output = backend_output(media.fingerprint.value());
    server::ProxyGenerationService service{
        open_cache(temporary.path() / "cache"),
        std::make_unique<ControlledBackend>(state)};

    state->output->encoded_metadata.frame_rate =
        assets::MediaFrameRate{24U, 1U};
    auto result = service.generate(media);
    PROXY_CHECK(!result);
    PROXY_CHECK(result.error().code ==
                server::ProxyGenerationErrorCode::InvalidMetadata);

    state->output = backend_output(media.fingerprint.value());
    state->output->encoded_metadata.clockwise_quarter_turns = 0U;
    result = service.generate(media);
    PROXY_CHECK(!result);
    PROXY_CHECK(result.error().code ==
                server::ProxyGenerationErrorCode::InvalidMetadata);

    state->output = backend_output(media.fingerprint.value());
    state->output->encoded_metadata.video_codec =
        assets::DerivativeCodec::Png;
    result = service.generate(media);
    PROXY_CHECK(!result);
    PROXY_CHECK(result.error().code ==
                server::ProxyGenerationErrorCode::InvalidMetadata);

    state->output = backend_output(media.fingerprint.value());
    state->output->frame_map.proxy_frame_rate =
        assets::MediaFrameRate{24U, 1U};
    result = service.generate(media);
    PROXY_CHECK(!result);
    PROXY_CHECK(result.error().code ==
                server::ProxyGenerationErrorCode::InvalidMetadata);

    // The source has audio, but the editor proxy profile carries none.
    state->output = backend_output(media.fingerprint.value());
    PROXY_CHECK(state->output->source_metadata.has_audio);
    state->output->encoded_metadata.audio_layout =
        assets::DerivativeAudioLayout::Stereo;
    state->output->encoded_metadata.audio_codec =
        assets::DerivativeAudioCodec::Aac;
    state->output->encoded_metadata.audio_sample_rate = 48'000U;
    state->output->encoded_metadata.audio_bitrate = 192'000U;
    result = service.generate(media);
    PROXY_CHECK(!result);
    PROXY_CHECK(result.error().code ==
                server::ProxyGenerationErrorCode::InvalidMetadata);

    state->output = backend_output(media.fingerprint.value());
    state->output->source_metadata.dimensions.width = 1'919U;
    result = service.generate(media);
    PROXY_CHECK(!result);
    PROXY_CHECK(result.error().code ==
                server::ProxyGenerationErrorCode::InvalidMetadata);
    const auto key = assets::requested_proxy_key(media);
    PROXY_CHECK(key.has_value());
    PROXY_CHECK(service.inspect(*key).value().state ==
                assets::DerivativeState::Missing);
}

void missing_and_oversized_backend_outputs_never_publish() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "output.mov";
    write_file(source, bytes("output-source"));
    const assets::MediaAsset media = asset(source, 71U);
    auto state = std::make_shared<BackendState>();
    state->omit_output_file = true;
    server::ProxyGenerationService missing_service{
        open_cache(temporary.path() / "cache-a"),
        std::make_unique<ControlledBackend>(state)};
    const auto missing = missing_service.generate(media);
    PROXY_CHECK(!missing);
    PROXY_CHECK(missing.error().code ==
                server::ProxyGenerationErrorCode::OutputMissing);

    state = std::make_shared<BackendState>();
    state->payload = bytes("larger-than-four");
    server::ProxyGenerationService oversized_service{
        open_cache(temporary.path() / "cache-b"),
        std::make_unique<ControlledBackend>(state),
        {4U, 100U}};
    const auto oversized = oversized_service.generate(media);
    PROXY_CHECK(!oversized);
    PROXY_CHECK(oversized.error().code ==
                server::ProxyGenerationErrorCode::OutputTooLarge);
    PROXY_CHECK(!work_files_exist(oversized_service.cache_root()));
}

void cancellation_stops_backend_and_cleans_partial_work() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "cancel.mov";
    write_file(source, bytes("cancel-source"));
    const assets::MediaAsset media = asset(source, 81U);
    auto state = std::make_shared<BackendState>();
    server::ProxyGenerationService pre_cancelled{
        open_cache(temporary.path() / "cache-a"),
        std::make_unique<ControlledBackend>(state)};
    std::stop_source already_stopped;
    already_stopped.request_stop();
    const auto before = pre_cancelled.generate(
        media, already_stopped.get_token());
    PROXY_CHECK(!before);
    PROXY_CHECK(before.error().code ==
                server::ProxyGenerationErrorCode::Cancelled);
    PROXY_CHECK(state->calls.load() == 0U);

    state = std::make_shared<BackendState>();
    state->wait_for_cancellation = true;
    server::ProxyGenerationService service{
        open_cache(temporary.path() / "cache-b"),
        std::make_unique<ControlledBackend>(state)};
    std::stop_source cancellation;
    std::optional<server::ProxyGenerationResult> result;
    std::jthread worker{[&] {
        result.emplace(service.generate(media, cancellation.get_token()));
    }};
    {
        std::unique_lock lock{state->mutex};
        const bool entered = state->condition.wait_for(
            lock, std::chrono::seconds{5}, [&] { return state->entered; });
        PROXY_CHECK(entered);
    }
    cancellation.request_stop();
    worker.join();
    PROXY_CHECK(result.has_value());
    PROXY_CHECK(!result->has_value());
    PROXY_CHECK(result->error().code ==
                server::ProxyGenerationErrorCode::Cancelled);
    PROXY_CHECK(!work_files_exist(service.cache_root()));
    const auto key = assets::requested_proxy_key(media);
    PROXY_CHECK(key.has_value());
    PROXY_CHECK(service.inspect(*key).value().state ==
                assets::DerivativeState::Missing);
}

void cache_envelopes_with_invalid_sidecars_are_not_cache_hits() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "invalid-artifact.mov";
    write_file(source, bytes("invalid-artifact-source"));
    const assets::MediaAsset media = asset(source, 91U);
    const auto key = assets::requested_proxy_key(media);
    PROXY_CHECK(key.has_value());
    auto cache = open_cache(temporary.path() / "cache");
    PROXY_CHECK(cache.publish(*key, bytes("not-a-proxy-artifact")));
    server::ProxyGenerationService service{std::move(cache)};

    const auto loaded = service.load(*key);
    PROXY_CHECK(!loaded);
    PROXY_CHECK(loaded.error().code ==
                server::ProxyGenerationErrorCode::ArtifactInvalid);
    const auto generated = service.generate(media);
    PROXY_CHECK(!generated);
    PROXY_CHECK(generated.error().code ==
                server::ProxyGenerationErrorCode::BackendUnavailable);
}

void backend_frame_progress_reaches_generation_callers() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "progress.mov";
    write_file(source, bytes("progress-source"));
    const assets::MediaAsset media = asset(source, 101U);
    const auto state = std::make_shared<BackendState>();
    server::ProxyGenerationService service{
        open_cache(temporary.path() / "cache"),
        std::make_unique<ControlledBackend>(state)};
    std::vector<std::pair<core::u64, core::u64>> progress;

    const auto generated = service.generate(
        media, {}, [&](core::u64 completed, core::u64 total) {
            progress.emplace_back(completed, total);
        });

    PROXY_CHECK(generated.has_value());
    PROXY_CHECK(progress ==
                std::vector<std::pair<core::u64, core::u64>>{
                    {1U, 3U}, {3U, 3U}});
}

void a_job_started_during_playback_asks_the_backend_to_yield() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "playing.mov";
    write_file(source, bytes("playing-source"));
    const assets::MediaAsset media = asset(source, 103U);
    const auto state = std::make_shared<BackendState>();
    server::ProxyGenerationService service{
        open_cache(temporary.path() / "cache"),
        std::make_unique<ControlledBackend>(state)};

    const auto generated = service.generate(
        media, {}, {}, server::ProxyGenerationService::Step{true});

    PROXY_CHECK(generated.has_value());
    PROXY_CHECK(state->calls == 1U);
    PROXY_CHECK(state->requested_yield == true);
}

// A proxy made in segments: each segment encodes only its frames and is
// published under its own key, and once all are published they join into the
// whole-file proxy, in source order, without another encode.
void segments_publish_apart_and_join_into_the_whole_proxy() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "long.mov";
    write_file(source, bytes("long-source"));
    assets::MediaAsset media = asset(source, 107U);
    // 450 frames at 29.97: segments of 300 and 150 frames.
    media.frame_count = 450U;
    const auto state = std::make_shared<BackendState>();
    state->source_frames = 450U;
    server::ProxyGenerationService service{
        open_cache(temporary.path() / "cache"),
        std::make_unique<ControlledBackend>(state)};
    using Step = server::ProxyGenerationService::Step;

    auto second = service.generate(media, {}, {}, Step{false, 1U});
    PROXY_CHECK(second.has_value());
    PROXY_CHECK((state->requested_range ==
                 server::ProxySourceRange{300U, 150U}));
    PROXY_CHECK(second.value().key.segment == 1U);
    PROXY_CHECK(second.value().frame_map.source_time_by_proxy_frame.size() ==
                150U);
    const auto whole_key = assets::requested_proxy_key(media);
    PROXY_CHECK(whole_key.has_value());
    PROXY_CHECK(second.value().key ==
                assets::derivative_segment_key(*whole_key, 1U));

    // Not every segment is published yet.
    auto early = service.generate(media, {}, {}, Step{false, {}, true});
    PROXY_CHECK(!early);
    PROXY_CHECK(early.error().code ==
                server::ProxyGenerationErrorCode::CacheNotReady);

    // A segment whose frames start one frame late is refused.
    state->output = backend_output(media.fingerprint.value(), 450U,
                                   server::ProxySourceRange{1U, 300U});
    auto shifted = service.generate(media, {}, {}, Step{false, 0U});
    PROXY_CHECK(!shifted);
    PROXY_CHECK(shifted.error().code ==
                server::ProxyGenerationErrorCode::InvalidMetadata);
    state->output.reset();

    auto first = service.generate(media, {}, {}, Step{false, 0U});
    PROXY_CHECK(first.has_value());
    PROXY_CHECK((state->requested_range ==
                 server::ProxySourceRange{0U, 300U}));

    const core::u32 encodes = state->calls;
    auto joined = service.generate(media, {}, {}, Step{false, {}, true});
    PROXY_CHECK(joined.has_value());
    PROXY_CHECK(state->calls == encodes + 1U);
    PROXY_CHECK(!state->requested_range.has_value());
    PROXY_CHECK((state->joined ==
                 std::vector<std::vector<core::u8>>{bytes("segment-0"),
                                                    bytes("segment-300")}));
    PROXY_CHECK(joined.value().key == *whole_key);
    PROXY_CHECK(joined.value().frame_map.source_time_by_proxy_frame.size() ==
                450U);
    PROXY_CHECK(service.inspect(*whole_key).value().state ==
                assets::DerivativeState::Ready);
    // The whole proxy is now a cache hit; nothing is joined again.
    auto again = service.generate(media);
    PROXY_CHECK(again.has_value() && again.value().cache_hit);
    PROXY_CHECK(state->calls == encodes + 1U);
}

// A run makes several segments in one encode and publishes each as it is
// handed over; a run stopped part way keeps what it published.
void a_run_publishes_each_segment_from_one_encode() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "run.mov";
    write_file(source, bytes("run-source"));
    assets::MediaAsset media = asset(source, 109U);
    // 750 frames at 29.97: segments of 300, 300 and 150 frames.
    media.frame_count = 750U;
    const auto state = std::make_shared<BackendState>();
    state->source_frames = 750U;
    const fs::path cache_root = temporary.path() / "cache";
    server::ProxyGenerationService service{
        open_cache(cache_root), std::make_unique<ControlledBackend>(state)};
    using Step = server::ProxyGenerationService::Step;
    const auto whole_key = assets::requested_proxy_key(media);
    PROXY_CHECK(whole_key.has_value());
    const auto ready = [&](core::u32 segment) {
        const auto entry = service.inspect(
            assets::derivative_segment_key(*whole_key, segment));
        return entry.has_value() &&
               entry.value().state == assets::DerivativeState::Ready;
    };

    // A run past the last segment is refused before anything is encoded.
    auto past = service.generate(media, {}, {}, Step{false, 1U, false, 3U});
    PROXY_CHECK(!past);
    PROXY_CHECK(past.error().code ==
                server::ProxyGenerationErrorCode::InvalidProfile);
    PROXY_CHECK(state->calls == 0U);

    // Stopped after its first segment: that one is published, the run
    // reports Cancelled, and no piece is left in the work folder.
    std::vector<core::u32> heard;
    auto stopped = service.generate(
        media, {}, {},
        Step{false, 0U, false, 3U,
             [&](core::u32 segment, const server::ProxyGenerationReceipt&) {
                 heard.push_back(segment);
                 return false;
             }});
    PROXY_CHECK(!stopped);
    PROXY_CHECK(stopped.error().code ==
                server::ProxyGenerationErrorCode::Cancelled);
    PROXY_CHECK((heard == std::vector<core::u32>{0U}));
    PROXY_CHECK(ready(0U));
    PROXY_CHECK(!ready(1U));
    PROXY_CHECK(!work_files_exist(cache_root));

    // Run again from the start: segment 0, already made, is reported as
    // made, and one encode covers the rest, cut at 300 frames.
    heard.clear();
    const core::u32 before = state->calls;
    auto run = service.generate(
        media, {}, {},
        Step{false, 0U, false, 3U,
             [&](core::u32 segment,
                 const server::ProxyGenerationReceipt& receipt) {
                 heard.push_back(segment);
                 PROXY_CHECK(receipt.key ==
                             assets::derivative_segment_key(*whole_key,
                                                            segment));
                 return true;
             }});
    PROXY_CHECK(run.has_value());
    PROXY_CHECK(state->calls == before + 1U);
    PROXY_CHECK((state->requested_range ==
                 server::ProxySourceRange{300U, 450U}));
    PROXY_CHECK((heard == std::vector<core::u32>{0U, 1U, 2U}));
    PROXY_CHECK((state->pieces.back() ==
                 server::ProxySourceRange{600U, 150U}));
    PROXY_CHECK(run.value().key ==
                assets::derivative_segment_key(*whole_key, 2U));
    PROXY_CHECK(run.value().frame_map.source_time_by_proxy_frame.size() ==
                150U);
    PROXY_CHECK(ready(1U) && ready(2U));
    PROXY_CHECK(!work_files_exist(cache_root));

    // The run's segments join as segments made alone do.
    auto joined = service.generate(media, {}, {}, Step{false, {}, true});
    PROXY_CHECK(joined.has_value());
    PROXY_CHECK((state->joined ==
                 std::vector<std::vector<core::u8>>{bytes("segment-0"),
                                                    bytes("segment-300"),
                                                    bytes("segment-600")}));
}

// Waits for the worker to finish every job it was given.
[[nodiscard]] bool worker_finished(const app::ProxyGenerationWorker& worker,
                                   const assets::DerivativeKey& key) {
    for (int attempt = 0; attempt < 1'000; ++attempt) {
        if (!worker.progress_for(key).has_value()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return false;
}

// The worker makes the rest of the file from the segment a view needs in
// one run, then the start of the file in another, then joins them.
void the_worker_runs_from_the_needed_segment_then_from_the_start() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "worker.mov";
    write_file(source, bytes("worker-source"));
    assets::MediaAsset media = asset(source, 113U);
    // Five segments of 300 frames.
    media.frame_count = 1'500U;
    const auto state = std::make_shared<BackendState>();
    state->source_frames = 1'500U;
    const auto key = assets::requested_proxy_key(media);
    PROXY_CHECK(key.has_value());
    {
        app::ProxyGenerationWorker worker{
            std::make_unique<server::ProxyGenerationService>(
                open_cache(temporary.path() / "cache"),
                std::make_unique<ControlledBackend>(state))};
        const std::array<app::ProxyGenerationWorker::Need, 1U> needs{
            {{media.id, 960U}}};
        worker.set_needs(needs);
        worker.request(media);
        PROXY_CHECK(worker_finished(worker, *key));
    }
    using Range = std::optional<server::ProxySourceRange>;
    PROXY_CHECK((state->ranges ==
                 std::vector<Range>{server::ProxySourceRange{900U, 600U},
                                    server::ProxySourceRange{0U, 900U},
                                    std::nullopt}));
    PROXY_CHECK(state->joins == 1U);
    PROXY_CHECK(state->pieces.size() == 5U);
}

// Between segments the worker looks again: a view that now needs a segment
// outside the run ends it, and what it made is kept rather than remade.
void the_worker_leaves_a_run_for_a_newly_needed_segment() {
    TemporaryDirectory temporary;
    const fs::path source = temporary.path() / "jump.mov";
    write_file(source, bytes("jump-source"));
    assets::MediaAsset media = asset(source, 117U);
    media.frame_count = 1'500U;
    const auto state = std::make_shared<BackendState>();
    state->source_frames = 1'500U;
    const auto key = assets::requested_proxy_key(media);
    PROXY_CHECK(key.has_value());
    {
        app::ProxyGenerationWorker worker{
            std::make_unique<server::ProxyGenerationService>(
                open_cache(temporary.path() / "cache"),
                std::make_unique<ControlledBackend>(state))};
        // The playhead jumps to segment 3 as the first run hands over its
        // first segment.
        const std::array<app::ProxyGenerationWorker::Need, 1U> jump{
            {{media.id, 1'000U}}};
        bool jumped = false;
        state->on_piece = [&](core::u32) {
            if (!jumped) {
                jumped = true;
                worker.set_needs(jump);
            }
        };
        worker.request(media);
        PROXY_CHECK(worker_finished(worker, *key));
    }
    using Range = std::optional<server::ProxySourceRange>;
    PROXY_CHECK((state->ranges ==
                 std::vector<Range>{server::ProxySourceRange{0U, 1'500U},
                                    server::ProxySourceRange{900U, 600U},
                                    server::ProxySourceRange{300U, 600U},
                                    std::nullopt}));
    // Segment 0 was made once: one piece per segment in all.
    PROXY_CHECK(state->pieces.size() == 5U);
    PROXY_CHECK(state->joins == 1U);
}

void next_to_source_storage_routes_from_each_media_path() {
    TemporaryDirectory temporary;
    const fs::path media_directory = temporary.path() / "media";
    PROXY_CHECK(fs::create_directory(media_directory));
    const fs::path source = media_directory / "plate.mov";
    write_file(source, bytes("next-to-source-plate"));
    const assets::MediaAsset media = asset(source, 111U);
    const auto key = assets::requested_proxy_key(media);
    PROXY_CHECK(key.has_value());
    const auto state = std::make_shared<BackendState>();
    state->output = backend_output(*media.fingerprint);
    server::ProxyGenerationService service{
        server::NextToSourceProxyCacheConfig{
            temporary.path() / "runtime", 16U * 1'024U * 1'024U},
        std::make_unique<ControlledBackend>(state)};

    const auto generated = service.generate(media);
    PROXY_CHECK(generated.has_value());
    const fs::path expected_root =
        media_directory / ".mediaserver-proxies";
    PROXY_CHECK(generated.value().cache_entry.path.string().starts_with(
        expected_root.string()));
    PROXY_CHECK(fs::is_regular_file(generated.value().cache_entry.path));
    const auto inspected = service.inspect(media, *key);
    PROXY_CHECK(inspected.has_value());
    PROXY_CHECK(inspected.value().state == assets::DerivativeState::Ready);
    PROXY_CHECK(service.load(media, *key).has_value());
}

void clearing_a_proxy_root_preserves_unrelated_files_and_the_root() {
    TemporaryDirectory temporary;
    const fs::path root = temporary.path() / "cache";
    PROXY_CHECK(fs::create_directory(root));
    for (const fs::path child : {
             fs::path{"v1/proxy-720p-cfr-h264"},
             fs::path{"v1/proxy-source-cfr-hevc-main10"},
             fs::path{".proxy-work"}, fs::path{".proxy-playback"}}) {
        PROXY_CHECK(fs::create_directories(root / child));
        write_file(root / child / "owned.bin", bytes("owned"));
    }
    PROXY_CHECK(fs::create_directories(root / "v1/thumbnail-kept"));
    write_file(root / "v1/thumbnail-kept/keep.bin", bytes("unrelated"));
    write_file(root / "keep.txt", bytes("unrelated"));

    const auto cleared = server::clear_proxy_cache_root(root);
    PROXY_CHECK(cleared);
    PROXY_CHECK(fs::is_directory(root));
    PROXY_CHECK(fs::is_regular_file(root / "keep.txt"));
    PROXY_CHECK(!fs::exists(root / "v1/proxy-720p-cfr-h264"));
    PROXY_CHECK(!fs::exists(
        root / "v1/proxy-source-cfr-hevc-main10"));
    PROXY_CHECK(fs::is_regular_file(
        root / "v1/thumbnail-kept/keep.bin"));
    PROXY_CHECK(!fs::exists(root / ".proxy-work"));
    PROXY_CHECK(!fs::exists(root / ".proxy-playback"));
    PROXY_CHECK(!server::clear_proxy_cache_root(root.root_path()));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 15U> cases{{
    {"unavailable default fails closed without publishing",
     &unavailable_default_fails_closed_without_publishing},
    {"successful backend publishes keyed payload and sidecar",
     &successful_backend_publishes_keyed_payload_and_sidecar},
    {"source revision must match before during and after encode",
     &source_revision_must_match_before_during_and_after_encode},
    {"metadata and frame map validation rejects semantic drift",
     &metadata_and_frame_map_validation_rejects_semantic_drift},
    {"missing and oversized backend outputs never publish",
     &missing_and_oversized_backend_outputs_never_publish},
    {"cancellation stops backend and cleans partial work",
     &cancellation_stops_backend_and_cleans_partial_work},
    {"cache envelopes with invalid sidecars are not cache hits",
     &cache_envelopes_with_invalid_sidecars_are_not_cache_hits},
    {"backend frame progress reaches generation callers",
     &backend_frame_progress_reaches_generation_callers},
    {"a job started during playback asks the backend to yield",
     &a_job_started_during_playback_asks_the_backend_to_yield},
    {"segments publish apart and join into the whole proxy",
     &segments_publish_apart_and_join_into_the_whole_proxy},
    {"a run publishes each segment from one encode",
     &a_run_publishes_each_segment_from_one_encode},
    {"the worker runs from the needed segment then from the start",
     &the_worker_runs_from_the_needed_segment_then_from_the_start},
    {"the worker leaves a run for a newly needed segment",
     &the_worker_leaves_a_run_for_a_newly_needed_segment},
    {"next-to-source storage routes from each media path",
     &next_to_source_storage_routes_from_each_media_path},
    {"clearing a proxy root preserves unrelated files and the root",
     &clearing_a_proxy_root_preserves_unrelated_files_and_the_root},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& caught) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << caught.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
