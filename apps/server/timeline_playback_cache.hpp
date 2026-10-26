#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/scene_model.hpp>

#include <span>
#include <vector>

// The Timeline's cache bar: which show frames this machine could play from
// video it has already decoded. The renderer keeps decoded source frames, not
// finished pictures, so a show frame is buffered when every video on air at it
// has its own source frame ready. Pure: the caller snapshots decode residency
// and says which file and numbering each placement decodes from.
namespace mediaserver::app {

struct ShowFrameRun final {
    core::u64 first{};
    core::u64 last{};

    bool operator==(const ShowFrameRun &) const = default;
};

// One video placement. `ready` is the decoded source frames of the file it
// actually plays, ascending. `decode_offset` is added to the document's source
// frame to get the decoded file's numbering: zero unless a proxy numbers its
// frames differently.
struct BufferedClip final {
    const scene::ClipPlacement *clip{};
    std::span<const core::u32> ready;
    core::i64 decode_offset{};
};

// Inclusive runs of buffered show frames within [first, last], ascending and
// disjoint. A frame with none of `clips` on air needs nothing decoded and is
// left out, so a gap between clips never reads as buffered.
[[nodiscard]] std::vector<ShowFrameRun>
buffered_show_frames(std::span<const BufferedClip> clips, scene::SceneTime rate,
                     core::u64 first, core::u64 last);

} // namespace mediaserver::app
