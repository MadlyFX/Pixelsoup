// The Asset Viewer, Library and Effects panels.

#include "shell_frame.hpp"

#include "native_dialogs.hpp"

namespace mediaserver::editor {

void EditorShell::Frame::draw_asset_viewer_panel(const gui::Rect &bounds) {
  viewer_drawn_now = true;
  const std::optional<doc::NodeId> target =
      impl.timeline_panel.insert_target_lane();
  std::string target_name;
  if (target.has_value()) {
    for (const scene::TimelineLane &lane : timeline_lanes) {
      if (lane.id == *target && !lane.implicit) {
        target_name = lane.name.empty() ? "an unnamed track" : lane.name;
        break;
      }
    }
  }
  // A model is observed in the model cache the renderer shares, never
  // waited for, and its facts are worked out on the viewer's worker.
  const assets::Asset *const viewed_asset =
      asset_registry.asset(impl.asset_viewer_panel.asset());
  const auto *const viewed_model =
      viewed_asset != nullptr ? std::get_if<assets::ModelAsset>(viewed_asset)
                              : nullptr;
  AssetViewerModelInput model_input{};
  if (viewed_model != nullptr) {
    const render::ModelCacheObservation observed =
        impl.viewer.model_cache.observe(
            viewed_model->source,
            impl.viewer.model_cache.publication_epoch());
    if (observed.ready()) {
      model_input.state = AssetViewerModelState::Ready;
      model_input.bounds = WorldBounds{observed.asset->bounds.minimum,
                                       observed.asset->bounds.maximum};
      if (impl.viewer.facts_worker == nullptr) {
        impl.viewer.facts_worker = std::make_unique<ModelFactsWorker>();
      }
      model_input.facts = impl.viewer.facts_worker->facts(
          viewed_model->source, observed.generation, observed.asset);
    } else if (observed.state == render::ModelCacheState::Failed) {
      model_input.state = AssetViewerModelState::Failed;
      model_input.diagnostic =
          "The importer refused the file. Open it in the application "
          "that made it, then Rescan the Library.";
    }
    model_input.checks.texture_budget_bytes =
        static_cast<core::u64>(
            authored_stage.renderer_settings.texture_budget_mib) *
        1024ULL * 1024ULL;
    model_input.checks.projection_mesh = viewed_model->projection_mesh;
    model_input.checks.static_placements = static_cast<core::u32>(
        std::count_if(authored_stage.models.begin(),
                      authored_stage.models.end(),
                      [&](const scene::ModelInstance &placed) {
                        return placed.source == viewed_model->source &&
                               placed.bake_static;
                      }));
    model_input.advanced = authored_stage.renderer_settings.backend ==
                           scene::RendererBackend::Advanced;
    impl.viewer.model_bounds = model_input.bounds;
  }
  const bool picture_current =
      impl.viewer.shown.has_value() &&
      (viewed_model != nullptr
           ? impl.viewer.shown->model.has_value() &&
                 impl.viewer.shown->model->asset ==
                     impl.asset_viewer_panel.asset()
           : !impl.viewer.shown->model.has_value() &&
                 impl.viewer.shown->request.asset ==
                     impl.asset_viewer_panel.asset());
  AssetViewerDrawInput viewer_input{};
  viewer_input.assets = &asset_registry;
  viewer_input.director = &asset_viewer;
  viewer_input.image = picture_current ? impl.viewer.texture : nullptr;
  viewer_input.decoding = picture_current && impl.viewer.decoding;
  if (viewed_model != nullptr && picture_current) {
    viewer_input.loading_detail = impl.viewer.model_status.detail;
    viewer_input.load_failed = !impl.viewer.model_status.loading &&
                               !impl.viewer.model_status.headline.empty();
  }
  viewer_input.image_fault = impl.viewer.fault;
  viewer_input.show_rate =
      scene::MediaFrameRate{timeline.rate_numerator,
                            timeline.rate_denominator};
  viewer_input.target_lane = target;
  viewer_input.target_name = target_name;
  viewer_input.edits_locked = switcher.rehearsal_locked;
  viewer_input.elements = elements;
  viewer_input.lanes = timeline_lanes;
  viewer_input.pixels_per_unit = impl.window.ui_scale();
  viewer_input.model = viewed_model != nullptr ? &model_input : nullptr;
  viewer_input.thumbnails = thumbnails;
  viewer_input.dragging_assets = impl.library_dragging;
  for (const LibraryProxyRuntimeObservation &observation :
       library_proxy_runtime) {
    if (observation.asset == impl.asset_viewer_panel.asset()) {
      viewer_input.editor_proxy = observation.editor_proxy;
      break;
    }
  }
  const doc::NodeId viewed_set = impl.asset_viewer_panel.asset();
  if (asset_viewer.plateset == viewed_set &&
      viewed_set != doc::NodeId{}) {
    viewer_input.plateset_analysis = &asset_viewer.plateset_analysis;
  }
  const AssetViewerFrame viewed =
      impl.asset_viewer_panel.draw(ui, bounds, viewer_input);
  if (viewed.plateset.has_value()) {
    plateset_editor_drawn = true;
    result.plateset_viewed = viewed_set;
    if (viewed.plateset->analyse.has_value()) {
      result.plateset_analyse = viewed_set;
      result.plateset_pathway = *viewed.plateset->analyse;
    }
    plateset_thumbnail_keys = viewed.plateset->thumbnails;
    const core::u64 gesture = impl.next_edit_gesture++;
    for (const assets::LibraryEdit &edit : viewed.plateset->edits) {
      result.asset_library_edits.push_back(
          AssetLibraryEditIntent{edit, gesture});
    }
    for (const LibraryProxyPolicyIntent &proxy :
         viewed.plateset->proxy_edits) {
      result.asset_proxy_edits.push_back(AssetProxyEditIntent{
          proxy.asset, proxy.requested_profile, proxy.policy, gesture});
    }
    if (!viewed.plateset->edits.empty() ||
        !viewed.plateset->proxy_edits.empty()) {
      impl.history_context = HistoryContext::Scene;
      impl.semantic_boundary = true;
    }
  }
  result.asset_viewer = viewed.request;
  impl.viewer.model_request = viewed.model_view;
  if (viewed.choose_hdri) {
    FilePickerRequest request{};
    request.kind = FilePickerKind::EnvironmentMap;
    request.current = impl.asset_viewer_panel.look().hdri_source;
    open_file_dialog([request] { return choose_property_file(request); },
                     [](Frame &frame, std::string path) {
                       frame.impl.asset_viewer_panel.set_hdri(std::move(path));
                     });
  }
  impl.viewer.width = viewed.surface_width;
  impl.viewer.height = viewed.surface_height;
  impl.viewer.probe_at = viewed.probe;
  if (viewed.source_edit.has_value() && target.has_value()) {
    result.source_edits.push_back(SourceRangeEditIntent{
        authored_stage.id, impl.asset_viewer_panel.asset(),
        viewed.source_edit->source_in, viewed.source_edit->source_frames,
        viewed.source_edit->mode, *target, timeline.show_frame,
        scene::MediaFrameRate{timeline.rate_numerator,
                              timeline.rate_denominator},
        impl.next_edit_gesture++});
    impl.history_context = HistoryContext::Scene;
    impl.semantic_boundary = true;
  }
  if (viewed.select_clip.has_value()) {
    if (const ElementInfo *const clip =
            find_element(elements, *viewed.select_clip);
        clip != nullptr && !clip->locked) {
      impl.stage_surface_selection.reset();
      impl.select_only(*viewed.select_clip);
      impl.history_context = HistoryContext::Scene;
      impl.semantic_boundary = true;
    }
  }
  if (viewed.drop.has_value()) {
    pending_viewer_drop = viewed.drop;
  }
  return;
}

void EditorShell::Frame::draw_library_panel(const gui::Rect &bounds) {
  impl.library_panel.set_changed_sources(project.changed_assets);
  const LibraryFrame library =
      impl.library_panel.draw(ui, bounds, asset_registry, thumbnails, {},
                              library_proxy_runtime);
  if (library.rescan_assets) {
    result.rescan_assets = true;
  }
  if (library.add.has_value()) {
    handle_library_add(library.add.value());
  }
  if (library.import_asset.has_value()) {
    import_asset(library.import_asset.value(), library.import_folder);
  }
  if (!library.accept_sources.empty()) {
    // Re-read each changed file on the workers that import uses; the
    // results are held until the last one is back.
    const core::u64 gesture = impl.next_edit_gesture++;
    for (const doc::NodeId id : library.accept_sources) {
      const assets::Asset *const asset = asset_registry.asset(id);
      if (asset == nullptr) {
        continue;
      }
      const std::string source{assets::asset_source(*asset)};
      if (std::holds_alternative<assets::MediaAsset>(*asset)) {
        try {
          Impl::PendingMediaProbe probe{
              Impl::PendingMediaAction::Revise, source, {}, false,
              gesture, {}, project.generation, assets::asset_folder(*asset)};
          probe.revise = id;
          impl.pending_media_probes.push_back(std::move(probe));
        } catch (...) {
          impl.ingest_message = "Could not queue a changed source";
        }
      } else {
        enqueue_model(source, {}, {}, {}, assets::asset_folder(*asset), id,
                      gesture);
      }
    }
  }
  if (!library.import_files.empty()) {
    import_files(library.import_files, library.import_folder);
    library_took_files = true;
  }
  if (library.view_asset.has_value()) {
    impl.asset_viewer_panel.open(*library.view_asset);
    impl.show_panel(panel_asset_viewer);
  }
  // One gesture for everything a single Delete or menu choice removed,
  // so it undoes as one.
  if (!library.remove_assets.empty() || !library.library_edits.empty()) {
    const core::u64 gesture = impl.next_edit_gesture++;
    for (const doc::NodeId removed : library.remove_assets) {
      result.asset_deletes.push_back(AssetDeleteIntent{removed, gesture});
    }
    for (const assets::LibraryEdit &edit : library.library_edits) {
      result.asset_library_edits.push_back(
          AssetLibraryEditIntent{edit, gesture});
    }
  }
  delete_consumed = delete_consumed || library.consume_delete;
  if (library.proxy_policy.has_value()) {
    result.asset_proxy_edits.push_back(AssetProxyEditIntent{
        library.proxy_policy->asset,
        library.proxy_policy->requested_profile,
        library.proxy_policy->policy,
        impl.next_edit_gesture++});
  }
  dragging_asset = library.dragging_asset;
  dragging_assets = library.dragging_assets;
  impl.library_dragging = library.dragging_asset.has_value();
  if (library.asset_drop.has_value()) {
    pending_asset_drop = library.asset_drop;
  }
  dragging_generator = library.dragging_generator;
  if (library.generator_drop.has_value()) {
    pending_generator_drop = library.generator_drop;
  }
  result.thumbnail_requests.insert(result.thumbnail_requests.end(),
                                   library.thumbnail_requests.begin(),
                                   library.thumbnail_requests.end());
  result.thumbnail_retain = library.thumbnail_retain;
  return;
}

void EditorShell::Frame::draw_effects_panel(const gui::Rect &bounds) {
  // The palette states what a drop would land on, so it needs the anchor's
  // name and not only its id. An element with no authored name is named the
  // way every other panel names it.
  const ElementInfo *const drop_target =
      find_element(elements, impl.anchor());
  std::array<char, 64U> target_title{};
  const std::string_view target_name =
      effect_target_name(drop_target, target_title);
  const auto request =
      impl.effects_panel.draw(ui, bounds, impl.anchor(), target_name);
  if (request.has_value()) {
    pending_effect_drop = request;
  }
  return;
}

} // namespace mediaserver::editor
