// Laying out on the Program view: snapping a moved layer to the canvas, to
// other layers and to guides; the rulers guides are pulled from; and the
// safe-area overlay.

#include "viewport_frame.hpp"

#include <mediaserver/editor/canvas_snap.hpp>
#include <mediaserver/editor/gizmo.hpp>
#include <mediaserver/gui/rect_cut.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <string_view>

namespace mediaserver::editor {

using namespace viewport;

namespace {

// How near, in panel units, a moved edge has to come to a line to take it,
// and a pointer to a guide to pick it up.
constexpr float snap_reach_pixels = 6.0F;
constexpr float guide_hit_pixels = 4.0F;
// Numbered ruler ticks are at least this far apart.
constexpr float ruler_label_spacing = 64.0F;
// Other layers offered as snap targets: enough for any picture a person
// lays out by hand, and a bound on the press that gathers them.
constexpr std::size_t maximum_snap_layers = 256U;
// The action- and title-safe shares of the picture.
constexpr float action_safe = 0.93F;
constexpr float title_safe = 0.90F;

}  // namespace

std::optional<CanvasBox>
ViewportPanel::canvas_box_bounds(const scene::Scene& scene, doc::NodeId node,
                                 const ViewportProjection* projection) {
    const auto box = layer_box(scene, node, projection);
    if (!box.has_value()) {
        return std::nullopt;
    }
    CanvasBox bounds{box->canvas[0U].x, box->canvas[0U].y, box->canvas[0U].x,
                     box->canvas[0U].y};
    for (const scene::CanvasProjection& corner : box->canvas) {
        bounds.left = std::min(bounds.left, corner.x);
        bounds.right = std::max(bounds.right, corner.x);
        bounds.top = std::min(bounds.top, corner.y);
        bounds.bottom = std::max(bounds.bottom, corner.y);
    }
    return bounds;
}

void ViewportPanel::clear_guides() noexcept {
    guides_x_.clear();
    guides_y_.clear();
    guide_drag_ = GuideDrag::None;
}

// Called on the press that begins a move: what the moved layer covers, and
// every line it may land on - the canvas's edges and middle, each other
// layer's, and the guides. Gathered once, so the drag itself is arithmetic.
void ViewportPanel::capture_snap(Frame& f, doc::NodeId node) {
    snap_box_.reset();
    snap_lines_x_.clear();
    snap_lines_y_.clear();
    if (role_ != ViewportRole::Program) {
        return;
    }
    snap_box_ = canvas_box_bounds(f.scene, node, f.projection);
    if (!snap_box_.has_value()) {
        return;
    }
    add_snap_lines(snap_lines_x_, 0.0F, f.canvas_width);
    add_snap_lines(snap_lines_y_, 0.0F, f.canvas_height);
    snap_lines_x_.insert(snap_lines_x_.end(), guides_x_.begin(),
                         guides_x_.end());
    snap_lines_y_.insert(snap_lines_y_.end(), guides_y_.begin(),
                         guides_y_.end());
    std::size_t offered = 0U;
    const auto offer = [&](doc::NodeId other) {
        if (other == node || other == doc::NodeId{} ||
            selected(f.selection, other) || offered >= maximum_snap_layers) {
            return;
        }
        if (const auto bounds =
                canvas_box_bounds(f.scene, other, f.projection)) {
            add_snap_lines(snap_lines_x_, bounds->left, bounds->right);
            add_snap_lines(snap_lines_y_, bounds->top, bounds->bottom);
            ++offered;
        }
    };
    for (const scene::MediaInstance& media : f.scene.media) {
        offer(media.source_node);
    }
    for (const scene::MeshInstance& instance : f.scene.instances) {
        offer(instance.source);
    }
    for (const scene::ModelInstance& model : f.scene.models) {
        offer(model.source_node);
    }
}

// Shifts a move of (dx, dy) canvas pixels so the layer lands on the lines it
// has come within reach of, and notes those lines for drawing. Ctrl reverses
// the toggle for the length of the drag, as it does on Stage.
void ViewportPanel::snap_canvas_move(Frame& f, float& dx, float& dy) {
    if (role_ != ViewportRole::Program || !snap_box_.has_value() ||
        canvas_snapping_ == f.ctx.ctrl_held() || !(canvas_scale_ > 0.0F)) {
        return;
    }
    const float reach = snap_reach_pixels / canvas_scale_;
    const CanvasBox& box = *snap_box_;
    const AxisSnap across = snap_axis(
        {box.left + dx, box.centre_x() + dx, box.right + dx}, snap_lines_x_,
        reach);
    const AxisSnap down = snap_axis(
        {box.top + dy, box.centre_y() + dy, box.bottom + dy}, snap_lines_y_,
        reach);
    dx += across.shift;
    dy += down.shift;
    f.result.snap_x = across.line;
    f.result.snap_y = down.line;
}

// The rulers along the top and left of the pane, and the guides pulled out
// of them. A guide is a line of the canvas, kept in its pixels, so it stays
// on the picture through any zoom or pan. Runs before the picture's own
// interaction: a press on a ruler or a guide is not a press on a layer.
void ViewportPanel::run_guides(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    if (role_ != ViewportRole::Program || !rulers_shown_) {
        guide_drag_ = GuideDrag::None;
        return;
    }
    const gui::Rect& top = f.ruler_top;
    const gui::Rect& left = f.ruler_left;
    const float pointer_x = ctx.pointer_x();
    const float pointer_y = ctx.pointer_y();
    const CanvasPoint at = to_canvas(frame.image, f.interaction_canvas,
                                     pointer_x, pointer_y);
    const bool busy = dragging_ || canvas_panning_ || shape_in_progress();
    // Guides are picked up with Select; the rulers give them out with any
    // tool, as the wheel zooms with any tool.
    std::optional<std::size_t> over_x;
    std::optional<std::size_t> over_y;
    if (canvas_tool_ == CanvasTool::Select && !busy && !f.occluded &&
        f.pane.contains(pointer_x, pointer_y) &&
        frame.box_handle == TransformBoxHandle::None) {
        for (std::size_t index = 0U; index < guides_x_.size(); ++index) {
            const gui::UiPoint on = canvas_to_image(
                frame.image, f.interaction_canvas, guides_x_[index], 0.0F);
            if (std::abs(on.x - pointer_x) <= guide_hit_pixels) {
                over_x = index;
            }
        }
        for (std::size_t index = 0U; index < guides_y_.size(); ++index) {
            const gui::UiPoint on = canvas_to_image(
                frame.image, f.interaction_canvas, 0.0F, guides_y_[index]);
            if (std::abs(on.y - pointer_y) <= guide_hit_pixels) {
                over_y = index;
            }
        }
    }
    const bool over_guide = over_x.has_value() || over_y.has_value();
    const gui::Interaction from_top =
        ctx.grab(gui::ui_id(id_, 0x6D1DU), busy ? gui::Rect{} : top);
    const gui::Interaction from_left =
        ctx.grab(gui::ui_id(id_, 0x6D1EU), busy ? gui::Rect{} : left);
    const gui::Interaction on_guide = ctx.grab(
        gui::ui_id(id_, 0x6D1FU), over_guide ? f.pane : gui::Rect{});
    if (from_top.pressed) {
        // Out of the top ruler comes a line across the picture.
        guides_y_.push_back(at.y);
        guide_drag_ = GuideDrag::Down;
        guide_index_ = guides_y_.size() - 1U;
    } else if (from_left.pressed) {
        guides_x_.push_back(at.x);
        guide_drag_ = GuideDrag::Across;
        guide_index_ = guides_x_.size() - 1U;
    } else if (on_guide.pressed && over_x.has_value()) {
        guide_drag_ = GuideDrag::Across;
        guide_index_ = *over_x;
    } else if (on_guide.pressed && over_y.has_value()) {
        guide_drag_ = GuideDrag::Down;
        guide_index_ = *over_y;
    }
    if (over_x.has_value() || guide_drag_ == GuideDrag::Across) {
        ctx.set_cursor(gui::Cursor::ResizeHorizontal);
    } else if (over_y.has_value() || guide_drag_ == GuideDrag::Down) {
        ctx.set_cursor(gui::Cursor::ResizeVertical);
    }
    if (guide_drag_ == GuideDrag::None) {
        return;
    }
    f.navigating = true;
    std::vector<float>& guides =
        guide_drag_ == GuideDrag::Across ? guides_x_ : guides_y_;
    if (guide_index_ >= guides.size()) {
        guide_drag_ = GuideDrag::None;
        return;
    }
    const bool held = from_top.held || from_left.held || on_guide.held;
    const bool released =
        from_top.released || from_left.released || on_guide.released;
    if (held && !released) {
        // Whole pixels: a guide is something to line up to.
        guides[guide_index_] =
            std::round(guide_drag_ == GuideDrag::Across ? at.x : at.y);
    }
    if (released || !held) {
        // Let go over a ruler, or off the pane, the guide is put away.
        const bool kept = f.pane.contains(pointer_x, pointer_y) &&
                          !top.contains(pointer_x, pointer_y) &&
                          !left.contains(pointer_x, pointer_y);
        if (!kept) {
            guides.erase(guides.begin() +
                         static_cast<std::ptrdiff_t>(guide_index_));
        }
        guide_drag_ = GuideDrag::None;
    }
}

// Drawn over the picture and clipped to it: the safe areas, the guides, and
// the lines a moved layer has snapped to.
void ViewportPanel::draw_layout_overlays(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    if (role_ != ViewportRole::Program) {
        return;
    }
    gui::UiDrawList& list = ctx.draw_list();
    const gui::Theme& theme = ctx.theme();
    const gui::Rect& image = frame.image;
    if (safe_areas_shown_) {
        gui::UiColor ink = theme.text_muted;
        ink.a = 0.75F;
        for (const float share : {action_safe, title_safe}) {
            const float inset_x = image.width * (1.0F - share) * 0.5F;
            const float inset_y = image.height * (1.0F - share) * 0.5F;
            const gui::UiPoint a{image.x + inset_x, image.y + inset_y};
            const gui::UiPoint b{image.right() - inset_x, image.y + inset_y};
            const gui::UiPoint c{image.right() - inset_x,
                                 image.bottom() - inset_y};
            const gui::UiPoint d{image.x + inset_x, image.bottom() - inset_y};
            haloed_line(list, a, b, 2.5F, 1.0F, ink);
            haloed_line(list, b, c, 2.5F, 1.0F, ink);
            haloed_line(list, c, d, 2.5F, 1.0F, ink);
            haloed_line(list, d, a, 2.5F, 1.0F, ink);
        }
        // The middle of the picture, as a small cross.
        const gui::UiPoint centre{image.x + image.width * 0.5F,
                                  image.y + image.height * 0.5F};
        haloed_line(list, gui::UiPoint{centre.x - 10.0F, centre.y},
                    gui::UiPoint{centre.x + 10.0F, centre.y}, 2.5F, 1.0F, ink);
        haloed_line(list, gui::UiPoint{centre.x, centre.y - 10.0F},
                    gui::UiPoint{centre.x, centre.y + 10.0F}, 2.5F, 1.0F, ink);
    }
    // Whole-pixel rules over a dark backing, so a line across the picture
    // is as crisp as one down it wherever it falls.
    const auto across = [&](float x, gui::UiColor color, float thickness) {
        const float at = std::floor(
            canvas_to_image(image, f.interaction_canvas, x, 0.0F).x);
        list.add_rect(at - 1.0F, f.pane.y, thickness + 2.0F, f.pane.height,
                      overlay_halo);
        list.add_rect(at, f.pane.y, thickness, f.pane.height, color);
    };
    const auto down = [&](float y, gui::UiColor color, float thickness) {
        const float at = std::floor(
            canvas_to_image(image, f.interaction_canvas, 0.0F, y).y);
        list.add_rect(f.pane.x, at - 1.0F, f.pane.width, thickness + 2.0F,
                      overlay_halo);
        list.add_rect(f.pane.x, at, f.pane.width, thickness, color);
    };
    if (rulers_shown_) {
        gui::UiColor guide = theme.accent;
        guide.a = 0.7F;
        for (const float x : guides_x_) {
            across(x, guide, 1.0F);
        }
        for (const float y : guides_y_) {
            down(y, guide, 1.0F);
        }
    }
    if (frame.snap_x.has_value()) {
        across(*frame.snap_x, theme.accent, 2.0F);
    }
    if (frame.snap_y.has_value()) {
        down(*frame.snap_y, theme.accent, 2.0F);
    }
}

// The rulers themselves, over everything else in the pane: ticks at round
// canvas pixels, numbered where there is room, and a mark where the pointer
// is.
void ViewportPanel::draw_rulers(Frame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    if (role_ != ViewportRole::Program || !rulers_shown_ ||
        f.ruler_top.width <= 0.0F || f.ruler_left.height <= 0.0F) {
        return;
    }
    const gui::Theme& theme = ctx.theme();
    const gui::Rect& top = f.ruler_top;
    const gui::Rect& left = f.ruler_left;
    ctx.panel(top, theme.header);
    ctx.panel(left, theme.header);
    ctx.panel(gui::Rect{left.x, top.y, left.width, top.height}, theme.header);
    ctx.panel(gui::Rect{top.x, top.bottom() - 1.0F, top.width, 1.0F},
              theme.divider);
    ctx.panel(gui::Rect{left.right() - 1.0F, left.y, 1.0F, left.height},
              theme.divider);
    ctx.tooltip(top, "Drag down from here for a guide across the picture");
    ctx.tooltip(left, "Drag across from here for a guide down the picture");
    const float scale = canvas_scale_;
    if (!(scale > 0.0F)) {
        return;
    }
    const float step = ruler_step(scale, ruler_label_spacing);
    const float minor = step / 5.0F;
    gui::UiDrawList& list = ctx.draw_list();
    const auto label = [&](const gui::Rect& at, float value) {
        std::array<char, 16U> text{};
        const int length = std::snprintf(text.data(), text.size(), "%.0f",
                                         static_cast<double>(value));
        ctx.label(at,
                  std::string_view{text.data(), static_cast<std::size_t>(
                                                    std::max(length, 0))},
                  theme.text_muted);
    };
    // Across the top.
    ctx.push_clip(top);
    {
        const float first =
            std::floor((top.x - frame.image.x) / scale / minor) * minor;
        for (float value = first;; value += minor) {
            const float x = frame.image.x + value * scale;
            if (x > top.right()) {
                break;
            }
            const float whole = std::round(value / step) * step;
            const bool major = std::abs(value - whole) < minor * 0.25F;
            list.add_rect(std::floor(x), top.bottom() - (major ? 8.0F : 4.0F),
                          1.0F, major ? 8.0F : 4.0F, theme.text_muted);
            if (major) {
                label(gui::Rect{x + 3.0F, top.y, ruler_label_spacing - 6.0F,
                                top.height - 4.0F},
                      whole);
            }
        }
        if (top.contains(ctx.pointer_x(), top.y + 1.0F) ||
            f.pane.contains(ctx.pointer_x(), ctx.pointer_y())) {
            list.add_rect(std::floor(ctx.pointer_x()), top.y, 1.0F, top.height,
                          theme.accent);
        }
    }
    ctx.pop_clip();
    // Down the left, each number above its tick.
    ctx.push_clip(left);
    {
        const float first =
            std::floor((left.y - frame.image.y) / scale / minor) * minor;
        for (float value = first;; value += minor) {
            const float y = frame.image.y + value * scale;
            if (y > left.bottom()) {
                break;
            }
            const float whole = std::round(value / step) * step;
            const bool major = std::abs(value - whole) < minor * 0.25F;
            list.add_rect(left.right() - (major ? 8.0F : 4.0F), std::floor(y),
                          major ? 8.0F : 4.0F, 1.0F, theme.text_muted);
            if (major) {
                label(gui::Rect{left.x + 2.0F, y + 1.0F, left.width - 4.0F,
                                14.0F},
                      whole);
            }
        }
        if (f.pane.contains(ctx.pointer_x(), ctx.pointer_y())) {
            list.add_rect(left.x, std::floor(ctx.pointer_y()), left.width, 1.0F,
                          theme.accent);
        }
    }
    ctx.pop_clip();
}

}  // namespace mediaserver::editor
