#include <mediaserver/render/compositor_scene_executor.hpp>

#include <mediaserver/compositor/compositor_model.hpp>
#include <mediaserver/compositor/compositor_schema.hpp>
#include <mediaserver/rhi/device.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
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

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 6U> cases{{
    {"blur spreads only the graph referenced layer",
     &blur_spreads_only_the_graph_referenced_layer},
    {"first class scene blur is layer local",
     &first_class_scene_blur_is_layer_local},
    {"a graph layer preserves unconnected scene pixels by default",
     &a_graph_layer_preserves_unconnected_scene_pixels_by_default},
    {"Composite input order changes the visible top layer",
     &composite_input_order_changes_the_visible_top_layer},
    {"an effect layer stays behind the geometry in front of it",
     &an_effect_layer_stays_behind_the_geometry_in_front_of_it},
    {"an effect layer still covers the geometry behind it",
     &an_effect_layer_still_covers_the_geometry_behind_it},
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
