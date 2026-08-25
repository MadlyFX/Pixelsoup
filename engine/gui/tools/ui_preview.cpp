#include <mediaserver/gui/dock.hpp>
#include <mediaserver/gui/input.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/gui/ui_renderer.hpp>
#include <mediaserver/gui/window.hpp>
#include <mediaserver/rhi/device.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// A runnable shell for the bespoke UI: the one part of the GUI module that
// cannot be asserted in a headless suite, since "a window opened, the panels
// rearranged, and the frame reached the screen" is only true on a machine with
// a display.
//
// It is also the shape the operator editor will take - a menu bar over docked
// panels the operator drags around - so it doubles as the target the ImGui
// panels get ported onto, one at a time, from GUI-4 on. Nothing here reaches
// into the engine yet: the viewport is a stand-in until the scene renderer's
// texture is composited into it.
namespace {

namespace core = mediaserver::core;
namespace gui = mediaserver::gui;
namespace rhi = mediaserver::rhi;

const gui::PanelId panel_viewport = gui::ui_id("panel.viewport");
const gui::PanelId panel_inspector = gui::ui_id("panel.inspector");
const gui::PanelId panel_timeline = gui::ui_id("panel.timeline");
const gui::PanelId panel_assets = gui::ui_id("panel.assets");

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
    const gui::NodeId hint = tree.leaf(gui::LayoutStyle{.width = gui::Size::grow()});
    const gui::NodeId add = tree.leaf(gui::LayoutStyle{
        .width = gui::Size::fixed(90.0F), .height = gui::Size::grow()});
    const gui::NodeId remove = tree.leaf(gui::LayoutStyle{
        .width = gui::Size::fixed(90.0F), .height = gui::Size::grow()});
    tree.end();
    tree.solve(bounds);

    ctx.label(tree.rect(title), "MEDIA SERVER", ctx.theme().text_muted);
    ctx.label(
        tree.rect(hint),
        "drag a panel tab to re-dock it, or onto the bar to float it",
        ctx.theme().text_muted);
    if (ctx.button(gui::ui_id("add layer"), tree.rect(add), "Add")) {
        ++model.clicks;
    }
    if (ctx.button(gui::ui_id("remove layer"), tree.rect(remove), "Remove")) {
        --model.clicks;
    }
}

// A stand-in for an engine render target, so the preview exercises the same
// path a real viewport does: an RHI texture sampled straight into the widget
// tree. A scene renderer's target goes here unchanged - it is an rhi::Texture
// either way, which is the point of the UI sharing the engine's device.
[[nodiscard]] std::optional<rhi::Texture> make_test_target(rhi::Device& device) {
    constexpr core::u32 size = 256U;
    rhi::TextureDesc desc{};
    desc.width = size;
    desc.height = size;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    auto created = device.create_texture(desc);
    if (!created) {
        return std::nullopt;
    }

    // A checkerboard with a coloured cross, so a stretch, a flip, or a wrong
    // sub-rectangle would all be obvious at a glance.
    std::vector<core::u8> pixels(
        static_cast<std::size_t>(size) * size * 4U, 0U);
    for (core::u32 y = 0U; y < size; ++y) {
        for (core::u32 x = 0U; x < size; ++x) {
            const bool light = ((x / 32U) + (y / 32U)) % 2U == 0U;
            const bool cross = x > size / 2U - 3U && x < size / 2U + 3U;
            const bool bar = y > size / 2U - 3U && y < size / 2U + 3U;
            const std::size_t at =
                (static_cast<std::size_t>(y) * size + x) * 4U;
            pixels[at + 0U] = cross ? 230U : (light ? 60U : 30U);
            pixels[at + 1U] = bar ? 190U : (light ? 70U : 34U);
            pixels[at + 2U] = light ? 90U : 44U;
            pixels[at + 3U] = 255U;
        }
    }

    auto acquired = device.acquire_commands();
    if (!acquired) {
        return std::nullopt;
    }
    auto commands = std::move(acquired).value();
    if (!commands.upload(created.value(), pixels)) {
        return std::nullopt;
    }
    auto submitted = device.submit(std::move(commands));
    if (!submitted || !std::move(submitted).value().wait()) {
        return std::nullopt;
    }
    return std::move(created).value();
}

void draw_viewport(
    gui::UiContext& ctx, const gui::Rect& bounds, const rhi::Texture* target) {
    ctx.panel(bounds, gui::UiColor{0.07F, 0.07F, 0.08F, 1.0F});
    if (target == nullptr) {
        return;
    }

    // Letterboxed to 16:9 and composited with no copy - the same call the
    // ported viewport panel makes with the scene renderer's target.
    const gui::Rect placed = ctx.image_fit(bounds, *target, 16.0F / 9.0F);
    ctx.label(
        gui::Rect{placed.x + 10.0F, placed.y + 8.0F, placed.width, 18.0F},
        "engine target",
        ctx.theme().text);
}

void draw_inspector(gui::UiContext& ctx, const gui::Rect& bounds, Model& model) {
    constexpr std::size_t row_count = 6U;
    gui::LayoutTree tree;
    std::array<Row, row_count> rows{};

    tree.begin(gui::LayoutStyle{
        .axis = gui::Axis::Column,
        .padding = gui::Padding::all(10.0F),
        .gap = 6.0F});
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
}

void draw_assets(gui::UiContext& ctx, const gui::Rect& bounds) {
    const std::array<const char*, 5U> items{
        {"stage.gltf", "loop_01.mp4", "grid.png", "cue_a.json", "wall_b.gltf"}};
    float y = bounds.y + 8.0F;
    for (const char* item : items) {
        const gui::Rect row{bounds.x + 10.0F, y, bounds.width - 20.0F, 20.0F};
        ctx.label(row, item);
        y += 22.0F;
    }
}

// The timeline is the UI's priority element - a layer list beside a time graph -
// so the preview sketches its skeleton rather than leaving the panel empty.
void draw_timeline(
    gui::UiContext& ctx, const gui::Rect& bounds, const Model& model) {
    constexpr float ruler_height = 22.0F;
    constexpr float track_width = 160.0F;
    constexpr float row_height = 24.0F;

    ctx.panel(bounds, gui::UiColor{0.11F, 0.11F, 0.13F, 1.0F});
    if (bounds.width <= track_width) {
        return;
    }

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

    // The playhead, parked where the opacity slider sits, so dragging a control
    // in another panel visibly moves something here.
    const float playhead = ruler.x + model.opacity * ruler.width;
    ctx.panel(
        gui::Rect{playhead, bounds.y, 2.0F, bounds.height},
        gui::UiColor{0.95F, 0.35F, 0.35F, 1.0F});
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

    // The starting arrangement: a viewport with an inspector beside it, the
    // assets tabbed behind the inspector, and the timeline across the bottom.
    // A fraction is the share the newcomer takes of what it is docked against.
    gui::DockLayout dock;
    const gui::DockId main = dock.add(panel_viewport, "Viewport");
    const gui::DockId side =
        dock.dock(panel_inspector, "Inspector", main, gui::DockSide::Right, 0.26F);
    dock.dock(panel_assets, "Assets", side, gui::DockSide::Center);
    dock.set_active(side, panel_inspector);
    dock.dock(panel_timeline, "Timeline", dock.root(), gui::DockSide::Bottom, 0.3F);

    gui::DockHost host{dock};

    auto target = make_test_target(window.device());
    if (!target.has_value()) {
        std::puts("could not create the viewport target");
    }

    const auto draw_panel = [&](gui::PanelId panel, const gui::Rect& bounds) {
        if (panel == panel_viewport) {
            draw_viewport(ctx, bounds, target.has_value() ? &target.value() : nullptr);
        } else if (panel == panel_inspector) {
            draw_inspector(ctx, bounds, model);
        } else if (panel == panel_assets) {
            draw_assets(ctx, bounds);
        } else if (panel == panel_timeline) {
            draw_timeline(ctx, bounds, model);
        }
    };

    while (window.pump(input)) {
        ctx.new_frame(window.time_seconds());

        const auto width = static_cast<float>(window.width());
        const auto height = static_cast<float>(window.height());
        constexpr float bar_height = 36.0F;

        draw_top_bar(ctx, gui::Rect{0.0F, 0.0F, width, bar_height}, model);
        host.update(
            ctx,
            gui::Rect{0.0F, bar_height, width, std::max(height - bar_height, 0.0F)},
            draw_panel);
        host.overlay(ctx);

        static_cast<void>(
            window.present(list, rhi::Color{0.06F, 0.06F, 0.07F, 1.0F}));
    }

    return 0;
}
