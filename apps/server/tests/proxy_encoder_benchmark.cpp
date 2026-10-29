#include "ffmpeg_proxy_encoder.hpp"

#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/assets/derivative_cache.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    namespace assets = mediaserver::assets;
    namespace doc = mediaserver::doc;
    namespace server = mediaserver::server;
    using Clock = std::chrono::steady_clock;
    const auto ms = [](Clock::time_point from, Clock::time_point to) {
        return std::chrono::duration<double, std::milli>(to - from).count();
    };
    // `segments FRAMES NUM/DEN` encodes the proxy one segment per encode,
    // and `run FRAMES NUM/DEN` all segments in one encode as the editor
    // does, then joins them; the source's frame count and rate stand in for
    // the probe import records.
    if (argc < 3 || (argc > 6 && argc != 9)) {
        std::cerr << "usage: mediaserver_proxy_encoder_benchmark SOURCE CACHE "
                     "[MATERIALIZED_MP4|-] [PROFILE] "
                     "[background|balanced|fast] [segments|run FRAMES "
                     "NUM/DEN]\n";
        return 2;
    }
    server::ProxyTranscodePriority priority =
        server::ProxyTranscodePriority::Background;
    if (argc >= 6) {
        const std::string name{argv[5]};
        if (name == "balanced") {
            priority = server::ProxyTranscodePriority::Balanced;
        } else if (name == "fast") {
            priority = server::ProxyTranscodePriority::Fast;
        } else if (name != "background") {
            std::cerr << "unknown priority\n";
            return 2;
        }
    }
    const std::string source{argv[1]};
    // The import's own fingerprint pass, which generation waits for.
    const auto fingerprint_start = Clock::now();
    const auto fingerprint = assets::source_fingerprint(source);
    const auto fingerprint_end = Clock::now();
    if (!fingerprint) {
        std::cerr << "source fingerprint failed\n";
        return 2;
    }
    const std::filesystem::path cache_root =
        std::filesystem::absolute(argv[2]);
    auto cache = assets::DerivativeCache::open(
        assets::DerivativeCacheConfig{
            cache_root, 8ULL * 1'024ULL * 1'024ULL * 1'024ULL});
    if (!cache) {
        std::cerr << "cache open failed\n";
        return 2;
    }
    assets::DerivativeProfileId profile =
        assets::DerivativeProfileId::Proxy720pCfrH264;
    if (argc >= 5) {
        const assets::DerivativeProfile* const selected =
            assets::derivative_profile(argv[4]);
        if (selected == nullptr ||
            selected->kind != assets::DerivativeKind::Proxy) {
            std::cerr << "unknown proxy profile\n";
            return 2;
        }
        profile = selected->id;
    }
    assets::MediaAsset media{
        doc::NodeId{1U},
        "proxy benchmark",
        source,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        fingerprint.value(),
        profile,
        assets::ProxyPlaybackPolicy::PreferProxy,
    };
    const bool in_run = argc == 9 && std::string{argv[6]} == "run";
    const bool segmented =
        in_run || (argc == 9 && std::string{argv[6]} == "segments");
    if (argc == 9 && !segmented) {
        std::cerr << "unknown mode\n";
        return 2;
    }
    if (segmented) {
        const std::string rate{argv[8]};
        const auto slash = rate.find('/');
        if (slash == std::string::npos) {
            std::cerr << "rate must be NUM/DEN\n";
            return 2;
        }
        media.frame_count =
            static_cast<mediaserver::core::u32>(std::stoul(argv[7]));
        media.frame_rate = assets::MediaFrameRate{
            static_cast<mediaserver::core::u32>(
                std::stoul(rate.substr(0U, slash))),
            static_cast<mediaserver::core::u32>(
                std::stoul(rate.substr(slash + 1U)))};
    }
    server::ProxyGenerationService service{
        std::move(cache).value(),
        server::ffmpeg_proxy_encoder_backend(priority)};
    // The progress callback brackets the encode: its first call follows the
    // source verification pass and decoder open, its last the final frame.
    Clock::time_point first_progress{};
    Clock::time_point last_progress{};
    mediaserver::core::u64 progressed_frames{};
    const auto generate_start = Clock::now();
    using Step = server::ProxyGenerationService::Step;
    if (in_run) {
        const auto count = assets::proxy_segment_count(*media.frame_count,
                                                       *media.frame_rate);
        auto previous = generate_start;
        const auto made = service.generate(
            media, {}, {},
            Step{false, 0U, false, count,
                 [&](mediaserver::core::u32 segment,
                     const server::ProxyGenerationReceipt& receipt) {
                     const auto now = Clock::now();
                     if (segment <= 2U || segment + 1U == count) {
                         std::cout << std::fixed << std::setprecision(1)
                                   << "segment=" << segment << " frames="
                                   << receipt.encoded_metadata.frame_count
                                   << " ms=" << ms(previous, now)
                                   << " since_start_ms="
                                   << ms(generate_start, now) << '\n';
                     }
                     previous = now;
                     return true;
                 }});
        if (!made) {
            std::cerr << "run failed code="
                      << static_cast<unsigned>(made.error().code) << '\n';
            return 1;
        }
        std::cout << "segments=" << count << " all_segments_ms="
                  << ms(generate_start, Clock::now()) << '\n';
    } else if (segmented) {
        const auto count = assets::proxy_segment_count(*media.frame_count,
                                                       *media.frame_rate);
        for (mediaserver::core::u32 segment = 0U; segment < count;
             ++segment) {
            const auto segment_start = Clock::now();
            Clock::time_point segment_first{};
            Clock::time_point segment_last{};
            const auto made = service.generate(
                media, {},
                [&](mediaserver::core::u64, mediaserver::core::u64) {
                    const auto now = Clock::now();
                    if (segment_first == Clock::time_point{}) {
                        segment_first = now;
                    }
                    segment_last = now;
                },
                Step{false, segment});
            if (!made) {
                std::cerr << "segment " << segment << " failed code="
                          << static_cast<unsigned>(made.error().code)
                          << '\n';
                return 1;
            }
            if (segment <= 2U || segment + 1U == count) {
                std::cout << std::fixed << std::setprecision(1) << "segment="
                          << segment << " frames="
                          << made.value().encoded_metadata.frame_count
                          << " ms=" << ms(segment_start, Clock::now())
                          << " before_encode_ms="
                          << ms(segment_start, segment_first)
                          << " encode_ms=" << ms(segment_first, segment_last)
                          << " after_encode_ms="
                          << ms(segment_last, Clock::now())
                          << " since_start_ms="
                          << ms(generate_start, Clock::now()) << '\n';
            }
        }
        std::cout << "segments=" << count << " all_segments_ms="
                  << ms(generate_start, Clock::now()) << '\n';
    }
    const auto join_start = Clock::now();
    const auto result = service.generate(
        media, {},
        [&](mediaserver::core::u64 completed, mediaserver::core::u64) {
            const auto now = Clock::now();
            if (first_progress == Clock::time_point{}) {
                first_progress = now;
            }
            last_progress = now;
            progressed_frames = completed;
        },
        Step{false, std::nullopt, segmented});
    const auto generate_end = Clock::now();
    if (segmented) {
        std::cout << "join_ms=" << ms(join_start, generate_end) << '\n';
    }
    if (!result) {
        std::cerr << "proxy generation failed code="
                  << static_cast<unsigned>(result.error().code);
        if (result.error().backend_error.has_value()) {
            std::cerr << " backend=" << static_cast<unsigned>(
                result.error().backend_error->code);
        }
        std::cerr << '\n';
        return 1;
    }
    const auto materialize_start = Clock::now();
    if (argc >= 4 && std::string{argv[3]} != "-") {
        const auto loaded = service.load(result.value().key);
        if (!loaded) {
            std::cerr << "proxy load failed\n";
            return 1;
        }
        std::ofstream output{
            std::filesystem::path{argv[3]},
            std::ios::binary | std::ios::trunc};
        output.write(
            reinterpret_cast<const char*>(
                loaded.value().encoded_payload.data()),
            static_cast<std::streamsize>(
                loaded.value().encoded_payload.size()));
        output.close();
        if (!output) {
            std::cerr << "proxy materialization failed\n";
            return 1;
        }
    }
    const auto materialize_end = Clock::now();
    const bool encoded = first_progress != Clock::time_point{};
    const double encode_ms =
        encoded ? ms(first_progress, last_progress) : 0.0;
    std::cout << std::fixed << std::setprecision(1)
              << "import_fingerprint_ms="
              << ms(fingerprint_start, fingerprint_end)
              << " generate_ms=" << ms(generate_start, generate_end)
              << " before_encode_ms="
              << (encoded ? ms(generate_start, first_progress) : 0.0)
              << " encode_ms=" << encode_ms
              << " after_encode_ms="
              << (encoded ? ms(last_progress, generate_end) : 0.0)
              << " materialize_ms=" << ms(materialize_start, materialize_end)
              << " encode_fps="
              << (encode_ms > 0.0
                      ? static_cast<double>(progressed_frames) * 1000.0 /
                            encode_ms
                      : 0.0)
              << '\n';
    std::cout << "proxy=" << result.value().encoded_metadata.dimensions.width
              << 'x' << result.value().encoded_metadata.dimensions.height
              << " frames=" << result.value().encoded_metadata.frame_count
              << " cache_hit=" << (result.value().cache_hit ? 1 : 0)
              << " cache=" << result.value().cache_entry.path.string()
              << '\n';
    return 0;
}
