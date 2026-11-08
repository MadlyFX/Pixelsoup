#pragma once

// Layout, colour, port and wire helpers behind the Compositor panel, and the
// types its draw phases share. Internal to the editor library.

#include <mediaserver/editor/compositor_panel.hpp>

#include "../workspace_panel_helpers.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mediaserver::editor::compositor_ui {

using namespace panel_helpers;

inline constexpr float compositor_toolbar_height = 26.0F;

// Node layout, in graph units at 100% zoom. Widths fit each node's own title
// and port names, and heights follow the editor font's line height, so no
// label is cut off whatever font or display scale the editor runs at.
inline constexpr float compositor_node_min_width = 176.0F;

inline constexpr float compositor_node_max_width = 360.0F;

inline constexpr float compositor_node_padding = 10.0F;

// From a port's dot to the start of its name.
inline constexpr float compositor_pin_inset = 13.0F;

// Room on a node's title line for its collapse toggle and problem badge.
inline constexpr float compositor_header_chrome = 40.0F;

// Below this zoom the toggle, badges and wire order numbers are left out, and
// below the next the values on a node are shown but not edited there.
inline constexpr float compositor_chrome_zoom = 0.6F;

inline constexpr float compositor_inline_edit_zoom = 0.9F;

// A bypassed node's amber: warm, so it reads apart from the cool selection
// and type colours, and quieter than the warning red.
inline constexpr gui::UiColor compositor_bypass_color{0.93F, 0.66F, 0.24F, 1.0F};

// Between an input's name and an output's name on the same row.
inline constexpr float compositor_pin_label_gap = 18.0F;

inline constexpr float compositor_port_radius = 5.0F;

// Text sizes, as multiples of the UI font, before zoom.
inline constexpr float compositor_title_text = 1.0F;

inline constexpr float compositor_detail_text = 0.875F;

inline constexpr float compositor_pin_text = 0.875F;

// Screen-space reach of the pointer, independent of zoom: how near a wire
// end has to come to a compatible port to snap to it, and how near the
// pointer has to be to a wire to pick it.
inline constexpr float compositor_port_snap_radius = 22.0F;

inline constexpr float compositor_wire_hit_radius = 6.0F;

// A released move whose edit never arrives - refused by the Director, say -
// stops overriding the graph after this many frames.
inline constexpr core::u32 compositor_pending_move_frames = 120U;

inline constexpr float compositor_minimum_zoom = 0.35F;

inline constexpr float compositor_maximum_zoom = 2.5F;

inline constexpr float compositor_zoom_step = 1.18F;

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

inline constexpr std::array<std::string_view, 7U> compositor_category_names{{
    "INPUTS",
    "COLOR",
    "KEYING",
    "BLUR",
    "TEXTURE",
    "SPATIAL / 3D",
    "COMBINE & OUTPUT",
}};

inline constexpr std::array<CompositorPaletteItem, 22U> compositor_palette_items{{
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
inline constexpr std::array<std::string_view, compositor::merge_blend_mode_count>
    merge_blend_labels{{"NORMAL", "ADD", "MULTIPLY", "SCREEN", "SUBTRACT",
                        "OVERLAY", "LIGHTEN", "DARKEN", "DIFFERENCE"}};

// In compositor::BackgroundMode order.
inline constexpr std::array<std::string_view, 3U> background_mode_labels{
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
                                      compositor::PortValueType type) noexcept;

// The colours a comment box can take: muted, so a box frames nodes rather
// than competing with them.
[[nodiscard]] gui::UiColor comment_color(core::u8 index) noexcept;

[[nodiscard]] constexpr std::string_view comment_color_name(core::u8 index) noexcept {
  constexpr std::array<std::string_view, compositor::comment_color_count> names{
      {"SLATE", "BLUE", "TEAL", "GREEN", "VIOLET", "ROSE"}};
  return names[index < names.size() ? index : 0U];
}

[[nodiscard]] gui::UiColor blend_color(gui::UiColor from, gui::UiColor to,
                                       float amount) noexcept;

// Each value type has its own port shape as well as its own colour, so the
// difference survives colour-blindness and a dimmed, incompatible port: an
// image is a circle, an effect a diamond, a filter a triangle and a
// transform set a square.
void draw_port_shape(gui::UiContext &ctx, compositor::PortValueType type,
                     float x, float y, float radius, gui::UiColor color);

// A connected port is filled; an open one is a ring showing the node behind.
void draw_port(gui::UiContext &ctx, compositor::PortValueType type, float x,
               float y, float radius, gui::UiColor color, bool filled,
               gui::UiColor hollow);

// One accent per Add Node category, so a node reads as the same family it
// was picked from.
[[nodiscard]] gui::UiColor
compositor_category_color(const gui::UiContext &ctx,
                          CompositorNodeCategory category) noexcept;

[[nodiscard]] gui::UiColor
compositor_node_color(const gui::UiContext &ctx,
                      compositor::NodeKind kind) noexcept;

// `text` at `size` times the UI font, in graph units. Zoomed text is drawn
// from the nearest baked size at or below what it asks for, so the nominal
// scaled width bounds it; a size baked exactly is measured as drawn.
[[nodiscard]] float compositor_text_width(const gui::UiContext &ctx,
                                          std::string_view text, float size);

// A line of that text, rounded up to whole units so the rows below it stay
// on the pixel grid at 100% and a glyph's rounded baseline cannot spill out.
[[nodiscard]] float compositor_text_height(const gui::UiContext &ctx,
                                           float size);

// Draws `text` at `size` times the UI font scaled by the graph's zoom, from
// the largest baked size that fits. Returns false, drawing nothing, when even
// the smallest baked text is too big - far zoomed out - so a caller can skip
// detail an operator could not read anyway.
bool compositor_label(gui::UiContext &ctx, const gui::Rect &bounds,
                      std::string_view text, gui::UiColor color, float size,
                      float zoom);

[[nodiscard]] CompositorNodeMetrics
compositor_node_metrics(const gui::UiContext &ctx);

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
inline_values(compositor::NodeKind kind) noexcept;

// An inline value that does nothing in the node's present wiring: one
// standing in for a wired input, or a Merge's constant under Normal, which
// has nothing to lay over BASE.
[[nodiscard]] bool inline_value_idle(const compositor::CompositorGraph &graph,
                                     const compositor::GraphNode &node,
                                     const InlineValue &value) noexcept;

// The authored value an inline row shows.
[[nodiscard]] double inline_value_of(const compositor::GraphNode &node,
                                     CompositorProperty property) noexcept;

// An open node's height: its port rows, then its inline value rows.
[[nodiscard]] float compositor_node_height(const CompositorNodeMetrics &metrics,
                                           compositor::NodeKind kind) noexcept;

// Wide enough for the title, a Scene Layer's source name, and the longest
// input and output names side by side on one row. Other details carry live
// values and are left out on purpose: a node must not change width while
// its value is being scrubbed. Rounded up so a renamed source does not nudge
// the node by a pixel.
[[nodiscard]] float compositor_node_width(const gui::UiContext &ctx,
                                          compositor::NodeKind kind,
                                          std::string_view title,
                                          std::string_view source);

[[nodiscard]] std::string compositor_node_detail(
    const compositor::GraphNode &node,
    std::span<const ElementInfo> elements);

[[nodiscard]] bool has_editable_parameters(compositor::NodeKind kind) noexcept;

// Bits of CompositorPanel's collapsed Parameters sections.
inline constexpr core::u32 led_car_position_section = 0U;

inline constexpr core::u32 led_car_rotation_section = 1U;

inline constexpr core::u32 led_car_plates_section = 2U;

inline constexpr core::u32 led_car_reflection_section = 3U;

inline constexpr core::u32 levels_master_section = 4U;

inline constexpr core::u32 levels_red_section = 5U;

inline constexpr core::u32 levels_green_section = 6U;

inline constexpr core::u32 levels_blue_section = 7U;

// Every field label the Parameters panel shows for a node kind, so the label
// column can be sized to the longest before any row is drawn. A label left
// out here still draws; it is shortened with an ellipsis if it does not fit.
[[nodiscard]] std::span<const std::string_view>
compositor_parameter_labels(compositor::NodeKind kind) noexcept;

[[nodiscard]] float
parameter_panel_height(const compositor::GraphNode &node) noexcept;

// Something for the operator to fix. A parked branch - a node kept in the
// graph that reaches no Output - is not: it is drawn faded and otherwise left
// alone, never counted or outlined as a problem.
[[nodiscard]] bool is_problem(
    const compositor::GraphDiagnostic &diagnostic) noexcept;

[[nodiscard]] bool node_has_diagnostic(const compositor::CompositorGraph &graph,
                                       doc::NodeId node) noexcept;

// Kept in the graph but reaching no Output.
[[nodiscard]] bool node_parked(const compositor::CompositorGraph &graph,
                               doc::NodeId node) noexcept;

// Waiting for a required input, and so taking no part in the picture.
[[nodiscard]] bool node_disabled(const compositor::CompositorGraph &graph,
                                 doc::NodeId node) noexcept;

// What a disabled node is waiting for, in the operator's words: each required
// input with no wire, or whose wires come only from nodes that are waiting
// themselves.
[[nodiscard]] std::string disabled_reason(
    const compositor::CompositorGraph &graph,
    const compositor::GraphNode &node);

[[nodiscard]] const CompositorPortVisual *
find_port(std::span<const CompositorPortVisual> ports,
          compositor::EdgeEndpoint endpoint) noexcept;

[[nodiscard]] std::string_view
port_type_name(compositor::PortValueType type) noexcept;

// Whether following wires downstream from `start` arrives at `target`.
[[nodiscard]] bool reaches(const compositor::CompositorGraph &graph,
                           doc::NodeId start, doc::NodeId target);

// Why a wire from `from` cannot end at `to`, in words an operator can act
// on; empty when it can. `ignored` are wires being moved, which do not count
// as already there. An occupied single input is connectable on purpose: the
// new wire replaces the old one in the same undo step.
[[nodiscard]] std::string
connection_refusal(const compositor::CompositorGraph &graph,
                   const CompositorPortVisual &from,
                   const CompositorPortVisual &to,
                   std::span<const doc::NodeId> ignored = {});

[[nodiscard]] bool can_connect(const compositor::CompositorGraph &graph,
                               const CompositorPortVisual &from,
                               const CompositorPortVisual &to,
                               std::span<const doc::NodeId> ignored = {});

// Retained for the read-only scene-element compatibility adapter below. The
// graph overload uses the typed node/port visuals above.
struct NodeBox final {
  gui::Rect bounds;
  std::string title;
  std::string detail;
  gui::UiColor color;
  bool highlighted{};
};

void draw_node(gui::UiContext &ctx, const NodeBox &node);

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
                                 float to_y, float zoom) noexcept;

void wire(gui::UiContext &ctx, float from_x, float from_y, float to_x,
          float to_y, gui::UiColor color, float thickness = 2.0F,
          float zoom = 1.0F);

// A wire through its reroute points: one curve per stretch, each leaving
// and entering horizontally, so a routed wire still reads as flowing right.
[[nodiscard]] std::vector<WirePath>
routed_wire(gui::UiPoint from, std::span<const gui::UiPoint> points,
            gui::UiPoint to, float zoom);

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
align_boxes(std::span<const LayoutBox> boxes, AlignEdge edge);

// Spaces three or more boxes evenly between the first and last along one
// axis; the outer two stay where they are.
[[nodiscard]] std::vector<CompositorMoveIntent>
distribute_boxes(std::span<const LayoutBox> boxes, bool horizontal);

// The pointer's distance from the drawn curve, for picking a wire.
[[nodiscard]] float wire_distance(const WirePath &path, float x,
                                  float y) noexcept;

// Shared by the phases of one CompositorPanel::draw.

inline constexpr float palette_search_height = 31.0F;
inline constexpr float palette_row_height = 23.0F;
inline constexpr float menu_row_height = 23.0F;
inline constexpr std::size_t menu_visible_rows = 12U;
inline constexpr float context_row_height = 23.0F;
inline constexpr float context_divider = 7.0F;

// The node search at the pointer: what it offers and where it sits. With a
// wire attached it offers only nodes with an input that wire can feed,
// each naming that input.
struct MenuItem final {
  const CompositorPaletteItem *item{};
  std::optional<core::u8> input;
};

// The right-click menu: what it offers for what was clicked, and where.
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

// Each wire's curve, once, for both picking and drawing: from its output
// through its reroute points to its input.
struct EdgeVisual final {
  const compositor::GraphEdge *edge{};
  const CompositorPortVisual *from{};
  const CompositorPortVisual *to{};
  std::vector<compositor::WirePoint> points;
  std::vector<WirePath> segments;
};

} // namespace mediaserver::editor::compositor_ui
