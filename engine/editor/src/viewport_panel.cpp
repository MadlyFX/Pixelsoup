#include <mediaserver/editor/viewport_panel.hpp>
#include <mediaserver/editor/light_guides.hpp>
#include <mediaserver/editor/operator_widgets.hpp>

#include <mediaserver/scene/manipulate.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <vector>
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
constexpr float view_ring_radius_pixels = 86.0F;
constexpr float view_ring_hit_pixels = 6.0F;
constexpr std::size_t maximum_imported_wire_triangles = 4096U;
constexpr std::size_t maximum_cached_model_bounds = 128U;
constexpr std::size_t maximum_cached_projection_surfaces = 256U;
constexpr double deep_select_hold_seconds = 0.45;
constexpr float deep_select_motion_pixels = 4.0F;

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
constexpr gui::UiColor gizmo_hot{1.0F, 0.94F, 0.56F, 1.0F};
constexpr gui::UiColor gizmo_view_color{0.90F, 0.92F, 0.96F, 0.85F};

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
            const char* name = "Point Light  ";
            switch (light.kind) {
            case scene::LocalLightKind::Point: break;
            case scene::LocalLightKind::Spot: name = "Spot Light  "; break;
            case scene::LocalLightKind::Rectangle: name = "Rectangle Light  "; break;
            case scene::LocalLightKind::Sphere: name = "Sphere Light  "; break;
            }
            return name + std::to_string(node.value());
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
                         const scene::Camera& camera, bool selected = false) {
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
    const gui::UiColor frustum = selected
                                     ? outline_primary
                                     : gui::UiColor{0.55F, 0.62F, 0.76F, 0.72F};
    const float width = selected ? 1.8F : 1.2F;
    for (std::size_t index = 0U; index < corners.size(); ++index) {
        projected_line(list, image, view, camera.eye, corners[index], width,
                       frustum);
        projected_line(list, image, view, corners[index],
                       corners[(index + 1U) % corners.size()], width, frustum);
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

void draw_light_guide(gui::UiDrawList& list, const gui::Rect& image,
                      const scene::Scene& view,
                      const scene::ViewMatrices& matrices,
                      const LightGuide& guide, gui::UiColor color, bool active) {
    for (const LightGuideSegment& edge : guide.segments()) {
        const auto points = project_light_guide_segment(edge, matrices, view.canvas);
        if (!points) continue;
        const auto a = to_ui(image, view.canvas, (*points)[0]);
        const auto b = to_ui(image, view.canvas, (*points)[1]);
        gui::UiColor ink = color;
        float thickness = active ? 2.0F : 1.4F;
        switch (edge.style) {
        case LightGuideStyle::Range: ink.a = 0.42F; thickness = 1.0F; break;
        case LightGuideStyle::InnerCone: ink.a = 0.55F; thickness = 1.2F; break;
        case LightGuideStyle::OuterCone: ink.a = active ? 0.9F : 0.6F; break;
        case LightGuideStyle::Source:
        case LightGuideStyle::Direction: break;
        }
        if (edge.style != LightGuideStyle::Range) {
            line(list, a, b, thickness + 2.0F, outline_shadow);
        }
        line(list, a, b, thickness, ink);
    }
}

void light_guide_caption(gui::UiContext& ctx, const gui::Rect& image,
                         std::string_view shape, std::string_view extent) {
    if (image.width < 160.0F || image.height < 65.0F) return;
    const gui::Rect caption{image.x + 8.0F, image.y + image.height - 50.0F,
                            std::min(image.width - 16.0F, 395.0F), 42.0F};
    ctx.panel(caption, gui::UiColor{0.015F, 0.025F, 0.04F, 0.88F});
    ctx.label({caption.x + 7, caption.y + 2, caption.width - 14, 18},
              shape, outline_primary);
    ctx.label({caption.x + 7, caption.y + 21, caption.width - 14, 18},
              extent, ctx.theme().text);
}

void draw_light_overlays(gui::UiContext& ctx, gui::UiDrawList& list, const gui::Rect& image,
                         const scene::Scene& view,
                         std::span<const doc::NodeId> selection,
                         doc::NodeId primary) {
    const auto matrices = scene::view_for_slice(view.camera, view.canvas,
                                               scene::whole(view.canvas));
    for (const auto& light : view.point_lights) {
        const bool active = selected(selection, light.source);
        const gui::UiColor color =
            active ? (light.source == primary ? outline_primary
                                              : outline_secondary)
                   : light_color;
        draw_light_icon(list, image, view, light.position, color, false);
        draw_light_guide(list, image, view, matrices, light_guide(light, active), color, active);
        if (active && light.source == primary) {
            std::array<char, 128U> shape{}, extent{};
            switch (light.kind) {
            case scene::LocalLightKind::Point:
                std::snprintf(shape.data(), shape.size(), "POINT | omnidirectional source");
                break;
            case scene::LocalLightKind::Spot:
                std::snprintf(shape.data(), shape.size(), "SPOT | inner %.3g / outer %.3g deg (half-angles)",
                              light.inner_cone * 360.0F, light.outer_cone * 360.0F);
                break;
            case scene::LocalLightKind::Rectangle:
                std::snprintf(shape.data(), shape.size(), "RECT | %.3g x %.3g m | %s sided",
                              light.width, light.height, light.two_sided ? "two" : "one");
                break;
            case scene::LocalLightKind::Sphere:
                std::snprintf(shape.data(), shape.size(), "SPHERE | source radius %.3g m", light.radius);
                break;
            }
            std::snprintf(extent.data(), extent.size(), "Dashed: range cutoff %.3g m", light.range);
            light_guide_caption(ctx, image, shape.data(), extent.data());
        }
    }
    for (const auto& light : view.directional_lights) {
        const scene::Vec3 anchor = scene::directional_light_anchor(view, light);
        const auto marker = scene::project_to_canvas(view.camera, view.canvas, anchor);
        if (!marker.visible || !std::isfinite(marker.x) || !std::isfinite(marker.y) ||
            !std::isfinite(marker.depth) || marker.x < 0.0F || marker.y < 0.0F ||
            marker.x > view.canvas.width || marker.y > view.canvas.height ||
            marker.depth < 0.0F || marker.depth > 1.0F) {
            continue;
        }
        const bool active = selected(selection, light.source);
        const gui::UiColor color =
            active ? (light.source == primary ? outline_primary
                                              : outline_secondary)
                   : light_color;
        draw_light_icon(list, image, view, anchor, color, true);
        draw_light_guide(list, image, view, matrices,
                         directional_light_guide(
                             anchor, light.direction,
                             directional_light_arrow_length(view.camera, anchor,
                                                            image.height)),
                         color, active);
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
    std::map<doc::NodeId,scene::ModelLocalBounds> batches;
    for (const auto& model : view.models) {
        if (selected(selection, model.source_node)) {
            const auto bounds =
                model_bounds ? model_bounds(model) : std::nullopt;
            if (model.motion_source.value()) {
                const auto local=bounds.value_or(scene::ModelLocalBounds{});
                const auto first=scene::transform_point(model.world,local.minimum);
                auto [entry,added]=batches.try_emplace(model.source_node);
                if (added) entry->second.minimum=entry->second.maximum=first;
                auto& group=entry->second;
                for (const float x : {local.minimum.x,local.maximum.x})
                    for (const float y : {local.minimum.y,local.maximum.y})
                        for (const float z : {local.minimum.z,local.maximum.z}) {
                            const auto p=scene::transform_point(model.world,{x,y,z});
                            group.minimum={std::min(group.minimum.x,p.x),std::min(group.minimum.y,p.y),std::min(group.minimum.z,p.z)};
                            group.maximum={std::max(group.maximum.x,p.x),std::max(group.maximum.y,p.y),std::max(group.maximum.z,p.z)};
                        }
                continue;
            }
            wire_box(list, image, view, model.world,
                     model.source_node == primary ? outline_primary
                                                  : outline_secondary,
                     bounds.value_or(scene::ModelLocalBounds{}), projection);
        }
    }
    // Picking and editing address the group. Thousands of per-tree outlines can
    // exhaust the UI vertex budget and prevent the Inspector from presenting.
    for (const auto& [id,bounds] : batches)
        wire_box(list,image,view,scene::identity(),id==primary?outline_primary:outline_secondary,bounds,projection);
}

constexpr std::size_t gizmo_ring_segments = 48U;

struct GizmoGeometry final {
    bool visible{};
    gui::UiPoint origin{};
    scene::Vec3 world_origin{};
    std::array<gui::UiPoint, 3U> ends{};
    std::array<float, 3U> canvas_dx{};
    std::array<float, 3U> canvas_dy{};
    std::array<scene::Vec3, 3U> world_axes{};
    float world_length{};
    // Rotate rings: one circle per axis in the plane it is normal to, and
    // whether each point faces the camera. Only the near half is a handle,
    // as in Blender - the far half sits behind the element.
    bool rings{};
    std::array<std::array<gui::UiPoint, gizmo_ring_segments + 1U>, 3U>
        ring_points{};
    std::array<std::array<bool, gizmo_ring_segments + 1U>, 3U> ring_front{};
};

[[nodiscard]] std::array<scene::Vec3, 3U> world_axes_of(
    const scene::Mat4& world) noexcept {
    static constexpr std::array<scene::Vec3, 3U> fallback_axes{
        scene::Vec3{1.0F, 0.0F, 0.0F}, scene::Vec3{0.0F, 1.0F, 0.0F},
        scene::Vec3{0.0F, 0.0F, 1.0F}};
    std::array<scene::Vec3, 3U> axes{};
    for (std::size_t index = 0U; index < axes.size(); ++index) {
        axes[index] = scene::normalize(
            scene::Vec3{world.at(0U, static_cast<core::u32>(index)),
                        world.at(1U, static_cast<core::u32>(index)),
                        world.at(2U, static_cast<core::u32>(index))},
            fallback_axes[index]);
    }
    return axes;
}

constexpr std::array<scene::Vec3, 3U> world_basis{
    scene::Vec3{1.0F, 0.0F, 0.0F}, scene::Vec3{0.0F, 1.0F, 0.0F},
    scene::Vec3{0.0F, 0.0F, 1.0F}};

[[nodiscard]] GizmoGeometry gizmo_geometry(
    const gui::Rect& image, const scene::Scene& view,
    scene::Vec3 origin_world, const std::array<scene::Vec3, 3U>& axes,
    bool rings = false) noexcept {
    GizmoGeometry result{};
    if (view.canvas.height == 0U || image.height <= 0.0F) {
        return result;
    }
    const scene::Vec3 forward =
        scene::normalize(view.camera.target - view.camera.eye);
    result.world_origin = origin_world;
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
    result.world_axes = axes;
    for (std::size_t index = 0U; index < result.world_axes.size(); ++index) {
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
    if (rings) {
        const float radius = result.world_length * 0.92F;
        for (std::size_t index = 0U; index < 3U; ++index) {
            const scene::Vec3 u = result.world_axes[(index + 1U) % 3U];
            const scene::Vec3 v = result.world_axes[(index + 2U) % 3U];
            for (std::size_t step = 0U; step <= gizmo_ring_segments; ++step) {
                const float turns = static_cast<float>(step) /
                                    static_cast<float>(gizmo_ring_segments);
                const scene::Vec3 offset =
                    (u * scene::deterministic::cos_turns(turns) +
                     v * scene::deterministic::sin_turns(turns)) *
                    radius;
                const auto point = scene::project_to_canvas(
                    view.camera, view.canvas, origin_world + offset);
                if (!point.visible) {
                    return GizmoGeometry{};
                }
                result.ring_points[index][step] =
                    to_ui(image, view.canvas, point);
                result.ring_front[index][step] =
                    scene::dot(offset, forward) <= radius * 0.02F;
            }
        }
        result.rings = true;
    }
    result.visible = true;
    return result;
}

[[nodiscard]] GizmoGeometry gizmo_geometry(const gui::Rect& image,
                                           const scene::Scene& view,
                                           scene::Vec3 world) noexcept {
    return gizmo_geometry(image, view, world, world_basis);
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

// The outer white ring Blender's rotate and scale gizmos share: rotation about
// the view axis, or uniform scale. Fixed in pixels, outside the axis handles.
[[nodiscard]] bool hit_view_ring(const GizmoGeometry& gizmo, float pointer_x,
                                 float pointer_y) noexcept {
    if (!gizmo.visible) {
        return false;
    }
    const float dx = pointer_x - gizmo.origin.x;
    const float dy = pointer_y - gizmo.origin.y;
    return std::abs(std::sqrt(dx * dx + dy * dy) - view_ring_radius_pixels) <=
           view_ring_hit_pixels;
}

[[nodiscard]] GizmoAxis hit_rotate_gizmo(const GizmoGeometry& gizmo,
                                         float pointer_x,
                                         float pointer_y) noexcept {
    if (!gizmo.visible || !gizmo.rings) {
        return GizmoAxis::None;
    }
    GizmoAxis best = GizmoAxis::None;
    float best_distance = gizmo_hit_pixels;
    for (std::size_t index = 0U; index < 3U; ++index) {
        for (std::size_t step = 0U; step < gizmo_ring_segments; ++step) {
            if (!gizmo.ring_front[index][step] ||
                !gizmo.ring_front[index][step + 1U]) {
                continue;
            }
            const float distance = distance_to_full_segment(
                pointer_x, pointer_y, gizmo.ring_points[index][step],
                gizmo.ring_points[index][step + 1U]);
            if (distance < best_distance) {
                best_distance = distance;
                best = static_cast<GizmoAxis>(
                    static_cast<core::u8>(GizmoAxis::X) +
                    static_cast<core::u8>(index));
            }
        }
    }
    if (best == GizmoAxis::None && hit_view_ring(gizmo, pointer_x, pointer_y)) {
        best = GizmoAxis::View;
    }
    return best;
}

[[nodiscard]] GizmoAxis hit_scale_gizmo(const GizmoGeometry& gizmo,
                                        float pointer_x,
                                        float pointer_y) noexcept {
    const GizmoAxis axis = hit_gizmo(gizmo, pointer_x, pointer_y);
    if (axis != GizmoAxis::None) {
        return axis;
    }
    return hit_view_ring(gizmo, pointer_x, pointer_y) ? GizmoAxis::View
                                                      : GizmoAxis::None;
}

void draw_view_ring(gui::UiDrawList& list, const GizmoGeometry& gizmo,
                    bool emphasized) {
    const gui::UiColor color = emphasized ? gizmo_hot : gizmo_view_color;
    gui::UiPoint previous{gizmo.origin.x + view_ring_radius_pixels,
                          gizmo.origin.y};
    for (std::size_t step = 1U; step <= gizmo_ring_segments; ++step) {
        const float turns = static_cast<float>(step) /
                            static_cast<float>(gizmo_ring_segments);
        const gui::UiPoint next{
            gizmo.origin.x + scene::deterministic::cos_turns(turns) *
                                 view_ring_radius_pixels,
            gizmo.origin.y + scene::deterministic::sin_turns(turns) *
                                 view_ring_radius_pixels};
        line(list, previous, next, 4.0F, outline_shadow);
        line(list, previous, next, emphasized ? 2.6F : 1.6F, color);
        previous = next;
    }
}

void draw_rotate_gizmo(gui::UiDrawList& list, const GizmoGeometry& gizmo,
                       GizmoAxis hovered, GizmoAxis active) {
    if (!gizmo.visible || !gizmo.rings) {
        return;
    }
    static constexpr std::array<gui::UiColor, 3U> colors{axis_x, axis_y,
                                                         axis_z};
    for (std::size_t index = 0U; index < 3U; ++index) {
        const GizmoAxis axis = static_cast<GizmoAxis>(
            static_cast<core::u8>(GizmoAxis::X) + static_cast<core::u8>(index));
        // A drag on one ring hides the others, so the one being turned reads.
        if (active != GizmoAxis::None && active != axis) {
            continue;
        }
        const bool emphasized = hovered == axis || active == axis;
        const gui::UiColor color = emphasized ? gizmo_hot : colors[index];
        const gui::UiColor faded{color.r, color.g, color.b, 0.28F};
        for (std::size_t step = 0U; step < gizmo_ring_segments; ++step) {
            const gui::UiPoint a = gizmo.ring_points[index][step];
            const gui::UiPoint b = gizmo.ring_points[index][step + 1U];
            if (gizmo.ring_front[index][step] &&
                gizmo.ring_front[index][step + 1U]) {
                line(list, a, b, 4.6F, outline_shadow);
                line(list, a, b, emphasized ? 3.0F : 2.1F, color);
            } else if (active == GizmoAxis::None) {
                line(list, a, b, 1.2F, faded);
            }
        }
    }
    if (active == GizmoAxis::None || active == GizmoAxis::View) {
        draw_view_ring(list, gizmo,
                       hovered == GizmoAxis::View || active == GizmoAxis::View);
    }
    list.add_rect(gizmo.origin.x - 3.0F, gizmo.origin.y - 3.0F, 6.0F, 6.0F,
                  gui::UiColor{0.90F, 0.92F, 0.96F, 1.0F});
}

void draw_scale_gizmo(gui::UiDrawList& list, const GizmoGeometry& gizmo,
                      GizmoAxis hovered, GizmoAxis active) {
    if (!gizmo.visible) {
        return;
    }
    static constexpr std::array<gui::UiColor, 3U> colors{axis_x, axis_y,
                                                         axis_z};
    for (std::size_t index = 0U; index < gizmo.ends.size(); ++index) {
        const GizmoAxis axis = static_cast<GizmoAxis>(
            static_cast<core::u8>(GizmoAxis::X) + static_cast<core::u8>(index));
        const bool emphasized = hovered == axis || active == axis;
        const gui::UiColor color = emphasized ? gizmo_hot : colors[index];
        line(list, gizmo.origin, gizmo.ends[index], 5.0F, outline_shadow);
        line(list, gizmo.origin, gizmo.ends[index], emphasized ? 3.2F : 2.2F,
             color);
        const gui::UiPoint end = gizmo.ends[index];
        list.add_rect(end.x - 5.5F, end.y - 5.5F, 11.0F, 11.0F,
                      outline_shadow);
        list.add_rect(end.x - 4.0F, end.y - 4.0F, 8.0F, 8.0F, color);
    }
    draw_view_ring(list, gizmo,
                   hovered == GizmoAxis::View || active == GizmoAxis::View);
    list.add_rect(gizmo.origin.x - 3.0F, gizmo.origin.y - 3.0F, 6.0F, 6.0F,
                  gui::UiColor{0.90F, 0.92F, 0.96F, 1.0F});
}

[[nodiscard]] bool is_xyz(GizmoAxis axis) noexcept {
    return axis == GizmoAxis::X || axis == GizmoAxis::Y ||
           axis == GizmoAxis::Z;
}

[[nodiscard]] float component_of(scene::Vec3 v, std::size_t index) noexcept {
    return index == 0U ? v.x : (index == 1U ? v.y : v.z);
}

[[nodiscard]] scene::Vec3 with_component(scene::Vec3 v, std::size_t index,
                                         float value) noexcept {
    if (index == 0U) {
        v.x = value;
    } else if (index == 1U) {
        v.y = value;
    } else {
        v.z = value;
    }
    return v;
}

[[nodiscard]] const char* axis_name(std::size_t index) noexcept {
    return index == 0U ? "X" : (index == 1U ? "Y" : "Z");
}

} // namespace

std::string viewport_deep_select_label(
    const scene::Scene& view, doc::NodeId node,
    std::span<const ElementInfo> elements) {
    const auto element = std::find_if(
        elements.begin(), elements.end(),
        [node](const ElementInfo& candidate) { return candidate.node == node; });
    return element != elements.end() ? element_display_name(*element)
                                     : deep_select_label(view, node);
}

std::optional<WorldBounds> ViewportPanel::world_bounds(
    const scene::Scene& scene, std::span<const doc::NodeId> nodes) {
    std::optional<WorldBounds> box;
    const auto include = [&](scene::Vec3 point) {
        if (!box.has_value()) {
            box = WorldBounds{point, point};
            return;
        }
        box->minimum = scene::Vec3{std::min(box->minimum.x, point.x),
                                   std::min(box->minimum.y, point.y),
                                   std::min(box->minimum.z, point.z)};
        box->maximum = scene::Vec3{std::max(box->maximum.x, point.x),
                                   std::max(box->maximum.y, point.y),
                                   std::max(box->maximum.z, point.z)};
    };
    const auto include_box = [&](const scene::Mat4& world,
                                 const scene::ModelLocalBounds& local) {
        for (std::size_t corner = 0U; corner < 8U; ++corner) {
            include(scene::transform_point(
                world,
                scene::Vec3{(corner & 1U) != 0U ? local.maximum.x : local.minimum.x,
                            (corner & 2U) != 0U ? local.maximum.y : local.minimum.y,
                            (corner & 4U) != 0U ? local.maximum.z : local.minimum.z}));
        }
    };
    for (const doc::NodeId node : nodes) {
        bool found = false;
        for (const auto& object : scene.instances) {
            if (object.source == node) {
                include_box(object.world, scene::ModelLocalBounds{});
                found = true;
            }
        }
        for (const auto& clip : scene.media) {
            if (clip.source_node == node) {
                include_box(clip.world, scene::ModelLocalBounds{});
                found = true;
            }
        }
        for (const auto& model : scene.models) {
            if (model.source_node == node) {
                include_box(model.world, model_bounds_for(model).value_or(
                                             scene::ModelLocalBounds{}));
                found = true;
            }
        }
        if (!found) {
            if (const auto world = scene::world_for(scene, node)) {
                include(scene::translation_of(*world));
            }
        }
    }
    return box;
}

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

std::optional<scene::SurfaceHit>
ViewportPanel::surface_at(const scene::Scene& scene, float canvas_x,
                          float canvas_y) {
    const auto ray =
        scene::canvas_ray(scene.camera, scene.canvas, canvas_x, canvas_y);
    if (!ray.has_value()) {
        return std::nullopt;
    }
    const scene::ModelRaycaster models =
        [this](const scene::ModelInstance& model, const scene::Ray& local,
               float limit) -> std::optional<scene::RayHit> {
        const std::optional<scene::ModelLocalBounds> bounds =
            model_bounds_for(model);
        const scene::ModelLocalBounds box =
            bounds.value_or(scene::ModelLocalBounds{});
        const auto outer =
            scene::raycast_box(local, box.minimum, box.maximum, limit);
        if (!outer.has_value() || !bounds.has_value()) {
            return outer;
        }
        // Inside its bounds the model's own triangles decide, once their
        // hierarchy is ready; until then the bounds stand in for them.
        const render::ModelCacheObservation observation =
            model_cache_.observe(model.source, model_publication_fence_);
        if (observation.ready()) {
            if (const auto bvh = surface_cache_.observe(observation.asset)) {
                return scene::raycast_mesh(*bvh, observation.asset->mesh,
                                           local, limit);
            }
        }
        return outer;
    };
    return scene::raycast_scene(scene, *ray, models, scene.camera.far_plane);
}

std::optional<scene::ModelLocalBounds>
ViewportPanel::model_source_bounds(const std::string& source) {
    scene::ModelInstance probe{};
    probe.source = source;
    return model_bounds_for(probe);
}

void ViewportPanel::draw_placement_preview(
    gui::UiContext& ctx, const gui::Rect& image, const scene::Scene& scene,
    std::span<const StagePlacement> placements, std::string_view caption) {
    if (placements.empty()) {
        return;
    }
    const gui::UiColor accent = ctx.theme().accent;
    gui::UiDrawList& list = ctx.draw_list();
    ctx.push_clip(image);
    for (const StagePlacement& placement : placements) {
        const scene::ModelLocalBounds& box = placement.bounds;
        if (!(box.minimum == box.maximum)) {
            wire_box(list, image, scene, placement_world(placement), accent,
                     box);
        }
        // A square on the surface where it rests, and the surface's normal.
        const scene::Vec3 n = placement.normal;
        const scene::Vec3 across = scene::normalize(
            scene::cross(n, std::abs(n.y) < 0.9F ? scene::Vec3{0.0F, 1.0F, 0.0F}
                                                 : scene::Vec3{1.0F, 0.0F, 0.0F}));
        const scene::Vec3 along = scene::cross(n, across);
        constexpr float half = 0.2F;
        const std::array<scene::Vec3, 4U> corners{
            placement.contact + across * half + along * half,
            placement.contact - across * half + along * half,
            placement.contact - across * half - along * half,
            placement.contact + across * half - along * half};
        for (std::size_t index = 0U; index < corners.size(); ++index) {
            projected_line(list, image, scene, corners[index],
                           corners[(index + 1U) % corners.size()], 4.0F,
                           outline_shadow);
            projected_line(list, image, scene, corners[index],
                           corners[(index + 1U) % corners.size()], 1.8F,
                           accent);
        }
        projected_line(list, image, scene, placement.contact,
                       placement.contact + n * 0.35F, 1.8F, accent);
    }
    const auto anchor = scene::project_to_canvas(scene.camera, scene.canvas,
                                                 placements.front().contact);
    if (!caption.empty() && anchor.visible) {
        // Left of the contact: the Library's drag thumbnail rides below and
        // to the right of the pointer.
        const gui::UiPoint at = to_ui(image, scene.canvas, anchor);
        const float width = ctx.font_width(caption) + 16.0F;
        const gui::Rect badge{at.x - 18.0F - width, at.y - 11.0F, width, 22.0F};
        ctx.panel(badge, accent);
        ctx.label(gui::Rect{badge.x + 8.0F, badge.y, badge.width - 8.0F,
                            badge.height},
                  caption, ctx.theme().background);
    }
    ctx.pop_clip();
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
    surface_cache_.request_reset();
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

namespace {

// The projective map taking the unit square onto the source quad: canvas
// (x, y) = (r0.p, r1.p) / (r2.p) for p = (u, v, 1). A planar surface seen
// through a pinhole camera lands on screen exactly this way, and it is how the
// renderer traces a pixel back onto the layer, so the outline drawn here and
// the coverage rendered there follow the same surface. Interpolating the four
// corners bilinearly instead bends straight mask edges and drifts away from
// the rendered mask whenever the layer is seen in perspective.
using QuadProjective = std::array<double, 9U>;

[[nodiscard]] std::optional<QuadProjective> square_to_quad(
    const MaskViewportSourceMapping& mapping) noexcept {
    const auto& q = mapping.corners;
    for (const MaskViewportCanvasPoint& corner : q) {
        if (!std::isfinite(corner.x) || !std::isfinite(corner.y)) {
            return std::nullopt;
        }
    }
    const float area_twice =
        q[0U].x * q[1U].y - q[1U].x * q[0U].y +
        q[1U].x * q[2U].y - q[2U].x * q[1U].y +
        q[2U].x * q[3U].y - q[3U].x * q[2U].y +
        q[3U].x * q[0U].y - q[0U].x * q[3U].y;
    if (!std::isfinite(area_twice) || std::abs(area_twice) < 1.0e-4F) {
        return std::nullopt;
    }
    const double x0 = q[0U].x, y0 = q[0U].y, x1 = q[1U].x, y1 = q[1U].y;
    const double x2 = q[2U].x, y2 = q[2U].y, x3 = q[3U].x, y3 = q[3U].y;
    const double sx = x0 - x1 + x2 - x3;
    const double sy = y0 - y1 + y2 - y3;
    const double dx1 = x1 - x2, dx2 = x3 - x2;
    const double dy1 = y1 - y2, dy2 = y3 - y2;
    const double denominator = dx1 * dy2 - dx2 * dy1;
    if (!std::isfinite(denominator) || std::abs(denominator) < 1.0e-12) {
        return std::nullopt;
    }
    const double g = (sx * dy2 - dx2 * sy) / denominator;
    const double h = (dx1 * sy - sx * dy1) / denominator;
    const QuadProjective result{
        x1 - x0 + g * x1, x3 - x0 + h * x3, x0,
        y1 - y0 + g * y1, y3 - y0 + h * y3, y0,
        g, h, 1.0};
    for (const double value : result) {
        if (!std::isfinite(value)) {
            return std::nullopt;
        }
    }
    return result;
}

} // namespace

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
        const auto map = square_to_quad(*source_mapping);
        if (!map.has_value()) {
            return std::nullopt;
        }
        const auto& m = *map;
        const double u = x;
        const double v = y;
        const double depth = m[6U] * u + m[7U] * v + m[8U];
        // Beyond the plane's horizon: no point on the surface lands there.
        if (!(depth > 1.0e-9)) {
            return std::nullopt;
        }
        canvas_x = static_cast<float>((m[0U] * u + m[1U] * v + m[2U]) / depth);
        canvas_y = static_cast<float>((m[3U] * u + m[4U] * v + m[5U]) / depth);
        if (!std::isfinite(canvas_x) || !std::isfinite(canvas_y)) {
            return std::nullopt;
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
    if (source_mapping == nullptr) {
        return std::nullopt;
    }
    const auto map = square_to_quad(*source_mapping);
    if (!map.has_value()) {
        return std::nullopt;
    }
    // The adjugate inverts the map up to scale, and scale cancels in the
    // division. Which side of the horizon the pointer is on is checked by
    // mapping the answer forward again.
    const auto& m = *map;
    const QuadProjective inverse{
        m[4U] * m[8U] - m[5U] * m[7U], m[2U] * m[7U] - m[1U] * m[8U],
        m[1U] * m[5U] - m[2U] * m[4U], m[5U] * m[6U] - m[3U] * m[8U],
        m[0U] * m[8U] - m[2U] * m[6U], m[2U] * m[3U] - m[0U] * m[5U],
        m[3U] * m[7U] - m[4U] * m[6U], m[1U] * m[6U] - m[0U] * m[7U],
        m[0U] * m[4U] - m[1U] * m[3U]};
    const double px = canvas_x;
    const double py = canvas_y;
    const double w = inverse[6U] * px + inverse[7U] * py + inverse[8U];
    if (!std::isfinite(w) || std::abs(w) < 1.0e-12) {
        return std::nullopt;
    }
    const double u = (inverse[0U] * px + inverse[1U] * py + inverse[2U]) / w;
    const double v = (inverse[3U] * px + inverse[4U] * py + inverse[5U]) / w;
    if (!std::isfinite(u) || !std::isfinite(v) ||
        !(m[6U] * u + m[7U] * v + m[8U] > 1.0e-9)) {
        return std::nullopt;
    }
    return MaskViewportNormalizedPoint{static_cast<float>(u),
                                       static_cast<float>(v)};
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
    // The renderer's unit for this scene, not the corners' spacing: a mask
    // measured against Program Output's crop or warp would rotate and
    // feather differently from the one rendered.
    const scene::MaskCoordinateExtent& extent =
        input.source_layer_mapping->extent;
    return {std::max(extent.width, 1.0F), std::max(extent.height, 1.0F)};
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

void ViewportPanel::draw_resolution_control(gui::UiContext& ctx,
                                             const gui::Rect& bounds) {
    static constexpr std::array<std::string_view, 3U> options{
        "Full", "Half", "Quarter"};
    std::size_t selected = static_cast<std::size_t>(resolution_);
    if (ctx.dropdown(mix(id_, 4U), bounds, options, selected)) {
        resolution_ = static_cast<PreviewResolution>(selected);
    }
}

TransformDelta ViewportPanel::snapped(const gui::UiContext& ctx,
                                     TransformDelta delta) const noexcept {
    if (snapping_ == ctx.ctrl_held()) {
        return delta;
    }
    const bool fine = ctx.shift_held();
    switch (delta.tool) {
    case TransformTool::Move: {
        // Per component of the gesture's own frame, so an axis-constrained
        // move snaps its length and a free one lands on the grid's steps.
        const float step = fine ? move_snap_fine_step : move_snap_step;
        scene::Vec3 result{};
        for (const scene::Vec3& axis : delta.basis) {
            result = result +
                     axis * snap_to_step(scene::dot(delta.translation, axis),
                                         step);
        }
        delta.translation = result;
        break;
    }
    case TransformTool::Rotate:
        delta.turns = snap_to_step(delta.turns,
                                   fine ? rotate_snap_fine_step_turns
                                        : rotate_snap_step_turns);
        break;
    case TransformTool::Scale: {
        const float step = fine ? scale_snap_fine_step : scale_snap_step;
        delta.factors = scene::Vec3{snap_to_step(delta.factors.x, step),
                                    snap_to_step(delta.factors.y, step),
                                    snap_to_step(delta.factors.z, step)};
        break;
    }
    }
    return delta;
}

void ViewportPanel::draw_keyboard_transform(gui::UiContext& ctx,
                                            gui::UiDrawList& list,
                                            const gui::Rect& image,
                                            const scene::Scene& scene,
                                            gui::UiPoint centre,
                                            std::optional<float> reading) const {
    static constexpr std::array<gui::UiColor, 3U> colors{axis_x, axis_y,
                                                         axis_z};
    const std::array<scene::Vec3, 3U>& axes =
        keyboard_.orientation == TransformOrientation::Global
            ? world_basis
            : keyboard_.local_axes;
    // An infinite constraint line, drawn as long as both ends stay in front
    // of the camera.
    const auto constraint_line = [&](std::size_t index) {
        for (const float reach : {200.0F, 40.0F, 8.0F, 1.5F}) {
            const auto from = scene::project_to_canvas(
                scene.camera, scene.canvas,
                keyboard_.pivot - axes[index] * reach);
            const auto to = scene::project_to_canvas(
                scene.camera, scene.canvas,
                keyboard_.pivot + axes[index] * reach);
            if (from.visible && to.visible) {
                line(list, to_ui(image, scene.canvas, from),
                     to_ui(image, scene.canvas, to), 1.6F, colors[index]);
                return;
            }
        }
    };
    if (keyboard_.constraint == Constraint::Axis) {
        constraint_line(keyboard_.axis);
    } else if (keyboard_.constraint == Constraint::Plane) {
        constraint_line((keyboard_.axis + 1U) % 3U);
        constraint_line((keyboard_.axis + 2U) % 3U);
    }
    if (keyboard_.tool != TransformTool::Move) {
        line(list, centre, gui::UiPoint{keyboard_.pointer_x, keyboard_.pointer_y},
             1.0F, gizmo_view_color);
        list.add_rect(centre.x - 3.0F, centre.y - 3.0F, 6.0F, 6.0F,
                      gizmo_view_color);
    }

    const char* tool = keyboard_.tool == TransformTool::Move
                           ? "MOVE"
                           : (keyboard_.tool == TransformTool::Rotate
                                  ? "ROTATE"
                                  : "SCALE");
    std::string constraint;
    switch (keyboard_.constraint) {
    case Constraint::None:
        constraint = keyboard_.tool == TransformTool::Rotate ? "VIEW AXIS"
                     : keyboard_.tool == TransformTool::Move ? "VIEW PLANE"
                                                             : "UNIFORM";
        break;
    case Constraint::Axis:
        constraint = std::string{axis_name(keyboard_.axis)};
        break;
    case Constraint::Plane:
        constraint = std::string{axis_name((keyboard_.axis + 1U) % 3U)} +
                     axis_name((keyboard_.axis + 2U) % 3U) + " PLANE";
        break;
    }
    if (keyboard_.constraint != Constraint::None) {
        constraint += keyboard_.orientation == TransformOrientation::Global
                          ? " GLOBAL"
                          : " LOCAL";
    }
    std::string value;
    if (keyboard_.entry.active()) {
        value = keyboard_.entry.display() + "|";
    } else if (reading.has_value()) {
        std::array<char, 32U> text{};
        const char* format = keyboard_.tool == TransformTool::Move
                                 ? "%.3f m"
                                 : (keyboard_.tool == TransformTool::Rotate
                                        ? "%.1f deg"
                                        : "%.3fx");
        const int length =
            std::snprintf(text.data(), text.size(), format, *reading);
        value.assign(text.data(),
                     static_cast<std::size_t>(std::max(length, 0)));
    }
    if (snapping_ != ctx.ctrl_held()) {
        value += "  SNAP";
    }
    const std::string status = std::string{tool} + "  " + constraint + "  " +
                               value +
                               "    ENTER / CLICK  CONFIRM    ESC / RIGHT-CLICK"
                               "  CANCEL";
    const gui::Rect bar{image.x + 8.0F, image.y + 8.0F,
                        std::max(std::min(image.width - 16.0F, 640.0F), 0.0F),
                        22.0F};
    ctx.panel(bar, ctx.theme().background);
    ctx.label(gui::Rect{bar.x + 8.0F, bar.y, std::max(bar.width - 16.0F, 0.0F),
                        bar.height},
              status, ctx.theme().text);
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
                                  const ViewportProjection* projection,
                                  std::span<const ViewportPlate> plates,
                                  std::span<const ElementInfo> elements) {
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
        // Keep preview resolution accessible even in a narrow dock: the
        // transform tools go first, the display toggles only when there is
        // room for them too.
        float left = toolbar.x + 7.0F;
        if (toolbar.width >= 520.0F) {
            // A selected choice is drawn pressed; every one is a button, so a
            // click on the current tool is harmless rather than dead.
            const auto choice = [&](core::u64 salt, float width,
                                    std::string_view label, bool chosen,
                                    std::string_view hint) {
                const gui::Rect rect{left, toolbar.y + 4.0F, width, 20.0F};
                const bool clicked = ctx.button(mix(id_, salt), rect, label);
                if (chosen) {
                    ctx.panel(rect, ctx.theme().widget_active);
                    ctx.label(gui::Rect{rect.x + 7.0F, rect.y,
                                        std::max(rect.width - 7.0F, 0.0F),
                                        rect.height},
                              label, ctx.theme().text);
                }
                ctx.tooltip(rect, hint);
                left += width + 4.0F;
                return clicked;
            };
            if (choice(10U, 46.0F, "MOVE", tool_ == TransformTool::Move,
                       "Move tool. G moves the selection from the keyboard; "
                       "X, Y or Z constrains it, Shift+X/Y/Z to a plane.")) {
                tool_ = TransformTool::Move;
            }
            if (choice(11U, 56.0F, "ROTATE", tool_ == TransformTool::Rotate,
                       "Rotate tool. R rotates the selection about the view, "
                       "or about X, Y or Z once one is typed.")) {
                tool_ = TransformTool::Rotate;
            }
            if (choice(12U, 48.0F, "SCALE", tool_ == TransformTool::Scale,
                       "Scale tool. S scales the selection; X, Y or Z scales "
                       "one axis, Shift+X/Y/Z the other two.")) {
                tool_ = TransformTool::Scale;
            }
            left += 6.0F;
            if (choice(13U, 56.0F,
                       orientation_ == TransformOrientation::Global ? "GLOBAL"
                                                                    : "LOCAL",
                       false,
                       "Gizmo and axis-key orientation: world axes, or the "
                       "selected element's own. Pressing an axis key twice "
                       "uses the other one for that transform.")) {
                orientation_ = orientation_ == TransformOrientation::Global
                                   ? TransformOrientation::Local
                                   : TransformOrientation::Global;
            }
            if (choice(14U, 44.0F, "SNAP", snapping_,
                       "Snap moves to 1 m, rotation to 5 degrees and scale to "
                       "0.1 (Shift: 0.1 m, 1 degree, 0.01). Shift+Tab "
                       "toggles; hold Ctrl to invert while dragging.")) {
                snapping_ = !snapping_;
            }
            left += 6.0F;
        }
        if (toolbar.width >= 740.0F) {
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
        }
        const float quality_x = toolbar.x + std::max(64.0F, toolbar.width - 158.0F);
        {
            ctx.label(
                gui::Rect{quality_x - 58.0F, toolbar.y, 56.0F, toolbar.height},
                "PREVIEW", ctx.theme().text_muted);
            draw_resolution_control(ctx,
                gui::Rect{quality_x, toolbar.y + 4.0F, 90.0F, 20.0F});
            ctx.panel(
                gui::Rect{quality_x + 96.0F, toolbar.y + 4.0F, 54.0F, 20.0F},
                ctx.theme().widget);
            std::array<char, 24U> fps_text{};
            const int fps_length = std::snprintf(
                fps_text.data(), fps_text.size(), "%.3g FPS",
                std::max(frames_per_second, 0.0F));
            ctx.label(
                gui::Rect{quality_x + 101.0F, toolbar.y + 4.0F, 49.0F, 20.0F},
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
    // Blender's median point: what the selection moves, turns and scales
    // about, and where the gizmo sits.
    std::optional<scene::Vec3> pivot;
    if (primary_world.has_value()) {
        scene::Vec3 sum{};
        float count = 0.0F;
        for (const doc::NodeId node : selection) {
            if (scene::element_locked(scene, node)) {
                continue;
            }
            if (const auto world = scene::world_for(scene, node)) {
                sum = sum + scene::translation_of(*world);
                count += 1.0F;
            }
        }
        pivot = count > 0.0F ? sum * (1.0F / count)
                             : scene::translation_of(*primary_world);
    }
    const std::array<scene::Vec3, 3U> primary_axes =
        primary_world.has_value() ? world_axes_of(*primary_world)
                                  : world_basis;
    const scene::Vec3 view_forward =
        scene::normalize(scene.camera.target - scene.camera.eye);
    if (keyboard_.active && !pivot.has_value()) {
        // The selection went away under a keyboard transform.
        keyboard_.active = false;
        frame.transform_ended = true;
        frame.transform_cancelled = true;
    }
    // A gizmo drag keeps the tool it began with; scale always works on the
    // element's own axes (see TransformOrientation).
    const TransformTool gizmo_tool =
        dragging_ && drag_axis_ != GizmoAxis::Screen ? drag_tool_ : tool_;
    const std::array<scene::Vec3, 3U>& gizmo_axes =
        gizmo_tool == TransformTool::Scale ||
                orientation_ == TransformOrientation::Local
            ? primary_axes
            : world_basis;
    const GizmoGeometry scene_gizmo =
        role_ == ViewportRole::Stage && show_gizmo_ && !keyboard_.active &&
                selected_surface_value == nullptr && pivot.has_value()
            ? gizmo_geometry(frame.image, scene, *pivot, gizmo_axes,
                             gizmo_tool == TransformTool::Rotate)
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
    GizmoAxis hovered_scene_axis = GizmoAxis::None;
    if (!occluded) {
        switch (gizmo_tool) {
        case TransformTool::Move:
            hovered_scene_axis =
                hit_gizmo(scene_gizmo, ctx.pointer_x(), ctx.pointer_y());
            break;
        case TransformTool::Rotate:
            hovered_scene_axis = hit_rotate_gizmo(scene_gizmo, ctx.pointer_x(),
                                                  ctx.pointer_y());
            break;
        case TransformTool::Scale:
            hovered_scene_axis = hit_scale_gizmo(scene_gizmo, ctx.pointer_x(),
                                                 ctx.pointer_y());
            break;
        }
    }
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

    // The depth chooser is a context menu: drawn over every panel, floating
    // ones included, and owning the pointer and keyboard while it is open.
    const gui::UiId deep_menu = mix(id_, 0xD33DU);
    const auto offer_deep_menu = [&]() -> std::optional<std::size_t> {
        std::vector<std::string> labels;
        labels.reserve(deep_candidates_.size());
        for (const doc::NodeId candidate : deep_candidates_) {
            labels.push_back(viewport_deep_select_label(scene, candidate,
                                                        elements));
        }
        const std::vector<std::string_view> options(labels.begin(),
                                                    labels.end());
        return ctx.context_menu(deep_menu, options);
    };
    if (deep_select_open_) {
        // A choice lands on the frame the menu closes, so it is read before
        // the closed menu is taken as dismissed.
        if (const auto chosen = offer_deep_menu()) {
            if (*chosen < deep_candidates_.size()) {
                frame.pressed = true;
                frame.picked = deep_candidates_[*chosen];
            }
            deep_select_open_ = false;
            deep_candidates_.clear();
        } else if (!ctx.menu_open(deep_menu)) {
            deep_select_open_ = false;
            deep_candidates_.clear();
        }
    }

    // --- Keyboard transforms, as in Blender: G, R or S over the Stage moves,
    // rotates or scales the selection with the pointer - no button held - until
    // Enter or a left click confirms it, or Escape or a right click puts it
    // back. X, Y or Z constrains to an axis (twice: the other orientation,
    // three times: free), Shift+X/Y/Z to the plane without it, and a typed
    // number gives the exact distance, angle in degrees, or factor.
    const bool pointer_over_image =
        !occluded && frame.image.contains(ctx.pointer_x(), ctx.pointer_y());
    if (swallow_pointer_ && !ctx.pointer_down() &&
        !ctx.secondary_pointer_down()) {
        swallow_pointer_ = false;
    }
    if (role_ == ViewportRole::Stage && pointer_over_image && !ctx.typing() &&
        ctx.shift_held() && ctx.key_pressed(gui::Key::Tab)) {
        snapping_ = !snapping_;
    }
    if (role_ == ViewportRole::Stage && !keyboard_.active &&
        !swallow_pointer_ && !dragging_ && !surface_dragging_ &&
        !deep_select_open_ && pivot.has_value() && pointer_over_image &&
        !ctx.pointer_down() && !ctx.secondary_pointer_down() &&
        !ctx.middle_pointer_down() && !ctx.alt_held() && !ctx.ctrl_held()) {
        std::optional<TransformTool> started;
        for (const char typed : ctx.shortcut_text()) {
            if (typed == 'g') {
                started = TransformTool::Move;
            } else if (typed == 'r') {
                started = TransformTool::Rotate;
            } else if (typed == 's') {
                started = TransformTool::Scale;
            }
        }
        if (started.has_value()) {
            keyboard_ = KeyboardTransform{};
            keyboard_.active = true;
            keyboard_.tool = *started;
            keyboard_.orientation = orientation_;
            keyboard_.pivot = *pivot;
            keyboard_.local_axes = primary_axes;
            keyboard_.press_x = keyboard_.pointer_x = keyboard_.last_x =
                ctx.pointer_x();
            keyboard_.press_y = keyboard_.pointer_y = keyboard_.last_y =
                ctx.pointer_y();
            tool_ = *started;
            frame.transform_began = true;
        }
    }

    gui::UiPoint keyboard_centre{frame.image.x + frame.image.width * 0.5F,
                                 frame.image.y + frame.image.height * 0.5F};
    std::optional<float> keyboard_reading;
    if (keyboard_.active) {
        if (const auto centre = scene::project_to_canvas(
                scene.camera, scene.canvas, keyboard_.pivot);
            centre.visible) {
            keyboard_centre = to_ui(frame.image, scene.canvas, centre);
        }
        if (!frame.transform_began) {
            for (const char typed : ctx.claimed_text()) {
                const char lower = typed >= 'A' && typed <= 'Z'
                                       ? static_cast<char>(typed - 'A' + 'a')
                                       : typed;
                if (lower == 'x' || lower == 'y' || lower == 'z') {
                    const std::size_t axis =
                        static_cast<std::size_t>(lower - 'x');
                    const Constraint wanted =
                        typed == lower ? Constraint::Axis : Constraint::Plane;
                    if (keyboard_.constraint != wanted ||
                        keyboard_.axis != axis) {
                        keyboard_.constraint = wanted;
                        keyboard_.axis = axis;
                        keyboard_.orientation = orientation_;
                    } else if (keyboard_.orientation == orientation_) {
                        keyboard_.orientation =
                            orientation_ == TransformOrientation::Global
                                ? TransformOrientation::Local
                                : TransformOrientation::Global;
                    } else {
                        keyboard_.constraint = Constraint::None;
                        keyboard_.orientation = orientation_;
                    }
                } else {
                    static_cast<void>(
                        keyboard_.entry.type(std::string_view{&typed, 1U}));
                }
            }
            if (ctx.key_pressed(gui::Key::Backspace)) {
                keyboard_.entry.backspace();
            }
        }
        // Shift slows the pointer to a tenth for fine work.
        const float motion_scale = ctx.shift_held() ? 0.1F : 1.0F;
        const float previous_x = keyboard_.pointer_x;
        const float previous_y = keyboard_.pointer_y;
        keyboard_.pointer_x += (ctx.pointer_x() - keyboard_.last_x) * motion_scale;
        keyboard_.pointer_y += (ctx.pointer_y() - keyboard_.last_y) * motion_scale;
        keyboard_.last_x = ctx.pointer_x();
        keyboard_.last_y = ctx.pointer_y();
        // Anticlockwise screen turns, whatever the axis: the axis decides the
        // sign when the delta is built, so changing it mid-way keeps the
        // rotation following the pointer.
        keyboard_.turns += sweep_turns(
            keyboard_centre.x, keyboard_centre.y, previous_x, previous_y,
            keyboard_.pointer_x, keyboard_.pointer_y, -view_forward,
            view_forward);

        const std::array<scene::Vec3, 3U>& axes =
            keyboard_.orientation == TransformOrientation::Global
                ? world_basis
                : keyboard_.local_axes;
        const std::size_t axis = keyboard_.axis;
        const float to_canvas_x = canvas_width / frame.image.width;
        const float to_canvas_y = canvas_height / frame.image.height;
        const float press_canvas_x =
            (keyboard_.press_x - frame.image.x) * to_canvas_x;
        const float press_canvas_y =
            (keyboard_.press_y - frame.image.y) * to_canvas_y;
        const float now_canvas_x =
            (keyboard_.pointer_x - frame.image.x) * to_canvas_x;
        const float now_canvas_y =
            (keyboard_.pointer_y - frame.image.y) * to_canvas_y;
        const float drag_x = now_canvas_x - press_canvas_x;
        const float drag_y = now_canvas_y - press_canvas_y;
        const std::optional<float> typed = keyboard_.entry.value();

        TransformDelta delta{};
        delta.tool = keyboard_.tool;
        delta.pivot = keyboard_.pivot;
        delta.basis = axes;
        switch (keyboard_.tool) {
        case TransformTool::Move:
            if (typed.has_value()) {
                const std::size_t along =
                    keyboard_.constraint == Constraint::Plane
                        ? (axis + 1U) % 3U
                        : (keyboard_.constraint == Constraint::Axis ? axis
                                                                    : 0U);
                delta.translation = axes[along] * *typed;
            } else if (keyboard_.constraint == Constraint::Axis) {
                const auto from = scene::project_to_canvas(
                    scene.camera, scene.canvas, keyboard_.pivot);
                const auto to = scene::project_to_canvas(
                    scene.camera, scene.canvas,
                    keyboard_.pivot + axes[axis]);
                const float unit_x = to.x - from.x;
                const float unit_y = to.y - from.y;
                const float unit_squared = unit_x * unit_x + unit_y * unit_y;
                if (from.visible && to.visible && unit_squared > 1.0e-4F) {
                    delta.translation =
                        axes[axis] *
                        ((drag_x * unit_x + drag_y * unit_y) / unit_squared);
                }
            } else {
                const scene::Vec3 free = scene::screen_drag_to_translation(
                    scene.camera, scene.canvas, keyboard_.pivot, drag_x,
                    drag_y);
                if (keyboard_.constraint == Constraint::Plane) {
                    delta.translation =
                        plane_drag_translation(scene.camera, scene.canvas,
                                               keyboard_.pivot, axes[axis],
                                               press_canvas_x, press_canvas_y,
                                               now_canvas_x, now_canvas_y)
                            .value_or(free -
                                      axes[axis] *
                                          scene::dot(free, axes[axis]));
                } else {
                    delta.translation = free;
                }
            }
            break;
        case TransformTool::Rotate:
            delta.axis = keyboard_.constraint == Constraint::None
                             ? -view_forward
                             : axes[axis];
            delta.turns =
                typed.has_value()
                    ? *typed / 360.0F
                    : keyboard_.turns *
                          (scene::dot(delta.axis, view_forward) < 0.0F ? 1.0F
                                                                       : -1.0F);
            break;
        case TransformTool::Scale: {
            const float press_dx = keyboard_.press_x - keyboard_centre.x;
            const float press_dy = keyboard_.press_y - keyboard_centre.y;
            const float now_dx = keyboard_.pointer_x - keyboard_centre.x;
            const float now_dy = keyboard_.pointer_y - keyboard_centre.y;
            const float reach =
                std::sqrt(press_dx * press_dx + press_dy * press_dy);
            float factor =
                reach > 1.0F
                    ? std::sqrt(now_dx * now_dx + now_dy * now_dy) / reach
                    : 1.0F;
            if (typed.has_value()) {
                factor = *typed;
            }
            switch (keyboard_.constraint) {
            case Constraint::None:
                delta.factors = scene::Vec3{factor, factor, factor};
                break;
            case Constraint::Axis:
                delta.factors =
                    with_component(scene::Vec3{1.0F, 1.0F, 1.0F}, axis, factor);
                break;
            case Constraint::Plane:
                delta.factors = with_component(
                    scene::Vec3{factor, factor, factor}, axis, 1.0F);
                break;
            }
            break;
        }
        }
        if (!typed.has_value()) {
            delta = snapped(ctx, delta);
        }

        const bool confirm =
            ctx.key_pressed(gui::Key::Enter) || ctx.pointer_pressed();
        const bool cancel = ctx.key_pressed(gui::Key::Escape) ||
                            ctx.secondary_pointer_pressed();
        if (cancel) {
            TransformDelta restore{};
            restore.tool = keyboard_.tool;
            restore.pivot = keyboard_.pivot;
            delta = restore;
            frame.transform_cancelled = true;
        }
        if (cancel || confirm) {
            keyboard_.active = false;
            frame.transform_ended = true;
            swallow_pointer_ =
                ctx.pointer_down() || ctx.secondary_pointer_down();
        }
        frame.transform = delta;
        switch (delta.tool) {
        case TransformTool::Move:
            keyboard_reading = scene::length(delta.translation);
            if (keyboard_.constraint == Constraint::Axis) {
                keyboard_reading = scene::dot(delta.translation, axes[axis]);
            }
            break;
        case TransformTool::Rotate:
            keyboard_reading = delta.turns * 360.0F;
            break;
        case TransformTool::Scale:
            keyboard_reading = keyboard_.constraint == Constraint::Plane
                                   ? component_of(delta.factors,
                                                  (axis + 1U) % 3U)
                                   : component_of(delta.factors,
                                                  keyboard_.constraint ==
                                                          Constraint::Axis
                                                      ? axis
                                                      : 0U);
            break;
        }
    }
    frame.keyboard_transform = keyboard_.active || swallow_pointer_;

    const gui::Rect interaction_bounds =
        occluded || frame.keyboard_transform ? gui::Rect{} : frame.image;
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
        [&](const std::vector<scene::ScenePick>& hits,
            bool plain_pick_available) {
            deep_candidates_.reserve(hits.size());
            for (const scene::ScenePick& hit : hits) {
                deep_candidates_.push_back(hit.node);
            }
            if (hits.size() > 1U ||
                (!plain_pick_available && !hits.empty())) {
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

        // On Stage, Alt+left orbits and left added to a right or middle drag
        // pans: camera chords that never pick, or drag, what is underneath.
        const bool camera_chord =
            role_ == ViewportRole::Stage &&
            (ctx.alt_held() || ctx.secondary_pointer_down() ||
             ctx.middle_pointer_down());
        if (camera_chord) {
            // Left to the shell's camera navigation.
        } else if (hovered_surface_axis != GizmoAxis::None &&
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
                   pivot.has_value()) {
            frame.picked = primary;
            drag_axis_ = hovered_scene_axis;
            drag_tool_ = gizmo_tool;
            press_world_ = *pivot;
            drag_pivot_ = *pivot;
            drag_basis_ = scene_gizmo.world_axes;
            drag_centre_x_ = scene_gizmo.origin.x;
            drag_centre_y_ = scene_gizmo.origin.y;
            drag_last_x_ = ctx.pointer_x();
            drag_last_y_ = ctx.pointer_y();
            drag_turns_ = 0.0F;
            axis_world_ = -view_forward;
            if (is_xyz(hovered_scene_axis)) {
                const std::size_t index = axis_index(hovered_scene_axis);
                axis_canvas_x_ = scene_gizmo.canvas_dx[index];
                axis_canvas_y_ = scene_gizmo.canvas_dy[index];
                axis_world_length_ = scene_gizmo.world_length;
                axis_world_ = scene_gizmo.world_axes[index];
            }
            // A scale handle follows the pointer: the factor is how far along
            // the handle (or out from the centre, for the ring) it now is,
            // over how far it was when pressed.
            const float from_x = ctx.pointer_x() - scene_gizmo.origin.x;
            const float from_y = ctx.pointer_y() - scene_gizmo.origin.y;
            drag_press_reach_ = std::sqrt(from_x * from_x + from_y * from_y);
            if (is_xyz(hovered_scene_axis)) {
                const std::size_t index = axis_index(hovered_scene_axis);
                const float handle_x =
                    scene_gizmo.ends[index].x - scene_gizmo.origin.x;
                const float handle_y =
                    scene_gizmo.ends[index].y - scene_gizmo.origin.y;
                const float handle =
                    std::sqrt(handle_x * handle_x + handle_y * handle_y);
                drag_press_reach_ =
                    handle > 0.0F
                        ? (from_x * handle_x + from_y * handle_y) / handle
                        : 0.0F;
            }
            drag_press_reach_ = std::max(drag_press_reach_, 1.0F);
            press_x_ = frame.pointer.x;
            press_y_ = frame.pointer.y;
            dragging_ = true;
            // The gizmo is drawn over the selected element. It owns a moving
            // press, but a stationary hold must still be able to inspect the
            // stack beneath it; otherwise selecting the front item makes the
            // depth chooser unreachable on the next press.
            prepare_deep_select(scene_hits_under_pointer(), true);
        } else {
            // A plain press takes the nearest element wholly in front of the
            // camera; one the camera is partly inside (a floor, a wall, a
            // room) leaves the press to camera navigation. Holding still still
            // offers every element under the pointer, those included.
            const std::vector<scene::ScenePick> hits =
                scene_hits_under_pointer();
            const auto front = std::find_if(
                hits.begin(), hits.end(), [](const scene::ScenePick& hit) {
                    return !hit.crosses_camera;
                });
            // A crossing-only hit leaves a plain press to camera navigation,
            // but holding that press must still offer the hit in the chooser.
            prepare_deep_select(hits, front != hits.end());
            if (front != hits.end()) {
                frame.picked = front->node;
                const auto world = scene::world_for(scene, front->node);
                if (world.has_value()) {
                    press_world_ = scene::translation_of(*world);
                    press_x_ = frame.pointer.x;
                    press_y_ = frame.pointer.y;
                    drag_axis_ = GizmoAxis::Screen;
                    drag_tool_ = TransformTool::Move;
                    drag_pivot_ = press_world_;
                    drag_basis_ = orientation_ == TransformOrientation::Local
                                      ? world_axes_of(*world)
                                      : world_basis;
                    dragging_ = true;
                }
            }
        }
        if (!frame.picked.has_value() && !frame.surface_picked.has_value()) {
            frame.empty_canvas =
                role_ == ViewportRole::Stage
                    ? EmptyCanvasInteraction::Navigate
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
            ctx.open_context_menu(deep_menu, deep_press_pointer_x_ + 12.0F,
                                  deep_press_pointer_y_ + 12.0F);
            static_cast<void>(offer_deep_menu());
            dragging_ = false;
            drag_axis_ = GizmoAxis::None;
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
            TransformDelta delta{};
            delta.tool = drag_tool_;
            delta.pivot = drag_pivot_;
            delta.basis = drag_basis_;
            switch (drag_tool_) {
            case TransformTool::Move:
                if (drag_axis_ == GizmoAxis::Screen) {
                    delta.translation =
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
                        delta.translation =
                            axis_world_ * (along * axis_world_length_);
                    }
                }
                break;
            case TransformTool::Rotate:
                drag_turns_ += sweep_turns(drag_centre_x_, drag_centre_y_,
                                           drag_last_x_, drag_last_y_,
                                           ctx.pointer_x(), ctx.pointer_y(),
                                           axis_world_, view_forward);
                drag_last_x_ = ctx.pointer_x();
                drag_last_y_ = ctx.pointer_y();
                delta.axis = axis_world_;
                delta.turns = drag_turns_;
                break;
            case TransformTool::Scale: {
                const float now_x = ctx.pointer_x() - drag_centre_x_;
                const float now_y = ctx.pointer_y() - drag_centre_y_;
                float reach = std::sqrt(now_x * now_x + now_y * now_y);
                if (is_xyz(drag_axis_)) {
                    const std::size_t index = axis_index(drag_axis_);
                    const float handle_x = axis_canvas_x_;
                    const float handle_y = axis_canvas_y_;
                    const float handle =
                        std::sqrt(handle_x * handle_x + handle_y * handle_y);
                    reach = handle > 0.0F
                                ? (now_x * handle_x + now_y * handle_y) / handle
                                : drag_press_reach_;
                    delta.factors =
                        with_component(scene::Vec3{1.0F, 1.0F, 1.0F}, index,
                                       reach / drag_press_reach_);
                } else {
                    const float factor = reach / drag_press_reach_;
                    delta.factors = scene::Vec3{factor, factor, factor};
                }
                break;
            }
            }
            delta = snapped(ctx, delta);
            frame.transform = delta;
            frame.translation = delta.translation;
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
        // Every camera of the scene on Stage, so a selected one has its gizmo
        // on something visible. A hand-built scene that lists none still
        // shows the viewpoint it was given.
        for (const scene::SceneCamera& placed : scene.cameras) {
            draw_camera_overlay(
                list, frame.image, scene, placed.camera,
                std::find(selection.begin(), selection.end(), placed.node) !=
                    selection.end());
        }
        if (camera_overlay != nullptr && scene.cameras.empty()) {
            draw_camera_overlay(list, frame.image, scene, *camera_overlay);
        }
        draw_light_overlays(ctx, list, frame.image, scene, selection, primary);
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
            const GizmoAxis active =
                dragging_ && drag_axis_ != GizmoAxis::Screen ? drag_axis_
                                                             : GizmoAxis::None;
            const GizmoAxis hovered =
                dragging_ ? GizmoAxis::None : hovered_scene_axis;
            switch (gizmo_tool) {
            case TransformTool::Move:
                draw_gizmo(list, scene_gizmo, hovered, active);
                break;
            case TransformTool::Rotate:
                draw_rotate_gizmo(list, scene_gizmo, hovered, active);
                break;
            case TransformTool::Scale:
                draw_scale_gizmo(list, scene_gizmo, hovered, active);
                break;
            }
            if (scene_gizmo.visible && active != GizmoAxis::None &&
                drag_tool_ != TransformTool::Move) {
                line(list, gui::UiPoint{drag_centre_x_, drag_centre_y_},
                     gui::UiPoint{ctx.pointer_x(), ctx.pointer_y()}, 1.0F,
                     gizmo_view_color);
            }
        }
    }
    if (keyboard_.active) {
        draw_keyboard_transform(ctx, list, frame.image, scene, keyboard_centre,
                                keyboard_reading);
    }
    ctx.pop_clip();

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
    keyboard_.active = false;
    swallow_pointer_ = false;
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
