#include <mediaserver/editor/editor_shell.hpp>

#include <mediaserver/editor/live_data_panel.hpp>

#include <mediaserver/editor/library_thumbnail_texture_cache.hpp>
#include <mediaserver/editor/timeline_filmstrip_texture_cache.hpp>

#include <mediaserver/editor/color_correction_panel.hpp>
#include <mediaserver/editor/effects_panel.hpp>
#include <mediaserver/editor/inspector_panel.hpp>
#include <mediaserver/editor/mapping_panels.hpp>
#include <mediaserver/editor/masks_panel.hpp>
#include <mediaserver/editor/selection.hpp>
#include <mediaserver/editor/selection_sets_panel.hpp>
#include <mediaserver/editor/stage_view.hpp>
#include <mediaserver/editor/texture_panel.hpp>
#include <mediaserver/editor/viewport_panel.hpp>
#include <mediaserver/editor/workspace_layout.hpp>
#include <mediaserver/editor/workspace_panels.hpp>
#include <mediaserver/gui/dock.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/gui/ui_renderer.hpp>
#include <mediaserver/gui/window.hpp>
#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/mapped_scene.hpp>
#include <mediaserver/render/media_source.hpp>
#include <mediaserver/render/model_loader.hpp>
#include <mediaserver/render/output_transform.hpp>
#include <mediaserver/render/projection_mesh_loader.hpp>
#include <mediaserver/render/projection_mesh_warp.hpp>
#include <mediaserver/render/scene_mixer.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/manipulate.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <future>
#include <mediaserver/render/model_ingest_queue.hpp>
#include <mediaserver/render/model_cache.hpp>
#include <mediaserver/render/baked_lighting.hpp>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

// The native file dialog is intentionally the only platform-flavoured helper
// left in the editor. Its header is external code, so keep it outside the
// project's warnings-as-errors surface.
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include <tinyfiledialogs.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace mediaserver::editor {
namespace {

namespace gui = mediaserver::gui;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;

using Result = core::Result<std::unique_ptr<EditorShell>, EditorError>;

constexpr float top_bar_height = 34.0F;
constexpr float status_bar_height = 28.0F;

// Docked panels sit on the window background with a six-unit gutter between
// them; every seam answers the pointer four units either side of that gap.
constexpr gui::DockMetrics editor_dock_metrics{
    .divider = 6.0F,
    .seam_reach = 4.0F,
    .tab_bar = 32.0F,
    .title_bar = 30.0F,
    .edge = 0.25F,
};

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

const gui::PanelId panel_stage = gui::ui_id("editor.panel.stage");
const gui::PanelId panel_program = gui::ui_id("editor.panel.program");
const gui::PanelId panel_inspector = gui::ui_id("editor.panel.inspector");
const gui::PanelId panel_inspector_overview =
    gui::ui_id("editor.panel.inspector-overview");
const gui::PanelId panel_timeline = gui::ui_id("editor.panel.timeline");
const gui::PanelId panel_nodes = gui::ui_id("editor.panel.nodes");
const gui::PanelId panel_cues = gui::ui_id("editor.panel.cues");
const gui::PanelId panel_automation = gui::ui_id("editor.panel.automation");
const gui::PanelId panel_scene = gui::ui_id("editor.panel.scene");
const gui::PanelId panel_selection_sets =
    gui::ui_id("editor.panel.selection-sets");
const gui::PanelId panel_masks = gui::ui_id("editor.panel.masks");
const gui::PanelId panel_library = gui::ui_id("editor.panel.library");
const gui::PanelId panel_effects = gui::ui_id("editor.panel.effects");
const gui::PanelId panel_color_correction =
    gui::ui_id("editor.panel.color-correction");
const gui::PanelId panel_texture = gui::ui_id("editor.panel.texture");
const gui::PanelId panel_macros = gui::ui_id("editor.panel.macros");
const gui::PanelId panel_compositor = gui::ui_id("editor.panel.compositor");
const gui::PanelId panel_output = gui::ui_id("editor.panel.output");
const gui::PanelId panel_hardware_mapping =
    gui::ui_id("editor.panel.hardware-mapping");
const gui::PanelId panel_feed_mapping = gui::ui_id("editor.panel.feed-mapping");
const gui::PanelId panel_live_data = gui::ui_id("editor.panel.live-data");

[[nodiscard]] gui::UiId mix(gui::UiId base, std::string_view name) noexcept {
  core::u64 hash = base ^ 0x9E3779B97F4A7C15ULL;
  for (const char c : name) {
    hash ^= static_cast<core::u8>(c);
    hash *= 1099511628211ULL;
  }
  return hash;
}

// The editor's palette. Surfaces step up in value from the window ground to
// panels to their headers, every panel is framed in `border`, and text sits
// well clear of all of them, so regions read apart in a dark room without any
// of them competing with the picture.
[[nodiscard]] constexpr gui::Theme editor_theme() noexcept {
  gui::Theme theme{};
  theme.background = gui::UiColor{0.031F, 0.039F, 0.051F, 1.0F};
  theme.panel = gui::UiColor{0.067F, 0.080F, 0.098F, 1.0F};
  theme.header = gui::UiColor{0.086F, 0.102F, 0.125F, 1.0F};
  theme.row = gui::UiColor{0.075F, 0.088F, 0.108F, 1.0F};
  theme.row_alt = gui::UiColor{0.067F, 0.080F, 0.098F, 1.0F};
  theme.widget = gui::UiColor{0.102F, 0.122F, 0.149F, 1.0F};
  theme.widget_hover = gui::UiColor{0.141F, 0.169F, 0.204F, 1.0F};
  theme.widget_active = gui::UiColor{0.000F, 0.345F, 0.565F, 1.0F};
  theme.accent = gui::UiColor{0.090F, 0.650F, 0.930F, 1.0F};
  theme.selection = gui::UiColor{0.039F, 0.255F, 0.420F, 1.0F};
  theme.track = gui::UiColor{0.039F, 0.047F, 0.059F, 1.0F};
  theme.text = gui::UiColor{0.850F, 0.880F, 0.910F, 1.0F};
  theme.text_muted = gui::UiColor{0.520F, 0.570F, 0.620F, 1.0F};
  theme.divider = gui::UiColor{0.118F, 0.137F, 0.165F, 1.0F};
  theme.border = gui::UiColor{0.165F, 0.192F, 0.231F, 1.0F};
  theme.playhead = gui::UiColor{0.090F, 0.650F, 0.930F, 1.0F};
  theme.keyframe = gui::UiColor{0.880F, 0.930F, 1.000F, 1.0F};
  theme.good = gui::UiColor{0.250F, 0.840F, 0.420F, 1.0F};
  theme.warning = gui::UiColor{0.960F, 0.400F, 0.320F, 1.0F};
  theme.padding = 5.0F;
  return theme;
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

void outline(gui::UiContext &ui, const gui::Rect &bounds, gui::UiColor color) {
  gui::UiDrawList &draw = ui.draw_list();
  draw.add_rect(bounds.x, bounds.y, bounds.width, 1.0F, color);
  draw.add_rect(bounds.x, bounds.y + std::max(bounds.height - 1.0F, 0.0F),
                bounds.width, 1.0F, color);
  draw.add_rect(bounds.x, bounds.y, 1.0F, bounds.height, color);
  draw.add_rect(bounds.x + std::max(bounds.width - 1.0F, 0.0F), bounds.y, 1.0F,
                bounds.height, color);
}

[[nodiscard]] std::optional<gui::UiPoint>
draw_eyedropper_overlay(gui::UiContext &ui, gui::UiId id,
                        const gui::Rect &image,
                        bool pointer_available = true) {
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
  outline(ui, notice, ui.theme().accent);
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

[[nodiscard]] const rhi::Texture *
record_editor_scene(render::CompositorSceneExecutor &executor,
                     rhi::CommandList &commands, const scene::Scene &frame,
                     const compositor::CompositorGraph &graph,
                     render::MediaCache *shared_media_cache,
                     const render::SceneRenderView *view = nullptr) noexcept {
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

struct ProgramOutputExtent final {
  core::u32 output_width{};
  core::u32 output_height{};
  core::u32 preview_width{};
  core::u32 preview_height{};
};

[[nodiscard]] ProgramOutputExtent program_output_extent(
    const scene::Scene &frame, const output::OutputMap *map,
    const topology::HardwareMap &hardware_map) noexcept {
  core::u32 width = map != nullptr ? map->source.width : frame.canvas.width;
  core::u32 height = map != nullptr ? map->source.height : frame.canvas.height;

  if (map != nullptr) {
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

[[nodiscard]] bool chrome_button(gui::UiContext &ui, gui::UiId id,
                                 const gui::Rect &bounds,
                                 std::string_view label, bool active = false,
                                 bool enabled = true) {
  const gui::Interaction it =
      enabled ? ui.grab(id, bounds) : gui::Interaction{};
  gui::UiColor body =
      !enabled ? ui.theme().track
               : (active ? ui.theme().widget_active : ui.theme().widget);
  if (enabled && it.held && !it.released) {
    body = ui.theme().widget_active;
  } else if (enabled && it.hovered && !active) {
    body = ui.theme().widget_hover;
  }
  ui.panel(bounds, body);
  outline(ui, bounds,
          !enabled ? ui.theme().divider
                   : (active ? ui.theme().accent : ui.theme().header));
  const float text_width = ui.font_width(label);
  ui.label(
      gui::Rect{bounds.x + std::max((bounds.width - text_width) * 0.5F, 2.0F),
                bounds.y, bounds.width, bounds.height},
      label,
      !enabled ? ui.theme().text_muted
               : (active ? gui::UiColor{0.920F, 0.970F, 1.000F, 1.0F}
                         : ui.theme().text));
  return enabled && it.clicked;
}

void draw_inline_remove(gui::UiContext &ui, const gui::Rect &bounds,
                        gui::UiColor color) {
  const float x = bounds.x + bounds.width * 0.5F;
  const float y = bounds.y + bounds.height * 0.5F;
  ui.draw_list().add_line(x - 3.0F, y - 3.0F, x + 3.0F, y + 3.0F, 1.0F,
                          color);
  ui.draw_list().add_line(x + 3.0F, y - 3.0F, x - 3.0F, y + 3.0F, 1.0F,
                          color);
}

void draw_brand_mark(gui::UiContext &ui, float x, float y) {
  auto &draw = ui.draw_list();
  constexpr gui::UiColor cyan{0.000F, 0.760F, 0.980F, 1.0F};
  constexpr gui::UiColor magenta{0.930F, 0.160F, 0.760F, 1.0F};
  draw.add_triangle(x, y + 2.0F, x + 15.0F, y + 8.0F, x, y + 16.0F, cyan);
  draw.add_triangle(x + 6.0F, y, x + 18.0F, y + 8.0F, x + 6.0F, y + 18.0F,
                    magenta);
  draw.add_triangle(x + 5.0F, y + 4.0F, x + 12.0F, y + 8.0F, x + 5.0F,
                    y + 13.0F, ui.theme().background);
}

struct HeaderActions final {
  bool program{};
  bool preview{};
  bool swap{};
  bool open_setup{};
};

enum class ApplicationMenu : core::u8 {
  None,
  File,
  Edit,
  View,
  Window,
};

enum class ApplicationMenuAction : core::u8 {
  None,
  NewProject,
  OpenProject,
  RecentProjects,
  OpenRecentProject,
  ImportLinkedScene,
  CancelLinkedSceneImport,
  StartSceneLink,
  SceneLinkDetails,
  StopSceneLink,
  SaveProject,
  SaveProjectAs,
  OpenPreferences,
  OpenProjectSettings,
  Undo,
  Redo,
  Copy,
  Paste,
  TogglePanel,
  ZoomIn,
  ZoomOut,
  ZoomReset,
  ResetWorkspace,
};

struct ApplicationMenuResult final {
  ApplicationMenuAction action{ApplicationMenuAction::None};
  gui::PanelId panel{};
  std::string project_path;
};

struct ApplicationMenuItem final {
  std::string_view label;
  ApplicationMenuAction action{ApplicationMenuAction::None};
};

constexpr std::array<ApplicationMenuItem, 9U> file_menu_items{{
    {"New Project", ApplicationMenuAction::NewProject},
    {"Open Project...", ApplicationMenuAction::OpenProject},
    {"Recent Projects", ApplicationMenuAction::RecentProjects},
    {"Connect to Blender...", ApplicationMenuAction::StartSceneLink},
    {"Publish Preview...", ApplicationMenuAction::SceneLinkDetails},
    {"Save Project", ApplicationMenuAction::SaveProject},
    {"Save Project As...", ApplicationMenuAction::SaveProjectAs},
    {"Project Settings...", ApplicationMenuAction::OpenProjectSettings},
    {"Preferences...", ApplicationMenuAction::OpenPreferences},
}};

constexpr float application_menu_row_height = 22.0F;
constexpr float application_menu_padding = 4.0F;
constexpr float file_menu_separator_height = 8.0F;

[[nodiscard]] std::string recent_project_label(std::string_view path) {
  const auto separator = path.find_last_of("/\\");
  if (separator == std::string_view::npos || separator + 1U == path.size()) {
    return std::string{path};
  }
  std::string label{path.substr(separator + 1U)};
  label += "  -  ";
  label += path.substr(0U, separator);
  return label;
}

struct MenuBarEntry final {
  std::string_view label;
  ApplicationMenu menu{ApplicationMenu::None};
  bool setup{};
};

// Only menus that do something. An entry that opens nothing teaches the
// operator to stop trusting the menu bar.
constexpr std::array<MenuBarEntry, 5U> menu_bar_entries{{
    {"File", ApplicationMenu::File, false},
    {"Edit", ApplicationMenu::Edit, false},
    {"View", ApplicationMenu::View, false},
    {"Setup", ApplicationMenu::None, true},
    {"Window", ApplicationMenu::Window, false},
}};

struct ViewMenuItem final {
  std::string_view label;
  std::string_view shortcut;
  ApplicationMenuAction action{ApplicationMenuAction::None};
};

constexpr std::array<ViewMenuItem, 4U> view_menu_items{{
    {"Zoom In", "Ctrl+=", ApplicationMenuAction::ZoomIn},
    {"Zoom Out", "Ctrl+-", ApplicationMenuAction::ZoomOut},
    {"Actual Size", "Ctrl+0", ApplicationMenuAction::ZoomReset},
    {"Reset Workspace", "", ApplicationMenuAction::ResetWorkspace},
}};

struct WindowPanelEntry final {
  gui::PanelId panel{};
  std::string_view title;
};

[[nodiscard]] const std::array<WindowPanelEntry, 21U> &window_panel_entries() {
  static const std::array<WindowPanelEntry, 21U> entries{{
      {panel_stage, "View"},
      {panel_program, "Program Output"},
      {panel_scene, "Scene"},
      {panel_selection_sets, "Selection Sets"},
      {panel_masks, "Masks"},
      {panel_library, "Library"},
      {panel_effects, "Effects"},
      {panel_color_correction, "Color Correction"},
      {panel_texture, "Texture"},
      {panel_macros, "Macros"},
      {panel_inspector_overview, "Inspector"},
      {panel_inspector, "Properties"},
      {panel_timeline, "Timeline"},
      {panel_nodes, "Nodes"},
      {panel_cues, "Cues"},
      {panel_automation, "Automation"},
      {panel_compositor, "Compositor"},
      {panel_output, "Output Monitor"},
      {panel_live_data, "Live Data"},
      {panel_hardware_mapping, "Hardware Map"},
      {panel_feed_mapping, "Feed Mapping"},
  }};
  return entries;
}

[[nodiscard]] gui::Rect menu_bar_entry_bounds(gui::UiContext &ui,
                                              std::string_view wanted) {
  float x = 158.0F;
  for (const MenuBarEntry &entry : menu_bar_entries) {
    const float width = ui.font_width(entry.label) + 18.0F;
    const gui::Rect bounds{x, 0.0F, width, top_bar_height};
    if (entry.label == wanted) {
      return bounds;
    }
    x += width;
  }
  return {};
}

[[nodiscard]] gui::Rect menu_popup_bounds(gui::UiContext &ui,
                                          ApplicationMenu menu) {
  if (menu == ApplicationMenu::File) {
    const gui::Rect anchor = menu_bar_entry_bounds(ui, "File");
    const float height =
        application_menu_padding * 2.0F +
        static_cast<float>(file_menu_items.size()) *
            application_menu_row_height +
        file_menu_separator_height * 2.0F;
    return gui::Rect{anchor.x, top_bar_height, 220.0F, height};
  }
  if (menu == ApplicationMenu::Edit) {
    const gui::Rect anchor = menu_bar_entry_bounds(ui, "Edit");
    return gui::Rect{anchor.x, top_bar_height, 190.0F, 104.0F};
  }
  if (menu == ApplicationMenu::View) {
    const gui::Rect anchor = menu_bar_entry_bounds(ui, "View");
    return gui::Rect{anchor.x, top_bar_height, 220.0F,
                     application_menu_padding * 2.0F +
                         static_cast<float>(view_menu_items.size()) *
                             application_menu_row_height};
  }
  if (menu == ApplicationMenu::Window) {
    const gui::Rect anchor = menu_bar_entry_bounds(ui, "Window");
    return gui::Rect{anchor.x, top_bar_height, 238.0F,
                     static_cast<float>(window_panel_entries().size()) * 21.0F +
                         8.0F};
  }
  return {};
}

[[nodiscard]] gui::Rect recent_projects_row(const gui::Rect &file_popup) {
  return gui::Rect{file_popup.x + application_menu_padding,
                   file_popup.y + application_menu_padding +
                       2.0F * application_menu_row_height,
                   file_popup.width - 2.0F * application_menu_padding,
                   application_menu_row_height};
}

[[nodiscard]] gui::Rect recent_projects_popup(const gui::Rect &file_popup,
                                              float viewport_width,
                                              std::size_t project_count) {
  constexpr float preferred_width = 420.0F;
  const float right_x = file_popup.x + file_popup.width;
  const float right_space = std::max(viewport_width - right_x, 0.0F);
  const bool opens_right = right_space >= file_popup.x;
  const float width = std::min(preferred_width,
                               opens_right ? right_space : file_popup.x);
  const float x = opens_right ? right_x : file_popup.x - width;
  return gui::Rect{x, recent_projects_row(file_popup).y, width,
                   2.0F * application_menu_padding +
                       application_menu_row_height *
                           static_cast<float>(std::max<std::size_t>(1U, project_count))};
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
  const char* const filters[]{"*.gltf", "*.glb", "*.obj", "*.msmodel"};
  const char* const path = tinyfd_openFileDialog("Choose UV Projection Mesh", "", 4,
      filters, "UV projection meshes (glTF, GLB, OBJ, model package)", 0);
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

[[nodiscard]] bool confirm_discard_project_changes(std::string_view action) {
  std::string message{
      "This project has unsaved changes.\n\nContinuing will discard "};
  message.append(action);
  message.append(". Continue?");
  return tinyfd_messageBox("Unsaved Project", message.c_str(), "yesno",
                           "warning", 0) != 0;
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

enum class DeleteDomain : core::u8 {
  Scene,
  Hardware,
  Feed,
  Surface,
  Compositor,
  Neutral,
};

[[nodiscard]] HeaderActions
draw_application_header(gui::UiContext &ui, float width,
                        const EditorSwitcherState &switcher,
                        ApplicationMenu &open_menu,
                        const EditorProjectState &project) {
  HeaderActions actions{};
  const gui::Rect top{0.0F, 0.0F, width, top_bar_height};
  ui.panel(top, ui.theme().background);
  ui.panel(gui::Rect{top.x, top.y + top.height - 1.0F, top.width, 1.0F},
           ui.theme().divider);

  draw_brand_mark(ui, 12.0F, 8.0F);
  ui.label(gui::Rect{38.0F, 0.0F, 116.0F, top_bar_height}, "MEDIA STAGE",
           ui.theme().text);

  float x = 158.0F;
  for (const MenuBarEntry &entry : menu_bar_entries) {
    const float menu_width = ui.font_width(entry.label) + 18.0F;
    const gui::Rect menu{x, 0.0F, menu_width, top_bar_height};
    const gui::Interaction it =
        ui.grab(mix(gui::ui_id("editor.menu"), entry.label), menu);
    const bool active =
        entry.menu != ApplicationMenu::None && open_menu == entry.menu;
    if (it.hovered || active) {
      ui.panel(menu, ui.theme().header);
    }
    ui.label(gui::Rect{menu.x + 9.0F, menu.y, menu.width - 9.0F, menu.height},
             entry.label,
             it.hovered || active ? ui.theme().text : ui.theme().text_muted);
    if (active) {
      ui.panel(gui::Rect{menu.x, menu.y + menu.height - 2.0F, menu.width, 2.0F},
               ui.theme().accent);
    }
    if (it.clicked) {
      if (entry.setup) {
        actions.open_setup = true;
        open_menu = ApplicationMenu::None;
      } else if (entry.menu != ApplicationMenu::None) {
        open_menu =
            open_menu == entry.menu ? ApplicationMenu::None : entry.menu;
      } else {
        open_menu = ApplicationMenu::None;
      }
    }
    x += menu_width;
  }

  // Program/Preview are mode selectors, not show-state jargon. Keep the whole
  // group an exact width so its labels stay stable when the window is resized.
  constexpr float modes_width = 246.0F;
  constexpr float modes_right_margin = 16.0F;
  const float modes_x =
      std::max(width - modes_width - modes_right_margin, x + 16.0F);
  // The open project, named in the middle of the bar the way every document
  // editor names its document, with a dot while it has unsaved changes.
  const float workspace_x = x + 12.0F;
  const float workspace_width = std::max(modes_x - workspace_x - 12.0F, 0.0F);
  if (workspace_width > 90.0F) {
    std::string name =
        project.path.empty()
            ? std::string{"Untitled"}
            : native_path_to_utf8(
                  native_path_from_utf8(project.path).stem());
    const float name_width = ui.font_width(name);
    const float chip_width =
        std::min(name_width + (project.dirty ? 34.0F : 22.0F), workspace_width);
    const gui::Rect chip{workspace_x + (workspace_width - chip_width) * 0.5F,
                         5.0F, chip_width, top_bar_height - 10.0F};
    ui.panel(chip, ui.theme().header);
    outline(ui, chip, ui.theme().border);
    ui.label(gui::Rect{chip.x + 11.0F, chip.y,
                       std::max(chip.width - (project.dirty ? 30.0F : 18.0F),
                                0.0F),
                       chip.height},
             name, ui.theme().text);
    if (project.dirty) {
      draw_status_dot(ui, chip.x + chip.width - 12.0F,
                      chip.y + chip.height * 0.5F, ui.theme().playhead, 3.0F);
      ui.tooltip(chip, "Unsaved changes");
    } else if (!project.path.empty()) {
      ui.tooltip(chip, project.path);
    }
  }

  float mode_x = modes_x;
  actions.program = chrome_button(
      ui, gui::ui_id("editor.mode.program"),
      gui::Rect{mode_x, 5.0F, 80.0F, 24.0F}, "Program",
      !switcher.rehearsal_mode, !switcher.rehearsal_locked);
  mode_x += 84.0F;
  const bool has_rehearsal = switcher.rehearsal_scene != scene::SceneId{};
  const bool canvas_mismatch = has_rehearsal && !switcher.dissolve_compatible;
  // Swap is held by the switcher's own preconditions - a Preview scene to
  // promote and no transition already running - and by nothing else except a
  // STRICT synchronization policy whose timing is unproven. A dissolve also
  // needs a matching canvas; the button says CUT rather than failing silently.
  const bool swap_enabled = !switcher.transition_active &&
                            !switcher.rehearsal_locked && has_rehearsal &&
                            !switcher.program_held && !switcher.publication_pending;
  actions.swap = chrome_button(
      ui, gui::ui_id("editor.mode.swap"), gui::Rect{mode_x, 5.0F, 74.0F, 24.0F},
      switcher.transition_active
          ? "FADING"
          : (switcher.program_held ? "HELD" : switcher.publication_pending ? "STAGING"
              : switcher.linked_publication ? "PUBLISH" : (canvas_mismatch ? "CUT" : "SWAP")),
      false, swap_enabled);
  mode_x += 78.0F;
  actions.preview = chrome_button(
      ui, gui::ui_id("editor.mode.preview"),
      gui::Rect{mode_x, 5.0F, 80.0F, 24.0F}, "Preview",
      switcher.rehearsal_mode, !switcher.rehearsal_locked);
  return actions;
}

[[nodiscard]] ApplicationMenuResult
run_application_menu_input(gui::UiContext &ui, float width, float height,
                           ApplicationMenu &open_menu,
                           bool &recent_submenu_open,
                           const gui::DockLayout &docks,
                           std::span<const std::string> recent_projects) {
  ApplicationMenuResult result{};
  const gui::UiId dismiss_id = gui::ui_id("editor.menu.dismiss");
  const gui::Rect whole{0.0F, 0.0F, width, height};
  if (ui.active() == dismiss_id) {
    static_cast<void>(ui.grab(dismiss_id, whole));
  }
  if (open_menu == ApplicationMenu::None) {
    recent_submenu_open = false;
    return result;
  }

  const gui::Rect popup = menu_popup_bounds(ui, open_menu);
  if (open_menu != ApplicationMenu::File) {
    recent_submenu_open = false;
  }
  const gui::Rect recent_row = recent_projects_row(popup);
  const gui::Rect recent_popup =
      recent_projects_popup(popup, width, recent_projects.size());
  if (open_menu == ApplicationMenu::File) {
    if (recent_row.contains(ui.pointer_x(), ui.pointer_y())) {
      recent_submenu_open = true;
    } else if (popup.contains(ui.pointer_x(), ui.pointer_y()) &&
               !recent_popup.contains(ui.pointer_x(), ui.pointer_y())) {
      recent_submenu_open = false;
    }
  }
  const std::string_view anchor_label =
      open_menu == ApplicationMenu::File   ? std::string_view{"File"}
      : open_menu == ApplicationMenu::Edit ? std::string_view{"Edit"}
      : open_menu == ApplicationMenu::View ? std::string_view{"View"}
                                           : std::string_view{"Window"};
  const gui::Rect anchor = menu_bar_entry_bounds(ui, anchor_label);
  if (ui.pointer_pressed() && !popup.contains(ui.pointer_x(), ui.pointer_y()) &&
      !(recent_submenu_open && open_menu == ApplicationMenu::File &&
        recent_popup.contains(ui.pointer_x(), ui.pointer_y())) &&
      !anchor.contains(ui.pointer_x(), ui.pointer_y())) {
    if (ui.pointer_y() >= top_bar_height) {
      const gui::Interaction dismissed = ui.grab(dismiss_id, whole);
      if (dismissed.pressed) {
        open_menu = ApplicationMenu::None;
        recent_submenu_open = false;
      }
    } else {
      open_menu = ApplicationMenu::None;
      recent_submenu_open = false;
    }
    return result;
  }

  if (open_menu == ApplicationMenu::File) {
    if (recent_submenu_open) {
      float recent_y = recent_popup.y + application_menu_padding;
      for (const std::string &path : recent_projects) {
        const gui::Rect row{recent_popup.x + application_menu_padding,
                            recent_y,
                            recent_popup.width - 2.0F * application_menu_padding,
                            application_menu_row_height};
        if (ui.grab(mix(gui::ui_id("editor.file-menu.recent"), path), row).clicked) {
          result.action = ApplicationMenuAction::OpenRecentProject;
          result.project_path = path;
          open_menu = ApplicationMenu::None;
          recent_submenu_open = false;
          return result;
        }
        recent_y += application_menu_row_height;
      }
    }
    float y = popup.y + application_menu_padding;
    for (const ApplicationMenuItem &item : file_menu_items) {
      const auto &[label, action] = item;
      if (action == ApplicationMenuAction::StartSceneLink) {
        y += file_menu_separator_height;
      }
      if (action == ApplicationMenuAction::OpenProjectSettings) {
        y += file_menu_separator_height;
      }
      const gui::Rect row{popup.x + application_menu_padding, y,
                          popup.width - application_menu_padding * 2.0F,
                          application_menu_row_height};
      if (action == ApplicationMenuAction::RecentProjects) {
        static_cast<void>(ui.grab(mix(gui::ui_id("editor.file-menu"), label), row));
        y += application_menu_row_height;
        continue;
      }
      if (ui.grab(mix(gui::ui_id("editor.file-menu"), label), row).clicked) {
        result.action = action;
        open_menu = ApplicationMenu::None;
        recent_submenu_open = false;
        return result;
      }
      y += application_menu_row_height;
    }
    return result;
  }

  if (open_menu == ApplicationMenu::View) {
    float y = popup.y + application_menu_padding;
    for (const ViewMenuItem &item : view_menu_items) {
      const gui::Rect row{popup.x + application_menu_padding, y,
                          popup.width - application_menu_padding * 2.0F,
                          application_menu_row_height};
      if (ui.grab(mix(gui::ui_id("editor.view-menu"), item.label), row)
              .clicked) {
        result.action = item.action;
        open_menu = ApplicationMenu::None;
        return result;
      }
      y += application_menu_row_height;
    }
    return result;
  }

  if (open_menu == ApplicationMenu::Edit) {
    constexpr std::array<std::pair<std::string_view, ApplicationMenuAction>, 4U>
        items{{
            {"Undo", ApplicationMenuAction::Undo},
            {"Redo", ApplicationMenuAction::Redo},
            {"Copy", ApplicationMenuAction::Copy},
            {"Paste", ApplicationMenuAction::Paste},
        }};
    float y = popup.y + 4.0F;
    for (const auto &[label, action] : items) {
      if (action == ApplicationMenuAction::Copy) {
        y += 8.0F;
      }
      const gui::Rect row{popup.x + 4.0F, y, popup.width - 8.0F, 22.0F};
      if (ui.grab(mix(gui::ui_id("editor.edit-menu"), label), row).clicked) {
        result.action = action;
        open_menu = ApplicationMenu::None;
        return result;
      }
      y += 22.0F;
    }
    return result;
  }

  float y = popup.y + 4.0F;
  for (const WindowPanelEntry &entry : window_panel_entries()) {
    const gui::Rect row{popup.x + 4.0F, y, popup.width - 8.0F, 21.0F};
    if (ui.grab(mix(gui::ui_id("editor.window-menu"), entry.title), row)
            .clicked) {
      result.action = ApplicationMenuAction::TogglePanel;
      result.panel = entry.panel;
      open_menu = ApplicationMenu::None;
      return result;
    }
    y += 21.0F;
  }
  static_cast<void>(docks);
  return result;
}

void draw_application_menu_overlay(gui::UiContext &ui,
                                   ApplicationMenu open_menu,
                                   bool recent_submenu_open,
                                   const gui::DockLayout &docks,
                                   std::span<const std::string> recent_projects,
                                   float viewport_width) {
  if (open_menu == ApplicationMenu::None) {
    return;
  }
  const gui::Rect popup = menu_popup_bounds(ui, open_menu);
  ui.panel(gui::Rect{popup.x + 3.0F, popup.y + 4.0F, popup.width,
                     popup.height},
           gui::UiColor{0.0F, 0.0F, 0.0F, 0.35F});
  ui.panel(popup, ui.theme().panel);
  outline(ui, popup, ui.theme().border);
  ui.push_clip(popup);

  const auto draw_row = [&](gui::UiId id, const gui::Rect &row,
                            std::string_view label, bool selected = false) {
    const bool hovered = row.contains(ui.pointer_x(), ui.pointer_y());
    if (hovered || ui.active() == id || selected) {
      ui.panel(row,
               hovered ? ui.theme().widget_hover : ui.theme().widget_active);
    }
    ui.label(gui::Rect{row.x + 8.0F, row.y, row.width - 12.0F, row.height},
             label, hovered || selected ? ui.theme().text : ui.theme().text_muted);
  };

  if (open_menu == ApplicationMenu::File) {
    float y = popup.y + application_menu_padding;
    for (const ApplicationMenuItem &item : file_menu_items) {
      const std::string_view label = item.label;
      if (item.action == ApplicationMenuAction::StartSceneLink) {
        ui.panel(gui::Rect{popup.x + 8.0F, y + 3.0F, popup.width - 16.0F, 1.0F},
                 ui.theme().divider);
        y += file_menu_separator_height;
      }
      if (label == "Project Settings...") {
        // Above the rule the File menu acts on the project file. Below it are
        // the two settings dialogs - one document, one machine - and the rule
        // is what makes that distinction visible at a glance.
        ui.panel(gui::Rect{popup.x + 8.0F, y + 3.0F, popup.width - 16.0F, 1.0F},
                 ui.theme().divider);
        y += file_menu_separator_height;
      }
      const gui::Rect row{popup.x + application_menu_padding, y,
                          popup.width - application_menu_padding * 2.0F,
                          application_menu_row_height};
      draw_row(mix(gui::ui_id("editor.file-menu"), label), row, label,
               item.action == ApplicationMenuAction::RecentProjects &&
                   recent_submenu_open);
      if (item.action == ApplicationMenuAction::RecentProjects) {
        ui.label(gui::Rect{row.x + row.width - 18.0F, row.y, 12.0F, row.height},
                 ">", ui.theme().text_muted);
      }
      y += application_menu_row_height;
    }
  } else if (open_menu == ApplicationMenu::View) {
    float y = popup.y + application_menu_padding;
    for (const ViewMenuItem &item : view_menu_items) {
      const gui::Rect row{popup.x + application_menu_padding, y,
                          popup.width - application_menu_padding * 2.0F,
                          application_menu_row_height};
      draw_row(mix(gui::ui_id("editor.view-menu"), item.label), row,
               item.label);
      const float shortcut_width = ui.font_width(item.shortcut);
      ui.label(gui::Rect{row.x + row.width - shortcut_width - 10.0F, row.y,
                         shortcut_width + 2.0F, row.height},
               item.shortcut, ui.theme().text_muted);
      y += application_menu_row_height;
    }
  } else if (open_menu == ApplicationMenu::Edit) {
    constexpr std::array<std::string_view, 4U> labels{
        "Undo    Ctrl+Z", "Redo    Ctrl+Shift+Z", "Copy    Ctrl+C",
        "Paste   Ctrl+V"};
    constexpr std::array<std::string_view, 4U> ids{"Undo", "Redo", "Copy",
                                                   "Paste"};
    float y = popup.y + 4.0F;
    for (std::size_t index = 0U; index < labels.size(); ++index) {
      if (index == 2U) {
        ui.panel(gui::Rect{popup.x + 8.0F, y + 3.0F, popup.width - 16.0F,
                           1.0F},
                 ui.theme().divider);
        y += 8.0F;
      }
      const gui::Rect row{popup.x + 4.0F, y, popup.width - 8.0F, 22.0F};
      draw_row(mix(gui::ui_id("editor.edit-menu"), ids[index]), row,
               labels[index]);
      y += 22.0F;
    }
  } else {
    float y = popup.y + 4.0F;
    for (const WindowPanelEntry &entry : window_panel_entries()) {
      const bool open = docks.leaf_of(entry.panel) != gui::no_dock;
      std::string label = open ? "[x] " : "[ ] ";
      label.append(entry.title);
      const gui::Rect row{popup.x + 4.0F, y, popup.width - 8.0F, 21.0F};
      draw_row(mix(gui::ui_id("editor.window-menu"), entry.title), row, label);
      y += 21.0F;
    }
  }
  ui.pop_clip();
  if (open_menu == ApplicationMenu::File && recent_submenu_open) {
    const gui::Rect submenu =
        recent_projects_popup(popup, viewport_width, recent_projects.size());
    ui.panel(gui::Rect{submenu.x + 3.0F, submenu.y + 4.0F,
                       submenu.width, submenu.height},
             gui::UiColor{0.0F, 0.0F, 0.0F, 0.35F});
    ui.panel(submenu, ui.theme().panel);
    outline(ui, submenu, ui.theme().border);
    ui.push_clip(submenu);
    float y = submenu.y + application_menu_padding;
    if (recent_projects.empty()) {
      ui.label(gui::Rect{submenu.x + 12.0F, y, submenu.width - 24.0F,
                         application_menu_row_height},
               "None yet", ui.theme().text_muted);
    } else {
      for (const std::string &path : recent_projects) {
        const gui::Rect row{submenu.x + application_menu_padding, y,
                            submenu.width - 2.0F * application_menu_padding,
                            application_menu_row_height};
        draw_row(mix(gui::ui_id("editor.file-menu.recent"), path), row,
                 recent_project_label(path));
        y += application_menu_row_height;
      }
    }
    ui.pop_clip();
  }
}

[[nodiscard]] bool draw_status_bar(gui::UiContext &ui, float width,
                                   float height,
                                   const EditorTimeline &timeline,
                                   const EditorSwitcherState &switcher,
                                   std::size_t selection_count,
                                   scene::Canvas canvas,
                                   const EditorProjectState &project,
                                   std::size_t pending_media_probes,
                                   std::string_view ingest_message,
                                   core::u16 zoom_percent) {
  const gui::Rect status{0.0F, std::max(height - status_bar_height, 0.0F),
                         width, status_bar_height};
  ui.panel(status, ui.theme().header);
  ui.panel(gui::Rect{status.x, status.y, status.width, 1.0F},
           ui.theme().border);

  // Each reading is a quiet label and a value, with a dot where the value is
  // a state - the way a broadcast status line reads, rather than a run of
  // capitals the operator has to parse.
  float x = status.x + 6.0F;
  const auto reading = [&](std::string_view label, std::string_view value,
                           std::optional<gui::UiColor> dot,
                           gui::UiColor value_color) {
    const float label_width = label.empty() ? 0.0F : ui.font_width(label) + 6.0F;
    const float dot_width = dot.has_value() ? 13.0F : 0.0F;
    const float value_width = ui.font_width(value);
    const float total = label_width + dot_width + value_width + 22.0F;
    if (!label.empty()) {
      ui.label(gui::Rect{x + 8.0F, status.y, label_width, status.height},
               label, ui.theme().text_muted);
    }
    if (dot.has_value()) {
      draw_status_dot(ui, x + 8.0F + label_width + 4.0F,
                      status.y + status.height * 0.5F, *dot, 3.0F);
    }
    ui.label(gui::Rect{x + 8.0F + label_width + dot_width, status.y,
                       value_width + 2.0F, status.height},
             value, value_color);
    x += total;
    ui.panel(gui::Rect{x - 1.0F, status.y + 7.0F, 1.0F, status.height - 14.0F},
             ui.theme().divider);
  };

  // Preview owns its own playhead, so an operator scrubbing a rehearsal scene
  // must still be able to read where the live show is without leaving Preview.
  reading(switcher.rehearsal_mode ? "Preview" : "Program",
          "frame " + std::to_string(timeline.show_frame),
          ui.theme().accent,
          ui.theme().text);
  reading("", timeline.playing ? "Playing" : "Paused",
          std::nullopt,
          ui.theme().text);
  if (switcher.rehearsal_mode)
    reading("Program", "frame " + std::to_string(timeline.program_frame), std::nullopt, ui.theme().text_muted);
  ui.tooltip({status.x, status.y, x - status.x, status.height},
             "Canvas " + std::to_string(canvas.width) + " x " + std::to_string(canvas.height) +
             " · " + std::to_string(selection_count) + " selected");

  // The interface zoom, at the right edge: what Ctrl+= and Ctrl+- changed,
  // and a click away from the View menu that changes it back.
  const std::string zoom_text = "UI " + std::to_string(zoom_percent) + "%";
  const float zoom_width = ui.font_width(zoom_text) + 20.0F;
  const gui::Rect zoom{width - zoom_width - 6.0F, status.y + 4.0F, zoom_width,
                       status.height - 8.0F};
  const gui::Interaction zoom_it =
      ui.grab(gui::ui_id("editor.status.zoom"), zoom);
  if (zoom_it.hovered || zoom_it.held) {
    ui.panel(zoom, ui.theme().widget_hover);
  }
  ui.label(gui::Rect{zoom.x + 10.0F, zoom.y, zoom_width - 10.0F, zoom.height},
           zoom_text,
           zoom_percent == 100U ? ui.theme().text_muted : ui.theme().text);
  ui.tooltip(zoom, "Interface size - Ctrl+= larger, Ctrl+- smaller, Ctrl+0 "
                   "as the display asks");
  width -= zoom_width + 10.0F;

  const float remaining = std::max(width - x, 0.0F);

  if (!ingest_message.empty()) {
    const float message_width = std::min(std::max(remaining, 0.0F), 580.0F);
    ui.label(gui::Rect{width - message_width - 10.0F, status.y, message_width, status.height},
        ingest_message, ui.theme().accent);
  } else if (!project.message.empty()) {
    const float message_width = std::min(std::max(remaining, 0.0F), 310.0F);
    const bool error = project.message.starts_with("ERROR");
    ui.label(gui::Rect{width - message_width - 10.0F, status.y, message_width,
                       status.height},
             project.message, error ? ui.theme().warning : ui.theme().good);
  } else if (pending_media_probes > 0U) {
    std::array<char, 48U> probing_text{};
    const int probing_length = std::snprintf(
        probing_text.data(), probing_text.size(), "READING MEDIA  %zu",
        pending_media_probes);
    const float message_width = std::min(std::max(remaining, 0.0F), 190.0F);
    ui.label(gui::Rect{width - message_width - 10.0F, status.y, message_width,
                       status.height},
             std::string_view{
                 probing_text.data(),
                 static_cast<std::size_t>(std::max(probing_length, 0))},
             ui.theme().accent);
  }
  return zoom_it.clicked;
}

// The editor's body text, in logical units. Every row height and inset in the
// panels is laid out around it.
constexpr core::u32 editor_font_size = 14U;

// The interface zoom steps Ctrl+= and Ctrl+- walk through, in percent of the
// display's own scale.
constexpr std::array<core::u16, 10U> ui_zoom_steps{
    {67U, 75U, 85U, 100U, 115U, 125U, 150U, 175U, 200U, 250U}};

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

struct EditorShell::Impl final {
  // Declare the device owner first so every GPU texture and submission,
  // including the asset-activation fence, is released before its device.
  gui::UiWindow window;
  struct PendingModelIngest {
    std::shared_ptr<render::ModelIngestJob> job;
    std::string display_name;
    core::u64 gesture{};
    core::u64 project_generation{};
    scene::SceneId add_scene{};
    std::optional<FeedSurfaceAddRequest> surface_add;
    std::optional<FeedSurfaceEditRequest> surface_edit;
    doc::NodeId texture_target{};
    std::string expected_source;
    doc::NodeId folder{};
    // The Library asset this ingest re-reads for an accepted change.
    doc::NodeId revise{};
  };
  std::vector<PendingModelIngest> pending_model_ingests;
  // Re-probed assets waiting for the rest of their gesture, so everything
  // one Accept asked for reaches the Director - and undoes - together.
  std::vector<AssetRevisionIntent> pending_revisions;
  render::ModelCache inspector_model_cache;
  core::AssetPublicationGate asset_gate;
  core::AssetSetReadiness assets{};
  std::optional<rhi::Texture> visible_program;
  struct PendingRecording {
    rhi::SubmittedWork work;
    core::u64 asset_digest{}, record_ns{}, profile_digest{}, sequence{};
  };
  // The RHI supports at most four contexts. Retaining their tickets preserves
  // timestamp ownership without reducing the existing recording concurrency.
  std::array<PendingRecording, 4U> pending_recordings{};
  core::FrameTiming frame_timing{};
  core::u64 next_recording_sequence{1U};
  core::u64 uploaded_asset_digest{};
  std::shared_ptr<render::BakeJob> bake_job;
  scene::BakedLighting bake_settings;
  doc::NodeId bake_stage;
  core::u64 bake_snapshot_digest{};
  std::optional<core::u64> look_document_digest;
  core::u64 look_asset_generation{};
  doc::NodeId look_stage;
  bool look_observed{}, look_stale{};
  std::string bake_message;
  std::string completed_bake_content;
  std::chrono::steady_clock::time_point program_wait_started{};
  StageAssetStatus stage_assets{true, "LOADING STAGE ASSETS...",
                                "Preparing the first Stage image."};
  std::string ingest_message;
  enum class PendingMediaAction : core::u8 { Add, Import, Revise };

  struct PendingMediaProbe final {
    PendingMediaAction action{};
    std::string source;
    std::future<render::MediaProbe> future;
    bool launch_failed{};
    core::u64 gesture{};
    scene::SceneId scene{};
    core::u64 project_generation{};
    // The Library folder an import lands in.
    doc::NodeId folder{};
    // The Library asset a Revise probe re-reads.
    doc::NodeId revise{};
  };

  gui::FontAtlas font;
  gui::InputState input;
  gui::UiDrawList draw_list;
  gui::UiContext ui;
  LibraryThumbnailTextureCache library_thumbnail_textures;
  TimelineFilmstripTextureCache timeline_filmstrip_textures;
  // Program, transition-to, and Stage are recorded into one command list.
  // Their media inputs share immutable source textures while their render
  // targets, playheads, and compositor state remain independent.
  render::MediaCache frame_media_cache;

  render::CompositorSceneExecutor program_renderer;
  render::CompositorSceneExecutor program_to_renderer;
  render::ProjectionMeshWarp program_warp;
  render::ProjectionMeshWarp program_to_warp;
  render::SceneMixer program_mixer;
  render::OutputTransform program_display;
  render::CompositorSceneExecutor stage_renderer;
  render::OutputTransform stage_display;
  render::OutputProjectionCache program_projection_cache;
  // A missed non-blocking command acquisition holds this last complete image.
  // The operator keeps a stale picture instead of a black panel while the GPU
  // drains; an actual record/submit fault invalidates it explicitly below.
  bool program_preview_valid{};
  core::u32 program_preview_width{};
  core::u32 program_preview_height{};

  gui::DockLayout docks;
  gui::DockHost dock_host;
  WorkspaceLayoutBytes saved_workspace_layout;
  core::u32 workspace_window_width{};
  core::u32 workspace_window_height{};
  // Operator interface zoom, in percent of the display's own scale.
  core::u16 ui_zoom_percent{100U};
  std::optional<InspectorColorSampleRequest> color_sample_request;
  ViewportPanel program_panel{gui::ui_id("editor.viewport.program"),
                              ViewportRole::Program};
  ViewportPanel stage_panel{gui::ui_id("editor.viewport.stage"),
                            ViewportRole::Stage};
  InspectorPanel inspector_panel{gui::ui_id("editor.inspector")};
  TimelinePanel timeline_panel{gui::ui_id("editor.timeline")};
  CuesPanel cues_panel{gui::ui_id("editor.cues")};
  ScenePanel scene_panel{gui::ui_id("editor.scene")};
  SelectionSetsPanel selection_sets_panel{
      gui::ui_id("editor.selection-sets")};
  MasksPanel masks_panel{gui::ui_id("editor.masks")};
  LibraryPanel library_panel{gui::ui_id("editor.library")};
  EffectsPanel effects_panel{gui::ui_id("editor.effects")};
  ColorCorrectionPanel color_correction_panel{
      gui::ui_id("editor.color-correction")};
  TexturePanel texture_panel{gui::ui_id("editor.texture")};
  CompositorPanel compositor_panel{gui::ui_id("editor.compositor")};
  OutputMonitorPanel output_panel{gui::ui_id("editor.output-monitor")};
  LiveDataPanel live_data_panel{gui::ui_id("editor.live-data")};
  PreferencesPanel preferences_panel{gui::ui_id("editor.preferences")};
  SceneSettingsPanel scene_settings_panel;
  SceneSettingsTarget scene_settings_selection{SceneSettingsTarget::None};
  ProjectSettingsPanel project_settings_panel{
      gui::ui_id("editor.project-settings")};
  HardwareMappingPanel hardware_mapping_panel{
      gui::ui_id("editor.hardware-mapping")};
  FeedMappingPanel feed_mapping_panel{gui::ui_id("editor.feed-mapping")};

  // Scene-node selection is editor-local and canonical. Compositor, physical
  // surface, and mapping selections remain in their distinct carriers below.
  NodeSelection scene_selection;
  // A saved-set row is a distinct typed panel selection. It never becomes a
  // Scene node selection and therefore cannot accidentally enter Inspector,
  // viewport, keyboard Delete, or clip/key clipboard operations.
  doc::NodeId selection_set_selection{};
  // Mask and Bezier-point selection is intentionally disjoint from canonical
  // Scene selection. The latter chooses the target; this carrier chooses the
  // operation/point within that target and is shared only by Masks + Stage.
  MasksPanelSelection mask_selection{};
  bool mask_coverage_visible{};
  // Group ids are allocated at the Director boundary. Keep the exact child
  // set briefly so the first projected frame containing their new parent can
  // promote that parent to the canonical selection.
  std::vector<doc::NodeId> pending_group_members;
  core::u32 pending_group_frames{};
  // A new empty group has no members to recognise it by, so keep the groups
  // that existed when it was asked for: the first projected frame with one
  // more under `pending_new_group_parent` (zero: the top level) selects it
  // and opens its name.
  std::optional<doc::NodeId> pending_new_group_parent;
  std::set<doc::NodeId> pending_new_group_known;
  core::u32 pending_new_group_frames{};
  std::optional<doc::NodeId> stage_surface_selection;
  std::optional<doc::NodeId> program_output_map_node;
  scene::SceneId displayed_scene{};
  std::size_t scene_list_offset{};
  std::optional<scene::SceneId> pending_scene_delete;
  // The PROJECT SCENES row whose right-click menu is open.
  std::optional<scene::SceneId> scene_menu_scene;
  // In-place renaming of a project scene's row.
  InlineRename scene_rename{gui::ui_id("editor.scene.rename")};
  ClickTracker scene_clicks;
  core::u64 project_generation{};
  DeleteDomain delete_domain{DeleteDomain::Scene};
  ApplicationMenu open_menu{ApplicationMenu::None};
  bool recent_projects_submenu_open{};
  bool scene_link_details_open{};
  bool blender_connection_open{};
  std::string blender_selection;
  std::size_t blender_page{};
  std::string publication_storage_draft;
  bool publication_storage_editing{};
  double hardware_warning_until{};
  double compositor_warning_until{};

  // The Stage camera is editor-local and never enters the document. It is
  // navigated as in the Unreal Editor (see stage_navigation_for): a button
  // joins a camera gesture only when it was pressed on the Stage, and the
  // gesture then follows whichever of those buttons are still held.
  StageOrbit stage_view{};
  bool navigate_left{};
  bool navigate_middle{};
  bool navigate_right{};
  // The gesture has moved the camera, so the cursor is hidden and locked.
  bool navigation_moved{};
  float fly_speed{stage_default_fly_speed};
  double fly_speed_shown_until{};
  double navigation_time{};

  // The Stage renderer follows the dock's actual content aspect (one frame
  // behind a resize), so the authoring view fills its pane instead of looking
  // like a letterboxed program feed. Program keeps the authored canvas.
  core::u32 stage_surface_width{};
  core::u32 stage_surface_height{};

  // A viewport drag captures every selected element's starting local position.
  // The panel reports one displayed-world delta; each node maps that through
  // its compositor and scene-group parents so the selection moves rigidly even
  // while the committed scene lags behind the drag.
  std::vector<doc::NodeId> group_nodes;
  std::vector<scene::Vec3> group_start;
  // Maps a displayed world-space drag vector back into each node's authored
  // parent space. This includes both scene-group ancestry and any compositor
  // spatial parent (for example an LED car-process plate slot).
  std::vector<scene::Mat4> group_delta_to_local;
  // What a rotate or scale needs as well: each node's evaluated rotation and
  // scale, and where and how it is drawn, parallel to group_nodes.
  std::vector<TransformStart> group_transform;
  // Blender-linked objects are placed by one world matrix: their authored
  // world when the gesture began, and how the Stage's display differs from it.
  std::vector<bool> group_linked;
  std::vector<scene::Mat4> group_authored_world;
  std::vector<scene::Mat4> group_display_adjustment;

  core::u64 edit_gesture{};
  core::u64 hardware_edit_gesture{};
  core::u64 feed_mapping_edit_gesture{};
  core::u64 compositor_edit_gesture{};
  HistoryContext history_context{HistoryContext::Scene};
  // Held for the whole of one clip drag so that the per-frame placements a
  // move or a trim authors collapse into a single undo step.
  core::u64 clip_edit_gesture{};
  core::u64 lane_edit_gesture{};
  core::u64 next_edit_gesture{1U};
  std::vector<PendingMediaProbe> pending_media_probes;
  bool manipulating{};
  bool semantic_boundary{};
  gui::UiId semantic_focus{};

  Impl(gui::UiWindow &&created_window, gui::FontAtlas &&created_font,
       render::CompositorSceneExecutor &&program,
       render::CompositorSceneExecutor &&program_to,
       render::ProjectionMeshWarp &&program_projection,
       render::ProjectionMeshWarp &&program_to_projection,
       render::SceneMixer &&mixer,
       render::OutputTransform &&program_output,
       render::CompositorSceneExecutor &&stage,
       render::OutputTransform &&stage_output,
       std::span<const std::byte> workspace_layout)
      : window(std::move(created_window)), font(std::move(created_font)),
        ui(input, draw_list, font, editor_theme()),
        library_thumbnail_textures(window.device()),
        timeline_filmstrip_textures(window.device()),
        program_renderer(std::move(program)),
        program_to_renderer(std::move(program_to)),
        program_warp(std::move(program_projection)),
        program_to_warp(std::move(program_to_projection)),
        program_mixer(std::move(mixer)),
        program_display(std::move(program_output)),
        stage_renderer(std::move(stage)),
        stage_display(std::move(stage_output)),
        dock_host(docks) {
    workspace_window_width = window.pixel_width();
    workspace_window_height = window.pixel_height();
    reset_workspace();
    if (!workspace_layout.empty()) {
      auto decoded = decode_workspace_layout(workspace_layout);
      if (decoded) {
        ui_zoom_percent = decoded.value().ui_zoom_percent;
        compositor_panel.set_parameter_panel_width(
            decoded.value().compositor_parameter_width);
        compositor_panel.show_minimap(decoded.value().compositor_minimap);
      }
      if (decoded && docks.restore(decoded.value().docks)) {
        stage_panel.set_resolution(decoded.value().stage_resolution);
        program_panel.set_resolution(decoded.value().program_resolution);
        saved_workspace_layout.assign(workspace_layout.begin(),
                                      workspace_layout.end());
      }
    }
  }

  void reset_workspace() {
    ui.clear_focus();
    docks = gui::DockLayout{};
    dock_host = gui::DockHost{docks};
    // Keep the picture and common edits visible; specialized tools share
    // dock tabs or open from Window. Every region remains rearrangeable.
    const gui::DockId stage_dock = docks.add(panel_stage, "Stage");
    const gui::DockId timeline_dock = docks.dock(
        panel_timeline, "Timeline", stage_dock, gui::DockSide::Bottom, 0.34F);
    static_cast<void>(
        docks.dock(panel_cues, "Cues", timeline_dock, gui::DockSide::Center));
    static_cast<void>(docks.dock(panel_automation, "Automation", timeline_dock,
                                 gui::DockSide::Center));
    docks.set_active(timeline_dock, panel_timeline);
    static_cast<void>(docks.dock(panel_compositor, "Compositor", timeline_dock,
                                gui::DockSide::Center));
    static_cast<void>(docks.dock(panel_output, "Output Monitor", timeline_dock,
                                gui::DockSide::Center));
    docks.set_active(timeline_dock, panel_timeline);

    const float workspace_width = static_cast<float>(window.width());
    const float properties_width = std::clamp(workspace_width * 0.25F, 320.0F, 440.0F);
    const float scene_width = std::clamp(workspace_width * 0.18F, 200.0F, 300.0F);

    const gui::DockId scene_dock = docks.dock(
        panel_scene, "Scene", docks.root(), gui::DockSide::Left,
        scene_width / std::max(workspace_width - properties_width - 6.0F, 1.0F));
    docks.set_active(scene_dock, panel_scene);
    const gui::DockId library_dock = docks.dock(
        panel_library, "Library", scene_dock, gui::DockSide::Bottom, 0.40F);
    static_cast<void>(docks.dock(panel_effects, "Effects", library_dock,
                                 gui::DockSide::Center));
    docks.set_active(library_dock, panel_library);

    const gui::DockId inspector_dock =
        docks.dock(panel_inspector, "Properties", docks.root(),
                   gui::DockSide::Right, properties_width / std::max(workspace_width, 1.0F));
    static_cast<void>(docks.dock(panel_color_correction, "Color",
                                 inspector_dock, gui::DockSide::Center));
    static_cast<void>(docks.dock(panel_texture, "Texture", inspector_dock,
                                 gui::DockSide::Center));
    static_cast<void>(docks.dock(panel_masks, "Masks", inspector_dock,
                                 gui::DockSide::Center));
    docks.set_active(inspector_dock, panel_inspector);

    // Program is a dock tab alongside Stage; it does not obscure the canvas
    // until the operator chooses to detach it.
    static_cast<void>(docks.dock(panel_program, "Program Output", stage_dock,
                                 gui::DockSide::Center));
    docks.set_active(stage_dock, panel_stage);

    // Register less frequently used panels so Window can open them on demand.
    for (const WindowPanelEntry &entry : window_panel_entries()) {
      if (docks.title_of(entry.panel).empty()) {
        docks.register_panel(entry.panel, entry.title);
      }
    }
  }

  [[nodiscard]] static std::string_view panel_title(gui::PanelId panel) {
    const auto found = std::find_if(window_panel_entries().begin(),
                                    window_panel_entries().end(),
                                    [panel](const WindowPanelEntry &entry) {
                                      return entry.panel == panel;
                                    });
    return found == window_panel_entries().end() ? std::string_view{"Panel"}
                                                 : found->title;
  }

  [[nodiscard]] gui::Rect setup_frame() const noexcept {
    const float width =
        std::clamp(static_cast<float>(window.width()) * 0.56F, 680.0F, 980.0F);
    const float height =
        std::clamp(static_cast<float>(window.height()) * 0.62F, 430.0F, 680.0F);
    return gui::Rect{
        std::max((static_cast<float>(window.width()) - width) * 0.5F, 18.0F),
        std::max((static_cast<float>(window.height()) - height) * 0.5F,
                 top_bar_height + 10.0F),
        width, height};
  }

  void show_mapping_panel(gui::PanelId wanted) {
    const gui::PanelId companion = wanted == panel_hardware_mapping
                                       ? panel_feed_mapping
                                       : panel_hardware_mapping;
    gui::DockId wanted_leaf = docks.leaf_of(wanted);
    if (wanted_leaf != gui::no_dock) {
      static_cast<void>(docks.show(wanted));
      return;
    }
    if (docks.has_saved_placement(wanted)) {
      static_cast<void>(docks.show(wanted));
      return;
    }
    const gui::DockId companion_leaf = docks.leaf_of(companion);
    if (companion_leaf != gui::no_dock) {
      wanted_leaf = docks.dock(wanted, panel_title(wanted), companion_leaf,
                               gui::DockSide::Center);
      docks.set_active(wanted_leaf, wanted);
      return;
    }

    // Introduce the panel through the ordinary tree, then detach it as a
    // real floating DockLayout root. The companion can later join it as a
    // tab without either setup surface occupying the everyday workspace.
    wanted_leaf = docks.add(wanted, panel_title(wanted));
    if (wanted_leaf != gui::no_dock) {
      wanted_leaf = docks.detach(wanted, setup_frame());
      docks.set_active(wanted_leaf, wanted);
    }
  }

  void show_setup() {
    constexpr std::array mapping_panels{panel_hardware_mapping,
                                        panel_feed_mapping};
    const bool saved_group =
        std::all_of(mapping_panels.begin(), mapping_panels.end(),
                    [this](gui::PanelId panel) {
                      return docks.is_open(panel) ||
                             docks.has_saved_placement(panel);
                    });
    if (saved_group) {
      docks.show_group(mapping_panels);
    } else {
      show_mapping_panel(panel_hardware_mapping);
      show_mapping_panel(panel_feed_mapping);
    }
    static_cast<void>(docks.show(panel_hardware_mapping));
  }

  void show_panel(gui::PanelId panel) {
    if (panel == panel_hardware_mapping || panel == panel_feed_mapping) {
      show_mapping_panel(panel);
      return;
    }
    if (docks.leaf_of(panel) != gui::no_dock) {
      const gui::DockId leaf = docks.leaf_of(panel);
      docks.set_active(leaf, panel);
      return;
    }

    gui::DockId leaf = docks.add(panel, panel_title(panel));
    if (panel == panel_program && leaf != gui::no_dock) {
      const float monitor_width = std::clamp(
          static_cast<float>(window.width()) * 0.24F, 300.0F, 420.0F);
      const float monitor_height = monitor_width * 9.0F / 16.0F + 68.0F;
      leaf = docks.detach(panel_program,
                          gui::Rect{static_cast<float>(window.width()) * 0.58F,
                                    static_cast<float>(window.height()) * 0.26F,
                                    monitor_width, monitor_height});
    }
    if (leaf != gui::no_dock) {
      docks.set_active(leaf, panel);
    }
  }

  void toggle_panel(gui::PanelId panel) {
    if (docks.leaf_of(panel) == gui::no_dock) {
      show_panel(panel);
    } else {
      docks.remove(panel);
    }
  }

  [[nodiscard]] bool has_selection() const noexcept {
    return !scene_selection.empty();
  }

  [[nodiscard]] bool is_selected(doc::NodeId node) const noexcept {
    return scene_selection.contains(node);
  }

  void select_only(doc::NodeId node) {
    scene_settings_selection = SceneSettingsTarget::None;
    pending_group_members.clear();
    pending_group_frames = 0U;
    pending_new_group_parent.reset();
    scene_selection.replace(node);
    delete_domain = DeleteDomain::Scene;
    history_context = HistoryContext::Scene;
    semantic_boundary = true;
  }

  void toggle(doc::NodeId node) {
    scene_settings_selection = SceneSettingsTarget::None;
    pending_group_members.clear();
    pending_group_frames = 0U;
    pending_new_group_parent.reset();
    scene_selection.toggle(node);
    delete_domain = scene_selection.empty() ? DeleteDomain::Neutral
                                            : DeleteDomain::Scene;
    history_context = HistoryContext::Scene;
    semantic_boundary = true;
  }

  void select_range(doc::NodeId node,
                    std::span<const doc::NodeId> visible_order) {
    pending_group_members.clear();
    pending_group_frames = 0U;
    pending_new_group_parent.reset();
    scene_selection.select_range(node, visible_order);
    delete_domain = DeleteDomain::Scene;
    history_context = HistoryContext::Scene;
    semantic_boundary = true;
  }

  void clear_scene_selection(bool activate_scene_history) {
    scene_settings_selection = SceneSettingsTarget::None;
    pending_group_members.clear();
    pending_group_frames = 0U;
    pending_new_group_parent.reset();
    scene_selection.clear();
    delete_domain = DeleteDomain::Neutral;
    if (activate_scene_history) {
      history_context = HistoryContext::Scene;
    }
    semantic_boundary = true;
  }

  [[nodiscard]] doc::NodeId anchor() const noexcept {
    return scene_selection.primary();
  }

  [[nodiscard]] core::u64 begin_gesture() noexcept {
    if (!manipulating) {
      manipulating = true;
      edit_gesture = next_edit_gesture++;
    }
    return edit_gesture;
  }
};

EditorShell::EditorShell(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

EditorShell::~EditorShell() {
  // The window owns the device, but scene targets, UI caches and pipelines
  // are destroyed first. Finish their submissions before any owner releases
  // resources; the device's own destructor would be too late.
  if (impl_) impl_->window.device().wait_idle();
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
      render::RendererLimits{}, render::MediaCacheConfig{}, working_format);
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
                       timeline_markers) noexcept {
  Impl &impl = *impl_;
  EditorFrame result{};
  // The plates the Director derived this frame, drawn over both viewports so
  // an operator can see the regions the physical cameras are looking at. A
  // camera drawing nothing contributes no entry, which is also when there is
  // nothing truthful to outline.
  std::vector<ViewportPlate> plate_overlay;
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
  reset_library_thumbnail_textures(project.generation);
  reset_timeline_filmstrip_textures(project.generation);

  if (!impl.window.pump(impl.input)) {
    if (project.dirty &&
        !confirm_discard_project_changes("those changes and close")) {
      impl.window.cancel_close_request();
      return result;
    }
    result.status = EditorStatus::Closed;
    return result;
  }
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
    impl.stage_panel.reset_project_state();
    impl.group_nodes.clear();
    impl.group_start.clear();
    impl.manipulating = false;
    impl.navigate_left = false;
    impl.navigate_middle = false;
    impl.navigate_right = false;
    impl.hardware_edit_gesture = 0U;
    impl.feed_mapping_edit_gesture = 0U;
    impl.compositor_edit_gesture = 0U;
    impl.clip_edit_gesture = 0U;
    impl.lane_edit_gesture = 0U;
    impl.delete_domain = DeleteDomain::Neutral;
    impl.ui.clear_focus();
    impl.timeline_panel.reset_project_state();
    impl.cues_panel.reset();
    impl.hardware_mapping_panel.reset_project_state();
    impl.feed_mapping_panel.reset_project_state();
    impl.compositor_panel.reset_project_state();
    impl.library_panel.reset_project_state();
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
  const std::span<const LibraryProxyRuntimeObservation>
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

  const scene::Scene &authored_stage =
      switcher.stage_scene != nullptr ? *switcher.stage_scene : scene;
  const scene::Camera stage_camera =
      stage_camera_for(authored_stage.camera, impl.stage_view);
  const auto request_clip_clipboard = [&](ClipClipboardAction action) {
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
  };
  const auto request_key_clipboard =
      [&](TimelineKeyClipboardEdit edit) {
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
      };
  // Marker and cue changes from either panel. One frame's batch is one
  // gesture: a multi-marker drag lands on release, so it undoes in one step.
  const auto route_marker_edits =
      [&](std::span<const TimelineMarkerEdit> edits) {
        if (edits.empty()) {
          return;
        }
        const core::u64 gesture = impl.next_edit_gesture++;
        for (const TimelineMarkerEdit &edit : edits) {
          result.marker_edits.push_back(TimelineMarkerEditIntent{
              edit.action, authored_stage.id, edit.marker, gesture});
        }
        impl.history_context = HistoryContext::Scene;
      };
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
  scene::Scene stage_scene = authored_stage;
  bypass_sampled_chroma_key(stage_scene, impl.color_sample_request);
  stage_scene.camera = stage_camera;
  if (impl.stage_surface_width > 0U && impl.stage_surface_height > 0U) {
    stage_scene.canvas =
        scene::Canvas{impl.stage_surface_width, impl.stage_surface_height};
  }
  // Keep the authored environment and lighting. Grid and guides are UI
  // overlays; making them readable must not change the scene's illumination.

  // Stage pixels already include compositor spatial nodes. Use that same
  // adjusted scene for picking and authoring chrome so a selected plate's
  // outline and gizmo land on its apparent rig position. Gesture edits still
  // use stage_scene below as their baseline, preserving the layer transform as
  // a local offset instead of baking the LED setup transform into it.
  scene::Scene stage_interaction_scene = stage_scene;
  if (compositor_graph.valid() && !compositor_graph.plan.steps.empty()) {
    const auto planned_stage =
        render::plan_compositor_scene(compositor_graph, stage_scene);
    if (planned_stage) {
      stage_interaction_scene = planned_stage.value().adjusted_scene;
    }
  }

  // Both scene targets are rendered before the UI pass. They belong to the
  // same device as UiWindow, so the panels sample them directly with no copy.
  rhi::Device &device = impl.window.device();
  const output::OutputMap *const program_output =
      selected_program_output(feed_map, impl.program_output_map_node);
  ProgramOutputExtent program_extent =
      program_output_extent(scene, program_output, hardware_map);
  const PreviewExtent program_preview = preview_extent(
      program_extent.preview_width, program_extent.preview_height,
      impl.program_panel.resolution());
  program_extent.preview_width = program_preview.width;
  program_extent.preview_height = program_preview.height;
  const PreviewExtent stage_preview = preview_extent(
      stage_scene.canvas.width, stage_scene.canvas.height,
      impl.stage_panel.resolution());
  if (impl.program_preview_width != program_preview.width ||
      impl.program_preview_height != program_preview.height ||
      impl.stage_renderer.width() != stage_preview.width ||
      impl.stage_renderer.height() != stage_preview.height) {
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
  const bool program_targets_ready =
      impl.program_renderer.resize(device, program_extent.preview_width,
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
  const bool stage_targets_ready =
      impl.stage_renderer.resize(device, stage_preview.width,
                                 stage_preview.height) &&
      impl.stage_display.resize(device, stage_preview.width,
                                stage_preview.height);
  const rhi::Texture *program_texture = impl.visible_program ? &*impl.visible_program : &impl.program_display.target();
  const rhi::Texture *stage_texture = &impl.stage_display.target();
  bool program_preview_ok = false;
  bool program_preview_held = false;
  bool program_mapping_unavailable = false;
  std::string_view program_preview_fault{"PROGRAM PREVIEW FAILED"};
  std::optional<render::RendererReadiness> renderer_readiness;
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

    // The Stage camera is a render override, not the geometry policy camera.
    // Keep the authored camera/canvas for LOD and shadow selection while the
    // matrices below drive the viewport. Swap the small camera carriers only;
    // copying the scene's thousands of drawable records again would undo the
    // submission savings this boundary exists to provide.
    render::SceneRenderView stage_view{};
    stage_view.matrices = scene::view_for_slice(
        stage_scene.camera, stage_scene.canvas, scene::whole(stage_scene.canvas));
    stage_view.eye = stage_scene.camera.eye;
    stage_view.forward = scene::normalize(
        stage_scene.camera.target - stage_scene.camera.eye);
    stage_view.history_identity = 0x5354'4147'4556'4945ULL;
    stage_view.clip_planes = render::SceneClipPlanes{
        stage_camera.near_plane, stage_camera.far_plane};
    auto geometry_camera = authored_stage.camera;
    auto geometry_canvas = authored_stage.canvas;
    std::swap(stage_scene.camera, geometry_camera);
    std::swap(stage_scene.canvas, geometry_canvas);
    const rhi::Texture *const rendered_stage =
        stage_targets_ready
            ? record_editor_scene(impl.stage_renderer, commands, stage_scene,
                                  compositor_graph, &impl.frame_media_cache,
                                  &stage_view)
            : nullptr;
    std::swap(stage_scene.camera, geometry_camera);
    std::swap(stage_scene.canvas, geometry_canvas);
    impl.stage_display.adopt_color(stage_scene.renderer_settings.color, scene::asset_generation_key(stage_scene));
    const bool stage_ok =
        rendered_stage != nullptr && impl.stage_display.render(
            commands, *rendered_stage,
            scene::CanvasRect{0U, 0U, stage_preview.width,
                              stage_preview.height},
            impl.stage_renderer.scene_linear_output());
    auto stage_assets = stage_asset_status(
        impl.stage_renderer.renderer_passes().geometry,
        impl.stage_renderer.media_cache_stats().pending_requests,
        rendered_stage != nullptr ? impl.stage_display.color_state()
                                  : render::DisplayLutState::Ready);
    if (!stage_targets_ready) {
      stage_assets = {false, "STAGE TARGETS UNAVAILABLE",
                      "The Stage render target could not be created."};
    } else if (!impl.stage_renderer.renderer_readiness().satisfied) {
      stage_assets = {false, "STAGE RENDER UNAVAILABLE",
                      std::string{impl.stage_renderer.renderer_readiness().headline()}};
    } else if (!stage_ok && stage_assets.headline.empty()) {
      stage_assets = {false, "STAGE RENDER FAILED",
                      "Check Renderer diagnostics."};
    }
    if (!stage_ok) {
      static_cast<void>(commands.clear(
          impl.stage_display.target(),
          rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}));
    }
    stage_texture = &impl.stage_display.target();
    const auto record_ns = static_cast<core::u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - cpu_record_begin).count());
    const auto profile_digest = scene::renderer_settings_digest(program_render_scene->renderer_settings);
    auto submitted = device.submit(std::move(commands));
    if (!submitted) {
      program_preview_ok = false;
      program_preview_fault = "PROGRAM SUBMIT FAILED";
      impl.stage_assets = {false, "STAGE SUBMIT FAILED",
                           "The Stage image could not be submitted to the GPU."};
    } else {
      impl.stage_assets = std::move(stage_assets);
      impl.asset_gate = next_asset_gate;
      impl.assets.visible_commit = impl.assets.digest == impl.asset_gate.visible_digest() ? impl.asset_gate.visible_commit() : 0;
      *recording_slot = {std::move(submitted).value(), prepared.ready ? prepared.digest : 0U,
          record_ns, profile_digest, impl.next_recording_sequence++};
    }
    impl.program_preview_valid = program_preview_ok;
  } else if (program_targets_ready && impl.program_preview_valid) {
    // Command acquisition is deliberately non-blocking. Keep the completed
    // prior image while all contexts are in flight; this is pressure, not a
    // renderer fault, and the next frame retries normally.
    program_preview_ok = true;
    program_preview_held = true;
  } else {
    program_preview_fault = "PROGRAM GPU BUSY";
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
  else if (impl.frame_media_cache.stats().pending_requests != 0)
    loading_detail = "Loading media: " + std::to_string(impl.frame_media_cache.stats().pending_requests) + " pending.";
  else if (program_geometry.assets.ready && !impl.assets.ready)
    loading_detail = "Waiting for GPU uploads to finish.";
  const auto program_assets = program_asset_status(impl.assets,
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
  std::string bake_activity = impl.look_stale
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
  std::optional<ViewportProjection> program_view_projection;
  if (program_preview_ok) {
    const scene::Canvas output_canvas{program_extent.output_width,
                                      program_extent.output_height};
    ViewportProjection projection{};
    projection.canvas = output_canvas;
    projection.forward =
        scene::normalize(scene.camera.target - scene.camera.eye);
    projection.up = scene.camera.up;
    if (program_output == nullptr) {
      projection.project = [&scene](scene::Vec3 point) {
        return scene::project_to_canvas(scene.camera, scene.canvas, point);
      };
      program_view_projection.emplace(std::move(projection));
    } else if (program_output->projection ==
               output::FeedProjection::CanvasSlice) {
      const scene::CanvasRect source = program_output->source;
      projection.project = [&scene, source,
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
        if (program_output->resolved_surface.has_value()) {
          projection.up = program_output->resolved_surface->geometry.up;
        }
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

  gui::UiContext &ui = impl.ui;
  ui.new_frame(impl.window.time_seconds());
  if (impl.color_sample_request.has_value() &&
      ui.key_pressed(gui::Key::Escape)) {
    impl.color_sample_request.reset();
  }
  if (ui.pointer_pressed() || ui.key_pressed(gui::Key::Escape) ||
      ui.key_pressed(gui::Key::Enter) || ui.key_pressed(gui::Key::Tab)) {
    // Pointer gestures own explicit transactions, and these keys commit,
    // cancel, or move text focus. All are semantic boundaries even when the
    // interaction ultimately authors no document change.
    impl.semantic_boundary = true;
  }
  const float width = static_cast<float>(impl.window.width());
  const float height = static_cast<float>(impl.window.height());
  const auto scene_selection_token = [&]() noexcept {
    core::u64 hash = 1469598103934665603ULL;
    const auto append = [&hash](core::u64 value) noexcept {
      for (unsigned shift = 0U; shift < 64U; shift += 8U) {
        hash ^= static_cast<core::u8>(value >> shift);
        hash *= 1099511628211ULL;
      }
    };
    append(impl.scene_selection.primary().value());
    for (const doc::NodeId node : impl.scene_selection.members()) {
      append(node.value());
    }
    return hash;
  };
  const auto begin_semantic_nudge =
      [&](const NumericNudge &nudge, HistoryContext context,
          core::u64 selection) noexcept {
        const core::u64 gesture = impl.next_edit_gesture++;
        result.semantic_coalescing = SemanticCoalescingRequest{
            context,
            SemanticCoalescingKey{
                nudge.focus, selection, nudge.property,
                nudge.direction == NumberFieldNudgeDirection::Increase
                    ? 1U
                    : 2U,
                static_cast<core::u64>(nudge.modifiers)},
            gesture};
        return gesture;
      };

  // Preferences is modal. It has to be drawn last to sit on top, so the only
  // way it can also own the pointer is to deny it to everything drawn before.
  // The block is lifted immediately before the dialog runs. Whether it was
  // taken is remembered rather than re-tested, because the File menu can open
  // the dialog partway through this same frame.
  const bool preferences_blocked_pointer =
      impl.preferences_panel.is_open() ||
      impl.project_settings_panel.is_open() || impl.scene_link_details_open || impl.blender_connection_open;
  if (preferences_blocked_pointer) {
    ui.push_pointer_block();
  }

  const auto delete_selection = [&]() {
    if (!impl.has_selection()) {
      return;
    }
    const std::span<const doc::NodeId> selected =
        impl.scene_selection.members();
    for (const doc::NodeId node : selected) {
      const auto* element = find_element(elements,node);
      if (node != project_settings.stage_node && (!element || !element->linked_collection)) result.deletes.push_back(node);
    }
    if (result.deletes.empty()) return;
    result.delete_gesture = impl.next_edit_gesture++;
    impl.scene_selection.clear();
    impl.semantic_boundary = true;
  };

  const HeaderActions header =
      draw_application_header(ui, width, switcher, impl.open_menu, project);
  const ApplicationMenuResult menu_result =
      run_application_menu_input(ui, width, height, impl.open_menu,
                                 impl.recent_projects_submenu_open, impl.docks,
                                 project.recent_projects);
  // Menu rows overlay scene controls. Consume both press and selection frames
  // so starting a link cannot also arm the scene-delete control underneath it.
  const bool application_menu_blocks_pointer = impl.open_menu != ApplicationMenu::None
      || menu_result.action != ApplicationMenuAction::None;
  if (application_menu_blocks_pointer) ui.push_pointer_block();
  switch (menu_result.action) {
  case ApplicationMenuAction::ImportLinkedScene: {
    const char* filters[] = {"*.mslink"};
    const char* path = tinyfd_openFileDialog("Import Linked Scene Manifest", "", 1, filters, "Linked scene manifest", 0);
    if (path) {
      result.import_linked_scene = ProjectFileRequest{path};
      result.linked_scene_import_gesture = impl.next_edit_gesture++;
    }
    break;
  }
  case ApplicationMenuAction::CancelLinkedSceneImport:
    result.cancel_linked_scene_import = true;
    break;
  case ApplicationMenuAction::StartSceneLink:
    impl.blender_connection_open = true;
    ui.clear_focus();
    break;
  case ApplicationMenuAction::StopSceneLink:
    result.stop_scene_link = true;
    break;
  case ApplicationMenuAction::SceneLinkDetails: {
    impl.scene_link_details_open = true;
    ui.clear_focus();
    break;
  }
  case ApplicationMenuAction::NewProject:
    if (!project.dirty || confirm_discard_project_changes(
                              "those changes and start a new project")) {
      result.new_project = true;
    }
    break;
  case ApplicationMenuAction::OpenProject:
    if (auto path = choose_project_to_open(); path.has_value()) {
      if (!project.dirty || confirm_discard_project_changes(
                                "those changes and open another project")) {
        result.load_project = ProjectFileRequest{std::move(*path)};
      }
    }
    break;
  case ApplicationMenuAction::RecentProjects:
    break;
  case ApplicationMenuAction::OpenRecentProject:
    if (!project.dirty || confirm_discard_project_changes(
                              "those changes and open another project")) {
      result.load_project = ProjectFileRequest{menu_result.project_path};
    }
    break;
  case ApplicationMenuAction::SaveProject:
    if (!project.path.empty()) {
      result.save_project = ProjectFileRequest{std::string{project.path}};
    } else if (auto path = choose_project_to_save(project.path);
               path.has_value()) {
      result.save_project = ProjectFileRequest{std::move(*path)};
    }
    break;
  case ApplicationMenuAction::SaveProjectAs:
    if (auto path = choose_project_to_save(project.path); path.has_value()) {
      result.save_project = ProjectFileRequest{std::move(*path)};
    }
    break;
  case ApplicationMenuAction::OpenPreferences:
    // Seeded from the stored values every time, so cancelling really discards.
    impl.preferences_panel.open(preferences.stored);
    break;
  case ApplicationMenuAction::OpenProjectSettings:
    impl.project_settings_panel.open(project_settings.settings,
                                     project_settings.renderer,
                                     project_settings.lens_profile,
                                     project_settings.environment);
    break;
  case ApplicationMenuAction::Undo:
    if (!impl.manipulating) {
      result.history = HistoryRequest{HistoryAction::Undo,
                                      impl.history_context,
                                      impl.next_edit_gesture++};
    }
    break;
  case ApplicationMenuAction::Redo:
    if (!impl.manipulating) {
      result.history = HistoryRequest{HistoryAction::Redo,
                                      impl.history_context,
                                      impl.next_edit_gesture++};
    }
    break;
  case ApplicationMenuAction::Copy:
  case ApplicationMenuAction::Paste:
    // Clipboard routing happens after panels have reported their typed domain.
    break;
  case ApplicationMenuAction::TogglePanel:
    if (impl.docks.is_open(menu_result.panel)) {
      impl.docks.close(menu_result.panel);
    } else if (impl.docks.show(menu_result.panel) == gui::no_dock) {
      impl.show_panel(menu_result.panel);
    }
    break;
  case ApplicationMenuAction::ZoomIn:
  case ApplicationMenuAction::ZoomOut:
    // Lands on the next frame, before anything is laid out at the new size.
    impl.ui_zoom_percent = next_ui_zoom(
        impl.ui_zoom_percent,
        menu_result.action == ApplicationMenuAction::ZoomIn);
    break;
  case ApplicationMenuAction::ZoomReset:
    impl.ui_zoom_percent = 100U;
    break;
  case ApplicationMenuAction::ResetWorkspace:
    impl.reset_workspace();
    break;
  case ApplicationMenuAction::None:
    break;
  }
  if (header.open_setup) {
    impl.show_setup();
  }
  if (header.program && switcher.rehearsal_mode &&
      !switcher.rehearsal_locked) {
    result.switcher = SwitcherRequest{SwitcherAction::ToggleRehearse};
  }
  if (header.preview && !switcher.rehearsal_mode &&
      !switcher.rehearsal_locked) {
    result.switcher = SwitcherRequest{SwitcherAction::ToggleRehearse};
  }
  if (header.swap) {
    // Two scenes on different canvases cannot be dissolved through one shared
    // output slice, so promoting them is a cut. The button says CUT in that
    // case; asking for a dissolve the Director would refuse is the behaviour
    // that used to make Swap look broken.
    const bool cut_only = switcher.rehearsal_scene != scene::SceneId{} &&
                          !switcher.dissolve_compatible;
    result.switcher =
        SwitcherRequest{SwitcherAction::Swap, {}, cut_only ? 0U : 30U};
  }

  const auto capture_group = [&](const scene::Scene &authored_scene,
                                 const scene::Scene &display_scene) {
    impl.group_nodes.clear();
    impl.group_start.clear();
    impl.group_delta_to_local.clear();
    impl.group_transform.clear();
    impl.group_linked.clear();
    impl.group_authored_world.clear();
    impl.group_display_adjustment.clear();
    for (const doc::NodeId node : impl.scene_selection.members()) {
      const auto authored_world = scene::world_for(authored_scene, node);
      const auto displayed_world = scene::world_for(display_scene, node);
      if (!authored_world.has_value() || !displayed_world.has_value()) {
        continue;
      }

      scene::Mat4 authored_parent = scene::identity();
      const ElementInfo *const element = find_element(elements, node);
      if (element != nullptr && element->parent_group.has_value()) {
        const auto parent =
            scene::world_for(authored_scene, *element->parent_group);
        if (parent.has_value()) {
          authored_parent = *parent;
        }
      }

      const scene::Mat4 authored_parent_inverse =
          scene::invert_affine(authored_parent);
      const scene::Vec3 local_start = scene::transform_point(
          authored_parent_inverse,
          scene::translation_of(*authored_world));
      const scene::Mat4 display_adjustment = scene::multiply(
          *displayed_world, scene::invert_affine(*authored_world));
      const scene::Mat4 displayed_parent =
          scene::multiply(display_adjustment, authored_parent);

      impl.group_nodes.push_back(node);
      impl.group_start.push_back(local_start);
      impl.group_delta_to_local.push_back(
          scene::invert_affine(displayed_parent));

      TransformStart start{};
      start.local_position = local_start;
      start.world_position = scene::translation_of(*displayed_world);
      start.world_to_local = impl.group_delta_to_local.back();
      for (std::size_t axis = 0U; axis < start.world_axes.size(); ++axis) {
        const auto column = static_cast<core::u32>(axis);
        start.world_axes[axis] = scene::normalize(
            scene::Vec3{displayed_world->at(0U, column),
                        displayed_world->at(1U, column),
                        displayed_world->at(2U, column)},
            start.world_axes[axis]);
      }
      if (element != nullptr) {
        const ElementInfo evaluated =
            evaluated_element(*element, keyframes, timeline.show_frame);
        start.rotation = evaluated.rotation;
        start.scale = evaluated.scale;
      }
      impl.group_transform.push_back(start);
      impl.group_linked.push_back(linked_occurrence(element));
      impl.group_authored_world.push_back(*authored_world);
      impl.group_display_adjustment.push_back(display_adjustment);
    }
  };

  // One transform component group (position, rotation or scale) written the
  // way the Inspector writes it: a key at the show frame on a component that
  // is animated, the base value on one that is not.
  const auto emit_components = [&](doc::NodeId node,
                                   scene::TransformChannel first,
                                   scene::Vec3 values, core::u64 gesture) {
    const std::array<double, 3U> components{values.x, values.y, values.z};
    for (std::size_t index = 0U; index < components.size(); ++index) {
      const auto component = static_cast<scene::TransformChannel>(
          static_cast<core::u8>(static_cast<std::size_t>(first) + index));
      if (impl.timeline_panel.auto_key_enabled() &&
          keyed(keyframes, node, component)) {
        result.keys.push_back(KeyIntent{node, component, timeline.show_frame,
                                        components[index], false, gesture});
      } else {
        result.property_edits.push_back(
            PropertyEdit{node, component, components[index], gesture});
      }
    }
  };

  // Lands one viewport move, rotate or scale on every captured element. A
  // rotate or scale orbits the elements about the shared pivot, so each one
  // off the pivot also moves; one on it keeps its position untouched rather
  // than gaining a key it never asked for.
  const auto emit_transform = [&](const TransformDelta &delta,
                                  core::u64 gesture) {
    for (std::size_t index = 0U; index < impl.group_nodes.size(); ++index) {
      const doc::NodeId node = impl.group_nodes[index];
      const TransformStart &start = impl.group_transform[index];
      const TransformResult moved = apply_transform(start, delta);
      const ElementInfo *const element = find_element(elements, node);
      if (delta.tool != TransformTool::Move && impl.group_linked[index]) {
        // The turn or resize as the whole matrix it produces, mapped from
        // what the Stage draws back to the authored placement.
        const scene::Mat4 &adjustment = impl.group_display_adjustment[index];
        const scene::Mat4 displayed =
            scene::multiply(adjustment, impl.group_authored_world[index]);
        result.world_edits.push_back(WorldEdit{
            node,
            scene::multiply(scene::invert_affine(adjustment),
                            transformed_world(displayed, delta)),
            gesture});
        continue;
      }
      const bool orbits =
          scene::length(start.world_position - delta.pivot) > 1.0e-5F;
      if (delta.tool == TransformTool::Move || orbits) {
        const bool position_keyed = impl.timeline_panel.auto_key_enabled() && (
            keyed(keyframes, node, scene::TransformChannel::PositionX) ||
            keyed(keyframes, node, scene::TransformChannel::PositionY) ||
            keyed(keyframes, node, scene::TransformChannel::PositionZ));
        if (!position_keyed) {
          result.edits.push_back(EditIntent{node, moved.position, gesture});
        } else {
          emit_components(node, scene::TransformChannel::PositionX,
                          moved.position, gesture);
        }
      }
      // Lights are placed, not turned or sized, from the viewport; a camera
      // turns but has no size.
      if (element == nullptr || is_light(element->kind)) {
        continue;
      }
      if (delta.tool == TransformTool::Rotate) {
        emit_components(node, scene::TransformChannel::RotationYaw,
                        moved.rotation, gesture);
      } else if (delta.tool == TransformTool::Scale &&
                 element->kind != ElementKind::Camera) {
        emit_components(node, scene::TransformChannel::ScaleX, moved.scale,
                        gesture);
      }
    }
  };

  // Alt+G / Alt+R / Alt+S over the Stage: Blender's clear location, rotation
  // and scale, as one undo step for the whole selection.
  const auto clear_transform = [&](TransformTool tool,
                                   const scene::Scene &authored_scene) {
    const core::u64 gesture = impl.next_edit_gesture++;
    impl.history_context = HistoryContext::Scene;
    for (const doc::NodeId node : impl.scene_selection.members()) {
      const ElementInfo *const element = find_element(elements, node);
      if (element == nullptr || element->locked) {
        continue;
      }
      if (tool != TransformTool::Move && linked_occurrence(element) &&
          !is_light(element->kind)) {
        // Unturned keeps each axis's length; unit scale keeps its direction.
        const auto world = scene::world_for(authored_scene, node);
        if (!world.has_value()) {
          continue;
        }
        scene::Mat4 cleared = *world;
        for (core::u32 column = 0U; column < 3U; ++column) {
          const scene::Vec3 axis{world->at(0U, column), world->at(1U, column),
                                 world->at(2U, column)};
          scene::Vec3 replacement{};
          if (tool == TransformTool::Rotate) {
            replacement = with_axis(column, scene::length(axis));
          } else {
            replacement = scene::normalize(axis, with_axis(column, 1.0F));
          }
          cleared.set(0U, column, replacement.x);
          cleared.set(1U, column, replacement.y);
          cleared.set(2U, column, replacement.z);
        }
        result.world_edits.push_back(WorldEdit{node, cleared, gesture});
        continue;
      }
      switch (tool) {
      case TransformTool::Move:
        if (impl.timeline_panel.auto_key_enabled() && (
            keyed(keyframes, node, scene::TransformChannel::PositionX) ||
            keyed(keyframes, node, scene::TransformChannel::PositionY) ||
            keyed(keyframes, node, scene::TransformChannel::PositionZ))) {
          emit_components(node, scene::TransformChannel::PositionX,
                          scene::Vec3{}, gesture);
        } else {
          result.edits.push_back(EditIntent{node, scene::Vec3{}, gesture});
        }
        break;
      case TransformTool::Rotate:
        if (!is_light(element->kind)) {
          emit_components(node, scene::TransformChannel::RotationYaw,
                          scene::Vec3{}, gesture);
        }
        break;
      case TransformTool::Scale:
        if (!is_light(element->kind) && element->kind != ElementKind::Camera) {
          emit_components(node, scene::TransformChannel::ScaleX,
                          scene::Vec3{1.0F, 1.0F, 1.0F}, gesture);
        }
        break;
      }
    }
  };

  // Projection surfaces are physical stage geometry belonging to the show, not
  // to the scene on the Stage, and the feed map they live in is drawn at the
  // Program playhead. Key them there too, or a surface dragged while Preview
  // sits at a different frame would author a key the operator cannot see.
  const auto route_surface_edit = [&](FeedSurfaceEditRequest request) {
    RoutedSurfaceEdit routed = route_surface_transform_edit(
        std::move(request), keyframes, timeline.program_frame,
        impl.timeline_panel.auto_key_enabled());
    result.keys.insert(result.keys.end(),
                       std::make_move_iterator(routed.keys.begin()),
                       std::make_move_iterator(routed.keys.end()));
    if (routed.base.has_value()) {
      result.feed_surface_edits.push_back(std::move(routed.base.value()));
    }
  };

  const auto enqueue_model = [&](std::string source, scene::SceneId add_scene,
      std::optional<FeedSurfaceAddRequest> surface_add,
      std::optional<FeedSurfaceEditRequest> surface_edit,
      doc::NodeId folder = {}, doc::NodeId revise = {},
      std::optional<core::u64> gesture = std::nullopt) {
    if (impl.pending_model_ingests.size() >= 16U) {
      impl.ingest_message = "Model import queue is full"; return;
    }
    std::optional<output::PhysicalSurface> calibration;
    float scale = 1.0F;
    if (surface_add) { calibration = surface_add->surface; scale = surface_add->mesh_scale; }
    if (surface_edit) { calibration = surface_edit->surface; scale = surface_edit->mesh_scale; }
    auto job = render::ModelIngestJob::start(source, calibration, scale);
    if (!job) { impl.ingest_message = "Model import worker is busy; retry import"; return; }
    if (surface_edit) {
      std::erase_if(impl.pending_model_ingests, [&](const auto& held) {
        return held.surface_edit && held.surface_edit->node == surface_edit->node;
      });
    }
    impl.pending_model_ingests.push_back({std::move(job),
        std::filesystem::path{source}.filename().string(),
        gesture.has_value() ? *gesture : impl.next_edit_gesture++,
        project.generation, add_scene, std::move(surface_add), std::move(surface_edit),
        {}, {}, folder, revise});
    impl.ingest_message.clear();
  };
  for (auto pending = impl.pending_model_ingests.begin(); pending != impl.pending_model_ingests.end();) {
    const auto* completed = pending->job->result();
    if (!completed) { ++pending; continue; }
    if (pending->project_generation == project.generation) {
      if (pending->texture_target.value() != 0U) {
        const auto* current = find_element(elements, pending->texture_target);
        if (!current || current->source != pending->expected_source || current->locked || current->linked) {
          impl.ingest_message = "Model changed during texture preparation; retry the assignment";
          pending = impl.pending_model_ingests.erase(pending);
          continue;
        }
      }
      // Projection validation used a specific calibration. Do not overwrite a
      // surface edited or deleted while the worker was reading the model.
      if (pending->surface_edit) {
        const auto& expected = *pending->surface_edit;
        const auto current = std::find_if(feed_map.surfaces.begin(), feed_map.surfaces.end(),
            [&](const auto& surface) { return surface.node == expected.node; });
        if (current == feed_map.surfaces.end() || current->geometry != expected.surface
            || current->mesh_scale != expected.mesh_scale) {
          impl.ingest_message = "Surface changed during import; retry UV Mesh Browse";
          pending = impl.pending_model_ingests.erase(pending);
          continue;
        }
      }
      if (!completed->diagnostic.empty()) {
        impl.ingest_message = completed->diagnostic;
      } else if (pending->revise != doc::NodeId{}) {
        // An accepted change: the same model asset, re-read.
        assets::ModelAsset asset{};
        asset.id = pending->revise;
        asset.display_name = pending->display_name;
        asset.source = completed->source;
        asset.vertex_count = completed->vertex_count;
        asset.triangle_count = completed->triangle_count;
        asset.material_count = completed->material_count;
        asset.texture_count = completed->texture_count;
        if (completed->largest_texture_width > 0U &&
            completed->largest_texture_height > 0U) {
          asset.largest_texture = assets::PixelDimensions{
              completed->largest_texture_width,
              completed->largest_texture_height};
        }
        const auto& bounds = completed->bounds;
        asset.bounds = assets::ModelBounds{bounds.minimum.x, bounds.minimum.y, bounds.minimum.z,
            bounds.maximum.x, bounds.maximum.y, bounds.maximum.z};
        impl.pending_revisions.push_back(
            AssetRevisionIntent{assets::Asset{std::move(asset)}, pending->gesture});
      } else {
        assets::ModelAsset asset{};
        asset.display_name = pending->display_name;
        asset.source = completed->source;
        asset.vertex_count = completed->vertex_count;
        asset.triangle_count = completed->triangle_count;
        asset.material_count = completed->material_count;
        asset.texture_count = completed->texture_count;
        if (completed->largest_texture_width > 0U &&
            completed->largest_texture_height > 0U) {
          asset.largest_texture = assets::PixelDimensions{
              completed->largest_texture_width,
              completed->largest_texture_height};
        }
        asset.folder = pending->folder;
        const auto& bounds = completed->bounds;
        asset.bounds = assets::ModelBounds{bounds.minimum.x, bounds.minimum.y, bounds.minimum.z,
            bounds.maximum.x, bounds.maximum.y, bounds.maximum.z};
        asset.projection_mesh = pending->surface_add.has_value() || pending->surface_edit.has_value();
        if (pending->add_scene != scene::SceneId{}) {
          result.adds.push_back(AddIntent{AddKind::Model, completed->source, 1U,
              pending->gesture, 16.0F / 9.0F, {}, pending->add_scene});
        } else {
          result.asset_imports.push_back({assets::Asset{std::move(asset)}, pending->gesture});
        }
        if (pending->texture_target.value() != 0U) {
          result.generic_edits.push_back(GenericEdit{pending->texture_target,
              std::string{scene::schema::source}, 0.0, pending->gesture, completed->source});
        }
        if (pending->surface_add) {
          auto request = std::move(*pending->surface_add);
          request.mesh_source = completed->source;
          request.name = projection_mesh_name(pending->display_name);
          request.gesture = pending->gesture;
          result.feed_surface_adds.push_back(std::move(request));
        }
        if (pending->surface_edit) {
          auto request = std::move(*pending->surface_edit);
          request.mesh_source = completed->source;
          request.name = projection_mesh_name(pending->display_name);
          request.shape = output::ProjectionSurfaceShape::ImportedMesh;
          request.gesture = pending->gesture;
          result.feed_surface_edits.push_back(std::move(request));
        }
        impl.ingest_message = "Model import complete";
      }
    }
    pending = impl.pending_model_ingests.erase(pending);
  }

  // Accepted changes go to the Director once every probe their gesture
  // started has finished, so one Accept is one Undo.
  if (!impl.pending_revisions.empty()) {
    std::erase_if(impl.pending_revisions, [&](AssetRevisionIntent &revision) {
      const core::u64 gesture = revision.gesture;
      const bool waiting =
          std::any_of(impl.pending_media_probes.begin(),
                      impl.pending_media_probes.end(),
                      [gesture](const Impl::PendingMediaProbe &probe) {
                        return probe.gesture == gesture;
                      }) ||
          std::any_of(impl.pending_model_ingests.begin(),
                      impl.pending_model_ingests.end(),
                      [gesture](const Impl::PendingModelIngest &ingest) {
                        return ingest.gesture == gesture;
                      });
      if (waiting) {
        return false;
      }
      result.asset_revisions.push_back(std::move(revision));
      return true;
    });
  }

  const auto handle_viewport = [&](const ViewportFrame &viewport,
                                   const scene::Scene &authored_view,
                                   const scene::Scene &display_view) {
    if (viewport.pressed) {
      if (viewport.surface_picked.has_value()) {
        impl.stage_surface_selection = viewport.surface_picked;
        impl.feed_mapping_panel.select_surface(*viewport.surface_picked);
        impl.scene_selection.clear();
        impl.group_nodes.clear();
        impl.group_start.clear();
        impl.delete_domain = DeleteDomain::Surface;
      } else if (viewport.picked.has_value()) {
        impl.stage_surface_selection.reset();
        // A viewport re-click can keep the same selection, so it may bypass
        // select_only()/toggle(). It is still an explicit choice of the Scene
        // history domain for the global undo/redo actions.
        impl.history_context = HistoryContext::Scene;
        const doc::NodeId picked = viewport.picked.value();
        if (ui.ctrl_held()) {
          impl.toggle(picked);
        } else if (ui.shift_held() || !impl.is_selected(picked)) {
          // A spatial viewport has no stable visible row order, so Shift is a
          // plain replacement here. Only Ctrl is the exact membership toggle.
          impl.select_only(picked);
        } else {
          impl.scene_selection.set_primary(picked);
        }
        if (impl.is_selected(picked)) {
          capture_group(authored_view, display_view);
        } else {
          impl.group_nodes.clear();
          impl.group_start.clear();
        }
      } else if (viewport.empty_canvas == EmptyCanvasInteraction::Navigate) {
        // Stage bare canvas is camera space. Preserve every selection domain;
        // the drag is camera navigation and must not masquerade as
        // deselection.
      } else if (viewport.empty_canvas ==
                 EmptyCanvasInteraction::ClearSelection) {
        impl.stage_surface_selection.reset();
        impl.clear_scene_selection(true);
        impl.group_nodes.clear();
        impl.group_start.clear();
      }
    }
    if (viewport.transform_began) {
      impl.history_context = HistoryContext::Scene;
      capture_group(authored_view, display_view);
    }
    if (viewport.transform.has_value() && !impl.group_nodes.empty()) {
      emit_transform(*viewport.transform, impl.begin_gesture());
    }
    if (viewport.released || viewport.transform_ended) {
      impl.group_nodes.clear();
      impl.group_start.clear();
    }
  };

  // Stage camera navigation, as in the Unreal Editor's perspective viewport.
  // `pointer_free` says no floating panel covers the pointer; `left_available`
  // that a left press on bare canvas may walk (mask authoring owns it).
  const auto navigate_stage_camera = [&](const ViewportFrame &viewport,
                                         bool pointer_free,
                                         bool left_available) {
    const bool already = impl.navigate_left || impl.navigate_middle ||
                         impl.navigate_right;
    const bool over =
        pointer_free && viewport.image.contains(ui.pointer_x(), ui.pointer_y());
    const bool left_press = left_available && viewport.pressed &&
                            viewport.empty_canvas ==
                                EmptyCanvasInteraction::Navigate;
    // A left drag that picked an element is that element's; a right or
    // middle press then leaves the camera alone.
    const bool element_drag =
        viewport.held && !impl.navigate_left && !left_press;
    if ((over || already) && !element_drag) {
      impl.navigate_right = impl.navigate_right || ui.secondary_pointer_pressed();
      impl.navigate_middle = impl.navigate_middle || ui.middle_pointer_pressed();
      impl.navigate_left = impl.navigate_left || left_press;
    }
    impl.navigate_left = impl.navigate_left && ui.pointer_down();
    impl.navigate_middle = impl.navigate_middle && ui.middle_pointer_down();
    impl.navigate_right = impl.navigate_right && ui.secondary_pointer_down();

    const double now = impl.window.time_seconds();
    const float seconds =
        std::clamp(static_cast<float>(now - impl.navigation_time), 0.0F, 0.1F);
    impl.navigation_time = now;
    const StageNavigation gesture = stage_navigation_for(StageNavigationButtons{
        impl.navigate_left, impl.navigate_middle, impl.navigate_right,
        ui.alt_held(), ui.shift_held()});
    if (gesture == StageNavigation::None) {
      impl.navigation_moved = false;
      if (over && ui.wheel() != 0.0F) {
        impl.stage_view = stage_wheel_dolly(impl.stage_view, ui.wheel());
      }
      // F frames the selection, as everywhere in Unreal.
      const std::string_view typed = ui.shortcut_text();
      if (over && !impl.scene_selection.empty() &&
          typed.find_first_of("fF") != std::string_view::npos) {
        if (const auto bounds = impl.stage_panel.world_bounds(
                stage_interaction_scene, impl.scene_selection.members())) {
          impl.stage_view = stage_focus(impl.stage_view, *bounds);
        }
      }
    } else {
      StageNavigationStep step{};
      step.gesture = gesture;
      step.dx = ui.pointer_motion_x();
      step.dy = ui.pointer_motion_y();
      step.seconds = seconds;
      step.units_per_pixel =
          stage_units_per_pixel(impl.stage_view, viewport.image.height);
      if (gesture == StageNavigation::Look) {
        const auto axis = [&](gui::MoveKey positive, gui::MoveKey negative) {
          return (ui.move_key_down(positive) ? 1.0F : 0.0F) -
                 (ui.move_key_down(negative) ? 1.0F : 0.0F);
        };
        step.move = scene::Vec3{axis(gui::MoveKey::D, gui::MoveKey::A),
                                axis(gui::MoveKey::E, gui::MoveKey::Q),
                                axis(gui::MoveKey::W, gui::MoveKey::S)};
        if (ui.wheel() != 0.0F) {
          impl.fly_speed = stage_fly_speed_after(impl.fly_speed, ui.wheel());
          impl.fly_speed_shown_until = now + 1.5;
        }
      }
      step.fly_speed = impl.fly_speed;
      impl.navigation_moved = impl.navigation_moved || step.dx != 0.0F ||
                              step.dy != 0.0F || step.move != scene::Vec3{};
      impl.stage_view = navigate_stage(impl.stage_view, step);
    }

    if (now < impl.fly_speed_shown_until) {
      char text[48]{};
      std::snprintf(text, sizeof(text), "CAMERA SPEED %.2f", impl.fly_speed);
      const gui::Rect badge{viewport.image.x + 8.0F,
                            viewport.image.y + viewport.image.height - 32.0F,
                            156.0F, 24.0F};
      ui.panel(badge, ui.theme().background);
      outline(ui, badge, ui.theme().divider);
      ui.label(gui::Rect{badge.x + 8.0F, badge.y, badge.width - 16.0F,
                         badge.height},
               text, ui.theme().text);
    }
  };

  const auto add_simple = [&](AddKind kind) {
    result.adds.push_back(AddIntent{kind,
                                    std::string{},
                                    1U,
                                    impl.next_edit_gesture++,
                                    16.0F / 9.0F,
                                    {},
                                    authored_stage.id});
  };

  const auto handle_library_add = [&](LibraryAddAction action) {
    switch (action) {
    case LibraryAddAction::Box:
      add_simple(AddKind::Box);
      return;
    case LibraryAddAction::Plane:
      add_simple(AddKind::Plane);
      return;
    case LibraryAddAction::Sphere:
      add_simple(AddKind::Sphere);
      return;
    case LibraryAddAction::DirectionalLight:
      add_simple(AddKind::DirectionalLight);
      return;
    case LibraryAddAction::PointLight:
      add_simple(AddKind::PointLight);
      return;
    case LibraryAddAction::SpotLight:
      add_simple(AddKind::SpotLight);
      return;
    case LibraryAddAction::Camera:
      add_simple(AddKind::Camera);
      return;
    case LibraryAddAction::Media: {
      const char *const filters[]{"*.mp4", "*.mov", "*.mkv", "*.webm",
                                  "*.avi", "*.mxf", "*.m4v", "*.mpg", "*.mpeg", "*.wmv",
                                  "*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tga", "*.exr", "*.hdr"};
      const char *const path = tinyfd_openFileDialog(
          "Add Media", "", static_cast<int>(std::size(filters)), filters, "Video and images", 0);
      if (path != nullptr) {
        const std::string source{path};
        try {
          impl.pending_media_probes.push_back(Impl::PendingMediaProbe{
              Impl::PendingMediaAction::Add, source, {}, false,
              impl.next_edit_gesture++, authored_stage.id,
              project.generation});
        } catch (...) {
          result.adds.push_back(AddIntent{
              AddKind::Media, source, 1U, impl.next_edit_gesture++,
              16.0F / 9.0F, {}, authored_stage.id});
        }
      }
      return;
    }
    case LibraryAddAction::Model: {
      const char *const filters[]{"*.gltf", "*.glb", "*.obj", "*.msmodel"};
      const char *const path = tinyfd_openFileDialog(
          "Add 3D Model", "", 4, filters, "3D models (glTF, OBJ, model package)", 0);
      if (path != nullptr) {
        enqueue_model(std::string{path}, authored_stage.id, {}, {});
      }
      return;
    }
    }
  };

  // Files become Library assets in `folder`: video and images are probed,
  // models go through the model worker, and anything else is named in the
  // status line rather than silently dropped.
  const auto import_files = [&](std::span<const std::string> sources,
                                doc::NodeId folder) {
    const core::u64 gesture = impl.next_edit_gesture++;
    std::size_t refused = 0U;
    for (const std::string &source : sources) {
      const std::optional<LibraryImportAction> kind =
          library_import_kind(source);
      if (kind == LibraryImportAction::Media) {
        try {
          impl.pending_media_probes.push_back(Impl::PendingMediaProbe{
              Impl::PendingMediaAction::Import, source, {}, false, gesture, {},
              project.generation, folder});
        } catch (...) {
          assets::MediaAsset asset{};
          asset.display_name =
              std::filesystem::path{source}.filename().string();
          asset.source = source;
          asset.folder = folder;
          result.asset_imports.push_back(AssetImportIntent{
              assets::Asset{std::move(asset)}, gesture});
        }
      } else if (kind == LibraryImportAction::Model) {
        enqueue_model(source, {}, {}, {}, folder);
      } else {
        ++refused;
      }
    }
    if (refused > 0U) {
      impl.ingest_message =
          refused == 1U ? "Not a media or model file; nothing imported"
                        : std::to_string(refused) +
                              " files are not media or models; skipped";
    }
  };

  const auto import_asset = [&](LibraryImportAction action,
                                doc::NodeId folder) {
    const bool media = action == LibraryImportAction::Media;
    const char *const media_filters[]{"*.mp4", "*.mov", "*.mkv", "*.webm",
                                      "*.avi", "*.mxf", "*.m4v", "*.mpg", "*.mpeg", "*.wmv",
                                      "*.png", "*.jpg", "*.jpeg", "*.bmp", "*.tga", "*.exr", "*.hdr"};
    const char *const model_filters[]{"*.gltf", "*.glb", "*.obj", "*.msmodel"};
    const char *const path = tinyfd_openFileDialog(
        media ? "Import Media Asset" : "Import 3D Model Asset", "",
        media ? static_cast<int>(std::size(media_filters)) : 4, media ? media_filters : model_filters,
        media ? "Video and images" : "3D models (glTF, OBJ)", 1);
    if (path == nullptr) {
      return;
    }
    const std::vector<std::string> sources = selected_dialog_paths(path);
    import_files(sources, folder);
  };

  const auto instantiate_asset = [&](doc::NodeId id,
                                     std::optional<scene::Vec3> position,
                                     std::optional<CompositorAddIntent>
                                         graph_layer = std::nullopt) {
    const assets::Asset *const found = asset_registry.asset(id);
    if (found == nullptr) {
      return;
    }
    if (const auto *const media = std::get_if<assets::MediaAsset>(found)) {
      float aspect = render::probe_media_aspect(media->source);
      if (media->dimensions.has_value()) {
        aspect = static_cast<float>(media->dimensions->width) /
                 static_cast<float>(media->dimensions->height);
      }
      result.adds.push_back(AddIntent{AddKind::Media, media->source,
                                      media->frame_count.value_or(1U),
                                      impl.next_edit_gesture++, aspect,
                                      media->id, authored_stage.id, position,
                                      media->frame_rate, graph_layer});
      return;
    }
    const auto &model = std::get<assets::ModelAsset>(*found);
    result.adds.push_back(AddIntent{AddKind::Model, model.source, 1U,
                                    impl.next_edit_gesture++, 16.0F / 9.0F,
                                    model.id, authored_stage.id, position,
                                    std::nullopt, graph_layer});
  };

  const auto draw_placeholder = [&](const gui::Rect &bounds,
                                    std::string_view title,
                                    std::span<const std::string_view> rows) {
    ui.panel(bounds, ui.theme().panel);
    ui.push_clip(bounds);
    ui.label(gui::Rect{bounds.x + 10.0F, bounds.y + 6.0F,
                       std::max(bounds.width - 20.0F, 0.0F), 22.0F},
             title, ui.theme().text);
    float y = bounds.y + 34.0F;
    for (std::size_t index = 0U; index < rows.size(); ++index) {
      const gui::Rect row{bounds.x + 8.0F, y,
                          std::max(bounds.width - 16.0F, 0.0F), 21.0F};
      ui.panel(row, index % 2U == 0U ? ui.theme().row : ui.theme().row_alt);
      ui.panel(gui::Rect{row.x, row.y, 2.0F, row.height},
               index == 0U ? ui.theme().accent : ui.theme().header);
      ui.label(gui::Rect{row.x + 9.0F, row.y, row.width - 9.0F, row.height},
               rows[index],
               index == 0U ? ui.theme().text : ui.theme().text_muted);
      y += 22.0F;
    }
    ui.pop_clip();
  };

  bool delete_consumed = false;
  bool clipboard_consumed = false;
  bool editor_control_editing = false;
  ViewportFrame stage_viewport_frame{};
  bool stage_viewport_drawn = false;
  std::optional<LibraryAssetDrop> pending_asset_drop;
  std::optional<doc::NodeId> dragging_asset;
  // Files dragged in from the desktop that the Library imported itself.
  bool library_took_files = false;
  std::optional<EffectApplyRequest> pending_effect_drop;
  std::optional<TimelineLayerDrop> pending_timeline_layer_drop;
  gui::Rect compositor_panel_bounds{};
  bool compositor_panel_drawn = false;
  bool timeline_panel_drawn = false;
  gui::Rect timeline_panel_bounds{};
  gui::Rect inspector_panel_bounds{};
  bool inspector_panel_drawn = false;
  SceneSettingsFrame live_scene_settings;
  std::optional<InspectorPanelRequest> pending_inspector_panel;
  const bool color_sample_active = impl.color_sample_request.has_value();
  const ElementInfo *const mask_target_element =
      find_element(elements, impl.anchor());
  const bool mask_target_supported =
      mask_target_element != nullptr &&
      !is_light(mask_target_element->kind);
  const MasksPanelTarget mask_target{
      authored_stage.id,
      mask_target_supported ? mask_target_element->node : doc::NodeId{},
      mask_target_element != nullptr &&
              mask_target_element->kind == ElementKind::Group
          ? scene::MaskCoordinateSpace::AuthoredCanvas
          : scene::MaskCoordinateSpace::SourceLayer};
  const bool mask_authoring_enabled =
      mask_target_supported && !mask_target_element->locked &&
      !switcher.rehearsal_locked;
  // Stage switches interaction domains only while Masks is the visible tab
  // and a real target is selected. With no target it stays an ordinary Stage,
  // so the operator can click a drawable and enter mask mode on the next
  // projected frame.
  const bool mask_stage_active =
      impl.dock_host.visible(panel_masks) && mask_target_supported;
  // Program Output shows the picture that is actually on the wall, which is
  // where a matte in the wrong place is noticed, so it authors masks too. It
  // may only do so while it and the Stage are the same scene: with Preview on
  // the Stage the two panels show different scenes, and a mask dragged over a
  // layer the picture does not contain would be authored somewhere nobody is
  // looking. A running dissolve is excluded outright - the picture is a mix of
  // two committed scenes and belongs to neither.
  const bool mask_program_active =
      mask_stage_active && authored_stage.id == switcher.program_scene &&
      !switcher.transition_active;
  const auto queue_mask_edit = [&](MaskEditRequest request) {
    result.mask_edit =
        MaskEditIntent{std::move(request), impl.begin_gesture()};
    impl.history_context = HistoryContext::Scene;
    impl.semantic_boundary = true;
  };
  const auto run_color_sampler =
      [&](gui::UiId id, const gui::Rect &image, const rhi::Texture &target,
          bool pointer_available = true) {
        if (!impl.color_sample_request.has_value()) {
          return;
        }
        const auto point =
            draw_eyedropper_overlay(ui, id, image, pointer_available);
        if (!point.has_value()) {
          return;
        }
        const auto sampled = sample_viewport_color(
            impl.window.device(), target, image, point->x, point->y);
        if (!sampled.has_value()) {
          return;
        }
        const InspectorColorSampleRequest request =
            std::move(*impl.color_sample_request);
        impl.color_sample_request.reset();
        const std::array<double, 3U> values{
            static_cast<double>(sampled->r),
            static_cast<double>(sampled->g),
            static_cast<double>(sampled->b)};
        const core::u64 gesture = impl.next_edit_gesture++;
        for (std::size_t component = 0U; component < values.size();
             ++component) {
          result.generic_edits.push_back(GenericEdit{
              request.node, request.properties[component], values[component],
              gesture});
        }
        impl.history_context = HistoryContext::Scene;
        impl.semantic_boundary = true;
      };
  // The mask target, its masks, and its authoring lock are one domain. Only
  // the picture a gesture is measured against differs between the two views,
  // which is exactly the projection each one passes.
  const auto mask_viewport_input =
      [&](const scene::Scene &view,
          const ViewportProjection *projection) {
        const scene::MaskInstance *selected_mask{};
        for (const scene::MaskInstance &mask : authored_stage.masks) {
          if (mask.source == impl.mask_selection.mask &&
              mask.scene == mask_target.scene &&
              mask.target == mask_target.target) {
            selected_mask = &mask;
            break;
          }
        }
        MaskViewportInput mask_input{};
        mask_input.target = mask_target;
        mask_input.masks = authored_stage.masks;
        mask_input.selection = impl.mask_selection;
        if (mask_target.coordinate_space ==
            scene::MaskCoordinateSpace::SourceLayer) {
          mask_input.source_layer_mapping =
              mask_source_mapping(view, mask_target.target, projection);
        }
        mask_input.pen_active =
            selected_mask != nullptr &&
            selected_mask->kind == scene::MaskKind::Bezier;
        mask_input.coverage_visible = impl.mask_coverage_visible;
        mask_input.authoring_enabled = mask_authoring_enabled;
        return mask_input;
      };
  const auto apply_mask_viewport_frame =
      [&](const MaskViewportFrame &mask_frame) {
        if (mask_frame.selection_changed) {
          impl.mask_selection = mask_frame.selection;
          impl.semantic_boundary = true;
        }
        if (!mask_frame.intent.has_value()) {
          return;
        }
        std::visit(
            [&](const auto &intent) {
              using Intent = std::decay_t<decltype(intent)>;
              if constexpr (std::is_same_v<Intent,
                                           MaskViewportCoverageIntent>) {
                impl.mask_coverage_visible = intent.visible;
              } else if constexpr (std::is_same_v<
                                       Intent,
                                       MaskViewportTranslateIntent>) {
                queue_mask_edit(MaskEditRequest{intent});
              } else if constexpr (std::is_same_v<
                                       Intent,
                                       MaskViewportShapeEditIntent>) {
                queue_mask_edit(MaskEditRequest{intent});
              } else if constexpr (std::is_same_v<
                                       Intent,
                                       MaskViewportPointInsertIntent>) {
                queue_mask_edit(MaskEditRequest{MasksPanelIntent{
                    MaskPointInsertIntent{intent.scene, intent.target,
                                          intent.mask, intent.index,
                                          intent.geometry}}});
              } else {
                static_assert(
                    std::is_same_v<Intent, MaskViewportPointEditIntent>);
                queue_mask_edit(MaskEditRequest{MasksPanelIntent{
                    MaskPointEditIntent{intent.scene, intent.target,
                                        intent.mask, intent.point,
                                        intent.patch}}});
              }
            },
            *mask_frame.intent);
      };
  // Compositor nodes are scene-scoped, so the graph on screen is the graph of
  // the scene on the Stage and editing it never reaches Program. The one thing
  // that still has to hold is a running dissolve: both of its inputs are
  // already committed pictures, so no graph edit may land mid-transition.
  const bool compositor_locked = switcher.rehearsal_locked;
  const auto draw_panel = [&](gui::PanelId panel, const gui::Rect &bounds) {
    if (panel == panel_stage) {
      // Rendered at the panel's physical size, so a dense display gets a
      // sharp Stage rather than a magnified one.
      const float pixels = impl.window.ui_scale();
      impl.stage_surface_width =
          static_cast<core::u32>(std::max(bounds.width * pixels, 1.0F));
      impl.stage_surface_height = static_cast<core::u32>(
          std::max((bounds.height - 29.0F) * pixels, 1.0F));

      // Floating dock frames are painted after Stage. When one is under
      // the pointer, keep Stage's picker and camera gestures from taking
      // the same press before the floating panel gets to handle it.
      std::optional<gui::Rect> floating_occluder;
      for (const gui::DockLayout::Floating &floating : impl.docks.floating()) {
        if (floating.node != impl.docks.leaf_of(panel_stage) &&
            floating.frame.contains(ui.pointer_x(), ui.pointer_y())) {
          floating_occluder = floating.frame;
          break;
        }
      }

      if (mask_stage_active && !color_sample_active) {
        const MaskViewportFrame mask_frame = impl.stage_panel.draw_masks(
            ui, bounds, *stage_texture, stage_scene,
            mask_viewport_input(stage_scene, nullptr), &scene.camera,
            static_cast<float>(timeline.rate_numerator) /
                static_cast<float>(
                    std::max(timeline.rate_denominator, 1U)));
        stage_viewport_frame.image = mask_frame.image;
        apply_mask_viewport_frame(mask_frame);
      } else {
        const gui::Rect *const interaction_block =
            color_sample_active
                ? &bounds
                : (floating_occluder.has_value()
                       ? &floating_occluder.value()
                       : nullptr);
        stage_viewport_frame = impl.stage_panel.draw(
            ui, bounds, *stage_texture, stage_interaction_scene,
            impl.scene_selection.members(), impl.anchor(), &scene.camera,
            interaction_block,
            static_cast<float>(timeline.rate_numerator) /
                static_cast<float>(
                    std::max(timeline.rate_denominator, 1U)),
            &feed_map,
            impl.stage_surface_selection.value_or(doc::NodeId{}), nullptr,
            plate_overlay, elements);
      }
      stage_viewport_drawn = true;
      // A passive notice leaves picking, framing and navigation available.
      // Retain the last submitted state when command acquisition is busy.
      if (!impl.stage_assets.headline.empty()) {
        const auto& image = stage_viewport_frame.image;
        const gui::Rect banner{image.x + 8.0F, image.y + 8.0F,
                               std::max(std::min(image.width - 16.0F, 520.0F), 0.0F),
                               std::min(54.0F, std::max(image.height - 16.0F, 0.0F))};
        ui.push_clip(image);
        ui.panel(banner, ui.theme().header);
        const auto color = impl.stage_assets.loading ? ui.theme().accent
                                                    : ui.theme().warning;
        ui.panel(gui::Rect{banner.x, banner.y, 3.0F, banner.height}, color);
        ui.label(gui::Rect{banner.x + 12.0F, banner.y + 4.0F,
                           std::max(banner.width - 24.0F, 0.0F), 21.0F},
                 impl.stage_assets.headline, color);
        ui.label(gui::Rect{banner.x + 12.0F, banner.y + 27.0F,
                           std::max(banner.width - 24.0F, 0.0F), 20.0F},
                 impl.stage_assets.detail, ui.theme().text);
        ui.pop_clip();
      }
      if (color_sample_active) {
        const bool pointer_available =
            !floating_occluder.has_value() ||
            !floating_occluder->contains(ui.pointer_x(), ui.pointer_y());
        run_color_sampler(gui::ui_id("editor.eyedropper.stage"),
                          stage_viewport_frame.image,
                          *stage_texture, pointer_available);
      }
      if (!color_sample_active && !switcher.rehearsal_locked) {
        if (stage_viewport_frame.surface_edit.has_value()) {
          const output::ProjectionSurface &surface =
              *stage_viewport_frame.surface_edit;
          impl.stage_surface_selection = surface.node;
          impl.feed_mapping_panel.select_surface(surface.node);
          impl.feed_mapping_panel.preview_surface_geometry(surface.node,
                                                           surface.geometry);
          FeedSurfaceTransform changed = FeedSurfaceTransform::All;
          switch (stage_viewport_frame.gizmo_axis) {
          case GizmoAxis::Screen:
            changed = FeedSurfaceTransform::PositionX |
                      FeedSurfaceTransform::PositionY |
                      FeedSurfaceTransform::PositionZ;
            break;
          case GizmoAxis::X:
            changed = FeedSurfaceTransform::PositionX;
            break;
          case GizmoAxis::Y:
            changed = FeedSurfaceTransform::PositionY;
            break;
          case GizmoAxis::Z:
            changed = FeedSurfaceTransform::PositionZ;
            break;
          case GizmoAxis::Yaw:
            changed = FeedSurfaceTransform::Yaw;
            break;
          case GizmoAxis::View:
          case GizmoAxis::None:
            break;
          }
          route_surface_edit(FeedSurfaceEditRequest{
              surface.node,
              surface.name,
              surface.geometry,
              impl.begin_gesture(),
              surface.shape,
              surface.horizontal_arc_turns,
              surface.mesh_columns,
              surface.mesh_source,
              surface.mesh_scale,
              changed});
        }
        if (!mask_stage_active) {
          handle_viewport(stage_viewport_frame, stage_scene,
                          stage_interaction_scene);
          const bool over_stage =
              !floating_occluder.has_value() &&
              stage_viewport_frame.image.contains(ui.pointer_x(),
                                                  ui.pointer_y());
          if (over_stage && !ui.typing() &&
              !impl.stage_panel.transforming() &&
              !impl.scene_selection.empty()) {
            if (ui.key_pressed(gui::Key::ClearLocation)) {
              clear_transform(TransformTool::Move, stage_scene);
            } else if (ui.key_pressed(gui::Key::ClearRotation)) {
              clear_transform(TransformTool::Rotate, stage_scene);
            } else if (ui.key_pressed(gui::Key::ClearScale)) {
              clear_transform(TransformTool::Scale, stage_scene);
            }
          }
        }
      } else if (!color_sample_active) {
        const float notice_width =
            std::min(std::max(bounds.width - 24.0F, 0.0F), 312.0F);
        const gui::Rect notice{bounds.x + (bounds.width - notice_width) * 0.5F,
                               bounds.y + 38.0F, notice_width, 24.0F};
        ui.panel(notice, ui.theme().background);
        outline(ui, notice, ui.theme().warning);
        ui.label(gui::Rect{notice.x + 8.0F, notice.y, notice.width - 16.0F,
                           notice.height},
                 "TRANSITION ACTIVE - REHEARSAL LOCKED", ui.theme().warning);
      }
      // The camera is the operator's own view, so it navigates through a
      // rehearsal lock and while masks are authored, though a mask tool keeps
      // the left button.
      if (color_sample_active || switcher.rehearsal_locked) {
        // Nothing may author into the Stage now; a keyboard transform must
        // not keep holding the keyboard waiting for a confirm.
        impl.stage_panel.cancel_transform();
      }
      if (color_sample_active) {
        impl.navigate_left = false;
        impl.navigate_middle = false;
        impl.navigate_right = false;
      } else {
        // A keyboard transform owns the pointer, its right-click cancel and
        // the wheel included.
        navigate_stage_camera(stage_viewport_frame,
                              !floating_occluder.has_value() &&
                                  !stage_viewport_frame.keyboard_transform,
                              !mask_stage_active);
      }
      return;
    }
    if (panel == panel_program) {
      ui.panel(bounds, ui.theme().background);

      const gui::Rect output_bar{bounds.x, bounds.y, bounds.width, 26.0F};
      ui.panel(output_bar, ui.theme().header);
      ui.panel(gui::Rect{output_bar.x, output_bar.y + output_bar.height - 1.0F,
                         output_bar.width, 1.0F},
               ui.theme().divider);
      // The tab already says Program Output; the bar names the mode only when
      // it changes what a drag does - authoring a matte - and there is room.
      if (mask_program_active && output_bar.width > 420.0F) {
        ui.label(gui::Rect{output_bar.x + 8.0F, output_bar.y,
                           std::max(output_bar.width - 350.0F, 0.0F),
                           output_bar.height},
                 "Masks", ui.theme().accent);
      }
      const float output_name_width =
          std::min(190.0F, std::max(output_bar.width - 170.0F, 60.0F));
      const gui::Rect output_name{output_bar.x + 6.0F, output_bar.y + 3.0F,
                                  output_name_width, output_bar.height - 6.0F};
      const float resolution_x =
          output_bar.x + std::max(output_bar.width - 96.0F, 64.0F);
      if (output_bar.width > 300.0F) {
        ui.label(gui::Rect{resolution_x - 62.0F, output_bar.y, 58.0F,
                           output_bar.height},
                 "Preview", ui.theme().text_muted);
      }
      impl.program_panel.draw_resolution_control(ui,
          gui::Rect{resolution_x, output_bar.y + 3.0F, 90.0F, 20.0F});
      std::array<char, 64U> output_identity{};
      if (program_output != nullptr) {
        static_cast<void>(std::snprintf(
            output_identity.data(), output_identity.size(),
            "Output %02u  -  node %u", program_output->node_index + 1U,
            program_output->node_index));
      } else {
        static_cast<void>(std::snprintf(output_identity.data(),
                                        output_identity.size(),
                                        "No output map"));
      }
      ui.tooltip(output_name, feed_map.maps.size() > 1U
                                  ? "Show the next output"
                                  : "The output this preview shows");
      if (chrome_button(ui, mix(gui::ui_id("editor.program"), "output"),
                        output_name, output_identity.data(), false,
                        feed_map.maps.size() > 1U)) {
        const auto current =
            std::find_if(feed_map.maps.begin(), feed_map.maps.end(),
                         [&](const output::OutputMap &map) {
                           return program_output != nullptr &&
                                  map.node == program_output->node;
                         });
        const auto next = current == feed_map.maps.end() ||
                                  std::next(current) == feed_map.maps.end()
                              ? feed_map.maps.begin()
                              : std::next(current);
        if (next != feed_map.maps.end()) {
          impl.program_output_map_node = next->node;
        }
      }

      constexpr float program_footer_height = 24.0F;
      const gui::Rect footer{
          bounds.x,
          bounds.y + std::max(bounds.height - program_footer_height,
                              output_bar.height),
          bounds.width,
          std::min(program_footer_height,
                   std::max(bounds.height - output_bar.height, 0.0F))};
      const gui::Rect content{
          bounds.x, bounds.y + output_bar.height, bounds.width,
          std::max(footer.y - bounds.y - output_bar.height, 0.0F)};
      ui.panel(content, gui::UiColor{0.0F, 0.0F, 0.0F, 1.0F});
      const ViewportProjection *const program_projection =
          program_view_projection.has_value()
              ? &program_view_projection.value()
              : nullptr;
      gui::Rect program_viewport_image{};
      if (program_preview_ok && mask_program_active &&
          !color_sample_active) {
        // The overlay is measured against the connector image, so a matte the
        // operator is looking at on the wall is the matte their pointer moves.
        const MaskViewportFrame mask_frame = impl.program_panel.draw_masks(
            ui, content, *program_texture, scene,
            mask_viewport_input(scene, program_projection), nullptr,
            static_cast<float>(timeline.rate_numerator) /
                static_cast<float>(std::max(timeline.rate_denominator, 1U)),
            program_projection);
        program_viewport_image = mask_frame.image;
        apply_mask_viewport_frame(mask_frame);
      } else if (program_preview_ok) {
        const gui::Rect *const interaction_block =
            color_sample_active ? &content : nullptr;
        const ViewportFrame program_viewport = impl.program_panel.draw(
            ui, content, *program_texture, scene,
            impl.scene_selection.members(), impl.anchor(), nullptr,
            interaction_block,
            static_cast<float>(timeline.rate_numerator) /
                static_cast<float>(std::max(timeline.rate_denominator, 1U)),
            nullptr, doc::NodeId{}, program_projection, plate_overlay,
            elements);
        program_viewport_image = program_viewport.image;
        if (!color_sample_active && !switcher.rehearsal_locked) {
          handle_viewport(program_viewport, scene, scene);
        }
      } else {
        const std::string_view fault =
            program_mapping_unavailable ? "OUTPUT MAPPING UNAVAILABLE"
                                        : program_preview_fault;
        const float label_width = ui.font_width(fault);
        ui.label(gui::Rect{content.x +
                               std::max((content.width - label_width) * 0.5F,
                                        8.0F),
                           content.y + std::max(content.height * 0.5F - 10.0F,
                                                0.0F),
                           label_width, 20.0F},
                 fault, ui.theme().warning);
      }
      if (program_preview_ok && color_sample_active) {
        bool pointer_available = true;
        for (const gui::DockLayout::Floating &floating :
             impl.docks.floating()) {
          if (floating.node != impl.docks.leaf_of(panel_program) &&
              floating.frame.contains(ui.pointer_x(), ui.pointer_y())) {
            pointer_available = false;
            break;
          }
        }
        run_color_sampler(gui::ui_id("editor.eyedropper.program"),
                          program_viewport_image, *program_texture,
                          pointer_available);
      }

      ui.panel(footer, ui.theme().header);
      // Every asset change - a live Blender mesh edit, a new clip - goes
      // through a second or so of loading and a synchronized switch. The
      // footer names that hand-over; the banner over the picture is kept for
      // a wait that has become the operator's problem.
      constexpr auto routine_wait = std::chrono::seconds{3};
      const bool program_wait_banner =
          program_assets.waiting &&
          (!program_assets.routine ||
           std::chrono::steady_clock::now() - impl.program_wait_started >= routine_wait);
      if (program_wait_banner) {
        const gui::Rect banner{content.x + 8.0F, content.y + 8.0F,
                              std::max(content.width - 16.0F, 0.0F), std::min(76.0F, content.height)};
        ui.panel(banner, ui.theme().header);
        ui.panel(gui::Rect{banner.x, banner.y, 3.0F, banner.height}, ui.theme().warning);
        ui.label(gui::Rect{banner.x + 12, banner.y + 6, banner.width - 24, 21},
                 program_assets.headline, ui.theme().warning);
        ui.label(gui::Rect{banner.x + 12, banner.y + 28, banner.width - 24, 20},
                 program_assets.detail, ui.theme().text);
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - impl.program_wait_started).count();
        const auto elapsed = "Waiting " + std::to_string(seconds) + "s - " +
            (impl.assets.visible_commit != 0 ? "waiting for output confirmation" :
             impl.asset_gate.visible_commit() != 0 ? "showing the previous Program image" : "preparing the first Program image");
        ui.label(gui::Rect{banner.x + 12, banner.y + 49, banner.width - 24, 20},
                 elapsed, ui.theme().text_muted);
      }
      std::array<char, 256U> preview_status{};
      const std::string_view renderer_status =
          program_assets.waiting ? std::string_view{program_assets.headline} : renderer_readiness.has_value()
              ? renderer_readiness->headline()
              : impl.program_renderer.renderer_readiness().headline();
      // The output's own size first - the thing an operator checks - then
      // the preview's when it differs, the mapping, and the renderer; the
      // whole line again in a tooltip when the panel is too narrow for it.
      const bool preview_differs =
          program_extent.preview_width != program_extent.output_width ||
          program_extent.preview_height != program_extent.output_height;
      const int preview_length =
          preview_differs
              ? std::snprintf(
                    preview_status.data(), preview_status.size(),
                    "%u x %u   preview %u x %u   %.*s   %.*s",
                    program_extent.output_width, program_extent.output_height,
                    program_extent.preview_width, program_extent.preview_height,
                    static_cast<int>(program_projection_name(program_output).size()),
                    program_projection_name(program_output).data(),
                    static_cast<int>(renderer_status.size()), renderer_status.data())
              : std::snprintf(
                    preview_status.data(), preview_status.size(),
                    "%u x %u   %.*s   %.*s", program_extent.output_width,
                    program_extent.output_height,
                    static_cast<int>(program_projection_name(program_output).size()),
                    program_projection_name(program_output).data(),
                    static_cast<int>(renderer_status.size()), renderer_status.data());
      const std::string_view state_text =
          !program_preview_ok ? "FAULT"
          : program_assets.waiting && !program_preview_held ? "WAIT"
          : program_preview_held ? "HOLD"
          : (timeline.playing ? "PLAYING" : "PAUSED");
      const gui::UiColor state_color =
          !program_preview_ok || program_preview_held || program_assets.waiting
              ? ui.theme().warning
              : (timeline.playing ? ui.theme().good : ui.theme().text_muted);
      const float state_width = pill_width(ui, state_text);
      // Clear of the corner, where a floating frame keeps its resize grip.
      constexpr float corner = 20.0F;
      ui.label(gui::Rect{footer.x + 8.0F, footer.y,
                         std::max(footer.width - state_width - corner - 14.0F,
                                  0.0F),
                         footer.height},
               std::string_view{
                   preview_status.data(),
                   std::min(preview_status.size() - 1U,
                            static_cast<std::size_t>(std::max(preview_length, 0)))},
               ui.theme().text_muted);
      static_cast<void>(draw_pill(
          ui, footer.x + footer.width - state_width - corner,
          footer.y + footer.height * 0.5F, state_text, state_color,
          program_preview_ok && !program_preview_held &&
              !program_assets.waiting && timeline.playing));
      return;
    }
    if (panel == panel_scene) {
      gui::Rect tree_bounds = bounds;
      if (!switcher.scenes.empty()) {
        // The show's scenes, above the scene being edited: which one is on
        // Program, which is in Preview, and the one the outliner below is
        // showing. Renamed in place like any row; deleted from the hover
        // cross or the menu, with a second click to confirm.
        constexpr float scene_header_height = 28.0F;
        constexpr float scene_row_height = 26.0F;
        constexpr std::size_t scene_rows_shown = 4U;
        const gui::Theme &theme = ui.theme();
        const std::size_t visible =
            std::min<std::size_t>(switcher.scenes.size(), scene_rows_shown);
        const std::size_t max_offset = switcher.scenes.size() > visible
                                           ? switcher.scenes.size() - visible
                                           : 0U;
        impl.scene_list_offset = std::min(impl.scene_list_offset, max_offset);
        const float scene_block_height =
            scene_header_height + static_cast<float>(visible) * scene_row_height +
            6.0F;
        ui.panel(gui::Rect{bounds.x, bounds.y, bounds.width, scene_block_height},
                 theme.panel);
        const gui::Rect header{bounds.x, bounds.y, bounds.width,
                               scene_header_height};
        ui.panel(header, theme.header);
        ui.label(gui::Rect{header.x + 10.0F, header.y, 120.0F, header.height},
                 "SCENES", theme.text_muted);
        const std::string count = std::to_string(switcher.scenes.size());
        ui.label(gui::Rect{header.x + 10.0F + ui.font_width("SCENES") + 8.0F,
                           header.y, 40.0F, header.height},
                 count, with_alpha(theme.text_muted, 0.7F));
        const gui::Rect add_scene{header.x + header.width - 56.0F,
                                  header.y + 2.0F, 26.0F, 24.0F};
        const gui::Rect copy_scene{header.x + header.width - 29.0F,
                                   header.y + 2.0F, 26.0F, 24.0F};
        if (icon_button(ui, gui::ui_id("editor.scene.create"), add_scene,
                        gui::Icon::Plus, "New scene", false,
                        !switcher.rehearsal_locked)) {
          result.switcher = SwitcherRequest{SwitcherAction::CreateScene};
          impl.scene_list_offset = max_offset + 1U;
        }
        if (icon_button(ui, gui::ui_id("editor.scene.duplicate"), copy_scene,
                        gui::Icon::Duplicate,
                        "Duplicate the scene being edited", false,
                        !switcher.rehearsal_locked)) {
          result.switcher = SwitcherRequest{SwitcherAction::DuplicateScene,
                                            authored_stage.id, 0U};
          impl.scene_list_offset = max_offset + 1U;
        }

        float row_y = header.y + header.height + 2.0F;
        const gui::Rect scene_rows{bounds.x, row_y, bounds.width,
                                   static_cast<float>(visible) *
                                       scene_row_height};
        if (scene_rows.contains(ui.pointer_x(), ui.pointer_y()) &&
            ui.wheel() != 0.0F) {
          if (ui.wheel() > 0.0F && impl.scene_list_offset > 0U) {
            --impl.scene_list_offset;
          } else if (ui.wheel() < 0.0F && impl.scene_list_offset < max_offset) {
            ++impl.scene_list_offset;
          }
        }
        const auto program_info =
            std::find_if(switcher.scenes.begin(), switcher.scenes.end(),
                         [&](const scene::SceneInfo &info) {
                           return info.id == switcher.program_scene;
                         });
        for (std::size_t local_index = 0U; local_index < visible;
             ++local_index) {
          const std::size_t index = impl.scene_list_offset + local_index;
          const scene::SceneInfo &info = switcher.scenes[index];
          const gui::Rect row{bounds.x + 4.0F, row_y,
                              std::max(bounds.width - 8.0F, 0.0F),
                              scene_row_height};
          const bool row_pointed = !ui.pointer_blocked() &&
                                   row.contains(ui.pointer_x(), ui.pointer_y());
          const bool is_program = info.id == switcher.program_scene;
          const bool is_rehearsal = info.id == switcher.rehearsal_scene;
          const bool is_edited = info.id == authored_stage.id;
          const bool compatible = is_program ||
                                  program_info == switcher.scenes.end() ||
                                  (scene::valid(program_info->canvas) &&
                                   program_info->canvas == info.canvas);
          // Any scene can be taken into Preview and worked on. A canvas that
          // differs from Program only rules out the dissolve, and the row says
          // so; refusing to open the scene at all would make a resize a dead
          // end.
          const bool selectable = !switcher.rehearsal_locked &&
                                  ((is_program && switcher.rehearsal_mode) ||
                                   !is_program);
          const bool delete_enabled = !switcher.rehearsal_locked && !is_program;
          const bool awaiting_delete = impl.pending_scene_delete == info.id;

          // Right to left: the hover cross, the on-air pill, then the name.
          constexpr float remove_width = 24.0F;
          const gui::Rect remove{row.x + row.width - remove_width, row.y,
                                 remove_width, row.height};
          const std::string_view tag =
              is_program ? "PGM" : (is_rehearsal ? "PVW" : (!compatible ? "SIZE" : ""));
          const float tag_width = tag.empty() ? 0.0F : pill_width(ui, tag) + 6.0F;
          // The small Blender-orange B before a scene's name is its Blender
          // button - well away from the PGM/PVW pill, so the link never reads
          // as an on-air state. A scene Blender holds always shows it; any
          // other row offers it while the pointer is over the row.
          const auto blender_link = std::find_if(
              project.blender_scenes.begin(), project.blender_scenes.end(),
              [&](const BlenderSceneState &state) { return state.scene == info.id; });
          const BlenderSceneState *const blender =
              blender_link == project.blender_scenes.end() ? nullptr : &*blender_link;
          const gui::Rect blender_hit{row.x + 4.0F, row.y, 22.0F, row.height};
          const float name_x = blender_hit.x + blender_hit.width + 4.0F;
          const gui::Rect name_bounds{
              name_x, row.y,
              std::max(remove.x - tag_width - name_x - 4.0F, 0.0F),
              row.height};
          const gui::Rect row_content{
              name_x - 2.0F, row.y,
              std::max(remove.x - name_x + 2.0F, 0.0F), row.height};

          const bool renaming = impl.scene_rename.editing(info.id.value());
          const gui::Interaction interaction =
              selectable && !renaming
                  ? ui.grab(gui::ui_id("editor.scene-slot") ^ info.id.value(),
                            row_content)
                  : gui::Interaction{};
          const bool show_remove = delete_enabled && (row_pointed || awaiting_delete);
          const gui::Interaction remove_interaction =
              show_remove
                  ? ui.grab(gui::ui_id("editor.scene-remove") ^ info.id.value(),
                            remove)
                  : gui::Interaction{};

          gui::UiColor background = theme.panel;
          if (is_edited) {
            background = theme.selection;
          } else if (interaction.hovered || row_pointed) {
            background = theme.widget_hover;
          }
          ui.panel(row, background);
          ui.panel(gui::Rect{row.x, row.y + 3.0F, 3.0F, row.height - 6.0F},
                   is_program ? theme.good
                              : (is_rehearsal ? theme.accent : theme.border));

          const gui::Interaction blender_interaction =
              ui.grab(gui::ui_id("editor.scene-blender") ^ info.id.value(),
                      blender_hit);
          if (blender != nullptr || row_pointed) {
            constexpr gui::UiColor blender_orange{0.918F, 0.471F, 0.098F, 1.0F};
            const bool open = blender != nullptr &&
                              (blender->connected || blender->running || blender->opening);
            const bool syncing = blender != nullptr && blender->connected && !blender->failed;
            gui::UiColor tone = blender != nullptr && blender->failed ? theme.warning
                                : open                               ? blender_orange
                                                                     : theme.text_muted;
            if (blender_interaction.hovered) {
              tone = blender == nullptr ? theme.text
                                        : gui::UiColor{std::min(tone.r * 1.25F, 1.0F), std::min(tone.g * 1.25F, 1.0F),
                                                       std::min(tone.b * 1.25F, 1.0F), tone.a};
            }
            // Filled while Blender syncs the scene; an outline while Blender
            // holds it but shows another, is starting, or is closed.
            const gui::Rect badge{blender_hit.x + 3.0F, row.y + 5.0F, 16.0F,
                                  row.height - 10.0F};
            if (syncing) ui.panel(badge, tone);
            outline(ui, badge, tone);
            const float glyph = ui.font_width("B");
            ui.label(gui::Rect{badge.x + std::max((badge.width - glyph) * 0.5F, 0.0F),
                               badge.y, badge.width, badge.height},
                     "B", syncing ? theme.background : tone);
            std::string tip;
            if (blender == nullptr) {
              tip = is_program ? "Open in Blender - edits there change Program directly"
                               : "Open in Blender";
            } else {
              tip = blender->failed      ? "Blender link failed"
                    : blender->connected ? "Syncing with Blender"
                    : blender->opening   ? "Opening in Blender..."
                    : blender->running   ? "In Blender, which shows another scene - click to show this one"
                                         : "Blender is closed - click to open it";
              if (!blender->file.empty()) tip += " - " + std::string{blender->file};
              if (!blender->status.empty()) tip += " - " + std::string{blender->status};
            }
            ui.tooltip(blender_hit, tip);
          }
          if (blender_interaction.clicked) {
            impl.pending_scene_delete.reset();
            result.open_in_blender = info.id;
          }

          if (renaming) {
            if (auto renamed = impl.scene_rename.draw(
                    ui, gui::Rect{name_bounds.x - 4.0F, row.y + 2.0F,
                                  name_bounds.width + 4.0F, row.height - 4.0F})) {
              result.renames.push_back(ElementRenameIntent{
                  info.id, std::move(*renamed), impl.next_edit_gesture++});
            }
          } else {
            ui.label(name_bounds,
                     awaiting_delete
                         ? std::string_view{"Delete this scene? Click x again"}
                         : (info.name.empty() ? std::string_view{"Untitled Scene"}
                                              : std::string_view{info.name}),
                     awaiting_delete ? theme.warning
                                     : (is_edited ? theme.text
                                                  : scale_color(theme.text, 0.9F)));
          }
          if (!tag.empty()) {
            const gui::Rect pill = draw_pill(
                ui, remove.x - tag_width, row.y + row.height * 0.5F, tag,
                !compatible ? theme.warning
                            : (is_program ? theme.good : theme.accent),
                is_program);
            ui.tooltip(pill, is_program ? "On Program - the live output"
                             : is_rehearsal
                                 ? "In Preview - edited without touching Program"
                                 : "A different canvas from Program: it will cut, not dissolve");
          }
          if (show_remove) {
            draw_inline_remove(
                ui, remove,
                remove_interaction.hovered || awaiting_delete ? theme.warning
                                                              : theme.text_muted);
            ui.tooltip(remove, "Delete scene");
          }
          if (row_pointed && ui.secondary_pointer_pressed()) {
            impl.scene_menu_scene = info.id;
            ui.open_context_menu(gui::ui_id("editor.scene-menu"), ui.pointer_x(),
                                 ui.pointer_y());
          }
          if (interaction.clicked) {
            impl.pending_scene_delete.reset();
            if (is_program && switcher.rehearsal_mode) {
              result.switcher = SwitcherRequest{SwitcherAction::ToggleRehearse};
            } else if (!is_program) {
              result.switcher =
                  SwitcherRequest{SwitcherAction::SelectRehearsal, info.id, 0U};
            }
            // A second click on the name renames the scene.
            if (name_bounds.contains(ui.pointer_x(), ui.pointer_y()) &&
                impl.scene_clicks.double_click(info.id.value(),
                                               ui.time_seconds())) {
              impl.scene_rename.begin(ui, info.id.value(), info.name);
            }
          }
          if (remove_interaction.clicked) {
            if (awaiting_delete) {
              result.switcher =
                  SwitcherRequest{SwitcherAction::DeleteScene, info.id, 0U};
              impl.pending_scene_delete.reset();
            } else {
              impl.pending_scene_delete = info.id;
            }
            impl.semantic_boundary = true;
          }
          row_y += scene_row_height;
        }
        impl.scene_rename.end_frame(ui);
        if (max_offset > 0U) {
          // A thin scroll thumb: the list holds more than it shows.
          const float track_height = static_cast<float>(visible) * scene_row_height;
          const float thumb = std::max(
              track_height * static_cast<float>(visible) /
                  static_cast<float>(switcher.scenes.size()),
              10.0F);
          const float travel = track_height - thumb;
          ui.panel(gui::Rect{bounds.x + bounds.width - 3.0F,
                             scene_rows.y + travel *
                                                static_cast<float>(impl.scene_list_offset) /
                                                static_cast<float>(max_offset),
                             2.0F, thumb},
                   theme.widget_hover);
        }
        if (impl.scene_menu_scene.has_value()) {
          const gui::UiId menu_id = gui::ui_id("editor.scene-menu");
          const scene::SceneId menu_scene = *impl.scene_menu_scene;
          const bool linked = std::any_of(
              project.blender_scenes.begin(), project.blender_scenes.end(),
              [&](const BlenderSceneState &state) {
                return state.scene == menu_scene &&
                       (state.connected || state.running || state.opening);
              });
          const bool deletable = !switcher.rehearsal_locked &&
                                 menu_scene != switcher.program_scene;
          std::vector<std::string_view> labels{"Rename", "Open in Blender"};
          if (linked) labels.emplace_back("Disconnect Blender");
          if (deletable) labels.emplace_back("Delete Scene");
          if (const auto chosen = ui.context_menu(menu_id, labels)) {
            const std::string_view picked = labels[*chosen];
            if (picked == "Rename") {
              const auto found = std::find_if(
                  switcher.scenes.begin(), switcher.scenes.end(),
                  [&](const scene::SceneInfo &info) { return info.id == menu_scene; });
              if (found != switcher.scenes.end()) {
                impl.scene_rename.begin(ui, menu_scene.value(), found->name);
              }
            } else if (picked == "Open in Blender") {
              result.open_in_blender = menu_scene;
            } else if (picked == "Disconnect Blender") {
              result.disconnect_blender_scene = menu_scene;
            } else {
              impl.pending_scene_delete = menu_scene;
            }
            impl.scene_menu_scene.reset();
          } else if (!ui.menu_open(menu_id)) {
            impl.scene_menu_scene.reset();
          }
        }
        ui.panel(gui::Rect{bounds.x, bounds.y + scene_block_height - 1.0F,
                           bounds.width, 1.0F},
                 theme.border);
        tree_bounds.y += scene_block_height;
        tree_bounds.height =
            std::max(tree_bounds.height - scene_block_height, 0.0F);
      }
      const auto request = impl.scene_panel.draw(
          ui, tree_bounds, elements, impl.scene_selection.members(),
          impl.anchor(), true, impl.anchor() == project_settings.stage_node && project_settings.stage_node != doc::NodeId{}
              ? SceneSettingsTarget::Environment : impl.scene_settings_selection,
          feed_map.surfaces, impl.stage_surface_selection.value_or(doc::NodeId{}));
      if (request.has_value() && request->surface) {
        // An LED surface row: the same selection a Stage pick makes, or the
        // same lock its Feed Mapping padlock writes.
        if (request->lock_changed) {
          result.lock_edits.push_back(LockEdit{
              request->node, request->locked, impl.next_edit_gesture++});
          if (request->locked &&
              impl.stage_surface_selection == request->node) {
            impl.stage_surface_selection.reset();
            impl.delete_domain = DeleteDomain::Neutral;
          }
        } else {
          impl.stage_surface_selection = request->node;
          impl.feed_mapping_panel.select_surface(request->node);
          impl.scene_selection.clear();
          impl.group_nodes.clear();
          impl.group_start.clear();
          impl.delete_domain = DeleteDomain::Surface;
        }
        return;
      }
      if (request.has_value()) {
        if (request->rename.has_value()) {
          result.renames.push_back(ElementRenameIntent{
              request->node, *request->rename, impl.next_edit_gesture++});
          impl.history_context = HistoryContext::Scene;
          return;
        }
        if (request->visibility_changed) {
          result.element_flags.push_back(ElementFlagIntent{
              authored_stage.id, request->node, TimelineElementFlag::Visible,
              request->visible, impl.next_edit_gesture++});
          impl.history_context = HistoryContext::Scene;
          return;
        }
        if (request->highlight_changed) {
          if (!impl.scene_selection.empty()) {
            result.highlights.push_back(ElementHighlightIntent{
                {impl.scene_selection.members().begin(),
                 impl.scene_selection.members().end()},
                request->highlight,
                impl.next_edit_gesture++});
            impl.history_context = HistoryContext::Scene;
          }
          return;
        }
        if (request->delete_selection) {
          delete_selection();
          return;
        }
        if (request->reorder_destination != doc::NodeId{} &&
            !request->structure.has_value()) {
          result.layer_orders.push_back(TimelineLayerOrderIntent{
              authored_stage.id, request->node, request->reorder_destination,
              request->reorder_before ? TimelineLayerPlacement::Before
                                      : TimelineLayerPlacement::After,
              impl.next_edit_gesture++});
          impl.history_context = HistoryContext::Scene;
          return;
        }
        if (request->settings != SceneSettingsTarget::None) {
          impl.clear_scene_selection(true);
          impl.stage_surface_selection.reset();
          if (request->settings == SceneSettingsTarget::Environment) impl.scene_selection.replace(project_settings.stage_node);
          else impl.scene_settings_selection = request->settings;
          impl.show_panel(panel_inspector);
          return;
        }
        if (request->structure.has_value()) {
          GroupEditIntent edit{};
          edit.scene = authored_stage.id;
          edit.gesture = impl.next_edit_gesture++;
          if (*request->structure ==
              SceneSelectionRequest::StructureAction::MoveInto) {
            edit.action = GroupEditAction::MoveInto;
            edit.selection = request->moved;
            edit.group = request->destination;
            edit.anchor = request->reorder_destination;
            edit.before = request->reorder_before;
          } else if (*request->structure ==
                     SceneSelectionRequest::StructureAction::Create) {
            edit.action = GroupEditAction::Create;
            edit.group = request->destination;
            edit.kind = request->group_kind;
            impl.pending_new_group_known.clear();
            for (const ElementInfo &element : elements) {
              if (element.kind == ElementKind::Group) {
                impl.pending_new_group_known.insert(element.node);
              }
            }
            impl.pending_new_group_parent = request->destination;
            impl.pending_new_group_frames = 120U;
          } else if (*request->structure ==
              SceneSelectionRequest::StructureAction::Group) {
            edit.action = GroupEditAction::Group;
            edit.selection.assign(impl.scene_selection.members().begin(),
                                  impl.scene_selection.members().end());
            impl.pending_group_members = edit.selection;
            impl.pending_group_frames = 120U;
          } else {
            edit.action = GroupEditAction::Ungroup;
            edit.group = impl.anchor();
            std::vector<const ElementInfo *> children;
            for (const ElementInfo &element : elements) {
              if (element.parent_group == edit.group) {
                children.push_back(&element);
              }
            }
            std::stable_sort(
                children.begin(), children.end(),
                [](const ElementInfo *left, const ElementInfo *right) noexcept {
                  return std::pair{left->group_order, left->node.value()} <
                         std::pair{right->group_order, right->node.value()};
                });
            impl.pending_group_members.clear();
            impl.pending_group_frames = 0U;
            impl.scene_selection.clear();
            if (!children.empty()) {
              impl.scene_selection.replace(children.front()->node);
              for (std::size_t index = 1U; index < children.size(); ++index) {
                impl.scene_selection.toggle(children[index]->node);
              }
              impl.scene_selection.set_primary(children.front()->node);
              impl.delete_domain = DeleteDomain::Scene;
            } else {
              impl.delete_domain = DeleteDomain::Neutral;
            }
          }
          impl.history_context = HistoryContext::Scene;
          impl.semantic_boundary = true;
          result.group_edit = std::move(edit);
        } else if (request->lock_changed) {
          result.lock_edits.push_back(LockEdit{
              request->node, request->locked, impl.next_edit_gesture++});
          if (request->locked) {
            static_cast<void>(impl.scene_selection.erase_if(
                [&](doc::NodeId node) { return node == request->node; }));
            impl.group_nodes.clear();
            impl.group_start.clear();
            impl.manipulating = false;
          }
        } else {
          impl.stage_surface_selection.reset();
          switch (request->action) {
          case SelectionAction::Replace:
            impl.select_only(request->node);
            break;
          case SelectionAction::Toggle:
            impl.toggle(request->node);
            break;
          case SelectionAction::Range:
            impl.select_range(request->node, request->visible_order);
            break;
          case SelectionAction::Clear:
            impl.clear_scene_selection(true);
            break;
          }
        }
      }
      return;
    }
    if (panel == panel_selection_sets) {
      SelectionSetPanelFrame set_frame = impl.selection_sets_panel.draw(
          ui, bounds, selection_sets, impl.selection_set_selection,
          impl.scene_selection.size(), !switcher.rehearsal_locked);
      if (set_frame.selection_changed) {
        impl.selection_set_selection = set_frame.selected;
        impl.history_context = HistoryContext::Scene;
        impl.semantic_boundary = true;
      }
      if (set_frame.intent.has_value()) {
        SelectionSetEditIntent edit{};
        edit.scene = authored_stage.id;
        edit.set = set_frame.intent->set;
        edit.name = std::move(set_frame.intent->name);
        switch (set_frame.intent->action) {
        case SelectionSetPanelAction::SaveNew:
          edit.action = SelectionSetEditAction::Create;
          edit.members.assign(impl.scene_selection.members().begin(),
                              impl.scene_selection.members().end());
          edit.gesture = impl.next_edit_gesture++;
          break;
        case SelectionSetPanelAction::ReplaceMembers:
          edit.action = SelectionSetEditAction::ReplaceMembers;
          edit.members.assign(impl.scene_selection.members().begin(),
                              impl.scene_selection.members().end());
          edit.gesture = impl.next_edit_gesture++;
          break;
        case SelectionSetPanelAction::Recall:
          edit.action = SelectionSetEditAction::Recall;
          break;
        case SelectionSetPanelAction::Rename:
          edit.action = SelectionSetEditAction::Rename;
          edit.gesture = impl.next_edit_gesture++;
          break;
        case SelectionSetPanelAction::Delete:
          edit.action = SelectionSetEditAction::Delete;
          edit.gesture = impl.next_edit_gesture++;
          break;
        }
        impl.history_context = HistoryContext::Scene;
        impl.semantic_boundary = true;
        result.selection_set_edit = std::move(edit);
      }
      return;
    }
    if (panel == panel_masks) {
      const MasksPanelFrame mask_frame = impl.masks_panel.draw(
          ui, bounds, mask_target, authored_stage.masks,
          authored_stage.mask_diagnostics, impl.mask_selection,
          mask_authoring_enabled);
      if (mask_frame.selection_changed) {
        impl.mask_selection = mask_frame.selection;
        impl.history_context = HistoryContext::Scene;
        impl.semantic_boundary = true;
      }
      if (mask_frame.intent.has_value()) {
        queue_mask_edit(MaskEditRequest{*mask_frame.intent});
      }
      return;
    }
    if (panel == panel_library) {
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
      if (library.instantiate_asset.has_value()) {
        instantiate_asset(library.instantiate_asset.value(), std::nullopt);
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
      if (library.asset_drop.has_value()) {
        pending_asset_drop = library.asset_drop;
      }
      result.thumbnail_requests.insert(result.thumbnail_requests.end(),
                                       library.thumbnail_requests.begin(),
                                       library.thumbnail_requests.end());
      result.thumbnail_retain = library.thumbnail_retain;
      return;
    }
    if (panel == panel_effects) {
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
    if (panel == panel_compositor || panel == panel_nodes) {
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
          const gui::Rect notice{
              bounds.x + 12.0F, bounds.y + 34.0F,
              std::min(std::max(bounds.width - 24.0F, 0.0F), 286.0F), 24.0F};
          ui.panel(notice, ui.theme().background);
          outline(ui, notice, ui.theme().warning);
          ui.label(gui::Rect{notice.x + 8.0F, notice.y,
                             std::max(notice.width - 16.0F, 0.0F),
                             notice.height},
                   "SELECT A SCENE ELEMENT FOR + LAYER", ui.theme().warning);
        }
      } else {
        impl.compositor_edit_gesture = 0U;
        const float notice_width =
            std::min(std::max(bounds.width - 24.0F, 0.0F), 360.0F);
        const gui::Rect notice{bounds.x + (bounds.width - notice_width) * 0.5F,
                               bounds.y + 34.0F, notice_width, 24.0F};
        ui.panel(notice, ui.theme().background);
        outline(ui, notice, ui.theme().warning);
        ui.label(gui::Rect{notice.x + 8.0F, notice.y,
                           std::max(notice.width - 16.0F, 0.0F), notice.height},
                 "PROJECT GRAPH - EXIT PREVIEW TO EDIT", ui.theme().warning);
      }
      return;
    }
    if (panel == panel_hardware_mapping) {
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
          if (binding.node == doc::NodeId{}) {
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
      if (mapping.incompatible_link_attempted) {
        impl.hardware_warning_until = ui.time_seconds() + 1.5;
      }
      if (!mapping.editing) {
        impl.hardware_edit_gesture = 0U;
      }
      if (ui.time_seconds() < impl.hardware_warning_until) {
        const gui::Rect notice{
            bounds.x + 12.0F, bounds.y + 34.0F,
            std::min(std::max(bounds.width - 24.0F, 0.0F), 286.0F), 24.0F};
        ui.panel(notice, ui.theme().background);
        outline(ui, notice, ui.theme().warning);
        ui.label(gui::Rect{notice.x + 8.0F, notice.y,
                           std::max(notice.width - 16.0F, 0.0F), notice.height},
                 "PORTS ARE INCOMPATIBLE OR ALREADY USED", ui.theme().warning);
      }
      return;
    }
    if (panel == panel_feed_mapping) {
      FeedMappingFrame mapping =
          impl.feed_mapping_panel.draw(ui, bounds, scene.canvas, feed_map,
                                       timeline.program_frame, &hardware_map);
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
      if (mapping.surface_add.has_value()) {
        auto request = std::move(*mapping.surface_add);
        if (request.shape == output::ProjectionSurfaceShape::ImportedMesh) {
          auto chosen = request.mesh_source.empty() ? choose_projection_mesh() : std::optional<std::string>{request.mesh_source};
          if (chosen) enqueue_model(*chosen, {}, std::move(request), {});
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
        const auto chosen = choose_projection_mesh();
        if (chosen) enqueue_model(*chosen, {}, {}, std::move(request));
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
    if (panel == panel_output) {
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
    if (panel == panel_live_data) {
      LiveDataState live = live_data;
      // The panel reports the frame being shown, and the authored policy the
      // Director is actually running - not the operator's unapplied draft.
      live.show_frame = timeline.program_frame;
      live.canvas = scene.canvas;
      live.icvfx = scene.renderer_settings.icvfx;
      impl.live_data_panel.draw(ui, bounds, live);
      return;
    }
    if (panel == panel_cues) {
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
    if (panel == panel_automation) {
      constexpr std::array<std::string_view, 2U> rows{
          "No automation lanes", "Use Timeline property diamonds"};
      draw_placeholder(bounds, "AUTOMATION LANES", rows);
      return;
    }
    if (panel == panel_macros) {
      constexpr std::array<std::string_view, 2U> rows{
          "No macros configured", "+  NEW MACRO (not available yet)"};
      draw_placeholder(bounds, "SHOW MACROS", rows);
      return;
    }
    if (panel == panel_timeline) {
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
          });
      result.filmstrip_requests = timeline_frame.filmstrip_requests;
      route_marker_edits(timeline_frame.marker_edits);
      if (timeline_frame.cues_armed.has_value()) {
        result.cues_armed = timeline_frame.cues_armed;
      }
      result.filmstrip_retain = timeline_frame.filmstrip_retain;
      if (timeline_frame.seek_to_frame.has_value()) {
        result.seek_to_frame = timeline_frame.seek_to_frame;
      }
      if (timeline_frame.transport != TransportRequest::None) {
        result.transport = timeline_frame.transport;
      }
      result.range = timeline_frame.range;
      result.show_rate = timeline_frame.show_rate;
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
                                          key.erase});
        }
      }
      if (!timeline_frame.key_moves.empty()) {
        const core::u64 gesture = impl.next_edit_gesture++;
        for (const TimelineKeyMoveEdit &move : timeline_frame.key_moves) {
          result.key_moves.push_back(KeyMove{move.target, move.component,
                                             move.from_frame, move.to_frame,
                                             gesture});
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
              impl.lane_edit_gesture, edit.destination});
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
    if (panel == panel_color_correction) {
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
    if (panel == panel_texture) {
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
    if (panel == panel_inspector || panel == panel_inspector_overview) {
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
                                            false, gesture, key.erase});
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
          const auto& request = *live_scene_settings.file_picker;
          if (auto path = choose_property_file(request)) {
            if (request.kind == FilePickerKind::BakeFolder) {
              impl.scene_settings_panel.set_bake_destination(std::move(*path));
            } else {
              auto values = live_scene_settings.project.apply.value_or(
                  ProjectSettingsValues{state.settings, state.renderer, state.environment, state.lens_profile});
              if (request.kind == FilePickerKind::ShowLut) values.renderer.color.show_lut_source = std::move(*path);
              if (request.kind == FilePickerKind::EnvironmentMap) values.environment.map_source = std::move(*path);
              live_scene_settings.project.apply = std::move(values);
            }
          }
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
        const auto& request = *inspector.file_picker;
        if (auto path = request.clear_texture ? std::optional<std::string>{std::string{}} : choose_property_file(request)) {
          if (path->size() > 1024U) {
            impl.ingest_message = "Resource path exceeds 1024 bytes";
          } else if (request.kind == FilePickerKind::ModelTexture && !request.targets.empty()) {
            const auto target = request.targets.front();
            const auto busy = std::any_of(impl.pending_model_ingests.begin(), impl.pending_model_ingests.end(),
                [&](const auto& pending) { return pending.texture_target == target; });
            if (busy || impl.pending_model_ingests.size() >= 16U) {
              impl.ingest_message = "A texture assignment is still preparing; wait for it to finish";
            } else {
              auto job = render::ModelIngestJob::start(request.model_source, {}, 1.0F,
                  render::ModelTextureAssignment{request.material_part, request.texture_role, *path});
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
              const auto gesture = impl.begin_gesture();
              result.generic_edits.push_back(GenericEdit{request.targets.front(),
                  std::string{texture_properties[request.texture_role]}, 0.0, gesture, *path});
            }
          } else {
            const auto gesture = impl.begin_gesture();
            for (const auto node : inspector.file_picker->targets)
              result.generic_edits.push_back(GenericEdit{node, std::string{scene::schema::ies_source}, 0.0, gesture, *path});
          }
        }
      }
      if (inspector.panel_request.has_value()) {
        pending_inspector_panel = inspector.panel_request;
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
                                          gesture, key.erase});
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
  };

  const float dock_height =
      std::max(height - top_bar_height - status_bar_height, 0.0F);
  impl.dock_host.update(ui, gui::Rect{0.0F, top_bar_height, width, dock_height},
                        draw_panel, editor_dock_metrics);
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
    outline(ui, bounds, ui.theme().accent);
    const std::string_view label = dragging_asset.has_value() ? "Replace source" : "Replace layer";
    const float label_width = ui.font_width(label) + 16.0F;
    const gui::Rect badge{bounds.x + bounds.width - label_width, bounds.y,
                           label_width, std::min(bounds.height, 24.0F)};
    ui.panel(badge, ui.theme().accent);
    ui.label({badge.x + 8.0F, badge.y, badge.width - 8.0F, badge.height},
             label, ui.theme().background);
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

  const auto delete_active_domain = [&]() {
    switch (impl.delete_domain) {
    case DeleteDomain::Scene:
      delete_selection();
      return;
    case DeleteDomain::Hardware: {
      if (!result.hardware_deletes.empty()) {
        return;
      }
      const HardwareSelection selected =
          impl.hardware_mapping_panel.selection();
      if (selected.kind != HardwareSelectionKind::None) {
        result.hardware_deletes.push_back(
            HardwareDeleteRequest{selected, impl.next_edit_gesture++});
      }
      return;
    }
    case DeleteDomain::Feed: {
      if (!result.feed_map_deletes.empty()) {
        return;
      }
      const std::optional<doc::NodeId> selected =
          impl.feed_mapping_panel.selected_map_node();
      if (selected.has_value()) {
        result.feed_map_deletes.push_back(
            FeedMapDeleteRequest{*selected, impl.next_edit_gesture++});
      }
      return;
    }
    case DeleteDomain::Surface: {
      if (!result.feed_surface_deletes.empty() ||
          !impl.stage_surface_selection.has_value()) {
        return;
      }
      result.feed_surface_deletes.push_back(FeedSurfaceDeleteRequest{
          *impl.stage_surface_selection, impl.next_edit_gesture++});
      return;
    }
    case DeleteDomain::Compositor: {
      if (compositor_locked || result.compositor_delete.has_value()) {
        return;
      }
      if (auto removed =
              impl.compositor_panel.selection_delete(compositor_graph)) {
        result.compositor_delete = std::move(removed);
        result.compositor_gesture = impl.next_edit_gesture++;
      }
      return;
    }
    case DeleteDomain::Neutral:
      return;
    }
  };
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

  if (pending_asset_drop.has_value() && stage_viewport_drawn &&
      !switcher.rehearsal_locked) {
    const CanvasPoint dropped =
        to_canvas(stage_viewport_frame.image, stage_scene.canvas,
                  pending_asset_drop->screen_x, pending_asset_drop->screen_y);
    if (dropped.inside) {
      const scene::Vec3 offset = scene::screen_drag_to_translation(
          stage_scene.camera, stage_scene.canvas, stage_scene.camera.target,
          dropped.x - static_cast<float>(stage_scene.canvas.width) * 0.5F,
          dropped.y - static_cast<float>(stage_scene.canvas.height) * 0.5F);
      // Several assets land side by side, a step apart, so none hides
      // another.
      const scene::Vec3 step = scene::screen_drag_to_translation(
          stage_scene.camera, stage_scene.canvas, stage_scene.camera.target,
          static_cast<float>(stage_scene.canvas.width) * 0.06F, 0.0F);
      const std::vector<doc::NodeId> &dropped_assets =
          pending_asset_drop->assets.empty()
              ? std::vector<doc::NodeId>{pending_asset_drop->asset}
              : pending_asset_drop->assets;
      for (std::size_t index = 0U; index < dropped_assets.size(); ++index) {
        const float along = static_cast<float>(index);
        instantiate_asset(
            dropped_assets[index],
            scene::Vec3{stage_scene.camera.target.x + offset.x + step.x * along,
                        stage_scene.camera.target.y + offset.y + step.y * along,
                        stage_scene.camera.target.z + offset.z +
                            step.z * along});
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
  const bool global_edit_shortcut_allowed =
      !impl.scene_link_details_open && !impl.blender_connection_open &&
      !impl.preferences_panel.is_open() &&
      !impl.project_settings_panel.is_open() && !editor_control_editing &&
      !ui.typing() && !ui.pointer_down() && !impl.manipulating;
  const bool scene_clipboard_domain =
      impl.history_context == HistoryContext::Scene &&
      (impl.delete_domain == DeleteDomain::Scene ||
       impl.delete_domain == DeleteDomain::Neutral);
  const bool menu_copy = menu_result.action == ApplicationMenuAction::Copy;
  const bool menu_paste = menu_result.action == ApplicationMenuAction::Paste;
  if ((menu_copy || menu_paste) && global_edit_shortcut_allowed &&
      !result.key_clipboard.has_value()) {
    const auto key_edit = impl.timeline_panel.key_clipboard_edit(
        menu_copy ? TimelineKeyClipboardAction::Copy
                  : TimelineKeyClipboardAction::Paste);
    if (key_edit.has_value()) {
      request_key_clipboard(*key_edit);
      clipboard_consumed = true;
    }
  }
  if (!clipboard_consumed && !result.clip_clipboard.has_value() &&
      !result.key_clipboard.has_value() && global_edit_shortcut_allowed &&
      scene_clipboard_domain) {
    if (menu_copy || ui.key_pressed(gui::Key::Copy)) {
      request_clip_clipboard(ClipClipboardAction::Copy);
    } else if (menu_paste || ui.key_pressed(gui::Key::Paste)) {
      request_clip_clipboard(ClipClipboardAction::Paste);
    }
  }
  if (!result.history.has_value() && global_edit_shortcut_allowed) {
    if (ui.key_pressed(gui::Key::Undo)) {
      result.history = HistoryRequest{HistoryAction::Undo,
                                      impl.history_context,
                                      impl.next_edit_gesture++};
    } else if (ui.key_pressed(gui::Key::Redo)) {
      result.history = HistoryRequest{HistoryAction::Redo,
                                      impl.history_context,
                                      impl.next_edit_gesture++};
    }
  }
  if (global_edit_shortcut_allowed && !delete_consumed &&
      ui.key_pressed(gui::Key::Delete)) {
    delete_active_domain();
  }

  if (draw_status_bar(
          ui, width, height, timeline, switcher, impl.scene_selection.size(),
          scene.canvas, project, impl.pending_media_probes.size(),
          !bake_activity.empty() ? std::string_view{bake_activity}
          : impl.pending_model_ingests.empty()
              ? std::string_view{impl.ingest_message}
              : render::ModelIngestJob::label(
                    impl.pending_model_ingests.front().job->stage()),
          impl.ui_zoom_percent)) {
    impl.open_menu = ApplicationMenu::View;
  }
  draw_application_menu_overlay(ui, impl.open_menu,
                                impl.recent_projects_submenu_open, impl.docks,
                                project.recent_projects, width);

  if (preferences_blocked_pointer) {
    ui.pop_pointer_block();
  }
  if (application_menu_blocks_pointer) ui.pop_pointer_block();
  if (impl.blender_connection_open) {
    const float card_width = std::min(720.0F, width - 24.0F);
    const gui::Rect card{(width-card_width)*0.5F, std::max((height-530.0F)*0.5F, 12.0F), card_width, 530.0F};
    ui.panel(card, ui.theme().panel);
    outline(ui, card, ui.theme().accent);
    ui.push_clip(card);
    ui.label({card.x+16,card.y+12,card.width-32,26},"CONNECT TO BLENDER",ui.theme().text);
    ui.label({card.x+16,card.y+44,card.width-32,22},"Link a Blender that is already open to the Preview scene. The B before a scene's name opens the project's own file.",ui.theme().text_muted);
    if (project.blender_instances.empty()) {
      ui.label({card.x+16,card.y+100,card.width-32,24},"Looking for Blender...",ui.theme().text);
      ui.label({card.x+16,card.y+132,card.width-32,24},"Open Blender and enable the Media Server LiveLink add-on.",ui.theme().text_muted);
      ui.label({card.x+16,card.y+164,card.width-32,24},"Available instances appear here automatically.",ui.theme().text_muted);
    } else {
      constexpr std::size_t per_page = 5;
      const auto pages = (project.blender_instances.size()+per_page-1)/per_page;
      impl.blender_page = std::min(impl.blender_page,pages-1);
      for (std::size_t i=impl.blender_page*per_page; i<std::min(project.blender_instances.size(),(impl.blender_page+1)*per_page); ++i) {
        const auto& instance = project.blender_instances[i];
        const float top = card.y+84+static_cast<float>(i%per_page)*48;
        std::string label = instance.label+" / "+instance.scene+" ["+instance.id.substr(0,6)+"]";
        // Each Blender edits one scene; say which, so a second Connect is a choice.
        for (const auto& [bound, name] : project.blender_bound_scenes)
          if (bound == instance.id) label += "  ->  " + name;
        if (chrome_button(ui,gui::ui_id("editor.blender.instance."+instance.id),
              {card.x+16,top,card.width-32,30},label,impl.blender_selection==instance.id)) impl.blender_selection=instance.id;
      }
      if (pages>1) {
        if (chrome_button(ui,gui::ui_id("editor.blender.previous"),{card.x+16,card.y+330,100,25},"Previous",false,impl.blender_page>0)) --impl.blender_page;
        if (chrome_button(ui,gui::ui_id("editor.blender.next"),{card.x+128,card.y+330,100,25},"Next",false,impl.blender_page+1<pages)) ++impl.blender_page;
      }
    }
    const auto active = std::find_if(project.blender_instances.begin(),project.blender_instances.end(),
        [&](const auto& item) { return item.id==project.blender_instance; });
    std::string status = active != project.blender_instances.end() ? active->status
        : project.blender_instance.empty() ? "Select an instance, then Connect." : "Waiting for Blender to become available...";
    if (!project.blender_error.empty()) status=project.blender_error;
    else if (active == project.blender_instances.end() && !project.scene_link_details.empty() && project.blender_connected) status=std::string{project.scene_link_details};
    std::istringstream lines{status}; std::string line; float y=card.y+370;
    while (std::getline(lines,line) && y<card.y+450) {
      while (!line.empty() && y<card.y+450) {
        const auto count=std::min<std::size_t>(line.size(),85);
        ui.label({card.x+16,y,card.width-32,20},std::string_view{line}.substr(0,count),ui.theme().text_muted);
        line.erase(0,count); y+=20;
      }
    }
    const bool available=std::any_of(project.blender_instances.begin(),project.blender_instances.end(),
        [&](const auto& item) { return item.id==impl.blender_selection; });
    if (chrome_button(ui,gui::ui_id("editor.blender.connect"),{card.x+16,card.y+482,130,30},"Connect",false,available))
      result.connect_blender=impl.blender_selection;
    // Resync and Disconnect act on the selected instance when it is linked,
    // else on the Preview scene's Blender.
    const bool selection_bound=std::any_of(project.blender_bound_scenes.begin(),project.blender_bound_scenes.end(),
        [&](const auto& item) { return item.first==impl.blender_selection; });
    const std::string acting=selection_bound ? impl.blender_selection : std::string{project.blender_instance};
    if (chrome_button(ui,gui::ui_id("editor.blender.resync"),{card.x+158,card.y+482,130,30},"Resync Scene",false,
          std::any_of(project.blender_instances.begin(),project.blender_instances.end(),[&](const auto& item) { return item.id==acting; })))
      result.resync_blender=acting;
    if (chrome_button(ui,gui::ui_id("editor.blender.disconnect"),{card.x+300,card.y+482,130,30},"Disconnect",false,!acting.empty()))
      result.disconnect_blender=acting;
    if (chrome_button(ui,gui::ui_id("editor.blender.close"),{card.x+card.width-116,card.y+482,100,30},"Close")
        || ui.key_pressed(gui::Key::Escape)) impl.blender_connection_open=false;
    ui.pop_clip();
  }
  if (impl.scene_link_details_open) {
    const float card_width = std::min(760.0F, width - 24.0F);
    const gui::Rect card{(width-card_width)*0.5F, std::max((height-664.0F)*0.5F, 20.0F), card_width, 664.0F};
    ui.panel(card, ui.theme().panel);
    outline(ui, card, ui.theme().accent);
    ui.push_clip(card);
    ui.label({card.x+16, card.y+10, card.width-32, 24}, "PUBLISH PREVIEW", ui.theme().text);
    const std::string details{project.scene_link_details.empty()
        ? "Connect a Blender scene from File > Connect to Blender." : project.scene_link_details};
    std::istringstream lines{details};
    std::string line; float y=card.y+45;
    while (std::getline(lines, line) && y<card.y+265) {
      // Status diagnostics can contain long paths; wrap without growing the card.
      do {
        const std::size_t count = std::min<std::size_t>(line.size(), 100);
        ui.label({card.x+16,y,card.width-32,21},std::string_view{line}.substr(0,count),ui.theme().text);
        line.erase(0,count); y+=21;
      } while (!line.empty() && y<card.y+265);
    }
    // Detailed diagnostics are separate from the connection workflow.
    ui.label({card.x+16,card.y+272,card.width-32,22},"NEGOTIATED CAPABILITIES AND MEASURED LINK",ui.theme().text);
    const std::string diagnostics{project.scene_link_diagnostics.empty()
        ? "No paired publisher yet. Capabilities and latency appear once one connects."
        : project.scene_link_diagnostics};
    {
      std::istringstream evidence{diagnostics};
      std::string row; float top=card.y+296;
      // Seven lines of evidence; the section is sized for all of them.
      while (std::getline(evidence,row) && top<card.y+440) {
        ui.label({card.x+16,top,card.width-32,20},std::string_view{row}.substr(0,100),ui.theme().text_muted);
        top+=20;
      }
    }
    ui.label({card.x+16,card.y+448,card.width-32,22},"PROGRAM PUBLICATION - shared storage visible at the same path on every node",ui.theme().text);
    if (!impl.publication_storage_editing) impl.publication_storage_draft=std::string{project.publication_storage};
    if (ui.text_field(gui::ui_id("editor.publication.storage"),{card.x+16,card.y+476,card.width-126,27},impl.publication_storage_draft)) impl.publication_storage_editing=true;
    if (chrome_button(ui,gui::ui_id("editor.publication.save"),{card.x+card.width-100,card.y+476,84,27},"Set folder")) {
      result.publication_storage=impl.publication_storage_draft; impl.publication_storage_editing=false;
    }
    if (impl.publication_storage_draft.empty() && !impl.publication_storage_editing && !project.publication_default_storage.empty())
      ui.label({card.x+24,card.y+476,card.width-150,27},"Default: "+std::string{project.publication_default_storage},ui.theme().text_muted);
    ui.label({card.x+16,card.y+510,card.width-32,24},project.publication_status,ui.theme().text_muted);
    if (chrome_button(ui,gui::ui_id("editor.publication.publish"),{card.x+16,card.y+546,156,30},"Publish Preview",false,
          !project.publication_scheduled && switcher.rehearsal_scene != scene::SceneId{})) result.publish_linked_scene=true;
    if (chrome_button(ui,gui::ui_id("editor.publication.cancel"),{card.x+188,card.y+546,156,30},"Cancel publication",false,
          project.publication_pending && !project.publication_scheduled)) result.cancel_linked_publication=true;
    if (chrome_button(ui,gui::ui_id("editor.publication.rollback"),{card.x+360,card.y+546,156,30},"Roll back Program",false,
          project.publication_rollback && !project.publication_pending)) result.rollback_linked_publication=true;
    if (chrome_button(ui,gui::ui_id("editor.scene-link.close"),{card.x+card.width-116,card.y+618,100,28},"Close")
        || ui.key_pressed(gui::Key::Escape)) impl.scene_link_details_open=false;
    ui.pop_clip();
  }
  if (impl.preferences_panel.is_open()) {
    const PreferencesFrame preferences_frame =
        impl.preferences_panel.draw(
            ui, gui::Rect{0.0F, 0.0F, width, height}, preferences);
    result.save_preferences = preferences_frame.save;
    result.render_node_password = preferences_frame.render_node_password;
    result.clear_proxy_cache = preferences_frame.clear_proxy_cache;
    if (!impl.preferences_panel.is_open()) {
      // A focused field must not keep the keyboard once the card is gone.
      ui.clear_focus();
    }
  }
  if (impl.bake_job && impl.bake_job->result()) {
    const auto& completed = *impl.bake_job->result();
    if (impl.bake_job->progress().stage.load() == render::BakeStage::Cancelled) impl.bake_message = "Bake cancelled";
    else if (completed.ready() && impl.bake_stage == project_settings.stage_node &&
             render::bake_input_digest(authored_stage) == impl.bake_snapshot_digest) {
      ProjectSettingsEdit edit{};
      edit.settings = project_settings.settings; edit.renderer = project_settings.renderer;
      edit.environment = project_settings.environment;
      edit.environment.baked = impl.bake_settings;
      edit.environment.baked.enabled = impl.bake_settings.enabled || std::none_of(impl.bake_settings.reflections.begin(), impl.bake_settings.reflections.end(), [](const auto& p) { return p.enabled; });
      edit.environment.baked.source = completed.source;
      edit.environment.baked.content = completed.asset->content;
      edit.environment.baked.input_revision = std::to_string(completed.asset->input_digest);
      edit.stage_node = impl.bake_stage; edit.node = project_settings.node;
      edit.lens_profile = project_settings.lens_profile; edit.camera_node = project_settings.camera_node;
      edit.gesture = impl.next_edit_gesture++;
      result.project_settings = edit;
      impl.bake_message = "Bake complete. Preparing the field on every Program node.";
      impl.completed_bake_content = completed.asset->content;
      impl.project_settings_panel.adopt_bake(edit.environment.baked);
    } else impl.bake_message = completed.ready() ? "Bake discarded: static geometry or lighting changed. Bake again." : completed.error;
    impl.bake_job.reset();
  }
  if (impl.project_settings_panel.is_open() || live_scene_settings.project.apply || live_scene_settings.project.bake || live_scene_settings.project.cancel_bake) {
    const bool modal_was_open = impl.project_settings_panel.is_open();
    // What the renderer actually did, filled here rather than by the caller
    // that assembles the rest: the program renderer is the thing that knows,
    // and it lives on this side of the boundary.
    ProjectSettingsState settings_view = project_settings;
    settings_view.passes = impl.program_renderer.renderer_passes();
    settings_view.reflection_anchor = impl.scene_selection.primary();
    settings_view.bake_running = impl.bake_job != nullptr;
    settings_view.bake_status = bake_activity;
    if (impl.bake_job) {
      const auto& progress = impl.bake_job->progress();
      settings_view.bake_status = std::string{render::bake_stage_label(progress.stage.load())};
      if (progress.total.load() != 0) settings_view.bake_status += " " + std::to_string(progress.completed.load()) + "/" + std::to_string(progress.total.load());
    }
    // And what became of the authored colour, from the display transform the
    // Program preview actually went through. Colour applies on the
    // scene-linear path alone, so a project handed Basic is told that rather
    // than told its look is loading.
    settings_view.color_status =
        !impl.program_renderer.scene_linear_output()
            ? ColorStatus::BackendUnsupported
            : color_status_for(impl.program_display.color_state());
    const ProjectSettingsFrame settings_frame = impl.project_settings_panel.is_open()
        ? impl.project_settings_panel.draw(ui, gui::Rect{0.0F, 0.0F, width, height}, settings_view)
        : live_scene_settings.project;
    if (settings_frame.cancel_bake && impl.bake_job) impl.bake_job->cancel();
    if (settings_frame.bake && !impl.bake_job) {
      auto snapshot = authored_stage;
      snapshot.environment = project_settings.environment;
      snapshot.environment.baked = *settings_frame.bake;
      impl.bake_settings = *settings_frame.bake;
      impl.completed_bake_content.clear();
      impl.bake_stage = project_settings.stage_node;
      impl.bake_snapshot_digest = render::bake_input_digest(authored_stage);
      impl.bake_job = render::BakeJob::start(std::move(snapshot), settings_frame.bake_destination);
      impl.bake_message = impl.bake_job ? "Bake queued" : "Bake worker busy; try again after the current bake.";
    }
    if (settings_frame.apply.has_value()) {
      ProjectSettingsEdit edit{};
      edit.settings = settings_frame.apply->settings;
      edit.renderer = settings_frame.apply->renderer;
      edit.environment = settings_frame.apply->environment;
      edit.stage_node = project_settings.stage_node;
      edit.node = project_settings.node;
      edit.lens_profile = settings_frame.apply->lens_profile;
      edit.camera_node = project_settings.camera_node;
      edit.gesture = modal_was_open ? impl.next_edit_gesture++ : impl.begin_gesture();
      result.project_settings = std::move(edit);
    }
    if (modal_was_open && !impl.project_settings_panel.is_open()) {
      ui.clear_focus();
    }
  }

  // A keyboard transform is one gesture from G to its confirm, though no
  // button is held through it.
  if (!ui.pointer_down() && !ui.typing() &&
      !impl.stage_panel.transforming()) {
    impl.manipulating = false;
  }

  const bool compositor_authored = std::any_of(result.source_replacements.begin(),
      result.source_replacements.end(), [](const auto &edit) { return edit.layer != doc::NodeId{}; }) ||
      !result.compositor_adds.empty() ||
      !result.compositor_moves.empty() ||
      result.compositor_clipboard.has_value() ||
      result.compositor_rename.has_value() ||
      result.compositor_comment.has_value() ||
      result.compositor_collapse.has_value() ||
      result.compositor_bypass.has_value() ||
      result.compositor_reorder.has_value() ||
      result.compositor_connect.has_value() ||
      result.compositor_rewire.has_value() ||
      result.compositor_waypoints.has_value() ||
      result.compositor_property_edit.has_value() ||
      result.compositor_delete.has_value();
  const bool selection_set_authored =
      result.selection_set_edit.has_value() &&
      result.selection_set_edit->action != SelectionSetEditAction::Recall;
  const bool scene_authored = !result.source_replacements.empty() || !result.edits.empty() || !result.keys.empty() ||
      !result.key_moves.empty() || result.key_duplicate.has_value() ||
      result.key_batch.has_value() ||
      !result.clip_timings.empty() ||
      !result.clip_splits.empty() || !result.lane_edits.empty() ||
      !result.element_flags.empty() || !result.renames.empty() ||
      !result.highlights.empty() ||
      !result.timeline_edits.empty() ||
      result.group_edit.has_value() ||
      selection_set_authored ||
      result.mask_edit.has_value() ||
      !result.property_edits.empty() ||
      !result.generic_edits.empty() || !result.adds.empty() ||
      !result.asset_deletes.empty() || !result.asset_proxy_edits.empty() ||
      !result.asset_revisions.empty() ||
      !result.asset_library_edits.empty() ||
      !result.effects.empty() || !result.film_grain_effect_edits.empty() ||
      !result.effect_deletes.empty() || !result.deletes.empty() ||
      result.project_settings.has_value() ||
      (result.clip_clipboard.has_value() &&
       result.clip_clipboard->action == ClipClipboardAction::Paste) ||
      (result.key_clipboard.has_value() &&
       result.key_clipboard->action == TimelineKeyClipboardAction::Paste);
  if (compositor_authored) {
    impl.history_context = HistoryContext::Compositor;
  } else if (scene_authored) {
    impl.history_context = HistoryContext::Scene;
  }
  if (result.history.has_value()) {
    // A pointer gesture can finish on the same frame as a keyboard history
    // chord. Navigate the context that gesture actually authored, not the one
    // that happened to be active on the preceding frame.
    result.history->context = impl.history_context;
  }
  if (ui.focus() != impl.semantic_focus) {
    impl.semantic_boundary = true;
    impl.semantic_focus = ui.focus();
  }
  const bool explicit_semantic_command =
      result.history.has_value() || result.clip_clipboard.has_value() ||
      result.key_clipboard.has_value() ||
      result.key_batch.has_value() ||
      !result.timeline_edits.empty() ||
      result.group_edit.has_value() ||
      result.selection_set_edit.has_value() ||
      result.mask_edit.has_value() ||
      result.seek_to_frame.has_value() || result.range.has_value() ||
      result.show_rate.has_value() ||
      result.transport != TransportRequest::None ||
      result.switcher.has_value() || result.save_project.has_value() ||
      result.load_project.has_value() || result.project_settings.has_value();
  if (explicit_semantic_command ||
      ((scene_authored || compositor_authored) &&
       !result.semantic_coalescing.has_value())) {
    impl.semantic_boundary = true;
  }
  result.close_semantic_coalescing = impl.semantic_boundary;

  // A drag can update a seam or floating rectangle every display frame. Wait
  // for release, then emit only the final arrangement so persistence never
  // turns a pointer gesture into a stream of disk writes.
  if (!ui.pointer_down()) {
    auto encoded = encode_workspace_layout(WorkspaceLayoutState{
        impl.docks.snapshot(), impl.workspace_window_width,
        impl.workspace_window_height, impl.stage_panel.resolution(),
        impl.program_panel.resolution(), impl.ui_zoom_percent,
        std::min(impl.compositor_panel.parameter_panel_width(),
                 max_compositor_parameter_width),
        impl.compositor_panel.minimap_shown()});
    if (encoded && encoded.value() != impl.saved_workspace_layout) {
      result.save_workspace_layout = std::move(encoded).value();
      impl.saved_workspace_layout = *result.save_workspace_layout;
    }
  }

  if (!stage_viewport_drawn) {
    impl.navigate_left = false;
    impl.navigate_middle = false;
    impl.navigate_right = false;
  }
  // A camera gesture owns the keyboard (W A S D Q E fly) until it ends, and
  // once it has moved the camera the cursor hides and stays put.
  const bool navigating =
      impl.navigate_left || impl.navigate_middle || impl.navigate_right;
  ui.set_navigating(navigating ||
                    (stage_viewport_drawn && impl.stage_panel.transforming()));
  impl.window.set_pointer_locked(navigating && impl.navigation_moved);

  // What is being dragged out of the Library rides above every panel.
  impl.library_panel.draw_drag_ghost(ui);
  ui.draw_overlays(gui::Rect{0.0F, 0.0F, width, height});
  impl.window.set_cursor(ui.cursor());
  static_cast<void>(impl.window.present(
      impl.draw_list, rhi::Color{0.043F, 0.047F, 0.059F, 1.0F}));
  return result;
}

} // namespace mediaserver::editor
