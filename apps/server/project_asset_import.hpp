#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>

#include <filesystem>
#include <optional>
#include <stop_token>
#include <system_error>

namespace mediaserver::app {

enum class ProjectAssetImportErrorCode {
    InvalidSource,
    ContentIdentityFailed,
    ContentTooLarge,
    SharedStoreRequired,
    StoreOpenFailed,
    SourceReadFailed,
    StoreWriteFailed,
    StoreFinalizeFailed,
    Cancelled,
    AllocationFailed,
};

struct ProjectAssetImportError final {
    ProjectAssetImportErrorCode code{
        ProjectAssetImportErrorCode::InvalidSource};
    std::error_code system_error;

    bool operator==(const ProjectAssetImportError&) const = default;
};

using ProjectAssetImportResult =
    core::Result<assets::Asset, ProjectAssetImportError>;

// Computes the stable identity before returning any authorable value. A
// self-contained asset is copied through the verified SharedStore sink; a
// dependency-capable source retains its primary-file identity but stays
// External until package manifests are implemented.
[[nodiscard]] ProjectAssetImportResult prepare_project_asset_import(
    assets::Asset asset, const std::filesystem::path& shared_store,
    std::stop_token stop = {});

// One PutNode carries source, probe, identity, and policy together. Callers can
// therefore schedule a single command instead of exposing a partially updated
// manifest entry to the replicated document.
[[nodiscard]] doc::Node make_project_asset_node(
    doc::NodeId id, const assets::Asset& asset);

struct ProjectAssetNodeUpsert final {
    doc::Node after;
    std::optional<doc::Node> before;

    bool operator==(const ProjectAssetNodeUpsert&) const = default;
};

// Reimport keeps the lowest existing exact-(kind, source) NodeId. Scene
// placements retain that stable identity while all probe/content metadata is
// replaced by the newly verified import in one PutNode. Distinct roles for the
// same file remain separate records.
[[nodiscard]] ProjectAssetNodeUpsert make_project_asset_upsert(
    const doc::Root& root, const assets::Asset& asset,
    doc::NodeId allocated_id);

} // namespace mediaserver::app
