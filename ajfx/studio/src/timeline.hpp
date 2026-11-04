#pragma once
#include "scene.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ajfx::studio {
// The timeline shows every item that has time in it, one track each, and
// the moments on it as markers the artist drags. Only the authored scene is
// written to; a marker is a view of one authored value.
// Drop, Delete, Push, Grab and Paint are acts of the scene's performance, on
// the track of the Object dropped, painted, or whose body was acted on; their
// `part` is the act's serial (for Paint, the stroke's: its first dab's). A
// grab and a stroke are bars over the steps they last. Blasts set off by
// hand (Blasted) are on a track of their own, item 0, after the rest.
enum class MarkerKind { Release, Move, Initial, Continuous, Burst, Blast, Drop, Delete, Push, Grab, Paint, Blasted };
// Whether a marker is an act of the performance (its part a serial).
[[nodiscard]] constexpr bool act_marker(MarkerKind kind) noexcept {
    return kind == MarkerKind::Drop || kind == MarkerKind::Delete || kind == MarkerKind::Push ||
           kind == MarkerKind::Grab || kind == MarkerKind::Paint || kind == MarkerKind::Blasted;
}
// The name of the track blasts set off by hand are on.
inline constexpr std::string_view blasts_track = "Blasts";

struct TrackMarker final {
    MarkerKind kind{MarkerKind::Burst};
    std::uint32_t item{};
    std::uint32_t part{}; // the source for a Burst or Initial, the act for a Drop or Delete; 0 otherwise
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
// timed: a static object that never releases and was never dropped, a group,
// a wind. Scenes step at scene_step_rate, so an act's tick is its time.
[[nodiscard]] std::vector<Track> collect_tracks(const Scene &scene, float loop_seconds);
// Moves a marker's moment, or its bar's length, writing the authored value.
// Clamped at 0 (a bar at 0.01 s). Returns whether the scene changed.
inline constexpr std::uint32_t scene_step_rate = 60U;
bool set_marker_time(Document &document, const TrackMarker &marker, float seconds);
bool set_marker_duration(Document &document, const TrackMarker &marker, float seconds);
// Takes away the act a marker stands for: a whole stroke for Paint, the act
// (and what was done to the bodies it made) otherwise.
bool remove_marker_act(Document &document, const TrackMarker &marker);
} // namespace ajfx::studio
