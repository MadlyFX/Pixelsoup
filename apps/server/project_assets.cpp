#include "project_assets.hpp"

#include <mediaserver/assets/asset_path.hpp>
#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

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

} // namespace

ProjectAssetManifest collect_project_asset_manifest(
    const doc::Root& root, const assets::AssetRegistry& registry) {
    ProjectAssetManifest manifest{};
    const doc::Delta contents = doc::diff(doc::Root{}, root);
    manifest.assets.reserve(contents.changes.size());

    for (const doc::NodeChange& change : contents.changes) {
        if (!change.after) {
            continue;
        }
        const doc::Node& node = *change.after;
        assets::AssetKind kind{};
        std::string source;
        std::optional<doc::NodeId> authored_asset;
        bool relevant = false;
        if (node.type == scene::schema::media_type) {
            relevant = true;
            kind = assets::AssetKind::Media;
            source = text(node.properties, scene::schema::source);
            authored_asset = node_id(node.properties, scene::schema::asset_id);
        } else if (node.type == scene::schema::model_type) {
            relevant = true;
            kind = assets::AssetKind::Model;
            source = text(node.properties, scene::schema::source);
            authored_asset = node_id(node.properties, scene::schema::asset_id);
        } else if (node.type == output::schema::surface_type &&
                   text(node.properties, output::schema::surface_shape) ==
                       output::schema::shape_imported_mesh) {
            relevant = true;
            kind = assets::AssetKind::ProjectionMesh;
            source = text(node.properties, output::schema::mesh_source);
            authored_asset =
                node_id(node.properties, output::schema::mesh_asset_id);
        }
        if (!relevant || !assets::valid_asset_source(source)) {
            continue;
        }

        // A placement's asset_id is the strongest link; exact-source lookup
        // repairs a placement whose record was deleted and re-imported. A
        // placement with no record at all is still a file this show opens, so
        // it belongs in the manifest either way - which is what keeps a node
        // and the Director agreeing about what has to be present.
        const assets::Asset* asset = authored_asset.has_value()
            ? registry.asset(*authored_asset)
            : nullptr;
        if (asset != nullptr && assets::asset_kind(*asset) != kind) {
            if (manifest.kind_mismatches !=
                std::numeric_limits<core::u32>::max()) {
                ++manifest.kind_mismatches;
            }
            asset = nullptr;
        }
        if (asset == nullptr) {
            asset = registry.asset_for_source(source, kind);
        }
        manifest.assets.push_back(RequiredProjectAsset{
            asset == nullptr ? doc::NodeId{} : assets::asset_id(*asset), kind,
            std::move(source)});
    }

    const auto order = [](const RequiredProjectAsset& left,
                          const RequiredProjectAsset& right) noexcept {
        if (left.kind != right.kind) {
            return left.kind < right.kind;
        }
        return left.source < right.source;
    };
    std::sort(manifest.assets.begin(), manifest.assets.end(), order);
    manifest.assets.erase(
        std::unique(manifest.assets.begin(), manifest.assets.end(),
                    [](const RequiredProjectAsset& left,
                       const RequiredProjectAsset& right) noexcept {
                        return left.kind == right.kind &&
                            left.source == right.source;
                    }),
        manifest.assets.end());

    core::u64 state = fnv_offset;
    mix_u64(state, manifest.assets.size());
    for (const RequiredProjectAsset& required : manifest.assets) {
        mix_byte(state, static_cast<core::u8>(required.kind));
        mix_text(state, required.source);
    }
    mix_u64(state, manifest.kind_mismatches);
    manifest.digest = state;
    return manifest;
}

ProjectAssetAvailability check_project_assets(
    const ProjectAssetManifest& manifest) {
    ProjectAssetAvailability availability{};
    availability.digest = manifest.digest;
    availability.assets.reserve(manifest.assets.size());
    for (const RequiredProjectAsset& required : manifest.assets) {
        const bool present = assets::asset_source_present(required.source);
        if (!present &&
            availability.missing != std::numeric_limits<core::u32>::max()) {
            ++availability.missing;
        }
        availability.assets.push_back(
            ProjectAssetPresence{required.asset, required.source, present});
    }
    return availability;
}

} // namespace mediaserver::app
