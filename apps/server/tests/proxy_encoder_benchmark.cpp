#include "ffmpeg_proxy_encoder.hpp"

#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/assets/derivative_cache.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    namespace assets = mediaserver::assets;
    namespace doc = mediaserver::doc;
    namespace server = mediaserver::server;
    if (argc < 3 || argc > 5) {
        std::cerr << "usage: mediaserver_proxy_encoder_benchmark SOURCE CACHE "
                     "[MATERIALIZED_MP4] [PROFILE]\n";
        return 2;
    }
    const std::string source{argv[1]};
    const auto fingerprint = assets::source_fingerprint(source);
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
    if (argc == 5) {
        const assets::DerivativeProfile* const selected =
            assets::derivative_profile(argv[4]);
        if (selected == nullptr ||
            selected->kind != assets::DerivativeKind::Proxy) {
            std::cerr << "unknown proxy profile\n";
            return 2;
        }
        profile = selected->id;
    }
    const assets::MediaAsset media{
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
    server::ProxyGenerationService service{
        std::move(cache).value(), server::ffmpeg_proxy_encoder_backend()};
    const auto result = service.generate(media);
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
    if (argc >= 4) {
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
    std::cout << "proxy=" << result.value().encoded_metadata.dimensions.width
              << 'x' << result.value().encoded_metadata.dimensions.height
              << " frames=" << result.value().encoded_metadata.frame_count
              << " cache_hit=" << (result.value().cache_hit ? 1 : 0)
              << " cache=" << result.value().cache_entry.path.string()
              << '\n';
    return 0;
}
