#pragma once

// The shell's own chrome: the show bar across the top, the application
// menus, and the status bar. Internal to the editor library.

#include "shell_support.hpp"

namespace mediaserver::editor::shell_detail {

[[nodiscard]] bool chrome_button(gui::UiContext &ui, gui::UiId id,
                                 const gui::Rect &bounds,
                                 std::string_view label, bool active = false,
                                 bool enabled = true);

// A warning along the top of a panel: a 24px box `top` below its edge, at
// most `max_width` wide, centred or 12px in from the left.
void draw_panel_warning(gui::UiContext &ui, const gui::Rect &area, float top,
                        float max_width, bool centred, std::string_view text);

// A status banner in the top-left of a picture: a bar of `color` down its
// left edge, the headline in that colour, then lines of detail.
void draw_status_banner(
    gui::UiContext &ui, const gui::Rect &area, float width, float height,
    gui::UiColor color, std::string_view headline,
    std::initializer_list<std::pair<std::string_view, gui::UiColor>> lines);

void draw_inline_remove(gui::UiContext &ui, const gui::Rect &bounds,
                        gui::UiColor color);

// One pixel of a wall, magnified: a soft square holding its red, green and
// blue emitters.
void draw_brand_mark(gui::UiContext &ui, float x, float y);

struct HeaderActions final {
  bool program{};
  bool preview{};
  bool swap{};
  bool open_setup{};
  // The show bar's own controls: run the cue stack, bring the Output Monitor
  // forward, and dim the editor for a long night.
  bool go{};
  bool show_outputs{};
  bool toggle_night{};
};

// The editor dimmed for the small hours: surfaces, text and every glow pulled
// down together, so the room stays dark and the wall stays the brightest
// thing in it. The picture itself is never touched.
[[nodiscard]] gui::Theme night_theme(gui::Theme theme) noexcept;

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
    {"Freeze Blender...", ApplicationMenuAction::SceneLinkDetails},
    {"Save Project", ApplicationMenuAction::SaveProject},
    {"Save Project As...", ApplicationMenuAction::SaveProjectAs},
    {"Project Settings...", ApplicationMenuAction::OpenProjectSettings},
    {"Preferences...", ApplicationMenuAction::OpenPreferences},
}};

constexpr float application_menu_row_height = 22.0F;

constexpr float application_menu_padding = 4.0F;

constexpr float file_menu_separator_height = 8.0F;

constexpr float window_menu_row_height = 21.0F;

[[nodiscard]] std::string recent_project_label(std::string_view path);

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

[[nodiscard]] const std::array<WindowPanelEntry, 22U> &window_panel_entries();

// Where each menu-bar entry sits: left to right after the brand, each as
// wide as its label and padding. The bar draws them there and the menus
// open under them.
[[nodiscard]] std::array<gui::Rect, menu_bar_entries.size()>
menu_bar_layout(gui::UiContext &ui);

[[nodiscard]] gui::Rect menu_bar_entry_bounds(gui::UiContext &ui,
                                              std::string_view wanted);

[[nodiscard]] gui::Rect menu_popup_bounds(gui::UiContext &ui,
                                          ApplicationMenu menu);

// One row of an open application menu, and the rule above it when it
// starts a new group.
struct MenuRow final {
  gui::Rect row;
  std::optional<gui::Rect> rule;
};

// Where `menu`'s rows sit in `popup`, in item order. Input and drawing both
// walk these, so a click lands on the row that was drawn.
[[nodiscard]] std::vector<MenuRow> menu_rows(ApplicationMenu menu,
                                             const gui::Rect &popup);

// The rows of the Recent Projects submenu.
[[nodiscard]] gui::Rect recent_project_row(const gui::Rect &submenu,
                                           std::size_t index);

[[nodiscard]] gui::Rect recent_projects_row(const gui::Rect &file_popup);

[[nodiscard]] gui::Rect recent_projects_popup(const gui::Rect &file_popup,
                                              float viewport_width,
                                              std::size_t project_count);

enum class DeleteDomain : core::u8 {
  Scene,
  Hardware,
  Feed,
  Surface,
  Compositor,
  Neutral,
};

// What the show bar knows about the show beyond the switcher: where the cue
// stack stands and how the render servers are doing.
struct ShowBarState final {
  const EditorTimeline *timeline{};
  std::span<const scene::TimelineMarker> markers{};
  const OutputMonitorState *outputs{};
  bool night{};
  // The Program picture as the editor last composited it, shown in the
  // Program card; null shows the card's lit tile instead. Borrowed for the
  // frame, like any viewport image.
  const rhi::Texture *program_picture{};
  float program_aspect{16.0F / 9.0F};
};

[[nodiscard]] std::string_view scene_name(const EditorSwitcherState &switcher,
                                          scene::SceneId id) noexcept;

// "Q2  Logo sting" for a cue, the way the Cue Stack names it.
[[nodiscard]] std::string cue_caption(
    std::span<const scene::TimelineMarker> markers, doc::NodeId id);

[[nodiscard]] HeaderActions
draw_application_header(gui::UiContext &ui, float width,
                        const EditorSwitcherState &switcher,
                        ApplicationMenu &open_menu,
                        const EditorProjectState &project,
                        const ShowBarState &show = {});

[[nodiscard]] ApplicationMenuResult
run_application_menu_input(gui::UiContext &ui, float width, float height,
                           ApplicationMenu &open_menu,
                           bool &recent_submenu_open,
                           const gui::DockLayout &docks,
                           std::span<const std::string> recent_projects);

void draw_application_menu_overlay(gui::UiContext &ui,
                                   ApplicationMenu open_menu,
                                   bool recent_submenu_open,
                                   const gui::DockLayout &docks,
                                   std::span<const std::string> recent_projects,
                                   float viewport_width);

[[nodiscard]] bool draw_status_bar(gui::UiContext &ui, float width,
                                   float height,
                                   const EditorTimeline &timeline,
                                   const EditorSwitcherState &switcher,
                                   std::size_t selection_count,
                                   scene::Canvas canvas,
                                   const EditorProjectState &project,
                                   std::size_t pending_media_probes,
                                   std::string_view ingest_message,
                                   core::u16 zoom_percent, float leading);

} // namespace mediaserver::editor::shell_detail
