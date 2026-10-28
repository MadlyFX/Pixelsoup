#pragma once
#include <mediaserver/render/frame_graph.hpp>

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>

// The Asset Viewer's inspection pass over a finished display image: one
// channel or luma as grey, and a display-only exposure offset. It reads the
// display transform's output and writes a same-size RGBA8 image for the
// panel. Nothing an output node draws passes through it.
namespace mediaserver::render {

enum class InspectionChannel : core::u8 {
    Rgb,
    Red,
    Green,
    Blue,
    Luma,
};

class InspectionView final {
public:
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<InspectionView, RenderError>
    create(rhi::Device& device, core::u32 width, core::u32 height);

    InspectionView(const InspectionView&) = delete;
    InspectionView& operator=(const InspectionView&) = delete;
    InspectionView(InspectionView&&) = default;
    InspectionView& operator=(InspectionView&&) = default;
    ~InspectionView() = default;

    [[nodiscard]] rhi::Texture& target() noexcept { return color_; }
    [[nodiscard]] const rhi::Texture& target() const noexcept { return color_; }

    [[nodiscard]] bool resize(rhi::Device& device, core::u32 width,
                              core::u32 height) noexcept;

    // `stops` is clamped to [-16, 16]; zero leaves the values as they are.
    [[nodiscard]] bool render(rhi::CommandList& commands,
                              const rhi::Texture& image,
                              InspectionChannel channel,
                              float stops) noexcept;

private:
    FrameGraph graph_;
    InspectionView(rhi::Texture color, rhi::GraphicsPipeline pipeline,
                   rhi::Sampler sampler) noexcept;

    rhi::Texture color_;
    rhi::GraphicsPipeline pipeline_;
    rhi::Sampler sampler_;
};

} // namespace mediaserver::render
