#include "viewport_support.hpp"

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

namespace mediaserver::editor::viewport {

namespace {

constexpr float surface_yaw_radius_pixels = 49.0F;

constexpr float surface_yaw_hit_pixels = 7.0F;

constexpr std::size_t maximum_imported_wire_triangles = 4096U;

constexpr gui::UiColor outline_secondary{0.18F, 0.72F, 0.92F, 0.92F};

constexpr gui::UiColor outline_primary{0.20F, 0.92F, 1.0F, 1.0F};

constexpr gui::UiColor light_color{1.0F, 0.78F, 0.24F, 0.92F};

// Track screens, in the hue the timeline and Inspector mark a track with.
constexpr gui::UiColor screen_color{0.906F, 0.596F, 0.400F, 0.55F};

constexpr gui::UiColor screen_selected{0.980F, 0.706F, 0.470F, 1.0F};

constexpr gui::UiColor screen_muted{0.549F, 0.529F, 0.510F, 0.45F};

} // namespace

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

namespace {

[[nodiscard]] scene::CanvasProjection project_world(
    const scene::Scene& view, const ViewportProjection* projection,
    scene::Vec3 point) {
    return projection != nullptr && projection->project
               ? projection->project(point)
               : scene::project_to_canvas(view.camera, view.canvas, point);
}

} // namespace

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

namespace {

template <std::size_t PointCount, std::size_t EdgeCount>
void wire_shape(gui::UiDrawList& list, const gui::Rect& image,
                const scene::Scene& view, const scene::Mat4& world,
                const std::array<scene::Vec3, PointCount>& points,
                const std::array<std::array<std::size_t, 2U>, EdgeCount>& edges,
                gui::UiColor color,
                const ViewportProjection* projection = nullptr,
                bool beyond_canvas_only = false) {
    std::array<gui::UiPoint, PointCount> projected{};
    const scene::Canvas canvas = viewport_canvas(view, projection);
    const auto canvas_width = static_cast<float>(canvas.width);
    const auto canvas_height = static_cast<float>(canvas.height);
    bool beyond = false;
    for (std::size_t index = 0U; index < points.size(); ++index) {
        const auto canvas_point = project_world(
            view, projection, scene::transform_point(world, points[index]));
        if (!canvas_point.visible) {
            return;
        }
        beyond = beyond || canvas_point.x < 0.0F || canvas_point.y < 0.0F ||
                 canvas_point.x > canvas_width ||
                 canvas_point.y > canvas_height;
        projected[index] = to_ui(image, canvas, canvas_point);
    }
    if (beyond_canvas_only && !beyond) {
        return;
    }
    for (const auto& edge : edges) {
        line(list, projected[edge[0U]], projected[edge[1U]], 5.0F,
             overlay_halo);
    }
    for (const auto& edge : edges) {
        line(list, projected[edge[0U]], projected[edge[1U]], 1.8F, color);
    }
}

} // namespace

void wire_box(gui::UiDrawList& list, const gui::Rect& image,
              const scene::Scene& view, const scene::Mat4& world,
              gui::UiColor color,
              scene::ModelLocalBounds bounds,
              const ViewportProjection* projection,
              bool beyond_canvas_only) {
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
    wire_shape(list, image, view, world, points, edges, color, projection,
               beyond_canvas_only);
}

namespace {

void wire_quad(gui::UiDrawList& list, const gui::Rect& image,
               const scene::Scene& view, const scene::Mat4& world, bool floor,
               gui::UiColor color,
               const ViewportProjection* projection = nullptr,
               bool beyond_canvas_only = false) {
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
    wire_shape(list, image, view, world, points, edges, color, projection,
               beyond_canvas_only);
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

} // namespace

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
                         const scene::Camera& camera, bool selected) {
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

namespace {

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

} // namespace

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
                             const ViewportProjection* projection,
                             std::optional<gui::UiColor> tint) {
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
        // A take is drawn as its plates, each its slot screen's picture:
        // choosing the take outlines every wall it is playing on, and
        // choosing one screen outlines that wall.
        const doc::NodeId chosen =
            selected(selection, clip.source_node)
                ? clip.source_node
                : (clip.placement != doc::NodeId{} &&
                           selected(selection, clip.placement)
                       ? clip.placement
                       : doc::NodeId{});
        if (chosen != doc::NodeId{}) {
            wire_quad(list, image, view, clip.world, false, outline(chosen),
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

void draw_offcanvas_outlines(gui::UiDrawList& list, const gui::Rect& image,
                             const scene::Scene& view,
                             const scene::ModelBoundsProvider& model_bounds,
                             const ViewportProjection* projection,
                             gui::UiColor color) {
    // Only what reaches past the canvas: a layer wholly on the picture is
    // seen in the picture itself.
    for (const auto& object : view.instances) {
        if (object.mesh.primitive == scene::Primitive::Quad ||
            object.mesh.primitive == scene::Primitive::Plane) {
            wire_quad(list, image, view, object.world,
                      object.mesh.primitive == scene::Primitive::Plane, color,
                      projection, true);
        } else {
            wire_box(list, image, view, object.world, color,
                     scene::ModelLocalBounds{}, projection, true);
        }
    }
    for (const auto& clip : view.media) {
        wire_quad(list, image, view, clip.world, false, color, projection,
                  true);
    }
    // Scattered models are boxed as their group, as selection outlines are.
    std::map<doc::NodeId, scene::ModelLocalBounds> batches;
    for (const auto& model : view.models) {
        const auto local = (model_bounds ? model_bounds(model) : std::nullopt)
                               .value_or(scene::ModelLocalBounds{});
        if (!model.motion_source.value()) {
            wire_box(list, image, view, model.world, color, local, projection,
                     true);
            continue;
        }
        auto [entry, added] = batches.try_emplace(model.source_node);
        auto& group = entry->second;
        for (const float x : {local.minimum.x, local.maximum.x}) {
            for (const float y : {local.minimum.y, local.maximum.y}) {
                for (const float z : {local.minimum.z, local.maximum.z}) {
                    const auto p =
                        scene::transform_point(model.world, {x, y, z});
                    if (added) {
                        group.minimum = group.maximum = p;
                        added = false;
                    }
                    group.minimum = {std::min(group.minimum.x, p.x),
                                     std::min(group.minimum.y, p.y),
                                     std::min(group.minimum.z, p.z)};
                    group.maximum = {std::max(group.maximum.x, p.x),
                                     std::max(group.maximum.y, p.y),
                                     std::max(group.maximum.z, p.z)};
                }
            }
        }
    }
    for (const auto& [id, bounds] : batches) {
        wire_box(list, image, view, scene::identity(), color, bounds,
                 projection, true);
    }
}

namespace {

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

} // namespace

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

} // namespace

// The Measure tool's picture: each segment with its length at its middle,
// the latest segment's axis parts and slope beneath, a path's total at its
// end, and what the pointer would snap to.
void draw_measurements(gui::UiContext& ctx, gui::UiDrawList& list,
                       const gui::Rect& image, const scene::Scene& view,
                       const MeasureChain& chain,
                       const std::optional<MeasurePoint>& hover,
                       bool hint_shown) {
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

} // namespace mediaserver::editor::viewport
