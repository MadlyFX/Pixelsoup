#include "project_asset_import.hpp"

#include <mediaserver/assets/asset_model.hpp>
#include <mediaserver/assets/asset_schema.hpp>
#include <mediaserver/assets/asset_transfer.hpp>
#include <mediaserver/assets/content_identity.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <new>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace mediaserver::app {
namespace {

[[nodiscard]] ProjectAssetImportResult import_failure(
    ProjectAssetImportErrorCode code, std::error_code system_error = {}) {
    return ProjectAssetImportResult::failure(
        ProjectAssetImportError{code, system_error});
}

[[nodiscard]] ProjectAssetImportError transfer_failure(
    ProjectAssetImportErrorCode code,
    const assets::AssetTransferError& error) noexcept {
    if (error.code == assets::AssetTransferErrorCode::Cancelled) {
        return ProjectAssetImportError{
            ProjectAssetImportErrorCode::Cancelled, error.system_error};
    }
    return ProjectAssetImportError{code, error.system_error};
}

[[nodiscard]] ProjectAssetImportResult cancelled_import() {
    return import_failure(ProjectAssetImportErrorCode::Cancelled);
}

[[nodiscard]] assets::AssetContent& mutable_content(
    assets::Asset& asset) noexcept {
    return std::visit(
        [](auto& held) noexcept -> assets::AssetContent& {
            return held.content;
        },
        asset);
}

[[nodiscard]] std::filesystem::path path_from_utf8(
    std::string_view encoded) {
    std::u8string native;
    native.reserve(encoded.size());
    for (const unsigned char byte : encoded) {
        native.push_back(static_cast<char8_t>(byte));
    }
    return std::filesystem::path{native};
}

[[nodiscard]] core::Result<assets::AssetTransferReceipt,
                           ProjectAssetImportError>
install_shared_store(const std::filesystem::path& source_path,
                     std::string_view logical_source,
                     const assets::ContentIdentity& identity,
                     const std::filesystem::path& shared_store,
                     std::stop_token stop) {
    using Result = core::Result<assets::AssetTransferReceipt,
                                ProjectAssetImportError>;
    if (stop.stop_requested()) {
        return Result::failure(ProjectAssetImportError{
            ProjectAssetImportErrorCode::Cancelled, {}});
    }
    if (shared_store.empty()) {
        return Result::failure(ProjectAssetImportError{
            ProjectAssetImportErrorCode::SharedStoreRequired, {}});
    }

    auto opened = assets::ResumableAssetSink::open(
        shared_store, identity, logical_source, {}, stop);
    if (!opened) {
        return Result::failure(transfer_failure(
            ProjectAssetImportErrorCode::StoreOpenFailed, opened.error()));
    }
    auto sink = std::move(opened).value();
    if (stop.stop_requested()) {
        return Result::failure(ProjectAssetImportError{
            ProjectAssetImportErrorCode::Cancelled, {}});
    }
    if (!sink.complete()) {
        if (stop.stop_requested()) {
            return Result::failure(ProjectAssetImportError{
                ProjectAssetImportErrorCode::Cancelled, {}});
        }
        std::ifstream input{source_path, std::ios::binary};
        if (stop.stop_requested()) {
            return Result::failure(ProjectAssetImportError{
                ProjectAssetImportErrorCode::Cancelled, {}});
        }
        if (!input.is_open()) {
            return Result::failure(ProjectAssetImportError{
                ProjectAssetImportErrorCode::SourceReadFailed, {}});
        }
        const core::u64 resume = sink.resume_offset();
        if (stop.stop_requested()) {
            return Result::failure(ProjectAssetImportError{
                ProjectAssetImportErrorCode::Cancelled, {}});
        }
        input.seekg(static_cast<std::streamoff>(resume), std::ios::beg);
        if (stop.stop_requested()) {
            return Result::failure(ProjectAssetImportError{
                ProjectAssetImportErrorCode::Cancelled, {}});
        }
        if (!input) {
            return Result::failure(ProjectAssetImportError{
                ProjectAssetImportErrorCode::SourceReadFailed, {}});
        }

        constexpr std::size_t chunk_bytes = 1024U * 1024U;
        std::vector<std::byte> bytes(chunk_bytes);
        core::u64 offset = resume;
        while (offset < identity.byte_size) {
            if (stop.stop_requested()) {
                return Result::failure(ProjectAssetImportError{
                    ProjectAssetImportErrorCode::Cancelled, {}});
            }
            const std::size_t wanted = static_cast<std::size_t>(
                std::min<core::u64>(identity.byte_size - offset,
                                    bytes.size()));
            input.read(reinterpret_cast<char*>(bytes.data()),
                       static_cast<std::streamsize>(wanted));
            if (stop.stop_requested()) {
                return Result::failure(ProjectAssetImportError{
                    ProjectAssetImportErrorCode::Cancelled, {}});
            }
            if (input.gcount() != static_cast<std::streamsize>(wanted)) {
                return Result::failure(ProjectAssetImportError{
                    ProjectAssetImportErrorCode::SourceReadFailed, {}});
            }
            auto appended = sink.append(
                offset, std::span<const std::byte>{bytes.data(), wanted});
            if (stop.stop_requested()) {
                return Result::failure(ProjectAssetImportError{
                    ProjectAssetImportErrorCode::Cancelled, {}});
            }
            if (!appended) {
                return Result::failure(transfer_failure(
                    ProjectAssetImportErrorCode::StoreWriteFailed,
                    appended.error()));
            }
            offset = appended.value();
        }
    }

    if (stop.stop_requested()) {
        return Result::failure(ProjectAssetImportError{
            ProjectAssetImportErrorCode::Cancelled, {}});
    }
    auto finalized = sink.finalize(stop);
    if (stop.stop_requested()) {
        return Result::failure(ProjectAssetImportError{
            ProjectAssetImportErrorCode::Cancelled, {}});
    }
    if (!finalized) {
        return Result::failure(transfer_failure(
            ProjectAssetImportErrorCode::StoreFinalizeFailed,
            finalized.error()));
    }
    return Result::success(std::move(finalized).value());
}

[[nodiscard]] std::string_view policy_name(
    assets::AssetDistributionPolicy policy) noexcept {
    switch (policy) {
    case assets::AssetDistributionPolicy::External:
        return assets::schema::policy_external;
    case assets::AssetDistributionPolicy::ProjectRelative:
        return assets::schema::policy_project_relative;
    case assets::AssetDistributionPolicy::SharedStore:
        return assets::schema::policy_shared_store;
    }
    return assets::schema::policy_external;
}

} // namespace

ProjectAssetImportResult prepare_project_asset_import(
    assets::Asset asset, const std::filesystem::path& shared_store,
    std::stop_token stop) {
    try {
        if (stop.stop_requested()) {
            return cancelled_import();
        }
        const std::string source{assets::asset_source(asset)};
        if (source.empty()) {
            return import_failure(ProjectAssetImportErrorCode::InvalidSource);
        }
        const std::filesystem::path source_path = path_from_utf8(source);
        if (stop.stop_requested()) {
            return cancelled_import();
        }
        auto identity = assets::hash_file(source_path, stop);
        if (!identity) {
            if (identity.error().code ==
                assets::ContentIntegrityErrorCode::Cancelled) {
                return cancelled_import();
            }
            return import_failure(
                ProjectAssetImportErrorCode::ContentIdentityFailed,
                identity.error().system_error);
        }
        if (stop.stop_requested()) {
            return cancelled_import();
        }
        if (identity.value().byte_size == 0U ||
            identity.value().byte_size > static_cast<core::u64>(
                std::numeric_limits<std::int64_t>::max())) {
            return import_failure(ProjectAssetImportErrorCode::ContentTooLarge);
        }

        assets::AssetContent& content = mutable_content(asset);
        content.identity = identity.value();
        content.policy = assets::asset_requires_dependency_manifest(asset)
            ? assets::AssetDistributionPolicy::External
            : assets::AssetDistributionPolicy::SharedStore;

        if (content.policy == assets::AssetDistributionPolicy::SharedStore) {
            auto installed = install_shared_store(
                source_path, source, *content.identity,
                shared_store, stop);
            if (!installed) {
                return ProjectAssetImportResult::failure(
                    std::move(installed).error());
            }
        }
        if (stop.stop_requested()) {
            return cancelled_import();
        }
        return ProjectAssetImportResult::success(std::move(asset));
    } catch (const std::bad_alloc&) {
        return import_failure(ProjectAssetImportErrorCode::AllocationFailed);
    } catch (const std::filesystem::filesystem_error& error) {
        return import_failure(ProjectAssetImportErrorCode::SourceReadFailed,
                              error.code());
    }
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
            node.properties.emplace(
                std::string{assets::schema::distribution_policy},
                doc::PropertyValue{std::string{policy_name(
                    asset.content.policy)}});
            if (asset.content.identity.has_value()) {
                node.properties.emplace(
                    std::string{assets::schema::content_sha256},
                    doc::PropertyValue{assets::format_sha256(
                        asset.content.identity->sha256)});
                node.properties.emplace(
                    std::string{assets::schema::content_size_bytes},
                    integer(asset.content.identity->byte_size));
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
