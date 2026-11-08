#include <mediaserver/editor/compositor_panel.hpp>

#include <mediaserver/gui/form_layout.hpp>
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

std::string compositor_scene_layer_detail(
    doc::NodeId scene_node, std::span<const ElementInfo> elements) {
  const auto element = std::find_if(
      elements.begin(), elements.end(),
      [scene_node](const ElementInfo &candidate) {
        return candidate.node == scene_node;
      });
  if (element != elements.end()) {
    return element_display_name(*element);
  }
  return "OBJECT #" + std::to_string(scene_node.value());
}

namespace {

constexpr float compositor_toolbar_height = 26.0F;
// Node layout, in graph units at 100% zoom. Widths fit each node's own title
// and port names, and heights follow the editor font's line height, so no
// label is cut off whatever font or display scale the editor runs at.
constexpr float compositor_node_min_width = 176.0F;
constexpr float compositor_node_max_width = 360.0F;
constexpr float compositor_node_padding = 10.0F;
// From a port's dot to the start of its name.
constexpr float compositor_pin_inset = 13.0F;
// Room on a node's title line for its collapse toggle and problem badge.
constexpr float compositor_header_chrome = 40.0F;
// Below this zoom the toggle, badges and wire order numbers are left out, and
// below the next the values on a node are shown but not edited there.
constexpr float compositor_chrome_zoom = 0.6F;
constexpr float compositor_inline_edit_zoom = 0.9F;
// A bypassed node's amber: warm, so it reads apart from the cool selection
// and type colours, and quieter than the warning red.
constexpr gui::UiColor compositor_bypass_color{0.93F, 0.66F, 0.24F, 1.0F};
// Between an input's name and an output's name on the same row.
constexpr float compositor_pin_label_gap = 18.0F;
constexpr float compositor_port_radius = 5.0F;
// Text sizes, as multiples of the UI font, before zoom.
constexpr float compositor_title_text = 1.0F;
constexpr float compositor_detail_text = 0.875F;
constexpr float compositor_pin_text = 0.875F;
// Screen-space reach of the pointer, independent of zoom: how near a wire
// end has to come to a compatible port to snap to it, and how near the
// pointer has to be to a wire to pick it.
constexpr float compositor_port_snap_radius = 22.0F;
constexpr float compositor_wire_hit_radius = 6.0F;
// A released move whose edit never arrives - refused by the Director, say -
// stops overriding the graph after this many frames.
constexpr core::u32 compositor_pending_move_frames = 120U;
constexpr float compositor_minimum_zoom = 0.35F;
constexpr float compositor_maximum_zoom = 2.5F;
constexpr float compositor_zoom_step = 1.18F;

enum class CompositorNodeCategory : core::u8 {
  Input,
  Color,
  Keying,
  Blur,
  Texture,
  Spatial,
  Combine,
  Count,
};

struct CompositorPaletteItem final {
  compositor::NodeKind kind;
  CompositorNodeCategory category;
  std::string_view search_terms;
};

constexpr std::array<std::string_view, 7U> compositor_category_names{{
    "INPUTS",
    "COLOR",
    "KEYING",
    "BLUR",
    "TEXTURE",
    "SPATIAL / 3D",
    "COMBINE & OUTPUT",
}};

constexpr std::array<CompositorPaletteItem, 22U> compositor_palette_items{{
    {compositor::NodeKind::SceneLayer, CompositorNodeCategory::Input,
     "source object timeline layer"},
    {compositor::NodeKind::Background, CompositorNodeCategory::Input,
     "solid colour color gradient fill backdrop matte linear radial"},
    {compositor::NodeKind::Brightness, CompositorNodeCategory::Color,
     "exposure gain"},
    {compositor::NodeKind::Saturation, CompositorNodeCategory::Color,
     "colour color intensity"},
    {compositor::NodeKind::Levels, CompositorNodeCategory::Color,
     "tonal range"},
    {compositor::NodeKind::LiftGammaGain, CompositorNodeCategory::Color,
     "shadows midtones highlights"},
    {compositor::NodeKind::ContrastPivot, CompositorNodeCategory::Color,
     "contrast pivot"},
    {compositor::NodeKind::ColorMatrix, CompositorNodeCategory::Color,
     "channel mixer rgb matrix"},
    {compositor::NodeKind::WhiteBalance, CompositorNodeCategory::Color,
     "temperature tint"},
    {compositor::NodeKind::Curves, CompositorNodeCategory::Color,
     "curve tonal"},
    {compositor::NodeKind::ChromaKey, CompositorNodeCategory::Keying,
     "green screen colour key"},
    {compositor::NodeKind::LumaKey, CompositorNodeCategory::Keying,
     "luminance key"},
    {compositor::NodeKind::Matte, CompositorNodeCategory::Keying,
     "alpha luma mask"},
    {compositor::NodeKind::Blur, CompositorNodeCategory::Blur,
     "gaussian smooth"},
    {compositor::NodeKind::BoxBlur, CompositorNodeCategory::Blur, "box smooth"},
    {compositor::NodeKind::DirectionalBlur, CompositorNodeCategory::Blur,
     "motion angle"},
    {compositor::NodeKind::FilmGrain, CompositorNodeCategory::Texture,
     "noise texture"},
    {compositor::NodeKind::RadialArray, CompositorNodeCategory::Spatial,
     "array instances radial spatial 3d"},
    {compositor::NodeKind::LedCarProcess, CompositorNodeCategory::Spatial,
     "led virtual production car process plates volume reflection ceiling"},
    {compositor::NodeKind::Composite, CompositorNodeCategory::Combine,
     "stack combine layers"},
    {compositor::NodeKind::Merge, CompositorNodeCategory::Combine,
     "mix blend overlay multiply add screen opacity brightness"},
    {compositor::NodeKind::Output, CompositorNodeCategory::Combine,
     "program final render"},
}};

// In compositor::MergeBlendMode order.
constexpr std::array<std::string_view, compositor::merge_blend_mode_count>
    merge_blend_labels{{"NORMAL", "ADD", "MULTIPLY", "SCREEN", "SUBTRACT",
                        "OVERLAY", "LIGHTEN", "DARKEN", "DIFFERENCE"}};

// In compositor::BackgroundMode order.
constexpr std::array<std::string_view, 3U> background_mode_labels{
    {"SOLID", "LINEAR GRADIENT", "RADIAL GRADIENT"}};

[[nodiscard]] constexpr std::size_t
category_index(CompositorNodeCategory category) noexcept {
  return static_cast<std::size_t>(category);
}

struct CompositorNodeVisual final {
  const compositor::GraphNode *node{};
  gui::Rect bounds;
};

struct CompositorPortVisual final {
  compositor::EdgeEndpoint endpoint;
  const compositor::PortDescriptor *descriptor{};
  // The dot's centre.
  float x{};
  float y{};
  // Its whole row on that side of the node: dot and name, so a press or a
  // drop does not have to find a ten-pixel circle.
  gui::Rect hit;
};

// Wires and ports are coloured by what flows through them, so an operator
// can see which ports accept a wire before trying. The palette stays cool:
// warm amber belongs to the playhead alone.
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

// The colours a comment box can take: muted, so a box frames nodes rather
// than competing with them.
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

[[nodiscard]] constexpr std::string_view comment_color_name(core::u8 index) noexcept {
  constexpr std::array<std::string_view, compositor::comment_color_count> names{
      {"SLATE", "BLUE", "TEAL", "GREEN", "VIOLET", "ROSE"}};
  return names[index < names.size() ? index : 0U];
}

[[nodiscard]] gui::UiColor blend_color(gui::UiColor from, gui::UiColor to,
                                       float amount) noexcept {
  return gui::UiColor{from.r + (to.r - from.r) * amount,
                      from.g + (to.g - from.g) * amount,
                      from.b + (to.b - from.b) * amount,
                      from.a + (to.a - from.a) * amount};
}

// Each value type has its own port shape as well as its own colour, so the
// difference survives colour-blindness and a dimmed, incompatible port: an
// image is a circle, an effect a diamond, a filter a triangle and a
// transform set a square.
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

// A connected port is filled; an open one is a ring showing the node behind.
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

// One accent per Add Node category, so a node reads as the same family it
// was picked from.
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

// `text` at `size` times the UI font, in graph units. Zoomed text is drawn
// from the nearest baked size at or below what it asks for, so the nominal
// scaled width bounds it; a size baked exactly is measured as drawn.
[[nodiscard]] float compositor_text_width(const gui::UiContext &ctx,
                                          std::string_view text, float size) {
  const float nominal = ctx.font_width(text) * size;
  const std::optional<float> baked = ctx.text_size_at_most(size);
  return baked.has_value() && *baked == size
             ? std::max(nominal, ctx.font_width(text, size))
             : nominal;
}

// A line of that text, rounded up to whole units so the rows below it stay
// on the pixel grid at 100% and a glyph's rounded baseline cannot spill out.
[[nodiscard]] float compositor_text_height(const gui::UiContext &ctx,
                                           float size) {
  const std::optional<float> baked = ctx.text_size_at_most(size);
  return std::ceil(baked.has_value() && *baked == size
                       ? std::max(ctx.line_height() * size,
                                  ctx.line_height(size))
                       : ctx.line_height() * size);
}

// Draws `text` at `size` times the UI font scaled by the graph's zoom, from
// the largest baked size that fits. Returns false, drawing nothing, when even
// the smallest baked text is too big - far zoomed out - so a caller can skip
// detail an operator could not read anyway.
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

// A value shown, and editable, on a node's own body: the one or two values
// an operator reaches for most, without opening Parameters.
struct InlineValue final {
  std::string_view label;
  CompositorProperty property{CompositorProperty::BrightnessAmount};
  double speed{};
  double minimum{};
  double maximum{};
  int decimals{};
  // Authored as a whole number.
  bool whole{};
  // The input this value stands in for: it applies, and is editable, only
  // while that input is unwired. Negative for an ordinary value.
  int unwired_input{-1};
};

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

// An inline value that does nothing in the node's present wiring: one
// standing in for a wired input, or a Merge's constant under Normal, which
// has nothing to lay over BASE.
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

// The authored value an inline row shows.
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

// An open node's height: its port rows, then its inline value rows.
[[nodiscard]] float compositor_node_height(const CompositorNodeMetrics &metrics,
                                           compositor::NodeKind kind) noexcept {
  const std::size_t rows =
      std::max({compositor::input_ports(kind).size(),
                compositor::output_ports(kind).size(), std::size_t{1U}}) +
      inline_values(kind).size();
  return metrics.header_height +
         static_cast<float>(rows) * metrics.row_height + metrics.footer_height;
}

// Wide enough for the title, a Scene Layer's source name, and the longest
// input and output names side by side on one row. Other details carry live
// values and are left out on purpose: a node must not change width while
// its value is being scrubbed. Rounded up so a renamed source does not nudge
// the node by a pixel.
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

// Bits of CompositorPanel's collapsed Parameters sections.
constexpr core::u32 led_car_position_section = 0U;
constexpr core::u32 led_car_rotation_section = 1U;
constexpr core::u32 led_car_plates_section = 2U;
constexpr core::u32 led_car_reflection_section = 3U;
constexpr core::u32 levels_master_section = 4U;
constexpr core::u32 levels_red_section = 5U;
constexpr core::u32 levels_green_section = 6U;
constexpr core::u32 levels_blue_section = 7U;

// Every field label the Parameters panel shows for a node kind, so the label
// column can be sized to the longest before any row is drawn. A label left
// out here still draws; it is shortened with an ellipsis if it does not fit.
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

// Something for the operator to fix. A parked branch - a node kept in the
// graph that reaches no Output - is not: it is drawn faded and otherwise left
// alone, never counted or outlined as a problem.
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

// Kept in the graph but reaching no Output.
[[nodiscard]] bool node_parked(const compositor::CompositorGraph &graph,
                               doc::NodeId node) noexcept {
  return std::any_of(
      graph.diagnostics.begin(), graph.diagnostics.end(),
      [node](const compositor::GraphDiagnostic &diagnostic) noexcept {
        return !is_problem(diagnostic) && diagnostic.subject == node;
      });
}

// Waiting for a required input, and so taking no part in the picture.
[[nodiscard]] bool node_disabled(const compositor::CompositorGraph &graph,
                                 doc::NodeId node) noexcept {
  const compositor::GraphNodeMetadata *const metadata = graph.metadata(node);
  return metadata != nullptr && metadata->disabled;
}

// What a disabled node is waiting for, in the operator's words: each required
// input with no wire, or whose wires come only from nodes that are waiting
// themselves.
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

// Whether following wires downstream from `start` arrives at `target`.
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

// Why a wire from `from` cannot end at `to`, in words an operator can act
// on; empty when it can. `ignored` are wires being moved, which do not count
// as already there. An occupied single input is connectable on purpose: the
// new wire replaces the old one in the same undo step.
[[nodiscard]] std::string
connection_refusal(const compositor::CompositorGraph &graph,
                   const CompositorPortVisual &from,
                   const CompositorPortVisual &to,
                   std::span<const doc::NodeId> ignored = {}) {
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
                               std::span<const doc::NodeId> ignored = {}) {
  return connection_refusal(graph, from, to, ignored).empty();
}

// Retained for the read-only scene-element compatibility adapter below. The
// graph overload uses the typed node/port visuals above.
struct NodeBox final {
  gui::Rect bounds;
  std::string title;
  std::string detail;
  gui::UiColor color;
  bool highlighted{};
};

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

// A wire leaves its output and enters its input horizontally, as a cubic
// whose reach grows with the distance it spans. A wire running backwards -
// to a node left of its source - reaches further out so it loops around
// rather than folding back over the nodes it joins.
struct WirePath final {
  static constexpr std::size_t max_points = 49U;
  std::array<gui::UiPoint, max_points> points{};
  std::size_t count{};

  [[nodiscard]] std::span<const gui::UiPoint> view() const noexcept {
    return std::span<const gui::UiPoint>{points.data(), count};
  }
};

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
          float to_y, gui::UiColor color, float thickness = 2.0F,
          float zoom = 1.0F) {
  const WirePath path = wire_path(from_x, from_y, to_x, to_y, zoom);
  ctx.draw_list().add_polyline(path.view(), thickness, color);
}

// A wire through its reroute points: one curve per stretch, each leaving
// and entering horizontally, so a routed wire still reads as flowing right.
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

// A node's box in graph units, for the layout tools.
struct LayoutBox final {
  doc::NodeId node{};
  float x{};
  float y{};
  float width{};
  float height{};
};

enum class AlignEdge : core::u8 { Left, Right, Top, Bottom };

// Lines the boxes up on one edge of the outermost of them.
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

// Spaces three or more boxes evenly between the first and last along one
// axis; the outer two stay where they are.
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

// The pointer's distance from the drawn curve, for picking a wire.
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

} // namespace

std::string_view compositor_diagnostic_message(
    const compositor::GraphDiagnostic &diagnostic) noexcept {
  switch (diagnostic.code) {
  case compositor::DiagnosticCode::NodesOverCapacity:
    return "Too many nodes; remove a node";
  case compositor::DiagnosticCode::EdgesOverCapacity:
    return "Too many connections; disconnect a wire";
  case compositor::DiagnosticCode::DanglingNode:
    return "A wire references a missing node";
  case compositor::DiagnosticCode::DanglingSceneReference:
    return "A Layer references a missing scene object";
  case compositor::DiagnosticCode::UnknownPort:
    return "A wire references a missing port";
  case compositor::DiagnosticCode::IncompatiblePorts:
    return "A wire joins different kinds of port; filters go into FILTERS";
  case compositor::DiagnosticCode::DuplicateInput:
    return "An input already has a connection";
  case compositor::DiagnosticCode::MissingOutput:
    return "Add and connect one Output node";
  case compositor::DiagnosticCode::DuplicateOutput:
    return "Keep only one Output node";
  case compositor::DiagnosticCode::DuplicateSceneLayerReference:
    return "One active Scene Layer per source is supported";
  case compositor::DiagnosticCode::Cycle:
    return "Cycle detected; wires must flow toward Output";
  case compositor::DiagnosticCode::DeadBranch:
    return "This branch does not reach Output";
  case compositor::DiagnosticCode::PassBudgetExceeded:
    return "Graph exceeds the authored pass budget";
  case compositor::DiagnosticCode::TextureBudgetExceeded:
    return "Graph exceeds the offscreen texture budget";
  case compositor::DiagnosticCode::TapBudgetExceeded:
    return "Graph exceeds the blur tap budget";
  case compositor::DiagnosticCode::MergeNestingTooDeep:
    return "Merges are nested more than 4 deep; flatten one";
  case compositor::DiagnosticCode::UnsupportedMatteTopology:
    return "Matte needs a Scene Layer source and a Scene Layer or Background "
           "matte";
  case compositor::DiagnosticCode::UnsupportedMatteGroupSource:
    return "A Matte cannot use a group";
  case compositor::DiagnosticCode::MatteBranchAlsoOutputReachable:
    return "A Matte's matte layer cannot also be shown";
  }
  return "Graph is not ready";
}

std::string_view compositor_diagnostic_severity_label(
    const compositor::GraphDiagnostic &diagnostic) noexcept {
  return compositor::diagnostic_severity_name(diagnostic.severity);
}

void draw_compositor_drop_target(gui::UiContext &ctx, const gui::Rect &bounds,
                                 std::string_view item_name) {
  gui::Rect canvas = bounds;
  static_cast<void>(gui::cut_top(canvas, compositor_toolbar_height));
  const bool hovered = canvas.contains(ctx.pointer_x(), ctx.pointer_y());
  std::string label{hovered ? "RELEASE TO ADD NODE  ·  "
                            : "DROP IN NODE GRAPH  ·  "};
  label += item_name.empty() ? "ITEM" : item_name;
  draw_drop_target(ctx, canvas, label, hovered, 360.0F);
}

std::optional<gui::UiPoint>
CompositorPanel::pending_position(doc::NodeId node) const noexcept {
  const auto found = std::find_if(
      pending_moves_.begin(), pending_moves_.end(),
      [node](const PendingMove &move) noexcept { return move.node == node; });
  return found == pending_moves_.end()
             ? std::nullopt
             : std::optional<gui::UiPoint>{gui::UiPoint{found->x, found->y}};
}

void CompositorPanel::colour_well_field(
    gui::UiContext &ctx, const gui::Rect &well, doc::NodeId node,
    core::u64 slot, gui::UiColor current,
    const std::array<CompositorProperty, 3U> &channels,
    CompositorFrame &frame) {
  const gui::UiId id = gui::ui_id(id_, "compositor.colour", node.value() * 8U + slot);
  gui::UiColor picked = current;
  if (ctx.color_well(id, well, picked)) {
    colour_target_ = ColourTarget{id, node, channels, picked};
  }
  if (ctx.color_open(id)) {
    colour_editing_ = true;
  }
  if (!colour_target_.has_value() || colour_target_->well != id ||
      colour_target_->node != node) {
    return;
  }
  // A frame carries one property, so the picked colour lands a channel a
  // frame, inside the one gesture the open picker holds.
  const std::array<float, 3U> now{current.r, current.g, current.b};
  const std::array<float, 3U> want{colour_target_->color.r,
                                   colour_target_->color.g,
                                   colour_target_->color.b};
  for (std::size_t index = 0U; index < 3U; ++index) {
    if (std::abs(now[index] - want[index]) <= 1.0e-6F) {
      continue;
    }
    if (!frame.property_edit.has_value()) {
      frame.property_edit = CompositorPropertyEditIntent{
          node, colour_target_->channels[index],
          CompositorPropertyValue{want[index]}};
    }
    return;
  }
  colour_target_.reset();
}

void CompositorPanel::select_only(doc::NodeId node) {
  selection_ = CompositorSelection{CompositorSelectionKind::Node, node};
  selected_nodes_.assign(1U, node);
}

std::optional<CompositorDeleteIntent> CompositorPanel::selection_delete(
    const compositor::CompositorGraph &graph) const {
  CompositorDeleteIntent remove;
  if (selection_.kind == CompositorSelectionKind::Edge ||
      selection_.kind == CompositorSelectionKind::Comment) {
    remove.targets.push_back(selection_);
  }
  for (const doc::NodeId node : selected_nodes_) {
    const compositor::GraphNode *const found = graph.node(node);
    // Output is structural: the graph keeps its one.
    if (found != nullptr && found->kind != compositor::NodeKind::Output) {
      remove.targets.push_back(
          CompositorSelection{CompositorSelectionKind::Node, node});
    }
  }
  return remove.targets.empty() ? std::nullopt
                                : std::optional<CompositorDeleteIntent>{
                                      std::move(remove)};
}

bool CompositorPanel::collapsed(
    const compositor::GraphNode &node) const noexcept {
  const auto held = std::find_if(
      pending_collapse_.begin(), pending_collapse_.end(),
      [&node](const auto &entry) noexcept { return entry.first == node.node; });
  return held != pending_collapse_.end() ? held->second : node.collapsed;
}

bool CompositorPanel::bypassed(
    const compositor::GraphNode &node) const noexcept {
  const auto held = std::find_if(
      pending_bypass_.begin(), pending_bypass_.end(),
      [&node](const auto &entry) noexcept { return entry.first == node.node; });
  return held != pending_bypass_.end() ? held->second : node.bypassed;
}

core::i64 CompositorPanel::edge_order(
    const compositor::GraphEdge &edge) const noexcept {
  const auto held = std::find_if(
      pending_order_.begin(), pending_order_.end(),
      [&edge](const auto &entry) noexcept { return entry.first == edge.node; });
  return held != pending_order_.end() ? held->second : edge.order;
}

std::vector<const compositor::GraphEdge *>
CompositorPanel::input_chain(const compositor::CompositorGraph &graph,
                             compositor::EdgeEndpoint input) const {
  std::vector<const compositor::GraphEdge *> chain;
  for (const compositor::GraphEdge &edge : graph.edges) {
    if (edge.to == input) {
      chain.push_back(std::addressof(edge));
    }
  }
  std::sort(chain.begin(), chain.end(),
            [this](const compositor::GraphEdge *left,
                   const compositor::GraphEdge *right) {
              const core::i64 a = edge_order(*left);
              const core::i64 b = edge_order(*right);
              return a != b ? a < b : left->node < right->node;
            });
  return chain;
}

std::optional<float>
CompositorPanel::port_row(const compositor::CompositorGraph &graph,
                          const compositor::GraphNode &node, core::u8 port,
                          bool input) const {
  const auto ports = input ? compositor::input_ports(node.kind)
                           : compositor::output_ports(node.kind);
  const bool packed = collapsed(node);
  std::size_t row{};
  for (const compositor::PortDescriptor &descriptor : ports) {
    const compositor::EdgeEndpoint endpoint{node.node, descriptor.index};
    // A collapsed node keeps only its connected ports, packed together.
    const bool shown =
        !packed ||
        std::any_of(graph.edges.begin(), graph.edges.end(),
                    [&](const compositor::GraphEdge &edge) noexcept {
                      return (input ? edge.to : edge.from) == endpoint;
                    });
    if (descriptor.index == port) {
      return shown ? std::optional<float>{static_cast<float>(row)}
                   : std::nullopt;
    }
    if (shown) {
      ++row;
    }
  }
  return std::nullopt;
}

std::optional<CompositorWireDrop>
CompositorPanel::effect_wire_at(const compositor::CompositorGraph &graph,
                                float x, float y,
                                compositor::PortValueType type) const {
  if (!last_canvas_.contains(x, y)) {
    return std::nullopt;
  }
  const DrawnWire *best = nullptr;
  float nearest = 8.0F;
  gui::UiPoint at{};
  for (const DrawnWire &drawn : drawn_wires_) {
    if (drawn.type != type) {
      continue;
    }
    for (std::size_t index = 0U; index + 1U < drawn.points.size(); ++index) {
      const gui::UiPoint a = drawn.points[index];
      const gui::UiPoint b = drawn.points[index + 1U];
      const float sx = b.x - a.x;
      const float sy = b.y - a.y;
      const float length = sx * sx + sy * sy;
      const float t =
          length > 0.0F
              ? std::clamp(((x - a.x) * sx + (y - a.y) * sy) / length, 0.0F,
                           1.0F)
              : 0.0F;
      const gui::UiPoint closest{a.x + sx * t, a.y + sy * t};
      const float distance = std::hypot(x - closest.x, y - closest.y);
      if (distance <= nearest) {
        nearest = distance;
        best = std::addressof(drawn);
        at = closest;
      }
    }
  }
  const compositor::GraphEdge *const edge =
      best != nullptr ? graph.edge(best->edge) : nullptr;
  if (edge == nullptr) {
    return std::nullopt;
  }
  // The input's wires in the order it applies them; the new one goes right
  // after this one, and the rest are spaced out to make room.
  const std::vector<const compositor::GraphEdge *> chain =
      input_chain(graph, edge->to);
  CompositorWireDrop drop{edge->node, edge->to, at.x, at.y};
  drop.type = type;
  for (std::size_t index = 0U; index < chain.size(); ++index) {
    const core::i64 spaced = static_cast<core::i64>(index) * 2;
    if (edge_order(*chain[index]) != spaced) {
      drop.reorder.emplace_back(chain[index]->node, spaced);
    }
    if (chain[index]->node == edge->node) {
      drop.order = spaced + 1;
    }
  }
  if (const compositor::GraphNode *const source = graph.node(edge->from.node)) {
    drop.after = compositor::node_kind_name(source->kind);
  }
  return drop;
}

void CompositorPanel::draw_wire_drop(gui::UiContext &ctx,
                                     const CompositorWireDrop &drop,
                                     std::string_view what) const {
  ctx.push_clip(last_canvas_);
  for (const DrawnWire &drawn : drawn_wires_) {
    if (drawn.edge == drop.edge) {
      ctx.draw_list().add_polyline(drawn.points,
                                   std::clamp(4.0F * view_.zoom, 3.0F, 6.0F),
                                   ctx.theme().text);
    }
  }
  draw_port_shape(ctx, drop.type, drop.x, drop.y,
                  std::max(4.0F, 6.0F * view_.zoom), ctx.theme().text);
  std::string label{what};
  label += "  -  INSERT AFTER ";
  label += drop.after;
  const float width = std::ceil(ctx.font_width(label)) + 16.0F;
  const float height = ctx.line_height() + 8.0F;
  const gui::Rect badge{drop.x + 12.0F, drop.y + 10.0F, width, height};
  ctx.panel(badge, with_alpha(ctx.theme().background, 0.92F));
  ctx.outline(badge, port_color(ctx, drop.type));
  ctx.label(gui::Rect{badge.x + 8.0F, badge.y, badge.width - 8.0F,
                      badge.height},
            label, ctx.theme().text);
  ctx.pop_clip();
}

gui::Rect CompositorPanel::node_screen_bounds(
    const compositor::GraphNode &node) const noexcept {
  const std::optional<gui::UiPoint> pending = pending_position(node.node);
  const gui::UiPoint size = node_size(node);
  return view_.to_screen(
      last_canvas_, gui::Rect{pending ? pending->x : node.editor_x,
                              pending ? pending->y : node.editor_y, size.x,
                              size.y});
}

std::optional<std::pair<doc::NodeId, gui::Rect>>
CompositorPanel::node_at(const compositor::CompositorGraph &graph,
                         float x, float y) const noexcept {
  if (!last_canvas_.contains(x, y)) return std::nullopt;
  for (auto it = graph.nodes.rbegin(); it != graph.nodes.rend(); ++it) {
    const gui::Rect bounds = node_screen_bounds(*it);
    if (bounds.contains(x, y)) return std::pair{it->node, gui::intersection(bounds, last_canvas_)};
  }
  return std::nullopt;
}

gui::UiPoint
CompositorPanel::node_size(const compositor::GraphNode &node) const noexcept {
  const auto found = std::lower_bound(
      node_sizes_.begin(), node_sizes_.end(), node.node,
      [](const NodeSize &size, doc::NodeId id) noexcept {
        return size.node < id;
      });
  if (found != node_sizes_.end() && found->node == node.node) {
    return gui::UiPoint{found->width, found->height};
  }
  return new_node_size(node.kind);
}

gui::UiPoint
CompositorPanel::new_node_size(compositor::NodeKind kind) const noexcept {
  const auto index = static_cast<std::size_t>(kind);
  if (index < new_node_sizes_.size() && new_node_sizes_[index].x > 0.0F) {
    return new_node_sizes_[index];
  }
  // Not drawn yet: the minimum width at the nominal 14-unit font.
  const CompositorNodeMetrics nominal{35.0F, 22.0F, 7.0F};
  return gui::UiPoint{compositor_node_min_width,
                      compositor_node_height(metrics_.row_height > 0.0F
                                                 ? metrics_
                                                 : nominal,
                                             kind)};
}

std::optional<compositor::EdgeEndpoint>
CompositorPanel::input_port_at(const compositor::CompositorGraph &graph,
                               float x, float y) const noexcept {
  if (!last_canvas_.contains(x, y)) return std::nullopt;
  // Topmost node first, as node_at() does, so the answer is the port the
  // operator can see.
  for (auto it = graph.nodes.rbegin(); it != graph.nodes.rend(); ++it) {
    const gui::Rect bounds = node_screen_bounds(*it);
    const float reach = compositor_port_radius * view_.zoom + 4.0F;
    const gui::Rect with_dots{bounds.x - reach, bounds.y,
                              bounds.width + reach, bounds.height};
    if (!with_dots.contains(x, y)) {
      continue;
    }
    const auto ports = compositor::input_ports(it->kind);
    for (std::size_t index = 0U; index < ports.size(); ++index) {
      const std::optional<float> place =
          port_row(graph, *it, ports[index].index, true);
      if (!place.has_value()) {
        continue;
      }
      const float row_top =
          bounds.y +
          (metrics_.header_height + metrics_.row_height * *place) * view_.zoom;
      const gui::Rect row{bounds.x - reach, row_top,
                          bounds.width * 0.5F + reach,
                          metrics_.row_height * view_.zoom};
      if (row.contains(x, y)) {
        return compositor::EdgeEndpoint{it->node, ports[index].index};
      }
    }
    if (bounds.contains(x, y)) {
      return std::nullopt; // over this node, but not over one of its inputs
    }
  }
  return std::nullopt;
}

std::optional<CompositorPortDrop> CompositorPanel::port_drop_target(
    const compositor::CompositorGraph &graph, float x, float y,
    compositor::PortValueType type) const noexcept {
  if (!last_canvas_.contains(x, y)) return std::nullopt;
  std::optional<CompositorPortDrop> best;
  float best_rank = std::numeric_limits<float>::infinity();
  // Later nodes draw on top, so on a tie the topmost wins.
  for (auto it = graph.nodes.rbegin(); it != graph.nodes.rend(); ++it) {
    const gui::Rect bounds = node_screen_bounds(*it);
    const float reach = compositor_port_radius * view_.zoom + 4.0F;
    const auto ports = compositor::input_ports(it->kind);
    for (std::size_t index = 0U; index < ports.size(); ++index) {
      const compositor::PortDescriptor &port = ports[index];
      const std::optional<float> place =
          port_row(graph, *it, port.index, true);
      if (port.type != type || !place.has_value()) {
        continue;
      }
      const float row_top =
          bounds.y +
          (metrics_.header_height + metrics_.row_height * *place) * view_.zoom;
      const float row_height = metrics_.row_height * view_.zoom;
      const gui::Rect row{bounds.x - reach, row_top,
                          bounds.width * 0.5F + reach, row_height};
      const float dot_y = row_top + row_height * 0.5F;
      const float distance = std::hypot(x - bounds.x, y - dot_y);
      const bool over_row = row.contains(x, y);
      if (!over_row && distance > compositor_port_snap_radius) {
        continue;
      }
      const float rank = over_row ? distance - 1.0e6F : distance;
      if (rank >= best_rank) {
        continue;
      }
      best_rank = rank;
      const compositor::EdgeEndpoint endpoint{it->node, port.index};
      std::optional<doc::NodeId> occupied;
      if (!port.multiple) {
        const auto wired = std::find_if(
            graph.edges.begin(), graph.edges.end(),
            [endpoint](const compositor::GraphEdge &edge) noexcept {
              return edge.to == endpoint;
            });
        if (wired != graph.edges.end()) {
          occupied = wired->node;
        }
      }
      best = CompositorPortDrop{endpoint, port.name, port.type, bounds.x,
                                dot_y, occupied, port.multiple};
    }
  }
  return best;
}

void CompositorPanel::draw_port_drop(gui::UiContext &ctx,
                                     const CompositorPortDrop &drop,
                                     std::string_view what) const {
  ctx.push_clip(last_canvas_);
  const gui::UiColor color = port_color(ctx, drop.type);
  wire(ctx, ctx.pointer_x(), ctx.pointer_y(), drop.x, drop.y,
       with_alpha(color, 0.9F), std::clamp(2.5F * view_.zoom, 2.0F, 4.0F), view_.zoom);
  const float radius = std::max(2.5F, compositor_port_radius * view_.zoom) + 3.0F;
  draw_port_shape(ctx, drop.type, drop.x, drop.y, radius + 1.5F,
                  ctx.theme().text);
  draw_port_shape(ctx, drop.type, drop.x, drop.y, radius, color);
  // The label sits left of the port, where the new node will arrive.
  std::string label{what};
  label += drop.occupied.has_value() ? "  -  REPLACE " : "  -  CONNECT TO ";
  label += drop.name;
  const float width = std::ceil(ctx.font_width(label)) + 16.0F;
  const float height = ctx.line_height() + 8.0F;
  const gui::Rect badge{drop.x - radius - 10.0F - width,
                        drop.y - height * 0.5F, width, height};
  ctx.panel(badge, with_alpha(ctx.theme().background, 0.92F));
  ctx.outline(badge, color);
  ctx.label(gui::Rect{badge.x + 8.0F, badge.y, badge.width - 8.0F,
                      badge.height},
            label, ctx.theme().text);
  ctx.pop_clip();
}

gui::UiPoint CompositorPanel::node_position_feeding(
    compositor::NodeKind kind, const CompositorPortDrop &drop) const noexcept {
  const gui::UiPoint size = new_node_size(kind);
  // A clear gap between the new node's output and the input it feeds.
  constexpr float gap = 70.0F;
  const gui::UiPoint at = view_.to_graph(last_canvas_, drop.x, drop.y);
  return gui::UiPoint{at.x - gap - size.x,
                      at.y - metrics_.header_height -
                          metrics_.row_height * 0.5F};
}

CompositorFrame
CompositorPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                      const compositor::CompositorGraph &compositor_graph,
                      std::span<const ElementInfo> elements) {
  CompositorFrame frame;
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return frame;
  }
  if (ctx.pointer_pressed()) {
    focused_ = bounds.contains(ctx.pointer_x(), ctx.pointer_y());
  }
  const auto comment_of =
      [&compositor_graph](doc::NodeId id) -> const compositor::GraphComment * {
    const auto found = std::find_if(
        compositor_graph.comments.begin(), compositor_graph.comments.end(),
        [id](const compositor::GraphComment &comment) noexcept {
          return comment.node == id;
        });
    return found == compositor_graph.comments.end() ? nullptr
                                                    : std::addressof(*found);
  };
  std::erase_if(selected_nodes_, [&](doc::NodeId node) {
    return compositor_graph.node(node) == nullptr;
  });
  if ((selection_.kind == CompositorSelectionKind::Node &&
       compositor_graph.node(selection_.node) == nullptr) ||
      (selection_.kind == CompositorSelectionKind::Edge &&
       compositor_graph.edge(selection_.node) == nullptr) ||
      (selection_.kind == CompositorSelectionKind::Comment &&
       comment_of(selection_.node) == nullptr)) {
    // The primary went away: another selected node takes its place.
    selection_ = selected_nodes_.empty()
                     ? CompositorSelection{}
                     : CompositorSelection{CompositorSelectionKind::Node,
                                           selected_nodes_.front()};
  }
  // A paste or duplicate selects what it added once that arrives.
  if (paste_baseline_.has_value()) {
    std::vector<doc::NodeId> added;
    for (const compositor::GraphNode &node : compositor_graph.nodes) {
      if (!std::binary_search(paste_baseline_->nodes.begin(),
                              paste_baseline_->nodes.end(), node.node)) {
        added.push_back(node.node);
      }
    }
    if (!added.empty()) {
      selected_nodes_ = std::move(added);
      selection_ = CompositorSelection{CompositorSelectionKind::Node,
                                       selected_nodes_.front()};
      paste_baseline_.reset();
    } else if (++paste_baseline_->frames > compositor_pending_move_frames) {
      paste_baseline_.reset();
    }
  }

  const gui::Rect toolbar{bounds.x, bounds.y, bounds.width,
                          std::min(bounds.height, compositor_toolbar_height)};
  const gui::Rect content{bounds.x, bounds.y + toolbar.height, bounds.width,
                          std::max(bounds.height - toolbar.height, 0.0F)};

  const compositor::GraphNode *parameter_node =
      selection_.kind == CompositorSelectionKind::Node
          ? compositor_graph.node(selection_.node)
          : nullptr;
  if (parameter_node != nullptr &&
      !has_editable_parameters(parameter_node->kind)) {
    parameter_node = nullptr;
  }
  if (parameter_node == nullptr) {
    if (parameter_node_.value() != 0U) {
      parameter_fields_.cancel(ctx);
      parameter_node_ = {};
    }
  } else if (parameter_node_ != parameter_node->node) {
    parameter_fields_.cancel(ctx);
    parameter_node_ = parameter_node->node;
    parameter_scroll_ = 0.0F;
    parameter_content_height_ = 0.0F;
  }

  // Collapse and bypass toggles end once the graph shows them.
  std::erase_if(pending_collapse_, [&](const auto &held) {
    const compositor::GraphNode *const node = compositor_graph.node(held.first);
    return node == nullptr || node->collapsed == held.second;
  });
  std::erase_if(pending_bypass_, [&](const auto &held) {
    const compositor::GraphNode *const node = compositor_graph.node(held.first);
    return node == nullptr || node->bypassed == held.second;
  });
  std::erase_if(pending_order_, [&](const auto &held) {
    const compositor::GraphEdge *const edge = compositor_graph.edge(held.first);
    return edge == nullptr || edge->order == held.second;
  });
  // Every node's size this frame, from the font it is drawn in.
  metrics_ = compositor_node_metrics(ctx);
  for (const CompositorPaletteItem &item : compositor_palette_items) {
    new_node_sizes_[static_cast<std::size_t>(item.kind)] = gui::UiPoint{
        compositor_node_width(ctx, item.kind,
                              compositor::node_kind_name(item.kind), {}),
        compositor_node_height(metrics_, item.kind)};
  }
  node_sizes_.clear();
  node_sizes_.reserve(compositor_graph.nodes.size());
  for (const compositor::GraphNode &node : compositor_graph.nodes) {
    const std::string_view title =
        node.label.empty() ? compositor::node_kind_name(node.kind)
                           : std::string_view{node.label};
    const std::string source =
        node.kind == compositor::NodeKind::SceneLayer
            ? compositor_node_detail(node, elements)
            : std::string{};
    float height = compositor_node_height(metrics_, node.kind);
    if (collapsed(node)) {
      std::size_t inputs_shown{};
      std::size_t outputs_shown{};
      for (const auto &port : compositor::input_ports(node.kind)) {
        inputs_shown += port_row(compositor_graph, node, port.index, true)
                            .has_value()
                            ? 1U
                            : 0U;
      }
      for (const auto &port : compositor::output_ports(node.kind)) {
        outputs_shown += port_row(compositor_graph, node, port.index, false)
                             .has_value()
                             ? 1U
                             : 0U;
      }
      height = metrics_.header_height +
               static_cast<float>(std::max(inputs_shown, outputs_shown)) *
                   metrics_.row_height +
               metrics_.footer_height;
    }
    node_sizes_.push_back(NodeSize{
        node.node, compositor_node_width(ctx, node.kind, title, source),
        height});
  }
  std::sort(node_sizes_.begin(), node_sizes_.end(),
            [](const NodeSize &left, const NodeSize &right) noexcept {
              return left.node < right.node;
            });

  // A released move keeps its position until the graph reports the edit.
  std::erase_if(pending_moves_, [&](PendingMove &held) {
    const compositor::GraphNode *const moved = compositor_graph.node(held.node);
    return moved == nullptr || moved->editor_x != held.from_x ||
           moved->editor_y != held.from_y ||
           ++held.frames > compositor_pending_move_frames;
  });
  if (pending_comment_.has_value()) {
    const compositor::GraphComment *const box =
        comment_of(pending_comment_->held.node);
    if (box == nullptr || *box != pending_comment_->from ||
        ++pending_comment_->frames > compositor_pending_move_frames) {
      pending_comment_.reset();
    }
  }

  if (pending_points_.has_value()) {
    const compositor::GraphEdge *const routed =
        compositor_graph.edge(pending_points_->edge);
    if (routed == nullptr || routed->points != pending_points_->from ||
        ++pending_points_->frames > compositor_pending_move_frames) {
      pending_points_.reset();
    }
  }
  if (point_drag_.has_value() &&
      compositor_graph.edge(point_drag_->edge) == nullptr) {
    point_drag_.reset();
  }
  if (pending_wires_.has_value()) {
    const bool arrived =
        std::all_of(pending_wires_->added.begin(), pending_wires_->added.end(),
                    [&](const CompositorWire &wire) {
                      return std::any_of(
                          compositor_graph.edges.begin(),
                          compositor_graph.edges.end(),
                          [&](const compositor::GraphEdge &edge) noexcept {
                            return edge.from == wire.from && edge.to == wire.to;
                          });
                    }) &&
        std::none_of(pending_wires_->removed.begin(),
                     pending_wires_->removed.end(), [&](doc::NodeId id) {
                       return compositor_graph.edge(id) != nullptr;
                     });
    if (arrived || ++pending_wires_->frames > compositor_pending_move_frames) {
      pending_wires_.reset();
    }
  }

  gui::Rect canvas = content;
  gui::Rect parameter_bounds{};
  // The seam the operator drags to resize Parameters: its left edge beside
  // the graph, or its top edge when the panel is too narrow for both.
  gui::Rect parameter_splitter{};
  bool parameter_side = false;
  bool parameter_resize_pressed = false;
  constexpr float minimum_graph_width = 256.0F;
  constexpr float minimum_graph_height = 72.0F;
  constexpr float minimum_parameter_width = 200.0F;
  constexpr float default_parameter_width = 280.0F;
  constexpr float maximum_parameter_width = 640.0F;
  constexpr float minimum_parameter_height = 96.0F;
  constexpr float splitter_reach = 3.0F;
  if (parameter_node != nullptr) {
    parameter_side =
        content.width >= minimum_graph_width + minimum_parameter_width;
    if (parameter_side) {
      const float requested =
          parameter_width_ > 0.0F ? parameter_width_ : default_parameter_width;
      const float width = std::clamp(
          requested, minimum_parameter_width,
          std::clamp(content.width - minimum_graph_width,
                     minimum_parameter_width, maximum_parameter_width));
      canvas.width = std::max(content.width - width, 0.0F);
      parameter_bounds =
          gui::Rect{canvas.right(), content.y, width, content.height};
      parameter_splitter =
          gui::Rect{parameter_bounds.x - splitter_reach, parameter_bounds.y,
                    splitter_reach * 2.0F, parameter_bounds.height};
    } else {
      const float available =
          std::max(content.height - minimum_graph_height, 0.0F);
      const float requested = parameter_height_ > 0.0F
                                  ? parameter_height_
                                  : parameter_panel_height(*parameter_node);
      const float height = std::min(
          std::max(requested, std::min(minimum_parameter_height, available)),
          available);
      canvas.height = std::max(content.height - height, 0.0F);
      parameter_bounds =
          gui::Rect{content.x, canvas.bottom(), content.width, height};
      parameter_splitter =
          gui::Rect{parameter_bounds.x, parameter_bounds.y - splitter_reach,
                    parameter_bounds.width, splitter_reach * 2.0F};
    }
    // Claimed before the graph so a press on the seam resizes rather than
    // starting a node drag under it.
    const gui::Interaction resize = clipped_grab(
        ctx, gui::ui_id(id_, "compositor.parameters.resize"), parameter_splitter,
        content);
    parameter_resize_pressed = resize.pressed;
    ctx.cursor_for(resize, parameter_side ? gui::Cursor::ResizeHorizontal
                                          : gui::Cursor::ResizeVertical);
    if (resize.held) {
      if (parameter_side) {
        parameter_width_ = std::clamp(
            content.right() - ctx.pointer_x(),
            minimum_parameter_width,
            std::clamp(content.width - minimum_graph_width,
                       minimum_parameter_width, maximum_parameter_width));
      } else {
        parameter_height_ = std::clamp(
            content.bottom() - ctx.pointer_y(),
            std::min(minimum_parameter_height,
                     std::max(content.height - minimum_graph_height, 0.0F)),
            std::max(content.height - minimum_graph_height, 0.0F));
      }
    }
  }
  last_canvas_ = canvas;
  const auto set_zoom = [&](float requested, float anchor_x,
                            float anchor_y) noexcept {
    const float next =
        std::clamp(requested, compositor_minimum_zoom, compositor_maximum_zoom);
    if (next == view_.zoom || canvas.width <= 0.0F || canvas.height <= 0.0F) {
      return;
    }
    view_.zoom_about(canvas, anchor_x, anchor_y, next);
  };

  ctx.push_clip(bounds);
  ctx.panel(bounds, scale(ctx.theme().background, 0.86F));
  ctx.panel(toolbar, ctx.theme().background);
  ctx.panel(gui::Rect{toolbar.x, toolbar.bottom() - 1.0F,
                      toolbar.width, 1.0F},
            ctx.theme().divider);
  const std::size_t output_count = static_cast<std::size_t>(std::count_if(
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
      set_zoom(view_.zoom / compositor_zoom_step, anchor_x, anchor_y);
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
      set_zoom(1.0F, anchor_x, anchor_y);
    }
    if (navigation_button("compositor.zoom_in", zoom_in, "+",
                          gui::Icon::Plus, "Zoom in")) {
      set_zoom(view_.zoom * compositor_zoom_step, anchor_x, anchor_y);
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

  const auto palette_matches = [&](const CompositorPaletteItem &item) {
    const std::string_view category =
        compositor_category_names[category_index(item.category)];
    return matches(compositor::node_kind_name(item.kind), node_search_) ||
           matches(category, node_search_) ||
           matches(item.search_terms, node_search_);
  };
  const auto palette_matches_text = [&](const CompositorPaletteItem &item,
                                        std::string_view search) {
    const std::string_view category =
        compositor_category_names[category_index(item.category)];
    return matches(compositor::node_kind_name(item.kind), search) ||
           matches(category, search) || matches(item.search_terms, search);
  };
  const auto palette_direct_matches = [&](const CompositorPaletteItem &item) {
    return matches(compositor::node_kind_name(item.kind), node_search_) ||
           matches(item.search_terms, node_search_);
  };
  std::size_t palette_item_rows{};
  if (!node_search_.empty()) {
    palette_item_rows = static_cast<std::size_t>(
        std::count_if(compositor_palette_items.begin(),
                      compositor_palette_items.end(), palette_matches));
  } else if (add_category_.has_value()) {
    palette_item_rows = static_cast<std::size_t>(std::count_if(
        compositor_palette_items.begin(), compositor_palette_items.end(),
        [&](const CompositorPaletteItem &item) {
          return category_index(item.category) == *add_category_;
        }));
  }
  constexpr float palette_search_height = 31.0F;
  constexpr float palette_row_height = 23.0F;
  const std::size_t palette_category_rows =
      node_search_.empty() ? compositor_category_names.size() : 0U;
  const float desired_palette_height =
      8.0F + palette_search_height +
      static_cast<float>(palette_category_rows + palette_item_rows) *
          palette_row_height;
  const float palette_width =
      std::min(292.0F, std::max(bounds.width - 16.0F, 0.0F));
  const gui::Rect palette_bounds{
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

  // The node search at the pointer: what it offers and where it sits. With a
  // wire attached it offers only nodes with an input that wire can feed,
  // each naming that input.
  struct MenuItem final {
    const CompositorPaletteItem *item{};
    std::optional<core::u8> input;
  };
  std::vector<MenuItem> menu_items;
  gui::Rect menu_bounds{};
  constexpr float menu_row_height = 23.0F;
  constexpr std::size_t menu_visible_rows = 12U;
  if (node_menu_.has_value()) {
    for (std::size_t pass = 0U; pass < 2U; ++pass) {
      for (const CompositorPaletteItem &item : compositor_palette_items) {
        const bool direct =
            matches(compositor::node_kind_name(item.kind), node_menu_->search) ||
            matches(item.search_terms, node_menu_->search);
        if (!palette_matches_text(item, node_menu_->search) ||
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

  // The right-click menu: what it offers for what was clicked, and where.
  // Moves a wire to `target`'s place in their input's order; the wires
  // between shift up or down one. The whole input is numbered afresh.
  const auto move_in_order = [&](doc::NodeId moved, doc::NodeId target) {
    const compositor::GraphEdge *const edge = compositor_graph.edge(moved);
    if (edge == nullptr || moved == target) {
      return;
    }
    std::vector<const compositor::GraphEdge *> chain =
        input_chain(compositor_graph, edge->to);
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
      if (edge_order(*chain[index]) == order) {
        continue;
      }
      reorder.orders.emplace_back(chain[index]->node, order);
      std::erase_if(pending_order_, [&](const auto &held) noexcept {
        return held.first == chain[index]->node;
      });
      pending_order_.emplace_back(chain[index]->node, order);
    }
    if (!reorder.orders.empty()) {
      frame.reorder = std::move(reorder);
    }
  };
  // The wire before or after one in its input's order, if any.
  const auto order_neighbour = [&](doc::NodeId id,
                                   int step) -> std::optional<doc::NodeId> {
    const compositor::GraphEdge *const edge = compositor_graph.edge(id);
    if (edge == nullptr) {
      return std::nullopt;
    }
    const std::vector<const compositor::GraphEdge *> chain =
        input_chain(compositor_graph, edge->to);
    const auto at = std::find(chain.begin(), chain.end(), edge);
    const auto index = std::distance(chain.begin(), at) + step;
    if (at == chain.end() || index < 0 ||
        index >= static_cast<std::ptrdiff_t>(chain.size())) {
      return std::nullopt;
    }
    return chain[static_cast<std::size_t>(index)]->node;
  };
  // The selected nodes that can be bypassed, and whether all of them are.
  const auto bypass_targets = [&]() {
    std::vector<doc::NodeId> targets;
    bool all_bypassed = true;
    for (const doc::NodeId id : selected_nodes_) {
      const compositor::GraphNode *const node = compositor_graph.node(id);
      if (node != nullptr && compositor::node_kind_bypassable(node->kind)) {
        targets.push_back(id);
        all_bypassed = all_bypassed && bypassed(*node);
      }
    }
    return std::pair{std::move(targets), all_bypassed};
  };
  // Bypasses the selection's bypassable nodes, or restores them when every
  // one already is.
  const auto toggle_bypass = [&]() {
    auto [targets, all_bypassed] = bypass_targets();
    if (targets.empty()) {
      return;
    }
    const bool bypass = !all_bypassed;
    for (const doc::NodeId target : targets) {
      std::erase_if(pending_bypass_, [target](const auto &held) noexcept {
        return held.first == target;
      });
      pending_bypass_.emplace_back(target, bypass);
    }
    frame.bypass = CompositorBypassIntent{std::move(targets), bypass};
  };
  enum class ContextAction : core::u8 {
    Rename, Duplicate, Copy, Delete,
    AlignLeft, AlignRight, AlignTop, AlignBottom,
    DistributeHorizontal, DistributeVertical,
    Straighten, Arrange, CommentSelection, Collapse, Bypass,
    AddPoint, Disconnect, Colour, MoveEarlier, MoveLater,
  };
  struct ContextItem final {
    std::string label;
    std::string_view shortcut;
    ContextAction action{ContextAction::Rename};
    bool enabled{true};
    core::u8 color{};
    bool divided{};
  };
  std::vector<ContextItem> context_items;
  gui::Rect context_bounds{};
  constexpr float context_row_height = 23.0F;
  constexpr float context_divider = 7.0F;
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
        const auto [targets, all_bypassed] = bypass_targets();
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
      const auto earlier = order_neighbour(context_menu_->target, -1);
      const auto later = order_neighbour(context_menu_->target, 1);
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

  const bool palette_pointer_blocked =
      (add_menu_open_ &&
       palette_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) ||
      (node_menu_.has_value() &&
       menu_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) ||
      (context_menu_.has_value() &&
       context_bounds.contains(ctx.pointer_x(), ctx.pointer_y()));
  if (palette_pointer_blocked) {
    ctx.push_pointer_block();
  }

  if (canvas.width <= 0.0F || canvas.height <= 0.0F) {
    if (palette_pointer_blocked) {
      ctx.pop_pointer_block();
    }
    ctx.pop_clip();
    return frame;
  }

  if (canvas.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      ctx.wheel() != 0.0F && dragging_node_ == doc::NodeId{} &&
      !connecting_from_.has_value() && !relink_.has_value() &&
      !point_drag_.has_value()) {
    set_zoom(view_.zoom * std::pow(compositor_zoom_step, ctx.wheel()),
             ctx.pointer_x(), ctx.pointer_y());
  }
  if (ctx.middle_pointer_pressed() &&
      canvas.contains(ctx.pointer_x(), ctx.pointer_y())) {
    panning_ = true;
  }
  if (panning_ && ctx.middle_pointer_down()) {
    view_.pan_x += ctx.pointer_dx();
    view_.pan_y += ctx.pointer_dy();
  }
  if (ctx.middle_pointer_released()) {
    panning_ = false;
  }

  // An element chosen in another panel brings its Scene Layer - or the one
  // for the Track it plays on - into view, outlined for a moment.
  if (reveal_element_ != doc::NodeId{}) {
    const auto layer_for =
        [&](doc::NodeId scene) -> const compositor::GraphNode * {
      const auto found = std::find_if(
          compositor_graph.nodes.begin(), compositor_graph.nodes.end(),
          [scene](const compositor::GraphNode &node) noexcept {
            const auto *parameters =
                std::get_if<compositor::SceneLayerParameters>(&node.parameters);
            return node.kind == compositor::NodeKind::SceneLayer &&
                   parameters != nullptr && parameters->scene_node == scene;
          });
      return found != compositor_graph.nodes.end() ? std::addressof(*found)
                                                   : nullptr;
    };
    const compositor::GraphNode *layer = layer_for(reveal_element_);
    if (layer == nullptr) {
      const auto element = std::find_if(
          elements.begin(), elements.end(),
          [this](const ElementInfo &info) noexcept {
            return info.node == reveal_element_;
          });
      if (element != elements.end() &&
          element->timeline_lane != doc::NodeId{}) {
        layer = layer_for(element->timeline_lane);
      }
    }
    if (layer != nullptr) {
      const gui::UiPoint size = node_size(*layer);
      const gui::UiPoint at =
          pending_position(layer->node)
              .value_or(gui::UiPoint{layer->editor_x, layer->editor_y});
      const gui::Rect placed{at.x, at.y, size.x, size.y};
      const gui::Rect shown = view_.to_screen(canvas, placed);
      // Already wholly in view, the view stays where the operator left it.
      if (shown.x < canvas.x || shown.y < canvas.y ||
          shown.right() > canvas.right() || shown.bottom() > canvas.bottom()) {
        view_.centre_on(canvas, placed.center_x(), placed.center_y());
      }
      reveal_node_ = layer->node;
      reveal_until_ = reveal_requested_at_ + 1.5;
    }
    reveal_element_ = {};
  }

  std::vector<CompositorNodeVisual> nodes;
  std::vector<CompositorPortVisual> inputs;
  std::vector<CompositorPortVisual> outputs;
  nodes.reserve(compositor_graph.nodes.size());
  inputs.reserve(compositor_graph.nodes.size() * 2U);
  outputs.reserve(compositor_graph.nodes.size());

  const auto build_visuals = [&]() {
    nodes.clear();
    inputs.clear();
    outputs.clear();
    for (const compositor::GraphNode &node : compositor_graph.nodes) {
      float editor_x = node.editor_x;
      float editor_y = node.editor_y;
      if (const std::optional<gui::UiPoint> held = pending_position(node.node)) {
        editor_x = held->x;
        editor_y = held->y;
      }
      const auto member = std::find_if(
          drag_group_.begin(), drag_group_.end(),
          [&node](const GroupMember &entry) noexcept {
            return entry.node == node.node;
          });
      if (dragging_node_ != doc::NodeId{} && member != drag_group_.end()) {
        const auto grabbed = std::find_if(
            drag_group_.begin(), drag_group_.end(),
            [this](const GroupMember &entry) noexcept {
              return entry.node == dragging_node_;
            });
        if (grabbed != drag_group_.end()) {
          editor_x = member->start_x + drag_x_ - grabbed->start_x;
          editor_y = member->start_y + drag_y_ - grabbed->start_y;
        }
      }
      if (comment_drag_.has_value() && !comment_drag_->resizing) {
        const auto carried = std::find_if(
            comment_drag_->carried.begin(), comment_drag_->carried.end(),
            [&node](const GroupMember &entry) noexcept {
              return entry.node == node.node;
            });
        if (carried != comment_drag_->carried.end()) {
          editor_x = carried->start_x + comment_drag_->dx;
          editor_y = carried->start_y + comment_drag_->dy;
        }
      }
      const gui::UiPoint size = node_size(node);
      const gui::Rect node_bounds =
          view_.to_screen(canvas, gui::Rect{editor_x, editor_y, size.x, size.y});
      nodes.push_back(CompositorNodeVisual{std::addressof(node), node_bounds});

      // A port's target is its half of the row - dot and name - reaching a
      // little past the node edge so the dot itself is inside it too.
      const float reach = compositor_port_radius * view_.zoom + 4.0F;
      const auto append_ports =
          [&](std::span<const compositor::PortDescriptor> descriptors,
              std::vector<CompositorPortVisual> &destination, bool input) {
            for (std::size_t index = 0U; index < descriptors.size(); ++index) {
              const compositor::PortDescriptor &descriptor = descriptors[index];
              const std::optional<float> place = port_row(
                  compositor_graph, node, descriptor.index, input);
              if (!place.has_value()) {
                continue;
              }
              const float row_top =
                  node_bounds.y +
                  (metrics_.header_height + metrics_.row_height * *place) *
                      view_.zoom;
              const float row_height = metrics_.row_height * view_.zoom;
              const float x =
                  input ? node_bounds.x : node_bounds.right();
              const gui::Rect hit =
                  input ? gui::Rect{node_bounds.x - reach, row_top,
                                    node_bounds.width * 0.5F + reach, row_height}
                        : gui::Rect{node_bounds.x + node_bounds.width * 0.5F,
                                    row_top, node_bounds.width * 0.5F + reach,
                                    row_height};
              destination.push_back(CompositorPortVisual{
                  compositor::EdgeEndpoint{node.node, descriptor.index},
                  std::addressof(descriptor), x, row_top + row_height * 0.5F,
                  hit});
            }
          };
      append_ports(compositor::input_ports(node.kind), inputs, true);
      append_ports(compositor::output_ports(node.kind), outputs, false);
    }
  };
  build_visuals();

  const auto to_screen = [&](compositor::WirePoint point) noexcept {
    return view_.to_screen(canvas, point.x, point.y);
  };
  const auto to_graph = [&](float x, float y) noexcept {
    const gui::UiPoint at = view_.to_graph(canvas, x, y);
    return compositor::WirePoint{at.x, at.y};
  };
  const auto edges_at = [&](compositor::EdgeEndpoint endpoint, bool source) {
    std::vector<doc::NodeId> found;
    for (const compositor::GraphEdge &edge : compositor_graph.edges) {
      if ((source ? edge.from : edge.to) == endpoint) {
        found.push_back(edge.node);
      }
    }
    return found;
  };
  // Wires lifted by a Ctrl drag do not count as already there while the
  // operator decides where they go.
  const std::span<const doc::NodeId> lifted =
      relink_.has_value() ? std::span<const doc::NodeId>{relink_->edges}
                          : std::span<const doc::NodeId>{};

  // While a wire is being drawn, the input it would connect to: the
  // compatible port whose row is under the pointer, or failing that the
  // nearest compatible dot within snapping reach.
  const auto snapped_input = [&]() -> const CompositorPortVisual * {
    if (!connecting_from_.has_value() ||
        !canvas.contains(ctx.pointer_x(), ctx.pointer_y())) {
      return nullptr;
    }
    const CompositorPortVisual *const from =
        find_port(outputs, *connecting_from_);
    if (from == nullptr) {
      return nullptr;
    }
    const CompositorPortVisual *best = nullptr;
    float best_rank = std::numeric_limits<float>::infinity();
    for (const CompositorPortVisual &to : inputs) {
      if (!can_connect(compositor_graph, *from, to, lifted)) {
        continue;
      }
      const float distance =
          std::hypot(ctx.pointer_x() - to.x, ctx.pointer_y() - to.y);
      const bool over_row = to.hit.contains(ctx.pointer_x(), ctx.pointer_y());
      if (!over_row && distance > compositor_port_snap_radius) {
        continue;
      }
      // A row under the pointer outranks any dot that is merely near.
      const float rank = over_row ? distance - 1.0e6F : distance;
      if (rank < best_rank) {
        best = std::addressof(to);
        best_rank = rank;
      }
    }
    return best;
  };
  // While wires lifted off an output are being moved, the output they would
  // move to: one every lifted wire's input could take.
  const auto snapped_output = [&]() -> const CompositorPortVisual * {
    if (!relink_.has_value() || !relink_->moving_source ||
        !canvas.contains(ctx.pointer_x(), ctx.pointer_y())) {
      return nullptr;
    }
    const CompositorPortVisual *best = nullptr;
    float best_rank = std::numeric_limits<float>::infinity();
    for (const CompositorPortVisual &from : outputs) {
      const bool accepts = std::all_of(
          relink_->edges.begin(), relink_->edges.end(),
          [&](doc::NodeId id) {
            const compositor::GraphEdge *const edge = compositor_graph.edge(id);
            const CompositorPortVisual *const to =
                edge != nullptr ? find_port(inputs, edge->to) : nullptr;
            return to != nullptr &&
                   (from.endpoint == relink_->origin ||
                    can_connect(compositor_graph, from, *to, lifted));
          });
      if (!accepts) {
        continue;
      }
      const float distance =
          std::hypot(ctx.pointer_x() - from.x, ctx.pointer_y() - from.y);
      const bool over_row =
          from.hit.contains(ctx.pointer_x(), ctx.pointer_y());
      if (!over_row && distance > compositor_port_snap_radius) {
        continue;
      }
      const float rank = over_row ? distance - 1.0e6F : distance;
      if (rank < best_rank) {
        best = std::addressof(from);
        best_rank = rank;
      }
    }
    return best;
  };

  bool selection_pressed = false;
  const bool pointer_over_any_node = std::any_of(
      nodes.begin(), nodes.end(), [&ctx](const CompositorNodeVisual &visual) {
        return visual.bounds.contains(ctx.pointer_x(), ctx.pointer_y());
      });
  if ((connecting_from_.has_value() || relink_.has_value()) &&
      ctx.pointer_released()) {
    if (relink_.has_value() && relink_->moving_source) {
      // Let go on another output: the wires move to it. On nothing: they
      // are disconnected. Back on their own output: nothing changes.
      const CompositorPortVisual *const target = snapped_output();
      if (target == nullptr || target->endpoint != relink_->origin) {
        CompositorRewireIntent rewire{relink_->edges, {}};
        if (target != nullptr) {
          for (const doc::NodeId id : relink_->edges) {
            if (const compositor::GraphEdge *const edge =
                    compositor_graph.edge(id)) {
              rewire.add_edges.push_back(
                  CompositorWire{target->endpoint, edge->to});
            }
          }
        }
        frame.rewire = std::move(rewire);
      }
    } else if (connecting_from_.has_value()) {
      const CompositorPortVisual *const from =
          find_port(outputs, *connecting_from_);
      const CompositorPortVisual *const to = snapped_input();
      std::optional<doc::NodeId> occupied;
      if (to != nullptr && to->descriptor != nullptr &&
          !to->descriptor->multiple) {
        const auto wired = std::find_if(
            compositor_graph.edges.begin(), compositor_graph.edges.end(),
            [to](const compositor::GraphEdge &edge) noexcept {
              return edge.to == to->endpoint;
            });
        if (wired != compositor_graph.edges.end()) {
          occupied = wired->node;
        }
      }
      if (relink_.has_value()) {
        // A wire lifted off an input: it lands on another input, or is
        // disconnected when let go on nothing; put back, nothing changes.
        if (to == nullptr || to->endpoint != relink_->origin) {
          CompositorRewireIntent rewire{relink_->edges, {}};
          if (occupied.has_value() &&
              std::find(lifted.begin(), lifted.end(), *occupied) ==
                  lifted.end()) {
            rewire.remove_edges.push_back(*occupied);
          }
          if (from != nullptr && to != nullptr) {
            rewire.add_edges.push_back(
                CompositorWire{from->endpoint, to->endpoint});
          }
          frame.rewire = std::move(rewire);
        }
      } else if (from != nullptr && to != nullptr) {
        frame.connect_ports =
            CompositorConnectIntent{from->endpoint, to->endpoint, occupied};
      } else if (from != nullptr &&
                 canvas.contains(ctx.pointer_x(), ctx.pointer_y()) &&
                 !pointer_over_any_node) {
        // Let go on empty canvas: offer the nodes that can take this wire,
        // to be created here already connected.
        node_menu_ = NodeMenu{ctx.pointer_x(), ctx.pointer_y(),
                              from->endpoint, from->descriptor->type};
      }
    }
    connecting_from_.reset();
    relink_.reset();
  }

  // A comment box as it is drawn: dragged, held after a release, or authored.
  const auto drawn_comment = [&](const compositor::GraphComment &comment) {
    compositor::GraphComment drawn = comment;
    if (pending_comment_.has_value() &&
        pending_comment_->held.node == comment.node) {
      drawn = pending_comment_->held;
    }
    if (comment_drag_.has_value() && comment_drag_->comment == comment.node) {
      drawn = comment_drag_->start;
      if (comment_drag_->resizing) {
        drawn.width = std::max(drawn.width + comment_drag_->dx,
                               compositor::min_comment_extent);
        drawn.height = std::max(drawn.height + comment_drag_->dy,
                                compositor::min_comment_extent);
      } else {
        drawn.x += comment_drag_->dx;
        drawn.y += comment_drag_->dy;
      }
    }
    return drawn;
  };
  // The graph's extent in graph units: every node and comment box, or only
  // the selected nodes.
  const auto graph_extent = [&](bool only_selected) -> std::optional<gui::Rect> {
    std::optional<gui::Rect> extent;
    for (const compositor::GraphNode &node : compositor_graph.nodes) {
      if (only_selected &&
          !std::binary_search(selected_nodes_.begin(), selected_nodes_.end(),
                              node.node)) {
        continue;
      }
      const std::optional<gui::UiPoint> held = pending_position(node.node);
      const gui::UiPoint size = node_size(node);
      const gui::Rect box{held ? held->x : node.editor_x,
                          held ? held->y : node.editor_y, size.x, size.y};
      extent = extent.has_value() ? gui::enclosing(*extent, box) : box;
    }
    if (!only_selected) {
      for (const compositor::GraphComment &comment : compositor_graph.comments) {
        const compositor::GraphComment drawn = drawn_comment(comment);
        const gui::Rect box{drawn.x, drawn.y, drawn.width, drawn.height};
        extent = extent.has_value() ? gui::enclosing(*extent, box) : box;
      }
    }
    return extent;
  };
  // The minimap: the whole graph and the visible part of it, bottom-right of
  // the canvas. Pressing or dragging in it centres the view there.
  gui::Rect minimap{};
  gui::Rect minimap_world{};
  float minimap_scale{};
  if (minimap_ && canvas.width >= 360.0F && canvas.height >= 220.0F) {
    const gui::Rect view = view_.visible(canvas);
    const std::optional<gui::Rect> extent = graph_extent(false);
    minimap_world = extent.has_value() ? gui::enclosing(view, *extent) : view;
    constexpr float apron = 60.0F;
    minimap_world = gui::Rect{minimap_world.x - apron, minimap_world.y - apron,
                              minimap_world.width + apron * 2.0F,
                              minimap_world.height + apron * 2.0F};
    minimap = gui::Rect{canvas.right() - 212.0F,
                        canvas.bottom() - 142.0F, 200.0F, 130.0F};
    minimap_scale = std::min(minimap.width / minimap_world.width,
                             minimap.height / minimap_world.height);
    const gui::Interaction interaction =
        clipped_grab(ctx, gui::ui_id(id_, "compositor.minimap"), minimap, canvas);
    if (interaction.pressed) {
      selection_pressed = true;
    }
    if (interaction.held) {
      const float graph_x =
          minimap_world.x + (ctx.pointer_x() - minimap.x) / minimap_scale;
      const float graph_y =
          minimap_world.y + (ctx.pointer_y() - minimap.y) / minimap_scale;
      view_.centre_on(canvas, graph_x, graph_y);
      build_visuals();
    }
  }

  // A wire's order number is pressed to drag it, and let go on another
  // number on the same input to move the wire to that place.
  const auto order_target = [&](float x, float y) -> const OrderDisc * {
    if (!order_drag_.has_value()) {
      return nullptr;
    }
    const auto dragged = std::find_if(
        order_discs_.begin(), order_discs_.end(),
        [this](const OrderDisc &disc) { return disc.edge == *order_drag_; });
    if (dragged == order_discs_.end()) {
      return nullptr;
    }
    for (const OrderDisc &disc : order_discs_) {
      if (disc.edge != dragged->edge && disc.input == dragged->input &&
          std::hypot(x - disc.x, y - disc.y) <= disc.radius + 6.0F) {
        return std::addressof(disc);
      }
    }
    return nullptr;
  };
  for (const OrderDisc &disc : order_discs_) {
    const float reach = disc.radius + 2.0F;
    const gui::Interaction interaction = clipped_grab(
        ctx, gui::ui_id(id_, "compositor.order", disc.edge.value()),
        gui::Rect{disc.x - reach, disc.y - reach, reach * 2.0F, reach * 2.0F},
        canvas);
    ctx.cursor_for(interaction, gui::Cursor::Move);
    if (interaction.pressed) {
      order_drag_ = disc.edge;
      selection_ = CompositorSelection{CompositorSelectionKind::Edge, disc.edge};
      selected_nodes_.clear();
      frame.selection = selection_;
      selection_pressed = true;
    }
    if (interaction.released && order_drag_ == disc.edge) {
      if (const OrderDisc *const target =
              order_target(ctx.pointer_x(), ctx.pointer_y())) {
        move_in_order(disc.edge, target->edge);
      }
      order_drag_.reset();
    }
  }
  if (order_drag_.has_value() && !ctx.pointer_down() &&
      !ctx.pointer_released()) {
    order_drag_.reset();
  }

  bool layout_changed = false;
  bool drag_released = false;
  doc::NodeId released_node{};
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
    const std::vector<doc::NodeId> wired = edges_at(port.endpoint, true);
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
  const auto is_selected = [this](doc::NodeId node) {
    return std::binary_search(selected_nodes_.begin(), selected_nodes_.end(),
                              node);
  };
  const auto add_to_selection = [this](doc::NodeId node) {
    const auto at = std::lower_bound(selected_nodes_.begin(),
                                     selected_nodes_.end(), node);
    if (at == selected_nodes_.end() || *at != node) {
      selected_nodes_.insert(at, node);
    }
  };
  // Where a node is now in graph units: held, if a released move is still
  // on its way, or as authored.
  const auto graph_position = [&](const compositor::GraphNode &node) {
    const std::optional<gui::UiPoint> held = pending_position(node.node);
    return held.has_value() ? *held : gui::UiPoint{node.editor_x, node.editor_y};
  };
  // Collapses or opens nodes: the selection, when the toggled node is in it.
  const auto toggle_collapse = [&](doc::NodeId node, bool fold) {
    std::vector<doc::NodeId> targets =
        is_selected(node) ? selected_nodes_ : std::vector<doc::NodeId>{node};
    for (const doc::NodeId target : targets) {
      std::erase_if(pending_collapse_, [target](const auto &held) noexcept {
        return held.first == target;
      });
      pending_collapse_.emplace_back(target, fold);
    }
    frame.collapse = CompositorCollapseIntent{std::move(targets), fold};
  };
  const bool chrome_readable = view_.zoom >= compositor_chrome_zoom;
  const bool inline_editable = view_.zoom >= compositor_inline_edit_zoom &&
                               !node_menu_.has_value() &&
                               !context_menu_.has_value();
  // The toggle sits at the right end of the title line.
  const auto toggle_bounds = [&](const CompositorNodeVisual &visual) {
    const float side =
        std::max(compositor_text_height(ctx, compositor_title_text), 14.0F) *
        view_.zoom;
    return gui::Rect{visual.bounds.right() - side -
                         4.0F * view_.zoom,
                     visual.bounds.y + 5.0F * view_.zoom, side, side};
  };
  // An inline value's row - below the port rows - and its field.
  const auto inline_row = [&](const CompositorNodeVisual &visual,
                              std::size_t index) {
    const std::size_t port_rows =
        std::max({compositor::input_ports(visual.node->kind).size(),
                  compositor::output_ports(visual.node->kind).size(),
                  std::size_t{1U}});
    return gui::Rect{
        visual.bounds.x,
        visual.bounds.y + (metrics_.header_height +
                           metrics_.row_height *
                               static_cast<float>(port_rows + index)) *
                              view_.zoom,
        visual.bounds.width, metrics_.row_height * view_.zoom};
  };
  const auto inline_field = [&](const gui::Rect &row) {
    gui::Rect inner = gui::inset(
        row, gui::Padding{0.0F, 2.0F * view_.zoom,
                          compositor_pin_inset * view_.zoom, 2.0F * view_.zoom});
    return gui::cut_right(inner, std::min(84.0F * view_.zoom, row.width * 0.5F));
  };
  const auto inside_canvas = [&canvas](const gui::Rect &rect) noexcept {
    return rect.x >= canvas.x && rect.y >= canvas.y &&
           rect.right() <= canvas.right() &&
           rect.bottom() <= canvas.bottom();
  };
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
      const gui::Rect field = inline_field(inline_row(visual, index));
      if (inside_canvas(field) &&
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
          toggle_bounds(visual), canvas);
      if (toggle.pressed) {
        selection_pressed = true;
      }
      if (toggle.clicked) {
        toggle_collapse(visual.node->node, !collapsed(*visual.node));
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
        if (!is_selected(visual.node->node)) {
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
          input != inputs.end() ? edges_at(input->endpoint, false)
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
        if (is_selected(pressed_node)) {
          std::erase(selected_nodes_, pressed_node);
          selection_ = selected_nodes_.empty()
                           ? CompositorSelection{}
                           : CompositorSelection{CompositorSelectionKind::Node,
                                                 selected_nodes_.front()};
        } else {
          add_to_selection(pressed_node);
          selection_ =
              CompositorSelection{CompositorSelectionKind::Node, pressed_node};
        }
      } else {
        // A press on a node already in a multiple selection keeps it, so the
        // whole selection can be dragged; Shift adds the node to it.
        const bool already = is_selected(pressed_node);
        press_was_selected_ = already && selected_nodes_.size() > 1U;
        if (selection_.kind != CompositorSelectionKind::Node) {
          selected_nodes_.clear();
        }
        if (ctx.shift_held()) {
          add_to_selection(pressed_node);
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
              const gui::UiPoint at = graph_position(*node);
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
  const float comment_header = std::max(metrics_.row_height, 24.0F) * view_.zoom;
  for (auto comment = compositor_graph.comments.rbegin();
       comment != compositor_graph.comments.rend(); ++comment) {
    const compositor::GraphComment drawn = drawn_comment(*comment);
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
            const gui::UiPoint at = graph_position(node);
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
        const compositor::GraphComment moved = drawn_comment(*comment);
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
    build_visuals();
  }

  // Each wire's curve, once, for both picking and drawing: from its output
  // through its reroute points to its input.
  struct EdgeVisual final {
    const compositor::GraphEdge *edge{};
    const CompositorPortVisual *from{};
    const CompositorPortVisual *to{};
    std::vector<compositor::WirePoint> points;
    std::vector<WirePath> segments;
  };
  const auto edge_points =
      [&](const compositor::GraphEdge &edge) -> std::vector<compositor::WirePoint> {
    if (point_drag_.has_value() && point_drag_->edge == edge.node) {
      return point_drag_->points;
    }
    if (pending_points_.has_value() && pending_points_->edge == edge.node) {
      return pending_points_->points;
    }
    return edge.points;
  };
  std::vector<EdgeVisual> edges;
  edges.reserve(compositor_graph.edges.size());
  for (const compositor::GraphEdge &edge : compositor_graph.edges) {
    const CompositorPortVisual *const from = find_port(outputs, edge.from);
    const CompositorPortVisual *const to = find_port(inputs, edge.to);
    if (from == nullptr || to == nullptr || from->descriptor == nullptr ||
        to->descriptor == nullptr ||
        from->descriptor->type != to->descriptor->type) {
      continue;
    }
    std::vector<compositor::WirePoint> points = edge_points(edge);
    std::vector<gui::UiPoint> screen;
    screen.reserve(points.size());
    for (const compositor::WirePoint point : points) {
      screen.push_back(to_screen(point));
    }
    edges.push_back(EdgeVisual{std::addressof(edge), from, to,
                               std::move(points),
                               routed_wire(gui::UiPoint{from->x, from->y},
                                           screen,
                                           gui::UiPoint{to->x, to->y}, view_.zoom)});
  }
  // Distance from the pointer to a wire, and the stretch it is nearest.
  const auto nearest_segment = [&](const EdgeVisual &visual) {
    float nearest = std::numeric_limits<float>::infinity();
    std::size_t segment{};
    for (std::size_t index = 0U; index < visual.segments.size(); ++index) {
      const float distance = wire_distance(visual.segments[index],
                                           ctx.pointer_x(), ctx.pointer_y());
      if (distance < nearest) {
        nearest = distance;
        segment = index;
      }
    }
    return std::pair{nearest, segment};
  };

  // A node being dragged that is free - no wires of its own - and has an
  // input and an output of the same kind can be dropped onto a wire of that
  // kind to splice itself in.
  const EdgeVisual *insert_edge = nullptr;
  std::optional<std::pair<core::u8, core::u8>> insert_ports;
  if (dragging_node_ != doc::NodeId{} && drag_group_.size() == 1U) {
    const compositor::GraphNode *const dragged =
        compositor_graph.node(dragging_node_);
    const bool free =
        dragged != nullptr &&
        std::none_of(compositor_graph.edges.begin(),
                     compositor_graph.edges.end(),
                     [&](const compositor::GraphEdge &edge) noexcept {
                       return edge.from.node == dragging_node_ ||
                              edge.to.node == dragging_node_;
                     });
    float nearest = 12.0F;
    for (const EdgeVisual &visual : edges) {
      if (!free) {
        break;
      }
      const compositor::PortValueType type = visual.from->descriptor->type;
      const auto in = std::find_if(
          compositor::input_ports(dragged->kind).begin(),
          compositor::input_ports(dragged->kind).end(),
          [type](const compositor::PortDescriptor &port) noexcept {
            return port.type == type;
          });
      const auto out = std::find_if(
          compositor::output_ports(dragged->kind).begin(),
          compositor::output_ports(dragged->kind).end(),
          [type](const compositor::PortDescriptor &port) noexcept {
            return port.type == type;
          });
      if (in == compositor::input_ports(dragged->kind).end() ||
          out == compositor::output_ports(dragged->kind).end()) {
        continue;
      }
      const float distance = nearest_segment(visual).first;
      if (distance <= nearest) {
        nearest = distance;
        insert_edge = std::addressof(visual);
        insert_ports = std::pair{in->index, out->index};
      }
    }
  }
  if (released_node != doc::NodeId{} && insert_edge != nullptr &&
      insert_ports.has_value()) {
    frame.rewire = CompositorRewireIntent{
        {insert_edge->edge->node},
        {CompositorWire{insert_edge->edge->from,
                        compositor::EdgeEndpoint{released_node,
                                                 insert_ports->first}},
         CompositorWire{compositor::EdgeEndpoint{released_node,
                                                 insert_ports->second},
                        insert_edge->edge->to}}};
  }

  // Reroute points: drag to move, double-click to remove. Each is claimed
  // before the wires so a press on one never selects the wire under it.
  const float point_radius = std::clamp(5.0F * view_.zoom, 4.0F, 7.0F);
  const auto emit_points = [&](doc::NodeId edge,
                               std::vector<compositor::WirePoint> points,
                               const std::vector<compositor::WirePoint> &from) {
    frame.waypoints = CompositorWaypointIntent{edge, points};
    pending_points_ = PendingPoints{edge, std::move(points), from, 0U};
  };
  for (const EdgeVisual &visual : edges) {
    for (std::size_t index = 0U; index < visual.points.size(); ++index) {
      const gui::UiPoint centre = to_screen(visual.points[index]);
      const gui::Rect handle{centre.x - point_radius - 2.0F,
                             centre.y - point_radius - 2.0F,
                             (point_radius + 2.0F) * 2.0F,
                             (point_radius + 2.0F) * 2.0F};
      const gui::Interaction interaction = clipped_grab(
          ctx,
          gui::ui_id(id_, "compositor.wire_point",
              visual.edge->node.value() * 32U + index),
          handle, canvas);
      ctx.cursor_for(interaction, gui::Cursor::Move);
      if (interaction.pressed) {
        selection_ =
            CompositorSelection{CompositorSelectionKind::Edge, visual.edge->node};
        selected_nodes_.clear();
        frame.selection = selection_;
        selection_pressed = true;
        if (clicks_.double_click(visual.edge->node.value() * 64U + 33U + index,
                                 ctx.time_seconds())) {
          std::vector<compositor::WirePoint> remaining = visual.points;
          remaining.erase(remaining.begin() +
                          static_cast<std::ptrdiff_t>(index));
          emit_points(visual.edge->node, std::move(remaining),
                      visual.edge->points);
        } else {
          point_drag_ = PointDrag{visual.edge->node, index, visual.points};
        }
      }
      if (interaction.held && point_drag_.has_value() &&
          point_drag_->edge == visual.edge->node &&
          point_drag_->index == index) {
        point_drag_->points[index] = to_graph(ctx.pointer_x(), ctx.pointer_y());
        if (interaction.released) {
          if (point_drag_->points != visual.edge->points) {
            emit_points(visual.edge->node, point_drag_->points,
                        visual.edge->points);
          }
          point_drag_.reset();
        }
      }
    }
  }

  // The wire nearest the pointer, within reach and not under a node, which
  // draws over it. Every wire still takes part in the interaction each
  // frame, so the one that was pressed always sees its release.
  const EdgeVisual *hovered_edge = nullptr;
  std::size_t hovered_segment{};
  const bool wiring = connecting_from_.has_value() || relink_.has_value();
  if (!wiring && dragging_node_ == doc::NodeId{} && !point_drag_.has_value() &&
      !pointer_over_any_node &&
      canvas.contains(ctx.pointer_x(), ctx.pointer_y())) {
    float nearest = compositor_wire_hit_radius;
    for (const EdgeVisual &visual : edges) {
      const auto [distance, segment] = nearest_segment(visual);
      if (distance <= nearest) {
        nearest = distance;
        hovered_edge = std::addressof(visual);
        hovered_segment = segment;
      }
    }
  }
  for (const EdgeVisual &visual : edges) {
    const bool hovered = std::addressof(visual) == hovered_edge;
    const gui::Rect pick =
        hovered ? gui::Rect{ctx.pointer_x() - 1.0F, ctx.pointer_y() - 1.0F,
                            2.0F, 2.0F}
                : gui::Rect{};
    const gui::Interaction interaction = ctx.grab(
        gui::ui_id(id_, "compositor.edge", visual.edge->node.value()), pick);
    ctx.cursor_for(interaction, gui::Cursor::Hand);
    if (!interaction.pressed) {
      continue;
    }
    selection_ =
        CompositorSelection{CompositorSelectionKind::Edge, visual.edge->node};
    selected_nodes_.clear();
    frame.selection = selection_;
    selection_pressed = true;
    if (ctx.alt_held()) {
      // Alt-click a wire to break it.
      frame.rewire = CompositorRewireIntent{{visual.edge->node}, {}};
    } else if (clicks_.double_click(visual.edge->node.value() * 64U,
                                    ctx.time_seconds()) &&
               visual.points.size() < compositor::max_wire_points) {
      // Double-click a wire to add a reroute point where it was clicked, in
      // the stretch it was clicked on.
      std::vector<compositor::WirePoint> points = visual.points;
      points.insert(points.begin() +
                        static_cast<std::ptrdiff_t>(hovered_segment),
                    to_graph(ctx.pointer_x(), ctx.pointer_y()));
      emit_points(visual.edge->node, std::move(points), visual.edge->points);
    }
  }
  // A right click on a node, a wire or a comment box's title opens its menu;
  // anywhere else on the canvas, like Tab, it opens the node search.
  if (!wiring && canvas.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      ctx.secondary_pointer_pressed() && !minimap.contains(ctx.pointer_x(),
                                                           ctx.pointer_y())) {
    node_menu_.reset();
    const auto node_under = std::find_if(
        nodes.rbegin(), nodes.rend(), [&ctx](const CompositorNodeVisual &visual) {
          return visual.bounds.contains(ctx.pointer_x(), ctx.pointer_y());
        });
    const auto comment_under = std::find_if(
        compositor_graph.comments.rbegin(), compositor_graph.comments.rend(),
        [&](const compositor::GraphComment &comment) {
          const compositor::GraphComment drawn = drawn_comment(comment);
          // Its title strip, which keeps its height at any zoom.
          gui::Rect title = view_.to_screen(
              canvas, gui::Rect{drawn.x, drawn.y, drawn.width, drawn.height});
          title.height = comment_header;
          return title.contains(ctx.pointer_x(), ctx.pointer_y());
        });
    if (node_under != nodes.rend()) {
      if (!is_selected(node_under->node->node)) {
        select_only(node_under->node->node);
      }
      selection_ = CompositorSelection{CompositorSelectionKind::Node,
                                       node_under->node->node};
      frame.selection = selection_;
      context_menu_ = ContextMenu{ctx.pointer_x(), ctx.pointer_y(),
                                  CompositorSelectionKind::Node,
                                  node_under->node->node};
    } else if (hovered_edge != nullptr) {
      selection_ = CompositorSelection{CompositorSelectionKind::Edge,
                                       hovered_edge->edge->node};
      selected_nodes_.clear();
      frame.selection = selection_;
      context_menu_ = ContextMenu{ctx.pointer_x(), ctx.pointer_y(),
                                  CompositorSelectionKind::Edge,
                                  hovered_edge->edge->node,
                                  to_graph(ctx.pointer_x(), ctx.pointer_y()),
                                  hovered_segment};
    } else if (comment_under != compositor_graph.comments.rend()) {
      selection_ = CompositorSelection{CompositorSelectionKind::Comment,
                                       comment_under->node};
      selected_nodes_.clear();
      frame.selection = selection_;
      context_menu_ = ContextMenu{ctx.pointer_x(), ctx.pointer_y(),
                                  CompositorSelectionKind::Comment,
                                  comment_under->node};
    } else {
      context_menu_.reset();
      node_menu_ = NodeMenu{ctx.pointer_x(), ctx.pointer_y()};
    }
  }
  if (!wiring && canvas.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      focused_ && !ctx.typing() && ctx.key_pressed(gui::Key::Tab)) {
    node_menu_ = NodeMenu{ctx.pointer_x(), ctx.pointer_y()};
  }

  // A press on empty canvas clears the selection and starts a rubber band;
  // with Shift the band adds to the selection, with Ctrl it takes from it.
  if (ctx.pointer_pressed() &&
      canvas.contains(ctx.pointer_x(), ctx.pointer_y()) && !selection_pressed &&
      !parameter_resize_pressed) {
    const Marquee::Mode mode = ctx.shift_held()  ? Marquee::Mode::Add
                               : ctx.ctrl_held() ? Marquee::Mode::Subtract
                                                 : Marquee::Mode::Replace;
    if (mode == Marquee::Mode::Replace) {
      selection_ = CompositorSelection{};
      selected_nodes_.clear();
      frame.selection = selection_;
    }
    marquee_ = Marquee{ctx.pointer_x(), ctx.pointer_y(), mode};
  }
  if (marquee_.has_value() && !ctx.pointer_down()) {
    // Dragged rightwards, the band takes the nodes it wholly contains;
    // leftwards, every node it touches.
    const float left = std::min(marquee_->start_x, ctx.pointer_x());
    const float top = std::min(marquee_->start_y, ctx.pointer_y());
    const gui::Rect band{left, top,
                         std::abs(ctx.pointer_x() - marquee_->start_x),
                         std::abs(ctx.pointer_y() - marquee_->start_y)};
    const bool containing = ctx.pointer_x() >= marquee_->start_x;
    if (band.width > 3.0F || band.height > 3.0F) {
      for (const CompositorNodeVisual &visual : nodes) {
        const gui::Rect &b = visual.bounds;
        const bool inside =
            containing
                ? b.x >= band.x && b.y >= band.y &&
                      b.right() <= band.right() &&
                      b.bottom() <= band.bottom()
                : b.x < band.right() && band.x < b.right() &&
                      b.y < band.bottom() && band.y < b.bottom();
        if (!inside) {
          continue;
        }
        if (marquee_->mode == Marquee::Mode::Subtract) {
          std::erase(selected_nodes_, visual.node->node);
        } else {
          add_to_selection(visual.node->node);
        }
      }
      if (selection_.kind != CompositorSelectionKind::Node ||
          !is_selected(selection_.node)) {
        selection_ = selected_nodes_.empty()
                         ? CompositorSelection{}
                         : CompositorSelection{CompositorSelectionKind::Node,
                                               selected_nodes_.front()};
      }
      frame.selection = selection_;
    }
    marquee_.reset();
  }

  // --- Layout tools, shared by the keyboard and the right-click menu. ---
  // The selected nodes' boxes in graph units.
  const auto selected_boxes = [&]() {
    std::vector<LayoutBox> boxes;
    for (const doc::NodeId id : selected_nodes_) {
      if (const compositor::GraphNode *const node = compositor_graph.node(id)) {
        const gui::UiPoint at = graph_position(*node);
        const gui::UiPoint size = node_size(*node);
        boxes.push_back(LayoutBox{id, at.x, at.y, size.x, size.y});
      }
    }
    return boxes;
  };
  // Authors moves as one step, held on screen until they arrive.
  const auto apply_moves = [&](const std::vector<CompositorMoveIntent> &moves) {
    for (const CompositorMoveIntent &move : moves) {
      const compositor::GraphNode *const node = compositor_graph.node(move.node);
      if (node == nullptr) {
        continue;
      }
      frame.move_nodes.push_back(move);
      std::erase_if(pending_moves_, [&move](const PendingMove &held) {
        return held.node == move.node;
      });
      pending_moves_.push_back(PendingMove{move.node, move.editor_x,
                                           move.editor_y, node->editor_x,
                                           node->editor_y, 0U});
    }
  };
  // Fits a graph-space rectangle in the canvas, no closer than 150%.
  const auto frame_view = [&](const gui::Rect &target) {
    constexpr float margin = 48.0F;
    const float fit = std::min(
        (canvas.width - margin * 2.0F) / std::max(target.width, 1.0F),
        (canvas.height - margin * 2.0F) / std::max(target.height, 1.0F));
    view_.zoom = std::clamp(fit, compositor_minimum_zoom, 1.5F);
    view_.centre_on(canvas, target.center_x(), target.center_y());
  };
  // Straighten Connection: each selected node moves up or down so the wire
  // into its first connected input runs level; a node with no input wire
  // levels the wire out of its first connected output instead. Left to
  // right, so a straightened chain stays straight.
  const auto straightened = [&]() {
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
      return top_of(node) + metrics_.header_height +
             metrics_.row_height * (row + 0.5F);
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
          y = port_y(*source,
                     port_row(compositor_graph, *source, in->from.port, false)
                         .value_or(0.0F)) -
              metrics_.header_height -
              metrics_.row_height *
                  (port_row(compositor_graph, *node, in->to.port, true)
                       .value_or(0.0F) +
                   0.5F);
        }
      } else if (out != nullptr) {
        if (const compositor::GraphNode *const target =
                compositor_graph.node(out->to.node)) {
          y = port_y(*target,
                     port_row(compositor_graph, *target, out->to.port, true)
                         .value_or(0.0F)) -
              metrics_.header_height -
              metrics_.row_height *
                  (port_row(compositor_graph, *node, out->from.port, false)
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
  };
  // Arrange: columns by how many wires stand between a node and the end of
  // its chain, right to left toward Output, each column ordered to follow
  // what it feeds. The selection's top-right corner stays put. With fewer
  // than two nodes selected it arranges the whole graph.
  const auto arranged = [&]() {
    std::vector<LayoutBox> boxes = selected_boxes();
    if (boxes.size() < 2U) {
      boxes.clear();
      for (const compositor::GraphNode &node : compositor_graph.nodes) {
        const gui::UiPoint at = graph_position(node);
        const gui::UiPoint size = node_size(node);
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
  };
  // A comment box around the selected nodes, with room for its title.
  const auto comment_selection = [&]() {
    if (const std::optional<gui::Rect> around = graph_extent(true)) {
      constexpr float margin = 28.0F;
      frame.comment = CompositorCommentIntent{
          doc::NodeId{},
          around->x - margin,
          around->y - margin - std::max(metrics_.row_height, 24.0F),
          around->width + margin * 2.0F,
          around->height + margin * 2.0F + std::max(metrics_.row_height, 24.0F),
          "Comment",
          0U};
    }
  };
  // Copy, paste and duplicate: the Director keeps the copied nodes; the ones
  // a paste or duplicate adds are selected when they arrive.
  const auto expect_new_nodes = [&]() {
    PasteBaseline baseline;
    for (const compositor::GraphNode &node : compositor_graph.nodes) {
      baseline.nodes.push_back(node.node);
    }
    std::sort(baseline.nodes.begin(), baseline.nodes.end());
    paste_baseline_ = std::move(baseline);
  };
  const auto say = [&](std::string text) {
    notice_ = std::move(text);
    notice_until_ = ctx.time_seconds() + 3.0;
  };
  const auto unplaceable = [&]() {
    return std::count_if(selected_nodes_.begin(), selected_nodes_.end(),
                         [&](doc::NodeId id) {
                           const compositor::GraphNode *const node =
                               compositor_graph.node(id);
                           return node != nullptr &&
                                  (node->kind == compositor::NodeKind::Output ||
                                   node->kind ==
                                       compositor::NodeKind::SceneLayer);
                         });
  };
  const auto copy_selection = [&]() {
    std::vector<doc::NodeId> chosen = selected_nodes_;
    if (selection_.kind == CompositorSelectionKind::Comment) {
      chosen.push_back(selection_.node);
    }
    if (!chosen.empty()) {
      frame.clipboard = CompositorClipboardIntent{
          CompositorClipboardAction::Copy, std::move(chosen)};
      say("Copied " + std::to_string(frame.clipboard->nodes.size()) +
          (frame.clipboard->nodes.size() == 1U ? " item" : " items"));
    }
  };
  const auto paste_here = [&]() {
    const bool inside = canvas.contains(ctx.pointer_x(), ctx.pointer_y());
    const compositor::WirePoint at =
        to_graph(inside ? ctx.pointer_x() : canvas.x + canvas.width * 0.5F,
                 inside ? ctx.pointer_y() : canvas.y + canvas.height * 0.5F);
    frame.clipboard = CompositorClipboardIntent{
        CompositorClipboardAction::Paste, {}, at.x, at.y};
    expect_new_nodes();
  };
  const auto duplicate_selection = [&]() {
    if (selected_nodes_.empty()) {
      return;
    }
    if (unplaceable() != 0) {
      say("Output and Scene Layers are not duplicated: one per graph, one per "
          "source");
    }
    frame.clipboard = CompositorClipboardIntent{
        CompositorClipboardAction::Duplicate, selected_nodes_, 32.0F, 32.0F};
    expect_new_nodes();
  };
  const auto start_rename = [&]() {
    if (selection_.kind == CompositorSelectionKind::Node) {
      if (const compositor::GraphNode *const node =
              compositor_graph.node(selection_.node)) {
        rename_ = Rename{node->node,
                         node->label.empty()
                             ? std::string{compositor::node_kind_name(node->kind)}
                             : node->label};
      }
    } else if (selection_.kind == CompositorSelectionKind::Comment) {
      if (const compositor::GraphComment *const comment =
              comment_of(selection_.node)) {
        rename_ = Rename{comment->node, comment->title};
      }
    }
  };

  // The keyboard, while the graph has it and no title is being typed.
  const bool pointer_in_canvas =
      canvas.contains(ctx.pointer_x(), ctx.pointer_y());
  if (focused_ && !ctx.typing() && !rename_.has_value() && !wiring &&
      dragging_node_ == doc::NodeId{} && !node_menu_.has_value()) {
    const std::string_view typed =
        pointer_in_canvas ? ctx.shortcut_text() : std::string_view{};
    if (ctx.key_pressed(gui::Key::Copy)) {
      copy_selection();
    } else if (ctx.key_pressed(gui::Key::Paste)) {
      paste_here();
    } else if (ctx.key_pressed(gui::Key::Duplicate)) {
      duplicate_selection();
    }
    if (ctx.key_pressed(gui::Key::Rename)) {
      start_rename();
    }
    // F frames the selection (or the graph), Home the whole graph.
    if (typed.find_first_of("fF") != std::string_view::npos) {
      std::optional<gui::Rect> target = graph_extent(true);
      if (!target.has_value()) {
        target = graph_extent(false);
      }
      if (target.has_value()) {
        frame_view(*target);
        build_visuals();
      }
    }
    if (ctx.key_pressed(gui::Key::Home)) {
      if (const std::optional<gui::Rect> target = graph_extent(false)) {
        frame_view(*target);
        build_visuals();
      }
    }
    if (typed.find_first_of("qQ") != std::string_view::npos) {
      apply_moves(straightened());
    }
    // D bypasses the selection, or restores it, as Disable does in other
    // compositors.
    if (typed.find_first_of("dD") != std::string_view::npos &&
        !ctx.ctrl_held()) {
      toggle_bypass();
    }
    if (typed.find_first_of("cC") != std::string_view::npos &&
        !selected_nodes_.empty()) {
      comment_selection();
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
      for (const LayoutBox &box : selected_boxes()) {
        moves.push_back(
            CompositorMoveIntent{box.node, box.x + nudge_x, box.y + nudge_y});
      }
      apply_moves(moves);
      const bool horizontal = nudge_x != 0.0F;
      frame.numeric_nudge = NumericNudge{
          gui::ui_id(id_, "compositor.nudge"), horizontal ? 1001U : 1002U,
          (horizontal ? nudge_x : nudge_y) > 0.0F
              ? NumberFieldNudgeDirection::Increase
              : NumberFieldNudgeDirection::Decrease,
          ctx.shift_held() ? gui::KeyMods::Shift : gui::KeyMods::None};
      build_visuals();
    }
  }

  ctx.push_clip(canvas);
  ctx.panel(canvas, scale(ctx.theme().background, 0.84F));
  // A quiet drafting-grid texture identifies this as a spatial graph surface
  // without competing with wires or implying rigid row/column alignment.
  const float dot_spacing = 32.0F * view_.zoom;
  constexpr float dot_size = 1.5F;
  const gui::UiColor dot_color = with_alpha(ctx.theme().divider, 0.42F);
  float first_dot_x =
      canvas.x + std::fmod(view_.pan_x + dot_spacing * 0.5F, dot_spacing);
  float first_dot_y =
      canvas.y + std::fmod(view_.pan_y + dot_spacing * 0.5F, dot_spacing);
  if (first_dot_x < canvas.x) {
    first_dot_x += dot_spacing;
  }
  if (first_dot_y < canvas.y) {
    first_dot_y += dot_spacing;
  }
  for (float y = first_dot_y; y < canvas.bottom(); y += dot_spacing) {
    for (float x = first_dot_x; x < canvas.right(); x += dot_spacing) {
      ctx.draw_list().add_rect(x - dot_size * 0.5F, y - dot_size * 0.5F,
                               dot_size, dot_size, dot_color);
    }
  }

  // Guidance sits behind graph content: it remains available in empty/error
  // states, while nodes and wires always paint over it and therefore never
  // become obscured by instructional chrome.
  const auto first_problem =
      std::find_if(compositor_graph.diagnostics.begin(),
                   compositor_graph.diagnostics.end(), is_problem);
  const bool graph_problem = first_problem != compositor_graph.diagnostics.end();
  // The Output is waiting for its image, so the graph composites nothing and
  // Program shows the scene as authored.
  const auto output_node = std::find_if(
      compositor_graph.nodes.begin(), compositor_graph.nodes.end(),
      [](const compositor::GraphNode &node) noexcept {
        return node.kind == compositor::NodeKind::Output;
      });
  const bool composites_nothing =
      output_node != compositor_graph.nodes.end() &&
      node_disabled(compositor_graph, output_node->node);
  if (compositor_graph.nodes.empty() || graph_problem || composites_nothing) {
    const bool diagnostic = graph_problem;
    gui::Rect top = gui::inset(canvas, 8.0F);
    gui::Rect guide = gui::cut_top(top, 28.0F);
    guide.width = std::min(guide.width, 660.0F);
    ctx.panel(guide, with_alpha(scale(ctx.theme().header, 0.92F), 0.94F));
    ctx.outline(guide,
                diagnostic ? with_alpha(ctx.theme().warning, 0.72F)
                      : with_alpha(ctx.theme().accent, 0.42F));
    std::string title;
    if (diagnostic) {
      title = first_problem->severity == compositor::DiagnosticSeverity::Error
                  ? "GRAPH ERROR: "
                  : "GRAPH WARNING: ";
    } else {
      title = "START: ";
    }
    title += diagnostic ? compositor_diagnostic_message(*first_problem)
             : compositor_graph.nodes.empty()
                 ? std::string_view{"Add a Layer for each scene object"}
                 : std::string_view{"Wire Layers into Output; until then "
                                    "Program shows the scene as authored"};
    ctx.label(gui::inset(guide, 8.0F, 3.0F), title,
              diagnostic ? ctx.theme().warning : ctx.theme().text_muted);
    ctx.tooltip(guide, title + " · Connect the output on the right of a node to an input on the left. Layers feed Composite, then Output.");
  }

  for (const compositor::GraphComment &comment : compositor_graph.comments) {
    const compositor::GraphComment drawn = drawn_comment(comment);
    const gui::Rect box = view_.to_screen(
        canvas, gui::Rect{drawn.x, drawn.y, drawn.width, drawn.height});
    const bool chosen = selection_ == CompositorSelection{
                                          CompositorSelectionKind::Comment,
                                          comment.node};
    const gui::UiColor color = comment_color(drawn.color);
    ctx.panel(box, with_alpha(color, 0.07F));
    gui::Rect below_title = box;
    const gui::Rect title = gui::cut_top(below_title, comment_header);
    ctx.panel(title, with_alpha(blend_color(ctx.theme().header, color, 0.35F),
                                0.92F));
    ctx.outline(box, chosen ? ctx.theme().accent : with_alpha(color, 0.55F),
                chosen ? 2.0F : 1.0F);
    static_cast<void>(compositor_label(
        ctx,
        gui::Rect{title.x + 8.0F * view_.zoom, title.y,
                  std::max(title.width - 16.0F * view_.zoom, 0.0F), title.height},
        drawn.title, ctx.theme().text, compositor_title_text, view_.zoom));
    // The corner grip it resizes by.
    const float grip_x = box.right() - 4.0F;
    const float grip_y = box.bottom() - 4.0F;
    for (float offset = 4.0F; offset <= 10.0F; offset += 3.0F) {
      ctx.draw_list().add_line(grip_x - offset, grip_y, grip_x, grip_y - offset,
                               1.0F, with_alpha(color, 0.8F));
    }
  }

  // Wires scale with the graph but never thin below what reads on screen.
  const float wire_width = std::clamp(2.0F * view_.zoom, 1.5F, 3.5F);
  // A selected node's flow stands out: every wire feeding it, directly or
  // further upstream, and every wire it feeds on its way to Output.
  std::set<doc::NodeId> upstream;
  std::set<doc::NodeId> downstream;
  if (selection_.kind == CompositorSelectionKind::Node) {
    const auto walk = [&](std::set<doc::NodeId> &reached, bool forward) {
      std::vector<doc::NodeId> pending{selection_.node};
      while (!pending.empty()) {
        const doc::NodeId node = pending.back();
        pending.pop_back();
        if (!reached.insert(node).second) {
          continue;
        }
        for (const compositor::GraphEdge &edge : compositor_graph.edges) {
          if ((forward ? edge.from.node : edge.to.node) == node) {
            pending.push_back(forward ? edge.to.node : edge.from.node);
          }
        }
      }
    };
    walk(upstream, false);
    walk(downstream, true);
  }
  const bool wires_held = pending_wires_.has_value();
  drawn_wires_.clear();
  order_discs_.clear();
  const OrderDisc *const order_drop =
      order_target(ctx.pointer_x(), ctx.pointer_y());
  std::optional<std::pair<std::string, gui::UiColor>> dragged_number;
  // A small numbered disc on a wire, or a problem mark.
  const auto wire_disc = [&](gui::UiPoint at, std::string_view text,
                             gui::UiColor ring, gui::UiColor fill) {
    const float radius = std::max(6.0F, 7.0F * view_.zoom);
    gui::add_disc(ctx.draw_list(), gui::UiPoint{at.x, at.y},
                  radius + 1.0F, ring);
    gui::add_disc(ctx.draw_list(), gui::UiPoint{at.x, at.y},
                  radius, fill);
    const gui::Rect disc = gui::around(at.x, at.y, radius * 2.0F, radius * 2.0F);
    ctx.label(gui::centered(disc, ctx.font_width(text, 0.75F) + 2.0F,
                            disc.height),
              text, ctx.theme().text, 0.75F);
    return disc;
  };
  // The point a distance back along a wire from its input end.
  const auto back_along = [](std::span<const gui::UiPoint> points,
                             float distance) {
    for (std::size_t index = points.size(); index > 1U; --index) {
      const gui::UiPoint a = points[index - 2U];
      const gui::UiPoint b = points[index - 1U];
      const float length = std::hypot(b.x - a.x, b.y - a.y);
      if (length >= distance && length > 0.0F) {
        const float t = distance / length;
        return gui::UiPoint{b.x + (a.x - b.x) * t, b.y + (a.y - b.y) * t};
      }
      distance -= length;
    }
    return points.empty() ? gui::UiPoint{} : points.front();
  };
  for (const EdgeVisual &visual : edges) {
    DrawnWire &drawn = drawn_wires_.emplace_back(
        DrawnWire{visual.edge->node, visual.from->descriptor->type,
                  visual.edge->to, {}});
    for (const WirePath &segment : visual.segments) {
      const std::span<const gui::UiPoint> run = segment.view();
      drawn.points.insert(drawn.points.end(),
                          run.begin() + (drawn.points.empty() || run.empty()
                                             ? 0
                                             : 1),
                          run.end());
    }
    if (std::find(lifted.begin(), lifted.end(), visual.edge->node) !=
        lifted.end()) {
      continue; // drawn following the pointer below
    }
    if (wires_held &&
        std::find(pending_wires_->removed.begin(),
                  pending_wires_->removed.end(),
                  visual.edge->node) != pending_wires_->removed.end()) {
      continue; // removed, and on its way out of the graph
    }
    const bool selected =
        selection_ ==
        CompositorSelection{CompositorSelectionKind::Edge, visual.edge->node};
    const bool in_flow = (upstream.contains(visual.edge->from.node) &&
                          upstream.contains(visual.edge->to.node)) ||
                         (downstream.contains(visual.edge->from.node) &&
                          downstream.contains(visual.edge->to.node));
    const bool hovered = std::addressof(visual) == hovered_edge;
    const bool splice = std::addressof(visual) == insert_edge;
    const gui::UiColor color = port_color(ctx, visual.from->descriptor->type);
    // A bypassed or disabled node's wire carries nothing, and says so by
    // fading.
    const compositor::GraphNode *const wire_source =
        compositor_graph.node(visual.edge->from.node);
    const bool carries_nothing =
        wire_source != nullptr &&
        (bypassed(*wire_source) ||
         node_disabled(compositor_graph, wire_source->node));
    gui::UiColor stroke = with_alpha(color, 0.78F);
    float width = wire_width;
    if (selected || splice) {
      stroke = ctx.theme().text;
      width = wire_width + 1.5F;
    } else if (carries_nothing) {
      stroke = with_alpha(color, hovered ? 0.55F : 0.3F);
    } else if (hovered || in_flow) {
      stroke = blend_color(color, ctx.theme().text, hovered ? 0.45F : 0.2F);
      width = wire_width + (hovered ? 1.0F : 0.5F);
    }
    for (const WirePath &segment : visual.segments) {
      ctx.draw_list().add_polyline(segment.view(), width, stroke);
    }
    // Reroute points are small handles on the wire itself.
    for (const compositor::WirePoint point : visual.points) {
      const gui::UiPoint centre = to_screen(point);
      gui::add_disc(ctx.draw_list(), gui::UiPoint{centre.x, centre.y},
                    point_radius + 1.0F, scale(ctx.theme().panel, 0.8F));
      gui::add_disc(ctx.draw_list(), gui::UiPoint{centre.x, centre.y},
                    point_radius,
                    selected || hovered ? ctx.theme().text : color);
    }
    if (!chrome_readable || drawn.points.size() < 2U) {
      continue;
    }
    // Where several wires feed one input, each is numbered in the order the
    // input takes them: effects on a layer run first to last.
    const std::vector<const compositor::GraphEdge *> chain =
        input_chain(compositor_graph, visual.edge->to);
    if (chain.size() >= 2U) {
      const auto rank = static_cast<std::size_t>(
          std::distance(chain.begin(),
                        std::find(chain.begin(), chain.end(), visual.edge))) +
                        1U;
      const std::string number = std::to_string(rank);
      const gui::UiPoint at =
          back_along(drawn.points, std::max(30.0F * view_.zoom, 22.0F));
      const float radius = std::max(6.0F, 7.0F * view_.zoom);
      order_discs_.push_back(
          OrderDisc{visual.edge->node, visual.edge->to, at.x, at.y, radius});
      if (order_drag_ == visual.edge->node) {
        // Drawn after the nodes, at the pointer.
        dragged_number = std::pair{number, stroke};
      } else {
        const bool target = order_drop != nullptr &&
                            order_drop->edge == visual.edge->node;
        static_cast<void>(wire_disc(at, number,
                                    target ? ctx.theme().text : stroke,
                                    target ? ctx.theme().accent
                                           : scale(ctx.theme().panel, 1.18F)));
      }
    }
    // A problem with the wire itself is marked halfway along it.
    std::string problems;
    bool error = false;
    for (const compositor::GraphDiagnostic &diagnostic :
         compositor_graph.diagnostics) {
      if (diagnostic.subject != visual.edge->node) {
        continue;
      }
      if (!problems.empty()) {
        problems += " | ";
      }
      problems += compositor_diagnostic_message(diagnostic);
      error = error ||
              diagnostic.severity == compositor::DiagnosticSeverity::Error;
    }
    if (!problems.empty()) {
      const gui::Rect mark = wire_disc(
          drawn.points[drawn.points.size() / 2U], "!", ctx.theme().warning,
          error ? ctx.theme().warning : scale(ctx.theme().panel, 1.18F));
      ctx.tooltip(mark, problems);
    }
  }
  if (wires_held) {
    for (const CompositorWire &held : pending_wires_->added) {
      const bool present = std::any_of(
          compositor_graph.edges.begin(), compositor_graph.edges.end(),
          [&held](const compositor::GraphEdge &edge) noexcept {
            return edge.from == held.from && edge.to == held.to;
          });
      const CompositorPortVisual *const from = find_port(outputs, held.from);
      const CompositorPortVisual *const to = find_port(inputs, held.to);
      if (!present && from != nullptr && to != nullptr) {
        wire(ctx, from->x, from->y, to->x, to->y,
             with_alpha(port_color(ctx, from->descriptor->type), 0.78F),
             wire_width, view_.zoom);
      }
    }
  }
  const CompositorPortVisual *const snapped = snapped_input();
  const CompositorPortVisual *const snapped_source = snapped_output();
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
  const CompositorPortVisual *const connecting_source =
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
    const gui::Rect toggle = toggle_bounds(visual);
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
      const gui::Rect row = inline_row(visual, index);
      const gui::Rect field = inline_field(row);
      const bool idle =
          inline_value_idle(compositor_graph, *visual.node, value);
      static_cast<void>(compositor_label(
          ctx, gui::span_x(row, pins.x, field.x - gap * 0.5F), value.label,
          idle ? with_alpha(ctx.theme().text_muted, 0.5F)
               : ctx.theme().text_muted,
          compositor_pin_text, view_.zoom));
      const double current = inline_value_of(*visual.node, value.property);
      if (idle || !inline_editable || !inside_canvas(field)) {
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
  // An order number being dragged rides on the pointer, over the nodes.
  if (dragged_number.has_value()) {
    static_cast<void>(
        wire_disc(gui::UiPoint{ctx.pointer_x(), ctx.pointer_y()},
                  dragged_number->first, ctx.theme().text,
                  dragged_number->second));
  }
  if (connecting_source != nullptr && snapped == nullptr) {
    const auto under = std::find_if(
        inputs.begin(), inputs.end(), [&ctx](const CompositorPortVisual &port) {
          return port.hit.contains(ctx.pointer_x(), ctx.pointer_y());
        });
    const std::string reason =
        under != inputs.end()
            ? connection_refusal(compositor_graph, *connecting_source, *under,
                                 lifted)
            : std::string{};
    if (!reason.empty()) {
      const float width = std::ceil(ctx.font_width(reason)) + 16.0F;
      const float height = ctx.line_height() + 8.0F;
      const gui::Rect badge{ctx.pointer_x() + 14.0F, ctx.pointer_y() + 10.0F,
                            width, height};
      ctx.panel(badge, with_alpha(ctx.theme().background, 0.94F));
      ctx.outline(badge, ctx.theme().warning);
      ctx.label(gui::Rect{badge.x + 8.0F, badge.y, badge.width - 8.0F,
                          badge.height},
                reason, ctx.theme().warning);
    }
  }
  if (marquee_.has_value() && ctx.pointer_down()) {
    const gui::Rect band{std::min(marquee_->start_x, ctx.pointer_x()),
                         std::min(marquee_->start_y, ctx.pointer_y()),
                         std::abs(ctx.pointer_x() - marquee_->start_x),
                         std::abs(ctx.pointer_y() - marquee_->start_y)};
    ctx.panel(band, with_alpha(ctx.theme().accent, 0.10F));
    ctx.outline(band, with_alpha(ctx.theme().accent, 0.75F));
  }
  if (minimap.width > 0.0F && minimap_scale > 0.0F) {
    const auto map = [&](float x, float y, float width, float height) {
      return gui::Rect{minimap.x + (x - minimap_world.x) * minimap_scale,
                       minimap.y + (y - minimap_world.y) * minimap_scale,
                       std::max(width * minimap_scale, 1.0F),
                       std::max(height * minimap_scale, 1.0F)};
    };
    ctx.panel(minimap, with_alpha(ctx.theme().background, 0.92F));
    for (const compositor::GraphComment &comment : compositor_graph.comments) {
      const compositor::GraphComment drawn = drawn_comment(comment);
      ctx.outline(map(drawn.x, drawn.y, drawn.width, drawn.height),
                  with_alpha(comment_color(drawn.color), 0.7F));
    }
    for (const CompositorNodeVisual &visual : nodes) {
      const gui::UiPoint at =
          view_.to_graph(canvas, visual.bounds.x, visual.bounds.y);
      ctx.panel(map(at.x, at.y, visual.bounds.width / view_.zoom,
                    visual.bounds.height / view_.zoom),
                with_alpha(compositor_node_color(ctx, visual.node->kind), 0.8F));
    }
    const gui::Rect seen = view_.visible(canvas);
    ctx.outline(map(seen.x, seen.y, seen.width, seen.height), ctx.theme().text);
    ctx.outline(minimap, ctx.theme().border);
  }
  if (ctx.time_seconds() < notice_until_ && !notice_.empty()) {
    const float width = std::ceil(ctx.font_width(notice_)) + 20.0F;
    const float height = ctx.line_height() + 10.0F;
    const gui::Rect badge{canvas.x + 10.0F,
                          canvas.bottom() - height - 10.0F, width,
                          height};
    ctx.panel(badge, with_alpha(ctx.theme().background, 0.94F));
    ctx.outline(badge, ctx.theme().border);
    ctx.label(gui::Rect{badge.x + 10.0F, badge.y, badge.width - 10.0F,
                        badge.height},
              notice_, ctx.theme().text);
  }
  ctx.pop_clip();

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
        if (!is_selected(parameter_node->node)) {
          select_only(parameter_node->node);
        }
        toggle_bypass();
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
          const bool direct = palette_direct_matches(item);
          if (!palette_matches(item) || direct != (pass == 0U)) {
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
          to_graph(node_menu_->screen_x, node_menu_->screen_y);
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
        start_rename();
        break;
      case ContextAction::Duplicate:
        duplicate_selection();
        break;
      case ContextAction::Copy:
        copy_selection();
        break;
      case ContextAction::Delete:
        frame.remove = selection_delete(compositor_graph);
        break;
      case ContextAction::AlignLeft:
        apply_moves(align_boxes(selected_boxes(), AlignEdge::Left));
        break;
      case ContextAction::AlignRight:
        apply_moves(align_boxes(selected_boxes(), AlignEdge::Right));
        break;
      case ContextAction::AlignTop:
        apply_moves(align_boxes(selected_boxes(), AlignEdge::Top));
        break;
      case ContextAction::AlignBottom:
        apply_moves(align_boxes(selected_boxes(), AlignEdge::Bottom));
        break;
      case ContextAction::DistributeHorizontal:
        apply_moves(distribute_boxes(selected_boxes(), true));
        break;
      case ContextAction::DistributeVertical:
        apply_moves(distribute_boxes(selected_boxes(), false));
        break;
      case ContextAction::Straighten:
        apply_moves(straightened());
        break;
      case ContextAction::Arrange:
        apply_moves(arranged());
        break;
      case ContextAction::CommentSelection:
        comment_selection();
        break;
      case ContextAction::Bypass:
        toggle_bypass();
        break;
      case ContextAction::Collapse:
        if (const compositor::GraphNode *const node =
                compositor_graph.node(menu.target)) {
          toggle_collapse(node->node, chosen->label.starts_with("COLLAPSE"));
        }
        break;
      case ContextAction::AddPoint:
        if (const compositor::GraphEdge *const edge =
                compositor_graph.edge(menu.target)) {
          std::vector<compositor::WirePoint> points = edge_points(*edge);
          points.insert(points.begin() +
                            static_cast<std::ptrdiff_t>(
                                std::min(menu.segment, points.size())),
                        menu.at);
          emit_points(edge->node, std::move(points), edge->points);
        }
        break;
      case ContextAction::MoveEarlier:
        if (const auto earlier = order_neighbour(menu.target, -1)) {
          move_in_order(menu.target, *earlier);
        }
        break;
      case ContextAction::MoveLater:
        if (const auto later = order_neighbour(menu.target, 1)) {
          move_in_order(menu.target, *later);
        }
        break;
      case ContextAction::Disconnect:
        frame.rewire = CompositorRewireIntent{{menu.target}, {}};
        break;
      case ContextAction::Colour:
        if (const compositor::GraphComment *const comment =
                comment_of(menu.target)) {
          compositor::GraphComment recoloured = drawn_comment(*comment);
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

  // A title being typed over its node or comment box: Enter or a click
  // elsewhere keeps it, Escape abandons it.
  if (rename_.has_value()) {
    std::optional<gui::Rect> field;
    if (const auto visual = std::find_if(
            nodes.begin(), nodes.end(),
            [this](const CompositorNodeVisual &entry) {
              return entry.node->node == rename_->node;
            });
        visual != nodes.end()) {
      field = gui::Rect{visual->bounds.x + 4.0F, visual->bounds.y + 3.0F,
                        std::max(visual->bounds.width - 8.0F, 40.0F),
                        std::max(ctx.line_height() + 6.0F, 22.0F)};
    } else if (const compositor::GraphComment *const comment =
                   comment_of(rename_->node)) {
      const compositor::GraphComment drawn = drawn_comment(*comment);
      const gui::Rect box = view_.to_screen(
          canvas, gui::Rect{drawn.x, drawn.y, drawn.width, drawn.height});
      field = gui::Rect{box.x + 4.0F, box.y + 2.0F,
                        std::max(box.width - 8.0F, 60.0F),
                        std::max(ctx.line_height() + 6.0F, 22.0F)};
    }
    const gui::UiId rename_id = gui::ui_id(id_, "compositor.rename");
    if (!field.has_value() || ctx.key_pressed(gui::Key::Escape)) {
      rename_.reset();
      ctx.clear_focus();
    } else {
      const bool commit =
          ctx.key_pressed(gui::Key::Enter) ||
          (!rename_->focus_pending && ctx.focus() != rename_id);
      if (rename_->focus_pending) {
        ctx.focus_text_field_selected(rename_id, rename_->text);
        rename_->focus_pending = false;
      }
      ctx.push_clip(canvas);
      ctx.panel(*field, ctx.theme().background);
      static_cast<void>(ctx.text_field(rename_id, *field, rename_->text));
      ctx.outline(*field, ctx.theme().accent);
      ctx.pop_clip();
      if (commit) {
        frame.rename = CompositorRenameIntent{rename_->node, rename_->text};
        rename_.reset();
        ctx.clear_focus();
      }
    }
  }

  // Hold what this frame authored on screen until the graph reports it.
  if (frame.connect_ports.has_value() || frame.rewire.has_value()) {
    PendingWires held;
    if (frame.connect_ports.has_value()) {
      held.added.push_back(
          CompositorWire{frame.connect_ports->from, frame.connect_ports->to});
      if (frame.connect_ports->replace_edge.has_value()) {
        held.removed.push_back(*frame.connect_ports->replace_edge);
      }
    }
    if (frame.rewire.has_value()) {
      held.added.insert(held.added.end(), frame.rewire->add_edges.begin(),
                        frame.rewire->add_edges.end());
      held.removed.insert(held.removed.end(),
                          frame.rewire->remove_edges.begin(),
                          frame.rewire->remove_edges.end());
    }
    pending_wires_ = std::move(held);
  }

  frame.editing =
      parameter_fields_.live() || inline_fields_.live() || colour_editing_;
  colour_editing_ = false;
  ctx.pop_clip();
  if (drag_released) {
    dragging_node_ = {};
  }
  return frame;
}

void CompositorPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                           std::span<const ElementInfo> elements,
                           doc::NodeId primary) const {
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return;
  }
  ctx.push_clip(bounds);
  ctx.panel(bounds, scale(ctx.theme().background, 0.86F));

  const float header_height = 20.0F;
  const gui::Rect header{bounds.x, bounds.y, bounds.width, header_height};
  ctx.panel(header, ctx.theme().background);
  ctx.label(gui::Rect{header.x + 8.0F, header.y, 68.0F, header.height}, "NODES",
            ctx.theme().text);
  ctx.label(gui::Rect{header.x + 76.0F, header.y, 52.0F, header.height}, "CUES",
            ctx.theme().text_muted);
  ctx.label(gui::Rect{header.x + 128.0F, header.y, 90.0F, header.height},
            "AUTOMATION", ctx.theme().text_muted);
  std::array<char, 24U> count{};
  const int count_length =
      std::snprintf(count.data(), count.size(), "%llu LAYERS",
                    static_cast<unsigned long long>(elements.size()));
  const std::string_view count_view{
      count.data(), static_cast<std::size_t>(std::max(count_length, 0))};
  const float count_width = ctx.font_width(count_view);
  ctx.label(gui::Rect{header.right() - count_width - 8.0F, header.y,
                      count_width, header.height},
            count_view, ctx.theme().text_muted);
  ctx.panel(
      gui::Rect{header.x, header.bottom() - 1.0F, header.width, 1.0F},
      ctx.theme().divider);

  const gui::Rect content{bounds.x, bounds.y + header_height, bounds.width,
                          std::max(bounds.height - header_height, 0.0F)};
  const float inputs_width = std::clamp(content.width * 0.19F, 120.0F, 180.0F);
  const gui::Rect inputs{content.x, content.y,
                         std::min(inputs_width, content.width), content.height};
  ctx.panel(inputs, ctx.theme().panel);
  ctx.panel(
      gui::Rect{inputs.right() - 1.0F, inputs.y, 1.0F, inputs.height},
      ctx.theme().divider);
  ctx.label(
      gui::Rect{inputs.x + 8.0F, inputs.y + 1.0F, inputs.width - 16.0F, 19.0F},
      "SCENE LAYERS", ctx.theme().text_muted);

  float input_y = inputs.y + 23.0F;
  const std::size_t input_count = std::min<std::size_t>(elements.size(), 6U);
  for (std::size_t index = 0U; index < input_count; ++index) {
    const gui::Rect row{inputs.x + 7.0F, input_y,
                        std::max(inputs.width - 14.0F, 0.0F), 21.0F};
    ctx.panel(row, index % 2U == 0U ? ctx.theme().row : ctx.theme().row_alt);
    ctx.panel(gui::Rect{row.x + 4.0F, row.y + 6.0F, 3.0F, 9.0F},
              kind_color(elements[index].kind));
    ctx.label(gui::Rect{row.x + 12.0F, row.y, std::max(row.width - 17.0F, 0.0F),
                        row.height},
              element_name(elements[index]), ctx.theme().text_muted);
    input_y += 24.0F;
  }
  if (elements.empty()) {
    ctx.label(gui::Rect{inputs.x + 8.0F, input_y,
                        std::max(inputs.width - 16.0F, 0.0F), 36.0F},
              "No scene layers", ctx.theme().text_muted);
  }

  const gui::Rect graph{inputs.right(), content.y,
                        std::max(content.width - inputs.width, 0.0F),
                        content.height};
  // A restrained engineering-paper grid gives an empty graph useful scale.
  for (float x = graph.x + 15.0F; x < graph.right(); x += 32.0F) {
    ctx.draw_list().add_line(x, graph.y, x, graph.bottom(), 1.0F,
                             with_alpha(ctx.theme().divider, 0.60F));
  }
  for (float y = graph.y + 15.0F; y < graph.bottom(); y += 32.0F) {
    ctx.draw_list().add_line(graph.x, y, graph.right(), y, 1.0F,
                             with_alpha(ctx.theme().divider, 0.60F));
  }

  if (graph.width < 450.0F || graph.height < 54.0F) {
    ctx.label(
        gui::Rect{graph.x + 8.0F, graph.y, graph.width - 16.0F, graph.height},
        "COMPOSITOR GRAPH", ctx.theme().text_muted);
    ctx.pop_clip();
    return;
  }

  constexpr float node_height = 44.0F;
  const float node_width = std::clamp(graph.width * 0.135F, 68.0F, 112.0F);
  const float centre_y = graph.y + graph.height * 0.52F;
  constexpr float graph_margin = 14.0F;
  const float column_gap = std::max(
      (graph.width - graph_margin * 2.0F - node_width * 5.0F) / 4.0F, 6.0F);
  const float left_x = graph.x + graph_margin;
  const float analyzer_x = left_x + node_width + column_gap;
  const float element_x = analyzer_x + node_width + column_gap;
  const float grade_x = element_x + node_width + column_gap;
  const float output_x = grade_x + node_width + column_gap;

  std::size_t active_effects = 0U;
  for (const ElementInfo &element : elements) {
    active_effects += element.style.effects.brightness_enabled ? 1U : 0U;
    active_effects += element.style.effects.saturation_enabled ? 1U : 0U;
    active_effects += element.style.effects.blur_enabled ? 1U : 0U;
  }
  std::array<char, 32U> effect_detail{};
  const int effect_length = std::snprintf(
      effect_detail.data(), effect_detail.size(), "%zu active", active_effects);
  const std::string effect_text{
      effect_detail.data(),
      static_cast<std::size_t>(std::max(effect_length, 0))};

  const NodeBox input_node{
      {left_x, centre_y - node_height * 0.5F, node_width, node_height},
      "Scene",
      "Authored layers",
      ctx.theme().accent,
      false};
  const NodeBox analyzer_node{
      {analyzer_x, centre_y - node_height * 0.5F, node_width, node_height},
      "Layer Stack",
      "Scene order",
      ctx.theme().keyframe,
      false};
  const NodeBox grade_node{
      {grade_x, centre_y - node_height * 0.5F, node_width, node_height},
      "Effects",
      effect_text,
      gui::UiColor{0.780F, 0.318F, 0.922F, 1.0F},
      false};
  const NodeBox output_node{
      {output_x, centre_y - node_height * 0.5F, node_width, node_height},
      "Program",
      "Preview",
      ctx.theme().good,
      false};

  std::vector<NodeBox> element_nodes;
  const std::size_t maximum_shown =
      graph.height >= 146.0F ? 3U : (graph.height >= 95.0F ? 2U : 1U);
  const std::size_t shown =
      std::min<std::size_t>(elements.size(), maximum_shown);
  element_nodes.reserve(std::max<std::size_t>(shown, 1U));
  if (shown == 0U) {
    element_nodes.push_back(NodeBox{
        {element_x, centre_y - node_height * 0.5F, node_width, node_height},
        "Scene Layers",
        "Empty",
        ctx.theme().text_muted,
        false});
  } else {
    const float total = static_cast<float>(shown) * node_height +
                        static_cast<float>(shown - 1U) * 7.0F;
    float y = centre_y - total * 0.5F;
    for (std::size_t index = 0U; index < shown; ++index) {
      const ElementInfo &element = elements[index];
      element_nodes.push_back(NodeBox{{element_x, y, node_width, node_height},
                                      element_name(element),
                                      std::string{kind_name(element.kind)},
                                      kind_color(element.kind),
                                      element.node == primary});
      y += node_height + 7.0F;
    }
  }

  wire(ctx, input_node.bounds.right(),
       input_node.bounds.y + input_node.bounds.height * 0.5F,
       analyzer_node.bounds.x,
       analyzer_node.bounds.y + analyzer_node.bounds.height * 0.5F,
       ctx.theme().accent);
  for (const NodeBox &element : element_nodes) {
    wire(ctx, analyzer_node.bounds.right(),
         analyzer_node.bounds.y + analyzer_node.bounds.height * 0.5F,
         element.bounds.x, element.bounds.y + element.bounds.height * 0.5F,
         element.color);
    wire(ctx, element.bounds.right(),
         element.bounds.y + element.bounds.height * 0.5F, grade_node.bounds.x,
         grade_node.bounds.y + grade_node.bounds.height * 0.5F, element.color);
  }
  wire(ctx, grade_node.bounds.right(),
       grade_node.bounds.y + grade_node.bounds.height * 0.5F,
       output_node.bounds.x,
       output_node.bounds.y + output_node.bounds.height * 0.5F,
       grade_node.color);

  draw_node(ctx, input_node);
  draw_node(ctx, analyzer_node);
  for (const NodeBox &element : element_nodes) {
    draw_node(ctx, element);
  }
  draw_node(ctx, grade_node);
  draw_node(ctx, output_node);
  ctx.pop_clip();
}

} // namespace mediaserver::editor
