#include <mediaserver/scene/raycast.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <optional>

namespace mediaserver::scene {
namespace {

// A hit closer than this, in ray parameter, is the surface the ray starts on.
constexpr float minimum_distance = 1.0e-5F;
constexpr core::u32 bvh_leaf_triangles = 4U;
constexpr std::size_t bvh_stack_depth = 96U;

[[nodiscard]] float component(Vec3 v, std::size_t axis) noexcept {
    return axis == 0U ? v.x : (axis == 1U ? v.y : v.z);
}

[[nodiscard]] Vec3 axis_normal(std::size_t axis, float direction) noexcept {
    // Faces back along the ray.
    const float sign = direction > 0.0F ? -1.0F : 1.0F;
    return axis == 0U   ? Vec3{sign, 0.0F, 0.0F}
           : axis == 1U ? Vec3{0.0F, sign, 0.0F}
                        : Vec3{0.0F, 0.0F, sign};
}

[[nodiscard]] Vec3 at(const Ray& ray, float distance) noexcept {
    return ray.origin + ray.direction * distance;
}

[[nodiscard]] Vec3 facing(Vec3 normal, Vec3 direction) noexcept {
    const Vec3 unit = normalize(normal, Vec3{0.0F, 1.0F, 0.0F});
    return dot(unit, direction) > 0.0F ? -unit : unit;
}

[[nodiscard]] Vec3 minimum_of(Vec3 a, Vec3 b) noexcept {
    return Vec3{std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)};
}

[[nodiscard]] Vec3 maximum_of(Vec3 a, Vec3 b) noexcept {
    return Vec3{std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z)};
}

// The parametric interval a ray spends inside a box, or none.
struct Slab final {
    float near{};
    float far{};
    std::size_t near_axis{};
    std::size_t far_axis{};
};

[[nodiscard]] std::optional<Slab> slab(const Ray& ray, Vec3 minimum,
                                       Vec3 maximum,
                                       float max_distance) noexcept {
    Slab result{-std::numeric_limits<float>::infinity(), max_distance, 0U, 0U};
    for (std::size_t axis = 0U; axis < 3U; ++axis) {
        const float origin = component(ray.origin, axis);
        const float direction = component(ray.direction, axis);
        const float low = component(minimum, axis);
        const float high = component(maximum, axis);
        if (direction == 0.0F) {
            if (origin < low || origin > high) {
                return std::nullopt;
            }
            continue;
        }
        float entry = (low - origin) / direction;
        float exit = (high - origin) / direction;
        if (entry > exit) {
            std::swap(entry, exit);
        }
        if (entry > result.near) {
            result.near = entry;
            result.near_axis = axis;
        }
        if (exit < result.far) {
            result.far = exit;
            result.far_axis = axis;
        }
        if (result.near > result.far) {
            return std::nullopt;
        }
    }
    return result;
}

[[nodiscard]] bool box_reachable(const Ray& ray, Vec3 minimum, Vec3 maximum,
                                 float max_distance) noexcept {
    const auto inside = slab(ray, minimum, maximum, max_distance);
    return inside.has_value() && inside->far >= minimum_distance;
}

// Two-sided Moller-Trumbore. Returns the ray parameter.
[[nodiscard]] std::optional<float> triangle(const Ray& ray, Vec3 a, Vec3 b,
                                            Vec3 c, float max_distance) noexcept {
    const Vec3 edge_ab = b - a;
    const Vec3 edge_ac = c - a;
    const Vec3 p = cross(ray.direction, edge_ac);
    const float determinant = dot(edge_ab, p);
    if (std::abs(determinant) < 1.0e-20F) {
        return std::nullopt;
    }
    const float inverse = 1.0F / determinant;
    const Vec3 offset = ray.origin - a;
    const float u = dot(offset, p) * inverse;
    if (u < 0.0F || u > 1.0F) {
        return std::nullopt;
    }
    const Vec3 q = cross(offset, edge_ab);
    const float v = dot(ray.direction, q) * inverse;
    if (v < 0.0F || u + v > 1.0F) {
        return std::nullopt;
    }
    const float distance = dot(edge_ac, q) * inverse;
    if (distance < minimum_distance || distance > max_distance) {
        return std::nullopt;
    }
    return distance;
}

// A flat unit square through the origin, perpendicular to `axis`.
[[nodiscard]] std::optional<RayHit> unit_square(const Ray& ray,
                                                std::size_t axis,
                                                float max_distance) noexcept {
    const float direction = component(ray.direction, axis);
    if (direction == 0.0F) {
        return std::nullopt;
    }
    const float distance = -component(ray.origin, axis) / direction;
    if (distance < minimum_distance || distance > max_distance) {
        return std::nullopt;
    }
    const Vec3 position = at(ray, distance);
    for (std::size_t other = 0U; other < 3U; ++other) {
        if (other != axis && std::abs(component(position, other)) > 0.5F) {
            return std::nullopt;
        }
    }
    return RayHit{distance, position, axis_normal(axis, direction)};
}

// Roots of a t^2 + b t + c, smaller first.
[[nodiscard]] std::optional<std::array<float, 2U>>
quadratic(float a, float b, float c) noexcept {
    if (a == 0.0F) {
        return std::nullopt;
    }
    const float discriminant = b * b - 4.0F * a * c;
    if (discriminant < 0.0F) {
        return std::nullopt;
    }
    const float root = std::sqrt(discriminant);
    float first = (-b - root) / (2.0F * a);
    float second = (-b + root) / (2.0F * a);
    if (first > second) {
        std::swap(first, second);
    }
    return std::array<float, 2U>{first, second};
}

[[nodiscard]] std::optional<RayHit> unit_sphere(const Ray& ray,
                                                float max_distance) noexcept {
    const auto roots =
        quadratic(dot(ray.direction, ray.direction),
                  2.0F * dot(ray.origin, ray.direction),
                  dot(ray.origin, ray.origin) - 0.25F);
    if (!roots.has_value()) {
        return std::nullopt;
    }
    for (const float distance : *roots) {
        if (distance >= minimum_distance && distance <= max_distance) {
            const Vec3 position = at(ray, distance);
            return RayHit{distance, position,
                          facing(position, ray.direction)};
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<RayHit> unit_cylinder(const Ray& ray,
                                                  float max_distance) noexcept {
    std::optional<RayHit> best;
    const auto consider = [&](float distance, Vec3 normal) {
        if (distance < minimum_distance || distance > max_distance ||
            (best.has_value() && distance >= best->distance)) {
            return;
        }
        best = RayHit{distance, at(ray, distance),
                      facing(normal, ray.direction)};
    };
    const auto roots = quadratic(
        ray.direction.x * ray.direction.x + ray.direction.z * ray.direction.z,
        2.0F * (ray.origin.x * ray.direction.x +
                ray.origin.z * ray.direction.z),
        ray.origin.x * ray.origin.x + ray.origin.z * ray.origin.z - 0.25F);
    if (roots.has_value()) {
        for (const float distance : *roots) {
            const Vec3 position = at(ray, distance);
            if (std::abs(position.y) <= 0.5F) {
                consider(distance, Vec3{position.x, 0.0F, position.z});
            }
        }
    }
    if (ray.direction.y != 0.0F) {
        for (const float cap : {-0.5F, 0.5F}) {
            const float distance = (cap - ray.origin.y) / ray.direction.y;
            const Vec3 position = at(ray, distance);
            if (position.x * position.x + position.z * position.z <= 0.25F) {
                consider(distance, Vec3{0.0F, cap, 0.0F});
            }
        }
    }
    return best;
}

struct BuildTriangle final {
    Vec3 minimum{};
    Vec3 maximum{};
    Vec3 centroid{};
};

class BvhBuilder final {
public:
    BvhBuilder(std::vector<BuildTriangle> triangles, MeshBvh& into,
               std::stop_token stop)
        : triangles_(std::move(triangles)), into_(into), stop_(stop) {}

    [[nodiscard]] bool build(core::u32 begin, core::u32 end) {
        if (stop_.stop_requested()) {
            return false;
        }
        const auto index = static_cast<core::u32>(into_.nodes.size());
        MeshBvhNode node{};
        node.minimum = Vec3{std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::infinity()};
        node.maximum = -node.minimum;
        Vec3 centroid_minimum = node.minimum;
        Vec3 centroid_maximum = node.maximum;
        for (core::u32 slot = begin; slot < end; ++slot) {
            const BuildTriangle& built = triangles_[into_.triangles[slot]];
            node.minimum = minimum_of(node.minimum, built.minimum);
            node.maximum = maximum_of(node.maximum, built.maximum);
            centroid_minimum = minimum_of(centroid_minimum, built.centroid);
            centroid_maximum = maximum_of(centroid_maximum, built.centroid);
        }
        into_.nodes.push_back(node);
        if (end - begin <= bvh_leaf_triangles) {
            into_.nodes[index].first = begin;
            into_.nodes[index].count = end - begin;
            return true;
        }

        // Median split on the widest spread of centroids keeps the tree
        // balanced, so the query stack has a fixed bound.
        const Vec3 extent = centroid_maximum - centroid_minimum;
        std::size_t axis = 0U;
        if (extent.y > component(extent, axis)) {
            axis = 1U;
        }
        if (extent.z > component(extent, axis)) {
            axis = 2U;
        }
        const core::u32 middle = begin + (end - begin) / 2U;
        std::nth_element(
            into_.triangles.begin() + begin, into_.triangles.begin() + middle,
            into_.triangles.begin() + end,
            [this, axis](core::u32 left, core::u32 right) {
                const float a = component(triangles_[left].centroid, axis);
                const float b = component(triangles_[right].centroid, axis);
                return a < b || (a == b && left < right);
            });
        if (!build(begin, middle)) {
            return false;
        }
        into_.nodes[index].first = static_cast<core::u32>(into_.nodes.size());
        return build(middle, end);
    }

private:
    std::vector<BuildTriangle> triangles_;
    MeshBvh& into_;
    std::stop_token stop_;
};

[[nodiscard]] float determinant3(const Mat4& m) noexcept {
    return m.at(0U, 0U) * (m.at(1U, 1U) * m.at(2U, 2U) -
                           m.at(1U, 2U) * m.at(2U, 1U)) -
           m.at(0U, 1U) * (m.at(1U, 0U) * m.at(2U, 2U) -
                           m.at(1U, 2U) * m.at(2U, 0U)) +
           m.at(0U, 2U) * (m.at(1U, 0U) * m.at(2U, 1U) -
                           m.at(1U, 1U) * m.at(2U, 0U));
}

[[nodiscard]] Vec3 transform_direction(const Mat4& m, Vec3 v) noexcept {
    return Vec3{
        m.at(0U, 0U) * v.x + m.at(0U, 1U) * v.y + m.at(0U, 2U) * v.z,
        m.at(1U, 0U) * v.x + m.at(1U, 1U) * v.y + m.at(1U, 2U) * v.z,
        m.at(2U, 0U) * v.x + m.at(2U, 1U) * v.y + m.at(2U, 2U) * v.z};
}

// Normals carry through the inverse transpose, which stays perpendicular to
// the surface under non-uniform scale.
[[nodiscard]] Vec3 transform_normal(const Mat4& inverse, Vec3 v) noexcept {
    return Vec3{
        inverse.at(0U, 0U) * v.x + inverse.at(1U, 0U) * v.y +
            inverse.at(2U, 0U) * v.z,
        inverse.at(0U, 1U) * v.x + inverse.at(1U, 1U) * v.y +
            inverse.at(2U, 1U) * v.z,
        inverse.at(0U, 2U) * v.x + inverse.at(1U, 2U) * v.y +
            inverse.at(2U, 2U) * v.z};
}

} // namespace

std::optional<Ray> canvas_ray(const Camera& camera, Canvas canvas,
                              float canvas_x, float canvas_y) noexcept {
    if (canvas.width == 0U || canvas.height == 0U) {
        return std::nullopt;
    }
    const ViewMatrices matrices = view_for_slice(
        camera, canvas, CanvasRect{0U, 0U, canvas.width, canvas.height});
    const Mat4 inverse = invert(matrices.view_projection);
    const float ndc_x =
        canvas_x / static_cast<float>(canvas.width) * 2.0F - 1.0F;
    const float ndc_y =
        1.0F - canvas_y / static_cast<float>(canvas.height) * 2.0F;
    // Reversed depth: the near plane is at 1.
    const Vec4 near = transform(inverse, Vec4{ndc_x, ndc_y, 1.0F, 1.0F});
    if (!(std::abs(near.w) > 1.0e-20F)) {
        return std::nullopt;
    }
    const Vec3 on_near{near.x / near.w, near.y / near.w, near.z / near.w};
    const Vec3 direction = on_near - camera.eye;
    if (!(length(direction) > 1.0e-12F) || !std::isfinite(direction.x) ||
        !std::isfinite(direction.y) || !std::isfinite(direction.z)) {
        return std::nullopt;
    }
    return Ray{camera.eye, normalize(direction)};
}

std::optional<RayHit> raycast_box(const Ray& ray, Vec3 minimum, Vec3 maximum,
                                  float max_distance) noexcept {
    const auto inside = slab(ray, minimum, maximum, max_distance);
    if (!inside.has_value()) {
        return std::nullopt;
    }
    if (inside->near >= minimum_distance) {
        return RayHit{inside->near, at(ray, inside->near),
                      axis_normal(inside->near_axis,
                                  component(ray.direction, inside->near_axis))};
    }
    if (inside->far >= minimum_distance && inside->far <= max_distance) {
        return RayHit{inside->far, at(ray, inside->far),
                      axis_normal(inside->far_axis,
                                  component(ray.direction, inside->far_axis))};
    }
    return std::nullopt;
}

std::optional<RayHit> raycast_primitive(const Ray& ray, Primitive primitive,
                                        float max_distance) noexcept {
    switch (primitive) {
    case Primitive::Plane:
        return unit_square(ray, 1U, max_distance);
    case Primitive::Quad:
        return unit_square(ray, 2U, max_distance);
    case Primitive::Box:
        return raycast_box(ray, Vec3{-0.5F, -0.5F, -0.5F},
                           Vec3{0.5F, 0.5F, 0.5F}, max_distance);
    case Primitive::Sphere:
        return unit_sphere(ray, max_distance);
    case Primitive::Cylinder:
        return unit_cylinder(ray, max_distance);
    }
    return std::nullopt;
}

MeshBvh build_mesh_bvh(const MeshData& mesh, std::stop_token stop) {
    MeshBvh result;
    const std::size_t count = mesh.indices.size() / 3U;
    if (count == 0U ||
        count > static_cast<std::size_t>(std::numeric_limits<core::u32>::max())) {
        return result;
    }
    std::vector<BuildTriangle> triangles(count);
    result.triangles.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        const core::u32 ia = mesh.indices[index * 3U];
        const core::u32 ib = mesh.indices[index * 3U + 1U];
        const core::u32 ic = mesh.indices[index * 3U + 2U];
        if (ia >= mesh.vertices.size() || ib >= mesh.vertices.size() ||
            ic >= mesh.vertices.size()) {
            continue;
        }
        const Vec3 a = mesh.vertices[ia].position;
        const Vec3 b = mesh.vertices[ib].position;
        const Vec3 c = mesh.vertices[ic].position;
        if (!(length(cross(b - a, c - a)) > 0.0F)) {
            continue;
        }
        BuildTriangle& built = triangles[index];
        built.minimum = minimum_of(a, minimum_of(b, c));
        built.maximum = maximum_of(a, maximum_of(b, c));
        built.centroid = (a + b + c) * (1.0F / 3.0F);
        result.triangles.push_back(static_cast<core::u32>(index));
    }
    if (result.triangles.empty()) {
        return MeshBvh{};
    }
    result.nodes.reserve(result.triangles.size() / 2U + 1U);
    BvhBuilder builder{std::move(triangles), result, stop};
    if (!builder.build(0U,
                       static_cast<core::u32>(result.triangles.size()))) {
        return MeshBvh{};
    }
    result.nodes.shrink_to_fit();
    result.triangles.shrink_to_fit();
    return result;
}

std::optional<RayHit> raycast_mesh(const MeshBvh& bvh, const MeshData& mesh,
                                   const Ray& ray,
                                   float max_distance) noexcept {
    if (bvh.empty()) {
        return std::nullopt;
    }
    float best = max_distance;
    std::optional<core::u32> best_triangle;
    std::array<core::u32, bvh_stack_depth> stack{};
    std::size_t depth = 0U;
    stack[depth++] = 0U;
    while (depth > 0U) {
        const MeshBvhNode& node = bvh.nodes[stack[--depth]];
        if (!box_reachable(ray, node.minimum, node.maximum, best)) {
            continue;
        }
        if (node.count > 0U) {
            for (core::u32 slot = node.first; slot < node.first + node.count;
                 ++slot) {
                const core::u32 number = bvh.triangles[slot];
                const auto hit = triangle(
                    ray, mesh.vertices[mesh.indices[number * 3U]].position,
                    mesh.vertices[mesh.indices[number * 3U + 1U]].position,
                    mesh.vertices[mesh.indices[number * 3U + 2U]].position,
                    best);
                if (hit.has_value() &&
                    (!best_triangle.has_value() || *hit < best ||
                     (*hit == best && number < *best_triangle))) {
                    best = *hit;
                    best_triangle = number;
                }
            }
            continue;
        }
        if (depth + 2U > stack.size()) {
            // A tree from build_mesh_bvh is balanced well inside the bound;
            // anything deeper is not one, and is not searched further.
            return std::nullopt;
        }
        const auto index = static_cast<core::u32>(&node - bvh.nodes.data());
        stack[depth++] = node.first;
        stack[depth++] = index + 1U;
    }
    if (!best_triangle.has_value()) {
        return std::nullopt;
    }
    const core::u32 number = *best_triangle;
    const Vec3 a = mesh.vertices[mesh.indices[number * 3U]].position;
    const Vec3 b = mesh.vertices[mesh.indices[number * 3U + 1U]].position;
    const Vec3 c = mesh.vertices[mesh.indices[number * 3U + 2U]].position;
    return RayHit{best, at(ray, best), facing(cross(b - a, c - a), ray.direction)};
}

bool mesh_triangles_near(const MeshBvh& bvh, const MeshData& mesh,
                         Vec3 centre, float radius,
                         std::vector<core::u32>& out, std::size_t limit) {
    if (bvh.empty()) {
        return true;
    }
    const Vec3 reach{radius, radius, radius};
    const Vec3 low = centre - reach;
    const Vec3 high = centre + reach;
    std::array<core::u32, bvh_stack_depth> stack{};
    std::size_t depth = 0U;
    stack[depth++] = 0U;
    std::size_t found = 0U;
    while (depth > 0U) {
        const core::u32 index = stack[--depth];
        const MeshBvhNode& node = bvh.nodes[index];
        if (node.maximum.x < low.x || node.minimum.x > high.x ||
            node.maximum.y < low.y || node.minimum.y > high.y ||
            node.maximum.z < low.z || node.minimum.z > high.z) {
            continue;
        }
        if (node.count > 0U) {
            for (core::u32 slot = node.first; slot < node.first + node.count;
                 ++slot) {
                const core::u32 number = bvh.triangles[slot];
                const Vec3 a = mesh.vertices[mesh.indices[number * 3U]].position;
                const Vec3 b =
                    mesh.vertices[mesh.indices[number * 3U + 1U]].position;
                const Vec3 c =
                    mesh.vertices[mesh.indices[number * 3U + 2U]].position;
                const Vec3 lowest = minimum_of(a, minimum_of(b, c));
                const Vec3 highest = maximum_of(a, maximum_of(b, c));
                if (highest.x < low.x || lowest.x > high.x ||
                    highest.y < low.y || lowest.y > high.y ||
                    highest.z < low.z || lowest.z > high.z) {
                    continue;
                }
                if (found == limit) {
                    return false;
                }
                out.push_back(number);
                ++found;
            }
            continue;
        }
        if (depth + 2U > stack.size()) {
            return false;
        }
        stack[depth++] = node.first;
        stack[depth++] = index + 1U;
    }
    return true;
}

std::optional<SurfaceHit> raycast_scene(const Scene& scene,
                                        const Ray& world_ray,
                                        const ModelRaycaster& models,
                                        float max_distance,
                                        const SurfaceFilter& counts) {
    std::optional<SurfaceHit> best;
    const auto consider = [&](doc::NodeId node, SurfaceSource source,
                              std::size_t index, const Mat4& world,
                              const auto& local_hit) {
        if (counts && !counts(node)) {
            return;
        }
        if (!(std::abs(determinant3(world)) > 1.0e-12F)) {
            // A zero scale draws nothing and has nothing to stand on.
            return;
        }
        const Mat4 inverse = invert_affine(world);
        const Ray local{transform_point(inverse, world_ray.origin),
                        transform_direction(inverse, world_ray.direction)};
        const float limit = best.has_value() ? best->distance : max_distance;
        const std::optional<RayHit> hit = local_hit(local, limit);
        if (!hit.has_value() || hit->distance > limit ||
            (best.has_value() && hit->distance == best->distance &&
             !(node < best->node))) {
            return;
        }
        best = SurfaceHit{node, source, index, hit->distance,
                          transform_point(world, hit->position),
                          facing(transform_normal(inverse, hit->normal),
                                 world_ray.direction)};
    };

    for (std::size_t index = 0U; index < scene.instances.size(); ++index) {
        const MeshInstance& object = scene.instances[index];
        consider(object.source, SurfaceSource::Instance, index, object.world,
                 [&](const Ray& local, float limit) {
                     return raycast_primitive(local, object.mesh.primitive,
                                              limit);
                 });
    }
    for (std::size_t index = 0U; index < scene.media.size(); ++index) {
        const MediaInstance& clip = scene.media[index];
        consider(clip.source_node, SurfaceSource::Media, index, clip.world,
                 [&](const Ray& local, float limit) {
                     return raycast_primitive(local, clip.mesh.primitive,
                                              limit);
                 });
    }
    for (std::size_t index = 0U; index < scene.models.size(); ++index) {
        const ModelInstance& model = scene.models[index];
        consider(model.source_node, SurfaceSource::Model, index, model.world,
                 [&](const Ray& local, float limit) -> std::optional<RayHit> {
                     if (models) {
                         return models(model, local, limit);
                     }
                     return raycast_primitive(local, Primitive::Box, limit);
                 });
    }
    return best;
}

} // namespace mediaserver::scene
