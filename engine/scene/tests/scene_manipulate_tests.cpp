#include <mediaserver/scene/manipulate.hpp>

#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
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

#define MANIP_CHECK(...)                                                       \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// A camera looking down -Z at the origin, the same shape the demo uses.
[[nodiscard]] scene::Camera front_camera() noexcept {
    scene::Camera camera{};
    camera.eye = scene::Vec3{0.0F, 0.0F, 5.0F};
    camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    camera.field_of_view = 0.125F;
    camera.near_plane = 0.1F;
    camera.far_plane = 100.0F;
    return camera;
}

constexpr scene::Canvas canvas{1'920U, 1'080U};

void a_horizontal_drag_moves_along_camera_right() {
    const auto camera = front_camera();
    const scene::Vec3 point{0.0F, 0.0F, 0.0F};

    const auto rightward =
        scene::screen_drag_to_translation(camera, canvas, point, 50.0F, 0.0F);
    // Camera right is +X here. A rightward drag moves along it and nowhere
    // else.
    MANIP_CHECK(rightward.x > 0.0F);
    MANIP_CHECK(std::abs(rightward.y) < 1.0e-4F);
    MANIP_CHECK(std::abs(rightward.z) < 1.0e-4F);

    // "Camera left" is the mirror.
    const auto leftward =
        scene::screen_drag_to_translation(camera, canvas, point, -50.0F, 0.0F);
    MANIP_CHECK(std::abs(leftward.x + rightward.x) < 1.0e-5F);
}

void a_vertical_drag_moves_along_camera_up() {
    const auto camera = front_camera();
    const scene::Vec3 point{0.0F, 0.0F, 0.0F};

    // Screen y runs down, so a downward drag (positive) moves the point down.
    const auto downward =
        scene::screen_drag_to_translation(camera, canvas, point, 0.0F, 40.0F);
    MANIP_CHECK(downward.y < 0.0F);
    MANIP_CHECK(std::abs(downward.x) < 1.0e-4F);
    MANIP_CHECK(std::abs(downward.z) < 1.0e-4F);
}

void a_drag_is_depth_locked() {
    const auto camera = front_camera();
    // Two points, one twice as far from the eye as the other along the view.
    const scene::Vec3 near_point{0.0F, 0.0F, 2.5F}; // depth 2.5
    const scene::Vec3 far_point{0.0F, 0.0F, 0.0F};  // depth 5.0

    const auto near_move = scene::screen_drag_to_translation(
        camera, canvas, near_point, 60.0F, 0.0F);
    const auto far_move = scene::screen_drag_to_translation(
        camera, canvas, far_point, 60.0F, 0.0F);
    // The farther point moves twice as far in world space for the same drag,
    // which is what keeps a drag feeling like it tracks the cursor at any
    // depth.
    MANIP_CHECK(std::abs(far_move.x - 2.0F * near_move.x) < 1.0e-4F);

    // A point at or behind the eye has no image plane to act on.
    const scene::Vec3 behind{0.0F, 0.0F, 6.0F}; // depth -1
    const auto none =
        scene::screen_drag_to_translation(camera, canvas, behind, 60.0F, 0.0F);
    MANIP_CHECK(none == (scene::Vec3{}));
}

void a_drag_then_reproject_lands_under_the_cursor() {
    // The property that makes it feel direct: move by the drag's translation
    // and the point reprojects exactly where the cursor went. Because the move
    // is parallel to the image plane at the point's depth, the depth is
    // unchanged and the mapping is one pixel of drag to one pixel of screen
    // motion.
    const auto camera = front_camera();
    const scene::Vec3 point{1.0F, 0.5F, 0.0F};

    const auto before = scene::project_to_canvas(camera, canvas, point);
    MANIP_CHECK(before.visible);

    const float drag_x = 120.0F;
    const float drag_y = -80.0F;
    const auto delta = scene::screen_drag_to_translation(camera, canvas, point,
                                                         drag_x, drag_y);
    const auto after = scene::project_to_canvas(camera, canvas, point + delta);
    MANIP_CHECK(after.visible);

    MANIP_CHECK(std::abs(after.x - (before.x + drag_x)) < 1.0F);
    MANIP_CHECK(std::abs(after.y - (before.y + drag_y)) < 1.0F);
}

[[nodiscard]] scene::MediaInstance clip_at(scene::Vec3 position, float size) {
    scene::MediaInstance clip{};
    clip.mesh = scene::MeshKey{scene::Primitive::Quad, 1U};
    clip.world = scene::multiply(scene::translation(position),
                                 scene::scaling(scene::Vec3{size, size, 1.0F}));
    clip.source = "clip_####.png";
    clip.source_node = doc::NodeId{7U};
    return clip;
}

void picking_hits_the_quad_under_the_point_and_misses_outside() {
    scene::Scene scn{};
    scn.camera = front_camera();
    scn.media.push_back(clip_at(scene::Vec3{0.0F, 0.0F, 0.0F}, 2.0F));

    // The quad is centred, so the canvas centre is on it.
    const auto* const hit =
        scene::pick_media(scn, canvas, static_cast<float>(canvas.width) / 2.0F,
                          static_cast<float>(canvas.height) / 2.0F);
    MANIP_CHECK(hit != nullptr);
    MANIP_CHECK(hit->source_node == doc::NodeId{7U});

    // A corner of the canvas is well outside the quad's projection.
    MANIP_CHECK(scene::pick_media(scn, canvas, 10.0F, 10.0F) == nullptr);

    // And media_for round-trips a selection back to its instance.
    MANIP_CHECK(scene::media_for(scn, doc::NodeId{7U}) != nullptr);
    MANIP_CHECK(scene::media_for(scn, doc::NodeId{8U}) == nullptr);
}

void picking_prefers_the_nearer_of_two_overlapping_clips() {
    scene::Scene scn{};
    scn.camera = front_camera();
    // A far clip and a near one, both centred and overlapping the centre.
    auto far_clip = clip_at(scene::Vec3{0.0F, 0.0F, 0.0F}, 2.0F);
    far_clip.source_node = doc::NodeId{1U};
    auto near_clip = clip_at(scene::Vec3{0.0F, 0.0F, 2.0F}, 2.0F);
    near_clip.source_node = doc::NodeId{2U};
    scn.media.push_back(far_clip);
    scn.media.push_back(near_clip);

    const auto* const hit =
        scene::pick_media(scn, canvas, static_cast<float>(canvas.width) / 2.0F,
                          static_cast<float>(canvas.height) / 2.0F);
    MANIP_CHECK(hit != nullptr);
    // The nearer clip (closer to the eye at z=5) wins.
    MANIP_CHECK(hit->source_node == doc::NodeId{2U});
}

void all_scene_picks_are_depth_sorted_and_the_nearest_api_agrees() {
    scene::Scene scn{};
    scn.canvas = canvas;
    scn.camera = front_camera();
    auto far_clip = clip_at(scene::Vec3{0.0F, 0.0F, -1.0F}, 2.0F);
    far_clip.source_node = doc::NodeId{3U};
    auto middle_clip = clip_at(scene::Vec3{0.0F, 0.0F, 0.0F}, 2.0F);
    middle_clip.source_node = doc::NodeId{2U};
    auto near_clip = clip_at(scene::Vec3{0.0F, 0.0F, 1.0F}, 2.0F);
    near_clip.source_node = doc::NodeId{1U};
    scn.media = {far_clip, near_clip, middle_clip};

    const float x = static_cast<float>(canvas.width) * 0.5F;
    const float y = static_cast<float>(canvas.height) * 0.5F;
    const std::vector<scene::ScenePick> hits =
        scene::pick_scene_all(scn, canvas, x, y);
    MANIP_CHECK(hits.size() == 3U);
    MANIP_CHECK(hits[0].node == near_clip.source_node);
    MANIP_CHECK(hits[1].node == middle_clip.source_node);
    MANIP_CHECK(hits[2].node == far_clip.source_node);
    const auto nearest = scene::pick_scene(scn, canvas, x, y);
    MANIP_CHECK(nearest.has_value());
    MANIP_CHECK(nearest.value() == hits.front());
}

void scene_picking_hits_objects_models_and_point_lights() {
    scene::Scene scn{};
    scn.canvas = canvas;
    scn.camera = front_camera();

    scene::MeshInstance object{};
    object.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    object.world = scene::translation(scene::Vec3{-1.5F, 0.0F, 0.0F});
    object.source = doc::NodeId{11U};
    scn.instances.push_back(object);

    scene::ModelInstance model{};
    model.world = scene::translation(scene::Vec3{1.5F, 0.0F, 0.0F});
    model.source_node = doc::NodeId{12U};
    scn.models.push_back(model);

    scene::PointLight light{};
    light.position = scene::Vec3{0.0F, 1.5F, 0.0F};
    light.source = doc::NodeId{13U};
    scn.point_lights.push_back(light);

    const auto object_screen = scene::project_to_canvas(
        scn.camera, canvas, scene::Vec3{-1.5F, 0.0F, 0.0F});
    const auto model_screen = scene::project_to_canvas(
        scn.camera, canvas, scene::Vec3{1.5F, 0.0F, 0.0F});
    const auto light_screen =
        scene::project_to_canvas(scn.camera, canvas, light.position);

    const auto object_hit =
        scene::pick_scene(scn, canvas, object_screen.x, object_screen.y);
    const auto model_hit =
        scene::pick_scene(scn, canvas, model_screen.x, model_screen.y);
    const auto light_hit =
        scene::pick_scene(scn, canvas, light_screen.x, light_screen.y);
    MANIP_CHECK(object_hit.has_value());
    MANIP_CHECK(object_hit->node == doc::NodeId{11U});
    MANIP_CHECK(model_hit.has_value());
    MANIP_CHECK(model_hit->node == doc::NodeId{12U});
    MANIP_CHECK(light_hit.has_value());
    MANIP_CHECK(light_hit->node == doc::NodeId{13U});
    MANIP_CHECK(!scene::pick_scene(scn, canvas, light_screen.x, light_screen.y,
                                   12.0F, false)
                     .has_value());
}

void imported_model_bounds_extend_picking_beyond_the_unit_proxy() {
    scene::Scene scn{};
    scn.canvas = canvas;
    scn.camera = front_camera();
    scene::ModelInstance model{};
    model.world = scene::identity();
    model.source = "offset-model.obj";
    model.source_node = doc::NodeId{18U};
    scn.models.push_back(model);

    const scene::Vec3 inside_imported_bounds{1.5F, 0.0F, 0.0F};
    const auto projected = scene::project_to_canvas(
        scn.camera, canvas, inside_imported_bounds);
    MANIP_CHECK(projected.visible);

    // The compatibility API still sees the historical [-.5, .5] proxy.
    MANIP_CHECK(!scene::pick_scene(scn, canvas, projected.x, projected.y,
                                   12.0F, false)
                     .has_value());

    const scene::ModelBoundsProvider imported_bounds =
        [](const scene::ModelInstance& instance)
        -> std::optional<scene::ModelLocalBounds> {
        if (instance.source == "offset-model.obj") {
            return scene::ModelLocalBounds{scene::Vec3{1.0F, -0.4F, -0.2F},
                                            scene::Vec3{2.0F, 0.4F, 0.2F}};
        }
        return std::nullopt;
    };
    const auto hits = scene::pick_scene_all(
        scn, canvas, projected.x, projected.y, imported_bounds, 12.0F, false);
    MANIP_CHECK(hits.size() == 1U);
    MANIP_CHECK(hits.front().node == model.source_node);
    const auto nearest = scene::pick_scene(
        scn, canvas, projected.x, projected.y, imported_bounds, 12.0F, false);
    MANIP_CHECK(nearest.has_value());
    MANIP_CHECK(nearest->node == model.source_node);
}

void scene_picking_prefers_nearer_geometry_over_farther_geometry() {
    scene::Scene scn{};
    scn.canvas = canvas;
    scn.camera = front_camera();

    scene::MeshInstance far_object{};
    far_object.world = scene::translation(scene::Vec3{0.0F, 0.0F, -1.0F});
    far_object.source = doc::NodeId{20U};
    scene::MeshInstance near_object{};
    near_object.world = scene::translation(scene::Vec3{0.0F, 0.0F, 1.0F});
    near_object.source = doc::NodeId{21U};
    scn.instances.push_back(far_object);
    scn.instances.push_back(near_object);

    const auto hit =
        scene::pick_scene(scn, canvas, static_cast<float>(canvas.width) * 0.5F,
                          static_cast<float>(canvas.height) * 0.5F);
    MANIP_CHECK(hit.has_value());
    MANIP_CHECK(hit->node == doc::NodeId{21U});
}

void a_directional_light_has_a_pickable_editor_anchor() {
    scene::Scene scn{};
    scn.canvas = canvas;
    scn.camera = front_camera();
    scene::DirectionalLight light{};
    light.direction = scene::Vec3{0.0F, -1.0F, -0.2F};
    light.source = doc::NodeId{30U};
    scn.directional_lights.push_back(light);

    const scene::Vec3 anchor = scene::directional_light_anchor(scn, light);
    const auto projected = scene::project_to_canvas(scn.camera, canvas, anchor);
    MANIP_CHECK(projected.visible);
    const auto hit = scene::pick_scene(scn, canvas, projected.x, projected.y);
    MANIP_CHECK(hit.has_value());
    MANIP_CHECK(hit->node == light.source);
    scn.camera.target = {12.0F, 0.0F, 0.0F};
    scn.camera.eye = {12.0F, 0.0F, 5.0F};
    MANIP_CHECK(scene::directional_light_anchor(scn, light) == anchor);
    const auto turned = scene::project_to_canvas(scn.camera, canvas, anchor);
    MANIP_CHECK(turned.x < 0.0F || turned.x > canvas.width ||
                turned.y < 0.0F || turned.y > canvas.height);
    MANIP_CHECK(!scene::pick_scene(scn, canvas, projected.x, projected.y));
}

void locked_elements_are_absent_from_editor_picking() {
    scene::Scene scn{};
    scn.canvas = canvas;
    scn.camera = front_camera();
    auto far_clip = clip_at(scene::Vec3{0.0F, 0.0F, 0.0F}, 2.0F);
    far_clip.source_node = doc::NodeId{40U};
    auto locked_near = clip_at(scene::Vec3{0.0F, 0.0F, 2.0F}, 2.0F);
    locked_near.source_node = doc::NodeId{41U};
    locked_near.locked = true;
    scn.media = {far_clip, locked_near};

    const float x = static_cast<float>(canvas.width) * 0.5F;
    const float y = static_cast<float>(canvas.height) * 0.5F;
    const auto* const media = scene::pick_media(scn, canvas, x, y);
    MANIP_CHECK(media != nullptr);
    MANIP_CHECK(media->source_node == far_clip.source_node);
    const auto hits = scene::pick_scene_all(scn, canvas, x, y);
    MANIP_CHECK(hits.size() == 1U);
    MANIP_CHECK(hits.front().node == far_clip.source_node);
    MANIP_CHECK(scene::element_locked(scn, locked_near.source_node));
    MANIP_CHECK(!scene::element_locked(scn, far_clip.source_node));
}

// A floor the camera stands over reaches behind it. It is still found under
// the pointer - clipped at the near plane - so a deep select can offer it, but
// it is marked as crossing the camera and never wins a plain click, which on
// Stage belongs to the orbit. A box standing on it lists in front of it.
void an_element_crossing_the_camera_is_listed_but_never_plainly_picked() {
    scene::Scene scn{};
    scn.canvas = canvas;
    scn.camera = front_camera();
    scn.camera.eye = scene::Vec3{0.0F, 1.5F, 5.0F};
    scn.camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};

    scene::MeshInstance floor{};
    floor.source = doc::NodeId{50U};
    floor.mesh = scene::MeshKey{scene::Primitive::Plane, 1U};
    floor.world = scene::multiply(
        scene::translation(scene::Vec3{0.0F, -0.5F, 0.0F}),
        scene::scaling(scene::Vec3{40.0F, 1.0F, 40.0F}));
    scn.instances.push_back(floor);
    scene::MeshInstance box{};
    box.source = doc::NodeId{51U};
    box.mesh = scene::MeshKey{scene::Primitive::Box, 1U};
    box.world = scene::identity();
    scn.instances.push_back(box);

    const auto centre = scene::project_to_canvas(scn.camera, canvas,
                                                 scene::Vec3{0.0F, 0.0F, 0.0F});
    MANIP_CHECK(centre.visible);
    const auto hits = scene::pick_scene_all(scn, canvas, centre.x, centre.y);
    MANIP_CHECK(hits.size() == 2U);
    MANIP_CHECK(hits[0].node == box.source);
    MANIP_CHECK(!hits[0].crosses_camera);
    MANIP_CHECK(hits[1].node == floor.source);
    MANIP_CHECK(hits[1].crosses_camera);
    const auto nearest = scene::pick_scene(scn, canvas, centre.x, centre.y);
    MANIP_CHECK(nearest.has_value() && nearest->node == box.source);

    // Bare floor: listed, never plainly picked.
    const auto beside = scene::project_to_canvas(
        scn.camera, canvas, scene::Vec3{2.5F, -0.5F, 1.0F});
    MANIP_CHECK(beside.visible);
    const auto floor_hits = scene::pick_scene_all(scn, canvas, beside.x, beside.y);
    MANIP_CHECK(floor_hits.size() == 1U);
    MANIP_CHECK(floor_hits[0].node == floor.source);
    MANIP_CHECK(floor_hits[0].crosses_camera);
    MANIP_CHECK(!scene::pick_scene(scn, canvas, beside.x, beside.y).has_value());

    // A projector alone knows no camera to clip at: the floor stays absent.
    const scene::SceneProjector projector = [&scn](scene::Vec3 point) noexcept {
        return scene::project_to_canvas(scn.camera, canvas, point);
    };
    MANIP_CHECK(scene::pick_scene_all(scn, beside.x, beside.y, projector).empty());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 13U> cases{{
    {"a horizontal drag moves along camera right",
     &a_horizontal_drag_moves_along_camera_right},
    {"a vertical drag moves along camera up",
     &a_vertical_drag_moves_along_camera_up},
    {"a drag is depth-locked", &a_drag_is_depth_locked},
    {"a drag then reproject lands under the cursor",
     &a_drag_then_reproject_lands_under_the_cursor},
    {"picking hits the quad under the point and misses outside",
     &picking_hits_the_quad_under_the_point_and_misses_outside},
    {"picking prefers the nearer of two overlapping clips",
     &picking_prefers_the_nearer_of_two_overlapping_clips},
    {"all scene picks are depth sorted and the nearest api agrees",
     &all_scene_picks_are_depth_sorted_and_the_nearest_api_agrees},
    {"scene picking hits objects models and point lights",
     &scene_picking_hits_objects_models_and_point_lights},
    {"imported model bounds extend picking beyond the unit proxy",
     &imported_model_bounds_extend_picking_beyond_the_unit_proxy},
    {"scene picking prefers nearer geometry over farther geometry",
     &scene_picking_prefers_nearer_geometry_over_farther_geometry},
    {"a directional light has a pickable editor anchor",
     &a_directional_light_has_a_pickable_editor_anchor},
    {"locked elements are absent from editor picking",
     &locked_elements_are_absent_from_editor_picking},
    {"an element crossing the camera is listed but never plainly picked",
     &an_element_crossing_the_camera_is_listed_but_never_plainly_picked},
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
