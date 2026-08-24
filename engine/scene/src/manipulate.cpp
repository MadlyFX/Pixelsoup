#include <mediaserver/scene/manipulate.hpp>

#include <array>
#include <cstddef>

namespace mediaserver::scene {
namespace {

[[nodiscard]] Vec3 apply(const Mat4& transform, Vec3 point) noexcept {
    return Vec3{
        transform.at(0U, 0U) * point.x + transform.at(0U, 1U) * point.y
            + transform.at(0U, 2U) * point.z + transform.at(0U, 3U),
        transform.at(1U, 0U) * point.x + transform.at(1U, 1U) * point.y
            + transform.at(1U, 2U) * point.z + transform.at(1U, 3U),
        transform.at(2U, 0U) * point.x + transform.at(2U, 1U) * point.y
            + transform.at(2U, 2U) * point.z + transform.at(2U, 3U)};
}

// True when (px, py) lies inside the convex quad, whatever its winding: the
// cross product of each edge with the point-to-vertex vector keeps one sign all
// the way round for an interior point.
[[nodiscard]] bool inside_quad(
    float px,
    float py,
    const std::array<CanvasProjection, 4U>& quad) noexcept {
    bool negative = false;
    bool positive = false;
    for (std::size_t i = 0U; i < 4U; ++i) {
        const auto& a = quad[i];
        const auto& b = quad[(i + 1U) % 4U];
        const auto cross =
            (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
        negative = negative || cross < 0.0F;
        positive = positive || cross > 0.0F;
    }
    return !(negative && positive);
}

} // namespace

Vec3 translation_of(const Mat4& transform) noexcept {
    return Vec3{
        transform.at(0U, 3U), transform.at(1U, 3U), transform.at(2U, 3U)};
}

Vec3 screen_drag_to_translation(
    const Camera& camera,
    Canvas canvas,
    Vec3 world_point,
    float drag_x,
    float drag_y) noexcept {
    if (canvas.height == 0U) {
        return Vec3{};
    }
    const auto forward = normalize(camera.target - camera.eye);
    const auto right = normalize(cross(forward, camera.up));
    const auto up = cross(right, forward);

    // Depth along the view direction. A point at or behind the eye has no image
    // plane to drag on.
    const auto depth = dot(world_point - camera.eye, forward);
    if (!(depth > 0.0F)) {
        return Vec3{};
    }

    // World height the canvas spans at this depth, divided by its pixels. The
    // field of view applies to height and pixels are square, so this scale is
    // the same horizontally.
    const auto units_per_pixel = 2.0F * depth
        * deterministic::tan_half_turns(camera.field_of_view)
        / static_cast<float>(canvas.height);

    return right * (drag_x * units_per_pixel)
        + up * (-drag_y * units_per_pixel);
}

const MediaInstance* pick_media(
    const Scene& scene,
    Canvas canvas,
    float canvas_x,
    float canvas_y) noexcept {
    // A screen-facing unit quad's corners, in order around its rim.
    static constexpr std::array<Vec3, 4U> corners{
        Vec3{-0.5F, -0.5F, 0.0F},
        Vec3{0.5F, -0.5F, 0.0F},
        Vec3{0.5F, 0.5F, 0.0F},
        Vec3{-0.5F, 0.5F, 0.0F}};

    const MediaInstance* best = nullptr;
    float best_depth = -1.0F; // reversed depth: larger is nearer
    for (const auto& clip : scene.media) {
        std::array<CanvasProjection, 4U> projected{};
        bool visible = true;
        float depth_sum = 0.0F;
        for (std::size_t i = 0U; i < corners.size(); ++i) {
            projected[i] = project_to_canvas(
                scene.camera, canvas, apply(clip.world, corners[i]));
            if (!projected[i].visible) {
                visible = false;
                break;
            }
            depth_sum += projected[i].depth;
        }
        if (!visible || !inside_quad(canvas_x, canvas_y, projected)) {
            continue;
        }
        const auto depth = depth_sum * 0.25F;
        if (best == nullptr || depth > best_depth) {
            best = &clip;
            best_depth = depth;
        }
    }
    return best;
}

const MediaInstance* media_for(const Scene& scene, doc::NodeId node) noexcept {
    for (const auto& clip : scene.media) {
        if (clip.source_node == node) {
            return &clip;
        }
    }
    return nullptr;
}

const Mat4* world_for(const Scene& scene, doc::NodeId node) noexcept {
    for (const auto& clip : scene.media) {
        if (clip.source_node == node) {
            return &clip.world;
        }
    }
    for (const auto& object : scene.instances) {
        if (object.source == node) {
            return &object.world;
        }
    }
    for (const auto& model : scene.models) {
        if (model.source_node == node) {
            return &model.world;
        }
    }
    return nullptr;
}

} // namespace mediaserver::scene
