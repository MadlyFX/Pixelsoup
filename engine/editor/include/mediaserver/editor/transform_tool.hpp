#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/math.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>

// The Stage's move, rotate and scale tools, as pure geometry.
//
// A transform gesture - a gizmo drag or a Blender-style G/R/S keyboard
// transform - is reported as one TransformDelta measured from where the
// gesture began, about a world pivot. The viewport panel produces it from
// pointer pixels; the shell resolves it into each selected element's authored
// position, yaw/pitch/roll and scale. Both halves live here, free of any
// window or document, so the arithmetic that decides where an element lands
// is tested without either.
namespace mediaserver::editor {

enum class TransformTool : core::u8 {
    Move,
    Rotate,
    Scale,
};

// The frame the gizmo and an axis constraint act in. Global is world X/Y/Z;
// Local is the primary element's own axes. Scale always acts on each
// element's own axes: a non-uniform scale along a world axis on a rotated
// element would need a shear the document cannot hold.
enum class TransformOrientation : core::u8 {
    Global,
    Local,
};

// Increment snapping, as Blender's: the snap toggle (or holding Ctrl to
// invert it) rounds a gesture's change, not the element's absolute value, so a
// part that sits off the grid keeps its offset. Holding Shift as well uses
// the fine step.
inline constexpr float move_snap_step = 1.0F;
inline constexpr float move_snap_fine_step = 0.1F;
inline constexpr float rotate_snap_step_turns = 5.0F / 360.0F;
inline constexpr float rotate_snap_fine_step_turns = 1.0F / 360.0F;
inline constexpr float scale_snap_step = 0.1F;
inline constexpr float scale_snap_fine_step = 0.01F;

// `value` rounded to the nearest multiple of `step`; unchanged for a
// non-positive step.
[[nodiscard]] float snap_to_step(float value, float step) noexcept;

// One gesture's change to the selection, from where it began.
struct TransformDelta final {
    TransformTool tool{TransformTool::Move};
    // Move: the world-space translation.
    scene::Vec3 translation{};
    // Rotate: a world unit axis through `pivot`, and right-handed turns.
    scene::Vec3 axis{0.0F, 1.0F, 0.0F};
    float turns{};
    // Scale: a factor along each world unit axis of `basis`, about `pivot`.
    std::array<scene::Vec3, 3U> basis{scene::Vec3{1.0F, 0.0F, 0.0F},
                                      scene::Vec3{0.0F, 1.0F, 0.0F},
                                      scene::Vec3{0.0F, 0.0F, 1.0F}};
    scene::Vec3 factors{1.0F, 1.0F, 1.0F};
    scene::Vec3 pivot{};

    bool operator==(const TransformDelta&) const = default;
};

// What a gesture needs to know about one element when it begins.
struct TransformStart final {
    // The element's authored position, in its parent's space.
    scene::Vec3 local_position{};
    // Authored yaw/pitch/roll turns and scale, as evaluated at the frame the
    // gesture began.
    scene::Vec3 rotation{};
    scene::Vec3 scale{1.0F, 1.0F, 1.0F};
    // Where the element is drawn, and its drawn axes (unit length).
    scene::Vec3 world_position{};
    std::array<scene::Vec3, 3U> world_axes{scene::Vec3{1.0F, 0.0F, 0.0F},
                                           scene::Vec3{0.0F, 1.0F, 0.0F},
                                           scene::Vec3{0.0F, 0.0F, 1.0F}};
    // Maps a drawn world vector into the authored parent space.
    scene::Mat4 world_to_local{scene::identity()};

    bool operator==(const TransformStart&) const = default;
};

struct TransformResult final {
    scene::Vec3 position{};
    scene::Vec3 rotation{};
    scene::Vec3 scale{1.0F, 1.0F, 1.0F};

    bool operator==(const TransformResult&) const = default;
};

// Resolves a gesture into one element's new authored values. Every element
// orbits or scales about the shared pivot, as a Blender selection does about
// its median point; rotation is re-expressed as the yaw/pitch/roll nearest
// the element's starting angles so keyed rotation interpolates the short way.
[[nodiscard]] TransformResult apply_transform(const TransformStart& start,
                                              const TransformDelta& delta) noexcept;

// The same gesture applied to a whole world matrix, for an element placed by
// one (a Blender-linked object): the axes turn, or each of the element's own
// axes stretches by the factor apply_transform gives it, and the origin orbits
// or spreads about the pivot.
[[nodiscard]] scene::Mat4 transformed_world(const scene::Mat4& world,
                                            const TransformDelta& delta) noexcept;

// The yaw/pitch/roll turns `scene::rotation` would need to produce the unit
// basis `axes` (its columns), choosing whichever of the two equivalent
// solutions, and whichever whole-turn offset, lies nearest `near`.
[[nodiscard]] scene::Vec3
rotation_turns_of(const std::array<scene::Vec3, 3U>& axes,
                  scene::Vec3 near = {}) noexcept;

// The world direction through canvas pixel (x, y) for a symmetric camera:
// the inverse of `scene::project_to_canvas` up to depth. Not normalised.
[[nodiscard]] scene::Vec3 pointer_ray(const scene::Camera& camera,
                                      scene::Canvas canvas, float canvas_x,
                                      float canvas_y) noexcept;

// The translation that keeps a point of the plane through `origin` (normal
// `normal`) under the pointer as it moves from `from` to `to` canvas pixels.
// Empty when either ray misses the plane, or meets it nearly edge-on, where
// a small pointer move would throw the element towards the horizon.
[[nodiscard]] std::optional<scene::Vec3>
plane_drag_translation(const scene::Camera& camera, scene::Canvas canvas,
                       scene::Vec3 origin, scene::Vec3 normal, float from_x,
                       float from_y, float to_x, float to_y) noexcept;

// The signed turns that rotate `axis` so the pointer's sweep around `centre`
// from `from` to `to` (pixels, y down) is matched on screen. `view_forward`
// says which way the axis faces the camera.
[[nodiscard]] float sweep_turns(float centre_x, float centre_y, float from_x,
                                float from_y, float to_x, float to_y,
                                scene::Vec3 axis,
                                scene::Vec3 view_forward) noexcept;

// Blender's typed value during a keyboard transform: digits, one '.', and a
// leading '-' that Minus toggles anywhere in the entry.
class NumericEntry final {
  public:
    // Applies typed characters; anything else is ignored. Returns whether the
    // entry changed.
    bool type(std::string_view text);
    void backspace() noexcept;
    void clear() noexcept { text_.clear(); negative_ = false; }
    [[nodiscard]] bool active() const noexcept {
        return !text_.empty() || negative_;
    }
    [[nodiscard]] std::optional<float> value() const noexcept;
    // The entry as shown to the operator, sign included.
    [[nodiscard]] std::string display() const;

  private:
    std::string text_;
    bool negative_{};
};

} // namespace mediaserver::editor
