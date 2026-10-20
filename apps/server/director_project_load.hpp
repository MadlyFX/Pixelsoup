#pragma once

// Project-file stages of the Director: recent paths, saved-baseline
// comparison, media-rate enrichment and staging a loaded project.

#include <mediaserver/cmd/command.hpp>
#include <mediaserver/cmd/director_scheduler.hpp>
#include <mediaserver/doc/document.hpp>
#include <mediaserver/persist/command_log.hpp>
#include <mediaserver/persist/durable_director.hpp>
#include <mediaserver/persist/project_file.hpp>
#include <mediaserver/persist/settings.hpp>
#include <mediaserver/show/transport_authority.hpp>
#include <mediaserver/show/transport_state.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace mediaserver::app {

[[nodiscard]] std::filesystem::path
native_path_from_utf8(std::string_view text);

[[nodiscard]] std::string
native_path_to_utf8(const std::filesystem::path &path);

[[nodiscard]] bool project_content_changed(const doc::Root &saved,
                                           const doc::Root &current);

[[nodiscard]] bool
recovered_project_matches_saved_baseline(const doc::Root &saved,
                                         const doc::Root &recovered);

[[nodiscard]] std::string project_file_failure(std::string_view operation,
                                               unsigned code);

[[nodiscard]] std::string_view
project_file_error_label(persist::ProjectFileErrorCode code) noexcept;

[[nodiscard]] std::string
project_file_failure(std::string_view operation,
                     const persist::ProjectFileError &error);

struct MediaRateEnrichment final {
    doc::Root project;
    std::size_t updated_nodes{};
};

[[nodiscard]] MediaRateEnrichment
enrich_media_frame_rates(const doc::Root &project);

[[nodiscard]] bool
restore_project_transport(show::TransportAuthority &transport,
                          const show::TransportAnchor &loaded,
                          cmd::FrameNumber at) noexcept;

[[nodiscard]] bool normalize_project_switcher(doc::Root &project,
                                              cmd::FrameNumber at);

enum class ProjectLoadStageErrorCode {
    ScheduleFailed,
    PrepareFailed,
    WirePreflightFailed,
    WalPreflightFailed,
    AbortFailed,
    AllocationFailed,
};

struct ProjectLoadStageError final {
    ProjectLoadStageErrorCode code{};
    std::optional<cmd::ScheduleError> schedule_error;
    std::optional<cmd::SealError> seal_error;
    std::optional<persist::CommandLogError> command_log_error;
};

using ProjectLoadStageResult =
    core::Result<cmd::DirectorScheduler, ProjectLoadStageError>;

[[nodiscard]] bool
command_log_limit_error(const persist::CommandLogError &error) noexcept;

[[nodiscard]] ProjectLoadStageResult
stage_project_load(const cmd::DirectorScheduler &current,
                   const doc::Root &replacement, cmd::FrameNumber frame,
                   persist::CommandLog &command_log,
                   persist::FrameDurability frame_durability);


// Reports a recent-project settings failure on stderr.
void report_recent_project_error(std::string_view operation,
                                 const persist::SettingsError &error);

} // namespace mediaserver::app
