#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/rhi/device.hpp>

#include <cstddef>
#include <deque>
#include <map>
#include <string>

// Resolves a clip's document reference to a GPU texture.
//
// A clip references an image sequence as a path template with a run of '#'
// marking the zero-padded frame number (plate_####.png), plus the frame to
// show. This turns that into pixels resident on the GPU, loading and uploading
// on a miss and keeping recent frames.
//
// It is deliberately spike-grade: a CPU decode of a still image per frame. It
// stands in for the real media pipeline - GPU decode of a compressed stream,
// prefetch, frame-exact seek - which replaces it behind this same call. Because
// it is behind a call, nothing above the renderer learns that a video is not
// yet a real codec.
namespace mediaserver::render {

class MediaCache final {
public:
    // Resolves (template, frame) to a texture, loading via stb_image and
    // uploading on `commands` on a miss. Returns null when the file cannot be
    // read or decoded, which the caller renders as nothing rather than a failed
    // frame.
    [[nodiscard]] const rhi::Texture* resolve(
        rhi::Device& device,
        rhi::CommandList& commands,
        const std::string& source_template,
        core::u32 frame_index);

    // Substitutes the zero-padded frame number into a '#'-run template. A
    // template with no '#' is a still image and is returned unchanged. Public
    // and pure - and deliberately not a printf format, so a document-supplied
    // string cannot be a format-string attack - so it is testable with no GPU
    // and no file on disk.
    [[nodiscard]] static std::string resolve_path(
        const std::string& source_template,
        core::u32 frame_index);

private:
    // Recent frames past this are evicted oldest-first. Generously above the
    // most clips one frame can reference, so a frame's own uploads are never
    // evicted out from under it mid-frame.
    static constexpr std::size_t max_resident = 128U;

    std::map<std::string, rhi::Texture> textures_;
    std::deque<std::string> order_;
};

} // namespace mediaserver::render
