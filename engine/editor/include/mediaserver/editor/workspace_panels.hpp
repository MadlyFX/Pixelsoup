#pragma once

#include <mediaserver/editor/file_picker.hpp>
#include <mediaserver/core/frame_timing.hpp>
#include <mediaserver/core/render_conformance.hpp>

#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/assets/proxy_policy.hpp>
#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/editor/selection.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/render/display_lut_cache.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>
#include <mediaserver/rhi/device.hpp>

#include <array>
#include <optional>
#include <span>
#include <set>
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

enum class SceneSettingsTarget : core::u8 { None, Renderer, Environment };

// A scene-tree selection gesture. Shift range requests carry the exact
// filtered/expanded element-row order the operator saw; the shell remains the
// sole owner of canonical selection state.
struct SceneSelectionRequest final {
  doc::NodeId node{};
  SelectionAction action{SelectionAction::Replace};
  std::vector<doc::NodeId> visible_order;
  bool lock_changed{};
  bool locked{};
  // The eye: the element's own visibility, the same bit the Timeline's eye
  // writes.
  bool visibility_changed{};
  bool visible{true};
  enum class StructureAction : core::u8 {
    Group,
    Ungroup,
    // File `moved` under `destination` (zero: the top level), keeping each
    // element where it is on the stage. When `reorder_destination` is set the
    // moved rows land beside that sibling rather than at the front.
    MoveInto,
  };
  std::optional<StructureAction> structure;
  doc::NodeId destination{};
  std::vector<doc::NodeId> moved;
  // A drag between two siblings: `node` lands before or after
  // `reorder_destination` in the front-to-back stack.
  doc::NodeId reorder_destination{};
  bool reorder_before{};
  // `node`'s new name, committed from the inline field.
  std::optional<std::string> rename;
  // The context menu's Delete: the shell deletes the current selection.
  bool delete_selection{};
  SceneSettingsTarget settings{SceneSettingsTarget::None};
  // `node` is an LED surface rather than a scene element: a plain click
  // selects it on Stage, and `lock_changed` sets its Feed Mapping lock.
  bool surface{};

  bool operator==(const SceneSelectionRequest &) const = default;
};

// The Scene outliner: the scene's real hierarchy - groups and Blender
// collections holding their children - with the same eye, padlock, icons and
// names the Timeline's layer list uses. Rows rename in place (double-click or
// F2), drag to reorder among siblings or onto a group to file under it, and
// carry a right-click menu with everything else a row can do. The stage's LED
// surfaces follow the scene's elements, as they do in the Timeline: physical
// geometry rather than scene nodes, so they carry a padlock but no eye and
// never file into the hierarchy.
class ScenePanel final {
public:
  explicit ScenePanel(gui::UiId id) noexcept
      : id_(id), rename_(id ^ 0x52454E414D455343ULL) {}

  [[nodiscard]] std::optional<SceneSelectionRequest>
  draw(gui::UiContext &ctx, const gui::Rect &bounds,
       std::span<const ElementInfo> elements,
       std::span<const doc::NodeId> selection, doc::NodeId primary = {},
       bool show_settings = false,
       SceneSettingsTarget settings_selection = SceneSettingsTarget::None,
       std::span<const output::ProjectionSurface> surfaces = {},
       doc::NodeId selected_surface = {});

  // Opens `element`'s name for editing, as F2 does.
  void begin_rename(gui::UiContext &ctx, const ElementInfo &element);
  [[nodiscard]] bool renaming() const noexcept { return rename_.active(); }

  [[nodiscard]] std::string_view search() const noexcept { return search_; }

private:
  gui::UiId id_{};
  std::string search_;
  float scroll_{};
  // Branches the operator closed. Everything starts open; selecting
  // something inside a closed branch opens it again.
  std::set<doc::NodeId> collapsed_;
  doc::NodeId revealed_{};
  InlineRename rename_;
  ClickTracker clicks_;
  // The outliner was the last panel clicked, so F2 is meant for it.
  bool focused_{};
  doc::NodeId drag_node_{};
  float drag_x_{};
  float drag_y_{};
  bool dragging_{};
  // The open context menu's row and entries, fixed when it opened so what
  // the operator reads is what a choice does.
  doc::NodeId menu_node_{};
  std::vector<std::string> menu_entries_;
  std::vector<core::u8> menu_commands_;
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
  SpotLight,
  Camera,
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

struct LibraryProxyTranscodeProgress final {
  core::u64 completed_frames{};
  core::u64 total_frames{};
  // A serialized proxy worker can have several visible jobs waiting behind
  // the active encode. Queued jobs retain a zero-width determinate track and
  // an explicit label instead of pretending that encoding has begun.
  bool queued{};

  bool operator==(const LibraryProxyTranscodeProgress &) const = default;
};

enum class LibraryMediaDecodeState : core::u8 {
  Unknown,
  HardwareAccelerated,
  CpuOnly,
  Unavailable,
};

struct LibraryProxyRuntimeObservation final {
  doc::NodeId asset{};
  std::optional<assets::DerivativeKey> key;
  LibraryProxyRuntimeState state{LibraryProxyRuntimeState::Source};
  assets::ProxyPlaybackReason reason{assets::ProxyPlaybackReason::SourcePolicy};
  LibraryProxyRuntimeIssue issue{LibraryProxyRuntimeIssue::None};
  std::optional<LibraryProxyTranscodeProgress> transcode_progress;
  LibraryMediaDecodeState media_decode{LibraryMediaDecodeState::Unknown};

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
    core::u32 assets_mismatched{};
    core::u32 assets_verification_failed{};
    std::string content_detail;
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
    // Render machines are controlled with no password: any Media Server on
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

private:
  bool conformance_expanded_{};
  bool timing_expanded_{};
  bool program_expanded_{};
  std::set<doc::NodeId> expanded_;
  gui::UiId id_{};
  float scroll_{};
};

// Machine-local settings the operator would otherwise pass on the command
// line. These are not document state: nothing here is saved into a project or
// replicated, so editing them authors no command.
//
// Every field takes effect when the process starts - except the render node
// password, which applies at once. The panel says which is which rather than
// implying a live change, and a command-line argument given this run still
// wins, so a scripted launch stays exactly reproducible.
struct EditorPreferences final {
  enum class TranscodePriority : core::u8 {
    Background,
    Balanced,
    Fast,
  };

  std::string director_listen;
  std::string show_id;
  std::string director_epoch;
  // Whether a render node password is set. The password itself never comes
  // back to the panel: it is kept only as the key it derives.
  bool render_node_password{};
  std::string wal_path;
  std::string proxy_cache_directory;
  bool proxy_cache_next_to_source{};
  core::u32 proxy_cache_gibibytes{8U};
  core::u32 output_buffer_frames{12U};
  core::u32 output_read_ahead_frames{12U};
  core::u32 playback_cache_mebibytes{2'048U};
  core::u32 playback_trail_frames{};
  TranscodePriority proxy_transcode_priority{TranscodePriority::Background};

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
  // --lifecycle-password or --lifecycle-secret-file was given this run, so
  // the stored password is not the one in use until the next plain launch.
  bool render_node_password_overridden{};
  bool wal_path_overridden{};
  // Result of the last save attempt, shown in the panel. Empty means nothing
  // has been attempted this session.
  std::string message;
};

struct PreferencesFrame final {
  // Present only on the frame the operator pressed Save.
  std::optional<EditorPreferences> save;
  // With `save`, when the operator typed a new render node password or
  // cleared it: the new password, empty for none. Absent keeps the stored one.
  std::optional<std::string> render_node_password;
  // Clears the cache currently in use on this machine. This is intentionally
  // independent of Save so an unsaved draft can never redirect a deletion.
  bool clear_proxy_cache{};
};

// The document half of the same idea: how this project's effects are computed.
//
// Split from Preferences deliberately. Preferences describe the machine and are
// never replicated; these change the picture, so they are an ordinary document
// edit that undoes, saves, and reaches every render node. Nothing here belongs
// beside an individual effect in the Inspector - a kernel width is set once per
// show and then left alone, and putting it on every blur node would be five
// controls an operator scrolls past forever.
// What became of the authored environment map, from the renderer's side.
enum class EnvironmentStatus : core::u8 {
  // No map authored. The ordinary case, and not a fault: the hemisphere is a
  // real answer, so this must not read as an error.
  NotAuthored,
  // Authored and still being read or decoded on the residency worker.
  Loading,
  // Read or decode failed. Sticky until the path changes.
  Failed,
  // Authored, and the renderer this project selected cannot use it. Image-
  // based lighting is Advanced only, and a Basic project shows the hemisphere
  // however good the map is.
  BackendUnsupported,
  // There is no stage node to store it on, so Apply has nowhere to write and
  // the field reverts. Its own state because that is a project-structure
  // problem rather than anything about the map, and the two look identical
  // from the operator's side - the value simply does not stick.
  NoStage,
  // Lighting the scene.
  Ready,
};

[[nodiscard]] std::string_view
environment_status_text(EnvironmentStatus status) noexcept;

// What became of the authored colour, from the output transform's side.
//
// Every way it can fail is otherwise silent: a look file that does not exist,
// a table still baking, and a project running Basic - where colour is never
// applied because the display transform lives on the scene-linear path - all
// show the same picture. Naming the state is the difference between "the look
// does nothing" and one specific thing to fix.
enum class ColorStatus : core::u8 {
  // The table for the authored transform and look is still baking on the
  // residency worker. The previous table stays on the output meanwhile.
  Loading,
  // Baked and live on the output.
  Ready,
  // The look file could not be read or is not a 3D `.cube`. The transform is
  // live without it, so the wall is predictable while the path is fixed.
  Failed,
  // The project is running Basic, which is display-referred already: nothing
  // here reaches the wall until Advanced is selected and available.
  BackendUnsupported,
};

[[nodiscard]] std::string_view color_status_text(ColorStatus status) noexcept;
// The residency state, in the operator's terms.
[[nodiscard]] ColorStatus
color_status_for(render::DisplayLutState state) noexcept;

struct ProjectSettingsState final {
  scene::EffectSettings settings{};
  scene::RendererSettings renderer{};
  // The lighting environment, which lives on the stage node rather than in the
  // renderer policy: it is content, not a capability. Edited here because it is
  // one per scene and is selected through the Environment scene row.
  scene::Environment environment{};
  doc::NodeId stage_node{};
  // What the renderer is actually doing with the authored map.
  //
  // Reported because every way this can fail is otherwise silent: a path that
  // does not resolve, a file that is not Radiance, and a project still on the
  // Basic backend all produce exactly the same thing on screen - the
  // hemisphere - and an operator has no way to tell which. Naming the state is
  // the difference between "it does not work" and one specific thing to fix.
  EnvironmentStatus environment_status{EnvironmentStatus::NotAuthored};
  // The document node the values came from, or zero when the project has none
  // and the authoring boundary must create one.
  doc::NodeId node{};
  // Lens calibration is camera-owned document state, unlike the project-wide
  // renderer policy above. Project Settings edits the active stage camera when
  // one is available; a zero source deliberately keeps the controls read-only.
  std::optional<scene::LensProfile> lens_profile;
  doc::NodeId camera_node{};
  // Which authored passes the renderer actually ran on the last frame.
  //
  // Last in the struct because callers brace-initialise the fields above
  // positionally, and because it is observed rather than edited - Apply never
  // writes it back.
  render::RendererPassStatus passes{};
  // What the output transform did with the authored colour. Observed, like
  // the passes above, and filled by the editor shell that owns the transform.
  ColorStatus color_status{ColorStatus::Ready};
  bool bake_running{};
  std::string bake_status;
  doc::NodeId reflection_anchor{};
  // Program is a published revision drawn with renderer settings other than
  // these; the next PUBLISH carries these to Program.
  bool program_renderer_differs{};
};

struct ProjectSettingsValues final {
  scene::EffectSettings settings{};
  scene::RendererSettings renderer{};
  scene::Environment environment{};
  std::optional<scene::LensProfile> lens_profile;

  bool operator==(const ProjectSettingsValues &) const = default;
};

struct ProjectSettingsFrame final {
  std::optional<scene::BakedLighting> bake;
  std::string bake_destination;
  bool cancel_bake{};
  // A committed modal draft or an immediate edit from the scene Inspector.
  std::optional<ProjectSettingsValues> apply;
};

struct EnvironmentKeyEdit final {
  scene::PropertyChannel channel{};
  double value{};
  bool erase{};
};

struct SceneSettingsFrame final {
  std::optional<FilePickerRequest> file_picker;
  ProjectSettingsFrame project;
  std::vector<EnvironmentKeyEdit> keys;
};

// Persistent scene controls share normal Inspector layout and author each
// gesture immediately. Resource/topology policies remain discrete settings;
// environment values use the same channels and diamonds as object properties.
class SceneSettingsPanel final {
public:
  void set_bake_destination(std::string path) { bake_destination_ = std::move(path); }
  [[nodiscard]] SceneSettingsFrame draw(gui::UiContext& ctx, const gui::Rect& bounds,
      SceneSettingsTarget target, const ProjectSettingsState& state,
      std::span<const scene::ChannelKeyframes> keyframes, core::u64 show_frame,
      std::span<const ElementInfo> elements = {});
private:
  NumberField fields_;
  SceneSettingsTarget target_{SceneSettingsTarget::None};
  doc::NodeId stage_{};
  float scroll_{};
  float content_height_{};
  std::array<bool, 10> sections_{{true,false,false,false,false,false,true,false,false,false}};
  std::string bake_destination_{"lighting"};
};

class ProjectSettingsPanel final {
public:
  explicit ProjectSettingsPanel(gui::UiId id) noexcept : id_(id) {}

  void open(const scene::EffectSettings &settings,
            const scene::RendererSettings &renderer,
            const std::optional<scene::LensProfile> &lens_profile = {},
            const scene::Environment &environment = {});
  void close() noexcept { open_ = false; }
  void adopt_bake(const scene::BakedLighting& baked) { environment_draft_.baked = baked; }
  [[nodiscard]] bool is_open() const noexcept { return open_; }

  // `bounds` is the whole window, as for Preferences: this is a modal card and
  // the caller must have denied the pointer to everything drawn before it.
  [[nodiscard]] ProjectSettingsFrame draw(gui::UiContext &ctx,
                                          const gui::Rect &bounds,
                                          const ProjectSettingsState &state);

private:
  enum class Category : core::u8 { Tracking, Lens };

  gui::UiId id_{};
  bool open_{};
  Category category_{Category::Tracking};
  scene::EffectSettings draft_{};
  scene::RendererSettings renderer_draft_{};
  scene::Environment environment_draft_{};
  std::optional<scene::LensProfile> lens_draft_;
  NumberField tracking_fields_;
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
  float scroll_{};
  EditorPreferences draft_;
  // The password being typed, and whether the operator touched it at all.
  std::string password_draft_;
  bool password_edited_{};
};

} // namespace mediaserver::editor
