#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <mediaserver/core/result.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/wire/protocol_codec.hpp>

// Per-user, per-machine settings: the recent project, and the operator's
// cluster/control preferences.
//
// None of this is a document node and none of it ever lives beside a project:
// it describes this installation, not the show, so it is never saved into a
// .mstage and never replicated to a render node. A stale or damaged setting is
// recoverable startup metadata and must never prevent the editor from opening
// its ordinary untitled project.
namespace mediaserver::persist {

inline constexpr std::uint16_t current_recent_project_format_version = 1U;
inline constexpr std::size_t max_recent_project_path_bytes = 32U * 1024U;

enum class SettingsErrorCode {
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

struct SettingsError final {
    SettingsErrorCode code{SettingsErrorCode::InvalidFormat};
    std::error_code system_error;
    // As with project files, a post-replacement directory-sync failure means
    // the new setting is installed although power-loss durability is unknown.
    bool committed{};

    bool operator==(const SettingsError &) const = default;
};

using SettingsPathResult = core::Result<std::filesystem::path, SettingsError>;
using RecentProjectPathLoadResult =
    core::Result<std::optional<std::filesystem::path>, SettingsError>;
using RecentProjectPathSaveResult =
    core::Result<std::filesystem::path, SettingsError>;

// The platform's per-user settings file. Windows uses LOCALAPPDATA, Unix uses
// XDG_CONFIG_HOME (or HOME/.config). Failure is a normal, nonfatal condition
// for startup callers: the editor simply has no automatic reopen feature for
// that run.
[[nodiscard]] SettingsPathResult default_recent_project_settings_file();

// Missing settings are success with no path. Existing settings are accepted
// only after their magic, version, exact length, checksums, UTF-8 path, and
// absolute-path invariant have been validated.
[[nodiscard]] RecentProjectPathLoadResult
load_recent_project_path(const std::filesystem::path &settings_file);

// Stores an absolute, lexically-normal UTF-8 project path. The new setting is
// written to an exclusive sibling temporary file, flushed, and atomically
// installed; a failure before replacement preserves the old setting.
[[nodiscard]] RecentProjectPathSaveResult
save_recent_project_path(const std::filesystem::path &settings_file,
                         const std::filesystem::path &project_file);

struct RecentProject final {
    std::filesystem::path path;
    doc::Root project;
};

struct RecentProjectOpenError final {
    std::optional<SettingsError> settings_error;
    std::optional<ProjectFileError> project_error;

    bool operator==(const RecentProjectOpenError &) const = default;
};

using RecentProjectOpenResult =
    core::Result<std::optional<RecentProject>, RecentProjectOpenError>;

inline constexpr std::uint16_t current_preferences_format_version = 1U;
inline constexpr std::size_t max_preference_value_bytes = 4U * 1024U;

// Playback tuning is machine-local: output nodes may have substantially more
// memory and tolerate more latency than the operator workstation. These
// bounds keep a damaged or accidentally extreme setting from turning startup
// into an unbounded allocation or queue request.
inline constexpr std::uint32_t default_output_buffer_frames = 12U;
inline constexpr std::uint32_t maximum_output_buffer_frames = 60U;
inline constexpr std::uint32_t default_output_read_ahead_frames = 12U;
inline constexpr std::uint32_t maximum_output_read_ahead_frames = 60U;
inline constexpr std::uint32_t default_playback_cache_mebibytes = 2'048U;
inline constexpr std::uint32_t minimum_playback_cache_mebibytes = 256U;
inline constexpr std::uint32_t maximum_playback_cache_mebibytes = 32'768U;
inline constexpr std::uint32_t default_playback_trail_frames = 0U;
inline constexpr std::uint32_t maximum_playback_trail_frames = 16U;
inline constexpr std::uint32_t default_proxy_cache_gibibytes = 8U;
inline constexpr std::uint32_t minimum_proxy_cache_gibibytes = 1U;
inline constexpr std::uint32_t maximum_proxy_cache_gibibytes = 1'024U;

enum class ProxyTranscodePriority : std::uint8_t {
    Background = 0U,
    Balanced = 1U,
    Fast = 2U,
};

[[nodiscard]] constexpr bool
valid_proxy_transcode_priority(ProxyTranscodePriority priority) noexcept {
    return priority == ProxyTranscodePriority::Background ||
           priority == ProxyTranscodePriority::Balanced ||
           priority == ProxyTranscodePriority::Fast;
}

// The machine-local settings an operator would otherwise have to pass on the
// command line every launch. Text fields are optional; numeric fields carry
// the built-in values when no record has overridden them.
//
// These take effect when the process starts, so the editor tells the operator
// that a change applies at next launch rather than pretending otherwise. A
// command-line argument always wins over the stored value, which is what keeps
// scripted and demo launches exactly reproducible.
struct Preferences final {
    // "ADDRESS:PORT" the Director listens on for render nodes.
    std::string director_listen;
    // 32 hexadecimal characters each. A Director cannot listen without both,
    // so they belong to the same setting as the endpoint rather than being a
    // separate thing to remember on the command line.
    std::string show_id;
    std::string director_epoch;
    // This machine's 32-256 byte lifecycle credential file. Only the path is
    // stored here; the secret itself is never written to settings.
    std::string lifecycle_secret_file;
    // The Director write-ahead log.
    std::string wal_path;
    // Empty keeps the built-in per-user cache location. When set, this is the
    // absolute machine-local directory used for proxy artifacts.
    std::string proxy_cache_directory;
    // Store each proxy cache in a dedicated folder beside its source media.
    // This takes precedence over proxy_cache_directory.
    bool proxy_cache_next_to_source{};
    std::uint32_t proxy_cache_gibibytes{default_proxy_cache_gibibytes};
    std::uint32_t output_buffer_frames{default_output_buffer_frames};
    std::uint32_t output_read_ahead_frames{default_output_read_ahead_frames};
    std::uint32_t playback_cache_mebibytes{default_playback_cache_mebibytes};
    std::uint32_t playback_trail_frames{default_playback_trail_frames};
    ProxyTranscodePriority proxy_transcode_priority{
        ProxyTranscodePriority::Background};

    [[nodiscard]] bool valid_tuning() const noexcept {
        return proxy_cache_gibibytes >= minimum_proxy_cache_gibibytes &&
               proxy_cache_gibibytes <= maximum_proxy_cache_gibibytes &&
               output_buffer_frames <= maximum_output_buffer_frames &&
               output_read_ahead_frames >= 1U &&
               output_read_ahead_frames <= maximum_output_read_ahead_frames &&
               playback_cache_mebibytes >= minimum_playback_cache_mebibytes &&
               playback_cache_mebibytes <= maximum_playback_cache_mebibytes &&
               playback_trail_frames <= maximum_playback_trail_frames &&
               valid_proxy_transcode_priority(proxy_transcode_priority);
    }

    [[nodiscard]] bool empty() const noexcept {
        return director_listen.empty() && show_id.empty() &&
               director_epoch.empty() && lifecycle_secret_file.empty() &&
               wal_path.empty() && proxy_cache_directory.empty() &&
               !proxy_cache_next_to_source &&
               proxy_cache_gibibytes == default_proxy_cache_gibibytes &&
               output_buffer_frames == default_output_buffer_frames &&
               output_read_ahead_frames == default_output_read_ahead_frames &&
               playback_cache_mebibytes == default_playback_cache_mebibytes &&
               playback_trail_frames == default_playback_trail_frames &&
               proxy_transcode_priority == ProxyTranscodePriority::Background;
    }

    bool operator==(const Preferences &) const = default;
};

using PreferencesLoadResult = core::Result<Preferences, SettingsError>;
using PreferencesSaveResult =
    core::Result<std::filesystem::path, SettingsError>;

// Beside the recent-project file, in the same per-user settings directory.
[[nodiscard]] SettingsPathResult default_preferences_file();

// A missing file is success with every field empty, because having no stored
// preference is the ordinary first-run state. A corrupt one is reported so the
// operator learns their settings were not applied, rather than silently
// running on defaults they did not choose.
[[nodiscard]] PreferencesLoadResult
load_preferences(const std::filesystem::path &preferences_file);

// Written to an exclusive sibling temporary file, flushed, and atomically
// installed, so an interrupted save leaves the previous preferences intact.
[[nodiscard]] PreferencesSaveResult
save_preferences(const std::filesystem::path &preferences_file,
                 const Preferences &preferences);

// The editor owns the meaning and versioning of these bytes; persist only
// gives the machine-local workspace record the same bounded, checksummed,
// atomic storage guarantees as the other per-user settings.
inline constexpr std::uint16_t current_workspace_layout_format_version = 1U;
inline constexpr std::size_t max_workspace_layout_setting_bytes = 256U * 1024U;

using WorkspaceLayoutBytes = std::vector<std::byte>;
using WorkspaceLayoutLoadResult =
    core::Result<WorkspaceLayoutBytes, SettingsError>;
using WorkspaceLayoutSaveResult =
    core::Result<std::filesystem::path, SettingsError>;

[[nodiscard]] SettingsPathResult default_workspace_layout_file();

// A missing file is the ordinary first-run state and yields an empty byte
// vector. Existing records are returned only after their envelope, size, and
// checksums validate; the editor then validates the inner dock schema.
[[nodiscard]] WorkspaceLayoutLoadResult
load_workspace_layout(const std::filesystem::path &layout_file);

[[nodiscard]] WorkspaceLayoutSaveResult
save_workspace_layout(const std::filesystem::path &layout_file,
                      std::span<const std::byte> layout);

// Startup probe: missing settings yield no candidate; corrupt/unreadable
// settings or projects return a diagnostic while leaving all live state
// untouched. Only a fully validated project is returned for staging.
[[nodiscard]] RecentProjectOpenResult
open_recent_project(const std::filesystem::path &settings_file,
                    const wire::DecodeLimits &limits = {});

} // namespace mediaserver::persist
