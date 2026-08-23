#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/canvas.hpp>

#include <vector>

// The evaluated feed map: what the output stage draws with, and the last place
// the answer is still a rectangle rather than a pixel.
//
// Like `scene::Scene`, a FeedMap is a value with no graph in it, and it exists
// to make one comparison possible: two nodes hold equal maps, or their regions
// of the wall will not line up. That comparison is `digest()`, and it is over
// the authored parameters - rectangles replicated from the document - not over
// any rendering of them, so it agrees across nodes exactly as the scene digest
// does and for the same reason.
namespace mediaserver::output {

// One node's binding: the canvas rectangle it outputs.
//
// This lives in the document because it has to be replicated - a node that
// disagreed with the Director about its own rectangle would render a correct
// image of the wrong region. A node selects its map by its own index and never
// invents one. Adjacent nodes' rectangles abut edge to edge with no overlap:
// this drives LED walls, where the seam is pixel-exact and there is nothing to
// blend.
struct OutputMap final {
    core::u32 node_index{};
    // The region of the shared canvas this node outputs. The origin anchors it
    // on the canvas.
    scene::CanvasRect source{};

    bool operator==(const OutputMap&) const = default;
};

// A cap, so a runaway document is refused rather than absorbed. Generous
// against any real wall and far below what would matter.
inline constexpr core::u32 max_output_maps = 256U;

struct FeedMap final {
    // Sorted by node index. One binding per node; the lowest node id wins a
    // duplicate, decided in the model so every node resolves it the same way.
    std::vector<OutputMap> maps;

    // The map bound to this node, or null when the document binds none. A node
    // with no map outputs nothing rather than guessing at the whole canvas:
    // an unmapped output should be black and obviously unmapped, exactly as an
    // unmapped scene slice is.
    [[nodiscard]] const OutputMap* map_for(core::u32 node_index) const noexcept;
};

// FNV-1a over every map in node-index order. Hashes the authored parameters,
// not a rendering, so two nodes that agree on the document agree on this.
// Counts are hashed before contents so a map that is a prefix of another cannot
// collide with it.
[[nodiscard]] core::u64 digest(const FeedMap& feed_map) noexcept;

} // namespace mediaserver::output
