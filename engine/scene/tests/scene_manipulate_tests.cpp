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
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define MANIP_CHECK(...) \
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
    // Camera right is +X here. A rightward drag moves along it and nowhere else.
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

    const auto near_move =
        scene::screen_drag_to_translation(camera, canvas, near_point, 60.0F, 0.0F);
    const auto far_move =
        scene::screen_drag_to_translation(camera, canvas, far_point, 60.0F, 0.0F);
    // The farther point moves twice as far in world space for the same drag,
    // which is what keeps a drag feeling like it tracks the cursor at any depth.
    MANIP_CHECK(std::abs(far_move.x - 2.0F * near_move.x) < 1.0e-4F);

    // A point at or behind the eye has no image plane to act on.
    const scene::Vec3 behind{0.0F, 0.0F, 6.0F}; // depth -1
    const auto none =
        scene::screen_drag_to_translation(camera, canvas, behind, 60.0F, 0.0F);
    MANIP_CHECK(none == (scene::Vec3{}));
}

void a_drag_then_reproject_lands_under_the_cursor() {
    // The property that makes it feel direct: move by the drag's translation and
    // the point reprojects exactly where the cursor went. Because the move is
    // parallel to the image plane at the point's depth, the depth is unchanged
    // and the mapping is one pixel of drag to one pixel of screen motion.
    const auto camera = front_camera();
    const scene::Vec3 point{1.0F, 0.5F, 0.0F};

    const auto before = scene::project_to_canvas(camera, canvas, point);
    MANIP_CHECK(before.visible);

    const float drag_x = 120.0F;
    const float drag_y = -80.0F;
    const auto delta =
        scene::screen_drag_to_translation(camera, canvas, point, drag_x, drag_y);
    const auto after =
        scene::project_to_canvas(camera, canvas, point + delta);
    MANIP_CHECK(after.visible);

    MANIP_CHECK(std::abs(after.x - (before.x + drag_x)) < 1.0F);
    MANIP_CHECK(std::abs(after.y - (before.y + drag_y)) < 1.0F);
}

[[nodiscard]] scene::MediaInstance clip_at(scene::Vec3 position, float size) {
    scene::MediaInstance clip{};
    clip.mesh = scene::MeshKey{scene::Primitive::Quad, 1U};
    clip.world = scene::multiply(
        scene::translation(position),
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
    const auto* const hit = scene::pick_media(
        scn,
        canvas,
        static_cast<float>(canvas.width) / 2.0F,
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

    const auto* const hit = scene::pick_media(
        scn,
        canvas,
        static_cast<float>(canvas.width) / 2.0F,
        static_cast<float>(canvas.height) / 2.0F);
    MANIP_CHECK(hit != nullptr);
    // The nearer clip (closer to the eye at z=5) wins.
    MANIP_CHECK(hit->source_node == doc::NodeId{2U});
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 6U> cases{{
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
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
