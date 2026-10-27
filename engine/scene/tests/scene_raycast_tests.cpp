#include <mediaserver/scene/raycast.hpp>

#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>

namespace {

namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define RAY_CHECK(...)                                                         \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] bool near(float a, float b, float tolerance = 1.0e-4F) {
    return std::abs(a - b) <= tolerance;
}

[[nodiscard]] bool near(scene::Vec3 a, scene::Vec3 b,
                        float tolerance = 1.0e-4F) {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) &&
           near(a.z, b.z, tolerance);
}

// Looking down at the floor from above and in front, as an operator does.
[[nodiscard]] scene::Camera stage_camera() noexcept {
    scene::Camera camera{};
    camera.eye = scene::Vec3{0.0F, 4.0F, 6.0F};
    camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    camera.field_of_view = 0.125F;
    camera.near_plane = 0.1F;
    camera.far_plane = 100.0F;
    return camera;
}

constexpr scene::Canvas canvas{1'920U, 1'080U};

void a_canvas_ray_passes_through_the_point_drawn_at_that_pixel() {
    const scene::Camera camera = stage_camera();
    for (const scene::Vec3 point :
         {scene::Vec3{0.0F, 0.0F, 0.0F}, scene::Vec3{1.25F, 0.5F, -2.0F},
          scene::Vec3{-3.0F, 1.75F, 1.5F}}) {
        const auto projected = scene::project_to_canvas(camera, canvas, point);
        RAY_CHECK(projected.visible);
        const auto ray =
            scene::canvas_ray(camera, canvas, projected.x, projected.y);
        RAY_CHECK(ray.has_value());
        RAY_CHECK(near(scene::length(ray->direction), 1.0F));
        RAY_CHECK(near(ray->origin, camera.eye));
        // The point lies on the ray: its offset from the eye is parallel.
        const scene::Vec3 offset = point - camera.eye;
        const float along = scene::dot(offset, ray->direction);
        RAY_CHECK(near(ray->origin + ray->direction * along, point, 1.0e-3F));
    }
    RAY_CHECK(!scene::canvas_ray(camera, scene::Canvas{}, 1.0F, 1.0F));
}

void a_box_is_hit_on_its_near_face_or_from_inside_on_its_far_wall() {
    const scene::Ray outside{scene::Vec3{0.25F, 3.0F, 0.0F},
                             scene::Vec3{0.0F, -1.0F, 0.0F}};
    const auto top = scene::raycast_box(outside, scene::Vec3{-1.0F, -1.0F, -1.0F},
                                        scene::Vec3{1.0F, 1.0F, 1.0F});
    RAY_CHECK(top.has_value());
    RAY_CHECK(near(top->distance, 2.0F));
    RAY_CHECK(near(top->position, scene::Vec3{0.25F, 1.0F, 0.0F}));
    RAY_CHECK(near(top->normal, scene::Vec3{0.0F, 1.0F, 0.0F}));

    // Standing inside a room looking at its +X wall: the wall faces back in.
    const scene::Ray inside{scene::Vec3{0.0F, 0.0F, 0.0F},
                            scene::Vec3{1.0F, 0.0F, 0.0F}};
    const auto wall = scene::raycast_box(inside, scene::Vec3{-2.0F, -2.0F, -2.0F},
                                         scene::Vec3{3.0F, 2.0F, 2.0F});
    RAY_CHECK(wall.has_value());
    RAY_CHECK(near(wall->distance, 3.0F));
    RAY_CHECK(near(wall->normal, scene::Vec3{-1.0F, 0.0F, 0.0F}));

    // Beyond the limit and pointing away both miss.
    RAY_CHECK(!scene::raycast_box(outside, scene::Vec3{-1.0F, -1.0F, -1.0F},
                                  scene::Vec3{1.0F, 1.0F, 1.0F}, 1.5F));
    const scene::Ray away{scene::Vec3{0.0F, 3.0F, 0.0F},
                          scene::Vec3{0.0F, 1.0F, 0.0F}};
    RAY_CHECK(!scene::raycast_box(away, scene::Vec3{-1.0F, -1.0F, -1.0F},
                                  scene::Vec3{1.0F, 1.0F, 1.0F}));
}

void each_primitive_is_hit_where_build_mesh_puts_its_surface() {
    const scene::Ray down{scene::Vec3{0.1F, 2.0F, 0.2F},
                          scene::Vec3{0.0F, -1.0F, 0.0F}};
    const auto plane = scene::raycast_primitive(down, scene::Primitive::Plane);
    RAY_CHECK(plane.has_value());
    RAY_CHECK(near(plane->distance, 2.0F));
    RAY_CHECK(near(plane->normal, scene::Vec3{0.0F, 1.0F, 0.0F}));
    // The floor plane is seen from below too, facing the ray.
    const scene::Ray up{scene::Vec3{0.1F, -2.0F, 0.2F},
                        scene::Vec3{0.0F, 1.0F, 0.0F}};
    const auto under = scene::raycast_primitive(up, scene::Primitive::Plane);
    RAY_CHECK(under.has_value());
    RAY_CHECK(near(under->normal, scene::Vec3{0.0F, -1.0F, 0.0F}));
    // A square has edges.
    const scene::Ray beside{scene::Vec3{0.75F, 2.0F, 0.0F},
                            scene::Vec3{0.0F, -1.0F, 0.0F}};
    RAY_CHECK(!scene::raycast_primitive(beside, scene::Primitive::Plane));

    // A Quad stands up facing +Z; a downward ray slides past it edge-on.
    RAY_CHECK(!scene::raycast_primitive(down, scene::Primitive::Quad));
    const scene::Ray toward{scene::Vec3{0.2F, -0.3F, 4.0F},
                            scene::Vec3{0.0F, 0.0F, -1.0F}};
    const auto quad = scene::raycast_primitive(toward, scene::Primitive::Quad);
    RAY_CHECK(quad.has_value());
    RAY_CHECK(near(quad->position, scene::Vec3{0.2F, -0.3F, 0.0F}));
    RAY_CHECK(near(quad->normal, scene::Vec3{0.0F, 0.0F, 1.0F}));

    const auto sphere =
        scene::raycast_primitive(toward, scene::Primitive::Sphere);
    RAY_CHECK(sphere.has_value());
    const float depth = std::sqrt(0.25F - 0.2F * 0.2F - 0.3F * 0.3F);
    RAY_CHECK(near(sphere->position.z, depth));
    RAY_CHECK(near(scene::length(sphere->position), 0.5F));
    RAY_CHECK(scene::dot(sphere->normal, toward.direction) < 0.0F);

    // Cylinder: the side from the front, the cap from above.
    const auto side =
        scene::raycast_primitive(toward, scene::Primitive::Cylinder);
    RAY_CHECK(side.has_value());
    RAY_CHECK(near(side->normal.y, 0.0F));
    RAY_CHECK(near(side->position.z, std::sqrt(0.25F - 0.04F)));
    const auto cap = scene::raycast_primitive(down, scene::Primitive::Cylinder);
    RAY_CHECK(cap.has_value());
    RAY_CHECK(near(cap->distance, 1.5F));
    RAY_CHECK(near(cap->normal, scene::Vec3{0.0F, 1.0F, 0.0F}));
}

// Every triangle, nearest first: what the hierarchy must agree with.
[[nodiscard]] std::optional<float> brute_force(const scene::MeshData& mesh,
                                               const scene::Ray& ray) {
    std::optional<float> best;
    for (std::size_t index = 0U; index + 2U < mesh.indices.size();
         index += 3U) {
        scene::MeshData one;
        one.vertices = {mesh.vertices[mesh.indices[index]],
                        mesh.vertices[mesh.indices[index + 1U]],
                        mesh.vertices[mesh.indices[index + 2U]]};
        one.indices = {0U, 1U, 2U};
        const scene::MeshBvh single = scene::build_mesh_bvh(one);
        if (const auto hit = scene::raycast_mesh(single, one, ray)) {
            if (!best.has_value() || hit->distance < *best) {
                best = hit->distance;
            }
        }
    }
    return best;
}

void a_mesh_hierarchy_finds_the_same_nearest_triangle_as_every_triangle() {
    scene::MeshData sphere;
    RAY_CHECK(scene::build_mesh(scene::MeshKey{scene::Primitive::Sphere, 24U},
                                sphere));
    const scene::MeshBvh bvh = scene::build_mesh_bvh(sphere);
    RAY_CHECK(!bvh.empty());
    // Pole fans have zero-area slivers, which are left out.
    RAY_CHECK(bvh.triangles.size() <= sphere.indices.size() / 3U);
    RAY_CHECK(bvh.triangles.size() * 10U > sphere.indices.size() / 3U * 9U);
    RAY_CHECK(bvh.bytes() > 0U);

    std::size_t hits = 0U;
    for (int row = -6; row <= 6; ++row) {
        for (int column = -6; column <= 6; ++column) {
            const scene::Ray ray{
                scene::Vec3{static_cast<float>(column) * 0.09F,
                            static_cast<float>(row) * 0.09F + 0.013F, 3.0F},
                scene::Vec3{0.05F, -0.02F, -1.0F}};
            const auto fast = scene::raycast_mesh(bvh, sphere, ray);
            const auto slow = brute_force(sphere, ray);
            RAY_CHECK(fast.has_value() == slow.has_value());
            if (fast.has_value()) {
                ++hits;
                RAY_CHECK(near(fast->distance, *slow, 1.0e-5F));
                RAY_CHECK(scene::dot(fast->normal, ray.direction) < 0.0F);
                // The tessellated sphere sits just inside the true one.
                RAY_CHECK(scene::length(fast->position) <= 0.5001F);
                RAY_CHECK(scene::length(fast->position) > 0.48F);
            }
        }
    }
    RAY_CHECK(hits > 60U);
    RAY_CHECK(hits < 169U);
}

void a_mesh_hierarchy_skips_unusable_triangles_and_honours_a_stop() {
    scene::MeshData mesh;
    mesh.vertices = {
        scene::MeshVertex{scene::Vec3{-1.0F, 0.0F, -1.0F}},
        scene::MeshVertex{scene::Vec3{1.0F, 0.0F, -1.0F}},
        scene::MeshVertex{scene::Vec3{0.0F, 0.0F, 1.0F}},
    };
    // One good triangle, one degenerate, one indexing past the buffer.
    mesh.indices = {0U, 1U, 2U, 0U, 0U, 1U, 0U, 1U, 7U};
    const scene::MeshBvh bvh = scene::build_mesh_bvh(mesh);
    RAY_CHECK(bvh.triangles.size() == 1U);
    const scene::Ray down{scene::Vec3{0.0F, 2.0F, 0.0F},
                          scene::Vec3{0.0F, -1.0F, 0.0F}};
    const auto hit = scene::raycast_mesh(bvh, mesh, down);
    RAY_CHECK(hit.has_value());
    RAY_CHECK(near(hit->distance, 2.0F));
    RAY_CHECK(near(hit->normal, scene::Vec3{0.0F, 1.0F, 0.0F}));

    std::stop_source stop;
    stop.request_stop();
    RAY_CHECK(scene::build_mesh_bvh(mesh, stop.get_token()).empty());
    RAY_CHECK(!scene::raycast_mesh(scene::MeshBvh{}, mesh, down));
}

void the_scene_reports_the_nearest_surface_and_its_world_normal() {
    scene::Scene scn{};
    // A wide floor slab: a box scaled 10 x 0.2 x 10, its top at y = 0.
    scene::MeshInstance floor{};
    floor.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    floor.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, -0.1F, 0.0F}),
        scene::scaling(scene::Vec3{10.0F, 0.2F, 10.0F}));
    floor.source = doc::NodeId{41U};
    floor.locked = true;
    scn.instances.push_back(floor);

    // A screen standing on it, turned a quarter to face +X.
    scene::MediaInstance screen{};
    screen.world = scene::multiply(
        scene::translation(scene::Vec3{2.0F, 1.0F, 0.0F}),
        scene::multiply(scene::rotation(0.25F, 0.0F, 0.0F),
                        scene::scaling(scene::Vec3{3.0F, 2.0F, 1.0F})));
    screen.source_node = doc::NodeId{42U};
    scn.media.push_back(screen);

    // Straight down: the locked floor still counts.
    const scene::Ray down{scene::Vec3{-3.0F, 5.0F, 1.0F},
                          scene::Vec3{0.0F, -1.0F, 0.0F}};
    const auto on_floor = scene::raycast_scene(scn, down);
    RAY_CHECK(on_floor.has_value());
    RAY_CHECK(on_floor->node == doc::NodeId{41U});
    RAY_CHECK(near(on_floor->position, scene::Vec3{-3.0F, 0.0F, 1.0F}));
    RAY_CHECK(near(on_floor->normal, scene::Vec3{0.0F, 1.0F, 0.0F}));
    RAY_CHECK(near(on_floor->distance, 5.0F));

    // Across the floor towards the screen from +X: the screen is nearer.
    const scene::Ray across{scene::Vec3{6.0F, 1.5F, 0.4F},
                            scene::Vec3{-1.0F, 0.0F, 0.0F}};
    const auto on_screen = scene::raycast_scene(scn, across);
    RAY_CHECK(on_screen.has_value());
    RAY_CHECK(on_screen->node == doc::NodeId{42U});
    RAY_CHECK(near(on_screen->position, scene::Vec3{2.0F, 1.5F, 0.4F}));
    RAY_CHECK(near(on_screen->normal, scene::Vec3{1.0F, 0.0F, 0.0F}));
    RAY_CHECK(near(on_screen->distance, 4.0F));

    // A limit short of everything finds nothing.
    RAY_CHECK(!scene::raycast_scene(scn, down, {}, 4.0F));
}

void a_model_uses_its_raycaster_and_otherwise_its_unit_proxy() {
    scene::Scene scn{};
    scene::ModelInstance model{};
    model.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, 1.0F, 0.0F}),
        scene::scaling(scene::Vec3{2.0F, 2.0F, 2.0F}));
    model.source = "set.glb";
    model.source_node = doc::NodeId{7U};
    scn.models.push_back(model);
    const scene::Ray down{scene::Vec3{0.0F, 10.0F, 0.0F},
                          scene::Vec3{0.0F, -1.0F, 0.0F}};

    // No raycaster: the unit cube, scaled to 2 m, so its top is at y = 2.
    const auto proxy = scene::raycast_scene(scn, down);
    RAY_CHECK(proxy.has_value());
    RAY_CHECK(near(proxy->position.y, 2.0F));

    // A raycaster answers in local space; distances stay world metres.
    std::size_t calls = 0U;
    const scene::ModelRaycaster raycaster =
        [&](const scene::ModelInstance& asked, const scene::Ray& local,
            float limit) -> std::optional<scene::RayHit> {
        ++calls;
        RAY_CHECK(asked.source == "set.glb");
        RAY_CHECK(near(local.origin, scene::Vec3{0.0F, 4.5F, 0.0F}));
        // Local plane y = 0.25 (world y = 1.5), sloped normal.
        const float distance = (0.25F - local.origin.y) / local.direction.y;
        if (distance > limit) {
            return std::nullopt;
        }
        return scene::RayHit{distance, local.origin + local.direction * distance,
                             scene::normalize(scene::Vec3{0.0F, 1.0F, 1.0F})};
    };
    const auto exact = scene::raycast_scene(scn, down, raycaster);
    RAY_CHECK(calls == 1U);
    RAY_CHECK(exact.has_value());
    RAY_CHECK(near(exact->position, scene::Vec3{0.0F, 1.5F, 0.0F}));
    RAY_CHECK(near(exact->distance, 8.5F));
    RAY_CHECK(near(exact->normal, scene::normalize(scene::Vec3{0.0F, 1.0F, 1.0F})));
}

void non_uniform_scale_keeps_the_normal_perpendicular_to_the_surface() {
    scene::Scene scn{};
    // A unit sphere stretched 4x along X: a long ellipsoid.
    scene::MeshInstance object{};
    object.mesh = scene::MeshKey{scene::Primitive::Sphere, 16U};
    object.world = scene::scaling(scene::Vec3{4.0F, 1.0F, 1.0F});
    object.source = doc::NodeId{3U};
    scn.instances.push_back(object);
    const scene::Ray down{scene::Vec3{1.0F, 5.0F, 0.0F},
                          scene::Vec3{0.0F, -1.0F, 0.0F}};
    const auto hit = scene::raycast_scene(scn, down);
    RAY_CHECK(hit.has_value());
    // On x^2/4 + y^2/0.25 = 1 (semi-axes 2 and 0.5), whose gradient
    // (x/4, 4y, 0) is the true normal.
    const scene::Vec3 p = hit->position;
    RAY_CHECK(near(p.x * p.x / 4.0F + p.y * p.y / 0.25F, 1.0F, 1.0e-3F));
    const scene::Vec3 expected =
        scene::normalize(scene::Vec3{p.x / 4.0F, p.y / 0.25F, 0.0F});
    RAY_CHECK(near(hit->normal, expected, 1.0e-3F));
}

void equal_distances_go_to_the_lower_node_and_zero_scale_is_skipped() {
    scene::Scene scn{};
    scene::MeshInstance second{};
    second.mesh = scene::MeshKey{scene::Primitive::Plane, 1U};
    second.world = scene::identity();
    second.source = doc::NodeId{9U};
    scene::MeshInstance first = second;
    first.source = doc::NodeId{4U};
    scene::MeshInstance flat = second;
    flat.source = doc::NodeId{1U};
    flat.world = scene::multiply(scene::translation(scene::Vec3{0.0F, 1.0F, 0.0F}),
                                 scene::scaling(scene::Vec3{0.0F, 1.0F, 1.0F}));
    scn.instances = {flat, second, first};
    const scene::Ray down{scene::Vec3{0.0F, 3.0F, 0.0F},
                          scene::Vec3{0.0F, -1.0F, 0.0F}};
    const auto hit = scene::raycast_scene(scn, down);
    RAY_CHECK(hit.has_value());
    RAY_CHECK(hit->node == doc::NodeId{4U});
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 9U> cases{{
    {"a canvas ray passes through the point drawn at that pixel",
     &a_canvas_ray_passes_through_the_point_drawn_at_that_pixel},
    {"a box is hit on its near face or from inside on its far wall",
     &a_box_is_hit_on_its_near_face_or_from_inside_on_its_far_wall},
    {"each primitive is hit where build_mesh puts its surface",
     &each_primitive_is_hit_where_build_mesh_puts_its_surface},
    {"a mesh hierarchy finds the same nearest triangle as every triangle",
     &a_mesh_hierarchy_finds_the_same_nearest_triangle_as_every_triangle},
    {"a mesh hierarchy skips unusable triangles and honours a stop",
     &a_mesh_hierarchy_skips_unusable_triangles_and_honours_a_stop},
    {"the scene reports the nearest surface and its world normal",
     &the_scene_reports_the_nearest_surface_and_its_world_normal},
    {"a model uses its raycaster and otherwise its unit proxy",
     &a_model_uses_its_raycaster_and_otherwise_its_unit_proxy},
    {"non-uniform scale keeps the normal perpendicular to the surface",
     &non_uniform_scale_keeps_the_normal_perpendicular_to_the_surface},
    {"equal distances go to the lower node and zero scale is skipped",
     &equal_distances_go_to_the_lower_node_and_zero_scale_is_skipped},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
