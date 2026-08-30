#include <mediaserver/editor/editor_shell.hpp>

#include <mediaserver/editor/library_thumbnail_texture_cache.hpp>
#include <mediaserver/editor/timeline_filmstrip_texture_cache.hpp>

#include <mediaserver/editor/color_correction_panel.hpp>
#include <mediaserver/editor/effects_panel.hpp>
#include <mediaserver/editor/inspector_panel.hpp>
#include <mediaserver/editor/mapping_panels.hpp>
#include <mediaserver/editor/masks_panel.hpp>
#include <mediaserver/editor/selection.hpp>
#include <mediaserver/editor/selection_sets_panel.hpp>
#include <mediaserver/editor/texture_panel.hpp>
#include <mediaserver/editor/viewport_panel.hpp>
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
#include <mediaserver/render/projection_mesh_loader.hpp>
#include <mediaserver/render/projection_mesh_warp.hpp>
#include <mediaserver/render/scene_mixer.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/manipulate.hpp>
#include <mediaserver/scene/math.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <span>
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

[[nodiscard]] std::optional<assets::MediaFrameRate>
probe_asset_frame_rate(const std::string &source) {
  const auto probed = render::probe_media_frame_rate(source);
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

[[nodiscard]] gui::UiId mix(gui::UiId base, std::string_view name) noexcept {
  core::u64 hash = base ^ 0x9E3779B97F4A7C15ULL;
  for (const char c : name) {
    hash ^= static_cast<core::u8>(c);
    hash *= 1099511628211ULL;
  }
  return hash;
}

[[nodiscard]] constexpr gui::Theme editor_theme() noexcept {
  gui::Theme theme{};
  theme.background = gui::UiColor{0.024F, 0.031F, 0.037F, 1.0F};
  theme.panel = gui::UiColor{0.061F, 0.073F, 0.084F, 1.0F};
  theme.header = gui::UiColor{0.082F, 0.098F, 0.112F, 1.0F};
  theme.row = gui::UiColor{0.071F, 0.084F, 0.096F, 1.0F};
  theme.row_alt = gui::UiColor{0.055F, 0.066F, 0.076F, 1.0F};
  theme.widget = gui::UiColor{0.075F, 0.090F, 0.104F, 1.0F};
  theme.widget_hover = gui::UiColor{0.106F, 0.129F, 0.149F, 1.0F};
  theme.widget_active = gui::UiColor{0.000F, 0.345F, 0.565F, 1.0F};
  theme.accent = gui::UiColor{0.000F, 0.620F, 0.890F, 1.0F};
  theme.selection = gui::UiColor{0.000F, 0.275F, 0.455F, 1.0F};
  theme.track = gui::UiColor{0.027F, 0.035F, 0.043F, 1.0F};
  theme.text = gui::UiColor{0.800F, 0.835F, 0.855F, 1.0F};
  theme.text_muted = gui::UiColor{0.430F, 0.475F, 0.510F, 1.0F};
  theme.divider = gui::UiColor{0.016F, 0.024F, 0.031F, 1.0F};
  theme.playhead = gui::UiColor{0.000F, 0.650F, 0.930F, 1.0F};
  theme.keyframe = gui::UiColor{0.620F, 0.890F, 1.000F, 1.0F};
  theme.good = gui::UiColor{0.250F, 0.840F, 0.370F, 1.0F};
  theme.warning = gui::UiColor{0.960F, 0.360F, 0.300F, 1.0F};
  theme.padding = 5.0F;
  return theme;
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

[[nodiscard]] const rhi::Texture *
record_editor_scene(render::CompositorSceneExecutor &executor,
                     rhi::CommandList &commands, const scene::Scene &frame,
                     const compositor::CompositorGraph &graph,
                     render::MediaCache *shared_media_cache) noexcept {
  const scene::CanvasRect slice = scene::whole(frame.canvas);
  if (!graph.valid() || graph.plan.steps.empty()) {
    auto rendered =
        executor.execute(commands, frame, slice, shared_media_cache);
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
        executor.execute(commands, frame, slice, shared_media_cache);
    return rendered ? rendered.value() : nullptr;
  }
  auto rendered =
      executor.execute(commands, graph, frame, slice, shared_media_cache);
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
    const topology::HardwareDevice *match{};
    for (const topology::HardwareDevice &device : hardware_map.devices) {
      if (device.kind != topology::HardwareDeviceKind::RenderServer ||
          device.node_index != map->node_index) {
        continue;
      }
      if (match != nullptr) {
        match = nullptr;
        break;
      }
      match = &device;
    }
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
  bool undo{};
  bool redo{};
  bool program{};
  bool preview{};
  bool swap{};
  bool open_setup{};
};

enum class ApplicationMenu : core::u8 {
  None,
  File,
  Window,
};

enum class ApplicationMenuAction : core::u8 {
  None,
  OpenProject,
  SaveProject,
  SaveProjectAs,
  OpenPreferences,
  OpenProjectSettings,
  TogglePanel,
};

struct ApplicationMenuResult final {
  ApplicationMenuAction action{ApplicationMenuAction::None};
  gui::PanelId panel{};
};

struct MenuBarEntry final {
  std::string_view label;
  ApplicationMenu menu{ApplicationMenu::None};
  bool setup{};
};

constexpr std::array<MenuBarEntry, 9U> menu_bar_entries{{
    {"File", ApplicationMenu::File, false},
    {"Edit", ApplicationMenu::None, false},
    {"Scene", ApplicationMenu::None, false},
    {"Setup", ApplicationMenu::None, true},
    {"Layers", ApplicationMenu::None, false},
    {"Tools", ApplicationMenu::None, false},
    {"Output", ApplicationMenu::None, false},
    {"Window", ApplicationMenu::Window, false},
    {"Help", ApplicationMenu::None, false},
}};

struct WindowPanelEntry final {
  gui::PanelId panel{};
  std::string_view title;
};

[[nodiscard]] const std::array<WindowPanelEntry, 20U> &window_panel_entries() {
  static const std::array<WindowPanelEntry, 20U> entries{{
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
    return gui::Rect{anchor.x, top_bar_height, 220.0F, 106.0F};
  }
  if (menu == ApplicationMenu::Window) {
    const gui::Rect anchor = menu_bar_entry_bounds(ui, "Window");
    return gui::Rect{anchor.x, top_bar_height, 238.0F,
                     static_cast<float>(window_panel_entries().size()) * 21.0F +
                         8.0F};
  }
  return {};
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

[[nodiscard]] std::string_view
projection_mesh_error_message(render::ProjectionMeshLoadError error) noexcept {
  switch (error) {
  case render::ProjectionMeshLoadError::InvalidSource:
    return "Select a glTF, GLB, or OBJ mesh file.";
  case render::ProjectionMeshLoadError::ModelLoadFailed:
    return "The selected mesh could not be read or contains no geometry.";
  case render::ProjectionMeshLoadError::InvalidCalibration:
    return "The surface calibration is invalid. Check its orientation and "
           "scale.";
  case render::ProjectionMeshLoadError::CapacityExceeded:
    return "The mesh is too dense for one projection output.";
  case render::ProjectionMeshLoadError::InvalidGeometry:
    return "The mesh contains invalid or degenerate geometry.";
  case render::ProjectionMeshLoadError::MissingOrDegenerateUv:
    return "The mesh needs a usable UV0 map for output placement.";
  case render::ProjectionMeshLoadError::UvOutsideConnector:
    return "UV0 must stay inside the connector range from 0 to 1.";
  case render::ProjectionMeshLoadError::AllocationFailed:
    return "There was not enough memory to import this projection mesh.";
  }
  return "The selected projection mesh is invalid.";
}

struct ProjectionMeshChoice final {
  std::string source;
  core::u64 vertex_count{};
  core::u64 triangle_count{};
};

[[nodiscard]] std::optional<ProjectionMeshChoice>
choose_projection_mesh(const output::PhysicalSurface &calibration,
                       float scale) {
  const char *const filters[]{"*.gltf", "*.glb", "*.obj"};
  const char *const path =
      tinyfd_openFileDialog("Choose UV Projection Mesh", "", 3, filters,
                            "UV projection meshes (glTF, GLB, OBJ)", 0);
  if (path == nullptr) {
    return std::nullopt;
  }

  const std::string source{path};
  const auto loaded = render::load_projection_mesh(source, calibration, scale);
  if (!loaded) {
    const std::string_view message =
        projection_mesh_error_message(loaded.error());
    static_cast<void>(tinyfd_messageBox("Invalid UV Projection Mesh",
                                        message.data(), "ok", "error", 1));
    return std::nullopt;
  }
  return ProjectionMeshChoice{
      source, static_cast<core::u64>(loaded.value().vertices.size()),
      static_cast<core::u64>(loaded.value().indices.size() / 3U)};
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
                        ApplicationMenu &open_menu) {
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
  // group an exact width so its labels and the history actions stay stable
  // when the window is resized.
  constexpr float modes_width = 268.0F;
  constexpr float modes_right_margin = 64.0F;
  const float modes_x =
      std::max(width - modes_width - modes_right_margin, x + 16.0F);
  const float workspace_x = x + 12.0F;
  const float workspace_width = std::max(modes_x - workspace_x - 12.0F, 0.0F);
  if (workspace_width > 90.0F) {
    const gui::Rect show_tab{workspace_x, 4.0F,
                             std::min(workspace_width, 132.0F), 26.0F};
    ui.panel(show_tab, ui.theme().header);
    ui.panel(gui::Rect{show_tab.x, show_tab.y, 2.0F, show_tab.height},
             ui.theme().accent);
    ui.label(gui::Rect{show_tab.x + 10.0F, show_tab.y, show_tab.width - 10.0F,
                       show_tab.height},
             "MAIN_SEQUENCE", ui.theme().text);
    if (workspace_width > 236.0F) {
      ui.label(gui::Rect{show_tab.x + show_tab.width + 18.0F, 0.0F,
                         workspace_width - show_tab.width - 18.0F,
                         top_bar_height},
               "SHOW_CONTROL    STAGE_PREVIEW", ui.theme().text_muted);
    }
  }

  float mode_x = modes_x;
  actions.program = chrome_button(
      ui, gui::ui_id("editor.mode.program"),
      gui::Rect{mode_x, 6.0F, 66.0F, 22.0F}, "PROGRAM",
      !switcher.rehearsal_mode, !switcher.rehearsal_locked);
  mode_x += 70.0F;
  const bool has_rehearsal = switcher.rehearsal_scene != scene::SceneId{};
  const bool canvas_mismatch = has_rehearsal && !switcher.dissolve_compatible;
  // Swap is held by the switcher's own preconditions - a Preview scene to
  // promote and no transition already running - and by nothing else except a
  // STRICT synchronization policy whose timing is unproven. A dissolve also
  // needs a matching canvas; the button says CUT rather than failing silently.
  const bool swap_enabled = !switcher.transition_active &&
                            !switcher.rehearsal_locked && has_rehearsal &&
                            !switcher.program_held;
  actions.swap = chrome_button(
      ui, gui::ui_id("editor.mode.swap"), gui::Rect{mode_x, 6.0F, 62.0F, 22.0F},
      switcher.transition_active
          ? "FADING"
          : (switcher.program_held ? "HELD"
                                   : (canvas_mismatch ? "CUT" : "SWAP")),
      false, swap_enabled);
  mode_x += 66.0F;
  actions.preview = chrome_button(
      ui, gui::ui_id("editor.mode.preview"),
      gui::Rect{mode_x, 6.0F, 66.0F, 22.0F}, "PREVIEW",
      switcher.rehearsal_mode, !switcher.rehearsal_locked);
  mode_x += 72.0F;
  actions.undo = chrome_button(ui, gui::ui_id("editor.toolbar.undo"),
                               gui::Rect{mode_x, 6.0F, 28.0F, 22.0F}, "<");
  mode_x += 32.0F;
  actions.redo = chrome_button(ui, gui::ui_id("editor.toolbar.redo"),
                               gui::Rect{mode_x, 6.0F, 28.0F, 22.0F}, ">");
  return actions;
}

[[nodiscard]] ApplicationMenuResult
run_application_menu_input(gui::UiContext &ui, float width, float height,
                           ApplicationMenu &open_menu,
                           const gui::DockLayout &docks) {
  ApplicationMenuResult result{};
  const gui::UiId dismiss_id = gui::ui_id("editor.menu.dismiss");
  const gui::Rect whole{0.0F, 0.0F, width, height};
  if (ui.active() == dismiss_id) {
    static_cast<void>(ui.grab(dismiss_id, whole));
  }
  if (open_menu == ApplicationMenu::None) {
    return result;
  }

  const gui::Rect popup = menu_popup_bounds(ui, open_menu);
  const gui::Rect anchor = menu_bar_entry_bounds(
      ui, open_menu == ApplicationMenu::File ? "File" : "Window");
  if (ui.pointer_pressed() && !popup.contains(ui.pointer_x(), ui.pointer_y()) &&
      !anchor.contains(ui.pointer_x(), ui.pointer_y())) {
    if (ui.pointer_y() >= top_bar_height) {
      const gui::Interaction dismissed = ui.grab(dismiss_id, whole);
      if (dismissed.pressed) {
        open_menu = ApplicationMenu::None;
      }
    } else {
      open_menu = ApplicationMenu::None;
    }
    return result;
  }

  if (open_menu == ApplicationMenu::File) {
    constexpr std::array<std::pair<std::string_view, ApplicationMenuAction>, 5U>
        items{{
            {"Open Project...", ApplicationMenuAction::OpenProject},
            {"Save Project", ApplicationMenuAction::SaveProject},
            {"Save Project As...", ApplicationMenuAction::SaveProjectAs},
            {"Project Settings...", ApplicationMenuAction::OpenProjectSettings},
            {"Preferences...", ApplicationMenuAction::OpenPreferences},
        }};
    float y = popup.y + 4.0F;
    for (const auto &[label, action] : items) {
      if (action == ApplicationMenuAction::OpenProjectSettings) {
        y += 8.0F;
      }
      const gui::Rect row{popup.x + 4.0F, y, popup.width - 8.0F, 22.0F};
      if (ui.grab(mix(gui::ui_id("editor.file-menu"), label), row).clicked) {
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
                                   const gui::DockLayout &docks) {
  if (open_menu == ApplicationMenu::None) {
    return;
  }
  const gui::Rect popup = menu_popup_bounds(ui, open_menu);
  ui.panel(popup, ui.theme().background);
  outline(ui, popup, ui.theme().divider);
  ui.push_clip(popup);

  const auto draw_row = [&](gui::UiId id, const gui::Rect &row,
                            std::string_view label) {
    const bool hovered = row.contains(ui.pointer_x(), ui.pointer_y());
    if (hovered || ui.active() == id) {
      ui.panel(row,
               hovered ? ui.theme().widget_hover : ui.theme().widget_active);
    }
    ui.label(gui::Rect{row.x + 8.0F, row.y, row.width - 12.0F, row.height},
             label, hovered ? ui.theme().text : ui.theme().text_muted);
  };

  if (open_menu == ApplicationMenu::File) {
    constexpr std::array<std::string_view, 5U> labels{
        "Open Project...", "Save Project", "Save Project As...",
        "Project Settings...", "Preferences..."};
    float y = popup.y + 4.0F;
    for (const std::string_view label : labels) {
      if (label == "Project Settings...") {
        // Above the rule the File menu acts on the project file. Below it are
        // the two settings dialogs - one document, one machine - and the rule
        // is what makes that distinction visible at a glance.
        ui.panel(gui::Rect{popup.x + 8.0F, y + 3.0F, popup.width - 16.0F, 1.0F},
                 ui.theme().divider);
        y += 8.0F;
      }
      const gui::Rect row{popup.x + 4.0F, y, popup.width - 8.0F, 22.0F};
      draw_row(mix(gui::ui_id("editor.file-menu"), label), row, label);
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
}

void draw_status_bar(gui::UiContext &ui, float width, float height,
                     const EditorTimeline &timeline,
                     const EditorSwitcherState &switcher,
                     std::size_t selection_count, scene::Canvas canvas,
                     const EditorProjectState &project) {
  const gui::Rect status{0.0F, std::max(height - status_bar_height, 0.0F),
                         width, status_bar_height};
  ui.panel(status, ui.theme().background);
  ui.panel(gui::Rect{status.x, status.y, status.width, 1.0F},
           ui.theme().divider);

  float x = status.x;
  const auto segment = [&](std::string_view text, float segment_width,
                           gui::UiColor color) {
    ui.panel(gui::Rect{x + segment_width - 1.0F, status.y + 5.0F, 1.0F,
                       status.height - 10.0F},
             ui.theme().divider);
    ui.label(
        gui::Rect{x + 10.0F, status.y, segment_width - 12.0F, status.height},
        text, color);
    x += segment_width;
  };

  std::string project_label{"PROJECT  "};
  if (project.path.empty()) {
    project_label.append("UNTITLED");
  } else {
    project_label.append(native_path_to_utf8(
        native_path_from_utf8(project.path).filename()));
  }
  if (project.dirty) {
    project_label.push_back('*');
  }
  segment(project_label, 190.0F,
          project.dirty ? ui.theme().warning : ui.theme().text_muted);
  // Preview owns its own playhead, so an operator scrubbing a rehearsal scene
  // must still be able to read where the live show is without leaving Preview.
  std::array<char, 48U> program_text{};
  const int program_length = std::snprintf(
      program_text.data(), program_text.size(), "%s  PGM %llu",
      switcher.rehearsal_mode ? "PREVIEW" : "PROGRAM",
      static_cast<unsigned long long>(timeline.program_frame));
  segment(std::string_view{program_text.data(),
                          static_cast<std::size_t>(
                              std::max(program_length, 0))},
          160.0F,
          switcher.rehearsal_mode ? ui.theme().accent : ui.theme().good);
  segment(timeline.playing ? "TIMELINE  PLAYING" : "TIMELINE  PAUSED", 132.0F,
          timeline.playing ? ui.theme().good : ui.theme().text_muted);
  std::array<char, 32U> canvas_text{};
  const int canvas_length =
      std::snprintf(canvas_text.data(), canvas_text.size(), "%u x %u",
                    canvas.width, canvas.height);
  segment(std::string_view{canvas_text.data(), static_cast<std::size_t>(
                                                   std::max(canvas_length, 0))},
          108.0F, ui.theme().text_muted);

  std::array<char, 48U> status_text{};
  const int status_length = std::snprintf(
      status_text.data(), status_text.size(), "FRAME %llu   SEL %zu",
      static_cast<unsigned long long>(timeline.show_frame), selection_count);
  const float remaining = std::max(width - x, 0.0F);
  ui.label(
      gui::Rect{x + 12.0F, status.y, std::max(remaining - 126.0F, 0.0F),
                status.height},
      std::string_view{status_text.data(),
                       static_cast<std::size_t>(std::max(status_length, 0))},
      ui.theme().text);

  if (!project.message.empty()) {
    const float message_width = std::min(std::max(remaining, 0.0F), 310.0F);
    const bool error = project.message.starts_with("ERROR");
    ui.label(gui::Rect{width - message_width - 10.0F, status.y, message_width,
                       status.height},
             project.message, error ? ui.theme().warning : ui.theme().good);
  } else if (remaining > 130.0F) {
    ui.label(gui::Rect{width - 118.0F, status.y, 108.0F, status.height},
             "LOCAL PREVIEW", ui.theme().text_muted);
  }
}

[[nodiscard]] gui::FontAtlas load_editor_font() {
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
  for (const char *path : candidates) {
    auto atlas = gui::FontAtlas::create(path, 12U);
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
  if (world == nullptr || (primitive != scene::Primitive::Quad &&
                           primitive != scene::Primitive::Plane)) {
    return std::nullopt;
  }
  const std::array<scene::Vec3, 4U> local =
      primitive == scene::Primitive::Quad
          ? std::array<scene::Vec3, 4U>{
                scene::Vec3{-0.5F, 0.5F, 0.0F},
                scene::Vec3{0.5F, 0.5F, 0.0F},
                scene::Vec3{0.5F, -0.5F, 0.0F},
                scene::Vec3{-0.5F, -0.5F, 0.0F}}
          : std::array<scene::Vec3, 4U>{
                scene::Vec3{-0.5F, 0.0F, -0.5F},
                scene::Vec3{0.5F, 0.0F, -0.5F},
                scene::Vec3{0.5F, 0.0F, 0.5F},
                scene::Vec3{-0.5F, 0.0F, 0.5F}};
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
  return mapping;
}

} // namespace

bool editor_available() noexcept {
  return render::CompositorSceneExecutor::shaders_available() &&
         render::SceneMixer::shaders_available() &&
         render::ProjectionMeshWarp::shaders_available() &&
         gui::UiRenderer::shaders_available() && rhi::Device::available();
}

RoutedSurfaceEdit route_surface_transform_edit(
    FeedSurfaceEditRequest request,
    std::span<const scene::ChannelKeyframes> keyframes, core::u64 show_frame) {
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
    if (keyed(keyframes, request.node, component.channel)) {
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
  // Window first: its device outlives both renderers and the UI context that
  // records their targets into the draw list.
  gui::UiWindow window;
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
  render::CompositorSceneExecutor stage_renderer;
  render::OutputProjectionCache program_projection_cache;

  gui::DockLayout docks;
  gui::DockHost dock_host;
  ViewportPanel program_panel{gui::ui_id("editor.viewport.program"),
                              ViewportRole::Program};
  ViewportPanel stage_panel{gui::ui_id("editor.viewport.stage"),
                            ViewportRole::Stage};
  InspectorPanel inspector_panel{gui::ui_id("editor.inspector")};
  TimelinePanel timeline_panel{gui::ui_id("editor.timeline")};
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
  PreferencesPanel preferences_panel{gui::ui_id("editor.preferences")};
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
  std::optional<doc::NodeId> stage_surface_selection;
  std::optional<doc::NodeId> program_output_map_node;
  scene::SceneId displayed_scene{};
  std::size_t scene_list_offset{};
  core::u64 project_generation{};
  DeleteDomain delete_domain{DeleteDomain::Scene};
  ApplicationMenu open_menu{ApplicationMenu::None};
  double hardware_warning_until{};
  double compositor_warning_until{};

  // The Stage camera orbits an editor-local point and never enters the
  // document. Angles are turns, matching scene math everywhere else.
  float camera_yaw{0.10F};
  float camera_pitch{0.06F};
  float camera_distance{9.0F};
  scene::Vec3 camera_target{};
  bool orbiting{};

  // The Stage renderer follows the dock's actual content aspect (one frame
  // behind a resize), so the authoring view fills its pane instead of looking
  // like a letterboxed program feed. Program keeps the authored canvas.
  core::u32 stage_surface_width{};
  core::u32 stage_surface_height{};

  // A viewport drag captures every selected element's starting position.
  // The panel reports one world delta, which is added to each start so a
  // group moves rigidly even while the committed scene lags behind the drag.
  std::vector<doc::NodeId> group_nodes;
  std::vector<scene::Vec3> group_start;

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
  bool manipulating{};
  bool semantic_boundary{};
  gui::UiId semantic_focus{};

  Impl(gui::UiWindow &&created_window, gui::FontAtlas &&created_font,
       render::CompositorSceneExecutor &&program,
       render::CompositorSceneExecutor &&program_to,
       render::ProjectionMeshWarp &&program_projection,
       render::ProjectionMeshWarp &&program_to_projection,
       render::SceneMixer &&mixer,
       render::CompositorSceneExecutor &&stage)
      : window(std::move(created_window)), font(std::move(created_font)),
        ui(input, draw_list, font, editor_theme()),
        library_thumbnail_textures(window.device()),
        timeline_filmstrip_textures(window.device()),
        program_renderer(std::move(program)),
        program_to_renderer(std::move(program_to)),
        program_warp(std::move(program_projection)),
        program_to_warp(std::move(program_to_projection)),
        program_mixer(std::move(mixer)), stage_renderer(std::move(stage)),
        dock_host(docks) {
    // The default workspace mirrors a live-production editor: a broad Stage
    // canvas in the centre; hierarchy/library and properties/output rails;
    // time and compositing surfaces stacked below. Every region is still a
    // real dock leaf, so this reference-like starting point remains fully
    // rearrangeable.
    const gui::DockId stage_dock = docks.add(panel_stage, "VIEW");
    const gui::DockId timeline_dock = docks.dock(
        panel_timeline, "TIMELINE", stage_dock, gui::DockSide::Bottom, 0.45F);
    static_cast<void>(
        docks.dock(panel_nodes, "NODES", timeline_dock, gui::DockSide::Center));
    static_cast<void>(
        docks.dock(panel_cues, "CUES", timeline_dock, gui::DockSide::Center));
    static_cast<void>(docks.dock(panel_automation, "AUTOMATION", timeline_dock,
                                 gui::DockSide::Center));
    docks.set_active(timeline_dock, panel_timeline);
    const gui::DockId compositor_dock =
        docks.dock(panel_compositor, "COMPOSITOR", timeline_dock,
                   gui::DockSide::Bottom, 0.31F);
    docks.set_active(compositor_dock, panel_compositor);

    const gui::DockId scene_dock = docks.dock(
        panel_scene, "SCENE", docks.root(), gui::DockSide::Left, 0.20F);
    static_cast<void>(docks.dock(panel_selection_sets, "SELECTION SETS",
                                 scene_dock, gui::DockSide::Center));
    docks.set_active(scene_dock, panel_scene);
    const gui::DockId library_dock = docks.dock(
        panel_library, "LIBRARY", scene_dock, gui::DockSide::Bottom, 0.40F);
    static_cast<void>(docks.dock(panel_macros, "MACROS", library_dock,
                                 gui::DockSide::Center));
    static_cast<void>(docks.dock(panel_effects, "EFFECTS", library_dock,
                                 gui::DockSide::Center));
    docks.set_active(library_dock, panel_library);

    const gui::DockId inspector_dock =
        docks.dock(panel_inspector_overview, "INSPECTOR", docks.root(),
                   gui::DockSide::Right, 0.22F);
    static_cast<void>(docks.dock(panel_inspector, "PROPERTIES", inspector_dock,
                                 gui::DockSide::Center));
    static_cast<void>(docks.dock(panel_color_correction, "COLOR",
                                 inspector_dock, gui::DockSide::Center));
    static_cast<void>(docks.dock(panel_texture, "TEXTURE", inspector_dock,
                                 gui::DockSide::Center));
    static_cast<void>(docks.dock(panel_masks, "MASKS", inspector_dock,
                                 gui::DockSide::Center));
    docks.set_active(inspector_dock, panel_inspector);
    static_cast<void>(docks.dock(panel_output, "OUTPUT MONITOR", inspector_dock,
                                 gui::DockSide::Bottom, 0.19F));

    // Program Output starts as the reference's monitor over Stage, but it
    // is a real dock panel rather than painted picture-in-picture. Its tab
    // can be dropped beside, beneath, or into any other leaf; dropping it
    // over open space detaches it again.
    static_cast<void>(docks.dock(panel_program, "PROGRAM OUTPUT", stage_dock,
                                 gui::DockSide::Center));
    const float monitor_width =
        std::clamp(static_cast<float>(window.width()) * 0.19F, 280.0F, 360.0F);
    const float monitor_height = monitor_width * 9.0F / 16.0F + 68.0F;
    static_cast<void>(docks.detach(
        panel_program, gui::Rect{static_cast<float>(window.width()) * 0.585F,
                                 static_cast<float>(window.height()) * 0.29F,
                                 monitor_width, monitor_height}));
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
    show_mapping_panel(panel_hardware_mapping);
    show_mapping_panel(panel_feed_mapping);
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
    pending_group_members.clear();
    pending_group_frames = 0U;
    scene_selection.replace(node);
    delete_domain = DeleteDomain::Scene;
    history_context = HistoryContext::Scene;
    semantic_boundary = true;
  }

  void toggle(doc::NodeId node) {
    pending_group_members.clear();
    pending_group_frames = 0U;
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
    scene_selection.select_range(node, visible_order);
    delete_domain = DeleteDomain::Scene;
    history_context = HistoryContext::Scene;
    semantic_boundary = true;
  }

  void clear_scene_selection(bool activate_scene_history) {
    pending_group_members.clear();
    pending_group_frames = 0U;
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

EditorShell::~EditorShell() = default;

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

  gui::WindowConfig window_config{};
  window_config.title = config.title;
  window_config.width = config.width;
  window_config.height = config.height;
  window_config.position_x = config.position_x;
  window_config.position_y = config.position_y;
  window_config.vsync = true;
  auto created_window = gui::UiWindow::create(window_config);
  if (!created_window) {
    return Result::failure(map_window_error(created_window.error()));
  }
  gui::UiWindow window = std::move(created_window).value();

  gui::FontAtlas font = load_editor_font();
  if (!window.set_atlas(font)) {
    return Result::failure(EditorError::AllocationFailed);
  }

  auto program = render::CompositorSceneExecutor::create(
      window.device(), config.width, config.height);
  if (!program) {
    return Result::failure(EditorError::Unavailable);
  }
  auto program_to = render::CompositorSceneExecutor::create(
      window.device(), config.width, config.height);
  if (!program_to) {
    return Result::failure(EditorError::Unavailable);
  }
  auto program_warp = render::ProjectionMeshWarp::create(
      window.device(), config.width, config.height);
  if (!program_warp) {
    return Result::failure(EditorError::Unavailable);
  }
  auto program_to_warp = render::ProjectionMeshWarp::create(
      window.device(), config.width, config.height);
  if (!program_to_warp) {
    return Result::failure(EditorError::Unavailable);
  }
  auto mixer =
      render::SceneMixer::create(window.device(), config.width, config.height);
  if (!mixer) {
    return Result::failure(EditorError::Unavailable);
  }
  auto stage = render::CompositorSceneExecutor::create(
      window.device(), config.width, config.height);
  if (!stage) {
    return Result::failure(EditorError::Unavailable);
  }

  auto *raw = new (std::nothrow)
      Impl{std::move(window),          std::move(font),
           std::move(program).value(), std::move(program_to).value(),
           std::move(program_warp).value(),
           std::move(program_to_warp).value(), std::move(mixer).value(),
           std::move(stage).value()};
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
                       filmstrips) noexcept {
  Impl &impl = *impl_;
  EditorFrame result{};
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
  if (impl.project_generation != project.generation) {
    impl.semantic_boundary = true;
    impl.project_generation = project.generation;
    impl.scene_selection.clear();
    impl.selection_set_selection = {};
    impl.mask_selection = {};
    impl.mask_coverage_visible = false;
    impl.pending_group_members.clear();
    impl.pending_group_frames = 0U;
    impl.stage_surface_selection.reset();
    impl.program_output_map_node.reset();
    impl.program_projection_cache.reset();
    impl.group_nodes.clear();
    impl.group_start.clear();
    impl.manipulating = false;
    impl.orbiting = false;
    impl.hardware_edit_gesture = 0U;
    impl.feed_mapping_edit_gesture = 0U;
    impl.compositor_edit_gesture = 0U;
    impl.clip_edit_gesture = 0U;
    impl.lane_edit_gesture = 0U;
    impl.delete_domain = DeleteDomain::Neutral;
    impl.ui.clear_focus();
    impl.timeline_panel.reset_project_state();
    impl.hardware_mapping_panel.reset_project_state();
    impl.feed_mapping_panel.reset_project_state();
    impl.compositor_panel.reset_project_state();
  }
  if (impl.scene_selection.erase_if([&](doc::NodeId node) {
        const ElementInfo *const element = find_element(elements, node);
        return element != nullptr && element->locked;
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

  // The free Stage view is a copy of the same evaluated scene with only its
  // camera replaced. Program continues to show the exact replicated camera.
  scene::Camera stage_camera{};
  stage_camera.target = impl.camera_target;
  {
    const float cp = scene::deterministic::cos_turns(impl.camera_pitch);
    const float sp = scene::deterministic::sin_turns(impl.camera_pitch);
    const float cy = scene::deterministic::cos_turns(impl.camera_yaw);
    const float sy = scene::deterministic::sin_turns(impl.camera_yaw);
    stage_camera.eye =
        impl.camera_target + scene::Vec3{impl.camera_distance * cp * sy,
                                         impl.camera_distance * sp,
                                         impl.camera_distance * cp * cy};
    stage_camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    stage_camera.field_of_view = 0.14F;
    stage_camera.near_plane = 0.05F;
    stage_camera.far_plane = 400.0F;
  }
  const scene::Scene &authored_stage =
      switcher.stage_scene != nullptr ? *switcher.stage_scene : scene;
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
  if (impl.displayed_scene != authored_stage.id) {
    impl.semantic_boundary = true;
    impl.displayed_scene = authored_stage.id;
    impl.timeline_panel.clear_key_context();
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
  scene::Scene stage_scene = authored_stage;
  stage_scene.camera = stage_camera;
  if (impl.stage_surface_width > 0U && impl.stage_surface_height > 0U) {
    stage_scene.canvas =
        scene::Canvas{impl.stage_surface_width, impl.stage_surface_height};
  }
  // The free editor camera uses a restrained neutral surround. Program keeps
  // the authored environment exactly; Stage gives saturated media, selection
  // edges, and spatial guides enough contrast to read like an authoring view.
  stage_scene.environment.sky_color = scene::Vec3{0.002F, 0.004F, 0.008F};
  stage_scene.environment.ground_color = scene::Vec3{0.008F, 0.011F, 0.016F};

  // Both scene targets are rendered before the UI pass. They belong to the
  // same device as UiWindow, so the panels sample them directly with no copy.
  rhi::Device &device = impl.window.device();
  const output::OutputMap *const program_output =
      selected_program_output(feed_map, impl.program_output_map_node);
  const ProgramOutputExtent program_extent =
      program_output_extent(scene, program_output, hardware_map);
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
                                program_extent.preview_height);
  static_cast<void>(impl.stage_renderer.resize(device, stage_scene.canvas.width,
                                               stage_scene.canvas.height));
  const rhi::Texture *program_texture = &impl.program_mixer.target();
  bool program_preview_ok = false;
  bool program_mapping_unavailable = false;
  auto acquired = device.acquire_commands();
  if (acquired) {
    auto commands = std::move(acquired).value();
    impl.frame_media_cache.begin_frame();
    struct EndMediaFrame final {
      render::MediaCache *cache{};
      ~EndMediaFrame() { cache->end_frame(); }
    } end_media_frame{&impl.frame_media_cache};
    const auto record_program =
        [&](render::CompositorSceneExecutor &executor,
            render::ProjectionMeshWarp &warp, const scene::Scene &frame,
            const compositor::CompositorGraph *graph)
        -> const rhi::Texture * {
      if (!program_targets_ready) {
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

    if (switcher.transition_active && switcher.transition_from != nullptr &&
        switcher.transition_to != nullptr) {
      const rhi::Texture *const from_target = record_program(
          impl.program_renderer, impl.program_warp, *switcher.transition_from,
          switcher.transition_from_graph);
      const rhi::Texture *const to_target = record_program(
          impl.program_to_renderer, impl.program_to_warp,
          *switcher.transition_to, switcher.transition_to_graph);
      program_preview_ok =
          from_target != nullptr && to_target != nullptr &&
          impl.program_mixer.render(commands, *from_target, *to_target,
                                    switcher.transition_mix);
      if (program_preview_ok) {
        program_texture = &impl.program_mixer.target();
      }
    } else {
      const rhi::Texture *const rendered = record_program(
          impl.program_renderer, impl.program_warp, scene,
          switcher.program_graph);
      program_preview_ok = rendered != nullptr;
      if (program_preview_ok) {
        program_texture = rendered;
      }
    }
    if (!program_preview_ok) {
      static_cast<void>(commands.clear(impl.program_mixer.target(),
                                       rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}));
      program_texture = &impl.program_mixer.target();
    }
    static_cast<void>(record_editor_scene(impl.stage_renderer, commands,
                                          stage_scene, compositor_graph,
                                          &impl.frame_media_cache));
    auto submitted = device.submit(std::move(commands));
    if (submitted) {
      if (!std::move(submitted).value().wait()) {
        program_preview_ok = false;
      }
    } else {
      program_preview_ok = false;
    }
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
          projection.project =
              [mesh, mesh_view, output_canvas](scene::Vec3 point) {
                return output::project_through_mesh_warp(
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
      impl.project_settings_panel.is_open();
  if (preferences_blocked_pointer) {
    ui.push_pointer_block();
  }

  const auto delete_selection = [&]() {
    if (!impl.has_selection()) {
      return;
    }
    const std::span<const doc::NodeId> selected =
        impl.scene_selection.members();
    result.deletes.assign(selected.begin(), selected.end());
    result.delete_gesture = impl.next_edit_gesture++;
    impl.scene_selection.clear();
    impl.semantic_boundary = true;
  };

  const HeaderActions header =
      draw_application_header(ui, width, switcher, impl.open_menu);
  const ApplicationMenuResult menu_result =
      run_application_menu_input(ui, width, height, impl.open_menu, impl.docks);
  switch (menu_result.action) {
  case ApplicationMenuAction::OpenProject:
    if (auto path = choose_project_to_open(); path.has_value()) {
      if (!project.dirty || confirm_discard_project_changes(
                                "those changes and open another project")) {
        result.load_project = ProjectFileRequest{std::move(*path)};
      }
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
    impl.project_settings_panel.open(project_settings.settings);
    break;
  case ApplicationMenuAction::TogglePanel:
    if (impl.docks.is_open(menu_result.panel)) {
      impl.docks.close(menu_result.panel);
    } else if (impl.docks.show(menu_result.panel) == gui::no_dock) {
      impl.show_panel(menu_result.panel);
    }
    break;
  case ApplicationMenuAction::None:
    break;
  }
  if (header.open_setup) {
    impl.show_setup();
  }
  if (!impl.manipulating) {
    if (header.undo) {
      result.history = HistoryRequest{HistoryAction::Undo,
                                      impl.history_context,
                                      impl.next_edit_gesture++};
    } else if (header.redo) {
      result.history = HistoryRequest{HistoryAction::Redo,
                                      impl.history_context,
                                      impl.next_edit_gesture++};
    }
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

  const auto capture_group = [&](const scene::Scene &view_scene) {
    impl.group_nodes.clear();
    impl.group_start.clear();
    for (const doc::NodeId node : impl.scene_selection.members()) {
      const auto world = scene::world_for(view_scene, node);
      if (world.has_value()) {
        impl.group_nodes.push_back(node);
        impl.group_start.push_back(scene::translation_of(world.value()));
      }
    }
  };

  const auto emit_group = [&](scene::Vec3 delta) {
    const core::u64 gesture = impl.begin_gesture();
    for (std::size_t index = 0U; index < impl.group_nodes.size(); ++index) {
      const doc::NodeId node = impl.group_nodes[index];
      const scene::Vec3 moved = impl.group_start[index] + delta;
      const bool position_keyed =
          keyed(keyframes, node, scene::TransformChannel::PositionX) ||
          keyed(keyframes, node, scene::TransformChannel::PositionY) ||
          keyed(keyframes, node, scene::TransformChannel::PositionZ);
      if (!position_keyed) {
        result.edits.push_back(EditIntent{node, moved, gesture});
        continue;
      }
      const std::array<std::pair<scene::TransformChannel, double>, 3U> axes{{
          {scene::TransformChannel::PositionX, moved.x},
          {scene::TransformChannel::PositionY, moved.y},
          {scene::TransformChannel::PositionZ, moved.z},
      }};
      for (const auto &[component, value] : axes) {
        if (keyed(keyframes, node, component)) {
          result.keys.push_back(KeyIntent{node, component, timeline.show_frame,
                                          value, false, gesture});
        } else {
          result.property_edits.push_back(
              PropertyEdit{node, component, value, gesture});
        }
      }
    }
  };

  // Projection surfaces are physical stage geometry belonging to the show, not
  // to the scene on the Stage, and the feed map they live in is drawn at the
  // Program playhead. Key them there too, or a surface dragged while Preview
  // sits at a different frame would author a key the operator cannot see.
  const auto route_surface_edit = [&](FeedSurfaceEditRequest request) {
    RoutedSurfaceEdit routed = route_surface_transform_edit(
        std::move(request), keyframes, timeline.program_frame);
    result.keys.insert(result.keys.end(),
                       std::make_move_iterator(routed.keys.begin()),
                       std::make_move_iterator(routed.keys.end()));
    if (routed.base.has_value()) {
      result.feed_surface_edits.push_back(std::move(routed.base.value()));
    }
  };

  const auto handle_viewport = [&](const ViewportFrame &viewport,
                                   const scene::Scene &view_scene,
                                   bool stage_view) {
    if (viewport.pressed) {
      if (viewport.surface_picked.has_value()) {
        impl.stage_surface_selection = viewport.surface_picked;
        impl.feed_mapping_panel.select_surface(*viewport.surface_picked);
        impl.scene_selection.clear();
        impl.group_nodes.clear();
        impl.group_start.clear();
        impl.orbiting = false;
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
          capture_group(view_scene);
        } else {
          impl.group_nodes.clear();
          impl.group_start.clear();
        }
      } else if (viewport.empty_canvas == EmptyCanvasInteraction::Orbit) {
        // Stage bare canvas is camera space. Preserve every selection domain;
        // the drag may become an orbit and must not masquerade as deselection.
        impl.orbiting = true;
      } else if (viewport.empty_canvas ==
                 EmptyCanvasInteraction::ClearSelection) {
        impl.stage_surface_selection.reset();
        impl.clear_scene_selection(true);
        impl.group_nodes.clear();
        impl.group_start.clear();
      }
    }
    if (viewport.dragging && !impl.group_nodes.empty()) {
      emit_group(viewport.translation);
    }
    if (stage_view && impl.orbiting && viewport.held && !viewport.pressed) {
      impl.camera_yaw -= ui.pointer_dx() * 0.0015F;
      impl.camera_pitch = std::clamp(
          impl.camera_pitch + ui.pointer_dy() * 0.0015F, -0.24F, 0.24F);
    }
    if (stage_view && viewport.hovered && ui.wheel() != 0.0F) {
      impl.camera_distance = std::clamp(
          impl.camera_distance * (1.0F - ui.wheel() * 0.1F), 1.0F, 200.0F);
    }
    if (viewport.released) {
      impl.orbiting = false;
      impl.group_nodes.clear();
      impl.group_start.clear();
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
    case LibraryAddAction::Media: {
      const char *const filters[]{"*.mp4", "*.mov", "*.mkv", "*.webm",
                                  "*.png", "*.jpg", "*.tga"};
      const char *const path = tinyfd_openFileDialog(
          "Add Media", "", 7, filters, "Video and images", 0);
      if (path != nullptr) {
        const std::string source{path};
        result.adds.push_back(AddIntent{AddKind::Media,
                                        source,
                                        render::probe_frame_count(source),
                                        impl.next_edit_gesture++,
                                        render::probe_media_aspect(source),
                                        {},
                                        authored_stage.id,
                                        {},
                                        probe_asset_frame_rate(source)});
      }
      return;
    }
    case LibraryAddAction::Model: {
      const char *const filters[]{"*.gltf", "*.glb", "*.obj"};
      const char *const path = tinyfd_openFileDialog(
          "Add 3D Model", "", 3, filters, "3D models (glTF, OBJ)", 0);
      if (path != nullptr) {
        result.adds.push_back(AddIntent{AddKind::Model,
                                        std::string{path},
                                        1U,
                                        impl.next_edit_gesture++,
                                        16.0F / 9.0F,
                                        {},
                                        authored_stage.id});
      }
      return;
    }
    }
  };

  const auto import_asset = [&](LibraryImportAction action) {
    const bool media = action == LibraryImportAction::Media;
    const char *const media_filters[]{"*.mp4", "*.mov", "*.mkv", "*.webm",
                                      "*.png", "*.jpg", "*.tga"};
    const char *const model_filters[]{"*.gltf", "*.glb", "*.obj"};
    const char *const path = tinyfd_openFileDialog(
        media ? "Import Media Asset" : "Import 3D Model Asset", "",
        media ? 7 : 3, media ? media_filters : model_filters,
        media ? "Video and images" : "3D models (glTF, OBJ)", 0);
    if (path == nullptr) {
      return;
    }
    const std::string source{path};
    const std::string display_name =
        std::filesystem::path{source}.filename().string();
    if (media) {
      assets::MediaAsset asset{};
      asset.display_name = display_name;
      asset.source = source;
      asset.frame_count = render::probe_frame_count(source);
      asset.frame_rate = probe_asset_frame_rate(source);
      result.asset_imports.push_back(AssetImportIntent{
          assets::Asset{std::move(asset)}, impl.next_edit_gesture++});
      return;
    }

    assets::ModelAsset asset{};
    asset.display_name = display_name;
    asset.source = source;
    render::ModelAsset probed{};
    if (render::load_model(source, probed)) {
      asset.triangle_count =
          static_cast<core::u64>(probed.mesh.indices.size() / 3U);
      asset.material_count = static_cast<core::u32>(probed.parts.size());
      asset.bounds =
          assets::ModelBounds{probed.bounds.minimum.x, probed.bounds.minimum.y,
                              probed.bounds.minimum.z, probed.bounds.maximum.x,
                              probed.bounds.maximum.y, probed.bounds.maximum.z};
    }
    result.asset_imports.push_back(AssetImportIntent{
        assets::Asset{std::move(asset)}, impl.next_edit_gesture++});
  };

  const auto instantiate_asset = [&](doc::NodeId id,
                                     std::optional<scene::Vec3> position) {
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
                                      media->frame_rate});
      return;
    }
    const auto &model = std::get<assets::ModelAsset>(*found);
    result.adds.push_back(AddIntent{AddKind::Model, model.source, 1U,
                                    impl.next_edit_gesture++, 16.0F / 9.0F,
                                    model.id, authored_stage.id, position});
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
  std::optional<EffectApplyRequest> pending_effect_drop;
  gui::Rect compositor_panel_bounds{};
  bool compositor_panel_drawn = false;
  gui::Rect inspector_panel_bounds{};
  bool inspector_panel_drawn = false;
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
      impl.stage_surface_width =
          static_cast<core::u32>(std::max(bounds.width, 1.0F));
      impl.stage_surface_height =
          static_cast<core::u32>(std::max(bounds.height - 29.0F, 1.0F));

      // Floating dock frames are painted after Stage. When one is under
      // the pointer, keep Stage's picker and camera gestures from taking
      // the same press before the floating panel gets to handle it.
      std::optional<gui::Rect> floating_occluder;
      for (const gui::DockLayout::Floating &floating : impl.docks.floating()) {
        if (floating.frame.contains(ui.pointer_x(), ui.pointer_y())) {
          floating_occluder = floating.frame;
          break;
        }
      }

      if (mask_stage_active) {
        const MaskViewportFrame mask_frame = impl.stage_panel.draw_masks(
            ui, bounds, impl.stage_renderer.target(), stage_scene,
            mask_viewport_input(stage_scene, nullptr), &scene.camera,
            static_cast<float>(timeline.rate_numerator) /
                static_cast<float>(
                    std::max(timeline.rate_denominator, 1U)));
        stage_viewport_frame.image = mask_frame.image;
        apply_mask_viewport_frame(mask_frame);
      } else {
        stage_viewport_frame = impl.stage_panel.draw(
            ui, bounds, impl.stage_renderer.target(), stage_scene,
            impl.scene_selection.members(), impl.anchor(), &scene.camera,
            floating_occluder.has_value() ? &floating_occluder.value()
                                          : nullptr,
            static_cast<float>(timeline.rate_numerator) /
                static_cast<float>(
                    std::max(timeline.rate_denominator, 1U)),
            &feed_map,
            impl.stage_surface_selection.value_or(doc::NodeId{}));
      }
      stage_viewport_drawn = true;
      if (!switcher.rehearsal_locked) {
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
          handle_viewport(stage_viewport_frame, stage_scene, true);
        }
      } else {
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
      return;
    }
    if (panel == panel_program) {
      ui.panel(bounds, ui.theme().background);

      const gui::Rect output_bar{bounds.x, bounds.y, bounds.width, 22.0F};
      ui.panel(output_bar, ui.theme().header);
      // The overlay is visible chrome, but the mode it belongs to is worth
      // naming: this is the live output, and a drag on it authors a matte.
      ui.label(gui::Rect{output_bar.x + 7.0F, output_bar.y,
                         output_bar.width * 0.43F, output_bar.height},
               mask_program_active ? "PROGRAM OUTPUT  /  MASKS"
                                   : "PROGRAM OUTPUT",
               mask_program_active ? ui.theme().accent
                                   : ui.theme().text_muted);
      const gui::Rect output_name{output_bar.x + output_bar.width * 0.45F,
                                  output_bar.y + 3.0F, output_bar.width * 0.52F,
                                  output_bar.height - 6.0F};
      std::array<char, 64U> output_identity{};
      if (program_output != nullptr) {
        static_cast<void>(std::snprintf(
            output_identity.data(), output_identity.size(),
            "OUTPUT %02u | NODE %u  v", program_output->node_index + 1U,
            program_output->node_index));
      } else {
        static_cast<void>(std::snprintf(output_identity.data(),
                                        output_identity.size(),
                                        "NO OUTPUT MAP"));
      }
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

      const gui::Rect footer{
          bounds.x, bounds.y + std::max(bounds.height - 18.0F, 22.0F),
          bounds.width, std::min(18.0F, std::max(bounds.height - 22.0F, 0.0F))};
      const gui::Rect content{
          bounds.x, bounds.y + output_bar.height, bounds.width,
          std::max(footer.y - bounds.y - output_bar.height, 0.0F)};
      ui.panel(content, gui::UiColor{0.0F, 0.0F, 0.0F, 1.0F});
      const ViewportProjection *const program_projection =
          program_view_projection.has_value()
              ? &program_view_projection.value()
              : nullptr;
      if (program_preview_ok && mask_program_active) {
        // The overlay is measured against the connector image, so a matte the
        // operator is looking at on the wall is the matte their pointer moves.
        apply_mask_viewport_frame(impl.program_panel.draw_masks(
            ui, content, *program_texture, scene,
            mask_viewport_input(scene, program_projection), nullptr,
            static_cast<float>(timeline.rate_numerator) /
                static_cast<float>(std::max(timeline.rate_denominator, 1U)),
            program_projection));
      } else if (program_preview_ok) {
        const ViewportFrame program_viewport = impl.program_panel.draw(
            ui, content, *program_texture, scene,
            impl.scene_selection.members(), impl.anchor(), nullptr, nullptr,
            static_cast<float>(timeline.rate_numerator) /
                static_cast<float>(std::max(timeline.rate_denominator, 1U)),
            nullptr, doc::NodeId{}, program_projection);
        if (!switcher.rehearsal_locked) {
          handle_viewport(program_viewport, scene, false);
        }
      } else {
        const std::string_view fault =
            program_mapping_unavailable ? "OUTPUT MAPPING UNAVAILABLE"
                                        : "PROGRAM PREVIEW FAILED";
        const float label_width = ui.font_width(fault);
        ui.label(gui::Rect{content.x +
                               std::max((content.width - label_width) * 0.5F,
                                        8.0F),
                           content.y + std::max(content.height * 0.5F - 10.0F,
                                                0.0F),
                           label_width, 20.0F},
                 fault, ui.theme().warning);
      }

      ui.panel(footer, ui.theme().header);
      std::array<char, 128U> preview_status{};
      const int preview_length = std::snprintf(
          preview_status.data(), preview_status.size(),
          "%u x %u  |  %.*s", program_extent.output_width,
          program_extent.output_height,
          static_cast<int>(program_projection_name(program_output).size()),
          program_projection_name(program_output).data());
      ui.label(gui::Rect{footer.x + 7.0F, footer.y,
                         std::max(footer.width - 60.0F, 0.0F), footer.height},
               std::string_view{
                   preview_status.data(),
                   static_cast<std::size_t>(std::max(preview_length, 0))},
               ui.theme().text_muted);
      ui.panel(gui::Rect{footer.x + footer.width - 44.0F, footer.y + 6.0F, 5.0F,
                         5.0F},
               program_preview_ok ? ui.theme().good : ui.theme().warning);
      ui.label(gui::Rect{footer.x + footer.width - 35.0F, footer.y, 34.0F,
                         footer.height},
               !program_preview_ok ? "FAULT"
                                   : (timeline.playing ? "PLAY" : "PAUSE"),
               !program_preview_ok
                   ? ui.theme().warning
                   : (timeline.playing ? ui.theme().good
                                       : ui.theme().text_muted));
      return;
    }
    if (panel == panel_scene) {
      gui::Rect tree_bounds = bounds;
      if (!switcher.scenes.empty()) {
        constexpr float scene_header_height = 20.0F;
        constexpr float scene_row_height = 21.0F;
        const std::size_t visible =
            std::min<std::size_t>(switcher.scenes.size(), 4U);
        const std::size_t max_offset = switcher.scenes.size() > visible
                                           ? switcher.scenes.size() - visible
                                           : 0U;
        impl.scene_list_offset = std::min(impl.scene_list_offset, max_offset);
        const float scene_block_height =
            scene_header_height +
            static_cast<float>(visible) * scene_row_height + 25.0F;
        ui.panel(
            gui::Rect{bounds.x, bounds.y, bounds.width, scene_block_height},
            ui.theme().panel);
        ui.label(gui::Rect{bounds.x + 8.0F, bounds.y,
                           std::max(bounds.width - 16.0F, 0.0F),
                           scene_header_height},
                 "PROJECT SCENES", ui.theme().text_muted);
        float row_y = bounds.y + scene_header_height;
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
          const bool is_program = info.id == switcher.program_scene;
          const bool is_rehearsal = info.id == switcher.rehearsal_scene;
          const bool compatible = is_program ||
                                  program_info == switcher.scenes.end() ||
                                  (scene::valid(program_info->canvas) &&
                                   program_info->canvas == info.canvas);
          // Any scene can be taken into Preview and worked on. A canvas that
          // differs from Program only rules out the dissolve, and the row says
          // so; refusing to let the operator open the scene at all would make
          // a scene resize a dead end.
          const bool selectable = !switcher.rehearsal_locked &&
                                  ((is_program && switcher.rehearsal_mode) ||
                                   !is_program);
          const gui::Interaction interaction =
              selectable
                  ? ui.grab(gui::ui_id("editor.scene-slot") ^ info.id.value(),
                            row)
                  : gui::Interaction{};
          ui.panel(row, is_rehearsal && switcher.rehearsal_mode
                            ? ui.theme().selection
                            : (interaction.hovered ? ui.theme().widget_hover
                                                   : ui.theme().row));
          ui.panel(gui::Rect{row.x, row.y, 2.0F, row.height},
                   is_program ? ui.theme().good
                              : (is_rehearsal ? ui.theme().accent
                                              : ui.theme().header));
          ui.label(gui::Rect{row.x + 8.0F, row.y,
                             std::max(row.width - 47.0F, 0.0F), row.height},
                   info.name.empty() ? "Untitled Scene" : info.name,
                   ui.theme().text);
          if (is_program || is_rehearsal || !compatible) {
            ui.label(gui::Rect{row.x + std::max(row.width - 39.0F, 0.0F), row.y,
                               36.0F, row.height},
                     is_program ? "PGM" : (is_rehearsal ? "RHS" : "SIZE"),
                     !compatible
                         ? ui.theme().warning
                         : (is_program ? ui.theme().good : ui.theme().accent));
          }
          if (interaction.clicked) {
            if (is_program && switcher.rehearsal_mode) {
              result.switcher = SwitcherRequest{SwitcherAction::ToggleRehearse};
            } else if (!is_program) {
              result.switcher =
                  SwitcherRequest{SwitcherAction::SelectRehearsal, info.id, 0U};
            }
          }
          row_y += scene_row_height;
        }
        const gui::Rect previous_scene{bounds.x + 4.0F, row_y + 2.0F, 24.0F,
                                       20.0F};
        if (chrome_button(ui, gui::ui_id("editor.scene.previous"),
                          previous_scene, "<", false,
                          impl.scene_list_offset > 0U)) {
          --impl.scene_list_offset;
        }
        const gui::Rect next_scene{bounds.x +
                                       std::max(bounds.width - 28.0F, 4.0F),
                                   row_y + 2.0F, 24.0F, 20.0F};
        if (chrome_button(ui, gui::ui_id("editor.scene.next"), next_scene, ">",
                          false, impl.scene_list_offset < max_offset)) {
          ++impl.scene_list_offset;
        }
        const float scene_action_x =
            previous_scene.x + previous_scene.width + 3.0F;
        const float scene_action_width =
            std::max(next_scene.x - scene_action_x - 3.0F, 1.0F);
        constexpr float scene_action_gap = 3.0F;
        const float scene_button_width =
            std::max((scene_action_width - scene_action_gap) * 0.5F, 1.0F);
        const gui::Rect add_scene{scene_action_x, row_y + 2.0F,
                                  scene_button_width, 20.0F};
        const gui::Rect copy_scene{
            add_scene.x + add_scene.width + scene_action_gap, add_scene.y,
            std::max(scene_action_width - scene_button_width - scene_action_gap,
                     1.0F),
            add_scene.height};
        if (chrome_button(ui, gui::ui_id("editor.scene.create"), add_scene,
                          "+ NEW", false, !switcher.rehearsal_locked)) {
          result.switcher = SwitcherRequest{SwitcherAction::CreateScene};
          impl.scene_list_offset =
              std::min(impl.scene_list_offset + 1U, max_offset + 1U);
        }
        if (chrome_button(ui, gui::ui_id("editor.scene.duplicate"), copy_scene,
                          "CREATE COPY", false, !switcher.rehearsal_locked)) {
          result.switcher = SwitcherRequest{SwitcherAction::DuplicateScene,
                                            authored_stage.id, 0U};
          impl.scene_list_offset =
              std::min(impl.scene_list_offset + 1U, max_offset + 1U);
        }
        tree_bounds.y += scene_block_height;
        tree_bounds.height =
            std::max(tree_bounds.height - scene_block_height, 0.0F);
      }
      const auto request = impl.scene_panel.draw(
          ui, tree_bounds, elements, impl.scene_selection.members(),
          impl.anchor());
      if (request.has_value()) {
        if (request->structure.has_value()) {
          GroupEditIntent edit{};
          edit.scene = authored_stage.id;
          edit.gesture = impl.next_edit_gesture++;
          if (*request->structure ==
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
      const LibraryFrame library =
          impl.library_panel.draw(ui, bounds, asset_registry, thumbnails, {},
                                  proxy_runtime);
      if (library.rescan_assets) {
        result.rescan_assets = true;
      }
      if (library.add.has_value()) {
        handle_library_add(library.add.value());
      }
      if (library.import_asset.has_value()) {
        import_asset(library.import_asset.value());
      }
      if (library.instantiate_asset.has_value()) {
        instantiate_asset(library.instantiate_asset.value(), std::nullopt);
      }
      if (library.remove_asset.has_value()) {
        result.asset_deletes.push_back(AssetDeleteIntent{
            library.remove_asset.value(), impl.next_edit_gesture++});
      }
      if (library.proxy_policy.has_value()) {
        result.asset_proxy_edits.push_back(AssetProxyEditIntent{
            library.proxy_policy->asset,
            library.proxy_policy->requested_profile,
            library.proxy_policy->policy,
            impl.next_edit_gesture++});
      }
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
      std::string_view target_name;
      if (drop_target != nullptr) {
        if (!drop_target->display_name.empty()) {
          target_name = drop_target->display_name;
        } else {
          const int length = std::snprintf(
              target_title.data(), target_title.size(), "%.*s %llu",
              static_cast<int>(kind_name(drop_target->kind).size()),
              kind_name(drop_target->kind).data(),
              static_cast<unsigned long long>(drop_target->node.value()));
          target_name = std::string_view{
              target_title.data(),
              static_cast<std::size_t>(std::clamp(
                  length, 0, static_cast<int>(target_title.size() - 1U)))};
        }
      }
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
          impl.compositor_panel.draw(ui, bounds, compositor_graph);
      if (graph_frame.selection.has_value()) {
        impl.delete_domain = DeleteDomain::Compositor;
        // Selection presses are reported even when the same node is clicked
        // again (and when empty canvas clears selection). This gives the
        // operator a non-authoring way to choose which per-context redo stack
        // the global history actions navigate.
        impl.history_context = HistoryContext::Compositor;
      }
      delete_consumed = delete_consumed || graph_frame.consume_delete;
      if (!compositor_locked) {
        result.compositor_add = graph_frame.add_node;
        if (result.compositor_add.has_value() &&
            result.compositor_add->kind == compositor::NodeKind::SceneLayer) {
          result.compositor_add->scene_node = impl.anchor();
          if (result.compositor_add->scene_node == doc::NodeId{}) {
            result.compositor_add.reset();
            impl.compositor_warning_until = ui.time_seconds() + 1.5;
          }
        }
        result.compositor_move = graph_frame.move_node;
        result.compositor_connect = graph_frame.connect_ports;
        result.compositor_property_edit = graph_frame.property_edit;
        result.compositor_delete = graph_frame.remove;
        if (graph_frame.add_node.has_value() ||
            graph_frame.move_node.has_value() ||
            graph_frame.connect_ports.has_value() ||
            graph_frame.remove.has_value()) {
          result.compositor_gesture = impl.next_edit_gesture++;
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
          impl.hardware_mapping_panel.draw(ui, bounds, hardware_map,
                                           hardware_runtime, sync_policy);
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
        if (impl.hardware_edit_gesture == 0U) {
          impl.hardware_edit_gesture = impl.next_edit_gesture++;
        }
        mapping.control_port_edit->gesture = impl.hardware_edit_gesture;
        result.hardware_control_port_edit = mapping.control_port_edit;
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
        mapping.edit->gesture = impl.next_edit_gesture++;
        result.feed_map_edits.push_back(mapping.edit.value());
      }
      if (mapping.remove.has_value()) {
        mapping.remove->gesture = impl.next_edit_gesture++;
        result.feed_map_deletes.push_back(mapping.remove.value());
      }
      if (mapping.projection_edit.has_value()) {
        mapping.projection_edit->gesture = impl.next_edit_gesture++;
        result.feed_projection_edits.push_back(mapping.projection_edit.value());
      }
      if (mapping.surface_add.has_value()) {
        FeedSurfaceAddRequest request = std::move(mapping.surface_add.value());
        bool ready = true;
        std::optional<ProjectionMeshChoice> chosen_mesh;
        if (request.shape == output::ProjectionSurfaceShape::ImportedMesh &&
            request.mesh_source.empty()) {
          chosen_mesh =
              choose_projection_mesh(request.surface, request.mesh_scale);
          ready = chosen_mesh.has_value();
          if (chosen_mesh.has_value()) {
            request.mesh_source = chosen_mesh->source;
            request.name = projection_mesh_name(request.mesh_source);
          }
        }
        if (ready) {
          request.gesture = impl.next_edit_gesture++;
          if (request.shape == output::ProjectionSurfaceShape::ImportedMesh &&
              !request.mesh_source.empty()) {
            assets::ModelAsset asset{};
            asset.display_name =
                std::filesystem::path{request.mesh_source}.filename().string();
            asset.source = request.mesh_source;
            asset.projection_mesh = true;
            if (chosen_mesh.has_value()) {
              asset.vertex_count = chosen_mesh->vertex_count;
              asset.triangle_count = chosen_mesh->triangle_count;
            }
            result.asset_imports.push_back(AssetImportIntent{
                assets::Asset{std::move(asset)}, request.gesture});
          }
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
        FeedSurfaceEditRequest request = std::move(mapping.mesh_browse.value());
        const auto chosen =
            choose_projection_mesh(request.surface, request.mesh_scale);
        if (chosen.has_value()) {
          request.mesh_source = chosen->source;
          request.name = projection_mesh_name(request.mesh_source);
          request.shape = output::ProjectionSurfaceShape::ImportedMesh;
          request.gesture = impl.next_edit_gesture++;
          assets::ModelAsset asset{};
          asset.display_name =
              std::filesystem::path{request.mesh_source}.filename().string();
          asset.source = request.mesh_source;
          asset.triangle_count = chosen->triangle_count;
          asset.vertex_count = chosen->vertex_count;
          asset.projection_mesh = true;
          result.asset_imports.push_back(AssetImportIntent{
              assets::Asset{std::move(asset)}, request.gesture});
          result.feed_surface_edits.push_back(std::move(request));
        }
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
      if (output.lifecycle.has_value()) {
        result.output_lifecycle = std::move(output.lifecycle.value());
      }
      return;
    }
    if (panel == panel_cues) {
      constexpr std::array<std::string_view, 2U> rows{
          "No cues authored", "+  ADD CUE (not available yet)"};
      draw_placeholder(bounds, "CUE STACK", rows);
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
      const TimelinePanelFrame timeline_frame =
          impl.timeline_panel.draw(ui, bounds, timeline, keyframes, elements,
                                   impl.scene_selection.members(), impl.anchor(),
                                   effects,
                                   feed_map.surfaces,
                                   impl.stage_surface_selection.value_or(
                                       doc::NodeId{}),
                                   timeline_lanes, clip_clipboard_span,
                                   key_scale_preview, filmstrips);
      result.filmstrip_requests = timeline_frame.filmstrip_requests;
      result.filmstrip_retain = timeline_frame.filmstrip_retain;
      result.seek_to_frame = timeline_frame.seek_to_frame;
      result.transport = timeline_frame.transport;
      result.range = timeline_frame.range;
      result.show_rate = timeline_frame.show_rate;
      editor_control_editing = editor_control_editing || timeline_frame.editing;
      delete_consumed = delete_consumed || timeline_frame.consume_delete;
      clipboard_consumed =
          clipboard_consumed || timeline_frame.consume_clipboard;
      if (timeline_frame.clipboard.has_value()) {
        request_clip_clipboard(*timeline_frame.clipboard);
      }
      if (timeline_frame.key_clipboard.has_value() &&
          !impl.preferences_panel.is_open() &&
          !impl.project_settings_panel.is_open()) {
        request_key_clipboard(*timeline_frame.key_clipboard);
      }
      if (timeline_frame.selection.has_value()) {
        const TimelineSelectionRequest request =
            timeline_frame.selection.value();
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
              find_element(elements, request.node);
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
        const core::u64 gesture = impl.next_edit_gesture++;
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
      if (!timeline_frame.element_flags.empty()) {
        if (impl.lane_edit_gesture == 0U) {
          impl.lane_edit_gesture = impl.next_edit_gesture++;
        }
        for (const TimelineElementFlagEdit &edit :
             timeline_frame.element_flags) {
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
      if (impl.effects_panel.dragging() && anchor != doc::NodeId{}) {
        outline(ui, bounds, ui.theme().accent);
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
      if (impl.effects_panel.dragging() && anchor != doc::NodeId{}) {
        outline(ui, bounds, ui.theme().accent);
      }
      return;
    }
    if (panel == panel_inspector || panel == panel_inspector_overview) {
      inspector_panel_bounds = bounds;
      inspector_panel_drawn = true;
      const doc::NodeId anchor = impl.anchor();
      std::vector<ElementInfo> inspector_elements;
      inspector_elements.reserve(impl.scene_selection.size());
      for (const doc::NodeId selected : impl.scene_selection.members()) {
        if (const ElementInfo *const element = find_element(elements, selected);
            element != nullptr) {
          inspector_elements.push_back(*element);
        }
      }
      const InspectorFrame inspector = impl.inspector_panel.draw(
          ui, bounds, std::span<const ElementInfo>{inspector_elements}, anchor,
          keyframes, timeline.show_frame, effects);
      core::u64 inspector_gesture{};
      if (!inspector.component_edits.empty() ||
          !inspector.named_edits.empty()) {
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
        if (keyed(keyframes, node, component)) {
          result.keys.push_back(KeyIntent{node, component,
                                          timeline.show_frame, value,
                                          false, inspector_gesture});
        } else {
          result.property_edits.push_back(
              PropertyEdit{node, component, value, inspector_gesture});
        }
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
      if (inspector.delete_requested) {
        delete_selection();
      }
      if (impl.effects_panel.dragging() && anchor != doc::NodeId{}) {
        outline(ui, bounds, ui.theme().accent);
      }
    }
  };

  const float dock_height =
      std::max(height - top_bar_height - status_bar_height, 0.0F);
  impl.dock_host.update(ui, gui::Rect{0.0F, top_bar_height, width, dock_height},
                        draw_panel,
                        gui::DockMetrics{2.0F, 24.0F, 20.0F, 0.25F});
  impl.dock_host.overlay(ui);

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
      const CompositorSelection selected = impl.compositor_panel.selection();
      if (selected.kind != CompositorSelectionKind::None) {
        result.compositor_delete = CompositorDeleteIntent{selected};
        result.compositor_gesture = impl.next_edit_gesture++;
      }
      return;
    }
    case DeleteDomain::Neutral:
      return;
    }
  };
  if (pending_effect_drop.has_value()) {
    std::optional<compositor::NodeKind> graph_effect_kind;
    switch (pending_effect_drop->effect) {
    case scene::EffectKind::Brightness:
      graph_effect_kind = compositor::NodeKind::Brightness;
      break;
    case scene::EffectKind::Saturation:
      graph_effect_kind = compositor::NodeKind::Saturation;
      break;
    case scene::EffectKind::Blur:
      graph_effect_kind = compositor::NodeKind::Blur;
      break;
    case scene::EffectKind::BoxBlur:
      graph_effect_kind = compositor::NodeKind::BoxBlur;
      break;
    case scene::EffectKind::DirectionalBlur:
      graph_effect_kind = compositor::NodeKind::DirectionalBlur;
      break;
    case scene::EffectKind::ChromaKey:
      graph_effect_kind = compositor::NodeKind::ChromaKey;
      break;
    case scene::EffectKind::LumaKey:
      graph_effect_kind = compositor::NodeKind::LumaKey;
      break;
    case scene::EffectKind::Levels:
      graph_effect_kind = compositor::NodeKind::Levels;
      break;
    case scene::EffectKind::LiftGammaGain:
      graph_effect_kind = compositor::NodeKind::LiftGammaGain;
      break;
    case scene::EffectKind::ContrastPivot:
      graph_effect_kind = compositor::NodeKind::ContrastPivot;
      break;
    case scene::EffectKind::ColorMatrix:
      graph_effect_kind = compositor::NodeKind::ColorMatrix;
      break;
    case scene::EffectKind::WhiteBalance:
      graph_effect_kind = compositor::NodeKind::WhiteBalance;
      break;
    case scene::EffectKind::Curves:
      graph_effect_kind = compositor::NodeKind::Curves;
      break;
    case scene::EffectKind::FilmGrain:
      graph_effect_kind = compositor::NodeKind::FilmGrain;
      break;
    case scene::EffectKind::Count:
      break;
    }
    const bool graph_drop =
        pending_effect_drop->dragged && compositor_panel_drawn &&
        compositor_panel_bounds.contains(pending_effect_drop->screen_x,
                                         pending_effect_drop->screen_y);
    const gui::Rect *const stage_drop_bounds =
        stage_viewport_drawn ? &stage_viewport_frame.image : nullptr;
    const gui::Rect *const inspector_drop_bounds =
        inspector_panel_drawn ? &inspector_panel_bounds : nullptr;
    if (graph_drop && graph_effect_kind.has_value() && !compositor_locked &&
        !result.compositor_add.has_value()) {
      constexpr float toolbar_height = 26.0F;
      constexpr float node_width = 164.0F;
      constexpr float node_height = 72.0F;
      const float content_width = compositor_panel_bounds.width;
      const float content_height =
          std::max(compositor_panel_bounds.height - toolbar_height, 0.0F);
      result.compositor_add = CompositorAddIntent{
          *graph_effect_kind,
          std::clamp(pending_effect_drop->screen_x - compositor_panel_bounds.x -
                         node_width * 0.5F,
                     0.0F, std::max(content_width - node_width, 0.0F)),
          std::clamp(pending_effect_drop->screen_y - compositor_panel_bounds.y -
                         toolbar_height - node_height * 0.5F,
                     0.0F, std::max(content_height - node_height, 0.0F))};
      result.compositor_gesture = impl.next_edit_gesture++;
    } else if (!graph_drop && effect_drop_targets_selected_layer(
                                  *pending_effect_drop, stage_drop_bounds,
                                  inspector_drop_bounds)) {
      result.effects.push_back(EffectIntent{pending_effect_drop->target,
                                            pending_effect_drop->effect,
                                            impl.next_edit_gesture++});
    }
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
      instantiate_asset(pending_asset_drop->asset,
                        scene::Vec3{stage_scene.camera.target.x + offset.x,
                                    stage_scene.camera.target.y + offset.y,
                                    stage_scene.camera.target.z + offset.z});
    }
  }

  // Global shortcuts stand down while a field owns the keyboard or a pointer
  // gesture is live. The window host has already translated the platform
  // chords into layout-independent history keys.
  const bool global_edit_shortcut_allowed =
      !impl.preferences_panel.is_open() &&
      !impl.project_settings_panel.is_open() && !editor_control_editing &&
      !ui.typing() && !ui.pointer_down() && !impl.manipulating;
  const bool scene_clipboard_domain =
      impl.history_context == HistoryContext::Scene &&
      (impl.delete_domain == DeleteDomain::Scene ||
       impl.delete_domain == DeleteDomain::Neutral);
  if (!clipboard_consumed && !result.clip_clipboard.has_value() &&
      !result.key_clipboard.has_value() && global_edit_shortcut_allowed &&
      scene_clipboard_domain) {
    if (ui.key_pressed(gui::Key::Copy)) {
      request_clip_clipboard(ClipClipboardAction::Copy);
    } else if (ui.key_pressed(gui::Key::Paste)) {
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

  draw_status_bar(ui, width, height, timeline, switcher,
                  impl.scene_selection.size(), scene.canvas, project);
  draw_application_menu_overlay(ui, impl.open_menu, impl.docks);

  if (preferences_blocked_pointer) {
    ui.pop_pointer_block();
  }
  if (impl.preferences_panel.is_open()) {
    result.save_preferences =
        impl.preferences_panel
            .draw(ui, gui::Rect{0.0F, 0.0F, width, height}, preferences)
            .save;
    if (!impl.preferences_panel.is_open()) {
      // A focused field must not keep the keyboard once the card is gone.
      ui.clear_focus();
    }
  }
  if (impl.project_settings_panel.is_open()) {
    const ProjectSettingsFrame settings_frame =
        impl.project_settings_panel.draw(
            ui, gui::Rect{0.0F, 0.0F, width, height}, project_settings);
    if (settings_frame.apply.has_value()) {
      result.project_settings =
          ProjectSettingsEdit{*settings_frame.apply, project_settings.node,
                              impl.next_edit_gesture++};
    }
    if (!impl.project_settings_panel.is_open()) {
      ui.clear_focus();
    }
  }

  if (!ui.pointer_down() && !ui.typing()) {
    impl.manipulating = false;
  }

  const bool compositor_authored = result.compositor_add.has_value() ||
      result.compositor_move.has_value() ||
      result.compositor_connect.has_value() ||
      result.compositor_property_edit.has_value() ||
      result.compositor_delete.has_value();
  const bool selection_set_authored =
      result.selection_set_edit.has_value() &&
      result.selection_set_edit->action != SelectionSetEditAction::Recall;
  const bool scene_authored = !result.edits.empty() || !result.keys.empty() ||
      !result.key_moves.empty() || result.key_duplicate.has_value() ||
      result.key_batch.has_value() ||
      !result.clip_timings.empty() ||
      !result.clip_splits.empty() || !result.lane_edits.empty() ||
      !result.element_flags.empty() ||
      !result.timeline_edits.empty() ||
      result.group_edit.has_value() ||
      selection_set_authored ||
      result.mask_edit.has_value() ||
      !result.property_edits.empty() ||
      !result.generic_edits.empty() || !result.adds.empty() ||
      !result.asset_deletes.empty() || !result.asset_proxy_edits.empty() ||
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

  static_cast<void>(impl.window.present(
      impl.draw_list, rhi::Color{0.043F, 0.047F, 0.059F, 1.0F}));
  return result;
}

} // namespace mediaserver::editor
