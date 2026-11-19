#include "pointer_overlay_pass.hpp"

#include <mediaserver/gui/ui_renderer.hpp>
#include <mediaserver/output/perspective_mapping.hpp>
#include <mediaserver/scene/raycast.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

namespace mediaserver::ui {
namespace {

// How far along a canvas pixel's line of sight the point is placed before a
// surface view projects it. Any distance in front of the eye lands on the
// same pixel, because every surface view looks from that eye; this one stays
// well clear of the near plane.
constexpr float sight_distance = 10.0F;

[[nodiscard]] scene::Canvas size_of(scene::CanvasRect rect) noexcept {
    return scene::Canvas{std::max(rect.width, 1U), std::max(rect.height, 1U)};
}

} // namespace

std::optional<WindowPoint> pointer_to_window(const scene::Scene& frame,
                                             const PointerMapView& map,
                                             show::PointerPoint point) noexcept {
    const auto place = [&map](float x, float y) {
        return WindowPoint{x + static_cast<float>(map.viewport.x),
                           y + static_cast<float>(map.viewport.y)};
    };
    if (map.projection == output::FeedProjection::CanvasSlice) {
        const scene::Canvas source = size_of(map.source);
        const scene::Canvas local = size_of(map.viewport);
        return place((point.x - static_cast<float>(map.source.x))
                         * static_cast<float>(local.width)
                         / static_cast<float>(source.width),
                     (point.y - static_cast<float>(map.source.y))
                         * static_cast<float>(local.height)
                         / static_cast<float>(source.height));
    }
    if (map.mapped == nullptr) {
        return std::nullopt;
    }
    const std::optional<scene::Ray> sight =
        scene::canvas_ray(frame.camera, frame.canvas, point.x, point.y);
    if (!sight.has_value()) {
        return std::nullopt;
    }
    const scene::Vec3 world =
        sight->origin + sight->direction * sight_distance;
    const scene::Canvas local = size_of(map.viewport);
    const render::MappedProjection& mapped = *map.mapped;
    scene::CanvasProjection projected{};
    if (mapped.retained_mesh != nullptr && mapped.mesh_view.has_value()) {
        projected = mapped.mesh_placement.has_value()
            ? output::project_through_mesh_warp(*mapped.retained_mesh,
                                                *mapped.mesh_view, local, world,
                                                *mapped.mesh_placement)
            : output::project_through_mesh_warp(*mapped.retained_mesh,
                                                *mapped.mesh_view, local,
                                                world);
    } else {
        projected = scene::project_to_canvas(mapped.render_view.matrices,
                                             local, world);
    }
    if (!projected.visible || !std::isfinite(projected.x)
        || !std::isfinite(projected.y)) {
        return std::nullopt;
    }
    return place(projected.x, projected.y);
}

struct PointerOverlayPass::Impl final {
    gui::UiRenderer renderer;
    gui::UiDrawList list;
    std::vector<gui::UiPoint> line;
    bool atlas_uploaded{};
};

std::optional<PointerOverlayPass>
PointerOverlayPass::create(rhi::Device& device, rhi::TextureFormat format) {
    if (!gui::UiRenderer::shaders_available()) {
        return std::nullopt;
    }
    auto renderer = gui::UiRenderer::create(device, format);
    if (!renderer) {
        return std::nullopt;
    }
    auto impl = std::unique_ptr<Impl>(
        new Impl{std::move(renderer).value(), gui::UiDrawList{}, {}, false});
    // The atlas is one white texel, sampled at its middle.
    impl->list.set_white_texel(0.5F, 0.5F);
    // Every stroke's points, so recording never grows it.
    impl->line.reserve(show::max_pointer_points);
    return PointerOverlayPass{std::move(impl)};
}

PointerOverlayPass::PointerOverlayPass(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
PointerOverlayPass::PointerOverlayPass(PointerOverlayPass&&) noexcept = default;
PointerOverlayPass&
PointerOverlayPass::operator=(PointerOverlayPass&&) noexcept = default;
PointerOverlayPass::~PointerOverlayPass() = default;

bool PointerOverlayPass::record(rhi::Device& device,
                                rhi::CommandList& commands,
                                rhi::Texture& target,
                                const show::PointerOverlay& overlay,
                                const scene::Scene& frame,
                                std::span<const PointerMapView> maps) {
    if (impl_ == nullptr || overlay.empty() || maps.empty()) {
        return true;
    }
    Impl& state = *impl_;
    if (!state.atlas_uploaded) {
        constexpr std::array<core::u8, 4U> white{255U, 255U, 255U, 255U};
        if (!state.renderer.upload_atlas(device, commands, white, 1U, 1U)) {
            return false;
        }
        state.atlas_uploaded = true;
    }
    const gui::UiColor red{show::pointer_red_r, show::pointer_red_g,
                           show::pointer_red_b, 1.0F};
    const float canvas_height = static_cast<float>(frame.canvas.height);
    gui::UiDrawList& list = state.list;
    list.reset();
    list.set_white_texel(0.5F, 0.5F);
    for (const PointerMapView& map : maps) {
        // How many window pixels one canvas pixel takes in this map, measured
        // where the pointer is, so a surface view's sizes follow its scale.
        const show::PointerPoint probe = overlay.dot.value_or(
            overlay.points.empty()
                ? show::PointerPoint{static_cast<float>(frame.canvas.width)
                                         * 0.5F,
                                     canvas_height * 0.5F}
                : overlay.points.front());
        const auto a = pointer_to_window(frame, map, probe);
        const auto b = pointer_to_window(
            frame, map, show::PointerPoint{probe.x, probe.y + 8.0F});
        float scale = static_cast<float>(map.viewport.height)
            / static_cast<float>(std::max(map.source.height, 1U));
        if (a.has_value() && b.has_value()) {
            const float measured = std::hypot(b->x - a->x, b->y - a->y) / 8.0F;
            if (std::isfinite(measured) && measured > 0.0F) {
                scale = measured;
            }
        }
        const float width = std::max(
            canvas_height * show::pointer_line_width_per_canvas_height * scale,
            2.0F);
        const float radius = std::max(
            canvas_height * show::pointer_dot_radius_per_canvas_height * scale,
            3.0F);

        list.push_clip(static_cast<float>(map.viewport.x),
                       static_cast<float>(map.viewport.y),
                       static_cast<float>(map.viewport.width),
                       static_cast<float>(map.viewport.height));
        core::u32 start = 0U;
        for (const core::u32 end : overlay.stroke_ends) {
            state.line.clear();
            const auto flush = [&] {
                if (state.line.size() >= 2U) {
                    list.add_polyline(state.line, width, red);
                } else if (state.line.size() == 1U) {
                    const gui::UiPoint at = state.line.front();
                    list.add_rounded_rect(at.x - width, at.y - width,
                                          width * 2.0F, width * 2.0F, width,
                                          red);
                }
                state.line.clear();
            };
            for (core::u32 index = start; index < end; ++index) {
                const auto at =
                    pointer_to_window(frame, map, overlay.points[index]);
                if (!at.has_value()) {
                    // Behind this view: the stroke breaks rather than
                    // jumping across the picture.
                    flush();
                    continue;
                }
                state.line.push_back(gui::UiPoint{at->x, at->y});
            }
            flush();
            start = end;
        }
        if (overlay.dot.has_value()) {
            if (const auto at = pointer_to_window(frame, map, *overlay.dot)) {
                list.add_rounded_glow(at->x - radius, at->y - radius,
                                      radius * 2.0F, radius * 2.0F, radius,
                                      radius * 1.5F,
                                      gui::UiColor{red.r, red.g, red.b, 0.55F});
                list.add_rounded_rect(at->x - radius, at->y - radius,
                                      radius * 2.0F, radius * 2.0F, radius,
                                      red);
                // A hot core, as a laser spot reads.
                const float core_radius = radius * 0.45F;
                list.add_rounded_rect(at->x - core_radius, at->y - core_radius,
                                      core_radius * 2.0F, core_radius * 2.0F,
                                      core_radius,
                                      gui::UiColor{1.0F, 0.86F, 0.82F, 1.0F});
            }
        }
        list.pop_clip();
    }
    return state.renderer.render(device, commands, target,
                                 rhi::Color{0.0F, 0.0F, 0.0F, 0.0F}, list,
                                 target.width(), target.height(), true);
}

} // namespace mediaserver::ui
