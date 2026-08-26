#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <system_error>

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/wire/protocol_codec.hpp>

// Per-user recent-project state.
//
// This is deliberately not a document node and never lives beside a project:
// it describes the editor installation, not the show. A stale or damaged
// setting is recoverable startup metadata and must never prevent the editor
// from opening its ordinary untitled project.
namespace mediaserver::persist {

inline constexpr std::uint16_t current_recent_project_format_version = 1U;
inline constexpr std::size_t max_recent_project_path_bytes = 32U * 1024U;

enum class RecentProjectErrorCode {
    SettingsUnavailable,
    InvalidSettingsPath,
    InvalidProjectPath,
    DirectoryCreateFailed,
    OpenFailed,
    ReadFailed,
    WriteFailed,
    SyncFailed,
    ReplaceFailed,
    InvalidFormat,
    UnsupportedVersion,
    CorruptFile,
    SizeLimitExceeded,
    AllocationFailed,
};

struct RecentProjectError final {
    RecentProjectErrorCode code{RecentProjectErrorCode::InvalidFormat};
    std::error_code system_error;
    // As with project files, a post-replacement directory-sync failure means
    // the new setting is installed although power-loss durability is unknown.
    bool committed{};

    bool operator==(const RecentProjectError&) const = default;
};

using RecentProjectSettingsPathResult =
    core::Result<std::filesystem::path, RecentProjectError>;
using RecentProjectPathLoadResult = core::Result<
    std::optional<std::filesystem::path>, RecentProjectError>;
using RecentProjectPathSaveResult =
    core::Result<std::filesystem::path, RecentProjectError>;

// The platform's per-user settings file. Windows uses LOCALAPPDATA, Unix uses
// XDG_CONFIG_HOME (or HOME/.config). Failure is a normal, nonfatal condition
// for startup callers: the editor simply has no automatic reopen feature for
// that run.
[[nodiscard]] RecentProjectSettingsPathResult
default_recent_project_settings_file();

// Missing settings are success with no path. Existing settings are accepted
// only after their magic, version, exact length, checksums, UTF-8 path, and
// absolute-path invariant have been validated.
[[nodiscard]] RecentProjectPathLoadResult load_recent_project_path(
    const std::filesystem::path& settings_file);

// Stores an absolute, lexically-normal UTF-8 project path. The new setting is
// written to an exclusive sibling temporary file, flushed, and atomically
// installed; a failure before replacement preserves the old setting.
[[nodiscard]] RecentProjectPathSaveResult save_recent_project_path(
    const std::filesystem::path& settings_file,
    const std::filesystem::path& project_file);

struct RecentProject final {
    std::filesystem::path path;
    doc::Root project;
};

struct RecentProjectOpenError final {
    std::optional<RecentProjectError> settings_error;
    std::optional<ProjectFileError> project_error;

    bool operator==(const RecentProjectOpenError&) const = default;
};

using RecentProjectOpenResult =
    core::Result<std::optional<RecentProject>, RecentProjectOpenError>;

// Startup probe: missing settings yield no candidate; corrupt/unreadable
// settings or projects return a diagnostic while leaving all live state
// untouched. Only a fully validated project is returned for staging.
[[nodiscard]] RecentProjectOpenResult open_recent_project(
    const std::filesystem::path& settings_file,
    const wire::DecodeLimits& limits = {});

} // namespace mediaserver::persist
