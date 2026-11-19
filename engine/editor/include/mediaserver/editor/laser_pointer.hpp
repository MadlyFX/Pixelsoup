#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/show/pointer_overlay.hpp>

#include <optional>
#include <span>
#include <vector>

// The laser pointer: hold L in the editor and a dot follows the pointer over
// the Stage and Program views, and over the wall. A left drag draws a line
// that goes when L is let go; a right drag draws one that stays, for the
// operator only, until L is pressed again.
//
// Nothing here is authored. The editor keeps these marks; while L is held the
// Director also sends them to the nodes as a transient picture (see
// show::PointerOverlay), and letting go clears the wall.
namespace mediaserver::editor {

// L let go sooner than this, with nothing drawn, was a tap: the Timeline's
// play-forward key rather than the pointer. The wall is sent nothing until a
// hold outlasts it, so a tap never flashes a dot on the LED wall.
inline constexpr double laser_tap_seconds = 0.25;

// What a stroke is drawn with: the left button's, gone when L is let go, or
// the right button's, kept on the operator's screen.
enum class LaserInk : core::u8 {
    Fleeting,
    Kept,
};

// Where a mark lives. On the Program view, a pixel of the authored canvas
// (z unused); on the Stage, a point in the scene, so the mark stays on what
// it was drawn over however the view moves.
enum class LaserSpace : core::u8 {
    Canvas,
    World,
};

struct LaserMark final {
    LaserSpace space{LaserSpace::Canvas};
    scene::Vec3 at{};

    bool operator==(const LaserMark&) const = default;
};

struct LaserStroke final {
    LaserInk ink{LaserInk::Fleeting};
    LaserSpace space{LaserSpace::Canvas};
    std::vector<scene::Vec3> points;

    bool operator==(const LaserStroke&) const = default;
};

class LaserPointer final {
  public:
    // L went down: the pointer is on, and the kept marks from last time go.
    void press();
    // L came up: the pointer is off, the dot and the left button's lines go,
    // and the right button's lines stay on the operator's screen.
    void release();
    [[nodiscard]] bool active() const noexcept { return active_; }

    // Where the dot is, or nothing while the pointer is off every view.
    void point_at(std::optional<LaserMark> dot);
    [[nodiscard]] const std::optional<LaserMark>& dot() const noexcept {
        return dot_;
    }

    // Starts a stroke at `at`. Refused while the pointer is off, while a
    // stroke is already being drawn, or once the strokes are full.
    bool begin_stroke(LaserInk ink, LaserMark at);
    // Adds a point to the stroke being drawn. Refused when none is, or when
    // the marks are full: the stroke simply stops growing.
    bool extend(scene::Vec3 at);
    void end_stroke() noexcept { drawing_ = false; }
    [[nodiscard]] bool drawing() const noexcept { return drawing_; }
    // The stroke being drawn is in this space; meaningful while drawing().
    [[nodiscard]] LaserSpace drawing_space() const noexcept;

    [[nodiscard]] std::span<const LaserStroke> strokes() const noexcept {
        return strokes_;
    }
    // Every point of every stroke, the budget a wall picture is held to.
    [[nodiscard]] std::size_t point_count() const noexcept {
        return point_count_;
    }
    // The strokes reached the most a picture may carry; drawing stops.
    [[nodiscard]] bool full() const noexcept;
    // Counts every visible change, so a caller can tell a picture moved.
    [[nodiscard]] core::u64 revision() const noexcept { return revision_; }

  private:
    bool active_{};
    bool drawing_{};
    std::optional<LaserMark> dot_;
    std::vector<LaserStroke> strokes_;
    std::size_t point_count_{};
    core::u64 revision_{};
};

// The picture the wall shows: while L is held, the dot and every stroke in
// pixels of the authored canvas, scene marks carried there through `camera`.
// A stroke that passes behind the camera is broken where it does. Empty once
// L is let go, which is how the wall is cleared. The control frame is left
// for the Director to stamp.
[[nodiscard]] show::PointerOverlay
wall_pointer_overlay(const LaserPointer& pointer, const scene::Camera& camera,
                     scene::Canvas canvas);

} // namespace mediaserver::editor
