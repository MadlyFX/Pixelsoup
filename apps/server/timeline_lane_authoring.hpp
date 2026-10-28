#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene.hpp>

#include <string>
#include <vector>

namespace mediaserver::app {

enum class LaneAuthorAction : core::u8 {
    Create,
    Rename,
    MoveEarlier,
    MoveLater,
    MoveTo,
    ToggleMute,
    ToggleLock,
    ToggleSyncLock,
    DeleteEmpty,
    DeleteClips,
    DeleteMoveClips,
};

struct LaneAuthorRequest final {
    LaneAuthorAction action{LaneAuthorAction::Create};
    scene::SceneId scene{};
    doc::NodeId lane{};
    doc::NodeId destination{};
    std::string name;

    bool operator==(const LaneAuthorRequest&) const = default;
};

enum class LaneAuthorError : core::u8 {
    InvalidScene,
    LaneNotFound,
    DestinationNotFound,
    CrossSceneDestination,
    InvalidName,
    NonEmptyLane,
    LaneLimit,
    IdExhausted,
    InvalidDocument,
    AllocationFailed,
};

struct LaneAuthorPlan final {
    doc::Root document;
    std::vector<doc::NodeId> changed;
    std::vector<doc::NodeId> erased;
    doc::NodeId created{};
    core::u64 next_node{};
};

// Plans one complete lane operation without mutating Director state. The
// caller schedules `document` as one ordinary Scene undo transaction. Delete
// never guesses: a non-empty lane needs DeleteClips or an explicit destination.
[[nodiscard]] core::Result<LaneAuthorPlan, LaneAuthorError>
plan_lane_authoring(const doc::Root& document,
                    const LaneAuthorRequest& request,
                    core::u64 next_node);

} // namespace mediaserver::app
