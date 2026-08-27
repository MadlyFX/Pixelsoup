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

inline constexpr std::string_view display_name = "display_name";
// The absolute path this asset lives at, on every machine in the show. It is
// the only content locator: nothing is copied, hashed, or content-addressed.
inline constexpr std::string_view source = "source";

inline constexpr std::string_view media_width = "media_width";
inline constexpr std::string_view media_height = "media_height";
inline constexpr std::string_view media_frame_count = "media_frame_count";
inline constexpr std::string_view media_rate_numerator =
    "media_rate_numerator";
inline constexpr std::string_view media_rate_denominator =
    "media_rate_denominator";

inline constexpr std::string_view model_triangle_count = "model_triangle_count";
inline constexpr std::string_view model_material_count = "model_material_count";
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
