// ViewportPanel::draw_masks: picking, dragging and drawing masks.

#include "viewport_frame.hpp"

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

namespace mediaserver::editor {

using namespace viewport;

MaskViewportFrame ViewportPanel::draw_masks(
    gui::UiContext& ctx, const gui::Rect& bounds,
    const rhi::Texture& render_target, const scene::Scene& scene,
    const MaskViewportInput& input, const scene::Camera* camera_overlay,
    float frames_per_second, const ViewportProjection* projection) {
    MaskFrame f{ctx,           bounds,
                render_target, scene,
                input,         camera_overlay,
                frames_per_second, projection};
    if (!begin_mask_frame(f)) {
        return f.result;
    }
    pick_mask(f);
    press_mask(f);
    drag_mask(f);
    release_mask(f);
    draw_mask_overlay(f);
    return f.result;
}

bool ViewportPanel::begin_mask_frame(MaskFrame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& bounds = f.bounds;
    auto& render_target = f.render_target;
    auto& scene = f.scene;
    auto& input = f.input;
    auto& camera_overlay = f.camera_overlay;
    auto& frames_per_second = f.frames_per_second;
    auto& projection = f.projection;
    frame.selection = input.selection;

    // Entering the mask domain terminates any scene/surface gesture. The full
    // panel occluder below then leaves the ordinary draw path visual-only.
    dragging_ = false;
    surface_dragging_ = false;
    drag_axis_ = GizmoAxis::None;
    keyboard_.active = false;
    swallow_pointer_ = false;
    deep_select_armed_ = false;
    deep_select_open_ = false;
    deep_candidates_.clear();
    const gui::Rect ordinary_interaction_block = bounds;
    // The mask tools own the image's top-right corner (COVERAGE).
    drawing_masks_ = true;
    const ViewportFrame ordinary = draw(
        ctx, bounds, render_target, scene, {}, {}, camera_overlay,
        &ordinary_interaction_block, frames_per_second, nullptr, {},
        projection);
    drawing_masks_ = false;
    frame.image = ordinary.image;
    // Mask coordinates resolve against whatever canvas this panel is showing:
    // the authored camera canvas on Stage, the output raster on Program. The
    // role no longer decides whether masks may be authored - having a picture
    // and a canvas to resolve against does.
    const scene::Canvas mask_canvas = viewport_canvas(scene, projection);
    if (frame.image.width <= 0.0F || frame.image.height <= 0.0F ||
        mask_canvas.width == 0U || mask_canvas.height == 0U) {
        mask_gesture_active_ = false;
        mask_gesture_cancelled_ = false;
        return false;
    }

    std::vector<const scene::MaskInstance*> masks;
    masks.reserve(input.masks.size());
    for (const scene::MaskInstance& mask : input.masks) {
        if (mask.scene == input.target.scene &&
            mask.target == input.target.target) {
            masks.push_back(&mask);
        }
    }
    std::sort(masks.begin(), masks.end(),
              [](const scene::MaskInstance* left,
                 const scene::MaskInstance* right) noexcept {
                  return std::tie(left->order, left->source) <
                         std::tie(right->order, right->source);
              });
    const auto find_mask = [&masks](doc::NodeId id)
        -> const scene::MaskInstance* {
        const auto found = std::find_if(
            masks.begin(), masks.end(),
            [id](const scene::MaskInstance* mask) noexcept {
                return mask->source == id;
            });
        return found == masks.end() ? nullptr : *found;
    };

    if (frame.selection.scene != input.target.scene ||
        frame.selection.target != input.target.target) {
        frame.selection = MasksPanelSelection{
            input.target.scene, input.target.target, {}, {}};
        frame.selection_changed = true;
        mask_gesture_active_ = false;
        mask_gesture_cancelled_ = false;
    }
    const scene::MaskInstance* selected_mask =
        find_mask(frame.selection.mask);
    if (frame.selection.mask && selected_mask == nullptr) {
        frame.selection.mask = {};
        frame.selection.point.reset();
        frame.selection_changed = true;
        mask_gesture_active_ = false;
        mask_gesture_cancelled_ = false;
    } else if (selected_mask != nullptr &&
               frame.selection.point.has_value() &&
               mask_point(*selected_mask, *frame.selection.point) == nullptr) {
        frame.selection.point.reset();
        frame.selection_changed = true;
        mask_gesture_active_ = false;
        mask_gesture_cancelled_ = false;
    }

    frame.mapping_valid = mask_normalized_to_viewport(
                              frame.image, mask_canvas,
                              input.target.coordinate_space,
                              source_mapping(input), 0.5F, 0.5F)
                              .has_value();
    // The image's top-right corner, 5 in.
    gui::Rect corner = gui::inset(frame.image, gui::Padding{0.0F, 5.0F, 5.0F, 0.0F});
    corner = gui::cut_top(corner, 21.0F);
    const gui::Rect coverage_button = gui::cut_right(corner, 101.0F);
    const gui::Interaction coverage_interaction =
        ctx.grab(gui::ui_id(id_, 0x4D41534BU), coverage_button);
    const gui::Rect mask_interaction_bounds =
        !frame.mapping_valid || coverage_interaction.hovered
        ? gui::Rect{}
        : frame.image;
    const gui::Interaction interaction =
        ctx.grab(gui::ui_id(id_, 0x4D41534CULL), mask_interaction_bounds);
    frame.hovered = interaction.hovered;
    frame.held = interaction.held;

    const auto normalized_pointer = mask_normalized_point(
        frame.image, mask_canvas, input, ctx.pointer_x(), ctx.pointer_y());
    std::vector<MaskPath> paths;
    paths.reserve(masks.size());
    for (const scene::MaskInstance* mask : masks) {
        paths.push_back(build_mask_path(*mask, frame.image, mask_canvas,
                                        input));
    }
    f.mask_canvas = mask_canvas;
    f.masks = std::move(masks);
    f.selected_mask = selected_mask;
    f.coverage_button = coverage_button;
    f.coverage_interaction = coverage_interaction;
    f.interaction = interaction;
    f.normalized_pointer = normalized_pointer;
    f.paths = std::move(paths);
    return true;
}

void ViewportPanel::pick_mask(MaskFrame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& input = f.input;
    auto& mask_canvas = f.mask_canvas;
    auto& masks = f.masks;
    auto& selected_mask = f.selected_mask;
    auto& normalized_pointer = f.normalized_pointer;
    auto& paths = f.paths;
    MaskPick pick{};
    if (frame.mapping_valid && normalized_pointer.has_value() &&
        !mask_gesture_active_ && !mask_gesture_cancelled_) {
        // A selected primitive exposes its transform directly on the picture:
        // four corner handles resize it and the stem above the top edge rotates
        // it. These handles outrank picking the mask body beneath them.
        if (selected_mask != nullptr &&
            selected_mask->kind != scene::MaskKind::Bezier) {
            constexpr std::array hits{
                MaskViewportHit::ResizeTopLeft,
                MaskViewportHit::ResizeTopRight,
                MaskViewportHit::ResizeBottomRight,
                MaskViewportHit::ResizeBottomLeft};
            const std::array local{
                MaskViewportNormalizedPoint{selected_mask->bounds.left,
                                            selected_mask->bounds.top},
                MaskViewportNormalizedPoint{selected_mask->bounds.right,
                                            selected_mask->bounds.top},
                MaskViewportNormalizedPoint{selected_mask->bounds.right,
                                            selected_mask->bounds.bottom},
                MaskViewportNormalizedPoint{selected_mask->bounds.left,
                                            selected_mask->bounds.bottom}};
            const auto extent = mask_coordinate_extent(mask_canvas, input);
            float best = mask_shape_handle_hit_pixels;
            for (std::size_t index = 0U; index < local.size(); ++index) {
                const auto rotated = rotate_shape_point(
                    local[index], selected_mask->bounds,
                    selected_mask->rotation_turns, extent);
                const auto screen = mask_screen_point(
                    frame.image, mask_canvas, input, rotated);
                if (!screen.has_value()) {
                    continue;
                }
                const float distance = point_distance(
                    *screen, ctx.pointer_x(), ctx.pointer_y());
                if (distance < best) {
                    best = distance;
                    pick = MaskPick{hits[index], selected_mask};
                }
            }
            const MaskViewportNormalizedPoint centre{
                (selected_mask->bounds.left + selected_mask->bounds.right) *
                    0.5F,
                (selected_mask->bounds.top + selected_mask->bounds.bottom) *
                    0.5F};
            const auto top = rotate_shape_point(
                {centre.x, selected_mask->bounds.top},
                selected_mask->bounds, selected_mask->rotation_turns, extent);
            const auto centre_screen = mask_screen_point(
                frame.image, mask_canvas, input, centre);
            const auto top_screen = mask_screen_point(
                frame.image, mask_canvas, input, top);
            if (centre_screen.has_value() && top_screen.has_value()) {
                const float dx = top_screen->x - centre_screen->x;
                const float dy = top_screen->y - centre_screen->y;
                const float length = std::sqrt(dx * dx + dy * dy);
                if (length > 1.0F) {
                    const gui::UiPoint rotate_screen{
                        top_screen->x + dx / length * 24.0F,
                        top_screen->y + dy / length * 24.0F};
                    const float distance = point_distance(
                        rotate_screen, ctx.pointer_x(), ctx.pointer_y());
                    if (distance < best) {
                        pick = MaskPick{MaskViewportHit::Rotate,
                                        selected_mask};
                    }
                }
            }
        }

        // Selected handles are deliberately first. A handle crossing its own
        // anchor remains draggable instead of degrading into a point move.
        if (pick.hit == MaskViewportHit::None && selected_mask != nullptr &&
            frame.selection.point.has_value()) {
            const scene::MaskPoint* point =
                mask_point(*selected_mask, *frame.selection.point);
            if (point != nullptr) {
                const auto anchor = mask_screen_point(
                    frame.image, mask_canvas, input, {point->x, point->y});
                const auto in = mask_screen_point(
                    frame.image, mask_canvas, input,
                    {point->in_x, point->in_y});
                const auto out = mask_screen_point(
                    frame.image, mask_canvas, input,
                    {point->out_x, point->out_y});
                float best = mask_handle_hit_pixels;
                if (anchor.has_value() && out.has_value() &&
                    point_distance(*anchor, out->x, out->y) > 2.0F) {
                    const float distance = point_distance(
                        *out, ctx.pointer_x(), ctx.pointer_y());
                    if (distance <= best) {
                        best = distance;
                        pick = MaskPick{MaskViewportHit::OutHandle,
                                        selected_mask, point->source};
                    }
                }
                if (anchor.has_value() && in.has_value() &&
                    point_distance(*anchor, in->x, in->y) > 2.0F) {
                    const float distance = point_distance(
                        *in, ctx.pointer_x(), ctx.pointer_y());
                    if (distance < best) {
                        pick = MaskPick{MaskViewportHit::InHandle,
                                        selected_mask, point->source};
                    }
                }
            }
        }

        // Anchors outrank path insertion and whole-mask translation.
        if (pick.hit == MaskViewportHit::None) {
            float best = mask_point_hit_pixels;
            for (auto mask_it = masks.rbegin(); mask_it != masks.rend();
                 ++mask_it) {
                for (const scene::MaskPoint& point : (*mask_it)->points) {
                    const auto screen = mask_screen_point(
                        frame.image, mask_canvas, input, {point.x, point.y});
                    if (!screen.has_value()) {
                        continue;
                    }
                    const float distance = point_distance(
                        *screen, ctx.pointer_x(), ctx.pointer_y());
                    if (distance < best) {
                        best = distance;
                        pick = MaskPick{MaskViewportHit::Point, *mask_it,
                                        point.source};
                    }
                }
            }
        }

        if (pick.hit == MaskViewportHit::None && input.pen_active &&
            selected_mask != nullptr &&
            selected_mask->kind == scene::MaskKind::Bezier) {
            const auto selected_it = std::find(masks.begin(), masks.end(),
                                               selected_mask);
            if (selected_it != masks.end()) {
                const std::size_t index = static_cast<std::size_t>(
                    selected_it - masks.begin());
                const MaskPathHit path_hit = closest_path_point(
                    paths[index], ctx.pointer_x(), ctx.pointer_y());
                if (path_hit.distance <= mask_path_hit_pixels) {
                    const scene::MaskPointGeometry geometry{
                        path_hit.normalized.x, path_hit.normalized.y,
                        path_hit.normalized.x, path_hit.normalized.y,
                        path_hit.normalized.x, path_hit.normalized.y};
                    pick = MaskPick{MaskViewportHit::Path, selected_mask, {},
                                    path_hit.insertion_index, geometry};
                }
            }
        }

        if (pick.hit == MaskViewportHit::None) {
            for (std::size_t reverse = masks.size(); reverse > 0U; --reverse) {
                const std::size_t index = reverse - 1U;
                const MaskPathHit path_hit = closest_path_point(
                    paths[index], ctx.pointer_x(), ctx.pointer_y());
                if (path_hit.distance <= mask_path_hit_pixels ||
                    inside_mask(*masks[index], *normalized_pointer,
                                mask_canvas, input)) {
                    pick = MaskPick{MaskViewportHit::WholeMask, masks[index]};
                    break;
                }
            }
        }
    }
    frame.hit = mask_gesture_active_ ? mask_gesture_hit_ : pick.hit;
    f.pick = pick;
}

void ViewportPanel::press_mask(MaskFrame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& input = f.input;
    auto& interaction = f.interaction;
    auto& normalized_pointer = f.normalized_pointer;
    auto& pick = f.pick;
    const auto find_mask = [&f](doc::NodeId id) {
        return f.find_mask(id);
    };
    frame.pressed_away = interaction.pressed && pick.mask == nullptr;
    if (interaction.pressed && pick.mask != nullptr) {
        if (frame.selection.mask != pick.mask->source ||
            frame.selection.point != pick.point) {
            frame.selection = MasksPanelSelection{
                input.target.scene, input.target.target, pick.mask->source,
                pick.point};
            frame.selection_changed = true;
        }
        if (input.authoring_enabled && normalized_pointer.has_value()) {
            mask_gesture_active_ = true;
            mask_gesture_cancelled_ = false;
            mask_gesture_hit_ = pick.hit;
            mask_gesture_scene_ = input.target.scene;
            mask_gesture_target_ = input.target.target;
            mask_gesture_mask_ = pick.mask->source;
            mask_gesture_point_ = pick.point;
            mask_press_ = *normalized_pointer;
            mask_gesture_start_ = *pick.mask;
            mask_gesture_preview_ = *pick.mask;
            mask_insert_index_ = pick.insertion_index;
            mask_insert_geometry_ = pick.insertion_geometry;
            frame.hit = pick.hit;
        }
    }

    if (mask_gesture_active_ &&
        (mask_gesture_scene_ != input.target.scene ||
         mask_gesture_target_ != input.target.target ||
         find_mask(mask_gesture_mask_) == nullptr ||
         !input.authoring_enabled)) {
        mask_gesture_active_ = false;
        mask_gesture_cancelled_ = true;
    }
    if (mask_gesture_active_ && ctx.key_pressed(gui::Key::Escape)) {
        mask_gesture_active_ = false;
        mask_gesture_cancelled_ = true;
    }
}

void ViewportPanel::drag_mask(MaskFrame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& input = f.input;
    auto& mask_canvas = f.mask_canvas;
    auto& interaction = f.interaction;
    auto& normalized_pointer = f.normalized_pointer;
    float gesture_dx{};
    float gesture_dy{};
    bool gesture_changed{};
    if (mask_gesture_active_ && (interaction.held || interaction.released) &&
        normalized_pointer.has_value()) {
        mask_gesture_preview_ = mask_gesture_start_;
        switch (mask_gesture_hit_) {
        case MaskViewportHit::WholeMask: {
            auto delta = constrained_delta(
                normalized_pointer->x - mask_press_.x,
                normalized_pointer->y - mask_press_.y, ctx.shift_held());
            delta = clamp_translation(mask_gesture_start_, delta.first,
                                      delta.second);
            gesture_dx = delta.first;
            gesture_dy = delta.second;
            translate_mask(mask_gesture_preview_, gesture_dx, gesture_dy);
            gesture_changed = std::abs(gesture_dx) + std::abs(gesture_dy) >=
                              mask_minimum_drag;
            break;
        }
        case MaskViewportHit::Point: {
            auto delta = constrained_delta(
                normalized_pointer->x - mask_press_.x,
                normalized_pointer->y - mask_press_.y, ctx.shift_held());
            delta = clamp_translation(mask_gesture_start_, delta.first,
                                      delta.second, mask_gesture_point_);
            gesture_dx = delta.first;
            gesture_dy = delta.second;
            translate_mask(mask_gesture_preview_, gesture_dx, gesture_dy,
                           mask_gesture_point_);
            gesture_changed = std::abs(gesture_dx) + std::abs(gesture_dy) >=
                              mask_minimum_drag;
            break;
        }
        case MaskViewportHit::InHandle:
        case MaskViewportHit::OutHandle: {
            if (!mask_gesture_point_.has_value()) {
                break;
            }
            scene::MaskPoint* point = mask_point(
                mask_gesture_preview_, *mask_gesture_point_);
            if (point == nullptr) {
                break;
            }
            const MaskViewportNormalizedPoint anchor{point->x, point->y};
            const MaskViewportNormalizedPoint value = constrained_handle(
                anchor, *normalized_pointer, ctx.shift_held());
            if (mask_gesture_hit_ == MaskViewportHit::InHandle) {
                point->in_x = value.x;
                point->in_y = value.y;
                if (!ctx.alt_held()) {
                    point->out_x = std::clamp(
                        anchor.x * 2.0F - value.x, 0.0F, 1.0F);
                    point->out_y = std::clamp(
                        anchor.y * 2.0F - value.y, 0.0F, 1.0F);
                }
            } else {
                point->out_x = value.x;
                point->out_y = value.y;
                if (!ctx.alt_held()) {
                    point->in_x = std::clamp(
                        anchor.x * 2.0F - value.x, 0.0F, 1.0F);
                    point->in_y = std::clamp(
                        anchor.y * 2.0F - value.y, 0.0F, 1.0F);
                }
            }
            const scene::MaskPoint* before = mask_point(
                mask_gesture_start_, *mask_gesture_point_);
            gesture_changed = before != nullptr &&
                !empty_patch(point_patch(*before, *point));
            break;
        }
        case MaskViewportHit::ResizeTopLeft:
        case MaskViewportHit::ResizeTopRight:
        case MaskViewportHit::ResizeBottomRight:
        case MaskViewportHit::ResizeBottomLeft: {
            const auto extent = mask_coordinate_extent(mask_canvas, input);
            const auto local = unrotate_shape_point(
                *normalized_pointer, mask_gesture_start_.bounds,
                mask_gesture_start_.rotation_turns, extent);
            const float centre_x =
                (mask_gesture_start_.bounds.left +
                 mask_gesture_start_.bounds.right) * 0.5F;
            const float centre_y =
                (mask_gesture_start_.bounds.top +
                 mask_gesture_start_.bounds.bottom) * 0.5F;
            float half_x = std::abs(local.x - centre_x);
            float half_y = std::abs(local.y - centre_y);
            if (ctx.shift_held()) {
                const float held = std::max(half_x * extent.x,
                                            half_y * extent.y);
                half_x = held / extent.x;
                half_y = held / extent.y;
            }
            half_x = std::clamp(
                half_x, 0.002F, std::min(centre_x, 1.0F - centre_x));
            half_y = std::clamp(
                half_y, 0.002F, std::min(centre_y, 1.0F - centre_y));
            mask_gesture_preview_.bounds = scene::MaskBounds{
                centre_x - half_x, centre_y - half_y,
                centre_x + half_x, centre_y + half_y};
            gesture_changed =
                mask_gesture_preview_.bounds != mask_gesture_start_.bounds;
            break;
        }
        case MaskViewportHit::Rotate: {
            constexpr float tau = 6.28318530717958647692F;
            const auto extent = mask_coordinate_extent(mask_canvas, input);
            const float centre_x =
                (mask_gesture_start_.bounds.left +
                 mask_gesture_start_.bounds.right) * 0.5F;
            const float centre_y =
                (mask_gesture_start_.bounds.top +
                 mask_gesture_start_.bounds.bottom) * 0.5F;
            const float dx = (normalized_pointer->x - centre_x) * extent.x;
            const float dy = (normalized_pointer->y - centre_y) * extent.y;
            if (std::abs(dx) + std::abs(dy) > 1.0e-5F) {
                float turns = std::atan2(dy, dx) / tau + 0.25F;
                if (ctx.shift_held()) {
                    turns = std::round(turns * 24.0F) / 24.0F;
                }
                turns -= std::floor(turns + 0.5F);
                mask_gesture_preview_.rotation_turns = turns;
                gesture_changed = std::abs(
                    turns - mask_gesture_start_.rotation_turns) >=
                    mask_minimum_drag;
            }
            break;
        }
        case MaskViewportHit::Path:
        case MaskViewportHit::None:
            break;
        }
    }
    frame.dragging = gesture_changed;
    f.gesture_dx = gesture_dx;
    f.gesture_dy = gesture_dy;
    f.gesture_changed = gesture_changed;
}

void ViewportPanel::release_mask(MaskFrame& f) {
    auto& frame = f.result;
    auto& interaction = f.interaction;
    auto& gesture_dx = f.gesture_dx;
    auto& gesture_dy = f.gesture_dy;
    auto& gesture_changed = f.gesture_changed;
    if (interaction.released) {
        frame.released = true;
        if (mask_gesture_cancelled_) {
            frame.cancelled = true;
        } else if (mask_gesture_active_) {
            switch (mask_gesture_hit_) {
            case MaskViewportHit::Path:
                frame.intent = MaskViewportPointInsertIntent{
                    mask_gesture_scene_, mask_gesture_target_,
                    mask_gesture_mask_, mask_insert_index_,
                    mask_insert_geometry_};
                break;
            case MaskViewportHit::WholeMask:
                if (gesture_changed) {
                    frame.intent = MaskViewportTranslateIntent{
                        mask_gesture_scene_, mask_gesture_target_,
                        mask_gesture_mask_, gesture_dx, gesture_dy};
                }
                break;
            case MaskViewportHit::Point:
            case MaskViewportHit::InHandle:
            case MaskViewportHit::OutHandle:
                if (gesture_changed && mask_gesture_point_.has_value()) {
                    const scene::MaskPoint* before = mask_point(
                        mask_gesture_start_, *mask_gesture_point_);
                    const scene::MaskPoint* after = mask_point(
                        mask_gesture_preview_, *mask_gesture_point_);
                    if (before != nullptr && after != nullptr) {
                        scene::MaskPointPatch patch =
                            point_patch(*before, *after);
                        if (!empty_patch(patch)) {
                            frame.intent = MaskViewportPointEditIntent{
                                mask_gesture_scene_, mask_gesture_target_,
                                mask_gesture_mask_, *mask_gesture_point_,
                                std::move(patch)};
                        }
                    }
                }
                break;
            case MaskViewportHit::ResizeTopLeft:
            case MaskViewportHit::ResizeTopRight:
            case MaskViewportHit::ResizeBottomRight:
            case MaskViewportHit::ResizeBottomLeft:
            case MaskViewportHit::Rotate:
                if (gesture_changed) {
                    frame.intent = MaskViewportShapeEditIntent{
                        mask_gesture_scene_, mask_gesture_target_,
                        mask_gesture_mask_, mask_gesture_preview_.bounds,
                        mask_gesture_preview_.rotation_turns};
                }
                break;
            case MaskViewportHit::None:
                break;
            }
        }
        mask_gesture_active_ = false;
        mask_gesture_cancelled_ = false;
        mask_gesture_hit_ = MaskViewportHit::None;
        mask_gesture_point_.reset();
    }
}

void ViewportPanel::draw_mask_overlay(MaskFrame& f) {
    auto& frame = f.result;
    auto& ctx = f.ctx;
    auto& input = f.input;
    auto& mask_canvas = f.mask_canvas;
    auto& masks = f.masks;
    auto& coverage_button = f.coverage_button;
    auto& coverage_interaction = f.coverage_interaction;
    ctx.push_clip(shown_image(frame.image));
    gui::UiDrawList& list = ctx.draw_list();
    const float canvas_scale =
        0.5F * (frame.image.width / static_cast<float>(mask_canvas.width) +
                frame.image.height / static_cast<float>(mask_canvas.height));
    for (std::size_t index = 0U; index < masks.size(); ++index) {
        const scene::MaskInstance* display = masks[index];
        if (mask_gesture_active_ && display->source == mask_gesture_mask_) {
            display = &mask_gesture_preview_;
        }
        const MaskPath path = build_mask_path(
            *display, frame.image, mask_canvas, input);
        const float expansion = std::clamp(
            std::abs(display->expansion_canvas_px) * canvas_scale * 2.0F +
                1.0F,
            1.0F, 80.0F);
        const float feather = std::clamp(
            display->feather_canvas_px * canvas_scale * 2.0F + expansion,
            expansion, 96.0F);
        if (display->feather_canvas_px > 0.0F) {
            draw_mask_path(list, path, feather, mask_feather);
        }
        if (display->expansion_canvas_px != 0.0F) {
            draw_mask_path(list, path, expansion, mask_expansion);
        }
        draw_mask_path(list, path, 4.5F, overlay_halo);
        const bool selected = display->source == frame.selection.mask;
        draw_mask_path(list, path, selected ? 2.2F : 1.4F,
                       selected ? mask_outline : mask_secondary);
        if (selected && display->kind != scene::MaskKind::Bezier) {
            const auto extent = mask_coordinate_extent(mask_canvas, input);
            const MaskViewportNormalizedPoint centre{
                (display->bounds.left + display->bounds.right) * 0.5F,
                (display->bounds.top + display->bounds.bottom) * 0.5F};
            const std::array local{
                MaskViewportNormalizedPoint{display->bounds.left,
                                            display->bounds.top},
                MaskViewportNormalizedPoint{display->bounds.right,
                                            display->bounds.top},
                MaskViewportNormalizedPoint{display->bounds.right,
                                            display->bounds.bottom},
                MaskViewportNormalizedPoint{display->bounds.left,
                                            display->bounds.bottom}};
            for (const auto point : local) {
                const auto screen = mask_screen_point(
                    frame.image, mask_canvas, input,
                    rotate_shape_point(point, display->bounds,
                                       display->rotation_turns, extent));
                if (screen.has_value()) {
                    draw_mask_point(list, *screen, mask_handle, 8.0F);
                    draw_mask_point(list, *screen, ctx.theme().track, 4.0F);
                }
            }
            const auto top = rotate_shape_point(
                {centre.x, display->bounds.top}, display->bounds,
                display->rotation_turns, extent);
            const auto centre_screen = mask_screen_point(
                frame.image, mask_canvas, input, centre);
            const auto top_screen = mask_screen_point(
                frame.image, mask_canvas, input, top);
            if (centre_screen.has_value() && top_screen.has_value()) {
                const float dx = top_screen->x - centre_screen->x;
                const float dy = top_screen->y - centre_screen->y;
                const float length = std::sqrt(dx * dx + dy * dy);
                if (length > 1.0F) {
                    const gui::UiPoint rotate_screen{
                        top_screen->x + dx / length * 24.0F,
                        top_screen->y + dy / length * 24.0F};
                    line(list, *top_screen, rotate_screen, 1.2F, mask_handle);
                    gui::add_diamond(list, rotate_screen, 5.0F, mask_handle);
                }
            }
        }
        if (!selected || display->kind != scene::MaskKind::Bezier) {
            continue;
        }
        for (const scene::MaskPoint& point : display->points) {
            const auto anchor = mask_screen_point(
                frame.image, mask_canvas, input, {point.x, point.y});
            if (!anchor.has_value()) {
                continue;
            }
            const bool point_selected =
                frame.selection.point == point.source;
            if (point_selected) {
                const auto in = mask_screen_point(
                    frame.image, mask_canvas, input,
                    {point.in_x, point.in_y});
                const auto out = mask_screen_point(
                    frame.image, mask_canvas, input,
                    {point.out_x, point.out_y});
                if (in.has_value() &&
                    point_distance(*anchor, in->x, in->y) > 2.0F) {
                    line(list, *anchor, *in, 1.2F, mask_handle);
                    draw_mask_point(list, *in, mask_handle, 5.0F);
                }
                if (out.has_value() &&
                    point_distance(*anchor, out->x, out->y) > 2.0F) {
                    line(list, *anchor, *out, 1.2F, mask_handle);
                    draw_mask_point(list, *out, mask_handle, 5.0F);
                }
            }
            draw_mask_point(list, *anchor,
                            point_selected ? mask_handle : mask_outline,
                            point_selected ? 8.0F : 6.0F);
        }
    }

    if (!frame.mapping_valid) {
        const gui::Rect notice{frame.image.x + 8.0F, frame.image.y + 8.0F,
                               244.0F, 23.0F};
        ctx.panel(notice, gui::UiColor{0.12F, 0.04F, 0.04F, 0.90F});
        ctx.label(gui::inset(notice, 6.0F, 0.0F),
                  "SOURCE MASK MAPPING UNAVAILABLE", ctx.theme().warning);
    }
    // A Masks tab changes what clicking the picture means. Keep that mode
    // visible in both Stage and Program Output instead of making a failed
    // object-selection click feel like a broken picker.
    // Program edits a mask only once one is selected, and goes back to the
    // layer on a click away; Stage is in this domain for as long as the Masks
    // tab is in front.
    const std::string_view selection_notice =
        role_ == ViewportRole::Program
            ? std::string_view{"Editing a mask - click away or press Esc "
                               "to finish"}
            : std::string_view{"Can't select while masking"};
    // Centred along the image's foot, 6 above it, sized to its words.
    gui::Rect foot = gui::inset(frame.image, gui::Padding{8.0F, 0.0F, 8.0F, 6.0F});
    foot = gui::cut_bottom(foot, 23.0F);
    const gui::Rect selection_notice_bounds = gui::centered(
        foot, std::min(foot.width, std::ceil(ctx.font_width(selection_notice)) + 16.0F),
        foot.height);
    ctx.panel(selection_notice_bounds,
              gui::UiColor{0.03F, 0.04F, 0.05F, 0.90F});
    ctx.label(gui::inset(selection_notice_bounds, 8.0F, 0.0F),
              selection_notice, ctx.theme().text_muted);
    const gui::UiColor coverage_body =
        coverage_interaction.held
        ? ctx.theme().widget_active
        : (input.coverage_visible ? ctx.theme().selection
                                  : (coverage_interaction.hovered
                                         ? ctx.theme().widget_hover
                                         : ctx.theme().widget));
    ctx.panel(coverage_button, coverage_body);
    ctx.label(coverage_button,
              input.coverage_visible ? "COVERAGE ON" : "COVERAGE",
              ctx.theme().text);
    ctx.pop_clip();

    if (coverage_interaction.clicked) {
        frame.intent = MaskViewportCoverageIntent{
            input.target.scene, input.target.target,
            !input.coverage_visible};
    }
}

const scene::MaskInstance*
ViewportPanel::MaskFrame::find_mask(doc::NodeId id) const {
    const auto found = std::find_if(
        masks.begin(), masks.end(),
        [id](const scene::MaskInstance* mask) noexcept {
            return mask->source == id;
        });
    return found == masks.end() ? nullptr : *found;
}

} // namespace mediaserver::editor
