#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/gui/ui_renderer.hpp>
#include <mediaserver/gui/window.hpp>

#include <array>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>

// A runnable shell for the bespoke UI: the one part of the GUI module that
// cannot be asserted in a headless suite, since "a window opened and the frame
// reached the screen" is only true on a machine with a display.
//
// It is also the shape the operator editor will take - a menu bar over a
// viewport, an inspector, and a timeline, divided by draggable seams - so it
// doubles as the target the ImGui panels get ported onto, one at a time, from
// GUI-4 on. Nothing here reaches into the engine yet: the viewport is a stand-in
// panel until the scene renderer's texture is composited into it.
namespace {

namespace gui = mediaserver::gui;
namespace rhi = mediaserver::rhi;

// The atlas needs a real font to carry glyphs. A stock Windows install has one
// of these; without any, the empty atlas still draws every solid fill, so the
// preview degrades to an unlabelled - but working - layout rather than nothing.
[[nodiscard]] gui::FontAtlas load_font() {
    constexpr std::array<const char*, 4U> candidates{
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/tahoma.ttf"};
    for (const char* path : candidates) {
        auto atlas = gui::FontAtlas::create(path, 15U);
        if (atlas.has_value()) {
            return std::move(atlas).value();
        }
    }
    std::puts("no system font found; drawing without text");
    return gui::FontAtlas::empty();
}

// One inspector row: a name on the left, a control filling the rest.
struct Row final {
    gui::NodeId label{};
    gui::NodeId field{};
};

// The state the preview edits. In the real editor these are scene properties;
// here they exist so every widget has something to move.
struct Model final {
    std::string name{"Wall A"};
    float opacity{0.8F};
    float position_x{0.0F};
    float position_y{0.0F};
    bool visible{true};
    bool locked{false};
    int clicks{};
};

void draw_top_bar(gui::UiContext& ctx, const gui::Rect& bounds, Model& model) {
    ctx.panel(bounds, gui::UiColor{0.10F, 0.10F, 0.12F, 1.0F});

    gui::LayoutTree tree;
    tree.begin(gui::LayoutStyle{
        .axis = gui::Axis::Row,
        .padding = gui::Padding{8.0F, 6.0F, 8.0F, 6.0F},
        .gap = 6.0F,
        .align = gui::Align::Stretch});
    const gui::NodeId title = tree.leaf(gui::LayoutStyle{
        .width = gui::Size::fixed(180.0F), .height = gui::Size::grow()});
    tree.leaf(gui::LayoutStyle{.width = gui::Size::grow()});
    const gui::NodeId add = tree.leaf(gui::LayoutStyle{
        .width = gui::Size::fixed(90.0F), .height = gui::Size::grow()});
    const gui::NodeId remove = tree.leaf(gui::LayoutStyle{
        .width = gui::Size::fixed(90.0F), .height = gui::Size::grow()});
    tree.end();
    tree.solve(bounds);

    ctx.label(tree.rect(title), "MEDIA SERVER", ctx.theme().text_muted);
    if (ctx.button(gui::ui_id("add layer"), tree.rect(add), "Add")) {
        ++model.clicks;
    }
    if (ctx.button(gui::ui_id("remove layer"), tree.rect(remove), "Remove")) {
        --model.clicks;
    }
}

void draw_viewport(gui::UiContext& ctx, const gui::Rect& bounds) {
    ctx.panel(bounds, gui::UiColor{0.07F, 0.07F, 0.08F, 1.0F});
    ctx.push_clip(bounds);

    // A stand-in for the scene target: the frame the wall would fill. When the
    // viewport panel is real this rectangle becomes the engine's texture.
    const float margin = 40.0F;
    const gui::Rect stage{
        bounds.x + margin,
        bounds.y + margin,
        bounds.width - margin * 2.0F,
        bounds.height - margin * 2.0F};
    if (stage.width > 0.0F && stage.height > 0.0F) {
        ctx.panel(stage, gui::UiColor{0.13F, 0.14F, 0.17F, 1.0F});
        ctx.label(
            gui::Rect{stage.x + 10.0F, stage.y + 8.0F, stage.width, 18.0F},
            "Viewport",
            ctx.theme().text_muted);
    }
    ctx.pop_clip();
}

void draw_inspector(gui::UiContext& ctx, const gui::Rect& bounds, Model& model) {
    ctx.panel(bounds);
    ctx.push_clip(bounds);

    constexpr std::size_t row_count = 6U;
    gui::LayoutTree tree;
    std::array<Row, row_count> rows{};

    tree.begin(gui::LayoutStyle{
        .axis = gui::Axis::Column,
        .padding = gui::Padding::all(10.0F),
        .gap = 6.0F});
    const gui::NodeId heading = tree.leaf(gui::LayoutStyle{
        .width = gui::Size::grow(), .height = gui::Size::fixed(20.0F)});
    for (Row& row : rows) {
        tree.begin(gui::LayoutStyle{
            .axis = gui::Axis::Row,
            .width = gui::Size::grow(),
            .height = gui::Size::fixed(22.0F),
            .gap = 8.0F});
        row.label = tree.leaf(gui::LayoutStyle{
            .width = gui::Size::fixed(64.0F), .height = gui::Size::grow()});
        row.field = tree.leaf(gui::LayoutStyle{
            .width = gui::Size::grow(), .height = gui::Size::grow()});
        tree.end();
    }
    tree.end();
    tree.solve(bounds);

    ctx.label(tree.rect(heading), "Inspector", ctx.theme().text_muted);

    ctx.label(tree.rect(rows[0].label), "Name");
    static_cast<void>(
        ctx.text_field(gui::ui_id("name"), tree.rect(rows[0].field), model.name));

    ctx.label(tree.rect(rows[1].label), "Opacity");
    static_cast<void>(ctx.slider(
        gui::ui_id("opacity"),
        tree.rect(rows[1].field),
        model.opacity,
        0.0F,
        1.0F));

    ctx.label(tree.rect(rows[2].label), "X");
    static_cast<void>(ctx.drag_float(
        gui::ui_id("pos x"), tree.rect(rows[2].field), model.position_x, 0.5F));

    ctx.label(tree.rect(rows[3].label), "Y");
    static_cast<void>(ctx.drag_float(
        gui::ui_id("pos y"), tree.rect(rows[3].field), model.position_y, 0.5F));

    static_cast<void>(ctx.checkbox(
        gui::ui_id("visible"), tree.rect(rows[4].field), model.visible, "Visible"));
    static_cast<void>(ctx.checkbox(
        gui::ui_id("locked"), tree.rect(rows[5].field), model.locked, "Locked"));

    ctx.pop_clip();
}

// The timeline is the UI's priority element - a layer list beside a time graph -
// so the preview sketches its skeleton rather than leaving the pane empty.
void draw_timeline(gui::UiContext& ctx, const gui::Rect& bounds, const Model& model) {
    ctx.panel(bounds, gui::UiColor{0.11F, 0.11F, 0.13F, 1.0F});
    ctx.push_clip(bounds);

    constexpr float ruler_height = 22.0F;
    constexpr float track_width = 160.0F;
    constexpr float row_height = 24.0F;

    const gui::Rect ruler{
        bounds.x + track_width, bounds.y, bounds.width - track_width, ruler_height};
    ctx.panel(ruler, gui::UiColor{0.09F, 0.09F, 0.11F, 1.0F});

    // Tick marks every second of a nominal ten-second span.
    constexpr int ticks = 10;
    for (int tick = 0; tick <= ticks; ++tick) {
        const float t = static_cast<float>(tick) / static_cast<float>(ticks);
        ctx.panel(
            gui::Rect{ruler.x + t * ruler.width, ruler.y, 1.0F, ruler.height},
            ctx.theme().text_muted);
    }

    const std::array<const char*, 3U> layers{{"Wall A", "Video 1", "Cue Light"}};
    const std::array<gui::UiColor, 3U> bars{{
        gui::UiColor{0.20F, 0.55F, 0.95F, 1.0F},
        gui::UiColor{0.35F, 0.70F, 0.45F, 1.0F},
        gui::UiColor{0.85F, 0.60F, 0.25F, 1.0F},
    }};

    for (std::size_t index = 0U; index < layers.size(); ++index) {
        const float y =
            bounds.y + ruler_height + static_cast<float>(index) * row_height;
        const gui::Rect name{bounds.x + 8.0F, y, track_width - 8.0F, row_height};
        ctx.label(name, layers[index]);

        // A clip bar, inset a little further along for each layer, standing in
        // for the keyframed spans the real timeline will draw.
        const float start = 0.05F + static_cast<float>(index) * 0.12F;
        const gui::Rect bar{
            ruler.x + start * ruler.width,
            y + 4.0F,
            ruler.width * 0.45F,
            row_height - 8.0F};
        ctx.panel(bar, bars[index]);
    }

    // The playhead, parked where the opacity slider sits, so dragging the
    // inspector visibly moves something in another panel.
    const float playhead = ruler.x + model.opacity * ruler.width;
    ctx.panel(
        gui::Rect{playhead, bounds.y, 2.0F, bounds.height},
        gui::UiColor{0.95F, 0.35F, 0.35F, 1.0F});

    ctx.pop_clip();
}

} // namespace

int main() {
    gui::WindowConfig config{};
    config.title = "Media Server - bespoke UI preview";
    config.width = 1440U;
    config.height = 900U;

    auto created = gui::UiWindow::create(config);
    if (!created) {
        std::puts("could not open a window: no display or no usable GPU");
        return 1;
    }
    auto window = std::move(created).value();

    const gui::FontAtlas font = load_font();
    if (!window.set_atlas(font)) {
        std::puts("could not upload the glyph atlas");
        return 1;
    }

    gui::InputState input;
    gui::UiDrawList list;
    gui::UiContext ctx{input, list, font};

    Model model{};
    float inspector_split = 0.74F;
    float timeline_split = 0.68F;

    while (window.pump(input)) {
        ctx.new_frame(window.time_seconds());

        const gui::Rect screen{
            0.0F,
            0.0F,
            static_cast<float>(window.width()),
            static_cast<float>(window.height())};

        gui::LayoutTree frame;
        frame.begin(gui::LayoutStyle{.axis = gui::Axis::Column});
        const gui::NodeId top = frame.leaf(gui::LayoutStyle{
            .width = gui::Size::grow(), .height = gui::Size::fixed(36.0F)});
        const gui::NodeId body = frame.leaf(gui::LayoutStyle{
            .width = gui::Size::grow(), .height = gui::Size::grow()});
        frame.end();
        frame.solve(screen);

        draw_top_bar(ctx, frame.rect(top), model);

        // The seams first, so the panes they hand back are what gets filled.
        const gui::Split rows = ctx.split(
            gui::ui_id("timeline seam"),
            frame.rect(body),
            timeline_split,
            gui::Axis::Column);
        const gui::Split columns = ctx.split(
            gui::ui_id("inspector seam"),
            rows.first,
            inspector_split,
            gui::Axis::Row);

        draw_viewport(ctx, columns.first);
        draw_inspector(ctx, columns.second, model);
        draw_timeline(ctx, rows.second, model);

        static_cast<void>(
            window.present(list, rhi::Color{0.06F, 0.06F, 0.07F, 1.0F}));
    }

    return 0;
}
