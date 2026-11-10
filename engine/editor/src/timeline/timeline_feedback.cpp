#include "timeline_internal.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What a Timeline drag tells the operator while it runs: how far it has gone
// and where it lands, in the ruler's units, and what a snap lined it up with.
// The chip itself is drawn with the rest of the drag feedback.
namespace mediaserver::editor {
using namespace timeline;

std::string TimelinePanel::signed_duration(Pass &p, core::i64 frames) {
  const core::u64 magnitude =
      frames < 0 ? static_cast<core::u64>(-(frames + 1)) + 1U
                 : static_cast<core::u64>(frames);
  return (frames < 0 ? "-" : "+") +
         compact_timecode(timecode(p, magnitude), true);
}

std::string TimelinePanel::snap_name(Pass &p, core::i64 frame) {
  if (frame == p.shown_playhead) {
    return "playhead";
  }
  if (frame == p.first_i) {
    return "In";
  }
  // A clip's exclusive end and Out's own frame both line up with Out.
  if (frame == p.last_i || frame == p.last_i + 1) {
    return "Out";
  }
  for (const scene::TimelineMarker &marker : p.input.markers) {
    if (marker.frame == frame) {
      return marker.name.empty() ? (marker.is_cue() ? "cue" : "marker")
                                 : marker.name;
    }
  }
  return "clip edge";
}

std::string TimelinePanel::drag_readout(Pass &p) {
  // A position in the ruler's units, signed for a clip parked before zero.
  const auto at = [&](core::i64 frame) {
    return frame < 0 ? "-" + timecode(p, static_cast<core::u64>(-frame))
                     : timecode(p, static_cast<core::u64>(frame));
  };
  const auto duration = [&](core::i64 frames) {
    return compact_timecode(
        timecode(p, static_cast<core::u64>(std::max<core::i64>(frames, 0))),
        true);
  };
  std::string text;
  std::optional<core::i64> guide;
  if (clip_.drag != ClipDrag::None && p.clip_drag_active) {
    const scene::ClipTiming &from = clip_.origin;
    const scene::ClipTiming &to = clip_.proposed;
    const core::i64 from_end = from.start + from.duration;
    const core::i64 to_end = to.start + to.duration;
    switch (clip_.drag) {
    case ClipDrag::Move:
      text = signed_duration(p, to.start - from.start) + "  >  " +
             at(to.start);
      break;
    case ClipDrag::TrimIn:
      text = "IN " + signed_duration(p, to.start - from.start) + "  >  " +
             at(to.start) + "   LENGTH " + duration(to.duration);
      break;
    case ClipDrag::TrimOut:
      // Out is the clip's last frame, the one an editor's Out names.
      text = "OUT " + signed_duration(p, to_end - from_end) + "  >  " +
             at(to_end - 1) + "   LENGTH " + duration(to.duration);
      break;
    case ClipDrag::FadeIn:
      text = "FADE IN " + duration(to.fade_in);
      break;
    case ClipDrag::FadeOut:
      text = "FADE OUT " + duration(to.fade_out);
      break;
    case ClipDrag::RippleTrimHead:
      text = "RIPPLE IN " + signed_duration(p, clip_.delta) + "   LENGTH " +
             duration(to.duration);
      break;
    case ClipDrag::RippleTrimTail:
      text = "RIPPLE OUT " + signed_duration(p, clip_.delta) + "   LENGTH " +
             duration(to.duration);
      break;
    case ClipDrag::Roll: {
      // The cut is the dragged clip's end when it is the left of the pair,
      // its start otherwise.
      const ElementInfo *const peer =
          find_element(p.input.elements, clip_.peer);
      const bool left = peer != nullptr && from.start <= peer->timing.start;
      text = "ROLL " + signed_duration(p, clip_.delta) + "  >  cut at " +
             at(left ? to_end : to.start);
      break;
    }
    case ClipDrag::Slip:
      text = "SLIP " + signed_duration(p, clip_.delta);
      break;
    case ClipDrag::Slide:
      text = "SLIDE " + signed_duration(p, clip_.delta) + "  >  " +
             at(to.start);
      break;
    case ClipDrag::None:
      break;
    }
    guide = clip_.snap_guide;
  } else if (track_.drag != ClipDrag::None && track_.moved) {
    switch (track_.drag) {
    case ClipDrag::TrimIn:
      text = "TRACK IN " + signed_duration(p, track_.delta) + "  >  " +
             at(track_.first + track_.delta);
      break;
    case ClipDrag::TrimOut:
      text = "TRACK OUT " + signed_duration(p, track_.delta) + "  >  " +
             at(track_.end + track_.delta - 1);
      break;
    default:
      text = "TRACK " + signed_duration(p, track_.delta) + "  >  " +
             at(track_.first + track_.delta);
      break;
    }
    if (track_.destination != doc::NodeId{}) {
      for (const scene::TimelineLane &lane : p.input.lanes) {
        if (lane.id == track_.destination && !lane.implicit) {
          text += "   REORDER  >  " + lane.name;
        }
      }
    }
    guide = track_.snap_guide;
  } else if (keys_.dragging) {
    const std::size_t count = keys_.drag_keys.size();
    text = std::string{keys_.duplicate ? "COPY " : ""} +
           std::to_string(count) + (count == 1U ? " KEY " : " KEYS ") +
           signed_duration(p, keys_.drag_delta);
    guide = keys_.snap_guide;
  } else if (markers_.drag_moved) {
    const std::size_t count = markers_.selected.size();
    text = std::string{count == 1U ? "MARKER " : "MARKERS "} +
           signed_duration(p, markers_.drag_delta);
    if (const auto index = marker_index(p, markers_.drag)) {
      text += "  >  " + at(shown_marker_frame(p.input.markers[*index]));
    }
    guide = markers_.snap_guide;
  }
  if (!text.empty() && guide.has_value()) {
    text += "   SNAP " + snap_name(p, *guide);
  }
  return text;
}

std::optional<TimelineEditPreview> TimelinePanel::edit_preview(Pass &p) {
  if (clip_.drag == ClipDrag::None || !p.clip_drag_active) {
    return std::nullopt;
  }
  const EditorTimeline &timeline = p.input.timeline;
  const scene::MediaFrameRate rate{timeline.rate_numerator,
                                   timeline.rate_denominator};
  // Every placed clip as the planners read them, from the document's own
  // timing - which is still the timing the drag began from.
  std::vector<scene::ClipPlacement> placements;
  for (const ElementInfo &element : p.input.elements) {
    if (element.kind == ElementKind::Media && element.timing.placed()) {
      placements.push_back(scene::ClipPlacement{
          element.node, {}, {}, element.sequence_start,
          element.sequence_count, element.timing, element.timeline_lane,
          element.clip_order, element.locked, element.source_frame_rate,
          std::nullopt, 0});
    }
  }
  // The tracks that keep time with a ripple, as the Director reads them:
  // sync locked and not locked.
  std::vector<doc::NodeId> synced;
  for (const scene::TimelineLane &lane : p.input.lanes) {
    if (!lane.implicit && lane.sync_locked && !lane.locked) {
      synced.push_back(lane.id);
    }
  }
  // The edits that move what follows on a track, or a neighbour, are planned
  // exactly as the Director will plan them, so the Stage shows the cut the
  // release will author. The others change the dragged clip alone.
  std::optional<scene::TimelineEditResult> planned;
  std::string_view kind;
  switch (clip_.drag) {
  case ClipDrag::TrimIn:
    kind = "IN";
    break;
  case ClipDrag::TrimOut:
    kind = "OUT";
    break;
  case ClipDrag::RippleTrimHead:
    kind = "RIPPLE IN";
    planned = scene::plan_ripple_trim(placements, clip_.node,
                                      scene::TimelineTrimEdge::Head,
                                      clip_.delta, rate, synced);
    break;
  case ClipDrag::RippleTrimTail:
    kind = "RIPPLE OUT";
    planned = scene::plan_ripple_trim(placements, clip_.node,
                                      scene::TimelineTrimEdge::Tail,
                                      clip_.delta, rate, synced);
    break;
  case ClipDrag::Roll:
    kind = "ROLL";
    planned = scene::plan_roll(placements, clip_.node, clip_.peer,
                               clip_.delta, rate);
    break;
  case ClipDrag::Slip:
    kind = "SLIP";
    planned = scene::plan_slip(placements, clip_.node, clip_.delta, rate);
    break;
  case ClipDrag::Slide:
    kind = "SLIDE";
    planned = scene::plan_slide(placements, clip_.node, clip_.delta, rate);
    break;
  case ClipDrag::Move:
  case ClipDrag::FadeIn:
  case ClipDrag::FadeOut:
  case ClipDrag::None:
    return std::nullopt;
  }

  TimelineEditPreview preview{};
  if (planned.has_value() && static_cast<bool>(*planned)) {
    for (const scene::ClipPlacement &update : planned->value().updates) {
      preview.timings.push_back({update.node, update.timing});
    }
  }
  if (std::none_of(preview.timings.begin(), preview.timings.end(),
                   [&](const scene::ClipTimingOverride &entry) {
                     return entry.clip == clip_.node;
                   })) {
    preview.timings.push_back({clip_.node, clip_.proposed});
  }
  std::sort(preview.timings.begin(), preview.timings.end(),
            [](const scene::ClipTimingOverride &left,
               const scene::ClipTimingOverride &right) {
              return left.clip < right.clip;
            });
  const auto timing_of = [&](doc::NodeId node) -> std::optional<scene::ClipTiming> {
    for (const scene::ClipTimingOverride &entry : preview.timings) {
      if (entry.clip == node) {
        return entry.timing;
      }
    }
    return std::nullopt;
  };

  // The frame the edit point now falls on: an in point's first frame, an out
  // point's last, the frame before a rolled cut, and a slipped or slid
  // clip's new first frame.
  const scene::ClipTiming dragged = timing_of(clip_.node).value_or(clip_.proposed);
  core::i64 shown = dragged.start;
  std::string_view which = "FIRST FRAME";
  switch (clip_.drag) {
  case ClipDrag::TrimOut:
  case ClipDrag::RippleTrimTail:
    shown = dragged.start + dragged.duration - 1;
    which = "LAST FRAME";
    break;
  case ClipDrag::Roll: {
    const ElementInfo *const peer = find_element(p.input.elements, clip_.peer);
    const bool dragged_left =
        peer != nullptr && clip_.origin.start <= peer->timing.start;
    const scene::ClipTiming left =
        dragged_left
            ? dragged
            : timing_of(clip_.peer).value_or(
                  peer != nullptr ? peer->timing : dragged);
    shown = left.start + left.duration - 1;
    which = "LAST FRAME BEFORE THE CUT";
    break;
  }
  default:
    break;
  }
  if (shown < 0) {
    return std::nullopt;
  }
  preview.frame = static_cast<core::u64>(shown);
  preview.caption = std::string{kind} + "  " + std::string{which} + "  " +
                    timecode(p, preview.frame);
  return preview;
}

} // namespace mediaserver::editor
