#pragma once

#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/render/mapped_scene.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/show/pointer_overlay.hpp>

#include <memory>
#include <optional>
#include <span>

// Draws the operator's laser pointer over a node's finished window image.
//
// The pointer arrives in authored canvas pixels. A window holds one or more
// output maps, each a slice of the canvas or a physical surface seen from the
// scene camera's eye, so each point is carried into every map the way the
// editor's Program view carries scene geometry into a connector image: a
// slice by offset and scale, a surface by sending the canvas pixel's line of
// sight through that map's own view and warp. Drawn last, after the display
// transform, so the dot is the same red whatever the show's grade.
namespace mediaserver::ui {

// One output map as the pointer sees it. `mapped` is the view the map was
// rendered with; null for a canvas slice, which needs none.
struct PointerMapView final {
    scene::CanvasRect viewport{};
    scene::CanvasRect source{};
    output::FeedProjection projection{output::FeedProjection::CanvasSlice};
    const render::MappedProjection* mapped{};
};

struct WindowPoint final {
    float x{};
    float y{};
};

// Where canvas pixel `point` lands in the window, through `map`. Empty when
// the map cannot see it: behind a surface view, or a surface map without a
// resolved view.
[[nodiscard]] std::optional<WindowPoint>
pointer_to_window(const scene::Scene& frame, const PointerMapView& map,
                  show::PointerPoint point) noexcept;

class PointerOverlayPass final {
  public:
    // Builds the pipeline for targets of `format`. Empty when the UI shaders
    // are not in this build or the device refuses the pipeline.
    [[nodiscard]] static std::optional<PointerOverlayPass>
    create(rhi::Device& device, rhi::TextureFormat format);

    PointerOverlayPass(PointerOverlayPass&&) noexcept;
    PointerOverlayPass& operator=(PointerOverlayPass&&) noexcept;
    ~PointerOverlayPass();

    // Draws `overlay` over `target` through each of `maps`, clipped to each
    // map's viewport. Nothing is recorded for an empty overlay.
    [[nodiscard]] bool record(rhi::Device& device, rhi::CommandList& commands,
                              rhi::Texture& target,
                              const show::PointerOverlay& overlay,
                              const scene::Scene& frame,
                              std::span<const PointerMapView> maps);

  private:
    struct Impl;
    explicit PointerOverlayPass(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace mediaserver::ui
