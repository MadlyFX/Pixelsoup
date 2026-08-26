#include <mediaserver/scene/canvas.hpp>

#include <cmath>

namespace mediaserver::scene {
namespace {

// A canvas larger than this is not a wall, it is a mistake in a document.
// Sixteen 4K outputs across is 61,440 pixels, so the bound is loose enough to
// be irrelevant in practice and tight enough that the pixel arithmetic below
// stays exact in binary32.
constexpr core::u64 max_canvas_extent = 65'535U;

[[nodiscard]] ViewMatrices identity_view() noexcept {
    return ViewMatrices{identity(), identity(), identity()};
}

// The perspective divide and the map from clip space back into canvas pixels.
// Shared so the whole-canvas path and the per-slice path cannot drift.
[[nodiscard]] CanvasProjection project(
    const Mat4& view_projection,
    CanvasRect rect,
    Vec3 world_point) noexcept {
    const auto clip = transform(
        view_projection, Vec4{world_point.x, world_point.y, world_point.z, 1.0F});
    const auto ndc_x = clip.w != 0.0F ? clip.x / clip.w : 0.0F;
    const auto ndc_y = clip.w != 0.0F ? clip.y / clip.w : 0.0F;
    const auto depth = clip.w != 0.0F ? clip.z / clip.w : 0.0F;

    const auto x = static_cast<float>(rect.x)
        + (ndc_x * 0.5F + 0.5F) * static_cast<float>(rect.width);
    const auto y = static_cast<float>(rect.y)
        + (0.5F - ndc_y * 0.5F) * static_cast<float>(rect.height);
    return CanvasProjection{x, y, depth, clip.w > 0.0F};
}

} // namespace

bool valid(Canvas canvas) noexcept {
    return canvas.width != 0U && canvas.height != 0U
        && static_cast<core::u64>(canvas.width) <= max_canvas_extent
        && static_cast<core::u64>(canvas.height) <= max_canvas_extent;
}

bool contains(Canvas canvas, CanvasRect slice) noexcept {
    if (!valid(canvas) || slice.width == 0U || slice.height == 0U) {
        return false;
    }
    const auto right = static_cast<core::u64>(slice.x)
        + static_cast<core::u64>(slice.width);
    const auto bottom = static_cast<core::u64>(slice.y)
        + static_cast<core::u64>(slice.height);
    return right <= static_cast<core::u64>(canvas.width)
        && bottom <= static_cast<core::u64>(canvas.height);
}

bool valid(const Camera& camera) noexcept {
    if (!std::isfinite(camera.near_plane) || !std::isfinite(camera.far_plane)) {
        return false;
    }
    if (!(camera.near_plane > 0.0F) || !(camera.far_plane > camera.near_plane)) {
        return false;
    }
    // Zero is what the deterministic tangent reports for a field of view at or
    // past half a turn, so this one check covers both the degenerate and the
    // out-of-range camera.
    if (!(deterministic::tan_half_turns(camera.field_of_view) > 0.0F)) {
        return false;
    }
    const auto direction = camera.target - camera.eye;
    return length(direction) > 0.0F;
}

CanvasRect whole(Canvas canvas) noexcept {
    return CanvasRect{0U, 0U, canvas.width, canvas.height};
}

FrustumWindow window_for_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice) noexcept {
    if (!valid(camera) || !contains(canvas, slice)) {
        return FrustumWindow{};
    }

    const auto near_plane = camera.near_plane;
    const auto canvas_width = static_cast<float>(canvas.width);
    const auto canvas_height = static_cast<float>(canvas.height);

    // The full near-plane window the camera sees, before the slice narrows it.
    const auto top = near_plane
        * deterministic::tan_half_turns(camera.field_of_view);
    const auto bottom = -top;
    const auto right = top * (canvas_width / canvas_height);
    const auto left = -right;
    const auto span_x = right - left;
    const auto span_y = top - bottom;

    // The slice's window, as fractions of the canvas.
    //
    // Both edges of a shared boundary are the same expression over the same
    // integers - node A's `x + width` is node B's `x` - so two adjacent slices
    // compute a bit-identical plane and the seam cannot open. This is the one
    // place in the projection where that matters, and it is why the fractions
    // are formed from the pixel counts rather than accumulated per slice.
    const auto edge = [](core::u32 origin, core::u32 extent) noexcept {
        return static_cast<float>(
            static_cast<core::u64>(origin) + static_cast<core::u64>(extent));
    };

    return FrustumWindow{
        left + span_x * (static_cast<float>(slice.x) / canvas_width),
        left + span_x * (edge(slice.x, slice.width) / canvas_width),
        top - span_y * (edge(slice.y, slice.height) / canvas_height),
        top - span_y * (static_cast<float>(slice.y) / canvas_height),
        near_plane,
        camera.far_plane,
    };
}

ViewMatrices view_for_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice) noexcept {
    const auto frustum = window_for_slice(camera, canvas, slice);
    const auto width = frustum.right - frustum.left;
    const auto height = frustum.top - frustum.bottom;
    const auto depth_span = frustum.far_plane - frustum.near_plane;
    if (!(width > 0.0F) || !(height > 0.0F) || !(depth_span > 0.0F)) {
        return identity_view();
    }

    const auto twice_near = frustum.near_plane + frustum.near_plane;

    Mat4 projection{};
    projection.set(0U, 0U, twice_near / width);
    projection.set(0U, 2U, (frustum.right + frustum.left) / width);
    projection.set(1U, 1U, twice_near / height);
    projection.set(1U, 2U, (frustum.top + frustum.bottom) / height);
    // Reversed depth: near maps to 1, far to 0. See the header.
    projection.set(2U, 2U, frustum.near_plane / depth_span);
    projection.set(2U, 3U, (frustum.near_plane * frustum.far_plane) / depth_span);
    projection.set(3U, 2U, -1.0F);

    const auto view = look_at(camera.eye, camera.target, camera.up);
    return ViewMatrices{view, projection, multiply(projection, view)};
}

CanvasProjection project_to_canvas(
    const Camera& camera,
    Canvas canvas,
    Vec3 world_point) noexcept {
    const auto rect = whole(canvas);
    const auto matrices = view_for_slice(camera, canvas, rect);
    return project(matrices.view_projection, rect, world_point);
}

CanvasProjection project_to_canvas(
    const ViewMatrices& matrices,
    Canvas canvas,
    Vec3 world_point) noexcept {
    return project(matrices.view_projection, whole(canvas), world_point);
}

CanvasProjection project_through_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice,
    Vec3 world_point) noexcept {
    const auto matrices = view_for_slice(camera, canvas, slice);
    return project(matrices.view_projection, slice, world_point);
}

} // namespace mediaserver::scene
