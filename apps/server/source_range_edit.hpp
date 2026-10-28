#pragma once

#include "timeline_edit_integration.hpp"

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/editor/editor_shell.hpp>

#include <span>

namespace mediaserver::app {

enum class SourceRangeEditError : core::u8 {
    // The source has no probed frame count, so a range cannot be checked.
    NotProbed,
    // The range is empty or runs past the source's last frame.
    RangeOutsideSource,
    // The track is locked, or no longer in the scene.
    TargetLaneUnavailable,
    // Clips moved or a handle ran out between the request and now.
    TimelineRejected,
    AllocationFailed,
};

// The Asset Viewer's marked range as one undoable document replacement.
//
// `placement` is the whole-source scene.media node an ordinary Library add
// of `asset` would author (make_added_node). The range trims it: its first
// show frame shows `source_in`, and it runs for source_span_show_frames of
// the range - every frame marked, at the show rate, by playback's own floor
// mapping. A still keeps the placement's duration. The trimmed clip then
// goes through exactly the planning a clipboard Insert or Overwrite takes,
// so sync-locked and locked tracks behave the same, and Overwrite lays it over
// the track in front of every clip already there, cutting and moving
// nothing. Insert at a point inside a clip first cuts that clip there, as the
// Razor does, so its second half moves along with the rest of the track.
// Append overwrites from the lane's last clip end, which moves nothing.
[[nodiscard]] core::Result<TimelineEditIntegrationPlan, SourceRangeEditError>
plan_source_range_edit(const doc::Root& document,
                       std::span<const scene::ClipPlacement> clips,
                       const assets::MediaAsset& asset,
                       const editor::SourceRangeEditIntent& request,
                       doc::Node placement, core::u64 next_node);

// Where Append lands on `lane`: the end of its last clip, or frame zero.
[[nodiscard]] core::u64 source_append_frame(
    std::span<const scene::ClipPlacement> clips, scene::SceneId scene,
    doc::NodeId lane) noexcept;

} // namespace mediaserver::app
