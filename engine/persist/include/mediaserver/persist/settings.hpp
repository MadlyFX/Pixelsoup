#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

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

    bool operator==(const SettingsError&) const = default;
};

using SettingsPathResult =
    core::Result<std::filesystem::path, SettingsError>;
using RecentProjectPathLoadResult = core::Result<
    std::optional<std::filesystem::path>, SettingsError>;
using RecentProjectPathSaveResult =
    core::Result<std::filesystem::path, SettingsError>;

// The platform's per-user settings file. Windows uses LOCALAPPDATA, Unix uses
// XDG_CONFIG_HOME (or HOME/.config). Failure is a normal, nonfatal condition
// for startup callers: the editor simply has no automatic reopen feature for
// that run.
[[nodiscard]] SettingsPathResult
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
    std::optional<SettingsError> settings_error;
    std::optional<ProjectFileError> project_error;

    bool operator==(const RecentProjectOpenError&) const = default;
};

using RecentProjectOpenResult =
    core::Result<std::optional<RecentProject>, RecentProjectOpenError>;

inline constexpr std::uint16_t current_preferences_format_version = 1U;
inline constexpr std::size_t max_preference_value_bytes = 4U * 1024U;

// The machine-local settings an operator would otherwise have to pass on the
// command line every launch. Every field is optional: an empty value means
// "no preference", and the caller keeps its built-in default.
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

    [[nodiscard]] bool empty() const noexcept {
        return director_listen.empty() && show_id.empty() &&
            director_epoch.empty() && lifecycle_secret_file.empty() &&
            wal_path.empty();
    }

    bool operator==(const Preferences&) const = default;
};

using PreferencesLoadResult = core::Result<Preferences, SettingsError>;
using PreferencesSaveResult = core::Result<std::filesystem::path,
                                           SettingsError>;

// Beside the recent-project file, in the same per-user settings directory.
[[nodiscard]] SettingsPathResult default_preferences_file();

// A missing file is success with every field empty, because having no stored
// preference is the ordinary first-run state. A corrupt one is reported so the
// operator learns their settings were not applied, rather than silently
// running on defaults they did not choose.
[[nodiscard]] PreferencesLoadResult load_preferences(
    const std::filesystem::path& preferences_file);

// Written to an exclusive sibling temporary file, flushed, and atomically
// installed, so an interrupted save leaves the previous preferences intact.
[[nodiscard]] PreferencesSaveResult save_preferences(
    const std::filesystem::path& preferences_file,
    const Preferences& preferences);

// Startup probe: missing settings yield no candidate; corrupt/unreadable
// settings or projects return a diagnostic while leaving all live state
// untouched. Only a fully validated project is returned for staging.
[[nodiscard]] RecentProjectOpenResult open_recent_project(
    const std::filesystem::path& settings_file,
    const wire::DecodeLimits& limits = {});

} // namespace mediaserver::persist
