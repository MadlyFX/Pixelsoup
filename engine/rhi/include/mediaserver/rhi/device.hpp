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
    // A depth buffer. Never sampled and never presented; it exists so that
    // triangles occlude each other correctly, and it is the only non-colour
    // format this seam has a use for.
    Depth32Float,
};

enum class Filter : core::u8 {
    Nearest,
    Linear,
};

enum class ShaderStage : core::u8 {
    Vertex,
    Fragment,
};

// Fixed-function colour composition for a graphics pipeline. Multiply and
// Screen have a small source-colour convention documented by their shaders:
// multiply supplies lerp(white, source, opacity), while screen supplies source
// premultiplied by opacity. Keeping the equations explicit here avoids a
// misleading alpha_blend boolean once the renderer has real layer modes.
enum class ColorBlend : core::u8 {
    Disabled,
    Normal,
    Add,
    Multiply,
    Screen,
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

// Whether this device can join its swapchains to a vendor present barrier.
//
// Genlock and frame lock only align raster boundaries. A present (swap)
// barrier is what makes every joined output advance from image N to image N+1
// on the same aligned boundary; without it two perfectly genlocked outputs can
// sit one frame apart forever. It is an application-level concept - the
// Khronos WGL_NV_swap_group specification describes joining windows to swap
// groups and binding those groups to a barrier - so it belongs here, at the
// presentation seam, and not in the show clock.
enum class PresentBarrierSupport : core::u8 {
    // No vendor presentation backend is installed for this device. This is
    // the honest default: SDL_GPU owns its own swapchains and exposes no
    // native D3D12/Vulkan handle to hand a vendor barrier, so a build with no
    // native presentation backend reports Unsupported rather than silently
    // presenting unsynchronized.
    Unsupported,
    Available,
};

enum class PresentBarrierStatus : core::u8 {
    Unsupported,
    NotJoined,
    Joining,
    Joined,
    // Joined earlier and dropped out. Distinguished from NotJoined because it
    // is a loss-of-sync event rather than a configuration that never started.
    Left,
    Failed,
};

// The vendor swap-group / present-barrier identifiers. They are authored
// project state rather than derived, so a show can share a barrier with
// equipment this application does not own.
struct PresentBarrierDesc final {
    core::u32 group{1U};
    core::u32 barrier{1U};

    bool operator==(const PresentBarrierDesc&) const = default;
};

// What the barrier itself reports about presentation. `presents_in_sync` is
// the number that matters: a joined client whose presents stop counting as in
// sync has lost the barrier even though it is still nominally a member.
struct PresentBarrierStatistics final {
    core::u64 presents{};
    core::u64 presents_in_sync{};
    core::u64 flips_in_sync{};
    core::u64 leave_events{};

    bool operator==(const PresentBarrierStatistics&) const = default;
};

// The seam a native presentation backend implements.
//
// On Windows/NVIDIA the production route is a native D3D12 presentation path
// that calls the NVAPI present-barrier functions; other vendors need their
// own. Nothing above this interface names a vendor, and a build with no
// backend installed keeps reporting Unsupported, which the Program readiness
// gate treats as SWAP BARRIER INCOMPLETE rather than as success.
//
// Implementations are process-wide singletons owned by the platform layer and
// must outlive every Swapchain. Calls are made from the presenting thread.
class PresentBarrierBackend {
public:
    PresentBarrierBackend() = default;
    PresentBarrierBackend(const PresentBarrierBackend&) = delete;
    PresentBarrierBackend& operator=(const PresentBarrierBackend&) = delete;
    PresentBarrierBackend(PresentBarrierBackend&&) = delete;
    PresentBarrierBackend& operator=(PresentBarrierBackend&&) = delete;
    virtual ~PresentBarrierBackend() = default;

    // Stable identifier for diagnostics, e.g. "nvapi-d3d12".
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    // Why the backend is or is not usable, in operator-readable text.
    [[nodiscard]] virtual std::string_view detail() const noexcept = 0;
    [[nodiscard]] virtual PresentBarrierSupport support(
        void* device_handle) const noexcept = 0;
    [[nodiscard]] virtual PresentBarrierStatus join(
        void* device_handle,
        void* window,
        const PresentBarrierDesc& desc) noexcept = 0;
    virtual void leave(void* window) noexcept = 0;
    [[nodiscard]] virtual PresentBarrierStatus status(
        void* window) const noexcept = 0;
    [[nodiscard]] virtual PresentBarrierStatistics statistics(
        void* window) const noexcept = 0;
};

// Installs (or clears, with nullptr) the process-wide native presentation
// backend. Call before any swapchain is claimed; the pointer is borrowed.
void install_present_barrier_backend(PresentBarrierBackend* backend) noexcept;
[[nodiscard]] PresentBarrierBackend* present_barrier_backend() noexcept;

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
    // A depth attachment. Mutually exclusive with the other two: a depth
    // format is not a colour target and this seam has no reason to sample one.
    bool depth_target{};
    // Retain a CPU-to-GPU transfer buffer alongside this texture and cycle its
    // backing storage on each upload. Intended for decoded video frames that
    // replace the whole texture repeatedly; ordinary/static textures keep the
    // allocation-free descriptor and one-shot staging behaviour.
    bool streaming_upload{};

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

// Depth behaviour of a pipeline.
//
// Off by default, because the 2D output path that shipped first has no depth
// at all and should not acquire one by accident.
//
// `greater` rather than `less` is the default once depth is on, because the
// scene layer uses a reversed-depth projection: the near plane maps to one and
// the far plane to zero, which is where a floating-point depth buffer keeps
// its precision. See `scene::view_for_slice`. A pipeline that paired this with
// a conventional projection would draw the scene inside out, which is at least
// a mistake nobody could miss.
struct DepthState final {
    bool enabled{};
    bool write{true};
    bool greater{true};

    bool operator==(const DepthState&) const = default;
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

    // The backend texture object (an SDL_GPUTexture* today). Declared void* so
    // this header stays free of the platform layer. Exposed so an operator
    // surface can hand a rendered target to a UI toolkit that composites it -
    // nothing in the frame path needs it.
    [[nodiscard]] void* backend_handle() const noexcept { return handle_; }

private:
    friend class Device;
    friend class CommandList;
    friend class Swapchain;

    Texture(Device& device, void* handle, const TextureDesc& desc) noexcept
        : device_(&device), handle_(handle), desc_(desc) {}

    void release() noexcept;

    Device* device_{};
    void* handle_{};
    // An SDL_GPUTransferBuffer* when streaming_upload is enabled and the first
    // upload has occurred. Kept backend-opaque with the texture handle.
    void* upload_staging_{};
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

// One batch of a pulled draw: how many vertex-stage invocations to make, and
// how many instances to make them for. No buffer range, because the vertex
// stage resolves its own addresses - see `CommandList::draw_pulled`.
struct DrawBatch final {
    core::u32 vertex_count{};
    core::u32 instance_count{};

    bool operator==(const DrawBatch&) const = default;
};

// One fragment-stage texture/sampler pair. Pulled draws may bind one global
// set or one fixed-size set per batch; fullscreen passes consume a global set.
struct SampledTexture final {
    const Texture* texture{};
    const Sampler* sampler{};
};

// Everything one pulled pass needs, gathered rather than passed as ten
// arguments. Spans are borrowed for the duration of the call.
struct PulledDraw final {
    const GraphicsPipeline* pipeline{};
    // Bound to the vertex stage at t0, t1, ... in order, and to the fragment
    // stage likewise. Whatever the shaders declare.
    std::span<const StorageBuffer* const> vertex_buffers;
    std::span<const StorageBuffer* const> fragment_buffers;
    // Bound once before the first batch: the view, and the lighting.
    std::span<const core::u8> vertex_uniform;
    std::span<const core::u8> fragment_uniform;
    // `batches.size() * batch_uniform_stride` bytes, one record per batch,
    // bound at the vertex stage's second uniform slot before each batch. This
    // is where a batch says which part of the pulled buffers is its own.
    std::span<const core::u8> batch_uniforms;
    core::u32 batch_uniform_stride{};
    std::span<const DrawBatch> batches;
    // Optional fragment sampled textures. With textures_per_batch == 0 the
    // whole span is bound once. Otherwise it must contain exactly
    // batches.size() * textures_per_batch entries, rebound before each batch.
    // This keeps per-primitive glTF materials inside one depth-tested pass.
    std::span<const SampledTexture> fragment_textures;
    core::u32 fragment_textures_per_batch{};
    // Optional: a texture and sampler bound at the fragment stage's slot 0, for
    // source compatibility with callers whose fragment shader samples exactly
    // one (the media/UI paths). Both null leaves this legacy binding unused;
    // it may not be combined with fragment_textures.
    const Texture* texture{};
    const Sampler* sampler{};
    // Compose over the target's existing contents instead of clearing them, so
    // a second pulled pass can draw into the same colour and depth a first pass
    // produced (media over the scene, depth-tested against it). `clear` and
    // `depth_clear` are then ignored.
    bool load{};
    // Keep the depth buffer after the pass so a following pass can load and
    // depth-test against it. Off by default: the lit scene pass discards depth,
    // and sets this only when a media pass will follow.
    bool store_depth{};
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

    // One depth-tested pass over a list of batches that share a pipeline and a
    // set of pulled buffers. This is the whole 3D draw path.
    //
    // "Pulled" means there is no vertex buffer and no index buffer binding:
    // the vertex stage indexes storage buffers itself, using
    // `SV_VertexID` plus whatever offset its own constants carry. That is what
    // keeps this seam free of vertex input state, and it is the direction the
    // renderer is going anyway - a bindless, GPU-driven pipeline has no fixed
    // vertex layout to describe.
    //
    // The uniform bytes are opaque here. The caller owns their layout, and the
    // backend does nothing with them but hand them to the right slot. Offsets
    // into the pulled buffers ride in `batch_uniforms` rather than in the draw
    // call for a portability reason worth stating: D3D's `SV_InstanceID`
    // excludes the draw's first-instance while Vulkan's `gl_InstanceIndex`
    // includes it, so a first-instance argument would mean two different
    // things on two backends. Passing the base explicitly means one shader is
    // correct on both.
    [[nodiscard]] bool draw_pulled(
        Texture& color,
        Color clear,
        Texture& depth,
        float depth_clear,
        const PulledDraw& draw) noexcept;

    // One fullscreen textured pass: clear `target`, sample `source` through
    // `sampler` across a fullscreen triangle drawn by `pipeline`, and end. This
    // is the output stage - feed-map crop and soft-edge blend today, warp and
    // calibration LUT later - and it is a third draw shape on purpose.
    //
    // Neither existing entrypoint fits it. `draw_instanced_quads` binds a
    // sampler but pushes no uniforms and runs a fixed coverage-atlas shader;
    // `draw_pulled` pushes uniforms but binds no texture at all. The output
    // stage needs both: an arbitrary sampled source and a custom fragment
    // shader told, through `fragment_uniform`, where the source rectangle sits
    // and how wide its blend is. The bytes are opaque here - the caller owns
    // their layout, exactly as with `draw_pulled` - and are bound at the
    // fragment stage's uniform slot 0. There is no vertex buffer: the three
    // corners come from SV_VertexID.
    [[nodiscard]] bool draw_output_transform(
        Texture& target,
        Color clear,
        const GraphicsPipeline& pipeline,
        std::span<const SampledTexture> sources,
        std::span<const core::u8> fragment_uniform,
        bool load = false) noexcept;

    // Source-compatible one-texture convenience used by OutputTransform.
    [[nodiscard]] bool draw_output_transform(
        Texture& target,
        Color clear,
        const GraphicsPipeline& pipeline,
        const Texture& source,
        const Sampler& sampler,
        std::span<const core::u8> fragment_uniform,
        bool load = false) noexcept;

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

    // Joins this window to a vendor swap group and present barrier. Returns
    // the resulting status, which is Unsupported when no native presentation
    // backend is installed. Joining is idempotent for the same descriptor.
    [[nodiscard]] PresentBarrierStatus join_present_barrier(
        const PresentBarrierDesc& desc) noexcept;
    void leave_present_barrier() noexcept;
    [[nodiscard]] PresentBarrierStatus present_barrier_status()
        const noexcept;
    [[nodiscard]] PresentBarrierDesc present_barrier() const noexcept {
        return barrier_;
    }
    [[nodiscard]] PresentBarrierStatistics present_barrier_statistics()
        const noexcept;

private:
    friend class Device;

    Swapchain(Device& device, void* window, PresentMode mode) noexcept
        : device_(&device), window_(window), mode_(mode) {}

    void release() noexcept;

    Device* device_{};
    void* window_{};
    PresentMode mode_{PresentMode::Vsync};
    PresentBarrierDesc barrier_{};
    PresentBarrierStatus barrier_status_{PresentBarrierStatus::Unsupported};
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
    //
    // `depth` defaults to off so that the 2D output path reads the same as it
    // did before depth existed.
    [[nodiscard]] core::Result<GraphicsPipeline, RhiError> create_pipeline(
        const ShaderDesc& vertex,
        const ShaderDesc& fragment,
        TextureFormat target_format,
        ColorBlend blend,
        DepthState depth = DepthState{}) noexcept;

    // Compatibility for the UI/output paths while their only distinction is
    // still opaque versus ordinary straight-alpha composition.
    [[nodiscard]] core::Result<GraphicsPipeline, RhiError> create_pipeline(
        const ShaderDesc& vertex,
        const ShaderDesc& fragment,
        TextureFormat target_format,
        bool alpha_blend,
        DepthState depth = DepthState{}) noexcept;

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

    // Whether swapchains from this device can join a vendor present barrier,
    // and the operator-readable reason when they cannot.
    [[nodiscard]] PresentBarrierSupport present_barrier_support()
        const noexcept;
    [[nodiscard]] std::string_view present_barrier_detail() const noexcept;

    // Blocks until the queue is idle. Startup and shutdown only.
    void wait_idle() noexcept;

    [[nodiscard]] void* backend_handle() const noexcept { return handle_; }

private:
    explicit Device(void* handle) noexcept : handle_(handle) {}

    void* handle_{};
};

} // namespace mediaserver::rhi
