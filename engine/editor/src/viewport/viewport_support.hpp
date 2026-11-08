#pragma once

// Helpers the viewport panel's sources share: overlay drawing, projected
// feed-map surfaces, gizmo axis arithmetic and the Stage toolbar's menu
// geometry. Internal to the editor library.

#include <mediaserver/editor/gizmo.hpp>
#include <mediaserver/editor/measurement.hpp>
#include <mediaserver/editor/viewport_panel.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/manipulate.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace mediaserver::editor::viewport {

inline constexpr float stage_toolbar_height = 29.0F;
inline constexpr float surface_hit_pixels = 8.0F;
inline constexpr double deep_select_hold_seconds = 0.45;
inline constexpr float deep_select_motion_pixels = 4.0F;

inline constexpr gui::UiColor surface_color{0.16F, 0.68F, 0.88F, 0.68F};
inline constexpr gui::UiColor surface_selected{0.20F, 0.92F, 1.0F, 1.0F};
inline constexpr gui::UiColor surface_fallback{1.0F, 0.62F, 0.18F, 0.92F};
inline constexpr gui::UiColor surface_yaw{1.0F, 0.74F, 0.22F, 1.0F};

struct SurfaceVisual final {
    std::vector<scene::Vec3> vertices;
    std::vector<std::array<std::size_t, 2U>> edges;
    bool fallback{};
};

struct ProjectedSurface final {
    const output::ProjectionSurface* surface{};
    output::ProjectionSurface display;
    SurfaceVisual visual;
    std::vector<gui::UiPoint> points;
    std::vector<bool> visible;
};

[[nodiscard]] bool selected(std::span<const doc::NodeId> selection,
                            doc::NodeId node) noexcept;

[[nodiscard]] scene::Canvas viewport_canvas(
    const scene::Scene& view,
    const ViewportProjection* projection) noexcept;

// Plane and Quad are flat: their box has no depth, so a screen measures and
// rests as the screen rather than as a cube.
[[nodiscard]] scene::ModelLocalBounds
primitive_local_bounds(scene::Primitive primitive) noexcept;

void line(gui::UiDrawList& list, gui::UiPoint a, gui::UiPoint b,
          float thickness, gui::UiColor color);

void wire_box(gui::UiDrawList& list, const gui::Rect& image,
              const scene::Scene& view, const scene::Mat4& world,
              gui::UiColor color,
              scene::ModelLocalBounds bounds = scene::ModelLocalBounds{},
              const ViewportProjection* projection = nullptr);

// The floor grid follows the view: centred under what the camera looks at,
// spaced by `stage_grid_step` with a brighter line every ten, and the world
// X (red) and Z (blue) axes wherever they cross it - or, in a Front or Side
// view, the upright grid with Y (green). Each line is drawn in
// pieces so the part in front of the camera still shows when the camera
// stands on the grid. Returns the spacing, for the readout.
float draw_grid(gui::UiDrawList& list, const gui::Rect& image,
                const scene::Scene& view);

void draw_camera_overlay(gui::UiDrawList& list, const gui::Rect& image,
                         const scene::Scene& view,
                         const scene::Camera& camera, bool selected = false);

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
                                 const scene::TrackedCameraView& camera);

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
                                const ViewportPlate& entry);

void draw_light_overlays(gui::UiContext& ctx, gui::UiDrawList& list, const gui::Rect& image,
                         const scene::Scene& view,
                         std::span<const doc::NodeId> selection,
                         doc::NodeId primary);

void draw_selection_outlines(gui::UiDrawList& list, const gui::Rect& image,
                             const scene::Scene& view,
                             std::span<const doc::NodeId> selection,
                             doc::NodeId primary,
                             const scene::ModelBoundsProvider& model_bounds,
                             const ViewportProjection* projection = nullptr,
                             std::optional<gui::UiColor> tint = std::nullopt);

// The outline of `surface` in the world. `mesh` is its generated or imported
// geometry when one is resolved. A camera-anchored stage places the authored
// stage coordinates first; a generated mesh keeps its stage-space vertices in
// the cache and is carried vertex by vertex here.
[[nodiscard]] SurfaceVisual
surface_visual(const output::ProjectionSurface& surface,
               const output::ProjectionMesh* mesh,
               const std::optional<output::StagePose>& stage);

void project_surface(ProjectedSurface& surface, const gui::Rect& image,
                     const scene::Scene& view);

[[nodiscard]] float surface_hit_distance(const ProjectedSurface& surface,
                                         float pointer_x,
                                         float pointer_y) noexcept;

void draw_surface(gui::UiDrawList& list, const ProjectedSurface& surface,
                  bool selected_surface);

// A track's screen, outlined where its plane sits.
//
// Without this a track with nothing on it is an invisible plane: the operator
// makes one, the Inspector offers a transform, and the viewport shows nothing
// to aim it at. The clips playing on it draw the picture; this draws the edge
// of the thing they are playing on, which is the part that stays true when
// the track is empty, muted, or between clips.
void draw_screen(gui::UiDrawList& list, const gui::Rect& image,
                 const scene::Scene& view, const scene::SceneScreen& screen,
                 bool selected_screen);

[[nodiscard]] bool hit_yaw_ring(const GizmoGeometry& gizmo, float pointer_x,
                                float pointer_y) noexcept;

void draw_yaw_ring(gui::UiDrawList& list, const gui::Theme& theme,
                   const GizmoGeometry& gizmo, bool hovered, bool active);

[[nodiscard]] bool is_xyz(GizmoAxis axis) noexcept;

[[nodiscard]] float component_of(scene::Vec3 v, std::size_t index) noexcept;

[[nodiscard]] scene::Vec3 with_component(scene::Vec3 v, std::size_t index,
                                         float value) noexcept;

[[nodiscard]] const char* axis_name(std::size_t index) noexcept;

// The Measure tool's picture: each segment with its length at its middle,
// the latest segment's axis parts and slope beneath, a path's total at its
// end, and what the pointer would snap to.
void draw_measurements(gui::UiContext& ctx, gui::UiDrawList& list,
                       const gui::Rect& image, const scene::Scene& view,
                       const MeasureChain& chain,
                       const std::optional<MeasurePoint>& hover,
                       bool hint_shown = true);

// The corner of a Stage image its view menu, `width` wide, occupies; empty
// when the image is too small to spare it.
gui::Rect stage_view_menu_bounds(const gui::Rect& image, float width) noexcept;

// The shading menu, `width` wide, left of the view menu; empty with it.
gui::Rect stage_shading_menu_bounds(const gui::Rect& view_menu,
                                    float width) noexcept;

[[nodiscard]] std::string_view stage_shading_name(render::ViewShading shading) noexcept;

[[nodiscard]] std::string_view stage_projection_name(StageProjection projection) noexcept;

// The width of a toolbar_menu showing `value`.
float toolbar_menu_width(const gui::UiContext& ctx, std::string_view value,
                         bool degrees = false) noexcept;

} // namespace mediaserver::editor::viewport
