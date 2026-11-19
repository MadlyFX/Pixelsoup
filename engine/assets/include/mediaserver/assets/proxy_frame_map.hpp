#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

#include <optional>
#include <vector>

namespace mediaserver::assets {

// A normalized, non-negative rational timestamp measured from the source
// asset's presentation origin. No timebase conversion in this layer uses a
// floating-point value.
struct PresentationTime final {
    core::u64 numerator{};
    core::u64 denominator{1U};

    bool operator==(const PresentationTime&) const = default;
};

enum class ProxyFrameMapErrorCode : core::u8 {
    InvalidDenominator,
    InvalidFrameRate,
    InvalidMap,
    Overflow,
    AllocationFailed,
};

struct ProxyFrameMapError final {
    ProxyFrameMapErrorCode code{ProxyFrameMapErrorCode::InvalidMap};

    bool operator==(const ProxyFrameMapError&) const = default;
};

using PresentationTimeResult =
    core::Result<PresentationTime, ProxyFrameMapError>;

[[nodiscard]] PresentationTimeResult
make_presentation_time(core::u64 numerator, core::u64 denominator) noexcept;
[[nodiscard]] bool valid_presentation_time(
    const PresentationTime& value) noexcept;
// Returns -1, 0, or 1. Both operands must be valid normalized values.
[[nodiscard]] int compare_presentation_time(
    const PresentationTime& left, const PresentationTime& right) noexcept;

// Exact ordinal * rate.denominator / rate.numerator with checked reduction
// before multiplication.
[[nodiscard]] PresentationTimeResult presentation_time_for_frame(
    core::u64 ordinal, MediaFrameRate frame_rate) noexcept;

struct ProxyFrameMap final {
    // This is the fixed CFR rate emitted by the derivative profile.
    MediaFrameRate proxy_frame_rate;
    // One exact source presentation time for every emitted proxy frame. Equal
    // adjacent entries explicitly encode duplicated CFR frames.
    std::vector<PresentationTime> source_time_by_proxy_frame;

    bool operator==(const ProxyFrameMap&) const = default;
};

[[nodiscard]] bool valid_proxy_frame_map(const ProxyFrameMap& map) noexcept;

using ProxyFrameMapResult = core::Result<ProxyFrameMap, ProxyFrameMapError>;
// Proxy frame j is source frame first_source_frame + j: a whole-file proxy
// starts at zero, a segment at its first frame.
[[nodiscard]] ProxyFrameMapResult make_identity_proxy_frame_map(
    core::u64 frame_count, MediaFrameRate frame_rate,
    core::u64 first_source_frame = 0U) noexcept;

using ProxyFrameLookupResult =
    core::Result<std::optional<core::u64>, ProxyFrameMapError>;
// At/before selects the final duplicate; at/after selects the first duplicate.
[[nodiscard]] ProxyFrameLookupResult proxy_frame_at_or_before(
    const ProxyFrameMap& map, const PresentationTime& source_time) noexcept;
[[nodiscard]] ProxyFrameLookupResult proxy_frame_at_or_after(
    const ProxyFrameMap& map, const PresentationTime& source_time) noexcept;
// The same at/before lookup for a map the caller already proved with
// valid_proxy_frame_map: O(log frames) instead of a full re-validation, for
// lookups made on every frame. Only source_time is checked here.
[[nodiscard]] ProxyFrameLookupResult proxy_frame_at_or_before_in_valid_map(
    const ProxyFrameMap& map, const PresentationTime& source_time) noexcept;

} // namespace mediaserver::assets
