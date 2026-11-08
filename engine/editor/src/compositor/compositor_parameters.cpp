// The Parameters panel beside or below the graph.

#include "compositor_frame.hpp"

#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/shapes.hpp>

namespace mediaserver::editor {

using namespace panel_helpers;
using namespace compositor_ui;

void CompositorPanel::draw_parameters(Frame &f) {
  gui::UiContext &ctx = f.ctx;
  const compositor::CompositorGraph &compositor_graph = f.compositor_graph;
  CompositorFrame &frame = f.frame;
  auto &parameter_node = f.parameter_node;
  auto &parameter_bounds = f.parameter_bounds;
  if (parameter_node != nullptr && parameter_bounds.width > 0.0F &&
      parameter_bounds.height > 0.0F) {
    ctx.push_clip(parameter_bounds);
    ctx.panel(parameter_bounds, scale(ctx.theme().panel, 0.98F));
    // The node's kind as a soft lit bar along the top.
    ctx.rounded(gui::Rect{parameter_bounds.x + 8.0F, parameter_bounds.y + 2.0F,
                          std::max(parameter_bounds.width - 16.0F, 0.0F), 3.0F},
                compositor_node_color(ctx, parameter_node->kind), 1.5F);
    // The heading's lines are as tall as the font says, never clipped.
    const float heading_height = ctx.line_height();
    const float subheading_height =
        compositor_text_height(ctx, compositor_detail_text);
    const float header_height =
        std::max(32.0F, std::ceil(3.0F + heading_height + subheading_height));
    gui::Rect heading =
        gui::inset(parameter_bounds, gui::Padding{8.0F, 2.0F, 8.0F, 0.0F});
    ctx.label(gui::cut_top(heading, heading_height), "Parameters",
              ctx.theme().text);
    static_cast<void>(compositor_label(
        ctx, gui::cut_top(heading, subheading_height),
        compositor::node_kind_name(parameter_node->kind), ctx.theme().text_muted,
        compositor_detail_text, 1.0F));
    // Bypass is a node's own switch, beside its name.
    if (compositor::node_kind_bypassable(parameter_node->kind)) {
      const bool off = bypassed(*parameter_node);
      const std::string_view caption = off ? "Bypassed" : "Bypass";
      gui::Rect corner =
          gui::inset(parameter_bounds, gui::Padding{0.0F, 5.0F, 8.0F, 0.0F});
      gui::Rect button =
          gui::cut_right(corner, std::ceil(ctx.font_width(caption)) + 16.0F);
      button.height = std::max(header_height - 10.0F, 18.0F);
      const gui::Interaction pressed = ctx.grab(
          gui::ui_id(id_, "compositor.parameters.bypass"), button);
      if (pressed.clicked) {
        if (!f.is_selected(parameter_node->node)) {
          select_only(parameter_node->node);
        }
        f.toggle_bypass();
      }
      ctx.cursor_for(pressed, gui::Cursor::Hand);
      ctx.rounded(button, off ? with_alpha(compositor_bypass_color, 0.85F)
                              : (pressed.hovered ? ctx.theme().widget_hover
                                                 : ctx.theme().widget),
                  button.height * 0.5F);
      ctx.label(gui::inset(button, gui::Padding{8.0F, 0.0F, 0.0F, 0.0F}),
                caption, off ? ctx.theme().background : ctx.theme().text);
      ctx.tooltip(button, "Pass this node through as if it did nothing (D)");
    }

    // The fields scroll beneath the heading. Their height is last frame's,
    // which is exact for every frame but the first after a selection - and
    // that frame starts at the top anyway.
    constexpr float row_height = 22.0F;
    constexpr float row_stride = 26.0F;
    constexpr float scrollbar_width = 8.0F;
    gui::Rect parameter_body = parameter_bounds;
    static_cast<void>(gui::cut_top(parameter_body, header_height));
    const float maximum_scroll =
        std::max(parameter_content_height_ - parameter_body.height, 0.0F);
    const bool scrollable = maximum_scroll > 0.0F;
    if (parameter_body.contains(ctx.pointer_x(), ctx.pointer_y())) {
      parameter_scroll_ -= ctx.wheel() * row_stride * 2.0F;
    }
    parameter_scroll_ = std::clamp(parameter_scroll_, 0.0F, maximum_scroll);
    // Rows 8px in from the sides - and clear of the scroll bar while it
    // shows - starting 4px under the heading.
    gui::Metrics row_metrics = ctx.theme().metrics;
    row_metrics.inset = 8.0F;
    row_metrics.gap = 4.0F;
    row_metrics.control = row_height;
    row_metrics.row_gap = row_stride - row_height;
    gui::FormLayout rows{
        gui::inset(parameter_body,
                   gui::Padding{0.0F, 0.0F,
                                scrollable ? scrollbar_width : 0.0F, 0.0F}),
        row_metrics, parameter_scroll_};
    const float content_width = rows.width();

    // The label column fits this node's longest label. When the panel is
    // too narrow for that and a usable field, each label sits above its
    // field instead of being cut short.
    float widest_label{};
    for (const std::string_view label :
         compositor_parameter_labels(parameter_node->kind)) {
      widest_label =
          std::max(widest_label, ctx.font_width(gui::sentence_case(label)));
    }
    constexpr float minimum_field_width = 84.0F;
    const float inline_label_width = std::max(60.0F, widest_label + 12.0F);
    const bool stacked_labels =
        content_width - inline_label_width < minimum_field_width;
    const float label_width = stacked_labels ? 0.0F : inline_label_width;
    const float stacked_label_height = ctx.line_height() + 2.0F;
    ctx.push_clip(parameter_body);
    // One labelled field row; returns where the field goes.
    const auto next_field = [&](std::string_view label) {
      const std::string shown = gui::sentence_case(label);
      if (stacked_labels) {
        ctx.label(rows.next(stacked_label_height, 0.0F), shown,
                  ctx.theme().text_muted);
      }
      gui::Rect field = rows.control();
      const gui::Rect label_cell = gui::cut_left(field, label_width);
      if (!stacked_labels) {
        ctx.label(label_cell, shown, ctx.theme().text_muted);
      }
      return field;
    };
    // A row spanning the panel, for buttons and notes.
    const auto full_row = [&]() { return rows.control(); };
    // A collapsible heading; true while its fields are shown.
    const auto section = [&](std::string_view title, core::u32 bit) {
      const gui::Rect row = full_row();
      const core::u32 mask = core::u32{1U} << bit;
      const gui::Interaction interaction = ctx.grab(
          gui::ui_id(id_, "compositor.parameters.section", bit), row);
      if (interaction.clicked) {
        collapsed_sections_ ^= mask;
      }
      const bool open = (collapsed_sections_ & mask) == 0U;
      ctx.cursor_for(interaction, gui::Cursor::Hand);
      ctx.rounded(row, interaction.hovered ? ctx.theme().widget_hover
                                           : ctx.theme().header,
                  ctx.theme().radius);
      draw_twirl(ctx, row.x + 10.0F, row.y + row.height * 0.5F, open,
                 interaction.hovered ? ctx.theme().accent
                                     : ctx.theme().text_muted);
      ctx.label(gui::Rect{row.x + 21.0F, row.y,
                          std::max(row.width - 25.0F, 0.0F), row.height},
                gui::sentence_case(title),
                interaction.hovered ? ctx.theme().text : ctx.theme().text_muted);
      return open;
    };
    const auto field_id = [&](CompositorProperty property) noexcept {
      return gui::ui_id(id_, "compositor.parameter",
                 parameter_node->node.value() * 16U +
                     static_cast<core::u64>(property));
    };
    const auto emit_float = [&](CompositorProperty property, double authored,
                                float current, float minimum, float maximum) {
      if (!std::isfinite(authored)) {
        return;
      }
      const float value =
          static_cast<float>(std::clamp(authored, static_cast<double>(minimum),
                                        static_cast<double>(maximum)));
      if (value != current) {
        frame.property_edit = CompositorPropertyEditIntent{
            parameter_node->node, property, CompositorPropertyValue{value}};
      }
    };
    const auto record_nudge = [&](CompositorProperty property, gui::UiId field,
                                  const NumberFieldEdit &edit) {
      if (edit.kind == NumberFieldEditKind::Nudge &&
          frame.property_edit.has_value() &&
          frame.property_edit->property == property) {
        frame.numeric_nudge =
            NumericNudge{field, static_cast<core::u64>(property) + 1U,
                         edit.direction, edit.modifiers};
      }
    };
    // Every field below reads its value straight out of the node's
    // parameter block, so its default is the same member of a fresh block
    // of the node's kind: found by where the value sits in the block. A
    // value that is not a member (a computed one) has no default here.
    const compositor::NodeParameters fresh_parameters = std::visit(
        [](const auto &current) {
          return compositor::NodeParameters{std::decay_t<decltype(current)>{}};
        },
        parameter_node->parameters);
    const auto default_of = [&]<typename Value>(const Value &value)
        -> std::optional<Value> {
      return std::visit(
          [&](const auto &current) -> std::optional<Value> {
            using Block = std::decay_t<decltype(current)>;
            const auto begin = reinterpret_cast<std::uintptr_t>(&current);
            const auto at = reinterpret_cast<std::uintptr_t>(&value);
            if (at < begin || at + sizeof(Value) > begin + sizeof(Block)) {
              return std::nullopt;
            }
            Value fresh{};
            std::memcpy(&fresh,
                        reinterpret_cast<const unsigned char *>(
                            &std::get<Block>(fresh_parameters)) +
                            (at - begin),
                        sizeof(Value));
            return fresh;
          },
          parameter_node->parameters);
    };
    // The reset arrow at the end of a field's label while its value differs
    // from the default; true on the frame it is pressed.
    const auto reset_field = [&](CompositorProperty property,
                                 const gui::Rect &field, bool differs,
                                 std::string_view tip) {
      if (!differs || stacked_labels) {
        return false;
      }
      return reset_button(
          ctx, field_id(property) ^ gui::ui_id("reset"),
          gui::Rect{field.x - reset_button_width - 2.0F, field.y,
                    reset_button_width, field.height},
          tip);
    };
    const auto scalar_field =
        [&](std::string_view label, CompositorProperty property,
            const float &value, double speed, float minimum, float maximum,
            int decimals) {
          const gui::Rect field = next_field(label);
          double authored = 0.0;
          NumberFieldEdit edit{};
          const gui::UiId numeric_id = field_id(property);
          if (parameter_fields_.draw(ctx, numeric_id, field, {},
                                     static_cast<double>(value), speed,
                                     authored, decimals, &edit)) {
            emit_float(property, authored, value, minimum, maximum);
            record_nudge(property, numeric_id, edit);
          }
          if (const std::optional<float> fresh = default_of(value)) {
            std::array<char, 48U> tip{};
            std::snprintf(tip.data(), tip.size(), "Reset to default (%.*f)",
                          decimals, static_cast<double>(*fresh));
            if (reset_field(property, field,
                            std::abs(value - *fresh) >
                                1.0e-5F * std::max(1.0F, std::abs(*fresh)),
                            tip.data()) &&
                !frame.property_edit.has_value()) {
              frame.property_edit = CompositorPropertyEditIntent{
                  parameter_node->node, property,
                  CompositorPropertyValue{*fresh}};
            }
          }
        };
    // A two-state value shown as a button naming its state; a click flips it.
    const auto switch_field = [&](std::string_view label,
                                  CompositorProperty property, bool on,
                                  std::string_view on_text,
                                  std::string_view off_text) {
      const gui::Rect button = next_field(label);
      const gui::Interaction pressed = ctx.grab(field_id(property), button);
      ctx.panel(button, pressed.hovered ? ctx.theme().widget_hover
                                        : ctx.theme().widget);
      ctx.label(gui::Rect{button.x + 6.0F, button.y,
                          std::max(button.width - 6.0F, 0.0F), button.height},
                on ? on_text : off_text,
                pressed.hovered ? ctx.theme().accent : ctx.theme().text);
      if (pressed.clicked) {
        frame.property_edit = CompositorPropertyEditIntent{
            parameter_node->node, property,
            CompositorPropertyValue{on ? core::u32{0U} : core::u32{1U}}};
      }
    };
    // One of several named values, chosen from a menu under the field.
    const auto choice_field = [&](std::string_view label,
                                  CompositorProperty property,
                                  std::span<const std::string_view> options,
                                  std::size_t current) {
      const gui::Rect well = next_field(label);
      std::size_t selected = current;
      if (ctx.dropdown(field_id(property), well, options, selected) &&
          selected != current && selected < options.size()) {
        frame.property_edit = CompositorPropertyEditIntent{
            parameter_node->node, property,
            CompositorPropertyValue{static_cast<core::u32>(selected)}};
      }
    };
    const auto unsigned_field = [&](std::string_view label,
                                    CompositorProperty property,
                                    const core::u32 &value, double speed,
                                    core::u32 minimum, core::u32 maximum) {
      const gui::Rect field = next_field(label);
      if (const std::optional<core::u32> fresh = default_of(value)) {
        const std::string tip =
            "Reset to default (" + std::to_string(*fresh) + ")";
        if (reset_field(property, field, value != *fresh, tip) &&
            !frame.property_edit.has_value()) {
          frame.property_edit = CompositorPropertyEditIntent{
              parameter_node->node, property, CompositorPropertyValue{*fresh}};
        }
      }
      double authored{};
      NumberFieldEdit edit{};
      const gui::UiId numeric_id = field_id(property);
      if (parameter_fields_.draw(ctx, numeric_id, field, {},
                                 static_cast<double>(value), speed, authored, 0,
                                 &edit) &&
          std::isfinite(authored)) {
        const core::u32 normalized = static_cast<core::u32>(
            std::clamp(std::round(authored), static_cast<double>(minimum),
                       static_cast<double>(maximum)));
        if (normalized != value) {
          frame.property_edit =
              CompositorPropertyEditIntent{parameter_node->node, property,
                                           CompositorPropertyValue{normalized}};
          record_nudge(property, numeric_id, edit);
        }
      }
    };

    switch (parameter_node->kind) {
    case compositor::NodeKind::Brightness: {
      const auto *parameters = std::get_if<compositor::BrightnessParameters>(
          &parameter_node->parameters);
      if (parameters != nullptr) {
        scalar_field("AMOUNT", CompositorProperty::BrightnessAmount,
                     parameters->amount, 0.01, 0.0F, compositor::max_brightness,
                     2);
      }
      break;
    }
    case compositor::NodeKind::Saturation: {
      const auto *parameters = std::get_if<compositor::SaturationParameters>(
          &parameter_node->parameters);
      if (parameters != nullptr) {
        scalar_field("AMOUNT", CompositorProperty::SaturationAmount,
                     parameters->amount, 0.01, 0.0F, compositor::max_saturation,
                     2);
      }
      break;
    }
    case compositor::NodeKind::Levels: {
      const auto *parameters = std::get_if<compositor::LevelsParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      using Property = CompositorProperty;
      // Each channel is its own section, so its labels need no prefix.
      const auto channel_fields =
          [&](std::string_view title, core::u32 section_bit,
              const scene::LevelsChannelParameters &channel,
              const std::array<Property, 5U> &properties) {
            if (!section(title, section_bit)) {
              return;
            }
            scalar_field("IN BLACK", properties[0U], channel.input_black,
                         0.005, 0.0F,
                         std::max(channel.input_white - 0.0001F, 0.0F), 3);
            scalar_field("IN WHITE", properties[1U], channel.input_white,
                         0.005, std::min(channel.input_black + 0.0001F, 1.0F),
                         1.0F, 3);
            scalar_field("GAMMA", properties[2U], channel.gamma, 0.01,
                         scene::min_levels_gamma, scene::max_levels_gamma, 3);
            scalar_field("OUT BLACK", properties[3U], channel.output_black,
                         0.005, 0.0F, 1.0F, 3);
            scalar_field("OUT WHITE", properties[4U], channel.output_white,
                         0.005, 0.0F, 1.0F, 3);
          };
      channel_fields(
          "MASTER", levels_master_section, parameters->master,
          {Property::LevelsMasterInputBlack, Property::LevelsMasterInputWhite,
           Property::LevelsMasterGamma, Property::LevelsMasterOutputBlack,
           Property::LevelsMasterOutputWhite});
      channel_fields("RED", levels_red_section, parameters->red,
                     {Property::LevelsRedInputBlack,
                      Property::LevelsRedInputWhite, Property::LevelsRedGamma,
                      Property::LevelsRedOutputBlack,
                      Property::LevelsRedOutputWhite});
      channel_fields(
          "GREEN", levels_green_section, parameters->green,
          {Property::LevelsGreenInputBlack, Property::LevelsGreenInputWhite,
           Property::LevelsGreenGamma, Property::LevelsGreenOutputBlack,
           Property::LevelsGreenOutputWhite});
      channel_fields("BLUE", levels_blue_section, parameters->blue,
                     {Property::LevelsBlueInputBlack,
                      Property::LevelsBlueInputWhite, Property::LevelsBlueGamma,
                      Property::LevelsBlueOutputBlack,
                      Property::LevelsBlueOutputWhite});
      break;
    }
    case compositor::NodeKind::LiftGammaGain: {
      const auto *parameters = std::get_if<compositor::LiftGammaGainParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      using Property = CompositorProperty;
      const auto channel_fields =
          [&scalar_field](std::string_view prefix, const scene::Vec3 &value,
                          const std::array<Property, 3U> &properties,
                          double speed, float minimum, float maximum) {
            const std::string red = std::string{prefix} + " R";
            const std::string green = std::string{prefix} + " G";
            const std::string blue = std::string{prefix} + " B";
            scalar_field(red, properties[0U], value.x, speed, minimum, maximum,
                         3);
            scalar_field(green, properties[1U], value.y, speed, minimum,
                         maximum, 3);
            scalar_field(blue, properties[2U], value.z, speed, minimum, maximum,
                         3);
          };
      channel_fields("LIFT", parameters->lift,
                     {Property::LiftGammaGainLiftR,
                      Property::LiftGammaGainLiftG,
                      Property::LiftGammaGainLiftB},
                     0.005, scene::min_lift_gamma_gain_lift,
                     scene::max_lift_gamma_gain_lift);
      channel_fields("GAMMA", parameters->gamma,
                     {Property::LiftGammaGainGammaR,
                      Property::LiftGammaGainGammaG,
                      Property::LiftGammaGainGammaB},
                     0.01, scene::min_lift_gamma_gain_gamma,
                     scene::max_lift_gamma_gain_gamma);
      channel_fields("GAIN", parameters->gain,
                     {Property::LiftGammaGainGainR,
                      Property::LiftGammaGainGainG,
                      Property::LiftGammaGainGainB},
                     0.01, scene::min_lift_gamma_gain_gain,
                     scene::max_lift_gamma_gain_gain);
      break;
    }
    case compositor::NodeKind::ContrastPivot: {
      const auto *parameters = std::get_if<compositor::ContrastPivotParameters>(
          &parameter_node->parameters);
      if (parameters != nullptr) {
        scalar_field("CONTRAST", CompositorProperty::ContrastPivotContrast,
                     parameters->contrast, 0.01,
                     scene::min_contrast_pivot_contrast,
                     scene::max_contrast_pivot_contrast, 3);
        scalar_field("PIVOT", CompositorProperty::ContrastPivotPivot,
                     parameters->pivot, 0.005, scene::min_contrast_pivot_pivot,
                     scene::max_contrast_pivot_pivot, 3);
      }
      break;
    }
    case compositor::NodeKind::ColorMatrix: {
      const auto *parameters = std::get_if<compositor::ColorMatrixParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      using Property = CompositorProperty;
      constexpr std::array<Property, 12U> properties{{
          Property::ColorMatrixM00,
          Property::ColorMatrixM01,
          Property::ColorMatrixM02,
          Property::ColorMatrixM10,
          Property::ColorMatrixM11,
          Property::ColorMatrixM12,
          Property::ColorMatrixM20,
          Property::ColorMatrixM21,
          Property::ColorMatrixM22,
          Property::ColorMatrixOffsetR,
          Property::ColorMatrixOffsetG,
          Property::ColorMatrixOffsetB,
      }};
      constexpr std::array<std::string_view, 12U> labels{{
          "RED FROM R",
          "RED FROM G",
          "RED FROM B",
          "GREEN FROM R",
          "GREEN FROM G",
          "GREEN FROM B",
          "BLUE FROM R",
          "BLUE FROM G",
          "BLUE FROM B",
          "RED OFFSET",
          "GREEN OFFSET",
          "BLUE OFFSET",
      }};
      const std::array<float, 12U> values{{
          parameters->matrix[0U],
          parameters->matrix[1U],
          parameters->matrix[2U],
          parameters->matrix[3U],
          parameters->matrix[4U],
          parameters->matrix[5U],
          parameters->matrix[6U],
          parameters->matrix[7U],
          parameters->matrix[8U],
          parameters->offset.x,
          parameters->offset.y,
          parameters->offset.z,
      }};
      for (std::size_t index = 0U; index < properties.size(); ++index) {
        scalar_field(labels[index], properties[index], values[index], 0.01,
                     scene::min_color_matrix_coefficient,
                     scene::max_color_matrix_coefficient, 3);
      }
      break;
    }
    case compositor::NodeKind::WhiteBalance: {
      const auto *parameters = std::get_if<compositor::WhiteBalanceParameters>(
          &parameter_node->parameters);
      if (parameters != nullptr) {
        scalar_field("TEMPERATURE K",
                     CompositorProperty::WhiteBalanceTemperatureKelvin,
                     parameters->temperature_kelvin, 25.0,
                     scene::min_white_balance_temperature_kelvin,
                     scene::max_white_balance_temperature_kelvin, 0);
        scalar_field("TINT", CompositorProperty::WhiteBalanceTint,
                     parameters->tint, 0.01, scene::min_white_balance_tint,
                     scene::max_white_balance_tint, 3);
      }
      break;
    }
    case compositor::NodeKind::Curves: {
      const auto *parameters = std::get_if<compositor::CurvesParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      const auto emit_curves = [&](compositor::CurvesParameters value) {
        const auto normalized = scene::normalized_curves(value);
        if (normalized.has_value() && *normalized != *parameters) {
          frame.property_edit = CompositorPropertyEditIntent{
              parameter_node->node, CompositorProperty::CurvesParameters,
              CompositorPropertyValue{*normalized}};
        }
      };
      const gui::Rect action_row = full_row();
      const float action_width = action_row.width * 0.5F;
      const gui::Rect reset{action_row.x, action_row.y, action_width,
                            action_row.height};
      const gui::Rect add{action_row.x + action_width, action_row.y,
                          action_width, action_row.height};
      const gui::Interaction reset_it = ctx.grab(
          gui::ui_id(id_, "compositor.curves.reset", parameter_node->node.value()),
          reset);
      const gui::Interaction add_it = ctx.grab(
          gui::ui_id(id_, "compositor.curves.add", parameter_node->node.value()), add);
      ctx.panel(reset, reset_it.hovered ? ctx.theme().widget_hover
                                        : ctx.theme().widget);
      ctx.panel(add,
                add_it.hovered ? ctx.theme().widget_hover : ctx.theme().widget);
      ctx.label(reset, "RESET",
                reset_it.hovered ? ctx.theme().accent : ctx.theme().text_muted);
      const bool can_add =
          parameters->master.points.size() < scene::max_curve_control_points;
      ctx.label(add, "+ MASTER POINT",
                add_it.hovered && can_add ? ctx.theme().accent
                                          : ctx.theme().text_muted);
      if (reset_it.clicked) {
        compositor::CurvesParameters reset_value{};
        if (const auto normalized = scene::normalized_curves(reset_value)) {
          emit_curves(*normalized);
        }
      }
      if (add_it.clicked && can_add && parameters->master.points.size() >= 2U) {
        compositor::CurvesParameters updated = *parameters;
        updated.lut.clear();
        const std::size_t endpoint = updated.master.points.size() - 1U;
        const scene::CurveControlPoint previous =
            updated.master.points[endpoint - 1U];
        updated.master.points[endpoint] = scene::CurveControlPoint{
            (previous.x + 1.0F) * 0.5F, (previous.y + 1.0F) * 0.5F};
        updated.master.points.push_back({1.0F, 1.0F});
        emit_curves(std::move(updated));
      }

      const std::string count =
          std::to_string(parameters->master.points.size()) +
          " master control points";
      ctx.label(full_row(), count, ctx.theme().text_muted);

      if (parameters->master.points.size() > 2U) {
        const std::size_t point = parameters->master.points.size() - 2U;
        const scene::CurveControlPoint current =
            parameters->master.points[point];
        const float minimum_x =
            parameters->master.points[point - 1U].x + 0.0001F;
        const float maximum_x =
            parameters->master.points[point + 1U].x - 0.0001F;
        const auto point_field = [&](std::string_view label, bool x_axis,
                                     float value, float minimum, float maximum,
                                     core::u64 salt) {
          const gui::Rect field = next_field(label);
          double authored{};
          NumberFieldEdit edit{};
          const gui::UiId numeric_id =
              gui::ui_id(id_, "compositor.curves.point",
                  parameter_node->node.value() * 64U + salt);
          if (parameter_fields_.draw(ctx, numeric_id, field, {}, value, 0.005,
                                     authored, 3, &edit) &&
              std::isfinite(authored)) {
            const float normalized = static_cast<float>(
                std::clamp(authored, static_cast<double>(minimum),
                           static_cast<double>(maximum)));
            if (normalized != value) {
              compositor::CurvesParameters updated = *parameters;
              updated.lut.clear();
              if (x_axis) {
                updated.master.points[point].x = normalized;
              } else {
                updated.master.points[point].y = normalized;
              }
              emit_curves(std::move(updated));
              if (frame.property_edit.has_value() &&
                  edit.kind == NumberFieldEditKind::Nudge) {
                frame.numeric_nudge =
                    NumericNudge{numeric_id,
                                 static_cast<core::u64>(
                                     CompositorProperty::CurvesParameters) +
                                     1U,
                                 edit.direction, edit.modifiers};
              }
            }
          }
        };
        point_field("LAST POINT X", true, current.x, minimum_x, maximum_x, 1U);
        point_field("LAST POINT Y", false, current.y, 0.0F, 1.0F, 2U);
      }
      break;
    }
    case compositor::NodeKind::FilmGrain: {
      const auto *parameters = std::get_if<compositor::FilmGrainParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      scalar_field("AMOUNT", CompositorProperty::FilmGrainAmount,
                   parameters->amount, 0.005, scene::min_film_grain_amount,
                   scene::max_film_grain_amount, 3);
      scalar_field("PIXEL SCALE", CompositorProperty::FilmGrainPixelScale,
                   parameters->pixel_scale, 0.25,
                   scene::min_film_grain_pixel_scale,
                   scene::max_film_grain_pixel_scale, 2);
      scalar_field("CHROMA", CompositorProperty::FilmGrainChromaRatio,
                   parameters->chroma_ratio, 0.005,
                   scene::min_film_grain_chroma_ratio,
                   scene::max_film_grain_chroma_ratio, 3);

      const gui::Rect seed_field = next_field("SEED");
      double authored_seed{};
      NumberFieldEdit seed_edit{};
      const gui::UiId seed_id = field_id(CompositorProperty::FilmGrainSeed);
      if (parameter_fields_.draw(ctx, seed_id, seed_field, {},
                                 static_cast<double>(parameters->seed), 1.0,
                                 authored_seed, 0, &seed_edit) &&
          std::isfinite(authored_seed)) {
        const core::u32 value = static_cast<core::u32>(std::clamp(
            std::round(authored_seed), 0.0,
            static_cast<double>(std::numeric_limits<core::u32>::max())));
        if (value != parameters->seed) {
          frame.property_edit = CompositorPropertyEditIntent{
              parameter_node->node, CompositorProperty::FilmGrainSeed,
              CompositorPropertyValue{value}};
          record_nudge(CompositorProperty::FilmGrainSeed, seed_id, seed_edit);
        }
      }

      const gui::Rect mode_button = next_field("MODE");
      const gui::UiId mode_id = field_id(CompositorProperty::FilmGrainMode);
      const gui::Interaction mode_it = ctx.grab(mode_id, mode_button);
      ctx.panel(mode_button, mode_it.hovered ? ctx.theme().widget_hover
                                             : ctx.theme().widget);
      ctx.label(mode_button,
                parameters->mode == scene::FilmGrainMode::RGB ? "RGB"
                                                              : "MONOCHROME",
                mode_it.hovered ? ctx.theme().accent : ctx.theme().text);
      if (mode_it.clicked) {
        const core::u32 value =
            parameters->mode == scene::FilmGrainMode::RGB ? 0U : 1U;
        frame.property_edit = CompositorPropertyEditIntent{
            parameter_node->node, CompositorProperty::FilmGrainMode,
            CompositorPropertyValue{value}};
      }
      break;
    }
    case compositor::NodeKind::ChromaKey: {
      const auto *parameters = std::get_if<compositor::ChromaKeyParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      // The colour being keyed out, as a well above its three channels.
      colour_well_field(
          ctx, next_field("KEY COLOUR"), parameter_node->node, 0U,
          gui::UiColor{parameters->key_color.x, parameters->key_color.y,
                       parameters->key_color.z, 1.0F},
          {CompositorProperty::ChromaKeyRed, CompositorProperty::ChromaKeyGreen,
           CompositorProperty::ChromaKeyBlue},
          frame);
      scalar_field("KEY RED", CompositorProperty::ChromaKeyRed,
                   parameters->key_color.x, 0.005, 0.0F, 1.0F, 3);
      scalar_field("KEY GREEN", CompositorProperty::ChromaKeyGreen,
                   parameters->key_color.y, 0.005, 0.0F, 1.0F, 3);
      scalar_field("KEY BLUE", CompositorProperty::ChromaKeyBlue,
                   parameters->key_color.z, 0.005, 0.0F, 1.0F, 3);
      scalar_field("TOLERANCE", CompositorProperty::ChromaKeyTolerance,
                   parameters->tolerance, 0.005, 0.0F, 1.0F, 3);
      scalar_field("SOFTNESS", CompositorProperty::ChromaKeySoftness,
                   parameters->softness, 0.005, 0.0F, 1.0F, 3);
      // Whole pixels either way: positive grows the matte, negative shrinks it.
      const gui::Rect edge_field = next_field("EDGE PX");
      double authored_edge{};
      NumberFieldEdit edge_edit{};
      const gui::UiId edge_id = field_id(CompositorProperty::ChromaKeyEdgePx);
      if (parameter_fields_.draw(ctx, edge_id, edge_field, {},
                                 static_cast<double>(parameters->morphology_px),
                                 1.0, authored_edge, 0, &edge_edit) &&
          std::isfinite(authored_edge)) {
        constexpr auto reach =
            static_cast<double>(scene::max_key_morphology_radius_px);
        const double rounded =
            std::clamp(std::round(authored_edge), -reach, reach);
        if (rounded != static_cast<double>(parameters->morphology_px)) {
          frame.property_edit = CompositorPropertyEditIntent{
              parameter_node->node, CompositorProperty::ChromaKeyEdgePx,
              CompositorPropertyValue{static_cast<float>(rounded)}};
          record_nudge(CompositorProperty::ChromaKeyEdgePx, edge_id, edge_edit);
        }
      }
      scalar_field("SPILL", CompositorProperty::ChromaKeySpill,
                   parameters->spill_suppression, 0.005, 0.0F, 1.0F, 3);
      scalar_field("MIX", CompositorProperty::ChromaKeyMix, parameters->mix,
                   0.005, 0.0F, 1.0F, 3);
      break;
    }
    case compositor::NodeKind::LumaKey: {
      const auto *parameters = std::get_if<compositor::LumaKeyParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      // Each threshold stops at the other, so the range never turns over.
      scalar_field("LOW", CompositorProperty::LumaKeyLow,
                   parameters->low_threshold, 0.005, 0.0F,
                   parameters->high_threshold, 3);
      scalar_field("HIGH", CompositorProperty::LumaKeyHigh,
                   parameters->high_threshold, 0.005,
                   parameters->low_threshold, 1.0F, 3);
      scalar_field("LOW SOFTNESS", CompositorProperty::LumaKeyLowSoftness,
                   parameters->low_softness, 0.005, 0.0F, 1.0F, 3);
      scalar_field("HIGH SOFTNESS", CompositorProperty::LumaKeyHighSoftness,
                   parameters->high_softness, 0.005, 0.0F, 1.0F, 3);
      switch_field("MATTE", CompositorProperty::LumaKeyInvert,
                   parameters->invert, "INVERTED", "NORMAL");
      scalar_field("MIX", CompositorProperty::LumaKeyMix, parameters->mix,
                   0.005, 0.0F, 1.0F, 3);
      break;
    }
    case compositor::NodeKind::Matte: {
      const auto *parameters = std::get_if<compositor::MatteParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      switch_field("MODE", CompositorProperty::MatteMode,
                   parameters->mode == compositor::MatteMode::Luma, "LUMA",
                   "ALPHA");
      switch_field("MATTE", CompositorProperty::MatteInvert,
                   parameters->invert, "INVERTED", "NORMAL");
      switch_field("CLAMP INPUT", CompositorProperty::MatteClampInput,
                   parameters->clamp_input, "ON", "OFF");
      break;
    }
    case compositor::NodeKind::Blur: {
      const auto *parameters =
          std::get_if<compositor::BlurParameters>(&parameter_node->parameters);
      if (parameters != nullptr) {
        scalar_field("RADIUS", CompositorProperty::BlurRadius,
                     parameters->radius, 0.10, 0.0F,
                     compositor::max_blur_radius, 1);
      }
      break;
    }
    case compositor::NodeKind::BoxBlur: {
      const auto *parameters = std::get_if<compositor::BoxBlurParameters>(
          &parameter_node->parameters);
      if (parameters != nullptr) {
        unsigned_field("RADIUS PX", CompositorProperty::BoxBlurRadius,
                       parameters->radius_px, 1.0, 0U,
                       compositor::max_box_blur_radius);
        unsigned_field("PASSES", CompositorProperty::BoxBlurPasses,
                       parameters->passes, 1.0, 1U,
                       compositor::max_box_blur_passes);
      }
      break;
    }
    case compositor::NodeKind::DirectionalBlur: {
      const auto *parameters =
          std::get_if<compositor::DirectionalBlurParameters>(
              &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      scalar_field("LENGTH PX", CompositorProperty::DirectionalBlurLength,
                   parameters->length_px, 0.10, 0.0F,
                   compositor::max_directional_blur_length, 2);

      const gui::Rect field = next_field("ANGLE TURNS");
      double authored{};
      NumberFieldEdit edit{};
      const CompositorProperty property =
          CompositorProperty::DirectionalBlurAngleTurns;
      const gui::UiId numeric_id = field_id(property);
      if (parameter_fields_.draw(ctx, numeric_id, field, {},
                                 static_cast<double>(parameters->angle_turns),
                                 0.001, authored, 3, &edit) &&
          std::isfinite(authored)) {
        double wrapped = authored - std::floor(authored);
        if (wrapped < 0.0) {
          wrapped += 1.0;
        }
        const float value = static_cast<float>(wrapped);
        if (value != parameters->angle_turns) {
          frame.property_edit = CompositorPropertyEditIntent{
              parameter_node->node, property, CompositorPropertyValue{value}};
          record_nudge(property, numeric_id, edit);
        }
      }
      break;
    }
    case compositor::NodeKind::RadialArray: {
      const auto *parameters = std::get_if<compositor::RadialArrayParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }

      const gui::Rect count_field = next_field("COUNT");
      double authored_count = 0.0;
      NumberFieldEdit count_edit{};
      const gui::UiId count_id = field_id(CompositorProperty::RadialCount);
      if (parameter_fields_.draw(ctx, count_id, count_field, {},
                                 static_cast<double>(parameters->count), 1.0,
                                 authored_count, 0, &count_edit) &&
          std::isfinite(authored_count)) {
        const double rounded = std::round(authored_count);
        const core::u32 value = static_cast<core::u32>(
            std::clamp(rounded, 1.0,
                       static_cast<double>(compositor::max_radial_instances)));
        if (value != parameters->count) {
          frame.property_edit = CompositorPropertyEditIntent{
              parameter_node->node, CompositorProperty::RadialCount,
              CompositorPropertyValue{value}};
          record_nudge(CompositorProperty::RadialCount, count_id, count_edit);
        }
      }

      scalar_field("RADIUS", CompositorProperty::RadialRadius,
                   parameters->radius, 0.05, 0.0F,
                   compositor::max_spatial_extent, 2);
      scalar_field("START", CompositorProperty::RadialStartAngleTurns,
                   parameters->start_angle, 0.001,
                   -compositor::max_spatial_extent,
                   compositor::max_spatial_extent, 3);

      const gui::Rect center_row = next_field("CENTER");
      const float gap = 4.0F;
      const float available = std::max(center_row.width - gap, 0.0F);
      const float field_width = available * 0.5F;
      const gui::Rect center_x_field{center_row.x, center_row.y, field_width,
                                     center_row.height};
      const gui::Rect center_y_field{center_x_field.x + field_width + gap,
                                     center_row.y, field_width,
                                     center_row.height};
      double authored_x = 0.0;
      NumberFieldEdit center_x_edit{};
      const gui::UiId center_x_id = field_id(CompositorProperty::RadialCenterX);
      if (parameter_fields_.draw(ctx, center_x_id, center_x_field, "X",
                                 static_cast<double>(parameters->center.x),
                                 0.05, authored_x, 2, &center_x_edit)) {
        emit_float(CompositorProperty::RadialCenterX, authored_x,
                   parameters->center.x, -compositor::max_spatial_extent,
                   compositor::max_spatial_extent);
        record_nudge(CompositorProperty::RadialCenterX, center_x_id,
                     center_x_edit);
      }
      double authored_y = 0.0;
      NumberFieldEdit center_y_edit{};
      const gui::UiId center_y_id = field_id(CompositorProperty::RadialCenterY);
      if (parameter_fields_.draw(ctx, center_y_id, center_y_field, "Y",
                                 static_cast<double>(parameters->center.y),
                                 0.05, authored_y, 2, &center_y_edit)) {
        emit_float(CompositorProperty::RadialCenterY, authored_y,
                   parameters->center.y, -compositor::max_spatial_extent,
                   compositor::max_spatial_extent);
        record_nudge(CompositorProperty::RadialCenterY, center_y_id,
                     center_y_edit);
      }
      break;
    }
    case compositor::NodeKind::LedCarProcess: {
      const auto *parameters = std::get_if<compositor::LedCarProcessParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      if (section("POSITION", led_car_position_section)) {
        scalar_field("X", CompositorProperty::LedCarPositionX,
                     parameters->position.x, 0.05,
                     -compositor::max_spatial_extent,
                     compositor::max_spatial_extent, 2);
        scalar_field("Y", CompositorProperty::LedCarPositionY,
                     parameters->position.y, 0.05,
                     -compositor::max_spatial_extent,
                     compositor::max_spatial_extent, 2);
        scalar_field("Z", CompositorProperty::LedCarPositionZ,
                     parameters->position.z, 0.05,
                     -compositor::max_spatial_extent,
                     compositor::max_spatial_extent, 2);
      }
      if (section("ROTATION (TURNS)", led_car_rotation_section)) {
        scalar_field("YAW", CompositorProperty::LedCarYawTurns,
                     parameters->yaw, 0.001, -compositor::max_spatial_extent,
                     compositor::max_spatial_extent, 3);
        scalar_field("PITCH", CompositorProperty::LedCarPitchTurns,
                     parameters->pitch, 0.001, -compositor::max_spatial_extent,
                     compositor::max_spatial_extent, 3);
        scalar_field("ROLL", CompositorProperty::LedCarRollTurns,
                     parameters->roll, 0.001, -compositor::max_spatial_extent,
                     compositor::max_spatial_extent, 3);
      }
      if (section("PLATES", led_car_plates_section)) {
        scalar_field("PLATE SCALE", CompositorProperty::LedCarPlateScale,
                     parameters->plate_scale, 0.05, 0.0F,
                     compositor::max_spatial_extent, 2);
        scalar_field("ARRAY SCALE", CompositorProperty::LedCarArrayScale,
                     parameters->array_scale, 0.05, 0.0F,
                     compositor::max_spatial_extent, 2);
        scalar_field("HORIZONTAL BLEND",
                     CompositorProperty::LedCarHorizontalBlend,
                     parameters->horizontal_blend, 0.005, 0.0F,
                     compositor::max_led_car_horizontal_blend, 3);
      }
      if (section("CEILING REFLECTION", led_car_reflection_section)) {
        scalar_field("HEIGHT", CompositorProperty::LedCarReflectionHeight,
                     parameters->reflection_height, 0.05,
                     -compositor::max_spatial_extent,
                     compositor::max_spatial_extent, 2);
        scalar_field("SCALE", CompositorProperty::LedCarReflectionScale,
                     parameters->reflection_scale, 0.05, 0.0F,
                     compositor::max_spatial_extent, 2);
      }
      break;
    }
    case compositor::NodeKind::Merge: {
      const auto *parameters = std::get_if<compositor::MergeParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      choice_field("BLEND", CompositorProperty::MergeBlendMode,
                   merge_blend_labels,
                   static_cast<std::size_t>(parameters->blend));
      scalar_field("OPACITY", CompositorProperty::MergeOpacity,
                   parameters->opacity, 0.005, 0.0F, 1.0F, 3);
      scalar_field("VALUE", CompositorProperty::MergeValue, parameters->value,
                   0.01, 0.0F, compositor::max_merge_value, 3);
      // Say when VALUE does nothing, rather than let a scrub look broken.
      const bool layers_wired = std::any_of(
          compositor_graph.edges.begin(), compositor_graph.edges.end(),
          [node = parameter_node->node](const compositor::GraphEdge &edge) {
            return edge.to == compositor::EdgeEndpoint{node, 1U};
          });
      const gui::Rect note = full_row();
      ctx.label(note,
                layers_wired ? "VALUE UNUSED: LAYERS IS WIRED"
                : parameters->blend == compositor::MergeBlendMode::Normal
                    ? "VALUE UNUSED WITH NORMAL"
                    : "BASE IS BLENDED WITH VALUE",
                ctx.theme().text_muted);
      break;
    }
    case compositor::NodeKind::Background: {
      const auto *parameters = std::get_if<compositor::BackgroundParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }
      using Property = CompositorProperty;
      choice_field("TYPE", Property::BackgroundMode, background_mode_labels,
                   static_cast<std::size_t>(parameters->mode));
      // A colour as a swatch above its four channels.
      const auto colour_fields =
          [&](std::string_view title, const compositor::BackgroundColor &colour,
              const std::array<Property, 4U> &properties) {
            colour_well_field(ctx, next_field(title), parameter_node->node,
                              properties[0U] == Property::BackgroundColorAR ? 1U : 2U,
                              gui::UiColor{colour.r, colour.g, colour.b, 1.0F},
                              {properties[0U], properties[1U], properties[2U]},
                              frame);
            scalar_field("RED", properties[0U], colour.r, 0.005, 0.0F, 1.0F,
                         3);
            scalar_field("GREEN", properties[1U], colour.g, 0.005, 0.0F, 1.0F,
                         3);
            scalar_field("BLUE", properties[2U], colour.b, 0.005, 0.0F, 1.0F,
                         3);
            scalar_field("ALPHA", properties[3U], colour.a, 0.005, 0.0F, 1.0F,
                         3);
          };
      colour_fields("COLOUR A", parameters->color_a,
                    {Property::BackgroundColorAR, Property::BackgroundColorAG,
                     Property::BackgroundColorAB,
                     Property::BackgroundColorAA});
      if (parameters->mode == compositor::BackgroundMode::Solid) {
        break;
      }
      colour_fields("COLOUR B", parameters->color_b,
                    {Property::BackgroundColorBR, Property::BackgroundColorBG,
                     Property::BackgroundColorBB,
                     Property::BackgroundColorBA});
      if (parameters->mode == compositor::BackgroundMode::Linear) {
        const gui::Rect field = next_field("ANGLE TURNS");
        double authored{};
        NumberFieldEdit edit{};
        const gui::UiId numeric_id = field_id(Property::BackgroundAngleTurns);
        if (parameter_fields_.draw(ctx, numeric_id, field, {},
                                   static_cast<double>(parameters->angle),
                                   0.001, authored, 3, &edit) &&
            std::isfinite(authored)) {
          double wrapped = authored - std::floor(authored);
          if (wrapped < 0.0) {
            wrapped += 1.0;
          }
          const float value = static_cast<float>(wrapped);
          if (value != parameters->angle) {
            frame.property_edit = CompositorPropertyEditIntent{
                parameter_node->node, Property::BackgroundAngleTurns,
                CompositorPropertyValue{value}};
            record_nudge(Property::BackgroundAngleTurns, numeric_id, edit);
          }
        }
      }
      scalar_field("CENTER X", Property::BackgroundCenterX,
                   parameters->center_x, 0.005,
                   -compositor::max_background_center,
                   compositor::max_background_center, 3);
      scalar_field("CENTER Y", Property::BackgroundCenterY,
                   parameters->center_y, 0.005,
                   -compositor::max_background_center,
                   compositor::max_background_center, 3);
      scalar_field("SIZE", Property::BackgroundSize, parameters->size, 0.01,
                   0.01F, compositor::max_background_gradient_size, 3);
      break;
    }
    case compositor::NodeKind::SceneLayer:
    case compositor::NodeKind::Composite:
    case compositor::NodeKind::Output:
      break;
    }
    ctx.pop_clip();
    parameter_content_height_ = rows.extent();

    // A draggable thumb shows where the visible rows sit in the whole list.
    if (scrollable && parameter_body.height > 0.0F) {
      gui::Rect track =
          gui::inset(parameter_body, gui::Padding{0.0F, 2.0F, 2.0F, 2.0F});
      track = gui::cut_right(track, scrollbar_width);
      const float thumb_height = std::max(
          24.0F, track.height * parameter_body.height /
                     std::max(parameter_content_height_, 1.0F));
      const float travel = std::max(track.height - thumb_height, 0.0F);
      const gui::Rect thumb{
          track.x,
          track.y + (maximum_scroll > 0.0F
                         ? travel * parameter_scroll_ / maximum_scroll
                         : 0.0F),
          track.width, std::min(thumb_height, track.height)};
      const gui::Interaction drag =
          ctx.grab(gui::ui_id(id_, "compositor.parameters.scroll"), thumb);
      if (drag.pressed) {
        parameter_thumb_grab_ = ctx.pointer_y() - thumb.y;
      }
      if (drag.held && travel > 0.0F) {
        parameter_scroll_ = std::clamp(
            (ctx.pointer_y() - parameter_thumb_grab_ - track.y) / travel *
                maximum_scroll,
            0.0F, maximum_scroll);
      }
      ctx.panel(track, with_alpha(ctx.theme().track, 0.8F));
      ctx.panel(thumb, drag.held ? ctx.theme().widget_active
                                 : (drag.hovered ? ctx.theme().widget_hover
                                                 : ctx.theme().widget));
    }
    ctx.pop_clip();
  }
}

} // namespace mediaserver::editor
