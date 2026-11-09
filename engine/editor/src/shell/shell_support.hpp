#pragma once

// Helpers the editor shell's translation units share: panel identities,
// the editor theme, preview sizing, file dialogs and small scene queries.
// Internal to the editor library.

#include <mediaserver/editor/editor_shell.hpp>

#include <mediaserver/editor/asset_viewer_panel.hpp>
#include <mediaserver/editor/model_facts_worker.hpp>
#include <mediaserver/editor/live_data_panel.hpp>
#include <mediaserver/editor/library_thumbnail_texture_cache.hpp>
#include <mediaserver/editor/timeline_filmstrip_texture_cache.hpp>
#include <mediaserver/editor/color_correction_panel.hpp>
#include <mediaserver/editor/effects_panel.hpp>
#include <mediaserver/editor/inspector_panel.hpp>
#include <mediaserver/editor/hardware_sprites.hpp>
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
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/gui/ui_renderer.hpp>
#include <mediaserver/gui/window.hpp>
#include <mediaserver/render/compositor_scene_executor.hpp>
#include <mediaserver/render/inspection_view.hpp>
#include <mediaserver/render/mapped_scene.hpp>
#include <mediaserver/render/particle_package_cache.hpp>
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
#include <mediaserver/scene/timeline_edit.hpp>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <future>
#include <mediaserver/render/model_ingest_queue.hpp>
#include <mediaserver/render/model_cache.hpp>
#include <mediaserver/render/baked_lighting.hpp>
#include <iterator>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace mediaserver::editor::shell_detail {

namespace gui = mediaserver::gui;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;

// The show bar is tall enough for the Program and Preview cards, Take and GO
// to be hit without aiming; see editor_dock_area().
constexpr float top_bar_height = editor_top_bar_height;

constexpr float status_bar_height = editor_status_bar_height;

constexpr float menu_bar_left = editor_menu_bar_left;

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
    std::string_view text);

[[nodiscard]] std::string
native_path_to_utf8(const std::filesystem::path &path);

[[nodiscard]] std::vector<std::string>
selected_dialog_paths(std::string_view selection);

[[nodiscard]] std::optional<assets::MediaFrameRate>
asset_frame_rate(const std::optional<render::MediaFrameRate> &probed);

inline const gui::PanelId panel_stage = gui::ui_id("editor.panel.stage");

inline const gui::PanelId panel_program = gui::ui_id("editor.panel.program");

inline const gui::PanelId panel_inspector = gui::ui_id("editor.panel.inspector");

inline const gui::PanelId panel_inspector_overview =
    gui::ui_id("editor.panel.inspector-overview");

inline const gui::PanelId panel_timeline = gui::ui_id("editor.panel.timeline");

inline const gui::PanelId panel_nodes = gui::ui_id("editor.panel.nodes");

inline const gui::PanelId panel_cues = gui::ui_id("editor.panel.cues");

inline const gui::PanelId panel_automation = gui::ui_id("editor.panel.automation");

inline const gui::PanelId panel_scene = gui::ui_id("editor.panel.scene");

inline const gui::PanelId panel_selection_sets =
    gui::ui_id("editor.panel.selection-sets");

inline const gui::PanelId panel_masks = gui::ui_id("editor.panel.masks");

inline const gui::PanelId panel_library = gui::ui_id("editor.panel.library");

inline const gui::PanelId panel_effects = gui::ui_id("editor.panel.effects");

inline const gui::PanelId panel_color_correction =
    gui::ui_id("editor.panel.color-correction");

inline const gui::PanelId panel_texture = gui::ui_id("editor.panel.texture");

inline const gui::PanelId panel_macros = gui::ui_id("editor.panel.macros");

inline const gui::PanelId panel_compositor = gui::ui_id("editor.panel.compositor");

inline const gui::PanelId panel_output = gui::ui_id("editor.panel.output");

inline const gui::PanelId panel_hardware_mapping =
    gui::ui_id("editor.panel.hardware-mapping");

inline const gui::PanelId panel_feed_mapping = gui::ui_id("editor.panel.feed-mapping");

inline const gui::PanelId panel_live_data = gui::ui_id("editor.panel.live-data");

inline const gui::PanelId panel_asset_viewer =
    gui::ui_id("editor.panel.asset-viewer");

// The editor's palette is gui::Theme's own (see ui_context.hpp for what each
// colour means); the editor only packs its forms a little tighter.
[[nodiscard]] constexpr gui::Theme editor_theme() noexcept {
  gui::Theme theme{};
  theme.padding = 5.0F;
  return theme;
}

[[nodiscard]] gui::UiColor with_alpha(gui::UiColor color, float alpha) noexcept;

[[nodiscard]] gui::UiColor scale_color(gui::UiColor color, float amount) noexcept;

[[nodiscard]] std::optional<gui::UiPoint>
draw_eyedropper_overlay(gui::UiContext &ui, gui::UiId id,
                        const gui::Rect &image,
                        bool pointer_available = true);

// A key colour has to come from the plate, not from the already-keyed result.
// While the eyedropper is armed, bypass Chroma Key operations on its target in
// the editor-only scene copies. Program remains untouched on the real output;
// only the Stage/Program preview textures rendered by this shell use the copy.
void bypass_sampled_chroma_key(
    scene::Scene &view,
    const std::optional<InspectorColorSampleRequest> &request);

[[nodiscard]] std::string_view
effect_target_name(const ElementInfo *target,
                   std::array<char, 64U> &storage) noexcept;

// The Stage views' renderers: the show's own limits, plus the wireframe
// pipelines their shading menu can ask for.
// A model thumbnail is the derivative profile's size. A model that has not
// loaded within about a minute, or whose textures have not settled within
// five seconds of drawing, is answered with no picture.
constexpr core::u32 model_thumbnail_width = 320U;

constexpr core::u32 model_thumbnail_height = 180U;

constexpr std::size_t model_thumbnail_queue = 8U;

constexpr core::u32 model_thumbnail_wait_frames = 3'600U;

constexpr core::u32 model_thumbnail_draw_frames = 300U;

[[nodiscard]] render::RendererLimits stage_view_limits() noexcept;

[[nodiscard]] const rhi::Texture *
record_editor_scene(render::CompositorSceneExecutor &executor,
                     rhi::CommandList &commands, const scene::Scene &frame,
                     const compositor::CompositorGraph &graph,
                     render::MediaCache *shared_media_cache,
                     const render::SceneRenderView *view = nullptr) noexcept;

[[nodiscard]] const output::OutputMap *selected_program_output(
    const output::FeedMap &feed_map,
    std::optional<doc::NodeId> &selected_map_node) noexcept;

struct ProgramOutputExtent final {
  core::u32 output_width{};
  core::u32 output_height{};
  core::u32 preview_width{};
  core::u32 preview_height{};
};

[[nodiscard]] ProgramOutputExtent program_output_extent(
    const scene::Scene &frame, const output::OutputMap *map,
    const topology::HardwareMap &hardware_map) noexcept;

[[nodiscard]] std::string_view
program_projection_name(const output::OutputMap *map) noexcept;

// The native pickers below block until the operator answers, so they are only
// ever called as the `show` of EditorShell::Frame::open_file_dialog, on the
// dialog's own thread and never on the frame thread.
[[nodiscard]] std::optional<std::string> choose_project_to_open();

[[nodiscard]] std::optional<std::string> choose_projection_mesh();

[[nodiscard]] std::optional<std::string> choose_property_file(const FilePickerRequest& request);

[[nodiscard]] std::string projection_mesh_name(std::string_view source);

[[nodiscard]] std::optional<std::string>
choose_project_to_save(std::string_view current_path);

// The editor's body text, in logical units. Every row height and inset in the
// panels is laid out around it.
constexpr core::u32 editor_font_size = 14U;

// The interface zoom steps Ctrl+= and Ctrl+- walk through, in percent of the
// display's own scale.
constexpr std::array<core::u16, 10U> ui_zoom_steps{
    {67U, 75U, 85U, 100U, 115U, 125U, 150U, 175U, 200U, 250U}};

[[nodiscard]] core::u16 next_ui_zoom(core::u16 current, bool larger) noexcept;

// The Compositor node an Effects-panel effect becomes when it is dropped on
// the graph, or nothing for an effect the graph has no node for.
[[nodiscard]] std::optional<compositor::NodeKind>
graph_effect_node(scene::EffectKind effect) noexcept;

[[nodiscard]] gui::FontAtlas load_editor_font(float scale);

[[nodiscard]] EditorError map_window_error(gui::WindowError error) noexcept;

[[nodiscard]] const ElementInfo *
find_element(const std::vector<ElementInfo> &elements,
             doc::NodeId node) noexcept;

// `length` along world axis `column` (0 = X).
[[nodiscard]] scene::Vec3 with_axis(core::u32 column, float length) noexcept;

// A Blender-linked object (not its collection): placed by one world matrix.
[[nodiscard]] bool linked_occurrence(const ElementInfo *element) noexcept;

[[nodiscard]] bool keyed(
    std::span<const scene::ChannelKeyframes> keyframes, doc::NodeId node,
    scene::TransformChannel component) noexcept;

[[nodiscard]] double surface_transform_value(
    const output::PhysicalSurface &surface,
    scene::TransformChannel component) noexcept;

[[nodiscard]] std::optional<MaskViewportSourceMapping>
mask_source_mapping(const scene::Scene &view, doc::NodeId target,
                    const ViewportProjection *projection);

} // namespace mediaserver::editor::shell_detail

namespace mediaserver::editor {
using namespace shell_detail;
} // namespace mediaserver::editor
