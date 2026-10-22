#include "cluster_observability.hpp"

#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace mediaserver::app {
namespace {

[[nodiscard]] const char* health_name(ClusterControlHealth health) noexcept {
    switch (health) {
    case ClusterControlHealth::Unmonitored:
        return "unmonitored";
    case ClusterControlHealth::Healthy:
        return "healthy";
    case ClusterControlHealth::Degraded:
        return "degraded";
    case ClusterControlHealth::Faulted:
        return "faulted";
    }
    return "faulted";
}

// The diagnostic stream is JSON Lines, so any control character or quote in a
// recorded label would corrupt the whole file. These labels are produced by
// this application, not by a peer, but escaping them keeps the format valid
// even if a future label gains punctuation.
[[nodiscard]] std::string json_text(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 8U);
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '"' || character == '\\') {
            escaped += '\\';
            escaped += character;
        } else if (byte < 0x20U) {
            escaped += ' ';
        } else {
            escaped += character;
        }
    }
    return escaped;
}

} // namespace

ClusterControlHealth
cluster_control_health(const ClusterObservation& observation) noexcept {
    // Checked before the node count: a stalled Director is faulted whether
    // or not any render node is configured.
    if (observation.control_stalled) {
        return ClusterControlHealth::Faulted;
    }
    if (observation.configured_nodes == 0U &&
        observation.live_sessions == 0U &&
        observation.catching_up_sessions == 0U) {
        return ClusterControlHealth::Unmonitored;
    }
    const auto& barrier = observation.barrier;
    if (barrier.last_error ==
            net::DirectorFrameBarrierObserverError::BarrierFaulted) {
        return ClusterControlHealth::Faulted;
    }
    if (observation.live_sessions != observation.configured_nodes ||
        observation.catching_up_sessions != 0U ||
        observation.release_lag_frames > 1U ||
        observation.pacer_lag_frames > 1U ||
        observation.open_barriers > 4U) {
        return ClusterControlHealth::Degraded;
    }
    return ClusterControlHealth::Healthy;
}

ControlBacklogEvent ControlBacklogWatch::observe(
    bool full,
    Clock::time_point now) noexcept {
    if (!full) {
        full_since_.reset();
        if (stalled_) {
            stalled_ = false;
            return ControlBacklogEvent::Recovered;
        }
        return ControlBacklogEvent::None;
    }
    if (!full_since_) {
        full_since_ = now;
    }
    if (!stalled_ && now - *full_since_ >= stall_after) {
        stalled_ = true;
        return ControlBacklogEvent::Stalled;
    }
    return ControlBacklogEvent::None;
}

std::string cluster_observation_json(const ClusterObservation& observation) {
    const auto& service = observation.sessions;
    const auto& barrier = observation.barrier;
    std::string json;
    json.reserve(768U);
    const auto field = [&](const char* name, core::u64 value) {
        json += ",\"";
        json += name;
        json += "\":";
        json += std::to_string(value);
    };
    json = "{\"schema\":1,\"health\":\"";
    json += health_name(observation.health);
    json += '"';
    field("time_unix_ms", observation.unix_time_ms);
    field("control_frame", observation.control_frame);
    field("show_frame", observation.show_frame);
    field("configured_nodes", observation.configured_nodes);
    field("live_sessions", observation.live_sessions);
    field("catching_up_sessions", observation.catching_up_sessions);
    field("release_lag_frames", observation.release_lag_frames);
    field("pacer_lag_frames", observation.pacer_lag_frames);
    field("open_barriers", observation.open_barriers);
    field("present_failures", observation.present_failures);
    field("accepted_connections", service.accepted_connections);
    field("capacity_rejections", service.capacity_rejections);
    field("session_id_exhaustions", service.session_id_exhaustions);
    field("worker_start_failures", service.worker_start_failures);
    field("subscription_failures", service.subscription_failures);
    field("resume_failures", service.resume_failures);
    field("live_registrations", service.live_registrations);
    field("observer_rejections", service.observer_rejections);
    field("validated_acknowledgements", service.validated_acknowledgements);
    field("heartbeat_acknowledgements",
          service.validated_heartbeat_acknowledgements);
    field("heartbeat_timeouts", service.heartbeat_timeouts);
    field("graceful_goodbyes", service.graceful_goodbyes);
    field("resync_disconnects", service.resync_disconnects);
    field("hub_closed_disconnects", service.hub_closed_disconnects);
    field("session_failures", service.session_failures);
    field("barrier_rejected_registrations", barrier.rejected_registrations);
    field("barrier_rejected_acknowledgements",
          barrier.rejected_acknowledgements);
    field("barrier_ignored_unregistrations",
          barrier.ignored_unregistrations);
    field("barrier_last_error",
          static_cast<core::u64>(barrier.last_error));
    field("lockstep_demotions", barrier.lockstep_demotions);
    json += ",\"control_stalled\":";
    json += observation.control_stalled ? "true" : "false";
    json += ",\"diagnostics_recording\":";
    json += observation.diagnostics_recording ? "true" : "false";
    json += ",\"external_time_sync_verified\":";
    json += observation.external_time_sync_verified ? "true" : "false";
    json += ",\"hardware_frame_lock_verified\":";
    json += observation.hardware_frame_lock_verified ? "true" : "false";
    json += ",\"sync_required\":";
    json += observation.sync_required ? "true" : "false";
    json += ",\"sync_satisfied\":";
    json += observation.sync_satisfied ? "true" : "false";
    json += ",\"sync_degraded\":";
    json += observation.sync_degraded ? "true" : "false";
    json += ",\"sync_simulated\":";
    json += observation.sync_simulated ? "true" : "false";
    json += ",\"sync_mode\":\"";
    json += json_text(observation.sync_mode);
    json += "\",\"sync_state\":\"";
    json += json_text(observation.sync_state);
    json += "\",\"program_state\":\"";
    json += json_text(observation.program_state);
    json += '"';
    field("sync_groups", observation.sync_groups);
    field("sync_expected_outputs", observation.sync_expected_outputs);
    field("sync_locked_outputs", observation.sync_locked_outputs);
    field("sync_barrier_joined_outputs",
          observation.sync_barrier_joined_outputs);
    field("sync_configuration_issues",
          observation.sync_configuration_issues);
    field("sync_missed_frames", observation.sync_missed_frames);
    field("sync_late_frames", observation.sync_late_frames);
    field("sync_held_frames", observation.sync_held_frames);
    field("sync_repeated_frames", observation.sync_repeated_frames);
    field("sync_loss_events", observation.sync_loss_events);
    json += '}';
    return json;
}

std::filesystem::path cluster_diagnostics_path(
    const std::filesystem::path& wal_path) {
    std::filesystem::path result = wal_path;
    result += ".cluster.jsonl";
    return result;
}

ClusterDiagnosticLog::ClusterDiagnosticLog(std::filesystem::path path) noexcept
    : path_(std::move(path)) {
    try {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(path_, error);
        if (error && error != std::errc::no_such_file_or_directory) {
            return;
        }
        if (!error && status.type() != std::filesystem::file_type::not_found) {
            if (!std::filesystem::is_regular_file(status) ||
                std::filesystem::is_symlink(status)) {
                return;
            }
            const auto size = std::filesystem::file_size(path_, error);
            if (error) {
                return;
            }
            if (size >= max_bytes) {
                std::filesystem::path previous = path_;
                previous += ".previous";
                const auto previous_status =
                    std::filesystem::symlink_status(previous, error);
                if (!error && previous_status.type() !=
                                  std::filesystem::file_type::not_found) {
                    if (!std::filesystem::is_regular_file(previous_status) ||
                        std::filesystem::is_symlink(previous_status)) {
                        return;
                    }
                    if (!std::filesystem::remove(previous, error) || error) {
                        return;
                    }
                } else if (error &&
                           error != std::errc::no_such_file_or_directory) {
                    return;
                }
                error.clear();
                std::filesystem::rename(path_, previous, error);
                if (error) {
                    return;
                }
            } else {
                bytes_ = size;
            }
        }
        stream_.open(path_, std::ios::binary | std::ios::app);
    } catch (...) {
        stream_.close();
    }
}

bool ClusterDiagnosticLog::rotate() noexcept {
    try {
        stream_.close();
        std::error_code error;
        std::filesystem::path previous = path_;
        previous += ".previous";
        const auto previous_status =
            std::filesystem::symlink_status(previous, error);
        if (!error && previous_status.type() !=
                          std::filesystem::file_type::not_found) {
            if (!std::filesystem::is_regular_file(previous_status) ||
                std::filesystem::is_symlink(previous_status) ||
                !std::filesystem::remove(previous, error) || error) {
                return false;
            }
        } else if (error &&
                   error != std::errc::no_such_file_or_directory) {
            return false;
        }
        error.clear();
        std::filesystem::rename(path_, previous, error);
        if (error) {
            return false;
        }
        stream_.clear();
        stream_.open(path_, std::ios::binary | std::ios::trunc);
        if (!stream_.is_open()) {
            return false;
        }
        bytes_ = 0U;
        return true;
    } catch (...) {
        stream_.close();
        return false;
    }
}

bool ClusterDiagnosticLog::append(
    const ClusterObservation& observation) noexcept {
    if (!stream_.is_open()) {
        return false;
    }
    try {
        const std::string line = cluster_observation_json(observation);
        const std::uintmax_t line_bytes =
            static_cast<std::uintmax_t>(line.size()) + 1U;
        if (line_bytes > max_bytes) {
            stream_.close();
            return false;
        }
        if (bytes_ > max_bytes - line_bytes && !rotate()) {
            return false;
        }
        stream_ << line << '\n';
        stream_.flush();
        if (!stream_) {
            stream_.close();
            return false;
        }
        bytes_ += line_bytes;
        return true;
    } catch (...) {
        stream_.close();
        return false;
    }
}

} // namespace mediaserver::app
