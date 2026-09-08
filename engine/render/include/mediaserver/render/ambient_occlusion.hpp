#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/math.hpp>

// Screen-space ambient occlusion.
//
// Image-based lighting made ambient a bright omnidirectional term, and without
// occlusion that term reaches into every crease and under every object. On a
// volume the specific failure is a prop that reads as hovering a centimetre
// above the floor, which a camera records and a grade cannot fix. This is the
// pass that puts objects back on the ground.
namespace mediaserver::render {

struct AmbientOcclusionStats final {
    core::u64 dispatches{};

    bool operator==(const AmbientOcclusionStats&) const = default;
};

class AmbientOcclusion final {
public:
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<AmbientOcclusion, RenderError> create(
        rhi::Device& device, core::u32 width, core::u32 height) noexcept;

    AmbientOcclusion(const AmbientOcclusion&) = delete;
    AmbientOcclusion& operator=(const AmbientOcclusion&) = delete;
    AmbientOcclusion(AmbientOcclusion&&) = default;
    AmbientOcclusion& operator=(AmbientOcclusion&&) = default;
    ~AmbientOcclusion() = default;

    [[nodiscard]] core::u32 width() const noexcept {
        return occlusion_.width();
    }
    [[nodiscard]] core::u32 height() const noexcept {
        return occlusion_.height();
    }
    [[nodiscard]] rhi::Texture& target() noexcept { return occlusion_; }

    [[nodiscard]] bool resize(rhi::Device& device, core::u32 width,
                              core::u32 height) noexcept;

    // `slice_origin` is this render's top-left in absolute canvas pixels. The
    // sample rotation is keyed to it rather than to a node-local index, so a
    // node holding half the wall rotates the kernel identically to a node
    // holding all of it and the two agree through a crease on their seam.
    [[nodiscard]] bool record(
        rhi::CommandList& commands, const rhi::Texture& depth,
        const rhi::Texture& normal,
        const scene::Mat4& inverse_view_projection,
        scene::CanvasRect slice, core::u64 show_frame, float radius,
        float strength) noexcept;

    [[nodiscard]] const AmbientOcclusionStats& stats() const noexcept {
        return stats_;
    }
    void reset_stats() noexcept { stats_ = {}; }

private:
    AmbientOcclusion(rhi::Texture occlusion, rhi::ComputePipeline pipeline,
                     rhi::Sampler sampler) noexcept;

    rhi::Texture occlusion_;
    rhi::ComputePipeline pipeline_;
    rhi::Sampler sampler_;
    AmbientOcclusionStats stats_{};
};

} // namespace mediaserver::render
