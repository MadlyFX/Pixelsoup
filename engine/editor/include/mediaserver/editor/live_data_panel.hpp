#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/scene.hpp>

#include <mediaserver/doc/ids.hpp>

#include <cstddef>
#include <optional>
#include <vector>
#include <span>
#include <string>
#include <string_view>

// Live Data: what is actually arriving from the stage, right now.
//
// Everything on this panel is a transient observation. Nothing here is written
// back to the project, and nothing here can be edited - a source is authored as
// a scene node and bound to a camera in the Inspector, and this says whether
// that authored source is working.
// Keeping the two apart matters on a stage: an operator staring at a dead wall
// needs to know whether the tracker is silent, sending to the wrong camera, or
// sending fine while something downstream is holding.
//
// It is a docked panel rather than a page of a modal dialog because it is a
// monitoring surface. An operator watches it during a take; a dialog they have
// to open, read, and dismiss cannot do that job.
namespace mediaserver::editor {

// Mirrors the Director's tracker listener state, which the editor cannot name
// directly: the listener lives in the host process and this library must not
// depend on it. The host converts.
enum class TrackingLinkState : core::u8 {
  // No source authored, or the authored source is switched off.
  Idle,
  // The socket could not be opened. A port already in use is the usual cause.
  BindFailed,
  // Listening, but nothing has arrived yet.
  Waiting,
  // Datagrams are arriving and parsing.
  Receiving,
};

[[nodiscard]] std::string_view tracking_link_state_name(
    TrackingLinkState state) noexcept;
[[nodiscard]] std::string_view pose_prediction_state_name(
    scene::PosePredictionState state) noexcept;

// One tracker's live state, as the host observed it. Plain values only, so the
// panel can render a frame without reaching into anything that might be
// changing underneath it.
struct TrackingRuntimeObservation final {
  TrackingLinkState state{TrackingLinkState::Idle};
  // The authored source this describes, so the panel can show what it is
  // waiting for without a second lookup.
  scene::TrackingSourceSettings source{};

  core::u64 datagrams{};
  core::u64 accepted{};
  core::u64 truncated{};
  core::u64 unrecognized{};
  core::u64 checksum_failures{};
  core::u64 invalid_values{};
  // Parsed, but addressed to a different camera on the stage. Its own count
  // because it is the one refusal an operator fixes by changing a setting
  // rather than a cable.
  core::u64 foreign_camera{};
  // NatNet: frames that arrived before the server had answered the version
  // query, and frames in which the rigid body was listed but not solved.
  core::u64 awaiting_handshake{};
  core::u64 untracked{};
  // What a NatNet server said it was, once it answered. Empty otherwise.
  std::string server_description;

  // The last pose that reached the Director, if any.
  bool has_pose{};
  scene::Vec3 position{};
  scene::Turns yaw{};
  scene::Turns pitch{};
  scene::Turns roll{};
  // Vertical angle the Director resolved from the authored sensor and the
  // pose's focal length.
  scene::Turns field_of_view{};
  core::u64 tracker_sequence{};

  // The per-frame channel, seen from the Director. `held_frames` counts show
  // frames answered by holding the previous pose, which is what a node does
  // when a pose does not arrive - never extrapolating.
  core::u64 poses_sent{};
  core::u64 held_frames{};
  core::u64 longest_hold{};
  scene::PosePredictionTelemetry prediction{};

  // Populated for BindFailed, and for a socket error while listening.
  std::string last_error;

  bool operator==(const TrackingRuntimeObservation &) const = default;
};

// One tracked camera, as the Director observed it this frame.
//
// A stage runs several, and they fail independently: one tracker dropping out
// while the other keeps running is exactly the situation an operator has to be
// able to read at a glance, so every camera gets its own row rather than the
// panel reporting whichever one happened to be first.
struct TrackedCameraObservation final {
  doc::NodeId camera{};
  // What the operator calls it, so the panel names a camera rather than a node
  // number.
  std::string name;
  // The feed's live state and counters.
  TrackingRuntimeObservation tracking{};
  // How this camera draws its plate, as authored. Whether it asked for one at
  // all is in here: a tracked camera with its inner frustum switched off is a
  // real thing to author - a witness camera, or one parked between setups -
  // and the panel says so rather than reporting it as a failure.
  scene::IcvfxCameraSettings icvfx{};
  // The inner-frustum rectangle the cohort derived for it this frame, in
  // canvas pixels. Absent when this camera is drawing no plate.
  std::optional<scene::CanvasRect> inner_rect{};
  // The rectangle is the whole-wall fallback rather than a fit.
  //
  // It means the camera is looking at the wall but its frustum is not bounded
  // by it, so the conservative answer is to cover everything. That is correct
  // and it is expensive: the plate costs every pixel of the wall and the
  // second render stops being the saving it exists to be. Reporting only the
  // coverage percentage hides it, because "100 %" looks like a camera framing
  // the wall exactly rather than one overshooting it.
  bool inner_rect_clamped{};
  // Why the Director could not derive the rectangle when `inner_rect` is
  // absent although the camera may well be drawing a plate: a curved or
  // UV-mesh wall whose mesh the Director does not hold (yet). Empty when the
  // rectangle was derived or the camera genuinely sees no wall.
  std::string inner_rect_unknown;

  bool operator==(const TrackedCameraObservation &) const = default;
};

// What one render node did with the poses the Director sent it, as that node
// last reported over its heartbeat.
//
// The Director cannot infer any of this. Its own channels say what it sent;
// only the node knows whether the pose arrived, whether the frame it answered
// was the one the pose was stamped for, and whether the slice it draws even
// intersected the plate. Those three failures look identical from here - a
// wall with no plate on it - and they have three different fixes.
struct NodeTrackingObservation final {
  core::u32 node_index{};
  // What the operator calls the machine.
  std::string name;
  // The node has a live session and has reported at least once. False is a
  // real state, not a gap: a node that has never reported is a node whose
  // plate nobody can vouch for.
  bool reported{};

  core::u64 pose_sequence{};
  core::u64 poses_missed{};
  // Received here and overwritten before the render loop took it. A different
  // machine's problem from poses_missed, and the panel must not merge them.
  core::u64 poses_coalesced{};
  core::u64 poses_applied{};
  core::u64 held_frames{};
  core::u64 plates_drawn{};
  // Not a failure. On a wide wall most outputs miss the plate rectangle, and
  // that skip is the entire performance argument for a second frustum.
  core::u64 plates_skipped{};
  core::u64 plate_refusals{};

  bool operator==(const NodeTrackingObservation &) const = default;
};

// What the whole ICVFX path is doing this frame, beside the trackers feeding
// it. Absent members mean the Director had nothing to report.
// One rigid body a NatNet server described, as an operator picks it.
struct TrackingRigidBody final {
  core::i32 id{};
  std::string name;

  bool operator==(const TrackingRigidBody &) const = default;
};

// One authored feed's link, whether or not any camera is bound to it. This is
// what the feed settings and a camera's rigid body picker read: the bodies a
// Motive server says it has are discovered, never authored, so they reach the
// editor here rather than through the document.
struct TrackingFeedObservation final {
  doc::NodeId source{};
  TrackingLinkState state{TrackingLinkState::Idle};
  // What a NatNet server said it was, and the address it is being reached
  // at, once known. Empty otherwise.
  std::string server_description;
  std::string server_address;
  std::vector<TrackingRigidBody> rigid_bodies;
  core::u64 accepted{};
  std::string last_error;

  bool operator==(const TrackingFeedObservation &) const = default;
};

[[nodiscard]] const TrackingFeedObservation *
find_feed(std::span<const TrackingFeedObservation> feeds,
          doc::NodeId source) noexcept;

struct LiveDataState final {
  // Every tracked camera, in composite order: the last one listed is the one
  // on top where two plates overlap.
  std::vector<TrackedCameraObservation> cameras;
  // The authored show-wide policy, so the panel can say whether a live pose is
  // actually being used for anything.
  scene::IcvfxSettings icvfx{};
  scene::Canvas canvas{};
  // Every render node, as it last reported. Empty on a single-machine show,
  // where the Director draws its own walls and the counters above are the
  // whole story.
  std::vector<NodeTrackingObservation> nodes;
  // Show frame the values above belong to.
  core::u64 show_frame{};
  // Every authored feed's link, bound or not.
  std::vector<TrackingFeedObservation> feeds;

  bool operator==(const LiveDataState &) const = default;
};

// What the inner-frustum section has to say.
//
// Derived rather than decided inline while drawing, because the three failing
// states need three different fixes and an operator has to be able to tell
// which one they are looking at. A pure function is also the only way to test
// that without rendering text.
enum class InnerFrustumReport : core::u8 {
  // Not authored, on this camera or show-wide. The wall shows the authored
  // camera where this plate would have been, on purpose.
  Off,
  // Authored, but no pose has arrived. Fix the tracker.
  WaitingForPose,
  // Authored and tracking, but the camera sees no part of this canvas. Fix
  // where the camera is pointing, or the wall's calibration.
  OffCanvas,
  // Authored and tracking, but the Director cannot say where the plate is:
  // it does not hold the curved or UV-mesh wall the plate would be on. The
  // nodes may be drawing it; `inner_rect_unknown` says why this cannot.
  Unknown,
  // A plate is on the wall.
  Drawing,
};

// `state` supplies the show-wide master switch, which takes every plate off
// the wall regardless of what a camera asked for.
[[nodiscard]] InnerFrustumReport
inner_frustum_report(const LiveDataState &state,
                     const TrackedCameraObservation &camera) noexcept;

// What the coverage row reports for one plate.
//
// Pure and separate for the same reason `inner_frustum_report` is: the
// difference between a plate that happens to fill the wall and one clamped to
// it is a thing to assert, not a string to read off a screenshot. They are
// opposite situations - the first is a camera framing the wall, the second is
// one whose frustum runs off the end of it - and only one of them is worth
// interrupting an operator about.
struct PlateCoverage final {
  // Fraction of the canvas the plate covers, as a percentage.
  float percent{};
  // The rectangle is the whole-wall fallback rather than a fit.
  bool clamped{};
  // There is a plate at all.
  bool drawing{};

  bool operator==(const PlateCoverage &) const = default;
};

[[nodiscard]] PlateCoverage
plate_coverage(const LiveDataState &state,
               const TrackedCameraObservation &camera) noexcept;

// What one node's row has to say, decided once rather than while drawing.
//
// Same reason as the two above: an operator reading this panel is triaging,
// and the difference between "this node never got the pose" and "this node got
// it and its slice does not touch the plate" decides whether they go and look
// at the network or at the wall calibration.
enum class NodeTrackingReport : core::u8 {
  // No heartbeat carrying tracking has arrived from this node.
  Silent,
  // Reporting, but has never received a pose.
  NoPose,
  // Receiving poses, but has drawn no plate and skipped none: its outputs
  // never planned one.
  NotPlanned,
  // Receiving poses; every plate it planned missed this node's slice. Correct
  // on most of a wide wall.
  NoSlice,
  // Drawing plates.
  Drawing,
  // Drawing, but poses went missing on the way here. The plate is running
  // behind its neighbours'.
  DrawingBehind,
  // Drawing, but this node is dropping poses it received because it cannot
  // drain them fast enough. Listed apart from DrawingBehind because the fix is
  // this machine, not the link to it.
  DrawingSlow,
};

[[nodiscard]] NodeTrackingReport
node_tracking_report(const NodeTrackingObservation &node) noexcept;

// One refusal worth showing. A column of zeroes is noise an operator has to
// read past to find the one number that is not zero, so only the counters that
// actually fired are listed.
struct RefusalRow final {
  std::string_view name;
  core::u64 count{};

  bool operator==(const RefusalRow &) const = default;
};

inline constexpr std::size_t max_refusal_rows = 7U;

// Fills `into` with the nonzero refusal counters and returns how many were
// written, never more than `max_refusal_rows`.
[[nodiscard]] std::size_t
nonzero_refusals(const TrackingRuntimeObservation &tracking,
                 std::span<RefusalRow, max_refusal_rows> into) noexcept;

class LiveDataPanel final {
public:
  explicit LiveDataPanel(gui::UiId id) noexcept : id_(id) {}

  void draw(gui::UiContext &ctx, const gui::Rect &bounds,
            const LiveDataState &state);

  // How tall the body drew last frame, and how far down it is scrolled. A
  // diagnostics panel reporting its own geometry is unobjectionable, and it is
  // what lets the scroll behaviour be asserted without rendering pixels.
  [[nodiscard]] float content_height() const noexcept {
    return scroll_.content_height();
  }
  [[nodiscard]] float scroll_offset() const noexcept {
    return scroll_.offset();
  }

private:
  gui::UiId id_{};
  // Bounded by how tall the body drew last frame rather than by a separate
  // row count, so the two cannot disagree about the size of the thing being
  // scrolled.
  gui::ScrollRegion scroll_;
};

} // namespace mediaserver::editor
