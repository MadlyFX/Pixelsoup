#pragma once
#include <mediaserver/core/frame_timing.hpp>
#include <mediaserver/core/render_conformance.hpp>
#include <mediaserver/core/asset_publication.hpp>
#include <mediaserver/core/present_failure.hpp>

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
    // This candidate was not submitted for display: GPU work, media or asset
    // publication is still pending. Buffered output keeps the previous image
    // and retries the same timeline position.
    Deferred,
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
    // The binding named a display this machine no longer presents as its own
    // desktop area. Distinct from the mode error: the mode was never reached
    // because there was no display to ask. Refused rather than falling back
    // to whichever display sits under the window's corner, which is how an
    // output ends up driving the wrong screen.
    DisplayTargetUnavailable,
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
    GoToEnd,
    // The cue GO: run from a hold, release a loop, or play to the next cue.
    Go,
    // An editor's J, K and L: play backwards, stop, play forwards, each from
    // wherever the show is. Playing either way while already playing the
    // other turns it round without moving it.
    PlayReverse,
    Pause,
    PlayForward,
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
// What a presenter did with inner-frustum plates, in the interface's own
// terms: this layer must not depend on the renderer that produces them.
struct PresenterPlateStats final {
    core::u64 drawn{};
    // Plates this output skipped because its slice missed the rectangle. On a
    // wide wall this is most outputs and it is the whole point of ICVFX, so it
    // is reported rather than treated as an absence.
    core::u64 skipped{};
    // Plans refused: a target past its bound, or a compositor that would not
    // accept one.
    core::u64 refusals{};

    bool operator==(const PresenterPlateStats&) const = default;
};

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
    // Complete output frames intentionally retained because at least one
    // visible media plate for the next frame was still pending.
    core::u64 media_deferred_frames{};
    // True when flips wait for the display refresh. Outputs do, so a frame
    // never tears, but the show thread never blocks on it: a refresh that is
    // not ready yet counts as a skipped present. Reported because a display
    // slower than the show rate looks like a slow node from the outside.
    bool vsync_limited{};

    bool operator==(const PresenterTiming&) const = default;
};

// GPU memory as the presenter's device sees it, sampled when asked. Kept out
// of PresenterTiming because it is a kernel query, not a per-frame counter,
// and because it moves with other processes on the same GPU. `known` is false
// on paths with no device or a backend that cannot tell.
struct PresenterMemory final {
    bool known{};
    // Adapter-local memory this process uses, and what the OS lets it keep.
    core::u64 local_usage{};
    core::u64 local_budget{};
    // System memory held on the GPU's behalf, demotions out of local memory
    // included. Growth here with local usage pinned at budget is a leak.
    core::u64 system_usage{};
    // Textures and geometry buffers the device created and still holds, by
    // the renderer's own count. Diverging from local_usage points outside it:
    // decoder surfaces, driver allocations, upload heaps.
    core::u64 committed{};

    bool operator==(const PresenterMemory&) const = default;
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

// The hardware source that supplies the frame boundaries for a genlocked
// output node. This stays in the show seam rather than exposing an RHI/vendor
// type to the playback loop.
enum class PresenterVideoReferenceState : core::u8 {
    Unknown,
    Unsupported,
    Waiting,
    Locked,
    Lost,
    Failed,
};

struct PresenterVideoReference final {
    PresenterVideoReferenceState state{
        PresenterVideoReferenceState::Unknown};
    core::u32 frame_counter{};

    bool operator==(const PresenterVideoReference&) const = default;
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
    core::u64 flips_in_sync{};
    core::u64 leave_events{};
    // Frames the platform showed again because no new swapchain image was
    // acquired. Reported here rather than derived, because only the
    // presentation path can tell a repeat from a deliberate hold.
    core::u64 repeated{};

    bool operator==(const PresenterSyncReport&) const = default;
};

// Renderer observation exported by a Program-capable presenter without making
// the show layer depend on engine/render. profile_digest binds this answer to
// every authored RendererSettings field; zero means no scene frame for a
// profile has reached the renderer yet.
enum class PresenterRendererReadinessState : core::u8 {
    Unknown,
    Ready,
    BasicFallback,
    MissingCapabilities,
    AdvancedImplementationUnavailable,
    InvalidPolicy,
    VideoMemoryShort,
};

struct PresenterRendererReadiness final {
    core::u64 profile_digest{};
    PresenterRendererReadinessState state{
        PresenterRendererReadinessState::Unknown};
    scene::RendererBackend requested{scene::RendererBackend::Basic};
    scene::RendererBackend selected{scene::RendererBackend::Basic};
    core::u64 missing_capabilities{};
    bool satisfied{};
    bool fallback{};

    core::AssetSetReadiness assets{};

    core::u64 temporal_restarts{};
    core::u64 temporal_restart_frame{};

    core::RenderAdapterIdentity adapter{};
    core::FrameTiming timing{};

    // VideoMemoryShort only: adapter-local bytes still needed, and the
    // headroom the OS budget left.
    core::u64 video_memory_needed{};
    core::u64 video_memory_free{};

    bool operator==(const PresenterRendererReadiness&) const = default;
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

    // Each side of a dissolve is composited through its own graph, because a
    // compositor graph belongs to a scene. Handing both scenes one graph would
    // make the first frames of a promotion show the outgoing scene's layers
    // treated by the incoming scene's rules.
    [[nodiscard]] virtual PresenterStatus present(
        const scene::Scene& from,
        const scene::Scene& to,
        float mix,
        const compositor::CompositorGraph& from_graph,
        const compositor::CompositorGraph& to_graph,
        scene::CanvasRect slice) noexcept {
        static_cast<void>(from_graph);
        static_cast<void>(to_graph);
        return present(from, to, mix, slice);
    }

    // True when the scene overload above will actually draw something. Checked
    // once at start-up so a node can refuse a 3D show it cannot present,
    // rather than failing every frame of it.
    [[nodiscard]] virtual bool renders_scenes() const noexcept { return false; }

    // Checkpoint replacement starts a new document stream. Forget publication
    // ordinals from the previous stream while retaining its displayed image
    // until the replacement is approved and complete. No I/O or GPU wait.
    virtual void begin_document_stream() noexcept {}

    // Presents this node's slice of a 3D scene put through its feed map: the
    // scene is rendered for the map's source rectangle, then the output stage
    // places it in the map's viewport inside the fixed window raster. This is
    // the OUTPUT
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

    // Presents every viewport assigned to one application window as one
    // atomic frame. Multi-output desktops such as NVIDIA Mosaic expose one
    // swapchain even when several physical connectors occupy regions in it.
    [[nodiscard]] virtual PresenterStatus present(
        const scene::Scene& frame,
        std::span<const output::OutputMap> maps) noexcept {
        return maps.size() == 1U ? present(frame, maps.front())
                                : PresenterStatus::Failed;
    }

    [[nodiscard]] virtual PresenterStatus present(
        const scene::Scene& frame,
        const compositor::CompositorGraph& graph,
        std::span<const output::OutputMap> maps) noexcept {
        return maps.size() == 1U ? present(frame, graph, maps.front())
                                : PresenterStatus::Failed;
    }

    // The mapped-output form of the two-scene transition above. Both scenes
    // are rendered for `map.source`, mixed as completed colour targets, then
    // sent through the same viewport/output transform as a single scene.
    // Output calibration therefore remains downstream of the switcher.
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
        const compositor::CompositorGraph& from_graph,
        const compositor::CompositorGraph& to_graph,
        const output::OutputMap& map) noexcept {
        static_cast<void>(from_graph);
        static_cast<void>(to_graph);
        return present(from, to, mix, map);
    }

    [[nodiscard]] virtual PresenterStatus present(
        const scene::Scene& from,
        const scene::Scene& to,
        float mix,
        std::span<const output::OutputMap> maps) noexcept {
        return maps.size() == 1U ? present(from, to, mix, maps.front())
                                : PresenterStatus::Failed;
    }

    [[nodiscard]] virtual PresenterStatus present(
        const scene::Scene& from,
        const scene::Scene& to,
        float mix,
        const compositor::CompositorGraph& from_graph,
        const compositor::CompositorGraph& to_graph,
        std::span<const output::OutputMap> maps) noexcept {
        return maps.size() == 1U
            ? present(from, to, mix, from_graph, to_graph, maps.front())
            : PresenterStatus::Failed;
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

    // GPU memory now. Queries the kernel, so callers sample it at their status
    // interval rather than per frame. A CPU presenter reports unknown.
    [[nodiscard]] virtual PresenterMemory memory() const noexcept { return {}; }

    // What this path did with the inner frustum.
    //
    // Reported through the presenter because the plate is drawn inside it, and
    // the node has no other handle on the stage that drew it. A presenter with
    // no ICVFX path reports zeroes, which is distinguishable from a plate that
    // was planned and skipped only because the skip is counted too.
    [[nodiscard]] virtual PresenterPlateStats plates() const noexcept {
        return {};
    }

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

    // Reads an already available video-reference frame counter. The node
    // boundary driver consumes this only for a policy that requires external
    // reference; it never derives show progression from wall-clock elapsed
    // time in that mode.
    [[nodiscard]] virtual PresenterVideoReference
    video_reference() const noexcept {
        return {};
    }

    // Already-computed renderer truth. This must not probe the adapter, wait
    // for a frame slot, or perform any I/O.
    [[nodiscard]] virtual PresenterRendererReadiness
    renderer_readiness() const noexcept {
        return {};
    }

    // Frames this presenter tried and failed to draw, and where the latest
    // one failed. Kept up to date by present(); never probes anything.
    [[nodiscard]] virtual core::PresentFailure
    present_failure() const noexcept {
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
