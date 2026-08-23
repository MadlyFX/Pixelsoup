#pragma once

#include <chrono>
#include <cstddef>
#include <span>
#include <string_view>

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>

// The render hardware interface.
//
// This is the seam the plan calls for in §0.3: SDL_GPU is the first backend,
// and the surface is deliberately narrow enough that a Vulkan-direct backend
// can replace it when bindless, mesh shaders, or the ray-traced ports demand
// it. Nothing above this header names SDL, and nothing in it exposes a
// backend type.
//
// The shape is dictated by what an output node actually does: render content
// into one offscreen colour target per output, then put that target on a
// display. It is not a general-purpose graphics API and should not grow into
// one speculatively.
namespace mediaserver::rhi {

enum class RhiError : core::u8 {
    // No usable backend on this machine. This is a first-class outcome, not a
    // fault: a build machine without a GPU still has to run the test suite.
    Unavailable,
    InvalidConfiguration,
    // The device offers no shader format this build carries bytecode for.
    ShaderFormatUnsupported,
    AllocationFailed,
    // The backend refused an operation it had accepted the inputs for.
    BackendFailure,
};

// Bytecode formats a device can accept. A build ships whichever it can
// compile; a device accepts whichever its backend implements. The intersection
// is what a pipeline may be created from, and an empty intersection is
// ShaderFormatUnsupported rather than a crash at draw time.
enum class ShaderFormat : core::u8 {
    Dxil,
    Dxbc,
    SpirV,
    Msl,
};

enum class TextureFormat : core::u8 {
    Rgba8Unorm,
    Bgra8Unorm,
};

enum class Filter : core::u8 {
    Nearest,
    Linear,
};

enum class ShaderStage : core::u8 {
    Vertex,
    Fragment,
};

// How a swapchain releases finished frames.
//
// Presentation must never pace the show. The frame boundary owns timing, and
// blocking on the display refresh beats against it; real output timing comes
// from genlock, not from the desktop compositor. So a swapchain asks for an
// unthrottled mode and falls back only when the platform offers nothing else.
enum class PresentMode : core::u8 {
    // Never waits for the display. Tears, and that is the correct trade for an
    // output node whose timing authority is elsewhere.
    Immediate,
    // Unthrottled but tear-free where the platform implements it.
    Mailbox,
    // Blocks until the display is ready. Correct only when nothing upstream
    // depends on frame timing.
    Vsync,
};

struct Color final {
    float r{};
    float g{};
    float b{};
    float a{1.0F};

    bool operator==(const Color&) const = default;
};

struct TextureDesc final {
    core::u32 width{};
    core::u32 height{};
    TextureFormat format{TextureFormat::Rgba8Unorm};
    // A texture may be rendered into, sampled, or both. Requesting neither is
    // an invalid configuration rather than a texture nothing can use.
    bool color_target{};
    bool sampled{};

    bool operator==(const TextureDesc&) const = default;
};

// One shader stage's compiled bytecode plus the resource counts the backend
// must be told before it will bind anything. SDL_GPU and Vulkan both require
// these up front; getting them wrong is a validation error, not a silent bind
// of nothing, which is why they are part of the description rather than
// inferred.
struct ShaderDesc final {
    ShaderStage stage{ShaderStage::Vertex};
    ShaderFormat format{ShaderFormat::Dxil};
    std::span<const core::u8> bytecode;
    std::string_view entry_point{"main"};
    core::u32 sampler_count{};
    core::u32 storage_buffer_count{};
    core::u32 uniform_buffer_count{};
};

class Device;

// Every resource is a move-only handle that releases itself back to the
// device that made it. Copying one would double-release; a moved-from handle
// is inert. None of them outlives its device, which is enforced by
// construction order in the presenter rather than by a back-reference count.
class Texture final {
public:
    Texture() noexcept = default;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;
    ~Texture();

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }
    [[nodiscard]] core::u32 width() const noexcept { return desc_.width; }
    [[nodiscard]] core::u32 height() const noexcept { return desc_.height; }
    [[nodiscard]] const TextureDesc& desc() const noexcept { return desc_; }

private:
    friend class Device;
    friend class CommandList;
    friend class Swapchain;

    Texture(Device& device, void* handle, const TextureDesc& desc) noexcept
        : device_(&device), handle_(handle), desc_(desc) {}

    void release() noexcept;

    Device* device_{};
    void* handle_{};
    TextureDesc desc_{};
};

// Read-only structured data a vertex shader indexes by instance. This is the
// whole per-frame upload the output path needs: a few hundred small records
// instead of a full surface.
class StorageBuffer final {
public:
    StorageBuffer() noexcept = default;
    StorageBuffer(const StorageBuffer&) = delete;
    StorageBuffer& operator=(const StorageBuffer&) = delete;
    StorageBuffer(StorageBuffer&& other) noexcept;
    StorageBuffer& operator=(StorageBuffer&& other) noexcept;
    ~StorageBuffer();

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }
    [[nodiscard]] core::u32 capacity_bytes() const noexcept { return bytes_; }

private:
    friend class Device;
    friend class CommandList;

    StorageBuffer(Device& device, void* handle, void* staging, core::u32 bytes) noexcept
        : device_(&device), handle_(handle), staging_(staging), bytes_(bytes) {}

    void release() noexcept;

    Device* device_{};
    void* handle_{};
    // The upload staging buffer is owned with the buffer it feeds, so a caller
    // cannot pair the wrong one or forget to allocate it on the frame path.
    void* staging_{};
    core::u32 bytes_{};
};

class Sampler final {
public:
    Sampler() noexcept = default;
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;
    Sampler(Sampler&& other) noexcept;
    Sampler& operator=(Sampler&& other) noexcept;
    ~Sampler();

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

private:
    friend class Device;
    friend class CommandList;

    Sampler(Device& device, void* handle) noexcept
        : device_(&device), handle_(handle) {}

    void release() noexcept;

    Device* device_{};
    void* handle_{};
};

// A complete graphics pipeline. V2 needs exactly one shape of draw - an
// instanced quad reading its rectangle, colour, and atlas region from a
// storage buffer - so there is no vertex input state here at all. Adding one
// later is additive; inventing it now would be a general-purpose API nobody
// asked for.
class GraphicsPipeline final {
public:
    GraphicsPipeline() noexcept = default;
    GraphicsPipeline(const GraphicsPipeline&) = delete;
    GraphicsPipeline& operator=(const GraphicsPipeline&) = delete;
    GraphicsPipeline(GraphicsPipeline&& other) noexcept;
    GraphicsPipeline& operator=(GraphicsPipeline&& other) noexcept;
    ~GraphicsPipeline();

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

private:
    friend class Device;
    friend class CommandList;

    GraphicsPipeline(Device& device, void* handle) noexcept
        : device_(&device), handle_(handle) {}

    void release() noexcept;

    Device* device_{};
    void* handle_{};
};

// A CPU-visible destination for a texture read back off the GPU. This is a
// diagnostic and self-check path, not a frame path.
class Readback final {
public:
    Readback() noexcept = default;
    Readback(const Readback&) = delete;
    Readback& operator=(const Readback&) = delete;
    Readback(Readback&& other) noexcept;
    Readback& operator=(Readback&& other) noexcept;
    ~Readback();

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }
    [[nodiscard]] core::u32 capacity_bytes() const noexcept { return bytes_; }

    // Valid only after the work that wrote it has completed. Returns an empty
    // span when the mapping fails.
    [[nodiscard]] std::span<const core::u8> map() noexcept;
    void unmap() noexcept;

private:
    friend class Device;
    friend class CommandList;

    Readback(Device& device, void* handle, core::u32 bytes) noexcept
        : device_(&device), handle_(handle), bytes_(bytes) {}

    void release() noexcept;

    Device* device_{};
    void* handle_{};
    core::u32 bytes_{};
    bool mapped_{};
};

// Work submitted to the queue, and the only per-frame timing SDL_GPU can
// actually supply: it has no timestamp query, so this measures wall time from
// submission until the fence signals. That is queue wait plus GPU execution,
// not GPU execution alone, and it is reported under that name so nobody reads
// it as a shader cost.
class SubmittedWork final {
public:
    SubmittedWork() noexcept = default;
    SubmittedWork(const SubmittedWork&) = delete;
    SubmittedWork& operator=(const SubmittedWork&) = delete;
    SubmittedWork(SubmittedWork&& other) noexcept;
    SubmittedWork& operator=(SubmittedWork&& other) noexcept;
    ~SubmittedWork();

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

    // True once the queue has finished this submission. Never blocks.
    [[nodiscard]] bool complete() noexcept;

    // Blocks until the submission completes and records how long it took.
    [[nodiscard]] bool wait() noexcept;

    [[nodiscard]] std::chrono::nanoseconds submit_to_complete() const noexcept {
        return elapsed_;
    }

private:
    friend class Device;

    SubmittedWork(
        Device& device,
        void* handle,
        std::chrono::steady_clock::time_point submitted_at) noexcept
        : device_(&device), handle_(handle), submitted_at_(submitted_at) {}

    void release() noexcept;

    Device* device_{};
    void* handle_{};
    std::chrono::steady_clock::time_point submitted_at_{};
    std::chrono::nanoseconds elapsed_{};
};

// One recorded batch of GPU work. Recording is not thread-safe and a list is
// consumed by submission, which is why it is move-only and inert afterwards.
class CommandList final {
public:
    CommandList() noexcept = default;
    CommandList(const CommandList&) = delete;
    CommandList& operator=(const CommandList&) = delete;
    CommandList(CommandList&& other) noexcept;
    CommandList& operator=(CommandList&& other) noexcept;
    ~CommandList();

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }

    // Replaces the contents of a storage buffer for this submission. The data
    // must fit the buffer's capacity; a larger write is refused rather than
    // truncated.
    [[nodiscard]] bool upload(
        StorageBuffer& buffer,
        std::span<const core::u8> data) noexcept;

    // Uploads whole-texture pixel data. Used once for the glyph atlas, not per
    // frame - the point of the GPU path is that per-frame pixel upload stops.
    [[nodiscard]] bool upload(
        Texture& texture,
        std::span<const core::u8> rgba) noexcept;

    // Begins a render pass that clears `target`, draws `instances` instanced
    // quads through `pipeline`, and ends. One call because that is the entire
    // shape of the output path, and a pass that a caller could leave open is a
    // state machine this seam does not need.
    [[nodiscard]] bool draw_instanced_quads(
        Texture& target,
        Color clear,
        const GraphicsPipeline& pipeline,
        const StorageBuffer& instances,
        core::u32 instance_count,
        const Texture& atlas,
        const Sampler& sampler) noexcept;

    [[nodiscard]] bool clear(Texture& target, Color colour) noexcept;

    [[nodiscard]] bool blit(
        const Texture& source,
        Texture& destination,
        Filter filter) noexcept;

    [[nodiscard]] bool download(const Texture& source, Readback& into) noexcept;

private:
    friend class Device;
    friend class Swapchain;

    CommandList(Device& device, void* handle) noexcept
        : device_(&device), handle_(handle) {}

    void release() noexcept;

    Device* device_{};
    void* handle_{};
};

struct SwapchainImage final {
    // Not owned. Valid only until the command list that acquired it is
    // submitted, which is why it is a borrowed view rather than a Texture.
    void* handle{};
    core::u32 width{};
    core::u32 height{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle != nullptr;
    }
};

// A window's presentable surface. Claiming a window is what turns a headless
// device into a displaying one; a device with no swapchain still renders and
// still reads back, which is what keeps the offscreen path testable.
class Swapchain final {
public:
    Swapchain() noexcept = default;
    Swapchain(const Swapchain&) = delete;
    Swapchain& operator=(const Swapchain&) = delete;
    Swapchain(Swapchain&& other) noexcept;
    Swapchain& operator=(Swapchain&& other) noexcept;
    ~Swapchain();

    [[nodiscard]] explicit operator bool() const noexcept {
        return window_ != nullptr;
    }

    // Waits for a presentable image and hands it to this command list. An
    // empty result with no error means the window is not currently showing -
    // minimised, or occluded - which playback treats as a skipped present.
    [[nodiscard]] SwapchainImage acquire(CommandList& commands) noexcept;

    // The mode actually in force. Reported because the requested one is not
    // always available and a silently vsync-limited output would look like a
    // slow GPU rather than a throttled one.
    [[nodiscard]] PresentMode present_mode() const noexcept { return mode_; }

    // Copies `source` onto the acquired image. The blit is the presentation:
    // no CPU pixel ever moves.
    [[nodiscard]] bool present(
        CommandList& commands,
        const SwapchainImage& image,
        const Texture& source,
        Filter filter) noexcept;

private:
    friend class Device;

    Swapchain(Device& device, void* window, PresentMode mode) noexcept
        : device_(&device), window_(window), mode_(mode) {}

    void release() noexcept;

    Device* device_{};
    void* window_{};
    PresentMode mode_{PresentMode::Vsync};
};

struct DeviceConfig final {
    // Enables backend validation. Costly, and worth it in a debug build.
    bool debug{};
    // Frames the backend may have in flight before acquire blocks. Two keeps
    // the queue fed without letting presentation latency grow unbounded.
    core::u32 frames_in_flight{2U};
};

// The GPU device. One per process; every resource above is created from it and
// must not outlive it.
class Device final {
public:
    [[nodiscard]] static core::Result<Device, RhiError> create(
        const DeviceConfig& config) noexcept;

    // True when this build could create a device at all. Lets a test skip
    // cleanly on a machine with no GPU instead of reporting a failure.
    [[nodiscard]] static bool available() noexcept;

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;
    ~Device();

    [[nodiscard]] std::string_view driver_name() const noexcept;
    [[nodiscard]] bool supports(ShaderFormat format) const noexcept;

    [[nodiscard]] core::Result<Texture, RhiError> create_texture(
        const TextureDesc& desc) noexcept;
    [[nodiscard]] core::Result<StorageBuffer, RhiError> create_storage_buffer(
        core::u32 bytes) noexcept;
    [[nodiscard]] core::Result<Sampler, RhiError> create_sampler(
        Filter filter) noexcept;
    [[nodiscard]] core::Result<Readback, RhiError> create_readback(
        core::u32 bytes) noexcept;

    // Both stages are consumed here rather than kept as separate objects: a
    // shader that is not part of a pipeline has no use in this seam.
    [[nodiscard]] core::Result<GraphicsPipeline, RhiError> create_pipeline(
        const ShaderDesc& vertex,
        const ShaderDesc& fragment,
        TextureFormat target_format,
        bool alpha_blend) noexcept;

    // Borrows an SDL_Window. Declared as void* so this header stays free of
    // the platform layer; the presenter that owns the window passes its own
    // handle straight through.
    // `preferred` is a request, not a guarantee: the platform decides, and the
    // resulting Swapchain reports what it got.
    [[nodiscard]] core::Result<Swapchain, RhiError> claim_window(
        void* window,
        PresentMode preferred = PresentMode::Immediate) noexcept;

    [[nodiscard]] core::Result<CommandList, RhiError> acquire_commands() noexcept;

    // Submits and returns the fence-backed handle. A failed submission
    // consumes the list either way: a command buffer cannot be re-submitted.
    [[nodiscard]] core::Result<SubmittedWork, RhiError> submit(
        CommandList commands) noexcept;

    // Blocks until the queue is idle. Startup and shutdown only.
    void wait_idle() noexcept;

    [[nodiscard]] void* backend_handle() const noexcept { return handle_; }

private:
    explicit Device(void* handle) noexcept : handle_(handle) {}

    void* handle_{};
};

} // namespace mediaserver::rhi
