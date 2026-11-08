// The Stage and Program viewports.

#include "shell_frame.hpp"

namespace mediaserver::editor {

void EditorShell::Frame::draw_stage_panel(const gui::Rect &bounds) {
  // Turns four views on or off. The first time, the other three look at
  // what the first does, from as far away.
  const auto set_four_views = [&](bool four) {
    impl.stage_four = four;
    impl.stage_four_failed = false;
    if (!four) {
      return;
    }
    for (std::size_t index = 1U; index < impl.stage_views.size();
         ++index) {
      Impl::StageViewport &other = impl.stage_views[index];
      if (!impl.stage_four_shown) {
        other.orbit.target = impl.stage_views[0U].orbit.target;
        other.orbit.distance = impl.stage_views[0U].orbit.distance;
        other.panel.share_surfaces_with(impl.stage_views[0U].panel);
      }
      other.panel.copy_tools_from(impl.stage_views[0U].panel);
    }
    impl.stage_four_shown = true;
  };
  // The scenes the panels and gestures read act for one view at a time:
  // its own camera, over its own canvas.
  const auto act_for = [&](std::size_t index) {
    impl.stage_current = index;
    const Impl::StageViewport &view = impl.stage_views[index];
    stage_scene.camera = stage_view_camera(view);
    stage_scene.canvas = stage_view_canvas(view);
    stage_interaction_scene.camera = stage_scene.camera;
    stage_interaction_scene.canvas = stage_scene.canvas;
  };
  // One Stage view drawn into `area`, with the toolbar across its top
  // when it stands alone.
  const auto draw_stage_view = [&](const gui::Rect &area, bool toolbar) {
    Impl::StageViewport &view = impl.stage();
    // Rendered at the pane's physical size, so a dense display gets a
    // sharp Stage rather than a magnified one.
    const float pixels = impl.window.ui_scale();
    view.surface_width =
        static_cast<core::u32>(std::max(area.width * pixels, 1.0F));
    view.surface_height = static_cast<core::u32>(std::max(
        (area.height - (toolbar ? 29.0F : 0.0F)) * pixels, 1.0F));

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
      const MaskViewportFrame mask_frame = view.panel.draw_masks(
          ui, area, *view.texture, stage_scene,
          mask_viewport_input(stage_scene, nullptr), &scene.camera,
          static_cast<float>(timeline.rate_numerator) /
              static_cast<float>(
                  std::max(timeline.rate_denominator, 1U)));
      view.frame = ViewportFrame{};
      view.frame.image = mask_frame.image;
      apply_mask_viewport_frame(mask_frame);
    } else {
      const gui::Rect *const interaction_block =
          color_sample_active
              ? &area
              : (floating_occluder.has_value()
                     ? &floating_occluder.value()
                     : nullptr);
      view.panel.set_stage_projection(view.orbit.projection);
      view.panel.set_stage_shading(view.shading);
      view.panel.set_stage_four_views(impl.stage_four);
      view.panel.set_toolbar_shown(toolbar);
      view.panel.set_highlight(impl.stage_history_highlight);
      view.frame = view.panel.draw(
          ui, area, *view.texture, stage_interaction_scene,
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
    if (view.frame.projection_choice.has_value()) {
      view.orbit.projection = *view.frame.projection_choice;
    }
    if (view.frame.layout_choice.has_value()) {
      set_four_views(*view.frame.layout_choice);
    }
    if (view.frame.shading_choice.has_value()) {
      choose_shading(*view.frame.shading_choice);
    }
    if (!impl.stage_bookmark_note.empty() &&
        impl.stage_bookmark_view == impl.stage_current &&
        impl.window.time_seconds() < impl.stage_bookmark_note_until) {
      gui::Rect head = gui::inset(view.frame.image,
                                  gui::Padding{0.0F, 10.0F, 0.0F, 0.0F});
      head = gui::cut_top(head, 24.0F);
      const gui::Rect note = gui::centered(
          head, ui.font_width(impl.stage_bookmark_note) + 20.0F,
          head.height);
      ui.panel(note, with_alpha(ui.theme().background, 0.86F));
      ui.label(gui::inset(note, gui::Padding{10.0F, 0.0F, 0.0F, 0.0F}),
               impl.stage_bookmark_note, ui.theme().text);
    }
    // While a Timeline edit is dragged the picture is its edit point, not
    // the playhead's frame; the caption says which, in the playhead's
    // colour, along the foot where the bookmark note never sits.
    if (stage_view_scene.has_value() &&
        !switcher.stage_view_caption.empty()) {
      const gui::Rect &image = view.frame.image;
      gui::Rect foot =
          gui::inset(image, gui::Padding{8.0F, 0.0F, 8.0F, 10.0F});
      foot = gui::cut_bottom(foot, 24.0F);
      const gui::Rect note = gui::centered(
          foot,
          std::min(ui.font_width(switcher.stage_view_caption) + 24.0F,
                   foot.width),
          foot.height);
      ui.push_clip(image);
      ui.panel(note, with_alpha(ui.theme().background, 0.86F));
      gui::Rect caption = note;
      ui.panel(gui::cut_left(caption, 3.0F), ui.theme().playhead);
      ui.label(gui::inset(caption, gui::Padding{9.0F, 0.0F, 2.0F, 0.0F}),
               switcher.stage_view_caption, ui.theme().playhead);
      ui.pop_clip();
    }
    // A passive notice leaves picking, framing and navigation available.
    // Retain the last submitted state when command acquisition is busy.
    if (!view.assets.headline.empty()) {
      const auto& image = view.frame.image;
      ui.push_clip(image);
      draw_status_banner(
          ui, image, 520.0F, 54.0F,
          view.assets.loading ? ui.theme().accent : ui.theme().warning,
          view.assets.headline, {{view.assets.detail, ui.theme().text}});
      ui.pop_clip();
    }
    if (color_sample_active) {
      const bool pointer_available =
          !floating_occluder.has_value() ||
          !floating_occluder->contains(ui.pointer_x(), ui.pointer_y());
      run_color_sampler(gui::ui_id("editor.eyedropper.stage"),
                        view.frame.image,
                        *view.texture, pointer_available);
    }
    if (!color_sample_active && !switcher.rehearsal_locked) {
      if (view.frame.surface_edit.has_value()) {
        const output::ProjectionSurface &surface =
            *view.frame.surface_edit;
        impl.stage_surface_selection = surface.node;
        impl.feed_mapping_panel.select_surface(surface.node);
        impl.feed_mapping_panel.preview_surface_geometry(surface.node,
                                                         surface.geometry);
        FeedSurfaceTransform changed = FeedSurfaceTransform::All;
        switch (view.frame.gizmo_axis) {
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
        handle_viewport(view.frame, stage_scene,
                        stage_interaction_scene);
        const bool over_stage =
            !floating_occluder.has_value() &&
            view.frame.image.contains(ui.pointer_x(),
                                                ui.pointer_y());
        if (over_stage && !ui.typing() &&
            !view.panel.transforming() &&
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
      draw_panel_warning(ui, area, 38.0F, 312.0F, true,
                         "TRANSITION ACTIVE - REHEARSAL LOCKED");
    }
    // The camera is the operator's own view, so it navigates through a
    // rehearsal lock and while masks are authored, though a mask tool keeps
    // the left button.
    if (color_sample_active || switcher.rehearsal_locked) {
      // Nothing may author into the Stage now; a keyboard transform must
      // not keep holding the keyboard waiting for a confirm.
      view.panel.cancel_transform();
    }
    if (color_sample_active) {
      impl.navigate_left = false;
      impl.navigate_middle = false;
      impl.navigate_right = false;
    } else {
      // A keyboard transform owns the pointer, its right-click cancel and
      // the wheel included.
      navigate_stage_camera(view.frame,
                            !floating_occluder.has_value() &&
                                !view.frame.keyboard_transform,
                            !mask_stage_active);
    }
  };

  // Measured once a frame, for whichever view a held fly belongs to.
  const double now = impl.window.time_seconds();
  impl.navigation_seconds = std::clamp(
      static_cast<float>(now - impl.navigation_time), 0.0F, 0.1F);
  impl.navigation_time = now;
  const bool four = stage_view_count == impl.stage_views.size() &&
                    !mask_stage_active && !color_sample_active;
  impl.stage_single_only = mask_stage_active || color_sample_active;
  if (!four) {
    // A gesture under way in a view that is no longer shown carries on
    // in the one that is.
    impl.navigation_view = 0U;
    act_for(0U);
    draw_stage_view(bounds, true);
  } else {
    // One toolbar over Top and Front (top row), Side and the first view
    // (bottom row). The first view's panel draws it and owns its
    // settings; each view takes them before it draws and hands back any
    // it changed (a key, a click) after.
    constexpr float toolbar_height = 29.0F;
    constexpr float gap = 2.0F;
    impl.stage_views[0U].panel.draw_toolbar(
        ui, gui::Rect{bounds.x, bounds.y, bounds.width, toolbar_height},
        static_cast<float>(timeline.rate_numerator) /
            static_cast<float>(std::max(timeline.rate_denominator, 1U)));
    const float top = bounds.y + toolbar_height;
    const float half_width = std::max((bounds.width - gap) * 0.5F, 1.0F);
    const float half_height =
        std::max((bounds.height - toolbar_height - gap) * 0.5F, 1.0F);
    const std::array<std::pair<std::size_t, gui::Rect>, 4U> quadrants{{
        {1U, gui::Rect{bounds.x, top, half_width, half_height}},
        {2U, gui::Rect{bounds.x + half_width + gap, top, half_width,
                       half_height}},
        {3U, gui::Rect{bounds.x, top + half_height + gap, half_width,
                       half_height}},
        {0U, gui::Rect{bounds.x + half_width + gap,
                       top + half_height + gap, half_width,
                       half_height}},
    }};
    ui.panel(gui::Rect{bounds.x, top, bounds.width,
                       bounds.height - toolbar_height},
             ui.theme().divider);
    for (const auto &[index, area] : quadrants) {
      act_for(index);
      ViewportPanel &panel_of_view = impl.stage_views[index].panel;
      if (index != 0U) {
        panel_of_view.copy_tools_from(impl.stage_views[0U].panel);
      }
      draw_stage_view(area, false);
      if (index != 0U) {
        impl.stage_views[0U].panel.copy_tools_from(panel_of_view);
      }
      if (!impl.stage_four) {
        break; // One view chosen from this view's menu.
      }
    }
  }
  // What follows the Stage this frame - a drop, its preview, a keyboard
  // transform's hold on the keys - acts for the view with the gesture,
  // else the one under the pointer.
  std::size_t focus = impl.stage_current;
  if (four) {
    focus = 0U;
    bool chosen = false;
    if (impl.navigate_left || impl.navigate_middle ||
        impl.navigate_right) {
      focus = impl.navigation_view;
      chosen = true;
    }
    for (std::size_t index = 0U; !chosen && index < impl.stage_views.size();
         ++index) {
      const Impl::StageViewport &view = impl.stage_views[index];
      if (view.panel.transforming() || view.panel.dragging() ||
          view.frame.held) {
        focus = index;
        chosen = true;
      }
    }
    for (std::size_t index = 0U; !chosen && index < impl.stage_views.size();
         ++index) {
      if (impl.stage_views[index].frame.image.contains(ui.pointer_x(),
                                                       ui.pointer_y())) {
        focus = index;
        chosen = true;
      }
    }
  }
  act_for(focus);
  stage_viewport_frame = impl.stage().frame;
  return;
}

void EditorShell::Frame::draw_program_panel(const gui::Rect &bounds) {
  ui.panel(bounds, ui.theme().background);

  const gui::Rect output_bar{bounds.x, bounds.y, bounds.width, 26.0F};
  ui.panel(output_bar, ui.theme().header);
  ui.panel(gui::Rect{output_bar.x, output_bar.bottom() - 1.0F,
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
  const float media_width = impl.program_panel.media_control_width(ui);
  const float output_name_width = std::min(
      190.0F,
      std::max(output_bar.width - 174.0F - media_width, 60.0F));
  const gui::Rect output_name{output_bar.x + 6.0F, output_bar.y + 3.0F,
                              output_name_width, output_bar.height - 6.0F};
  // The preview resolution, as on the Stage toolbar: a screen and the
  // current choice.
  const float resolution_width =
      impl.program_panel.resolution_control_width(ui);
  const float resolution_x =
      output_bar.x +
      std::max(output_bar.width - resolution_width - 6.0F, 64.0F);
  impl.program_panel.draw_resolution_control(
      ui, gui::Rect{resolution_x, output_bar.y + 3.0F, resolution_width,
                    20.0F});
  if (resolution_x - media_width - 4.0F >= output_name.right() + 4.0F) {
    impl.program_panel.draw_media_control(
        ui, gui::Rect{resolution_x - media_width - 4.0F,
                      output_bar.y + 3.0F, media_width, 20.0F});
  }
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
  if (chrome_button(ui, gui::ui_id(gui::ui_id("editor.program"), "output"),
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
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - impl.program_wait_started).count();
    const auto elapsed = "Waiting " + std::to_string(seconds) + "s - " +
        (impl.assets.visible_commit != 0 ? "waiting for output confirmation" :
         impl.asset_gate.visible_commit() != 0 ? "showing the previous Program image" : "preparing the first Program image");
    draw_status_banner(ui, content, content.width, 76.0F,
                       ui.theme().warning, program_assets.headline,
                       {{program_assets.detail, ui.theme().text},
                        {elapsed, ui.theme().text_muted}});
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
      ui, footer.right() - state_width - corner,
      footer.y + footer.height * 0.5F, state_text, state_color,
      program_preview_ok && !program_preview_held &&
          !program_assets.waiting && timeline.playing));
  return;
}

} // namespace mediaserver::editor
