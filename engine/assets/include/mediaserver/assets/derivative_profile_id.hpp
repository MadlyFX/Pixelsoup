#pragma once

#include <mediaserver/core/types.hpp>

namespace mediaserver::assets {

// Profile identifiers are persistent derivative vocabulary. Existing values
// must never be repurposed; changed output semantics require a profile-version
// or generator-version bump.
enum class DerivativeProfileId : core::u16 {
    Thumbnail320x180Srgb = 1U,
    Filmstrip8x160x90Srgb = 2U,
    Waveform2048PeakRms = 3U,
    Proxy720pCfrH264 = 4U,
};

} // namespace mediaserver::assets
