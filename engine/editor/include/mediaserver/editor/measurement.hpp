#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/scene/math.hpp>

#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Stage's Measure tool: distances between points picked on the scene, in
// metres. Measurements are editor-only and temporary - nothing here is ever
// authored into the document.
namespace mediaserver::editor {

// What a measured point snapped to, best first.
enum class MeasureSnap : core::u8 {
    Vertex,
    EdgeMidpoint,
    Surface,
    // Nothing under the pointer: the stage floor at y = 0.
    Floor,
};

struct MeasurePoint final {
    scene::Vec3 position{};
    MeasureSnap snap{MeasureSnap::Surface};
    doc::NodeId element{};

    bool operator==(const MeasurePoint&) const = default;
};

struct MeasureReading final {
    float length{};
    // Second point minus first, per world axis.
    scene::Vec3 delta{};
    // Rise over the horizontal run, in degrees: 0 level, 90 straight up.
    float slope_degrees{};
};

[[nodiscard]] MeasureReading measure(scene::Vec3 from, scene::Vec3 to) noexcept;

// "3.214 m". Millimetres are the finest step an LED wall is built to.
[[nodiscard]] std::string format_metres(float metres);

// "X 1.200  Y -0.250  Z 2.981  slope 4.4 deg"
[[nodiscard]] std::string format_reading_detail(const MeasureReading& reading);

// The total length of a chain of points.
[[nodiscard]] float path_length(std::span<const MeasurePoint> points) noexcept;

// Where a click puts the next point. Without Shift, a click after a finished
// measurement starts a new one and the second click finishes it; Shift adds
// to the chain so a path can be measured. `open` says whether the last point
// still follows the pointer.
struct MeasureChain final {
    std::vector<MeasurePoint> points;
    bool open{};

    void click(const MeasurePoint& point, bool chain);
    void clear() noexcept;
};

[[nodiscard]] std::string_view measure_snap_name(MeasureSnap snap) noexcept;

} // namespace mediaserver::editor
