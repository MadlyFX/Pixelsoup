#pragma once

#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mediaserver::assets {
struct AssetRegistry;
}

// The supporting surfaces around the editor's viewports and timeline.
//
// These panels deliberately keep document semantics out of the UI. The scene
// tree reports a selection request, the library reports the kind of item the
// operator wants to create, and the compositor/output monitor are presently
// rich visual models of the intended workflow. The shell remains responsible
// for turning either request into an authored command.
namespace mediaserver::editor {

// A scene-tree row click. Ctrl and Shift both request a toggle so the shell's
// selection model stays consistent with the timeline and viewport.
struct SceneSelectionRequest final {
  doc::NodeId node{};
  bool toggle{};

  bool operator==(const SceneSelectionRequest &) const = default;
};

class ScenePanel final {
public:
  explicit ScenePanel(gui::UiId id) noexcept : id_(id) {}

  [[nodiscard]] std::optional<SceneSelectionRequest>
  draw(gui::UiContext &ctx, const gui::Rect &bounds,
       std::span<const ElementInfo> elements,
       std::span<const doc::NodeId> selection, doc::NodeId primary = {});

  [[nodiscard]] std::string_view search() const noexcept { return search_; }

private:
  gui::UiId id_{};
  std::string search_;
  float scroll_{};
  bool root_open_{true};
  std::array<bool, 3U> group_open_{{true, true, true}};
};

// Intentionally separate from AddKind in editor_shell.hpp: this header is a
// leaf panel API and can therefore be included by the shell without a cycle.
// Media and Model remain only for the transitional draw() wrapper used by the
// current shell. The asset-aware LibraryFrame never returns either through
// `add`; imports and existing-asset instantiation have distinct fields below.
enum class LibraryAddAction : core::u8 {
  Box,
  Plane,
  Sphere,
  Media,
  Model,
  DirectionalLight,
  PointLight,
};

enum class LibraryImportAction : core::u8 {
  Media,
  Model,
};

struct LibraryAssetDrop final {
  doc::NodeId asset{};
  float screen_x{};
  float screen_y{};

  bool operator==(const LibraryAssetDrop &) const = default;
};

// One library interaction. The three intents are deliberately separate:
// importing adds a persistent project asset, while instantiating places an
// already-imported asset in the scene. Generator/light actions continue to
// author scene elements directly.
struct LibraryFrame final {
  std::optional<LibraryAddAction> add;
  std::optional<LibraryImportAction> import_asset;
  std::optional<doc::NodeId> instantiate_asset;
  // Removes only the persistent Library record. Existing scene placements
  // keep their authored source fallback and therefore continue to render.
  std::optional<doc::NodeId> remove_asset;
  // A row dragged beyond the click threshold keeps its capture outside the
  // Library. The shell resolves this screen-space release against Stage so a
  // persistent asset can be placed where it was dropped.
  std::optional<doc::NodeId> dragging_asset;
  std::optional<LibraryAssetDrop> asset_drop;

  bool operator==(const LibraryFrame &) const = default;
};

class LibraryPanel final {
public:
  explicit LibraryPanel(gui::UiId id) noexcept : id_(id) {}

  [[nodiscard]] LibraryFrame draw(gui::UiContext &ctx, const gui::Rect &bounds,
                                  const assets::AssetRegistry &assets);

  // Transitional wrapper for the current shell. It draws an empty registry
  // and maps IMPORT MEDIA/MODEL back to the old Media/Model actions. New
  // integrations should use the frame overload so import and instantiate
  // can never be mistaken for the same operation.
  [[nodiscard]] std::optional<LibraryAddAction> draw(gui::UiContext &ctx,
                                                     const gui::Rect &bounds);

  [[nodiscard]] std::string_view search() const noexcept { return search_; }

private:
  gui::UiId id_{};
  std::string search_;
  float scroll_{};
  std::array<bool, 4U> category_open_{{true, true, true, true}};
  doc::NodeId drag_asset_{};
  float drag_start_x_{};
  float drag_start_y_{};
  bool drag_moved_{};
  std::string drag_label_;
};

struct CompositorAddIntent final {
  compositor::NodeKind kind{compositor::NodeKind::Brightness};
  float editor_x{};
  float editor_y{};
  // SceneLayer binds the currently selected scene element; unused otherwise.
  doc::NodeId scene_node{};

  bool operator==(const CompositorAddIntent &) const = default;
};

struct CompositorMoveIntent final {
  doc::NodeId node{};
  float editor_x{};
  float editor_y{};

  bool operator==(const CompositorMoveIntent &) const = default;
};

struct CompositorConnectIntent final {
  compositor::EdgeEndpoint from;
  compositor::EdgeEndpoint to;
  // Dropping onto an occupied single-input port is an atomic rewire: erase
  // this edge in the same transaction before authoring the new connection.
  std::optional<doc::NodeId> replace_edge;

  bool operator==(const CompositorConnectIntent &) const = default;
};

enum class CompositorProperty : core::u8 {
  BrightnessAmount,
  BlurRadius,
  RadialCount,
  RadialRadius,
  RadialStartAngleTurns,
  RadialCenterX,
  RadialCenterY,
};

using CompositorPropertyValue = std::variant<float, core::u32>;

struct CompositorPropertyEditIntent final {
  doc::NodeId node{};
  CompositorProperty property{CompositorProperty::BrightnessAmount};
  CompositorPropertyValue value{0.0F};

  bool operator==(const CompositorPropertyEditIntent &) const = default;
};

enum class CompositorSelectionKind : core::u8 {
  None,
  Node,
  Edge,
};

struct CompositorSelection final {
  CompositorSelectionKind kind{CompositorSelectionKind::None};
  doc::NodeId node{};

  bool operator==(const CompositorSelection &) const = default;
};

// Edges are first-class document nodes, so disconnecting is the same compact
// remove intent as deleting a node. When the target is a node, the authoring
// boundary should erase incident edge nodes in the same transaction.
struct CompositorDeleteIntent final {
  CompositorSelection target;

  bool operator==(const CompositorDeleteIntent &) const = default;
};

struct CompositorFrame final {
  // Set on every explicit node/edge selection press, including a re-click, so
  // the shell can make Compositor the active Delete-key domain.
  std::optional<CompositorSelection> selection;
  std::optional<CompositorAddIntent> add_node;
  std::optional<CompositorMoveIntent> move_node;
  std::optional<CompositorConnectIntent> connect_ports;
  std::optional<CompositorPropertyEditIntent> property_edit;
  // Emitted by DELETE NODE / DISCONNECT in the graph toolbar.
  std::optional<CompositorDeleteIntent> remove;
  // Delete was handled by this focused panel, so the shell must not also
  // delete the current scene selection.
  bool consume_delete{};
  // True while a numeric field is being scrubbed or typed, allowing the
  // authoring layer to group live values into one undo gesture.
  bool editing{};

  bool operator==(const CompositorFrame &) const = default;
};

// Operator-facing wording for model diagnostics. The graph model deliberately
// carries stable codes rather than UI copy; the panel owns this plain-language
// translation so authoring errors remain understandable without leaking UI
// concerns into evaluation.
[[nodiscard]] std::string_view compositor_diagnostic_message(
    const compositor::GraphDiagnostic &diagnostic) noexcept;

// A headless immediate-mode editor for the persistent compositor graph. The
// panel never mutates the evaluated projection: it reports compact intents for
// the shell/authoring layer to turn into ordinary document commands.
class CompositorPanel final {
public:
  explicit CompositorPanel(gui::UiId id) noexcept : id_(id) {}

  [[nodiscard]] CompositorFrame draw(gui::UiContext &ctx,
                                     const gui::Rect &bounds,
                                     const compositor::CompositorGraph &graph);

  // Transitional read-only adapter for the current shell. It presents each
  // scene element as a Scene Layer node; new integrations should pass the
  // evaluated CompositorGraph to the overload above.
  void draw(gui::UiContext &ctx, const gui::Rect &bounds,
            std::span<const ElementInfo> elements,
            doc::NodeId primary = {}) const;

  [[nodiscard]] CompositorSelection selection() const noexcept {
    return selection_;
  }

  void reset_project_state() noexcept {
    selection_ = {};
    dragging_node_ = {};
    connecting_from_.reset();
    parameter_node_ = {};
    focused_ = false;
    parameter_fields_ = NumberField{};
  }

private:
  gui::UiId id_{};
  CompositorSelection selection_{};
  doc::NodeId dragging_node_{};
  float drag_offset_x_{};
  float drag_offset_y_{};
  float drag_x_{};
  float drag_y_{};
  std::optional<compositor::EdgeEndpoint> connecting_from_;
  compositor::PortValueType connecting_type_{compositor::PortValueType::Image};
  doc::NodeId parameter_node_{};
  bool focused_{};
  NumberField parameter_fields_;
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
    std::string content_detail;
    SyncStatus sync{SyncStatus::Unsynced};
    core::u32 sync_locked_outputs{};
    core::u32 sync_expected_outputs{};
    core::u32 barrier_joined_outputs{};
    core::u32 barrier_expected_outputs{};
    core::u64 missed_frames{};
    core::u64 late_frames{};
    core::u64 held_frames{};
    core::u64 repeated_frames{};
    core::u64 loss_of_sync_events{};
    std::string sync_detail;

    [[nodiscard]] bool content_ready() const noexcept {
      return content == ContentStatus::NotRequired ||
             content == ContentStatus::Ready;
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
    core::u64 heartbeat_timeouts{};
    core::u64 session_failures{};
    core::u64 barrier_rejections{};
    core::u64 present_failures{};
    bool diagnostics_recording{};
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

    bool operator==(const Cluster &) const = default;
  } cluster{};
};

enum class OutputLifecycleAction : core::u8 {
  Status,
  Start,
  Restart,
  Stop,
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
  std::optional<OutputLifecycleRequest> lifecycle;
};

// The Program row is local render status. Render-server rows are explicit,
// document-bound lifecycle targets; drawing only emits one fixed action intent
// and never performs network work on the UI thread.
class OutputMonitorPanel final {
public:
  explicit OutputMonitorPanel(gui::UiId id) noexcept : id_(id) {}

  [[nodiscard]] OutputMonitorFrame
  draw(gui::UiContext &ctx, const gui::Rect &bounds,
       const OutputMonitorState &state);

private:
  gui::UiId id_{};
  float scroll_{};
};

// Machine-local settings the operator would otherwise pass on the command
// line. These are not document state: nothing here is saved into a project or
// replicated, so editing them authors no command.
//
// Every field takes effect when the process starts. The panel says so rather
// than implying a live change, and a command-line argument given this run
// still wins, so a scripted launch stays exactly reproducible.
struct EditorPreferences final {
  std::string director_listen;
  std::string show_id;
  std::string director_epoch;
  std::string lifecycle_secret_file;
  std::string wal_path;

  bool operator==(const EditorPreferences &) const = default;
};

// What the panel shows: the stored values it edits, what this run is actually
// using, and whether a command-line argument overrode each field.
struct EditorPreferencesState final {
  EditorPreferences stored;
  EditorPreferences active;
  bool director_listen_overridden{};
  bool show_id_overridden{};
  bool director_epoch_overridden{};
  bool lifecycle_secret_file_overridden{};
  bool wal_path_overridden{};
  // Result of the last save attempt, shown in the panel. Empty means nothing
  // has been attempted this session.
  std::string message;
};

struct PreferencesFrame final {
  // Present only on the frame the operator pressed Save.
  std::optional<EditorPreferences> save;
};

// The document half of the same idea: how this project's effects are computed.
//
// Split from Preferences deliberately. Preferences describe the machine and are
// never replicated; these change the picture, so they are an ordinary document
// edit that undoes, saves, and reaches every render node. Nothing here belongs
// beside an individual effect in the Inspector - a kernel width is set once per
// show and then left alone, and putting it on every blur node would be five
// controls an operator scrolls past forever.
struct ProjectSettingsState final {
  scene::EffectSettings settings{};
  // The document node the values came from, or zero when the project has none
  // and the authoring boundary must create one.
  doc::NodeId node{};
};

struct ProjectSettingsFrame final {
  // Present only on the frame the operator pressed Apply.
  std::optional<scene::EffectSettings> apply;
};

class ProjectSettingsPanel final {
public:
  explicit ProjectSettingsPanel(gui::UiId id) noexcept : id_(id) {}

  void open(const scene::EffectSettings &settings);
  void close() noexcept { open_ = false; }
  [[nodiscard]] bool is_open() const noexcept { return open_; }

  // `bounds` is the whole window, as for Preferences: this is a modal card and
  // the caller must have denied the pointer to everything drawn before it.
  [[nodiscard]] ProjectSettingsFrame draw(gui::UiContext &ctx,
                                          const gui::Rect &bounds,
                                          const ProjectSettingsState &state);

private:
  gui::UiId id_{};
  bool open_{};
  scene::EffectSettings draft_{};
};

// The Preferences dialog: modal, editor-local, and authoring nothing.
//
// It owns its draft so cancelling really discards, and it dims and covers the
// whole window. The scrim is hit-tested after every control in the card,
// because gui::UiContext resolves a frame's pointer first-wins: a full-screen
// grab registered first would swallow the clicks meant for the card. Callers
// must also deny the pointer to everything drawn earlier in the frame - see
// gui::UiContext::push_pointer_block - or the workspace behind stays live.
class PreferencesPanel final {
public:
  explicit PreferencesPanel(gui::UiId id) noexcept : id_(id) {}

  void open(const EditorPreferences &stored);
  void close() noexcept;
  [[nodiscard]] bool is_open() const noexcept { return open_; }
  [[nodiscard]] const EditorPreferences &draft() const noexcept {
    return draft_;
  }

  // `bounds` is the whole window. Returns a save request when the operator
  // pressed Save; closing is reported through is_open().
  [[nodiscard]] PreferencesFrame draw(gui::UiContext &ctx,
                                      const gui::Rect &bounds,
                                      const EditorPreferencesState &state);

private:
  gui::UiId id_{};
  bool open_{};
  EditorPreferences draft_;
};

} // namespace mediaserver::editor
