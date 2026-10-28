#include "thumbnail_service.hpp"

#include <mediaserver/assets/asset_path.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>

#include <stb_image.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
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

#define THUMBNAIL_CHECK(...)                                                 \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        static std::atomic<core::u64> sequence{};
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch().count();
        path_ = fs::temp_directory_path() /
            ("mediaserver_thumbnail_service_" + std::to_string(stamp) + "_" +
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

void write(const fs::path& path, std::string_view content) {
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    output.close();
    if (!output) {
        throw std::runtime_error{"could not write source fixture"};
    }
}

[[nodiscard]] assets::MediaAsset make_asset(
    doc::NodeId id, const fs::path& path, core::u32 frame_count = 100U) {
    const std::string source = assets::asset_source_from_path(path);
    const auto fingerprint = assets::source_fingerprint(source);
    if (!fingerprint) {
        throw std::runtime_error{"could not fingerprint source fixture"};
    }
    return assets::MediaAsset{
        id,
        "Test media",
        source,
        assets::PixelDimensions{4U, 2U},
        frame_count,
        assets::MediaFrameRate{24U, 1U},
        fingerprint.value(),
    };
}

[[nodiscard]] assets::DerivativeCache open_cache(const fs::path& root) {
    auto opened = assets::DerivativeCache::open(
        {root, 16U * 1'024U * 1'024U});
    if (!opened) {
        throw std::runtime_error{"could not open derivative cache"};
    }
    return std::move(opened).value();
}

struct DecoderState final {
    std::mutex mutex;
    std::condition_variable ready;
    core::u32 frame_count{100U};
    std::optional<core::u32> valid_ordinal{12U};
    bool accept_all_ordinals{};
    std::vector<core::u32> ordinals;
    bool block{};
    bool entered{};
    bool release{};
    core::u8 rotation{};
    core::u32 pixel_aspect_numerator{1U};
    core::u32 pixel_aspect_denominator{1U};
};

class FakeDecoder final : public server::ThumbnailDecoder {
public:
    explicit FakeDecoder(std::shared_ptr<DecoderState> state)
        : state_(std::move(state)) {}

    core::u32 frame_count(const std::string&) override {
        std::lock_guard lock{state_->mutex};
        return state_->frame_count;
    }

    server::ThumbnailRgbaFrame decode(
        const std::string&, core::u32 source_ordinal,
        std::stop_token stop) override {
        std::unique_lock lock{state_->mutex};
        state_->ordinals.push_back(source_ordinal);
        state_->entered = true;
        state_->ready.notify_all();
        std::stop_callback cancellation{stop, [state = state_] {
            state->ready.notify_all();
        }};
        state_->ready.wait(lock, [&] {
            return !state_->block || state_->release || stop.stop_requested();
        });
        if (stop.stop_requested() ||
            (!state_->accept_all_ordinals &&
             (!state_->valid_ordinal.has_value() ||
              source_ordinal != *state_->valid_ordinal))) {
            return {};
        }
        const core::u8 rotation = state_->rotation;
        const core::u32 aspect_numerator =
            state_->pixel_aspect_numerator;
        const core::u32 aspect_denominator =
            state_->pixel_aspect_denominator;
        lock.unlock();

        std::vector<core::u8> pixels(4U * 2U * 4U, 0U);
        for (std::size_t index = 0U; index < pixels.size(); index += 4U) {
            pixels[index] = 0xffU;
            pixels[index + 1U] = static_cast<core::u8>(source_ordinal);
            pixels[index + 3U] = 0xffU;
        }
        return server::ThumbnailRgbaFrame{
            4U, 2U, std::move(pixels), rotation,
            aspect_numerator, aspect_denominator};
    }

private:
    std::shared_ptr<DecoderState> state_;
};

[[nodiscard]] server::ThumbnailDecoderFactory factory(
    const std::shared_ptr<DecoderState>& state) {
    return [state] { return std::make_unique<FakeDecoder>(state); };
}

[[nodiscard]] server::ThumbnailSnapshot wait_for_state(
    server::ThumbnailService& service, const assets::DerivativeKey& key,
    assets::DerivativeState state) {
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds{5};
    while (std::chrono::steady_clock::now() < deadline) {
        const auto snapshot = service.snapshot(key);
        if (snapshot.has_value() && snapshot->state == state) {
            return *snapshot;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    throw std::runtime_error{"timed out waiting for thumbnail state"};
}

[[nodiscard]] std::vector<core::u32> decoded_ordinals(
    const std::shared_ptr<DecoderState>& state) {
    std::lock_guard lock{state->mutex};
    return state->ordinals;
}

void deterministic_probe_png_and_asset_identity() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "source.bin";
    write(source_path, "stable source revision");
    assets::MediaAsset asset = make_asset(doc::NodeId{41U}, source_path);
    const assets::MediaAsset original = asset;
    const assets::AssetRegistry registry{{assets::Asset{asset}}};
    const core::u64 registry_digest = assets::digest(registry);
    auto decoder = std::make_shared<DecoderState>();
    server::ThumbnailService service{
        open_cache(temporary.path() / "cache"),
        server::ThumbnailServiceConfig{1U, 8U, 4U, 32U},
        factory(decoder)};

    const auto requested = service.request(asset);
    THUMBNAIL_CHECK(requested);
    THUMBNAIL_CHECK(requested.value().state ==
                    assets::DerivativeState::Queued);
    THUMBNAIL_CHECK(requested.value().key.asset == asset.id);
    const server::ThumbnailSnapshot ready = wait_for_state(
        service, requested.value().key, assets::DerivativeState::Ready);
    THUMBNAIL_CHECK(ready.source_ordinal == 12U);
    THUMBNAIL_CHECK(ready.attempted_frames == 3U);
    THUMBNAIL_CHECK(decoded_ordinals(decoder) ==
                    std::vector<core::u32>{10U, 11U, 12U});

    const auto loaded = service.load(ready.key);
    THUMBNAIL_CHECK(loaded);
    THUMBNAIL_CHECK(loaded.value().entry.state ==
                    assets::DerivativeState::Ready);
    const std::vector<core::u8>& png = loaded.value().payload;
    THUMBNAIL_CHECK(png.size() <=
                    static_cast<std::size_t>(
                        std::numeric_limits<int>::max()));
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(
        png.data(), static_cast<int>(png.size()), &width, &height, &channels, 4);
    THUMBNAIL_CHECK(decoded != nullptr);
    THUMBNAIL_CHECK(width == 320);
    THUMBNAIL_CHECK(height == 180);
    // 2:1 source content is transparently letterboxed into 16:9.
    THUMBNAIL_CHECK(decoded[3U] == 0U);
    const std::size_t centre =
        (static_cast<std::size_t>(90) * 320U + 160U) * 4U;
    THUMBNAIL_CHECK(decoded[centre] == 0xffU);
    THUMBNAIL_CHECK(decoded[centre + 3U] == 0xffU);
    stbi_image_free(decoded);

    THUMBNAIL_CHECK(asset == original);
    THUMBNAIL_CHECK(registry.assets.front() == assets::Asset{original});
    THUMBNAIL_CHECK(assets::digest(registry) == registry_digest);

    const auto cached = service.request(asset);
    THUMBNAIL_CHECK(cached);
    THUMBNAIL_CHECK(cached.value().state == assets::DerivativeState::Ready);
    THUMBNAIL_CHECK(decoded_ordinals(decoder).size() == 3U);
}

void production_decoder_generates_a_still_thumbnail() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "source.ppm";
    std::string ppm{"P6\n4 2\n255\n"};
    for (std::size_t pixel = 0U; pixel < 8U; ++pixel) {
        ppm.push_back(static_cast<char>(0xff));
        ppm.push_back('\0');
        ppm.push_back('\0');
    }
    write(source_path, ppm);
    const assets::MediaAsset asset = make_asset(
        doc::NodeId{42U}, source_path, 1U);
    server::ThumbnailService service{
        open_cache(temporary.path() / "cache"),
        server::ThumbnailServiceConfig{1U, 4U, 2U, 16U}};

    const auto requested = service.request(asset);
    THUMBNAIL_CHECK(requested);
    const server::ThumbnailSnapshot ready = wait_for_state(
        service, requested.value().key,
        assets::DerivativeState::Ready);
    THUMBNAIL_CHECK(ready.source_ordinal == 0U);
    const auto loaded = service.load(ready.key);
    THUMBNAIL_CHECK(loaded);
    THUMBNAIL_CHECK(loaded.value().payload.size() > 24U);
}

void filmstrip_uses_uniform_ordinals_and_one_manifest_cache_key() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "source.bin";
    write(source_path, "filmstrip source revision");
    const assets::MediaAsset asset = make_asset(
        doc::NodeId{43U}, source_path, 8U);
    auto decoder = std::make_shared<DecoderState>();
    decoder->frame_count = 8U;
    decoder->accept_all_ordinals = true;
    server::ThumbnailService service{
        open_cache(temporary.path() / "cache"),
        server::ThumbnailServiceConfig{1U, 4U, 2U, 16U},
        factory(decoder)};

    const auto requested = service.request_filmstrip(asset);
    THUMBNAIL_CHECK(requested);
    THUMBNAIL_CHECK(requested.value().key.asset == asset.id);
    THUMBNAIL_CHECK(requested.value().key.profile ==
                    assets::DerivativeProfileId::Filmstrip8x160x90Srgb);
    const server::ThumbnailSnapshot ready = wait_for_state(
        service, requested.value().key, assets::DerivativeState::Ready);
    const std::vector<core::u64> expected_ordinals{
        0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U};
    THUMBNAIL_CHECK(ready.source_ordinals == expected_ordinals);
    THUMBNAIL_CHECK(ready.attempted_frames == 8U);
    THUMBNAIL_CHECK(decoded_ordinals(decoder) ==
                    std::vector<core::u32>{0U, 1U, 2U, 3U,
                                           4U, 5U, 6U, 7U});

    const auto loaded = service.load(ready.key);
    THUMBNAIL_CHECK(loaded);
    const auto& png = loaded.value().payload;
    THUMBNAIL_CHECK(png.size() <=
                    static_cast<std::size_t>(
                        std::numeric_limits<int>::max()));
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decoded = stbi_load_from_memory(
        png.data(), static_cast<int>(png.size()), &width, &height, &channels,
        4);
    THUMBNAIL_CHECK(decoded != nullptr);
    THUMBNAIL_CHECK(width == 8 * 160);
    THUMBNAIL_CHECK(height == 90);
    for (std::size_t tile = 0U; tile < 8U; ++tile) {
        const std::size_t centre =
            (static_cast<std::size_t>(45) * static_cast<std::size_t>(width) +
             tile * 160U + 80U) * 4U;
        THUMBNAIL_CHECK(decoded[centre] == 0xffU);
        THUMBNAIL_CHECK(decoded[centre + 1U] ==
                        static_cast<stbi_uc>(tile));
        THUMBNAIL_CHECK(decoded[centre + 3U] == 0xffU);
    }
    stbi_image_free(decoded);

    const auto cached = service.request_filmstrip(asset);
    THUMBNAIL_CHECK(cached);
    THUMBNAIL_CHECK(cached.value().state == assets::DerivativeState::Ready);
    THUMBNAIL_CHECK(decoded_ordinals(decoder).size() == 8U);
}

void retain_cancels_in_flight_work() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "source.bin";
    write(source_path, "cancel source revision");
    const assets::MediaAsset asset = make_asset(
        doc::NodeId{51U}, source_path);
    auto decoder = std::make_shared<DecoderState>();
    decoder->block = true;
    decoder->valid_ordinal = 10U;
    server::ThumbnailService service{
        open_cache(temporary.path() / "cache"),
        server::ThumbnailServiceConfig{1U, 4U, 2U, 16U},
        factory(decoder)};
    const auto requested = service.request(asset);
    THUMBNAIL_CHECK(requested);
    static_cast<void>(wait_for_state(
        service, requested.value().key,
        assets::DerivativeState::Generating));

    service.retain({});
    const server::ThumbnailSnapshot cancelled = wait_for_state(
        service, requested.value().key,
        assets::DerivativeState::Cancelled);
    THUMBNAIL_CHECK(cancelled.key.asset == asset.id);
    const auto loaded = service.load(cancelled.key);
    THUMBNAIL_CHECK(loaded);
    THUMBNAIL_CHECK(loaded.value().entry.state ==
                    assets::DerivativeState::Missing);
}

void visible_work_displaces_bounded_prefetch() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "source.bin";
    write(source_path, "queue source revision");
    const assets::MediaAsset first = make_asset(
        doc::NodeId{61U}, source_path);
    const assets::MediaAsset prefetched = make_asset(
        doc::NodeId{62U}, source_path);
    const assets::MediaAsset visible = make_asset(
        doc::NodeId{63U}, source_path);
    auto decoder = std::make_shared<DecoderState>();
    decoder->block = true;
    decoder->valid_ordinal = 10U;
    server::ThumbnailService service{
        open_cache(temporary.path() / "cache"),
        server::ThumbnailServiceConfig{1U, 1U, 1U, 16U},
        factory(decoder)};

    const auto first_request = service.request(first);
    THUMBNAIL_CHECK(first_request);
    static_cast<void>(wait_for_state(
        service, first_request.value().key,
        assets::DerivativeState::Generating));
    const auto prefetch_request = service.request(
        prefetched, server::ThumbnailRequestPriority::Prefetch);
    THUMBNAIL_CHECK(prefetch_request);
    THUMBNAIL_CHECK(prefetch_request.value().state ==
                    assets::DerivativeState::Queued);
    const auto visible_request = service.request(visible);
    THUMBNAIL_CHECK(visible_request);
    THUMBNAIL_CHECK(visible_request.value().state ==
                    assets::DerivativeState::Queued);
    const server::ThumbnailSnapshot evicted = wait_for_state(
        service, prefetch_request.value().key,
        assets::DerivativeState::Cancelled);
    THUMBNAIL_CHECK(evicted.failure ==
                    server::ThumbnailFailureCode::QueueCapacity);

    {
        std::lock_guard lock{decoder->mutex};
        decoder->release = true;
        decoder->ready.notify_all();
    }
    static_cast<void>(wait_for_state(
        service, first_request.value().key,
        assets::DerivativeState::Ready));
    static_cast<void>(wait_for_state(
        service, visible_request.value().key,
        assets::DerivativeState::Ready));
    THUMBNAIL_CHECK(service.stats().prefetch_limit == 1U);
    THUMBNAIL_CHECK(service.stats().pending_limit == 1U);
}

void fingerprint_mismatch_is_visible_stale_and_never_decodes() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "source.bin";
    write(source_path, "expected revision");
    const assets::MediaAsset asset = make_asset(
        doc::NodeId{71U}, source_path);
    write(source_path, "changed revision");
    auto decoder = std::make_shared<DecoderState>();
    server::ThumbnailService service{
        open_cache(temporary.path() / "cache"),
        server::ThumbnailServiceConfig{1U, 4U, 2U, 16U},
        factory(decoder)};

    const auto requested = service.request(asset);
    THUMBNAIL_CHECK(requested);
    const server::ThumbnailSnapshot stale = wait_for_state(
        service, requested.value().key,
        assets::DerivativeState::Stale);
    THUMBNAIL_CHECK(stale.failure ==
                    server::ThumbnailFailureCode::FingerprintMismatch);
    THUMBNAIL_CHECK(decoded_ordinals(decoder).empty());
    THUMBNAIL_CHECK(stale.key.asset == asset.id);
}

void source_change_during_decode_is_not_published() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "source.bin";
    write(source_path, "expected in-flight revision");
    const assets::MediaAsset asset = make_asset(
        doc::NodeId{72U}, source_path);
    auto decoder = std::make_shared<DecoderState>();
    decoder->block = true;
    decoder->valid_ordinal = 10U;
    server::ThumbnailService service{
        open_cache(temporary.path() / "cache"),
        server::ThumbnailServiceConfig{1U, 4U, 2U, 16U},
        factory(decoder)};

    const auto requested = service.request(asset);
    THUMBNAIL_CHECK(requested);
    static_cast<void>(wait_for_state(
        service, requested.value().key,
        assets::DerivativeState::Generating));
    {
        std::unique_lock lock{decoder->mutex};
        THUMBNAIL_CHECK(decoder->ready.wait_for(
            lock, std::chrono::seconds{5},
            [&] { return decoder->entered; }));
    }
    write(source_path, "replacement during decode");
    {
        std::lock_guard lock{decoder->mutex};
        decoder->release = true;
        decoder->ready.notify_all();
    }
    const server::ThumbnailSnapshot stale = wait_for_state(
        service, requested.value().key,
        assets::DerivativeState::Stale);
    THUMBNAIL_CHECK(stale.failure ==
                    server::ThumbnailFailureCode::FingerprintMismatch);
    const auto loaded = service.load(stale.key);
    THUMBNAIL_CHECK(loaded);
    THUMBNAIL_CHECK(loaded.value().entry.state ==
                    assets::DerivativeState::Missing);
}

void no_valid_frame_reports_failed_after_exact_fallback_order() {
    TemporaryDirectory temporary;
    const fs::path source_path = temporary.path() / "source.bin";
    write(source_path, "decode failure revision");
    const assets::MediaAsset asset = make_asset(
        doc::NodeId{81U}, source_path, 3U);
    auto decoder = std::make_shared<DecoderState>();
    decoder->frame_count = 3U;
    decoder->valid_ordinal.reset();
    server::ThumbnailService service{
        open_cache(temporary.path() / "cache"),
        server::ThumbnailServiceConfig{1U, 4U, 2U, 16U},
        factory(decoder)};

    const auto requested = service.request(asset);
    THUMBNAIL_CHECK(requested);
    const server::ThumbnailSnapshot failed = wait_for_state(
        service, requested.value().key,
        assets::DerivativeState::Failed);
    THUMBNAIL_CHECK(failed.failure ==
                    server::ThumbnailFailureCode::NoValidFrame);
    THUMBNAIL_CHECK(decoded_ordinals(decoder) ==
                    std::vector<core::u32>{1U, 2U, 0U});
}

[[nodiscard]] assets::ModelAsset make_model(doc::NodeId id,
                                            const fs::path& path) {
    const std::string source = assets::asset_source_from_path(path);
    const auto fingerprint = assets::source_fingerprint(source);
    if (!fingerprint) {
        throw std::runtime_error{"could not fingerprint model fixture"};
    }
    assets::ModelAsset model{};
    model.id = id;
    model.display_name = "Test model";
    model.source = source;
    model.fingerprint = fingerprint.value();
    return model;
}

// A model's thumbnail is drawn by the editor: the request waits, untouched by
// the decode workers, until the editor takes it; the picture it brings back
// is encoded and cached under the model profile's key. An empty picture
// fails it, and work cancelled while the editor draws drops its picture.
void model_thumbnails_wait_for_the_editors_picture() {
    TemporaryDirectory temporary;
    const fs::path drawn_path = temporary.path() / "drawn.glb";
    const fs::path broken_path = temporary.path() / "broken.glb";
    const fs::path dropped_path = temporary.path() / "dropped.glb";
    write(drawn_path, "a model revision");
    write(broken_path, "a model that does not draw");
    write(dropped_path, "a model nobody looks at any more");
    const assets::ModelAsset drawn = make_model(doc::NodeId{61U}, drawn_path);
    const assets::ModelAsset broken = make_model(doc::NodeId{62U}, broken_path);
    const assets::ModelAsset dropped = make_model(doc::NodeId{63U}, dropped_path);
    auto decoder = std::make_shared<DecoderState>();
    server::ThumbnailService service{
        open_cache(temporary.path() / "cache"),
        server::ThumbnailServiceConfig{1U, 8U, 4U, 32U},
        factory(decoder)};

    const auto requested = service.request_model(drawn);
    THUMBNAIL_CHECK(requested);
    THUMBNAIL_CHECK(requested.value().state == assets::DerivativeState::Queued);
    THUMBNAIL_CHECK(requested.value().key.profile ==
                    assets::DerivativeProfileId::ModelTurntable320x180Srgb);
    THUMBNAIL_CHECK(requested.value().key.asset == drawn.id);
    // Asking again while it waits is the same work.
    THUMBNAIL_CHECK(service.request_model(drawn).value().revision ==
                    requested.value().revision);
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    THUMBNAIL_CHECK(service.snapshot(requested.value().key)->state ==
                    assets::DerivativeState::Queued);
    THUMBNAIL_CHECK(decoded_ordinals(decoder).empty());

    const std::vector<server::ModelThumbnailRender> taken =
        service.take_model_renders(4U);
    THUMBNAIL_CHECK(taken.size() == 1U);
    THUMBNAIL_CHECK(taken.front().key == requested.value().key);
    THUMBNAIL_CHECK(taken.front().source == drawn.source);
    THUMBNAIL_CHECK(service.snapshot(requested.value().key)->state ==
                    assets::DerivativeState::Generating);
    THUMBNAIL_CHECK(service.take_model_renders(4U).empty());

    // A red first pixel on blue.
    std::vector<core::u8> picture(320U * 180U * 4U, 0U);
    for (std::size_t at = 0U; at < picture.size(); at += 4U) {
        picture[at + 2U] = 200U;
        picture[at + 3U] = 255U;
    }
    picture[0U] = 255U;
    picture[2U] = 0U;
    service.complete_model_render(requested.value().key, picture);
    const server::ThumbnailSnapshot ready = wait_for_state(
        service, requested.value().key, assets::DerivativeState::Ready);
    const auto loaded = service.load(ready.key);
    THUMBNAIL_CHECK(loaded);
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* const decoded = stbi_load_from_memory(
        loaded.value().payload.data(),
        static_cast<int>(loaded.value().payload.size()), &width, &height,
        &channels, 4);
    THUMBNAIL_CHECK(decoded != nullptr);
    THUMBNAIL_CHECK(width == 320 && height == 180);
    THUMBNAIL_CHECK(decoded[0U] == 255U && decoded[2U] == 0U);
    THUMBNAIL_CHECK(decoded[4U] == 0U && decoded[6U] == 200U);
    stbi_image_free(decoded);
    // Cached, it is Ready at once and needs no second picture.
    THUMBNAIL_CHECK(service.request_model(drawn).value().state ==
                    assets::DerivativeState::Ready);
    THUMBNAIL_CHECK(service.take_model_renders(4U).empty());

    // No picture: Failed.
    const auto broken_request = service.request_model(broken);
    THUMBNAIL_CHECK(service.take_model_renders(4U).size() == 1U);
    service.complete_model_render(broken_request.value().key, {});
    const server::ThumbnailSnapshot failed = wait_for_state(
        service, broken_request.value().key, assets::DerivativeState::Failed);
    THUMBNAIL_CHECK(failed.failure == server::ThumbnailFailureCode::NoValidFrame);

    // Cancelled while drawn: the picture is dropped and nothing is cached.
    const auto dropped_request = service.request_model(dropped);
    THUMBNAIL_CHECK(service.take_model_renders(4U).size() == 1U);
    service.retain({});
    THUMBNAIL_CHECK(service.snapshot(dropped_request.value().key)->state ==
                    assets::DerivativeState::Cancelled);
    service.complete_model_render(dropped_request.value().key, picture);
    std::this_thread::sleep_for(std::chrono::milliseconds{20});
    THUMBNAIL_CHECK(service.snapshot(dropped_request.value().key)->state ==
                    assets::DerivativeState::Cancelled);
    const auto never = service.load(dropped_request.value().key);
    THUMBNAIL_CHECK(never);
    THUMBNAIL_CHECK(never.value().entry.state ==
                    assets::DerivativeState::Missing);
    // A model without a fingerprint is refused outright.
    assets::ModelAsset unfingerprinted = drawn;
    unfingerprinted.fingerprint.reset();
    THUMBNAIL_CHECK(!service.request_model(unfingerprinted));
}

void invalid_assets_never_enter_the_queue() {
    TemporaryDirectory temporary;
    auto decoder = std::make_shared<DecoderState>();
    server::ThumbnailService service{
        open_cache(temporary.path() / "cache"), {}, factory(decoder)};
    assets::MediaAsset missing_fingerprint{};
    missing_fingerprint.id = doc::NodeId{91U};
    missing_fingerprint.source = assets::asset_source_from_path(
        temporary.path() / "not-read.bin");
    const auto requested = service.request(missing_fingerprint);
    THUMBNAIL_CHECK(!requested);
    THUMBNAIL_CHECK(requested.error().code ==
                    server::ThumbnailServiceErrorCode::MissingFingerprint);
    THUMBNAIL_CHECK(service.stats().queued == 0U);
    THUMBNAIL_CHECK(service.stats().generating == 0U);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 10U> cases{{
    {"deterministic probe PNG and asset identity",
     &deterministic_probe_png_and_asset_identity},
    {"production decoder generates a still thumbnail",
     &production_decoder_generates_a_still_thumbnail},
    {"filmstrip uses uniform ordinals and one manifest cache key",
     &filmstrip_uses_uniform_ordinals_and_one_manifest_cache_key},
    {"retain cancels in-flight work", &retain_cancels_in_flight_work},
    {"visible work displaces bounded prefetch",
     &visible_work_displaces_bounded_prefetch},
    {"fingerprint mismatch is visible stale and never decodes",
     &fingerprint_mismatch_is_visible_stale_and_never_decodes},
    {"source change during decode is not published",
     &source_change_during_decode_is_not_published},
    {"no valid frame reports failed after exact fallback order",
     &no_valid_frame_reports_failed_after_exact_fallback_order},
    {"invalid assets never enter the queue",
     &invalid_assets_never_enter_the_queue},
    {"model thumbnails wait for the editor's picture",
     &model_thumbnails_wait_for_the_editors_picture},
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
