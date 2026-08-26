#pragma once

#include <mediaserver/net/cluster_lifecycle.hpp>

#include <filesystem>

namespace mediaserver::app {

// Machine-local lifecycle-agent view of the verified shared store. Requests
// contain only digest+size identities; no authored or local path crosses the
// network. Every probe re-hashes the canonical blob: content-addressed names
// are not treated as proof that bytes were not modified out of band.
class SharedStoreAssetReadinessProvider final
    : public net::ClusterAssetReadinessProvider {
public:
    explicit SharedStoreAssetReadinessProvider(
        std::filesystem::path store_root) noexcept;

    [[nodiscard]] std::optional<net::ClusterAssetReadinessState>
    probe(const net::ClusterAssetIdentity& identity,
          std::stop_token stop = {}) noexcept override;

private:
    std::filesystem::path store_root_;
};

} // namespace mediaserver::app
