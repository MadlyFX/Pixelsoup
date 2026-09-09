#include <mediaserver/render/baked_lighting.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>
#include <mediaserver/render/model_animation.hpp>
#include <mediaserver/render/model_package.hpp>
#include <mediaserver/render/scene_bridge.hpp>
#include <mediaserver/render/scene_renderer.hpp>
#include <mediaserver/scene/scene_model.hpp>
#include <mediaserver/scene/scene_schema.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>

using namespace mediaserver;
#define CHECK(...)                                                             \
  do {                                                                         \
    if (!(__VA_ARGS__))                                                        \
      throw std::runtime_error(std::to_string(__LINE__) + ": " +               \
                               #__VA_ARGS__);                                  \
  } while (false)

namespace {
std::string fixture(const std::filesystem::path &directory,
                    bool rigid = false) {
  std::filesystem::create_directories(directory);
  std::vector<core::u8> bytes;
  auto floats = [&](std::initializer_list<float> values) {
    for (float value : values) {
      const auto bits = std::bit_cast<core::u32>(value);
      for (unsigned b = 0; b < 4; ++b)
        bytes.push_back(static_cast<core::u8>(bits >> (b * 8)));
    }
  };
  // Positions (0), normals (48), joints (96), weights (128), indices
  // (192), inverse bind matrices (216), key times (344), translations (352).
  floats({-.6F, -.5F, 0, .6F, -.5F, 0, .6F, .5F, 0, -.6F, .5F, 0});
  floats({0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1});
  for (unsigned i = 0; i < 4; ++i)
    for (unsigned j = 0; j < 4; ++j) {
      bytes.push_back(j == 1 ? 1U : 0U);
      bytes.push_back(0);
    }
  floats({1, 0, 0, 0, 1, 0, 0, 0, .5F, .5F, 0, 0, .5F, .5F, 0, 0});
  for (core::u32 i : {0U, 1U, 2U, 0U, 2U, 3U})
    for (unsigned b = 0; b < 4; ++b)
      bytes.push_back(static_cast<core::u8>(i >> (b * 8)));
  for (unsigned joint = 0; joint < 2; ++joint) {
    auto inverse = scene::identity();
    if (joint)
      inverse.m[12] = -.2F;
    for (float value : inverse.m)
      floats({value});
  }
  floats({0, 1});
  floats(rigid ? std::initializer_list<float>{.1F, 0, 0, .6F, 0, 0}
               : std::initializer_list<float>{.2F, 0, 0, .7F, 0, 0});
  CHECK(bytes.size() == 376U);
  const std::string stem = rigid ? "rigid" : "skin";
  std::ofstream binary{directory / (stem + ".bin"), std::ios::binary};
  binary.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  binary.close();
  std::ostringstream json;
  json << R"({"asset":{"version":"2.0"},"buffers":[{"uri":")" << stem
       << R"(.bin","byteLength":376}],"bufferViews":[
{"buffer":0,"byteOffset":0,"byteLength":48},{"buffer":0,"byteOffset":48,"byteLength":48},
{"buffer":0,"byteOffset":96,"byteLength":32},{"buffer":0,"byteOffset":128,"byteLength":64},
{"buffer":0,"byteOffset":192,"byteLength":24},{"buffer":0,"byteOffset":216,"byteLength":128},
{"buffer":0,"byteOffset":344,"byteLength":8},{"buffer":0,"byteOffset":352,"byteLength":24}],"accessors":[
{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[-0.6,-0.5,0],"max":[0.6,0.5,0]},
{"bufferView":1,"componentType":5126,"count":4,"type":"VEC3"},
{"bufferView":2,"componentType":5123,"count":4,"type":"VEC4"},
{"bufferView":3,"componentType":5126,"count":4,"type":"VEC4"},
{"bufferView":4,"componentType":5125,"count":6,"type":"SCALAR"},
{"bufferView":5,"componentType":5126,"count":2,"type":"MAT4"},
{"bufferView":6,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},
{"bufferView":7,"componentType":5126,"count":2,"type":"VEC3"}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"JOINTS_0":2,"WEIGHTS_0":3},"indices":4}]}],
"nodes":[{"mesh":0,"translation":[0.1,0,0])";
  if (!rigid)
    json << R"(,"skin":0)";
  json
      << R"(},{"children":[2]},{"translation":[0.2,0,0]}],"skins":[{"joints":[1,2],"inverseBindMatrices":5}],
"scenes":[{"nodes":[0,1]}],"scene":0,"animations":[{"name":"Bend","samplers":[{"input":6,"output":7,"interpolation":"LINEAR"}],"channels":[{"sampler":0,"target":{"node":)";
  json << (rigid ? 0 : 2) << R"(,"path":"translation"}}]}]})";
  const auto source = directory / (stem + ".gltf");
  std::ofstream output{source};
  output << json.str();
  output.close();
  return source.string();
}
void near(float a, float b) { CHECK(std::abs(a - b) < 2e-6F); }
std::vector<scene::Mat4> evaluate(const render::ModelAsset &asset,
                                  core::u64 frame,
                                  scene::ModelAnimation settings = {}) {
  auto scratch = std::make_unique<render::ModelPoseScratch>();
  std::vector<scene::Mat4> result(asset.pose_bindings.size());
  CHECK(render::evaluate_model_pose(asset, settings, frame, {60, 1}, *scratch,
                                    result));
  return result;
}
void cpu(const std::filesystem::path &dir) {
  const auto source = fixture(dir);
  render::ModelAsset asset;
  CHECK(render::load_model(source, asset));
  CHECK(asset.animation_nodes.size() == 3 && asset.pose_bindings.size() == 2 &&
        asset.animations.size() == 1);
  near(asset.mesh.vertices[0].position.x, -.5F);
  CHECK(asset.vertex_metadata[2].joints[1] == 1 &&
        asset.vertex_metadata[2].weights.x == .5F);
  const auto pose = evaluate(asset, 30);
  near(pose[0].m[12], -.1F);
  near(pose[1].m[12], .15F);
  CHECK(evaluate(asset, 30) == pose);
  CHECK(evaluate(asset, 90) == pose);
  const auto huge = std::numeric_limits<core::u64>::max();
  CHECK(evaluate(asset, huge) == evaluate(asset, huge % 60));
  CHECK(evaluate(asset, (1ULL << 60) + 1) != evaluate(asset, (1ULL << 60) + 2));
  scene::ModelAnimation settings;
  settings.start_frame = huge - 40;
  CHECK(evaluate(asset, huge - 10, settings) == pose);
  settings = {};
  settings.loop = false;
  near(evaluate(asset, huge, settings)[1].m[12], .4F);
  settings.enabled = false;
  near(evaluate(asset, 30, settings)[1].m[12], -.1F);
  CHECK(render::model_animation_time(24000, {24000, 1001}, {}, 1) == 0);
  near(static_cast<float>(
           render::model_animation_time(24001, {24000, 1001}, {}, 1)),
       1001.0F / 24000.0F);
  // The package preserves all channels, hierarchy, weights and inverse binds.
  const auto package = render::write_model_package(source, asset);
  CHECK(!package.empty());
  CHECK(package == render::write_model_package(source, asset));
  render::ModelAsset loaded;
  CHECK(render::load_model_package(package, loaded));
  CHECK(asset.animations == loaded.animations &&
        asset.animation_nodes == loaded.animation_nodes &&
        asset.pose_bindings == loaded.pose_bindings);
  CHECK(asset.vertex_metadata == loaded.vertex_metadata);
  CHECK(evaluate(loaded, 30) == pose);
  // Version 2 has the same static payload but no trailing animation tables.
  auto legacy = asset;
  legacy.animation_nodes.clear();
  legacy.pose_bindings.clear();
  legacy.animations.clear();
  const auto static_package = render::write_model_package(source, legacy);
  CHECK(!static_package.empty());
  std::ifstream static_file{static_package, std::ios::binary};
  std::string old_bytes{std::istreambuf_iterator<char>{static_file}, {}};
  old_bytes.resize(old_bytes.size() - 20U);
  old_bytes[4] = 2;
  core::u64 old_hash = 14695981039346656037ULL;
  for (unsigned char b : old_bytes) {
    old_hash ^= b;
    old_hash *= 1099511628211ULL;
  }
  for (unsigned b = 0; b < 8; ++b)
    old_bytes.push_back(static_cast<char>(old_hash >> (b * 8)));
  const auto old_path = (dir / "legacy.msmodel").string();
  std::ofstream old_file{old_path, std::ios::binary};
  old_file.write(old_bytes.data(),
                 static_cast<std::streamsize>(old_bytes.size()));
  old_file.close();
  CHECK(render::load_model_package(old_path, loaded));
  CHECK(loaded.animation_nodes.empty());
  std::ifstream file{package, std::ios::binary};
  std::string data{std::istreambuf_iterator<char>{file}, {}};
  data[data.size() - 20] ^= 1;
  const auto corrupt = (dir / "corrupt.msmodel").string();
  std::ofstream bad{corrupt, std::ios::binary};
  bad.write(data.data(), static_cast<std::streamsize>(data.size()));
  bad.close();
  CHECK(!render::load_model_package(corrupt, loaded));
  auto invalid = asset;
  invalid.vertex_metadata[0].joints[0] = 4096;
  CHECK(!render::validate_model_animation(invalid));
  invalid = asset;
  invalid.animation_nodes[0].parent = 2;
  CHECK(!render::validate_model_animation(invalid));
  invalid = asset;
  invalid.animations[0].channels[0].times[1] = 0;
  CHECK(!render::validate_model_animation(invalid));
  invalid = asset;
  invalid.animations[0].channels.push_back(invalid.animations[0].channels[0]);
  CHECK(!render::validate_model_animation(invalid));
  invalid = asset;
  invalid.vertex_metadata[0].weights.x = -1;
  CHECK(!render::validate_model_animation(invalid));
  // Independent channel equations: STEP, Hermite and quaternion slerp.
  auto &channel = asset.animations[0].channels[0];
  channel.interpolation = render::ModelInterpolation::Step;
  near(evaluate(asset, 59)[1].m[12], -.1F);
  channel.interpolation = render::ModelInterpolation::CubicSpline;
  channel.values = {{0, 0, 0, 0}, {.2F, 0, 0, 0}, {1, 0, 0, 0},
                    {0, 0, 0, 0}, {.7F, 0, 0, 0}, {0, 0, 0, 0}};
  near(evaluate(asset, 30)[1].m[12], .275F);
  channel.interpolation = render::ModelInterpolation::Linear;
  channel.path = render::ModelAnimationPath::Rotation;
  channel.values = {{0, 0, 0, 1}, {0, 0, 1, 0}};
  const auto turn = evaluate(asset, 30)[1];
  near(turn.m[0], 0);
  near(turn.m[1], 1);
  near(turn.m[4], -1);
  channel.values[1] = {0, 0, 0, -1};
  near(evaluate(asset, 30)[1].m[0], 1);
  render::ModelAsset rigid;
  CHECK(render::load_model(fixture(dir, true), rigid));
  near(evaluate(rigid, 30)[0].m[12], .25F);
  settings = {};
  settings.wind = {.5F, 0, 0};
  settings.wind_period_frames = 120;
  near(render::model_wind(settings, 30).x, .5F);
  CHECK(render::model_wind(settings, huge) ==
        render::model_wind(settings, huge % 120));
  std::ofstream lod{source + ".lod"};
  lod << "MSLOD 1\n16 \"rigid.gltf\"\n";
  lod.close();
  CHECK(!render::load_model(source, loaded));
  std::filesystem::remove(source + ".lod");
  // Authored controls survive document evaluation and affect scene/bridge
  // identity.
  doc::Node node;
  node.id = doc::NodeId{9};
  node.type = std::string{scene::schema::model_type};
  node.properties = {
      {"source", source},
      {"animation_clip", std::int64_t{1}},
      {"animation_loop", false},
      {"animation_start_frame", std::int64_t{9007199254740993LL}},
      {"wind_x", .5},
      {"wind_period_frames", std::int64_t{90}}};
  scene::SceneModel model;
  model.reset(doc::Root{}.set(node));
  const auto s = model.evaluate({7, 60, 1});
  CHECK(s.models.size() == 1);
  CHECK(s.models[0].animation.start_frame == 9007199254740993ULL);
  CHECK(s.models[0].animation.clip == 1 && !s.models[0].animation.loop &&
        s.models[0].animation.wind.x == .5F);
  auto bridge = render::SceneBridge::create(4);
  CHECK(bridge);
  CHECK(bridge.value().update(s));
  for (unsigned field = 0; field < 8; ++field) {
    auto changed = s;
    auto &a = changed.models[0].animation;
    switch (field) {
    case 0:
      a.enabled = false;
      break;
    case 1:
      ++a.clip;
      break;
    case 2:
      a.loop = true;
      break;
    case 3:
      ++a.start_frame;
      break;
    case 4:
      a.wind.x += 1;
      break;
    case 5:
      a.wind.y = 1;
      break;
    case 6:
      a.wind.z = 1;
      break;
    default:
      ++a.wind_period_frames;
      break;
    }
    CHECK(scene::digest(changed) != scene::digest(s));
    CHECK(bridge.value().update(s));
    const auto change = bridge.value().update(changed);
    CHECK(change && change.value().updated == 1);
  }
  auto bake_scene = s;
  bake_scene.models[0].animation = {};
  CHECK(!render::bake_lighting(bake_scene).ready());
  std::cout << "[pass] glTF hierarchy/skin, interpolation, integer clock, "
               "validation, packages, controls, bake guard\n";
}
rhi::CommandList acquire(rhi::Device &d) {
  auto c = d.acquire_commands();
  CHECK(c);
  return std::move(c).value();
}
std::vector<core::u8> download(rhi::Device &d, rhi::CommandList c,
                               const rhi::Texture &texture, core::u32 stride) {
  auto rb = d.create_readback(texture.width() * texture.height() * stride);
  CHECK(rb);
  CHECK(c.download(texture, rb.value()));
  auto submitted = d.submit(std::move(c));
  CHECK(submitted);
  CHECK(submitted.value().wait());
  auto data = rb.value().map();
  std::vector<core::u8> result(data.begin(), data.end());
  rb.value().unmap();
  return result;
}
render::RendererLimits limits() {
  render::RendererLimits l;
  l.max_instances = 32;
  l.max_vertices = 128;
  l.max_indices = 256;
  l.directional_shadow_resolution = 32;
  l.shadow_atlas.extent = 32;
  l.max_history_slots = 2;
  l.max_temporal_history_targets = 2;
  return l;
}
void gpu(rhi::Device &device, const std::string &source) {
  render::ModelCache cache;
  static_cast<void>(cache.observe(source, cache.publication_epoch()));
  CHECK(render::detail::wait_for_shared_model_cache_for_test());
  auto full = render::SceneRenderer::create(
      device, 128, 64, limits(), render::SceneRendererMode::AdvancedLinear);
  CHECK(full);
  auto left = render::SceneRenderer::create(
      device, 63, 64, limits(), render::SceneRendererMode::AdvancedLinear);
  CHECK(left);
  auto right = render::SceneRenderer::create(
      device, 65, 64, limits(), render::SceneRendererMode::AdvancedLinear);
  CHECK(right);
  scene::Scene s;
  s.id = doc::NodeId{1};
  s.canvas = {128, 64};
  s.show_rate = {60, 1};
  s.camera.eye = {0, 0, 2};
  s.camera.target = {0, 0, 0};
  s.camera.field_of_view = .125F;
  s.renderer_settings.shadow_cascades = 0;
  s.renderer_settings.gi_strength = 0;
  s.renderer_settings.frustum_culling = true;
  s.renderer_settings.motion_blur = true;
  s.renderer_settings.motion_blur_radius = 16;
  s.environment.ambient = 0;
  s.environment.sky_color = s.environment.ground_color = {0, 0, 0};
  scene::ModelInstance instance;
  instance.world = instance.normal = scene::identity();
  instance.source = source;
  instance.source_node = doc::NodeId{3};
  instance.material.base_color = {0, 0, 0};
  instance.material.emissive = {2, 1, .25F};
  s.models.push_back(instance);
  const auto draw = [&](render::SceneRenderer &r, scene::CanvasRect slice) {
    auto c = acquire(device);
    CHECK(r.render(c, s, slice));
    return download(device, std::move(c), r.target(), 8);
  };
  const auto exact = [&](const auto &a, const auto &b, const auto &whole,
                         core::u32 stride) {
    for (std::size_t y = 0; y < 64; ++y) {
      CHECK(std::equal(a.begin() + y * 63 * stride,
                       a.begin() + (y + 1) * 63 * stride,
                       whole.begin() + y * 128 * stride));
      CHECK(std::equal(b.begin() + y * 65 * stride,
                       b.begin() + (y + 1) * 65 * stride,
                       whole.begin() + y * 128 * stride + 63 * stride));
    }
  };
  std::vector<core::u8> frame_zero;
  for (core::u64 f = 0; f < 32; ++f) {
    s.show_frame = f;
    const auto image = draw(full.value(), scene::whole(s.canvas));
    if (f == 0)
      frame_zero = image;
    const auto before_hold = download(
        device, acquire(device), full.value().object_velocity_target(), 16);
    CHECK(image == draw(full.value(), scene::whole(s.canvas)));
    CHECK(!full.value().stats().geometry_uploaded);
    exact(draw(left.value(), {0, 0, 63, 64}),
          draw(right.value(), {63, 0, 65, 64}), image, 8);
    const auto velocity = download(device, acquire(device),
                                   full.value().object_velocity_target(), 16);
    CHECK(velocity == before_hold);
    exact(download(device, acquire(device),
                   left.value().object_velocity_target(), 16),
          download(device, acquire(device),
                   right.value().object_velocity_target(), 16),
          velocity, 16);
    CHECK(velocity == download(device, acquire(device),
                               full.value().object_velocity_target(), 16));
    if (f == 30) {
      CHECK(image != frame_zero);
      // GPU object motion follows the independently projected displacement:
      // bottom vertices stay fixed; upper vertices move 0.25/60 per frame.
      bool moving = false;
      const auto top =
          scene::project_to_canvas(s.camera, s.canvas, {.125F, .5F, 0});
      const auto prior_top = scene::project_to_canvas(
          s.camera, s.canvas, {.125F - .25F / 60.0F, .5F, 0});
      const auto bottom =
          scene::project_to_canvas(s.camera, s.canvas, {0, -.5F, 0});
      for (std::size_t i = 0; i < velocity.size(); i += 16) {
        float v[4]{};
        std::memcpy(v, velocity.data() + i, 16);
        if (std::abs(v[0]) > 1e-6F) {
          const float y = static_cast<float>((i / 16) / 128) + .5F;
          const float fraction = (bottom.y - y) / (bottom.y - top.y);
          near(v[0], fraction * (top.x - prior_top.x) / 128.0F);
          near(v[1], 0);
          moving = true;
        }
      }
      CHECK(moving);
    }
  }
  s.show_frame = 0;
  CHECK(draw(full.value(), scene::whole(s.canvas)) == frame_zero);
  s.show_frame = 30;
  const auto sought = draw(full.value(), scene::whole(s.canvas));
  s.show_frame = std::numeric_limits<core::u64>::max();
  const auto huge = draw(full.value(), scene::whole(s.canvas));
  s.show_frame = std::numeric_limits<core::u64>::max() % 60;
  CHECK(draw(full.value(), scene::whole(s.canvas)) == huge);
  // Independent per-instance clocks and instancing must agree with separate
  // draws.
  s.models[0].world = scene::translation({-.65F, 0, 0});
  s.models[0].normal = scene::identity();
  s.models.push_back(s.models[0]);
  s.models[1].source_node = doc::NodeId{4};
  s.models[1].world = scene::translation({.65F, 0, 0});
  s.models[1].animation.start_frame = 20;
  s.show_frame = 30;
  s.renderer_settings.geometry_instancing = true;
  const auto instanced = draw(full.value(), scene::whole(s.canvas));
  s.renderer_settings.geometry_instancing = false;
  CHECK(draw(full.value(), scene::whole(s.canvas)) == instanced);
  s.models.resize(1);
  s.models[0] = instance;
  s.models[0].animation.enabled = false;
  s.models[0].animation.wind = {.25F, 0, 0};
  s.show_frame = 0;
  const auto still = draw(full.value(), scene::whole(s.canvas));
  s.show_frame = 30;
  const auto wind = draw(full.value(), scene::whole(s.canvas));
  CHECK(wind != still && wind != sought);
  CHECK(draw(full.value(), scene::whole(s.canvas)) == wind);
  exact(draw(left.value(), {0, 0, 63, 64}),
        draw(right.value(), {63, 0, 65, 64}), wind, 8);
  // Shadow replay uses the same deformed vertices, including wind.
  s.renderer_settings.shadow_cascades = 1;
  scene::DirectionalLight sun;
  sun.direction = {.2F, -.4F, -1};
  s.directional_lights.push_back(sun);
  static_cast<void>(draw(full.value(), scene::whole(s.canvas)));
  const auto shadow =
      download(device, acquire(device), full.value().cascade_target(0), 4);
  static_cast<void>(draw(left.value(), {0, 0, 63, 64}));
  CHECK(download(device, acquire(device), left.value().cascade_target(0), 4) ==
        shadow);
  s.show_frame = 0;
  static_cast<void>(draw(full.value(), scene::whole(s.canvas)));
  CHECK(download(device, acquire(device), full.value().cascade_target(0), 4) !=
        shadow);
  s.models[0].animation.enabled = true;
  s.show_frame = 30;
  static_cast<void>(draw(full.value(), scene::whole(s.canvas)));
  s.show_frame = 31;
  s.show_rate = {120, 1};
  static_cast<void>(draw(full.value(), scene::whole(s.canvas)));
  const auto cadence_reset = download(
      device, acquire(device), full.value().object_velocity_target(), 16);
  for (std::size_t i = 0; i < cadence_reset.size(); i += 16) {
    float motion[4]{};
    std::memcpy(motion, cadence_reset.data() + i, 16);
    CHECK(motion[0] == 0 && motion[1] == 0);
  }
  auto hybrid = render::HybridSceneRenderer::create(device, 128, 64, limits());
  CHECK(hybrid);
  s.renderer_settings.motion_blur = false;
  const auto request =
      hybrid.value().convenience_request(s, scene::whole(s.canvas), {});
  CHECK(request.history.motion_rate == s.show_rate);
  s.models[0].animation.clip = 63;
  s.models[0].animation.enabled = true;
  {
    auto c = acquire(device);
    CHECK(!full.value().render(c, s, scene::whole(s.canvas)));
  }
  std::cout << "[pass] 32 GPU frames, exact odd slices/color/velocity, "
               "Hold/seek, long clock, instancing, wind, invalid clip\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    const bool run_gpu = argc > 1 && std::string_view{argv[1]} == "--gpu";
    const auto directory =
        std::filesystem::temp_directory_path() /
        (run_gpu ? "mediaserver-animation-gpu" : "mediaserver-animation-cpu");
    if (!run_gpu) {
      cpu(directory);
      return 0;
    }
    render::ModelAsset asset;
    const auto source = fixture(directory);
    CHECK(render::load_model(source, asset));
    const auto package = render::write_model_package(source, asset);
    CHECK(!package.empty());
    bool ran = false;
    for (auto backend : {rhi::BackendKind::SdlGpu, rhi::BackendKind::D3d12}) {
      if (!rhi::Device::available(backend))
        continue;
      rhi::DeviceConfig config;
      config.backend = backend;
      config.debug = true;
      auto opened = rhi::Device::create(config);
      CHECK(opened);
      auto device = std::move(opened).value();
      std::cout << "backend=" << static_cast<int>(backend)
                << " advanced=" << device.capabilities().advanced_raster()
                << '\n';
      if (!device.capabilities().advanced_raster())
        continue;
      gpu(device, package);
      ran = true;
    }
    return ran ? 0 : 77;
  } catch (const std::exception &e) {
    std::cerr << "[fail] " << e.what() << '\n';
    return 1;
  }
}
