#include <mediaserver/render/mapped_scene.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <cmath>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>

namespace {

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

#define MAPPED_CHECK(...)                                                      \
  check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class Fixture final {
public:
  Fixture() {
    static std::atomic<core::u64> next{1U};
    directory_ = std::filesystem::temp_directory_path() /
                 (std::string{"mediaserver-mapped-scene-cache-"} +
                  std::to_string(next.fetch_add(1U)));
    std::error_code ignored{};
    static_cast<void>(std::filesystem::remove_all(directory_, ignored));
    MAPPED_CHECK(std::filesystem::create_directories(directory_));
  }

  Fixture(const Fixture &) = delete;
  Fixture &operator=(const Fixture &) = delete;

  ~Fixture() {
    std::error_code ignored{};
    static_cast<void>(std::filesystem::remove_all(directory_, ignored));
  }

  [[nodiscard]] std::string path(std::string_view name) const {
    return (directory_ / name).string();
  }

  void write(std::string_view name, std::string_view contents) const {
    std::ofstream stream{directory_ / name, std::ios::binary | std::ios::trunc};
    stream.write(contents.data(),
                 static_cast<std::streamsize>(contents.size()));
    stream.close();
    MAPPED_CHECK(stream.good());
  }

  void remove(std::string_view name) const {
    std::error_code error{};
    static_cast<void>(std::filesystem::remove(directory_ / name, error));
    MAPPED_CHECK(!error);
  }

private:
  std::filesystem::path directory_;
};

constexpr std::string_view patch_a = "v -1 1 0\n"
                                     "v -1 -1 0\n"
                                     "v 1 1 0\n"
                                     "vt 0 0\n"
                                     "vt 0 1\n"
                                     "vt 1 0\n"
                                     "f 1/1 2/2 3/3\n";

constexpr std::string_view patch_b = "v -2 1 0\n"
                                     "v -2 -1 0\n"
                                     "v 2 1 0\n"
                                     "vt 0 0\n"
                                     "vt 0 1\n"
                                     "vt 1 0\n"
                                     "f 1/1 2/2 3/3\n";

[[nodiscard]] scene::Scene frame() {
  scene::Scene value{};
  value.canvas = scene::Canvas{1920U, 1080U};
  value.camera.eye = scene::Vec3{0.0F, 0.0F, 0.0F};
  value.camera.target = scene::Vec3{0.0F, 0.0F, -5.0F};
  return value;
}

[[nodiscard]] output::OutputMap imported_map(const std::string &source,
                                             core::u64 map_node = 11U) {
  output::ResolvedProjectionSurface surface{};
  surface.geometry.center = scene::Vec3{0.0F, 0.0F, -5.0F};
  surface.geometry.right = scene::Vec3{1.0F, 0.0F, 0.0F};
  surface.geometry.up = scene::Vec3{0.0F, 1.0F, 0.0F};
  surface.geometry.width = 4.0F;
  surface.geometry.height = 2.0F;
  surface.geometry.raster = scene::Canvas{1920U, 1080U};
  surface.shape = output::ProjectionSurfaceShape::ImportedMesh;
  surface.mesh_source = source;
  surface.mesh_asset = doc::NodeId{31U};

  output::OutputMap map{};
  map.node = doc::NodeId{map_node};
  map.source = scene::whole(surface.geometry.raster);
  map.projection = output::FeedProjection::PlanarSurface;
  map.surface = doc::NodeId{21U};
  map.resolved_surface = std::move(surface);
  map.viewport = map.source;
  return map;
}

[[nodiscard]] output::OutputMap arc_map(float turns = 0.20F,
                                        core::u64 map_node = 41U) {
  output::ResolvedProjectionSurface surface{};
  surface.geometry.center = scene::Vec3{0.0F, 0.0F, -5.0F};
  surface.geometry.right = scene::Vec3{1.0F, 0.0F, 0.0F};
  surface.geometry.up = scene::Vec3{0.0F, 1.0F, 0.0F};
  surface.geometry.width = 4.0F;
  surface.geometry.height = 2.0F;
  surface.geometry.raster = scene::Canvas{1920U, 1080U};
  surface.shape = output::ProjectionSurfaceShape::HorizontalArc;
  surface.horizontal_arc_turns = turns;
  surface.mesh_columns = 32U;

  output::OutputMap map{};
  map.node = doc::NodeId{map_node};
  map.source = scene::whole(surface.geometry.raster);
  map.projection = output::FeedProjection::PlanarSurface;
  map.surface = doc::NodeId{51U};
  map.resolved_surface = std::move(surface);
  map.viewport = map.source;
  return map;
}

[[nodiscard]] std::optional<render::MappedProjection>
wait_and_resolve(render::OutputProjectionCache &cache,
                 const scene::Scene &scene_frame,
                 const output::OutputMap &map) {
  MAPPED_CHECK(
      render::detail::wait_for_output_projection_cache_for_test(cache));
  return cache.resolve(scene_frame, map);
}

void failed_imports_are_sticky_until_an_explicit_refresh() {
  Fixture files;
  const std::string source = files.path("repaired.obj");
  const auto scene_frame = frame();
  const auto map = imported_map(source);
  render::OutputProjectionCache cache;

  const auto cold_gate =
      render::detail::block_next_output_projection_cache_load_for_test(cache);
  MAPPED_CHECK(cold_gate != nullptr);
  MAPPED_CHECK(!cache.resolve(scene_frame, map).has_value());
  // The loader is causally stopped before touching the path, proving the cold
  // call returned null from admission rather than synchronously discovering
  // the missing file.
  MAPPED_CHECK(cold_gate->wait_until_blocked_for_test());
  MAPPED_CHECK(cache.status().state ==
               render::OutputProjectionCacheState::Loading);
  cold_gate->release_for_test();
  MAPPED_CHECK(cold_gate->wait_until_finished_for_test());
  MAPPED_CHECK(
      render::detail::wait_for_output_projection_cache_for_test(cache));
  const auto failed = cache.status();
  MAPPED_CHECK(failed.state == render::OutputProjectionCacheState::Failed);
  MAPPED_CHECK(!failed.stale_ready);

  files.write("repaired.obj", patch_a);
  // More than the former frame-count retry interval: an unchanged authored
  // key must still do no work and remain failed after the file is repaired.
  for (std::size_t attempt = 0U; attempt < 256U; ++attempt) {
    MAPPED_CHECK(!cache.resolve(scene_frame, map).has_value());
  }
  MAPPED_CHECK(cache.status() == failed);

  MAPPED_CHECK(cache.refresh_imported_mesh());
  const auto ready = wait_and_resolve(cache, scene_frame, map);
  MAPPED_CHECK(ready.has_value());
  MAPPED_CHECK(ready->projection_mesh() != nullptr);
  MAPPED_CHECK(cache.status().state ==
               render::OutputProjectionCacheState::Ready);
  MAPPED_CHECK(cache.status().generation != failed.generation);
}

void same_key_refresh_holds_ready_but_authored_changes_do_not() {
  Fixture files;
  files.write("first.obj", patch_a);
  files.write("second.obj", patch_b);
  const auto scene_frame = frame();
  auto map = imported_map(files.path("first.obj"));
  render::OutputProjectionCache cache;

  MAPPED_CHECK(!cache.resolve(scene_frame, map).has_value());
  auto first = wait_and_resolve(cache, scene_frame, map);
  MAPPED_CHECK(first.has_value());
  const output::ProjectionMesh *const first_mesh = first->projection_mesh();
  MAPPED_CHECK(first_mesh != nullptr);

  files.remove("first.obj");
  const auto failed_refresh_gate =
      render::detail::block_next_output_projection_cache_load_for_test(cache);
  MAPPED_CHECK(cache.refresh_imported_mesh());
  MAPPED_CHECK(failed_refresh_gate->wait_until_blocked_for_test());
  // Refresh preserves the old published allocation for this exact authored
  // owner/key while the worker is causally held before the read.
  const auto held = cache.resolve(scene_frame, map);
  MAPPED_CHECK(held.has_value());
  MAPPED_CHECK(held->projection_mesh() == first_mesh);
  const auto refreshing = cache.status();
  MAPPED_CHECK(refreshing.state == render::OutputProjectionCacheState::Loading);
  MAPPED_CHECK(refreshing.stale_ready);
  failed_refresh_gate->release_for_test();
  MAPPED_CHECK(failed_refresh_gate->wait_until_finished_for_test());
  MAPPED_CHECK(
      render::detail::wait_for_output_projection_cache_for_test(cache));
  MAPPED_CHECK(cache.status().state ==
               render::OutputProjectionCacheState::Failed);
  MAPPED_CHECK(cache.status().stale_ready);
  const auto held_after_failure = cache.resolve(scene_frame, map);
  MAPPED_CHECK(held_after_failure.has_value());
  MAPPED_CHECK(held_after_failure->projection_mesh() == first_mesh);

  files.write("first.obj", patch_b);
  const auto successful_refresh_gate =
      render::detail::block_next_output_projection_cache_load_for_test(cache);
  MAPPED_CHECK(cache.refresh_imported_mesh());
  MAPPED_CHECK(successful_refresh_gate->wait_until_blocked_for_test());
  const auto held_until_success = cache.resolve(scene_frame, map);
  MAPPED_CHECK(held_until_success.has_value());
  MAPPED_CHECK(held_until_success->projection_mesh() == first_mesh);
  successful_refresh_gate->release_for_test();
  MAPPED_CHECK(successful_refresh_gate->wait_until_finished_for_test());
  auto refreshed = wait_and_resolve(cache, scene_frame, map);
  MAPPED_CHECK(refreshed.has_value());
  MAPPED_CHECK(refreshed->projection_mesh() != nullptr);
  MAPPED_CHECK(refreshed->projection_mesh() != first_mesh);

  map.resolved_surface->mesh_source = files.path("second.obj");
  map.resolved_surface->mesh_asset = doc::NodeId{32U};
  // A different source/config identity cannot borrow the former surface's
  // stale warp, even though that allocation is still retained by `refreshed`.
  MAPPED_CHECK(!cache.resolve(scene_frame, map).has_value());
  MAPPED_CHECK(
      cache.status().state == render::OutputProjectionCacheState::Loading ||
      cache.status().state == render::OutputProjectionCacheState::Ready);
  const auto second = wait_and_resolve(cache, scene_frame, map);
  MAPPED_CHECK(second.has_value());
  MAPPED_CHECK(second->projection_mesh() != nullptr);
  MAPPED_CHECK(second->projection_mesh() != refreshed->projection_mesh());
}

void animated_imported_placement_reuses_one_local_source_generation() {
  Fixture files;
  files.write("animated.obj", patch_a);
  files.write("replacement.obj", patch_b);
  const auto scene_frame = frame();
  auto map = imported_map(files.path("animated.obj"));
  render::OutputProjectionCache cache;

  MAPPED_CHECK(!cache.resolve(scene_frame, map).has_value());
  auto first = wait_and_resolve(cache, scene_frame, map);
  MAPPED_CHECK(first.has_value());
  MAPPED_CHECK(first->projection_mesh() != nullptr);
  MAPPED_CHECK(first->mesh_view.has_value());
  MAPPED_CHECK(first->mesh_placement.has_value());
  const output::ProjectionMesh *const local_mesh = first->projection_mesh();
  MAPPED_CHECK(local_mesh->vertices.front().position ==
               (scene::Vec3{-1.0F, 1.0F, 0.0F}));
  const core::u64 source_generation = cache.status().generation;
  const output::ProjectionMeshPlacement first_placement =
      *first->mesh_placement;
  const scene::Vec3 first_position = output::place_projection_mesh_position(
      local_mesh->vertices.front().position, first_placement);
  const auto first_warp = output::mesh_warp_vertices(
      *local_mesh, *first->mesh_view, first_placement);
  MAPPED_CHECK(first_warp.has_value());

  std::optional<render::MappedProjection> animated;
  for (core::u32 sample = 1U; sample <= 128U; ++sample) {
    map.node = doc::NodeId{100U + sample};
    map.surface = doc::NodeId{300U + sample};
    map.source = scene::CanvasRect{sample % 300U, sample % 100U, 960U, 540U};
    map.resolved_surface->geometry.center =
        scene::Vec3{static_cast<float>(sample) * 0.01F,
                    static_cast<float>(sample) * -0.002F,
                    -5.0F - static_cast<float>(sample) * 0.003F};
    map.resolved_surface->geometry.right =
        scene::Vec3{1.0F, 0.0F, static_cast<float>(sample) * 0.001F};
    map.resolved_surface->geometry.width =
        4.0F + static_cast<float>(sample) * 0.02F;
    map.resolved_surface->geometry.height =
        2.0F + static_cast<float>(sample) * 0.01F;
    map.resolved_surface->mesh_scale =
        0.5F + static_cast<float>(sample) * 0.01F;
    map.resolved_surface->mesh_asset = doc::NodeId{900U + sample};

    animated = cache.resolve(scene_frame, map);
    MAPPED_CHECK(animated.has_value());
    MAPPED_CHECK(animated->projection_mesh() == local_mesh);
    MAPPED_CHECK(animated->mesh_view.has_value());
    MAPPED_CHECK(animated->mesh_placement.has_value());
    MAPPED_CHECK(cache.status().generation == source_generation);
  }

  const scene::Vec3 animated_position =
      output::place_projection_mesh_position(
          local_mesh->vertices.front().position, *animated->mesh_placement);
  MAPPED_CHECK(animated_position != first_position);
  MAPPED_CHECK(animated->mesh_view->matrices.view !=
               first->mesh_view->matrices.view);
  const auto animated_warp = output::mesh_warp_vertices(
      *local_mesh, *animated->mesh_view, *animated->mesh_placement);
  MAPPED_CHECK(animated_warp.has_value());
  MAPPED_CHECK(animated_warp.value().front().source_clip !=
               first_warp.value().front().source_clip);

  const scene::Vec3 local_sample =
      (local_mesh->vertices[0U].position +
       local_mesh->vertices[1U].position +
       local_mesh->vertices[2U].position) *
      (1.0F / 3.0F);
  const scene::Vec3 first_world = output::place_projection_mesh_position(
      local_sample, first_placement);
  const scene::Vec3 animated_world = output::place_projection_mesh_position(
      local_sample, *animated->mesh_placement);
  const scene::Canvas destination{1920U, 1080U};
  const auto first_projected = output::project_through_mesh_warp(
      *local_mesh, *first->mesh_view, destination, first_world,
      first_placement);
  const auto animated_projected = output::project_through_mesh_warp(
      *local_mesh, *animated->mesh_view, destination, animated_world,
      *animated->mesh_placement);
  MAPPED_CHECK(first_projected.visible);
  MAPPED_CHECK(animated_projected.visible);
  MAPPED_CHECK(std::abs(first_projected.x - animated_projected.x) < 0.01F);
  MAPPED_CHECK(std::abs(first_projected.y - animated_projected.y) < 0.01F);

  const auto refresh_gate =
      render::detail::block_next_output_projection_cache_load_for_test(cache);
  MAPPED_CHECK(cache.refresh_imported_mesh());
  MAPPED_CHECK(refresh_gate->wait_until_blocked_for_test());
  const core::u64 refresh_generation = cache.status().generation;
  MAPPED_CHECK(refresh_generation != source_generation);
  MAPPED_CHECK(!cache.refresh_imported_mesh());
  for (core::u32 sample = 0U; sample < 128U; ++sample) {
    map.resolved_surface->geometry.center.x += 0.001F;
    map.resolved_surface->mesh_scale += 0.001F;
    const auto stale = cache.resolve(scene_frame, map);
    MAPPED_CHECK(stale.has_value());
    MAPPED_CHECK(stale->projection_mesh() == local_mesh);
    MAPPED_CHECK(cache.status().generation == refresh_generation);
  }
  refresh_gate->release_for_test();
  MAPPED_CHECK(refresh_gate->wait_until_finished_for_test());
  auto refreshed = wait_and_resolve(cache, scene_frame, map);
  MAPPED_CHECK(refreshed.has_value());
  MAPPED_CHECK(refreshed->projection_mesh() != local_mesh);
  MAPPED_CHECK(cache.status().generation == refresh_generation);

  map.resolved_surface->mesh_source = files.path("replacement.obj");
  MAPPED_CHECK(!cache.resolve(scene_frame, map).has_value());
  MAPPED_CHECK(cache.status().generation != refresh_generation);
  const auto replacement = wait_and_resolve(cache, scene_frame, map);
  MAPPED_CHECK(replacement.has_value());
  MAPPED_CHECK(replacement->projection_mesh() != refreshed->projection_mesh());
}

void an_unchanged_horizontal_arc_reuses_one_mesh_allocation() {
  const auto scene_frame = frame();
  const auto map = arc_map();
  render::OutputProjectionCache cache;

  const auto first_mesh = cache.resolve_mesh(map);
  const output::ProjectionMesh *const retained = first_mesh.get();
  MAPPED_CHECK(retained != nullptr);
  MAPPED_CHECK(cache.status().state ==
               render::OutputProjectionCacheState::Ready);

  for (std::size_t frame_index = 0U; frame_index < 256U; ++frame_index) {
    const auto repeated_mesh = cache.resolve_mesh(map);
    MAPPED_CHECK(repeated_mesh.get() == retained);
  }
  const auto full_projection = cache.resolve(scene_frame, map);
  MAPPED_CHECK(full_projection.has_value());
  MAPPED_CHECK(full_projection->projection_mesh() == retained);

  auto changed = map;
  changed.resolved_surface->horizontal_arc_turns = 0.25F;
  const auto rebuilt_mesh = cache.resolve_mesh(changed);
  MAPPED_CHECK(rebuilt_mesh != nullptr);
  MAPPED_CHECK(rebuilt_mesh.get() != retained);
}

void resolve_never_waits_for_the_publication_lock() {
  const auto scene_frame = frame();
  const auto map = arc_map();
  auto different = map;
  different.resolved_surface->horizontal_arc_turns = 0.25F;
  render::OutputProjectionCache cache;
  const auto ready = cache.resolve(scene_frame, map);
  MAPPED_CHECK(ready.has_value());
  const output::ProjectionMesh *const retained = ready->projection_mesh();

  std::optional<render::MappedProjection> same_key;
  std::optional<render::MappedProjection> changed_key;
  std::promise<void> completed;
  std::future<void> completion = completed.get_future();
  std::thread resolver;
  bool completed_while_locked = false;

  render::detail::hold_output_projection_cache_lock_for_test(cache, [&] {
    resolver = std::thread{[&] {
      same_key = cache.resolve(scene_frame, map);
      changed_key = cache.resolve(scene_frame, different);
      completed.set_value();
    }};
    completed_while_locked = completion.wait_for(std::chrono::seconds{2}) ==
                             std::future_status::ready;
  });
  resolver.join();

  MAPPED_CHECK(completed_while_locked);
  MAPPED_CHECK(same_key.has_value());
  MAPPED_CHECK(same_key->projection_mesh() == retained);
  MAPPED_CHECK(!changed_key.has_value());
}

void reset_and_teardown_do_not_join_an_in_flight_loader() {
  Fixture files;
  files.write("blocked.obj", patch_a);
  const auto scene_frame = frame();
  const auto map = imported_map(files.path("blocked.obj"));

  auto cache = std::make_unique<render::OutputProjectionCache>();
  const auto reset_gate =
      render::detail::block_next_output_projection_cache_load_for_test(*cache);
  MAPPED_CHECK(!cache->resolve(scene_frame, map).has_value());
  MAPPED_CHECK(reset_gate->wait_until_blocked_for_test());
  // This call is intentionally made while the worker is causally blocked. A
  // cache-owned worker/join would deadlock here; generation invalidation must
  // return and let the orphaned publication be discarded later.
  cache->reset();
  MAPPED_CHECK(cache->status().state ==
               render::OutputProjectionCacheState::Empty);
  reset_gate->release_for_test();
  MAPPED_CHECK(reset_gate->wait_until_finished_for_test());
  MAPPED_CHECK(cache->status().state ==
               render::OutputProjectionCacheState::Empty);

  auto destroyed = std::make_unique<render::OutputProjectionCache>();
  const auto teardown_gate =
      render::detail::block_next_output_projection_cache_load_for_test(
          *destroyed);
  MAPPED_CHECK(!destroyed->resolve(scene_frame, map).has_value());
  MAPPED_CHECK(teardown_gate->wait_until_blocked_for_test());
  destroyed.reset();
  teardown_gate->release_for_test();
  MAPPED_CHECK(teardown_gate->wait_until_finished_for_test());
}

[[nodiscard]] bool near(scene::Vec3 a, scene::Vec3 b) noexcept {
  return std::abs(a.x - b.x) < 1.0e-5F && std::abs(a.y - b.y) < 1.0e-5F &&
         std::abs(a.z - b.z) < 1.0e-5F;
}

// The camera a fly-through has reached: far from the origin, looking down
// world +X, a quarter turn from where the room was authored to face.
[[nodiscard]] scene::Scene flown_frame(float x) {
  scene::Scene value = frame();
  value.camera.eye = scene::Vec3{x, 2.0F, 7.0F};
  value.camera.target = scene::Vec3{x + 1.0F, 2.0F, 7.0F};
  return value;
}

void a_camera_stage_carries_flat_and_arc_faces_without_rebuilding_meshes() {
  output::OutputMap ceiling = arc_map();
  // Facing down, two metres over the viewer.
  output::orient(ceiling.resolved_surface->geometry, 0.0F, 0.25F);
  ceiling.resolved_surface->geometry.center = scene::Vec3{0.0F, 2.0F, 0.0F};
  ceiling.resolved_surface->stage.anchor = output::StageAnchor::Camera;
  output::OutputMap front = arc_map(0.20F, 42U);
  front.resolved_surface->shape = output::ProjectionSurfaceShape::Planar;
  front.resolved_surface->stage.anchor = output::StageAnchor::Camera;

  render::OutputProjectionCache ceiling_cache;
  render::OutputProjectionCache front_cache;
  const auto first = ceiling_cache.resolve(flown_frame(10.0F), ceiling);
  const auto flat = front_cache.resolve(flown_frame(10.0F), front);
  MAPPED_CHECK(first.has_value() && flat.has_value());
  // The front face looks where the camera looks, the ceiling straight up,
  // both from the camera's own eye.
  MAPPED_CHECK(near(flat->render_view.forward, scene::Vec3{1.0F, 0.0F, 0.0F}));
  MAPPED_CHECK(near(flat->render_view.eye, scene::Vec3{10.0F, 2.0F, 7.0F}));
  MAPPED_CHECK(near(first->render_view.forward, scene::Vec3{0.0F, 1.0F, 0.0F}));
  MAPPED_CHECK(first->mesh_placement.has_value());
  MAPPED_CHECK(near(flat->surface_up, scene::Vec3{0.0F, 1.0F, 0.0F}));

  // Flying on moves the view but never the cached stage-space mesh.
  const output::ProjectionMesh *const retained = first->projection_mesh();
  MAPPED_CHECK(retained != nullptr);
  for (int step = 1; step <= 32; ++step) {
    const auto later = ceiling_cache.resolve(
        flown_frame(10.0F + static_cast<float>(step)), ceiling);
    MAPPED_CHECK(later.has_value());
    MAPPED_CHECK(later->projection_mesh() == retained);
  }
  // Nor does moving the viewer within the room: the stage is placement.
  output::OutputMap shifted = ceiling;
  shifted.resolved_surface->stage.eye = scene::Vec3{0.0F, 0.5F, 0.0F};
  const auto shifted_view = ceiling_cache.resolve(flown_frame(10.0F), shifted);
  MAPPED_CHECK(shifted_view.has_value());
  MAPPED_CHECK(shifted_view->projection_mesh() == retained);
  const auto moved = ceiling_cache.resolve(flown_frame(30.0F), ceiling);
  MAPPED_CHECK(moved.has_value());
  MAPPED_CHECK(near(moved->render_view.eye, scene::Vec3{30.0F, 2.0F, 7.0F}));
  MAPPED_CHECK(moved->render_view.matrices.view !=
               first->render_view.matrices.view);

  // The same front face fixed in the world keeps facing where it was built
  // to face while the camera has turned a quarter turn away from it.
  output::OutputMap fixed = front;
  fixed.resolved_surface->stage = output::StagePlacement{};
  render::OutputProjectionCache fixed_cache;
  const auto world = fixed_cache.resolve(flown_frame(10.0F), fixed);
  MAPPED_CHECK(world.has_value());
  MAPPED_CHECK(near(world->render_view.forward, scene::Vec3{0.0F, 0.0F, -1.0F}));

  // A camera with no direction has no room to carry: refused, not guessed.
  scene::Scene stuck = flown_frame(10.0F);
  stuck.camera.target = stuck.camera.eye;
  MAPPED_CHECK(!front_cache.resolve(stuck, front).has_value());
}

struct Case final {
  const char *name;
  void (*function)();
};

void authored_refresh_generation_replaces_the_mesh_and_diagnostics_reuse_it() {
  Fixture files;
  files.write("revision.obj", patch_a);
  auto scene_frame = frame();
  scene_frame.asset_refresh_generation = 15;
  const auto map = imported_map(files.path("revision.obj"));
  render::OutputProjectionCache cache;
  static_cast<void>(cache.resolve(scene_frame, map));
  MAPPED_CHECK(render::detail::wait_for_output_projection_cache_for_test(cache));
  const auto first = cache.resolve(scene_frame, map);
  MAPPED_CHECK(first && first->projection_mesh());
  files.write("revision.obj", patch_b);
  MAPPED_CHECK(cache.resolve(scene_frame, map)->projection_mesh() == first->projection_mesh());
  scene_frame.asset_refresh_generation = 16;
  MAPPED_CHECK(!cache.resolve(scene_frame, map));
  MAPPED_CHECK(render::detail::wait_for_output_projection_cache_for_test(cache));
  const auto second = cache.resolve(scene_frame, map);
  MAPPED_CHECK(second && second->projection_mesh() != first->projection_mesh());
  MAPPED_CHECK(cache.resolve(scene_frame, map)->projection_mesh() == second->projection_mesh());
}

// A tracked plate's rectangle has to be derived over the complete surface,
// not over the region one output owns, or two nodes would disagree about where
// the plate ends. The projection therefore carries the complete mesh beside the
// region mesh it warps with.
void a_mapped_projection_carries_the_complete_surface_mesh() {
  const auto scene_frame = frame();

  // An arc output owning the right half of its raster: the region mesh spans
  // only that half in surface terms, the complete mesh the whole raster.
  auto half = arc_map(0.20F, 61U);
  half.source = scene::CanvasRect{960U, 0U, 960U, 1080U};
  half.viewport = scene::CanvasRect{0U, 0U, 960U, 1080U};
  render::OutputProjectionCache half_cache;
  const auto partial = half_cache.resolve(scene_frame, half);
  MAPPED_CHECK(partial.has_value());
  MAPPED_CHECK(partial->retained_mesh != nullptr);
  MAPPED_CHECK(partial->surface_mesh != nullptr);
  MAPPED_CHECK(partial->surface_mesh != partial->retained_mesh);
  // 32 authored columns over the whole raster: 33 ribs of two vertices. The
  // half-raster region mesh has its own 16 columns, 17 ribs.
  MAPPED_CHECK(partial->surface_mesh->vertices.size() == 66U);
  MAPPED_CHECK(partial->retained_mesh->vertices.size() == 34U);
  // The complete mesh's left edge is the raster's left edge; its first rib
  // sits left of the region mesh's first rib, which starts mid-wall.
  MAPPED_CHECK(partial->surface_mesh->vertices.front().destination_u == 0.0F);
  MAPPED_CHECK(partial->surface_mesh->vertices.back().destination_u == 1.0F);
  MAPPED_CHECK(partial->surface_mesh->vertices.front().position.x <
               partial->retained_mesh->vertices.front().position.x - 1.0F);
  // Its region's left rib is the complete mesh's middle rib, bit for bit,
  // because both are built through the same absolute-pixel expression.
  MAPPED_CHECK(partial->surface_mesh->vertices[32U].position ==
               partial->retained_mesh->vertices.front().position);
  // Held, not rebuilt, while the authored key is unchanged.
  const auto again = half_cache.resolve(scene_frame, half);
  MAPPED_CHECK(again.has_value());
  MAPPED_CHECK(again->surface_mesh == partial->surface_mesh);

  // An output owning the whole raster already warps with the complete mesh.
  render::OutputProjectionCache whole_cache;
  const auto whole = whole_cache.resolve(scene_frame, arc_map(0.20F, 62U));
  MAPPED_CHECK(whole.has_value());
  MAPPED_CHECK(whole->surface_mesh == whole->retained_mesh);

  // An imported patch always owns its whole raster, so its file is the
  // complete mesh.
  Fixture files;
  files.write("complete.obj", patch_a);
  const auto imported = imported_map(files.path("complete.obj"), 63U);
  render::OutputProjectionCache imported_cache;
  static_cast<void>(imported_cache.resolve(scene_frame, imported));
  const auto loaded = wait_and_resolve(imported_cache, scene_frame, imported);
  MAPPED_CHECK(loaded.has_value());
  MAPPED_CHECK(loaded->surface_mesh != nullptr);
  MAPPED_CHECK(loaded->surface_mesh == loaded->retained_mesh);
  MAPPED_CHECK(loaded->mesh_placement.has_value());
}

constexpr std::array<Case, 9U> cases{{
    {"a mapped projection carries the complete surface mesh",
     &a_mapped_projection_carries_the_complete_surface_mesh},
    {"a camera stage carries flat and arc faces without rebuilding meshes",
     &a_camera_stage_carries_flat_and_arc_faces_without_rebuilding_meshes},
    {"authored refresh and diagnostic mesh reuse", &authored_refresh_generation_replaces_the_mesh_and_diagnostics_reuse_it},
    {"failed imports are sticky until an explicit refresh",
     &failed_imports_are_sticky_until_an_explicit_refresh},
    {"same-key refresh holds ready but authored changes do not",
     &same_key_refresh_holds_ready_but_authored_changes_do_not},
    {"animated imported placement reuses one local source generation",
     &animated_imported_placement_reuses_one_local_source_generation},
    {"an unchanged horizontal arc reuses one mesh allocation",
     &an_unchanged_horizontal_arc_reuses_one_mesh_allocation},
    {"resolve never waits for the publication lock",
     &resolve_never_waits_for_the_publication_lock},
    {"reset and teardown do not join an in-flight loader",
     &reset_and_teardown_do_not_join_an_in_flight_loader},
}};

} // namespace

int main() {
  std::size_t failures{};
  for (const Case &test : cases) {
    try {
      test.function();
      std::cout << "[pass] " << test.name << '\n';
    } catch (const std::exception &error) {
      ++failures;
      std::cerr << "[fail] " << test.name << ": " << error.what() << '\n';
    }
  }
  std::cout << cases.size() - failures << '/' << cases.size()
            << " tests passed\n";
  return failures == 0U ? 0 : 1;
}
