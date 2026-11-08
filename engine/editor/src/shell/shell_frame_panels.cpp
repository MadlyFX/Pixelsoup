// What the panels share this frame, and the dispatch that draws each docked panel.

#include "shell_frame.hpp"

namespace mediaserver::editor {

void EditorShell::Frame::prepare_panels() {
  delete_consumed = false;
  clipboard_consumed = false;
  editor_control_editing = false;
  stage_viewport_drawn = false;
  viewer_drawn_now = false;
  // The Asset Viewer drew a plateset's rig this frame, so a Library drop
  // can land on its slots.
  plateset_editor_drawn = false;
  // The thumbnails its slot tiles show, kept and made like the Library's.
  // Kept plateset moments where the Stage scene plays them: each placed
  // front plate's moments, through its clip's trim and rate, inside it.
  for (const ElementInfo &plate : elements) {
    if (plate.kind != ElementKind::Media || plate.plateset == doc::NodeId{} ||
        plate.plateset_slot != "front" || !plate.timing.placed()) {
      continue;
    }
    const assets::Asset *const asset = asset_registry.asset(plate.plateset);
    const auto *const set =
        asset != nullptr ? std::get_if<assets::PlatesetAsset>(asset) : nullptr;
    if (set == nullptr) {
      continue;
    }
    const scene::MediaFrameRate source = plate.source_frame_rate.value_or(
        scene::MediaFrameRate{timeline.rate_numerator,
                              timeline.rate_denominator});
    if (source.numerator == 0U || timeline.rate_denominator == 0U) {
      continue;
    }
    for (const assets::PlatesetMoment &moment : set->moments) {
      const core::i64 into = moment.frame - plate.timing.source_in;
      if (into < 0) {
        continue;
      }
      const core::i64 show =
          plate.timing.start +
          into * static_cast<core::i64>(timeline.rate_numerator) *
              static_cast<core::i64>(source.denominator) /
              (static_cast<core::i64>(timeline.rate_denominator) *
               static_cast<core::i64>(source.numerator));
      if (show < plate.timing.start ||
          show >= plate.timing.start + plate.timing.duration) {
        continue;
      }
      timeline_moments.push_back(
          TimelineMoment{show, moment.kind, plate.node});
    }
  }
  // The Library's names for the platesets placed plates come from, for the
  // Timeline's take rows.
  for (const ElementInfo &plate : elements) {
    if (plate.kind != ElementKind::Media || plate.plateset == doc::NodeId{} ||
        std::any_of(timeline_platesets.begin(), timeline_platesets.end(),
                    [&](const TimelinePlateset &named) {
                      return named.asset == plate.plateset;
                    })) {
      continue;
    }
    const assets::Asset *const asset = asset_registry.asset(plate.plateset);
    if (const auto *const set =
            asset != nullptr ? std::get_if<assets::PlatesetAsset>(asset)
                             : nullptr) {
      timeline_platesets.push_back(
          TimelinePlateset{plate.plateset, set->display_name});
    }
  }
  // Files dragged in from the desktop that the Library imported itself.
  library_took_files = false;
  compositor_panel_drawn = false;
  timeline_panel_drawn = false;
  inspector_panel_drawn = false;
  color_sample_active = impl.color_sample_request.has_value();
  // Sampling a colour or shaping a mask is judged against the real pixels,
  // so every view shows the source while either is under way.
  const bool precise_media_work =
      color_sample_active || impl.mask_selection.mask != doc::NodeId{};
  for (auto &view : impl.stage_views) {
    view.panel.set_precise_work(precise_media_work);
  }
  impl.program_panel.set_precise_work(precise_media_work);
  const ElementInfo *const mask_target_element =
      find_element(elements, impl.anchor());
  const bool mask_target_supported =
      mask_target_element != nullptr &&
      !is_light(mask_target_element->kind);
  mask_target = MasksPanelTarget{
      authored_stage.id,
      mask_target_supported ? mask_target_element->node : doc::NodeId{},
      mask_target_element != nullptr &&
              mask_target_element->kind == ElementKind::Group
          ? scene::MaskCoordinateSpace::AuthoredCanvas
          : scene::MaskCoordinateSpace::SourceLayer};
  mask_authoring_enabled = mask_target_supported && !mask_target_element->locked &&
      !switcher.rehearsal_locked;
  // Stage switches interaction domains only while Masks is the visible tab
  // and a real target is selected. With no target it stays an ordinary Stage,
  // so the operator can click a drawable and enter mask mode on the next
  // projected frame.
  mask_stage_active = impl.dock_host.visible(panel_masks) && mask_target_supported;
  // Program Output shows the picture that is actually on the wall, which is
  // where a matte in the wrong place is noticed, so it authors masks too. It
  // may only do so while it and the Stage are the same scene: with Preview on
  // the Stage the two panels show different scenes, and a mask dragged over a
  // layer the picture does not contain would be authored somewhere nobody is
  // looking. A running dissolve is excluded outright - the picture is a mix of
  // two committed scenes and belongs to neither.
  mask_program_active = mask_stage_active && authored_stage.id == switcher.program_scene &&
      !switcher.transition_active;
  // Compositor nodes are scene-scoped, so the graph on screen is the graph of
  // the scene on the Stage and editing it never reaches Program. The one thing
  // that still has to hold is a running dissolve: both of its inputs are
  // already committed pictures, so no graph edit may land mid-transition.
  compositor_locked = switcher.rehearsal_locked;
}

// The other plates of `clip`'s take: the same take, start and length.
// A plate moved or trimmed on its own has left its take.
std::vector<const ElementInfo *>
EditorShell::Frame::take_siblings(const ElementInfo &clip) {
  std::vector<const ElementInfo *> plates;
  if (clip.plateset_take == 0) {
    return plates;
  }
  for (const ElementInfo &other : elements) {
    if (other.node != clip.node && other.kind == ElementKind::Media &&
        other.plateset_take == clip.plateset_take &&
        other.timing.start == clip.timing.start &&
        other.timing.duration == clip.timing.duration && !other.locked) {
      plates.push_back(std::addressof(other));
    }
  }
  return plates;
}

void EditorShell::Frame::queue_mask_edit(MaskEditRequest request) {
  result.mask_edit =
      MaskEditIntent{std::move(request), impl.begin_gesture()};
  impl.history_context = HistoryContext::Scene;
  impl.semantic_boundary = true;
}

void EditorShell::Frame::run_color_sampler(gui::UiId id, const gui::Rect &image,
                                           const rhi::Texture &target,
                                           bool pointer_available) {
  if (!impl.color_sample_request.has_value()) {
    return;
  }
  const auto point =
      draw_eyedropper_overlay(ui, id, image, pointer_available);
  if (!point.has_value()) {
    return;
  }
  const auto sampled = sample_viewport_color(
      impl.window.device(), target, image, point->x, point->y);
  if (!sampled.has_value()) {
    return;
  }
  const InspectorColorSampleRequest request =
      std::move(*impl.color_sample_request);
  impl.color_sample_request.reset();
  const std::array<double, 3U> values{
      static_cast<double>(sampled->r),
      static_cast<double>(sampled->g),
      static_cast<double>(sampled->b)};
  const core::u64 gesture = impl.next_edit_gesture++;
  for (std::size_t component = 0U; component < values.size();
       ++component) {
    result.generic_edits.push_back(GenericEdit{
        request.node, request.properties[component], values[component],
        gesture});
  }
  impl.history_context = HistoryContext::Scene;
  impl.semantic_boundary = true;
}

// The mask target, its masks, and its authoring lock are one domain. Only
// the picture a gesture is measured against differs between the two views,
// which is exactly the projection each one passes.
MaskViewportInput
EditorShell::Frame::mask_viewport_input(const scene::Scene &view,
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
}

void EditorShell::Frame::apply_mask_viewport_frame(
    const MaskViewportFrame &mask_frame) {
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
                                 MaskViewportShapeEditIntent>) {
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
}

// A Timeline clip's source in the Asset Viewer: at the source frame under
// the playhead for match frame, else at the clip's first source frame,
// with In and Out around the source frames its first pass plays.
void EditorShell::Frame::open_clip_in_viewer(
    const TimelineViewClipRequest &request) {
  const ElementInfo *const clip = find_element(elements, request.clip);
  if (clip == nullptr || clip->kind != ElementKind::Media) {
    return;
  }
  impl.show_panel(panel_asset_viewer);
  // A take row opens the set its plates come from, rig and all.
  if (request.plateset && clip->plateset != doc::NodeId{}) {
    impl.asset_viewer_panel.open(clip->plateset);
    return;
  }
  const assets::Asset *const asset =
      asset_registry.asset_for_source(clip->source, assets::AssetKind::Media);
  const auto *const media =
      asset != nullptr ? std::get_if<assets::MediaAsset>(asset) : nullptr;
  if (media == nullptr) {
    impl.asset_viewer_panel.show_message(
        "The clip's source is not in the Library", ui.time_seconds());
    return;
  }
  const core::u32 count = asset_viewer_frame_count(*media);
  const scene::MediaFrameRate show_rate{timeline.rate_numerator,
                                        timeline.rate_denominator};
  scene::ClipPlacement placement{};
  placement.node = clip->node;
  placement.source = clip->source;
  placement.sequence_start = clip->sequence_start;
  placement.sequence_count = std::max(clip->sequence_count, 1U);
  placement.timing = clip->timing;
  placement.frame_rate = clip->source_frame_rate;
  const scene::SceneTime time{0U, show_rate.numerator,
                              show_rate.denominator};
  const core::u64 at =
      request.at_playhead
          ? timeline.show_frame
          : static_cast<core::u64>(std::max<core::i64>(clip->timing.start, 0));
  const core::u32 shown =
      scene::clip_source_frame(placement, time, at) - placement.sequence_start;
  const core::u32 first = static_cast<core::u32>(
      static_cast<core::u64>(std::max<core::i64>(clip->timing.source_in, 0)) %
      count);
  core::u32 last = count - 1U;
  if (clip->timing.duration > 0) {
    // The last frame the clip's floor mapping reaches, unwrapped.
    if (const auto reached = scene::advance_source_phase(
            0, clip->timing.duration - 1, show_rate,
            clip->source_frame_rate, std::numeric_limits<core::u32>::max(),
            scene::SourcePhaseRounding::Floor);
        reached && reached.value() >= 0) {
      last = static_cast<core::u32>(std::min<core::i64>(
          static_cast<core::i64>(first) + reached.value(),
          static_cast<core::i64>(count) - 1));
    }
  }
  impl.asset_viewer_panel.open(media->id, std::min(shown, count - 1U),
                               std::pair{first, last});
}

void EditorShell::Frame::draw_panel(gui::PanelId panel,
                                    const gui::Rect &bounds) {
  if (panel == panel_stage) {
    draw_stage_panel(bounds);
    return;
  }
  if (panel == panel_program) {
    draw_program_panel(bounds);
    return;
  }
  if (panel == panel_scene) {
    draw_scene_panel(bounds);
    return;
  }
  if (panel == panel_selection_sets) {
    draw_selection_sets_panel(bounds);
    return;
  }
  if (panel == panel_masks) {
    draw_masks_panel(bounds);
    return;
  }
  if (panel == panel_asset_viewer) {
    draw_asset_viewer_panel(bounds);
    return;
  }
  if (panel == panel_library) {
    draw_library_panel(bounds);
    return;
  }
  if (panel == panel_effects) {
    draw_effects_panel(bounds);
    return;
  }
  if (panel == panel_compositor || panel == panel_nodes) {
    draw_compositor_panel(bounds);
    return;
  }
  if (panel == panel_hardware_mapping) {
    draw_hardware_mapping_panel(bounds);
    return;
  }
  if (panel == panel_feed_mapping) {
    draw_feed_mapping_panel(bounds);
    return;
  }
  if (panel == panel_output) {
    draw_output_panel(bounds);
    return;
  }
  if (panel == panel_live_data) {
    draw_live_data_panel(bounds);
    return;
  }
  if (panel == panel_cues) {
    draw_cues_panel(bounds);
    return;
  }
  if (panel == panel_automation) {
    draw_automation_panel(bounds);
    return;
  }
  if (panel == panel_macros) {
    draw_macros_panel(bounds);
    return;
  }
  if (panel == panel_timeline) {
    draw_timeline_panel(bounds);
    return;
  }
  if (panel == panel_color_correction) {
    draw_color_correction_panel(bounds);
    return;
  }
  if (panel == panel_texture) {
    draw_texture_panel(bounds);
    return;
  }
  if (panel == panel_inspector || panel == panel_inspector_overview) {
    draw_inspector_panel(bounds);
    return;
  }
}

} // namespace mediaserver::editor
