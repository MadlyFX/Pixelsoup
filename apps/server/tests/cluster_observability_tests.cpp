#include "cluster_observability.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

namespace app = mediaserver::app;
namespace core = mediaserver::core;
namespace net = mediaserver::net;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "line " << line << ": check failed: " << expression
                  << '\n';
        std::exit(1);
    }
}

#define OBS_CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class TempDirectory final {
public:
    TempDirectory() {
        static std::atomic_uint64_t sequence{};
        path_ = std::filesystem::temp_directory_path() /
            ("mediaserver-observability-" +
             std::to_string(sequence.fetch_add(1U)));
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

app::ClusterObservation healthy_observation() {
    app::ClusterObservation observation{};
    observation.configured_nodes = 2U;
    observation.live_sessions = 2U;
    observation.sessions.live_registrations = 2U;
    observation.barrier.live_sessions = 2U;
    return observation;
}

void health_separates_control_faults_from_hardware_timing() {
    app::ClusterObservation observation = healthy_observation();
    OBS_CHECK(app::cluster_control_health(observation) ==
              app::ClusterControlHealth::Healthy);
    OBS_CHECK(!observation.external_time_sync_verified);
    OBS_CHECK(!observation.hardware_frame_lock_verified);

    observation.live_sessions = 1U;
    OBS_CHECK(app::cluster_control_health(observation) ==
              app::ClusterControlHealth::Degraded);
    observation.live_sessions = 2U;
    // Historical incident counters remain in diagnostics, but current health
    // recovers once membership and frame progress recover.
    observation.sessions.heartbeat_timeouts = 1U;
    OBS_CHECK(app::cluster_control_health(observation) ==
              app::ClusterControlHealth::Healthy);
    observation.barrier.last_error =
        net::DirectorFrameBarrierObserverError::BarrierFaulted;
    OBS_CHECK(app::cluster_control_health(observation) ==
              app::ClusterControlHealth::Faulted);

    observation.configured_nodes = 0U;
    observation.live_sessions = 0U;
    OBS_CHECK(app::cluster_control_health(observation) ==
              app::ClusterControlHealth::Unmonitored);
    observation.barrier.last_error =
        net::DirectorFrameBarrierObserverError::None;
    observation.live_sessions = 1U;
    OBS_CHECK(app::cluster_control_health(observation) ==
              app::ClusterControlHealth::Degraded);
}

void diagnostics_are_machine_readable_and_append_only() {
    TempDirectory temporary;
    const auto path = app::cluster_diagnostics_path(
        temporary.path() / "director.wal");
    app::ClusterDiagnosticLog log{path};
    OBS_CHECK(log.available());

    app::ClusterObservation observation = healthy_observation();
    observation.health = app::cluster_control_health(observation);
    observation.unix_time_ms = 123U;
    observation.control_frame = 45U;
    observation.show_frame = 44U;
    observation.diagnostics_recording = true;
    OBS_CHECK(log.append(observation));
    OBS_CHECK(log.append(observation));

    std::ifstream input{path, std::ios::binary};
    const std::string contents{std::istreambuf_iterator<char>{input}, {}};
    OBS_CHECK(contents.find("\"schema\":1") != std::string::npos);
    OBS_CHECK(contents.find("\"health\":\"healthy\"") !=
              std::string::npos);
    OBS_CHECK(contents.find("\"control_frame\":45") != std::string::npos);
    OBS_CHECK(std::count(contents.begin(), contents.end(), '\n') == 2);
}

void oversized_diagnostics_rotate_one_previous_file() {
    TempDirectory temporary;
    const auto path = temporary.path() / "director.wal.cluster.jsonl";
    {
        std::ofstream output{path, std::ios::binary};
        output << "old";
    }
    std::filesystem::resize_file(path, app::ClusterDiagnosticLog::max_bytes);
    app::ClusterDiagnosticLog log{path};
    OBS_CHECK(log.available());
    OBS_CHECK(std::filesystem::exists(path.string() + ".previous"));
    app::ClusterObservation observation = healthy_observation();
    observation.health = app::cluster_control_health(observation);
    OBS_CHECK(log.append(observation));
    OBS_CHECK(std::filesystem::file_size(path) <
              app::ClusterDiagnosticLog::max_bytes);
}

// A post-show reader must be able to tell REFERENCE LOST from SWAP BARRIER
// INCOMPLETE without re-deriving either, and a healthy control plane must
// never imply that physical timing was verified.
void diagnostics_record_the_exact_sync_verdict() {
    app::ClusterObservation observation = healthy_observation();
    observation.health = app::cluster_control_health(observation);
    OBS_CHECK(observation.health == app::ClusterControlHealth::Healthy);
    const std::string unsynced = app::cluster_observation_json(observation);
    OBS_CHECK(unsynced.find("\"sync_required\":false") != std::string::npos);
    OBS_CHECK(unsynced.find("\"sync_satisfied\":true") != std::string::npos);
    OBS_CHECK(unsynced.find("\"sync_mode\":\"unsynced\"") !=
              std::string::npos);
    OBS_CHECK(unsynced.find("\"hardware_frame_lock_verified\":false") !=
              std::string::npos);

    observation.sync_required = true;
    observation.sync_satisfied = false;
    observation.sync_mode = "EXTERNAL GENLOCK";
    observation.sync_state = "REFERENCE LOST";
    observation.program_state = "ASSETS NOT ADOPTED";
    observation.sync_groups = 2U;
    observation.sync_expected_outputs = 4U;
    observation.sync_locked_outputs = 1U;
    observation.sync_barrier_joined_outputs = 0U;
    observation.sync_configuration_issues = 0U;
    observation.sync_missed_frames = 3U;
    observation.sync_late_frames = 5U;
    observation.sync_held_frames = 7U;
    observation.sync_repeated_frames = 11U;
    observation.sync_loss_events = 2U;
    const std::string lost = app::cluster_observation_json(observation);
    OBS_CHECK(lost.find("\"sync_state\":\"REFERENCE LOST\"") !=
              std::string::npos);
    // The physical verdict and the first unmet Program condition are recorded
    // separately, so a post-show reader can tell a raster problem from a hold
    // that had nothing to do with timing.
    OBS_CHECK(lost.find("\"program_state\":\"ASSETS NOT ADOPTED\"") !=
              std::string::npos);
    OBS_CHECK(lost.find("\"sync_mode\":\"EXTERNAL GENLOCK\"") !=
              std::string::npos);
    OBS_CHECK(lost.find("\"sync_required\":true") != std::string::npos);
    OBS_CHECK(lost.find("\"sync_satisfied\":false") != std::string::npos);
    OBS_CHECK(lost.find("\"sync_expected_outputs\":4") != std::string::npos);
    OBS_CHECK(lost.find("\"sync_locked_outputs\":1") != std::string::npos);
    OBS_CHECK(lost.find("\"sync_held_frames\":7") != std::string::npos);
    OBS_CHECK(lost.find("\"sync_loss_events\":2") != std::string::npos);
    // Control health is unchanged by any of it: the two remain separate facts.
    OBS_CHECK(app::cluster_control_health(observation) ==
              app::ClusterControlHealth::Healthy);

    // The stream is JSON Lines, so a label may never break the record.
    observation.sync_state = "BAD \"QUOTE\"\nAND NEWLINE";
    const std::string escaped = app::cluster_observation_json(observation);
    OBS_CHECK(escaped.find('\n') == std::string::npos);
    OBS_CHECK(escaped.find("BAD \\\"QUOTE\\\" AND NEWLINE") !=
              std::string::npos);
}

void active_diagnostics_rotate_before_crossing_the_limit() {
    TempDirectory temporary;
    const auto path = temporary.path() / "director.wal.cluster.jsonl";
    {
        std::ofstream output{path, std::ios::binary};
        output << "old";
    }
    std::filesystem::resize_file(
        path, app::ClusterDiagnosticLog::max_bytes - 8U);
    app::ClusterDiagnosticLog log{path};
    OBS_CHECK(log.available());
    app::ClusterObservation observation = healthy_observation();
    observation.health = app::cluster_control_health(observation);
    OBS_CHECK(log.append(observation));
    OBS_CHECK(std::filesystem::exists(path.string() + ".previous"));
    OBS_CHECK(std::filesystem::file_size(path) <
              app::ClusterDiagnosticLog::max_bytes);
}

} // namespace

int main() {
    health_separates_control_faults_from_hardware_timing();
    diagnostics_are_machine_readable_and_append_only();
    oversized_diagnostics_rotate_one_previous_file();
    active_diagnostics_rotate_before_crossing_the_limit();
    diagnostics_record_the_exact_sync_verdict();
    std::cout << "5/5 cluster observability tests passed\n";
    return 0;
}
