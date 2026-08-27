#include "project_asset_import.hpp"

#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/assets/asset_path.hpp>
#include <mediaserver/assets/asset_schema.hpp>

#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace mediaserver::app {

ProjectAssetImportResult prepare_project_asset_import(assets::Asset asset) {
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
    std::visit(
        [&](const auto& asset) {
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
                        std::string{assets::schema::projection_vertex_count},
                        integer(*asset.vertex_count));
                }
                if (!asset.projection_mesh &&
                    asset.material_count.has_value()) {
                    node.properties.emplace(
                        std::string{assets::schema::model_material_count},
                        integer(*asset.material_count));
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
    result.after = make_project_asset_node(target, asset);
    if (existing != nullptr) {
        if (const doc::Node* const before = root.get(target);
            before != nullptr) {
            result.before = *before;
        }
    }
    return result;
}

} // namespace mediaserver::app
