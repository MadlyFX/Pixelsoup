#pragma once

#include <string_view>

// The document's feed-map vocabulary, in one place.
//
// A feed map is the OUTPUT stage of §1.2 of the architecture plan: it decides
// which rectangle of the shared canvas a node puts on its connector.
//
// This product drives LED walls, not projectors, so there is no soft-edge
// blend between nodes. LED panels abut with no overlap - adjacent nodes drive
// physically distinct, touching panels - so a fade would only darken each
// node's boundary with nothing to sum against, opening a dark band at every
// seam. The seam is instead pixel-exact by construction: the scene layer
// renders a bit-identical shared frustum plane, so two nodes' regions tile the
// canvas without gap or overlap. What remains for the output stage, and lands
// downstream of this, is the per-panel/per-wall calibration LUT and dither.
//
// Like the scene, it is document nodes and nothing else - the same immutable,
// versioned, command-edited nodes - so a feed-map edit replicates, undoes, and
// journals through the ordinary paths, and every node derives an identical map
// from the same committed revision.
//
// The names are strings for the same reason the scene's are: the document's
// property map is string-to-scalar, so every field is a lookup and a type
// check, paid once per edit rather than once per frame.
namespace mediaserver::output::schema {

// ---------------------------------------------------------------------------
// Node types

// One node's binding: the canvas rectangle it outputs. Selected by
// `node_index`; a node never invents one.
inline constexpr std::string_view map_type = "output.map";

// ---------------------------------------------------------------------------
// output.map

inline constexpr std::string_view node_index = "node_index";
// The rectangle of the shared canvas this node outputs. Its origin anchors the
// node on the canvas; adjacent nodes' rectangles abut edge to edge.
inline constexpr std::string_view source_x = "source_x";
inline constexpr std::string_view source_y = "source_y";
inline constexpr std::string_view source_width = "source_width";
inline constexpr std::string_view source_height = "source_height";

} // namespace mediaserver::output::schema
