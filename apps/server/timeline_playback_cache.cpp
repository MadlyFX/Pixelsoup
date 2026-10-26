#include "timeline_playback_cache.hpp"

#include <algorithm>
#include <limits>

namespace mediaserver::app {

std::vector<ShowFrameRun>
buffered_show_frames(std::span<const BufferedClip> clips, scene::SceneTime rate,
                     core::u64 first, core::u64 last) {
    std::vector<ShowFrameRun> runs;
    if (last < first) {
        return runs;
    }
    constexpr auto signed_ceiling =
        static_cast<core::u64>(std::numeric_limits<core::i64>::max());
    // What each frame needs: nothing, everything ready, or something missing.
    enum class Need : core::u8 { None, Ready, Missing };
    std::vector<Need> needs(static_cast<std::size_t>(last - first) + 1U,
                            Need::None);
    for (const BufferedClip &entry : clips) {
        if (entry.clip == nullptr) {
            continue;
        }
        const scene::ClipPlacement &clip = *entry.clip;
        for (core::u64 frame = first;; ++frame) {
            const auto signed_frame =
                static_cast<core::i64>(std::min(frame, signed_ceiling));
            if (scene::clip_visible(clip.timing, signed_frame)) {
                // Nothing decoded needs no source frame to know it is waiting.
                const core::i64 decoded =
                    entry.ready.empty()
                        ? -1
                        : static_cast<core::i64>(
                              scene::clip_source_frame(clip, rate, frame)) +
                              entry.decode_offset;
                const bool ready =
                    decoded >= 0 &&
                    decoded <= std::numeric_limits<core::u32>::max() &&
                    std::binary_search(entry.ready.begin(), entry.ready.end(),
                                       static_cast<core::u32>(decoded));
                Need &need = needs[static_cast<std::size_t>(frame - first)];
                if (!ready) {
                    need = Need::Missing;
                } else if (need == Need::None) {
                    need = Need::Ready;
                }
            }
            if (frame == last) {
                break;
            }
        }
    }
    for (std::size_t index = 0U; index < needs.size(); ++index) {
        if (needs[index] != Need::Ready) {
            continue;
        }
        const core::u64 frame = first + index;
        if (!runs.empty() && runs.back().last + 1U == frame) {
            runs.back().last = frame;
        } else {
            runs.push_back(ShowFrameRun{frame, frame});
        }
    }
    return runs;
}

} // namespace mediaserver::app
