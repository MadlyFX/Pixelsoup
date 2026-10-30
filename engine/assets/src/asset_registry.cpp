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

void hash_fingerprint(
    core::u64& hash,
    const std::optional<ContentFingerprint>& fingerprint) noexcept {
    hash_byte(hash, fingerprint.has_value() ? 1U : 0U);
    if (!fingerprint.has_value()) {
        return;
    }
    hash_u64(hash, fingerprint->byte_length);
    for (const core::u8 byte : fingerprint->sha256.bytes) {
        hash_byte(hash, byte);
    }
}

} // namespace

AssetKind asset_kind(const Asset& asset) noexcept {
    if (std::holds_alternative<MediaAsset>(asset)) {
        return AssetKind::Media;
    }
    if (std::holds_alternative<PlatesetAsset>(asset)) {
        return AssetKind::Plateset;
    }
    if (std::holds_alternative<ParticleEffectAsset>(asset)) {
        return AssetKind::ParticleEffect;
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
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(held)>,
                                         PlatesetAsset>) {
                return {};
            } else {
                return held.source;
            }
        },
        asset);
}

const std::optional<ContentFingerprint>&
asset_fingerprint(const Asset& asset) noexcept {
    // A plateset names other assets and has no content of its own.
    static const std::optional<ContentFingerprint> none;
    return std::visit(
        [](const auto& held) -> const std::optional<ContentFingerprint>& {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(held)>,
                                         PlatesetAsset>) {
                return none;
            } else {
                return held.fingerprint;
            }
        },
        asset);
}

doc::NodeId asset_folder(const Asset& asset) noexcept {
    return std::visit([](const auto& held) noexcept { return held.folder; },
                      asset);
}

const AssetFolder* AssetRegistry::folder(doc::NodeId id) const noexcept {
    if (id == doc::NodeId{}) {
        return nullptr;
    }
    const auto found = std::lower_bound(
        folders.begin(), folders.end(), id,
        [](const AssetFolder& entry, doc::NodeId wanted) noexcept {
            return entry.id < wanted;
        });
    return found != folders.end() && found->id == id ? &*found : nullptr;
}

doc::NodeId AssetRegistry::folder_of(const Asset& entry) const noexcept {
    const doc::NodeId held = asset_folder(entry);
    return folder(held) != nullptr ? held : doc::NodeId{};
}

bool AssetRegistry::folder_within(doc::NodeId candidate,
                                  doc::NodeId ancestor) const noexcept {
    // Evaluation made the folders acyclic; the bound keeps a hand-built
    // registry from looping.
    for (std::size_t steps = 0U; steps <= folders.size(); ++steps) {
        if (candidate == ancestor) {
            return true;
        }
        const AssetFolder* const held = folder(candidate);
        if (held == nullptr) {
            return false;
        }
        candidate = held->parent;
    }
    return false;
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
        hash_u64(hash, asset_folder(asset).value());
        std::visit(
            [&hash](const auto& held) noexcept {
                using Held = std::remove_cvref_t<decltype(held)>;
                if constexpr (std::is_same_v<Held, PlatesetAsset>) {
                    hash_byte(hash, static_cast<core::u8>(held.rig));
                    for (const PlatesetSlotAssignment& slot : held.slots) {
                        hash_u64(hash, slot.media.value());
                        hash_u64(hash, static_cast<core::u64>(slot.offset));
                    }
                    hash_byte(hash, held.half_resolution ? 1U : 0U);
                    hash_u64(hash, static_cast<core::u64>(held.moments.size()));
                    for (const PlatesetMoment& moment : held.moments) {
                        hash_u64(hash, static_cast<core::u64>(moment.frame));
                        hash_byte(hash, static_cast<core::u8>(moment.kind));
                    }
                } else if constexpr (std::is_same_v<Held, MediaAsset>) {
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
                    hash_fingerprint(hash, held.fingerprint);
                    hash_byte(hash,
                              held.requested_proxy_profile.has_value() ? 1U
                                                                       : 0U);
                    if (held.requested_proxy_profile.has_value()) {
                        hash_u32(hash, static_cast<core::u32>(
                                           *held.requested_proxy_profile));
                    }
                    hash_byte(hash, static_cast<core::u8>(
                                        held.proxy_playback_policy));
                    hash_text(hash, held.codec);
                    hash_text(hash, held.start_timecode);
                } else if constexpr (std::is_same_v<Held,
                                                    ParticleEffectAsset>) {
                    hash_fingerprint(hash, held.fingerprint);
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
                    hash_fingerprint(hash, held.fingerprint);
                    hash_optional_u32(hash, held.texture_count);
                    hash_byte(hash,
                              held.largest_texture.has_value() ? 1U : 0U);
                    if (held.largest_texture.has_value()) {
                        hash_u32(hash, held.largest_texture->width);
                        hash_u32(hash, held.largest_texture->height);
                    }
                }
            },
            asset);
    }
    hash_u64(hash, static_cast<core::u64>(registry.folders.size()));
    for (const AssetFolder& folder : registry.folders) {
        hash_u64(hash, folder.id.value());
        hash_text(hash, folder.display_name);
        hash_u64(hash, folder.parent.value());
    }
    return hash;
}

} // namespace mediaserver::assets
