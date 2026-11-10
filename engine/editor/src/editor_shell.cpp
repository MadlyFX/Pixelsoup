#include <mediaserver/editor/editor_shell.hpp>

#include "shell/shell_frame.hpp"

namespace mediaserver::editor {

namespace {
using Result = core::Result<std::unique_ptr<EditorShell>, EditorError>;
} // namespace

bool editor_available() noexcept {
  return render::CompositorSceneExecutor::shaders_available() &&
         render::SceneMixer::shaders_available() &&
         render::ProjectionMeshWarp::shaders_available() &&
         render::OutputTransform::shaders_available() &&
         gui::UiRenderer::shaders_available() && rhi::Device::available();
}

RoutedSurfaceEdit route_surface_transform_edit(
    FeedSurfaceEditRequest request,
    std::span<const scene::ChannelKeyframes> keyframes, core::u64 show_frame,
    bool auto_key) {
  RoutedSurfaceEdit routed{};
  const FeedSurfaceTransform changed = request.transform_components;
  if (changed == FeedSurfaceTransform::None) {
    routed.base = std::move(request);
    return routed;
  }

  struct SurfaceComponent final {
    FeedSurfaceTransform bit{};
    scene::TransformChannel channel{};
  };
  constexpr std::array<SurfaceComponent, 4U> components{{
      {FeedSurfaceTransform::PositionX, scene::TransformChannel::PositionX},
      {FeedSurfaceTransform::PositionY, scene::TransformChannel::PositionY},
      {FeedSurfaceTransform::PositionZ, scene::TransformChannel::PositionZ},
      {FeedSurfaceTransform::Yaw, scene::TransformChannel::RotationYaw},
  }};

  routed.keys.reserve(components.size());
  FeedSurfaceTransform authored_base = FeedSurfaceTransform::None;
  for (const SurfaceComponent &component : components) {
    if (!has_transform(changed, component.bit)) {
      continue;
    }
    if (auto_key && keyed(keyframes, request.node, component.channel)) {
      routed.keys.push_back(KeyIntent{
          request.node, component.channel, show_frame,
          surface_transform_value(request.surface, component.channel), false,
          request.gesture});
    } else {
      authored_base |= component.bit;
    }
  }
  if (authored_base != FeedSurfaceTransform::None) {
    request.transform_components = authored_base;
    routed.base = std::move(request);
  }
  return routed;
}

EditorShell::EditorShell(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

EditorShell::~EditorShell() {
  // The window owns the device, but scene targets, UI caches and pipelines
  // are destroyed first. Finish their submissions before any owner releases
  // resources; the device's own destructor would be too late.
  if (impl_) impl_->window.device().wait_idle();
}

core::u32 EditorShell::model_thumbnail_room() const noexcept {
  const std::size_t queued = impl_->model_thumbnails.queue.size();
  return queued >= model_thumbnail_queue
             ? 0U
             : static_cast<core::u32>(model_thumbnail_queue - queued);
}

bool EditorShell::queue_model_thumbnail(const assets::DerivativeKey &key,
                                        std::string source) {
  if (model_thumbnail_room() == 0U || source.empty()) {
    return false;
  }
  impl_->model_thumbnails.queue.push_back(
      Impl::ModelThumbnailWork::Job{key, std::move(source)});
  return true;
}

std::vector<RenderedModelThumbnail> EditorShell::take_model_thumbnails() {
  std::vector<RenderedModelThumbnail> taken;
  taken.swap(impl_->model_thumbnails.finished);
  return taken;
}

void EditorShell::reset_library_thumbnail_textures(
    core::u64 project_generation) noexcept {
  if (impl_->library_thumbnail_textures.project_generation() !=
      project_generation) {
    impl_->library_thumbnail_textures.reset(project_generation);
  }
}

const rhi::Texture *EditorShell::library_thumbnail_texture(
    const assets::DerivativeKey &key) noexcept {
  return impl_->library_thumbnail_textures.find(key);
}

bool EditorShell::upload_library_thumbnail(
    const assets::DerivativeKey &key,
    std::span<const core::u8> encoded_png) noexcept {
  if (impl_->library_thumbnail_textures.find(key) != nullptr) {
    return true;
  }
  try {
    const render::DecodedFrame decoded =
        render::decode_image_bytes(encoded_png);
    if (!decoded.valid()) {
      return false;
    }
    rhi::Device &device = impl_->window.device();
    auto acquired = device.acquire_commands();
    if (!acquired) {
      return false;
    }
    rhi::CommandList commands = std::move(acquired).value();
    if (impl_->library_thumbnail_textures.upload(commands, key, decoded) ==
        nullptr) {
      return false;
    }
    auto submitted = device.submit(std::move(commands));
    if (!submitted || !std::move(submitted).value().wait()) {
      impl_->library_thumbnail_textures.clear();
      return false;
    }
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

void EditorShell::retain_library_thumbnail_textures(
    std::span<const assets::DerivativeKey> keys) noexcept {
  impl_->library_thumbnail_textures.retain(keys);
}

void EditorShell::reset_timeline_filmstrip_textures(
    core::u64 project_generation) noexcept {
  if (impl_->timeline_filmstrip_textures.project_generation() !=
      project_generation) {
    impl_->timeline_filmstrip_textures.reset(project_generation);
  }
}

const rhi::Texture *EditorShell::timeline_filmstrip_texture(
    const assets::DerivativeKey &key) noexcept {
  return impl_->timeline_filmstrip_textures.find(key);
}

bool EditorShell::upload_timeline_filmstrip(
    const assets::DerivativeKey &key,
    std::span<const core::u8> encoded_png) noexcept {
  if (impl_->timeline_filmstrip_textures.find(key) != nullptr) {
    return true;
  }
  try {
    const render::DecodedFrame decoded =
        render::decode_image_bytes(encoded_png);
    if (!decoded.valid()) {
      return false;
    }
    rhi::Device &device = impl_->window.device();
    auto acquired = device.acquire_commands();
    if (!acquired) {
      return false;
    }
    rhi::CommandList commands = std::move(acquired).value();
    if (impl_->timeline_filmstrip_textures.upload(commands, key, decoded) ==
        nullptr) {
      return false;
    }
    auto submitted = device.submit(std::move(commands));
    if (!submitted || !std::move(submitted).value().wait()) {
      impl_->timeline_filmstrip_textures.clear();
      return false;
    }
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

void EditorShell::retain_timeline_filmstrip_textures(
    std::span<const assets::DerivativeKey> keys) noexcept {
  impl_->timeline_filmstrip_textures.retain(keys);
}

void EditorShell::set_view_media_counts(ViewMediaCounts program,
                                        ViewMediaCounts stage) noexcept {
  impl_->program_panel.set_media_counts(program);
  for (auto &view : impl_->stage_views) {
    view.panel.set_media_counts(stage);
  }
}

bool EditorShell::apply_selection_set_recall(
    scene::SceneId wanted_scene, std::span<const doc::NodeId> targets,
    std::optional<doc::NodeId> primary) noexcept {
  Impl &impl = *impl_;
  if (wanted_scene == scene::SceneId{} ||
      wanted_scene != impl.displayed_scene) {
    return false;
  }
  try {
    NodeSelection recalled;
    for (const doc::NodeId target : targets) {
      if (target == doc::NodeId{} || recalled.contains(target)) {
        continue;
      }
      if (recalled.empty()) {
        recalled.replace(target);
      } else {
        recalled.toggle(target);
      }
    }
    if (primary.has_value()) {
      recalled.set_primary(*primary);
    }
    impl.scene_selection = std::move(recalled);
    impl.pending_group_members.clear();
    impl.pending_group_frames = 0U;
    impl.group_nodes.clear();
    impl.group_start.clear();
    impl.manipulating = false;
    impl.delete_domain = impl.scene_selection.empty()
                             ? DeleteDomain::Neutral
                             : DeleteDomain::Scene;
    impl.history_context = HistoryContext::Scene;
    impl.semantic_boundary = true;
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

bool EditorShell::apply_mask_selection(scene::SceneId wanted_scene,
                                       doc::NodeId target,
                                       doc::NodeId mask) noexcept {
  Impl &impl = *impl_;
  if (wanted_scene == scene::SceneId{} ||
      wanted_scene != impl.displayed_scene || mask == doc::NodeId{}) {
    return false;
  }
  impl.pending_mask_selection =
      MasksPanelSelection{wanted_scene, target, mask, std::nullopt};
  impl.pending_mask_frames = 240U;
  return true;
}

bool EditorShell::apply_key_edit_selection(
    scene::SceneId wanted_scene,
    std::span<const TimelineKeyAddress> selection) noexcept {
  Impl &impl = *impl_;
  if (wanted_scene == scene::SceneId{} ||
      wanted_scene != impl.displayed_scene) {
    return false;
  }
  try {
    impl.timeline_panel.replace_key_selection(selection);
    impl.history_context = HistoryContext::Scene;
    return true;
  } catch (const std::bad_alloc &) {
    return false;
  }
}

Result EditorShell::create(const EditorConfig &config) noexcept {
  if (!editor_available()) {
    return Result::failure(EditorError::Unavailable);
  }

  core::u32 restored_width = config.width;
  core::u32 restored_height = config.height;
  core::u16 restored_zoom = 100U;
  if (!config.workspace_layout.empty()) {
    const auto decoded = decode_workspace_layout(config.workspace_layout);
    if (decoded && decoded.value().window_width != 0U &&
        decoded.value().window_height != 0U) {
      restored_width = decoded.value().window_width;
      restored_height = decoded.value().window_height;
    }
    if (decoded) {
      restored_zoom = decoded.value().ui_zoom_percent;
    }
  }

  gui::WindowConfig window_config{};
  window_config.title = config.title;
  window_config.width = restored_width;
  window_config.height = restored_height;
  window_config.position_x = config.position_x;
  window_config.position_y = config.position_y;
  window_config.vsync = true;
  auto created_window = gui::UiWindow::create(window_config);
  if (!created_window) {
    return Result::failure(map_window_error(created_window.error()));
  }
  gui::UiWindow window = std::move(created_window).value();
  const rhi::TextureFormat working_format =
      window.device().capabilities().backend == rhi::BackendKind::D3d12
          ? rhi::TextureFormat::Rgba16Float
          : rhi::TextureFormat::Rgba8Unorm;

  window.set_zoom(static_cast<float>(restored_zoom) / 100.0F);
  gui::FontAtlas font = load_editor_font(window.ui_scale());
  if (!window.set_atlas(font)) {
    return Result::failure(EditorError::AllocationFailed);
  }

  auto program = render::CompositorSceneExecutor::create(
      window.device(), restored_width, restored_height,
      render::RendererLimits{}, render::MediaCacheConfig{}, working_format);
  if (!program) {
    return Result::failure(EditorError::Unavailable);
  }
  auto program_to = render::CompositorSceneExecutor::create(
      window.device(), restored_width, restored_height,
      render::RendererLimits{}, render::MediaCacheConfig{}, working_format);
  if (!program_to) {
    return Result::failure(EditorError::Unavailable);
  }
  auto program_warp = render::ProjectionMeshWarp::create(
      window.device(), restored_width, restored_height,
      render::ProjectionMeshWarpLimits{}, working_format);
  if (!program_warp) {
    return Result::failure(EditorError::Unavailable);
  }
  auto program_to_warp = render::ProjectionMeshWarp::create(
      window.device(), restored_width, restored_height,
      render::ProjectionMeshWarpLimits{}, working_format);
  if (!program_to_warp) {
    return Result::failure(EditorError::Unavailable);
  }
  auto mixer =
      render::SceneMixer::create(window.device(), restored_width,
                                 restored_height, working_format);
  if (!mixer) {
    return Result::failure(EditorError::Unavailable);
  }
  auto stage = render::CompositorSceneExecutor::create(
      window.device(), restored_width, restored_height,
      stage_view_limits(), render::MediaCacheConfig{}, working_format);
  if (!stage) {
    return Result::failure(EditorError::Unavailable);
  }
  auto program_display = render::OutputTransform::create(
      window.device(), restored_width, restored_height);
  if (!program_display) {
    return Result::failure(EditorError::Unavailable);
  }
  auto stage_display = render::OutputTransform::create(
      window.device(), restored_width, restored_height);
  if (!stage_display) {
    return Result::failure(EditorError::Unavailable);
  }

  auto *raw = new (std::nothrow)
      Impl{std::move(window),          std::move(font),
           std::move(program).value(), std::move(program_to).value(),
           std::move(program_warp).value(),
           std::move(program_to_warp).value(), std::move(mixer).value(),
           std::move(program_display).value(), std::move(stage).value(),
           std::move(stage_display).value(), config.workspace_layout};
  if (raw == nullptr) {
    return Result::failure(EditorError::AllocationFailed);
  }
  std::unique_ptr<Impl> impl{raw};
  // The Hardware Map draws its devices from a sprite sheet on this device.
  // Without it the map still works, with plain boxes for devices.
  if (auto acquired = impl->window.device().acquire_commands()) {
    rhi::CommandList commands = std::move(acquired).value();
    std::optional<rhi::Texture> sheet =
        create_hardware_sprite_texture(impl->window.device(), commands);
    auto submitted = impl->window.device().submit(std::move(commands));
    if (sheet.has_value() && submitted && std::move(submitted).value().wait()) {
      impl->hardware_sprites = std::move(sheet);
      impl->hardware_mapping_panel.set_sprites(&*impl->hardware_sprites);
    }
  }
  auto *shell = new (std::nothrow) EditorShell{std::move(impl)};
  if (shell == nullptr) {
    return Result::failure(EditorError::AllocationFailed);
  }
  return Result::success(std::unique_ptr<EditorShell>{shell});
}

EditorFrame
EditorShell::frame(const scene::Scene &scene, const EditorTimeline &timeline,
                   const std::vector<scene::ChannelKeyframes> &keyframes,
                   const std::vector<ElementInfo> &elements,
                   const std::vector<scene::EffectInstance> &effects,
                   const topology::HardwareMap &hardware_map,
                   const output::FeedMap &feed_map,
                   const EditorSwitcherState &switcher,
                   const assets::AssetRegistry &asset_registry,
                   std::span<const LibraryThumbnailObservation> thumbnails,
                   const compositor::CompositorGraph &compositor_graph,
                   const EditorProjectState &project,
                   const OutputMonitorState &output_monitor,
                   const topology::HardwareRuntimeSnapshot &hardware_runtime,
                   const sync::SyncPolicy &sync_policy,
                   const EditorPreferencesState &preferences,
                   const ProjectSettingsState &project_settings,
                   std::span<const LibraryProxyRuntimeObservation>
                       proxy_runtime,
                   std::span<const scene::TimelineLane>
                       timeline_lanes,
                   std::optional<core::i64> clip_clipboard_span,
                   std::span<const scene::SelectionSetSummary>
                       selection_sets,
                   std::optional<TimelineKeyScalePreview>
                       key_scale_preview,
                   std::span<const TimelineFilmstripObservation>
                       filmstrips,
                   const LiveDataState &live_data,
                   std::span<const scene::TrackingSource>
                       tracking_sources,
                   std::span<const scene::TimelineMarker>
                       timeline_markers,
                   const AssetViewerInput &asset_viewer,
                   const HistoryState &history_state) noexcept {
  Frame frame{*this, *impl_,
             scene, timeline, keyframes, elements, effects, hardware_map,
             feed_map, switcher, asset_registry, thumbnails, compositor_graph,
             project, output_monitor, hardware_runtime, sync_policy,
             preferences, project_settings, proxy_runtime, timeline_lanes,
             clip_clipboard_span, selection_sets, key_scale_preview,
             filmstrips, live_data, tracking_sources, timeline_markers,
             asset_viewer, history_state};
  if (frame.start()) {
    frame.prepare_views();
    frame.render_views();
    frame.run_header();
    frame.adopt_model_ingests();
    frame.prepare_panels();
    frame.dock_panels();
    frame.apply_drops();
    frame.finish();
  }
  return std::move(frame.result);
}

} // namespace mediaserver::editor
