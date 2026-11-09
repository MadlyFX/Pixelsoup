// The show bar and application menus at the top of the window.

#include "shell_frame.hpp"

#include "native_dialogs.hpp"

namespace mediaserver::editor {

void EditorShell::Frame::run_header() {
  ui.set_theme(impl.night ? night_theme(editor_theme()) : editor_theme());
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
  width = static_cast<float>(impl.window.width());
  height = static_cast<float>(impl.window.height());

  // Preferences is modal. It has to be drawn last to sit on top, so the only
  // way it can also own the pointer is to deny it to everything drawn before.
  // The block is lifted immediately before the dialog runs. Whether it was
  // taken is remembered rather than re-tested, because the File menu can open
  // the dialog partway through this same frame.
  preferences_blocked_pointer = impl.preferences_panel.is_open() ||
      impl.project_settings_panel.is_open() || impl.scene_link_details_open || impl.blender_connection_open;
  if (preferences_blocked_pointer) {
    ui.push_pointer_block();
  }

  const HeaderActions header =
      draw_application_header(ui, width, switcher, impl.open_menu, project,
                              [&] {
                                ShowBarState show{&timeline, timeline_markers,
                                                  &output_monitor, impl.night};
                                if (impl.visible_program.has_value() &&
                                    impl.program_preview_valid &&
                                    impl.visible_program->height() != 0U) {
                                  show.program_picture = &*impl.visible_program;
                                  show.program_aspect =
                                      static_cast<float>(impl.visible_program->width()) /
                                      static_cast<float>(impl.visible_program->height());
                                }
                                return show;
                              }());
  menu_result = run_application_menu_input(ui, width, height, impl.open_menu,
                                 impl.recent_projects_submenu_open, impl.docks,
                                 project.recent_projects);
  // Menu rows overlay scene controls. Consume both press and selection frames
  // so starting a link cannot also arm the scene-delete control underneath it.
  application_menu_blocks_pointer = impl.open_menu != ApplicationMenu::None
      || menu_result.action != ApplicationMenuAction::None;
  if (application_menu_blocks_pointer) ui.push_pointer_block();
  switch (menu_result.action) {
  case ApplicationMenuAction::ImportLinkedScene:
    open_file_dialog(
        []() -> std::optional<std::string> {
          const char *const filters[] = {"*.mslink"};
          const char *const path = tinyfd_openFileDialog(
              "Import Linked Scene Manifest", "", 1, filters,
              "Linked scene manifest", 0);
          return path != nullptr ? std::optional<std::string>{path}
                                 : std::nullopt;
        },
        [](Frame &frame, std::string path) {
          frame.result.import_linked_scene = ProjectFileRequest{std::move(path)};
          frame.result.linked_scene_import_gesture =
              frame.impl.next_edit_gesture++;
        });
    break;
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
    confirm_discard("those changes and start a new project",
                    [](Frame &frame) { frame.result.new_project = true; });
    break;
  case ApplicationMenuAction::OpenProject:
    // Choose first, then confirm: the question is asked about the project as
    // it is once a file has been picked.
    open_file_dialog([] { return choose_project_to_open(); },
                     [](Frame &frame, std::string path) {
                       frame.confirm_discard(
                           "those changes and open another project",
                           [path = std::move(path)](Frame &next) {
                             next.result.load_project =
                                 ProjectFileRequest{path};
                           });
                     });
    break;
  case ApplicationMenuAction::RecentProjects:
    break;
  case ApplicationMenuAction::OpenRecentProject:
    confirm_discard("those changes and open another project",
                    [path = menu_result.project_path](Frame &frame) {
                      frame.result.load_project = ProjectFileRequest{path};
                    });
    break;
  case ApplicationMenuAction::SaveProject:
    save_project();
    break;
  case ApplicationMenuAction::SaveProjectAs:
    open_file_dialog(
        [current = std::string{project.path}] {
          return choose_project_to_save(current);
        },
        [](Frame &frame, std::string path) {
          frame.result.save_project = ProjectFileRequest{std::move(path)};
        });
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
    } else if (menu_result.panel == panel_hardware_mapping ||
               menu_result.panel == panel_feed_mapping) {
      impl.show_panel(menu_result.panel);
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
  if (header.toggle_night) {
    impl.night = !impl.night;
  }
  if (header.go) {
    result.transport = TransportRequest::Go;
  }
  if (header.show_outputs &&
      impl.docks.show(panel_output) == gui::no_dock) {
    impl.show_panel(panel_output);
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
}

core::u64 EditorShell::Frame::scene_selection_token() noexcept {
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
}

core::u64
EditorShell::Frame::begin_semantic_nudge(const NumericNudge &nudge,
                                         HistoryContext context,
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
}

void EditorShell::Frame::delete_selection() {
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
}

} // namespace mediaserver::editor
