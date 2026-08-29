#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Named Selection Sets are replicated document metadata, but recalling one is
// deliberately not a document edit. The set and its ordered membership travel
// with the show; the resulting selection remains local to each editor.
namespace mediaserver::scene {

namespace selection_set_schema {

inline constexpr std::string_view set_type = "scene.selection_set";
inline constexpr std::string_view member_type =
    "scene.selection_set_member";
inline constexpr std::string_view name = "selection_set_name";
inline constexpr std::string_view set_id = "selection_set_id";
inline constexpr std::string_view target_id = "selection_set_target";
inline constexpr std::string_view member_order = "selection_set_order";

} // namespace selection_set_schema

// Selection Sets are editor metadata, but they still live in the replicated
// document and therefore need explicit resource bounds. Names are UTF-8 text
// and this cap counts encoded bytes, matching the document carrier.
inline constexpr std::size_t max_selection_set_name_bytes = 128U;
inline constexpr std::size_t max_selection_set_members = 256U;

enum class SelectionSetErrorCode : core::u8 {
    InvalidScene,
    InvalidName,
    NameTooLong,
    EmptySelection,
    MemberCapacity,
    InvalidFirstId,
    IdExhausted,
    SetNotFound,
    NotASelectionSet,
    InvalidSet,
    CrossSceneSet,
    TargetMissing,
    UnsupportedTarget,
    CrossSceneTarget,
    TargetLocked,
    InvalidTarget,
    InvalidReference,
    OrderOverflow,
    AllocationFailed,
};

struct SelectionSetError final {
    SelectionSetErrorCode code{SelectionSetErrorCode::InvalidSet};
    // The set, target, or requested first id that caused the failure.
    doc::NodeId subject{};

    bool operator==(const SelectionSetError&) const = default;
};

struct SelectionSetMember final {
    doc::NodeId node{};
    doc::NodeId target{};
    core::i64 order{};

    bool operator==(const SelectionSetMember&) const = default;
};

struct SelectionSetInfo final {
    doc::NodeId node{};
    SceneId scene{};
    std::string name;
    // Persisted order, with member node id as the deterministic tie-break.
    std::vector<SelectionSetMember> members;

    bool operator==(const SelectionSetInfo&) const = default;
};

struct SelectionSetSummary final {
    doc::NodeId node{};
    SceneId scene{};
    std::string name;
    std::size_t member_count{};
    // Members with no decodable target reference or whose target node no
    // longer exists. Locked/incompatible existing targets are not mislabeled
    // as missing; recall reports those separately.
    std::size_t missing_count{};

    bool operator==(const SelectionSetSummary&) const = default;
};

struct SelectionSetCatalog final {
    // Set-node-id order, which is stable across every replica.
    std::vector<SelectionSetSummary> sets;
    // Malformed set nodes explicitly owned by the requested scene.
    std::vector<SelectionSetError> diagnostics;
};

// One replacement-ready authoring result. `puts` and `erases` are
// independently node-id sorted and never overlap. Scheduling `after` as one
// document replacement gives create/replace/rename/delete one undo step.
struct SelectionSetPlan final {
    doc::NodeId set{};
    doc::Root after;
    std::vector<doc::Node> puts;
    std::vector<doc::NodeId> erases;
    std::vector<SelectionSetMember> members;
    std::size_t duplicate_inputs{};
};

enum class SelectionSetRecallSkipReason : core::u8 {
    MalformedMember,
    MissingTarget,
    UnsupportedTarget,
    CrossSceneTarget,
    LockedTarget,
    InvalidTarget,
    DuplicateTarget,
};

struct SelectionSetRecallSkip final {
    doc::NodeId member{};
    std::optional<doc::NodeId> target;
    SelectionSetRecallSkipReason reason{
        SelectionSetRecallSkipReason::MalformedMember};

    bool operator==(const SelectionSetRecallSkip&) const = default;
};

struct SelectionSetRecall final {
    SelectionSetInfo set;
    // Valid, unlocked editor-local targets in persisted member order.
    std::vector<doc::NodeId> targets;
    std::optional<doc::NodeId> primary;
    std::vector<SelectionSetRecallSkip> skips;
};

using SelectionSetPlanResult =
    core::Result<SelectionSetPlan, SelectionSetError>;
using SelectionSetRecallResult =
    core::Result<SelectionSetRecall, SelectionSetError>;
using SelectionSetCatalogResult =
    core::Result<SelectionSetCatalog, SelectionSetError>;

// Read-only scene-local catalog for editor panels. It performs no selection
// recall and never changes document/history state.
[[nodiscard]] SelectionSetCatalogResult selection_sets(
    const doc::Root& document, SceneId scene);

// Creates one set node followed by dense ordered member nodes. Input
// membership is canonicalized to unique ascending target ids so allocation
// never depends on click order. Every target must be a selectable, unlocked
// element owned by `scene`; authoring failures are atomic.
[[nodiscard]] SelectionSetPlanResult plan_create_selection_set(
    const doc::Root& document, SceneId scene, std::string_view name,
    std::span<const doc::NodeId> targets, doc::NodeId first_id);

// Replaces membership without changing set identity or name. Existing member
// ids are retained for the same target where possible, then recycled in
// ascending id order; only excess members allocate from `first_id`.
[[nodiscard]] SelectionSetPlanResult plan_replace_selection_set(
    const doc::Root& document, SceneId scene, doc::NodeId set,
    std::span<const doc::NodeId> targets, doc::NodeId first_id);

[[nodiscard]] SelectionSetPlanResult plan_rename_selection_set(
    const doc::Root& document, SceneId scene, doc::NodeId set,
    std::string_view name);

// Deletes the set and every member node whose typed set reference points to
// it. Targets are editor content and are never deleted.
[[nodiscard]] SelectionSetPlanResult plan_delete_selection_set(
    const doc::Root& document, SceneId scene, doc::NodeId set);

// Resolves persisted members without changing `document`. Missing, locked,
// cross-scene, incompatible, malformed, and duplicate targets are reported and
// skipped; primary is the first surviving member.
[[nodiscard]] SelectionSetRecallResult recall_selection_set(
    const doc::Root& document, SceneId scene, doc::NodeId set);

} // namespace mediaserver::scene
