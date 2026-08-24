#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/render/media_source.hpp>
#include <mediaserver/rhi/device.hpp>

#include <cstddef>
#include <deque>
#include <map>
#include <memory>
#include <string>

// Resolves a clip's document reference to a GPU texture, and keeps recent ones.
//
// The decode itself lives behind a MediaSource (media_source.hpp); this type is
// only the GPU residency around it: on a miss it asks the source to decode, then
// uploads and caches by the source's key, evicting oldest-first. Swapping the
// source - image sequence today, a real codec later - changes what is decoded
// without touching anything above this seam.
namespace mediaserver::render {

class MediaCache final {
public:
    // Defaults to an image-sequence source; a caller can inject another (a HAP
    // or FFmpeg source, or a stub in a test).
    MediaCache();
    explicit MediaCache(std::unique_ptr<MediaSource> source);

    // Resolves (source, frame) to a texture, decoding via the MediaSource and
    // uploading on `commands` on a miss. Returns null when the source cannot
    // produce the frame, which the caller renders as nothing rather than a
    // failed frame.
    [[nodiscard]] const rhi::Texture* resolve(
        rhi::Device& device,
        rhi::CommandList& commands,
        const std::string& source_template,
        core::u32 frame_index);

private:
    // Recent frames past this are evicted oldest-first. Generously above the
    // most clips one frame can reference, so a frame's own uploads are never
    // evicted out from under it mid-frame.
    static constexpr std::size_t max_resident = 128U;

    std::unique_ptr<MediaSource> source_;
    std::map<std::string, rhi::Texture> textures_;
    std::deque<std::string> order_;
};

} // namespace mediaserver::render
