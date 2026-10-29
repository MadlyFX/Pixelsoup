#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/scene/scene.hpp>

#include <optional>
#include <string>
#include <vector>

// Vehicle process platesets placed in a scene: one track and clip per
// plate, wired into an LED Car Process, and kept in step afterwards. Every
// planner here is a pure function of the document, so a placement, a take
// swap or an offset change is one ordinary Scene transaction.
namespace mediaserver::app {

// What a placement does when the scene already has an LED Car Process whose
// sockets the plates need.
enum class PlatesetRigChoice : core::u8 {
  // Use the first rig: free sockets take the plates; occupied ones keep
  // their wires unless every needed socket is free. With no rig, make one.
  Auto,
  // Use the first rig and replace whatever is wired to the needed sockets.
  Replace,
  // Always make a new rig beside the old.
  NewRig,
};

struct PlatesetPlacementRequest final {
  scene::SceneId scene{};
  doc::NodeId plateset{};
  // The show frame every plate starts on.
  core::i64 start{};
  scene::MediaFrameRate show_rate{24U, 1U};
  PlatesetRigChoice rig{PlatesetRigChoice::Auto};
};

enum class PlatesetAuthoringError : core::u8 {
  NotFound,
  NotReady,
  NotATake,
  NothingToSwap,
  InvalidScene,
  AllocationFailed,
};

struct PlatesetPlacementPlan final {
  doc::Root document;
  core::u64 next_node{};
  // In slot order, one per placed plate.
  std::vector<doc::NodeId> lanes;
  std::vector<doc::NodeId> clips;
  // The LED Car Process the plates were wired to.
  doc::NodeId rig{};
  bool created_rig{};
  // Sockets that already held a wire and kept it (Auto with a busy rig).
  std::vector<assets::PlatesetSlot> skipped;
};

// The show frames a source span lasts at `show_rate`, rounded up so the last
// source frame is never cut short.
[[nodiscard]] core::i64 plateset_show_frames(core::i64 source_frames,
                                             assets::MediaFrameRate source,
                                             scene::MediaFrameRate show_rate);

// Whether placing in `scene` would replace wires on an existing rig: the
// question the editor asks the operator before an Auto placement.
[[nodiscard]] bool plateset_rig_busy(const doc::Root &document,
                                     scene::SceneId scene,
                                     doc::NodeId plateset);

[[nodiscard]] core::Result<PlatesetPlacementPlan, PlatesetAuthoringError>
plan_plateset_placement(const doc::Root &document,
                        const PlatesetPlacementRequest &request,
                        core::u64 next_node);

// The clips edited with `clip`: the same take, start and length, in scene
// order. Includes `clip` itself; empty when it is not a plate.
[[nodiscard]] std::vector<doc::NodeId>
plateset_take_clips(const doc::Root &document, doc::NodeId clip);

struct PlatesetSwapPlan final {
  doc::Root document;
  // Slots the new set has no plate for: those clips were left as they were.
  std::vector<assets::PlatesetSlot> unmatched;
};

// Plays `plateset` in place of the take `clip` belongs to: each plate takes
// the new set's media for its slot, keeping its trims, effects, track and
// wiring, and the take position its offset implies.
[[nodiscard]] core::Result<PlatesetSwapPlan, PlatesetAuthoringError>
plan_plateset_swap(const doc::Root &document, doc::NodeId clip,
                   doc::NodeId plateset);

// After a plateset's offsets change from `before` to `after`, every placed
// plate of that set moves its source in by the difference, so placements
// stay in sync with the set, and a take now running past the end of one of
// its plates is shortened to what they all still cover. Returns `after`
// with those clips updated.
[[nodiscard]] doc::Root follow_plateset_offsets(const doc::Root &before,
                                                doc::Root after,
                                                doc::NodeId plateset,
                                                scene::MediaFrameRate show_rate);

// Every placed plate of `plateset` takes the set's decode size, full or
// half, as `document` now says it is.
[[nodiscard]] doc::Root follow_plateset_resolution(doc::Root document,
                                                   doc::NodeId plateset);

[[nodiscard]] std::string
plateset_authoring_error_text(PlatesetAuthoringError error);

} // namespace mediaserver::app
