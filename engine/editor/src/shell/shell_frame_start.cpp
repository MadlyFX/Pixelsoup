// Starting a frame: input, project changes, media probes and the Stage's views.

#include "shell_frame.hpp"

namespace mediaserver::editor {

bool EditorShell::Frame::start() {
  impl.stage_history_highlight.swap(impl.history_hover);
  impl.history_hover.clear();
  // The plates the Director derived this frame, drawn over both viewports so
  // an operator can see the regions the physical cameras are looking at. A
  // camera drawing nothing contributes no entry, which is also when there is
  // nothing truthful to outline.
  for (const TrackedCameraObservation &tracked : live_data.cameras) {
    if (tracked.inner_rect.has_value()) {
      plate_overlay.push_back(ViewportPlate{
          *tracked.inner_rect, scene::clamped(tracked.icvfx).soft_edge_px});
    }
  }
  impl.semantic_boundary = false;
  // A hidden Library has no rows to retain. Keeping this intent engaged makes
  // cancellation deterministic when the operator closes or switches tabs.
  result.thumbnail_retain = LibraryThumbnailRetainIntent{};
  result.filmstrip_retain = TimelineFilmstripRetainIntent{};
  shell.reset_library_thumbnail_textures(project.generation);
  shell.reset_timeline_filmstrip_textures(project.generation);

  if (!impl.window.pump(impl.input)) {
    if (project.dirty) {
      // Asked off the frame thread; the editor closes on the frame that
      // adopts the operator's yes.
      impl.window.cancel_close_request();
      confirm_discard("those changes and close", [](Frame &frame) {
        frame.result.status = EditorStatus::Closed;
      });
      return false;
    }
    result.status = EditorStatus::Closed;
    return false;
  }
  adopt_dialog();
  if (result.status == EditorStatus::Closed) {
    return false;
  }
  offer_recovery();
  if (impl.window.pixel_width() != 0U && impl.window.pixel_height() != 0U) {
    impl.workspace_window_width = impl.window.pixel_width();
    impl.workspace_window_height = impl.window.pixel_height();
  }
  // Interface zoom lands before anything is laid out, so a frame's pointer,
  // layout, and glyphs all agree on one scale. The keys are read straight from
  // the input because the UiContext has not started its frame yet.
  if (!impl.ui.typing()) {
    for (const gui::Key key : impl.input.keys()) {
      if (key == gui::Key::ZoomIn || key == gui::Key::ZoomOut) {
        impl.ui_zoom_percent =
            next_ui_zoom(impl.ui_zoom_percent, key == gui::Key::ZoomIn);
      } else if (key == gui::Key::ZoomReset) {
        impl.ui_zoom_percent = 100U;
      }
    }
  }
  impl.window.set_zoom(static_cast<float>(impl.ui_zoom_percent) / 100.0F);
  if (const float scale = impl.window.ui_scale();
      std::fabs(scale - impl.font.scale()) > 0.001F) {
    // A new display scale or zoom: re-bake the glyphs for it. The context
    // holds the atlas by reference, so it is replaced in place.
    impl.font = load_editor_font(scale);
    impl.draw_list.set_white_texel(impl.font.white_u(), impl.font.white_v());
    static_cast<void>(impl.window.set_atlas(impl.font));
  }
  impl.draw_list.set_scale(impl.window.ui_scale());
  if (impl.project_generation != project.generation) {
    impl.semantic_boundary = true;
    impl.pending_model_ingests.clear();
    impl.pending_revisions.clear();
    if (impl.bake_job) impl.bake_job->cancel();
    impl.bake_job.reset();
    impl.bake_message.clear();
    impl.completed_bake_content.clear();
    impl.program_wait_started = {};
    impl.stage_assets = {true, "LOADING STAGE ASSETS...",
                         "Preparing the first Stage image."};
    impl.asset_gate.reset();
    impl.assets = {};
    impl.uploaded_asset_digest = 0;
    for (auto& pending : impl.pending_recordings) pending = {};
    impl.frame_timing = {};
    impl.visible_program.reset();
    impl.program_preview_valid = false;
    impl.ingest_message.clear();
    impl.project_generation = project.generation;
    impl.scene_selection.clear();
    impl.selection_set_selection = {};
    impl.mask_selection = {};
    impl.mask_coverage_visible = false;
    impl.color_sample_request.reset();
    impl.pending_group_members.clear();
    impl.pending_group_frames = 0U;
    impl.stage_surface_selection.reset();
    impl.program_output_map_node.reset();
    impl.pending_scene_delete.reset();
    impl.program_projection_cache.reset();
    impl.program_panel.reset_project_state();
    for (Impl::StageViewport &view : impl.stage_views) {
      view.panel.reset_project_state();
      view.assets = impl.stage_assets;
    }
    impl.group_nodes.clear();
    impl.group_start.clear();
    impl.manipulating = false;
    impl.navigate_left = false;
    impl.navigate_middle = false;
    impl.navigate_right = false;
    impl.hardware_edit_gesture = 0U;
    impl.walls_offered.clear();
    impl.feed_mapping_edit_gesture = 0U;
    impl.compositor_edit_gesture = 0U;
    impl.clip_edit_gesture = 0U;
    impl.lane_edit_gesture = 0U;
    impl.delete_domain = DeleteDomain::Neutral;
    impl.ui.clear_focus();
    impl.timeline_panel.reset_project_state();
    impl.cues_panel.reset();
    impl.history_panel.reset();
    impl.history_hover.clear();
    impl.hardware_mapping_panel.reset_project_state();
    // A different show stands on a different stage.
    impl.hardware_mapping_panel.frame_devices();
    impl.feed_mapping_panel.reset_project_state();
    impl.compositor_panel.reset_project_state();
    impl.library_panel.reset_project_state();
    impl.asset_viewer_panel.reset_project_state();
    impl.viewer.shown.reset();
    impl.viewer.texture = nullptr;
    impl.viewer.model_request.reset();
    impl.viewer.model_bounds.reset();
    // Every queued thumbnail is answered, drawn or not.
    for (const Impl::ModelThumbnailWork::Job &job :
         impl.model_thumbnails.queue) {
      if (!impl.model_thumbnails.downloading ||
          &job != &impl.model_thumbnails.queue.front()) {
        impl.model_thumbnails.finished.push_back(
            RenderedModelThumbnail{job.key, {}});
      }
    }
    if (impl.model_thumbnails.downloading &&
        !impl.model_thumbnails.queue.empty()) {
      Impl::ModelThumbnailWork::Job front =
          std::move(impl.model_thumbnails.queue.front());
      impl.model_thumbnails.queue.clear();
      impl.model_thumbnails.queue.push_back(std::move(front));
    } else {
      impl.model_thumbnails.queue.clear();
    }
  }

  // A processor's first report of its screens offers a wall card for each,
  // cabled from its LED OUT, as a starting point. Once per processor per
  // session: the operator may delete or change what it placed.
  try {
    for (const topology::HardwareProcessorNode &report :
         hardware_runtime.processors) {
      if (report.screens.empty() || report.stale ||
          report.reachability !=
              topology::HardwareRuntimeReachability::Online ||
          std::find(impl.walls_offered.begin(), impl.walls_offered.end(),
                    report.device) != impl.walls_offered.end()) {
        continue;
      }
      impl.walls_offered.push_back(report.device);
      std::vector<topology::HardwareWallProposal> walls =
          topology::proposed_walls(hardware_map, report);
      if (!walls.empty()) {
        result.hardware_walls.push_back(HardwareWallsRequest{
            report.device, std::move(walls), impl.next_edit_gesture++});
      }
    }
  } catch (const std::bad_alloc &) {
    result.hardware_walls.clear();
  }

  // Media stream discovery can take seconds for multi-gigabyte MOV files.
  // Consume completed work here without ever waiting on the editor thread.
  constexpr std::size_t maximum_concurrent_media_probes = 2U;
  std::size_t active_media_probes = static_cast<std::size_t>(std::count_if(
      impl.pending_media_probes.begin(), impl.pending_media_probes.end(),
      [](const Impl::PendingMediaProbe &probe) {
        return probe.future.valid();
      }));
  for (auto &probe : impl.pending_media_probes) {
    if (active_media_probes >= maximum_concurrent_media_probes) {
      break;
    }
    if (probe.future.valid() || probe.launch_failed) {
      continue;
    }
    try {
      probe.future = std::async(std::launch::async, [source = probe.source] {
        return render::probe_media(source);
      });
      ++active_media_probes;
    } catch (...) {
      probe.launch_failed = true;
    }
  }
  for (auto probe = impl.pending_media_probes.begin();
       probe != impl.pending_media_probes.end();) {
    if (!probe->launch_failed &&
        (!probe->future.valid() ||
         probe->future.wait_for(std::chrono::seconds{0}) !=
             std::future_status::ready)) {
      ++probe;
      continue;
    }
    render::MediaProbe metadata{};
    try {
      if (probe->future.valid()) {
        metadata = probe->future.get();
      }
    } catch (...) {
      // A failed metadata read retains the same safe defaults as the previous
      // synchronous probes; decoding will report the actual media failure.
    }
    if (probe->project_generation == project.generation) {
      const auto rate = asset_frame_rate(metadata.frame_rate);
      if (probe->action == Impl::PendingMediaAction::Add) {
        result.adds.push_back(AddIntent{
            AddKind::Media, probe->source, metadata.frame_count,
            probe->gesture, metadata.aspect(), {}, probe->scene, {}, rate});
      } else {
        assets::MediaAsset asset{};
        asset.display_name =
            std::filesystem::path{probe->source}.filename().string();
        asset.source = probe->source;
        if (metadata.width > 0U && metadata.height > 0U) {
          asset.dimensions =
              assets::PixelDimensions{metadata.width, metadata.height};
        }
        asset.frame_count = metadata.frame_count;
        asset.frame_rate = rate;
        asset.codec = metadata.codec;
        asset.start_timecode = metadata.start_timecode;
        asset.folder = probe->folder;
        if (probe->action == Impl::PendingMediaAction::Revise) {
          asset.id = probe->revise;
          impl.pending_revisions.push_back(AssetRevisionIntent{
              assets::Asset{std::move(asset)}, probe->gesture});
        } else {
          result.asset_imports.push_back(AssetImportIntent{
              assets::Asset{std::move(asset)}, probe->gesture});
        }
      }
    }
    probe = impl.pending_media_probes.erase(probe);
  }

  // Codec compatibility and output-proxy progress are renderer-owned runtime
  // observations supplied by the configured output nodes. The editor never
  // substitutes the operator machine's GPU result for those reports.
  library_proxy_runtime = proxy_runtime;
  // Blender can remove a selected source between editor frames. Keep the
  // stage settings selection, but release rows that no longer exist.
  if (impl.scene_selection.erase_if([&](doc::NodeId node) {
        const ElementInfo *const element = find_element(elements, node);
        return element != nullptr ? element->locked
            : node != project_settings.stage_node;
      })) {
    impl.group_nodes.clear();
    impl.group_start.clear();
    impl.manipulating = false;
    impl.semantic_boundary = true;
  }
  if (impl.stage_surface_selection.has_value()) {
    const output::ProjectionSurface *const selected_surface =
        feed_map.surface_for(*impl.stage_surface_selection);
    if (selected_surface == nullptr || selected_surface->locked) {
      impl.stage_surface_selection.reset();
    }
  }
  return true;
}

void EditorShell::Frame::prepare_views() {
  const scene::Camera stage_camera =
      stage_camera_for(authored_stage.camera, impl.stage_views[0U].orbit);
  if (impl.displayed_scene != authored_stage.id) {
    impl.semantic_boundary = true;
    impl.displayed_scene = authored_stage.id;
    impl.timeline_panel.clear_key_context();
    impl.cues_panel.reset();
    impl.ui.clear_focus();
    const auto displayed =
        std::find_if(switcher.scenes.begin(), switcher.scenes.end(),
                     [&](const scene::SceneInfo &info) {
                       return info.id == authored_stage.id;
                     });
    if (displayed != switcher.scenes.end()) {
      const std::size_t index = static_cast<std::size_t>(
          std::distance(switcher.scenes.begin(), displayed));
      constexpr std::size_t scene_page_size = 4U;
      impl.scene_list_offset =
          index >= scene_page_size ? index - scene_page_size + 1U : 0U;
    }
    impl.scene_selection.clear();
    impl.selection_set_selection = {};
    impl.mask_selection = {};
    impl.mask_coverage_visible = false;
    impl.pending_group_members.clear();
    impl.pending_group_frames = 0U;
    impl.group_nodes.clear();
    impl.group_start.clear();
  }
  if (impl.pending_scene_delete.has_value()) {
    const bool target_still_exists = std::any_of(
        switcher.scenes.begin(), switcher.scenes.end(),
        [&](const scene::SceneInfo &info) {
          return info.id == *impl.pending_scene_delete;
        });
    if (!target_still_exists ||
        *impl.pending_scene_delete == switcher.program_scene ||
        impl.ui.key_pressed(gui::Key::Escape)) {
      impl.pending_scene_delete.reset();
    }
  }
  if (!impl.pending_group_members.empty()) {
    std::vector<doc::NodeId> wanted = impl.pending_group_members;
    std::sort(wanted.begin(), wanted.end());
    const auto created = std::find_if(
        elements.begin(), elements.end(), [&](const ElementInfo &candidate) {
          if (candidate.kind != ElementKind::Group) {
            return false;
          }
          std::vector<doc::NodeId> children;
          for (const ElementInfo &element : elements) {
            if (element.parent_group == candidate.node) {
              children.push_back(element.node);
            }
          }
          std::sort(children.begin(), children.end());
          return children == wanted;
        });
    if (created != elements.end()) {
      const doc::NodeId group = created->node;
      impl.pending_group_members.clear();
      impl.pending_group_frames = 0U;
      impl.select_only(group);
    } else if (impl.pending_group_frames == 0U) {
      impl.pending_group_members.clear();
    } else {
      --impl.pending_group_frames;
    }
  }
  if (impl.pending_new_group_parent.has_value()) {
    const doc::NodeId parent = *impl.pending_new_group_parent;
    const auto created = std::find_if(
        elements.begin(), elements.end(), [&](const ElementInfo &candidate) {
          return candidate.kind == ElementKind::Group && !candidate.linked &&
                 candidate.parent_group.value_or(doc::NodeId{}) == parent &&
                 !impl.pending_new_group_known.contains(candidate.node);
        });
    if (created != elements.end()) {
      impl.select_only(created->node);
      impl.scene_panel.begin_rename(impl.ui, *created);
    } else if (impl.pending_new_group_frames == 0U) {
      impl.pending_new_group_parent.reset();
    } else {
      --impl.pending_new_group_frames;
    }
  }
  stage_scene = authored_stage;
  bypass_sampled_chroma_key(stage_scene, impl.color_sample_request);
  stage_scene.camera = stage_camera;
  stage_scene.canvas = stage_view_canvas(impl.stage_views[0U]);
  // While a Timeline edit is dragged the Stage views draw its edit point
  // instead, set up exactly as the Stage scene is. Copied only then: the
  // drag lasts a gesture, not a show.
  if (switcher.stage_view_override != nullptr) {
    stage_view_scene = *switcher.stage_view_override;
    bypass_sampled_chroma_key(*stage_view_scene, impl.color_sample_request);
    stage_view_scene->camera = stage_scene.camera;
    stage_view_scene->canvas = stage_scene.canvas;
  }
  // Keep the authored environment and lighting. Grid and guides are UI
  // overlays; making them readable must not change the scene's illumination.

  // Stage pixels already include compositor spatial nodes. Use that same
  // adjusted scene for picking and authoring chrome so a selected plate's
  // outline and gizmo land on its apparent rig position. Gesture edits still
  // use stage_scene below as their baseline, preserving the layer transform as
  // a local offset instead of baking the LED setup transform into it.
  stage_interaction_scene = stage_scene;
  if (compositor_graph.valid() && !compositor_graph.plan.steps.empty()) {
    const auto planned_stage =
        render::plan_compositor_scene(compositor_graph, stage_scene);
    if (planned_stage) {
      stage_interaction_scene = planned_stage.value().adjusted_scene;
    }
  }

  // Program's pixels include its own graph's spatial nodes too - a car's
  // walls stand round its rig, not stacked where their tracks were made - so
  // its picking and gizmo read the scene that graph places, as the Stage's
  // do. Edits still start from `scene`, keeping a wall's own transform a
  // correction on its rig socket.
  {
    const compositor::CompositorGraph &program_graph =
        switcher.program_graph != nullptr ? *switcher.program_graph
                                          : compositor_graph;
    if (program_graph.valid() && !program_graph.plan.steps.empty()) {
      auto planned_program =
          render::plan_compositor_scene(program_graph, scene);
      if (planned_program) {
        program_interaction_scene =
            std::move(planned_program.value().adjusted_scene);
      }
    }
  }

  // Both scene targets are rendered before the UI pass. They belong to the
  // same device as UiWindow, so the panels sample them directly with no copy.
  program_output = selected_program_output(feed_map, impl.program_output_map_node);
  program_extent = program_output_extent(scene, program_output, hardware_map);
  const PreviewExtent program_preview = preview_extent(
      program_extent.preview_width, program_extent.preview_height,
      impl.program_panel.resolution());
  program_extent.preview_width = program_preview.width;
  program_extent.preview_height = program_preview.height;
  // The other three views render once four are asked for: their executors
  // are made at the first frame that wants them, and kept.
  if (impl.stage_four && !impl.stage_four_failed &&
      !impl.stage_views[1U].renderer.has_value()) {
    const rhi::TextureFormat format =
        device.capabilities().backend == rhi::BackendKind::D3d12
            ? rhi::TextureFormat::Rgba16Float
            : rhi::TextureFormat::Rgba8Unorm;
    for (std::size_t index = 1U; index < impl.stage_views.size(); ++index) {
      Impl::StageViewport &view = impl.stage_views[index];
      auto renderer = render::CompositorSceneExecutor::create(
          device, 64U, 64U, stage_view_limits(), render::MediaCacheConfig{},
          format);
      auto display = render::OutputTransform::create(device, 64U, 64U);
      if (!renderer || !display) {
        impl.stage_four_failed = true;
        break;
      }
      view.renderer.emplace(std::move(renderer).value());
      view.display.emplace(std::move(display).value());
    }
    if (impl.stage_four_failed) {
      for (std::size_t index = 1U; index < impl.stage_views.size(); ++index) {
        impl.stage_views[index].renderer.reset();
        impl.stage_views[index].display.reset();
      }
      impl.stage_four = false;
      impl.stage_bookmark_note = "Four views are unavailable on this GPU";
      impl.stage_bookmark_note_until = impl.window.time_seconds() + 3.0;
      impl.stage_bookmark_view = 0U;
    }
  }
  stage_view_count = impl.stage_four && !impl.stage_single_only &&
              impl.stage_views[1U].renderer.has_value()
          ? impl.stage_views.size()
          : 1U;
  bool stage_resized = false;
  for (std::size_t index = 0U; index < stage_view_count; ++index) {
    const scene::Canvas canvas = stage_view_canvas(impl.stage_views[index]);
    stage_previews[index] =
        preview_extent(canvas.width, canvas.height,
                       impl.stage_views[index].panel.resolution());
    stage_resized = stage_resized ||
                    stage_renderer_of(index).width() !=
                        stage_previews[index].width ||
                    stage_renderer_of(index).height() !=
                        stage_previews[index].height;
  }
  if (impl.program_preview_width != program_preview.width ||
      impl.program_preview_height != program_preview.height ||
      stage_resized) {
    // An explicit preview resize replaces textures and descriptors referenced
    // by both scene and UI submissions. Drain once at this boundary only.
    device.wait_idle();
  }
  if (impl.program_preview_width != program_extent.preview_width ||
      impl.program_preview_height != program_extent.preview_height) {
    impl.program_preview_valid = false;
    impl.program_preview_width = program_extent.preview_width;
    impl.program_preview_height = program_extent.preview_height;
  }
  program_targets_ready = impl.program_renderer.resize(device, program_extent.preview_width,
                                   program_extent.preview_height) &&
      impl.program_to_renderer.resize(device, program_extent.preview_width,
                                      program_extent.preview_height) &&
      impl.program_warp.resize(device, program_extent.preview_width,
                               program_extent.preview_height) &&
      impl.program_to_warp.resize(device, program_extent.preview_width,
                                  program_extent.preview_height) &&
      impl.program_mixer.resize(device, program_extent.preview_width,
                                program_extent.preview_height) &&
      impl.program_display.resize(device, program_extent.preview_width,
                                  program_extent.preview_height);
  for (std::size_t index = 0U; index < stage_view_count; ++index) {
    stage_targets_ready[index] =
        stage_renderer_of(index).resize(device, stage_previews[index].width,
                                        stage_previews[index].height) &&
        stage_display_of(index).resize(device, stage_previews[index].width,
                                       stage_previews[index].height);
    impl.stage_views[index].texture = &stage_display_of(index).target();
  }
  // The Asset Viewer renders only while it is on screen with a media asset
  // to show. Hidden for a few seconds, it gives its targets back; the drain
  // is the same once-per-boundary wait a preview resize takes.
  viewer_media = nullptr;
  viewer_model = nullptr;
  if (viewer.drawn && asset_viewer.scene != nullptr &&
      asset_viewer.shown.asset == impl.asset_viewer_panel.asset()) {
    if (const assets::Asset *const found =
            asset_registry.asset(asset_viewer.shown.asset)) {
      viewer_media = std::get_if<assets::MediaAsset>(found);
    }
  }
  if (viewer.drawn && viewer.model_request.has_value() &&
      viewer.model_request->asset == impl.asset_viewer_panel.asset()) {
    if (const assets::Asset *const found =
            asset_registry.asset(viewer.model_request->asset)) {
      viewer_model = std::get_if<assets::ModelAsset>(found);
    }
  }
  viewer.hidden_frames = viewer.drawn
                             ? 0U
                             : std::min(viewer.hidden_frames + 1U, 1000U);
  if (viewer.hidden_frames > 180U &&
      (viewer.renderer.has_value() || viewer.readback.has_value())) {
    device.wait_idle();
    viewer.renderer.reset();
    viewer.display.reset();
    viewer.inspection.reset();
    viewer.readback.reset();
    viewer.probe_work = rhi::SubmittedWork{};
    viewer.probe_pending = false;
    viewer.shown.reset();
    viewer.texture = nullptr;
    viewer.failed = false;
  }
  viewer_ready = false;
  if ((viewer_media != nullptr || viewer_model != nullptr) &&
      viewer.width != 0U && viewer.height != 0U && !viewer.failed) {
    if (!viewer.renderer.has_value()) {
      auto renderer = render::CompositorSceneExecutor::create(
          device, viewer.width, viewer.height, stage_view_limits(),
          render::MediaCacheConfig{},
          device.capabilities().backend == rhi::BackendKind::D3d12
              ? rhi::TextureFormat::Rgba16Float
              : rhi::TextureFormat::Rgba8Unorm);
      auto display =
          render::OutputTransform::create(device, viewer.width, viewer.height);
      auto inspection =
          render::InspectionView::create(device, viewer.width, viewer.height);
      if (renderer && display && inspection) {
        viewer.renderer.emplace(std::move(renderer).value());
        viewer.display.emplace(std::move(display).value());
        viewer.inspection.emplace(std::move(inspection).value());
      } else {
        viewer.failed = true;
        viewer.fault = "ASSET VIEWER UNAVAILABLE ON THIS GPU";
      }
    } else if (viewer.renderer->width() != viewer.width ||
               viewer.renderer->height() != viewer.height) {
      device.wait_idle();
      viewer.shown.reset();
      viewer.texture = nullptr;
      if (!viewer.renderer->resize(device, viewer.width, viewer.height) ||
          !viewer.display->resize(device, viewer.width, viewer.height) ||
          !viewer.inspection->resize(device, viewer.width, viewer.height)) {
        viewer.failed = true;
        viewer.fault = "ASSET VIEWER TARGETS UNAVAILABLE";
      }
    }
    viewer_ready = viewer.renderer.has_value() && !viewer.failed;
  }
  // A model thumbnail's download is read the same way, once it is done.
  if (thumbs.downloading && thumbs.download && thumbs.download.complete()) {
    thumbs.downloading = false;
    std::vector<core::u8> pixels;
    if (thumbs.readback.has_value() && !thumbs.queue.empty()) {
      const auto mapped = thumbs.readback->map();
      const std::size_t bytes = std::size_t{model_thumbnail_width} *
                                model_thumbnail_height * 4U;
      if (mapped.size() >= bytes) {
        pixels.assign(mapped.begin(), mapped.begin() + bytes);
      }
      thumbs.readback->unmap();
    }
    if (!thumbs.queue.empty()) {
      thumbs.finished.push_back(RenderedModelThumbnail{
          thumbs.queue.front().key, std::move(pixels)});
      thumbs.queue.pop_front();
    }
  }
  thumbs.idle_frames = thumbs.queue.empty() && !thumbs.downloading
                           ? std::min(thumbs.idle_frames + 1U, 1000U)
                           : 0U;
  if (thumbs.idle_frames > 600U &&
      (thumbs.renderer.has_value() || thumbs.readback.has_value())) {
    device.wait_idle();
    thumbs.renderer.reset();
    thumbs.display.reset();
    thumbs.readback.reset();
    thumbs.download = rhi::SubmittedWork{};
  }
  // A finished probe download is read here, a frame or more after it was
  // asked for, so the probe never waits on the GPU.
  if (viewer.probe_pending && viewer.probe_work &&
      viewer.probe_work.complete()) {
    viewer.probe_pending = false;
    std::optional<gui::UiColor> probed;
    if (viewer.readback.has_value()) {
      const auto pixels = viewer.readback->map();
      const std::size_t offset =
          (static_cast<std::size_t>(viewer.probe_y) * viewer.probe_width +
           viewer.probe_x) *
          4U;
      if (viewer.probe_x < viewer.probe_width &&
          viewer.probe_y < viewer.probe_height &&
          offset + 3U < pixels.size()) {
        constexpr float to_unit = 1.0F / 255.0F;
        probed = gui::UiColor{static_cast<float>(pixels[offset]) * to_unit,
                              static_cast<float>(pixels[offset + 1U]) * to_unit,
                              static_cast<float>(pixels[offset + 2U]) * to_unit,
                              1.0F};
      }
      viewer.readback->unmap();
    }
    impl.asset_viewer_panel.set_probe_value(probed);
  }
  if (!viewer.probe_at.has_value()) {
    impl.asset_viewer_panel.set_probe_value(std::nullopt);
  }
}

void EditorShell::Frame::request_clip_clipboard(ClipClipboardAction action) {
  ClipClipboardRequest request{};
  request.action = action;
  request.scene = authored_stage.id;
  request.playhead = timeline.show_frame;
  if (action == ClipClipboardAction::Copy) {
    request.selection.assign(impl.scene_selection.members().begin(),
                             impl.scene_selection.members().end());
  } else {
    request.transaction = impl.next_edit_gesture++;
  }
  result.clip_clipboard = std::move(request);
  impl.history_context = HistoryContext::Scene;
}

void EditorShell::Frame::request_key_clipboard(TimelineKeyClipboardEdit edit) {
  KeyClipboardRequest request{};
  request.action = edit.action;
  request.scene = authored_stage.id;
  request.playhead = timeline.show_frame;
  request.selection = std::move(edit.keys);
  if (request.action == TimelineKeyClipboardAction::Paste) {
    request.transaction = impl.next_edit_gesture++;
  }
  result.key_clipboard = std::move(request);
  impl.history_context = HistoryContext::Scene;
}

// Marker and cue changes from either panel. One frame's batch is one
// gesture: a multi-marker drag lands on release, so it undoes in one step.
void EditorShell::Frame::route_marker_edits(
    std::span<const TimelineMarkerEdit> edits) {
  if (edits.empty()) {
    return;
  }
  const core::u64 gesture = impl.next_edit_gesture++;
  for (const TimelineMarkerEdit &edit : edits) {
    result.marker_edits.push_back(TimelineMarkerEditIntent{
        edit.action, authored_stage.id, edit.marker, gesture});
  }
  impl.history_context = HistoryContext::Scene;
}

// A Stage view's camera and canvas: its own orbit over the authored camera,
// at its pane's size. The first view's set the scene up; the others swap
// in while they are rendered and drawn.
scene::Camera
EditorShell::Frame::stage_view_camera(const Impl::StageViewport &view) {
  return stage_camera_for(authored_stage.camera, view.orbit);
}

scene::Canvas
EditorShell::Frame::stage_view_canvas(const Impl::StageViewport &view) {
  return view.surface_width > 0U && view.surface_height > 0U
             ? scene::Canvas{view.surface_width, view.surface_height}
             : authored_stage.canvas;
}

render::CompositorSceneExecutor &
EditorShell::Frame::stage_renderer_of(std::size_t index) {
  return index == 0U ? impl.stage_renderer
                     : *impl.stage_views[index].renderer;
}

render::OutputTransform &
EditorShell::Frame::stage_display_of(std::size_t index) {
  return index == 0U ? impl.stage_display : *impl.stage_views[index].display;
}

} // namespace mediaserver::editor
