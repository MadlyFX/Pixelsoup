#pragma once

#include <mediaserver/assets/asset_registry.hpp>
#include <mediaserver/compositor/compositor_graph.hpp>
#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/mapping_panels.hpp>
#include <mediaserver/editor/cues_panel.hpp>
#include <mediaserver/editor/timeline_panel.hpp>
#include <mediaserver/editor/viewport_panel.hpp>
#include <mediaserver/editor/live_data_panel.hpp>
#include <mediaserver/editor/workspace_panels.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>
#include <mediaserver/render/blender_discovery.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/selection_set.hpp>
#include <mediaserver/topology/hardware_map.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
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

// A Blender-linked object's whole placement after a viewport rotate, scale, or
// clear. A linked occurrence is placed by one world matrix (an override on
// the source object's), not by yaw/pitch/roll and scale fields, so a turn or a
// resize reaches it as the matrix it produces. Same gesture rules as
// EditIntent.
struct WorldEdit final {
  doc::NodeId node{};
  scene::Mat4 world{};
  core::u64 gesture{};

  bool operator==(const WorldEdit &) const = default;
};

// A kind of element the operator can add to the scene. The Director turns each
// into a new document node with sensible defaults.
enum class AddKind : core::u8 {
  Box,
  Plane,
  Sphere,
  DirectionalLight,
  PointLight,
  // A spot is a point light with a cone. Its own entry rather than a point the
  // operator then converts, because a lamp, a window and a practical are what
  // people actually place, and a cone authored after the fact starts pointing
  // wherever the default said.
  SpotLight,
  Model,
  Media,
  // A camera on the stage. Adding one authors an ordinary scene.camera node;
  // assigning it a tracking source is what makes it the inner frustum's.
  Camera,
  // A tracker feed. Project state rather than a thing on the stage, so it is
  // added from the camera that wants one rather than from the Library.
  TrackingSource,
  // A named tracker feed. Its own node so several cameras can name different
  // ones, and so a camera's binding survives the source being re-authored.
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
  // Exact native cadence for video. Absence deliberately means legacy/image
  // sequence 1:1 mapping, so old aggregate initializers remain compatible.
  std::optional<assets::MediaFrameRate> frame_rate;

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

// Removes one persistent Library asset record. Scene placements are separate
// document nodes with a source fallback, so they intentionally are not
// cascaded when an import is removed from the Library.
struct AssetDeleteIntent final {
  doc::NodeId asset{};
  core::u64 gesture{};

  bool operator==(const AssetDeleteIntent &) const = default;
};

struct AssetProxyEditIntent final {
  doc::NodeId asset{};
  std::optional<assets::DerivativeProfileId> requested_profile;
  assets::ProxyPlaybackPolicy policy{assets::ProxyPlaybackPolicy::Source};
  core::u64 gesture{};

  bool operator==(const AssetProxyEditIntent &) const = default;
};

// Adds one first-class effect node targeting a scene element. The Director
// allocates the effect node ID and authors it like every other project node.
struct EffectIntent final {
  doc::NodeId target{};
  scene::EffectKind kind{scene::EffectKind::Brightness};
  core::u64 gesture{};

  bool operator==(const EffectIntent &) const = default;
};

// Replaces the complete typed Film Grain tuple on one first-class effect.
// Keeping mode and seed with the floating controls prevents a frame from
// evaluating a mixture of two UI states while a gesture is in flight.
struct FilmGrainEffectEditIntent final {
  doc::NodeId effect{};
  scene::FilmGrainParameters parameters{};
  core::u64 gesture{};

  bool operator==(const FilmGrainEffectEditIntent &) const = default;
};

// Removes one first-class effect and any animation channels that target it.
// The Director performs the cascade at the authoring boundary so it is one
// replicated, undoable document transaction rather than an Inspector-local
// visibility toggle.
struct EffectDeleteIntent final {
  doc::NodeId effect{};
  core::u64 gesture{};

  bool operator==(const EffectDeleteIntent &) const = default;
};

// One clip placement written back to its document node. The whole ClipTiming
// travels together because it is one node and one PutNode; a drag authors one
// of these per frame, all sharing a gesture, so the wall follows the pointer
// and the whole drag still undoes in a single step.
struct ClipTimingEdit final {
  doc::NodeId clip{};
  scene::ClipTiming timing{};
  core::u64 gesture{};
  // See TimelineClipEdit::lane. Travels with the placement so a drag that
  // crosses lanes stays one undo step with the movement that carried it.
  std::optional<doc::NodeId> lane;

  bool operator==(const ClipTimingEdit &) const = default;
};

// A splice. `clip` keeps the frames before `frame` and a new placement, which
// the Director allocates an id for, takes the frames from `frame` onward with
// its trim advanced to match. Neither half re-encodes or copies any media: a
// cut is two placements of one source.
struct ClipSplitIntent final {
  doc::NodeId clip{};
  core::u64 frame{};
  core::u64 gesture{};

  bool operator==(const ClipSplitIntent &) const = default;
};

// See TimelineElementFlagEdit. The shell attaches the active scene and one
// gesture so a run of toggles collapses into a single undo step.
struct ElementFlagIntent final {
  scene::SceneId scene{};
  doc::NodeId element{};
  TimelineElementFlag flag{TimelineElementFlag::Visible};
  bool value{};
  core::u64 gesture{};

  bool operator==(const ElementFlagIntent &) const = default;
};

struct TimelineLaneEditIntent final {
  TimelineLaneAction action{TimelineLaneAction::Create};
  scene::SceneId scene{};
  doc::NodeId lane{};
  std::string name;
  core::u64 gesture{};
  doc::NodeId destination{};

  bool operator==(const TimelineLaneEditIntent &) const = default;
};

struct TimelineLayerOrderIntent final {
  scene::SceneId scene{};
  doc::NodeId layer{};
  doc::NodeId destination{};
  TimelineLayerPlacement placement{TimelineLayerPlacement::Before};
  core::u64 gesture{};

  bool operator==(const TimelineLayerOrderIntent &) const = default;
};

// One marker or cue change. The shell attaches the Stage scene and one
// gesture, so dragging several selected markers is one undo step and the
// Director authors each as a Scene transaction like any other edit.
struct TimelineMarkerEditIntent final {
  TimelineMarkerAction action{TimelineMarkerAction::Create};
  scene::SceneId scene{};
  scene::TimelineMarker marker{};
  core::u64 gesture{};

  bool operator==(const TimelineMarkerEditIntent &) const = default;
};

// One semantic advanced timeline operation. The panel supplies stable
// clip/lane addresses and a signed show-frame gesture; the shell attaches the
// active Stage scene, exact show rate/playhead, and one undo transaction.
struct TimelineEditIntent final {
  scene::SceneId scene{};
  TimelineEditRequest request{};
  scene::MediaFrameRate show_rate{};
  core::u64 playhead{};
  core::u64 transaction{};

  bool operator==(const TimelineEditIntent &) const = default;
};

enum class GroupEditAction : core::u8 {
  Group,
  Ungroup,
  // File `selection` under `group` (zero: the top level) without moving any
  // of it on the stage. `anchor` names a sibling in the destination the moved
  // nodes land beside; without one they land at the front.
  MoveInto,
};

// One semantic Scene hierarchy operation. Group carries the complete canonical
// selection observed by the panel; Ungroup carries the selected group in
// `group`. The Director validates against its projected document and authors
// the complete planner result as one Scene transaction.
struct GroupEditIntent final {
  GroupEditAction action{GroupEditAction::Group};
  scene::SceneId scene{};
  std::vector<doc::NodeId> selection;
  doc::NodeId group{};
  doc::NodeId anchor{};
  bool before{};
  core::u64 gesture{};

  bool operator==(const GroupEditIntent &) const = default;
};

enum class SelectionSetEditAction : core::u8 {
  Create,
  ReplaceMembers,
  Recall,
  Rename,
  Delete,
};

// One typed Selection Sets-panel operation. Create and ReplaceMembers carry
// the exact canonical Scene selection observed by the editor; Recall carries
// no document mutation and is resolved by the Director against its projected
// document before replacing only this editor's local Scene selection.
struct SelectionSetEditIntent final {
  SelectionSetEditAction action{SelectionSetEditAction::Recall};
  scene::SceneId scene{};
  doc::NodeId set{};
  std::string name;
  std::vector<doc::NodeId> members;
  core::u64 gesture{};

  bool operator==(const SelectionSetEditIntent &) const = default;
};

// Panel property/point actions and Stage transform gestures share one Director
// route. Viewport translations stay semantic so a Bezier move can update every
// child point in one replacement-document transaction; primitive resize and
// rotation likewise land as one shape edit.
using MaskEditRequest =
    std::variant<MasksPanelIntent, MaskViewportTranslateIntent,
                 MaskViewportShapeEditIntent>;

struct MaskEditIntent final {
  MaskEditRequest request;
  core::u64 transaction{};

  bool operator==(const MaskEditIntent &) const = default;
};

// The project's effect settings, applied from the Project Settings dialog.
// `node` is the settings node the values were read from, or zero when the
// project has none and the authoring boundary must create one - the same
// provenance rule the sync policy edit uses.
struct ProjectSettingsEdit final {
  scene::EffectSettings settings{};
  scene::RendererSettings renderer{};
  // The lighting environment and the stage node that owns it. A zero stage
  // node means the document has none, and the environment is then not written
  // rather than inventing a node this dialog has no business creating.
  scene::Environment environment{};
  doc::NodeId stage_node{};
  doc::NodeId node{};
  // Calibration remains on the authored camera node. A missing profile is the
  // explicit request to return that camera to its legacy turn-FOV path.
  std::optional<scene::LensProfile> lens_profile;
  doc::NodeId camera_node{};
  core::u64 gesture{};

  bool operator==(const ProjectSettingsEdit &) const = default;
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
  // Delete the exact non-Program scene after the editor's inline confirmation.
  DeleteScene,
};

struct SwitcherRequest final {
  SwitcherAction action{SwitcherAction::ToggleRehearse};
  // SelectRehearsal's destination, DuplicateScene's source, or DeleteScene's
  // exact target.
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
  // Compositor graphs are scene-scoped, so Program and the scene on the Stage
  // do not share one. The `compositor_graph` argument to frame() is the Stage
  // scene's graph - the one the Compositor panel edits; these are the graphs
  // the Program viewport needs to draw the live output faithfully. A null
  // pointer falls back to the Stage graph, which is what a caller with one
  // scene wants.
  const compositor::CompositorGraph *program_graph{};
  const compositor::CompositorGraph *transition_from_graph{};
  const compositor::CompositorGraph *transition_to_graph{};
  float transition_mix{1.0F};
  bool transition_active{};
  // A Loop cue's crossfade on Program. The transition fields above describe
  // the two frames it mixes, but the switcher itself is free: SWAP and
  // Program masks stay available, because only the show's time is moving.
  bool loop_fade_active{};
  bool rehearsal_mode{};
  bool rehearsal_locked{};
  // Defaults to true so callers that do not expose multi-scene switching
  // retain the previous enabled-state behaviour. The Director sets this
  // explicitly whenever Program and Rehearsal are populated.
  bool dissolve_compatible{true};
  // True only when the project asks for physical output synchronization under
  // a STRICT failure policy and that timing is not proven. Promoting Program
  // is the moment the claim becomes visible on the wall, so it is the moment
  // the claim has to hold. Nothing else holds a swap: whether render nodes
  // have the media is reported, not enforced. Output Monitor's timing line
  // names the unmet condition.
  bool program_held{};
  bool linked_publication{};
  bool publication_pending{};
};

// One scene's Blender link, as the PROJECT SCENES row and its menu show it.
// Scenes that were never opened in Blender have no entry.
struct BlenderSceneState final {
  scene::SceneId scene{};
  // The associated .blend; empty while a Blender opened from the Connect
  // dialog has not been saved yet.
  std::string_view file;
  // `running` - a Blender with this scene's file is open; `opening` - Media
  // Server started one and is waiting for it to report in.
  bool running{}, opening{}, connected{}, pending{}, failed{};
  std::string_view status;
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
  std::string_view scene_link_details;
  // Negotiated capabilities, publisher build and measured link evidence.
  std::string_view scene_link_diagnostics;
  std::string_view publication_storage;
  std::string_view publication_status;
  bool publication_pending{}, publication_scheduled{}, publication_rollback{};
  std::span<const render::BlenderInstance> blender_instances;
  // The instance linked to the Preview scene, the dialog's status line, and
  // whether that link is connected.
  std::string_view blender_instance, blender_error;
  bool blender_connected{};
  std::span<const BlenderSceneState> blender_scenes;
  // Which scene each discovered instance is linked to, by instance id.
  std::span<const std::pair<std::string, std::string>> blender_bound_scenes;
  // Where Publish stages Program files when no shared folder was chosen.
  std::string_view publication_default_storage;
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

  std::optional<std::string> text;
  bool operator==(const GenericEdit &) const = default;
};

// A new name for one node, from any list that renames in place: a scene
// element, a group, a track, or a project scene. The Director writes it into
// the property that node type keeps its name in, so the Scene outliner, the
// Timeline and the Inspector all rename the same thing the same way.
struct ElementRenameIntent final {
  doc::NodeId node{};
  std::string name;
  core::u64 gesture{};

  bool operator==(const ElementRenameIntent &) const = default;
};

// Persisted editor-selection lock shared by scene elements and calibrated LED
// surfaces. Locking never changes render output; it only removes the target
// from interactive selection and manipulation.
struct LockEdit final {
  doc::NodeId node{};
  bool locked{};
  core::u64 gesture{};

  bool operator==(const LockEdit &) const = default;
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

// Splits one surface transform gesture at the editor boundary. Components that
// already own channels become playhead keys; the remaining changed components
// stay in one masked base edit. Both lanes retain the caller's gesture so a
// mixed screen drag is one undo transaction.
struct RoutedSurfaceEdit final {
  std::optional<FeedSurfaceEditRequest> base;
  std::vector<KeyIntent> keys;
};

[[nodiscard]] RoutedSurfaceEdit route_surface_transform_edit(
    FeedSurfaceEditRequest request,
    std::span<const scene::ChannelKeyframes> keyframes,
    core::u64 show_frame);

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

enum class HistoryAction : core::u8 {
  Undo,
  Redo,
};

enum class HistoryContext : core::u8 {
  Scene,
  Compositor,
};

// Director-local identity for a repeatable semantic editor action. It mirrors
// the scheduler's opaque key without coupling the editor library to cmd.
struct SemanticCoalescingKey final {
  core::u64 focus{};
  core::u64 selection{};
  core::u64 property{};
  core::u64 direction{};
  core::u64 modifiers{};

  bool operator==(const SemanticCoalescingKey &) const = default;
};

struct SemanticCoalescingRequest final {
  HistoryContext context{HistoryContext::Scene};
  SemanticCoalescingKey key;
  // Fresh identity for this physical repeat. The Director uses it only when
  // policy opens a new logical transaction; continuation reuses the active
  // scheduler transaction instead.
  core::u64 gesture{};

  bool operator==(const SemanticCoalescingRequest &) const = default;
};

// One navigation step through the scene-edit history. The transaction is
// freshly allocated from the same gesture counter as ordinary edits, so the
// commands authored by undo or redo cannot collide with an existing gesture.
struct HistoryRequest final {
  HistoryAction action{HistoryAction::Undo};
  HistoryContext context{HistoryContext::Scene};
  core::u64 transaction{};

  bool operator==(const HistoryRequest &) const = default;
};

// A local clip clipboard command. Copy carries document ids so the Director
// snapshots their current nodes when it receives the command; Paste carries
// the active Stage scene and playhead used to rewrite the immutable snapshot.
struct ClipClipboardRequest final {
  ClipClipboardAction action{ClipClipboardAction::Copy};
  scene::SceneId scene{};
  core::u64 playhead{};
  std::vector<doc::NodeId> selection;
  core::u64 transaction{};

  bool operator==(const ClipClipboardRequest &) const = default;
};

// Director-local key clipboard requests. Copy carries only stable key
// addresses; the Director snapshots value/interpolation from its projected
// document at command time. Paste keeps the original target/property and
// anchors the earliest copied key at the active Timeline playhead.
struct KeyClipboardRequest final {
  TimelineKeyClipboardAction action{TimelineKeyClipboardAction::Copy};
  scene::SceneId scene{};
  core::u64 playhead{};
  std::vector<TimelineKeyAddress> selection;
  core::u64 transaction{};

  bool operator==(const KeyClipboardRequest &) const = default;
};

// One Alt-drag duplicate batch. Every selected source uses the same integer
// frame delta and the complete batch shares one undo transaction.
struct KeyDuplicateRequest final {
  scene::SceneId scene{};
  std::vector<TimelineKeyAddress> selection;
  core::i64 delta{};
  core::u64 transaction{};

  bool operator==(const KeyDuplicateRequest &) const = default;
};

struct KeyBatchRequest final {
  scene::SceneId scene{};
  TimelineKeyBatchEdit edit;
  core::u64 transaction{};

  bool operator==(const KeyBatchRequest &) const = default;
};

struct KeyScalePreviewRequest final {
  scene::SceneId scene{};
  TimelineKeyScalePreviewRequest preview;

  bool operator==(const KeyScalePreviewRequest &) const = default;
};

// The result of one editor frame: whether it is still open, and the edits to
// author. Edits are already coalesced to one per node per frame.
struct EditorFrame final {
  core::AssetSetReadiness assets{};
  EditorStatus status{EditorStatus::Running};
  // Latest readiness produced while recording Program. Empty means no GPU
  // frame slot was available, so the Director keeps the preceding answer
  // rather than manufacturing a new one or waiting for the device.
  std::optional<render::RendererReadiness> renderer_readiness;
  core::RenderAdapterIdentity render_adapter{};
  core::FrameTiming frame_timing{};
  bool check_render_conformance{};
  core::u64 temporal_restarts{};
  core::u64 temporal_restart_frame{};
  std::vector<EditIntent> edits;
  std::vector<WorldEdit> world_edits;
  // Set when the operator asked to move backward or forward through the active
  // scene or compositor context. Selecting in either panel changes that active
  // context without authoring; a gesture-grouped drag remains one step in
  // either direction.
  std::optional<HistoryRequest> history;
  std::optional<ClipClipboardRequest> clip_clipboard;
  std::optional<KeyClipboardRequest> key_clipboard;
  std::optional<KeyDuplicateRequest> key_duplicate;
  std::optional<KeyBatchRequest> key_batch;
  std::optional<KeyScalePreviewRequest> key_scale_preview;
  std::optional<SemanticCoalescingRequest> semantic_coalescing;
  // Focus/selection/panel/project transitions and non-keyboard gestures close
  // an open repeat even when they author no command themselves.
  bool close_semantic_coalescing{};
  // Set when the operator scrubbed the timeline: the show frame to seek to.
  // The Director pauses and seeks the transport there; the editor does not own
  // the show clock, it only requests the move.
  std::optional<core::u64> seek_to_frame{};
  // Keyframe requests the operator made this frame (key, clear, or erase).
  std::vector<KeyIntent> keys;
  // Keyframes dragged to a new time this frame.
  std::vector<KeyMove> key_moves;
  // Clip placements moved, trimmed, or re-faded on the timeline this frame.
  std::vector<ClipTimingEdit> clip_timings;
  // Clips cut at the playhead this frame.
  std::vector<ClipSplitIntent> clip_splits;
  std::vector<TimelineLaneEditIntent> lane_edits;
  std::vector<TimelineLayerOrderIntent> layer_orders;
  std::vector<ElementFlagIntent> element_flags;
  // In-place renames from the outliner, the Timeline, or the scene list.
  std::vector<ElementRenameIntent> renames;
  // The operator asked the Library to look at the sources again.
  bool rescan_assets{};
  std::vector<TimelineEditIntent> timeline_edits;
  // Markers and cues created, moved, renamed, or deleted this frame, from the
  // Timeline or the Cue Stack.
  std::vector<TimelineMarkerEditIntent> marker_edits;
  // The operator armed or disarmed the show's cues.
  std::optional<bool> cues_armed;
  std::optional<GroupEditIntent> group_edit;
  std::optional<SelectionSetEditIntent> selection_set_edit;
  std::optional<MaskEditIntent> mask_edit;
  // Inspector edits: single transform components set to new base values.
  std::vector<PropertyEdit> property_edits;
  // Inspector edits of named properties (light fields).
  std::vector<GenericEdit> generic_edits;
  // Scene-tree and Feed Mapping lock changes.
  std::vector<LockEdit> lock_edits;
  // A transport action from the timeline's controls, or None.
  TransportRequest transport{TransportRequest::None};
  // A change to the show range/loop, when the operator edited them.
  std::optional<RangeRequest> range{};
  // Exact project-wide show cadence selected in the Timeline.
  std::optional<ShowRateRequest> show_rate{};
  // Elements to create this frame.
  std::vector<AddIntent> adds;
  std::vector<AssetImportIntent> asset_imports;
  std::vector<AssetDeleteIntent> asset_deletes;
  std::vector<AssetProxyEditIntent> asset_proxy_edits;
  // Machine-local derivative work requested by visible Library rows. These
  // intents never author document nodes or substitute a placement's asset id.
  std::vector<LibraryThumbnailRequestIntent> thumbnail_requests;
  std::optional<LibraryThumbnailRetainIntent> thumbnail_retain;
  std::vector<TimelineFilmstripRequestIntent> filmstrip_requests;
  std::optional<TimelineFilmstripRetainIntent> filmstrip_retain;
  std::vector<EffectIntent> effects;
  std::vector<FilmGrainEffectEditIntent> film_grain_effect_edits;
  std::vector<EffectDeleteIntent> effect_deletes;
  std::optional<SwitcherRequest> switcher;
  std::vector<HardwarePlaceRequest> hardware_places;
  std::vector<HardwareMoveRequest> hardware_moves;
  std::vector<HardwareLinkRequest> hardware_links;
  std::vector<HardwarePropertyEdit> hardware_property_edits;
  // Project-wide lifecycle listener range. Empty settings provenance in the
  // request means the authoring boundary must create hardware.settings.
  std::optional<HardwareControlPortEdit> hardware_control_port_edit;
  // Project-wide output synchronization policy. An empty policy provenance in
  // the request means the authoring boundary must create sync.policy.
  std::optional<HardwareSyncPolicyEdit> sync_policy_edit;
  std::vector<HardwareOutputBindingEdit> hardware_output_binding_edits;
  std::vector<HardwareOutputBindingDelete> hardware_output_binding_deletes;
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
  // File > New Project: replace the project with an untitled, empty one.
  bool new_project{};
  std::optional<ProjectFileRequest> import_linked_scene;
  bool cancel_linked_scene_import{};
  bool start_scene_link{}, stop_scene_link{};
  // Links a discovered Blender instance to the Preview scene.
  std::string connect_blender;
  // Disconnects one discovered instance, or restarts its link with a fresh
  // session and full snapshot (Connect dialog).
  std::string disconnect_blender, resync_blender;
  // Opens a scene's .blend in Blender (creating it the first time) and links
  // it; or disconnects the Blender linked to a scene. From PROJECT SCENES.
  std::optional<scene::SceneId> open_in_blender, disconnect_blender_scene;
  std::optional<std::string> publication_storage;
  bool publish_linked_scene{}, cancel_linked_publication{}, rollback_linked_publication{};
  core::u64 linked_scene_import_gesture{};
  // Set only when the operator pressed Save in Preferences. The Director
  // validates and persists it; the editor never writes settings itself.
  std::optional<EditorPreferences> save_preferences;
  // With save_preferences: a newly typed render node password, or empty
  // for none. Absent keeps the stored one.
  std::optional<std::string> render_node_password;
  // Set on the Clear Cache button frame. The Director resolves and clears the
  // cache currently in use; an unsaved preferences draft cannot redirect it.
  bool clear_proxy_cache{};
  // Emitted after a settled dock/tab/floating-panel change. The Director
  // stores this opaque, versioned machine-local record outside the project.
  std::optional<std::vector<std::byte>> save_workspace_layout;
  // Set only when the operator pressed Apply in Project Settings. Unlike
  // preferences this is a document edit: it undoes, saves, and replicates.
  std::optional<ProjectSettingsEdit> project_settings;
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
  // Previously validated settings-envelope payload. Invalid or incompatible
  // dock schemas are ignored and replaced by the built-in workspace.
  std::span<const std::byte> workspace_layout{};
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
        std::span<const LibraryThumbnailObservation> thumbnails = {},
        const compositor::CompositorGraph &compositor_graph = {},
        const EditorProjectState &project = {},
        const OutputMonitorState &output_monitor = {},
        const topology::HardwareRuntimeSnapshot &hardware_runtime = {},
        const sync::SyncPolicy &sync_policy = {},
        const EditorPreferencesState &preferences = {},
        const ProjectSettingsState &project_settings = {},
        std::span<const LibraryProxyRuntimeObservation> proxy_runtime = {},
        std::span<const scene::TimelineLane> timeline_lanes = {},
        std::optional<core::i64> clip_clipboard_span = std::nullopt,
        std::span<const scene::SelectionSetSummary> selection_sets = {},
        std::optional<TimelineKeyScalePreview> key_scale_preview =
            std::nullopt,
        std::span<const TimelineFilmstripObservation> filmstrips = {},
        const LiveDataState &live_data = {},
        std::span<const scene::TrackingSource> tracking_sources = {},
        std::span<const scene::TimelineMarker> timeline_markers = {}) noexcept;

  // Applies a Director-validated recall without authoring a command or
  // touching any non-Scene selection domain. Returns false if the scene is no
  // longer displayed or the local selection carrier could not be allocated.
  [[nodiscard]] bool apply_selection_set_recall(
      scene::SceneId scene, std::span<const doc::NodeId> targets,
      std::optional<doc::NodeId> primary = std::nullopt) noexcept;

  [[nodiscard]] bool apply_key_edit_selection(
      scene::SceneId scene,
      std::span<const TimelineKeyAddress> selection) noexcept;

  // Runtime-only Library derivative residency. The caller owns generation
  // and disk-cache policy; EditorShell owns GPU objects because they must use
  // the same device as the UI draw list.
  void reset_library_thumbnail_textures(core::u64 project_generation) noexcept;
  [[nodiscard]] const rhi::Texture *
  library_thumbnail_texture(const assets::DerivativeKey &key) noexcept;
  [[nodiscard]] bool upload_library_thumbnail(
      const assets::DerivativeKey &key,
      std::span<const core::u8> encoded_png) noexcept;
  void retain_library_thumbnail_textures(
      std::span<const assets::DerivativeKey> keys) noexcept;

  // Runtime-only Timeline filmstrip atlas residency. This cache shares the UI
  // device but not the Library thumbnail capacity or derivative identity.
  void reset_timeline_filmstrip_textures(core::u64 project_generation) noexcept;
  [[nodiscard]] const rhi::Texture *
  timeline_filmstrip_texture(const assets::DerivativeKey &key) noexcept;
  [[nodiscard]] bool upload_timeline_filmstrip(
      const assets::DerivativeKey &key,
      std::span<const core::u8> encoded_png) noexcept;
  void retain_timeline_filmstrip_textures(
      std::span<const assets::DerivativeKey> keys) noexcept;

private:
  struct Impl;

  explicit EditorShell(std::unique_ptr<Impl> impl) noexcept;

  std::unique_ptr<Impl> impl_;
};

} // namespace mediaserver::editor
