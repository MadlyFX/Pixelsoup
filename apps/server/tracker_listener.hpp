#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/track/pose_slot.hpp>
#include <mediaserver/track/tracked_pose.hpp>

#include <mediaserver/doc/ids.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Director's tracker socket, and the only one in the product.
//
// `engine/track` has no I/O by design, so this is where a datagram actually
// arrives. It lives in the Director process and nowhere else: a render node
// that sampled its own tracker would get a different pose from its neighbour
// and open a seam mid-wall, so there is exactly one reader and it fans its
// sample out over the replicated per-frame channel.
//
// The receive loop owns a thread and does nothing but read, parse, and publish
// into a `track::PoseSlot`. It never touches the frame path: the Director's
// frame boundary samples the slot, which cannot block on this thread, and a
// tracker that stalls or floods costs the show nothing but held poses.
namespace mediaserver::server {

enum class TrackerListenerState : core::u8 {
    // No source authored, or the authored source is switched off.
    Idle,
    // The socket could not be opened - a port already in use is the usual
    // cause, and it is sticky until the operator changes the source.
    BindFailed,
    // Listening, but nothing has arrived yet.
    Waiting,
    // Datagrams are arriving and parsing.
    Receiving,
};

[[nodiscard]] std::string_view tracker_listener_state_name(
    TrackerListenerState state) noexcept;

// Everything the operator's panel needs, as one already-computed snapshot.
// Reading it performs no syscall, no allocation, and no wait.
struct TrackerListenerStatus final {
    TrackerListenerState state{TrackerListenerState::Idle};
    scene::TrackingSourceSettings source{};
    // Total datagrams that arrived on the socket, and how they resolved.
    core::u64 datagrams{};
    core::u64 accepted{};
    // Refused by the parser: wrong length, wrong identifier, failed checksum,
    // a value that cannot describe a camera. Counted by kind so an operator
    // can tell a wrong-protocol source from a flaky cable.
    core::u64 truncated{};
    core::u64 unrecognized{};
    core::u64 checksum_failures{};
    core::u64 invalid_values{};
    // Parsed correctly but addressed to a different camera on the stage.
    core::u64 foreign_camera{};
    // The last pose that reached the slot, if any.
    std::optional<track::TrackedPose> last_pose;
    // Vertical field of view resolved from the authored sensor and the pose's
    // focal length, in turns. Zero when no pose has arrived.
    scene::Turns field_of_view{};
    // The socket's last error text, for the state above. Empty when none.
    std::string last_error;

    [[nodiscard]] bool listening() const noexcept {
        return state == TrackerListenerState::Waiting
            || state == TrackerListenerState::Receiving;
    }
};

// One supervised UDP receiver.
//
// `configure` is the only control: it is idempotent for an unchanged source,
// and a changed one closes the old socket and opens the new. It never blocks
// the caller on the receive thread beyond a bounded join.
class TrackerListener final {
public:
    TrackerListener();
    TrackerListener(const TrackerListener&) = delete;
    TrackerListener& operator=(const TrackerListener&) = delete;
    TrackerListener(TrackerListener&&) = delete;
    TrackerListener& operator=(TrackerListener&&) = delete;
    ~TrackerListener();

    // Applies an authored source. Returns false only when the socket could not
    // be opened, which is also visible in the status.
    bool configure(const scene::TrackingSourceSettings& source) noexcept;
    void stop() noexcept;

    [[nodiscard]] TrackerListenerStatus status() const;

    // The frame boundary's read. Non-blocking, never waits on the receive
    // thread, and absent until a pose has been published.
    [[nodiscard]] std::optional<track::PoseSample> sample() const noexcept;

    // Parses, counts, and publishes one datagram. The receive loop calls this
    // for every packet it reads, and a test calls it directly to exercise the
    // same path without binding a port.
    bool deliver(std::span<const std::byte> datagram) noexcept;

private:
    // Issues one asynchronous receive and re-issues from its own completion,
    // so the socket is always armed and a datagram wakes the thread the moment
    // it lands rather than at the next tick of a poll.
    void arm_receive() noexcept;

    struct State;
    std::unique_ptr<State> state_;
};

// One listener per authored source.
//
// A volume has more than one camera on it, and each names its own feed. The
// pool is what turns the authored list into open sockets: it opens what was
// added, closes what was removed, and leaves an unchanged source's socket
// alone so editing a sensor height mid-take does not drop a link.
//
// Bounded by `scene::max_tracking_sources`, so a runaway document is refused
// rather than absorbed into an unbounded set of sockets.
class TrackerListenerPool final {
public:
    TrackerListenerPool();
    TrackerListenerPool(const TrackerListenerPool &) = delete;
    TrackerListenerPool &operator=(const TrackerListenerPool &) = delete;
    TrackerListenerPool(TrackerListenerPool &&) = delete;
    TrackerListenerPool &operator=(TrackerListenerPool &&) = delete;
    ~TrackerListenerPool();

    // Applies the authored list. Idempotent for an unchanged list.
    void configure(const std::vector<scene::TrackingSource> &sources);
    void stop() noexcept;

    // The pose slot for one source, or nothing when that source is not open.
    // A zero node - no camera bound to anything - is always nothing.
    [[nodiscard]] std::optional<track::PoseSample>
    sample(doc::NodeId source) const noexcept;

    [[nodiscard]] TrackerListenerStatus
    status(doc::NodeId source) const noexcept;

    // The lens angle and sensor shape the Director resolved for one source.
    // Zero and one for a source that is not open, which `stamped_tracker_sample`
    // then refuses - a pose with no lens is not a camera.
    [[nodiscard]] scene::Turns field_of_view(doc::NodeId source) const noexcept;
    [[nodiscard]] float aspect_ratio(doc::NodeId source) const noexcept;

    [[nodiscard]] std::size_t open_sources() const noexcept;

private:
    struct Entry final {
        doc::NodeId node{};
        std::string name;
        std::unique_ptr<TrackerListener> listener;
    };

    [[nodiscard]] const Entry *find(doc::NodeId source) const noexcept;

    std::vector<Entry> entries_;
};

} // namespace mediaserver::server
