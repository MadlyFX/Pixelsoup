#include "director_live_data.hpp"

#include <mediaserver/output/perspective_mapping.hpp>
#include <mediaserver/render/icvfx.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>

// The Live Data panel's plate rectangle, as the Director derives it.
//
// The property under test is the one the panel exists for: on every wall
// shape, the Director reports the rectangle the nodes draw. A curved or UV-mesh
// wall used to be skipped outright, so the panel said the camera saw nothing
// while it was pointed straight at the wall. Where the Director cannot know -
// a UV mesh it has not loaded - it now says that instead.
namespace {

namespace app = mediaserver::app;
namespace core = mediaserver::core;
namespace doc = mediaserver::doc;
namespace output = mediaserver::output;
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;

void check(bool condition, const char *expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define LIVE_DATA_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

constexpr scene::Canvas raster{3'840U, 2'160U};

// A tracked camera at the origin looking down -Z with an 18 degree lens: a
// plate in the middle of a wall 4 m in front of it.
[[nodiscard]] scene::Scene tracked_frame() {
    scene::Scene frame{};
    frame.canvas = raster;
    frame.renderer_settings.icvfx.inner_frustum = true;
    scene::TrackedCameraView view{};
    view.camera = doc::NodeId{11U};
    view.source = doc::NodeId{3U};
    view.authored.eye = scene::Vec3{};
    view.authored.target = scene::Vec3{0.0F, 0.0F, -1.0F};
    view.icvfx.inner_frustum = true;
    view.icvfx.margin_px = 24U;
    view.pose.position = scene::Vec3{};
    view.pose.field_of_view = 0.05F;
    view.pose.aspect_ratio = 16.0F / 9.0F;
    view.pose.valid = true;
    frame.tracked_cameras.push_back(view);
    return frame;
}

[[nodiscard]] output::PhysicalSurface front_wall() noexcept {
    return output::PhysicalSurface{scene::Vec3{0.0F, 0.0F, -4.0F},
                                   scene::Vec3{1.0F, 0.0F, 0.0F},
                                   scene::Vec3{0.0F, 1.0F, 0.0F}, 8.0F, 4.5F,
                                   raster};
}

[[nodiscard]] output::ProjectionSurface arc_surface(core::u64 node = 21U) {
    output::ProjectionSurface surface{};
    surface.node = doc::NodeId{node};
    surface.geometry = front_wall();
    surface.shape = output::ProjectionSurfaceShape::HorizontalArc;
    surface.horizontal_arc_turns = 0.2F;
    surface.mesh_columns = 32U;
    return surface;
}

[[nodiscard]] output::ProjectionSurface imported_surface(std::string source) {
    output::ProjectionSurface surface{};
    surface.node = doc::NodeId{22U};
    surface.geometry = front_wall();
    surface.shape = output::ProjectionSurfaceShape::ImportedMesh;
    surface.mesh_source = std::move(source);
    return surface;
}

void a_curved_wall_reports_the_rectangle_the_nodes_draw() {
    const scene::Scene frame = tracked_frame();
    const scene::TrackedCameraView &view = frame.tracked_cameras.front();
    output::FeedMap feed_map{};
    feed_map.surfaces.push_back(arc_surface());
    app::DirectorSurfaceMeshes meshes;

    app::DirectorPlateUnknown unknown{};
    const auto reported = app::director_inner_frustum_rect(
        frame, view, feed_map, &meshes, &unknown);
    LIVE_DATA_CHECK(reported.has_value());
    LIVE_DATA_CHECK(unknown.reason.empty());
    LIVE_DATA_CHECK(meshes.size() == 1U);

    // What a node computes for the same camera over the complete arc - the
    // node's own derivation, from its own mesh build.
    const auto complete = output::horizontal_arc_mesh(front_wall(), 0.2F, 32U);
    LIVE_DATA_CHECK(complete.has_value());
    const scene::InnerFrustumRegion drawn = render::inner_frustum_mesh_region(
        render::IcvfxSurfaceMesh{&complete.value(), nullptr}, raster,
        render::tracked_stage_camera(view), view, view.icvfx.margin_px);
    LIVE_DATA_CHECK(drawn.valid);
    LIVE_DATA_CHECK(*reported == drawn);

    // And it is where the camera is pointing: centred on a camera on the
    // wall's axis, a part of the wall rather than all of it, and a fit rather
    // than the whole-wall fallback.
    const scene::CanvasRect rect = reported->rect;
    LIVE_DATA_CHECK(!reported->clamped);
    LIVE_DATA_CHECK(rect.width < raster.width / 2U);
    LIVE_DATA_CHECK(rect.height < raster.height);
    const core::u32 middle = rect.x + rect.width / 2U;
    LIVE_DATA_CHECK(middle + 2U >= raster.width / 2U &&
                    middle <= raster.width / 2U + 2U);

    // Asked again on the next refresh, the same mesh answers.
    LIVE_DATA_CHECK(app::director_inner_frustum_rect(frame, view, feed_map,
                                                     &meshes) == reported);
    LIVE_DATA_CHECK(meshes.size() == 1U);
}

void a_wall_the_director_does_not_hold_is_unknown_rather_than_unseen() {
    const scene::Scene frame = tracked_frame();
    const scene::TrackedCameraView &view = frame.tracked_cameras.front();

    // No mesh store on this path: a curved wall is unknown, with a reason.
    output::FeedMap curved{};
    curved.surfaces.push_back(arc_surface());
    app::DirectorPlateUnknown unknown{};
    LIVE_DATA_CHECK(!app::director_inner_frustum_rect(frame, view, curved,
                                                      nullptr, &unknown)
                         .has_value());
    LIVE_DATA_CHECK(!unknown.reason.empty());

    // A UV mesh whose file the Director has not read: still unknown, and the
    // reason names the mesh - whether the worker has finished failing yet or
    // is still on it.
    const std::string missing =
        (std::filesystem::temp_directory_path() /
         "mediaserver-director-live-data-missing-wall.obj")
            .string();
    std::error_code ignored{};
    static_cast<void>(std::filesystem::remove(missing, ignored));
    output::FeedMap imported{};
    imported.surfaces.push_back(imported_surface(missing));
    app::DirectorSurfaceMeshes meshes;
    LIVE_DATA_CHECK(!app::director_inner_frustum_rect(frame, view, imported,
                                                      &meshes, &unknown)
                         .has_value());
    LIVE_DATA_CHECK(unknown.reason.find("UV mesh") != std::string_view::npos);
    // The worker settles on "could not load" without the control path ever
    // waiting for it; poll the way the editor refresh does.
    bool failed = false;
    for (int attempt = 0; attempt < 200 && !failed; ++attempt) {
        static_cast<void>(app::director_inner_frustum_rect(
            frame, view, imported, &meshes, &unknown));
        failed = unknown.reason.find("could not load") != std::string_view::npos;
        if (!failed) {
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }
    LIVE_DATA_CHECK(failed);

    // A wall it does hold still wins over one it does not.
    imported.surfaces.push_back(arc_surface());
    const auto reported = app::director_inner_frustum_rect(
        frame, view, imported, &meshes, &unknown);
    LIVE_DATA_CHECK(reported.has_value());
    LIVE_DATA_CHECK(unknown.reason.empty());

    // A camera genuinely facing away from a held wall sees nothing, and that
    // is not "unknown".
    scene::Scene away = frame;
    away.tracked_cameras.front().pose.yaw = 0.5F;
    LIVE_DATA_CHECK(!app::director_inner_frustum_rect(
                         away, away.tracked_cameras.front(), curved, &meshes,
                         &unknown)
                         .has_value());
    LIVE_DATA_CHECK(unknown.reason.empty());

    // A surface removed from the map is forgotten.
    meshes.retain(curved);
    LIVE_DATA_CHECK(meshes.size() == 1U);
    meshes.retain(output::FeedMap{});
    LIVE_DATA_CHECK(meshes.size() == 0U);
}

void a_flat_wall_is_reported_as_before() {
    const scene::Scene frame = tracked_frame();
    const scene::TrackedCameraView &view = frame.tracked_cameras.front();
    output::FeedMap feed_map{};
    output::ProjectionSurface flat{};
    flat.node = doc::NodeId{23U};
    flat.geometry = front_wall();
    feed_map.surfaces.push_back(flat);
    app::DirectorSurfaceMeshes meshes;
    const auto reported =
        app::director_inner_frustum_rect(frame, view, feed_map, &meshes);
    const auto expected = output::inner_frustum_region(
        front_wall(), render::tracked_stage_camera(view),
        view.pose.aspect_ratio, view.icvfx.margin_px);
    LIVE_DATA_CHECK(reported.has_value() && expected.has_value());
    LIVE_DATA_CHECK(*reported == expected.value());
    // No mesh is held for a flat wall.
    LIVE_DATA_CHECK(meshes.size() == 0U);
}

struct Case final {
    const char *name;
    void (*function)();
};

constexpr std::array<Case, 3U> cases{{
    {"a curved wall reports the rectangle the nodes draw",
     &a_curved_wall_reports_the_rectangle_the_nodes_draw},
    {"a wall the director does not hold is unknown rather than unseen",
     &a_wall_the_director_does_not_hold_is_unknown_rather_than_unseen},
    {"a flat wall is reported as before", &a_flat_wall_is_reported_as_before},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case &entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception &error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
