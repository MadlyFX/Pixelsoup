#include "im_viewport.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace ajfx::studio {
namespace {

constexpr float handle_radius = 5.5F;
// The most line pieces Paths draws in a frame, so a dense effect cannot bury
// the interface's vertex budget.
constexpr std::size_t max_path_segments = 12000U;

float length(Vec3 v) { return std::sqrt(dot(v, v)); }

Vec3 normalized(Vec3 v, Vec3 fallback) {
    const float l = length(v);
    return l > 1.0e-6F ? v * (1.0F / l) : fallback;
}

// Where a ray meets the plane through `point` with `normal`, or nothing when
// it runs along the plane or away from it.
std::optional<Vec3> meet(Vec3 origin, Vec3 direction, Vec3 point, Vec3 normal) {
    const float facing = dot(direction, normal);
    if (std::fabs(facing) < 1.0e-3F) {
        return std::nullopt;
    }
    const float t = dot(point - origin, normal) / facing;
    if (!(t > 0.0F) || t > 1.0e4F) {
        return std::nullopt;
    }
    return origin + direction * t;
}

ImU32 path_color(const Color& c, float alpha) {
    const float peak = std::max({c.r, c.g, c.b, 1.0F});
    return ImGui::ColorConvertFloat4ToU32(
        ImVec4{c.r / peak, c.g / peak, c.b / peak, alpha * std::clamp(0.25F + c.a * 0.75F, 0.25F, 1.0F)});
}

ImVec2 point(const gui::UiPoint& p) { return ImVec2{p.x, p.y}; }

void draw_paths(ImDrawList* list, const StageCamera& camera, const ViewportInputs& in, float scale) {
    const Analysis& analysis = *in.analysis;
    const double step = static_cast<double>(std::max<std::uint64_t>(analysis.path_step, 1U));
    std::size_t total = 0U;
    for (const ParticlePath& path : analysis.paths) {
        if (path.layer == in.layer) {
            total += path.points.size() > 1U ? path.points.size() - 1U : 0U;
        }
    }
    const std::size_t stride = std::max<std::size_t>(1U, (total + max_path_segments - 1U) / max_path_segments);
    for (const ParticlePath& path : analysis.paths) {
        if (path.layer != in.layer) {
            continue;
        }
        const std::size_t last = path.points.size() - 1U;
        std::optional<gui::UiPoint> from = camera.project(path.points.front());
        for (std::size_t i = 0U; i < last;) {
            const std::size_t next = std::min(i + stride, last);
            const std::optional<gui::UiPoint> to = camera.project(path.points[next]);
            if (from && to) {
                list->AddLine(point(*from), point(*to), path_color(path.colors[i], 0.55F), scale);
            }
            from = to;
            i = next;
        }
        const double at = (in.frame - static_cast<double>(path.birth)) / step;
        if (at >= 0.0 && at <= static_cast<double>(path.points.size() - 1U)) {
            const auto k = static_cast<std::size_t>(at);
            const std::size_t next = std::min(k + 1U, path.points.size() - 1U);
            const float f = static_cast<float>(at - static_cast<double>(k));
            const Vec3 p = path.points[k] + (path.points[next] - path.points[k]) * f;
            if (const std::optional<gui::UiPoint> screen = camera.project(p)) {
                list->AddCircleFilled(point(*screen), 2.2F * scale, path_color(path.colors[k], 1.0F), 8);
            }
        }
    }
}

void draw_guides(ImDrawList* list, const StageCamera& camera, const GuideSet& guides, float scale) {
    const ImU32 focus = ImGui::GetColorU32(ImGuiCol_ButtonActive);
    const ImU32 quiet = IM_COL32(255, 255, 255, 56);
    std::vector<ImVec2> points;
    for (const Guide& guide : guides.guides) {
        for (const GuideStroke& stroke : guide.strokes) {
            ImU32 colour = guide.focused ? focus : quiet;
            if (stroke.faint) {
                colour = (colour & 0x00FFFFFFU) | (static_cast<ImU32>(((colour >> 24U) & 0xFFU) * 0.45F) << 24U);
            }
            const float thickness = (guide.focused && !stroke.faint ? 1.6F : 1.0F) * scale;
            points.clear();
            const auto flush = [&] {
                if (points.size() >= 2U) {
                    list->AddPolyline(points.data(), static_cast<int>(points.size()), colour, ImDrawFlags_None, thickness);
                }
                points.clear();
            };
            for (const Vec3& world : stroke.points) {
                if (const std::optional<gui::UiPoint> p = camera.project(world)) {
                    points.push_back(point(*p));
                } else {
                    flush();
                }
            }
            if (stroke.closed && !stroke.points.empty() && points.size() == stroke.points.size()) {
                points.push_back(points.front());
            }
            if (stroke.arrow && points.size() >= 2U) {
                const ImVec2 tip = points.back();
                const ImVec2 from = points[points.size() - 2U];
                const ImVec2 d{tip.x - from.x, tip.y - from.y};
                const float l = std::sqrt(d.x * d.x + d.y * d.y);
                if (l > 0.0F) {
                    const float len = (guide.kind == GuideKind::Field ? 5.0F : 8.0F) * scale;
                    const float half = (guide.kind == GuideKind::Field ? 2.5F : 4.0F) * scale;
                    const ImVec2 n{d.x / l, d.y / l};
                    const ImVec2 base{tip.x - n.x * len, tip.y - n.y * len};
                    list->AddTriangleFilled(tip, ImVec2{base.x - n.y * half, base.y + n.x * half},
                                            ImVec2{base.x + n.y * half, base.y - n.x * half}, colour);
                }
            }
            flush();
        }
    }
}

} // namespace

bool Viewport::handles(const StageCamera& camera, const gui::Rect& view, Document& document, const GuideSet& guides,
                       float scale) {
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImGuiIO& io = ImGui::GetIO();
    bool held_any = false;
    for (const Handle& handle : guides.handles) {
        const std::optional<gui::UiPoint> at = camera.project(handle.at);
        const bool dragged = drag_ && drag_->handle.block == handle.block && drag_->handle.control == handle.control &&
                             drag_->handle.end == handle.end;
        if (!at || (!view.contains(at->x, at->y) && !dragged)) {
            continue;
        }
        ImGui::PushID(static_cast<int>(handle.block));
        ImGui::PushID(handle.control.c_str());
        ImGui::PushID(handle.end);
        const float reach = 10.0F * scale;
        ImGui::SetCursorScreenPos(ImVec2{at->x - reach, at->y - reach});
        ImGui::InvisibleButton("##handle", ImVec2{reach * 2.0F, reach * 2.0F});
        const bool hovered = ImGui::IsItemHovered();
        const bool active = ImGui::IsItemActive();

        Vec3 origin{};
        Vec3 direction{};
        camera.ray(io.MousePos.x, io.MousePos.y, origin, direction);
        const Vec3 up{0.0F, 1.0F, 0.0F};
        // The plane a drag moves in, through the handle.
        const auto plane_hit = [&](const Handle& h, bool vertical) -> std::optional<Vec3> {
            switch (h.motion) {
            case HandleMotion::Floor:
                if (!vertical) {
                    return meet(origin, direction, h.at, up);
                }
                [[fallthrough]];
            case HandleMotion::Height: {
                const Vec3 facing = normalized(Vec3{camera.forward().x, 0.0F, camera.forward().z}, Vec3{0.0F, 0.0F, 1.0F});
                const std::optional<Vec3> p = meet(origin, direction, h.at, facing);
                return p ? std::optional<Vec3>{Vec3{h.at.x, p->y, h.at.z}} : std::nullopt;
            }
            case HandleMotion::Heading:
                return meet(origin, direction, h.origin, up);
            case HandleMotion::Radius:
                return meet(origin, direction, h.origin, camera.forward());
            case HandleMotion::Corner:
                return meet(origin, direction, h.at, camera.forward());
            }
            return std::nullopt;
        };

        if (ImGui::IsItemActivated()) {
            const bool vertical = io.KeyShift;
            const std::optional<Vec3> p = plane_hit(handle, vertical);
            Vec3 grab{};
            if (p && handle.motion != HandleMotion::Radius && handle.motion != HandleMotion::Heading) {
                grab = handle.at - *p;
            }
            drag_ = Drag{handle, grab, vertical};
        }
        const bool mine = drag_ && drag_->handle.block == handle.block && drag_->handle.control == handle.control &&
                          drag_->handle.end == handle.end;
        if (active && mine) {
            held_any = true;
            // Shift changes the plane mid-drag: the grab is taken afresh so
            // the handle stays under the pointer.
            const bool vertical = io.KeyShift && handle.motion == HandleMotion::Floor;
            if (vertical != drag_->vertical) {
                const std::optional<Vec3> p = plane_hit(handle, vertical);
                drag_->grab = p ? handle.at - *p : Vec3{};
                drag_->vertical = vertical;
            }
            if (const std::optional<Vec3> p = plane_hit(handle, vertical)) {
                Vec3 world = *p + drag_->grab;
                if (handle.motion == HandleMotion::Floor) {
                    world = vertical ? Vec3{handle.at.x, world.y, handle.at.z} : Vec3{world.x, handle.at.y, world.z};
                }
                if (std::optional<Value> value = handle_value(document, handle, world)) {
                    if (const auto place = document.find_block(handle.block)) {
                        Block& block = place->layer->blocks[place->index];
                        if (block.find(handle.control) == nullptr || !(*block.find(handle.control) == *value)) {
                            block.set(handle.control, std::move(*value));
                        }
                    }
                }
            }
        }

        const float radius = (handle_radius + (hovered || active ? 1.5F : 0.0F)) * scale;
        const ImVec2 centre = point(*at);
        list->AddCircleFilled(centre, radius + 1.5F * scale, IM_COL32(15, 15, 20, 255), 16);
        const ImU32 accent = ImGui::GetColorU32(ImGuiCol_ButtonActive);
        if (handle.motion == HandleMotion::Floor) {
            list->AddCircleFilled(centre, radius, accent, 16);
        } else {
            const float r = radius + 0.5F * scale;
            list->AddQuadFilled(ImVec2{centre.x, centre.y - r}, ImVec2{centre.x + r, centre.y}, ImVec2{centre.x, centre.y + r},
                                ImVec2{centre.x - r, centre.y}, accent);
        }
        if (hovered || active) {
            std::string tip = handle.label;
            switch (handle.motion) {
            case HandleMotion::Floor: tip += ": drag across the floor, hold Shift to raise or lower"; break;
            case HandleMotion::Height: tip += ": drag up or down"; break;
            case HandleMotion::Radius: tip += ": drag in or out"; break;
            case HandleMotion::Heading: tip += ": drag round to turn"; break;
            case HandleMotion::Corner: tip += ": drag the corner"; break;
            }
            ImGui::SetTooltip("%s", tip.c_str());
        }
        ImGui::PopID();
        ImGui::PopID();
        ImGui::PopID();
    }
    if (!held_any) {
        drag_.reset();
    }
    return held_any;
}

ViewportEvents Viewport::draw(const gui::Rect& view, const CameraPose& pose, Document& document,
                              const ViewportInputs& inputs, float scale) {
    ViewportEvents events;
    ImGui::SetNextWindowPos(ImVec2{view.x, view.y});
    ImGui::SetNextWindowSize(ImVec2{view.width, view.height});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0F, 0.0F});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4{0.0F, 0.0F, 0.0F, 0.0F});
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                       ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav;
    const bool open = ImGui::Begin("##viewport", nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!open) {
        ImGui::End();
        return events;
    }
    const float rounding = 8.0F * scale;
    ImDrawList* list = ImGui::GetWindowDrawList();
    const ImVec2 min{view.x, view.y};
    const ImVec2 max{view.x + view.width, view.y + view.height};
    if (inputs.has_picture) {
        list->AddImageRounded(inputs.picture, min, max, ImVec2{0.0F, 0.0F}, ImVec2{1.0F, 1.0F}, IM_COL32_WHITE, rounding);
    } else {
        list->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_FrameBg), rounding);
    }

    // The camera takes the pointer over the whole picture, but lets a handle
    // above it have it first.
    ImGui::SetCursorScreenPos(min);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##camera", ImVec2{view.width, view.height},
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle |
                               ImGuiButtonFlags_MouseButtonRight);
    const ImGuiIO& io = ImGui::GetIO();
    events.hovered = ImGui::IsItemHovered();
    // The frame a press lands on carries the pointer's whole jump from where
    // it was, which is no drag.
    if (ImGui::IsItemActive() && !ImGui::IsItemActivated()) {
        if (io.MouseDown[2]) {
            events.panning = true;
        } else if (io.MouseDown[1]) {
            events.dollying = true;
        } else if (io.MouseDown[0] && (io.MouseDelta.x != 0.0F || io.MouseDelta.y != 0.0F)) {
            events.orbiting = true;
        }
        events.drag = io.MouseDelta;
    }
    if (events.hovered) {
        events.wheel = io.MouseWheel;
    }
    if (ImGui::IsItemDeactivated() && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0F * scale)) {
        events.clicked = true;
        events.click_at = io.MousePos;
        events.ctrl = io.KeyCtrl;
        events.double_clicked = io.MouseClickedCount[0] >= 2;
    }

    const StageCamera camera{pose, view};
    list->PushClipRect(min, max, true);
    const bool any_path = inputs.analysis != nullptr && inputs.layer != 0U &&
                          std::any_of(inputs.analysis->paths.begin(), inputs.analysis->paths.end(),
                                      [&](const ParticlePath& path) { return path.layer == inputs.layer; });
    if (inputs.show_paths && any_path) {
        // The picture is veiled a little so the paths read over bright glow.
        list->AddRectFilled(min, max, IM_COL32(10, 10, 14, 90));
        draw_paths(list, camera, inputs, scale);
    }
    if (inputs.show_guides && inputs.guides != nullptr) {
        draw_guides(list, camera, *inputs.guides, scale);
    }
    list->PopClipRect();
    if (inputs.show_handles && inputs.guides != nullptr) {
        events.handle_held = handles(camera, view, document, *inputs.guides, scale);
    } else {
        drag_.reset();
    }
    ImGui::End();
    return events;
}

} // namespace ajfx::studio
