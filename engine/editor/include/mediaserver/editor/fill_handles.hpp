#pragma once

#include <mediaserver/editor/transform_box.hpp>
#include <mediaserver/scene/scene.hpp>

// A gradient's handles on the surface it fills, as the Program view's
// Gradient tool draws and drags them.
//
// The renderer places a fill in the surface's real proportions: a centre, and
// for a linear fill a ramp along a direction whose length is a share of the
// surface's extent that way, for a radial one a radius that is a share of
// half its diagonal. Everything here is that same arithmetic turned round,
// in fractions across and down the surface, so a handle sits exactly where
// the picture changes and a drag of one writes the value that puts it there.
namespace mediaserver::editor {

struct FillHandles final {
    // False for a single colour, which has nowhere to put a handle.
    bool visible{};
    // Where the fill is centred: the middle of a linear ramp, the centre of
    // a radial one.
    BoxPoint centre{};
    // Linear: where the ramp starts (colour A) and ends (colour B). Radial:
    // `end` is a point on its rim, level with the centre, and `start` the
    // centre itself.
    BoxPoint start{};
    BoxPoint end{};
};

// `width` and `height` are the surface's real size, in any one unit.
[[nodiscard]] FillHandles fill_handles(const scene::Fill& fill, float width,
                                       float height) noexcept;

// The fill with its centre moved to `centre`.
[[nodiscard]] scene::Fill fill_with_centre(scene::Fill fill,
                                           BoxPoint centre) noexcept;

// The fill with its far handle dragged to `end`, its centre staying put: a
// linear ramp turns to point at it and takes twice that reach as its length;
// a radial one takes the reach as its radius.
[[nodiscard]] scene::Fill fill_with_end(scene::Fill fill, float width,
                                        float height, BoxPoint end) noexcept;

// The fill drawn afresh as a line from `from` to `to`: a linear ramp runs
// the length of the line, colour A at its start; a radial one is centred on
// its start and reaches its end.
[[nodiscard]] scene::Fill fill_along(scene::Fill fill, float width,
                                     float height, BoxPoint from,
                                     BoxPoint to) noexcept;

}  // namespace mediaserver::editor
