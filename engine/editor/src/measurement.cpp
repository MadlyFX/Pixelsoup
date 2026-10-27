#include <mediaserver/editor/measurement.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace mediaserver::editor {

MeasureReading measure(scene::Vec3 from, scene::Vec3 to) noexcept {
    MeasureReading reading{};
    reading.delta = to - from;
    reading.length = scene::length(reading.delta);
    const float run = std::sqrt(reading.delta.x * reading.delta.x +
                                reading.delta.z * reading.delta.z);
    reading.slope_degrees =
        reading.length > 0.0F
            ? std::atan2(reading.delta.y, run) * 180.0F / std::numbers::pi_v<float>
            : 0.0F;
    return reading;
}

std::string format_metres(float metres) {
    std::array<char, 32U> text{};
    const int written = std::snprintf(text.data(), text.size(), "%.3f m",
                                      static_cast<double>(metres));
    return std::string{text.data(),
                       static_cast<std::size_t>(written > 0 ? written : 0)};
}

std::string format_reading_detail(const MeasureReading& reading) {
    // A negative zero reads as noise; print it as zero.
    const auto clean = [](float value) {
        return static_cast<double>(std::abs(value) < 0.0005F ? 0.0F : value);
    };
    std::array<char, 96U> text{};
    const int written = std::snprintf(
        text.data(), text.size(), "X %.3f  Y %.3f  Z %.3f  slope %.1f deg",
        clean(reading.delta.x), clean(reading.delta.y), clean(reading.delta.z),
        std::abs(reading.slope_degrees) < 0.05F
            ? 0.0
            : static_cast<double>(reading.slope_degrees));
    return std::string{text.data(),
                       static_cast<std::size_t>(written > 0 ? written : 0)};
}

float path_length(std::span<const MeasurePoint> points) noexcept {
    float total = 0.0F;
    for (std::size_t index = 1U; index < points.size(); ++index) {
        total += scene::length(points[index].position -
                               points[index - 1U].position);
    }
    return total;
}

void MeasureChain::click(const MeasurePoint& point, bool chain) {
    if (chain) {
        points.push_back(point);
        open = true;
        return;
    }
    if (open && !points.empty()) {
        points.push_back(point);
        open = false;
        return;
    }
    points.assign(1U, point);
    open = true;
}

void MeasureChain::clear() noexcept {
    points.clear();
    open = false;
}

std::string_view measure_snap_name(MeasureSnap snap) noexcept {
    switch (snap) {
    case MeasureSnap::Vertex:
        return "Vertex";
    case MeasureSnap::EdgeMidpoint:
        return "Midpoint";
    case MeasureSnap::Surface:
        return "Surface";
    case MeasureSnap::Floor:
        return "Floor";
    }
    return "Surface";
}

} // namespace mediaserver::editor
