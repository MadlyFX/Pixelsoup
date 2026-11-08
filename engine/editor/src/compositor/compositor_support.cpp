#include "compositor_support.hpp"

#include <mediaserver/gui/shapes.hpp>

namespace mediaserver::editor::compositor_ui {

[[nodiscard]] gui::UiColor port_color(const gui::UiContext &,
                                      compositor::PortValueType type) noexcept {
  switch (type) {
  case compositor::PortValueType::Image:
    return gui::UiColor{0.380F, 0.741F, 0.949F, 1.0F};
  case compositor::PortValueType::Effect:
    return gui::UiColor{0.722F, 0.580F, 0.976F, 1.0F};
  case compositor::PortValueType::TransformSet:
    return gui::UiColor{0.412F, 0.808F, 0.698F, 1.0F};
  case compositor::PortValueType::Filter:
    return gui::UiColor{0.471F, 0.851F, 0.384F, 1.0F};
  }
  return gui::UiColor{0.380F, 0.741F, 0.949F, 1.0F};
}

[[nodiscard]] gui::UiColor comment_color(core::u8 index) noexcept {
  constexpr std::array<gui::UiColor, compositor::comment_color_count> palette{{
      {0.55F, 0.60F, 0.68F, 1.0F},
      {0.36F, 0.58F, 0.92F, 1.0F},
      {0.32F, 0.72F, 0.70F, 1.0F},
      {0.45F, 0.74F, 0.42F, 1.0F},
      {0.66F, 0.52F, 0.90F, 1.0F},
      {0.88F, 0.47F, 0.60F, 1.0F},
  }};
  return palette[index < palette.size() ? index : 0U];
}

[[nodiscard]] gui::UiColor blend_color(gui::UiColor from, gui::UiColor to,
                                       float amount) noexcept {
  return gui::UiColor{from.r + (to.r - from.r) * amount,
                      from.g + (to.g - from.g) * amount,
                      from.b + (to.b - from.b) * amount,
                      from.a + (to.a - from.a) * amount};
}

void draw_port_shape(gui::UiContext &ctx, compositor::PortValueType type,
                     float x, float y, float radius, gui::UiColor color) {
  switch (type) {
  case compositor::PortValueType::Image:
    gui::add_disc(ctx.draw_list(), gui::UiPoint{x, y},
                  radius, color);
    return;
  case compositor::PortValueType::Effect: {
    const float reach = radius * 1.25F;
    const std::array<gui::UiPoint, 4U> diamond{{{x, y - reach},
                                                {x + reach, y},
                                                {x, y + reach},
                                                {x - reach, y}}};
    ctx.draw_list().add_convex_poly(diamond, color);
    return;
  }
  case compositor::PortValueType::TransformSet: {
    const float half = radius * 0.9F;
    const std::array<gui::UiPoint, 4U> square{{{x - half, y - half},
                                               {x + half, y - half},
                                               {x + half, y + half},
                                               {x - half, y + half}}};
    ctx.draw_list().add_convex_poly(square, color);
    return;
  }
  case compositor::PortValueType::Filter: {
    // Pointing along the wire, the way the image flows through it.
    const float reach = radius * 1.3F;
    const std::array<gui::UiPoint, 3U> triangle{{{x - reach * 0.8F, y - reach},
                                                 {x + reach, y},
                                                 {x - reach * 0.8F, y + reach}}};
    ctx.draw_list().add_convex_poly(triangle, color);
    return;
  }
  }
}

void draw_port(gui::UiContext &ctx, compositor::PortValueType type, float x,
               float y, float radius, gui::UiColor color, bool filled,
               gui::UiColor hollow) {
  draw_port_shape(ctx, type, x, y, radius, color);
  if (!filled) {
    draw_port_shape(ctx, type, x, y,
                    std::max(radius - std::max(1.5F, radius * 0.4F), 0.5F),
                    hollow);
  }
}

[[nodiscard]] gui::UiColor
compositor_category_color(const gui::UiContext &ctx,
                          CompositorNodeCategory category) noexcept {
  switch (category) {
  case CompositorNodeCategory::Input:
    return port_color(ctx, compositor::PortValueType::Image);
  case CompositorNodeCategory::Color:
    return gui::UiColor{0.890F, 0.502F, 0.706F, 1.0F};
  case CompositorNodeCategory::Keying:
    return gui::UiColor{0.471F, 0.808F, 0.471F, 1.0F};
  case CompositorNodeCategory::Blur:
    return gui::UiColor{0.553F, 0.635F, 0.976F, 1.0F};
  case CompositorNodeCategory::Texture:
    return gui::UiColor{0.722F, 0.671F, 0.600F, 1.0F};
  case CompositorNodeCategory::Spatial:
    return port_color(ctx, compositor::PortValueType::TransformSet);
  case CompositorNodeCategory::Combine:
  case CompositorNodeCategory::Count:
    break;
  }
  return ctx.theme().text;
}

[[nodiscard]] gui::UiColor
compositor_node_color(const gui::UiContext &ctx,
                      compositor::NodeKind kind) noexcept {
  const auto found =
      std::find_if(compositor_palette_items.begin(),
                   compositor_palette_items.end(),
                   [kind](const CompositorPaletteItem &item) noexcept {
                     return item.kind == kind;
                   });
  return compositor_category_color(ctx, found == compositor_palette_items.end()
                                            ? CompositorNodeCategory::Combine
                                            : found->category);
}

[[nodiscard]] float compositor_text_width(const gui::UiContext &ctx,
                                          std::string_view text, float size) {
  const float nominal = ctx.font_width(text) * size;
  const std::optional<float> baked = ctx.text_size_at_most(size);
  return baked.has_value() && *baked == size
             ? std::max(nominal, ctx.font_width(text, size))
             : nominal;
}

[[nodiscard]] float compositor_text_height(const gui::UiContext &ctx,
                                           float size) {
  const std::optional<float> baked = ctx.text_size_at_most(size);
  return std::ceil(baked.has_value() && *baked == size
                       ? std::max(ctx.line_height() * size,
                                  ctx.line_height(size))
                       : ctx.line_height() * size);
}

bool compositor_label(gui::UiContext &ctx, const gui::Rect &bounds,
                      std::string_view text, gui::UiColor color, float size,
                      float zoom) {
  const std::optional<float> baked = ctx.text_size_at_most(size * zoom);
  if (!baked.has_value() || bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return false;
  }
  ctx.label(bounds, text, color, *baked);
  return true;
}

[[nodiscard]] CompositorNodeMetrics
compositor_node_metrics(const gui::UiContext &ctx) {
  const float title = compositor_text_height(ctx, compositor_title_text);
  const float detail = compositor_text_height(ctx, compositor_detail_text);
  const float pin = compositor_text_height(ctx, compositor_pin_text);
  return CompositorNodeMetrics{5.0F + title + detail + 3.0F,
                               std::max(22.0F, pin + 8.0F), 7.0F};
}

[[nodiscard]] std::span<const InlineValue>
inline_values(compositor::NodeKind kind) noexcept {
  using P = CompositorProperty;
  static constexpr std::array<InlineValue, 1U> brightness{
      {{"AMOUNT", P::BrightnessAmount, 0.01, 0.0, compositor::max_brightness,
        2}}};
  static constexpr std::array<InlineValue, 1U> saturation{
      {{"AMOUNT", P::SaturationAmount, 0.01, 0.0, compositor::max_saturation,
        2}}};
  static constexpr std::array<InlineValue, 1U> blur{
      {{"RADIUS", P::BlurRadius, 0.10, 0.0, compositor::max_blur_radius, 1}}};
  static constexpr std::array<InlineValue, 2U> box_blur{
      {{"RADIUS PX", P::BoxBlurRadius, 1.0, 0.0,
        compositor::max_box_blur_radius, 0, true},
       {"PASSES", P::BoxBlurPasses, 1.0, 1.0, compositor::max_box_blur_passes,
        0, true}}};
  static constexpr std::array<InlineValue, 1U> directional_blur{
      {{"LENGTH PX", P::DirectionalBlurLength, 0.10, 0.0,
        compositor::max_directional_blur_length, 2}}};
  static constexpr std::array<InlineValue, 2U> contrast_pivot{
      {{"CONTRAST", P::ContrastPivotContrast, 0.01,
        scene::min_contrast_pivot_contrast, scene::max_contrast_pivot_contrast,
        3},
       {"PIVOT", P::ContrastPivotPivot, 0.005, scene::min_contrast_pivot_pivot,
        scene::max_contrast_pivot_pivot, 3}}};
  static constexpr std::array<InlineValue, 2U> white_balance{
      {{"KELVIN", P::WhiteBalanceTemperatureKelvin, 25.0,
        scene::min_white_balance_temperature_kelvin,
        scene::max_white_balance_temperature_kelvin, 0},
       {"TINT", P::WhiteBalanceTint, 0.01, scene::min_white_balance_tint,
        scene::max_white_balance_tint, 3}}};
  static constexpr std::array<InlineValue, 1U> film_grain{
      {{"AMOUNT", P::FilmGrainAmount, 0.005, scene::min_film_grain_amount,
        scene::max_film_grain_amount, 3}}};
  static constexpr std::array<InlineValue, 2U> radial_array{
      {{"COUNT", P::RadialCount, 1.0, 1.0, compositor::max_radial_instances, 0,
        true},
       {"RADIUS", P::RadialRadius, 0.05, 0.0, compositor::max_spatial_extent,
        2}}};
  // VALUE is what LAYERS presents while nothing is wired to it.
  static constexpr std::array<InlineValue, 2U> merge{
      {{"OPACITY", P::MergeOpacity, 0.005, 0.0, 1.0, 2},
       {"VALUE", P::MergeValue, 0.01, 0.0, compositor::max_merge_value, 2,
        false, 1}}};
  switch (kind) {
  case compositor::NodeKind::Brightness:
    return brightness;
  case compositor::NodeKind::Saturation:
    return saturation;
  case compositor::NodeKind::Blur:
    return blur;
  case compositor::NodeKind::BoxBlur:
    return box_blur;
  case compositor::NodeKind::DirectionalBlur:
    return directional_blur;
  case compositor::NodeKind::ContrastPivot:
    return contrast_pivot;
  case compositor::NodeKind::WhiteBalance:
    return white_balance;
  case compositor::NodeKind::FilmGrain:
    return film_grain;
  case compositor::NodeKind::RadialArray:
    return radial_array;
  case compositor::NodeKind::Merge:
    return merge;
  default:
    return {};
  }
}

[[nodiscard]] bool inline_value_idle(const compositor::CompositorGraph &graph,
                                     const compositor::GraphNode &node,
                                     const InlineValue &value) noexcept {
  if (value.unwired_input >= 0) {
    const compositor::EdgeEndpoint input{
        node.node, static_cast<core::u8>(value.unwired_input)};
    if (std::any_of(graph.edges.begin(), graph.edges.end(),
                    [input](const compositor::GraphEdge &edge) noexcept {
                      return edge.to == input;
                    })) {
      return true;
    }
  }
  const auto *merge = std::get_if<compositor::MergeParameters>(&node.parameters);
  return value.property == CompositorProperty::MergeValue &&
         merge != nullptr &&
         merge->blend == compositor::MergeBlendMode::Normal;
}

[[nodiscard]] double inline_value_of(const compositor::GraphNode &node,
                                     CompositorProperty property) noexcept {
  using P = CompositorProperty;
  const auto &parameters = node.parameters;
  switch (property) {
  case P::BrightnessAmount:
    if (const auto *value = std::get_if<compositor::BrightnessParameters>(&parameters))
      return value->amount;
    break;
  case P::SaturationAmount:
    if (const auto *value = std::get_if<compositor::SaturationParameters>(&parameters))
      return value->amount;
    break;
  case P::BlurRadius:
    if (const auto *value = std::get_if<compositor::BlurParameters>(&parameters))
      return value->radius;
    break;
  case P::BoxBlurRadius:
    if (const auto *value = std::get_if<compositor::BoxBlurParameters>(&parameters))
      return value->radius_px;
    break;
  case P::BoxBlurPasses:
    if (const auto *value = std::get_if<compositor::BoxBlurParameters>(&parameters))
      return value->passes;
    break;
  case P::DirectionalBlurLength:
    if (const auto *value =
            std::get_if<compositor::DirectionalBlurParameters>(&parameters))
      return value->length_px;
    break;
  case P::ContrastPivotContrast:
    if (const auto *value =
            std::get_if<compositor::ContrastPivotParameters>(&parameters))
      return value->contrast;
    break;
  case P::ContrastPivotPivot:
    if (const auto *value =
            std::get_if<compositor::ContrastPivotParameters>(&parameters))
      return value->pivot;
    break;
  case P::WhiteBalanceTemperatureKelvin:
    if (const auto *value =
            std::get_if<compositor::WhiteBalanceParameters>(&parameters))
      return value->temperature_kelvin;
    break;
  case P::WhiteBalanceTint:
    if (const auto *value =
            std::get_if<compositor::WhiteBalanceParameters>(&parameters))
      return value->tint;
    break;
  case P::FilmGrainAmount:
    if (const auto *value = std::get_if<compositor::FilmGrainParameters>(&parameters))
      return value->amount;
    break;
  case P::RadialCount:
    if (const auto *value =
            std::get_if<compositor::RadialArrayParameters>(&parameters))
      return value->count;
    break;
  case P::RadialRadius:
    if (const auto *value =
            std::get_if<compositor::RadialArrayParameters>(&parameters))
      return value->radius;
    break;
  case P::MergeOpacity:
    if (const auto *value = std::get_if<compositor::MergeParameters>(&parameters))
      return value->opacity;
    break;
  case P::MergeValue:
    if (const auto *value = std::get_if<compositor::MergeParameters>(&parameters))
      return value->value;
    break;
  default:
    break;
  }
  return 0.0;
}

[[nodiscard]] float compositor_node_height(const CompositorNodeMetrics &metrics,
                                           compositor::NodeKind kind) noexcept {
  const std::size_t rows =
      std::max({compositor::input_ports(kind).size(),
                compositor::output_ports(kind).size(), std::size_t{1U}}) +
      inline_values(kind).size();
  return metrics.header_height +
         static_cast<float>(rows) * metrics.row_height + metrics.footer_height;
}

[[nodiscard]] float compositor_node_width(const gui::UiContext &ctx,
                                          compositor::NodeKind kind,
                                          std::string_view title,
                                          std::string_view source) {
  float widest_input{};
  for (const compositor::PortDescriptor &port : compositor::input_ports(kind)) {
    widest_input = std::max(
        widest_input, compositor_text_width(ctx, port.name, compositor_pin_text));
  }
  float widest_output{};
  for (const compositor::PortDescriptor &port :
       compositor::output_ports(kind)) {
    widest_output =
        std::max(widest_output,
                 compositor_text_width(ctx, port.name, compositor_pin_text));
  }
  const float pins =
      (widest_input > 0.0F ? compositor_pin_inset + widest_input
                           : compositor_node_padding) +
      compositor_pin_label_gap +
      (widest_output > 0.0F ? widest_output + compositor_pin_inset
                            : compositor_node_padding);
  // The title shares its line with the collapse toggle and a problem badge.
  float heading =
      std::max(compositor_text_width(ctx, title, compositor_title_text) +
                   compositor_header_chrome,
               compositor_text_width(ctx, source, compositor_detail_text)) +
      compositor_node_padding * 2.0F;
  // An inline row is its label then a field wide enough to scrub.
  for (const InlineValue &value : inline_values(kind)) {
    heading = std::max(heading,
                       compositor_text_width(ctx, value.label, compositor_pin_text) +
                           compositor_node_padding * 2.0F + 96.0F);
  }
  const float content = std::ceil(std::max(pins, heading) / 8.0F) * 8.0F;
  return std::clamp(content, compositor_node_min_width,
                    compositor_node_max_width);
}

[[nodiscard]] std::string compositor_node_detail(
    const compositor::GraphNode &node,
    std::span<const ElementInfo> elements) {
  std::array<char, 64U> text{};
  int length = 0;
  switch (node.kind) {
  case compositor::NodeKind::SceneLayer: {
    const auto *parameters =
        std::get_if<compositor::SceneLayerParameters>(&node.parameters);
    return compositor_scene_layer_detail(
        parameters != nullptr ? parameters->scene_node : doc::NodeId{},
        elements);
  }
  case compositor::NodeKind::Brightness: {
    const auto *parameters =
        std::get_if<compositor::BrightnessParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "AMOUNT %.2f",
        static_cast<double>(parameters != nullptr ? parameters->amount : 1.0F));
    break;
  }
  case compositor::NodeKind::Saturation: {
    const auto *parameters =
        std::get_if<compositor::SaturationParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "AMOUNT %.2f",
        static_cast<double>(parameters != nullptr ? parameters->amount : 1.0F));
    break;
  }
  case compositor::NodeKind::Levels: {
    const auto *parameters =
        std::get_if<compositor::LevelsParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "MASTER %.2f / %.2f / %.2f",
        static_cast<double>(
            parameters != nullptr ? parameters->master.input_black : 0.0F),
        static_cast<double>(parameters != nullptr ? parameters->master.gamma
                                                  : 1.0F),
        static_cast<double>(
            parameters != nullptr ? parameters->master.input_white : 1.0F));
    break;
  }
  case compositor::NodeKind::LiftGammaGain: {
    const auto *parameters =
        std::get_if<compositor::LiftGammaGainParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "L %.2f  G %.2f  GAIN %.2f",
        static_cast<double>(parameters != nullptr ? parameters->lift.x : 0.0F),
        static_cast<double>(parameters != nullptr ? parameters->gamma.x : 1.0F),
        static_cast<double>(parameters != nullptr ? parameters->gain.x : 1.0F));
    break;
  }
  case compositor::NodeKind::ContrastPivot: {
    const auto *parameters =
        std::get_if<compositor::ContrastPivotParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "CONTRAST %.2f  PIVOT %.2f",
        static_cast<double>(parameters != nullptr ? parameters->contrast
                                                  : 1.0F),
        static_cast<double>(parameters != nullptr ? parameters->pivot : 0.5F));
    break;
  }
  case compositor::NodeKind::ColorMatrix:
    return "3x3 RGB MATRIX + OFFSET";
  case compositor::NodeKind::WhiteBalance: {
    const auto *parameters =
        std::get_if<compositor::WhiteBalanceParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "%.0f K  TINT %.2f",
        static_cast<double>(parameters != nullptr
                                ? parameters->temperature_kelvin
                                : scene::d65_white_balance_temperature_kelvin),
        static_cast<double>(parameters != nullptr ? parameters->tint : 0.0F));
    break;
  }
  case compositor::NodeKind::Curves: {
    const auto *parameters =
        std::get_if<compositor::CurvesParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "MASTER %llu POINTS  RGB %u",
        static_cast<unsigned long long>(
            parameters != nullptr ? parameters->master.points.size() : 2U),
        static_cast<unsigned>(parameters != nullptr &&
                              (parameters->red.has_value() ||
                               parameters->green.has_value() ||
                               parameters->blue.has_value())));
    break;
  }
  case compositor::NodeKind::FilmGrain: {
    const auto *parameters =
        std::get_if<compositor::FilmGrainParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "AMOUNT %.2f  %s",
        static_cast<double>(parameters != nullptr ? parameters->amount : 0.0F),
        parameters != nullptr && parameters->mode == scene::FilmGrainMode::RGB
            ? "RGB"
            : "MONO");
    break;
  }
  case compositor::NodeKind::Blur: {
    const auto *parameters =
        std::get_if<compositor::BlurParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "RADIUS %.1f",
        static_cast<double>(parameters != nullptr ? parameters->radius : 0.0F));
    break;
  }
  case compositor::NodeKind::BoxBlur: {
    const auto *parameters =
        std::get_if<compositor::BoxBlurParameters>(&node.parameters);
    length = std::snprintf(text.data(), text.size(), "RADIUS %u  PASSES %u",
                           parameters != nullptr ? parameters->radius_px : 0U,
                           parameters != nullptr ? parameters->passes : 1U);
    break;
  }
  case compositor::NodeKind::DirectionalBlur: {
    const auto *parameters =
        std::get_if<compositor::DirectionalBlurParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "LENGTH %.1f  ANGLE %.3f",
        static_cast<double>(parameters != nullptr ? parameters->length_px
                                                  : 0.0F),
        static_cast<double>(parameters != nullptr ? parameters->angle_turns
                                                  : 0.0F));
    break;
  }
  case compositor::NodeKind::ChromaKey: {
    const auto *parameters =
        std::get_if<compositor::ChromaKeyParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "TOLERANCE %.2f  MIX %.2f",
        static_cast<double>(parameters != nullptr ? parameters->tolerance
                                                  : 0.1F),
        static_cast<double>(parameters != nullptr ? parameters->mix : 1.0F));
    break;
  }
  case compositor::NodeKind::LumaKey: {
    const auto *parameters =
        std::get_if<compositor::LumaKeyParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "%.2f - %.2f%s",
        static_cast<double>(parameters != nullptr ? parameters->low_threshold
                                                  : 0.0F),
        static_cast<double>(parameters != nullptr ? parameters->high_threshold
                                                  : 1.0F),
        parameters != nullptr && parameters->invert ? "  INVERTED" : "");
    break;
  }
  case compositor::NodeKind::Matte: {
    const auto *parameters =
        std::get_if<compositor::MatteParameters>(&node.parameters);
    if (parameters == nullptr) {
      return "ALPHA MATTE";
    }
    return parameters->mode == compositor::MatteMode::Luma ? "LUMA MATTE"
                                                           : "ALPHA MATTE";
  }
  case compositor::NodeKind::RadialArray: {
    const auto *parameters =
        std::get_if<compositor::RadialArrayParameters>(&node.parameters);
    length = std::snprintf(text.data(), text.size(), "%u INSTANCES",
                           parameters != nullptr ? parameters->count : 0U);
    break;
  }
  case compositor::NodeKind::LedCarProcess: {
    const auto *parameters =
        std::get_if<compositor::LedCarProcessParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "8 WALL + CEILING  ARRAY %.2f",
        static_cast<double>(parameters != nullptr ? parameters->array_scale
                                                  : 4.0F));
    break;
  }
  case compositor::NodeKind::Composite:
    return "COMBINES IMAGE LAYERS";
  case compositor::NodeKind::Output:
    return "PROGRAM IMAGE";
  case compositor::NodeKind::Merge: {
    const auto *parameters =
        std::get_if<compositor::MergeParameters>(&node.parameters);
    if (parameters == nullptr) {
      return "NORMAL";
    }
    const std::string_view blend =
        merge_blend_labels[static_cast<std::size_t>(parameters->blend) %
                           merge_blend_labels.size()];
    length = std::snprintf(
        text.data(), text.size(), "%.*s  %d%%", static_cast<int>(blend.size()),
        blend.data(),
        static_cast<int>(std::lround(parameters->opacity * 100.0F)));
    break;
  }
  case compositor::NodeKind::Background: {
    const auto *parameters =
        std::get_if<compositor::BackgroundParameters>(&node.parameters);
    return std::string{background_mode_labels
                           [parameters != nullptr
                                ? static_cast<std::size_t>(parameters->mode) %
                                      background_mode_labels.size()
                                : 0U]};
  }
  }
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] bool has_editable_parameters(compositor::NodeKind kind) noexcept {
  switch (kind) {
  case compositor::NodeKind::Brightness:
  case compositor::NodeKind::Saturation:
  case compositor::NodeKind::Levels:
  case compositor::NodeKind::LiftGammaGain:
  case compositor::NodeKind::ContrastPivot:
  case compositor::NodeKind::ColorMatrix:
  case compositor::NodeKind::WhiteBalance:
  case compositor::NodeKind::Curves:
  case compositor::NodeKind::FilmGrain:
  case compositor::NodeKind::Blur:
  case compositor::NodeKind::BoxBlur:
  case compositor::NodeKind::DirectionalBlur:
  case compositor::NodeKind::RadialArray:
  case compositor::NodeKind::LedCarProcess:
  case compositor::NodeKind::ChromaKey:
  case compositor::NodeKind::LumaKey:
  case compositor::NodeKind::Matte:
  case compositor::NodeKind::Merge:
  case compositor::NodeKind::Background:
    return true;
  case compositor::NodeKind::SceneLayer:
  case compositor::NodeKind::Composite:
  case compositor::NodeKind::Output:
    return false;
  }
  return false;
}

[[nodiscard]] std::span<const std::string_view>
compositor_parameter_labels(compositor::NodeKind kind) noexcept {
  static constexpr std::array<std::string_view, 1U> amount{{"AMOUNT"}};
  static constexpr std::array<std::string_view, 5U> levels{
      {"IN BLACK", "IN WHITE", "GAMMA", "OUT BLACK", "OUT WHITE"}};
  static constexpr std::array<std::string_view, 9U> lift_gamma_gain{
      {"LIFT R", "LIFT G", "LIFT B", "GAMMA R", "GAMMA G", "GAMMA B", "GAIN R",
       "GAIN G", "GAIN B"}};
  static constexpr std::array<std::string_view, 2U> contrast_pivot{
      {"CONTRAST", "PIVOT"}};
  static constexpr std::array<std::string_view, 12U> color_matrix{
      {"RED FROM R", "RED FROM G", "RED FROM B", "GREEN FROM R", "GREEN FROM G",
       "GREEN FROM B", "BLUE FROM R", "BLUE FROM G", "BLUE FROM B",
       "RED OFFSET", "GREEN OFFSET", "BLUE OFFSET"}};
  static constexpr std::array<std::string_view, 2U> white_balance{
      {"TEMPERATURE K", "TINT"}};
  static constexpr std::array<std::string_view, 2U> curves{
      {"LAST POINT X", "LAST POINT Y"}};
  static constexpr std::array<std::string_view, 5U> film_grain{
      {"AMOUNT", "PIXEL SCALE", "CHROMA", "SEED", "MODE"}};
  static constexpr std::array<std::string_view, 1U> blur{{"RADIUS"}};
  static constexpr std::array<std::string_view, 2U> box_blur{
      {"RADIUS PX", "PASSES"}};
  static constexpr std::array<std::string_view, 2U> directional_blur{
      {"LENGTH PX", "ANGLE TURNS"}};
  static constexpr std::array<std::string_view, 4U> radial_array{
      {"COUNT", "RADIUS", "START", "CENTER"}};
  static constexpr std::array<std::string_view, 11U> led_car{
      {"X", "Y", "Z", "YAW", "PITCH", "ROLL", "PLATE SCALE", "ARRAY SCALE",
       "HORIZONTAL BLEND", "HEIGHT", "SCALE"}};
  static constexpr std::array<std::string_view, 9U> chroma_key{
      {"KEY COLOUR", "KEY RED", "KEY GREEN", "KEY BLUE", "TOLERANCE",
       "SOFTNESS", "EDGE PX", "SPILL", "MIX"}};
  static constexpr std::array<std::string_view, 6U> luma_key{
      {"LOW", "HIGH", "LOW SOFTNESS", "HIGH SOFTNESS", "MATTE", "MIX"}};
  static constexpr std::array<std::string_view, 3U> matte{
      {"MODE", "MATTE", "CLAMP INPUT"}};
  static constexpr std::array<std::string_view, 3U> merge{
      {"BLEND", "OPACITY", "VALUE"}};
  static constexpr std::array<std::string_view, 11U> background{
      {"TYPE", "COLOUR A", "COLOUR B", "RED", "GREEN", "BLUE", "ALPHA",
       "ANGLE TURNS", "CENTER X", "CENTER Y", "SIZE"}};
  switch (kind) {
  case compositor::NodeKind::Brightness:
  case compositor::NodeKind::Saturation:
    return amount;
  case compositor::NodeKind::Levels:
    return levels;
  case compositor::NodeKind::LiftGammaGain:
    return lift_gamma_gain;
  case compositor::NodeKind::ContrastPivot:
    return contrast_pivot;
  case compositor::NodeKind::ColorMatrix:
    return color_matrix;
  case compositor::NodeKind::WhiteBalance:
    return white_balance;
  case compositor::NodeKind::Curves:
    return curves;
  case compositor::NodeKind::FilmGrain:
    return film_grain;
  case compositor::NodeKind::Blur:
    return blur;
  case compositor::NodeKind::BoxBlur:
    return box_blur;
  case compositor::NodeKind::DirectionalBlur:
    return directional_blur;
  case compositor::NodeKind::RadialArray:
    return radial_array;
  case compositor::NodeKind::LedCarProcess:
    return led_car;
  case compositor::NodeKind::ChromaKey:
    return chroma_key;
  case compositor::NodeKind::LumaKey:
    return luma_key;
  case compositor::NodeKind::Matte:
    return matte;
  case compositor::NodeKind::Merge:
    return merge;
  case compositor::NodeKind::Background:
    return background;
  case compositor::NodeKind::SceneLayer:
  case compositor::NodeKind::Composite:
  case compositor::NodeKind::Output:
    break;
  }
  return {};
}

[[nodiscard]] float
parameter_panel_height(const compositor::GraphNode &node) noexcept {
  constexpr float header = 32.0F;
  constexpr float footer = 8.0F;
  constexpr float row = 26.0F;
  switch (node.kind) {
  case compositor::NodeKind::RadialArray:
    return header + footer + 4.0F * row;
  case compositor::NodeKind::LedCarProcess:
    // Eleven fields under four section headings.
    return header + footer + 15.0F * row;
  case compositor::NodeKind::Levels:
    // Five fields for each of four channel sections.
    return header + footer + 24.0F * row;
  case compositor::NodeKind::LiftGammaGain:
    return header + footer + 9.0F * row;
  case compositor::NodeKind::ContrastPivot:
    return header + footer + 2.0F * row;
  case compositor::NodeKind::ColorMatrix:
    return header + footer + 12.0F * row;
  case compositor::NodeKind::WhiteBalance:
    return header + footer + 2.0F * row;
  case compositor::NodeKind::Curves: {
    const auto *parameters =
        std::get_if<compositor::CurvesParameters>(&node.parameters);
    const bool has_interior =
        parameters != nullptr && parameters->master.points.size() > 2U;
    return header + footer + (has_interior ? 4.0F : 2.0F) * row;
  }
  case compositor::NodeKind::FilmGrain:
    return header + footer + 5.0F * row;
  case compositor::NodeKind::ChromaKey:
    return header + footer + 9.0F * row;
  case compositor::NodeKind::LumaKey:
    return header + footer + 6.0F * row;
  case compositor::NodeKind::Matte:
    return header + footer + 3.0F * row;
  case compositor::NodeKind::Merge:
    // Three fields and the note saying when VALUE applies.
    return header + footer + 4.0F * row;
  case compositor::NodeKind::Background: {
    const auto *parameters =
        std::get_if<compositor::BackgroundParameters>(&node.parameters);
    const auto mode = parameters != nullptr ? parameters->mode
                                            : compositor::BackgroundMode::Solid;
    // Type and one colour; a gradient adds a colour, its placement, and for
    // Linear its angle.
    const float rows =
        mode == compositor::BackgroundMode::Solid
            ? 6.0F
            : (mode == compositor::BackgroundMode::Linear ? 15.0F : 14.0F);
    return header + footer + rows * row;
  }
  case compositor::NodeKind::BoxBlur:
  case compositor::NodeKind::DirectionalBlur:
    return header + footer + 2.0F * row;
  case compositor::NodeKind::Brightness:
  case compositor::NodeKind::Saturation:
  case compositor::NodeKind::Blur:
  case compositor::NodeKind::SceneLayer:
  case compositor::NodeKind::Composite:
  case compositor::NodeKind::Output:
    return header + footer + row;
  }
  return header + footer + row;
}

[[nodiscard]] bool is_problem(
    const compositor::GraphDiagnostic &diagnostic) noexcept {
  return diagnostic.code != compositor::DiagnosticCode::DeadBranch;
}

[[nodiscard]] bool node_has_diagnostic(const compositor::CompositorGraph &graph,
                                       doc::NodeId node) noexcept {
  return std::any_of(
      graph.diagnostics.begin(), graph.diagnostics.end(),
      [node](const compositor::GraphDiagnostic &diagnostic) noexcept {
        return is_problem(diagnostic) &&
               (diagnostic.subject == node || diagnostic.related == node);
      });
}

[[nodiscard]] bool node_parked(const compositor::CompositorGraph &graph,
                               doc::NodeId node) noexcept {
  return std::any_of(
      graph.diagnostics.begin(), graph.diagnostics.end(),
      [node](const compositor::GraphDiagnostic &diagnostic) noexcept {
        return !is_problem(diagnostic) && diagnostic.subject == node;
      });
}

[[nodiscard]] bool node_disabled(const compositor::CompositorGraph &graph,
                                 doc::NodeId node) noexcept {
  const compositor::GraphNodeMetadata *const metadata = graph.metadata(node);
  return metadata != nullptr && metadata->disabled;
}

[[nodiscard]] std::string disabled_reason(
    const compositor::CompositorGraph &graph,
    const compositor::GraphNode &node) {
  std::string waits;
  for (const compositor::PortDescriptor &port :
       compositor::input_ports(node.kind)) {
    if (!port.required) {
      continue;
    }
    bool wired = false;
    bool fed = false;
    for (const compositor::GraphEdge &edge : graph.edges) {
      if (edge.to.node == node.node && edge.to.port == port.index) {
        wired = true;
        fed = fed || !node_disabled(graph, edge.from.node);
      }
    }
    if (fed) {
      continue;
    }
    if (!waits.empty()) {
      waits += " and ";
    }
    waits += wired ? "what feeds " : "";
    waits += port.name;
    waits += wired ? " is complete" : " is connected";
  }
  std::string reason = "Disabled until " + waits;
  if (node.kind == compositor::NodeKind::Output) {
    reason += "; until then Program shows the scene as authored";
  }
  return reason;
}

[[nodiscard]] const CompositorPortVisual *
find_port(std::span<const CompositorPortVisual> ports,
          compositor::EdgeEndpoint endpoint) noexcept {
  const auto found =
      std::find_if(ports.begin(), ports.end(),
                   [endpoint](const CompositorPortVisual &port) noexcept {
                     return port.endpoint == endpoint;
                   });
  return found == ports.end() ? nullptr : std::addressof(*found);
}

[[nodiscard]] std::string_view
port_type_name(compositor::PortValueType type) noexcept {
  switch (type) {
  case compositor::PortValueType::Image:
    return "an image";
  case compositor::PortValueType::Effect:
    return "an effect";
  case compositor::PortValueType::TransformSet:
    return "array instances";
  case compositor::PortValueType::Filter:
    return "a filter (blur or key)";
  }
  return "an image";
}

[[nodiscard]] bool reaches(const compositor::CompositorGraph &graph,
                           doc::NodeId start, doc::NodeId target) {
  std::vector<doc::NodeId> pending{start};
  std::set<doc::NodeId> seen;
  while (!pending.empty()) {
    const doc::NodeId node = pending.back();
    pending.pop_back();
    if (node == target) {
      return true;
    }
    if (!seen.insert(node).second) {
      continue;
    }
    for (const compositor::GraphEdge &edge : graph.edges) {
      if (edge.from.node == node) {
        pending.push_back(edge.to.node);
      }
    }
  }
  return false;
}

[[nodiscard]] std::string
connection_refusal(const compositor::CompositorGraph &graph,
                   const CompositorPortVisual &from,
                   const CompositorPortVisual &to,
                   std::span<const doc::NodeId> ignored) {
  if (from.descriptor == nullptr || to.descriptor == nullptr ||
      from.descriptor->direction != compositor::PortDirection::Output ||
      to.descriptor->direction != compositor::PortDirection::Input) {
    return "Connect an output to an input";
  }
  if (from.descriptor->type != to.descriptor->type) {
    return std::string{"This input takes "} +
           std::string{port_type_name(to.descriptor->type)} +
           ", not " + std::string{port_type_name(from.descriptor->type)};
  }
  if (from.endpoint.node == to.endpoint.node) {
    return "A node cannot feed itself";
  }
  const auto counts = [ignored](const compositor::GraphEdge &edge) {
    return std::find(ignored.begin(), ignored.end(), edge.node) ==
           ignored.end();
  };
  if (std::any_of(graph.edges.begin(), graph.edges.end(),
                  [&](const compositor::GraphEdge &edge) {
                    return counts(edge) && edge.from == from.endpoint &&
                           edge.to == to.endpoint;
                  })) {
    return "Already connected here";
  }
  if (!from.descriptor->multiple &&
      std::any_of(graph.edges.begin(), graph.edges.end(),
                  [&](const compositor::GraphEdge &edge) {
                    return counts(edge) && edge.from == from.endpoint;
                  })) {
    return "This output already has its one wire";
  }
  if (reaches(graph, to.endpoint.node, from.endpoint.node)) {
    return "That would make a loop; wires flow toward Output";
  }
  return {};
}

[[nodiscard]] bool can_connect(const compositor::CompositorGraph &graph,
                               const CompositorPortVisual &from,
                               const CompositorPortVisual &to,
                               std::span<const doc::NodeId> ignored) {
  return connection_refusal(graph, from, to, ignored).empty();
}

void draw_node(gui::UiContext &ctx, const NodeBox &node) {
  const gui::UiColor body = node.highlighted
                                ? scale(ctx.theme().selection, 1.22F)
                                : scale(ctx.theme().panel, 1.08F);
  ctx.panel(node.bounds, body);
  ctx.outline(node.bounds,
              node.highlighted ? ctx.theme().accent : ctx.theme().widget_hover);
  // A band in the node's colour inside the outline's top edge.
  gui::Rect inside = gui::inset(node.bounds, 1.0F);
  ctx.panel(gui::cut_top(inside, 4.0F), node.color);
  // The title, and the detail under it overlapping its line by a pixel.
  const gui::Rect text =
      gui::inset(node.bounds, gui::Padding{9.0F, 7.0F, 7.0F, 0.0F});
  ctx.label(gui::Rect{text.x, text.y, text.width, 17.0F}, node.title,
            ctx.theme().text);
  ctx.label(gui::Rect{text.x, text.y + 16.0F, text.width, 16.0F}, node.detail,
            ctx.theme().text_muted);
  // Its input and output, at the middle of either side.
  gui::add_disc(ctx.draw_list(),
                gui::UiPoint{node.bounds.x, node.bounds.center_y()}, 3.0F,
                ctx.theme().keyframe);
  gui::add_disc(ctx.draw_list(),
                gui::UiPoint{node.bounds.right(), node.bounds.center_y()},
                3.0F, node.color);
}

[[nodiscard]] WirePath wire_path(float from_x, float from_y, float to_x,
                                 float to_y, float zoom) noexcept {
  const float dx = to_x - from_x;
  const float dy = to_y - from_y;
  float reach = std::clamp(std::abs(dx) * 0.5F, 40.0F * zoom, 220.0F * zoom);
  if (dx < 0.0F) {
    reach += std::min(std::abs(dy), std::abs(dx)) * 0.25F + 30.0F * zoom;
  }
  const gui::UiPoint p0{from_x, from_y};
  const gui::UiPoint p1{from_x + reach, from_y};
  const gui::UiPoint p2{to_x - reach, to_y};
  const gui::UiPoint p3{to_x, to_y};
  // The control polygon bounds the curve's length; about one segment per
  // ten pixels keeps the stroke smooth without wasting vertices.
  const auto distance = [](gui::UiPoint a, gui::UiPoint b) noexcept {
    return std::hypot(b.x - a.x, b.y - a.y);
  };
  const float length = distance(p0, p1) + distance(p1, p2) + distance(p2, p3);
  const std::size_t segments = std::clamp<std::size_t>(
      static_cast<std::size_t>(std::ceil(length / 10.0F)), 12U,
      WirePath::max_points - 1U);
  WirePath path;
  path.count = segments + 1U;
  for (std::size_t index = 0U; index <= segments; ++index) {
    const float t = static_cast<float>(index) / static_cast<float>(segments);
    const float u = 1.0F - t;
    const float a = u * u * u;
    const float b = 3.0F * u * u * t;
    const float c = 3.0F * u * t * t;
    const float d = t * t * t;
    path.points[index] =
        gui::UiPoint{a * p0.x + b * p1.x + c * p2.x + d * p3.x,
                     a * p0.y + b * p1.y + c * p2.y + d * p3.y};
  }
  return path;
}

void wire(gui::UiContext &ctx, float from_x, float from_y, float to_x,
          float to_y, gui::UiColor color, float thickness,
          float zoom) {
  const WirePath path = wire_path(from_x, from_y, to_x, to_y, zoom);
  ctx.draw_list().add_polyline(path.view(), thickness, color);
}

[[nodiscard]] std::vector<WirePath>
routed_wire(gui::UiPoint from, std::span<const gui::UiPoint> points,
            gui::UiPoint to, float zoom) {
  std::vector<WirePath> segments;
  segments.reserve(points.size() + 1U);
  gui::UiPoint start = from;
  for (const gui::UiPoint point : points) {
    segments.push_back(wire_path(start.x, start.y, point.x, point.y, zoom));
    start = point;
  }
  segments.push_back(wire_path(start.x, start.y, to.x, to.y, zoom));
  return segments;
}

[[nodiscard]] std::vector<CompositorMoveIntent>
align_boxes(std::span<const LayoutBox> boxes, AlignEdge edge) {
  std::vector<CompositorMoveIntent> moves;
  if (boxes.size() < 2U) {
    return moves;
  }
  float target = edge == AlignEdge::Left || edge == AlignEdge::Top
                     ? std::numeric_limits<float>::infinity()
                     : -std::numeric_limits<float>::infinity();
  for (const LayoutBox &box : boxes) {
    switch (edge) {
    case AlignEdge::Left:
      target = std::min(target, box.x);
      break;
    case AlignEdge::Right:
      target = std::max(target, box.x + box.width);
      break;
    case AlignEdge::Top:
      target = std::min(target, box.y);
      break;
    case AlignEdge::Bottom:
      target = std::max(target, box.y + box.height);
      break;
    }
  }
  for (const LayoutBox &box : boxes) {
    CompositorMoveIntent move{box.node, box.x, box.y};
    switch (edge) {
    case AlignEdge::Left:
      move.editor_x = target;
      break;
    case AlignEdge::Right:
      move.editor_x = target - box.width;
      break;
    case AlignEdge::Top:
      move.editor_y = target;
      break;
    case AlignEdge::Bottom:
      move.editor_y = target - box.height;
      break;
    }
    if (move.editor_x != box.x || move.editor_y != box.y) {
      moves.push_back(move);
    }
  }
  return moves;
}

[[nodiscard]] std::vector<CompositorMoveIntent>
distribute_boxes(std::span<const LayoutBox> boxes, bool horizontal) {
  std::vector<CompositorMoveIntent> moves;
  if (boxes.size() < 3U) {
    return moves;
  }
  std::vector<LayoutBox> sorted{boxes.begin(), boxes.end()};
  std::sort(sorted.begin(), sorted.end(),
            [horizontal](const LayoutBox &left, const LayoutBox &right) {
              const float a = horizontal ? left.x : left.y;
              const float b = horizontal ? right.x : right.y;
              return a != b ? a < b : left.node < right.node;
            });
  const auto position = [horizontal](const LayoutBox &box) {
    return horizontal ? box.x : box.y;
  };
  const auto extent = [horizontal](const LayoutBox &box) {
    return horizontal ? box.width : box.height;
  };
  float occupied{};
  for (const LayoutBox &box : sorted) {
    occupied += extent(box);
  }
  const float span =
      position(sorted.back()) + extent(sorted.back()) - position(sorted.front());
  const float gap = (span - occupied) / static_cast<float>(sorted.size() - 1U);
  float cursor = position(sorted.front()) + extent(sorted.front()) + gap;
  for (std::size_t index = 1U; index + 1U < sorted.size(); ++index) {
    const LayoutBox &box = sorted[index];
    if (cursor != position(box)) {
      moves.push_back(CompositorMoveIntent{box.node,
                                           horizontal ? cursor : box.x,
                                           horizontal ? box.y : cursor});
    }
    cursor += extent(box) + gap;
  }
  return moves;
}

[[nodiscard]] float wire_distance(const WirePath &path, float x,
                                  float y) noexcept {
  float nearest = std::numeric_limits<float>::infinity();
  for (std::size_t index = 0U; index + 1U < path.count; ++index) {
    const gui::UiPoint a = path.points[index];
    const gui::UiPoint b = path.points[index + 1U];
    const float sx = b.x - a.x;
    const float sy = b.y - a.y;
    const float length_squared = sx * sx + sy * sy;
    const float t =
        length_squared > 0.0F
            ? std::clamp(((x - a.x) * sx + (y - a.y) * sy) / length_squared,
                         0.0F, 1.0F)
            : 0.0F;
    nearest = std::min(nearest, std::hypot(x - (a.x + sx * t),
                                           y - (a.y + sy * t)));
  }
  return nearest;
}

} // namespace mediaserver::editor::compositor_ui
