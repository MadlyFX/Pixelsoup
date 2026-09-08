#pragma once

#include <array>
#include <mediaserver/render/model_loader.hpp>
#include <span>

namespace mediaserver::render {

// Constructed exclusively from replicated scene state. No slice, output extent,
// render-view override, clock, or previous selection can enter this contract.
class GeometryFrusta final {
  public:
    explicit GeometryFrusta(const scene::Scene& frame) noexcept;
    [[nodiscard]] bool visible(const scene::Mat4& world,
                               const ModelBounds& bounds) const noexcept;

  private:
    std::array<std::array<scene::Vec4, 6U>, scene::max_tracked_cameras + 1U>
        planes_{};
    core::u32 count_{};
    bool conservative_{};
};

// Conservative projected AABB diameter, in authored canvas pixels. Near-plane
// intersections and unusable bounds select the finest level. Stateless by
// design.
[[nodiscard]] float projected_model_pixels(const scene::Scene& frame,
                                           const scene::Mat4& world,
                                           const ModelBounds& bounds) noexcept;
[[nodiscard]] core::u32
select_model_lod(const scene::Scene& frame, const scene::Mat4& world,
                 const ModelBounds& bounds,
                 std::span<const ModelLod> levels) noexcept;

struct GeometrySelection final {
    doc::NodeId source{};
    bool model{};
    bool visible{};
    core::u32 lod{};
    bool operator==(const GeometrySelection&) const = default;
};

} // namespace mediaserver::render
