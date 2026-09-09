#pragma once

#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/assets/derivative_profile_id.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Persistent project assets as an evaluated, headless document view.
//
// An asset's stable identity is its document NodeId. Its absolute source path
// locates content and its optional fingerprint identifies the expected content
// revision at that path. Scene placements are separate nodes and may come and
// go without affecting this registry.
namespace mediaserver::assets {

struct PixelDimensions final {
    core::u32 width{};
    core::u32 height{};

    bool operator==(const PixelDimensions&) const = default;
};

struct MediaFrameRate final {
    core::u32 numerator{};
    core::u32 denominator{};

    bool operator==(const MediaFrameRate&) const = default;
};

struct ModelBounds final {
    double minimum_x{};
    double minimum_y{};
    double minimum_z{};
    double maximum_x{};
    double maximum_y{};
    double maximum_z{};

    bool operator==(const ModelBounds&) const = default;
};

// Authored playback intent is separate from machine-local proxy availability.
// Source is the default for imported assets; PreferProxy may visibly fall back
// to this same asset's source, while RequireProxy may not.
enum class ProxyPlaybackPolicy : core::u8 {
    Source,
    PreferProxy,
    RequireProxy,
};

struct MediaAsset final {
    doc::NodeId id{};
    std::string display_name;
    // Absolute path, identical on every machine in the show.
    std::string source;
    // Probe results are stored in the document when available. Absence is
    // distinct from zero: an unprobed asset remains valid, while an explicitly
    // authored zero-size or zero-frame result is malformed.
    std::optional<PixelDimensions> dimensions;
    std::optional<core::u32> frame_count;
    std::optional<MediaFrameRate> frame_rate;
    std::optional<ContentFingerprint> fingerprint;
    // The profile is an authored request, not a derivative identity. Runtime
    // generation state and cache paths never enter this persistent model.
    std::optional<DerivativeProfileId> requested_proxy_profile;
    ProxyPlaybackPolicy proxy_playback_policy{ProxyPlaybackPolicy::Source};

    bool operator==(const MediaAsset&) const = default;
};

struct ModelAsset final {
    doc::NodeId id{};
    std::string display_name;
    std::string source;
    std::optional<core::u64> triangle_count;
    std::optional<core::u32> material_count;
    std::optional<ModelBounds> bounds;
    // Projection meshes share the model loader's geometric probe vocabulary,
    // but remain a distinct manifest role and never appear as scene models.
    std::optional<core::u64> vertex_count;
    bool projection_mesh{};
    std::optional<ContentFingerprint> fingerprint;

    bool operator==(const ModelAsset&) const = default;
};

using Asset = std::variant<MediaAsset, ModelAsset>;

enum class AssetKind : core::u8 {
    Media,
    Model,
    ProjectionMesh,
    IesProfile,
    ColorLut,
};

[[nodiscard]] AssetKind asset_kind(const Asset& asset) noexcept;
[[nodiscard]] doc::NodeId asset_id(const Asset& asset) noexcept;
[[nodiscard]] std::string_view asset_display_name(const Asset& asset) noexcept;
[[nodiscard]] std::string_view asset_source(const Asset& asset) noexcept;
[[nodiscard]] const std::optional<ContentFingerprint>&
asset_fingerprint(const Asset& asset) noexcept;

inline constexpr core::u32 max_project_assets = 1'024U;

struct AssetRegistry final {
    // Sorted by document NodeId after exact-(kind, source) deduplication.
    // Source comparison is deliberately byte-for-byte, case-sensitive, and
    // performs no filesystem lookup or path canonicalisation. The lowest
    // NodeId wins within a kind; one source may have several manifest roles.
    std::vector<Asset> assets;

    [[nodiscard]] const Asset* asset(doc::NodeId id) const noexcept;
    [[nodiscard]] const Asset*
    asset_for_source(std::string_view exact_source) const noexcept;
    // Kind-aware lookup for placements and reimports. This avoids treating a
    // scene model and projection mesh that use the same file as one asset.
    [[nodiscard]] const Asset*
    asset_for_source(std::string_view exact_source,
                     AssetKind kind) const noexcept;
};

// FNV-1a over every evaluated authored field, including optional-presence
// markers and exact double bit patterns for model bounds.
[[nodiscard]] core::u64 digest(const AssetRegistry& registry) noexcept;

} // namespace mediaserver::assets
