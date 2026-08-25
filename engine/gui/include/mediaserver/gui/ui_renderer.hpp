#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>

#include <span>
#include <vector>

// The 2D draw foundation for the bespoke operator UI.
//
// This is the first piece of the UI stack that replaces the throwaway Dear ImGui
// shell (see the architecture plan's UI section): a retained-mode widget layer
// rendered on the app's own RHI, so the engine's 3D viewport textures stay
// zero-copy panels. This layer is only the drawing: a CPU draw list of coloured
// and textured quads, and a renderer that turns it into one pulled pass on the
// RHI. Layout, input, widgets, and docking build on top.
//
// One atlas serves both text and solid fills: a glyph's coverage is in the atlas
// alpha, and a solid fill samples a reserved white texel, so a single pipeline
// draws everything and there is one pass per frame. Clipping rides per batch and
// is applied in the fragment stage, because the RHI exposes no scissor.
namespace mediaserver::gui {

struct UiColor final {
    float r{};
    float g{};
    float b{};
    float a{1.0F};
};

// A point in the UI's pixel space, for the primitives that are not rectangles.
struct UiPoint final {
    float x{};
    float y{};
};

// A vertex as the UI shader reads it: position in pixels, atlas uv, straight
// colour. Matches ui.vert.hlsl's UiVertex exactly.
struct UiVertex final {
    float x{};
    float y{};
    float u{};
    float v{};
    float r{};
    float g{};
    float b{};
    float a{};
};

// A run of vertices sharing a clip rectangle and a texture, drawn as one batch.
// The clip rect is in pixels (min x, min y, max x, max y).
struct UiBatch final {
    core::u32 first_vertex{};
    core::u32 vertex_count{};
    float clip_min_x{};
    float clip_min_y{};
    float clip_max_x{};
    float clip_max_y{};
    // What this run samples, or null for the glyph atlas that carries the text
    // and the white texel. A non-null texture is an engine target composited
    // straight into a panel - the plan's "viewports are just textures in the
    // widget tree, with zero copies" - and costs one more pass, not a copy.
    const rhi::Texture* texture{};
};

// A frame's worth of 2D drawing, accumulated on the CPU then handed to the
// renderer. Coordinates are pixels with the origin at the top left. Quads are
// emitted as two triangles (six vertices); there is no index buffer, matching
// the pulled shader.
class UiDrawList final {
public:
    // The atlas uv of the reserved opaque white texel, so solid fills sample it.
    // Set once from the font atlas before drawing.
    void set_white_texel(float u, float v) noexcept;

    void reset() noexcept;

    // Clip rectangles nest: a pushed rect is intersected with the one below, so a
    // child never draws outside its parent. Every quad is clipped to the top.
    void push_clip(float x, float y, float width, float height);
    void pop_clip();

    // A solid rectangle in the current colour, sampling the white texel.
    void add_rect(float x, float y, float width, float height, UiColor color);

    // A rectangle textured from an atlas region (a glyph, or any atlas sprite),
    // tinted by `color`. uv is in [0,1] atlas space.
    void add_textured(
        float x,
        float y,
        float width,
        float height,
        float u0,
        float v0,
        float u1,
        float v1,
        UiColor color);

    // A filled triangle in the current colour. The three primitives below are
    // what the operator surfaces need that a rectangle cannot express: a
    // keyframe diamond, a twirl arrow, a gizmo axis running off at an angle.
    // They cost nothing extra - a solid fill already samples the white texel,
    // so they batch with every other solid quad and clip per fragment like one.
    void add_triangle(
        float x0,
        float y0,
        float x1,
        float y1,
        float x2,
        float y2,
        UiColor color);

    // A convex polygon, fan-triangulated from its first point. Fewer than three
    // points draw nothing; a concave outline will self-overlap rather than be
    // rejected, which is the same bargain every immediate-mode list makes.
    void add_convex_poly(std::span<const UiPoint> points, UiColor color);

    // A line segment `thickness` pixels wide, with square ends. Diagonal lines
    // are not anti-aliased: at the one- and two-pixel widths the UI draws them
    // at, coverage blending reads as a smear rather than a smooth edge.
    void add_line(
        float x0,
        float y0,
        float x1,
        float y1,
        float thickness,
        UiColor color);

    // A rectangle sampling an arbitrary texture rather than the atlas - an
    // engine render target dropped into a panel. `tint` multiplies it, so white
    // shows it untouched. The texture is borrowed and must outlive the frame
    // this list is rendered in.
    void add_image(
        float x,
        float y,
        float width,
        float height,
        const rhi::Texture& texture,
        UiColor tint = UiColor{1.0F, 1.0F, 1.0F, 1.0F});

    [[nodiscard]] const std::vector<UiVertex>& vertices() const noexcept {
        return vertices_;
    }
    [[nodiscard]] const std::vector<UiBatch>& batches() const noexcept {
        return batches_;
    }

private:
    struct Clip final {
        float min_x{};
        float min_y{};
        float max_x{};
        float max_y{};
    };

    void quad(
        float x,
        float y,
        float width,
        float height,
        float u0,
        float v0,
        float u1,
        float v1,
        UiColor color,
        const rhi::Texture* texture);
    // Opens (or continues) the batch this run belongs to. Returns false when the
    // current clip rectangle is empty, in which case the caller emits nothing.
    [[nodiscard]] bool begin_run(const rhi::Texture* texture);
    // One vertex sampling the white texel, counted into the open batch. Only
    // valid straight after a begin_run() that returned true.
    void push_solid(float x, float y, UiColor color);
    [[nodiscard]] Clip current_clip() const noexcept;

    float white_u_{};
    float white_v_{};
    std::vector<UiVertex> vertices_;
    std::vector<UiBatch> batches_;
    std::vector<Clip> clip_stack_;
};

// Renders a UiDrawList onto a colour target through the RHI. Owns the pipeline,
// the vertex arena, the glyph/sprite atlas, and a depth target the pulled draw
// requires but the UI never uses.
class UiRenderer final {
public:
    [[nodiscard]] static bool shaders_available() noexcept;

    [[nodiscard]] static core::Result<UiRenderer, render::RenderError> create(
        rhi::Device& device, rhi::TextureFormat target_format);

    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;
    UiRenderer(UiRenderer&&) = default;
    UiRenderer& operator=(UiRenderer&&) = default;
    ~UiRenderer() = default;

    // Uploads the atlas pixels (RGBA8, rgb = 255 with coverage in alpha, plus a
    // white texel) once. Call before rendering; re-call only if the atlas grows.
    [[nodiscard]] bool upload_atlas(
        rhi::Device& device,
        rhi::CommandList& commands,
        std::span<const core::u8> rgba,
        core::u32 width,
        core::u32 height);

    // Records one UI pass into `target`, clearing it to `clear` first. `target`
    // must be a colour target sized `width` x `height`.
    [[nodiscard]] bool render(
        rhi::Device& device,
        rhi::CommandList& commands,
        rhi::Texture& target,
        rhi::Color clear,
        const UiDrawList& draw_list,
        core::u32 width,
        core::u32 height);

private:
    UiRenderer(
        rhi::GraphicsPipeline pipeline,
        rhi::Sampler sampler,
        rhi::StorageBuffer vertices) noexcept;

    rhi::GraphicsPipeline pipeline_;
    rhi::Sampler sampler_;
    rhi::StorageBuffer vertex_buffer_;
    rhi::Texture atlas_;
    rhi::Texture depth_;
    std::vector<core::u8> vertex_scratch_;
    std::vector<core::u8> batch_scratch_;
    std::vector<rhi::DrawBatch> pass_batches_;
    core::u32 depth_width_{};
    core::u32 depth_height_{};
};

} // namespace mediaserver::gui
