#include <mediaserver/compositor/compositor_model.hpp>
#include <mediaserver/compositor/compositor_schema.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace compositor = mediaserver::compositor;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;
namespace schema = mediaserver::compositor::schema;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define GRAPH_CHECK(...)                                                       \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] Property whole(std::string_view name, std::int64_t value) {
  return {std::string{name}, value};
}

[[nodiscard]] Property real(std::string_view name, double value) {
  return {std::string{name}, value};
}

[[nodiscard]] doc::Node node(core::u64 id, std::string_view type,
                             std::initializer_list<Property> properties = {}) {
  doc::Node result{};
  result.id = doc::NodeId{id};
  result.type = std::string{type};
  for (const auto &[name, value] : properties) {
    result.properties.emplace(name, value);
  }
  return result;
}

// A Scene Layer takes effects on port 0 and filters on port 2.
constexpr std::int64_t filters_port = 2;

[[nodiscard]] doc::Node edge(core::u64 id, core::u64 from, core::u64 to,
                             core::i64 order = 0, std::int64_t to_port = 0) {
  return node(id, schema::edge_type,
              {whole(schema::from_node, static_cast<std::int64_t>(from)),
               whole(schema::from_port, 0),
               whole(schema::to_node, static_cast<std::int64_t>(to)),
               whole(schema::to_port, to_port),
               whole(schema::order, order)});
}

// A fixture with exactly one Stage owns every graph and scene node that does
// not name a Stage itself; fixtures with several Stages say which they mean.
[[nodiscard]] std::vector<doc::Node> owned_by_the_only_stage(std::vector<doc::Node> nodes) {
    const doc::Node* only{};
    std::size_t stages{};
    for (const doc::Node& value : nodes) {
        if (value.type == "scene.stage") {
            only = &value;
            ++stages;
        }
    }
    if (stages != 1U) {
        return nodes;
    }
    const auto owner = static_cast<std::int64_t>(only->id.value());
    for (doc::Node& value : nodes) {
        const std::string_view type{value.type};
        constexpr std::array<std::string_view, 9U> scoped{
            "scene.camera", "scene.object", "scene.group", "scene.light",
            "scene.media", "scene.timeline_lane", "scene.timeline_marker",
            "scene.model", "scene.particle_effect"};
        if ((type.starts_with("compositor.") && type != "compositor.edge") ||
            std::find(scoped.begin(), scoped.end(), type) != scoped.end()) {
            value.properties.emplace("scene_id", doc::PropertyValue{owner});
        }
    }
    return nodes;
}

[[nodiscard]] doc::Root root_of(std::vector<doc::Node> nodes) {
  nodes = owned_by_the_only_stage(std::move(nodes));
  doc::Root root;
  for (doc::Node &value : nodes) {
    root = root.set(std::move(value));
  }
  return root;
}

[[nodiscard]] doc::Root spatial_graph(bool identities = false) {
  return root_of({
      node(100U, scene::schema::stage_type,
           {whole(scene::schema::canvas_width, 1920),
            whole(scene::schema::canvas_height, 1080)}),
      node(200U, scene::schema::object_type,
           {whole(scene::schema::scene_id, 100)}),
      node(300U, scene::schema::settings_type,
           {whole(scene::schema::directional_blur_taps, 9)}),
      node(1U, schema::box_blur_type,
           {whole(schema::box_blur_radius, identities ? 0 : 3),
            whole(schema::box_blur_passes, 2)}),
      node(2U, schema::directional_blur_type,
           {real(schema::directional_blur_length, identities ? 0.0 : 10.0),
            real(schema::directional_blur_angle_turns, 1.25)}),
      node(3U, schema::scene_layer_type, {whole(schema::scene_node, 200)}),
      node(4U, schema::output_type),
      edge(10U, 1U, 3U, 0, filters_port),
      edge(11U, 2U, 3U, 1, filters_port),
      edge(12U, 3U, 4U),
  });
}

void typed_graph_plans_exact_resources_and_bounds() {
  const doc::Root root = spatial_graph();
  compositor::CompositorModel model;
  model.reset(root);
  const compositor::CompositorGraph graph =
      model.evaluate(doc::NodeId{100U});
  GRAPH_CHECK(graph.valid());

  const auto *box = graph.node(doc::NodeId{1U});
  const auto *directional = graph.node(doc::NodeId{2U});
  GRAPH_CHECK(box != nullptr && directional != nullptr);
  GRAPH_CHECK(std::get<compositor::BoxBlurParameters>(box->parameters) ==
              (scene::BoxBlurParameters{3U, 2U}));
  GRAPH_CHECK(std::get<compositor::DirectionalBlurParameters>(
                  directional->parameters) ==
              (scene::DirectionalBlurParameters{10.0F, 0.25F}));
  GRAPH_CHECK(graph.step(box->node)->requires_offscreen);
  GRAPH_CHECK(graph.step(directional->node)->requires_offscreen);

  const auto *box_metadata = graph.metadata(box->node);
  const auto *directional_metadata = graph.metadata(directional->node);
  GRAPH_CHECK(box_metadata != nullptr && directional_metadata != nullptr);
  GRAPH_CHECK(box_metadata->execution_domain ==
              compositor::GraphExecutionDomain::IsolatedLayer);
  GRAPH_CHECK(box_metadata->bounds_expansion ==
              (scene::EffectBoundsExpansion{6.0F, 6.0F}));
  GRAPH_CHECK(std::abs(directional_metadata->bounds_expansion.x) <= 1.0e-5F);
  GRAPH_CHECK(std::abs(directional_metadata->bounds_expansion.y - 5.0F) <=
              1.0e-5F);

  GRAPH_CHECK(graph.resources.active_scene_layers == 1U);
  GRAPH_CHECK(graph.resources.box_blurred_scene_layers == 1U);
  GRAPH_CHECK(graph.resources.directional_blurred_scene_layers == 1U);
  GRAPH_CHECK(graph.resources.offscreen_textures == 6U);
  GRAPH_CHECK(graph.resources.render_passes == 7U);
  GRAPH_CHECK(graph.resources.blur_taps_per_pixel == 37U);
  GRAPH_CHECK(std::abs(graph.resources.max_layer_bounds_expansion.x - 6.0F) <=
              1.0e-5F);
  GRAPH_CHECK(std::abs(graph.resources.max_layer_bounds_expansion.y - 11.0F) <=
              1.0e-5F);
}

void apply_retains_authored_spatial_order() {
  const doc::Root root = spatial_graph();
  compositor::CompositorModel compositor_model;
  compositor_model.reset(root);
  const compositor::CompositorGraph graph =
      compositor_model.evaluate(doc::NodeId{100U});
  scene::SceneModel scene_model;
  scene_model.reset(root);
  const scene::Scene source =
      scene_model.evaluate(doc::NodeId{100U}, scene::SceneTime{0U, 60U, 1U});
  const scene::Scene adjusted = compositor::apply(graph, source);
  GRAPH_CHECK(adjusted.instances.size() == 1U);
  const auto &operations =
      adjusted.instances.front().style.effects.spatial_operations;
  GRAPH_CHECK(operations.size() == 2U);
  GRAPH_CHECK(operations[0U].kind == scene::EffectKind::BoxBlur);
  GRAPH_CHECK(operations[1U].kind == scene::EffectKind::DirectionalBlur);
}

void identities_are_retained_but_allocate_nothing() {
  compositor::CompositorModel model;
  model.reset(spatial_graph(true));
  const compositor::CompositorGraph graph =
      model.evaluate(doc::NodeId{100U});
  GRAPH_CHECK(graph.valid());
  GRAPH_CHECK(!graph.step(doc::NodeId{1U})->requires_offscreen);
  GRAPH_CHECK(!graph.step(doc::NodeId{2U})->requires_offscreen);
  GRAPH_CHECK(graph.metadata(doc::NodeId{1U})->identity ==
              compositor::EffectIdentityState::Identity);
  GRAPH_CHECK(graph.metadata(doc::NodeId{2U})->identity ==
              compositor::EffectIdentityState::Identity);
  GRAPH_CHECK(graph.resources.box_blurred_scene_layers == 0U);
  GRAPH_CHECK(graph.resources.directional_blurred_scene_layers == 0U);
  GRAPH_CHECK(graph.resources.max_layer_bounds_expansion ==
              (scene::EffectBoundsExpansion{}));
}

void malformed_parameters_and_reverse_domain_order_fail_closed() {
  doc::Root malformed = root_of({
      node(1U, schema::box_blur_type,
           {real(schema::box_blur_radius, 1.5),
            whole(schema::box_blur_passes, 1)}),
      node(2U, schema::directional_blur_type,
           {real(schema::directional_blur_length, 513.0),
            real(schema::directional_blur_angle_turns, 0.0)}),
  });
  compositor::CompositorModel malformed_model;
  malformed_model.reset(malformed);
  GRAPH_CHECK(malformed_model.size() == 0U);
  GRAPH_CHECK(malformed_model.counters().rejected == 2U);

  // An effect has no place among the filters: wired into FILTERS
  // after the blurs, it is refused rather than run out of order.
  doc::Root reversed = spatial_graph();
  reversed = reversed.set(
      node(5U, schema::brightness_type,
           {real(schema::amount, 2.0), whole(scene::schema::scene_id, 100)}));
  reversed = reversed.set(edge(13U, 5U, 3U, 2, filters_port));
  compositor::CompositorModel reversed_model;
  reversed_model.reset(reversed);
  const compositor::CompositorGraph graph =
      reversed_model.evaluate(doc::NodeId{100U});
  GRAPH_CHECK(!graph.valid());
  GRAPH_CHECK(
      std::any_of(graph.diagnostics.begin(), graph.diagnostics.end(),
                  [](const compositor::GraphDiagnostic &diagnostic) noexcept {
                    return diagnostic.code ==
                           compositor::DiagnosticCode::IncompatiblePorts;
                  }));
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 4U> cases{{
    {"typed graph plans exact resources and bounds",
     &typed_graph_plans_exact_resources_and_bounds},
    {"apply retains authored spatial order",
     &apply_retains_authored_spatial_order},
    {"identities are retained but allocate nothing",
     &identities_are_retained_but_allocate_nothing},
    {"malformed parameters and reverse domain order fail closed",
     &malformed_parameters_and_reverse_domain_order_fail_closed},
}};

} // namespace

int main() {
  std::size_t failures{};
  for (const Case &test : cases) {
    try {
      test.function();
      std::cout << "[pass] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
    }
  }
  std::cout << cases.size() - failures << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
