#pragma once

#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/assets/proxy_policy.hpp>
#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/editor/selection.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/rhi/device.hpp>

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

// A scene-tree selection gesture. Shift range requests carry the exact
// filtered/expanded element-row order the operator saw; the shell remains the
// sole owner of canonical selection state.
struct SceneSelectionRequest final {
  doc::NodeId node{};
  SelectionAction action{SelectionAction::Replace};
  std::vector<doc::NodeId> visible_order;
  bool lock_changed{};
  bool locked{};
  enum class StructureAction : core::u8 {
    Group,
    Ungroup,
  };
  std::optional<StructureAction> structure;

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
  std::array<bool, 4U> group_open_{{true, true, true, true}};
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

// Runtime-only projection of the app/server thumbnail service. These values
// are observations, never document state and never an alternate Asset node.
enum class LibraryThumbnailState : core::u8 {
  Placeholder,
  Queued,
  Generating,
  Ready,
  Stale,
  Failed,
};

struct LibraryThumbnailObservation final {
  // Contains the persistent Asset NodeId plus its expected content fingerprint
  // and the frozen thumbnail profile/generator versions.
  assets::DerivativeKey key;
  LibraryThumbnailState state{LibraryThumbnailState::Placeholder};
  // Borrowed for this draw only. Ready without a texture remains a typed Ready
  // status while the app finishes its CPU-to-GPU upload.
  const rhi::Texture *texture{};

  bool operator==(const LibraryThumbnailObservation &) const = default;
};

// Machine-local playback truth for one persistent Library MediaAsset. The
// optional key names the exact requested source revision/profile/generator;
// it remains a derivative cache key and never becomes another Asset identity.
enum class LibraryProxyRuntimeState : core::u8 {
  Source,
  ProxyReady,
  PreferFallback,
  RequiredUnready,
  Failed,
};

// Editor-owned mirror of app/server failures. Keeping this type in the editor
// prevents the reusable panel API from depending on the executable layer.
enum class LibraryProxyRuntimeIssue : core::u8 {
  None,
  MissingAssetIdentity,
  AssetNotFound,
  AssetKindMismatch,
  PlacementSourceMismatch,
  AvailabilityMissing,
  CacheInspectionFailed,
  ArtifactLoadFailed,
  ArtifactMetadataMismatch,
  FrameMappingFailed,
  MaterializationFailed,
};

struct LibraryProxyRuntimeObservation final {
  doc::NodeId asset{};
  std::optional<assets::DerivativeKey> key;
  LibraryProxyRuntimeState state{LibraryProxyRuntimeState::Source};
  assets::ProxyPlaybackReason reason{assets::ProxyPlaybackReason::SourcePolicy};
  LibraryProxyRuntimeIssue issue{LibraryProxyRuntimeIssue::None};

  bool operator==(const LibraryProxyRuntimeObservation &) const = default;
};

// Stable, concise text used by the Library row and by non-visual tests. The
// authored SRC/AUTO/REQ control remains separate from this runtime status.
[[nodiscard]] std::string
library_proxy_runtime_status(const LibraryProxyRuntimeObservation &observation);

enum class LibraryThumbnailRequestPriority : core::u8 {
  Visible,
  Prefetch,
};

struct LibraryThumbnailRequestIntent final {
  assets::DerivativeKey key;
  LibraryThumbnailRequestPriority priority{
      LibraryThumbnailRequestPriority::Visible};

  bool operator==(const LibraryThumbnailRequestIntent &) const = default;
};

// The exact desired set for this frame. Applying it to the service's retain()
// API makes cancellation explicit when filtered, collapsed, or scrolled rows
// leave both the visible region and bounded prefetch window.
struct LibraryThumbnailRetainIntent final {
  std::vector<assets::DerivativeKey> keys;

  bool operator==(const LibraryThumbnailRetainIntent &) const = default;
};

struct LibraryThumbnailWindow final {
  core::u32 prefetch_asset_rows{4U};
  core::u32 maximum_request_intents{16U};

  bool operator==(const LibraryThumbnailWindow &) const = default;
};

inline constexpr core::u32 maximum_library_thumbnail_prefetch_rows = 32U;
inline constexpr core::u32 maximum_library_thumbnail_request_intents = 64U;

struct LibraryAssetDrop final {
  doc::NodeId asset{};
  float screen_x{};
  float screen_y{};

  bool operator==(const LibraryAssetDrop &) const = default;
};

// One explicit authored proxy preference change. Availability and generation
// remain machine-local observations; this carries only the profile request and
// Source/Prefer/Require policy stored on the existing MediaAsset node.
struct LibraryProxyPolicyIntent final {
  doc::NodeId asset{};
  std::optional<assets::DerivativeProfileId> requested_profile;
  assets::ProxyPlaybackPolicy policy{assets::ProxyPlaybackPolicy::Source};

  bool operator==(const LibraryProxyPolicyIntent &) const = default;
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
  std::optional<LibraryProxyPolicyIntent> proxy_policy;
  // Look at the sources on disk again, reading their bytes. Nothing does this
  // on a timer, so a file that arrived after the project opened is noticed
  // when the operator says to look.
  bool rescan_assets{};
  std::vector<LibraryThumbnailRequestIntent> thumbnail_requests;
  // Disengaged on the source-compatible draw overload. Engaged, including an
  // empty key vector, on every thumbnail-aware draw.
  std::optional<LibraryThumbnailRetainIntent> thumbnail_retain;

  bool operator==(const LibraryFrame &) const = default;
};

class LibraryPanel final {
public:
  explicit LibraryPanel(gui::UiId id) noexcept : id_(id) {}

  [[nodiscard]] LibraryFrame draw(gui::UiContext &ctx, const gui::Rect &bounds,
                                  const assets::AssetRegistry &assets);

  [[nodiscard]] LibraryFrame
  draw(gui::UiContext &ctx, const gui::Rect &bounds,
       const assets::AssetRegistry &assets,
       std::span<const LibraryThumbnailObservation> thumbnails,
       LibraryThumbnailWindow window = {});

  [[nodiscard]] LibraryFrame
  draw(gui::UiContext &ctx, const gui::Rect &bounds,
       const assets::AssetRegistry &assets,
       std::span<const LibraryThumbnailObservation> thumbnails,
       LibraryThumbnailWindow window,
       std::span<const LibraryProxyRuntimeObservation> proxy_runtime);

  // Transitional wrapper for the current shell. It draws an empty registry
  // and maps IMPORT MEDIA/MODEL back to the old Media/Model actions. New
  // integrations should use the frame overload so import and instantiate
  // can never be mistaken for the same operation.
  [[nodiscard]] std::optional<LibraryAddAction> draw(gui::UiContext &ctx,
                                                     const gui::Rect &bounds);

  [[nodiscard]] std::string_view search() const noexcept { return search_; }

private:
  [[nodiscard]] LibraryFrame
  draw_impl(gui::UiContext &ctx, const gui::Rect &bounds,
            const assets::AssetRegistry &assets,
            std::span<const LibraryThumbnailObservation> thumbnails,
            const LibraryThumbnailWindow *thumbnail_window,
            std::span<const LibraryProxyRuntimeObservation> proxy_runtime);

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
  SaturationAmount,
  LevelsMasterInputBlack,
  LevelsMasterInputWhite,
  LevelsMasterGamma,
  LevelsMasterOutputBlack,
  LevelsMasterOutputWhite,
  LevelsRedInputBlack,
  LevelsRedInputWhite,
  LevelsRedGamma,
  LevelsRedOutputBlack,
  LevelsRedOutputWhite,
  LevelsGreenInputBlack,
  LevelsGreenInputWhite,
  LevelsGreenGamma,
  LevelsGreenOutputBlack,
  LevelsGreenOutputWhite,
  LevelsBlueInputBlack,
  LevelsBlueInputWhite,
  LevelsBlueGamma,
  LevelsBlueOutputBlack,
  LevelsBlueOutputWhite,
  LiftGammaGainLiftR,
  LiftGammaGainLiftG,
  LiftGammaGainLiftB,
  LiftGammaGainGammaR,
  LiftGammaGainGammaG,
  LiftGammaGainGammaB,
  LiftGammaGainGainR,
  LiftGammaGainGainG,
  LiftGammaGainGainB,
  ContrastPivotContrast,
  ContrastPivotPivot,
  ColorMatrixM00,
  ColorMatrixM01,
  ColorMatrixM02,
  ColorMatrixM10,
  ColorMatrixM11,
  ColorMatrixM12,
  ColorMatrixM20,
  ColorMatrixM21,
  ColorMatrixM22,
  ColorMatrixOffsetR,
  ColorMatrixOffsetG,
  ColorMatrixOffsetB,
  WhiteBalanceTemperatureKelvin,
  WhiteBalanceTint,
  CurvesParameters,
  FilmGrainAmount,
  FilmGrainPixelScale,
  FilmGrainChromaRatio,
  FilmGrainSeed,
  FilmGrainMode,
  BlurRadius,
  BoxBlurRadius,
  BoxBlurPasses,
  DirectionalBlurLength,
  DirectionalBlurAngleTurns,
  RadialCount,
  RadialRadius,
  RadialStartAngleTurns,
  RadialCenterX,
  RadialCenterY,
  LedCarPositionX,
  LedCarPositionY,
  LedCarPositionZ,
  LedCarYawTurns,
  LedCarPitchTurns,
  LedCarRollTurns,
  LedCarPlateScale,
  LedCarArrayScale,
  LedCarReflectionHeight,
  LedCarReflectionScale,
  LedCarHorizontalBlend,
};

using CompositorPropertyValue =
    std::variant<float, core::u32, compositor::CurvesParameters>;

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
  std::optional<NumericNudge> numeric_nudge;
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
[[nodiscard]] std::string_view compositor_diagnostic_severity_label(
    const compositor::GraphDiagnostic &diagnostic) noexcept;

// Cross-panel drag feedback for Effects and Timeline rows. The graph canvas is
// a valid drop target while either source is lifted; the stronger hovered
// state makes the eventual add location explicit before release.
void draw_compositor_drop_target(gui::UiContext &ctx, const gui::Rect &bounds,
                                 std::string_view item_name);

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

  // Converts a screen-space drop into the graph-space top-left needed by a
  // newly created node, centring the node under the pointer. Effects and the
  // Timeline use this so cross-panel drops agree with the visible pan/zoom.
  [[nodiscard]] std::optional<gui::UiPoint>
  node_drop_position(float screen_x, float screen_y, float node_width,
                     float node_height) const noexcept {
    if (!last_canvas_.contains(screen_x, screen_y) || zoom_ <= 0.0F) {
      return std::nullopt;
    }
    return gui::UiPoint{
        (screen_x - last_canvas_.x - pan_x_) / zoom_ - node_width * 0.5F,
        (screen_y - last_canvas_.y - pan_y_) / zoom_ - node_height * 0.5F};
  }

  [[nodiscard]] float graph_zoom() const noexcept { return zoom_; }
  [[nodiscard]] gui::UiPoint graph_pan() const noexcept {
    return gui::UiPoint{pan_x_, pan_y_};
  }

  void reset_project_state() noexcept {
    selection_ = {};
    dragging_node_ = {};
    connecting_from_.reset();
    parameter_node_ = {};
    focused_ = false;
    add_menu_open_ = false;
    add_category_.reset();
    node_search_.clear();
    parameter_fields_ = NumberField{};
    pan_x_ = 0.0F;
    pan_y_ = 0.0F;
    zoom_ = 1.0F;
    panning_ = false;
    last_canvas_ = {};
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
  bool add_menu_open_{};
  std::optional<core::u8> add_category_;
  std::string node_search_;
  NumberField parameter_fields_;
  float pan_x_{};
  float pan_y_{};
  float zoom_{1.0F};
  bool panning_{};
  gui::Rect last_canvas_{};
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
    core::u32 assets_mismatched{};
    core::u32 assets_verification_failed{};
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

// Pure operator wording for one node's machine-local content readiness. Kept
// public so lifecycle tests can prove missing files, stale revisions, and local
// verification failures remain distinguishable without inspecting glyphs.
[[nodiscard]] std::string
output_node_content_label(const OutputMonitorState::Node &node);

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

  [[nodiscard]] OutputMonitorFrame draw(gui::UiContext &ctx,
                                        const gui::Rect &bounds,
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
