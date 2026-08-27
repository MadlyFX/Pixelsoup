#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/document.hpp>

#include <string>
#include <vector>

namespace mediaserver::app {

// One file the current document needs open on every render machine, derived
// from scene.media, scene.model, and imported output.surface references.
//
// Entries are deduplicated by (kind, source) and sorted the same way, so the
// Director and every node compute an identical manifest from the same
// document. Library assets nothing currently uses are not included, so an
// operator can keep unused imports without a node reporting them missing.
struct RequiredProjectAsset final {
    // The Library record this reference resolved to, or zero when a placement
    // carries a source with no Library row. Both are opened identically; the
    // record only supplies the display name and probe metadata.
    doc::NodeId asset{};
    assets::AssetKind kind{assets::AssetKind::Media};
    // Absolute path, the same on every machine in the show.
    std::string source;

    bool operator==(const RequiredProjectAsset&) const = default;
};

struct ProjectAssetManifest final {
    std::vector<RequiredProjectAsset> assets;
    // Placements whose asset_id names a Library record of a different kind -
    // an authoring error, not a distribution one.
    core::u32 kind_mismatches{};
    // Deterministic key over every required (kind, source). Playback uses it
    // to skip rechecking the filesystem after unrelated document edits, and
    // nodes report it back so the Director knows which manifest a readiness
    // answer refers to.
    core::u64 digest{};

    [[nodiscard]] bool empty() const noexcept {
        return assets.empty() && kind_mismatches == 0U;
    }

    bool operator==(const ProjectAssetManifest&) const = default;
};

[[nodiscard]] ProjectAssetManifest collect_project_asset_manifest(
    const doc::Root& root, const assets::AssetRegistry& registry);

struct ProjectAssetPresence final {
    doc::NodeId asset{};
    std::string source;
    bool present{};

    bool operator==(const ProjectAssetPresence&) const = default;
};

// What one machine found when it looked. `digest` binds the answer to the
// exact manifest it was asked about, so a stale report can never be read as
// readiness for a newer one.
struct ProjectAssetAvailability final {
    core::u64 digest{};
    std::vector<ProjectAssetPresence> assets;
    core::u32 missing{};

    [[nodiscard]] bool ready() const noexcept { return missing == 0U; }

    bool operator==(const ProjectAssetAvailability&) const = default;
};

// Stats every required source on this machine. One stat per entry and no
// reads, so this is cheap enough to run whenever the manifest changes rather
// than on a background cadence.
[[nodiscard]] ProjectAssetAvailability check_project_assets(
    const ProjectAssetManifest& manifest);

} // namespace mediaserver::app
