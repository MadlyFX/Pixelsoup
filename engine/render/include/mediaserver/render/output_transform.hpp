#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>

// The output transform: a source texture in, one connector-ready colour target
// out.
//
// This is the GPU half of the OUTPUT stage (§1.2 of the architecture plan),
// the counterpart to what `SceneRenderer` is for the scene. The source is
// whatever produced the node's region - today the scene renderer's target, and
// later an ST 2110 receive - so the transform names no source in particular.
//
// Today it is a straight sample: this product drives LED walls, where adjacent
// nodes abut pixel-exact and there is no overlap to blend. The pass exists as
// the seam where the LED colour output lands next - the per-panel/per-wall
// calibration LUT and dither before 10/12-bit quantisation, applied last on the
// node that owns the output. The fullscreen-triangle draw is shaped to grow
// into those without a new draw path.
namespace mediaserver::render {

class OutputTransform final {
public:
    // Fails with ShaderFormatUnsupported on a build with no output bytecode,
    // which is a normal outcome on a machine without a shader compiler.
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<OutputTransform, RenderError> create(
        rhi::Device& device,
        core::u32 width,
        core::u32 height);

    OutputTransform(const OutputTransform&) = delete;
    OutputTransform& operator=(const OutputTransform&) = delete;
    OutputTransform(OutputTransform&&) = default;
    OutputTransform& operator=(OutputTransform&&) = default;
    ~OutputTransform() = default;

    // The colour target this transform draws into. Sampled and presentable; the
    // caller blits it to a swapchain or reads it back.
    [[nodiscard]] rhi::Texture& target() noexcept { return color_; }
    [[nodiscard]] core::u32 width() const noexcept { return color_.width(); }
    [[nodiscard]] core::u32 height() const noexcept { return color_.height(); }

    // Reallocates the colour target. A no-op when the size is unchanged, so it
    // is safe to call every frame from a resizable window.
    [[nodiscard]] bool resize(
        rhi::Device& device,
        core::u32 width,
        core::u32 height) noexcept;

    // Records one output frame: samples `source` - the node's rendered region -
    // onto `target()`. A straight copy today; the calibration LUT and dither
    // land here.
    [[nodiscard]] bool render(
        rhi::CommandList& commands,
        const rhi::Texture& source);

private:
    OutputTransform(
        rhi::Texture color,
        rhi::GraphicsPipeline pipeline,
        rhi::Sampler sampler) noexcept;

    rhi::Texture color_;
    rhi::GraphicsPipeline pipeline_;
    rhi::Sampler sampler_;
};

} // namespace mediaserver::render
