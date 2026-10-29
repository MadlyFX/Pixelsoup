#include <mediaserver/assets/proxy_frame_map.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <numeric>

namespace mediaserver::assets {
namespace {

[[nodiscard]] ProxyFrameMapError error(
    ProxyFrameMapErrorCode code) noexcept {
    return ProxyFrameMapError{code};
}

} // namespace

PresentationTimeResult make_presentation_time(
    core::u64 numerator, core::u64 denominator) noexcept {
    if (denominator == 0U) {
        return PresentationTimeResult::failure(
            error(ProxyFrameMapErrorCode::InvalidDenominator));
    }
    if (numerator == 0U) {
        return PresentationTimeResult::success(PresentationTime{0U, 1U});
    }
    const core::u64 divisor = std::gcd(numerator, denominator);
    return PresentationTimeResult::success(
        PresentationTime{numerator / divisor, denominator / divisor});
}

bool valid_presentation_time(const PresentationTime& value) noexcept {
    return value.denominator != 0U &&
        ((value.numerator == 0U && value.denominator == 1U) ||
         (value.numerator != 0U &&
          std::gcd(value.numerator, value.denominator) == 1U));
}

int compare_presentation_time(
    const PresentationTime& left, const PresentationTime& right) noexcept {
    core::u64 left_numerator = left.numerator;
    core::u64 left_denominator = left.denominator;
    core::u64 right_numerator = right.numerator;
    core::u64 right_denominator = right.denominator;
    bool inverted = false;

    // Continued-fraction comparison avoids an overflowing cross product.
    while (true) {
        const core::u64 left_integer = left_numerator / left_denominator;
        const core::u64 right_integer = right_numerator / right_denominator;
        if (left_integer != right_integer) {
            const int result = left_integer < right_integer ? -1 : 1;
            return inverted ? -result : result;
        }

        const core::u64 left_remainder =
            left_numerator % left_denominator;
        const core::u64 right_remainder =
            right_numerator % right_denominator;
        if (left_remainder == 0U || right_remainder == 0U) {
            if (left_remainder == right_remainder) {
                return 0;
            }
            const int result = left_remainder == 0U ? -1 : 1;
            return inverted ? -result : result;
        }

        left_numerator = left_denominator;
        left_denominator = left_remainder;
        right_numerator = right_denominator;
        right_denominator = right_remainder;
        inverted = !inverted;
    }
}

PresentationTimeResult presentation_time_for_frame(
    core::u64 ordinal, MediaFrameRate frame_rate) noexcept {
    if (frame_rate.numerator == 0U || frame_rate.denominator == 0U) {
        return PresentationTimeResult::failure(
            error(ProxyFrameMapErrorCode::InvalidFrameRate));
    }
    const core::u64 divisor = std::gcd(
        ordinal, static_cast<core::u64>(frame_rate.numerator));
    const core::u64 reduced_ordinal = ordinal / divisor;
    const core::u64 reduced_rate_numerator =
        static_cast<core::u64>(frame_rate.numerator) / divisor;
    if (reduced_ordinal > std::numeric_limits<core::u64>::max() /
                              frame_rate.denominator) {
        return PresentationTimeResult::failure(
            error(ProxyFrameMapErrorCode::Overflow));
    }
    return make_presentation_time(
        reduced_ordinal * frame_rate.denominator, reduced_rate_numerator);
}

bool valid_proxy_frame_map(const ProxyFrameMap& map) noexcept {
    if (map.proxy_frame_rate.numerator == 0U ||
        map.proxy_frame_rate.denominator == 0U ||
        std::gcd(map.proxy_frame_rate.numerator,
                 map.proxy_frame_rate.denominator) != 1U) {
        return false;
    }
    for (std::size_t index = 0U;
         index < map.source_time_by_proxy_frame.size(); ++index) {
        if (!valid_presentation_time(
                map.source_time_by_proxy_frame[index])) {
            return false;
        }
        if (index != 0U && compare_presentation_time(
                map.source_time_by_proxy_frame[index - 1U],
                map.source_time_by_proxy_frame[index]) > 0) {
            return false;
        }
    }
    return true;
}

ProxyFrameMapResult make_identity_proxy_frame_map(
    core::u64 frame_count, MediaFrameRate frame_rate,
    core::u64 first_source_frame) noexcept {
    try {
        if (frame_rate.numerator == 0U || frame_rate.denominator == 0U) {
            return ProxyFrameMapResult::failure(
                error(ProxyFrameMapErrorCode::InvalidFrameRate));
        }
        const core::u32 rate_divisor =
            std::gcd(frame_rate.numerator, frame_rate.denominator);
        frame_rate.numerator /= rate_divisor;
        frame_rate.denominator /= rate_divisor;

        ProxyFrameMap map{frame_rate, {}};
        if (frame_count > map.source_time_by_proxy_frame.max_size()) {
            return ProxyFrameMapResult::failure(
                error(ProxyFrameMapErrorCode::AllocationFailed));
        }
        map.source_time_by_proxy_frame.reserve(
            static_cast<std::size_t>(frame_count));
        if (first_source_frame >
            std::numeric_limits<core::u64>::max() - frame_count) {
            return ProxyFrameMapResult::failure(
                error(ProxyFrameMapErrorCode::InvalidMap));
        }
        for (core::u64 ordinal = 0U; ordinal < frame_count; ++ordinal) {
            auto time = presentation_time_for_frame(
                first_source_frame + ordinal, frame_rate);
            if (!time) {
                return ProxyFrameMapResult::failure(time.error());
            }
            map.source_time_by_proxy_frame.push_back(
                std::move(time).value());
        }
        return ProxyFrameMapResult::success(std::move(map));
    } catch (const std::bad_alloc&) {
        return ProxyFrameMapResult::failure(
            error(ProxyFrameMapErrorCode::AllocationFailed));
    } catch (...) {
        return ProxyFrameMapResult::failure(
            error(ProxyFrameMapErrorCode::AllocationFailed));
    }
}

ProxyFrameLookupResult proxy_frame_at_or_before(
    const ProxyFrameMap& map, const PresentationTime& source_time) noexcept {
    if (!valid_proxy_frame_map(map) ||
        !valid_presentation_time(source_time)) {
        return ProxyFrameLookupResult::failure(
            error(ProxyFrameMapErrorCode::InvalidMap));
    }
    std::size_t first = 0U;
    std::size_t count = map.source_time_by_proxy_frame.size();
    while (count != 0U) {
        const std::size_t step = count / 2U;
        const std::size_t candidate = first + step;
        if (compare_presentation_time(
                map.source_time_by_proxy_frame[candidate], source_time) <= 0) {
            first = candidate + 1U;
            count -= step + 1U;
        } else {
            count = step;
        }
    }
    if (first == 0U) {
        return ProxyFrameLookupResult::success(std::nullopt);
    }
    return ProxyFrameLookupResult::success(
        static_cast<core::u64>(first - 1U));
}

ProxyFrameLookupResult proxy_frame_at_or_after(
    const ProxyFrameMap& map, const PresentationTime& source_time) noexcept {
    if (!valid_proxy_frame_map(map) ||
        !valid_presentation_time(source_time)) {
        return ProxyFrameLookupResult::failure(
            error(ProxyFrameMapErrorCode::InvalidMap));
    }
    std::size_t first = 0U;
    std::size_t count = map.source_time_by_proxy_frame.size();
    while (count != 0U) {
        const std::size_t step = count / 2U;
        const std::size_t candidate = first + step;
        if (compare_presentation_time(
                map.source_time_by_proxy_frame[candidate], source_time) < 0) {
            first = candidate + 1U;
            count -= step + 1U;
        } else {
            count = step;
        }
    }
    if (first == map.source_time_by_proxy_frame.size()) {
        return ProxyFrameLookupResult::success(std::nullopt);
    }
    return ProxyFrameLookupResult::success(static_cast<core::u64>(first));
}

} // namespace mediaserver::assets
