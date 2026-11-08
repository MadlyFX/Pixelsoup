#include <mediaserver/editor/scene_panel.hpp>

#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/gui/shapes.hpp>

#include "workspace_panel_helpers.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mediaserver::editor {

using namespace panel_helpers;

namespace {

// The Scene outliner is the document's hierarchy, not a sort by kind: groups
// hold their children, Blender collections hold theirs, and siblings stand in
// the same front-to-back order the Timeline's layer list shows them in.
[[nodiscard]] doc::NodeId outliner_parent(const ElementInfo &element) noexcept {
  return element.linked ? element.linked_parent
                        : element.parent_group.value_or(doc::NodeId{});
}

[[nodiscard]] bool outliner_lists(const ElementInfo &element) noexcept {
  // A track is a timeline header, and the environment is the Environment
  // settings row above the tree; listing either again would put one thing in
  // two places with two ways to select it.
  return element.kind != ElementKind::Track &&
         element.kind != ElementKind::Environment;
}

[[nodiscard]] bool outliner_orderable(const ElementInfo &element) noexcept {
  return element.kind == ElementKind::Object ||
         element.kind == ElementKind::Media ||
         element.kind == ElementKind::Model ||
         element.kind == ElementKind::ParticleEffect ||
         element.kind == ElementKind::Group || is_light(element.kind);
}

[[nodiscard]] bool outliner_groupable(const ElementInfo &element) noexcept {
  return !element.linked &&
         (element.kind == ElementKind::Object ||
          element.kind == ElementKind::Media ||
          element.kind == ElementKind::Model ||
          element.kind == ElementKind::ParticleEffect ||
          element.kind == ElementKind::Group);
}

struct OutlinerRow final {
  const ElementInfo *element{};
  std::size_t depth{};
  bool has_children{};
};

enum class SceneMenuCommand : core::u8 {
  Rename,
  ToggleVisible,
  ToggleLocked,
  Highlight,
  Group,
  Ungroup,
  MoveOut,
  NewGroup,
  NewVehicleProcessGroup,
  Delete,
};

constexpr float scene_toolbar_height = 32.0F;
constexpr float scene_footer_height = 30.0F;
constexpr float scene_drag_threshold = 5.0F;

} // namespace

void ScenePanel::begin_rename(gui::UiContext &ctx, const ElementInfo &element) {
  if (element.locked || element.linked) {
    return;
  }
  rename_.begin(ctx, element.node.value(), element_display_name(element));
}

std::optional<SceneSelectionRequest>
ScenePanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                 std::span<const ElementInfo> elements,
                 std::span<const doc::NodeId> selection, doc::NodeId primary,
                 bool show_settings, SceneSettingsTarget settings_selection,
                 std::span<const output::ProjectionSurface> surfaces,
                 doc::NodeId selected_surface) {
  std::optional<SceneSelectionRequest> request;
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    rename_.end_frame(ctx);
    return request;
  }
  const gui::Theme &theme = ctx.theme();
  const float pointer_x = ctx.pointer_x();
  const float pointer_y = ctx.pointer_y();
  if (ctx.pointer_pressed() || ctx.secondary_pointer_pressed()) {
    focused_ = bounds.contains(pointer_x, pointer_y);
  }

  ctx.push_clip(bounds);
  ctx.panel(bounds);

  const auto find = [&](doc::NodeId node) -> const ElementInfo * {
    for (const ElementInfo &element : elements) {
      if (element.node == node) {
        return &element;
      }
    }
    return nullptr;
  };

  // ---------------------------------------------------------------- tree
  std::map<doc::NodeId, std::vector<const ElementInfo *>> children;
  std::set<doc::NodeId> listed_nodes;
  std::size_t listed_count = 0U;
  for (const ElementInfo &element : elements) {
    if (outliner_lists(element)) {
      listed_nodes.insert(element.node);
      ++listed_count;
    }
  }
  for (const ElementInfo &element : elements) {
    if (!outliner_lists(element)) {
      continue;
    }
    const doc::NodeId parent = outliner_parent(element);
    children[parent != element.node && listed_nodes.contains(parent)
                 ? parent
                 : doc::NodeId{}]
        .push_back(&element);
  }
  for (auto &[parent, siblings] : children) {
    // Front first, as in the layer stack: a higher group_order is nearer the
    // camera and sits higher in the list. Unordered kinds (cameras) follow.
    std::stable_sort(siblings.begin(), siblings.end(),
                     [](const ElementInfo *left, const ElementInfo *right) {
                       const bool left_ordered = outliner_orderable(*left);
                       const bool right_ordered = outliner_orderable(*right);
                       if (left_ordered != right_ordered) {
                         return left_ordered;
                       }
                       return left_ordered &&
                              left->group_order > right->group_order;
                     });
  }

  // Reveal a newly selected element wherever it sits, the way the Timeline
  // opens to it.
  if (primary != revealed_) {
    revealed_ = primary;
    const ElementInfo *walk = find(primary);
    std::size_t guard = 0U;
    while (walk != nullptr && guard++ < elements.size()) {
      const doc::NodeId parent = outliner_parent(*walk);
      if (parent == doc::NodeId{} || parent == walk->node) {
        break;
      }
      collapsed_.erase(parent);
      walk = find(parent);
    }
  }

  // Search keeps a match's ancestors so its place in the hierarchy is never
  // lost, and opens every branch while it applies.
  std::set<doc::NodeId> matching;
  if (!search_.empty()) {
    for (const ElementInfo &element : elements) {
      if (!outliner_lists(element) ||
          (!matches(element_display_name(element), search_) &&
           !matches(kind_name(element), search_))) {
        continue;
      }
      const ElementInfo *walk = &element;
      std::size_t guard = 0U;
      while (walk != nullptr && guard++ <= elements.size() &&
             matching.insert(walk->node).second) {
        const doc::NodeId parent = outliner_parent(*walk);
        walk = parent == walk->node ? nullptr : find(parent);
      }
    }
  }

  std::vector<OutlinerRow> rows;
  rows.reserve(listed_count);
  {
    std::set<doc::NodeId> visited;
    const auto append = [&](const auto &self, const ElementInfo *element,
                            std::size_t depth) -> void {
      if (!visited.insert(element->node).second) {
        return;
      }
      if (!search_.empty() && !matching.contains(element->node)) {
        return;
      }
      const auto found = children.find(element->node);
      const bool has_children =
          found != children.end() && !found->second.empty();
      rows.push_back(OutlinerRow{element, depth, has_children});
      if (has_children &&
          (!search_.empty() || !collapsed_.contains(element->node))) {
        for (const ElementInfo *child : found->second) {
          self(self, child, depth + 1U);
        }
      }
    };
    if (const auto roots = children.find(doc::NodeId{});
        roots != children.end()) {
      for (const ElementInfo *root : roots->second) {
        append(append, root, 0U);
      }
    }
  }
  std::vector<doc::NodeId> visible_order;
  visible_order.reserve(rows.size());
  for (const OutlinerRow &row : rows) {
    if (!row.element->locked) {
      visible_order.push_back(row.element->node);
    }
  }
  std::vector<const output::ProjectionSurface *> surface_rows;
  surface_rows.reserve(surfaces.size());
  for (const output::ProjectionSurface &surface : surfaces) {
    if (search_.empty() || matches(surface.name, search_) ||
        matches("LED output physical surface", search_)) {
      surface_rows.push_back(&surface);
    }
  }

  // ------------------------------------------------------------- toolbar
  gui::Rect area = bounds;
  const gui::Rect toolbar = gui::cut_top(area, scene_toolbar_height);
  const gui::Rect footer = gui::cut_bottom(area, scene_footer_height);
  ctx.panel(toolbar, theme.header);
  {
    gui::Rect rule = toolbar;
    ctx.panel(gui::cut_bottom(rule, 1.0F), theme.divider);
  }
  std::array<char, 32U> count_text{};
  const int count_length = std::snprintf(
      count_text.data(), count_text.size(), "%llu",
      static_cast<unsigned long long>(listed_count));
  const std::string_view count{
      count_text.data(), static_cast<std::size_t>(std::max(count_length, 0))};
  // The search field, and the element count after it.
  gui::Rect search = toolbar;
  const gui::Rect count_box =
      gui::cut_right(search, ctx.font_width(count) + 12.0F);
  search_field(ctx, gui::ui_id(id_, "scene.search"), gui::inset(search, 0.0F, 2.0F),
               search_, "Search scene");
  ctx.label(gui::inset(count_box, gui::Padding{2.0F, 0.0F, 0.0F, 0.0F}), count,
            theme.text_muted);
  ctx.tooltip(count_box, "Elements in this scene");

  // ---------------------------------------------------------------- list
  const gui::Rect list_bounds = area;
  constexpr float row_height = list_metrics::row_height;
  const std::size_t settings_rows = show_settings ? 2U : 0U;
  const float content_height =
      static_cast<float>(settings_rows + rows.size() + surface_rows.size()) *
          row_height +
      (show_settings ? 6.0F : 0.0F);

  // Drag state from earlier frames: whether a press has travelled far enough
  // to be a move rather than a click.
  if (drag_node_ != doc::NodeId{}) {
    const ElementInfo *const dragged = find(drag_node_);
    if (dragged == nullptr || dragged->locked) {
      drag_node_ = doc::NodeId{};
      dragging_ = false;
    } else if (!dragging_ && ctx.pointer_down() &&
               std::fabs(pointer_x - drag_x_) + std::fabs(pointer_y - drag_y_) >
                   scene_drag_threshold) {
      dragging_ = true;
    }
  }
  const auto descends_from = [&](doc::NodeId node, doc::NodeId ancestor) {
    const ElementInfo *walk = find(node);
    std::size_t guard = 0U;
    while (walk != nullptr && guard++ <= elements.size()) {
      if (walk->node == ancestor) {
        return true;
      }
      const doc::NodeId parent = outliner_parent(*walk);
      if (parent == walk->node) {
        break;
      }
      walk = find(parent);
    }
    return false;
  };
  // What the moving rows are: the whole selection when the dragged row is
  // part of it, otherwise just that row.
  std::vector<doc::NodeId> moving;
  if (dragging_) {
    if (std::find(selection.begin(), selection.end(), drag_node_) !=
        selection.end()) {
      for (const doc::NodeId node : selection) {
        const ElementInfo *const element = find(node);
        if (element != nullptr && outliner_groupable(*element) &&
            !element->locked) {
          moving.push_back(node);
        }
      }
    }
    if (moving.empty()) {
      moving.push_back(drag_node_);
    }
  }
  const auto movable_into = [&](const ElementInfo &target) {
    if (target.kind != ElementKind::Group || target.linked || target.locked) {
      return false;
    }
    return std::none_of(moving.begin(), moving.end(), [&](doc::NodeId node) {
      const ElementInfo *const element = find(node);
      return element == nullptr || !outliner_groupable(*element) ||
             descends_from(target.node, node);
    });
  };
  enum class DropKind : core::u8 { None, Before, After, Into };
  DropKind drop{DropKind::None};
  const ElementInfo *drop_target{};
  float drop_line_y{};
  float drop_line_x{};

  // Rows down the list, scrolled; the settings rows are set off from the
  // scene's own by a rule.
  gui::Metrics rows_metrics = theme.metrics;
  rows_metrics.inset = 0.0F;
  rows_metrics.gap = 0.0F;
  gui::FormLayout list{list_bounds, rows_metrics,
                       scroll_.begin(ctx, list_bounds, row_height * 3.0F)};
  bool pointer_over_content = false;

  if (show_settings) {
    const std::array<std::pair<SceneSettingsTarget, std::string_view>, 2U>
        settings{{{SceneSettingsTarget::Renderer, "Renderer"},
                  {SceneSettingsTarget::Environment, "Environment"}}};
    for (const auto &[target, label] : settings) {
      const gui::Rect row = list.next(row_height, 0.0F);
      const gui::Interaction interaction =
          clipped_grab(ctx, gui::ui_id(id_, label), row, list_bounds);
      pointer_over_content =
          pointer_over_content || row.contains(pointer_x, pointer_y);
      const bool chosen = settings_selection == target;
      if (chosen) {
        ctx.panel(row, theme.selection);
        ctx.panel(gui::Rect{row.x, row.y, 2.0F, row.height}, theme.accent);
      } else if (interaction.hovered) {
        ctx.panel(row, theme.widget_hover);
      }
      const float icon_x = row.x + list_metrics::toggle + 4.0F +
                           list_metrics::twirl_width + 8.0F;
      if (target == SceneSettingsTarget::Environment) {
        draw_kind_icon(ctx, ElementKind::Environment,
                       scene::LocalLightKind::Point, icon_x,
                       row.y + row.height * 0.5F,
                       element_kind_color(ElementKind::Environment));
      } else {
        // The renderer: a gear-like cog, drawn as a ring and teeth.
        const float cx = icon_x;
        const float cy = row.y + row.height * 0.5F;
        draw_filled_circle(ctx, cx, cy, 5.0F, theme.text_muted);
        draw_filled_circle(ctx, cx, cy, 2.2F,
                           chosen ? theme.selection : theme.panel);
        for (std::size_t tooth = 0U; tooth < 6U; ++tooth) {
          const float angle = 6.28318530718F * static_cast<float>(tooth) / 6.0F;
          ctx.draw_list().add_line(cx + std::cos(angle) * 4.0F,
                                   cy + std::sin(angle) * 4.0F,
                                   cx + std::cos(angle) * 7.0F,
                                   cy + std::sin(angle) * 7.0F, 2.0F,
                                   theme.text_muted);
        }
      }
      ctx.label(gui::Rect{icon_x + 14.0F, row.y,
                          std::max(row.width - icon_x - 14.0F + row.x, 0.0F),
                          row.height},
                label, chosen ? theme.text : scale(theme.text, 0.9F));
      if (interaction.clicked) {
        SceneSelectionRequest next{};
        next.settings = target;
        request = next;
      }
    }
    // A rule between the scene's settings and the things in it.
    list.space(2.0F);
    ctx.panel(gui::inset(list.next(1.0F, 3.0F), 8.0F, 0.0F), theme.divider);
  }

  const float tree_x = list_bounds.x + list_metrics::toggle + 4.0F;
  const gui::UiColor guide_color = with_alpha(theme.border, 0.9F);
  std::optional<std::size_t> menu_row;
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const OutlinerRow &entry = rows[index];
    const ElementInfo &element = *entry.element;
    const gui::Rect row = list.next(row_height, 0.0F);
    if (row.bottom() <= list_bounds.y || row.y >= list_bounds.bottom()) {
      continue;
    }
    const bool row_hovered = !ctx.pointer_blocked() &&
                             row.contains(pointer_x, pointer_y) &&
                             list_bounds.contains(pointer_x, pointer_y);
    pointer_over_content = pointer_over_content || row_hovered;
    const float centre_y = row.y + row.height * 0.5F;
    const float depth_x =
        tree_x + static_cast<float>(entry.depth) * list_metrics::indent;
    // The eye at the far left, the padlock at the far right.
    gui::Rect toggles = gui::inset(row, 2.0F, 0.0F);
    const gui::Rect eye_bounds = gui::cut_left(toggles, list_metrics::toggle);
    const gui::Rect lock_bounds = gui::cut_right(toggles, list_metrics::toggle);
    const gui::Rect twirl_bounds{depth_x, row.y, list_metrics::twirl_width,
                                 row.height};
    const float icon_x = depth_x + list_metrics::twirl_width + 8.0F;
    const float name_x = icon_x + 13.0F;
    std::string tag;
    if (element.linked_collection) {
      tag = element.linked_paused     ? "paused"
            : element.linked_pending  ? "updating"
            : element.linked_failed   ? "refused"
            : !element.linked_connected ? "offline"
                                        : "";
    } else if (element.linked) {
      tag = "Blender";
    }
    const float tag_width = tag.empty() ? 0.0F : ctx.font_width(tag) + 10.0F;
    const gui::Rect name_bounds{
        name_x, row.y,
        std::max(lock_bounds.x - name_x - tag_width - 4.0F, 0.0F), row.height};

    const bool is_selected =
        std::find(selection.begin(), selection.end(), element.node) !=
        selection.end();
    const bool is_drop_target_row =
        dragging_ && row_hovered && element.node != drag_node_;

    // Background: selection, hover, the operator's highlight, nothing - no
    // zebra; the hierarchy is the structure the eye follows, and alternating
    // fills fight the guides. The highlight keeps a bar at the right edge so
    // it still reads on a selected row.
    // Each is a soft capsule inset from the panel's edges, the primary
    // selection ringed in the accent.
    const gui::Rect capsule = gui::inset(row, 3.0F, 1.0F);
    if (element.highlight.has_value()) {
      const gui::UiColor highlight = timeline_marker_color(*element.highlight);
      if (!is_selected) {
        ctx.rounded(capsule, with_alpha(highlight, 0.2F), theme.radius);
      }
      ctx.rounded(gui::Rect{capsule.right() - 5.0F, capsule.y + 5.0F, 3.0F,
                            std::max(capsule.height - 10.0F, 0.0F)},
                  highlight, 1.5F);
    }
    if (is_selected) {
      ctx.rounded(capsule,
                  element.node == primary ? scale(theme.selection, 1.2F)
                                          : theme.selection,
                  theme.radius);
      if (element.node == primary) {
        ctx.rounded_outline(capsule, with_alpha(theme.accent, 0.45F),
                            theme.radius);
      }
    } else if (row_hovered && !dragging_) {
      ctx.rounded(capsule, with_alpha(theme.widget_hover, 0.7F), theme.radius);
    }
    draw_indent_guides(ctx, tree_x, row.y, row.height, entry.depth,
                       guide_color);

    // Visibility, far left like every compositor's eye column.
    if (row_flag_toggle(ctx, gui::ui_id(id_, "scene.element.visible",
                                 element.node.value()),
                        eye_bounds, RowFlag::Visible, element.visible,
                        row_hovered)) {
      SceneSelectionRequest next{};
      next.node = element.node;
      next.visibility_changed = true;
      next.visible = !element.visible;
      request = std::move(next);
    }
    if (entry.has_children) {
      const gui::Interaction twirl = clipped_grab(
          ctx, gui::ui_id(id_, "scene.twirl", element.node.value()), twirl_bounds,
          list_bounds);
      const bool open = !search_.empty() || !collapsed_.contains(element.node);
      draw_twirl(ctx, twirl_bounds.x + twirl_bounds.width * 0.5F, centre_y,
                 open, twirl.hovered ? theme.text : theme.text_muted);
      if (twirl.clicked && search_.empty()) {
        if (open) {
          collapsed_.insert(element.node);
        } else {
          collapsed_.erase(element.node);
        }
      }
    }
    const gui::UiColor kind_tint = element_kind_color(element);
    draw_element_icon(ctx, element, icon_x, centre_y,
                      element.effective_visible ? kind_tint
                                                : with_alpha(kind_tint, 0.4F));

    // The row's body: everything between the twirl and the lock.
    const gui::Rect body{icon_x - 8.0F, row.y,
                         std::max(lock_bounds.x - icon_x + 8.0F, 0.0F),
                         row.height};
    const bool renaming = rename_.editing(element.node.value());
    gui::Interaction interaction{};
    if (renaming) {
      if (auto renamed = rename_.draw(
              ctx, gui::Rect{name_bounds.x - 4.0F, row.y + 2.0F,
                             std::max(lock_bounds.x - name_bounds.x, 24.0F),
                             row.height - 4.0F})) {
        SceneSelectionRequest next{};
        next.node = element.node;
        next.rename = std::move(*renamed);
        request = std::move(next);
      }
    } else {
      if (!element.locked) {
        interaction = clipped_grab(
            ctx, gui::ui_id(id_, "scene.element", element.node.value()), body,
            list_bounds);
      }
      const gui::UiColor name_color =
          element.locked ? theme.text_muted
                         : (!element.effective_visible
                                ? with_alpha(theme.text, 0.45F)
                                : (is_selected ? theme.text
                                               : scale(theme.text, 0.92F)));
      ctx.label(name_bounds, element_display_name(element), name_color);
      if (!tag.empty()) {
        ctx.label(gui::Rect{name_bounds.right() + 4.0F, row.y,
                            tag_width, row.height},
                  tag,
                  element.linked_failed ? theme.warning
                                        : with_alpha(theme.text_muted, 0.85F));
      }
    }

    if (row_flag_toggle(
            ctx, gui::ui_id(id_, "scene.element.lock", element.node.value()),
            lock_bounds, RowFlag::Locked, element.locked, row_hovered)) {
      SceneSelectionRequest next{};
      next.node = element.node;
      next.lock_changed = true;
      next.locked = !element.locked;
      request = std::move(next);
    }

    if (interaction.pressed && !element.linked && outliner_orderable(element)) {
      drag_node_ = element.node;
      drag_x_ = pointer_x;
      drag_y_ = pointer_y;
      dragging_ = false;
    }
    if (interaction.clicked && !dragging_) {
      SceneSelectionRequest next{};
      next.node = element.node;
      next.action = selection_action(ctx.ctrl_held(), ctx.shift_held(), true);
      if (next.action == SelectionAction::Range) {
        next.visible_order = visible_order;
      }
      const bool plain_click = next.action == SelectionAction::Replace;
      request = std::move(next);
      // A second click on the same name opens it for renaming.
      if (plain_click && name_bounds.contains(pointer_x, pointer_y) &&
          clicks_.double_click(element.node.value(), ctx.time_seconds()) &&
          !element.linked) {
        rename_.begin(ctx, element.node.value(),
                      element_display_name(element));
      }
    }
    if (row_hovered && ctx.secondary_pointer_pressed() && !element.locked) {
      menu_row = index;
    }

    // Where a drag would land if let go here.
    if (is_drop_target_row) {
      const float local = (pointer_y - row.y) / row.height;
      const bool into_band = local > 0.25F && local < 0.75F;
      if (into_band && movable_into(element)) {
        drop = DropKind::Into;
        drop_target = &element;
      } else if (outliner_orderable(element) && !element.linked &&
                 std::none_of(moving.begin(), moving.end(),
                              [&](doc::NodeId node) {
                                return descends_from(element.node, node);
                              })) {
        drop = local < 0.5F ? DropKind::Before : DropKind::After;
        drop_target = &element;
        drop_line_y = local < 0.5F ? row.y : row.bottom();
        drop_line_x = depth_x;
      }
      if (drop == DropKind::Into) {
        const gui::Rect target_row = row.inflated(-1.0F, -1.0F);
        ctx.panel(target_row, with_alpha(theme.accent, 0.18F));
        auto &draw = ctx.draw_list();
        draw.add_rect(target_row.x, target_row.y, target_row.width, 1.0F,
                      theme.accent);
        draw.add_rect(target_row.x, target_row.bottom() - 1.0F,
                      target_row.width, 1.0F, theme.accent);
      }
    }
  }
  // The LED surfaces, after everything the scene holds. A click selects one
  // on Stage, as picking it there does; the padlock is its Feed Mapping lock.
  for (const output::ProjectionSurface *const surface : surface_rows) {
    const gui::Rect row = list.next(row_height, 0.0F);
    if (row.bottom() <= list_bounds.y || row.y >= list_bounds.bottom()) {
      continue;
    }
    const bool row_hovered = !ctx.pointer_blocked() && !dragging_ &&
                             row.contains(pointer_x, pointer_y) &&
                             list_bounds.contains(pointer_x, pointer_y);
    pointer_over_content = pointer_over_content || row_hovered;
    const bool is_selected = selected_surface == surface->node;
    if (is_selected) {
      ctx.panel(row, scale(theme.selection, 1.2F));
      ctx.panel(gui::Rect{row.x, row.y, 2.0F, row.height}, theme.accent);
    } else if (row_hovered) {
      ctx.panel(row, with_alpha(theme.widget_hover, 0.7F));
    }
    gui::Rect toggles = gui::inset(row, 2.0F, 0.0F);
    const gui::Rect lock_bounds = gui::cut_right(toggles, list_metrics::toggle);
    const float icon_x = tree_x + list_metrics::twirl_width + 8.0F;
    const float name_x = icon_x + 13.0F;
    draw_kind_icon(ctx, ElementKind::Track, scene::LocalLightKind::Point,
                   icon_x, row.y + row.height * 0.5F, theme.accent);
    const std::string_view tag{"LED"};
    const float tag_width = ctx.font_width(tag) + 10.0F;
    const gui::Rect name_bounds{
        name_x, row.y,
        std::max(lock_bounds.x - name_x - tag_width - 4.0F, 0.0F), row.height};
    ctx.label(name_bounds,
              surface->name.empty() ? std::string_view{"Surface"}
                                    : std::string_view{surface->name},
              surface->locked ? theme.text_muted
                              : (is_selected ? theme.text
                                             : scale(theme.text, 0.92F)));
    ctx.label(gui::Rect{name_bounds.right() + 4.0F, row.y,
                        tag_width, row.height},
              tag, with_alpha(theme.text_muted, 0.85F));
    if (!surface->locked) {
      const gui::Interaction interaction = clipped_grab(
          ctx, gui::ui_id(id_, "scene.surface", surface->node.value()),
          gui::Rect{icon_x - 8.0F, row.y,
                    std::max(lock_bounds.x - icon_x + 8.0F, 0.0F), row.height},
          list_bounds);
      if (interaction.clicked) {
        SceneSelectionRequest next{};
        next.node = surface->node;
        next.surface = true;
        request = std::move(next);
      }
    }
    if (row_flag_toggle(
            ctx, gui::ui_id(id_, "scene.surface.lock", surface->node.value()),
            lock_bounds, RowFlag::Locked, surface->locked, row_hovered)) {
      SceneSelectionRequest next{};
      next.node = surface->node;
      next.surface = true;
      next.lock_changed = true;
      next.locked = !surface->locked;
      request = std::move(next);
    }
  }
  if (drop == DropKind::Before || drop == DropKind::After) {
    ctx.panel(gui::Rect{drop_line_x, drop_line_y - 1.0F,
                        std::max(list_bounds.right() -
                                     drop_line_x - 4.0F,
                                 0.0F),
                        2.0F},
              theme.accent);
    draw_filled_circle(ctx, drop_line_x, drop_line_y, 3.0F, theme.accent);
  }
  const bool background_menu = !ctx.pointer_blocked() && !dragging_ &&
                               ctx.secondary_pointer_pressed() &&
                               list_bounds.contains(pointer_x, pointer_y) &&
                               !pointer_over_content;
  if (!request.has_value() && ctx.pointer_pressed() &&
      list_bounds.contains(pointer_x, pointer_y) && !pointer_over_content) {
    SceneSelectionRequest clear{};
    clear.action = SelectionAction::Clear;
    request = std::move(clear);
  }
  if (rows.empty() && surface_rows.empty()) {
    list.space(4.0F);
    ctx.label(gui::inset(list.next(row_height, 0.0F), 12.0F, 0.0F),
              search_.empty() ? "Nothing in this scene yet - add from Library"
                              : "No matches",
              theme.text_muted);
  }
  scroll_.end(ctx, content_height);

  // The drop, on release: into a group, or beside a sibling - which is a
  // reorder when the two already share a parent, and a move otherwise.
  if (dragging_ && !ctx.pointer_down()) {
    if (drop_target != nullptr && drop != DropKind::None) {
      SceneSelectionRequest next{};
      if (drop == DropKind::Into) {
        next.structure = SceneSelectionRequest::StructureAction::MoveInto;
        next.destination = drop_target->node;
        next.moved = moving;
      } else {
        const ElementInfo *const dragged = find(drag_node_);
        const bool siblings =
            dragged != nullptr && moving.size() == 1U &&
            outliner_parent(*dragged) == outliner_parent(*drop_target);
        if (siblings) {
          next.node = drag_node_;
          next.reorder_destination = drop_target->node;
          next.reorder_before = drop == DropKind::Before;
        } else {
          next.structure = SceneSelectionRequest::StructureAction::MoveInto;
          next.destination = outliner_parent(*drop_target);
          next.moved = moving;
          next.reorder_destination = drop_target->node;
          next.reorder_before = drop == DropKind::Before;
        }
      }
      request = std::move(next);
    }
    drag_node_ = doc::NodeId{};
    dragging_ = false;
  } else if (!ctx.pointer_down()) {
    drag_node_ = doc::NodeId{};
    dragging_ = false;
  }
  if (dragging_) {
    // The rows riding under the pointer, so the eye can follow the move.
    const ElementInfo *const dragged = find(drag_node_);
    std::string label = dragged != nullptr ? element_display_name(*dragged)
                                           : std::string{};
    if (moving.size() > 1U) {
      label += " +" + std::to_string(moving.size() - 1U);
    }
    const float width = ctx.font_width(label) + 16.0F;
    const gui::Rect ghost{pointer_x + 12.0F, pointer_y - 10.0F, width, 20.0F};
    ctx.panel(ghost, with_alpha(theme.accent, 0.85F));
    ctx.label(gui::Rect{ghost.x + 8.0F, ghost.y, width - 8.0F, ghost.height},
              label, theme.text);
    ctx.set_cursor(gui::Cursor::Move);
  }

  // F2 renames the selected row while the outliner has the keyboard's
  // attention - the last panel clicked.
  if (focused_ && !ctx.typing() && ctx.key_pressed(gui::Key::Rename)) {
    if (const ElementInfo *const target = find(primary);
        target != nullptr && outliner_lists(*target)) {
      begin_rename(ctx, *target);
    }
  }

  // ------------------------------------------------------------- footer
  ctx.panel(footer, theme.header);
  {
    gui::Rect rule = footer;
    ctx.panel(gui::cut_top(rule, 1.0F), theme.divider);
  }
  const auto primary_element = find(primary);
  const bool can_group =
      !selection.empty() &&
      std::all_of(selection.begin(), selection.end(), [&](doc::NodeId node) {
        const ElementInfo *const found = find(node);
        return found != nullptr && !found->locked &&
               outliner_groupable(*found);
      });
  std::string selection_text =
      selection.empty() ? std::string{"No selection"}
                        : (selection.size() == 1U && primary_element != nullptr
                               ? element_display_name(*primary_element)
                               : std::to_string(selection.size()) +
                                     " selected");
  ctx.label(gui::inset(footer, 10.0F, 0.0F), selection_text,
            selection.empty() ? theme.text_muted : theme.text);

  // ------------------------------------------------------ context menu
  const gui::UiId menu_id = gui::ui_id(id_, "scene.menu");
  const gui::UiId color_menu_id = gui::ui_id(id_, "scene.color_menu");
  const auto add = [&](std::string label, SceneMenuCommand command) {
    menu_entries_.push_back(std::move(label));
    menu_commands_.push_back(static_cast<core::u8>(command));
  };
  if (menu_row.has_value()) {
    const ElementInfo &element = *rows[*menu_row].element;
    menu_node_ = element.node;
    menu_background_ = false;
    color_menu_ = false;
    menu_entries_.clear();
    menu_commands_.clear();
    const bool in_selection =
        std::find(selection.begin(), selection.end(), element.node) !=
        selection.end();
    if (!in_selection) {
      // A right press acts on what it is over, so the row becomes the
      // selection first - the menu then means exactly that row.
      SceneSelectionRequest next{};
      next.node = element.node;
      next.action = SelectionAction::Replace;
      request = std::move(next);
    }
    if (!element.linked) {
      add("Rename      F2", SceneMenuCommand::Rename);
    }
    add(element.visible ? "Hide" : "Show", SceneMenuCommand::ToggleVisible);
    add("Lock", SceneMenuCommand::ToggleLocked);
    if (!element.linked) {
      add("Highlight Color...", SceneMenuCommand::Highlight);
    }
    const bool group_ready =
        outliner_groupable(element) &&
        (!in_selection || can_group);
    if (group_ready) {
      add(in_selection && selection.size() > 1U ? "Group Selection"
                                                : "Group",
          SceneMenuCommand::Group);
    }
    const bool own_group =
        element.kind == ElementKind::Group && !element.linked;
    if (own_group && element.visible) {
      add("Ungroup", SceneMenuCommand::Ungroup);
    }
    if (!element.linked && element.parent_group.has_value()) {
      add("Move Out of Group", SceneMenuCommand::MoveOut);
    }
    if (own_group) {
      add("New Group Inside", SceneMenuCommand::NewGroup);
      add("New Vehicle Process Group Inside",
          SceneMenuCommand::NewVehicleProcessGroup);
    }
    if (!element.linked_collection) {
      add("Delete", SceneMenuCommand::Delete);
    }
    ctx.open_context_menu(menu_id, pointer_x, pointer_y);
  } else if (background_menu) {
    // The list's empty space: new groups at the top level.
    menu_node_ = doc::NodeId{};
    menu_background_ = true;
    color_menu_ = false;
    menu_entries_.clear();
    menu_commands_.clear();
    add("New Group", SceneMenuCommand::NewGroup);
    add("New Vehicle Process Group",
        SceneMenuCommand::NewVehicleProcessGroup);
    ctx.open_context_menu(menu_id, pointer_x, pointer_y);
  }
  if (menu_node_ != doc::NodeId{} || menu_background_) {
    std::vector<std::string_view> labels(menu_entries_.begin(),
                                         menu_entries_.end());
    if (const auto chosen = ctx.context_menu(menu_id, labels)) {
      const ElementInfo *const element = find(menu_node_);
      if ((element != nullptr || menu_background_) &&
          *chosen < menu_commands_.size()) {
        const auto command =
            static_cast<SceneMenuCommand>(menu_commands_[*chosen]);
        SceneSelectionRequest next{};
        next.node = menu_node_;
        switch (command) {
        case SceneMenuCommand::Rename:
          begin_rename(ctx, *element);
          break;
        case SceneMenuCommand::ToggleVisible:
          next.visibility_changed = true;
          next.visible = !element->visible;
          request = std::move(next);
          break;
        case SceneMenuCommand::ToggleLocked:
          next.lock_changed = true;
          next.locked = !element->locked;
          request = std::move(next);
          break;
        case SceneMenuCommand::Highlight:
          // The colours open where the menu was, so the choice stays under
          // the pointer.
          color_menu_ = true;
          ctx.open_context_menu(color_menu_id, pointer_x, pointer_y);
          break;
        case SceneMenuCommand::Group:
          next.structure = SceneSelectionRequest::StructureAction::Group;
          request = std::move(next);
          break;
        case SceneMenuCommand::Ungroup:
          next.structure = SceneSelectionRequest::StructureAction::Ungroup;
          request = std::move(next);
          break;
        case SceneMenuCommand::MoveOut: {
          next.structure = SceneSelectionRequest::StructureAction::MoveInto;
          const ElementInfo *const parent = find(*element->parent_group);
          next.destination = parent != nullptr
                                 ? outliner_parent(*parent)
                                 : doc::NodeId{};
          next.moved = {element->node};
          if (parent != nullptr) {
            next.reorder_destination = parent->node;
            next.reorder_before = false;
          }
          request = std::move(next);
          break;
        }
        case SceneMenuCommand::NewGroup:
        case SceneMenuCommand::NewVehicleProcessGroup:
          next.structure = SceneSelectionRequest::StructureAction::Create;
          next.destination = menu_node_;
          next.group_kind = command == SceneMenuCommand::NewVehicleProcessGroup
                                ? scene::GroupKind::VehicleProcess
                                : scene::GroupKind::Folder;
          request = std::move(next);
          break;
        case SceneMenuCommand::Delete:
          next.delete_selection = true;
          request = std::move(next);
          break;
        }
      }
      menu_node_ = doc::NodeId{};
      menu_background_ = false;
    } else if (!ctx.menu_open(menu_id)) {
      menu_node_ = doc::NodeId{};
      menu_background_ = false;
    }
  }

  // The highlight colours: None, then the marker palette, each with its chip.
  // The choice applies to the whole selection, as Delete does.
  if (color_menu_) {
    std::array<std::string_view, scene::timeline_marker_colors + 1U> labels{};
    std::array<gui::UiColor, scene::timeline_marker_colors + 1U> swatches{};
    labels[0] = "None";
    swatches[0] = gui::UiColor{0.0F, 0.0F, 0.0F, 0.0F};
    for (core::u8 color = 0U; color < scene::timeline_marker_colors; ++color) {
      labels[color + 1U] = timeline_marker_color_name(color);
      swatches[color + 1U] = timeline_marker_color(color);
    }
    if (const auto chosen =
            ctx.context_menu(color_menu_id, labels, swatches)) {
      SceneSelectionRequest next{};
      next.highlight_changed = true;
      if (*chosen > 0U) {
        next.highlight = static_cast<core::u8>(*chosen - 1U);
      }
      request = std::move(next);
      color_menu_ = false;
    } else if (!ctx.menu_open(color_menu_id)) {
      color_menu_ = false;
    }
  }

  rename_.end_frame(ctx);
  ctx.pop_clip();
  return request;
}

} // namespace mediaserver::editor
