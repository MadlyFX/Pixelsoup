#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>

#include <optional>
#include <compare>
#include <string>
#include <vector>

namespace mediaserver::app {

enum class ProjectAssetImportErrorCode {
    // The chosen file is not an absolute path, or contains a '.'/'..'
    // component that would resolve differently per working directory.
    InvalidSource,
    // Nothing openable lives at that path on this machine right now.
    SourceMissing,
    // The source existed but could not be read consistently to completion.
    FingerprintFailed,
};

struct ProjectAssetImportError final {
    ProjectAssetImportErrorCode code{
        ProjectAssetImportErrorCode::InvalidSource};

    bool operator==(const ProjectAssetImportError&) const = default;
};

using ProjectAssetImportResult =
    core::Result<assets::Asset, ProjectAssetImportError>;

// Importing records a reference, never a copy: the authored absolute path is
// the content locator, while a streaming length/SHA-256 fingerprint proves
// which revision every render machine opened. No media bytes enter the project.
//
// Presence and a stable full-file read are checked here while the operator has
// the file dialog in mind. Per-machine readiness subsequently verifies the
// same expected fingerprint; this import step is not a durable availability
// guarantee.
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

// Library removal and revision changes are planned against the exact sealed
// document rather than inferred from the evaluated registry. This keeps Asset
// NodeId identity stable and makes every reference blocking a destructive
// removal visible to the operator.
struct ProjectAssetReference final {
    doc::NodeId node{};
    std::string property;

    auto operator<=>(const ProjectAssetReference&) const = default;
};

enum class ProjectAssetAuthoringErrorCode {
    AssetNotFound,
    AssetKindMismatch,
    InvalidPreparedAsset,
    SourceMismatch,
    InUse,
    AllocationFailed,
};

struct ProjectAssetAuthoringError final {
    ProjectAssetAuthoringErrorCode code{
        ProjectAssetAuthoringErrorCode::AssetNotFound};
    std::vector<ProjectAssetReference> references;

    bool operator==(const ProjectAssetAuthoringError&) const = default;
};

struct ProjectAssetDeletePlan final {
    doc::Root after;
    doc::Node erased;

    bool operator==(const ProjectAssetDeletePlan&) const = default;
};

using ProjectAssetDeleteResult =
    core::Result<ProjectAssetDeletePlan, ProjectAssetAuthoringError>;

// Refuses deletion while any scene placement or projection surface carries an
// exact reference to the Asset id. Callers may show error.references, but may
// not silently cascade or leave a placement with an invented replacement id.
[[nodiscard]] ProjectAssetDeleteResult plan_project_asset_delete(
    const doc::Root& root, doc::NodeId asset);

enum class ProjectAssetRevisionMode {
    // The chosen bytes must still live at the authored source path.
    AcceptCurrentSource,
    // The prepared replacement may carry a new absolute locator.
    Relink,
};

struct ProjectAssetRevisionPlan final {
    doc::Root after;
    doc::Node before;
    doc::Node changed;
    bool modified{};

    bool operator==(const ProjectAssetRevisionPlan&) const = default;
};

using ProjectAssetRevisionResult =
    core::Result<ProjectAssetRevisionPlan, ProjectAssetAuthoringError>;

// `replacement` is a fully probed/fingerprinted import candidate. The plan
// keeps the existing NodeId, display name, proxy request/policy, and unknown
// properties while replacing only content-locator and typed probe metadata.
[[nodiscard]] ProjectAssetRevisionResult plan_project_asset_revision(
    const doc::Root& root, doc::NodeId asset,
    const assets::Asset& replacement, ProjectAssetRevisionMode mode);

} // namespace mediaserver::app
