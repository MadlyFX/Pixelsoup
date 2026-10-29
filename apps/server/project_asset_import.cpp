#include "project_asset_import.hpp"

#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/assets/asset_path.hpp>
#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/assets/plateset.hpp>
#include <mediaserver/assets/proxy_policy.hpp>
#include <mediaserver/output/feed_map_schema.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <new>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace mediaserver::app {

ProjectAssetImportResult prepare_project_asset_import(
    assets::Asset asset) {
    const std::string_view source = assets::asset_source(asset);
    if (!assets::valid_asset_source(source)) {
        return ProjectAssetImportResult::failure(
            ProjectAssetImportError{
                ProjectAssetImportErrorCode::InvalidSource});
    }
    if (!assets::asset_source_present(source)) {
        return ProjectAssetImportResult::failure(
            ProjectAssetImportError{
                ProjectAssetImportErrorCode::SourceMissing});
    }
    auto fingerprint = assets::source_fingerprint(source);
    if (!fingerprint) {
        const auto code = fingerprint.error().code ==
                assets::ContentFingerprintErrorCode::SourceMissing
            ? ProjectAssetImportErrorCode::SourceMissing
            : ProjectAssetImportErrorCode::FingerprintFailed;
        return ProjectAssetImportResult::failure(ProjectAssetImportError{code});
    }
    const assets::ContentFingerprint measured =
        std::move(fingerprint).value();
    std::visit(
        [&measured](auto& held) {
            // A plateset names other assets and is never imported from a file.
            if constexpr (!std::is_same_v<std::decay_t<decltype(held)>,
                                          assets::PlatesetAsset>) {
                held.fingerprint = measured;
            }
        },
        asset);
    return ProjectAssetImportResult::success(std::move(asset));
}

doc::Node make_project_asset_node(doc::NodeId id,
                                  const assets::Asset& imported) {
    doc::Node node{};
    node.id = id;
    const auto integer = [](core::u64 value) {
        return doc::PropertyValue{static_cast<std::int64_t>(value)};
    };
    const auto real = [](double value) { return doc::PropertyValue{value}; };
    const auto write = [&](const auto& asset) {
            using Asset = std::decay_t<decltype(asset)>;
            if constexpr (std::is_same_v<Asset, assets::MediaAsset>) {
                node.type = std::string{assets::schema::media_type};
            } else {
                node.type = std::string{
                    asset.projection_mesh
                        ? assets::schema::projection_mesh_type
                        : assets::schema::model_type};
            }
            node.properties.emplace(
                std::string{assets::schema::display_name},
                doc::PropertyValue{asset.display_name});
            node.properties.emplace(
                std::string{assets::schema::source},
                doc::PropertyValue{asset.source});
            if (asset.folder != doc::NodeId{}) {
                node.properties.emplace(
                    std::string{assets::schema::folder},
                    integer(asset.folder.value()));
            }
            if (asset.fingerprint.has_value()) {
                node.properties.emplace(
                    std::string{assets::schema::expected_byte_length},
                    integer(asset.fingerprint->byte_length));
                node.properties.emplace(
                    std::string{assets::schema::expected_sha256},
                    doc::PropertyValue{
                        assets::sha256_hex(asset.fingerprint->sha256)});
            }
            if constexpr (std::is_same_v<Asset, assets::MediaAsset>) {
                if (asset.dimensions.has_value()) {
                    node.properties.emplace(
                        std::string{assets::schema::media_width},
                        integer(asset.dimensions->width));
                    node.properties.emplace(
                        std::string{assets::schema::media_height},
                        integer(asset.dimensions->height));
                }
                if (asset.frame_count.has_value()) {
                    node.properties.emplace(
                        std::string{assets::schema::media_frame_count},
                        integer(*asset.frame_count));
                }
                if (asset.frame_rate.has_value()) {
                    node.properties.emplace(
                        std::string{assets::schema::media_rate_numerator},
                        integer(asset.frame_rate->numerator));
                    node.properties.emplace(
                        std::string{assets::schema::media_rate_denominator},
                        integer(asset.frame_rate->denominator));
                }
                if (!asset.codec.empty()) {
                    node.properties.emplace(
                        std::string{assets::schema::media_codec},
                        doc::PropertyValue{asset.codec});
                }
                if (!asset.start_timecode.empty()) {
                    node.properties.emplace(
                        std::string{assets::schema::media_start_timecode},
                        doc::PropertyValue{asset.start_timecode});
                }
                if (asset.requested_proxy_profile.has_value()) {
                    const assets::DerivativeProfile* const profile =
                        assets::derivative_profile(
                            *asset.requested_proxy_profile);
                    if (profile != nullptr &&
                        profile->kind == assets::DerivativeKind::Proxy) {
                        node.properties.emplace(
                            std::string{
                                assets::schema::requested_proxy_profile},
                            doc::PropertyValue{std::string{profile->name}});
                    }
                }
                if (asset.proxy_playback_policy !=
                    assets::ProxyPlaybackPolicy::Source) {
                    node.properties.emplace(
                        std::string{assets::schema::proxy_playback_policy},
                        doc::PropertyValue{std::string{
                            assets::proxy_playback_policy_name(
                                asset.proxy_playback_policy)}});
                }
            } else {
                if (asset.triangle_count.has_value()) {
                    node.properties.emplace(
                        std::string{asset.projection_mesh
                            ? assets::schema::projection_triangle_count
                            : assets::schema::model_triangle_count},
                        integer(*asset.triangle_count));
                }
                if (asset.vertex_count.has_value()) {
                    node.properties.emplace(
                        std::string{asset.projection_mesh
                            ? assets::schema::projection_vertex_count
                            : assets::schema::model_vertex_count},
                        integer(*asset.vertex_count));
                }
                if (!asset.projection_mesh &&
                    asset.material_count.has_value()) {
                    node.properties.emplace(
                        std::string{assets::schema::model_material_count},
                        integer(*asset.material_count));
                }
                if (!asset.projection_mesh &&
                    asset.texture_count.has_value()) {
                    node.properties.emplace(
                        std::string{assets::schema::model_texture_count},
                        integer(*asset.texture_count));
                }
                if (!asset.projection_mesh &&
                    asset.largest_texture.has_value()) {
                    node.properties.emplace(
                        std::string{assets::schema::model_texture_width},
                        integer(asset.largest_texture->width));
                    node.properties.emplace(
                        std::string{assets::schema::model_texture_height},
                        integer(asset.largest_texture->height));
                }
                if (asset.bounds.has_value()) {
                    node.properties.emplace(
                        std::string{assets::schema::bounds_minimum_x},
                        real(asset.bounds->minimum_x));
                    node.properties.emplace(
                        std::string{assets::schema::bounds_minimum_y},
                        real(asset.bounds->minimum_y));
                    node.properties.emplace(
                        std::string{assets::schema::bounds_minimum_z},
                        real(asset.bounds->minimum_z));
                    node.properties.emplace(
                        std::string{assets::schema::bounds_maximum_x},
                        real(asset.bounds->maximum_x));
                    node.properties.emplace(
                        std::string{assets::schema::bounds_maximum_y},
                        real(asset.bounds->maximum_y));
                    node.properties.emplace(
                        std::string{assets::schema::bounds_maximum_z},
                        real(asset.bounds->maximum_z));
                }
            }
        };
    std::visit(
        [&write](const auto& asset) {
            if constexpr (!std::is_same_v<std::decay_t<decltype(asset)>,
                                          assets::PlatesetAsset>) {
                write(asset);
            }
        },
        imported);
    return node;
}

ProjectAssetNodeUpsert make_project_asset_upsert(
    const doc::Root& root, const assets::Asset& asset,
    doc::NodeId allocated_id) {
    assets::AssetModel model;
    model.reset(root);
    const assets::AssetRegistry registry = model.evaluate();
    const assets::Asset* const existing =
        registry.asset_for_source(assets::asset_source(asset),
                                  assets::asset_kind(asset));
    const doc::NodeId target =
        existing == nullptr ? allocated_id : assets::asset_id(*existing);
    ProjectAssetNodeUpsert result{};
    // Importing a file the Library already holds refreshes it where the
    // operator filed it, unless this import names a folder of its own.
    if (existing != nullptr && assets::asset_folder(asset) == doc::NodeId{} &&
        assets::asset_folder(*existing) != doc::NodeId{}) {
        assets::Asset refiled = asset;
        std::visit([folder = assets::asset_folder(*existing)](
                       auto& held) noexcept { held.folder = folder; },
                   refiled);
        result.after = make_project_asset_node(target, refiled);
    } else {
        result.after = make_project_asset_node(target, asset);
    }
    if (existing != nullptr) {
        if (const doc::Node* const before = root.get(target);
            before != nullptr) {
            result.before = *before;
        }
    }
    return result;
}

namespace {

[[nodiscard]] bool is_asset_node(const doc::Node& node) noexcept {
    return node.type == assets::schema::media_type ||
           node.type == assets::schema::model_type ||
           node.type == assets::schema::projection_mesh_type ||
           node.type == assets::schema::plateset_type;
}

[[nodiscard]] ProjectAssetAuthoringError authoring_error(
    ProjectAssetAuthoringErrorCode code) {
    return ProjectAssetAuthoringError{code, {}};
}

[[nodiscard]] bool references_asset(const doc::PropertyValue& value,
                                    doc::NodeId asset) noexcept {
    const auto* const reference = std::get_if<std::int64_t>(&value);
    return reference != nullptr && *reference > 0 &&
           static_cast<core::u64>(*reference) == asset.value();
}

constexpr std::array<std::string_view, 26U> asset_properties{{
    assets::schema::display_name,
    assets::schema::source,
    assets::schema::expected_byte_length,
    assets::schema::expected_sha256,
    assets::schema::media_width,
    assets::schema::media_height,
    assets::schema::media_frame_count,
    assets::schema::media_rate_numerator,
    assets::schema::media_rate_denominator,
    assets::schema::media_codec,
    assets::schema::media_start_timecode,
    assets::schema::requested_proxy_profile,
    assets::schema::proxy_playback_policy,
    assets::schema::model_triangle_count,
    assets::schema::model_material_count,
    assets::schema::model_vertex_count,
    assets::schema::model_texture_count,
    assets::schema::model_texture_width,
    assets::schema::model_texture_height,
    assets::schema::projection_vertex_count,
    assets::schema::projection_triangle_count,
    assets::schema::bounds_minimum_x,
    assets::schema::bounds_minimum_y,
    assets::schema::bounds_minimum_z,
    assets::schema::bounds_maximum_x,
    assets::schema::bounds_maximum_y,
}};

// Kept separate because the fixed array above intentionally makes additions
// compile-visible; this final member would otherwise be easy to omit while
// changing its declared size.
constexpr std::string_view final_asset_property =
    assets::schema::bounds_maximum_z;

void erase_asset_properties(doc::Node& node) {
    for (const std::string_view property : asset_properties) {
        node.properties.erase(std::string{property});
    }
    node.properties.erase(std::string{final_asset_property});
}

} // namespace

ProjectAssetDeleteResult plan_project_asset_delete(
    const doc::Root& root, doc::NodeId asset) {
    using Result = ProjectAssetDeleteResult;
    try {
        const doc::Node* const existing = root.get(asset);
        if (existing == nullptr || !is_asset_node(*existing)) {
            return Result::failure(authoring_error(
                ProjectAssetAuthoringErrorCode::AssetNotFound));
        }
        std::vector<ProjectAssetReference> references;
        const doc::Delta contents = doc::diff(doc::Root{}, root);
        constexpr std::array<std::string_view, 2U> reference_properties{{
            scene::schema::asset_id,
            output::schema::mesh_asset_id,
        }};
        for (const doc::NodeChange& change : contents.changes) {
            if (!change.after || change.id == asset) {
                continue;
            }
            for (const std::string_view property : reference_properties) {
                const auto found = change.after->properties.find(property);
                if (found != change.after->properties.end() &&
                    references_asset(found->second, asset)) {
                    references.push_back(ProjectAssetReference{
                        change.id, std::string{property}});
                }
            }
            // A plate in a plateset is in use: removing it would leave the
            // set with a hole the operator did not ask for.
            if (change.after->type == assets::schema::plateset_type) {
                for (const assets::PlatesetSlot slot :
                     assets::all_plateset_slots) {
                    const std::string property =
                        assets::plateset_slot_property(slot);
                    const auto found = change.after->properties.find(property);
                    if (found != change.after->properties.end() &&
                        references_asset(found->second, asset)) {
                        references.push_back(
                            ProjectAssetReference{change.id, property});
                    }
                }
            }
        }
        std::sort(references.begin(), references.end());
        references.erase(std::unique(references.begin(), references.end()),
                         references.end());
        if (!references.empty()) {
            return Result::failure(ProjectAssetAuthoringError{
                ProjectAssetAuthoringErrorCode::InUse,
                std::move(references)});
        }
        const std::optional<doc::Root> erased = root.erase(asset);
        if (!erased.has_value()) {
            return Result::failure(authoring_error(
                ProjectAssetAuthoringErrorCode::AssetNotFound));
        }
        return Result::success(
            ProjectAssetDeletePlan{*erased, *existing});
    } catch (const std::bad_alloc&) {
        return Result::failure(authoring_error(
            ProjectAssetAuthoringErrorCode::AllocationFailed));
    }
}

ProjectAssetRevisionResult plan_project_asset_revision(
    const doc::Root& root, doc::NodeId asset,
    const assets::Asset& replacement, ProjectAssetRevisionMode mode) {
    using Result = ProjectAssetRevisionResult;
    try {
        const doc::Node* const existing_node = root.get(asset);
        if (existing_node == nullptr || !is_asset_node(*existing_node)) {
            return Result::failure(authoring_error(
                ProjectAssetAuthoringErrorCode::AssetNotFound));
        }
        assets::AssetModel model;
        model.reset(root);
        const assets::AssetRegistry registry = model.evaluate();
        const assets::Asset* const existing = registry.asset(asset);
        if (existing == nullptr) {
            return Result::failure(authoring_error(
                ProjectAssetAuthoringErrorCode::AssetNotFound));
        }
        if (assets::asset_kind(*existing) !=
            assets::asset_kind(replacement)) {
            return Result::failure(authoring_error(
                ProjectAssetAuthoringErrorCode::AssetKindMismatch));
        }
        if (!assets::valid_asset_source(
                assets::asset_source(replacement)) ||
            !assets::asset_fingerprint(replacement).has_value()) {
            return Result::failure(authoring_error(
                ProjectAssetAuthoringErrorCode::InvalidPreparedAsset));
        }
        if (mode == ProjectAssetRevisionMode::AcceptCurrentSource &&
            assets::asset_source(*existing) !=
                assets::asset_source(replacement)) {
            return Result::failure(authoring_error(
                ProjectAssetAuthoringErrorCode::SourceMismatch));
        }

        assets::Asset normalized = replacement;
        std::visit([&](auto& candidate) { candidate.id = asset; }, normalized);
        std::visit(
            [&](auto& candidate) {
                using Candidate = std::decay_t<decltype(candidate)>;
                if (const auto* const prior =
                        std::get_if<Candidate>(existing)) {
                    candidate.display_name = prior->display_name;
                    candidate.folder = prior->folder;
                    if constexpr (std::is_same_v<
                                      Candidate, assets::MediaAsset>) {
                        candidate.requested_proxy_profile =
                            prior->requested_proxy_profile;
                        candidate.proxy_playback_policy =
                            prior->proxy_playback_policy;
                    }
                }
            },
            normalized);

        const doc::Node replacement_node =
            make_project_asset_node(asset, normalized);
        doc::Node changed = *existing_node;
        erase_asset_properties(changed);
        changed.type = replacement_node.type;
        for (const auto& [name, value] : replacement_node.properties) {
            changed.properties.insert_or_assign(name, value);
        }
        const bool modified = changed != *existing_node;
        const doc::Root after = modified ? root.set(changed) : root;
        return Result::success(ProjectAssetRevisionPlan{
            after, *existing_node, std::move(changed), modified});
    } catch (const std::bad_alloc&) {
        return Result::failure(authoring_error(
            ProjectAssetAuthoringErrorCode::AllocationFailed));
    }
}

} // namespace mediaserver::app
