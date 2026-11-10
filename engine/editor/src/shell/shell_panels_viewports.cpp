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
  const float media_x = resolution_x - media_width - 4.0F;
  if (media_x >= output_name.right() + 4.0F) {
    impl.program_panel.draw_media_control(
        ui, gui::Rect{media_x, output_bar.y + 3.0F, media_width, 20.0F});
  }
  // How large the picture is drawn: fitted to the panel, or a share of the
  // output's own pixels. The menu sets it; the wheel and the Zoom tool move
  // it from there.
  constexpr float zoom_width = 58.0F;
  const gui::Rect zoom_control{media_x - zoom_width - 4.0F,
                               output_bar.y + 3.0F, zoom_width, 20.0F};
  if (zoom_control.x >= output_name.right() + 4.0F) {
    const float pixels = impl.window.ui_scale();
    std::array<char, 16U> zoom_text{};
    if (impl.program_panel.canvas_fitted()) {
      static_cast<void>(
          std::snprintf(zoom_text.data(), zoom_text.size(), "Fit"));
    } else {
      static_cast<void>(std::snprintf(
          zoom_text.data(), zoom_text.size(), "%.0f%%",
          static_cast<double>(impl.program_panel.canvas_scale() * pixels *
                              100.0F)));
    }
    const gui::UiId zoom_menu =
        gui::ui_id(gui::ui_id("editor.program"), "zoom.menu");
    ui.tooltip(zoom_control,
               "How large the picture is drawn. The wheel zooms on the "
               "pointer; F fits it to the panel.");
    if (chrome_button(ui, gui::ui_id(gui::ui_id("editor.program"), "zoom"),
                      zoom_control, zoom_text.data(),
                      ui.menu_open(zoom_menu))) {
      ui.open_context_menu(zoom_menu, zoom_control.x, zoom_control.bottom());
    }
    constexpr std::array<std::string_view, 5U> zoom_options{
        "Fit            F", "50%", "100%", "200%", "400%"};
    constexpr std::array<float, 5U> zoom_scales{0.0F, 0.5F, 1.0F, 2.0F, 4.0F};
    if (const auto chosen = ui.context_menu(zoom_menu, zoom_options)) {
      if (*chosen == 0U) {
        impl.program_panel.fit_canvas();
      } else if (*chosen < zoom_scales.size()) {
        impl.program_panel.set_canvas_scale(zoom_scales[*chosen] /
                                            std::max(pixels, 0.01F));
      }
    }
  }
  std::array<char, 64U> output_identity{};
  if (impl.program_canvas_view) {
    static_cast<void>(std::snprintf(
        output_identity.data(), output_identity.size(),
        program_canvas_stage ? "Canvas  -  Preview" : "Canvas"));
  } else if (program_output != nullptr) {
    static_cast<void>(std::snprintf(
        output_identity.data(), output_identity.size(),
        "Output %02u  -  node %u", program_output->node_index + 1U,
        program_output->node_index));
  } else {
    static_cast<void>(std::snprintf(output_identity.data(),
                                    output_identity.size(),
                                    "No output map"));
  }
  // The views go round: each output's connector image, then the whole
  // canvas laid flat, which is the picture layers are moved about on.
  ui.tooltip(output_name,
             impl.program_canvas_view
                 ? "The whole canvas, flat. Click for the first output."
                 : "What this view shows. Click for the next output, then "
                   "the whole canvas laid flat.");
  if (chrome_button(ui, gui::ui_id(gui::ui_id("editor.program"), "output"),
                    output_name, output_identity.data(),
                    impl.program_canvas_view)) {
    const auto current =
        std::find_if(feed_map.maps.begin(), feed_map.maps.end(),
                     [&](const output::OutputMap &map) {
                       return program_output != nullptr &&
                              map.node == program_output->node;
                     });
    if (impl.program_canvas_view) {
      impl.program_canvas_view = false;
      if (!feed_map.maps.empty()) {
        impl.program_output_map_node = feed_map.maps.front().node;
      }
    } else if (current == feed_map.maps.end() ||
               std::next(current) == feed_map.maps.end()) {
      impl.program_canvas_view = true;
    } else {
      impl.program_output_map_node = std::next(current)->node;
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
  // Under the output bar, the selected layer's place on the canvas; down
  // the left edge, the tools.
  constexpr float options_height = 26.0F;
  const gui::Rect options_bar{
      bounds.x, output_bar.bottom(), bounds.width,
      std::clamp(footer.y - output_bar.bottom(), 0.0F, options_height)};
  const gui::Rect below_bars{
      bounds.x, options_bar.bottom(), bounds.width,
      std::max(footer.y - options_bar.bottom(), 0.0F)};
  const gui::Rect palette{
      below_bars.x, below_bars.y,
      std::min(canvas_tool_palette_width, below_bars.width),
      below_bars.height};
  const gui::Rect content{palette.right(), below_bars.y,
                          std::max(below_bars.width - palette.width, 0.0F),
                          below_bars.height};
  ui.panel(content, gui::UiColor{0.0F, 0.0F, 0.0F, 1.0F});
  if (const auto chosen = draw_canvas_tool_palette(
          ui, gui::ui_id("editor.program.tools"), palette,
          impl.program_panel.canvas_tool(), switcher.rehearsal_locked)) {
    impl.program_panel.set_canvas_tool(*chosen);
  }
  if (content.contains(ui.pointer_x(), ui.pointer_y()) && !ui.typing() &&
      !ui.ctrl_held() && !ui.alt_held() && !ui.pointer_down()) {
    const std::string_view typed = ui.shortcut_text();
    if (const auto tool = canvas_tool_for_shortcut(typed);
        tool.has_value() &&
        !(switcher.rehearsal_locked && canvas_tool(*tool).authors)) {
      impl.program_panel.set_canvas_tool(*tool);
    }
    if (typed.find_first_of("fF") != std::string_view::npos) {
      impl.program_panel.fit_canvas();
    }
  }
  // A view that may not author keeps the tools that only move it.
  if (switcher.rehearsal_locked &&
      canvas_tool(impl.program_panel.canvas_tool()).authors) {
    impl.program_panel.cancel_shape();
    impl.program_panel.set_canvas_tool(CanvasTool::Hand);
  }
  impl.program_panel.set_box_enabled(!switcher.rehearsal_locked &&
                                     !color_sample_active);
  // A new card is placed by where its corners are in the scene, which a
  // crop of the canvas still says and a warped surface's pixels do not.
  impl.program_panel.set_card_drawing_enabled(
      program_canvas_stage || program_output == nullptr ||
      program_output->projection == output::FeedProjection::CanvasSlice);
  // What the panel shows and authors into: Program's scene through the
  // output's projection, or - in the Canvas view while the Stage is on a
  // Preview scene - that scene through its own camera. The Stage's scenes
  // carry whichever Stage camera drew last, so they borrow the authored one
  // for as long as this panel reads them.
  const ViewportProjection *program_projection =
      program_view_projection.has_value()
          ? &program_view_projection.value()
          : nullptr;
  const scene::Scene *shown_authored = &scene;
  const scene::Scene *shown_masks = &scene;
  const scene::Scene *shown_view = program_interaction_scene.has_value()
                                       ? &*program_interaction_scene
                                       : &scene;
  const rhi::Texture *shown_texture = program_texture;
  bool picture_ok = program_preview_ok;
  struct BorrowedCamera final {
    scene::Scene *first{};
    scene::Scene *second{};
    scene::Camera first_camera{};
    scene::Canvas first_canvas{};
    scene::Camera second_camera{};
    scene::Canvas second_canvas{};
    ~BorrowedCamera() {
      if (first != nullptr) {
        first->camera = first_camera;
        first->canvas = first_canvas;
        second->camera = second_camera;
        second->canvas = second_canvas;
      }
    }
  } borrowed;
  if (program_canvas_stage) {
    borrowed.first = &stage_scene;
    borrowed.second = &stage_interaction_scene;
    borrowed.first_camera = stage_scene.camera;
    borrowed.first_canvas = stage_scene.canvas;
    borrowed.second_camera = stage_interaction_scene.camera;
    borrowed.second_canvas = stage_interaction_scene.canvas;
    stage_scene.camera = authored_stage.camera;
    stage_scene.canvas = authored_stage.canvas;
    stage_interaction_scene.camera = authored_stage.camera;
    stage_interaction_scene.canvas = authored_stage.canvas;
    program_projection = nullptr;
    shown_authored = &stage_scene;
    shown_masks = &stage_scene;
    shown_view = &stage_interaction_scene;
    shown_texture = canvas_stage_texture;
    picture_ok = canvas_stage_texture != nullptr;
  }
  draw_program_options(options_bar, *shown_authored, *shown_view,
                       program_projection, picture_ok);
  gui::Rect program_viewport_image{};
  if (picture_ok && mask_program_active && !color_sample_active) {
    // The overlay is measured against the connector image, so a matte the
    // operator is looking at on the wall is the matte their pointer moves.
    const MaskViewportFrame mask_frame = impl.program_panel.draw_masks(
        ui, content, *shown_texture, *shown_masks,
        mask_viewport_input(*shown_masks, program_projection), nullptr,
        static_cast<float>(timeline.rate_numerator) /
            static_cast<float>(std::max(timeline.rate_denominator, 1U)),
        program_projection);
    program_viewport_image = mask_frame.image;
    apply_mask_viewport_frame(mask_frame);
    // A click away from the mask, or Esc with nothing held, goes back to
    // the layer it belongs to.
    const bool escaped =
        !mask_frame.held && !ui.typing() &&
        content.contains(ui.pointer_x(), ui.pointer_y()) &&
        ui.key_pressed(gui::Key::Escape);
    if (mask_frame.pressed_away || escaped) {
      impl.mask_selection.mask = {};
      impl.mask_selection.point.reset();
      impl.semantic_boundary = true;
    }
  } else if (picture_ok) {
    const gui::Rect *const interaction_block =
        color_sample_active ? &content : nullptr;
    const ViewportFrame program_viewport = impl.program_panel.draw(
        ui, content, *shown_texture, *shown_view,
        impl.scene_selection.members(), impl.anchor(), nullptr,
        interaction_block,
        static_cast<float>(timeline.rate_numerator) /
            static_cast<float>(std::max(timeline.rate_denominator, 1U)),
        nullptr, doc::NodeId{}, program_projection, plate_overlay,
        elements);
    program_viewport_image = program_viewport.image;
    if (!color_sample_active && !switcher.rehearsal_locked) {
      handle_viewport(program_viewport, *shown_authored, *shown_view);
      if (program_viewport.shape.has_value()) {
        author_canvas_shape(*program_viewport.shape, *shown_view,
                            program_projection);
      }
      // The Gradient tool's drag: each value of the fill that changed, as
      // Properties would write it, the whole drag one undo step.
      if (program_viewport.fill.has_value()) {
        const doc::NodeId node = impl.anchor();
        const ElementInfo *const element = find_element(elements, node);
        const scene::Fill *before{};
        for (const scene::MeshInstance &instance : shown_view->instances) {
          if (instance.source == node && instance.material.fill.has_value()) {
            before = &*instance.material.fill;
            break;
          }
        }
        if (before != nullptr && element != nullptr && !element->locked) {
          const scene::Fill &after = *program_viewport.fill;
          const core::u64 gesture = impl.begin_gesture();
          const std::array<std::tuple<std::string_view, float, float>, 4U>
              values{{{scene::schema::fill_center_x, before->center_x,
                       after.center_x},
                      {scene::schema::fill_center_y, before->center_y,
                       after.center_y},
                      {scene::schema::fill_angle_turns, before->angle,
                       after.angle},
                      {scene::schema::fill_size, before->size, after.size}}};
          for (const auto &[property, was, now] : values) {
            if (was != now) {
              result.generic_edits.push_back(
                  GenericEdit{node, std::string{property},
                              static_cast<double>(now), gesture});
            }
          }
          impl.history_context = HistoryContext::Scene;
        }
      }
    }
  } else {
    const std::string_view fault =
        program_canvas_stage
            ? std::string_view{"Preparing the Preview canvas"}
            : (program_mapping_unavailable
                   ? std::string_view{"OUTPUT MAPPING UNAVAILABLE"}
                   : program_preview_fault);
    const float label_width = ui.font_width(fault);
    ui.label(gui::Rect{content.x +
                           std::max((content.width - label_width) * 0.5F,
                                    8.0F),
                       content.y + std::max(content.height * 0.5F - 10.0F,
                                            0.0F),
                       label_width, 20.0F},
             fault, ui.theme().warning);
  }
  if (picture_ok && color_sample_active) {
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
                      program_viewport_image, *shown_texture,
                      pointer_available);
  }

  ui.panel(footer, ui.theme().header);
  // Every asset change - a live Blender mesh edit, a new clip - goes
  // through a second or so of loading and a synchronized switch. The
  // footer names that hand-over; the banner over the picture is kept for
  // a wait that has become the operator's problem.
  constexpr auto routine_wait = std::chrono::seconds{3};
  // Program's own wait is not this picture's while the Canvas view shows a
  // Preview scene; the show bar still carries it.
  const bool program_wait_banner =
      !program_canvas_stage && program_assets.waiting &&
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
      program_canvas_stage
          ? std::snprintf(preview_status.data(), preview_status.size(),
                          "%u x %u   The scene in Preview, not on air",
                          program_extent.output_width,
                          program_extent.output_height)
      : preview_differs
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
      program_canvas_stage ? "Preview"
      : !program_preview_ok ? "FAULT"
      : program_assets.waiting && !program_preview_held ? "WAIT"
      : program_preview_held ? "HOLD"
      : (timeline.playing ? "PLAYING" : "PAUSED");
  const gui::UiColor state_color =
      program_canvas_stage ? ui.theme().preview
      : !program_preview_ok || program_preview_held || program_assets.waiting
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
      !program_canvas_stage && program_preview_ok && !program_preview_held &&
          !program_assets.waiting && timeline.playing));
  return;
}

// The strip under Program's output bar: where the selected layer is on the
// picture, how large, and how far turned, in the pixels of the view shown.
// The same numbers the box's handles change, for when one has to be exact.
void EditorShell::Frame::draw_program_options(
    const gui::Rect &bar, const scene::Scene &authored,
    const scene::Scene &view, const ViewportProjection *projection,
    bool picture_ok) {
  const gui::Theme &theme = ui.theme();
  ui.panel(bar, theme.header);
  if (bar.height < 24.0F) {
    return;
  }
  ui.panel(gui::Rect{bar.x, bar.bottom() - 1.0F, bar.width, 1.0F},
           theme.divider);
  const CanvasTool tool = impl.program_panel.canvas_tool();
  const doc::NodeId node = impl.anchor();
  const ElementInfo *const element = find_element(elements, node);
  const std::optional<CanvasBoxMetrics> metrics =
      picture_ok && tool == CanvasTool::Select && element != nullptr
          ? impl.program_panel.canvas_box_metrics(view, node, projection)
          : std::nullopt;
  gui::Rect row = gui::inset(bar, gui::Padding{8.0F, 3.0F, 8.0F, 4.0F});
  // At the right end, whatever is selected: lining layers up, and what the
  // picture is laid out against.
  {
    const gui::UiId tools = gui::ui_id("editor.program.layout");
    const gui::Rect safe = gui::cut_right(row, 24.0F);
    if (icon_button(ui, gui::ui_id(tools, 1U), safe, gui::Icon::Guides,
                    "Safe areas: the action-safe and title-safe frames and "
                    "the middle of the picture",
                    impl.program_panel.safe_areas_shown())) {
      impl.program_panel.set_safe_areas_shown(
          !impl.program_panel.safe_areas_shown());
    }
    const gui::Rect rulers = gui::cut_right(row, 24.0F);
    if (icon_button(ui, gui::ui_id(tools, 2U), rulers, gui::Icon::Measure,
                    "Rulers and guides. Drag out of a ruler for a guide; "
                    "drag a guide back onto one to remove it",
                    impl.program_panel.rulers_shown())) {
      impl.program_panel.set_rulers_shown(!impl.program_panel.rulers_shown());
    }
    const gui::Rect snap = gui::cut_right(row, 24.0F);
    if (icon_button(ui, gui::ui_id(tools, 3U), snap, gui::Icon::Snap,
                    "Snap a moved layer to the picture's edges and middle, "
                    "to other layers and to guides. Hold Ctrl to move "
                    "without it",
                    impl.program_panel.canvas_snapping())) {
      impl.program_panel.set_canvas_snapping(
          !impl.program_panel.canvas_snapping());
    }
    static_cast<void>(gui::cut_right(row, 6.0F));
    const gui::Rect align = gui::cut_right(row, 56.0F);
    const bool can_align = picture_ok && !impl.scene_selection.empty() &&
                           !switcher.rehearsal_locked && !color_sample_active;
    const gui::UiId align_menu = gui::ui_id(tools, 5U);
    ui.tooltip(align,
               "Line the selected layers up with each other, or one layer "
               "with the picture; or space three or more evenly");
    if (chrome_button(ui, gui::ui_id(tools, 4U), align, "Align",
                      ui.menu_open(align_menu), can_align)) {
      ui.open_context_menu(align_menu, align.x, align.bottom());
    }
    constexpr std::array<std::string_view, 8U> align_options{
        "Align left",       "Align centre",     "Align right",
        "Align top",        "Align middle",     "Align bottom",
        "Distribute across", "Distribute down"};
    if (const auto chosen = ui.context_menu(align_menu, align_options)) {
      if (can_align && *chosen < align_options.size()) {
        align_selection(*chosen, authored, view, projection);
      }
    }
    static_cast<void>(gui::cut_right(row, 8.0F));
  }
  if (!metrics.has_value()) {
    std::string_view hint = "Select a layer to move, turn and resize it";
    if (tool == CanvasTool::Hand) {
      hint = "Drag to move the picture inside the panel";
    } else if (tool == CanvasTool::Zoom) {
      hint = "Click to zoom in, Alt+click to zoom out";
    } else if (tool == CanvasTool::Pen) {
      hint = "Click to place points, drag to curve them; click the first "
             "point or press Enter to close";
    } else if (draws_shape(tool)) {
      hint = "Drag on a selected layer to mask it, or on bare picture to "
             "make a new card";
    } else if (tool == CanvasTool::Gradient) {
      hint = "On a selected Background with a gradient: drag its middle, "
             "its end, or a new line across it";
    }
    ui.label(row, hint, theme.text_muted);
    return;
  }
  if (mask_program_active) {
    ui.label(row,
             "Editing a mask: drag it, its handles or its points. Click "
             "away or press Esc to go back to the layer",
             theme.text_muted);
    return;
  }
  const bool editable = !element->locked && !switcher.rehearsal_locked &&
                        !color_sample_active;
  const ElementInfo evaluated =
      evaluated_element(*element, keyframes, timeline.show_frame);
  const auto apply = [&](CanvasBoxField field, float value) {
    const auto delta = impl.program_panel.canvas_box_edit(view, node,
                                                          projection, field,
                                                          value);
    if (!delta.has_value()) {
      return;
    }
    impl.history_context = HistoryContext::Scene;
    capture_group(authored, view);
    if (!impl.group_nodes.empty()) {
      emit_transform(*delta, impl.begin_gesture());
    }
    impl.group_nodes.clear();
    impl.group_start.clear();
  };

  const gui::UiId base = gui::ui_id("editor.program.options");
  constexpr float field_width = 78.0F;
  constexpr float gap = 4.0F;
  constexpr float group_gap = 12.0F;
  float x = row.x;
  // One value: a field while it may be edited, the same number as plain
  // text while it may not, so a locked layer still reads.
  const auto value_field = [&](core::u64 salt, std::string_view letter,
                               std::string_view tip, CanvasBoxField field,
                               float value, double speed, int decimals,
                               float scale) {
    const gui::Rect cell{x, row.y, field_width, row.height};
    x += field_width + gap;
    if (cell.right() > row.right()) {
      return;
    }
    ui.tooltip(cell, tip);
    if (!editable) {
      std::array<char, 40U> text{};
      const int length = std::snprintf(
          text.data(), text.size(), "%.*s %.*f",
          static_cast<int>(letter.size()), letter.data(), decimals,
          static_cast<double>(value));
      ui.label(cell,
               std::string_view{text.data(),
                                static_cast<std::size_t>(std::max(length, 0))},
               theme.text_muted);
      return;
    }
    double out = static_cast<double>(value);
    if (impl.program_fields.draw(ui, gui::ui_id(base, salt), cell, letter,
                                 static_cast<double>(value), speed, out,
                                 decimals)) {
      apply(field, static_cast<float>(out) * scale);
    }
  };
  // The arrow that puts a group of values back, shown while they differ.
  const auto reset = [&](core::u64 salt, bool differs, std::string_view tip,
                         TransformTool cleared) {
    const gui::Rect cell{x - gap + 1.0F, row.y, reset_button_width,
                         row.height};
    x += reset_button_width + group_gap - gap;
    if (!differs || !editable || cell.right() > row.right()) {
      return;
    }
    if (reset_button(ui, gui::ui_id(base, salt), cell, tip)) {
      clear_transform(cleared, authored);
    }
  };
  const auto differs = [](scene::Vec3 a, scene::Vec3 b) {
    return a.x != b.x || a.y != b.y || a.z != b.z;
  };
  value_field(1U, "X", "Where the layer's middle is across the picture, in "
                       "pixels from its left edge",
              CanvasBoxField::X, metrics->x, 1.0, 1, 1.0F);
  value_field(2U, "Y", "Where the layer's middle is down the picture, in "
                       "pixels from its top edge",
              CanvasBoxField::Y, metrics->y, 1.0, 1, 1.0F);
  reset(10U, differs(evaluated.position, scene::Vec3{}),
        "Reset position to 0, 0, 0", TransformTool::Move);
  value_field(3U, "W", "The layer's width on the picture, in pixels",
              CanvasBoxField::Width, metrics->width, 1.0, 1, 1.0F);
  value_field(4U, "H", "The layer's height on the picture, in pixels",
              CanvasBoxField::Height, metrics->height, 1.0, 1, 1.0F);
  reset(11U, differs(evaluated.scale, scene::Vec3{1.0F, 1.0F, 1.0F}),
        "Reset scale to 1", TransformTool::Scale);
  if (metrics->sides) {
    value_field(5U, "R", "How far the layer is turned, in degrees clockwise",
                CanvasBoxField::Rotation, metrics->turns * 360.0F, 0.5, 1,
                1.0F / 360.0F);
    reset(12U, differs(evaluated.rotation, scene::Vec3{}),
          "Reset rotation to 0", TransformTool::Rotate);
  }
}

} // namespace mediaserver::editor
