#pragma once

#include <array>
#include <cstddef>
#include <memory>

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/output/feed_map.hpp>
#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/scene.hpp>
#include <mediaserver/show/test_pattern.hpp>

namespace mediaserver::compositor {
struct CompositorGraph;
}

namespace mediaserver::show {

enum class PresenterStatus : core::u8 {
    Presented,
    // Nothing was drawn this boundary and nothing is wrong: the window is
    // minimised or otherwise not showing. Playback continues.
    Skipped,
    // The viewer closed the output. Playback should stop.
    Closed,
    Failed,
};

enum class PresenterError : core::u8 {
    InvalidConfiguration,
    AllocationFailed,
    // The platform refused a window, renderer, or texture.
    DisplayUnavailable,
    // A physical fullscreen binding requested a native size/rational refresh
    // combination the selected display did not advertise or apply exactly.
    ExactDisplayModeUnavailable,
};

// Operator intent read from an output window. A node ignores these: what it
// presents is replicated state, so only the Director acts on them.
enum class PresenterCommand : core::u8 {
    None,
    Quit,
    TogglePlay,
    StepBack,
    StepForward,
    JumpBack,
    JumpForward,
    GoToStart,
};

// A caller-owned 32-bit surface allocated once. Both the headless and windowed
// presenters rasterise into one of these, so the presented pixels are always
// available for a digest without reading back a GPU texture.
class PresenterSurface final {
public:
    [[nodiscard]] static core::Result<PresenterSurface, PresenterError> create(
        core::u32 width,
        core::u32 height) noexcept;

    PresenterSurface(const PresenterSurface&) = delete;
    PresenterSurface& operator=(const PresenterSurface&) = delete;
    PresenterSurface(PresenterSurface&& other) noexcept;
    PresenterSurface& operator=(PresenterSurface&& other) noexcept;
    ~PresenterSurface() = default;

    [[nodiscard]] const FrameBufferView& view() const noexcept { return view_; }
    [[nodiscard]] core::u32 width() const noexcept { return view_.width; }
    [[nodiscard]] core::u32 height() const noexcept { return view_.height; }
    [[nodiscard]] core::u32 stride() const noexcept { return view_.stride; }
    [[nodiscard]] const core::u8* pixels() const noexcept {
        return view_.pixels.data();
    }

private:
    PresenterSurface(
        std::unique_ptr<core::u8[]> storage,
        std::size_t bytes,
        core::u32 width,
        core::u32 height,
        core::u32 stride) noexcept;

    std::unique_ptr<core::u8[]> storage_;
    FrameBufferView view_{};
};

// The result of reading the presented output back from the platform. This is a
// start-up self-check, not a per-frame operation: it proves the whole upload
// and present path - including pixel order - actually put the rasterised bytes
// on the display, which no amount of testing the rasteriser alone can show.
struct PresenterReadback final {
    core::u32 width{};
    core::u32 height{};
    core::u64 digest{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return width != 0U && height != 0U;
    }
    bool operator==(const PresenterReadback&) const = default;
};

// Per-frame cost of the presentation path.
//
// SDL_GPU exposes no timestamp query, so no presenter here can report GPU
// execution time. Two things can be measured honestly, and both are, because
// they answer different questions:
//
//  * `present_ns` - wall time spent inside present(). This is exact, and it is
//    the number that says whether presentation fits in the frame budget.
//  * `submit_to_observed_ns` - from submitting a frame's work to first
//    observing its fence signalled. An *upper bound* on GPU time and nothing
//    more: the fence is only examined when the next frame comes round, so a
//    frame that finished in a microsecond still reads as a frame period if
//    nothing looked sooner. Named for what it measures rather than for what
//    would be more useful.
struct PresenterTiming final {
    bool gpu{};
    core::u64 frames{};
    core::u64 last_present_ns{};
    core::u64 peak_present_ns{};
    core::u64 last_submit_to_observed_ns{};
    core::u64 peak_submit_to_observed_ns{};
    // Frames the display was not ready for. Not an error: a minimised or
    // occluded window still lets the show run.
    core::u64 skipped_presents{};
    // True when the swapchain is blocking on the display refresh. An output
    // node should never be: the frame boundary owns timing. Reported because
    // a vsync-limited path looks exactly like a slow one from the outside.
    bool vsync_limited{};

    bool operator==(const PresenterTiming&) const = default;
};

// Whether this presenter joined the vendor swap group / present barrier the
// project asked for. Deliberately mirrors the RHI enum without depending on
// it: a headless presenter and a headless test both need to name these states
// without pulling a graphics backend into the show layer.
enum class PresenterBarrierState : core::u8 {
    Unknown,
    // The presentation backend cannot join a barrier at all.
    Unsupported,
    NotJoined,
    Joining,
    Joined,
    // Joined earlier and dropped out: a loss-of-sync event.
    Left,
    Failed,
};

// The project synchronization policy, reduced to what a presenter needs.
// Disabled means the project makes no physical timing claim, in which case a
// presenter must leave any barrier it holds rather than silently keeping it.
struct PresenterSyncRequest final {
    bool enabled{};
    core::u32 group{1U};
    core::u32 barrier{1U};

    bool operator==(const PresenterSyncRequest&) const = default;
};

// What the presentation path can say about physical presentation.
// `presents_in_sync` is the number that matters: a nominal barrier member
// whose presents stop counting as in sync has lost the barrier.
struct PresenterSyncReport final {
    PresenterBarrierState barrier{PresenterBarrierState::Unknown};
    core::u32 group{};
    core::u32 barrier_id{};
    core::u64 presents{};
    core::u64 presents_in_sync{};
    core::u64 leave_events{};
    // Frames the platform showed again because no new swapchain image was
    // acquired. Reported here rather than derived, because only the
    // presentation path can tell a repeat from a deliberate hold.
    core::u64 repeated{};

    bool operator==(const PresenterSyncReport&) const = default;
};

// The presentation seam.
//
// present() runs immediately after the commit boundary, never inside it. That
// ordering is the point: the headless presenter allocates nothing, but a
// windowed one hands pixels to a platform driver that may allocate or block,
// and none of that is allowed to delay a commit the cluster is waiting on.
//
// The interface deliberately carries no document reference. Everything a
// window needs has already been resolved from the committed root, so a
// presenter can never walk the trie or hold the document past a frame.
//
// Implementations are created behind a unique_ptr, which keeps a windowed
// presenter free to own raw platform handles without being movable.
class Presenter {
public:
    Presenter() = default;
    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    Presenter(Presenter&&) = delete;
    Presenter& operator=(Presenter&&) = delete;
    virtual ~Presenter() = default;

    [[nodiscard]] virtual PresenterStatus present(
        const TestPatternInput& frame) noexcept = 0;

    // Presents this node's slice of a 3D scene.
    //
    // A second entry point rather than a second presenter, because a node
    // presents through exactly one window and the choice of what to draw is
    // per frame, not per process. The two paths share the window, the
    // swapchain, and the timing, which is the reason to keep them together.
    //
    // `slice` is this node's rectangle of `frame.canvas`. The scene arrives
    // already evaluated at a show frame - no document, no clock, and nothing
    // for a presenter to interpret differently from its peers.
    //
    // The default refuses. A presenter with no 3D path saying so is better
    // than one that quietly shows a test pattern instead: an operator who
    // asked for a scene and got a colour bar has learned nothing about why.
    [[nodiscard]] virtual PresenterStatus present(
        const scene::Scene& frame,
        scene::CanvasRect slice) noexcept {
        static_cast<void>(frame);
        static_cast<void>(slice);
        return PresenterStatus::Failed;
    }

    // Graph-aware scene presentation. The default preserves compatibility by
    // presenting the evaluated scene unchanged; GPU presenters override this
    // to execute spatial compositor passes such as per-layer blur.
    [[nodiscard]] virtual PresenterStatus present(
        const scene::Scene& frame,
        const compositor::CompositorGraph& graph,
        scene::CanvasRect slice) noexcept {
        static_cast<void>(graph);
        return present(frame, slice);
    }

    // Presents a completed Program transition between two independently
    // evaluated scenes. Both scenes describe the same canvas and `slice` is
    // this node's rectangle of that canvas. `mix` is the deterministic
    // control-frame value: zero is entirely `from`, one is entirely `to`.
    // Implementations clamp values outside that range.
    //
    // Keeping this as a presentation operation is important. Blending scene
    // elements before rendering gives different answers for depth, alpha,
    // effects, and dissimilar scene graphs; a switcher dissolves two finished
    // pictures instead. The default refuses so older presenters retain their
    // single-scene behaviour without pretending to support transitions.
    [[nodiscard]] virtual PresenterStatus present(
        const scene::Scene& from,
        const scene::Scene& to,
        float mix,
        scene::CanvasRect slice) noexcept {
        static_cast<void>(from);
        static_cast<void>(to);
        static_cast<void>(mix);
        static_cast<void>(slice);
        return PresenterStatus::Failed;
    }

    [[nodiscard]] virtual PresenterStatus present(
        const scene::Scene& from,
        const scene::Scene& to,
        float mix,
        const compositor::CompositorGraph& graph,
        scene::CanvasRect slice) noexcept {
        static_cast<void>(graph);
        return present(from, to, mix, slice);
    }

    // True when the scene overload above will actually draw something. Checked
    // once at start-up so a node can refuse a 3D show it cannot present,
    // rather than failing every frame of it.
    [[nodiscard]] virtual bool renders_scenes() const noexcept { return false; }

    // Presents this node's slice of a 3D scene put through its feed map: the
    // scene is rendered for the map's source rectangle, then the output stage
    // crops and soft-edge blends it on the way to the window. This is the OUTPUT
    // stage the plan (§1.2) sits downstream of the render, and it is a separate
    // entry point for the same reason the scene one is - the choice of what to
    // draw is per frame, not per process, and all three paths share the window,
    // the swapchain, and the timing.
    //
    // `map` is this node's binding from the replicated feed map. The scene is
    // already evaluated at a show frame, so two nodes given the same scene and
    // the same map produce contributions that reconstruct one canvas.
    //
    // The default refuses, exactly as the scene overload does: a node asked for
    // an output show it cannot present should say so, not fall back to something
    // that looks nothing like it.
    [[nodiscard]] virtual PresenterStatus present(
        const scene::Scene& frame,
        const output::OutputMap& map) noexcept {
        static_cast<void>(frame);
        static_cast<void>(map);
        return PresenterStatus::Failed;
    }

    [[nodiscard]] virtual PresenterStatus present(
        const scene::Scene& frame,
        const compositor::CompositorGraph& graph,
        const output::OutputMap& map) noexcept {
        static_cast<void>(graph);
        return present(frame, map);
    }

    // The mapped-output form of the two-scene transition above. Both scenes
    // are rendered for `map.source`, mixed as completed colour targets, then
    // sent through the same output transform as a single scene. Output
    // calibration therefore remains downstream of the switcher.
    [[nodiscard]] virtual PresenterStatus present(
        const scene::Scene& from,
        const scene::Scene& to,
        float mix,
        const output::OutputMap& map) noexcept {
        static_cast<void>(from);
        static_cast<void>(to);
        static_cast<void>(mix);
        static_cast<void>(map);
        return PresenterStatus::Failed;
    }

    [[nodiscard]] virtual PresenterStatus present(
        const scene::Scene& from,
        const scene::Scene& to,
        float mix,
        const compositor::CompositorGraph& graph,
        const output::OutputMap& map) noexcept {
        static_cast<void>(graph);
        return present(from, to, mix, map);
    }

    // True when the output overload above will actually draw something.
    [[nodiscard]] virtual bool renders_output() const noexcept { return false; }

    // The pixels most recently presented. Stable until the next present().
    [[nodiscard]] virtual const FrameBufferView& surface() const noexcept = 0;

    // Pops one queued operator command. A presenter with no input surface
    // never produces any.
    [[nodiscard]] virtual PresenterCommand take_command() noexcept {
        return PresenterCommand::None;
    }

    // True when this presenter drives a real window. Used only for reporting.
    [[nodiscard]] virtual bool windowed() const noexcept { return false; }

    // Reads back what the platform actually presented. A presenter with no
    // display pipeline reports nothing, which is not a failure.
    [[nodiscard]] virtual PresenterReadback readback() noexcept { return {}; }

    // What this path cost. A CPU presenter reports nothing but its frame
    // count, which is the honest answer: there is no submission to time.
    [[nodiscard]] virtual PresenterTiming timing() const noexcept { return {}; }

    // Holds the previously presented frame for this boundary.
    //
    // The group late-frame policy calls this instead of present() when one
    // output of a synchronization group has no complete frame: repeating one
    // frame everywhere is far less visible on an LED wall than advancing one
    // segment while its neighbour stands still. Implementations must still
    // service platform events, so a held window can still be closed.
    [[nodiscard]] virtual PresenterStatus hold() noexcept {
        return PresenterStatus::Skipped;
    }

    // Applies the project synchronization policy to this presentation path.
    // The default refuses, which is the correct answer for a presenter with
    // no swapchain: a headless or CPU path cannot join a present barrier and
    // must never report that it did.
    [[nodiscard]] virtual PresenterBarrierState apply_sync(
        const PresenterSyncRequest& request) noexcept {
        static_cast<void>(request);
        return PresenterBarrierState::Unsupported;
    }

    [[nodiscard]] virtual PresenterSyncReport sync_report() const noexcept {
        return {};
    }
};

using PresenterResult =
    core::Result<std::unique_ptr<Presenter>, PresenterError>;

// Rasterises to memory and nothing else. This is the presenter a headless node
// uses, and the one that makes an automated run checkable: the surface digest
// is exactly what a window would have shown.
class HeadlessPresenter final : public Presenter {
public:
    [[nodiscard]] static PresenterResult create(
        core::u32 width,
        core::u32 height) noexcept;

    [[nodiscard]] PresenterStatus present(
        const TestPatternInput& frame) noexcept override;
    [[nodiscard]] const FrameBufferView& surface() const noexcept override {
        return surface_.view();
    }

private:
    explicit HeadlessPresenter(PresenterSurface surface) noexcept
        : surface_(std::move(surface)) {}

    PresenterSurface surface_;
};

} // namespace mediaserver::show
