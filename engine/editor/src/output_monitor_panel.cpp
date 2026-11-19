#include <cctype>
#include <mediaserver/editor/output_monitor_panel.hpp>

#include <mediaserver/editor/mapping_panels.hpp>
#include <mediaserver/gui/shapes.hpp>

#include "workspace_panel_helpers.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace mediaserver::editor {

using namespace panel_helpers;

namespace {

[[nodiscard]] std::string resolution_text(core::u32 width, core::u32 height) {
  std::array<char, 32U> output{};
  const int length =
      std::snprintf(output.data(), output.size(), "%u x %u", width, height);
  return std::string{output.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

// GPU memory in decimal gigabytes with one decimal, as operators read a card.
[[nodiscard]] std::string gpu_memory_text(core::u64 bytes) {
  std::array<char, 32U> output{};
  const int length = std::snprintf(output.data(), output.size(), "%.1f GB",
                                   static_cast<double>(bytes) / 1.0e9);
  return std::string{output.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

[[nodiscard]] std::string fps_text(float fps) {
  std::array<char, 16U> output{};
  const int length = std::snprintf(output.data(), output.size(), "%.0f",
                                   static_cast<double>(fps));
  return std::string{output.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

// Lines worth the operator's eye in a log: failures, faults and lockstep
// trouble. Colour is never the only signal - the words are right there.
[[nodiscard]] bool log_line_is_trouble(std::string_view line) noexcept {
  for (const std::string_view word :
       {std::string_view{"error"}, std::string_view{"ERROR"},
        std::string_view{"failed"}, std::string_view{"FAILED"},
        std::string_view{"fault"}, std::string_view{"FELL BEHIND"},
        std::string_view{"dropped"}}) {
    if (line.find(word) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] std::string byte_count_text(core::u64 bytes) {
  std::array<char, 32U> output{};
  const int length =
      bytes >= 1024U * 1024U
          ? std::snprintf(output.data(), output.size(), "%.1f MB",
                          static_cast<double>(bytes) / (1024.0 * 1024.0))
      : bytes >= 1024U
          ? std::snprintf(output.data(), output.size(), "%.1f KB",
                          static_cast<double>(bytes) / 1024.0)
          : std::snprintf(output.data(), output.size(), "%llu bytes",
                          static_cast<unsigned long long>(bytes));
  return std::string{output.data(),
                     static_cast<std::size_t>(std::max(length, 0))};
}

} // namespace

std::string output_node_log_summary(const OutputMonitorState::Node &node) {
  const OutputNodeLog *log = node.log.get();
  if (log == nullptr || log->status == OutputNodeLog::Status::Fetching) {
    return node.log_fetching || log != nullptr ? "Reading the log"
                                               : "Log not read yet";
  }
  switch (log->status) {
  case OutputNodeLog::Status::Fetching:
    break;
  case OutputNodeLog::Status::Available: {
    std::string text = "Read at " + log->fetched_at + " - " +
                       std::to_string(log->lines.size()) +
                       (log->lines.size() == 1U ? " line" : " lines");
    if (log->previous_run_lines > 0U) {
      text += ", " + std::to_string(log->previous_run_lines) +
              " from the previous run";
    }
    if (log->dropped_bytes > 0U) {
      text += ", " + byte_count_text(log->dropped_bytes) +
              " dropped by a slow disk";
    }
    return text;
  }
  case OutputNodeLog::Status::Unavailable:
    return log->detail.empty() ? std::string{"No log on this machine"}
                               : "No log - " + log->detail;
  case OutputNodeLog::Status::Failed:
    return log->detail.empty()
               ? std::string{"Could not read the log"}
               : "Could not read the log - " + log->detail;
  }
  return {};
}

std::string_view output_node_connection_label(
    const OutputMonitorState::Node &node) noexcept {
  using Connection = OutputMonitorState::Node::Connection;
  switch (node.connection) {
  case Connection::Live: return "LIVE";
  case Connection::CatchingUp: return "CATCHING UP";
  case Connection::Disconnected: return "CONNECTING";
  }
  return "CONNECTING";
}

std::string_view output_node_deployment_label(
    const OutputMonitorState::Node &node) noexcept {
  using OutputStatus = OutputMonitorState::Node::OutputStatus;
  switch (node.output) {
  case OutputStatus::Unknown:
  case OutputStatus::Deployed:
    return {};
  case OutputStatus::InventoryUnavailable:
    return "NO DISPLAY INVENTORY ON THAT MACHINE";
  case OutputStatus::InvalidMode:
    return "SAVED OUTPUT MODE IS OUT OF RANGE";
  case OutputStatus::DisplayMissing:
    return "BOUND DISPLAY IS NOT CONNECTED";
  case OutputStatus::DisplayInactive:
    return "BOUND DISPLAY IS CONNECTED BUT SWITCHED OFF";
  case OutputStatus::DisplayDuplicated:
    return "TWO DISPLAYS CLAIM THE SAME BINDING";
  case OutputStatus::DisplayMirrored:
    return "BOUND DISPLAY MIRRORS ANOTHER - EXTEND THE DESKTOP";
  case OutputStatus::DuplicateRenderServer:
    return "TWO RENDER SERVERS CLAIM THIS NODE INDEX";
  case OutputStatus::PresenterRefused:
    return "DISPLAY DOES NOT OFFER THE SAVED MODE";
  case OutputStatus::NoBoundDisplay:
    return "NO DISPLAY IS BOUND TO THIS RENDER SERVER";
  }
  return "OUTPUT NOT DEPLOYED";
}

std::string output_node_failure_text(const OutputMonitorState::Node &node) {
  const core::PresentFailure &failure = node.present_failure;
  if (failure.failed_frames == 0U) {
    return {};
  }
  std::string text;
  if (failure.step == core::PresentStep::DrawMap) {
    text = "Drawing " +
           (node.present_failure_map.empty() ? std::string{"a map"}
                                             : node.present_failure_map) +
           " failed";
  } else if (failure.step == core::PresentStep::MapProjection) {
    text = "The projection surface of " +
           (node.present_failure_map.empty() ? std::string{"a map"}
                                             : node.present_failure_map) +
           " is unavailable";
  } else {
    text = std::string{core::present_step_text(failure.step)};
    if (!text.empty()) {
      text[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    }
  }
  if (const auto reason = core::gpu_refusal_text(failure.refusal);
      !reason.empty()) {
    text += ": ";
    text += reason;
  }
  if (failure.show_frame != 0U) {
    text += " (show frame " + std::to_string(failure.show_frame) + ")";
  }
  return text;
}

std::string output_node_content_label(const OutputMonitorState::Node &node) {
  using ContentStatus = OutputMonitorState::Node::ContentStatus;
  switch (node.content) {
  case ContentStatus::NotRequired:
    return {};
  case ContentStatus::Checking:
    return "ASSETS CHECKING";
  case ContentStatus::Ready:
    return std::string{"ASSETS "} + std::to_string(node.assets_ready) + '/' +
           std::to_string(node.assets_required) + " READY";
  case ContentStatus::Missing:
    return std::string{"ASSETS MISSING "} +
           std::to_string(node.assets_missing) + " / STALE " +
           std::to_string(node.assets_mismatched);
  case ContentStatus::Error:
    return node.assets_verification_failed == 0U
               ? std::string{"ASSET CHECK ERROR"}
               : std::string{"ASSET VERIFY FAILED "} +
                     std::to_string(node.assets_verification_failed);
  }
  return "ASSET CHECK ERROR";
}

OutputMonitorFrame OutputMonitorPanel::draw(gui::UiContext &ctx,
                                            const gui::Rect &bounds,
                                            const OutputMonitorState &state) {
  OutputMonitorFrame frame{};
  if (bounds.width <= 0.0F || bounds.height <= 0.0F) {
    return frame;
  }
  const gui::Theme &theme = ctx.theme();
  ctx.push_clip(bounds);
  ctx.panel(bounds);

  using ClusterHealth = OutputMonitorState::Cluster::Health;
  using NodeStatus = OutputMonitorState::Node::Status;
  using ContentStatus = OutputMonitorState::Node::ContentStatus;
  using NodeSync = OutputMonitorState::Node::SyncStatus;

  const auto status_label = [](NodeStatus status) -> std::string_view {
    switch (status) {
    case NodeStatus::Disabled:
      return "CONTROL OFF";
    case NodeStatus::InvalidConfiguration:
      return "CONFIG ERROR";
    case NodeStatus::DuplicateNodeIndex:
      return "DUPLICATE INDEX";
    case NodeStatus::Probing:
      return "CHECKING";
    case NodeStatus::Stopped:
      return "STOPPED";
    case NodeStatus::Starting:
      return "STARTING";
    case NodeStatus::Running:
      return "LIVE";
    case NodeStatus::Stopping:
      return "STOPPING";
    case NodeStatus::Faulted:
      return "FAULTED";
    case NodeStatus::Unreachable:
      return "OFFLINE";
    case NodeStatus::AuthenticationFailed:
      return "AUTH ERROR";
    case NodeStatus::ProtocolError:
      return "PROTOCOL ERROR";
    case NodeStatus::ConfigurationMismatch:
      return "NODE MISMATCH";
    case NodeStatus::Busy:
      return "BUSY";
    }
    return "UNKNOWN";
  };
  const auto status_color = [&](NodeStatus status) {
    switch (status) {
    case NodeStatus::Running:
      return theme.good;
    case NodeStatus::Starting:
    case NodeStatus::Stopping:
    case NodeStatus::Probing:
    case NodeStatus::Busy:
      return theme.caution;
    case NodeStatus::Faulted:
    case NodeStatus::AuthenticationFailed:
    case NodeStatus::ProtocolError:
    case NodeStatus::ConfigurationMismatch:
    case NodeStatus::InvalidConfiguration:
    case NodeStatus::DuplicateNodeIndex:
      return theme.warning;
    case NodeStatus::Disabled:
    case NodeStatus::Stopped:
    case NodeStatus::Unreachable:
      return theme.text_muted;
    }
    return theme.text_muted;
  };
  const auto sync_label = [](NodeSync status) -> std::string_view {
    switch (status) {
    case NodeSync::Unsynced:
      return "UNSYNCED";
    case NodeSync::Unverified:
      return "UNVERIFIED";
    case NodeSync::SyncDeviceMissing:
      return "NO SYNC DEVICE";
    case NodeSync::ReferenceLost:
      return "REF LOST";
    case NodeSync::ReferenceNotInUse:
      return "REF UNUSED";
    case NodeSync::Unlocked:
      return "UNLOCKED";
    case NodeSync::BarrierIncomplete:
      return "BARRIER PARTIAL";
    case NodeSync::BarrierLost:
      return "BARRIER LOST";
    case NodeSync::ShowClockReferenceUnavailable:
      return "CLOCK REF N/A";
    case NodeSync::ShowClockReferenceLost:
      return "CLOCK REF LOST";
    case NodeSync::Simulated:
      return "SIMULATED";
    case NodeSync::Locked:
      return "LOCKED";
    }
    return "UNVERIFIED";
  };
  const auto sync_color = [&](NodeSync status) {
    switch (status) {
    case NodeSync::Locked:
      return theme.good;
    case NodeSync::Simulated:
    case NodeSync::Unverified:
      return theme.keyframe;
    case NodeSync::SyncDeviceMissing:
    case NodeSync::ReferenceLost:
    case NodeSync::ReferenceNotInUse:
    case NodeSync::Unlocked:
    case NodeSync::BarrierIncomplete:
    case NodeSync::BarrierLost:
    case NodeSync::ShowClockReferenceUnavailable:
    case NodeSync::ShowClockReferenceLost:
      return theme.warning;
    case NodeSync::Unsynced:
      return theme.text_muted;
    }
    return theme.text_muted;
  };
  const auto content_color = [&](ContentStatus status) {
    switch (status) {
    case ContentStatus::Ready:
      return theme.good;
    case ContentStatus::Checking:
      return theme.caution;
    case ContentStatus::Missing:
    case ContentStatus::Error:
      return theme.warning;
    case ContentStatus::NotRequired:
      return theme.text_muted;
    }
    return theme.text_muted;
  };
  const auto content_short = [](const OutputMonitorState::Node &node) {
    switch (node.content) {
    case ContentStatus::NotRequired:
      return std::string{};
    case ContentStatus::Checking:
      return std::string{"CHECKING"};
    case ContentStatus::Ready:
      return std::to_string(node.assets_ready) + "/" +
             std::to_string(node.assets_required);
    case ContentStatus::Missing:
      return "MISSING " +
             std::to_string(node.assets_missing + node.assets_mismatched);
    case ContentStatus::Error:
      return std::string{"VERIFY FAILED"};
    }
    return std::string{};
  };
  // One line of physical timing for a node, kept whole for its details and
  // its tooltip rather than run along the row.
  const auto node_sync_line = [&](const OutputMonitorState::Node &node) {
    std::string line{"Locked "};
    line += std::to_string(node.sync_locked_outputs) + "/" +
            std::to_string(node.sync_expected_outputs);
    line += "  Barrier " + std::to_string(node.barrier_joined_outputs) + "/" +
            std::to_string(node.barrier_expected_outputs);
    line += "  Present sync " + std::to_string(node.barrier_presents_in_sync) +
            "/" + std::to_string(node.barrier_presents);
    line += "  Show clock ";
    line += node.show_clock_reference;
    if (node.show_clock_reference == "LOCKED") {
      line += " #" + std::to_string(node.show_clock_reference_frame);
    }
    line += "  Missed " + std::to_string(node.missed_frames) + " late " +
            std::to_string(node.late_frames) + " held " +
            std::to_string(node.held_frames) + " repeated " +
            std::to_string(node.repeated_frames);
    if (node.loss_of_sync_events != 0U) {
      line += "  Sync loss " + std::to_string(node.loss_of_sync_events);
    }
    if (!node.sync_detail.empty()) {
      line += " - ";
      line += node.sync_detail;
    }
    return line;
  };

  // ------------------------------------------------------------ summary
  // The cluster at a glance: one chip per fact, coloured by how it stands,
  // each carrying its full wording in a tooltip. What used to be two run-on
  // lines of capitals is now something the eye reads in one pass.
  gui::Rect area = bounds;
  const gui::Rect summary = gui::cut_top(area, summary_height);
  // The frame number at the right; chips laid in from the left until the
  // next would run into it.
  gui::Rect chips = summary;
  const std::string frame_label = "Frame " + std::to_string(state.frame);
  ctx.label(gui::cut_right(chips, ctx.font_width(frame_label) + 14.0F),
            frame_label, theme.text_muted);
  chips = gui::inset(chips, gui::Padding{8.0F, 5.0F, 6.0F, 5.0F});
  const auto chip = [&](std::string_view text, gui::UiColor color,
                        std::string_view tip) {
    const float width = ctx.font_width(text) + 26.0F;
    if (width > chips.width) {
      return;
    }
    const gui::Rect body = gui::cut_left(chips, width);
    static_cast<void>(gui::cut_left(chips, 6.0F));
    ctx.rounded(body, with_alpha(color, 0.14F), body.height * 0.5F);
    draw_status_dot(ctx, body.x + 10.0F, body.center_y(), color, 3.0F);
    ctx.label(gui::inset(body, gui::Padding{19.0F, 0.0F, 1.0F, 0.0F}), text,
              theme.text);
    ctx.tooltip(body, tip);
  };
  const bool cluster_visible =
      state.cluster.health != ClusterHealth::Unmonitored ||
      state.cluster.configured_nodes != 0U || state.cluster.renderer_active;
  if (!cluster_visible) {
    chip("Local only", theme.text_muted,
         "No render servers are configured; Program plays on this machine");
  } else {
    const gui::UiColor health_color =
        state.cluster.health == ClusterHealth::Healthy
            ? theme.good
            : (state.cluster.health == ClusterHealth::Faulted
                   ? theme.warning
                   : (state.cluster.health == ClusterHealth::Degraded
                          ? theme.caution
                          : theme.text_muted));
    const std::string_view health =
        state.cluster.health == ClusterHealth::Healthy    ? "Healthy"
        : state.cluster.health == ClusterHealth::Degraded ? "Degraded"
        : state.cluster.health == ClusterHealth::Faulted  ? "Fault"
                                                          : "Unmonitored";
    std::string control_tip{"Control "};
    control_tip += health;
    control_tip += " - " + std::to_string(state.cluster.live_sessions) +
                   " of " + std::to_string(state.cluster.configured_nodes) +
                   " render servers live";
    if (state.cluster.catching_up_sessions != 0U) {
      control_tip += ", " + std::to_string(state.cluster.catching_up_sessions) +
                     " catching up";
    }
    chip(std::string{health} + "  " +
             std::to_string(state.cluster.live_sessions) + "/" +
             std::to_string(state.cluster.configured_nodes) + " live",
         health_color, control_tip);
    // Next to the health chip so it is never the one squeezed off the row.
    if (state.cluster.control_stalled) {
      chip("Edits paused", theme.warning,
           "The Director has stopped sealing frames because the oldest one "
           "is still unfinished. Edits are not applied until it clears; "
           "the Director console names the frame and what it is waiting "
           "for.");
    }

    const bool timing_verified =
        state.cluster.sync_satisfied && !state.cluster.sync_degraded &&
        (!state.cluster.sync_required ||
         (state.cluster.external_time_sync_verified &&
          state.cluster.hardware_frame_lock_verified));
    std::string sync_tip{"Sync policy "};
    sync_tip += state.cluster.sync_mode;
    sync_tip += " - ";
    sync_tip += state.cluster.sync_headline;
    if (state.cluster.sync_required) {
      sync_tip += " - locked " +
                  std::to_string(state.cluster.sync_locked_outputs) + "/" +
                  std::to_string(state.cluster.sync_expected_outputs) +
                  ", barrier " +
                  std::to_string(state.cluster.barrier_joined_outputs) + "/" +
                  std::to_string(state.cluster.sync_expected_outputs);
      if (state.cluster.sync_groups != 0U) {
        sync_tip += ", " + std::to_string(state.cluster.sync_groups) + " groups";
      }
      if (state.cluster.sync_configuration_issues != 0U) {
        sync_tip += ", " +
                    std::to_string(state.cluster.sync_configuration_issues) +
                    " configuration issues";
      }
      if (state.cluster.sync_degraded) {
        sync_tip += ", promoted degraded";
      }
    }
    chip(std::string{"Sync "} + std::string{state.cluster.sync_mode},
         timing_verified ? theme.good
                         : (state.cluster.sync_required ? theme.warning
                                                        : theme.text_muted),
         sync_tip);

    if (state.cluster.renderer_active) {
      std::string renderer_tip{state.cluster.renderer_headline};
      if (state.cluster.conformance_difference.reason !=
          core::ConformanceMismatch::None) {
        renderer_tip += ": ";
        renderer_tip +=
            core::conformance_detail(state.cluster.conformance_difference);
      }
      if (state.cluster.renderer_memory_needed != 0U) {
        renderer_tip += ": its GPU needs " +
                        gpu_memory_text(state.cluster.renderer_memory_needed) +
                        " more for this graph's layers and has " +
                        gpu_memory_text(state.cluster.renderer_memory_free) +
                        " free";
      }
      std::string renderer_text;
      if (state.cluster.renderer_satisfied) {
        renderer_text = state.cluster.renderer_selected_advanced
                            ? "Advanced"
                            : "Basic";
        if (state.cluster.renderer_fallback) {
          renderer_text += " (fallback)";
        }
      } else {
        renderer_text = state.cluster.renderer_failure_local
                            ? "Renderer: editor"
                            : "Renderer: node " +
                                  std::to_string(
                                      state.cluster.renderer_failure_node);
      }
      chip(renderer_text,
           state.cluster.renderer_satisfied ? theme.good : theme.warning,
           renderer_tip);
    }
    if (state.cluster.preview_memory_needed != 0U) {
      chip("Preview short of GPU memory", theme.warning,
           "This machine's preview needs " +
               gpu_memory_text(state.cluster.preview_memory_needed) +
               " more GPU memory for this graph's layers and has " +
               gpu_memory_text(state.cluster.preview_memory_free) +
               " free. Render nodes draw Program, so the show is not held.");
    }
    if (state.cluster.release_lag_frames != 0U ||
        state.cluster.pacer_lag_frames != 0U) {
      const core::u64 lag = std::max(state.cluster.release_lag_frames,
                                     state.cluster.pacer_lag_frames);
      chip("Lag " + std::to_string(lag) + "f", theme.keyframe,
           "Release lag " + std::to_string(state.cluster.release_lag_frames) +
               " frames, pacer lag " +
               std::to_string(state.cluster.pacer_lag_frames) + " frames");
    }
    const core::u64 incidents =
        state.cluster.heartbeat_timeouts + state.cluster.session_failures +
        state.cluster.barrier_rejections + state.cluster.present_failures +
        state.cluster.lockstep_demotions;
    if (incidents != 0U) {
      chip(std::to_string(incidents) + " incidents", theme.warning,
           "Heartbeat timeouts " +
               std::to_string(state.cluster.heartbeat_timeouts) +
               ", session failures " +
               std::to_string(state.cluster.session_failures) +
               ", barrier rejections " +
               std::to_string(state.cluster.barrier_rejections) +
               ", present failures " +
               std::to_string(state.cluster.present_failures) +
               ", nodes dropped from lockstep for lagging " +
               std::to_string(state.cluster.lockstep_demotions));
    }
    if (state.cluster.diagnostics_recording) {
      chip("Recording", theme.warning, "Diagnostics log is recording");
    }
    if (state.cluster.control_unsecured && state.cluster.configured_nodes != 0U) {
      chip("No password", theme.warning,
           "Render machines are controlled without a password: any Media "
           "Server on this network can start and stop them. Set a render "
           "node password in Preferences, and give each render machine "
           "the same one in its setup window.");
    }
  }

  // ------------------------------------------------------------- columns
  const bool any_assets = std::any_of(
      state.nodes.begin(), state.nodes.end(),
      [](const OutputMonitorState::Node &node) {
        return node.content != ContentStatus::NotRequired;
      });
  constexpr float twirl_width = 22.0F;
  constexpr float resolution_width = 100.0F;
  constexpr float fps_width = 40.0F;
  constexpr float status_width = 100.0F;
  constexpr float assets_width = 96.0F;
  constexpr float sync_width = 108.0F;
  constexpr float actions_width = 62.0F;
  constexpr float minimum_name = 120.0F;
  float available = bounds.width - twirl_width - actions_width - 12.0F;
  const auto fits = [&](float width) {
    if (available - width >= minimum_name) {
      available -= width;
      return true;
    }
    return false;
  };
  const bool show_status = fits(status_width);
  const bool show_resolution = fits(resolution_width);
  const bool show_fps = fits(fps_width);
  const bool show_assets = any_assets && fits(assets_width);
  const bool show_sync = state.cluster.sync_required && fits(sync_width);
  // The columns, laid in from the right: the actions, then each shown fact;
  // the name takes what is left after the twirl.
  gui::Rect lanes =
      gui::inset(bounds, gui::Padding{twirl_width + 4.0F, 0.0F, 6.0F, 0.0F});
  const gui::Rect actions_column = gui::cut_right(lanes, actions_width);
  const auto place = [&](bool shown, float width) {
    return shown ? gui::cut_right(lanes, width) : gui::Rect{};
  };
  const gui::Rect sync_column = place(show_sync, sync_width);
  const gui::Rect assets_column = place(show_assets, assets_width);
  const gui::Rect status_column = place(show_status, status_width);
  const gui::Rect fps_column = place(show_fps, fps_width);
  const gui::Rect resolution_column = place(show_resolution, resolution_width);
  const gui::Rect name_column =
      gui::inset(lanes, gui::Padding{0.0F, 0.0F, 8.0F, 0.0F});
  const float sync_x = sync_column.x;
  const float assets_x = assets_column.x;
  const float status_x = status_column.x;
  const float name_x = name_column.x;
  const float name_width = name_column.width;
  const float actions_x = actions_column.x;

  const gui::Rect header = gui::cut_top(area, header_height);
  ctx.panel(header, theme.panel);
  {
    gui::Rect rule = header;
    ctx.panel(gui::cut_bottom(rule, 1.0F), theme.divider);
  }
  const auto heading = [&](bool shown, const gui::Rect &column,
                           std::string_view text) {
    if (shown) {
      ctx.label(gui::cell(column, header), text, theme.text_muted);
    }
  };
  heading(true, name_column, "OUTPUT");
  heading(show_resolution, resolution_column, "RESOLUTION");
  heading(show_fps, fps_column, "FPS");
  heading(show_status, status_column, "STATUS");
  heading(show_assets, assets_column, "ASSETS");
  heading(show_sync, sync_column, "SYNC");

  // ----------------------------------------------------- row geometry
  // Details open under a row when it is clicked, so the evidence an operator
  // needs for one output never has to share a line with another's.
  const auto node_details = [&](const OutputMonitorState::Node &node) {
    std::vector<std::pair<std::string, std::string>> lines;
    lines.emplace_back("Endpoint", node.endpoint.empty() ? std::string{"-"}
                                                         : node.endpoint);
    std::string status{status_label(node.status)};
    if (!node.detail.empty()) {
      status += " - " + node.detail;
    }
    lines.emplace_back("Status", std::move(status));
    if (node.status == NodeStatus::Running) {
      std::string connection{output_node_connection_label(node)};
      if (node.connection == OutputMonitorState::Node::Connection::Disconnected) {
        connection += " - waiting for the node to connect automatically; "
                      "check its Director address and show ID if this persists";
      } else {
        connection += " - frame " + std::to_string(node.committed_frame) +
                      " / " + std::to_string(node.target_frame);
      }
      lines.emplace_back("Editor connection", std::move(connection));
    }
    // Above Assets deliberately. A node whose displays failed reports nothing
    // for its renderer and its assets, so those rows read like a slow load;
    // the operator should meet the actual reason first.
    if (node.output_failed()) {
      lines.emplace_back(
          "Output", std::string{output_node_deployment_label(node)} +
                        " - this node is not driving its displays");
    }
    // Also above Assets: a node that cannot draw holds its last picture, and
    // the cause is a refusal on that machine, not a slow load.
    if (node.present_failure.failed_frames != 0U) {
      const std::string count =
          std::to_string(node.present_failure.failed_frames) +
          (node.present_failure.failed_frames == 1U ? " frame" : " frames");
      lines.emplace_back(
          "Drawing",
          node.frames_failing()
              ? output_node_failure_text(node) +
                    " - holding the last picture, " + count + " failed"
              : "Drawing again - " + count + " failed earlier. Last: " +
                    output_node_failure_text(node));
    }
    if (node.content != ContentStatus::NotRequired) {
      std::string content = output_node_content_label(node);
      if (!node.content_detail.empty()) {
        content += " - " + node.content_detail;
      }
      lines.emplace_back("Assets", std::move(content));
    }
    if (state.cluster.sync_required) {
      lines.emplace_back(std::string{"Sync "} +
                             std::string{sync_label(node.sync)},
                         node_sync_line(node));
    }
    return lines;
  };
  constexpr float detail_line = 20.0F;
  const float timing_lines = state.cluster.conformance_active ? 3.0F : 0.0F;
  // The log toggle line, and the log itself while it is open.
  const float log_box_height =
      log_line_height * static_cast<float>(log_visible_lines) + 8.0F;
  const auto log_height = [&](const OutputMonitorState::Node &node) {
    return 26.0F +
           (log_open_.contains(node.device) ? log_box_height + 4.0F : 0.0F);
  };
  const auto node_height = [&](const OutputMonitorState::Node &node) {
    if (!expanded_.contains(node.device)) {
      return row_height;
    }
    return row_height + 8.0F +
           detail_line * (static_cast<float>(node_details(node).size()) +
                          timing_lines) +
           log_height(node);
  };
  const float program_details_height =
      program_expanded_ && state.cluster.conformance_active
          ? 8.0F + 24.0F + (timing_expanded_ ? 2.0F + 1.0F +
                                                   static_cast<float>(
                                                       core::render_pass_count)
                                             : 2.0F) *
                                detail_line +
                24.0F + detail_line +
                (conformance_expanded_
                     ? detail_line * (4.0F + static_cast<float>(
                                                 core::conformance_pass_count))
                     : 0.0F)
          : 0.0F;
  const float program_height = row_height + program_details_height;

  const gui::Rect footer = gui::cut_bottom(area, footer_height);
  const gui::Rect list_bounds = area;
  float content_height = program_height;
  for (const OutputMonitorState::Node &node : state.nodes) {
    content_height += node_height(node);
  }

  const auto status_pill = [&](float x, float centre_y, std::string_view text,
                               gui::UiColor color, bool filled,
                               std::string_view tip) {
    const gui::Rect pill =
        draw_pill(ctx, x, centre_y, gui::sentence_case(text), color, filled);
    ctx.tooltip(pill, tip);
  };
  const auto row_cells = [&](const gui::Rect &row, std::string_view name,
                             std::string_view tag, std::string_view resolution,
                             std::string_view fps) {
    const float centre_y = row.y + row_height * 0.5F;
    const float tag_width = tag.empty() ? 0.0F : pill_width(ctx, tag) + 8.0F;
    const float name_text = std::min(ctx.font_width(name),
                                     std::max(name_width - tag_width, 0.0F));
    ctx.label(gui::Rect{name_x, row.y, name_text + 2.0F, row_height}, name,
              theme.text);
    if (!tag.empty() && name_width > tag_width + 40.0F) {
      static_cast<void>(draw_pill(ctx, name_x + name_text + 8.0F, centre_y,
                                  gui::sentence_case(tag), theme.text_muted,
                                  false));
    }
    if (show_resolution) {
      ctx.label(gui::cell(resolution_column, row), resolution,
                theme.text_muted);
    }
    if (show_fps) {
      ctx.label(gui::cell(fps_column, row), fps, theme.text_muted);
    }
  };

  // Over an open log the wheel scrolls the log, not the table.
  const bool over_log = log_hover_.contains(ctx.pointer_x(), ctx.pointer_y());
  log_hover_ = gui::Rect{};
  float y = list_bounds.y - scroll_.begin(ctx, list_bounds,
                                          over_log ? 0.0F : row_height * 2.0F);
  // The `index`th action button from the right of a row's action column.
  const auto action_bounds = [&](const gui::Rect &line, std::size_t index) {
    gui::Rect slots = gui::cell(actions_column, line);
    gui::Rect slot{};
    for (std::size_t step = 0U; step <= index; ++step) {
      slot = gui::cut_right(slots, 28.0F);
    }
    return gui::centered(gui::cut_left(slot, 26.0F), 26.0F, 24.0F);
  };
  // A row's details: lines of text down the name column, `top` below the
  // row's own line.
  const auto detail_lines = [&](const gui::Rect &line, const gui::Rect &row,
                                float top) {
    gui::Metrics metrics = theme.metrics;
    metrics.inset = 0.0F;
    metrics.gap = 0.0F;
    return gui::FormLayout{
        gui::Rect{name_x, line.bottom() + top,
                  std::max(row.right() - name_x - 12.0F, 0.0F), 0.0F},
        metrics};
  };

  // ---------------------------------------------------------- Program
  const std::string program_resolution =
      resolution_text(state.width, state.height);
  const std::string program_fps = fps_text(state.frames_per_second);
  {
    const gui::Rect row{bounds.x, y, bounds.width, program_height};
    gui::Rect line = row;
    line = gui::cut_top(line, row_height);
    ctx.rounded(gui::inset(row, 4.0F, 2.0F), theme.row, theme.radius);
    const float centre_y = line.center_y();
    if (state.cluster.conformance_active) {
      draw_twirl(ctx, bounds.x + 12.0F, centre_y, program_expanded_,
                 theme.text_muted);
    }
    row_cells(line, "Program", "PREVIEW", program_resolution, program_fps);
    if (show_status) {
      status_pill(status_x, centre_y, state.playing ? "PLAYING" : "PAUSED",
                  state.playing ? theme.good : theme.text_muted, state.playing,
                  state.playing ? "Program is playing" : "Program is paused");
    }
    if (state.cluster.conformance_active) {
      if (icon_button(ctx, gui::ui_id(id_, "conformance.check"),
                      action_bounds(line, 0U), gui::Icon::Restart,
                      "Check the render cohort: every node renders the same "
                      "fixed scene and compares")) {
        frame.check_render_conformance = true;
      }
      const gui::Interaction toggle =
          clipped_grab(ctx, gui::ui_id(id_, "program.details"),
                       gui::Rect{bounds.x, line.y,
                                 actions_x - bounds.x, row_height},
                       list_bounds);
      if (toggle.clicked) {
        program_expanded_ = !program_expanded_;
      }
    }
    if (program_details_height > 0.0F) {
      gui::FormLayout details = detail_lines(line, row, 4.0F);
      // A details toggle: a 160px button, 4px above the next line.
      const auto toggle_button = [&](std::string_view key,
                                     std::string_view label) {
        gui::Rect strip = details.next(20.0F, 4.0F);
        return ctx.button(gui::ui_id(id_, key), gui::cut_left(strip, 160.0F), label);
      };
      const auto text_line = [&](float indent = 0.0F) {
        return gui::inset(details.next(detail_line, 0.0F),
                          gui::Padding{indent, 0.0F, 0.0F, 0.0F});
      };
      if (toggle_button("timing.details",
                        timing_expanded_ ? "Hide pass costs" : "Pass costs")) {
        timing_expanded_ = !timing_expanded_;
      }
      const core::FrameTiming &t = state.cluster.frame_timing;
      std::array<char, 256U> text{};
      if (t.sequence == 0U) {
        ctx.label(text_line(), "Frame timing awaiting a completed submission",
                  theme.text_muted);
        details.space(detail_line);
      } else {
        std::snprintf(text.data(), text.size(),
                      "CPU record %.3f ms   GPU %s%.3f ms   Upload %.2f MiB",
                      static_cast<double>(t.cpu_record_ns) / 1e6,
                      t.gpu_valid ? "" : "unavailable ",
                      static_cast<double>(t.gpu_ns) / 1e6,
                      static_cast<double>(t.upload_bytes) / 1048576.0);
        ctx.label(text_line(), text.data(),
                  t.over_budget(state.cluster.frame_budget_us) ? theme.warning
                                                                : theme.text);
        std::snprintf(
            text.data(), text.size(),
            "Present %.3f ms   Barrier %llu/%llu presents, %llu flips   "
            "Budget %.3f ms%s",
            static_cast<double>(t.present_ns) / 1e6,
            static_cast<unsigned long long>(t.presents_in_sync),
            static_cast<unsigned long long>(t.presents),
            static_cast<unsigned long long>(t.flips_in_sync),
            static_cast<double>(state.cluster.frame_budget_us) / 1000.0,
            state.cluster.frame_budget_us == 0U ? " (report only)" : "");
        ctx.label(text_line(), text.data(), theme.text_muted);
      }
      if (timing_expanded_) {
        std::snprintf(text.data(), text.size(),
                      "Completed submission %llu   Dropped intervals %llu",
                      static_cast<unsigned long long>(t.sequence),
                      static_cast<unsigned long long>(t.query_overflows));
        ctx.label(text_line(), text.data(), theme.text_muted);
        for (std::size_t i = 0U; i < core::render_pass_count; ++i) {
          std::snprintf(text.data(), text.size(), "%.*s  %.3f ms",
                        static_cast<int>(core::render_pass_names[i].size()),
                        core::render_pass_names[i].data(),
                        static_cast<double>(t.pass_ns[i]) / 1e6);
          ctx.label(text_line(12.0F), text.data(),
                    t.pass_ns[i] != 0U ? theme.text : theme.text_muted);
        }
      }
      if (toggle_button("conformance.details",
                        conformance_expanded_ ? "Hide pass digests"
                                              : "Pass digests")) {
        conformance_expanded_ = !conformance_expanded_;
      }
      const auto &proof = state.cluster.conformance;
      std::string proof_summary;
      switch (proof.state) {
      case core::ConformanceState::Unknown:
        proof_summary = "Render check awaiting a Program device";
        break;
      case core::ConformanceState::Running:
        proof_summary = "Rendering the fixed conformance scene on this machine";
        break;
      case core::ConformanceState::Failed:
        proof_summary = "Local render check failed: ";
        proof_summary += core::conformance_detail(
            {core::ConformanceMismatch::Failed, proof.failed_pass});
        break;
      case core::ConformanceState::Passed:
        proof_summary = "Local render check passed - ";
        proof_summary += proof.strict_float ? "IEEE strict" : "experimental relaxed";
        break;
      }
      ctx.label(text_line(), proof_summary, theme.text_muted);
      if (conformance_expanded_) {
        const auto version = proof.adapter.driver_version;
        ctx.label(text_line(), proof.adapter.name.data(), theme.text_muted);
        ctx.label(text_line(),
                  "Driver " + std::to_string(version >> 48U) + "." +
                      std::to_string((version >> 32U) & 65535U) + "." +
                      std::to_string((version >> 16U) & 65535U) + "." +
                      std::to_string(version & 65535U),
                  theme.text_muted);
        constexpr char hex[] = "0123456789abcdef";
        std::string bundle;
        for (auto byte : proof.shader_bundle) {
          bundle += hex[byte >> 4U];
          bundle += hex[byte & 15U];
        }
        ctx.label(text_line(), "Shader SHA-256 " + bundle, theme.text_muted);
        details.space(detail_line);
        for (std::size_t i = 0; i < core::conformance_pass_count; ++i) {
          std::string pass{core::conformance_pass_names[i]};
          pass += "  ";
          for (int shift = 60; shift >= 0; shift -= 4) {
            pass += hex[(proof.passes[i] >> shift) & 15U];
          }
          const bool differs =
              static_cast<std::size_t>(
                  state.cluster.conformance_difference.pass) == i;
          if (differs) {
            pass += "  first difference";
          }
          ctx.label(text_line(12.0F), pass,
                    differs ? theme.warning : theme.text_muted);
        }
      }
    }
    y += program_height;
  }

  // ------------------------------------------------------- render servers
  for (std::size_t index = 0U; index < state.nodes.size(); ++index) {
    const OutputMonitorState::Node &node = state.nodes[index];
    const float height = node_height(node);
    const gui::Rect row{bounds.x, y, bounds.width, height};
    gui::Rect line = row;
    line = gui::cut_top(line, row_height);
    y += height;
    if (row.bottom() <= list_bounds.y || row.y >= list_bounds.bottom()) {
      continue;
    }
    const bool expanded = expanded_.contains(node.device);
    const bool hovered = !ctx.pointer_blocked() &&
                         line.contains(ctx.pointer_x(), ctx.pointer_y()) &&
                         list_bounds.contains(ctx.pointer_x(), ctx.pointer_y());
    const gui::Rect card = gui::inset(row, 4.0F, 2.0F);
    ctx.rounded(card,
                hovered ? gui::blend(theme.row, theme.widget_hover, 0.5F)
                        : theme.row,
                theme.radius);
    const float centre_y = line.center_y();
    draw_twirl(ctx, bounds.x + 12.0F, centre_y, expanded,
               hovered ? theme.text : theme.text_muted);
    const std::string identity = feed_output_identity(node.node_index);
    row_cells(line, node.name.empty() ? identity : node.name, identity,
              resolution_text(node.width, node.height), program_fps);

    const bool awaiting_connection = node.status == NodeStatus::Running &&
        node.connection != OutputMonitorState::Node::Connection::Live;
    // A live session whose outputs never opened is the case this row used to
    // read as healthy: the process is running and connected, and nothing it
    // reports afterwards is true of a display. Say OUTPUT FAILED instead, and
    // put the reason in the tooltip rather than only in the expanded detail.
    const bool output_failed = node.output_failed();
    // Live and connected, but every frame it tries fails: from the wall this
    // is a frozen picture, so the row says so rather than LIVE.
    const bool frames_failing = !output_failed && node.frames_failing();
    const gui::UiColor node_color = output_failed || frames_failing
        ? theme.warning
        : awaiting_connection ? theme.caution
                              : status_color(node.status);
    std::string status_tip{status_label(node.status)};
    if (!node.detail.empty()) {
      status_tip += " - " + node.detail;
    }
    const auto displayed_status = output_failed
        ? std::string_view{"OUTPUT FAILED"}
        : frames_failing ? std::string_view{"FRAMES FAILING"}
        : node.status == NodeStatus::Running
            ? output_node_connection_label(node)
            : status_label(node.status);
    if (node.status == NodeStatus::Running) {
      status_tip += " - ";
      status_tip += displayed_status;
    }
    if (output_failed) {
      status_tip += " - ";
      status_tip += output_node_deployment_label(node);
    }
    if (frames_failing) {
      status_tip += " - ";
      status_tip += output_node_failure_text(node);
    }
    if (show_status) {
      const bool faulted = node.status == NodeStatus::Faulted ||
                           node.status == NodeStatus::AuthenticationFailed ||
                           node.status == NodeStatus::ProtocolError ||
                           node.status == NodeStatus::ConfigurationMismatch ||
                           node.status == NodeStatus::InvalidConfiguration ||
                           node.status == NodeStatus::DuplicateNodeIndex;
      // Lit when live or broken - the two states worth seeing from across
      // the room - and outlined otherwise.
      status_pill(status_x, centre_y, displayed_status, node_color,
                  node.status == NodeStatus::Running || faulted ||
                      frames_failing,
                  status_tip);
    }
    if (show_assets && node.content != ContentStatus::NotRequired) {
      std::string tip = output_node_content_label(node);
      if (!node.content_detail.empty()) {
        tip += " - " + node.content_detail;
      }
      status_pill(assets_x, centre_y, content_short(node),
                  content_color(node.content), false, tip);
    }
    if (show_sync) {
      status_pill(sync_x, centre_y, sync_label(node.sync),
                  sync_color(node.sync), false, node_sync_line(node));
    }

    // Lifecycle actions: the same icon in the same place on every row.
    const bool row_visible = gui::intersection(line, list_bounds).height > 0.0F;
    float button_x = actions_column.right();
    std::size_t placed = 0U;
    const auto action = [&](std::string_view suffix, gui::Icon icon,
                            std::string_view tip, OutputLifecycleAction kind,
                            bool enabled) {
      const gui::Rect button = action_bounds(line, placed++);
      button_x = button.x;
      const gui::UiId button_id = gui::ui_id(
          id_, suffix, node.device.value() * 8U + static_cast<core::u64>(kind));
      if (row_visible &&
          icon_button(ctx, button_id, button, icon, tip, false, enabled) &&
          !frame.lifecycle.has_value()) {
        frame.lifecycle = OutputLifecycleRequest{node.device, node.node_index,
                                                 node.endpoint, kind};
      }
    };
    switch (node.status) {
    case NodeStatus::Stopped:
    case NodeStatus::Faulted:
      action("output.lifecycle.start", gui::Icon::Play,
             node.content_ready() ? "Start this output"
                                  : "Start - waiting for its assets",
             OutputLifecycleAction::Start, node.content_ready());
      break;
    case NodeStatus::Running:
      action("output.lifecycle.stop", gui::Icon::Stop, "Stop this output",
             OutputLifecycleAction::Stop, true);
      action("output.lifecycle.restart", gui::Icon::Restart,
             "Restart this output", OutputLifecycleAction::Restart,
             node.content_ready());
      break;
    case NodeStatus::Unreachable:
    case NodeStatus::AuthenticationFailed:
    case NodeStatus::ProtocolError:
    case NodeStatus::ConfigurationMismatch:
      action("output.lifecycle.status", gui::Icon::Restart,
             "Try to reach this output again", OutputLifecycleAction::Status,
             true);
      break;
    case NodeStatus::Disabled:
    case NodeStatus::InvalidConfiguration:
    case NodeStatus::DuplicateNodeIndex:
    case NodeStatus::Probing:
    case NodeStatus::Starting:
    case NodeStatus::Stopping:
    case NodeStatus::Busy:
      break;
    }
    // The rest of the row opens its details; drawn after the buttons so they
    // keep their presses.
    const gui::Interaction toggle = clipped_grab(
        ctx, gui::ui_id(id_, "output.details", node.device.value()),
        gui::Rect{line.x, line.y, std::max(button_x - line.x - 2.0F, 0.0F),
                  row_height},
        list_bounds);
    if (toggle.clicked) {
      if (expanded) {
        expanded_.erase(node.device);
      } else {
        expanded_.insert(node.device);
      }
    }
    if (expanded) {
      gui::FormLayout details = detail_lines(line, row, 2.0F);
      // A key in a 110px column and its value 6px after it.
      const auto detail = [&](std::string_view key, std::string_view value,
                              gui::UiColor color) {
        gui::Rect strip = details.next(detail_line, 0.0F);
        ctx.label(gui::cut_left(strip, 110.0F), key, theme.text_muted);
        static_cast<void>(gui::cut_left(strip, 6.0F));
        ctx.label(strip, value, color);
      };
      for (const auto &[key, value] : node_details(node)) {
        detail(key, value, theme.text);
      }
      if (state.cluster.conformance_active) {
        const core::FrameTiming &t = node.frame_timing;
        std::array<char, 256U> text{};
        std::snprintf(text.data(), text.size(),
                      "CPU %.3f ms   GPU %s%.3f ms   Present %.3f ms",
                      static_cast<double>(t.cpu_record_ns) / 1e6,
                      t.gpu_valid ? "" : "unavailable ",
                      static_cast<double>(t.gpu_ns) / 1e6,
                      static_cast<double>(t.present_ns) / 1e6);
        detail("Frame timing",
               t.sequence == 0U ? std::string_view{"awaiting a submission"}
                                : std::string_view{text.data()},
               theme.text);
        const auto difference = core::compare_render_conformance(
            state.cluster.conformance, node.conformance);
        std::string check =
            difference.reason != core::ConformanceMismatch::None
                ? std::string{core::conformance_detail(difference)}
            : difference.adapter_differs
                ? std::string{"matches on another GPU / driver"}
                : std::string{"matches"};
        detail("Render check", check,
               difference.reason == core::ConformanceMismatch::None
                   ? theme.good
                   : theme.warning);
        detail("Adapter", node.conformance.adapter.name.data(),
               theme.text_muted);
      }

      // The node's own log, read back by its machine's agent: what it said
      // while it was hung, lagging or reconnecting, and how its last run
      // ended. Asking never touches the show.
      const auto open_view = log_open_.find(node.device);
      const bool log_open = open_view != log_open_.end();
      gui::Rect strip = details.next(22.0F, 4.0F);
      if (ctx.button(gui::ui_id(id_, "output.log", node.device.value()),
                     gui::cut_left(strip, 120.0F),
                     log_open ? "Hide log" : "Show log")) {
        if (log_open) {
          log_open_.erase(open_view);
        } else {
          log_open_.emplace(node.device, LogView{});
        }
      }
      // Drawn from the next frame on, once the row has grown to hold it.
      if (const auto view = log_open_.find(node.device);
          log_open && view != log_open_.end()) {
        LogView &log_view = view->second;
        static_cast<void>(gui::cut_left(strip, 8.0F));
        const gui::Rect refresh = gui::cut_left(strip, 26.0F);
        const bool refresh_pressed =
            icon_button(ctx,
                        gui::ui_id(id_, "output.log.refresh",
                                   node.device.value()),
                        gui::centered(refresh, 26.0F, 22.0F),
                        gui::Icon::Restart, "Read the log again", false,
                        !node.log_fetching);
        static_cast<void>(gui::cut_left(strip, 6.0F));
        const OutputNodeLog *log = node.log.get();
        const bool failed =
            log != nullptr && log->status == OutputNodeLog::Status::Failed;
        const std::string log_summary = output_node_log_summary(node);
        ctx.label(strip, log_summary,
                  failed ? theme.warning
                  : log != nullptr && log->dropped_bytes > 0U
                      ? theme.caution
                      : theme.text_muted);
        if (log != nullptr && !log->detail.empty()) {
          ctx.tooltip(strip, log->detail);
        }
        // Read on open, then again every few seconds while it stays open.
        const double now = ctx.time_seconds();
        if ((refresh_pressed || now >= log_view.refresh_at) &&
            !node.log_fetching && !frame.lifecycle.has_value()) {
          frame.lifecycle = OutputLifecycleRequest{
              node.device, node.node_index, node.endpoint,
              OutputLifecycleAction::FetchLog};
          log_view.refresh_at = now + log_refresh_seconds;
        }

        const gui::Rect box = details.next(log_box_height, 4.0F);
        ctx.rounded(box, theme.track, theme.radius_small);
        if (const gui::Rect shown = gui::intersection(box, list_bounds);
            shown.contains(ctx.pointer_x(), ctx.pointer_y())) {
          log_hover_ = shown;
        }
        // New lines arrive at the bottom; follow them unless the operator
        // has scrolled up to read something.
        if (log_view.seen.lock() != node.log) {
          if (log_view.seen.expired() || log_view.scroll.at_end()) {
            log_view.scroll.scroll_to_end();
          }
          log_view.seen = node.log;
        }
        const gui::Rect viewport =
            gui::intersection(gui::inset(box, 8.0F, 4.0F), list_bounds);
        const float offset =
            log_view.scroll.begin(ctx, viewport, log_line_height * 3.0F);
        const gui::Rect inner = gui::inset(box, 8.0F, 4.0F);
        float content = 0.0F;
        if (log != nullptr && log->status == OutputNodeLog::Status::Available) {
          const std::size_t previous =
              std::min(log->previous_run_lines, log->lines.size());
          // A divider where the current run begins, when the previous run's
          // last lines are shown above it.
          const std::size_t rows =
              log->lines.size() + (previous > 0U ? 2U : 0U);
          content = static_cast<float>(rows) * log_line_height;
          const auto first = static_cast<std::size_t>(
              std::max(offset / log_line_height, 0.0F));
          const std::size_t last = std::min(
              rows, first + log_visible_lines + 2U);
          for (std::size_t row_index = first; row_index < last; ++row_index) {
            const gui::Rect text{
                inner.x,
                inner.y - offset +
                    static_cast<float>(row_index) * log_line_height,
                inner.width, log_line_height};
            if (previous > 0U && row_index == 0U) {
              ctx.label(text, "Previous run", theme.text_muted);
              continue;
            }
            if (previous > 0U && row_index == previous + 1U) {
              ctx.label(text, "Current run", theme.text_muted);
              continue;
            }
            const std::size_t line_index =
                previous > 0U
                    ? row_index - (row_index <= previous ? 1U : 2U)
                    : row_index;
            const std::string &log_line = log->lines[line_index];
            ctx.label(text, log_line,
                      log_line_is_trouble(log_line) ? theme.warning
                      : line_index < previous   ? theme.text_muted
                                                : theme.text);
          }
        } else {
          ctx.label(gui::Rect{inner.x, inner.y, inner.width, log_line_height},
                    log_summary, theme.text_muted);
          content = log_line_height;
        }
        log_view.scroll.end(ctx, content);
      }
    }
  }
  if (state.nodes.empty() && list_bounds.height > row_height * 2.0F) {
    const gui::Rect empty{name_x, y + 6.0F, name_width + 200.0F, detail_line};
    ctx.label(empty, "No render servers", theme.text_muted);
    ctx.tooltip(empty, "Add and cable a Render Server in Hardware Map to drive "
                       "physical outputs");
  }
  scroll_.end(ctx, content_height);

  if (list_bounds.height > 8.0F) {
    const gui::Rect thumb = scroll_.thumb(
        gui::Rect{list_bounds.right() - 4.0F, list_bounds.y, 3.0F,
                  list_bounds.height},
        16.0F);
    if (thumb.height > 0.0F) {
      ctx.panel(thumb, theme.widget_hover);
    }
  }

  // -------------------------------------------------------------- footer
  ctx.panel(footer, theme.header);
  {
    gui::Rect rule = footer;
    ctx.panel(gui::cut_top(rule, 1.0F), theme.divider);
  }
  const auto actionable = [&](OutputLifecycleAction action) {
    return static_cast<std::size_t>(std::count_if(
        state.nodes.begin(), state.nodes.end(), [action](const auto &node) {
          const bool configured = node.status != NodeStatus::Disabled &&
                                  node.status != NodeStatus::InvalidConfiguration &&
                                  node.status != NodeStatus::DuplicateNodeIndex;
          return configured && (action == OutputLifecycleAction::Stop ||
                                node.content_ready());
        }));
  };
  // Batch actions laid in from the right; the count takes what is left.
  gui::Rect batch = gui::inset(footer, gui::Padding{0.0F, 4.0F, 6.0F, 4.0F});
  const auto batch_button = [&](std::string_view suffix, std::string_view label,
                                OutputLifecycleAction action, bool caution) {
    // Stopping every output takes the wall dark, so it asks twice: the
    // first press arms it for a few seconds and says so, the second stops.
    const bool confirmable = action == OutputLifecycleAction::Stop;
    const bool armed =
        confirmable && ctx.time_seconds() < stop_all_armed_until_;
    constexpr std::string_view confirm_label = "Confirm stop";
    const float label_width =
        confirmable ? std::max(ctx.font_width(label), ctx.font_width(confirm_label))
                    : ctx.font_width(label);
    const gui::Rect button = gui::cut_right(batch, label_width + 44.0F);
    static_cast<void>(gui::cut_right(batch, 6.0F));
    const bool enabled = actionable(action) > 0U;
    const gui::Interaction interaction =
        enabled ? ctx.grab(gui::ui_id(id_, suffix), button) : gui::Interaction{};
    if (enabled) {
      ctx.cursor_for(interaction, gui::Cursor::Hand);
    }
    const float radius = button.height * 0.5F;
    const bool pressed = interaction.held && !interaction.released;
    if (armed && enabled) {
      ctx.glow(button, theme.warning, radius, 10.0F);
    }
    ctx.rounded(button,
                !enabled ? with_alpha(theme.widget, 0.5F)
                         : (pressed ? theme.widget_active
                            : armed ? theme.warning
                                    : (caution ? with_alpha(theme.warning,
                                                            interaction.hovered
                                                                ? 0.22F
                                                                : 0.0F)
                                               : (interaction.hovered
                                                      ? theme.widget_hover
                                                      : theme.widget))),
                radius);
    if (caution && enabled && !armed) {
      ctx.rounded_outline(button, with_alpha(theme.warning, 0.6F), radius, 1.5F);
    }
    const gui::UiColor ink = !enabled ? with_alpha(theme.text_muted, 0.6F)
                             : armed  ? theme.background
                                      : (caution ? theme.warning : theme.text);
    const gui::Icon icon = action == OutputLifecycleAction::Stop
                               ? gui::Icon::Stop
                               : (action == OutputLifecycleAction::Restart
                                      ? gui::Icon::Restart
                                      : gui::Icon::Power);
    gui::draw_icon(ctx.draw_list(), icon, button.x + 17.0F, button.center_y(),
                   ink, 0.9F);
    ctx.label(gui::Rect{button.x + 30.0F, button.y,
                        std::max(button.width - 34.0F, 0.0F), button.height},
              armed ? confirm_label : label, ink);
    if (confirmable) {
      ctx.tooltip(button, armed ? "Press again to stop every output"
                                : "Stop every output (asks first)");
    }
    if (enabled && interaction.clicked && !frame.lifecycle.has_value()) {
      if (confirmable && !armed) {
        stop_all_armed_until_ = ctx.time_seconds() + 4.0;
      } else {
        frame.lifecycle = OutputLifecycleRequest{{}, 0U, {}, action, true};
        stop_all_armed_until_ = -1.0;
      }
    }
  };
  if (!state.nodes.empty()) {
    batch_button("output.lifecycle.stop-all", "Stop all",
                 OutputLifecycleAction::Stop, true);
    batch_button("output.lifecycle.restart-all", "Restart all",
                 OutputLifecycleAction::Restart, false);
    batch_button("output.lifecycle.start-all", "Start all",
                 OutputLifecycleAction::Start, false);
  }
  const auto live = static_cast<std::size_t>(std::count_if(
      state.nodes.begin(), state.nodes.end(), [](const auto &node) {
        return node.status == NodeStatus::Running &&
            node.connection == OutputMonitorState::Node::Connection::Live;
      }));
  const std::string count =
      state.nodes.empty()
          ? std::string{"Program only"}
          : std::to_string(state.nodes.size()) +
                (state.nodes.size() == 1U ? " render server, "
                                          : " render servers, ") +
                std::to_string(live) + " live";
  ctx.label(gui::Rect{footer.x + 10.0F, footer.y,
                      std::max(batch.right() - footer.x - 14.0F, 0.0F),
                      footer.height},
            count, theme.text_muted);
  ctx.pop_clip();
  return frame;
}

StageAssetStatus stage_asset_status(const render::RenderStats& geometry,
                                   core::u64 pending_media,
                                   render::DisplayLutState display) {
  using render::TextureStreamState;
  const auto failed = [](std::string detail) {
    return StageAssetStatus{false, "STAGE ASSET LOAD FAILED", std::move(detail)};
  };
  const auto loading = [](std::string detail) {
    return StageAssetStatus{true, "LOADING STAGE ASSETS...", std::move(detail)};
  };
  if (geometry.models_failed != 0U)
    return failed("A model could not load. Check its source path, then Rescan in Library.");
  if (geometry.model_integration_refusals != 0U)
    return failed("Model GPU resources are unavailable. Check Renderer diagnostics.");
  switch (geometry.texture_streaming.state) {
  case TextureStreamState::CapacityRefused:
    return failed("Material textures exceed the GPU texture budget. Check Renderer settings.");
  case TextureStreamState::AllocationFailed:
    return failed("GPU memory for material textures could not be allocated.");
  case TextureStreamState::UploadFailed:
    return failed("Material textures could not be uploaded to the GPU.");
  default: break;
  }
  if (!geometry.baked_ready && geometry.baked_state == render::ProbeCacheState::Failed)
    return failed("Baked lighting could not load. Check its path or rebake the scene.");
  if (geometry.particles_unsupported != 0U)
    return failed(render::particle_runtime_built
        ? "Particle effects play only in the Advanced renderer. Choose it in Renderer settings."
        : "This build does not include particle effects. Remove the effect to continue.");
  // The package's own reason (a missing texture, kernels from another
  // version) tells the operator what to fix.
  if (geometry.particles_failed != 0U)
    return failed(geometry.particle_issue.empty()
        ? "A particle effect could not load. Check its source path, then Rescan in Library."
        : "A particle effect could not load: " + geometry.particle_issue + ". Fix it, then Rescan in Library.");
  if (display == render::DisplayLutState::Failed)
    return failed("The Stage display color transform could not load.");
  if (geometry.models_pending != 0U || geometry.models_stale != 0U)
    return loading("Preparing models: " +
        std::to_string(static_cast<core::u64>(geometry.models_pending) + geometry.models_stale) + " pending.");
  if (geometry.texture_streaming.state != TextureStreamState::Ready)
    return loading("Preparing material textures on the GPU.");
  if (!geometry.baked_ready)
    return loading("Loading baked lighting.");
  if (pending_media != 0U)
    return loading("Loading media: " + std::to_string(pending_media) + " pending.");
  if (geometry.particles_pending != 0U)
    return loading("Loading particle effects: " + std::to_string(geometry.particles_pending) + " pending.");
  if (geometry.particles_behind != 0U)
    return loading("Particle effects are catching up with the playhead.");
  if (display != render::DisplayLutState::Ready)
    return loading("Preparing the Stage display color transform.");
  if (!geometry.assets.ready)
    return loading("Preparing scene assets and GPU resources.");
  return {};
}

ProgramAssetStatus
program_asset_status(core::AssetSetReadiness local, core::AssetCommit permit,
                     core::u64 control_frame, bool required,
                     const OutputMonitorState::Cluster &cluster,
                     std::string_view loading_detail) {
  if (!required || (local.ready && local.digest == 0))
    return {};
  const bool local_visible = local.ready && local.visible_commit != 0;
  if (local_visible) {
    if (cluster.asset_cohort.members <= 1 || cluster.asset_cohort.visible)
      return {};
  }
  if (!local.ready)
    return {true, "PREPARING PROGRAM", std::string{loading_detail}, true};
  if (cluster.asset_waiting_node)
    return {true,
            "WAITING FOR OUTPUT NODE " +
                std::to_string(*cluster.asset_waiting_node),
            std::to_string(cluster.asset_cohort.ready_members) + "/" +
                std::to_string(cluster.asset_cohort.members) +
                " renderers ready. Check Output Monitor and Hardware Map.",
            true};
  if (permit.digest == local.digest && permit.control_frame != 0) {
    if (permit.control_frame >= control_frame) {
      const bool retry =
          local_visible && permit.control_frame > local.visible_commit;
      return {true, retry ? "RETRYING OUTPUT UPDATE" : "UPDATING PROGRAM",
              "Assets ready. Switching in " +
                  std::to_string(permit.control_frame - control_frame) +
                  " control frames.",
              !retry};
    }
    if (local_visible && !core::asset_commit_confirmation_expired(
                             permit.control_frame, control_frame))
      return {true, "CONFIRMING OUTPUT UPDATE",
              "Program updated. Waiting for all output nodes to confirm the "
              "same update.",
              true};
    return {true, local_visible ? "OUTPUT ACTIVATION MISSED"
                               : "PROGRAM ACTIVATION MISSED",
            "Waiting for activation to be retried. Renderers that missed "
            "activation hold their last complete image."};
  }
  return {true, "WAITING TO UPDATE PROGRAM",
          "Assets ready. Waiting for synchronized activation.", true};
}

} // namespace mediaserver::editor
