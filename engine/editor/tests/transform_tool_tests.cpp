#include <mediaserver/editor/transform_tool.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/math.hpp>

#include <array>
#include <cmath>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

// The Stage transform tools' arithmetic, with no window and no document: where
// a rotate or scale about a pivot leaves each element, how yaw/pitch/roll is
// recovered from a turned basis, and the pointer geometry the gizmos and the
// G/R/S keyboard transforms share.
namespace {

namespace editor = mediaserver::editor;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define TOOL_CHECK(...)                                                        \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] bool near(float a, float b, float tolerance = 1.0e-4F) noexcept {
    return std::fabs(a - b) < tolerance;
}

[[nodiscard]] bool near(scene::Vec3 a, scene::Vec3 b,
                        float tolerance = 1.0e-4F) noexcept {
    return near(a.x, b.x, tolerance) && near(a.y, b.y, tolerance) &&
           near(a.z, b.z, tolerance);
}

[[nodiscard]] std::array<scene::Vec3, 3U> columns(const scene::Mat4& m) {
    std::array<scene::Vec3, 3U> out{};
    for (mediaserver::core::u32 c = 0U; c < 3U; ++c) {
        out[c] = scene::Vec3{m.at(0U, c), m.at(1U, c), m.at(2U, c)};
    }
    return out;
}

void snapping_rounds_to_the_nearest_step() {
    TOOL_CHECK(near(editor::snap_to_step(1.26F, 0.5F), 1.5F));
    TOOL_CHECK(near(editor::snap_to_step(-0.74F, 0.5F), -0.5F));
    TOOL_CHECK(near(editor::snap_to_step(13.0F / 360.0F,
                                         editor::rotate_snap_step_turns),
                    15.0F / 360.0F));
    // A step of zero leaves the value alone rather than dividing by it.
    TOOL_CHECK(near(editor::snap_to_step(0.37F, 0.0F), 0.37F));
}

void yaw_pitch_roll_is_recovered_from_a_turned_basis() {
    const std::array<scene::Vec3, 5U> cases{
        scene::Vec3{0.10F, 0.05F, -0.20F}, scene::Vec3{0.40F, -0.15F, 0.30F},
        scene::Vec3{-0.30F, 0.20F, 0.45F}, scene::Vec3{0.0F, 0.0F, 0.125F},
        scene::Vec3{0.75F, 0.0F, 0.0F}};
    for (const scene::Vec3 ypr : cases) {
        const auto axes = columns(scene::rotation(ypr.x, ypr.y, ypr.z));
        const scene::Vec3 recovered = editor::rotation_turns_of(axes, ypr);
        // Asked for the angles nearest the originals, it returns them.
        TOOL_CHECK(near(recovered, ypr, 1.0e-3F));
    }

    // Past vertical the other solution (yaw and roll a half turn round) is
    // nearer the starting angles, so it wins and the key does not flip.
    const scene::Vec3 start{0.5F, 0.0F, 0.5F};
    const auto flipped = columns(scene::rotation(0.0F, 0.5F - 0.1F, 0.0F));
    const scene::Vec3 settled = editor::rotation_turns_of(flipped, start);
    const auto rebuilt = columns(scene::rotation(settled.x, settled.y,
                                                 settled.z));
    for (std::size_t c = 0U; c < 3U; ++c) {
        TOOL_CHECK(near(rebuilt[c], flipped[c], 1.0e-3F));
    }
    TOOL_CHECK(near(settled.x, 0.5F, 1.0e-3F));
    TOOL_CHECK(near(settled.z, 0.5F, 1.0e-3F));

    // Straight up (gimbal lock) keeps the starting roll and still rebuilds
    // the same orientation.
    const scene::Vec3 locked_start{0.1F, 0.25F, 0.05F};
    const auto locked = columns(scene::rotation(0.1F, 0.25F, 0.05F));
    const scene::Vec3 unlocked = editor::rotation_turns_of(locked, locked_start);
    TOOL_CHECK(near(unlocked.z, 0.05F, 1.0e-3F));
    const auto relocked = columns(scene::rotation(unlocked.x, unlocked.y,
                                                  unlocked.z));
    for (std::size_t c = 0U; c < 3U; ++c) {
        TOOL_CHECK(near(relocked[c], locked[c], 1.0e-3F));
    }
}

void a_rotation_about_world_up_turns_yaw_and_orbits_about_the_pivot() {
    editor::TransformStart start{};
    start.local_position = scene::Vec3{2.0F, 0.0F, 0.0F};
    start.world_position = scene::Vec3{2.0F, 0.0F, 0.0F};

    editor::TransformDelta delta{};
    delta.tool = editor::TransformTool::Rotate;
    delta.axis = scene::Vec3{0.0F, 1.0F, 0.0F};
    delta.turns = 0.25F;
    delta.pivot = scene::Vec3{};

    const editor::TransformResult result = editor::apply_transform(start, delta);
    // Right-handed about +Y: +X swings to -Z.
    TOOL_CHECK(near(result.position, scene::Vec3{0.0F, 0.0F, -2.0F}));
    TOOL_CHECK(near(result.rotation, scene::Vec3{0.25F, 0.0F, 0.0F}));
    TOOL_CHECK(near(result.scale, scene::Vec3{1.0F, 1.0F, 1.0F}));

    // An element on the pivot only turns.
    start.world_position = delta.pivot;
    start.local_position = scene::Vec3{0.3F, 0.4F, 0.5F};
    const editor::TransformResult spun = editor::apply_transform(start, delta);
    TOOL_CHECK(near(spun.position, start.local_position));
}

void a_rotation_composes_with_the_existing_orientation() {
    editor::TransformStart start{};
    start.rotation = scene::Vec3{0.1F, 0.0F, 0.0F};
    editor::TransformDelta delta{};
    delta.tool = editor::TransformTool::Rotate;
    // About world X: a pitch on top of the existing yaw, which yaw/pitch/roll
    // cannot express as one added component.
    delta.axis = scene::Vec3{1.0F, 0.0F, 0.0F};
    delta.turns = 0.1F;
    const editor::TransformResult result = editor::apply_transform(start, delta);

    const auto expected_before = columns(scene::rotation(0.1F, 0.0F, 0.0F));
    const auto got = columns(scene::rotation(result.rotation.x,
                                             result.rotation.y,
                                             result.rotation.z));
    for (std::size_t c = 0U; c < 3U; ++c) {
        const scene::Vec3 expected = scene::rotate_around(
            expected_before[c], delta.axis, delta.turns);
        TOOL_CHECK(near(got[c], expected, 1.0e-4F));
    }
}

void a_parent_rotation_maps_world_gestures_into_local_values() {
    // A parent turned a quarter about +Y: world +X is the child's local +Z.
    const scene::Mat4 parent = scene::rotation(0.25F, 0.0F, 0.0F);
    editor::TransformStart start{};
    start.local_position = scene::Vec3{1.0F, 0.0F, 0.0F};
    start.world_position = scene::transform_point(parent, start.local_position);
    start.world_to_local = scene::invert_affine(parent);

    editor::TransformDelta move{};
    move.tool = editor::TransformTool::Move;
    move.translation = scene::Vec3{1.0F, 0.0F, 0.0F};
    const editor::TransformResult moved = editor::apply_transform(start, move);
    TOOL_CHECK(near(moved.position, scene::Vec3{1.0F, 0.0F, 1.0F}));

    // Turning about world X is turning about local Z here.
    editor::TransformDelta turn{};
    turn.tool = editor::TransformTool::Rotate;
    turn.axis = scene::Vec3{1.0F, 0.0F, 0.0F};
    turn.turns = 0.125F;
    turn.pivot = start.world_position;
    const editor::TransformResult turned = editor::apply_transform(start, turn);
    const auto axes = columns(scene::rotation(turned.rotation.x,
                                              turned.rotation.y,
                                              turned.rotation.z));
    TOOL_CHECK(near(axes[2U], scene::Vec3{0.0F, 0.0F, 1.0F}, 1.0e-4F));
    TOOL_CHECK(near(turned.position, start.local_position));
}

void scale_reaches_each_elements_own_axes_and_spreads_about_the_pivot() {
    editor::TransformStart start{};
    start.local_position = scene::Vec3{1.0F, 0.0F, 0.0F};
    start.world_position = scene::Vec3{1.0F, 0.0F, 0.0F};
    start.scale = scene::Vec3{2.0F, 3.0F, 4.0F};

    editor::TransformDelta delta{};
    delta.tool = editor::TransformTool::Scale;
    delta.factors = scene::Vec3{2.0F, 1.0F, 1.0F};
    const editor::TransformResult wider = editor::apply_transform(start, delta);
    TOOL_CHECK(near(wider.position, scene::Vec3{2.0F, 0.0F, 0.0F}));
    TOOL_CHECK(near(wider.scale, scene::Vec3{4.0F, 3.0F, 4.0F}));

    // The same world-X stretch on an element turned a quarter about +Y lands
    // on its own Z axis, which is the one lying along world X.
    const auto turned = columns(scene::rotation(0.25F, 0.0F, 0.0F));
    start.world_axes = turned;
    const editor::TransformResult deeper = editor::apply_transform(start, delta);
    TOOL_CHECK(near(deeper.scale, scene::Vec3{2.0F, 3.0F, 8.0F}));

    // A negative factor mirrors rather than losing its sign.
    start.world_axes = {scene::Vec3{1.0F, 0.0F, 0.0F},
                        scene::Vec3{0.0F, 1.0F, 0.0F},
                        scene::Vec3{0.0F, 0.0F, 1.0F}};
    delta.factors = scene::Vec3{-1.0F, 1.0F, 1.0F};
    const editor::TransformResult mirrored = editor::apply_transform(start, delta);
    TOOL_CHECK(near(mirrored.scale, scene::Vec3{-2.0F, 3.0F, 4.0F}));
    TOOL_CHECK(near(mirrored.position, scene::Vec3{-1.0F, 0.0F, 0.0F}));
}

void a_whole_world_matrix_turns_and_stretches_like_the_fields_do() {
    // A linked object's placement: turned, unevenly sized, off the pivot.
    const scene::Mat4 world = scene::multiply(
        scene::translation(scene::Vec3{2.0F, 0.0F, 0.0F}),
        scene::multiply(scene::rotation(0.1F, 0.05F, 0.0F),
                        scene::scaling(scene::Vec3{1.0F, 2.0F, 3.0F})));
    editor::TransformStart start{};
    start.rotation = scene::Vec3{0.1F, 0.05F, 0.0F};
    start.scale = scene::Vec3{1.0F, 2.0F, 3.0F};
    start.local_position = scene::Vec3{2.0F, 0.0F, 0.0F};
    start.world_position = start.local_position;
    const auto axes = columns(world);
    for (std::size_t c = 0U; c < 3U; ++c) {
        start.world_axes[c] = scene::normalize(axes[c]);
    }

    editor::TransformDelta turn{};
    turn.tool = editor::TransformTool::Rotate;
    turn.axis = scene::Vec3{0.0F, 1.0F, 0.0F};
    turn.turns = 0.2F;
    editor::TransformDelta stretch{};
    stretch.tool = editor::TransformTool::Scale;
    stretch.factors = scene::Vec3{1.5F, 0.5F, 2.0F};

    // The matrix and the yaw/pitch/roll-and-scale fields agree on the result.
    for (const editor::TransformDelta& delta : {turn, stretch}) {
        const scene::Mat4 got = editor::transformed_world(world, delta);
        const editor::TransformResult fields =
            editor::apply_transform(start, delta);
        const scene::Mat4 expected = scene::multiply(
            scene::translation(fields.position),
            scene::multiply(scene::rotation(fields.rotation.x,
                                            fields.rotation.y,
                                            fields.rotation.z),
                            scene::scaling(fields.scale)));
        for (std::size_t i = 0U; i < 16U; ++i) {
            TOOL_CHECK(near(got.m[i], expected.m[i], 1.0e-3F));
        }
    }
}

[[nodiscard]] scene::Camera oblique_camera() {
    scene::Camera camera{};
    camera.eye = scene::Vec3{3.0F, 4.0F, 9.0F};
    camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    camera.field_of_view = 0.14F;
    return camera;
}

void the_pointer_ray_passes_through_what_projects_there() {
    const scene::Camera camera = oblique_camera();
    const scene::Canvas canvas{1920U, 1080U};
    const scene::Vec3 point{1.5F, -0.5F, 0.75F};
    const auto projected = scene::project_to_canvas(camera, canvas, point);
    TOOL_CHECK(projected.visible);
    const scene::Vec3 ray =
        editor::pointer_ray(camera, canvas, projected.x, projected.y);
    TOOL_CHECK(scene::dot(scene::normalize(ray),
                          scene::normalize(point - camera.eye)) > 0.999999F);
}

void a_plane_drag_keeps_the_grabbed_point_under_the_pointer() {
    const scene::Camera camera = oblique_camera();
    const scene::Canvas canvas{1920U, 1080U};
    const scene::Vec3 from{0.5F, 0.0F, -0.25F};
    const scene::Vec3 to{-1.25F, 0.0F, 1.5F};
    const auto a = scene::project_to_canvas(camera, canvas, from);
    const auto b = scene::project_to_canvas(camera, canvas, to);
    const auto moved = editor::plane_drag_translation(
        camera, canvas, scene::Vec3{}, scene::Vec3{0.0F, 1.0F, 0.0F}, a.x, a.y,
        b.x, b.y);
    TOOL_CHECK(moved.has_value());
    TOOL_CHECK(near(*moved, to - from, 1.0e-3F));

    // A plane seen edge-on has no stable point to keep; it says so.
    const auto edge_on = editor::plane_drag_translation(
        camera, canvas, scene::Vec3{},
        scene::cross(scene::normalize(camera.target - camera.eye),
                     scene::Vec3{0.0F, 1.0F, 0.0F}),
        960.0F, 540.0F, 990.0F, 540.0F);
    TOOL_CHECK(!edge_on.has_value());
}

void a_sweep_turns_the_way_the_pointer_circles() {
    const scene::Vec3 forward{0.0F, 0.0F, -1.0F};
    const scene::Vec3 toward_viewer{0.0F, 0.0F, 1.0F};
    // Right of centre to above it (y runs down): a quarter anticlockwise on
    // screen, a positive turn about an axis facing the viewer.
    TOOL_CHECK(near(editor::sweep_turns(100.0F, 100.0F, 150.0F, 100.0F,
                                        100.0F, 50.0F, toward_viewer, forward),
                    0.25F));
    // The same sweep about an axis facing away is negative.
    TOOL_CHECK(near(editor::sweep_turns(100.0F, 100.0F, 150.0F, 100.0F,
                                        100.0F, 50.0F, -toward_viewer, forward),
                    -0.25F));
    // Crossing the screen's -x side wraps rather than jumping a whole turn.
    // (-10, 1) to (-10, -1) is a small clockwise-on-screen step past the
    // atan2 seam: a small negative turn, not nearly a whole one.
    const float seam_step =
        -2.0F * std::atan2(1.0F, 10.0F) / (2.0F * 3.14159265F);
    TOOL_CHECK(near(editor::sweep_turns(0.0F, 0.0F, -10.0F, 1.0F, -10.0F, -1.0F,
                                        toward_viewer, forward),
                    seam_step));
}

void typed_values_read_like_blender() {
    editor::NumericEntry entry;
    TOOL_CHECK(!entry.active());
    TOOL_CHECK(!entry.value().has_value());
    TOOL_CHECK(entry.type("1.5"));
    TOOL_CHECK(near(*entry.value(), 1.5F));
    // Minus toggles the sign wherever it is typed; a second point is ignored.
    TOOL_CHECK(entry.type("-.2"));
    TOOL_CHECK(near(*entry.value(), -1.52F));
    TOOL_CHECK(entry.display() == "-1.52");
    entry.backspace();
    TOOL_CHECK(near(*entry.value(), -1.5F));
    // Letters are not numbers; they belong to the axis keys.
    TOOL_CHECK(!entry.type("x"));
    entry.clear();
    TOOL_CHECK(!entry.active());
}

} // namespace

int main() {
    struct Case final {
        const char* name;
        void (*run)();
    };
    const std::array cases{
        Case{"snapping_rounds_to_the_nearest_step",
             snapping_rounds_to_the_nearest_step},
        Case{"yaw_pitch_roll_is_recovered_from_a_turned_basis",
             yaw_pitch_roll_is_recovered_from_a_turned_basis},
        Case{"a_rotation_about_world_up_turns_yaw_and_orbits_about_the_pivot",
             a_rotation_about_world_up_turns_yaw_and_orbits_about_the_pivot},
        Case{"a_rotation_composes_with_the_existing_orientation",
             a_rotation_composes_with_the_existing_orientation},
        Case{"a_parent_rotation_maps_world_gestures_into_local_values",
             a_parent_rotation_maps_world_gestures_into_local_values},
        Case{"scale_reaches_each_elements_own_axes_and_spreads_about_the_pivot",
             scale_reaches_each_elements_own_axes_and_spreads_about_the_pivot},
        Case{"a_whole_world_matrix_turns_and_stretches_like_the_fields_do",
             a_whole_world_matrix_turns_and_stretches_like_the_fields_do},
        Case{"the_pointer_ray_passes_through_what_projects_there",
             the_pointer_ray_passes_through_what_projects_there},
        Case{"a_plane_drag_keeps_the_grabbed_point_under_the_pointer",
             a_plane_drag_keeps_the_grabbed_point_under_the_pointer},
        Case{"a_sweep_turns_the_way_the_pointer_circles",
             a_sweep_turns_the_way_the_pointer_circles},
        Case{"typed_values_read_like_blender", typed_values_read_like_blender},
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
