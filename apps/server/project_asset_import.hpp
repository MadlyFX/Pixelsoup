#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>

#include <optional>

namespace mediaserver::app {

enum class ProjectAssetImportErrorCode {
    // The chosen file is not an absolute path, or contains a '.'/'..'
    // component that would resolve differently per working directory.
    InvalidSource,
    // Nothing openable lives at that path on this machine right now.
    SourceMissing,
};

struct ProjectAssetImportError final {
    ProjectAssetImportErrorCode code{
        ProjectAssetImportErrorCode::InvalidSource};

    bool operator==(const ProjectAssetImportError&) const = default;
};

using ProjectAssetImportResult =
    core::Result<assets::Asset, ProjectAssetImportError>;

// Importing records a reference, never a copy: the authored absolute path is
// the asset's only content locator, and every render machine opens that same
// path. The check is therefore one stat, so import stays instant for a 40 GB
// clip and the project never grows a second copy of the media.
//
// Presence is checked here only to fail an obviously wrong pick while the
// operator still has the file dialog in mind. It is not a durable guarantee:
// per-machine readiness is reported continuously by each node instead.
[[nodiscard]] ProjectAssetImportResult prepare_project_asset_import(
    assets::Asset asset);

// One PutNode carries source and probe metadata together, so callers schedule
// a single command instead of exposing a partially written asset record to the
// replicated document.
[[nodiscard]] doc::Node make_project_asset_node(
    doc::NodeId id, const assets::Asset& asset);

struct ProjectAssetNodeUpsert final {
    doc::Node after;
    std::optional<doc::Node> before;

    bool operator==(const ProjectAssetNodeUpsert&) const = default;
};

// Reimport keeps the lowest existing exact-(kind, source) NodeId. Scene
// placements retain that stable identity while all probe metadata is replaced
// by the newly probed import in one PutNode. Distinct roles for the same file
// remain separate records.
[[nodiscard]] ProjectAssetNodeUpsert make_project_asset_upsert(
    const doc::Root& root, const assets::Asset& asset,
    doc::NodeId allocated_id);

} // namespace mediaserver::app
