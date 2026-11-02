#include "sandbox_tools.hpp"

#include <mediaserver/editor/operator_widgets.hpp>

#include <algorithm>
#include <cmath>
#include <string>

namespace ajfx::studio {
namespace {

namespace editor = mediaserver::editor;

std::size_t slot_of(SandboxTool tool) noexcept {
    return static_cast<std::size_t>(tool);
}

std::string_view tool_tip(SandboxTool tool) noexcept {
    switch (tool) {
    case SandboxTool::Select:
        return "Select: click a thing to open its block";
    case SandboxTool::Drop:
        return "Drop a thing: a crate falls where you click, at the playhead";
    case SandboxTool::Explode:
        return "Explode here: an explosion where you click, at the playhead";
    case SandboxTool::Push:
        return "Push: shoves what you click away from you, at the playhead";
    }
    return {};
}

// The way a push goes: away from the camera, never down into the floor.
Vec3 away_from(const StageCamera& camera) {
    const Vec3 forward = camera.forward();
    const Vec3 way{forward.x, std::max(forward.y, 0.0F), forward.z};
    const float length = std::sqrt(dot(way, way));
    return length > 1.0e-4F ? way * (1.0F / length) : Vec3{0.0F, 0.0F, -1.0F};
}

} // namespace

std::optional<gui::Rect> SandboxTools::button(SandboxTool tool) const noexcept {
    return buttons_[slot_of(tool)];
}

bool SandboxTools::draw_strip(gui::UiContext& ctx, float x, float y, Document& document, Uid reroll) {
    const gui::Theme& theme = ctx.theme();
    float at = y;
    for (const SandboxTool tool : sandbox_tools) {
        const gui::Rect key{x, at, key_size, key_size};
        buttons_[slot_of(tool)] = key;
        const gui::UiId id = part_id(0U, "studio.sandbox", static_cast<std::uint32_t>(slot_of(tool)));
        const bool on = tool_ == tool;
        bool clicked = false;
        if (tool == SandboxTool::Explode) {
            clicked = glyph_key(ctx, id, key, draw_burst, on, tool_tip(tool), theme.accent);
        } else {
            const gui::Icon icon = tool == SandboxTool::Select ? gui::Icon::Select
                                   : tool == SandboxTool::Drop ? gui::Icon::Surface
                                                               : gui::Icon::MoveTool;
            clicked = editor::tool_key(ctx, id, key, icon, on, tool_tip(tool), theme.accent);
        }
        if (clicked) {
            // A tool clicked again goes back to selecting.
            tool_ = on ? SandboxTool::Select : tool;
        }
        at += key_size + key_gap;
    }
    at += key_gap * 2.0F;
    const gui::Rect die{x, at, key_size, key_size};
    dice_ = die;
    const Layer* layer = document.layer(reroll);
    // Kept with the strip, so the text outlives the frame whatever the
    // tooltip keeps of it.
    tip_ = layer != nullptr && !layer->name.empty() ? "Try another random seed for " + layer->name
                                                    : std::string{"Try another random seed"};
    if (glyph_key(ctx, part_id(0U, "studio.sandbox.dice"), die, draw_dice, false, tip_, theme.accent)) {
        return reroll_seed(document, layer != nullptr ? reroll : 0U);
    }
    return false;
}

std::optional<SandboxResult> SandboxTools::use_click(gui::UiContext& ctx, const gui::Interaction& view,
                                                     const StageCamera& camera, Document& document,
                                                     const SandboxScene& scene) {
    if (view.pressed) {
        press_ = gui::UiPoint{ctx.pointer_x(), ctx.pointer_y()};
    }
    if (!view.released) {
        return std::nullopt;
    }
    const std::optional<gui::UiPoint> from = press_;
    press_.reset();
    if (!view.clicked || !from) {
        return std::nullopt;
    }
    const float x = ctx.pointer_x();
    const float y = ctx.pointer_y();
    if (std::hypot(x - from->x, y - from->y) > sandbox_click_travel) {
        return std::nullopt;
    }
    Vec3 origin{};
    Vec3 direction{};
    camera.ray(x, y, origin, direction);
    const std::optional<StageHit> hit = pick_stage(scene.bodies, origin, direction, scene.floor);
    if (!hit) {
        return std::nullopt;
    }
    std::optional<SandboxEdit> made;
    switch (tool_) {
    case SandboxTool::Select:
        if (!hit->on_body || (hit->block == 0U && hit->layer == 0U)) {
            return std::nullopt;
        }
        return SandboxResult{SandboxTool::Select, hit->layer, hit->block};
    case SandboxTool::Drop:
        made = drop_thing(document, scene.layer, hit->point, scene.seconds,
                          sandbox_material(document, scene.selected, scene.layer));
        break;
    case SandboxTool::Explode:
        made = explode_here(document, scene.layer, hit->point, scene.seconds);
        break;
    case SandboxTool::Push:
        // A thing is pushed at its middle; the floor where it was clicked.
        made = push_here(document, scene.layer, hit->on_body ? hit->center : hit->point, away_from(camera),
                         scene.seconds);
        break;
    }
    if (!made) {
        return std::nullopt;
    }
    return SandboxResult{tool_, made->layer, made->block};
}

} // namespace ajfx::studio
