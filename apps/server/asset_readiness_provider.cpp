#include "asset_readiness_provider.hpp"

#include <mediaserver/assets/content_identity.hpp>
#include <mediaserver/assets/content_resolver.hpp>

#include <system_error>
#include <utility>

namespace mediaserver::app {
namespace {

[[nodiscard]] assets::ContentIdentity content_identity(
    const net::ClusterAssetIdentity& identity) noexcept {
    assets::ContentIdentity result{};
    result.sha256 = identity.sha256;
    result.byte_size = identity.byte_size;
    return result;
}

} // namespace

SharedStoreAssetReadinessProvider::SharedStoreAssetReadinessProvider(
    std::filesystem::path store_root) noexcept
    : store_root_(std::move(store_root)) {}

std::optional<net::ClusterAssetReadinessState>
SharedStoreAssetReadinessProvider::probe(
    const net::ClusterAssetIdentity& wire_identity,
    std::stop_token stop) noexcept {
    using State = net::ClusterAssetReadinessState;
    try {
        if (stop.stop_requested() || store_root_.empty() ||
            wire_identity.byte_size == 0U ||
            wire_identity.byte_size > net::max_cluster_asset_bytes) {
            return std::nullopt;
        }
        const assets::ContentIdentity identity =
            content_identity(wire_identity);
        const auto relative =
            assets::shared_store_blob_relative_path(identity);
        if (relative.empty()) {
            return std::nullopt;
        }
        const std::filesystem::path path = store_root_ / relative;
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path, error);
        if (stop.stop_requested()) {
            return std::nullopt;
        }
        if (error) {
            if (error == std::errc::no_such_file_or_directory ||
                error == std::errc::not_a_directory) {
                return State::Missing;
            }
            return std::nullopt;
        }
        if (!std::filesystem::is_regular_file(status)) {
            return State::Corrupt;
        }
        State state = State::Corrupt;
        const auto verified = assets::verify_file(path, identity, stop);
        if (stop.stop_requested()) {
            return std::nullopt;
        }
        if (!verified) {
            return std::nullopt;
        }
        if (verified.value() == assets::ContentVerification::Verified) {
            state = State::Ready;
        }
        return state;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace mediaserver::app
