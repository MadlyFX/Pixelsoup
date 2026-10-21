#include <mediaserver/output/feed_map.hpp>

#include <bit>
#include <algorithm>
#include <string_view>

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

void mix(core::u64& state, float value) noexcept {
    mix(state, static_cast<core::u64>(std::bit_cast<core::u32>(value)));
}

void mix(core::u64& state, scene::Vec3 value) noexcept {
    mix(state, value.x);
    mix(state, value.y);
    mix(state, value.z);
}

void mix(core::u64& state, const PhysicalSurface& surface) noexcept {
    mix(state, surface.center);
    mix(state, surface.right);
    mix(state, surface.up);
    mix(state, surface.width);
    mix(state, surface.height);
    mix(state, static_cast<core::u64>(surface.raster.width));
    mix(state, static_cast<core::u64>(surface.raster.height));
}

void mix(core::u64& state, std::string_view text) noexcept;

void mix(core::u64& state, const scene::WallColorSettings& source) noexcept {
    const auto color = scene::clamped(source);
    if (color == scene::WallColorSettings{}) return;
    mix(state, core::u64{0x57414c4c434f4c52ULL});
    mix(state, static_cast<core::u64>(color.enabled));
    for (float c : color.primaries) mix(state, c);
    mix(state, color.camera_inverse_lut_source);
}

void mix(core::u64& state,
         const ResolvedProjectionSurface& surface) noexcept {
    mix(state, surface.geometry);
    mix(state, static_cast<core::u64>(surface.shape));
    mix(state, surface.horizontal_arc_turns);
    mix(state, static_cast<core::u64>(surface.mesh_columns));
    mix(state, surface.mesh_source);
    mix(state, surface.mesh_scale);
    mix(state, surface.color);
    if (surface.mesh_asset) {
        mix(state, 0x6173736574ULL);
        mix(state, surface.mesh_asset.value());
    }
}

void mix(core::u64& state, std::string_view text) noexcept {
    mix(state, static_cast<core::u64>(text.size()));
    for (const char character : text) {
        state ^= static_cast<core::u8>(character);
        state *= fnv_prime;
    }
}

} // namespace

std::span<const OutputMap> FeedMap::maps_for(core::u32 node_index) const noexcept {
    const auto first = std::lower_bound(
        maps.begin(), maps.end(), node_index,
        [](const OutputMap& map, core::u32 wanted) noexcept {
            return map.node_index < wanted;
        });
    const auto last = std::upper_bound(
        first, maps.end(), node_index,
        [](core::u32 wanted, const OutputMap& map) noexcept {
            return wanted < map.node_index;
        });
    return std::span<const OutputMap>{first, last};
}

const OutputMap* FeedMap::map_node(doc::NodeId node) const noexcept {
    for (const auto& map : maps) {
        if (map.node == node) {
            return &map;
        }
    }
    return nullptr;
}

const OutputMap* FeedMap::map_for(core::u32 node_index) const noexcept {
    const auto bound = maps_for(node_index);
    return bound.empty() ? nullptr : &bound.front();
}

const ProjectionSurface* FeedMap::surface_for(doc::NodeId node) const noexcept {
    for (const auto& surface : surfaces) {
        if (surface.node == node) {
            return &surface;
        }
    }
    return nullptr;
}

core::u64 digest(const FeedMap& feed_map) noexcept {
    auto state = fnv_offset_basis;

    mix(state, static_cast<core::u64>(feed_map.maps.size()));
    for (const auto& map : feed_map.maps) {
        mix(state, map.node.value());
        mix(state, static_cast<core::u64>(map.node_index));
        mix(state, map.name);
        mix(state, static_cast<core::u64>(map.order));
        mix(state, map.source);
        mix(state, static_cast<core::u64>(map.projection));
        mix(state, map.surface.value());
        mix(state, static_cast<core::u64>(
                       map.resolved_surface.has_value() ? 1U : 0U));
        if (map.resolved_surface.has_value()) {
            mix(state, *map.resolved_surface);
        }
        mix(state, map.viewport);
    }

    mix(state, static_cast<core::u64>(feed_map.surfaces.size()));
    for (const auto& surface : feed_map.surfaces) {
        mix(state, surface.node.value());
        mix(state, surface.name);
        mix(state, static_cast<core::u64>(surface.order));
        mix(state, static_cast<core::u64>(surface.locked));
        mix(state, surface.geometry);
        mix(state, static_cast<core::u64>(surface.shape));
        mix(state, surface.horizontal_arc_turns);
        mix(state, static_cast<core::u64>(surface.mesh_columns));
        mix(state, surface.mesh_source);
        mix(state, surface.mesh_scale);
    mix(state, surface.color);
        if (surface.mesh_asset) {
            mix(state, 0x6173736574ULL);
            mix(state, surface.mesh_asset.value());
        }
    }

    return state;
}

} // namespace mediaserver::output
