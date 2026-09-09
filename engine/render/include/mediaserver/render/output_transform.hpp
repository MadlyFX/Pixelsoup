#pragma once
#include <mediaserver/render/frame_graph.hpp>

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/display_lut_cache.hpp>
#include <mediaserver/render/display_transform.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/scene.hpp>

// The output transform: a source texture in, one connector-ready colour target
// out.
//
// This is the GPU half of the OUTPUT stage (§1.2 of the architecture plan),
// the counterpart to what `SceneRenderer` is for the scene. The source is
// whatever produced the node's region - today the scene renderer's target, and
// later an ST 2110 receive - so the transform names no source in particular.
//
// It scales the source into an independently positioned viewport and leaves
// the rest of the fixed window raster black. On the scene-linear path it is
// also the colour boundary: the baked display transform composed with the
// authored show look, and an ordered dither to the output's own bit depth,
// applied last on the node that owns the output. Per-panel calibration stays
// downstream on the LED processor for this deployment.
namespace mediaserver::render {

class OutputTransform final {
    friend class ConformanceHarness;
public:
    // Fails with ShaderFormatUnsupported on a build with no output bytecode,
    // which is a normal outcome on a machine without a shader compiler.
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<OutputTransform, RenderError> create(
        rhi::Device& device,
        core::u32 width,
        core::u32 height,
        bool scene_linear_input = false,
        rhi::TextureFormat target_format = rhi::TextureFormat::Rgba8Unorm);

    OutputTransform(const OutputTransform&) = delete;
    OutputTransform& operator=(const OutputTransform&) = delete;
    OutputTransform(OutputTransform&&) = default;
    OutputTransform& operator=(OutputTransform&&) = default;
    ~OutputTransform() = default;

    // The colour target this transform draws into. Sampled and presentable; the
    // caller blits it to a swapchain or reads it back.
    [[nodiscard]] rhi::Texture& target() noexcept { return color_; }

    // Replaces the baked display transform. The table is uploaded on the next
    // composite rather than here: one arriving mid-recording would grade part
    // of a frame differently from the rest of it.
    //
    // The production seam is `adopt_color` below, which takes the table from
    // the process-shared residency worker; this is the seam a measured table
    // or a test hands one in through directly.
    void set_display_lut(DisplayLut lut);

    // Takes the project's authored colour for the frames that follow: the
    // dither switch immediately, and the baked table for the authored
    // transform and look once the residency worker has it. Called once per
    // frame before anything is recorded, so a table can never change between
    // two viewports of one window - and never waits, reads a file or bakes:
    // a table that is not ready yet leaves the resident one in place and
    // reports `color_state()` as Pending.
    void adopt_color(const scene::ColorSettings& color, core::u64 authored_generation = 0U) noexcept;
    // Call after adopt_color for every wall in the window, before recording.
    // Sixteen independent profiles are preallocated; exceeding the bound
    // refuses readiness. Disabled walls use the project transform.
    [[nodiscard]] bool adopt_wall_color(doc::NodeId wall,
        const scene::WallColorSettings& settings) noexcept;
    [[nodiscard]] const scene::ColorSettings& color() const noexcept {
        return color_settings_;
    }
    // What became of the authored colour, for the operator: Ready, still
    // baking, or a look file that could not be read. Ready until the first
    // `adopt_color`, because the compiled default is resident from creation.
    [[nodiscard]] DisplayLutState color_state() const noexcept {
        return color_state_;
    }
    [[nodiscard]] const DisplayLut& display_lut() const noexcept {
        return shared_lut_ ? *shared_lut_ : lut_;
    }
    // Integer targets quantise to 8/10 bits. Float targets report 16 and
    // carry linear Rec.709/scRGB, without integer quantisation or dither.
    [[nodiscard]] core::u32 output_bit_depth() const noexcept;

    [[nodiscard]] core::u32 width() const noexcept { return color_.width(); }
    [[nodiscard]] core::u32 height() const noexcept { return color_.height(); }

    // Reallocates the colour target. A no-op when the size is unchanged, so it
    // is safe to call every frame from a resizable window.
    [[nodiscard]] bool resize(
        rhi::Device& device,
        core::u32 width,
        core::u32 height) noexcept;

    // Records one output frame: samples `source` into the required `viewport`
    // inside the fixed-size target.
    //
    // `canvas_region` is the rectangle of the canvas - or of the surface
    // raster, for a mapped output - that the viewport shows, and it is what
    // keys the dither: the pattern is a function of the wall pixel rather than
    // of where this node's window happens to put it, so two nodes dither the
    // pixel they share identically. The overloads without it assume the
    // viewport shows the region from the origin, which is right for a preview
    // and for any output with nothing beside it.
    [[nodiscard]] bool render(
        rhi::CommandList& commands,
        const rhi::Texture& source,
        scene::CanvasRect viewport);
    [[nodiscard]] bool render(
        rhi::CommandList& commands,
        const rhi::Texture& source,
        scene::CanvasRect viewport,
        bool scene_linear_input);
    [[nodiscard]] bool render(
        rhi::CommandList& commands,
        const rhi::Texture& source,
        scene::CanvasRect viewport,
        bool scene_linear_input,
        scene::CanvasRect canvas_region);

    // Starts a multi-viewport frame with an opaque black window, then adds one
    // source without disturbing viewports already composited into the target.
    [[nodiscard]] bool clear(rhi::CommandList& commands) noexcept;
    [[nodiscard]] bool composite(
        rhi::CommandList& commands,
        const rhi::Texture& source,
        scene::CanvasRect viewport);
    [[nodiscard]] bool composite(
        rhi::CommandList& commands,
        const rhi::Texture& source,
        scene::CanvasRect viewport,
        bool scene_linear_input);
    [[nodiscard]] bool composite(
        rhi::CommandList& commands,
        const rhi::Texture& source,
        scene::CanvasRect viewport,
        bool scene_linear_input,
        scene::CanvasRect canvas_region, doc::NodeId wall = {});

private:
    FrameGraph graph_;
    OutputTransform(
        rhi::Texture color,
        rhi::GraphicsPipeline display_pipeline,
        rhi::GraphicsPipeline linear_pipeline,
        rhi::Sampler sampler,
        bool default_scene_linear) noexcept;

    rhi::Texture color_;
    rhi::GraphicsPipeline display_pipeline_;
    rhi::GraphicsPipeline linear_pipeline_;
    rhi::Sampler sampler_;
    // The baked transform, and the atlas the shader reads it from. Present
    // only on the scene-linear path: a display-referred source arrives already
    // transformed and passes straight through.
    rhi::Texture lut_texture_;
    DisplayLut lut_;
    bool lut_uploaded_{};
    rhi::CommandRecordingToken lut_recording_;
    bool default_scene_linear_{};
    // The authored colour and where its table stands. The cache handle is a
    // process-shared one, so every output on this machine grades from the
    // same bake.
    DisplayLutCache lut_cache_;
    scene::ColorSettings color_settings_{};
    DisplayLutState color_state_{DisplayLutState::Ready};
    core::u64 adopted_generation_{};
    core::u64 authored_generation_{};
    std::shared_ptr<const DisplayLut> shared_lut_;
    std::shared_ptr<const std::vector<core::u8>> atlas_;
    struct WallProfile final {
        doc::NodeId wall{};
        bool active{};
        core::u64 generation{};
        bool uploaded{};
        rhi::CommandRecordingToken recording;
        rhi::Texture texture;
        std::shared_ptr<const std::vector<core::u8>> atlas;
    };
    std::array<WallProfile, 16U> wall_profiles_;
};

} // namespace mediaserver::render
