#include <mediaserver/render/compositor_scene_executor.hpp>

#include <mediaserver/compositor/compositor_model.hpp>
#include <mediaserver/compositor/compositor_schema.hpp>
#include <mediaserver/rhi/device.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace compositor = mediaserver::compositor;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;
namespace schema = mediaserver::compositor::schema;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define EXECUTOR_CHECK(...)                                                    \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

using Property = std::pair<std::string, doc::PropertyValue>;

[[nodiscard]] Property whole(std::string_view name, std::int64_t value) {
    return {std::string{name}, doc::PropertyValue{value}};
}

[[nodiscard]] Property real(std::string_view name, double value) {
    return {std::string{name}, doc::PropertyValue{value}};
}

[[nodiscard]] Property flag(std::string_view name, bool value) {
    return {std::string{name}, doc::PropertyValue{value}};
}

[[nodiscard]] doc::Node node(core::u64 id, std::string_view type,
                             std::initializer_list<Property> properties = {}) {
    doc::Node result{};
    result.id = doc::NodeId{id};
    result.type = std::string{type};
    for (const auto& property : properties) {
        result.properties.emplace(property.first, property.second);
    }
    return result;
}

[[nodiscard]] doc::Node layer(core::u64 id, core::u64 scene_node) {
    return node(
        id, schema::scene_layer_type,
        {whole(schema::scene_node, static_cast<std::int64_t>(scene_node))});
}

[[nodiscard]] doc::Node edge(core::u64 id, core::u64 from, core::u8 from_port,
                             core::u64 to, core::u8 to_port,
                             core::i64 order = 0) {
    return node(id, schema::edge_type,
                {whole(schema::from_node, static_cast<std::int64_t>(from)),
                 whole(schema::from_port, from_port),
                 whole(schema::to_node, static_cast<std::int64_t>(to)),
                 whole(schema::to_port, to_port), whole(schema::order, order)});
}

[[nodiscard]] compositor::CompositorGraph graph(bool blur_red) {
    std::vector<doc::Node> nodes{
        node(100U, "scene.object"),
        node(101U, "scene.object"),
        layer(10U, 100U),
        layer(11U, 101U),
        node(12U, schema::composite_type),
        node(13U, schema::output_type),
        edge(20U, 10U, 0U, 12U, 0U, 0),
        edge(21U, 11U, 0U, 12U, 0U, 1),
        edge(22U, 12U, 0U, 13U, 0U),
    };
    if (blur_red) {
        nodes.push_back(
            node(1U, schema::blur_type, {real(schema::radius, 6.0)}));
        nodes.push_back(edge(23U, 1U, 0U, 10U, 0U));
    }
    doc::Root root;
    for (const doc::Node& value : nodes) {
        root = root.set(value);
    }
    compositor::CompositorModel model;
    model.reset(root);
    return model.evaluate();
}

[[nodiscard]] compositor::CompositorGraph
single_layer_graph(bool replace_scene) {
    std::vector<doc::Node> nodes{
        node(100U, "scene.object"),
        node(101U, "scene.object"),
        layer(10U, 100U),
        node(13U, schema::output_type,
             {flag(schema::replace_scene, replace_scene)}),
        edge(20U, 10U, 0U, 13U, 0U),
    };
    doc::Root root;
    for (const doc::Node& value : nodes) {
        root = root.set(value);
    }
    compositor::CompositorModel model;
    model.reset(root);
    return model.evaluate();
}

enum class KeyGraphKind : core::u8 {
    None,
    Chroma,
    Luma,
};

[[nodiscard]] compositor::CompositorGraph key_graph(
    core::u64 scene_node, KeyGraphKind kind, float mix = 1.0F) {
    std::vector<doc::Node> nodes{
        node(scene_node, "scene.object"), layer(10U, scene_node),
        node(13U, schema::output_type,
             {flag(schema::replace_scene, true)}),
        edge(20U, 10U, 0U, 13U, 0U),
    };
    if (kind == KeyGraphKind::Chroma) {
        nodes.push_back(node(
            1U, schema::chroma_key_type,
            {real(schema::chroma_key_color_r, 0.0),
             real(schema::chroma_key_color_g, 1.0),
             real(schema::chroma_key_color_b, 0.0),
             real(schema::chroma_key_tolerance, 0.15),
             real(schema::chroma_key_softness, 0.05),
             whole(schema::chroma_key_morphology_px, 0),
             real(schema::chroma_key_spill_suppression, 0.5),
             real(schema::chroma_key_mix, mix)}));
        nodes.push_back(edge(21U, 1U, 0U, 10U, 0U));
    } else if (kind == KeyGraphKind::Luma) {
        nodes.push_back(node(
            1U, schema::luma_key_type,
            {real(schema::luma_key_low_threshold, 0.0),
             real(schema::luma_key_high_threshold, 0.5),
             real(schema::luma_key_low_softness, 0.0),
             real(schema::luma_key_high_softness, 0.0),
             flag(schema::luma_key_invert, false),
             real(schema::luma_key_mix, mix)}));
        nodes.push_back(edge(21U, 1U, 0U, 10U, 0U));
    }
    doc::Root root;
    for (const doc::Node& value : nodes) {
        root = root.set(value);
    }
    compositor::CompositorModel model;
    model.reset(root);
    return model.evaluate();
}

[[nodiscard]] compositor::CompositorGraph
ordered_composite_graph(bool red_on_top) {
    std::vector<doc::Node> nodes{
        node(100U, "scene.object"),
        node(101U, "scene.object"),
        layer(10U, 100U),
        layer(11U, 101U),
        node(12U, schema::composite_type),
        node(13U, schema::output_type),
        edge(20U, 10U, 0U, 12U, 0U, red_on_top ? 1 : 0),
        edge(21U, 11U, 0U, 12U, 0U, red_on_top ? 0 : 1),
        edge(22U, 12U, 0U, 13U, 0U),
    };
    doc::Root root;
    for (const doc::Node& value : nodes) {
        root = root.set(value);
    }
    compositor::CompositorModel model;
    model.reset(root);
    return model.evaluate();
}

[[nodiscard]] scene::Camera front_camera() noexcept {
    scene::Camera camera{};
    camera.eye = scene::Vec3{0.0F, 0.0F, 3.0F};
    camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    camera.field_of_view = 0.125F;
    camera.near_plane = 0.1F;
    camera.far_plane = 100.0F;
    return camera;
}

void add_quad_at(scene::Scene& value, core::u64 id, scene::Vec3 position,
                 scene::Vec3 emissive, float half_extent) {
    scene::MeshInstance instance{};
    instance.mesh = scene::MeshKey{scene::Primitive::Quad, 1U};
    instance.world = scene::multiply(
        scene::translation(position),
        scene::scaling(scene::Vec3{half_extent, half_extent, 1.0F}));
    instance.normal = scene::normal_basis(instance.world);
    instance.material.base_color = scene::Vec3{};
    instance.material.emissive = emissive;
    instance.source = doc::NodeId{id};
    value.instances.push_back(instance);
}

void add_quad(scene::Scene& value, core::u64 id, float x,
              scene::Vec3 emissive) {
    scene::MeshInstance instance{};
    instance.mesh = scene::MeshKey{scene::Primitive::Quad, 1U};
    instance.world =
        scene::multiply(scene::translation(scene::Vec3{x, 0.0F, 0.0F}),
                        scene::scaling(scene::Vec3{0.5F, 0.5F, 1.0F}));
    instance.normal = scene::normal_basis(instance.world);
    instance.material.base_color = scene::Vec3{};
    instance.material.emissive = emissive;
    instance.source = doc::NodeId{id};
    value.instances.push_back(instance);
}

[[nodiscard]] scene::Scene two_layer_scene(scene::Canvas canvas) {
    scene::Scene value{};
    value.canvas = canvas;
    value.camera = front_camera();
    value.environment.sky_color = scene::Vec3{};
    value.environment.ground_color = scene::Vec3{};
    value.environment.ambient = 0.0F;
    add_quad(value, 100U, -0.8F, scene::Vec3{0.25F, 0.0F, 0.0F});
    add_quad(value, 101U, 0.8F, scene::Vec3{0.0F, 0.0F, 0.25F});
    return value;
}

[[nodiscard]] std::string write_split_key_tga() {
    constexpr core::u32 width = 8U;
    constexpr core::u32 height = 8U;
    std::array<core::u8, 18U> header{};
    header[2U] = 2U;
    header[12U] = static_cast<core::u8>(width);
    header[14U] = static_cast<core::u8>(height);
    header[16U] = 32U;
    header[17U] = 0x28U;
    std::vector<core::u8> pixels;
    pixels.reserve(static_cast<std::size_t>(width) * height * 4U);
    for (core::u32 y = 0U; y < height; ++y) {
        static_cast<void>(y);
        for (core::u32 x = 0U; x < width; ++x) {
            const bool red = x < width / 2U;
            pixels.push_back(0U);
            pixels.push_back(red ? 0U : 255U);
            pixels.push_back(red ? 255U : 0U);
            pixels.push_back(255U);
        }
    }
    static core::u64 sequence{};
    const std::filesystem::path path =
        std::filesystem::temp_directory_path()
        / ("mediaserver_compositor_key_" + std::to_string(++sequence)
           + ".tga");
    std::ofstream output{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(header.data()),
                 static_cast<std::streamsize>(header.size()));
    output.write(reinterpret_cast<const char*>(pixels.data()),
                 static_cast<std::streamsize>(pixels.size()));
    output.close();
    check(static_cast<bool>(output), "write keyed media fixture", __LINE__);
    return path.string();
}

[[nodiscard]] scene::Scene media_scene(scene::Canvas canvas,
                                       const std::string& source) {
    scene::Scene value{};
    value.canvas = canvas;
    value.camera = front_camera();
    value.environment.sky_color = scene::Vec3{};
    value.environment.ground_color = scene::Vec3{};
    value.environment.ambient = 0.0F;
    scene::MediaInstance media{};
    media.mesh = scene::MeshKey{scene::Primitive::Quad, 1U};
    media.world = scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F});
    media.normal = scene::normal_basis(media.world);
    media.source = source;
    media.source_node = doc::NodeId{200U};
    value.media.push_back(std::move(media));
    return value;
}

[[nodiscard]] scene::MaskProgram rectangle_mask_program(
    core::u64 target, scene::MaskBounds bounds, float opacity = 1.0F,
    float feather_canvas_px = 0.0F,
    float expansion_canvas_px = 0.0F, bool invert = false) {
    scene::MaskInstance operation{};
    operation.source = doc::NodeId{900U};
    operation.scene = doc::NodeId{1U};
    operation.target = doc::NodeId{target};
    operation.kind = scene::MaskKind::Rectangle;
    operation.combine = scene::MaskCombine::Add;
    operation.bounds = bounds;
    operation.opacity = opacity;
    operation.feather_canvas_px = feather_canvas_px;
    operation.expansion_canvas_px = expansion_canvas_px;
    operation.invert = invert;
    return scene::MaskProgram{
        doc::NodeId{target}, scene::MaskCoordinateSpace::SourceLayer,
        {std::move(operation)}};
}

[[nodiscard]] render::RendererLimits test_limits() noexcept {
    render::RendererLimits limits{};
    limits.max_instances = 32U;
    limits.max_vertices = 1U << 12U;
    limits.max_indices = 1U << 13U;
    limits.max_lights = 8U;
    return limits;
}

[[nodiscard]] rhi::Device open_device() {
    rhi::DeviceConfig config{};
    config.debug = false;
    auto created = rhi::Device::create(config);
    check(created.has_value(), "device creation", __LINE__);
    return std::move(created).value();
}

struct Image final {
    core::u32 width{};
    core::u32 height{};
    std::vector<core::u8> rgba;
    render::CompositorSceneStats stats{};

    [[nodiscard]] std::size_t at(core::u32 x, core::u32 y) const noexcept {
        return (static_cast<std::size_t>(y) * width + x) * 4U;
    }
};

template <typename Record>
[[nodiscard]] Image capture(rhi::Device& device,
                            const scene::Scene& scene_value, Record&& record) {
    auto created = render::CompositorSceneExecutor::create(
        device, scene_value.canvas.width, scene_value.canvas.height,
        test_limits());
    check(created.has_value(), "executor creation", __LINE__);
    auto executor = std::move(created).value();
    const core::u32 bytes =
        scene_value.canvas.width * scene_value.canvas.height * 4U;
    auto readback = device.create_readback(bytes);
    check(readback.has_value(), "readback creation", __LINE__);
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "command acquisition", __LINE__);
    auto commands = std::move(acquired).value();
    auto result = record(executor, commands);
    check(result.has_value(), "executor render", __LINE__);
    const rhi::Texture* const output = result.value();
    check(output != nullptr, "executor output", __LINE__);
    check(commands.download(*output, readback.value()), "output download",
          __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "executor submit", __LINE__);
    check(std::move(submitted).value().wait(), "executor wait", __LINE__);

    Image image{};
    image.width = output->width();
    image.height = output->height();
    image.stats = executor.stats();
    const auto pixels = readback.value().map();
    check(pixels.size() >= bytes, "output map", __LINE__);
    image.rgba.assign(pixels.begin(), pixels.begin() + bytes);
    readback.value().unmap();
    return image;
}

template <typename Record>
[[nodiscard]] Image capture_sized(rhi::Device& device,
                                  core::u32 width, core::u32 height,
                                  Record&& record) {
    auto created = render::CompositorSceneExecutor::create(
        device, width, height, test_limits());
    check(created.has_value(), "sized executor creation", __LINE__);
    auto executor = std::move(created).value();
    const core::u32 bytes = width * height * 4U;
    auto readback = device.create_readback(bytes);
    check(readback.has_value(), "sized readback creation", __LINE__);
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "sized command acquisition", __LINE__);
    auto commands = std::move(acquired).value();
    auto result = record(executor, commands);
    check(result.has_value(), "sized executor render", __LINE__);
    const rhi::Texture* const output = result.value();
    check(output != nullptr && output->width() == width &&
              output->height() == height,
          "sized executor output", __LINE__);
    check(commands.download(*output, readback.value()),
          "sized output download", __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "sized executor submit", __LINE__);
    check(std::move(submitted).value().wait(), "sized executor wait",
          __LINE__);

    Image image{};
    image.width = width;
    image.height = height;
    image.stats = executor.stats();
    const auto pixels = readback.value().map();
    check(pixels.size() >= bytes, "sized output map", __LINE__);
    image.rgba.assign(pixels.begin(), pixels.begin() + bytes);
    readback.value().unmap();
    return image;
}

[[nodiscard]] Image
execute_graph(rhi::Device& device,
              const compositor::CompositorGraph& graph_value,
              const scene::Scene& scene_value) {
    return capture(
        device, scene_value,
        [&graph_value, &scene_value](render::CompositorSceneExecutor& executor,
                                     rhi::CommandList& commands) {
            return executor.execute(commands, graph_value, scene_value,
                                    scene::whole(scene_value.canvas));
        });
}

[[nodiscard]] Image execute_scene(rhi::Device& device,
                                  const scene::Scene& scene_value) {
    return capture(device, scene_value,
                   [&scene_value](render::CompositorSceneExecutor& executor,
                                  rhi::CommandList& commands) {
                       return executor.execute(
                           commands, scene_value,
                           scene::whole(scene_value.canvas));
                   });
}

[[nodiscard]] int difference(core::u8 left, core::u8 right) noexcept {
    const int a = static_cast<int>(left);
    const int b = static_cast<int>(right);
    return a > b ? a - b : b - a;
}

void check_local_blur(const Image& sharp, const Image& blurred,
                      core::u32 sharp_passes, core::u8 expected_corner_alpha) {
    EXECUTOR_CHECK(sharp.stats.layers == sharp_passes);
    EXECUTOR_CHECK(sharp.stats.blurred_layers == 0U);
    EXECUTOR_CHECK(blurred.stats.layers == 2U);
    EXECUTOR_CHECK(blurred.stats.blurred_layers == 1U);
    EXECUTOR_CHECK(blurred.stats.composite_passes == 2U);
    EXECUTOR_CHECK(sharp.rgba[sharp.at(0U, 0U) + 3U] == expected_corner_alpha);
    EXECUTOR_CHECK(blurred.rgba[blurred.at(0U, 0U) + 3U] ==
                   expected_corner_alpha);

    core::u32 red_min_x = std::numeric_limits<core::u32>::max();
    core::u32 red_max_x{};
    core::u32 blue_min_x = std::numeric_limits<core::u32>::max();
    core::u32 blue_max_x{};
    core::u32 red_pixels{};
    core::u32 blue_pixels{};
    for (core::u32 y = 0U; y < sharp.height; ++y) {
        for (core::u32 x = 0U; x < sharp.width; ++x) {
            const std::size_t index = sharp.at(x, y);
            if (sharp.rgba[index] > 20U && sharp.rgba[index + 2U] < 5U) {
                red_min_x = std::min(red_min_x, x);
                red_max_x = std::max(red_max_x, x);
                ++red_pixels;
            }
            if (sharp.rgba[index + 2U] > 20U && sharp.rgba[index] < 5U) {
                blue_min_x = std::min(blue_min_x, x);
                blue_max_x = std::max(blue_max_x, x);
                ++blue_pixels;
            }
        }
    }
    EXECUTOR_CHECK(red_pixels > 50U);
    EXECUTOR_CHECK(blue_pixels > 50U);
    EXECUTOR_CHECK(red_max_x + 8U < blue_min_x);

    core::u32 spread_pixels{};
    for (core::u32 y = 0U; y < sharp.height; ++y) {
        for (core::u32 x = 0U; x < sharp.width; ++x) {
            const std::size_t index = sharp.at(x, y);
            if ((x < red_min_x || x > red_max_x) && sharp.rgba[index] == 0U &&
                blurred.rgba[index] > 2U && blurred.rgba[index + 2U] < 5U) {
                ++spread_pixels;
            }
        }
    }
    EXECUTOR_CHECK(spread_pixels > 20U);

    // The blue layer is well outside the six-pixel red kernel. Its complete
    // sharp footprint must survive pixel-for-pixel apart from one code value
    // of fullscreen sampler rounding.
    for (core::u32 y = 0U; y < sharp.height; ++y) {
        for (core::u32 x = blue_min_x; x <= blue_max_x; ++x) {
            const std::size_t index = sharp.at(x, y);
            if (sharp.rgba[index + 2U] <= 20U) {
                continue;
            }
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                EXECUTOR_CHECK(difference(sharp.rgba[index + channel],
                                          blurred.rgba[index + channel]) <= 1);
            }
        }
    }
}

void blur_spreads_only_the_graph_referenced_layer() {
    auto device = open_device();
    constexpr scene::Canvas canvas{96U, 64U};
    const scene::Scene source = two_layer_scene(canvas);
    const auto sharp_graph = graph(false);
    const auto blurred_graph = graph(true);
    EXECUTOR_CHECK(sharp_graph.valid());
    EXECUTOR_CHECK(blurred_graph.valid());
    const Image sharp = execute_graph(device, sharp_graph, source);
    const Image blurred = execute_graph(device, blurred_graph, source);
    check_local_blur(sharp, blurred, 2U, 255U);
}

void first_class_scene_blur_is_layer_local() {
    auto device = open_device();
    constexpr scene::Canvas canvas{96U, 64U};
    scene::Scene sharp_scene = two_layer_scene(canvas);
    scene::Scene blurred_scene = sharp_scene;
    blurred_scene.instances.front().style.effects.blur_enabled = true;
    blurred_scene.instances.front().style.effects.blur_radius = 6.0F;

    const Image sharp = execute_scene(device, sharp_scene);
    const Image blurred = execute_scene(device, blurred_scene);
    check_local_blur(sharp, blurred, 1U, 255U);
}

struct ColorFootprint final {
  core::u32 min_x{std::numeric_limits<core::u32>::max()};
  core::u32 max_x{};
  core::u32 min_y{std::numeric_limits<core::u32>::max()};
  core::u32 max_y{};
  core::u32 pixels{};
};

[[nodiscard]] ColorFootprint red_footprint(const Image &image,
                                           core::u8 threshold) {
  ColorFootprint result{};
  for (core::u32 y = 0U; y < image.height; ++y) {
    for (core::u32 x = 0U; x < image.width; ++x) {
      const std::size_t index = image.at(x, y);
      if (image.rgba[index] <= threshold || image.rgba[index + 2U] >= 5U) {
        continue;
      }
      result.min_x = std::min(result.min_x, x);
      result.max_x = std::max(result.max_x, x);
      result.min_y = std::min(result.min_y, y);
      result.max_y = std::max(result.max_y, y);
      ++result.pixels;
    }
  }
  return result;
}

struct SpatialSpread final {
  core::u32 horizontal{};
  core::u32 vertical{};
};

[[nodiscard]] SpatialSpread spread_outside(const Image &sharp,
                                           const Image &effected,
                                           const ColorFootprint &footprint) {
  SpatialSpread result{};
  for (core::u32 y = 0U; y < sharp.height; ++y) {
    for (core::u32 x = 0U; x < sharp.width; ++x) {
      const std::size_t index = sharp.at(x, y);
      if (sharp.rgba[index] != 0U || effected.rgba[index] <= 2U ||
          effected.rgba[index + 2U] >= 5U) {
        continue;
      }
      const bool within_x = x >= footprint.min_x && x <= footprint.max_x;
      const bool within_y = y >= footprint.min_y && y <= footprint.max_y;
      if (!within_x && within_y) {
        ++result.horizontal;
      }
      if (within_x && !within_y) {
        ++result.vertical;
      }
    }
  }
  return result;
}

void box_and_directional_blur_execute_as_isolated_gpu_layers() {
  auto device = open_device();
  constexpr scene::Canvas canvas{96U, 64U};
  const scene::Scene sharp_scene = two_layer_scene(canvas);
  scene::Scene box_scene = sharp_scene;
  scene::SpatialEffectOperation box{};
  box.kind = scene::EffectKind::BoxBlur;
  box.box_blur = scene::BoxBlurParameters{4U, 1U};
  box_scene.instances.front().style.effects.spatial_operations.push_back(box);
  scene::Scene directional_scene = sharp_scene;
  scene::SpatialEffectOperation directional{};
  directional.kind = scene::EffectKind::DirectionalBlur;
  directional.directional_blur = scene::DirectionalBlurParameters{16.0F, 0.0F};
  directional_scene.instances.front()
      .style.effects.spatial_operations.push_back(directional);

  const Image sharp = execute_scene(device, sharp_scene);
  const Image boxed = execute_scene(device, box_scene);
  const Image directed = execute_scene(device, directional_scene);
  EXECUTOR_CHECK(sharp.stats.layers == 1U);
  EXECUTOR_CHECK(boxed.stats.layers == 2U);
  EXECUTOR_CHECK(boxed.stats.composite_passes == 2U);
  EXECUTOR_CHECK(boxed.stats.blurred_layers == 0U);
  EXECUTOR_CHECK(boxed.stats.box_blurred_layers == 1U);
  EXECUTOR_CHECK(boxed.stats.directional_blurred_layers == 0U);
  EXECUTOR_CHECK(directed.stats.layers == 2U);
  EXECUTOR_CHECK(directed.stats.composite_passes == 2U);
  EXECUTOR_CHECK(directed.stats.blurred_layers == 0U);
  EXECUTOR_CHECK(directed.stats.box_blurred_layers == 0U);
  EXECUTOR_CHECK(directed.stats.directional_blurred_layers == 1U);

  const ColorFootprint footprint = red_footprint(sharp, 20U);
  EXECUTOR_CHECK(footprint.pixels > 50U);
  const SpatialSpread box_spread = spread_outside(sharp, boxed, footprint);
  const SpatialSpread directional_spread =
      spread_outside(sharp, directed, footprint);
  // A separable box expands the isolated red layer on both axes. A
  // zero-turn directional kernel is a one-line horizontal gather, so it
  // expands only along X. The untouched blue layer remains in the shared
  // base pass in both renders.
  EXECUTOR_CHECK(box_spread.horizontal > 20U);
  EXECUTOR_CHECK(box_spread.vertical > 20U);
  EXECUTOR_CHECK(directional_spread.horizontal > 20U);
  EXECUTOR_CHECK(directional_spread.vertical == 0U);
}

void identity_effects_are_pixel_equivalent_to_no_effect() {
  auto device = open_device();
  constexpr scene::Canvas canvas{96U, 64U};
  scene::Scene plain = two_layer_scene(canvas);
  scene::Scene identity = plain;
  identity.instances.front().style.effects.brightness_enabled = true;
  identity.instances.front().style.effects.brightness = 1.0F;
  identity.instances.front().style.effects.saturation_enabled = true;
  identity.instances.front().style.effects.saturation = 1.0F;
  identity.instances.front().style.effects.blur_enabled = true;
  identity.instances.front().style.effects.blur_radius = 0.0F;
  scene::SpatialEffectOperation box_identity{};
  box_identity.kind = scene::EffectKind::BoxBlur;
  box_identity.box_blur = scene::BoxBlurParameters{0U, 1U};
  identity.instances.front().style.effects.spatial_operations.push_back(
      box_identity);
  scene::SpatialEffectOperation directional_identity{};
  directional_identity.kind = scene::EffectKind::DirectionalBlur;
  directional_identity.directional_blur =
      scene::DirectionalBlurParameters{0.0F, 0.25F};
  identity.instances.front().style.effects.spatial_operations.push_back(
      directional_identity);
  scene::SpatialEffectOperation chroma_identity{};
  chroma_identity.kind = scene::EffectKind::ChromaKey;
  chroma_identity.chroma_key.mix = 0.0F;
  // Mix zero canonicalizes before irrelevant controls are inspected.
  chroma_identity.chroma_key.tolerance =
      std::numeric_limits<float>::quiet_NaN();
  chroma_identity.chroma_key.morphology_px =
      scene::max_key_morphology_radius_px + 1;
  identity.instances.front().style.effects.spatial_operations.push_back(
      chroma_identity);
  scene::SpatialEffectOperation luma_identity{};
  luma_identity.kind = scene::EffectKind::LumaKey;
  luma_identity.luma_key.mix = 0.0F;
  luma_identity.luma_key.low_threshold = 0.9F;
  luma_identity.luma_key.high_threshold = 0.1F;
  identity.instances.front().style.effects.spatial_operations.push_back(
      luma_identity);
  scene::MaskProgram mask_identity{};
  mask_identity.target = doc::NodeId{999U};
  mask_identity.coordinate_space =
      static_cast<scene::MaskCoordinateSpace>(255U);
  identity.mask_programs.push_back(mask_identity);

  const Image plain_image = execute_scene(device, plain);
  const Image identity_image = execute_scene(device, identity);
  EXECUTOR_CHECK(identity_image.stats == plain_image.stats);
  EXECUTOR_CHECK(identity_image.rgba == plain_image.rgba);

  for (const float invalid : {-0.1F, scene::max_saturation + 0.1F,
                              std::numeric_limits<float>::infinity()}) {
    identity.instances.front().style.effects.saturation = invalid;
    const Image invalid_image = execute_scene(device, identity);
    EXECUTOR_CHECK(invalid_image.stats == plain_image.stats);
    EXECUTOR_CHECK(invalid_image.rgba == plain_image.rgba);
  }
}

void saturation_changes_only_its_layer_without_adding_a_pass() {
  auto device = open_device();
  constexpr scene::Canvas canvas{96U, 64U};
  const scene::Scene plain = two_layer_scene(canvas);
  for (const scene::BrightnessCurve response :
       {scene::BrightnessCurve::Exposure, scene::BrightnessCurve::Gain}) {
    scene::Scene effected = plain;
    effected.effect_settings.brightness_curve = response;
    effected.instances.front().style.effects.saturation_enabled = true;
    effected.instances.front().style.effects.saturation = 0.0F;
    scene::Scene reference = plain;
    reference.effect_settings.brightness_curve = response;

    const Image plain_image = execute_scene(device, reference);
    const Image saturation_image = execute_scene(device, effected);
    EXECUTOR_CHECK(saturation_image.stats == plain_image.stats);
    EXECUTOR_CHECK(saturation_image.stats.layers == 1U);
    EXECUTOR_CHECK(saturation_image.stats.blurred_layers == 0U);

    core::u32 changed_left{};
    for (core::u32 y = 0U; y < canvas.height; ++y) {
      for (core::u32 x = 0U; x < canvas.width; ++x) {
        const std::size_t index = plain_image.at(x, y);
        const bool changed = !std::equal(plain_image.rgba.begin() + index,
                                         plain_image.rgba.begin() + index + 4U,
                                         saturation_image.rgba.begin() + index);
        if (x < canvas.width / 2U) {
          changed_left += changed ? 1U : 0U;
        } else {
          // The unselected blue layer is outside the effected
          // footprint and remains byte-for-byte identical.
          EXECUTOR_CHECK(!changed);
        }
      }
    }
    EXECUTOR_CHECK(changed_left > 50U);
  }
}

void a_graph_layer_preserves_unconnected_scene_pixels_by_default() {
    auto device = open_device();
    constexpr scene::Canvas canvas{96U, 64U};
    const scene::Scene source = two_layer_scene(canvas);
    const auto preserving = single_layer_graph(false);
    const auto replacing = single_layer_graph(true);
    EXECUTOR_CHECK(preserving.valid());
    EXECUTOR_CHECK(replacing.valid());

    const Image preserved = execute_graph(device, preserving, source);
    const Image replaced = execute_graph(device, replacing, source);
    EXECUTOR_CHECK(preserved.stats.layers == 2U);
    EXECUTOR_CHECK(replaced.stats.layers == 1U);
    EXECUTOR_CHECK(preserved.rgba[preserved.at(0U, 0U) + 3U] == 255U);
    EXECUTOR_CHECK(replaced.rgba[replaced.at(0U, 0U) + 3U] == 0U);

    core::u32 preserved_blue{};
    core::u32 replaced_blue{};
    for (core::u32 y = 0U; y < preserved.height; ++y) {
        for (core::u32 x = 0U; x < preserved.width; ++x) {
            const std::size_t index = preserved.at(x, y);
            if (preserved.rgba[index + 2U] > 20U &&
                preserved.rgba[index] < 5U) {
                ++preserved_blue;
            }
            if (replaced.rgba[index + 2U] > 20U && replaced.rgba[index] < 5U) {
                ++replaced_blue;
            }
        }
    }
    EXECUTOR_CHECK(preserved_blue > 50U);
    EXECUTOR_CHECK(replaced_blue == 0U);
}

void composite_input_order_changes_the_visible_top_layer() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    scene::Scene source{};
    source.canvas = canvas;
    source.camera = front_camera();
    source.environment.sky_color = scene::Vec3{};
    source.environment.ground_color = scene::Vec3{};
    source.environment.ambient = 0.0F;
    add_quad(source, 100U, 0.0F, scene::Vec3{0.25F, 0.0F, 0.0F});
    add_quad(source, 101U, 0.0F, scene::Vec3{0.0F, 0.0F, 0.25F});

    const auto red_graph = ordered_composite_graph(true);
    const auto blue_graph = ordered_composite_graph(false);
    EXECUTOR_CHECK(red_graph.valid());
    EXECUTOR_CHECK(blue_graph.valid());
    const Image red_top = execute_graph(device, red_graph, source);
    const Image blue_top = execute_graph(device, blue_graph, source);
    const std::size_t center =
        red_top.at(canvas.width / 2U, canvas.height / 2U);
    EXECUTOR_CHECK(red_top.stats.layers == 2U);
    EXECUTOR_CHECK(blue_top.stats.layers == 2U);
    EXECUTOR_CHECK(red_top.rgba[center] > 20U);
    EXECUTOR_CHECK(red_top.rgba[center + 2U] < 5U);
    EXECUTOR_CHECK(blue_top.rgba[center + 2U] > 20U);
    EXECUTOR_CHECK(blue_top.rgba[center] < 5U);
}

// Two quads on the same axis, the front one nearer the camera than the back
// one. `blurred_behind` puts the blur on the far quad, which is the reported
// defect: before depth priming the isolated layer composited over the frame
// and the near quad vanished.
[[nodiscard]] scene::Scene occlusion_scene(scene::Canvas canvas,
                                           bool blur_far) {
    scene::Scene value{};
    value.canvas = canvas;
    value.camera = front_camera();
    value.environment.sky_color = scene::Vec3{};
    value.environment.ground_color = scene::Vec3{};
    value.environment.ambient = 0.0F;
    // Far, red, large. Near, blue, small, centred over it. The camera sits at
    // z = 3 with a 45 degree vertical field and the unit quad spans half a
    // world unit, so on a 64 pixel canvas the red quad covers x in roughly
    // [18, 47] and the blue one only [27, 37].
    add_quad_at(value, 100U, scene::Vec3{0.0F, 0.0F, -1.0F},
                scene::Vec3{0.6F, 0.0F, 0.0F}, 1.5F);
    add_quad_at(value, 101U, scene::Vec3{0.0F, 0.0F, 1.0F},
                scene::Vec3{0.0F, 0.0F, 0.6F}, 0.25F);
    auto& blurred = blur_far ? value.instances[0U] : value.instances[1U];
    blurred.style.effects.blur_enabled = true;
    blurred.style.effects.blur_radius = 4.0F;
    return value;
}

void an_effect_layer_stays_behind_the_geometry_in_front_of_it() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    const Image image = execute_scene(device, occlusion_scene(canvas, true));
    EXECUTOR_CHECK(image.stats.layers == 2U);
    EXECUTOR_CHECK(image.stats.blurred_layers == 1U);

    // The near blue quad still owns the centre; the blurred red quad behind it
    // is only visible around the edges.
    const std::size_t centre = image.at(canvas.width / 2U, canvas.height / 2U);
    EXECUTOR_CHECK(image.rgba[centre + 2U] > 20U);
    EXECUTOR_CHECK(image.rgba[centre] < 20U);
    const std::size_t outside = image.at(20U, canvas.height / 2U);
    EXECUTOR_CHECK(image.rgba[outside] > 20U);
    EXECUTOR_CHECK(image.rgba[outside + 2U] < 20U);
}

void an_effect_layer_still_covers_the_geometry_behind_it() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    const Image image = execute_scene(device, occlusion_scene(canvas, false));
    EXECUTOR_CHECK(image.stats.layers == 2U);
    EXECUTOR_CHECK(image.stats.blurred_layers == 1U);

    // Depth priming must not overcorrect: the blurred quad is in front here,
    // so it is the one the centre shows.
    const std::size_t centre = image.at(canvas.width / 2U, canvas.height / 2U);
    EXECUTOR_CHECK(image.rgba[centre + 2U] > 20U);
    const std::size_t outside = image.at(20U, canvas.height / 2U);
    EXECUTOR_CHECK(image.rgba[outside] > 20U);
    EXECUTOR_CHECK(image.rgba[outside + 2U] < 20U);
}

void a_front_effect_layer_respects_depth_when_node_ids_disagree() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    scene::Scene source = occlusion_scene(canvas, false);
    // The front blurred quad now has the lower node id. The old pass builder
    // used that id as layer order, composited it first, and then painted the
    // physically farther red quad over it—the media-plane failure captured by
    // the operator recording.
    source.instances[0U].source = doc::NodeId{101U};
    source.instances[1U].source = doc::NodeId{100U};

    const Image image = execute_scene(device, source);
    EXECUTOR_CHECK(image.stats.layers == 2U);
    EXECUTOR_CHECK(image.stats.blurred_layers == 1U);
    const std::size_t centre = image.at(canvas.width / 2U,
                                        canvas.height / 2U);
    EXECUTOR_CHECK(image.rgba[centre + 2U] > 20U);
    EXECUTOR_CHECK(image.rgba[centre] < 20U);
}

void chroma_key_executes_on_lit_and_media_layers() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};

    scene::Scene lit{};
    lit.canvas = canvas;
    lit.camera = front_camera();
    lit.environment.sky_color = scene::Vec3{1.0F, 1.0F, 1.0F};
    lit.environment.ground_color = scene::Vec3{1.0F, 1.0F, 1.0F};
    lit.environment.ambient = 1.0F;
    scene::MeshInstance surface{};
    surface.mesh = scene::MeshKey{scene::Primitive::Quad, 1U};
    surface.world = scene::scaling(scene::Vec3{3.0F, 3.0F, 1.0F});
    surface.normal = scene::normal_basis(surface.world);
    surface.material.base_color = scene::Vec3{0.0F, 1.0F, 0.0F};
    surface.material.emissive = scene::Vec3{};
    surface.source = doc::NodeId{100U};
    lit.instances.push_back(surface);

    const auto plain_lit_graph = key_graph(100U, KeyGraphKind::None);
    const auto keyed_lit_graph = key_graph(100U, KeyGraphKind::Chroma);
    EXECUTOR_CHECK(plain_lit_graph.valid());
    EXECUTOR_CHECK(keyed_lit_graph.valid());
    const Image plain_lit = execute_graph(device, plain_lit_graph, lit);
    const Image keyed_lit = execute_graph(device, keyed_lit_graph, lit);
    const std::size_t centre =
        plain_lit.at(canvas.width / 2U, canvas.height / 2U);
    EXECUTOR_CHECK(plain_lit.rgba[centre + 1U] > 100U);
    EXECUTOR_CHECK(plain_lit.rgba[centre + 3U] > 240U);
    EXECUTOR_CHECK(keyed_lit.rgba[centre + 3U] < 10U);
    EXECUTOR_CHECK(keyed_lit.rgba[centre] < 10U);
    EXECUTOR_CHECK(keyed_lit.rgba[centre + 1U] < 10U);
    EXECUTOR_CHECK(keyed_lit.stats.chroma_keyed_layers == 1U);
    EXECUTOR_CHECK(keyed_lit.stats.keyer_passes == 2U);
    EXECUTOR_CHECK(keyed_lit.stats.keyer_pixels ==
                   2ULL * canvas.width * canvas.height);

    const std::string source = write_split_key_tga();
    const scene::Scene media = media_scene(canvas, source);
    const auto plain_media_graph = key_graph(200U, KeyGraphKind::None);
    const auto keyed_media_graph = key_graph(200U, KeyGraphKind::Chroma);
    EXECUTOR_CHECK(plain_media_graph.valid());
    EXECUTOR_CHECK(keyed_media_graph.valid());
    const Image plain_media =
        execute_graph(device, plain_media_graph, media);
    const Image keyed_media =
        execute_graph(device, keyed_media_graph, media);
    const std::size_t red = plain_media.at(
        canvas.width / 4U, canvas.height / 2U);
    const std::size_t green = plain_media.at(
        (canvas.width * 3U) / 4U, canvas.height / 2U);
    EXECUTOR_CHECK(plain_media.rgba[red] > 200U);
    EXECUTOR_CHECK(plain_media.rgba[green + 1U] > 200U);
    EXECUTOR_CHECK(keyed_media.rgba[red] > 200U);
    EXECUTOR_CHECK(keyed_media.rgba[red + 3U] > 240U);
    EXECUTOR_CHECK(keyed_media.rgba[green + 3U] < 10U);
    EXECUTOR_CHECK(keyed_media.stats.chroma_keyed_layers == 1U);
    EXECUTOR_CHECK(keyed_media.stats.keyer_passes == 2U);
    std::error_code ignored;
    std::filesystem::remove(source, ignored);
}

[[nodiscard]] scene::Scene white_layer_scene(scene::Canvas canvas,
                                             float opacity) {
    scene::Scene value{};
    value.canvas = canvas;
    value.camera = front_camera();
    value.environment.sky_color = scene::Vec3{};
    value.environment.ground_color = scene::Vec3{};
    value.environment.ambient = 0.0F;
    add_quad_at(value, 100U, scene::Vec3{},
                scene::Vec3{1.0F, 1.0F, 1.0F}, 3.0F);
    value.instances.front().style.opacity = opacity;
    return value;
}

void drawable_mask_gpu_gates_opaque_partial_and_transparent_source() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    const auto graph_value = key_graph(100U, KeyGraphKind::None);
    EXECUTOR_CHECK(graph_value.valid());

    scene::Scene opaque = white_layer_scene(canvas, 1.0F);
    opaque.mask_programs.push_back(rectangle_mask_program(
        100U, scene::MaskBounds{0.0F, 0.0F, 0.5F, 1.0F}, 0.5F));
    const Image opaque_masked = execute_graph(device, graph_value, opaque);
    const std::size_t opaque_left =
        opaque_masked.at(canvas.width / 4U, canvas.height / 2U);
    const std::size_t opaque_right =
        opaque_masked.at((canvas.width * 3U) / 4U, canvas.height / 2U);
    EXECUTOR_CHECK(opaque_masked.rgba[opaque_left + 3U] >= 120U &&
                   opaque_masked.rgba[opaque_left + 3U] <= 136U);
    EXECUTOR_CHECK(opaque_masked.rgba[opaque_right + 3U] < 5U);
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        EXECUTOR_CHECK(opaque_masked.rgba[opaque_left + channel] <=
                       opaque_masked.rgba[opaque_left + 3U] + 2U);
        EXECUTOR_CHECK(opaque_masked.rgba[opaque_right + channel] < 5U);
    }
    EXECUTOR_CHECK(opaque_masked.stats.masked_layers == 1U);
    EXECUTOR_CHECK(opaque_masked.stats.mask_passes == 2U);
    EXECUTOR_CHECK(opaque_masked.stats.mask_pixels ==
                   2ULL * canvas.width * canvas.height);

    scene::Scene translucent = white_layer_scene(canvas, 0.5F);
    translucent.mask_programs = opaque.mask_programs;
    const Image translucent_masked =
        execute_graph(device, graph_value, translucent);
    const std::size_t translucent_left =
        translucent_masked.at(canvas.width / 4U, canvas.height / 2U);
    EXECUTOR_CHECK(translucent_masked.rgba[translucent_left + 3U] >= 56U &&
                   translucent_masked.rgba[translucent_left + 3U] <= 72U);
    EXECUTOR_CHECK(translucent_masked.rgba[translucent_left + 3U] <
                   opaque_masked.rgba[opaque_left + 3U]);

    // Expansion fills the coverage field but cannot invent colour or alpha in
    // the isolated layer's transparent padding.
    scene::Scene padded{};
    padded.canvas = canvas;
    padded.camera = front_camera();
    padded.environment.sky_color = scene::Vec3{};
    padded.environment.ground_color = scene::Vec3{};
    padded.environment.ambient = 0.0F;
    add_quad_at(padded, 100U, scene::Vec3{},
                scene::Vec3{1.0F, 1.0F, 1.0F}, 0.5F);
    padded.mask_programs.push_back(rectangle_mask_program(
        100U, scene::MaskBounds{0.45F, 0.45F, 0.55F, 0.55F},
        1.0F, 8.0F, 200.0F));
    const Image padded_masked = execute_graph(device, graph_value, padded);
    const std::size_t corner = padded_masked.at(0U, 0U);
    EXECUTOR_CHECK(padded_masked.rgba[corner] == 0U);
    EXECUTOR_CHECK(padded_masked.rgba[corner + 1U] == 0U);
    EXECUTOR_CHECK(padded_masked.rgba[corner + 2U] == 0U);
    EXECUTOR_CHECK(padded_masked.rgba[corner + 3U] == 0U);
}

void a_masked_layer_retains_its_intersecting_tested_depth() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    scene::Scene value = occlusion_scene(canvas, true);
    value.mask_programs.push_back(rectangle_mask_program(
        100U, scene::MaskBounds{0.0F, 0.0F, 1.0F, 1.0F}));

    const Image image = execute_scene(device, value);
    EXECUTOR_CHECK(image.stats.layers == 2U);
    EXECUTOR_CHECK(image.stats.masked_layers == 1U);
    EXECUTOR_CHECK(image.stats.mask_passes == 2U);
    const std::size_t centre =
        image.at(canvas.width / 2U, canvas.height / 2U);
    EXECUTOR_CHECK(image.rgba[centre + 2U] > 20U);
    EXECUTOR_CHECK(image.rgba[centre] < 20U);
    const std::size_t outside = image.at(20U, canvas.height / 2U);
    EXECUTOR_CHECK(image.rgba[outside] > 20U);
    EXECUTOR_CHECK(image.rgba[outside + 2U] < 20U);
}

void luma_key_preserves_premultiplication_for_opaque_and_translucent_layers() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    const auto plain_graph = key_graph(100U, KeyGraphKind::None);
    const auto keyed_graph = key_graph(100U, KeyGraphKind::Luma, 0.5F);
    EXECUTOR_CHECK(plain_graph.valid());
    EXECUTOR_CHECK(keyed_graph.valid());

    const Image plain_opaque =
        execute_graph(device, plain_graph, white_layer_scene(canvas, 1.0F));
    const Image keyed_opaque =
        execute_graph(device, keyed_graph, white_layer_scene(canvas, 1.0F));
    const Image plain_translucent =
        execute_graph(device, plain_graph, white_layer_scene(canvas, 0.5F));
    const Image keyed_translucent =
        execute_graph(device, keyed_graph,
                      white_layer_scene(canvas, 0.5F));
    const std::size_t centre =
        keyed_opaque.at(canvas.width / 2U, canvas.height / 2U);
    const core::u8 plain_opaque_alpha = plain_opaque.rgba[centre + 3U];
    const core::u8 keyed_opaque_alpha = keyed_opaque.rgba[centre + 3U];
    const core::u8 plain_translucent_alpha =
        plain_translucent.rgba[centre + 3U];
    const core::u8 keyed_translucent_alpha =
        keyed_translucent.rgba[centre + 3U];
    EXECUTOR_CHECK(plain_opaque_alpha > 245U);
    EXECUTOR_CHECK(keyed_opaque_alpha >= 120U &&
                   keyed_opaque_alpha <= 136U);
    EXECUTOR_CHECK(plain_translucent_alpha >= 120U &&
                   plain_translucent_alpha <= 136U);
    EXECUTOR_CHECK(keyed_translucent_alpha >= 56U &&
                   keyed_translucent_alpha <= 72U);
    EXECUTOR_CHECK(keyed_translucent_alpha < keyed_opaque_alpha);
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const core::u8 opaque = keyed_opaque.rgba[centre + channel];
        const core::u8 translucent =
            keyed_translucent.rgba[centre + channel];
        // Tone mapping means lit white is not numerically one, but a valid
        // premultiplied result can never carry a colour component above its
        // coverage and halving source coverage must halve every component.
        EXECUTOR_CHECK(static_cast<int>(opaque) <=
                       static_cast<int>(keyed_opaque_alpha) + 2);
        EXECUTOR_CHECK(static_cast<int>(translucent) <=
                       static_cast<int>(keyed_translucent_alpha) + 2);
        EXECUTOR_CHECK(difference(
                           static_cast<core::u8>(opaque / 2U),
                           translucent) <= 2);
    }
    EXECUTOR_CHECK(keyed_opaque.stats.luma_keyed_layers == 1U);
    EXECUTOR_CHECK(keyed_translucent.stats.luma_keyed_layers == 1U);
    EXECUTOR_CHECK(keyed_opaque.stats.keyer_passes == 2U);
    EXECUTOR_CHECK(keyed_translucent.stats.keyer_passes == 2U);
}

void a_keyed_layer_retains_its_intersecting_tested_depth() {
    auto device = open_device();
    constexpr scene::Canvas canvas{64U, 64U};
    scene::Scene value{};
    value.canvas = canvas;
    value.camera = front_camera();
    value.environment.sky_color = scene::Vec3{};
    value.environment.ground_color = scene::Vec3{};
    value.environment.ambient = 0.0F;
    add_quad_at(value, 100U, scene::Vec3{0.0F, 0.0F, -1.0F},
                scene::Vec3{0.6F, 0.0F, 0.0F}, 1.5F);
    add_quad_at(value, 101U, scene::Vec3{0.0F, 0.0F, 1.0F},
                scene::Vec3{0.0F, 0.0F, 0.6F}, 0.25F);
    scene::SpatialEffectOperation key{};
    key.kind = scene::EffectKind::ChromaKey;
    key.chroma_key = scene::ChromaKeyParameters{
        {0.0F, 1.0F, 0.0F}, 0.01F, 0.0F, 0, 0.0F, 1.0F};
    value.instances.front().style.effects.spatial_operations.push_back(key);

    const Image image = execute_scene(device, value);
    EXECUTOR_CHECK(image.stats.layers == 2U);
    EXECUTOR_CHECK(image.stats.chroma_keyed_layers == 1U);
    EXECUTOR_CHECK(image.stats.keyer_passes == 2U);
    const std::size_t centre =
        image.at(canvas.width / 2U, canvas.height / 2U);
    EXECUTOR_CHECK(image.rgba[centre + 2U] > 20U);
    EXECUTOR_CHECK(image.rgba[centre] < 20U);
    const std::size_t outside = image.at(20U, canvas.height / 2U);
    EXECUTOR_CHECK(image.rgba[outside] > 20U);
    EXECUTOR_CHECK(image.rgba[outside + 2U] < 20U);
}

void keyed_media_is_consistent_across_slices_and_explicit_output_views() {
    auto device = open_device();
    constexpr scene::Canvas canvas{96U, 64U};
    constexpr core::u32 half = canvas.width / 2U;
    const std::string source = write_split_key_tga();
    const scene::Scene media = media_scene(canvas, source);
    const auto graph_value = key_graph(200U, KeyGraphKind::Chroma);
    EXECUTOR_CHECK(graph_value.valid());
    const Image whole = execute_graph(device, graph_value, media);
    const Image left = capture_sized(
        device, half, canvas.height,
        [&graph_value, &media](render::CompositorSceneExecutor& executor,
                               rhi::CommandList& commands) {
            return executor.execute(
                commands, graph_value, media,
                scene::CanvasRect{0U, 0U, half, canvas.height});
        });
    const Image right = capture_sized(
        device, half, canvas.height,
        [&graph_value, &media](render::CompositorSceneExecutor& executor,
                               rhi::CommandList& commands) {
            return executor.execute(
                commands, graph_value, media,
                scene::CanvasRect{half, 0U, half, canvas.height});
        });
    EXECUTOR_CHECK(left.stats.chroma_keyed_layers == 1U);
    EXECUTOR_CHECK(right.stats.chroma_keyed_layers == 1U);
    long total_difference{};
    int worst{};
    for (core::u32 y = 0U; y < canvas.height; ++y) {
        for (core::u32 x = 0U; x < canvas.width; ++x) {
            const Image& part = x < half ? left : right;
            const core::u32 part_x = x < half ? x : x - half;
            const std::size_t whole_index = whole.at(x, y);
            const std::size_t part_index = part.at(part_x, y);
            for (std::size_t channel = 0U; channel < 4U; ++channel) {
                const int delta = difference(
                    whole.rgba[whole_index + channel],
                    part.rgba[part_index + channel]);
                total_difference += delta;
                worst = std::max(worst, delta);
            }
        }
    }
    EXECUTOR_CHECK(worst <= 8);
    EXECUTOR_CHECK(total_difference <=
                   static_cast<long>(canvas.width * canvas.height * 4U));

    const scene::CanvasRect full = scene::whole(canvas);
    const render::SceneRenderView explicit_view{
        scene::view_for_slice(media.camera, canvas, full), media.camera.eye,
        scene::normalize(media.camera.target - media.camera.eye)};
    const Image mapped = capture_sized(
        device, canvas.width, canvas.height,
        [&graph_value, &media, &explicit_view](
            render::CompositorSceneExecutor& executor,
            rhi::CommandList& commands) {
            return executor.execute(commands, graph_value, media,
                                    explicit_view);
        });
    EXECUTOR_CHECK(mapped.stats.chroma_keyed_layers == 1U);
    EXECUTOR_CHECK(mapped.rgba == whole.rgba);
    std::error_code ignored;
    std::filesystem::remove(source, ignored);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 16U> cases{{
    {"blur spreads only the graph referenced layer",
     &blur_spreads_only_the_graph_referenced_layer},
    {"first class scene blur is layer local",
     &first_class_scene_blur_is_layer_local},
    {"Box and Directional Blur execute as isolated GPU layers",
     &box_and_directional_blur_execute_as_isolated_gpu_layers},
    {"identity effects are pixel equivalent to no effect",
     &identity_effects_are_pixel_equivalent_to_no_effect},
    {"saturation changes only its layer without adding a pass",
     &saturation_changes_only_its_layer_without_adding_a_pass},
    {"a graph layer preserves unconnected scene pixels by default",
     &a_graph_layer_preserves_unconnected_scene_pixels_by_default},
    {"Composite input order changes the visible top layer",
     &composite_input_order_changes_the_visible_top_layer},
    {"an effect layer stays behind the geometry in front of it",
     &an_effect_layer_stays_behind_the_geometry_in_front_of_it},
    {"an effect layer still covers the geometry behind it",
     &an_effect_layer_still_covers_the_geometry_behind_it},
    {"a front effect layer respects depth when node ids disagree",
     &a_front_effect_layer_respects_depth_when_node_ids_disagree},
    {"Chroma Key executes on lit and media layers",
     &chroma_key_executes_on_lit_and_media_layers},
    {"Luma Key preserves premultiplication for opaque and translucent layers",
     &luma_key_preserves_premultiplication_for_opaque_and_translucent_layers},
    {"a keyed layer retains its intersecting tested depth",
     &a_keyed_layer_retains_its_intersecting_tested_depth},
    {"drawable mask GPU gates opaque, partial, and transparent source",
     &drawable_mask_gpu_gates_opaque_partial_and_transparent_source},
    {"a masked layer retains its intersecting tested depth",
     &a_masked_layer_retains_its_intersecting_tested_depth},
    {"keyed media is consistent across slices and explicit output views",
     &keyed_media_is_consistent_across_slices_and_explicit_output_views},
}};

} // namespace

int main() {
    if (!rhi::Device::available() ||
        !render::CompositorSceneExecutor::shaders_available()) {
        std::cout << "[skip] compositor scene executor unavailable\n";
        return 0;
    }
    {
        auto device = open_device();
        const auto probe = render::CompositorSceneExecutor::create(
            device, 4U, 4U, test_limits());
        if (!probe &&
            probe.error() == render::RenderError::ShaderFormatUnsupported) {
            std::cout << "[skip] compositor shader format unsupported\n";
            return 0;
        }
    }
    std::size_t failures = 0U;
    for (const Case& test : cases) {
        try {
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << cases.size() - failures << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
