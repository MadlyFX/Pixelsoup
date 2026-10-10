#pragma once
#include <mediaserver/core/frame_timing.hpp>
#include <array>
#include <mediaserver/core/render_conformance.hpp>

#include <chrono>
#include <cstddef>
#include <limits>
#include <memory>
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
    // This backend exposes no such operation, on any input. Distinct from
    // InvalidConfiguration, which means the request was malformed and a
    // corrected one would work, and from Unavailable, which is about the
    // machine rather than the backend. A caller seeing this must select a
    // different path, not retry with different arguments - and it is the
    // counterpart to an honest capability flag: whatever `capabilities()`
    // reports false, the matching entry point refuses with this.
    OperationUnsupported,
};

// The implementation object that owns resources. This is deliberately not the
// native graphics API: SDL GPU may select D3D12, Vulkan, or Metal internally,
// but it remains the owner and therefore the only code allowed to release the
// object. Native backends use their own value when they land.
enum class AssetPublicationDomain : core::u8 { Models, Environments, Probes, Ies };

enum class BackendKind : core::u8 {
    SdlGpu,
    D3d12,
    Vulkan,
};

enum class NativeObjectKind : core::u8 {
    Device,
    Texture,
    Buffer,
    CommandList,
    SwapchainImage,
    Swapchain,
};

// Typed native interop replaces the old convention that every void pointer was
// an SDL object. The pointer is still opaque to consumers, but a caller must
// check both tags before handing it to a platform API.
struct NativeHandle final {
    BackendKind backend{BackendKind::SdlGpu};
    NativeObjectKind object{NativeObjectKind::Device};
    void* value{};

    [[nodiscard]] explicit operator bool() const noexcept {
        return value != nullptr;
    }
    bool operator==(const NativeHandle&) const = default;
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
    Bc7Unorm,
    Bc5Unorm,
    // Scene-linear working formats. RGBA16F is the normal advanced render and
    // compositor target; RGBA32F exists for diagnostic/reference passes.
    Rgba16Float,
    Rgba32Float,
    // Motion vectors and scalar hit/variance products.
    Rg16Float,
    R32Float,
    // Stable document/object identity. Integer data is never color converted.
    R32Uint,
    // Video-plane sampling formats. R16 carries P010 luma and Rg16 its
    // interleaved chroma; neither is a presentation format.
    R16Unorm,
    Rg16Unorm,
    // A depth buffer. Never sampled and never presented; it exists so that
    // triangles occlude each other correctly, and it is the only non-colour
    // format this seam has a use for.
    Depth32Float,
    Rgb10A2Unorm,
};

[[nodiscard]] constexpr bool block_compressed(TextureFormat format) noexcept {
    return format == TextureFormat::Bc7Unorm || format == TextureFormat::Bc5Unorm;
}

enum class Filter : core::u8 {
    Nearest,
    Linear,
};

// Immutable sampler state. The legacy Filter overload retains clamp addressing.
enum class AddressMode : core::u8 { Clamp, Repeat, Mirror };
enum class SamplerCompare : core::u8 {
    Disabled, Never, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Always
};
struct SamplerDesc final {
    Filter min_filter{Filter::Linear};
    Filter mag_filter{Filter::Linear};
    Filter mip_filter{Filter::Linear};
    AddressMode address_u{AddressMode::Clamp};
    AddressMode address_v{AddressMode::Clamp};
    AddressMode address_w{AddressMode::Clamp};
    core::u32 max_anisotropy{1U};
    float lod_bias{};
    float min_lod{};
    float max_lod{std::numeric_limits<float>::max()};
    SamplerCompare comparison{SamplerCompare::Disabled};
    bool operator==(const SamplerDesc&) const = default;
};

enum class ShaderStage : core::u8 {
    Vertex,
    Fragment,
    Compute,
};

// A capability snapshot is immutable for the lifetime of a device. It reports
// operations callable through this RHI, not features merely exposed by the
// adapter or an underlying graphics API. It is queried during renderer
// construction/readiness, never probed on the frame path. Unsupported advanced
// requirements are therefore a deterministic gate, not a per-frame quality
// heuristic.
struct DeviceCapabilities final {
    BackendKind backend{BackendKind::SdlGpu};
    // The backend implements and tests the complete command/resource subset
    // used by today's Advanced scene rasterizer. Format booleans alone cannot
    // establish this: SDL_GPU may expose every attachment format while the
    // native Advanced implementation still exists only on D3D12.
    bool advanced_scene_raster{};
    bool offscreen{};
    bool rgba16_float{};
    bool rgba32_float{};
    bool sampled_depth{};
    bool integer_targets{};
    // A storage texture can be bound to a dispatch and read or written through
    // it. Not "this format and resource can be constructed", which is what it
    // meant before there was anything to dispatch: a backend that can make the
    // resource and not use it reports false.
    bool storage_images{};
    // create_compute_pipeline and CommandList::dispatch work on this device.
    bool compute{};
    bool bindless{};
    bool timestamp_queries{};
    bool explicit_barriers{};
    bool async_compute{};
    bool indirect_draw{};
    bool acceleration_structures{};
    bool ray_queries{};
    bool external_memory{};

    bool operator==(const DeviceCapabilities&) const = default;

    [[nodiscard]] bool advanced_raster() const noexcept {
        return advanced_scene_raster && offscreen && rgba16_float &&
            sampled_depth && integer_targets;
    }

    [[nodiscard]] bool advanced_ray_tracing() const noexcept {
        return advanced_raster() && acceleration_structures && ray_queries;
    }
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
    PremultipliedNormal,
    PremultipliedAdd,
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

// Native objects a vendor presentation implementation needs in order to join
// a barrier. The RHI owns every object; the span is borrowed for the duration
// of the call only. A native backend must copy values it needs after join.
struct PresentBarrierTarget final {
    NativeHandle device;
    NativeHandle swapchain;
    std::span<const NativeHandle> images;

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(device) && static_cast<bool>(swapchain);
    }
};

// A vendor backend can either let the RHI use IDXGISwapChain::Present as
// normal, perform presentation itself (NVAPI's legacy D3D1x route), or refuse
// the frame. The last two answers are used only for a swapchain it previously
// joined; every other output continues through the normal RHI path.
enum class PresentBarrierPresentResult : core::u8 {
    PassThrough,
    Presented,
    Failed,
};

// The timing authority for a genlocked show clock. A frame counter is a
// hardware fact, not a duration estimated from the operating-system clock.
// `Locked` carries the current modulo-2^32 frame count from the video
// reference. Other states deliberately carry no counter.
enum class VideoReferenceState : core::u8 {
    Unsupported,
    Waiting,
    Locked,
    Lost,
    Failed,
};

struct VideoReferenceSample final {
    VideoReferenceState state{VideoReferenceState::Unsupported};
    core::u32 frame_counter{};

    bool operator==(const VideoReferenceSample&) const = default;
};

// The seam a native presentation backend implements.
//
// On Windows/NVIDIA the production route is a native D3D12 presentation path
// that calls the NVAPI present-barrier functions; other vendors need their
// own. Nothing above this interface names a vendor, and a build with no
// backend installed keeps reporting Unsupported, which the Program readiness
// gate treats as SWAP BARRIER INCOMPLETE rather than as success.
//
// Implementations are process-wide registrations owned by the platform layer.
// Registry acquisition returns a shared lease, so uninstalling one while a
// swapchain still exists never leaves the swapchain with a dangling backend.
// Calls are made from the presenting thread.
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
        const NativeHandle& device) const noexcept = 0;
    [[nodiscard]] virtual PresentBarrierStatus join(
        const PresentBarrierTarget& target,
        const PresentBarrierDesc& desc) noexcept = 0;
    virtual void leave(const NativeHandle& swapchain) noexcept = 0;
    [[nodiscard]] virtual PresentBarrierStatus status(
        const NativeHandle& swapchain) const noexcept = 0;
    [[nodiscard]] virtual PresentBarrierStatistics statistics(
        const NativeHandle& swapchain) const noexcept = 0;

    // A reference-driven D3D12 backend may hold a reused backbuffer until the
    // driver signals its dedicated present-barrier fence. This is a poll: it
    // must never wait on the frame path.
    [[nodiscard]] virtual bool render_ready(
        const NativeHandle& swapchain,
        const NativeHandle& image) const noexcept {
        static_cast<void>(swapchain);
        static_cast<void>(image);
        return true;
    }

    // Called by the native D3D12 submit path at the actual presentation point.
    // The default keeps ordinary present semantics for backends that only
    // report state. A backend returning Presented has already issued the
    // native present operation.
    [[nodiscard]] virtual PresentBarrierPresentResult present(
        const NativeHandle& device,
        const NativeHandle& swapchain,
        const NativeHandle& image,
        core::u32 sync_interval,
        core::u32 flags) noexcept {
        static_cast<void>(device);
        static_cast<void>(swapchain);
        static_cast<void>(image);
        static_cast<void>(sync_interval);
        static_cast<void>(flags);
        return PresentBarrierPresentResult::PassThrough;
    }

    // DXGI may replace every backbuffer after a resize. The RHI calls this
    // only on that event; the default leaves a non-resource-owning backend's
    // cached status alone.
    [[nodiscard]] virtual PresentBarrierStatus refresh(
        const PresentBarrierTarget& target) noexcept {
        return status(target.swapchain);
    }

    // Used by the show layer through the presenter seam. Implementations
    // sample the video-reference frame counter; they must not synthesize one
    // from wall-clock time.
    [[nodiscard]] virtual VideoReferenceSample video_reference(
        const NativeHandle& device) const noexcept {
        static_cast<void>(device);
        return {};
    }
};

using PresentBarrierBackendHandle = std::shared_ptr<PresentBarrierBackend>;

// Installs (or clears, with an empty handle) the process-wide native
// presentation backend. Call before any swapchain is claimed. Every RHI call
// takes a short shared lease, so a swapchain may safely outlive installation.
void install_present_barrier_backend(PresentBarrierBackendHandle backend)
    noexcept;
[[nodiscard]] PresentBarrierBackendHandle acquire_present_barrier_backend()
    noexcept;

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
    // Read/write use from compute. Read-only sampling remains `sampled`; these
    // flags express UAV/storage access and are validated independently.
    bool storage_read{};
    bool storage_write{};
    // Arrays and mip chains are concrete DDGI/denoise requirements. Ordinary
    // 2D targets retain one layer and one level.
    core::u32 array_layers{1U};
    core::u32 mip_levels{1U};
    // Sample this six-layer array as a cubemap.
    //
    // It changes only how the texture is *read*: a cube view lets the hardware
    // filter across face edges, which an array view cannot. That matters for a
    // prefiltered environment - at high roughness the filter kernel spans face
    // boundaries, and without seamless filtering the seams are visible as lines
    // on every reflective surface in the shot. Writing still addresses one face
    // at a time through an ordinary array storage view.
    //
    // Requires exactly six layers.
    bool cube{};
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
    // Zero keeps the paired texture/sampler layout. Native graphics can bind
    // more SRVs while sharing the first sampler_count sampler descriptors.
    // Storage-buffer registers follow this resolved texture count.
    core::u32 sampled_texture_count{};
    core::u32 storage_buffer_count{};
    core::u32 uniform_buffer_count{};
    // Read-write texture bindings. Stated rather than inferred for the same
    // reason as the counts above: a root signature is built before any
    // dispatch is recorded, and guessing wrong is a validation error rather
    // than a bind of nothing. Zero for every graphics stage today.
    core::u32 storage_texture_count{};
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

// Queryable, allocation-free lifetime receipt for one command recording.
// Resource owners use it to publish persistent "uploaded/resident" state only
// after Device::submit accepts the recording, or roll back after cancellation.
// The receipt is safe to retain after CommandList is moved or destroyed; it
// owns no renderer/cache pointer and invokes no callbacks.
enum class CommandRecordingOutcome : core::u8 {
    Unavailable,
    Pending,
    Submitted,
    Canceled,
};

class CommandRecordingToken final {
public:
    CommandRecordingToken() noexcept = default;
    CommandRecordingToken(const CommandRecordingToken& other) noexcept;
    CommandRecordingToken& operator=(
        const CommandRecordingToken& other) noexcept;
    CommandRecordingToken(CommandRecordingToken&& other) noexcept;
    CommandRecordingToken& operator=(CommandRecordingToken&& other) noexcept;
    ~CommandRecordingToken();

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] CommandRecordingOutcome outcome() const noexcept;
    [[nodiscard]] bool same_recording(
        const CommandRecordingToken& other) const noexcept;

private:
    friend class CommandList;

    CommandRecordingToken(core::u32 slot, core::u64 identity) noexcept
        : slot_(slot), identity_(identity) {}
    void retain() noexcept;
    void release() noexcept;

    core::u32 slot_{std::numeric_limits<core::u32>::max()};
    core::u64 identity_{};
};

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
    // Nonblocking lifetime query for residency reclamation. SDL defers release
    // internally; native D3D12 reports recording and submitted fence ownership.
    [[nodiscard]] bool safe_to_release() const noexcept;

    [[nodiscard]] NativeHandle native_handle() const noexcept {
        return NativeHandle{backend_, NativeObjectKind::Texture, native_};
    }

    // The backend texture object (an SDL_GPUTexture* today). Declared void* so
    // this header stays free of the platform layer. Exposed so an operator
    // surface can hand a rendered target to a UI toolkit that composites it -
    // nothing in the frame path needs it.
    // Compatibility for UI toolkit adapters not yet migrated to NativeHandle.
    // New platform interop must use native_handle() and validate its tags.
    [[nodiscard]] void* backend_handle() const noexcept { return handle_; }

private:
    friend class Device;
    friend class CommandList;
    friend class Swapchain;

    Texture(Device& device, void* handle, void* native,
            BackendKind backend, const TextureDesc& desc) noexcept
        : device_(&device), handle_(handle), native_(native),
          backend_(backend), desc_(desc) {}

    void release() noexcept;

    Device* device_{};
    void* handle_{};
    void* native_{};
    BackendKind backend_{BackendKind::SdlGpu};
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
    // A device-local buffer written by range (Device::create_geometry_buffer).
    [[nodiscard]] bool ranged() const noexcept { return ranged_; }
    // Nonblocking lifetime query, as for Texture: SDL defers release
    // internally; native D3D12 reports recording and submitted fence use.
    [[nodiscard]] bool safe_to_release() const noexcept;
    [[nodiscard]] NativeHandle native_handle() const noexcept {
        return NativeHandle{backend_, NativeObjectKind::Buffer, native_};
    }
    [[nodiscard]] void* backend_handle() const noexcept { return handle_; }

private:
    friend class Device;
    friend class CommandList;

    StorageBuffer(Device& device, void* handle, void* native, void* staging,
                  BackendKind backend, core::u32 bytes, bool ranged = false) noexcept
        : device_(&device), handle_(handle), native_(native), staging_(staging),
          backend_(backend), bytes_(bytes), ranged_(ranged) {}

    void release() noexcept;

    Device* device_{};
    void* handle_{};
    void* native_{};
    // The upload staging buffer is owned with the buffer it feeds, so a caller
    // cannot pair the wrong one or forget to allocate it on the frame path.
    void* staging_{};
    BackendKind backend_{BackendKind::SdlGpu};
    core::u32 bytes_{};
    bool ranged_{};
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
    [[nodiscard]] void* backend_handle() const noexcept { return handle_; }

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
    [[nodiscard]] void* backend_handle() const noexcept { return handle_; }

private:
    friend class Device;
    friend class CommandList;

    GraphicsPipeline(Device& device, void* handle) noexcept
        : device_(&device), handle_(handle) {}

    void release() noexcept;

    Device* device_{};
    void* handle_{};
};

// One compute shader and the root signature its declared resource counts
// imply. Separate from GraphicsPipeline rather than a mode of it: the two share
// no state - no blend, no depth, no target format - and a single type carrying
// both would have to answer which half was meaningful at every call.
class ComputePipeline final {
public:
    ComputePipeline() noexcept = default;
    ComputePipeline(const ComputePipeline&) = delete;
    ComputePipeline& operator=(const ComputePipeline&) = delete;
    ComputePipeline(ComputePipeline&& other) noexcept;
    ComputePipeline& operator=(ComputePipeline&& other) noexcept;
    ~ComputePipeline();

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }
    [[nodiscard]] void* backend_handle() const noexcept { return handle_; }

private:
    friend class Device;
    friend class CommandList;

    ComputePipeline(Device& device, void* handle) noexcept
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
    [[nodiscard]] void* backend_handle() const noexcept { return handle_; }

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
    [[nodiscard]] void* backend_handle() const noexcept { return handle_; }

    // True once the queue has finished this submission. Never blocks.
    [[nodiscard]] bool complete() noexcept;

    // Blocks until the submission completes and records how long it took.
    [[nodiscard]] bool wait() noexcept;

    [[nodiscard]] std::chrono::nanoseconds submit_to_complete() const noexcept {
        return elapsed_;
    }

    [[nodiscard]] core::FrameTiming frame_timing() const noexcept;

    // GPU timestamp interval for backends with timestamp queries. Zero means
    // the backend cannot provide one; it is never synthesized from wall time.
    [[nodiscard]] std::chrono::nanoseconds gpu_execution() const noexcept {
        return gpu_elapsed_;
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
    std::chrono::nanoseconds gpu_elapsed_{};
};

// One batch of a pulled draw: how many vertex-stage invocations to make, and
// how many instances to make them for. No buffer range, because the vertex
// stage resolves its own addresses - see `CommandList::draw_pulled`.
struct DrawBatch final {
    core::u32 vertex_count{};
    core::u32 instance_count{};

    bool operator==(const DrawBatch&) const = default;
};

// What a resource is being used for, for an explicitly requested transition.
//
// Every draw, dispatch and copy already transitions what it touches, so a
// caller recording one pass at a time needs none of this. It exists for a
// caller that plans a whole frame ahead - a compiled render graph - and wants
// the barriers to come from the plan it validated rather than from whatever
// each pass body happens to do. Both routes go through the same state journal,
// so a cancelled list still restores exactly what it found.
enum class ResourceState : core::u8 {
    ColorTarget,
    DepthTarget,
    // Readable by any shader stage. One state rather than a graphics/compute
    // pair because a planner should not have to know which stage will read a
    // texture two passes from now, and the combined read state is legal for
    // both.
    ShaderRead,
    // Read-write from compute. Read and write are separate values because a
    // plan distinguishes them even though the resource state does not: the
    // difference decides where a barrier is needed, not what state to be in.
    StorageRead,
    StorageWrite,
    CopySource,
    CopyDestination,
    Present,
};

// A sub-rectangle of a render target, in target pixels.
//
// An empty rectangle - either extent zero - means the whole target, which is
// what every existing caller wants and what a default-constructed value gives
// them.
//
// Clearing stays whole-target in both backends. That is worth stating because
// it is a trap: a pass that sets a viewport and clears wipes the pixels outside
// the viewport too, so a caller rendering into a sub-rectangle of a target it
// wants to keep the rest of must load rather than clear.
struct ViewportRect final {
    core::u32 x{};
    core::u32 y{};
    core::u32 width{};
    core::u32 height{};

    [[nodiscard]] bool empty() const noexcept {
        return width == 0U || height == 0U;
    }
    // Whether this rectangle fits a target of the given extent. A viewport
    // that does not is refused rather than clamped: clamping would silently
    // rasterize a different region than the caller computed, and the caller
    // computed it from a slice geometry two nodes have to agree on.
    [[nodiscard]] bool fits(core::u32 target_width,
                            core::u32 target_height) const noexcept {
        return width <= target_width && height <= target_height
            && x <= target_width - width && y <= target_height - height;
    }

    bool operator==(const ViewportRect&) const = default;
};

// A full-canvas raster transform can extend outside a slice's render target.
// The target (or ViewportRect) still supplies the scissor. Keeping projection
// and viewport scale common avoids per-slice triangle interpolation drift.
struct RasterViewport final {
    float x{}, y{}, width{}, height{};
    [[nodiscard]] bool empty() const noexcept { return width == 0.0F && height == 0.0F; }
    [[nodiscard]] bool valid() const noexcept {
        return empty() || (width > 0.0F && height > 0.0F
            && x >= -32768.0F && y >= -32768.0F
            && x + width <= 32767.0F && y + height <= 32767.0F
            && width <= 32767.0F && height <= 32767.0F);
    }
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
    // Rasterize into this sub-rectangle of the target rather than all of it.
    //
    // This is what lets a padded slice render pay for only the apron the scene
    // actually authored: the target is allocated once for the largest apron the
    // project permits, and each frame rasterizes the region it needs. Empty
    // means the whole target, which is every caller that predates it.
    //
    // The clear is not scoped to it - see ViewportRect.
    ViewportRect viewport{};
    RasterViewport raster_viewport{};
};

// One read-write texture binding, naming an exact subresource.
//
// A whole-resource view would have been less to write and wrong from the first
// real user: prefiltering an environment map writes one mip of one cube face
// per dispatch, and a view spanning the chain has no way to say which. The mip
// and layer are therefore part of the binding rather than a later addition, and
// a single-level single-layer texture simply leaves both at zero.
struct StorageTexture final {
    const Texture* texture{};
    core::u32 mip_level{};
    core::u32 array_layer{};
    // Written by the dispatch. A read-only storage binding is still a UAV -
    // that is what distinguishes it from `sampled`, which cannot scatter - but
    // declaring the intent lets the backend leave a read-only resource out of
    // the UAV barrier it inserts between dependent dispatches.
    bool write{};
};

// Everything one dispatch needs, gathered rather than passed as ten arguments,
// matching `PulledDraw`. Spans are borrowed for the duration of the call and
// the uniform bytes are opaque here: the caller owns their layout.
struct ComputeDispatch final {
    const ComputePipeline* pipeline{};
    // Bound at t0, t1, ... in declaration order, exactly as many as the
    // shader declared. A count that disagrees is refused rather than partially
    // bound.
    std::span<const StorageBuffer* const> storage_buffers;
    std::span<const SampledTexture> sampled_textures;
    std::span<const StorageTexture> storage_textures;
    std::span<const core::u8> uniform;
    // Thread groups, not threads. The shader owns its group size and this is
    // the count of groups, so a caller that changes `numthreads` must change
    // this too - which is the honest coupling, and the alternative would be
    // this seam guessing a divisor it cannot know.
    core::u32 groups_x{1U};
    core::u32 groups_y{1U};
    core::u32 groups_z{1U};
};

// One recorded batch of GPU work. Recording is not thread-safe and a list is
// consumed by submission, which is why it is move-only and inert afterwards.
class CommandList final {
public:
    // A bounded timestamp pair. Zero is unavailable; overflow is counted.
    [[nodiscard]] core::u32 begin_gpu_pass(core::RenderPass pass) noexcept;
    void end_gpu_pass(core::u32 token) noexcept;
    [[nodiscard]] core::u64 upload_bytes() const noexcept;
    [[nodiscard]] Device* device() const noexcept { return device_; }

    CommandList() noexcept = default;
    CommandList(const CommandList&) = delete;
    CommandList& operator=(const CommandList&) = delete;
    CommandList(CommandList&& other) noexcept;
    CommandList& operator=(CommandList&& other) noexcept;
    ~CommandList();

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }
    [[nodiscard]] NativeHandle native_handle() const noexcept {
        return NativeHandle{backend_, NativeObjectKind::CommandList, native_};
    }
    [[nodiscard]] void* backend_handle() const noexcept { return handle_; }

    // Imported assets complete asynchronously. The first asset observer in a
    // recording latches the process-wide publication epoch here; every later
    // renderer, compositor layer, and mapped view using this same command list
    // receives exactly that epoch. This is an allocation-free local recording
    // boundary, not a cross-node asset-commit protocol. Zero is reserved for
    // "not latched" and is never accepted as a publication epoch.
    [[nodiscard]] core::u64 latch_asset_publication_epoch(
        core::u64 current, AssetPublicationDomain domain = AssetPublicationDomain::Models) noexcept {
        const auto index = static_cast<std::size_t>(domain);
        if (index >= asset_publication_epochs_.size()) return 0U;
        auto& epoch = asset_publication_epochs_[index];
        if (epoch == 0U && current != 0U) epoch = current;
        return epoch;
    }

    // Lazily claims a fixed process ledger slot for this recording. No heap,
    // wait, or callback is involved. An unavailable token means the bounded
    // ledger is full, so a caller that needs transactional residency must
    // preserve its stale answer and refuse new integration.
    [[nodiscard]] CommandRecordingToken recording_token() noexcept;

    // Replaces the contents of a storage buffer for subsequent commands in
    // this recording. Each successful non-empty upload is an immutable
    // snapshot for draws already recorded on the same command list, so it can
    // upload and draw the same logical buffer more than once. Snapshot storage
    // is fixed and bounded; exhaustion or an in-flight persistent-buffer
    // conflict is refused rather than waiting or allocating on the application
    // frame path. The data must fit the buffer's capacity; a larger write is
    // refused rather than truncated.
    [[nodiscard]] bool upload(
        StorageBuffer& buffer,
        std::span<const core::u8> data) noexcept;

    // Writes `data` at byte `offset` of a ranged buffer, for every later
    // command in this recording and every later submission. Draws recorded
    // earlier keep reading what was there, so the caller writes only ranges
    // that no outstanding draw reads. All or nothing: refused, with nothing
    // recorded, when this recording's upload space cannot hold the whole
    // write. Never waits or allocates device memory.
    [[nodiscard]] bool upload_range(
        StorageBuffer& buffer,
        core::u32 offset,
        std::span<const core::u8> data) noexcept;
    // Copies the first `bytes` of one ranged buffer into another on the GPU,
    // for growing an arena without re-uploading it.
    [[nodiscard]] bool copy_buffer(
        const StorageBuffer& source,
        StorageBuffer& destination,
        core::u32 bytes) noexcept;
    // Bytes upload_range can still accept in this recording.
    [[nodiscard]] core::u64 upload_space() const noexcept;

    // Uploads whole-texture pixel data. Used once for the glyph atlas, not per
    // frame - the point of the GPU path is that per-frame pixel upload stops.
    [[nodiscard]] bool upload(
        Texture& texture,
        std::span<const core::u8> pixels) noexcept;

    // A complete-width row range of one mip/layer. Allows a large immutable
    // texture to be staged over bounded recordings without exposing partial
    // contents to sampling. The caller owns that publication transaction.
    [[nodiscard]] bool upload_texture_rows(
        Texture& texture, core::u32 mip, core::u32 layer,
        core::u32 first_row, core::u32 row_count,
        std::span<const core::u8> pixels) noexcept;

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

    // One compute dispatch. No pass to open or close: like every other entry
    // point here it is a whole operation, because a pass a caller could leave
    // open is a state machine this seam does not need.
    //
    // Resource transitions are recorded through the same journal the draw
    // paths use, so a cancelled list restores every texture this touched. A
    // dispatch that writes a texture a later dispatch reads is separated by a
    // UAV barrier automatically - that dependency is visible here and nowhere
    // above, so making the caller ask for it would only invite forgetting.
    //
    // A refused dispatch records nothing. Every binding is resolved and
    // checked before the first barrier is written, so false leaves the list
    // exactly as it was and the caller may record something else on it. This
    // is stronger than the draw paths offer and it has to be: a storage
    // binding a texture never asked for would transition a resource into a
    // state it cannot hold, which the debug layer treats as fatal to the
    // whole list rather than to the one call.
    //
    // Determinism is the caller's to keep: the RHI guarantees the same bytes
    // in and the same bytes out, but a shader whose result depends on the
    // order thread groups happen to finish will disagree between two nodes
    // running identical work. See the note in
    // `advanced icvfx renderer/04-compute-in-the-rhi.md`.
    [[nodiscard]] bool dispatch(const ComputeDispatch& work) noexcept;

    // Puts  into  for the commands recorded after this.
    //
    // Recorded through the same journal every implicit transition uses, so a
    // cancelled list restores this texture along with the rest and a second
    // recording cannot touch a texture this one has claimed. Requesting the
    // state a texture already holds records nothing and succeeds.
    //
    // Refused with false on a backend whose `capabilities().explicit_barriers`
    // is false, and on a texture this recording may not claim.
    [[nodiscard]] bool transition(Texture& target, ResourceState state) noexcept;

    [[nodiscard]] bool clear(Texture& target, Color colour) noexcept;

    [[nodiscard]] bool blit(
        const Texture& source,
        Texture& destination,
        Filter filter) noexcept;

    // Copies a sub-rectangle of `source` to the origin of `destination`.
    //
    // An exact copy: no filter, no format conversion, no scale. This is the
    // crop at the end of a padded slice render, and it has to be exact -
    // resampling a rectangle that is already at the right pitch would change
    // the pixels this whole path exists to keep identical to a full-canvas
    // render.
    //
    // Formats must match and the rectangle must fit both textures. Anything
    // else is refused rather than adjusted.
    [[nodiscard]] bool copy_region(
        const Texture& source,
        ViewportRect region,
        Texture& destination) noexcept;

    [[nodiscard]] bool download(const Texture& source, Readback& into, core::u32 mip = 0U, core::u32 layer = 0U) noexcept;

private:
    friend class Device;
    friend class Swapchain;

    CommandList(Device& device, void* handle, void* native,
                BackendKind backend) noexcept
        : device_(&device), handle_(handle), native_(native), backend_(backend) {}

    void release() noexcept;
    void finish_recording(CommandRecordingOutcome outcome) noexcept;

    Device* device_{};
    void* handle_{};
    void* native_{};
    BackendKind backend_{BackendKind::SdlGpu};
    std::array<core::u64, 4U> asset_publication_epochs_{};
    // SDL has no fixed upload pages: ranged uploads are bounded per recording.
    core::u64 range_upload_bytes_{};
    CommandRecordingToken recording_token_;
    // Once the fixed ledger refuses this recording, every later subsystem in
    // the same list must receive the same refusal. Otherwise a slot released
    // between mapped views could make only the later view transactional.
    bool recording_token_attempted_{};
};

struct SwapchainImage final {
    // Not owned. Valid only until the command list that acquired it is
    // submitted, which is why it is a borrowed view rather than a Texture.
    void* handle{};
    void* native{};
    core::u32 width{};
    core::u32 height{};
    BackendKind backend{BackendKind::SdlGpu};

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle != nullptr;
    }

    [[nodiscard]] NativeHandle native_handle() const noexcept {
        return NativeHandle{backend, NativeObjectKind::SwapchainImage,
                            native != nullptr ? native : handle};
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

    // The native swapchain is exposed only as a typed opaque handle. It is
    // intended for the installed present-barrier backend; callers above RHI
    // must not assume a platform type from this value.
    [[nodiscard]] NativeHandle native_handle() const noexcept;

    // Polls for a presentable image and hands it to this command list. It
    // never waits for the desktop compositor or an in-flight backbuffer. An
    // empty result means playback keeps the previous image and skips present.
    [[nodiscard]] SwapchainImage acquire(CommandList& commands) noexcept;

    // The mode actually in force. Reported because the requested one is not
    // always available and a silently vsync-limited output would look like a
    // slow GPU rather than a throttled one.
    [[nodiscard]] PresentMode present_mode() const noexcept { return mode_; }
    [[nodiscard]] TextureFormat format() const noexcept { return format_; }

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

    Swapchain(Device& device, void* window, void* handle,
              PresentMode mode, TextureFormat format = TextureFormat::Rgba8Unorm) noexcept
        : device_(&device), window_(window), handle_(handle), mode_(mode), format_(format) {}

    void release() noexcept;

    Device* device_{};
    void* window_{};
    void* handle_{};
    PresentMode mode_{PresentMode::Vsync};
    TextureFormat format_{TextureFormat::Rgba8Unorm};
    PresentBarrierDesc barrier_{};
    PresentBarrierStatus barrier_status_{PresentBarrierStatus::Unsupported};
};

struct DeviceConfig final {
    // Enables backend validation. Costly, and worth it in a debug build.
    bool debug{};
    // Frames the backend may have in flight. Native acquire refuses when all
    // slots are busy; it never blocks the frame path. Two keeps the queue fed
    // without letting presentation latency grow unbounded.
    core::u32 frames_in_flight{2U};
    // A requested backend never silently selects another implementation.
    BackendKind backend{BackendKind::SdlGpu};
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
    [[nodiscard]] static bool available(BackendKind backend) noexcept;

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;
    ~Device();

    [[nodiscard]] core::RenderAdapterIdentity adapter_identity() const noexcept;
    [[nodiscard]] std::string_view driver_name() const noexcept;
    [[nodiscard]] bool supports(ShaderFormat format) const noexcept;
    [[nodiscard]] bool supports(TextureFormat format) const noexcept;
    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept {
        return capabilities_;
    }

    [[nodiscard]] core::Result<Texture, RhiError> create_texture(
        const TextureDesc& desc) noexcept;
    [[nodiscard]] core::Result<StorageBuffer, RhiError> create_storage_buffer(
        core::u32 bytes) noexcept;
    // A device-local buffer for large data that changes by range, such as
    // mesh arenas: written with CommandList::upload_range and grown with
    // CommandList::copy_buffer, never re-uploaded whole.
    [[nodiscard]] core::Result<StorageBuffer, RhiError> create_geometry_buffer(
        core::u32 bytes) noexcept;
    [[nodiscard]] core::Result<Sampler, RhiError> create_sampler(
        const SamplerDesc& desc) noexcept;
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

    // One shader, because a compute pipeline is one shader. Refused with
    // `OperationUnsupported` on a backend whose `capabilities().compute` is
    // false, rather than created and then failing at the first dispatch.
    [[nodiscard]] core::Result<ComputePipeline, RhiError> create_compute_pipeline(
        const ShaderDesc& compute) noexcept;

    // Borrows an SDL_Window. Declared as void* so this header stays free of
    // the platform layer; the presenter that owns the window passes its own
    // handle straight through.
    // `preferred` is a request, not a guarantee: the platform decides, and the
    // resulting Swapchain reports what it got.
    [[nodiscard]] core::Result<Swapchain, RhiError> claim_window(
        void* window,
        PresentMode preferred = PresentMode::Immediate,
        TextureFormat format = TextureFormat::Rgba8Unorm) noexcept;

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

    // Samples the hardware video-reference counter when an installed native
    // backend owns this D3D12 device. SDL_GPU and builds without that backend
    // honestly return Unsupported.
    [[nodiscard]] VideoReferenceSample video_reference() const noexcept;

    // Blocks until the queue is idle. Startup and shutdown only.
    void wait_idle() noexcept;

    [[nodiscard]] NativeHandle native_handle() const noexcept {
        return NativeHandle{capabilities_.backend, NativeObjectKind::Device,
                            native_};
    }

    // Backend implementation compatibility. Code above engine/rhi must use
    // native_handle() instead of assuming the pointer's concrete type.
    [[nodiscard]] void* backend_handle() const noexcept { return handle_; }

private:
    Device(void* handle, void* native, DeviceCapabilities capabilities) noexcept
        : handle_(handle), native_(native), capabilities_(capabilities) {}

    void* handle_{};
    void* native_{};
    DeviceCapabilities capabilities_{};
};

} // namespace mediaserver::rhi
