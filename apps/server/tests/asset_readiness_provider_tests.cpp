#include "asset_readiness_provider.hpp"

#include <mediaserver/assets/asset_transfer.hpp>
#include <mediaserver/assets/content_identity.hpp>
#include <mediaserver/assets/content_resolver.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stop_token>
#include <string_view>

namespace mediaserver {
namespace {

#define READINESS_CHECK(condition)                                           \
    do {                                                                     \
        if (!(condition)) {                                                  \
            std::cerr << "check failed at line " << __LINE__ << ": "       \
                      << #condition << '\n';                                 \
            return false;                                                    \
        }                                                                    \
    } while (false)

struct TemporaryDirectory final {
    std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        "mediaserver-asset-readiness-provider-tests";

    TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
};

[[nodiscard]] bool writes_and_detects_verified_content() {
    TemporaryDirectory temporary;
    const auto source = temporary.path / "source.mov";
    {
        std::ofstream stream{source, std::ios::binary};
        stream << "portable-content";
    }
    auto hashed = assets::hash_file(source);
    READINESS_CHECK(hashed);
    net::ClusterAssetIdentity identity{hashed.value().sha256,
                                       hashed.value().byte_size};
    app::SharedStoreAssetReadinessProvider provider{temporary.path / "store"};
    std::stop_source cancelled;
    READINESS_CHECK(cancelled.request_stop());
    READINESS_CHECK(!provider.probe(identity, cancelled.get_token()));
    READINESS_CHECK(provider.probe(identity) ==
                    net::ClusterAssetReadinessState::Missing);

    auto source_handle = assets::VerifiedAssetSource::open_registered(
        source, hashed.value());
    READINESS_CHECK(source_handle);
    auto sink = assets::ResumableAssetSink::open(
        temporary.path / "store", hashed.value(), source.string());
    READINESS_CHECK(sink);
    auto chunk = source_handle.value().read_chunk(
        0U, static_cast<std::size_t>(hashed.value().byte_size));
    READINESS_CHECK(chunk);
    READINESS_CHECK(sink.value().append(chunk.value().offset,
                                        chunk.value().bytes));
    READINESS_CHECK(sink.value().finalize());
    READINESS_CHECK(provider.probe(identity) ==
                    net::ClusterAssetReadinessState::Ready);

    const auto blob = temporary.path / "store" /
        assets::shared_store_blob_relative_path(hashed.value());
    const auto original_time = std::filesystem::last_write_time(blob);
    {
        std::ofstream stream{blob, std::ios::binary | std::ios::trunc};
        stream << "corrupt-content!";
    }
    READINESS_CHECK(std::filesystem::file_size(blob) ==
                    hashed.value().byte_size);
    std::filesystem::last_write_time(blob, original_time);
    READINESS_CHECK(provider.probe(identity) ==
                    net::ClusterAssetReadinessState::Corrupt);
    std::filesystem::remove(blob);
    std::filesystem::create_directory(blob);
    READINESS_CHECK(provider.probe(identity) ==
                    net::ClusterAssetReadinessState::Corrupt);
    return true;
}

} // namespace
} // namespace mediaserver

int main() {
    return mediaserver::writes_and_detects_verified_content() ? 0 : 1;
}
