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
