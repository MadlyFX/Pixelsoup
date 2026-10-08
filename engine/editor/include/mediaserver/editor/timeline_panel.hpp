#pragma once

#include <mediaserver/assets/derivative.hpp>
#include <mediaserver/assets/waveform.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/editor/selection.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/scene/key_edit.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/scene/timeline_edit.hpp>

#include <compare>
#include <cstddef>
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
};

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
  core::u64 frame{};

  bool operator==(const TimelineClipSplit &) const = default;
};

// A layer-row selection request. Scene-layer Shift clicks carry the visible
// visible layer order so expanded property rows and collapsed/filtered layers cannot
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

// A layer row released away from the Timeline. The shell resolves this
// screen-space drop against the Compositor after every dock has drawn, so a
// row can become a Scene Layer node without either panel knowing dock layout.
struct TimelineLayerDrop final {
  doc::NodeId layer{};
  float screen_x{};
  float screen_y{};

  bool operator==(const TimelineLayerDrop &) const = default;
};

// Advanced editorial tools stay semantic all the way to the Director. The
// panel reports the operator's signed show-frame gesture and stable clip/lane
// addresses; it never approximates the document changes locally.
enum class TimelineEditTool : core::u8 {
  Standard,
  RippleTrim,
  RippleDelete,
  Roll,
  Slip,
  Slide,
  Insert,
  Overwrite,
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
  std::vector<TimelineEditRequest> timeline_edits;
  std::optional<TimelineSelectionRequest> selection;
  TransportRequest transport{TransportRequest::None};
  std::optional<RangeRequest> range;
  std::optional<ShowRateRequest> show_rate;
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
  explicit TimelinePanel(gui::UiId id) noexcept : id_(id) {}

  // Draws the toolbar, time ruler, layer tree, key lanes, and playhead into
  // `bounds`. `selection` is the shell's current selection and `primary` is
  // its anchor, used by row-local key diamonds and the clear action.
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
       std::span<const TimelineWaveformObservation> waveforms = {});

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
    return selected_keys_.size();
  }
  [[nodiscard]] bool dragging_clip() const noexcept {
    return clip_drag_ != ClipDrag::None;
  }
  [[nodiscard]] bool dragging_layer() const noexcept {
    return layer_drag_node_ != doc::NodeId{};
  }
  [[nodiscard]] doc::NodeId dragging_layer_node() const noexcept {
    return layer_drag_node_;
  }
  [[nodiscard]] bool dragging_compositor_source() const noexcept {
    return lane_drag_node_ != doc::NodeId{} || dragging_layer();
  }
  [[nodiscard]] doc::NodeId dragging_compositor_source_node() const noexcept {
    return lane_drag_node_ != doc::NodeId{} ? lane_drag_node_
                                            : layer_drag_node_;
  }
  [[nodiscard]] bool snapping_enabled() const noexcept { return snapping_; }
  [[nodiscard]] std::string_view search() const noexcept { return search_; }
  [[nodiscard]] TimelineEditTool edit_tool() const noexcept {
    return edit_tool_;
  }
  [[nodiscard]] bool lasso_mode() const noexcept { return lasso_mode_; }
  [[nodiscard]] bool scale_open() const noexcept { return scale_open_; }
  [[nodiscard]] float curve_value_zoom() const noexcept {
    return curve_value_zoom_;
  }
  [[nodiscard]] float curve_value_pan() const noexcept {
    return curve_value_pan_;
  }
  [[nodiscard]] bool
  channel_visible(doc::NodeId target,
                  scene::PropertyChannel property) const noexcept {
    return !hidden_channels_.contains(ChannelIdentity{target, property});
  }

  // A successful project replacement invalidates document-addressed UI
  // state and requests a fresh fit on the next draw. View preferences such
  // as snapping and graph/dope-sheet mode intentionally survive the load.
  void reset_project_state() noexcept;

private:
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

  struct ChannelIdentity final {
    doc::NodeId target{};
    scene::PropertyChannel property{};

    auto operator<=>(const ChannelIdentity &) const = default;
  };

  [[nodiscard]] bool
  selected(doc::NodeId node,
           std::span<const doc::NodeId> selection) const noexcept;
  [[nodiscard]] bool key_selected(const SelectedKey &key) const noexcept;
  void canonicalize_key_selection() noexcept;

  gui::UiId id_{};

  // Time view. The first non-empty frame fits the authored show range; after
  // that wheel zoom and Shift+wheel pan persist exactly as the operator left
  // them.
  float pixels_per_frame_{};
  double left_frame_{};
  bool time_view_initialized_{};

  float row_scroll_{};
  std::string search_;
  std::set<core::u64> expanded_layers_;
  std::set<core::u64> expanded_groups_;
  // Object hierarchy and animated properties fold independently. Collections
  // start closed, and selecting a descendant elsewhere reveals its ancestors.
  std::set<doc::NodeId> expanded_hierarchies_;
  doc::NodeId hierarchy_selection_{};
  doc::NodeId selected_lane_{};
  // A track drag follows the same lift/target/drop contract as Feed Mapping,
  // but uses an insertion cursor rather than target-band highlighting because
  // tracks reorder between peers; they never nest inside one another.
  doc::NodeId lane_drag_node_{};
  float lane_drag_start_x_{};
  float lane_drag_start_y_{};
  bool lane_drag_moved_{};
  doc::NodeId lane_destination_{};
  doc::NodeId lane_name_node_{};
  std::string lane_name_draft_;
  // Renaming a track is a double-click, so a single click can mean what it
  // means everywhere else in the panel: select the thing.
  doc::NodeId lane_click_node_{};
  double lane_click_time_{};

  // Objects, models, lights, loose clips, groups, and clips inside one track
  // share the same lift/insert gesture. The Director decides which order
  // domain applies; this state remains pointer-local.
  doc::NodeId layer_drag_node_{};
  float layer_drag_start_x_{};
  float layer_drag_start_y_{};
  bool layer_drag_moved_{};

  NumberField range_fields_;

  std::vector<SelectedKey> selected_keys_;

  // Every key selected when a drag begins moves by one shared delta. The
  // source list remains fixed until release, so document latency cannot pull
  // the diamonds back underneath the pointer.
  bool dragging_key_{};
  bool duplicate_drag_{};
  std::vector<SelectedKey> drag_keys_;
  core::i64 drag_anchor_frame_{};
  core::i64 drag_delta_{};
  std::optional<core::i64> snap_guide_;

  bool snapping_{true};
  bool graph_mode_{};
  bool lasso_mode_{};
  bool key_tools_open_{};
  bool scale_open_{};
  scene::KeyTimeAnchor scale_anchor_{scene::KeyTimeAnchor::FirstSelected};
  std::string scale_numerator_{"1"};
  std::string scale_denominator_{"1"};
  std::string scale_user_anchor_{"0"};
  float curve_value_zoom_{1.0F};
  float curve_value_pan_{};
  std::set<ChannelIdentity> hidden_channels_;
  std::optional<ChannelIdentity> active_channel_;
  NumberField key_value_field_;
  bool rate_menu_open_{};
  bool edit_menu_open_{};
  TimelineEditTool edit_tool_{TimelineEditTool::Standard};
  bool timeline_focused_{};
  bool panning_{};

  // An empty-lane press still seeks immediately, but travelling beyond a
  // short threshold turns it into the familiar dope-sheet selection box.
  bool marquee_tracking_{};
  bool marquee_active_{};
  float marquee_start_x_{};
  float marquee_start_y_{};
  bool marquee_shift_{};
  bool marquee_ctrl_{};
  std::vector<SelectedKey> marquee_base_;
  std::vector<gui::UiPoint> lasso_points_;

  // A live clip gesture. The proposal is held here and drawn from here for
  // the same reason a key drag is: the committed placement can lag the
  // pointer by several frames, and a bar that snapped back underneath the
  // cursor would be unusable.
  ClipDrag clip_drag_{ClipDrag::None};
  doc::NodeId clip_drag_node_{};
  doc::NodeId clip_drag_peer_{};
  scene::ClipTiming clip_drag_origin_{};
  scene::ClipTiming clip_drag_proposed_{};
  core::i64 clip_drag_anchor_{};
  core::i64 clip_drag_delta_{};
  // Clip bars use the same lift contract as tracks and Feed Mapping. A press
  // can remain an ordinary selection, while a real drag previews one
  // destination and commits once on release. Layer-row drags use the dedicated
  // stack gesture above; bar drags change show time and may change tracks.
  float clip_drag_start_x_{};
  float clip_drag_start_y_{};
  bool clip_drag_moved_{};
  std::optional<doc::NodeId> clip_drop_lane_;
  std::optional<core::i64> clip_snap_guide_;
};

} // namespace mediaserver::editor
