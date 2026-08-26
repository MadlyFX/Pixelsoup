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
    std::cout << "4/4 cluster observability tests passed\n";
    return 0;
}
