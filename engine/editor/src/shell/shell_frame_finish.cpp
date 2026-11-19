// Ending a frame: shortcuts, modal dialogs, the status bar and presenting.

#include "shell_frame.hpp"

#include "native_dialogs.hpp"
#include "../timeline/timeline_internal.hpp"

namespace mediaserver::editor {

namespace {

// The timeline rate a video's own rate maps to, when that is one the show can
// run at. Other source rates never prompt: the Director would refuse them.
[[nodiscard]] std::optional<ShowRateRequest>
matching_common_rate(const assets::MediaFrameRate &source) noexcept {
  if (source.numerator == 0U || source.denominator == 0U) {
    return std::nullopt;
  }
  for (const timeline::RateChoice &choice : timeline::common_rates) {
    if (static_cast<core::u64>(choice.rate.numerator) * source.denominator ==
        static_cast<core::u64>(source.numerator) * choice.rate.denominator) {
      return choice.rate;
    }
  }
  return std::nullopt;
}

[[nodiscard]] std::string_view rate_name(ShowRateRequest rate) noexcept {
  for (const timeline::RateChoice &choice : timeline::common_rates) {
    if (choice.rate == rate) {
      return choice.label;
    }
  }
  return "this rate";
}

} // namespace

void EditorShell::Frame::finish() {
  // A video whose frame rate differs from the timeline's is held while the
  // operator chooses whether the timeline should take the clip's rate.
  const gui::UiId rate_menu = gui::ui_id("timeline.match_clip_rate");
  const ShowRateRequest timeline_rate{timeline.rate_numerator,
                                      timeline.rate_denominator};
  if (!impl.pending_rate_match.has_value()) {
    std::optional<ShowRateRequest> wanted;
    std::vector<AddIntent> held;
    for (auto added = result.adds.begin(); added != result.adds.end();) {
      std::optional<ShowRateRequest> rate;
      if (added->kind == AddKind::Media && added->count > 1U &&
          added->frame_rate.has_value()) {
        rate = matching_common_rate(*added->frame_rate);
      }
      if (rate.has_value() && *rate != timeline_rate &&
          (!wanted.has_value() || *wanted == *rate)) {
        wanted = rate;
        held.push_back(std::move(*added));
        added = result.adds.erase(added);
      } else {
        ++added;
      }
    }
    if (wanted.has_value()) {
      impl.pending_rate_match = EditorShell::Impl::PendingRateMatch{
          std::move(held), *wanted, timeline_rate, false, 0U};
      ui.open_context_menu(rate_menu, ui.pointer_x(), ui.pointer_y());
    }
  }
  if (impl.pending_rate_match.has_value()) {
    auto &pending = *impl.pending_rate_match;
    bool release = false;
    if (!pending.changing) {
      const std::string change =
          "Change the timeline to " + std::string{rate_name(pending.rate)};
      const std::string keep =
          "Keep the timeline at " + std::string{rate_name(pending.previous)};
      const std::array<std::string_view, 2U> choices{change, keep};
      if (const auto chosen = ui.context_menu(rate_menu, choices)) {
        if (*chosen == 0U) {
          if (!result.show_rate.has_value()) {
            result.show_rate = pending.rate;
          }
          pending.changing = true;
        } else {
          release = true;
        }
      } else if (!ui.menu_open(rate_menu)) {
        // Dismissed: the clips still go in, at the timeline's own rate.
        release = true;
      }
    } else {
      ++pending.frames_waited;
      release = timeline_rate == pending.rate || pending.frames_waited > 120U;
      if (release && timeline_rate == pending.rate) {
        // Lengths were measured against the old rate; remeasure them.
        for (AddIntent &add : pending.adds) {
          if (add.timing.has_value()) {
            const core::i64 numerator =
                add.timing->duration *
                static_cast<core::i64>(pending.rate.numerator) *
                static_cast<core::i64>(pending.previous.denominator);
            const core::i64 denominator =
                static_cast<core::i64>(pending.rate.denominator) *
                static_cast<core::i64>(pending.previous.numerator);
            add.timing->duration = std::max<core::i64>(
                (numerator + denominator - 1) / denominator, 1);
          }
        }
      }
    }
    if (release) {
      for (AddIntent &add : pending.adds) {
        result.adds.push_back(std::move(add));
      }
      impl.history_context = HistoryContext::Scene;
      impl.semantic_boundary = true;
      impl.pending_rate_match.reset();
    }
  }

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
  if (global_edit_shortcut_allowed && !result.save_project.has_value() &&
      ui.key_pressed(gui::Key::Save)) {
    save_project();
  }
  if (global_edit_shortcut_allowed && !delete_consumed &&
      ui.key_pressed(gui::Key::Delete)) {
    delete_active_domain();
  }

  // The History strip: fixed chrome under the docked panels rather than a
  // panel of its own, put away by its chevron and brought back from the
  // status bar.
  constexpr float history_reveal_width = 96.0F;
  if (!impl.history_hidden) {
    // A jump or step edits whatever is on the scene being edited, so it is
    // on air exactly when that scene is Program.
    const bool on_air = authored_stage.id == switcher.program_scene;
    HistoryPanelFrame history = impl.history_panel.draw(
        ui, editor_history_strip(width, height), history_state,
        impl.history_context, on_air);
    impl.history_hover = std::move(history.hovered_nodes);
    if (history.request.has_value() && !impl.manipulating &&
        !result.history.has_value()) {
      HistoryRequest request = *history.request;
      request.transaction = impl.next_edit_gesture;
      impl.next_edit_gesture += request.transaction_count;
      result.history = request;
    }
    impl.history_hidden = history.hide;
  }
  const bool history_was_hidden = impl.history_hidden;

  if (draw_status_bar(
          ui, width, height, timeline, switcher, impl.scene_selection.size(),
          scene.canvas, project, impl.pending_media_probes.size(),
          !bake_activity.empty() ? std::string_view{bake_activity}
          : impl.pending_model_ingests.empty()
              ? std::string_view{impl.ingest_message}
              : render::ModelIngestJob::label(
                    impl.pending_model_ingests.front().job->stage()),
          impl.ui_zoom_percent,
          history_was_hidden ? history_reveal_width + 6.0F : 0.0F)) {
    impl.open_menu = ApplicationMenu::View;
  }
  // Over the status bar, in the room it left at its left end.
  if (history_was_hidden &&
      history_toggle(ui, gui::ui_id("editor.status.history"),
                     gui::Rect{8.0F, height - status_bar_height + 3.0F,
                               history_reveal_width, status_bar_height - 6.0F},
                     false, "History")) {
    impl.history_hidden = false;
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
    ui.outline(card, ui.theme().accent);
    ui.push_clip(card);
    const gui::Rect blender_title{card.x+16,card.y+12,card.width-32,26};
    ui.label(blender_title,"CONNECT TO BLENDER",ui.theme().text);
    ui.tooltip(blender_title,"Link a Blender that is already open to the Preview scene. The B before a scene's name opens the project's own file.");
    if (project.blender_instances.empty()) {
      const gui::Rect looking{card.x+16,card.y+100,card.width-32,24};
      ui.label(looking,"Looking for Blender...",ui.theme().text);
      ui.tooltip(looking,"Open Blender and enable the Pixelsoup Live Link add-on; it appears here automatically");
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
        : project.blender_instance.empty() ? "No instance selected" : "Waiting for Blender to become available...";
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
    ui.outline(card, ui.theme().accent);
    ui.push_clip(card);
    ui.label({card.x+16, card.y+10, card.width-32, 24}, "FREEZE BLENDER - optional: lock a copy that Blender edits cannot change", ui.theme().text);
    const std::string details{project.scene_link_details.empty()
        ? "Linked Blender scenes go on air live. Freeze Blender is only for a Program that must not change." : project.scene_link_details};
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
    if (chrome_button(ui,gui::ui_id("editor.publication.publish"),{card.x+16,card.y+546,156,30},"Freeze Blender",false,
          !project.publication_scheduled && switcher.rehearsal_scene != scene::SceneId{})) result.publish_linked_scene=true;
    if (chrome_button(ui,gui::ui_id("editor.publication.cancel"),{card.x+188,card.y+546,156,30},"Cancel freeze",false,
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
    if (preferences_frame.browse_proxy_cache.has_value()) {
      open_file_dialog(
          [start = *preferences_frame.browse_proxy_cache]()
              -> std::optional<std::string> {
            const char *const selected = tinyfd_selectFolderDialog(
                "Select Proxy Cache Folder", start.c_str());
            return selected != nullptr ? std::optional<std::string>{selected}
                                       : std::nullopt;
          },
          [](Frame &frame, std::string folder) {
            frame.impl.preferences_panel.set_proxy_cache_directory(
                std::move(folder));
          });
    }
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
    settings_view.tracking_sources.assign(tracking_sources.begin(),
                                          tracking_sources.end());
    settings_view.feeds = live_data.feeds;
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
    if (!settings_frame.feed_edits.empty()) {
      // One gesture with the settings above when both changed, so Apply is
      // one undo step whatever it touched.
      const core::u64 gesture = result.project_settings.has_value()
          ? result.project_settings->gesture
          : impl.next_edit_gesture++;
      for (const ProjectFeedEdit &edit : settings_frame.feed_edits) {
        result.generic_edits.push_back(
            GenericEdit{edit.node, edit.property, edit.value, gesture,
                        edit.text});
      }
    }
    if (settings_frame.add_tracking_source) {
      result.adds.push_back(AddIntent{AddKind::TrackingSource,
                                      std::string{},
                                      1U,
                                      impl.next_edit_gesture++,
                                      16.0F / 9.0F,
                                      {},
                                      authored_stage.id});
    }
    if (modal_was_open && !impl.project_settings_panel.is_open()) {
      ui.clear_focus();
    }
  }

  // A keyboard transform is one gesture from G to its confirm, though no
  // button is held through it.
  if (!ui.pointer_down() && !ui.typing() &&
      !impl.stage().panel.transforming()) {
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
      !result.scene_duplicates.empty() ||
      result.key_batch.has_value() ||
      !result.clip_timings.empty() ||
      !result.clip_splits.empty() || !result.lane_edits.empty() ||
      !result.plateset_places.empty() || !result.plateset_swaps.empty() ||
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

  // Where the laser pointer points now, and the picture the wall shows.
  // Scene marks reach the wall through Program's camera, the one the wall
  // is drawn from.
  impl.laser.point_at(laser_pointed);
  if (laser_on_wall_due || impl.laser_on_wall) {
    result.pointer = laser_on_wall_due
                         ? wall_pointer_overlay(impl.laser, scene.camera,
                                                scene.canvas)
                         : show::PointerOverlay{};
    impl.laser_on_wall = laser_on_wall_due;
  }

  result.program_media = impl.program_panel.drawn_media();
  result.stage_media = std::any_of(
                           impl.stage_views.begin(), impl.stage_views.end(),
                           [](const auto &view) {
                             return view.panel.drawn_media() ==
                                    MediaRendition::Full;
                           })
                           ? MediaRendition::Full
                           : MediaRendition::Proxy;

  // A drag can update a seam or floating rectangle every display frame. Wait
  // for release, then emit only the final arrangement so persistence never
  // turns a pointer gesture into a stream of disk writes.
  if (!ui.pointer_down()) {
    auto encoded = encode_workspace_layout(WorkspaceLayoutState{
        impl.docks.snapshot(), impl.workspace_window_width,
        impl.workspace_window_height,
        impl.stage_views[0U].panel.resolution(),
        impl.program_panel.resolution(), impl.ui_zoom_percent,
        std::min(impl.compositor_panel.parameter_panel_width(),
                 max_compositor_parameter_width),
        impl.compositor_panel.minimap_shown(), impl.night,
        impl.history_hidden, impl.stage_views[0U].panel.media(),
        impl.program_panel.media()});
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
                    (stage_viewport_drawn && impl.stage().panel.transforming()));
  impl.window.set_pointer_locked(navigating && impl.navigation_moved);

  // What is being dragged out of the Library or the Asset Viewer rides above
  // every panel.
  impl.library_panel.draw_drag_ghost(ui);
  impl.asset_viewer_panel.draw_drag_ghost(ui, impl.viewer.texture);
  impl.viewer.drawn = viewer_drawn_now;
  if (viewer_drawn_now && impl.asset_viewer_panel.asset() != doc::NodeId{}) {
    const assets::Asset *const viewed =
        asset_registry.asset(impl.asset_viewer_panel.asset());
    const auto *const media =
        viewed != nullptr ? std::get_if<assets::MediaAsset>(viewed) : nullptr;
    const assets::DerivativeProfile *const profile = assets::derivative_profile(
        assets::DerivativeProfileId::Filmstrip8x160x90Srgb);
    if (media != nullptr && media->fingerprint.has_value() &&
        profile != nullptr) {
      const assets::DerivativeKey key =
          assets::make_derivative_key(media->id, *media->fingerprint, *profile);
      if (!asset_viewer.filmstrip.has_value() ||
          asset_viewer.filmstrip->key != key ||
          asset_viewer.filmstrip->state != assets::DerivativeState::Ready) {
        result.filmstrip_requests.push_back(TimelineFilmstripRequestIntent{
            key, TimelineFilmstripRequestPriority::Visible});
      }
      if (!result.filmstrip_retain.has_value()) {
        result.filmstrip_retain = TimelineFilmstripRetainIntent{};
      }
      auto &keys = result.filmstrip_retain->keys;
      if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
        keys.push_back(key);
      }
    }
  }
  ui.draw_overlays(gui::Rect{0.0F, 0.0F, width, height});
  impl.window.set_cursor(ui.cursor());
  static_cast<void>(impl.window.present(
      impl.draw_list, rhi::Color{0.043F, 0.047F, 0.059F, 1.0F}));
}

void EditorShell::Frame::delete_active_domain() {
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
}

} // namespace mediaserver::editor
