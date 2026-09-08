#include <mediaserver/editor/viewport_panel.hpp>

#include <mediaserver/scene/manipulate.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

namespace mediaserver::editor {
namespace {

constexpr float stage_toolbar_height = 29.0F;
constexpr float gizmo_length_pixels = 72.0F;
constexpr float gizmo_hit_pixels = 9.0F;
constexpr float surface_hit_pixels = 8.0F;
constexpr float surface_yaw_radius_pixels = 49.0F;
constexpr float surface_yaw_hit_pixels = 7.0F;
constexpr std::size_t maximum_imported_wire_triangles = 4096U;
constexpr std::size_t maximum_cached_model_bounds = 128U;
constexpr std::size_t maximum_cached_projection_surfaces = 256U;
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
// Track screens, in the hue the timeline and Inspector mark a track with.
constexpr gui::UiColor screen_color{0.906F, 0.596F, 0.400F, 0.55F};
constexpr gui::UiColor screen_selected{0.980F, 0.706F, 0.470F, 1.0F};
constexpr gui::UiColor screen_muted{0.549F, 0.529F, 0.510F, 0.45F};
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

// The physical camera on the volume, drawn where the tracker says it is.
//
// Deliberately distinct from the authored camera overlay above: this is the
// camera that is actually in the room, and an operator needs to be able to
// tell at a glance whether it is where they think it is. It uses the tracked
// pose's own sensor aspect rather than the canvas aspect, because the shape of
// this frustum is the shape of the camera, not of the wall.
//
// A held pose is drawn dimmer rather than hidden. It is still where the wall
// believes the camera is - every node holds the same one - and hiding it would
// suggest tracking had stopped affecting the picture, which is not true.
void draw_tracked_camera_overlay(gui::UiDrawList& list,
                                 const gui::Rect& image,
                                 const scene::Scene& view,
                                 const scene::TrackedCameraView& camera) {
    if (!camera.pose.valid) {
        return;
    }
    const scene::Camera tracked = scene::tracked_view_camera(
        camera.authored, camera.pose, camera.encoders);
    if (!scene::valid(tracked)) {
        return;
    }
    const scene::Vec3 forward =
        scene::normalize(tracked.target - tracked.eye);
    const scene::Vec3 right = scene::normalize(
        scene::cross(forward, tracked.up), scene::Vec3{1.0F, 0.0F, 0.0F});
    const scene::Vec3 up = scene::cross(right, forward);

    // Long enough to read as a direction, short enough not to fill the view.
    constexpr float distance = 3.0F;
    const float half_height =
        distance * scene::deterministic::tan_half_turns(tracked.field_of_view);
    const float aspect =
        camera.pose.aspect_ratio > 0.0F ? camera.pose.aspect_ratio : 1.0F;
    const float half_width = half_height * aspect;
    const scene::Vec3 centre = tracked.eye + forward * distance;
    const std::array<scene::Vec3, 4U> corners{
        centre - right * half_width - up * half_height,
        centre + right * half_width - up * half_height,
        centre + right * half_width + up * half_height,
        centre - right * half_width + up * half_height};

    const gui::UiColor live{0.29F, 0.85F, 0.55F, 0.85F};
    const gui::UiColor held{0.29F, 0.85F, 0.55F, 0.38F};
    const gui::UiColor color = camera.pose.held ? held : live;
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        projected_line(list, image, view, tracked.eye, corners[index], 1.4F,
                       color);
        projected_line(list, image, view, corners[index],
                       corners[(index + 1U) % corners.size()], 1.4F, color);
    }
    // A bar across the top of the frustum, so which way is up is unambiguous
    // when the camera rolls.
    projected_line(list, image, view, corners[3U], corners[2U], 2.6F, color);
    draw_light_icon(list, image, view, tracked.eye, color, false);
}

// The plate: the region of the canvas the physical camera is looking at.
//
// Drawn in canvas coordinates over the rendered image, not in 3D, because it
// *is* a region of the image - the boundary between the tracked camera's view
// and the authored one. Getting to look at it is how an operator confirms the
// margin is wide enough and the camera is pointing where they think.
//
// The inner band is the feather, where the two renders cross-dissolve. Showing
// it separately matters: an operator judging whether the transition will be
// visible on camera needs to see how much of the plate edge is a ramp.
void draw_inner_frustum_overlay(gui::UiDrawList& list, const gui::Rect& image,
                                const scene::Scene& view,
                                const ViewportPlate& entry) {
    const scene::CanvasRect& plate = entry.rect;
    if (view.canvas.width == 0U || view.canvas.height == 0U ||
        plate.width == 0U || plate.height == 0U) {
        return;
    }
    const auto to_image_x = [&](float canvas_x) {
        return image.x + canvas_x / static_cast<float>(view.canvas.width) *
                             image.width;
    };
    const auto to_image_y = [&](float canvas_y) {
        return image.y + canvas_y / static_cast<float>(view.canvas.height) *
                             image.height;
    };
    const auto outline = [&](float left, float top, float right, float bottom,
                             float thickness, gui::UiColor color) {
        const float width = std::max(right - left, 0.0F);
        const float height = std::max(bottom - top, 0.0F);
        list.add_rect(left, top, width, thickness, color);
        list.add_rect(left, bottom - thickness, width, thickness, color);
        list.add_rect(left, top, thickness, height, color);
        list.add_rect(right - thickness, top, thickness, height, color);
    };

    const float left = to_image_x(static_cast<float>(plate.x));
    const float top = to_image_y(static_cast<float>(plate.y));
    const float right = to_image_x(static_cast<float>(plate.x + plate.width));
    const float bottom = to_image_y(static_cast<float>(plate.y + plate.height));

    const gui::UiColor plate_color{0.29F, 0.85F, 0.55F, 0.90F};
    const gui::UiColor feather_color{0.29F, 0.85F, 0.55F, 0.35F};
    outline(left, top, right, bottom, 2.0F, plate_color);

    // The feather sits inside the rectangle, so its inset is the authored
    // width scaled into image pixels the same way the edges were. The width is
    // this camera's own, not the show's: two cameras can soften differently.
    const core::u32 feather_px = std::min(
        entry.soft_edge_px, std::min(plate.width, plate.height) / 2U);
    if (feather_px == 0U) {
        return;
    }
    const float inset_x =
        static_cast<float>(feather_px) /
        static_cast<float>(view.canvas.width) * image.width;
    const float inset_y =
        static_cast<float>(feather_px) /
        static_cast<float>(view.canvas.height) * image.height;
    if (right - left <= inset_x * 2.0F || bottom - top <= inset_y * 2.0F) {
        return;
    }
    outline(left + inset_x, top + inset_y, right - inset_x, bottom - inset_y,
            1.0F, feather_color);
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
    std::array<scene::Vec3, 3U> world_axes{};
    float world_length{};
};

[[nodiscard]] GizmoGeometry gizmo_geometry(const gui::Rect& image,
                                           const scene::Scene& view,
                                           const scene::Mat4& world) noexcept {
    GizmoGeometry result{};
    if (view.canvas.height == 0U || image.height <= 0.0F) {
        return result;
    }
    const scene::Vec3 forward =
        scene::normalize(view.camera.target - view.camera.eye);
    const scene::Vec3 origin_world = scene::translation_of(world);
    const float depth = scene::dot(origin_world - view.camera.eye, forward);
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
        scene::project_to_canvas(view.camera, view.canvas, origin_world);
    if (!origin.visible) {
        return result;
    }
    result.origin = to_ui(image, view.canvas, origin);
    static constexpr std::array<scene::Vec3, 3U> fallback_axes{
        scene::Vec3{1.0F, 0.0F, 0.0F}, scene::Vec3{0.0F, 1.0F, 0.0F},
        scene::Vec3{0.0F, 0.0F, 1.0F}};
    for (std::size_t index = 0U; index < result.world_axes.size(); ++index) {
        result.world_axes[index] = scene::normalize(
            scene::Vec3{world.at(0U, static_cast<core::u32>(index)),
                        world.at(1U, static_cast<core::u32>(index)),
                        world.at(2U, static_cast<core::u32>(index))},
            fallback_axes[index]);
        const auto endpoint =
            scene::project_to_canvas(view.camera, view.canvas,
                                     origin_world + result.world_axes[index] *
                                                        result.world_length);
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

[[nodiscard]] GizmoGeometry gizmo_geometry(const gui::Rect& image,
                                           const scene::Scene& view,
                                           scene::Vec3 world) noexcept {
    return gizmo_geometry(image, view, scene::translation(world));
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
                        nullptr,
                    float imported_scale = 1.0F) {
    SurfaceVisual visual{};
    visual.vertices.reserve(mesh.vertices.size());
    const output::ProjectionMeshPlacement imported_placement{
        imported_calibration != nullptr ? *imported_calibration
                                        : output::PhysicalSurface{},
        imported_scale};
    for (const output::ProjectionMeshVertex& vertex : mesh.vertices) {
        if (imported_calibration == nullptr) {
            visual.vertices.push_back(vertex.position);
        } else {
            visual.vertices.push_back(output::place_projection_mesh_position(
                vertex.position, imported_placement));
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

// A track's screen, outlined where its plane sits.
//
// Without this a track with nothing on it is an invisible plane: the operator
// makes one, the Inspector offers a transform, and the viewport shows nothing
// to aim it at. The clips playing on it draw the picture; this draws the edge
// of the thing they are playing on, which is the part that stays true when
// the track is empty, muted, or between clips.
void draw_screen(gui::UiDrawList& list, const gui::Rect& image,
                 const scene::Scene& view, const scene::SceneScreen& screen,
                 bool selected_screen) {
    // The quad primitive spans one unit centred on its origin, so these are
    // the plane's own corners before its transform.
    constexpr std::array<scene::Vec3, 4U> corners{{
        {-0.5F, 0.5F, 0.0F},
        {0.5F, 0.5F, 0.0F},
        {0.5F, -0.5F, 0.0F},
        {-0.5F, -0.5F, 0.0F}}};
    std::array<gui::UiPoint, 4U> points{};
    std::array<bool, 4U> visible{};
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        const scene::CanvasProjection projected = scene::project_to_canvas(
            view.camera, view.canvas,
            scene::transform_point(screen.world, corners[index]));
        visible[index] = projected.visible;
        if (projected.visible) {
            points[index] = to_ui(image, view.canvas, projected);
        }
    }
    const gui::UiColor color = screen.muted
        ? screen_muted
        : (selected_screen ? screen_selected : screen_color);
    for (std::size_t index = 0U; index < points.size(); ++index) {
        const std::size_t next = (index + 1U) % points.size();
        if (!visible[index] || !visible[next]) {
            continue;
        }
        line(list, points[index], points[next],
             selected_screen ? 4.0F : 3.0F, outline_shadow);
        line(list, points[index], points[next],
             selected_screen ? 2.0F : 1.0F, color);
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
    const std::string& source = model.source;
    if (source.empty()) {
        return std::nullopt;
    }
    auto held = model_bounds_cache_.find(source);
    const render::ModelCacheObservation observation =
        model_cache_.observe(source, model_publication_fence_);
    if (observation.ready()) {
        const scene::ModelLocalBounds bounds{observation.asset->bounds.minimum,
                                             observation.asset->bounds.maximum};
        const core::u64 stamp = next_model_bounds_stamp();
        if (held == model_bounds_cache_.end() &&
            model_bounds_cache_.size() >= maximum_cached_model_bounds) {
            held = std::min_element(
                model_bounds_cache_.begin(), model_bounds_cache_.end(),
                [](const auto& left, const auto& right) noexcept {
                    return left.second.stamp < right.second.stamp;
                });
            if (held != model_bounds_cache_.end()) {
                model_bounds_cache_.erase(held);
            }
            held = model_bounds_cache_.end();
        }
        if (held == model_bounds_cache_.end()) {
            held = model_bounds_cache_
                       .emplace(source,
                                CachedModelBounds{bounds, stamp})
                       .first;
        } else {
            held->second = CachedModelBounds{bounds, stamp};
        }
        return held->second.bounds;
    }

    // Busy/Pending/Failed never borrows another source's bounds. A previous
    // exact-source success is safe to keep through an explicit refresh.
    if (held != model_bounds_cache_.end()) {
        held->second.stamp = next_model_bounds_stamp();
        return held->second.bounds;
    }
    return std::nullopt;
}

core::u64 ViewportPanel::next_model_bounds_stamp() noexcept {
    ++model_bounds_stamp_;
    if (model_bounds_stamp_ != 0U) {
        return model_bounds_stamp_;
    }
    core::u64 largest{};
    for (auto& [unused, cached] : model_bounds_cache_) {
        static_cast<void>(unused);
        cached.stamp = cached.stamp / 2U + 1U;
        largest = std::max(largest, cached.stamp);
    }
    model_bounds_stamp_ = largest + 1U;
    return model_bounds_stamp_;
}

core::u64 ViewportPanel::next_projection_mesh_stamp() noexcept {
    ++projection_mesh_stamp_;
    if (projection_mesh_stamp_ != 0U) {
        return projection_mesh_stamp_;
    }
    core::u64 largest{};
    for (auto& [unused, cached] : projection_mesh_cache_) {
        static_cast<void>(unused);
        cached.stamp = cached.stamp / 2U + 1U;
        largest = std::max(largest, cached.stamp);
    }
    projection_mesh_stamp_ = largest + 1U;
    return projection_mesh_stamp_;
}

std::shared_ptr<const output::ProjectionMesh>
ViewportPanel::projection_mesh_for(
    const output::ProjectionSurface& surface) {
    const core::u64 key = surface.node.value();
    auto held = projection_mesh_cache_.find(key);
    const core::u64 stamp = next_projection_mesh_stamp();
    if (held == projection_mesh_cache_.end()) {
        if (projection_mesh_cache_.size() >=
            maximum_cached_projection_surfaces) {
            const auto oldest = std::min_element(
                projection_mesh_cache_.begin(), projection_mesh_cache_.end(),
                [](const auto& left, const auto& right) noexcept {
                    return left.second.stamp < right.second.stamp;
                });
            if (oldest != projection_mesh_cache_.end()) {
                projection_mesh_cache_.erase(oldest);
            }
        }
        held = projection_mesh_cache_
                   .try_emplace(key, CachedProjectionState{})
                   .first;
    }
    held->second.stamp = stamp;

    output::ResolvedProjectionSurface resolved{
        surface.geometry, surface.shape, surface.horizontal_arc_turns,
        surface.mesh_columns, surface.mesh_source, surface.mesh_scale,
        surface.mesh_asset};
    output::OutputMap map{};
    map.node = surface.node;
    map.source = scene::whole(surface.geometry.raster);
    map.projection = output::FeedProjection::PlanarSurface;
    map.surface = surface.node;
    map.resolved_surface = std::move(resolved);
    return held->second.cache.resolve_mesh(map);
}

void ViewportPanel::reset_project_state() noexcept {
    dragging_ = false;
    surface_dragging_ = false;
    drag_axis_ = GizmoAxis::None;
    surface_preview_.reset();
    deep_select_armed_ = false;
    deep_select_open_ = false;
    deep_candidates_.clear();
    model_publication_fence_ = 0U;
    model_bounds_stamp_ = 0U;
    model_bounds_cache_.clear();
    projection_mesh_stamp_ = 0U;
    projection_mesh_cache_.clear();
    mask_gesture_active_ = false;
    mask_gesture_cancelled_ = false;
    mask_gesture_hit_ = MaskViewportHit::None;
    mask_gesture_point_.reset();
}

bool ViewportPanel::refresh_projection_surface(doc::NodeId surface) noexcept {
    const auto found = projection_mesh_cache_.find(surface.value());
    return found != projection_mesh_cache_.end() &&
           found->second.cache.refresh_imported_mesh();
}

bool detail::wait_for_viewport_projection_surface_for_test(
    ViewportPanel& panel, doc::NodeId surface) noexcept {
    const auto found = panel.projection_mesh_cache_.find(surface.value());
    return found != panel.projection_mesh_cache_.end() &&
           render::detail::wait_for_output_projection_cache_for_test(
               found->second.cache);
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

std::optional<gui::UiColor> sample_viewport_color(
    rhi::Device& device, const rhi::Texture& target,
    const gui::Rect& image, float pointer_x, float pointer_y) noexcept {
    if (!target || target.desc().format != rhi::TextureFormat::Rgba8Unorm ||
        target.width() == 0U || target.height() == 0U ||
        image.width <= 0.0F || image.height <= 0.0F ||
        !image.contains(pointer_x, pointer_y)) {
        return std::nullopt;
    }
    const std::uint64_t byte_count =
        static_cast<std::uint64_t>(target.width()) * target.height() * 4U;
    if (byte_count == 0U ||
        byte_count > std::numeric_limits<core::u32>::max()) {
        return std::nullopt;
    }
    auto readback =
        device.create_readback(static_cast<core::u32>(byte_count));
    if (!readback) {
        return std::nullopt;
    }
    auto acquired = device.acquire_commands();
    if (!acquired) {
        return std::nullopt;
    }
    auto commands = std::move(acquired).value();
    if (!commands.download(target, readback.value())) {
        return std::nullopt;
    }
    auto submitted = device.submit(std::move(commands));
    if (!submitted || !std::move(submitted).value().wait()) {
        return std::nullopt;
    }
    const auto pixels = readback.value().map();
    if (pixels.size() < byte_count) {
        return std::nullopt;
    }

    const float normalized_x =
        std::clamp((pointer_x - image.x) / image.width, 0.0F, 1.0F);
    const float normalized_y =
        std::clamp((pointer_y - image.y) / image.height, 0.0F, 1.0F);
    const core::u32 x = std::min(
        static_cast<core::u32>(normalized_x * static_cast<float>(target.width())),
        target.width() - 1U);
    const core::u32 y = std::min(
        static_cast<core::u32>(normalized_y * static_cast<float>(target.height())),
        target.height() - 1U);
    const std::size_t offset =
        (static_cast<std::size_t>(y) * target.width() + x) * 4U;
    constexpr float to_unit = 1.0F / 255.0F;
    const gui::UiColor color{
        static_cast<float>(pixels[offset]) * to_unit,
        static_cast<float>(pixels[offset + 1U]) * to_unit,
        static_cast<float>(pixels[offset + 2U]) * to_unit,
        static_cast<float>(pixels[offset + 3U]) * to_unit};
    readback.value().unmap();
    return color;
}

std::optional<gui::UiPoint> mask_normalized_to_viewport(
    const gui::Rect& image, scene::Canvas canvas,
    scene::MaskCoordinateSpace coordinate_space,
    const MaskViewportSourceMapping* source_mapping, float x,
    float y) noexcept {
    if (image.width <= 0.0F || image.height <= 0.0F ||
        canvas.width == 0U || canvas.height == 0U || !std::isfinite(x) ||
        !std::isfinite(y)) {
        return std::nullopt;
    }
    float canvas_x{};
    float canvas_y{};
    if (coordinate_space == scene::MaskCoordinateSpace::AuthoredCanvas) {
        canvas_x = x * static_cast<float>(canvas.width);
        canvas_y = y * static_cast<float>(canvas.height);
    } else {
        if (source_mapping == nullptr) {
            return std::nullopt;
        }
        for (const MaskViewportCanvasPoint& corner : source_mapping->corners) {
            if (!std::isfinite(corner.x) || !std::isfinite(corner.y)) {
                return std::nullopt;
            }
        }
        const auto& corners = source_mapping->corners;
        const float area_twice =
            corners[0U].x * corners[1U].y -
                corners[1U].x * corners[0U].y +
            corners[1U].x * corners[2U].y -
                corners[2U].x * corners[1U].y +
            corners[2U].x * corners[3U].y -
                corners[3U].x * corners[2U].y +
            corners[3U].x * corners[0U].y -
                corners[0U].x * corners[3U].y;
        if (!std::isfinite(area_twice) || std::abs(area_twice) < 1.0e-4F) {
            return std::nullopt;
        }
        const float one_minus_x = 1.0F - x;
        const float one_minus_y = 1.0F - y;
        const std::array weights{
            one_minus_x * one_minus_y, x * one_minus_y, x * y,
            one_minus_x * y};
        for (std::size_t index = 0U; index < corners.size(); ++index) {
            canvas_x += corners[index].x * weights[index];
            canvas_y += corners[index].y * weights[index];
        }
    }
    return gui::UiPoint{
        image.x + canvas_x * image.width / static_cast<float>(canvas.width),
        image.y + canvas_y * image.height /
                      static_cast<float>(canvas.height)};
}

std::optional<MaskViewportNormalizedPoint> mask_viewport_to_normalized(
    const gui::Rect& image, scene::Canvas canvas,
    scene::MaskCoordinateSpace coordinate_space,
    const MaskViewportSourceMapping* source_mapping, float pointer_x,
    float pointer_y) noexcept {
    if (image.width <= 0.0F || image.height <= 0.0F ||
        canvas.width == 0U || canvas.height == 0U ||
        !std::isfinite(pointer_x) || !std::isfinite(pointer_y)) {
        return std::nullopt;
    }
    const float canvas_x =
        (pointer_x - image.x) * static_cast<float>(canvas.width) / image.width;
    const float canvas_y =
        (pointer_y - image.y) * static_cast<float>(canvas.height) /
        image.height;
    if (coordinate_space == scene::MaskCoordinateSpace::AuthoredCanvas) {
        return MaskViewportNormalizedPoint{
            canvas_x / static_cast<float>(canvas.width),
            canvas_y / static_cast<float>(canvas.height)};
    }
    if (source_mapping == nullptr ||
        !mask_normalized_to_viewport(image, canvas, coordinate_space,
                                     source_mapping, 0.5F, 0.5F)
             .has_value()) {
        return std::nullopt;
    }

    const auto& q = source_mapping->corners;
    const float ax = q[0U].x;
    const float ay = q[0U].y;
    const float bx = q[1U].x - q[0U].x;
    const float by = q[1U].y - q[0U].y;
    const float cx = q[3U].x - q[0U].x;
    const float cy = q[3U].y - q[0U].y;
    const float dx = q[0U].x - q[1U].x + q[2U].x - q[3U].x;
    const float dy = q[0U].y - q[1U].y + q[2U].y - q[3U].y;
    float u = 0.5F;
    float v = 0.5F;
    const float affine_determinant = bx * cy - by * cx;
    if (std::abs(affine_determinant) > 1.0e-8F) {
        const float px = canvas_x - ax;
        const float py = canvas_y - ay;
        u = (px * cy - py * cx) / affine_determinant;
        v = (bx * py - by * px) / affine_determinant;
    }
    for (std::size_t iteration = 0U; iteration < 12U; ++iteration) {
        const float projected_x = ax + bx * u + cx * v + dx * u * v;
        const float projected_y = ay + by * u + cy * v + dy * u * v;
        const float error_x = projected_x - canvas_x;
        const float error_y = projected_y - canvas_y;
        const float j00 = bx + dx * v;
        const float j01 = cx + dx * u;
        const float j10 = by + dy * v;
        const float j11 = cy + dy * u;
        const float determinant = j00 * j11 - j01 * j10;
        if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-8F) {
            return std::nullopt;
        }
        const float delta_u = (error_x * j11 - error_y * j01) / determinant;
        const float delta_v = (j00 * error_y - j10 * error_x) / determinant;
        u -= delta_u;
        v -= delta_v;
        if (std::abs(delta_u) + std::abs(delta_v) < 1.0e-6F) {
            break;
        }
    }
    if (!std::isfinite(u) || !std::isfinite(v)) {
        return std::nullopt;
    }
    const float check_x = ax + bx * u + cx * v + dx * u * v;
    const float check_y = ay + by * u + cy * v + dy * u * v;
    const float error_x = check_x - canvas_x;
    const float error_y = check_y - canvas_y;
    if (error_x * error_x + error_y * error_y > 0.01F) {
        return std::nullopt;
    }
    return MaskViewportNormalizedPoint{u, v};
}

namespace {

constexpr float mask_point_hit_pixels = 8.0F;
constexpr float mask_handle_hit_pixels = 7.0F;
constexpr float mask_shape_handle_hit_pixels = 10.0F;
constexpr float mask_path_hit_pixels = 7.0F;
constexpr float mask_minimum_drag = 1.0e-5F;
constexpr std::size_t mask_curve_steps = 16U;
constexpr std::size_t mask_ellipse_steps = 64U;
constexpr gui::UiColor mask_outline{0.20F, 0.92F, 1.0F, 1.0F};
constexpr gui::UiColor mask_secondary{0.24F, 0.70F, 0.88F, 0.78F};
constexpr gui::UiColor mask_handle{0.96F, 0.82F, 0.28F, 1.0F};
constexpr gui::UiColor mask_feather{0.26F, 0.68F, 1.0F, 0.18F};
constexpr gui::UiColor mask_expansion{0.96F, 0.56F, 0.20F, 0.26F};

struct MaskPathVertex final {
    MaskViewportNormalizedPoint normalized{};
    gui::UiPoint screen{};
    std::size_t insertion_index{};
};

struct MaskPath final {
    std::vector<MaskPathVertex> vertices;
};

struct MaskPathHit final {
    float distance{std::numeric_limits<float>::infinity()};
    MaskViewportNormalizedPoint normalized{};
    std::size_t insertion_index{};
};

struct MaskPick final {
    MaskViewportHit hit{MaskViewportHit::None};
    const scene::MaskInstance* mask{};
    std::optional<doc::NodeId> point;
    std::size_t insertion_index{};
    scene::MaskPointGeometry insertion_geometry{};
};

[[nodiscard]] MaskViewportNormalizedPoint mask_coordinate_extent(
    scene::Canvas canvas, const MaskViewportInput& input) noexcept {
    if (input.target.coordinate_space ==
            scene::MaskCoordinateSpace::AuthoredCanvas ||
        !input.source_layer_mapping.has_value()) {
        return {static_cast<float>(canvas.width),
                static_cast<float>(canvas.height)};
    }
    const auto& corners = input.source_layer_mapping->corners;
    const auto distance = [](const MaskViewportCanvasPoint& a,
                             const MaskViewportCanvasPoint& b) noexcept {
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        return std::sqrt(dx * dx + dy * dy);
    };
    return {
        std::max((distance(corners[0U], corners[1U]) +
                  distance(corners[3U], corners[2U])) * 0.5F, 1.0F),
        std::max((distance(corners[0U], corners[3U]) +
                  distance(corners[1U], corners[2U])) * 0.5F, 1.0F)};
}

[[nodiscard]] MaskViewportNormalizedPoint rotate_shape_point(
    MaskViewportNormalizedPoint point, const scene::MaskBounds& bounds,
    float turns, MaskViewportNormalizedPoint extent) noexcept {
    constexpr float tau = 6.28318530717958647692F;
    const float centre_x = (bounds.left + bounds.right) * 0.5F;
    const float centre_y = (bounds.top + bounds.bottom) * 0.5F;
    const float dx = (point.x - centre_x) * extent.x;
    const float dy = (point.y - centre_y) * extent.y;
    const float angle = turns * tau;
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    return {centre_x + (cosine * dx - sine * dy) / extent.x,
            centre_y + (sine * dx + cosine * dy) / extent.y};
}

[[nodiscard]] MaskViewportNormalizedPoint unrotate_shape_point(
    MaskViewportNormalizedPoint point, const scene::MaskBounds& bounds,
    float turns, MaskViewportNormalizedPoint extent) noexcept {
    return rotate_shape_point(point, bounds, -turns, extent);
}

[[nodiscard]] const MaskViewportSourceMapping* source_mapping(
    const MaskViewportInput& input) noexcept {
    return input.source_layer_mapping.has_value()
        ? &*input.source_layer_mapping
        : nullptr;
}

[[nodiscard]] std::optional<gui::UiPoint> mask_screen_point(
    const gui::Rect& image, scene::Canvas canvas,
    const MaskViewportInput& input,
    MaskViewportNormalizedPoint point) noexcept {
    return mask_normalized_to_viewport(
        image, canvas, input.target.coordinate_space, source_mapping(input),
        point.x, point.y);
}

[[nodiscard]] std::optional<MaskViewportNormalizedPoint> mask_normalized_point(
    const gui::Rect& image, scene::Canvas canvas,
    const MaskViewportInput& input, float x, float y) noexcept {
    return mask_viewport_to_normalized(
        image, canvas, input.target.coordinate_space, source_mapping(input), x,
        y);
}

[[nodiscard]] MaskViewportNormalizedPoint cubic(
    MaskViewportNormalizedPoint p0, MaskViewportNormalizedPoint p1,
    MaskViewportNormalizedPoint p2, MaskViewportNormalizedPoint p3,
    float t) noexcept {
    const float one_minus = 1.0F - t;
    const float a = one_minus * one_minus * one_minus;
    const float b = 3.0F * one_minus * one_minus * t;
    const float c = 3.0F * one_minus * t * t;
    const float d = t * t * t;
    return MaskViewportNormalizedPoint{
        a * p0.x + b * p1.x + c * p2.x + d * p3.x,
        a * p0.y + b * p1.y + c * p2.y + d * p3.y};
}

[[nodiscard]] MaskPath build_mask_path(
    const scene::MaskInstance& mask, const gui::Rect& image,
    scene::Canvas canvas, const MaskViewportInput& input) {
    MaskPath path{};
    const MaskViewportNormalizedPoint coordinate_extent =
        mask_coordinate_extent(canvas, input);
    const auto append = [&](MaskViewportNormalizedPoint normalized,
                            std::size_t insertion_index) {
        if (mask.kind != scene::MaskKind::Bezier &&
            mask.rotation_turns != 0.0F) {
            normalized = rotate_shape_point(
                normalized, mask.bounds, mask.rotation_turns,
                coordinate_extent);
        }
        const auto screen = mask_screen_point(image, canvas, input, normalized);
        if (screen.has_value()) {
            path.vertices.push_back(
                MaskPathVertex{normalized, *screen, insertion_index});
        }
    };
    switch (mask.kind) {
    case scene::MaskKind::Rectangle:
        append({mask.bounds.left, mask.bounds.top}, 0U);
        append({mask.bounds.right, mask.bounds.top}, 0U);
        append({mask.bounds.right, mask.bounds.bottom}, 0U);
        append({mask.bounds.left, mask.bounds.bottom}, 0U);
        append({mask.bounds.left, mask.bounds.top}, 0U);
        break;
    case scene::MaskKind::Ellipse: {
        constexpr float tau = 6.28318530717958647692F;
        const float centre_x = (mask.bounds.left + mask.bounds.right) * 0.5F;
        const float centre_y = (mask.bounds.top + mask.bounds.bottom) * 0.5F;
        const float radius_x = (mask.bounds.right - mask.bounds.left) * 0.5F;
        const float radius_y = (mask.bounds.bottom - mask.bounds.top) * 0.5F;
        for (std::size_t index = 0U; index <= mask_ellipse_steps; ++index) {
            const float angle = tau * static_cast<float>(index) /
                                static_cast<float>(mask_ellipse_steps);
            append({centre_x + std::cos(angle) * radius_x,
                    centre_y + std::sin(angle) * radius_y},
                   0U);
        }
        break;
    }
    case scene::MaskKind::Bezier:
        if (mask.points.size() < 2U) {
            break;
        }
        for (std::size_t segment = 0U; segment < mask.points.size();
             ++segment) {
            const scene::MaskPoint& first = mask.points[segment];
            const scene::MaskPoint& second =
                mask.points[(segment + 1U) % mask.points.size()];
            const MaskViewportNormalizedPoint p0{first.x, first.y};
            const MaskViewportNormalizedPoint p1{first.out_x, first.out_y};
            const MaskViewportNormalizedPoint p2{second.in_x, second.in_y};
            const MaskViewportNormalizedPoint p3{second.x, second.y};
            const std::size_t begin = segment == 0U ? 0U : 1U;
            for (std::size_t step = begin; step <= mask_curve_steps; ++step) {
                append(cubic(p0, p1, p2, p3,
                             static_cast<float>(step) /
                                 static_cast<float>(mask_curve_steps)),
                       segment + 1U);
            }
        }
        break;
    }
    return path;
}

void draw_mask_path(gui::UiDrawList& list, const MaskPath& path,
                    float thickness, gui::UiColor color) {
    if (path.vertices.size() < 2U) {
        return;
    }
    for (std::size_t index = 1U; index < path.vertices.size(); ++index) {
        line(list, path.vertices[index - 1U].screen,
             path.vertices[index].screen, thickness, color);
    }
}

[[nodiscard]] float point_distance(gui::UiPoint a, float x,
                                   float y) noexcept {
    const float dx = a.x - x;
    const float dy = a.y - y;
    return std::sqrt(dx * dx + dy * dy);
}

[[nodiscard]] MaskPathHit closest_path_point(const MaskPath& path, float x,
                                             float y) noexcept {
    MaskPathHit closest{};
    for (std::size_t index = 1U; index < path.vertices.size(); ++index) {
        const MaskPathVertex& a = path.vertices[index - 1U];
        const MaskPathVertex& b = path.vertices[index];
        const float dx = b.screen.x - a.screen.x;
        const float dy = b.screen.y - a.screen.y;
        const float squared = dx * dx + dy * dy;
        const float t = squared > 0.0F
            ? std::clamp(((x - a.screen.x) * dx + (y - a.screen.y) * dy) /
                             squared,
                         0.0F, 1.0F)
            : 0.0F;
        const float px = a.screen.x + dx * t;
        const float py = a.screen.y + dy * t;
        const float distance = std::sqrt((x - px) * (x - px) +
                                         (y - py) * (y - py));
        if (distance < closest.distance) {
            closest.distance = distance;
            closest.normalized = MaskViewportNormalizedPoint{
                a.normalized.x + (b.normalized.x - a.normalized.x) * t,
                a.normalized.y + (b.normalized.y - a.normalized.y) * t};
            closest.insertion_index = b.insertion_index;
        }
    }
    return closest;
}

[[nodiscard]] bool inside_mask(const scene::MaskInstance& mask,
                               MaskViewportNormalizedPoint point,
                               scene::Canvas canvas,
                               const MaskViewportInput& input) noexcept {
    if (mask.kind != scene::MaskKind::Bezier &&
        mask.rotation_turns != 0.0F) {
        point = unrotate_shape_point(
            point, mask.bounds, mask.rotation_turns,
            mask_coordinate_extent(canvas, input));
    }
    switch (mask.kind) {
    case scene::MaskKind::Rectangle:
        return point.x >= mask.bounds.left && point.x <= mask.bounds.right &&
               point.y >= mask.bounds.top && point.y <= mask.bounds.bottom;
    case scene::MaskKind::Ellipse: {
        const float radius_x = (mask.bounds.right - mask.bounds.left) * 0.5F;
        const float radius_y = (mask.bounds.bottom - mask.bounds.top) * 0.5F;
        if (radius_x <= 0.0F || radius_y <= 0.0F) {
            return false;
        }
        const float centre_x = (mask.bounds.left + mask.bounds.right) * 0.5F;
        const float centre_y = (mask.bounds.top + mask.bounds.bottom) * 0.5F;
        const float x = (point.x - centre_x) / radius_x;
        const float y = (point.y - centre_y) / radius_y;
        return x * x + y * y <= 1.0F;
    }
    case scene::MaskKind::Bezier: {
        if (mask.points.size() < 3U) {
            return false;
        }
        bool inside{};
        std::size_t previous = mask.points.size() - 1U;
        for (std::size_t index = 0U; index < mask.points.size(); ++index) {
            const scene::MaskPoint& a = mask.points[index];
            const scene::MaskPoint& b = mask.points[previous];
            const bool crosses = (a.y > point.y) != (b.y > point.y);
            if (crosses) {
                const float intersect =
                    (b.x - a.x) * (point.y - a.y) / (b.y - a.y) + a.x;
                if (point.x < intersect) {
                    inside = !inside;
                }
            }
            previous = index;
        }
        return inside;
    }
    }
    return false;
}

[[nodiscard]] const scene::MaskPoint* mask_point(
    const scene::MaskInstance& mask, doc::NodeId id) noexcept {
    const auto found = std::find_if(
        mask.points.begin(), mask.points.end(),
        [id](const scene::MaskPoint& point) noexcept {
            return point.source == id;
        });
    return found == mask.points.end() ? nullptr : &*found;
}

[[nodiscard]] scene::MaskPoint* mask_point(scene::MaskInstance& mask,
                                           doc::NodeId id) noexcept {
    const auto found = std::find_if(
        mask.points.begin(), mask.points.end(),
        [id](const scene::MaskPoint& point) noexcept {
            return point.source == id;
        });
    return found == mask.points.end() ? nullptr : &*found;
}

[[nodiscard]] std::pair<float, float> clamp_translation(
    const scene::MaskInstance& mask, float dx, float dy,
    std::optional<doc::NodeId> only_point = std::nullopt) noexcept {
    float minimum_x = 1.0F;
    float minimum_y = 1.0F;
    float maximum_x = 0.0F;
    float maximum_y = 0.0F;
    const auto include = [&](float x, float y) {
        minimum_x = std::min(minimum_x, x);
        minimum_y = std::min(minimum_y, y);
        maximum_x = std::max(maximum_x, x);
        maximum_y = std::max(maximum_y, y);
    };
    if (mask.kind != scene::MaskKind::Bezier) {
        include(mask.bounds.left, mask.bounds.top);
        include(mask.bounds.right, mask.bounds.bottom);
    } else {
        for (const scene::MaskPoint& point : mask.points) {
            if (only_point.has_value() && point.source != *only_point) {
                continue;
            }
            include(point.x, point.y);
            include(point.in_x, point.in_y);
            include(point.out_x, point.out_y);
        }
    }
    dx = std::clamp(dx, -minimum_x, 1.0F - maximum_x);
    dy = std::clamp(dy, -minimum_y, 1.0F - maximum_y);
    return {dx, dy};
}

void translate_mask(scene::MaskInstance& mask, float dx, float dy,
                    std::optional<doc::NodeId> only_point = std::nullopt) {
    if (mask.kind != scene::MaskKind::Bezier) {
        mask.bounds.left += dx;
        mask.bounds.right += dx;
        mask.bounds.top += dy;
        mask.bounds.bottom += dy;
        return;
    }
    for (scene::MaskPoint& point : mask.points) {
        if (only_point.has_value() && point.source != *only_point) {
            continue;
        }
        point.x += dx;
        point.y += dy;
        point.in_x += dx;
        point.in_y += dy;
        point.out_x += dx;
        point.out_y += dy;
    }
}

[[nodiscard]] std::pair<float, float> constrained_delta(float dx, float dy,
                                                        bool shift) noexcept {
    if (!shift) {
        return {dx, dy};
    }
    if (std::abs(dx) >= std::abs(dy)) {
        return {dx, 0.0F};
    }
    return {0.0F, dy};
}

[[nodiscard]] MaskViewportNormalizedPoint constrained_handle(
    MaskViewportNormalizedPoint anchor, MaskViewportNormalizedPoint value,
    bool shift) noexcept {
    if (shift) {
        constexpr float eighth_turn = 0.78539816339744830962F;
        const float dx = value.x - anchor.x;
        const float dy = value.y - anchor.y;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length > 0.0F) {
            const float angle = std::round(std::atan2(dy, dx) / eighth_turn) *
                                eighth_turn;
            value.x = anchor.x + std::cos(angle) * length;
            value.y = anchor.y + std::sin(angle) * length;
        }
    }
    value.x = std::clamp(value.x, 0.0F, 1.0F);
    value.y = std::clamp(value.y, 0.0F, 1.0F);
    return value;
}

[[nodiscard]] scene::MaskPointPatch point_patch(
    const scene::MaskPoint& before, const scene::MaskPoint& after) {
    scene::MaskPointPatch patch{};
    if (before.x != after.x) {
        patch.x = after.x;
    }
    if (before.y != after.y) {
        patch.y = after.y;
    }
    if (before.in_x != after.in_x) {
        patch.in_x = after.in_x;
    }
    if (before.in_y != after.in_y) {
        patch.in_y = after.in_y;
    }
    if (before.out_x != after.out_x) {
        patch.out_x = after.out_x;
    }
    if (before.out_y != after.out_y) {
        patch.out_y = after.out_y;
    }
    return patch;
}

[[nodiscard]] bool empty_patch(const scene::MaskPointPatch& patch) noexcept {
    return !patch.x.has_value() && !patch.y.has_value() &&
           !patch.in_x.has_value() && !patch.in_y.has_value() &&
           !patch.out_x.has_value() && !patch.out_y.has_value();
}

void draw_mask_point(gui::UiDrawList& list, gui::UiPoint point,
                     gui::UiColor color, float size = 7.0F) {
    list.add_rect(point.x - size * 0.5F, point.y - size * 0.5F, size, size,
                  color);
}

} // namespace

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
                                  const ViewportProjection* projection,
                                  std::span<const ViewportPlate> plates) {
    ViewportFrame frame{};
    // One lock-free fence per editor frame prevents a completion from becoming
    // visible halfway through a multi-model pick/outline pass.
    model_publication_fence_ = model_cache_.publication_epoch();

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
    if (selected_surface_value != nullptr && selected_surface_value->locked) {
        selected_surface_value = nullptr;
        surface_dragging_ = false;
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
                const auto mesh = projection_mesh_for(display);
                projected.visual = mesh != nullptr
                                       ? mesh_surface_visual(*mesh, true)
                                       : planar_surface_visual(display, true);
                break;
            }
            case output::ProjectionSurfaceShape::ImportedMesh: {
                const auto mesh = projection_mesh_for(display);
                projected.visual =
                    mesh != nullptr
                        ? mesh_surface_visual(*mesh, false, &display.geometry,
                                              display.mesh_scale)
                        : planar_surface_visual(display, true);
                break;
            }
            }
            project_surface(projected, frame.image, scene);
            projected_surfaces.push_back(std::move(projected));
        }
    }

    const bool primary_locked = scene::element_locked(scene, primary);
    if (primary_locked) {
        dragging_ = false;
    }
    const auto primary_world = primary_locked
                                   ? std::optional<scene::Mat4>{}
                                   : scene::world_for(scene, primary);
    const GizmoGeometry scene_gizmo =
        role_ == ViewportRole::Stage && show_gizmo_ &&
                selected_surface_value == nullptr && primary_world.has_value()
            ? gizmo_geometry(frame.image, scene,
                             *primary_world)
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
            if (projected.surface->locked) {
                continue;
            }
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

    const auto scene_hits_under_pointer = [&]() {
        const float light_radius = 12.0F * canvas_width / frame.image.width;
        return projection != nullptr && projection->project
                   ? scene::pick_scene_all(
                         scene, frame.pointer.x, frame.pointer.y,
                         projection->project, model_bounds, light_radius,
                         role_ == ViewportRole::Stage && show_overlays_)
                   : scene::pick_scene_all(
                         scene, scene.canvas, frame.pointer.x,
                         frame.pointer.y, model_bounds, light_radius,
                         role_ == ViewportRole::Stage && show_overlays_);
    };
    const auto prepare_deep_select =
        [&](const std::vector<scene::ScenePick>& hits) {
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
        };

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
                axis_world_ = surface_gizmo.world_axes[index];
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
            axis_world_ = scene_gizmo.world_axes[index];
            press_x_ = frame.pointer.x;
            press_y_ = frame.pointer.y;
            dragging_ = true;
            // The gizmo is drawn over the selected element. It owns a moving
            // press, but a stationary hold must still be able to inspect the
            // stack beneath it; otherwise selecting the front item makes the
            // depth chooser unreachable on the next press.
            prepare_deep_select(scene_hits_under_pointer());
        } else {
            const std::vector<scene::ScenePick> hits =
                scene_hits_under_pointer();
            if (!hits.empty()) {
                frame.picked = hits.front().node;
                prepare_deep_select(hits);
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
        if (!frame.picked.has_value() && !frame.surface_picked.has_value()) {
            frame.empty_canvas =
                role_ == ViewportRole::Stage
                    ? EmptyCanvasInteraction::Orbit
                    : EmptyCanvasInteraction::ClearSelection;
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
                    translation =
                        axis_world_ * (along * axis_world_length_);
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
                    const auto mesh = projection_mesh_for(edited);
                    projected.visual =
                        mesh != nullptr ? mesh_surface_visual(*mesh, true)
                             : planar_surface_visual(edited, true);
                    break;
                }
                case output::ProjectionSurfaceShape::ImportedMesh: {
                    const auto mesh = projection_mesh_for(edited);
                    projected.visual =
                        mesh != nullptr
                            ? mesh_surface_visual(*mesh, false,
                                                  &edited.geometry,
                                                  edited.mesh_scale)
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
                        axis_world_ * (along * axis_world_length_);
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
        for (const scene::SceneScreen& screen : scene.screens) {
            draw_screen(list, frame.image, scene, screen,
                        selected(selection, screen.node));
        }
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
        for (const scene::TrackedCameraView& tracked :
             scene.tracked_cameras) {
            draw_tracked_camera_overlay(list, frame.image, scene, tracked);
        }
        if (camera_overlay != nullptr) {
            draw_camera_overlay(list, frame.image, scene, *camera_overlay);
        }
        draw_light_overlays(list, frame.image, scene, selection, primary);
    }
    // The plate is a region of the *image*, not a thing in the scene, so it
    // belongs on Program Output as much as on Stage - more, in fact, since
    // Program is the picture an operator is judging when they decide whether
    // the margin is wide enough.
    if (show_overlays_) {
        for (const ViewportPlate& plate : plates) {
            draw_inner_frustum_overlay(list, frame.image, scene, plate);
        }
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

MaskViewportFrame ViewportPanel::draw_masks(
    gui::UiContext& ctx, const gui::Rect& bounds,
    const rhi::Texture& render_target, const scene::Scene& scene,
    const MaskViewportInput& input, const scene::Camera* camera_overlay,
    float frames_per_second, const ViewportProjection* projection) {
    MaskViewportFrame frame{};
    frame.selection = input.selection;

    // Entering the mask domain terminates any scene/surface gesture. The full
    // panel occluder below then leaves the ordinary draw path visual-only.
    dragging_ = false;
    surface_dragging_ = false;
    drag_axis_ = GizmoAxis::None;
    deep_select_armed_ = false;
    deep_select_open_ = false;
    deep_candidates_.clear();
    const gui::Rect ordinary_interaction_block = bounds;
    const ViewportFrame ordinary = draw(
        ctx, bounds, render_target, scene, {}, {}, camera_overlay,
        &ordinary_interaction_block, frames_per_second, nullptr, {},
        projection);
    frame.image = ordinary.image;
    // Mask coordinates resolve against whatever canvas this panel is showing:
    // the authored camera canvas on Stage, the output raster on Program. The
    // role no longer decides whether masks may be authored - having a picture
    // and a canvas to resolve against does.
    const scene::Canvas mask_canvas = viewport_canvas(scene, projection);
    if (frame.image.width <= 0.0F || frame.image.height <= 0.0F ||
        mask_canvas.width == 0U || mask_canvas.height == 0U) {
        mask_gesture_active_ = false;
        mask_gesture_cancelled_ = false;
        return frame;
    }

    std::vector<const scene::MaskInstance*> masks;
    masks.reserve(input.masks.size());
    for (const scene::MaskInstance& mask : input.masks) {
        if (mask.scene == input.target.scene &&
            mask.target == input.target.target) {
            masks.push_back(&mask);
        }
    }
    std::sort(masks.begin(), masks.end(),
              [](const scene::MaskInstance* left,
                 const scene::MaskInstance* right) noexcept {
                  return std::tie(left->order, left->source) <
                         std::tie(right->order, right->source);
              });
    const auto find_mask = [&masks](doc::NodeId id)
        -> const scene::MaskInstance* {
        const auto found = std::find_if(
            masks.begin(), masks.end(),
            [id](const scene::MaskInstance* mask) noexcept {
                return mask->source == id;
            });
        return found == masks.end() ? nullptr : *found;
    };

    if (frame.selection.scene != input.target.scene ||
        frame.selection.target != input.target.target) {
        frame.selection = MasksPanelSelection{
            input.target.scene, input.target.target, {}, {}};
        frame.selection_changed = true;
        mask_gesture_active_ = false;
        mask_gesture_cancelled_ = false;
    }
    const scene::MaskInstance* selected_mask =
        find_mask(frame.selection.mask);
    if (frame.selection.mask && selected_mask == nullptr) {
        frame.selection.mask = {};
        frame.selection.point.reset();
        frame.selection_changed = true;
        mask_gesture_active_ = false;
        mask_gesture_cancelled_ = false;
    } else if (selected_mask != nullptr &&
               frame.selection.point.has_value() &&
               mask_point(*selected_mask, *frame.selection.point) == nullptr) {
        frame.selection.point.reset();
        frame.selection_changed = true;
        mask_gesture_active_ = false;
        mask_gesture_cancelled_ = false;
    }

    frame.mapping_valid = mask_normalized_to_viewport(
                              frame.image, mask_canvas,
                              input.target.coordinate_space,
                              source_mapping(input), 0.5F, 0.5F)
                              .has_value();
    const gui::Rect coverage_button{
        frame.image.x + std::max(frame.image.width - 106.0F, 0.0F),
        frame.image.y + 5.0F,
        std::min(101.0F, frame.image.width), 21.0F};
    const gui::Interaction coverage_interaction =
        ctx.grab(mix(id_, 0x4D41534BU), coverage_button);
    const gui::Rect mask_interaction_bounds =
        !frame.mapping_valid || coverage_interaction.hovered
        ? gui::Rect{}
        : frame.image;
    const gui::Interaction interaction =
        ctx.grab(mix(id_, 0x4D41534CULL), mask_interaction_bounds);
    frame.hovered = interaction.hovered;
    frame.held = interaction.held;

    const auto normalized_pointer = mask_normalized_point(
        frame.image, mask_canvas, input, ctx.pointer_x(), ctx.pointer_y());
    std::vector<MaskPath> paths;
    paths.reserve(masks.size());
    for (const scene::MaskInstance* mask : masks) {
        paths.push_back(build_mask_path(*mask, frame.image, mask_canvas,
                                        input));
    }

    MaskPick pick{};
    if (frame.mapping_valid && normalized_pointer.has_value() &&
        !mask_gesture_active_ && !mask_gesture_cancelled_) {
        // A selected primitive exposes its transform directly on the picture:
        // four corner handles resize it and the stem above the top edge rotates
        // it. These handles outrank picking the mask body beneath them.
        if (selected_mask != nullptr &&
            selected_mask->kind != scene::MaskKind::Bezier) {
            constexpr std::array hits{
                MaskViewportHit::ResizeTopLeft,
                MaskViewportHit::ResizeTopRight,
                MaskViewportHit::ResizeBottomRight,
                MaskViewportHit::ResizeBottomLeft};
            const std::array local{
                MaskViewportNormalizedPoint{selected_mask->bounds.left,
                                            selected_mask->bounds.top},
                MaskViewportNormalizedPoint{selected_mask->bounds.right,
                                            selected_mask->bounds.top},
                MaskViewportNormalizedPoint{selected_mask->bounds.right,
                                            selected_mask->bounds.bottom},
                MaskViewportNormalizedPoint{selected_mask->bounds.left,
                                            selected_mask->bounds.bottom}};
            const auto extent = mask_coordinate_extent(mask_canvas, input);
            float best = mask_shape_handle_hit_pixels;
            for (std::size_t index = 0U; index < local.size(); ++index) {
                const auto rotated = rotate_shape_point(
                    local[index], selected_mask->bounds,
                    selected_mask->rotation_turns, extent);
                const auto screen = mask_screen_point(
                    frame.image, mask_canvas, input, rotated);
                if (!screen.has_value()) {
                    continue;
                }
                const float distance = point_distance(
                    *screen, ctx.pointer_x(), ctx.pointer_y());
                if (distance < best) {
                    best = distance;
                    pick = MaskPick{hits[index], selected_mask};
                }
            }
            const MaskViewportNormalizedPoint centre{
                (selected_mask->bounds.left + selected_mask->bounds.right) *
                    0.5F,
                (selected_mask->bounds.top + selected_mask->bounds.bottom) *
                    0.5F};
            const auto top = rotate_shape_point(
                {centre.x, selected_mask->bounds.top},
                selected_mask->bounds, selected_mask->rotation_turns, extent);
            const auto centre_screen = mask_screen_point(
                frame.image, mask_canvas, input, centre);
            const auto top_screen = mask_screen_point(
                frame.image, mask_canvas, input, top);
            if (centre_screen.has_value() && top_screen.has_value()) {
                const float dx = top_screen->x - centre_screen->x;
                const float dy = top_screen->y - centre_screen->y;
                const float length = std::sqrt(dx * dx + dy * dy);
                if (length > 1.0F) {
                    const gui::UiPoint rotate_screen{
                        top_screen->x + dx / length * 24.0F,
                        top_screen->y + dy / length * 24.0F};
                    const float distance = point_distance(
                        rotate_screen, ctx.pointer_x(), ctx.pointer_y());
                    if (distance < best) {
                        pick = MaskPick{MaskViewportHit::Rotate,
                                        selected_mask};
                    }
                }
            }
        }

        // Selected handles are deliberately first. A handle crossing its own
        // anchor remains draggable instead of degrading into a point move.
        if (pick.hit == MaskViewportHit::None && selected_mask != nullptr &&
            frame.selection.point.has_value()) {
            const scene::MaskPoint* point =
                mask_point(*selected_mask, *frame.selection.point);
            if (point != nullptr) {
                const auto anchor = mask_screen_point(
                    frame.image, mask_canvas, input, {point->x, point->y});
                const auto in = mask_screen_point(
                    frame.image, mask_canvas, input,
                    {point->in_x, point->in_y});
                const auto out = mask_screen_point(
                    frame.image, mask_canvas, input,
                    {point->out_x, point->out_y});
                float best = mask_handle_hit_pixels;
                if (anchor.has_value() && out.has_value() &&
                    point_distance(*anchor, out->x, out->y) > 2.0F) {
                    const float distance = point_distance(
                        *out, ctx.pointer_x(), ctx.pointer_y());
                    if (distance <= best) {
                        best = distance;
                        pick = MaskPick{MaskViewportHit::OutHandle,
                                        selected_mask, point->source};
                    }
                }
                if (anchor.has_value() && in.has_value() &&
                    point_distance(*anchor, in->x, in->y) > 2.0F) {
                    const float distance = point_distance(
                        *in, ctx.pointer_x(), ctx.pointer_y());
                    if (distance < best) {
                        pick = MaskPick{MaskViewportHit::InHandle,
                                        selected_mask, point->source};
                    }
                }
            }
        }

        // Anchors outrank path insertion and whole-mask translation.
        if (pick.hit == MaskViewportHit::None) {
            float best = mask_point_hit_pixels;
            for (auto mask_it = masks.rbegin(); mask_it != masks.rend();
                 ++mask_it) {
                for (const scene::MaskPoint& point : (*mask_it)->points) {
                    const auto screen = mask_screen_point(
                        frame.image, mask_canvas, input, {point.x, point.y});
                    if (!screen.has_value()) {
                        continue;
                    }
                    const float distance = point_distance(
                        *screen, ctx.pointer_x(), ctx.pointer_y());
                    if (distance < best) {
                        best = distance;
                        pick = MaskPick{MaskViewportHit::Point, *mask_it,
                                        point.source};
                    }
                }
            }
        }

        if (pick.hit == MaskViewportHit::None && input.pen_active &&
            selected_mask != nullptr &&
            selected_mask->kind == scene::MaskKind::Bezier) {
            const auto selected_it = std::find(masks.begin(), masks.end(),
                                               selected_mask);
            if (selected_it != masks.end()) {
                const std::size_t index = static_cast<std::size_t>(
                    selected_it - masks.begin());
                const MaskPathHit path_hit = closest_path_point(
                    paths[index], ctx.pointer_x(), ctx.pointer_y());
                if (path_hit.distance <= mask_path_hit_pixels) {
                    const scene::MaskPointGeometry geometry{
                        path_hit.normalized.x, path_hit.normalized.y,
                        path_hit.normalized.x, path_hit.normalized.y,
                        path_hit.normalized.x, path_hit.normalized.y};
                    pick = MaskPick{MaskViewportHit::Path, selected_mask, {},
                                    path_hit.insertion_index, geometry};
                }
            }
        }

        if (pick.hit == MaskViewportHit::None) {
            for (std::size_t reverse = masks.size(); reverse > 0U; --reverse) {
                const std::size_t index = reverse - 1U;
                const MaskPathHit path_hit = closest_path_point(
                    paths[index], ctx.pointer_x(), ctx.pointer_y());
                if (path_hit.distance <= mask_path_hit_pixels ||
                    inside_mask(*masks[index], *normalized_pointer,
                                mask_canvas, input)) {
                    pick = MaskPick{MaskViewportHit::WholeMask, masks[index]};
                    break;
                }
            }
        }
    }
    frame.hit = mask_gesture_active_ ? mask_gesture_hit_ : pick.hit;

    if (interaction.pressed && pick.mask != nullptr) {
        if (frame.selection.mask != pick.mask->source ||
            frame.selection.point != pick.point) {
            frame.selection = MasksPanelSelection{
                input.target.scene, input.target.target, pick.mask->source,
                pick.point};
            frame.selection_changed = true;
        }
        if (input.authoring_enabled && normalized_pointer.has_value()) {
            mask_gesture_active_ = true;
            mask_gesture_cancelled_ = false;
            mask_gesture_hit_ = pick.hit;
            mask_gesture_scene_ = input.target.scene;
            mask_gesture_target_ = input.target.target;
            mask_gesture_mask_ = pick.mask->source;
            mask_gesture_point_ = pick.point;
            mask_press_ = *normalized_pointer;
            mask_gesture_start_ = *pick.mask;
            mask_gesture_preview_ = *pick.mask;
            mask_insert_index_ = pick.insertion_index;
            mask_insert_geometry_ = pick.insertion_geometry;
            frame.hit = pick.hit;
        }
    }

    if (mask_gesture_active_ &&
        (mask_gesture_scene_ != input.target.scene ||
         mask_gesture_target_ != input.target.target ||
         find_mask(mask_gesture_mask_) == nullptr ||
         !input.authoring_enabled)) {
        mask_gesture_active_ = false;
        mask_gesture_cancelled_ = true;
    }
    if (mask_gesture_active_ && ctx.key_pressed(gui::Key::Escape)) {
        mask_gesture_active_ = false;
        mask_gesture_cancelled_ = true;
    }

    float gesture_dx{};
    float gesture_dy{};
    bool gesture_changed{};
    if (mask_gesture_active_ && (interaction.held || interaction.released) &&
        normalized_pointer.has_value()) {
        mask_gesture_preview_ = mask_gesture_start_;
        switch (mask_gesture_hit_) {
        case MaskViewportHit::WholeMask: {
            auto delta = constrained_delta(
                normalized_pointer->x - mask_press_.x,
                normalized_pointer->y - mask_press_.y, ctx.shift_held());
            delta = clamp_translation(mask_gesture_start_, delta.first,
                                      delta.second);
            gesture_dx = delta.first;
            gesture_dy = delta.second;
            translate_mask(mask_gesture_preview_, gesture_dx, gesture_dy);
            gesture_changed = std::abs(gesture_dx) + std::abs(gesture_dy) >=
                              mask_minimum_drag;
            break;
        }
        case MaskViewportHit::Point: {
            auto delta = constrained_delta(
                normalized_pointer->x - mask_press_.x,
                normalized_pointer->y - mask_press_.y, ctx.shift_held());
            delta = clamp_translation(mask_gesture_start_, delta.first,
                                      delta.second, mask_gesture_point_);
            gesture_dx = delta.first;
            gesture_dy = delta.second;
            translate_mask(mask_gesture_preview_, gesture_dx, gesture_dy,
                           mask_gesture_point_);
            gesture_changed = std::abs(gesture_dx) + std::abs(gesture_dy) >=
                              mask_minimum_drag;
            break;
        }
        case MaskViewportHit::InHandle:
        case MaskViewportHit::OutHandle: {
            if (!mask_gesture_point_.has_value()) {
                break;
            }
            scene::MaskPoint* point = mask_point(
                mask_gesture_preview_, *mask_gesture_point_);
            if (point == nullptr) {
                break;
            }
            const MaskViewportNormalizedPoint anchor{point->x, point->y};
            const MaskViewportNormalizedPoint value = constrained_handle(
                anchor, *normalized_pointer, ctx.shift_held());
            if (mask_gesture_hit_ == MaskViewportHit::InHandle) {
                point->in_x = value.x;
                point->in_y = value.y;
                if (!ctx.alt_held()) {
                    point->out_x = std::clamp(
                        anchor.x * 2.0F - value.x, 0.0F, 1.0F);
                    point->out_y = std::clamp(
                        anchor.y * 2.0F - value.y, 0.0F, 1.0F);
                }
            } else {
                point->out_x = value.x;
                point->out_y = value.y;
                if (!ctx.alt_held()) {
                    point->in_x = std::clamp(
                        anchor.x * 2.0F - value.x, 0.0F, 1.0F);
                    point->in_y = std::clamp(
                        anchor.y * 2.0F - value.y, 0.0F, 1.0F);
                }
            }
            const scene::MaskPoint* before = mask_point(
                mask_gesture_start_, *mask_gesture_point_);
            gesture_changed = before != nullptr &&
                !empty_patch(point_patch(*before, *point));
            break;
        }
        case MaskViewportHit::ResizeTopLeft:
        case MaskViewportHit::ResizeTopRight:
        case MaskViewportHit::ResizeBottomRight:
        case MaskViewportHit::ResizeBottomLeft: {
            const auto extent = mask_coordinate_extent(mask_canvas, input);
            const auto local = unrotate_shape_point(
                *normalized_pointer, mask_gesture_start_.bounds,
                mask_gesture_start_.rotation_turns, extent);
            const float centre_x =
                (mask_gesture_start_.bounds.left +
                 mask_gesture_start_.bounds.right) * 0.5F;
            const float centre_y =
                (mask_gesture_start_.bounds.top +
                 mask_gesture_start_.bounds.bottom) * 0.5F;
            float half_x = std::abs(local.x - centre_x);
            float half_y = std::abs(local.y - centre_y);
            if (ctx.shift_held()) {
                const float held = std::max(half_x * extent.x,
                                            half_y * extent.y);
                half_x = held / extent.x;
                half_y = held / extent.y;
            }
            half_x = std::clamp(
                half_x, 0.002F, std::min(centre_x, 1.0F - centre_x));
            half_y = std::clamp(
                half_y, 0.002F, std::min(centre_y, 1.0F - centre_y));
            mask_gesture_preview_.bounds = scene::MaskBounds{
                centre_x - half_x, centre_y - half_y,
                centre_x + half_x, centre_y + half_y};
            gesture_changed =
                mask_gesture_preview_.bounds != mask_gesture_start_.bounds;
            break;
        }
        case MaskViewportHit::Rotate: {
            constexpr float tau = 6.28318530717958647692F;
            const auto extent = mask_coordinate_extent(mask_canvas, input);
            const float centre_x =
                (mask_gesture_start_.bounds.left +
                 mask_gesture_start_.bounds.right) * 0.5F;
            const float centre_y =
                (mask_gesture_start_.bounds.top +
                 mask_gesture_start_.bounds.bottom) * 0.5F;
            const float dx = (normalized_pointer->x - centre_x) * extent.x;
            const float dy = (normalized_pointer->y - centre_y) * extent.y;
            if (std::abs(dx) + std::abs(dy) > 1.0e-5F) {
                float turns = std::atan2(dy, dx) / tau + 0.25F;
                if (ctx.shift_held()) {
                    turns = std::round(turns * 24.0F) / 24.0F;
                }
                turns -= std::floor(turns + 0.5F);
                mask_gesture_preview_.rotation_turns = turns;
                gesture_changed = std::abs(
                    turns - mask_gesture_start_.rotation_turns) >=
                    mask_minimum_drag;
            }
            break;
        }
        case MaskViewportHit::Path:
        case MaskViewportHit::None:
            break;
        }
    }
    frame.dragging = gesture_changed;

    if (interaction.released) {
        frame.released = true;
        if (mask_gesture_cancelled_) {
            frame.cancelled = true;
        } else if (mask_gesture_active_) {
            switch (mask_gesture_hit_) {
            case MaskViewportHit::Path:
                frame.intent = MaskViewportPointInsertIntent{
                    mask_gesture_scene_, mask_gesture_target_,
                    mask_gesture_mask_, mask_insert_index_,
                    mask_insert_geometry_};
                break;
            case MaskViewportHit::WholeMask:
                if (gesture_changed) {
                    frame.intent = MaskViewportTranslateIntent{
                        mask_gesture_scene_, mask_gesture_target_,
                        mask_gesture_mask_, gesture_dx, gesture_dy};
                }
                break;
            case MaskViewportHit::Point:
            case MaskViewportHit::InHandle:
            case MaskViewportHit::OutHandle:
                if (gesture_changed && mask_gesture_point_.has_value()) {
                    const scene::MaskPoint* before = mask_point(
                        mask_gesture_start_, *mask_gesture_point_);
                    const scene::MaskPoint* after = mask_point(
                        mask_gesture_preview_, *mask_gesture_point_);
                    if (before != nullptr && after != nullptr) {
                        scene::MaskPointPatch patch =
                            point_patch(*before, *after);
                        if (!empty_patch(patch)) {
                            frame.intent = MaskViewportPointEditIntent{
                                mask_gesture_scene_, mask_gesture_target_,
                                mask_gesture_mask_, *mask_gesture_point_,
                                std::move(patch)};
                        }
                    }
                }
                break;
            case MaskViewportHit::ResizeTopLeft:
            case MaskViewportHit::ResizeTopRight:
            case MaskViewportHit::ResizeBottomRight:
            case MaskViewportHit::ResizeBottomLeft:
            case MaskViewportHit::Rotate:
                if (gesture_changed) {
                    frame.intent = MaskViewportShapeEditIntent{
                        mask_gesture_scene_, mask_gesture_target_,
                        mask_gesture_mask_, mask_gesture_preview_.bounds,
                        mask_gesture_preview_.rotation_turns};
                }
                break;
            case MaskViewportHit::None:
                break;
            }
        }
        mask_gesture_active_ = false;
        mask_gesture_cancelled_ = false;
        mask_gesture_hit_ = MaskViewportHit::None;
        mask_gesture_point_.reset();
    }

    ctx.push_clip(frame.image);
    gui::UiDrawList& list = ctx.draw_list();
    const float canvas_scale =
        0.5F * (frame.image.width / static_cast<float>(mask_canvas.width) +
                frame.image.height / static_cast<float>(mask_canvas.height));
    for (std::size_t index = 0U; index < masks.size(); ++index) {
        const scene::MaskInstance* display = masks[index];
        if (mask_gesture_active_ && display->source == mask_gesture_mask_) {
            display = &mask_gesture_preview_;
        }
        const MaskPath path = build_mask_path(
            *display, frame.image, mask_canvas, input);
        const float expansion = std::clamp(
            std::abs(display->expansion_canvas_px) * canvas_scale * 2.0F +
                1.0F,
            1.0F, 80.0F);
        const float feather = std::clamp(
            display->feather_canvas_px * canvas_scale * 2.0F + expansion,
            expansion, 96.0F);
        if (display->feather_canvas_px > 0.0F) {
            draw_mask_path(list, path, feather, mask_feather);
        }
        if (display->expansion_canvas_px != 0.0F) {
            draw_mask_path(list, path, expansion, mask_expansion);
        }
        draw_mask_path(list, path, 4.5F, outline_shadow);
        const bool selected = display->source == frame.selection.mask;
        draw_mask_path(list, path, selected ? 2.2F : 1.4F,
                       selected ? mask_outline : mask_secondary);
        if (selected && display->kind != scene::MaskKind::Bezier) {
            const auto extent = mask_coordinate_extent(mask_canvas, input);
            const MaskViewportNormalizedPoint centre{
                (display->bounds.left + display->bounds.right) * 0.5F,
                (display->bounds.top + display->bounds.bottom) * 0.5F};
            const std::array local{
                MaskViewportNormalizedPoint{display->bounds.left,
                                            display->bounds.top},
                MaskViewportNormalizedPoint{display->bounds.right,
                                            display->bounds.top},
                MaskViewportNormalizedPoint{display->bounds.right,
                                            display->bounds.bottom},
                MaskViewportNormalizedPoint{display->bounds.left,
                                            display->bounds.bottom}};
            for (const auto point : local) {
                const auto screen = mask_screen_point(
                    frame.image, mask_canvas, input,
                    rotate_shape_point(point, display->bounds,
                                       display->rotation_turns, extent));
                if (screen.has_value()) {
                    draw_mask_point(list, *screen, mask_handle, 8.0F);
                    draw_mask_point(list, *screen, ctx.theme().track, 4.0F);
                }
            }
            const auto top = rotate_shape_point(
                {centre.x, display->bounds.top}, display->bounds,
                display->rotation_turns, extent);
            const auto centre_screen = mask_screen_point(
                frame.image, mask_canvas, input, centre);
            const auto top_screen = mask_screen_point(
                frame.image, mask_canvas, input, top);
            if (centre_screen.has_value() && top_screen.has_value()) {
                const float dx = top_screen->x - centre_screen->x;
                const float dy = top_screen->y - centre_screen->y;
                const float length = std::sqrt(dx * dx + dy * dy);
                if (length > 1.0F) {
                    const gui::UiPoint rotate_screen{
                        top_screen->x + dx / length * 24.0F,
                        top_screen->y + dy / length * 24.0F};
                    line(list, *top_screen, rotate_screen, 1.2F, mask_handle);
                    constexpr float diamond = 5.0F;
                    const std::array<gui::UiPoint, 4U> points{{
                        {rotate_screen.x, rotate_screen.y - diamond},
                        {rotate_screen.x + diamond, rotate_screen.y},
                        {rotate_screen.x, rotate_screen.y + diamond},
                        {rotate_screen.x - diamond, rotate_screen.y}}};
                    list.add_convex_poly(points, mask_handle);
                }
            }
        }
        if (!selected || display->kind != scene::MaskKind::Bezier) {
            continue;
        }
        for (const scene::MaskPoint& point : display->points) {
            const auto anchor = mask_screen_point(
                frame.image, mask_canvas, input, {point.x, point.y});
            if (!anchor.has_value()) {
                continue;
            }
            const bool point_selected =
                frame.selection.point == point.source;
            if (point_selected) {
                const auto in = mask_screen_point(
                    frame.image, mask_canvas, input,
                    {point.in_x, point.in_y});
                const auto out = mask_screen_point(
                    frame.image, mask_canvas, input,
                    {point.out_x, point.out_y});
                if (in.has_value() &&
                    point_distance(*anchor, in->x, in->y) > 2.0F) {
                    line(list, *anchor, *in, 1.2F, mask_handle);
                    draw_mask_point(list, *in, mask_handle, 5.0F);
                }
                if (out.has_value() &&
                    point_distance(*anchor, out->x, out->y) > 2.0F) {
                    line(list, *anchor, *out, 1.2F, mask_handle);
                    draw_mask_point(list, *out, mask_handle, 5.0F);
                }
            }
            draw_mask_point(list, *anchor,
                            point_selected ? mask_handle : mask_outline,
                            point_selected ? 8.0F : 6.0F);
        }
    }

    if (!frame.mapping_valid) {
        ctx.panel(gui::Rect{frame.image.x + 8.0F, frame.image.y + 8.0F,
                            244.0F, 23.0F},
                  gui::UiColor{0.12F, 0.04F, 0.04F, 0.90F});
        ctx.label(gui::Rect{frame.image.x + 14.0F, frame.image.y + 8.0F,
                            232.0F, 23.0F},
                  "SOURCE MASK MAPPING UNAVAILABLE", ctx.theme().warning);
    }
    // A Masks tab changes what clicking the picture means. Keep that mode
    // visible in both Stage and Program Output instead of making a failed
    // object-selection click feel like a broken picker.
    constexpr std::string_view selection_notice =
        "OBJECT SELECTION LOCKED WHILE MASKS IS OPEN";
    const float selection_notice_width = std::min(
        std::max(frame.image.width - 16.0F, 0.0F), 326.0F);
    const gui::Rect selection_notice_bounds{
        frame.image.x + (frame.image.width - selection_notice_width) * 0.5F,
        frame.image.y + std::max(frame.image.height - 29.0F, 0.0F),
        selection_notice_width, 23.0F};
    ctx.panel(selection_notice_bounds,
              gui::UiColor{0.03F, 0.04F, 0.05F, 0.90F});
    ctx.label(gui::Rect{selection_notice_bounds.x + 8.0F,
                        selection_notice_bounds.y,
                        std::max(selection_notice_bounds.width - 16.0F, 0.0F),
                        selection_notice_bounds.height},
              selection_notice, ctx.theme().text_muted);
    const gui::UiColor coverage_body =
        coverage_interaction.held
        ? ctx.theme().widget_active
        : (input.coverage_visible ? ctx.theme().selection
                                  : (coverage_interaction.hovered
                                         ? ctx.theme().widget_hover
                                         : ctx.theme().widget));
    ctx.panel(coverage_button, coverage_body);
    ctx.label(coverage_button,
              input.coverage_visible ? "COVERAGE ON" : "COVERAGE",
              ctx.theme().text);
    ctx.pop_clip();

    if (coverage_interaction.clicked) {
        frame.intent = MaskViewportCoverageIntent{
            input.target.scene, input.target.target,
            !input.coverage_visible};
    }
    return frame;
}

} // namespace mediaserver::editor
