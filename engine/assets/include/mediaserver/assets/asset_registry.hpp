#pragma once

#include <mediaserver/assets/content_fingerprint.hpp>
#include <mediaserver/assets/derivative_profile_id.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>

#include <array>
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
    // Descriptive probe results the Library shows. Neither changes how the
    // source decodes: the codec is FFmpeg's name for the video stream, and
    // the start timecode is the container's own `HH:MM:SS:FF` (`;` before
    // the frames for drop-frame), absent when the file carries none.
    std::string codec;
    std::string start_timecode;
    // The Library folder holding this asset; zero is the Library's top level.
    doc::NodeId folder{};

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
    // The model's own textures as imported: how many, and the largest one's
    // size. Absent for an asset imported before these were recorded.
    std::optional<core::u32> texture_count;
    std::optional<PixelDimensions> largest_texture;
    doc::NodeId folder{};

    bool operator==(const ModelAsset&) const = default;
};

// The slots of a vehicle process plateset, in LED Car Process input port
// order: eight wall plates clockwise from the front, then the ceiling
// reflection. The value is the port index, so a slot wires to its socket
// without a lookup table.
enum class PlatesetSlot : core::u8 {
    Front,
    FrontRight,
    Right,
    BackRight,
    Back,
    BackLeft,
    Left,
    FrontLeft,
    Ceiling,
};

inline constexpr std::size_t plateset_slot_count = 9U;

// Rig templates: which slots the Library offers for this array.
enum class PlatesetRig : core::u8 {
    Nine,
    Five,
    Three,
};

struct PlatesetSlotAssignment final {
    // The asset.media node playing in this slot; zero is an empty slot.
    doc::NodeId media{};
    // Source frames of this plate before the take's common start. Never
    // negative: the plate that starts last has zero.
    core::i64 offset{};

    bool operator==(const PlatesetSlotAssignment&) const = default;
};

// What the car did. Its kind is all a moment says: the name it reads by and
// the colour it is drawn in follow from it, the same on every take.
enum class PlatesetMomentKind : core::u8 {
    Stopped,
    TurningLeft,
    TurningRight,
    Bump,
    IntoShade,
};

// A moment kept from the front plate - a stop, a turn - at that plate's own
// source frame, so it stays with the footage wherever the take is placed.
struct PlatesetMoment final {
    core::i64 frame{};
    PlatesetMomentKind kind{PlatesetMomentKind::Stopped};

    bool operator==(const PlatesetMoment&) const = default;
};

inline constexpr std::size_t max_plateset_moments = 256U;

struct PlatesetAsset final {
    doc::NodeId id{};
    std::string display_name;
    PlatesetRig rig{PlatesetRig::Nine};
    std::array<PlatesetSlotAssignment, plateset_slot_count> slots{};
    // Placed plates decode at half size: a quarter of the memory per frame.
    bool half_resolution{};
    // In front plate frame order.
    std::vector<PlatesetMoment> moments;
    doc::NodeId folder{};

    [[nodiscard]] const PlatesetSlotAssignment&
    slot(PlatesetSlot role) const noexcept {
        return slots[static_cast<std::size_t>(role)];
    }

    bool operator==(const PlatesetAsset&) const = default;
};

using Asset = std::variant<MediaAsset, ModelAsset, PlatesetAsset>;

enum class AssetKind : core::u8 {
    Media,
    Model,
    ProjectionMesh,
    IesProfile,
    ColorLut,
    Plateset,
};

[[nodiscard]] AssetKind asset_kind(const Asset& asset) noexcept;
[[nodiscard]] doc::NodeId asset_id(const Asset& asset) noexcept;
[[nodiscard]] std::string_view asset_display_name(const Asset& asset) noexcept;
[[nodiscard]] std::string_view asset_source(const Asset& asset) noexcept;
[[nodiscard]] const std::optional<ContentFingerprint>&
asset_fingerprint(const Asset& asset) noexcept;
[[nodiscard]] doc::NodeId asset_folder(const Asset& asset) noexcept;

// A Library folder: organisation only. It never changes what an asset is or
// how it renders, and removing one never removes the assets inside it.
// `parent` is zero at the Library's top level.
struct AssetFolder final {
    doc::NodeId id{};
    std::string display_name;
    doc::NodeId parent{};

    bool operator==(const AssetFolder&) const = default;
};

inline constexpr core::u32 max_asset_folders = 512U;

inline constexpr core::u32 max_project_assets = 1'024U;

struct AssetRegistry final {
    // Sorted by document NodeId after exact-(kind, source) deduplication.
    // Source comparison is deliberately byte-for-byte, case-sensitive, and
    // performs no filesystem lookup or path canonicalisation. The lowest
    // NodeId wins within a kind; one source may have several manifest roles.
    std::vector<Asset> assets;
    // Sorted by NodeId. Every `parent` names another folder here, and no
    // folder is its own ancestor: a dangling or cyclic parent is evaluated
    // as the top level, so one bad node never hides a branch.
    std::vector<AssetFolder> folders;

    [[nodiscard]] const Asset* asset(doc::NodeId id) const noexcept;
    [[nodiscard]] const AssetFolder* folder(doc::NodeId id) const noexcept;
    // Where `asset` shows in the Library: its folder when that folder exists,
    // otherwise the top level.
    [[nodiscard]] doc::NodeId folder_of(const Asset& asset) const noexcept;
    // True when `folder` is `ancestor` or sits anywhere beneath it.
    [[nodiscard]] bool folder_within(doc::NodeId folder,
                                     doc::NodeId ancestor) const noexcept;
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
