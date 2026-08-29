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

} // namespace mediaserver::scene
