#pragma once
#include <mediaserver/render/frame_graph.hpp>

#include <mediaserver/core/result.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/math.hpp>

namespace mediaserver::render {

// Only ICVFX constructs this context. All units are metres, including the
// replicated wall geometry. Physical lens controls are never raw encoders.
struct PlateLens final {
    float focal_length_m{};
    float focus_distance_m{};
    float aperture_f_number{};
    scene::Vec3 eye{}, forward{}, right{}, up{};
    scene::Vec3 wall_right{}, wall_up{}, wall_normal{};
    float wall_plane_distance{}; // dot(normal, wall_center - eye)
    float region_width_m{}, region_height_m{};
    bool operator==(const PlateLens&) const = default;
};

[[nodiscard]] bool valid(const PlateLens& lens) noexcept;
// Thin-lens sensor radius after subtracting the blur already supplied by the
// physical wall. Infinity is a valid virtual depth. No sensor crop assumption.
[[nodiscard]] float residual_coc_radius_m(const PlateLens& lens,
    float virtual_depth_m, float wall_depth_m) noexcept;

class PlateDepthOfField final {
public:
    [[nodiscard]] static core::Result<PlateDepthOfField, RenderError> create(
        rhi::Device& device, core::u32 width, core::u32 height,
        rhi::TextureFormat format) noexcept;
    [[nodiscard]] core::Result<const rhi::Texture*, RenderError> apply(
        rhi::CommandList& commands, const rhi::Texture& color,
        const rhi::Texture& depth, const PlateLens& lens,
        const scene::Mat4& view_projection, core::u32 visible_width,
        core::u32 visible_height, core::u32 maximum_radius) noexcept;
private:
    FrameGraph graph_;
    rhi::Texture output_;
    rhi::Sampler sampler_;
    rhi::GraphicsPipeline pipeline_;
};

} // namespace mediaserver::render
