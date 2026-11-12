// Recording Program, the Stage views and the Asset Viewer, and reporting their readiness.

#include "shell_frame.hpp"

namespace mediaserver::editor {

void EditorShell::Frame::render_views() {
  program_texture = impl.visible_program ? &*impl.visible_program : &impl.program_display.target();
  program_preview_ok = false;
  program_preview_held = false;
  program_mapping_unavailable = false;
  program_preview_fault = std::string_view{"PROGRAM PREVIEW FAILED"};
  std::optional<scene::Scene> sampled_program_scene;
  std::optional<scene::Scene> sampled_transition_from;
  std::optional<scene::Scene> sampled_transition_to;
  const scene::Scene *program_render_scene = &scene;
  const scene::Scene *transition_from_scene = switcher.transition_from;
  const scene::Scene *transition_to_scene = switcher.transition_to;
  if (impl.color_sample_request.has_value()) {
    sampled_program_scene.emplace(scene);
    bypass_sampled_chroma_key(*sampled_program_scene,
                              impl.color_sample_request);
    program_render_scene = &*sampled_program_scene;
    if (switcher.transition_from != nullptr) {
      sampled_transition_from.emplace(*switcher.transition_from);
      bypass_sampled_chroma_key(*sampled_transition_from,
                                impl.color_sample_request);
      transition_from_scene = &*sampled_transition_from;
    }
    if (switcher.transition_to != nullptr) {
      sampled_transition_to.emplace(*switcher.transition_to);
      bypass_sampled_chroma_key(*sampled_transition_to,
                                impl.color_sample_request);
      transition_to_scene = &*sampled_transition_to;
    }
  }
  for (auto& pending : impl.pending_recordings) {
    if (!pending.work || !pending.work.complete()) continue;
    auto completed = pending.work.frame_timing();
    completed.sequence = pending.sequence;
    completed.cpu_record_ns = pending.record_ns;
    completed.profile_digest = pending.profile_digest;
    if (completed.sequence >= impl.frame_timing.sequence) {
      if (pending.asset_digest != 0U) impl.uploaded_asset_digest = pending.asset_digest;
      impl.frame_timing = completed;
      impl.program_renderer.observe_completed_timing(completed);
      impl.program_to_renderer.observe_completed_timing(completed);
      impl.stage_renderer.observe_completed_timing(completed);
      for (Impl::StageViewport &view : impl.stage_views) {
        if (view.renderer.has_value()) {
          view.renderer->observe_completed_timing(completed);
        }
      }
    }
    pending = {};
  }
  auto recording_slot = std::find_if(impl.pending_recordings.begin(), impl.pending_recordings.end(),
      [](const auto& pending) { return !pending.work; });
  const auto cpu_record_begin = std::chrono::steady_clock::now();
  auto acquired = recording_slot == impl.pending_recordings.end()
      ? core::Result<rhi::CommandList, rhi::RhiError>::failure(rhi::RhiError::Unavailable)
      : device.acquire_commands();
  if (acquired) {
    auto commands = std::move(acquired).value();
    auto next_asset_gate = impl.asset_gate;
    impl.frame_media_cache.begin_frame();
    struct EndMediaFrame final {
      render::MediaCache *cache{};
      ~EndMediaFrame() { cache->end_frame(); }
    } end_media_frame{&impl.frame_media_cache};
    bool program_scene_linear = false;
    const auto record_program =
        [&](render::CompositorSceneExecutor &executor,
            render::ProjectionMeshWarp &warp, const scene::Scene &frame,
            const compositor::CompositorGraph *graph) -> const rhi::Texture * {
      if (!program_targets_ready) {
        program_preview_fault = "PROGRAM TARGETS UNAVAILABLE";
        return nullptr;
      }
      // Program composites through the graph of the scene it is showing, not
      // through whichever graph the Compositor panel happens to be editing.
      const compositor::CompositorGraph &program_composite =
          graph != nullptr ? *graph : compositor_graph;
      if (program_output == nullptr) {
        return record_editor_scene(executor, commands, frame,
                                   program_composite,
                                   &impl.frame_media_cache);
      }
      return render::record_output_mapped_scene(
          executor, warp, impl.program_projection_cache, commands,
          program_composite, frame, *program_output,
          program_mapping_unavailable, &impl.frame_media_cache);
    };

    if ((switcher.transition_active || switcher.loop_fade_active) &&
        transition_from_scene != nullptr && transition_to_scene != nullptr) {
      const rhi::Texture *const from_target = record_program(
          impl.program_renderer, impl.program_warp, *transition_from_scene,
          switcher.transition_from_graph);
      renderer_readiness = impl.program_renderer.renderer_readiness();
      const rhi::Texture *const to_target = record_program(
          impl.program_to_renderer, impl.program_to_warp,
          *transition_to_scene, switcher.transition_to_graph);
      if (renderer_readiness->satisfied) {
        // Keep the first unsatisfied side of a transition; otherwise the
        // incoming side is the most recent exact observation.
        renderer_readiness =
            impl.program_to_renderer.renderer_readiness();
      }
      program_preview_ok =
          from_target != nullptr && to_target != nullptr &&
          impl.program_renderer.scene_linear_output() ==
              impl.program_to_renderer.scene_linear_output() &&
          impl.program_mixer.render(commands, *from_target, *to_target,
                                    switcher.transition_mix);
      if (!program_preview_ok) {
        program_preview_fault = "PROGRAM TRANSITION RENDER FAILED";
      }
      if (program_preview_ok) {
        program_texture = &impl.program_mixer.target();
        program_scene_linear =
            impl.program_renderer.scene_linear_output();
      }
    } else {
      const rhi::Texture *const rendered = record_program(
          impl.program_renderer, impl.program_warp, *program_render_scene,
          switcher.program_graph);
      renderer_readiness = impl.program_renderer.renderer_readiness();
      program_preview_ok = rendered != nullptr;
      if (!program_preview_ok) {
        program_preview_fault = renderer_readiness->satisfied
                                    ? "PROGRAM RENDER FAILED"
                                    : renderer_readiness->headline();
      }
      if (program_preview_ok) {
        program_texture = rendered;
        program_scene_linear =
            impl.program_renderer.scene_linear_output();
      }
    }
    if (program_preview_ok) {
      // The project's authored colour, taken once per frame before the draw.
      // Project-wide, so the scene being shown and any transition partner
      // carry the same settings; the region keys the dither to the part of
      // the wall this preview shows, as the node presenting it does.
      impl.program_display.adopt_color(
          program_render_scene->renderer_settings.color, scene::asset_generation_key(*program_render_scene));
      program_preview_ok = impl.program_display.render(
          commands, *program_texture,
          scene::CanvasRect{0U, 0U, program_extent.preview_width,
                            program_extent.preview_height},
          program_scene_linear,
          program_output != nullptr ? program_output->source
                                    : scene::whole(scene.canvas));
      if (!program_preview_ok) {
        program_preview_fault = "PROGRAM DISPLAY TRANSFORM FAILED";
      }
    }
    if (!program_preview_ok) {
      static_cast<void>(commands.clear(
          impl.program_display.target(),
          rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}));
    }
    program_texture = &impl.program_display.target();
    auto prepared = impl.program_renderer.renderer_passes().geometry.assets;
    prepared.ready = prepared.ready && program_render_scene->publication_verified && program_preview_ok && !program_mapping_unavailable && impl.frame_media_cache.stats().pending_requests == 0;
    if ((switcher.transition_active || switcher.loop_fade_active) &&
        transition_from_scene && transition_to_scene) {
      const auto other = impl.program_to_renderer.renderer_passes().geometry.assets;
      // A Loop cue's crossfade goes on publishing the set already on screen,
      // the leaving frame's: a crossfade is not a reveal, and an asset switch
      // mid-mix would hold the picture just as it should be moving.
      if (!switcher.loop_fade_active) {
        prepared.digest ^= other.digest + 0x9e3779b97f4a7c15ULL + (prepared.digest << 6) + (prepared.digest >> 2);
      }
      prepared.ready = prepared.ready && other.ready;
    }
    if (program_scene_linear) prepared.ready = prepared.ready && impl.program_display.color_state() == render::DisplayLutState::Ready;
    impl.assets = prepared;
    impl.assets.ready = prepared.ready && (prepared.digest == 0 || prepared.digest == impl.uploaded_asset_digest);
    impl.assets.visible_commit = impl.assets.digest == impl.asset_gate.visible_digest() ? impl.asset_gate.visible_commit() : 0;
    const bool assets_allowed = next_asset_gate.present(impl.assets, program_render_scene->asset_commit_released ? program_render_scene->asset_commit : core::AssetCommit{},
        program_render_scene->control_frame, program_render_scene->show_frame, program_render_scene->asset_cohort_required);
    if (!impl.visible_program || impl.visible_program->width() != program_texture->width() || impl.visible_program->height() != program_texture->height()) {
      auto texture = device.create_texture(rhi::TextureDesc{program_texture->width(), program_texture->height(), rhi::TextureFormat::Rgba8Unorm, true, true});
      if (texture) { impl.visible_program.emplace(std::move(texture).value()); static_cast<void>(commands.clear(*impl.visible_program, rhi::Color{0,0,0,1})); }
    }
    if (impl.visible_program) {
      if (assets_allowed && program_preview_ok) program_preview_ok = commands.blit(*program_texture, *impl.visible_program, rhi::Filter::Nearest);
      else { program_preview_held = true; program_preview_ok = true; program_preview_fault = "PROGRAM WAITING FOR ASSET COHORT"; }
      program_texture = &*impl.visible_program;
    } else {
      program_preview_ok = false;
      program_preview_fault = "PROGRAM RETAINED TARGET UNAVAILABLE";
    }
    if (!program_preview_ok || program_preview_held) next_asset_gate = impl.asset_gate;

    // The Canvas view of the scene on the Stage, through that scene's own
    // camera: the whole authored canvas, as Program would show it were the
    // scene on air. Drawn by the transition's second executor, which nothing
    // else uses outside a transition.
    canvas_stage_texture = nullptr;
    if (program_canvas_stage && impl.canvas_display.has_value()) {
      scene::Scene &drawn_stage =
          stage_view_scene.has_value() ? *stage_view_scene : stage_scene;
      auto authored_camera = authored_stage.camera;
      auto authored_canvas = authored_stage.canvas;
      std::swap(drawn_stage.camera, authored_camera);
      std::swap(drawn_stage.canvas, authored_canvas);
      const rhi::Texture *const rendered_canvas = record_editor_scene(
          impl.program_to_renderer, commands, drawn_stage, compositor_graph,
          &impl.frame_media_cache);
      std::swap(drawn_stage.camera, authored_camera);
      std::swap(drawn_stage.canvas, authored_canvas);
      impl.canvas_display->adopt_color(drawn_stage.renderer_settings.color,
                                       scene::asset_generation_key(drawn_stage));
      if (rendered_canvas != nullptr &&
          impl.canvas_display->render(
              commands, *rendered_canvas,
              scene::CanvasRect{0U, 0U, program_extent.preview_width,
                                program_extent.preview_height},
              impl.program_to_renderer.scene_linear_output())) {
        canvas_stage_texture = &impl.canvas_display->target();
      }
      impl.canvas_preview_valid = canvas_stage_texture != nullptr;
    }

    // The Stage cameras are a render override, not the geometry policy
    // camera. Keep the authored camera/canvas for LOD and shadow selection
    // while the matrices below drive each view. Swap the small camera
    // carriers only; copying the scene's thousands of drawable records again
    // would undo the submission savings this boundary exists to provide.
    std::array<StageAssetStatus, 4U> stage_assets{};
    for (std::size_t index = 0U; index < stage_view_count; ++index) {
      Impl::StageViewport &view = impl.stage_views[index];
      render::CompositorSceneExecutor &renderer = stage_renderer_of(index);
      render::OutputTransform &display = stage_display_of(index);
      const scene::Camera view_camera = stage_view_camera(view);
      const scene::Canvas view_canvas = stage_view_canvas(view);
      render::SceneRenderView stage_view{};
      stage_view.matrices = scene::view_for_slice(view_camera, view_canvas,
                                                  scene::whole(view_canvas));
      stage_view.eye = view_camera.eye;
      stage_view.forward =
          scene::normalize(view_camera.target - view_camera.eye);
      // A projection change is a cut, not camera motion: each one keeps its
      // own temporal history.
      stage_view.history_identity =
          0x5354'4147'4556'4945ULL ^
          static_cast<core::u64>(view.orbit.projection);
      stage_view.clip_planes = render::SceneClipPlanes{
          view_camera.near_plane, view_camera.far_plane};
      stage_view.shading = view.shading;
      scene::Scene &drawn_stage =
          stage_view_scene.has_value() ? *stage_view_scene : stage_scene;
      auto geometry_camera = authored_stage.camera;
      auto geometry_canvas = authored_stage.canvas;
      std::swap(drawn_stage.camera, geometry_camera);
      std::swap(drawn_stage.canvas, geometry_canvas);
      const rhi::Texture *const rendered_stage =
          stage_targets_ready[index]
              ? record_editor_scene(renderer, commands, drawn_stage,
                                    compositor_graph, &impl.frame_media_cache,
                                    &stage_view)
              : nullptr;
      std::swap(drawn_stage.camera, geometry_camera);
      std::swap(drawn_stage.canvas, geometry_canvas);
      display.adopt_color(drawn_stage.renderer_settings.color,
                          scene::asset_generation_key(drawn_stage));
      const bool stage_ok =
          rendered_stage != nullptr &&
          display.render(commands, *rendered_stage,
                         scene::CanvasRect{0U, 0U, stage_previews[index].width,
                                           stage_previews[index].height},
                         renderer.scene_linear_output());
      StageAssetStatus &status = stage_assets[index];
      // Only media with nothing to show raises the banner: a clip holding
      // its last picture while the next frame, or its proxy, decodes is
      // already on screen.
      status = stage_asset_status(
          renderer.renderer_passes().geometry,
          renderer.media_cache_stats().blank_requests,
          rendered_stage != nullptr ? display.color_state()
                                    : render::DisplayLutState::Ready);
      if (!stage_targets_ready[index]) {
        status = {false, "STAGE TARGETS UNAVAILABLE",
                  "The Stage render target could not be created."};
      } else if (!renderer.renderer_readiness().satisfied) {
        status = {false, "STAGE RENDER UNAVAILABLE",
                  std::string{renderer.renderer_readiness().headline()}};
      } else if (!stage_ok && status.headline.empty()) {
        status = {false, "STAGE RENDER FAILED", "Check Renderer diagnostics."};
      }
      if (!stage_ok) {
        static_cast<void>(commands.clear(display.target(),
                                         rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}));
      }
      view.texture = &display.target();
    }
    // A model in the Asset Viewer's look-dev scene, through its orbit.
    // Redrawn only when what it shows changes - a turn of the turntable or a
    // frame of its clip is a change - or while its geometry, textures or
    // colour table are still arriving.
    if (viewer_ready && viewer_model != nullptr) {
      const AssetViewerModelView &request = *viewer.model_request;
      const bool stage_lit =
          request.look.environment == AssetViewerEnvironment::Stage;
      Impl::AssetViewerRender::Shown wanted{};
      wanted.width = viewer.width;
      wanted.height = viewer.height;
      wanted.color = authored_stage.renderer_settings.color;
      wanted.source = viewer_model->source;
      wanted.model = request;
      wanted.model_bounds = viewer.model_bounds;
      if (stage_lit) {
        wanted.stage_environment = authored_stage.environment;
        wanted.stage_lights = authored_stage.directional_lights;
      }
      if (!viewer.shown.has_value() || *viewer.shown != wanted ||
          viewer.decoding ||
          viewer.display->color_state() != render::DisplayLutState::Ready) {
        const WorldBounds box =
            asset_viewer_model_bounds(*viewer_model, viewer.model_bounds);
        scene::Scene viewer_scene = asset_viewer_model_scene(
            *viewer_model, request, box, authored_stage.renderer_settings,
            &authored_stage);
        viewer_scene.canvas = scene::Canvas{viewer.width, viewer.height};
        const scene::Camera &camera = viewer_scene.camera;
        render::SceneRenderView viewer_view{};
        viewer_view.matrices = scene::view_for_slice(
            camera, viewer_scene.canvas, scene::whole(viewer_scene.canvas));
        viewer_view.eye = camera.eye;
        viewer_view.forward = scene::normalize(camera.target - camera.eye);
        viewer_view.history_identity = 0x4153'5345'544D'4F44ULL;
        viewer_view.clip_planes =
            render::SceneClipPlanes{camera.near_plane, camera.far_plane};
        viewer_view.shading = request.shading;
        viewer_view.model_part = request.part;
        const rhi::Texture *const rendered = record_editor_scene(
            *viewer.renderer, commands, viewer_scene,
            compositor::CompositorGraph{}, nullptr, &viewer_view);
        viewer.display->adopt_color(viewer_scene.renderer_settings.color,
                                    scene::asset_generation_key(viewer_scene));
        const bool viewer_ok =
            rendered != nullptr &&
            viewer.display->render(
                commands, *rendered,
                scene::CanvasRect{0U, 0U, viewer.width, viewer.height},
                viewer.renderer->scene_linear_output());
        viewer.model_status = stage_asset_status(
            viewer.renderer->renderer_passes().geometry,
            viewer.renderer->media_cache_stats().blank_requests,
            viewer.display->color_state());
        viewer.decoding =
            !viewer.renderer->renderer_passes().geometry.assets.ready;
        if (viewer_ok) {
          viewer.shown = std::move(wanted);
          viewer.texture = &viewer.display->target();
          viewer.fault.clear();
        } else {
          viewer.shown.reset();
          viewer.texture = nullptr;
          viewer.fault =
              viewer.renderer->renderer_readiness().satisfied
                  ? std::string{"ASSET VIEWER RENDER FAILED"}
                  : std::string{
                        viewer.renderer->renderer_readiness().headline()};
        }
      }
    }
    // The Asset Viewer's one-clip scene, through the orthographic view its
    // zoom and pan describe. Redrawn only when what it shows changes, or
    // while its frames or colour table are still arriving.
    if (viewer_ready && viewer_media != nullptr) {
      const AssetViewerFraming framing = impl.asset_viewer_panel.framing();
      Impl::AssetViewerRender::Shown wanted{};
      wanted.request = asset_viewer.shown;
      wanted.source = asset_viewer.scene->media.empty()
                          ? std::string{}
                          : asset_viewer.scene->media.front().source;
      wanted.rendition = asset_viewer.rendition;
      wanted.framing = framing;
      wanted.channel = impl.asset_viewer_panel.channel();
      wanted.exposure = impl.asset_viewer_panel.exposure_stops();
      wanted.width = viewer.width;
      wanted.height = viewer.height;
      wanted.color = asset_viewer.scene->renderer_settings.color;
      if (!viewer.shown.has_value() || *viewer.shown != wanted ||
          viewer.decoding ||
          viewer.display->color_state() != render::DisplayLutState::Ready) {
        scene::Scene viewer_scene = *asset_viewer.scene;
        viewer_scene.canvas = scene::Canvas{viewer.width, viewer.height};
        const scene::Camera camera = asset_viewer_camera(
            framing, asset_viewer_pixels(*viewer_media),
            static_cast<float>(viewer.width),
            static_cast<float>(viewer.height));
        render::SceneRenderView viewer_view{};
        viewer_view.matrices = scene::view_for_slice(
            camera, viewer_scene.canvas, scene::whole(viewer_scene.canvas));
        viewer_view.eye = camera.eye;
        viewer_view.forward = scene::Vec3{0.0F, 0.0F, -1.0F};
        viewer_view.history_identity = 0x4153'5345'5456'4945ULL;
        viewer_view.clip_planes =
            render::SceneClipPlanes{camera.near_plane, camera.far_plane};
        const rhi::Texture *const rendered = record_editor_scene(
            *viewer.renderer, commands, viewer_scene,
            compositor::CompositorGraph{}, nullptr, &viewer_view);
        viewer.display->adopt_color(viewer_scene.renderer_settings.color,
                                    scene::asset_generation_key(viewer_scene));
        static constexpr std::array<render::InspectionChannel, 5U> channels{
            render::InspectionChannel::Rgb, render::InspectionChannel::Red,
            render::InspectionChannel::Green, render::InspectionChannel::Blue,
            render::InspectionChannel::Luma};
        const bool viewer_ok =
            rendered != nullptr &&
            viewer.display->render(
                commands, *rendered,
                scene::CanvasRect{0U, 0U, viewer.width, viewer.height},
                viewer.renderer->scene_linear_output()) &&
            viewer.inspection->render(
                commands, viewer.display->target(),
                channels[static_cast<std::size_t>(wanted.channel)],
                wanted.exposure);
        viewer.decoding =
            viewer.renderer->media_cache_stats().pending_requests != 0U;
        if (viewer_ok) {
          viewer.shown = std::move(wanted);
          viewer.texture = &viewer.inspection->target();
          viewer.fault.clear();
        } else {
          viewer.shown.reset();
          viewer.texture = nullptr;
          viewer.fault =
              viewer.renderer->renderer_readiness().satisfied
                  ? std::string{"ASSET VIEWER RENDER FAILED"}
                  : std::string{
                        viewer.renderer->renderer_readiness().headline()};
        }
      }
    }
    const auto record_ns = static_cast<core::u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - cpu_record_begin).count());
    const auto profile_digest = scene::renderer_settings_digest(program_render_scene->renderer_settings);
    auto submitted = device.submit(std::move(commands));
    if (!submitted) {
      program_preview_ok = false;
      program_preview_fault = "PROGRAM SUBMIT FAILED";
      impl.canvas_preview_valid = false;
      canvas_stage_texture = nullptr;
      impl.stage_assets = {false, "STAGE SUBMIT FAILED",
                           "The Stage image could not be submitted to the GPU."};
      for (std::size_t index = 0U; index < stage_view_count; ++index) {
        impl.stage_views[index].assets = impl.stage_assets;
      }
    } else {
      // What the Stage reports is the first view's, or the first view that
      // has something to say.
      impl.stage_assets = stage_assets[0U];
      for (std::size_t index = 0U; index < stage_view_count; ++index) {
        impl.stage_views[index].assets = std::move(stage_assets[index]);
        if (impl.stage_assets.headline.empty()) {
          impl.stage_assets = impl.stage_views[index].assets;
        }
      }
      impl.asset_gate = next_asset_gate;
      impl.assets.visible_commit = impl.assets.digest == impl.asset_gate.visible_digest() ? impl.asset_gate.visible_commit() : 0;
      *recording_slot = {std::move(submitted).value(), prepared.ready ? prepared.digest : 0U,
          record_ns, profile_digest, impl.next_recording_sequence++};
    }
    impl.program_preview_valid = program_preview_ok;
    // The probe reads the display image the panel shows (before the channel
    // view), in its own small submission, at most ten times a second.
    if (viewer_ready && viewer_media != nullptr && viewer.shown.has_value() &&
        viewer.probe_at.has_value() && !viewer.probe_pending &&
        impl.window.time_seconds() - viewer.probe_time >= 0.1 &&
        viewer.display->target().desc().format ==
            rhi::TextureFormat::Rgba8Unorm) {
      const rhi::Texture &image = viewer.display->target();
      const std::uint64_t bytes =
          static_cast<std::uint64_t>(image.width()) * image.height() * 4U;
      if (bytes != 0U && bytes <= std::numeric_limits<core::u32>::max()) {
        if (!viewer.readback.has_value() ||
            viewer.readback->capacity_bytes() < bytes) {
          viewer.readback.reset();
          if (auto created =
                  device.create_readback(static_cast<core::u32>(bytes))) {
            viewer.readback.emplace(std::move(created).value());
          }
        }
        if (viewer.readback.has_value()) {
          if (auto probe_commands = device.acquire_commands()) {
            auto list = std::move(probe_commands).value();
            if (list.download(image, *viewer.readback)) {
              if (auto work = device.submit(std::move(list))) {
                viewer.probe_work = std::move(work).value();
                viewer.probe_pending = true;
                viewer.probe_x = viewer.probe_at->first;
                viewer.probe_y = viewer.probe_at->second;
                viewer.probe_width = image.width();
                viewer.probe_height = image.height();
                viewer.probe_time = impl.window.time_seconds();
              }
            }
          }
        }
      }
    }
    // The next queued model thumbnail. A model still loading is asked again
    // next frame; once it, its textures and the colour table have arrived
    // and a second draw agrees, the picture is downloaded.
    if (!thumbs.queue.empty() && !thumbs.downloading) {
      Impl::ModelThumbnailWork::Job &job = thumbs.queue.front();
      const render::ModelCacheObservation observed =
          impl.viewer.model_cache.observe(
              job.source, impl.viewer.model_cache.publication_epoch());
      const bool refused =
          (!observed.ready() &&
           observed.state == render::ModelCacheState::Failed) ||
          ++job.waited > model_thumbnail_wait_frames ||
          job.drawn > model_thumbnail_draw_frames;
      if (!refused && observed.ready() && !thumbs.renderer.has_value()) {
        render::RendererLimits limits{};
        limits.view_shading = true;
        auto made = render::CompositorSceneExecutor::create(
            device, model_thumbnail_width, model_thumbnail_height, limits,
            render::MediaCacheConfig{},
            device.capabilities().backend == rhi::BackendKind::D3d12
                ? rhi::TextureFormat::Rgba16Float
                : rhi::TextureFormat::Rgba8Unorm);
        auto display = render::OutputTransform::create(
            device, model_thumbnail_width, model_thumbnail_height);
        if (made && display) {
          thumbs.renderer.emplace(std::move(made).value());
          thumbs.display.emplace(std::move(display).value());
        }
      }
      if (refused || (observed.ready() && !thumbs.renderer.has_value())) {
        thumbs.finished.push_back(RenderedModelThumbnail{job.key, {}});
        thumbs.queue.pop_front();
      } else if (observed.ready()) {
        auto thumb_commands = device.acquire_commands();
        if (thumb_commands) {
          rhi::CommandList list = std::move(thumb_commands).value();
          assets::ModelAsset model{};
          model.source = job.source;
          const WorldBounds bounds{observed.asset->bounds.minimum,
                                   observed.asset->bounds.maximum};
          AssetViewerModelView look{};
          look.orbit = asset_viewer_model_home(bounds);
          look.look.grid = false;
          look.look.figure = false;
          // Fixed settings, so a thumbnail depends on the model alone.
          scene::RendererSettings settings{};
          settings.backend = scene::RendererBackend::Advanced;
          scene::Scene picture =
              asset_viewer_model_scene(model, look, bounds, settings, nullptr);
          picture.canvas =
              scene::Canvas{model_thumbnail_width, model_thumbnail_height};
          render::SceneRenderView thumb_view{};
          thumb_view.matrices = scene::view_for_slice(
              picture.camera, picture.canvas, scene::whole(picture.canvas));
          thumb_view.eye = picture.camera.eye;
          thumb_view.forward =
              scene::normalize(picture.camera.target - picture.camera.eye);
          thumb_view.history_identity = 0x4153'5345'5448'554DULL;
          thumb_view.clip_planes = render::SceneClipPlanes{
              picture.camera.near_plane, picture.camera.far_plane};
          const rhi::Texture *const drawn = record_editor_scene(
              *thumbs.renderer, list, picture, compositor::CompositorGraph{},
              nullptr, &thumb_view);
          thumbs.display->adopt_color(picture.renderer_settings.color);
          const bool shown =
              drawn != nullptr &&
              thumbs.display->render(
                  list, *drawn,
                  scene::CanvasRect{0U, 0U, model_thumbnail_width,
                                    model_thumbnail_height},
                  thumbs.renderer->scene_linear_output());
          ++job.drawn;
          const bool ready =
              shown && thumbs.renderer->renderer_passes().geometry.assets.ready &&
              thumbs.display->color_state() == render::DisplayLutState::Ready &&
              thumbs.display->target().desc().format ==
                  rhi::TextureFormat::Rgba8Unorm;
          bool fetch = ready && job.settled;
          job.settled = ready;
          if (fetch && !thumbs.readback.has_value()) {
            if (auto created = device.create_readback(
                    model_thumbnail_width * model_thumbnail_height * 4U)) {
              thumbs.readback.emplace(std::move(created).value());
            }
          }
          fetch = fetch && thumbs.readback.has_value() &&
                  list.download(thumbs.display->target(), *thumbs.readback);
          if (auto work = device.submit(std::move(list)); work && fetch) {
            thumbs.download = std::move(work).value();
            thumbs.downloading = true;
          }
        }
      }
    }
  } else if (program_targets_ready && impl.program_preview_valid) {
    // Command acquisition is deliberately non-blocking. Keep the completed
    // prior image while all contexts are in flight; this is pressure, not a
    // renderer fault, and the next frame retries normally.
    program_preview_ok = true;
    program_preview_held = true;
  } else {
    program_preview_fault = "PROGRAM GPU BUSY";
  }
  // The same pressure leaves the Preview canvas on its last whole picture.
  if (!acquired && program_canvas_stage && impl.canvas_preview_valid &&
      impl.canvas_display.has_value()) {
    canvas_stage_texture = &impl.canvas_display->target();
  }
  result.renderer_readiness = renderer_readiness;
  result.render_adapter = impl.window.device().adapter_identity();
  result.frame_timing = impl.frame_timing;
  result.temporal_restarts = impl.program_renderer.temporal_restarts();
  result.temporal_restart_frame = impl.program_renderer.temporal_restart_frame();
  if (impl.program_to_renderer.temporal_document_version() == impl.program_renderer.temporal_document_version()
      && impl.program_to_renderer.renderer_profile_digest() == impl.program_renderer.renderer_profile_digest()) {
    result.temporal_restarts = std::max(result.temporal_restarts, impl.program_to_renderer.temporal_restarts());
    result.temporal_restart_frame = std::max(result.temporal_restart_frame, impl.program_to_renderer.temporal_restart_frame());
  }
  result.assets = impl.assets;
  result.stage_assets = impl.stage_assets;

  const auto& program_geometry = impl.program_renderer.renderer_passes().geometry;
  std::string loading_detail = "Loading scene assets and preparing GPU resources.";
  if (program_geometry.models_failed != 0)
    loading_detail = "A model could not load. Check its source path, then Rescan in Library.";
  else if (program_geometry.models_pending || program_geometry.models_stale)
    loading_detail = "Loading models: " + std::to_string(program_geometry.models_pending + program_geometry.models_stale) + " pending.";
  else if (!program_geometry.baked_ready)
    loading_detail = program_geometry.baked_state == render::ProbeCacheState::Failed
        ? "The baked lighting file could not load. Check its path or rebake the scene."
        : "Loading baked lighting and uploading the probe field.";
  else if (program_geometry.texture_streaming.state != render::TextureStreamState::Ready)
    loading_detail = "Preparing material textures on the GPU.";
  else if (program_geometry.particles_unsupported != 0U)
    loading_detail = render::particle_runtime_built
        ? "Particle effects play only in the Advanced renderer. Choose it in Renderer settings."
        : "This build does not include particle effects. Remove the effect to continue.";
  else if (program_geometry.particles_failed != 0U)
    loading_detail = program_geometry.particle_issue.empty()
        ? "A particle effect could not load. Check its source path, then Rescan in Library."
        : "A particle effect could not load: " + program_geometry.particle_issue + ". Fix it, then Rescan in Library.";
  else if (program_geometry.particles_pending != 0U)
    loading_detail = "Loading particle effects: " + std::to_string(program_geometry.particles_pending) + " pending.";
  else if (program_geometry.particles_behind != 0U)
    loading_detail = "Particle effects are catching up with the playhead.";
  else if (impl.frame_media_cache.stats().pending_requests != 0)
    loading_detail = "Loading media: " + std::to_string(impl.frame_media_cache.stats().pending_requests) + " pending.";
  else if (program_geometry.assets.ready && !impl.assets.ready)
    loading_detail = "Waiting for GPU uploads to finish.";
  program_assets = program_asset_status(impl.assets,
      program_render_scene->asset_commit, program_render_scene->control_frame,
      program_render_scene->asset_cohort_required, output_monitor.cluster, loading_detail);
  if (program_assets.waiting) {
    if (impl.program_wait_started == std::chrono::steady_clock::time_point{})
      impl.program_wait_started = std::chrono::steady_clock::now();
  } else impl.program_wait_started = {};

  const auto look_generation=scene::asset_generation_key(authored_stage);
  if (!impl.look_observed || impl.look_document_digest != authored_stage.temporal_document_digest
      || impl.look_stage != authored_stage.id || impl.look_asset_generation != look_generation) {
    impl.look_observed=true; impl.look_document_digest=authored_stage.temporal_document_digest;
    impl.look_stage=authored_stage.id; impl.look_asset_generation=look_generation;
    impl.look_stale=authored_stage.publication_revision.empty() && render::baked_lighting_stale(authored_stage);
  }
  bake_activity = impl.look_stale
      ? "STALE LIGHTING: static inputs changed. Rebake GI/reflections or disable them before publishing." : impl.bake_message;
  if (impl.look_stale && !impl.bake_message.empty() && impl.bake_message != "Bake queued"
      && !impl.bake_message.starts_with("Bake complete"))
    bake_activity = "STALE LIGHTING: " + impl.bake_message + " Rebake before publishing.";
  if (impl.bake_job) {
    const auto& progress = impl.bake_job->progress();
    bake_activity = std::string{render::bake_stage_label(progress.stage.load())};
    if (progress.total.load() != 0)
      bake_activity += " " + std::to_string(progress.completed.load()) + "/" + std::to_string(progress.total.load());
  } else if (!impl.look_stale && !impl.completed_bake_content.empty() &&
             authored_stage.environment.baked.content == impl.completed_bake_content) {
    const bool on_program = scene.environment.baked.enabled &&
        scene.environment.baked.content == impl.completed_bake_content;
    bake_activity = !on_program ? "Bake complete. Ready in Stage."
        : program_assets.waiting ? "Bake complete. " + program_assets.headline + "."
        : "Bake complete. Lighting is live in Program.";
  }

  // Program's displayed pixels can be a crop, an asymmetric physical view, or
  // a projective UV warp. Give the interactive viewport the matching forward
  // projection so selection, direct manipulation, and outlines register with
  // the final connector image rather than with the authored camera canvas.
  std::optional<render::MappedProjection> program_mapped_projection;
  if (program_preview_ok) {
    const scene::Canvas output_canvas{program_extent.output_width,
                                      program_extent.output_height};
    ViewportProjection projection{};
    projection.canvas = output_canvas;
    projection.forward =
        scene::normalize(scene.camera.target - scene.camera.eye);
    projection.up = scene.camera.up;
    if (program_output == nullptr) {
      projection.project = [this](scene::Vec3 point) {
        return scene::project_to_canvas(scene.camera, scene.canvas, point);
      };
      program_view_projection.emplace(std::move(projection));
    } else if (program_output->projection ==
               output::FeedProjection::CanvasSlice) {
      const scene::CanvasRect source = program_output->source;
      projection.project = [this, source,
                            output_canvas](scene::Vec3 point) {
        scene::CanvasProjection mapped =
            scene::project_to_canvas(scene.camera, scene.canvas, point);
        mapped.x =
            (mapped.x - static_cast<float>(source.x)) *
            static_cast<float>(output_canvas.width) /
            static_cast<float>(source.width);
        mapped.y =
            (mapped.y - static_cast<float>(source.y)) *
            static_cast<float>(output_canvas.height) /
            static_cast<float>(source.height);
        return mapped;
      };
      program_view_projection.emplace(std::move(projection));
    } else {
      program_mapped_projection =
          impl.program_projection_cache.resolve(scene, *program_output);
      if (program_mapped_projection.has_value()) {
        render::MappedProjection *const mapped =
            &program_mapped_projection.value();
        projection.forward = mapped->render_view.forward;
        // In the world, after a camera-anchored stage has carried the surface.
        projection.up = mapped->surface_up;
        const output::ProjectionMesh *const mesh =
            mapped->projection_mesh();
        if (mesh != nullptr && mapped->mesh_view.has_value()) {
          const output::MeshPerspectiveView *const mesh_view =
              &mapped->mesh_view.value();
          const std::optional<output::ProjectionMeshPlacement> mesh_placement =
              mapped->mesh_placement;
          projection.project =
              [mesh, mesh_view, mesh_placement,
               output_canvas](scene::Vec3 point) {
                return mesh_placement.has_value()
                           ? output::project_through_mesh_warp(
                                 *mesh, *mesh_view, output_canvas, point,
                                 *mesh_placement)
                           : output::project_through_mesh_warp(
                                 *mesh, *mesh_view, output_canvas, point);
              };
        } else {
          const scene::ViewMatrices matrices =
              mapped->render_view.matrices;
          projection.project =
              [matrices, output_canvas](scene::Vec3 point) {
                return scene::project_to_canvas(matrices, output_canvas,
                                                point);
              };
        }
        program_view_projection.emplace(std::move(projection));
      }
    }
  }
}

} // namespace mediaserver::editor
