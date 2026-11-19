#pragma once

#include <mediaserver/core/types.hpp>

#include <cstddef>
#include <optional>
#include <vector>

// The operator's laser pointer as the wall shows it: a dot and the strokes
// drawn while L is held in the editor, in pixels of the authored canvas.
//
// It is transient on purpose. It never enters the document, a saved project
// or a digest; the Director sends the newest one to every node beside the
// frame stream, stamped with the control frame it belongs to, and each node
// draws it over its own outputs. Canvas pixels rather than output pixels, so
// one message means the same place on every node: a canvas pixel is a line of
// sight from the scene camera, and every output view looks from that eye.
namespace mediaserver::show {

struct PointerPoint final {
    float x{};
    float y{};

    bool operator==(const PointerPoint&) const = default;
};

// The most a message may carry. The editor stops a stroke growing when it
// reaches either, so a node never has to draw more than this.
inline constexpr std::size_t max_pointer_points = 4096U;
inline constexpr std::size_t max_pointer_strokes = 256U;
// How far past the canvas a point may sit, in canvas pixels. A stroke runs off
// the picture freely; a number past this is a fault, not a stroke.
inline constexpr float max_pointer_coordinate = 1'000'000.0F;

// How the pointer looks, the same on the wall and in the editor: a laser-red
// dot and lines, sized against the canvas height so they read alike on any
// wall. The editor's Theme carries the same red.
inline constexpr float pointer_red_r = 1.0F;
inline constexpr float pointer_red_g = 0.16F;
inline constexpr float pointer_red_b = 0.12F;
inline constexpr float pointer_dot_radius_per_canvas_height = 1.0F / 110.0F;
inline constexpr float pointer_line_width_per_canvas_height = 1.0F / 240.0F;

struct PointerOverlay final {
    // The control frame this picture is for: a node shows it from that frame
    // on, so neighbouring nodes change it on the same frame.
    core::u64 control_frame{};
    // Where the dot is. Empty while the pointer is off the picture, and after
    // L is let go, which is how the wall is cleared.
    std::optional<PointerPoint> dot;
    // Every stroke's points one after another; `stroke_ends[i]` is one past
    // the last point of stroke i. A stroke of one point is a dab.
    std::vector<PointerPoint> points;
    std::vector<core::u32> stroke_ends;

    [[nodiscard]] bool empty() const noexcept {
        return !dot.has_value() && stroke_ends.empty();
    }

    bool operator==(const PointerOverlay&) const = default;
};

// True when every coordinate is finite and within bounds, the strokes are
// within budget, and each stroke has at least one point.
[[nodiscard]] bool valid(const PointerOverlay& overlay) noexcept;

// The pointer pictures a node has received, held until their frames come.
//
// The Director stamps a picture with the frame it authored it on, which is
// ahead of the frame a node is presenting. A node that showed each picture on
// arrival would change it a frame or two away from its neighbour, splitting
// a moving dot across a seam; this holds it until its frame. A few are kept,
// newest wins past that, because a picture nobody presented is worth nothing.
class PointerOverlayChannel final {
  public:
    static constexpr std::size_t capacity = 8U;

    // Takes a picture. One no newer than the newest held is refused: the
    // stream is ordered, so it can only be a resend.
    bool accept(PointerOverlay overlay);

    // The newest picture whose frame has come by `control_frame`, dropping
    // the ones it replaces. Empty before the first picture's frame.
    [[nodiscard]] const PointerOverlay* at(core::u64 control_frame);

    void clear() noexcept {
        pending_.clear();
        current_.reset();
    }

  private:
    std::vector<PointerOverlay> pending_;
    std::optional<PointerOverlay> current_;
};

} // namespace mediaserver::show
