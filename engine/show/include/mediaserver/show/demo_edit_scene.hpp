#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/node.hpp>
#include <mediaserver/show/transport_state.hpp>

#include <string_view>
#include <vector>

// A small scene for the editor demo: a static camera, a floor, a light, and one
// clip on a screen-facing quad the operator can drag.
//
// Deliberately simpler than the demo scene the wall uses: the camera does not
// orbit, so a drag reads cleanly against a still frame. Like the others this is
// placeholder content - a stand-in for a project the operator surface will load
// - and it honours the one show-layer constraint: its node ids clear the
// transport's reserved id.
namespace mediaserver::show {

// The id the edit scene's first node takes; the rest follow consecutively.
inline constexpr core::u64 demo_edit_node_base = 1'201U;

static_assert(
    demo_edit_node_base > transport_node_id.value(),
    "the demo edit scene must not reuse the transport's reserved node id");

// How many frames the demo clip sequence holds. Shared so the file writer and
// the scene node agree.
inline constexpr core::u32 demo_clip_frames = 48U;

// The fresh editor scene and its bootstrap output map must describe the same
// canvas. Keep the dimensions beside the scene vocabulary so the Director does
// not silently seed a cropped output when either side changes.
inline constexpr core::u32 demo_edit_canvas_width = 1'920U;
inline constexpr core::u32 demo_edit_canvas_height = 1'080U;

// The edit scene, with `clip_source` as the media node's sequence template
// (e.g. a "#"-run path the renderer resolves per frame). The clip sits at the
// origin facing the camera; everything else is context to place it against.
[[nodiscard]] std::vector<doc::Node> demo_edit_scene(std::string_view clip_source);

} // namespace mediaserver::show
