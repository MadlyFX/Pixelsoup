// Pressing, dragging, collapsing and editing nodes and comment boxes.

#include "compositor_frame.hpp"

#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/shapes.hpp>

namespace mediaserver::editor {

using namespace panel_helpers;
using namespace compositor_ui;

bool CompositorPanel::Frame::is_selected(doc::NodeId node) {
  return std::binary_search(panel.selected_nodes_.begin(),
                            panel.selected_nodes_.end(), node);
}

void CompositorPanel::Frame::add_to_selection(doc::NodeId node) {
  const auto at = std::lower_bound(panel.selected_nodes_.begin(),
                                   panel.selected_nodes_.end(), node);
  if (at == panel.selected_nodes_.end() || *at != node) {
    panel.selected_nodes_.insert(at, node);
  }
}

// Where a node is now in graph units: held, if a released move is still
// on its way, or as authored.
gui::UiPoint
CompositorPanel::Frame::graph_position(const compositor::GraphNode &node) {
  const std::optional<gui::UiPoint> held = panel.pending_position(node.node);
  return held.has_value() ? *held : gui::UiPoint{node.editor_x, node.editor_y};
}

// Collapses or opens nodes: the selection, when the toggled node is in it.
void CompositorPanel::Frame::toggle_collapse(doc::NodeId node, bool fold) {
  std::vector<doc::NodeId> targets = is_selected(node)
                                         ? panel.selected_nodes_
                                         : std::vector<doc::NodeId>{node};
  for (const doc::NodeId target : targets) {
    std::erase_if(panel.pending_collapse_, [target](const auto &held) noexcept {
      return held.first == target;
    });
    panel.pending_collapse_.emplace_back(target, fold);
  }
  frame.collapse = CompositorCollapseIntent{std::move(targets), fold};
}

// The toggle sits at the right end of the title line.
gui::Rect
CompositorPanel::Frame::toggle_bounds(const CompositorNodeVisual &visual) {
  const float side =
      std::max(compositor_text_height(ctx, compositor_title_text), 14.0F) *
      panel.view_.zoom;
  return gui::Rect{visual.bounds.right() - side -
                       4.0F * panel.view_.zoom,
                   visual.bounds.y + 5.0F * panel.view_.zoom, side, side};
}

// An inline value's row - below the port rows - and its field.
gui::Rect CompositorPanel::Frame::inline_row(const CompositorNodeVisual &visual,
                                             std::size_t index) {
  const std::size_t port_rows =
      std::max({compositor::input_ports(visual.node->kind).size(),
                compositor::output_ports(visual.node->kind).size(),
                std::size_t{1U}});
  return gui::Rect{
      visual.bounds.x,
      visual.bounds.y + (panel.metrics_.header_height +
                         panel.metrics_.row_height *
                             static_cast<float>(port_rows + index)) *
                            panel.view_.zoom,
      visual.bounds.width, panel.metrics_.row_height * panel.view_.zoom};
}

gui::Rect CompositorPanel::Frame::inline_field(const gui::Rect &row) {
  gui::Rect inner =
      gui::inset(row, gui::Padding{0.0F, 2.0F * panel.view_.zoom,
                                   compositor_pin_inset * panel.view_.zoom,
                                   2.0F * panel.view_.zoom});
  return gui::cut_right(inner,
                        std::min(84.0F * panel.view_.zoom, row.width * 0.5F));
}

bool CompositorPanel::Frame::inside_canvas(const gui::Rect &rect) noexcept {
  return rect.x >= canvas.x && rect.y >= canvas.y &&
         rect.right() <= canvas.right() &&
         rect.bottom() <= canvas.bottom();
}

void CompositorPanel::interact_nodes(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const compositor::CompositorGraph &compositor_graph = f.compositor_graph;
  CompositorFrame &frame = f.frame;
  auto &canvas = f.canvas;
  auto &nodes = f.nodes;
  auto &inputs = f.inputs;
  auto &outputs = f.outputs;
  auto &selection_pressed = f.selection_pressed;
  auto &drag_released = f.drag_released;
  auto &released_node = f.released_node;
  auto &chrome_readable = f.chrome_readable;
  auto &inline_editable = f.inline_editable;
  auto &comment_header = f.comment_header;
  bool layout_changed = false;
  drag_released = false;
  for (const CompositorPortVisual &port : outputs) {
    const gui::Interaction interaction =
        clipped_grab(ctx,
                     gui::ui_id(id_, "compositor.output_port",
                         port.endpoint.node.value() * 16U + port.endpoint.port),
                     port.hit, canvas);
    ctx.cursor_for(interaction, gui::Cursor::Hand);
    if (!interaction.pressed) {
      continue;
    }
    select_only(port.endpoint.node);
    frame.selection = selection_;
    selection_pressed = true;
    const std::vector<doc::NodeId> wired = f.edges_at(port.endpoint, true);
    if (ctx.alt_held()) {
      // Alt-click breaks every wire leaving this output.
      if (!wired.empty()) {
        frame.rewire = CompositorRewireIntent{wired, {}};
      }
    } else if (ctx.ctrl_held() && !wired.empty()) {
      // Ctrl-drag lifts them to move to another output.
      relink_ = Relink{wired, true, port.endpoint};
      connecting_type_ = port.descriptor->type;
    } else {
      connecting_from_ = port.endpoint;
      connecting_type_ = port.descriptor->type;
    }
  }
  chrome_readable = view_.zoom >= compositor_chrome_zoom;
  inline_editable = view_.zoom >= compositor_inline_edit_zoom &&
                               !node_menu_.has_value() &&
                               !context_menu_.has_value();
  // The inline field under the pointer, if it is editable there.
  const auto over_inline_field = [&](const CompositorNodeVisual &visual) {
    if (!inline_editable || collapsed(*visual.node)) {
      return false;
    }
    const std::span<const InlineValue> values =
        inline_values(visual.node->kind);
    for (std::size_t index = 0U; index < values.size(); ++index) {
      if (inline_value_idle(compositor_graph, *visual.node, values[index])) {
        continue;
      }
      const gui::Rect field = f.inline_field(f.inline_row(visual, index));
      if (f.inside_canvas(field) &&
          field.contains(ctx.pointer_x(), ctx.pointer_y())) {
        return true;
      }
    }
    return false;
  };
  if (chrome_readable) {
    for (const CompositorNodeVisual &visual : nodes) {
      const gui::Interaction toggle = clipped_grab(
          ctx, gui::ui_id(id_, "compositor.collapse", visual.node->node.value()),
          f.toggle_bounds(visual), canvas);
      if (toggle.pressed) {
        selection_pressed = true;
      }
      if (toggle.clicked) {
        f.toggle_collapse(visual.node->node, !collapsed(*visual.node));
      }
    }
  }
  for (const CompositorNodeVisual &visual : nodes) {
    const gui::UiId node_id =
        gui::ui_id(id_, "compositor.node", visual.node->node.value());
    if (ctx.active() != node_id && over_inline_field(visual)) {
      // The field drawn on the node takes this press, and the node is chosen
      // as if it had been pressed.
      if (ctx.pointer_pressed()) {
        selection_pressed = true;
        if (!f.is_selected(visual.node->node)) {
          select_only(visual.node->node);
          frame.selection = selection_;
        }
      }
      continue;
    }
    const gui::Interaction interaction =
        clipped_grab(ctx, node_id, visual.bounds, canvas);
    const doc::NodeId pressed_node = visual.node->node;
    if (interaction.pressed) {
      selection_pressed = true;
      // With Alt or Ctrl, a press on an input acts on that input's wire
      // instead of moving the node: Alt breaks it, Ctrl lifts it to move.
      const auto input = std::find_if(
          inputs.begin(), inputs.end(), [&](const CompositorPortVisual &port) {
            return port.endpoint.node == visual.node->node &&
                   port.hit.contains(ctx.pointer_x(), ctx.pointer_y());
          });
      const std::vector<doc::NodeId> wired =
          input != inputs.end() ? f.edges_at(input->endpoint, false)
                                : std::vector<doc::NodeId>{};
      const compositor::GraphEdge *const lifted_edge =
          wired.empty() ? nullptr : compositor_graph.edge(wired.front());
      if (input != inputs.end() && ctx.alt_held() && !wired.empty()) {
        select_only(pressed_node);
        frame.rewire = CompositorRewireIntent{wired, {}};
      } else if (input != inputs.end() && ctx.ctrl_held() &&
                 lifted_edge != nullptr) {
        select_only(pressed_node);
        relink_ = Relink{{lifted_edge->node}, false, input->endpoint};
        connecting_from_ = lifted_edge->from;
        connecting_type_ = input->descriptor->type;
      } else if (ctx.ctrl_held()) {
        // Ctrl-click toggles one node in or out of the selection.
        if (f.is_selected(pressed_node)) {
          std::erase(selected_nodes_, pressed_node);
          selection_ = selected_nodes_.empty()
                           ? CompositorSelection{}
                           : CompositorSelection{CompositorSelectionKind::Node,
                                                 selected_nodes_.front()};
        } else {
          f.add_to_selection(pressed_node);
          selection_ =
              CompositorSelection{CompositorSelectionKind::Node, pressed_node};
        }
      } else {
        // A press on a node already in a multiple selection keeps it, so the
        // whole selection can be dragged; Shift adds the node to it.
        const bool already = f.is_selected(pressed_node);
        press_was_selected_ = already && selected_nodes_.size() > 1U;
        if (selection_.kind != CompositorSelectionKind::Node) {
          selected_nodes_.clear();
        }
        if (ctx.shift_held()) {
          f.add_to_selection(pressed_node);
        } else if (!already) {
          selected_nodes_.assign(1U, pressed_node);
        }
        selection_ =
            CompositorSelection{CompositorSelectionKind::Node, pressed_node};
        const bool on_title =
            ctx.pointer_y() < visual.bounds.y + metrics_.header_height * view_.zoom;
        if (on_title && clicks_.double_click(pressed_node.value() * 64U + 7U,
                                             ctx.time_seconds())) {
          // Double-click a node's title to rename it.
          rename_ = Rename{pressed_node,
                           visual.node->label.empty()
                               ? std::string{compositor::node_kind_name(
                                     visual.node->kind)}
                               : visual.node->label};
        } else {
          dragging_node_ = pressed_node;
          drag_moved_ = false;
          const gui::UiPoint start =
              view_.to_graph(canvas, visual.bounds.x, visual.bounds.y);
          drag_x_ = start.x;
          drag_y_ = start.y;
          drag_offset_x_ = ctx.pointer_x() - visual.bounds.x;
          drag_offset_y_ = ctx.pointer_y() - visual.bounds.y;
          drag_group_.clear();
          for (const doc::NodeId member : selected_nodes_) {
            if (const compositor::GraphNode *const node =
                    compositor_graph.node(member)) {
              const gui::UiPoint at = f.graph_position(*node);
              drag_group_.push_back(GroupMember{member, at.x, at.y});
            }
          }
        }
      }
      frame.selection = selection_;
    }
    if (interaction.held && dragging_node_ == pressed_node) {
      ctx.set_cursor(gui::Cursor::Move);
      const float next_x =
          (ctx.pointer_x() - canvas.x - view_.pan_x - drag_offset_x_) / view_.zoom;
      const float next_y =
          (ctx.pointer_y() - canvas.y - view_.pan_y - drag_offset_y_) / view_.zoom;
      const bool stepped = next_x != drag_x_ || next_y != drag_y_;
      layout_changed = layout_changed || stepped;
      drag_moved_ = drag_moved_ || stepped;
      drag_x_ = next_x;
      drag_y_ = next_y;
      if (interaction.released) {
        const auto grabbed = std::find_if(
            drag_group_.begin(), drag_group_.end(),
            [pressed_node](const GroupMember &entry) noexcept {
              return entry.node == pressed_node;
            });
        if (drag_moved_ && grabbed != drag_group_.end()) {
          // Every node in the group moves by the same amount, in one step.
          const float dx = drag_x_ - grabbed->start_x;
          const float dy = drag_y_ - grabbed->start_y;
          for (const GroupMember &member : drag_group_) {
            const compositor::GraphNode *const node =
                compositor_graph.node(member.node);
            const float x = member.start_x + dx;
            const float y = member.start_y + dy;
            if (node == nullptr || (x == node->editor_x && y == node->editor_y)) {
              continue;
            }
            frame.move_nodes.push_back(CompositorMoveIntent{member.node, x, y});
            std::erase_if(pending_moves_, [&member](const PendingMove &held) {
              return held.node == member.node;
            });
            pending_moves_.push_back(PendingMove{member.node, x, y,
                                                 node->editor_x,
                                                 node->editor_y, 0U});
          }
        } else if (!drag_moved_ && press_was_selected_ && !ctx.shift_held()) {
          // A click, not a drag, on one of several selected nodes picks it
          // out on its own.
          select_only(pressed_node);
        }
        drag_released = true;
        released_node = pressed_node;
        if (drag_moved_) {
          // Set down: each moved node starts a little small and springs
          // back to size, so it feels planted where it was let go.
          for (const GroupMember &member : drag_group_) {
            ctx.settle(gui::ui_id(id_, "compositor.plant", member.node.value()),
                       0.94F);
          }
        }
      }
    }
  }
  comment_header = std::max(metrics_.row_height, 24.0F) * view_.zoom;
  for (auto comment = compositor_graph.comments.rbegin();
       comment != compositor_graph.comments.rend(); ++comment) {
    const compositor::GraphComment drawn = f.drawn_comment(*comment);
    const gui::Rect box = view_.to_screen(
        canvas, gui::Rect{drawn.x, drawn.y, drawn.width, drawn.height});
    const gui::Rect title{box.x, box.y, box.width,
                          std::min(comment_header, box.height)};
    const gui::Rect corner{box.right() - 14.0F,
                           box.bottom() - 14.0F, 14.0F, 14.0F};
    const gui::Interaction resize = clipped_grab(
        ctx, gui::ui_id(id_, "compositor.comment.size", comment->node.value()), corner,
        canvas);
    const gui::Interaction move = clipped_grab(
        ctx, gui::ui_id(id_, "compositor.comment", comment->node.value()), title,
        canvas);
    ctx.cursor_for(resize, gui::Cursor::ResizeDiagonal);
    ctx.cursor_for(move, gui::Cursor::Move);
    if (resize.pressed || move.pressed) {
      selection_ =
          CompositorSelection{CompositorSelectionKind::Comment, comment->node};
      selected_nodes_.clear();
      frame.selection = selection_;
      selection_pressed = true;
      if (move.pressed && clicks_.double_click(comment->node.value() * 64U + 9U,
                                               ctx.time_seconds())) {
        rename_ = Rename{comment->node, drawn.title};
      } else {
        CommentDrag drag{comment->node, resize.pressed, ctx.pointer_x(),
                         ctx.pointer_y(), drawn, {}};
        if (move.pressed) {
          // It carries the nodes that sit wholly inside it.
          for (const compositor::GraphNode &node : compositor_graph.nodes) {
            const gui::UiPoint at = f.graph_position(node);
            const gui::UiPoint size = node_size(node);
            if (at.x >= drawn.x && at.y >= drawn.y &&
                at.x + size.x <= drawn.x + drawn.width &&
                at.y + size.y <= drawn.y + drawn.height) {
              drag.carried.push_back(GroupMember{node.node, at.x, at.y});
            }
          }
        }
        comment_drag_ = std::move(drag);
      }
    }
    if ((resize.held || move.held) && comment_drag_.has_value() &&
        comment_drag_->comment == comment->node) {
      const float dx = (ctx.pointer_x() - comment_drag_->press_x) / view_.zoom;
      const float dy = (ctx.pointer_y() - comment_drag_->press_y) / view_.zoom;
      layout_changed = layout_changed || dx != comment_drag_->dx ||
                       dy != comment_drag_->dy;
      comment_drag_->dx = dx;
      comment_drag_->dy = dy;
      if (resize.released || move.released) {
        const compositor::GraphComment moved = f.drawn_comment(*comment);
        if (moved != *comment) {
          frame.comment = CompositorCommentIntent{
              moved.node, moved.x,      moved.y,    moved.width,
              moved.height, moved.title, moved.color};
          pending_comment_ = PendingComment{moved, *comment, 0U};
        }
        for (const GroupMember &member : comment_drag_->carried) {
          const compositor::GraphNode *const node =
              compositor_graph.node(member.node);
          const float x = member.start_x + dx;
          const float y = member.start_y + dy;
          if (node == nullptr || (x == node->editor_x && y == node->editor_y)) {
            continue;
          }
          frame.move_nodes.push_back(CompositorMoveIntent{member.node, x, y});
          std::erase_if(pending_moves_, [&member](const PendingMove &held) {
            return held.node == member.node;
          });
          pending_moves_.push_back(PendingMove{member.node, x, y, node->editor_x,
                                               node->editor_y, 0U});
        }
        comment_drag_.reset();
        layout_changed = true;
      }
    }
  }
  if (layout_changed) {
    f.build_visuals();
  }
}

} // namespace mediaserver::editor
