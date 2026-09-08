#include <mediaserver/render/plate_depth_of_field.hpp>
#include <mediaserver/scene/scene.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/output_vert_dxil.h>
#include <mediaserver/shaders/plate_dof_frag_dxil.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>

namespace mediaserver::render {
namespace {
struct Constants final {
    scene::Mat4 inverse;
    std::array<float, 4> extent, optics, eye, forward, right, up;
    std::array<float, 4> wall_right, wall_up, wall_normal;
};
static_assert(sizeof(Constants) == 208U);
std::array<float, 4> packed(scene::Vec3 v, float w = 0.0F) noexcept {
    return {v.x, v.y, v.z, w};
}
} // namespace

bool valid(const PlateLens& lens) noexcept {
    const auto unit = [](scene::Vec3 v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)
            && std::abs(scene::dot(v, v) - 1.0F) < 0.001F;
    };
    return std::isfinite(lens.focal_length_m) && lens.focal_length_m >= 0.001F && lens.focal_length_m <= 2.0F
        && std::isfinite(lens.focus_distance_m) && lens.focus_distance_m > lens.focal_length_m
        && lens.focus_distance_m <= 100000.0F
        && std::isfinite(lens.aperture_f_number) && lens.aperture_f_number >= 0.1F && lens.aperture_f_number <= 128.0F
        && std::isfinite(lens.wall_plane_distance) && lens.wall_plane_distance < -0.00001F
        && std::isfinite(lens.region_width_m) && lens.region_width_m > 0.0F
        && std::isfinite(lens.region_height_m) && lens.region_height_m > 0.0F
        && std::isfinite(lens.eye.x) && std::isfinite(lens.eye.y) && std::isfinite(lens.eye.z)
        && unit(lens.forward) && unit(lens.right) && unit(lens.up)
        && unit(lens.wall_right) && unit(lens.wall_up) && unit(lens.wall_normal);
}

float residual_coc_radius_m(const PlateLens& lens, float depth, float wall_depth) noexcept {
    if (!valid(lens) || std::isnan(depth) || depth <= 0.0F
        || !std::isfinite(wall_depth) || wall_depth <= 0.0F) return 0.0F;
    const double f = lens.focal_length_m, focus = lens.focus_distance_m;
    const double residual = std::max(std::abs(1.0 - focus / depth)
        - std::abs(1.0 - focus / wall_depth), 0.0);
    return static_cast<float>(f * f / (2.0 * lens.aperture_f_number * (focus - f)) * residual);
}

core::Result<PlateDepthOfField, RenderError> PlateDepthOfField::create(
    rhi::Device& device, core::u32 width, core::u32 height, rhi::TextureFormat format) noexcept {
    using Result = core::Result<PlateDepthOfField, RenderError>;
#if !defined(MEDIASERVER_HAS_DXIL_SHADERS)
    static_cast<void>(device); static_cast<void>(width); static_cast<void>(height); static_cast<void>(format);
    return Result::failure(RenderError::ShaderFormatUnsupported);
#else
    if (width == 0U || height == 0U) return Result::failure(RenderError::InvalidConfiguration);
    rhi::TextureDesc desc{}; desc.width = width; desc.height = height; desc.format = format;
    desc.sampled = desc.color_target = true;
    auto output = device.create_texture(desc);
    auto sampler = device.create_sampler(rhi::Filter::Nearest);
    if (!output || !sampler) return Result::failure(RenderError::AllocationFailed);
    rhi::ShaderDesc vertex{}; vertex.stage = rhi::ShaderStage::Vertex;
    vertex.bytecode = {reinterpret_cast<const core::u8*>(mediaserver_output_vert_dxil), sizeof(mediaserver_output_vert_dxil)};
    rhi::ShaderDesc fragment{}; fragment.stage = rhi::ShaderStage::Fragment;
    fragment.sampler_count = 2U; fragment.uniform_buffer_count = 1U;
    fragment.bytecode = {reinterpret_cast<const core::u8*>(mediaserver_plate_dof_frag_dxil), sizeof(mediaserver_plate_dof_frag_dxil)};
    auto pipeline = device.create_pipeline(vertex, fragment, format, false, {});
    if (!pipeline) return Result::failure(RenderError::BackendFailure);
    PlateDepthOfField result;
    result.output_ = std::move(output).value(); result.sampler_ = std::move(sampler).value();
    result.pipeline_ = std::move(pipeline).value();
    return Result::success(std::move(result));
#endif
}

core::Result<const rhi::Texture*, RenderError> PlateDepthOfField::apply(
    rhi::CommandList& commands, const rhi::Texture& color, const rhi::Texture& depth,
    const PlateLens& lens, const scene::Mat4& view_projection,
    core::u32 visible_width, core::u32 visible_height, core::u32 maximum_radius) noexcept {
    using Result = core::Result<const rhi::Texture*, RenderError>;
    if (maximum_radius == 0U) return Result::success(&color);
    if (!valid(lens) || maximum_radius > scene::max_plate_dof_radius || visible_width == 0U || visible_height == 0U
        || color.width() != output_.width() || color.height() != output_.height()
        || color.desc().format != output_.desc().format || &color == &output_
        || depth.width() != color.width() || depth.height() != color.height()
        || (depth.desc().format != rhi::TextureFormat::R32Float && depth.desc().format != rhi::TextureFormat::Depth32Float))
        return Result::failure(RenderError::InvalidConfiguration);
    const float f = lens.focal_length_m, focus = lens.focus_distance_m;
    const Constants constants{scene::invert(view_projection),
        {static_cast<float>(color.width()), static_cast<float>(color.height()), static_cast<float>(maximum_radius), 0},
        {f, focus, f * f / (2.0F * lens.aperture_f_number * (focus - f)), 0},
        packed(lens.eye), packed(lens.forward), packed(lens.right), packed(lens.up),
        packed(lens.wall_right, static_cast<float>(visible_width) / lens.region_width_m),
        packed(lens.wall_up, static_cast<float>(visible_height) / lens.region_height_m),
        packed(lens.wall_normal, lens.wall_plane_distance)};
    const std::array inputs{rhi::SampledTexture{&color, &sampler_}, rhi::SampledTexture{&depth, &sampler_}};
    if (!commands.draw_output_transform(output_, {0, 0, 0, 0}, pipeline_, inputs,
        {reinterpret_cast<const core::u8*>(&constants), sizeof(constants)}))
        return Result::failure(RenderError::BackendFailure);
    return Result::success(&output_);
}
} // namespace mediaserver::render
