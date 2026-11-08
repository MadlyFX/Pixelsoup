// Layout tools (align, straighten, arrange, comment, copy, paste,
// duplicate, rename) and the graph's keyboard.

#include "compositor_frame.hpp"

#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/shapes.hpp>

namespace mediaserver::editor {

using namespace panel_helpers;
using namespace compositor_ui;

// The selected nodes' boxes in graph units.
std::vector<LayoutBox> CompositorPanel::Frame::selected_boxes() {
  std::vector<LayoutBox> boxes;
  for (const doc::NodeId id : panel.selected_nodes_) {
    if (const compositor::GraphNode *const node = compositor_graph.node(id)) {
      const gui::UiPoint at = graph_position(*node);
      const gui::UiPoint size = panel.node_size(*node);
      boxes.push_back(LayoutBox{id, at.x, at.y, size.x, size.y});
    }
  }
  return boxes;
}

// Authors moves as one step, held on screen until they arrive.
void
CompositorPanel::Frame::apply_moves(
    const std::vector<CompositorMoveIntent> &moves) {
  for (const CompositorMoveIntent &move : moves) {
    const compositor::GraphNode *const node = compositor_graph.node(move.node);
    if (node == nullptr) {
      continue;
    }
    frame.move_nodes.push_back(move);
    std::erase_if(panel.pending_moves_, [&move](const PendingMove &held) {
      return held.node == move.node;
    });
    panel.pending_moves_.push_back(PendingMove{move.node, move.editor_x,
                                         move.editor_y, node->editor_x,
                                         node->editor_y, 0U});
  }
}

// Fits a graph-space rectangle in the canvas, no closer than 150%.
void CompositorPanel::Frame::frame_view(const gui::Rect &target) {
  constexpr float margin = 48.0F;
  const float fit = std::min(
      (canvas.width - margin * 2.0F) / std::max(target.width, 1.0F),
      (canvas.height - margin * 2.0F) / std::max(target.height, 1.0F));
  panel.view_.zoom = std::clamp(fit, compositor_minimum_zoom, 1.5F);
  panel.view_.centre_on(canvas, target.center_x(), target.center_y());
}

// Straighten Connection: each selected node moves up or down so the wire
// into its first connected input runs level; a node with no input wire
// levels the wire out of its first connected output instead. Left to
// right, so a straightened chain stays straight.
std::vector<CompositorMoveIntent> CompositorPanel::Frame::straightened() {
  std::vector<LayoutBox> boxes = selected_boxes();
  std::sort(boxes.begin(), boxes.end(),
            [](const LayoutBox &left, const LayoutBox &right) {
              return left.x != right.x ? left.x < right.x
                                       : left.node < right.node;
            });
  std::map<doc::NodeId, float> placed;
  const auto top_of = [&](const compositor::GraphNode &node) {
    const auto found = placed.find(node.node);
    return found != placed.end() ? found->second : graph_position(node).y;
  };
  const auto port_y = [&](const compositor::GraphNode &node, float row) {
    return top_of(node) + panel.metrics_.header_height +
           panel.metrics_.row_height * (row + 0.5F);
  };
  std::vector<CompositorMoveIntent> moves;
  for (const LayoutBox &box : boxes) {
    const compositor::GraphNode *const node = compositor_graph.node(box.node);
    const compositor::GraphEdge *in{};
    const compositor::GraphEdge *out{};
    for (const compositor::GraphEdge &edge : compositor_graph.edges) {
      if (edge.to.node == box.node &&
          (in == nullptr || edge.to.port < in->to.port)) {
        in = std::addressof(edge);
      }
      if (edge.from.node == box.node &&
          (out == nullptr || edge.from.port < out->from.port)) {
        out = std::addressof(edge);
      }
    }
    std::optional<float> y;
    if (in != nullptr) {
      if (const compositor::GraphNode *const source =
              compositor_graph.node(in->from.node)) {
        y = port_y(
                *source,
                panel.port_row(compositor_graph, *source, in->from.port, false)
                    .value_or(0.0F)) -
            panel.metrics_.header_height -
            panel.metrics_.row_height *
                (panel.port_row(compositor_graph, *node, in->to.port, true)
                     .value_or(0.0F) +
                 0.5F);
      }
    } else if (out != nullptr) {
      if (const compositor::GraphNode *const target =
              compositor_graph.node(out->to.node)) {
        y = port_y(*target,
                   panel.port_row(compositor_graph, *target, out->to.port, true)
                       .value_or(0.0F)) -
            panel.metrics_.header_height -
            panel.metrics_.row_height *
                (panel.port_row(compositor_graph, *node, out->from.port, false)
                     .value_or(0.0F) +
                 0.5F);
      }
    }
    if (y.has_value()) {
      placed[box.node] = *y;
      if (*y != box.y) {
        moves.push_back(CompositorMoveIntent{box.node, box.x, *y});
      }
    }
  }
  return moves;
}

// Arrange: columns by how many wires stand between a node and the end of
// its chain, right to left toward Output, each column ordered to follow
// what it feeds. The selection's top-right corner stays put. With fewer
// than two nodes selected it arranges the whole graph.
std::vector<CompositorMoveIntent> CompositorPanel::Frame::arranged() {
  std::vector<LayoutBox> boxes = selected_boxes();
  if (boxes.size() < 2U) {
    boxes.clear();
    for (const compositor::GraphNode &node : compositor_graph.nodes) {
      const gui::UiPoint at = graph_position(node);
      const gui::UiPoint size = panel.node_size(node);
      boxes.push_back(LayoutBox{node.node, at.x, at.y, size.x, size.y});
    }
  }
  std::vector<CompositorMoveIntent> moves;
  if (boxes.empty()) {
    return moves;
  }
  std::map<doc::NodeId, const LayoutBox *> chosen;
  float right = -std::numeric_limits<float>::infinity();
  float top = std::numeric_limits<float>::infinity();
  for (const LayoutBox &box : boxes) {
    chosen.emplace(box.node, std::addressof(box));
    right = std::max(right, box.x + box.width);
    top = std::min(top, box.y);
  }
  // Longest run of wires to a node with nothing downstream among those
  // being arranged. A loop, which the graph refuses anyway, ends a run.
  std::map<doc::NodeId, std::size_t> depth;
  std::set<doc::NodeId> visiting;
  const std::function<std::size_t(doc::NodeId)> depth_of =
      [&](doc::NodeId id) -> std::size_t {
    if (const auto known = depth.find(id); known != depth.end()) {
      return known->second;
    }
    if (!visiting.insert(id).second) {
      return 0U;
    }
    std::size_t deepest = 0U;
    for (const compositor::GraphEdge &edge : compositor_graph.edges) {
      if (edge.from.node == id && chosen.contains(edge.to.node)) {
        deepest = std::max(deepest, depth_of(edge.to.node) + 1U);
      }
    }
    visiting.erase(id);
    depth[id] = deepest;
    return deepest;
  };
  std::map<std::size_t, std::vector<const LayoutBox *>> columns;
  for (const auto &[id, box] : chosen) {
    columns[depth_of(id)].push_back(box);
  }
  constexpr float column_gap = 80.0F;
  constexpr float row_gap = 30.0F;
  std::map<doc::NodeId, float> placed_y;
  float column_right = right;
  for (auto &[column, members] : columns) {
    float width{};
    for (const LayoutBox *box : members) {
      width = std::max(width, box->width);
    }
    // Each node lines up with what it feeds, averaged; the first column
    // keeps its current order.
    const auto key = [&](const LayoutBox *box) {
      float total{};
      std::size_t count{};
      for (const compositor::GraphEdge &edge : compositor_graph.edges) {
        if (edge.from.node == box->node && placed_y.contains(edge.to.node)) {
          total += placed_y.at(edge.to.node);
          ++count;
        }
      }
      return count != 0U ? total / static_cast<float>(count) : box->y;
    };
    std::sort(members.begin(), members.end(),
              [&](const LayoutBox *left, const LayoutBox *right_box) {
                const float a = key(left);
                const float b = key(right_box);
                return a != b ? a < b : left->node < right_box->node;
              });
    const float x = column_right - width;
    float y = top;
    for (const LayoutBox *box : members) {
      placed_y[box->node] = y;
      if (x != box->x || y != box->y) {
        moves.push_back(CompositorMoveIntent{box->node, x, y});
      }
      y += box->height + row_gap;
    }
    column_right = x - column_gap;
    static_cast<void>(column);
  }
  return moves;
}

// A comment box around the selected nodes, with room for its title.
void CompositorPanel::Frame::comment_selection() {
  if (const std::optional<gui::Rect> around = graph_extent(true)) {
    constexpr float margin = 28.0F;
    frame.comment = CompositorCommentIntent{
        doc::NodeId{},
        around->x - margin,
        around->y - margin - std::max(panel.metrics_.row_height, 24.0F),
        around->width + margin * 2.0F,
        around->height + margin * 2.0F +
            std::max(panel.metrics_.row_height, 24.0F),
        "Comment",
        0U};
  }
}

// Copy, paste and duplicate: the Director keeps the copied nodes; the ones
// a paste or duplicate adds are selected when they arrive.
void CompositorPanel::Frame::expect_new_nodes() {
  PasteBaseline baseline;
  for (const compositor::GraphNode &node : compositor_graph.nodes) {
    baseline.nodes.push_back(node.node);
  }
  std::sort(baseline.nodes.begin(), baseline.nodes.end());
  panel.paste_baseline_ = std::move(baseline);
}

void CompositorPanel::Frame::say(std::string text) {
  panel.notice_ = std::move(text);
  panel.notice_until_ = ctx.time_seconds() + 3.0;
}

std::ptrdiff_t CompositorPanel::Frame::unplaceable() {
  return std::count_if(panel.selected_nodes_.begin(),
                       panel.selected_nodes_.end(), [&](doc::NodeId id) {
                         const compositor::GraphNode *const node =
                             compositor_graph.node(id);
                         return node != nullptr &&
                                (node->kind == compositor::NodeKind::Output ||
                                 node->kind ==
                                     compositor::NodeKind::SceneLayer);
                       });
}

void CompositorPanel::Frame::copy_selection() {
  std::vector<doc::NodeId> chosen = panel.selected_nodes_;
  if (panel.selection_.kind == CompositorSelectionKind::Comment) {
    chosen.push_back(panel.selection_.node);
  }
  if (!chosen.empty()) {
    frame.clipboard = CompositorClipboardIntent{
        CompositorClipboardAction::Copy, std::move(chosen)};
    say("Copied " + std::to_string(frame.clipboard->nodes.size()) +
        (frame.clipboard->nodes.size() == 1U ? " item" : " items"));
  }
}

void CompositorPanel::Frame::paste_here() {
  const bool inside = canvas.contains(ctx.pointer_x(), ctx.pointer_y());
  const compositor::WirePoint at =
      to_graph(inside ? ctx.pointer_x() : canvas.x + canvas.width * 0.5F,
               inside ? ctx.pointer_y() : canvas.y + canvas.height * 0.5F);
  frame.clipboard = CompositorClipboardIntent{
      CompositorClipboardAction::Paste, {}, at.x, at.y};
  expect_new_nodes();
}

void CompositorPanel::Frame::duplicate_selection() {
  if (panel.selected_nodes_.empty()) {
    return;
  }
  if (unplaceable() != 0) {
    say("Output and Scene Layers are not duplicated: one per graph, one per "
        "source");
  }
  frame.clipboard =
      CompositorClipboardIntent{CompositorClipboardAction::Duplicate,
                                panel.selected_nodes_, 32.0F, 32.0F};
  expect_new_nodes();
}

void CompositorPanel::Frame::start_rename() {
  if (panel.selection_.kind == CompositorSelectionKind::Node) {
    if (const compositor::GraphNode *const node =
            compositor_graph.node(panel.selection_.node)) {
      panel.rename_ = Rename{node->node,
                       node->label.empty()
                           ? std::string{compositor::node_kind_name(node->kind)}
                           : node->label};
    }
  } else if (panel.selection_.kind == CompositorSelectionKind::Comment) {
    if (const compositor::GraphComment *const comment =
            comment_of(panel.selection_.node)) {
      panel.rename_ = Rename{comment->node, comment->title};
    }
  }
}

void CompositorPanel::handle_keyboard(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  CompositorFrame &frame = f.frame;
  auto &canvas = f.canvas;
  auto &wiring = f.wiring;
  // The keyboard, while the graph has it and no title is being typed.
  const bool pointer_in_canvas =
      canvas.contains(ctx.pointer_x(), ctx.pointer_y());
  if (focused_ && !ctx.typing() && !rename_.has_value() && !wiring &&
      dragging_node_ == doc::NodeId{} && !node_menu_.has_value()) {
    const std::string_view typed =
        pointer_in_canvas ? ctx.shortcut_text() : std::string_view{};
    if (ctx.key_pressed(gui::Key::Copy)) {
      f.copy_selection();
    } else if (ctx.key_pressed(gui::Key::Paste)) {
      f.paste_here();
    } else if (ctx.key_pressed(gui::Key::Duplicate)) {
      f.duplicate_selection();
    }
    if (ctx.key_pressed(gui::Key::Rename)) {
      f.start_rename();
    }
    // F frames the selection (or the graph), Home the whole graph.
    if (typed.find_first_of("fF") != std::string_view::npos) {
      std::optional<gui::Rect> target = f.graph_extent(true);
      if (!target.has_value()) {
        target = f.graph_extent(false);
      }
      if (target.has_value()) {
        f.frame_view(*target);
        f.build_visuals();
      }
    }
    if (ctx.key_pressed(gui::Key::Home)) {
      if (const std::optional<gui::Rect> target = f.graph_extent(false)) {
        f.frame_view(*target);
        f.build_visuals();
      }
    }
    if (typed.find_first_of("qQ") != std::string_view::npos) {
      f.apply_moves(f.straightened());
    }
    // D bypasses the selection, or restores it, as Disable does in other
    // compositors.
    if (typed.find_first_of("dD") != std::string_view::npos &&
        !ctx.ctrl_held()) {
      f.toggle_bypass();
    }
    if (typed.find_first_of("cC") != std::string_view::npos &&
        !selected_nodes_.empty()) {
      f.comment_selection();
    }
    // Arrows nudge the selection 10 units, or 1 with Shift. A run of
    // nudges in one direction is one undo step.
    const float step = ctx.shift_held() ? 1.0F : 10.0F;
    float nudge_x{};
    float nudge_y{};
    if (ctx.key_pressed(gui::Key::Left)) nudge_x -= step;
    if (ctx.key_pressed(gui::Key::Right)) nudge_x += step;
    if (ctx.key_pressed(gui::Key::Up)) nudge_y -= step;
    if (ctx.key_pressed(gui::Key::Down)) nudge_y += step;
    if ((nudge_x != 0.0F || nudge_y != 0.0F) && !selected_nodes_.empty()) {
      std::vector<CompositorMoveIntent> moves;
      for (const LayoutBox &box : f.selected_boxes()) {
        moves.push_back(
            CompositorMoveIntent{box.node, box.x + nudge_x, box.y + nudge_y});
      }
      f.apply_moves(moves);
      const bool horizontal = nudge_x != 0.0F;
      frame.numeric_nudge = NumericNudge{
          gui::ui_id(id_, "compositor.nudge"), horizontal ? 1001U : 1002U,
          (horizontal ? nudge_x : nudge_y) > 0.0F
              ? NumberFieldNudgeDirection::Increase
              : NumberFieldNudgeDirection::Decrease,
          ctx.shift_held() ? gui::KeyMods::Shift : gui::KeyMods::None};
      f.build_visuals();
    }
  }
}

} // namespace mediaserver::editor
