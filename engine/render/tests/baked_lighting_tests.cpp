#include <mediaserver/net/session_protocol.hpp>
#include <mediaserver/render/baked_lighting.hpp>
#include <mediaserver/render/model_loader.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;
namespace doc = mediaserver::doc;
namespace net = mediaserver::net;
namespace rhi = mediaserver::rhi;
void check(bool ok, const char *text, int line) {
  if (!ok)
    throw std::runtime_error(std::to_string(line) + ": " + text);
}
#define CHECK(...)                                                             \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)
std::string utf8(const std::filesystem::path &path) {
  const auto s = path.u8string();
  return {s.begin(), s.end()};
}
scene::Scene fixture() {
  scene::Scene s;
  s.canvas = {64, 32};
  s.camera.eye = {0, 1, 5};
  s.camera.target = {0, 0, 0};
  // A uniform default fill: this unlit scene's only light. The background
  // colour is deliberately different, because it lights nothing.
  s.environment.fill_sky_color = {0.25F, 0.5F, 1};
  s.environment.ground_color = s.environment.fill_sky_color;
  s.environment.sky_color = {1, 0, 0};
  s.environment.baked.minimum = {-0.5F, -0.5F, -0.5F};
  s.environment.baked.maximum = {0.5F, 0.5F, 0.5F};
  s.environment.baked.count_x = 2;
  s.environment.baked.count_y = 2;
  s.environment.baked.count_z = 2;
  s.environment.baked.samples = 1024;
  s.environment.baked.bounces = 4;
  return s;
}
scene::MeshInstance box(scene::Vec3 center, scene::Vec3 scale,
                        scene::Vec3 color, scene::Vec3 emission = {}) {
  scene::MeshInstance b;
  b.mesh = {scene::Primitive::Box, 1};
  b.world = scene::multiply(scene::translation(center), scene::scaling(scale));
  b.normal = scene::normal_basis(b.world);
  b.material.base_color = color;
  b.material.emissive = emission;
  return b;
}
void analytic_environment_and_determinism() {
  auto s = fixture();
  const auto a = render::bake_lighting(s);
  CHECK(a.ready());
  for (auto n : {scene::Vec3{1, 0, 0}, scene::Vec3{0, 1, 0},
                 scene::Vec3{0, 0, 1}, scene::Vec3{0, -1, 0}}) {
    const auto e =
        render::sample_probe_irradiance(*a.asset, {0.123F, 0.2F, -0.3F}, n);
    CHECK(std::abs(e.x / 3.14159265F - 0.25F) < 0.005F);
    CHECK(std::abs(e.y / 3.14159265F - 0.5F) < 0.005F);
    CHECK(std::abs(e.z / 3.14159265F - 1) < 0.005F);
  }
  auto moved = s;
  moved.camera.eye = {99, 88, 77};
  moved.show_frame = 987654321ULL;
  const auto b = render::bake_lighting(moved);
  CHECK(b.ready());
  CHECK(render::encode_probe_asset(*a.asset) ==
        render::encode_probe_asset(*b.asset));
  auto bytes = render::encode_probe_asset(*a.asset);
  const auto decoded = render::decode_probe_asset(bytes);
  CHECK(decoded.ready());
  CHECK(decoded.asset->probes == a.asset->probes);
  bytes[120] ^= 4;
  CHECK(!render::decode_probe_asset(bytes).ready());
  bytes.resize(24);
  CHECK(!render::decode_probe_asset(bytes).ready());
}
void closed_box_transport_and_light_leaks() {
  auto s = fixture();
  s.instances.push_back(
      box({0, 0, 0}, {4, 4, 4}, {0.5F, 0.5F, 0.5F}, {0.2F, 0.2F, 0.2F}));
  const auto a = render::bake_lighting(s);
  CHECK(a.ready());
  // Five surface visits with albedo 1/2 and emission 1/5:
  // L = .2 * (1 + .5 + .25 + .125 + .0625) = .3875.
  const auto e =
      render::sample_probe_irradiance(*a.asset, {0, 0, 0}, {0, 1, 0});
  CHECK(std::abs(e.x / 3.14159265F - 0.3875F) < 0.008F);
  s.instances[0].material.emissive = {};
  s.environment.baked.minimum = {-3, -0.5F, -0.5F};
  s.environment.baked.maximum = {1, 0.5F, 0.5F};
  const auto dark = render::bake_lighting(s);
  CHECK(dark.ready());
  // The left probes see the bright environment outside the box. The right
  // probes are in its sealed interior. Interpolation must reject the former.
  const auto interior =
      render::sample_probe_irradiance(*dark.asset, {-0.5F, 0, 0}, {1, 0, 0});
  CHECK(interior.x < 0.015F && interior.y < 0.015F && interior.z < 0.015F);
  const auto exterior =
      render::sample_probe_irradiance(*dark.asset, {-3, 0, 0}, {-1, 0, 0});
  CHECK(exterior.z > 0.2F);
}
void coloured_bounce_and_dynamic_exclusion() {
  auto s = fixture();
  s.environment.fill_sky_color = {};
  s.environment.ground_color = {};
  s.environment.baked.minimum = {-0.5F, 0.1F, -0.5F};
  s.environment.baked.maximum = {0.5F, 0.4F, 0.5F};
  s.instances.push_back(box({0, -0.1F, 0}, {8, 0.2F, 8}, {0.8F, 0.02F, 0.01F}));
  scene::DirectionalLight light;
  light.direction = {0, -1, 0};
  light.intensity = 3;
  s.directional_lights.push_back(light);
  const auto bake = render::bake_lighting(s);
  CHECK(bake.ready());
  const auto e =
      render::sample_probe_irradiance(*bake.asset, {0, 0.2F, 0}, {0, -1, 0});
  CHECK(e.x > 0.5F && e.x > e.y * 10);
  s.directional_lights[0].bake_indirect = false;
  const auto live = render::bake_lighting(s);
  CHECK(live.ready());
  CHECK(
      render::sample_probe_irradiance(*live.asset, {0, 0.2F, 0}, {0, -1, 0}).x <
      0.0001F);
  s.directional_lights[0].bake_indirect = true;
  s.instances[0].bake_static = false;
  const auto moving = render::bake_lighting(s);
  CHECK(moving.ready());
  CHECK(render::sample_probe_irradiance(*moving.asset, {0, 0.2F, 0}, {0, -1, 0})
            .x < 0.0001F);
}
void ies_transport_obeys_the_measured_web() {
  const auto path = std::filesystem::temp_directory_path() / "mediaserver-bake-ies.ies";
  { std::ofstream file(path); file << "IESNA:LM-63-2002\nTILT=NONE\n1 1000 1 4 1 1 2 0 0 0 1 1 10\n0 60 90 180\n0\n100 100 0 0\n"; CHECK(file); }
  auto s = fixture(); s.environment.fill_sky_color = s.environment.ground_color = {};
  s.environment.baked.samples = 256; s.environment.baked.bounces = 1;
  s.environment.baked.minimum = {-0.5F, 0.1F, -0.5F};
  s.environment.baked.maximum = {0.5F, 0.4F, 0.5F};
  s.instances.push_back(box({0, -0.1F, 0}, {8, 0.2F, 8}, {0.5F, 0.5F, 0.5F}));
  scene::PointLight lamp; lamp.position = {0, 2, 0}; lamp.direction = {0, -1, 0};
  lamp.intensity = 1000; lamp.unit = scene::LightUnit::Candela; lamp.ies_source = utf8(path);
  s.point_lights.push_back(lamp);
  const auto lit = render::bake_lighting(s); CHECK(lit.ready());
  const auto sample = [](const auto& result) { return render::sample_probe_irradiance(*result.asset, {0, 0.2F, 0}, {0, -1, 0}).y; };
  CHECK(sample(lit) > 0.01F);
  s.point_lights[0].direction = {0, 1, 0};
  const auto dark = render::bake_lighting(s); CHECK(dark.ready()); CHECK(sample(dark) < 0.00001F);
  s.point_lights[0].ies_source += ".missing";
  CHECK(!render::bake_lighting(s).ready());
  std::filesystem::remove(path);
}

void area_transport_obeys_shape_and_sides() {
  auto s = fixture(); s.environment.fill_sky_color = s.environment.ground_color = {};
  s.environment.baked.samples = 256; s.environment.baked.bounces = 1;
  s.environment.baked.minimum = {-0.5F, 0.1F, -0.5F};
  s.environment.baked.maximum = {0.5F, 0.4F, 0.5F};
  s.instances.push_back(box({0, -0.1F, 0}, {8, 0.2F, 8}, {0.5F, 0.5F, 0.5F}));
  scene::PointLight lamp; lamp.position = {0, 2, 0}; lamp.direction = {0, -1, 0};
  lamp.kind = scene::LocalLightKind::Rectangle; lamp.width = 2; lamp.height = 1;
  lamp.intensity = 1000; lamp.unit = scene::LightUnit::Nits; s.point_lights.push_back(lamp);
  const auto sample = [](const auto& result) { CHECK(result.ready()); return render::sample_probe_irradiance(*result.asset, {0, 0.2F, 0}, {0, -1, 0}).y; };
  const auto a = render::bake_lighting(s); const float lit = sample(a); CHECK(lit > 0.01F);
  const auto repeated = render::bake_lighting(s); CHECK(a.asset->probes == repeated.asset->probes);
  s.point_lights[0].direction = {0, 1, 0}; CHECK(sample(render::bake_lighting(s)) < 0.00001F);
  s.point_lights[0].two_sided = true; CHECK(std::abs(sample(render::bake_lighting(s)) / lit - 1) < 0.002F);
  s.point_lights[0].kind = scene::LocalLightKind::Sphere; s.point_lights[0].radius = 0.4F;
  const float small = sample(render::bake_lighting(s)); CHECK(small > 0.001F);
  s.point_lights[0].radius = 0.8F; CHECK(sample(render::bake_lighting(s)) > small * 2);
  s.point_lights[0].bake_indirect = false; CHECK(sample(render::bake_lighting(s)) < 0.00001F);
}

void photometric_transport_matches_live_units() {
  auto s = fixture();
  s.environment.fill_sky_color = {};
  s.environment.ground_color = {};
  s.environment.baked.samples = 256;
  s.environment.baked.bounces = 1;
  s.environment.baked.minimum = {-0.5F, 0.1F, -0.5F};
  s.environment.baked.maximum = {0.5F, 0.4F, 0.5F};
  s.instances.push_back(box({0, -0.1F, 0}, {8, 0.2F, 8}, {0.5F, 0.5F, 0.5F}));
  scene::DirectionalLight sun;
  sun.intensity = 3.14159265F;
  s.directional_lights.push_back(sun);
  const auto baseline = render::bake_lighting(s); CHECK(baseline.ready());
  s.directional_lights[0].use_color_temperature = true;
  const auto neutral = render::bake_lighting(s); CHECK(neutral.ready());
  CHECK(baseline.asset->probes == neutral.asset->probes);
  s.directional_lights[0].unit = scene::LightUnit::Lux;
  s.directional_lights[0].intensity = 1000;
  const auto lux = render::bake_lighting(s); CHECK(lux.ready());
  const auto sample = [](const auto& result) {
    return render::sample_probe_irradiance(*result.asset, {0, 0.2F, 0}, {0, -1, 0});
  };
  const auto reference = sample(baseline);
  CHECK(reference.y > 0.5F);
  CHECK(std::abs(sample(lux).y / reference.y - 1) < 0.001F);
  const auto key = render::bake_input_digest(s);
  s.renderer_settings.photometric_scale *= 2;
  CHECK(render::bake_input_digest(s) != key);
  const auto doubled = render::bake_lighting(s); CHECK(doubled.ready());
  CHECK(std::abs(sample(doubled).y / reference.y - 2) < 0.001F);
  s.renderer_settings.photometric_scale /= 2;
  s.directional_lights[0].color_temperature_kelvin = 3200;
  const auto warm = render::bake_lighting(s); CHECK(warm.ready());
  CHECK(sample(warm).x > sample(warm).z * 2);
  s.directional_lights[0].color_temperature_kelvin = 8000;
  const auto cool = render::bake_lighting(s); CHECK(cool.ready());
  CHECK(sample(cool).z > sample(cool).x * 1.25F);
  s.directional_lights.clear();
  scene::PointLight lamp;
  lamp.position = {0, 2, 0}; lamp.range = 10;
  lamp.intensity = 400; lamp.unit = scene::LightUnit::Candela;
  s.point_lights.push_back(lamp);
  const auto candela = render::bake_lighting(s); CHECK(candela.ready());
  CHECK(sample(candela).y > 0.01F);
  s.point_lights[0].unit = scene::LightUnit::Lumens;
  s.point_lights[0].intensity = 5026.548246F;
  const auto lumens = render::bake_lighting(s); CHECK(lumens.ready());
  CHECK(std::abs(sample(lumens).y / sample(candela).y - 1) < 0.001F);
  s.point_lights[0].kind = scene::LocalLightKind::Spot;
  s.point_lights[0].use_color_temperature = true;
  s.point_lights[0].color_temperature_kelvin = 3200;
  const auto spot = render::bake_lighting(s); CHECK(spot.ready());
  CHECK(sample(spot).x > sample(spot).z * 2);
  s.point_lights[0].range = 2;
  const auto range = render::bake_lighting(s); CHECK(range.ready());
  CHECK(sample(range).x == 0 && sample(range).y == 0 && sample(range).z == 0);
}
void probe_visibility_is_continuous_across_direction_cells() {
  render::ProbeAsset field;
  field.grid = fixture().environment.baked;
  field.grid.minimum = {-1, -1, -1};
  field.grid.maximum = {1, 1, 1};
  field.probes.resize(8);
  for (unsigned i = 0; i < field.probes.size(); ++i) {
    auto &probe = field.probes[i];
    const float energy = i == 0 ? 3.5449077F : 0.35449077F;
    probe.values[0] = {energy, energy, energy, 0};
    for (unsigned cell = 0; cell < 64; ++cell) {
      const bool blocked = i == 0 && cell % 8 < 4;
      const float mean = blocked ? 0.1F : 1000.0F;
      const float moment = mean * mean + 0.00001F;
      auto &packed = probe.values[9 + cell / 2];
      if (cell % 2 == 0) {
        packed.x = mean;
        packed.y = moment;
      } else {
        packed.z = mean;
        packed.w = moment;
      }
    }
  }
  // Cross an octahedral cell boundary on a flat receiver. Nearest-cell
  // moments made this arbitrarily small movement produce a visible patch.
  const auto left =
      render::sample_probe_irradiance(field, {-1.0001F, 0, 0}, {0, 1, 0});
  const auto right =
      render::sample_probe_irradiance(field, {-0.9999F, 0, 0}, {0, 1, 0});
  CHECK(left.x > 0.3F && right.x > 0.3F);
  CHECK(std::abs(left.x - right.x) < 0.005F);
}
void authored_round_trip() {
  scene::BakedLighting value;
  value.enabled = true;
  value.minimum = {-7, -3, -9};
  value.maximum = {8, 4, 10};
  value.count_x = 3;
  value.count_y = 5;
  value.count_z = 7;
  value.samples = 512;
  value.bounces = 6;
  value.strength = 0.75F;
  value.source = "test-lighting.msgi";
  value.content = "fedcba9876543210";
  doc::Node stage;
  stage.id = doc::NodeId{1};
  stage.type = std::string{scene::schema::stage_type};
  stage.properties = scene::baked_lighting_properties(value);
  doc::Root root;
  root = root.set(stage);
  scene::SceneModel model;
  model.reset(root);
  const auto evaluated = model.evaluate({0, 60, 1});
  CHECK(evaluated.environment.baked == value);
  auto other = evaluated;
  other.environment.baked.samples = 1024;
  CHECK(scene::digest(other) != scene::digest(evaluated));
  other = evaluated;
  other.environment.baked.content = "123456789abcdef0";
  CHECK(scene::asset_set_digest(other) != scene::asset_set_digest(evaluated));
  value.count_x = 0;
  value.count_y = 1000;
  value.samples = 1;
  value.bounces = 999;
  value.strength = std::numeric_limits<float>::infinity();
  const auto bounded = scene::clamped(value);
  CHECK(bounded.count_x == 2 && bounded.count_y == 16 &&
        bounded.samples == 64 && bounded.bounces == 16 &&
        bounded.strength == 1);
}
void small_hdr_highlights_survive_default_bake_sampling() {
  auto s = fixture();
  const auto path = std::filesystem::temp_directory_path() /
                    "mediaserver-small-bake-highlight.hdr";
  constexpr unsigned width = 512, height = 256;
  std::vector<core::u8> pixels(width * height * 4);
  // A tiny 900-radiance softbox centred around 45 degrees elevation.
  // Uniform 256-ray sampling misses it; its integrated flux remains known.
  for (unsigned y = 63; y <= 64; ++y)
    for (unsigned x = 255; x <= 256; ++x) {
      const auto at = (y * width + x) * 4;
      pixels[at] = pixels[at + 1] = pixels[at + 2] = 225;
      pixels[at + 3] = 138;
    }
  {
    std::ofstream file(path, std::ios::binary);
    file << "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 256 +X 512\n";
    file.write(reinterpret_cast<const char *>(pixels.data()),
               static_cast<std::streamsize>(pixels.size()));
  }
  s.environment.map_source = utf8(path);
  s.environment.map_intensity = 0.13F;
  s.environment.baked.samples = 256;
  const auto baked = render::bake_lighting(s);
  CHECK(baked.ready());
  const auto irradiance =
      render::sample_probe_irradiance(*baked.asset, {}, {0, 1, 0});
  constexpr double pi = 3.141592653589793;
  const double solid_angle = (4 * pi / width) * (std::cos(pi * 63 / height) -
                                                 std::cos(pi * 65 / height));
  const double expected = 900 * 0.13 * solid_angle * std::cos(pi / 4);
  CHECK(std::abs(irradiance.x - expected) < expected * 0.15);
  // Importance sampling must not introduce ambient through closed walls.
  s.instances.push_back(box({}, {4, 4, 4}, {}));
  const auto enclosed = render::bake_lighting(s);
  CHECK(enclosed.ready());
  CHECK(render::sample_probe_irradiance(*enclosed.asset, {}, {0, 1, 0}).x <
        0.00001F);
  std::filesystem::remove(path);
}
void residency_and_sticky_failure() {
  const auto bake = render::bake_lighting(fixture());
  CHECK(bake.ready());
  const auto directory =
      std::filesystem::temp_directory_path() / "mediaserver-baked-lighting-tests";
  std::filesystem::create_directories(directory);
  const auto path = directory / "sticky.msgi";
  std::error_code ec;
  std::filesystem::remove(path, ec);
  render::ProbeCache cache;
  static_cast<void>(cache.observe(utf8(path), bake.asset->content));
  cache.wait_for_test();
  CHECK(cache.observe(utf8(path), bake.asset->content).state ==
        render::ProbeCacheState::Failed);
  const auto bytes = render::encode_probe_asset(*bake.asset);
  {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  }
  CHECK(cache.observe(utf8(path), bake.asset->content).state ==
        render::ProbeCacheState::Failed);
  CHECK(cache.refresh(utf8(path)));
  cache.wait_for_test();
  const auto ready = cache.observe(utf8(path), bake.asset->content);
  CHECK(ready.asset);
  CHECK(!cache
             .observe(utf8(path), bake.asset->content,
                      ready.publication_epoch - 1)
             .asset);
  std::filesystem::remove(path, ec);
  CHECK(cache.observe(utf8(path), bake.asset->content).asset == ready.asset);
  CHECK(cache.stats().admissions >= 1 && cache.stats().failures >= 1);
  const auto written =
      render::write_probe_asset(*bake.asset, utf8(directory / "immutable"));
  CHECK(written.ready());
  CHECK(render::load_probe_asset(written.source, bake.asset->content).ready());
  CHECK(!render::load_probe_asset(written.source, "bad-generation").ready());
}
void cohort_commit_is_exact() {
  doc::Node saved_stage;
  saved_stage.id = doc::NodeId{100031};
  saved_stage.type = std::string{scene::schema::stage_type};
  saved_stage.properties =
      scene::baked_lighting_properties(fixture().environment.baked);
  saved_stage.properties.emplace(
      std::string{scene::schema::asset_commit_control},
      doc::PropertyValue{core::i64{909}});
  saved_stage.properties.emplace(
      std::string{scene::schema::asset_commit_digest},
      doc::PropertyValue{core::i64{17}});
  saved_stage.properties.emplace(std::string{scene::schema::asset_commit_show},
                                 doc::PropertyValue{core::i64{2424}});
  const auto saved = doc::Root{}.set(saved_stage);
  const auto reopened = scene::without_asset_commits(saved);
  scene::SceneModel reopened_model;
  reopened_model.reset(reopened);
  const auto reopened_scene = reopened_model.evaluate({2424, 24, 1});
  CHECK(reopened_scene.asset_commit == core::AssetCommit{});
  CHECK(reopened_scene.environment.baked == fixture().environment.baked);
  CHECK(saved.get(saved_stage.id)
            ->properties.contains(
                std::string{scene::schema::asset_commit_control}));
  CHECK(scene::without_asset_commits(reopened).same_snapshot(reopened));
  core::AssetPublicationGate a, b;
  core::AssetSetReadiness ar{17, true, 0}, br{17, false, 0};
  core::AssetCohort blocked;
  blocked.observe(&ar);
  blocked.observe(&br);
  CHECK(!blocked.can_commit());
  CHECK(!a.present(ar, {}, 9, 99, true));
  CHECK(!b.present(br, {}, 9, 99, true));
  br.ready = true;
  core::AssetCohort complete;
  complete.observe(&ar);
  complete.observe(&br);
  CHECK(complete.can_commit());
  CHECK(complete.ready_members == 2);
  core::AssetCohort replacing;
  const core::AssetSetReadiness previous{11, true, 5};
  replacing.observe(&ar);
  replacing.observe(&previous);
  CHECK(!replacing.can_commit() && replacing.ready_members == 1);
  const core::AssetCommit permit{17, 10, 100};
  CHECK(core::asset_presentation_frame(ar, permit, 9) == 10);
  CHECK(core::asset_presentation_frame(ar, permit, 10) == 10);
  CHECK(core::asset_presentation_frame(ar, permit, 11) == 0);
  CHECK(core::asset_presentation_frame({17, false, 0}, permit, 9) == 0);
  CHECK(core::asset_presentation_frame({17, true, 10}, permit, 10) == 0);
  CHECK(!a.present(ar, permit, 9, 99, true));
  CHECK(a.present(ar, permit, 10, 100, true));
  CHECK(b.present(br, permit, 10, 100, true));
  CHECK(a.visible_commit() == 10 && b.visible_commit() == 10);
  ar.digest = 29;
  br.digest = 29;
  br.ready = false;
  blocked = {};
  blocked.observe(&ar);
  blocked.observe(&br);
  CHECK(!blocked.can_commit());
  CHECK(!a.present(ar, permit, 11, 101, true));
  br.ready = true;
  const core::AssetCommit replacement{29, 20, 110};
  CHECK(a.present(ar, replacement, 20, 110, true));
  CHECK(b.present(br, replacement, 20, 110, true));
  core::AssetPublicationGate late;
  CHECK(!late.present(br, replacement, 21, 111, true));
  CHECK(late.visible_commit() == 0);
  core::AssetPublicationGate parked;
  CHECK(!parked.present(br, replacement, 21, 110, true));
}
void stale_worker_result_and_handle_release() {
  const auto baked = render::bake_lighting(fixture());
  CHECK(baked.ready());
  const auto directory =
      std::filesystem::temp_directory_path() / "mediaserver-baked-lighting-tests";
  const auto written =
      render::write_probe_asset(*baked.asset, utf8(directory / "fenced"));
  CHECK(written.ready());
  render::ProbeCache cache;
  cache.wait_for_test();
  std::promise<void> entered, release;
  auto entered_future = entered.get_future();
  auto released = release.get_future().share();
  cache.set_next_load_gate_for_test(
      std::make_shared<const std::function<void()>>([&] {
        entered.set_value();
        released.wait();
      }));
  const auto old =
      cache.observe(written.source, written.asset->content, 0, 1001);
  const bool blocked = entered_future.wait_for(std::chrono::seconds{5}) ==
                       std::future_status::ready;
  if (!blocked)
    release.set_value();
  CHECK(blocked);
  const auto new_generation =
      cache.observe(written.source, written.asset->content, 0, 1002);
  const auto before = std::chrono::steady_clock::now();
  {
    auto handle = std::make_unique<render::ProbeCache>();
    static_cast<void>(handle->observe(written.source, written.asset->content));
  }
  const auto elapsed = std::chrono::steady_clock::now() - before;
  release.set_value();
  cache.wait_for_test();
  CHECK(elapsed < std::chrono::milliseconds{100});
  CHECK(new_generation.generation > old.generation);
  CHECK(cache.stats().late_results > 0);
  const auto ready =
      cache.observe(written.source, written.asset->content, 0, 1002);
  CHECK(ready.state == render::ProbeCacheState::Ready && ready.asset);
  CHECK(ready.generation == new_generation.generation);
}
void cancellation_and_snapshot_fencing() {
  auto s = fixture();
  const auto identity = render::bake_input_digest(s);
  s.camera.eye.x += 7;
  s.show_frame += 99;
  CHECK(render::bake_input_digest(s) == identity);
  s.environment.baked.source = "prior.msgi";
  CHECK(render::bake_input_digest(s) == identity);
  s.environment.fog_background_distance = 5.0F;
  CHECK(render::bake_input_digest(s) == identity);
  s.instances.push_back(box({0, 0, 0}, {2, 2, 2}, {0.5F, 0.5F, 0.5F}));
  CHECK(render::bake_input_digest(s) != identity);
  s.instances.back().bake_static = false;
  CHECK(render::bake_input_digest(s) == identity);
  render::BakeProgress cancelled;
  cancelled.cancel = true;
  CHECK(!render::bake_lighting(s, &cancelled).ready());
  s.environment.baked.count_x = 16;
  s.environment.baked.count_y = 16;
  s.environment.baked.count_z = 16;
  s.environment.baked.samples = 65536;
  auto job =
      render::BakeJob::start(s, utf8(std::filesystem::temp_directory_path() /
                                     "mediaserver-baked-lighting-tests" / "cancelled"));
  CHECK(job);
  job->cancel();
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds{5};
  while (!job->result() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
  CHECK(job->result());
  CHECK(job->progress().stage.load() == render::BakeStage::Cancelled);
}
void heartbeat_carries_nondefault_asset_evidence() {
  net::HeartbeatAck ack;
  ack.session_id = {1, 2};
  ack.director_epoch = {3, 4};
  ack.sequence = 8;
  ack.acknowledged_sequence = 9;
  ack.visible_through = {mediaserver::cmd::FrameNumber{6}, doc::Version{5},
                         mediaserver::cmd::CommandId{7}};
  ack.renderer.profile_digest = 13;
  ack.renderer.state = net::RuntimeRendererReadinessState::Ready;
  ack.renderer.requested = net::RuntimeRendererBackend::Advanced;
  ack.renderer.selected = ack.renderer.requested;
  ack.renderer.satisfied = true;
  ack.renderer.assets = {0xfedcba9876543210ULL, true, 123456};
  const auto bytes = net::encode_session(net::SessionMessage{ack});
  CHECK(bytes);
  const auto decoded = net::decode_session(bytes.value());
  CHECK(decoded);
  CHECK(std::get<net::HeartbeatAck>(decoded.value()).renderer.assets ==
        ack.renderer.assets);
}
int main() {
  unsigned failed = 0;
  for (const auto &test :
       {std::pair{"analytic environment and deterministic bytes",
                  analytic_environment_and_determinism},
        std::pair{"closed box and visibility",
                  closed_box_transport_and_light_leaks},
        std::pair{"coloured bounce and dynamic lights",
                  coloured_bounce_and_dynamic_exclusion},
        std::pair{"authored state", authored_round_trip},
        std::pair{"IES baked transport", ies_transport_obeys_the_measured_web},
        std::pair{"area light transport", area_transport_obeys_shape_and_sides},
        std::pair{"photometric transport", photometric_transport_matches_live_units},
        std::pair{"small HDR highlights",
                  small_hdr_highlights_survive_default_bake_sampling},
        std::pair{"residency", residency_and_sticky_failure},
        std::pair{"cohort commit", cohort_commit_is_exact},
        std::pair{"continuous probe visibility",
                  probe_visibility_is_continuous_across_direction_cells},
        std::pair{"heartbeat asset evidence",
                  heartbeat_carries_nondefault_asset_evidence},
        std::pair{"stale worker and handle release",
                  stale_worker_result_and_handle_release},
        std::pair{"cancellation and snapshot",
                  cancellation_and_snapshot_fencing}}) {
    try {
      test.second();
      std::cout << "[pass] " << test.first << '\n';
    } catch (const std::exception &e) {
      ++failed;
      std::cerr << "[fail] " << test.first << ": " << e.what() << '\n';
    }
  }
  return failed ? 1 : 0;
}
