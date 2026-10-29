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
    // The other plates of a take travel with the one dragged, so their ends
    // are where it already is, not something to snap to.
    const ElementInfo *const dragged = find_element(elements, exclude);
    for (const ElementInfo &element : elements) {
      if (element.node == exclude || !editable_clip(element)) {
        continue;
      }
      if (dragged != nullptr && dragged->plateset_take != 0 &&
          element.plateset_take == dragged->plateset_take &&
          element.timing.start == clip_.origin.start &&
          element.timing.duration == clip_.origin.duration) {
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
  // A take's bar changes the take's time only: its plates stay on their
  // own tracks, in their own places in the stack.
  if (clip_.drag == ClipDrag::Move && clip_.take) {
    clip_.drop_lane.reset();
    clip_landing_.reset();
  } else if (clip_.drag == ClipDrag::Move) {
    clip_.drop_lane = clip_drag_active ? clip_lane_under_pointer(p, clip_pointer.y)
                                       : std::optional<doc::NodeId>{};
    // Up or down within its own track - or among the loose layers, for a
    // clip on none - the bar is lifted to another place in the stack, as a
    // clip is dragged between video tracks in an editor.
    clip_landing_.reset();
    const ElementInfo *const dragged = find_element(elements, clip_.node);
    if (clip_drag_active && dragged != nullptr &&
        clip_.drop_lane == std::optional{dragged->timeline_lane}) {
      clip_landing_ =
          layer_landing(p, *dragged, clip_pointer.x, clip_pointer.y);
    }
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
          if (clip_landing_.has_value() && !retarget.has_value()) {
            frame.layer_reorders.push_back(TimelineLayerReorder{
                clip_.node, clip_landing_->destination,
                clip_landing_->placement});
          }
          break;
        }
        case ClipDrag::None:
          break;
        }
      }
      // A take bar that was dragged was not clicked, so the next press on
      // it starts a gesture of its own rather than completing a double-click.
      if (clip_.take && clip_.moved) {
        clicks_.reset();
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
      clip_.take = false;
      clip_landing_.reset();
    }
  }
}

scene::ClipTiming
TimelinePanel::shown_timing(const ElementInfo &element) const noexcept {
  if (clip_.drag != ClipDrag::None && clip_.node == element.node) {
    return clip_.proposed;
  }
  const auto in = [&](const std::vector<TrackClip> &clips)
      -> const TrackClip * {
    const auto found = std::find_if(
        clips.begin(), clips.end(),
        [&](const TrackClip &clip) { return clip.node == element.node; });
    return found != clips.end() ? &*found : nullptr;
  };
  if (track_.drag != ClipDrag::None) {
    if (const TrackClip *const clip = in(track_.clips)) {
      return clip->proposed;
    }
  }
  if (clip_.pending_edit.has_value() && clip_.pending_edit->clip == element.node) {
    return clip_.pending_edit->timing;
  }
  if (const TrackClip *const clip = in(track_.pending)) {
    return clip->proposed;
  }
  return element.timing;
}

std::optional<TimelinePanel::LayerLanding>
TimelinePanel::layer_landing(Pass &p, const ElementInfo &source, float x,
                             float y) {
  const std::span<const ElementInfo> elements = p.input.elements;
  const gui::Rect &rows_bounds = p.layout.rows;
  const std::vector<Row> &rows = p.built.rows;
  // Peers share an order domain: clips on one track, or loose layers under
  // one parent.
  const auto peers = [](const ElementInfo &left,
                        const ElementInfo &right) noexcept {
    const bool left_tracked = left.kind == ElementKind::Media &&
                              left.timeline_lane != doc::NodeId{};
    const bool right_tracked = right.kind == ElementKind::Media &&
                               right.timeline_lane != doc::NodeId{};
    if (left_tracked || right_tracked) {
      return left_tracked && right_tracked &&
             left.timeline_lane == right.timeline_lane;
    }
    return layer_parent(left) == layer_parent(right);
  };
  // A layer's band is its row and everything twirled open under it, so
  // there is no dead zone over an expanded layer's properties. The deepest
  // band holding the point wins.
  std::optional<LayerLanding> landing;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const Row &row = rows[index];
    if (row.kind != Row::Kind::Layer || row.surface) {
      continue;
    }
    const ElementInfo *const element = find_element(elements, row.node);
    if (element == nullptr || !orderable_layer(*element) ||
        element->node == source.node || element->locked ||
        !peers(source, *element)) {
      continue;
    }
    std::size_t end = index + 1U;
    while (end < rows.size() && rows[end].kind != Row::Kind::LaneHeader &&
           rows[end].kind != Row::Kind::PlatesetHeader &&
           (rows[end].kind != Row::Kind::Layer ||
            rows[end].indent > row.indent)) {
      ++end;
    }
    const float raw_top = rows_bounds.y + row_top(rows, index) - row_scroll_;
    const float raw_bottom = rows_bounds.y + row_top(rows, end) - row_scroll_;
    const float top = std::max(raw_top, rows_bounds.y);
    const float bottom =
        std::min(raw_bottom, rows_bounds.bottom());
    const gui::Rect band{rows_bounds.x, top, rows_bounds.width, bottom - top};
    if (bottom <= top || !band.contains(x, y)) {
      continue;
    }
    const bool before = y < band.center_y();
    landing = LayerLanding{element->node,
                           before ? TimelineLayerPlacement::Before
                                  : TimelineLayerPlacement::After,
                           before ? band.y : band.bottom()};
  }
  return landing;
}

void TimelinePanel::resolve_track_gesture(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const EditorTimeline &timeline = p.input.timeline;
  const std::span<const ElementInfo> elements = p.input.elements;
  TimelinePanelFrame &frame = p.frame;
  const gui::Rect &graph = p.layout.graph;
  const gui::Rect &rows_bounds = p.layout.rows;
  const std::vector<Row> &rows = p.built.rows;

  // Released changes stay drawn while every clip they moved still has the
  // timing the drag began from, for a dozen frames at most.
  if (!track_.pending.empty()) {
    const bool waiting =
        track_.pending_frames > 0U &&
        std::all_of(track_.pending.begin(), track_.pending.end(),
                    [&](const TrackClip &clip) {
                      const ElementInfo *const element =
                          find_element(elements, clip.node);
                      return element != nullptr &&
                             element->timing == clip.origin;
                    });
    if (waiting) {
      --track_.pending_frames;
    } else {
      track_.pending.clear();
    }
  }
  if (track_.drag == ClipDrag::None) {
    return;
  }
  // The track went away under the drag, or a clip on it did.
  if (std::any_of(track_.clips.begin(), track_.clips.end(),
                  [&](const TrackClip &clip) {
                    const ElementInfo *const element =
                        find_element(elements, clip.node);
                    return element == nullptr ||
                           element->timeline_lane != track_.lane;
                  })) {
    track_ = TrackGesture{.pending = std::move(track_.pending),
                          .pending_frames = track_.pending_frames};
    return;
  }
  const gui::UiPoint pointer =
      track_.release.value_or(gui::UiPoint{p.gesture_x, p.gesture_y});
  {
    const float dx = pointer.x - track_.start_x;
    const float dy = pointer.y - track_.start_y;
    track_.moved =
        track_.moved || dx * dx + dy * dy >= lane_drag_threshold_squared;
  }
  if (track_.moved) {
    // What the track's ends catch on: the show range, the playhead, the
    // markers and every clip on another track.
    std::vector<core::i64> targets{p.first_i, p.last_i + 1,
                                   signed_frame(timeline.show_frame)};
    for (const ElementInfo &element : elements) {
      if (editable_clip(element) && element.timeline_lane != track_.lane) {
        targets.push_back(element.timing.start);
        targets.push_back(element.timing.start + element.timing.duration);
      }
    }
    for (const scene::TimelineMarker &marker : p.input.markers) {
      targets.push_back(marker.frame);
    }
    const core::i64 tolerance = static_cast<core::i64>(std::max(
        1.0, std::ceil(8.0 / static_cast<double>(time_axis_.pixels_per_frame))));
    track_.snap_guide.reset();
    const auto snapped = [&](core::i64 edge) {
      if (!snapping_ || ctx.alt_held()) {
        return edge;
      }
      const auto target = nearest_snap(edge, targets, tolerance);
      if (!target.has_value()) {
        return edge;
      }
      track_.snap_guide = *target;
      return *target;
    };
    const core::i64 delta =
        static_cast<core::i64>(std::llround(time_axis_.frame(pointer.x, graph.x))) -
        track_.anchor;
    track_.destination = doc::NodeId{};
    switch (track_.drag) {
    case ClipDrag::Move: {
      core::i64 shift = delta;
      if (delta != 0) {
        // The head first, then the tail: either end butts against a cut.
        shift = snapped(track_.first + delta) - track_.first;
        if (!track_.snap_guide.has_value()) {
          const core::i64 tail = snapped(track_.end + delta);
          if (track_.snap_guide.has_value()) {
            shift = tail - track_.end;
          }
        }
      }
      for (TrackClip &clip : track_.clips) {
        clip.proposed = clip.origin;
        clip.proposed.start = clip.origin.start + shift;
      }
      track_.delta = shift;
      // Over another track's band, the track will take that place in the
      // stack - the same reorder its header drags to.
      const float local = pointer.y - rows_bounds.y + row_scroll_;
      if (rows_bounds.contains(pointer.x, pointer.y) && local >= 0.0F) {
        if (const std::optional<std::size_t> under = row_at(rows, local);
            under.has_value() && rows[*under].lane_member &&
            rows[*under].lane_owner != track_.lane) {
          track_.destination = rows[*under].lane_owner;
        }
      }
      break;
    }
    case ClipDrag::TrimIn: {
      // The clips that open the track lose or gain frames at their heads;
      // each keeps at least one frame and its source stays put under its
      // tail.
      const core::i64 head =
          delta == 0 ? track_.first : snapped(track_.first + delta);
      track_.delta = head - track_.first;
      for (TrackClip &clip : track_.clips) {
        clip.proposed = clip.origin;
        if (clip.origin.start != track_.first) {
          continue;
        }
        const core::i64 start =
            std::min(head, clip.origin.start + clip.origin.duration - 1);
        const core::i64 shift = start - clip.origin.start;
        const ElementInfo *const element = find_element(elements, clip.node);
        if (element == nullptr) {
          continue;
        }
        const auto source = scene::advance_source_phase(
            clip.origin.source_in, shift,
            scene::MediaFrameRate{timeline.rate_numerator,
                                  timeline.rate_denominator},
            element->source_frame_rate, element->sequence_count,
            scene::SourcePhaseRounding::Floor);
        if (!source) {
          continue;
        }
        clip.proposed.start = start;
        clip.proposed.duration = clip.origin.duration - shift;
        clip.proposed.source_in = source.value();
      }
      break;
    }
    case ClipDrag::TrimOut: {
      const core::i64 tail =
          delta == 0 ? track_.end : snapped(track_.end + delta);
      track_.delta = tail - track_.end;
      for (TrackClip &clip : track_.clips) {
        clip.proposed = clip.origin;
        if (clip.origin.start + clip.origin.duration != track_.end) {
          continue;
        }
        clip.proposed.duration =
            std::max(tail, clip.origin.start + 1) - clip.origin.start;
      }
      break;
    }
    default:
      break;
    }
    for (TrackClip &clip : track_.clips) {
      clip.proposed.fade_in =
          std::clamp(clip.proposed.fade_in, core::i64{0}, clip.proposed.duration);
      clip.proposed.fade_out = std::clamp(clip.proposed.fade_out, core::i64{0},
                                          clip.proposed.duration);
    }
  }
  if (ctx.pointer_down()) {
    return;
  }
  std::vector<TrackClip> changed;
  if (track_.moved) {
    for (const TrackClip &clip : track_.clips) {
      if (clip.proposed != clip.origin) {
        frame.clip_edits.push_back(
            TimelineClipEdit{clip.node, clip.proposed, std::nullopt});
        changed.push_back(clip);
      }
    }
    if (track_.drag == ClipDrag::Move && track_.destination != doc::NodeId{}) {
      frame.lane_edits.push_back(TimelineLaneEdit{
          TimelineLaneAction::MoveTo, track_.lane, {}, track_.destination});
    }
  }
  track_ = TrackGesture{};
  track_.pending = std::move(changed);
  track_.pending_frames = track_.pending.empty() ? 0U : 12U;
}

std::optional<doc::NodeId>
TimelinePanel::clip_lane_under_pointer(Pass &p, float pointer_y) {
  const std::span<const scene::TimelineLane> lanes = p.input.lanes;
  const gui::Rect &rows_bounds = p.layout.rows;
  std::vector<Row> &rows = p.built.rows;

  if (pointer_y < rows_bounds.y ||
      pointer_y >= rows_bounds.bottom()) {
    return std::nullopt;
  }
  const float local = pointer_y - rows_bounds.y + row_scroll_;
  if (local < 0.0F) {
    return std::nullopt;
  }
  const std::optional<std::size_t> row_index = row_at(rows, local);
  // A take row is the plates' tracks folded up, not a place of its own.
  if (row_index.has_value() &&
      rows[*row_index].kind == Row::Kind::PlatesetHeader) {
    return std::nullopt;
  }
  const doc::NodeId target =
      row_index.has_value() && rows[*row_index].lane_member
          ? rows[*row_index].lane_owner
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
