#pragma once

#include <array>
#include <cstddef>
#include <string>

#include <mediaserver/core/types.hpp>
#include <mediaserver/render/hybrid_scene_renderer.hpp>
#include <mediaserver/render/mapped_scene.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/show/presenter.hpp>

#include <functional>
#include <span>

namespace mediaserver::ui {

struct SdlPresenterConfig final {
    std::string title{"media-server"};
    core::u32 width{960U};
    core::u32 height{540U};
    // Window placement, so a Director preview and its node outputs can be
    // arranged without a window manager.
    core::i32 position_x{-1};
    core::i32 position_y{-1};
    // Position is explicit independently of sign because a monitor may have
    // negative desktop coordinates. False preserves window-manager placement.
    bool position_explicit{};
    bool fullscreen{};
    bool borderless{};
    bool resizable{true};
    // An explicit physical-output binding may require an exact exclusive
    // fullscreen mode. Zero leaves SDL in the current/desktop mode for legacy
    // windows. When `require_exact_display_mode` is true, creation fails if
    // the target display does not advertise this exact size and rational
    // refresh rate; silently choosing a nearby mode would make the Hardware
    // Map lie about what is actually being driven.
    core::u32 refresh_numerator{};
    core::u32 refresh_denominator{};
    bool require_exact_display_mode{};
    // The desktop rectangle of the display this output is bound to, resolved
    // from the authored physical target by the caller. Width zero means the
    // caller named no display and the legacy position-based behavior applies.
    //
    // With a rectangle, the presenter selects the display whose bounds equal
    // it rather than asking which display contains the window's corner. The
    // two differ exactly when it matters: mirrored or overlapping desktops
    // share an origin, so a point lands on whichever display was enumerated
    // first and an exclusive mode would be applied to the wrong screen. A
    // rectangle that matches no display is refused, never approximated.
    core::i32 display_bounds_x{};
    core::i32 display_bounds_y{};
    core::u32 display_bounds_width{};
    core::u32 display_bounds_height{};
    // Physical output precision. Deeper formats require the native backend.
    core::u32 output_bit_depth{8U};
    // Only an operator surface reads the keyboard. A node output window is
    // deliberately input-inert: what it shows is replicated state.
    bool accepts_commands{};
    // Render through the rhi seam onto a GPU target instead of uploading a CPU
    // surface. Refused rather than downgraded when the build has no shader
    // bytecode or the machine has no GPU, because an operator who asked for
    // the GPU path and silently got the CPU one would have no way to tell.
    bool gpu{};
    // Also build the two 3D scene renderers and transition mixer, so this
    // presenter can accept both the single-scene and two-scene overloads of
    // present(). Implies `gpu`: there is no CPU path for a depth-tested,
    // shaded scene and there should not be one.
    //
    // Separate from `gpu` because the pipeline, the depth buffer, and the
    // mesh arenas cost real memory, and a node driving a 2D show should not
    // pay for them. Two renderers are intentional: a dissolve combines two
    // finished pictures rather than interleaving two scenes' elements.
    bool scene{};
    // Also build feed-map projection warp support, so this presenter can
    // accept the output overloads of present(). Every scene presenter already
    // owns the final display transform; `output` adds physical mapping.
    bool output{};
    // Output-node mode: use a deeper decode/read-ahead cache and never put a
    // scene on the swapchain while one of its visible media frames is pending.
    // Operator previews leave this false to remain low-latency.
    bool buffered_media{};
    core::u32 media_read_ahead_frames{12U};
    core::u32 media_cache_mebibytes{2'048U};
    core::u32 media_trail_frames{};
};

// True when this build carries shader bytecode a device could accept. False
// means the GPU path cannot be constructed however capable the machine is.
[[nodiscard]] bool gpu_output_available() noexcept;

// The same question for the 3D path, which needs its own bytecode. A build can
// have one and not the other only if the shader build was partially edited, so
// this is mostly here to keep the reason for a refusal specific.
[[nodiscard]] bool scene_output_available() noexcept;

// The same question for the output stage, which carries its own bytecode too.
// True means this build can render a scene through a feed map onto the window.
[[nodiscard]] bool output_output_available() noexcept;

// SDL3 window output for the playback shell.
//
// Two presentation paths behind one window, chosen by configuration:
//
//  * CPU - the pattern is rasterised into a surface and uploaded to a
//    streaming texture each frame. A full copy per frame per output, and the
//    fallback the plan calls for.
//  * GPU - the pattern is described as instanced quads, the few kilobytes of
//    instance data are uploaded, and one draw fills an offscreen colour target
//    which is then blitted onto the swapchain. No pixel crosses the bus.
//
// They share the window, the event dispatch, and the command queue, because
// those are process-wide concerns and duplicating them is how a second window
// ends up stealing the first one's close event.
//
// SDL is initialised on first use and shut down when the last presenter is
// destroyed, so a headless role never initialises a video subsystem.
// What one GPU frame records into a command list, and which texture the
// swapchain should then blit. Returning null means the frame failed.
using RecordFrame =
    std::function<const rhi::Texture *(rhi::CommandList &commands)>;

class SdlPresenter final : public show::Presenter {
  public:
    [[nodiscard]] static show::PresenterResult
    create(const SdlPresenterConfig &config) noexcept;

    ~SdlPresenter() override;

    [[nodiscard]] show::PresenterStatus
    present(const show::TestPatternInput &frame) noexcept override;
    [[nodiscard]] show::PresenterStatus
    present(const mediaserver::scene::Scene &frame,
            mediaserver::scene::CanvasRect slice) noexcept override;
    [[nodiscard]] show::PresenterStatus
    present(const mediaserver::scene::Scene &frame,
            const mediaserver::compositor::CompositorGraph &graph,
            mediaserver::scene::CanvasRect slice) noexcept override;
    [[nodiscard]] show::PresenterStatus
    present(const mediaserver::scene::Scene &from,
            const mediaserver::scene::Scene &to, float mix,
            mediaserver::scene::CanvasRect slice) noexcept override;
    [[nodiscard]] show::PresenterStatus
    present(const mediaserver::scene::Scene &from,
            const mediaserver::scene::Scene &to, float mix,
            const mediaserver::compositor::CompositorGraph &from_graph,
            const mediaserver::compositor::CompositorGraph &to_graph,
            mediaserver::scene::CanvasRect slice) noexcept override;
    [[nodiscard]] show::PresenterStatus
    present(const mediaserver::scene::Scene &frame,
            const mediaserver::output::OutputMap &map) noexcept override;
    [[nodiscard]] show::PresenterStatus
    present(const mediaserver::scene::Scene &frame,
            const mediaserver::compositor::CompositorGraph &graph,
            const mediaserver::output::OutputMap &map) noexcept override;
    [[nodiscard]] show::PresenterStatus present(
        const mediaserver::scene::Scene &frame,
        std::span<const mediaserver::output::OutputMap> maps) noexcept override;
    [[nodiscard]] show::PresenterStatus present(
        const mediaserver::scene::Scene &frame,
        const mediaserver::compositor::CompositorGraph &graph,
        std::span<const mediaserver::output::OutputMap> maps) noexcept override;
    [[nodiscard]] show::PresenterStatus
    present(const mediaserver::scene::Scene &from,
            const mediaserver::scene::Scene &to, float mix,
            const mediaserver::output::OutputMap &map) noexcept override;
    [[nodiscard]] show::PresenterStatus
    present(const mediaserver::scene::Scene &from,
            const mediaserver::scene::Scene &to, float mix,
            const mediaserver::compositor::CompositorGraph &from_graph,
            const mediaserver::compositor::CompositorGraph &to_graph,
            const mediaserver::output::OutputMap &map) noexcept override;
    [[nodiscard]] show::PresenterStatus present(
        const mediaserver::scene::Scene &from,
        const mediaserver::scene::Scene &to, float mix,
        std::span<const mediaserver::output::OutputMap> maps) noexcept override;
    [[nodiscard]] show::PresenterStatus present(
        const mediaserver::scene::Scene &from,
        const mediaserver::scene::Scene &to, float mix,
        const mediaserver::compositor::CompositorGraph &from_graph,
        const mediaserver::compositor::CompositorGraph &to_graph,
        std::span<const mediaserver::output::OutputMap> maps) noexcept override;
    [[nodiscard]] bool renders_scenes() const noexcept override {
        return scene_;
    }
    [[nodiscard]] bool renders_output() const noexcept override {
        return output_;
    }
    [[nodiscard]] const show::FrameBufferView &
    surface() const noexcept override {
        return surface_.view();
    }
    [[nodiscard]] show::PresenterCommand take_command() noexcept override;
    [[nodiscard]] bool windowed() const noexcept override { return true; }
    [[nodiscard]] show::PresenterReadback readback() noexcept override;
    [[nodiscard]] show::PresenterTiming timing() const noexcept override {
        return timing_;
    }
    [[nodiscard]] show::PresenterMemory memory() const noexcept override;
    [[nodiscard]] show::PresenterPlateStats plates() const noexcept override;
    [[nodiscard]] show::PresenterStatus hold() noexcept override;
    void begin_document_stream() noexcept override;
    // Joins or leaves the vendor present barrier through the rhi seam. The
    // GPU path is the only one that can: the CPU path has no swapchain, and a
    // presenter with no barrier backend installed reports Unsupported rather
    // than pretending its window is swap locked.
    [[nodiscard]] show::PresenterBarrierState
    apply_sync(const show::PresenterSyncRequest &request) noexcept override;
    [[nodiscard]] show::PresenterSyncReport
    sync_report() const noexcept override;
    [[nodiscard]] show::PresenterVideoReference
    video_reference() const noexcept override;

    // Platform window identifier. Exposed so a diagnostic or test can address
    // this exact window on the shared event queue.
    [[nodiscard]] core::u32 window_id() const noexcept;
    // Already-known runtime facts. Neither accessor probes the adapter or
    // waits for work; the readiness value is the most recent Program record.
    [[nodiscard]] rhi::BackendKind backend() const noexcept;
    [[nodiscard]] render::RendererReadiness render_readiness() const noexcept;
    // Machine-local operator action for a repaired/replaced imported
    // projection mesh. This only records a new background-load generation;
    // it performs no file I/O and does not imply any cross-node fanout.
    [[nodiscard]] bool refresh_output_projection_mesh() noexcept;
    // Machine-local operator action for repaired/replaced model content. The
    // process-shared generation is advanced once through scene_from; this does
    // not fan the request out to synchronized render nodes.
    [[nodiscard]] bool refresh_model_source(
        const std::string& source) noexcept;
    // Lock-free observation for contextual UI. Failures remain sticky until
    // the explicit action above or an authored mapping-key change.
    [[nodiscard]] render::OutputProjectionCacheStatus
    output_projection_mesh_status() const noexcept;
    [[nodiscard]] show::PresenterRendererReadiness
    renderer_readiness() const noexcept override;
    [[nodiscard]] core::PresentFailure
    present_failure() const noexcept override {
        return failure_;
    }

  private:
    struct Platform;

    SdlPresenter(show::PresenterSurface surface, Platform *platform,
                 bool accepts_commands, bool gpu, bool scene, bool output,
                 bool buffered_media) noexcept;

    void pump() noexcept;
    // Nonblocking fence retirement, also used when retaining an existing image.
    [[nodiscard]] bool retire_pending() noexcept;
    [[nodiscard]] bool enqueue(show::PresenterCommand command) noexcept;
    // Builds the device, swapchain, target, atlas, and pipeline. A member
    // because it touches the private platform block; static because it runs
    // before there is a presenter to be a member of.
    [[nodiscard]] static bool
    build_gpu_resources(Platform &platform,
                        const SdlPresenterConfig &config) noexcept;
    [[nodiscard]] show::PresenterStatus
    present_cpu(const show::TestPatternInput &frame) noexcept;
    [[nodiscard]] show::PresenterStatus
    present_gpu(const show::TestPatternInput &frame) noexcept;
    // Acquire, record, present, submit. Shared by both GPU paths because the
    // only thing that differs between them is what gets recorded, and letting
    // them drift would mean two definitions of a presented frame.
    [[nodiscard]] show::PresenterStatus
    present_recorded(const RecordFrame &record,
                     const bool *media_complete = nullptr,
                     const scene::Scene *asset_scene = nullptr,
                     const scene::Scene *asset_partner = nullptr) noexcept;

    // Counts a failed frame and remembers where it failed.
    [[nodiscard]] show::PresenterStatus
    fail(core::PresentStep step,
         core::GpuRefusal refusal = core::GpuRefusal::None) noexcept;

    static constexpr std::size_t command_capacity = 16U;

    show::PresenterSurface surface_;
    Platform *platform_{};
    bool accepts_commands_{};
    bool gpu_{};
    bool scene_{};
    bool output_{};
    bool buffered_media_{};
    bool closed_{};
    show::PresenterTiming timing_{};
    core::PresentFailure failure_{};
    // The show frame and output map being drawn, so a failure can name them.
    core::u64 drawing_frame_{};
    core::u64 drawing_map_{};
    show::PresenterSyncRequest sync_request_{};
    show::PresenterBarrierState sync_state_{
        show::PresenterBarrierState::Unknown};
    core::u64 sync_leave_events_{};
    std::array<show::PresenterCommand, command_capacity> commands_{};
    std::size_t command_head_{};
    std::size_t command_size_{};
};

} // namespace mediaserver::ui
