#include <mediaserver/editor/viewport_panel.hpp>

#include <mediaserver/render/model_loader.hpp>
#include <mediaserver/render/projection_mesh_loader.hpp>
#include <mediaserver/scene/manipulate.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <string>

namespace mediaserver::editor {
namespace {

constexpr float stage_toolbar_height = 29.0F;
constexpr float gizmo_length_pixels = 72.0F;
constexpr float gizmo_hit_pixels = 9.0F;
constexpr float surface_hit_pixels = 8.0F;
constexpr float surface_yaw_radius_pixels = 49.0F;
constexpr float surface_yaw_hit_pixels = 7.0F;
constexpr std::size_t maximum_imported_wire_triangles = 4096U;
constexpr double deep_select_hold_seconds = 0.45;
constexpr float deep_select_motion_pixels = 4.0F;
constexpr float deep_select_width = 210.0F;
constexpr float deep_select_header_height = 25.0F;
constexpr float deep_select_row_height = 23.0F;

constexpr gui::UiColor outline_shadow{0.0F, 0.02F, 0.03F, 0.88F};
constexpr gui::UiColor outline_secondary{0.18F, 0.72F, 0.92F, 0.92F};
constexpr gui::UiColor outline_primary{0.20F, 0.92F, 1.0F, 1.0F};
constexpr gui::UiColor axis_x{0.96F, 0.24F, 0.25F, 1.0F};
constexpr gui::UiColor axis_y{0.42F, 0.92F, 0.28F, 1.0F};
constexpr gui::UiColor axis_z{0.20F, 0.52F, 1.0F, 1.0F};
constexpr gui::UiColor light_color{1.0F, 0.78F, 0.24F, 0.92F};
constexpr gui::UiColor surface_color{0.16F, 0.68F, 0.88F, 0.68F};
constexpr gui::UiColor surface_selected{0.20F, 0.92F, 1.0F, 1.0F};
constexpr gui::UiColor surface_fallback{1.0F, 0.62F, 0.18F, 0.92F};
constexpr gui::UiColor surface_yaw{1.0F, 0.74F, 0.22F, 1.0F};

[[nodiscard]] gui::UiId mix(gui::UiId base, core::u64 value) noexcept {
    core::u64 hash = base ^ 0x9E3779B97F4A7C15ULL;
    hash ^= value;
    hash *= 1099511628211ULL;
    return hash;
}

[[nodiscard]] gui::UiPoint
to_ui(const gui::Rect& image, scene::Canvas canvas,
      const scene::CanvasProjection& point) noexcept {
    const float canvas_width = static_cast<float>(canvas.width);
    const float canvas_height = static_cast<float>(canvas.height);
    return gui::UiPoint{image.x + point.x * image.width / canvas_width,
                        image.y + point.y * image.height / canvas_height};
}

[[nodiscard]] bool selected(std::span<const doc::NodeId> selection,
                            doc::NodeId node) noexcept {
    return std::find(selection.begin(), selection.end(), node) !=
           selection.end();
}

[[nodiscard]] scene::Canvas viewport_canvas(
    const scene::Scene& view,
    const ViewportProjection* projection) noexcept {
    return projection != nullptr ? projection->canvas : view.canvas;
}

[[nodiscard]] scene::CanvasProjection project_world(
    const scene::Scene& view, const ViewportProjection* projection,
    scene::Vec3 point) {
    return projection != nullptr && projection->project
               ? projection->project(point)
               : scene::project_to_canvas(view.camera, view.canvas, point);
}

[[nodiscard]] std::string deep_select_label(const scene::Scene& view,
                                            doc::NodeId node) {
    for (const scene::MeshInstance& instance : view.instances) {
        if (instance.source == node) {
            return "Object  " + std::to_string(node.value());
        }
    }
    for (const scene::MediaInstance& media : view.media) {
        if (media.source_node == node) {
            return "Media  " + std::to_string(node.value());
        }
    }
    for (const scene::ModelInstance& model : view.models) {
        if (model.source_node == node) {
            return "Model  " + std::to_string(node.value());
        }
    }
    for (const scene::PointLight& light : view.point_lights) {
        if (light.source == node) {
            return "Point Light  " + std::to_string(node.value());
        }
    }
    for (const scene::DirectionalLight& light : view.directional_lights) {
        if (light.source == node) {
            return "Directional Light  " + std::to_string(node.value());
        }
    }
    return "Element  " + std::to_string(node.value());
}

void line(gui::UiDrawList& list, gui::UiPoint a, gui::UiPoint b,
          float thickness, gui::UiColor color) {
    list.add_line(a.x, a.y, b.x, b.y, thickness, color);
}

void projected_line(gui::UiDrawList& list, const gui::Rect& image,
                    const scene::Scene& view, scene::Vec3 a, scene::Vec3 b,
                    float thickness, gui::UiColor color) {
    const auto projected_a =
        scene::project_to_canvas(view.camera, view.canvas, a);
    const auto projected_b =
        scene::project_to_canvas(view.camera, view.canvas, b);
    if (!projected_a.visible || !projected_b.visible) {
        return;
    }
    line(list, to_ui(image, view.canvas, projected_a),
         to_ui(image, view.canvas, projected_b), thickness, color);
}

template <std::size_t PointCount, std::size_t EdgeCount>
void wire_shape(gui::UiDrawList& list, const gui::Rect& image,
                const scene::Scene& view, const scene::Mat4& world,
                const std::array<scene::Vec3, PointCount>& points,
                const std::array<std::array<std::size_t, 2U>, EdgeCount>& edges,
                gui::UiColor color,
                const ViewportProjection* projection = nullptr) {
    std::array<gui::UiPoint, PointCount> projected{};
    const scene::Canvas canvas = viewport_canvas(view, projection);
    for (std::size_t index = 0U; index < points.size(); ++index) {
        const auto canvas_point = project_world(
            view, projection, scene::transform_point(world, points[index]));
        if (!canvas_point.visible) {
            return;
        }
        projected[index] = to_ui(image, canvas, canvas_point);
    }
    for (const auto& edge : edges) {
        line(list, projected[edge[0U]], projected[edge[1U]], 5.0F,
             outline_shadow);
    }
    for (const auto& edge : edges) {
        line(list, projected[edge[0U]], projected[edge[1U]], 1.8F, color);
    }
}

void wire_box(gui::UiDrawList& list, const gui::Rect& image,
              const scene::Scene& view, const scene::Mat4& world,
              gui::UiColor color,
              scene::ModelLocalBounds bounds = scene::ModelLocalBounds{},
              const ViewportProjection* projection = nullptr) {
    const std::array<scene::Vec3, 8U> points{
        scene::Vec3{bounds.minimum.x, bounds.minimum.y, bounds.minimum.z},
        scene::Vec3{bounds.maximum.x, bounds.minimum.y, bounds.minimum.z},
        scene::Vec3{bounds.maximum.x, bounds.maximum.y, bounds.minimum.z},
        scene::Vec3{bounds.minimum.x, bounds.maximum.y, bounds.minimum.z},
        scene::Vec3{bounds.minimum.x, bounds.minimum.y, bounds.maximum.z},
        scene::Vec3{bounds.maximum.x, bounds.minimum.y, bounds.maximum.z},
        scene::Vec3{bounds.maximum.x, bounds.maximum.y, bounds.maximum.z},
        scene::Vec3{bounds.minimum.x, bounds.maximum.y, bounds.maximum.z}};
    static constexpr std::array<std::array<std::size_t, 2U>, 12U> edges{{
        {{0U, 1U}},
        {{1U, 2U}},
        {{2U, 3U}},
        {{3U, 0U}},
        {{4U, 5U}},
        {{5U, 6U}},
        {{6U, 7U}},
        {{7U, 4U}},
        {{0U, 4U}},
        {{1U, 5U}},
        {{2U, 6U}},
        {{3U, 7U}},
    }};
    wire_shape(list, image, view, world, points, edges, color, projection);
}

void wire_quad(gui::UiDrawList& list, const gui::Rect& image,
               const scene::Scene& view, const scene::Mat4& world, bool floor,
               gui::UiColor color,
               const ViewportProjection* projection = nullptr) {
    const std::array<scene::Vec3, 4U> points =
        floor ? std::array<scene::Vec3, 4U>{scene::Vec3{-0.5F, 0.0F, -0.5F},
                                            scene::Vec3{0.5F, 0.0F, -0.5F},
                                            scene::Vec3{0.5F, 0.0F, 0.5F},
                                            scene::Vec3{-0.5F, 0.0F, 0.5F}}
              : std::array<scene::Vec3, 4U>{scene::Vec3{-0.5F, -0.5F, 0.0F},
                                            scene::Vec3{0.5F, -0.5F, 0.0F},
                                            scene::Vec3{0.5F, 0.5F, 0.0F},
                                            scene::Vec3{-0.5F, 0.5F, 0.0F}};
    static constexpr std::array<std::array<std::size_t, 2U>, 4U> edges{{
        {{0U, 1U}},
        {{1U, 2U}},
        {{2U, 3U}},
        {{3U, 0U}},
    }};
    wire_shape(list, image, view, world, points, edges, color, projection);
}

void draw_light_icon(gui::UiDrawList& list, const gui::Rect& image,
                     const scene::Scene& view, scene::Vec3 position,
                     gui::UiColor color, bool sun) {
    const auto projected =
        scene::project_to_canvas(view.camera, view.canvas, position);
    if (!projected.visible) {
        return;
    }
    const gui::UiPoint centre = to_ui(image, view.canvas, projected);
    const float radius = sun ? 8.0F : 6.0F;
    static constexpr std::array<gui::UiPoint, 8U> directions{
        gui::UiPoint{1.0F, 0.0F},  gui::UiPoint{0.707F, 0.707F},
        gui::UiPoint{0.0F, 1.0F},  gui::UiPoint{-0.707F, 0.707F},
        gui::UiPoint{-1.0F, 0.0F}, gui::UiPoint{-0.707F, -0.707F},
        gui::UiPoint{0.0F, -1.0F}, gui::UiPoint{0.707F, -0.707F}};
    for (const auto direction : directions) {
        const gui::UiPoint inner{centre.x + direction.x * radius,
                                 centre.y + direction.y * radius};
        const gui::UiPoint outer{
            centre.x + direction.x * (radius + (sun ? 6.0F : 3.0F)),
            centre.y + direction.y * (radius + (sun ? 6.0F : 3.0F))};
        line(list, inner, outer, 1.5F, color);
    }
    const std::array<gui::UiPoint, 4U> diamond{
        gui::UiPoint{centre.x, centre.y - radius},
        gui::UiPoint{centre.x + radius, centre.y},
        gui::UiPoint{centre.x, centre.y + radius},
        gui::UiPoint{centre.x - radius, centre.y}};
    for (std::size_t index = 0U; index < diamond.size(); ++index) {
        line(list, diamond[index], diamond[(index + 1U) % diamond.size()], 1.8F,
             color);
    }
}

void draw_grid(gui::UiDrawList& list, const gui::Rect& image,
               const scene::Scene& view) {
    constexpr int half_grid = 10;
    constexpr float spacing = 1.0F;
    for (int line_index = -half_grid; line_index <= half_grid; ++line_index) {
        const float offset = static_cast<float>(line_index) * spacing;
        const bool major = line_index == 0 || line_index % 5 == 0;
        const gui::UiColor grid =
            major ? gui::UiColor{0.30F, 0.38F, 0.48F, 0.34F}
                  : gui::UiColor{0.22F, 0.28F, 0.36F, 0.18F};
        projected_line(
            list, image, view, scene::Vec3{-10.0F, 0.0F, offset},
            scene::Vec3{10.0F, 0.0F, offset}, major ? 1.2F : 1.0F,
            line_index == 0 ? gui::UiColor{0.72F, 0.18F, 0.18F, 0.45F} : grid);
        projected_line(
            list, image, view, scene::Vec3{offset, 0.0F, -10.0F},
            scene::Vec3{offset, 0.0F, 10.0F}, major ? 1.2F : 1.0F,
            line_index == 0 ? gui::UiColor{0.16F, 0.34F, 0.82F, 0.50F} : grid);
    }
}

void draw_camera_overlay(gui::UiDrawList& list, const gui::Rect& image,
                         const scene::Scene& view,
                         const scene::Camera& camera) {
    const scene::Vec3 forward = scene::normalize(camera.target - camera.eye);
    const scene::Vec3 right = scene::normalize(scene::cross(forward, camera.up),
                                               scene::Vec3{1.0F, 0.0F, 0.0F});
    const scene::Vec3 up = scene::cross(right, forward);
    const float distance = std::clamp(
        scene::length(camera.target - camera.eye) * 0.35F, 1.0F, 4.0F);
    const float half_height =
        distance * scene::deterministic::tan_half_turns(camera.field_of_view);
    const float aspect = view.canvas.height == 0U
                             ? 1.0F
                             : static_cast<float>(view.canvas.width) /
                                   static_cast<float>(view.canvas.height);
    const float half_width = half_height * aspect;
    const scene::Vec3 centre = camera.eye + forward * distance;
    const std::array<scene::Vec3, 4U> corners{
        centre - right * half_width - up * half_height,
        centre + right * half_width - up * half_height,
        centre + right * half_width + up * half_height,
        centre - right * half_width + up * half_height};
    const gui::UiColor frustum{0.55F, 0.62F, 0.76F, 0.72F};
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        projected_line(list, image, view, camera.eye, corners[index], 1.2F,
                       frustum);
        projected_line(list, image, view, corners[index],
                       corners[(index + 1U) % corners.size()], 1.2F, frustum);
    }
    draw_light_icon(list, image, view, camera.eye, frustum, false);
}

void draw_light_overlays(gui::UiDrawList& list, const gui::Rect& image,
                         const scene::Scene& view,
                         std::span<const doc::NodeId> selection,
                         doc::NodeId primary) {
    for (const auto& light : view.point_lights) {
        const bool active = selected(selection, light.source);
        const gui::UiColor color =
            active ? (light.source == primary ? outline_primary
                                              : outline_secondary)
                   : light_color;
        draw_light_icon(list, image, view, light.position, color, false);

        // A camera-facing range ring communicates the light's influence without
        // adding a 3D mesh to the output renderer.
        const scene::Vec3 forward =
            scene::normalize(view.camera.target - view.camera.eye);
        const scene::Vec3 right =
            scene::normalize(scene::cross(forward, view.camera.up),
                             scene::Vec3{1.0F, 0.0F, 0.0F});
        const scene::Vec3 up = scene::cross(right, forward);
        constexpr std::size_t segments = 32U;
        const float radius = std::clamp(light.range, 0.25F, 8.0F);
        scene::Vec3 previous = light.position + right * radius;
        for (std::size_t index = 1U; index <= segments; ++index) {
            const float turns =
                static_cast<float>(index) / static_cast<float>(segments);
            const scene::Vec3 next =
                light.position +
                right * (scene::deterministic::cos_turns(turns) * radius) +
                up * (scene::deterministic::sin_turns(turns) * radius);
            projected_line(list, image, view, previous, next, 1.0F,
                           gui::UiColor{color.r, color.g, color.b,
                                        active ? 0.42F : 0.18F});
            previous = next;
        }
    }
    for (const auto& light : view.directional_lights) {
        const scene::Vec3 anchor = scene::directional_light_anchor(view, light);
        const bool active = selected(selection, light.source);
        const gui::UiColor color =
            active ? (light.source == primary ? outline_primary
                                              : outline_secondary)
                   : light_color;
        draw_light_icon(list, image, view, anchor, color, true);
        projected_line(list, image, view, anchor,
                       anchor + scene::normalize(light.direction) * 1.4F,
                       active ? 2.4F : 1.5F, color);
    }
}

void draw_selection_outlines(gui::UiDrawList& list, const gui::Rect& image,
                             const scene::Scene& view,
                             std::span<const doc::NodeId> selection,
                             doc::NodeId primary,
                             const scene::ModelBoundsProvider& model_bounds,
                             const ViewportProjection* projection = nullptr) {
    for (const auto& object : view.instances) {
        if (!selected(selection, object.source)) {
            continue;
        }
        const gui::UiColor color =
            object.source == primary ? outline_primary : outline_secondary;
        if (object.mesh.primitive == scene::Primitive::Quad ||
            object.mesh.primitive == scene::Primitive::Plane) {
            wire_quad(list, image, view, object.world,
                      object.mesh.primitive == scene::Primitive::Plane, color,
                      projection);
        } else {
            wire_box(list, image, view, object.world, color,
                     scene::ModelLocalBounds{}, projection);
        }
    }
    for (const auto& clip : view.media) {
        if (selected(selection, clip.source_node)) {
            wire_quad(list, image, view, clip.world, false,
                      clip.source_node == primary ? outline_primary
                                                  : outline_secondary,
                      projection);
        }
    }
    for (const auto& model : view.models) {
        if (selected(selection, model.source_node)) {
            const auto bounds =
                model_bounds ? model_bounds(model) : std::nullopt;
            wire_box(list, image, view, model.world,
                     model.source_node == primary ? outline_primary
                                                  : outline_secondary,
                     bounds.value_or(scene::ModelLocalBounds{}), projection);
        }
    }
}

struct GizmoGeometry final {
    bool visible{};
    gui::UiPoint origin{};
    std::array<gui::UiPoint, 3U> ends{};
    std::array<float, 3U> canvas_dx{};
    std::array<float, 3U> canvas_dy{};
    float world_length{};
};

[[nodiscard]] GizmoGeometry gizmo_geometry(const gui::Rect& image,
                                           const scene::Scene& view,
                                           scene::Vec3 world) noexcept {
    GizmoGeometry result{};
    if (view.canvas.height == 0U || image.height <= 0.0F) {
        return result;
    }
    const scene::Vec3 forward =
        scene::normalize(view.camera.target - view.camera.eye);
    const float depth = scene::dot(world - view.camera.eye, forward);
    if (!(depth > 0.0F)) {
        return result;
    }
    const float canvas_pixels = gizmo_length_pixels *
                                static_cast<float>(view.canvas.height) /
                                image.height;
    const float units_per_pixel =
        2.0F * depth *
        scene::deterministic::tan_half_turns(view.camera.field_of_view) /
        static_cast<float>(view.canvas.height);
    result.world_length = units_per_pixel * canvas_pixels;

    const auto origin =
        scene::project_to_canvas(view.camera, view.canvas, world);
    if (!origin.visible) {
        return result;
    }
    result.origin = to_ui(image, view.canvas, origin);
    static constexpr std::array<scene::Vec3, 3U> axes{
        scene::Vec3{1.0F, 0.0F, 0.0F}, scene::Vec3{0.0F, 1.0F, 0.0F},
        scene::Vec3{0.0F, 0.0F, 1.0F}};
    for (std::size_t index = 0U; index < axes.size(); ++index) {
        const auto endpoint =
            scene::project_to_canvas(view.camera, view.canvas,
                                     world + axes[index] * result.world_length);
        if (!endpoint.visible) {
            return GizmoGeometry{};
        }
        result.canvas_dx[index] = endpoint.x - origin.x;
        result.canvas_dy[index] = endpoint.y - origin.y;
        result.ends[index] = to_ui(image, view.canvas, endpoint);
    }
    result.visible = true;
    return result;
}

[[nodiscard]] float distance_to_segment(float px, float py, gui::UiPoint a,
                                        gui::UiPoint b) noexcept {
    const float vx = b.x - a.x;
    const float vy = b.y - a.y;
    const float length_squared = vx * vx + vy * vy;
    if (!(length_squared > 0.0F)) {
        return std::numeric_limits<float>::infinity();
    }
    const float t = std::clamp(
        ((px - a.x) * vx + (py - a.y) * vy) / length_squared, 0.18F, 1.12F);
    const float dx = px - (a.x + vx * t);
    const float dy = py - (a.y + vy * t);
    return std::sqrt(dx * dx + dy * dy);
}

[[nodiscard]] float distance_to_full_segment(float px, float py,
                                             gui::UiPoint a,
                                             gui::UiPoint b) noexcept {
    const float vx = b.x - a.x;
    const float vy = b.y - a.y;
    const float length_squared = vx * vx + vy * vy;
    if (!(length_squared > 0.0F)) {
        return std::numeric_limits<float>::infinity();
    }
    const float t = std::clamp(
        ((px - a.x) * vx + (py - a.y) * vy) / length_squared, 0.0F, 1.0F);
    const float dx = px - (a.x + vx * t);
    const float dy = py - (a.y + vy * t);
    return std::sqrt(dx * dx + dy * dy);
}

struct SurfaceVisual final {
    std::vector<scene::Vec3> vertices;
    std::vector<std::array<std::size_t, 2U>> edges;
    bool fallback{};
};

[[nodiscard]] SurfaceVisual
planar_surface_visual(const output::ProjectionSurface& surface,
                      bool fallback = false) {
    SurfaceVisual visual{};
    visual.fallback = fallback;
    const auto corners = output::surface_corners(
        surface.geometry, scene::whole(surface.geometry.raster));
    if (!corners) {
        return visual;
    }
    const output::SurfaceCorners value = corners.value();
    visual.vertices = {value.upper_left, value.upper_right, value.lower_right,
                       value.lower_left};
    visual.edges = {{{0U, 1U}}, {{1U, 2U}}, {{2U, 3U}}, {{3U, 0U}}};
    return visual;
}

[[nodiscard]] SurfaceVisual
mesh_surface_visual(const output::ProjectionMesh& mesh,
                    bool horizontal_arc,
                    const output::PhysicalSurface* imported_calibration =
                        nullptr) {
    SurfaceVisual visual{};
    visual.vertices.reserve(mesh.vertices.size());
    scene::Vec3 imported_right{};
    scene::Vec3 imported_up{};
    scene::Vec3 imported_front{};
    if (imported_calibration != nullptr) {
        imported_right = scene::normalize(imported_calibration->right,
                                          scene::Vec3{1.0F, 0.0F, 0.0F});
        imported_up = scene::normalize(
            imported_calibration->up -
                imported_right *
                    scene::dot(imported_calibration->up, imported_right),
            scene::Vec3{0.0F, 1.0F, 0.0F});
        imported_front = scene::normalize(
            scene::cross(imported_right, imported_up),
            scene::Vec3{0.0F, 0.0F, 1.0F});
    }
    for (const output::ProjectionMeshVertex& vertex : mesh.vertices) {
        if (imported_calibration == nullptr) {
            visual.vertices.push_back(vertex.position);
        } else {
            visual.vertices.push_back(
                imported_calibration->center +
                imported_right * vertex.position.x +
                imported_up * vertex.position.y +
                imported_front * vertex.position.z);
        }
    }
    if (horizontal_arc) {
        // horizontal_arc_mesh is an exact top/bottom strip. Showing each
        // measured/generated column makes the authored curvature legible
        // without covering the scene with triangle diagonals.
        const std::size_t columns = visual.vertices.size() / 2U;
        if (columns >= 2U) {
            visual.edges.reserve(columns * 3U - 2U);
            for (std::size_t column = 0U; column < columns; ++column) {
                visual.edges.push_back({column * 2U, column * 2U + 1U});
                if (column + 1U < columns) {
                    visual.edges.push_back(
                        {column * 2U, (column + 1U) * 2U});
                    visual.edges.push_back(
                        {column * 2U + 1U, (column + 1U) * 2U + 1U});
                }
            }
        }
        return visual;
    }

    // Imported walls can be production-sized. Keep the viewport overlay
    // bounded while retaining the actual calibrated vertex positions. Small
    // meshes show every triangle; large ones are sampled at a stable stride.
    const std::size_t triangle_count = mesh.indices.size() / 3U;
    const std::size_t stride = std::max<std::size_t>(
        1U, (triangle_count + maximum_imported_wire_triangles - 1U) /
                maximum_imported_wire_triangles);
    visual.edges.reserve(std::min(triangle_count,
                                  maximum_imported_wire_triangles) *
                         3U);
    for (std::size_t triangle = 0U; triangle < triangle_count;
         triangle += stride) {
        const std::size_t base = triangle * 3U;
        const std::size_t a = mesh.indices[base];
        const std::size_t b = mesh.indices[base + 1U];
        const std::size_t c = mesh.indices[base + 2U];
        if (a >= visual.vertices.size() || b >= visual.vertices.size() ||
            c >= visual.vertices.size()) {
            continue;
        }
        visual.edges.push_back({a, b});
        visual.edges.push_back({b, c});
        visual.edges.push_back({c, a});
    }
    return visual;
}

struct ProjectedSurface final {
    const output::ProjectionSurface* surface{};
    output::ProjectionSurface display;
    SurfaceVisual visual;
    std::vector<gui::UiPoint> points;
    std::vector<bool> visible;
};

void project_surface(ProjectedSurface& surface, const gui::Rect& image,
                     const scene::Scene& view) {
    surface.points.resize(surface.visual.vertices.size());
    surface.visible.resize(surface.visual.vertices.size());
    for (std::size_t index = 0U; index < surface.visual.vertices.size();
         ++index) {
        const scene::CanvasProjection projected = scene::project_to_canvas(
            view.camera, view.canvas, surface.visual.vertices[index]);
        surface.visible[index] = projected.visible;
        if (projected.visible) {
            surface.points[index] = to_ui(image, view.canvas, projected);
        }
    }
}

[[nodiscard]] float surface_hit_distance(const ProjectedSurface& surface,
                                         float pointer_x,
                                         float pointer_y) noexcept {
    float closest = std::numeric_limits<float>::infinity();
    for (const auto& edge : surface.visual.edges) {
        if (edge[0U] >= surface.points.size() ||
            edge[1U] >= surface.points.size() ||
            !surface.visible[edge[0U]] || !surface.visible[edge[1U]]) {
            continue;
        }
        closest = std::min(
            closest,
            distance_to_full_segment(pointer_x, pointer_y,
                                     surface.points[edge[0U]],
                                     surface.points[edge[1U]]));
    }
    return closest;
}

void draw_surface(gui::UiDrawList& list, const ProjectedSurface& surface,
                  bool selected_surface) {
    const gui::UiColor color =
        surface.visual.fallback
            ? surface_fallback
            : (selected_surface ? surface_selected : surface_color);
    for (const auto& edge : surface.visual.edges) {
        if (edge[0U] >= surface.points.size() ||
            edge[1U] >= surface.points.size() ||
            !surface.visible[edge[0U]] || !surface.visible[edge[1U]]) {
            continue;
        }
        line(list, surface.points[edge[0U]], surface.points[edge[1U]],
             selected_surface ? 4.5F : 3.5F, outline_shadow);
        line(list, surface.points[edge[0U]], surface.points[edge[1U]],
             selected_surface ? 2.2F : 1.25F, color);
    }
}

[[nodiscard]] bool hit_yaw_ring(const GizmoGeometry& gizmo, float pointer_x,
                                float pointer_y) noexcept {
    if (!gizmo.visible) {
        return false;
    }
    const float dx = pointer_x - gizmo.origin.x;
    const float dy = pointer_y - gizmo.origin.y;
    const float radius = std::sqrt(dx * dx + dy * dy);
    return std::abs(radius - surface_yaw_radius_pixels) <=
           surface_yaw_hit_pixels;
}

void draw_yaw_ring(gui::UiDrawList& list, const GizmoGeometry& gizmo,
                   bool hovered, bool active) {
    if (!gizmo.visible) {
        return;
    }
    constexpr std::size_t segments = 48U;
    const gui::UiColor color = hovered || active
                                   ? gui::UiColor{1.0F, 0.94F, 0.56F, 1.0F}
                                   : surface_yaw;
    gui::UiPoint previous{gizmo.origin.x + surface_yaw_radius_pixels,
                          gizmo.origin.y};
    for (std::size_t index = 1U; index <= segments; ++index) {
        const float turns =
            static_cast<float>(index) / static_cast<float>(segments);
        const gui::UiPoint next{
            gizmo.origin.x + scene::deterministic::cos_turns(turns) *
                                   surface_yaw_radius_pixels,
            gizmo.origin.y + scene::deterministic::sin_turns(turns) *
                                   surface_yaw_radius_pixels};
        line(list, previous, next, 4.5F, outline_shadow);
        line(list, previous, next, active ? 2.8F : 1.8F, color);
        previous = next;
    }
    const gui::UiPoint marker{gizmo.origin.x + surface_yaw_radius_pixels,
                              gizmo.origin.y};
    list.add_triangle(marker.x + 6.0F, marker.y, marker.x - 3.0F,
                      marker.y - 4.5F, marker.x - 3.0F, marker.y + 4.5F,
                      color);
}

[[nodiscard]] GizmoAxis hit_gizmo(const GizmoGeometry& gizmo, float pointer_x,
                                  float pointer_y) noexcept {
    if (!gizmo.visible) {
        return GizmoAxis::None;
    }
    GizmoAxis best = GizmoAxis::None;
    float best_distance = gizmo_hit_pixels;
    for (std::size_t index = 0U; index < gizmo.ends.size(); ++index) {
        const float screen_dx = gizmo.ends[index].x - gizmo.origin.x;
        const float screen_dy = gizmo.ends[index].y - gizmo.origin.y;
        if (screen_dx * screen_dx + screen_dy * screen_dy < 18.0F * 18.0F) {
            continue;
        }
        const float distance = distance_to_segment(
            pointer_x, pointer_y, gizmo.origin, gizmo.ends[index]);
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<GizmoAxis>(static_cast<core::u8>(GizmoAxis::X) +
                                          static_cast<core::u8>(index));
        }
    }
    return best;
}

[[nodiscard]] std::size_t axis_index(GizmoAxis axis) noexcept {
    return static_cast<std::size_t>(static_cast<core::u8>(axis) -
                                    static_cast<core::u8>(GizmoAxis::X));
}

[[nodiscard]] scene::Vec3 axis_vector(GizmoAxis axis) noexcept {
    switch (axis) {
    case GizmoAxis::X:
        return scene::Vec3{1.0F, 0.0F, 0.0F};
    case GizmoAxis::Y:
        return scene::Vec3{0.0F, 1.0F, 0.0F};
    case GizmoAxis::Z:
        return scene::Vec3{0.0F, 0.0F, 1.0F};
    case GizmoAxis::None:
    case GizmoAxis::Screen:
        break;
    }
    return scene::Vec3{};
}

void draw_gizmo(gui::UiDrawList& list, const GizmoGeometry& gizmo,
                GizmoAxis hovered, GizmoAxis active) {
    if (!gizmo.visible) {
        return;
    }
    static constexpr std::array<gui::UiColor, 3U> colors{axis_x, axis_y,
                                                         axis_z};
    for (std::size_t index = 0U; index < gizmo.ends.size(); ++index) {
        const GizmoAxis axis = static_cast<GizmoAxis>(
            static_cast<core::u8>(GizmoAxis::X) + static_cast<core::u8>(index));
        gui::UiColor color = colors[index];
        const bool emphasized = hovered == axis || active == axis;
        if (emphasized) {
            color = gui::UiColor{1.0F, 0.94F, 0.56F, 1.0F};
        }
        line(list, gizmo.origin, gizmo.ends[index], 5.0F, outline_shadow);
        line(list, gizmo.origin, gizmo.ends[index], emphasized ? 3.2F : 2.2F,
             color);

        const float dx = gizmo.ends[index].x - gizmo.origin.x;
        const float dy = gizmo.ends[index].y - gizmo.origin.y;
        const float magnitude = std::sqrt(dx * dx + dy * dy);
        if (magnitude > 1.0F) {
            const float nx = dx / magnitude;
            const float ny = dy / magnitude;
            const float px = -ny;
            const float py = nx;
            list.add_triangle(gizmo.ends[index].x, gizmo.ends[index].y,
                              gizmo.ends[index].x - nx * 10.0F + px * 4.5F,
                              gizmo.ends[index].y - ny * 10.0F + py * 4.5F,
                              gizmo.ends[index].x - nx * 10.0F - px * 4.5F,
                              gizmo.ends[index].y - ny * 10.0F - py * 4.5F,
                              color);
        }
    }
    list.add_rect(gizmo.origin.x - 3.0F, gizmo.origin.y - 3.0F, 6.0F, 6.0F,
                  gui::UiColor{0.90F, 0.92F, 0.96F, 1.0F});
}

} // namespace

std::optional<scene::ModelLocalBounds>
ViewportPanel::model_bounds_for(const scene::ModelInstance& model) {
    if (model.source.empty()) {
        return std::nullopt;
    }
    const auto held = model_bounds_cache_.find(model.source);
    if (held != model_bounds_cache_.end()) {
        return held->second;
    }

    render::ModelAsset asset{};
    if (!render::load_model(model.source, asset)) {
        return std::nullopt;
    }
    const scene::ModelLocalBounds bounds{asset.bounds.minimum,
                                          asset.bounds.maximum};
    model_bounds_cache_.emplace(model.source, bounds);
    return bounds;
}

const output::ProjectionMesh* ViewportPanel::projection_mesh_for(
    const output::ProjectionSurface& surface) {
    const core::u64 key = surface.node.value();
    std::error_code timestamp_error;
    const auto timestamp =
        std::filesystem::last_write_time(surface.mesh_source, timestamp_error);
    const core::i64 source_stamp =
        timestamp_error
            ? 0
            : static_cast<core::i64>(timestamp.time_since_epoch().count());
    auto held = projection_mesh_cache_.find(key);
    if (held != projection_mesh_cache_.end() &&
        held->second.source == surface.mesh_source &&
        held->second.scale == surface.mesh_scale &&
        held->second.source_stamp == source_stamp) {
        return held->second.mesh.has_value() ? &held->second.mesh.value()
                                             : nullptr;
    }

    CachedProjectionMesh decoded{};
    decoded.source = surface.mesh_source;
    decoded.scale = surface.mesh_scale;
    decoded.source_stamp = source_stamp;
    output::PhysicalSurface canonical = surface.geometry;
    canonical.center = scene::Vec3{};
    canonical.right = scene::Vec3{1.0F, 0.0F, 0.0F};
    canonical.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    auto loaded = render::load_projection_mesh(
        surface.mesh_source, canonical, surface.mesh_scale);
    if (loaded) {
        decoded.mesh.emplace(std::move(loaded).value());
    }
    auto [inserted, unused] = projection_mesh_cache_.insert_or_assign(
        key, std::move(decoded));
    static_cast<void>(unused);
    return inserted->second.mesh.has_value() ? &inserted->second.mesh.value()
                                             : nullptr;
}

CanvasPoint to_canvas(const gui::Rect& image, scene::Canvas canvas,
                      float pointer_x, float pointer_y) noexcept {
    CanvasPoint point{};
    if (image.width <= 0.0F || image.height <= 0.0F) {
        return point;
    }
    point.x = ((pointer_x - image.x) / image.width) *
              static_cast<float>(canvas.width);
    point.y = ((pointer_y - image.y) / image.height) *
              static_cast<float>(canvas.height);
    point.inside = image.contains(pointer_x, pointer_y);
    return point;
}

ViewportFrame ViewportPanel::draw(gui::UiContext& ctx, const gui::Rect& bounds,
                                  const rhi::Texture& target,
                                  const scene::Scene& scene,
                                  std::span<const doc::NodeId> selection,
                                  doc::NodeId primary,
                                  const scene::Camera* camera_overlay,
                                  const gui::Rect* interaction_occluder,
                                  float frames_per_second,
                                  const output::FeedMap* feed_map,
                                  doc::NodeId selected_surface_node,
                                  const ViewportProjection* projection) {
    ViewportFrame frame{};

    const scene::Canvas interaction_canvas =
        viewport_canvas(scene, projection);
    const auto canvas_width = static_cast<float>(interaction_canvas.width);
    const auto canvas_height = static_cast<float>(interaction_canvas.height);
    if (canvas_width <= 0.0F || canvas_height <= 0.0F) {
        return frame;
    }
    const scene::ModelBoundsProvider model_bounds =
        [this](const scene::ModelInstance& model) {
            return model_bounds_for(model);
        };

    gui::Rect image_bounds = bounds;
    if (role_ == ViewportRole::Stage && bounds.height > stage_toolbar_height) {
        const gui::Rect toolbar{bounds.x, bounds.y, bounds.width,
                                stage_toolbar_height};
        ctx.panel(toolbar, ctx.theme().background);
        ctx.panel(gui::Rect{toolbar.x, toolbar.y + toolbar.height - 1.0F,
                            toolbar.width, 1.0F},
                  ctx.theme().divider);
        float left = toolbar.x + 7.0F;
        ctx.label(gui::Rect{left, toolbar.y, 34.0F, toolbar.height}, "VIEW",
                  ctx.theme().text_muted);
        left += 37.0F;
        const gui::Rect camera{left, toolbar.y + 4.0F, 104.0F, 20.0F};
        ctx.panel(camera, ctx.theme().widget);
        ctx.label(gui::Rect{camera.x + 7.0F, camera.y, camera.width - 7.0F,
                            camera.height},
                  "Camera_Main  v", ctx.theme().text);
        left += 109.0F;
        const auto disabled_tool = [&](float x, float width,
                                       std::string_view label) {
            const gui::Rect tool{x, toolbar.y + 4.0F, width, 20.0F};
            ctx.panel(tool, ctx.theme().track);
            ctx.label(gui::Rect{tool.x + 7.0F, tool.y,
                                std::max(tool.width - 7.0F, 0.0F),
                                tool.height},
                      label, ctx.theme().text_muted);
        };
        disabled_tool(left, 24.0F, "S");
        left += 28.0F;
        disabled_tool(left, 24.0F, "M");
        left += 30.0F;
        disabled_tool(left, 28.0F, "2D");
        left += 32.0F;
        ctx.panel(gui::Rect{left, toolbar.y + 4.0F, 28.0F, 20.0F},
                  ctx.theme().widget_active);
        ctx.label(gui::Rect{left + 6.0F, toolbar.y + 4.0F, 22.0F, 20.0F}, "3D",
                  ctx.theme().text);
        left += 34.0F;

        float button_x = left;
        if (ctx.button(mix(id_, 1U),
                       gui::Rect{button_x, toolbar.y + 4.0F, 44.0F, 20.0F},
                       show_grid_ ? "GRID" : "grid")) {
            show_grid_ = !show_grid_;
        }
        button_x += 48.0F;
        if (ctx.button(mix(id_, 2U),
                       gui::Rect{button_x, toolbar.y + 4.0F, 52.0F, 20.0F},
                       show_overlays_ ? "GUIDES" : "guides")) {
            show_overlays_ = !show_overlays_;
        }
        button_x += 56.0F;
        if (ctx.button(mix(id_, 3U),
                       gui::Rect{button_x, toolbar.y + 4.0F, 50.0F, 20.0F},
                       show_gizmo_ ? "GIZMO" : "gizmo")) {
            show_gizmo_ = !show_gizmo_;
        }
        const float quality_x = toolbar.x + toolbar.width - 144.0F;
        if (quality_x > button_x + 58.0F) {
            ctx.label(
                gui::Rect{quality_x - 49.0F, toolbar.y, 46.0F, toolbar.height},
                "QUALITY", ctx.theme().text_muted);
            ctx.panel(gui::Rect{quality_x, toolbar.y + 4.0F, 78.0F, 20.0F},
                      ctx.theme().widget);
            ctx.label(
                gui::Rect{quality_x + 7.0F, toolbar.y + 4.0F, 71.0F, 20.0F},
                "HIGH  v", ctx.theme().text);
            ctx.panel(
                gui::Rect{quality_x + 84.0F, toolbar.y + 4.0F, 54.0F, 20.0F},
                ctx.theme().widget);
            std::array<char, 24U> fps_text{};
            const int fps_length = std::snprintf(
                fps_text.data(), fps_text.size(), "%.3g FPS",
                std::max(frames_per_second, 0.0F));
            ctx.label(
                gui::Rect{quality_x + 89.0F, toolbar.y + 4.0F, 49.0F, 20.0F},
                std::string_view{
                    fps_text.data(), static_cast<std::size_t>(
                                         std::max(fps_length, 0))},
                ctx.theme().text);
        }
        image_bounds.y += stage_toolbar_height;
        image_bounds.height -= stage_toolbar_height;
    }

    // Letterboxed to the canvas, never stretched: a pixel the operator clicks
    // has to be the pixel the renderer drew, or picking lies.
    frame.image =
        ctx.image_fit(image_bounds, target, canvas_width / canvas_height);
    if (frame.image.width <= 0.0F || frame.image.height <= 0.0F) {
        return frame;
    }

    if (surface_preview_.has_value()) {
        const output::ProjectionSurface* const committed =
            feed_map != nullptr
                ? feed_map->surface_for(surface_preview_->node)
                : nullptr;
        const bool committed_preview =
            committed != nullptr && *committed == *surface_preview_;
        const bool changed_selection =
            selected_surface_node != surface_preview_->node;
        const bool expired = !surface_dragging_ &&
                             ctx.time_seconds() > surface_preview_deadline_;
        if (committed_preview || changed_selection || expired) {
            surface_preview_.reset();
        }
    }

    std::vector<ProjectedSurface> projected_surfaces;
    const output::ProjectionSurface* selected_surface_value =
        feed_map != nullptr
            ? feed_map->surface_for(selected_surface_node)
            : nullptr;
    if (surface_preview_.has_value() &&
        surface_preview_->node == selected_surface_node) {
        selected_surface_value = &surface_preview_.value();
    }
    if (role_ == ViewportRole::Stage && show_overlays_ && feed_map != nullptr) {
        projected_surfaces.reserve(feed_map->surfaces.size());
        for (const output::ProjectionSurface& surface : feed_map->surfaces) {
            ProjectedSurface projected{};
            projected.surface = &surface;
            projected.display =
                surface_preview_.has_value() &&
                        surface_preview_->node == surface.node
                    ? surface_preview_.value()
                    : surface;
            const output::ProjectionSurface& display = projected.display;
            switch (display.shape) {
            case output::ProjectionSurfaceShape::Planar:
                projected.visual = planar_surface_visual(display);
                break;
            case output::ProjectionSurfaceShape::HorizontalArc: {
                auto mesh = output::horizontal_arc_mesh(
                    display.geometry, display.horizontal_arc_turns,
                    display.mesh_columns);
                projected.visual = mesh
                                       ? mesh_surface_visual(mesh.value(), true)
                                       : planar_surface_visual(display, true);
                break;
            }
            case output::ProjectionSurfaceShape::ImportedMesh: {
                const output::ProjectionMesh* const mesh =
                    projection_mesh_for(display);
                projected.visual =
                    mesh != nullptr
                        ? mesh_surface_visual(*mesh, false, &display.geometry)
                        : planar_surface_visual(display, true);
                break;
            }
            }
            project_surface(projected, frame.image, scene);
            projected_surfaces.push_back(std::move(projected));
        }
    }

    const auto primary_world = scene::world_for(scene, primary);
    const GizmoGeometry scene_gizmo =
        role_ == ViewportRole::Stage && show_gizmo_ &&
                selected_surface_value == nullptr && primary_world.has_value()
            ? gizmo_geometry(frame.image, scene,
                             scene::translation_of(*primary_world))
            : GizmoGeometry{};
    GizmoGeometry surface_gizmo =
        role_ == ViewportRole::Stage && show_gizmo_ &&
                selected_surface_value != nullptr
            ? gizmo_geometry(frame.image, scene,
                             selected_surface_value->geometry.center)
            : GizmoGeometry{};
    const bool occluded =
        interaction_occluder != nullptr &&
        interaction_occluder->contains(ctx.pointer_x(), ctx.pointer_y());
    const GizmoAxis hovered_scene_axis =
        occluded ? GizmoAxis::None
                 : hit_gizmo(scene_gizmo, ctx.pointer_x(), ctx.pointer_y());
    GizmoAxis hovered_surface_axis =
        occluded ? GizmoAxis::None
                 : hit_gizmo(surface_gizmo, ctx.pointer_x(), ctx.pointer_y());
    const bool hovered_surface_yaw =
        !occluded && hovered_surface_axis == GizmoAxis::None &&
        hit_yaw_ring(surface_gizmo, ctx.pointer_x(), ctx.pointer_y());
    if (hovered_surface_yaw) {
        hovered_surface_axis = GizmoAxis::Yaw;
    }

    const ProjectedSurface* hit_surface = nullptr;
    if (!occluded) {
        float closest_surface = surface_hit_pixels;
        for (const ProjectedSurface& projected : projected_surfaces) {
            const float distance = surface_hit_distance(
                projected, ctx.pointer_x(), ctx.pointer_y());
            if (distance < closest_surface) {
                closest_surface = distance;
                hit_surface = &projected;
            }
        }
    }

    const auto deep_popup_bounds = [&]() noexcept {
        const float rows = static_cast<float>(deep_candidates_.size());
        const float height = deep_select_header_height +
                             rows * deep_select_row_height + 2.0F;
        return gui::Rect{
            std::clamp(deep_popup_anchor_x_, frame.image.x + 4.0F,
                       std::max(frame.image.x + 4.0F,
                                frame.image.x + frame.image.width -
                                    deep_select_width - 4.0F)),
            std::clamp(deep_popup_anchor_y_, frame.image.y + 4.0F,
                       std::max(frame.image.y + 4.0F,
                                frame.image.y + frame.image.height - height -
                                    4.0F)),
            deep_select_width, height};
    };
    gui::Rect popup = deep_popup_bounds();
    if (deep_select_open_ &&
        (ctx.key_pressed(gui::Key::Escape) ||
         (ctx.pointer_pressed() &&
          !popup.contains(ctx.pointer_x(), ctx.pointer_y())))) {
        deep_select_open_ = false;
        deep_candidates_.clear();
    }
    popup = deep_popup_bounds();
    const bool popup_owns_pointer =
        deep_select_open_ &&
        popup.contains(ctx.pointer_x(), ctx.pointer_y());

    const gui::Rect interaction_bounds =
        occluded || popup_owns_pointer ? gui::Rect{} : frame.image;
    const gui::Interaction it = ctx.grab(id_, interaction_bounds);
    frame.hovered = it.hovered;
    frame.held = it.held;
    frame.pointer =
        to_canvas(frame.image, interaction_canvas, ctx.pointer_x(),
                  ctx.pointer_y());

    if (it.pressed) {
        frame.pressed = true;
        drag_axis_ = GizmoAxis::None;
        dragging_ = false;
        surface_dragging_ = false;
        deep_select_armed_ = false;
        deep_select_open_ = false;
        deep_candidates_.clear();

        if (hovered_surface_axis != GizmoAxis::None &&
            selected_surface_value != nullptr) {
            frame.surface_picked = selected_surface_value->node;
            surface_drag_start_ = *selected_surface_value;
            drag_axis_ = hovered_surface_axis;
            press_world_ = selected_surface_value->geometry.center;
            press_x_ = frame.pointer.x;
            press_y_ = frame.pointer.y;
            press_pointer_x_ = ctx.pointer_x();
            if (hovered_surface_axis != GizmoAxis::Yaw) {
                const std::size_t index = axis_index(hovered_surface_axis);
                axis_canvas_x_ = surface_gizmo.canvas_dx[index];
                axis_canvas_y_ = surface_gizmo.canvas_dy[index];
                axis_world_length_ = surface_gizmo.world_length;
            }
            surface_dragging_ = true;
        } else if (hit_surface != nullptr) {
            frame.surface_picked = hit_surface->surface->node;
            surface_drag_start_ = hit_surface->display;
            drag_axis_ = GizmoAxis::Screen;
            press_world_ = hit_surface->display.geometry.center;
            press_x_ = frame.pointer.x;
            press_y_ = frame.pointer.y;
            press_pointer_x_ = ctx.pointer_x();
            surface_dragging_ = true;
        } else if (hovered_scene_axis != GizmoAxis::None &&
                   primary_world.has_value()) {
            frame.picked = primary;
            drag_axis_ = hovered_scene_axis;
            press_world_ = scene::translation_of(*primary_world);
            const std::size_t index = axis_index(hovered_scene_axis);
            axis_canvas_x_ = scene_gizmo.canvas_dx[index];
            axis_canvas_y_ = scene_gizmo.canvas_dy[index];
            axis_world_length_ = scene_gizmo.world_length;
            press_x_ = frame.pointer.x;
            press_y_ = frame.pointer.y;
            dragging_ = true;
        } else {
            const float light_radius = 12.0F * canvas_width / frame.image.width;
            const std::vector<scene::ScenePick> hits =
                projection != nullptr && projection->project
                    ? scene::pick_scene_all(
                          scene, frame.pointer.x, frame.pointer.y,
                          projection->project, model_bounds, light_radius,
                          role_ == ViewportRole::Stage && show_overlays_)
                    : scene::pick_scene_all(
                          scene, scene.canvas, frame.pointer.x,
                          frame.pointer.y, model_bounds, light_radius,
                          role_ == ViewportRole::Stage && show_overlays_);
            if (!hits.empty()) {
                frame.picked = hits.front().node;
                deep_candidates_.reserve(hits.size());
                for (const scene::ScenePick& hit : hits) {
                    deep_candidates_.push_back(hit.node);
                }
                if (hits.size() > 1U) {
                    deep_select_armed_ = true;
                    deep_press_time_ = ctx.time_seconds();
                    deep_press_pointer_x_ = ctx.pointer_x();
                    deep_press_pointer_y_ = ctx.pointer_y();
                }
                const auto world = scene::world_for(scene, hits.front().node);
                if (world.has_value()) {
                    press_world_ = scene::translation_of(*world);
                    press_x_ = frame.pointer.x;
                    press_y_ = frame.pointer.y;
                    drag_axis_ = GizmoAxis::Screen;
                    dragging_ = true;
                }
            }
        }
    }

    if (it.held && surface_dragging_) {
        output::ProjectionSurface edited = surface_drag_start_;
        const float dx = frame.pointer.x - press_x_;
        const float dy = frame.pointer.y - press_y_;
        bool changed = false;
        if (drag_axis_ == GizmoAxis::Yaw) {
            const float yaw_delta =
                (ctx.pointer_x() - press_pointer_x_) / 360.0F;
            if (yaw_delta != 0.0F) {
                const scene::Vec3 stage_up{0.0F, 1.0F, 0.0F};
                edited.geometry.right = scene::rotate_around(
                    surface_drag_start_.geometry.right, stage_up, yaw_delta);
                edited.geometry.up = scene::rotate_around(
                    surface_drag_start_.geometry.up, stage_up, yaw_delta);
                changed = true;
            }
        } else if (dx != 0.0F || dy != 0.0F) {
            scene::Vec3 translation{};
            if (drag_axis_ == GizmoAxis::Screen) {
                translation = scene::screen_drag_to_translation(
                    scene.camera, scene.canvas, press_world_, dx, dy);
            } else {
                const float axis_pixels_squared =
                    axis_canvas_x_ * axis_canvas_x_ +
                    axis_canvas_y_ * axis_canvas_y_;
                if (axis_pixels_squared > 0.0F) {
                    const float along =
                        (dx * axis_canvas_x_ + dy * axis_canvas_y_) /
                        axis_pixels_squared;
                    translation = axis_vector(drag_axis_) *
                                  (along * axis_world_length_);
                }
            }
            edited.geometry.center =
                surface_drag_start_.geometry.center + translation;
            changed = translation != scene::Vec3{};
        }
        if (changed) {
            frame.surface_dragging = true;
            surface_preview_ = edited;
            surface_preview_deadline_ = ctx.time_seconds() + 1.0;
            frame.surface_edit = edited;
            frame.gizmo_axis = drag_axis_;
            surface_gizmo = gizmo_geometry(frame.image, scene,
                                           edited.geometry.center);
            for (ProjectedSurface& projected : projected_surfaces) {
                if (projected.surface->node != edited.node) {
                    continue;
                }
                projected.display = edited;
                switch (edited.shape) {
                case output::ProjectionSurfaceShape::Planar:
                    projected.visual = planar_surface_visual(edited);
                    break;
                case output::ProjectionSurfaceShape::HorizontalArc: {
                    auto mesh = output::horizontal_arc_mesh(
                        edited.geometry, edited.horizontal_arc_turns,
                        edited.mesh_columns);
                    projected.visual =
                        mesh ? mesh_surface_visual(mesh.value(), true)
                             : planar_surface_visual(edited, true);
                    break;
                }
                case output::ProjectionSurfaceShape::ImportedMesh: {
                    const output::ProjectionMesh* const mesh =
                        projection_mesh_for(edited);
                    projected.visual =
                        mesh != nullptr
                            ? mesh_surface_visual(*mesh, false,
                                                  &edited.geometry)
                            : planar_surface_visual(edited, true);
                    break;
                }
                }
                project_surface(projected, frame.image, scene);
                break;
            }
        }
    }

    if (it.held && deep_select_armed_) {
        const float pointer_dx = ctx.pointer_x() - deep_press_pointer_x_;
        const float pointer_dy = ctx.pointer_y() - deep_press_pointer_y_;
        const float travel_squared =
            pointer_dx * pointer_dx + pointer_dy * pointer_dy;
        const float limit_squared =
            deep_select_motion_pixels * deep_select_motion_pixels;
        if (travel_squared > limit_squared) {
            deep_select_armed_ = false;
        } else if (ctx.time_seconds() - deep_press_time_ >=
                   deep_select_hold_seconds) {
            deep_select_armed_ = false;
            deep_select_open_ = true;
            deep_popup_anchor_x_ = deep_press_pointer_x_ + 12.0F;
            deep_popup_anchor_y_ = deep_press_pointer_y_ + 12.0F;
            dragging_ = false;
            drag_axis_ = GizmoAxis::None;
            popup = deep_popup_bounds();
        }
    }

    if (it.held && dragging_) {
        const float dx = frame.pointer.x - press_x_;
        const float dy = frame.pointer.y - press_y_;
        // With several candidates, a tiny stationary movement remains part of
        // the long-press gesture. Once travel crosses the threshold the exact
        // same press becomes a normal drag from its original world point.
        if (!deep_select_armed_ && (dx != 0.0F || dy != 0.0F)) {
            frame.dragging = true;
            frame.gizmo_axis = drag_axis_;
            if (drag_axis_ == GizmoAxis::Screen) {
                frame.translation =
                    projection != nullptr && projection->project
                        ? scene::screen_drag_to_translation(
                              press_world_, projection->forward,
                              projection->up, projection->project, dx, dy)
                        : scene::screen_drag_to_translation(
                              scene.camera, scene.canvas, press_world_, dx,
                              dy);
            } else {
                const float axis_pixels_squared =
                    axis_canvas_x_ * axis_canvas_x_ +
                    axis_canvas_y_ * axis_canvas_y_;
                if (axis_pixels_squared > 0.0F) {
                    const float along =
                        (dx * axis_canvas_x_ + dy * axis_canvas_y_) /
                        axis_pixels_squared;
                    frame.translation =
                        axis_vector(drag_axis_) * (along * axis_world_length_);
                }
            }
        }
    }

    if (it.released) {
        frame.released = true;
        deep_select_armed_ = false;
        dragging_ = false;
        surface_dragging_ = false;
        drag_axis_ = GizmoAxis::None;
    }

    // The editor chrome is a clipped 2D pass over the zero-copy scene image.
    // Program stays clean except for selection outlines; Stage adds the spatial
    // guides an operator needs to author the scene.
    ctx.push_clip(frame.image);
    gui::UiDrawList& list = ctx.draw_list();
    if (role_ == ViewportRole::Stage && show_grid_) {
        draw_grid(list, frame.image, scene);
    }
    if (role_ == ViewportRole::Stage && show_overlays_) {
        for (const ProjectedSurface& projected : projected_surfaces) {
            const bool is_selected =
                projected.surface->node == selected_surface_node;
            draw_surface(list, projected,
                         is_selected);
            const scene::CanvasProjection centre = scene::project_to_canvas(
                scene.camera, scene.canvas, projected.display.geometry.center);
            if (centre.visible) {
                const gui::UiPoint label_at =
                    to_ui(frame.image, scene.canvas, centre);
                const std::string fallback_name =
                    "SURFACE " +
                    std::to_string(projected.surface->node.value());
                ctx.label(gui::Rect{label_at.x + 8.0F, label_at.y - 18.0F,
                                    190.0F, 18.0F},
                          projected.display.name.empty()
                              ? std::string_view{fallback_name}
                              : std::string_view{projected.display.name},
                          is_selected ? surface_selected : surface_color);
                if (projected.visual.fallback && is_selected) {
                    ctx.label(gui::Rect{label_at.x + 8.0F, label_at.y + 8.0F,
                                        190.0F, 18.0F},
                              "CALIBRATION BOUNDS (MESH UNAVAILABLE)",
                              surface_fallback);
                }
            }
        }
        if (camera_overlay != nullptr) {
            draw_camera_overlay(list, frame.image, scene, *camera_overlay);
        }
        draw_light_overlays(list, frame.image, scene, selection, primary);
    }
    draw_selection_outlines(list, frame.image, scene, selection, primary,
                            model_bounds, projection);
    if (role_ == ViewportRole::Stage && show_gizmo_) {
        if (selected_surface_value != nullptr) {
            const GizmoAxis translation_hover =
                hovered_surface_axis == GizmoAxis::Yaw
                    ? GizmoAxis::None
                    : hovered_surface_axis;
            const GizmoAxis translation_active =
                surface_dragging_ && drag_axis_ != GizmoAxis::Yaw
                    ? drag_axis_
                    : GizmoAxis::None;
            draw_gizmo(list, surface_gizmo, translation_hover,
                       translation_active);
            draw_yaw_ring(list, surface_gizmo, hovered_surface_yaw,
                          surface_dragging_ && drag_axis_ == GizmoAxis::Yaw);
            if (surface_gizmo.visible) {
                ctx.label(gui::Rect{surface_gizmo.origin.x + 54.0F,
                                    surface_gizmo.origin.y - 10.0F, 34.0F,
                                    18.0F},
                          "YAW", surface_yaw);
            }
        } else {
            draw_gizmo(list, scene_gizmo, hovered_scene_axis, drag_axis_);
        }
    }
    ctx.pop_clip();

    if (deep_select_open_) {
        popup = deep_popup_bounds();
        ctx.push_clip(frame.image);
        ctx.panel(popup, gui::UiColor{0.035F, 0.043F, 0.055F, 0.98F});
        ctx.panel(gui::Rect{popup.x, popup.y, popup.width,
                            deep_select_header_height},
                  ctx.theme().header);
        ctx.panel(gui::Rect{popup.x, popup.y + deep_select_header_height - 1.0F,
                            popup.width, 1.0F},
                  ctx.theme().accent);
        ctx.label(gui::Rect{popup.x + 8.0F, popup.y, popup.width - 16.0F,
                            deep_select_header_height},
                  "SELECT UNDER CURSOR", ctx.theme().text_muted);
        for (std::size_t index = 0U; index < deep_candidates_.size(); ++index) {
            const gui::Rect row{
                popup.x + 1.0F,
                popup.y + deep_select_header_height +
                    static_cast<float>(index) * deep_select_row_height,
                popup.width - 2.0F, deep_select_row_height};
            const gui::Interaction row_it =
                ctx.grab(mix(id_, 0xD330U + static_cast<core::u64>(index)), row);
            ctx.panel(row,
                      row_it.held   ? ctx.theme().widget_active
                      : row_it.hovered ? ctx.theme().widget_hover
                                       : (index % 2U == 0U ? ctx.theme().row
                                                          : ctx.theme().row_alt));
            const doc::NodeId candidate = deep_candidates_[index];
            ctx.label(gui::Rect{row.x + 9.0F, row.y, row.width - 18.0F,
                                row.height},
                      deep_select_label(scene, candidate), ctx.theme().text);
            if (row_it.clicked) {
                frame.pressed = true;
                frame.picked = candidate;
                deep_select_open_ = false;
                deep_candidates_.clear();
                break;
            }
        }
        ctx.pop_clip();
    }
    frame.deep_select_open = deep_select_open_;
    frame.deep_candidate_count = deep_candidates_.size();
    return frame;
}

} // namespace mediaserver::editor
