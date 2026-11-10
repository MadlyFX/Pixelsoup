// Laying out the dock, then landing whatever was dragged between panels.

#include "shell_frame.hpp"

#include "native_dialogs.hpp"

namespace mediaserver::editor {

void EditorShell::Frame::dock_panels() {
  impl.dock_host.update(ui, editor_dock_area(width, height, !impl.history_hidden),
                        [this](gui::PanelId panel, const gui::Rect &bounds) {
                          draw_panel(panel, bounds);
                        },
                        editor_dock_metrics);
  impl.dock_host.overlay(ui);

  // Dock mutations are deferred until panel traversal is finished. Activating
  // a sibling tab while its leaf is being drawn would invalidate the layout
  // state the host is currently walking.
  if (pending_inspector_panel.has_value()) {
    const gui::PanelId requested =
        *pending_inspector_panel == InspectorPanelRequest::Color
            ? panel_color_correction
            : panel_texture;
    static_cast<void>(impl.docks.show(requested));
  }

  // Resolve drag feedback after every dock has drawn. This avoids panel order
  // deciding whether the Inspector sees the frame in which the drag starts,
  // and keeps the target above the inspector controls it is explaining.
  if (impl.effects_panel.dragging() && inspector_panel_drawn &&
      impl.anchor() != doc::NodeId{}) {
    const ElementInfo *const drop_target =
        find_element(elements, impl.anchor());
    std::array<char, 64U> target_title{};
    draw_effect_drop_target(
        ui, inspector_panel_bounds,
        effect_target_name(drop_target, target_title));
  }
  // Both hover feedback and release use the drawn target geometry and the same
  // compatibility policy. Resolving here makes dock traversal order irrelevant.
}

void EditorShell::Frame::apply_drops() {
  const auto replacement_at = [&](float x, float y, doc::NodeId asset_id,
                                  doc::NodeId source_layer)
      -> std::optional<std::pair<SourceReplacementIntent, gui::Rect>> {
    if (switcher.rehearsal_locked || ui.pointer_blocked()) return std::nullopt;
    const assets::Asset *const asset = asset_registry.asset(asset_id);
    const ElementInfo *const source = find_element(elements, source_layer);
    const auto compatible = [&](doc::NodeId node) {
      const ElementInfo *const target = find_element(elements, node);
      return target != nullptr &&
          (asset != nullptr ? can_replace_source(*target, *asset)
                            : source != nullptr && can_replace_layer(*target, *source));
    };
    if (asset != nullptr && timeline_panel_drawn) {
      for (const auto &target : impl.timeline_panel.replacement_targets()) {
        if (target.bounds.contains(x, y) && compatible(target.node)) {
          return std::pair{SourceReplacementIntent{target.node, asset_id, {},
                                                   authored_stage.id}, target.bounds};
        }
      }
    }
    if (compositor_panel_drawn && !compositor_locked) {
      if (const auto hit = impl.compositor_panel.node_at(compositor_graph, x, y)) {
        const auto node = std::find_if(compositor_graph.nodes.begin(), compositor_graph.nodes.end(),
            [&](const compositor::GraphNode &entry) { return entry.node == hit->first; });
        if (node != compositor_graph.nodes.end() && node->kind == compositor::NodeKind::SceneLayer) {
          const auto *parameters = std::get_if<compositor::SceneLayerParameters>(&node->parameters);
          if (parameters != nullptr && compatible(parameters->scene_node)) {
            return std::pair{SourceReplacementIntent{
                asset != nullptr ? parameters->scene_node : node->node,
                asset_id, source_layer, authored_stage.id}, hit->second};
          }
        }
      }
    }
    return std::nullopt;
  };
  // A drag from another panel can land on a Compositor input: an effect on
  // an effect input, a Timeline layer or a Library asset on an image input.
  // The input is resolved before the node around it, so landing on a port
  // connects rather than replaces.
  const auto port_drop_at = [&](float x, float y,
                                compositor::PortValueType type)
      -> std::optional<CompositorPortDrop> {
    if (!compositor_panel_drawn || compositor_locked ||
        switcher.rehearsal_locked) {
      return std::nullopt;
    }
    return impl.compositor_panel.port_drop_target(compositor_graph, x, y,
                                                  type);
  };
  // An effect let go on an effect wire, away from any node, joins that
  // wire's input right after the wire's own effect.
  const auto wire_drop_at = [&](float x, float y,
                                compositor::PortValueType type)
      -> std::optional<CompositorWireDrop> {
    if (!compositor_panel_drawn || compositor_locked ||
        switcher.rehearsal_locked ||
        impl.compositor_panel.node_at(compositor_graph, x, y).has_value()) {
      return std::nullopt;
    }
    return impl.compositor_panel.effect_wire_at(compositor_graph, x, y, type);
  };
  // What a dragged effect's node puts out: an effect or a filter.
  const auto effect_type = [](compositor::NodeKind kind) {
    return compositor::output_ports(kind).front().type;
  };
  // Where dragged Library assets land on Stage: side by side a step apart,
  // so none hides another, each resting on the surface under its own line of
  // sight. Empty off the Stage image. The drop and its preview share this, so
  // what the outline shows is what the release authors.
  const auto stage_placements_at =
      [&](std::span<const doc::NodeId> ids, float screen_x, float screen_y)
      -> std::vector<std::pair<doc::NodeId, StagePlacement>> {
    std::vector<std::pair<doc::NodeId, StagePlacement>> placed;
    if (!stage_viewport_drawn || switcher.rehearsal_locked) {
      return placed;
    }
    const scene::Canvas canvas = stage_interaction_scene.canvas;
    const CanvasPoint dropped =
        to_canvas(stage_viewport_frame.image, canvas, screen_x, screen_y);
    if (!dropped.inside) {
      return placed;
    }
    const float step = static_cast<float>(canvas.width) * 0.06F;
    const bool snap = impl.stage().panel.snapping() != ui.ctrl_held();
    for (std::size_t index = 0U; index < ids.size(); ++index) {
      const assets::Asset *const asset = asset_registry.asset(ids[index]);
      if (asset == nullptr ||
          std::holds_alternative<assets::PlatesetAsset>(*asset)) {
        continue;
      }
      const float x = dropped.x + step * static_cast<float>(index);
      const auto ray = scene::canvas_ray(stage_interaction_scene.camera,
                                         canvas, x, dropped.y);
      if (!ray.has_value()) {
        continue;
      }
      StagePlacementRequest request{};
      request.ray = *ray;
      request.hit =
          impl.stage().panel.surface_at(stage_interaction_scene, x, dropped.y);
      request.camera = stage_interaction_scene.camera;
      request.snap = snap;
      request.snap_move = impl.stage().panel.snap_steps().move;
      request.snap_turns = impl.stage().panel.snap_steps().rotate_turns;
      if (const auto *const media = std::get_if<assets::MediaAsset>(asset)) {
        request.media = true;
        float aspect = 16.0F / 9.0F;
        if (media->dimensions.has_value() && media->dimensions->height > 0U) {
          aspect = static_cast<float>(media->dimensions->width) /
                   static_cast<float>(media->dimensions->height);
        }
        request.bounds = media_placement_bounds(aspect);
      } else if (const auto *const model =
                     std::get_if<assets::ModelAsset>(asset)) {
        if (const auto bounds =
                impl.stage().panel.model_source_bounds(model->source)) {
          request.bounds = *bounds;
        }
      }
      // A particle effect has no extent before it runs: it lands at the
      // point under the pointer.
      if (const auto placement = place_on_stage(request)) {
        placed.emplace_back(ids[index], *placement);
      }
    }
    return placed;
  };

  // Where a dragged surface generator lands on Stage, by the same rule as a
  // dragged asset: a Background hangs like a screen, the shapes rest on what
  // is under the pointer at their default size.
  const auto generator_placement_at =
      [&](LibraryAddAction action, float screen_x,
          float screen_y) -> std::optional<StagePlacement> {
    if (!stage_viewport_drawn || switcher.rehearsal_locked ||
        !library_generator_placeable(action)) {
      return std::nullopt;
    }
    const scene::Canvas canvas = stage_interaction_scene.canvas;
    const CanvasPoint dropped =
        to_canvas(stage_viewport_frame.image, canvas, screen_x, screen_y);
    if (!dropped.inside) {
      return std::nullopt;
    }
    const auto ray = scene::canvas_ray(stage_interaction_scene.camera, canvas,
                                       dropped.x, dropped.y);
    if (!ray.has_value()) {
      return std::nullopt;
    }
    StagePlacementRequest request{};
    request.ray = *ray;
    request.hit = impl.stage().panel.surface_at(stage_interaction_scene,
                                                dropped.x, dropped.y);
    request.camera = stage_interaction_scene.camera;
    request.snap = impl.stage().panel.snapping() != ui.ctrl_held();
    request.snap_move = impl.stage().panel.snap_steps().move;
    request.snap_turns = impl.stage().panel.snap_steps().rotate_turns;
    if (action == LibraryAddAction::Background) {
      request.media = true;
      request.bounds = media_placement_bounds(16.0F / 9.0F);
    } else if (action == LibraryAddAction::Plane) {
      request.bounds = scene::ModelLocalBounds{
          scene::Vec3{-0.5F, 0.0F, -0.5F}, scene::Vec3{0.5F, 0.0F, 0.5F}};
    } else {
      request.bounds = scene::ModelLocalBounds{
          scene::Vec3{-0.5F, -0.5F, -0.5F}, scene::Vec3{0.5F, 0.5F, 0.5F}};
    }
    return place_on_stage(request);
  };
  const auto placement_caption = [&](const StagePlacement &lead) {
    switch (lead.surface) {
    case StagePlacementSurface::Element:
      // The name Scene shows, or its kind and number when it has none.
      return "On " + viewport_deep_select_label(stage_interaction_scene,
                                                lead.element, elements);
    case StagePlacementSurface::Floor:
      return std::string{"On floor"};
    case StagePlacementSurface::ViewPlane:
      return std::string{"In the air"};
    }
    return std::string{};
  };

  std::optional<CompositorPortDrop> port_hover;
  std::optional<CompositorWireDrop> wire_hover;
  std::string port_hover_what;
  if (!ui.pointer_blocked()) {
    const std::optional<scene::EffectKind> dragged_effect =
        impl.effects_panel.dragging_effect();
    if (dragged_effect.has_value() &&
        graph_effect_node(*dragged_effect).has_value()) {
      const compositor::PortValueType carried =
          effect_type(*graph_effect_node(*dragged_effect));
      port_hover = port_drop_at(ui.pointer_x(), ui.pointer_y(), carried);
      if (!port_hover.has_value()) {
        wire_hover = wire_drop_at(ui.pointer_x(), ui.pointer_y(), carried);
      }
      port_hover_what = impl.effects_panel.dragging_name();
    } else if (impl.timeline_panel.dragging_compositor_source() ||
               dragging_asset.has_value()) {
      port_hover = port_drop_at(ui.pointer_x(), ui.pointer_y(),
                                compositor::PortValueType::Image);
      const std::size_t count =
          impl.timeline_panel.dragging_compositor_source_count();
      if (dragging_asset.has_value()) {
        const assets::Asset *const asset = asset_registry.asset(*dragging_asset);
        port_hover_what = asset != nullptr
                              ? std::string{assets::asset_display_name(*asset)}
                              : std::string{"Asset"};
      } else if (count > 1U) {
        port_hover_what = std::to_string(count) + " layers";
      } else {
        const ElementInfo *const dragged = find_element(
            elements, impl.timeline_panel.dragging_compositor_source_node());
        port_hover_what = dragged != nullptr ? dragged->display_name
                                             : std::string{"Layer"};
      }
    }
  }
  const doc::NodeId lifted_layer =
      impl.timeline_panel.dragging_compositor_source_count() == 1U
          ? impl.timeline_panel.dragging_compositor_source_node() : doc::NodeId{};
  const auto replacement_hover =
      port_hover.has_value()
          ? std::nullopt
          : replacement_at(ui.pointer_x(), ui.pointer_y(),
                           dragging_asset.value_or(doc::NodeId{}), lifted_layer);
  if (port_hover.has_value()) {
    impl.compositor_panel.draw_port_drop(ui, *port_hover, port_hover_what);
  } else if (wire_hover.has_value()) {
    impl.compositor_panel.draw_wire_drop(ui, *wire_hover, port_hover_what);
  }
  if (replacement_hover.has_value()) {
    const auto &bounds = replacement_hover->second;
    ui.panel(bounds, with_alpha(ui.theme().accent, 0.18F));
    ui.outline(bounds, ui.theme().accent);
    const std::string_view label = dragging_asset.has_value() ? "Replace source" : "Replace layer";
    const float label_width = ui.font_width(label) + 16.0F;
    const gui::Rect badge{bounds.right() - label_width, bounds.y,
                           label_width, std::min(bounds.height, 24.0F)};
    ui.panel(badge, ui.theme().accent);
    ui.label({badge.x + 8.0F, badge.y, badge.width - 8.0F, badge.height},
             label, ui.theme().background);
  }
  if (!dragging_assets.empty() && !port_hover.has_value() &&
      !wire_hover.has_value() && !replacement_hover.has_value() &&
      !ui.pointer_blocked() &&
      !(timeline_panel_drawn &&
        timeline_panel_bounds.contains(ui.pointer_x(), ui.pointer_y()))) {
    const auto placed =
        stage_placements_at(dragging_assets, ui.pointer_x(), ui.pointer_y());
    if (!placed.empty()) {
      std::vector<StagePlacement> placements;
      placements.reserve(placed.size());
      for (const auto &[unused, placement] : placed) {
        static_cast<void>(unused);
        placements.push_back(placement);
      }
      impl.stage().panel.draw_placement_preview(
          ui, stage_viewport_frame.image, stage_interaction_scene, placements,
          placement_caption(placements.front()));
    }
  }
  if (dragging_generator.has_value() && !ui.pointer_blocked() &&
      !(timeline_panel_drawn &&
        timeline_panel_bounds.contains(ui.pointer_x(), ui.pointer_y()))) {
    if (const auto placement = generator_placement_at(
            *dragging_generator, ui.pointer_x(), ui.pointer_y())) {
      const std::array<StagePlacement, 1U> placements{*placement};
      impl.stage().panel.draw_placement_preview(
          ui, stage_viewport_frame.image, stage_interaction_scene, placements,
          placement_caption(*placement));
    }
  }
  if (compositor_panel_drawn && !compositor_locked && !port_hover.has_value() &&
      !wire_hover.has_value()) {
    if (impl.effects_panel.dragging()) {
      draw_compositor_drop_target(ui, compositor_panel_bounds,
                                  impl.effects_panel.dragging_name());
    } else if (impl.timeline_panel.dragging_compositor_source() &&
               !impl.compositor_panel.node_at(compositor_graph, ui.pointer_x(), ui.pointer_y()).has_value()) {
      const ElementInfo *const dragged = find_element(
          elements, impl.timeline_panel.dragging_compositor_source_node());
      std::array<char, 64U> dragged_title{};
      const std::size_t count =
          impl.timeline_panel.dragging_compositor_source_count();
      const std::string group_title = std::to_string(count) + " layers";
      draw_compositor_drop_target(
          ui, compositor_panel_bounds,
          count > 1U ? std::string_view{group_title}
                     : effect_target_name(dragged, dragged_title));
    }
  }

  if (pending_effect_drop.has_value()) {
    const std::optional<compositor::NodeKind> graph_effect_kind =
        graph_effect_node(pending_effect_drop->effect);
    const gui::UiPoint graph_effect_node_size =
        impl.compositor_panel.new_node_size(
            graph_effect_kind.value_or(compositor::NodeKind::Brightness));
    const std::optional<gui::UiPoint> graph_drop_position =
        compositor_panel_drawn
            ? impl.compositor_panel.node_drop_position(
                  pending_effect_drop->screen_x,
                  pending_effect_drop->screen_y, graph_effect_node_size.x,
                  graph_effect_node_size.y)
            : std::optional<gui::UiPoint>{};
    const std::optional<CompositorPortDrop> effect_port =
        graph_effect_kind.has_value()
            ? port_drop_at(pending_effect_drop->screen_x,
                           pending_effect_drop->screen_y,
                           effect_type(*graph_effect_kind))
            : std::nullopt;
    const std::optional<CompositorWireDrop> effect_wire =
        graph_effect_kind.has_value() && !effect_port.has_value()
            ? wire_drop_at(pending_effect_drop->screen_x,
                           pending_effect_drop->screen_y,
                           effect_type(*graph_effect_kind))
            : std::nullopt;
    const bool graph_drop = graph_drop_position.has_value();
    const gui::Rect *const stage_drop_bounds =
        stage_viewport_drawn ? &stage_viewport_frame.image : nullptr;
    const gui::Rect *const inspector_drop_bounds =
        inspector_panel_drawn ? &inspector_panel_bounds : nullptr;
    if (effect_port.has_value() && result.compositor_adds.empty()) {
      const gui::UiPoint at = impl.compositor_panel.node_position_feeding(
          *graph_effect_kind, *effect_port);
      CompositorAddIntent add{*graph_effect_kind, at.x, at.y};
      add.links.push_back(CompositorAddLink{
          true, compositor::output_ports(*graph_effect_kind).front().index,
          effect_port->endpoint});
      if (effect_port->occupied.has_value()) {
        add.remove_edges.push_back(*effect_port->occupied);
      }
      result.compositor_adds.push_back(std::move(add));
      result.compositor_gesture = impl.next_edit_gesture++;
    } else if (effect_wire.has_value() && graph_drop &&
               result.compositor_adds.empty()) {
      CompositorAddIntent add{*graph_effect_kind, graph_drop_position->x,
                              graph_drop_position->y};
      add.links.push_back(CompositorAddLink{
          true, compositor::output_ports(*graph_effect_kind).front().index,
          effect_wire->input, effect_wire->order});
      add.reorder = effect_wire->reorder;
      result.compositor_adds.push_back(std::move(add));
      result.compositor_gesture = impl.next_edit_gesture++;
    } else if (graph_drop && graph_effect_kind.has_value() &&
               !compositor_locked && result.compositor_adds.empty()) {
      result.compositor_adds.push_back(CompositorAddIntent{
          *graph_effect_kind,
          graph_drop_position->x,
          graph_drop_position->y});
      result.compositor_gesture = impl.next_edit_gesture++;
    } else if (!graph_drop && effect_drop_targets_selected_layer(
                                  *pending_effect_drop, stage_drop_bounds,
                                  inspector_drop_bounds)) {
      result.effects.push_back(EffectIntent{pending_effect_drop->target,
                                            pending_effect_drop->effect,
                                            impl.next_edit_gesture++});
    }
  }

  if (pending_timeline_layer_drop.has_value() && compositor_panel_drawn &&
      !compositor_locked && result.compositor_adds.empty()) {
    const TimelineLayerDrop &drop = *pending_timeline_layer_drop;
    const gui::UiPoint graph_layer_node_size =
        impl.compositor_panel.new_node_size(compositor::NodeKind::SceneLayer);
    const float graph_layer_node_height = graph_layer_node_size.y;
    const std::optional<gui::UiPoint> graph_drop_position =
        impl.compositor_panel.node_drop_position(
            drop.screen_x, drop.screen_y, graph_layer_node_size.x,
            graph_layer_node_height);
    const std::optional<CompositorPortDrop> layer_port = port_drop_at(
        drop.screen_x, drop.screen_y, compositor::PortValueType::Image);
    const auto replacement =
        drop.layers.size() == 1U && !layer_port.has_value()
            ? replacement_at(drop.screen_x, drop.screen_y, {},
                             drop.layers.front())
            : std::nullopt;
    if (layer_port.has_value()) {
      // Each layer feeds the input: through the Scene Layer node it already
      // has, or a new one placed to its left. An input that takes one wire
      // takes the first layer only, replacing what it had.
      CompositorRewireIntent rewire;
      if (layer_port->occupied.has_value()) {
        rewire.remove_edges.push_back(*layer_port->occupied);
      }
      std::size_t placed{};
      for (const doc::NodeId layer : drop.layers) {
        const ElementInfo *const dropped = find_element(elements, layer);
        if (dropped == nullptr || dropped->locked) {
          continue;
        }
        const auto existing = std::find_if(
            compositor_graph.nodes.begin(), compositor_graph.nodes.end(),
            [layer](const compositor::GraphNode &node) {
              const auto *parameters =
                  std::get_if<compositor::SceneLayerParameters>(
                      &node.parameters);
              return node.kind == compositor::NodeKind::SceneLayer &&
                     parameters != nullptr && parameters->scene_node == layer;
            });
        if (existing != compositor_graph.nodes.end()) {
          rewire.add_edges.push_back(CompositorWire{
              compositor::EdgeEndpoint{existing->node, 0U},
              layer_port->endpoint});
        } else {
          gui::UiPoint at = impl.compositor_panel.node_position_feeding(
              compositor::NodeKind::SceneLayer, *layer_port);
          at.y += static_cast<float>(placed++) * (graph_layer_node_height + 16.0F);
          CompositorAddIntent add{compositor::NodeKind::SceneLayer, at.x, at.y,
                                  layer};
          add.links.push_back(
              CompositorAddLink{true, 0U, layer_port->endpoint});
          result.compositor_adds.push_back(std::move(add));
        }
        if (!layer_port->multiple) {
          break;
        }
      }
      if (!rewire.add_edges.empty()) {
        result.compositor_rewire = std::move(rewire);
      } else if (!result.compositor_adds.empty() && !rewire.remove_edges.empty()) {
        result.compositor_adds.front().remove_edges = rewire.remove_edges;
      }
      if (!result.compositor_adds.empty() ||
          result.compositor_rewire.has_value()) {
        result.compositor_gesture = impl.next_edit_gesture++;
      }
    } else if (replacement.has_value()) {
      auto intent = replacement->first;
      intent.gesture = impl.next_edit_gesture++;
      result.source_replacements.push_back(intent);
      impl.history_context = HistoryContext::Compositor;
      impl.semantic_boundary = true;
    } else if (graph_drop_position.has_value() &&
               !impl.compositor_panel.node_at(compositor_graph, drop.screen_x, drop.screen_y).has_value()) {
      const auto overlaps = [&](doc::NodeId layer, doc::NodeId existing_node) {
        if (existing_node == layer) {
          return true;
        }
        const ElementInfo *const dropped = find_element(elements, layer);
        const ElementInfo *const existing = find_element(elements, existing_node);
        return (dropped != nullptr && dropped->kind == ElementKind::Track &&
                existing != nullptr && existing->timeline_lane == layer) ||
               (existing != nullptr && existing->kind == ElementKind::Track &&
                dropped != nullptr && dropped->timeline_lane == existing_node);
      };
      for (const doc::NodeId layer : drop.layers) {
        const ElementInfo *const dropped = find_element(elements, layer);
        if (dropped == nullptr || dropped->locked) {
          continue;
        }
        const bool already_present = std::any_of(
            compositor_graph.nodes.begin(), compositor_graph.nodes.end(),
            [&](const compositor::GraphNode &node) {
              const auto *parameters =
                  std::get_if<compositor::SceneLayerParameters>(&node.parameters);
              return node.kind == compositor::NodeKind::SceneLayer &&
                     parameters != nullptr &&
                     overlaps(layer, parameters->scene_node);
            }) || std::any_of(
                result.compositor_adds.begin(), result.compositor_adds.end(),
                [&](const CompositorAddIntent &added) {
                  return overlaps(layer, added.scene_node);
                });
        if (!already_present) {
          // Keep the first node under the pointer and stack the remaining
          // nodes in graph space, independent of the current zoom and pan.
          const float offset = static_cast<float>(result.compositor_adds.size()) *
                               (graph_layer_node_height + 16.0F);
          result.compositor_adds.push_back(CompositorAddIntent{
              compositor::NodeKind::SceneLayer, graph_drop_position->x,
              graph_drop_position->y + offset, layer});
        }
      }
      if (!result.compositor_adds.empty()) {
        result.compositor_gesture = impl.next_edit_gesture++;
      }
    }
  }

  // The Asset Viewer's picture let go. On a track's band it is laid over that
  // track from the frame under the pointer; elsewhere on the Timeline it is a
  // new layer starting there, and on the Stage a new placement starting at
  // the playhead. Either way it carries the marked range.
  if (pending_viewer_drop.has_value() && !switcher.rehearsal_locked) {
    const AssetViewerDrop drop = *pending_viewer_drop;
    const assets::Asset *const found = asset_registry.asset(drop.asset);
    const auto *const media =
        found != nullptr ? std::get_if<assets::MediaAsset>(found) : nullptr;
    const scene::MediaFrameRate show_rate{timeline.rate_numerator,
                                          timeline.rate_denominator};
    std::optional<scene::ClipTiming> timing;
    if (media != nullptr && asset_viewer_frame_count(*media) > 1U) {
      std::optional<scene::MediaFrameRate> source_rate;
      if (media->frame_rate.has_value()) {
        source_rate = scene::MediaFrameRate{media->frame_rate->numerator,
                                            media->frame_rate->denominator};
      }
      if (const auto span = scene::source_span_show_frames(
              drop.source_frames, show_rate, source_rate)) {
        scene::ClipTiming placed{};
        placed.start = static_cast<core::i64>(timeline.show_frame);
        placed.duration = span.value();
        placed.source_in = static_cast<core::i64>(drop.source_in);
        timing = placed;
      }
    }
    if (media != nullptr && timeline_panel_drawn &&
        timeline_panel_bounds.contains(drop.screen_x, drop.screen_y)) {
      const auto point =
          impl.timeline_panel.lane_point_at(drop.screen_x, drop.screen_y);
      // A file is laid over a track's own screen. A vehicle track has no
      // screen of its own and its slot screens show its takes, so on either
      // the drop does nothing rather than file a clip where none can play.
      const bool on_a_car =
          point.has_value() &&
          std::any_of(timeline_lanes.begin(), timeline_lanes.end(),
                      [&](const scene::TimelineLane &lane) {
                        return lane.id == point->lane &&
                               (lane.vehicle || lane.parent != doc::NodeId{});
                      });
      if (on_a_car) {
        // Nothing to place.
      } else if (point.has_value()) {
        result.source_edits.push_back(SourceRangeEditIntent{
            authored_stage.id, drop.asset, drop.source_in, drop.source_frames,
            SourceEditMode::Overwrite, point->lane,
            static_cast<core::u64>(std::max<core::i64>(point->frame, 0)),
            show_rate, impl.next_edit_gesture++});
      } else {
        if (timing.has_value()) {
          if (const auto frame =
                  impl.timeline_panel.frame_at(drop.screen_x, drop.screen_y)) {
            timing->start = *frame;
          }
        }
        instantiate_asset(drop.asset, std::nullopt, std::nullopt, std::nullopt,
                          timing);
      }
      impl.history_context = HistoryContext::Scene;
      impl.semantic_boundary = true;
    } else if (media != nullptr) {
      const std::array<doc::NodeId, 1U> dropped{drop.asset};
      for (const auto &[asset, placement] :
           stage_placements_at(dropped, drop.screen_x, drop.screen_y)) {
        instantiate_asset(asset, placement.position, std::nullopt,
                          placement.rotation == scene::Vec3{}
                              ? std::nullopt
                              : std::optional<scene::Vec3>{placement.rotation},
                          timing);
        impl.history_context = HistoryContext::Scene;
        impl.semantic_boundary = true;
      }
    }
  }

  // A plateset let go on the Timeline or Stage is placed: on a take it
  // plays instead of it; on a vehicle track's band it is a new take there
  // from the frame under the pointer; anywhere else on the Timeline it
  // starts where it lands, and on Stage at the playhead. Only a scene with
  // no vehicle track yet, whose rig already plays something on the sockets
  // the plates need, has anything to ask the operator first.
  if (pending_asset_drop.has_value() && !switcher.rehearsal_locked) {
    const auto &drop = *pending_asset_drop;
    const assets::Asset *const dropped = asset_registry.asset(drop.asset);
    const auto *const plateset =
        dropped != nullptr ? std::get_if<assets::PlatesetAsset>(dropped)
                           : nullptr;
    const bool on_timeline =
        timeline_panel_drawn &&
        timeline_panel_bounds.contains(drop.screen_x, drop.screen_y);
    const bool on_rig = plateset_editor_drawn &&
                        impl.asset_viewer_panel.plateset_editor().rig_contains(
                            drop.screen_x, drop.screen_y);
    if (plateset != nullptr && !on_rig) {
      // Let go on a take - its own row, or its bar on its vehicle track -
      // the plateset plays instead of it. Let go on a vehicle track where no
      // take is, it is a new take there.
      std::optional<doc::NodeId> take_clip;
      doc::NodeId track{};
      if (on_timeline) {
        for (const auto &target : impl.timeline_panel.replacement_targets()) {
          const ElementInfo *const clip = find_element(elements, target.node);
          if (target.bounds.contains(drop.screen_x, drop.screen_y) &&
              clip != nullptr && clip->take) {
            take_clip = target.node;
            break;
          }
        }
        const auto point =
            impl.timeline_panel.lane_point_at(drop.screen_x, drop.screen_y);
        const auto under =
            point.has_value()
                ? std::find_if(timeline_lanes.begin(), timeline_lanes.end(),
                               [&](const scene::TimelineLane &lane) {
                                 return lane.id == point->lane;
                               })
                : timeline_lanes.end();
        if (under != timeline_lanes.end() && under->vehicle) {
          track = under->id;
          // The take in front at that frame, as the scene would show it.
          const ElementInfo *front = nullptr;
          for (const ElementInfo &clip : elements) {
            if (clip.kind == ElementKind::Media && clip.take &&
                clip.timeline_lane == track && clip.timing.placed() &&
                point->frame >= clip.timing.start &&
                point->frame <= clip.timing.last_frame() &&
                (front == nullptr || clip.clip_order >= front->clip_order)) {
              front = std::addressof(clip);
            }
          }
          if (!take_clip.has_value() && front != nullptr) {
            take_clip = front->node;
          }
        }
      }
      if (take_clip.has_value()) {
        result.plateset_swaps.push_back(PlatesetSwapIntent{
            authored_stage.id, *take_clip, plateset->id,
            impl.next_edit_gesture++});
      } else {
        core::i64 start = static_cast<core::i64>(timeline.show_frame);
        if (on_timeline) {
          if (const auto at = impl.timeline_panel.frame_at(drop.screen_x,
                                                           drop.screen_y)) {
            start = std::max<core::i64>(*at, 0);
          }
        }
        // Busy: the scene's first rig already has a wire on a socket one of
        // these plates plays in.
        const compositor::GraphNode *rig = nullptr;
        for (const compositor::GraphNode &node : compositor_graph.nodes) {
          if (node.kind == compositor::NodeKind::LedCarProcess &&
              (rig == nullptr || node.node < rig->node)) {
            rig = std::addressof(node);
          }
        }
        // With a vehicle track in the scene the take goes on it - the one
        // it was let go on, else the scene's own - and nothing is rewired.
        const bool has_vehicle_track = std::any_of(
            timeline_lanes.begin(), timeline_lanes.end(),
            [](const scene::TimelineLane &lane) { return lane.vehicle; });
        bool busy = false;
        if (rig != nullptr && !has_vehicle_track) {
          for (const compositor::GraphEdge &edge : compositor_graph.edges) {
            if (edge.to.node == rig->node &&
                edge.to.port < assets::plateset_slot_count &&
                plateset->slots[edge.to.port].media != doc::NodeId{}) {
              busy = true;
            }
          }
        }
        PlatesetPlaceIntent place{authored_stage.id, plateset->id, start,
                                  PlatesetRigChoice::Auto, 0U, track};
        if (busy) {
          impl.pending_plateset_place = place;
          ui.open_context_menu(gui::ui_id("plateset.rig.choice"),
                               drop.screen_x, drop.screen_y);
        } else {
          place.gesture = impl.next_edit_gesture++;
          result.plateset_places.push_back(place);
        }
      }
      impl.history_context = HistoryContext::Scene;
      impl.semantic_boundary = true;
      pending_asset_drop.reset();
    }
  }
  if (impl.pending_plateset_place.has_value()) {
    constexpr std::array<std::string_view, 3U> choices{
        "Replace the plates on the LED Car Process",
        "Add a second LED Car Process",
        "Keep its wires; fill the free sockets"};
    const gui::UiId menu = gui::ui_id("plateset.rig.choice");
    if (const auto chosen = ui.context_menu(menu, choices)) {
      PlatesetPlaceIntent place = *impl.pending_plateset_place;
      place.rig = *chosen == 0U   ? PlatesetRigChoice::Replace
                  : *chosen == 1U ? PlatesetRigChoice::NewRig
                                  : PlatesetRigChoice::Auto;
      place.gesture = impl.next_edit_gesture++;
      result.plateset_places.push_back(place);
      impl.pending_plateset_place.reset();
    } else if (!ui.menu_open(menu)) {
      impl.pending_plateset_place.reset();
    }
  }

  // Plates let go on an open plateset's rig: one on a slot plays there;
  // several anywhere on the rig fill the empty slots their names propose.
  if (pending_asset_drop.has_value() && plateset_editor_drawn) {
    const auto &drop = *pending_asset_drop;
    const PlatesetEditor &rig = impl.asset_viewer_panel.plateset_editor();
    const assets::Asset *const open =
        asset_registry.asset(impl.asset_viewer_panel.asset());
    const auto *const plateset =
        open != nullptr ? std::get_if<assets::PlatesetAsset>(open) : nullptr;
    if (plateset != nullptr && rig.rig_contains(drop.screen_x, drop.screen_y)) {
      const std::vector<doc::NodeId> dropped =
          drop.assets.empty() ? std::vector<doc::NodeId>{drop.asset}
                              : drop.assets;
      std::vector<const assets::MediaAsset *> plates;
      for (const doc::NodeId id : dropped) {
        const assets::Asset *const asset = asset_registry.asset(id);
        if (const auto *const media =
                asset != nullptr ? std::get_if<assets::MediaAsset>(asset)
                                 : nullptr) {
          plates.push_back(media);
        }
      }
      const core::u64 gesture = impl.next_edit_gesture++;
      const auto assign = [&](assets::PlatesetSlot slot, doc::NodeId media) {
        assets::LibraryEdit edit{};
        edit.kind = assets::LibraryEdit::Kind::SetPlatesetSlot;
        edit.target = plateset->id;
        edit.slot = slot;
        edit.media = media;
        result.asset_library_edits.push_back(
            AssetLibraryEditIntent{std::move(edit), gesture});
      };
      const auto slot = rig.slot_at(drop.screen_x, drop.screen_y);
      if (plates.size() == 1U && slot.has_value()) {
        assign(*slot, plates.front()->id);
      } else if (!plates.empty()) {
        const assets::PlatesetProposal proposal =
            assets::propose_plateset(plates);
        std::vector<doc::NodeId> loose = proposal.unplaced;
        for (const assets::PlatesetSlot candidate :
             assets::all_plateset_slots) {
          const std::size_t index = static_cast<std::size_t>(candidate);
          if (plateset->slots[index].media != doc::NodeId{}) {
            continue;
          }
          if (proposal.slots[index] != doc::NodeId{}) {
            assign(candidate, proposal.slots[index]);
          }
        }
        // What the names did not place lands where it was dropped.
        if (slot.has_value() && !loose.empty()) {
          assign(*slot, loose.front());
        }
      }
      impl.history_context = HistoryContext::Scene;
      impl.semantic_boundary = true;
      pending_asset_drop.reset();
    }
  }
  if (pending_asset_drop.has_value()) {
    const auto &drop = *pending_asset_drop;
    if (const auto port = port_drop_at(drop.screen_x, drop.screen_y,
                                       compositor::PortValueType::Image)) {
      const gui::UiPoint at = impl.compositor_panel.node_position_feeding(
          compositor::NodeKind::SceneLayer, *port);
      CompositorAddIntent layer{compositor::NodeKind::SceneLayer, at.x, at.y};
      layer.links.push_back(CompositorAddLink{true, 0U, port->endpoint});
      if (port->occupied.has_value()) {
        layer.remove_edges.push_back(*port->occupied);
      }
      instantiate_asset(drop.asset, std::nullopt, std::move(layer));
      impl.history_context = HistoryContext::Compositor;
      impl.semantic_boundary = true;
      pending_asset_drop.reset();
    }
  }
  if (pending_asset_drop.has_value()) {
    const auto &drop = *pending_asset_drop;
    if (const auto replacement = replacement_at(drop.screen_x, drop.screen_y, drop.asset, {})) {
      auto intent = replacement->first;
      intent.gesture = impl.next_edit_gesture++;
      result.source_replacements.push_back(intent);
      impl.history_context = HistoryContext::Scene;
      impl.semantic_boundary = true;
      pending_asset_drop.reset();
    }
  }

  // Let go anywhere else on the Timeline, assets become new layers, as a
  // clip dragged into an editor's timeline does.
  if (pending_asset_drop.has_value() && timeline_panel_drawn &&
      !switcher.rehearsal_locked &&
      timeline_panel_bounds.contains(pending_asset_drop->screen_x,
                                     pending_asset_drop->screen_y)) {
    const std::vector<doc::NodeId> dropped_assets =
        pending_asset_drop->assets.empty()
            ? std::vector<doc::NodeId>{pending_asset_drop->asset}
            : pending_asset_drop->assets;
    for (const doc::NodeId asset : dropped_assets) {
      instantiate_asset(asset, std::nullopt);
    }
    impl.history_context = HistoryContext::Scene;
    impl.semantic_boundary = true;
    pending_asset_drop.reset();
  }

  if (pending_asset_drop.has_value()) {
    const std::vector<doc::NodeId> &dropped_assets =
        pending_asset_drop->assets.empty()
            ? std::vector<doc::NodeId>{pending_asset_drop->asset}
            : pending_asset_drop->assets;
    for (const auto &[asset, placement] :
         stage_placements_at(dropped_assets, pending_asset_drop->screen_x,
                             pending_asset_drop->screen_y)) {
      instantiate_asset(asset, placement.position, std::nullopt,
                        placement.rotation == scene::Vec3{}
                            ? std::nullopt
                            : std::optional<scene::Vec3>{placement.rotation});
    }
  }

  if (pending_generator_drop.has_value() &&
      !(timeline_panel_drawn &&
        timeline_panel_bounds.contains(pending_generator_drop->screen_x,
                                       pending_generator_drop->screen_y))) {
    const auto placement = generator_placement_at(
        pending_generator_drop->action, pending_generator_drop->screen_x,
        pending_generator_drop->screen_y);
    if (placement.has_value()) {
      AddKind kind = AddKind::Box;
      switch (pending_generator_drop->action) {
      case LibraryAddAction::Plane:
        kind = AddKind::Plane;
        break;
      case LibraryAddAction::Sphere:
        kind = AddKind::Sphere;
        break;
      case LibraryAddAction::Background:
        kind = AddKind::Background;
        break;
      default:
        break;
      }
      AddIntent add{kind,
                    std::string{},
                    1U,
                    impl.next_edit_gesture++,
                    16.0F / 9.0F,
                    {},
                    authored_stage.id};
      add.position = placement->position;
      if (placement->rotation != scene::Vec3{}) {
        add.rotation = placement->rotation;
      }
      result.adds.push_back(std::move(add));
      impl.history_context = HistoryContext::Scene;
      impl.semantic_boundary = true;
    }
  }

  // The plateset page's thumbnails ride with the Library's: kept while it
  // shows them, and asked for when they are not made yet.
  if (!plateset_thumbnail_keys.empty()) {
    if (!result.thumbnail_retain.has_value()) {
      result.thumbnail_retain = LibraryThumbnailRetainIntent{};
    }
    for (const assets::DerivativeKey &key : plateset_thumbnail_keys) {
      auto &kept = result.thumbnail_retain->keys;
      if (std::find(kept.begin(), kept.end(), key) == kept.end()) {
        kept.push_back(key);
      }
      const auto observed = std::find_if(
          thumbnails.begin(), thumbnails.end(),
          [&key](const LibraryThumbnailObservation &entry) {
            return entry.key == key;
          });
      const bool wanted =
          observed == thumbnails.end() ||
          observed->state == LibraryThumbnailState::Placeholder ||
          observed->state == LibraryThumbnailState::Stale;
      const bool asked = std::any_of(
          result.thumbnail_requests.begin(), result.thumbnail_requests.end(),
          [&key](const LibraryThumbnailRequestIntent &request) {
            return request.key == key;
          });
      if (wanted && !asked) {
        result.thumbnail_requests.push_back(LibraryThumbnailRequestIntent{
            key, LibraryThumbnailRequestPriority::Visible});
      }
    }
  }

  // Files from the desktop let go anywhere but the Library still become
  // Library assets, at the top level.
  if (!library_took_files && !ui.dropped_files().empty()) {
    const std::vector<std::string> dropped(ui.dropped_files().begin(),
                                           ui.dropped_files().end());
    import_files(dropped, doc::NodeId{});
  }

  // Global shortcuts stand down while a field owns the keyboard or a pointer
  // gesture is live. The window host has already translated the platform
  // chords into layout-independent history keys.
}

} // namespace mediaserver::editor
