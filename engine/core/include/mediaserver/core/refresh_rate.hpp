#pragma once

#include <mediaserver/core/types.hpp>

namespace mediaserver::core {

// A refresh rate in thousandths of a hertz, rounded: 60000/1001 is 59940.
// A zero denominator is no rate at all.
[[nodiscard]] constexpr u32 refresh_millihertz(u32 numerator,
                                               u32 denominator) noexcept {
    if (denominator == 0U) {
        return 0U;
    }
    const u64 scaled = static_cast<u64>(numerator) * 1000U;
    return static_cast<u32>((scaled + denominator / 2U) / denominator);
}

// Rates within this of each other are the same rate. 23.976 is also written
// 23.98, a typed rate is stored as 24000/1001, and a display driver reports
// the same signal as its own pixel-clock fraction; all of these are one rate.
// Neighbouring standard rates (23.976 and 24, 59.94 and 60) are at least
// 23 mHz apart, so they stay distinct.
inline constexpr u32 refresh_tolerance_millihertz = 10U;

// Whether two exact rate fractions name the same display rate. A missing rate
// (zero numerator or denominator) matches nothing.
[[nodiscard]] constexpr bool same_refresh_rate(
    u32 left_numerator, u32 left_denominator, u32 right_numerator,
    u32 right_denominator) noexcept {
    if (left_numerator == 0U || left_denominator == 0U ||
        right_numerator == 0U || right_denominator == 0U) {
        return false;
    }
    const u32 left = refresh_millihertz(left_numerator, left_denominator);
    const u32 right = refresh_millihertz(right_numerator, right_denominator);
    return (left > right ? left - right : right - left) <=
           refresh_tolerance_millihertz;
}

} // namespace mediaserver::core
