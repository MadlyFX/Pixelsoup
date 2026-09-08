#include "test_support.hpp"

#include <mediaserver/core/frame_arena.hpp>
#include <mediaserver/core/idle_report.hpp>
#include <mediaserver/core/strong_id.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace {

using FirstId = mediaserver::core::StrongId<struct FirstTag, std::uint64_t>;
using SecondId = mediaserver::core::StrongId<struct SecondTag, std::uint64_t>;

static_assert(!std::is_convertible_v<FirstId, SecondId>);
static_assert(!std::is_convertible_v<std::uint64_t, FirstId>);

MS_TEST("strong IDs preserve type boundaries") {
    MS_CHECK(FirstId{7U}.value() == 7U);
    MS_CHECK(FirstId{7U} == FirstId{7U});
    MS_CHECK(FirstId{7U} != FirstId{8U});
}

MS_TEST("frame arena aligns, exhausts, and resets caller-owned memory") {
    alignas(64) std::array<std::byte, 128U> storage{};
    mediaserver::core::FrameArena arena{storage};

    const auto first = arena.allocate(7U, 1U);
    const auto aligned = arena.allocate(16U, 16U);
    MS_CHECK(first != nullptr);
    MS_CHECK(aligned != nullptr);
    MS_CHECK(reinterpret_cast<std::uintptr_t>(aligned) % 16U == 0U);
    MS_CHECK(arena.used() >= 23U);
    MS_CHECK(arena.allocate(256U) == nullptr);

    arena.reset();
    MS_CHECK(arena.used() == 0U);
    MS_CHECK(arena.remaining() == arena.capacity());
}


// Honest idle reporting: a feature drawing nothing must say why, and must say
// it once. Repeating a reason every frame would be worse than the silence this
// replaces, so the dedup is the contract rather than a nicety.
MS_TEST("an idle reason is reported once and then stays quiet") {
    static const char feature[] = "unit test feature";
    static const char first[] = "the first distinct reason";
    static const char second[] = "a different reason";

    MS_CHECK(mediaserver::core::report_idle_once(feature, first));
    // Same reason, same pointer: already said.
    MS_CHECK(!mediaserver::core::report_idle_once(feature, first));
    MS_CHECK(!mediaserver::core::report_idle_once(feature, first));
    // A different reason is still heard - one noisy condition must not mask
    // another, which is the whole point of naming them separately.
    MS_CHECK(mediaserver::core::report_idle_once(feature, second));
    MS_CHECK(!mediaserver::core::report_idle_once(feature, second));

    // Nothing to say, and nothing that crashes saying it.
    MS_CHECK(!mediaserver::core::report_idle_once(nullptr, first));
    MS_CHECK(!mediaserver::core::report_idle_once(feature, nullptr));
}
} // namespace
