#pragma once

#include <string_view>

// Document vocabulary for persistent project assets. These are registry
// entries, not scene placements: a scene.media/scene.model node may refer to a
// source, but removing that placement never removes an asset.* node.
namespace mediaserver::assets::schema {

inline constexpr std::string_view media_type = "asset.media";
inline constexpr std::string_view model_type = "asset.model";
// Calibration geometry is a project asset too. A physical output surface may
// refer to this node without making the mesh's content identity part of the
// surface placement itself.
inline constexpr std::string_view projection_mesh_type =
    "asset.projection_mesh";

// A Library folder. It carries a display_name and an optional `folder`
// naming its parent folder, exactly as an asset names the folder it is in.
inline constexpr std::string_view folder_type = "asset.folder";

// A vehicle process plateset: the camera angles of one driving take, each
// filed in the LED Car Process slot it plays in. It names media assets rather
// than files, so it has no source of its own and never enters a manifest.
inline constexpr std::string_view plateset_type = "asset.plateset";
// Which rig template the plateset is laid out for (`nine`, `five`, `three`).
// Absent is nine. It decides which slots the Library offers, never which
// assigned slot plays: an assignment outside the template is kept.
inline constexpr std::string_view plateset_rig = "plateset_rig";
inline constexpr std::string_view plateset_rig_nine = "nine";
inline constexpr std::string_view plateset_rig_five = "five";
inline constexpr std::string_view plateset_rig_three = "three";
// Per slot, `slot_<role>` names the asset.media node playing there and
// `slot_<role>_offset` is the sync offset: how many source frames of that
// plate come before the take's common start. Roles are the LED Car Process
// socket names in `plateset_slot_key` order.
inline constexpr std::string_view plateset_slot_prefix = "slot_";
inline constexpr std::string_view plateset_offset_suffix = "_offset";
// Every placed plate of the set decodes at half size. Present and true, or
// absent.
inline constexpr std::string_view plateset_half_resolution =
    "plateset_half_resolution";
// Moments kept from the front plate: `moment_<n>_frame` (front plate source
// frame) and `moment_<n>_kind` (`stopped`, `turning_left`, `turning_right`,
// `bump`, `into_shade`), numbered from zero with no gaps. They belong to the
// footage, so they follow every placement.
inline constexpr std::string_view plateset_moment_prefix = "moment_";

inline constexpr std::string_view display_name = "display_name";
// The asset.folder node an asset (or folder) is filed under. Absent: the
// Library's top level. Organisation only; it never affects rendering.
inline constexpr std::string_view folder = "folder";
// The absolute path this asset lives at, on every machine in the show. It is
// the content locator; the optional expected fingerprint below verifies that
// every machine resolved that locator to the revision the project authored.
inline constexpr std::string_view source = "source";
// These fields are an all-or-nothing pair. Absence means an asset has not yet
// been fingerprinted; a partially authored or malformed pair rejects the node.
inline constexpr std::string_view expected_byte_length =
    "expected_byte_length";
inline constexpr std::string_view expected_sha256 = "expected_sha256";

inline constexpr std::string_view media_width = "media_width";
inline constexpr std::string_view media_height = "media_height";
inline constexpr std::string_view media_frame_count = "media_frame_count";
inline constexpr std::string_view media_rate_numerator =
    "media_rate_numerator";
inline constexpr std::string_view media_rate_denominator =
    "media_rate_denominator";
inline constexpr std::string_view media_codec = "media_codec";
inline constexpr std::string_view media_start_timecode =
    "media_start_timecode";
// Profile names use the versioned derivative catalog vocabulary. Policy is
// stored as text so project files remain stable if C++ enum layouts change.
inline constexpr std::string_view requested_proxy_profile =
    "requested_proxy_profile";
inline constexpr std::string_view proxy_playback_policy =
    "proxy_playback_policy";
inline constexpr std::string_view proxy_policy_source = "source";
inline constexpr std::string_view proxy_policy_prefer = "prefer_proxy";
inline constexpr std::string_view proxy_policy_require = "require_proxy";

inline constexpr std::string_view model_triangle_count = "model_triangle_count";
inline constexpr std::string_view model_material_count = "model_material_count";
inline constexpr std::string_view model_vertex_count = "model_vertex_count";
inline constexpr std::string_view model_texture_count = "model_texture_count";
inline constexpr std::string_view model_texture_width = "model_texture_width";
inline constexpr std::string_view model_texture_height =
    "model_texture_height";
inline constexpr std::string_view projection_vertex_count =
    "projection_vertex_count";
inline constexpr std::string_view projection_triangle_count =
    "projection_triangle_count";
inline constexpr std::string_view bounds_minimum_x = "bounds_minimum_x";
inline constexpr std::string_view bounds_minimum_y = "bounds_minimum_y";
inline constexpr std::string_view bounds_minimum_z = "bounds_minimum_z";
inline constexpr std::string_view bounds_maximum_x = "bounds_maximum_x";
inline constexpr std::string_view bounds_maximum_y = "bounds_maximum_y";
inline constexpr std::string_view bounds_maximum_z = "bounds_maximum_z";

} // namespace mediaserver::assets::schema
