// The node search at the pointer and the right-click menu: what each
// offers, then drawing and acting on it.

#include "compositor_frame.hpp"

#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/shapes.hpp>

namespace mediaserver::editor {

using namespace panel_helpers;
using namespace compositor_ui;

// Moves a wire to `target`'s place in their input's order; the wires
// between shift up or down one. The whole input is numbered afresh.
void CompositorPanel::Frame::move_in_order(doc::NodeId moved,
                                           doc::NodeId target) {
  const compositor::GraphEdge *const edge = compositor_graph.edge(moved);
  if (edge == nullptr || moved == target) {
    return;
  }
  std::vector<const compositor::GraphEdge *> chain =
      panel.input_chain(compositor_graph, edge->to);
  const auto from = std::find(chain.begin(), chain.end(), edge);
  const auto to = std::find_if(
      chain.begin(), chain.end(),
      [target](const compositor::GraphEdge *other) noexcept {
        return other->node == target;
      });
  if (from == chain.end() || to == chain.end()) {
    return;
  }
  const auto place = std::distance(chain.begin(), to);
  chain.erase(from);
  chain.insert(chain.begin() + place, edge);
  CompositorReorderIntent reorder;
  for (std::size_t index = 0U; index < chain.size(); ++index) {
    const auto order = static_cast<core::i64>(index);
    if (panel.edge_order(*chain[index]) == order) {
      continue;
    }
    reorder.orders.emplace_back(chain[index]->node, order);
    std::erase_if(panel.pending_order_, [&](const auto &held) noexcept {
      return held.first == chain[index]->node;
    });
    panel.pending_order_.emplace_back(chain[index]->node, order);
  }
  if (!reorder.orders.empty()) {
    frame.reorder = std::move(reorder);
  }
}

// The wire before or after one in its input's order, if any.
std::optional<doc::NodeId>
CompositorPanel::Frame::order_neighbour(doc::NodeId id, int step) {
  const compositor::GraphEdge *const edge = compositor_graph.edge(id);
  if (edge == nullptr) {
    return std::nullopt;
  }
  const std::vector<const compositor::GraphEdge *> chain =
      panel.input_chain(compositor_graph, edge->to);
  const auto at = std::find(chain.begin(), chain.end(), edge);
  const auto index = std::distance(chain.begin(), at) + step;
  if (at == chain.end() || index < 0 ||
      index >= static_cast<std::ptrdiff_t>(chain.size())) {
    return std::nullopt;
  }
  return chain[static_cast<std::size_t>(index)]->node;
}

// The selected nodes that can be bypassed, and whether all of them are.
std::pair<std::vector<doc::NodeId>, bool>
CompositorPanel::Frame::bypass_targets() {
  std::vector<doc::NodeId> targets;
  bool all_bypassed = true;
  for (const doc::NodeId id : panel.selected_nodes_) {
    const compositor::GraphNode *const node = compositor_graph.node(id);
    if (node != nullptr && compositor::node_kind_bypassable(node->kind)) {
      targets.push_back(id);
      all_bypassed = all_bypassed && panel.bypassed(*node);
    }
  }
  return std::pair{std::move(targets), all_bypassed};
}

// Bypasses the selection's bypassable nodes, or restores them when every
// one already is.
void CompositorPanel::Frame::toggle_bypass() {
  auto [targets, all_bypassed] = bypass_targets();
  if (targets.empty()) {
    return;
  }
  const bool bypass = !all_bypassed;
  for (const doc::NodeId target : targets) {
    std::erase_if(panel.pending_bypass_, [target](const auto &held) noexcept {
      return held.first == target;
    });
    panel.pending_bypass_.emplace_back(target, bypass);
  }
  frame.bypass = CompositorBypassIntent{std::move(targets), bypass};
}

void CompositorPanel::build_menus(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const gui::Rect &bounds = f.bounds;
  const compositor::CompositorGraph &compositor_graph = f.compositor_graph;
  auto &output_count = f.output_count;
  auto &menu_items = f.menu_items;
  auto &menu_bounds = f.menu_bounds;
  auto &context_items = f.context_items;
  auto &context_bounds = f.context_bounds;
  if (node_menu_.has_value()) {
    for (std::size_t pass = 0U; pass < 2U; ++pass) {
      for (const CompositorPaletteItem &item : compositor_palette_items) {
        const bool direct =
            matches(compositor::node_kind_name(item.kind), node_menu_->search) ||
            matches(item.search_terms, node_menu_->search);
        if (!f.palette_matches_text(item, node_menu_->search) ||
            direct != (pass == 0U) ||
            (item.kind == compositor::NodeKind::Output && output_count != 0U)) {
          continue;
        }
        std::optional<core::u8> input;
        if (node_menu_->from.has_value()) {
          const auto ports = compositor::input_ports(item.kind);
          const auto port = std::find_if(
              ports.begin(), ports.end(),
              [&](const compositor::PortDescriptor &candidate) noexcept {
                return candidate.type == node_menu_->type;
              });
          if (port == ports.end()) {
            continue;
          }
          input = port->index;
        }
        menu_items.push_back(MenuItem{std::addressof(item), input});
      }
    }
    if (!menu_items.empty()) {
      node_menu_->highlight =
          std::min(node_menu_->highlight, menu_items.size() - 1U);
    }
    const float width = std::min(300.0F, std::max(bounds.width - 16.0F, 0.0F));
    const float height =
        34.0F + menu_row_height *
                    static_cast<float>(std::max<std::size_t>(
                        std::min(menu_items.size(), menu_visible_rows), 1U));
    // Kept inside the panel: shifted left or up from the pointer if needed.
    menu_bounds = gui::Rect{
        std::clamp(node_menu_->screen_x, bounds.x + 4.0F,
                   std::max(bounds.right() - width - 4.0F,
                            bounds.x + 4.0F)),
        std::clamp(node_menu_->screen_y, bounds.y + compositor_toolbar_height,
                   std::max(bounds.bottom() - height - 4.0F,
                            bounds.y + compositor_toolbar_height)),
        width, height};
    if (ctx.pointer_pressed() &&
        !menu_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
      node_menu_.reset();
      ctx.clear_focus();
    }
  }

  if (context_menu_.has_value()) {
    const auto item = [&](std::string label, std::string_view shortcut,
                          ContextAction action, bool enabled = true,
                          bool divided = false) {
      context_items.push_back(
          ContextItem{std::move(label), shortcut, action, enabled, 0U, divided});
    };
    const std::size_t count = selected_nodes_.size();
    switch (context_menu_->kind) {
    case CompositorSelectionKind::Node:
      item("RENAME", "F2", ContextAction::Rename);
      item("DUPLICATE", "CTRL+D", ContextAction::Duplicate);
      item("COPY", "CTRL+C", ContextAction::Copy);
      item(count > 1U ? "DELETE " + std::to_string(count) + " NODES" : "DELETE",
           "DEL", ContextAction::Delete,
           selection_delete(compositor_graph).has_value());
      item("ALIGN LEFT", {}, ContextAction::AlignLeft, count >= 2U, true);
      item("ALIGN RIGHT", {}, ContextAction::AlignRight, count >= 2U);
      item("ALIGN TOP", {}, ContextAction::AlignTop, count >= 2U);
      item("ALIGN BOTTOM", {}, ContextAction::AlignBottom, count >= 2U);
      item("DISTRIBUTE ACROSS", {}, ContextAction::DistributeHorizontal,
           count >= 3U);
      item("DISTRIBUTE DOWN", {}, ContextAction::DistributeVertical,
           count >= 3U);
      item("STRAIGHTEN CONNECTION", "Q", ContextAction::Straighten, true, true);
      item(count >= 2U ? "ARRANGE SELECTION" : "ARRANGE GRAPH", {},
           ContextAction::Arrange);
      item("COMMENT SELECTION", "C", ContextAction::CommentSelection);
      {
        // Collapses them all unless every one already is.
        const bool all_folded = std::all_of(
            selected_nodes_.begin(), selected_nodes_.end(),
            [&](doc::NodeId id) {
              const compositor::GraphNode *const node =
                  compositor_graph.node(id);
              return node != nullptr && collapsed(*node);
            });
        item(std::string{all_folded ? "EXPAND" : "COLLAPSE"} +
                 (count > 1U ? " NODES" : ""),
             {}, ContextAction::Collapse, count >= 1U, true);
        const auto [targets, all_bypassed] = f.bypass_targets();
        item(!targets.empty() && all_bypassed ? "RESTORE" : "BYPASS", "D",
             ContextAction::Bypass, !targets.empty());
      }
      break;
    case CompositorSelectionKind::Edge: {
      const compositor::GraphEdge *const edge =
          compositor_graph.edge(context_menu_->target);
      item("ADD REROUTE POINT", "DOUBLE-CLICK", ContextAction::AddPoint,
           edge != nullptr && edge->points.size() < compositor::max_wire_points);
      item("DISCONNECT", "ALT+CLICK", ContextAction::Disconnect);
      // Where the wire shares its input, its place in the input's order.
      const auto earlier = f.order_neighbour(context_menu_->target, -1);
      const auto later = f.order_neighbour(context_menu_->target, 1);
      if (earlier.has_value() || later.has_value()) {
        item("MOVE EARLIER", {}, ContextAction::MoveEarlier,
             earlier.has_value(), true);
        item("MOVE LATER", {}, ContextAction::MoveLater, later.has_value());
      }
      break;
    }
    case CompositorSelectionKind::Comment:
      item("RENAME", "F2", ContextAction::Rename);
      for (core::u8 index = 0U; index < compositor::comment_color_count;
           ++index) {
        item(std::string{"COLOUR  "} + std::string{comment_color_name(index)},
             {}, ContextAction::Colour, true, index == 0U);
        context_items.back().color = index;
      }
      item("DELETE", "DEL", ContextAction::Delete, true, true);
      break;
    case CompositorSelectionKind::None:
      break;
    }
    float height = 8.0F;
    for (const ContextItem &entry : context_items) {
      height += context_row_height + (entry.divided ? context_divider : 0.0F);
    }
    const float width = 250.0F;
    context_bounds = gui::Rect{
        std::clamp(context_menu_->screen_x, bounds.x + 4.0F,
                   std::max(bounds.right() - width - 4.0F,
                            bounds.x + 4.0F)),
        std::clamp(context_menu_->screen_y, bounds.y + compositor_toolbar_height,
                   std::max(bounds.bottom() - height - 4.0F,
                            bounds.y + compositor_toolbar_height)),
        width, height};
    if ((ctx.pointer_pressed() || ctx.secondary_pointer_pressed()) &&
        !context_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
      context_menu_.reset();
      context_items.clear();
    }
  }
}

void CompositorPanel::draw_node_search(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  CompositorFrame &frame = f.frame;
  auto &menu_items = f.menu_items;
  auto &menu_bounds = f.menu_bounds;
  if (node_menu_.has_value() && menu_bounds.width > 0.0F) {
    const gui::UiId search_id = gui::ui_id(id_, "compositor.node_menu.search");
    if (node_menu_->focus_pending) {
      ctx.focus_text_field(search_id, node_menu_->search);
      node_menu_->focus_pending = false;
    }
    // Arrows walk the list and Enter takes the highlighted node, even while
    // the search field has the keyboard; Escape closes.
    std::optional<std::size_t> chosen;
    if (ctx.key_pressed(gui::Key::Escape)) {
      node_menu_.reset();
      ctx.clear_focus();
    } else {
      if (ctx.key_pressed(gui::Key::Down) && !menu_items.empty()) {
        node_menu_->highlight =
            std::min(node_menu_->highlight + 1U, menu_items.size() - 1U);
      }
      if (ctx.key_pressed(gui::Key::Up) && node_menu_->highlight > 0U) {
        --node_menu_->highlight;
      }
      if (ctx.key_pressed(gui::Key::Enter) && !menu_items.empty()) {
        chosen = node_menu_->highlight;
      }
    }
    if (node_menu_.has_value()) {
      ctx.push_clip(menu_bounds);
      ctx.panel(menu_bounds, scale(ctx.theme().panel, 1.04F));
      ctx.outline(menu_bounds, ctx.theme().widget_hover);
      const std::string placeholder =
          node_menu_->from.has_value()
              ? std::string{"Add a node taking "} +
                    std::string{port_type_name(node_menu_->type)}
              : std::string{"Search nodes and effects"};
      const std::string before = node_menu_->search;
      search_field(ctx, search_id,
                   gui::Rect{menu_bounds.x + 4.0F, menu_bounds.y + 4.0F,
                             std::max(menu_bounds.width - 8.0F, 0.0F), 27.0F},
                   node_menu_->search, placeholder);
      if (node_menu_->search != before) {
        node_menu_->highlight = 0U;
      }
      // The highlighted row stays in view as the arrows move it.
      const std::size_t first =
          node_menu_->highlight >= menu_visible_rows
              ? node_menu_->highlight - menu_visible_rows + 1U
              : 0U;
      float row_y = menu_bounds.y + 34.0F;
      if (menu_items.empty()) {
        ctx.label(gui::Rect{menu_bounds.x + 10.0F, row_y,
                            std::max(menu_bounds.width - 20.0F, 0.0F),
                            menu_row_height},
                  "NO NODES FOUND", ctx.theme().text_muted);
      }
      for (std::size_t index = first;
           index < menu_items.size() && index < first + menu_visible_rows;
           ++index) {
        const MenuItem &entry = menu_items[index];
        const gui::Rect row{menu_bounds.x + 4.0F, row_y,
                            std::max(menu_bounds.width - 8.0F, 0.0F),
                            menu_row_height};
        row_y += menu_row_height;
        const gui::Interaction interaction = ctx.grab(
            gui::ui_id(id_, "compositor.node_menu.item",
                static_cast<core::u64>(entry.item->kind)),
            row);
        if (interaction.hovered) {
          node_menu_->highlight = index;
        }
        const bool highlighted = index == node_menu_->highlight;
        ctx.panel(row, highlighted ? ctx.theme().widget_hover : ctx.theme().row);
        ctx.panel(gui::Rect{row.x, row.y, 3.0F, row.height},
                  compositor_node_color(ctx, entry.item->kind));
        std::string label{compositor::node_kind_name(entry.item->kind)};
        if (entry.input.has_value()) {
          const auto ports = compositor::input_ports(entry.item->kind);
          label += "  >  ";
          label += ports[*entry.input].name;
        }
        ctx.label(gui::Rect{row.x + 10.0F, row.y,
                            std::max(row.width - 14.0F, 0.0F), row.height},
                  label,
                  highlighted ? ctx.theme().text : ctx.theme().text_muted);
        if (interaction.clicked) {
          chosen = index;
        }
      }
      ctx.pop_clip();
    }
    if (chosen.has_value() && node_menu_.has_value()) {
      const MenuItem &entry = menu_items[*chosen];
      const compositor::WirePoint at =
          f.to_graph(node_menu_->screen_x, node_menu_->screen_y);
      CompositorAddIntent add{entry.item->kind, at.x, at.y};
      if (entry.input.has_value() && node_menu_->from.has_value()) {
        // The new node's input lands where the wire was let go.
        const auto ports = compositor::input_ports(entry.item->kind);
        const auto row = static_cast<float>(std::distance(
            ports.begin(),
            std::find_if(ports.begin(), ports.end(),
                         [&](const compositor::PortDescriptor &port) noexcept {
                           return port.index == *entry.input;
                         })));
        add.editor_y = at.y - metrics_.header_height -
                       metrics_.row_height * (row + 0.5F);
        add.links.push_back(
            CompositorAddLink{false, *entry.input, *node_menu_->from});
      }
      frame.add_node = std::move(add);
      node_menu_.reset();
      ctx.clear_focus();
    }
  }
}

void CompositorPanel::draw_context_menu(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const compositor::CompositorGraph &compositor_graph = f.compositor_graph;
  CompositorFrame &frame = f.frame;
  auto &context_items = f.context_items;
  auto &context_bounds = f.context_bounds;
  if (context_menu_.has_value() && context_bounds.width > 0.0F) {
    std::optional<ContextItem> chosen;
    if (ctx.key_pressed(gui::Key::Escape)) {
      context_menu_.reset();
    } else {
      ctx.push_clip(context_bounds);
      ctx.panel(context_bounds, scale(ctx.theme().panel, 1.04F));
      ctx.outline(context_bounds, ctx.theme().widget_hover);
      float row_y = context_bounds.y + 4.0F;
      for (std::size_t index = 0U; index < context_items.size(); ++index) {
        const ContextItem &entry = context_items[index];
        if (entry.divided) {
          ctx.panel(gui::Rect{context_bounds.x + 8.0F, row_y + 3.0F,
                              context_bounds.width - 16.0F, 1.0F},
                    ctx.theme().divider);
          row_y += context_divider;
        }
        const gui::Rect row{context_bounds.x + 4.0F, row_y,
                            context_bounds.width - 8.0F, context_row_height};
        row_y += context_row_height;
        gui::Interaction interaction{};
        if (entry.enabled) {
          interaction =
              ctx.grab(gui::ui_id(id_, "compositor.context_item", index), row);
        }
        if (interaction.hovered) {
          ctx.panel(row, ctx.theme().widget_hover);
        }
        float text_x = row.x + 10.0F;
        if (entry.action == ContextAction::Colour) {
          ctx.panel(gui::Rect{row.x + 10.0F, row.y + 6.0F, 11.0F,
                              row.height - 12.0F},
                    comment_color(entry.color));
          text_x += 18.0F;
        }
        const gui::UiColor text = !entry.enabled ? with_alpha(ctx.theme().text_muted, 0.5F)
                                  : interaction.hovered ? ctx.theme().text
                                                        : ctx.theme().text_muted;
        const float shortcut_width = ctx.font_width(entry.shortcut);
        ctx.label(gui::Rect{text_x, row.y,
                            std::max(row.right() - text_x -
                                         shortcut_width - 14.0F,
                                     0.0F),
                            row.height},
                  entry.label, text);
        if (!entry.shortcut.empty()) {
          ctx.label(gui::Rect{row.right() - shortcut_width - 8.0F, row.y,
                              shortcut_width + 2.0F, row.height},
                    entry.shortcut, with_alpha(ctx.theme().text_muted, 0.8F));
        }
        if (interaction.clicked) {
          chosen = entry;
        }
      }
      ctx.pop_clip();
    }
    if (chosen.has_value() && context_menu_.has_value()) {
      const ContextMenu menu = *context_menu_;
      context_menu_.reset();
      switch (chosen->action) {
      case ContextAction::Rename:
        f.start_rename();
        break;
      case ContextAction::Duplicate:
        f.duplicate_selection();
        break;
      case ContextAction::Copy:
        f.copy_selection();
        break;
      case ContextAction::Delete:
        frame.remove = selection_delete(compositor_graph);
        break;
      case ContextAction::AlignLeft:
        f.apply_moves(align_boxes(f.selected_boxes(), AlignEdge::Left));
        break;
      case ContextAction::AlignRight:
        f.apply_moves(align_boxes(f.selected_boxes(), AlignEdge::Right));
        break;
      case ContextAction::AlignTop:
        f.apply_moves(align_boxes(f.selected_boxes(), AlignEdge::Top));
        break;
      case ContextAction::AlignBottom:
        f.apply_moves(align_boxes(f.selected_boxes(), AlignEdge::Bottom));
        break;
      case ContextAction::DistributeHorizontal:
        f.apply_moves(distribute_boxes(f.selected_boxes(), true));
        break;
      case ContextAction::DistributeVertical:
        f.apply_moves(distribute_boxes(f.selected_boxes(), false));
        break;
      case ContextAction::Straighten:
        f.apply_moves(f.straightened());
        break;
      case ContextAction::Arrange:
        f.apply_moves(f.arranged());
        break;
      case ContextAction::CommentSelection:
        f.comment_selection();
        break;
      case ContextAction::Bypass:
        f.toggle_bypass();
        break;
      case ContextAction::Collapse:
        if (const compositor::GraphNode *const node =
                compositor_graph.node(menu.target)) {
          f.toggle_collapse(node->node, chosen->label.starts_with("COLLAPSE"));
        }
        break;
      case ContextAction::AddPoint:
        if (const compositor::GraphEdge *const edge =
                compositor_graph.edge(menu.target)) {
          std::vector<compositor::WirePoint> points = f.edge_points(*edge);
          points.insert(points.begin() +
                            static_cast<std::ptrdiff_t>(
                                std::min(menu.segment, points.size())),
                        menu.at);
          f.emit_points(edge->node, std::move(points), edge->points);
        }
        break;
      case ContextAction::MoveEarlier:
        if (const auto earlier = f.order_neighbour(menu.target, -1)) {
          f.move_in_order(menu.target, *earlier);
        }
        break;
      case ContextAction::MoveLater:
        if (const auto later = f.order_neighbour(menu.target, 1)) {
          f.move_in_order(menu.target, *later);
        }
        break;
      case ContextAction::Disconnect:
        frame.rewire = CompositorRewireIntent{{menu.target}, {}};
        break;
      case ContextAction::Colour:
        if (const compositor::GraphComment *const comment =
                f.comment_of(menu.target)) {
          compositor::GraphComment recoloured = f.drawn_comment(*comment);
          recoloured.color = chosen->color;
          frame.comment = CompositorCommentIntent{
              recoloured.node,  recoloured.x,     recoloured.y,
              recoloured.width, recoloured.height, recoloured.title,
              recoloured.color};
          pending_comment_ = PendingComment{recoloured, *comment, 0U};
        }
        break;
      }
    }
  }
}

} // namespace mediaserver::editor
