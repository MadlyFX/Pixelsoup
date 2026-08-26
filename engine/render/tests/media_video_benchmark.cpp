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
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

// Optional, real-file Release benchmark. It records the production
// SceneRenderer path twice per command list (Program + Stage), so CPU decode is
// shared while GPU texture/upload residency remains intentionally per renderer.
// No threshold is enforced unless explicitly requested on the command line.
namespace {

namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;

struct Options final {
    std::string path;
    std::size_t warmup{8U};
    std::size_t frames{48U};
    double cadence_fps{};
    double minimum_ready_ratio{-1.0};
    double maximum_render_ms{-1.0};
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
            "[--warmup N] [--frames N] [--cadence-fps FPS] "
            "[--min-ready-ratio 0..1] [--max-render-ms MS]"};
    }
    Options options{};
    options.path = argv[1];
    for (int index = 2; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (index + 1 >= argc) {
            throw std::runtime_error{"option is missing its value"};
        }
        const std::string_view value{argv[++index]};
        if (argument == "--warmup") {
            options.warmup = parse_count(value);
        } else if (argument == "--frames") {
            options.frames = parse_count(value);
        } else if (argument == "--cadence-fps") {
            options.cadence_fps = parse_positive(value);
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

[[nodiscard]] rhi::Device open_device() {
    rhi::DeviceConfig config{};
    config.debug = false;
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

[[nodiscard]] scene::Scene video_scene(const std::string& path) {
    scene::Scene result{};
    result.canvas = scene::Canvas{160U, 90U};
    result.camera.eye = scene::Vec3{0.0F, 0.0F, 3.0F};
    result.camera.target = scene::Vec3{};
    result.camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    result.camera.field_of_view = 0.125F;
    result.camera.near_plane = 0.1F;
    result.camera.far_plane = 100.0F;
    scene::MediaInstance clip{};
    clip.mesh = scene::MeshKey{scene::Primitive::Quad, 1U};
    clip.world = scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F});
    clip.normal = scene::normal_basis(clip.world);
    clip.source = path;
    clip.source_node = mediaserver::doc::NodeId{1U};
    result.media.push_back(std::move(clip));
    return result;
}

struct Measurements final {
    std::size_t calls{};
    std::size_t ready{};
    std::size_t held{};
    std::size_t blank{};
    core::u64 uploads{};
    double render_ms_total{};
    double render_ms_max{};
    double submit_wait_ms_total{};
    double submit_wait_ms_max{};
    std::size_t peak_gpu_resident_bytes{};
    std::size_t peak_cpu_ready_bytes{};
};

void record_cache_result(
    const render::SceneRenderer& renderer, Measurements& measurements) {
    const render::MediaCacheStats& stats = renderer.media_cache_stats();
    ++measurements.calls;
    measurements.uploads += stats.uploads;
    if (stats.held_frames > 0U && renderer.stats().media == 1U) {
        ++measurements.held;
    } else if (stats.pending_requests == 0U
               && renderer.stats().media == 1U) {
        ++measurements.ready;
    } else {
        ++measurements.blank;
    }
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

    const core::u32 source_frames = render::probe_frame_count(options.path);
    const auto native_rate = render::probe_media_frame_rate(options.path);
    const double cadence_fps = options.cadence_fps > 0.0
        ? options.cadence_fps
        : (native_rate.has_value()
               ? static_cast<double>(native_rate->numerator)
                    / static_cast<double>(native_rate->denominator)
               : 24.0);
    if (source_frames < 2U) {
        throw std::runtime_error{"video probe found fewer than two frames"};
    }

    auto device = open_device();
    auto program_created = render::SceneRenderer::create(
        device, 160U, 90U, benchmark_limits());
    auto stage_created = render::SceneRenderer::create(
        device, 160U, 90U, benchmark_limits());
    if (!program_created || !stage_created) {
        throw std::runtime_error{"SceneRenderer creation failed"};
    }
    auto program = std::move(program_created).value();
    auto stage = std::move(stage_created).value();
    auto frame = video_scene(options.path);

    const render::MediaDecodeStats decode_before = render::media_decode_stats();
    Measurements measured{};
    const std::size_t total = options.warmup + options.frames;
    const auto period = std::chrono::duration<double>{1.0 / cadence_fps};
    auto next_tick = std::chrono::steady_clock::now();
    const auto benchmark_start = next_tick;
    for (std::size_t ordinal = 0U; ordinal < total; ++ordinal) {
        frame.media.front().frame_index = static_cast<core::u32>(
            ordinal % static_cast<std::size_t>(source_frames));
        auto acquired = device.acquire_commands();
        if (!acquired) {
            throw std::runtime_error{"command-list acquisition failed"};
        }
        auto commands = std::move(acquired).value();

        const auto program_start = std::chrono::steady_clock::now();
        const bool program_ok = program.render(
            commands, frame, scene::whole(frame.canvas));
        const auto program_end = std::chrono::steady_clock::now();
        const bool stage_ok = stage.render(
            commands, frame, scene::whole(frame.canvas));
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
            record_cache_result(program, measured);
            record_cache_result(stage, measured);
            measured.peak_gpu_resident_bytes = std::max(
                measured.peak_gpu_resident_bytes,
                program.media_cache_stats().resident_bytes
                    + stage.media_cache_stats().resident_bytes);
            measured.peak_cpu_ready_bytes = std::max(
                measured.peak_cpu_ready_bytes,
                render::media_decode_stats().ready_bytes);
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

    std::cout << std::fixed << std::setprecision(3)
              << "path=" << options.path << '\n'
              << "source_frames=" << source_frames
              << " cadence_fps=" << cadence_fps
              << " warmup=" << options.warmup
              << " measured_frames=" << options.frames << '\n'
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
              << " peak_gpu_resident_bytes="
              << measured.peak_gpu_resident_bytes << '\n'
              << "decode_completed="
              << decode_after.completed_frames - decode_before.completed_frames
              << " decode_failed="
              << decode_after.failed_frames - decode_before.failed_frames
              << " cpu_ready_frames=" << decode_after.ready_frames
              << " peak_cpu_ready_bytes=" << measured.peak_cpu_ready_bytes
              << " cpu_byte_budget=" << decode_after.byte_budget << '\n'
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
