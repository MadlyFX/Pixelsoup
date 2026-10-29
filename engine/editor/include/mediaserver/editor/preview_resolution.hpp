#pragma once

#include <mediaserver/core/types.hpp>

#include <algorithm>

namespace mediaserver::editor {

// Machine-local editor previews only. Never apply this to an authored canvas,
// output map, or render-node surface.
enum class PreviewResolution : core::u8 { Full, Half, Quarter };

[[nodiscard]] constexpr bool valid(PreviewResolution resolution) noexcept {
    return resolution == PreviewResolution::Full ||
           resolution == PreviewResolution::Half ||
           resolution == PreviewResolution::Quarter;
}

// Which file an editor view decodes for its video. Proxy draws a UHD clip's
// 720p working proxy once one is ready, and the source until then; Full
// always draws the source. Like the preview resolution it is the editor's
// own: render nodes always decode what the project asks for.
enum class MediaRendition : core::u8 { Proxy, Full };

[[nodiscard]] constexpr bool valid(MediaRendition rendition) noexcept {
    return rendition == MediaRendition::Proxy ||
           rendition == MediaRendition::Full;
}

// What a view's clips drew on the last frame, as the Director resolved them:
// how many drew a proxy, and how many show their source only until their
// proxy is ready.
struct ViewMediaCounts final {
    core::u32 proxied{};
    core::u32 waiting{};

    bool operator==(const ViewMediaCounts&) const = default;
};

struct PreviewExtent final {
    core::u32 width{};
    core::u32 height{};
    bool operator==(const PreviewExtent&) const = default;
};

// Scale each axis; Half shades one quarter as many pixels. The caller keeps
// its logical canvas for projection, picking, masks, and display placement.
[[nodiscard]] constexpr PreviewExtent preview_extent(
    core::u32 width, core::u32 height, PreviewResolution resolution) noexcept {
    const core::u32 divisor = resolution == PreviewResolution::Quarter ? 4U
                           : resolution == PreviewResolution::Half ? 2U : 1U;
    return {std::max(1U, width / divisor), std::max(1U, height / divisor)};
}

} // namespace mediaserver::editor
