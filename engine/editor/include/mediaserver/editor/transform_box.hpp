#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <array>

// The 2D transform box drawn round a selected layer in the Program view: the
// layer's own outline with a handle at each corner and edge, and a turn
// handle on a stalk above it. That is the box every editing and compositing
// tool draws, so an operator who has resized a layer anywhere else already
// knows this one.
//
// Geometry, hit-testing and drawing only, in panel pixels. Nothing here knows
// what the box surrounds: a caller hands over four projected corners, asks
// which handle the pointer is on, and resolves a drag with the arithmetic
// below. What that drag then means to a document lives in transform_tool.hpp.
namespace mediaserver::editor {

enum class TransformBoxHandle : core::u8 {
    None,
    TopLeft,
    Top,
    TopRight,
    Right,
    BottomRight,
    Bottom,
    BottomLeft,
    Left,
    Rotate,
};

// A point of the box as fractions across (u) and down (v) it: (0, 0) is the
// first corner, (1, 1) the third.
struct BoxPoint final {
    float u{};
    float v{};

    bool operator==(const BoxPoint&) const = default;
};

inline constexpr float transform_box_handle_size = 8.0F;
inline constexpr float transform_box_hit_pixels = 9.0F;
// How far above the top edge the turn handle stands.
inline constexpr float transform_box_stalk_pixels = 26.0F;
// An edge shorter than this keeps its corner handles and gives up the one in
// its middle, which would sit on top of them.
inline constexpr float transform_box_edge_minimum = 34.0F;

struct TransformBox final {
    // False for a box with no area on screen: nothing is drawn and nothing
    // is hit.
    bool visible{};
    // A flat layer's own outline, whose sides resize independently. Otherwise
    // the box only bounds what it surrounds and resizes it as a whole.
    bool sides{};
    // Top-left, top-right, bottom-right, bottom-left, as the layer has them:
    // a layer turned upside down has its top-left corner at the bottom right.
    std::array<gui::UiPoint, 4U> corners{};
    gui::UiPoint centre{};
    gui::UiPoint rotate{};
};

[[nodiscard]] TransformBox
transform_box(const std::array<gui::UiPoint, 4U>& corners,
              bool sides) noexcept;

[[nodiscard]] bool is_corner(TransformBoxHandle handle) noexcept;
[[nodiscard]] bool is_edge(TransformBoxHandle handle) noexcept;
// Where a corner or edge handle sits on the unit box, and the handle across
// the box from it: what a resize holds still.
[[nodiscard]] BoxPoint handle_point(TransformBoxHandle handle) noexcept;
[[nodiscard]] TransformBoxHandle opposite(TransformBoxHandle handle) noexcept;
// A handle's position in panel pixels.
[[nodiscard]] gui::UiPoint handle_position(const TransformBox& box,
                                           TransformBoxHandle handle) noexcept;

// The handle under the pointer: the turn handle, then corners, then edges.
[[nodiscard]] TransformBoxHandle hit_transform_box(const TransformBox& box,
                                                   float pointer_x,
                                                   float pointer_y) noexcept;

// The cursor a handle shows: the resize arrow nearest the direction it pulls
// in on screen, so a turned layer's handles still point the way they move.
[[nodiscard]] gui::Cursor handle_cursor(const TransformBox& box,
                                        TransformBoxHandle handle) noexcept;

// How much a resize stretches the box across and down it.
struct BoxScale final {
    float across{1.0F};
    float down{1.0F};

    bool operator==(const BoxScale&) const = default;
};

// The smallest a resize makes a side, as a share of what it was. A drag past
// the anchor stops here rather than turning the layer inside out.
inline constexpr float transform_box_minimum_scale = 0.01F;

// Resolves a resize. `handle` is the handle held, `anchor` the point of the
// box that stays still, and `travel` how far the pointer has moved since the
// press, all in the units of the box as it stood at the press. An edge
// handle stretches one way only. `uniform` keeps a corner drag's proportions,
// taking whichever direction the pointer has gone further in.
[[nodiscard]] BoxScale resolve_box_scale(TransformBoxHandle handle,
                                         BoxPoint anchor, BoxPoint travel,
                                         bool uniform) noexcept;

void draw_transform_box(gui::UiDrawList& list, const gui::Theme& theme,
                        const TransformBox& box, TransformBoxHandle hovered,
                        TransformBoxHandle active);

}  // namespace mediaserver::editor
