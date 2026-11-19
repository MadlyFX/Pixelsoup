#pragma once

// Tracked-camera pose channels and the Live Data panel projection.

#include "camera_prediction.hpp"
#include "tracked_camera_bridge.hpp"
#include "tracker_listener.hpp"

#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/live_data_panel.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/render/mapped_scene.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/track/pose_channel.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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
// Curved (horizontal arc) and imported UV-mesh faces are reported over their
// complete surface mesh, through `render::inner_frustum_mesh_region` - the
// same call the nodes size their plates with. The Director holds those meshes
// in `DirectorSurfaceMeshes`; until one is held, the face is reported as not
// known rather than as a camera that sees nothing.
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

// The complete surface mesh of every curved or UV-mesh face, as the Director
// reports plate rectangles over them.
//
// One `render::OutputProjectionCache` per surface, the same cache a node
// draws through: an arc is built once per authored change, and an imported
// file is read on the cache's background worker - never on the control
// thread, which only ever polls. An entry's whole-raster region mesh is the
// complete mesh by construction (an imported patch always owns its whole
// raster; an arc over its whole raster is `horizontal_arc_mesh` without a
// region). Entries are created when a surface first appears and dropped
// when it leaves the feed map.
class DirectorSurfaceMeshes final {
public:
    struct Answer final {
        // The complete mesh, or null.
        std::shared_ptr<const output::ProjectionMesh> mesh;
        // Imported meshes stay file-local; this places them on the stage.
        std::optional<output::ProjectionMeshPlacement> placement;
        // Why `mesh` is null, for the operator.
        std::string_view missing;
    };

    [[nodiscard]] Answer resolve(const output::ProjectionSurface &surface,
                                 const output::StagePlacement &stage,
                                 core::u64 asset_generation);

    // Drops entries for surfaces no longer in `feed_map`.
    void retain(const output::FeedMap &feed_map);

    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

private:
    struct Entry final {
        doc::NodeId surface{};
        std::unique_ptr<render::OutputProjectionCache> cache;
    };
    std::vector<Entry> entries_;
};

// Why the Director could not report a plate rectangle for a camera that may
// well be drawing one: a curved or UV-mesh face whose mesh it does not hold.
// Empty whenever the rectangle was derived or the camera truly sees nothing.
struct DirectorPlateUnknown final {
    std::string_view reason;
};

// `meshes` is optional: without it curved and UV-mesh faces are reported as
// unknown with that reason, never as "sees no part of the canvas".
[[nodiscard]] std::optional<scene::InnerFrustumRegion> director_inner_frustum_rect(
    const scene::Scene &frame, const scene::TrackedCameraView &view,
    const output::FeedMap &feed_map,
    DirectorSurfaceMeshes *meshes = nullptr,
    DirectorPlateUnknown *unknown = nullptr);

[[nodiscard]] std::string summarize_node_tracking(
    std::span<const editor::NodeTrackingObservation> nodes);

[[nodiscard]] editor::LiveDataState director_live_data(
    const server::TrackerListenerPool &listeners,
    std::span<const scene::TrackingSource> sources,
    const TrackedCameraChannels &channels,
    const server::DirectorPosePredictors &predictors,
    const scene::Scene &frame,
    const output::FeedMap &feed_map,
    core::u64 control_frame,
    core::u64 show_frame,
    std::vector<editor::NodeTrackingObservation> nodes,
    DirectorSurfaceMeshes *surface_meshes = nullptr);

} // namespace mediaserver::app
