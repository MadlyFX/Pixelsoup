#pragma once

// The Output Monitor: Program and render-node status and lifecycle.

#include <mediaserver/core/frame_timing.hpp>
#include <mediaserver/core/present_failure.hpp>
#include <mediaserver/core/render_conformance.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/gui/form_layout.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/render/display_lut_cache.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>

#include <memory>
#include <optional>
#include <span>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace mediaserver::editor {

// A render node's recent log, as its machine-local lifecycle agent last read
// it from disk. The agent outlives the render process, so this is available
// while the node is hung, out of lockstep, reconnecting or stopped. Immutable
// once published: rows share it by pointer rather than copying lines each
// frame.
struct OutputNodeLog final {
  enum class Status : core::u8 {
    // Nothing has answered yet.
    Fetching,
    Available,
    // The agent keeps no log, or the render process never ran there.
    Unavailable,
    Failed,
  };
  Status status{Status::Fetching};
  // Oldest first. The first previous_run_lines came from the run before the
  // current one - after a crash or restart, how it ended.
  std::vector<std::string> lines;
  std::size_t previous_run_lines{};
  // Bytes the render process dropped rather than wait for its disk.
  core::u64 dropped_bytes{};
  bool truncated{};
  // Where the agent read it, or why it could not.
  std::string detail;
  // Local wall-clock time of the answer, "14:03:27".
  std::string fetched_at;
};

struct OutputMonitorState final {
  core::u64 frame{};
  core::u32 width{1920U};
  core::u32 height{1080U};
  float frames_per_second{60.0F};
  bool playing{};
  struct Node final {
    enum class ContentStatus : core::u8 {
      NotRequired,
      Checking,
      Ready,
      Missing,
      Error,
    };

    // How this output is presenting physically. Every value is a transient
    // observation; none is ever written back to the project.
    enum class SyncStatus : core::u8 {
      // The project makes no physical timing claim.
      Unsynced,
      // Nothing has reported, or this build/platform cannot query the sync
      // device. Never treated as evidence of lock.
      Unverified,
      SyncDeviceMissing,
      ReferenceLost,
      ReferenceNotInUse,
      Unlocked,
      BarrierIncomplete,
      // A formerly joined client left, or the driver's own in-sync present
      // counter fell behind the present counter.
      BarrierLost,
      ShowClockReferenceUnavailable,
      ShowClockReferenceLost,
      // Locked and joined, but the evidence came from a simulated provider.
      Simulated,
      Locked,
    };

    doc::NodeId device{};
    std::string name;
    std::string endpoint;
    core::u32 node_index{};
    core::u32 width{1920U};
    core::u32 height{1080U};
    enum class Status : core::u8 {
      Disabled,
      InvalidConfiguration,
      DuplicateNodeIndex,
      Probing,
      Stopped,
      Starting,
      Running,
      Stopping,
      Faulted,
      Unreachable,
      AuthenticationFailed,
      ProtocolError,
      ConfigurationMismatch,
      Busy,
    } status{Status::Disabled};
    core::u64 generation{};
    std::string detail;
    // Transient content readiness reported by the machine-local lifecycle
    // agent for the currently required project manifest. None of these fields
    // are persisted in the project or allowed to rewrite authored assets.
    ContentStatus content{ContentStatus::NotRequired};
    core::u32 assets_required{};
    core::u32 assets_ready{};
    core::u32 assets_missing{};
    bool missing_media_only{};
    core::u32 assets_mismatched{};
    core::u32 assets_verification_failed{};
    std::string content_detail;
    enum class Connection : core::u8 { Disconnected, CatchingUp, Live };
    Connection connection{Connection::Disconnected};
    core::u64 committed_frame{};
    core::u64 target_frame{};
    SyncStatus sync{SyncStatus::Unsynced};
    core::u32 sync_locked_outputs{};
    core::u32 sync_expected_outputs{};
    core::u32 barrier_joined_outputs{};
    core::u32 barrier_expected_outputs{};
    core::u64 barrier_presents{};
    core::u64 barrier_presents_in_sync{};
    // The hardware source currently advancing this node's ShowClock. This is
    // deliberately shown beside barrier membership: a joined barrier alone
    // does not establish camera-reference timing.
    std::string_view show_clock_reference{"UNKNOWN"};
    core::u32 show_clock_reference_frame{};
    core::u64 missed_frames{};
    core::u64 late_frames{};
    core::u64 held_frames{};
    core::u64 repeated_frames{};
    core::u64 loss_of_sync_events{};
    std::string sync_detail;
    core::RenderConformance conformance{};
    core::FrameTiming frame_timing{};
    core::u32 frame_budget_us{};
    // Whether this node opened the physical outputs its bindings name. A node
    // that could not has no presenters, so everything downstream of it -
    // renderer evidence, asset readiness, the Program gate - reports the
    // honest default and reads as "still loading". This is the one field that
    // distinguishes the two, so it is shown on the row rather than inferred.
    //
    // Mirrors the runtime vocabulary rather than borrowing it, the way
    // SyncStatus above does: a panel describes what an operator sees, and the
    // editor does not depend on the session protocol.
    enum class OutputStatus : core::u8 {
      // Nothing reported. Never evidence that the outputs opened.
      Unknown,
      Deployed,
      InventoryUnavailable,
      InvalidMode,
      DisplayMissing,
      DisplayInactive,
      DisplayDuplicated,
      DisplayMirrored,
      DuplicateRenderServer,
      PresenterRefused,
      NoBoundDisplay,
    } output{OutputStatus::Unknown};

    // The last log fetched for this node, or null when none was asked for.
    std::shared_ptr<const OutputNodeLog> log;
    // A log request is on its way.
    bool log_fetching{};
    // Frames this node failed to draw, and where the latest one failed. A
    // node that keeps failing holds its last picture, which from the wall
    // looks like a stall; this is what says why.
    core::PresentFailure present_failure{};
    // The failed map's operator name, resolved from the feed map.
    std::string present_failure_map;

    [[nodiscard]] bool frames_failing() const noexcept {
      return present_failure.ongoing;
    }

    [[nodiscard]] bool output_failed() const noexcept {
      return output != OutputStatus::Unknown &&
             output != OutputStatus::Deployed;
    }

    [[nodiscard]] bool content_ready() const noexcept {
      return content == ContentStatus::NotRequired ||
             content == ContentStatus::Ready || missing_media_only;
    }
  };
  // Borrowed only for draw(); the runtime coordinator owns the value-safe row
  // snapshot and the shell never retains it past the frame.
  std::span<const Node> nodes{};
  struct Cluster final {
    enum class Health : core::u8 {
      Unmonitored,
      Healthy,
      Degraded,
      Faulted,
    } health{Health::Unmonitored};
    core::u32 configured_nodes{};
    core::u32 live_sessions{};
    core::u32 catching_up_sessions{};
    core::u64 release_lag_frames{};
    core::u64 pacer_lag_frames{};
    // The Director is sealing no control frames, so edits are not applied.
    bool control_stalled{};
    core::u64 heartbeat_timeouts{};
    core::u64 session_failures{};
    core::u64 barrier_rejections{};
    // Nodes taken out of lockstep for holding every output back too long.
    core::u64 lockstep_demotions{};
    core::u64 present_failures{};
    bool diagnostics_recording{};
    // Render machines are controlled with no password: any Pixelsoup on
    // the network could start and stop them too.
    bool control_unsecured{};
    // These remain false until an explicit platform/vendor implementation
    // measures the corresponding physical timing fact.
    bool external_time_sync_verified{};
    bool hardware_frame_lock_verified{};
    // The project synchronization policy and what the cluster can actually
    // prove about it. `sync_headline` is the exact operator wording produced
    // by the readiness gate - SYNC UNVERIFIED, REFERENCE LOST, SWAP BARRIER
    // INCOMPLETE - and is never replaced by a generic online state.
    std::string_view sync_mode{"UNSYNCED"};
    std::string_view sync_headline{"SYNC POLICY UNSYNCED"};
    bool sync_required{};
    bool sync_satisfied{true};
    bool sync_degraded{};
    core::u32 sync_groups{};
    core::u32 sync_locked_outputs{};
    core::u32 sync_expected_outputs{};
    core::u32 barrier_joined_outputs{};
    // Authored-topology problems that block a physical policy before any
    // measurement is consulted.
    core::u32 sync_configuration_issues{};

    // Renderer cohort truth is carried separately from physical sync. The
    // Output Monitor shows it on the existing control line, and only names a
    // node when that node is the first condition holding Program.
    std::string_view renderer_headline{"RENDERER INACTIVE"};
    bool renderer_active{};
    bool renderer_satisfied{true};
    bool renderer_fallback{};
    bool renderer_selected_advanced{};
    bool renderer_failure_local{};
    core::u32 renderer_failure_node{};
    // When the failing renderer is short of GPU memory: what it still needs
    // and what its OS budget leaves free. Zero otherwise.
    core::u64 renderer_memory_needed{};
    core::u64 renderer_memory_free{};
    // The editor's own preview is short of GPU memory. It holds nothing,
    // because the render nodes draw Program, so it is reported apart.
    core::u64 preview_memory_needed{};
    core::u64 preview_memory_free{};
    bool conformance_active{};
    core::RenderConformance conformance{};
    core::FrameTiming frame_timing{};
    core::u32 frame_budget_us{};
    core::ConformanceComparison conformance_difference{};
    core::AssetCohort asset_cohort{};
    std::optional<core::u32> asset_waiting_node;

    bool operator==(const Cluster &) const = default;
  } cluster{};
};

struct StageAssetStatus final {
  bool loading{};
  std::string headline;
  std::string detail;
};

// Stage's own preparation evidence, independent of Program's asset cohort.
[[nodiscard]] StageAssetStatus stage_asset_status(
    const render::RenderStats& geometry, core::u64 pending_media,
    render::DisplayLutState display);

struct ProgramAssetStatus final {
  bool waiting{};
  std::string headline;
  std::string detail;
  // The ordinary hand-over every asset change goes through - loading, the
  // scheduled switch, outputs confirming it. Only a missed activation is not.
  // The Program monitor names routine waits in its footer and raises a
  // banner only once one lasts long enough to need the operator.
  bool routine{};
};

// Uses publication evidence, independently of renderer capability readiness.
[[nodiscard]] ProgramAssetStatus program_asset_status(
    core::AssetSetReadiness local, core::AssetCommit permit,
    core::u64 control_frame, bool required,
    const OutputMonitorState::Cluster& cluster, std::string_view loading_detail);

// Pure operator wording for one node's machine-local content readiness. Kept
// public so lifecycle tests can prove missing files, stale revisions, and local
// verification failures remain distinguishable without inspecting glyphs.
[[nodiscard]] std::string
output_node_content_label(const OutputMonitorState::Node &node);

[[nodiscard]] std::string_view
output_node_connection_label(const OutputMonitorState::Node &node) noexcept;

// Why this node is not driving the displays its bindings name, in the words
// an operator would use to go and fix it. Empty when nothing is wrong or
// nothing has been reported. Kept public so tests can prove a display fault
// stays distinguishable from an asset one without inspecting glyphs.
[[nodiscard]] std::string_view
output_node_deployment_label(const OutputMonitorState::Node &node) noexcept;

// Where this node's latest failed frame failed and why, naming the map:
// "Drawing Wall - DP1.2, viewport 2 failed: ...". Empty when no frame has
// failed. Public so tests can check the wording without inspecting glyphs.
[[nodiscard]] std::string
output_node_failure_text(const OutputMonitorState::Node &node);

// One line on what an open node log shows: when it was read and how much of
// it is the previous run, or why there is none. Public so tests can check the
// wording without inspecting glyphs.
[[nodiscard]] std::string
output_node_log_summary(const OutputMonitorState::Node &node);

enum class OutputLifecycleAction : core::u8 {
  Status,
  Start,
  Restart,
  Stop,
  // Read the node's recent log from its lifecycle agent. Changes nothing.
  FetchLog,
};

struct OutputLifecycleRequest final {
  doc::NodeId device{};
  core::u32 node_index{};
  std::string endpoint;
  OutputLifecycleAction action{OutputLifecycleAction::Status};
  // A cluster action is expanded by the background controller from the latest
  // valid, unique render-server targets. No stale row list is captured here.
  bool all{};

  bool operator==(const OutputLifecycleRequest &) const = default;
};

struct OutputMonitorFrame final {
  bool check_render_conformance{};
  std::optional<OutputLifecycleRequest> lifecycle;
};

// The Program row is local render status. Render-server rows are explicit,
// document-bound lifecycle targets; drawing only emits one fixed action intent
// and never performs network work on the UI thread.
//
// Laid out as a table: a strip of status chips for the cluster, then one row
// per output - name, resolution, rate, a status pill, assets and sync where
// they apply, and its lifecycle buttons - with everything longer behind a
// click that opens the row's details.
class OutputMonitorPanel final {
public:
  explicit OutputMonitorPanel(gui::UiId id) noexcept : id_(id) {}

  [[nodiscard]] OutputMonitorFrame draw(gui::UiContext &ctx,
                                        const gui::Rect &bounds,
                                        const OutputMonitorState &state);

  // The table's fixed geometry, from the top of the panel.
  static constexpr float summary_height = 30.0F;
  static constexpr float header_height = 22.0F;
  static constexpr float row_height = 32.0F;
  static constexpr float footer_height = 32.0F;
  // An open log: this many lines of this height are in view at once, and it
  // asks its agent again this often while it stays open.
  static constexpr float log_line_height = 16.0F;
  static constexpr std::size_t log_visible_lines = 14U;
  static constexpr double log_refresh_seconds = 3.0;

private:
  // A row whose log is open: its scroll position, the log it last drew (to
  // follow new lines while the operator is at the bottom), and when it next
  // asks for fresh lines.
  struct LogView final {
    gui::ScrollRegion scroll;
    std::weak_ptr<const OutputNodeLog> seen;
    double refresh_at{-1.0};
  };
  std::map<doc::NodeId, LogView> log_open_;
  // Where a log was drawn last frame. The table does not scroll while the
  // pointer is over it, so the wheel moves the log instead.
  gui::Rect log_hover_{};
  bool conformance_expanded_{};
  bool timing_expanded_{};
  bool program_expanded_{};
  // Until when Stop all is armed by its first press; a second press before
  // then stops every output.
  double stop_all_armed_until_{-1.0};
  std::set<doc::NodeId> expanded_;
  gui::UiId id_{};
  gui::ScrollRegion scroll_;
};

} // namespace mediaserver::editor
