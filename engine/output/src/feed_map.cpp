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

// A world-fixed stage hashes nothing, so every project that never authored a
// stage keeps the digest it always had.
void mix(core::u64& state, const StagePlacement& stage) noexcept {
    if (stage == StagePlacement{}) return;
    mix(state, core::u64{0x5354414745ULL}); // "STAGE"
    mix(state, static_cast<core::u64>(stage.anchor));
    mix(state, stage.eye);
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
    mix(state, surface.stage);
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

scene::Canvas viewport_extent(std::span<const OutputMap> maps) noexcept {
    core::u64 right = 0U;
    core::u64 bottom = 0U;
    for (const OutputMap& map : maps) {
        if (map.viewport.width == 0U || map.viewport.height == 0U) {
            continue;
        }
        right = std::max(right, static_cast<core::u64>(map.viewport.x) +
                                    map.viewport.width);
        bottom = std::max(bottom, static_cast<core::u64>(map.viewport.y) +
                                      map.viewport.height);
    }
    constexpr core::u64 limit = 0xffff'ffffULL;
    return scene::Canvas{static_cast<core::u32>(std::min(right, limit)),
                         static_cast<core::u32>(std::min(bottom, limit))};
}

scene::CanvasRect scale_viewport(scene::CanvasRect viewport,
                                 scene::Canvas window,
                                 scene::Canvas picture) noexcept {
    if (window.width == 0U || window.height == 0U || picture.width == 0U ||
        picture.height == 0U || viewport.width == 0U ||
        viewport.height == 0U) {
        return {};
    }
    // One edge in the picture: floor, so a shared edge lands on one pixel
    // for both of the viewports that meet there.
    const auto edge = [](core::u64 at, core::u32 from, core::u32 to) {
        return std::min<core::u64>(at * to / from, to);
    };
    const core::u64 left = edge(viewport.x, window.width, picture.width);
    const core::u64 top = edge(viewport.y, window.height, picture.height);
    if (left >= picture.width || top >= picture.height) {
        return {};
    }
    const core::u64 right = std::max(
        edge(static_cast<core::u64>(viewport.x) + viewport.width,
             window.width, picture.width),
        left + 1U);
    const core::u64 bottom = std::max(
        edge(static_cast<core::u64>(viewport.y) + viewport.height,
             window.height, picture.height),
        top + 1U);
    return scene::CanvasRect{
        static_cast<core::u32>(left), static_cast<core::u32>(top),
        static_cast<core::u32>(std::min<core::u64>(right, picture.width) - left),
        static_cast<core::u32>(std::min<core::u64>(bottom, picture.height) - top)};
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

    mix(state, feed_map.stage);

    return state;
}

} // namespace mediaserver::output
