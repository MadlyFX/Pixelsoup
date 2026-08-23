#include "test_support.hpp"

#include <mediaserver/core/frame_arena.hpp>
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

} // namespace

