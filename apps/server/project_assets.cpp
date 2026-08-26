#include "project_assets.hpp"

#include <mediaserver/assets/content_identity.hpp>
#include <mediaserver/assets/asset_transfer.hpp>
#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <limits>
#include <set>
#include <string_view>

namespace mediaserver::app {
namespace {

constexpr core::u64 fnv_offset = 1469598103934665603ULL;
constexpr core::u64 fnv_prime = 1099511628211ULL;

void mix_byte(core::u64& state, core::u8 value) noexcept {
    state ^= value;
    state *= fnv_prime;
}

void mix_u64(core::u64& state, core::u64 value) noexcept {
    for (core::u32 shift = 0U; shift < 64U; shift += 8U) {
        mix_byte(state, static_cast<core::u8>((value >> shift) & 0xffU));
    }
}

void mix_text(core::u64& state, std::string_view value) noexcept {
    mix_u64(state, static_cast<core::u64>(value.size()));
    for (const char character : value) {
        mix_byte(state, static_cast<core::u8>(character));
    }
}

struct Reference final {
    doc::NodeId node{};
    assets::AssetKind expected{assets::AssetKind::Media};
    std::string source;
    const assets::Asset* asset{};
};

[[nodiscard]] const doc::PropertyValue* find(
    const doc::PropertyMap& properties, std::string_view name) noexcept {
    const auto found = properties.find(name);
    return found == properties.end() ? nullptr : &found->second;
}

[[nodiscard]] std::string text(const doc::PropertyMap& properties,
                               std::string_view name) {
    const auto* const value = find(properties, name);
    const auto* const held = value == nullptr
        ? nullptr
        : std::get_if<std::string>(value);
    return held == nullptr ? std::string{} : *held;
}

[[nodiscard]] std::optional<doc::NodeId> node_id(
    const doc::PropertyMap& properties, std::string_view name) noexcept {
    const auto* const value = find(properties, name);
    const auto* const held = value == nullptr
        ? nullptr
        : std::get_if<std::int64_t>(value);
    if (held == nullptr || *held < 0) {
        return std::nullopt;
    }
    return doc::NodeId{static_cast<core::u64>(*held)};
}

[[nodiscard]] bool kind_matches(const assets::Asset& asset,
                                assets::AssetKind expected) noexcept {
    return assets::asset_kind(asset) == expected;
}

[[nodiscard]] bool path_entry_exists(
    const std::filesystem::path& path) noexcept {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    return !error && status.type() != std::filesystem::file_type::not_found;
}

[[nodiscard]] std::string path_to_utf8(
    const std::filesystem::path& path) {
    const std::u8string encoded = path.u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const char8_t byte : encoded) {
        result.push_back(static_cast<char>(byte));
    }
    return result;
}

[[nodiscard]] std::vector<Reference> references(
    const doc::Root& root, const assets::AssetRegistry& registry,
    core::u32& unregistered, core::u32& mismatches) {
    std::vector<Reference> result;
    const doc::Delta contents = doc::diff(doc::Root{}, root);
    result.reserve(contents.changes.size());
    for (const doc::NodeChange& change : contents.changes) {
        if (!change.after) {
            continue;
        }
        const doc::Node& node = *change.after;
        assets::AssetKind expected{};
        std::string source;
        std::optional<doc::NodeId> authored_asset;
        bool relevant = false;
        if (node.type == scene::schema::media_type) {
            relevant = true;
            expected = assets::AssetKind::Media;
            source = text(node.properties, scene::schema::source);
            authored_asset = node_id(node.properties, scene::schema::asset_id);
        } else if (node.type == scene::schema::model_type) {
            relevant = true;
            expected = assets::AssetKind::Model;
            source = text(node.properties, scene::schema::source);
            authored_asset = node_id(node.properties, scene::schema::asset_id);
        } else if (node.type == output::schema::surface_type &&
                   text(node.properties, output::schema::surface_shape) ==
                       output::schema::shape_imported_mesh) {
            relevant = true;
            expected = assets::AssetKind::ProjectionMesh;
            source = text(node.properties, output::schema::mesh_source);
            authored_asset =
                node_id(node.properties, output::schema::mesh_asset_id);
        }
        if (!relevant || source.empty()) {
            continue;
        }

        const assets::Asset* asset = authored_asset.has_value()
            ? registry.asset(*authored_asset)
            : nullptr;
        // Removing a Library record intentionally leaves placements usable.
        // Exact-source lookup also migrates old placements that predate
        // asset_id, and repairs a stale deleted identity when the same source
        // has since been re-imported.
        if (asset == nullptr) {
            asset = registry.asset_for_source(source, expected);
        }
        if (asset == nullptr) {
            if (unregistered != std::numeric_limits<core::u32>::max()) {
                ++unregistered;
            }
            result.push_back(Reference{node.id, expected, std::move(source),
                                       nullptr});
            continue;
        }
        if (!kind_matches(*asset, expected)) {
            if (mismatches != std::numeric_limits<core::u32>::max()) {
                ++mismatches;
            }
            result.push_back(Reference{node.id, expected, std::move(source),
                                       nullptr});
            continue;
        }
        result.push_back(
            Reference{node.id, expected, std::move(source), asset});
    }
    return result;
}

[[nodiscard]] ProjectAssetLocalCheck resolve_local_asset(
    const RequiredProjectAsset& asset,
    const assets::ContentResolutionRoots& roots,
    std::stop_token stop) {
    const assets::AssetContent& content = asset.content;
    if (!content.identity.has_value()) {
        return ProjectAssetLocalCheck{
            asset.asset, LocalAssetState::Unverified, {}};
    }

    ProjectAssetLocalCheck result{};
    result.asset = asset.asset;
    if (stop.stop_requested()) {
        return result;
    }
    if (content.policy == assets::AssetDistributionPolicy::SharedStore) {
        const auto alias_relative = assets::shared_store_relative_path(
            *content.identity, asset.logical_source);
        const bool had_store_entry =
            path_entry_exists(
                roots.shared_store /
                assets::shared_store_blob_relative_path(*content.identity)) ||
            (alias_relative.has_value() &&
             path_entry_exists(roots.shared_store / *alias_relative));
        auto stored = assets::resolve_verified_asset(
            roots.shared_store, *content.identity,
            asset.logical_source, {}, stop);
        if (stored) {
            result.local_source = path_to_utf8(stored.value().local_path);
            result.state = LocalAssetState::Verified;
        } else if (stored.error().code ==
                   assets::AssetTransferErrorCode::Incomplete) {
            result.state = had_store_entry ? LocalAssetState::Corrupt
                                           : LocalAssetState::Missing;
        } else if (stored.error().code ==
                       assets::AssetTransferErrorCode::HashMismatch ||
                   stored.error().code ==
                       assets::AssetTransferErrorCode::SizeMismatch) {
            result.state = LocalAssetState::Corrupt;
        }
        return result;
    }

    auto path = assets::resolve_content_source(
        asset.logical_source, content, roots);
    if (!path) {
        return result;
    }
    auto verified =
        assets::verify_file(path.value(), *content.identity, stop);
    if (verified) {
        switch (verified.value()) {
        case assets::ContentVerification::Verified:
            result.local_source = path_to_utf8(path.value());
            result.state = LocalAssetState::Verified;
            break;
        case assets::ContentVerification::SizeMismatch:
        case assets::ContentVerification::DigestMismatch:
            result.state = LocalAssetState::Corrupt;
            break;
        }
    } else if (verified.error().code ==
                   assets::ContentIntegrityErrorCode::InvalidPath ||
               verified.error().code ==
                   assets::ContentIntegrityErrorCode::NotRegularFile ||
               verified.error().code ==
                   assets::ContentIntegrityErrorCode::OpenFailed) {
        result.state = LocalAssetState::Missing;
    }
    return result;
}

[[nodiscard]] bool retryable(LocalAssetState state) noexcept {
    return state == LocalAssetState::Missing ||
        state == LocalAssetState::Corrupt ||
        state == LocalAssetState::Error;
}

[[nodiscard]] core::u64 manifest_digest(
    const ProjectAssetManifest& manifest,
    const std::vector<Reference>& references) noexcept {
    core::u64 state = fnv_offset;
    mix_u64(state, manifest.assets.size());
    for (const RequiredProjectAsset& asset : manifest.assets) {
        mix_u64(state, asset.asset.value());
        mix_byte(state, static_cast<core::u8>(asset.kind));
        mix_text(state, asset.logical_source);
        mix_byte(state, static_cast<core::u8>(asset.content.policy));
        mix_byte(state, asset.content.identity.has_value() ? 1U : 0U);
        if (asset.content.identity.has_value()) {
            for (const core::u8 byte : asset.content.identity->sha256) {
                mix_byte(state, byte);
            }
            mix_u64(state, asset.content.identity->byte_size);
        }
    }
    // This digest is also the runtime-resolution invalidation key. Include
    // every placement-to-asset association and exact authored source so an
    // unrelated scene edit remains cheap, while changing a reference cannot
    // accidentally reuse a stale verified path.
    mix_u64(state, references.size());
    for (const Reference& reference : references) {
        mix_u64(state, reference.node.value());
        mix_byte(state, static_cast<core::u8>(reference.expected));
        mix_text(state, reference.source);
        mix_byte(state, reference.asset == nullptr ? 0U : 1U);
        if (reference.asset != nullptr) {
            mix_u64(state, assets::asset_id(*reference.asset).value());
            mix_byte(state,
                     static_cast<core::u8>(assets::asset_kind(
                         *reference.asset)));
        }
    }
    mix_u64(state, manifest.unregistered_references);
    mix_u64(state, manifest.kind_mismatches);
    mix_u64(state, manifest.unverified_assets);
    mix_u64(state, manifest.dependency_manifests_required);
    mix_u64(state, manifest.non_shared_assets);
    return state;
}

[[nodiscard]] const std::string* resolved_for_reference(
    const ResolvedProjectAssets& resolved, doc::NodeId placement,
    assets::AssetKind kind, std::string_view source) {
    if (placement) {
        const auto by_placement = resolved.placements.find(placement);
        if (by_placement != resolved.placements.end()) {
            return &by_placement->second;
        }
    }
    const auto by_source = resolved.sources.find(
        ResolvedProjectAssets::SourceKey{kind, std::string{source}});
    return by_source == resolved.sources.end() ? nullptr
                                                : &by_source->second;
}

} // namespace

ProjectAssetManifest collect_project_asset_manifest(
    const doc::Root& root, const assets::AssetRegistry& registry) {
    ProjectAssetManifest manifest{};
    auto refs = references(root, registry, manifest.unregistered_references,
                           manifest.kind_mismatches);
    std::set<doc::NodeId> added;
    for (const Reference& reference : refs) {
        if (reference.asset == nullptr ||
            !added.insert(assets::asset_id(*reference.asset)).second) {
            continue;
        }
        const assets::Asset& asset = *reference.asset;
        const assets::AssetContent& content = assets::asset_content(asset);
        manifest.assets.push_back(RequiredProjectAsset{
            assets::asset_id(asset), assets::asset_kind(asset), content,
            std::string{assets::asset_source(asset)}});
        if (!content.identity.has_value() &&
            manifest.unverified_assets !=
                std::numeric_limits<core::u32>::max()) {
            ++manifest.unverified_assets;
        }
        if (assets::asset_requires_dependency_manifest(asset) &&
            manifest.dependency_manifests_required !=
                std::numeric_limits<core::u32>::max()) {
            ++manifest.dependency_manifests_required;
        }
        if (content.policy != assets::AssetDistributionPolicy::SharedStore &&
            manifest.non_shared_assets !=
                std::numeric_limits<core::u32>::max()) {
            ++manifest.non_shared_assets;
        }
    }
    std::sort(manifest.assets.begin(), manifest.assets.end(),
              [](const auto& left, const auto& right) noexcept {
                  return left.asset < right.asset;
              });
    manifest.digest = manifest_digest(manifest, refs);
    return manifest;
}

ProjectAssetResolutionPlan prepare_project_asset_resolution(
    const doc::Root& root, const assets::AssetRegistry& registry) {
    ProjectAssetResolutionPlan plan{};
    plan.manifest = collect_project_asset_manifest(root, registry);
    core::u32 ignored_unregistered{};
    core::u32 ignored_mismatches{};
    const auto refs = references(root, registry, ignored_unregistered,
                                 ignored_mismatches);
    std::map<doc::NodeId, LocalAssetObservation> observations;
    plan.bindings.reserve(refs.size());
    for (const Reference& reference : refs) {
        if (reference.asset == nullptr) {
            continue;
        }
        const doc::NodeId id = assets::asset_id(*reference.asset);
        plan.bindings.push_back(ProjectAssetResolutionBinding{
            id, reference.node, reference.expected, reference.source});
        const assets::AssetContent& content =
            assets::asset_content(*reference.asset);
        const LocalAssetState initial = content.identity.has_value()
            ? LocalAssetState::Checking
            : LocalAssetState::Unverified;
        observations.try_emplace(id, LocalAssetObservation{id, initial});
        if (!content.identity.has_value()) {
            continue;
        }
        // A present empty path is the runtime deny overlay: managed authored
        // paths cannot leak into a frame while their bytes are being checked.
        plan.fail_closed.assets.try_emplace(id, std::string{});
        plan.fail_closed.placements.insert_or_assign(reference.node,
                                                      std::string{});
        plan.fail_closed.sources.insert_or_assign(
            ResolvedProjectAssets::SourceKey{reference.expected,
                                             reference.source},
            std::string{});
    }
    std::sort(plan.bindings.begin(), plan.bindings.end(),
              [](const ProjectAssetResolutionBinding& left,
                 const ProjectAssetResolutionBinding& right) noexcept {
                  if (left.asset != right.asset) {
                      return left.asset < right.asset;
                  }
                  return left.placement < right.placement;
              });
    plan.fail_closed.observations.reserve(observations.size());
    for (const auto& [id, observation] : observations) {
        static_cast<void>(id);
        plan.fail_closed.observations.push_back(observation);
    }
    return plan;
}

ProjectAssetLocalCheck check_project_asset(
    const RequiredProjectAsset& asset,
    const assets::ContentResolutionRoots& roots,
    std::stop_token stop) {
    return resolve_local_asset(asset, roots, stop);
}

void apply_project_asset_check(
    ResolvedProjectAssets& resolved,
    const ProjectAssetResolutionPlan& plan,
    ProjectAssetLocalCheck check) {
    const auto observation = std::find_if(
        resolved.observations.begin(), resolved.observations.end(),
        [&](const LocalAssetObservation& candidate) noexcept {
            return candidate.asset == check.asset;
        });
    if (observation != resolved.observations.end()) {
        observation->state = check.state;
    }
    if (check.state == LocalAssetState::Unverified) {
        return;
    }
    resolved.assets.insert_or_assign(check.asset, check.local_source);
    const auto first = std::lower_bound(
        plan.bindings.begin(), plan.bindings.end(), check.asset,
        [](const ProjectAssetResolutionBinding& binding,
           doc::NodeId asset) noexcept { return binding.asset < asset; });
    for (auto binding = first;
         binding != plan.bindings.end() && binding->asset == check.asset;
         ++binding) {
        resolved.placements.insert_or_assign(binding->placement,
                                             check.local_source);
        resolved.sources.insert_or_assign(
            ResolvedProjectAssets::SourceKey{binding->kind,
                                             binding->authored_source},
            check.local_source);
    }
}

ResolvedProjectAssets resolve_project_assets(
    const doc::Root& root, const assets::AssetRegistry& registry,
    const assets::ContentResolutionRoots& roots) {
    const ProjectAssetResolutionPlan plan =
        prepare_project_asset_resolution(root, registry);
    ResolvedProjectAssets resolved = plan.fail_closed;
    for (const RequiredProjectAsset& asset : plan.manifest.assets) {
        apply_project_asset_check(
            resolved, plan, check_project_asset(asset, roots));
    }
    return resolved;
}

bool ResolvedProjectAssets::has_retryable_observations() const noexcept {
    return std::any_of(
        observations.begin(), observations.end(),
        [](const LocalAssetObservation& observation) noexcept {
            return retryable(observation.state);
        });
}

bool ResolvedProjectAssets::all_required_verified(
    const ProjectAssetManifest& manifest) const noexcept {
    if (!manifest.shared_store_ready() ||
        observations.size() != manifest.assets.size()) {
        return false;
    }
    for (const RequiredProjectAsset& asset : manifest.assets) {
        const auto observation = std::lower_bound(
            observations.begin(), observations.end(), asset.asset,
            [](const LocalAssetObservation& candidate,
               doc::NodeId wanted) noexcept {
                return candidate.asset < wanted;
            });
        if (observation == observations.end() ||
            observation->asset != asset.asset ||
            observation->state != LocalAssetState::Verified) {
            return false;
        }
    }
    return true;
}

bool ResolvedProjectAssets::all_required_locally_usable(
    const ProjectAssetManifest& manifest) const noexcept {
    if (observations.size() != manifest.assets.size()) {
        return false;
    }
    for (const RequiredProjectAsset& asset : manifest.assets) {
        const auto observation = std::lower_bound(
            observations.begin(), observations.end(), asset.asset,
            [](const LocalAssetObservation& candidate,
               doc::NodeId wanted) noexcept {
                return candidate.asset < wanted;
            });
        if (observation == observations.end() ||
            observation->asset != asset.asset) {
            return false;
        }
        const LocalAssetState expected = asset.content.identity.has_value()
            ? LocalAssetState::Verified
            : LocalAssetState::Unverified;
        if (observation->state != expected) {
            return false;
        }
    }
    return true;
}

void retry_unresolved_project_assets(
    ResolvedProjectAssets& resolved, const doc::Root& root,
    const assets::AssetRegistry& registry,
    const assets::ContentResolutionRoots& roots) {
    std::set<doc::NodeId> retry_ids;
    for (const LocalAssetObservation& observation : resolved.observations) {
        if (retryable(observation.state)) {
            retry_ids.insert(observation.asset);
        }
    }
    if (retry_ids.empty()) {
        return;
    }

    const ProjectAssetResolutionPlan plan =
        prepare_project_asset_resolution(root, registry);
    for (const RequiredProjectAsset& asset : plan.manifest.assets) {
        if (!retry_ids.contains(asset.asset)) {
            continue;
        }
        apply_project_asset_check(
            resolved, plan, check_project_asset(asset, roots));
    }
}

void ResolvedProjectAssets::apply(scene::Scene& scene) const {
    for (scene::MediaInstance& media : scene.media) {
        const std::string* const found = resolved_for_reference(
            *this, media.source_node, assets::AssetKind::Media,
            media.source);
        if (found != nullptr) {
            media.resolved_source = *found;
        }
    }
    for (scene::ModelInstance& model : scene.models) {
        const std::string* const found = resolved_for_reference(
            *this, model.source_node, assets::AssetKind::Model,
            model.source);
        if (found != nullptr) {
            model.resolved_source = *found;
        }
    }
}

void ResolvedProjectAssets::apply(output::FeedMap& feed_map) const {
    for (output::ProjectionSurface& surface : feed_map.surfaces) {
        const auto by_id = surface.mesh_asset
            ? assets.find(surface.mesh_asset)
            : assets.end();
        if (by_id != assets.end()) {
            surface.resolved_mesh_source = by_id->second;
            continue;
        }
        const std::string* const fallback = resolved_for_reference(
            *this, surface.node, assets::AssetKind::ProjectionMesh,
            surface.mesh_source);
        if (fallback != nullptr) {
            surface.resolved_mesh_source = *fallback;
        }
    }
    for (output::OutputMap& map : feed_map.maps) {
        if (!map.resolved_surface.has_value()) {
            continue;
        }
        const auto by_id = map.resolved_surface->mesh_asset
            ? assets.find(map.resolved_surface->mesh_asset)
            : assets.end();
        if (by_id != assets.end()) {
            map.resolved_surface->resolved_mesh_source = by_id->second;
            continue;
        }
        const std::string* const fallback = resolved_for_reference(
            *this, {}, assets::AssetKind::ProjectionMesh,
            map.resolved_surface->mesh_source);
        if (fallback != nullptr) {
            map.resolved_surface->resolved_mesh_source = *fallback;
        }
    }
}

} // namespace mediaserver::app
