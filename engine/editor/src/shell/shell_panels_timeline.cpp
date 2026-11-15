// The Cues, Automation, Macros and Timeline panels.

#include "shell_frame.hpp"

namespace mediaserver::editor {

void EditorShell::Frame::draw_cues_panel(const gui::Rect &bounds) {
  const CuesPanelFrame cues = impl.cues_panel.draw(
      ui, bounds, timeline, timeline_markers,
      impl.timeline_panel.selected_markers());
  route_marker_edits(cues.marker_edits);
  if (cues.transport != TransportRequest::None) {
    result.transport = cues.transport;
  }
  if (cues.seek_to_frame.has_value()) {
    result.seek_to_frame = cues.seek_to_frame;
  }
  if (cues.cues_armed.has_value()) {
    result.cues_armed = cues.cues_armed;
  }
  if (cues.selected.has_value()) {
    impl.timeline_panel.select_marker(*cues.selected);
  }
  editor_control_editing = editor_control_editing || cues.editing;
  delete_consumed = delete_consumed || cues.consume_delete;
  return;
}

void EditorShell::Frame::draw_automation_panel(const gui::Rect &bounds) {
  constexpr std::array<std::string_view, 2U> rows{
      "No automation lanes", "Use Timeline property diamonds"};
  draw_placeholder(bounds, "AUTOMATION LANES", rows);
  return;
}

void EditorShell::Frame::draw_macros_panel(const gui::Rect &bounds) {
  constexpr std::array<std::string_view, 2U> rows{
      "No macros configured", "+  NEW MACRO (not available yet)"};
  draw_placeholder(bounds, "SHOW MACROS", rows);
  return;
}

void EditorShell::Frame::draw_timeline_panel(const gui::Rect &bounds) {
  timeline_panel_drawn = true;
  timeline_panel_bounds = bounds;
  auto timeline_elements = elements;
  if (project_settings.stage_node != doc::NodeId{}) {
    ElementInfo environment{};
    environment.kind = ElementKind::Environment;
    environment.node = project_settings.stage_node;
    environment.display_name = "Environment";
    environment.environment = project_settings.environment;
    timeline_elements.insert(timeline_elements.begin(), environment);
  }
  const TimelinePanelFrame timeline_frame = impl.timeline_panel.draw(
      ui, bounds,
      TimelineDrawInput{
          .timeline = timeline,
          .keyframes = keyframes,
          .elements = timeline_elements,
          .selection = impl.scene_selection.members(),
          .primary = impl.anchor(),
          .effects = effects,
          .surfaces = feed_map.surfaces,
          .selected_surface =
              impl.stage_surface_selection.value_or(doc::NodeId{}),
          .lanes = timeline_lanes,
          .clipboard_span = clip_clipboard_span,
          .key_scale_preview = key_scale_preview,
          .filmstrips = filmstrips,
          .markers = timeline_markers,
          .moments = timeline_moments,
      });
  result.filmstrip_requests = timeline_frame.filmstrip_requests;
  route_marker_edits(timeline_frame.marker_edits);
  if (timeline_frame.cues_armed.has_value()) {
    result.cues_armed = timeline_frame.cues_armed;
  }
  result.filmstrip_retain = timeline_frame.filmstrip_retain;
  if (timeline_frame.view_clip.has_value()) {
    open_clip_in_viewer(*timeline_frame.view_clip);
  }
  if (timeline_frame.seek_to_frame.has_value()) {
    result.seek_to_frame = timeline_frame.seek_to_frame;
  }
  if (timeline_frame.transport != TransportRequest::None) {
    result.transport = timeline_frame.transport;
  }
  result.range = timeline_frame.range;
  result.show_rate = timeline_frame.show_rate;
  result.stage_edit_preview = timeline_frame.edit_preview;
  result.stage_solo = timeline_frame.solo;
  // A clip row's Full/Half switch is the Inspector's Resolution
  // setting, authored the same way, one undo step per switch.
  for (const TimelineMediaResolutionEdit &edit :
       timeline_frame.media_resolutions) {
    result.generic_edits.push_back(GenericEdit{
        edit.clip, std::string{scene::schema::media_resolution}, 0.0,
        impl.begin_gesture(),
        std::string{edit.resolution == scene::MediaResolution::Half
                        ? scene::schema::media_resolution_half
                        : scene::schema::media_resolution_full}});
  }
  pending_timeline_layer_drop = timeline_frame.layer_drop;
  editor_control_editing = editor_control_editing || timeline_frame.editing;
  delete_consumed = delete_consumed || timeline_frame.consume_delete;
  clipboard_consumed =
      clipboard_consumed || timeline_frame.consume_clipboard;
  if (timeline_frame.key_clipboard.has_value() &&
      !impl.scene_link_details_open && !impl.blender_connection_open && !impl.preferences_panel.is_open() &&
      !impl.project_settings_panel.is_open()) {
    request_key_clipboard(*timeline_frame.key_clipboard);
  }
  if (timeline_frame.selection.has_value()) {
    const TimelineSelectionRequest request =
        timeline_frame.selection.value();
    // A layer chosen on the Timeline brings its Scene Layer into view in
    // the Compositor, without choosing it there.
    if (request.action == SelectionAction::Replace && !request.surface) {
      impl.compositor_panel.reveal_element(request.node, ui.time_seconds());
    }
    if (request.action == SelectionAction::Clear) {
      impl.stage_surface_selection.reset();
      impl.clear_scene_selection(true);
      impl.group_nodes.clear();
      impl.group_start.clear();
    } else if (request.surface) {
      const output::ProjectionSurface *const requested_surface =
          feed_map.surface_for(request.node);
      if (requested_surface == nullptr || requested_surface->locked) {
        return;
      }
      const bool deselect =
          request.action == SelectionAction::Toggle &&
          impl.stage_surface_selection == request.node;
      impl.stage_surface_selection =
          deselect ? std::optional<doc::NodeId>{}
                   : std::optional<doc::NodeId>{request.node};
      if (!deselect) {
        impl.feed_mapping_panel.select_surface(request.node);
      }
      impl.scene_selection.clear();
      impl.group_nodes.clear();
      impl.group_start.clear();
      impl.delete_domain =
          deselect ? DeleteDomain::Neutral : DeleteDomain::Surface;
    } else {
      const ElementInfo *const requested_element =
          find_element(timeline_elements, request.node);
      if (requested_element == nullptr || requested_element->locked) {
        return;
      }
      impl.stage_surface_selection.reset();
      switch (request.action) {
      case SelectionAction::Replace:
        impl.select_only(request.node);
        break;
      case SelectionAction::Toggle:
        impl.toggle(request.node);
        break;
      case SelectionAction::Range:
        impl.select_range(request.node, request.visible_order);
        break;
      case SelectionAction::Clear:
        break;
      }
    }
  }
  if (!timeline_frame.keys.empty()) {
    // A value scrubbed in a Timeline field keys on every frame it moves;
    // the whole scrub is one gesture, so it undoes in one step.
    const core::u64 gesture = timeline_frame.editing
                                  ? impl.begin_gesture()
                                  : impl.next_edit_gesture++;
    for (const TimelineKeyEdit &key : timeline_frame.keys) {
      result.keys.push_back(KeyIntent{key.target, key.component, key.frame,
                                      key.value, key.clear, gesture,
                                      key.erase, key.parameter});
    }
  }
  if (!timeline_frame.key_moves.empty()) {
    const core::u64 gesture = impl.next_edit_gesture++;
    for (const TimelineKeyMoveEdit &move : timeline_frame.key_moves) {
      result.key_moves.push_back(KeyMove{move.target, move.component,
                                         move.from_frame, move.to_frame,
                                         gesture, move.parameter});
    }
  }
  if (timeline_frame.key_duplicate.has_value()) {
    result.key_duplicate = KeyDuplicateRequest{
        authored_stage.id, timeline_frame.key_duplicate->keys,
        timeline_frame.key_duplicate->delta, impl.next_edit_gesture++};
  }
  if (timeline_frame.key_batch.has_value()) {
    result.key_batch = KeyBatchRequest{
        authored_stage.id, std::move(*timeline_frame.key_batch),
        impl.next_edit_gesture++};
  }
  if (timeline_frame.key_scale_preview.has_value()) {
    result.key_scale_preview = KeyScalePreviewRequest{
        authored_stage.id,
        std::move(*timeline_frame.key_scale_preview)};
  }
  if (!timeline_frame.clip_edits.empty()) {
    if (impl.clip_edit_gesture == 0U) {
      impl.clip_edit_gesture = impl.next_edit_gesture++;
    }
    for (const TimelineClipEdit &edit : timeline_frame.clip_edits) {
      result.clip_timings.push_back(ClipTimingEdit{
          edit.clip, edit.timing, impl.clip_edit_gesture, edit.lane});
    }
  }
  // A clip or track bar dragged to a new time and a new place in the
  // stack is one undo step: the reorder joins the retime's transaction.
  if (!timeline_frame.clip_edits.empty() &&
      (!timeline_frame.layer_reorders.empty() ||
       !timeline_frame.lane_edits.empty()) &&
      impl.lane_edit_gesture == 0U) {
    impl.lane_edit_gesture = impl.clip_edit_gesture;
  }
  if (!impl.timeline_panel.dragging_clip()) {
    // The gesture ends with the pointer, so the next drag is its own
    // transaction even when it moves the same clip again.
    impl.clip_edit_gesture = 0U;
  }
  if (!timeline_frame.clip_splits.empty()) {
    const core::u64 gesture = impl.next_edit_gesture++;
    for (const TimelineClipSplit &split : timeline_frame.clip_splits) {
      result.clip_splits.push_back(
          ClipSplitIntent{split.clip, split.frame, gesture});
    }
  }
  for (const TimelineRename &rename : timeline_frame.renames) {
    result.renames.push_back(ElementRenameIntent{
        rename.node, rename.name, impl.next_edit_gesture++});
    impl.history_context = HistoryContext::Scene;
  }
  if (!timeline_frame.element_flags.empty()) {
    if (impl.lane_edit_gesture == 0U) {
      impl.lane_edit_gesture = impl.next_edit_gesture++;
    }
    for (const TimelineElementFlagEdit &edit :
         timeline_frame.element_flags) {
      // A surface's padlock is its Feed Mapping lock: one state, one
      // undo domain, whichever panel set it.
      if (feed_map.surface_for(edit.element) != nullptr) {
        if (edit.flag == TimelineElementFlag::Locked) {
          result.lock_edits.push_back(LockEdit{
              edit.element, edit.value, impl.lane_edit_gesture});
          if (edit.value &&
              impl.stage_surface_selection == edit.element) {
            impl.stage_surface_selection.reset();
            impl.delete_domain = DeleteDomain::Neutral;
          }
        }
        continue;
      }
      result.element_flags.push_back(
          ElementFlagIntent{authored_stage.id, edit.element, edit.flag,
                            edit.value, impl.lane_edit_gesture});
    }
    impl.history_context = HistoryContext::Scene;
  }
  if (!timeline_frame.lane_edits.empty()) {
    if (impl.lane_edit_gesture == 0U) {
      impl.lane_edit_gesture = impl.next_edit_gesture++;
    }
    for (const TimelineLaneEdit &edit : timeline_frame.lane_edits) {
      result.lane_edits.push_back(TimelineLaneEditIntent{
          edit.action, authored_stage.id, edit.lane, edit.name,
          impl.lane_edit_gesture, edit.destination, edit.frame});
    }
    impl.history_context = HistoryContext::Scene;
  }
  if (!timeline_frame.layer_reorders.empty()) {
    if (impl.lane_edit_gesture == 0U) {
      impl.lane_edit_gesture = impl.next_edit_gesture++;
    }
    for (const TimelineLayerReorder &edit :
         timeline_frame.layer_reorders) {
      result.layer_orders.push_back(TimelineLayerOrderIntent{
          authored_stage.id, edit.layer, edit.destination, edit.placement,
          impl.lane_edit_gesture});
    }
    impl.history_context = HistoryContext::Scene;
  }
  if (!timeline_frame.lane_editing) {
    impl.lane_edit_gesture = 0U;
  }
  if (!timeline_frame.timeline_edits.empty()) {
    const core::u64 transaction = impl.next_edit_gesture++;
    const scene::MediaFrameRate show_rate{
        timeline.rate_numerator, timeline.rate_denominator};
    for (const TimelineEditRequest &request :
         timeline_frame.timeline_edits) {
      result.timeline_edits.push_back(TimelineEditIntent{
          authored_stage.id, request, show_rate, timeline.show_frame,
          transaction});
    }
    impl.history_context = HistoryContext::Scene;
  }
  return;
}

} // namespace mediaserver::editor
