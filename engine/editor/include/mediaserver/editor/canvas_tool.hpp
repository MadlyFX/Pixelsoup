#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <optional>
#include <span>
#include <string_view>

// The tools of the Program view's palette.
//
// A tool is one entry here - its mark, its name, its key and what it is for -
// and one branch of the viewport's pointer handling. The palette, the
// shortcuts and the tooltips are all read from this table, so adding a tool
// (a shape, a pen, later a tracker) is adding an entry and its gesture, never
// another button drawn by hand.
namespace mediaserver::editor {

enum class CanvasTool : core::u8 {
    // Pick layers and move, turn and resize them by their box.
    Select,
    // Drag the picture about inside the panel.
    Hand,
    // Click to zoom in on a point, Alt+click to zoom out.
    Zoom,
    // Drag out a rectangle or an ellipse; click out a closed curve. On a
    // selected layer the shape is a mask of it. With nothing selected it is a
    // new Background card of that shape.
    Rectangle,
    Ellipse,
    Pen,
    // Drag a Background's gradient about on the picture: its centre, its
    // far end, or a fresh line from where it starts to where it ends.
    Gradient,
};

// The tools that draw a shape.
[[nodiscard]] constexpr bool draws_shape(CanvasTool tool) noexcept {
    return tool == CanvasTool::Rectangle || tool == CanvasTool::Ellipse ||
           tool == CanvasTool::Pen;
}

struct CanvasToolDescriptor final {
    CanvasTool tool{CanvasTool::Select};
    gui::Icon icon{gui::Icon::Select};
    // Sentence case, as the palette's tooltip says it.
    std::string_view name;
    // What the tool does, after its name in the tooltip.
    std::string_view hint;
    // The lower-case key that chooses it while the pointer is over the view.
    char shortcut{};
    // Whether the tool changes the document. A view that may not author (a
    // running transition, a rehearsal lock) still offers the tools that only
    // move the view.
    bool authors{};
};

// Every tool, in palette order.
[[nodiscard]] std::span<const CanvasToolDescriptor> canvas_tools() noexcept;
[[nodiscard]] const CanvasToolDescriptor& canvas_tool(CanvasTool tool) noexcept;

// The tool a typed key chooses, upper or lower case; empty for any other key.
[[nodiscard]] std::optional<CanvasTool>
canvas_tool_for_shortcut(std::string_view typed) noexcept;

// The palette is one column of keys this wide, down the view's left edge.
inline constexpr float canvas_tool_palette_width = 36.0F;

// Draws the palette into `bounds` with `active` lit. Returns the tool
// clicked this frame. While `authoring_locked`, tools that author are drawn
// but refuse, and their tooltip says why.
[[nodiscard]] std::optional<CanvasTool>
draw_canvas_tool_palette(gui::UiContext& ctx, gui::UiId id,
                         const gui::Rect& bounds, CanvasTool active,
                         bool authoring_locked = false);

}  // namespace mediaserver::editor
