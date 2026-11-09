// The Compositor, Hardware Map, Feed Mapping, Output Monitor and Live Data panels.

#include "shell_frame.hpp"

#include "native_dialogs.hpp"

namespace mediaserver::editor {

void EditorShell::Frame::draw_compositor_panel(const gui::Rect &bounds) {
  compositor_panel_bounds = bounds;
  compositor_panel_drawn = true;
  const CompositorFrame graph_frame =
      impl.compositor_panel.draw(ui, bounds, compositor_graph, elements);
  if (graph_frame.selection.has_value()) {
    impl.delete_domain = DeleteDomain::Compositor;
    // One Scene Layer chosen points the Timeline at its layer or track.
    // Each panel keeps its own selection.
    if (graph_frame.selection->kind == CompositorSelectionKind::Node &&
        impl.compositor_panel.selected_nodes().size() == 1U) {
      if (const compositor::GraphNode *const chosen =
              compositor_graph.node(graph_frame.selection->node)) {
        const auto *const layer =
            std::get_if<compositor::SceneLayerParameters>(
                &chosen->parameters);
        if (layer != nullptr && layer->scene_node != doc::NodeId{}) {
          impl.timeline_panel.reveal(layer->scene_node, ui.time_seconds());
        }
      }
    }
    // Selection presses are reported even when the same node is clicked
    // again (and when empty canvas clears selection). This gives the
    // operator a non-authoring way to choose which per-context redo stack
    // the global history actions navigate.
    impl.history_context = HistoryContext::Compositor;
  }
  delete_consumed = delete_consumed || graph_frame.consume_delete;
  if (!compositor_locked) {
    auto add_node = graph_frame.add_node;
    if (add_node.has_value() &&
        add_node->kind == compositor::NodeKind::SceneLayer) {
      add_node->scene_node = impl.anchor();
      if (add_node->scene_node == doc::NodeId{}) {
        add_node.reset();
        impl.compositor_warning_until = ui.time_seconds() + 1.5;
      }
    }
    if (add_node.has_value()) {
      result.compositor_adds.push_back(*add_node);
    }
    result.compositor_moves = graph_frame.move_nodes;
    result.compositor_connect = graph_frame.connect_ports;
    result.compositor_rewire = graph_frame.rewire;
    result.compositor_waypoints = graph_frame.waypoints;
    result.compositor_property_edit = graph_frame.property_edit;
    result.compositor_delete = graph_frame.remove;
    result.compositor_clipboard = graph_frame.clipboard;
    result.compositor_rename = graph_frame.rename;
    result.compositor_comment = graph_frame.comment;
    result.compositor_collapse = graph_frame.collapse;
    result.compositor_bypass = graph_frame.bypass;
    result.compositor_reorder = graph_frame.reorder;
    if (graph_frame.add_node.has_value() ||
        !graph_frame.move_nodes.empty() ||
        graph_frame.connect_ports.has_value() ||
        graph_frame.rewire.has_value() ||
        graph_frame.waypoints.has_value() ||
        graph_frame.remove.has_value() ||
        graph_frame.clipboard.has_value() ||
        graph_frame.rename.has_value() ||
        graph_frame.comment.has_value() ||
        graph_frame.collapse.has_value() ||
        graph_frame.bypass.has_value() ||
        graph_frame.reorder.has_value()) {
      result.compositor_gesture = impl.next_edit_gesture++;
    }
    if (!graph_frame.move_nodes.empty() &&
        graph_frame.numeric_nudge.has_value() &&
        !graph_frame.property_edit.has_value()) {
      // Arrow-key nudges of the same selection in the same direction
      // join one undo step, as numeric nudges do.
      core::u64 selection = 0xcbf29ce484222325ULL;
      for (const doc::NodeId node :
           impl.compositor_panel.selected_nodes()) {
        selection = (selection ^ node.value()) * 0x100000001b3ULL;
      }
      result.compositor_gesture = begin_semantic_nudge(
          *graph_frame.numeric_nudge, HistoryContext::Compositor,
          selection);
    }
    if (graph_frame.property_edit.has_value()) {
      if (graph_frame.numeric_nudge.has_value()) {
        const CompositorSelection selected =
            impl.compositor_panel.selection();
        const core::u64 selection =
            selected.node.value() ^
            (static_cast<core::u64>(selected.kind) << 60U);
        result.compositor_gesture = begin_semantic_nudge(
            *graph_frame.numeric_nudge, HistoryContext::Compositor,
            selection);
        impl.compositor_edit_gesture = 0U;
      } else {
        if (impl.compositor_edit_gesture == 0U) {
          impl.compositor_edit_gesture = impl.next_edit_gesture++;
        }
        result.compositor_gesture = impl.compositor_edit_gesture;
      }
    }
    if (!graph_frame.editing) {
      impl.compositor_edit_gesture = 0U;
    }
    if (ui.time_seconds() < impl.compositor_warning_until) {
      draw_panel_warning(ui, bounds, 34.0F, 286.0F, false,
                         "SELECT A SCENE ELEMENT FOR + LAYER");
    }
  } else {
    impl.compositor_edit_gesture = 0U;
    draw_panel_warning(ui, bounds, 34.0F, 360.0F, true,
                       "PROJECT GRAPH - EXIT PREVIEW TO EDIT");
  }
  return;
}

void EditorShell::Frame::draw_hardware_mapping_panel(const gui::Rect &bounds) {
  HardwareMappingFrame mapping =
      // The authored canvas, as Feed Mapping uses: `stage_scene` carries
      // the Stage viewport's own pixel size in its place.
      impl.hardware_mapping_panel.draw(ui, bounds, hardware_map,
                                       hardware_runtime, sync_policy,
                                       scene.canvas);
  if (mapping.selection.has_value()) {
    impl.delete_domain = DeleteDomain::Hardware;
  }
  delete_consumed = delete_consumed || mapping.consume_delete;
  if (mapping.place.has_value()) {
    mapping.place->gesture = impl.next_edit_gesture++;
    result.hardware_places.push_back(mapping.place.value());
  }
  if (mapping.move.has_value()) {
    mapping.move->gesture = impl.next_edit_gesture++;
    result.hardware_moves.push_back(mapping.move.value());
  }
  if (mapping.link.has_value()) {
    if (mapping.link->output_binding.has_value()) {
      topology::HardwareOutputBinding &binding =
          *mapping.link->output_binding;
      // A moved cable hands its feed, and the region mapped to it, to the
      // output it moved to - when it was its old output's last cable, so
      // the old binding goes with it.
      const topology::HardwareLink *const replaced =
          hardware_map.link(mapping.link->replaces);
      const bool replaced_shared =
          replaced != nullptr &&
          std::any_of(hardware_map.links.begin(), hardware_map.links.end(),
                      [&](const topology::HardwareLink &other) {
                        return other.node != replaced->node &&
                               other.from == replaced->from;
                      });
      const topology::HardwareOutputBinding *const replaced_binding =
          replaced != nullptr && !replaced_shared
              ? hardware_map.output_binding_for(replaced->from.device,
                                                replaced->from.port)
              : nullptr;
      if (binding.node == doc::NodeId{} && replaced_binding != nullptr) {
        binding.feed_index = replaced_binding->feed_index;
      } else if (binding.node == doc::NodeId{}) {
        std::optional<core::u32> free_feed;
        if (feed_map.maps.size() < output::max_output_maps) {
          for (core::u32 index = 0U; index < output::max_output_maps;
               ++index) {
            if (feed_map.map_for(index) == nullptr) {
              free_feed = index;
              break;
            }
          }
        }
        if (free_feed.has_value()) {
          binding.feed_index = *free_feed;
        } else {
          // A physical cable must never be committed without the output
          // region that makes it renderable. The project can expose up to
          // max_output_maps regions, so a full map rejects this gesture.
          mapping.link.reset();
        }
      }
      if (mapping.link.has_value() && scene::valid(scene.canvas)) {
        mapping.link->output_region = scene::whole(scene.canvas);
      } else if (mapping.link.has_value()) {
        mapping.link.reset();
      }
    }
  }
  if (mapping.link.has_value()) {
    mapping.link->gesture = impl.next_edit_gesture++;
    result.hardware_links.push_back(mapping.link.value());
  }
  if (mapping.property_edit.has_value()) {
    if (impl.hardware_edit_gesture == 0U) {
      impl.hardware_edit_gesture = impl.next_edit_gesture++;
    }
    mapping.property_edit->gesture = impl.hardware_edit_gesture;
    result.hardware_property_edits.push_back(mapping.property_edit.value());
  }
  if (mapping.link_region_edit.has_value()) {
    if (impl.hardware_edit_gesture == 0U) {
      impl.hardware_edit_gesture = impl.next_edit_gesture++;
    }
    mapping.link_region_edit->gesture = impl.hardware_edit_gesture;
    result.hardware_link_region_edits.push_back(
        mapping.link_region_edit.value());
  }
  if (mapping.control_port_edit.has_value()) {
    if (mapping.place.has_value() && mapping.place->machine.has_value()) {
      // The first detected machine placed brings the control range to
      // where it listens; one gesture, so one undo takes back both.
      mapping.control_port_edit->gesture = mapping.place->gesture;
    } else {
      if (impl.hardware_edit_gesture == 0U) {
        impl.hardware_edit_gesture = impl.next_edit_gesture++;
      }
      mapping.control_port_edit->gesture = impl.hardware_edit_gesture;
    }
    result.hardware_control_port_edit = mapping.control_port_edit;
  }
  if (mapping.canvas_edit.has_value()) {
    if (impl.hardware_edit_gesture == 0U) {
      impl.hardware_edit_gesture = impl.next_edit_gesture++;
    }
    mapping.canvas_edit->gesture = impl.hardware_edit_gesture;
    result.hardware_canvas_edit = mapping.canvas_edit;
  }
  if (mapping.sync_policy_edit.has_value()) {
    if (impl.hardware_edit_gesture == 0U) {
      impl.hardware_edit_gesture = impl.next_edit_gesture++;
    }
    mapping.sync_policy_edit->gesture = impl.hardware_edit_gesture;
    result.sync_policy_edit = mapping.sync_policy_edit;
  }
  if (mapping.output_binding_edit.has_value()) {
    if (impl.hardware_edit_gesture == 0U) {
      impl.hardware_edit_gesture = impl.next_edit_gesture++;
    }
    mapping.output_binding_edit->gesture = impl.hardware_edit_gesture;
    result.hardware_output_binding_edits.push_back(
        *mapping.output_binding_edit);
  }
  if (mapping.output_binding_delete.has_value()) {
    mapping.output_binding_delete->gesture =
        impl.next_edit_gesture++;
    result.hardware_output_binding_deletes.push_back(
        *mapping.output_binding_delete);
  }
  if (mapping.remove.has_value()) {
    mapping.remove->gesture = impl.next_edit_gesture++;
    result.hardware_deletes.push_back(mapping.remove.value());
  }
  result.search_processors =
      result.search_processors || mapping.search_processors;
  if (mapping.processor_edid.has_value()) {
    result.processor_edid_writes.push_back(*mapping.processor_edid);
  }
  if (mapping.incompatible_link_attempted) {
    impl.hardware_warning_until = ui.time_seconds() + 1.5;
  }
  if (!mapping.editing) {
    impl.hardware_edit_gesture = 0U;
  }
  if (ui.time_seconds() < impl.hardware_warning_until) {
    draw_panel_warning(ui, bounds, 34.0F, 286.0F, false,
                       "PORTS ARE INCOMPATIBLE OR ALREADY USED");
  }
  return;
}

void EditorShell::Frame::draw_feed_mapping_panel(const gui::Rect &bounds) {
  FeedMappingFrame mapping =
      impl.feed_mapping_panel.draw(ui, bounds, scene.canvas, feed_map,
                                   timeline.program_frame, &hardware_map,
                                   &hardware_runtime);
  if (mapping.surface_selection.has_value()) {
    const output::ProjectionSurface *const selected_surface =
        feed_map.surface_for(*mapping.surface_selection);
    if (selected_surface != nullptr && !selected_surface->locked) {
      impl.stage_surface_selection = mapping.surface_selection;
      impl.scene_selection.clear();
      impl.group_nodes.clear();
      impl.group_start.clear();
      impl.delete_domain = DeleteDomain::Surface;
    }
  }
  if (mapping.selection.has_value()) {
    impl.delete_domain = DeleteDomain::Feed;
    impl.program_output_map_node = *mapping.selection;
  }
  delete_consumed = delete_consumed || mapping.consume_delete;
  if (mapping.add.has_value()) {
    mapping.add->gesture = impl.next_edit_gesture++;
    result.feed_map_adds.push_back(mapping.add.value());
  }
  if (mapping.edit.has_value()) {
    if (impl.feed_mapping_edit_gesture == 0U) {
      impl.feed_mapping_edit_gesture = impl.next_edit_gesture++;
    }
    mapping.edit->gesture = impl.feed_mapping_edit_gesture;
    result.feed_map_edits.push_back(mapping.edit.value());
  }
  if (mapping.remove.has_value()) {
    mapping.remove->gesture = impl.next_edit_gesture++;
    result.feed_map_deletes.push_back(mapping.remove.value());
  }
  if (mapping.projection_edit.has_value()) {
    if (impl.feed_mapping_edit_gesture == 0U) {
      impl.feed_mapping_edit_gesture = impl.next_edit_gesture++;
    }
    mapping.projection_edit->gesture = impl.feed_mapping_edit_gesture;
    result.feed_projection_edits.push_back(mapping.projection_edit.value());
  }
  if (mapping.organize.has_value()) {
    mapping.organize->gesture = impl.next_edit_gesture++;
    result.feed_organize.push_back(std::move(*mapping.organize));
  }
  if (mapping.layout.has_value()) {
    mapping.layout->gesture = impl.next_edit_gesture++;
    result.feed_layout = std::move(mapping.layout);
  }
  if (mapping.stage_edit.has_value()) {
    if (impl.feed_mapping_edit_gesture == 0U) {
      impl.feed_mapping_edit_gesture = impl.next_edit_gesture++;
    }
    mapping.stage_edit->gesture = impl.feed_mapping_edit_gesture;
    result.feed_stage = std::move(mapping.stage_edit);
  }
  if (mapping.surface_add.has_value()) {
    auto request = std::move(*mapping.surface_add);
    if (request.shape == output::ProjectionSurfaceShape::ImportedMesh) {
      if (!request.mesh_source.empty()) {
        std::string source = request.mesh_source;
        enqueue_model(std::move(source), {}, std::move(request), {});
      } else {
        open_file_dialog([] { return choose_projection_mesh(); },
                         [request = std::move(request)](Frame &frame,
                                                        std::string source) {
                           frame.enqueue_model(std::move(source), {}, request,
                                               {});
                         });
      }
    } else {
      request.gesture = impl.next_edit_gesture++;
      result.feed_surface_adds.push_back(std::move(request));
    }
  }
  if (mapping.surface_edit.has_value()) {
    if (impl.feed_mapping_edit_gesture == 0U) {
      impl.feed_mapping_edit_gesture = impl.next_edit_gesture++;
    }
    mapping.surface_edit->gesture = impl.feed_mapping_edit_gesture;
    route_surface_edit(std::move(mapping.surface_edit.value()));
  }
  if (mapping.mesh_browse.has_value()) {
    auto request = std::move(*mapping.mesh_browse);
    open_file_dialog([] { return choose_projection_mesh(); },
                     [request = std::move(request)](Frame &frame,
                                                    std::string source) {
                       frame.enqueue_model(std::move(source), {}, {}, request);
                     });
  }
  if (mapping.surface_remove.has_value()) {
    if (impl.stage_surface_selection == mapping.surface_remove->node) {
      impl.stage_surface_selection.reset();
    }
    mapping.surface_remove->gesture = impl.next_edit_gesture++;
    result.feed_surface_deletes.push_back(mapping.surface_remove.value());
  }
  if (mapping.surface_lock.has_value()) {
    result.lock_edits.push_back(
        LockEdit{mapping.surface_lock->node, mapping.surface_lock->locked,
                 impl.next_edit_gesture++});
    if (mapping.surface_lock->locked &&
        impl.stage_surface_selection == mapping.surface_lock->node) {
      impl.stage_surface_selection.reset();
      impl.delete_domain = DeleteDomain::Neutral;
    }
  }
  if (!mapping.editing) {
    impl.feed_mapping_edit_gesture = 0U;
  }
  return;
}

void EditorShell::Frame::draw_output_panel(const gui::Rect &bounds) {
  OutputMonitorState monitor = output_monitor;
  // The Output Monitor reports the live wall, so it stays on Program even
  // while the operator scrubs Preview somewhere else entirely.
  monitor.frame = timeline.program_frame;
  monitor.width = scene.canvas.width;
  monitor.height = scene.canvas.height;
  monitor.frames_per_second =
      static_cast<float>(timeline.rate_numerator) /
      static_cast<float>(std::max(timeline.rate_denominator, 1U));
  monitor.playing = timeline.program_playing;
  OutputMonitorFrame output = impl.output_panel.draw(ui, bounds, monitor);
  result.check_render_conformance = output.check_render_conformance;
  if (output.lifecycle.has_value()) {
    result.output_lifecycle = std::move(output.lifecycle.value());
  }
  return;
}

void EditorShell::Frame::draw_live_data_panel(const gui::Rect &bounds) {
  LiveDataState live = live_data;
  // The panel reports the frame being shown, and the authored policy the
  // Director is actually running - not the operator's unapplied draft.
  live.show_frame = timeline.program_frame;
  live.canvas = scene.canvas;
  live.icvfx = scene.renderer_settings.icvfx;
  impl.live_data_panel.draw(ui, bounds, live);
  return;
}

} // namespace mediaserver::editor
