#include <mediaserver/editor/canvas_tool.hpp>

#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/rect_cut.hpp>

#include <array>
#include <string>

namespace mediaserver::editor {

namespace {

constexpr std::array<CanvasToolDescriptor, 7U> tools{{
    {CanvasTool::Select, gui::Icon::Select, "Select",
     "Click a layer to select it. Drag it to move it, a corner or edge of "
     "its box to resize it, and the handle above the box to turn it.",
     'v', true},
    {CanvasTool::Hand, gui::Icon::Hand, "Hand",
     "Drag to move the picture inside the panel. A middle drag does the "
     "same with any tool.",
     'h', false},
    {CanvasTool::Zoom, gui::Icon::ZoomTool, "Zoom",
     "Click to zoom in on a point, Alt+click to zoom out. The wheel zooms "
     "with any tool.",
     'z', false},
    {CanvasTool::Rectangle, gui::Icon::RectangleTool, "Rectangle",
     "Drag out a rectangle: a mask on the selected layer, or a new card "
     "when nothing is selected. Shift keeps it square, Alt draws from the "
     "middle.",
     'r', true},
    {CanvasTool::Ellipse, gui::Icon::EllipseTool, "Ellipse",
     "Drag out an ellipse: a mask on the selected layer, or a new card when "
     "nothing is selected. Shift keeps it round, Alt draws from the middle.",
     'e', true},
    {CanvasTool::Pen, gui::Icon::PenTool, "Pen",
     "Click to place points and drag to curve them. Click the first point "
     "or press Enter to close the shape; Backspace takes the last point "
     "back and Esc drops the shape.",
     'p', true},
    {CanvasTool::Gradient, gui::Icon::GradientTool, "Gradient",
     "On a selected Background with a gradient: drag its middle to move "
     "it, its end to turn and stretch it, or anywhere else to draw it "
     "afresh from start to end.",
     'g', true},
}};

constexpr float key_size = 28.0F;
constexpr float key_gap = 4.0F;

}  // namespace

std::span<const CanvasToolDescriptor> canvas_tools() noexcept { return tools; }

const CanvasToolDescriptor& canvas_tool(CanvasTool tool) noexcept {
    for (const CanvasToolDescriptor& entry : tools) {
        if (entry.tool == tool) {
            return entry;
        }
    }
    return tools.front();
}

std::optional<CanvasTool>
canvas_tool_for_shortcut(std::string_view typed) noexcept {
    for (const char key : typed) {
        const char lower =
            key >= 'A' && key <= 'Z' ? static_cast<char>(key - 'A' + 'a') : key;
        for (const CanvasToolDescriptor& entry : tools) {
            if (entry.shortcut == lower) {
                return entry.tool;
            }
        }
    }
    return std::nullopt;
}

std::optional<CanvasTool>
draw_canvas_tool_palette(gui::UiContext& ctx, gui::UiId id,
                         const gui::Rect& bounds, CanvasTool active,
                         bool authoring_locked) {
    const gui::Theme& theme = ctx.theme();
    ctx.panel(bounds, theme.header);
    ctx.panel(gui::Rect{bounds.right() - 1.0F, bounds.y, 1.0F, bounds.height},
              theme.divider);
    std::optional<CanvasTool> chosen;
    float y = bounds.y + key_gap;
    for (const CanvasToolDescriptor& entry : tools) {
        const gui::Rect key{bounds.x + (bounds.width - key_size) * 0.5F, y,
                            key_size, key_size};
        if (key.bottom() > bounds.bottom()) {
            break;
        }
        const bool refused = authoring_locked && entry.authors;
        std::string tooltip{entry.name};
        tooltip += " (";
        tooltip += static_cast<char>(entry.shortcut - 'a' + 'A');
        tooltip += "). ";
        tooltip += refused ? "Editing is locked while a transition runs."
                           : entry.hint;
        if (tool_key(ctx,
                     gui::ui_id(id, static_cast<core::u64>(entry.tool) + 1U),
                     key, entry.icon, entry.tool == active, tooltip,
                     theme.accent) &&
            !refused) {
            chosen = entry.tool;
        }
        y += key_size + key_gap;
    }
    return chosen;
}

}  // namespace mediaserver::editor
