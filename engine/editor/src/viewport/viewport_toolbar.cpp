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

// The corner of a Stage image its view menu, `width` wide, occupies; empty
// when the image is too small to spare it.
gui::Rect stage_view_menu_bounds(const gui::Rect& image, float width) noexcept {
    if (image.width < width + 16.0F || image.height < 40.0F) {
        return gui::Rect{};
    }
    gui::Rect corner = gui::inset(image, 6.0F);
    corner = gui::cut_top(corner, 22.0F);
    return gui::cut_right(corner, width);
}

// The shading menu, `width` wide, left of the view menu; empty with it.
gui::Rect stage_shading_menu_bounds(const gui::Rect& view_menu,
                                    float width) noexcept {
    if (view_menu.width <= 0.0F) {
        return gui::Rect{};
    }
    return gui::Rect{view_menu.x - width - 4.0F, view_menu.y, width,
                     view_menu.height};
}

std::string_view stage_shading_name(render::ViewShading shading) noexcept {
    switch (shading) {
    case render::ViewShading::Unlit:
        return "Unlit";
    case render::ViewShading::LightsOnly:
        return "Lights only";
    case render::ViewShading::Wireframe:
        return "Wireframe";
    case render::ViewShading::UvChecker:
        return "UV checker";
    case render::ViewShading::WorldNormals:
        return "World normals";
    case render::ViewShading::Lit:
        break;
    }
    return "Lit";
}

std::string_view stage_projection_name(StageProjection projection) noexcept {
    switch (projection) {
    case StageProjection::Top:
        return "Top";
    case StageProjection::Front:
        return "Front";
    case StageProjection::Side:
        return "Side";
    case StageProjection::Perspective:
        break;
    }
    return "Perspective";
}

namespace {

// The Stage toolbar's buttons: 26 wide, 22 high, three below its top.
constexpr float toolbar_button = 26.0F;

constexpr float toolbar_group_gap = 9.0F;

gui::UiColor toolbar_ink(const gui::Theme& theme, float t) noexcept {
    return gui::UiColor{
        theme.text_muted.r + (theme.text.r - theme.text_muted.r) * t,
        theme.text_muted.g + (theme.text.g - theme.text_muted.g) * t,
        theme.text_muted.b + (theme.text.b - theme.text_muted.b) * t, 1.0F};
}

void draw_chevron(gui::UiDrawList& list, float x, float y, gui::UiColor color) {
    list.add_line(x - 3.0F, y - 1.5F, x, y + 1.5F, 1.0F, color);
    list.add_line(x, y + 1.5F, x + 3.0F, y - 1.5F, 1.0F, color);
}

} // namespace

// The width of a toolbar_menu showing `value`.
float toolbar_menu_width(const gui::UiContext& ctx, std::string_view value,
                         bool degrees) noexcept {
    return 23.0F + ctx.font_width(value) + (degrees ? 5.0F : 0.0F) + 19.0F;
}

namespace {

// A button that opens a menu, showing its icon and the current choice, so
// the setting is read without opening it. `quiet` greys a value that is not
// in force; `overlay` gives it a body of its own, for a menu that floats over
// the picture rather than sitting in the toolbar.
bool toolbar_menu(gui::UiContext& ctx, gui::UiId id, const gui::Rect& bounds,
                  gui::Icon icon, std::string_view value, bool open,
                  bool degrees = false, bool quiet = false,
                  bool overlay = false) {
    const gui::Theme& theme = ctx.theme();
    const gui::Interaction it = ctx.grab(id, bounds);
    ctx.cursor_for(it, gui::Cursor::Hand);
    if (overlay) {
        ctx.panel(bounds, gui::UiColor{0.035F, 0.040F, 0.052F, 0.80F});
    }
    if (it.held && !it.released) {
        ctx.panel(bounds, theme.widget_active);
    } else if (open || it.hovered) {
        ctx.panel(bounds, theme.widget_hover);
    }
    const bool lit = open || it.hovered;
    const float middle = bounds.y + bounds.height * 0.5F;
    gui::UiDrawList& list = ctx.draw_list();
    gui::draw_icon(list, icon, bounds.x + 12.0F, middle,
                   toolbar_ink(theme, lit ? 1.0F : (quiet ? 0.0F : 0.75F)));
    const gui::UiColor ink = quiet && !lit ? theme.text_muted : theme.text;
    const float text_width = ctx.font_width(value);
    ctx.label(gui::Rect{bounds.x + 23.0F, bounds.y, text_width + 2.0F,
                        bounds.height},
              value, ink);
    if (degrees) {
        // The atlas is ASCII, so the degree sign is drawn.
        const float ring_x = bounds.x + 23.0F + text_width + 1.5F;
        const float ring_y = middle - 5.0F;
        list.add_rect(ring_x, ring_y, 3.0F, 1.0F, ink);
        list.add_rect(ring_x, ring_y + 2.0F, 3.0F, 1.0F, ink);
        list.add_rect(ring_x, ring_y, 1.0F, 3.0F, ink);
        list.add_rect(ring_x + 2.0F, ring_y, 1.0F, 3.0F, ink);
    }
    draw_chevron(list, bounds.right() - 9.0F, middle + 1.0F,
                 lit ? theme.text : theme.text_muted);
    return it.clicked;
}

// The nearest of `choices` to `value`, by index.
template <typename Choices>
std::size_t nearest_choice(const Choices& choices, float value) noexcept {
    std::size_t best = 0U;
    for (std::size_t index = 1U; index < choices.size(); ++index) {
        if (std::abs(choices[index] - value) <
            std::abs(choices[best] - value)) {
            best = index;
        }
    }
    return best;
}

} // namespace

} // namespace mediaserver::editor::viewport

namespace mediaserver::editor {

using namespace viewport;

void ViewportPanel::draw_stage_view_menu(gui::UiContext& ctx,
                                         const gui::Rect& menu,
                                         ViewportFrame& frame) {
    if (menu.width <= 0.0F) {
        return;
    }
    const gui::UiId menu_id = gui::ui_id(id_, 40U);
    if (toolbar_menu(ctx, gui::ui_id(id_, 41U), menu, gui::Icon::Projection,
                     stage_projection_name(stage_projection_),
                     ctx.menu_open(menu_id), false, false, true)) {
        ctx.open_context_menu(menu_id, menu.x, menu.bottom());
    }
    ctx.tooltip(menu,
                "This view's projection, and one or four views. Top, Front "
                "and Side are orthographic: drag to pan, wheel to zoom.");
    const std::array<std::string_view, 5U> options{
        "Perspective    Alt+P", "Top            Alt+J",
        "Front          Alt+H", "Side           Alt+K", "Four views"};
    const std::array<bool, 5U> checked{
        stage_projection_ == StageProjection::Perspective,
        stage_projection_ == StageProjection::Top,
        stage_projection_ == StageProjection::Front,
        stage_projection_ == StageProjection::Side, stage_four_views_};
    if (const auto chosen = ctx.context_menu(menu_id, options, checked)) {
        if (*chosen < 4U) {
            frame.projection_choice = static_cast<StageProjection>(*chosen);
        } else {
            frame.layout_choice = !stage_four_views_;
        }
    }
}

void ViewportPanel::draw_toolbar(gui::UiContext& ctx, const gui::Rect& toolbar,
                                 float frames_per_second) {
    const gui::Theme& theme = ctx.theme();
    ctx.panel(toolbar, theme.background);
    {
        gui::Rect rule = toolbar;
        ctx.panel(gui::cut_bottom(rule, 1.0F), theme.divider);
    }
    // Controls are taken from both ends of one strip, 3px under the top and
    // 4px clear of the rule.
    gui::Rect bar = gui::inset(toolbar, gui::Padding{6.0F, 3.0F, 8.0F, 4.0F});
    const auto separator = [&](float x) {
        ctx.panel(gui::Rect{std::floor(x), toolbar.y + 8.0F, 1.0F,
                            toolbar.height - 17.0F},
                  theme.border);
    };

    // The preview's resolution and frame rate hold the right end at any
    // width; the tools fill from the left, and the display toggles take what
    // is left between them, folding into one menu when it is not enough.
    std::array<char, 24U> fps_text{};
    const int fps_length =
        std::snprintf(fps_text.data(), fps_text.size(), "%.3g fps",
                      std::max(frames_per_second, 0.0F));
    const std::string_view fps{
        fps_text.data(), static_cast<std::size_t>(std::max(fps_length, 0))};
    ctx.label(gui::cell(gui::cut_right(bar, ctx.font_width(fps)), toolbar),
              fps, theme.text_muted);
    static_cast<void>(gui::cut_right(bar, 8.0F));
    draw_resolution_control(ctx,
                            gui::cut_right(bar, resolution_control_width(ctx)));
    static_cast<void>(gui::cut_right(bar, 4.0F));
    draw_media_control(ctx, gui::cut_right(bar, media_control_width(ctx)));
    static_cast<void>(gui::cut_right(bar, toolbar_group_gap));

    static constexpr std::array<std::string_view, move_snap_choices.size()>
        distances{"0.01 m", "0.1 m", "0.5 m", "1 m", "5 m"};
    static constexpr std::array<std::string_view,
                                rotate_snap_choices_degrees.size()>
        degrees{"1", "5", "15", "45", "90"};
    static constexpr std::array<std::string_view,
                                rotate_snap_choices_degrees.size()>
        angles{"1 degree", "5 degrees", "15 degrees", "45 degrees",
               "90 degrees"};
    const std::size_t move_step =
        nearest_choice(move_snap_choices, snap_steps_.move);
    const std::size_t turn_step = nearest_choice(
        rotate_snap_choices_degrees, snap_steps_.rotate_turns * 360.0F);
    const float distance_width = toolbar_menu_width(ctx, distances[move_step]);
    const float angle_width =
        toolbar_menu_width(ctx, degrees[turn_step], true);
    constexpr float display_menu_width = 38.0F;
    constexpr float snap_gap = 2.0F;
    const float tools_width = 4.0F * toolbar_button + toolbar_group_gap +
                              toolbar_button + toolbar_group_gap +
                              toolbar_button + snap_gap + distance_width +
                              snap_gap + angle_width + snap_gap +
                              toolbar_button;
    if (tools_width + toolbar_group_gap + display_menu_width <= bar.width) {
        const auto tool = [&](core::u64 salt, gui::Icon icon, bool engaged,
                              std::string_view hint) {
            return icon_button(ctx, gui::ui_id(id_, salt),
                               gui::cut_left(bar, toolbar_button), icon, hint,
                               engaged);
        };
        const auto next_group = [&] {
            static_cast<void>(gui::cut_left(bar, toolbar_group_gap));
            separator(bar.x - toolbar_group_gap * 0.5F);
        };
        if (tool(10U, gui::Icon::MoveTool,
                 !measuring_ && tool_ == TransformTool::Move,
                 "Move. G moves the selection from the keyboard; X, Y or Z "
                 "constrains it, Shift+X/Y/Z to a plane.")) {
            tool_ = TransformTool::Move;
            set_measuring(false);
        }
        if (tool(11U, gui::Icon::RotateTool,
                 !measuring_ && tool_ == TransformTool::Rotate,
                 "Rotate. R rotates the selection about the view, or about "
                 "X, Y or Z once one is typed.")) {
            tool_ = TransformTool::Rotate;
            set_measuring(false);
        }
        if (tool(12U, gui::Icon::ScaleTool,
                 !measuring_ && tool_ == TransformTool::Scale,
                 "Scale. S scales the selection; X, Y or Z scales one axis, "
                 "Shift+X/Y/Z the other two.")) {
            tool_ = TransformTool::Scale;
            set_measuring(false);
        }
        if (tool(15U, gui::Icon::Measure, measuring_,
                 "Measure (M). Click two points for the distance between "
                 "them in metres; points snap to vertices, then edge "
                 "midpoints, then the surface. Shift+click adds to a path. "
                 "Nothing measured is saved.")) {
            set_measuring(!measuring_);
        }
        next_group();
        const bool global = orientation_ == TransformOrientation::Global;
        if (tool(13U, global ? gui::Icon::World : gui::Icon::Local, false,
                 global ? "World axes: the gizmo and the axis keys follow the "
                          "Stage. Click for the element's own axes; an axis "
                          "key pressed twice uses the other for that "
                          "transform."
                        : "Local axes: the gizmo and the axis keys follow the "
                          "selected element. Click for the Stage's axes; an "
                          "axis key pressed twice uses the other for that "
                          "transform.")) {
            orientation_ = global ? TransformOrientation::Local
                                  : TransformOrientation::Global;
        }
        next_group();
        if (tool(14U, gui::Icon::Snap, snapping_,
                 "Snap moves, turns and scales to the steps beside this "
                 "(Shift: a tenth of the distance and scale step, a fifth "
                 "of the angle). Shift+Tab toggles; hold Ctrl to invert "
                 "while dragging.")) {
            snapping_ = !snapping_;
        }
        static_cast<void>(gui::cut_left(bar, snap_gap));
        const gui::Rect distance = gui::cut_left(bar, distance_width);
        const gui::UiId distance_menu = gui::ui_id(id_, 22U);
        if (toolbar_menu(ctx, gui::ui_id(id_, 16U), distance, gui::Icon::Grid,
                         distances[move_step], ctx.menu_open(distance_menu),
                         false, !snapping_)) {
            ctx.open_context_menu(distance_menu, distance.x,
                                  distance.bottom());
        }
        ctx.tooltip(distance, "Move snap step.");
        {
            std::array<bool, move_snap_choices.size()> current{};
            current[move_step] = true;
            if (const auto chosen =
                    ctx.context_menu(distance_menu, distances, current)) {
                snap_steps_.move = move_snap_choices[*chosen];
            }
        }
        static_cast<void>(gui::cut_left(bar, snap_gap));
        const gui::Rect angle = gui::cut_left(bar, angle_width);
        const gui::UiId angle_menu = gui::ui_id(id_, 23U);
        if (toolbar_menu(ctx, gui::ui_id(id_, 17U), angle, gui::Icon::Angle,
                         degrees[turn_step], ctx.menu_open(angle_menu), true,
                         !snapping_)) {
            ctx.open_context_menu(angle_menu, angle.x,
                                  angle.bottom());
        }
        ctx.tooltip(angle, "Rotate snap step.");
        {
            std::array<bool, rotate_snap_choices_degrees.size()> current{};
            current[turn_step] = true;
            if (const auto chosen =
                    ctx.context_menu(angle_menu, angles, current)) {
                snap_steps_.rotate_turns =
                    rotate_snap_choices_degrees[*chosen] / 360.0F;
            }
        }
        static_cast<void>(gui::cut_left(bar, snap_gap));
        if (tool(18U, gui::Icon::Surface, surface_snapping_,
                 "Surface snapping: an element dragged by its body rides "
                 "over whatever is under the pointer and rests on it. Hold "
                 "V while moving to put the dragged element's vertex "
                 "nearest the pointer onto the vertex under it.")) {
            surface_snapping_ = !surface_snapping_;
        }
    }

    // The display toggles, right-aligned before the preview controls.
    constexpr float toggles_width = 4.0F * toolbar_button;
    if (toolbar_group_gap + toggles_width <= bar.width) {
        separator(bar.right() + toolbar_group_gap * 0.5F);
        gui::Rect toggles = gui::cut_right(bar, toggles_width);
        const auto toggle = [&](core::u64 salt, gui::Icon icon, bool& shown,
                                std::string_view hint) {
            if (icon_button(ctx, gui::ui_id(id_, salt),
                            gui::cut_left(toggles, toolbar_button), icon,
                            hint, shown)) {
                shown = !shown;
            }
        };
        toggle(1U, gui::Icon::Grid, show_grid_,
               "Grid: the floor grid, its spacing following the zoom.");
        toggle(2U, gui::Icon::Guides, show_overlays_,
               "Guides: camera frustums, light reach and other helpers "
               "drawn over the Stage.");
        toggle(3U, gui::Icon::Gizmo, show_gizmo_,
               "Gizmo: the selection's transform handles.");
        toggle(19U, gui::Icon::Bounds, show_bounds_,
               "Bounds: every element's bounding box, selected or not.");
    } else if (toolbar_group_gap + display_menu_width <= bar.width) {
        separator(bar.right() + toolbar_group_gap * 0.5F);
        // One menu holds them, so they stay reachable in a narrow dock.
        const gui::Rect show = gui::cut_right(bar, display_menu_width);
        const gui::UiId show_menu = gui::ui_id(id_, 20U);
        const gui::Interaction it = ctx.grab(gui::ui_id(id_, 21U), show);
        ctx.cursor_for(it, gui::Cursor::Hand);
        const bool open = ctx.menu_open(show_menu);
        if (it.held && !it.released) {
            ctx.panel(show, theme.widget_active);
        } else if (open || it.hovered) {
            ctx.panel(show, theme.widget_hover);
        }
        const bool lit = open || it.hovered;
        const float middle = show.y + show.height * 0.5F;
        gui::draw_eye_glyph(ctx.draw_list(), show.x + 13.0F, middle, true,
                            toolbar_ink(theme, lit ? 1.0F : 0.75F));
        draw_chevron(ctx.draw_list(), show.right() - 8.0F, middle,
                     lit ? theme.text : theme.text_muted);
        if (it.clicked) {
            ctx.open_context_menu(show_menu, show.x, show.bottom());
        }
        ctx.tooltip(show, "Show: grid, guides, gizmo and bounds.");
        constexpr std::array<std::string_view, 4U> options{
            "Grid", "Guides", "Gizmo", "Bounds"};
        const std::array<bool, 4U> shown{show_grid_, show_overlays_,
                                         show_gizmo_, show_bounds_};
        if (const auto chosen = ctx.context_menu(show_menu, options, shown)) {
            switch (*chosen) {
            case 0U: show_grid_ = !show_grid_; break;
            case 1U: show_overlays_ = !show_overlays_; break;
            case 2U: show_gizmo_ = !show_gizmo_; break;
            default: show_bounds_ = !show_bounds_; break;
            }
        }
    }
}

namespace {

constexpr std::array<std::string_view, 3U> resolution_names{"Full", "Half",
                                                          "Quarter"};

} // namespace

float ViewportPanel::resolution_control_width(
    const gui::UiContext& ctx) const noexcept {
    return toolbar_menu_width(
        ctx, resolution_names[static_cast<std::size_t>(resolution_)]);
}

void ViewportPanel::draw_resolution_control(gui::UiContext& ctx,
                                             const gui::Rect& bounds) {
    const auto current_index = static_cast<std::size_t>(resolution_);
    const gui::UiId menu = gui::ui_id(id_, 24U);
    if (toolbar_menu(ctx, gui::ui_id(id_, 4U), bounds, gui::Icon::Screen,
                     resolution_names[current_index], ctx.menu_open(menu))) {
        ctx.open_context_menu(menu, bounds.x, bounds.bottom());
    }
    ctx.tooltip(bounds,
                "Preview resolution: lower renders this view faster. What "
                "a click picks is unchanged.");
    std::array<bool, 3U> current{};
    current[current_index] = true;
    if (const auto chosen = ctx.context_menu(menu, resolution_names, current)) {
        resolution_ = static_cast<PreviewResolution>(*chosen);
    }
}

namespace {

constexpr std::array<std::string_view, 2U> media_names{"Proxy", "Full res"};

} // namespace

float ViewportPanel::media_control_width(
    const gui::UiContext& ctx) const noexcept {
    return toolbar_menu_width(
        ctx, media_names[static_cast<std::size_t>(drawn_media())]);
}

void ViewportPanel::draw_media_control(gui::UiContext& ctx,
                                       const gui::Rect& bounds) {
    const MediaRendition drawn = drawn_media();
    const gui::UiId menu = gui::ui_id(id_, 25U);
    if (toolbar_menu(ctx, gui::ui_id(id_, 5U), bounds,
                     drawn == MediaRendition::Full
                         ? gui::Icon::FullResolution
                         : gui::Icon::HalfResolution,
                     media_names[static_cast<std::size_t>(drawn)],
                     ctx.menu_open(menu))) {
        ctx.open_context_menu(menu, bounds.x, bounds.bottom());
    }
    std::string tip;
    if (precise_work_ && media_ == MediaRendition::Proxy) {
        tip = "Media: full resolution while a mask is selected or the "
              "eyedropper is armed, then back to proxies.";
    } else if (drawn == MediaRendition::Full) {
        tip = media_counts_.proxied == 0U
                  ? "Media: every clip plays its source. Proxies keep being "
                    "made for when you switch back."
                  : "Media: clips play their source, except " +
                        std::to_string(media_counts_.proxied) +
                        " whose asset asks for a proxy everywhere.";
    } else {
        tip = "Media: UHD video plays a 720p proxy here; outputs always "
              "play the source.";
        const auto clips = [](core::u32 count) {
            return std::to_string(count) + (count == 1U ? " clip" : " clips");
        };
        if (media_counts_.proxied > 0U) {
            tip += " " + clips(media_counts_.proxied) + " on proxies.";
        }
        if (media_counts_.waiting > 0U) {
            tip += " " + clips(media_counts_.waiting) +
                   " on the source, which plays more slowly, until " +
                   (media_counts_.waiting == 1U ? "its proxy is"
                                                : "their proxies are") +
                   " made";
            if (media_counts_.waiting_total_frames > 0U) {
                const auto percent = std::min<core::u64>(
                    99U, media_counts_.waiting_made_frames * 100U /
                             media_counts_.waiting_total_frames);
                tip += " (" + std::to_string(percent) + "% so far)";
            }
            tip += ". The part at the playhead is made first.";
        }
    }
    // The project's editor proxies still being made, whatever this view
    // plays: how soon Proxy will play every clip from a proxy.
    const bool all_coming =
        media_counts_.all_made_frames < media_counts_.all_total_frames;
    if (all_coming) {
        const auto percent = std::min<core::u64>(
            99U, media_counts_.all_made_frames * 100U /
                     media_counts_.all_total_frames);
        tip += " The project's editor proxies are " +
               std::to_string(percent) + "% made.";
    }
    ctx.tooltip(bounds, tip);
    if (all_coming) {
        draw_proxy_progress(
            ctx, gui::ui_id(id_, 26U), bounds,
            static_cast<float>(media_counts_.all_made_frames) /
                static_cast<float>(media_counts_.all_total_frames));
    } else if (drawn == MediaRendition::Proxy && media_counts_.waiting > 0U) {
        draw_proxy_progress(
            ctx, gui::ui_id(id_, 26U), bounds,
            media_counts_.waiting_total_frames == 0U
                ? 0.0F
                : static_cast<float>(media_counts_.waiting_made_frames) /
                      static_cast<float>(media_counts_.waiting_total_frames));
    }
    std::array<bool, 2U> current{};
    current[static_cast<std::size_t>(media_)] = true;
    if (const auto chosen = ctx.context_menu(menu, media_names, current)) {
        media_ = static_cast<MediaRendition>(*chosen);
    }
}

void ViewportPanel::copy_tools_from(const ViewportPanel& other) noexcept {
    tool_ = other.tool_;
    orientation_ = other.orientation_;
    snapping_ = other.snapping_;
    snap_steps_ = other.snap_steps_;
    surface_snapping_ = other.surface_snapping_;
    if (measuring_ != other.measuring_) {
        set_measuring(other.measuring_);
    }
    show_grid_ = other.show_grid_;
    show_overlays_ = other.show_overlays_;
    show_gizmo_ = other.show_gizmo_;
    show_bounds_ = other.show_bounds_;
    resolution_ = other.resolution_;
    media_ = other.media_;
}

void ViewportPanel::draw_stage_shading_menu(gui::UiContext& ctx,
                                            const gui::Rect& menu,
                                            ViewportFrame& frame) {
    if (menu.width <= 0.0F) {
        return;
    }
    const gui::UiId menu_id = gui::ui_id(id_, 42U);
    if (toolbar_menu(ctx, gui::ui_id(id_, 43U), menu, gui::Icon::Shading,
                     stage_shading_name(stage_shading_),
                     ctx.menu_open(menu_id), false, false, true)) {
        ctx.open_context_menu(menu_id, menu.x, menu.bottom());
    }
    ctx.tooltip(menu,
                "How this view shades. Only the Stage changes: Program and "
                "the outputs always show the lit picture.");
    // In ViewShading order.
    constexpr std::array<std::string_view, 6U> options{
        "Lit            Alt+4", "Unlit          Alt+3",
        "Lights only    Alt+6", "Wireframe      Alt+2",
        "UV checker",           "World normals"};
    std::array<bool, 6U> checked{};
    checked[static_cast<std::size_t>(stage_shading_)] = true;
    if (const auto chosen = ctx.context_menu(menu_id, options, checked)) {
        frame.shading_choice = static_cast<render::ViewShading>(*chosen);
    }
}

} // namespace mediaserver::editor
