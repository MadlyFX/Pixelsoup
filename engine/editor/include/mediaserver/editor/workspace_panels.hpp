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
      Unverified,
      Missing,
      Corrupt,
      Error,
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
    core::u32 assets_corrupt{};
    core::u32 assets_unverified{};
    std::string content_detail;

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

} // namespace mediaserver::editor
