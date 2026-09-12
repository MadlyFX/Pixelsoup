#pragma once
#include <mediaserver/scene/scene.hpp>
#include <string>

namespace mediaserver::render {
// Fixed-size transient data. Never belongs to a document, WAL or undo stack.
struct LinkedStageSample {
    core::u64 scene{}, control_frame{}, show_frame{}, camera{};
    scene::MediaFrameRate rate{60, 1};
    scene::Mat4 camera_world{scene::identity()};
    float sensor_width_mm{36}, sensor_height_mm{20.25F}, focal_length_mm{35};
    float near_m{0.1F}, far_m{1000}, focus_m{}, aperture{2.8F};
    bool tracked{}, held{}, calibrated{};
};
[[nodiscard]] LinkedStageSample linked_stage_sample(const scene::Scene& value, core::u64 control_frame) noexcept;
// Worker-only formatting. Alignment maps authoring coordinates to physical stage.
[[nodiscard]] std::string linked_stage_text(const LinkedStageSample& sample,
    const scene::Mat4& alignment = scene::identity());
} // namespace mediaserver::render
