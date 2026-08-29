#include "proxy_generation_service.hpp"

#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/assets/proxy_policy.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
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
    const auto result = assets::content_fingerprint(path.string());
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

[[nodiscard]] server::ProxyBackendOutput backend_output(
    const assets::ContentFingerprint& consumed_fingerprint) {
    const assets::MediaFrameRate rate{30'000U, 1'001U};
    auto map = assets::make_identity_proxy_frame_map(3U, rate);
    PROXY_CHECK(map.has_value());
    return server::ProxyBackendOutput{
        consumed_fingerprint,
        server::ProxySourceMetadata{
            assets::PixelDimensions{1'920U, 1'080U},
            3U,
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
            3U,
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
            assets::DerivativeAudioLayout::Stereo,
            assets::DerivativeAudioCodec::Aac,
            48'000U,
            192'000U,
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
        }
        state_->condition.notify_all();

        if (!state_->omit_output_file) {
            write_file(request.output_file, state_->payload);
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
            : backend_output(request.source_fingerprint);
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

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 7U> cases{{
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
