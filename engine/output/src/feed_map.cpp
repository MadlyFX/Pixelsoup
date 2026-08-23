#include <mediaserver/output/feed_map.hpp>

namespace mediaserver::output {
namespace {

constexpr core::u64 fnv_offset_basis = 14'695'981'039'346'656'037ULL;
constexpr core::u64 fnv_prime = 1'099'511'628'211ULL;

void mix(core::u64& state, core::u64 value) noexcept {
    for (core::u32 shift = 0U; shift < 64U; shift += 8U) {
        state ^= (value >> shift) & 0xffULL;
        state *= fnv_prime;
    }
}

void mix(core::u64& state, scene::CanvasRect rect) noexcept {
    mix(state, static_cast<core::u64>(rect.x));
    mix(state, static_cast<core::u64>(rect.y));
    mix(state, static_cast<core::u64>(rect.width));
    mix(state, static_cast<core::u64>(rect.height));
}

} // namespace

const OutputMap* FeedMap::map_for(core::u32 node_index) const noexcept {
    for (const auto& map : maps) {
        if (map.node_index == node_index) {
            return &map;
        }
    }
    return nullptr;
}

core::u64 digest(const FeedMap& feed_map) noexcept {
    auto state = fnv_offset_basis;

    mix(state, static_cast<core::u64>(feed_map.maps.size()));
    for (const auto& map : feed_map.maps) {
        mix(state, static_cast<core::u64>(map.node_index));
        mix(state, map.source);
    }

    return state;
}

} // namespace mediaserver::output
