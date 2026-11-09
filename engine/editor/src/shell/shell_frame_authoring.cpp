// Turning viewport gestures, Library actions and finished model ingests into edits.

#include "shell_frame.hpp"

#include "native_dialogs.hpp"

#include <thread>

namespace mediaserver::editor {

void EditorShell::Frame::adopt_model_ingests() {
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
}

void EditorShell::Frame::capture_group(const scene::Scene &authored_scene,
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
}

// One transform component group (position, rotation or scale) written the
// way the Inspector writes it: a key at the show frame on a component that
// is animated, the base value on one that is not.
void EditorShell::Frame::emit_components(doc::NodeId node,
                                         scene::TransformChannel first,
                                         scene::Vec3 values,
                                         core::u64 gesture) {
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
}

// Lands one viewport move, rotate or scale on every captured element. A
// rotate or scale orbits the elements about the shared pivot, so each one
// off the pivot also moves; one on it keeps its position untouched rather
// than gaining a key it never asked for.
void EditorShell::Frame::emit_transform(const TransformDelta &delta,
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
}

// Alt+G / Alt+R / Alt+S over the Stage: Blender's clear location, rotation
// and scale, as one undo step for the whole selection.
void EditorShell::Frame::clear_transform(TransformTool tool,
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
}

// Projection surfaces are physical stage geometry belonging to the show, not
// to the scene on the Stage, and the feed map they live in is drawn at the
// Program playhead. Key them there too, or a surface dragged while Preview
// sits at a different frame would author a key the operator cannot see.
void EditorShell::Frame::route_surface_edit(FeedSurfaceEditRequest request) {
  RoutedSurfaceEdit routed = route_surface_transform_edit(
      std::move(request), keyframes, timeline.program_frame,
      impl.timeline_panel.auto_key_enabled());
  result.keys.insert(result.keys.end(),
                     std::make_move_iterator(routed.keys.begin()),
                     std::make_move_iterator(routed.keys.end()));
  if (routed.base.has_value()) {
    result.feed_surface_edits.push_back(std::move(routed.base.value()));
  }
}

void EditorShell::Frame::enqueue_model(
    std::string source, scene::SceneId add_scene,
    std::optional<FeedSurfaceAddRequest> surface_add,
    std::optional<FeedSurfaceEditRequest> surface_edit, doc::NodeId folder,
    doc::NodeId revise, std::optional<core::u64> gesture) {
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
}

void EditorShell::Frame::handle_viewport(const ViewportFrame &viewport,
                                         const scene::Scene &authored_view,
                                         const scene::Scene &display_view) {
  // Ctrl+Alt box: the elements it touched become the selection, or join
  // it with Shift. An empty box clears it, as in Unreal.
  if (viewport.marquee.has_value()) {
    impl.stage_surface_selection.reset();
    if (!viewport.marquee_adds) {
      impl.clear_scene_selection(true);
    }
    for (const doc::NodeId node : *viewport.marquee) {
      if (!impl.is_selected(node)) {
        impl.toggle(node);
      }
    }
  }
  // Alt on a gizmo handle: the copies stay where the selection stands.
  if (viewport.duplicate && !impl.scene_selection.empty()) {
    const auto members = impl.scene_selection.members();
    result.scene_duplicates.push_back(SceneDuplicateRequest{
        authored_stage.id,
        std::vector<doc::NodeId>(members.begin(), members.end()),
        impl.next_edit_gesture++});
    impl.history_context = HistoryContext::Scene;
    impl.semantic_boundary = true;
  }
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
}

// The current Stage view's shading. Wireframe needs a device that can
// rasterize edges; without one the view stays as it is and says why.
void EditorShell::Frame::choose_shading(render::ViewShading shading) {
  if (shading == render::ViewShading::Wireframe &&
      !impl.window.device().capabilities().wireframe_fill) {
    impl.stage_bookmark_note = "Wireframe is unavailable on this GPU";
    impl.stage_bookmark_note_until = impl.window.time_seconds() + 3.0;
    impl.stage_bookmark_view = impl.stage_current;
    return;
  }
  impl.stage().shading = shading;
}

// Stage camera navigation, as in the Unreal Editor's perspective viewport.
// `pointer_free` says no floating panel covers the pointer; `left_available`
// that a left press on bare canvas may walk (mask authoring owns it).
void EditorShell::Frame::navigate_stage_camera(const ViewportFrame &viewport,
                                               bool pointer_free,
                                               bool left_available) {
  const bool already = impl.navigate_left || impl.navigate_middle ||
                       impl.navigate_right;
  // A gesture belongs to the view it began in; the others leave it be.
  if (already && impl.navigation_view != impl.stage_current) {
    return;
  }
  const bool over =
      pointer_free && viewport.image.contains(ui.pointer_x(), ui.pointer_y());
  const bool left_press = left_available && viewport.pressed &&
                          viewport.empty_canvas ==
                              EmptyCanvasInteraction::Navigate;
  // A left drag that picked an element is that element's; a right or
  // middle press then leaves the camera alone.
  const bool element_drag =
      viewport.held && !impl.navigate_left && !left_press;
  if ((over || already) && !element_drag && !viewport.pivot_editing) {
    impl.navigate_right = impl.navigate_right || ui.secondary_pointer_pressed();
    impl.navigate_middle = impl.navigate_middle || ui.middle_pointer_pressed();
    impl.navigate_left = impl.navigate_left || left_press;
    if (!already) {
      impl.navigation_view = impl.stage_current;
    }
  }
  impl.navigate_left = impl.navigate_left && ui.pointer_down();
  impl.navigate_middle = impl.navigate_middle && ui.middle_pointer_down();
  impl.navigate_right = impl.navigate_right && ui.secondary_pointer_down();

  const double now = impl.window.time_seconds();
  const float seconds = impl.navigation_seconds;
  const StageNavigation gesture = stage_navigation_for(StageNavigationButtons{
      impl.navigate_left, impl.navigate_middle, impl.navigate_right,
      ui.alt_held(), ui.shift_held()});
  if (gesture == StageNavigation::None) {
    impl.navigation_moved = false;
    if (over && ui.wheel() != 0.0F) {
      impl.stage().orbit = stage_wheel_dolly(impl.stage().orbit, ui.wheel());
    }
    // F frames the selection, as everywhere in Unreal.
    const std::string_view typed = ui.shortcut_text();
    if (over && !impl.scene_selection.empty() &&
        typed.find_first_of("fF") != std::string_view::npos) {
      if (const auto bounds = impl.stage().panel.world_bounds(
              stage_interaction_scene, impl.scene_selection.members())) {
        impl.stage().orbit = stage_focus(impl.stage().orbit, *bounds);
      }
    }
    // Alt+P, J, H and K choose the projection, as the view's menu does.
    if (over) {
      constexpr std::array<std::pair<gui::Key, StageProjection>, 4U> views{
          {{gui::Key::ViewPerspective, StageProjection::Perspective},
           {gui::Key::ViewTop, StageProjection::Top},
           {gui::Key::ViewFront, StageProjection::Front},
           {gui::Key::ViewSide, StageProjection::Side}}};
      for (const auto &[key, projection] : views) {
        if (ui.key_pressed(key)) {
          impl.stage().orbit.projection = projection;
        }
      }
      constexpr std::array<std::pair<gui::Key, render::ViewShading>, 4U>
          shadings{{{gui::Key::ShadeLit, render::ViewShading::Lit},
                    {gui::Key::ShadeUnlit, render::ViewShading::Unlit},
                    {gui::Key::ShadeLightsOnly,
                     render::ViewShading::LightsOnly},
                    {gui::Key::ShadeWireframe,
                     render::ViewShading::Wireframe}}};
      for (const auto &[key, shading] : shadings) {
        if (ui.key_pressed(key)) {
          choose_shading(shading);
        }
      }
    }
    // Ctrl+1..9 keeps this view in a slot and the digit returns to it, as
    // Unreal's camera bookmarks. Editor-local: the show's cameras are
    // untouched.
    if (over && !impl.stage().panel.transforming()) {
      for (std::size_t slot = 0U; slot < impl.stage_bookmarks.size();
           ++slot) {
        if (ui.key_pressed(static_cast<gui::Key>(
                static_cast<core::u8>(gui::Key::SaveView1) + slot))) {
          impl.stage_bookmarks[slot] = impl.stage().orbit;
          impl.stage_bookmark_note = "View " + std::to_string(slot + 1U) +
                                     " saved";
          impl.stage_bookmark_view = impl.stage_current;
          impl.stage_bookmark_note_until = now + 1.5;
        }
      }
      for (const char key : typed) {
        if (key < '1' || key > '9') {
          continue;
        }
        const auto slot = static_cast<std::size_t>(key - '1');
        if (impl.stage_bookmarks[slot].has_value()) {
          impl.stage().orbit = *impl.stage_bookmarks[slot];
          impl.stage_bookmark_note =
              "View " + std::string(1U, key);
        } else {
          impl.stage_bookmark_note = "No view " + std::string(1U, key) +
                                     " yet: Ctrl+" + std::string(1U, key) +
                                     " keeps one";
        }
        impl.stage_bookmark_note_until = now + 1.5;
        impl.stage_bookmark_view = impl.stage_current;
      }
    }
  } else {
    StageNavigationStep step{};
    step.gesture = gesture;
    step.dx = ui.pointer_motion_x();
    step.dy = ui.pointer_motion_y();
    step.seconds = seconds;
    step.units_per_pixel =
        stage_units_per_pixel(impl.stage().orbit, viewport.image.height);
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
    impl.stage().orbit = navigate_stage(impl.stage().orbit, step);
  }

  if (now < impl.fly_speed_shown_until &&
      impl.navigation_view == impl.stage_current) {
    char text[48]{};
    std::snprintf(text, sizeof(text), "CAMERA SPEED %.2f", impl.fly_speed);
    const gui::Rect badge{viewport.image.x + 8.0F,
                          viewport.image.bottom() - 32.0F,
                          156.0F, 24.0F};
    ui.panel(badge, ui.theme().background);
    ui.outline(badge, ui.theme().divider);
    ui.label(gui::Rect{badge.x + 8.0F, badge.y, badge.width - 16.0F,
                       badge.height},
             text, ui.theme().text);
  }
}

void EditorShell::Frame::add_simple(AddKind kind) {
  result.adds.push_back(AddIntent{kind,
                                  std::string{},
                                  1U,
                                  impl.next_edit_gesture++,
                                  16.0F / 9.0F,
                                  {},
                                  authored_stage.id});
}

void EditorShell::Frame::handle_library_add(LibraryAddAction action) {
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
  case LibraryAddAction::Background:
    add_simple(AddKind::Background);
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
    open_file_dialog(
        []() -> std::optional<std::string> {
          const char *const filters[]{
              "*.mp4", "*.mov", "*.mkv", "*.webm", "*.avi", "*.mxf",
              "*.m4v", "*.mpg", "*.mpeg", "*.wmv", "*.png", "*.jpg",
              "*.jpeg", "*.bmp", "*.tga", "*.exr", "*.hdr"};
          const char *const path = tinyfd_openFileDialog(
              "Add Media", "", static_cast<int>(std::size(filters)), filters,
              "Video and images", 0);
          return path != nullptr ? std::optional<std::string>{path}
                                 : std::nullopt;
        },
        [](Frame &frame, std::string source) {
          try {
            frame.impl.pending_media_probes.push_back(Impl::PendingMediaProbe{
                Impl::PendingMediaAction::Add, source, {}, false,
                frame.impl.next_edit_gesture++, frame.authored_stage.id,
                frame.project.generation});
          } catch (...) {
            frame.result.adds.push_back(AddIntent{
                AddKind::Media, source, 1U, frame.impl.next_edit_gesture++,
                16.0F / 9.0F, {}, frame.authored_stage.id});
          }
        });
    return;
  }
  case LibraryAddAction::Model: {
    open_file_dialog(
        []() -> std::optional<std::string> {
          const char *const filters[]{"*.gltf", "*.glb", "*.obj",
                                      "*.msmodel"};
          const char *const path = tinyfd_openFileDialog(
              "Add 3D Model", "", 4, filters,
              "3D models (glTF, OBJ, model package)", 0);
          return path != nullptr ? std::optional<std::string>{path}
                                 : std::nullopt;
        },
        [](Frame &frame, std::string source) {
          frame.enqueue_model(std::move(source), frame.authored_stage.id, {},
                              {});
        });
    return;
  }
  }
}

// Files become Library assets in `folder`: video and images are probed,
// models go through the model worker, and anything else is named in the
// status line rather than silently dropped.
void EditorShell::Frame::import_files(std::span<const std::string> sources,
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
    } else if (kind == LibraryImportAction::ParticleEffect) {
      // A package carries everything it needs; there is nothing to probe.
      // Each renderer reads and checks it off its frame path.
      assets::ParticleEffectAsset asset{};
      asset.display_name = std::filesystem::path{source}.stem().string();
      asset.source = source;
      asset.folder = folder;
      result.asset_imports.push_back(
          AssetImportIntent{assets::Asset{std::move(asset)}, gesture});
    } else {
      ++refused;
    }
  }
  if (refused > 0U) {
    impl.ingest_message =
        refused == 1U
            ? "Not a media, model or particle file; nothing imported"
            : std::to_string(refused) +
                  " files are not media, models or particle effects; skipped";
  }
}

void EditorShell::Frame::import_asset(LibraryImportAction action,
                                      doc::NodeId folder) {
  const bool media = action == LibraryImportAction::Media;
  const bool particles = action == LibraryImportAction::ParticleEffect;
  open_file_dialog(
      [media, particles]() -> std::optional<std::string> {
        const char *const media_filters[]{
            "*.mp4", "*.mov", "*.mkv", "*.webm", "*.avi", "*.mxf",
            "*.m4v", "*.mpg", "*.mpeg", "*.wmv", "*.png", "*.jpg",
            "*.jpeg", "*.bmp", "*.tga", "*.exr", "*.hdr"};
        const char *const model_filters[]{"*.gltf", "*.glb", "*.obj",
                                          "*.msmodel"};
        const char *const particle_filters[]{"*.ajfx"};
        const char *const path = tinyfd_openFileDialog(
            media       ? "Import Media Asset"
            : particles ? "Import Particle Effect Asset"
                        : "Import 3D Model Asset",
            "",
            media       ? static_cast<int>(std::size(media_filters))
            : particles ? 1
                        : 4,
            media       ? media_filters
            : particles ? particle_filters
                        : model_filters,
            media       ? "Video and images"
            : particles ? "ajfx particle effects"
                        : "3D models (glTF, OBJ)",
            1);
        return path != nullptr ? std::optional<std::string>{path}
                               : std::nullopt;
      },
      [folder](Frame &frame, std::string chosen) {
        const std::vector<std::string> sources =
            selected_dialog_paths(chosen);
        frame.import_files(sources, folder);
      });
}

void EditorShell::Frame::open_file_dialog(
    std::function<std::optional<std::string>()> show,
    std::function<void(Frame &, std::string)> apply) {
  using Dialog = AsyncDialog<Frame>;
  const Dialog::Open opened = impl.dialog.open(
      [show = std::move(show)]() -> std::optional<std::string> {
        // tinyfiledialogs answers in static buffers; a dialog opened from
        // anywhere else is never waited for.
        const std::unique_lock dialog{native_dialog_mutex(), std::try_to_lock};
        return dialog.owns_lock() ? show() : std::nullopt;
      },
      std::move(apply), project.generation);
  if (opened == Dialog::Open::Busy) {
    impl.ingest_message = "Another dialog is already open";
  } else if (opened == Dialog::Open::Failed) {
    impl.ingest_message = "Could not open the dialog";
  }
}

void EditorShell::Frame::open_confirm_dialog(
    std::string title, std::string message,
    std::function<void(Frame &)> yes) {
  open_file_dialog(
      [title = std::move(title),
       message = std::move(message)]() -> std::optional<std::string> {
        return tinyfd_messageBox(title.c_str(), message.c_str(), "yesno",
                                 "warning", 0) != 0
                   ? std::optional<std::string>{std::string{}}
                   : std::nullopt;
      },
      [yes = std::move(yes)](Frame &frame, std::string) {
        if (yes) {
          yes(frame);
        }
      });
}

void EditorShell::Frame::confirm_discard(std::string_view action,
                                         std::function<void(Frame &)> then) {
  if (!project.dirty) {
    then(*this);
    return;
  }
  std::string message{
      "This project has unsaved changes.\n\nContinuing will discard "};
  message.append(action);
  message.append(". Continue?");
  open_confirm_dialog("Unsaved Project", std::move(message), std::move(then));
}

void EditorShell::Frame::save_project() {
  if (!project.path.empty()) {
    result.save_project = ProjectFileRequest{std::string{project.path}};
    return;
  }
  open_file_dialog(
      [current = std::string{project.path}] {
        return choose_project_to_save(current);
      },
      [](Frame &frame, std::string path) {
        frame.result.save_project = ProjectFileRequest{std::move(path)};
      });
}

void EditorShell::Frame::adopt_dialog() {
  if (impl.dialog.adopt(*this, project.generation) ==
      AsyncDialog<Frame>::Adopt::Stale) {
    impl.ingest_message =
        "Another project opened while the dialog was up; nothing applied";
  }
}

void EditorShell::Frame::instantiate_asset(
    doc::NodeId id, std::optional<scene::Vec3> position,
    std::optional<CompositorAddIntent> graph_layer,
    std::optional<scene::Vec3> rotation,
    std::optional<scene::ClipTiming> timing) {
  const assets::Asset *const found = asset_registry.asset(id);
  if (found == nullptr ||
      std::holds_alternative<assets::PlatesetAsset>(*found)) {
    return;
  }
  if (const auto *const media = std::get_if<assets::MediaAsset>(found)) {
    // The import probe already measured the frame on a worker. Opening the
    // source here would read it on the frame thread, which for a large file
    // on a network share stalls the Director long enough to drop render nodes
    // from lockstep, so an unprobed asset is placed 16:9 instead.
    float aspect = 16.0F / 9.0F;
    if (media->dimensions.has_value() && media->dimensions->width != 0U &&
        media->dimensions->height != 0U) {
      aspect = static_cast<float>(media->dimensions->width) /
               static_cast<float>(media->dimensions->height);
    }
    result.adds.push_back(AddIntent{AddKind::Media, media->source,
                                    media->frame_count.value_or(1U),
                                    impl.next_edit_gesture++, aspect,
                                    media->id, authored_stage.id, position,
                                    media->frame_rate, graph_layer});
    result.adds.back().rotation = rotation;
    result.adds.back().timing = timing;
    return;
  }
  if (const auto *const effect =
          std::get_if<assets::ParticleEffectAsset>(found)) {
    result.adds.push_back(AddIntent{AddKind::ParticleEffect, effect->source,
                                    1U, impl.next_edit_gesture++,
                                    16.0F / 9.0F, effect->id,
                                    authored_stage.id, position});
    result.adds.back().rotation = rotation;
    return;
  }
  const auto &model = std::get<assets::ModelAsset>(*found);
  result.adds.push_back(AddIntent{AddKind::Model, model.source, 1U,
                                  impl.next_edit_gesture++, 16.0F / 9.0F,
                                  model.id, authored_stage.id, position,
                                  std::nullopt, graph_layer});
  result.adds.back().rotation = rotation;
}

void EditorShell::Frame::draw_placeholder(
    const gui::Rect &bounds, std::string_view title,
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
}

} // namespace mediaserver::editor
