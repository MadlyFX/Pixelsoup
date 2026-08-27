#pragma once

#include <string_view>

namespace mediaserver::sync::schema {

inline constexpr std::string_view policy_type = "sync.policy";

inline constexpr std::string_view mode = "mode";
inline constexpr std::string_view width = "width";
inline constexpr std::string_view height = "height";
inline constexpr std::string_view refresh_numerator = "refresh_numerator";
inline constexpr std::string_view refresh_denominator = "refresh_denominator";
inline constexpr std::string_view group = "group";
inline constexpr std::string_view barrier = "barrier";
inline constexpr std::string_view failure_policy = "failure_policy";
inline constexpr std::string_view late_frame_policy = "late_frame_policy";

inline constexpr std::string_view unsynced_mode = "unsynced";
inline constexpr std::string_view framelock_mode = "framelock";
inline constexpr std::string_view external_genlock_mode = "external_genlock";

inline constexpr std::string_view strict_failure = "strict";
inline constexpr std::string_view degraded_failure = "degraded";

inline constexpr std::string_view hold_group_late_frame = "hold_group";
inline constexpr std::string_view advance_late_frame = "advance";

} // namespace mediaserver::sync::schema
