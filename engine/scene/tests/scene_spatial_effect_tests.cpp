#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

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

namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;
namespace schema = mediaserver::scene::schema;

void check(bool condition, const char *expression, int line) {
  if (!condition) {
    throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                             ": check failed: " + expression};
  }
}

#define SPATIAL_CHECK(...)                                                     \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] Property whole(std::string_view name, std::int64_t value) {
  return {std::string{name}, value};
}

[[nodiscard]] Property real(std::string_view name, double value) {
  return {std::string{name}, value};
}

[[nodiscard]] Property text(std::string_view name, std::string_view value) {
  return {std::string{name}, std::string{value}};
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

[[nodiscard]] doc::Node channel(core::u64 id, core::u64 target,
                                std::string_view component, double value) {
  doc::Node result = node(
      id, schema::channel_type,
      {whole(schema::channel_target, static_cast<std::int64_t>(target)),
       text(schema::channel_component, component)});
  const std::string key = std::string{schema::channel_key_prefix} + "0";
  result.properties.emplace(
      key + std::string{schema::channel_key_frame_suffix},
      doc::PropertyValue{std::int64_t{0}});
  result.properties.emplace(
      key + std::string{schema::channel_key_value_suffix},
      doc::PropertyValue{value});
  return result;
}

[[nodiscard]] doc::Root spatial_root() {
  doc::Root root;
  root = root.set(node(1U, schema::stage_type));
  root = root.set(node(10U, schema::object_type, {whole(schema::scene_id, 1)}));
  root = root.set(
      node(20U, schema::effect_type,
           {whole(schema::effect_target, 10),
            text(schema::effect_kind, schema::box_blur_kind),
            whole(schema::effect_order, 4), whole(schema::box_blur_radius, 3),
            whole(schema::box_blur_passes, 2)}));
  root =
      root.set(node(21U, schema::effect_type,
                    {whole(schema::effect_target, 10),
                     text(schema::effect_kind, schema::directional_blur_kind),
                     whole(schema::effect_order, 5),
                     real(schema::directional_blur_length, 10.0),
                     real(schema::directional_blur_angle_turns, 1.25)}));
  root = root.set(node(30U, schema::settings_type,
                       {whole(schema::directional_blur_taps, 16)}));
  return root;
}

void typed_scene_projection_preserves_spatial_order() {
  scene::SceneModel model;
  model.reset(spatial_root());
  const auto authored = model.effects();
  SPATIAL_CHECK(authored.size() == 2U);
  SPATIAL_CHECK(authored[0U].kind == scene::EffectKind::BoxBlur);
  SPATIAL_CHECK(authored[0U].box_blur == (scene::BoxBlurParameters{3U, 2U}));
  SPATIAL_CHECK(authored[1U].kind == scene::EffectKind::DirectionalBlur);
  SPATIAL_CHECK(authored[1U].directional_blur.length_px == 10.0F);
  SPATIAL_CHECK(authored[1U].directional_blur.angle_turns == 0.25F);

  const scene::Scene evaluated =
      model.evaluate(doc::NodeId{1U}, scene::SceneTime{0U, 60U, 1U});
  SPATIAL_CHECK(evaluated.instances.size() == 1U);
  const auto &program =
      evaluated.instances.front().style.effects.spatial_operations;
  SPATIAL_CHECK(program.size() == 2U);
  SPATIAL_CHECK(program[0U].kind == scene::EffectKind::BoxBlur);
  SPATIAL_CHECK(program[0U].box_blur.radius_px == 3U);
  SPATIAL_CHECK(program[1U].kind == scene::EffectKind::DirectionalBlur);
  SPATIAL_CHECK(evaluated.effect_settings.directional_blur_taps == 17U);
}

void identity_and_bounds_contracts_are_exact() {
  SPATIAL_CHECK(scene::box_blur_is_identity(scene::BoxBlurParameters{0U, 8U}));
  SPATIAL_CHECK(!scene::box_blur_is_identity(scene::BoxBlurParameters{2U, 3U}));
  SPATIAL_CHECK(scene::box_blur_bounds(scene::BoxBlurParameters{2U, 3U}) ==
                (scene::EffectBoundsExpansion{6.0F, 6.0F}));
  SPATIAL_CHECK(scene::directional_blur_is_identity(
      scene::DirectionalBlurParameters{0.0F, 0.33F}));

  const auto horizontal = scene::directional_blur_bounds(
      scene::DirectionalBlurParameters{10.0F, 0.0F});
  SPATIAL_CHECK(std::abs(horizontal.x - 5.0F) <= 1.0e-5F);
  SPATIAL_CHECK(std::abs(horizontal.y) <= 1.0e-5F);
  const auto vertical = scene::directional_blur_bounds(
      scene::DirectionalBlurParameters{10.0F, 0.25F});
  SPATIAL_CHECK(std::abs(vertical.x) <= 1.0e-5F);
  SPATIAL_CHECK(std::abs(vertical.y - 5.0F) <= 1.0e-5F);
}

void malformed_spatial_nodes_fail_closed() {
  doc::Root root = spatial_root();
  root = root.set(node(22U, schema::effect_type,
                       {whole(schema::effect_target, 10),
                        text(schema::effect_kind, schema::box_blur_kind),
                        whole(schema::box_blur_radius, 1),
                        whole(schema::box_blur_passes, 0)}));
  root = root.set(node(
      23U, schema::effect_type,
      {whole(schema::effect_target, 10),
       text(schema::effect_kind, schema::directional_blur_kind),
       real(schema::directional_blur_length,
            static_cast<double>(scene::max_directional_blur_length_px) + 1.0),
       real(schema::directional_blur_angle_turns, 0.0)}));
  root = root.set(node(24U, schema::effect_type,
                       {whole(schema::effect_target, 10),
                        text(schema::effect_kind, schema::box_blur_kind),
                        real(schema::box_blur_radius, 1.5),
                        whole(schema::box_blur_passes, 1)}));
  scene::SceneModel model;
  model.reset(root);
  SPATIAL_CHECK(model.effects().size() == 2U);
  SPATIAL_CHECK(model.counters().rejected >= 3U);
}

void spatial_state_participates_in_the_scene_digest() {
  scene::SceneModel model;
  model.reset(spatial_root());
  scene::Scene first =
      model.evaluate(doc::NodeId{1U}, scene::SceneTime{0U, 60U, 1U});
  scene::Scene second = first;
  second.instances.front()
      .style.effects.spatial_operations[1U]
      .directional_blur.angle_turns = 0.5F;
  SPATIAL_CHECK(scene::digest(first) != scene::digest(second));
  second = first;
  second.effect_settings.directional_blur_taps = 19U;
  SPATIAL_CHECK(scene::digest(first) != scene::digest(second));
}

void primary_spatial_channels_sample_into_typed_parameters() {
  doc::Root root = spatial_root();
  root = root.set(channel(40U, 20U, schema::box_blur_radius, 2.5));
  root = root.set(
      channel(41U, 21U, schema::directional_blur_length, 999.0));

  scene::SceneModel model;
  model.reset(root);
  const scene::Scene evaluated =
      model.evaluate(doc::NodeId{1U}, scene::SceneTime{0U, 60U, 1U});
  SPATIAL_CHECK(evaluated.effects.size() == 2U);
  // std::round defines the contract here: exact half-way values go away from
  // zero before the typed radius is clamped.
  SPATIAL_CHECK(evaluated.effects[0U].box_blur.radius_px == 3U);
  SPATIAL_CHECK(evaluated.effects[1U].directional_blur.length_px ==
                scene::max_directional_blur_length_px);
  SPATIAL_CHECK(
      evaluated.instances.front().style.effects.spatial_operations[0U]
          .box_blur.radius_px == 3U);
  SPATIAL_CHECK(
      evaluated.instances.front().style.effects.spatial_operations[1U]
          .directional_blur.length_px ==
      scene::max_directional_blur_length_px);

  const auto keys = model.channel_keyframes();
  SPATIAL_CHECK(keys.size() == 2U);
  SPATIAL_CHECK(keys[0U].component ==
                scene::PropertyChannel::BoxBlurRadius);
  SPATIAL_CHECK(keys[1U].component ==
                scene::PropertyChannel::DirectionalBlurLength);
}

struct Case final {
  const char *name;
  void (*function)();
};

constexpr std::array<Case, 5U> cases{{
    {"typed scene projection preserves spatial order",
     &typed_scene_projection_preserves_spatial_order},
    {"identity and bounds contracts are exact",
     &identity_and_bounds_contracts_are_exact},
    {"malformed spatial nodes fail closed",
     &malformed_spatial_nodes_fail_closed},
    {"spatial state participates in the scene digest",
     &spatial_state_participates_in_the_scene_digest},
    {"primary spatial channels sample into typed parameters",
     &primary_spatial_channels_sample_into_typed_parameters},
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
