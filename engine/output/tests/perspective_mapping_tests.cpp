#include <mediaserver/output/perspective_mapping.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

namespace core = mediaserver::core;
namespace output = mediaserver::output;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define PERSPECTIVE_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr scene::Vec3 eye{0.0F, 0.0F, 0.0F};

[[nodiscard]] output::PhysicalSurface front_surface() noexcept {
    return output::PhysicalSurface{
        scene::Vec3{0.0F, 0.0F, -4.0F},
        scene::Vec3{1.0F, 0.0F, 0.0F},
        scene::Vec3{0.0F, 1.0F, 0.0F},
        8.0F,
        4.5F,
        scene::Canvas{3'840U, 2'160U},
    };
}

[[nodiscard]] scene::Vec4 clip_point(const output::PerspectiveView& view,
                                     scene::Vec3 point) noexcept {
    const auto clip = scene::transform(
        view.matrices.view_projection,
        scene::Vec4{point.x, point.y, point.z, 1.0F});
    return scene::Vec4{
        clip.x / clip.w, clip.y / clip.w, clip.z / clip.w, clip.w};
}

void a_front_surface_builds_the_expected_off_axis_window() {
    const auto surface = front_surface();
    auto built = output::perspective_view_for(
        surface, scene::whole(surface.raster), eye, 0.1F, 1'000.0F);
    PERSPECTIVE_CHECK(built.has_value());
    const auto& view = built.value();

    PERSPECTIVE_CHECK(view.eye_distance == 4.0F);
    PERSPECTIVE_CHECK(view.frustum.left == -0.1F);
    PERSPECTIVE_CHECK(view.frustum.right == 0.1F);
    PERSPECTIVE_CHECK(std::abs(view.frustum.bottom + 0.05625F) < 1.0e-7F);
    PERSPECTIVE_CHECK(std::abs(view.frustum.top - 0.05625F) < 1.0e-7F);

    const auto lower_left = clip_point(view, view.corners.lower_left);
    const auto lower_right = clip_point(view, view.corners.lower_right);
    const auto upper_left = clip_point(view, view.corners.upper_left);
    const auto upper_right = clip_point(view, view.corners.upper_right);
    PERSPECTIVE_CHECK(std::abs(lower_left.x + 1.0F) < 1.0e-6F);
    PERSPECTIVE_CHECK(std::abs(lower_left.y + 1.0F) < 1.0e-6F);
    PERSPECTIVE_CHECK(std::abs(lower_right.x - 1.0F) < 1.0e-6F);
    PERSPECTIVE_CHECK(std::abs(lower_right.y + 1.0F) < 1.0e-6F);
    PERSPECTIVE_CHECK(std::abs(upper_left.x + 1.0F) < 1.0e-6F);
    PERSPECTIVE_CHECK(std::abs(upper_left.y - 1.0F) < 1.0e-6F);
    PERSPECTIVE_CHECK(std::abs(upper_right.x - 1.0F) < 1.0e-6F);
    PERSPECTIVE_CHECK(std::abs(upper_right.y - 1.0F) < 1.0e-6F);
}

void left_and_right_walls_look_into_their_side_of_the_scene() {
    // Their X axes point away from the front wall's centre seam, while each
    // front normal points toward the common stage origin. The generated view
    // directions must consequently be -X and +X without authored cameras.
    const std::array<output::PhysicalSurface, 2U> surfaces{{
        output::PhysicalSurface{
            scene::Vec3{-4.0F, 0.0F, 0.0F},
            scene::Vec3{0.0F, 0.0F, -1.0F},
            scene::Vec3{0.0F, 1.0F, 0.0F},
            8.0F,
            4.5F,
            scene::Canvas{3'840U, 2'160U}},
        output::PhysicalSurface{
            scene::Vec3{4.0F, 0.0F, 0.0F},
            scene::Vec3{0.0F, 0.0F, 1.0F},
            scene::Vec3{0.0F, 1.0F, 0.0F},
            8.0F,
            4.5F,
            scene::Canvas{3'840U, 2'160U}},
    }};

    for (const auto& surface : surfaces) {
        auto built = output::perspective_view_for(
            surface, scene::whole(surface.raster), eye, 0.1F, 100.0F);
        PERSPECTIVE_CHECK(built.has_value());
        const auto& view = built.value();
        PERSPECTIVE_CHECK(view.eye_distance == 4.0F);

        // The physical centre lands in the centre of its own raster.
        const auto projected =
            output::project_through_surface(view, surface.center);
        PERSPECTIVE_CHECK(projected.visible);
        PERSPECTIVE_CHECK(
            std::abs(projected.x - 1'920.0F) < 1.0e-3F);
        PERSPECTIVE_CHECK(
            std::abs(projected.y - 1'080.0F) < 1.0e-3F);

        // World-space view direction is from the eye to the wall centre. A
        // point twice as far along it remains at raster centre.
        const auto direction = scene::normalize(surface.center - eye);
        const auto deeper = output::project_through_surface(
            view, surface.center + direction * 3.0F);
        PERSPECTIVE_CHECK(deeper.visible);
        PERSPECTIVE_CHECK(std::abs(deeper.x - projected.x) < 1.0e-3F);
        PERSPECTIVE_CHECK(std::abs(deeper.y - projected.y) < 1.0e-3F);
    }

    // Reversing a surface's authored handedness exposes its back and is
    // diagnosed, rather than silently mirroring the content.
    auto backwards = surfaces[0U];
    backwards.right = -backwards.right;
    const auto rejected = output::perspective_view_for(
        backwards, scene::whole(backwards.raster), eye, 0.1F, 100.0F);
    PERSPECTIVE_CHECK(!rejected.has_value());
    PERSPECTIVE_CHECK(
        rejected.error() == output::PerspectiveMappingError::EyeNotInFront);
}

void adjacent_ports_share_bit_identical_physical_and_frustum_edges() {
    const auto surface = front_surface();
    constexpr scene::CanvasRect left{0U, 0U, 1'237U, 2'160U};
    constexpr scene::CanvasRect right{
        1'237U, 0U, 3'840U - 1'237U, 2'160U};

    auto left_view = output::perspective_view_for(
        surface, left, eye, 0.1F, 1'000.0F);
    auto right_view = output::perspective_view_for(
        surface, right, eye, 0.1F, 1'000.0F);
    PERSPECTIVE_CHECK(left_view.has_value() && right_view.has_value());

    // Equality, not a tolerance. Both ports derive the same physical edge from
    // absolute raster column 1237, and the same near-plane expression from it.
    PERSPECTIVE_CHECK(
        left_view.value().corners.lower_right
        == right_view.value().corners.lower_left);
    PERSPECTIVE_CHECK(
        left_view.value().corners.upper_right
        == right_view.value().corners.upper_left);
    PERSPECTIVE_CHECK(
        left_view.value().frustum.right
        == right_view.value().frustum.left);

    // Any world point projects to the same full-surface pixel through either
    // partition, even though each view uses a different off-axis matrix.
    constexpr scene::Vec3 sample{0.25F, 0.3F, -9.0F};
    const auto through_left =
        output::project_through_surface(left_view.value(), sample);
    const auto through_right =
        output::project_through_surface(right_view.value(), sample);
    PERSPECTIVE_CHECK(through_left.visible && through_right.visible);
    PERSPECTIVE_CHECK(
        std::abs(through_left.x - through_right.x) < 1.0e-3F);
    PERSPECTIVE_CHECK(
        std::abs(through_left.y - through_right.y) < 1.0e-3F);
    PERSPECTIVE_CHECK(through_left.depth == through_right.depth);
}

void invalid_surfaces_regions_eyes_and_clip_ranges_are_diagnosed() {
    const auto surface = front_surface();

    auto flat = surface;
    flat.right = flat.up;
    const auto invalid_surface = output::surface_corners(
        flat, scene::whole(flat.raster));
    PERSPECTIVE_CHECK(!invalid_surface.has_value());
    PERSPECTIVE_CHECK(
        invalid_surface.error()
        == output::PerspectiveMappingError::InvalidSurface);

    const auto outside = output::surface_corners(
        surface, scene::CanvasRect{3'840U, 0U, 1U, 1U});
    PERSPECTIVE_CHECK(!outside.has_value());
    PERSPECTIVE_CHECK(
        outside.error()
        == output::PerspectiveMappingError::RegionOutsideSurface);

    const auto invalid_eye = output::perspective_view_for(
        surface, scene::whole(surface.raster),
        scene::Vec3{std::numeric_limits<float>::quiet_NaN(), 0.0F, 0.0F},
        0.1F, 100.0F);
    PERSPECTIVE_CHECK(!invalid_eye.has_value());
    PERSPECTIVE_CHECK(
        invalid_eye.error() == output::PerspectiveMappingError::InvalidEye);

    const auto invalid_clip = output::perspective_view_for(
        surface, scene::whole(surface.raster), eye, 100.0F, 0.1F);
    PERSPECTIVE_CHECK(!invalid_clip.has_value());
    PERSPECTIVE_CHECK(
        invalid_clip.error()
        == output::PerspectiveMappingError::InvalidClipRange);

    const auto behind = output::perspective_view_for(
        surface, scene::whole(surface.raster),
        scene::Vec3{0.0F, 0.0F, -5.0F}, 0.1F, 100.0F);
    PERSPECTIVE_CHECK(!behind.has_value());
    PERSPECTIVE_CHECK(
        behind.error() == output::PerspectiveMappingError::EyeNotInFront);
}

[[nodiscard]] output::ProjectionMesh front_mesh() {
    output::ProjectionMesh mesh{};
    mesh.vertices = {
        {{-4.0F, 2.25F, -4.0F}, 0.0F, 0.0F},
        {{4.0F, 2.25F, -4.0F}, 1.0F, 0.0F},
        {{-4.0F, -2.25F, -4.0F}, 0.0F, 1.0F},
        {{4.0F, -2.25F, -4.0F}, 1.0F, 1.0F},
    };
    mesh.indices = {0U, 2U, 1U, 1U, 2U, 3U};
    return mesh;
}

[[nodiscard]] output::ProjectionMesh curved_mesh() {
    auto surface = front_surface();
    surface.height = 3.0F;
    auto generated = output::horizontal_arc_mesh(
        surface, 1.0F / 3.0F, 4U);
    if (!generated) {
        throw std::runtime_error{"horizontal arc generation failed"};
    }
    return std::move(generated).value();
}

void a_planar_mesh_matches_the_flat_surface_projection() {
    const output::PhysicalSurface surface = front_surface();
    const auto planar = output::perspective_view_for(
        surface, scene::whole(surface.raster), eye, 0.1F, 1'000.0F);
    const auto mesh = output::mesh_perspective_view_for(
        front_mesh(), eye, 0.1F, 1'000.0F);
    PERSPECTIVE_CHECK(planar.has_value() && mesh.has_value());
    PERSPECTIVE_CHECK(
        std::abs(planar.value().frustum.left - mesh.value().frustum.left)
        < 1.0e-7F);
    PERSPECTIVE_CHECK(
        std::abs(planar.value().frustum.right - mesh.value().frustum.right)
        < 1.0e-7F);
    PERSPECTIVE_CHECK(
        std::abs(planar.value().frustum.bottom - mesh.value().frustum.bottom)
        < 1.0e-7F);
    PERSPECTIVE_CHECK(
        std::abs(planar.value().frustum.top - mesh.value().frustum.top)
        < 1.0e-7F);

    const auto warp = output::mesh_warp_vertices(front_mesh(), mesh.value());
    PERSPECTIVE_CHECK(warp.has_value());
    PERSPECTIVE_CHECK(warp.value().size() == 4U);
    PERSPECTIVE_CHECK(
        warp.value()[0U].destination_clip ==
        (scene::Vec4{-1.0F, 1.0F, 0.0F, 1.0F}));
    PERSPECTIVE_CHECK(
        warp.value()[3U].destination_clip ==
        (scene::Vec4{1.0F, -1.0F, 0.0F, 1.0F}));
}

void editor_projection_follows_the_final_mesh_warp() {
    const output::ProjectionMesh mesh = front_mesh();
    const auto view = output::mesh_perspective_view_for(
        mesh, eye, 0.1F, 1'000.0F);
    PERSPECTIVE_CHECK(view.has_value());
    constexpr scene::Canvas destination{1'280U, 720U};

    // Each physical mesh corner must return to the destination pixel authored
    // by its UV0. This is the same projective divide the GPU performs and is
    // the basis for mapped Program-output selection and outlines.
    for (const output::ProjectionMeshVertex& vertex : mesh.vertices) {
        const scene::CanvasProjection projected =
            output::project_through_mesh_warp(mesh, view.value(), destination,
                                              vertex.position);
        PERSPECTIVE_CHECK(projected.visible);
        PERSPECTIVE_CHECK(
            std::abs(projected.x -
                     vertex.destination_u *
                         static_cast<float>(destination.width)) <
            1.0e-3F);
        PERSPECTIVE_CHECK(
            std::abs(projected.y -
                     vertex.destination_v *
                         static_cast<float>(destination.height)) <
            1.0e-3F);
    }
}

void one_mesh_view_encloses_a_curved_led_wall() {
    const output::ProjectionMesh mesh = curved_mesh();
    const auto view = output::mesh_perspective_view_for(
        mesh, eye, 0.1F, 1'000.0F);
    PERSPECTIVE_CHECK(view.has_value());
    const auto warp = output::mesh_warp_vertices(mesh, view.value());
    PERSPECTIVE_CHECK(warp.has_value());
    PERSPECTIVE_CHECK(warp.value().size() == mesh.vertices.size());

    // Every physical rib is inside the one fitted scene render. Destination
    // coordinates still cover one ordinary flat connector raster.
    for (std::size_t index = 0U; index < warp.value().size(); ++index) {
        const scene::Vec4 source = warp.value()[index].source_clip;
        PERSPECTIVE_CHECK(source.w > 0.0F);
        PERSPECTIVE_CHECK(std::abs(source.x / source.w) <= 1.00001F);
        PERSPECTIVE_CHECK(std::abs(source.y / source.w) <= 1.00001F);
        const float expected_x =
            mesh.vertices[index].destination_u * 2.0F - 1.0F;
        PERSPECTIVE_CHECK(
            warp.value()[index].destination_clip.x == expected_x);
    }

    // Curvature makes source sampling projective rather than a disguised
    // fullscreen copy: the quarter rib does not land at its destination UV.
    const scene::Vec4 quarter_source = warp.value()[2U].source_clip;
    const float source_x = quarter_source.x / quarter_source.w;
    PERSPECTIVE_CHECK(
        std::abs(source_x - warp.value()[2U].destination_clip.x) > 0.01F);
}

void invalid_meshes_and_patches_behind_the_eye_are_rejected() {
    output::ProjectionMesh malformed = front_mesh();
    malformed.indices.back() = 99U;
    PERSPECTIVE_CHECK(!output::valid(malformed));
    const auto invalid = output::mesh_perspective_view_for(
        malformed, eye, 0.1F, 100.0F);
    PERSPECTIVE_CHECK(!invalid.has_value());
    PERSPECTIVE_CHECK(
        invalid.error() == output::PerspectiveMappingError::InvalidMesh);

    output::ProjectionMesh crossing = front_mesh();
    crossing.vertices[0U].position.z = 1.0F;
    const auto behind = output::mesh_perspective_view_for(
        crossing, eye, 0.1F, 100.0F);
    PERSPECTIVE_CHECK(!behind.has_value());
    PERSPECTIVE_CHECK(
        behind.error() == output::PerspectiveMappingError::MeshBehindEye);
}

void adjacent_arc_feeds_share_the_same_physical_mesh_edge() {
    const output::PhysicalSurface surface = front_surface();
    constexpr scene::CanvasRect left{0U, 0U, 1'237U, 2'160U};
    constexpr scene::CanvasRect right{
        1'237U, 0U, 3'840U - 1'237U, 2'160U};
    const auto left_mesh =
        output::horizontal_arc_mesh(surface, 0.25F, 64U, left);
    const auto right_mesh =
        output::horizontal_arc_mesh(surface, 0.25F, 64U, right);
    PERSPECTIVE_CHECK(left_mesh.has_value() && right_mesh.has_value());
    const std::size_t left_top = left_mesh.value().vertices.size() - 2U;
    const std::size_t left_bottom = left_mesh.value().vertices.size() - 1U;
    PERSPECTIVE_CHECK(
        left_mesh.value().vertices[left_top].position ==
        right_mesh.value().vertices[0U].position);
    PERSPECTIVE_CHECK(
        left_mesh.value().vertices[left_bottom].position ==
        right_mesh.value().vertices[1U].position);

    const auto left_view = output::mesh_perspective_view_for(
        left_mesh.value(), eye, 0.1F, 100.0F);
    const auto right_view = output::mesh_perspective_view_for(
        right_mesh.value(), eye, 0.1F, 100.0F);
    PERSPECTIVE_CHECK(left_view.has_value() && right_view.has_value());
    const auto left_warp =
        output::mesh_warp_vertices(left_mesh.value(), left_view.value());
    const auto right_warp =
        output::mesh_warp_vertices(right_mesh.value(), right_view.value());
    PERSPECTIVE_CHECK(left_warp.has_value() && right_warp.has_value());
    const scene::Vec4 left_clip = left_warp.value()[left_top].source_clip;
    const scene::Vec4 right_clip = right_warp.value()[0U].source_clip;
    PERSPECTIVE_CHECK(std::abs(left_clip.x / left_clip.w - 1.0F) < 1.0e-5F);
    PERSPECTIVE_CHECK(
        std::abs(right_clip.x / right_clip.w + 1.0F) < 1.0e-5F);
}

// A camera on the stage looking straight at the wall, with a long enough lens
// that the region it sees is a well inside the wall rather than all of it.
[[nodiscard]] scene::Camera tracked_camera(
    scene::Vec3 position, scene::Turns field_of_view) noexcept {
    scene::Camera camera{};
    camera.eye = position;
    camera.target = scene::Vec3{position.x, position.y, position.z - 1.0F};
    camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    camera.field_of_view = field_of_view;
    camera.near_plane = 0.05F;
    camera.far_plane = 1'000.0F;
    return camera;
}

void the_inner_frustum_lands_where_the_camera_is_pointing() {
    const output::PhysicalSurface surface = front_surface();
    // The surface is 8 x 4.5 metres, four metres away, its centre on the
    // camera axis. A 2 x tan(half fov) x 4 metre patch of it is what a camera
    // at the origin sees.
    const auto centred = output::inner_frustum_region(
        surface, tracked_camera(scene::Vec3{}, 0.05F), 16.0F / 9.0F, 0U);
    PERSPECTIVE_CHECK(centred.has_value());
    PERSPECTIVE_CHECK(centred.value().valid);
    PERSPECTIVE_CHECK(!centred.value().clamped);

    const scene::CanvasRect rect = centred.value().rect;
    // Centred on the wall to within the outward rounding.
    const float centre_x =
        static_cast<float>(rect.x) + static_cast<float>(rect.width) * 0.5F;
    const float centre_y =
        static_cast<float>(rect.y) + static_cast<float>(rect.height) * 0.5F;
    PERSPECTIVE_CHECK(std::abs(centre_x - 1'920.0F) < 2.0F);
    PERSPECTIVE_CHECK(std::abs(centre_y - 1'080.0F) < 2.0F);
    // tan(9 degrees) x 4 m x 2 = 1.267 m wide out of 8 m of wall.
    const float expected_width =
        (2.0F * std::tan(0.05F * 2.0F * 3.14159265358979F / 2.0F) * 4.0F
         * (16.0F / 9.0F) / 8.0F)
        * 3'840.0F;
    PERSPECTIVE_CHECK(
        std::abs(static_cast<float>(rect.width) - expected_width) < 4.0F);

    // Moving the camera to its left moves the region to its left, which is
    // decreasing raster x for a wall whose +x runs with the raster.
    const auto shifted = output::inner_frustum_region(
        surface, tracked_camera(scene::Vec3{-1.0F, 0.0F, 0.0F}, 0.05F),
        16.0F / 9.0F, 0U);
    PERSPECTIVE_CHECK(shifted.has_value() && shifted.value().valid);
    PERSPECTIVE_CHECK(shifted.value().rect.x < rect.x);
}

void an_unbounded_or_offscreen_frustum_clamps_to_the_whole_raster() {
    const output::PhysicalSurface surface = front_surface();
    // A wide lens close in sees past both ends of the wall. 0.2 turns is 72
    // degrees vertical; `scene::valid` refuses a camera at or past 90, so this
    // is near the widest a tracked camera may author.
    const auto wide = output::inner_frustum_region(
        surface, tracked_camera(scene::Vec3{0.0F, 0.0F, 1.0F}, 0.2F),
        16.0F / 9.0F, 0U);
    PERSPECTIVE_CHECK(wide.has_value());
    PERSPECTIVE_CHECK(wide.value().valid);
    PERSPECTIVE_CHECK(wide.value().clamped);
    PERSPECTIVE_CHECK(wide.value().rect == scene::whole(surface.raster));

    // A camera in front of the wall whose frustum straddles its plane - some
    // corner rays reach it, some diverge away - still has to be covered
    // outward, so the whole raster is the answer. Wasteful, never wrong.
    scene::Camera along = tracked_camera(scene::Vec3{0.0F, 0.0F, -3.5F}, 0.05F);
    along.target = scene::Vec3{4.0F, 0.0F, -3.5F};
    const auto straddling =
        output::inner_frustum_region(surface, along, 16.0F / 9.0F, 0U);
    PERSPECTIVE_CHECK(straddling.has_value());
    PERSPECTIVE_CHECK(straddling.value().valid);
    PERSPECTIVE_CHECK(straddling.value().clamped);
    PERSPECTIVE_CHECK(straddling.value().rect == scene::whole(surface.raster));

    // But a camera that cannot see this wall at all draws nothing on it.
    //
    // This is the distinction the clamp above must not swallow. "The whole
    // wall" is the conservative answer only while the camera is looking at the
    // wall; for a camera looking away it is not conservative, it is a
    // full-wall plate that replaces the outer frustum lighting the set, and it
    // reports 100% wall coverage to the operator at exactly the moment the
    // camera is pointing somewhere else.
    scene::Camera facing_away =
        tracked_camera(scene::Vec3{0.0F, 0.0F, 0.0F}, 0.05F);
    facing_away.target = scene::Vec3{0.0F, 0.0F, 1.0F};
    const auto turned =
        output::inner_frustum_region(surface, facing_away, 16.0F / 9.0F, 0U);
    PERSPECTIVE_CHECK(turned.has_value());
    PERSPECTIVE_CHECK(!turned.value().valid);
    PERSPECTIVE_CHECK(!turned.value().clamped);

    // Behind the wall is the same answer, for the same reason
    // `perspective_view_for` refuses that eye: an LED wall emits from its
    // authored front face and has nothing to show someone standing behind it.
    const auto behind = output::inner_frustum_region(
        surface, tracked_camera(scene::Vec3{0.0F, 0.0F, -6.0F}, 0.05F),
        16.0F / 9.0F, 0U);
    PERSPECTIVE_CHECK(behind.has_value());
    PERSPECTIVE_CHECK(!behind.value().valid);

    // On the plane is not in front of it.
    const auto coplanar = output::inner_frustum_region(
        surface, tracked_camera(scene::Vec3{0.0F, 0.0F, -4.0F}, 0.05F),
        16.0F / 9.0F, 0U);
    PERSPECTIVE_CHECK(coplanar.has_value());
    PERSPECTIVE_CHECK(!coplanar.value().valid);

    // An unusable surface, camera, or aspect is diagnosed rather than
    // producing a plausible rectangle.
    output::PhysicalSurface broken = surface;
    broken.width = 0.0F;
    PERSPECTIVE_CHECK(!output::inner_frustum_region(
        broken, tracked_camera(scene::Vec3{}, 0.05F), 1.0F, 0U));
    scene::Camera degenerate = tracked_camera(scene::Vec3{}, 0.05F);
    degenerate.target = degenerate.eye;
    PERSPECTIVE_CHECK(
        !output::inner_frustum_region(surface, degenerate, 1.0F, 0U));
    PERSPECTIVE_CHECK(!output::inner_frustum_region(
        surface, tracked_camera(scene::Vec3{}, 0.05F), 0.0F, 0U));
}

// The property the whole feature rests on: the region is derived from the
// complete authored surface and the replicated pose, never from the region a
// particular node happens to own.
void every_node_derives_the_same_inner_frustum_rectangle() {
    const output::PhysicalSurface surface = front_surface();
    const std::array<scene::CanvasRect, 3U> ports{{
        {0U, 0U, 1'280U, 2'160U},
        {1'280U, 0U, 1'280U, 2'160U},
        {2'560U, 0U, 1'280U, 2'160U},
    }};

    for (int step = -6; step <= 6; ++step) {
        const scene::Camera camera = tracked_camera(
            scene::Vec3{static_cast<float>(step) * 0.31F,
                        static_cast<float>(step) * 0.17F, 0.5F},
            0.06F);
        const auto region =
            output::inner_frustum_region(surface, camera, 16.0F / 9.0F, 24U);
        PERSPECTIVE_CHECK(region.has_value());

        // Every node evaluates the identical expression over the identical
        // replicated values, so the answers are equal bit for bit.
        for (std::size_t node = 1U; node < ports.size(); ++node) {
            const auto again = output::inner_frustum_region(
                surface, camera, 16.0F / 9.0F, 24U);
            PERSPECTIVE_CHECK(again.has_value());
            PERSPECTIVE_CHECK(again.value() == region.value());
        }

        // At least one port has to render it, and a port that misses it
        // entirely skips the inner render. That skip is the performance
        // argument for ICVFX, and it is a rectangle test on replicated values.
        std::size_t rendering = 0U;
        for (const scene::CanvasRect& port : ports) {
            if (scene::intersects(port, region.value().rect)) {
                ++rendering;
            }
        }
        PERSPECTIVE_CHECK(rendering >= 1U);
        PERSPECTIVE_CHECK(rendering <= ports.size());
    }

    // A tight region in the middle port really is skipped by the outer two,
    // rather than every port always claiming a hit.
    const auto middle = output::inner_frustum_region(
        surface, tracked_camera(scene::Vec3{}, 0.04F), 16.0F / 9.0F, 8U);
    PERSPECTIVE_CHECK(middle.has_value() && middle.value().valid);
    PERSPECTIVE_CHECK(!scene::intersects(ports[0], middle.value().rect));
    PERSPECTIVE_CHECK(scene::intersects(ports[1], middle.value().rect));
    PERSPECTIVE_CHECK(!scene::intersects(ports[2], middle.value().rect));
}

void lens_distortion_never_reduces_physical_inner_frustum_coverage() {
    const output::PhysicalSurface surface = front_surface();
    const scene::Camera camera = tracked_camera(scene::Vec3{}, 0.05F);
    scene::LensProfile undistorted{};
    undistorted.sensor_width_mm = 36.0F;
    undistorted.sensor_height_mm = 24.0F;
    undistorted.focal_length_mm = 50.0F;
    scene::LensProfile distorted = undistorted;
    distorted.distortion.k1 = 0.2F;

    const auto plain = output::inner_frustum_region(
        surface, camera, undistorted, scene::LensEncoders{}, 0U);
    const auto covered = output::inner_frustum_region(
        surface, camera, distorted, scene::LensEncoders{}, 0U);
    PERSPECTIVE_CHECK(plain.has_value() && covered.has_value());
    PERSPECTIVE_CHECK(plain.value().valid && covered.value().valid);
    const scene::CanvasRect plain_rect = plain.value().rect;
    const scene::CanvasRect covered_rect = covered.value().rect;
    PERSPECTIVE_CHECK(covered_rect.x <= plain_rect.x);
    PERSPECTIVE_CHECK(covered_rect.y <= plain_rect.y);
    PERSPECTIVE_CHECK(
        covered_rect.x + covered_rect.width >= plain_rect.x + plain_rect.width);
    PERSPECTIVE_CHECK(
        covered_rect.y + covered_rect.height >= plain_rect.y + plain_rect.height);
}

void a_curved_mesh_bounds_the_region_conservatively() {
    const output::ProjectionMesh mesh = curved_mesh();
    const scene::Canvas destination{1'920U, 1'080U};

    // A narrow lens at the arc's centre covers a fraction of the destination.
    const auto narrow = output::inner_frustum_region(
        mesh, destination, tracked_camera(scene::Vec3{}, 0.03F),
        16.0F / 9.0F, 0U);
    PERSPECTIVE_CHECK(narrow.valid);
    PERSPECTIVE_CHECK(narrow.rect.width < destination.width);

    // Widening the lens can only grow the region, never shrink it: the test
    // is conservative by construction and must stay that way.
    const auto wide = output::inner_frustum_region(
        mesh, destination, tracked_camera(scene::Vec3{}, 0.15F),
        16.0F / 9.0F, 0U);
    PERSPECTIVE_CHECK(wide.valid);
    PERSPECTIVE_CHECK(wide.rect.width >= narrow.rect.width);
    PERSPECTIVE_CHECK(wide.rect.x <= narrow.rect.x);

    // A camera facing away from the arc covers none of it.
    scene::Camera behind = tracked_camera(scene::Vec3{}, 0.03F);
    behind.target = scene::Vec3{0.0F, 0.0F, 1.0F};
    PERSPECTIVE_CHECK(
        !output::inner_frustum_region(mesh, destination, behind,
                                      16.0F / 9.0F, 0U)
             .valid);

    output::ProjectionMesh empty{};
    PERSPECTIVE_CHECK(
        !output::inner_frustum_region(empty, destination,
                                      tracked_camera(scene::Vec3{}, 0.03F),
                                      16.0F / 9.0F, 0U)
             .valid);
}

[[nodiscard]] bool near(scene::Vec3 a, scene::Vec3 b, float tolerance) noexcept {
    return std::abs(a.x - b.x) < tolerance && std::abs(a.y - b.y) < tolerance
        && std::abs(a.z - b.z) < tolerance;
}

// A 4 m cube room around a viewer at its centre: the front wall and the
// ceiling, authored the way an operator does - yaw zero, the ceiling pitched a
// quarter turn so it faces down.
[[nodiscard]] output::PhysicalSurface cave_front() noexcept {
    output::PhysicalSurface front{};
    front.center = scene::Vec3{0.0F, 0.0F, -2.0F};
    output::orient(front, 0.0F, 0.0F);
    front.width = 4.0F;
    front.height = 4.0F;
    front.raster = scene::Canvas{1'024U, 1'024U};
    return front;
}

[[nodiscard]] output::PhysicalSurface cave_ceiling() noexcept {
    output::PhysicalSurface ceiling = cave_front();
    ceiling.center = scene::Vec3{0.0F, 2.0F, 0.0F};
    output::orient(ceiling, 0.0F, 0.25F);
    return ceiling;
}

void a_pitched_quarter_turn_faces_a_ceiling_down_onto_the_front_wall() {
    const output::PhysicalSurface front = cave_front();
    const output::PhysicalSurface ceiling = cave_ceiling();
    // Pitch zero is exactly the upright basis every wall has always had.
    PERSPECTIVE_CHECK(front.up == (scene::Vec3{0.0F, 1.0F, 0.0F}));
    PERSPECTIVE_CHECK(front.right == (scene::Vec3{1.0F, 0.0F, 0.0F}));

    const auto front_corners =
        output::surface_corners(front, scene::whole(front.raster));
    const auto ceiling_corners =
        output::surface_corners(ceiling, scene::whole(ceiling.raster));
    PERSPECTIVE_CHECK(front_corners.has_value() && ceiling_corners.has_value());
    // The ceiling's bottom edge is the front wall's top edge, so the two
    // images meet along the fold rather than leaving a gap or a flip.
    PERSPECTIVE_CHECK(near(ceiling_corners.value().lower_left,
                           front_corners.value().upper_left, 1.0e-5F));
    PERSPECTIVE_CHECK(near(ceiling_corners.value().lower_right,
                           front_corners.value().upper_right, 1.0e-5F));

    const auto view = output::perspective_view_for(
        ceiling, scene::whole(ceiling.raster), eye, 0.1F, 100.0F);
    PERSPECTIVE_CHECK(view.has_value());
    PERSPECTIVE_CHECK(near(view.value().forward,
                           scene::Vec3{0.0F, 1.0F, 0.0F}, 1.0e-6F));
    // A point on the fold is the top row of one image and the bottom row of
    // the other, seen from the one shared eye.
    const scene::Vec3 fold{0.5F, 2.0F, -2.0F};
    const auto front_view = output::perspective_view_for(
        front, scene::whole(front.raster), eye, 0.1F, 100.0F);
    PERSPECTIVE_CHECK(front_view.has_value());
    const auto on_front =
        output::project_through_surface(front_view.value(), fold);
    const auto on_ceiling = output::project_through_surface(view.value(), fold);
    PERSPECTIVE_CHECK(std::abs(on_front.y - 0.0F) < 1.0e-2F);
    PERSPECTIVE_CHECK(std::abs(on_ceiling.y - 1'024.0F) < 1.0e-2F);
    PERSPECTIVE_CHECK(std::abs(on_front.x - on_ceiling.x) < 1.0e-2F);
}

void a_yaw_keeps_the_authored_pitch() {
    output::PhysicalSurface ceiling = cave_ceiling();
    output::set_yaw_keeping_pitch(ceiling, 0.25F);
    const output::SurfacePitchBasis pitch =
        output::surface_pitch_basis(ceiling);
    PERSPECTIVE_CHECK(std::abs(pitch.cosine) < 1.0e-6F);
    PERSPECTIVE_CHECK(std::abs(pitch.sine - 1.0F) < 1.0e-6F);
    // Still facing down; only the way its raster runs has turned.
    const scene::Vec3 normal = scene::cross(ceiling.right, ceiling.up);
    PERSPECTIVE_CHECK(near(normal, scene::Vec3{0.0F, -1.0F, 0.0F}, 1.0e-6F));
    PERSPECTIVE_CHECK(near(ceiling.right, scene::Vec3{0.0F, 0.0F, -1.0F},
                           1.0e-6F));

    output::PhysicalSurface wall = cave_front();
    output::set_yaw_keeping_pitch(wall, 0.125F);
    PERSPECTIVE_CHECK(wall.up == (scene::Vec3{0.0F, 1.0F, 0.0F}));
}

[[nodiscard]] scene::Camera flying_camera() noexcept {
    // Somewhere far from the origin, looking down world +X - a quarter turn
    // from the stage's own -Z.
    scene::Camera camera{};
    camera.eye = scene::Vec3{40.0F, 3.0F, -12.0F};
    camera.target = scene::Vec3{41.0F, 3.0F, -12.0F};
    camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    return camera;
}

void a_camera_anchored_stage_carries_every_face_with_the_camera() {
    const scene::Camera camera = flying_camera();
    // The viewer stands 1.6 m above the room's floor origin.
    const scene::Vec3 viewer{0.0F, 1.6F, 0.0F};
    const auto pose = output::camera_stage_pose(camera, viewer);
    PERSPECTIVE_CHECK(pose.has_value());
    PERSPECTIVE_CHECK(near(output::stage_point(pose.value(), viewer),
                           camera.eye, 1.0e-5F));
    PERSPECTIVE_CHECK(near(
        output::stage_direction(pose.value(), scene::Vec3{0.0F, 0.0F, -1.0F}),
        scene::Vec3{1.0F, 0.0F, 0.0F}, 1.0e-6F));

    output::PhysicalSurface front = cave_front();
    front.center = scene::Vec3{0.0F, 1.6F, -2.0F};
    output::PhysicalSurface ceiling = cave_ceiling();
    ceiling.center = scene::Vec3{0.0F, 3.6F, 0.0F};
    const output::PhysicalSurface placed_front =
        output::place_on_stage(front, pose.value());
    const output::PhysicalSurface placed_ceiling =
        output::place_on_stage(ceiling, pose.value());
    PERSPECTIVE_CHECK(near(placed_front.center,
                           scene::Vec3{42.0F, 3.0F, -12.0F}, 1.0e-5F));
    PERSPECTIVE_CHECK(placed_front.raster == front.raster);
    PERSPECTIVE_CHECK(placed_front.width == front.width);

    const auto front_view = output::perspective_view_for(
        placed_front, scene::whole(placed_front.raster), camera.eye, 0.1F,
        100.0F);
    const auto ceiling_view = output::perspective_view_for(
        placed_ceiling, scene::whole(placed_ceiling.raster), camera.eye, 0.1F,
        100.0F);
    PERSPECTIVE_CHECK(front_view.has_value() && ceiling_view.has_value());
    // The front looks where the camera looks and the ceiling straight up,
    // and the frustum is the one the room had standing still at the origin.
    PERSPECTIVE_CHECK(near(front_view.value().forward,
                           scene::Vec3{1.0F, 0.0F, 0.0F}, 1.0e-6F));
    PERSPECTIVE_CHECK(near(ceiling_view.value().forward,
                           scene::Vec3{0.0F, 1.0F, 0.0F}, 1.0e-6F));
    const auto still = output::perspective_view_for(
        cave_ceiling(), scene::whole(ceiling.raster), eye, 0.1F, 100.0F);
    PERSPECTIVE_CHECK(still.has_value());
    PERSPECTIVE_CHECK(std::abs(ceiling_view.value().frustum.left -
                               still.value().frustum.left) < 1.0e-6F);
    PERSPECTIVE_CHECK(std::abs(ceiling_view.value().frustum.top -
                               still.value().frustum.top) < 1.0e-6F);

    // A generated mesh carried by the same pose lands where the surface does.
    const output::ProjectionMeshPlacement placement =
        output::stage_mesh_placement(pose.value());
    PERSPECTIVE_CHECK(output::valid(placement));
    PERSPECTIVE_CHECK(near(
        output::place_projection_mesh_position(front.center, placement),
        placed_front.center, 1.0e-5F));
}

void a_camera_without_a_direction_or_roll_is_refused() {
    scene::Camera camera = flying_camera();
    camera.target = camera.eye;
    const auto still = output::camera_stage_pose(camera, scene::Vec3{});
    PERSPECTIVE_CHECK(!still.has_value());
    PERSPECTIVE_CHECK(still.error()
                      == output::PerspectiveMappingError::InvalidStage);
    camera = flying_camera();
    camera.target = camera.eye + scene::Vec3{0.0F, 1.0F, 0.0F};
    PERSPECTIVE_CHECK(
        !output::camera_stage_pose(camera, scene::Vec3{}).has_value());
    camera = flying_camera();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    PERSPECTIVE_CHECK(!output::camera_stage_pose(
                           camera, scene::Vec3{nan, 0.0F, 0.0F})
                           .has_value());
}

struct Case final {
    const char* name;
    void (*function)();
};

// A calibrated camera sizes its plate from its own lens, not from the feed's
// single fallback point.
//
// The rectangle used to come from a field of view the Director derived from the
// feed's sensor height and fallback focal length - one point, which cannot
// follow a zoom encoder - while the camera carried a calibrated profile that
// sized nothing. Two lenses deciding one plate is a background that slides
// against the set piece it should be locked to during a push-in.
void a_calibrated_lens_sizes_the_plate_rather_than_the_feed_fallback() {
    const auto surface = front_surface();
    const auto centred = tracked_camera(scene::Vec3{0.0F, 0.0F, 0.0F}, 0.05F);

    // A profile whose focal length at these encoders is much longer than the
    // camera's authored angle implies, so the two cannot be confused.
    scene::LensProfile profile{};
    profile.sensor_width_mm = 36.0F;
    profile.sensor_height_mm = 24.0F;
    profile.focal_length_mm = 85.0F;
    const scene::LensEncoders encoders{};

    const auto from_feed =
        output::inner_frustum_region(surface, centred, 16.0F / 9.0F, 0U);
    const auto from_lens =
        output::inner_frustum_region(surface, centred, profile, encoders, 0U);
    PERSPECTIVE_CHECK(from_feed.has_value() && from_feed.value().valid);
    PERSPECTIVE_CHECK(from_lens.has_value() && from_lens.value().valid);
    // An 85 mm on a 36x24 sensor is a much narrower field than the authored
    // 0.05-turn camera, so the calibrated plate is smaller. The point is only
    // that the lens decides it: the two must not agree by accident.
    PERSPECTIVE_CHECK(from_lens.value().rect.width
                      != from_feed.value().rect.width);
    PERSPECTIVE_CHECK(from_lens.value().rect.width
                      < from_feed.value().rect.width);

    // And the profile is what moves it: a longer focal length is a smaller
    // plate, monotonically.
    scene::LensProfile longer = profile;
    longer.focal_length_mm = 200.0F;
    const auto tighter =
        output::inner_frustum_region(surface, centred, longer, encoders, 0U);
    PERSPECTIVE_CHECK(tighter.has_value() && tighter.value().valid);
    PERSPECTIVE_CHECK(tighter.value().rect.width
                      < from_lens.value().rect.width);

    // The lens-aware form carries the same refusals as the plain one, which is
    // the property that was missing while the two were separate copies: a
    // camera that cannot see the wall draws nothing on it, rather than getting
    // the whole-wall clamp meant for a frustum that faces it and overshoots.
    scene::Camera behind = tracked_camera(scene::Vec3{0.0F, 0.0F, -6.0F}, 0.05F);
    const auto refused =
        output::inner_frustum_region(surface, behind, profile, encoders, 0U);
    PERSPECTIVE_CHECK(refused.has_value());
    PERSPECTIVE_CHECK(!refused.value().valid);

    scene::Camera facing_away =
        tracked_camera(scene::Vec3{0.0F, 0.0F, 0.0F}, 0.05F);
    facing_away.target = scene::Vec3{0.0F, 0.0F, 1.0F};
    const auto turned = output::inner_frustum_region(
        surface, facing_away, profile, encoders, 0U);
    PERSPECTIVE_CHECK(turned.has_value());
    PERSPECTIVE_CHECK(!turned.value().valid);
}

constexpr std::array<Case, 19U> cases{{
    {"a pitched quarter turn faces a ceiling down onto the front wall",
     &a_pitched_quarter_turn_faces_a_ceiling_down_onto_the_front_wall},
    {"a yaw keeps the authored pitch", &a_yaw_keeps_the_authored_pitch},
    {"a camera-anchored stage carries every face with the camera",
     &a_camera_anchored_stage_carries_every_face_with_the_camera},
    {"a camera without a direction or roll is refused",
     &a_camera_without_a_direction_or_roll_is_refused},
    {"a front surface builds the expected off-axis window",
     &a_front_surface_builds_the_expected_off_axis_window},
    {"left and right walls look into their side of the scene",
     &left_and_right_walls_look_into_their_side_of_the_scene},
    {"adjacent ports share bit-identical physical and frustum edges",
     &adjacent_ports_share_bit_identical_physical_and_frustum_edges},
    {"invalid inputs are diagnosed",
     &invalid_surfaces_regions_eyes_and_clip_ranges_are_diagnosed},
    {"a planar mesh matches the flat surface projection",
     &a_planar_mesh_matches_the_flat_surface_projection},
    {"editor projection follows the final mesh warp",
     &editor_projection_follows_the_final_mesh_warp},
    {"one mesh view encloses a curved LED wall",
     &one_mesh_view_encloses_a_curved_led_wall},
    {"invalid meshes and behind-eye patches are rejected",
     &invalid_meshes_and_patches_behind_the_eye_are_rejected},
    {"adjacent arc feeds share the same physical mesh edge",
     &adjacent_arc_feeds_share_the_same_physical_mesh_edge},
    {"the inner frustum lands where the camera is pointing",
     &the_inner_frustum_lands_where_the_camera_is_pointing},
    {"an unbounded or offscreen frustum clamps to the whole raster",
     &an_unbounded_or_offscreen_frustum_clamps_to_the_whole_raster},
    {"every node derives the same inner-frustum rectangle",
     &every_node_derives_the_same_inner_frustum_rectangle},
    {"lens distortion never reduces physical inner-frustum coverage",
     &lens_distortion_never_reduces_physical_inner_frustum_coverage},
    {"a calibrated lens sizes the plate rather than the feed fallback",
     &a_calibrated_lens_sizes_the_plate_rather_than_the_feed_fallback},
    {"a curved mesh bounds the region conservatively",
     &a_curved_mesh_bounds_the_region_conservatively},
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
