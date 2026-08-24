#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/doc/ids.hpp>
#include <mediaserver/scene/math.hpp>
#include <mediaserver/scene/scene.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

// The operator editor: a Dear ImGui surface for placing scene elements.
//
// This is the throwaway shell of §9's plan - a real but disposable UI, isolated
// in this module so the bespoke operator surface can replace it without
// touching anything else. It exists to prove the interaction model: the 2D wall
// view and (later) a 3D view are two editors of the one replicated scene, and a
// drag in either resolves - through the pure math in scene::manipulate - to a
// document command.
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
// single undo step even though it authors one command per sealed frame (which is
// what moves the clip on the nodes as the drag happens). Zero is "no gesture".
struct EditIntent final {
    doc::NodeId node{};
    scene::Vec3 position{};
    core::u64 gesture{};

    bool operator==(const EditIntent&) const = default;
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
};

// A request to create one element. The Director allocates the node id; `gesture`
// makes the creation a single undo step. `source` is the file for a Model (empty
// otherwise).
struct AddIntent final {
    AddKind kind{};
    std::string source;
    core::u64 gesture{};

    bool operator==(const AddIntent&) const = default;
};

enum class ElementKind : core::u8 {
    Object,
    Media,
    Model,
    DirectionalLight,
    PointLight,
};

// A scene element the Inspector shows and edits, with its authored values (the
// base, before channels) read from the document by the Director. Objects and
// clips use the transform; lights use the light fields.
struct ElementInfo final {
    doc::NodeId node{};
    ElementKind kind{ElementKind::Object};
    scene::Vec3 position{};
    scene::Vec3 rotation{}; // yaw, pitch, roll (turns)
    scene::Vec3 scale{1.0F, 1.0F, 1.0F};
    scene::Vec3 color{1.0F, 1.0F, 1.0F};
    float intensity{1.0F};
    scene::Vec3 direction{0.0F, -1.0F, 0.0F};
    float range{10.0F};

    bool operator==(const ElementInfo&) const = default;
};

// An Inspector edit of a named document property (used for light fields, which
// are not transform channels). The Director sets the property directly.
struct GenericEdit final {
    doc::NodeId node{};
    std::string property;
    double value{};
    core::u64 gesture{};

    bool operator==(const GenericEdit&) const = default;
};

// One Inspector edit: set a single transform component of `node` to `value`.
// The Director authors it as a base-property PutNode, grouped by `gesture`.
struct PropertyEdit final {
    doc::NodeId node{};
    scene::TransformChannel component{};
    double value{};
    core::u64 gesture{};

    bool operator==(const PropertyEdit&) const = default;
};

// One keyframe request on a single transform component: set (or clear) `target`'s
// channel for `component`. When keying, `value` is written at `frame`; when
// `clear` is set, the whole channel is removed. The Director authors it as a
// scene.channel edit; `gesture` groups a multi-component key (e.g. all of
// Position) into one undo step.
struct KeyIntent final {
    doc::NodeId target{};
    scene::TransformChannel component{};
    core::u64 frame{};
    double value{};
    bool clear{};
    core::u64 gesture{};

    bool operator==(const KeyIntent&) const = default;
};

// A keyframe retimed by dragging its diamond: the key at `from_frame` on
// (target, component) moves to `to_frame`. Authored once on release.
struct KeyMove final {
    doc::NodeId target{};
    scene::TransformChannel component{};
    core::i64 from_frame{};
    core::i64 to_frame{};
    core::u64 gesture{};

    bool operator==(const KeyMove&) const = default;
};

// A transport action the operator asked for from the timeline. The Director
// owns the transport, so the editor only names the request and the Director
// applies it (pausing to scrub, stepping, etc.), exactly like the keyboard
// controls a windowed node has.
enum class TransportRequest : core::u8 {
    None,
    TogglePlay,
    StepBack,
    StepForward,
    JumpBack,
    JumpForward,
    GoToStart,
};

// Where the show is, so the editor can draw a timeline and a playhead. Passed
// in each frame because the transport owns it; the editor only reflects it and
// asks to move it.
struct EditorTimeline final {
    core::u64 show_frame{};
    core::u64 first_frame{};
    core::u64 last_frame{};
    bool playing{};
    bool loop{};
};

// A change to the show's range and loop from the timeline. The Director applies
// it to the transport (clamping the position into the new range) and replicates.
struct RangeRequest final {
    core::u64 first{};
    core::u64 last{};
    bool loop{};

    bool operator==(const RangeRequest&) const = default;
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
    // Keyframe requests the operator made this frame (key or clear).
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

// True when this build can open the editor: it has the scene shader bytecode and
// a GPU to run it on. Checked once so the Director can refuse cleanly.
[[nodiscard]] bool editor_available() noexcept;

class EditorShell final {
public:
    [[nodiscard]] static core::Result<std::unique_ptr<EditorShell>, EditorError>
    create(const EditorConfig& config) noexcept;

    EditorShell(const EditorShell&) = delete;
    EditorShell& operator=(const EditorShell&) = delete;
    EditorShell(EditorShell&&) = delete;
    EditorShell& operator=(EditorShell&&) = delete;
    ~EditorShell();

    // Renders the wall view of `scene`, runs one UI frame, and returns the
    // operator's edits and whether the window is still open. `scene` is the
    // committed scene evaluated at the current show frame - the same value every
    // node draws - so the editor edits exactly what the wall shows. `timeline`
    // is where the show is, so the editor can draw the playhead and scrub it.
    // `keyframes` is where each animated node holds keys, for drawing them on the
    // timeline; `elements` carries the selection's authored transform for the
    // Inspector.
    [[nodiscard]] EditorFrame frame(
        const scene::Scene& scene,
        const EditorTimeline& timeline,
        const std::vector<scene::ChannelKeyframes>& keyframes,
        const std::vector<ElementInfo>& elements) noexcept;

private:
    struct Impl;

    explicit EditorShell(std::unique_ptr<Impl> impl) noexcept;

    // The After Effects-style timeline panel. A static helper on Impl so it can
    // reach the view state while keeping the (already large) frame() readable.
    static void draw_timeline(
        Impl& impl,
        EditorFrame& result,
        const scene::Scene& scene,
        const EditorTimeline& timeline,
        const std::vector<scene::ChannelKeyframes>& keyframes,
        const std::vector<ElementInfo>& elements) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace mediaserver::editor
