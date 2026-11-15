#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace mediaserver::scene {

// Planning is deliberately separate from authoring. The caller can wrap the
// returned puts in its own command/undo transaction and choose when to
// schedule it; this layer neither mutates the root nor knows a transport.
enum class SceneCloneError {
    SourceNotFound,
    InvalidFirstId,
    IdExhausted,
    AllocationFailed,
};

struct SceneClonePlan final {
    SceneId source{};
    SceneId destination{};
    // New nodes in ascending original-node-id order. Applying these as puts
    // produces the complete clone; none exist in `root` when planning returns.
    std::vector<doc::Node> puts;
    // (original, clone), in the same deterministic order as puts.
    std::vector<std::pair<doc::NodeId, doc::NodeId>> remap;

    [[nodiscard]] std::optional<doc::NodeId> cloned_id(
        doc::NodeId original) const noexcept;
};

// Allocates ascending ids beginning at `first_id`, skipping every id already
// occupied in root. Generated ids stay within signed 64-bit range because
// document reference properties are signed integers.
[[nodiscard]] core::Result<SceneClonePlan, SceneCloneError> plan_clone_scene(
    const doc::Root& root,
    SceneId source,
    doc::NodeId first_id);

enum class SceneDeleteError {
    SceneNotFound,
    AllocationFailed,
};

struct SceneDeletePlan final {
    SceneId scene{};
    // The stage and every scene-owned dependent node, in ascending node-id
    // order. This includes editor metadata and compositor objects that carry
    // the shared scene_id ownership property, plus their dependent records.
    std::vector<doc::NodeId> erases;
};

// Plans a complete scene removal without mutating `root`. Assets remain in the
// project library: placements are scene-owned, while asset records are not.
[[nodiscard]] core::Result<SceneDeletePlan, SceneDeleteError>
plan_delete_scene(const doc::Root& root, SceneId scene);

enum class EffectDeletePlanError {
    EffectNotFound,
    NotAnEffect,
    AllocationFailed,
};

struct EffectDeletePlan final {
    // Animation channels first in ascending node-id order, followed by the
    // effect itself. Authoring every erase in one transaction makes removal
    // and restoration atomic from the operator's point of view.
    std::vector<doc::NodeId> erases;
};

// Plans the referential cascade for a first-class scene.effect. Only channels
// whose channel_target is exactly `effect` are included; channels belonging to
// the drawable or another effect remain untouched.
[[nodiscard]] core::Result<EffectDeletePlan, EffectDeletePlanError>
plan_delete_effect(const doc::Root& root, doc::NodeId effect);

struct ElementDeletePlan final {
    // Every record that only exists to describe the element (its effects,
    // masks, mask points, animation channels and selection-set memberships,
    // transitively), in ascending node-id order, followed by the element.
    std::vector<doc::NodeId> erases;
};

// Plans the referential cascade for deleting one scene element without
// mutating `root`. Without it the element's effects stay behind in the
// document pointing at an id that no longer exists, and come back to life when
// a later element is given that id (a project reload restarts id allocation).
[[nodiscard]] core::Result<ElementDeletePlan, EffectDeletePlanError>
plan_delete_element(const doc::Root& root, doc::NodeId element);

enum class GroupEditError {
    InvalidScene,
    EmptySelection,
    DuplicateSelection,
    UnsupportedNode,
    CrossSceneSelection,
    AlreadyGrouped,
    Locked,
    InvalidName,
    InvalidFirstId,
    IdExhausted,
    GroupNotFound,
    NotAGroup,
    InvalidHierarchy,
    AnimatedTransform,
    TransformNotRepresentable,
    VisibilityNotRepresentable,
    OrderOverflow,
    AllocationFailed,
};

// A pure authoring plan. `after` is ready for one Director
// schedule_document_replace call, which makes all puts/erases one undo step.
// The explicit lists are retained for command UIs, tests, and callers that
// author a typed PutNodes batch directly. Both lists are node-id sorted.
struct GroupEditPlan final {
    SceneId scene{};
    doc::NodeId group{};
    doc::Root after;
    std::vector<doc::Node> puts;
    std::vector<doc::NodeId> erases;
    std::optional<doc::NodeId> primary_after;
};

// Wraps selected, unlocked, top-level object/media/model/group nodes in one
// new identity group. The first free signed-reference id at or after first_id
// is allocated, and children receive dense group_order in deterministic
// (prior order,id) order. Identity parenting preserves every world transform,
// including animated children, without baking a frame.
[[nodiscard]] core::Result<GroupEditPlan, GroupEditError> plan_group_nodes(
    const doc::Root& root,
    SceneId scene,
    std::span<const doc::NodeId> selection,
    doc::NodeId first_id,
    std::string_view name = "Group");

// Authors one new, empty identity group - a folder, or another GroupKind -
// under `parent`, or at the scene's top level without one. It lands in front
// of its new siblings. Refused for a parent that is missing, not a group, a
// linked Blender collection, locked, or in another scene.
[[nodiscard]] core::Result<GroupEditPlan, GroupEditError> plan_create_group(
    const doc::Root& root,
    SceneId scene,
    std::optional<doc::NodeId> parent,
    doc::NodeId first_id,
    std::string_view name = "Group",
    GroupKind kind = GroupKind::Folder);

// Removes one group and reparents its direct children to the old parent. Each
// child's new local is `group local * child local`; the plan rejects animated
// transforms or matrices that this TRS schema cannot represent (notably
// shear), rather than changing the picture. Deleting a group intentionally
// has these same preserve-children semantics.
[[nodiscard]] core::Result<GroupEditPlan, GroupEditError> plan_ungroup_nodes(
    const doc::Root& root, SceneId scene, doc::NodeId group);
[[nodiscard]] core::Result<GroupEditPlan, GroupEditError>
plan_delete_group_preserving_children(
    const doc::Root& root, SceneId scene, doc::NodeId group);

// Files `selection` under `group` - or at the scene's top level when `group`
// is empty - without moving anything on the stage: each node's new local
// transform is the one that puts it exactly where it already is under its new
// parent. The moved nodes land beside `anchor` (a sibling in the destination;
// `before` meaning in front of it) or, without one, at the front, and the
// destination's siblings are renumbered densely front to back.
//
// Refused rather than approximated: a node whose own transform, or whose old
// or new parents' transforms, are animated while its parent space changes; a
// world transform the TRS schema cannot represent; a group moved into itself
// or its own descendant; locked nodes or destinations; clips filed on a
// track, whose order belongs to the track rather than the hierarchy.
[[nodiscard]] core::Result<GroupEditPlan, GroupEditError> plan_move_into_group(
    const doc::Root& root,
    SceneId scene,
    std::span<const doc::NodeId> selection,
    std::optional<doc::NodeId> group,
    std::optional<doc::NodeId> anchor = std::nullopt,
    bool before = true);

// The layer list is a front-to-back editor projection of two existing
// document orders. Clips filed in one explicit timeline track use clip_order;
// ordinary scene-layer siblings use group_order. Keeping this distinction
// here prevents a row drag from accidentally becoming a hierarchy reparent or
// a track assignment.
enum class LayerOrderPlacement {
    Before,
    After,
};

enum class LayerOrderError {
    InvalidScene,
    SourceNotFound,
    DestinationNotFound,
    UnsupportedNode,
    CrossScene,
    CrossOrderDomain,
    InvalidHierarchy,
    Locked,
    OrderOverflow,
    AllocationFailed,
};

struct LayerOrderPlan final {
    SceneId scene{};
    doc::NodeId source{};
    doc::NodeId destination{};
    doc::Root after;
    // Only nodes whose dense authored order changed, sorted by node id.
    std::vector<doc::Node> puts;
};

// Moves `source` before/after `destination` in the visible front-to-back
// layer list. The planner validates that both are peers in one order domain,
// then rewrites that complete peer order densely in one atomic plan.
[[nodiscard]] core::Result<LayerOrderPlan, LayerOrderError>
plan_reorder_layer(const doc::Root& root, SceneId scene,
                   doc::NodeId source, doc::NodeId destination,
                   LayerOrderPlacement placement);

} // namespace mediaserver::scene
