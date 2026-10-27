#pragma once

#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/assets/waveform.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/editor/selection.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/scene/key_edit.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/timeline_edit.hpp>

#include <compare>
#include <cstddef>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The editor's layer timeline, on the bespoke UI stack.
//
// The panel owns view state only: horizontal time zoom/pan, vertical scroll,
// twirl state, key selection, snapping, and in-progress marquee/retime
// gestures. Show time, scene selection, authored values, and keyframes arrive
// as immutable inputs every frame. The panel returns requests for the shell to
// turn into document and transport commands; it never mutates either one
// directly.
namespace mediaserver::editor {

namespace timeline {
// Which rows the operator has twirled open. Objects, property groups, and
// hierarchy parents fold independently; selecting a descendant elsewhere
// reveals its ancestors once, when it becomes the anchor.
struct RowsState final {
  std::set<core::u64> expanded_layers;
  std::set<core::u64> expanded_groups;
  std::set<doc::NodeId> expanded_hierarchies;
  doc::NodeId hierarchy_selection{};
  // Tracks start open; a closed track lists only its header, which then
  // summarises its clips.
  std::set<doc::NodeId> collapsed_lanes;
};

// One animatable property of one target: where a channel's keys live.
struct ChannelId final {
  doc::NodeId target{};
  scene::PropertyChannel property{};

  auto operator<=>(const ChannelId &) const = default;
};

class TimelineGraphView;
} // namespace timeline

// A transport action the operator asked for. The Director owns transport, so
// the editor names the request rather than moving the show clock itself.
enum class TransportRequest : core::u8 {
  None,
  TogglePlay,
  StepBack,
  StepForward,
  JumpBack,
  JumpForward,
  GoToStart,
  GoToEnd,
  // The live operator's one action: run from a hold, release a loop, or play
  // from a stop to the next cue. A show already running through its cues
  // ignores it, because GO never skips content.
  Go,
};

// An inclusive run of show frames, `first` <= `last`.
struct TimelineFrameRun final {
  core::u64 first{};
  core::u64 last{};

  bool operator==(const TimelineFrameRun &) const = default;
};

// Where one render node's output last put a complete frame on screen. A node
// presents behind the Director by its decode headroom, and further behind
// after an underrun, so it is drawn as its own playhead rather than assumed to
// sit under the editor's. `latency_frames` is how far it trails the newest
// frame it has received.
struct TimelineNodePlayhead final {
  core::u32 node_index{};
  std::string name;
  core::u64 show_frame{};
  core::u64 latency_frames{};

  bool operator==(const TimelineNodePlayhead &) const = default;
};

// Where the editor is in time. It is passed in each frame because transport
// owns it; the timeline only reflects it and asks to seek or change its range.
//
// `show_frame` is the frame being authored - the Preview playhead while the
// operator is rehearsing, and the live Program playhead otherwise. Everything
// scene-scoped keys against it. `program_frame` is always the live playhead,
// for the things that are not scene-scoped: the Program viewport, and the feed
// map, whose surfaces are physical stage geometry belonging to the show rather
// than to the scene on the Stage. Range, loop, and rate are one show-wide
// setting, so Preview and Program share them.
struct EditorTimeline final {
  core::u64 show_frame{};
  core::u64 program_frame{};
  core::u64 first_frame{};
  core::u64 last_frame{};
  // `playing` is the Stage playhead's state, which is what the timeline's own
  // transport buttons reflect. `program_playing` is the live show's.
  bool playing{};
  bool program_playing{};
  bool loop{};
  core::u32 rate_numerator{50U};
  core::u32 rate_denominator{1U};
  // Where the Stage playhead stands against the cues of the scene on the
  // Stage. Ids are cue marker nodes, zero meaning none. The Director derives
  // these from its own transport, so the timeline reports what the show will
  // actually do rather than guessing from the marker list.
  bool cues_armed{true};
  doc::NodeId held_cue{};
  doc::NodeId looping_cue{};
  doc::NodeId next_cue{};
  // Frames until a playing show reaches `next_cue`.
  std::optional<core::u64> frames_to_next_cue;
  // Frames until the Loop cue holding out its wait goes round.
  std::optional<core::u64> frames_to_loop_restart;
  // Show frames of the scene on the Stage whose video this machine has
  // already decoded and holds ready, ascending and disjoint - the editor's
  // equivalent of a RAM preview bar. A frame with no video on it needs
  // nothing decoded and is never listed. Machine-local runtime truth: it says
  // nothing about what a render node holds.
  std::vector<TimelineFrameRun> buffered;
  // Each live render node's playhead, when the Stage shows the scene on
  // Program. Empty while rehearsing another scene, where the nodes' position
  // belongs to a different timeline.
  std::vector<TimelineNodePlayhead> node_playheads;
};

// The marker palette. The document stores the index; the editor owns the hue,
// so every panel that shows a marker paints it the same.
[[nodiscard]] gui::UiColor timeline_marker_color(core::u8 index) noexcept;
[[nodiscard]] std::string_view timeline_marker_color_name(
    core::u8 index) noexcept;

// HH:MM:SS:FF at the show's nominal integer rate - the form a stage manager
// reads off a cue sheet. 29.97 shows count 30 frames to the second without
// drop-frame correction, which is what every other figure in the editor uses.
[[nodiscard]] std::string format_timecode(core::u64 frame,
                                          core::u32 rate_numerator,
                                          core::u32 rate_denominator);

// Reads what an operator types into a time field: HH:MM:SS:FF, MM:SS:FF,
// SS:FF, or a bare frame count, with ';' or '.' accepted for ':'. A leading
// '+' or '-' is relative to `relative_to`. Nothing for text that names no
// frame; negative results clamp to zero.
[[nodiscard]] std::optional<core::u64>
parse_timecode(std::string_view text, core::u32 rate_numerator,
               core::u32 rate_denominator, core::u64 relative_to);

// An exact project show-rate choice. The Timeline exposes the common film,
// broadcast, and display cadences, including their 1000/1001 forms, while the
// Director remains the authority that validates and applies the request.
struct ShowRateRequest final {
  core::u32 numerator{50U};
  core::u32 denominator{1U};

  bool operator==(const ShowRateRequest &) const = default;
};

// A change to the show's range and loop. The Director applies it to transport
// and clamps the playhead into the result.
struct RangeRequest final {
  core::u64 first{};
  core::u64 last{};
  bool loop{};

  bool operator==(const RangeRequest &) const = default;
};

// A key request before the shell assigns its undo gesture. All three components
// emitted by one group/property diamond belong to the same gesture when the
// shell maps the panel frame into its EditorFrame.
struct TimelineKeyEdit final {
  doc::NodeId target{};
  scene::TransformChannel component{};
  core::u64 frame{};
  double value{};
  bool clear{};
  // Removes only the key at `frame`; unlike clear, the channel and its other
  // keys survive. Used by Delete on a diamond selection.
  bool erase{};

  bool operator==(const TimelineKeyEdit &) const = default;
};

// A key retime authored once on release. Holding the drag locally is important:
// the committed key list can lag several frames behind the pointer without
// pulling the diamond back to where it began.
struct TimelineKeyMoveEdit final {
  doc::NodeId target{};
  scene::TransformChannel component{};
  core::i64 from_frame{};
  core::i64 to_frame{};

  bool operator==(const TimelineKeyMoveEdit &) const = default;
};

// Stable address of one authored key in the current document. Values and
// interpolation are deliberately not cached in the editor: the Director
// snapshots them from its projected document when Copy or Alt-duplicate is
// accepted.
struct TimelineKeyAddress final {
  doc::NodeId target{};
  scene::PropertyChannel property{};
  core::i64 frame{};

  auto operator<=>(const TimelineKeyAddress &) const = default;
};

// Alt-drag duplicates the complete selected key set with one shared frame
// delta. The Director applies this batch atomically and reports targets that
// became locked or incompatible after the editor frame was drawn.
struct TimelineKeyDuplicateEdit final {
  std::vector<TimelineKeyAddress> keys;
  core::i64 delta{};

  bool operator==(const TimelineKeyDuplicateEdit &) const = default;
};

enum class TimelineKeyBatchAction : core::u8 {
  Move,
  Duplicate,
  Delete,
  Scale,
  // Interpolation or tangent mode across the selection: `ease`.
  Ease,
  // One key's handles after a Curves handle drag: `tangent`, on the single
  // address in `keys`.
  Tangent,
};

// A complete typed key operation. The panel never authors individual channel
// rewrites for these paths: the Director snapshots and plans the full address
// set through scene::key_edit, then schedules its replacement as one Scene
// transaction.
struct TimelineKeyBatchEdit final {
  TimelineKeyBatchAction action{TimelineKeyBatchAction::Move};
  std::vector<TimelineKeyAddress> keys;
  core::i64 delta{};
  scene::KeyTimeScale scale{};
  scene::KeyEase ease{scene::KeyEase::Linear};
  scene::KeyTangent tangent{};

  bool operator==(const TimelineKeyBatchEdit &) const = default;
};

// Scale preview is read-only and Director-local. `signature` binds a response
// to the exact selected addresses and rational settings currently shown in the
// popup so stale collision counts can never enable Apply.
struct TimelineKeyScalePreviewRequest final {
  core::u64 signature{};
  std::vector<TimelineKeyAddress> keys;
  scene::KeyTimeScale scale{};

  bool operator==(const TimelineKeyScalePreviewRequest &) const = default;
};

struct TimelineKeyScalePreview final {
  core::u64 signature{};
  bool valid{};
  std::size_t selected{};
  std::size_t applied{};
  std::size_t skips{};
  std::size_t collisions{};

  bool operator==(const TimelineKeyScalePreview &) const = default;
};

enum class TimelineKeyClipboardAction : core::u8 {
  Copy,
  Paste,
};

struct TimelineKeyClipboardEdit final {
  TimelineKeyClipboardAction action{TimelineKeyClipboardAction::Copy};
  std::vector<TimelineKeyAddress> keys;

  bool operator==(const TimelineKeyClipboardEdit &) const = default;
};

// A clip's placement after a timeline gesture: moved, trimmed, or its fade
// ramps adjusted. The whole placement travels together because it is one
// document node and one PutNode; emitting five separate property edits for one
// drag would be five commands a frame and five things to keep consistent.
struct TimelineClipEdit final {
  doc::NodeId clip{};
  scene::ClipTiming timing{};
  // Engaged when the drag also moved the clip between lanes. The implicit
  // lane's zero id is a real destination, so the option - not the id - is
  // what distinguishes "no lane change" from "move to the implicit lane".
  std::optional<doc::NodeId> lane;

  bool operator==(const TimelineClipEdit &) const = default;
};

// A cut. The clip is split at `frame`, which becomes the first frame of the
// second placement; both halves keep playing the same source frames they did
// before the cut, so splicing is non-destructive.
struct TimelineClipSplit final {
  doc::NodeId clip{};
  core::i64 frame{};

  bool operator==(const TimelineClipSplit &) const = default;
};

// A row selection request. Layer and Track Shift clicks carry the visible
// row order so expanded property rows and collapsed/filtered layers cannot
// distort a contiguous range. Physical surfaces remain a distinct, singular
// Mapping selection domain.
struct TimelineSelectionRequest final {
  doc::NodeId node{};
  SelectionAction action{SelectionAction::Replace};
  std::vector<doc::NodeId> visible_order;
  // Physical surfaces share document ids with scene layers but belong to the
  // Stage/Mapping selection domain rather than the Inspector's scene domain.
  bool surface{};

  bool operator==(const TimelineSelectionRequest &) const = default;
};

enum class ClipClipboardAction : core::u8 {
  Copy,
  Paste,
};

enum class TimelineLaneAction : core::u8 {
  Create,
  Rename,
  MoveEarlier,
  MoveLater,
  MoveTo,
  ToggleMute,
  ToggleLock,
  DeleteEmpty,
  DeleteClips,
  DeleteMoveClips,
};

// Mute and lock belong to anything the timeline lists, not only to tracks:
// a light, a model, or an LED surface is as worth switching off while working
// as a clip is. Both are one boolean on the element's own node.
enum class TimelineElementFlag : core::u8 {
  Visible,
  Locked,
};

struct TimelineElementFlagEdit final {
  doc::NodeId element{};
  TimelineElementFlag flag{TimelineElementFlag::Visible};
  bool value{};

  bool operator==(const TimelineElementFlagEdit &) const = default;
};

// A layer renamed in place - the same edit the Scene outliner makes, so the
// two lists can never disagree about what something is called. Tracks rename
// through TimelineLaneEdit, which owns their name.
struct TimelineRename final {
  doc::NodeId node{};
  std::string name;

  bool operator==(const TimelineRename &) const = default;
};

enum class TimelineMarkerAction : core::u8 {
  Create,
  Update,
  Delete,
};

// One marker or cue change. The marker travels whole because it is one
// document node: a drag, a rename, a recolour, and turning a marker into a
// Hold cue are all an Update carrying the complete new value. Create ignores
// `marker.id`, which the Director allocates; Delete reads nothing else.
struct TimelineMarkerEdit final {
  TimelineMarkerAction action{TimelineMarkerAction::Create};
  scene::TimelineMarker marker{};

  bool operator==(const TimelineMarkerEdit &) const = default;
};

struct TimelineLaneEdit final {
  TimelineLaneAction action{TimelineLaneAction::Create};
  doc::NodeId lane{};
  std::string name;
  doc::NodeId destination{};

  bool operator==(const TimelineLaneEdit &) const = default;
};

enum class TimelineLayerPlacement : core::u8 {
  Before,
  After,
};

// One insertion in the visible front-to-back layer stack. The Director owns
// validation and dense order authoring; the panel names only stable row ids and
// which side of the destination row the pointer chose.
struct TimelineLayerReorder final {
  doc::NodeId layer{};
  doc::NodeId destination{};
  TimelineLayerPlacement placement{TimelineLayerPlacement::Before};

  bool operator==(const TimelineLayerReorder &) const = default;
};

// Selected layer rows released away from the Timeline. The shell resolves this
// screen-space drop against the Compositor after every dock has drawn, so a
// group becomes Scene Layer nodes without either panel knowing dock layout.
struct TimelineLayerDrop final {
  std::vector<doc::NodeId> layers;
  float screen_x{};
  float screen_y{};

  bool operator==(const TimelineLayerDrop &) const = default;
};

// The pointer tools, as every editor has them. Each acts on whatever clip it
// is pressed on - there is no selection to prepare first - and the advanced
// ones stay semantic all the way to the Director: the panel reports the
// operator's signed show-frame gesture and stable clip addresses, and never
// approximates the document change locally.
enum class TimelineEditTool : core::u8 {
  // Move, trim, and fade.
  Standard,
  // Trim an edge and pull everything after it along.
  RippleTrim,
  // Move the cut between two butted clips.
  Roll,
  // Change which source frames a clip shows without moving it.
  Slip,
  // Move a clip between its neighbours, trimming them to suit.
  Slide,
  // Cut a clip where it is clicked.
  Razor,
};

struct TimelineRippleTrimRequest final {
  doc::NodeId clip{};
  scene::TimelineTrimEdge edge{scene::TimelineTrimEdge::Tail};
  core::i64 delta{};

  bool operator==(const TimelineRippleTrimRequest &) const = default;
};

struct TimelineRippleDeleteRequest final {
  std::vector<doc::NodeId> clips;

  bool operator==(const TimelineRippleDeleteRequest &) const = default;
};

struct TimelineRollRequest final {
  doc::NodeId first{};
  doc::NodeId second{};
  core::i64 delta{};

  bool operator==(const TimelineRollRequest &) const = default;
};

struct TimelineSlipRequest final {
  doc::NodeId clip{};
  core::i64 delta{};

  bool operator==(const TimelineSlipRequest &) const = default;
};

struct TimelineSlideRequest final {
  doc::NodeId clip{};
  core::i64 delta{};

  bool operator==(const TimelineSlideRequest &) const = default;
};

// Insert and Overwrite consume the Director-local clip clipboard at the
// current Stage playhead. Lane zero is the scene's implicit lane and remains
// a valid, explicit target here.
struct TimelineInsertRequest final {
  doc::NodeId lane{};

  bool operator==(const TimelineInsertRequest &) const = default;
};

struct TimelineOverwriteRequest final {
  doc::NodeId lane{};

  bool operator==(const TimelineOverwriteRequest &) const = default;
};

using TimelineEditRequest =
    std::variant<TimelineRippleTrimRequest, TimelineRippleDeleteRequest,
                 TimelineRollRequest, TimelineSlipRequest, TimelineSlideRequest,
                 TimelineInsertRequest, TimelineOverwriteRequest>;

// Runtime-only filmstrip projection for one clip. `key` remains rooted in the
// persistent Library Asset id and exact source fingerprint; the clip id is
// only the row that borrows the atlas this frame.
struct TimelineFilmstripObservation final {
  doc::NodeId clip{};
  assets::DerivativeKey key;
  assets::DerivativeState state{assets::DerivativeState::Missing};
  const rhi::Texture *texture{};

  bool operator==(const TimelineFilmstripObservation &) const = default;
};

enum class TimelineFilmstripRequestPriority : core::u8 {
  Visible,
  Prefetch,
};

struct TimelineFilmstripRequestIntent final {
  assets::DerivativeKey key;
  TimelineFilmstripRequestPriority priority{
      TimelineFilmstripRequestPriority::Visible};

  bool operator==(const TimelineFilmstripRequestIntent &) const = default;
};

struct TimelineFilmstripRetainIntent final {
  std::vector<assets::DerivativeKey> keys;

  bool operator==(const TimelineFilmstripRetainIntent &) const = default;
};

// Runtime-only waveform projection for one clip. The fixed envelope belongs
// to the existing Asset revision; `bins` borrows service-owned storage for the
// duration of this draw and is never persisted by the editor.
struct TimelineWaveformObservation final {
  doc::NodeId clip{};
  assets::DerivativeKey key;
  assets::DerivativeState state{assets::DerivativeState::Missing};
  std::span<const assets::WaveformPeakRms> bins;
};

enum class TimelineWaveformRequestPriority : core::u8 {
  Visible,
  Prefetch,
};

struct TimelineWaveformRequestIntent final {
  assets::DerivativeKey key;
  TimelineWaveformRequestPriority priority{
      TimelineWaveformRequestPriority::Visible};

  bool operator==(const TimelineWaveformRequestIntent &) const = default;
};

struct TimelineWaveformRetainIntent final {
  std::vector<assets::DerivativeKey> keys;

  bool operator==(const TimelineWaveformRetainIntent &) const = default;
};

// The Timeline's horizontal view: which show frame sits at the graph's left
// edge and how many pixels one frame spans. It persists across frames exactly
// as the operator left it; the graph's left edge comes from each frame's
// layout, so every mapping takes it as `origin`.
struct TimelineTimeAxis final {
  static constexpr float minimum_pixels_per_frame = 0.02F;
  static constexpr float maximum_pixels_per_frame = 200.0F;

  double left_frame{};
  float pixels_per_frame{};
  // False until the first frame with room fits the authored show range.
  bool initialized{};

  [[nodiscard]] float x(double frame, float origin) const noexcept {
    return origin + static_cast<float>((frame - left_frame) *
                                       static_cast<double>(pixels_per_frame));
  }
  [[nodiscard]] double frame(float x, float origin) const noexcept {
    return left_frame + static_cast<double>(x - origin) /
                            static_cast<double>(pixels_per_frame);
  }
  // Moves the view by `pixels`; positive shows later frames.
  void pan(double pixels) noexcept {
    left_frame += pixels / static_cast<double>(pixels_per_frame);
  }
  // Shows `span` frames from `begin` across `width` pixels.
  void fit(double begin, double span, float width) noexcept;
  // Scales the zoom by `factor` while `anchor` stays at `anchor_x`.
  void zoom_about(double anchor, float anchor_x, float origin,
                  double factor) noexcept;

  bool operator==(const TimelineTimeAxis &) const = default;
};

// Everything the Timeline draws from in one frame. Show time, the scene's
// elements and keys, the shell's selection, and the runtime derivative
// observations arrive fresh every frame; the panel keeps none of them.
struct TimelineDrawInput final {
  EditorTimeline timeline{};
  std::span<const scene::ChannelKeyframes> keyframes;
  std::span<const ElementInfo> elements;
  // The shell's current selection and its anchor, used by row-local key
  // diamonds and the clear action.
  std::span<const doc::NodeId> selection;
  doc::NodeId primary{};
  std::span<const scene::EffectInstance> effects;
  std::span<const output::ProjectionSurface> surfaces;
  doc::NodeId selected_surface{};
  std::span<const scene::TimelineLane> lanes;
  std::optional<core::i64> clipboard_span;
  std::optional<TimelineKeyScalePreview> key_scale_preview;
  std::span<const TimelineFilmstripObservation> filmstrips;
  std::span<const TimelineWaveformObservation> waveforms;
  std::span<const scene::TimelineMarker> markers;
};

struct TimelinePanelFrame final {
  std::optional<core::u64> seek_to_frame;
  std::vector<TimelineKeyEdit> keys;
  std::vector<TimelineKeyMoveEdit> key_moves;
  std::optional<TimelineKeyDuplicateEdit> key_duplicate;
  std::optional<TimelineKeyBatchEdit> key_batch;
  std::optional<TimelineKeyScalePreviewRequest> key_scale_preview;
  std::optional<TimelineKeyClipboardEdit> key_clipboard;
  // A dropped clip placement. The panel previews the lifted bar locally and
  // reports exactly one edit on release, matching every other list-to-target
  // drag surface in the editor.
  std::vector<TimelineClipEdit> clip_edits;
  std::vector<TimelineClipSplit> clip_splits;
  std::vector<TimelineLaneEdit> lane_edits;
  std::vector<TimelineLayerReorder> layer_reorders;
  std::optional<TimelineLayerDrop> layer_drop;
  std::vector<TimelineElementFlagEdit> element_flags;
  std::vector<TimelineRename> renames;
  std::vector<TimelineEditRequest> timeline_edits;
  // The visible gesture readout is independent of the clip bar's width.
  std::string clip_status;
  bool clip_status_error{};
  // Marker and cue changes from one gesture. A drag of several selected
  // markers reports one Update each, which the shell gives one undo step.
  std::vector<TimelineMarkerEdit> marker_edits;
  std::optional<TimelineSelectionRequest> selection;
  TransportRequest transport{TransportRequest::None};
  std::optional<RangeRequest> range;
  std::optional<ShowRateRequest> show_rate;
  // The operator armed or disarmed every cue.
  std::optional<bool> cues_armed;
  // Exact machine-local derivative work for visible clip rows plus the
  // bounded adjacent-row prefetch window. These never author the document.
  std::vector<TimelineFilmstripRequestIntent> filmstrip_requests;
  std::optional<TimelineFilmstripRetainIntent> filmstrip_retain;
  std::vector<TimelineWaveformRequestIntent> waveform_requests;
  std::optional<TimelineWaveformRetainIntent> waveform_retain;
  // Exact range entry owns the keyboard while true. The shell uses this to
  // keep global shortcuts from firing through the field.
  bool editing{};
  // Delete was aimed at selected timeline keys, so the shell must not also
  // delete the selected scene layers.
  bool consume_delete{};
  // Ctrl+C/Ctrl+V belonged to selected timeline keys, so the shell must not
  // also route the same chord to the clip clipboard.
  bool consume_clipboard{};
  bool lane_editing{};
};

class TimelinePanel final {
public:
  explicit TimelinePanel(gui::UiId id);
  ~TimelinePanel();
  TimelinePanel(const TimelinePanel &) = delete;
  TimelinePanel &operator=(const TimelinePanel &) = delete;

  // Draws the toolbar, time ruler, layer tree, key lanes, and playhead into
  // `bounds`.
  TimelinePanelFrame draw(gui::UiContext &ctx, const gui::Rect &bounds,
                          const TimelineDrawInput &input);

  // The same frame with each input passed separately. `selection` is the
  // shell's current selection and `primary` is its anchor.
  TimelinePanelFrame
  draw(gui::UiContext &ctx, const gui::Rect &bounds,
       const EditorTimeline &timeline,
       std::span<const scene::ChannelKeyframes> keyframes,
       std::span<const ElementInfo> elements,
       std::span<const doc::NodeId> selection, doc::NodeId primary = {},
       std::span<const scene::EffectInstance> effects = {},
       std::span<const output::ProjectionSurface> surfaces = {},
       doc::NodeId selected_surface = {},
       std::span<const scene::TimelineLane> lanes = {},
       std::optional<core::i64> clipboard_span = std::nullopt,
       std::optional<TimelineKeyScalePreview> key_scale_preview = std::nullopt,
       std::span<const TimelineFilmstripObservation> filmstrips = {},
       std::span<const TimelineWaveformObservation> waveforms = {},
       std::span<const scene::TimelineMarker> markers = {});

  struct ReplacementTarget final {
    doc::NodeId node{};
    gui::Rect bounds{};
  };
  [[nodiscard]] std::span<const ReplacementTarget> replacement_targets() const noexcept {
    return replacement_targets_;
  }

  // Replaces only the Timeline's typed key selection after the Director has
  // accepted an atomic plan. Addresses remain unique and sorted; no Scene
  // layer, mapping, compositor, or Selection Set carrier is touched.
  void replace_key_selection(std::span<const TimelineKeyAddress> selection);

  // The application Edit menu asks the active editing surface for its typed
  // clipboard request. Keeping the key addresses here lets broad Copy/Paste
  // live in one conventional menu without flattening key and clip clipboards
  // into one ambiguous payload.
  [[nodiscard]] std::optional<TimelineKeyClipboardEdit>
  key_clipboard_edit(TimelineKeyClipboardAction action) const;

  // A Stage-scene switch invalidates every typed key address without changing
  // Timeline view preferences. Close document-addressed tools and visibility
  // state so a stale key domain cannot author into the newly displayed scene.
  void clear_key_context() noexcept;

  [[nodiscard]] std::size_t selected_key_count() const noexcept {
    return keys_.selected.size();
  }
  [[nodiscard]] bool dragging_clip() const noexcept {
    return clip_.drag != ClipDrag::None;
  }
  [[nodiscard]] bool dragging_layer() const noexcept {
    return reorder_.layer_node != doc::NodeId{};
  }
  [[nodiscard]] doc::NodeId dragging_layer_node() const noexcept {
    return reorder_.layer_node;
  }
  [[nodiscard]] bool dragging_compositor_source() const noexcept {
    return reorder_.lane_node != doc::NodeId{} || dragging_layer();
  }
  [[nodiscard]] doc::NodeId dragging_compositor_source_node() const noexcept {
    return reorder_.lane_node != doc::NodeId{} ? reorder_.lane_node
                                            : reorder_.layer_node;
  }
  [[nodiscard]] std::size_t dragging_compositor_source_count() const noexcept {
    return reorder_.layer_nodes.size();
  }
  [[nodiscard]] bool snapping_enabled() const noexcept { return snapping_; }
  [[nodiscard]] bool auto_key_enabled() const noexcept { return auto_key_; }
  // Whether the cache bar and the render nodes' playheads are drawn.
  [[nodiscard]] bool playback_overlay_enabled() const noexcept {
    return playback_overlay_;
  }
  [[nodiscard]] std::string_view search() const noexcept { return search_; }
  [[nodiscard]] std::span<const doc::NodeId> selected_markers() const noexcept {
    return markers_.selected;
  }
  [[nodiscard]] bool dragging_marker() const noexcept {
    return markers_.drag != doc::NodeId{};
  }
  // Selects one marker from outside the panel - the Cue Stack's row click -
  // so both views agree on which cue the operator is working on.
  void select_marker(doc::NodeId marker);
  [[nodiscard]] TimelineEditTool edit_tool() const noexcept {
    return edit_tool_;
  }
  [[nodiscard]] bool lasso_mode() const noexcept { return keys_.lasso_mode; }
  [[nodiscard]] bool renaming() const noexcept { return rename_.active(); }
  // The layer list's width, as the operator last dragged it; zero until they
  // do, which lets it follow the panel's own width.
  [[nodiscard]] float list_width() const noexcept { return list_width_; }
  [[nodiscard]] bool scale_open() const noexcept { return key_tools_.scale_open; }
  [[nodiscard]] bool curves_mode() const noexcept { return graph_mode_; }
  // The Curves view's value zoom and pan since its last fit: 1 and 0 right
  // after one.
  [[nodiscard]] float curve_value_zoom() const noexcept;
  [[nodiscard]] float curve_value_pan() const noexcept;
  [[nodiscard]] bool channel_visible(doc::NodeId target,
                                     scene::PropertyChannel property) const
      noexcept;

  // Brings a layer's or track's row into view - opening the track and the
  // groups it sits in - and outlines it for a moment. The selection is left
  // as it is: this is how another panel points at a row, not a choice.
  void reveal(doc::NodeId node, double now) noexcept {
    reveal_node_ = node;
    reveal_scroll_ = node != doc::NodeId{};
    reveal_until_ = now + 1.5;
  }
  // The row most recently pointed at this way, while it is outlined.
  [[nodiscard]] doc::NodeId revealed_row(double now) const noexcept {
    return now < reveal_until_ ? reveal_node_ : doc::NodeId{};
  }

  // A successful project replacement invalidates document-addressed UI
  // state and requests a fresh fit on the next draw. View preferences such
  // as snapping and graph/dope-sheet mode intentionally survive the load.
  void reset_project_state() noexcept;

private:
  // Values one frame's sections hand to later ones: hit lists gathered while
  // rows draw, right-click targets the context menu reads, and the drag
  // feedback drawn last. Rebuilt on every draw; never outlives it.
  struct FrameScratch;
  // One draw's working state, handed from each section of draw() to the next.
  struct Pass;

  // The sections of draw(), in the order it runs them.
  [[nodiscard]] bool open_pass(Pass &p);
  void prune_key_selection(Pass &p);
  void navigate(Pass &p);
  void request_derivatives(Pass &p);
  void draw_toolbar(Pass &p);
  void draw_ruler(Pass &p);
  void draw_marker_strip(Pass &p);
  void track_key_drag(Pass &p);
  void resolve_clip_gesture(Pass &p);
  void resolve_reorder_targets(Pass &p);
  void draw_rows(Pass &p);
  void resolve_track_drop(Pass &p);
  void finish_marquee(Pass &p);
  void draw_time_overlays(Pass &p);
  // The cache bar along the top of the marker strip and each render node's
  // playhead, under the editor's own.
  void draw_playback_overlay(Pass &p);
  void commit_key_drag(Pass &p);
  void draw_key_tools(Pass &p);
  void draw_rate_menu(Pass &p);
  void draw_drag_feedback(Pass &p);
  void handle_actions(Pass &p);
  void close_pass(Pass &p);

  // One visible row, handed from draw_rows() to its cell renderers.
  struct RowContext;
  // Each row kind's list cell and lane cell, the clip bar's parts, and the
  // presses the row label and then the lane take, in draw_rows() order.
  void layout_row(RowContext &rc);
  void collect_row_hits(RowContext &rc);
  void collect_list_hits(RowContext &rc);
  void begin_row(RowContext &rc);
  void track_header_list_cell(RowContext &rc);
  void layer_list_cell(RowContext &rc);
  void properties_header_list_cell(RowContext &rc);
  void property_group_list_cell(RowContext &rc);
  void channel_list_cell(RowContext &rc);
  void property_label(RowContext &rc, float right);
  void channel_visibility_toggle(RowContext &rc);
  void row_key_button(RowContext &rc);
  void row_key_steps(RowContext &rc);
  void channel_value_cell(RowContext &rc);
  void group_value_cell(RowContext &rc);
  bool row_label_press(RowContext &rc);
  void layer_lane_cell(RowContext &rc);
  // A closed parent's read-only summary: its span bar, unless a clip bar
  // already shows it, and its key ticks.
  void summary_lane_cell(RowContext &rc, bool span_bar);
  void clip_bar_cell(RowContext &rc);
  bool draw_clip_filmstrip(RowContext &rc, bool live);
  void draw_clip_waveform(RowContext &rc, bool drew_filmstrip);
  void draw_clip_edges(RowContext &rc);
  void draw_clip_ramps(RowContext &rc);
  void draw_clip_label(RowContext &rc);
  void draw_clip_hover(RowContext &rc);
  void property_group_lane_cell(RowContext &rc);
  void channel_lane_cell(RowContext &rc);
  void lane_press(RowContext &rc);
  // A right press anywhere across the row opens the row's menu.
  void note_row_menu_press(RowContext &rc);

  // Curves mode: where the graph is, what it shows, and the graph itself.
  [[nodiscard]] gui::Rect graph_area(const Pass &p) const noexcept;
  void collect_graph(Pass &p, std::span<const RowContext> visible);
  void draw_graph(Pass &p);

  // Queries and gestures more than one section reaches.
  std::string timecode(Pass &p, core::u64 value);
  std::optional<std::size_t> marker_index(Pass &p, doc::NodeId id);
  void capture_layer_drag(Pass &p, doc::NodeId grabbed);
  std::vector<TimelineKeyAddress> selected_addresses() const;
  const TimelineFilmstripObservation *
  filmstrip_for_clip(Pass &p, doc::NodeId clip) noexcept;
  const TimelineWaveformObservation *
  waveform_for_clip(Pass &p, doc::NodeId clip) noexcept;
  void zoom_to_fit(Pass &p) noexcept;
  void create_marker(Pass &p, core::i64 at, scene::CueAction cue);
  std::string cue_label(Pass &p, doc::NodeId id);
  std::string seconds_label(Pass &p, core::u64 frames);
  std::vector<core::i64> edit_points(Pass &p,
                                     doc::NodeId exclude_clip,
                                     bool include_markers);
  core::i64 razor_frame(Pass &p, float px);
  core::i64 shown_marker_frame(const scene::TimelineMarker &marker) const;
  void update_key_drag(Pass &p);
  std::optional<doc::NodeId> clip_lane_under_pointer(Pass &p, float pointer_y);

  std::vector<ReplacementTarget> replacement_targets_;

  // Which part of a clip bar a drag grabbed. Move slides the placement;
  // TrimIn and TrimOut change one edge while the source stays put under the
  // other; the fade handles set the head and tail ramp lengths.
  enum class ClipDrag : core::u8 {
    None,
    Move,
    TrimIn,
    TrimOut,
    FadeIn,
    FadeOut,
    RippleTrimHead,
    RippleTrimTail,
    Roll,
    Slip,
    Slide,
  };

  struct SelectedKey final {
    doc::NodeId target{};
    scene::PropertyChannel component{};
    core::i64 frame{};

    bool operator==(const SelectedKey &) const = default;
  };

  using ChannelIdentity = timeline::ChannelId;

  // Presses in a lane or on the graph: a key selects and lifts the
  // selection, an empty lane or graph clears the selection, seeks and starts
  // a marquee, and holding it scrubs.
  void press_key(Pass &p, const SelectedKey &hit);
  void press_lane_background(Pass &p, bool locked);
  void scrub_held_lane(Pass &p, const gui::Interaction &lane);
  // Seeks to the nearest key before or after the playhead on the selected
  // layers' channels.
  void step_to_selected_key(Pass &p, bool forward);
  // Asks the Director to ease every selected key: one batch, one undo step.
  void ease_selected_keys(Pass &p, scene::KeyEase ease);
  // The unlocked key that owns the pointer this frame, in a lane or on the
  // Curves graph.
  [[nodiscard]] std::optional<SelectedKey> key_under_pointer(Pass &p) const;
  // Whether any selected key is Bezier, so has handles a mode can set.
  [[nodiscard]] bool selection_has_cubic(const Pass &p) const;

  [[nodiscard]] bool
  selected(doc::NodeId node,
           std::span<const doc::NodeId> selection) const noexcept;
  [[nodiscard]] bool key_selected(const SelectedKey &key) const noexcept;
  void canonicalize_key_selection() noexcept;

  gui::UiId id_{};

  // Time view. The first non-empty frame fits the authored show range; after
  // that wheel zoom and Shift+wheel pan persist exactly as the operator left
  // them.
  TimelineTimeAxis time_axis_;

  float row_scroll_{};
  std::string search_;
  // Object hierarchy and animated properties fold independently. Collections
  // start closed, and selecting a descendant elsewhere reveals its ancestors.
  timeline::RowsState rows_state_;
  doc::NodeId selected_lane_{};
  // Renaming any row - a layer or a track - is a double-click or F2, so a
  // single click can mean what it means everywhere else: select the thing.
  InlineRename rename_;
  ClickTracker clicks_;
  // The operator's width for the layer list; zero follows the panel.
  float list_width_{};

  // Lifted layers and tracks.
  struct ReorderState final {
    // A track drag follows the same lift/target/drop contract as Feed
    // Mapping, but uses an insertion cursor rather than target-band
    // highlighting because tracks reorder between peers; they never nest
    // inside one another.
    doc::NodeId lane_node{};
    float lane_start_x{};
    float lane_start_y{};
    bool lane_moved{};
    doc::NodeId lane_destination{};
    // Objects, models, lights, loose clips, groups, and clips inside one
    // track share the same lift/insert gesture. The Director decides which
    // order domain applies; this state remains pointer-local.
    doc::NodeId layer_node{};
    // Snapshot the selected rows at lift, in timeline order. Selection may
    // be updated by the shell while the pointer is travelling between
    // panels.
    std::vector<doc::NodeId> layer_nodes;
    float layer_start_x{};
    float layer_start_y{};
    bool layer_moved{};
  };
  ReorderState reorder_;

  NumberField range_fields_;
  // Each channel row's value at the playhead.
  NumberField value_fields_;

  // The Timeline's own key selection, kept apart from scene layers.
  struct KeySelection final {
    std::vector<SelectedKey> selected;
    // Every key selected when a drag begins moves by one shared delta. The
    // source list remains fixed until release, so document latency cannot
    // pull the diamonds back underneath the pointer.
    bool dragging{};
    bool duplicate{};
    std::vector<SelectedKey> drag_keys;
    core::i64 drag_anchor_frame{};
    core::i64 drag_delta{};
    std::optional<core::i64> snap_guide;
    // An empty-lane press still seeks immediately, but travelling beyond a
    // short threshold turns it into the familiar dope-sheet selection box,
    // or a freehand lasso in lasso mode.
    bool lasso_mode{};
    bool marquee_tracking{};
    bool marquee_active{};
    float marquee_start_x{};
    float marquee_start_y{};
    bool marquee_shift{};
    bool marquee_ctrl{};
    std::vector<SelectedKey> marquee_base;
    std::vector<gui::UiPoint> lasso_points;
  };
  KeySelection keys_;

  // A Curves handle drag, previewed on a copy of the key's channel and
  // committed once, on release, as one tangent edit. The copy stays on
  // screen until the edited channel comes back or a second has passed.
  struct HandleDrag final {
    bool active{};
    SelectedKey key;
    bool incoming{};
    scene::ChannelKeyframes base;
    scene::ChannelKeyframes preview;
    int hold_frames{};
  };
  HandleDrag handle_drag_;
  void press_handle(Pass &p, std::size_t handle);
  void track_handle_drag(Pass &p);

  bool snapping_{true};
  bool graph_mode_{};
  // The cache bar and node playheads. A view preference, like snapping.
  bool playback_overlay_{true};
  // The KEYS popup and its time-scale fields.
  struct KeyToolsState final {
    bool open{};
    bool scale_open{};
    scene::KeyTimeAnchor scale_anchor{scene::KeyTimeAnchor::FirstSelected};
    std::string scale_numerator{"1"};
    std::string scale_denominator{"1"};
    std::string scale_user_anchor{"0"};
    // The active channel's value at the playhead.
    NumberField value_field;
  };
  KeyToolsState key_tools_;
  // The Curves view: its value axis, which channels are shown and which one
  // is active. It shares time_axis_ and keys_ with the dope sheet, so the
  // two zoom, pan and select together.
  std::unique_ptr<timeline::TimelineGraphView> graph_;
  TimelineEditTool edit_tool_{TimelineEditTool::Standard};
  bool timeline_focused_{};
  bool panning_{};

  // A live clip gesture. The proposal is held here and drawn from here for
  // the same reason a key drag is: the committed placement can lag the
  // pointer by several frames, and a bar that snapped back underneath the
  // cursor would be unusable.
  struct ClipGesture final {
    ClipDrag drag{ClipDrag::None};
    doc::NodeId node{};
    doc::NodeId peer{};
    scene::ClipTiming origin{};
    scene::ClipTiming proposed{};
    // Keep a released gesture visible while its single edit reaches
    // authority.
    std::optional<TimelineClipEdit> pending_edit;
    scene::ClipTiming pending_origin{};
    core::u32 pending_frames{};
    std::string status;
    bool status_error{};
    double status_until{};
    core::i64 anchor{};
    core::i64 delta{};
    // Clip bars use the same lift contract as tracks and Feed Mapping. A
    // press can remain an ordinary selection, while a real drag previews one
    // destination and commits once on release. Layer-row drags use the
    // dedicated stack gesture; bar drags change show time and may change
    // tracks.
    float start_x{};
    float start_y{};
    std::optional<gui::UiPoint> release;
    bool moved{};
    std::optional<doc::NodeId> drop_lane;
    std::optional<core::i64> snap_guide;
    // A press on a clip that did its work on the press - a razor cut - and
    // must not turn into a scrub while the button stays down.
    bool press_consumed{};
  };
  ClipGesture clip_;
  double previous_draw_time_{};
  bool auto_key_{true};
  // A row another panel pointed at: still to be scrolled to, and outlined
  // until reveal_until_.
  doc::NodeId reveal_node_{};
  bool reveal_scroll_{};
  double reveal_until_{};

  // The marker strip's state.
  struct MarkerState final {
    // Markers keep their own selection, separate from scene layers:
    // selecting a cue to move it must not take the Inspector away from the
    // clip being timed against it. Sorted and unique.
    std::vector<doc::NodeId> selected;
    // A marker drag. Like a clip, the dragged markers preview from the panel
    // and the document changes once, on release.
    doc::NodeId drag{};
    float drag_start_x{};
    core::i64 drag_anchor{};
    core::i64 drag_delta{};
    bool drag_moved{};
    std::optional<core::i64> snap_guide;
    // Double-click detection and the inline name field, for markers.
    doc::NodeId click{};
    double click_time{};
    double strip_click_time{-1.0};
    float strip_click_x{};
    doc::NodeId name_node{};
    std::string name_draft;
  };
  MarkerState markers_;

  // The In/Out handles on the ruler, dragged locally and committed once.
  enum class RangeHandle : core::u8 { None, In, Out };

  // The toolbar's own state: the show-rate menu, and typed time entry - the
  // playhead readout and the In/Out fields all scrub on a drag and open for
  // typing on a click.
  struct ToolbarState final {
    bool rate_menu_open{};
    gui::UiId time_drag_id{};
    float time_drag_start_x{};
    core::u64 time_drag_origin{};
    bool time_drag_moved{};
    gui::UiId time_edit_id{};
    std::string time_draft;
  };
  ToolbarState toolbar_;

  struct RulerState final {
    RangeHandle range_drag{RangeHandle::None};
    core::u64 range_drag_value{};
    // The ruler reads in timecode or in frames; both are the same show
    // frames.
    bool frames{};
  };
  RulerState ruler_;

  // The last seek this panel asked for, until the Director's playhead
  // arrives there. Keys act on it so they never lag the hand.
  std::optional<core::u64> requested_playhead_;
  core::u32 requested_playhead_frames_{};

  // The one open context menu. Its entries are fixed when it opens, so what
  // the operator reads is exactly what a choice will do even if the document
  // moves underneath it.
  struct MenuEntry final {
    std::string label;
    core::u8 command{};
    doc::NodeId node{};
    doc::NodeId other{};
    core::i64 frame{};
    core::i64 end{};
  };
  std::vector<MenuEntry> menu_entries_;
};

} // namespace mediaserver::editor
