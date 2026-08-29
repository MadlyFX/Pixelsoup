#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/timeline_edit.hpp>

#include <vector>

namespace mediaserver::app {

enum class TimelineEditAuthorError : core::u8 {
    InvalidPlan,
    ClipNotFound,
    NotMediaClip,
    LockedClip,
    LockedLane,
    IdExhausted,
    InvalidDocument,
    AllocationFailed,
};

struct TimelineEditAuthorPlan final {
    // Complete replacement snapshot. Scheduling this once through
    // DirectorScheduler makes every clip and subgraph mutation one history
    // transaction.
    doc::Root document;
    // Existing media nodes replaced from the planner, sorted by id.
    std::vector<doc::NodeId> changed;
    // Clips plus their dependent effects/channels, sorted by id.
    std::vector<doc::NodeId> erased;
    // Split media remainders and their cloned effect/channel subgraphs, sorted
    // by newly allocated id.
    std::vector<doc::Node> created;
    core::u64 next_node{};
};

// Converts one pure scene::TimelineEditPlan into one replacement document.
// Existing media nodes retain every property outside ClipPlacement (including
// asset identity, transform, layer style, and properties outside the planner's
// ownership). Split media
// are full clones of their source placement with planner-owned placement
// fields applied; first-class effects and channels are cloned transitively and
// retargeted. Channel key frames remain at their absolute show ordinals, which
// is the zero-offset time rewrite required by an in-place overwrite split. A
// planner create id is an allocation-order hint because the pure planner sees
// clips rather than every document node; collisions are advanced from
// `next_node` in ascending order at this document boundary.
[[nodiscard]] core::Result<TimelineEditAuthorPlan, TimelineEditAuthorError>
plan_timeline_edit_authoring(
    const doc::Root& document,
    const scene::TimelineEditPlan& edit,
    core::u64 next_node);

} // namespace mediaserver::app
