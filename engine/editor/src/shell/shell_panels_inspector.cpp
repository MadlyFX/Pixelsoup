// The Color Correction, Texture and Inspector panels.

#include "shell_frame.hpp"

#include "native_dialogs.hpp"

namespace mediaserver::editor {

void EditorShell::Frame::draw_color_correction_panel(const gui::Rect &bounds) {
  // Treat the dedicated Color surface as an Inspector drop target: an
  // operator can drag Levels here, then edit it without changing panels.
  inspector_panel_bounds = bounds;
  inspector_panel_drawn = true;
  const doc::NodeId anchor = impl.anchor();
  const ColorCorrectionFrame color = impl.color_correction_panel.draw(
      ui, bounds, anchor, effects);
  if (!color.edits.empty()) {
    const core::u64 gesture = color.numeric_nudge.has_value()
        ? begin_semantic_nudge(*color.numeric_nudge,
                               HistoryContext::Scene,
                               scene_selection_token())
        : impl.begin_gesture();
    for (const ColorCorrectionEdit &edit : color.edits) {
      result.generic_edits.push_back(GenericEdit{
          edit.effect, edit.property, edit.value, gesture});
    }
  }
  return;
}

void EditorShell::Frame::draw_texture_panel(const gui::Rect &bounds) {
  // Texture controls author the first enabled Film Grain effect on the
  // primary Scene selection. The panel is also a valid Effects drop
  // target so the complete add/edit workflow does not require a detour.
  inspector_panel_bounds = bounds;
  inspector_panel_drawn = true;
  const doc::NodeId anchor = impl.anchor();
  const TexturePanelFrame texture = impl.texture_panel.draw(
      ui, bounds, anchor, effects);
  if (texture.edit.has_value()) {
    const core::u64 gesture = texture.numeric_nudge.has_value()
        ? begin_semantic_nudge(*texture.numeric_nudge,
                               HistoryContext::Scene,
                               scene_selection_token())
        : impl.begin_gesture();
    result.film_grain_effect_edits.push_back(
        FilmGrainEffectEditIntent{texture.edit->effect,
                                  texture.edit->film_grain, gesture});
  }
  return;
}

void EditorShell::Frame::draw_inspector_panel(const gui::Rect &bounds) {
  // A surface selection replaces the Scene selection wherever it is made,
  // so while one is held it is what Properties shows.
  const output::ProjectionSurface *const inspected_surface =
      impl.stage_surface_selection.has_value() &&
              impl.scene_selection.empty()
          ? feed_map.surface_for(*impl.stage_surface_selection)
          : nullptr;
  if (inspected_surface != nullptr) {
    inspector_panel_bounds = bounds;
    inspector_panel_drawn = true;
    // The feed map is evaluated at the Program playhead, so that is
    // where these keys are read and authored, as route_surface_edit does.
    InspectorFrame inspector = impl.inspector_panel.draw_surface(
        ui, bounds, *inspected_surface, keyframes,
        timeline.program_frame);
    if (switcher.rehearsal_locked) {
      return;
    }
    if (inspector.surface_edit.has_value()) {
      FeedSurfaceEditRequest &edit = *inspector.surface_edit;
      impl.feed_mapping_panel.preview_surface_geometry(edit.node,
                                                       edit.surface);
      edit.gesture = impl.begin_gesture();
      route_surface_edit(std::move(edit));
    }
    if (!inspector.key_edits.empty()) {
      const core::u64 gesture = impl.next_edit_gesture++;
      for (const InspectorKeyEdit &key : inspector.key_edits) {
        result.keys.push_back(KeyIntent{key.node, key.component,
                                        timeline.program_frame, key.value,
                                        false, gesture, key.erase, key.parameter});
      }
    }
    return;
  }
  const auto settings_target = impl.anchor() == project_settings.stage_node && project_settings.stage_node != doc::NodeId{}
      ? SceneSettingsTarget::Environment : impl.scene_selection.empty() ? impl.scene_settings_selection : SceneSettingsTarget::None;
  if (settings_target != SceneSettingsTarget::None) {
    auto state = project_settings;
    state.passes = impl.program_renderer.renderer_passes();
    state.color_status = color_status_for(impl.program_display.color_state());
    state.bake_running = impl.bake_job != nullptr;
    state.bake_status = bake_activity;
    live_scene_settings = impl.scene_settings_panel.draw(ui,bounds,settings_target,state,keyframes,timeline.show_frame,elements,impl.timeline_panel.auto_key_enabled());
    if (live_scene_settings.file_picker) {
      const FilePickerRequest request = *live_scene_settings.file_picker;
      open_file_dialog(
          [request] { return choose_property_file(request); },
          [kind = request.kind](Frame &frame, std::string path) {
            if (kind == FilePickerKind::BakeFolder) {
              frame.impl.scene_settings_panel.set_bake_destination(
                  std::move(path));
              return;
            }
            // Applied to the settings as they are when the operator answers,
            // as one discrete edit.
            const ProjectSettingsState &now = frame.project_settings;
            ProjectSettingsEdit edit{};
            edit.settings = now.settings;
            edit.renderer = now.renderer;
            edit.environment = now.environment;
            edit.lens_profile = now.lens_profile;
            edit.stage_node = now.stage_node;
            edit.node = now.node;
            edit.camera_node = now.camera_node;
            if (kind == FilePickerKind::ShowLut)
              edit.renderer.color.show_lut_source = std::move(path);
            if (kind == FilePickerKind::EnvironmentMap)
              edit.environment.map_source = std::move(path);
            edit.gesture = frame.impl.next_edit_gesture++;
            frame.result.project_settings = std::move(edit);
          });
    }
    if (!live_scene_settings.keys.empty()) {
      const auto gesture = impl.begin_gesture();
      for (const auto& key : live_scene_settings.keys)
        result.keys.push_back(KeyIntent{project_settings.stage_node,key.channel,timeline.show_frame,key.value,false,gesture,key.erase});
    }
    impl.delete_domain = DeleteDomain::Neutral;
    return;
  }

  inspector_panel_bounds = bounds;
  inspector_panel_drawn = true;
  const doc::NodeId anchor = impl.anchor();
  std::vector<ElementInfo> inspector_elements;
  inspector_elements.reserve(impl.scene_selection.size());
  for (const doc::NodeId selected : impl.scene_selection.members()) {
    if (const ElementInfo *const element = find_element(elements, selected);
        element != nullptr) {
      inspector_elements.push_back(*element);
      if (element->kind == ElementKind::Track && element->vehicle) {
        // The car's layout is its LED Car Process's: the one its screens'
        // Scene Layers are wired into, the lowest id where they feed two.
        const compositor::GraphNode *rig = nullptr;
        for (const compositor::GraphEdge &edge : compositor_graph.edges) {
          const compositor::GraphNode *const from =
              compositor_graph.node(edge.from.node);
          const compositor::GraphNode *const to =
              compositor_graph.node(edge.to.node);
          if (from == nullptr || to == nullptr ||
              to->kind != compositor::NodeKind::LedCarProcess ||
              (rig != nullptr && rig->node < to->node)) {
            continue;
          }
          const auto *const layer =
              std::get_if<compositor::SceneLayerParameters>(&from->parameters);
          if (layer != nullptr && layer->scene_node != doc::NodeId{} &&
              std::find(element->slot_screens.begin(),
                        element->slot_screens.end(),
                        layer->scene_node) != element->slot_screens.end()) {
            rig = to;
          }
        }
        if (rig != nullptr) {
          if (const auto *const layout =
                  std::get_if<compositor::LedCarProcessParameters>(
                      &rig->parameters)) {
            inspector_elements.back().vehicle_rig = rig->node;
            inspector_elements.back().rig = *layout;
          }
        }
      }
      if (element->kind == ElementKind::ParticleEffect) {
        // Read on the cache's worker; the section fills in when it lands.
        const render::ParticlePackageObservation observed =
            impl.inspector_particle_cache.observe(element->source);
        if (observed.package != nullptr) {
          for (const ajfx::ParameterDesc &published : observed.package->effect.parameters) {
            ParticleParameterInfo info{};
            info.name = published.name;
            info.components = published.type == ajfx::ParameterType::Float ? 1U
                : published.type == ajfx::ParameterType::Vec3 ? 3U : 4U;
            info.color = published.type == ajfx::ParameterType::Color;
            info.min = published.min;
            info.max = published.max;
            info.fallback = published.default_value;
            inspector_elements.back().particle_parameters.push_back(std::move(info));
          }
        } else if (observed.state == render::ParticlePackageState::Failed) {
          inspector_elements.back().particle_status = observed.error.empty()
              ? "The package could not be read; check its source file"
              : "The package could not be read: " + observed.error;
        }
      }
      if (element->kind == ElementKind::Model && !element->linked) {
        const auto observed = impl.inspector_model_cache.observe(element->source,
            impl.inspector_model_cache.publication_epoch());
        if (observed.asset) inspector_elements.back().model_material_parts =
            static_cast<core::u32>(observed.asset->parts.size());
        else if (observed.state == render::ModelCacheState::Failed)
          inspector_elements.back().model_material_status = "Model import failed; check its source file";
      }
    }
  }
  const InspectorFrame inspector = impl.inspector_panel.draw(
      ui, bounds, std::span<const ElementInfo>{inspector_elements}, anchor,
      keyframes, timeline.show_frame, effects, tracking_sources);
  if (inspector.file_picker) {
    const FilePickerRequest &request = *inspector.file_picker;
    if (request.clear_texture) {
      apply_inspector_file(request, {});
    } else {
      open_file_dialog([request] { return choose_property_file(request); },
                       [request](Frame &frame, std::string path) {
                         frame.apply_inspector_file(request, std::move(path));
                       });
    }
  }
  if (inspector.panel_request.has_value()) {
    pending_inspector_panel = inspector.panel_request;
  }
  // A wall clicked on the Inspector's rig: that slot screen is selected and
  // its row listed under its car on the Timeline.
  if (inspector.select_node.has_value()) {
    const ElementInfo *const wall =
        find_element(elements, *inspector.select_node);
    if (wall != nullptr && !wall->locked) {
      impl.stage_surface_selection.reset();
      impl.select_only(wall->node);
      impl.timeline_panel.reveal(wall->node, ui.time_seconds());
    }
  }
  if (inspector.open_asset.has_value() &&
      asset_registry.asset(*inspector.open_asset) != nullptr) {
    impl.show_panel(panel_asset_viewer);
    impl.asset_viewer_panel.open(*inspector.open_asset);
  }
  if (!inspector.effect_adds.empty()) {
    const auto gesture = impl.next_edit_gesture++;
    for (const auto& effect : inspector.effect_adds)
      result.effects.push_back({effect.target, effect.kind, gesture});
  }
  if (inspector.color_sample_request.has_value()) {
    impl.color_sample_request = inspector.color_sample_request;
  }
  core::u64 inspector_gesture{};
  if (!inspector.component_edits.empty() ||
      !inspector.named_edits.empty() || !inspector.text_edits.empty()) {
    inspector_gesture = inspector.numeric_nudge.has_value()
        ? begin_semantic_nudge(*inspector.numeric_nudge,
                               HistoryContext::Scene,
                               scene_selection_token())
        : impl.begin_gesture();
  }
  for (const ComponentEdit &edit : inspector.component_edits) {
    doc::NodeId node = edit.node;
    scene::TransformChannel component = edit.component;
    double value = edit.value;
    // A locked screen derives its height from its width, so a height the
    // operator types has to become the width that produces it. Writing
    // scale_y instead would author a number evaluation then ignores - a
    // field that visibly does nothing, which is worse than no field.
    if (component == scene::TransformChannel::ScaleY) {
      const ElementInfo *const target = find_element(elements, node);
      if (target != nullptr && target->kind == ElementKind::Track &&
          target->aspect_locked && target->raster_height != 0U) {
        component = scene::TransformChannel::ScaleX;
        value = value *
                static_cast<double>(target->raster_width) /
                static_cast<double>(target->raster_height);
      }
    }
    if (impl.timeline_panel.auto_key_enabled() &&
        keyed(keyframes, node, component)) {
      result.keys.push_back(KeyIntent{node, component,
                                      timeline.show_frame, value,
                                      false, inspector_gesture});
    } else {
      result.property_edits.push_back(
          PropertyEdit{node, component, value, inspector_gesture});
    }
  }
  for (const auto& edit : inspector.text_edits) {
    if (edit.property == "link_action") {
      if (edit.value == "connect") { impl.blender_connection_open = true; continue; }
      if (edit.value == "disconnect") { result.stop_scene_link = true; continue; }
      if (edit.value == "details") { impl.blender_connection_open = true; continue; }
      if (edit.value == "copy") { request_clip_clipboard(ClipClipboardAction::Copy); continue; }
      if (edit.value == "unlink") result.stop_scene_link = true;
    }
    result.generic_edits.push_back(GenericEdit{edit.node, edit.property, 0.0, inspector_gesture, edit.value});
  }
  for (const NamedEdit &edit : inspector.named_edits) {
    result.generic_edits.push_back(GenericEdit{
        edit.node, edit.property, edit.value, inspector_gesture});
  }
  if (!inspector.effect_deletes.empty()) {
    const core::u64 gesture = impl.next_edit_gesture++;
    for (const doc::NodeId effect : inspector.effect_deletes) {
      result.effect_deletes.push_back(
          EffectDeleteIntent{effect, gesture});
    }
  }
  if (!inspector.key_edits.empty()) {
    const core::u64 gesture = impl.next_edit_gesture++;
    for (const InspectorKeyEdit &key : inspector.key_edits) {
      result.keys.push_back(KeyIntent{key.node, key.component,
                                      timeline.show_frame, key.value, false,
                                      gesture, key.erase, key.parameter});
    }
  }
  if (inspector.add_tracking_source) {
    // Authored like any other node, so it undoes and replicates the same
    // way. It lands unbound and switched off: the operator picks it in the
    // list below and gives it a port, and until they do it is honestly not
    // listening rather than half-configured.
    result.adds.push_back(AddIntent{AddKind::TrackingSource,
                                    std::string{},
                                    1U,
                                    impl.next_edit_gesture++,
                                    16.0F / 9.0F,
                                    {},
                                    authored_stage.id});
  }
  if (inspector.delete_requested) {
    delete_selection();
  }
}

// A picked (or cleared) resource path, applied on the frame that adopts the
// operator's answer as one discrete edit.
void EditorShell::Frame::apply_inspector_file(const FilePickerRequest &request,
                                              std::string path) {
  if (path.size() > 1024U) {
    impl.ingest_message = "Resource path exceeds 1024 bytes";
  } else if (request.kind == FilePickerKind::ModelTexture && !request.targets.empty()) {
    const auto target = request.targets.front();
    const auto busy = std::any_of(impl.pending_model_ingests.begin(), impl.pending_model_ingests.end(),
        [&](const auto& pending) { return pending.texture_target == target; });
    if (busy || impl.pending_model_ingests.size() >= 16U) {
      impl.ingest_message = "A texture assignment is still preparing; wait for it to finish";
    } else {
      auto job = render::ModelIngestJob::start(request.model_source, {}, 1.0F,
          render::ModelTextureAssignment{request.material_part, request.texture_role, path});
      if (job) {
        Impl::PendingModelIngest pending{};
        pending.job = std::move(job);
        pending.display_name = std::filesystem::path{request.model_source}.filename().string();
        pending.gesture = impl.next_edit_gesture++;
        pending.project_generation = project.generation;
        pending.texture_target = target;
        pending.expected_source = request.model_source;
        impl.pending_model_ingests.push_back(std::move(pending));
        impl.ingest_message = "Preparing material texture...";
      } else impl.ingest_message = "Model worker is busy; retry texture assignment";
    }
  } else if (request.kind == FilePickerKind::GeneratorTexture && !request.targets.empty()) {
    constexpr std::array<std::string_view, 5U> texture_properties{
        scene::schema::base_color_texture,
        scene::schema::metallic_roughness_texture,
        scene::schema::normal_texture,
        scene::schema::occlusion_texture,
        scene::schema::emissive_texture};
    if (request.texture_role < texture_properties.size()) {
      const auto gesture = impl.next_edit_gesture++;
      result.generic_edits.push_back(GenericEdit{request.targets.front(),
          std::string{texture_properties[request.texture_role]}, 0.0, gesture, path});
    }
  } else {
    const auto gesture = impl.next_edit_gesture++;
    for (const auto node : request.targets)
      result.generic_edits.push_back(GenericEdit{node, std::string{scene::schema::ies_source}, 0.0, gesture, path});
  }
}

} // namespace mediaserver::editor
