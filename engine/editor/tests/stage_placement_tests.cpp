#include <mediaserver/editor/model_surface_cache.hpp>
#include <mediaserver/editor/stage_placement.hpp>
#include <mediaserver/editor/transform_tool.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/raycast.hpp>

#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

// Where a Library asset dropped on the Stage lands, and the background
// hierarchies that let the Stage hit an imported model on its triangles. No
// window and no document: a surface hit and a camera in, a placement out.
namespace {

namespace doc = mediaserver::doc;
namespace editor = mediaserver::editor;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define PLACE_CHECK(...)                                                       \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] bool near(float a, float b, float tolerance = 1.0e-4F) noexcept {
    return std::fabs(a - b) < tolerance;
}

[[nodiscard]] bool near(scene::Vec3 a, scene::Vec3 b,
                        float tolerance = 1.0e-4F) noexcept {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) &&
           near(a.z, b.z, tolerance);
}

[[nodiscard]] scene::Camera camera() noexcept {
    scene::Camera view{};
    view.eye = scene::Vec3{0.0F, 4.0F, 6.0F};
    view.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    view.far_plane = 100.0F;
    return view;
}

// The world direction a placement turns the element's +Z (its face) to.
[[nodiscard]] scene::Vec3 facing_of(const editor::StagePlacement& placement) {
    const scene::Mat4 turned = scene::rotation(
        placement.rotation.x, placement.rotation.y, placement.rotation.z);
    return scene::Vec3{turned.at(0U, 2U), turned.at(1U, 2U), turned.at(2U, 2U)};
}

[[nodiscard]] scene::Ray ray_to(scene::Vec3 point) {
    const scene::Vec3 eye = camera().eye;
    return scene::Ray{eye, scene::normalize(point - eye)};
}

void a_screen_stands_on_a_set_piece_and_turns_to_the_camera() {
    // A platform top at y = 0.5.
    const scene::Vec3 contact{1.0F, 0.5F, -2.0F};
    editor::StagePlacementRequest request{};
    request.ray = ray_to(contact);
    request.hit = scene::SurfaceHit{doc::NodeId{31U}, scene::SurfaceSource::Instance, 0U, 6.0F, contact,
                                    scene::Vec3{0.0F, 1.0F, 0.0F}};
    request.camera = camera();
    request.media = true;
    request.bounds = editor::media_placement_bounds(2.0F);
    const auto placed = editor::place_on_stage(request);
    PLACE_CHECK(placed.has_value());
    PLACE_CHECK(placed->surface == editor::StagePlacementSurface::Element);
    PLACE_CHECK(placed->element == doc::NodeId{31U});
    // Its bottom edge (one metre below its centre) sits on the platform.
    PLACE_CHECK(near(placed->position, scene::Vec3{1.0F, 1.5F, -2.0F}));
    PLACE_CHECK(near(placed->rotation.y, 0.0F));
    PLACE_CHECK(near(placed->rotation.z, 0.0F));
    const scene::Vec3 toward = camera().eye - contact;
    PLACE_CHECK(near(facing_of(*placed),
                     scene::normalize(scene::Vec3{toward.x, 0.0F, toward.z})));
}

void a_screen_hangs_flat_on_a_wall_facing_out_of_it() {
    const scene::Vec3 contact{3.0F, 1.2F, -1.0F};
    const scene::Vec3 wall{-1.0F, 0.0F, 0.0F};
    editor::StagePlacementRequest request{};
    request.ray = ray_to(contact);
    request.hit = scene::SurfaceHit{doc::NodeId{5U}, scene::SurfaceSource::Instance, 0U, 5.0F, contact, wall};
    request.camera = camera();
    request.media = true;
    request.bounds = editor::media_placement_bounds(16.0F / 9.0F);
    // Snapping steps across the wall, never off it.
    request.snap = true;
    const auto placed = editor::place_on_stage(request);
    PLACE_CHECK(placed.has_value());
    PLACE_CHECK(near(facing_of(*placed), wall));
    PLACE_CHECK(near(placed->rotation.x, -0.25F));
    // A millimetre proud of the wall, at the nearest metre along it.
    PLACE_CHECK(near(placed->position, scene::Vec3{2.999F, 1.0F, -1.0F}));
}

void a_model_rests_its_lowest_point_on_the_floor_with_nothing_underneath() {
    editor::StagePlacementRequest request{};
    request.ray = ray_to(scene::Vec3{-2.0F, 0.0F, 1.0F});
    request.camera = camera();
    request.bounds = scene::ModelLocalBounds{scene::Vec3{-1.0F, -0.3F, -1.0F},
                                             scene::Vec3{1.0F, 2.0F, 1.0F}};
    const auto placed = editor::place_on_stage(request);
    PLACE_CHECK(placed.has_value());
    PLACE_CHECK(placed->surface == editor::StagePlacementSurface::Floor);
    PLACE_CHECK(near(placed->contact, scene::Vec3{-2.0F, 0.0F, 1.0F}, 1.0e-3F));
    PLACE_CHECK(near(placed->position, scene::Vec3{-2.0F, 0.3F, 1.0F}, 1.0e-3F));
    // Models keep their authored orientation.
    PLACE_CHECK(placed->rotation == scene::Vec3{});

    // Snapped, the contact steps to the metre grid across the floor.
    request.ray = ray_to(scene::Vec3{1.4F, 0.0F, -2.6F});
    request.snap = true;
    const auto snapped = editor::place_on_stage(request);
    PLACE_CHECK(snapped.has_value());
    PLACE_CHECK(near(snapped->position, scene::Vec3{1.0F, 0.3F, -3.0F}, 1.0e-3F));

    // A finer chosen step lands nearer the pointer.
    request.snap_move = 0.5F;
    const auto finer = editor::place_on_stage(request);
    PLACE_CHECK(finer.has_value());
    PLACE_CHECK(near(finer->position, scene::Vec3{1.5F, 0.3F, -2.5F}, 1.0e-3F));
}

void a_model_hangs_below_a_ceiling() {
    const scene::Vec3 contact{0.5F, 3.0F, -4.0F};
    editor::StagePlacementRequest request{};
    request.ray = scene::Ray{scene::Vec3{0.5F, 1.0F, -4.0F},
                             scene::Vec3{0.0F, 1.0F, 0.0F}};
    request.hit = scene::SurfaceHit{doc::NodeId{2U}, scene::SurfaceSource::Instance, 0U, 2.0F, contact,
                                    scene::Vec3{0.0F, -1.0F, 0.0F}};
    request.camera = camera();
    request.bounds = scene::ModelLocalBounds{scene::Vec3{-0.2F, -0.3F, -0.2F},
                                             scene::Vec3{0.2F, 1.2F, 0.2F}};
    const auto placed = editor::place_on_stage(request);
    PLACE_CHECK(placed.has_value());
    PLACE_CHECK(near(placed->position, scene::Vec3{0.5F, 1.8F, -4.0F}));
}

void with_no_floor_in_view_it_falls_back_to_the_point_of_interest_depth() {
    editor::StagePlacementRequest request{};
    request.camera = camera();
    request.bounds = scene::ModelLocalBounds{scene::Vec3{-1.0F, -1.0F, -1.0F},
                                             scene::Vec3{1.0F, 1.0F, 1.0F}};
    // Up into empty sky, still ahead of the camera.
    const scene::Vec3 forward =
        scene::normalize(camera().target - camera().eye);
    request.ray = scene::Ray{camera().eye,
                             scene::normalize(forward + scene::Vec3{0.0F, 1.2F, 0.0F})};
    const auto placed = editor::place_on_stage(request);
    PLACE_CHECK(placed.has_value());
    PLACE_CHECK(placed->surface == editor::StagePlacementSurface::ViewPlane);
    // On the plane through the target, facing the camera; no resting offset.
    PLACE_CHECK(near(scene::dot(placed->contact - camera().target, forward), 0.0F,
                     1.0e-3F));
    PLACE_CHECK(placed->position == placed->contact);

    // Floor beyond the far plane is not a floor.
    request.camera.far_plane = 1.0F;
    request.ray = ray_to(scene::Vec3{0.0F, 0.0F, -20.0F});
    const auto distant = editor::place_on_stage(request);
    PLACE_CHECK(distant.has_value());
    PLACE_CHECK(distant->surface == editor::StagePlacementSurface::ViewPlane);

    // Looking away from the point of interest there is nowhere to put it.
    request.ray = scene::Ray{camera().eye, -forward};
    PLACE_CHECK(!editor::place_on_stage(request).has_value());
}

[[nodiscard]] std::shared_ptr<const scene::MeshData> floor_tile(float y) {
    auto asset = std::make_shared<scene::MeshData>();
    asset->vertices = {
        scene::MeshVertex{scene::Vec3{-1.0F, y, -1.0F}},
        scene::MeshVertex{scene::Vec3{1.0F, y, -1.0F}},
        scene::MeshVertex{scene::Vec3{1.0F, y, 1.0F}},
        scene::MeshVertex{scene::Vec3{-1.0F, y, 1.0F}},
    };
    asset->indices = {0U, 1U, 2U, 0U, 2U, 3U};
    return asset;
}

void the_surface_cache_builds_in_the_background_and_never_on_lookup() {
    editor::ModelSurfaceCache cache;
    const auto tile = floor_tile(0.25F);
    // The first look queues the build and answers nothing.
    PLACE_CHECK(cache.observe(tile) == nullptr);
    PLACE_CHECK(editor::detail::wait_for_model_surface_cache_for_test(cache));
    const auto bvh = cache.observe(tile);
    PLACE_CHECK(bvh != nullptr);
    PLACE_CHECK(bvh->triangles.size() == 2U);
    const auto hit = scene::raycast_mesh(
        *bvh, *tile,
        scene::Ray{scene::Vec3{0.5F, 3.0F, 0.5F}, scene::Vec3{0.0F, -1.0F, 0.0F}});
    PLACE_CHECK(hit.has_value());
    PLACE_CHECK(near(hit->position.y, 0.25F));
    // Further looks reuse it.
    PLACE_CHECK(cache.observe(tile) == bvh);
    PLACE_CHECK(cache.stats().builds == 1U);
    PLACE_CHECK(cache.stats().ready == 1U);

    // A refreshed model is a new asset with its own hierarchy.
    const auto refreshed = floor_tile(0.75F);
    PLACE_CHECK(cache.observe(refreshed) == nullptr);
    PLACE_CHECK(editor::detail::wait_for_model_surface_cache_for_test(cache));
    PLACE_CHECK(cache.observe(refreshed) != nullptr);
    PLACE_CHECK(cache.stats().builds == 2U);

    // The project boundary drops them all, on the worker.
    cache.request_reset();
    PLACE_CHECK(editor::detail::wait_for_model_surface_cache_for_test(cache));
    PLACE_CHECK(cache.stats().ready == 0U);
    PLACE_CHECK(cache.stats().bytes == 0U);
    PLACE_CHECK(cache.observe(tile) == nullptr);
}

void the_surface_cache_keeps_to_its_budget_and_its_queue() {
    // Room for one tile's hierarchy, not two.
    const auto one = floor_tile(0.0F);
    const std::size_t one_size = scene::build_mesh_bvh(*one).bytes();
    editor::ModelSurfaceCache cache{editor::ModelSurfaceCacheConfig{1U, one_size + one_size / 2U}};
    const auto other = floor_tile(1.0F);
    PLACE_CHECK(cache.observe(one) == nullptr);
    // The queue is full: this look waits for a later frame.
    PLACE_CHECK(cache.observe(other) == nullptr);
    PLACE_CHECK(editor::detail::wait_for_model_surface_cache_for_test(cache));
    PLACE_CHECK(cache.stats().builds == 1U);
    PLACE_CHECK(cache.observe(one) != nullptr);
    PLACE_CHECK(cache.observe(other) == nullptr);
    PLACE_CHECK(editor::detail::wait_for_model_surface_cache_for_test(cache));
    // The newest stays; the least recently used went to make room.
    PLACE_CHECK(cache.observe(other) != nullptr);
    PLACE_CHECK(cache.stats().evictions == 1U);
    PLACE_CHECK(cache.stats().ready == 1U);
    PLACE_CHECK(cache.stats().bytes <= one_size + one_size / 2U);

    // One hierarchy bigger than the whole budget is never kept, nor rebuilt
    // on every look.
    editor::ModelSurfaceCache tiny{editor::ModelSurfaceCacheConfig{4U, 1U}};
    PLACE_CHECK(tiny.observe(one) == nullptr);
    PLACE_CHECK(editor::detail::wait_for_model_surface_cache_for_test(tiny));
    PLACE_CHECK(tiny.observe(one) == nullptr);
    PLACE_CHECK(editor::detail::wait_for_model_surface_cache_for_test(tiny));
    PLACE_CHECK(tiny.stats().builds == 1U);
}

struct Case final {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const std::array cases{
        Case{"a screen stands on a set piece and turns to the camera",
             a_screen_stands_on_a_set_piece_and_turns_to_the_camera},
        Case{"a screen hangs flat on a wall facing out of it",
             a_screen_hangs_flat_on_a_wall_facing_out_of_it},
        Case{"a model rests its lowest point on the floor with nothing underneath",
             a_model_rests_its_lowest_point_on_the_floor_with_nothing_underneath},
        Case{"a model hangs below a ceiling", a_model_hangs_below_a_ceiling},
        Case{"with no floor in view it falls back to the point of interest depth",
             with_no_floor_in_view_it_falls_back_to_the_point_of_interest_depth},
        Case{"the surface cache builds in the background and never on lookup",
             the_surface_cache_builds_in_the_background_and_never_on_lookup},
        Case{"the surface cache keeps to its budget and its queue",
             the_surface_cache_keeps_to_its_budget_and_its_queue},
    };
    int failures = 0;
    for (const Case& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
