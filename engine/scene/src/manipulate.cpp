#include <mediaserver/scene/manipulate.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>

namespace mediaserver::scene {
namespace {

[[nodiscard]] Vec3 apply(const Mat4& transform, Vec3 point) noexcept {
    return Vec3{
        transform.at(0U, 0U) * point.x + transform.at(0U, 1U) * point.y +
            transform.at(0U, 2U) * point.z + transform.at(0U, 3U),
        transform.at(1U, 0U) * point.x + transform.at(1U, 1U) * point.y +
            transform.at(1U, 2U) * point.z + transform.at(1U, 3U),
        transform.at(2U, 0U) * point.x + transform.at(2U, 1U) * point.y +
            transform.at(2U, 2U) * point.z + transform.at(2U, 3U)};
}

// True when (px, py) lies inside the convex polygon, whatever its winding: the
// cross product of each edge with the point-to-vertex vector keeps one sign all
// the way round for an interior point.
[[nodiscard]] bool
inside_polygon(float px, float py, const CanvasProjection* points,
               std::size_t count) noexcept {
    float twice_area = 0.0F;
    bool negative = false;
    bool positive = false;
    for (std::size_t i = 0U; i < count; ++i) {
        const auto& a = points[i];
        const auto& b = points[(i + 1U) % count];
        twice_area += a.x * b.y - b.x * a.y;
        const auto cross = (b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x);
        negative = negative || cross < 0.0F;
        positive = positive || cross > 0.0F;
    }
    // A zero-area face (common for a flat imported model's edge faces) cannot
    // contain a pixel. Without this guard every point has the same vacuous
    // cross-product sign and the collapsed face reports a viewport-wide hit.
    return std::abs(twice_area) > 1.0e-3F && !(negative && positive);
}

[[nodiscard]] bool
inside_quad(float px, float py,
            const std::array<CanvasProjection, 4U>& quad) noexcept {
    return inside_polygon(px, py, quad.data(), quad.size());
}

struct ProjectedHit final {
    bool hit{};
    float depth{};
    bool crosses_camera{};
};

// The depth of a planar convex polygon at the canvas point. Projected depth
// is linear in canvas space across a plane, so the fan triangle holding the
// point interpolates it exactly; the vertex average is the fallback.
[[nodiscard]] float depth_at(float px, float py, const CanvasProjection* points,
                             std::size_t count) noexcept {
    float average = 0.0F;
    for (std::size_t index = 0U; index < count; ++index) {
        average += points[index].depth;
    }
    average /= static_cast<float>(count);
    const auto& a = points[0U];
    for (std::size_t index = 1U; index + 1U < count; ++index) {
        const auto& b = points[index];
        const auto& c = points[index + 1U];
        const float area = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
        if (std::abs(area) <= 1.0e-6F) {
            continue;
        }
        const float wb = ((px - a.x) * (c.y - a.y) - (c.x - a.x) * (py - a.y)) / area;
        const float wc = ((b.x - a.x) * (py - a.y) - (px - a.x) * (b.y - a.y)) / area;
        const float wa = 1.0F - wb - wc;
        constexpr float slack = -1.0e-4F;
        if (wa >= slack && wb >= slack && wc >= slack) {
            return wa * a.depth + wb * b.depth + wc * c.depth;
        }
    }
    return average;
}

// The camera's near plane in world space. A face reaching behind the camera
// is clipped to it before projection, so an element the camera stands in or
// beside can still be found under the pointer.
struct NearPlane final {
    Vec3 point{};
    Vec3 normal{0.0F, 0.0F, -1.0F};
};

[[nodiscard]] std::optional<NearPlane> near_plane_of(const Camera& camera) noexcept {
    const Vec3 forward = normalize(camera.target - camera.eye, Vec3{});
    if (length(forward) == 0.0F || !(camera.near_plane > 0.0F)) {
        return std::nullopt;
    }
    // A hair past the near plane, so a clipped vertex projects in front.
    return NearPlane{camera.eye + forward * (camera.near_plane * 1.01F),
                     forward};
}

// One world-space face against the pointer. A face entirely in front of the
// camera is tested as projected; one that crosses the camera is clipped to
// the near plane first (when there is one) and reported as crossing.
[[nodiscard]] ProjectedHit
face_hit(const SceneProjector& projector, const std::array<Vec3, 4U>& corners,
         const NearPlane* near, float canvas_x, float canvas_y) noexcept {
    std::array<CanvasProjection, 4U> projected{};
    bool in_front = true;
    float depth = 0.0F;
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        projected[index] = projector(corners[index]);
        in_front = in_front && projected[index].visible;
        depth += projected[index].depth;
    }
    if (in_front) {
        return inside_quad(canvas_x, canvas_y, projected)
                   ? ProjectedHit{true, depth * 0.25F, false}
                   : ProjectedHit{};
    }
    if (near == nullptr) {
        return ProjectedHit{};
    }
    // Sutherland-Hodgman against the one plane: at most one extra vertex.
    std::array<Vec3, 5U> clipped{};
    std::size_t count = 0U;
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        const Vec3 a = corners[index];
        const Vec3 b = corners[(index + 1U) % corners.size()];
        const float da = dot(a - near->point, near->normal);
        const float db = dot(b - near->point, near->normal);
        if (da >= 0.0F && count < clipped.size()) {
            clipped[count++] = a;
        }
        if ((da >= 0.0F) != (db >= 0.0F) && count < clipped.size()) {
            clipped[count++] = a + (b - a) * (da / (da - db));
        }
    }
    if (count < 3U) {
        return ProjectedHit{};
    }
    std::array<CanvasProjection, 5U> kept{};
    for (std::size_t index = 0U; index < count; ++index) {
        kept[index] = projector(clipped[index]);
        if (!kept[index].visible) {
            return ProjectedHit{};
        }
    }
    if (!inside_polygon(canvas_x, canvas_y, kept.data(), count)) {
        return ProjectedHit{};
    }
    // The clipped vertices sit on the near plane, so an average would put a
    // floor in front of the box standing on it.
    return ProjectedHit{true, depth_at(canvas_x, canvas_y, kept.data(), count),
                        true};
}

[[nodiscard]] ProjectedHit
projected_quad_hit(const SceneProjector& projector, const Mat4& world,
                   const std::array<Vec3, 4U>& corners, float canvas_x,
                   float canvas_y, const NearPlane* near) noexcept {
    std::array<Vec3, 4U> placed{};
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        placed[index] = transform_point(world, corners[index]);
    }
    return face_hit(projector, placed, near, canvas_x, canvas_y);
}

[[nodiscard]] bool valid(const ModelLocalBounds& bounds) noexcept {
    return std::isfinite(bounds.minimum.x) &&
           std::isfinite(bounds.minimum.y) &&
           std::isfinite(bounds.minimum.z) &&
           std::isfinite(bounds.maximum.x) &&
           std::isfinite(bounds.maximum.y) &&
           std::isfinite(bounds.maximum.z) &&
           bounds.minimum.x <= bounds.maximum.x &&
           bounds.minimum.y <= bounds.maximum.y &&
           bounds.minimum.z <= bounds.maximum.z;
}

[[nodiscard]] ProjectedHit
projected_box_hit(const SceneProjector& projector, const Mat4& world,
                  ModelLocalBounds bounds, float canvas_x,
                  float canvas_y, const NearPlane* near) noexcept {
    if (!valid(bounds)) {
        bounds = ModelLocalBounds{};
    }
    const std::array<Vec3, 8U> corners{
        Vec3{bounds.minimum.x, bounds.minimum.y, bounds.minimum.z},
        Vec3{bounds.maximum.x, bounds.minimum.y, bounds.minimum.z},
        Vec3{bounds.maximum.x, bounds.maximum.y, bounds.minimum.z},
        Vec3{bounds.minimum.x, bounds.maximum.y, bounds.minimum.z},
        Vec3{bounds.minimum.x, bounds.minimum.y, bounds.maximum.z},
        Vec3{bounds.maximum.x, bounds.minimum.y, bounds.maximum.z},
        Vec3{bounds.maximum.x, bounds.maximum.y, bounds.maximum.z},
        Vec3{bounds.minimum.x, bounds.maximum.y, bounds.maximum.z}};
    static constexpr std::array<std::array<std::size_t, 4U>, 6U> faces{{
        {{0U, 1U, 2U, 3U}},
        {{4U, 7U, 6U, 5U}},
        {{0U, 4U, 5U, 1U}},
        {{3U, 2U, 6U, 7U}},
        {{0U, 3U, 7U, 4U}},
        {{1U, 5U, 6U, 2U}},
    }};

    std::array<Vec3, 8U> placed{};
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        placed[index] = transform_point(world, corners[index]);
    }

    ProjectedHit result{};
    result.depth = -std::numeric_limits<float>::infinity();
    for (const auto& face : faces) {
        const ProjectedHit one = face_hit(
            projector,
            {placed[face[0U]], placed[face[1U]], placed[face[2U]],
             placed[face[3U]]},
            near, canvas_x, canvas_y);
        if (one.hit) {
            result.hit = true;
            result.depth = std::max(result.depth, one.depth);
        }
    }
    if (result.hit) {
        // The whole box, not just the face that was hit, decides whether the
        // element crosses the camera: a plain click never takes a box the
        // camera is partly inside.
        for (const Vec3& corner : placed) {
            result.crosses_camera =
                result.crosses_camera || !projector(corner).visible;
        }
    }
    return result;
}

[[nodiscard]] ProjectedHit projected_mesh_hit(const SceneProjector& projector,
                                              const Mat4& world,
                                              Primitive primitive,
                                              float canvas_x,
                                              float canvas_y,
                                              const NearPlane* near) noexcept {
    static constexpr std::array<Vec3, 4U> quad{
        Vec3{-0.5F, -0.5F, 0.0F}, Vec3{0.5F, -0.5F, 0.0F},
        Vec3{0.5F, 0.5F, 0.0F}, Vec3{-0.5F, 0.5F, 0.0F}};
    static constexpr std::array<Vec3, 4U> plane{
        Vec3{-0.5F, 0.0F, -0.5F}, Vec3{0.5F, 0.0F, -0.5F},
        Vec3{0.5F, 0.0F, 0.5F}, Vec3{-0.5F, 0.0F, 0.5F}};
    if (primitive == Primitive::Quad) {
        return projected_quad_hit(projector, world, quad, canvas_x, canvas_y,
                                  near);
    }
    if (primitive == Primitive::Plane) {
        return projected_quad_hit(projector, world, plane, canvas_x, canvas_y,
                                  near);
    }
    // Procedural volumes have unit local bounds. Imported models take the
    // bounds-provider path in pick_scene_all below.
    return projected_box_hit(projector, world, ModelLocalBounds{}, canvas_x,
                             canvas_y, near);
}

[[nodiscard]] std::vector<ScenePick>
pick_all(const Scene& scene, float canvas_x, float canvas_y,
         const SceneProjector& projector,
         const ModelBoundsProvider& model_bounds, float light_radius,
         bool include_lights, const NearPlane* near);

} // namespace

Vec3 translation_of(const Mat4& transform) noexcept {
    return Vec3{transform.at(0U, 3U), transform.at(1U, 3U),
                transform.at(2U, 3U)};
}

Vec3 screen_drag_to_translation(const Camera& camera, Canvas canvas,
                                Vec3 world_point, float drag_x,
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
    const auto units_per_pixel =
        2.0F * depth * deterministic::tan_half_turns(camera.field_of_view) /
        static_cast<float>(canvas.height);

    return right * (drag_x * units_per_pixel) +
           up * (-drag_y * units_per_pixel);
}

Vec3 screen_drag_to_translation(Vec3 world_point, Vec3 view_forward,
                                Vec3 view_up,
                                const SceneProjector& projector, float drag_x,
                                float drag_y) noexcept {
    if (!projector) {
        return Vec3{};
    }
    const Vec3 forward = normalize(view_forward);
    const Vec3 right = normalize(cross(forward, view_up));
    const Vec3 up = cross(right, forward);
    if (length(forward) == 0.0F || length(right) == 0.0F ||
        length(up) == 0.0F) {
        return Vec3{};
    }
    const CanvasProjection origin = projector(world_point);
    constexpr float probe_distance = 0.01F;
    const CanvasProjection horizontal =
        projector(world_point + right * probe_distance);
    const CanvasProjection vertical =
        projector(world_point + up * probe_distance);
    if (!origin.visible || !horizontal.visible || !vertical.visible) {
        return Vec3{};
    }
    const float xx = (horizontal.x - origin.x) / probe_distance;
    const float xy = (vertical.x - origin.x) / probe_distance;
    const float yx = (horizontal.y - origin.y) / probe_distance;
    const float yy = (vertical.y - origin.y) / probe_distance;
    const float determinant = xx * yy - xy * yx;
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-6F) {
        return Vec3{};
    }
    const float along_right = (drag_x * yy - xy * drag_y) / determinant;
    const float along_up = (xx * drag_y - drag_x * yx) / determinant;
    return right * along_right + up * along_up;
}

const MediaInstance* pick_media(const Scene& scene, Canvas canvas,
                                float canvas_x, float canvas_y) noexcept {
    // A screen-facing unit quad's corners, in order around its rim.
    static constexpr std::array<Vec3, 4U> corners{
        Vec3{-0.5F, -0.5F, 0.0F}, Vec3{0.5F, -0.5F, 0.0F},
        Vec3{0.5F, 0.5F, 0.0F}, Vec3{-0.5F, 0.5F, 0.0F}};

    const MediaInstance* best = nullptr;
    float best_depth = -1.0F; // reversed depth: larger is nearer
    for (const auto& clip : scene.media) {
        if (clip.locked) {
            continue;
        }
        std::array<CanvasProjection, 4U> projected{};
        bool visible = true;
        float depth_sum = 0.0F;
        for (std::size_t i = 0U; i < corners.size(); ++i) {
            projected[i] = project_to_canvas(scene.camera, canvas,
                                             apply(clip.world, corners[i]));
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

Vec3 directional_light_anchor(const Scene& scene,
                              const DirectionalLight& light) noexcept {
    const float view_distance = length(scene.camera.target - scene.camera.eye);
    const float offset = std::clamp(view_distance * 0.32F, 1.5F, 4.0F);
    const Vec3 direction = normalize(light.direction, Vec3{0.0F, -1.0F, 0.0F});
    return scene.camera.target - direction * offset;
}

std::optional<ScenePick> pick_scene(const Scene& scene, Canvas canvas,
                                    float canvas_x, float canvas_y,
                                    float light_radius,
                                    bool include_lights) {
    return pick_scene(scene, canvas, canvas_x, canvas_y, ModelBoundsProvider{},
                      light_radius, include_lights);
}

std::optional<ScenePick>
pick_scene(const Scene& scene, Canvas canvas, float canvas_x, float canvas_y,
           const ModelBoundsProvider& model_bounds, float light_radius,
           bool include_lights) {
    const auto hits = pick_scene_all(scene, canvas, canvas_x, canvas_y,
                                     model_bounds, light_radius,
                                     include_lights);
    const auto front = std::find_if(
        hits.begin(), hits.end(),
        [](const ScenePick& hit) noexcept { return !hit.crosses_camera; });
    return front == hits.end() ? std::optional<ScenePick>{}
                               : std::optional<ScenePick>{*front};
}

std::vector<ScenePick> pick_scene_all(const Scene& scene, Canvas canvas,
                                      float canvas_x, float canvas_y,
                                      float light_radius,
                                      bool include_lights) {
    return pick_scene_all(scene, canvas, canvas_x, canvas_y,
                          ModelBoundsProvider{}, light_radius, include_lights);
}

std::vector<ScenePick>
pick_scene_all(const Scene& scene, Canvas canvas, float canvas_x,
               float canvas_y, const ModelBoundsProvider& model_bounds,
               float light_radius, bool include_lights) {
    const SceneProjector projector = [&scene, canvas](Vec3 point) noexcept {
        return project_to_canvas(scene.camera, canvas, point);
    };
    const auto near = near_plane_of(scene.camera);
    return pick_all(scene, canvas_x, canvas_y, projector, model_bounds,
                    light_radius, include_lights, near ? &*near : nullptr);
}

std::vector<ScenePick>
pick_scene_all(const Scene& scene, float canvas_x, float canvas_y,
               const SceneProjector& projector,
               const ModelBoundsProvider& model_bounds, float light_radius,
               bool include_lights) {
    return pick_all(scene, canvas_x, canvas_y, projector, model_bounds,
                    light_radius, include_lights, nullptr);
}

namespace {

std::vector<ScenePick>
pick_all(const Scene& scene, float canvas_x, float canvas_y,
         const SceneProjector& projector,
         const ModelBoundsProvider& model_bounds, float light_radius,
         bool include_lights, const NearPlane* near) {
    std::vector<ScenePick> hits;
    hits.reserve(scene.instances.size() + scene.media.size() +
                 scene.models.size() +
                 (include_lights ? scene.point_lights.size() +
                                       scene.directional_lights.size()
                                 : 0U));
    const auto consider = [&hits](doc::NodeId node, Vec3 position,
                                  const ProjectedHit& hit) {
        if (!hit.hit) {
            return;
        }
        const auto existing = std::find_if(
            hits.begin(), hits.end(), [node](const ScenePick& held) noexcept {
                return held.node == node;
            });
        if (existing == hits.end()) {
            hits.push_back(
                ScenePick{node, position, hit.depth, hit.crosses_camera});
        } else {
            // One element is plainly clickable if any of its parts is.
            const bool crosses =
                existing->crosses_camera && hit.crosses_camera;
            if (hit.depth > existing->depth) {
                *existing = ScenePick{node, position, hit.depth, crosses};
            }
            existing->crosses_camera = crosses;
        }
    };

    for (const auto& object : scene.instances) {
        if (object.locked) {
            continue;
        }
        consider(object.source, translation_of(object.world),
                 projected_mesh_hit(projector, object.world,
                                    object.mesh.primitive, canvas_x, canvas_y,
                                    near));
    }
    for (const auto& clip : scene.media) {
        if (clip.locked) {
            continue;
        }
        consider(clip.source_node, translation_of(clip.world),
                 projected_mesh_hit(projector, clip.world,
                                    Primitive::Quad, canvas_x, canvas_y, near));
    }
    for (const auto& model : scene.models) {
        if (model.locked) {
            continue;
        }
        ModelLocalBounds bounds{};
        if (model_bounds) {
            const auto provided = model_bounds(model);
            if (provided.has_value() && valid(*provided)) {
                bounds = *provided;
            }
        }
        consider(model.source_node, translation_of(model.world),
                 projected_box_hit(projector, model.world, bounds,
                                   canvas_x, canvas_y, near));
    }

    if (!include_lights) {
        std::stable_sort(
            hits.begin(), hits.end(),
            [](const ScenePick& left, const ScenePick& right) noexcept {
                return left.depth != right.depth ? left.depth > right.depth
                                                 : left.node < right.node;
            });
        return hits;
    }
    const float radius_squared =
        std::max(light_radius, 0.0F) * std::max(light_radius, 0.0F);
    const auto consider_light = [&](doc::NodeId node, Vec3 position) {
        const CanvasProjection projected = projector(position);
        if (!projected.visible) {
            return;
        }
        const float dx = canvas_x - projected.x;
        const float dy = canvas_y - projected.y;
        if (dx * dx + dy * dy <= radius_squared) {
            consider(node, position, ProjectedHit{true, projected.depth, false});
        }
    };
    for (const auto& light : scene.point_lights) {
        if (light.locked) {
            continue;
        }
        consider_light(light.source, light.position);
    }
    for (const auto& light : scene.directional_lights) {
        if (light.locked) {
            continue;
        }
        consider_light(light.source, directional_light_anchor(scene, light));
    }
    std::stable_sort(
        hits.begin(), hits.end(),
        [](const ScenePick& left, const ScenePick& right) noexcept {
            return left.depth != right.depth ? left.depth > right.depth
                                             : left.node < right.node;
        });
    return hits;
}

} // namespace

const MediaInstance* media_for(const Scene& scene, doc::NodeId node) noexcept {
    for (const auto& clip : scene.media) {
        if (clip.source_node == node) {
            return &clip;
        }
    }
    return nullptr;
}

bool element_locked(const Scene& scene, doc::NodeId node) noexcept {
    for (const auto& group : scene.groups) {
        if (group.node == node) {
            return group.effective_locked;
        }
    }
    for (const auto& clip : scene.media) {
        if (clip.source_node == node) {
            return clip.locked;
        }
    }
    for (const auto& object : scene.instances) {
        if (object.source == node) {
            return object.locked;
        }
    }
    for (const auto& model : scene.models) {
        if (model.source_node == node) {
            return model.locked;
        }
    }
    for (const auto& light : scene.point_lights) {
        if (light.source == node) {
            return light.locked;
        }
    }
    for (const auto& light : scene.directional_lights) {
        if (light.source == node) {
            return light.locked;
        }
    }
    return false;
}

std::optional<Mat4> world_for(const Scene& scene, doc::NodeId node) noexcept {
    for (const auto& group : scene.groups) {
        if (group.node == node) {
            return group.world;
        }
    }
    // Before clips, because a clip on a screen reports the screen's world and
    // a track asked about itself must get its own answer rather than one that
    // happens to look the same until the track is empty.
    for (const auto& screen : scene.screens) {
        if (screen.node == node) {
            return screen.world;
        }
    }
    for (const auto& clip : scene.media) {
        if (clip.source_node == node) {
            return clip.world;
        }
    }
    for (const auto& object : scene.instances) {
        if (object.source == node) {
            return object.world;
        }
    }
    for (const auto& model : scene.models) {
        if (model.source_node == node) {
            return model.motion_source.value() ? model.batch_world : model.world;
        }
    }
    for (const auto& light : scene.point_lights) {
        if (light.source == node) {
            // A point light has a position but no rotation or scale, so its
            // "world" is a pure translation the gizmo can move.
            return translation(light.position);
        }
    }
    return std::nullopt;
}

} // namespace mediaserver::scene
