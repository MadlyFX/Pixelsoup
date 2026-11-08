// The Scene outliner, Selection Sets and Masks panels.

#include "shell_frame.hpp"

namespace mediaserver::editor {

void EditorShell::Frame::draw_scene_panel(const gui::Rect &bounds) {
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
    ui.label(gui::Rect{header.x + 10.0F, header.y, 120.0F, header.height},
             "Scenes", theme.text_muted);
    const std::string count = std::to_string(switcher.scenes.size());
    ui.label(gui::Rect{header.x + 10.0F + ui.font_width("Scenes") + 8.0F,
                       header.y, 40.0F, header.height},
             count, with_alpha(theme.text_muted, 0.7F));
    const gui::Rect add_scene{header.right() - 56.0F,
                              header.y + 2.0F, 26.0F, 24.0F};
    const gui::Rect copy_scene{header.right() - 29.0F,
                               header.y + 2.0F, 26.0F, 24.0F};
    // The list scrolls to the scene a click adds once it exists. This
    // frame's rows still index the current list, so the scroll is applied
    // after they are drawn and clamped by the next frame.
    bool scroll_to_added = false;
    if (icon_button(ui, gui::ui_id("editor.scene.create"), add_scene,
                    gui::Icon::Plus, "New scene", false,
                    !switcher.rehearsal_locked)) {
      result.switcher = SwitcherRequest{SwitcherAction::CreateScene};
      scroll_to_added = true;
    }
    if (icon_button(ui, gui::ui_id("editor.scene.duplicate"), copy_scene,
                    gui::Icon::Duplicate,
                    "Duplicate the scene being edited", false,
                    !switcher.rehearsal_locked)) {
      result.switcher = SwitcherRequest{SwitcherAction::DuplicateScene,
                                        authored_stage.id, 0U};
      scroll_to_added = true;
    }

    float row_y = header.bottom() + 2.0F;
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
      const gui::Rect remove{row.right() - remove_width, row.y,
                             remove_width, row.height};
      const std::string_view tag =
          is_program ? "ON AIR"
                     : (is_rehearsal ? "PREVIEW" : (!compatible ? "Size" : ""));
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
      const float name_x = blender_hit.right() + 4.0F;
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
      // A soft capsule per scene, with its state as a lit stripe at the
      // left: green on air, cyan in Preview.
      ui.rounded(row, background, theme.radius);
      ui.rounded(gui::Rect{row.x + 3.0F, row.y + 6.0F, 3.0F, row.height - 12.0F},
                 is_program ? theme.good
                            : (is_rehearsal ? theme.preview : theme.border),
                 1.5F);

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
        ui.outline(badge, tone);
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
            !compatible ? theme.caution
                        : (is_program ? theme.good : theme.preview),
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
      ui.panel(gui::Rect{bounds.right() - 3.0F,
                         scene_rows.y + travel *
                                            static_cast<float>(impl.scene_list_offset) /
                                            static_cast<float>(max_offset),
                         2.0F, thumb},
               theme.widget_hover);
    }
    if (scroll_to_added) impl.scene_list_offset = max_offset + 1U;
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

void EditorShell::Frame::draw_selection_sets_panel(const gui::Rect &bounds) {
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

void EditorShell::Frame::draw_masks_panel(const gui::Rect &bounds) {
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

} // namespace mediaserver::editor
