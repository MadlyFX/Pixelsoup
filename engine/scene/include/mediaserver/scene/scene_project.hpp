#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>
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

} // namespace mediaserver::scene
