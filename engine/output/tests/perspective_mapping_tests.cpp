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

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 9U> cases{{
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
