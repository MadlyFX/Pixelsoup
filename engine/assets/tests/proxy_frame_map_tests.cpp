#include <mediaserver/assets/proxy_frame_map.hpp>

#include <array>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

namespace assets = mediaserver::assets;
namespace core = mediaserver::core;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} +
                                 std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define FRAME_MAP_CHECK(...)                                                 \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] assets::PresentationTime time(core::u64 numerator,
                                             core::u64 denominator) {
    auto result = assets::make_presentation_time(numerator, denominator);
    if (!result) {
        throw std::runtime_error{"invalid test timestamp"};
    }
    return std::move(result).value();
}

void fractional_rates_remain_exact() {
    const assets::MediaFrameRate rate{24'000U, 1'001U};
    const auto first = assets::presentation_time_for_frame(1U, rate);
    const auto one_second_batch = assets::presentation_time_for_frame(
        24'000U, rate);
    FRAME_MAP_CHECK(first);
    FRAME_MAP_CHECK(one_second_batch);
    FRAME_MAP_CHECK(first.value() == time(1'001U, 24'000U));
    FRAME_MAP_CHECK(one_second_batch.value() == time(1'001U, 1U));
}

void continued_fraction_comparison_cannot_overflow() {
    constexpr core::u64 maximum = std::numeric_limits<core::u64>::max();
    const assets::PresentationTime left{maximum - 2U, maximum - 1U};
    const assets::PresentationTime right{maximum - 1U, maximum};
    FRAME_MAP_CHECK(assets::valid_presentation_time(left));
    FRAME_MAP_CHECK(assets::valid_presentation_time(right));
    FRAME_MAP_CHECK(assets::compare_presentation_time(left, right) < 0);
    FRAME_MAP_CHECK(assets::compare_presentation_time(right, left) > 0);
}

void identity_maps_round_trip_fractional_frame_ordinals() {
    const assets::MediaFrameRate rate{30'000U, 1'001U};
    const auto result = assets::make_identity_proxy_frame_map(120U, rate);
    FRAME_MAP_CHECK(result);
    const assets::ProxyFrameMap& map = result.value();
    FRAME_MAP_CHECK(assets::valid_proxy_frame_map(map));
    const auto requested = assets::presentation_time_for_frame(73U, rate);
    FRAME_MAP_CHECK(requested);
    const auto before = assets::proxy_frame_at_or_before(
        map, requested.value());
    const auto after = assets::proxy_frame_at_or_after(
        map, requested.value());
    FRAME_MAP_CHECK(before);
    FRAME_MAP_CHECK(after);
    FRAME_MAP_CHECK(before.value() == 73U);
    FRAME_MAP_CHECK(after.value() == 73U);
}

void duplicate_source_times_have_explicit_tie_rules() {
    assets::ProxyFrameMap map{
        {30U, 1U},
        {time(0U, 1U), time(1U, 30U), time(1U, 30U), time(1U, 15U)},
    };
    FRAME_MAP_CHECK(assets::valid_proxy_frame_map(map));
    const auto before = assets::proxy_frame_at_or_before(map, time(1U, 30U));
    const auto after = assets::proxy_frame_at_or_after(map, time(1U, 30U));
    FRAME_MAP_CHECK(before);
    FRAME_MAP_CHECK(after);
    FRAME_MAP_CHECK(before.value() == 2U);
    FRAME_MAP_CHECK(after.value() == 1U);

    const auto before_start = assets::proxy_frame_at_or_before(
        map, time(0U, 1U));
    const auto after_end = assets::proxy_frame_at_or_after(
        map, time(1U, 1U));
    FRAME_MAP_CHECK(before_start.value() == 0U);
    FRAME_MAP_CHECK(!after_end.value().has_value());
}

void malformed_maps_and_overflow_are_rejected() {
    FRAME_MAP_CHECK(!assets::make_presentation_time(1U, 0U));
    FRAME_MAP_CHECK(!assets::presentation_time_for_frame(
        1U, assets::MediaFrameRate{}));
    FRAME_MAP_CHECK(!assets::presentation_time_for_frame(
        std::numeric_limits<core::u64>::max(),
        assets::MediaFrameRate{1U,
            std::numeric_limits<core::u32>::max()}));

    assets::ProxyFrameMap descending{
        {24U, 1U},
        {time(1U, 1U), time(1U, 2U)},
    };
    FRAME_MAP_CHECK(!assets::valid_proxy_frame_map(descending));
    FRAME_MAP_CHECK(!assets::proxy_frame_at_or_before(
        descending, time(3U, 4U)));

    assets::ProxyFrameMap noncanonical_rate{{60U, 2U}, {time(0U, 1U)}};
    FRAME_MAP_CHECK(!assets::valid_proxy_frame_map(noncanonical_rate));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 5U> cases{{
    {"fractional rates remain exact", &fractional_rates_remain_exact},
    {"continued fraction comparison cannot overflow",
     &continued_fraction_comparison_cannot_overflow},
    {"identity maps round trip fractional frame ordinals",
     &identity_maps_round_trip_fractional_frame_ordinals},
    {"duplicate source times have explicit tie rules",
     &duplicate_source_times_have_explicit_tie_rules},
    {"malformed maps and overflow are rejected",
     &malformed_maps_and_overflow_are_rejected},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const Case& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& caught) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << caught.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
