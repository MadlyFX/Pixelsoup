#include <mediaserver/render/mask_raster.hpp>

#include <mediaserver/rhi/device.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} +
                                 std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define MASK_RASTER_CHECK(...)                                                \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] int difference(core::u8 left, core::u8 right) noexcept {
    const int value = static_cast<int>(left) - static_cast<int>(right);
    return value < 0 ? -value : value;
}

[[nodiscard]] core::u8 as_byte(float value) noexcept {
    return static_cast<core::u8>(std::lround(
        std::clamp(value, 0.0F, 1.0F) * 255.0F));
}

struct Image final {
    core::u32 width{};
    core::u32 height{};
    std::vector<core::u8> rgba;

    [[nodiscard]] std::size_t at(core::u32 x, core::u32 y) const noexcept {
        return (static_cast<std::size_t>(y) * width + x) * 4U;
    }

    [[nodiscard]] core::u8 channel(core::u32 x, core::u32 y,
                                   std::size_t component) const noexcept {
        return rgba[at(x, y) + component];
    }
};

[[nodiscard]] std::vector<core::u8> solid_pixels(
    core::u32 width, core::u32 height,
    const std::array<core::u8, 4U>& color) {
    std::vector<core::u8> result(
        static_cast<std::size_t>(width) * height * 4U);
    for (std::size_t index = 0U; index < result.size(); index += 4U) {
        std::copy(color.begin(), color.end(), result.begin() + index);
    }
    return result;
}

void set_pixel(std::vector<core::u8>& pixels, core::u32 width, core::u32 x,
               core::u32 y, const std::array<core::u8, 4U>& color) {
    const std::size_t index =
        (static_cast<std::size_t>(y) * width + x) * 4U;
    MASK_RASTER_CHECK(index + color.size() <= pixels.size());
    std::copy(color.begin(), color.end(), pixels.begin() + index);
}

[[nodiscard]] std::vector<rhi::Color> reference_source(
    std::span<const core::u8> pixels) {
    MASK_RASTER_CHECK((pixels.size() % 4U) == 0U);
    std::vector<rhi::Color> result(pixels.size() / 4U);
    for (std::size_t index = 0U; index < result.size(); ++index) {
        const std::size_t byte = index * 4U;
        result[index] = rhi::Color{
            static_cast<float>(pixels[byte]) / 255.0F,
            static_cast<float>(pixels[byte + 1U]) / 255.0F,
            static_cast<float>(pixels[byte + 2U]) / 255.0F,
            static_cast<float>(pixels[byte + 3U]) / 255.0F};
    }
    return result;
}

void matches_reference(const Image& image,
                       std::span<const rhi::Color> expected,
                       int tolerance = 4) {
    MASK_RASTER_CHECK(expected.size() ==
                      static_cast<std::size_t>(image.width) * image.height);
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        const std::size_t byte = index * 4U;
        MASK_RASTER_CHECK(
            difference(image.rgba[byte], as_byte(expected[index].r)) <=
            tolerance);
        MASK_RASTER_CHECK(
            difference(image.rgba[byte + 1U], as_byte(expected[index].g)) <=
            tolerance);
        MASK_RASTER_CHECK(
            difference(image.rgba[byte + 2U], as_byte(expected[index].b)) <=
            tolerance);
        MASK_RASTER_CHECK(
            difference(image.rgba[byte + 3U], as_byte(expected[index].a)) <=
            tolerance);
    }
}

[[nodiscard]] scene::MaskInstance rectangle(
    core::u64 id, core::u64 target, scene::MaskBounds bounds,
    scene::MaskCombine combine = scene::MaskCombine::Add,
    core::i64 order = 0) {
    scene::MaskInstance result{};
    result.source = doc::NodeId{id};
    result.scene = doc::NodeId{1U};
    result.target = doc::NodeId{target};
    result.kind = scene::MaskKind::Rectangle;
    result.combine = combine;
    result.order = order;
    result.bounds = bounds;
    return result;
}

[[nodiscard]] scene::MaskInstance ellipse(
    core::u64 id, core::u64 target, scene::MaskBounds bounds,
    scene::MaskCombine combine = scene::MaskCombine::Add,
    core::i64 order = 0) {
    scene::MaskInstance result = rectangle(id, target, bounds, combine, order);
    result.kind = scene::MaskKind::Ellipse;
    return result;
}

[[nodiscard]] scene::MaskPoint point(core::u64 id, core::u64 mask,
                                     core::i64 order, float x,
                                     float y) {
    scene::MaskPoint result{};
    result.source = doc::NodeId{id};
    result.scene = doc::NodeId{1U};
    result.mask = doc::NodeId{mask};
    result.order = order;
    result.x = x;
    result.y = y;
    result.in_x = x;
    result.in_y = y;
    result.out_x = x;
    result.out_y = y;
    return result;
}

[[nodiscard]] scene::MaskInstance bezier(core::u64 id, core::u64 target,
                                         core::i64 order = 0) {
    scene::MaskInstance result{};
    result.source = doc::NodeId{id};
    result.scene = doc::NodeId{1U};
    result.target = doc::NodeId{target};
    result.kind = scene::MaskKind::Bezier;
    result.combine = scene::MaskCombine::Add;
    result.order = order;
    result.points = {
        point(id * 10U + 1U, id, 0, 0.2F, 0.2F),
        point(id * 10U + 2U, id, 1, 0.8F, 0.2F),
        point(id * 10U + 3U, id, 2, 0.8F, 0.8F),
        point(id * 10U + 4U, id, 3, 0.2F, 0.8F),
    };
    return result;
}

[[nodiscard]] scene::MaskProgram program(
    std::vector<scene::MaskInstance> operations,
    scene::MaskCoordinateSpace coordinate_space =
        scene::MaskCoordinateSpace::SourceLayer) {
    const doc::NodeId target = operations.empty()
        ? doc::NodeId{}
        : operations.front().target;
    return scene::MaskProgram{
        target, coordinate_space, std::move(operations)};
}

[[nodiscard]] render::MaskRasterMapping mapping(float width,
                                                float height) noexcept {
    return render::affine_mask_raster_mapping(0.0F, 0.0F, 1.0F, 1.0F,
                                              width, height);
}

[[nodiscard]] rhi::Device open_device() {
    rhi::DeviceConfig config{};
    config.debug = false;
    auto created = rhi::Device::create(config);
    check(created.has_value(), "device creation", __LINE__);
    return std::move(created).value();
}

[[nodiscard]] rhi::Texture upload_source(rhi::Device& device,
                                         core::u32 width, core::u32 height,
                                         std::span<const core::u8> rgba) {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    auto texture = device.create_texture(desc);
    check(texture.has_value(), "source texture", __LINE__);
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "upload command list", __LINE__);
    auto commands = std::move(acquired).value();
    check(commands.upload(texture.value(), rgba), "source upload", __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "upload submit", __LINE__);
    check(std::move(submitted).value().wait(), "upload wait", __LINE__);
    return std::move(texture).value();
}

[[nodiscard]] rhi::Texture create_depth(rhi::Device& device,
                                        core::u32 width,
                                        core::u32 height) {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Depth32Float;
    desc.depth_target = true;
    auto texture = device.create_texture(desc);
    check(texture.has_value(), "depth texture", __LINE__);
    return std::move(texture).value();
}

struct GpuResult final {
    Image image;
    const rhi::Texture* color{};
    const rhi::Texture* depth{};
};

template <typename Apply>
[[nodiscard]] GpuResult run(rhi::Device& device, Apply&& apply) {
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "mask command list", __LINE__);
    auto commands = std::move(acquired).value();
    auto result = apply(commands);
    check(result.has_value(), "mask apply", __LINE__);
    const render::MaskedLayer layer = result.value();
    check(layer.color != nullptr, "masked color", __LINE__);
    check(layer.depth != nullptr, "retained depth", __LINE__);
    const core::u32 byte_count =
        layer.color->width() * layer.color->height() * 4U;
    auto readback = device.create_readback(byte_count);
    check(readback.has_value(), "mask readback", __LINE__);
    check(commands.download(*layer.color, readback.value()),
          "mask download", __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "mask submit", __LINE__);
    check(std::move(submitted).value().wait(), "mask wait", __LINE__);
    Image image{};
    image.width = layer.color->width();
    image.height = layer.color->height();
    const auto mapped = readback.value().map();
    check(mapped.size() >= byte_count, "mask map", __LINE__);
    image.rgba.assign(mapped.begin(), mapped.begin() + byte_count);
    readback.value().unmap();
    return GpuResult{std::move(image), layer.color, layer.depth};
}

void cpu_shapes_folds_and_validation_are_deterministic() {
    constexpr core::u32 width = 20U;
    constexpr core::u32 height = 16U;
    const auto raster_mapping = mapping(200.0F, 160.0F);

    auto held_rectangle = rectangle(
        10U, 100U, scene::MaskBounds{0.2F, 0.25F, 0.8F, 0.75F});
    held_rectangle.feather_canvas_px = 3.0F;
    held_rectangle.expansion_canvas_px = 2.0F;
    held_rectangle.opacity = 0.625F;
    const auto rectangle_program = program({held_rectangle});
    const auto first = render::mask_coverage_reference(
        width, height, rectangle_program, raster_mapping);
    const auto second = render::mask_coverage_reference(
        width, height, rectangle_program, raster_mapping);
    MASK_RASTER_CHECK(first.has_value());
    MASK_RASTER_CHECK(second.has_value());
    MASK_RASTER_CHECK(first.value() == second.value());
    MASK_RASTER_CHECK(first.value()[8U * width + 10U] > 0.61F);
    MASK_RASTER_CHECK(first.value().front() == 0.0F);
    MASK_RASTER_CHECK(render::mask_program_bounds(rectangle_program) ==
                      render::MaskBoundsExpansion{});

    auto held_ellipse = ellipse(
        11U, 100U, scene::MaskBounds{0.2F, 0.2F, 0.8F, 0.8F});
    held_ellipse.invert = true;
    const auto ellipse_result = render::mask_coverage_reference(
        width, height, program({held_ellipse}), raster_mapping);
    MASK_RASTER_CHECK(ellipse_result.has_value());
    MASK_RASTER_CHECK(ellipse_result.value()[8U * width + 10U] == 0.0F);
    MASK_RASTER_CHECK(ellipse_result.value().front() == 1.0F);

    auto held_bezier = bezier(12U, 100U);
    held_bezier.feather_canvas_px = 1.0F;
    const auto bezier_result = render::mask_coverage_reference(
        width, height, program({held_bezier}), raster_mapping);
    MASK_RASTER_CHECK(bezier_result.has_value());
    MASK_RASTER_CHECK(bezier_result.value()[8U * width + 10U] > 0.99F);
    MASK_RASTER_CHECK(bezier_result.value().front() == 0.0F);

    const auto a = rectangle(
        20U, 100U, scene::MaskBounds{0.1F, 0.1F, 0.7F, 0.9F},
        scene::MaskCombine::Add, 0);
    const auto b = rectangle(
        21U, 100U, scene::MaskBounds{0.35F, 0.1F, 0.9F, 0.9F},
        scene::MaskCombine::Subtract, 1);
    const auto c = rectangle(
        22U, 100U, scene::MaskBounds{0.0F, 0.35F, 1.0F, 0.65F},
        scene::MaskCombine::Intersect, 1);
    const auto d = rectangle(
        23U, 100U, scene::MaskBounds{0.0F, 0.0F, 0.25F, 1.0F},
        scene::MaskCombine::Difference, 2);
    const auto folded = program({a, b, c, d});
    MASK_RASTER_CHECK(render::valid_mask_program(folded));
    const auto folded_result = render::mask_coverage_reference(
        width, height, folded, raster_mapping);
    MASK_RASTER_CHECK(folded_result.has_value());
    MASK_RASTER_CHECK(folded_result.value()[8U * width + 2U] == 0.0F);
    MASK_RASTER_CHECK(folded_result.value()[8U * width + 6U] == 1.0F);
    MASK_RASTER_CHECK(folded_result.value()[8U * width + 12U] == 0.0F);

    scene::MaskProgram malformed = folded;
    malformed.operations.front().combine = scene::MaskCombine::Subtract;
    MASK_RASTER_CHECK(!render::valid_mask_program(malformed));
    MASK_RASTER_CHECK(!render::mask_coverage_reference(
        width, height, malformed, raster_mapping).has_value());
    malformed = folded;
    std::swap(malformed.operations[1U], malformed.operations[2U]);
    MASK_RASTER_CHECK(!render::valid_mask_program(malformed));
    malformed = program({bezier(30U, 100U)});
    malformed.operations.front().points.resize(2U);
    MASK_RASTER_CHECK(!render::valid_mask_program(malformed));
}

void gpu_identity_is_byte_pointer_pass_and_stat_inert() {
    auto device = open_device();
    constexpr core::u32 width = 7U;
    constexpr core::u32 height = 5U;
    auto pixels = solid_pixels(width, height, {64U, 32U, 16U, 128U});
    set_pixel(pixels, width, 0U, 0U, {0U, 0U, 0U, 0U});
    const auto source = upload_source(device, width, height, pixels);
    const auto depth = create_depth(device, width, height);
    auto created = render::MaskRaster::create(device, width, height);
    MASK_RASTER_CHECK(created.has_value());
    auto raster = std::move(created).value();

    scene::MaskProgram identity{};
    identity.coordinate_space =
        static_cast<scene::MaskCoordinateSpace>(255U);
    render::MaskRasterMapping stale{};
    stale.coordinate_width_canvas_px =
        std::numeric_limits<float>::quiet_NaN();
    const GpuResult held = run(
        device,
        [&raster, &source, &depth, &identity, &stale](
            rhi::CommandList& commands) {
            return raster.apply(commands, source, depth, identity, stale);
        });
    MASK_RASTER_CHECK(held.color == &source);
    MASK_RASTER_CHECK(held.depth == &depth);
    MASK_RASTER_CHECK(held.image.rgba == pixels);
    MASK_RASTER_CHECK(raster.stats() == render::MaskRasterStats{});

    scene::MaskProgram malformed = program({rectangle(
        10U, 100U, scene::MaskBounds{0.8F, 0.2F, 0.2F, 0.8F})});
    auto acquired = device.acquire_commands();
    MASK_RASTER_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    const auto rejected = raster.apply(
        commands, source, depth, malformed, mapping(70.0F, 50.0F));
    MASK_RASTER_CHECK(!rejected.has_value());
    MASK_RASTER_CHECK(rejected.error() ==
                      render::RenderError::InvalidConfiguration);
    MASK_RASTER_CHECK(raster.stats() == render::MaskRasterStats{});
}

void rectangle_ellipse_feather_expansion_and_alpha_match_gpu() {
    auto device = open_device();
    constexpr core::u32 width = 18U;
    constexpr core::u32 height = 14U;
    auto pixels = solid_pixels(width, height, {96U, 48U, 24U, 128U});
    for (core::u32 x = 0U; x < width; ++x) {
        set_pixel(pixels, width, x, 0U, {0U, 0U, 0U, 0U});
        set_pixel(pixels, width, x, height - 1U, {0U, 0U, 0U, 0U});
    }
    for (core::u32 y = 0U; y < height; ++y) {
        set_pixel(pixels, width, 0U, y, {0U, 0U, 0U, 0U});
        set_pixel(pixels, width, width - 1U, y, {0U, 0U, 0U, 0U});
    }
    const auto colors = reference_source(pixels);
    const auto source = upload_source(device, width, height, pixels);
    const auto depth = create_depth(device, width, height);
    auto created = render::MaskRaster::create(device, width, height);
    MASK_RASTER_CHECK(created.has_value());
    auto raster = std::move(created).value();

    auto operation = rectangle(
        10U, 100U, scene::MaskBounds{0.25F, 0.25F, 0.75F, 0.75F});
    operation.feather_canvas_px = 5.0F;
    operation.expansion_canvas_px = 4.0F;
    operation.opacity = 0.6F;
    operation.rotation_turns = 0.125F;
    const auto held_program = program({operation});
    const auto held_mapping = mapping(180.0F, 140.0F);
    const auto expected = render::mask_layer_reference(
        colors, width, height, held_program, held_mapping);
    MASK_RASTER_CHECK(expected.has_value());
    const GpuResult masked = run(
        device,
        [&raster, &source, &depth, &held_program, &held_mapping](
            rhi::CommandList& commands) {
            return raster.apply(
                commands, source, depth, held_program, held_mapping);
        });
    matches_reference(masked.image, expected.value(), 4);
    MASK_RASTER_CHECK(masked.color != &source);
    MASK_RASTER_CHECK(masked.depth == &depth);
    MASK_RASTER_CHECK(masked.image.channel(0U, 0U, 3U) == 0U);
    MASK_RASTER_CHECK(masked.image.channel(0U, 0U, 0U) == 0U);
    MASK_RASTER_CHECK(masked.image.channel(width / 2U, height / 2U, 3U) >=
                      75U);
    MASK_RASTER_CHECK(raster.stats() ==
                      render::MaskRasterStats{2U, 504U, 1U});

    raster.reset_stats();
    auto ellipse_operation = ellipse(
        11U, 100U, scene::MaskBounds{0.15F, 0.15F, 0.85F, 0.85F});
    ellipse_operation.invert = true;
    ellipse_operation.opacity = 0.75F;
    ellipse_operation.feather_canvas_px = 2.0F;
    ellipse_operation.rotation_turns = -0.08F;
    const auto ellipse_program = program({ellipse_operation});
    const auto ellipse_expected = render::mask_layer_reference(
        colors, width, height, ellipse_program, held_mapping);
    MASK_RASTER_CHECK(ellipse_expected.has_value());
    const GpuResult ellipse_masked = run(
        device,
        [&raster, &source, &depth, &ellipse_program, &held_mapping](
            rhi::CommandList& commands) {
            return raster.apply(
                commands, source, depth, ellipse_program, held_mapping);
        });
    matches_reference(ellipse_masked.image, ellipse_expected.value(), 4);
    MASK_RASTER_CHECK(ellipse_masked.depth == &depth);
}

void closed_bezier_authored_canvas_matches_gpu_and_retains_depth() {
    auto device = open_device();
    constexpr core::u32 width = 24U;
    constexpr core::u32 height = 14U;
    const auto pixels = solid_pixels(width, height, {51U, 102U, 153U, 255U});
    const auto colors = reference_source(pixels);
    const auto source = upload_source(device, width, height, pixels);
    const auto depth = create_depth(device, width, height);
    auto created = render::MaskRaster::create(device, width, height);
    MASK_RASTER_CHECK(created.has_value());
    auto raster = std::move(created).value();

    auto operation = bezier(20U, 500U);
    operation.feather_canvas_px = 1.5F;
    operation.opacity = 0.8F;
    const auto group_program = program(
        {operation}, scene::MaskCoordinateSpace::AuthoredCanvas);
    const render::MaskRasterMapping group_mapping =
        render::affine_mask_raster_mapping(
            0.25F, 0.0F, 0.5F, 1.0F, 480.0F, 280.0F);
    MASK_RASTER_CHECK(render::valid_mask_program(group_program));
    const auto expected = render::mask_layer_reference(
        colors, width, height, group_program, group_mapping);
    MASK_RASTER_CHECK(expected.has_value());
    const GpuResult masked = run(
        device,
        [&raster, &source, &depth, &group_program, &group_mapping](
            rhi::CommandList& commands) {
            return raster.apply(
                commands, source, depth, group_program, group_mapping);
        });
    matches_reference(masked.image, expected.value(), 5);
    MASK_RASTER_CHECK(masked.depth == &depth);
    MASK_RASTER_CHECK(masked.image.channel(width / 2U, height / 2U, 3U) >=
                      200U);
    MASK_RASTER_CHECK(masked.image.channel(width - 1U, 0U, 3U) == 0U);
}

void ordered_add_subtract_intersect_difference_matches_gpu() {
    auto device = open_device();
    constexpr core::u32 width = 20U;
    constexpr core::u32 height = 12U;
    const auto pixels = solid_pixels(width, height, {255U, 128U, 64U, 255U});
    const auto colors = reference_source(pixels);
    const auto source = upload_source(device, width, height, pixels);
    const auto depth = create_depth(device, width, height);
    auto created = render::MaskRaster::create(device, width, height);
    MASK_RASTER_CHECK(created.has_value());
    auto raster = std::move(created).value();

    const auto held_program = program({
        rectangle(20U, 100U, {0.1F, 0.1F, 0.7F, 0.9F},
                  scene::MaskCombine::Add, 0),
        rectangle(21U, 100U, {0.35F, 0.1F, 0.9F, 0.9F},
                  scene::MaskCombine::Subtract, 1),
        rectangle(22U, 100U, {0.0F, 0.35F, 1.0F, 0.65F},
                  scene::MaskCombine::Intersect, 1),
        rectangle(23U, 100U, {0.0F, 0.0F, 0.25F, 1.0F},
                  scene::MaskCombine::Difference, 2),
    });
    const auto held_mapping = mapping(200.0F, 120.0F);
    const auto expected = render::mask_layer_reference(
        colors, width, height, held_program, held_mapping);
    MASK_RASTER_CHECK(expected.has_value());
    const GpuResult masked = run(
        device,
        [&raster, &source, &depth, &held_program, &held_mapping](
            rhi::CommandList& commands) {
            return raster.apply(
                commands, source, depth, held_program, held_mapping);
        });
    matches_reference(masked.image, expected.value(), 4);
    MASK_RASTER_CHECK(masked.depth == &depth);
    MASK_RASTER_CHECK(raster.stats() ==
                      render::MaskRasterStats{5U, 1'200U, 4U});
}

void resize_is_transactional() {
    auto device = open_device();
    auto created = render::MaskRaster::create(device, 7U, 5U);
    MASK_RASTER_CHECK(created.has_value());
    auto raster = std::move(created).value();
    MASK_RASTER_CHECK(!raster.resize(device, 0U, 9U));
    MASK_RASTER_CHECK(raster.width() == 7U && raster.height() == 5U);
    MASK_RASTER_CHECK(raster.resize(device, 17U, 9U));
    MASK_RASTER_CHECK(raster.width() == 17U && raster.height() == 9U);
    MASK_RASTER_CHECK(raster.resize(device, 17U, 9U));
    MASK_RASTER_CHECK(raster.stats() == render::MaskRasterStats{});
}

struct MaskCoordinate final {
    float x{};
    float y{};
};

[[nodiscard]] MaskCoordinate apply(const render::MaskRasterMapping& mapping,
                                   float u, float v) noexcept {
    const auto& m = mapping.mask_from_output;
    const float depth = m[6U] * u + m[7U] * v + m[8U];
    return MaskCoordinate{(m[0U] * u + m[1U] * v + m[2U]) / depth,
                          (m[3U] * u + m[4U] * v + m[5U]) / depth};
}

void source_layer_mapping_follows_the_drawable_plane() {
    constexpr scene::Canvas canvas{1920U, 1080U};
    const scene::Camera camera{};
    // Off to the right and turned away, so the quad is seen in perspective
    // and the middle of the frame is not on it at all.
    const scene::Mat4 world = scene::multiply(
        scene::translation(scene::Vec3{1.6F, 0.3F, -5.0F}),
        scene::multiply(scene::rotation(0.1F, 0.04F, 0.0F),
                        scene::scaling(scene::Vec3{2.0F, 1.2F, 1.0F})));
    const scene::Mat4 view_projection =
        scene::view_for_slice(camera, canvas, scene::whole(canvas))
            .view_projection;
    const scene::MaskCoordinateExtent extent = scene::mask_source_extent(
        camera, canvas, world, scene::Primitive::Quad);
    MASK_RASTER_CHECK(extent.width > 100.0F && extent.width < 1000.0F);
    MASK_RASTER_CHECK(extent.height > 100.0F && extent.height < 1000.0F);

    // Where scene projection puts a point of the quad's UV square.
    const auto canvas_point = [&world, &camera, canvas](float u, float v) {
        const scene::Vec3 local{-0.5F + u, 0.5F - v, 0.0F};
        return scene::project_to_canvas(
            camera, canvas, scene::transform_point(world, local));
    };

    const auto whole = render::source_layer_mask_mapping(
        view_projection, canvas, scene::whole(canvas), world,
        scene::Primitive::Quad, extent);
    // A padded slice: the output covers this canvas rectangle only.
    constexpr scene::CanvasRect padded{1000U, 200U, 900U, 700U};
    const auto sliced = render::source_layer_mask_mapping(
        view_projection, canvas, padded, world, scene::Primitive::Quad,
        extent);
    MASK_RASTER_CHECK(whole.has_value() && sliced.has_value());
    MASK_RASTER_CHECK(render::valid_mask_raster_mapping(*whole));
    MASK_RASTER_CHECK(whole->coordinate_width_canvas_px == extent.width);
    MASK_RASTER_CHECK(whole->coordinate_height_canvas_px == extent.height);
    for (const MaskCoordinate expected :
         {MaskCoordinate{0.1F, 0.2F}, MaskCoordinate{0.5F, 0.5F},
          MaskCoordinate{0.9F, 0.7F}, MaskCoordinate{0.3F, 0.95F}}) {
        const scene::CanvasProjection at =
            canvas_point(expected.x, expected.y);
        MASK_RASTER_CHECK(at.visible);
        const MaskCoordinate from_whole = apply(
            *whole, at.x / static_cast<float>(canvas.width),
            at.y / static_cast<float>(canvas.height));
        MASK_RASTER_CHECK(std::abs(from_whole.x - expected.x) < 1.0e-3F);
        MASK_RASTER_CHECK(std::abs(from_whole.y - expected.y) < 1.0e-3F);
        const MaskCoordinate from_slice = apply(
            *sliced,
            (at.x - static_cast<float>(padded.x)) /
                static_cast<float>(padded.width),
            (at.y - static_cast<float>(padded.y)) /
                static_cast<float>(padded.height));
        MASK_RASTER_CHECK(std::abs(from_slice.x - expected.x) < 1.0e-3F);
        MASK_RASTER_CHECK(std::abs(from_slice.y - expected.y) < 1.0e-3F);
    }

    // Coverage lands on the quad, not on the same fraction of the frame.
    constexpr core::u32 width = 192U;
    constexpr core::u32 height = 108U;
    const auto coverage = render::mask_coverage_reference(
        width, height,
        program({rectangle(40U, 100U,
                           scene::MaskBounds{0.25F, 0.25F, 0.75F, 0.75F})}),
        *whole);
    MASK_RASTER_CHECK(coverage.has_value());
    const auto sample = [&coverage](scene::CanvasProjection at) {
        const auto x = static_cast<std::size_t>(at.x / 10.0F);
        const auto y = static_cast<std::size_t>(at.y / 10.0F);
        return coverage.value()[y * width + x];
    };
    MASK_RASTER_CHECK(sample(canvas_point(0.5F, 0.5F)) == 1.0F);
    MASK_RASTER_CHECK(sample(canvas_point(0.4F, 0.65F)) == 1.0F);
    MASK_RASTER_CHECK(sample(canvas_point(0.1F, 0.5F)) == 0.0F);
    MASK_RASTER_CHECK(sample(canvas_point(0.5F, 0.9F)) == 0.0F);
    MASK_RASTER_CHECK(
        sample(scene::CanvasProjection{960.0F, 540.0F, 0.0F, true}) == 0.0F);

    // A plane through the eye is seen edge-on: no pixel lies on it, so the
    // mask covers nothing rather than dividing by zero.
    const scene::Mat4 edge_on = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 0.0F, -5.0F}),
        scene::scaling(scene::Vec3{2.0F, 1.0F, 2.0F}));
    const auto flat = render::source_layer_mask_mapping(
        view_projection, canvas, scene::whole(canvas), edge_on,
        scene::Primitive::Plane, extent);
    MASK_RASTER_CHECK(flat.has_value());
    MASK_RASTER_CHECK(render::valid_mask_raster_mapping(*flat));
    const auto flat_coverage = render::mask_coverage_reference(
        16U, 9U,
        program({rectangle(41U, 100U,
                           scene::MaskBounds{0.0F, 0.0F, 1.0F, 1.0F})}),
        *flat);
    MASK_RASTER_CHECK(flat_coverage.has_value());
    MASK_RASTER_CHECK(std::all_of(flat_coverage.value().begin(),
                                  flat_coverage.value().end(),
                                  [](float value) { return value == 0.0F; }));

    // A volume has no one surface for mask coordinates to name.
    MASK_RASTER_CHECK(!render::source_layer_mask_mapping(
                           view_projection, canvas, scene::whole(canvas),
                           world, scene::Primitive::Box, extent)
                           .has_value());
}

void perspective_source_layer_coverage_matches_gpu() {
    auto device = open_device();
    constexpr core::u32 width = 48U;
    constexpr core::u32 height = 27U;
    const auto pixels = solid_pixels(width, height, {200U, 120U, 40U, 255U});
    const auto colors = reference_source(pixels);
    const auto source = upload_source(device, width, height, pixels);
    const auto depth = create_depth(device, width, height);
    auto created = render::MaskRaster::create(device, width, height);
    MASK_RASTER_CHECK(created.has_value());
    auto raster = std::move(created).value();

    constexpr scene::Canvas canvas{1920U, 1080U};
    const scene::Camera camera{};
    const scene::Mat4 world = scene::multiply(
        scene::translation(scene::Vec3{0.4F, -0.2F, -3.0F}),
        scene::multiply(scene::rotation(0.12F, -0.06F, 0.02F),
                        scene::scaling(scene::Vec3{2.4F, 1.5F, 1.0F})));
    const auto layer_mapping = render::source_layer_mask_mapping(
        scene::view_for_slice(camera, canvas, scene::whole(canvas))
            .view_projection,
        canvas, scene::CanvasRect{240U, 135U, 1440U, 810U}, world,
        scene::Primitive::Quad,
        scene::mask_source_extent(camera, canvas, world,
                                  scene::Primitive::Quad));
    MASK_RASTER_CHECK(layer_mapping.has_value());
    auto operation = rectangle(
        12U, 100U, scene::MaskBounds{0.2F, 0.3F, 0.7F, 0.8F});
    operation.rotation_turns = 0.07F;
    operation.feather_canvas_px = 24.0F;
    const auto held_program = program({operation});
    const auto expected = render::mask_layer_reference(
        colors, width, height, held_program, *layer_mapping);
    MASK_RASTER_CHECK(expected.has_value());
    const auto opaque = std::count_if(
        expected.value().begin(), expected.value().end(),
        [](const rhi::Color& color) { return color.a > 0.99F; });
    const auto clear = std::count_if(
        expected.value().begin(), expected.value().end(),
        [](const rhi::Color& color) { return color.a < 0.01F; });
    MASK_RASTER_CHECK(opaque > 20 && clear > 20);
    const auto& held_mapping = *layer_mapping;
    const GpuResult masked = run(
        device,
        [&raster, &source, &depth, &held_program, &held_mapping](
            rhi::CommandList& commands) {
            return raster.apply(
                commands, source, depth, held_program, held_mapping);
        });
    matches_reference(masked.image, expected.value(), 4);
    MASK_RASTER_CHECK(masked.depth == &depth);
}

struct Case final {
    const char* name;
    void (*function)();
    bool requires_gpu{};
};

constexpr std::array<Case, 8U> cases{{
    {"CPU shapes, folds, and validation are deterministic",
     &cpu_shapes_folds_and_validation_are_deterministic, false},
    {"Source-layer mapping follows the drawable plane",
     &source_layer_mapping_follows_the_drawable_plane, false},
    {"GPU identity is byte, pointer, pass, and stat inert",
     &gpu_identity_is_byte_pointer_pass_and_stat_inert, true},
    {"Rectangle, ellipse, feather, expansion, and alpha match GPU",
     &rectangle_ellipse_feather_expansion_and_alpha_match_gpu, true},
    {"Closed Bezier authored-canvas coverage matches GPU and retains depth",
     &closed_bezier_authored_canvas_matches_gpu_and_retains_depth, true},
    {"Ordered Add/Subtract/Intersect/Difference matches GPU",
     &ordered_add_subtract_intersect_difference_matches_gpu, true},
    {"Resize is transactional", &resize_is_transactional, true},
    {"Perspective source-layer coverage matches GPU",
     &perspective_source_layer_coverage_matches_gpu, true},
}};

[[nodiscard]] bool gpu_mask_available() {
    if (!rhi::Device::available() || !render::MaskRaster::shaders_available()) {
        return false;
    }
    auto device = open_device();
    const auto probe = render::MaskRaster::create(device, 4U, 4U);
    if (!probe &&
        probe.error() == render::RenderError::ShaderFormatUnsupported) {
        return false;
    }
    MASK_RASTER_CHECK(probe.has_value());
    return true;
}

} // namespace

int main() {
    bool gpu_available = false;
    try {
        gpu_available = gpu_mask_available();
    } catch (const std::exception& error) {
        std::cout << "[skip] mask raster GPU unavailable: " << error.what()
                  << '\n';
    }

    std::size_t failures{};
    std::size_t skipped{};
    for (const Case& test : cases) {
        if (test.requires_gpu && !gpu_available) {
            ++skipped;
            std::cout << "[skip] " << test.name << '\n';
            continue;
        }
        try {
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << cases.size() - failures - skipped << '/' << cases.size()
              << " tests passed";
    if (skipped != 0U) {
        std::cout << " (" << skipped << " GPU tests skipped)";
    }
    std::cout << '\n';
    return failures == 0U ? 0 : 1;
}
