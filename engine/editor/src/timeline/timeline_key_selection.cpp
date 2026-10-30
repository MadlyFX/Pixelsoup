#include "timeline_internal.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

// Timeline key selection: the typed selection of key addresses, dropping
// keys whose target became locked, the shared-delta drag and Alt-duplicate of
// every selected key, and the marquee and lasso that select them.
namespace mediaserver::editor {
using namespace timeline;

bool TimelinePanel::key_selected(const SelectedKey &key) const noexcept {
  return std::find(keys_.selected.begin(), keys_.selected.end(), key) !=
         keys_.selected.end();
}

void TimelinePanel::canonicalize_key_selection() noexcept {
  std::sort(keys_.selected.begin(), keys_.selected.end(),
            [](const SelectedKey &left, const SelectedKey &right) noexcept {
              return std::tie(left.target, left.component, left.parameter,
                              left.frame) <
                     std::tie(right.target, right.component, right.parameter,
                              right.frame);
            });
  keys_.selected.erase(
      std::unique(keys_.selected.begin(), keys_.selected.end()),
      keys_.selected.end());
}

void TimelinePanel::replace_key_selection(
    std::span<const TimelineKeyAddress> selection) {
  std::vector<SelectedKey> next;
  next.reserve(selection.size());
  for (const TimelineKeyAddress &address : selection) {
    next.push_back(
        SelectedKey{address.target, address.property, address.frame, address.parameter});
  }
  keys_.selected = std::move(next);
  canonicalize_key_selection();
  keys_.drag_keys.clear();
  keys_.dragging = false;
  keys_.duplicate = false;
  keys_.drag_delta = 0;
  keys_.snap_guide.reset();
}

std::optional<TimelineKeyClipboardEdit> TimelinePanel::key_clipboard_edit(
    TimelineKeyClipboardAction action) const {
  if (keys_.selected.empty()) {
    return std::nullopt;
  }
  TimelineKeyClipboardEdit edit{};
  edit.action = action;
  if (action == TimelineKeyClipboardAction::Copy) {
    edit.keys.reserve(keys_.selected.size());
    for (const SelectedKey &key : keys_.selected) {
      edit.keys.push_back(
          TimelineKeyAddress{key.target, key.component, key.frame, key.parameter});
    }
    std::sort(edit.keys.begin(), edit.keys.end());
    edit.keys.erase(std::unique(edit.keys.begin(), edit.keys.end()),
                    edit.keys.end());
  }
  return edit;
}

void TimelinePanel::prune_key_selection(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const std::span<const ElementInfo> elements = p.input.elements;
  const std::span<const scene::EffectInstance> effects = p.input.effects;
  const std::span<const output::ProjectionSurface> surfaces = p.input.surfaces;

  const auto target_locked = [&](doc::NodeId target) noexcept {
    if (const ElementInfo *const element = find_element(elements, target)) {
      return element->locked;
    }
    if (const output::ProjectionSurface *const surface =
            find_surface(surfaces, target)) {
      return surface->locked;
    }
    const auto effect = std::find_if(
        effects.begin(), effects.end(),
        [target](const auto &candidate) { return candidate.source == target; });
    if (effect != effects.end()) {
      const ElementInfo *const element = find_element(elements, effect->target);
      return element != nullptr && element->locked;
    }
    return false;
  };
  keys_.selected.erase(std::remove_if(keys_.selected.begin(),
                                      keys_.selected.end(),
                                      [&](const SelectedKey &key) {
                                        return target_locked(key.target);
                                      }),
                       keys_.selected.end());
  if (graph_->active().has_value() && target_locked(graph_->active()->target)) {
    graph_->set_active(std::nullopt);
    key_tools_.value_field.cancel(ctx);
  }
  keys_.drag_keys.erase(std::remove_if(keys_.drag_keys.begin(), keys_.drag_keys.end(),
                                  [&](const SelectedKey &key) {
                                    return target_locked(key.target);
                                  }),
                   keys_.drag_keys.end());
  if (keys_.drag_keys.empty()) {
    keys_.dragging = false;
    keys_.duplicate = false;
    keys_.drag_delta = 0;
    keys_.snap_guide.reset();
  }

}

std::vector<TimelineKeyAddress> TimelinePanel::selected_addresses() const {
  std::vector<TimelineKeyAddress> addresses;
  addresses.reserve(keys_.selected.size());
  for (const SelectedKey &key : keys_.selected) {
    addresses.push_back(
        TimelineKeyAddress{key.target, key.component, key.frame, key.parameter});
  }
  std::sort(addresses.begin(), addresses.end());
  addresses.erase(std::unique(addresses.begin(), addresses.end()),
                  addresses.end());
  return addresses;
}

void TimelinePanel::track_key_drag(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &rows_bounds = p.layout.rows;
  const gui::Rect &graph = p.layout.graph;

  // A selected key drag follows time even when the pointer wanders into
  // another lane. Every selected diamond shares one delta, and any one of
  // them may magnetise to the playhead, range edges, or an unselected key.
  keys_.snap_guide.reset();
  update_key_drag(p);

  if (keys_.marquee_tracking && ctx.pointer_down()) {
    const float travel = std::fabs(ctx.pointer_x() - keys_.marquee_start_x) +
                         std::fabs(ctx.pointer_y() - keys_.marquee_start_y);
    keys_.marquee_active = keys_.marquee_active || travel > marquee_threshold;
    if (keys_.lasso_mode) {
      const gui::UiPoint point{
          std::clamp(ctx.pointer_x(), graph.x, graph.right()),
          std::clamp(ctx.pointer_y(), rows_bounds.y,
                     rows_bounds.bottom())};
      if (keys_.lasso_points.empty()) {
        keys_.lasso_points.push_back(
            gui::UiPoint{keys_.marquee_start_x, keys_.marquee_start_y});
      }
      const gui::UiPoint &previous = keys_.lasso_points.back();
      const float dx = point.x - previous.x;
      const float dy = point.y - previous.y;
      if (dx * dx + dy * dy >= 9.0F) {
        keys_.lasso_points.push_back(point);
      }
    }
  }

}

void TimelinePanel::update_key_drag(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const EditorTimeline &timeline = p.input.timeline;
  const std::span<const scene::ChannelKeyframes> keyframes = p.input.keyframes;
  const std::span<const scene::TimelineMarker> markers = p.input.markers;
  const gui::Rect &graph = p.layout.graph;
  const auto x_to_frame = [&](float x) noexcept {
    return time_axis_.frame(x, graph.x);
  };
  auto &gesture_x = p.gesture_x;
  auto &first = p.first;
  auto &last = p.last;
  auto &first_i = p.first_i;
  auto &last_i = p.last_i;

  if (!keys_.dragging) {
    return;
  }
  const core::i64 pointer_frame =
      signed_frame(rounded_frame(x_to_frame(gesture_x), first, last));
  core::i64 proposed_delta = pointer_frame - keys_.drag_anchor_frame;

  core::i64 earliest = keys_.drag_anchor_frame;
  core::i64 latest = keys_.drag_anchor_frame;
  for (const SelectedKey &key : keys_.drag_keys) {
    earliest = std::min(earliest, key.frame);
    latest = std::max(latest, key.frame);
  }
  proposed_delta =
      std::clamp(proposed_delta, first_i - earliest, last_i - latest);

  if (snapping_ && !keys_.duplicate && !ctx.alt_held() &&
      !keys_.drag_keys.empty()) {
    float best_distance = snap_distance + 1.0F;
    core::i64 best_adjustment = 0;
    std::optional<core::i64> best_target;
    const auto consider = [&](core::i64 moving, core::i64 target) noexcept {
      const core::i64 adjustment = target - moving;
      const float distance =
          std::fabs(static_cast<float>(adjustment) * time_axis_.pixels_per_frame);
      if (distance <= snap_distance && distance < best_distance) {
        best_distance = distance;
        best_adjustment = adjustment;
        best_target = target;
      }
    };

    for (const SelectedKey &moving : keys_.drag_keys) {
      const core::i64 proposed = moving.frame + proposed_delta;
      consider(proposed, signed_frame(timeline.show_frame));
      consider(proposed, first_i);
      consider(proposed, last_i);
      for (const scene::TimelineMarker &marker : markers) {
        consider(proposed, marker.frame);
      }
      for (const scene::ChannelKeyframes &channel : keyframes) {
        for (const core::i64 candidate : channel.frames) {
          const SelectedKey identity{channel.target, channel.component,
                                     candidate};
          if (std::find(keys_.drag_keys.begin(), keys_.drag_keys.end(), identity) ==
              keys_.drag_keys.end()) {
            consider(proposed, candidate);
          }
        }
      }
    }
    if (best_target.has_value()) {
      proposed_delta = std::clamp(proposed_delta + best_adjustment,
                                  first_i - earliest, last_i - latest);
      keys_.snap_guide = best_target;
    }
  }
  keys_.drag_delta = proposed_delta;
}

void TimelinePanel::finish_marquee(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  FrameScratch &scratch = p.scratch;
  const gui::Rect &rows_bounds = p.layout.rows;
  const gui::Rect &graph = p.layout.graph;

  if (keys_.marquee_tracking && keys_.marquee_active && ctx.pointer_down()) {
    ctx.push_clip(rows_bounds);
    if (keys_.lasso_mode) {
      for (std::size_t index = 1U; index < keys_.lasso_points.size(); ++index) {
        const gui::UiPoint &a = keys_.lasso_points[index - 1U];
        const gui::UiPoint &b = keys_.lasso_points[index];
        ctx.draw_list().add_line(a.x, a.y, b.x, b.y, 1.0F, ctx.theme().accent);
      }
      if (keys_.lasso_points.size() >= 2U) {
        const gui::UiPoint &first_point = keys_.lasso_points.front();
        const gui::UiPoint &last_point = keys_.lasso_points.back();
        ctx.draw_list().add_line(last_point.x, last_point.y, first_point.x,
                                 first_point.y, 1.0F,
                                 alpha(ctx.theme().accent, 0.65F));
      }
    } else {
      const float left =
          std::max(std::min(keys_.marquee_start_x, ctx.pointer_x()), graph.x);
      const float right = std::min(std::max(keys_.marquee_start_x, ctx.pointer_x()),
                                   graph.right());
      const float top =
          std::max(std::min(keys_.marquee_start_y, ctx.pointer_y()), rows_bounds.y);
      const float bottom = std::min(std::max(keys_.marquee_start_y, ctx.pointer_y()),
                                    rows_bounds.bottom());
      const gui::Rect marquee{left, top, std::max(right - left, 0.0F),
                              std::max(bottom - top, 0.0F)};
      ctx.panel(marquee, alpha(ctx.theme().accent, 0.15F));
      ctx.draw_list().add_line(marquee.x, marquee.y, marquee.right(),
                               marquee.y, 1.0F, ctx.theme().accent);
      ctx.draw_list().add_line(
          marquee.x, marquee.bottom(), marquee.right(),
          marquee.bottom(), 1.0F, ctx.theme().accent);
      ctx.draw_list().add_line(marquee.x, marquee.y, marquee.x,
                               marquee.bottom(), 1.0F,
                               ctx.theme().accent);
      ctx.draw_list().add_line(
          marquee.right(), marquee.y, marquee.right(),
          marquee.bottom(), 1.0F, ctx.theme().accent);
    }
    ctx.pop_clip();
  }

  if (keys_.marquee_tracking && !ctx.pointer_down()) {
    if (keys_.marquee_active) {
      const float left = std::min(keys_.marquee_start_x, ctx.pointer_x());
      const float right = std::max(keys_.marquee_start_x, ctx.pointer_x());
      const float top = std::min(keys_.marquee_start_y, ctx.pointer_y());
      const float bottom = std::max(keys_.marquee_start_y, ctx.pointer_y());
      keys_.selected = keys_.marquee_base;
      if (!keys_.marquee_shift && !keys_.marquee_ctrl) {
        keys_.selected.clear();
      }
      for (const FrameScratch::VisibleKeyPoint &point : scratch.visible_keys) {
        const bool hit = keys_.lasso_mode
                             ? point_in_polygon(point.x, point.y, keys_.lasso_points)
                             : (point.x >= left && point.x <= right &&
                                point.y >= top && point.y <= bottom);
        if (!hit) {
          continue;
        }
        const auto found =
            std::find(keys_.selected.begin(), keys_.selected.end(), point.key);
        if (keys_.marquee_ctrl) {
          if (found == keys_.selected.end()) {
            keys_.selected.push_back(point.key);
          } else {
            keys_.selected.erase(found);
          }
        } else if (found == keys_.selected.end()) {
          keys_.selected.push_back(point.key);
        }
      }
      canonicalize_key_selection();
    }
    keys_.marquee_tracking = false;
    keys_.marquee_active = false;
    keys_.marquee_base.clear();
    keys_.lasso_points.clear();
  }

}

void TimelinePanel::commit_key_drag(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  TimelinePanelFrame &frame = p.frame;

  // Commit the complete selection once, on release. Moving positive time is
  // authored from the last key backwards (and negative time first-key
  // forwards), preventing one destination from hiding another source key in
  // the same channel while the batch is applied.
  if (keys_.dragging && !ctx.pointer_down()) {
    update_key_drag(p);
    if (keys_.drag_delta != 0) {
      TimelineKeyBatchEdit batch{};
      batch.action = keys_.duplicate ? TimelineKeyBatchAction::Duplicate
                                     : TimelineKeyBatchAction::Move;
      batch.delta = keys_.drag_delta;
      batch.keys.reserve(keys_.drag_keys.size());
      for (const SelectedKey &key : keys_.drag_keys) {
        batch.keys.push_back(
            TimelineKeyAddress{key.target, key.component, key.frame, key.parameter});
      }
      std::sort(batch.keys.begin(), batch.keys.end());
      batch.keys.erase(std::unique(batch.keys.begin(), batch.keys.end()),
                       batch.keys.end());
      frame.key_batch = std::move(batch);
    }
    keys_.dragging = false;
    keys_.duplicate = false;
    keys_.drag_keys.clear();
    keys_.drag_delta = 0;
    keys_.snap_guide.reset();
  }

}

} // namespace mediaserver::editor
