#include "asset_readiness_provider.hpp"

#include <mediaserver/assets/asset_path.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>

namespace mediaserver::app {

net::ClusterAssetReadinessState PathAssetReadinessProvider::probe_state(
    std::string_view source) const noexcept {
    return assets::asset_source_present(source)
        ? net::ClusterAssetReadinessState::Ready
        : net::ClusterAssetReadinessState::Missing;
}

std::optional<net::ClusterAssetReadinessState>
PathAssetReadinessProvider::probe(
    const net::ClusterAssetRequirement& requirement,
                                  std::stop_token stop) noexcept {
    if (stop.stop_requested() ||
        requirement.source.size() > net::max_cluster_asset_source_bytes) {
        return std::nullopt;
    }
    // A malformed source is a broken request, not an observation about a real
    // file. Answering Missing here would read as "this machine looked and the
    // file was not there", which is a different and misleading claim.
    if (!assets::valid_asset_source(requirement.source)) {
        return std::nullopt;
    }
    if (!requirement.fingerprinted) {
        return probe_state(requirement.source);
    }

    auto observed = assets::source_fingerprint(requirement.source, stop);
    if (!observed) {
        using Code = assets::ContentFingerprintErrorCode;
        switch (observed.error().code) {
        case Code::SourceMissing:
            return net::ClusterAssetReadinessState::Missing;
        case Code::Cancelled:
            return std::nullopt;
        case Code::InvalidSource:
        case Code::OpenFailed:
        case Code::ReadFailed:
        case Code::SourceChanged:
        case Code::SizeLimitExceeded:
        case Code::AllocationFailed:
            return net::ClusterAssetReadinessState::VerificationFailed;
        }
        return net::ClusterAssetReadinessState::VerificationFailed;
    }
    if (observed.value().byte_length != requirement.expected_byte_length) {
        return net::ClusterAssetReadinessState::ByteLengthMismatch;
    }
    if (observed.value().sha256.bytes != requirement.expected_sha256) {
        return net::ClusterAssetReadinessState::Sha256Mismatch;
    }
    return net::ClusterAssetReadinessState::Ready;
}

} // namespace mediaserver::app
