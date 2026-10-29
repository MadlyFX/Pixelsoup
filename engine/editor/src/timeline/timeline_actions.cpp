#include "timeline_internal.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <vector>

// What the Timeline does when asked: the shared actions (ripple, clear keys,
// marker edits, zoom, In/Out), the context menus that offer only what applies
// to what was right-clicked, and the single-key shortcuts. A cut or a cue
// means the same thing however it is asked for.
namespace mediaserver::editor {
using namespace timeline;

void TimelinePanel::handle_actions(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const EditorTimeline &timeline = p.input.timeline;
  const std::span<const ElementInfo> elements = p.input.elements;
  const std::span<const doc::NodeId> selection = p.input.selection;
  const std::span<const scene::EffectInstance> effects = p.input.effects;
  const std::span<const scene::TimelineLane> lanes = p.input.lanes;
  const std::span<const scene::TimelineMarker> markers = p.input.markers;
  const std::optional<core::i64> &clipboard_span = p.input.clipboard_span;
  TimelinePanelFrame &frame = p.frame;
  FrameScratch &scratch = p.scratch;
  const gui::Rect &ruler = p.layout.ruler;
  const gui::Rect &graph = p.layout.graph;
  const gui::Rect &strip_graph = p.layout.strip_graph;
  std::vector<Row> &rows = p.built.rows;
  const auto frame_to_x = [&](double value) noexcept {
    return time_axis_.x(value, graph.x);
  };
  const auto x_to_frame = [&](float x) noexcept {
    return time_axis_.frame(x, graph.x);
  };
  auto &typing_at_start = p.typing_at_start;
  auto &first = p.first;
  auto &last = p.last;
  auto &selected_clips = p.selected_clips;
  auto &clip_selection_valid = p.clip_selection_valid;
  auto &target_lane = p.target_lane;
  auto &paste_ready = p.paste_ready;
  auto &scale_numerator_id = p.scale_numerator_id;
  auto &scale_denominator_id = p.scale_denominator_id;
  auto &scale_user_anchor_id = p.scale_user_anchor_id;
  auto &acting_frame = p.acting_frame;
  auto &playhead = p.playhead;
  auto &search_id = p.search_id;

  // Choosing a layer, clip, or track leaves the marker domain, so Delete and
  // the menus aim at what was just chosen.
  if (frame.selection.has_value()) {
    markers_.selected.clear();
  }
  frame.editing = frame.editing || range_fields_.live() ||
                  key_tools_.value_field.live() || value_fields_.live() ||
                  ctx.focus() == search_id ||
                  ctx.focus() == scale_numerator_id ||
                  ctx.focus() == scale_denominator_id ||
                  ctx.focus() == scale_user_anchor_id || toolbar_.rate_menu_open ||
                  toolbar_.time_drag_id != gui::UiId{} ||
                  toolbar_.time_edit_id != gui::UiId{} ||
                  markers_.name_node != doc::NodeId{} || frame.lane_editing ||
                  rename_.active();

  // ---------------------------------------------------------------------
  // Shared actions. The context menus, the shortcuts, and the toolbar all
  // reach these, so a cut or a cue means the same thing however it is asked
  // for.
  const auto ripple_ready = [&](std::span<const ElementInfo *const> clips) {
    if (clips.empty()) {
      return false;
    }
    std::vector<const ElementInfo *> ordered{clips.begin(), clips.end()};
    std::sort(ordered.begin(), ordered.end(),
              [](const ElementInfo *left, const ElementInfo *right) noexcept {
                return left->timing.start < right->timing.start;
              });
    const doc::NodeId lane = ordered.front()->timeline_lane;
    core::i64 begin = ordered.front()->timing.start;
    core::i64 end = begin;
    for (const ElementInfo *clip : ordered) {
      const auto clip_end = exclusive_clip_end(clip->timing);
      if (!editable_clip(*clip) || !clip_end.has_value() ||
          clip->timeline_lane != lane || clip->timing.start > end) {
        return false;
      }
      end = std::max(end, *clip_end);
    }
    // Nothing else in the track may straddle the span being closed up.
    for (const ElementInfo &other : elements) {
      if (other.kind != ElementKind::Media || !other.timing.placed() ||
          other.timeline_lane != lane ||
          std::find(ordered.begin(), ordered.end(), &other) != ordered.end()) {
        continue;
      }
      const auto other_end = exclusive_clip_end(other.timing);
      if (!other_end.has_value() ||
          (other.timing.start < end && *other_end > begin)) {
        return false;
      }
    }
    return true;
  };
  const auto ripple_delete = [&](std::span<const ElementInfo *const> clips) {
    TimelineRippleDeleteRequest request{};
    for (const ElementInfo *clip : clips) {
      request.clips.push_back(clip->node);
    }
    std::sort(request.clips.begin(), request.clips.end());
    frame.timeline_edits.emplace_back(std::move(request));
  };
  const auto clear_keys = [&](doc::NodeId layer) {
    // Transform and opacity belong to the layer. Effect parameters belong to
    // their first-class effect nodes, so those addresses are cleared
    // explicitly instead of inventing brightness and blur channels on the
    // layer itself.
    for (core::u8 component = 0U;
         component <= static_cast<core::u8>(scene::PropertyChannel::Opacity);
         ++component) {
      frame.keys.push_back(TimelineKeyEdit{
          layer, static_cast<scene::TransformChannel>(component), 0U, 0.0,
          true});
    }
    if (const ElementInfo *const element = find_element(elements, layer);
        element != nullptr && element->kind == ElementKind::Camera) {
      for (auto component = static_cast<core::u8>(scene::PropertyChannel::CameraFieldOfView);
           component <= static_cast<core::u8>(scene::PropertyChannel::CameraAperture);
           ++component) {
        frame.keys.push_back(TimelineKeyEdit{
            layer, static_cast<scene::PropertyChannel>(component), 0U, 0.0, true});
      }
    }
    // A light's output and colour are its own channels too.
    if (const ElementInfo *const element = find_element(elements, layer);
        element != nullptr && is_light(element->kind)) {
      for (auto component =
               static_cast<core::u8>(scene::PropertyChannel::LightIntensity);
           component <=
           static_cast<core::u8>(scene::PropertyChannel::LightRange);
           ++component) {
        frame.keys.push_back(TimelineKeyEdit{
            layer, static_cast<scene::PropertyChannel>(component), 0U, 0.0,
            true});
      }
    }
    for (const scene::EffectInstance &effect : effects) {
      if (effect.target != layer) {
        continue;
      }
      std::optional<scene::PropertyChannel> property;
      switch (effect.kind) {
      case scene::EffectKind::Brightness:
        property = scene::PropertyChannel::Brightness;
        break;
      case scene::EffectKind::Saturation:
        property = scene::PropertyChannel::Saturation;
        break;
      case scene::EffectKind::Blur:
        property = scene::PropertyChannel::BlurRadius;
        break;
      case scene::EffectKind::BoxBlur:
        property = scene::PropertyChannel::BoxBlurRadius;
        break;
      case scene::EffectKind::DirectionalBlur:
        property = scene::PropertyChannel::DirectionalBlurLength;
        break;
      case scene::EffectKind::FilmGrain:
        property = scene::PropertyChannel::FilmGrainAmount;
        break;
      case scene::EffectKind::ChromaKey:
        property = scene::PropertyChannel::ChromaKeyMix;
        break;
      case scene::EffectKind::LumaKey:
        property = scene::PropertyChannel::LumaKeyMix;
        break;
      case scene::EffectKind::Levels:
      case scene::EffectKind::LiftGammaGain:
      case scene::EffectKind::ContrastPivot:
      case scene::EffectKind::ColorMatrix:
      case scene::EffectKind::WhiteBalance:
      case scene::EffectKind::Curves:
      case scene::EffectKind::Count:
        break;
      }
      if (property.has_value()) {
        frame.keys.push_back(
            TimelineKeyEdit{effect.source, *property, 0U, 0.0, true});
      }
    }
  };
  const auto update_marker = [&](doc::NodeId id, auto &&change) {
    if (const auto index = marker_index(p, id)) {
      TimelineMarkerEdit edit{TimelineMarkerAction::Update, markers[*index]};
      change(edit.marker);
      if (edit.marker != markers[*index]) {
        frame.marker_edits.push_back(std::move(edit));
      }
    }
  };
  const auto delete_markers = [&](std::span<const doc::NodeId> ids) {
    for (const doc::NodeId id : ids) {
      if (const auto index = marker_index(p, id)) {
        frame.marker_edits.push_back(
            TimelineMarkerEdit{TimelineMarkerAction::Delete, markers[*index]});
      }
    }
  };
  const auto open_marker_name = [&](doc::NodeId id) {
    if (const auto index = marker_index(p, id)) {
      markers_.name_node = id;
      markers_.name_draft = markers[*index].name;
      ctx.focus_text_field(mix(id_, "marker name", id.value()),
                           markers_.name_draft);
    }
  };
  const auto zoom_about_playhead = [&](double factor) {
    const double anchor = static_cast<double>(timeline.show_frame);
    const float anchor_x = frame_to_x(anchor);
    const float keep_x = anchor_x >= graph.x && anchor_x <= graph.right()
                             ? anchor_x
                             : graph.center_x();
    time_axis_.zoom_about(anchor, keep_x, graph.x, factor);
  };
  // A track splits into two tracks at the cut, as a clip splits into two
  // clips: the Director moves what plays from the cut on to a second track.
  const auto split_track = [&](doc::NodeId lane, core::i64 cut) {
    TimelineLaneEdit split{TimelineLaneAction::Split, lane, {}};
    split.frame = cut;
    frame.lane_edits.push_back(std::move(split));
  };
  // The tracks the operator has selected, by their headers or bars.
  std::vector<doc::NodeId> selected_tracks;
  for (const scene::TimelineLane &lane : lanes) {
    if (!lane.implicit &&
        (selected(lane.id, selection) || lane.id == selected_lane_)) {
      selected_tracks.push_back(lane.id);
    }
  }
  const auto set_in = [&](core::u64 at) {
    frame.range = RangeRequest{at, std::max(last, at), timeline.loop};
  };
  const auto set_out = [&](core::u64 at) {
    frame.range = RangeRequest{std::min(first, at), at, timeline.loop};
  };

  // ---------------------------------------------------------------------
  // Context menus. A right press names what is under it; the menu lists only
  // what applies to that thing, which is what keeps track deletion, cutting,
  // and cue conversion off a toolbar full of buttons that mostly do nothing.
  const gui::UiId menu_id = mix(id_, "context menu");
  const auto add_entry = [&](std::string label, MenuCommand command,
                             doc::NodeId node = {}, core::i64 at = 0,
                             doc::NodeId other = {}, core::i64 end = 0) {
    menu_entries_.push_back(MenuEntry{std::move(label),
                                      static_cast<core::u8>(command), node,
                                      other, at, end});
  };
  bool open_menu{};
  const core::i64 pointer_frame_i = static_cast<core::i64>(rounded_frame(
      x_to_frame(ctx.pointer_x()), 0U,
      static_cast<core::u64>(std::numeric_limits<core::i64>::max())));
  if (ctx.secondary_pointer_pressed()) {
    menu_entries_.clear();
    const gui::Rect ruler_graph{graph.x, ruler.y, graph.width, ruler.height};
    if (scratch.right_marker.has_value()) {
      const scene::TimelineMarker &marker = markers[*scratch.right_marker];
      if (std::find(markers_.selected.begin(), markers_.selected.end(),
                    marker.id) == markers_.selected.end()) {
        markers_.selected.assign(1U, marker.id);
      }
      add_entry("Rename...", MenuCommand::RenameMarker, marker.id);
      if (marker.cue != scene::CueAction::Hold) {
        add_entry("Make Hold Cue", MenuCommand::MarkerToHold, marker.id);
      }
      if (marker.cue != scene::CueAction::Loop) {
        add_entry("Make Loop Cue", MenuCommand::MarkerToLoop, marker.id);
      }
      if (marker.is_cue()) {
        add_entry("Make Plain Marker", MenuCommand::MarkerToPlain, marker.id);
      }
      add_entry(std::string{"Color: "} +
                    std::string{timeline_marker_color_name(static_cast<core::u8>(
                        (marker.color + 1U) % scene::timeline_marker_colors))},
                MenuCommand::MarkerNextColor, marker.id);
      add_entry("Go To", MenuCommand::GoToMarker, marker.id);
      add_entry(markers_.selected.size() > 1U
                    ? "Delete Selected Markers"
                    : (marker.is_cue() ? "Delete Cue" : "Delete Marker"),
                MenuCommand::DeleteMarker, marker.id);
      open_menu = true;
    } else if (const auto key = key_under_pointer(p); key.has_value()) {
      // A key's menu eases the whole selection, which takes the key first
      // when it was not already part of it.
      if (!key_selected(*key)) {
        keys_.selected.assign(1U, *key);
      }
      const auto ease_entry = [&](std::string label, scene::KeyEase ease) {
        add_entry(std::move(label), MenuCommand::EaseKeys, {},
                  static_cast<core::i64>(ease));
      };
      ease_entry("Linear", scene::KeyEase::Linear);
      ease_entry("Hold", scene::KeyEase::Hold);
      ease_entry("Ease In      Shift+F9", scene::KeyEase::EaseIn);
      ease_entry("Ease Out      Ctrl+F9", scene::KeyEase::EaseOut);
      ease_entry("Easy Ease      F9", scene::KeyEase::EasyEase);
      ease_entry("Auto Bezier", scene::KeyEase::Auto);
      if (selection_has_cubic(p)) {
        ease_entry("Aligned Handles", scene::KeyEase::Aligned);
        ease_entry("Broken Handles", scene::KeyEase::Broken);
        ease_entry("Flat Handles", scene::KeyEase::Flat);
      }
      open_menu = true;
    } else if (ruler_graph.contains(ctx.pointer_x(), ctx.pointer_y()) ||
               strip_graph.contains(ctx.pointer_x(), ctx.pointer_y())) {
      const core::i64 at = std::max<core::i64>(pointer_frame_i, 0);
      add_entry("Add Marker Here", MenuCommand::AddMarker, {}, at);
      add_entry("Add Hold Cue Here", MenuCommand::AddHoldCue, {}, at);
      add_entry("Add Loop Cue Here", MenuCommand::AddLoopCue, {}, at);
      add_entry("Set In Here", MenuCommand::SetIn, {}, at);
      add_entry("Set Out Here", MenuCommand::SetOut, {}, at);
      add_entry("Zoom to Fit", MenuCommand::ZoomFit);
      add_entry(ruler_.frames ? "Show Timecode" : "Show Frames",
                MenuCommand::ToggleRulerUnits);
      open_menu = true;
    } else if (scratch.right_row.has_value()) {
      const Row &row = rows[*scratch.right_row];
      if (row.kind == Row::Kind::LaneHeader) {
        const auto lane = std::find_if(
            lanes.begin(), lanes.end(), [&](const scene::TimelineLane &held) {
              return !held.implicit && held.id == row.node;
            });
        if (lane != lanes.end()) {
          add_entry("Rename Track", MenuCommand::RenameTrack, lane->id);
          add_entry(lane->muted ? "Unmute Track" : "Mute Track",
                    MenuCommand::ToggleTrackMute, lane->id);
          add_entry(lane->locked ? "Unlock Track" : "Lock Track",
                    MenuCommand::ToggleTrackLock, lane->id);
          // The track's own bar edits it as one clip; so does its menu.
          bool spanned = false;
          core::i64 track_first = 0;
          core::i64 track_last = 0;
          for (const ElementInfo &held : elements) {
            if (held.kind != ElementKind::Media ||
                held.timeline_lane != lane->id || !held.timing.placed()) {
              continue;
            }
            track_first = spanned ? std::min(track_first, held.timing.start)
                                  : held.timing.start;
            track_last = spanned ? std::max(track_last, held.timing.last_frame())
                                 : held.timing.last_frame();
            spanned = true;
          }
          if (!lane->locked &&
              track_splittable(elements, lane->id, signed_frame(acting_frame))) {
            add_entry("Split Track at Playhead      Ctrl+K",
                      MenuCommand::SplitTrackAtPlayhead, lane->id);
          }
          if (spanned) {
            add_entry("Set In/Out to Track      X", MenuCommand::RangeToClip,
                      lane->id, track_first, {}, track_last);
          }
          if (!lane->locked) {
            add_entry("Clear Keyframes", MenuCommand::ClearKeys, lane->id);
          }
          if (clipboard_span.has_value() && *clipboard_span > 0 &&
              !lane->locked) {
            add_entry("Insert Clipboard at Playhead",
                      MenuCommand::InsertClipboard, lane->id);
            add_entry("Overwrite Clipboard at Playhead",
                      MenuCommand::OverwriteClipboard, lane->id);
          }
          add_entry("New Track", MenuCommand::NewTrack);
          const bool empty = std::none_of(
              elements.begin(), elements.end(), [&](const ElementInfo &held) {
                return held.kind == ElementKind::Media &&
                       held.timeline_lane == lane->id;
              });
          if (empty) {
            add_entry("Delete Track", MenuCommand::DeleteTrack, lane->id);
          } else {
            add_entry("Delete Track and Its Clips",
                      MenuCommand::DeleteTrackClips, lane->id);
            for (const scene::TimelineLane &destination : lanes) {
              if (!destination.implicit && destination.id != lane->id &&
                  destination.scene == lane->scene) {
                add_entry("Move Clips to " + destination.name +
                              ", Delete Track",
                          MenuCommand::MoveClipsDeleteTrack, lane->id, 0,
                          destination.id);
              }
            }
          }
          open_menu = true;
        }
      } else if (row.kind == Row::Kind::PlatesetHeader) {
        // The set as one thing: its tracks folded or listed, switched,
        // cut, ranged and removed together.
        const PlatesetGroup &group = p.built.platesets[row.plateset];
        const bool listed = rows_state_.expanded_platesets.contains(row.node);
        add_entry(listed ? "Fold Plate Tracks" : "List Plate Tracks",
                  MenuCommand::TogglePlatesetOpen, row.node);
        add_entry(row.muted ? "Unmute Plates" : "Mute Plates",
                  MenuCommand::TogglePlatesetMute, row.node);
        add_entry(row.locked ? "Unlock Plates" : "Lock Plates",
                  MenuCommand::TogglePlatesetLock, row.node);
        bool spanned = false;
        core::i64 set_first = 0;
        core::i64 set_last = 0;
        for (const PlatesetTake &take : group.takes) {
          const core::i64 take_last = take.start + take.duration - 1;
          if (!take.locked && playhead > take.start && playhead <= take_last) {
            add_entry("Split Take at Playhead", MenuCommand::SplitAtPlayhead,
                      take.lead);
          }
          set_first = spanned ? std::min(set_first, take.start) : take.start;
          set_last = spanned ? std::max(set_last, take_last) : take_last;
          spanned = true;
        }
        if (spanned) {
          add_entry("Set In/Out to Plateset", MenuCommand::RangeToClip,
                    row.node, set_first, {}, set_last);
          add_entry("Open Plateset in Asset Viewer",
                    MenuCommand::OpenPlatesetInAssetViewer,
                    group.takes.front().lead);
        }
        if (!row.locked) {
          add_entry("Delete Plateset Tracks and Plates",
                    MenuCommand::DeletePlatesetTracks, row.node);
        }
        open_menu = true;
      } else if (row.kind == Row::Kind::Layer && !row.surface) {
        const ElementInfo *const element = find_element(elements, row.node);
        if (element != nullptr && !element->locked) {
          const bool clip = element->kind == ElementKind::Media &&
                            element->timing.placed();
          if (clip) {
            if (playhead > element->timing.start &&
                playhead <= element->timing.last_frame()) {
              add_entry("Split at Playhead", MenuCommand::SplitAtPlayhead,
                        element->node);
            }
            const bool in_selection = selected(element->node, selection);
            if (in_selection && selected_clips.size() > 1U &&
                ripple_ready(selected_clips)) {
              add_entry("Ripple Delete Selection",
                        MenuCommand::RippleDeleteSelection);
            } else {
              const std::array<const ElementInfo *, 1U> alone{element};
              if (ripple_ready(alone)) {
                add_entry("Ripple Delete", MenuCommand::RippleDeleteClip,
                          element->node);
              }
            }
            if (clipboard_span.has_value() && *clipboard_span > 0) {
              add_entry("Insert Clipboard at Playhead",
                        MenuCommand::InsertClipboard, element->timeline_lane);
              add_entry("Overwrite Clipboard at Playhead",
                        MenuCommand::OverwriteClipboard,
                        element->timeline_lane);
            }
            add_entry("Set In/Out to Clip", MenuCommand::RangeToClip,
                      element->node, element->timing.start,
                      {}, element->timing.last_frame());
            add_entry("Open in Asset Viewer", MenuCommand::OpenInAssetViewer,
                      element->node);
          }
          if (!element->linked &&
              element->kind != ElementKind::Environment) {
            add_entry("Rename      F2", MenuCommand::RenameLayer,
                      element->node);
          }
          if (element->kind != ElementKind::Environment) {
            add_entry("Clear Keyframes", MenuCommand::ClearKeys,
                      element->node);
          }
          open_menu = !menu_entries_.empty();
        }
      }
    }
    if (open_menu) {
      ctx.open_context_menu(menu_id, ctx.pointer_x(), ctx.pointer_y());
    } else {
      menu_entries_.clear();
    }
  }
  if (!menu_entries_.empty()) {
    std::vector<std::string_view> labels;
    labels.reserve(menu_entries_.size());
    for (const MenuEntry &entry : menu_entries_) {
      labels.push_back(entry.label);
    }
    if (const auto chosen = ctx.context_menu(menu_id, labels);
        chosen.has_value() && *chosen < menu_entries_.size()) {
      const MenuEntry entry = menu_entries_[*chosen];
      menu_entries_.clear();
      const auto lane_named = [&](doc::NodeId id) -> std::string {
        for (const scene::TimelineLane &lane : lanes) {
          if (lane.id == id) {
            return lane.name;
          }
        }
        return {};
      };
      // The placed set a take row's entry names, while it is still placed.
      const auto plateset_named =
          [&](doc::NodeId key) -> const PlatesetGroup * {
        const auto found = std::find_if(
            p.built.platesets.begin(), p.built.platesets.end(),
            [&](const PlatesetGroup &group) { return group.key == key; });
        return found != p.built.platesets.end() ? &*found : nullptr;
      };
      const auto switch_plateset = [&](doc::NodeId key,
                                       TimelineLaneAction action) {
        const PlatesetGroup *const group = plateset_named(key);
        if (group == nullptr) {
          return;
        }
        // Every track to the state the row does not show yet.
        const bool all = std::all_of(
            group->lanes.begin(), group->lanes.end(), [&](doc::NodeId id) {
              return std::any_of(
                  lanes.begin(), lanes.end(),
                  [&](const scene::TimelineLane &lane) {
                    return lane.id == id &&
                           (action == TimelineLaneAction::ToggleMute
                                ? lane.muted
                                : lane.locked);
                  });
            });
        for (TimelineLaneEdit &edit :
             plateset_switch_edits(*group, lanes, action, !all)) {
          frame.lane_edits.push_back(std::move(edit));
        }
      };
      switch (static_cast<MenuCommand>(entry.command)) {
      case MenuCommand::TogglePlatesetOpen:
        if (!rows_state_.expanded_platesets.erase(entry.node)) {
          rows_state_.expanded_platesets.insert(entry.node);
        }
        break;
      case MenuCommand::TogglePlatesetMute:
        switch_plateset(entry.node, TimelineLaneAction::ToggleMute);
        break;
      case MenuCommand::TogglePlatesetLock:
        switch_plateset(entry.node, TimelineLaneAction::ToggleLock);
        break;
      case MenuCommand::DeletePlatesetTracks:
        if (const PlatesetGroup *const group = plateset_named(entry.node)) {
          for (const doc::NodeId lane : group->lanes) {
            frame.lane_edits.push_back(TimelineLaneEdit{
                TimelineLaneAction::DeleteClips, lane, {}});
          }
        }
        break;
      case MenuCommand::OpenPlatesetInAssetViewer:
        frame.view_clip = TimelineViewClipRequest{entry.node, false, true};
        break;
      case MenuCommand::SplitAtPlayhead:
        frame.clip_splits.push_back(
            TimelineClipSplit{entry.node, signed_frame(acting_frame)});
        break;
      case MenuCommand::SplitTrackAtPlayhead:
        split_track(entry.node, signed_frame(acting_frame));
        break;
      case MenuCommand::RippleDeleteSelection:
        ripple_delete(selected_clips);
        break;
      case MenuCommand::RippleDeleteClip:
        if (const ElementInfo *const clip = find_element(elements, entry.node)) {
          const std::array<const ElementInfo *, 1U> alone{clip};
          ripple_delete(alone);
        }
        break;
      case MenuCommand::InsertClipboard:
        frame.timeline_edits.emplace_back(TimelineInsertRequest{entry.node});
        break;
      case MenuCommand::OpenInAssetViewer:
        frame.view_clip = TimelineViewClipRequest{entry.node, false};
        break;
      case MenuCommand::OverwriteClipboard:
        frame.timeline_edits.emplace_back(TimelineOverwriteRequest{entry.node});
        break;
      case MenuCommand::RangeToClip:
        frame.range = RangeRequest{
            static_cast<core::u64>(std::max<core::i64>(entry.frame, 0)),
            static_cast<core::u64>(std::max<core::i64>(entry.end, 0)),
            timeline.loop};
        break;
      case MenuCommand::ClearKeys:
        clear_keys(entry.node);
        break;
      case MenuCommand::RenameTrack:
        rename_.begin(ctx, entry.node.value(), lane_named(entry.node));
        break;
      case MenuCommand::RenameLayer:
        if (const ElementInfo *const element =
                find_element(elements, entry.node)) {
          rename_.begin(ctx, entry.node.value(), layer_name(*element));
        }
        break;
      case MenuCommand::ToggleTrackMute:
        frame.lane_edits.push_back(TimelineLaneEdit{
            TimelineLaneAction::ToggleMute, entry.node, {}});
        break;
      case MenuCommand::ToggleTrackLock:
        frame.lane_edits.push_back(TimelineLaneEdit{
            TimelineLaneAction::ToggleLock, entry.node, {}});
        break;
      case MenuCommand::DeleteTrack:
        frame.lane_edits.push_back(TimelineLaneEdit{
            TimelineLaneAction::DeleteEmpty, entry.node, {}});
        break;
      case MenuCommand::DeleteTrackClips:
        frame.lane_edits.push_back(TimelineLaneEdit{
            TimelineLaneAction::DeleteClips, entry.node, {}});
        break;
      case MenuCommand::MoveClipsDeleteTrack:
        frame.lane_edits.push_back(TimelineLaneEdit{
            TimelineLaneAction::DeleteMoveClips, entry.node, {}, entry.other});
        break;
      case MenuCommand::NewTrack:
        frame.lane_edits.push_back(
            TimelineLaneEdit{TimelineLaneAction::Create, {}, {}});
        break;
      case MenuCommand::AddMarker:
        create_marker(p, entry.frame, scene::CueAction::None);
        break;
      case MenuCommand::AddHoldCue:
        create_marker(p, entry.frame, scene::CueAction::Hold);
        break;
      case MenuCommand::AddLoopCue:
        create_marker(p, entry.frame, scene::CueAction::Loop);
        break;
      case MenuCommand::SetIn:
        set_in(static_cast<core::u64>(std::max<core::i64>(entry.frame, 0)));
        break;
      case MenuCommand::SetOut:
        set_out(static_cast<core::u64>(std::max<core::i64>(entry.frame, 0)));
        break;
      case MenuCommand::ZoomFit:
        zoom_to_fit(p);
        break;
      case MenuCommand::ToggleRulerUnits:
        ruler_.frames = !ruler_.frames;
        break;
      case MenuCommand::RenameMarker:
        open_marker_name(entry.node);
        break;
      case MenuCommand::MarkerToHold:
        update_marker(entry.node, [](scene::TimelineMarker &marker) {
          marker.cue = scene::CueAction::Hold;
        });
        break;
      case MenuCommand::MarkerToLoop:
        update_marker(entry.node, [](scene::TimelineMarker &marker) {
          marker.cue = scene::CueAction::Loop;
        });
        break;
      case MenuCommand::MarkerToPlain:
        update_marker(entry.node, [](scene::TimelineMarker &marker) {
          marker.cue = scene::CueAction::None;
        });
        break;
      case MenuCommand::MarkerNextColor:
        update_marker(entry.node, [](scene::TimelineMarker &marker) {
          marker.color = static_cast<core::u8>(
              (marker.color + 1U) % scene::timeline_marker_colors);
        });
        break;
      case MenuCommand::GoToMarker:
        if (const auto index = marker_index(p, entry.node)) {
          frame.seek_to_frame = static_cast<core::u64>(
                  std::max<core::i64>(markers[*index].frame, 0));
        }
        break;
      case MenuCommand::DeleteMarker:
        if (std::find(markers_.selected.begin(), markers_.selected.end(),
                      entry.node) != markers_.selected.end()) {
          delete_markers(markers_.selected);
        } else {
          const std::array<doc::NodeId, 1U> alone{entry.node};
          delete_markers(alone);
        }
        break;
      case MenuCommand::EaseKeys:
        ease_selected_keys(p, static_cast<scene::KeyEase>(entry.frame));
        break;
      }
    } else if (!ctx.menu_open(menu_id)) {
      menu_entries_.clear();
    }
  }

  // ---------------------------------------------------------------------
  // Keyboard. The single-key vocabulary every editor shares, live only while
  // the timeline has the operator's attention and nothing is being typed.
  // A field that owned the keyboard when this frame began has just taken its
  // Enter as a commit; it must never also run the show.
  const bool shortcuts = timeline_focused_ && !frame.editing &&
                         !typing_at_start && !ctx.typing() &&
                         !ctx.pointer_down();
  if (shortcuts) {
    // Alt moves what is selected rather than the playhead: a frame, or a
    // second with Shift, as the arrows step the playhead.
    const core::i64 nudge =
        ctx.shift_held() ? static_cast<core::i64>(p.fps) : 1;
    if (ctx.alt_held() && ctx.key_pressed(gui::Key::Left)) {
      nudge_selection(p, -nudge);
    } else if (ctx.alt_held() && ctx.key_pressed(gui::Key::Right)) {
      nudge_selection(p, nudge);
    } else if (ctx.key_pressed(gui::Key::Left)) {
      frame.transport = ctx.shift_held() ? TransportRequest::JumpBack
                                         : TransportRequest::StepBack;
    } else if (ctx.key_pressed(gui::Key::Right)) {
      frame.transport = ctx.shift_held() ? TransportRequest::JumpForward
                                         : TransportRequest::StepForward;
    } else if (ctx.key_pressed(gui::Key::Home)) {
      frame.transport = TransportRequest::GoToStart;
    } else if (ctx.key_pressed(gui::Key::End)) {
      frame.transport = TransportRequest::GoToEnd;
    } else if (ctx.key_pressed(gui::Key::Enter)) {
      frame.transport = TransportRequest::Go;
    }
    if (ctx.key_pressed(gui::Key::Up) || ctx.key_pressed(gui::Key::Down)) {
      // Previous or next edit point: a cut, a range end, or a marker.
      const bool forward = ctx.key_pressed(gui::Key::Down);
      std::optional<core::i64> target;
      for (const core::i64 point : edit_points(p, doc::NodeId{}, true)) {
        if (forward ? point > playhead : point < playhead) {
          if (!target.has_value() ||
              (forward ? point < *target : point > *target)) {
            target = point;
          }
        }
      }
      if (target.has_value()) {
        frame.seek_to_frame = static_cast<core::u64>(std::max<core::i64>(*target, 0));
      }
    }
    if (ctx.key_pressed(gui::Key::TrimHeadToPlayhead)) {
      trim_to_playhead(p, scene::TimelineTrimEdge::Head);
    } else if (ctx.key_pressed(gui::Key::TrimTailToPlayhead)) {
      trim_to_playhead(p, scene::TimelineTrimEdge::Tail);
    }
    if (ctx.key_pressed(gui::Key::Split)) {
      for (const ElementInfo *clip : selected_clips) {
        if (playhead > clip->timing.start &&
            playhead <= clip->timing.last_frame()) {
          frame.clip_splits.push_back(
              TimelineClipSplit{clip->node, signed_frame(acting_frame)});
        }
      }
      // A selected track splits as a selected clip does.
      if (selected_clips.empty()) {
        const core::i64 cut = signed_frame(acting_frame);
        for (const doc::NodeId lane : selected_tracks) {
          const auto held = std::find_if(
              lanes.begin(), lanes.end(),
              [&](const scene::TimelineLane &each) { return each.id == lane; });
          if (held != lanes.end() && !held->locked &&
              track_splittable(elements, lane, cut)) {
            split_track(lane, cut);
          }
        }
      }
    }
    if ((ctx.key_pressed(gui::Key::Delete) ||
         ctx.key_pressed(gui::Key::Backspace)) &&
        !markers_.selected.empty()) {
      delete_markers(markers_.selected);
      frame.consume_delete = true;
    } else if (ctx.key_pressed(gui::Key::Delete) && ctx.shift_held() &&
               keys_.selected.empty() && clip_selection_valid &&
               ripple_ready(selected_clips)) {
      ripple_delete(selected_clips);
      frame.consume_delete = true;
    }
    for (const char key : ctx.shortcut_text()) {
      switch (key) {
      case ' ':
        frame.transport = TransportRequest::TogglePlay;
        break;
      case 'i':
      case 'I':
        set_in(acting_frame);
        break;
      case 'o':
      case 'O':
        set_out(acting_frame);
        break;
      case 'x':
      case 'X': {
        // Mark the selected clips: In/Out around them. With no clip selected,
        // a selected track marks every placed clip on it, as its bar shows.
        std::optional<std::pair<core::i64, core::i64>> marked;
        const auto mark = [&](const scene::ClipTiming &timing) {
          marked = marked.has_value()
                       ? std::pair{std::min(marked->first, timing.start),
                                   std::max(marked->second,
                                            timing.last_frame())}
                       : std::pair{timing.start, timing.last_frame()};
        };
        for (const ElementInfo *clip : selected_clips) {
          mark(clip->timing);
        }
        if (selected_clips.empty()) {
          for (const ElementInfo &element : elements) {
            if (element.kind == ElementKind::Media &&
                element.timing.placed() &&
                std::find(selected_tracks.begin(), selected_tracks.end(),
                          element.timeline_lane) != selected_tracks.end()) {
              mark(element.timing);
            }
          }
        }
        if (marked.has_value()) {
          frame.range = RangeRequest{
              static_cast<core::u64>(std::max<core::i64>(marked->first, 0)),
              static_cast<core::u64>(std::max<core::i64>(marked->second, 0)),
              timeline.loop};
        }
        break;
      }
      case 'm':
      case 'M': {
        // On an existing marker, M names it - the quickest way to label a
        // place the playhead was parked on.
        const auto here = std::find_if(
            markers.begin(), markers.end(),
            [&](const scene::TimelineMarker &marker) {
              return marker.frame == playhead;
            });
        if (here != markers.end()) {
          markers_.selected.assign(1U, here->id);
          open_marker_name(here->id);
        } else {
          create_marker(p, playhead, scene::CueAction::None);
        }
        break;
      }
      case 'q':
        create_marker(p, playhead, scene::CueAction::Hold);
        break;
      case 'Q':
        create_marker(p, playhead, scene::CueAction::Loop);
        break;
      case 's':
      case 'S':
        snapping_ = !snapping_;
        break;
      // Match frame: the selected clip under the playhead opens in the Asset
      // Viewer at the source frame it is showing.
      case 'f':
      case 'F':
        for (const ElementInfo *clip : selected_clips) {
          if (playhead >= clip->timing.start &&
              playhead <= clip->timing.last_frame()) {
            frame.view_clip = TimelineViewClipRequest{clip->node, true};
            break;
          }
        }
        break;
      // J, K and L shuttle as in every editor: backwards, stop, forwards.
      case 'j':
      case 'J':
        frame.transport = TransportRequest::PlayReverse;
        break;
      case 'k':
        frame.transport = TransportRequest::Pause;
        break;
      case 'l':
      case 'L':
        frame.transport = TransportRequest::PlayForward;
        break;
      case 'K':
        auto_key_ = !auto_key_;
        break;
      case '?':
        shortcut_sheet_open_ = !shortcut_sheet_open_;
        break;
      case 'v':
      case 'V':
        edit_tool_ = TimelineEditTool::Standard;
        break;
      case 'b':
      case 'B':
        edit_tool_ = TimelineEditTool::RippleTrim;
        break;
      case 'n':
      case 'N':
        edit_tool_ = TimelineEditTool::Roll;
        break;
      case 'y':
      case 'Y':
        edit_tool_ = TimelineEditTool::Slip;
        break;
      case 'u':
      case 'U':
        edit_tool_ = TimelineEditTool::Slide;
        break;
      case 'c':
      case 'C':
        edit_tool_ = TimelineEditTool::Razor;
        break;
      case '[':
        step_to_selected_key(p, false);
        break;
      case ']':
        step_to_selected_key(p, true);
        break;
      case '\\':
        zoom_to_fit(p);
        break;
      case '=':
      case '+':
        zoom_about_playhead(1.5);
        break;
      case '-':
      case '_':
        zoom_about_playhead(1.0 / 1.5);
        break;
      case ',':
        if (paste_ready) {
          frame.timeline_edits.emplace_back(TimelineInsertRequest{*target_lane});
        }
        break;
      case '.':
        if (paste_ready) {
          frame.timeline_edits.emplace_back(
              TimelineOverwriteRequest{*target_lane});
        }
        break;
      default:
        break;
      }
    }
  }
  if (timeline_focused_ && !frame.editing && !ctx.typing() &&
      !ctx.pointer_down() && !keys_.selected.empty()) {
    std::optional<TimelineKeyClipboardAction> action;
    if (ctx.key_pressed(gui::Key::Copy)) {
      action = TimelineKeyClipboardAction::Copy;
    } else if (ctx.key_pressed(gui::Key::Paste)) {
      action = TimelineKeyClipboardAction::Paste;
    }
    if (action.has_value()) {
      if (auto edit = key_clipboard_edit(*action); edit.has_value()) {
        frame.key_clipboard = std::move(*edit);
        frame.consume_clipboard = true;
      }
    }
  }
  // F9 eases the selected keys both ways, Shift+F9 in and Ctrl+F9 out, as
  // After Effects' Easy Ease chords do.
  if (timeline_focused_ && !frame.editing && !ctx.typing() &&
      !ctx.pointer_down() && !keys_.selected.empty() &&
      ctx.key_pressed(gui::Key::EasyEase)) {
    ease_selected_keys(p, ctx.ctrl_held()    ? scene::KeyEase::EaseOut
                          : ctx.shift_held() ? scene::KeyEase::EaseIn
                                             : scene::KeyEase::EasyEase);
  }
  if (timeline_focused_ && !frame.editing && !keys_.selected.empty() &&
      ctx.key_pressed(gui::Key::Delete)) {
    TimelineKeyBatchEdit batch{};
    batch.action = TimelineKeyBatchAction::Delete;
    batch.keys.reserve(keys_.selected.size());
    for (const SelectedKey &key : keys_.selected) {
      batch.keys.push_back(
          TimelineKeyAddress{key.target, key.component, key.frame});
    }
    std::sort(batch.keys.begin(), batch.keys.end());
    batch.keys.erase(std::unique(batch.keys.begin(), batch.keys.end()),
                     batch.keys.end());
    frame.key_batch = std::move(batch);
    frame.consume_delete = true;
  }
}

void TimelinePanel::ease_selected_keys(Pass &p, scene::KeyEase ease) {
  if (keys_.selected.empty()) {
    return;
  }
  TimelineKeyBatchEdit batch{};
  batch.action = TimelineKeyBatchAction::Ease;
  batch.keys = selected_addresses();
  batch.ease = ease;
  p.frame.key_batch = std::move(batch);
}

std::optional<TimelinePanel::SelectedKey>
TimelinePanel::key_under_pointer(Pass &p) const {
  const float x = p.ctx.pointer_x();
  const float y = p.ctx.pointer_y();
  const HitTarget *const owner = p.hits.owner(x, y);
  if (owner == nullptr || owner->priority != HitPriority::Key ||
      tag_part(owner->tag) != RowPart::Key) {
    return std::nullopt;
  }
  // The owner is the nearest key mark; the drawn keys say which key it is.
  std::optional<SelectedKey> nearest;
  float nearest_distance = key_hit_radius;
  for (const FrameScratch::VisibleKeyPoint &point : p.scratch.visible_keys) {
    const float distance = std::hypot(point.x - x, point.y - y);
    if (distance <= nearest_distance) {
      nearest = point.key;
      nearest_distance = distance;
    }
  }
  return nearest;
}

bool TimelinePanel::selection_has_cubic(const Pass &p) const {
  for (const SelectedKey &key : keys_.selected) {
    for (const scene::ChannelKeyframes &channel : p.input.keyframes) {
      if (channel.target != key.target || channel.component != key.component) {
        continue;
      }
      const auto found = std::lower_bound(channel.frames.begin(),
                                          channel.frames.end(), key.frame);
      const auto index =
          static_cast<std::size_t>(found - channel.frames.begin());
      if (found != channel.frames.end() && *found == key.frame &&
          index < channel.interpolations.size() &&
          channel.interpolations[index] == scene::KeyInterpolation::Cubic) {
        return true;
      }
    }
  }
  return false;
}

// [ and ] step the playhead to the previous or next key on any channel of the
// selected layers - their transforms, looks and effects - or of the selected
// surface. Nothing selected, or no key that way, leaves it where it is.
void TimelinePanel::step_to_selected_key(Pass &p, bool forward) {
  const std::span<const doc::NodeId> selection = p.input.selection;
  std::vector<doc::NodeId> targets(selection.begin(), selection.end());
  for (const scene::EffectInstance &effect : p.input.effects) {
    if (selected(effect.target, selection)) {
      targets.push_back(effect.source);
    }
  }
  if (p.input.selected_surface != doc::NodeId{}) {
    targets.push_back(p.input.selected_surface);
  }
  std::optional<core::i64> target;
  for (const scene::ChannelKeyframes &channel : p.input.keyframes) {
    if (std::find(targets.begin(), targets.end(), channel.target) !=
        targets.end()) {
      nearer_key(target, channel.frames, p.playhead, forward);
    }
  }
  if (target.has_value()) {
    p.frame.seek_to_frame =
        static_cast<core::u64>(std::max<core::i64>(*target, 0));
  }
}

} // namespace mediaserver::editor
