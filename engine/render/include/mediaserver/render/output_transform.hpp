#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>

// The output transform: a source texture in, one connector-ready colour target
// out.
//
// This is the GPU half of the OUTPUT stage (§1.2 of the architecture plan),
// the counterpart to what `SceneRenderer` is for the scene. The source is
// whatever produced the node's region - today the scene renderer's target, and
// later an ST 2110 receive - so the transform names no source in particular.
//
// It scales the source into an independently positioned viewport and leaves
// the rest of the fixed window raster black. The pass is also the seam where
// LED colour output lands next - the per-panel/per-wall calibration LUT and
// dither before 10/12-bit quantisation, applied last on the node that owns the
// output.
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

    // Records one output frame: samples `source` into the required `viewport`
    // inside the fixed-size target. Calibration LUT and dither land here.
    [[nodiscard]] bool render(
        rhi::CommandList& commands,
        const rhi::Texture& source,
        scene::CanvasRect viewport);

    // Starts a multi-viewport frame with an opaque black window, then adds one
    // source without disturbing viewports already composited into the target.
    [[nodiscard]] bool clear(rhi::CommandList& commands) noexcept;
    [[nodiscard]] bool composite(
        rhi::CommandList& commands,
        const rhi::Texture& source,
        scene::CanvasRect viewport);

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
