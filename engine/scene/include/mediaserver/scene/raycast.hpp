#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/geometry.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <stop_token>
#include <vector>

// Surface hits: where a line from the eye through a viewport pixel first meets
// the scene. Picking (`manipulate.hpp`) answers *which* element is under the
// pointer from projected bounds; this answers *where on it*, with the surface
// direction there, which is what placing something on a floor or a wall needs.
//
// Editor-only and headless. Nothing here enters the replicated document or a
// digest: a hit becomes authored state only through an ordinary edit.
namespace mediaserver::scene {

// A half-line. `direction` need not be unit length; distances returned against
// a ray are in multiples of it, so a world ray with a unit direction measures
// metres and the same ray carried into an element's local space keeps them.
struct Ray final {
    Vec3 origin{};
    Vec3 direction{0.0F, 0.0F, -1.0F};

    bool operator==(const Ray&) const = default;
};

// The world-space ray from the camera's eye through canvas pixel (x, y), y
// running down, through the same full-canvas projection `project_to_canvas`
// uses. Its direction is unit length. Empty for a degenerate camera or canvas.
// An orthographic camera's rays are parallel: each starts on the eye plane
// under its pixel and runs along the view.
[[nodiscard]] std::optional<Ray> canvas_ray(const Camera& camera, Canvas canvas,
                                            float canvas_x,
                                            float canvas_y) noexcept;

// A hit in the space of the ray it was found with. `normal` is unit length and
// faces back along the ray, whichever side of a two-sided surface was struck.
struct RayHit final {
    float distance{};
    Vec3 position{};
    Vec3 normal{};

    bool operator==(const RayHit&) const = default;
};

// The near face of an axis-aligned box, or its far face from inside it - a
// room the camera stands in is hit on the wall it faces.
[[nodiscard]] std::optional<RayHit>
raycast_box(const Ray& ray, Vec3 minimum, Vec3 maximum,
            float max_distance = std::numeric_limits<float>::infinity()) noexcept;

// A unit primitive exactly as `build_mesh` describes it, solved analytically
// rather than through its tessellation.
[[nodiscard]] std::optional<RayHit>
raycast_primitive(const Ray& ray, Primitive primitive,
                  float max_distance = std::numeric_limits<float>::infinity()) noexcept;

// A bounding-volume hierarchy over one mesh's triangles. Building it is the
// expensive part and belongs on a worker; a query is a few dozen box tests.
struct MeshBvhNode final {
    Vec3 minimum{};
    Vec3 maximum{};
    // A leaf (count > 0) owns `triangles[first, first + count)`. An interior
    // node's left child follows it directly and `first` is its right child.
    core::u32 first{};
    core::u32 count{};
};

struct MeshBvh final {
    std::vector<MeshBvhNode> nodes;
    // Triangle numbers (index-buffer offset / 3), in leaf order.
    std::vector<core::u32> triangles;

    [[nodiscard]] bool empty() const noexcept { return nodes.empty(); }
    [[nodiscard]] std::size_t bytes() const noexcept {
        return nodes.capacity() * sizeof(MeshBvhNode) +
               triangles.capacity() * sizeof(core::u32);
    }
};

// Triangles whose indices fall outside the vertex buffer, or which have no
// area, are left out. Empty when the mesh has no usable triangle or `stop` was
// requested part way through.
[[nodiscard]] MeshBvh build_mesh_bvh(const MeshData& mesh,
                                     std::stop_token stop = {});

// The nearest two-sided triangle hit. `bvh` must have been built from `mesh`.
[[nodiscard]] std::optional<RayHit>
raycast_mesh(const MeshBvh& bvh, const MeshData& mesh, const Ray& ray,
             float max_distance = std::numeric_limits<float>::infinity()) noexcept;

// Appends the triangles whose bounds come within `radius` of `centre`, at most
// `limit` of them, nearest leaves first - the candidates a vertex snap looks at
// around a surface hit. Returns false when the limit cut the search short.
bool mesh_triangles_near(const MeshBvh& bvh, const MeshData& mesh,
                         Vec3 centre, float radius,
                         std::vector<core::u32>& out, std::size_t limit);

// Supplies a hit for one imported model, against a ray already carried into
// the model's local space. The geometry owner decides how exact it can be
// right now (a mesh once its hierarchy is ready, its bounds until then).
using ModelRaycaster =
    std::function<std::optional<RayHit>(const ModelInstance&, const Ray& local,
                                        float max_distance)>;

// Which list of the Scene a surface hit came from, so a caller can reach the
// element's geometry (`Scene::instances`, `media` or `models` at `index`).
enum class SurfaceSource : core::u8 {
    Instance,
    Media,
    Model,
};

struct SurfaceHit final {
    doc::NodeId node{};
    SurfaceSource source{SurfaceSource::Instance};
    std::size_t index{};
    float distance{};
    Vec3 position{};
    Vec3 normal{};

    bool operator==(const SurfaceHit&) const = default;
};

// Says whether an element's surface takes part. Empty: every element does.
using SurfaceFilter = std::function<bool(doc::NodeId)>;

// The nearest surface along a world ray among procedural objects, media
// quads and imported models. Locked elements still count: a locked floor is
// still a floor to set things on. Lights and screens have no surface. Models
// without a raycaster are their unit-cube proxy, as in picking. Ties go to
// the lower node id. `counts` leaves elements out - what is being dragged,
// say, so it never lands on itself.
[[nodiscard]] std::optional<SurfaceHit>
raycast_scene(const Scene& scene, const Ray& world_ray,
              const ModelRaycaster& models = {},
              float max_distance = std::numeric_limits<float>::infinity(),
              const SurfaceFilter& counts = {});

} // namespace mediaserver::scene
