#include <mediaserver/editor/workspace_panels.hpp>

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/editor/mapping_panels.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mediaserver::editor {
namespace {

namespace gui = mediaserver::gui;

constexpr float title_height = 25.0F;
constexpr float search_height = 25.0F;
constexpr float tree_row_height = 20.0F;
constexpr float section_row_height = 21.0F;
constexpr float inset = 6.0F;

[[nodiscard]] gui::UiId mix(gui::UiId base, std::string_view name,
                            core::u64 index = 0U) noexcept {
  core::u64 hash = base ^ 0x9E3779B97F4A7C15ULL;
  for (const char character : name) {
    hash ^= static_cast<core::u8>(character);
    hash *= 1099511628211ULL;
  }
  hash ^= index + 0x9E3779B9ULL;
  hash *= 1099511628211ULL;
  return hash;
}

[[nodiscard]] gui::UiColor with_alpha(gui::UiColor color,
                                      float opacity) noexcept {
  color.a = opacity;
  return color;
}

[[nodiscard]] gui::UiColor scale(gui::UiColor color, float amount) noexcept {
  color.r *= amount;
  color.g *= amount;
  color.b *= amount;
  return color;
}

void border(gui::UiContext &ctx, const gui::Rect &bounds, gui::UiColor color,
            float thickness = 1.0F) {
  auto &draw = ctx.draw_list();
  draw.add_rect(bounds.x, bounds.y, bounds.width, thickness, color);
  draw.add_rect(bounds.x, bounds.y + bounds.height - thickness, bounds.width,
                thickness, color);
  draw.add_rect(bounds.x, bounds.y, thickness, bounds.height, color);
  draw.add_rect(bounds.x + bounds.width - thickness, bounds.y, thickness,
                bounds.height, color);
}

void draw_twirl(gui::UiContext &ctx, float centre_x, float centre_y, bool open,
                gui::UiColor color) {
  auto &draw = ctx.draw_list();
  if (open) {
    draw.add_triangle(centre_x - 3.5F, centre_y - 2.0F, centre_x + 3.5F,
                      centre_y - 2.0F, centre_x, centre_y + 2.5F, color);
    return;
  }
  draw.add_triangle(centre_x - 2.0F, centre_y - 3.5F, centre_x + 2.5F, centre_y,
                    centre_x - 2.0F, centre_y + 3.5F, color);
}

void draw_circle(gui::UiContext &ctx, float centre_x, float centre_y,
                 float radius, gui::UiColor color) {
  constexpr std::size_t point_count = 12U;
  constexpr float tau = 6.28318530718F;
  std::array<gui::UiPoint, point_count> points{};
  for (std::size_t index = 0U; index < point_count; ++index) {
    const float angle =
        tau * static_cast<float>(index) / static_cast<float>(point_count);
    points[index] = gui::UiPoint{centre_x + std::cos(angle) * radius,
                                 centre_y + std::sin(angle) * radius};
  }
  ctx.draw_list().add_convex_poly(points, color);
}

[[nodiscard]] std::string element_name(const ElementInfo &element) {
  std::array<char, 64U> output{};
  const std::string_view kind = kind_name(element.kind);
  const int length =
      std::snprintf(output.data(), output.size(), "%.*s_%03llu",
                    static_cast<int>(kind.size()), kind.data(),
                    static_cast<unsigned long long>(element.node.value()));
  return std::string{output.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string lowercase(std::string_view input) {
  std::string result;
  result.reserve(input.size());
  for (const char character : input) {
    if (character >= 'A' && character <= 'Z') {
      result.push_back(static_cast<char>(character - 'A' + 'a'));
    } else {
      result.push_back(character);
    }
  }
  return result;
}

[[nodiscard]] bool matches(std::string_view name, std::string_view query) {
  return query.empty() ||
         lowercase(name).find(lowercase(query)) != std::string::npos;
}

[[nodiscard]] bool selected(doc::NodeId node,
                            std::span<const doc::NodeId> selection) noexcept {
  return std::find(selection.begin(), selection.end(), node) != selection.end();
}

enum class SceneGroup : core::u8 {
  Visuals,
  Media,
  Lights,
};

[[nodiscard]] SceneGroup group_for(ElementKind kind) noexcept {
  if (kind == ElementKind::Media) {
    return SceneGroup::Media;
  }
  if (is_light(kind)) {
    return SceneGroup::Lights;
  }
  return SceneGroup::Visuals;
}

constexpr std::array<std::string_view, 3U> scene_group_names{"VISUALS", "MEDIA",
                                                             "LIGHTS"};

[[nodiscard]] std::size_t group_index(SceneGroup group) noexcept {
  return static_cast<std::size_t>(group);
}

void panel_title(gui::UiContext &ctx, const gui::Rect &bounds,
                 std::string_view title, std::string_view trailing = {}) {
  const gui::Rect header{bounds.x, bounds.y, bounds.width,
                         std::min(bounds.height, title_height)};
  ctx.panel(header, ctx.theme().background);
  ctx.panel(
      gui::Rect{header.x, header.y + header.height - 1.0F, header.width, 1.0F},
      ctx.theme().divider);
  ctx.label(
      gui::Rect{header.x + 8.0F, header.y, header.width - 16.0F, header.height},
      title, ctx.theme().text);
  if (!trailing.empty()) {
    const float width = ctx.font_width(trailing);
    ctx.label(gui::Rect{header.x + header.width - width - 8.0F, header.y, width,
                        header.height},
              trailing, ctx.theme().text_muted);
  }
}

void search_field(gui::UiContext &ctx, gui::UiId id, const gui::Rect &bounds,
                  std::string &value, std::string_view placeholder) {
  const gui::Rect field{bounds.x + inset, bounds.y + 3.0F,
                        std::max(bounds.width - inset * 2.0F, 0.0F),
                        std::max(bounds.height - 6.0F, 0.0F)};
  static_cast<void>(ctx.text_field(id, field, value));
  if (value.empty() && ctx.focus() != id) {
    ctx.label(
        gui::Rect{field.x + 19.0F, field.y, field.width - 23.0F, field.height},
        placeholder, ctx.theme().text_muted);
  }
  // A small magnifier, built from the same primitives as the rest of the
  // chrome, keeps the search affordance readable with no icon dependency.
  draw_circle(ctx, field.x + 9.0F, field.y + field.height * 0.46F, 3.0F,
              ctx.theme().text_muted);
  draw_circle(ctx, field.x + 9.0F, field.y + field.height * 0.46F, 1.7F,
              ctx.theme().widget);
  ctx.draw_list().add_line(
      field.x + 11.0F, field.y + field.height * 0.46F + 2.0F, field.x + 14.0F,
      field.y + field.height * 0.46F + 5.0F, 1.0F, ctx.theme().text_muted);
}

void draw_scene_kind_icon(gui::UiContext &ctx, const ElementInfo &element,
                          float x, float y) {
  const gui::UiColor color = kind_color(element.kind);
  auto &draw = ctx.draw_list();
  if (is_light(element.kind)) {
    draw_circle(ctx, x + 5.0F, y + 5.0F, 3.0F, color);
    for (std::size_t ray = 0U; ray < 4U; ++ray) {
      constexpr std::array<gui::UiPoint, 4U> directions{
          {{0.0F, -1.0F}, {1.0F, 0.0F}, {0.0F, 1.0F}, {-1.0F, 0.0F}}};
      const gui::UiPoint direction = directions[ray];
      draw.add_line(x + 5.0F + direction.x * 4.5F,
                    y + 5.0F + direction.y * 4.5F,
                    x + 5.0F + direction.x * 6.5F,
                    y + 5.0F + direction.y * 6.5F, 1.0F, color);
    }
    return;
  }
  if (element.kind == ElementKind::Media) {
    draw.add_rect(x, y + 1.0F, 10.0F, 8.0F, with_alpha(color, 0.28F));
    border(ctx, gui::Rect{x, y + 1.0F, 10.0F, 8.0F}, color);
    draw.add_triangle(x + 3.0F, y + 3.0F, x + 3.0F, y + 7.0F, x + 7.0F,
                      y + 5.0F, color);
    return;
  }
  draw.add_rect(x + 1.0F, y + 1.0F, 8.0F, 8.0F, with_alpha(color, 0.22F));
  border(ctx, gui::Rect{x + 1.0F, y + 1.0F, 8.0F, 8.0F}, color);
  draw.add_line(x + 1.0F, y + 1.0F, x + 4.0F, y - 1.0F, 1.0F, color);
  draw.add_line(x + 9.0F, y + 1.0F, x + 6.0F, y - 1.0F, 1.0F, color);
  draw.add_line(x + 4.0F, y - 1.0F, x + 6.0F, y - 1.0F, 1.0F, color);
}

struct LibraryItem final {
  std::string_view name;
  std::string_view detail;
  LibraryAddAction action;
  std::size_t category;
};

enum class LibraryCategory : std::size_t {
  Generators,
  Media,
  Models,
  Lights,
  Count,
};

constexpr std::array<std::string_view, 4U> library_category_names{
    "GENERATORS", "MEDIA", "MODELS", "LIGHTS"};

constexpr std::array<LibraryItem, 5U> library_items{{
    {"Box", "3D primitive", LibraryAddAction::Box, 0U},
    {"Plane", "2D surface", LibraryAddAction::Plane, 0U},
    {"Sphere", "3D primitive", LibraryAddAction::Sphere, 0U},
    {"Directional Light", "Global sun", LibraryAddAction::DirectionalLight, 3U},
    {"Point Light", "Local emitter", LibraryAddAction::PointLight, 3U},
}};

constexpr float asset_row_height = 48.0F;
constexpr float import_row_height = 26.0F;
constexpr float empty_row_height = 24.0F;

[[nodiscard]] gui::Rect intersect(const gui::Rect &a,
                                  const gui::Rect &b) noexcept {
  const float left = std::max(a.x, b.x);
  const float top = std::max(a.y, b.y);
  const float right = std::min(a.x + a.width, b.x + b.width);
  const float bottom = std::min(a.y + a.height, b.y + b.height);
  return gui::Rect{left, top, std::max(right - left, 0.0F),
                   std::max(bottom - top, 0.0F)};
}

[[nodiscard]] gui::Interaction clipped_grab(gui::UiContext &ctx, gui::UiId id,
                                            const gui::Rect &bounds,
                                            const gui::Rect &clip) noexcept {
  const gui::Rect hit = intersect(bounds, clip);
  return hit.width > 0.0F && hit.height > 0.0F ? ctx.grab(id, hit)
                                               : gui::Interaction{};
}

[[nodiscard]] std::string asset_detail(const assets::Asset &asset) {
  std::array<char, 72U> text{};
  int length = 0;
  if (const auto *const media = std::get_if<assets::MediaAsset>(&asset)) {
    if (media->dimensions.has_value() && media->frame_count.has_value()) {
      length = std::snprintf(text.data(), text.size(), "%u x %u | %u FR",
                             media->dimensions->width,
                             media->dimensions->height, *media->frame_count);
    } else if (media->dimensions.has_value()) {
      length =
          std::snprintf(text.data(), text.size(), "%u x %u",
                        media->dimensions->width, media->dimensions->height);
    } else if (media->frame_count.has_value()) {
      length = std::snprintf(text.data(), text.size(), "%u FRAMES",
                             *media->frame_count);
    } else {
      return "METADATA NOT PROBED";
    }
  } else {
    const auto &model = std::get<assets::ModelAsset>(asset);
    if (model.triangle_count.has_value() && model.material_count.has_value()) {
      length =
          std::snprintf(text.data(), text.size(), "%llu TRIS | %u MATS",
                        static_cast<unsigned long long>(*model.triangle_count),
                        *model.material_count);
    } else if (model.triangle_count.has_value()) {
      length =
          std::snprintf(text.data(), text.size(), "%llu TRIANGLES",
                        static_cast<unsigned long long>(*model.triangle_count));
    } else if (model.material_count.has_value()) {
      length = std::snprintf(text.data(), text.size(), "%u MATERIALS",
                             *model.material_count);
    } else if (model.bounds.has_value()) {
      return "BOUNDS STORED";
    } else {
      return "METADATA NOT PROBED";
    }
  }
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] bool asset_matches(const assets::Asset &asset,
                                 std::string_view detail,
                                 std::string_view query) {
  return matches(assets::asset_display_name(asset), query) ||
         matches(assets::asset_source(asset), query) ||
         matches(detail, query) ||
         matches(assets::asset_kind(asset) == assets::AssetKind::Media
                     ? std::string_view{"media video image"}
                     : std::string_view{"model 3d gltf obj"},
                 query);
}

void draw_library_icon(gui::UiContext &ctx, LibraryAddAction action,
                       const gui::Rect &bounds) {
  const gui::UiColor color =
      action == LibraryAddAction::DirectionalLight ||
              action == LibraryAddAction::PointLight
          ? ctx.theme().warning
          : (action == LibraryAddAction::Media ? ctx.theme().accent
                                               : ctx.theme().keyframe);
  auto &draw = ctx.draw_list();
  const float centre_x = bounds.x + bounds.width * 0.5F;
  const float centre_y = bounds.y + bounds.height * 0.5F;
  if (action == LibraryAddAction::DirectionalLight ||
      action == LibraryAddAction::PointLight) {
    draw_circle(ctx, centre_x, centre_y, 4.0F, color);
    draw.add_line(centre_x - 7.0F, centre_y, centre_x - 5.0F, centre_y, 1.0F,
                  color);
    draw.add_line(centre_x + 5.0F, centre_y, centre_x + 7.0F, centre_y, 1.0F,
                  color);
    return;
  }
  if (action == LibraryAddAction::Media) {
    draw.add_rect(bounds.x + 2.0F, bounds.y + 3.0F, bounds.width - 4.0F,
                  bounds.height - 6.0F, with_alpha(color, 0.24F));
    border(ctx,
           gui::Rect{bounds.x + 2.0F, bounds.y + 3.0F, bounds.width - 4.0F,
                     bounds.height - 6.0F},
           color);
    draw.add_triangle(centre_x - 2.0F, centre_y - 4.0F, centre_x - 2.0F,
                      centre_y + 4.0F, centre_x + 4.0F, centre_y, color);
    return;
  }
  if (action == LibraryAddAction::Model) {
    draw.add_line(bounds.x + 2.0F, bounds.y + bounds.height - 3.0F, centre_x,
                  bounds.y + 2.0F, 1.0F, color);
    draw.add_line(centre_x, bounds.y + 2.0F, bounds.x + bounds.width - 2.0F,
                  bounds.y + bounds.height - 3.0F, 1.0F, color);
    draw.add_line(bounds.x + 2.0F, bounds.y + bounds.height - 3.0F,
                  bounds.x + bounds.width - 2.0F,
                  bounds.y + bounds.height - 3.0F, 1.0F, color);
    return;
  }
  const gui::Rect cube{bounds.x + 3.0F, bounds.y + 3.0F, bounds.width - 6.0F,
                       bounds.height - 6.0F};
  draw.add_rect(cube.x, cube.y, cube.width, cube.height,
                with_alpha(color, 0.18F));
  border(ctx, cube, color);
  if (action == LibraryAddAction::Sphere) {
    draw_circle(ctx, centre_x, centre_y, cube.width * 0.46F, color);
    draw_circle(ctx, centre_x, centre_y, cube.width * 0.33F,
                ctx.theme().widget);
  } else if (action == LibraryAddAction::Plane) {
    draw.add_line(cube.x, cube.y + cube.height, cube.x + cube.width, cube.y,
                  1.0F, color);
  }
}

void draw_plus(gui::UiContext &ctx, float x, float y, gui::UiColor color) {
  ctx.draw_list().add_line(x - 3.0F, y, x + 3.0F, y, 1.0F, color);
  ctx.draw_list().add_line(x, y - 3.0F, x, y + 3.0F, 1.0F, color);
}

void draw_remove(gui::UiContext &ctx, float x, float y, gui::UiColor color) {
  ctx.draw_list().add_line(x - 3.0F, y - 3.0F, x + 3.0F, y + 3.0F, 1.0F,
                           color);
  ctx.draw_list().add_line(x + 3.0F, y - 3.0F, x - 3.0F, y + 3.0F, 1.0F,
                           color);
}

constexpr float compositor_toolbar_height = 26.0F;
constexpr float compositor_node_width = 164.0F;
constexpr float compositor_node_header_height = 26.0F;
constexpr float compositor_port_row_height = 20.0F;
constexpr float compositor_port_hit_radius = 7.0F;

struct CompositorNodeVisual final {
  const compositor::GraphNode *node{};
  gui::Rect bounds;
};

struct CompositorPortVisual final {
  compositor::EdgeEndpoint endpoint;
  const compositor::PortDescriptor *descriptor{};
  float x{};
  float y{};
  gui::Rect hit;
};

[[nodiscard]] gui::UiColor port_color(const gui::UiContext &ctx,
                                      compositor::PortValueType type) noexcept {
  switch (type) {
  case compositor::PortValueType::Image:
    return ctx.theme().accent;
  case compositor::PortValueType::Effect:
    return gui::UiColor{0.780F, 0.318F, 0.922F, 1.0F};
  case compositor::PortValueType::TransformSet:
    return ctx.theme().playhead;
  }
  return ctx.theme().text_muted;
}

[[nodiscard]] gui::UiColor
compositor_node_color(const gui::UiContext &ctx,
                      compositor::NodeKind kind) noexcept {
  switch (kind) {
  case compositor::NodeKind::SceneLayer:
    return ctx.theme().good;
  case compositor::NodeKind::Brightness:
    return ctx.theme().keyframe;
  case compositor::NodeKind::Blur:
    return gui::UiColor{0.780F, 0.318F, 0.922F, 1.0F};
  case compositor::NodeKind::RadialArray:
    return ctx.theme().playhead;
  case compositor::NodeKind::Composite:
    return ctx.theme().accent;
  case compositor::NodeKind::Output:
    return ctx.theme().good;
  }
  return ctx.theme().text_muted;
}

[[nodiscard]] float compositor_node_height(compositor::NodeKind kind) noexcept {
  const std::size_t rows =
      std::max({compositor::input_ports(kind).size(),
                compositor::output_ports(kind).size(), std::size_t{1U}});
  return compositor_node_header_height +
         static_cast<float>(rows) * compositor_port_row_height + 8.0F;
}

[[nodiscard]] std::string
compositor_node_detail(const compositor::GraphNode &node) {
  std::array<char, 64U> text{};
  int length = 0;
  switch (node.kind) {
  case compositor::NodeKind::SceneLayer: {
    const auto *parameters =
        std::get_if<compositor::SceneLayerParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "OBJECT #%llu",
        static_cast<unsigned long long>(
            parameters != nullptr ? parameters->scene_node.value() : 0U));
    break;
  }
  case compositor::NodeKind::Brightness: {
    const auto *parameters =
        std::get_if<compositor::BrightnessParameters>(&node.parameters);
    length = std::snprintf(
        text.data(), text.size(), "AMOUNT %.2f",
        static_cast<double>(parameters != nullptr ? parameters->amount : 1.0F));
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
  case compositor::NodeKind::RadialArray: {
    const auto *parameters =
        std::get_if<compositor::RadialArrayParameters>(&node.parameters);
    length = std::snprintf(text.data(), text.size(), "%u INSTANCES",
                           parameters != nullptr ? parameters->count : 0U);
    break;
  }
  case compositor::NodeKind::Composite:
    return "COMBINES IMAGE LAYERS";
  case compositor::NodeKind::Output:
    return "PROGRAM IMAGE";
  }
  return std::string{text.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] bool has_editable_parameters(compositor::NodeKind kind) noexcept {
  return kind == compositor::NodeKind::Brightness ||
         kind == compositor::NodeKind::Blur ||
         kind == compositor::NodeKind::RadialArray;
}

[[nodiscard]] float parameter_panel_height(compositor::NodeKind kind) noexcept {
  constexpr float header = 32.0F;
  constexpr float footer = 8.0F;
  constexpr float row = 26.0F;
  return header + footer +
         (kind == compositor::NodeKind::RadialArray ? 4.0F : 1.0F) * row;
}

[[nodiscard]] bool node_has_diagnostic(const compositor::CompositorGraph &graph,
                                       doc::NodeId node) noexcept {
  return std::any_of(
      graph.diagnostics.begin(), graph.diagnostics.end(),
      [node](const compositor::GraphDiagnostic &diagnostic) noexcept {
        return diagnostic.subject == node || diagnostic.related == node;
      });
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

[[nodiscard]] bool can_connect(const compositor::CompositorGraph &graph,
                               const CompositorPortVisual &from,
                               const CompositorPortVisual &to) noexcept {
  if (from.descriptor == nullptr || to.descriptor == nullptr ||
      from.descriptor->direction != compositor::PortDirection::Output ||
      to.descriptor->direction != compositor::PortDirection::Input ||
      from.descriptor->type != to.descriptor->type) {
    return false;
  }
  const bool duplicate = std::any_of(
      graph.edges.begin(), graph.edges.end(),
      [&from, &to](const compositor::GraphEdge &edge) noexcept {
        return edge.from == from.endpoint && edge.to == to.endpoint;
      });
  const bool output_occupied =
      !from.descriptor->multiple &&
      std::any_of(graph.edges.begin(), graph.edges.end(),
                  [&from](const compositor::GraphEdge &edge) noexcept {
                    return edge.from == from.endpoint;
                  });
  // An occupied single-input port is intentionally connectable: the emitted
  // intent names its current edge so the authoring boundary can replace it in
  // one transaction. This is the natural graph-editor rewire gesture.
  return !duplicate && !output_occupied;
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
  border(ctx, node.bounds,
         node.highlighted ? ctx.theme().accent : ctx.theme().widget_hover);
  ctx.panel(gui::Rect{node.bounds.x + 1.0F, node.bounds.y + 1.0F,
                      node.bounds.width - 2.0F, 4.0F},
            node.color);
  ctx.label(gui::Rect{node.bounds.x + 9.0F, node.bounds.y + 7.0F,
                      node.bounds.width - 16.0F, 17.0F},
            node.title, ctx.theme().text);
  ctx.label(gui::Rect{node.bounds.x + 9.0F, node.bounds.y + 23.0F,
                      node.bounds.width - 16.0F, 16.0F},
            node.detail, ctx.theme().text_muted);
  draw_circle(ctx, node.bounds.x, node.bounds.y + node.bounds.height * 0.5F,
              3.0F, ctx.theme().keyframe);
  draw_circle(ctx, node.bounds.x + node.bounds.width,
              node.bounds.y + node.bounds.height * 0.5F, 3.0F, node.color);
}

void wire(gui::UiContext &ctx, float from_x, float from_y, float to_x,
          float to_y, gui::UiColor color, float thickness = 1.5F) {
  const float middle_x = (from_x + to_x) * 0.5F;
  auto &draw = ctx.draw_list();
  draw.add_line(from_x, from_y, middle_x, from_y, thickness, color);
  draw.add_line(middle_x, from_y, middle_x, to_y, thickness, color);
  draw.add_line(middle_x, to_y, to_x, to_y, thickness, color);
}

[[nodiscard]] std::array<gui::Rect, 3U>
wire_hit_bounds(float from_x, float from_y, float to_x, float to_y) noexcept {
  constexpr float radius = 5.0F;
  const float middle_x = (from_x + to_x) * 0.5F;
  const auto horizontal = [](float left, float right, float y) noexcept {
    constexpr float hit_radius = 5.0F;
    const float minimum = std::min(left, right);
    return gui::Rect{minimum - hit_radius, y - hit_radius,
                     std::abs(right - left) + hit_radius * 2.0F,
                     hit_radius * 2.0F};
  };
  const float minimum_y = std::min(from_y, to_y);
  return std::array<gui::Rect, 3U>{
      horizontal(from_x, middle_x, from_y),
      gui::Rect{middle_x - radius, minimum_y - radius, radius * 2.0F,
                std::abs(to_y - from_y) + radius * 2.0F},
      horizontal(middle_x, to_x, to_y)};
}

[[nodiscard]] std::string resolution_text(core::u32 width, core::u32 height) {
  std::array<char, 32U> output{};
  const int length =
      std::snprintf(output.data(), output.size(), "%u x %u", width, height);
  return std::string{output.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string fps_text(float fps) {
  std::array<char, 16U> output{};
  const int length = std::snprintf(output.data(), output.size(), "%.0f",
                                   static_cast<double>(fps));
  return std::string{output.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string frame_text(core::u64 frame) {
  std::array<char, 32U> output{};
  const int length = std::snprintf(output.data(), output.size(), "FRAME %08llu",
                                   static_cast<unsigned long long>(frame));
  return std::string{output.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
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
    return "Port types do not match";
  case compositor::DiagnosticCode::DuplicateInput:
    return "An input already has a connection";
  case compositor::DiagnosticCode::MissingRequiredInput:
    return "A required input is not connected";
  case compositor::DiagnosticCode::MissingOutput:
    return "Add and connect one Output node";
  case compositor::DiagnosticCode::DuplicateOutput:
    return "Keep only one Output node";
  case compositor::DiagnosticCode::Cycle:
    return "Cycle detected; wires must flow toward Output";
  }
  return "Graph is not ready";
}

std::optional<SceneSelectionRequest>
ScenePanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                 std::span<const ElementInfo> elements,
                 std::span<const doc::NodeId> selection, doc::NodeId primary) {
  std::optional<SceneSelectionRequest> request;
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return request;
  }

  ctx.push_clip(bounds);
  ctx.panel(bounds);
  std::array<char, 32U> count_text{};
  const int count_length =
      std::snprintf(count_text.data(), count_text.size(), "%llu ITEMS",
                    static_cast<unsigned long long>(elements.size()));
  panel_title(
      ctx, bounds, "SCENE",
      std::string_view{count_text.data(),
                       static_cast<std::size_t>(std::max(count_length, 0))});

  const gui::Rect search_bounds{bounds.x, bounds.y + title_height, bounds.width,
                                search_height};
  search_field(ctx, mix(id_, "scene.search"), search_bounds, search_,
               "Search Scene...");

  std::array<std::vector<const ElementInfo *>, 3U> groups;
  for (const ElementInfo &element : elements) {
    const std::string name = element_name(element);
    if (matches(name, search_) || matches(kind_name(element.kind), search_)) {
      groups[group_index(group_for(element.kind))].push_back(&element);
    }
  }

  const gui::Rect list_bounds{
      bounds.x, search_bounds.y + search_bounds.height, bounds.width,
      std::max(bounds.height - title_height - search_height - 18.0F, 0.0F)};
  float content_height = tree_row_height;
  if (root_open_) {
    for (std::size_t group = 0U; group < groups.size(); ++group) {
      if (groups[group].empty() && !search_.empty()) {
        continue;
      }
      content_height += section_row_height;
      if (group_open_[group]) {
        content_height +=
            tree_row_height * static_cast<float>(groups[group].size());
      }
    }
  }
  const float maximum_scroll =
      std::max(content_height - list_bounds.height, 0.0F);
  if (list_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
    scroll_ -= ctx.wheel() * tree_row_height * 2.0F;
  }
  scroll_ = std::clamp(scroll_, 0.0F, maximum_scroll);

  ctx.push_clip(list_bounds);
  float y = list_bounds.y - scroll_;
  const gui::Rect root_row{list_bounds.x, y, list_bounds.width,
                           tree_row_height};
  const gui::Interaction root_interaction =
      ctx.grab(mix(id_, "scene.root"), root_row);
  ctx.panel(root_row, root_interaction.hovered ? ctx.theme().widget_hover
                                               : ctx.theme().header);
  draw_twirl(ctx, root_row.x + 11.0F, root_row.y + root_row.height * 0.5F,
             root_open_, ctx.theme().text_muted);
  ctx.label(gui::Rect{root_row.x + 22.0F, root_row.y, root_row.width - 28.0F,
                      root_row.height},
            "SCENE", ctx.theme().text);
  if (root_interaction.clicked) {
    root_open_ = !root_open_;
  }
  y += tree_row_height;

  if (root_open_) {
    for (std::size_t group = 0U; group < groups.size(); ++group) {
      if (groups[group].empty() && !search_.empty()) {
        continue;
      }
      const gui::Rect group_row{list_bounds.x, y, list_bounds.width,
                                section_row_height};
      const gui::Interaction group_interaction =
          ctx.grab(mix(id_, "scene.group", group), group_row);
      ctx.panel(group_row, group_interaction.hovered ? ctx.theme().widget_hover
                                                     : ctx.theme().row_alt);
      draw_twirl(ctx, group_row.x + 24.0F,
                 group_row.y + group_row.height * 0.5F, group_open_[group],
                 ctx.theme().text_muted);
      ctx.label(gui::Rect{group_row.x + 35.0F, group_row.y,
                          group_row.width - 69.0F, group_row.height},
                scene_group_names[group], ctx.theme().text_muted);
      std::array<char, 24U> group_count{};
      const int group_length =
          std::snprintf(group_count.data(), group_count.size(), "%llu",
                        static_cast<unsigned long long>(groups[group].size()));
      ctx.label(
          gui::Rect{group_row.x + group_row.width - 28.0F, group_row.y, 22.0F,
                    group_row.height},
          std::string_view{group_count.data(),
                           static_cast<std::size_t>(std::max(group_length, 0))},
          ctx.theme().text_muted);
      if (group_interaction.clicked) {
        group_open_[group] = !group_open_[group];
      }
      y += section_row_height;

      if (!group_open_[group]) {
        continue;
      }
      std::size_t row_index = 0U;
      for (const ElementInfo *const element : groups[group]) {
        const gui::Rect row{list_bounds.x, y, list_bounds.width,
                            tree_row_height};
        const gui::UiId row_id =
            mix(id_, "scene.element", element->node.value());
        const gui::Interaction interaction = ctx.grab(row_id, row);
        const bool is_selected = selected(element->node, selection);
        gui::UiColor row_color =
            row_index % 2U == 0U ? ctx.theme().row : ctx.theme().row_alt;
        if (is_selected) {
          row_color = element->node == primary
                          ? scale(ctx.theme().selection, 1.22F)
                          : ctx.theme().selection;
        } else if (interaction.hovered) {
          row_color = ctx.theme().widget_hover;
        }
        ctx.panel(row, row_color);
        if (element->node == primary) {
          ctx.panel(gui::Rect{row.x, row.y, 2.0F, row.height},
                    ctx.theme().accent);
        }
        draw_scene_kind_icon(ctx, *element, row.x + 10.0F, row.y + 5.0F);
        const std::string name = element_name(*element);
        ctx.label(gui::Rect{row.x + 27.0F, row.y,
                            std::max(row.width - 43.0F, 0.0F), row.height},
                  name,
                  is_selected ? ctx.theme().text
                              : scale(ctx.theme().text, 0.88F));
        ctx.panel(gui::Rect{row.x + row.width - 5.0F, row.y + 6.0F, 3.0F, 8.0F},
                  kind_color(element->kind));
        if (interaction.clicked) {
          request = SceneSelectionRequest{element->node,
                                          ctx.ctrl_held() || ctx.shift_held()};
        }
        y += tree_row_height;
        ++row_index;
      }
    }
  }
  ctx.pop_clip();

  const gui::Rect footer{bounds.x, bounds.y + bounds.height - 18.0F,
                         bounds.width, 18.0F};
  ctx.panel(footer, ctx.theme().background);
  ctx.panel(gui::Rect{footer.x, footer.y, footer.width, 1.0F},
            ctx.theme().divider);
  ctx.label(
      gui::Rect{footer.x + 7.0F, footer.y, footer.width - 38.0F, footer.height},
      selection.empty() ? "NO SELECTION" : "SELECTION ACTIVE",
      selection.empty() ? ctx.theme().text_muted : ctx.theme().accent);
  ctx.pop_clip();
  return request;
}

LibraryFrame LibraryPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                                const assets::AssetRegistry &registry) {
  LibraryFrame frame;
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return frame;
  }

  ctx.push_clip(bounds);
  ctx.panel(bounds);
  std::array<char, 32U> asset_count{};
  const int asset_count_length =
      std::snprintf(asset_count.data(), asset_count.size(), "%llu ASSETS",
                    static_cast<unsigned long long>(registry.assets.size()));
  panel_title(ctx, bounds, "LIBRARY",
              std::string_view{
                  asset_count.data(),
                  static_cast<std::size_t>(std::max(asset_count_length, 0))});
  const gui::Rect search_bounds{bounds.x, bounds.y + title_height, bounds.width,
                                search_height};
  search_field(ctx, mix(id_, "library.search"), search_bounds, search_,
               "Search Assets and Generators...");
  const gui::Rect list_bounds{
      bounds.x, search_bounds.y + search_bounds.height, bounds.width,
      std::max(bounds.height - title_height - search_height - 18.0F, 0.0F)};

  struct AssetRow final {
    const assets::Asset *asset{};
    std::string detail;
  };

  constexpr std::size_t category_count =
      static_cast<std::size_t>(LibraryCategory::Count);
  constexpr std::size_t media_category =
      static_cast<std::size_t>(LibraryCategory::Media);
  constexpr std::size_t model_category =
      static_cast<std::size_t>(LibraryCategory::Models);

  std::array<bool, category_count> category_query{};
  for (std::size_t category = 0U; category < category_count; ++category) {
    category_query[category] =
        matches(library_category_names[category], search_);
  }

  std::array<std::vector<const LibraryItem *>, category_count> items;
  for (const LibraryItem &item : library_items) {
    if (category_query[item.category] || matches(item.name, search_) ||
        matches(item.detail, search_)) {
      items[item.category].push_back(&item);
    }
  }

  std::array<std::vector<AssetRow>, category_count> asset_rows;
  std::array<std::size_t, category_count> total_assets{};
  for (const assets::Asset &asset : registry.assets) {
    const std::size_t category =
        assets::asset_kind(asset) == assets::AssetKind::Media ? media_category
                                                              : model_category;
    ++total_assets[category];
    std::string detail = asset_detail(asset);
    if (category_query[category] || asset_matches(asset, detail, search_)) {
      asset_rows[category].push_back(
          AssetRow{std::addressof(asset), std::move(detail)});
    }
  }

  std::array<bool, category_count> import_visible{};
  import_visible[media_category] = category_query[media_category] ||
                                   matches("Import Media Video Image", search_);
  import_visible[model_category] =
      category_query[model_category] || matches("Import Model 3D", search_);

  std::array<bool, category_count> category_visible{};
  bool has_results = false;
  for (std::size_t category = 0U; category < category_count; ++category) {
    category_visible[category] = search_.empty() || category_query[category] ||
                                 !items[category].empty() ||
                                 !asset_rows[category].empty() ||
                                 import_visible[category];
    has_results = has_results || category_visible[category];
  }

  float content_height = 0.0F;
  for (std::size_t category = 0U; category < category_count; ++category) {
    if (!category_visible[category]) {
      continue;
    }
    content_height += section_row_height;
    if (category_open_[category]) {
      content_height +=
          tree_row_height * static_cast<float>(items[category].size());
      if (import_visible[category]) {
        content_height += import_row_height;
      }
      content_height +=
          asset_row_height * static_cast<float>(asset_rows[category].size());
      if (search_.empty() &&
          (category == media_category || category == model_category) &&
          total_assets[category] == 0U) {
        content_height += empty_row_height;
      }
    }
  }
  if (!has_results) {
    content_height = empty_row_height;
  }
  const float maximum_scroll =
      std::max(content_height - list_bounds.height, 0.0F);
  if (list_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
    scroll_ -= ctx.wheel() * tree_row_height * 2.0F;
  }
  scroll_ = std::clamp(scroll_, 0.0F, maximum_scroll);

  ctx.push_clip(list_bounds);
  float y = list_bounds.y - scroll_;
  if (!has_results) {
    ctx.label(gui::Rect{list_bounds.x + 8.0F, y,
                        std::max(list_bounds.width - 16.0F, 0.0F),
                        empty_row_height},
              "NO LIBRARY RESULTS", ctx.theme().text_muted);
  }

  for (std::size_t category = 0U; category < category_count; ++category) {
    if (!category_visible[category]) {
      continue;
    }
    const gui::Rect category_row{list_bounds.x, y, list_bounds.width,
                                 section_row_height};
    const gui::Interaction category_interaction = clipped_grab(
        ctx, mix(id_, "library.category", category), category_row, list_bounds);
    if (intersect(category_row, list_bounds).height > 0.0F) {
      ctx.panel(category_row, category_interaction.hovered
                                  ? ctx.theme().widget_hover
                                  : ctx.theme().header);
      draw_twirl(ctx, category_row.x + 11.0F,
                 category_row.y + category_row.height * 0.5F,
                 category_open_[category], ctx.theme().text_muted);
      ctx.label(gui::Rect{category_row.x + 22.0F, category_row.y,
                          std::max(category_row.width - 58.0F, 0.0F),
                          category_row.height},
                library_category_names[category], ctx.theme().text_muted);
      const std::size_t visible_count =
          items[category].size() + asset_rows[category].size();
      if (visible_count > 0U && category_row.width > 90.0F) {
        std::array<char, 16U> count{};
        const int count_length =
            std::snprintf(count.data(), count.size(), "%llu",
                          static_cast<unsigned long long>(visible_count));
        ctx.label(gui::Rect{category_row.x + category_row.width - 29.0F,
                            category_row.y, 21.0F, category_row.height},
                  std::string_view{
                      count.data(),
                      static_cast<std::size_t>(std::max(count_length, 0))},
                  ctx.theme().text_muted);
      }
    }
    if (category_interaction.clicked) {
      category_open_[category] = !category_open_[category];
    }
    y += section_row_height;
    if (!category_open_[category]) {
      continue;
    }

    std::size_t item_index = 0U;
    for (const LibraryItem *const item : items[category]) {
      const gui::Rect row{list_bounds.x, y, list_bounds.width, tree_row_height};
      const gui::Interaction interaction = clipped_grab(
          ctx, mix(id_, "library.item", static_cast<core::u64>(item->action)),
          row, list_bounds);
      if (intersect(row, list_bounds).height > 0.0F) {
        ctx.panel(row, interaction.hovered
                           ? scale(ctx.theme().widget_hover, 1.08F)
                           : (item_index % 2U == 0U ? ctx.theme().row
                                                    : ctx.theme().row_alt));
        const gui::Rect icon{row.x + 8.0F, row.y + 2.0F, 16.0F, 16.0F};
        draw_library_icon(ctx, item->action, icon);
        ctx.label(gui::Rect{row.x + 31.0F, row.y,
                            std::max(row.width * 0.54F, 0.0F), row.height},
                  item->name, ctx.theme().text);
        if (row.width > 170.0F) {
          ctx.label(gui::Rect{row.x + row.width * 0.58F, row.y,
                              std::max(row.width * 0.30F, 0.0F), row.height},
                    item->detail, ctx.theme().text_muted);
        }
        draw_plus(ctx, row.x + row.width - 12.0F, row.y + row.height * 0.5F,
                  interaction.hovered ? ctx.theme().accent
                                      : ctx.theme().text_muted);
      }
      if (interaction.clicked) {
        frame.add = item->action;
      }
      y += tree_row_height;
      ++item_index;
    }

    if (import_visible[category]) {
      const gui::Rect row{list_bounds.x, y, list_bounds.width,
                          import_row_height};
      const gui::Rect button{row.x + 7.0F, row.y + 3.0F,
                             std::max(row.width - 14.0F, 0.0F),
                             row.height - 6.0F};
      const gui::Interaction interaction = clipped_grab(
          ctx, mix(id_, "library.import", category), button, list_bounds);
      if (intersect(row, list_bounds).height > 0.0F) {
        ctx.panel(button, interaction.hovered
                              ? ctx.theme().widget_hover
                              : scale(ctx.theme().widget, 0.94F));
        border(ctx, button,
               interaction.hovered ? ctx.theme().accent : ctx.theme().divider);
        draw_plus(ctx, button.x + 10.0F, button.y + button.height * 0.5F,
                  interaction.hovered ? ctx.theme().accent
                                      : ctx.theme().text_muted);
        ctx.label(
            gui::Rect{button.x + 20.0F, button.y,
                      std::max(button.width - 25.0F, 0.0F), button.height},
            category == media_category ? "IMPORT MEDIA" : "IMPORT MODEL",
            interaction.hovered ? ctx.theme().text : ctx.theme().text_muted);
      }
      if (interaction.clicked) {
        frame.import_asset = category == media_category
                                 ? LibraryImportAction::Media
                                 : LibraryImportAction::Model;
      }
      y += import_row_height;
    }

    std::size_t asset_index = 0U;
    for (const AssetRow &entry : asset_rows[category]) {
      const gui::Rect row{list_bounds.x, y, list_bounds.width,
                          asset_row_height};
      const doc::NodeId asset_id = assets::asset_id(*entry.asset);
      const float action_width = std::min(28.0F, row.width);
      const gui::Rect content{row.x, row.y,
                              std::max(row.width - action_width, 0.0F),
                              row.height};
      const gui::Rect remove{content.x + content.width, row.y, action_width,
                             row.height};
      const gui::Interaction interaction = clipped_grab(
          ctx, mix(id_, "library.asset", asset_id.value()), content,
          list_bounds);
      const gui::Interaction remove_interaction = clipped_grab(
          ctx, mix(id_, "library.asset.remove", asset_id.value()), remove,
          list_bounds);
      if (intersect(row, list_bounds).height > 0.0F) {
        ctx.panel(row, interaction.hovered || remove_interaction.hovered
                           ? scale(ctx.theme().widget_hover, 1.06F)
                           : (asset_index % 2U == 0U ? ctx.theme().row
                                                     : ctx.theme().row_alt));
        const bool is_media =
            assets::asset_kind(*entry.asset) == assets::AssetKind::Media;
        const gui::Rect icon{row.x + 8.0F, row.y + 15.0F, 16.0F, 16.0F};
        draw_library_icon(
            ctx, is_media ? LibraryAddAction::Media : LibraryAddAction::Model,
            icon);
        ctx.label(gui::Rect{row.x + 31.0F, row.y + 1.0F,
                            std::max(content.width - 48.0F, 0.0F), 15.0F},
                  assets::asset_display_name(*entry.asset), ctx.theme().text);
        ctx.label(gui::Rect{row.x + 31.0F, row.y + 16.0F,
                            std::max(content.width - 48.0F, 0.0F), 15.0F},
                  assets::asset_source(*entry.asset),
                  scale(ctx.theme().text_muted, 0.90F));
        ctx.label(gui::Rect{row.x + 31.0F, row.y + 31.0F,
                            std::max(content.width - 48.0F, 0.0F), 15.0F},
                  entry.detail, ctx.theme().text_muted);
        draw_plus(ctx, content.x + content.width - 10.0F,
                  row.y + row.height * 0.5F,
                  interaction.hovered ? ctx.theme().accent
                                      : ctx.theme().text_muted);
        ctx.panel(gui::Rect{remove.x, remove.y + 6.0F, 1.0F,
                            std::max(remove.height - 12.0F, 0.0F)},
                  ctx.theme().divider);
        draw_remove(ctx, remove.x + remove.width * 0.5F,
                    remove.y + remove.height * 0.5F,
                    remove_interaction.hovered ? ctx.theme().warning
                                               : ctx.theme().text_muted);
      }
      if (interaction.pressed) {
        drag_asset_ = asset_id;
        drag_start_x_ = ctx.pointer_x();
        drag_start_y_ = ctx.pointer_y();
        drag_moved_ = false;
        drag_label_ = std::string{assets::asset_display_name(*entry.asset)};
      }
      if (interaction.held && drag_asset_ == asset_id) {
        const float dx = ctx.pointer_x() - drag_start_x_;
        const float dy = ctx.pointer_y() - drag_start_y_;
        drag_moved_ = drag_moved_ || dx * dx + dy * dy >= 16.0F;
        if (drag_moved_) {
          frame.dragging_asset = asset_id;
        }
      }
      if (interaction.released && drag_asset_ == asset_id) {
        if (drag_moved_) {
          frame.asset_drop =
              LibraryAssetDrop{asset_id, ctx.pointer_x(), ctx.pointer_y()};
        } else if (interaction.clicked) {
          frame.instantiate_asset = asset_id;
        }
        drag_asset_ = {};
        drag_moved_ = false;
        drag_label_.clear();
      }
      if (remove_interaction.clicked) {
        frame.remove_asset = asset_id;
        if (drag_asset_ == asset_id) {
          drag_asset_ = {};
          drag_moved_ = false;
          drag_label_.clear();
        }
      }
      y += asset_row_height;
      ++asset_index;
    }

    if (search_.empty() &&
        (category == media_category || category == model_category) &&
        total_assets[category] == 0U) {
      const gui::Rect row{list_bounds.x, y, list_bounds.width,
                          empty_row_height};
      if (intersect(row, list_bounds).height > 0.0F) {
        ctx.label(gui::Rect{row.x + 31.0F, row.y,
                            std::max(row.width - 39.0F, 0.0F), row.height},
                  category == media_category ? "NO MEDIA IMPORTED"
                                             : "NO MODELS IMPORTED",
                  scale(ctx.theme().text_muted, 0.82F));
      }
      y += empty_row_height;
    }
  }
  ctx.pop_clip();

  if (maximum_scroll > 0.0F && list_bounds.height > 0.0F) {
    const gui::Rect track{list_bounds.x + list_bounds.width - 4.0F,
                          list_bounds.y, 3.0F, list_bounds.height};
    ctx.panel(track, scale(ctx.theme().divider, 0.75F));
    const float thumb_height = std::max(
        list_bounds.height * list_bounds.height / content_height, 20.0F);
    const float thumb_range = std::max(list_bounds.height - thumb_height, 0.0F);
    const float thumb_y =
        list_bounds.y + thumb_range * (scroll_ / maximum_scroll);
    ctx.panel(gui::Rect{track.x, thumb_y, track.width, thumb_height},
              ctx.theme().text_muted);
  }

  const gui::Rect footer{bounds.x, bounds.y + bounds.height - 18.0F,
                         bounds.width, 18.0F};
  ctx.panel(footer, ctx.theme().background);
  ctx.panel(gui::Rect{footer.x, footer.y, footer.width, 1.0F},
            ctx.theme().divider);
  ctx.label(
      gui::Rect{footer.x + 7.0F, footer.y, footer.width - 14.0F, footer.height},
      maximum_scroll > 0.0F ? "SCROLL FOR MORE"
                            : "CLICK + TO PLACE  |  X REMOVE",
      ctx.theme().text_muted);
  ctx.pop_clip();

  if (frame.dragging_asset.has_value()) {
    const float ghost_width =
        std::clamp(static_cast<float>(drag_label_.size()) * 7.0F + 28.0F,
                   108.0F, 220.0F);
    const gui::Rect ghost{ctx.pointer_x() + 12.0F, ctx.pointer_y() + 10.0F,
                          ghost_width, 24.0F};
    ctx.panel(ghost, with_alpha(ctx.theme().background, 0.94F));
    border(ctx, ghost, ctx.theme().accent);
    draw_plus(ctx, ghost.x + 10.0F, ghost.y + ghost.height * 0.5F,
              ctx.theme().accent);
    ctx.label(gui::Rect{ghost.x + 21.0F, ghost.y,
                        std::max(ghost.width - 25.0F, 0.0F), ghost.height},
              drag_label_, ctx.theme().text);
  }
  return frame;
}

std::optional<LibraryAddAction> LibraryPanel::draw(gui::UiContext &ctx,
                                                   const gui::Rect &bounds) {
  static const assets::AssetRegistry empty_registry;
  const LibraryFrame frame = draw(ctx, bounds, empty_registry);
  if (frame.add.has_value()) {
    return frame.add;
  }
  if (frame.import_asset == LibraryImportAction::Media) {
    return LibraryAddAction::Media;
  }
  if (frame.import_asset == LibraryImportAction::Model) {
    return LibraryAddAction::Model;
  }
  return std::nullopt;
}

CompositorFrame
CompositorPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                      const compositor::CompositorGraph &compositor_graph) {
  CompositorFrame frame;
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return frame;
  }
  if (ctx.pointer_pressed()) {
    focused_ = bounds.contains(ctx.pointer_x(), ctx.pointer_y());
  }
  if ((selection_.kind == CompositorSelectionKind::Node &&
       compositor_graph.node(selection_.node) == nullptr) ||
      (selection_.kind == CompositorSelectionKind::Edge &&
       compositor_graph.edge(selection_.node) == nullptr)) {
    selection_ = CompositorSelection{};
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
  }

  gui::Rect canvas = content;
  gui::Rect parameter_bounds{};
  if (parameter_node != nullptr) {
    constexpr float preferred_width = 224.0F;
    constexpr float minimum_graph_width = 256.0F;
    const float required_height = parameter_panel_height(parameter_node->kind);
    if (content.width >= minimum_graph_width + 190.0F) {
      const float width =
          std::clamp(content.width * 0.28F, 190.0F, preferred_width);
      canvas.width = std::max(content.width - width, 0.0F);
      parameter_bounds =
          gui::Rect{canvas.x + canvas.width, content.y, width, content.height};
    } else {
      const float height =
          std::min(required_height, std::max(content.height - 72.0F, 0.0F));
      canvas.height = std::max(content.height - height, 0.0F);
      parameter_bounds =
          gui::Rect{content.x, canvas.y + canvas.height, content.width, height};
    }
  }

  ctx.push_clip(bounds);
  ctx.panel(bounds, scale(ctx.theme().background, 0.86F));
  ctx.panel(toolbar, ctx.theme().background);
  ctx.panel(gui::Rect{toolbar.x, toolbar.y + toolbar.height - 1.0F,
                      toolbar.width, 1.0F},
            ctx.theme().divider);
  ctx.label(gui::Rect{toolbar.x + 8.0F, toolbar.y, 80.0F, toolbar.height},
            "COMPOSITOR", ctx.theme().text);

  struct AddButton final {
    compositor::NodeKind kind;
    std::string_view label;
    float width;
  };
  constexpr std::array<AddButton, 6U> add_buttons{{
      {compositor::NodeKind::Brightness, "+ BRIGHT", 76.0F},
      {compositor::NodeKind::Blur, "+ BLUR", 54.0F},
      {compositor::NodeKind::RadialArray, "+ RADIAL", 74.0F},
      {compositor::NodeKind::SceneLayer, "+ LAYER", 62.0F},
      {compositor::NodeKind::Composite, "+ COMP", 72.0F},
      {compositor::NodeKind::Output, "+ OUTPUT", 72.0F},
  }};
  const std::size_t output_count = static_cast<std::size_t>(std::count_if(
      compositor_graph.nodes.begin(), compositor_graph.nodes.end(),
      [](const compositor::GraphNode &node) noexcept {
        return node.kind == compositor::NodeKind::Output;
      }));
  float button_x = toolbar.x + 92.0F;
  const float button_limit = toolbar.x + toolbar.width - 108.0F;
  for (const AddButton &button : add_buttons) {
    const gui::Rect button_bounds{button_x, toolbar.y + 3.0F, button.width,
                                  std::max(toolbar.height - 6.0F, 0.0F)};
    if (button_bounds.x + button_bounds.width > button_limit) {
      break;
    }
    const bool enabled = button.kind != compositor::NodeKind::Output ||
                         output_count == 0U;
    gui::Interaction interaction{};
    if (enabled) {
      interaction = clipped_grab(
          ctx,
          mix(id_, "compositor.add", static_cast<core::u64>(button.kind)),
          button_bounds, toolbar);
    }
    gui::UiColor button_color = ctx.theme().track;
    if (enabled) {
      button_color = interaction.held
                         ? ctx.theme().widget_active
                         : (interaction.hovered ? ctx.theme().widget_hover
                                                : ctx.theme().widget);
    }
    ctx.panel(button_bounds, button_color);
    if (!enabled) {
      border(ctx, button_bounds, ctx.theme().divider);
    }
    ctx.label(button_bounds,
              enabled ? button.label : std::string_view{"OUTPUT SET"},
              enabled && interaction.hovered ? ctx.theme().text
                                             : ctx.theme().text_muted);
    if (interaction.clicked) {
      const float cascade =
          static_cast<float>(compositor_graph.nodes.size() % 4U) * 14.0F;
      const float node_height = compositor_node_height(button.kind);
      frame.add_node = CompositorAddIntent{
          button.kind,
          std::clamp((canvas.width - compositor_node_width) * 0.5F + cascade,
                     0.0F,
                     std::max(canvas.width - compositor_node_width, 0.0F)),
          std::clamp((canvas.height - node_height) * 0.5F + cascade, 0.0F,
                     std::max(canvas.height - node_height, 0.0F))};
    }
    button_x += button.width + 4.0F;
  }

  std::array<char, 40U> graph_status{};
  const bool invalid_output_count = output_count > 1U;
  int graph_status_length{};
  if (invalid_output_count) {
    graph_status_length = std::snprintf(
        graph_status.data(), graph_status.size(), "%llu OUTPUTS",
        static_cast<unsigned long long>(output_count));
  } else if (compositor_graph.diagnostics.empty()) {
    graph_status_length = std::snprintf(
        graph_status.data(), graph_status.size(), "%llu NODES",
        static_cast<unsigned long long>(compositor_graph.nodes.size()));
  } else {
    graph_status_length = std::snprintf(
        graph_status.data(), graph_status.size(), "%llu ISSUES",
        static_cast<unsigned long long>(compositor_graph.diagnostics.size()));
  }
  const std::string_view graph_status_text{
      graph_status.data(),
      static_cast<std::size_t>(std::max(graph_status_length, 0))};
  const float status_width = ctx.font_width(graph_status_text);
  const bool has_selection = selection_.kind != CompositorSelectionKind::None;
  const float trailing_control_width = has_selection ? 98.0F : 0.0F;
  ctx.label(gui::Rect{toolbar.x + toolbar.width - status_width - 8.0F -
                          trailing_control_width,
                      toolbar.y, status_width, toolbar.height},
            graph_status_text,
            !invalid_output_count && compositor_graph.diagnostics.empty()
                ? ctx.theme().text_muted
                : ctx.theme().warning);
  if (has_selection) {
    const gui::Rect remove_bounds{toolbar.x + toolbar.width - 94.0F,
                                  toolbar.y + 3.0F, 86.0F,
                                  std::max(toolbar.height - 6.0F, 0.0F)};
    const std::string_view label =
        selection_.kind == CompositorSelectionKind::Edge
            ? std::string_view{"DISCONNECT"}
            : std::string_view{"DELETE NODE"};
    const gui::Interaction interaction = clipped_grab(
        ctx, mix(id_, "compositor.remove"), remove_bounds, toolbar);
    ctx.panel(remove_bounds,
              interaction.held ? ctx.theme().widget_active
                               : (interaction.hovered ? ctx.theme().widget_hover
                                                      : ctx.theme().widget));
    border(ctx, remove_bounds,
           interaction.hovered ? ctx.theme().warning : ctx.theme().divider);
    ctx.label(remove_bounds, label,
              interaction.hovered ? ctx.theme().text : ctx.theme().text_muted);
    if (interaction.clicked) {
      frame.remove = CompositorDeleteIntent{selection_};
    }
  }
  if (focused_ && has_selection && !ctx.typing() &&
      ctx.key_pressed(gui::Key::Delete)) {
    frame.remove = CompositorDeleteIntent{selection_};
    frame.consume_delete = true;
  }

  if (canvas.width <= 0.0F || canvas.height <= 0.0F) {
    ctx.pop_clip();
    return frame;
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
      const bool dragging = dragging_node_ == node.node;
      const float editor_x = dragging ? drag_x_ : node.editor_x;
      const float editor_y = dragging ? drag_y_ : node.editor_y;
      const gui::Rect node_bounds{canvas.x + editor_x, canvas.y + editor_y,
                                  compositor_node_width,
                                  compositor_node_height(node.kind)};
      nodes.push_back(CompositorNodeVisual{std::addressof(node), node_bounds});

      const auto append_ports =
          [&](std::span<const compositor::PortDescriptor> descriptors,
              std::vector<CompositorPortVisual> &destination, bool input) {
            for (std::size_t index = 0U; index < descriptors.size(); ++index) {
              const compositor::PortDescriptor &descriptor = descriptors[index];
              const float x =
                  input ? node_bounds.x : node_bounds.x + node_bounds.width;
              const float y = node_bounds.y + compositor_node_header_height +
                              compositor_port_row_height *
                                  (static_cast<float>(index) + 0.5F);
              destination.push_back(CompositorPortVisual{
                  compositor::EdgeEndpoint{node.node, descriptor.index},
                  std::addressof(descriptor), x, y,
                  gui::Rect{x - compositor_port_hit_radius,
                            y - compositor_port_hit_radius,
                            compositor_port_hit_radius * 2.0F,
                            compositor_port_hit_radius * 2.0F}});
            }
          };
      append_ports(compositor::input_ports(node.kind), inputs, true);
      append_ports(compositor::output_ports(node.kind), outputs, false);
    }
  };
  build_visuals();

  bool selection_pressed = false;
  if (connecting_from_.has_value() && ctx.pointer_released()) {
    const CompositorPortVisual *const from =
        find_port(outputs, *connecting_from_);
    if (from != nullptr) {
      for (const CompositorPortVisual &to : inputs) {
        if (to.hit.contains(ctx.pointer_x(), ctx.pointer_y()) &&
            can_connect(compositor_graph, *from, to)) {
          std::optional<doc::NodeId> replace_edge;
          if (to.descriptor != nullptr && !to.descriptor->multiple) {
            const auto occupied = std::find_if(
                compositor_graph.edges.begin(), compositor_graph.edges.end(),
                [&to](const compositor::GraphEdge &edge) noexcept {
                  return edge.to == to.endpoint;
                });
            if (occupied != compositor_graph.edges.end()) {
              replace_edge = occupied->node;
            }
          }
          frame.connect_ports = CompositorConnectIntent{
              from->endpoint, to.endpoint, replace_edge};
          break;
        }
      }
    }
    connecting_from_.reset();
  }

  bool layout_changed = false;
  bool drag_released = false;
  for (const CompositorPortVisual &port : outputs) {
    const gui::Interaction interaction =
        clipped_grab(ctx,
                     mix(id_, "compositor.output_port",
                         port.endpoint.node.value() * 16U + port.endpoint.port),
                     port.hit, canvas);
    if (interaction.pressed) {
      connecting_from_ = port.endpoint;
      connecting_type_ = port.descriptor->type;
      selection_ = CompositorSelection{CompositorSelectionKind::Node,
                                       port.endpoint.node};
      frame.selection = selection_;
      selection_pressed = true;
    }
  }
  for (const CompositorNodeVisual &visual : nodes) {
    const gui::Rect move_hit{
        visual.bounds.x + compositor_port_hit_radius, visual.bounds.y,
        std::max(visual.bounds.width - compositor_port_hit_radius * 2.0F, 0.0F),
        visual.bounds.height};
    const gui::Interaction interaction = clipped_grab(
        ctx, mix(id_, "compositor.node", visual.node->node.value()), move_hit,
        canvas);
    if (interaction.pressed) {
      selection_ = CompositorSelection{CompositorSelectionKind::Node,
                                       visual.node->node};
      frame.selection = selection_;
      selection_pressed = true;
      dragging_node_ = visual.node->node;
      drag_x_ = visual.bounds.x - canvas.x;
      drag_y_ = visual.bounds.y - canvas.y;
      drag_offset_x_ = ctx.pointer_x() - visual.bounds.x;
      drag_offset_y_ = ctx.pointer_y() - visual.bounds.y;
    }
    if (interaction.held && dragging_node_ == visual.node->node) {
      const float next_x =
          std::clamp(ctx.pointer_x() - canvas.x - drag_offset_x_, 0.0F,
                     std::max(canvas.width - visual.bounds.width, 0.0F));
      const float next_y =
          std::clamp(ctx.pointer_y() - canvas.y - drag_offset_y_, 0.0F,
                     std::max(canvas.height - visual.bounds.height, 0.0F));
      layout_changed = layout_changed || next_x != drag_x_ || next_y != drag_y_;
      drag_x_ = next_x;
      drag_y_ = next_y;
      if (interaction.released) {
        if (drag_x_ != visual.node->editor_x ||
            drag_y_ != visual.node->editor_y) {
          frame.move_node =
              CompositorMoveIntent{visual.node->node, drag_x_, drag_y_};
        }
        drag_released = true;
      }
    }
  }
  if (layout_changed) {
    build_visuals();
  }

  for (const compositor::GraphEdge &edge : compositor_graph.edges) {
    const CompositorPortVisual *const from = find_port(outputs, edge.from);
    const CompositorPortVisual *const to = find_port(inputs, edge.to);
    if (from == nullptr || to == nullptr || from->descriptor == nullptr ||
        to->descriptor == nullptr ||
        from->descriptor->type != to->descriptor->type) {
      continue;
    }
    const auto hit_bounds = wire_hit_bounds(from->x, from->y, to->x, to->y);
    for (std::size_t segment = 0U; segment < hit_bounds.size(); ++segment) {
      const gui::Interaction interaction = clipped_grab(
          ctx,
          mix(id_, "compositor.edge",
              edge.node.value() * hit_bounds.size() + segment),
          hit_bounds[segment], canvas);
      if (interaction.pressed) {
        selection_ = CompositorSelection{CompositorSelectionKind::Edge,
                                         edge.node};
        frame.selection = selection_;
        selection_pressed = true;
      }
    }
  }
  if (ctx.pointer_pressed() && canvas.contains(ctx.pointer_x(), ctx.pointer_y()) &&
      !selection_pressed) {
    selection_ = CompositorSelection{};
    frame.selection = selection_;
  }

  ctx.push_clip(canvas);
  ctx.panel(canvas, scale(ctx.theme().background, 0.84F));
  for (float x = canvas.x + 16.0F; x < canvas.x + canvas.width; x += 32.0F) {
    ctx.draw_list().add_line(x, canvas.y, x, canvas.y + canvas.height, 1.0F,
                             with_alpha(ctx.theme().divider, 0.62F));
  }
  for (float y = canvas.y + 16.0F; y < canvas.y + canvas.height; y += 32.0F) {
    ctx.draw_list().add_line(canvas.x, y, canvas.x + canvas.width, y, 1.0F,
                             with_alpha(ctx.theme().divider, 0.62F));
  }

  // Guidance sits behind graph content: it remains available in empty/error
  // states, while nodes and wires always paint over it and therefore never
  // become obscured by instructional chrome.
  if (compositor_graph.nodes.empty() ||
      !compositor_graph.diagnostics.empty()) {
    const bool diagnostic = !compositor_graph.diagnostics.empty();
    const gui::Rect guide{
        canvas.x + 8.0F, canvas.y + 8.0F,
        std::min(std::max(canvas.width - 16.0F, 0.0F), 660.0F), 40.0F};
    ctx.panel(guide, with_alpha(scale(ctx.theme().header, 0.92F), 0.94F));
    border(ctx, guide,
           diagnostic ? with_alpha(ctx.theme().warning, 0.72F)
                      : with_alpha(ctx.theme().accent, 0.42F));
    std::string title = diagnostic ? "GRAPH ISSUE: " : "START: ";
    title += diagnostic
                 ? compositor_diagnostic_message(
                       compositor_graph.diagnostics.front())
                 : std::string_view{"Add a Layer for each scene object"};
    ctx.label(gui::Rect{guide.x + 8.0F, guide.y + 3.0F,
                        std::max(guide.width - 16.0F, 0.0F), 17.0F},
              title,
              diagnostic ? ctx.theme().warning : ctx.theme().text_muted);
    ctx.label(gui::Rect{guide.x + 8.0F, guide.y + 20.0F,
                        std::max(guide.width - 16.0F, 0.0F), 17.0F},
              "FLOW: EFFECT / RADIAL -> LAYER -> COMP -> OUTPUT  |  "
              "DRAG RIGHT OUT TO LEFT IN",
              ctx.theme().text_muted);
  }

  for (const compositor::GraphEdge &edge : compositor_graph.edges) {
    const CompositorPortVisual *const from = find_port(outputs, edge.from);
    const CompositorPortVisual *const to = find_port(inputs, edge.to);
    if (from == nullptr || to == nullptr || from->descriptor == nullptr ||
        to->descriptor == nullptr ||
        from->descriptor->type != to->descriptor->type) {
      continue;
    }
    const bool selected =
        selection_ == CompositorSelection{CompositorSelectionKind::Edge,
                                           edge.node};
    wire(ctx, from->x, from->y, to->x, to->y,
         selected ? ctx.theme().text
                  : with_alpha(port_color(ctx, from->descriptor->type), 0.82F),
         selected ? 3.0F : 1.5F);
  }
  if (connecting_from_.has_value()) {
    const CompositorPortVisual *const from =
        find_port(outputs, *connecting_from_);
    if (from != nullptr) {
      wire(ctx, from->x, from->y, ctx.pointer_x(), ctx.pointer_y(),
           port_color(ctx, connecting_type_));
    }
  }

  for (const CompositorNodeVisual &visual : nodes) {
    const bool selected =
        selection_ == CompositorSelection{CompositorSelectionKind::Node,
                                           visual.node->node};
    const bool diagnostic =
        node_has_diagnostic(compositor_graph, visual.node->node);
    ctx.panel(visual.bounds, selected ? scale(ctx.theme().selection, 1.18F)
                                      : scale(ctx.theme().panel, 1.06F));
    border(ctx, visual.bounds,
           diagnostic
               ? ctx.theme().warning
               : (selected ? ctx.theme().accent : ctx.theme().widget_hover));
    const gui::UiColor kind = compositor_node_color(ctx, visual.node->kind);
    const gui::Rect node_header{visual.bounds.x + 1.0F, visual.bounds.y + 1.0F,
                                std::max(visual.bounds.width - 2.0F, 0.0F),
                                compositor_node_header_height - 2.0F};
    ctx.panel(node_header, scale(ctx.theme().header, 1.08F));
    ctx.panel(gui::Rect{node_header.x, node_header.y, 3.0F, node_header.height},
              kind);
    const std::string_view title =
        visual.node->label.empty()
            ? compositor::node_kind_name(visual.node->kind)
            : std::string_view{visual.node->label};
    ctx.label(gui::Rect{node_header.x + 9.0F, node_header.y,
                        std::max(node_header.width - 17.0F, 0.0F), 13.0F},
              title, ctx.theme().text);
    const std::string detail = compositor_node_detail(*visual.node);
    ctx.label(gui::Rect{node_header.x + 9.0F, node_header.y + 12.0F,
                        std::max(node_header.width - 17.0F, 0.0F), 12.0F},
              detail, ctx.theme().text_muted);

    for (const CompositorPortVisual &port : inputs) {
      if (port.endpoint.node != visual.node->node) {
        continue;
      }
      const bool compatible = connecting_from_.has_value() &&
                              port.descriptor->type == connecting_type_;
      const bool hovered = port.hit.contains(ctx.pointer_x(), ctx.pointer_y());
      const gui::UiColor color = port_color(ctx, port.descriptor->type);
      draw_circle(ctx, port.x, port.y, compatible && hovered ? 6.0F : 4.5F,
                  compatible && hovered ? ctx.theme().text : color);
      draw_circle(ctx, port.x, port.y, 2.0F, ctx.theme().panel);
      const std::string input_label =
          "IN  " + std::string{port.descriptor->name};
      ctx.label(gui::Rect{port.x + 9.0F, port.y - 8.0F,
                          std::max(visual.bounds.width * 0.48F, 0.0F), 16.0F},
                input_label,
                compatible ? ctx.theme().text : ctx.theme().text_muted);
    }
    for (const CompositorPortVisual &port : outputs) {
      if (port.endpoint.node != visual.node->node) {
        continue;
      }
      const bool hovered = port.hit.contains(ctx.pointer_x(), ctx.pointer_y());
      const bool active = connecting_from_ == port.endpoint;
      const gui::UiColor color = port_color(ctx, port.descriptor->type);
      draw_circle(ctx, port.x, port.y, hovered || active ? 5.5F : 4.5F,
                  active ? ctx.theme().text : color);
      draw_circle(ctx, port.x, port.y, 2.0F, ctx.theme().panel);
      const std::string output_label =
          std::string{port.descriptor->name} + "  OUT";
      const float label_width = ctx.font_width(output_label);
      ctx.label(gui::Rect{port.x - label_width - 9.0F, port.y - 8.0F,
                          label_width, 16.0F},
                output_label, ctx.theme().text_muted);
    }
  }
  ctx.pop_clip();

  if (parameter_node != nullptr && parameter_bounds.width > 0.0F &&
      parameter_bounds.height > 0.0F) {
    ctx.push_clip(parameter_bounds);
    ctx.panel(parameter_bounds, scale(ctx.theme().panel, 0.98F));
    border(ctx, parameter_bounds, ctx.theme().divider);
    ctx.panel(gui::Rect{parameter_bounds.x, parameter_bounds.y,
                        parameter_bounds.width, 3.0F},
              compositor_node_color(ctx, parameter_node->kind));
    ctx.label(gui::Rect{parameter_bounds.x + 8.0F, parameter_bounds.y + 3.0F,
                        std::max(parameter_bounds.width - 16.0F, 0.0F), 15.0F},
              "PARAMETERS", ctx.theme().text);
    ctx.label(gui::Rect{parameter_bounds.x + 8.0F, parameter_bounds.y + 17.0F,
                        std::max(parameter_bounds.width - 16.0F, 0.0F), 13.0F},
              compositor::node_kind_name(parameter_node->kind),
              ctx.theme().text_muted);

    float parameter_y = parameter_bounds.y + 32.0F;
    const float label_width =
        std::clamp(parameter_bounds.width * 0.34F, 60.0F, 82.0F);
    const auto field_id = [&](CompositorProperty property) noexcept {
      return mix(id_, "compositor.parameter",
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
    const auto scalar_field =
        [&](std::string_view label, CompositorProperty property, float value,
            double speed, float minimum, float maximum, int decimals) {
          const gui::Rect row{parameter_bounds.x + 8.0F, parameter_y,
                              std::max(parameter_bounds.width - 16.0F, 0.0F),
                              22.0F};
          parameter_y += 26.0F;
          ctx.label(gui::Rect{row.x, row.y, label_width, row.height}, label,
                    ctx.theme().text_muted);
          const gui::Rect field{row.x + label_width, row.y,
                                std::max(row.width - label_width, 0.0F),
                                row.height};
          double authored = 0.0;
          if (parameter_fields_.draw(ctx, field_id(property), field, {},
                                     static_cast<double>(value), speed,
                                     authored, decimals)) {
            emit_float(property, authored, value, minimum, maximum);
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
    case compositor::NodeKind::RadialArray: {
      const auto *parameters = std::get_if<compositor::RadialArrayParameters>(
          &parameter_node->parameters);
      if (parameters == nullptr) {
        break;
      }

      const gui::Rect count_row{parameter_bounds.x + 8.0F, parameter_y,
                                std::max(parameter_bounds.width - 16.0F, 0.0F),
                                22.0F};
      parameter_y += 26.0F;
      ctx.label(
          gui::Rect{count_row.x, count_row.y, label_width, count_row.height},
          "COUNT", ctx.theme().text_muted);
      const gui::Rect count_field{count_row.x + label_width, count_row.y,
                                  std::max(count_row.width - label_width, 0.0F),
                                  count_row.height};
      double authored_count = 0.0;
      if (parameter_fields_.draw(
              ctx, field_id(CompositorProperty::RadialCount), count_field, {},
              static_cast<double>(parameters->count), 1.0, authored_count, 0) &&
          std::isfinite(authored_count)) {
        const double rounded = std::round(authored_count);
        const core::u32 value = static_cast<core::u32>(
            std::clamp(rounded, 1.0,
                       static_cast<double>(compositor::max_radial_instances)));
        if (value != parameters->count) {
          frame.property_edit = CompositorPropertyEditIntent{
              parameter_node->node, CompositorProperty::RadialCount,
              CompositorPropertyValue{value}};
        }
      }

      scalar_field("RADIUS", CompositorProperty::RadialRadius,
                   parameters->radius, 0.05, 0.0F,
                   compositor::max_spatial_extent, 2);
      scalar_field("START", CompositorProperty::RadialStartAngleTurns,
                   parameters->start_angle, 0.001,
                   -compositor::max_spatial_extent,
                   compositor::max_spatial_extent, 3);

      const gui::Rect center_row{parameter_bounds.x + 8.0F, parameter_y,
                                 std::max(parameter_bounds.width - 16.0F, 0.0F),
                                 22.0F};
      ctx.label(
          gui::Rect{center_row.x, center_row.y, label_width, center_row.height},
          "CENTER", ctx.theme().text_muted);
      const float gap = 4.0F;
      const float available =
          std::max(center_row.width - label_width - gap, 0.0F);
      const float field_width = available * 0.5F;
      const gui::Rect center_x_field{center_row.x + label_width, center_row.y,
                                     field_width, center_row.height};
      const gui::Rect center_y_field{center_x_field.x + field_width + gap,
                                     center_row.y, field_width,
                                     center_row.height};
      double authored_x = 0.0;
      if (parameter_fields_.draw(
              ctx, field_id(CompositorProperty::RadialCenterX), center_x_field,
              "X", static_cast<double>(parameters->center.x), 0.05, authored_x,
              2)) {
        emit_float(CompositorProperty::RadialCenterX, authored_x,
                   parameters->center.x, -compositor::max_spatial_extent,
                   compositor::max_spatial_extent);
      }
      double authored_y = 0.0;
      if (parameter_fields_.draw(
              ctx, field_id(CompositorProperty::RadialCenterY), center_y_field,
              "Y", static_cast<double>(parameters->center.y), 0.05, authored_y,
              2)) {
        emit_float(CompositorProperty::RadialCenterY, authored_y,
                   parameters->center.y, -compositor::max_spatial_extent,
                   compositor::max_spatial_extent);
      }
      break;
    }
    case compositor::NodeKind::SceneLayer:
    case compositor::NodeKind::Composite:
    case compositor::NodeKind::Output:
      break;
    }
    ctx.pop_clip();
  }
  frame.editing = parameter_fields_.live();
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
  ctx.label(gui::Rect{header.x + header.width - count_width - 8.0F, header.y,
                      count_width, header.height},
            count_view, ctx.theme().text_muted);
  ctx.panel(
      gui::Rect{header.x, header.y + header.height - 1.0F, header.width, 1.0F},
      ctx.theme().divider);

  const gui::Rect content{bounds.x, bounds.y + header_height, bounds.width,
                          std::max(bounds.height - header_height, 0.0F)};
  const float inputs_width = std::clamp(content.width * 0.19F, 120.0F, 180.0F);
  const gui::Rect inputs{content.x, content.y,
                         std::min(inputs_width, content.width), content.height};
  ctx.panel(inputs, ctx.theme().panel);
  ctx.panel(
      gui::Rect{inputs.x + inputs.width - 1.0F, inputs.y, 1.0F, inputs.height},
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

  const gui::Rect graph{inputs.x + inputs.width, content.y,
                        std::max(content.width - inputs.width, 0.0F),
                        content.height};
  // A restrained engineering-paper grid gives an empty graph useful scale.
  for (float x = graph.x + 15.0F; x < graph.x + graph.width; x += 32.0F) {
    ctx.draw_list().add_line(x, graph.y, x, graph.y + graph.height, 1.0F,
                             with_alpha(ctx.theme().divider, 0.60F));
  }
  for (float y = graph.y + 15.0F; y < graph.y + graph.height; y += 32.0F) {
    ctx.draw_list().add_line(graph.x, y, graph.x + graph.width, y, 1.0F,
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

  wire(ctx, input_node.bounds.x + input_node.bounds.width,
       input_node.bounds.y + input_node.bounds.height * 0.5F,
       analyzer_node.bounds.x,
       analyzer_node.bounds.y + analyzer_node.bounds.height * 0.5F,
       ctx.theme().accent);
  for (const NodeBox &element : element_nodes) {
    wire(ctx, analyzer_node.bounds.x + analyzer_node.bounds.width,
         analyzer_node.bounds.y + analyzer_node.bounds.height * 0.5F,
         element.bounds.x, element.bounds.y + element.bounds.height * 0.5F,
         element.color);
    wire(ctx, element.bounds.x + element.bounds.width,
         element.bounds.y + element.bounds.height * 0.5F, grade_node.bounds.x,
         grade_node.bounds.y + grade_node.bounds.height * 0.5F, element.color);
  }
  wire(ctx, grade_node.bounds.x + grade_node.bounds.width,
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

OutputMonitorFrame
OutputMonitorPanel::draw(gui::UiContext &ctx, const gui::Rect &bounds,
                         const OutputMonitorState &state) {
  OutputMonitorFrame frame{};
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return frame;
  }
  ctx.push_clip(bounds);
  ctx.panel(bounds);
  panel_title(ctx, bounds, "OUTPUT MONITOR", frame_text(state.frame));

  const float table_y = bounds.y + title_height;
  constexpr float column_height = 18.0F;
  const gui::Rect columns{bounds.x, table_y, bounds.width, column_height};
  ctx.panel(columns, ctx.theme().header);
  const float name_x = columns.x + 8.0F;
  const float resolution_x = columns.x + columns.width * 0.39F;
  const float fps_x = columns.x + columns.width * 0.65F;
  const float status_x = columns.x + columns.width * 0.76F;
  ctx.label(gui::Rect{name_x, columns.y, resolution_x - name_x - 4.0F,
                      columns.height},
            "OUTPUT", ctx.theme().text_muted);
  ctx.label(gui::Rect{resolution_x, columns.y, fps_x - resolution_x - 4.0F,
                      columns.height},
            "RESOLUTION", ctx.theme().text_muted);
  ctx.label(
      gui::Rect{fps_x, columns.y, status_x - fps_x - 4.0F, columns.height},
      "FPS", ctx.theme().text_muted);
  ctx.label(gui::Rect{status_x, columns.y,
                      columns.x + columns.width - status_x - 4.0F,
                      columns.height},
            "STATUS", ctx.theme().text_muted);

  using ClusterHealth = OutputMonitorState::Cluster::Health;
  const bool cluster_visible =
      state.cluster.health != ClusterHealth::Unmonitored ||
      state.cluster.configured_nodes != 0U;
  const float program_row_height = cluster_visible ? 56.0F : 20.0F;
  const std::string primary_resolution =
      resolution_text(state.width, state.height);
  const std::string fps = fps_text(state.frames_per_second);
  constexpr float footer_height = 18.0F;
  const gui::Rect list_bounds{
      bounds.x, columns.y + columns.height, bounds.width,
      std::max(bounds.y + bounds.height - footer_height -
                   (columns.y + columns.height),
               0.0F)};
  // A third row per output only when the project actually claims physical
  // synchronization. An UNSYNCED show has nothing to report there and should
  // not pay the vertical space for an empty line.
  const float node_row_height = state.cluster.sync_required ? 56.0F : 40.0F;
  const float content_height =
      program_row_height +
      node_row_height * static_cast<float>(state.nodes.size());
  const float maximum_scroll =
      std::max(content_height - list_bounds.height, 0.0F);
  if (list_bounds.contains(ctx.pointer_x(), ctx.pointer_y())) {
    scroll_ -= ctx.wheel() * node_row_height;
  }
  scroll_ = std::clamp(scroll_, 0.0F, maximum_scroll);
  float y = list_bounds.y - scroll_;
  ctx.push_clip(list_bounds);
  const gui::Rect program_row{bounds.x, y, bounds.width, program_row_height};
  ctx.panel(program_row, ctx.theme().row);
  ctx.label(gui::Rect{name_x, program_row.y,
                      resolution_x - name_x - 4.0F, 20.0F},
            "PROGRAM PREVIEW", ctx.theme().text);
  ctx.label(gui::Rect{resolution_x, program_row.y,
                      fps_x - resolution_x - 4.0F, 20.0F},
            primary_resolution, ctx.theme().text_muted);
  ctx.label(gui::Rect{fps_x, program_row.y,
                      status_x - fps_x - 4.0F, 20.0F},
            fps, ctx.theme().text_muted);
  const gui::UiColor program_color =
      state.playing ? ctx.theme().good : ctx.theme().text_muted;
  draw_circle(ctx, status_x + 5.0F,
              program_row.y + 10.0F, 2.5F,
              program_color);
  ctx.label(gui::Rect{status_x + 12.0F, program_row.y, 50.0F,
                      20.0F},
            state.playing ? "PLAY" : "PAUSE", program_color);
  if (cluster_visible) {
    const auto health_label = [](ClusterHealth health) -> std::string_view {
      switch (health) {
      case ClusterHealth::Healthy:
        return "CONTROL HEALTHY";
      case ClusterHealth::Degraded:
        return "CONTROL DEGRADED";
      case ClusterHealth::Faulted:
        return "CONTROL FAULT";
      case ClusterHealth::Unmonitored:
        return "CONTROL UNMONITORED";
      }
      return "CONTROL FAULT";
    };
    const gui::UiColor health_color =
        state.cluster.health == ClusterHealth::Healthy
            ? ctx.theme().good
            : (state.cluster.health == ClusterHealth::Faulted
                   ? ctx.theme().warning
                   : ctx.theme().keyframe);
    std::string control{health_label(state.cluster.health)};
    control += " | LIVE ";
    control += std::to_string(state.cluster.live_sessions);
    control += '/';
    control += std::to_string(state.cluster.configured_nodes);
    if (state.cluster.catching_up_sessions != 0U) {
      control += " | CATCHING UP ";
      control += std::to_string(state.cluster.catching_up_sessions);
    }
    control += " | RELEASE LAG ";
    control += std::to_string(state.cluster.release_lag_frames);
    control += " | PACER LAG ";
    control += std::to_string(state.cluster.pacer_lag_frames);
    const gui::Rect control_bounds{program_row.x + 8.0F,
                                   program_row.y + 20.0F,
                                   std::max(program_row.width - 16.0F, 0.0F),
                                   18.0F};
    ctx.push_clip(control_bounds);
    ctx.label(control_bounds, control, health_color);
    ctx.pop_clip();

    // Physical timing is reported as three separate facts on one line: the
    // authored policy, exactly what the cluster can prove about it, and how
    // much of the group is actually locked and joined. A green control state
    // never changes any of them.
    const bool timing_verified =
        state.cluster.sync_satisfied && !state.cluster.sync_degraded &&
        (!state.cluster.sync_required ||
         (state.cluster.external_time_sync_verified &&
          state.cluster.hardware_frame_lock_verified));
    std::string timing{"SYNC "};
    timing += state.cluster.sync_mode;
    timing += " | ";
    timing += state.cluster.sync_headline;
    if (state.cluster.sync_required) {
      timing += " | LOCKED ";
      timing += std::to_string(state.cluster.sync_locked_outputs);
      timing += '/';
      timing += std::to_string(state.cluster.sync_expected_outputs);
      timing += " | BARRIER ";
      timing += std::to_string(state.cluster.barrier_joined_outputs);
      timing += '/';
      timing += std::to_string(state.cluster.sync_expected_outputs);
      if (state.cluster.sync_groups != 0U) {
        timing += " | GROUPS ";
        timing += std::to_string(state.cluster.sync_groups);
      }
      if (state.cluster.sync_configuration_issues != 0U) {
        timing += " | CONFIG ISSUES ";
        timing += std::to_string(state.cluster.sync_configuration_issues);
      }
      if (state.cluster.sync_degraded) {
        timing += " | DEGRADED PROMOTION";
      }
    }
    const core::u64 incidents = state.cluster.heartbeat_timeouts +
                                state.cluster.session_failures +
                                state.cluster.barrier_rejections +
                                state.cluster.present_failures;
    if (incidents != 0U) {
      timing += " | INCIDENTS ";
      timing += std::to_string(incidents);
    }
    timing += state.cluster.diagnostics_recording ? " | DIAG LOG ON"
                                                  : " | DIAG LOG OFF";
    const gui::Rect timing_bounds{program_row.x + 8.0F,
                                  program_row.y + 38.0F,
                                  std::max(program_row.width - 16.0F, 0.0F),
                                  18.0F};
    ctx.push_clip(timing_bounds);
    ctx.label(timing_bounds, timing,
              timing_verified
                  ? ctx.theme().good
                  : (state.cluster.sync_required ? ctx.theme().warning
                                                 : ctx.theme().text_muted));
    ctx.pop_clip();
  }
  y += program_row_height;

  using NodeStatus = OutputMonitorState::Node::Status;
  using ContentStatus = OutputMonitorState::Node::ContentStatus;
  const auto status_label = [](NodeStatus status) -> std::string_view {
    switch (status) {
    case NodeStatus::Disabled:
      return "CONTROL OFF";
    case NodeStatus::InvalidConfiguration:
      return "CONFIG ERROR";
    case NodeStatus::DuplicateNodeIndex:
      return "DUPLICATE INDEX";
    case NodeStatus::Probing:
      return "CHECKING";
    case NodeStatus::Stopped:
      return "STOPPED";
    case NodeStatus::Starting:
      return "STARTING";
    case NodeStatus::Running:
      return "RUNNING";
    case NodeStatus::Stopping:
      return "STOPPING";
    case NodeStatus::Faulted:
      return "FAULTED";
    case NodeStatus::Unreachable:
      return "OFFLINE";
    case NodeStatus::AuthenticationFailed:
      return "AUTH ERROR";
    case NodeStatus::ProtocolError:
      return "PROTOCOL ERROR";
    case NodeStatus::ConfigurationMismatch:
      return "NODE MISMATCH";
    case NodeStatus::Busy:
      return "BUSY";
    }
    return "UNKNOWN";
  };
  const auto status_color = [&](NodeStatus status) {
    switch (status) {
    case NodeStatus::Running:
      return ctx.theme().good;
    case NodeStatus::Starting:
    case NodeStatus::Stopping:
    case NodeStatus::Probing:
    case NodeStatus::Busy:
      return ctx.theme().keyframe;
    case NodeStatus::Faulted:
    case NodeStatus::AuthenticationFailed:
    case NodeStatus::ProtocolError:
    case NodeStatus::ConfigurationMismatch:
    case NodeStatus::InvalidConfiguration:
    case NodeStatus::DuplicateNodeIndex:
      return ctx.theme().warning;
    case NodeStatus::Disabled:
    case NodeStatus::Stopped:
    case NodeStatus::Unreachable:
      return ctx.theme().text_muted;
    }
    return ctx.theme().text_muted;
  };
  const auto content_label = [](const OutputMonitorState::Node &node) {
    switch (node.content) {
    case ContentStatus::NotRequired:
      return std::string{};
    case ContentStatus::Checking:
      return std::string{"ASSETS CHECKING"};
    case ContentStatus::Ready:
      return std::string{"ASSETS "} + std::to_string(node.assets_ready) +
             '/' + std::to_string(node.assets_required) + " READY";
    case ContentStatus::Unverified:
      return std::string{"ASSETS UNVERIFIED "} +
             std::to_string(node.assets_unverified);
    case ContentStatus::Missing:
      return std::string{"ASSETS MISSING "} +
             std::to_string(node.assets_missing);
    case ContentStatus::Corrupt:
      return std::string{"ASSETS CORRUPT "} +
             std::to_string(node.assets_corrupt);
    case ContentStatus::Error:
      return std::string{"ASSET CHECK ERROR"};
    }
    return std::string{"ASSET CHECK ERROR"};
  };
  using NodeSync = OutputMonitorState::Node::SyncStatus;
  const auto sync_label = [](NodeSync status) -> std::string_view {
    switch (status) {
    case NodeSync::Unsynced:
      return "SYNC UNSYNCED";
    case NodeSync::Unverified:
      return "SYNC UNVERIFIED";
    case NodeSync::SyncDeviceMissing:
      return "SYNC DEVICE MISSING";
    case NodeSync::ReferenceLost:
      return "REFERENCE LOST";
    case NodeSync::ReferenceNotInUse:
      return "REFERENCE NOT IN USE";
    case NodeSync::Unlocked:
      return "OUTPUTS UNLOCKED";
    case NodeSync::BarrierIncomplete:
      return "SWAP BARRIER INCOMPLETE";
    case NodeSync::Simulated:
      return "SYNC SIMULATED";
    case NodeSync::Locked:
      return "SYNC LOCKED";
    }
    return "SYNC UNVERIFIED";
  };
  const auto sync_color = [&](NodeSync status) {
    switch (status) {
    case NodeSync::Locked:
      return ctx.theme().good;
    case NodeSync::Simulated:
    case NodeSync::Unverified:
      return ctx.theme().keyframe;
    case NodeSync::SyncDeviceMissing:
    case NodeSync::ReferenceLost:
    case NodeSync::ReferenceNotInUse:
    case NodeSync::Unlocked:
    case NodeSync::BarrierIncomplete:
      return ctx.theme().warning;
    case NodeSync::Unsynced:
      return ctx.theme().text_muted;
    }
    return ctx.theme().text_muted;
  };
  const auto content_color = [&](ContentStatus status) {
    switch (status) {
    case ContentStatus::Ready:
      return ctx.theme().good;
    case ContentStatus::Checking:
      return ctx.theme().keyframe;
    case ContentStatus::Unverified:
    case ContentStatus::Missing:
    case ContentStatus::Corrupt:
    case ContentStatus::Error:
      return ctx.theme().warning;
    case ContentStatus::NotRequired:
      return ctx.theme().text_muted;
    }
    return ctx.theme().text_muted;
  };

  for (std::size_t index = 0U; index < state.nodes.size(); ++index) {
    const OutputMonitorState::Node &node = state.nodes[index];
    const gui::Rect row{bounds.x, y, bounds.width, node_row_height};
    ctx.panel(row, index % 2U == 0U ? ctx.theme().row_alt : ctx.theme().row);
    const std::string identity =
        feed_output_identity(node.node_index) + "  " + node.name;
    const gui::Rect identity_bounds{name_x, row.y,
                                    resolution_x - name_x - 4.0F, 20.0F};
    ctx.push_clip(identity_bounds);
    ctx.label(identity_bounds, identity, ctx.theme().text);
    ctx.pop_clip();
    ctx.label(gui::Rect{resolution_x, row.y, fps_x - resolution_x - 4.0F,
                        20.0F},
              resolution_text(node.width, node.height),
              ctx.theme().text_muted);
    ctx.label(gui::Rect{fps_x, row.y, status_x - fps_x - 4.0F, 20.0F}, fps,
              ctx.theme().text_muted);
    const gui::UiColor node_status_color = status_color(node.status);
    draw_circle(ctx, status_x + 5.0F, row.y + 10.0F, 2.5F,
                node_status_color);
    ctx.label(gui::Rect{status_x + 12.0F, row.y,
                        std::max(row.x + row.width - status_x - 16.0F, 0.0F),
                        20.0F},
              status_label(node.status), node_status_color);

    float button_right = row.x + row.width - 4.0F;
    const bool row_visible = intersect(row, list_bounds).height > 0.0F;
    const auto action_button = [&](std::string_view suffix,
                                   std::string_view label, float width,
                                   OutputLifecycleAction action,
                                   bool enabled = true) {
      button_right -= width;
      const gui::Rect button{button_right, row.y + 20.0F, width - 3.0F,
                             17.0F};
      const gui::UiId button_id =
          mix(id_, suffix,
              node.device.value() * 8U + static_cast<core::u64>(action));
      const bool clicked =
          enabled && row_visible && ctx.button(button_id, button, label);
      if (!enabled) {
        ctx.panel(button, scale(ctx.theme().widget, 0.65F));
        border(ctx, button, ctx.theme().divider);
        ctx.label(button, label, scale(ctx.theme().text_muted, 0.65F));
      }
      if (clicked && !frame.lifecycle.has_value()) {
        frame.lifecycle = OutputLifecycleRequest{
            node.device, node.node_index, node.endpoint, action};
      }
      button_right -= 3.0F;
    };
    switch (node.status) {
    case NodeStatus::Stopped:
    case NodeStatus::Faulted:
      action_button("output.lifecycle.start", "START", 52.0F,
                    OutputLifecycleAction::Start, node.content_ready());
      break;
    case NodeStatus::Running:
      action_button("output.lifecycle.stop", "STOP", 47.0F,
                    OutputLifecycleAction::Stop);
      action_button("output.lifecycle.restart", "RESTART", 64.0F,
                    OutputLifecycleAction::Restart,
                    node.content_ready());
      break;
    case NodeStatus::Unreachable:
    case NodeStatus::AuthenticationFailed:
    case NodeStatus::ProtocolError:
    case NodeStatus::ConfigurationMismatch:
      action_button("output.lifecycle.status", "RETRY", 52.0F,
                    OutputLifecycleAction::Status);
      break;
    case NodeStatus::Disabled:
    case NodeStatus::InvalidConfiguration:
    case NodeStatus::DuplicateNodeIndex:
    case NodeStatus::Probing:
    case NodeStatus::Starting:
    case NodeStatus::Stopping:
    case NodeStatus::Busy:
      break;
    }
    const float detail_width = std::max(button_right - name_x - 4.0F, 0.0F);
    const std::string asset_status = content_label(node);
    std::string detail;
    if (!asset_status.empty()) {
      detail = asset_status;
      if (!node.content_detail.empty()) {
        detail += " - ";
        detail += node.content_detail;
      }
    } else {
      detail = node.detail.empty() ? node.endpoint : node.detail;
    }
    const gui::Rect detail_bounds{name_x, row.y + 20.0F, detail_width, 17.0F};
    ctx.push_clip(detail_bounds);
    ctx.label(detail_bounds, detail,
              asset_status.empty() ? ctx.theme().text_muted
                                   : content_color(node.content));
    ctx.pop_clip();
    if (state.cluster.sync_required) {
      std::string sync_line{sync_label(node.sync)};
      sync_line += " | LOCKED ";
      sync_line += std::to_string(node.sync_locked_outputs);
      sync_line += '/';
      sync_line += std::to_string(node.sync_expected_outputs);
      sync_line += " | BARRIER ";
      sync_line += std::to_string(node.barrier_joined_outputs);
      sync_line += '/';
      sync_line += std::to_string(node.barrier_expected_outputs);
      sync_line += " | MISSED ";
      sync_line += std::to_string(node.missed_frames);
      sync_line += " LATE ";
      sync_line += std::to_string(node.late_frames);
      sync_line += " HELD ";
      sync_line += std::to_string(node.held_frames);
      sync_line += " REPEATED ";
      sync_line += std::to_string(node.repeated_frames);
      if (node.loss_of_sync_events != 0U) {
        sync_line += " | SYNC LOSS ";
        sync_line += std::to_string(node.loss_of_sync_events);
      }
      if (!node.sync_detail.empty()) {
        sync_line += " - ";
        sync_line += node.sync_detail;
      }
      const gui::Rect sync_bounds{
          name_x, row.y + 37.0F,
          std::max(row.x + row.width - name_x - 12.0F, 0.0F), 17.0F};
      ctx.push_clip(sync_bounds);
      ctx.label(sync_bounds, sync_line, sync_color(node.sync));
      ctx.pop_clip();
    }
    y += node_row_height;
  }
  ctx.pop_clip();

  if (maximum_scroll > 0.0F && list_bounds.height > 8.0F) {
    const float thumb_height = std::max(
        list_bounds.height * list_bounds.height / content_height, 12.0F);
    const float thumb_range = std::max(list_bounds.height - thumb_height, 0.0F);
    const float thumb_y = list_bounds.y +
                          thumb_range * (scroll_ / maximum_scroll);
    ctx.panel(gui::Rect{list_bounds.x + list_bounds.width - 3.0F, thumb_y,
                        2.0F, thumb_height},
              ctx.theme().text_muted);
  }
  const gui::Rect footer{bounds.x, bounds.y + bounds.height - footer_height,
                         bounds.width, footer_height};
  ctx.panel(footer, ctx.theme().background);
  ctx.panel(gui::Rect{footer.x, footer.y, footer.width, 1.0F},
            ctx.theme().divider);
  const auto actionable = [&](OutputLifecycleAction action) {
    return static_cast<std::size_t>(std::count_if(
      state.nodes.begin(), state.nodes.end(), [action](const auto &node) {
        using Status = OutputMonitorState::Node::Status;
        const bool configured = node.status != Status::Disabled &&
               node.status != Status::InvalidConfiguration &&
               node.status != Status::DuplicateNodeIndex;
        return configured &&
               (action == OutputLifecycleAction::Stop ||
                node.content_ready());
      }));
  };
  constexpr float batch_width = 68.0F;
  float batch_right = footer.x + footer.width - 3.0F;
  const auto batch_button = [&](std::string_view suffix,
                                std::string_view label,
                                OutputLifecycleAction action,
                                bool caution) {
    batch_right -= batch_width;
    const gui::Rect button{batch_right, footer.y + 1.0F, batch_width - 3.0F,
                           footer.height - 2.0F};
    if (actionable(action) > 0U) {
      const gui::Interaction interaction = ctx.grab(mix(id_, suffix), button);
      const gui::UiColor base = caution ? scale(ctx.theme().warning, 0.36F)
                                        : ctx.theme().widget;
      ctx.panel(button,
                interaction.hovered ? scale(base, 1.24F) : base);
      border(ctx, button, caution ? ctx.theme().warning : ctx.theme().divider);
      ctx.label(button, label,
                caution ? ctx.theme().warning : ctx.theme().text_muted);
      if (!frame.lifecycle.has_value() && interaction.clicked) {
        frame.lifecycle = OutputLifecycleRequest{
            {}, 0U, {}, action, true};
      }
    } else {
      ctx.panel(button, scale(ctx.theme().widget, 0.65F));
      ctx.label(button, label, scale(ctx.theme().text_muted, 0.65F));
    }
    batch_right -= 2.0F;
  };
  batch_button("output.lifecycle.stop-all", "STOP ALL",
               OutputLifecycleAction::Stop, true);
  batch_button("output.lifecycle.restart-all", "RESTART ALL",
               OutputLifecycleAction::Restart, false);
  batch_button("output.lifecycle.start-all", "START ALL",
               OutputLifecycleAction::Start, false);
  const gui::Rect footer_text{
      footer.x + 8.0F, footer.y,
      std::max(batch_right - footer.x - 12.0F, 0.0F), footer.height};
  ctx.push_clip(footer_text);
  ctx.label(footer_text,
            state.nodes.empty()
                ? "Add Render Servers in Hardware Mapping"
                : "Lifecycle targets are configured per Render Server",
            ctx.theme().text_muted);
  ctx.pop_clip();
  ctx.pop_clip();
  return frame;
}

} // namespace mediaserver::editor
