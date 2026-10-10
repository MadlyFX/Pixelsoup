#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene.hpp>

#include <cstddef>

namespace mediaserver::app {

// Bounded like tracks: markers are replicated document content, and a show
// with more named places than this is a show with a cue list nobody can run.
inline constexpr std::size_t max_scene_markers = 1'024U;

enum class MarkerAuthorAction : core::u8 {
    Create,
    Update,
    Delete,
};

// One marker operation. The marker travels whole, because it is one document
// node: an Update replaces every authored field of `marker.id`, which is what
// lets a drag, a rename, and a turn into a cue all be the same request. Create
// ignores `marker.id`; Delete reads nothing else.
struct MarkerAuthorRequest final {
    MarkerAuthorAction action{MarkerAuthorAction::Create};
    scene::SceneId scene{};
    scene::TimelineMarker marker{};

    bool operator==(const MarkerAuthorRequest&) const = default;
};

enum class MarkerAuthorError : core::u8 {
    InvalidScene,
    MarkerNotFound,
    CrossSceneMarker,
    InvalidFrame,
    InvalidName,
    InvalidNote,
    InvalidColor,
    InvalidLoopTiming,
    MarkerLimit,
    IdExhausted,
    AllocationFailed,
};

struct MarkerAuthorPlan final {
    doc::Root document;
    // The marker created, updated, or deleted.
    doc::NodeId marker{};
    core::u64 next_node{};
};

// Plans one marker operation without mutating Director state. The caller
// schedules `document` as one ordinary Scene undo transaction, so moving a cue
// undoes like moving a clip.
[[nodiscard]] core::Result<MarkerAuthorPlan, MarkerAuthorError>
plan_marker_authoring(const doc::Root& document,
                      const MarkerAuthorRequest& request,
                      core::u64 next_node);

} // namespace mediaserver::app
