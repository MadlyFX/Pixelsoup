#include "shell_chrome.hpp"

namespace mediaserver::editor::shell_detail {

[[nodiscard]] bool chrome_button(gui::UiContext &ui, gui::UiId id,
                                 const gui::Rect &bounds,
                                 std::string_view label, bool active,
                                 bool enabled) {
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
  ui.outline(bounds,
             !enabled ? ui.theme().divider
                   : (active ? ui.theme().accent : ui.theme().header));
  ui.label_centered(bounds, label,
                    !enabled ? ui.theme().text_muted
                             : (active ? gui::UiColor{0.920F, 0.970F, 1.000F, 1.0F}
                                       : ui.theme().text));
  return enabled && it.clicked;
}

// A warning along the top of a panel: a 24px box `top` below its edge, at
// most `max_width` wide, centred or 12px in from the left.
void draw_panel_warning(gui::UiContext &ui, const gui::Rect &area, float top,
                        float max_width, bool centred, std::string_view text) {
  gui::Rect strip = gui::inset(area, gui::Padding{12.0F, top, 12.0F, 0.0F});
  strip = gui::cut_top(strip, 24.0F);
  const float width = std::min(strip.width, max_width);
  const gui::Rect notice = centred ? gui::centered(strip, width, strip.height)
                                   : gui::cut_left(strip, width);
  ui.panel(notice, ui.theme().background);
  ui.outline(notice, ui.theme().warning);
  ui.label(gui::inset(notice, 8.0F, 0.0F), text, ui.theme().warning);
}

// A status banner in the top-left of a picture: a bar of `color` down its
// left edge, the headline in that colour, then lines of detail.
void draw_status_banner(
    gui::UiContext &ui, const gui::Rect &area, float width, float height,
    gui::UiColor color, std::string_view headline,
    std::initializer_list<std::pair<std::string_view, gui::UiColor>> lines) {
  gui::Rect corner = gui::inset(area, 8.0F);
  gui::Rect banner = gui::cut_top(corner, height);
  banner = gui::cut_left(banner, width);
  ui.panel(banner, ui.theme().header);
  gui::Rect text = banner;
  ui.panel(gui::cut_left(text, 3.0F), color);
  gui::Metrics metrics{};
  metrics.inset = 0.0F;
  metrics.gap = 5.0F;
  gui::FormLayout rows{
      gui::inset(text, gui::Padding{9.0F, 0.0F, 12.0F, 0.0F}), metrics};
  ui.label(rows.next(21.0F, 1.0F), headline, color);
  for (const auto &[line, line_color] : lines) {
    ui.label(rows.next(20.0F, 1.0F), line, line_color);
  }
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

// One pixel of a wall, magnified: a soft square holding its red, green and
// blue emitters.
void draw_brand_mark(gui::UiContext &ui, float x, float y) {
  const gui::Rect tile{x, y, 32.0F, 32.0F};
  ui.rounded(tile, ui.theme().header, 10.0F);
  constexpr std::array<gui::UiColor, 3U> emitters{{
      {1.000F, 0.420F, 0.420F, 1.0F},
      {0.357F, 0.890F, 0.561F, 1.0F},
      {0.353F, 0.659F, 1.000F, 1.0F},
  }};
  for (std::size_t index = 0U; index < emitters.size(); ++index) {
    const gui::Rect emitter{x + 7.5F + 6.5F * static_cast<float>(index),
                            y + 8.0F, 4.0F, 16.0F};
    ui.glow(emitter, emitters[index], 2.0F, 5.0F);
    ui.rounded(emitter, emitters[index], 2.0F);
  }
}

// The editor dimmed for the small hours: surfaces, text and every glow pulled
// down together, so the room stays dark and the wall stays the brightest
// thing in it. The picture itself is never touched.
[[nodiscard]] gui::Theme night_theme(gui::Theme theme) noexcept {
  const auto dim = [](gui::UiColor color, float amount) noexcept {
    return gui::UiColor{color.r * amount, color.g * amount, color.b * amount,
                        color.a};
  };
  theme.background = dim(theme.background, 0.7F);
  theme.panel = dim(theme.panel, 0.72F);
  theme.header = dim(theme.header, 0.74F);
  theme.row = dim(theme.row, 0.72F);
  theme.row_alt = dim(theme.row_alt, 0.72F);
  theme.widget = dim(theme.widget, 0.76F);
  theme.widget_hover = dim(theme.widget_hover, 0.8F);
  theme.track = dim(theme.track, 0.7F);
  theme.divider = dim(theme.divider, 0.75F);
  theme.border = dim(theme.border, 0.75F);
  theme.text = dim(theme.text, 0.82F);
  theme.text_muted = dim(theme.text_muted, 0.85F);
  theme.glow *= 0.45F;
  // The gizmo keeps its daylight values. Like the playhead and the state
  // colours it is an instrument drawn over the picture, not a surface behind
  // it, and the axis an operator has to grab at three in the morning is the
  // last thing to dim.
  return theme;
}

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

[[nodiscard]] const std::array<WindowPanelEntry, 22U> &window_panel_entries() {
  static const std::array<WindowPanelEntry, 22U> entries{{
      {panel_stage, "View"},
      {panel_program, "Program Output"},
      {panel_asset_viewer, "Asset Viewer"},
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

// Where each menu-bar entry sits: left to right after the brand, each as
// wide as its label and padding. The bar draws them there and the menus
// open under them.
[[nodiscard]] std::array<gui::Rect, menu_bar_entries.size()>
menu_bar_layout(gui::UiContext &ui) {
  std::array<gui::Rect, menu_bar_entries.size()> entries{};
  gui::Rect bar{menu_bar_left, 0.0F, std::numeric_limits<float>::max(),
                top_bar_height};
  for (std::size_t index = 0U; index < entries.size(); ++index) {
    entries[index] = gui::cut_left(
        bar, ui.font_width(menu_bar_entries[index].label) + 18.0F);
  }
  return entries;
}

[[nodiscard]] gui::Rect menu_bar_entry_bounds(gui::UiContext &ui,
                                              std::string_view wanted) {
  const auto entries = menu_bar_layout(ui);
  for (std::size_t index = 0U; index < entries.size(); ++index) {
    if (menu_bar_entries[index].label == wanted) {
      return entries[index];
    }
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
                     static_cast<float>(window_panel_entries().size()) *
                             window_menu_row_height +
                         application_menu_padding * 2.0F};
  }
  return {};
}

// Where `menu`'s rows sit in `popup`, in item order. Input and drawing both
// walk these, so a click lands on the row that was drawn.
[[nodiscard]] std::vector<MenuRow> menu_rows(ApplicationMenu menu,
                                             const gui::Rect &popup) {
  gui::Metrics metrics{};
  metrics.inset = application_menu_padding;
  metrics.gap = application_menu_padding;
  gui::FormLayout rows{popup, metrics};
  std::vector<MenuRow> laid;
  const auto add = [&](float height, bool rule_above) {
    MenuRow row{};
    if (rule_above) {
      const gui::Rect gap = rows.next(file_menu_separator_height, 0.0F);
      row.rule = gui::inset(
          gap, gui::Padding{4.0F, 3.0F, 4.0F, file_menu_separator_height - 4.0F});
    }
    row.row = rows.next(height, 0.0F);
    laid.push_back(row);
  };
  switch (menu) {
  case ApplicationMenu::File:
    // Above the rules the File menu acts on the project file; the scene link
    // is its own group; below the last are the two settings dialogs - one
    // document, one machine.
    for (const ApplicationMenuItem &item : file_menu_items) {
      add(application_menu_row_height,
          item.action == ApplicationMenuAction::StartSceneLink ||
              item.action == ApplicationMenuAction::OpenProjectSettings);
    }
    break;
  case ApplicationMenu::View:
    for (std::size_t index = 0U; index < view_menu_items.size(); ++index) {
      add(application_menu_row_height, false);
    }
    break;
  case ApplicationMenu::Edit:
    // Undo and Redo, then the clipboard.
    for (std::size_t index = 0U; index < 4U; ++index) {
      add(application_menu_row_height, index == 2U);
    }
    break;
  case ApplicationMenu::Window:
    for (std::size_t index = 0U; index < window_panel_entries().size();
         ++index) {
      add(window_menu_row_height, false);
    }
    break;
  case ApplicationMenu::None:
    break;
  }
  return laid;
}

// The rows of the Recent Projects submenu.
[[nodiscard]] gui::Rect recent_project_row(const gui::Rect &submenu,
                                           std::size_t index) {
  return gui::list_row(
      gui::inset(submenu, application_menu_padding), index,
      application_menu_row_height);
}

[[nodiscard]] gui::Rect recent_projects_row(const gui::Rect &file_popup) {
  const std::vector<MenuRow> rows =
      menu_rows(ApplicationMenu::File, file_popup);
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    if (file_menu_items[index].action == ApplicationMenuAction::RecentProjects) {
      return rows[index].row;
    }
  }
  return {};
}

[[nodiscard]] gui::Rect recent_projects_popup(const gui::Rect &file_popup,
                                              float viewport_width,
                                              std::size_t project_count) {
  constexpr float preferred_width = 420.0F;
  const float right_x = file_popup.right();
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

[[nodiscard]] std::string_view scene_name(const EditorSwitcherState &switcher,
                                          scene::SceneId id) noexcept {
  for (const scene::SceneInfo &info : switcher.scenes) {
    if (info.id == id) {
      return info.name;
    }
  }
  return {};
}

// "Q2  Logo sting" for a cue, the way the Cue Stack names it.
[[nodiscard]] std::string cue_caption(
    std::span<const scene::TimelineMarker> markers, doc::NodeId id) {
  std::size_t number = 0U;
  for (const scene::TimelineMarker &marker : markers) {
    if (marker.is_cue()) {
      ++number;
    }
    if (marker.id == id) {
      std::string text = marker.is_cue() ? "Q" + std::to_string(number)
                                         : std::string{"M"};
      if (!marker.name.empty()) {
        text += "  ";
        text += marker.name;
      }
      return text;
    }
  }
  return {};
}

[[nodiscard]] HeaderActions
draw_application_header(gui::UiContext &ui, float width,
                        const EditorSwitcherState &switcher,
                        ApplicationMenu &open_menu,
                        const EditorProjectState &project,
                        const ShowBarState &show) {
  HeaderActions actions{};
  const gui::Theme &theme = ui.theme();
  const gui::Rect top{0.0F, 0.0F, width, top_bar_height};
  ui.panel(top, theme.background);
  const float middle = top_bar_height * 0.5F;
  // Two lines in a card: a small capitalised state over the thing it names.
  const auto two_lines = [&](const gui::Rect &cell, std::string_view caption,
                             gui::UiColor caption_color,
                             std::string_view value, gui::UiColor value_color) {
    const float line = ui.line_height();
    const float block = line * 2.0F - 2.0F;
    const float y = cell.y + (cell.height - block) * 0.5F;
    ui.label(gui::Rect{cell.x, y, cell.width, line}, caption, caption_color);
    ui.label(gui::Rect{cell.x, y + line - 2.0F, cell.width, line}, value,
             value_color);
  };

  // --- The project: a wall pixel for a mark, its name, whether it is saved.
  draw_brand_mark(ui, 14.0F, middle - 16.0F);
  {
    std::string name =
        project.path.empty()
            ? std::string{"Untitled"}
            : native_path_to_utf8(native_path_from_utf8(project.path).stem());
    const gui::Rect cell{56.0F, 0.0F, menu_bar_left - 64.0F, top_bar_height};
    const float line = ui.line_height();
    const float y = middle - line + 1.0F;
    ui.label(gui::Rect{cell.x, y, cell.width, line}, name, theme.text);
    const gui::UiColor state = project.dirty ? theme.caution : theme.good;
    draw_status_dot(ui, cell.x + 4.0F, y + line + line * 0.5F - 2.0F, state,
                    3.0F);
    ui.label(gui::Rect{cell.x + 13.0F, y + line - 2.0F, cell.width - 13.0F,
                       line},
             project.dirty ? "Unsaved changes" : "Saved", theme.text_muted);
    if (!project.path.empty()) {
      ui.tooltip(cell, project.path);
    }
  }

  // --- The menus: quiet words that light up as pills under the pointer.
  const auto menus = menu_bar_layout(ui);
  for (std::size_t index = 0U; index < menus.size(); ++index) {
    const MenuBarEntry &entry = menu_bar_entries[index];
    const gui::Rect menu = menus[index];
    const gui::UiId menu_id = gui::ui_id(gui::ui_id("editor.menu"), entry.label);
    const gui::Interaction it = ui.grab(menu_id, menu);
    ui.cursor_for(it, gui::Cursor::Hand);
    const bool active =
        entry.menu != ApplicationMenu::None && open_menu == entry.menu;
    const float hover =
        ui.ease(menu_id ^ gui::ui_id("hover"), it.hovered || active ? 1.0F : 0.0F);
    const gui::Rect pill = gui::centered(menu, menu.width, 32.0F);
    if (hover > 0.0F) {
      ui.rounded(pill, gui::fade(theme.widget, hover), 16.0F);
    }
    ui.label_centered(pill, entry.label,
                      gui::blend(theme.text_muted, theme.text, hover));
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
  }
  const float menus_right = menus.back().right();

  // --- The right-hand cluster, laid out from the window's right edge: the
  // night dim, render-server health, then the cue stack with its GO.
  gui::Rect right{menus_right, 0.0F, std::max(width - menus_right - 10.0F, 0.0F),
                  top_bar_height};
  {
    const gui::Rect night = gui::centered(gui::cut_right(right, 40.0F), 40.0F, 40.0F);
    actions.toggle_night = icon_button(
        ui, gui::ui_id("editor.show-bar.night"), night, gui::Icon::Night,
        show.night ? "Night dim is on: back to full brightness"
                   : "Night dim: calm the editor for a long night",
        show.night);
    static_cast<void>(gui::cut_right(right, 8.0F));
  }

  const bool roomy = width >= 1500.0F;
  if (show.outputs != nullptr && width >= 1100.0F) {
    const OutputMonitorState &outputs = *show.outputs;
    std::size_t live = 0U;
    for (const OutputMonitorState::Node &node : outputs.nodes) {
      live += node.connection == OutputMonitorState::Node::Connection::Live ? 1U : 0U;
    }
    const std::size_t total = outputs.nodes.size();
    std::string_view caption = "No outputs";
    gui::UiColor caption_color = theme.text_muted;
    if (total != 0U) {
      switch (outputs.cluster.health) {
      case OutputMonitorState::Cluster::Health::Healthy:
        caption = "All live";
        caption_color = theme.good;
        break;
      case OutputMonitorState::Cluster::Health::Degraded:
        caption = "Degraded";
        caption_color = theme.caution;
        break;
      case OutputMonitorState::Cluster::Health::Faulted:
        caption = "Faulted";
        caption_color = theme.warning;
        break;
      case OutputMonitorState::Cluster::Health::Unmonitored:
        caption = live == total ? "Live" : "Offline";
        caption_color = live == total ? theme.good : theme.text_muted;
        break;
      }
    }
    const std::string count =
        std::to_string(live) + " / " + std::to_string(total) + " live";
    const float text_width = std::max(ui.font_width(caption), ui.font_width(count));
    const gui::Rect card = gui::centered(
        gui::cut_right(right, text_width + 32.0F), text_width + 32.0F, 46.0F);
    static_cast<void>(gui::cut_right(right, 8.0F));
    const gui::UiId id = gui::ui_id("editor.show-bar.health");
    const gui::Interaction it = ui.grab(id, card);
    ui.cursor_for(it, gui::Cursor::Hand);
    const float hover = ui.ease(id ^ gui::ui_id("hover"), it.hovered ? 1.0F : 0.0F);
    ui.rounded(card, gui::blend(theme.header, theme.widget, hover), 14.0F);
    two_lines(gui::Rect{card.x + 14.0F, card.y, text_width + 4.0F, card.height},
              caption, caption_color, count, theme.text);
    ui.tooltip(card, std::string{caption} + ": " + count +
                         ". Click for the Output Monitor");
    actions.show_outputs = it.clicked;
  }

  if (show.timeline != nullptr && width >= 1000.0F) {
    const EditorTimeline &timeline = *show.timeline;
    bool any_cue = false;
    for (const scene::TimelineMarker &marker : show.markers) {
      any_cue = any_cue || marker.is_cue();
    }
    std::string_view caption = "Next cue";
    gui::UiColor caption_color = theme.cue;
    std::string what;
    if (!any_cue) {
      caption = "No cues";
      caption_color = theme.text_muted;
      what = "Q adds one";
    } else if (!timeline.cues_armed) {
      caption = "Cues off";
      caption_color = theme.text_muted;
      what = "Playing through";
    } else if (timeline.held_cue != doc::NodeId{}) {
      caption = "Holding";
      caption_color = theme.caution;
      what = cue_caption(show.markers, timeline.held_cue);
    } else if (timeline.looping_cue != doc::NodeId{}) {
      caption = "Looping";
      caption_color = theme.accent;
      what = cue_caption(show.markers, timeline.looping_cue);
    } else if (timeline.next_cue != doc::NodeId{}) {
      what = cue_caption(show.markers, timeline.next_cue);
    } else {
      caption = "End of stack";
      caption_color = theme.text_muted;
    }
    const float text_width = roomy ? 150.0F : 96.0F;
    const gui::Rect card = gui::centered(
        gui::cut_right(right, text_width + 70.0F), text_width + 70.0F, 50.0F);
    static_cast<void>(gui::cut_right(right, 8.0F));
    ui.rounded(card, theme.header, 25.0F);
    two_lines(gui::Rect{card.x + 18.0F, card.y, text_width, card.height},
              caption, caption_color, what, theme.text);
    // GO: the stack's one big key, lit magenta whenever there is a cue to
    // run and the show is not already running to it.
    const bool running = timeline.playing && timeline.looping_cue == doc::NodeId{} &&
                         timeline.held_cue == doc::NodeId{};
    const bool live = any_cue && !running;
    const gui::Rect go{card.right() - 46.0F, card.center_y() - 20.0F, 40.0F, 40.0F};
    const gui::UiId go_id = gui::ui_id("editor.show-bar.go");
    const gui::Interaction it = any_cue ? ui.grab(go_id, go) : gui::Interaction{};
    if (any_cue) {
      ui.cursor_for(it, gui::Cursor::Hand);
    }
    const bool pressed = it.held && !it.released;
    const gui::Rect key = pressed ? go.inflated(-1.5F, -1.5F) : go;
    if (live) {
      ui.glow(key, theme.cue, 20.0F, it.hovered ? 14.0F : 10.0F);
      ui.rounded(key, it.hovered ? gui::blend(theme.cue, theme.text, 0.15F) : theme.cue, key.height * 0.5F);
    } else {
      ui.rounded(key, theme.widget, key.height * 0.5F);
    }
    ui.label_centered(key, "GO",
                      live ? gui::UiColor{0.16F, 0.03F, 0.13F, 1.0F} : theme.text_muted);
    ui.tooltip(go, "GO: run from a hold, release a loop, or play to the next cue");
    actions.go = any_cue && it.clicked;
  }

  // --- Program, Take and Preview, centred in what is left. Program glows
  // green because it is on the wall; the one being edited is ringed.
  const float left_edge = menus_right + 16.0F;
  const float right_edge = right.right() - 8.0F;
  const float room = std::max(right_edge - left_edge, 0.0F);
  const float card_width = std::clamp((room - 120.0F) * 0.5F, 140.0F, 220.0F);
  constexpr float take_width = 104.0F;
  constexpr float group_gap = 10.0F;
  const float group_width = card_width * 2.0F + take_width + group_gap * 2.0F;
  const float group_x = std::clamp(width * 0.5F - group_width * 0.5F, left_edge,
                                   std::max(left_edge, right_edge - group_width));
  const gui::Rect program_card{group_x, middle - 23.0F, card_width, 46.0F};
  const gui::Rect take{program_card.right() + group_gap, middle - 22.0F,
                       take_width, 44.0F};
  const gui::Rect preview_card{take.right() + group_gap, middle - 23.0F,
                               card_width, 46.0F};
  const bool modes_enabled = !switcher.rehearsal_locked;
  const auto monitor = [&](const gui::Rect &card, gui::UiId id,
                           std::string_view caption, gui::UiColor color,
                           std::string_view name, bool editing, bool lit,
                           std::string_view tip,
                           const rhi::Texture *picture = nullptr) {
    const gui::Interaction it =
        modes_enabled ? ui.grab(id, card) : gui::Interaction{};
    if (modes_enabled) {
      ui.cursor_for(it, gui::Cursor::Hand);
    }
    const float hover = ui.ease(id ^ gui::ui_id("hover"), it.hovered ? 1.0F : 0.0F);
    if (lit) {
      ui.glow(card, color, 14.0F, 12.0F);
    }
    ui.rounded(card, gui::blend(theme.header, theme.widget, hover * 0.8F), 14.0F);
    if (editing) {
      ui.rounded_outline(card, gui::fade(color, 0.85F), 14.0F, 1.5F);
    }
    // The wall itself: the picture on it when there is one, otherwise a
    // tile of six lit emitters in the card's colour.
    const gui::Rect tile{card.x + 7.0F, card.y + 7.0F, 52.0F, 32.0F};
    if (picture != nullptr) {
      ui.rounded(tile, gui::UiColor{0.0F, 0.0F, 0.0F, 1.0F}, 8.0F);
      static_cast<void>(ui.image_fit(tile, *picture, show.program_aspect));
      ui.draw_list().add_corner_mask(tile.x, tile.y, tile.width, tile.height,
                                     8.0F, theme.header);
      ui.rounded_outline(tile, gui::fade(color, 0.5F), 8.0F);
    } else {
      ui.rounded(tile, gui::fade(color, 0.16F), 8.0F);
      for (std::size_t index = 0U; index < 6U; ++index) {
        const float cx = tile.x + 16.0F + 10.0F * static_cast<float>(index % 3U);
        const float cy = tile.y + 11.0F + 10.0F * static_cast<float>(index / 3U);
        draw_filled_circle(ui, cx, cy, 2.0F,
                           gui::fade(color, lit ? 0.95F : 0.6F));
      }
    }
    two_lines(gui::Rect{tile.right() + 10.0F, card.y,
                        card.right() - tile.right() - 18.0F, card.height},
              caption, color, name.empty() ? std::string_view{"No scene"} : name,
              theme.text);
    ui.tooltip(card, tip);
    return modes_enabled && it.clicked;
  };
  actions.program = monitor(
      program_card, gui::ui_id("editor.mode.program"),
      switcher.program_held ? "On air - held" : "On air", theme.good,
      scene_name(switcher, switcher.program_scene), !switcher.rehearsal_mode,
      true, "Program is on the wall. Click to edit it directly",
      show.program_picture);
  const bool has_rehearsal = switcher.rehearsal_scene != scene::SceneId{};
  actions.preview = monitor(
      preview_card, gui::ui_id("editor.mode.preview"), "Preview",
      theme.preview,
      has_rehearsal ? scene_name(switcher, switcher.rehearsal_scene)
                    : std::string_view{"Rehearse here"},
      switcher.rehearsal_mode, switcher.rehearsal_mode,
      "Preview: rehearse a scene off air, then Take it to the wall");

  // Take: held by the switcher's own preconditions - a Preview scene to
  // promote and no transition already running - and by nothing else except a
  // STRICT synchronization policy whose timing is unproven. A dissolve also
  // needs a matching canvas; the key says Cut rather than failing silently.
  const bool canvas_mismatch = has_rehearsal && !switcher.dissolve_compatible;
  const bool swap_enabled = !switcher.transition_active &&
                            !switcher.rehearsal_locked && has_rehearsal &&
                            !switcher.program_held && !switcher.publication_pending;
  const std::string_view take_label =
      switcher.transition_active
          ? "Fading"
          : (switcher.program_held ? "Held"
             : switcher.publication_pending ? "Staging"
             : switcher.linked_publication ? "Publish"
             : (canvas_mismatch ? "Cut" : "Take"));
  {
    const gui::UiId id = gui::ui_id("editor.mode.swap");
    const gui::Interaction it = swap_enabled ? ui.grab(id, take) : gui::Interaction{};
    if (swap_enabled) {
      ui.cursor_for(it, gui::Cursor::Hand);
    }
    const bool pressed = it.held && !it.released;
    const float hover = ui.ease(id ^ gui::ui_id("hover"), it.hovered ? 1.0F : 0.0F);
    const gui::Rect key = pressed ? take.inflated(-1.5F, -1.5F) : take;
    const float radius = key.height * 0.5F;
    if (switcher.transition_active) {
      // The dissolve fills the key from the left as it runs.
      ui.glow(key, theme.good, radius, 12.0F);
      ui.rounded(key, theme.widget, radius);
      const float mix = std::clamp(switcher.transition_mix, 0.0F, 1.0F);
      if (mix > 0.0F) {
        ui.rounded(gui::Rect{key.x, key.y, std::max(key.width * mix, key.height), key.height},
                   gui::fade(theme.good, 0.45F), radius);
      }
    } else if (swap_enabled) {
      ui.rounded(key, pressed ? theme.widget_active
                              : gui::blend(theme.widget, theme.widget_hover, hover),
                 radius);
      ui.rounded_outline(key, gui::fade(theme.good, 0.35F + 0.4F * hover), radius, 1.5F);
    } else {
      ui.rounded(key, theme.track, radius);
    }
    const gui::UiColor ink = swap_enabled || switcher.transition_active
                                 ? theme.text : theme.text_muted;
    const float label_width = ui.font_width(take_label);
    const float content = 18.0F + 8.0F + label_width;
    const float x = key.center_x() - content * 0.5F;
    gui::draw_icon(ui.draw_list(), gui::Icon::Take, x + 9.0F, key.center_y(), ink, 1.15F);
    ui.label(gui::Rect{x + 26.0F, key.y, label_width + 2.0F, key.height}, take_label, ink);
    ui.tooltip(take, canvas_mismatch
                         ? "Cut Preview to the wall (Ctrl+Enter). The canvases "
                           "differ, so it cannot dissolve"
                         : "Take: dissolve Preview to the wall (Ctrl+Enter)");
    // Ctrl+Enter is the keyboard's Take, held by everything that holds the
    // key - and by any field or menu that has the keyboard.
    const bool take_key = !ui.typing() && ui.key_pressed(gui::Key::Take);
    actions.swap = swap_enabled && (it.clicked || take_key);
  }
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
      for (std::size_t index = 0U; index < recent_projects.size(); ++index) {
        const std::string &path = recent_projects[index];
        const gui::Rect row = recent_project_row(recent_popup, index);
        if (ui.grab(gui::ui_id(gui::ui_id("editor.file-menu.recent"), path), row).clicked) {
          result.action = ApplicationMenuAction::OpenRecentProject;
          result.project_path = path;
          open_menu = ApplicationMenu::None;
          recent_submenu_open = false;
          return result;
        }
      }
    }
    const std::vector<MenuRow> rows = menu_rows(open_menu, popup);
    for (std::size_t index = 0U; index < file_menu_items.size(); ++index) {
      const auto &[label, action] = file_menu_items[index];
      const gui::Rect row = rows[index].row;
      if (action == ApplicationMenuAction::RecentProjects) {
        static_cast<void>(ui.grab(gui::ui_id(gui::ui_id("editor.file-menu"), label), row));
        continue;
      }
      if (ui.grab(gui::ui_id(gui::ui_id("editor.file-menu"), label), row).clicked) {
        result.action = action;
        open_menu = ApplicationMenu::None;
        recent_submenu_open = false;
        return result;
      }
    }
    return result;
  }

  const std::vector<MenuRow> rows = menu_rows(open_menu, popup);
  if (open_menu == ApplicationMenu::View) {
    for (std::size_t index = 0U; index < view_menu_items.size(); ++index) {
      const ViewMenuItem &item = view_menu_items[index];
      if (ui.grab(gui::ui_id(gui::ui_id("editor.view-menu"), item.label),
                  rows[index].row)
              .clicked) {
        result.action = item.action;
        open_menu = ApplicationMenu::None;
        return result;
      }
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
    for (std::size_t index = 0U; index < items.size(); ++index) {
      const auto &[label, action] = items[index];
      if (ui.grab(gui::ui_id(gui::ui_id("editor.edit-menu"), label), rows[index].row)
              .clicked) {
        result.action = action;
        open_menu = ApplicationMenu::None;
        return result;
      }
    }
    return result;
  }

  for (std::size_t index = 0U; index < window_panel_entries().size();
       ++index) {
    const WindowPanelEntry &entry = window_panel_entries()[index];
    if (ui.grab(gui::ui_id(gui::ui_id("editor.window-menu"), entry.title),
                rows[index].row)
            .clicked) {
      result.action = ApplicationMenuAction::TogglePanel;
      result.panel = entry.panel;
      open_menu = ApplicationMenu::None;
      return result;
    }
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
  // A popup card with its shadow down and to the right.
  const auto card = [&](const gui::Rect &bounds) {
    gui::Rect shadow = bounds;
    shadow.x += 3.0F;
    shadow.y += 4.0F;
    ui.panel(shadow, gui::UiColor{0.0F, 0.0F, 0.0F, 0.35F});
    ui.panel(bounds, ui.theme().panel);
    ui.outline(bounds, ui.theme().border);
    ui.push_clip(bounds);
  };
  card(popup);
  const std::vector<MenuRow> rows = menu_rows(open_menu, popup);
  const auto rule = [&](const MenuRow &row) {
    if (row.rule.has_value()) {
      ui.panel(*row.rule, ui.theme().divider);
    }
  };

  const auto draw_row = [&](gui::UiId id, const gui::Rect &row,
                            std::string_view label, bool selected = false) {
    const bool hovered = row.contains(ui.pointer_x(), ui.pointer_y());
    if (hovered || ui.active() == id || selected) {
      ui.panel(row,
               hovered ? ui.theme().widget_hover : ui.theme().widget_active);
    }
    ui.label(gui::inset(row, gui::Padding{8.0F, 0.0F, 4.0F, 0.0F}), label,
             hovered || selected ? ui.theme().text : ui.theme().text_muted);
  };

  if (open_menu == ApplicationMenu::File) {
    for (std::size_t index = 0U; index < file_menu_items.size(); ++index) {
      const ApplicationMenuItem &item = file_menu_items[index];
      const std::string_view label = item.label;
      rule(rows[index]);
      const gui::Rect row = rows[index].row;
      draw_row(gui::ui_id(gui::ui_id("editor.file-menu"), label), row, label,
               item.action == ApplicationMenuAction::RecentProjects &&
                   recent_submenu_open);
      if (item.action == ApplicationMenuAction::RecentProjects) {
        gui::Rect arrow = gui::inset(row, gui::Padding{0.0F, 0.0F, 6.0F, 0.0F});
        ui.label(gui::cut_right(arrow, 12.0F), ">", ui.theme().text_muted);
      }
    }
  } else if (open_menu == ApplicationMenu::View) {
    for (std::size_t index = 0U; index < view_menu_items.size(); ++index) {
      const ViewMenuItem &item = view_menu_items[index];
      const gui::Rect row = rows[index].row;
      draw_row(gui::ui_id(gui::ui_id("editor.view-menu"), item.label), row,
               item.label);
      gui::Rect shortcut = gui::inset(row, gui::Padding{0.0F, 0.0F, 8.0F, 0.0F});
      ui.label(gui::cut_right(shortcut, ui.font_width(item.shortcut) + 2.0F),
               item.shortcut, ui.theme().text_muted);
    }
  } else if (open_menu == ApplicationMenu::Edit) {
    constexpr std::array<std::string_view, 4U> labels{
        "Undo    Ctrl+Z", "Redo    Ctrl+Shift+Z", "Copy    Ctrl+C",
        "Paste   Ctrl+V"};
    constexpr std::array<std::string_view, 4U> ids{"Undo", "Redo", "Copy",
                                                   "Paste"};
    for (std::size_t index = 0U; index < labels.size(); ++index) {
      rule(rows[index]);
      draw_row(gui::ui_id(gui::ui_id("editor.edit-menu"), ids[index]),
               rows[index].row, labels[index]);
    }
  } else {
    for (std::size_t index = 0U; index < window_panel_entries().size();
         ++index) {
      const WindowPanelEntry &entry = window_panel_entries()[index];
      const bool open = docks.leaf_of(entry.panel) != gui::no_dock;
      std::string label = open ? "[x] " : "[ ] ";
      label.append(entry.title);
      draw_row(gui::ui_id(gui::ui_id("editor.window-menu"), entry.title),
               rows[index].row, label);
    }
  }
  ui.pop_clip();
  if (open_menu == ApplicationMenu::File && recent_submenu_open) {
    const gui::Rect submenu =
        recent_projects_popup(popup, viewport_width, recent_projects.size());
    card(submenu);
    if (recent_projects.empty()) {
      ui.label(gui::inset(recent_project_row(submenu, 0U), 8.0F, 0.0F),
               "None yet", ui.theme().text_muted);
    } else {
      for (std::size_t index = 0U; index < recent_projects.size(); ++index) {
        const std::string &path = recent_projects[index];
        draw_row(gui::ui_id(gui::ui_id("editor.file-menu.recent"), path),
                 recent_project_row(submenu, index),
                 recent_project_label(path));
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
                                   core::u16 zoom_percent, float leading) {
  const gui::Rect status{0.0F, std::max(height - status_bar_height, 0.0F),
                         width, status_bar_height};
  ui.panel(status, ui.theme().background);

  // Each reading is a soft chip: a quiet label and a value, with a dot where
  // the value is a state - the way a broadcast status line reads, rather than
  // a run of capitals the operator has to parse. Chips run in from the left.
  // `leading` is kept clear at the left for the History strip's chip.
  gui::Rect readings =
      gui::inset(status, gui::Padding{8.0F + leading, 0.0F, 0.0F, 0.0F});
  const auto reading = [&](std::string_view label, std::string_view value,
                           std::optional<gui::UiColor> dot,
                           gui::UiColor value_color) {
    const float label_width = label.empty() ? 0.0F : ui.font_width(label) + 6.0F;
    const float dot_width = dot.has_value() ? 13.0F : 0.0F;
    const float value_width = ui.font_width(value);
    gui::Rect cell = gui::cut_left(
        readings, label_width + dot_width + value_width + 22.0F);
    static_cast<void>(gui::cut_left(readings, 6.0F));
    const gui::Rect chip = gui::inset(cell, 0.0F, 3.0F);
    ui.rounded(chip, ui.theme().panel, chip.height * 0.5F);
    static_cast<void>(gui::cut_left(cell, 10.0F));
    const gui::Rect label_cell = gui::cut_left(cell, label_width);
    if (!label.empty()) {
      ui.label(label_cell, label, ui.theme().text_muted);
    }
    const gui::Rect dot_cell = gui::cut_left(cell, dot_width);
    if (dot.has_value()) {
      draw_status_dot(ui, dot_cell.x + 4.0F, dot_cell.center_y(), *dot, 3.0F);
    }
    ui.label(gui::cut_left(cell, value_width + 2.0F), value, value_color);
  };

  // Preview owns its own playhead, so an operator scrubbing a rehearsal scene
  // must still be able to read where the live show is without leaving Preview.
  reading(switcher.rehearsal_mode ? "Preview" : "Program",
          "frame " + std::to_string(timeline.show_frame),
          std::nullopt,
          ui.theme().text);
  reading("", timeline.playing ? "Playing" : "Paused",
          std::nullopt,
          ui.theme().text);
  if (switcher.rehearsal_mode)
    reading("Program", "frame " + std::to_string(timeline.program_frame), std::nullopt, ui.theme().text_muted);
  ui.tooltip({status.x, status.y, readings.x - status.x, status.height},
             "Canvas " + std::to_string(canvas.width) + " x " + std::to_string(canvas.height) +
             " · " + std::to_string(selection_count) + " selected");

  // The interface zoom, at the right edge: what Ctrl+= and Ctrl+- changed,
  // and a click away from the View menu that changes it back.
  const std::string zoom_text = "UI " + std::to_string(zoom_percent) + "%";
  gui::Rect trailing = gui::inset(status, gui::Padding{0.0F, 4.0F, 6.0F, 4.0F});
  const gui::Rect zoom =
      gui::cut_right(trailing, ui.font_width(zoom_text) + 20.0F);
  const gui::Interaction zoom_it =
      ui.grab(gui::ui_id("editor.status.zoom"), zoom);
  ui.cursor_for(zoom_it, gui::Cursor::Hand);
  ui.rounded(zoom, zoom_it.hovered || zoom_it.held ? ui.theme().widget_hover
                                                  : ui.theme().panel,
             zoom.height * 0.5F);
  ui.label(gui::inset(zoom, gui::Padding{10.0F, 0.0F, 0.0F, 0.0F}), zoom_text,
           zoom_percent == 100U ? ui.theme().text_muted : ui.theme().text);
  ui.tooltip(zoom, "Interface size - Ctrl+= larger, Ctrl+- smaller, Ctrl+0 "
                   "as the display asks");

  // One message at a time, right-aligned between the readings and the zoom,
  // never wider than `limit`.
  const gui::Rect messages{readings.x, status.y,
                           std::max(zoom.x - 4.0F - readings.x, 0.0F),
                           status.height};
  const auto message_box = [&](float limit) {
    gui::Rect lane = messages;
    lane.x -= 10.0F;
    return gui::cut_right(lane, std::min(messages.width, limit));
  };

  if (!ingest_message.empty()) {
    ui.label(message_box(580.0F), ingest_message, ui.theme().accent);
  } else if (!project.message.empty()) {
    const bool error = project.message.starts_with("ERROR");
    ui.label(message_box(310.0F), project.message,
             error ? ui.theme().warning : ui.theme().good);
  } else if (pending_media_probes > 0U) {
    std::array<char, 48U> probing_text{};
    const int probing_length = std::snprintf(
        probing_text.data(), probing_text.size(), "Reading media  %zu",
        pending_media_probes);
    ui.label(message_box(190.0F),
             std::string_view{
                 probing_text.data(),
                 static_cast<std::size_t>(std::max(probing_length, 0))},
             ui.theme().accent);
  }
  return zoom_it.clicked;
}

} // namespace mediaserver::editor::shell_detail
