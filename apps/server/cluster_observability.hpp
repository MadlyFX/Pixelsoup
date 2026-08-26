#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/net/director_frame_barrier.hpp>
#include <mediaserver/net/director_session_service.hpp>

#include <filesystem>
#include <fstream>
#include <cstdint>
#include <string>

namespace mediaserver::app {

enum class ClusterControlHealth : core::u8 {
    Unmonitored,
    Healthy,
    Degraded,
    Faulted,
};

// One coherent operator/post-show observation. Control-plane convergence and
// physical presentation timing are deliberately separate: this application
// currently proves the former and makes no PTP/genlock/frame-lock claim.
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
    net::DirectorSessionServiceCounters sessions{};
    net::DirectorFrameBarrierObserverDiagnostics barrier{};
    bool diagnostics_recording{};
    bool external_time_sync_verified{};
    bool hardware_frame_lock_verified{};

    bool operator==(const ClusterObservation&) const = default;
};

[[nodiscard]] ClusterControlHealth
cluster_control_health(const ClusterObservation& observation) noexcept;

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
