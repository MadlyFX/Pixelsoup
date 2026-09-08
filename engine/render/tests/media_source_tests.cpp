#include <mediaserver/render/media_cache.hpp>
#include <mediaserver/render/media_source.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// The decode seam, checked with no GPU. The GPU media suite proves a clip lands
// on the quad; this proves the layer beneath it - the MediaSource - resolves
// paths and decodes frames, and that decode is frame-exact: any frame is
// reachable directly and the same frame always yields the same pixels.
namespace {

namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define SOURCE_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// A small solid-colour 32-bit uncompressed TGA (top-left origin). stb_image
// reads it, and a solid colour makes a frame's identity obvious after decode.
void write_solid_tga(
    const std::string& path, core::u8 red, core::u8 green, core::u8 blue) {
    constexpr core::u32 width = 4U;
    constexpr core::u32 height = 4U;
    std::array<core::u8, 18U> header{};
    header[2U] = 2U; // uncompressed true-colour
    header[12U] = static_cast<core::u8>(width & 0xffU);
    header[14U] = static_cast<core::u8>(height & 0xffU);
    header[16U] = 32U;   // bits per pixel
    header[17U] = 0x28U; // top-left origin, 8 alpha bits

    std::vector<core::u8> pixels;
    for (core::u32 i = 0U; i < width * height; ++i) {
        pixels.push_back(blue);  // TGA is BGRA
        pixels.push_back(green);
        pixels.push_back(red);
        pixels.push_back(255U);
    }
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(header.data()), header.size());
    out.write(
        reinterpret_cast<const char*>(pixels.data()),
        static_cast<std::streamsize>(pixels.size()));
    out.close();
    check(static_cast<bool>(out), "wrote a test frame", __LINE__);
}

[[nodiscard]] std::string temp_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

void the_frame_field_is_substituted_not_formatted() {
    // A run of '#' is the zero-padded frame field.
    SOURCE_CHECK(
        render::ImageSequenceSource::resolve_path("plate_####.png", 7U)
        == "plate_0007.png");
    // No field: a still, returned unchanged whatever the frame.
    SOURCE_CHECK(
        render::ImageSequenceSource::resolve_path("still.png", 3U) == "still.png");
    // A number wider than the field is not truncated.
    SOURCE_CHECK(
        render::ImageSequenceSource::resolve_path("f_#.png", 42U) == "f_42.png");
    // Not a printf format: a '%' in the template is a literal, not a directive.
    SOURCE_CHECK(
        render::ImageSequenceSource::resolve_path("100%_#.png", 5U)
        == "100%_5.png");
}

void a_frame_decodes_to_its_pixels() {
    render::ImageSequenceSource source;
    const auto path = temp_path("mediaserver_source_solid.tga");
    write_solid_tga(path, 220U, 30U, 40U); // a red frame

    const auto frame = source.decode(path, 0U);
    SOURCE_CHECK(frame.valid());
    SOURCE_CHECK(frame.width == 4U);
    SOURCE_CHECK(frame.height == 4U);
    SOURCE_CHECK(frame.format == rhi::TextureFormat::Rgba8Unorm);
    SOURCE_CHECK(frame.pixels.size() == 4U * 4U * 4U);
    // Decoded as RGBA: red first, full alpha.
    SOURCE_CHECK(frame.pixels[0U] > 200U);
    SOURCE_CHECK(frame.pixels[1U] < 60U);
    SOURCE_CHECK(frame.pixels[2U] < 60U);
    SOURCE_CHECK(frame.pixels[3U] == 255U);
}

void decode_is_frame_exact_and_reachable_in_any_order() {
    render::ImageSequenceSource source;
    const auto zero = temp_path("mediaserver_source_seq_0.tga");
    const auto one = temp_path("mediaserver_source_seq_1.tga");
    write_solid_tga(zero, 220U, 30U, 40U); // frame 0: red
    write_solid_tga(one, 30U, 210U, 50U);  // frame 1: green
    const std::string sequence =
        (std::filesystem::temp_directory_path() / "mediaserver_source_seq_#.tga")
            .string();

    // Frame 1 is reachable without first decoding frame 0.
    const auto green = source.decode(sequence, 1U);
    SOURCE_CHECK(green.valid());
    SOURCE_CHECK(green.pixels[0U] < 60U && green.pixels[1U] > 200U);

    // Frame 0 decodes to its own pixels, and re-decoding yields the same bytes:
    // decode is a pure function of (source, frame).
    const auto red_a = source.decode(sequence, 0U);
    const auto red_b = source.decode(sequence, 0U);
    SOURCE_CHECK(red_a.valid());
    SOURCE_CHECK(red_a.pixels[0U] > 200U && red_a.pixels[1U] < 60U);
    SOURCE_CHECK(red_a.pixels == red_b.pixels);
    // The two frames are genuinely different pixels.
    SOURCE_CHECK(red_a.pixels != green.pixels);

    // The cache key is stable and distinguishes frames.
    SOURCE_CHECK(source.key(sequence, 0U) != source.key(sequence, 1U));
    SOURCE_CHECK(source.key(sequence, 0U) == source.key(sequence, 0U));
}

void a_frame_that_cannot_be_read_is_invalid() {
    render::ImageSequenceSource source;
    const auto frame = source.decode("mediaserver_no_such_file_####.png", 3U);
    SOURCE_CHECK(!frame.valid());
    // An empty source names nothing.
    SOURCE_CHECK(source.decode("", 0U).valid() == false);
    // Stills and image sequences intentionally have no intrinsic rate: they
    // retain the legacy one-file-per-show-frame mapping.
    SOURCE_CHECK(!render::probe_media_frame_rate("still.png").has_value());
    SOURCE_CHECK(
        !render::probe_media_frame_rate("plate_####.exr").has_value());
}

struct BlockingPrewarmState final {
    std::mutex mutex;
    std::condition_variable changed;
    bool decode_entered{};
    bool release_decode{};
    bool decode_exited{};
    bool cache_destroyed{};
    bool source_destroyed{};
    core::u32 decode_calls{};
    std::thread::id decode_thread;
    std::vector<core::u32> decoded_frames;
};

class BlockingPrewarmSource final : public render::MediaSource {
public:
    explicit BlockingPrewarmSource(
        std::shared_ptr<BlockingPrewarmState> state)
        : state_(std::move(state)) {}

    ~BlockingPrewarmSource() override {
        std::lock_guard lock{state_->mutex};
        state_->source_destroyed = true;
        state_->changed.notify_all();
    }

    [[nodiscard]] std::string key(
        const std::string& source, core::u32 frame_index) const override {
        return source + "#" + std::to_string(frame_index);
    }

    [[nodiscard]] render::DecodedFrame decode(
        const std::string&, core::u32 frame_index) override {
        std::unique_lock lock{state_->mutex};
        ++state_->decode_calls;
        state_->decode_thread = std::this_thread::get_id();
        state_->decoded_frames.push_back(frame_index);
        state_->decode_entered = true;
        state_->changed.notify_all();
        state_->changed.wait(
            lock, [&] { return state_->release_decode; });
        state_->decode_exited = true;
        state_->changed.notify_all();
        return {};
    }

private:
    std::shared_ptr<BlockingPrewarmState> state_;
};

void cache_teardown_never_joins_an_in_flight_decode() {
    const auto state = std::make_shared<BlockingPrewarmState>();
    auto cache = std::make_unique<render::MediaCache>(
        std::make_unique<BlockingPrewarmSource>(state));
    const std::thread::id caller = std::this_thread::get_id();

    // Admission is intentionally try-lock/drop. Retry the operator-invoked
    // prewarm until the process-global worker has accepted and entered it.
    const auto enter_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    bool entered = false;
    while (!entered && std::chrono::steady_clock::now() < enter_deadline) {
        cache->prewarm("blocking-prewarm.png", 0U);
        std::unique_lock lock{state->mutex};
        entered = state->changed.wait_for(
            lock, std::chrono::milliseconds{1},
            [&] { return state->decode_entered; });
    }
    if (!entered) {
        std::lock_guard lock{state->mutex};
        state->release_decode = true;
        state->changed.notify_all();
    }
    SOURCE_CHECK(entered);

    // Destroy on another thread so a regression that joins the blocked decode
    // is observable without hanging this CPU-only suite. Release happens only
    // after teardown has had a bounded opportunity to return independently.
    std::jthread destroyer{
        [state, owned = std::move(cache)]() mutable {
            owned.reset();
            std::lock_guard lock{state->mutex};
            state->cache_destroyed = true;
            state->changed.notify_all();
        }};
    bool returned_before_release = false;
    {
        std::unique_lock lock{state->mutex};
        returned_before_release = state->changed.wait_for(
            lock, std::chrono::seconds{2},
            [&] { return state->cache_destroyed; });
        state->release_decode = true;
        state->changed.notify_all();
    }
    destroyer.join();

    SOURCE_CHECK(returned_before_release);
    {
        std::unique_lock lock{state->mutex};
        SOURCE_CHECK(state->decode_calls == 1U);
        SOURCE_CHECK(state->decode_thread != caller);
        SOURCE_CHECK(state->changed.wait_for(
            lock, std::chrono::seconds{2},
            [&] {
                return state->decode_exited && state->source_destroyed;
            }));
    }
}

void one_process_queue_enforces_each_cache_s_speculative_bound() {
    const auto bounded_state = std::make_shared<BlockingPrewarmState>();
    render::MediaCacheConfig bounded_config{};
    bounded_config.max_read_ahead_requests = 2U;
    auto bounded = std::make_unique<render::MediaCache>(
        std::make_unique<BlockingPrewarmSource>(bounded_state),
        bounded_config);

    const auto enter_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    bool entered = false;
    while (!entered && std::chrono::steady_clock::now() < enter_deadline) {
        bounded->prewarm("bounded.png", 0U);
        std::unique_lock lock{bounded_state->mutex};
        entered = bounded_state->changed.wait_for(
            lock, std::chrono::milliseconds{1},
            [&] { return bounded_state->decode_entered; });
    }
    if (!entered) {
        std::lock_guard lock{bounded_state->mutex};
        bounded_state->release_decode = true;
        bounded_state->changed.notify_all();
    }
    SOURCE_CHECK(entered);

    // Frame zero occupies the process worker. One speculative slot remains
    // for this cache, so frame one is admitted and frame two is dropped.
    for (core::u32 retry = 0U; retry < 8U; ++retry) {
        bounded->prewarm("bounded.png", 1U);
    }
    for (core::u32 retry = 0U; retry < 8U; ++retry) {
        bounded->prewarm("bounded.png", 2U);
    }

    // A second cache supplies a FIFO barrier. When its decode has run, every
    // earlier accepted speculative job has run too, so the negative bound
    // assertion below does not rely on sleeping and hoping frame two is late.
    const auto barrier_state = std::make_shared<BlockingPrewarmState>();
    {
        std::lock_guard lock{barrier_state->mutex};
        barrier_state->release_decode = true;
    }
    auto barrier = std::make_unique<render::MediaCache>(
        std::make_unique<BlockingPrewarmSource>(barrier_state));
    for (core::u32 retry = 0U; retry < 8U; ++retry) {
        barrier->prewarm("barrier.png", 9U);
    }

    {
        std::lock_guard lock{bounded_state->mutex};
        bounded_state->release_decode = true;
        bounded_state->changed.notify_all();
    }
    {
        std::unique_lock lock{barrier_state->mutex};
        SOURCE_CHECK(barrier_state->changed.wait_for(
            lock, std::chrono::seconds{2},
            [&] { return barrier_state->decode_exited; }));
    }
    {
        std::lock_guard lock{bounded_state->mutex};
        SOURCE_CHECK(
            bounded_state->decoded_frames
            == std::vector<core::u32>{0U, 1U});
    }
    {
        std::scoped_lock lock{
            bounded_state->mutex, barrier_state->mutex};
        SOURCE_CHECK(bounded_state->decode_thread
                     == barrier_state->decode_thread);
        SOURCE_CHECK(barrier_state->decoded_frames
                     == std::vector<core::u32>{9U});
    }
}

struct RepairableSourceState final {
    std::mutex mutex;
    std::condition_variable changed;
    bool available{};
    core::u32 decode_calls{};
    core::u32 valid_decodes{};
};

class RepairableBackgroundSource final : public render::MediaSource {
public:
    explicit RepairableBackgroundSource(
        std::shared_ptr<RepairableSourceState> state)
        : state_(std::move(state)) {}

    [[nodiscard]] std::string key(
        const std::string& source, core::u32 frame_index) const override {
        return source + "#" + std::to_string(frame_index);
    }

    [[nodiscard]] render::DecodedFrame decode(
        const std::string&, core::u32 frame_index) override {
        bool available = false;
        {
            std::lock_guard lock{state_->mutex};
            ++state_->decode_calls;
            available = state_->available;
            if (available) {
                ++state_->valid_decodes;
            }
            state_->changed.notify_all();
        }
        if (!available) {
            return {};
        }
        render::DecodedFrame frame{};
        frame.width = 2U;
        frame.height = 2U;
        frame.source_frame = frame_index;
        frame.pixels.resize(2U * 2U * 4U, 255U);
        return frame;
    }

private:
    std::shared_ptr<RepairableSourceState> state_;
};

void wait_for_decode_count(
    render::MediaCache& cache,
    const std::shared_ptr<RepairableSourceState>& state,
    const std::string& source,
    core::u32 count) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    for (;;) {
        {
            std::lock_guard lock{state->mutex};
            if (state->decode_calls >= count) {
                return;
            }
        }
        cache.prewarm(source, 0U);
        std::unique_lock lock{state->mutex};
        if (state->changed.wait_for(
                lock, std::chrono::milliseconds{1},
                [&] { return state->decode_calls >= count; })) {
            return;
        }
        SOURCE_CHECK(std::chrono::steady_clock::now() < deadline);
    }
}

void pass_background_executor_barrier(core::u32 identity) {
    const auto state = std::make_shared<BlockingPrewarmState>();
    {
        std::lock_guard lock{state->mutex};
        state->release_decode = true;
    }
    render::MediaCache barrier{
        std::make_unique<BlockingPrewarmSource>(state)};
    const std::string source =
        "refresh-barrier-" + std::to_string(identity) + ".png";
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{2};
    for (;;) {
        barrier.prewarm(source, 0U);
        std::unique_lock lock{state->mutex};
        if (state->changed.wait_for(
                lock, std::chrono::milliseconds{1},
                [&] { return state->decode_entered; })) {
            return;
        }
        SOURCE_CHECK(std::chrono::steady_clock::now() < deadline);
    }
}

void a_sticky_background_failure_recovers_only_after_explicit_refresh() {
    const auto state = std::make_shared<RepairableSourceState>();
    render::MediaCache cache{
        std::make_unique<RepairableBackgroundSource>(state)};
    const std::string source = "repairable-still.png";

    wait_for_decode_count(cache, state, source, 1U);
    pass_background_executor_barrier(1U);

    // The completed invalid frame is a sticky bounded answer. Repeated visible
    // and speculative demand cannot turn file repair into an implicit retry.
    for (core::u32 attempt = 0U; attempt < 64U; ++attempt) {
        cache.prewarm(source, 0U);
    }
    pass_background_executor_barrier(2U);
    {
        std::lock_guard lock{state->mutex};
        SOURCE_CHECK(state->decode_calls == 1U);
        state->available = true;
    }

    // The source inherits MediaSource::refresh() == false. MediaCache still
    // owns the BackgroundRequired generation/ready state and must invalidate
    // it through this explicit operator seam.
    SOURCE_CHECK(cache.refresh_source(source));
    wait_for_decode_count(cache, state, source, 2U);
    pass_background_executor_barrier(3U);

    for (core::u32 attempt = 0U; attempt < 64U; ++attempt) {
        cache.prewarm(source, 0U);
    }
    pass_background_executor_barrier(4U);
    {
        std::lock_guard lock{state->mutex};
        SOURCE_CHECK(state->decode_calls == 2U);
        SOURCE_CHECK(state->valid_decodes == 1U);
    }
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 7U> cases{{
    {"the frame field is substituted not formatted",
     &the_frame_field_is_substituted_not_formatted},
    {"a frame decodes to its pixels", &a_frame_decodes_to_its_pixels},
    {"decode is frame-exact and reachable in any order",
     &decode_is_frame_exact_and_reachable_in_any_order},
    {"a frame that cannot be read is invalid",
     &a_frame_that_cannot_be_read_is_invalid},
    {"cache teardown never joins an in-flight decode",
     &cache_teardown_never_joins_an_in_flight_decode},
    {"one process queue enforces each cache's speculative bound",
     &one_process_queue_enforces_each_cache_s_speculative_bound},
    {"a sticky background failure recovers only after explicit refresh",
     &a_sticky_background_failure_recovers_only_after_explicit_refresh},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
