#include <mediaserver/render/mask_raster.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/mask_compose_frag_dxil.h>
#include <mediaserver/shaders/mask_raster_frag_dxil.h>
#include <mediaserver/shaders/output_vert_dxil.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <span>
#include <tuple>
#include <utility>

namespace mediaserver::render {
namespace {

using CreateResult = core::Result<MaskRaster, RenderError>;

struct Vec2 final {
    float x{};
    float y{};
};

[[nodiscard]] Vec2 operator+(Vec2 left, Vec2 right) noexcept {
    return Vec2{left.x + right.x, left.y + right.y};
}

[[nodiscard]] Vec2 operator-(Vec2 left, Vec2 right) noexcept {
    return Vec2{left.x - right.x, left.y - right.y};
}

[[nodiscard]] Vec2 operator*(Vec2 value, float scale) noexcept {
    return Vec2{value.x * scale, value.y * scale};
}

[[nodiscard]] Vec2 operator/(Vec2 value, Vec2 divisor) noexcept {
    return Vec2{value.x / divisor.x, value.y / divisor.y};
}

[[nodiscard]] float dot(Vec2 left, Vec2 right) noexcept {
    return left.x * right.x + left.y * right.y;
}

[[nodiscard]] float length(Vec2 value) noexcept {
    return std::sqrt(dot(value, value));
}

[[nodiscard]] float cross(Vec2 left, Vec2 right) noexcept {
    return left.x * right.y - left.y * right.x;
}

[[nodiscard]] Vec2 lerp(Vec2 left, Vec2 right, float parameter) noexcept {
    return left + (right - left) * parameter;
}

[[nodiscard]] float saturate(float value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] bool finite(float value) noexcept {
    return std::isfinite(value);
}

[[nodiscard]] bool in_unit(float value) noexcept {
    return finite(value) && value >= 0.0F && value <= 1.0F;
}

[[nodiscard]] bool valid_color(const rhi::Color& value) noexcept {
    return in_unit(value.r) && in_unit(value.g) && in_unit(value.b) &&
        in_unit(value.a) && value.r <= value.a + 1.0e-6F &&
        value.g <= value.a + 1.0e-6F && value.b <= value.a + 1.0e-6F;
}

[[nodiscard]] bool valid_kind(scene::MaskKind kind) noexcept {
    switch (kind) {
    case scene::MaskKind::Rectangle:
    case scene::MaskKind::Ellipse:
    case scene::MaskKind::Bezier:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_combine(scene::MaskCombine combine) noexcept {
    switch (combine) {
    case scene::MaskCombine::Add:
    case scene::MaskCombine::Subtract:
    case scene::MaskCombine::Intersect:
    case scene::MaskCombine::Difference:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_coordinate_space(
    scene::MaskCoordinateSpace space) noexcept {
    switch (space) {
    case scene::MaskCoordinateSpace::SourceLayer:
    case scene::MaskCoordinateSpace::AuthoredCanvas:
        return true;
    }
    return false;
}

[[nodiscard]] bool valid_bounds(const scene::MaskBounds& bounds) noexcept {
    return in_unit(bounds.left) && in_unit(bounds.top) &&
        in_unit(bounds.right) && in_unit(bounds.bottom) &&
        bounds.left < bounds.right && bounds.top < bounds.bottom;
}

[[nodiscard]] bool valid_point(const scene::MaskPoint& point,
                               const scene::MaskInstance& mask) noexcept {
    return point.mask == mask.source && point.scene == mask.scene &&
        in_unit(point.x) && in_unit(point.y) && in_unit(point.in_x) &&
        in_unit(point.in_y) && in_unit(point.out_x) && in_unit(point.out_y);
}

[[nodiscard]] bool valid_operation(
    const scene::MaskInstance& operation) noexcept {
    if (!operation.enabled || !valid_kind(operation.kind) ||
        !valid_combine(operation.combine) ||
        !finite(operation.feather_canvas_px) ||
        operation.feather_canvas_px < 0.0F ||
        operation.feather_canvas_px > scene::max_mask_feather_canvas_px ||
        !finite(operation.expansion_canvas_px) ||
        operation.expansion_canvas_px <
            -scene::max_mask_expansion_canvas_px ||
        operation.expansion_canvas_px >
            scene::max_mask_expansion_canvas_px ||
        !in_unit(operation.opacity) || !finite(operation.rotation_turns)) {
        return false;
    }
    if (operation.kind != scene::MaskKind::Bezier) {
        return operation.points.empty() && valid_bounds(operation.bounds);
    }
    if (operation.points.size() < 3U ||
        operation.points.size() > scene::max_mask_points_per_mask) {
        return false;
    }
    for (std::size_t index = 0U; index < operation.points.size(); ++index) {
        if (!valid_point(operation.points[index], operation)) {
            return false;
        }
        if (index != 0U &&
            operation.points[index - 1U].order >=
                operation.points[index].order) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] float rectangle_distance(Vec2 point,
                                       const scene::MaskBounds& bounds,
                                       Vec2 extent,
                                       float rotation_turns) noexcept {
    const Vec2 lower{bounds.left * extent.x, bounds.top * extent.y};
    const Vec2 upper{bounds.right * extent.x, bounds.bottom * extent.y};
    const Vec2 centre = (lower + upper) * 0.5F;
    const Vec2 half_extent = (upper - lower) * 0.5F;
    constexpr float tau = 6.28318530717958647692F;
    const float angle = rotation_turns * tau;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    const Vec2 held = point - centre;
    const Vec2 local{cosine * held.x + sine * held.y,
                     -sine * held.x + cosine * held.y};
    const Vec2 delta{std::abs(local.x) - half_extent.x,
                     std::abs(local.y) - half_extent.y};
    const Vec2 outside{std::max(delta.x, 0.0F),
                       std::max(delta.y, 0.0F)};
    return length(outside) + std::min(std::max(delta.x, delta.y), 0.0F);
}

[[nodiscard]] float ellipse_distance(Vec2 point,
                                     const scene::MaskBounds& bounds,
                                     Vec2 extent,
                                     float rotation_turns) noexcept {
    const Vec2 lower{bounds.left * extent.x, bounds.top * extent.y};
    const Vec2 upper{bounds.right * extent.x, bounds.bottom * extent.y};
    const Vec2 centre = (lower + upper) * 0.5F;
    const Vec2 radius{
        std::max((upper.x - lower.x) * 0.5F, 1.0e-6F),
        std::max((upper.y - lower.y) * 0.5F, 1.0e-6F)};
    constexpr float tau = 6.28318530717958647692F;
    const float angle = rotation_turns * tau;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    const Vec2 held = point - centre;
    const Vec2 local{cosine * held.x + sine * held.y,
                     -sine * held.x + cosine * held.y};
    const float k0 = length(local / radius);
    const Vec2 radius_squared{radius.x * radius.x, radius.y * radius.y};
    const float k1 = length(local / radius_squared);
    return k1 > 1.0e-8F
        ? k0 * (k0 - 1.0F) / k1
        : -std::min(radius.x, radius.y);
}

[[nodiscard]] Vec2 position(const scene::MaskPoint& point,
                            Vec2 extent) noexcept {
    return Vec2{point.x * extent.x, point.y * extent.y};
}

[[nodiscard]] Vec2 in_handle(const scene::MaskPoint& point,
                             Vec2 extent) noexcept {
    return Vec2{point.in_x * extent.x, point.in_y * extent.y};
}

[[nodiscard]] Vec2 out_handle(const scene::MaskPoint& point,
                              Vec2 extent) noexcept {
    return Vec2{point.out_x * extent.x, point.out_y * extent.y};
}

[[nodiscard]] Vec2 cubic_point(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3,
                               float parameter) noexcept {
    const Vec2 a = lerp(p0, p1, parameter);
    const Vec2 b = lerp(p1, p2, parameter);
    const Vec2 c = lerp(p2, p3, parameter);
    const Vec2 d = lerp(a, b, parameter);
    const Vec2 e = lerp(b, c, parameter);
    return lerp(d, e, parameter);
}

void accumulate_segment(Vec2 point, Vec2 start, Vec2 finish,
                        float& minimum_distance_squared,
                        core::i32& winding) noexcept {
    const Vec2 direction = finish - start;
    const float denominator = dot(direction, direction);
    const float parameter = denominator > 0.0F
        ? saturate(dot(point - start, direction) / denominator)
        : 0.0F;
    const Vec2 delta = point - (start + direction * parameter);
    minimum_distance_squared =
        std::min(minimum_distance_squared, dot(delta, delta));
    if (start.y <= point.y) {
        if (finish.y > point.y &&
            cross(finish - start, point - start) > 0.0F) {
            ++winding;
        }
    } else if (finish.y <= point.y &&
               cross(finish - start, point - start) < 0.0F) {
        --winding;
    }
}

[[nodiscard]] float bezier_distance(
    Vec2 point, const scene::MaskInstance& operation,
    Vec2 extent) noexcept {
    float minimum_distance_squared = std::numeric_limits<float>::max();
    core::i32 winding{};
    for (std::size_t index = 0U; index < operation.points.size(); ++index) {
        const std::size_t next = index + 1U == operation.points.size()
            ? 0U
            : index + 1U;
        const Vec2 p0 = position(operation.points[index], extent);
        const Vec2 p1 = out_handle(operation.points[index], extent);
        const Vec2 p2 = in_handle(operation.points[next], extent);
        const Vec2 p3 = position(operation.points[next], extent);
        Vec2 start = p0;
        for (core::u32 sample = 1U; sample <= mask_bezier_subdivisions;
             ++sample) {
            const float parameter = static_cast<float>(sample) /
                static_cast<float>(mask_bezier_subdivisions);
            const Vec2 finish = cubic_point(p0, p1, p2, p3, parameter);
            accumulate_segment(point, start, finish,
                               minimum_distance_squared, winding);
            start = finish;
        }
    }
    const float distance =
        std::sqrt(std::max(minimum_distance_squared, 0.0F));
    return winding != 0 ? -distance : distance;
}

[[nodiscard]] float operation_coverage(
    float output_u, float output_v, const scene::MaskInstance& operation,
    const MaskRasterMapping& mapping) noexcept {
    const Vec2 extent{mapping.coordinate_width_canvas_px,
                      mapping.coordinate_height_canvas_px};
    const Vec2 point{
        (mapping.origin_x + output_u * mapping.extent_x) * extent.x,
        (mapping.origin_y + output_v * mapping.extent_y) * extent.y};
    float signed_distance{};
    switch (operation.kind) {
    case scene::MaskKind::Rectangle:
        signed_distance = rectangle_distance(
            point, operation.bounds, extent, operation.rotation_turns);
        break;
    case scene::MaskKind::Ellipse:
        signed_distance = ellipse_distance(
            point, operation.bounds, extent, operation.rotation_turns);
        break;
    case scene::MaskKind::Bezier:
        signed_distance = bezier_distance(point, operation, extent);
        break;
    }
    signed_distance -= operation.expansion_canvas_px;
    float coverage = signed_distance <= 0.0F ? 1.0F : 0.0F;
    if (operation.feather_canvas_px > 0.0F) {
        const float parameter = saturate(
            (operation.feather_canvas_px - signed_distance) /
            (2.0F * operation.feather_canvas_px));
        coverage = parameter * parameter * (3.0F - 2.0F * parameter);
    }
    if (operation.invert) {
        coverage = 1.0F - coverage;
    }
    return saturate(coverage * operation.opacity);
}

[[nodiscard]] bool valid_reference_size(core::u32 width,
                                        core::u32 height) noexcept {
    return width != 0U && height != 0U &&
        static_cast<std::size_t>(width) <=
        std::numeric_limits<std::size_t>::max() /
            static_cast<std::size_t>(height);
}

struct ShaderBlobs final {
    std::span<const core::u8> vertex;
    std::span<const core::u8> raster;
    std::span<const core::u8> compose;
    rhi::ShaderFormat format{rhi::ShaderFormat::Dxil};
    bool present{};
};

[[nodiscard]] ShaderBlobs shader_blobs() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return ShaderBlobs{
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_output_vert_dxil),
            sizeof(mediaserver_output_vert_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_mask_raster_frag_dxil),
            sizeof(mediaserver_mask_raster_frag_dxil)},
        std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(mediaserver_mask_compose_frag_dxil),
            sizeof(mediaserver_mask_compose_frag_dxil)},
        rhi::ShaderFormat::Dxil,
        true,
    };
#else
    return ShaderBlobs{};
#endif
}

[[nodiscard]] rhi::TextureDesc color_desc(core::u32 width,
                                          core::u32 height,
                                          rhi::TextureFormat format) noexcept {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = format;
    desc.color_target = true;
    desc.sampled = true;
    return desc;
}

[[nodiscard]] core::Result<rhi::Texture, RenderError>
create_color(rhi::Device& device, core::u32 width, core::u32 height,
             rhi::TextureFormat format) {
    auto texture = device.create_texture(color_desc(width, height, format));
    if (!texture) {
        return core::Result<rhi::Texture, RenderError>::failure(
            RenderError::AllocationFailed);
    }
    return core::Result<rhi::Texture, RenderError>::success(
        std::move(texture).value());
}

[[nodiscard]] core::Result<rhi::GraphicsPipeline, RenderError>
create_pipeline(rhi::Device& device, const ShaderBlobs& blobs,
                std::span<const core::u8> fragment_bytecode,
                core::u32 sampler_count, rhi::TextureFormat format) {
    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = blobs.format;
    vertex.bytecode = blobs.vertex;
    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = blobs.format;
    fragment.bytecode = fragment_bytecode;
    fragment.sampler_count = sampler_count;
    fragment.uniform_buffer_count = 1U;
    auto pipeline = device.create_pipeline(
        vertex, fragment, format, false,
        rhi::DepthState{});
    if (!pipeline) {
        return core::Result<rhi::GraphicsPipeline, RenderError>::failure(
            pipeline.error() == rhi::RhiError::ShaderFormatUnsupported
                ? RenderError::ShaderFormatUnsupported
                : RenderError::BackendFailure);
    }
    return core::Result<rhi::GraphicsPipeline, RenderError>::success(
        std::move(pipeline).value());
}

struct alignas(16) GpuMaskPoint final {
    std::array<float, 4U> position_in{};
    std::array<float, 4U> out{};
};
static_assert(sizeof(GpuMaskPoint) == 32U);

struct alignas(16) MaskRasterUniform final {
    std::array<float, 4U> mapping{};
    std::array<float, 4U> geometry{};
    std::array<float, 4U> operation{};
    std::array<core::u32, 4U> counts{};
    std::array<GpuMaskPoint, scene::max_mask_points_per_mask> points{};
};
static_assert(sizeof(MaskRasterUniform) == 8'256U);

struct alignas(16) MaskComposeUniform final {
    std::array<float, 4U> reserved{};
};
static_assert(sizeof(MaskComposeUniform) == 16U);

template <typename Value>
[[nodiscard]] std::span<const core::u8> bytes(const Value& value) noexcept {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(&value), sizeof(value)};
}

[[nodiscard]] MaskRasterUniform make_uniform(
    const scene::MaskInstance& operation,
    const MaskRasterMapping& mapping, bool seed) noexcept {
    MaskRasterUniform value{};
    value.mapping = {mapping.origin_x, mapping.origin_y, mapping.extent_x,
                     mapping.extent_y};
    value.geometry = {mapping.coordinate_width_canvas_px,
                      mapping.coordinate_height_canvas_px,
                      operation.feather_canvas_px,
                      operation.expansion_canvas_px};
    value.operation = {
        operation.opacity,
        static_cast<float>(static_cast<core::u8>(operation.kind)),
        static_cast<float>(static_cast<core::u8>(operation.combine)),
        operation.invert ? 1.0F : 0.0F};
    value.counts[0U] = static_cast<core::u32>(operation.points.size());
    value.counts[1U] = seed ? 1U : 0U;
    if (operation.kind != scene::MaskKind::Bezier) {
        value.points[0U].position_in = {
            operation.bounds.left, operation.bounds.top,
            operation.bounds.right, operation.bounds.bottom};
        value.points[0U].out[0U] = operation.rotation_turns;
        return value;
    }
    for (std::size_t index = 0U; index < operation.points.size(); ++index) {
        const scene::MaskPoint& point = operation.points[index];
        value.points[index].position_in = {
            point.x, point.y, point.in_x, point.in_y};
        value.points[index].out = {point.out_x, point.out_y, 0.0F, 0.0F};
    }
    return value;
}

[[nodiscard]] bool valid_source(const rhi::Texture& color,
                                const rhi::Texture& depth,
                                core::u32 width,
                                core::u32 height) noexcept {
    return color && color.desc().sampled &&
        color.desc().format != rhi::TextureFormat::Depth32Float &&
        color.width() == width && color.height() == height && depth &&
        depth.desc().format == rhi::TextureFormat::Depth32Float &&
        depth.desc().depth_target && depth.width() == width &&
        depth.height() == height;
}

} // namespace

bool valid_mask_raster_mapping(const MaskRasterMapping& mapping) noexcept {
    return finite(mapping.origin_x) && finite(mapping.origin_y) &&
        finite(mapping.extent_x) && finite(mapping.extent_y) &&
        finite(mapping.coordinate_width_canvas_px) &&
        finite(mapping.coordinate_height_canvas_px) &&
        mapping.origin_x >= 0.0F && mapping.origin_y >= 0.0F &&
        mapping.extent_x > 0.0F && mapping.extent_y > 0.0F &&
        mapping.origin_x + mapping.extent_x <= 1.0F &&
        mapping.origin_y + mapping.extent_y <= 1.0F &&
        mapping.coordinate_width_canvas_px > 0.0F &&
        mapping.coordinate_height_canvas_px > 0.0F;
}

bool mask_program_is_identity(const scene::MaskProgram& program) noexcept {
    return program.operations.empty();
}

bool valid_mask_program(const scene::MaskProgram& program) noexcept {
    if (mask_program_is_identity(program)) {
        return true;
    }
    if (!valid_coordinate_space(program.coordinate_space) ||
        program.operations.size() > scene::max_scene_masks ||
        program.operations.front().combine != scene::MaskCombine::Add) {
        return false;
    }
    for (std::size_t index = 0U; index < program.operations.size(); ++index) {
        const scene::MaskInstance& operation = program.operations[index];
        if (operation.target != program.target ||
            !valid_operation(operation)) {
            return false;
        }
        if (index != 0U) {
            const scene::MaskInstance& previous =
                program.operations[index - 1U];
            if (std::tuple{previous.order, previous.source.value()} >=
                std::tuple{operation.order, operation.source.value()}) {
                return false;
            }
        }
    }
    return true;
}

MaskCoverageReferenceResult mask_coverage_reference(
    core::u32 width, core::u32 height,
    const scene::MaskProgram& program,
    const MaskRasterMapping& mapping) {
    if (!valid_reference_size(width, height)) {
        return MaskCoverageReferenceResult::failure(
            RenderError::InvalidConfiguration);
    }
    const std::size_t pixel_count =
        static_cast<std::size_t>(width) * height;
    if (mask_program_is_identity(program)) {
        return MaskCoverageReferenceResult::success(
            std::vector<float>(pixel_count, 1.0F));
    }
    if (!valid_mask_program(program) ||
        !valid_mask_raster_mapping(mapping)) {
        return MaskCoverageReferenceResult::failure(
            RenderError::InvalidConfiguration);
    }
    std::vector<float> result(pixel_count);
    for (core::u32 y = 0U; y < height; ++y) {
        const float v = (static_cast<float>(y) + 0.5F) /
            static_cast<float>(height);
        for (core::u32 x = 0U; x < width; ++x) {
            const float u = (static_cast<float>(x) + 0.5F) /
                static_cast<float>(width);
            float coverage = operation_coverage(
                u, v, program.operations.front(), mapping);
            for (std::size_t index = 1U;
                 index < program.operations.size(); ++index) {
                const scene::MaskInstance& operation =
                    program.operations[index];
                coverage = scene::fold_mask_coverage(
                    coverage,
                    operation_coverage(u, v, operation, mapping),
                    operation.combine);
            }
            result[static_cast<std::size_t>(y) * width + x] =
                saturate(coverage);
        }
    }
    return MaskCoverageReferenceResult::success(std::move(result));
}

MaskedReferenceResult mask_layer_reference(
    std::span<const rhi::Color> source, core::u32 width, core::u32 height,
    const scene::MaskProgram& program,
    const MaskRasterMapping& mapping) {
    if (!valid_reference_size(width, height) ||
        source.size() != static_cast<std::size_t>(width) * height ||
        !std::all_of(source.begin(), source.end(), valid_color)) {
        return MaskedReferenceResult::failure(
            RenderError::InvalidConfiguration);
    }
    if (mask_program_is_identity(program)) {
        return MaskedReferenceResult::success(
            std::vector<rhi::Color>{source.begin(), source.end()});
    }
    auto coverage =
        mask_coverage_reference(width, height, program, mapping);
    if (!coverage) {
        return MaskedReferenceResult::failure(coverage.error());
    }
    std::vector<rhi::Color> result(source.size());
    for (std::size_t index = 0U; index < source.size(); ++index) {
        const float held = coverage.value()[index];
        result[index] = rhi::Color{
            source[index].r * held, source[index].g * held,
            source[index].b * held, source[index].a * held};
    }
    return MaskedReferenceResult::success(std::move(result));
}

bool MaskRaster::shaders_available() noexcept {
    return shader_blobs().present;
}

MaskRaster::MaskRaster(rhi::Texture matte_a, rhi::Texture matte_b,
                       rhi::Texture output_a, rhi::Texture output_b,
                       rhi::GraphicsPipeline raster_pipeline,
                       rhi::GraphicsPipeline compose_pipeline,
                       rhi::Sampler sampler) noexcept
    : matte_a_(std::move(matte_a)), matte_b_(std::move(matte_b)),
      output_a_(std::move(output_a)), output_b_(std::move(output_b)),
      raster_pipeline_(std::move(raster_pipeline)),
      compose_pipeline_(std::move(compose_pipeline)),
      sampler_(std::move(sampler)) {}

CreateResult MaskRaster::create(rhi::Device& device, core::u32 width,
                                core::u32 height,
                                rhi::TextureFormat format) {
    const ShaderBlobs blobs = shader_blobs();
    if (!blobs.present || !device.supports(blobs.format)) {
        return CreateResult::failure(RenderError::ShaderFormatUnsupported);
    }
    if (width == 0U || height == 0U) {
        return CreateResult::failure(RenderError::InvalidConfiguration);
    }
    auto matte_a = create_color(device, width, height, format);
    auto matte_b = create_color(device, width, height, format);
    auto output_a = create_color(device, width, height, format);
    auto output_b = create_color(device, width, height, format);
    if (!matte_a || !matte_b || !output_a || !output_b) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }
    auto raster = create_pipeline(device, blobs, blobs.raster, 1U, format);
    auto compose = create_pipeline(device, blobs, blobs.compose, 2U, format);
    if (!raster || !compose) {
        return CreateResult::failure(!raster ? raster.error()
                                             : compose.error());
    }
    auto sampler = device.create_sampler(rhi::Filter::Nearest);
    if (!sampler) {
        return CreateResult::failure(RenderError::AllocationFailed);
    }
    return CreateResult::success(MaskRaster{
        std::move(matte_a).value(), std::move(matte_b).value(),
        std::move(output_a).value(), std::move(output_b).value(),
        std::move(raster).value(), std::move(compose).value(),
        std::move(sampler).value()});
}

bool MaskRaster::resize(rhi::Device& device, core::u32 width,
                        core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return false;
    }
    if (this->width() == width && this->height() == height) {
        return true;
    }
    const auto format = output_a_.desc().format;
    auto matte_a = create_color(device, width, height, format);
    auto matte_b = create_color(device, width, height, format);
    auto output_a = create_color(device, width, height, format);
    auto output_b = create_color(device, width, height, format);
    if (!matte_a || !matte_b || !output_a || !output_b) {
        return false;
    }
    matte_a_ = std::move(matte_a).value();
    matte_b_ = std::move(matte_b).value();
    output_a_ = std::move(output_a).value();
    output_b_ = std::move(output_b).value();
    return true;
}

rhi::Texture& MaskRaster::output_for(const rhi::Texture& source) noexcept {
    return &source == &output_a_ ? output_b_ : output_a_;
}

void MaskRaster::record_pass(bool raster_operation) noexcept {
    ++stats_.recorded_passes;
    stats_.processed_pixels +=
        static_cast<core::u64>(width()) * static_cast<core::u64>(height());
    if (raster_operation) {
        ++stats_.rasterized_operations;
    }
}

MaskRasterResult MaskRaster::apply(
    rhi::CommandList& commands, const rhi::Texture& source_color,
    const rhi::Texture& source_depth,
    const scene::MaskProgram& program,
    const MaskRasterMapping& mapping) noexcept {
    if (!valid_source(source_color, source_depth, width(), height())) {
        return MaskRasterResult::failure(RenderError::InvalidConfiguration);
    }
    if (mask_program_is_identity(program)) {
        return MaskRasterResult::success(
            MaskedLayer{&source_color, &source_depth});
    }
    if (!valid_mask_program(program) ||
        !valid_mask_raster_mapping(mapping)) {
        return MaskRasterResult::failure(RenderError::InvalidConfiguration);
    }

    const rhi::Texture* accumulated{};
    for (std::size_t index = 0U; index < program.operations.size(); ++index) {
        rhi::Texture& target = index % 2U == 0U ? matte_a_ : matte_b_;
        const rhi::Texture& source = accumulated != nullptr
            ? *accumulated
            : source_color;
        const MaskRasterUniform uniform =
            make_uniform(program.operations[index], mapping, index == 0U);
        if (!commands.draw_output_transform(
                target, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F},
                raster_pipeline_, source, sampler_, bytes(uniform))) {
            return MaskRasterResult::failure(RenderError::BackendFailure);
        }
        record_pass(true);
        accumulated = &target;
    }

    rhi::Texture& output = output_for(source_color);
    const std::array<rhi::SampledTexture, 2U> sources{{
        rhi::SampledTexture{&source_color, &sampler_},
        rhi::SampledTexture{accumulated, &sampler_},
    }};
    const MaskComposeUniform uniform{};
    if (!commands.draw_output_transform(
            output, rhi::Color{0.0F, 0.0F, 0.0F, 0.0F},
            compose_pipeline_, sources, bytes(uniform))) {
        return MaskRasterResult::failure(RenderError::BackendFailure);
    }
    record_pass(false);
    return MaskRasterResult::success(MaskedLayer{&output, &source_depth});
}

} // namespace mediaserver::render
