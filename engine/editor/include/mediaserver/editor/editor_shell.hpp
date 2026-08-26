#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/mapping_panels.hpp>
#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/editor/workspace_panels.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/topology/hardware_map.hpp>

#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The operator editor: the bespoke docked surface for placing and animating
// scene elements.
//
// Program and Stage are two editors of the one replicated scene, and a drag in
// either resolves through the pure math in scene::manipulate to a document
// command. The panels share the engine's RHI device, so rendered viewports are
// sampled directly into the UI without a copy or readback.
//
// It authors nothing itself. Each frame it returns the edits the operator made
// as intents; the Director turns those into PutNode commands on the frame-
// scheduled bus, so undo, replication, and replay stay exactly as they are for
// every other edit. The editor never mutates scene or document state directly.
namespace mediaserver::editor {

enum class EditorError : core::u8 {
  // No GPU, or a device/window that could not be created.
  Unavailable,
  DisplayUnavailable,
  AllocationFailed,
};

enum class EditorStatus : core::u8 {
  Running,
  // The operator closed the editor window. The show should stop.
  Closed,
};

// One edit the operator made this frame: a node moved to a new world position.
// The Director reads the node's current transform, sets its position, and
// authors a PutNode. Position only for now; rotation and scale follow the same
// shape.
//
// `gesture` identifies the continuous manipulation this edit belongs to: every
// edit from one drag (mouse-down to release), in either view and whether by the
// gizmo or a bare drag, shares one value, and it changes when the next drag
// begins. The Director uses it as the transaction id, so a whole drag is a
// single undo step even though it authors one command per sealed frame (which
// is what moves the clip on the nodes as the drag happens). Zero is "no
// gesture".
struct EditIntent final {
  doc::NodeId node{};
  scene::Vec3 position{};
  core::u64 gesture{};

  bool operator==(const EditIntent &) const = default;
};

// A kind of element the operator can add to the scene. The Director turns each
// into a new document node with sensible defaults.
enum class AddKind : core::u8 {
  Box,
  Plane,
  Sphere,
  DirectionalLight,
  PointLight,
  Model,
  Media,
};

// A request to create one element. The Director allocates the node id;
// `gesture` makes the creation a single undo step. `source` is the file for a
// Model (empty otherwise).
struct AddIntent final {
  AddKind kind{};
  std::string source;
  // For a Media clip: how many frames it holds (a video's frame count, 1 for a
  // still), so the loop wraps at the right length. Unused for other kinds.
  core::u32 count{1U};
  core::u64 gesture{};
  // For a Media clip: the source's width/height, so its quad matches rather
  // than assuming 16:9. Last field, so other kinds' initializers keep working.
  float aspect{16.0F / 9.0F};
  // A placement can retain its persistent Library identity while keeping the
  // source string as a renderer/backward-compatible fallback.
  doc::NodeId asset{};
  scene::SceneId scene{};
  // Dragging an asset into Stage resolves the pointer onto the camera-target
  // plane. A click-to-add leaves this empty and uses the document defaults.
  std::optional<scene::Vec3> position;

  bool operator==(const AddIntent &) const = default;
};

// Import adds an asset.* document node only. It deliberately does not place an
// object in a scene; clicking the resulting Library row performs that separate
// action. Metadata is probed once at import and persisted with the asset.
struct AssetImportIntent final {
  assets::Asset asset;
  core::u64 gesture{};

  bool operator==(const AssetImportIntent &) const = default;
};

// Adds one first-class effect node targeting a scene element. The Director
// allocates the effect node ID and authors it like every other project node.
struct EffectIntent final {
  doc::NodeId target{};
  scene::EffectKind kind{scene::EffectKind::Brightness};
  core::u64 gesture{};

  bool operator==(const EffectIntent &) const = default;
};

// Program/Rehearsal controls are kept separate from transport. Rehearsal is
// an operator-local editing mode, while selecting a rehearsal scene and
// swapping the two slots are replicated document edits performed by the
// Director at a control-frame boundary.
enum class SwitcherAction : core::u8 {
  ToggleRehearse,
  Swap,
  SelectRehearsal,
  CreateScene,
  // Create a new scene by cloning `SwitcherRequest::scene`. Kept distinct
  // from CreateScene so the Director never has to infer copy semantics from
  // a non-zero source id.
  DuplicateScene,
};

struct SwitcherRequest final {
  SwitcherAction action{SwitcherAction::ToggleRehearse};
  // SelectRehearsal's destination or DuplicateScene's exact clone source.
  scene::SceneId scene{};
  // Swap is a dissolve by default. A zero duration is an explicit cut.
  core::u64 transition_control_frames{30U};

  bool operator==(const SwitcherRequest &) const = default;
};

// The evaluated switcher view used only for drawing the editor. All pointers
// are borrowed for the duration of frame(). `program` remains the first scene
// argument to frame(); these optional values provide the sandbox Stage and the
// two completed inputs needed while Program is dissolving.
struct EditorSwitcherState final {
  std::span<const scene::SceneInfo> scenes{};
  scene::SceneId program_scene{};
  scene::SceneId rehearsal_scene{};
  const scene::Scene *stage_scene{};
  const scene::Scene *transition_from{};
  const scene::Scene *transition_to{};
  float transition_mix{1.0F};
  bool transition_active{};
  bool rehearsal_mode{};
  bool rehearsal_locked{};
  // Defaults to true so callers that do not expose multi-scene switching
  // retain the previous enabled-state behaviour. The Director sets this
  // explicitly whenever Program and Rehearsal are populated.
  bool dissolve_compatible{true};
};

// Project-file state is owned by the Director because only it owns the complete
// authored document. The editor borrows this small view for its File menu and
// status bar, then returns path requests like every other authoring intent.
struct EditorProjectState final {
  std::string_view path;
  std::string_view message;
  bool dirty{};
  // Incremented only after a successful load, so editor-local selections and
  // gestures cannot retain node identities from the previous project.
  core::u64 generation{};
};

struct ProjectFileRequest final {
  std::string path;

  bool operator==(const ProjectFileRequest &) const = default;
};

// An Inspector edit of a named document property (used for light fields, which
// are not transform channels). The Director sets the property directly.
struct GenericEdit final {
  doc::NodeId node{};
  std::string property;
  double value{};
  core::u64 gesture{};

  bool operator==(const GenericEdit &) const = default;
};

// One Inspector edit: set a single transform component of `node` to `value`.
// The Director authors it as a base-property PutNode, grouped by `gesture`.
struct PropertyEdit final {
  doc::NodeId node{};
  scene::TransformChannel component{};
  double value{};
  core::u64 gesture{};

  bool operator==(const PropertyEdit &) const = default;
};

// One keyframe request on a single transform component: set, erase one key, or
// clear `target`'s channel for `component`. When keying, `value` is written at
// `frame`; `erase` removes only that indexed key; `clear` removes the whole
// channel. The Director authors it as a scene.channel edit; `gesture` groups a
// multi-component or multi-selection action into one undo step.
struct KeyIntent final {
  doc::NodeId target{};
  scene::TransformChannel component{};
  core::u64 frame{};
  double value{};
  bool clear{};
  core::u64 gesture{};
  // Delete one key at `frame` while preserving the rest of the channel.
  bool erase{};

  bool operator==(const KeyIntent &) const = default;
};

// A keyframe retimed by dragging its diamond: the key at `from_frame` on
// (target, component) moves to `to_frame`. Authored once on release.
struct KeyMove final {
  doc::NodeId target{};
  scene::TransformChannel component{};
  core::i64 from_frame{};
  core::i64 to_frame{};
  core::u64 gesture{};

  bool operator==(const KeyMove &) const = default;
};

// The result of one editor frame: whether it is still open, and the edits to
// author. Edits are already coalesced to one per node per frame.
struct EditorFrame final {
  EditorStatus status{EditorStatus::Running};
  std::vector<EditIntent> edits;
  // Non-zero when the operator asked to undo the last scene edit this frame.
  // The value is a fresh transaction id (from the same counter as gestures, so
  // it cannot collide with an edit's) for the inverse commands the undo
  // authors. The Director undoes the scene-edit context, which a whole
  // gesture-grouped drag leaves as a single step. Zero is "no undo".
  core::u64 undo_transaction{};
  // Set when the operator scrubbed the timeline: the show frame to seek to.
  // The Director pauses and seeks the transport there; the editor does not own
  // the show clock, it only requests the move.
  std::optional<core::u64> seek_to_frame{};
  // Keyframe requests the operator made this frame (key, clear, or erase).
  std::vector<KeyIntent> keys;
  // Keyframes dragged to a new time this frame.
  std::vector<KeyMove> key_moves;
  // Inspector edits: single transform components set to new base values.
  std::vector<PropertyEdit> property_edits;
  // Inspector edits of named properties (light fields).
  std::vector<GenericEdit> generic_edits;
  // A transport action from the timeline's controls, or None.
  TransportRequest transport{TransportRequest::None};
  // A change to the show range/loop, when the operator edited them.
  std::optional<RangeRequest> range{};
  // Elements to create this frame.
  std::vector<AddIntent> adds;
  std::vector<AssetImportIntent> asset_imports;
  std::vector<EffectIntent> effects;
  std::optional<SwitcherRequest> switcher;
  std::vector<HardwarePlaceRequest> hardware_places;
  std::vector<HardwareMoveRequest> hardware_moves;
  std::vector<HardwareLinkRequest> hardware_links;
  std::vector<HardwarePropertyEdit> hardware_property_edits;
  // Project-wide lifecycle listener range. Empty settings provenance in the
  // request means the authoring boundary must create hardware.settings.
  std::optional<HardwareControlPortEdit> hardware_control_port_edit;
  std::vector<HardwareDeleteRequest> hardware_deletes;
  std::vector<FeedMapAddRequest> feed_map_adds;
  std::vector<FeedMapRectEdit> feed_map_edits;
  std::vector<FeedMapDeleteRequest> feed_map_deletes;
  // Physical projection setup remains ordinary document authoring. Surface
  // geometry is reusable project state; a feed assignment selects a policy,
  // one surface, and the raster region owned by that output connector. UV
  // mesh browse requests are resolved and validated inside EditorShell, so
  // these vectors never expose a cancelled or invalid native-file choice.
  std::vector<FeedProjectionEdit> feed_projection_edits;
  std::vector<FeedSurfaceAddRequest> feed_surface_adds;
  std::vector<FeedSurfaceEditRequest> feed_surface_edits;
  std::vector<FeedSurfaceDeleteRequest> feed_surface_deletes;
  std::optional<CompositorAddIntent> compositor_add;
  std::optional<CompositorMoveIntent> compositor_move;
  std::optional<CompositorConnectIntent> compositor_connect;
  std::optional<CompositorPropertyEditIntent> compositor_property_edit;
  std::optional<CompositorDeleteIntent> compositor_delete;
  core::u64 compositor_gesture{};
  std::optional<ProjectFileRequest> save_project;
  std::optional<ProjectFileRequest> load_project;
  // Fixed lifecycle action from Output Monitor. The Director hands this to a
  // background controller; EditorShell never performs network I/O.
  std::optional<OutputLifecycleRequest> output_lifecycle;
  // Elements to delete this frame, and the shared gesture for the deletion.
  std::vector<doc::NodeId> deletes;
  core::u64 delete_gesture{};
};

struct EditorConfig final {
  std::string title{"media-server editor"};
  core::u32 width{1600U};
  core::u32 height{900U};
  // Window placement, so the editor and an output node can be arranged side by
  // side without a window manager. Negative leaves it to the platform.
  core::i32 position_x{-1};
  core::i32 position_y{-1};
};

// True when this build can open the editor: it has the scene shader bytecode
// and a GPU to run it on. Checked once so the Director can refuse cleanly.
[[nodiscard]] bool editor_available() noexcept;

class EditorShell final {
public:
  [[nodiscard]] static core::Result<std::unique_ptr<EditorShell>, EditorError>
  create(const EditorConfig &config) noexcept;

  EditorShell(const EditorShell &) = delete;
  EditorShell &operator=(const EditorShell &) = delete;
  EditorShell(EditorShell &&) = delete;
  EditorShell &operator=(EditorShell &&) = delete;
  ~EditorShell();

  // Renders the wall view of `scene`, runs one UI frame, and returns the
  // operator's edits and whether the window is still open. `scene` is the
  // committed scene evaluated at the current show frame - the same value every
  // node draws - so the editor edits exactly what the wall shows. `timeline`
  // is where the show is, so the editor can draw the playhead and scrub it.
  // `keyframes` is where each animated node holds keys, for drawing them on the
  // timeline; `elements` carries the selection's authored transform for the
  // Inspector.
  [[nodiscard]] EditorFrame
  frame(const scene::Scene &scene, const EditorTimeline &timeline,
        const std::vector<scene::ChannelKeyframes> &keyframes,
        const std::vector<ElementInfo> &elements,
        const std::vector<scene::EffectInstance> &effects = {},
        const topology::HardwareMap &hardware_map = {},
        const output::FeedMap &feed_map = {},
        const EditorSwitcherState &switcher = {},
        const assets::AssetRegistry &assets = {},
        const compositor::CompositorGraph &compositor_graph = {},
        const EditorProjectState &project = {},
        const OutputMonitorState &output_monitor = {},
        const topology::HardwareRuntimeSnapshot &hardware_runtime = {})
      noexcept;

private:
  struct Impl;

  explicit EditorShell(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

} // namespace mediaserver::editor
