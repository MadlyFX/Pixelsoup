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

[[nodiscard]] std::string_view extension(std::string_view source) noexcept {
    const std::size_t slash = source.find_last_of("/\\");
    const std::size_t dot = source.find_last_of('.');
    if (dot == std::string_view::npos ||
        (slash != std::string_view::npos && dot < slash)) {
        return {};
    }
    return source.substr(dot);
}

[[nodiscard]] bool ascii_iequals(std::string_view left,
                                 std::string_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index) {
        const auto lower = [](char character) noexcept {
            return character >= 'A' && character <= 'Z'
                ? static_cast<char>(character - 'A' + 'a')
                : character;
        };
        if (lower(left[index]) != lower(right[index])) {
            return false;
        }
    }
    return true;
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

const AssetContent& asset_content(const Asset& asset) noexcept {
    return std::visit(
        [](const auto& held) noexcept -> const AssetContent& {
            return held.content;
        },
        asset);
}

bool asset_requires_dependency_manifest(const Asset& asset) noexcept {
    if (const auto* const media = std::get_if<MediaAsset>(&asset)) {
        // One '#' run denotes an image sequence. A primary-file digest cannot
        // describe every frame or prove a node has the complete sequence.
        return media->source.find('#') != std::string::npos;
    }
    const ModelAsset& model = std::get<ModelAsset>(asset);
    const std::string_view suffix = extension(model.source);
    // Text glTF can refer to buffers and images. A scene OBJ can refer to MTL
    // and texture files; projection OBJ consumes geometry/UV only, so its
    // rendered result is fully represented by the primary file.
    return ascii_iequals(suffix, ".gltf") ||
        (ascii_iequals(suffix, ".obj") && !model.projection_mesh);
}

AssetPortability asset_portability(const Asset& asset) noexcept {
    const AssetContent& content = asset_content(asset);
    if (!content.identity.has_value()) {
        return AssetPortability::Unverified;
    }
    if (asset_requires_dependency_manifest(asset)) {
        return AssetPortability::DependencyManifestRequired;
    }
    return content.policy == AssetDistributionPolicy::External
        ? AssetPortability::IdentifiedExternal
        : AssetPortability::Portable;
}

bool asset_is_portable(const Asset& asset) noexcept {
    return asset_portability(asset) == AssetPortability::Portable;
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

const Asset*
AssetRegistry::asset_for_content(const ContentIdentity& identity) const noexcept {
    const auto found =
        std::find_if(assets.begin(), assets.end(),
                     [&identity](const Asset& entry) noexcept {
                         return asset_content(entry).identity == identity;
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
        const AssetContent& content = asset_content(asset);
        hash_byte(hash, static_cast<core::u8>(content.policy));
        hash_byte(hash, content.identity.has_value() ? 1U : 0U);
        if (content.identity.has_value()) {
            for (const core::u8 byte : content.identity->sha256) {
                hash_byte(hash, byte);
            }
            hash_u64(hash, content.identity->byte_size);
        }
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
