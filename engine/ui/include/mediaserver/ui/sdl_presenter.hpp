#pragma once

#include <array>
#include <cstddef>
#include <string>

#include <mediaserver/core/types.hpp>
#include <mediaserver/rhi/device.hpp>
#include <mediaserver/show/presenter.hpp>

#include <functional>

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
    // Also build the output transform, so this presenter can accept the
    // output overloads of present(). Implies `scene`: the transform's source
    // is either the scene renderer or transition mixer's target, so there is
    // nothing to transform without it.
    bool output{};
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
    std::function<const rhi::Texture*(rhi::CommandList& commands)>;

class SdlPresenter final : public show::Presenter {
public:
    [[nodiscard]] static show::PresenterResult create(
        const SdlPresenterConfig& config) noexcept;

    ~SdlPresenter() override;

    [[nodiscard]] show::PresenterStatus present(
        const show::TestPatternInput& frame) noexcept override;
    [[nodiscard]] show::PresenterStatus present(
        const mediaserver::scene::Scene& frame,
        mediaserver::scene::CanvasRect slice) noexcept override;
    [[nodiscard]] show::PresenterStatus present(
        const mediaserver::scene::Scene& frame,
        const mediaserver::compositor::CompositorGraph& graph,
        mediaserver::scene::CanvasRect slice) noexcept override;
    [[nodiscard]] show::PresenterStatus present(
        const mediaserver::scene::Scene& from,
        const mediaserver::scene::Scene& to,
        float mix,
        mediaserver::scene::CanvasRect slice) noexcept override;
    [[nodiscard]] show::PresenterStatus present(
        const mediaserver::scene::Scene& from,
        const mediaserver::scene::Scene& to,
        float mix,
        const mediaserver::compositor::CompositorGraph& graph,
        mediaserver::scene::CanvasRect slice) noexcept override;
    [[nodiscard]] show::PresenterStatus present(
        const mediaserver::scene::Scene& frame,
        const mediaserver::output::OutputMap& map) noexcept override;
    [[nodiscard]] show::PresenterStatus present(
        const mediaserver::scene::Scene& frame,
        const mediaserver::compositor::CompositorGraph& graph,
        const mediaserver::output::OutputMap& map) noexcept override;
    [[nodiscard]] show::PresenterStatus present(
        const mediaserver::scene::Scene& from,
        const mediaserver::scene::Scene& to,
        float mix,
        const mediaserver::output::OutputMap& map) noexcept override;
    [[nodiscard]] show::PresenterStatus present(
        const mediaserver::scene::Scene& from,
        const mediaserver::scene::Scene& to,
        float mix,
        const mediaserver::compositor::CompositorGraph& graph,
        const mediaserver::output::OutputMap& map) noexcept override;
    [[nodiscard]] bool renders_scenes() const noexcept override { return scene_; }
    [[nodiscard]] bool renders_output() const noexcept override { return output_; }
    [[nodiscard]] const show::FrameBufferView& surface() const noexcept override {
        return surface_.view();
    }
    [[nodiscard]] show::PresenterCommand take_command() noexcept override;
    [[nodiscard]] bool windowed() const noexcept override { return true; }
    [[nodiscard]] show::PresenterReadback readback() noexcept override;
    [[nodiscard]] show::PresenterTiming timing() const noexcept override {
        return timing_;
    }

    // Platform window identifier. Exposed so a diagnostic or test can address
    // this exact window on the shared event queue.
    [[nodiscard]] core::u32 window_id() const noexcept;

private:
    struct Platform;

    SdlPresenter(
        show::PresenterSurface surface,
        Platform* platform,
        bool accepts_commands,
        bool gpu,
        bool scene,
        bool output) noexcept;

    void pump() noexcept;
    [[nodiscard]] bool enqueue(show::PresenterCommand command) noexcept;
    // Builds the device, swapchain, target, atlas, and pipeline. A member
    // because it touches the private platform block; static because it runs
    // before there is a presenter to be a member of.
    [[nodiscard]] static bool build_gpu_resources(
        Platform& platform,
        const SdlPresenterConfig& config) noexcept;
    [[nodiscard]] show::PresenterStatus present_cpu(
        const show::TestPatternInput& frame) noexcept;
    [[nodiscard]] show::PresenterStatus present_gpu(
        const show::TestPatternInput& frame) noexcept;
    // Acquire, record, present, submit. Shared by both GPU paths because the
    // only thing that differs between them is what gets recorded, and letting
    // them drift would mean two definitions of a presented frame.
    [[nodiscard]] show::PresenterStatus present_recorded(
        const RecordFrame& record) noexcept;

    static constexpr std::size_t command_capacity = 16U;

    show::PresenterSurface surface_;
    Platform* platform_{};
    bool accepts_commands_{};
    bool gpu_{};
    bool scene_{};
    bool output_{};
    bool closed_{};
    show::PresenterTiming timing_{};
    std::array<show::PresenterCommand, command_capacity> commands_{};
    std::size_t command_head_{};
    std::size_t command_size_{};
};

} // namespace mediaserver::ui
