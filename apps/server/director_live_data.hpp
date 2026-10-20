#pragma once

// Tracked-camera pose channels and the Live Data panel projection.

#include "camera_prediction.hpp"
#include "tracked_camera_bridge.hpp"
#include "tracker_listener.hpp"

#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/live_data_panel.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/track/pose_channel.hpp>

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mediaserver::app {

// The Live Data panel's view of the tracker, assembled from the socket's own
// counters and from the pose channel a node would be running.
//
// Two separate things are reported deliberately. The listener says what
// arrived on the wire; the channel says what a node would be *drawing*, which
// is the held pose whenever a frame has none. An operator reading a dead wall
// needs to tell those apart.
// Where the plate lands, for the operator's benefit.
//
// A node derives this for its own slice on the frame path; the Director
// derives it here purely to report it. Both go through the same
// `output::inner_frustum_region`, so what the panel says matches what the wall
// is doing rather than being a second opinion.
//
// Only planar faces are reported. A curved or imported face needs the
// mesh-bounded overload and its complete surface mesh, which the Director does
// not resolve on this path; reporting nothing for those is honest, whereas
// reporting a planar approximation of a curved wall would not be.
// One pose channel per tracked camera, keyed by the camera node.
//
// A channel is the hold-don't-extrapolate rule, and each camera needs its own:
// two cameras on a stage drop out independently, and one holding while the
// other is live is exactly the situation the wall has to survive. Entries are
// created when a camera first appears and then kept, so nothing here allocates
// on a settled frame path.
class TrackedCameraChannels final {
public:
    struct Entry final {
        doc::NodeId camera{};
        track::PoseChannel channel;
        server::TrackedCameraLens lens{};
        core::u64 accepted{};
    };

    [[nodiscard]] Entry &open(doc::NodeId camera) {
        for (Entry &entry : entries_) {
            if (entry.camera == camera) {
                return entry;
            }
        }
        entries_.push_back(Entry{camera, track::PoseChannel{}, {}, 0U});
        return entries_.back();
    }

    [[nodiscard]] const Entry *find(doc::NodeId camera) const noexcept {
        for (const Entry &entry : entries_) {
            if (entry.camera == camera) {
                return &entry;
            }
        }
        return nullptr;
    }

    [[nodiscard]] core::u64 accepted() const noexcept {
        core::u64 total = 0U;
        for (const Entry &entry : entries_) {
            total += entry.accepted;
        }
        return total;
    }

    [[nodiscard]] std::span<const Entry> entries() const noexcept {
        return entries_;
    }

    void reset() noexcept {
        for (Entry &entry : entries_) {
            entry.channel.reset();
            entry.lens = {};
        }
    }

private:
    std::vector<Entry> entries_;
};

void inject_tracked_poses(scene::Scene &scene,
                          TrackedCameraChannels &channels,
                          core::u64 control_frame, bool advance);

[[nodiscard]] std::optional<scene::InnerFrustumRegion> director_inner_frustum_rect(
    const scene::Scene &frame, const scene::TrackedCameraView &view,
    const output::FeedMap &feed_map);

[[nodiscard]] std::string summarize_node_tracking(
    std::span<const editor::NodeTrackingObservation> nodes);

[[nodiscard]] editor::LiveDataState director_live_data(
    const server::TrackerListenerPool &listeners,
    const TrackedCameraChannels &channels,
    const server::DirectorPosePredictors &predictors,
    const scene::Scene &frame,
    const output::FeedMap &feed_map,
    core::u64 control_frame,
    core::u64 show_frame,
    std::vector<editor::NodeTrackingObservation> nodes);

} // namespace mediaserver::app
