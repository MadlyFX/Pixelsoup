#include <mediaserver/scene/manipulate.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

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

// True when (px, py) lies inside the convex quad, whatever its winding: the
// cross product of each edge with the point-to-vertex vector keeps one sign all
// the way round for an interior point.
[[nodiscard]] bool
inside_quad(float px, float py,
            const std::array<CanvasProjection, 4U>& quad) noexcept {
    float twice_area = 0.0F;
    bool negative = false;
    bool positive = false;
    for (std::size_t i = 0U; i < 4U; ++i) {
        const auto& a = quad[i];
        const auto& b = quad[(i + 1U) % 4U];
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

struct ProjectedHit final {
    bool hit{};
    float depth{};
};

[[nodiscard]] ProjectedHit
projected_quad_hit(const SceneProjector& projector, const Mat4& world,
                   const std::array<Vec3, 4U>& corners, float canvas_x,
                   float canvas_y) noexcept {
    std::array<CanvasProjection, 4U> projected{};
    float depth = 0.0F;
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        projected[index] =
            projector(transform_point(world, corners[index]));
        if (!projected[index].visible) {
            return ProjectedHit{};
        }
        depth += projected[index].depth;
    }
    if (!inside_quad(canvas_x, canvas_y, projected)) {
        return ProjectedHit{};
    }
    return ProjectedHit{true, depth * 0.25F};
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
                  float canvas_y) noexcept {
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

    std::array<CanvasProjection, 8U> projected{};
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        projected[index] =
            projector(transform_point(world, corners[index]));
        if (!projected[index].visible) {
            return ProjectedHit{};
        }
    }

    ProjectedHit result{};
    result.depth = -std::numeric_limits<float>::infinity();
    for (const auto& face : faces) {
        std::array<CanvasProjection, 4U> quad{};
        float depth = 0.0F;
        for (std::size_t index = 0U; index < face.size(); ++index) {
            quad[index] = projected[face[index]];
            depth += quad[index].depth;
        }
        if (inside_quad(canvas_x, canvas_y, quad)) {
            result.hit = true;
            result.depth = std::max(result.depth, depth * 0.25F);
        }
    }
    return result;
}

[[nodiscard]] ProjectedHit projected_mesh_hit(const SceneProjector& projector,
                                              const Mat4& world,
                                              Primitive primitive,
                                              float canvas_x,
                                              float canvas_y) noexcept {
    static constexpr std::array<Vec3, 4U> quad{
        Vec3{-0.5F, -0.5F, 0.0F}, Vec3{0.5F, -0.5F, 0.0F},
        Vec3{0.5F, 0.5F, 0.0F}, Vec3{-0.5F, 0.5F, 0.0F}};
    static constexpr std::array<Vec3, 4U> plane{
        Vec3{-0.5F, 0.0F, -0.5F}, Vec3{0.5F, 0.0F, -0.5F},
        Vec3{0.5F, 0.0F, 0.5F}, Vec3{-0.5F, 0.0F, 0.5F}};
    if (primitive == Primitive::Quad) {
        return projected_quad_hit(projector, world, quad, canvas_x, canvas_y);
    }
    if (primitive == Primitive::Plane) {
        return projected_quad_hit(projector, world, plane, canvas_x, canvas_y);
    }
    // Procedural volumes have unit local bounds. Imported models take the
    // bounds-provider path in pick_scene_all below.
    return projected_box_hit(projector, world, ModelLocalBounds{}, canvas_x,
                             canvas_y);
}

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
    auto hits = pick_scene_all(scene, canvas, canvas_x, canvas_y, model_bounds,
                               light_radius, include_lights);
    return hits.empty() ? std::optional<ScenePick>{}
                        : std::optional<ScenePick>{hits.front()};
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
    return pick_scene_all(scene, canvas_x, canvas_y, projector, model_bounds,
                          light_radius, include_lights);
}

std::vector<ScenePick>
pick_scene_all(const Scene& scene, float canvas_x, float canvas_y,
               const SceneProjector& projector,
               const ModelBoundsProvider& model_bounds, float light_radius,
               bool include_lights) {
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
            hits.push_back(ScenePick{node, position, hit.depth});
        } else if (hit.depth > existing->depth) {
            *existing = ScenePick{node, position, hit.depth};
        }
    };

    for (const auto& object : scene.instances) {
        if (object.locked) {
            continue;
        }
        consider(object.source, translation_of(object.world),
                 projected_mesh_hit(projector, object.world,
                                    object.mesh.primitive, canvas_x, canvas_y));
    }
    for (const auto& clip : scene.media) {
        if (clip.locked) {
            continue;
        }
        consider(clip.source_node, translation_of(clip.world),
                 projected_mesh_hit(projector, clip.world,
                                    Primitive::Quad, canvas_x, canvas_y));
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
                                   canvas_x, canvas_y));
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
            consider(node, position, ProjectedHit{true, projected.depth});
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

const MediaInstance* media_for(const Scene& scene, doc::NodeId node) noexcept {
    for (const auto& clip : scene.media) {
        if (clip.source_node == node) {
            return &clip;
        }
    }
    return nullptr;
}

bool element_locked(const Scene& scene, doc::NodeId node) noexcept {
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
            return model.world;
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
