// The toolbar, and the Add palette it opens.

#include "compositor_frame.hpp"

#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/shapes.hpp>

namespace mediaserver::editor {

using namespace panel_helpers;
using namespace compositor_ui;

bool
CompositorPanel::Frame::palette_matches(const CompositorPaletteItem &item) {
  const std::string_view category =
      compositor_category_names[category_index(item.category)];
  return matches(compositor::node_kind_name(item.kind), panel.node_search_) ||
         matches(category, panel.node_search_) ||
         matches(item.search_terms, panel.node_search_);
}

bool
CompositorPanel::Frame::palette_matches_text(const CompositorPaletteItem &item,
                                             std::string_view search) {
  const std::string_view category =
      compositor_category_names[category_index(item.category)];
  return matches(compositor::node_kind_name(item.kind), search) ||
         matches(category, search) || matches(item.search_terms, search);
}

bool
CompositorPanel::Frame::palette_direct_matches(
    const CompositorPaletteItem &item) {
  return matches(compositor::node_kind_name(item.kind), panel.node_search_) ||
         matches(item.search_terms, panel.node_search_);
}

void CompositorPanel::draw_toolbar(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const gui::Rect &bounds = f.bounds;
  const compositor::CompositorGraph &compositor_graph = f.compositor_graph;
  CompositorFrame &frame = f.frame;
  auto &toolbar = f.toolbar;
  auto &content = f.content;
  auto &canvas = f.canvas;
  auto &output_count = f.output_count;
  auto &palette_bounds = f.palette_bounds;
  ctx.push_clip(bounds);
  ctx.panel(bounds, scale(ctx.theme().background, 0.86F));
  ctx.panel(toolbar, ctx.theme().background);
  ctx.panel(gui::Rect{toolbar.x, toolbar.bottom() - 1.0F,
                      toolbar.width, 1.0F},
            ctx.theme().divider);
  output_count = static_cast<std::size_t>(std::count_if(
      compositor_graph.nodes.begin(), compositor_graph.nodes.end(),
      [](const compositor::GraphNode &node) noexcept {
        return node.kind == compositor::NodeKind::Output;
      }));
  // Toolbar buttons are as wide as their words, so none is cut short.
  const float button_height = std::max(toolbar.height - 6.0F, 0.0F);
  const auto button_width = [&ctx](std::string_view label, float minimum) {
    return std::max(minimum, std::ceil(ctx.font_width(label)) + 14.0F);
  };
  const std::string_view add_label =
      add_menu_open_ ? "ADD NODE  -" : "ADD NODE  +";
  const gui::Rect add_button{toolbar.x + 8.0F, toolbar.y + 3.0F,
                             button_width("ADD NODE  +", 94.0F), button_height};
  static_cast<void>(add_label);
  // The graph's one coloured button: a node is what this panel makes.
  if (pill_button(ctx, gui::ui_id(id_, "compositor.add_menu"), add_button,
                  gui::Icon::Plus, "Add node",
                  "Add a node (or right-click the canvas)",
                  ButtonTone::Primary, true, add_menu_open_)) {
    add_menu_open_ = !add_menu_open_;
    if (!add_menu_open_) {
      add_category_.reset();
      ctx.clear_focus();
    }
  }

  if (toolbar.width >= 520.0F) {
    const float navigation_x = add_button.right() + 8.0F;
    const gui::Rect zoom_out{navigation_x, toolbar.y + 3.0F,
                             button_width("-", 22.0F), button_height};
    const gui::Rect zoom_value{zoom_out.right() + 2.0F,
                               toolbar.y + 3.0F, button_width("250%", 48.0F),
                               button_height};
    const gui::Rect zoom_in{zoom_value.right() + 2.0F,
                            toolbar.y + 3.0F, button_width("+", 22.0F),
                            button_height};
    const gui::Rect reset_view{zoom_in.right() + 4.0F,
                               toolbar.y + 3.0F, button_width("RESET", 54.0F),
                               button_height};
    const float anchor_x = canvas.x + canvas.width * 0.5F;
    const float anchor_y = canvas.y + canvas.height * 0.5F;
    // View controls: marks, named on hover, with the zoom itself as the
    // one word.
    const auto navigation_button =
        [&](std::string_view salt, const gui::Rect &button,
            std::string_view label, std::optional<gui::Icon> icon = std::nullopt,
            std::string_view tip = {}, bool engaged = false) {
          return pill_button(ctx, gui::ui_id(id_, salt), button, icon,
                             icon.has_value() ? std::string_view{} : label,
                             tip, ButtonTone::Quiet, true, engaged);
        };
    if (navigation_button("compositor.zoom_out", zoom_out, "-",
                          gui::Icon::Minus, "Zoom out")) {
      f.set_zoom(view_.zoom / compositor_zoom_step, anchor_x, anchor_y);
    }
    std::array<char, 16U> zoom_text{};
    const int zoom_length =
        std::snprintf(zoom_text.data(), zoom_text.size(), "%.0f%%",
                      static_cast<double>(view_.zoom * 100.0F));
    if (navigation_button("compositor.zoom_reset", zoom_value,
                          std::string_view{zoom_text.data(),
                                           static_cast<std::size_t>(
                                               std::max(zoom_length, 0))},
                          std::nullopt, "Back to 100%")) {
      f.set_zoom(1.0F, anchor_x, anchor_y);
    }
    if (navigation_button("compositor.zoom_in", zoom_in, "+",
                          gui::Icon::Plus, "Zoom in")) {
      f.set_zoom(view_.zoom * compositor_zoom_step, anchor_x, anchor_y);
    }
    if (navigation_button("compositor.view_reset", reset_view, "RESET",
                          gui::Icon::Fit, "Reset the view")) {
      view_ = gui::CanvasView{};
    }
    const gui::Rect map_toggle{reset_view.right() + 4.0F,
                               toolbar.y + 3.0F, button_width("MAP", 44.0F),
                               button_height};
    if (navigation_button("compositor.minimap_toggle", map_toggle, "MAP",
                          gui::Icon::Map, "Overview map", minimap_)) {
      minimap_ = !minimap_;
    }
  }

  std::size_t palette_item_rows{};
  if (!node_search_.empty()) {
    palette_item_rows = static_cast<std::size_t>(
        std::count_if(compositor_palette_items.begin(),
                      compositor_palette_items.end(),
                      [&f](const CompositorPaletteItem &item) {
                        return f.palette_matches(item);
                      }));
  } else if (add_category_.has_value()) {
    palette_item_rows = static_cast<std::size_t>(std::count_if(
        compositor_palette_items.begin(), compositor_palette_items.end(),
        [&](const CompositorPaletteItem &item) {
          return category_index(item.category) == *add_category_;
        }));
  }
  const std::size_t palette_category_rows =
      node_search_.empty() ? compositor_category_names.size() : 0U;
  const float desired_palette_height =
      8.0F + palette_search_height +
      static_cast<float>(palette_category_rows + palette_item_rows) *
          palette_row_height;
  const float palette_width =
      std::min(292.0F, std::max(bounds.width - 16.0F, 0.0F));
  palette_bounds = gui::Rect{
      toolbar.x + 8.0F, content.y, palette_width,
      std::min(desired_palette_height,
               std::max(bounds.height - toolbar.height, 0.0F))};
  if (add_menu_open_ && ctx.pointer_pressed() &&
      !add_button.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      !palette_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
    add_menu_open_ = false;
    add_category_.reset();
    ctx.clear_focus();
  }

  std::array<char, 64U> graph_status{};
  const bool invalid_output_count = output_count > 1U;
  const std::size_t error_count = static_cast<std::size_t>(std::count_if(
      compositor_graph.diagnostics.begin(), compositor_graph.diagnostics.end(),
      [](const compositor::GraphDiagnostic &diagnostic) noexcept {
        return diagnostic.severity == compositor::DiagnosticSeverity::Error;
      }));
  const std::size_t warning_count = static_cast<std::size_t>(std::count_if(
      compositor_graph.diagnostics.begin(), compositor_graph.diagnostics.end(),
      [](const compositor::GraphDiagnostic &diagnostic) noexcept {
        return is_problem(diagnostic) &&
               diagnostic.severity == compositor::DiagnosticSeverity::Warning;
      }));
  const bool graph_problems = error_count + warning_count != 0U;
  int graph_status_length{};
  if (invalid_output_count) {
    graph_status_length =
        std::snprintf(graph_status.data(), graph_status.size(), "%llu outputs",
                      static_cast<unsigned long long>(output_count));
  } else if (!graph_problems) {
    graph_status_length = std::snprintf(
        graph_status.data(), graph_status.size(), "%llu nodes",
        static_cast<unsigned long long>(compositor_graph.nodes.size()));
  } else if (error_count != 0U && warning_count != 0U) {
    graph_status_length = std::snprintf(
        graph_status.data(), graph_status.size(), "%llu errors, %llu warnings",
        static_cast<unsigned long long>(error_count),
        static_cast<unsigned long long>(warning_count));
  } else if (error_count != 0U) {
    graph_status_length =
        std::snprintf(graph_status.data(), graph_status.size(), "%llu errors",
                      static_cast<unsigned long long>(error_count));
  } else {
    graph_status_length =
        std::snprintf(graph_status.data(), graph_status.size(), "%llu warnings",
                      static_cast<unsigned long long>(warning_count));
  }
  const std::string_view graph_status_text{
      graph_status.data(),
      static_cast<std::size_t>(std::max(graph_status_length, 0))};
  const float status_width = ctx.font_width(graph_status_text);
  const bool has_selection = selection_.kind != CompositorSelectionKind::None;
  const std::optional<CompositorDeleteIntent> removable =
      selection_delete(compositor_graph);
  const bool selected_required_output = has_selection && !removable.has_value();
  std::string remove_label;
  if (selection_.kind == CompositorSelectionKind::Edge) {
    remove_label = "Disconnect";
  } else if (selection_.kind == CompositorSelectionKind::Comment) {
    remove_label = "Delete comment";
  } else if (selected_required_output) {
    remove_label = "Output required";
  } else if (removable.has_value() && removable->targets.size() > 1U) {
    remove_label = "Delete " + std::to_string(removable->targets.size()) +
                   " nodes";
  } else {
    remove_label = "Delete node";
  }
  const float remove_width = button_width(remove_label, 86.0F) + 22.0F;
  const float trailing_control_width =
      has_selection ? remove_width + 12.0F : 0.0F;
  ctx.label(gui::Rect{toolbar.right() - status_width - 8.0F -
                          trailing_control_width,
                      toolbar.y, status_width, toolbar.height},
            graph_status_text,
            !invalid_output_count && !graph_problems ? ctx.theme().text_muted
                                                     : ctx.theme().warning);
  if (has_selection) {
    const gui::Rect remove_bounds{
        toolbar.right() - remove_width - 8.0F, toolbar.y + 3.0F,
        remove_width, button_height};
    // Destructive, so red-ringed, and set apart at the toolbar's far end.
    if (pill_button(ctx, gui::ui_id(id_, "compositor.remove"), remove_bounds,
                    selection_.kind == CompositorSelectionKind::Edge
                        ? gui::Icon::Close
                        : gui::Icon::Trash,
                    remove_label, "Delete (Del)", ButtonTone::Danger,
                    !selected_required_output) &&
        removable.has_value()) {
      frame.remove = removable;
    }
  }
  if (focused_ && has_selection && !ctx.typing() &&
      ctx.key_pressed(gui::Key::Delete)) {
    frame.remove = removable;
    frame.consume_delete = true;
  }
}

void CompositorPanel::draw_add_palette(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const compositor::CompositorGraph &compositor_graph = f.compositor_graph;
  CompositorFrame &frame = f.frame;
  auto &canvas = f.canvas;
  auto &output_count = f.output_count;
  auto &palette_bounds = f.palette_bounds;
  auto &palette_pointer_blocked = f.palette_pointer_blocked;
  if (palette_pointer_blocked) {
    ctx.pop_pointer_block();
  }
  if (add_menu_open_) {
    ctx.push_clip(palette_bounds);
    ctx.panel(palette_bounds, scale(ctx.theme().panel, 1.04F));
    ctx.outline(palette_bounds, ctx.theme().widget_hover);
    const gui::Rect palette_search{palette_bounds.x + 4.0F,
                                   palette_bounds.y + 4.0F,
                                   std::max(palette_bounds.width - 8.0F, 0.0F),
                                   palette_search_height - 4.0F};
    search_field(ctx, gui::ui_id(id_, "compositor.node_search"), palette_search,
                 node_search_, "Search nodes and effects");

    float palette_y = palette_bounds.y + palette_search_height + 4.0F;
    const auto add_palette_node = [&](const CompositorPaletteItem &item,
                                      const gui::Rect &row, core::u64 salt) {
      const bool enabled =
          item.kind != compositor::NodeKind::Output || output_count == 0U;
      gui::Interaction interaction{};
      if (enabled) {
        interaction = ctx.grab(gui::ui_id(id_, "compositor.palette_item", salt), row);
      }
      ctx.panel(row,
                enabled ? (interaction.held
                               ? ctx.theme().widget_active
                               : (interaction.hovered ? ctx.theme().widget_hover
                                                      : ctx.theme().row))
                        : ctx.theme().track);
      ctx.panel(gui::Rect{row.x, row.y, 3.0F, row.height},
                with_alpha(compositor_node_color(ctx, item.kind),
                           enabled ? 1.0F : 0.35F));
      std::string label{compositor::node_kind_name(item.kind)};
      if (!enabled) {
        label += "  ·  ALREADY ADDED";
      }
      ctx.label(gui::Rect{row.x + 10.0F, row.y,
                          std::max(row.width - 14.0F, 0.0F), row.height},
                label,
                enabled && interaction.hovered ? ctx.theme().text
                                               : ctx.theme().text_muted);
      if (!interaction.clicked) {
        return;
      }
      const float cascade =
          static_cast<float>(compositor_graph.nodes.size() % 4U) * 14.0F;
      const gui::UiPoint size = new_node_size(item.kind);
      const float node_height = size.y;
      const float node_width = size.x;
      frame.add_node = CompositorAddIntent{
          item.kind,
          (canvas.width * 0.5F - view_.pan_x) / view_.zoom - node_width * 0.5F + cascade,
          (canvas.height * 0.5F - view_.pan_y) / view_.zoom - node_height * 0.5F +
              cascade};
      add_menu_open_ = false;
      add_category_.reset();
      node_search_.clear();
      ctx.clear_focus();
    };

    if (node_search_.empty()) {
      for (std::size_t category = 0U;
           category < compositor_category_names.size(); ++category) {
        const gui::Rect row{palette_bounds.x + 4.0F, palette_y,
                            std::max(palette_bounds.width - 8.0F, 0.0F),
                            palette_row_height};
        const gui::Interaction interaction =
            ctx.grab(gui::ui_id(id_, "compositor.palette_category", category), row);
        const bool open = add_category_ == static_cast<core::u8>(category);
        ctx.panel(row, interaction.hovered ? ctx.theme().widget_hover
                                           : ctx.theme().header);
        draw_twirl(ctx, row.x + 9.0F, row.y + row.height * 0.5F, open,
                   interaction.hovered ? ctx.theme().accent
                                       : ctx.theme().text_muted);
        ctx.label(gui::Rect{row.x + 19.0F, row.y,
                            std::max(row.width - 23.0F, 0.0F), row.height},
                  compositor_category_names[category],
                  interaction.hovered ? ctx.theme().text
                                      : ctx.theme().text_muted);
        if (interaction.clicked) {
          add_category_ =
              open ? std::optional<core::u8>{}
                   : std::optional<core::u8>{static_cast<core::u8>(category)};
        }
        palette_y += palette_row_height;
        if (!open) {
          continue;
        }
        for (std::size_t index = 0U; index < compositor_palette_items.size();
             ++index) {
          const CompositorPaletteItem &item = compositor_palette_items[index];
          if (category_index(item.category) != category) {
            continue;
          }
          const gui::Rect item_row{palette_bounds.x + 8.0F, palette_y,
                                   std::max(palette_bounds.width - 12.0F, 0.0F),
                                   palette_row_height};
          add_palette_node(item, item_row, index);
          palette_y += palette_row_height;
        }
      }
    } else {
      std::size_t match_count{};
      // Direct name/alias matches lead; category-only matches follow. Typing
      // "output" therefore puts Output before Composite even though both live
      // in the Combine & Output category.
      for (std::size_t pass = 0U; pass < 2U; ++pass) {
        for (std::size_t index = 0U; index < compositor_palette_items.size();
             ++index) {
          const CompositorPaletteItem &item = compositor_palette_items[index];
          const bool direct = f.palette_direct_matches(item);
          if (!f.palette_matches(item) || direct != (pass == 0U)) {
            continue;
          }
          const gui::Rect item_row{palette_bounds.x + 4.0F, palette_y,
                                   std::max(palette_bounds.width - 8.0F, 0.0F),
                                   palette_row_height};
          add_palette_node(item, item_row, index);
          palette_y += palette_row_height;
          ++match_count;
        }
      }
      if (match_count == 0U) {
        ctx.label(gui::Rect{palette_bounds.x + 10.0F, palette_y,
                            std::max(palette_bounds.width - 20.0F, 0.0F),
                            palette_row_height},
                  "NO NODES FOUND", ctx.theme().text_muted);
      }
    }
    ctx.pop_clip();
  }
}

} // namespace mediaserver::editor
