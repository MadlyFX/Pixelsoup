#include <mediaserver/assets/asset_registry.hpp>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <type_traits>

namespace mediaserver::assets {
namespace {

constexpr core::u64 fnv_offset = 1469598103934665603ULL;
constexpr core::u64 fnv_prime = 1099511628211ULL;

void hash_byte(core::u64& hash, core::u8 value) noexcept {
    hash ^= value;
    hash *= fnv_prime;
}

void hash_u64(core::u64& hash, core::u64 value) noexcept {
    for (core::u32 shift = 0U; shift < 64U; shift += 8U) {
        hash_byte(hash, static_cast<core::u8>((value >> shift) & 0xFFU));
    }
}

void hash_u32(core::u64& hash, core::u32 value) noexcept {
    hash_u64(hash, static_cast<core::u64>(value));
}

void hash_double(core::u64& hash, double value) noexcept {
    hash_u64(hash, std::bit_cast<core::u64>(value));
}

void hash_text(core::u64& hash, std::string_view value) noexcept {
    hash_u64(hash, static_cast<core::u64>(value.size()));
    for (const char character : value) {
        hash_byte(hash, static_cast<core::u8>(character));
    }
}

template <typename T>
void hash_optional_u32(core::u64& hash,
                       const std::optional<T>& value) noexcept {
    hash_byte(hash, value.has_value() ? 1U : 0U);
    if (value.has_value()) {
        hash_u32(hash, static_cast<core::u32>(*value));
    }
}

} // namespace

AssetKind asset_kind(const Asset& asset) noexcept {
    if (std::holds_alternative<MediaAsset>(asset)) {
        return AssetKind::Media;
    }
    return std::get<ModelAsset>(asset).projection_mesh
        ? AssetKind::ProjectionMesh
        : AssetKind::Model;
}

doc::NodeId asset_id(const Asset& asset) noexcept {
    return std::visit([](const auto& held) noexcept { return held.id; }, asset);
}

std::string_view asset_display_name(const Asset& asset) noexcept {
    return std::visit(
        [](const auto& held) noexcept -> std::string_view {
            return held.display_name;
        },
        asset);
}

std::string_view asset_source(const Asset& asset) noexcept {
    return std::visit(
        [](const auto& held) noexcept -> std::string_view {
            return held.source;
        },
        asset);
}

const Asset* AssetRegistry::asset(doc::NodeId id) const noexcept {
    const auto found =
        std::lower_bound(assets.begin(), assets.end(), id,
                         [](const Asset& entry, doc::NodeId wanted) noexcept {
                             return asset_id(entry) < wanted;
                         });
    return found != assets.end() && asset_id(*found) == id ? &*found : nullptr;
}

const Asset*
AssetRegistry::asset_for_source(std::string_view exact_source) const noexcept {
    const auto found =
        std::find_if(assets.begin(), assets.end(),
                     [exact_source](const Asset& entry) noexcept {
                         return asset_source(entry) == exact_source;
                     });
    return found == assets.end() ? nullptr : &*found;
}

const Asset* AssetRegistry::asset_for_source(
    std::string_view exact_source, AssetKind kind) const noexcept {
    const auto found =
        std::find_if(assets.begin(), assets.end(),
                     [exact_source, kind](const Asset& entry) noexcept {
                         return asset_kind(entry) == kind &&
                             asset_source(entry) == exact_source;
                     });
    return found == assets.end() ? nullptr : &*found;
}

core::u64 digest(const AssetRegistry& registry) noexcept {
    core::u64 hash = fnv_offset;
    hash_u64(hash, static_cast<core::u64>(registry.assets.size()));
    for (const Asset& asset : registry.assets) {
        hash_u64(hash, asset_id(asset).value());
        hash_byte(hash, static_cast<core::u8>(asset_kind(asset)));
        hash_text(hash, asset_display_name(asset));
        hash_text(hash, asset_source(asset));
        std::visit(
            [&hash](const auto& held) noexcept {
                using Held = std::remove_cvref_t<decltype(held)>;
                if constexpr (std::is_same_v<Held, MediaAsset>) {
                    hash_byte(hash, held.dimensions.has_value() ? 1U : 0U);
                    if (held.dimensions.has_value()) {
                        hash_u32(hash, held.dimensions->width);
                        hash_u32(hash, held.dimensions->height);
                    }
                    hash_optional_u32(hash, held.frame_count);
                    hash_byte(hash, held.frame_rate.has_value() ? 1U : 0U);
                    if (held.frame_rate.has_value()) {
                        hash_u32(hash, held.frame_rate->numerator);
                        hash_u32(hash, held.frame_rate->denominator);
                    }
                } else {
                    hash_byte(hash, held.projection_mesh ? 1U : 0U);
                    hash_byte(hash, held.vertex_count.has_value() ? 1U : 0U);
                    if (held.vertex_count.has_value()) {
                        hash_u64(hash, *held.vertex_count);
                    }
                    hash_byte(hash, held.triangle_count.has_value() ? 1U : 0U);
                    if (held.triangle_count.has_value()) {
                        hash_u64(hash, *held.triangle_count);
                    }
                    hash_optional_u32(hash, held.material_count);
                    hash_byte(hash, held.bounds.has_value() ? 1U : 0U);
                    if (held.bounds.has_value()) {
                        hash_double(hash, held.bounds->minimum_x);
                        hash_double(hash, held.bounds->minimum_y);
                        hash_double(hash, held.bounds->minimum_z);
                        hash_double(hash, held.bounds->maximum_x);
                        hash_double(hash, held.bounds->maximum_y);
                        hash_double(hash, held.bounds->maximum_z);
                    }
                }
            },
            asset);
    }
    return hash;
}

} // namespace mediaserver::assets
