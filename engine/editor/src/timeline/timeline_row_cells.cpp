#include "timeline_internal.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
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
  // A right press on a row opens that row's menu once the rows are drawn.

  ctx.push_clip(rows_bounds);
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const Row &row = rows[index];
    const float y = rows_bounds.y + row.top - row_scroll_;
    if (y + row.height <= rows_bounds.y ||
        y >= rows_bounds.y + rows_bounds.height) {
      continue;
    }
    RowContext rc{p, row, index};
    rc.y = y;
    rc.track_number = track_numbers[index];
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
      break;
    }
    lane_press(rc);
    ctx.pop_clip();
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
  auto &row_channel_identity = rc.row_channel_identity;
  auto &is_selected = rc.is_selected;
  auto &layer_like = rc.layer_like;
  auto &row_hovered = rc.row_hovered;
  auto &background = rc.background;
  auto &row_group = rc.row_group;
  auto &open = rc.open;
  auto &content_x = rc.content_x;
  auto &lock_x = rc.lock_x;
  auto &interaction_y = rc.interaction_y;

  if (row.kind == Row::Kind::Layer && !row.surface) {
    const float top = std::max(y, rows_bounds.y);
    const float bottom = std::min(y + row.height, rows_bounds.y + rows_bounds.height);
    replacement_targets_.push_back({row.node,
        {rows_bounds.x, top, rows_bounds.width, bottom - top}});
  }
  rc.centre_y = y + row.height * 0.5F;
  if (row.kind == Row::Kind::Component || row.kind == Row::Kind::Property) {
    row_channel_identity = ChannelIdentity{
        row.kind == Row::Kind::Property ? row.property_target : row.node,
        row.kind == Row::Kind::Property
            ? row.property
            : group_channel(row.group, row.component)};
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
  rc.open = row.hierarchy
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
  rc.interaction_y = std::max(y, rows_bounds.y);
  const float interaction_bottom =
      std::min(y + row.height, rows_bounds.y + rows_bounds.height);
  rc.interaction_height =
      std::max(interaction_bottom - interaction_y, 0.0F);

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
  name_bounds = gui::Rect{name_x, y, std::max(lock_x - name_x - 4.0F, 0.0F),
                          row.height};
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
  // A surface keeps its badge, just inside the padlock.
  const float name_right =
      (has_toggles ? lock_x : graph.x) - (row.surface ? 60.0F : 0.0F) -
      (has_toggles ? 0.0F : 64.0F);
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
}

// A parent's Properties foldout: only its name.
void TimelinePanel::properties_header_list_cell(RowContext &rc) {
  property_label(rc);
}

// A property group - Position, Rotation, Scale, Color: its name and the
// diamond that keys every component at the playhead.
void TimelinePanel::property_group_list_cell(RowContext &rc) {
  property_label(rc);
  row_key_button(rc);
}

// One channel - a component or a property: its name, its visibility
// swatch and its key diamond.
void TimelinePanel::channel_list_cell(RowContext &rc) {
  property_label(rc);
  channel_visibility_toggle(rc);
  row_key_button(rc);
}

// A property, a property group or the Properties foldout: its name.
void TimelinePanel::property_label(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &graph = p.layout.graph;
  const Row &row = rc.row;
  auto &y = rc.y;
  auto &row_channel_identity = rc.row_channel_identity;
  auto &content_x = rc.content_x;
  auto &name_bounds = rc.name_bounds;

  // A property: named in the colour its keys and curve are drawn in, so
  // the list and the lanes read as one table.
  name_bounds = gui::Rect{content_x, y,
                          std::max(graph.x - content_x - 48.0F, 0.0F),
                          row.height};
  ctx.label(name_bounds, row.label,
            row.kind == Row::Kind::Component ||
                    row.kind == Row::Kind::Property
                ? channel_color(row_channel_identity->property)
                : row.color);
}

// The swatch that shows or hides one channel's keys and curve.
void TimelinePanel::channel_visibility_toggle(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &graph = p.layout.graph;
  const Row &row = rc.row;
  auto &row_channel_identity = rc.row_channel_identity;
  auto &interaction_y = rc.interaction_y;
  auto &interaction_height = rc.interaction_height;

  if (row_channel_identity.has_value()) {
    const bool channel_hidden =
        hidden_channels_.contains(*row_channel_identity);
    const gui::Rect visibility_bounds{
        graph.x - 41.0F,
        interaction_y + std::max((interaction_height - 14.0F) * 0.5F, 0.0F),
        14.0F, std::min(interaction_height, 14.0F)};
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
      active_channel_ = *row_channel_identity;
      if (channel_hidden) {
        hidden_channels_.erase(*row_channel_identity);
      } else {
        hidden_channels_.insert(*row_channel_identity);
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
  const std::span<const scene::ChannelKeyframes> keyframes = p.input.keyframes;
  const std::span<const ElementInfo> elements = p.input.elements;
  const std::span<const output::ProjectionSurface> surfaces = p.input.surfaces;
  TimelinePanelFrame &frame = p.frame;
  const gui::Rect &graph = p.layout.graph;
  const Row &row = rc.row;
  auto &background = rc.background;
  auto &interaction_y = rc.interaction_y;
  auto &interaction_height = rc.interaction_height;

  // Property-local key controls: the group diamond keys all three axes;
  // a component diamond keys only that channel. A filled diamond means
  // there is already a key at the playhead, while a hollow bright one
  // means the channel is animated elsewhere.
  if (!row.locked && (row.kind == Row::Kind::Group ||
                      row.kind == Row::Kind::Component ||
                      row.kind == Row::Kind::Property)) {
    const ElementInfo *const row_element = find_element(elements, row.node);
    const output::ProjectionSurface *const row_surface =
        row.surface ? find_surface(surfaces, row.node) : nullptr;
    if ((row_surface != nullptr) || row_element != nullptr) {
      bool animated = false;
      bool keyed_here = true;
      const core::i64 show_frame = signed_frame(timeline.show_frame);
      const core::u8 first_component =
          row.kind == Row::Kind::Property
              ? static_cast<core::u8>(row.property)
              : static_cast<core::u8>(group_channel(
                    row.group,
                    row.kind == Row::Kind::Component ? row.component : 0U));
      const core::u8 component_count =
          row.kind == Row::Kind::Group ? row.component_count : 1U;
      const doc::NodeId key_target =
          row.kind == Row::Kind::Property ? row.property_target : row.node;
      for (core::u8 offset = 0U; offset < component_count; ++offset) {
        const auto component =
            static_cast<scene::TransformChannel>(first_component + offset);
        const scene::ChannelKeyframes *const authored =
            channel_for(keyframes, key_target, component);
        animated =
            animated || (authored != nullptr && !authored->frames.empty());
        keyed_here =
            keyed_here && authored != nullptr &&
            std::find(authored->frames.begin(), authored->frames.end(),
                      show_frame) != authored->frames.end();
      }

      const gui::Rect key_bounds{
          graph.x - 22.0F,
          interaction_y + std::max((interaction_height - 18.0F) * 0.5F, 0.0F),
          18.0F, std::min(interaction_height, 18.0F)};
      if (key_button(ctx, mix(id_, "row key", row_identity(row)),
                     key_bounds, animated, keyed_here, background)) {
        for (core::u8 offset = 0U; offset < component_count; ++offset) {
          const auto component =
              static_cast<scene::TransformChannel>(first_component + offset);
          frame.keys.push_back(TimelineKeyEdit{
              key_target, component, timeline.show_frame,
              row.kind == Row::Kind::Property
                  ? row.property_value
                  : (row_surface != nullptr
                         ? surface_component_value(*row_surface, component)
                         : component_value(*row_element, component)),
              false, keyed_here});
        }
      }
    }
  }

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
      if (row.hierarchy) {
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
  const std::span<const ElementInfo> elements = p.input.elements;
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
  auto &clip_timing = rc.clip_timing;

  // The time lane: a clip bar for a placed media layer, a translucent
  // authored-span bar for every other layer, aggregate diamonds for
  // groups, and draggable diamonds for component channels.
  // Filled in for a placed clip row, and consulted by the lane
  // interaction below so that a press on the bar becomes a clip gesture
  // rather than a scrub.
  rc.clip_element = nullptr;
  if (row.kind == Row::Kind::Layer && !row.surface) {
    const ElementInfo *const layer_element = find_element(elements, row.node);
    if (layer_element != nullptr &&
        layer_element->kind == ElementKind::Media &&
        layer_element->timing.placed()) {
      clip_element = layer_element;
      clip_timing =
          clip_.drag != ClipDrag::None && clip_.node == row.node
              ? clip_.proposed
              : (clip_.pending_edit.has_value() &&
                         clip_.pending_edit->clip == row.node
                     ? clip_.pending_edit->timing
                     : layer_element->timing);
    }
  }
  if (clip_element != nullptr) {
    clip_bar_cell(rc);
  } else if (row.kind == Row::Kind::Layer) {
    // A layer with no placement is present for the whole show: a quiet
    // band in its colour, so it reads as "always on" rather than as a clip.
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
  }

}

// A placed clip: body, filmstrip and waveform, trim edges, ramps and fade
// knobs, its name, and what a press would do.
void TimelinePanel::clip_bar_cell(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const gui::Rect &graph = p.layout.graph;
  const auto frame_to_x = [&](double value) noexcept {
    return time_axis_.x(value, graph.x);
  };
  const Row &row = rc.row;
  auto &y = rc.y;
  auto &clip_timing = rc.clip_timing;
  auto &clip_bar = rc.clip_bar;

  const float x0 = frame_to_x(static_cast<double>(clip_timing.start));
  const float x1 = frame_to_x(
      static_cast<double>(clip_timing.start + clip_timing.duration));
  clip_bar =
      gui::Rect{x0, y + 3.0F, std::max(x1 - x0, 2.0F), row.height - 6.0F};
  const bool live =
      clip_.drag != ClipDrag::None && clip_.node == row.node;
  ctx.panel(clip_bar, alpha(row.color, live ? 0.42F : 0.30F));
  const bool drew_filmstrip = draw_clip_filmstrip(rc, live);
  draw_clip_waveform(rc, drew_filmstrip);
  draw_clip_edges(rc);
  draw_clip_ramps(rc);
  draw_clip_label(rc);
  draw_clip_hover(rc);
}

bool TimelinePanel::draw_clip_filmstrip(RowContext &rc, bool live) {
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
  auto &clip_bar = rc.clip_bar;

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
    if (looped) {
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

void TimelinePanel::draw_clip_waveform(RowContext &rc, bool drew_filmstrip) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const EditorTimeline &timeline = p.input.timeline;
  const gui::Rect &graph = p.layout.graph;
  const auto x_to_frame = [&](float x) noexcept {
    return time_axis_.frame(x, graph.x);
  };
  const Row &row = rc.row;
  auto &centre_y = rc.centre_y;
  auto &clip_element = rc.clip_element;
  auto &clip_timing = rc.clip_timing;
  auto &clip_bar = rc.clip_bar;

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
  const gui::Rect &graph = p.layout.graph;
  const auto frame_to_x = [&](double value) noexcept {
    return time_axis_.x(value, graph.x);
  };
  const Row &row = rc.row;
  auto &clip_timing = rc.clip_timing;
  auto &clip_bar = rc.clip_bar;
  auto &clip_fade_in_x = rc.clip_fade_in_x;
  auto &clip_fade_out_x = rc.clip_fade_out_x;

  // The ramps, drawn the way an editor draws them: a diagonal from
  // the bar's foot at the cut to its head where the clip is fully in.
  clip_fade_in_x = frame_to_x(
      static_cast<double>(clip_timing.start + clip_timing.fade_in));
  clip_fade_out_x = frame_to_x(static_cast<double>(
      clip_timing.start + clip_timing.duration - clip_timing.fade_out));
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
  const auto fade_handle = [&](float handle_x, bool engaged) {
    const bool near =
        edit_tool_ == TimelineEditTool::Standard &&
        !ctx.pointer_blocked() && clip_.drag == ClipDrag::None &&
        std::fabs(ctx.pointer_x() - handle_x) <= fade_handle_reach &&
        ctx.pointer_y() >= clip_bar.y - 4.0F &&
        ctx.pointer_y() <= clip_bar.y + fade_handle_reach;
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
  fade_handle(clip_fade_in_x, clip_.drag == ClipDrag::FadeIn &&
                                  clip_.node == row.node);
  fade_handle(clip_fade_out_x, clip_.drag == ClipDrag::FadeOut &&
                                   clip_.node == row.node);

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
    if (!clip_element->on_air) {
      name += "  off air";
    }
    const float text_width =
        std::min(ctx.font_width(name) + 8.0F, name_right - name_left);
    ctx.panel(gui::Rect{name_left - 4.0F, clip_bar.y + 2.0F, text_width,
                        clip_bar.height - 4.0F},
              alpha(ctx.theme().background, 0.55F));
    ctx.label(gui::Rect{name_left, clip_bar.y, name_right - name_left,
                        clip_bar.height},
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
  auto &clip_fade_in_x = rc.clip_fade_in_x;
  auto &clip_fade_out_x = rc.clip_fade_out_x;

  const float grab = edge_grab_width(clip_bar.width);
  // What a press here would do, shown before it is done: the trim edge
  // under a Selection pointer lights, and the razor draws its cut.
  if (!row.locked &&
      clip_bar.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      clip_.drag == ClipDrag::None && !ctx.pointer_blocked()) {
    const float px = ctx.pointer_x();
    if (edit_tool_ == TimelineEditTool::Razor) {
      const float cut_x =
          frame_to_x(static_cast<double>(razor_frame(p, px)));
      ctx.panel(gui::Rect{cut_x - 0.5F, clip_bar.y - 2.0F, 1.5F,
                          clip_bar.height + 4.0F},
                ctx.theme().warning);
    } else if (edit_tool_ == TimelineEditTool::Standard) {
      const bool on_fade =
          ctx.pointer_y() <= clip_bar.y + fade_handle_reach &&
          (std::fabs(px - clip_fade_in_x) <= fade_handle_reach ||
           std::fabs(px - clip_fade_out_x) <= fade_handle_reach);
      if (on_fade) {
        // The handle itself shows the hover.
      } else if (px <= clip_bar.x + grab) {
        ctx.panel(gui::Rect{clip_bar.x, clip_bar.y, grab, clip_bar.height},
                  row.color);
        ctx.set_cursor(gui::Cursor::ResizeHorizontal);
      } else if (px >= clip_bar.x + clip_bar.width - grab) {
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

  const auto draw_frames = [&](const std::vector<core::i64> *frames,
                               gui::UiColor color) {
    if (frames == nullptr) {
      return;
    }
    for (const core::i64 key : *frames) {
      draw_diamond(ctx, frame_to_x(static_cast<double>(key)), centre_y,
                   key_radius, color);
    }
  };

  for (core::u8 component = 0U; component < row.component_count;
       ++component) {
    const auto aggregate = group_channel(row.group, component);
    if (hidden_channels_.contains(ChannelIdentity{row.node, aggregate})) {
      continue;
    }
    draw_frames(frames_for(keyframes, row.node, aggregate),
                alpha(channel_color(aggregate), 0.58F));
  }

}

// One channel's lane: its keys, or in Curves mode its curve.
void TimelinePanel::channel_lane_cell(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const std::span<const scene::ChannelKeyframes> keyframes = p.input.keyframes;
  FrameScratch &scratch = p.scratch;
  const gui::Rect &graph = p.layout.graph;
  const auto frame_to_x = [&](double value) noexcept {
    return time_axis_.x(value, graph.x);
  };
  const Row &row = rc.row;
  auto &y = rc.y;
  auto &centre_y = rc.centre_y;
  auto &row_channel_identity = rc.row_channel_identity;
  auto &component_frames = rc.component_frames;
  auto &component_x = rc.component_x;
  auto &component_y = rc.component_y;
  auto &channel = rc.channel;

  const scene::ChannelKeyframes *component_channel = nullptr;
  bool component_visible = true;
  gui::UiColor component_color = ctx.theme().keyframe;
  channel = row_channel_identity->property;
  const doc::NodeId channel_target = row_channel_identity->target;
  component_visible = !hidden_channels_.contains(*row_channel_identity);
  component_color = channel_color(channel);
  component_channel = component_visible
                          ? channel_for(keyframes, channel_target, channel)
                          : nullptr;
  component_frames =
      component_channel != nullptr ? &component_channel->frames : nullptr;
  if (component_frames != nullptr) {
    component_x.reserve(component_frames->size());
    component_y.reserve(component_frames->size());

    double minimum_value = std::numeric_limits<double>::infinity();
    double maximum_value = -std::numeric_limits<double>::infinity();
    const bool has_values = component_channel->values.size() ==
                            component_channel->frames.size();
    if (graph_mode_ && has_values) {
      for (const double value : component_channel->values) {
        if (std::isfinite(value)) {
          minimum_value = std::min(minimum_value, value);
          maximum_value = std::max(maximum_value, value);
        }
      }
    }
    const double value_span = maximum_value - minimum_value;
    const auto value_y = [&](std::size_t key_index) noexcept {
      if (!graph_mode_ || !has_values || !std::isfinite(value_span) ||
          value_span <= 1.0e-12) {
        return centre_y;
      }
      const double value = component_channel->values[key_index];
      if (!std::isfinite(value)) {
        return centre_y;
      }
      const double normal = 0.5 +
                            (((maximum_value - value) / value_span) - 0.5) *
                                static_cast<double>(curve_value_zoom_) +
                            static_cast<double>(curve_value_pan_);
      return y + 4.0F + static_cast<float>(normal) * (row.height - 8.0F);
    };

    if (graph_mode_ && has_values && std::isfinite(value_span) &&
        value_span > 1.0e-12) {
      ctx.draw_list().add_line(graph.x, centre_y, graph.x + graph.width,
                               centre_y, 1.0F,
                               alpha(component_color, 0.18F));
      std::array<char, 32U> maximum_label{};
      std::array<char, 32U> minimum_label{};
      const int maximum_length =
          std::snprintf(maximum_label.data(), maximum_label.size(), "%.3g",
                        maximum_value);
      const int minimum_length =
          std::snprintf(minimum_label.data(), minimum_label.size(), "%.3g",
                        minimum_value);
      ctx.label(gui::Rect{graph.x + 3.0F, y, std::min(graph.width, 52.0F),
                          row.height * 0.5F},
                std::string_view{
                    maximum_label.data(),
                    static_cast<std::size_t>(std::max(maximum_length, 0))},
                alpha(component_color, 0.68F));
      ctx.label(gui::Rect{graph.x + 3.0F, centre_y,
                          std::min(graph.width, 52.0F), row.height * 0.5F},
                std::string_view{
                    minimum_label.data(),
                    static_cast<std::size_t>(std::max(minimum_length, 0))},
                alpha(component_color, 0.68F));
    }

    float previous_x = 0.0F;
    float previous_y = centre_y;
    bool have_previous = false;
    for (std::size_t key_index = 0U; key_index < component_frames->size();
         ++key_index) {
      const core::i64 key_frame = (*component_frames)[key_index];
      const SelectedKey identity{channel_target, channel, key_frame};
      const auto moving =
          std::find(keys_.drag_keys.begin(), keys_.drag_keys.end(), identity);
      const bool moving_key = keys_.dragging && moving != keys_.drag_keys.end();
      const core::i64 shown_frame = moving_key && !keys_.duplicate
                                        ? key_frame + keys_.drag_delta
                                        : key_frame;
      const float key_x = frame_to_x(static_cast<double>(shown_frame));
      const float key_y = value_y(key_index);
      if (graph_mode_ && have_previous) {
        const auto interpolation =
            component_channel->interpolations.size() ==
                    component_channel->frames.size()
                ? component_channel->interpolations[key_index - 1U]
                : scene::KeyInterpolation::Linear;
        if (interpolation == scene::KeyInterpolation::Hold) {
          ctx.draw_list().add_line(previous_x, previous_y, key_x,
                                   previous_y, 1.0F,
                                   alpha(component_color, 0.72F));
          ctx.draw_list().add_line(key_x, previous_y, key_x, key_y, 1.0F,
                                   alpha(component_color, 0.72F));
        } else if (interpolation == scene::KeyInterpolation::Cubic) {
          float curve_x = previous_x;
          float curve_y = previous_y;
          constexpr std::size_t samples = 8U;
          for (std::size_t sample = 1U; sample <= samples; ++sample) {
            const float t =
                static_cast<float>(sample) / static_cast<float>(samples);
            const float eased = t * t * (3.0F - 2.0F * t);
            const float next_x = previous_x + (key_x - previous_x) * t;
            const float next_y = previous_y + (key_y - previous_y) * eased;
            ctx.draw_list().add_line(curve_x, curve_y, next_x, next_y, 1.0F,
                                     alpha(component_color, 0.72F));
            curve_x = next_x;
            curve_y = next_y;
          }
        } else {
          ctx.draw_list().add_line(previous_x, previous_y, key_x, key_y,
                                   1.0F, alpha(component_color, 0.72F));
        }
      }
      have_previous = true;
      previous_x = key_x;
      previous_y = key_y;
      component_x.push_back(key_x);
      component_y.push_back(key_y);

      const bool is_key_selected = key_selected(identity);
      if (is_key_selected) {
        draw_diamond(ctx, key_x, key_y, key_radius + 2.0F,
                     alpha(ctx.theme().accent, 0.50F));
      }
      draw_diamond(ctx, key_x, key_y,
                   is_key_selected ? key_radius + 0.5F : key_radius,
                   is_key_selected ? ctx.theme().playhead
                                   : component_color);
      if (moving_key && keys_.duplicate && keys_.drag_delta != 0) {
        const float duplicate_x =
            frame_to_x(static_cast<double>(key_frame + keys_.drag_delta));
        draw_diamond(ctx, duplicate_x, key_y, key_radius + 0.5F,
                     ctx.theme().accent);
      }
      if (!row.locked) {
        scratch.visible_keys.push_back(FrameScratch::VisibleKeyPoint{identity, key_x, key_y});
      }
    }
  }
}

// A press in a row's lane: a razor cut, a clip gesture, a key, a marquee
// or a scrub.
void TimelinePanel::lane_press(RowContext &rc) {
  Pass &p = rc.p;
  gui::UiContext &ctx = p.ctx;
  const std::span<const scene::ChannelKeyframes> keyframes = p.input.keyframes;
  const std::span<const ElementInfo> elements = p.input.elements;
  TimelinePanelFrame &frame = p.frame;
  FrameScratch &scratch = p.scratch;
  const gui::Rect &rows_bounds = p.layout.rows;
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
  auto &clip_fade_in_x = rc.clip_fade_in_x;
  auto &clip_fade_out_x = rc.clip_fade_out_x;
  auto &component_frames = rc.component_frames;
  auto &component_x = rc.component_x;
  auto &component_y = rc.component_y;
  auto &channel = rc.channel;

  const gui::Interaction lane_it = ctx.grab(
      mix(id_, "lane", row_identity(row)),
      gui::Rect{graph.x, interaction_y, graph.width, interaction_height});
  if (ctx.secondary_pointer_pressed() && interaction_height > 0.0F &&
      ctx.pointer_y() >= interaction_y &&
      ctx.pointer_y() < interaction_y + interaction_height &&
      ctx.pointer_x() >= rows_bounds.x &&
      ctx.pointer_x() < rows_bounds.x + rows_bounds.width) {
    scratch.right_row = index;
  }
  // The razor cuts where it lands and starts no drag; Shift cuts every
  // unlocked clip under that frame, on every track.
  // A native event batch may already contain motion or release after the
  // down edge. Classify the gesture where it began, not at that endpoint.
  const float press_x = ctx.pointer_press_x();
  const float press_y = ctx.pointer_press_y();
  const bool razor_press = !row.locked && lane_it.pressed &&
                           clip_element != nullptr &&
                           edit_tool_ == TimelineEditTool::Razor &&
                           clip_bar.contains(press_x, press_y);
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
  } else if (!row.locked && lane_it.pressed && clip_element != nullptr &&
             (clip_bar.contains(press_x, press_y) ||
              (edit_tool_ == TimelineEditTool::Standard &&
               press_y >= clip_bar.y - 4.0F &&
               press_y <= clip_bar.y + fade_handle_reach &&
               (std::fabs(press_x - clip_fade_in_x) <= fade_handle_reach ||
                std::fabs(press_x - clip_fade_out_x) <= fade_handle_reach)))) {
    clip_.press_consumed = true;
    keys_.selected.clear();
    keys_.drag_keys.clear();
    keys_.dragging = false;
    keys_.duplicate = false;
    markers_.selected.clear();
    const float grab = edge_grab_width(clip_bar.width);
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
      // Standard placement editing. Fade handles sit on the bar's
      // top edge and take precedence over the trim strips.
      const bool on_top = press_y <= clip_bar.y + fade_handle_reach;
      const float to_in = std::fabs(px - clip_fade_in_x);
      const float to_out = std::fabs(px - clip_fade_out_x);
      if (on_top && to_in <= fade_handle_reach && to_in <= to_out) {
        clip_.drag = ClipDrag::FadeIn;
      } else if (on_top && to_out <= fade_handle_reach) {
        clip_.drag = ClipDrag::FadeOut;
      } else if (px <= clip_bar.x + grab) {
        clip_.drag = ClipDrag::TrimIn;
      } else if (px >= clip_bar.x + clip_bar.width - grab) {
        clip_.drag = ClipDrag::TrimOut;
      } else {
        clip_.drag = ClipDrag::Move;
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
  } else if (lane_it.pressed) {
    bool hit_key = false;
    if ((row.kind == Row::Kind::Component ||
         row.kind == Row::Kind::Property) &&
        component_frames != nullptr) {
      for (std::size_t key_index = 0U; key_index < component_frames->size();
           ++key_index) {
        const core::i64 key_frame = (*component_frames)[key_index];
        const float dx = component_x[key_index] - press_x;
        const float dy = component_y[key_index] - press_y;
        if (dx * dx + dy * dy <= key_hit_radius * key_hit_radius) {
          const SelectedKey hit{row.kind == Row::Kind::Property
                                    ? row.property_target
                                    : row.node,
                                channel, key_frame};
          active_channel_ = ChannelIdentity{hit.target, hit.component};
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
              const auto *const authored = frames_for(
                  keyframes, selected_key.target, selected_key.component);
              if (authored != nullptr &&
                  std::find(authored->begin(), authored->end(),
                            selected_key.frame) != authored->end()) {
                keys_.drag_keys.push_back(selected_key);
              }
            }
            if (keys_.drag_keys.empty()) {
              keys_.drag_keys.push_back(hit);
            }
            keys_.dragging = true;
            // Alt is latched at press: releasing the modifier
            // before the pointer does not turn a duplicate into
            // a destructive retime halfway through the gesture.
            keys_.duplicate = ctx.alt_held();
            keys_.drag_anchor_frame = key_frame;
            keys_.drag_delta = 0;
            keys_.marquee_tracking = false;
            keys_.marquee_active = false;
            keys_.lasso_points.clear();
          }
          hit_key = true;
          markers_.selected.clear();
          break;
        }
      }
    }
    if (!hit_key && !ctx.ctrl_held() && !ctx.shift_held()) {
      TimelineSelectionRequest clear{};
      clear.action = SelectionAction::Clear;
      frame.selection = std::move(clear);
    }
    if (!hit_key && !row.locked) {
      keys_.marquee_base = keys_.selected;
      keys_.marquee_shift = ctx.shift_held();
      keys_.marquee_ctrl = ctx.ctrl_held();
      if (!keys_.marquee_shift && !keys_.marquee_ctrl) {
        keys_.selected.clear();
      }
      keys_.marquee_tracking = true;
      keys_.marquee_active = false;
      keys_.marquee_start_x = press_x;
      keys_.marquee_start_y = press_y;
      keys_.lasso_points.clear();
      if (keys_.lasso_mode) {
        keys_.lasso_points.push_back(
            gui::UiPoint{keys_.marquee_start_x, keys_.marquee_start_y});
      }
      frame.seek_to_frame =
          rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
    } else if (row.locked) {
      frame.seek_to_frame =
          rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
    }
  }
  if (lane_it.held && !keys_.dragging && !keys_.marquee_active &&
      clip_.drag == ClipDrag::None && !clip_.press_consumed &&
      reorder_.layer_node == doc::NodeId{} &&
      reorder_.lane_node == doc::NodeId{}) {
    frame.seek_to_frame =
        rounded_frame(x_to_frame(ctx.pointer_x()), first, last);
  }
}

} // namespace mediaserver::editor
