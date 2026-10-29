#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <dxgi1_4.h>
#pragma comment(lib, "dxgi.lib")
#endif

#include <mediaserver/render/scene_renderer.hpp>

#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Optional, real-file Release benchmark. It records the production
// SceneRenderer path twice per command list (Program + Stage) through the same
// frame-scoped media cache, matching the editor's decode and GPU-upload reuse.
// No threshold is enforced unless explicitly requested on the command line.
namespace {

namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;

struct Options final {
    std::vector<std::string> paths;
    std::size_t warmup{8U};
    std::size_t frames{48U};
    double cadence_fps{};
    double minimum_ready_ratio{-1.0};
    double maximum_render_ms{-1.0};
    std::optional<std::size_t> second_clip_offset;
    // Image sequences have no probed length; this supplies it.
    std::size_t sequence_frames{};
    // Frames advanced per tick: 1 is playback, larger values model a
    // scrub drag. Reverse walks backwards from the last frame.
    std::size_t stride{1U};
    bool reverse{};
    // When non-zero, measure time-to-exact-frame for this many seeks
    // instead of cadence playback.
    std::size_t seek_targets{};
    bool hold_frame{};
    bool radial_plates{};
    bool buffered_output{};
    bool d3d12{};
    bool half_resolution{};
};

[[nodiscard]] std::size_t parse_count(std::string_view text) {
    const auto value = std::stoull(std::string{text});
    if (value == 0U || value > 100000U) {
        throw std::runtime_error{"frame count is outside 1..100000"};
    }
    return static_cast<std::size_t>(value);
}

[[nodiscard]] double parse_positive(std::string_view text) {
    const double value = std::stod(std::string{text});
    if (!std::isfinite(value) || !(value > 0.0)) {
        throw std::runtime_error{"expected a finite positive number"};
    }
    return value;
}

[[nodiscard]] Options parse_options(int argc, char** argv) {
    if (argc < 2) {
        throw std::runtime_error{
            "usage: mediaserver_render_media_video_benchmark <video> "
            "[--video PATH]... [--warmup N] [--frames N] "
            "[--cadence-fps FPS] "
            "[--hold-frame] [--radial-plates] [--buffered-output] [--d3d12] "
            "[--half-resolution] "
            "[--second-clip-offset N] [--min-ready-ratio 0..1] "
            "[--max-render-ms MS] [--sequence-frames N] [--stride N] "
            "[--reverse] [--seek-targets N]"};
    }
    Options options{};
    options.paths.emplace_back(argv[1]);
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--hold-frame") {
            options.hold_frame = true;
            continue;
        }
        if (argument == "--radial-plates") {
            options.radial_plates = true;
            continue;
        }
        if (argument == "--buffered-output") {
            options.buffered_output = true;
            continue;
        }
        if (argument == "--d3d12") {
            options.d3d12 = true;
            continue;
        }
        if (argument == "--half-resolution") {
            options.half_resolution = true;
            continue;
        }
        if (argument == "--reverse") {
            options.reverse = true;
            continue;
        }
        if (index + 1 >= argc) {
            throw std::runtime_error{"option is missing its value"};
        }
        const std::string_view value{argv[++index]};
        if (argument == "--video") {
            options.paths.emplace_back(value);
        } else if (argument == "--warmup") {
            options.warmup = parse_count(value);
        } else if (argument == "--frames") {
            options.frames = parse_count(value);
        } else if (argument == "--cadence-fps") {
            options.cadence_fps = parse_positive(value);
        } else if (argument == "--second-clip-offset") {
            options.second_clip_offset = parse_count(value);
        } else if (argument == "--sequence-frames") {
            options.sequence_frames = parse_count(value);
        } else if (argument == "--stride") {
            options.stride = parse_count(value);
        } else if (argument == "--seek-targets") {
            options.seek_targets = parse_count(value);
        } else if (argument == "--min-ready-ratio") {
            options.minimum_ready_ratio = std::stod(std::string{value});
            if (!std::isfinite(options.minimum_ready_ratio)
                || options.minimum_ready_ratio < 0.0
                || options.minimum_ready_ratio > 1.0) {
                throw std::runtime_error{"ready ratio must be within 0..1"};
            }
        } else if (argument == "--max-render-ms") {
            options.maximum_render_ms = parse_positive(value);
        } else {
            throw std::runtime_error{
                std::string{"unknown option: "} + std::string{argument}};
        }
    }
    return options;
}

// Device-local memory this process holds, summed over adapters, in MiB. The
// decoder's own surfaces are allocated by the driver on FFmpeg's behalf, so
// this is the only number that includes the read-ahead held on the device.
[[nodiscard]] double process_vram_mib() {
#if defined(_WIN32)
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(
            __uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        return -1.0;
    }
    UINT64 usage = 0U;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT index = 0U;
         factory->EnumAdapters1(index, &adapter) != DXGI_ERROR_NOT_FOUND;
         ++index) {
        IDXGIAdapter3* queried = nullptr;
        if (SUCCEEDED(adapter->QueryInterface(
                __uuidof(IDXGIAdapter3), reinterpret_cast<void**>(&queried)))) {
            DXGI_QUERY_VIDEO_MEMORY_INFO info{};
            if (SUCCEEDED(queried->QueryVideoMemoryInfo(
                    0U, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
                usage += info.CurrentUsage;
            }
            queried->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return static_cast<double>(usage) / 1048576.0;
#else
    return -1.0;
#endif
}

[[nodiscard]] rhi::Device open_device(bool d3d12) {
    rhi::DeviceConfig config{};
    config.debug = false;
    config.backend = d3d12 ? rhi::BackendKind::D3d12 : rhi::BackendKind::SdlGpu;
    auto created = rhi::Device::create(config);
    if (!created) {
        throw std::runtime_error{"GPU device creation failed"};
    }
    return std::move(created).value();
}

[[nodiscard]] render::RendererLimits benchmark_limits() noexcept {
    render::RendererLimits limits{};
    limits.max_instances = 16U;
    limits.max_vertices = 1U << 14U;
    limits.max_indices = 1U << 15U;
    limits.max_lights = 4U;
    return limits;
}

[[nodiscard]] scene::Scene video_scene(
    const std::vector<std::string>& paths,
    const std::optional<std::size_t>& second_clip_offset,
    bool radial_plates) {
    scene::Scene result{};
    result.canvas = scene::Canvas{160U, 90U};
    result.camera.eye = scene::Vec3{0.0F, 0.0F, 3.0F};
    result.camera.target = scene::Vec3{};
    result.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    result.camera.field_of_view = 0.125F;
    result.camera.near_plane = 0.1F;
    result.camera.far_plane = 100.0F;
    core::u64 node = 1U;
    for (std::size_t path_index = 0U; path_index < paths.size(); ++path_index) {
        const auto& path = paths[path_index];
        scene::MediaInstance clip{};
        clip.mesh = scene::MeshKey{scene::Primitive::Quad, 1U};
        const scene::Mat4 local =
            scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F});
        if (radial_plates && path_index < 8U) {
            const scene::Turns angle = static_cast<float>(
                static_cast<double>(path_index) / 8.0);
            const scene::Vec3 position{
                scene::deterministic::sin_turns(angle) * 4.0F,
                0.0F,
                scene::deterministic::cos_turns(angle) * 4.0F};
            const scene::Mat4 slot = scene::multiply(
                scene::translation(position),
                scene::rotation(
                    scene::wrap_turns(static_cast<double>(angle) + 0.5),
                    0.0F, 0.0F));
            clip.world = scene::multiply(slot, local);
        } else if (radial_plates && path_index == 8U) {
            const scene::Mat4 slot = scene::multiply(
                scene::translation(scene::Vec3{0.0F, 4.0F, 0.0F}),
                scene::rotation(0.0F, 0.25F, 0.0F));
            clip.world = scene::multiply(slot, local);
        } else {
            clip.world = local;
        }
        clip.normal = scene::normal_basis(clip.world);
        clip.source = path;
        clip.source_node = mediaserver::doc::NodeId{node++};
        result.media.push_back(clip);
        if (second_clip_offset.has_value()) {
            clip.source_node = mediaserver::doc::NodeId{node++};
            clip.frame_index = static_cast<core::u32>(*second_clip_offset);
            result.media.push_back(std::move(clip));
        }
    }
    return result;
}

struct Measurements final {
    std::size_t calls{};
    std::size_t ready{};
    std::size_t held{};
    std::size_t blank{};
    core::u64 uploads{};
    core::u64 planar_uploads{};
    double render_ms_total{};
    double render_ms_max{};
    double submit_wait_ms_total{};
    double submit_wait_ms_max{};
    std::size_t peak_gpu_resident_bytes{};
    std::size_t peak_cpu_ready_bytes{};
    double peak_vram_mib{};
};

void record_cache_result(
    const render::SceneRenderer& renderer,
    const render::MediaCacheStats& stats,
    std::size_t expected_media,
    Measurements& measurements) {
    ++measurements.calls;
    const std::size_t accounted_media =
        static_cast<std::size_t>(renderer.stats().media)
        + static_cast<std::size_t>(renderer.stats().culled_media);
    if (stats.held_frames > 0U
        && accounted_media == expected_media) {
        ++measurements.held;
    } else if (stats.pending_requests == 0U
               && accounted_media == expected_media) {
        ++measurements.ready;
    } else {
        ++measurements.blank;
    }
}

// Seek latency: every clip jumps to the same target, and the editor's two
// renders repeat at about 500 Hz until both draw that exact frame. Targets
// step through the file by the golden ratio, so consecutive seeks land far
// apart and in both directions, as a playhead click or fast drag does.
[[nodiscard]] int run_seeks(
    const Options& options, rhi::Device& device,
    render::SceneRenderer& program, render::SceneRenderer& stage,
    scene::Scene& frame, render::MediaCache& cache,
    const std::map<std::string, render::MediaProbe>& source_metadata) {
    std::size_t shortest = std::numeric_limits<std::size_t>::max();
    for (const auto& [path, metadata] : source_metadata) {
        shortest = std::min(
            shortest, static_cast<std::size_t>(metadata.frame_count));
    }
    const auto render_until_exact = [&](core::u32 target) {
        for (auto& clip : frame.media) {
            clip.frame_index = target;
        }
        const auto start = std::chrono::steady_clock::now();
        std::size_t renders = 0U;
        while (true) {
            auto acquired = device.acquire_commands();
            if (!acquired) {
                throw std::runtime_error{"command-list acquisition failed"};
            }
            auto commands = std::move(acquired).value();
            cache.begin_frame();
            const bool program_ok = program.render(
                commands, frame, scene::whole(frame.canvas),
                render::SceneRenderOptions{}, cache);
            const bool stage_ok = stage.render(
                commands, frame, scene::whole(frame.canvas),
                render::SceneRenderOptions{}, cache);
            const render::MediaCacheStats stats = cache.stats();
            if (!program_ok || !stage_ok) {
                throw std::runtime_error{"SceneRenderer record failed"};
            }
            auto submitted = device.submit(std::move(commands));
            if (!submitted || !std::move(submitted).value().wait()) {
                throw std::runtime_error{"GPU submission failed"};
            }
            cache.end_frame();
            ++renders;
            const std::size_t accounted =
                static_cast<std::size_t>(stage.stats().media) +
                static_cast<std::size_t>(stage.stats().culled_media);
            const double elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            if (stats.held_frames == 0U && stats.pending_requests == 0U &&
                accounted == frame.media.size()) {
                return std::pair{elapsed, renders};
            }
            if (elapsed > 10'000.0) {
                return std::pair{-1.0, renders};
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
        }
    };
    // Opens every decoder before anything is timed.
    static_cast<void>(render_until_exact(0U));
    std::vector<double> latencies;
    std::size_t timeouts = 0U;
    std::size_t renders_total = 0U;
    double position = 0.0;
    for (std::size_t seek = 0U; seek < options.seek_targets; ++seek) {
        position = std::fmod(position + 0.6180339887498949, 1.0);
        const auto target = static_cast<core::u32>(
            position * static_cast<double>(shortest - 1U));
        const auto [latency, renders] = render_until_exact(target);
        renders_total += renders;
        if (latency < 0.0) {
            ++timeouts;
        } else {
            latencies.push_back(latency);
        }
    }
    std::sort(latencies.begin(), latencies.end());
    const auto percentile = [&](double fraction) {
        if (latencies.empty()) {
            return 0.0;
        }
        const auto index = static_cast<std::size_t>(
            fraction * static_cast<double>(latencies.size() - 1U) + 0.5);
        return latencies[index];
    };
    double sum = 0.0;
    for (const double latency : latencies) {
        sum += latency;
    }
    const render::MediaProbe& first = source_metadata.at(options.paths.front());
    std::cout << std::fixed << std::setprecision(1)
              << "sources=" << options.paths.size()
              << " backend=" << (options.d3d12 ? "D3D12" : "SDL")
              << " source_size=" << first.width << 'x' << first.height
              << " source_frames=" << first.frame_count
              << " codec=" << first.codec
              << " pixel_format=" << first.pixel_format
              << " simultaneous_clips=" << frame.media.size() << '\n'
              << "seeks=" << options.seek_targets
              << " timeouts=" << timeouts
              << " seek_ms_mean="
              << (latencies.empty()
                      ? 0.0
                      : sum / static_cast<double>(latencies.size()))
              << " seek_ms_p50=" << percentile(0.5)
              << " seek_ms_p95=" << percentile(0.95)
              << " seek_ms_max=" << percentile(1.0)
              << " renders_per_seek="
              << static_cast<double>(renders_total) /
                     static_cast<double>(options.seek_targets)
              << '\n';
    return timeouts == 0U ? 0 : 1;
}

[[nodiscard]] int run(const Options& options) {
#if !defined(NDEBUG)
    if (options.minimum_ready_ratio >= 0.0
        || options.maximum_render_ms >= 0.0) {
        throw std::runtime_error{
            "performance thresholds are accepted only by a Release build"};
    }
    std::cout << "warning: Debug build; measurements are diagnostic only\n";
#endif

    std::map<std::string, render::MediaProbe> source_metadata;
    for (const auto& path : options.paths) {
        render::MediaProbe metadata = render::probe_media(path);
        if (options.sequence_frames > 0U && metadata.frame_count < 2U) {
            metadata.frame_count =
                static_cast<core::u32>(options.sequence_frames);
        }
        if (metadata.frame_count < 2U) {
            throw std::runtime_error{
                "video probe found fewer than two frames: " + path};
        }
        source_metadata[path] = metadata;
    }
    const auto native_rate = source_metadata.at(options.paths.front()).frame_rate;
    const double cadence_fps = options.cadence_fps > 0.0
        ? options.cadence_fps
        : (native_rate.has_value()
               ? static_cast<double>(native_rate->numerator)
                    / static_cast<double>(native_rate->denominator)
               : 24.0);
    auto device = open_device(options.d3d12);
    const double idle_vram_mib = process_vram_mib();
    auto program_created = render::SceneRenderer::create(
        device, 160U, 90U, benchmark_limits());
    auto stage_created = render::SceneRenderer::create(
        device, 160U, 90U, benchmark_limits());
    if (!program_created || !stage_created) {
        throw std::runtime_error{"SceneRenderer creation failed"};
    }
    auto program = std::move(program_created).value();
    auto stage = std::move(stage_created).value();
    auto frame = video_scene(
        options.paths, options.second_clip_offset, options.radial_plates);
    if (options.half_resolution) {
        for (auto& clip : frame.media) {
            clip.resolution = scene::MediaResolution::Half;
        }
    }
    if (options.buffered_output) {
        render::configure_buffered_output_media_decode();
    }
    render::MediaCache shared_media_cache{
        options.buffered_output
            ? render::MediaCacheConfig::buffered_output()
            : render::MediaCacheConfig{}};

    if (options.seek_targets > 0U) {
        return run_seeks(options, device, program, stage, frame,
                         shared_media_cache, source_metadata);
    }

    const render::MediaDecodeStats decode_before = render::media_decode_stats();
    Measurements measured{};
    const std::size_t total = options.warmup + options.frames;
    const auto period = std::chrono::duration<double>{1.0 / cadence_fps};
    auto next_tick = std::chrono::steady_clock::now();
    const auto benchmark_start = next_tick;
    for (std::size_t ordinal = 0U; ordinal < total; ++ordinal) {
        for (std::size_t clip = 0U; clip < frame.media.size(); ++clip) {
            const std::size_t offset =
                !options.second_clip_offset.has_value() || clip % 2U == 0U
                ? 0U
                : *options.second_clip_offset;
            const auto source_frames = static_cast<std::size_t>(
                source_metadata.at(frame.media[clip].source).frame_count);
            const std::size_t walked =
                (options.hold_frame ? offset
                                    : ordinal * options.stride + offset) %
                source_frames;
            frame.media[clip].frame_index = static_cast<core::u32>(
                options.reverse ? source_frames - 1U - walked : walked);
        }
        auto acquired = device.acquire_commands();
        if (!acquired) {
            throw std::runtime_error{"command-list acquisition failed"};
        }
        auto commands = std::move(acquired).value();
        shared_media_cache.begin_frame();

        const auto program_start = std::chrono::steady_clock::now();
        const bool program_ok = program.render(
            commands, frame, scene::whole(frame.canvas),
            render::SceneRenderOptions{}, shared_media_cache);
        const render::MediaCacheStats program_cache_stats =
            shared_media_cache.stats();
        const auto program_end = std::chrono::steady_clock::now();
        const bool stage_ok = stage.render(
            commands, frame, scene::whole(frame.canvas),
            render::SceneRenderOptions{}, shared_media_cache);
        const render::MediaCacheStats stage_cache_stats =
            shared_media_cache.stats();
        const auto stage_end = std::chrono::steady_clock::now();
        if (!program_ok || !stage_ok) {
            throw std::runtime_error{"SceneRenderer record failed"};
        }

        const auto submit_start = std::chrono::steady_clock::now();
        auto submitted = device.submit(std::move(commands));
        if (!submitted || !std::move(submitted).value().wait()) {
            throw std::runtime_error{"GPU submission failed"};
        }
        const auto submit_end = std::chrono::steady_clock::now();
        shared_media_cache.end_frame();

        if (ordinal >= options.warmup) {
            const double program_ms =
                std::chrono::duration<double, std::milli>(
                    program_end - program_start).count();
            const double stage_ms =
                std::chrono::duration<double, std::milli>(
                    stage_end - program_end).count();
            measured.render_ms_total += program_ms + stage_ms;
            measured.render_ms_max = std::max(
                measured.render_ms_max, std::max(program_ms, stage_ms));
            const double submit_ms =
                std::chrono::duration<double, std::milli>(
                    submit_end - submit_start).count();
            measured.submit_wait_ms_total += submit_ms;
            measured.submit_wait_ms_max =
                std::max(measured.submit_wait_ms_max, submit_ms);
            record_cache_result(
                program, program_cache_stats, frame.media.size(), measured);
            record_cache_result(
                stage, stage_cache_stats, frame.media.size(), measured);
            measured.uploads += stage_cache_stats.uploads;
            measured.planar_uploads += stage_cache_stats.planar_uploads;
            measured.peak_gpu_resident_bytes = std::max(
                measured.peak_gpu_resident_bytes,
                shared_media_cache.stats().resident_bytes);
            measured.peak_cpu_ready_bytes = std::max(
                measured.peak_cpu_ready_bytes,
                render::media_decode_stats().ready_bytes);
            measured.peak_vram_mib =
                std::max(measured.peak_vram_mib, process_vram_mib());
        }

        next_tick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            period);
        std::this_thread::sleep_until(next_tick);
    }
    const auto benchmark_end = std::chrono::steady_clock::now();
    const render::MediaDecodeStats decode_after = render::media_decode_stats();

    const double ready_ratio = measured.calls == 0U
        ? 0.0
        : static_cast<double>(measured.ready)
            / static_cast<double>(measured.calls);
    const double held_ratio = measured.calls == 0U
        ? 0.0
        : static_cast<double>(measured.held)
            / static_cast<double>(measured.calls);
    const double mean_render_ms = measured.calls == 0U
        ? 0.0
        : measured.render_ms_total / static_cast<double>(measured.calls);
    const double mean_submit_ms = options.frames == 0U
        ? 0.0
        : measured.submit_wait_ms_total / static_cast<double>(options.frames);
    const double elapsed_seconds =
        std::chrono::duration<double>(benchmark_end - benchmark_start).count();
    const render::MediaProbe& first_metadata =
        source_metadata.at(options.paths.front());

    std::cout << std::fixed << std::setprecision(3)
              << "sources=" << options.paths.size()
              << " backend=" << (options.d3d12 ? "D3D12" : "SDL")
              << " source_size=" << first_metadata.width << 'x'
              << first_metadata.height
              << " source_frames=" << first_metadata.frame_count
              << " codec=" << first_metadata.codec
              << " profile=" << first_metadata.profile
              << " pixel_format=" << first_metadata.pixel_format
              << " bits=" << first_metadata.bits_per_raw_sample
              << " cadence_fps=" << cadence_fps
              << " hold_frame=" << (options.hold_frame ? 1 : 0)
              << " radial_plates=" << (options.radial_plates ? 1 : 0)
              << " buffered_output=" << (options.buffered_output ? 1 : 0)
              << " half_resolution=" << (options.half_resolution ? 1 : 0)
              << " warmup=" << options.warmup
              << " measured_frames=" << options.frames
              << " simultaneous_clips=" << frame.media.size() << '\n'
              << "renderer_calls=" << measured.calls
              << " exact_ready=" << measured.ready
              << " held=" << measured.held
              << " blank=" << measured.blank
              << " ready_ratio=" << ready_ratio
              << " held_ratio=" << held_ratio << '\n'
              << "render_call_ms_mean=" << mean_render_ms
              << " render_call_ms_max=" << measured.render_ms_max << '\n'
              << "submit_wait_ms_mean=" << mean_submit_ms
              << " submit_wait_ms_max=" << measured.submit_wait_ms_max << '\n'
              << "gpu_uploads=" << measured.uploads
              << " planar_gpu_uploads=" << measured.planar_uploads
              << " peak_gpu_resident_bytes="
              << measured.peak_gpu_resident_bytes << '\n'
              << "decode_completed="
              << decode_after.completed_frames - decode_before.completed_frames
              << " hardware_sources=" << decode_after.hardware_sources
              << " hardware_configured_sources="
              << decode_after.hardware_configured_sources
              << " decode_failed="
              << decode_after.failed_frames - decode_before.failed_frames
              << " cpu_ready_frames=" << decode_after.ready_frames
              << " peak_cpu_ready_bytes=" << measured.peak_cpu_ready_bytes
              << " cpu_byte_budget=" << decode_after.byte_budget << '\n'
              << "idle_vram_mib=" << idle_vram_mib
              << " peak_vram_mib=" << measured.peak_vram_mib
              << " peak_vram_growth_mib="
              << measured.peak_vram_mib - idle_vram_mib << '\n'
              << "wall_seconds=" << elapsed_seconds << '\n';

    bool passed = true;
    if (options.minimum_ready_ratio >= 0.0
        && ready_ratio < options.minimum_ready_ratio) {
        std::cerr << "threshold failed: ready ratio " << ready_ratio
                  << " < " << options.minimum_ready_ratio << '\n';
        passed = false;
    }
    if (options.maximum_render_ms >= 0.0
        && measured.render_ms_max > options.maximum_render_ms) {
        std::cerr << "threshold failed: maximum render call "
                  << measured.render_ms_max << " ms > "
                  << options.maximum_render_ms << " ms\n";
        passed = false;
    }
    return passed ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(parse_options(argc, argv));
    } catch (const std::exception& error) {
        std::cerr << "benchmark error: " << error.what() << '\n';
        return 2;
    }
}
