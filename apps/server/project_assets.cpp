#include "project_assets.hpp"

#include <mediaserver/assets/asset_path.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/scene/linked_scene.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <limits>
#include <filesystem>
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

void mix_fingerprint(
    core::u64& state,
    const std::optional<assets::ContentFingerprint>& fingerprint) noexcept {
    mix_byte(state, fingerprint.has_value() ? 1U : 0U);
    if (!fingerprint.has_value()) {
        return;
    }
    mix_u64(state, fingerprint->byte_length);
    for (const core::u8 byte : fingerprint->sha256.bytes) {
        mix_byte(state, byte);
    }
}

void increment(core::u32& value) noexcept {
    if (value != std::numeric_limits<core::u32>::max()) {
        ++value;
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
        const auto include_color = [&](std::string_view property) {
            auto path = text(node.properties, property);
            if (assets::valid_asset_source(path)) manifest.assets.push_back(RequiredProjectAsset{
                {}, assets::AssetKind::ColorLut, std::move(path), {}});
        };
        if (node.type == output::schema::surface_type) include_color(output::schema::camera_inverse_lut);
        if (node.type == scene::schema::settings_type) include_color(scene::schema::show_lut);
        assets::AssetKind kind{};
        std::string source;
        std::optional<doc::NodeId> authored_asset;
        bool relevant = false;
        if (node.type == scene::schema::light_type) {
            relevant = true;
            kind = assets::AssetKind::IesProfile;
            source = text(node.properties, scene::schema::ies_source);
        } else if (node.type == scene::schema::media_type) {
            relevant = true;
            kind = assets::AssetKind::Media;
            source = text(node.properties, scene::schema::source);
            authored_asset = node_id(node.properties, scene::schema::asset_id);
        } else if (node.type == scene::schema::model_type) {
            relevant = true;
            kind = assets::AssetKind::Model;
            source = text(node.properties, scene::schema::source);
            authored_asset = node_id(node.properties, scene::schema::asset_id);
        } else if (node.type == scene::link_schema::prototype_type) {
            // Geometry Blender sent: the ingested package every linked
            // occurrence of this prototype draws.
            relevant = true;
            kind = assets::AssetKind::Model;
            source = text(node.properties, scene::schema::source);
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
        // An image sequence is a '#' run resolved per frame. A machine has it
        // when its first frame opens - one stat, like any other source - and
        // there is no single file whose revision a fingerprint could name.
        if (const auto run = source.find('#');
            kind == assets::AssetKind::Media && run != std::string::npos) {
            auto end = run;
            while (end < source.size() && source[end] == '#') ++end;
            const auto* start = find(node.properties, scene::schema::sequence_start);
            const auto* first = start == nullptr ? nullptr : std::get_if<std::int64_t>(start);
            auto number = std::to_string(first != nullptr && *first > 0 ? *first : 0);
            if (number.size() < end - run) number.insert(0U, end - run - number.size(), '0');
            manifest.assets.push_back(RequiredProjectAsset{
                {}, kind, source.substr(0U, run) + number + source.substr(end), {}});
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
            std::move(source),
            asset == nullptr
                ? std::optional<assets::ContentFingerprint>{}
                : assets::asset_fingerprint(*asset)});
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
        mix_fingerprint(state, required.expected_fingerprint);
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
        ProjectAssetPresence presence{};
        presence.asset = required.asset;
        presence.kind = required.kind;
        presence.source = required.source;
        presence.expected_fingerprint = required.expected_fingerprint;

        if (!required.expected_fingerprint.has_value()) {
            presence.present = assets::asset_source_present(required.source);
            presence.state = presence.present
                ? ProjectAssetPresence::State::Ready
                : ProjectAssetPresence::State::Missing;
            if (!presence.present) {
                increment(availability.missing);
            }
            availability.assets.push_back(std::move(presence));
            continue;
        }

        auto observed = assets::source_fingerprint(required.source);
        if (!observed) {
            if (observed.error().code ==
                    assets::ContentFingerprintErrorCode::SourceMissing ||
                observed.error().code ==
                    assets::ContentFingerprintErrorCode::InvalidSource) {
                presence.state = ProjectAssetPresence::State::Missing;
                increment(availability.missing);
            } else {
                presence.present = true;
                presence.state =
                    ProjectAssetPresence::State::VerificationFailed;
                increment(availability.verification_failed);
            }
            availability.assets.push_back(std::move(presence));
            continue;
        }

        presence.present = true;
        presence.observed_fingerprint = observed.value();
        if (observed.value().byte_length !=
            required.expected_fingerprint->byte_length) {
            presence.state =
                ProjectAssetPresence::State::ByteLengthMismatch;
            increment(availability.mismatched);
        } else if (observed.value().sha256 !=
                   required.expected_fingerprint->sha256) {
            presence.state = ProjectAssetPresence::State::Sha256Mismatch;
            increment(availability.mismatched);
        } else {
            presence.state = ProjectAssetPresence::State::Ready;
        }
        availability.assets.push_back(std::move(presence));
    }
    return availability;
}

std::vector<ChangedLibrarySource> find_changed_library_sources(
    const assets::AssetRegistry& registry) {
    std::vector<ChangedLibrarySource> changed;
    for (const assets::Asset& asset : registry.assets) {
        const std::optional<assets::ContentFingerprint>& recorded =
            assets::asset_fingerprint(asset);
        if (!recorded.has_value()) {
            continue;
        }
        auto observed =
            assets::source_fingerprint(assets::asset_source(asset));
        if (!observed || observed.value() == *recorded) {
            continue;
        }
        changed.push_back(ChangedLibrarySource{assets::asset_id(asset),
                                               std::move(observed).value()});
    }
    return changed;
}

std::vector<doc::NodeId> still_changed_library_sources(
    const assets::AssetRegistry& registry,
    const std::vector<ChangedLibrarySource>& found) {
    std::vector<doc::NodeId> changed;
    for (const ChangedLibrarySource& entry : found) {
        const assets::Asset* const asset = registry.asset(entry.asset);
        if (asset == nullptr) {
            continue;
        }
        const std::optional<assets::ContentFingerprint>& recorded =
            assets::asset_fingerprint(*asset);
        if (recorded.has_value() && *recorded != entry.observed) {
            changed.push_back(entry.asset);
        }
    }
    return changed;
}

ProjectAssetAvailability scan_project_assets(
    const ProjectAssetManifest& manifest) {
    ProjectAssetAvailability availability{};
    availability.digest = manifest.digest;
    availability.assets.reserve(manifest.assets.size());
    for (const RequiredProjectAsset& required : manifest.assets) {
        ProjectAssetPresence presence{};
        presence.asset = required.asset;
        presence.kind = required.kind;
        presence.source = required.source;
        presence.expected_fingerprint = required.expected_fingerprint;
        presence.present = assets::asset_source_present(required.source);
        presence.state = presence.present
            ? ProjectAssetPresence::State::Ready
            : ProjectAssetPresence::State::Missing;
        if (!presence.present) {
            increment(availability.missing);
        }
        availability.assets.push_back(std::move(presence));
    }
    return availability;
}

} // namespace mediaserver::app
