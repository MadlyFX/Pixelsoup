#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/show/transport_state.hpp>

#include <vector>

// The built-in 3D scene, expressed as document nodes.
//
// This is placeholder content - a stand-in for a project the operator surface
// will load once it exists - not engine mechanism. It lives in `show` rather
// than in `scene` because the one constraint it has to honour is a show-layer
// fact: a scene node may not take an id the runtime reserves for itself, and
// the only such id today is show::transport_node_id, which the Director writes
// on every play, pause and seek. A collision is silent and slice-dependent -
// the transport node overwrites the stage, the canvas falls back to its
// 1920x1080 default, and only the slices that no longer fit that default fail
// to render - so the constraint is worth enforcing where it cannot be ignored.
namespace mediaserver::show {

// The id the scene's first node takes; the rest follow consecutively. Chosen to
// clear every reserved id with room to spare.
inline constexpr core::u64 demo_scene_node_base = 1'001U;

// Enforced here, next to the reserved id, so the scene and the transport can
// never be authored into a collision without the build failing first.
static_assert(
    demo_scene_node_base > transport_node_id.value(),
    "the demo scene must not reuse the transport's reserved node id");

// A small but complete scene: a 2560x720 canvas split into two 1280x720 halves,
// a camera that orbits the origin, a ground plane, a spinning box, a bobbing
// sphere, and a key light, plus a two-node feed map binding node 0 to the left
// half and node 1 to the right. Every animated quantity is a rate, so a playing
// show authors no further edits and two nodes derive an identical scene from the
// same show frame.
[[nodiscard]] std::vector<doc::Node> demo_scene();

} // namespace mediaserver::show
