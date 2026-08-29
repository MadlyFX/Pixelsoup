#pragma once

#include <mediaserver/net/cluster_lifecycle.hpp>

namespace mediaserver::app {

// Machine-local lifecycle-agent answer to "does this machine have these exact
// bytes at the authored path".
//
// The agent can answer while the render child is stopped, which is what lets
// Output Monitor show a missing file before anyone presses START. It reports
// It never returns a directory listing, a rewritten path, or contents.
class PathAssetReadinessProvider final
    : public net::ClusterAssetReadinessProvider {
public:
    [[nodiscard]] net::ClusterAssetReadinessState
    probe_state(std::string_view source) const noexcept;

    [[nodiscard]] std::optional<net::ClusterAssetReadinessState>
    probe(const net::ClusterAssetRequirement& requirement,
          std::stop_token stop = {}) noexcept override;
};

} // namespace mediaserver::app
