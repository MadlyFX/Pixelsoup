#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/assets/content_resolver.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/scene/scene.hpp>

#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace mediaserver::app {

// One manifest entry required by a scene placement or imported projection
// surface. Library assets that are not used by the scheduled document are not
// included, so an operator can retain unused imports without blocking Program.
struct RequiredProjectAsset final {
    doc::NodeId asset{};
    assets::AssetKind kind{assets::AssetKind::Media};
    assets::AssetContent content;
    std::string logical_source;

    bool operator==(const RequiredProjectAsset&) const = default;
};

struct ProjectAssetManifest final {
    // Stable asset-node order, one entry per required Library asset.
    std::vector<RequiredProjectAsset> assets;
    core::u32 unregistered_references{};
    core::u32 kind_mismatches{};
    core::u32 unverified_assets{};
    core::u32 dependency_manifests_required{};
    core::u32 non_shared_assets{};
    // Deterministic key over required asset identities plus every exact
    // placement/source association. Playback uses it to avoid re-hashing
    // verified files after unrelated document edits.
    core::u64 digest{};

    [[nodiscard]] bool empty() const noexcept {
        return assets.empty() && unregistered_references == 0U &&
            kind_mismatches == 0U;
    }
    [[nodiscard]] bool shared_store_ready() const noexcept {
        return unregistered_references == 0U && kind_mismatches == 0U &&
            unverified_assets == 0U &&
            dependency_manifests_required == 0U && non_shared_assets == 0U;
    }

    bool operator==(const ProjectAssetManifest&) const = default;
};

// Derives only the assets required by the current document: scene.media,
// scene.model, and imported output.surface references. A placement's asset_id
// wins when valid; exact-source lookup is the legacy migration fallback.
[[nodiscard]] ProjectAssetManifest collect_project_asset_manifest(
    const doc::Root& root, const assets::AssetRegistry& registry);

enum class LocalAssetState : core::u8 {
    Checking,
    Verified,
    Missing,
    Corrupt,
    Unverified,
    Error,
};

struct LocalAssetObservation final {
    doc::NodeId asset{};
    LocalAssetState state{LocalAssetState::Unverified};

    bool operator==(const LocalAssetObservation&) const = default;
};

// Runtime-only mapping from authored references to verified machine-local
// paths. A present empty mapping is a managed asset that must render nothing;
// an absent mapping is a legacy/unregistered reference and keeps its authored
// fallback. Full verification is performed when the required-content manifest
// or its resolution roots change. Missing/corrupt/error observations may then
// be retried on a bounded playback cadence without re-hashing entries already
// observed as Verified.
struct ResolvedProjectAssets final {
    // Explicit asset identity remains the strongest lookup for evaluated
    // output surfaces. Placement identity preserves the exact asset selected
    // by a scene/surface document node even when multiple roles author the
    // same source text. The role-scoped source map is only the legacy fallback.
    std::map<doc::NodeId, std::string> assets;
    std::map<doc::NodeId, std::string> placements;
    using SourceKey = std::pair<assets::AssetKind, std::string>;
    std::map<SourceKey, std::string> sources;
    std::vector<LocalAssetObservation> observations;

    [[nodiscard]] bool has_retryable_observations() const noexcept;
    [[nodiscard]] bool all_required_verified(
        const ProjectAssetManifest& manifest) const noexcept;
    // Local preview compatibility is deliberately broader than cluster
    // adoption: identified entries must be Verified, while legacy Unverified
    // entries may retain their authored fallback. Portability policy and
    // unresolved registry references are enforced only when a render cluster
    // actually needs reproducible bytes.
    [[nodiscard]] bool all_required_locally_usable(
        const ProjectAssetManifest& manifest) const noexcept;
    void apply(scene::Scene& scene) const;
    void apply(output::FeedMap& feed_map) const;
};

struct ProjectAssetResolutionBinding final {
    doc::NodeId asset{};
    doc::NodeId placement{};
    assets::AssetKind kind{assets::AssetKind::Media};
    std::string authored_source;

    bool operator==(const ProjectAssetResolutionBinding&) const = default;
};

// Cheap, filesystem-free product installed immediately when the required
// manifest changes. `fail_closed` suppresses every content-identified managed
// source until the background resolver publishes an exact-fenced completion.
struct ProjectAssetResolutionPlan final {
    ProjectAssetManifest manifest;
    std::vector<ProjectAssetResolutionBinding> bindings;
    ResolvedProjectAssets fail_closed;
};

[[nodiscard]] ProjectAssetResolutionPlan prepare_project_asset_resolution(
    const doc::Root& root, const assets::AssetRegistry& registry);

struct ProjectAssetLocalCheck final {
    doc::NodeId asset{};
    LocalAssetState state{LocalAssetState::Error};
    std::string local_source;
};

// One cancellable filesystem check. Cancellation is observed at the assets
// layer's bounded read/copy checkpoints and is reported as Error; callers must
// discard the result whenever stop_requested() is true.
[[nodiscard]] ProjectAssetLocalCheck check_project_asset(
    const RequiredProjectAsset& asset,
    const assets::ContentResolutionRoots& roots,
    std::stop_token stop = {});

void apply_project_asset_check(
    ResolvedProjectAssets& resolved,
    const ProjectAssetResolutionPlan& plan,
    ProjectAssetLocalCheck check);

[[nodiscard]] ResolvedProjectAssets resolve_project_assets(
    const doc::Root& root, const assets::AssetRegistry& registry,
    const assets::ContentResolutionRoots& roots);

// Rechecks only assets whose previous local observation was Missing, Corrupt,
// or Error. Verified paths remain untouched, so a synchronized store arrival
// does not turn the retry cadence into a full-manifest hash loop. The caller
// must use the same required-content manifest as `resolved`; a manifest change
// requires resolve_project_assets instead.
void retry_unresolved_project_assets(
    ResolvedProjectAssets& resolved, const doc::Root& root,
    const assets::AssetRegistry& registry,
    const assets::ContentResolutionRoots& roots);

} // namespace mediaserver::app
