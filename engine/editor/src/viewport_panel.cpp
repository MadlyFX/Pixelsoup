#include <mediaserver/editor/viewport_panel.hpp>
#include <mediaserver/editor/gizmo.hpp>
#include <mediaserver/editor/light_guides.hpp>
#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/rect_cut.hpp>
#include <mediaserver/gui/shapes.hpp>

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
constexpr float surface_hit_pixels = 8.0F;
constexpr float surface_yaw_radius_pixels = 49.0F;
constexpr float surface_yaw_hit_pixels = 7.0F;
constexpr std::size_t maximum_imported_wire_triangles = 4096U;
constexpr std::size_t maximum_cached_model_bounds = 128U;
constexpr std::size_t maximum_cached_projection_surfaces = 256U;
constexpr double deep_select_hold_seconds = 0.45;
constexpr float deep_select_motion_pixels = 4.0F;

constexpr gui::UiColor outline_secondary{0.18F, 0.72F, 0.92F, 0.92F};
constexpr gui::UiColor outline_primary{0.20F, 0.92F, 1.0F, 1.0F};
constexpr gui::UiColor light_color{1.0F, 0.78F, 0.24F, 0.92F};
constexpr gui::UiColor surface_color{0.16F, 0.68F, 0.88F, 0.68F};
constexpr gui::UiColor surface_selected{0.20F, 0.92F, 1.0F, 1.0F};
constexpr gui::UiColor surface_fallback{1.0F, 0.62F, 0.18F, 0.92F};
// Track screens, in the hue the timeline and Inspector mark a track with.
constexpr gui::UiColor screen_color{0.906F, 0.596F, 0.400F, 0.55F};
constexpr gui::UiColor screen_selected{0.980F, 0.706F, 0.470F, 1.0F};
constexpr gui::UiColor screen_muted{0.549F, 0.529F, 0.510F, 0.45F};
constexpr gui::UiColor surface_yaw{1.0F, 0.74F, 0.22F, 1.0F};

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

// Plane and Quad are flat: their box has no depth, so a screen measures and
// rests as the screen rather than as a cube.
[[nodiscard]] scene::ModelLocalBounds
primitive_local_bounds(scene::Primitive primitive) noexcept {
    scene::ModelLocalBounds box{};
    if (primitive == scene::Primitive::Plane) {
        box.minimum.y = box.maximum.y = 0.0F;
    } else if (primitive == scene::Primitive::Quad) {
        box.minimum.z = box.maximum.z = 0.0F;
    }
    return box;
}

void line(gui::UiDrawList& list, gui::UiPoint a, gui::UiPoint b,
          float thickness, gui::UiColor color) {
    list.add_line(a.x, a.y, b.x, b.y, thickness, color);
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
             overlay_halo);
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

// The floor grid follows the view: centred under what the camera looks at,
// spaced by `stage_grid_step` with a brighter line every ten, and the world
// X (red) and Z (blue) axes wherever they cross it - or, in a Front or Side
// view, the upright grid with Y (green). Each line is drawn in
// pieces so the part in front of the camera still shows when the camera
// stands on the grid. Returns the spacing, for the readout.
float draw_grid(gui::UiDrawList& list, const gui::Rect& image,
                const scene::Scene& view) {
    // An orthographic view's reach is the height it shows; the eye stands
    // well back from what it looks at.
    const float reach =
        scene::orthographic(view.camera)
            ? view.camera.orthographic_height
            : std::max(scene::length(view.camera.eye - view.camera.target),
                       std::abs(view.camera.eye.y));
    const float step = stage_grid_step(reach);
    const float major = step * 10.0F;
    constexpr int half_grid = 40;
    constexpr int pieces = 8;
    // The floor, unless an orthographic view looks along it: a Front or Side
    // view gets the upright grid through the origin facing it instead.
    const scene::Vec3 forward = scene::normalize(
        view.camera.target - view.camera.eye, scene::Vec3{0.0F, -1.0F, 0.0F});
    std::size_t across = 0U; // x
    std::size_t along = 2U;  // z
    if (scene::orthographic(view.camera) && std::abs(forward.y) < 0.5F) {
        along = 1U;
        across = std::abs(forward.x) > std::abs(forward.z) ? 2U : 0U;
    }
    const auto component = [](scene::Vec3 v, std::size_t axis) {
        return axis == 0U ? v.x : axis == 1U ? v.y : v.z;
    };
    const auto point = [&](float u, float v) {
        scene::Vec3 result{};
        (across == 0U ? result.x : result.z) = u;
        (along == 1U ? result.y : result.z) = v;
        return result;
    };
    const float centre_u =
        std::round(component(view.camera.target, across) / major) * major;
    const float centre_v =
        std::round(component(view.camera.target, along) / major) * major;
    const float extent = static_cast<float>(half_grid) * step;
    const gui::UiColor minor_color{0.22F, 0.28F, 0.36F, 0.18F};
    const gui::UiColor major_color{0.30F, 0.38F, 0.48F, 0.34F};
    const std::array<gui::UiColor, 3U> axis_color{
        gui::UiColor{0.72F, 0.18F, 0.18F, 0.45F},
        gui::UiColor{0.22F, 0.62F, 0.24F, 0.50F},
        gui::UiColor{0.16F, 0.34F, 0.82F, 0.50F}};
    const auto pieced = [&](scene::Vec3 from, scene::Vec3 to, float thickness,
                            gui::UiColor color) {
        for (int piece = 0; piece < pieces; ++piece) {
            const float a = static_cast<float>(piece) / pieces;
            const float b = static_cast<float>(piece + 1) / pieces;
            projected_line(list, image, view, from + (to - from) * a,
                           from + (to - from) * b, thickness, color);
        }
    };
    for (int line_index = -half_grid; line_index <= half_grid; ++line_index) {
        const float offset = static_cast<float>(line_index) * step;
        const bool is_major = line_index % 10 == 0;
        const float v = centre_v + offset;
        const float u = centre_u + offset;
        const float thickness = is_major ? 1.2F : 1.0F;
        const gui::UiColor grid = is_major ? major_color : minor_color;
        // A line of constant v runs along the `across` axis, and is that
        // axis itself where v is zero.
        pieced(point(centre_u - extent, v), point(centre_u + extent, v),
               thickness,
               std::abs(v) < step * 0.5F ? axis_color[across] : grid);
        pieced(point(u, centre_v - extent), point(u, centre_v + extent),
               thickness,
               std::abs(u) < step * 0.5F ? axis_color[along] : grid);
    }
    return step;
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
    const gui::UiPoint top_left = canvas_to_image(
        image, view.canvas, static_cast<float>(plate.x),
        static_cast<float>(plate.y));
    const gui::UiPoint bottom_right = canvas_to_image(
        image, view.canvas, static_cast<float>(plate.x + plate.width),
        static_cast<float>(plate.y + plate.height));
    const gui::Rect drawn = gui::spanning(top_left.x, top_left.y, bottom_right.x,
                                            bottom_right.y);

    const gui::UiColor plate_color{0.29F, 0.85F, 0.55F, 0.90F};
    const gui::UiColor feather_color{0.29F, 0.85F, 0.55F, 0.35F};
    gui::add_outline(list, drawn, 2.0F, plate_color);

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
    if (drawn.width <= inset_x * 2.0F || drawn.height <= inset_y * 2.0F) {
        return;
    }
    gui::add_outline(list, gui::inset(drawn, inset_x, inset_y), 1.0F,
                     feather_color);
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
            line(list, a, b, thickness + 2.0F, overlay_halo);
        }
        line(list, a, b, thickness, ink);
    }
}

void light_guide_caption(gui::UiContext& ctx, const gui::Rect& image,
                         std::string_view shape, std::string_view extent) {
    if (image.width < 160.0F || image.height < 65.0F) return;
    gui::Rect foot = gui::inset(image, 8.0F);
    gui::Rect caption = gui::cut_bottom(foot, 42.0F);
    caption.width = std::min(caption.width, 395.0F);
    ctx.panel(caption, gui::UiColor{0.015F, 0.025F, 0.04F, 0.88F});
    // Two lines, a pixel apart.
    gui::Rect lines = gui::inset(caption, gui::Padding{7.0F, 2.0F, 7.0F, 0.0F});
    ctx.label(gui::cut_top(lines, 18.0F), shape, outline_primary);
    static_cast<void>(gui::cut_top(lines, 1.0F));
    ctx.label(gui::cut_top(lines, 18.0F), extent, ctx.theme().text);
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
                             const ViewportProjection* projection = nullptr,
                             std::optional<gui::UiColor> tint = std::nullopt) {
    // One colour for every outline when tinted - the History strip's hover -
    // otherwise the selection's primary and secondary.
    const auto outline = [&](doc::NodeId id) {
        return tint.value_or(id == primary ? outline_primary : outline_secondary);
    };
    for (const auto& object : view.instances) {
        if (!selected(selection, object.source)) {
            continue;
        }
        const gui::UiColor color = outline(object.source);
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
                      outline(clip.source_node),
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
                     outline(model.source_node),
                     bounds.value_or(scene::ModelLocalBounds{}), projection);
        }
    }
    // Picking and editing address the group. Thousands of per-tree outlines can
    // exhaust the UI vertex budget and prevent the Inspector from presenting.
    for (const auto& [id,bounds] : batches)
        wire_box(list,image,view,scene::identity(),outline(id),bounds,projection);
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

// The outline of `surface` in the world. `mesh` is its generated or imported
// geometry when one is resolved. A camera-anchored stage places the authored
// stage coordinates first; a generated mesh keeps its stage-space vertices in
// the cache and is carried vertex by vertex here.
[[nodiscard]] SurfaceVisual
surface_visual(const output::ProjectionSurface& surface,
               const output::ProjectionMesh* mesh,
               const std::optional<output::StagePose>& stage) {
    output::ProjectionSurface placed = surface;
    if (stage.has_value()) {
        placed.geometry = output::place_on_stage(surface.geometry, *stage);
    }
    switch (surface.shape) {
    case output::ProjectionSurfaceShape::Planar:
        return planar_surface_visual(placed);
    case output::ProjectionSurfaceShape::HorizontalArc: {
        if (mesh == nullptr) {
            return planar_surface_visual(placed, true);
        }
        SurfaceVisual visual = mesh_surface_visual(*mesh, true);
        if (stage.has_value()) {
            for (scene::Vec3& vertex : visual.vertices) {
                vertex = output::stage_point(*stage, vertex);
            }
        }
        return visual;
    }
    case output::ProjectionSurfaceShape::ImportedMesh:
        return mesh != nullptr
                   ? mesh_surface_visual(*mesh, false, &placed.geometry,
                                         surface.mesh_scale)
                   : planar_surface_visual(placed, true);
    }
    return SurfaceVisual{};
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
        haloed_line(list, surface.points[edge[0U]], surface.points[edge[1U]],
                    selected_surface ? 4.5F : 3.5F,
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
        haloed_line(list, points[index], points[next],
                    selected_screen ? 4.0F : 3.0F,
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

void draw_yaw_ring(gui::UiDrawList& list, const gui::Theme& theme,
                   const GizmoGeometry& gizmo, bool hovered, bool active) {
    if (!gizmo.visible) {
        return;
    }
    const gui::UiColor color =
        hovered || active ? theme.gizmo_hot : surface_yaw;
    haloed_ring(list, gizmo.origin, surface_yaw_radius_pixels, 48U, 4.5F,
                active ? 2.8F : 1.8F, color);
    // An arrowhead astride the ring at its right, pointing out.
    gui::add_arrowhead(list,
                       gui::UiPoint{gizmo.origin.x + surface_yaw_radius_pixels +
                                        6.0F,
                                    gizmo.origin.y},
                       gui::UiPoint{1.0F, 0.0F}, 9.0F, 4.5F, color);
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
    const auto primitive_bounds = primitive_local_bounds;
    for (const doc::NodeId node : nodes) {
        bool found = false;
        for (const auto& object : scene.instances) {
            if (object.source == node) {
                include_box(object.world,
                            primitive_bounds(object.mesh.primitive));
                found = true;
            }
        }
        for (const auto& clip : scene.media) {
            if (clip.source_node == node) {
                include_box(clip.world, primitive_bounds(clip.mesh.primitive));
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
                          float canvas_y, const scene::SurfaceFilter& counts) {
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
            const std::shared_ptr<const scene::MeshData> mesh{
                observation.asset, &observation.asset->mesh};
            if (const auto bvh = surface_cache_->observe(mesh)) {
                return scene::raycast_mesh(*bvh, *mesh, local, limit);
            }
        }
        return outer;
    };
    return scene::raycast_scene(scene, *ray, models, scene.camera.far_plane,
                                counts);
}

std::shared_ptr<const scene::MeshData>
ViewportPanel::primitive_mesh(scene::MeshKey key) {
    key = scene::normalized(key);
    const auto found = primitive_meshes_.find(key);
    if (found != primitive_meshes_.end()) {
        return found->second;
    }
    // Distinct primitive keys in a show are few; a scene that churns through
    // tessellations starts the table again rather than growing it.
    constexpr std::size_t maximum_primitive_meshes = 64U;
    if (primitive_meshes_.size() >= maximum_primitive_meshes) {
        primitive_meshes_.clear();
    }
    auto mesh = std::make_shared<scene::MeshData>();
    if (!scene::build_mesh(key, *mesh)) {
        return nullptr;
    }
    return primitive_meshes_.emplace(key, std::move(mesh)).first->second;
}

std::optional<MeasurePoint>
ViewportPanel::measure_point_at(const scene::Scene& scene, float canvas_x,
                                float canvas_y, float reach,
                                const scene::SurfaceFilter& counts) {
    const auto ray =
        scene::canvas_ray(scene.camera, scene.canvas, canvas_x, canvas_y);
    if (!ray.has_value()) {
        return std::nullopt;
    }
    const std::optional<scene::SurfaceHit> hit =
        surface_at(scene, canvas_x, canvas_y, counts);
    if (!hit.has_value()) {
        if (ray->direction.y < -1.0e-6F && ray->origin.y > 0.0F) {
            const float distance = -ray->origin.y / ray->direction.y;
            if (distance <= scene.camera.far_plane) {
                scene::Vec3 floor = ray->origin + ray->direction * distance;
                floor.y = 0.0F;
                return MeasurePoint{floor, MeasureSnap::Floor, {}};
            }
        }
        return std::nullopt;
    }
    const MeasurePoint point{hit->position, MeasureSnap::Surface, hit->node};

    std::shared_ptr<const scene::MeshData> mesh;
    scene::Mat4 world{};
    switch (hit->source) {
    case scene::SurfaceSource::Instance:
        mesh = primitive_mesh(scene.instances[hit->index].mesh);
        world = scene.instances[hit->index].world;
        break;
    case scene::SurfaceSource::Media:
        mesh = primitive_mesh(scene.media[hit->index].mesh);
        world = scene.media[hit->index].world;
        break;
    case scene::SurfaceSource::Model: {
        const scene::ModelInstance& model = scene.models[hit->index];
        const render::ModelCacheObservation observation =
            model_cache_.observe(model.source, model_publication_fence_);
        if (observation.ready()) {
            mesh = std::shared_ptr<const scene::MeshData>{
                observation.asset, &observation.asset->mesh};
        }
        world = model.world;
        break;
    }
    }
    const std::shared_ptr<const scene::MeshBvh> bvh =
        mesh != nullptr ? surface_cache_->observe(mesh) : nullptr;
    if (bvh == nullptr) {
        return point;
    }

    // `reach` canvas pixels at the hit's depth, carried into the element's
    // local space. The inverse's longest axis bounds how far that can stretch.
    const float pixel_world = scene::units_per_canvas_pixel(
        scene.camera, hit->distance, static_cast<float>(scene.canvas.height));
    const scene::Mat4 inverse = scene::invert_affine(world);
    float inverse_scale = 0.0F;
    for (core::u32 column = 0U; column < 3U; ++column) {
        inverse_scale = std::max(
            inverse_scale,
            scene::length(scene::Vec3{inverse.at(0U, column),
                                      inverse.at(1U, column),
                                      inverse.at(2U, column)}));
    }
    const float local_reach = reach * pixel_world * inverse_scale * 1.75F;
    std::vector<core::u32> triangles;
    static_cast<void>(scene::mesh_triangles_near(
        *bvh, *mesh, scene::transform_point(inverse, hit->position),
        local_reach, triangles, 512U));

    // Nearest on screen wins; a vertex anywhere in reach beats a midpoint.
    // Points level on screen go to the one nearer the surface the pointer is
    // on: in an orthographic view a corner and the corner straight behind it
    // land on the same pixel, and only the front one can be seen.
    float vertex_reach = reach;
    float midpoint_reach = reach;
    std::optional<scene::Vec3> vertex;
    std::optional<scene::Vec3> midpoint;
    constexpr float level = 0.25F;
    const auto closer = [&](float distance, float best,
                            const std::optional<scene::Vec3>& chosen,
                            scene::Vec3 candidate) {
        if (distance < best - level) {
            return true;
        }
        return chosen.has_value() && distance < best + level &&
               scene::length(candidate - hit->position) <
                   scene::length(*chosen - hit->position);
    };
    const auto screen_distance = [&](scene::Vec3 candidate) {
        const auto projected =
            scene::project_to_canvas(scene.camera, scene.canvas, candidate);
        if (!projected.visible) {
            return std::numeric_limits<float>::infinity();
        }
        const float dx = projected.x - canvas_x;
        const float dy = projected.y - canvas_y;
        return std::sqrt(dx * dx + dy * dy);
    };
    for (const core::u32 triangle : triangles) {
        std::array<scene::Vec3, 3U> corners{};
        for (core::u32 corner = 0U; corner < 3U; ++corner) {
            corners[corner] = scene::transform_point(
                world,
                mesh->vertices[mesh->indices[triangle * 3U + corner]].position);
        }
        for (core::u32 corner = 0U; corner < 3U; ++corner) {
            const float distance = screen_distance(corners[corner]);
            if (distance < reach &&
                (!vertex.has_value() ||
                 closer(distance, vertex_reach, vertex, corners[corner]))) {
                vertex_reach = std::min(vertex_reach, distance);
                vertex = corners[corner];
            }
            const scene::Vec3 middle =
                (corners[corner] + corners[(corner + 1U) % 3U]) * 0.5F;
            const float middle_distance = screen_distance(middle);
            if (middle_distance < reach &&
                (!midpoint.has_value() ||
                 closer(middle_distance, midpoint_reach, midpoint, middle))) {
                midpoint_reach = std::min(midpoint_reach, middle_distance);
                midpoint = middle;
            }
        }
    }
    if (vertex.has_value()) {
        return MeasurePoint{*vertex, MeasureSnap::Vertex, hit->node};
    }
    if (midpoint.has_value()) {
        return MeasurePoint{*midpoint, MeasureSnap::EdgeMidpoint, hit->node};
    }
    return point;
}

scene::ModelLocalBounds
ViewportPanel::element_local_bounds(const scene::Scene& scene,
                                    doc::NodeId node) {
    for (const auto& object : scene.instances) {
        if (object.source == node) {
            return primitive_local_bounds(object.mesh.primitive);
        }
    }
    for (const auto& clip : scene.media) {
        if (clip.source_node == node) {
            return primitive_local_bounds(clip.mesh.primitive);
        }
    }
    for (const auto& model : scene.models) {
        if (model.source_node == node) {
            return model_bounds_for(model).value_or(scene::ModelLocalBounds{});
        }
    }
    return scene::ModelLocalBounds{scene::Vec3{}, scene::Vec3{}};
}

std::optional<scene::Vec3> ViewportPanel::nearest_element_vertex(
    const scene::Scene& scene, doc::NodeId node, const scene::Mat4& world,
    scene::Vec3 near) {
    std::shared_ptr<const scene::MeshData> mesh;
    for (const auto& object : scene.instances) {
        if (object.source == node) {
            mesh = primitive_mesh(object.mesh);
        }
    }
    for (const auto& clip : scene.media) {
        if (mesh == nullptr && clip.source_node == node) {
            mesh = primitive_mesh(clip.mesh);
        }
    }
    for (const auto& model : scene.models) {
        if (mesh == nullptr && model.source_node == node) {
            const render::ModelCacheObservation observation =
                model_cache_.observe(model.source, model_publication_fence_);
            if (observation.ready()) {
                mesh = std::shared_ptr<const scene::MeshData>{
                    observation.asset, &observation.asset->mesh};
            }
        }
    }
    if (mesh == nullptr || mesh->vertices.empty()) {
        return std::nullopt;
    }
    // Once per drag, when V is first held: a plain pass over the vertices.
    const scene::MeshVertex* best = &mesh->vertices.front();
    float best_distance = std::numeric_limits<float>::infinity();
    for (const scene::MeshVertex& vertex : mesh->vertices) {
        const scene::Vec3 offset =
            scene::transform_point(world, vertex.position) - near;
        const float distance = scene::dot(offset, offset);
        if (distance < best_distance) {
            best_distance = distance;
            best = &vertex;
        }
    }
    return scene::transform_point(world, best->position);
}

std::vector<doc::NodeId>
ViewportPanel::elements_in_marquee(const scene::Scene& scene,
                                   const gui::Rect& image,
                                   const gui::Rect& box) {
    std::vector<doc::NodeId> inside;
    if (image.width <= 0.0F || image.height <= 0.0F) {
        return inside;
    }
    // An element counts when the screen box around its visible corners
    // touches the drawn box, as Unreal's marquee does.
    const auto touches = [&](const scene::Mat4& world,
                             const scene::ModelLocalBounds& bounds) {
        float left = std::numeric_limits<float>::infinity();
        float top = left;
        float right = -left;
        float bottom = -left;
        bool any = false;
        for (unsigned corner = 0U; corner < 8U; ++corner) {
            const scene::Vec3 local{
                (corner & 1U) != 0U ? bounds.maximum.x : bounds.minimum.x,
                (corner & 2U) != 0U ? bounds.maximum.y : bounds.minimum.y,
                (corner & 4U) != 0U ? bounds.maximum.z : bounds.minimum.z};
            const auto projected = scene::project_to_canvas(
                scene.camera, scene.canvas, scene::transform_point(world, local));
            if (!projected.visible) {
                continue;
            }
            const gui::UiPoint at = to_ui(image, scene.canvas, projected);
            left = std::min(left, at.x);
            right = std::max(right, at.x);
            top = std::min(top, at.y);
            bottom = std::max(bottom, at.y);
            any = true;
        }
        return any && right >= box.x && left <= box.right() &&
               bottom >= box.y && top <= box.bottom();
    };
    const auto add = [&inside](doc::NodeId node) {
        if (std::find(inside.begin(), inside.end(), node) == inside.end()) {
            inside.push_back(node);
        }
    };
    for (const auto& object : scene.instances) {
        if (!object.locked &&
            touches(object.world, primitive_local_bounds(object.mesh.primitive))) {
            add(object.source);
        }
    }
    for (const auto& clip : scene.media) {
        if (!clip.locked &&
            touches(clip.world, primitive_local_bounds(clip.mesh.primitive))) {
            add(clip.source_node);
        }
    }
    for (const auto& model : scene.models) {
        if (!model.locked &&
            touches(model.world,
                    model_bounds_for(model).value_or(scene::ModelLocalBounds{}))) {
            add(model.source_node);
        }
    }
    for (const auto& light : scene.point_lights) {
        if (!light.locked &&
            touches(scene::translation(light.position),
                    scene::ModelLocalBounds{scene::Vec3{}, scene::Vec3{}})) {
            add(light.source);
        }
    }
    return inside;
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
            haloed_projected_line(list, image, scene, corners[index],
                                  corners[(index + 1U) % corners.size()], 4.0F,
                                  1.8F, accent);
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
        ctx.label(gui::inset(badge, gui::Padding{8.0F, 0.0F, 0.0F, 0.0F}),
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
    surface_cache_->request_reset();
    primitive_meshes_.clear();
    set_measuring(false);
    pivot_override_.reset();
    pivot_selection_.clear();
    pivot_dragging_ = false;
    marquee_active_ = false;
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

bool detail::wait_for_viewport_surface_hierarchies_for_test(
    ViewportPanel& panel) noexcept {
    return detail::wait_for_model_surface_cache_for_test(*panel.surface_cache_);
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
    return canvas_to_image(image, canvas, canvas_x, canvas_y);
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
    square(list, point, size, color);
}

} // namespace

bool ViewportPanel::snap_drag(const gui::UiContext& ctx,
                              const scene::Scene& scene,
                              std::span<const doc::NodeId> selection,
                              const ViewportFrame& frame,
                              TransformDelta& delta) {
    drag_snap_target_.reset();
    if (role_ != ViewportRole::Stage || frame.image.width <= 0.0F) {
        return false;
    }
    // What moves never counts as somewhere to land.
    const doc::NodeId dragged = drag_node_;
    const scene::SurfaceFilter elsewhere =
        [dragged, selection](doc::NodeId node) {
            return node != dragged && !selected(selection, node);
        };
    if (ctx.move_key_down(gui::MoveKey::V)) {
        if (!drag_source_vertex_.has_value() && drag_grab_.has_value()) {
            drag_source_vertex_ =
                nearest_element_vertex(scene, drag_node_, drag_start_world_,
                                       *drag_grab_)
                    .value_or(*drag_grab_);
        }
        const auto target = measure_point_at(
            scene, frame.pointer.x, frame.pointer.y,
            14.0F * static_cast<float>(scene.canvas.width) / frame.image.width,
            elsewhere);
        if (drag_source_vertex_.has_value() && target.has_value() &&
            (target->snap == MeasureSnap::Vertex ||
             target->snap == MeasureSnap::EdgeMidpoint)) {
            delta.translation = target->position - *drag_source_vertex_;
            drag_snap_target_ = target;
            return true;
        }
        return false;
    }
    if (!surface_snapping_ || drag_axis_ != GizmoAxis::Screen) {
        return false;
    }
    const auto hit =
        surface_at(scene, frame.pointer.x, frame.pointer.y, elsewhere);
    if (!hit.has_value()) {
        return false;
    }
    const scene::Vec3 contact =
        snapping_ != ctx.ctrl_held()
            ? snap_contact(hit->position, hit->normal,
                           ctx.shift_held() ? snap_steps_.fine_move()
                                            : snap_steps_.move)
            : hit->position;
    delta.translation = resting_origin(contact, hit->normal, drag_start_world_,
                                       drag_start_bounds_) -
                        scene::translation_of(drag_start_world_);
    return true;
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
        const float step = fine ? snap_steps_.fine_move() : snap_steps_.move;
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
                                   fine ? snap_steps_.fine_rotate_turns()
                                        : snap_steps_.rotate_turns);
        break;
    case TransformTool::Scale: {
        const float step =
            fine ? snap_steps_.fine_scale() : snap_steps_.scale;
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
    const std::array<gui::UiColor, 3U> colors{ctx.theme().gizmo_axis_x,
                                              ctx.theme().gizmo_axis_y,
                                              ctx.theme().gizmo_axis_z};
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
             1.0F, ctx.theme().gizmo_view);
        square(list, centre, gizmo_hub_size, ctx.theme().gizmo_view);
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
    gui::Rect corner = gui::inset(image, 8.0F);
    corner = gui::cut_top(corner, 22.0F);
    const gui::Rect bar = gui::cut_left(corner, 640.0F);
    ctx.panel(bar, ctx.theme().background);
    ctx.label(gui::inset(bar, 8.0F, 0.0F), status, ctx.theme().text);
}

namespace {

constexpr gui::UiColor measure_color{1.0F, 0.80F, 0.22F, 1.0F};
constexpr gui::UiColor measure_ink{0.06F, 0.05F, 0.02F, 1.0F};
constexpr gui::UiColor measure_shade{0.04F, 0.05F, 0.06F, 0.86F};

void measure_badge(gui::UiContext& ctx, float x, float y,
                   std::string_view text, bool strong) {
    // Centred on x, its top at y.
    const gui::Rect badge = gui::centered(gui::Rect{x, y, 0.0F, 20.0F},
                                          ctx.font_width(text) + 12.0F, 20.0F);
    ctx.panel(badge, strong ? measure_color : measure_shade);
    ctx.label(gui::inset(badge, gui::Padding{6.0F, 0.0F, 0.0F, 0.0F}), text,
              strong ? measure_ink : measure_color);
}

// The Measure tool's picture: each segment with its length at its middle,
// the latest segment's axis parts and slope beneath, a path's total at its
// end, and what the pointer would snap to.
void draw_measurements(gui::UiContext& ctx, gui::UiDrawList& list,
                       const gui::Rect& image, const scene::Scene& view,
                       const MeasureChain& chain,
                       const std::optional<MeasurePoint>& hover,
                       bool hint_shown = true) {
    std::vector<MeasurePoint> points = chain.points;
    if (chain.open && hover.has_value() && !points.empty()) {
        points.push_back(*hover);
    }
    const auto ui_of = [&](scene::Vec3 world) -> std::optional<gui::UiPoint> {
        const auto projected =
            scene::project_to_canvas(view.camera, view.canvas, world);
        if (!projected.visible) {
            return std::nullopt;
        }
        return to_ui(image, view.canvas, projected);
    };
    for (std::size_t index = 1U; index < points.size(); ++index) {
        const scene::Vec3 from = points[index - 1U].position;
        const scene::Vec3 to = points[index].position;
        haloed_projected_line(list, image, view, from, to, 5.0F, 2.0F,
                              measure_color);
        const auto middle = ui_of((from + to) * 0.5F);
        if (!middle.has_value()) {
            continue;
        }
        const MeasureReading reading = measure(from, to);
        const bool latest = index + 1U == points.size();
        // Above the segment, so neither badge covers its end points.
        measure_badge(ctx, middle->x, middle->y - (latest ? 52.0F : 30.0F),
                      format_metres(reading.length), true);
        if (latest) {
            measure_badge(ctx, middle->x, middle->y - 30.0F,
                          format_reading_detail(reading), false);
        }
    }
    for (const MeasurePoint& point : chain.points) {
        if (const auto at = ui_of(point.position)) {
            haloed_square(list, *at, 6.0F, 1.0F, measure_color);
        }
    }
    if (points.size() > 2U) {
        if (const auto end = ui_of(points.back().position)) {
            measure_badge(ctx, end->x, end->y + 12.0F,
                          "Total " + format_metres(path_length(points)), true);
        }
    }
    if (hover.has_value()) {
        if (const auto at = ui_of(hover->position)) {
            switch (hover->snap) {
            case MeasureSnap::Vertex:
                haloed_square(list, *at, 9.0F, 1.5F, measure_color);
                break;
            case MeasureSnap::EdgeMidpoint:
                list.add_triangle(at->x, at->y - 7.0F, at->x + 7.0F,
                                  at->y + 5.0F, at->x - 7.0F, at->y + 5.0F,
                                  measure_color);
                break;
            case MeasureSnap::Surface:
            case MeasureSnap::Floor: {
                const std::array<gui::UiPoint, 4U> ring{
                    gui::UiPoint{at->x - 5.0F, at->y - 5.0F},
                    gui::UiPoint{at->x + 5.0F, at->y - 5.0F},
                    gui::UiPoint{at->x + 5.0F, at->y + 5.0F},
                    gui::UiPoint{at->x - 5.0F, at->y + 5.0F}};
                for (std::size_t index = 0U; index < ring.size(); ++index) {
                    line(list, ring[index], ring[(index + 1U) % ring.size()],
                         1.5F, measure_color);
                }
                break;
            }
            }
            ctx.label(gui::Rect{at->x + 10.0F, at->y + 4.0F, 120.0F, 18.0F},
                      measure_snap_name(hover->snap), measure_color);
        }
    }
    if (!hint_shown) {
        return;
    }
    const std::string_view hint =
        "MEASURE   Click two points.  Shift+click adds to a path.  "
        "Esc clears; M or Esc again leaves.";
    gui::Rect corner = gui::inset(image, 6.0F);
    corner = gui::cut_top(corner, 22.0F);
    const gui::Rect box = gui::cut_left(corner, ctx.font_width(hint) + 14.0F);
    ctx.panel(box, measure_shade);
    ctx.label(gui::inset(box, gui::Padding{7.0F, 0.0F, 3.0F, 0.0F}), hint,
              measure_color);
}

// The corner of a Stage image its view menu, `width` wide, occupies; empty
// when the image is too small to spare it.
gui::Rect stage_view_menu_bounds(const gui::Rect& image, float width) noexcept {
    if (image.width < width + 16.0F || image.height < 40.0F) {
        return gui::Rect{};
    }
    gui::Rect corner = gui::inset(image, 6.0F);
    corner = gui::cut_top(corner, 22.0F);
    return gui::cut_right(corner, width);
}

// The shading menu, `width` wide, left of the view menu; empty with it.
gui::Rect stage_shading_menu_bounds(const gui::Rect& view_menu,
                                    float width) noexcept {
    if (view_menu.width <= 0.0F) {
        return gui::Rect{};
    }
    return gui::Rect{view_menu.x - width - 4.0F, view_menu.y, width,
                     view_menu.height};
}

std::string_view stage_shading_name(render::ViewShading shading) noexcept {
    switch (shading) {
    case render::ViewShading::Unlit:
        return "Unlit";
    case render::ViewShading::LightsOnly:
        return "Lights only";
    case render::ViewShading::Wireframe:
        return "Wireframe";
    case render::ViewShading::UvChecker:
        return "UV checker";
    case render::ViewShading::WorldNormals:
        return "World normals";
    case render::ViewShading::Lit:
        break;
    }
    return "Lit";
}

std::string_view stage_projection_name(StageProjection projection) noexcept {
    switch (projection) {
    case StageProjection::Top:
        return "Top";
    case StageProjection::Front:
        return "Front";
    case StageProjection::Side:
        return "Side";
    case StageProjection::Perspective:
        break;
    }
    return "Perspective";
}

// The Stage toolbar's buttons: 26 wide, 22 high, three below its top.
constexpr float toolbar_button = 26.0F;
constexpr float toolbar_group_gap = 9.0F;

gui::UiColor toolbar_ink(const gui::Theme& theme, float t) noexcept {
    return gui::UiColor{
        theme.text_muted.r + (theme.text.r - theme.text_muted.r) * t,
        theme.text_muted.g + (theme.text.g - theme.text_muted.g) * t,
        theme.text_muted.b + (theme.text.b - theme.text_muted.b) * t, 1.0F};
}

void draw_chevron(gui::UiDrawList& list, float x, float y, gui::UiColor color) {
    list.add_line(x - 3.0F, y - 1.5F, x, y + 1.5F, 1.0F, color);
    list.add_line(x, y + 1.5F, x + 3.0F, y - 1.5F, 1.0F, color);
}

// The width of a toolbar_menu showing `value`.
float toolbar_menu_width(const gui::UiContext& ctx, std::string_view value,
                         bool degrees = false) noexcept {
    return 23.0F + ctx.font_width(value) + (degrees ? 5.0F : 0.0F) + 19.0F;
}

// A button that opens a menu, showing its icon and the current choice, so
// the setting is read without opening it. `quiet` greys a value that is not
// in force; `overlay` gives it a body of its own, for a menu that floats over
// the picture rather than sitting in the toolbar.
bool toolbar_menu(gui::UiContext& ctx, gui::UiId id, const gui::Rect& bounds,
                  gui::Icon icon, std::string_view value, bool open,
                  bool degrees = false, bool quiet = false,
                  bool overlay = false) {
    const gui::Theme& theme = ctx.theme();
    const gui::Interaction it = ctx.grab(id, bounds);
    ctx.cursor_for(it, gui::Cursor::Hand);
    if (overlay) {
        ctx.panel(bounds, gui::UiColor{0.035F, 0.040F, 0.052F, 0.80F});
    }
    if (it.held && !it.released) {
        ctx.panel(bounds, theme.widget_active);
    } else if (open || it.hovered) {
        ctx.panel(bounds, theme.widget_hover);
    }
    const bool lit = open || it.hovered;
    const float middle = bounds.y + bounds.height * 0.5F;
    gui::UiDrawList& list = ctx.draw_list();
    gui::draw_icon(list, icon, bounds.x + 12.0F, middle,
                   toolbar_ink(theme, lit ? 1.0F : (quiet ? 0.0F : 0.75F)));
    const gui::UiColor ink = quiet && !lit ? theme.text_muted : theme.text;
    const float text_width = ctx.font_width(value);
    ctx.label(gui::Rect{bounds.x + 23.0F, bounds.y, text_width + 2.0F,
                        bounds.height},
              value, ink);
    if (degrees) {
        // The atlas is ASCII, so the degree sign is drawn.
        const float ring_x = bounds.x + 23.0F + text_width + 1.5F;
        const float ring_y = middle - 5.0F;
        list.add_rect(ring_x, ring_y, 3.0F, 1.0F, ink);
        list.add_rect(ring_x, ring_y + 2.0F, 3.0F, 1.0F, ink);
        list.add_rect(ring_x, ring_y, 1.0F, 3.0F, ink);
        list.add_rect(ring_x + 2.0F, ring_y, 1.0F, 3.0F, ink);
    }
    draw_chevron(list, bounds.right() - 9.0F, middle + 1.0F,
                 lit ? theme.text : theme.text_muted);
    return it.clicked;
}

// The nearest of `choices` to `value`, by index.
template <typename Choices>
std::size_t nearest_choice(const Choices& choices, float value) noexcept {
    std::size_t best = 0U;
    for (std::size_t index = 1U; index < choices.size(); ++index) {
        if (std::abs(choices[index] - value) <
            std::abs(choices[best] - value)) {
            best = index;
        }
    }
    return best;
}

} // namespace

void ViewportPanel::draw_stage_view_menu(gui::UiContext& ctx,
                                         const gui::Rect& menu,
                                         ViewportFrame& frame) {
    if (menu.width <= 0.0F) {
        return;
    }
    const gui::UiId menu_id = gui::ui_id(id_, 40U);
    if (toolbar_menu(ctx, gui::ui_id(id_, 41U), menu, gui::Icon::Projection,
                     stage_projection_name(stage_projection_),
                     ctx.menu_open(menu_id), false, false, true)) {
        ctx.open_context_menu(menu_id, menu.x, menu.bottom());
    }
    ctx.tooltip(menu,
                "This view's projection, and one or four views. Top, Front "
                "and Side are orthographic: drag to pan, wheel to zoom.");
    const std::array<std::string_view, 5U> options{
        "Perspective    Alt+P", "Top            Alt+J",
        "Front          Alt+H", "Side           Alt+K", "Four views"};
    const std::array<bool, 5U> checked{
        stage_projection_ == StageProjection::Perspective,
        stage_projection_ == StageProjection::Top,
        stage_projection_ == StageProjection::Front,
        stage_projection_ == StageProjection::Side, stage_four_views_};
    if (const auto chosen = ctx.context_menu(menu_id, options, checked)) {
        if (*chosen < 4U) {
            frame.projection_choice = static_cast<StageProjection>(*chosen);
        } else {
            frame.layout_choice = !stage_four_views_;
        }
    }
}

void ViewportPanel::draw_toolbar(gui::UiContext& ctx, const gui::Rect& toolbar,
                                 float frames_per_second) {
    const gui::Theme& theme = ctx.theme();
    ctx.panel(toolbar, theme.background);
    {
        gui::Rect rule = toolbar;
        ctx.panel(gui::cut_bottom(rule, 1.0F), theme.divider);
    }
    // Controls are taken from both ends of one strip, 3px under the top and
    // 4px clear of the rule.
    gui::Rect bar = gui::inset(toolbar, gui::Padding{6.0F, 3.0F, 8.0F, 4.0F});
    const auto separator = [&](float x) {
        ctx.panel(gui::Rect{std::floor(x), toolbar.y + 8.0F, 1.0F,
                            toolbar.height - 17.0F},
                  theme.border);
    };

    // The preview's resolution and frame rate hold the right end at any
    // width; the tools fill from the left, and the display toggles take what
    // is left between them, folding into one menu when it is not enough.
    std::array<char, 24U> fps_text{};
    const int fps_length =
        std::snprintf(fps_text.data(), fps_text.size(), "%.3g fps",
                      std::max(frames_per_second, 0.0F));
    const std::string_view fps{
        fps_text.data(), static_cast<std::size_t>(std::max(fps_length, 0))};
    ctx.label(gui::cell(gui::cut_right(bar, ctx.font_width(fps)), toolbar),
              fps, theme.text_muted);
    static_cast<void>(gui::cut_right(bar, 8.0F));
    draw_resolution_control(ctx,
                            gui::cut_right(bar, resolution_control_width(ctx)));
    static_cast<void>(gui::cut_right(bar, 4.0F));
    draw_media_control(ctx, gui::cut_right(bar, media_control_width(ctx)));
    static_cast<void>(gui::cut_right(bar, toolbar_group_gap));

    static constexpr std::array<std::string_view, move_snap_choices.size()>
        distances{"0.01 m", "0.1 m", "0.5 m", "1 m", "5 m"};
    static constexpr std::array<std::string_view,
                                rotate_snap_choices_degrees.size()>
        degrees{"1", "5", "15", "45", "90"};
    static constexpr std::array<std::string_view,
                                rotate_snap_choices_degrees.size()>
        angles{"1 degree", "5 degrees", "15 degrees", "45 degrees",
               "90 degrees"};
    const std::size_t move_step =
        nearest_choice(move_snap_choices, snap_steps_.move);
    const std::size_t turn_step = nearest_choice(
        rotate_snap_choices_degrees, snap_steps_.rotate_turns * 360.0F);
    const float distance_width = toolbar_menu_width(ctx, distances[move_step]);
    const float angle_width =
        toolbar_menu_width(ctx, degrees[turn_step], true);
    constexpr float display_menu_width = 38.0F;
    constexpr float snap_gap = 2.0F;
    const float tools_width = 4.0F * toolbar_button + toolbar_group_gap +
                              toolbar_button + toolbar_group_gap +
                              toolbar_button + snap_gap + distance_width +
                              snap_gap + angle_width + snap_gap +
                              toolbar_button;
    if (tools_width + toolbar_group_gap + display_menu_width <= bar.width) {
        const auto tool = [&](core::u64 salt, gui::Icon icon, bool engaged,
                              std::string_view hint) {
            return icon_button(ctx, gui::ui_id(id_, salt),
                               gui::cut_left(bar, toolbar_button), icon, hint,
                               engaged);
        };
        const auto next_group = [&] {
            static_cast<void>(gui::cut_left(bar, toolbar_group_gap));
            separator(bar.x - toolbar_group_gap * 0.5F);
        };
        if (tool(10U, gui::Icon::MoveTool,
                 !measuring_ && tool_ == TransformTool::Move,
                 "Move. G moves the selection from the keyboard; X, Y or Z "
                 "constrains it, Shift+X/Y/Z to a plane.")) {
            tool_ = TransformTool::Move;
            set_measuring(false);
        }
        if (tool(11U, gui::Icon::RotateTool,
                 !measuring_ && tool_ == TransformTool::Rotate,
                 "Rotate. R rotates the selection about the view, or about "
                 "X, Y or Z once one is typed.")) {
            tool_ = TransformTool::Rotate;
            set_measuring(false);
        }
        if (tool(12U, gui::Icon::ScaleTool,
                 !measuring_ && tool_ == TransformTool::Scale,
                 "Scale. S scales the selection; X, Y or Z scales one axis, "
                 "Shift+X/Y/Z the other two.")) {
            tool_ = TransformTool::Scale;
            set_measuring(false);
        }
        if (tool(15U, gui::Icon::Measure, measuring_,
                 "Measure (M). Click two points for the distance between "
                 "them in metres; points snap to vertices, then edge "
                 "midpoints, then the surface. Shift+click adds to a path. "
                 "Nothing measured is saved.")) {
            set_measuring(!measuring_);
        }
        next_group();
        const bool global = orientation_ == TransformOrientation::Global;
        if (tool(13U, global ? gui::Icon::World : gui::Icon::Local, false,
                 global ? "World axes: the gizmo and the axis keys follow the "
                          "Stage. Click for the element's own axes; an axis "
                          "key pressed twice uses the other for that "
                          "transform."
                        : "Local axes: the gizmo and the axis keys follow the "
                          "selected element. Click for the Stage's axes; an "
                          "axis key pressed twice uses the other for that "
                          "transform.")) {
            orientation_ = global ? TransformOrientation::Local
                                  : TransformOrientation::Global;
        }
        next_group();
        if (tool(14U, gui::Icon::Snap, snapping_,
                 "Snap moves, turns and scales to the steps beside this "
                 "(Shift: a tenth of the distance and scale step, a fifth "
                 "of the angle). Shift+Tab toggles; hold Ctrl to invert "
                 "while dragging.")) {
            snapping_ = !snapping_;
        }
        static_cast<void>(gui::cut_left(bar, snap_gap));
        const gui::Rect distance = gui::cut_left(bar, distance_width);
        const gui::UiId distance_menu = gui::ui_id(id_, 22U);
        if (toolbar_menu(ctx, gui::ui_id(id_, 16U), distance, gui::Icon::Grid,
                         distances[move_step], ctx.menu_open(distance_menu),
                         false, !snapping_)) {
            ctx.open_context_menu(distance_menu, distance.x,
                                  distance.bottom());
        }
        ctx.tooltip(distance, "Move snap step.");
        {
            std::array<bool, move_snap_choices.size()> current{};
            current[move_step] = true;
            if (const auto chosen =
                    ctx.context_menu(distance_menu, distances, current)) {
                snap_steps_.move = move_snap_choices[*chosen];
            }
        }
        static_cast<void>(gui::cut_left(bar, snap_gap));
        const gui::Rect angle = gui::cut_left(bar, angle_width);
        const gui::UiId angle_menu = gui::ui_id(id_, 23U);
        if (toolbar_menu(ctx, gui::ui_id(id_, 17U), angle, gui::Icon::Angle,
                         degrees[turn_step], ctx.menu_open(angle_menu), true,
                         !snapping_)) {
            ctx.open_context_menu(angle_menu, angle.x,
                                  angle.bottom());
        }
        ctx.tooltip(angle, "Rotate snap step.");
        {
            std::array<bool, rotate_snap_choices_degrees.size()> current{};
            current[turn_step] = true;
            if (const auto chosen =
                    ctx.context_menu(angle_menu, angles, current)) {
                snap_steps_.rotate_turns =
                    rotate_snap_choices_degrees[*chosen] / 360.0F;
            }
        }
        static_cast<void>(gui::cut_left(bar, snap_gap));
        if (tool(18U, gui::Icon::Surface, surface_snapping_,
                 "Surface snapping: an element dragged by its body rides "
                 "over whatever is under the pointer and rests on it. Hold "
                 "V while moving to put the dragged element's vertex "
                 "nearest the pointer onto the vertex under it.")) {
            surface_snapping_ = !surface_snapping_;
        }
    }

    // The display toggles, right-aligned before the preview controls.
    constexpr float toggles_width = 4.0F * toolbar_button;
    if (toolbar_group_gap + toggles_width <= bar.width) {
        separator(bar.right() + toolbar_group_gap * 0.5F);
        gui::Rect toggles = gui::cut_right(bar, toggles_width);
        const auto toggle = [&](core::u64 salt, gui::Icon icon, bool& shown,
                                std::string_view hint) {
            if (icon_button(ctx, gui::ui_id(id_, salt),
                            gui::cut_left(toggles, toolbar_button), icon,
                            hint, shown)) {
                shown = !shown;
            }
        };
        toggle(1U, gui::Icon::Grid, show_grid_,
               "Grid: the floor grid, its spacing following the zoom.");
        toggle(2U, gui::Icon::Guides, show_overlays_,
               "Guides: camera frustums, light reach and other helpers "
               "drawn over the Stage.");
        toggle(3U, gui::Icon::Gizmo, show_gizmo_,
               "Gizmo: the selection's transform handles.");
        toggle(19U, gui::Icon::Bounds, show_bounds_,
               "Bounds: every element's bounding box, selected or not.");
    } else if (toolbar_group_gap + display_menu_width <= bar.width) {
        separator(bar.right() + toolbar_group_gap * 0.5F);
        // One menu holds them, so they stay reachable in a narrow dock.
        const gui::Rect show = gui::cut_right(bar, display_menu_width);
        const gui::UiId show_menu = gui::ui_id(id_, 20U);
        const gui::Interaction it = ctx.grab(gui::ui_id(id_, 21U), show);
        ctx.cursor_for(it, gui::Cursor::Hand);
        const bool open = ctx.menu_open(show_menu);
        if (it.held && !it.released) {
            ctx.panel(show, theme.widget_active);
        } else if (open || it.hovered) {
            ctx.panel(show, theme.widget_hover);
        }
        const bool lit = open || it.hovered;
        const float middle = show.y + show.height * 0.5F;
        gui::draw_eye_glyph(ctx.draw_list(), show.x + 13.0F, middle, true,
                            toolbar_ink(theme, lit ? 1.0F : 0.75F));
        draw_chevron(ctx.draw_list(), show.right() - 8.0F, middle,
                     lit ? theme.text : theme.text_muted);
        if (it.clicked) {
            ctx.open_context_menu(show_menu, show.x, show.bottom());
        }
        ctx.tooltip(show, "Show: grid, guides, gizmo and bounds.");
        constexpr std::array<std::string_view, 4U> options{
            "Grid", "Guides", "Gizmo", "Bounds"};
        const std::array<bool, 4U> shown{show_grid_, show_overlays_,
                                         show_gizmo_, show_bounds_};
        if (const auto chosen = ctx.context_menu(show_menu, options, shown)) {
            switch (*chosen) {
            case 0U: show_grid_ = !show_grid_; break;
            case 1U: show_overlays_ = !show_overlays_; break;
            case 2U: show_gizmo_ = !show_gizmo_; break;
            default: show_bounds_ = !show_bounds_; break;
            }
        }
    }
}

namespace {

constexpr std::array<std::string_view, 3U> resolution_names{"Full", "Half",
                                                          "Quarter"};

} // namespace

float ViewportPanel::resolution_control_width(
    const gui::UiContext& ctx) const noexcept {
    return toolbar_menu_width(
        ctx, resolution_names[static_cast<std::size_t>(resolution_)]);
}

void ViewportPanel::draw_resolution_control(gui::UiContext& ctx,
                                             const gui::Rect& bounds) {
    const auto current_index = static_cast<std::size_t>(resolution_);
    const gui::UiId menu = gui::ui_id(id_, 24U);
    if (toolbar_menu(ctx, gui::ui_id(id_, 4U), bounds, gui::Icon::Screen,
                     resolution_names[current_index], ctx.menu_open(menu))) {
        ctx.open_context_menu(menu, bounds.x, bounds.bottom());
    }
    ctx.tooltip(bounds,
                "Preview resolution: lower renders this view faster. What "
                "a click picks is unchanged.");
    std::array<bool, 3U> current{};
    current[current_index] = true;
    if (const auto chosen = ctx.context_menu(menu, resolution_names, current)) {
        resolution_ = static_cast<PreviewResolution>(*chosen);
    }
}

namespace {

constexpr std::array<std::string_view, 2U> media_names{"Proxy", "Full res"};

} // namespace

float ViewportPanel::media_control_width(
    const gui::UiContext& ctx) const noexcept {
    return toolbar_menu_width(
        ctx, media_names[static_cast<std::size_t>(drawn_media())]);
}

void ViewportPanel::draw_media_control(gui::UiContext& ctx,
                                       const gui::Rect& bounds) {
    const MediaRendition drawn = drawn_media();
    const gui::UiId menu = gui::ui_id(id_, 25U);
    if (toolbar_menu(ctx, gui::ui_id(id_, 5U), bounds,
                     drawn == MediaRendition::Full
                         ? gui::Icon::FullResolution
                         : gui::Icon::HalfResolution,
                     media_names[static_cast<std::size_t>(drawn)],
                     ctx.menu_open(menu))) {
        ctx.open_context_menu(menu, bounds.x, bounds.bottom());
    }
    std::string tip;
    if (precise_work_ && media_ == MediaRendition::Proxy) {
        tip = "Media: full resolution while a mask is selected or the "
              "eyedropper is armed, then back to proxies.";
    } else if (drawn == MediaRendition::Full) {
        tip = media_counts_.proxied == 0U
                  ? "Media: every clip plays its source. Proxies keep being "
                    "made for when you switch back."
                  : "Media: clips play their source, except " +
                        std::to_string(media_counts_.proxied) +
                        " whose asset asks for a proxy everywhere.";
    } else {
        tip = "Media: UHD video plays a 720p proxy here; outputs always "
              "play the source.";
        const auto clips = [](core::u32 count) {
            return std::to_string(count) + (count == 1U ? " clip" : " clips");
        };
        if (media_counts_.proxied > 0U) {
            tip += " " + clips(media_counts_.proxied) + " on proxies.";
        }
        if (media_counts_.waiting > 0U) {
            tip += " " + clips(media_counts_.waiting) +
                   " on the source, which plays more slowly, until " +
                   (media_counts_.waiting == 1U ? "its proxy is"
                                                : "their proxies are") +
                   " made";
            if (media_counts_.waiting_total_frames > 0U) {
                const auto percent = std::min<core::u64>(
                    99U, media_counts_.waiting_made_frames * 100U /
                             media_counts_.waiting_total_frames);
                tip += " (" + std::to_string(percent) + "% so far)";
            }
            tip += ". The part at the playhead is made first.";
        }
    }
    // The project's editor proxies still being made, whatever this view
    // plays: how soon Proxy will play every clip from a proxy.
    const bool all_coming =
        media_counts_.all_made_frames < media_counts_.all_total_frames;
    if (all_coming) {
        const auto percent = std::min<core::u64>(
            99U, media_counts_.all_made_frames * 100U /
                     media_counts_.all_total_frames);
        tip += " The project's editor proxies are " +
               std::to_string(percent) + "% made.";
    }
    ctx.tooltip(bounds, tip);
    if (all_coming) {
        draw_proxy_progress(
            ctx, gui::ui_id(id_, 26U), bounds,
            static_cast<float>(media_counts_.all_made_frames) /
                static_cast<float>(media_counts_.all_total_frames));
    } else if (drawn == MediaRendition::Proxy && media_counts_.waiting > 0U) {
        draw_proxy_progress(
            ctx, gui::ui_id(id_, 26U), bounds,
            media_counts_.waiting_total_frames == 0U
                ? 0.0F
                : static_cast<float>(media_counts_.waiting_made_frames) /
                      static_cast<float>(media_counts_.waiting_total_frames));
    }
    std::array<bool, 2U> current{};
    current[static_cast<std::size_t>(media_)] = true;
    if (const auto chosen = ctx.context_menu(menu, media_names, current)) {
        media_ = static_cast<MediaRendition>(*chosen);
    }
}

void ViewportPanel::copy_tools_from(const ViewportPanel& other) noexcept {
    tool_ = other.tool_;
    orientation_ = other.orientation_;
    snapping_ = other.snapping_;
    snap_steps_ = other.snap_steps_;
    surface_snapping_ = other.surface_snapping_;
    if (measuring_ != other.measuring_) {
        set_measuring(other.measuring_);
    }
    show_grid_ = other.show_grid_;
    show_overlays_ = other.show_overlays_;
    show_gizmo_ = other.show_gizmo_;
    show_bounds_ = other.show_bounds_;
    resolution_ = other.resolution_;
    media_ = other.media_;
}

void ViewportPanel::draw_stage_shading_menu(gui::UiContext& ctx,
                                            const gui::Rect& menu,
                                            ViewportFrame& frame) {
    if (menu.width <= 0.0F) {
        return;
    }
    const gui::UiId menu_id = gui::ui_id(id_, 42U);
    if (toolbar_menu(ctx, gui::ui_id(id_, 43U), menu, gui::Icon::Shading,
                     stage_shading_name(stage_shading_),
                     ctx.menu_open(menu_id), false, false, true)) {
        ctx.open_context_menu(menu_id, menu.x, menu.bottom());
    }
    ctx.tooltip(menu,
                "How this view shades. Only the Stage changes: Program and "
                "the outputs always show the lit picture.");
    // In ViewShading order.
    constexpr std::array<std::string_view, 6U> options{
        "Lit            Alt+4", "Unlit          Alt+3",
        "Lights only    Alt+6", "Wireframe      Alt+2",
        "UV checker",           "World normals"};
    std::array<bool, 6U> checked{};
    checked[static_cast<std::size_t>(stage_shading_)] = true;
    if (const auto chosen = ctx.context_menu(menu_id, options, checked)) {
        frame.shading_choice = static_cast<render::ViewShading>(*chosen);
    }
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
    if (role_ == ViewportRole::Stage && toolbar_shown_ &&
        bounds.height > stage_toolbar_height) {
        draw_toolbar(ctx,
                     gui::Rect{bounds.x, bounds.y, bounds.width,
                               stage_toolbar_height},
                     frames_per_second);
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
    // The view's own menu sits over the image's top-right corner, so the
    // pointer over it picks nothing underneath.
    const gui::Rect view_menu =
        role_ == ViewportRole::Stage && projection == nullptr && !drawing_masks_
            ? stage_view_menu_bounds(
                  frame.image,
                  toolbar_menu_width(ctx,
                                     stage_projection_name(stage_projection_)))
            : gui::Rect{};
    const gui::Rect shading_menu = stage_shading_menu_bounds(
        view_menu,
        toolbar_menu_width(ctx, stage_shading_name(stage_shading_)));

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
    // A stage carried by the scene camera is drawn where the authored camera
    // puts it, so the operator sees the room the camera is carrying. Its
    // gizmo would move a surface through world coordinates the document does
    // not hold, so on that stage a surface is picked here and placed in stage
    // coordinates from Feed Mapping.
    const bool camera_stage =
        feed_map != nullptr &&
        feed_map->stage.anchor == output::StageAnchor::Camera;
    std::optional<output::StagePose> stage_pose;
    if (camera_stage) {
        selected_surface_value = nullptr;
        surface_dragging_ = false;
        const auto pose = output::camera_stage_pose(
            camera_overlay != nullptr ? *camera_overlay : scene.camera,
            feed_map->stage.eye);
        if (pose) {
            stage_pose = pose.value();
        }
    }
    if (role_ == ViewportRole::Stage && show_overlays_ && feed_map != nullptr &&
        (!camera_stage || stage_pose.has_value())) {
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
            const auto mesh =
                display.shape == output::ProjectionSurfaceShape::Planar
                    ? std::shared_ptr<const output::ProjectionMesh>{}
                    : projection_mesh_for(display);
            projected.visual = surface_visual(display, mesh.get(), stage_pose);
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
    // A moved pivot belongs to the selection it was set for.
    if (pivot_override_.has_value() &&
        !std::equal(selection.begin(), selection.end(),
                    pivot_selection_.begin(), pivot_selection_.end())) {
        pivot_override_.reset();
    }
    if (pivot.has_value() && pivot_override_.has_value()) {
        pivot = *pivot_override_;
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
        (interaction_occluder != nullptr &&
         interaction_occluder->contains(ctx.pointer_x(), ctx.pointer_y())) ||
        view_menu.contains(ctx.pointer_x(), ctx.pointer_y()) ||
        shading_menu.contains(ctx.pointer_x(), ctx.pointer_y());
    GizmoAxis hovered_scene_axis = GizmoAxis::None;
    if (!occluded && !measuring_) {
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

    // A middle drag from the gizmo's centre moves the pivot, snapping like
    // the Measure tool; a middle click there puts it back. Unreal does this
    // with Alt+middle, which pans here.
    if (role_ == ViewportRole::Stage && projection == nullptr && !occluded &&
        scene_gizmo.visible && !keyboard_.active &&
        ctx.middle_pointer_pressed()) {
        const float dx = ctx.pointer_x() - scene_gizmo.origin.x;
        const float dy = ctx.pointer_y() - scene_gizmo.origin.y;
        if (dx * dx + dy * dy <= 12.0F * 12.0F) {
            pivot_dragging_ = true;
            pivot_moved_ = false;
            pivot_press_x_ = ctx.pointer_x();
            pivot_press_y_ = ctx.pointer_y();
        }
    }
    if (pivot_dragging_) {
        frame.pivot_editing = true;
        if (ctx.middle_pointer_down()) {
            const float dx = ctx.pointer_x() - pivot_press_x_;
            const float dy = ctx.pointer_y() - pivot_press_y_;
            pivot_moved_ = pivot_moved_ || dx * dx + dy * dy > 9.0F;
            if (pivot_moved_) {
                const CanvasPoint at = to_canvas(
                    frame.image, interaction_canvas, ctx.pointer_x(),
                    ctx.pointer_y());
                if (const auto point = measure_point_at(
                        scene, at.x, at.y,
                        12.0F * canvas_width / frame.image.width)) {
                    pivot_override_ = point->position;
                    pivot_selection_.assign(selection.begin(), selection.end());
                    drag_snap_target_ = point;
                }
            }
        } else {
            if (!pivot_moved_) {
                pivot_override_.reset();
            }
            pivot_dragging_ = false;
            drag_snap_target_.reset();
        }
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
    const gui::UiId deep_menu = gui::ui_id(id_, 0xD33DU);
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
    if (role_ == ViewportRole::Stage && pointer_over_image && !ctx.typing() &&
        !keyboard_.active && !ctx.ctrl_held() && !ctx.alt_held()) {
        if (ctx.shortcut_text().find_first_of("mM") != std::string_view::npos) {
            set_measuring(!measuring_);
        } else if (measuring_ && ctx.key_pressed(gui::Key::Escape)) {
            if (measure_.points.empty()) {
                set_measuring(false);
            } else {
                measure_.clear();
            }
        }
    }
    if (role_ == ViewportRole::Stage && !keyboard_.active && !measuring_ &&
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
    measure_hover_.reset();
    if (measuring_ && role_ == ViewportRole::Stage && projection == nullptr &&
        it.hovered) {
        // Twelve interface pixels of reach, as Unreal's vertex snap feels.
        measure_hover_ = measure_point_at(
            scene, frame.pointer.x, frame.pointer.y,
            12.0F * canvas_width / frame.image.width);
    }

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
        // Two Unreal chords come first: Ctrl+Alt draws a selection box, and
        // Alt on a gizmo handle duplicates what it moves.
        const bool other_buttons =
            ctx.secondary_pointer_down() || ctx.middle_pointer_down();
        const bool marquee_chord = role_ == ViewportRole::Stage &&
                                   ctx.ctrl_held() && ctx.alt_held() &&
                                   !other_buttons && projection == nullptr;
        const bool duplicate_chord =
            role_ == ViewportRole::Stage && ctx.alt_held() &&
            !ctx.ctrl_held() && !other_buttons && !measuring_ &&
            hovered_scene_axis != GizmoAxis::None && pivot.has_value();
        const bool camera_chord =
            role_ == ViewportRole::Stage && !marquee_chord && !duplicate_chord &&
            (ctx.alt_held() || other_buttons);
        if (marquee_chord) {
            marquee_active_ = true;
            marquee_x_ = ctx.pointer_x();
            marquee_y_ = ctx.pointer_y();
        } else if (camera_chord) {
            // Left to the shell's camera navigation.
        } else if (measuring_) {
            if (measure_hover_.has_value()) {
                measure_.click(*measure_hover_, ctx.shift_held());
            }
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
            // Picked, not dragged, on a stage the camera carries.
            surface_dragging_ = !camera_stage;
        } else if (hovered_scene_axis != GizmoAxis::None &&
                   pivot.has_value()) {
            frame.picked = primary;
            frame.duplicate = duplicate_chord;
            drag_axis_ = hovered_scene_axis;
            drag_tool_ = gizmo_tool;
            drag_node_ = primary;
            drag_start_world_ = primary_world.value_or(scene::identity());
            drag_start_bounds_ = element_local_bounds(scene, primary);
            drag_grab_ = *pivot;
            drag_source_vertex_.reset();
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
                    drag_node_ = front->node;
                    drag_start_world_ = *world;
                    drag_start_bounds_ = element_local_bounds(scene, front->node);
                    const doc::NodeId grabbed = front->node;
                    const auto grab = projection == nullptr
                                          ? surface_at(scene, frame.pointer.x,
                                                       frame.pointer.y,
                                                       [grabbed](doc::NodeId node) {
                                                           return node == grabbed;
                                                       })
                                          : std::nullopt;
                    drag_grab_ = grab.has_value()
                                     ? grab->position
                                     : scene::translation_of(*world);
                    drag_source_vertex_.reset();
                    dragging_ = true;
                }
            }
        }
        if (!marquee_chord && (camera_chord || !measuring_) &&
            !frame.picked.has_value() && !frame.surface_picked.has_value()) {
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
                const auto mesh =
                    edited.shape == output::ProjectionSurfaceShape::Planar
                        ? std::shared_ptr<const output::ProjectionMesh>{}
                        : projection_mesh_for(edited);
                projected.visual =
                    surface_visual(edited, mesh.get(), stage_pose);
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
            // A surface or vertex snap puts the element exactly; the grid
            // step does not round it afterwards.
            bool exact = false;
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
                exact = snap_drag(ctx, scene, selection, frame, delta);
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
            if (!exact) {
                delta = snapped(ctx, delta);
            }
            frame.transform = delta;
            frame.translation = delta.translation;
        }
    }
    if ((!it.held || !dragging_) && !pivot_dragging_) {
        drag_snap_target_.reset();
    }
    if (marquee_active_ && (it.released || !it.held)) {
        marquee_active_ = false;
        const gui::Rect box{std::min(marquee_x_, ctx.pointer_x()),
                            std::min(marquee_y_, ctx.pointer_y()),
                            std::abs(ctx.pointer_x() - marquee_x_),
                            std::abs(ctx.pointer_y() - marquee_y_)};
        frame.marquee = elements_in_marquee(scene, frame.image, box);
        frame.marquee_adds = ctx.shift_held();
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
        const float step = draw_grid(list, frame.image, scene);
        const std::string text = "Grid " + format_metres(step);
        // The grid's spacing in the bottom-right corner.
        gui::Rect foot = gui::inset(frame.image,
                                    gui::Padding{0.0F, 0.0F, 0.0F, 4.0F});
        foot = gui::cut_bottom(foot, 22.0F);
        ctx.label(gui::cut_right(foot, ctx.font_width(text) + 14.0F), text,
                  ctx.theme().text_muted);
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
    if (role_ == ViewportRole::Stage && show_bounds_) {
        const gui::UiColor bounds_color{0.45F, 0.78F, 0.86F, 0.55F};
        for (const auto& object : scene.instances) {
            wire_box(list, frame.image, scene, object.world, bounds_color,
                     primitive_local_bounds(object.mesh.primitive), projection);
        }
        for (const auto& clip : scene.media) {
            wire_box(list, frame.image, scene, clip.world, bounds_color,
                     primitive_local_bounds(clip.mesh.primitive), projection);
        }
        for (const auto& model : scene.models) {
            wire_box(list, frame.image, scene, model.world, bounds_color,
                     model_bounds_for(model).value_or(scene::ModelLocalBounds{}),
                     projection);
        }
    }
    draw_selection_outlines(list, frame.image, scene, selection, primary,
                            model_bounds, projection);
    if (!highlight_.empty()) {
        draw_selection_outlines(list, frame.image, scene, highlight_, {},
                                model_bounds, projection,
                                ctx.theme().caution);
    }
    // The selection's size along the world axes, in metres.
    if (role_ == ViewportRole::Stage && show_overlays_ && projection == nullptr &&
        !selection.empty()) {
        if (const auto box = world_bounds(scene, selection)) {
            const scene::Vec3 size = box->maximum - box->minimum;
            if (size.x > 0.0F || size.y > 0.0F || size.z > 0.0F) {
                const std::string text =
                    "W " + format_metres(size.x) + "   H " +
                    format_metres(size.y) + "   D " + format_metres(size.z);
                // The size in the bottom-left corner.
                gui::Rect foot = gui::inset(frame.image, 6.0F);
                foot = gui::cut_bottom(foot, 22.0F);
                const gui::Rect badge =
                    gui::cut_left(foot, ctx.font_width(text) + 14.0F);
                ctx.panel(badge, gui::UiColor{0.04F, 0.05F, 0.06F, 0.82F});
                ctx.label(gui::inset(badge, gui::Padding{7.0F, 0.0F, 0.0F, 0.0F}),
                          text, ctx.theme().text);
                ctx.tooltip(badge,
                            "The selection's bounding box along the world X "
                            "(W), Y (H) and Z (D) axes.");
            }
        }
    }
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
            draw_gizmo(list, ctx.theme(), surface_gizmo, translation_hover,
                       translation_active);
            draw_yaw_ring(list, ctx.theme(), surface_gizmo,
                          hovered_surface_yaw,
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
                draw_gizmo(list, ctx.theme(), scene_gizmo, hovered, active);
                break;
            case TransformTool::Rotate:
                draw_rotate_gizmo(list, ctx.theme(), scene_gizmo, hovered,
                                  active);
                break;
            case TransformTool::Scale:
                draw_scale_gizmo(list, ctx.theme(), scene_gizmo, hovered,
                                 active);
                break;
            }
            if (scene_gizmo.visible && active != GizmoAxis::None &&
                drag_tool_ != TransformTool::Move) {
                line(list, gui::UiPoint{drag_centre_x_, drag_centre_y_},
                     gui::UiPoint{ctx.pointer_x(), ctx.pointer_y()}, 1.0F,
                     ctx.theme().gizmo_view);
            }
        }
    }
    if (keyboard_.active) {
        draw_keyboard_transform(ctx, list, frame.image, scene, keyboard_centre,
                                keyboard_reading);
    }
    if (measuring_ && role_ == ViewportRole::Stage && projection == nullptr) {
        draw_measurements(ctx, list, frame.image, scene, measure_,
                          measure_hover_);
    }
    if (marquee_active_) {
        const float x0 = std::min(marquee_x_, ctx.pointer_x());
        const float y0 = std::min(marquee_y_, ctx.pointer_y());
        const float x1 = std::max(marquee_x_, ctx.pointer_x());
        const float y1 = std::max(marquee_y_, ctx.pointer_y());
        list.add_rect(x0, y0, x1 - x0, y1 - y0,
                      gui::UiColor{0.35F, 0.62F, 1.0F, 0.12F});
        const gui::UiColor edge{0.45F, 0.70F, 1.0F, 0.95F};
        line(list, gui::UiPoint{x0, y0}, gui::UiPoint{x1, y0}, 1.0F, edge);
        line(list, gui::UiPoint{x1, y0}, gui::UiPoint{x1, y1}, 1.0F, edge);
        line(list, gui::UiPoint{x1, y1}, gui::UiPoint{x0, y1}, 1.0F, edge);
        line(list, gui::UiPoint{x0, y1}, gui::UiPoint{x0, y0}, 1.0F, edge);
    }
    if (drag_snap_target_.has_value()) {
        draw_measurements(ctx, list, frame.image, scene, MeasureChain{},
                          drag_snap_target_, false);
    }
    ctx.pop_clip();
    draw_stage_view_menu(ctx, view_menu, frame);
    draw_stage_shading_menu(ctx, shading_menu, frame);

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
    // The mask tools own the image's top-right corner (COVERAGE).
    drawing_masks_ = true;
    const ViewportFrame ordinary = draw(
        ctx, bounds, render_target, scene, {}, {}, camera_overlay,
        &ordinary_interaction_block, frames_per_second, nullptr, {},
        projection);
    drawing_masks_ = false;
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
    // The image's top-right corner, 5 in.
    gui::Rect corner = gui::inset(frame.image, gui::Padding{0.0F, 5.0F, 5.0F, 0.0F});
    corner = gui::cut_top(corner, 21.0F);
    const gui::Rect coverage_button = gui::cut_right(corner, 101.0F);
    const gui::Interaction coverage_interaction =
        ctx.grab(gui::ui_id(id_, 0x4D41534BU), coverage_button);
    const gui::Rect mask_interaction_bounds =
        !frame.mapping_valid || coverage_interaction.hovered
        ? gui::Rect{}
        : frame.image;
    const gui::Interaction interaction =
        ctx.grab(gui::ui_id(id_, 0x4D41534CULL), mask_interaction_bounds);
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
        draw_mask_path(list, path, 4.5F, overlay_halo);
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
                    gui::add_diamond(list, rotate_screen, 5.0F, mask_handle);
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
        const gui::Rect notice{frame.image.x + 8.0F, frame.image.y + 8.0F,
                               244.0F, 23.0F};
        ctx.panel(notice, gui::UiColor{0.12F, 0.04F, 0.04F, 0.90F});
        ctx.label(gui::inset(notice, 6.0F, 0.0F),
                  "SOURCE MASK MAPPING UNAVAILABLE", ctx.theme().warning);
    }
    // A Masks tab changes what clicking the picture means. Keep that mode
    // visible in both Stage and Program Output instead of making a failed
    // object-selection click feel like a broken picker.
    constexpr std::string_view selection_notice = "Can't select while masking";
    // Centred along the image's foot, 6 above it, sized to its words.
    gui::Rect foot = gui::inset(frame.image, gui::Padding{8.0F, 0.0F, 8.0F, 6.0F});
    foot = gui::cut_bottom(foot, 23.0F);
    const gui::Rect selection_notice_bounds = gui::centered(
        foot, std::min(foot.width, std::ceil(ctx.font_width(selection_notice)) + 16.0F),
        foot.height);
    ctx.panel(selection_notice_bounds,
              gui::UiColor{0.03F, 0.04F, 0.05F, 0.90F});
    ctx.label(gui::inset(selection_notice_bounds, 8.0F, 0.0F),
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
