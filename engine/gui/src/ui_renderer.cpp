#include <mediaserver/gui/ui_renderer.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/ui_frag_dxil.h>
#include <mediaserver/shaders/ui_vert_dxil.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>

namespace mediaserver::gui {
namespace {

namespace rhi = mediaserver::rhi;
using render::RenderError;

// Ample for a dense operator screen; a frame past this is refused rather than
// grown on the draw path.
constexpr core::u32 max_ui_vertices = 1U << 18U;

// Matches ui.vert.hlsl's ViewConstants (b0) and BatchConstants (b1).
struct ViewConstants final {
    float target_width{};
    float target_height{};
    float pad0{};
    float pad1{};
};
struct BatchConstants final {
    core::u32 first_vertex{};
    core::u32 pad_a{};
    core::u32 pad_b{};
    core::u32 pad_c{};
    float clip_min_x{};
    float clip_min_y{};
    float clip_max_x{};
    float clip_max_y{};
};

template <typename T>
void append(std::vector<core::u8>& out, const T& value) {
    const auto* const bytes = reinterpret_cast<const core::u8*>(&value);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

} // namespace

// ---------------------------------------------------------------------------
// UiDrawList

void UiDrawList::set_white_texel(float u, float v) noexcept {
    white_u_ = u;
    white_v_ = v;
}

void UiDrawList::set_scale(float scale) noexcept {
    scale_ = std::isfinite(scale) && scale > 0.0F ? scale : 1.0F;
}

void UiDrawList::snap_span(
    float start, float extent, float& from, float& to) const noexcept {
    const float physical = extent * scale_;
    from = std::round(start * scale_);
    if (!(physical > 0.0F)) {
        // Empty (or inverted) stays empty: a zero-width rule must not grow a
        // pixel just because thin spans are given at least one.
        to = from;
        return;
    }
    // Thin spans keep one width wherever they fall; wider ones round both
    // edges so neighbours meet without a seam or an overlap.
    to = physical < 2.5F ? from + std::max(1.0F, std::round(physical))
                         : std::round((start + extent) * scale_);
}

void UiDrawList::reset() noexcept {
    vertices_.clear();
    batches_.clear();
    clip_stack_.clear();
}

void UiDrawList::push_clip(float x, float y, float width, float height) {
    // Clips snap exactly as rectangles do, so a panel and the clip of its own
    // bounds cover the same pixels.
    Clip clip{};
    snap_span(x, std::max(width, 0.0F), clip.min_x, clip.max_x);
    snap_span(y, std::max(height, 0.0F), clip.min_y, clip.max_y);
    if (!clip_stack_.empty()) {
        const Clip& parent = clip_stack_.back();
        clip.min_x = std::max(clip.min_x, parent.min_x);
        clip.min_y = std::max(clip.min_y, parent.min_y);
        clip.max_x = std::min(clip.max_x, parent.max_x);
        clip.max_y = std::min(clip.max_y, parent.max_y);
    }
    clip_stack_.push_back(clip);
}

void UiDrawList::pop_clip() {
    if (!clip_stack_.empty()) {
        clip_stack_.pop_back();
    }
}

UiDrawList::Clip UiDrawList::current_clip() const noexcept {
    if (!clip_stack_.empty()) {
        return clip_stack_.back();
    }
    return Clip{-1.0e6F, -1.0e6F, 1.0e6F, 1.0e6F};
}

void UiDrawList::add_rect(
    float x, float y, float width, float height, UiColor color) {
    quad(x, y, width, height, white_u_, white_v_, white_u_, white_v_, color, nullptr);
}

void UiDrawList::add_textured(
    float x,
    float y,
    float width,
    float height,
    float u0,
    float v0,
    float u1,
    float v1,
    UiColor color) {
    quad(x, y, width, height, u0, v0, u1, v1, color, nullptr);
}

void UiDrawList::add_image(
    float x,
    float y,
    float width,
    float height,
    const rhi::Texture& texture,
    UiColor tint) {
    // The whole texture across the rectangle. Its own uv runs top-left down,
    // the same way the UI's pixels do, so no flip is needed.
    quad(x, y, width, height, 0.0F, 0.0F, 1.0F, 1.0F, tint, &texture);
}

void UiDrawList::add_image_region(
    float x, float y, float width, float height,
    const rhi::Texture& texture, float u0, float v0, float u1, float v1,
    UiColor tint) {
    quad(x, y, width, height, u0, v0, u1, v1, tint, &texture);
}

void UiDrawList::add_sprite(
    float x, float y, float width, float height,
    const rhi::Texture& texture, float u0, float v0, float u1, float v1,
    UiColor tint) {
    quad(x, y, width, height, u0, v0, u1, v1, tint, &texture, true);
}

void UiDrawList::add_triangle(
    float x0,
    float y0,
    float x1,
    float y1,
    float x2,
    float y2,
    UiColor color) {
    if (!begin_run(nullptr)) {
        return;
    }
    push_solid(x0, y0, color);
    push_solid(x1, y1, color);
    push_solid(x2, y2, color);
}

void UiDrawList::add_convex_poly(
    std::span<const UiPoint> points, UiColor color) {
    if (points.size() < 3U) {
        return;
    }
    if (!begin_run(nullptr)) {
        return;
    }
    // A fan from the first point: for a convex outline every triangle lies
    // inside it, so the fill is exact and no triangle is drawn twice.
    for (std::size_t i = 1U; i + 1U < points.size(); ++i) {
        push_solid(points[0U].x, points[0U].y, color);
        push_solid(points[i].x, points[i].y, color);
        push_solid(points[i + 1U].x, points[i + 1U].y, color);
    }
}

void UiDrawList::add_line(
    float x0,
    float y0,
    float x1,
    float y1,
    float thickness,
    UiColor color) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length <= 0.0F || thickness <= 0.0F) {
        return;
    }
    // The segment's normal, scaled to half the width: the quad is the segment
    // pushed out either side of its own centre line. A one-unit stroke never
    // thins below one physical pixel, which is where it would start to break up.
    const float half = std::max(thickness * scale_, 1.0F) * 0.5F / scale_;
    const float nx = (-dy / length) * half;
    const float ny = (dx / length) * half;

    if (!begin_run(nullptr)) {
        return;
    }
    push_solid(x0 + nx, y0 + ny, color);
    push_solid(x1 + nx, y1 + ny, color);
    push_solid(x1 - nx, y1 - ny, color);
    push_solid(x0 + nx, y0 + ny, color);
    push_solid(x1 - nx, y1 - ny, color);
    push_solid(x0 - nx, y0 - ny, color);
}

void UiDrawList::add_polyline(
    std::span<const UiPoint> input, float thickness, UiColor color) {
    if (input.size() < 2U || thickness <= 0.0F) {
        return;
    }
    // Drop repeated points, which have no direction to offset along.
    std::vector<UiPoint> points;
    points.reserve(input.size());
    for (const UiPoint& point : input) {
        if (points.empty() || point.x != points.back().x ||
            point.y != points.back().y) {
            points.push_back(point);
        }
    }
    if (points.size() < 2U) {
        return;
    }

    // The solid core never thins below one physical pixel, like add_line; the
    // fringe is one physical pixel more on each side, fading out.
    const float half = std::max(thickness * scale_, 1.0F) * 0.5F / scale_;
    const float fringe = 1.0F / scale_;

    // One offset direction per point. An interior point takes the mitre of
    // its two segments, so neighbouring quads meet without a gap or overlap;
    // the mitre is capped where a sharp turn would spike far past the stroke.
    std::vector<UiPoint> normals(points.size());
    const auto segment_normal = [&points](std::size_t from) noexcept {
        const float dx = points[from + 1U].x - points[from].x;
        const float dy = points[from + 1U].y - points[from].y;
        const float length = std::sqrt(dx * dx + dy * dy);
        return UiPoint{-dy / length, dx / length};
    };
    normals.front() = segment_normal(0U);
    normals.back() = segment_normal(points.size() - 2U);
    for (std::size_t i = 1U; i + 1U < points.size(); ++i) {
        const UiPoint before = segment_normal(i - 1U);
        const UiPoint after = segment_normal(i);
        UiPoint mitre{before.x + after.x, before.y + after.y};
        const float length = std::sqrt(mitre.x * mitre.x + mitre.y * mitre.y);
        if (length < 1.0e-4F) {
            normals[i] = after; // a full reversal: no sensible mitre
            continue;
        }
        mitre.x /= length;
        mitre.y /= length;
        // 1 / cos(half the turn), the mitre's stretch, limited to 2.
        const float cosine = mitre.x * after.x + mitre.y * after.y;
        const float stretch = 1.0F / std::max(cosine, 0.5F);
        normals[i] = UiPoint{mitre.x * stretch, mitre.y * stretch};
    }

    if (!begin_run(nullptr)) {
        return;
    }
    const UiColor clear{color.r, color.g, color.b, 0.0F};
    // A strip between offsets `inner` and `outer` (signed distances from the
    // centre line) along one segment, shaded from the inner colour outward.
    const auto band = [&](std::size_t i, float inner, float outer,
                          UiColor inner_color, UiColor outer_color) {
        const UiPoint& a = points[i];
        const UiPoint& b = points[i + 1U];
        const UiPoint& na = normals[i];
        const UiPoint& nb = normals[i + 1U];
        const UiPoint a_in{a.x + na.x * inner, a.y + na.y * inner};
        const UiPoint a_out{a.x + na.x * outer, a.y + na.y * outer};
        const UiPoint b_in{b.x + nb.x * inner, b.y + nb.y * inner};
        const UiPoint b_out{b.x + nb.x * outer, b.y + nb.y * outer};
        push_solid(a_in.x, a_in.y, inner_color);
        push_solid(b_in.x, b_in.y, inner_color);
        push_solid(b_out.x, b_out.y, outer_color);
        push_solid(a_in.x, a_in.y, inner_color);
        push_solid(b_out.x, b_out.y, outer_color);
        push_solid(a_out.x, a_out.y, outer_color);
    };
    for (std::size_t i = 0U; i + 1U < points.size(); ++i) {
        band(i, -half, half, color, color);
        band(i, half, half + fringe, color, clear);
        band(i, -half, -half - fringe, color, clear);
    }
}

bool UiDrawList::rounded_outline(
    float x,
    float y,
    float width,
    float height,
    float radius,
    core::u8 corners,
    RoundedOutline& out) const noexcept {
    snap_span(x, width, out.x0, out.x1);
    snap_span(y, height, out.y0, out.y1);
    if (out.x1 <= out.x0 || out.y1 <= out.y0) {
        return false;
    }
    const float limit = std::min(out.x1 - out.x0, out.y1 - out.y0) * 0.5F;
    const float pixels =
        std::clamp(std::isfinite(radius) ? radius * scale_ : 0.0F, 0.0F, limit);
    for (std::size_t corner = 0U; corner < 4U; ++corner) {
        out.radius[corner] =
            (corners & (1U << corner)) != 0U ? pixels : 0.0F;
    }
    // Cut finely enough that no chord strays a quarter pixel from the arc:
    // two pieces for a small chip's corner, up to ten for a large pill.
    out.segments = static_cast<core::u32>(
        std::clamp(std::ceil(std::sqrt(pixels) * 1.3F), 2.0F, 10.0F));
    return pixels >= 0.75F;
}

template <typename Visit>
void UiDrawList::walk_outline(
    const RoundedOutline& outline, float offset, Visit&& visit) const {
    constexpr float quarter = 1.57079632679F;
    // Top-left, top-right, bottom-right, bottom-left: each corner's arc
    // starts where the previous edge arrives, turning clockwise on screen.
    const std::array<float, 4U> start{
        2.0F * quarter, 3.0F * quarter, 0.0F, quarter};
    const std::array<UiPoint, 4U> centre{
        UiPoint{outline.x0 + outline.radius[0], outline.y0 + outline.radius[0]},
        UiPoint{outline.x1 - outline.radius[1], outline.y0 + outline.radius[1]},
        UiPoint{outline.x1 - outline.radius[2], outline.y1 - outline.radius[2]},
        UiPoint{outline.x0 + outline.radius[3], outline.y1 - outline.radius[3]}};
    for (std::size_t corner = 0U; corner < 4U; ++corner) {
        const float reach = std::max(outline.radius[corner] + offset, 0.0F);
        // A square corner keeps its two end points: a zero-length arc whose
        // normals still fan the fringe around the point.
        const core::u32 steps =
            outline.radius[corner] > 0.0F ? outline.segments : 1U;
        for (core::u32 step = 0U; step <= steps; ++step) {
            const float angle = start[corner] +
                quarter * static_cast<float>(step) / static_cast<float>(steps);
            const float nx = std::cos(angle);
            const float ny = std::sin(angle);
            if (outline.radius[corner] > 0.0F) {
                visit(centre[corner].x + nx * reach,
                      centre[corner].y + ny * reach);
            } else {
                // Push a square corner out along the diagonal so the offset
                // outline stays a rectangle `offset` from this one.
                const float sx = corner == 0U || corner == 3U ? -1.0F : 1.0F;
                const float sy = corner < 2U ? -1.0F : 1.0F;
                visit(centre[corner].x + sx * offset,
                      centre[corner].y + sy * offset);
            }
        }
    }
}

void UiDrawList::add_rounded_rect(
    float x,
    float y,
    float width,
    float height,
    float radius,
    UiColor color,
    core::u8 corners) {
    RoundedOutline outline{};
    if (!rounded_outline(x, y, width, height, radius, corners, outline)) {
        add_rect(x, y, width, height, color);
        return;
    }
    if (!begin_run(nullptr)) {
        return;
    }
    // The fill stops half a pixel inside the edge and a one-pixel fringe
    // fades across it, so a straight edge on the pixel grid stays crisp while
    // the arcs come out smooth.
    constexpr std::size_t max_points = 4U * 11U;
    std::array<UiPoint, max_points> inner{};
    std::array<UiPoint, max_points> outer{};
    std::size_t count = 0U;
    walk_outline(outline, -0.5F, [&](float px, float py) {
        inner[count++] = UiPoint{px, py};
    });
    count = 0U;
    walk_outline(outline, 0.5F, [&](float px, float py) {
        outer[count++] = UiPoint{px, py};
    });
    const UiColor clear{color.r, color.g, color.b, 0.0F};
    for (std::size_t i = 1U; i + 1U < count; ++i) {
        push_pixel(inner[0U].x, inner[0U].y, color);
        push_pixel(inner[i].x, inner[i].y, color);
        push_pixel(inner[i + 1U].x, inner[i + 1U].y, color);
    }
    for (std::size_t i = 0U; i < count; ++i) {
        const std::size_t next = (i + 1U) % count;
        push_pixel(inner[i].x, inner[i].y, color);
        push_pixel(inner[next].x, inner[next].y, color);
        push_pixel(outer[next].x, outer[next].y, clear);
        push_pixel(inner[i].x, inner[i].y, color);
        push_pixel(outer[next].x, outer[next].y, clear);
        push_pixel(outer[i].x, outer[i].y, clear);
    }
}

void UiDrawList::add_rounded_outline(
    float x,
    float y,
    float width,
    float height,
    float radius,
    float thickness,
    UiColor color,
    core::u8 corners) {
    if (thickness <= 0.0F) {
        return;
    }
    RoundedOutline outline{};
    const bool rounded =
        rounded_outline(x, y, width, height, radius, corners, outline);
    if (outline.x1 <= outline.x0 || outline.y1 <= outline.y0) {
        return;
    }
    if (!rounded) {
        add_outline_box(x, y, width, height, thickness, color);
        return;
    }
    if (!begin_run(nullptr)) {
        return;
    }
    const float stroke = std::max(thickness * scale_, 1.0F);
    // Four rings of points: the outer fringe, the stroke's two edges, and the
    // inner fringe, each a band of quads between neighbouring rings.
    constexpr std::size_t max_points = 4U * 11U;
    std::array<std::array<UiPoint, max_points>, 4U> rings{};
    const std::array<float, 4U> offsets{
        0.5F, -0.5F, std::min(-stroke + 0.5F, -0.5F), -stroke - 0.5F};
    std::size_t count = 0U;
    for (std::size_t ring = 0U; ring < rings.size(); ++ring) {
        count = 0U;
        walk_outline(outline, offsets[ring], [&](float px, float py) {
            rings[ring][count++] = UiPoint{px, py};
        });
    }
    const UiColor clear{color.r, color.g, color.b, 0.0F};
    const std::array<UiColor, 4U> shade{clear, color, color, clear};
    for (std::size_t band = 0U; band + 1U < rings.size(); ++band) {
        const auto& a = rings[band];
        const auto& b = rings[band + 1U];
        for (std::size_t i = 0U; i < count; ++i) {
            const std::size_t next = (i + 1U) % count;
            push_pixel(a[i].x, a[i].y, shade[band]);
            push_pixel(a[next].x, a[next].y, shade[band]);
            push_pixel(b[next].x, b[next].y, shade[band + 1U]);
            push_pixel(a[i].x, a[i].y, shade[band]);
            push_pixel(b[next].x, b[next].y, shade[band + 1U]);
            push_pixel(b[i].x, b[i].y, shade[band + 1U]);
        }
    }
}

void UiDrawList::add_rounded_glow(
    float x,
    float y,
    float width,
    float height,
    float radius,
    float spread,
    UiColor color) {
    if (spread <= 0.0F || color.a <= 0.0F) {
        return;
    }
    RoundedOutline outline{};
    static_cast<void>(rounded_outline(
        x, y, width, height, radius, corner_all, outline));
    if (outline.x1 <= outline.x0 || outline.y1 <= outline.y0) {
        return;
    }
    // A glow wants round corners even on a square box, or its corners read
    // as spikes; give each at least a few pixels of arc.
    for (float& corner : outline.radius) {
        corner = std::max(corner, std::min(3.0F * scale_,
            std::min(outline.x1 - outline.x0, outline.y1 - outline.y0) * 0.5F));
    }
    outline.segments = std::max(outline.segments, 4U);
    if (!begin_run(nullptr)) {
        return;
    }
    const float pixels = spread * scale_;
    // Three rings fall off roughly like a blur: bright at the edge, most of
    // the light gone a third of the way out, nothing at the rim.
    constexpr std::size_t max_points = 4U * 11U;
    std::array<std::array<UiPoint, max_points>, 3U> rings{};
    const std::array<float, 3U> offsets{0.0F, pixels * 0.35F, pixels};
    const std::array<UiColor, 3U> shade{
        color, UiColor{color.r, color.g, color.b, color.a * 0.4F},
        UiColor{color.r, color.g, color.b, 0.0F}};
    std::size_t count = 0U;
    for (std::size_t ring = 0U; ring < rings.size(); ++ring) {
        count = 0U;
        walk_outline(outline, offsets[ring], [&](float px, float py) {
            rings[ring][count++] = UiPoint{px, py};
        });
    }
    for (std::size_t band = 0U; band + 1U < rings.size(); ++band) {
        const auto& a = rings[band];
        const auto& b = rings[band + 1U];
        for (std::size_t i = 0U; i < count; ++i) {
            const std::size_t next = (i + 1U) % count;
            push_pixel(a[i].x, a[i].y, shade[band]);
            push_pixel(a[next].x, a[next].y, shade[band]);
            push_pixel(b[next].x, b[next].y, shade[band + 1U]);
            push_pixel(a[i].x, a[i].y, shade[band]);
            push_pixel(b[next].x, b[next].y, shade[band + 1U]);
            push_pixel(b[i].x, b[i].y, shade[band + 1U]);
        }
    }
}

void UiDrawList::add_corner_mask(
    float x,
    float y,
    float width,
    float height,
    float radius,
    UiColor color,
    core::u8 corners) {
    RoundedOutline outline{};
    if (!rounded_outline(x, y, width, height, radius, corners, outline) ||
        !begin_run(nullptr)) {
        return;
    }
    constexpr float quarter = 1.57079632679F;
    const std::array<float, 4U> start{
        2.0F * quarter, 3.0F * quarter, 0.0F, quarter};
    const std::array<UiPoint, 4U> square{
        UiPoint{outline.x0, outline.y0}, UiPoint{outline.x1, outline.y0},
        UiPoint{outline.x1, outline.y1}, UiPoint{outline.x0, outline.y1}};
    const UiColor clear{color.r, color.g, color.b, 0.0F};
    for (std::size_t corner = 0U; corner < 4U; ++corner) {
        const float r = outline.radius[corner];
        if (r <= 0.0F) {
            continue;
        }
        const float sx = corner == 0U || corner == 3U ? 1.0F : -1.0F;
        const float sy = corner < 2U ? 1.0F : -1.0F;
        const UiPoint centre{square[corner].x + sx * r, square[corner].y + sy * r};
        // A fan from the square corner to the arc pushed half a pixel out,
        // then a fringe fading inward across the arc: the mask is solid
        // outside the curve and gone half a pixel inside it.
        UiPoint previous_outer{};
        UiPoint previous_inner{};
        for (core::u32 step = 0U; step <= outline.segments; ++step) {
            const float angle = start[corner] + quarter *
                static_cast<float>(step) / static_cast<float>(outline.segments);
            const float nx = std::cos(angle);
            const float ny = std::sin(angle);
            const UiPoint outer{centre.x + nx * (r + 0.5F), centre.y + ny * (r + 0.5F)};
            const UiPoint inner{centre.x + nx * std::max(r - 0.5F, 0.0F),
                                centre.y + ny * std::max(r - 0.5F, 0.0F)};
            if (step > 0U) {
                push_pixel(square[corner].x, square[corner].y, color);
                push_pixel(previous_outer.x, previous_outer.y, color);
                push_pixel(outer.x, outer.y, color);
                push_pixel(previous_outer.x, previous_outer.y, color);
                push_pixel(outer.x, outer.y, color);
                push_pixel(inner.x, inner.y, clear);
                push_pixel(previous_outer.x, previous_outer.y, color);
                push_pixel(inner.x, inner.y, clear);
                push_pixel(previous_inner.x, previous_inner.y, clear);
            }
            previous_outer = outer;
            previous_inner = inner;
        }
    }
}

void UiDrawList::add_gradient_rect(
    float x,
    float y,
    float width,
    float height,
    UiColor top_left,
    UiColor top_right,
    UiColor bottom_right,
    UiColor bottom_left) {
    float x0{};
    float x1{};
    float y0{};
    float y1{};
    snap_span(x, width, x0, x1);
    snap_span(y, height, y0, y1);
    if (x1 <= x0 || y1 <= y0 || !begin_run(nullptr)) {
        return;
    }
    push_pixel(x0, y0, top_left);
    push_pixel(x1, y0, top_right);
    push_pixel(x1, y1, bottom_right);
    push_pixel(x0, y0, top_left);
    push_pixel(x1, y1, bottom_right);
    push_pixel(x0, y1, bottom_left);
}

void UiDrawList::add_outline_box(
    float x, float y, float width, float height, float thickness,
    UiColor color) {
    if (width <= 0.0F || height <= 0.0F) {
        return;
    }
    const float t = std::min({thickness, width * 0.5F, height * 0.5F});
    add_rect(x, y, width, t, color);
    add_rect(x, y + height - t, width, t, color);
    add_rect(x, y + t, t, height - 2.0F * t, color);
    add_rect(x + width - t, y + t, t, height - 2.0F * t, color);
}

bool UiDrawList::begin_run(const rhi::Texture* texture, bool nearest) {
    const Clip clip = current_clip();
    // Fully-clipped geometry contributes nothing; skip it so an offscreen
    // widget costs no vertices.
    if (clip.min_x >= clip.max_x || clip.min_y >= clip.max_y) {
        return false;
    }
    // A batch runs until either the clip or the texture changes: both are bound
    // per batch, and the texture additionally costs a pass, so they break the
    // run together rather than separately.
    const bool same_run = !batches_.empty()
        && batches_.back().clip_min_x == clip.min_x
        && batches_.back().clip_min_y == clip.min_y
        && batches_.back().clip_max_x == clip.max_x
        && batches_.back().clip_max_y == clip.max_y
        && batches_.back().texture == texture
        && batches_.back().nearest == nearest;
    if (!same_run) {
        batches_.push_back(UiBatch{
            static_cast<core::u32>(vertices_.size()),
            0U,
            clip.min_x,
            clip.min_y,
            clip.max_x,
            clip.max_y,
            texture,
            nearest});
    }
    return true;
}

void UiDrawList::add_triangles(
    std::span<const UiVertex> vertices, const rhi::Texture* texture,
    float clip_min_x, float clip_min_y, float clip_max_x, float clip_max_y) {
    if (vertices.empty() || clip_min_x >= clip_max_x
        || clip_min_y >= clip_max_y) {
        return;
    }
    const bool same_run = !batches_.empty()
        && batches_.back().clip_min_x == clip_min_x
        && batches_.back().clip_min_y == clip_min_y
        && batches_.back().clip_max_x == clip_max_x
        && batches_.back().clip_max_y == clip_max_y
        && batches_.back().texture == texture && !batches_.back().nearest;
    if (!same_run) {
        batches_.push_back(UiBatch{
            static_cast<core::u32>(vertices_.size()), 0U, clip_min_x,
            clip_min_y, clip_max_x, clip_max_y, texture, false});
    }
    vertices_.insert(vertices_.end(), vertices.begin(), vertices.end());
    batches_.back().vertex_count += static_cast<core::u32>(vertices.size());
}

void UiDrawList::push_solid(float x, float y, UiColor color) {
    vertices_.push_back(UiVertex{
        x * scale_, y * scale_, white_u_, white_v_, color.r, color.g, color.b,
        color.a});
    ++batches_.back().vertex_count;
}

void UiDrawList::scale_since(std::size_t first, float centre_x, float centre_y,
                             float factor) noexcept {
    if (factor == 1.0F || !std::isfinite(factor)) {
        return;
    }
    const float cx = centre_x * scale_;
    const float cy = centre_y * scale_;
    for (std::size_t index = first; index < vertices_.size(); ++index) {
        UiVertex& vertex = vertices_[index];
        vertex.x = cx + (vertex.x - cx) * factor;
        vertex.y = cy + (vertex.y - cy) * factor;
    }
}

void UiDrawList::push_pixel(float x, float y, UiColor color) {
    vertices_.push_back(UiVertex{
        x, y, white_u_, white_v_, color.r, color.g, color.b, color.a});
    ++batches_.back().vertex_count;
}

void UiDrawList::quad(
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
    bool nearest) {
    float x0{};
    float x1{};
    float y0{};
    float y1{};
    snap_span(x, width, x0, x1);
    snap_span(y, height, y0, y1);
    if (x1 <= x0 || y1 <= y0) {
        return;
    }
    if (!begin_run(texture, nearest && texture != nullptr)) {
        return;
    }

    const UiVertex tl{x0, y0, u0, v0, color.r, color.g, color.b, color.a};
    const UiVertex tr{x1, y0, u1, v0, color.r, color.g, color.b, color.a};
    const UiVertex br{x1, y1, u1, v1, color.r, color.g, color.b, color.a};
    const UiVertex bl{x0, y1, u0, v1, color.r, color.g, color.b, color.a};
    vertices_.push_back(tl);
    vertices_.push_back(tr);
    vertices_.push_back(br);
    vertices_.push_back(tl);
    vertices_.push_back(br);
    vertices_.push_back(bl);
    batches_.back().vertex_count += 6U;
}

// ---------------------------------------------------------------------------
// UiRenderer

UiRenderer::UiRenderer(
    rhi::GraphicsPipeline pipeline,
    rhi::Sampler sampler,
    rhi::Sampler nearest_sampler,
    rhi::StorageBuffer vertices) noexcept
    : pipeline_(std::move(pipeline))
    , sampler_(std::move(sampler))
    , nearest_sampler_(std::move(nearest_sampler))
    , vertex_buffer_(std::move(vertices)) {}

bool UiRenderer::shaders_available() noexcept {
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    return true;
#else
    return false;
#endif
}

core::Result<UiRenderer, RenderError> UiRenderer::create(
    rhi::Device& device, rhi::TextureFormat target_format) {
    using Result = core::Result<UiRenderer, RenderError>;
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = rhi::ShaderFormat::Dxil;
    vertex.bytecode = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_ui_vert_dxil),
        sizeof(mediaserver_ui_vert_dxil)};
    vertex.storage_buffer_count = 1U;
    vertex.uniform_buffer_count = 2U;

    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = rhi::ShaderFormat::Dxil;
    fragment.bytecode = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_ui_frag_dxil),
        sizeof(mediaserver_ui_frag_dxil)};
    fragment.sampler_count = 1U;

    // draw_pulled always binds a depth target, so the pipeline must declare one
    // to match the pass. The UI never occludes itself: depth is enabled only to
    // satisfy that match, with writes off and a GREATER test the far-plane z of
    // every UI vertex passes against the 0.0-cleared buffer.
    const rhi::DepthState depth{true, false, true};
    auto pipeline =
        device.create_pipeline(vertex, fragment, target_format, true, depth);
    if (!pipeline) {
        return Result::failure(RenderError::ShaderFormatUnsupported);
    }
    auto sampler = device.create_sampler(rhi::Filter::Linear);
    auto nearest_sampler = device.create_sampler(rhi::Filter::Nearest);
    if (!sampler || !nearest_sampler) {
        return Result::failure(RenderError::Unavailable);
    }
    auto vertices = device.create_storage_buffer(
        max_ui_vertices * static_cast<core::u32>(sizeof(UiVertex)));
    if (!vertices) {
        return Result::failure(RenderError::AllocationFailed);
    }
    return Result::success(UiRenderer{
        std::move(pipeline).value(),
        std::move(sampler).value(),
        std::move(nearest_sampler).value(),
        std::move(vertices).value()});
#else
    static_cast<void>(device);
    static_cast<void>(target_format);
    return Result::failure(RenderError::ShaderFormatUnsupported);
#endif
}

bool UiRenderer::upload_atlas(
    rhi::Device& device,
    rhi::CommandList& commands,
    std::span<const core::u8> rgba,
    core::u32 width,
    core::u32 height) {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    auto texture = device.create_texture(desc);
    if (!texture) {
        return false;
    }
    atlas_ = std::move(texture).value();
    return commands.upload(atlas_, rgba);
}

bool UiRenderer::render(
    rhi::Device& device,
    rhi::CommandList& commands,
    rhi::Texture& target,
    rhi::Color clear,
    const UiDrawList& draw_list,
    core::u32 width,
    core::u32 height) {
    if (atlas_.backend_handle() == nullptr) {
        return false; // upload_atlas must run first
    }

    // The pulled draw requires a depth target of the colour's size; the UI never
    // tests depth, but the entrypoint binds one. Kept sized to the target.
    if (depth_.backend_handle() == nullptr || depth_width_ != width
        || depth_height_ != height) {
        rhi::TextureDesc desc{};
        desc.width = width;
        desc.height = height;
        desc.format = rhi::TextureFormat::Depth32Float;
        desc.depth_target = true;
        auto depth = device.create_texture(desc);
        if (!depth) {
            return false;
        }
        depth_ = std::move(depth).value();
        depth_width_ = width;
        depth_height_ = height;
    }

    const auto& vertices = draw_list.vertices();
    if (vertices.size() > max_ui_vertices) {
        return false;
    }
    vertex_scratch_.resize(vertices.size() * sizeof(UiVertex));
    if (!vertices.empty()) {
        std::memcpy(
            vertex_scratch_.data(), vertices.data(), vertex_scratch_.size());
        if (!commands.upload(vertex_buffer_, vertex_scratch_)) {
            return false;
        }
    }

    const ViewConstants view{
        static_cast<float>(width), static_cast<float>(height), 0.0F, 0.0F};
    const rhi::StorageBuffer* const vertex_buffers[] = {&vertex_buffer_};

    const auto record = [&](const rhi::Texture& texture, bool nearest,
                            bool first) {
        rhi::PulledDraw draw{};
        draw.pipeline = &pipeline_;
        draw.vertex_buffers = vertex_buffers;
        draw.vertex_uniform = std::span<const core::u8>{
            reinterpret_cast<const core::u8*>(&view), sizeof(view)};
        draw.batch_uniforms = batch_scratch_;
        draw.batch_uniform_stride = sizeof(BatchConstants);
        draw.batches = pass_batches_;
        draw.texture = &texture;
        draw.sampler = nearest ? &nearest_sampler_ : &sampler_;
        // The first pass clears; the rest compose over what it left, so the
        // painter's order across textures is still the CPU draw order.
        draw.load = !first;
        // Depth is only present to match the pass. It is never written, but a
        // later pass loads and tests against it, so it has to survive.
        draw.store_depth = true;
        return commands.draw_pulled(target, clear, depth_, 0.0F, draw);
    };

    // The RHI binds one texture per pass, so consecutive batches are grouped by
    // the texture they sample and each group becomes a pass. A UI of nothing but
    // text and fills is one pass, exactly as before; a panel showing an engine
    // target costs one more, and no copy.
    const auto& batches = draw_list.batches();
    bool first_pass = true;
    std::size_t index = 0U;
    while (index < batches.size()) {
        const rhi::Texture* const texture = batches[index].texture;
        const bool nearest = batches[index].nearest;
        batch_scratch_.clear();
        pass_batches_.clear();

        std::size_t end = index;
        while (end < batches.size() && batches[end].texture == texture
               && batches[end].nearest == nearest) {
            const UiBatch& batch = batches[end];
            ++end;
            if (batch.vertex_count == 0U) {
                continue;
            }
            BatchConstants constants{};
            constants.first_vertex = batch.first_vertex;
            constants.clip_min_x = batch.clip_min_x;
            constants.clip_min_y = batch.clip_min_y;
            constants.clip_max_x = batch.clip_max_x;
            constants.clip_max_y = batch.clip_max_y;
            append(batch_scratch_, constants);
            pass_batches_.push_back(rhi::DrawBatch{batch.vertex_count, 1U});
        }
        index = end;

        if (pass_batches_.empty()) {
            continue;
        }
        if (!record(texture != nullptr ? *texture : atlas_, nearest,
                    first_pass)) {
            return false;
        }
        first_pass = false;
    }

    if (first_pass) {
        // Nothing to draw, but the target still owes its clear.
        batch_scratch_.clear();
        pass_batches_.clear();
        return record(atlas_, false, true);
    }
    return true;
}

} // namespace mediaserver::gui
