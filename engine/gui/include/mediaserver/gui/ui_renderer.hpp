#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/render/render_error.hpp>
#include <mediaserver/rhi/device.hpp>

#include <array>
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
    // Sample `texture` with nearest filtering: pixel art drawn larger than
    // its own pixels stays crisp instead of blurring between them.
    bool nearest{};
};

// A frame's worth of 2D drawing, accumulated on the CPU then handed to the
// renderer. Callers draw in logical units with the origin at the top left; the
// list multiplies by its scale as it emits, so the vertices and clip rects it
// holds are physical pixels. Quads are emitted as two triangles (six vertices);
// there is no index buffer, matching the pulled shader.
//
// Axis-aligned quads land on whole pixels: each edge is rounded on its own, so
// two panels that share an edge in logical units still share it on screen, and
// anything thinner than a few pixels keeps one exact width wherever it sits.
// Without that a one-unit border at 150% would come out one pixel here and two
// there, which reads as noise rather than as a line.
class UiDrawList final {
public:
    // The atlas uv of the reserved opaque white texel, so solid fills sample it.
    // Set once from the font atlas before drawing.
    void set_white_texel(float u, float v) noexcept;

    // Physical pixels per logical unit. One by default, which leaves every
    // coordinate exactly as given. Survives reset(): it is the display's
    // property, not the frame's.
    void set_scale(float scale) noexcept;
    [[nodiscard]] float scale() const noexcept { return scale_; }

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

    // An open stroke through `points`, `thickness` units wide, with mitred
    // joins and an anti-aliased edge: a one-pixel fringe fades to transparent
    // either side. This is for curves - a graph wire tessellated into short
    // segments - where add_line's hard diagonal edges would show every step.
    // Fewer than two points draw nothing; repeated points are skipped.
    void add_polyline(
        std::span<const UiPoint> points, float thickness, UiColor color);

    // A rectangle with rounded corners, filled. The edges land on whole
    // pixels as add_rect's do; the corners are arcs with a one-pixel
    // anti-aliased fringe, so a pill or a card reads as soft without blurring
    // its straight sides. `radius` is clamped to half the shorter side, and
    // `corners` picks which corners round (the others stay square). A radius
    // under a pixel draws an ordinary rectangle.
    void add_rounded_rect(
        float x,
        float y,
        float width,
        float height,
        float radius,
        UiColor color,
        core::u8 corners = corner_all);
    // The same outline as a stroke `thickness` units wide, inside the edge.
    void add_rounded_outline(
        float x,
        float y,
        float width,
        float height,
        float radius,
        float thickness,
        UiColor color,
        core::u8 corners = corner_all);
    // A soft halo `spread` units deep outside the same outline, fading from
    // `color` at the edge to nothing: how a live control lights up.
    void add_rounded_glow(
        float x,
        float y,
        float width,
        float height,
        float radius,
        float spread,
        UiColor color);
    // Paints the outside of each rounded corner - the sliver between the
    // box's square corner and its arc - in `color`. Drawn over square content
    // in the colour behind the box, it rounds that content's corners without
    // a rounded clip: how a docked panel keeps soft corners whatever its
    // contents paint edge to edge.
    void add_corner_mask(
        float x,
        float y,
        float width,
        float height,
        float radius,
        UiColor color,
        core::u8 corners = corner_all);
    // A rectangle whose four corners carry their own colours, blended across
    // it: a colour picker's fields and rails, a fade.
    void add_gradient_rect(
        float x,
        float y,
        float width,
        float height,
        UiColor top_left,
        UiColor top_right,
        UiColor bottom_right,
        UiColor bottom_left);

    // Which corners of a rounded rectangle round.
    static constexpr core::u8 corner_top_left = 1U;
    static constexpr core::u8 corner_top_right = 2U;
    static constexpr core::u8 corner_bottom_right = 4U;
    static constexpr core::u8 corner_bottom_left = 8U;
    static constexpr core::u8 corner_top = corner_top_left | corner_top_right;
    static constexpr core::u8 corner_bottom =
        corner_bottom_left | corner_bottom_right;
    static constexpr core::u8 corner_left = corner_top_left | corner_bottom_left;
    static constexpr core::u8 corner_right =
        corner_top_right | corner_bottom_right;
    static constexpr core::u8 corner_all = 15U;

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

    // The same borrowed image path with an explicit normalized source region.
    // This is used by filmstrip atlases so Timeline clips can show only the
    // source interval they reference without allocating per-clip textures.
    void add_image_region(
        float x,
        float y,
        float width,
        float height,
        const rhi::Texture& texture,
        float u0,
        float v0,
        float u1,
        float v1,
        UiColor tint = UiColor{1.0F, 1.0F, 1.0F, 1.0F});

    // The same region drawn with nearest filtering, so each texel lands as a
    // hard-edged block - pixel art at any zoom. The texture is borrowed as
    // for add_image.
    void add_sprite(
        float x,
        float y,
        float width,
        float height,
        const rhi::Texture& texture,
        float u0,
        float v0,
        float u1,
        float v1,
        UiColor tint = UiColor{1.0F, 1.0F, 1.0F, 1.0F});

    // Scales everything drawn since vertex `first` about (centre_x,
    // centre_y) - logical units - by `factor`. A whole compound mark grows
    // or shrinks as one, text included, without redrawing it: how a dragged
    // node lifts and settles. Clip rectangles stay where they were.
    void scale_since(std::size_t first, float centre_x, float centre_y,
                     float factor) noexcept;

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
        const rhi::Texture* texture,
        bool nearest = false);
    // Opens (or continues) the batch this run belongs to. Returns false when the
    // current clip rectangle is empty, in which case the caller emits nothing.
    [[nodiscard]] bool begin_run(const rhi::Texture* texture,
                                 bool nearest = false);
    // One vertex sampling the white texel, counted into the open batch. Only
    // valid straight after a begin_run() that returned true.
    void push_solid(float x, float y, UiColor color);
    // The same, already in physical pixels.
    void push_pixel(float x, float y, UiColor color);
    // Four edges `thickness` deep inside the box: the square outline a
    // rounded one falls back to below a pixel of radius.
    void add_outline_box(float x, float y, float width, float height,
                         float thickness, UiColor color);
    // The outline of a rounded rectangle in physical pixels: the snapped box,
    // each corner's arc centre and radius, and how finely each arc is cut.
    struct RoundedOutline final {
        float x0{};
        float y0{};
        float x1{};
        float y1{};
        std::array<float, 4U> radius{};
        core::u32 segments{};
    };
    [[nodiscard]] bool rounded_outline(
        float x,
        float y,
        float width,
        float height,
        float radius,
        core::u8 corners,
        RoundedOutline& out) const noexcept;
    // Walks the outline clockwise from the top-left corner, calling `visit`
    // with each point offset `offset` pixels outward along its normal.
    template <typename Visit>
    void walk_outline(const RoundedOutline& outline, float offset,
                      Visit&& visit) const;
    [[nodiscard]] Clip current_clip() const noexcept;
    // One logical span [start, start + extent) onto the pixel grid.
    void snap_span(float start, float extent, float& from, float& to) const noexcept;

    float scale_{1.0F};
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
        rhi::Sampler nearest_sampler,
        rhi::StorageBuffer vertices) noexcept;

    rhi::GraphicsPipeline pipeline_;
    rhi::Sampler sampler_;
    rhi::Sampler nearest_sampler_;
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
