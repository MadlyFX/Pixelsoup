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
    // Razors the track into two at `frame`. See plan_lane_authoring.
    Split,
};

struct LaneAuthorRequest final {
    LaneAuthorAction action{LaneAuthorAction::Create};
    scene::SceneId scene{};
    doc::NodeId lane{};
    doc::NodeId destination{};
    std::string name;
    // Split only: the first frame of the new track, and the show rate the
    // source frame visible at the cut is worked out at.
    core::i64 frame{};
    scene::MediaFrameRate show_rate{};

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
    // Split: the lane, or a clip the cut has to change, is locked.
    Locked,
    // Split: the cut leaves one of the two tracks with nothing on it.
    NothingToSplit,
    // Split: a vehicle track and its slot screens are one car, and half a
    // car is not a track. Its takes are cut with the clip razor.
    VehicleTrack,
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
//
// Split is a razor for a whole track, the way a clip's razor is for one clip:
// the track keeps what plays before the cut and a new track directly in front
// of it - the same screen, look, effects and keyframes - takes what plays from
// the cut on. A clip straddling the cut is split, its tail going to the new
// track. Both tracks must end up holding something; a cut outside the track's
// clips would only be a move, and is refused rather than performed as one.
//
// Deleting a vehicle track deletes its slot screens with it.
[[nodiscard]] core::Result<LaneAuthorPlan, LaneAuthorError>
plan_lane_authoring(const doc::Root& document,
                    const LaneAuthorRequest& request,
                    core::u64 next_node);

} // namespace mediaserver::app
