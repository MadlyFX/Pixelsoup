#pragma once
#include "scene.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ajfx::studio {
// The timeline shows every item that has time in it, one track each, and
// the moments on it as markers the artist drags. Only the authored scene is
// written to; a marker is a view of one authored value.
enum class MarkerKind { Release, Move, Initial, Continuous, Burst, Blast };

struct TrackMarker final {
    MarkerKind kind{MarkerKind::Burst};
    std::uint32_t item{};
    std::uint32_t part{}; // the source for a Burst or Initial; 0 otherwise
    float seconds{};
    float duration{}; // a bar rather than a point when above 0
    bool draggable{true};
    [[nodiscard]] std::string_view label() const;
    friend bool operator==(const TrackMarker &, const TrackMarker &) = default;
};

struct Track final {
    std::uint32_t item{};
    std::string name;
    std::vector<TrackMarker> markers;
    friend bool operator==(const Track &, const Track &) = default;
};

// The tracks, in tree order (presentation only), skipping items with nothing
// timed: a static object that never releases, a group, a wind.
[[nodiscard]] std::vector<Track> collect_tracks(const Scene &scene, float loop_seconds);
// Moves a marker's moment, or its bar's length, writing the authored value.
// Clamped at 0 (a bar at 0.01 s). Returns whether the scene changed.
bool set_marker_time(Document &document, const TrackMarker &marker, float seconds);
bool set_marker_duration(Document &document, const TrackMarker &marker, float seconds);
} // namespace ajfx::studio
