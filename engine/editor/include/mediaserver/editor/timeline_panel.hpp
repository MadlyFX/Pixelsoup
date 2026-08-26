#pragma once

#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/editor/element.hpp>
#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/gui/layout.hpp>
#include <mediaserver/gui/ui_context.hpp>
#include <mediaserver/scene/scene.hpp>

#include <cstddef>
#include <optional>
#include <set>
#include <span>
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

// Where the show is. It is passed in each frame because transport owns it; the
// timeline only reflects it and asks to seek or change its range.
struct EditorTimeline final {
    core::u64 show_frame{};
    core::u64 first_frame{};
    core::u64 last_frame{};
    bool playing{};
    bool loop{};
    core::u32 rate_numerator{24U};
    core::u32 rate_denominator{1U};
};

// A change to the show's range and loop. The Director applies it to transport
// and clamps the playhead into the result.
struct RangeRequest final {
    core::u64 first{};
    core::u64 last{};
    bool loop{};

    bool operator==(const RangeRequest&) const = default;
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

    bool operator==(const TimelineKeyEdit&) const = default;
};

// A key retime authored once on release. Holding the drag locally is important:
// the committed key list can lag several frames behind the pointer without
// pulling the diamond back to where it began.
struct TimelineKeyMoveEdit final {
    doc::NodeId target{};
    scene::TransformChannel component{};
    core::i64 from_frame{};
    core::i64 to_frame{};

    bool operator==(const TimelineKeyMoveEdit&) const = default;
};

// A layer-row selection request. `toggle` is set for Shift/Ctrl-click;
// otherwise the shell replaces the selection with `node`.
struct TimelineSelectionRequest final {
    doc::NodeId node{};
    bool toggle{};

    bool operator==(const TimelineSelectionRequest&) const = default;
};

struct TimelinePanelFrame final {
    std::optional<core::u64> seek_to_frame;
    std::vector<TimelineKeyEdit> keys;
    std::vector<TimelineKeyMoveEdit> key_moves;
    std::optional<TimelineSelectionRequest> selection;
    TransportRequest transport{TransportRequest::None};
    std::optional<RangeRequest> range;
    // Exact range entry owns the keyboard while true. The shell uses this to
    // keep global shortcuts from firing through the field.
    bool editing{};
    // Delete was aimed at selected timeline keys, so the shell must not also
    // delete the selected scene layers.
    bool consume_delete{};
};

class TimelinePanel final {
public:
    explicit TimelinePanel(gui::UiId id) noexcept : id_(id) {}

    // Draws the toolbar, time ruler, layer tree, key lanes, and playhead into
    // `bounds`. `selection` is the shell's current selection and `primary` is
    // its anchor, used by row-local key diamonds and the clear action.
    TimelinePanelFrame draw(gui::UiContext& ctx, const gui::Rect& bounds,
                            const EditorTimeline& timeline,
                            std::span<const scene::ChannelKeyframes> keyframes,
                            std::span<const ElementInfo> elements,
                            std::span<const doc::NodeId> selection,
                            doc::NodeId primary = {},
                            std::span<const scene::EffectInstance> effects = {});

    [[nodiscard]] std::size_t selected_key_count() const noexcept {
        return selected_keys_.size();
    }
    [[nodiscard]] bool snapping_enabled() const noexcept { return snapping_; }

    // A successful project replacement invalidates document-addressed UI
    // state and requests a fresh fit on the next draw. View preferences such
    // as snapping and graph/dope-sheet mode intentionally survive the load.
    void reset_project_state() noexcept;

private:
    struct SelectedKey final {
        doc::NodeId target{};
        scene::TransformChannel component{};
        core::i64 frame{};

        bool operator==(const SelectedKey&) const = default;
    };

    [[nodiscard]] bool
    selected(doc::NodeId node,
             std::span<const doc::NodeId> selection) const noexcept;
    [[nodiscard]] bool key_selected(const SelectedKey& key) const noexcept;

    gui::UiId id_{};

    // Time view. The first non-empty frame fits the authored show range; after
    // that wheel zoom and Shift+wheel pan persist exactly as the operator left
    // them.
    float pixels_per_frame_{};
    double left_frame_{};
    bool time_view_initialized_{};

    float row_scroll_{};
    std::set<core::u64> expanded_layers_;
    std::set<core::u64> expanded_groups_;

    NumberField range_fields_;

    std::vector<SelectedKey> selected_keys_;

    // Every key selected when a drag begins moves by one shared delta. The
    // source list remains fixed until release, so document latency cannot pull
    // the diamonds back underneath the pointer.
    bool dragging_key_{};
    std::vector<SelectedKey> drag_keys_;
    core::i64 drag_anchor_frame_{};
    core::i64 drag_delta_{};
    std::optional<core::i64> snap_guide_;

    bool snapping_{true};
    bool graph_mode_{};
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
};

} // namespace mediaserver::editor
