#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/net/director_frame_barrier.hpp>
#include <mediaserver/net/director_session_service.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdint>
#include <optional>
#include <string>

namespace mediaserver::app {

enum class ClusterControlHealth : core::u8 {
    Unmonitored,
    Healthy,
    Degraded,
    Faulted,
};

// One coherent operator/post-show observation.
//
// Control-plane convergence and physical presentation timing remain separate
// facts, and both are recorded. The control fields say whether every node is
// working on the same show state; the sync fields say what the cluster could
// actually prove about the outputs presenting it on the same raster boundary.
// A healthy control plane never sets the sync fields.
struct ClusterObservation final {
    ClusterControlHealth health{ClusterControlHealth::Unmonitored};
    core::u64 unix_time_ms{};
    core::u64 control_frame{};
    core::u64 show_frame{};
    core::u32 configured_nodes{};
    core::u32 live_sessions{};
    core::u32 catching_up_sessions{};
    core::u64 release_lag_frames{};
    core::u64 pacer_lag_frames{};
    core::u64 open_barriers{};
    core::u64 present_failures{};
    // The Director has sealed no control frame for longer than ordinary
    // backpressure explains, so operator edits are not being applied.
    bool control_stalled{};
    net::DirectorSessionServiceCounters sessions{};
    net::DirectorFrameBarrierObserverDiagnostics barrier{};
    bool diagnostics_recording{};
    bool external_time_sync_verified{};
    bool hardware_frame_lock_verified{};
    // The authored policy and the exact readiness verdict, recorded verbatim
    // so a post-show reader can tell REFERENCE LOST from SWAP BARRIER
    // INCOMPLETE without re-deriving either.
    std::string sync_mode{"unsynced"};
    // The physical-timing verdict, and the first unmet Program condition
    // overall. They differ whenever Program is held for a reason that is not
    // about raster boundaries.
    std::string sync_state{"unsynced"};
    std::string program_state{"unsynced"};
    bool sync_required{};
    bool sync_satisfied{true};
    bool sync_degraded{};
    bool sync_simulated{};
    core::u32 sync_groups{};
    core::u32 sync_expected_outputs{};
    core::u32 sync_locked_outputs{};
    core::u32 sync_barrier_joined_outputs{};
    core::u32 sync_configuration_issues{};
    core::u64 sync_missed_frames{};
    core::u64 sync_late_frames{};
    core::u64 sync_held_frames{};
    core::u64 sync_repeated_frames{};
    core::u64 sync_loss_events{};

    bool operator==(const ClusterObservation&) const = default;
};

[[nodiscard]] ClusterControlHealth
cluster_control_health(const ClusterObservation& observation) noexcept;

enum class ControlBacklogEvent : core::u8 {
    None,
    Stalled,
    Recovered,
};

// The Director seals no control frame while its unretired-frame backlog is
// full, and operator edits are applied only to a sealed frame. Full for a
// moment is ordinary backpressure - a native file dialog leaves pacing debt
// that the cohort then drains - so only a backlog that stays full past
// `stall_after` is reported. Unreported, a stall looks like every edit
// snapping back for no reason.
class ControlBacklogWatch final {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds stall_after{1000};

    // Each transition is returned once: Stalled when a full backlog first
    // outlasts `stall_after`, Recovered when a stalled backlog drains.
    [[nodiscard]] ControlBacklogEvent observe(bool full,
                                              Clock::time_point now) noexcept;
    [[nodiscard]] bool stalled() const noexcept { return stalled_; }

private:
    std::optional<Clock::time_point> full_since_;
    bool stalled_{};
};

[[nodiscard]] std::string
cluster_observation_json(const ClusterObservation& observation);

[[nodiscard]] std::filesystem::path cluster_diagnostics_path(
    const std::filesystem::path& wal_path);

// Append-only during a run, bounded across runs. At 32 MiB the previous file
// is retained as `.previous` and a fresh JSONL stream begins. Failure to open
// or rotate never interrupts Program; availability is surfaced to the UI.
class ClusterDiagnosticLog final {
public:
    static constexpr std::uintmax_t max_bytes{32U * 1024U * 1024U};

    explicit ClusterDiagnosticLog(std::filesystem::path path) noexcept;
    ClusterDiagnosticLog(const ClusterDiagnosticLog&) = delete;
    ClusterDiagnosticLog& operator=(const ClusterDiagnosticLog&) = delete;
    ClusterDiagnosticLog(ClusterDiagnosticLog&&) = delete;
    ClusterDiagnosticLog& operator=(ClusterDiagnosticLog&&) = delete;
    ~ClusterDiagnosticLog() = default;

    [[nodiscard]] bool available() const noexcept { return stream_.is_open(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }
    [[nodiscard]] bool append(const ClusterObservation& observation) noexcept;

private:
    [[nodiscard]] bool rotate() noexcept;

    std::filesystem::path path_;
    std::ofstream stream_;
    std::uintmax_t bytes_{};
};

} // namespace mediaserver::app
