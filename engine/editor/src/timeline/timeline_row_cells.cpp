#include "timeline_internal.hpp"

#include <mediaserver/editor/timeline_filmstrip_texture_cache.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

// The Timeline's rows. draw_rows() lays each visible row out, draws its list
// cell and its lane cell through the renderer for its kind - track header,
// layer (with its clip bar), Properties foldout, property group or channel -
// and lets the row label and then the lane take a press, in that order.
namespace mediaserver::editor {
using namespace timeline;

void TimelinePanel::draw_rows(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const std::span<const ElementInfo> elements = p.input.elements;
  const gui::Rect &rows_bounds = p.layout.rows;
  const gui::Rect &graph = p.layout.graph;
  std::vector<Row> &rows = p.built.rows;
  auto &anchor = p.anchor;

  std::vector<std::size_t> track_numbers(rows.size(), 0U);
  {
    std::size_t number = 0U;
    for (std::size_t index = 0U; index < rows.size(); ++index) {
      if (rows[index].kind == Row::Kind::LaneHeader) {
        track_numbers[index] = ++number;
      }
    }
  }
  // Lay every visible row out and gather its lane's pointer targets before
  // any row draws, so one owner can be named for the press and the hover.
  p.hits.clear();
  std::vector<RowContext> visible;
  visible.reserve(64U);
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const Row &row = rows[index];
    const float y = rows_bounds.y + row.top - row_scroll_;
    if (y + row.height <= rows_bounds.y ||
        y >= rows_bounds.y + rows_bounds.height) {
      continue;
    }
    RowContext &rc = visible.emplace_back(p, row, index);
    rc.y = y;
    rc.track_number = track_numbers[index];
    layout_row(rc);
    collect_list_hits(rc);
    if (!graph_mode_) {
      collect_row_hits(rc);
    }
  }
  if (graph_mode_) {
    collect_graph(p, visible);
  }
  if (ctx.pointer_pressed()) {
    if (const HitTarget *const owner =
            p.hits.owner(ctx.pointer_press_x(), ctx.pointer_press_y())) {
      p.press_owner = *owner;
    }
  }
  if (!ctx.pointer_blocked()) {
    if (const HitTarget *const owner =
            p.hits.owner(ctx.pointer_x(), ctx.pointer_y())) {
      p.hover_owner = *owner;
    }
  }

  ctx.push_clip(rows_bounds);
  for (RowContext &rc : visible) {
    const Row &row = rc.row;
    const float y = rc.y;
    begin_row(rc);
    switch (row.kind) {
    case Row::Kind::LaneHeader:
      track_header_list_cell(rc);
      break;
    case Row::Kind::Layer:
      layer_list_cell(rc);
      break;
    case Row::Kind::PropertiesHeader:
      properties_header_list_cell(rc);
      break;
    case Row::Kind::Group:
      property_group_list_cell(rc);
      break;
    case Row::Kind::Component:
    case Row::Kind::Property:
      channel_list_cell(rc);
      break;
    }
    if (!row_label_press(rc)) {
      continue;
    }
    // In Curves mode the graph replaces every row's lane.
    if (graph_mode_) {
      note_row_menu_press(rc);
      continue;
    }
    ctx.push_clip(gui::Rect{graph.x, y, graph.width, row.height});
    switch (row.kind) {
    case Row::Kind::Layer:
      layer_lane_cell(rc);
      break;
    case Row::Kind::Group:
      property_group_lane_cell(rc);
      break;
    case Row::Kind::Component:
    case Row::Kind::Property:
      channel_lane_cell(rc);
      break;
    case Row::Kind::LaneHeader:
    case Row::Kind::PropertiesHeader:
      summary_lane_cell(rc, true);
      break;
    }
    lane_press(rc);
    ctx.pop_clip();
  }
  if (graph_mode_) {
    draw_graph(p);
  }
  // A new track is made where the track list ends, as in every editor;
  // the + on the ruler is the same action for when the list is long.
  {
    const float button_y = rows_bounds.y + rows_extent(rows) - row_scroll_;
    if (button_y < rows_bounds.y + rows_bounds.height &&
        button_y + row_height > rows_bounds.y) {
      const gui::Rect button{rows_bounds.x + 8.0F, button_y + 3.0F,
                             std::min(120.0F, p.layout.list_width - 16.0F),
                             row_height - 6.0F};
      const gui::Interaction made =
          ctx.grab(mix(id_, "new track row"), button);
      ctx.panel(button, made.hovered ? ctx.theme().widget_hover
                                     : alpha(ctx.theme().widget, 0.6F));
      gui::draw_icon(ctx.draw_list(), gui::Icon::Plus, button.x + 10.0F,
                     button.y + button.height * 0.5F,
                     made.hovered ? ctx.theme().text : ctx.theme().text_muted);
      ctx.label(gui::Rect{button.x + 20.0F, button.y,
                          std::max(button.width - 22.0F, 0.0F), button.height},
                "New track",
                made.hovered ? ctx.theme().text : ctx.theme().text_muted);
      ctx.tooltip(button, "Add a track at the bottom of the list");
      if (made.clicked) {
        p.frame.lane_edits.push_back(
            TimelineLaneEdit{TimelineLaneAction::Create, {}, {}});
      }
    }
  }
  // A row another panel pointed at is outlined, fading.
  if (ctx.time_seconds() < reveal_until_) {
    for (const RowContext &rc : visible) {
      if ((rc.row.kind == Row::Kind::Layer ||
           rc.row.kind == Row::Kind::LaneHeader) &&
          rc.row.node == reveal_node_) {
        const float left = static_cast<float>(reveal_until_ - ctx.time_seconds());
        gui::UiColor color = ctx.theme().accent;
        color.a = std::clamp(left / 1.5F, 0.0F, 1.0F);
        draw_border(ctx,
                    gui::Rect{rows_bounds.x, rc.y,
                              graph.x + graph.width - rows_bounds.x,
                              rc.row.height},
                    color, 2.0F);
      }
    }
  }

  ctx.pop_clip();
  rename_.end_frame(ctx);

  // F2 renames the selected layer or track while the timeline has the
  // operator's attention.
  if (timeline_focused_ && !ctx.typing() && ctx.key_pressed(gui::Key::Rename)) {
    for (const Row &row : rows) {
      if (row.kind == Row::Kind::LaneHeader && row.node == selected_lane_ &&
          !row.locked) {
        rename_.begin(ctx, row.node.value(), row.label);
        break;
      }
      if (row.kind == Row::Kind::Layer && !row.surface && row.node == anchor &&
          !row.locked) {
        const ElementInfo *const element = find_element(elements, row.node);
        if (element != nullptr && !element->linked &&
            element->kind != ElementKind::Environment) {
          rename_.begin(ctx, row.node.value(), row.label);
        }
        break;
      }
    }
  }

}

// Where the row and what its lane shows will be drawn, worked out before any
// row draws so its pointer targets can be gathered first: the row's band, a
// placed clip's bar and fade knobs as a gesture or pending edit shows them,
// and a channel's keys as a key drag moves them and Curves mode raises them.
void TimelinePanel::layout_row(RowContext &rc) {
  Pass &p = rc.p;
  const std::span<const scene::ChannelKeyframes> keyframes = p.input.keyframes;
  const std::span<const ElementInfo> elements = p.input.elements;
  const gui::Rect &rows_bounds = p.layout.rows;
  const gui::Rect &graph = p.layout.graph;
  const auto frame_to_x = [&](double value) noexcept {
    return time_axis_.x(value, graph.x);
  };
  const Row &row = rc.row;
  const float y = rc.y;

  rc.centre_y = y + row.height * 0.5F;
  rc.interaction_y = std::max(y, rows_bounds.y);
  const float interaction_bottom =
      std::min(y + row.height, rows_bounds.y + rows_bounds.height);
  rc.interaction_height =
      std::max(interaction_bottom - rc.interaction_y, 0.0F);
  rc.controls =
      channel_controls(graph.x, rc.interaction_y, rc.interaction_height);
  // What the row's channels hold at the show frame, for its value cell and
  // its key diamond: their keys evaluated there, or where a channel has none
  // its base value, the only reason to look its element up.
  if (const std::optional<RowChannels> channels = row_channels(row)) {
    std::optional<const ElementInfo *> element;
    std::optional<const output::ProjectionSurface *> surface;
    for (core::u8 offset = 0U;
         offset < channels->count && offset < rc.values.size(); ++offset) {
      const scene::PropertyChannel channel = channels->at(offset);
      rc.keys[offset] = channel_for(keyframes, channels->target, channel);
      const std::optional<double> sampled =
          sample_channel_at(rc.keys[offset], p.input.timeline.show_frame);
      if (sampled.has_value() && std::isfinite(*sampled)) {
        rc.values[offset] = *sampled;
      } else if (row.kind == Row::Kind::Property) {
        rc.values[offset] = row.property_value;
      } else if (row.surface) {
        if (!surface.has_value()) {
          surface = find_surface(p.input.surfaces, row.node);
        }
        rc.values[offset] = *surface != nullptr
                                ? surface_component_value(**surface, channel)
                                : 0.0;
      } else {
        if (!element.has_value()) {
          element = find_element(elements, row.node);
        }
        rc.values[offset] =
            *element != nullptr ? component_value(**element, channel) : 0.0;
      }
    }
  }
  if (row.kind == Row::Kind::Component || row.kind == Row::Kind::Property) {
    rc.row_channel_identity = ChannelIdentity{
        row.kind == Row::Kind::Property ? row.property_target : row.node,
        row.kind == Row::Kind::Property
            ? row.property
            : group_channel(row.group, row.component)};
  }

  if (row.kind == Row::Kind::Layer && !row.surface) {
    const ElementInfo *const element = find_element(elements, row.node);
    if (element != nullptr && element->kind == ElementKind::Media &&
        element->timing.placed()) {
      rc.clip_element = element;
      rc.clip_timing =
          clip_.drag != ClipDrag::None && clip_.node == row.node
              ? clip_.proposed
              : (clip_.pending_edit.has_value() &&
                         clip_.pending_edit->clip == row.node
                     ? clip_.pending_edit->timing
                     : element->timing);
      const scene::ClipTiming &timing = rc.clip_timing;
      const float x0 = frame_to_x(static_cast<double>(timing.start));
      const float x1 =
          frame_to_x(static_cast<double>(timing.start + timing.duration));
      rc.clip_bar =
          gui::Rect{x0, y + 3.0F, std::max(x1 - x0, 2.0F), row.height - 6.0F};
      rc.clip_fade_in_x =
          frame_to_x(static_cast<double>(timing.start + timing.fade_in));
      rc.clip_fade_out_x = frame_to_x(static_cast<double>(
          timing.start + timing.duration - timing.fade_out));
    }
  }

  if (!rc.row_channel_identity.has_value()) {
    return;
  }
  const ChannelIdentity identity = *rc.row_channel_identity;
  rc.channel = identity.property;
  rc.component_channel = graph_->visible(identity) ? rc.keys[0U] : nullptr;
  rc.component_frames = rc.component_channel != nullptr
                            ? &rc.component_channel->frames
                            : nullptr;
  if (rc.component_channel == nullptr) {
    return;
  }
  const scene::ChannelKeyframes &channel = *rc.component_channel;
  rc.component_x.reserve(channel.frames.size());
  rc.component_y.reserve(channel.frames.size());
  for (std::size_t key_index = 0U; key_index < channel.frames.size();
       ++key_index) {
    const core::i64 key_frame = channel.frames[key_index];
    const SelectedKey key{identity.target, identity.property, key_frame};
    const bool moving =
        keys_.dragging && std::find(keys_.drag_keys.begin(),
                                    keys_.drag_keys.end(),
                                    key) != keys_.drag_keys.end();
    const core::i64 shown_frame =
        moving && !keys_.duplicate ? key_frame + keys_.drag_delta : key_frame;
    rc.component_x.push_back(frame_to_x(static_cast<double>(shown_frame)));
    rc.component_y.push_back(rc.centre_y);
  }
}

// The row's lane targets: the lane itself, the channel's keys, and unless the
// row is locked its clip's body, trim edges and fade knobs. Which of them a
// press or the hover belongs to is settled once every row has registered.
void TimelinePanel::collect_row_hits(RowContext &rc) {
  Pass &p = rc.p;
  const gui::Rect &graph = p.layout.graph;
  const Row &row = rc.row;
  const core::u64 identity = row_identity(row);
  const gui::Rect lane{graph.x, rc.interaction_y, graph.width,
                       rc.interaction_height};
  p.hits.add(HitTarget{mix(id_, "lane", identity), lane, HitPriority::Lane,
                       lane, row_part_tag(rc.index, RowPart::Lane)});

  for (std::size_t key = 0U; key < rc.component_x.size(); ++key) {
    const float x = rc.component_x[key];
    const float y = rc.component_y[key];
    HitTarget target{
        mix(mix(id_, "key", identity), "frame",
            static_cast<core::u64>((*rc.component_frames)[key])),
        gui::Rect{x - key_hit_radius, y - key_hit_radius,
                  key_hit_radius * 2.0F, key_hit_radius * 2.0F},
        HitPriority::Key, gui::Rect{x, y, 0.0F, 0.0F},
        row_part_tag(rc.index, RowPart::Key, key)};
    target.reach = key_hit_radius;
    p.hits.add(target);
  }

  if (row.locked || rc.clip_element == nullptr) {
    return;
  }
  const gui::Rect &bar = rc.clip_bar;
  p.hits.add(HitTarget{mix(id_, "clip body", identity), bar,
                       HitPriority::ClipBody, bar,
                       row_part_tag(rc.index, RowPart::ClipBody)});
  if (edit_tool_ != TimelineEditTool::Standard) {
    return;
  }
  const float grab = edge_grab_width(bar.width);
  p.hits.add(HitTarget{mix(id_, "trim in", identity),
                       gui::Rect{bar.x - trim_outer_reach, bar.y,
                                 grab + trim_outer_reach, bar.height},
                       HitPriority::TrimEdge,
                       gui::Rect{bar.x, bar.y, 0.0F, bar.height},
                       row_part_tag(rc.index, RowPart::TrimIn)});
  p.hits.add(HitTarget{mix(id_, "trim out", identity),
                       gui::Rect{bar.x + bar.width - grab, bar.y,
                                 grab + trim_outer_reach, bar.height},
                       HitPriority::TrimEdge,
                       gui::Rect{bar.x + bar.width, bar.y, 0.0F, bar.height},
                       row_part_tag(rc.index, RowPart::TrimOut)});
  // The knobs ride the bar's top edge; each answers within a disc around
  // its centre, but no deeper into the bar than fade_knob_depth: a knob sits
  // on its trim edge whenever the clip has no fade, and the rest of that
  // end is the edge's to trim.
  const auto knob = [&](float x, std::string_view name, RowPart part) {
    const float centre_y = bar.y + 1.0F;
    const float top = centre_y - fade_handle_reach;
    HitTarget target{mix(id_, name, identity),
                     gui::Rect{x - fade_handle_reach, top,
                               fade_handle_reach * 2.0F,
                               bar.y + fade_knob_depth - top},
                     HitPriority::FadeKnob,
                     gui::Rect{x, centre_y, 0.0F, 0.0F},
                     row_part_tag(rc.index, part)};
    target.reach = fade_handle_reach;
    p.hits.add(target);
  };
  knob(rc.clip_fade_in_x, "fade in", RowPart::FadeIn);
  knob(rc.clip_fade_out_x, "fade out", RowPart::FadeOut);
}

// The list cell's own targets: a channel or group row's steps to its
// previous and next key. They sit beside the lane, which a key's disc at the
// lane's very edge reaches into; the nearer mark owns the point.
void TimelinePanel::collect_list_hits(RowContext &rc) {
  if (!row_channels(rc.row).has_value() || rc.interaction_height <= 0.0F) {
    return;
  }
  const core::u64 identity = row_identity(rc.row);
  const auto add = [&](const gui::Rect &bounds, std::string_view name,
                       RowPart part) {
    rc.p.hits.add(HitTarget{
        mix(id_, name, identity), bounds, HitPriority::Key,
        gui::Rect{bounds.x + bounds.width * 0.5F, rc.centre_y, 0.0F, 0.0F},
        row_part_tag(rc.index, part)});
  };
  add(rc.controls.previous, "previous key", RowPart::PreviousKey);
  add(rc.controls.next, "next key", RowPart::NextKey);
}

// The row's background, rule, twirl and the geometry its cells share.
void TimelinePanel::begin_row(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const std::span<const doc::NodeId> selection = p.input.selection;
  const doc::NodeId selected_surface = p.input.selected_surface;
  const float &list_width = p.layout.list_width;
  const gui::Rect &rows_bounds = p.layout.rows;
  const gui::Rect &graph = p.layout.graph;
  const Row &row = rc.row;
  auto &y = rc.y;
  auto &centre_y = rc.centre_y;
  auto &is_selected = rc.is_selected;
  auto &layer_like = rc.layer_like;
  auto &row_hovered = rc.row_hovered;
  auto &background = rc.background;
  auto &row_group = rc.row_group;
  auto &open = rc.open;
  auto &content_x = rc.content_x;
  auto &lock_x = rc.lock_x;

  if (row.kind == Row::Kind::Layer && !row.surface) {
    const float top = std::max(y, rows_bounds.y);
    const float bottom = std::min(y + row.height, rows_bounds.y + rows_bounds.height);
    replacement_targets_.push_back({row.node,
        {rows_bounds.x, top, rows_bounds.width, bottom - top}});
  }
  rc.is_selected =
      row.kind == Row::Kind::LaneHeader
          ? row.node == selected_lane_
          : (row.surface ? row.node == selected_surface
                         : selected(row.node, selection));
  rc.layer_like =
      row.kind == Row::Kind::Layer || row.kind == Row::Kind::LaneHeader;
  const gui::Theme &row_theme = ctx.theme();
  rc.row_hovered =
      !ctx.pointer_blocked() &&
      rows_bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      ctx.pointer_y() >= y && ctx.pointer_y() < y + row.height;
  if (is_selected && layer_like) {
    background = row_theme.selection;
  } else if (is_selected) {
    // The selected layer's own properties: tinted, not flooded, so the
    // selection still reads as one row.
    background = mix_colors(row_theme.track, row_theme.selection, 0.35F);
  } else if (row.kind == Row::Kind::LaneHeader) {
    background = row_theme.header;
  } else if (layer_like) {
    background = row_theme.row;
  } else {
    // Property rows sit a shade below the layer they open from, so the eye
    // reads them as that layer's contents rather than as more layers.
    background = row_theme.track;
  }
  ctx.panel(gui::Rect{rows_bounds.x, y, rows_bounds.width, row.height},
            background);
  if (!is_selected && row_hovered && layer_like) {
    ctx.panel(gui::Rect{rows_bounds.x, y, list_width, row.height},
              alpha(row_theme.widget_hover, 0.6F));
  }
  // Every layer is ruled off across the whole panel; property rows only
  // across the lanes, so a twirled-open layer still reads as one block.
  ctx.panel(gui::Rect{layer_like ? rows_bounds.x : graph.x,
                      y + row.height - 1.0F,
                      layer_like ? rows_bounds.width : graph.width, 1.0F},
            row_theme.divider);
  if (layer_like && !row.surface) {
    // The row's own colour down its left edge: the kind for a layer, the
    // track's for a track. Selection takes the accent.
    ctx.panel(gui::Rect{rows_bounds.x, y, 3.0F, row.height - 1.0F},
              is_selected ? row_theme.accent
                          : alpha(row.color, row.locked ? 0.35F : 0.85F));
  }

  const float tree_x = rows_bounds.x + list_metrics::toggle + 6.0F;
  const float twirl_x =
      tree_x + static_cast<float>(row.indent) * list_metrics::indent;
  draw_indent_guides(ctx, tree_x, y, row.height, row.indent,
                     alpha(row_theme.border, 0.9F));
  rc.row_group = group_id(row.node, row.group);
  rc.open = row.kind == Row::Kind::LaneHeader
      ? (!search_.empty() || !rows_state_.collapsed_lanes.contains(row.node))
      : row.hierarchy
      ? (!search_.empty() || rows_state_.expanded_hierarchies.contains(row.node))
      : ((row.kind == Row::Kind::Layer ||
          row.kind == Row::Kind::PropertiesHeader)
             ? rows_state_.expanded_layers.contains(row.node.value())
             : (row.kind == Row::Kind::Group &&
                rows_state_.expanded_groups.contains(row_group)));
  if (row.expandable) {
    const bool twirl_hovered =
        row_hovered && ctx.pointer_x() >= twirl_x &&
        ctx.pointer_x() < twirl_x + list_metrics::twirl_width;
    draw_twirl(ctx, twirl_x + list_metrics::twirl_width * 0.5F, centre_y,
               open, twirl_hovered ? row_theme.text : row_theme.text_muted);
  }
  // Left of this a press opens or closes the row; right of it, it means
  // the row itself.
  rc.content_x = twirl_x + list_metrics::twirl_width + 2.0F;
  rc.name_x = content_x;
  rc.name_bounds = gui::Rect{content_x, y,
                        std::max(graph.x - content_x - 48.0F, 0.0F),
                        row.height};
  rc.lock_x = graph.x - list_metrics::toggle - 4.0F;
  rc.eye_bounds = gui::Rect{rows_bounds.x + 4.0F, y, list_metrics::toggle,
                             row.height};
  rc.lock_bounds = gui::Rect{lock_x, y, list_metrics::toggle, row.height};

}

// A track: its number, name and mute and lock.
void TimelinePanel::track_header_list_cell(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  TimelinePanelFrame &frame = p.frame;
  const Row &row = rc.row;
  auto &track_number = rc.track_number;
  auto &y = rc.y;
  auto &centre_y = rc.centre_y;
  auto &row_hovered = rc.row_hovered;
  auto &content_x = rc.content_x;
  auto &name_x = rc.name_x;
  auto &name_bounds = rc.name_bounds;
  auto &lock_x = rc.lock_x;
  auto &eye_bounds = rc.eye_bounds;
  auto &lock_bounds = rc.lock_bounds;
  const gui::Theme &row_theme = ctx.theme();

  // Tracks are numbered top-down, the way they are called across a
  // stage: "kill track two".
  const std::string number = std::to_string(track_number);
  const float badge_width = std::max(18.0F, ctx.font_width(number) + 8.0F);
  const gui::Rect badge{content_x + 2.0F, centre_y - 8.0F, badge_width,
                        16.0F};
  ctx.panel(badge, row.muted ? alpha(row_theme.text_muted, 0.6F)
                             : row.color);
  ctx.label(gui::Rect{badge.x + std::max((badge.width -
                                          ctx.font_width(number)) *
                                             0.5F,
                                         0.0F),
                      badge.y, badge.width, badge.height},
            number, row_theme.background);
  name_x = badge.x + badge.width + 8.0F;
  // The track's working switches, just inside its padlock: where Insert
  // and Overwrite land, whether it keeps time with ripples elsewhere, and
  // whether the Stage shows it alone. Each reads its state from across a
  // stage when lit.
  constexpr float chip_height = 16.0F;
  const gui::Rect solo_chip{lock_x - 4.0F - 18.0F, centre_y - 8.0F, 18.0F,
                            chip_height};
  const gui::Rect sync_chip{solo_chip.x - 2.0F - 38.0F, solo_chip.y, 38.0F,
                            chip_height};
  const gui::Rect target_chip{sync_chip.x - 2.0F - 30.0F, solo_chip.y, 30.0F,
                              chip_height};
  name_bounds = gui::Rect{name_x, y,
                          std::max(target_chip.x - name_x - 4.0F, 0.0F),
                          row.height};
  const bool targeted =
      targeted_lane_.has_value() && *targeted_lane_ == row.node;
  if (chip_toggle(ctx, mix(id_, "track target", row.node.value()),
                  target_chip, "TGT", targeted, row_theme.accent,
                  targeted ? "Insert and Overwrite land on this track. Click "
                             "to stop aiming them here."
                           : "Aim Insert and Overwrite at this track, "
                             "whatever is selected.",
                  !row.locked || targeted)) {
    if (targeted) {
      targeted_lane_.reset();
    } else {
      targeted_lane_ = row.node;
    }
  }
  if (chip_toggle(ctx, mix(id_, "track sync lock", row.node.value()),
                  sync_chip, "SYNC", row.sync_locked, row_theme.good,
                  row.sync_locked
                      ? "Sync locked: a ripple on another track moves this "
                        "track's later clips by the same amount. Click to "
                        "let it stay put."
                      : "Sync lock: keep this track in time with ripple "
                        "edits made on other tracks.",
                  !row.locked)) {
    frame.lane_edits.push_back(TimelineLaneEdit{
        TimelineLaneAction::ToggleSyncLock, row.node, {}});
  }
  solo_toggle(rc, solo_chip);
  if (rename_.editing(row.node.value())) {
    // While a track's name is open the shell holds one lane gesture and
    // keeps the keyboard's shortcuts away from the field.
    frame.lane_editing = true;
    if (auto renamed = rename_.draw(
            ctx, gui::Rect{name_bounds.x - 4.0F, y + 2.0F,
                           name_bounds.width + 4.0F, row.height - 4.0F})) {
      frame.lane_edits.push_back(TimelineLaneEdit{
          TimelineLaneAction::Rename, row.node, std::move(*renamed)});
    }
  } else {
    ctx.label(name_bounds, row.label,
              row.muted ? row_theme.text_muted : row_theme.text);
  }
  // Muting a track hides it, so its switch is the eye every layer has;
  // the toggles double as the state readout, so no badge repeats them.
  if (row_flag_toggle(ctx, mix(id_, "track mute", row.node.value()),
                      eye_bounds, RowFlag::Visible, !row.muted,
                      row_hovered)) {
    frame.lane_edits.push_back(TimelineLaneEdit{
        TimelineLaneAction::ToggleMute, row.node, {}});
  }
  if (row_flag_toggle(ctx, mix(id_, "track lock", row.node.value()),
                      lock_bounds, RowFlag::Locked, row.locked,
                      row_hovered)) {
    frame.lane_edits.push_back(TimelineLaneEdit{
        TimelineLaneAction::ToggleLock, row.node, {}});
  }
}

// A row's Stage solo: lit while the row is soloed. Soloing anything draws, on
// the Stage alone, only what is soloed and what is inside it.
void TimelinePanel::solo_toggle(RowContext &rc, const gui::Rect &bounds) {
  gui::UiContext &ctx = rc.p.ctx;
  const doc::NodeId node = rc.row.node;
  const bool soloed = soloed_.contains(node);
  if (chip_toggle(ctx, mix(id_, "solo", node.value()), bounds, "S", soloed,
                  ctx.theme().warning,
                  soloed ? "Soloed: the Stage draws only what is soloed and "
                           "what is inside it. Program and the outputs draw "
                           "everything."
                         : "Solo on the Stage: draw only this and what is "
                           "inside it while you work. Lights keep lighting "
                           "unless a light is soloed. Program is "
                           "unaffected.")) {
    if (soloed) {
      soloed_.erase(node);
    } else {
      soloed_.insert(node);
    }
  }
}

// A layer: kind icon, name, and visibility and lock.
void TimelinePanel::layer_list_cell(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const std::span<const ElementInfo> elements = p.input.elements;
  TimelinePanelFrame &frame = p.frame;
  const gui::Rect &graph = p.layout.graph;
  const Row &row = rc.row;
  auto &y = rc.y;
  auto &centre_y = rc.centre_y;
  auto &row_hovered = rc.row_hovered;
  auto &content_x = rc.content_x;
  auto &name_x = rc.name_x;
  auto &name_bounds = rc.name_bounds;
  auto &lock_x = rc.lock_x;
  auto &eye_bounds = rc.eye_bounds;
  auto &lock_bounds = rc.lock_bounds;
  const gui::Theme &row_theme = ctx.theme();

  const ElementInfo *const row_element =
      row.surface ? nullptr : find_element(elements, row.node);
  const float icon_cx = content_x + 8.0F;
  if (row_element != nullptr) {
    draw_element_icon(ctx, *row_element, icon_cx, centre_y,
                      row_element->effective_visible
                          ? row.color
                          : alpha(row.color, 0.4F));
  } else {
    draw_kind_icon(ctx, ElementKind::Track, scene::LocalLightKind::Point,
                   icon_cx, centre_y, row.color);
  }
  name_x = icon_cx + 13.0F;
  // Everything the timeline lists can be switched off and protected, not
  // only the clips: a light or an LED surface is as worth muting while
  // working as a plate is. A surface is physical stage geometry rather
  // than a scene node, so it has no eye - but it takes the same padlock,
  // the one its Feed Mapping lock and Stage picking already honour. The
  // environment is always there.
  const bool has_toggles =
      row.surface || row_element == nullptr ||
      row_element->kind != ElementKind::Environment;
  // Anything the Stage draws - a layer, a model, a light, a group or a
  // collection - can be soloed there, from just inside its padlock.
  constexpr float solo_width = 18.0F;
  const bool solo_chip =
      has_toggles && !row.surface && row_element != nullptr &&
      orderable_layer(*row_element);
  // A placed video clip carries its decode resolution inside that, where
  // the state is read and switched without the Inspector.
  constexpr float resolution_width = 40.0F;
  const bool resolution_chip =
      has_toggles && !row.surface && row_element != nullptr &&
      row_element->kind == ElementKind::Media &&
      row_element->timing.placed();
  // A surface keeps its badge, just inside the padlock.
  const float name_right =
      (has_toggles ? lock_x : graph.x) - (row.surface ? 60.0F : 0.0F) -
      (has_toggles ? 0.0F : 64.0F) -
      (solo_chip ? solo_width + 4.0F : 0.0F) -
      (resolution_chip ? resolution_width + 4.0F : 0.0F);
  name_bounds = gui::Rect{name_x, y,
                          std::max(name_right - name_x - 6.0F, 0.0F),
                          row.height};
  if (rename_.editing(row.node.value())) {
    if (auto renamed = rename_.draw(
            ctx, gui::Rect{name_bounds.x - 4.0F, y + 2.0F,
                           name_bounds.width + 4.0F, row.height - 4.0F})) {
      frame.renames.push_back(TimelineRename{row.node, std::move(*renamed)});
    }
  } else {
    const bool shown =
        row_element == nullptr || row_element->effective_visible;
    ctx.label(name_bounds, row.label,
              row.locked ? row_theme.text_muted
                         : (shown ? row_theme.text
                                  : alpha(row_theme.text, 0.45F)));
  }
  if (row.surface) {
    ctx.label(gui::Rect{lock_x - 60.0F, y, 56.0F, row.height}, "SURFACE",
              row_theme.text_muted);
    if (row_flag_toggle(ctx, mix(id_, "surface lock", row.node.value()),
                        lock_bounds, RowFlag::Locked, row.locked,
                        row_hovered)) {
      frame.element_flags.push_back(TimelineElementFlagEdit{
          row.node, TimelineElementFlag::Locked, !row.locked});
    }
  } else if (has_toggles) {
    const bool hidden = !row.element_visible;
    if (row_flag_toggle(ctx, mix(id_, "element mute", row.node.value()),
                        eye_bounds, RowFlag::Visible, row.element_visible,
                        row_hovered)) {
      frame.element_flags.push_back(TimelineElementFlagEdit{
          row.node, TimelineElementFlag::Visible, hidden});
    }
    if (row_flag_toggle(ctx, mix(id_, "element lock", row.node.value()),
                        lock_bounds, RowFlag::Locked, row.locked,
                        row_hovered)) {
      frame.element_flags.push_back(TimelineElementFlagEdit{
          row.node, TimelineElementFlag::Locked, !row.locked});
    }
  }
  if (solo_chip) {
    solo_toggle(rc, gui::Rect{lock_x - 4.0F - solo_width, centre_y - 8.0F,
                              solo_width, 16.0F});
  }
  if (resolution_chip) {
    // Half is the one to notice - a softer picture wherever the clip is
    // shown larger than half its source - so it is the one lit, in the
    // warning colour.
    const bool half =
        row_element->media_resolution == scene::MediaResolution::Half;
    const float right = lock_x - 4.0F - (solo_chip ? solo_width + 4.0F : 0.0F);
    const gui::Rect chip{right - resolution_width, centre_y - 8.0F,
                         resolution_width, 16.0F};
    if (chip_toggle(
            ctx, mix(id_, "clip resolution", row.node.value()), chip,
            half ? "HALF" : "FULL", half, row_theme.warning,
            half ? "Half resolution: each decoded frame is held at half "
                   "width and height - a quarter of the video memory, softer "
                   "wherever it is shown larger than half its source. Click "
                   "for Full."
                 : "Full resolution. Click for Half: a quarter of the video "
                   "memory, softer wherever the clip is shown larger than "
                   "half its source size. Decoding costs the same.",
            !row.locked)) {
      frame.media_resolutions.push_back(TimelineMediaResolutionEdit{
          row.node, half ? scene::MediaResolution::Full
                         : scene::MediaResolution::Half});
    }
  }
}

// A parent's Properties foldout: only its name.
void TimelinePanel::properties_header_list_cell(RowContext &rc) {
  property_label(rc, rc.p.layout.graph.x - 48.0F);
}

// A property group - Position, Rotation, Scale, Color: its name, its
// components' values while it is closed, and the diamond that keys every
// component at the playhead between the steps to their keys.
void TimelinePanel::property_group_list_cell(RowContext &rc) {
  group_value_cell(rc);
  property_label(rc, rc.value_bounds.x - 4.0F);
  row_key_button(rc);
  row_key_steps(rc);
}

// One channel - a component or a property: its name, its value at the
// playhead, its visibility swatch and its key diamond between the steps to
// its previous and next key.
void TimelinePanel::channel_list_cell(RowContext &rc) {
  channel_value_cell(rc);
  property_label(rc, rc.value_bounds.x - 4.0F);
  channel_visibility_toggle(rc);
  row_key_button(rc);
  row_key_steps(rc);
}

// A property, a property group or the Properties foldout: its name, up to
// `right`.
void TimelinePanel::property_label(RowContext &rc, float right) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const Row &row = rc.row;
  auto &y = rc.y;
  auto &row_channel_identity = rc.row_channel_identity;
  auto &content_x = rc.content_x;
  auto &name_bounds = rc.name_bounds;

  // A property: named in the colour its keys and curve are drawn in, so
  // the list and the lanes read as one table.
  name_bounds = gui::Rect{content_x, y, std::max(right - content_x, 0.0F),
                          row.height};
  ctx.label(name_bounds, row.label,
            row.kind == Row::Kind::Component ||
                    row.kind == Row::Kind::Property
                ? channel_color(row_channel_identity->property)
                : row.color);
}

// A channel's value at the playhead. Where an edit keys - an animated
// channel with Auto Key on - it is a field that scrubs or takes a typed
// value and keys it at the playhead. Anywhere else an edit would change the
// base value, which is the Inspector's to edit, so the value is only shown.
void TimelinePanel::channel_value_cell(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const Row &row = rc.row;
  const float width =
      value_share(rc.content_x, rc.controls.value_right, row_value_width);
  rc.value_bounds =
      gui::Rect{rc.controls.value_right - width, rc.interaction_y + 3.0F,
                width, std::max(rc.interaction_height - 6.0F, 0.0F)};
  const gui::Rect &bounds = rc.value_bounds;
  if (!rc.row_channel_identity.has_value() || bounds.width <= 0.0F ||
      bounds.height <= 0.0F) {
    return;
  }
  const ChannelIdentity identity = *rc.row_channel_identity;
  const scene::ChannelKeyframes *const keys = rc.keys[0U];
  const bool animated = keys != nullptr && !keys->frames.empty();
  const double value = rc.values[0U];
  if (animated && auto_key_ && !row.locked) {
    double edited = value;
    if (value_fields_.draw(ctx, mix(id_, "row value", row_identity(row)),
                           bounds, "", value,
                           std::max(std::fabs(value) * 0.01, 0.01), edited,
                           3) &&
        std::isfinite(edited)) {
      p.frame.keys.push_back(TimelineKeyEdit{identity.target,
                                             identity.property,
                                             p.input.timeline.show_frame,
                                             edited, false, false});
    }
    return;
  }
  std::array<char, 32U> text{};
  const int length = std::snprintf(text.data(), text.size(), "%.3f", value);
  const std::string_view shown{
      text.data(), static_cast<std::size_t>(std::max(length, 0))};
  const float text_width =
      std::min(ctx.font_width(shown) + 1.0F, bounds.width);
  ctx.label(gui::Rect{bounds.x + bounds.width - text_width, rc.y, text_width,
                      row.height},
            shown, ctx.theme().text_muted);
  ctx.tooltip(bounds, row.locked ? "Unlock the layer to edit its keys"
                      : animated ? "Turn on Auto Key (K) to key a value here"
                                 : "Key it with the diamond to edit it here; "
                                   "the Inspector edits its base value");
}

// A closed property group's components at the playhead, as "x, y, z". An
// open group lists them on its own rows instead. The text takes only the
// room it needs, so the group's name keeps the rest.
void TimelinePanel::group_value_cell(RowContext &rc) {
  gui::UiContext &ctx = rc.p.ctx;
  const Row &row = rc.row;
  rc.value_bounds = gui::Rect{rc.controls.value_right, rc.y, 0.0F, row.height};
  if (rc.open) {
    return;
  }
  std::string shown;
  for (core::u8 component = 0U; component < row.component_count;
       ++component) {
    std::array<char, 32U> text{};
    const int length = std::snprintf(text.data(), text.size(), "%.2f",
                                     rc.values[component]);
    if (component > 0U) {
      shown += ", ";
    }
    shown.append(text.data(), static_cast<std::size_t>(std::max(length, 0)));
  }
  const float width =
      value_share(rc.content_x, rc.controls.value_right,
                  std::min(ctx.font_width(shown) + 1.0F, group_value_width));
  rc.value_bounds.x -= width;
  rc.value_bounds.width = width;
  if (width > 0.0F) {
    ctx.label(rc.value_bounds, shown, ctx.theme().text_muted);
  }
}

// The swatch that shows or hides one channel's keys and curve.
void TimelinePanel::channel_visibility_toggle(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const Row &row = rc.row;
  auto &row_channel_identity = rc.row_channel_identity;

  if (row_channel_identity.has_value()) {
    const bool channel_hidden =
        !graph_->visible(*row_channel_identity);
    const gui::Rect &visibility_bounds = rc.controls.swatch;
    const gui::Interaction visibility = ctx.grab(
        mix(id_, "channel visibility", row_identity(row)),
        visibility_bounds);
    ctx.panel(visibility_bounds,
              channel_hidden ? alpha(ctx.theme().track, 0.82F)
                             : channel_color(row_channel_identity->property));
    if (channel_hidden) {
      ctx.draw_list().add_line(visibility_bounds.x, visibility_bounds.y,
                               visibility_bounds.x + visibility_bounds.width,
                               visibility_bounds.y + visibility_bounds.height,
                               1.0F, ctx.theme().warning);
    }
    if (visibility.clicked) {
      graph_->set_active(*row_channel_identity);
      if (channel_hidden) {
        graph_->set_visible(*row_channel_identity, true);
      } else {
        graph_->set_visible(*row_channel_identity, false);
        keys_.selected.erase(
            std::remove_if(keys_.selected.begin(), keys_.selected.end(),
                           [&](const SelectedKey &key) {
                             return key.target ==
                                        row_channel_identity->target &&
                                    key.component ==
                                        row_channel_identity->property;
                           }),
            keys_.selected.end());
        keys_.drag_keys.erase(
            std::remove_if(keys_.drag_keys.begin(), keys_.drag_keys.end(),
                           [&](const SelectedKey &key) {
                             return key.target ==
                                        row_channel_identity->target &&
                                    key.component ==
                                        row_channel_identity->property;
                           }),
            keys_.drag_keys.end());
        if (keys_.drag_keys.empty()) {
          keys_.dragging = false;
          keys_.duplicate = false;
        }
      }
    }
  }

}

void TimelinePanel::row_key_button(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const EditorTimeline &timeline = p.input.timeline;
  const std::span<const ElementInfo> elements = p.input.elements;
  const std::span<const output::ProjectionSurface> surfaces = p.input.surfaces;
  TimelinePanelFrame &frame = p.frame;
  const Row &row = rc.row;
  const std::optional<RowChannels> channels = row_channels(row);

  // Property-local key controls: the group diamond keys all three axes;
  // a component diamond keys only that channel. A filled diamond means
  // there is already a key at the playhead, while a hollow bright one
  // means the channel is animated elsewhere. A key takes the value the
  // row shows: the channel evaluated at the playhead.
  if (row.locked || !channels.has_value()) {
    return;
  }
  const bool exists = row.surface ? find_surface(surfaces, row.node) != nullptr
                                  : find_element(elements, row.node) != nullptr;
  if (!exists) {
    return;
  }
  bool animated = false;
  bool keyed_here = true;
  const core::i64 show_frame = signed_frame(timeline.show_frame);
  for (core::u8 offset = 0U; offset < channels->count; ++offset) {
    const scene::ChannelKeyframes *const authored = rc.keys[offset];
    animated = animated || (authored != nullptr && !authored->frames.empty());
    keyed_here = keyed_here && authored != nullptr &&
                 std::find(authored->frames.begin(), authored->frames.end(),
                           show_frame) != authored->frames.end();
  }
  if (key_button(ctx, mix(id_, "row key", row_identity(row)),
                 rc.controls.key, animated, keyed_here, rc.background)) {
    for (core::u8 offset = 0U; offset < channels->count; ++offset) {
      frame.keys.push_back(TimelineKeyEdit{
          channels->target, channels->at(offset), timeline.show_frame,
          rc.values[offset], false, keyed_here});
    }
  }
}

// The steps either side of the key diamond: to the channel's previous and
// next key, or for a group the nearest key on any of its components. A step
// with no key that way is drawn dim and does nothing. They step from the
// playhead the panel last asked for, so repeated presses walk key to key
// without waiting for the show to arrive at each.
void TimelinePanel::row_key_steps(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const Row &row = rc.row;
  const std::optional<RowChannels> channels = row_channels(row);
  if (!channels.has_value()) {
    return;
  }
  bool animated = false;
  std::optional<core::i64> previous;
  std::optional<core::i64> next;
  for (core::u8 offset = 0U; offset < channels->count; ++offset) {
    const scene::ChannelKeyframes *const keys = rc.keys[offset];
    if (keys == nullptr || keys->frames.empty()) {
      continue;
    }
    animated = true;
    nearer_key(previous, keys->frames, p.playhead, false);
    nearer_key(next, keys->frames, p.playhead, true);
  }
  if (!animated) {
    return;
  }
  const auto step = [&](const gui::Rect &bounds, std::string_view name,
                        RowPart part, std::optional<core::i64> target,
                        bool forward, std::string_view tooltip) {
    const gui::Interaction it =
        ctx.grab(mix(id_, name, row_identity(row)), bounds);
    const bool owner = p.hover_owner.has_value() &&
                       p.hover_owner->tag == row_part_tag(rc.index, part);
    const gui::UiColor color =
        !target.has_value()
            ? alpha(ctx.theme().text_muted, 0.3F)
            : (owner ? ctx.theme().accent : ctx.theme().text_muted);
    const float cx = bounds.x + bounds.width * 0.5F;
    const float cy = rc.centre_y;
    const float tip = forward ? 3.0F : -3.0F;
    const std::array<gui::UiPoint, 3U> arrow{
        {{cx - tip, cy - 4.0F}, {cx + tip, cy}, {cx - tip, cy + 4.0F}}};
    ctx.draw_list().add_convex_poly(arrow, color);
    if (!target.has_value()) {
      return;
    }
    ctx.tooltip(bounds, tooltip);
    if (it.clicked && owner) {
      p.frame.seek_to_frame =
          std::clamp(static_cast<core::u64>(std::max<core::i64>(*target, 0)),
                     p.first, p.last);
    }
  };
  step(rc.controls.previous, "previous key", RowPart::PreviousKey, previous,
       false, "Previous key ([)");
  step(rc.controls.next, "next key", RowPart::NextKey, next, true,
       "Next key (])");
}

// Select, twirl, rename or lift the row from its list cell. False when the
// press handed the frame to a track header, whose lane then is not drawn.
bool TimelinePanel::row_label_press(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const std::span<const ElementInfo> elements = p.input.elements;
  const std::span<const doc::NodeId> selection = p.input.selection;
  TimelinePanelFrame &frame = p.frame;
  FrameScratch &scratch = p.scratch;
  const float &list_width = p.layout.list_width;
  const gui::Rect &rows_bounds = p.layout.rows;
  const std::vector<doc::NodeId> &visible_order = p.built.visible_order;
  const Row &row = rc.row;
  auto &row_group = rc.row_group;
  auto &open = rc.open;
  auto &content_x = rc.content_x;
  auto &name_x = rc.name_x;
  auto &name_bounds = rc.name_bounds;
  auto &interaction_y = rc.interaction_y;
  auto &interaction_height = rc.interaction_height;

  // A header being renamed gives its width to the text field; a header that
  // is merely selected stays fully grabbable, because selecting a track is
  // now the ordinary thing a click does to it.
  const float label_width =
      rename_.editing(row.node.value())
          ? std::max(name_x - rows_bounds.x - 4.0F, 0.0F)
          : list_width;
  const gui::Interaction label_it =
      ctx.grab(mix(id_, "row", row_identity(row)),
               gui::Rect{rows_bounds.x, interaction_y, label_width,
                         interaction_height});
  if (label_it.pressed && row.kind == Row::Kind::LaneHeader && !row.locked) {
    reorder_.lane_node = row.node;
    capture_layer_drag(p, row.node);
    reorder_.lane_start_x = ctx.pointer_press_x();
    reorder_.lane_start_y = ctx.pointer_press_y();
    reorder_.lane_moved = false;
  } else if (label_it.pressed && row.kind == Row::Kind::Layer &&
             !row.surface &&
             !(row.expandable && ctx.pointer_press_x() < content_x)) {
    const ElementInfo *const row_element = find_element(elements, row.node);
    if (row_element != nullptr && orderable_layer(*row_element) &&
        !row.locked) {
      keys_.selected.clear();
      keys_.drag_keys.clear();
      keys_.dragging = false;
      keys_.duplicate = false;
      reorder_.layer_node = row.node;
      const bool preserve_selection = selected(row.node, selection);
      capture_layer_drag(p, row.node);
      reorder_.layer_start_x = ctx.pointer_press_x();
      reorder_.layer_start_y = ctx.pointer_press_y();
      reorder_.layer_moved = false;
      keys_.marquee_tracking = false;
      keys_.marquee_active = false;
      keys_.lasso_points.clear();
      // Keep a selected group intact until a click is released. Modifier
      // clicks also select on release, so Ctrl toggles exactly once.
      if (!preserve_selection && !ctx.ctrl_held() && !ctx.shift_held()) {
        TimelineSelectionRequest next{};
        next.node = row.node;
        next.action = SelectionAction::Replace;
        frame.selection = std::move(next);
      }
    }
  }
  const bool dragged_header_click =
      row.kind == Row::Kind::LaneHeader && row.node == reorder_.lane_node &&
      reorder_.lane_moved;
  const bool dragged_layer_click =
      row.kind == Row::Kind::Layer && row.node == scratch.released_layer_drag_node &&
      scratch.released_layer_drag_moved;
  if (label_it.clicked && !dragged_header_click && !dragged_layer_click) {
    if (row.kind == Row::Kind::LaneHeader &&
        !(row.expandable && ctx.pointer_x() < content_x)) {
      selected_lane_ = row.node;
      reorder_.lane_destination = doc::NodeId{};
      keys_.selected.clear();
      keys_.drag_keys.clear();
      keys_.dragging = false;
      keys_.duplicate = false;
      // A track is a screen, so selecting one has to reach the Inspector
      // like selecting anything else does. Without this the header would
      // highlight and the Inspector would keep showing whatever was
      // selected before, which is the shape of a control that does nothing.
      if (!row.locked) {
        TimelineSelectionRequest next{};
        next.node = row.node;
        next.action =
            selection_action(ctx.ctrl_held(), ctx.shift_held(), true);
        if (next.action == SelectionAction::Range) {
          next.visible_order = visible_order;
        }
        frame.selection = std::move(next);
      }
      // A second click on the same name opens it for editing, focused
      // with the whole name selected so typing replaces it.
      if (!row.locked && !ctx.ctrl_held() && !ctx.shift_held() &&
          name_bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
          clicks_.double_click(row.node.value(), ctx.time_seconds())) {
        rename_.begin(ctx, row.node.value(), row.label);
      }
      return false;
    }
    const bool on_twirl = row.expandable && ctx.pointer_x() < content_x;
    if (on_twirl) {
      if (row.kind == Row::Kind::LaneHeader) {
        if (open) {
          rows_state_.collapsed_lanes.insert(row.node);
        } else {
          rows_state_.collapsed_lanes.erase(row.node);
        }
      } else if (row.hierarchy) {
        if (open) {
          rows_state_.expanded_hierarchies.erase(row.node);
        } else {
          rows_state_.expanded_hierarchies.insert(row.node);
        }
      } else if (row.kind == Row::Kind::Layer ||
                 row.kind == Row::Kind::PropertiesHeader) {
        if (open) {
          rows_state_.expanded_layers.erase(row.node.value());
        } else {
          rows_state_.expanded_layers.insert(row.node.value());
        }
      } else if (open) {
        rows_state_.expanded_groups.erase(row_group);
      } else {
        rows_state_.expanded_groups.insert(row_group);
      }
    } else if (!row.locked) {
      // A layer-row click leaves the key-selection domain. This
      // keeps a later Ctrl+C/Ctrl+V aimed at the newly selected
      // clip/layer rather than at stale Timeline diamonds.
      keys_.selected.clear();
      keys_.drag_keys.clear();
      keys_.dragging = false;
      keys_.duplicate = false;
      TimelineSelectionRequest next{};
      next.node = row.node;
      next.action =
          selection_action(ctx.ctrl_held(), ctx.shift_held(), !row.surface);
      if (next.action == SelectionAction::Range) {
        next.visible_order = visible_order;
      }
      next.surface = row.surface;
      const bool plain_click = next.action == SelectionAction::Replace;
      frame.selection = std::move(next);
      // A second click on a layer's name renames it, as in the outliner.
      if (row.kind == Row::Kind::Layer && !row.surface && plain_click &&
          name_bounds.contains(ctx.pointer_x(), ctx.pointer_y()) &&
          clicks_.double_click(row.node.value(), ctx.time_seconds())) {
        const ElementInfo *const element = find_element(elements, row.node);
        if (element != nullptr && !element->linked &&
            element->kind != ElementKind::Environment) {
          rename_.begin(ctx, row.node.value(), row.label);
        }
      }
    }
  }

  return true;
}

// A layer's lane: its clip bar, or the always-on band of an unplaced
// layer.
void TimelinePanel::layer_lane_cell(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &graph = p.layout.graph;
  const auto frame_to_x = [&](double value) noexcept {
    return time_axis_.x(value, graph.x);
  };
  auto &first_i = p.first_i;
  auto &last_i = p.last_i;
  const Row &row = rc.row;
  auto &y = rc.y;
  auto &centre_y = rc.centre_y;
  auto &clip_element = rc.clip_element;

  // A placed clip's bar with its folded keys over it; an unplaced media
  // layer's always-on band; or, for any other layer, a summary of what it
  // and anything folded under it animate - nothing at all when that is
  // nothing.
  if (clip_element != nullptr) {
    clip_bar_cell(rc);
    summary_lane_cell(rc, false);
  } else if (row.kind == Row::Kind::Layer && !row.surface &&
             row.element_kind == ElementKind::Media) {
    // A media layer with no placement is present for the whole show: a
    // quiet band in its colour, so it reads as "always on", not as a clip.
    const float x0 = frame_to_x(static_cast<double>(first_i));
    const float x1 = frame_to_x(static_cast<double>(last_i));
    const gui::Rect clip{x0, y + 5.0F, std::max(x1 - x0, 1.0F),
                         row.height - 10.0F};
    ctx.panel(clip, alpha(row.color, row.element_visible ? 0.16F : 0.06F));
    ctx.panel(gui::Rect{clip.x, clip.y, clip.width, 1.0F},
              alpha(row.color, row.element_visible ? 0.45F : 0.18F));
    if (row.element_kind == ElementKind::Media) {
      const float begin = std::max(clip.x, graph.x);
      const float end = std::min(clip.x + clip.width, graph.x + graph.width);
      std::size_t sample = 0U;
      for (float x = begin + 2.0F; x < end - 1.0F; x += 5.0F) {
        const core::u64 pattern =
            row.node.value() * 17U + sample * 29U + sample * sample;
        const float amplitude = 2.0F + static_cast<float>(pattern % 7U);
        ctx.panel(gui::Rect{x, centre_y - amplitude * 0.5F, 1.0F, amplitude},
                  alpha(row.color, 0.78F));
        ++sample;
      }
    }
    summary_lane_cell(rc, false);
  } else {
    summary_lane_cell(rc, true);
  }

}

// A placed clip: body, filmstrip and waveform, trim edges, ramps and fade
// knobs, its name, and what a press would do.
void TimelinePanel::clip_bar_cell(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const Row &row = rc.row;
  auto &clip_bar = rc.clip_bar;

  const bool live =
      clip_.drag != ClipDrag::None && clip_.node == row.node;
  ctx.panel(clip_bar, alpha(row.color, live ? 0.42F : 0.30F));
  // A row tall enough to read both gives the waveform a band of its own
  // under the pictures, as an editor stacks a clip's video over its audio;
  // a short one overlays the two.
  const TimelineWaveformObservation *const waveform =
      waveform_for_clip(p, row.node);
  const bool has_waveform =
      waveform != nullptr &&
      waveform->state == assets::DerivativeState::Ready &&
      waveform->bins.size() == assets::waveform_peak_rms_bins;
  gui::Rect pictures = clip_bar;
  gui::Rect sound = clip_bar;
  if (has_waveform && clip_bar.height >= split_clip_height) {
    const float band = std::max(std::floor(clip_bar.height * 0.36F), 14.0F);
    pictures.height = clip_bar.height - band;
    sound = gui::Rect{clip_bar.x, pictures.y + pictures.height, clip_bar.width,
                      band};
    ctx.panel(sound, alpha(ctx.theme().background, 0.45F));
  }
  const bool drew_filmstrip = draw_clip_filmstrip(rc, live, pictures);
  draw_clip_waveform(rc, drew_filmstrip && sound.y == pictures.y, sound);
  draw_clip_edges(rc);
  draw_clip_ramps(rc);
  draw_clip_label(rc);
  draw_clip_hover(rc);
}

bool TimelinePanel::draw_clip_filmstrip(RowContext &rc, bool live,
                                        const gui::Rect &area) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const EditorTimeline &timeline = p.input.timeline;
  const gui::Rect &graph = p.layout.graph;
  const auto x_to_frame = [&](float x) noexcept {
    return time_axis_.frame(x, graph.x);
  };
  const Row &row = rc.row;
  auto &clip_element = rc.clip_element;
  auto &clip_timing = rc.clip_timing;
  const gui::Rect &clip_bar = area;

  bool drew_filmstrip = false;
  if (const TimelineFilmstripObservation *const observation =
          filmstrip_for_clip(p, row.node);
      observation != nullptr &&
      observation->state == assets::DerivativeState::Ready &&
      observation->texture != nullptr) {
    float u0 = 0.0F;
    float u1 = 1.0F;
    bool looped = false;
    const scene::MediaFrameRate source_mapping_show_rate{
        timeline.rate_numerator, timeline.rate_denominator};
    if (clip_element->sequence_count > 1U &&
        clip_timing.source_in >= 0 && clip_timing.duration > 0) {
      const auto mapped = scene::source_ordinal_delta(
          clip_timing.duration - 1, source_mapping_show_rate,
          clip_element->source_frame_rate);
      const core::i64 maximum_source =
          static_cast<core::i64>(clip_element->sequence_count - 1U);
      if (mapped && mapped.value() >= 0 &&
          clip_timing.source_in <= maximum_source - mapped.value()) {
        const auto atlas_center = [maximum_source](core::i64 ordinal) {
          const double normalized = static_cast<double>(ordinal) /
                                    static_cast<double>(maximum_source);
          return static_cast<float>(
              (normalized * 7.0 + 0.5) / 8.0);
        };
        u0 = atlas_center(clip_timing.source_in);
        u1 = atlas_center(clip_timing.source_in + mapped.value());
        constexpr float one_tile = 1.0F / 8.0F;
        if (u1 - u0 < one_tile) {
          const float midpoint = (u0 + u1) * 0.5F;
          u0 = std::clamp(midpoint - one_tile * 0.5F, 0.0F,
                          1.0F - one_tile);
          u1 = u0 + one_tile;
        }
      } else {
        looped = true;
      }
    }
    const gui::UiColor tint{1.0F, 1.0F, 1.0F, live ? 0.62F : 0.78F};
    if (clip_bar.height >= tiled_filmstrip_height && clip_timing.duration > 0) {
      // Whole frames at the source's shape, anchored to the clip's start so
      // they ride with it rather than swimming as the view pans. Each shows
      // the nearest of the atlas's eight thumbnails to the frame at its
      // left edge; the last is cropped where the clip ends.
      const float tile_width =
          clip_bar.height * static_cast<float>(timeline_filmstrip_tile_width) /
          static_cast<float>(timeline_filmstrip_tile_height);
      const float clip_right = clip_bar.x + clip_bar.width;
      const float visible_right = std::min(clip_right, graph.x + graph.width);
      const float first_tile = std::floor(
          std::max(std::max(clip_bar.x, graph.x) - clip_bar.x, 0.0F) /
          tile_width);
      for (unsigned drawn = 0U; drawn < maximum_filmstrip_tiles; ++drawn) {
        const float x =
            clip_bar.x + (first_tile + static_cast<float>(drawn)) * tile_width;
        if (x >= visible_right) {
          break;
        }
        const float width = std::min(tile_width, clip_right - x);
        const double offset = std::clamp(
            std::floor(static_cast<double>(x - clip_bar.x) /
                       static_cast<double>(time_axis_.pixels_per_frame)),
            0.0, static_cast<double>(clip_timing.duration - 1));
        unsigned thumbnail = 0U;
        if (clip_element->sequence_count > 1U && clip_timing.source_in >= 0) {
          const auto phase = scene::advance_source_phase(
              clip_timing.source_in, static_cast<core::i64>(offset),
              source_mapping_show_rate, clip_element->source_frame_rate,
              clip_element->sequence_count, scene::SourcePhaseRounding::Floor);
          if (!phase) {
            continue;
          }
          thumbnail = static_cast<unsigned>(std::llround(
              static_cast<double>(phase.value()) * 7.0 /
              static_cast<double>(clip_element->sequence_count - 1U)));
        }
        const float left_uv = static_cast<float>(thumbnail) / 8.0F;
        ctx.draw_list().add_image_region(
            x, clip_bar.y, width, clip_bar.height, *observation->texture,
            left_uv, 0.0F, left_uv + 0.125F * (width / tile_width), 1.0F,
            tint);
        // A hairline between frames, so a run of similar pictures still
        // reads as frames.
        ctx.panel(gui::Rect{x, clip_bar.y, 1.0F, clip_bar.height},
                  alpha(ctx.theme().background, 0.6F));
      }
    } else if (looped) {
      // Long placements repeat the source. Sample the existing eight-thumb
      // atlas at visible timeline positions, with a fixed per-row budget.
      // Neither a very long placement nor many loops increases this work.
      const float begin = std::max(clip_bar.x, graph.x);
      const float end = std::min(clip_bar.x + clip_bar.width,
                                 graph.x + graph.width);
      if (end > begin) {
        const unsigned tiles = static_cast<unsigned>(std::clamp(
            std::ceil((end - begin) / 72.0F), 1.0F, 32.0F));
        const float width = (end - begin) / static_cast<float>(tiles);
        for (unsigned tile = 0U; tile < tiles; ++tile) {
          const float x = begin + static_cast<float>(tile) * width;
          const double offset = std::clamp(
              x_to_frame(x + width * 0.5F) -
                  static_cast<double>(clip_timing.start),
              0.0, static_cast<double>(clip_timing.duration - 1));
          const core::i64 show_offset =
              offset >= static_cast<double>(clip_timing.duration - 1)
                  ? clip_timing.duration - 1
                  : static_cast<core::i64>(offset);
          const auto phase = scene::advance_source_phase(
              clip_timing.source_in, show_offset,
              source_mapping_show_rate, clip_element->source_frame_rate,
              clip_element->sequence_count, scene::SourcePhaseRounding::Floor);
          if (!phase) {
            continue;
          }
          const auto thumbnail = static_cast<unsigned>(std::llround(
              static_cast<double>(phase.value()) * 7.0 /
              static_cast<double>(clip_element->sequence_count - 1U)));
          const float left_uv = static_cast<float>(thumbnail) / 8.0F;
          ctx.draw_list().add_image_region(
              x, clip_bar.y, width, clip_bar.height, *observation->texture,
              left_uv, 0.0F, left_uv + 0.125F, 1.0F, tint);
        }
      }
    } else {
      ctx.draw_list().add_image_region(
          clip_bar.x, clip_bar.y, clip_bar.width, clip_bar.height,
          *observation->texture, u0, 0.0F, u1, 1.0F, tint);
    }
    drew_filmstrip = true;
  }
  return drew_filmstrip;
}

void TimelinePanel::draw_clip_waveform(RowContext &rc, bool drew_filmstrip,
                                       const gui::Rect &area) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const EditorTimeline &timeline = p.input.timeline;
  const gui::Rect &graph = p.layout.graph;
  const auto x_to_frame = [&](float x) noexcept {
    return time_axis_.frame(x, graph.x);
  };
  const Row &row = rc.row;
  const float centre_y = area.y + area.height * 0.5F;
  auto &clip_element = rc.clip_element;
  auto &clip_timing = rc.clip_timing;
  const gui::Rect &clip_bar = area;

  if (const TimelineWaveformObservation *const waveform =
          waveform_for_clip(p, row.node);
      waveform != nullptr &&
      waveform->state == assets::DerivativeState::Ready &&
      waveform->bins.size() == assets::waveform_peak_rms_bins &&
      clip_element->sequence_count > 0U && clip_timing.duration > 0) {
    const float begin = std::max(clip_bar.x, graph.x) + 1.0F;
    const float end =
        std::min(clip_bar.x + clip_bar.width, graph.x + graph.width) - 1.0F;
    const float half_height = std::max(clip_bar.height * 0.42F, 1.0F);
    const scene::MediaFrameRate show_frame_rate{timeline.rate_numerator,
                                                 timeline.rate_denominator};
    const core::i64 maximum_source = static_cast<core::i64>(
        std::min<core::u64>(
            clip_element->sequence_count - 1U,
            static_cast<core::u64>(std::numeric_limits<core::i64>::max())));
    for (float x = begin; x <= end; x += 2.0F) {
      const double offset = std::clamp(
          std::floor(x_to_frame(x)) - static_cast<double>(clip_timing.start),
          0.0, static_cast<double>(clip_timing.duration - 1));
      const core::i64 show_offset =
          offset >= static_cast<double>(clip_timing.duration - 1)
              ? clip_timing.duration - 1
              : static_cast<core::i64>(offset);
      const auto source_phase = scene::advance_source_phase(
          clip_timing.source_in, show_offset, show_frame_rate,
          clip_element->source_frame_rate, clip_element->sequence_count,
          scene::SourcePhaseRounding::Floor);
      if (!source_phase) {
        continue;
      }
      const core::i64 source_ordinal = source_phase.value();
      const double normalized =
          maximum_source > 0
              ? static_cast<double>(source_ordinal) /
                    static_cast<double>(maximum_source)
              : 0.0;
      const std::size_t bin = std::min<std::size_t>(
          static_cast<std::size_t>(std::floor(
              normalized * static_cast<double>(waveform->bins.size()))),
          waveform->bins.size() - 1U);
      const assets::WaveformPeakRms sample = waveform->bins[bin];
      const float peak_height = sample.peak * half_height;
      const float rms_height = sample.rms * half_height;
      ctx.panel(gui::Rect{x, centre_y - rms_height, 1.0F,
                          std::max(rms_height * 2.0F, 1.0F)},
                alpha(ctx.theme().keyframe,
                      drew_filmstrip ? 0.68F : 0.76F));
      ctx.panel(gui::Rect{x, centre_y - peak_height, 1.0F, 1.0F},
                alpha(ctx.theme().text, 0.82F));
      ctx.panel(gui::Rect{x, centre_y + peak_height, 1.0F, 1.0F},
                alpha(ctx.theme().text, 0.82F));
    }
  }
}

void TimelinePanel::draw_clip_edges(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const Row &row = rc.row;
  auto &clip_bar = rc.clip_bar;

  ctx.panel(gui::Rect{clip_bar.x, clip_bar.y, clip_bar.width, 1.0F},
            alpha(row.color, 0.85F));
  ctx.panel(gui::Rect{clip_bar.x, clip_bar.y + clip_bar.height - 1.0F,
                      clip_bar.width, 1.0F},
            alpha(row.color, 0.85F));
  const float grab = edge_grab_width(clip_bar.width);
  ctx.panel(gui::Rect{clip_bar.x, clip_bar.y, grab, clip_bar.height},
            alpha(row.color, 0.68F));
  ctx.panel(gui::Rect{clip_bar.x + clip_bar.width - grab, clip_bar.y, grab,
                      clip_bar.height},
            alpha(row.color, 0.68F));

}

void TimelinePanel::draw_clip_ramps(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const Row &row = rc.row;
  auto &clip_timing = rc.clip_timing;
  auto &clip_bar = rc.clip_bar;
  auto &clip_fade_in_x = rc.clip_fade_in_x;
  auto &clip_fade_out_x = rc.clip_fade_out_x;

  // The ramps, drawn the way an editor draws them: a diagonal from
  // the bar's foot at the cut to its head where the clip is fully in.
  const gui::UiColor ramp = alpha(ctx.theme().keyframe, 0.9F);
  if (clip_timing.fade_in > 0) {
    ctx.draw_list().add_line(clip_bar.x, clip_bar.y + clip_bar.height,
                             clip_fade_in_x, clip_bar.y, 1.0F, ramp);
  }
  if (clip_timing.fade_out > 0) {
    ctx.draw_list().add_line(clip_fade_out_x, clip_bar.y,
                             clip_bar.x + clip_bar.width,
                             clip_bar.y + clip_bar.height, 1.0F, ramp);
  }
  // The fade handles ride the bar's top edge as round knobs. They grow
  // and light under the pointer, and answer a press several units away,
  // so a crossfade can be pulled without hunting for a pixel.
  const auto fade_handle = [&](float handle_x, RowPart part, bool engaged) {
    const bool near = clip_.drag == ClipDrag::None &&
                      p.hover_owner.has_value() &&
                      tag_row(p.hover_owner->tag) == rc.index &&
                      tag_part(p.hover_owner->tag) == part;
    const bool lit = engaged || near;
    draw_filled_circle(ctx, handle_x, clip_bar.y + 1.0F,
                       lit ? 6.0F : 4.5F,
                       alpha(ctx.theme().background, 0.85F));
    draw_filled_circle(ctx, handle_x, clip_bar.y + 1.0F,
                       lit ? 4.5F : 3.0F,
                       lit ? ctx.theme().accent : ramp);
    if (near) {
      ctx.set_cursor(gui::Cursor::ResizeHorizontal);
      ctx.tooltip(gui::Rect{handle_x - fade_handle_reach, clip_bar.y - 4.0F,
                            fade_handle_reach * 2.0F,
                            fade_handle_reach + 4.0F},
                  "Drag to set the fade");
    }
  };
  fade_handle(clip_fade_in_x, RowPart::FadeIn,
              clip_.drag == ClipDrag::FadeIn && clip_.node == row.node);
  fade_handle(clip_fade_out_x, RowPart::FadeOut,
              clip_.drag == ClipDrag::FadeOut && clip_.node == row.node);

}

void TimelinePanel::draw_clip_label(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const std::span<const doc::NodeId> selection = p.input.selection;
  const gui::Rect &graph = p.layout.graph;
  const Row &row = rc.row;
  auto &clip_element = rc.clip_element;
  auto &clip_timing = rc.clip_timing;
  auto &clip_bar = rc.clip_bar;

  // Every editor names its clips on the bar. The name rides on a dark
  // backing so it reads over any filmstrip, and stays pinned to the
  // visible start of a bar scrolled half off the left edge.
  const float name_left =
      std::max(clip_bar.x, graph.x) + 6.0F + edge_grab_width(clip_bar.width);
  const float name_right = clip_bar.x + clip_bar.width - 6.0F;
  if (name_right - name_left >= 24.0F) {
    std::string name = row.label;
    if (clip_.drag == ClipDrag::Slip && clip_.node == row.node) {
      name += "  Source " + std::to_string(clip_timing.source_in);
    }
    if (clip_element->media_resolution == scene::MediaResolution::Half) {
      name += "  HALF";
    }
    if (!clip_element->on_air) {
      name += "  off air";
    }
    // On a tall row the name keeps to a band along the top, as a source
    // name sits over a clip's first frame, and leaves the pictures clear.
    const float band = std::min(clip_bar.height, row_height - 6.0F);
    const float text_width =
        std::min(ctx.font_width(name) + 8.0F, name_right - name_left);
    ctx.panel(gui::Rect{name_left - 4.0F, clip_bar.y + 2.0F, text_width,
                        band - 4.0F},
              alpha(ctx.theme().background, 0.55F));
    ctx.label(gui::Rect{name_left, clip_bar.y, name_right - name_left, band},
              name,
              clip_element->on_air ? ctx.theme().text
                                   : ctx.theme().text_muted);
  }
  if (selected(row.node, selection)) {
    draw_border(ctx, clip_bar, ctx.theme().text, 1.5F);
  }
}

void TimelinePanel::draw_clip_hover(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &graph = p.layout.graph;
  const auto frame_to_x = [&](double value) noexcept {
    return time_axis_.x(value, graph.x);
  };
  const Row &row = rc.row;
  auto &clip_bar = rc.clip_bar;

  const float grab = edge_grab_width(clip_bar.width);
  // What a press here would do, shown before it is done: the trim edge
  // under a Selection pointer lights, and the razor draws its cut.
  const RowPart hovered =
      p.hover_owner.has_value() && tag_row(p.hover_owner->tag) == rc.index
          ? tag_part(p.hover_owner->tag)
          : RowPart::Lane;
  if (hovered != RowPart::Lane && clip_.drag == ClipDrag::None &&
      !ctx.pointer_blocked()) {
    const float px = ctx.pointer_x();
    if (edit_tool_ == TimelineEditTool::Razor) {
      const float cut_x =
          frame_to_x(static_cast<double>(razor_frame(p, px)));
      ctx.panel(gui::Rect{cut_x - 0.5F, clip_bar.y - 2.0F, 1.5F,
                          clip_bar.height + 4.0F},
                ctx.theme().warning);
    } else if (edit_tool_ == TimelineEditTool::Standard) {
      if (hovered == RowPart::FadeIn || hovered == RowPart::FadeOut) {
        // The handle itself shows the hover.
      } else if (hovered == RowPart::TrimIn) {
        ctx.panel(gui::Rect{clip_bar.x, clip_bar.y, grab, clip_bar.height},
                  row.color);
        ctx.set_cursor(gui::Cursor::ResizeHorizontal);
      } else if (hovered == RowPart::TrimOut) {
        ctx.panel(gui::Rect{clip_bar.x + clip_bar.width - grab,
                            clip_bar.y, grab, clip_bar.height},
                  row.color);
        ctx.set_cursor(gui::Cursor::ResizeHorizontal);
      } else {
        ctx.set_cursor(gui::Cursor::Move);
      }
    }
  }
}

// A property group's lane: its components' keys, aggregated.
void TimelinePanel::property_group_lane_cell(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const std::span<const scene::ChannelKeyframes> keyframes = p.input.keyframes;
  const gui::Rect &graph = p.layout.graph;
  const auto frame_to_x = [&](double value) noexcept {
    return time_axis_.x(value, graph.x);
  };
  const Row &row = rc.row;
  auto &centre_y = rc.centre_y;

  const auto draw_keys = [&](const scene::ChannelKeyframes *keys,
                             gui::UiColor color) {
    if (keys == nullptr) {
      return;
    }
    for (std::size_t key = 0U; key < keys->frames.size(); ++key) {
      draw_key_glyph(ctx, key_glyph(*keys, key),
                     frame_to_x(static_cast<double>(keys->frames[key])),
                     centre_y, key_radius, color);
    }
  };

  for (core::u8 component = 0U; component < row.component_count;
       ++component) {
    const auto aggregate = group_channel(row.group, component);
    if (!graph_->visible(ChannelIdentity{row.node, aggregate})) {
      continue;
    }
    draw_keys(channel_for(keyframes, row.node, aggregate),
              alpha(channel_color(aggregate), 0.58F));
  }

}

// One channel's lane in the dope sheet: its keys where layout_row() placed
// them. Curves mode draws its curve on the graph instead.
void TimelinePanel::channel_lane_cell(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  FrameScratch &scratch = p.scratch;
  const gui::Rect &graph = p.layout.graph;
  const auto frame_to_x = [&](double value) noexcept {
    return time_axis_.x(value, graph.x);
  };
  const Row &row = rc.row;
  const scene::ChannelKeyframes *const component_channel =
      rc.component_channel;
  if (component_channel == nullptr) {
    return;
  }
  const doc::NodeId channel_target = rc.row_channel_identity->target;
  const scene::TransformChannel channel = rc.channel;
  const gui::UiColor component_color = channel_color(channel);

  for (std::size_t key_index = 0U; key_index < component_channel->frames.size();
       ++key_index) {
    const core::i64 key_frame = component_channel->frames[key_index];
    const SelectedKey identity{channel_target, channel, key_frame};
    const auto moving =
        std::find(keys_.drag_keys.begin(), keys_.drag_keys.end(), identity);
    const bool moving_key = keys_.dragging && moving != keys_.drag_keys.end();
    const float key_x = rc.component_x[key_index];
    const float key_y = rc.component_y[key_index];
    const bool is_key_selected = key_selected(identity);
    const KeyGlyph glyph = key_glyph(*component_channel, key_index);
    if (is_key_selected) {
      draw_key_glyph(ctx, glyph, key_x, key_y, key_radius + 2.0F,
                     alpha(ctx.theme().accent, 0.50F));
    }
    draw_key_glyph(ctx, glyph, key_x, key_y,
                   is_key_selected ? key_radius + 0.5F : key_radius,
                   is_key_selected ? ctx.theme().playhead : component_color);
    if (moving_key && keys_.duplicate && keys_.drag_delta != 0) {
      const float duplicate_x =
          frame_to_x(static_cast<double>(key_frame + keys_.drag_delta));
      draw_key_glyph(ctx, glyph, duplicate_x, key_y, key_radius + 0.5F,
                     ctx.theme().accent);
    }
    if (!row.locked) {
      scratch.visible_keys.push_back(
          FrameScratch::VisibleKeyPoint{identity, key_x, key_y});
    }
  }
}

// A press in a row's lane: a razor cut, a clip gesture, a key, a marquee
// or a scrub.
void TimelinePanel::lane_press(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const std::span<const ElementInfo> elements = p.input.elements;
  TimelinePanelFrame &frame = p.frame;
  const gui::Rect &graph = p.layout.graph;
  const std::vector<doc::NodeId> &visible_order = p.built.visible_order;
  const auto x_to_frame = [&](float x) noexcept {
    return time_axis_.frame(x, graph.x);
  };
  auto &first = p.first;
  auto &last = p.last;
  const Row &row = rc.row;
  const std::size_t index = rc.index;
  auto &interaction_y = rc.interaction_y;
  auto &interaction_height = rc.interaction_height;
  auto &clip_element = rc.clip_element;
  auto &clip_timing = rc.clip_timing;
  auto &clip_bar = rc.clip_bar;
  auto &component_frames = rc.component_frames;
  auto &channel = rc.channel;

  const gui::Interaction lane_it = ctx.grab(
      mix(id_, "lane", row_identity(row)),
      gui::Rect{graph.x, interaction_y, graph.width, interaction_height});
  note_row_menu_press(rc);
  // The razor cuts where it lands and starts no drag; Shift cuts every
  // unlocked clip under that frame, on every track.
  // A native event batch may already contain motion or release after the
  // down edge. Classify the gesture where it began, not at that endpoint.
  const float press_x = ctx.pointer_press_x();
  const float press_y = ctx.pointer_press_y();
  // A press acts on the one target that owns it, and only in its own row.
  const bool owned_here = lane_it.pressed && p.press_owner.has_value() &&
                          tag_row(p.press_owner->tag) == index;
  const RowPart owned =
      owned_here ? tag_part(p.press_owner->tag) : RowPart::Lane;
  const std::size_t owned_item =
      owned_here ? tag_item(p.press_owner->tag) : 0U;
  const bool on_clip = owned == RowPart::ClipBody ||
                       owned == RowPart::TrimIn || owned == RowPart::TrimOut ||
                       owned == RowPart::FadeIn || owned == RowPart::FadeOut;
  const bool razor_press = owned_here && on_clip &&
                           edit_tool_ == TimelineEditTool::Razor;
  if (razor_press) {
    clip_.press_consumed = true;
    const core::i64 cut = razor_frame(p, press_x);
    for (const ElementInfo &element : elements) {
      if (!editable_clip(element) ||
          (element.node != row.node && !ctx.shift_held())) {
        continue;
      }
      if (cut > element.timing.start && cut <= element.timing.last_frame()) {
        frame.clip_splits.push_back(
            TimelineClipSplit{element.node, cut});
      }
    }
  } else if (owned_here && on_clip) {
    clip_.press_consumed = true;
    keys_.selected.clear();
    keys_.drag_keys.clear();
    keys_.dragging = false;
    keys_.duplicate = false;
    markers_.selected.clear();
    const float px = press_x;
    const float midpoint = clip_bar.x + clip_bar.width * 0.5F;
    clip_.peer = doc::NodeId{};
    doc::NodeId dragged = row.node;
    scene::ClipTiming dragged_timing = clip_timing;
    if (edit_tool_ == TimelineEditTool::RippleTrim) {
      clip_.drag =
          px < midpoint ? ClipDrag::RippleTrimHead : ClipDrag::RippleTrimTail;
    } else if (edit_tool_ == TimelineEditTool::Roll) {
      // Roll the cut nearest the press: the clip butted against this
      // one's head on the left half, its tail on the right half, falling
      // back to whichever neighbour exists.
      const auto neighbour = [&](bool before) -> const ElementInfo * {
        for (const ElementInfo &candidate : elements) {
          if (candidate.node == row.node || !editable_clip(candidate) ||
              candidate.timeline_lane != clip_element->timeline_lane) {
            continue;
          }
          const core::i64 candidate_end =
              candidate.timing.start + candidate.timing.duration;
          if (before ? candidate_end == clip_timing.start
                     : candidate.timing.start ==
                           clip_timing.start + clip_timing.duration) {
            return &candidate;
          }
        }
        return nullptr;
      };
      const ElementInfo *left_of = neighbour(true);
      const ElementInfo *right_of = neighbour(false);
      const bool head = px < midpoint;
      const ElementInfo *peer = head ? (left_of ? left_of : right_of)
                                     : (right_of ? right_of : left_of);
      if (peer != nullptr) {
        clip_.drag = ClipDrag::Roll;
        // The earlier clip leads, so the request reads left to right.
        const bool peer_first = peer->timing.start < clip_timing.start;
        dragged = peer_first ? peer->node : row.node;
        clip_.peer = peer_first ? row.node : peer->node;
        dragged_timing = peer_first ? peer->timing : clip_timing;
      } else {
        // Nothing butts against this clip, so there is no cut to roll.
        clip_.drag = ClipDrag::None;
        clip_.press_consumed = true;
      }
    } else if (edit_tool_ == TimelineEditTool::Slip) {
      clip_.drag = ClipDrag::Slip;
    } else if (edit_tool_ == TimelineEditTool::Slide) {
      clip_.drag = ClipDrag::Slide;
    } else {
      // Standard placement editing: whichever part owns the press. Fade
      // knobs on the bar's top edge outrank the trim edges, which outrank
      // the body.
      switch (owned) {
      case RowPart::FadeIn:
        clip_.drag = ClipDrag::FadeIn;
        break;
      case RowPart::FadeOut:
        clip_.drag = ClipDrag::FadeOut;
        break;
      case RowPart::TrimIn:
        clip_.drag = ClipDrag::TrimIn;
        break;
      case RowPart::TrimOut:
        clip_.drag = ClipDrag::TrimOut;
        break;
      case RowPart::ClipBody:
      case RowPart::Lane:
      case RowPart::Key:
        clip_.drag = ClipDrag::Move;
        break;
      }
    }
    clip_.node = clip_.drag != ClipDrag::None ? dragged : doc::NodeId{};
    clip_.origin = dragged_timing;
    clip_.proposed = dragged_timing;
    clip_.status.clear();
    clip_.status_error = false;
    constexpr core::i64 edit_limit = std::numeric_limits<core::i64>::max() / 4;
    if (dragged_timing.start < -edit_limit || dragged_timing.start > edit_limit ||
        dragged_timing.duration > edit_limit || dragged_timing.source_in > edit_limit) {
      // Imported documents can carry a wider range than the interactive
      // workspace. Refuse arithmetic beyond that range rather than wrap it.
      clip_.drag = ClipDrag::None;
      clip_.node = doc::NodeId{};
      clip_.status = "EDIT UNAVAILABLE  /  Clip timing exceeds the editable range";
      clip_.status_error = true;
      clip_.status_until = ctx.time_seconds() + 3.0;
    }
    clip_.anchor = static_cast<core::i64>(std::llround(x_to_frame(px)));
    clip_.delta = 0;
    clip_.start_x = press_x;
    clip_.start_y = press_y;
    // A whole gesture can be captured after the proposal phase above.
    // Preserve its up position until the next draw resolves it, even if
    // later motion in this batch or the next one moves the free pointer.
    clip_.release = lane_it.released
        ? std::optional{gui::UiPoint{ctx.pointer_release_x(),
                                     ctx.pointer_release_y()}}
        : std::nullopt;
    clip_.moved = false;
    clip_.drop_lane.reset();
    clip_.snap_guide.reset();
    keys_.marquee_tracking = false;
    keys_.marquee_active = false;
    keys_.lasso_points.clear();
    TimelineSelectionRequest next{};
    next.node = row.node;
    next.action = selection_action(ctx.ctrl_held(), ctx.shift_held(), true);
    if (next.action == SelectionAction::Range) {
      next.visible_order = visible_order;
    }
    frame.selection = std::move(next);
  } else if (owned_here) {
    if ((row.kind == Row::Kind::Component ||
         row.kind == Row::Kind::Property) &&
        component_frames != nullptr && owned == RowPart::Key &&
        owned_item < component_frames->size()) {
      press_key(p, SelectedKey{row.kind == Row::Kind::Property
                                   ? row.property_target
                                   : row.node,
                               channel, (*component_frames)[owned_item]});
      if (row.locked) {
        frame.seek_to_frame =
            rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
      }
    } else {
      press_lane_background(p, row.locked);
    }
  }
  scrub_held_lane(p, lane_it);
}

void TimelinePanel::note_row_menu_press(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &rows_bounds = p.layout.rows;
  if (ctx.secondary_pointer_pressed() && rc.interaction_height > 0.0F &&
      ctx.pointer_y() >= rc.interaction_y &&
      ctx.pointer_y() < rc.interaction_y + rc.interaction_height &&
      ctx.pointer_x() >= rows_bounds.x &&
      ctx.pointer_x() < rows_bounds.x + rows_bounds.width) {
    p.scratch.right_row = rc.index;
  }
}

// A press on a key: it becomes the active channel, the selection takes the
// key (Ctrl toggles it, Shift adds it), and unless Ctrl just dropped it the
// whole selection lifts for a shared-delta drag.
void TimelinePanel::press_key(Pass &p, const SelectedKey &hit) {
  gui::UiContext &ctx = p.ctx;
  const std::span<const scene::ChannelKeyframes> keyframes = p.input.keyframes;

  graph_->set_active(ChannelIdentity{hit.target, hit.component});
  bool begin_drag = true;
  const auto selected_it =
      std::find(keys_.selected.begin(), keys_.selected.end(), hit);
  if (ctx.ctrl_held()) {
    if (selected_it != keys_.selected.end()) {
      keys_.selected.erase(selected_it);
      begin_drag = false;
    } else {
      keys_.selected.push_back(hit);
    }
  } else if (ctx.shift_held()) {
    if (selected_it == keys_.selected.end()) {
      keys_.selected.push_back(hit);
    }
  } else if (selected_it == keys_.selected.end()) {
    keys_.selected.assign(1U, hit);
  }
  canonicalize_key_selection();

  if (begin_drag) {
    keys_.drag_keys.clear();
    for (const SelectedKey &selected_key : keys_.selected) {
      const auto *const authored =
          frames_for(keyframes, selected_key.target, selected_key.component);
      if (authored != nullptr &&
          std::find(authored->begin(), authored->end(), selected_key.frame) !=
              authored->end()) {
        keys_.drag_keys.push_back(selected_key);
      }
    }
    if (keys_.drag_keys.empty()) {
      keys_.drag_keys.push_back(hit);
    }
    keys_.dragging = true;
    // Alt is latched at press: releasing the modifier before the pointer
    // does not turn a duplicate into a destructive retime halfway through
    // the gesture.
    keys_.duplicate = ctx.alt_held();
    keys_.drag_anchor_frame = hit.frame;
    keys_.drag_delta = 0;
    keys_.marquee_tracking = false;
    keys_.marquee_active = false;
    keys_.lasso_points.clear();
  }
  markers_.selected.clear();
}

// A press on an empty lane or graph: without a modifier it clears the scene
// selection; unless the row is locked it starts a marquee (or lasso) from the
// press; and it seeks to the frame under the pointer.
void TimelinePanel::press_lane_background(Pass &p, bool locked) {
  gui::UiContext &ctx = p.ctx;
  TimelinePanelFrame &frame = p.frame;
  const gui::Rect &graph = p.layout.graph;

  if (!ctx.ctrl_held() && !ctx.shift_held()) {
    TimelineSelectionRequest clear{};
    clear.action = SelectionAction::Clear;
    frame.selection = std::move(clear);
  }
  if (!locked) {
    keys_.marquee_base = keys_.selected;
    keys_.marquee_shift = ctx.shift_held();
    keys_.marquee_ctrl = ctx.ctrl_held();
    if (!keys_.marquee_shift && !keys_.marquee_ctrl) {
      keys_.selected.clear();
    }
    keys_.marquee_tracking = true;
    keys_.marquee_active = false;
    keys_.marquee_start_x = ctx.pointer_press_x();
    keys_.marquee_start_y = ctx.pointer_press_y();
    keys_.lasso_points.clear();
    if (keys_.lasso_mode) {
      keys_.lasso_points.push_back(
          gui::UiPoint{keys_.marquee_start_x, keys_.marquee_start_y});
    }
  }
  frame.seek_to_frame = rounded_frame(time_axis_.frame(ctx.pointer_x(), graph.x),
                                      p.first, p.last);
}

// Holding a press on an empty lane or graph scrubs, unless a key, clip,
// marquee or row lift has taken the gesture.
void TimelinePanel::scrub_held_lane(Pass &p, const gui::Interaction &lane) {
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &graph = p.layout.graph;
  if (lane.held && !keys_.dragging && !keys_.marquee_active &&
      !handle_drag_.active &&
      clip_.drag == ClipDrag::None && !clip_.press_consumed &&
      reorder_.layer_node == doc::NodeId{} &&
      reorder_.lane_node == doc::NodeId{}) {
    p.frame.seek_to_frame = rounded_frame(
        time_axis_.frame(ctx.pointer_x(), graph.x), p.first, p.last);
  }
}

gui::Rect TimelinePanel::graph_area(const Pass &p) const noexcept {
  const gui::Rect &graph = p.layout.graph;
  const gui::Rect &rows_bounds = p.layout.rows;
  return gui::Rect{graph.x, rows_bounds.y, graph.width, rows_bounds.height};
}

// What the Curves graph shows: every channel whose row is on screen in the
// list beside it, then each selected layer's own animated channels, once
// each, if shown and sampleable;
// and each of their keys where it draws - at its frame on the shared time
// axis, shifted by a key drag, and at its value on the graph's value axis.
// Keys of unlocked targets are registered as pointer targets over the graph
// itself, and offered to the marquee.
void TimelinePanel::collect_graph(Pass &p,
                                  std::span<const RowContext> visible) {
  const std::span<const scene::ChannelKeyframes> keyframes = p.input.keyframes;
  const std::span<const ElementInfo> elements = p.input.elements;
  const std::span<const doc::NodeId> selection = p.input.selection;
  const gui::Rect area = graph_area(p);
  std::vector<GraphChannel> &channels = p.graph_channels;
  std::vector<GraphKey> &graph_keys = p.graph_keys;
  std::vector<GraphHandle> &handles = p.graph_handles;
  channels.clear();
  graph_keys.clear();
  handles.clear();
  track_handle_drag(p);
  const bool handle_preview =
      handle_drag_.active || handle_drag_.hold_frames > 0;

  std::set<ChannelIdentity> seen;
  const auto add = [&](const ChannelIdentity &id, bool locked) {
    if (!graph_->visible(id) || !seen.insert(id).second) {
      return;
    }
    const scene::ChannelKeyframes *keys =
        channel_for(keyframes, id.target, id.property);
    if (keys != nullptr && handle_preview &&
        id == ChannelIdentity{handle_drag_.key.target,
                              handle_drag_.key.component}) {
      keys = &handle_drag_.preview;
    }
    if (keys == nullptr || keys->frames.empty() ||
        keys->values.size() != keys->frames.size()) {
      return;
    }
    channels.push_back(GraphChannel{id, keys, channel_color(id.property),
                                    graph_->active() == id, locked});
  };
  for (const RowContext &rc : visible) {
    const Row &row = rc.row;
    if (row.kind == Row::Kind::Component || row.kind == Row::Kind::Property) {
      add(ChannelIdentity{row.kind == Row::Kind::Property ? row.property_target
                                                          : row.node,
                          row.kind == Row::Kind::Property
                              ? row.property
                              : group_channel(row.group, row.component)},
          row.locked);
    }
  }
  for (const doc::NodeId node : selection) {
    const ElementInfo *const element = find_element(elements, node);
    for (const scene::ChannelKeyframes &channel : keyframes) {
      if (channel.target == node) {
        add(ChannelIdentity{channel.target, channel.component},
            element != nullptr && element->locked);
      }
    }
  }
  if (graph_->fit_requested() && !channels.empty()) {
    graph_->fit(channels, p.first_i, p.last_i);
  }

  const timeline::ValueAxis &axis = graph_->axis();
  for (std::size_t index = 0U; index < channels.size(); ++index) {
    const GraphChannel &channel = channels[index];
    const scene::ChannelKeyframes &keys = *channel.keys;
    for (std::size_t key = 0U; key < keys.frames.size(); ++key) {
      if (!std::isfinite(keys.values[key])) {
        continue;
      }
      const SelectedKey identity{channel.id.target, channel.id.property,
                                 keys.frames[key]};
      const bool moving =
          keys_.dragging && std::find(keys_.drag_keys.begin(),
                                      keys_.drag_keys.end(),
                                      identity) != keys_.drag_keys.end();
      const core::i64 shown = moving && !keys_.duplicate
                                  ? identity.frame + keys_.drag_delta
                                  : identity.frame;
      GraphKey drawn{index,
                     key,
                     identity.frame,
                     time_axis_.x(static_cast<double>(shown), area.x),
                     axis.y(keys.values[key], area),
                     key_selected(identity)};
      if (moving && keys_.duplicate && keys_.drag_delta != 0) {
        drawn.duplicate_x = time_axis_.x(
            static_cast<double>(identity.frame + keys_.drag_delta), area.x);
      }
      graph_keys.push_back(drawn);
    }
  }

  p.hits.add(HitTarget{mix(id_, "graph"), area, HitPriority::Lane, area,
                       row_part_tag(graph_row, RowPart::Lane)});
  for (std::size_t index = 0U; index < graph_keys.size(); ++index) {
    const GraphKey &key = graph_keys[index];
    const GraphChannel &channel = channels[key.channel];
    if (channel.locked) {
      continue;
    }
    HitTarget target{
        mix(mix(mix(id_, "graph key", channel.id.target.value()), "property",
                static_cast<core::u64>(channel.id.property)),
            "frame", static_cast<core::u64>(key.frame)),
        gui::Rect{key.x - key_hit_radius, key.y - key_hit_radius,
                  key_hit_radius * 2.0F, key_hit_radius * 2.0F},
        HitPriority::Key, gui::Rect{key.x, key.y, 0.0F, 0.0F},
        row_part_tag(graph_row, RowPart::Key, index)};
    target.reach = key_hit_radius;
    p.hits.add(target);
    p.scratch.visible_keys.push_back(FrameScratch::VisibleKeyPoint{
        SelectedKey{channel.id.target, channel.id.property, key.frame}, key.x,
        key.y});
  }

  // Each selected Bezier key's handles, unless its keys are being dragged.
  // A handle answers as far out as a key does, or half way to its key where
  // the two are closer, so the key keeps the side nearest it.
  if (keys_.dragging) {
    return;
  }
  for (std::size_t index = 0U; index < graph_keys.size(); ++index) {
    const GraphKey &key = graph_keys[index];
    const GraphChannel &channel = channels[key.channel];
    if (!key.selected || channel.locked) {
      continue;
    }
    for (const bool incoming : {true, false}) {
      const auto point =
          TimelineGraphView::handle_point(*channel.keys, key.index, incoming);
      if (!point.has_value()) {
        continue;
      }
      const float x = time_axis_.x(point->frame, area.x);
      const float y = axis.y(point->value, area);
      handles.push_back(GraphHandle{index, incoming, x, y});
      const float reach =
          std::min(key_hit_radius, std::hypot(x - key.x, y - key.y) * 0.5F);
      if (reach < 2.0F) {
        continue;
      }
      HitTarget target{
          mix(mix(mix(id_, "graph handle", channel.id.target.value()),
                  "frame", static_cast<core::u64>(key.frame)),
              "side", incoming ? 1U : 2U),
          gui::Rect{x - reach, y - reach, reach * 2.0F, reach * 2.0F},
          HitPriority::Handle, gui::Rect{x, y, 0.0F, 0.0F},
          row_part_tag(graph_row, RowPart::Handle, handles.size() - 1U)};
      target.reach = reach;
      p.hits.add(target);
    }
  }
}

// A press on a handle starts its drag; the key selection stays as it was.
void TimelinePanel::press_handle(Pass &p, std::size_t handle) {
  const GraphHandle &pressed = p.graph_handles[handle];
  const GraphKey &key = p.graph_keys[pressed.key];
  const GraphChannel &channel = p.graph_channels[key.channel];
  const scene::ChannelKeyframes *const input = channel_for(
      p.input.keyframes, channel.id.target, channel.id.property);
  if (input == nullptr) {
    return;
  }
  graph_->set_active(channel.id);
  handle_drag_ = HandleDrag{
      true, SelectedKey{channel.id.target, channel.id.property, key.frame},
      pressed.incoming, *input, *input, 0};
}

// A held handle follows the pointer on the preview copy; its release
// commits the tangent it shows as one edit.
void TimelinePanel::track_handle_drag(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  if (!handle_drag_.active) {
    if (handle_drag_.hold_frames > 0) {
      const scene::ChannelKeyframes *const input =
          channel_for(p.input.keyframes, handle_drag_.key.target,
                      handle_drag_.key.component);
      if (input == nullptr || *input != handle_drag_.base) {
        handle_drag_.hold_frames = 0;
      } else {
        --handle_drag_.hold_frames;
      }
    }
    return;
  }
  const scene::ChannelKeyframes &base = handle_drag_.base;
  const auto found = std::find(base.frames.begin(), base.frames.end(),
                               handle_drag_.key.frame);
  if (found == base.frames.end()) {
    handle_drag_.active = false;
    return;
  }
  const auto index = static_cast<std::size_t>(found - base.frames.begin());
  const gui::Rect area = graph_area(p);
  if (const auto tangent = TimelineGraphView::dragged_tangent(
          base, index, handle_drag_.incoming,
          time_axis_.frame(ctx.pointer_x(), area.x),
          graph_->axis().value(ctx.pointer_y(), area), ctx.alt_held())) {
    handle_drag_.preview = base;
    handle_drag_.preview.tangents[index] = *tangent;
  }
  if (ctx.pointer_down()) {
    return;
  }
  handle_drag_.active = false;
  const std::optional<scene::KeyTangent> &shown =
      handle_drag_.preview.tangents[index];
  if (shown.has_value() && shown != base.tangents[index]) {
    TimelineKeyBatchEdit batch{};
    batch.action = TimelineKeyBatchAction::Tangent;
    batch.keys.push_back(TimelineKeyAddress{handle_drag_.key.target,
                                            handle_drag_.key.component,
                                            handle_drag_.key.frame});
    batch.tangent = *shown;
    p.frame.key_batch = std::move(batch);
    handle_drag_.hold_frames = 60;
  }
}

// The Curves graph over the rows' lanes, and a press on it: a key selects
// and lifts like a dope-sheet key, anywhere else clears, seeks and starts a
// marquee, and holding scrubs.
void TimelinePanel::draw_graph(Pass &p) {
  gui::UiContext &ctx = p.ctx;
  const gui::Rect area = graph_area(p);
  graph_->draw(ctx, area, time_axis_, p.graph_channels, p.graph_keys,
               p.graph_handles);
  const gui::Interaction graph_it = ctx.grab(mix(id_, "graph"), area);
  const bool owned_here = graph_it.pressed && p.press_owner.has_value() &&
                          tag_row(p.press_owner->tag) == graph_row;
  if (owned_here) {
    if (tag_part(p.press_owner->tag) == RowPart::Handle &&
        tag_item(p.press_owner->tag) < p.graph_handles.size()) {
      press_handle(p, tag_item(p.press_owner->tag));
    } else if (tag_part(p.press_owner->tag) == RowPart::Key &&
        tag_item(p.press_owner->tag) < p.graph_keys.size()) {
      const GraphKey &key = p.graph_keys[tag_item(p.press_owner->tag)];
      const GraphChannel &channel = p.graph_channels[key.channel];
      press_key(p, SelectedKey{channel.id.target, channel.id.property,
                               key.frame});
    } else {
      press_lane_background(p, false);
    }
  }
  scrub_held_lane(p, graph_it);
}

// A closed parent's summary in its lane, read-only: a thin bar over the
// frames its descendants' clips and keys span (omitted where a clip bar
// already shows them), and a small tick on every frame they key - in the
// channel's colour when every key there is one channel's, otherwise neutral,
// and larger where several keys meet.
void TimelinePanel::summary_lane_cell(RowContext &rc, bool span_bar) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &graph = p.layout.graph;
  const Row &row = rc.row;
  if (row.summary == no_row) {
    return;
  }
  const RowSummary &summary = p.built.summaries[row.summary];
  const float centre_y = rc.centre_y;
  if (span_bar) {
    const float x0 = time_axis_.x(static_cast<double>(summary.first), graph.x);
    const float x1 = time_axis_.x(static_cast<double>(summary.end), graph.x);
    ctx.panel(gui::Rect{x0, centre_y - 1.5F, std::max(x1 - x0, 2.0F), 3.0F},
              alpha(row.color, row.element_visible ? 0.55F : 0.25F));
  }
  const float left = graph.x - summary_key_radius * 2.0F;
  const float right = graph.x + graph.width + summary_key_radius * 2.0F;
  for (const SummaryKey &key : p.built.keys(summary)) {
    const float x = time_axis_.x(static_cast<double>(key.frame), graph.x);
    if (x < left || x > right) {
      continue;
    }
    const gui::UiColor colour = key.mixed ? ctx.theme().text_muted
                                          : channel_color(key.channel);
    draw_diamond(ctx, x, centre_y,
                 key.count > 1U ? summary_key_radius + 1.0F
                                : summary_key_radius,
                 alpha(colour, 0.9F));
  }
}

} // namespace mediaserver::editor
