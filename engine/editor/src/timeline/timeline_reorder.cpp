#include "timeline_internal.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

// Reordering on the Timeline: lifting layers and tracks, resolving the band
// or insertion edge they would land on, committing one absolute reorder on
// release (or a screen-space drop off the panel), and the drag feedback drawn
// over everything else - including a moving clip's ghost and status.
namespace mediaserver::editor {
using namespace timeline;

void TimelinePanel::capture_layer_drag(Pass &p, doc::NodeId grabbed) {
  const std::span<const ElementInfo> elements = p.input.elements;
  const std::span<const doc::NodeId> selection = p.input.selection;
  const std::span<const scene::TimelineLane> lanes = p.input.lanes;
  const std::vector<doc::NodeId> &visible_order = p.built.visible_order;

  reorder_.layer_nodes.clear();
  if (!selected(grabbed, selection)) {
    reorder_.layer_nodes.push_back(grabbed);
    return;
  }
  const auto append_selected = [&](doc::NodeId node) {
    if (!selected(node, selection)) {
      return;
    }
    const ElementInfo *const element = find_element(elements, node);
    const bool layer = element != nullptr && !element->locked &&
                       (orderable_layer(*element) ||
                        element->kind == ElementKind::Track);
    const bool track = std::any_of(lanes.begin(), lanes.end(),
        [&](const scene::TimelineLane &lane) {
          return !lane.implicit && !lane.locked && lane.id == node;
        });
    if ((layer || track) &&
        std::find(reorder_.layer_nodes.begin(), reorder_.layer_nodes.end(), node) ==
            reorder_.layer_nodes.end()) {
      reorder_.layer_nodes.push_back(node);
    }
  };
  for (const doc::NodeId node : visible_order) {
    append_selected(node);
  }
  // A collapsed parent does not remove its children from selection.
  for (const ElementInfo &element : elements) {
    append_selected(element.node);
  }
}

void TimelinePanel::resolve_reorder_targets(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const std::span<const ElementInfo> elements = p.input.elements;
  const std::span<const scene::TimelineLane> lanes = p.input.lanes;
  TimelinePanelFrame &frame = p.frame;
  FrameScratch &scratch = p.scratch;
  const gui::Rect &rows_bounds = p.layout.rows;
  std::vector<Row> &rows = p.built.rows;
  auto &gesture_y = p.gesture_y;
  auto &clip_drag_active = p.clip_drag_active;

  // Track reordering deliberately mirrors the Feed Mapping surface drop:
  // movement lifts the source, every complete track band is a target, and the
  // document changes only once the pointer is released. A band includes its
  // clips and expanded property rows, so there are no narrow dead zones.
  auto &lane_drop_targets = p.lane_drop_targets;
  lane_drop_targets.reserve(lanes.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    if (rows[index].kind != Row::Kind::LaneHeader) {
      continue;
    }
    const doc::NodeId lane = rows[index].node;
    std::size_t end = index + 1U;
    while (end < rows.size() && rows[end].lane_member &&
           rows[end].lane_owner == lane) {
      ++end;
    }
    const float raw_top = rows_bounds.y + row_top(rows, index) - row_scroll_;
    const float raw_bottom = rows_bounds.y + row_top(rows, end) - row_scroll_;
    const float top = std::max(raw_top, rows_bounds.y);
    const float bottom = std::min(raw_bottom, rows_bounds.y + rows_bounds.height);
    if (bottom > top) {
      lane_drop_targets.push_back(LaneDropTarget{
          lane, gui::Rect{rows_bounds.x, top, rows_bounds.width, bottom - top},
          rows[index].label, rows[index].locked});
    }
  }

  if (reorder_.lane_node != doc::NodeId{} && ctx.pointer_down()) {
    const float dx = ctx.pointer_x() - reorder_.lane_start_x;
    const float dy = ctx.pointer_y() - reorder_.lane_start_y;
    reorder_.lane_moved = reorder_.lane_moved ||
                       dx * dx + dy * dy >= lane_drag_threshold_squared;
  }
  p.lane_drag_active =
      reorder_.lane_node != doc::NodeId{} && reorder_.lane_moved;

  if (reorder_.layer_node != doc::NodeId{} && ctx.pointer_down()) {
    const float dx = ctx.pointer_x() - reorder_.layer_start_x;
    const float dy = ctx.pointer_y() - reorder_.layer_start_y;
    reorder_.layer_moved = reorder_.layer_moved ||
                        dx * dx + dy * dy >= lane_drag_threshold_squared;
  }
  scratch.layer_drag_active =
      reorder_.layer_node != doc::NodeId{} && reorder_.layer_moved;

  // A track drag is ordering, so it gets only an insertion cursor. A clip
  // drag is assignment, so outlining a track band is truthful there: the clip
  // will become a child of that track. Keeping these feedback vocabularies
  // separate prevents a reorder from looking like nesting.
  scratch.clip_move_feedback =
      clip_drag_active && clip_.drag == ClipDrag::Move;
  p.clip_drop_hovered = nullptr;
  auto &clip_drop_hovered = p.clip_drop_hovered;
  if (scratch.clip_move_feedback && clip_.drop_lane.has_value() &&
      *clip_.drop_lane != doc::NodeId{}) {
    const auto target = std::find_if(
        lane_drop_targets.begin(), lane_drop_targets.end(),
        [&](const LaneDropTarget &candidate) {
          return !candidate.locked && candidate.lane == *clip_.drop_lane;
        });
    if (target != lane_drop_targets.end()) {
      clip_drop_hovered = &*target;
    }
  }
  if (scratch.clip_move_feedback) {
    const ElementInfo *const dragged = find_element(elements, clip_.node);
    scratch.clip_drag_label = dragged != nullptr ? layer_name(*dragged) : "CLIP";
  }

  p.layer_lane_hovered = nullptr;
  auto &layer_lane_hovered = p.layer_lane_hovered;
  bool layer_drop_committed{};
  if (reorder_.layer_node != doc::NodeId{}) {
    const ElementInfo *const source =
        find_element(elements, reorder_.layer_node);
    if (source == nullptr || !orderable_layer(*source) || source->locked) {
      reorder_.layer_node = doc::NodeId{};
      reorder_.layer_nodes.clear();
      reorder_.layer_moved = false;
    } else {
      scratch.layer_drag_label = layer_name(*source);
      if (reorder_.layer_nodes.size() > 1U) {
        scratch.layer_drag_label = std::to_string(reorder_.layer_nodes.size()) + " layers";
      }
      if (scratch.layer_drag_active) {
        frame.lane_editing = ctx.pointer_down();
        std::optional<doc::NodeId> assignment;
        if (source->kind == ElementKind::Media &&
            rows_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
          assignment = clip_lane_under_pointer(p, gesture_y);
          if (assignment.has_value() && *assignment != source->timeline_lane) {
            scratch.layer_assignment_feedback = true;
            if (*assignment != doc::NodeId{}) {
              const auto target = std::find_if(
                  lane_drop_targets.begin(), lane_drop_targets.end(),
                  [&](const LaneDropTarget &candidate) {
                    return !candidate.locked &&
                           candidate.lane == *assignment;
                  });
              if (target != lane_drop_targets.end()) {
                layer_lane_hovered = &*target;
              }
            }
            if (!ctx.pointer_down()) {
              frame.clip_edits.push_back(
                  TimelineClipEdit{source->node, source->timing, assignment});
              layer_drop_committed = true;
            }
          }
        }
        if (!scratch.layer_assignment_feedback) {
          // A layer target owns its complete expanded row band; the upper or
          // lower half chooses before or after it.
          if (const std::optional<LayerLanding> landing = layer_landing(
                  p, *source, ctx.pointer_x(), ctx.pointer_y())) {
            scratch.layer_drop_landing_y = landing->y;
            if (!ctx.pointer_down()) {
              frame.layer_reorders.push_back(TimelineLayerReorder{
                  source->node, landing->destination, landing->placement});
              layer_drop_committed = true;
            }
          }
        }
      }
      if (!ctx.pointer_down()) {
        if (reorder_.layer_moved && !layer_drop_committed) {
          frame.layer_drop = TimelineLayerDrop{
              reorder_.layer_nodes, ctx.pointer_x(), ctx.pointer_y()};
        }
        scratch.released_layer_drag_node = reorder_.layer_node;
        scratch.released_layer_drag_moved = reorder_.layer_moved;
        reorder_.layer_node = doc::NodeId{};
        reorder_.layer_nodes.clear();
        reorder_.layer_start_x = 0.0F;
        reorder_.layer_start_y = 0.0F;
        reorder_.layer_moved = false;
      }
    }
  }

}

void TimelinePanel::resolve_track_drop(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const std::span<const scene::TimelineLane> lanes = p.input.lanes;
  TimelinePanelFrame &frame = p.frame;
  FrameScratch &scratch = p.scratch;
  auto &lane_drop_targets = p.lane_drop_targets;
  auto &lane_drag_active = p.lane_drag_active;

  // Resolve the track drop after every band has been drawn. Reordering is an
  // insertion between peers, never a drop into a peer, so the only destination
  // feedback is the exact landing edge. Crossing several tracks still reports
  // one absolute document edit rather than one adjacent swap per band.
  const LaneDropTarget *lane_drop_hovered = nullptr;
  bool lane_drop_committed{};
  if (reorder_.lane_node != doc::NodeId{}) {
    std::vector<doc::NodeId> stack;
    for (const scene::TimelineLane &lane : lanes) {
      if (!lane.implicit) {
        stack.push_back(lane.id);
      }
    }
    const auto dragged = std::find(stack.begin(), stack.end(), reorder_.lane_node);
    if (dragged == stack.end()) {
      // The track went away under the drag - a peer deleted it, or the scene
      // changed. Nothing to reorder and nothing to report.
      reorder_.lane_node = doc::NodeId{};
      reorder_.layer_nodes.clear();
      reorder_.lane_moved = false;
    } else if (lane_drag_active) {
      scratch.lane_drag_feedback = true;
      frame.lane_editing = ctx.pointer_down();
      for (const LaneDropTarget &target : lane_drop_targets) {
        if (target.bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
          lane_drop_hovered = &target;
        }
      }

      if (lane_drop_hovered != nullptr) {
        const auto destination =
            std::find(stack.begin(), stack.end(), lane_drop_hovered->lane);
        if (destination != stack.end() && destination != dragged) {
          const bool toward_front = destination > dragged;
          scratch.lane_drop_landing_y =
              toward_front ? lane_drop_hovered->bounds.y
                            : lane_drop_hovered->bounds.y +
                                  lane_drop_hovered->bounds.height;
          if (!ctx.pointer_down()) {
            frame.lane_edits.push_back(TimelineLaneEdit{
                TimelineLaneAction::MoveTo, reorder_.lane_node, {},
                lane_drop_hovered->lane});
            lane_drop_committed = true;
          }
        }
      }

      const auto source = std::find_if(
          lane_drop_targets.begin(), lane_drop_targets.end(),
          [&](const LaneDropTarget &target) { return target.lane == reorder_.lane_node; });
      if (source != lane_drop_targets.end()) {
        scratch.lane_drag_label = source->label;
      }
      if (reorder_.layer_nodes.size() > 1U) {
        scratch.lane_drag_label = std::to_string(reorder_.layer_nodes.size()) + " layers";
      }
    }
    if (!ctx.pointer_down()) {
      if (reorder_.lane_moved && !lane_drop_committed) {
        frame.layer_drop = TimelineLayerDrop{
            reorder_.layer_nodes, ctx.pointer_x(), ctx.pointer_y()};
      }
      reorder_.lane_node = doc::NodeId{};
      reorder_.layer_nodes.clear();
      reorder_.lane_start_x = 0.0F;
      reorder_.lane_start_y = 0.0F;
      reorder_.lane_moved = false;
    }
  }

}

void TimelinePanel::draw_drag_feedback(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &bounds = p.bounds;
  const std::span<const ElementInfo> elements = p.input.elements;
  TimelinePanelFrame &frame = p.frame;
  FrameScratch &scratch = p.scratch;
  const gui::Rect &rows_bounds = p.layout.rows;
  const gui::Rect &graph = p.layout.graph;
  const auto frame_to_x = [&](double value) noexcept {
    return time_axis_.x(value, graph.x);
  };
  auto &lane_drop_targets = p.lane_drop_targets;
  auto &clip_drop_hovered = p.clip_drop_hovered;
  auto &layer_lane_hovered = p.layer_lane_hovered;
  frame.drag_readout = drag_readout(p);
  frame.edit_preview = edit_preview(p);

  // An insertion cursor across the rows: where a reorder lands.
  const auto landing_line = [&](float y) {
    ctx.panel(gui::Rect{rows_bounds.x, y - 1.5F, rows_bounds.width, 3.0F},
              ctx.theme().accent);
    ctx.panel(gui::Rect{rows_bounds.x, y - 5.0F, 3.0F, 10.0F},
              ctx.theme().accent);
    ctx.panel(gui::Rect{rows_bounds.x + rows_bounds.width - 3.0F, y - 5.0F,
                        3.0F, 10.0F},
              ctx.theme().accent);
  };

  // Drag feedback is the panel's final visual layer, just as it is in Feed
  // Mapping. The playhead, filmstrips, scroll thumb, and popups therefore
  // cannot cut through a lifted item or obscure its destination.
  if (scratch.layer_drag_active) {
    if (scratch.layer_drop_landing_y.has_value()) {
      landing_line(*scratch.layer_drop_landing_y);
    }
    if (scratch.layer_assignment_feedback) {
      for (const LaneDropTarget &target : lane_drop_targets) {
        if (target.locked) {
          continue;
        }
        const bool hovered = &target == layer_lane_hovered;
        ctx.outline(target.bounds,
                    hovered ? ctx.theme().accent
                            : alpha(ctx.theme().accent, 0.28F),
                    hovered ? 2.0F : 1.0F);
      }
    }
    std::string ghost_label{
        scratch.layer_assignment_feedback ? "MOVE  " : "REORDER  "};
    ghost_label += scratch.layer_drag_label;
    if (layer_lane_hovered != nullptr) {
      ghost_label += "  >  ";
      ghost_label += layer_lane_hovered->label;
    } else if (scratch.layer_assignment_feedback) {
      ghost_label += "  >  OWN TRANSFORM";
    }
    const float ghost_width =
        std::clamp(ctx.font_width(ghost_label) + 26.0F, 128.0F, 300.0F);
    const gui::Rect ghost{ctx.pointer_x() + 12.0F, ctx.pointer_y() + 8.0F,
                          ghost_width, row_height};
    ctx.panel(ghost, ctx.theme().widget_active);
    ctx.panel(gui::Rect{ghost.x, ghost.y, 3.0F, ghost.height},
              ctx.theme().accent);
    ctx.label(gui::Rect{ghost.x + 10.0F, ghost.y,
                        std::max(ghost.width - 14.0F, 0.0F), ghost.height},
              ghost_label, ctx.theme().text);
  }

  if (scratch.lane_drag_feedback) {
    if (scratch.lane_drop_landing_y.has_value()) {
      landing_line(*scratch.lane_drop_landing_y);
    }
    std::string ghost_label{"REORDER  "};
    ghost_label += scratch.lane_drag_label;
    const float ghost_width =
        std::clamp(ctx.font_width(ghost_label) + 26.0F, 128.0F, 240.0F);
    const gui::Rect ghost{ctx.pointer_x() + 12.0F, ctx.pointer_y() + 8.0F,
                          ghost_width, row_height};
    ctx.panel(ghost, ctx.theme().widget_active);
    ctx.panel(gui::Rect{ghost.x, ghost.y, 3.0F, ghost.height},
              ctx.theme().accent);
    ctx.label(gui::Rect{ghost.x + 10.0F, ghost.y,
                        std::max(ghost.width - 14.0F, 0.0F), ghost.height},
              ghost_label, ctx.theme().text);
  }

  // A track bar carried onto another track's band reorders the track, as a
  // header drag does, and lands on the same edge.
  if (track_.drag == ClipDrag::Move && track_.moved &&
      track_.destination != doc::NodeId{}) {
    std::vector<doc::NodeId> stack;
    for (const scene::TimelineLane &lane : p.input.lanes) {
      if (!lane.implicit) {
        stack.push_back(lane.id);
      }
    }
    const auto dragged = std::find(stack.begin(), stack.end(), track_.lane);
    const auto destination =
        std::find(stack.begin(), stack.end(), track_.destination);
    const auto band = std::find_if(
        lane_drop_targets.begin(), lane_drop_targets.end(),
        [&](const LaneDropTarget &target) {
          return target.lane == track_.destination;
        });
    if (dragged != stack.end() && destination != stack.end() &&
        band != lane_drop_targets.end()) {
      landing_line(destination > dragged
                       ? band->bounds.y
                       : band->bounds.y + band->bounds.height);
    }
  }

  if (scratch.clip_move_feedback) {
    if (clip_landing_.has_value()) {
      landing_line(clip_landing_->y);
    }
    // Track outlines mean exactly one thing here: dropping the clip into that
    // track. This is the nesting operation the same outline would falsely
    // suggest during a track reorder.
    for (const LaneDropTarget &target : lane_drop_targets) {
      if (target.locked) {
        continue;
      }
      const bool hovered = &target == clip_drop_hovered;
      ctx.outline(target.bounds,
                  hovered ? ctx.theme().accent
                          : alpha(ctx.theme().accent, 0.28F),
                  hovered ? 2.0F : 1.0F);
    }

    if (clip_.drop_lane.has_value()) {
      const float preview_x = frame_to_x(
          static_cast<double>(clip_.proposed.start));
      const float preview_right = frame_to_x(static_cast<double>(
          clip_.proposed.start + clip_.proposed.duration));
      const float preview_y = std::clamp(
          ctx.pointer_y() - (row_height - 6.0F) * 0.5F,
          rows_bounds.y + 3.0F,
          rows_bounds.y + rows_bounds.height - row_height + 3.0F);
      const gui::Rect preview_bar{
          std::max(preview_x, graph.x), preview_y,
          std::max(std::min(preview_right, graph.x + graph.width) -
                       std::max(preview_x, graph.x),
                   1.0F),
          row_height - 6.0F};
      ctx.panel(preview_bar, alpha(ctx.theme().accent, 0.24F));
      ctx.outline(preview_bar, alpha(ctx.theme().accent, 0.90F), 1.0F);
    }

    std::string ghost_label{"MOVE  "};
    ghost_label += scratch.clip_drag_label;
    if (clip_landing_.has_value()) {
      const ElementInfo *const peer =
          find_element(elements, clip_landing_->destination);
      ghost_label += clip_landing_->placement == TimelineLayerPlacement::Before
                         ? "  >  above "
                         : "  >  below ";
      ghost_label += peer != nullptr ? layer_name(*peer) : "layer";
    } else if (clip_drop_hovered != nullptr) {
      ghost_label += "  >  ";
      ghost_label += clip_drop_hovered->label;
    } else if (clip_.drop_lane.has_value() &&
               *clip_.drop_lane == doc::NodeId{}) {
      const ElementInfo *const dragged =
          find_element(elements, clip_.node);
      if (dragged != nullptr && dragged->timeline_lane != doc::NodeId{}) {
        ghost_label += "  >  OWN TRANSFORM";
      }
    }
    // Where it is going in time, after where it is going in the stack.
    ghost_label += "   " + frame.drag_readout;
    const float ghost_width =
        std::clamp(ctx.font_width(ghost_label) + 26.0F, 128.0F, 520.0F);
    const gui::Rect ghost{ctx.pointer_x() + 12.0F, ctx.pointer_y() + 8.0F,
                          ghost_width, row_height};
    ctx.panel(ghost, ctx.theme().widget_active);
    ctx.panel(gui::Rect{ghost.x, ghost.y, 3.0F, ghost.height},
              ctx.theme().accent);
    ctx.label(gui::Rect{ghost.x + 10.0F, ghost.y,
                        std::max(ghost.width - 14.0F, 0.0F), ghost.height},
              ghost_label, ctx.theme().text);
  }

  // Every other clip, key or marker drag carries the same chip beside the
  // pointer, with only the readout: how far, to where, and what it snapped
  // to. It stays inside the panel, flipping above the pointer at the foot.
  if (!scratch.clip_move_feedback && !frame.drag_readout.empty()) {
    const float chip_width = std::clamp(
        ctx.font_width(frame.drag_readout) + 22.0F, 96.0F, 520.0F);
    float chip_x = ctx.pointer_x() + 14.0F;
    float chip_y = ctx.pointer_y() + 10.0F;
    if (chip_x + chip_width > bounds.x + bounds.width) {
      chip_x = std::max(bounds.x, ctx.pointer_x() - 14.0F - chip_width);
    }
    if (chip_y + row_height > bounds.y + bounds.height) {
      chip_y = ctx.pointer_y() - 10.0F - row_height;
    }
    const gui::Rect chip{chip_x, chip_y, chip_width, row_height};
    ctx.panel(chip, ctx.theme().widget_active);
    ctx.panel(gui::Rect{chip.x, chip.y, 3.0F, chip.height},
              ctx.theme().accent);
    ctx.label(gui::Rect{chip.x + 10.0F, chip.y,
                        std::max(chip.width - 14.0F, 0.0F), chip.height},
              frame.drag_readout, ctx.theme().text);
  }

  // Keep source feedback readable even for very short or partly hidden clips.
  // It remains after release while the authored result reaches the editor.
  if (!clip_.status.empty() && ctx.time_seconds() <= clip_.status_until) {
    frame.clip_status = clip_.status;
    frame.clip_status_error = clip_.status_error;
    const gui::Rect status{bounds.x + 8.0F, bounds.y + bounds.height - 30.0F,
                           std::max(bounds.width - 16.0F, 0.0F), 24.0F};
    ctx.panel(status, ctx.theme().background);
    ctx.outline(status,
                clip_.status_error ? ctx.theme().warning : ctx.theme().accent,
                1.0F);
    ctx.label(gui::Rect{status.x + 8.0F, status.y,
                        std::max(status.width - 16.0F, 0.0F), status.height},
              clip_.status, clip_.status_error ? ctx.theme().warning
                                               : ctx.theme().text);
  }

}

} // namespace mediaserver::editor
