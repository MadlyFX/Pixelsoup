#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/show/demo_scene.hpp>
#include <mediaserver/show/transport_state.hpp>

#include <vector>

// The built-in feed map that goes with the built-in scene, as document nodes.
//
// Placeholder content, like the demo scene: a stand-in for the feed map an
// operator surface will author once it exists. It lives here for the same
// reason the demo scene does - the one constraint it has to honour is a
// show-layer fact, that a node may not take an id the runtime reserves or one
// the demo scene already uses. Getting that wrong is silent and slice-dependent,
// so it is enforced where it cannot be ignored.
namespace mediaserver::show {

// The id the feed map's first node takes; the rest follow consecutively. Chosen
// to clear the transport's reserved id and the whole demo-scene id range.
inline constexpr core::u64 demo_feed_map_node_base = 1'101U;

static_assert(
    demo_feed_map_node_base > transport_node_id.value(),
    "the demo feed map must not reuse the transport's reserved node id");
static_assert(
    demo_feed_map_node_base > demo_scene_node_base + 7U,
    "the demo feed map must not reuse a demo scene node id");

// Two adjacent output maps for the 2560x720 demo canvas: node 0 outputs the
// left 1280 columns, node 1 the right 1280. They abut at column 1280 with no
// overlap - this is an LED wall, so the seam is pixel-exact and there is nothing
// to blend. The scene each node renders is its own `source` rectangle.
[[nodiscard]] std::vector<doc::Node> demo_feed_map();

} // namespace mediaserver::show
