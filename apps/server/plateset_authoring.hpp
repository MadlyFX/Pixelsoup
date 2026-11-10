#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>
#include <string>
#include <vector>

// Vehicle process platesets placed in a scene. A placement is one take: a
// single clip on a vehicle track, which plays each of the set's plates on
// that track's slot screen for it. The track and its screens are the car -
// where each wall stands, how it is graded, how it is wired into an LED Car
// Process - and stay as they are while takes come and go on it. Every planner
// here is a pure function of the document, so a placement, a take swap or a
// change to the set is one ordinary Scene transaction.
namespace mediaserver::app {

// What a placement that has to make a vehicle track does when the scene
// already has an LED Car Process whose sockets the plates need.
enum class PlatesetRigChoice : core::u8 {
  // Put the take on the scene's vehicle track when it has one. Otherwise
  // make a track and use the first rig: free sockets take its screens;
  // occupied ones keep their wires. With no rig, make one.
  Auto,
  // Make a track, use the first rig and replace whatever is wired to the
  // sockets its screens need.
  Replace,
  // Always make a new vehicle track and a new rig beside the old.
  NewRig,
};

struct PlatesetPlacementRequest final {
  scene::SceneId scene{};
  doc::NodeId plateset{};
  // The show frame the take starts on.
  core::i64 start{};
  scene::MediaFrameRate show_rate{24U, 1U};
  PlatesetRigChoice rig{PlatesetRigChoice::Auto};
  // The vehicle track to put the take on, when the operator chose one. Zero
  // leaves it to `rig`.
  doc::NodeId track{};
};

enum class PlatesetAuthoringError : core::u8 {
  NotFound,
  NotReady,
  NotATake,
  NotAVehicleTrack,
  NothingToSwap,
  InvalidScene,
  AllocationFailed,
};

struct PlatesetPlacementPlan final {
  doc::Root document;
  core::u64 next_node{};
  // The vehicle track the take is on, and whether this placement made it.
  doc::NodeId track{};
  bool created_track{};
  // The take.
  doc::NodeId clip{};
  // Slot screens this placement made, in slot order: every plate of the set
  // whose slot the track had no screen for.
  std::vector<doc::NodeId> screens;
  // The LED Car Process those screens were wired to; zero when it made none.
  doc::NodeId rig{};
  bool created_rig{};
  // New screens whose socket already held a wire and kept it.
  std::vector<assets::PlatesetSlot> skipped;
};

// The show frames a source span lasts at `show_rate`, rounded up so the last
// source frame is never cut short.
[[nodiscard]] core::i64 plateset_show_frames(core::i64 source_frames,
                                             assets::MediaFrameRate source,
                                             scene::MediaFrameRate show_rate);

// Whether placing in `scene` would have to choose what to do with wires on
// an existing rig: the question the editor asks the operator before an Auto
// placement. Never once the scene has a vehicle track - the take goes on it.
[[nodiscard]] bool plateset_rig_busy(const doc::Root &document,
                                     scene::SceneId scene,
                                     doc::NodeId plateset);

[[nodiscard]] core::Result<PlatesetPlacementPlan, PlatesetAuthoringError>
plan_plateset_placement(const doc::Root &document,
                        const PlatesetPlacementRequest &request,
                        core::u64 next_node);

struct PlatesetSwapPlan final {
  doc::Root document;
  core::u64 next_node{};
  // Slots the take was playing that the new set has no plate for: those
  // screens stay, and show nothing while this take is up.
  std::vector<assets::PlatesetSlot> unmatched;
  // Slot screens made for plates the track had no screen for.
  std::vector<doc::NodeId> screens;
};

// Plays `plateset` in place of the take `clip`: the one clip takes the new
// set's plates and keeps its place on the Timeline, its trim as a position in
// the take, its fades and its effects. It is shortened if the new take runs
// out sooner. The track and its screens - where each wall stands and how it
// is graded - are not touched, which is the point.
[[nodiscard]] core::Result<PlatesetSwapPlan, PlatesetAuthoringError>
plan_plateset_swap(const doc::Root &document, doc::NodeId clip,
                   doc::NodeId plateset, scene::MediaFrameRate show_rate,
                   core::u64 next_node);

// After `plateset` changes in the Library - a plate moved to another slot, a
// sync offset, its decode size - every take of it in `document` plays the
// set as it now is, at the same position in the take, and one now running
// past the end of a plate is shortened to what they all still cover. A set
// that can no longer be placed leaves its takes as they were.
[[nodiscard]] doc::Root follow_plateset(doc::Root document,
                                        doc::NodeId plateset,
                                        scene::MediaFrameRate show_rate);

[[nodiscard]] std::string
plateset_authoring_error_text(PlatesetAuthoringError error);

} // namespace mediaserver::app
