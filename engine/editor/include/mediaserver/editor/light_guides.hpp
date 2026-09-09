#pragma once

#include <mediaserver/scene/scene.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <span>

namespace mediaserver::editor {

enum class LightGuideStyle { Source, InnerCone, OuterCone, Range, Direction };

struct LightGuideSegment final {
    scene::Vec3 a, b;
    LightGuideStyle style{};
};

// Editor geometry only. Fixed storage/tessellation, independent of the camera.
// Source dimensions are always exact; unselected spots use a short direction
// glyph. Selection expands them to the real radial cutoff and adds range guides.
struct LightGuide final {
    std::array<LightGuideSegment, 384U> storage{};
    std::size_t count{};
    [[nodiscard]] std::span<const LightGuideSegment> segments() const noexcept {
        return {storage.data(), count};
    }
};

[[nodiscard]] LightGuide light_guide(const scene::PointLight& light,
                                    bool selected) noexcept;
[[nodiscard]] LightGuide directional_light_guide(scene::Vec3 anchor,
                                                scene::Vec3 direction) noexcept;

// Clip before perspective divide, including edges crossing the eye/near plane.
// Returned points are canvas pixels; no enormous offscreen UI triangles.
[[nodiscard]] std::optional<std::array<scene::CanvasProjection, 2U>>
project_light_guide_segment(const LightGuideSegment& segment,
                            const scene::ViewMatrices& matrices,
                            scene::Canvas canvas) noexcept;

} // namespace mediaserver::editor
