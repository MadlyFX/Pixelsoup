#include "shell_support.hpp"

#include "native_dialogs.hpp"

namespace mediaserver::editor::shell_detail {

[[nodiscard]] std::filesystem::path native_path_from_utf8(
    std::string_view text) {
  std::u8string encoded;
  encoded.reserve(text.size());
  for (const char byte : text) {
    encoded.push_back(
        static_cast<char8_t>(static_cast<unsigned char>(byte)));
  }
  return std::filesystem::path{encoded};
}

[[nodiscard]] std::string
native_path_to_utf8(const std::filesystem::path &path) {
  const std::u8string encoded = path.u8string();
  std::string text;
  text.reserve(encoded.size());
  for (const char8_t byte : encoded) {
    text.push_back(std::bit_cast<char>(byte));
  }
  return text;
}

[[nodiscard]] std::vector<std::string>
selected_dialog_paths(std::string_view selection) {
  std::vector<std::string> paths;
  std::size_t begin = 0U;
  while (begin <= selection.size()) {
    const std::size_t separator = selection.find('|', begin);
    const std::size_t end = separator == std::string_view::npos
                                ? selection.size()
                                : separator;
    if (end > begin) {
      paths.emplace_back(selection.substr(begin, end - begin));
    }
    if (separator == std::string_view::npos) {
      break;
    }
    begin = separator + 1U;
  }
  return paths;
}

[[nodiscard]] std::optional<assets::MediaFrameRate>
asset_frame_rate(const std::optional<render::MediaFrameRate> &probed) {
  if (!probed.has_value()) {
    return std::nullopt;
  }
  return assets::MediaFrameRate{probed->numerator, probed->denominator};
}

[[nodiscard]] gui::UiColor with_alpha(gui::UiColor color, float alpha) noexcept {
  color.a = alpha;
  return color;
}

[[nodiscard]] gui::UiColor scale_color(gui::UiColor color, float amount) noexcept {
  color.r *= amount;
  color.g *= amount;
  color.b *= amount;
  return color;
}

[[nodiscard]] std::optional<gui::UiPoint>
draw_eyedropper_overlay(gui::UiContext &ui, gui::UiId id,
                        const gui::Rect &image,
                        bool pointer_available) {
  if (image.width <= 0.0F || image.height <= 0.0F) {
    return std::nullopt;
  }
  const gui::Interaction interaction =
      ui.grab(id, pointer_available ? image : gui::Rect{});
  const bool pointer_inside =
      pointer_available && image.contains(ui.pointer_x(), ui.pointer_y());
  if (pointer_inside) {
    constexpr float arm = 11.0F;
    constexpr float gap = 3.0F;
    gui::UiDrawList &draw = ui.draw_list();
    const gui::UiColor shadow{0.0F, 0.0F, 0.0F, 0.9F};
    const gui::UiColor mark{1.0F, 1.0F, 1.0F, 1.0F};
    const float x = ui.pointer_x();
    const float y = ui.pointer_y();
    draw.add_line(x - arm - 1.0F, y, x - gap - 1.0F, y, 3.0F, shadow);
    draw.add_line(x + gap + 1.0F, y, x + arm + 1.0F, y, 3.0F, shadow);
    draw.add_line(x, y - arm - 1.0F, x, y - gap - 1.0F, 3.0F, shadow);
    draw.add_line(x, y + gap + 1.0F, x, y + arm + 1.0F, 3.0F, shadow);
    draw.add_line(x - arm, y, x - gap, y, 1.0F, mark);
    draw.add_line(x + gap, y, x + arm, y, 1.0F, mark);
    draw.add_line(x, y - arm, x, y - gap, 1.0F, mark);
    draw.add_line(x, y + gap, x, y + arm, 1.0F, mark);
  }

  const float notice_width =
      std::min(std::max(image.width - 16.0F, 0.0F), 330.0F);
  const gui::Rect notice{
      image.x + (image.width - notice_width) * 0.5F, image.y + 9.0F,
      notice_width, 24.0F};
  ui.panel(notice, ui.theme().background);
  ui.outline(notice, ui.theme().accent);
  ui.label(gui::Rect{notice.x + 8.0F, notice.y,
                     std::max(notice.width - 16.0F, 0.0F), notice.height},
           "EYEDROPPER - CLICK A PIXEL  |  ESC CANCEL",
           ui.theme().text);
  if (!interaction.clicked) {
    return std::nullopt;
  }
  return gui::UiPoint{ui.pointer_x(), ui.pointer_y()};
}

// A key colour has to come from the plate, not from the already-keyed result.
// While the eyedropper is armed, bypass Chroma Key operations on its target in
// the editor-only scene copies. Program remains untouched on the real output;
// only the Stage/Program preview textures rendered by this shell use the copy.
void bypass_sampled_chroma_key(
    scene::Scene &view,
    const std::optional<InspectorColorSampleRequest> &request) {
  if (!request.has_value() ||
      request->properties[0U] != scene::schema::chroma_key_color_r) {
    return;
  }
  const auto selected = std::find_if(
      view.effects.begin(), view.effects.end(), [&](const auto &effect) {
        return effect.source == request->node &&
               effect.kind == scene::EffectKind::ChromaKey;
      });
  if (selected == view.effects.end()) {
    return;
  }
  const doc::NodeId target = selected->target;
  view.effects.erase(
      std::remove_if(view.effects.begin(), view.effects.end(),
                     [target](const auto &effect) {
                       return effect.target == target &&
                              effect.kind == scene::EffectKind::ChromaKey;
                     }),
      view.effects.end());
  const auto strip = [](scene::LayerStyle &style) {
    auto &operations = style.effects.spatial_operations;
    operations.erase(
        std::remove_if(operations.begin(), operations.end(),
                       [](const auto &operation) {
                         return operation.kind ==
                                scene::EffectKind::ChromaKey;
                       }),
        operations.end());
  };
  for (scene::MeshInstance &instance : view.instances) {
    if (instance.source == target) {
      strip(instance.style);
    }
  }
  for (scene::MediaInstance &media : view.media) {
    if (media.source_node == target || media.timeline_lane == target) {
      strip(media.style);
    }
  }
  for (scene::ModelInstance &model : view.models) {
    if (model.source_node == target) {
      strip(model.style);
    }
  }
}

[[nodiscard]] std::string_view
effect_target_name(const ElementInfo *target,
                   std::array<char, 64U> &storage) noexcept {
  if (target == nullptr) {
    return {};
  }
  if (!target->display_name.empty()) {
    return target->display_name;
  }
  const int length = std::snprintf(
      storage.data(), storage.size(), "%.*s %llu",
      static_cast<int>(kind_name(*target).size()),
      kind_name(*target).data(),
      static_cast<unsigned long long>(target->node.value()));
  return std::string_view{
      storage.data(),
      static_cast<std::size_t>(std::clamp(
          length, 0, static_cast<int>(storage.size() - 1U)))};
}

[[nodiscard]] render::RendererLimits stage_view_limits() noexcept {
  render::RendererLimits limits{};
  limits.view_shading = true;
  return limits;
}

[[nodiscard]] const rhi::Texture *
record_editor_scene(render::CompositorSceneExecutor &executor,
                     rhi::CommandList &commands, const scene::Scene &frame,
                     const compositor::CompositorGraph &graph,
                     render::MediaCache *shared_media_cache,
                     const render::SceneRenderView *view) noexcept {
  const scene::CanvasRect slice = scene::whole(frame.canvas);
  if (!graph.valid() || graph.plan.steps.empty()) {
    auto rendered =
        view != nullptr
            ? executor.execute(commands, frame, *view, shared_media_cache)
            : executor.execute(commands, frame, slice, shared_media_cache);
    return rendered ? rendered.value() : nullptr;
  }
  const auto planned = render::plan_compositor_scene(graph, frame);
  if (!planned) {
    // Compositor graphs are project-level while the switcher renders one
    // scoped scene at a time. A graph containing only another scene's
    // layers must not blank this viewport.
    if (planned.error() !=
        render::CompositorScenePlanError::MissingSceneLayer) {
      return nullptr;
    }
    auto rendered =
        view != nullptr
            ? executor.execute(commands, frame, *view, shared_media_cache)
            : executor.execute(commands, frame, slice, shared_media_cache);
    return rendered ? rendered.value() : nullptr;
  }
  auto rendered =
      view != nullptr
          ? executor.execute(commands, graph, frame, *view, shared_media_cache)
          : executor.execute(commands, graph, frame, slice, shared_media_cache);
  return rendered ? rendered.value() : nullptr;
}

[[nodiscard]] const output::OutputMap *selected_program_output(
    const output::FeedMap &feed_map,
    std::optional<doc::NodeId> &selected_map_node) noexcept {
  if (selected_map_node.has_value()) {
    if (const output::OutputMap *const selected =
            feed_map.map_node(*selected_map_node);
        selected != nullptr) {
      return selected;
    }
    selected_map_node.reset();
  }
  if (const output::OutputMap *const node_zero = feed_map.map_for(0U);
      node_zero != nullptr) {
    selected_map_node = node_zero->node;
    return node_zero;
  }
  if (!feed_map.maps.empty()) {
    selected_map_node = feed_map.maps.front().node;
    return &feed_map.maps.front();
  }
  return nullptr;
}

[[nodiscard]] ProgramOutputExtent program_output_extent(
    const scene::Scene &frame, const output::OutputMap *map,
    std::span<const output::OutputMap> window,
    const topology::HardwareMap &hardware_map) noexcept {
  core::u32 width = map != nullptr ? map->source.width : frame.canvas.width;
  core::u32 height = map != nullptr ? map->source.height : frame.canvas.height;

  if (map != nullptr) {
    if (const scene::Canvas laid_out = output::viewport_extent(window);
        laid_out.width > 0U && laid_out.height > 0U) {
      width = laid_out.width;
      height = laid_out.height;
    }
    const topology::RenderServerLookup owner =
        topology::render_server_for_node(hardware_map, map->node_index);
    const topology::HardwareDevice *const match =
        owner.duplicate ? nullptr : owner.device;
    if (match != nullptr &&
        match->window_mode != topology::HardwareWindowMode::Inherit &&
        match->width > 0U && match->height > 0U &&
        match->width <= topology::max_hardware_output_width &&
        match->height <= topology::max_hardware_output_height) {
      width = match->width;
      height = match->height;
    }
  }

  width = std::max(width, 1U);
  height = std::max(height, 1U);
  constexpr core::u32 max_preview_axis = 1'920U;
  const core::u32 longest = std::max(width, height);
  if (longest <= max_preview_axis) {
    return ProgramOutputExtent{width, height, width, height};
  }
  const core::u32 preview_width = std::max(
      1U, static_cast<core::u32>(static_cast<core::u64>(width) *
                                 max_preview_axis / longest));
  const core::u32 preview_height = std::max(
      1U, static_cast<core::u32>(static_cast<core::u64>(height) *
                                 max_preview_axis / longest));
  return ProgramOutputExtent{width, height, preview_width, preview_height};
}

[[nodiscard]] std::string_view
program_projection_name(const output::OutputMap *map) noexcept {
  if (map == nullptr) {
    return "LOCAL CANVAS - UNMAPPED";
  }
  if (map->projection == output::FeedProjection::CanvasSlice) {
    return "CANVAS CROP";
  }
  if (!map->resolved_surface.has_value()) {
    return "MAPPING UNAVAILABLE";
  }
  switch (map->resolved_surface->shape) {
  case output::ProjectionSurfaceShape::Planar:
    return "PHYSICAL FLAT";
  case output::ProjectionSurfaceShape::HorizontalArc:
    return "PHYSICAL ARC";
  case output::ProjectionSurfaceShape::ImportedMesh:
    return "UV MESH";
  }
  return "MAPPING UNAVAILABLE";
}

[[nodiscard]] std::optional<std::string> choose_project_to_open() {
  const char *const filters[]{"*.mstage"};
  const char *const path = tinyfd_openFileDialog(
      "Open Media Stage Project", "", 1, filters, "Media Stage projects", 0);
  if (path == nullptr) {
    return std::nullopt;
  }
  return std::string{path};
}

[[nodiscard]] std::optional<std::string> choose_projection_mesh() {
  const char* const filters[]{"*.gltf", "*.glb", "*.obj", "*.fbx", "*.msmodel"};
  const char* const path = tinyfd_openFileDialog("Choose UV Projection Mesh", "",
      static_cast<int>(std::size(filters)), filters,
      "UV projection meshes (glTF, GLB, OBJ, FBX, model package)", 0);
  return path ? std::optional<std::string>{path} : std::nullopt;
}

[[nodiscard]] std::optional<std::string> choose_property_file(const FilePickerRequest& request) {
  const char* path = nullptr;
  switch (request.kind) {
  case FilePickerKind::IesProfile: {
    const char* filters[]{"*.ies"};
    path = tinyfd_openFileDialog("Choose IES Profile", request.current.c_str(), 1, filters, "IES light profiles", 0);
    break;
  }
  case FilePickerKind::ShowLut: {
    const char* filters[]{"*.cube"};
    path = tinyfd_openFileDialog("Choose Show LUT", request.current.c_str(), 1, filters, "Cube color lookup tables", 0);
    break;
  }
  case FilePickerKind::EnvironmentMap: {
    const char* filters[]{"*.hdr", "*.exr"};
    path = tinyfd_openFileDialog("Choose Environment Map", request.current.c_str(), 2, filters, "HDR environment maps", 0);
    break;
  }
  case FilePickerKind::BakeFolder:
    path = tinyfd_selectFolderDialog("Choose Bake Folder", request.current.c_str());
    break;
  case FilePickerKind::ModelTexture: {
    const char* filters[]{"*.png", "*.jpg", "*.jpeg", "*.tga", "*.bmp"};
    path = tinyfd_openFileDialog("Assign Material Texture", request.current.c_str(), 5, filters,
        "Texture images (PNG, JPEG, TGA, BMP)", 0);
    break;
  }
  case FilePickerKind::GeneratorTexture: {
    const char* filters[]{"*.png", "*.jpg", "*.jpeg", "*.tga", "*.bmp"};
    path = tinyfd_openFileDialog("Choose Generator Texture", request.current.c_str(), 5, filters,
        "Texture images (PNG, JPEG, TGA, BMP)", 0);
    break;
  }
  }
  return path != nullptr ? std::optional<std::string>{path} : std::nullopt;
}

[[nodiscard]] std::string projection_mesh_name(std::string_view source) {
  const std::filesystem::path path{source};
  const std::string stem = path.stem().string();
  if (!stem.empty()) {
    return stem;
  }
  const std::string filename = path.filename().string();
  return filename.empty() ? std::string{"UV Mesh"} : filename;
}

[[nodiscard]] std::optional<std::string>
choose_project_to_save(std::string_view current_path) {
  const char *const filters[]{"*.mstage"};
  const std::string suggested = current_path.empty()
                                    ? std::string{"Untitled.mstage"}
                                    : std::string{current_path};
  const char *const path =
      tinyfd_saveFileDialog("Save Media Stage Project", suggested.c_str(), 1,
                            filters, "Media Stage projects");
  if (path == nullptr) {
    return std::nullopt;
  }
  std::filesystem::path normalized = native_path_from_utf8(path);
  if (normalized.extension().empty()) {
    normalized.replace_extension(".mstage");
  }
  return native_path_to_utf8(normalized);
}

[[nodiscard]] core::u16 next_ui_zoom(core::u16 current, bool larger) noexcept {
  if (larger) {
    for (const core::u16 step : ui_zoom_steps) {
      if (step > current) {
        return step;
      }
    }
    return ui_zoom_steps.back();
  }
  for (auto step = ui_zoom_steps.rbegin(); step != ui_zoom_steps.rend();
       ++step) {
    if (*step < current) {
      return *step;
    }
  }
  return ui_zoom_steps.front();
}

// The Compositor node an Effects-panel effect becomes when it is dropped on
// the graph, or nothing for an effect the graph has no node for.
[[nodiscard]] std::optional<compositor::NodeKind>
graph_effect_node(scene::EffectKind effect) noexcept {
  switch (effect) {
  case scene::EffectKind::Brightness:
    return compositor::NodeKind::Brightness;
  case scene::EffectKind::Saturation:
    return compositor::NodeKind::Saturation;
  case scene::EffectKind::Blur:
    return compositor::NodeKind::Blur;
  case scene::EffectKind::BoxBlur:
    return compositor::NodeKind::BoxBlur;
  case scene::EffectKind::DirectionalBlur:
    return compositor::NodeKind::DirectionalBlur;
  case scene::EffectKind::ChromaKey:
    return compositor::NodeKind::ChromaKey;
  case scene::EffectKind::LumaKey:
    return compositor::NodeKind::LumaKey;
  case scene::EffectKind::Levels:
    return compositor::NodeKind::Levels;
  case scene::EffectKind::LiftGammaGain:
    return compositor::NodeKind::LiftGammaGain;
  case scene::EffectKind::ContrastPivot:
    return compositor::NodeKind::ContrastPivot;
  case scene::EffectKind::ColorMatrix:
    return compositor::NodeKind::ColorMatrix;
  case scene::EffectKind::WhiteBalance:
    return compositor::NodeKind::WhiteBalance;
  case scene::EffectKind::Curves:
    return compositor::NodeKind::Curves;
  case scene::EffectKind::FilmGrain:
    return compositor::NodeKind::FilmGrain;
  case scene::EffectKind::Count:
    break;
  }
  return std::nullopt;
}

[[nodiscard]] gui::FontAtlas load_editor_font(float scale) {
  // The first entries cover the supported desktop platforms; the empty atlas
  // keeps the editor operational (without glyphs) on an unusual install.
  constexpr std::array<const char *, 9U> candidates{
      "C:/Windows/Fonts/segoeui.ttf",
      "C:/Windows/Fonts/arial.ttf",
      "/System/Library/Fonts/SFNS.ttf",
      "/System/Library/Fonts/Supplemental/Arial.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/TTF/LiberationSans-Regular.ttf",
      "/usr/local/share/fonts/DejaVuSans.ttf"};
  // Extra sizes for the Compositor graph, whose text follows its zoom. A
  // size past what a glyph page can hold at a dense display is left out; the
  // graph then draws that text at the largest size that was baked.
  constexpr std::array<float, 9U> graph_zoom_sizes{
      0.5F, 0.625F, 0.75F, 0.875F, 1.25F, 1.5F, 1.75F, 2.0F, 2.5F};
  constexpr float largest_glyph_px = 72.0F;
  std::vector<float> sizes;
  for (const float size : graph_zoom_sizes) {
    if (static_cast<float>(editor_font_size) * scale * size <= largest_glyph_px) {
      sizes.push_back(size);
    }
  }
  for (const char *path : candidates) {
    auto atlas = gui::FontAtlas::create(path, editor_font_size, scale, sizes);
    if (atlas.has_value()) {
      return std::move(atlas).value();
    }
  }
  return gui::FontAtlas::empty();
}

[[nodiscard]] EditorError map_window_error(gui::WindowError error) noexcept {
  switch (error) {
  case gui::WindowError::DisplayUnavailable:
    return EditorError::DisplayUnavailable;
  case gui::WindowError::AllocationFailed:
    return EditorError::AllocationFailed;
  case gui::WindowError::Unavailable:
  case gui::WindowError::BackendFailure:
    break;
  }
  return EditorError::Unavailable;
}

[[nodiscard]] const ElementInfo *
find_element(const std::vector<ElementInfo> &elements,
             doc::NodeId node) noexcept {
  for (const ElementInfo &element : elements) {
    if (element.node == node) {
      return &element;
    }
  }
  return nullptr;
}

// `length` along world axis `column` (0 = X).
[[nodiscard]] scene::Vec3 with_axis(core::u32 column, float length) noexcept {
  return scene::Vec3{column == 0U ? length : 0.0F,
                     column == 1U ? length : 0.0F,
                     column == 2U ? length : 0.0F};
}

// A Blender-linked object (not its collection): placed by one world matrix.
[[nodiscard]] bool linked_occurrence(const ElementInfo *element) noexcept {
  return element != nullptr && element->linked && !element->linked_collection;
}

[[nodiscard]] bool keyed(
    std::span<const scene::ChannelKeyframes> keyframes, doc::NodeId node,
    scene::TransformChannel component) noexcept {
  for (const scene::ChannelKeyframes &entry : keyframes) {
    if (entry.target == node && entry.component == component &&
        !entry.frames.empty()) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] double surface_transform_value(
    const output::PhysicalSurface &surface,
    scene::TransformChannel component) noexcept {
  switch (component) {
  case scene::TransformChannel::PositionX:
    return surface.center.x;
  case scene::TransformChannel::PositionY:
    return surface.center.y;
  case scene::TransformChannel::PositionZ:
    return surface.center.z;
  case scene::TransformChannel::RotationYaw: {
    constexpr double tau = 6.28318530717958647692;
    return std::atan2(-static_cast<double>(surface.right.z),
                      static_cast<double>(surface.right.x)) /
           tau;
  }
  default:
    return 0.0;
  }
}

[[nodiscard]] std::optional<MaskViewportSourceMapping>
mask_source_mapping(const scene::Scene &view, doc::NodeId target,
                    const ViewportProjection *projection) {
  const scene::Mat4 *world{};
  scene::Primitive primitive{scene::Primitive::Quad};
  for (const scene::MediaInstance &media : view.media) {
    if (media.source_node == target) {
      world = &media.world;
      primitive = media.mesh.primitive;
      break;
    }
  }
  if (world == nullptr) {
    for (const scene::MeshInstance &instance : view.instances) {
      if (instance.source == target) {
        world = &instance.world;
        primitive = instance.mesh.primitive;
        break;
      }
    }
  }
  // UVs on Quad and Plane define one unambiguous source plane. Volumes and
  // imported models have multiple/arbitrary UV islands, so presenting one
  // guessed quad would author in the wrong coordinate system; Stage reports
  // mapping unavailable instead.
  if (world == nullptr) {
    return std::nullopt;
  }
  const auto plane = scene::mask_source_plane(primitive);
  if (!plane.has_value()) {
    return std::nullopt;
  }
  const std::array<scene::Vec3, 4U> &local = *plane;
  // The layer's four corners are projected with whatever projector the view
  // showing them uses, so the quad the mask resolves inside is the quad on
  // screen. On Stage that is the authored camera; on Program Output it is the
  // crop or warp that produced the connector image. Everything downstream is
  // canvas-space, so the two views need nothing else to agree.
  MaskViewportSourceMapping mapping{};
  for (std::size_t index = 0U; index < local.size(); ++index) {
    const scene::Vec3 corner =
        scene::transform_point(*world, local[index]);
    const scene::CanvasProjection projected =
        projection != nullptr && projection->project
            ? projection->project(corner)
            : scene::project_to_canvas(view.camera, view.canvas, corner);
    if (!projected.visible || !std::isfinite(projected.x) ||
        !std::isfinite(projected.y)) {
      return std::nullopt;
    }
    mapping.corners[index] =
        MaskViewportCanvasPoint{projected.x, projected.y};
  }
  // Measured as the renderer measures it for this scene, so a rotated or
  // feathered outline keeps the shape the rendered mask has.
  mapping.extent = scene::mask_source_extent(view.camera, view.canvas,
                                             *world, primitive);
  return mapping;
}

} // namespace mediaserver::editor::shell_detail
