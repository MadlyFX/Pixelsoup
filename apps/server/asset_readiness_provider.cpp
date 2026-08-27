#include "asset_readiness_provider.hpp"

#include <mediaserver/assets/asset_path.hpp>

namespace mediaserver::app {

net::ClusterAssetReadinessState PathAssetReadinessProvider::probe_state(
    std::string_view source) const noexcept {
    return assets::asset_source_present(source)
        ? net::ClusterAssetReadinessState::Ready
        : net::ClusterAssetReadinessState::Missing;
}

std::optional<net::ClusterAssetReadinessState>
PathAssetReadinessProvider::probe(std::string_view source,
                                  std::stop_token stop) noexcept {
    // One stat, so cancellation is only ever observed before the call. There
    // is no long-running work here to interrupt partway through.
    if (stop.stop_requested() ||
        source.size() > net::max_cluster_asset_source_bytes) {
        return std::nullopt;
    }
    // A malformed source is a broken request, not an observation about a real
    // file. Answering Missing here would read as "this machine looked and the
    // file was not there", which is a different and misleading claim.
    if (!assets::valid_asset_source(source)) {
        return std::nullopt;
    }
    return probe_state(source);
}

} // namespace mediaserver::app
