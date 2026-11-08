// Drawing the nodes: title, ports, inline values and badges.

#include "compositor_frame.hpp"

#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/shapes.hpp>

namespace mediaserver::editor {

using namespace panel_helpers;
using namespace compositor_ui;

void CompositorPanel::draw_nodes(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const compositor::CompositorGraph &compositor_graph = f.compositor_graph;
  const std::span<const ElementInfo> elements = f.elements;
  CompositorFrame &frame = f.frame;
  auto &nodes = f.nodes;
  auto &inputs = f.inputs;
  auto &outputs = f.outputs;
  auto &lifted = f.lifted;
  auto &chrome_readable = f.chrome_readable;
  auto &inline_editable = f.inline_editable;
  auto &wire_width = f.wire_width;
  auto &snapped = f.snapped;
  auto &connecting_source = f.connecting_source;
  snapped = f.snapped_input();
  const CompositorPortVisual *const snapped_source = f.snapped_output();
  if (connecting_from_.has_value()) {
    const CompositorPortVisual *const from =
        find_port(outputs, *connecting_from_);
    if (from != nullptr) {
      // The wire lands on the port it would connect to, so the operator sees
      // the result before letting go.
      const float end_x = snapped != nullptr ? snapped->x : ctx.pointer_x();
      const float end_y = snapped != nullptr ? snapped->y : ctx.pointer_y();
      wire(ctx, from->x, from->y, end_x, end_y,
           port_color(ctx, connecting_type_), wire_width + 0.5F, view_.zoom);
    }
  }
  if (node_menu_.has_value() && node_menu_->from.has_value()) {
    // The wire waits at the search for the node it will feed.
    if (const CompositorPortVisual *const from =
            find_port(outputs, *node_menu_->from)) {
      wire(ctx, from->x, from->y, node_menu_->screen_x, node_menu_->screen_y,
           port_color(ctx, node_menu_->type), wire_width + 0.5F, view_.zoom);
    }
  }
  if (relink_.has_value() && relink_->moving_source) {
    // Wires lifted off an output follow the pointer from their inputs.
    const float start_x =
        snapped_source != nullptr ? snapped_source->x : ctx.pointer_x();
    const float start_y =
        snapped_source != nullptr ? snapped_source->y : ctx.pointer_y();
    for (const doc::NodeId id : relink_->edges) {
      const compositor::GraphEdge *const edge = compositor_graph.edge(id);
      const CompositorPortVisual *const to =
          edge != nullptr ? find_port(inputs, edge->to) : nullptr;
      if (to != nullptr) {
        wire(ctx, start_x, start_y, to->x, to->y,
             port_color(ctx, connecting_type_), wire_width + 0.5F, view_.zoom);
      }
    }
  }

  const auto input_connected = [&](compositor::EdgeEndpoint endpoint) {
    return std::any_of(compositor_graph.edges.begin(),
                       compositor_graph.edges.end(),
                       [endpoint](const compositor::GraphEdge &edge) noexcept {
                         return edge.to == endpoint;
                       });
  };
  const auto output_connected = [&](compositor::EdgeEndpoint endpoint) {
    return std::any_of(compositor_graph.edges.begin(),
                       compositor_graph.edges.end(),
                       [endpoint](const compositor::GraphEdge &edge) noexcept {
                         return edge.from == endpoint;
                       });
  };
  connecting_source =
      connecting_from_.has_value() ? find_port(outputs, *connecting_from_)
                                   : nullptr;
  const float node_title_height =
      compositor_text_height(ctx, compositor_title_text) * view_.zoom;
  const float node_detail_height =
      compositor_text_height(ctx, compositor_detail_text) * view_.zoom;
  const float port_radius = std::max(2.5F, compositor_port_radius * view_.zoom);

  for (const CompositorNodeVisual &visual : nodes) {
    const bool selected = std::binary_search(
        selected_nodes_.begin(), selected_nodes_.end(), visual.node->node);
    const bool diagnostic =
        node_has_diagnostic(compositor_graph, visual.node->node);
    const bool parked = node_parked(compositor_graph, visual.node->node);
    const bool waiting = node_disabled(compositor_graph, visual.node->node);
    const gui::UiColor kind = compositor_node_color(ctx, visual.node->kind);
    const gui::UiColor body = scale(ctx.theme().panel, 1.18F);
    // Everything this node draws from here is one mark, scaled as one when
    // it is lifted by a drag or settles after a drop.
    const std::size_t first_vertex = ctx.draw_list().vertices().size();
    const bool carried =
        dragging_node_ != doc::NodeId{} && drag_moved_ && ctx.pointer_down() &&
        std::any_of(drag_group_.begin(), drag_group_.end(),
                    [&visual](const GroupMember &member) noexcept {
                      return member.node == visual.node->node;
                    });
    const float card_radius = std::max(3.0F, 10.0F * view_.zoom);

    // A soft shadow lifts the card off the grid, further while it is carried.
    ctx.shadow(visual.bounds, card_radius,
               std::max(4.0F, (carried ? 22.0F : 12.0F) * view_.zoom));
    if (selected) {
      ctx.glow(visual.bounds, ctx.theme().accent, card_radius,
               std::max(4.0F, 10.0F * view_.zoom));
    }
    ctx.rounded(visual.bounds, body, card_radius);

    gui::Rect below_header = visual.bounds;
    gui::Rect node_header =
        gui::cut_top(below_header, metrics_.header_height * view_.zoom);
    // The title band carries the node's kind as a tint, with a fine line of
    // it where the band meets the ports.
    ctx.rounded(node_header, blend_color(ctx.theme().header, kind, 0.2F),
                card_radius, gui::UiDrawList::corner_top);
    ctx.panel(gui::cut_bottom(node_header, std::max(1.0F, 1.5F * view_.zoom)),
              with_alpha(kind, 0.7F));
    if (diagnostic || selected) {
      ctx.rounded_outline(
          visual.bounds, diagnostic ? ctx.theme().warning : ctx.theme().accent,
          card_radius, std::max(1.0F, std::min(2.0F, 2.0F * view_.zoom)));
    }

    const std::string_view title =
        visual.node->label.empty()
            ? compositor::node_kind_name(visual.node->kind)
            : std::string_view{visual.node->label};
    // The text lines, padded in from the sides; the title stops short of
    // the problem badge and the toggle at its right when they show.
    const gui::Rect text_area =
        gui::inset(visual.bounds, compositor_node_padding * view_.zoom, 0.0F);
    const gui::Rect toggle = f.toggle_bounds(visual);
    const gui::Rect title_line{text_area.x, visual.bounds.y + 5.0F * view_.zoom,
                               text_area.width, node_title_height};
    const gui::Rect title_bounds =
        chrome_readable
            ? gui::span_x(title_line, text_area.x,
                          toggle.x - toggle.width - 6.0F * view_.zoom)
            : title_line;
    const bool passed_through = bypassed(*visual.node);
    if (compositor_label(ctx, title_bounds, title, ctx.theme().text,
                         compositor_title_text, view_.zoom)) {
      const std::string detail =
          passed_through ? std::string{"BYPASSED"}
          : waiting      ? std::string{"DISABLED"}
                         : compositor_node_detail(*visual.node, elements);
      const gui::Rect detail_line{text_area.x, title_line.bottom(),
                                  text_area.width, node_detail_height};
      static_cast<void>(compositor_label(
          ctx, detail_line, detail,
          passed_through ? compositor_bypass_color : ctx.theme().text_muted,
          compositor_detail_text, view_.zoom));
      if (waiting) {
        ctx.tooltip(detail_line,
                    disabled_reason(compositor_graph, *visual.node));
      }
    }

    // Names share a row: an input on the left, an output on the right. Each
    // gets the room the other leaves, so the two never overlap.
    const auto port_label_width = [&](const CompositorPortVisual *port) {
      if (port == nullptr) {
        return 0.0F;
      }
      const std::optional<float> baked =
          ctx.text_size_at_most(compositor_pin_text * view_.zoom);
      return baked.has_value() ? ctx.font_width(port->descriptor->name, *baked)
                               : 0.0F;
    };
    const auto row_port = [&](const std::vector<CompositorPortVisual> &ports,
                              std::size_t row) -> const CompositorPortVisual * {
      std::size_t seen = 0U;
      for (const CompositorPortVisual &port : ports) {
        if (port.endpoint.node == visual.node->node && seen++ == row) {
          return std::addressof(port);
        }
      }
      return nullptr;
    };
    // The toggle: a chevron pointing down while open, right while collapsed.
    const bool folded = collapsed(*visual.node);
    if (chrome_readable) {
      const bool over = toggle.contains(ctx.pointer_x(), ctx.pointer_y());
      if (over) {
        ctx.panel(toggle, with_alpha(ctx.theme().widget_hover, 0.9F));
      }
      draw_twirl(ctx, toggle.center_x(), toggle.center_y(), !folded,
                 over ? ctx.theme().text : ctx.theme().text_muted);
      ctx.tooltip(toggle, folded ? "Expand node" : "Collapse node");
      // Problems with the node are marked beside it, and named on hover: a
      // filled mark for an error, a ring for a warning.
      std::string problems;
      bool marked = false;
      bool error = false;
      for (const compositor::GraphDiagnostic &problem :
           compositor_graph.diagnostics) {
        if (problem.subject != visual.node->node &&
            problem.related != visual.node->node) {
          continue;
        }
        if (!problems.empty()) {
          problems += " | ";
        }
        problems += compositor_diagnostic_severity_label(problem);
        problems += ": ";
        problems += compositor_diagnostic_message(problem);
        // A branch that does not reach Output is already drawn faded.
        marked = marked ||
                 problem.code != compositor::DiagnosticCode::DeadBranch;
        error = error ||
                problem.severity == compositor::DiagnosticSeverity::Error;
      }
      if (marked) {
        const gui::Rect badge{toggle.x - toggle.width - 3.0F * view_.zoom, toggle.y,
                              toggle.width, toggle.height};
        const float radius = badge.width * 0.42F;
        const float centre_x = badge.center_x();
        const float centre_y = badge.center_y();
        gui::add_disc(ctx.draw_list(), gui::UiPoint{centre_x, centre_y},
                      radius, ctx.theme().warning);
        if (!error) {
          gui::add_disc(
              ctx.draw_list(), gui::UiPoint{centre_x, centre_y},
              std::max(radius - std::max(1.5F, 2.0F * view_.zoom), 0.0F),
              blend_color(ctx.theme().header, kind, 0.16F));
        }
        ctx.label(gui::centered(badge, ctx.font_width("!", 0.75F) + 2.0F,
                                badge.height),
                  "!", error ? ctx.theme().background : ctx.theme().warning,
                  0.75F);
        ctx.tooltip(badge, problems);
      }
    }

    const float pin_inset = compositor_pin_inset * view_.zoom;
    const float gap = compositor_pin_label_gap * view_.zoom;
    const auto shown_ports = [&](const std::vector<CompositorPortVisual> &ports) {
      return static_cast<std::size_t>(std::count_if(
          ports.begin(), ports.end(),
          [&visual](const CompositorPortVisual &port) noexcept {
            return port.endpoint.node == visual.node->node;
          }));
    };
    const std::size_t rows = std::max(shown_ports(inputs), shown_ports(outputs));
    const gui::Rect pins = gui::inset(visual.bounds, pin_inset, 0.0F);
    for (std::size_t row = 0U; row < rows; ++row) {
      const CompositorPortVisual *const input = row_port(inputs, row);
      const CompositorPortVisual *const output = row_port(outputs, row);
      const float row_height = metrics_.row_height * view_.zoom;
      const float input_width = port_label_width(input);
      const float output_width = port_label_width(output);
      const float right = pins.right();
      if (input != nullptr) {
        const bool compatible = connecting_source != nullptr &&
                                can_connect(compositor_graph,
                                            *connecting_source, *input, lifted);
        const float limit =
            output != nullptr ? right - output_width - gap : right;
        static_cast<void>(compositor_label(
            ctx,
            gui::span_x(gui::rule_y(pins, input->y, row_height), pins.x, limit),
            input->descriptor->name,
            compatible ? ctx.theme().text : ctx.theme().text_muted,
            compositor_pin_text, view_.zoom));
      }
      if (output != nullptr) {
        const float left =
            input != nullptr ? pins.x + input_width + gap : pins.x;
        const float width = std::min(output_width, std::max(right - left, 0.0F));
        static_cast<void>(compositor_label(
            ctx,
            gui::span_x(gui::rule_y(pins, output->y, row_height), right - width,
                        right),
            output->descriptor->name, ctx.theme().text_muted,
            compositor_pin_text, view_.zoom));
      }
    }

    // The values shown on the node, editable there at a working zoom.
    const std::span<const InlineValue> values =
        folded ? std::span<const InlineValue>{}
               : inline_values(visual.node->kind);
    for (std::size_t index = 0U; index < values.size(); ++index) {
      const InlineValue &value = values[index];
      const gui::Rect row = f.inline_row(visual, index);
      const gui::Rect field = f.inline_field(row);
      const bool idle =
          inline_value_idle(compositor_graph, *visual.node, value);
      static_cast<void>(compositor_label(
          ctx, gui::span_x(row, pins.x, field.x - gap * 0.5F), value.label,
          idle ? with_alpha(ctx.theme().text_muted, 0.5F)
               : ctx.theme().text_muted,
          compositor_pin_text, view_.zoom));
      const double current = inline_value_of(*visual.node, value.property);
      if (idle || !inline_editable || !f.inside_canvas(field)) {
        std::array<char, 32U> text{};
        const int length =
            idle ? std::snprintf(text.data(), text.size(), "-")
                 : std::snprintf(text.data(), text.size(), "%.*f",
                                 value.decimals, current);
        const std::string_view shown{
            text.data(), static_cast<std::size_t>(std::max(length, 0))};
        const std::optional<float> baked =
            ctx.text_size_at_most(compositor_pin_text * view_.zoom);
        const float width =
            baked.has_value() ? ctx.font_width(shown, *baked) : 0.0F;
        // Right-aligned in the field, cut at its left.
        static_cast<void>(compositor_label(
            ctx,
            gui::span_x(row, std::max(field.right() - width, field.x),
                        field.right()),
            shown, ctx.theme().text, compositor_pin_text, view_.zoom));
        continue;
      }
      const gui::UiId field_id =
          gui::ui_id(id_, "compositor.inline",
              visual.node->node.value() * 16U +
                  static_cast<core::u64>(value.property));
      double authored{};
      NumberFieldEdit edit{};
      if (!inline_fields_.draw(ctx, field_id, field, {}, current, value.speed,
                               authored, value.decimals, &edit) ||
          !std::isfinite(authored)) {
        continue;
      }
      const double clamped = std::clamp(value.whole ? std::round(authored)
                                                    : authored,
                                        value.minimum, value.maximum);
      if (value.whole) {
        const auto whole = static_cast<core::u32>(clamped);
        if (whole == static_cast<core::u32>(current)) {
          continue;
        }
        frame.property_edit = CompositorPropertyEditIntent{
            visual.node->node, value.property, CompositorPropertyValue{whole}};
      } else {
        const auto amount = static_cast<float>(clamped);
        if (amount == static_cast<float>(current)) {
          continue;
        }
        frame.property_edit = CompositorPropertyEditIntent{
            visual.node->node, value.property, CompositorPropertyValue{amount}};
      }
      if (edit.kind == NumberFieldEditKind::Nudge) {
        frame.numeric_nudge =
            NumericNudge{field_id, static_cast<core::u64>(value.property) + 1U,
                         edit.direction, edit.modifiers};
      }
    }

    // Dots over names: a filled shape is connected and a ring is open; a node
    // waiting on an open input says DISABLED rather than drawing it as a
    // problem. While a wire is being drawn, ports it cannot reach fade back.
    for (const CompositorPortVisual &port : inputs) {
      if (port.endpoint.node != visual.node->node) {
        continue;
      }
      const bool connected = input_connected(port.endpoint);
      const bool compatible =
          connecting_source != nullptr &&
          can_connect(compositor_graph, *connecting_source, port, lifted);
      const bool target = snapped == std::addressof(port);
      gui::UiColor color = port_color(ctx, port.descriptor->type);
      if (connecting_source != nullptr && !compatible) {
        color = with_alpha(color, 0.3F);
      }
      const float radius =
          target ? port_radius + 2.0F
                 : (compatible ? port_radius + 1.0F : port_radius);
      if (target) {
        draw_port_shape(ctx, port.descriptor->type, port.x, port.y,
                        radius + 1.5F, ctx.theme().text);
      }
      draw_port(ctx, port.descriptor->type, port.x, port.y, radius, color,
                connected || target, body);
    }
    for (const CompositorPortVisual &port : outputs) {
      if (port.endpoint.node != visual.node->node) {
        continue;
      }
      const bool hovered = port.hit.contains(ctx.pointer_x(), ctx.pointer_y()) &&
                           !connecting_from_.has_value();
      const bool active = connecting_from_ == port.endpoint ||
                          snapped_source == std::addressof(port);
      const gui::UiColor color = port_color(ctx, port.descriptor->type);
      const float radius =
          hovered || active ? port_radius + 1.0F : port_radius;
      if (hovered || active) {
        draw_port_shape(ctx, port.descriptor->type, port.x, port.y,
                        radius + 1.5F, ctx.theme().text);
      }
      draw_port(ctx, port.descriptor->type, port.x, port.y, radius,
                connecting_source != nullptr && !active
                    ? with_alpha(color, 0.3F)
                    : color,
                output_connected(port.endpoint) || active, body);
    }
    // A node whose output never reaches Output, or that is waiting for an
    // input, is drawn faded: it costs nothing on screen until it is wired.
    if ((parked || waiting) && !selected) {
      ctx.rounded(visual.bounds, with_alpha(ctx.theme().background, 0.45F),
                  card_radius);
    }
    // A bypassed node's body is dimmed below its title, which stays
    // readable, and edged in the transport's amber.
    if (passed_through) {
      const float below = std::min(metrics_.header_height * view_.zoom,
                                   visual.bounds.height);
      ctx.rounded(gui::Rect{visual.bounds.x, visual.bounds.y + below,
                            visual.bounds.width, visual.bounds.height - below},
                  with_alpha(ctx.theme().background, 0.5F), card_radius,
                  gui::UiDrawList::corner_bottom);
      ctx.rounded_outline(visual.bounds,
                          with_alpha(compositor_bypass_color, 0.8F), card_radius,
                          std::max(1.0F, std::min(2.0F, 2.0F * view_.zoom)));
    }
    // Brought here from another panel: outlined, fading, for a moment.
    if (visual.node->node == reveal_node_ && ctx.time_seconds() < reveal_until_) {
      const float left = static_cast<float>(reveal_until_ - ctx.time_seconds());
      const float grow = 3.0F;
      ctx.rounded_outline(
          gui::Rect{visual.bounds.x - grow, visual.bounds.y - grow,
                    visual.bounds.width + grow * 2.0F,
                    visual.bounds.height + grow * 2.0F},
          with_alpha(ctx.theme().accent, std::clamp(left / 1.5F, 0.0F, 1.0F)),
          card_radius + grow, 2.0F);
    }
    // Carried nodes rise a touch; a node just set down springs back from a
    // little small. At rest the factor is exactly one and nothing moves.
    const float plant = ctx.spring(
        gui::ui_id(id_, "compositor.plant", visual.node->node.value()),
        carried ? 1.025F : 1.0F);
    ctx.draw_list().scale_since(first_vertex, visual.bounds.center_x(),
                                visual.bounds.center_y(), plant);
  }
}

} // namespace mediaserver::editor
