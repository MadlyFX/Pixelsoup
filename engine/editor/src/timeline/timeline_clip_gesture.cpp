#include "timeline_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

// A live clip gesture on the Timeline: move, trim, fade, ripple trim, roll,
// slip and slide. The proposal is resolved here before the rows draw, with
// snapping, edge scrolling and the destination track, and held after release
// until the Director adopts the single edit it reports.
namespace mediaserver::editor {
using namespace timeline;

void TimelinePanel::resolve_clip_gesture(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const EditorTimeline &timeline = p.input.timeline;
  const std::span<const ElementInfo> elements = p.input.elements;
  const std::span<const scene::TimelineMarker> markers = p.input.markers;
  TimelinePanelFrame &frame = p.frame;
  const gui::Rect &graph = p.layout.graph;
  const auto x_to_frame = [&](float x) noexcept {
    return time_axis_.frame(x, graph.x);
  };
  auto &gesture_x = p.gesture_x;
  auto &gesture_y = p.gesture_y;
  auto &first_i = p.first_i;
  auto &last_i = p.last_i;

  // Every boundary a clip gesture may land on: the show range, the playhead,
  // and the ends of every other clip. Butt-joining two clips and cutting on
  // the playhead are the two edits a video timeline is asked for constantly,
  // and neither should require pixel accuracy.
  const auto snap_targets = [&](doc::NodeId exclude) {
    std::vector<core::i64> targets{first_i, last_i + 1,
                                   signed_frame(timeline.show_frame)};
    for (const ElementInfo &element : elements) {
      if (element.node == exclude || !editable_clip(element)) {
        continue;
      }
      targets.push_back(element.timing.start);
      targets.push_back(element.timing.start + element.timing.duration);
    }
    // A cue is placed against a cut and a cut against a cue, so each pulls
    // the other.
    for (const scene::TimelineMarker &marker : markers) {
      targets.push_back(marker.frame);
    }
    return targets;
  };

  // A vertical clip drop names a track band, or the ordinary top-level area
  // outside every band. Locked tracks are not destinations. Keeping this
  // resolver independent of the graph lets the same gesture begin on a media
  // row when its bar is short or off screen.

  if (clip_.pending_edit.has_value()) {
    const ElementInfo *const target =
        find_element(elements, clip_.pending_edit->clip);
    if (target == nullptr || target->timing != clip_.pending_origin ||
        clip_.pending_frames == 0U) {
      clip_.pending_edit.reset();
    } else {
      --clip_.pending_frames;
    }
  }
  const gui::UiPoint clip_pointer =
      clip_.release.value_or(gui::UiPoint{gesture_x, gesture_y});
  if (clip_.drag != ClipDrag::None) {
    const float dx = clip_pointer.x - clip_.start_x;
    const float dy = clip_pointer.y - clip_.start_y;
    clip_.moved = clip_.moved ||
                       dx * dx + dy * dy >= lane_drag_threshold_squared;
  }
  p.clip_drag_active =
      clip_.drag != ClipDrag::None && clip_.moved;
  auto &clip_drag_active = p.clip_drag_active;
  if (clip_.drag == ClipDrag::Move) {
    clip_.drop_lane = clip_drag_active ? clip_lane_under_pointer(p, clip_pointer.y)
                                       : std::optional<doc::NodeId>{};
  }

  // Resolve a live clip proposal before anything is drawn, so the bar under
  // the pointer follows the gesture without authoring intermediate document
  // edits. Like Feed Mapping, the document changes once, on release.
  if (clip_.drag != ClipDrag::None) {
    if (clip_drag_active) {
      clip_.status.clear();
      clip_.status_error = false;
      clip_.status_until = ctx.time_seconds() + 3.0;
      const core::i64 pointer_frame =
          static_cast<core::i64>(std::llround(x_to_frame(clip_pointer.x)));
      const core::i64 delta = pointer_frame - clip_.anchor;
      const scene::ClipTiming origin = clip_.origin;
      scene::ClipTiming proposed = origin;
      const auto targets = snap_targets(clip_.node);
      const core::i64 tolerance = static_cast<core::i64>(std::max(
          1.0, std::ceil(8.0 / static_cast<double>(time_axis_.pixels_per_frame))));
      clip_.snap_guide.reset();
      const auto snapped = [&](core::i64 edge) {
        // Alt suspends snapping for the duration of a gesture, the
        // same temporary override a key drag already honours.
        if (!snapping_ || ctx.alt_held()) {
          return edge;
        }
        const auto target = nearest_snap(edge, targets, tolerance);
        if (!target.has_value()) {
          return edge;
        }
        clip_.snap_guide = *target;
        return *target;
      };

      core::i64 semantic_delta = delta;
      if (delta == 0) {
        semantic_delta = 0;
      } else if (clip_.drag == ClipDrag::RippleTrimHead) {
        semantic_delta = snapped(origin.start + delta) - origin.start;
      } else if (clip_.drag == ClipDrag::RippleTrimTail) {
        const core::i64 tail = origin.start + origin.duration;
        semantic_delta = snapped(tail + delta) - tail;
      } else if (clip_.drag == ClipDrag::Roll) {
        const ElementInfo *const first_clip =
            find_element(elements, clip_.node);
        const ElementInfo *const second_clip =
            find_element(elements, clip_.peer);
        if (first_clip != nullptr && second_clip != nullptr) {
          const ElementInfo *const left =
              first_clip->timing.start <= second_clip->timing.start
                  ? first_clip
                  : second_clip;
          const core::i64 cut = left->timing.start + left->timing.duration;
          semantic_delta = snapped(cut + delta) - cut;
        }
      } else if (clip_.drag == ClipDrag::Slide) {
        semantic_delta = snapped(origin.start + delta) - origin.start;
      }
      clip_.delta = semantic_delta;

      switch (clip_.drag) {
      case ClipDrag::Move: {
        core::i64 start = origin.start;
        if (delta != 0) {
          start = snapped(origin.start + delta);
          if (!clip_.snap_guide.has_value()) {
            // The tail is as good a thing to butt against as the head.
            const core::i64 tail = origin.start + delta + origin.duration;
            const core::i64 tail_snapped = snapped(tail);
            if (clip_.snap_guide.has_value()) {
              start = tail_snapped - origin.duration;
            }
          }
        }
        proposed.start = start;
        break;
      }
      case ClipDrag::TrimIn: {
        // The out point stays where it is and the source slides with
        // the in point, which is what makes this a trim rather than a
        // move. Convert at the source rate and retain the source frame at
        // the new cut; the authored source phase currently has integer precision.
        const core::i64 limit = origin.start + origin.duration - 1;
        const core::i64 start = delta == 0
                                    ? origin.start
                                    : std::min(snapped(origin.start + delta), limit);
        const core::i64 shift = start - origin.start;
        proposed.start = start;
        proposed.duration = origin.duration - shift;
        if (const ElementInfo *const clip = find_element(elements, clip_.node)) {
          const auto source = scene::advance_source_phase(
              origin.source_in, shift,
              scene::MediaFrameRate{timeline.rate_numerator, timeline.rate_denominator},
              clip->source_frame_rate, clip->sequence_count,
              scene::SourcePhaseRounding::Floor);
          if (source) {
            proposed.source_in = source.value();
          } else {
            proposed = origin;
            clip_.status = "TRIM UNAVAILABLE  /  Check source frames and frame rate";
            clip_.status_error = true;
          }
        }
        break;
      }
      case ClipDrag::TrimOut: {
        const core::i64 end =
            delta == 0
                ? origin.start + origin.duration
                : std::max(snapped(origin.start + origin.duration + delta),
                           origin.start + 1);
        proposed.duration = end - origin.start;
        break;
      }
      case ClipDrag::FadeIn:
        proposed.fade_in =
            std::clamp(origin.fade_in + delta, core::i64{0}, origin.duration);
        break;
      case ClipDrag::FadeOut:
        proposed.fade_out =
            std::clamp(origin.fade_out - delta, core::i64{0}, origin.duration);
        break;
      case ClipDrag::RippleTrimHead:
        proposed.duration =
            std::max(origin.duration - semantic_delta, core::i64{1});
        proposed.source_in =
            std::max(origin.source_in + semantic_delta, core::i64{0});
        break;
      case ClipDrag::RippleTrimTail:
        proposed.duration =
            std::max(origin.duration + semantic_delta, core::i64{1});
        break;
      case ClipDrag::Roll: {
        const ElementInfo *const peer = find_element(elements, clip_.peer);
        const bool dragged_left =
            peer != nullptr && origin.start <= peer->timing.start;
        if (dragged_left) {
          proposed.duration =
              std::max(origin.duration + semantic_delta, core::i64{1});
        } else {
          proposed.start = origin.start + semantic_delta;
          proposed.duration =
              std::max(origin.duration - semantic_delta, core::i64{1});
          proposed.source_in =
              std::max(origin.source_in + semantic_delta, core::i64{0});
        }
        break;
      }
      case ClipDrag::Slip: {
        const ElementInfo *const clip = find_element(elements, clip_.node);
        if (clip != nullptr) {
          scene::ClipPlacement placement{};
          placement.node = clip->node;
          placement.sequence_count = clip->sequence_count;
          placement.timing = origin;
          placement.frame_rate = clip->source_frame_rate;
          const auto planned = scene::plan_slip(
              std::span{&placement, 1U}, clip->node, semantic_delta,
              scene::MediaFrameRate{timeline.rate_numerator,
                                    timeline.rate_denominator});
          if (planned) {
            clip_.delta = planned.value().applied_delta;
            if (!planned.value().updates.empty()) {
              proposed = planned.value().updates.front().timing;
            }
            clip_.status = "SLIP  /  Source " +
                           std::to_string(proposed.source_in) + " of " +
                           std::to_string(clip->sequence_count) + " frames";
            const auto offset = scene::source_ordinal_delta(
                semantic_delta,
                scene::MediaFrameRate{timeline.rate_numerator,
                                      timeline.rate_denominator},
                clip->source_frame_rate);
            const core::i64 phase = origin.source_in % clip->sequence_count;
            if (!offset || offset.value() < -phase ||
                offset.value() > static_cast<core::i64>(clip->sequence_count) -
                                     1 - phase) {
              clip_.status += "  /  wrapped";
            }
            if (clip->sequence_count == 1U) {
              clip_.status += "  /  One source frame; image is unchanged";
            } else if (ctx.pointer_down()) {
              clip_.status += "  /  release to apply";
            }
          } else {
            clip_.delta = 0;
            clip_.status_error = true;
            clip_.status = "SLIP UNAVAILABLE  /  ";
            switch (planned.error()) {
            case scene::TimelineEditError::InvalidRate:
              clip_.status += "Invalid source or show frame rate";
              break;
            case scene::TimelineEditError::LockedClip:
              clip_.status += "Clip is locked";
              break;
            case scene::TimelineEditError::ClipNotFound:
              clip_.status += "Clip is no longer available";
              break;
            case scene::TimelineEditError::IntegerOverflow:
              clip_.status += "Source offset exceeds the supported range";
              break;
            default:
              clip_.status += "Check source frame count and clip timing";
              break;
            }
          }
        }
        break;
      }
      case ClipDrag::Slide:
        proposed.start = origin.start + semantic_delta;
        break;
      case ClipDrag::None:
        break;
      }
      proposed.fade_in =
          std::clamp(proposed.fade_in, core::i64{0}, proposed.duration);
      proposed.fade_out =
          std::clamp(proposed.fade_out, core::i64{0}, proposed.duration);
      clip_.proposed = proposed;
      if (clip_.status.empty() &&
          (clip_.drag == ClipDrag::Move || clip_.drag == ClipDrag::TrimIn ||
           clip_.drag == ClipDrag::TrimOut)) {
        clip_.status = clip_.drag == ClipDrag::Move ? "MOVE" : "TRIM";
        clip_.status += "  /  Start " + std::to_string(proposed.start) +
                        "  /  End " +
                        std::to_string(proposed.start + proposed.duration) +
                        "  /  Source " + std::to_string(proposed.source_in);
      }
    }
    if (!ctx.pointer_down()) {
      if (clip_.moved) {
        switch (clip_.drag) {
        case ClipDrag::RippleTrimHead:
          if (clip_.delta != 0) {
            frame.timeline_edits.emplace_back(TimelineRippleTrimRequest{
                clip_.node, scene::TimelineTrimEdge::Head,
                clip_.delta});
          }
          break;
        case ClipDrag::RippleTrimTail:
          if (clip_.delta != 0) {
            frame.timeline_edits.emplace_back(TimelineRippleTrimRequest{
                clip_.node, scene::TimelineTrimEdge::Tail,
                clip_.delta});
          }
          break;
        case ClipDrag::Roll:
          if (clip_.delta != 0) {
            frame.timeline_edits.emplace_back(TimelineRollRequest{
                clip_.node, clip_.peer, clip_.delta});
          }
          break;
        case ClipDrag::Slip:
          if (clip_.delta != 0) {
            frame.timeline_edits.emplace_back(
                TimelineSlipRequest{clip_.node, clip_.delta});
            clip_.pending_edit = TimelineClipEdit{
                clip_.node, clip_.proposed, std::nullopt};
            clip_.pending_origin = clip_.origin;
            clip_.pending_frames = 12U;
          }
          break;
        case ClipDrag::Slide:
          if (clip_.delta != 0) {
            frame.timeline_edits.emplace_back(
                TimelineSlideRequest{clip_.node, clip_.delta});
          }
          break;
        case ClipDrag::Move:
        case ClipDrag::TrimIn:
        case ClipDrag::TrimOut:
        case ClipDrag::FadeIn:
        case ClipDrag::FadeOut: {
          std::optional<doc::NodeId> retarget;
          const ElementInfo *const dragged =
              clip_.drag == ClipDrag::Move
                  ? find_element(elements, clip_.node)
                  : nullptr;
          if (dragged != nullptr && clip_.drop_lane.has_value() &&
              *clip_.drop_lane != dragged->timeline_lane) {
            retarget = *clip_.drop_lane;
          }
          if (clip_.proposed != clip_.origin ||
              retarget.has_value()) {
            frame.clip_edits.push_back(TimelineClipEdit{
                clip_.node, clip_.proposed, retarget});
            clip_.pending_edit = frame.clip_edits.back();
            clip_.pending_origin = clip_.origin;
            clip_.pending_frames = 12U;
          }
          break;
        }
        case ClipDrag::None:
          break;
        }
      }
      clip_.drag = ClipDrag::None;
      clip_.node = doc::NodeId{};
      clip_.peer = doc::NodeId{};
      clip_.delta = 0;
      clip_.start_x = 0.0F;
      clip_.start_y = 0.0F;
      clip_.release.reset();
      clip_.moved = false;
      clip_.drop_lane.reset();
      clip_.snap_guide.reset();
    }
  }


}

std::optional<doc::NodeId>
TimelinePanel::clip_lane_under_pointer(Pass &p, float pointer_y) {
  const std::span<const scene::TimelineLane> lanes = p.input.lanes;
  const gui::Rect &rows_bounds = p.layout.rows;
  std::vector<Row> &rows = p.built.rows;

  if (pointer_y < rows_bounds.y ||
      pointer_y >= rows_bounds.y + rows_bounds.height) {
    return std::nullopt;
  }
  const float local = pointer_y - rows_bounds.y + row_scroll_;
  if (local < 0.0F) {
    return std::nullopt;
  }
  const auto row_index = static_cast<std::size_t>(local / row_height);
  const doc::NodeId target =
      row_index < rows.size() && rows[row_index].lane_member
          ? rows[row_index].lane_owner
          : doc::NodeId{};
  if (target == doc::NodeId{}) {
    return target;
  }
  const auto lane = std::find_if(
      lanes.begin(), lanes.end(), [&](const scene::TimelineLane &candidate) {
        return !candidate.implicit && candidate.id == target;
      });
  if (lane == lanes.end() || lane->locked) {
    return std::nullopt;
  }
  return target;
}

} // namespace mediaserver::editor
