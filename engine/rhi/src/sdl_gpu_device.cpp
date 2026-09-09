#include <mediaserver/rhi/device.hpp>

#include "d3d12_backend.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace mediaserver::rhi {
namespace {

constexpr core::u32 recording_ledger_capacity = 1024U;
constexpr core::u32 invalid_recording_slot =
    std::numeric_limits<core::u32>::max();

struct RecordingLedgerSlot final {
    std::atomic<core::u64> identity{};
    std::atomic<core::u32> references{};
    std::atomic<CommandRecordingOutcome> outcome{
        CommandRecordingOutcome::Unavailable};
};

std::array<RecordingLedgerSlot, recording_ledger_capacity> recording_ledger;
std::atomic<core::u64> next_recording_identity{1U};

[[nodiscard]] core::u64 allocate_recording_identity() noexcept {
    core::u64 identity = next_recording_identity.fetch_add(
        1U, std::memory_order_relaxed);
    if (identity == 0U) {
        identity = next_recording_identity.fetch_add(
            1U, std::memory_order_relaxed);
    }
    return identity;
}

[[nodiscard]] SDL_GPUDevice* as_device(void* handle) noexcept {
    return static_cast<SDL_GPUDevice*>(handle);
}

[[nodiscard]] SDL_GPUTextureFormat as_format(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::Rgb10A2Unorm: return SDL_GPU_TEXTUREFORMAT_R10G10B10A2_UNORM;
    case TextureFormat::Bc7Unorm: return SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM;
    case TextureFormat::Bc5Unorm: return SDL_GPU_TEXTUREFORMAT_BC5_RG_UNORM;
    case TextureFormat::Bgra8Unorm:
        return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    case TextureFormat::Rgba16Float:
        return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_FLOAT;
    case TextureFormat::Rgba32Float:
        return SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
    case TextureFormat::Rg16Float:
        return SDL_GPU_TEXTUREFORMAT_R16G16_FLOAT;
    case TextureFormat::R32Float:
        return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
    case TextureFormat::R32Uint:
        return SDL_GPU_TEXTUREFORMAT_R32_UINT;
    case TextureFormat::R16Unorm:
        return SDL_GPU_TEXTUREFORMAT_R16_UNORM;
    case TextureFormat::Rg16Unorm:
        return SDL_GPU_TEXTUREFORMAT_R16G16_UNORM;
    case TextureFormat::Depth32Float:
        return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    case TextureFormat::Rgba8Unorm:
        break;
    }
    return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
}

[[nodiscard]] constexpr std::size_t bytes_per_pixel(
    TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::Bc7Unorm:
    case TextureFormat::Bc5Unorm: return 0U;
    case TextureFormat::R16Unorm:
        return 2U;
    case TextureFormat::Rg16Float:
        return 4U;
    case TextureFormat::Rgba16Float:
        return 8U;
    case TextureFormat::Rgba32Float:
        return 16U;
    case TextureFormat::Rgb10A2Unorm:
    case TextureFormat::Bgra8Unorm:
    case TextureFormat::Rg16Unorm:
    case TextureFormat::Rgba8Unorm:
    case TextureFormat::R32Float:
    case TextureFormat::R32Uint:
    case TextureFormat::Depth32Float:
        return 4U;
    }
    return 0U;
}

[[nodiscard]] constexpr bool is_depth(TextureFormat format) noexcept {
    return format == TextureFormat::Depth32Float;
}

[[nodiscard]] SDL_GPUFilter as_filter(Filter filter) noexcept {
    return filter == Filter::Linear ? SDL_GPU_FILTER_LINEAR
                                    : SDL_GPU_FILTER_NEAREST;
}

[[nodiscard]] SDL_GPUPresentMode as_present_mode(PresentMode mode) noexcept {
    switch (mode) {
    case PresentMode::Immediate:
        return SDL_GPU_PRESENTMODE_IMMEDIATE;
    case PresentMode::Mailbox:
        return SDL_GPU_PRESENTMODE_MAILBOX;
    case PresentMode::Vsync:
        break;
    }
    return SDL_GPU_PRESENTMODE_VSYNC;
}

[[nodiscard]] SDL_GPUShaderFormat as_shader_format(ShaderFormat format) noexcept {
    switch (format) {
    case ShaderFormat::Dxbc:
        return SDL_GPU_SHADERFORMAT_DXBC;
    case ShaderFormat::SpirV:
        return SDL_GPU_SHADERFORMAT_SPIRV;
    case ShaderFormat::Msl:
        return SDL_GPU_SHADERFORMAT_MSL;
    case ShaderFormat::Dxil:
        break;
    }
    return SDL_GPU_SHADERFORMAT_DXIL;
}

[[nodiscard]] constexpr bool valid_blend(ColorBlend blend) noexcept {
    switch (blend) {
    case ColorBlend::Disabled:
    case ColorBlend::Normal:
    case ColorBlend::Add:
    case ColorBlend::Multiply:
    case ColorBlend::Screen:
    case ColorBlend::PremultipliedNormal:
    case ColorBlend::PremultipliedAdd:
        return true;
    }
    return false;
}

void configure_blend(
    SDL_GPUColorTargetBlendState& state,
    ColorBlend blend) noexcept {
    if (blend == ColorBlend::Disabled) {
        return;
    }
    state.enable_blend = true;
    state.color_blend_op = SDL_GPU_BLENDOP_ADD;
    state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;

    switch (blend) {
    case ColorBlend::Normal:
        state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        state.dst_color_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        // Straight-alpha source over. With the scene's opaque clear this stays
        // one, while remaining correct for an alpha-bearing offscreen target.
        state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        state.dst_alpha_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case ColorBlend::Add:
        // Opacity weights only the added source; destination colour survives.
        state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        break;
    case ColorBlend::Multiply:
        // Shader supplies lerp(white, source, opacity), producing
        // destination * that value here.
        state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_DST_COLOR;
        state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        break;
    case ColorBlend::Screen:
        // Shader supplies source * opacity: S + D * (1 - S).
        state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        state.dst_color_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
        state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        break;
    case ColorBlend::PremultipliedNormal:
        state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        state.dst_color_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        state.dst_alpha_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case ColorBlend::PremultipliedAdd:
        state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
        state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        break;
    case ColorBlend::Disabled:
        break;
    }
}

// Every format this build knows how to hand a device. Asking for all of them
// lets the backend pick; the pipeline then checks that the bytecode it holds
// is one the chosen device actually accepts.
constexpr SDL_GPUShaderFormat all_shader_formats = SDL_GPU_SHADERFORMAT_DXIL
    | SDL_GPU_SHADERFORMAT_DXBC | SDL_GPU_SHADERFORMAT_SPIRV
    | SDL_GPU_SHADERFORMAT_MSL;

[[nodiscard]] bool ensure_video() noexcept {
    // SDL_GPU needs the video subsystem initialised even when nothing is
    // displayed. Initialising twice is refcounted, so this is safe to repeat.
    return SDL_InitSubSystem(SDL_INIT_VIDEO);
}

} // namespace

// ----------------------------------------------- CommandRecordingToken

CommandRecordingToken::CommandRecordingToken(
    const CommandRecordingToken& other) noexcept
    : slot_(other.slot_), identity_(other.identity_) {
    retain();
}

CommandRecordingToken& CommandRecordingToken::operator=(
    const CommandRecordingToken& other) noexcept {
    if (this != &other) {
        CommandRecordingToken retained{other};
        *this = std::move(retained);
    }
    return *this;
}

CommandRecordingToken::CommandRecordingToken(
    CommandRecordingToken&& other) noexcept
    : slot_(std::exchange(other.slot_, invalid_recording_slot)),
      identity_(std::exchange(other.identity_, 0U)) {}

CommandRecordingToken& CommandRecordingToken::operator=(
    CommandRecordingToken&& other) noexcept {
    if (this != &other) {
        release();
        slot_ = std::exchange(other.slot_, invalid_recording_slot);
        identity_ = std::exchange(other.identity_, 0U);
    }
    return *this;
}

CommandRecordingToken::~CommandRecordingToken() {
    release();
}

CommandRecordingToken::operator bool() const noexcept {
    return slot_ < recording_ledger_capacity && identity_ != 0U
        && recording_ledger[slot_].identity.load(std::memory_order_acquire)
            == identity_
        && recording_ledger[slot_].references.load(
               std::memory_order_relaxed) != 0U;
}

CommandRecordingOutcome CommandRecordingToken::outcome() const noexcept {
    if (!*this) {
        return CommandRecordingOutcome::Unavailable;
    }
    return recording_ledger[slot_].outcome.load(std::memory_order_acquire);
}

bool CommandRecordingToken::same_recording(
    const CommandRecordingToken& other) const noexcept {
    return *this && other && slot_ == other.slot_
        && identity_ == other.identity_;
}

void CommandRecordingToken::retain() noexcept {
    if (slot_ >= recording_ledger_capacity || identity_ == 0U
        || recording_ledger[slot_].identity.load(
               std::memory_order_acquire) != identity_) {
        slot_ = invalid_recording_slot;
        identity_ = 0U;
        return;
    }
    recording_ledger[slot_].references.fetch_add(
        1U, std::memory_order_relaxed);
}

void CommandRecordingToken::release() noexcept {
    if (slot_ < recording_ledger_capacity && identity_ != 0U
        && recording_ledger[slot_].identity.load(
               std::memory_order_acquire) == identity_) {
        recording_ledger[slot_].references.fetch_sub(
            1U, std::memory_order_release);
    }
    slot_ = invalid_recording_slot;
    identity_ = 0U;
}

// ----------------------------------------------------------------- Texture

Texture::Texture(Texture&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      handle_(std::exchange(other.handle_, nullptr)),
      native_(std::exchange(other.native_, nullptr)),
      upload_staging_(std::exchange(other.upload_staging_, nullptr)),
      backend_(std::exchange(other.backend_, BackendKind::SdlGpu)),
      desc_(other.desc_) {}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
        native_ = std::exchange(other.native_, nullptr);
        upload_staging_ = std::exchange(other.upload_staging_, nullptr);
        backend_ = std::exchange(other.backend_, BackendKind::SdlGpu);
        desc_ = other.desc_;
    }
    return *this;
}

Texture::~Texture() {
    release();
}

bool Texture::safe_to_release() const noexcept {
    return !*this || backend_ != BackendKind::D3d12 ||
        detail::d3d12_texture_safe_to_release(device_->backend_handle(), handle_);
}

void Texture::release() noexcept {
    if (device_ != nullptr) {
        if (backend_ == BackendKind::D3d12) {
            detail::d3d12_release_texture(device_->backend_handle(), handle_);
            device_ = nullptr;
            handle_ = nullptr;
            native_ = nullptr;
            upload_staging_ = nullptr;
            backend_ = BackendKind::SdlGpu;
            return;
        }
        auto* const device = as_device(device_->backend_handle());
        if (handle_ != nullptr) {
            SDL_ReleaseGPUTexture(
                device, static_cast<SDL_GPUTexture*>(handle_));
        }
        if (upload_staging_ != nullptr) {
            SDL_ReleaseGPUTransferBuffer(
                device,
                static_cast<SDL_GPUTransferBuffer*>(upload_staging_));
        }
    }
    device_ = nullptr;
    handle_ = nullptr;
    native_ = nullptr;
    upload_staging_ = nullptr;
    backend_ = BackendKind::SdlGpu;
}

// ----------------------------------------------------------- StorageBuffer

StorageBuffer::StorageBuffer(StorageBuffer&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      handle_(std::exchange(other.handle_, nullptr)),
      native_(std::exchange(other.native_, nullptr)),
      staging_(std::exchange(other.staging_, nullptr)),
      backend_(std::exchange(other.backend_, BackendKind::SdlGpu)),
      bytes_(std::exchange(other.bytes_, 0U)) {}

StorageBuffer& StorageBuffer::operator=(StorageBuffer&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
        native_ = std::exchange(other.native_, nullptr);
        staging_ = std::exchange(other.staging_, nullptr);
        backend_ = std::exchange(other.backend_, BackendKind::SdlGpu);
        bytes_ = std::exchange(other.bytes_, 0U);
    }
    return *this;
}

StorageBuffer::~StorageBuffer() {
    release();
}

void StorageBuffer::release() noexcept {
    if (device_ != nullptr) {
        if (backend_ == BackendKind::D3d12) {
            detail::d3d12_release_storage_buffer(
                device_->backend_handle(), handle_);
            device_ = nullptr;
            handle_ = nullptr;
            native_ = nullptr;
            staging_ = nullptr;
            backend_ = BackendKind::SdlGpu;
            bytes_ = 0U;
            return;
        }
        auto* const device = as_device(device_->backend_handle());
        if (handle_ != nullptr) {
            SDL_ReleaseGPUBuffer(device, static_cast<SDL_GPUBuffer*>(handle_));
        }
        if (staging_ != nullptr) {
            SDL_ReleaseGPUTransferBuffer(
                device, static_cast<SDL_GPUTransferBuffer*>(staging_));
        }
    }
    device_ = nullptr;
    handle_ = nullptr;
    native_ = nullptr;
    staging_ = nullptr;
    backend_ = BackendKind::SdlGpu;
    bytes_ = 0U;
}

// ----------------------------------------------------------------- Sampler

Sampler::Sampler(Sampler&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      handle_(std::exchange(other.handle_, nullptr)) {}

Sampler& Sampler::operator=(Sampler&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

Sampler::~Sampler() {
    release();
}

void Sampler::release() noexcept {
    if (device_ != nullptr && handle_ != nullptr) {
        if (device_->capabilities().backend == BackendKind::D3d12) {
            detail::d3d12_release_sampler(device_->backend_handle(), handle_);
            device_ = nullptr;
            handle_ = nullptr;
            return;
        }
        SDL_ReleaseGPUSampler(
            as_device(device_->backend_handle()),
            static_cast<SDL_GPUSampler*>(handle_));
    }
    device_ = nullptr;
    handle_ = nullptr;
}

// -------------------------------------------------------- GraphicsPipeline

GraphicsPipeline::GraphicsPipeline(GraphicsPipeline&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      handle_(std::exchange(other.handle_, nullptr)) {}

GraphicsPipeline& GraphicsPipeline::operator=(GraphicsPipeline&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

GraphicsPipeline::~GraphicsPipeline() {
    release();
}

void GraphicsPipeline::release() noexcept {
    if (device_ != nullptr && handle_ != nullptr) {
        if (device_->capabilities().backend == BackendKind::D3d12) {
            detail::d3d12_release_pipeline(device_->backend_handle(), handle_);
            device_ = nullptr;
            handle_ = nullptr;
            return;
        }
        SDL_ReleaseGPUGraphicsPipeline(
            as_device(device_->backend_handle()),
            static_cast<SDL_GPUGraphicsPipeline*>(handle_));
    }
    device_ = nullptr;
    handle_ = nullptr;
}

// ---------------------------------------------------- ComputePipeline

ComputePipeline::ComputePipeline(ComputePipeline&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      handle_(std::exchange(other.handle_, nullptr)) {}

ComputePipeline& ComputePipeline::operator=(ComputePipeline&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

ComputePipeline::~ComputePipeline() {
    release();
}

void ComputePipeline::release() noexcept {
    if (device_ != nullptr && handle_ != nullptr &&
        device_->capabilities().backend == BackendKind::D3d12) {
        detail::d3d12_release_compute_pipeline(
            device_->backend_handle(), handle_);
    }
    // No SDL branch: this device never hands out a compute pipeline, so a
    // non-null handle on one is impossible rather than merely unhandled.
    device_ = nullptr;
    handle_ = nullptr;
}

// ---------------------------------------------------------------- Readback

Readback::Readback(Readback&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      handle_(std::exchange(other.handle_, nullptr)),
      bytes_(std::exchange(other.bytes_, 0U)),
      mapped_(std::exchange(other.mapped_, false)) {}

Readback& Readback::operator=(Readback&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0U);
        mapped_ = std::exchange(other.mapped_, false);
    }
    return *this;
}

Readback::~Readback() {
    release();
}

std::span<const core::u8> Readback::map() noexcept {
    if (device_ == nullptr || handle_ == nullptr || mapped_) {
        return {};
    }
    if (device_->capabilities().backend == BackendKind::D3d12) {
        auto mapped = detail::d3d12_map_readback(handle_, bytes_);
        mapped_ = !mapped.empty();
        return mapped;
    }
    auto* const pixels = SDL_MapGPUTransferBuffer(
        as_device(device_->backend_handle()),
        static_cast<SDL_GPUTransferBuffer*>(handle_),
        false);
    if (pixels == nullptr) {
        return {};
    }
    mapped_ = true;
    return std::span<const core::u8>{
        static_cast<const core::u8*>(pixels), static_cast<std::size_t>(bytes_)};
}

void Readback::unmap() noexcept {
    if (device_ == nullptr || handle_ == nullptr || !mapped_) {
        return;
    }
    if (device_->capabilities().backend == BackendKind::D3d12) {
        detail::d3d12_unmap_readback(handle_);
        mapped_ = false;
        return;
    }
    SDL_UnmapGPUTransferBuffer(
        as_device(device_->backend_handle()),
        static_cast<SDL_GPUTransferBuffer*>(handle_));
    mapped_ = false;
}

void Readback::release() noexcept {
    unmap();
    if (device_ != nullptr && handle_ != nullptr) {
        if (device_->capabilities().backend == BackendKind::D3d12) {
            detail::d3d12_release_readback(device_->backend_handle(), handle_);
            device_ = nullptr;
            handle_ = nullptr;
            bytes_ = 0U;
            return;
        }
        SDL_ReleaseGPUTransferBuffer(
            as_device(device_->backend_handle()),
            static_cast<SDL_GPUTransferBuffer*>(handle_));
    }
    device_ = nullptr;
    handle_ = nullptr;
    bytes_ = 0U;
}

// ----------------------------------------------------------- SubmittedWork

SubmittedWork::SubmittedWork(SubmittedWork&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      handle_(std::exchange(other.handle_, nullptr)),
      submitted_at_(other.submitted_at_),
      elapsed_(other.elapsed_), gpu_elapsed_(other.gpu_elapsed_) {}

SubmittedWork& SubmittedWork::operator=(SubmittedWork&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
        submitted_at_ = other.submitted_at_;
        elapsed_ = other.elapsed_;
        gpu_elapsed_ = other.gpu_elapsed_;
    }
    return *this;
}

SubmittedWork::~SubmittedWork() {
    release();
}

bool SubmittedWork::complete() noexcept {
    if (device_ == nullptr || handle_ == nullptr) {
        return true;
    }
    if (device_->capabilities().backend == BackendKind::D3d12) {
        if (!detail::d3d12_work_complete(device_->backend_handle(), handle_)) {
            return false;
        }
        elapsed_ = std::chrono::steady_clock::now() - submitted_at_;
        gpu_elapsed_ = std::chrono::nanoseconds{
            detail::d3d12_work_gpu_nanoseconds(
                device_->backend_handle(), handle_)};
        return true;
    }
    if (!SDL_QueryGPUFence(
            as_device(device_->backend_handle()),
            static_cast<SDL_GPUFence*>(handle_))) {
        return false;
    }
    elapsed_ = std::chrono::steady_clock::now() - submitted_at_;
    return true;
}

bool SubmittedWork::wait() noexcept {
    if (device_ == nullptr || handle_ == nullptr) {
        return true;
    }
    if (device_->capabilities().backend == BackendKind::D3d12) {
        const bool waited =
            detail::d3d12_wait_work(device_->backend_handle(), handle_);
        elapsed_ = std::chrono::steady_clock::now() - submitted_at_;
        if (waited) {
            gpu_elapsed_ = std::chrono::nanoseconds{
                detail::d3d12_work_gpu_nanoseconds(
                    device_->backend_handle(), handle_)};
        }
        return waited;
    }
    auto* fence = static_cast<SDL_GPUFence*>(handle_);
    const auto waited =
        SDL_WaitForGPUFences(as_device(device_->backend_handle()), true, &fence, 1U);
    elapsed_ = std::chrono::steady_clock::now() - submitted_at_;
    return waited;
}

core::FrameTiming SubmittedWork::frame_timing() const noexcept {
    return device_ && handle_ && device_->capabilities().backend == BackendKind::D3d12
        ? detail::d3d12_work_frame_timing(device_->backend_handle(), handle_) : core::FrameTiming{};
}
core::u32 CommandList::begin_gpu_pass(core::RenderPass pass) noexcept {
    return backend_ == BackendKind::D3d12 ? detail::d3d12_begin_gpu_pass(handle_, pass) : 0U;
}
void CommandList::end_gpu_pass(core::u32 token) noexcept {
    if (backend_ == BackendKind::D3d12) detail::d3d12_end_gpu_pass(handle_, token);
}
core::u64 CommandList::upload_bytes() const noexcept {
    return backend_ == BackendKind::D3d12 ? detail::d3d12_upload_bytes(handle_) : 0U;
}

void SubmittedWork::release() noexcept {
    if (device_ != nullptr && handle_ != nullptr) {
        if (device_->capabilities().backend == BackendKind::D3d12) {
            detail::d3d12_release_work(device_->backend_handle(), handle_);
            device_ = nullptr;
            handle_ = nullptr;
            return;
        }
        SDL_ReleaseGPUFence(
            as_device(device_->backend_handle()),
            static_cast<SDL_GPUFence*>(handle_));
    }
    device_ = nullptr;
    handle_ = nullptr;
}

// ------------------------------------------------------------- CommandList

CommandList::CommandList(CommandList&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      handle_(std::exchange(other.handle_, nullptr)),
      native_(std::exchange(other.native_, nullptr)),
      backend_(std::exchange(other.backend_, BackendKind::SdlGpu)),
      asset_publication_epochs_(
          std::exchange(other.asset_publication_epochs_, {})),
      recording_token_(std::move(other.recording_token_)),
      recording_token_attempted_(
          std::exchange(other.recording_token_attempted_, false)) {}

CommandList& CommandList::operator=(CommandList&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
        native_ = std::exchange(other.native_, nullptr);
        backend_ = std::exchange(other.backend_, BackendKind::SdlGpu);
        asset_publication_epochs_ =
            std::exchange(other.asset_publication_epochs_, {});
        recording_token_ = std::move(other.recording_token_);
        recording_token_attempted_ =
            std::exchange(other.recording_token_attempted_, false);
    }
    return *this;
}

CommandList::~CommandList() {
    release();
}

void CommandList::release() noexcept {
    if (device_ != nullptr && handle_ != nullptr) {
        if (backend_ == BackendKind::D3d12) {
            detail::d3d12_release_commands(device_->backend_handle(), handle_);
            // Publish cancellation only after backend rollback has stopped
            // dereferencing every resource captured by this recording.
            finish_recording(CommandRecordingOutcome::Canceled);
            device_ = nullptr;
            handle_ = nullptr;
            native_ = nullptr;
            backend_ = BackendKind::SdlGpu;
            asset_publication_epochs_ = {};
            recording_token_attempted_ = false;
            return;
        }
        // Cancelling is the only correct disposal for a list nobody submitted:
        // the backend otherwise holds the buffer for the life of the device.
        static_cast<void>(SDL_CancelGPUCommandBuffer(
            static_cast<SDL_GPUCommandBuffer*>(handle_)));
    }
    finish_recording(CommandRecordingOutcome::Canceled);
    device_ = nullptr;
    handle_ = nullptr;
    native_ = nullptr;
    backend_ = BackendKind::SdlGpu;
    asset_publication_epochs_ = {};
    recording_token_attempted_ = false;
}

CommandRecordingToken CommandList::recording_token() noexcept {
    if (device_ == nullptr || handle_ == nullptr) {
        return {};
    }
    if (recording_token_) {
        return recording_token_;
    }
    if (recording_token_attempted_) {
        return {};
    }
    recording_token_attempted_ = true;
    for (core::u32 index = 0U; index < recording_ledger_capacity; ++index) {
        core::u32 expected{};
        if (!recording_ledger[index].references.compare_exchange_strong(
                expected, 1U, std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            continue;
        }
        const core::u64 identity = allocate_recording_identity();
        recording_ledger[index].identity.store(
            identity, std::memory_order_relaxed);
        recording_ledger[index].outcome.store(
            CommandRecordingOutcome::Pending, std::memory_order_release);
        recording_token_ = CommandRecordingToken{index, identity};
        return recording_token_;
    }
    return {};
}

void CommandList::finish_recording(CommandRecordingOutcome outcome) noexcept {
    if (recording_token_
        && outcome != CommandRecordingOutcome::Pending
        && outcome != CommandRecordingOutcome::Unavailable) {
        auto& slot = recording_ledger[recording_token_.slot_];
        if (slot.identity.load(std::memory_order_acquire)
            == recording_token_.identity_) {
            CommandRecordingOutcome expected =
                CommandRecordingOutcome::Pending;
            static_cast<void>(slot.outcome.compare_exchange_strong(
                expected, outcome, std::memory_order_release,
                std::memory_order_relaxed));
        }
    }
    recording_token_ = {};
}

bool CommandList::upload(
    StorageBuffer& buffer,
    std::span<const core::u8> data) noexcept {
    if (device_ == nullptr || handle_ == nullptr || !buffer
        || data.size() > static_cast<std::size_t>(buffer.bytes_)) {
        return false;
    }
    if (data.empty()) {
        return true;
    }
    if (backend_ == BackendKind::D3d12) {
        return detail::d3d12_upload_buffer(handle_, buffer.handle_, data);
    }

    auto* const device = as_device(device_->backend_handle());
    auto* const staging = static_cast<SDL_GPUTransferBuffer*>(buffer.staging_);
    // Both halves are cycled. A command list may upload the same logical
    // storage buffer for several mapped views before submission; cycling the
    // transfer source preserves the bytes captured by each copy, and cycling
    // the GPU destination preserves the resource captured by each earlier
    // draw. SDL leaves the current backing in place when it is unbound, so the
    // first upload does not rotate needlessly.
    auto* const mapped = SDL_MapGPUTransferBuffer(device, staging, true);
    if (mapped == nullptr) {
        return false;
    }
    std::copy(data.begin(), data.end(), static_cast<core::u8*>(mapped));
    SDL_UnmapGPUTransferBuffer(device, staging);

    auto* const pass =
        SDL_BeginGPUCopyPass(static_cast<SDL_GPUCommandBuffer*>(handle_));
    if (pass == nullptr) {
        return false;
    }
    const SDL_GPUTransferBufferLocation source{staging, 0U};
    const SDL_GPUBufferRegion destination{
        static_cast<SDL_GPUBuffer*>(buffer.handle_),
        0U,
        static_cast<Uint32>(data.size()),
    };
    SDL_UploadToGPUBuffer(pass, &source, &destination, true);
    SDL_EndGPUCopyPass(pass);
    return true;
}

bool CommandList::upload(Texture& texture, std::span<const core::u8> pixels) noexcept {
    if (texture.desc_.mip_levels != 1U || texture.desc_.array_layers != 1U)
        return false;
    return upload_texture_rows(texture, 0U, 0U, 0U, texture.height(), pixels);
}

bool CommandList::upload_texture_rows(
    Texture& texture, core::u32 mip, core::u32 layer,
    core::u32 first_row, core::u32 row_count,
    std::span<const core::u8> pixels) noexcept {
    if (device_ == nullptr || handle_ == nullptr || !texture
        || texture.device_ != device_ || mip >= texture.desc_.mip_levels ||
        mip >= 32U || layer >= texture.desc_.array_layers) {
        return false;
    }
    const core::u32 width = std::max(1U, texture.desc_.width >> mip);
    const core::u32 height = std::max(1U, texture.desc_.height >> mip);
    if (row_count == 0U || first_row >= height || row_count > height - first_row ||
        (texture.desc_.streaming_upload && (mip != 0U || layer != 0U ||
         first_row != 0U || row_count != height))) return false;
    const bool compressed = block_compressed(texture.desc_.format);
    if (compressed && (first_row % 4U != 0U || (row_count % 4U != 0U && first_row + row_count != height))) return false;
    const auto required = compressed
        ? static_cast<std::size_t>((width + 3U) / 4U) * ((row_count + 3U) / 4U) * 16U
        : static_cast<std::size_t>(width) * row_count * bytes_per_pixel(texture.desc_.format);
    if (pixels.size() != required || required == 0U
        || required
            > static_cast<std::size_t>(std::numeric_limits<Uint32>::max())) {
        return false;
    }
    if (backend_ == BackendKind::D3d12) {
        return detail::d3d12_upload_texture_rows(handle_, texture.handle_, mip, layer,
                                                   first_row, row_count, pixels);
    }

    auto* const device = as_device(device_->backend_handle());
    auto* staging =
        static_cast<SDL_GPUTransferBuffer*>(texture.upload_staging_);
    const bool streaming = texture.desc_.streaming_upload;
    if (staging == nullptr) {
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        info.size = static_cast<Uint32>(required);
        staging = SDL_CreateGPUTransferBuffer(device, &info);
        if (staging == nullptr) {
            return false;
        }
        if (streaming) {
            texture.upload_staging_ = staging;
        }
    }

    // A streaming texture retains this logical transfer buffer. Cycling asks
    // SDL to provide fresh backing storage only when an earlier submission is
    // still consuming the previous bytes, so recording never overwrites an
    // in-flight video frame. Static uploads preserve the transient path.
    auto* const mapped = SDL_MapGPUTransferBuffer(device, staging, streaming);
    if (mapped == nullptr) {
        if (!streaming) {
            SDL_ReleaseGPUTransferBuffer(device, staging);
        }
        return false;
    }
    std::copy(pixels.begin(), pixels.end(), static_cast<core::u8*>(mapped));
    SDL_UnmapGPUTransferBuffer(device, staging);

    auto* const pass =
        SDL_BeginGPUCopyPass(static_cast<SDL_GPUCommandBuffer*>(handle_));
    if (pass == nullptr) {
        if (!streaming) {
            SDL_ReleaseGPUTransferBuffer(device, staging);
        }
        return false;
    }
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = staging;
    source.pixels_per_row = compressed ? (width + 3U) / 4U * 4U : width;
    source.rows_per_layer = compressed ? (row_count + 3U) / 4U * 4U : row_count;
    SDL_GPUTextureRegion region{};
    region.texture = static_cast<SDL_GPUTexture*>(texture.handle_);
    region.mip_level = mip;
    region.layer = layer;
    region.y = first_row;
    region.w = width;
    region.h = row_count;
    region.d = 1U;
    SDL_UploadToGPUTexture(pass, &source, &region, false);
    SDL_EndGPUCopyPass(pass);

    if (!streaming) {
        // Releasing a transient transfer buffer is deferred by the backend
        // until the work referencing it retires, so this is safe before
        // submission. Streaming textures instead retain and cycle it.
        SDL_ReleaseGPUTransferBuffer(device, staging);
    }
    return true;
}

bool CommandList::draw_instanced_quads(
    Texture& target,
    Color clear,
    const GraphicsPipeline& pipeline,
    const StorageBuffer& instances,
    core::u32 instance_count,
    const Texture& atlas,
    const Sampler& sampler) noexcept {
    if (device_ == nullptr || handle_ == nullptr || !target || !pipeline
        || !instances || !atlas || !sampler) {
        return false;
    }
    if (backend_ == BackendKind::D3d12) {
        return detail::d3d12_draw_instanced_quads(
            handle_, target.handle_, clear, pipeline.handle_, instances.handle_,
            instance_count, atlas.handle_, sampler.handle_);
    }

    SDL_GPUColorTargetInfo colour{};
    colour.texture = static_cast<SDL_GPUTexture*>(target.handle_);
    colour.clear_color = SDL_FColor{clear.r, clear.g, clear.b, clear.a};
    colour.load_op = SDL_GPU_LOADOP_CLEAR;
    colour.store_op = SDL_GPU_STOREOP_STORE;

    auto* const pass = SDL_BeginGPURenderPass(
        static_cast<SDL_GPUCommandBuffer*>(handle_), &colour, 1U, nullptr);
    if (pass == nullptr) {
        return false;
    }
    if (instance_count != 0U) {
        SDL_BindGPUGraphicsPipeline(
            pass, static_cast<SDL_GPUGraphicsPipeline*>(pipeline.handle_));
        auto* const buffer = static_cast<SDL_GPUBuffer*>(instances.handle_);
        SDL_BindGPUVertexStorageBuffers(pass, 0U, &buffer, 1U);
        const SDL_GPUTextureSamplerBinding binding{
            static_cast<SDL_GPUTexture*>(atlas.handle_),
            static_cast<SDL_GPUSampler*>(sampler.handle_),
        };
        SDL_BindGPUFragmentSamplers(pass, 0U, &binding, 1U);
        // Six vertices per quad, one instance per record. The vertex shader
        // builds the corner from SV_VertexID, so there is no vertex buffer.
        SDL_DrawGPUPrimitives(pass, 6U, instance_count, 0U, 0U);
    }
    SDL_EndGPURenderPass(pass);
    return true;
}

bool CommandList::draw_pulled(
    Texture& color,
    Color clear,
    Texture& depth,
    float depth_clear,
    const PulledDraw& draw) noexcept {
    if (device_ == nullptr || handle_ == nullptr || !color || !depth
        || draw.pipeline == nullptr || !*draw.pipeline) {
        return false;
    }
    if (!draw.viewport.fits(color.width(), color.height()) || !draw.raster_viewport.valid()) {
        return false;
    }
    if (!depth.desc().depth_target
        || depth.width() != color.width() || depth.height() != color.height()) {
        // A depth buffer of a different size than its colour target is a
        // resize that half happened. Refusing beats rendering a frame that is
        // depth-tested against the wrong pixels.
        return false;
    }
    // Per-batch uniforms are optional: a pipeline whose vertex stage declares
    // fewer than two uniform slots has no per-batch record, and the backends
    // already gate the binding on that count. What is refused is a caller who
    // supplied one inconsistently - bytes without a stride would bind the same
    // record to every batch, a stride without bytes has nothing to bind, and a
    // stride with too few bytes would read past the end.
    const auto stride = static_cast<std::size_t>(draw.batch_uniform_stride);
    if (!draw.batches.empty()) {
        if (draw.batch_uniforms.empty() != (stride == 0U)) {
            return false;
        }
        if (stride != 0U
            && draw.batch_uniforms.size() < draw.batches.size() * stride) {
            return false;
        }
    }

    if (backend_ == BackendKind::D3d12) {
        return detail::d3d12_draw_pulled(
            handle_, color.handle_, clear, depth.handle_, depth_clear, draw);
    }

    constexpr std::size_t maximum_sampled_textures = 16U;
    const bool has_legacy_texture =
        draw.texture != nullptr || draw.sampler != nullptr;
    if ((draw.texture == nullptr) != (draw.sampler == nullptr) ||
        (has_legacy_texture &&
         (!*draw.texture || !*draw.sampler ||
          !draw.fragment_textures.empty()))) {
        return false;
    }
    const auto valid_sampled_textures = [](std::span<const SampledTexture> sources) {
        if (sources.size() > maximum_sampled_textures) {
            return false;
        }
        return std::all_of(
            sources.begin(), sources.end(), [](const SampledTexture& source) {
                return source.texture != nullptr && source.sampler != nullptr &&
                    *source.texture && *source.sampler;
            });
    };
    const auto textures_per_batch =
        static_cast<std::size_t>(draw.fragment_textures_per_batch);
    if (textures_per_batch == 0U) {
        if (!valid_sampled_textures(draw.fragment_textures)) {
            return false;
        }
    } else if (textures_per_batch > maximum_sampled_textures ||
               draw.batches.size() >
                   std::numeric_limits<std::size_t>::max() /
                       textures_per_batch ||
               draw.fragment_textures.size() !=
                   draw.batches.size() * textures_per_batch) {
        return false;
    } else {
        for (std::size_t batch = 0U; batch < draw.batches.size(); ++batch) {
            if (!valid_sampled_textures(draw.fragment_textures.subspan(
                    batch * textures_per_batch, textures_per_batch))) {
                return false;
            }
        }
    }

    // The most storage buffers either stage will bind. Four is what the scene
    // shader needs - vertices, indices, instances, lights - and a fixed array
    // keeps this call free of an allocation on the frame path.
    constexpr std::size_t max_pulled_buffers = 8U;
    std::array<SDL_GPUBuffer*, max_pulled_buffers> vertex_buffers{};
    std::array<SDL_GPUBuffer*, max_pulled_buffers> fragment_buffers{};
    core::u32 vertex_count{};
    core::u32 fragment_count{};

    // A lambda rather than a file-scope helper because the buffer handles are
    // private to StorageBuffer and reachable only from here.
    const auto gather = [](std::span<const StorageBuffer* const> source,
                           std::array<SDL_GPUBuffer*, max_pulled_buffers>& into,
                           core::u32& count) noexcept {
        if (source.size() > max_pulled_buffers) {
            return false;
        }
        count = 0U;
        for (const auto* const buffer : source) {
            if (buffer == nullptr || !*buffer) {
                return false;
            }
            into[count] = static_cast<SDL_GPUBuffer*>(buffer->handle_);
            ++count;
        }
        return true;
    };

    if (!gather(draw.vertex_buffers, vertex_buffers, vertex_count)
        || !gather(draw.fragment_buffers, fragment_buffers, fragment_count)) {
        return false;
    }

    auto* const commands = static_cast<SDL_GPUCommandBuffer*>(handle_);

    SDL_GPUColorTargetInfo colour{};
    colour.texture = static_cast<SDL_GPUTexture*>(color.handle_);
    colour.clear_color = SDL_FColor{clear.r, clear.g, clear.b, clear.a};
    // Load composes over what a previous pass produced; clear starts fresh.
    colour.load_op = draw.load ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
    colour.store_op = SDL_GPU_STOREOP_STORE;

    SDL_GPUDepthStencilTargetInfo depth_info{};
    depth_info.texture = static_cast<SDL_GPUTexture*>(depth.handle_);
    depth_info.clear_depth = depth_clear;
    depth_info.load_op = draw.load ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
    // Discarding depth lets a tiled backend skip writing it to memory, which is
    // the common case; a pass that a media pass will depth-test against keeps
    // it instead.
    depth_info.store_op =
        draw.store_depth ? SDL_GPU_STOREOP_STORE : SDL_GPU_STOREOP_DONT_CARE;
    depth_info.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth_info.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    // Cycle for a fresh clear (avoids a hazard with the previous frame's use);
    // a load must reuse the existing contents rather than rotate to a new one.
    depth_info.cycle = !draw.load;

    auto* const pass =
        SDL_BeginGPURenderPass(commands, &colour, 1U, &depth_info);
    if (pass == nullptr) {
        return false;
    }

    // Set after the pass begins and before anything is drawn, so the clear
    // above still covers the whole target while the rasterization does not.
    if (!draw.viewport.empty()) {
        const SDL_GPUViewport viewport{
            static_cast<float>(draw.viewport.x),
            static_cast<float>(draw.viewport.y),
            static_cast<float>(draw.viewport.width),
            static_cast<float>(draw.viewport.height),
            0.0F,
            1.0F,
        };
        SDL_SetGPUViewport(pass, &viewport);
        // The scissor as well as the viewport: the viewport transform alone
        // does not stop a primitive being rasterized outside it.
        const SDL_Rect scissor{
            static_cast<int>(draw.viewport.x),
            static_cast<int>(draw.viewport.y),
            static_cast<int>(draw.viewport.width),
            static_cast<int>(draw.viewport.height),
        };
        SDL_SetGPUScissor(pass, &scissor);
    }

    if (!draw.raster_viewport.empty()) {
        const auto& raster = draw.raster_viewport;
        const SDL_GPUViewport viewport{raster.x, raster.y, raster.width, raster.height, 0.0F, 1.0F};
        SDL_SetGPUViewport(pass, &viewport);
    }

    // An empty batch list still runs the pass, which clears the target. A node
    // whose document contains nothing to draw should show the environment, not
    // last frame's pixels.
    if (!draw.batches.empty()) {
        SDL_BindGPUGraphicsPipeline(
            pass, static_cast<SDL_GPUGraphicsPipeline*>(draw.pipeline->handle_));
        if (vertex_count != 0U) {
            SDL_BindGPUVertexStorageBuffers(
                pass, 0U, vertex_buffers.data(), vertex_count);
        }
        if (fragment_count != 0U) {
            SDL_BindGPUFragmentStorageBuffers(
                pass, 0U, fragment_buffers.data(), fragment_count);
        }
        std::array<SDL_GPUTextureSamplerBinding, maximum_sampled_textures>
            sampled_bindings{};
        const auto bind_sampled =
            [&](std::span<const SampledTexture> sources) noexcept {
            for (std::size_t index = 0U; index < sources.size(); ++index) {
                sampled_bindings[index] = SDL_GPUTextureSamplerBinding{
                    static_cast<SDL_GPUTexture*>(sources[index].texture->handle_),
                    static_cast<SDL_GPUSampler*>(sources[index].sampler->handle_)};
            }
            SDL_BindGPUFragmentSamplers(
                pass, 0U, sampled_bindings.data(),
                static_cast<core::u32>(sources.size()));
        };
        if (!draw.fragment_textures.empty() && textures_per_batch == 0U) {
            bind_sampled(draw.fragment_textures);
        } else if (has_legacy_texture) {
            const SDL_GPUTextureSamplerBinding binding{
                static_cast<SDL_GPUTexture*>(draw.texture->handle_),
                static_cast<SDL_GPUSampler*>(draw.sampler->handle_),
            };
            SDL_BindGPUFragmentSamplers(pass, 0U, &binding, 1U);
        }
        if (!draw.vertex_uniform.empty()) {
            SDL_PushGPUVertexUniformData(
                commands,
                0U,
                draw.vertex_uniform.data(),
                static_cast<core::u32>(draw.vertex_uniform.size()));
        }
        if (!draw.fragment_uniform.empty()) {
            SDL_PushGPUFragmentUniformData(
                commands,
                0U,
                draw.fragment_uniform.data(),
                static_cast<core::u32>(draw.fragment_uniform.size()));
        }

        for (std::size_t index = 0U; index < draw.batches.size(); ++index) {
            const auto& batch = draw.batches[index];
            if (batch.vertex_count == 0U || batch.instance_count == 0U) {
                continue;
            }
            if (textures_per_batch != 0U) {
                bind_sampled(draw.fragment_textures.subspan(
                    index * textures_per_batch, textures_per_batch));
            }
            if (stride != 0U) {
                SDL_PushGPUVertexUniformData(
                    commands,
                    1U,
                    draw.batch_uniforms.data() + index * stride,
                    draw.batch_uniform_stride);
            }
            // First vertex and first instance are always zero: the shader
            // resolves both from the record just pushed. See the header.
            SDL_DrawGPUPrimitives(
                pass, batch.vertex_count, batch.instance_count, 0U, 0U);
        }
    }

    SDL_EndGPURenderPass(pass);
    return true;
}

bool CommandList::draw_output_transform(
    Texture& target,
    Color clear,
    const GraphicsPipeline& pipeline,
    std::span<const SampledTexture> sources,
    std::span<const core::u8> fragment_uniform,
    bool load) noexcept {
    constexpr std::size_t maximum_sources = 8U;
    if (device_ == nullptr || handle_ == nullptr || !target || !pipeline ||
        sources.empty() || sources.size() > maximum_sources) {
        return false;
    }
    if (backend_ == BackendKind::D3d12) {
        return detail::d3d12_draw_output_transform(
            handle_, target.handle_, clear, pipeline.handle_, sources,
            fragment_uniform, load);
    }
    std::array<SDL_GPUTextureSamplerBinding, maximum_sources> bindings{};
    for (std::size_t index = 0U; index < sources.size(); ++index) {
        const SampledTexture& source = sources[index];
        if (source.texture == nullptr || source.sampler == nullptr ||
            !*source.texture || !*source.sampler) {
            return false;
        }
        bindings[index] = SDL_GPUTextureSamplerBinding{
            static_cast<SDL_GPUTexture*>(source.texture->handle_),
            static_cast<SDL_GPUSampler*>(source.sampler->handle_)};
    }

    auto* const commands = static_cast<SDL_GPUCommandBuffer*>(handle_);

    SDL_GPUColorTargetInfo colour{};
    colour.texture = static_cast<SDL_GPUTexture*>(target.handle_);
    colour.clear_color = SDL_FColor{clear.r, clear.g, clear.b, clear.a};
    colour.load_op = load ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
    colour.store_op = SDL_GPU_STOREOP_STORE;

    auto* const pass = SDL_BeginGPURenderPass(commands, &colour, 1U, nullptr);
    if (pass == nullptr) {
        return false;
    }

    SDL_BindGPUGraphicsPipeline(
        pass, static_cast<SDL_GPUGraphicsPipeline*>(pipeline.handle_));
    SDL_BindGPUFragmentSamplers(pass, 0U, bindings.data(),
                                static_cast<core::u32>(sources.size()));
    if (!fragment_uniform.empty()) {
        SDL_PushGPUFragmentUniformData(
            commands,
            0U,
            fragment_uniform.data(),
            static_cast<core::u32>(fragment_uniform.size()));
    }
    // Three vertices, one instance: the vertex shader builds a fullscreen
    // triangle from SV_VertexID, so there is no vertex buffer to bind.
    SDL_DrawGPUPrimitives(pass, 3U, 1U, 0U, 0U);

    SDL_EndGPURenderPass(pass);
    return true;
}

bool CommandList::draw_output_transform(
    Texture& target,
    Color clear,
    const GraphicsPipeline& pipeline,
    const Texture& source,
    const Sampler& sampler,
    std::span<const core::u8> fragment_uniform,
    bool load) noexcept {
    const std::array<SampledTexture, 1U> sources{{
        SampledTexture{&source, &sampler},
    }};
    return draw_output_transform(target, clear, pipeline, sources,
                                 fragment_uniform, load);
}

bool CommandList::dispatch(const ComputeDispatch& work) noexcept {
    if (device_ == nullptr || handle_ == nullptr || work.pipeline == nullptr ||
        !*work.pipeline) {
        return false;
    }
    // Only the backend that can produce a compute pipeline can consume one, so
    // there is no SDL branch to refuse: a pipeline from that device does not
    // exist, and this is unreachable rather than unimplemented.
    if (backend_ != BackendKind::D3d12) {
        return false;
    }
    return detail::d3d12_dispatch(handle_, work);
}

bool CommandList::transition(Texture& target, ResourceState state) noexcept {
    if (device_ == nullptr || handle_ == nullptr || !target) {
        return false;
    }
    // Gated on the capability rather than the backend kind, so the flag and the
    // entry point cannot drift apart. SDL_GPU owns its own barriers inside each
    // pass and exposes no way to ask for one, which is exactly what the flag
    // reporting false means.
    if (!device_->capabilities().explicit_barriers) {
        return false;
    }
    return detail::d3d12_transition(handle_, target.handle_, state);
}

bool CommandList::clear(Texture& target, Color colour) noexcept {
    if (device_ == nullptr || handle_ == nullptr || !target) {
        return false;
    }
    if (backend_ == BackendKind::D3d12) {
        return detail::d3d12_clear(handle_, target.handle_, colour);
    }
    SDL_GPUColorTargetInfo info{};
    info.texture = static_cast<SDL_GPUTexture*>(target.handle_);
    info.clear_color = SDL_FColor{colour.r, colour.g, colour.b, colour.a};
    info.load_op = SDL_GPU_LOADOP_CLEAR;
    info.store_op = SDL_GPU_STOREOP_STORE;
    auto* const pass = SDL_BeginGPURenderPass(
        static_cast<SDL_GPUCommandBuffer*>(handle_), &info, 1U, nullptr);
    if (pass == nullptr) {
        return false;
    }
    SDL_EndGPURenderPass(pass);
    return true;
}

bool CommandList::copy_region(
    const Texture& source,
    ViewportRect region,
    Texture& destination) noexcept {
    if (device_ == nullptr || handle_ == nullptr || !source || !destination ||
        region.empty() ||
        source.desc_.format != destination.desc_.format ||
        !region.fits(source.desc_.width, source.desc_.height) ||
        region.width > destination.desc_.width ||
        region.height > destination.desc_.height) {
        return false;
    }
    if (backend_ == BackendKind::D3d12) {
        return detail::d3d12_copy_region(
            handle_, source.handle_, region, destination.handle_);
    }
    // A same-size blit with matching source and destination rectangles. SDL
    // has no exact copy primitive, but a 1:1 blit performs no resampling, so
    // the pixels are unchanged - the filter is irrelevant when the rectangles
    // are the same size, and Nearest says so.
    SDL_GPUBlitInfo blit{};
    blit.source.texture = static_cast<SDL_GPUTexture*>(source.handle_);
    blit.source.x = region.x;
    blit.source.y = region.y;
    blit.source.w = region.width;
    blit.source.h = region.height;
    blit.destination.texture =
        static_cast<SDL_GPUTexture*>(destination.handle_);
    blit.destination.w = region.width;
    blit.destination.h = region.height;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = as_filter(Filter::Nearest);
    SDL_BlitGPUTexture(static_cast<SDL_GPUCommandBuffer*>(handle_), &blit);
    return true;
}

bool CommandList::blit(
    const Texture& source,
    Texture& destination,
    Filter filter) noexcept {
    if (device_ == nullptr || handle_ == nullptr || !source || !destination) {
        return false;
    }
    if (backend_ == BackendKind::D3d12) {
        return detail::d3d12_blit(
            handle_, source.handle_, destination.handle_, filter);
    }
    SDL_GPUBlitInfo blit{};
    blit.source.texture = static_cast<SDL_GPUTexture*>(source.handle_);
    blit.source.w = source.desc_.width;
    blit.source.h = source.desc_.height;
    blit.destination.texture = static_cast<SDL_GPUTexture*>(destination.handle_);
    blit.destination.w = destination.desc_.width;
    blit.destination.h = destination.desc_.height;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = as_filter(filter);
    SDL_BlitGPUTexture(static_cast<SDL_GPUCommandBuffer*>(handle_), &blit);
    return true;
}

bool CommandList::download(const Texture& source, Readback& into, core::u32 mip, core::u32 layer) noexcept {
    if (device_ == nullptr || handle_ == nullptr || !source || !into) {
        return false;
    }
    if (mip >= source.desc_.mip_levels || layer >= source.desc_.array_layers) return false;
    const auto required = static_cast<std::size_t>(std::max(1U, source.desc_.width >> mip))
        * std::max(1U, source.desc_.height >> mip) * bytes_per_pixel(source.desc_.format);
    if (required > static_cast<std::size_t>(into.bytes_)) {
        return false;
    }
    if (backend_ == BackendKind::D3d12) {
        return detail::d3d12_download(handle_, source.handle_, into.handle_, mip, layer);
    }
    if (mip != 0U || layer != 0U) return false;

    auto* const pass =
        SDL_BeginGPUCopyPass(static_cast<SDL_GPUCommandBuffer*>(handle_));
    if (pass == nullptr) {
        return false;
    }
    SDL_GPUTextureRegion region{};
    region.texture = static_cast<SDL_GPUTexture*>(source.handle_);
    region.w = source.desc_.width;
    region.h = source.desc_.height;
    region.d = 1U;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = static_cast<SDL_GPUTransferBuffer*>(into.handle_);
    destination.pixels_per_row = source.desc_.width;
    destination.rows_per_layer = source.desc_.height;
    SDL_DownloadFromGPUTexture(pass, &region, &destination);
    SDL_EndGPUCopyPass(pass);
    return true;
}

// --------------------------------------------------------------- Swapchain

Swapchain::Swapchain(Swapchain&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      window_(std::exchange(other.window_, nullptr)),
      handle_(std::exchange(other.handle_, nullptr)),
      mode_(other.mode_),
      format_(other.format_),
      barrier_(other.barrier_),
      barrier_status_(std::exchange(other.barrier_status_,
                                    PresentBarrierStatus::Unsupported)) {}

Swapchain& Swapchain::operator=(Swapchain&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, nullptr);
        window_ = std::exchange(other.window_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
        mode_ = other.mode_;
        format_ = other.format_;
        barrier_ = other.barrier_;
        barrier_status_ = std::exchange(other.barrier_status_,
                                        PresentBarrierStatus::Unsupported);
    }
    return *this;
}

Swapchain::~Swapchain() {
    release();
}

void Swapchain::release() noexcept {
    if (device_ != nullptr && window_ != nullptr) {
        leave_present_barrier();
        if (device_->capabilities().backend == BackendKind::D3d12) {
            detail::d3d12_release_swapchain(
                device_->backend_handle(), handle_);
        } else {
            SDL_ReleaseWindowFromGPUDevice(
                as_device(device_->backend_handle()),
                static_cast<SDL_Window*>(window_));
        }
    }
    device_ = nullptr;
    window_ = nullptr;
    handle_ = nullptr;
    barrier_status_ = PresentBarrierStatus::Unsupported;
}

NativeHandle Swapchain::native_handle() const noexcept {
    if (device_ == nullptr || handle_ == nullptr ||
        device_->capabilities().backend != BackendKind::D3d12) {
        return NativeHandle{device_ != nullptr
                                ? device_->capabilities().backend
                                : BackendKind::SdlGpu,
                            NativeObjectKind::Swapchain, nullptr};
    }
    return NativeHandle{BackendKind::D3d12, NativeObjectKind::Swapchain,
                        detail::d3d12_native_swapchain(handle_)};
}

PresentBarrierStatus Swapchain::join_present_barrier(
    const PresentBarrierDesc& desc) noexcept {
    if (device_ == nullptr || window_ == nullptr || desc.group == 0U ||
        desc.barrier == 0U) {
        barrier_status_ = PresentBarrierStatus::Failed;
        return barrier_status_;
    }
    const PresentBarrierBackendHandle backend =
        acquire_present_barrier_backend();
    if (backend == nullptr) {
        barrier_ = desc;
        barrier_status_ = PresentBarrierStatus::Unsupported;
        return barrier_status_;
    }
    if (barrier_status_ == PresentBarrierStatus::Joined && barrier_ == desc) {
        return barrier_status_;
    }
    if (barrier_status_ == PresentBarrierStatus::Joined) {
        backend->leave(native_handle());
    }

    std::array<NativeHandle, 4U> images{};
    std::size_t image_count{};
    if (device_->capabilities().backend == BackendKind::D3d12) {
        const std::span<void* const> native_images =
            detail::d3d12_present_barrier_images(handle_);
        image_count = std::min(images.size(), native_images.size());
        for (std::size_t index = 0U; index < image_count; ++index) {
            images[index] = NativeHandle{BackendKind::D3d12,
                                         NativeObjectKind::SwapchainImage,
                                         native_images[index]};
        }
    }
    barrier_ = desc;
    barrier_status_ = backend->join(
        PresentBarrierTarget{device_->native_handle(), native_handle(),
                             std::span<const NativeHandle>{images.data(),
                                                           image_count}},
        desc);
    return barrier_status_;
}

void Swapchain::leave_present_barrier() noexcept {
    const PresentBarrierBackendHandle backend =
        acquire_present_barrier_backend();
    if (backend == nullptr || window_ == nullptr) {
        return;
    }
    // A failed, lost, or unsupported native client may still retain its
    // target identity while the backend records diagnostics. Always offer it
    // teardown; the backend treats an unknown target as a no-op.
    backend->leave(native_handle());
    if (barrier_status_ != PresentBarrierStatus::Unsupported &&
        barrier_status_ != PresentBarrierStatus::NotJoined) {
        barrier_status_ = PresentBarrierStatus::Left;
    }
}

PresentBarrierStatus Swapchain::present_barrier_status() const noexcept {
    const PresentBarrierBackendHandle backend =
        acquire_present_barrier_backend();
    if (backend == nullptr || window_ == nullptr) {
        return PresentBarrierStatus::Unsupported;
    }
    // The backend is authoritative: a client can be dropped from a barrier
    // without this side ever calling leave().
    return backend->status(native_handle());
}

PresentBarrierStatistics Swapchain::present_barrier_statistics()
    const noexcept {
    const PresentBarrierBackendHandle backend =
        acquire_present_barrier_backend();
    if (backend == nullptr || window_ == nullptr) {
        return {};
    }
    return backend->statistics(native_handle());
}

SwapchainImage Swapchain::acquire(CommandList& commands) noexcept {
    if (device_ == nullptr || window_ == nullptr || !commands) {
        return {};
    }
    if (device_->capabilities().backend == BackendKind::D3d12) {
        const auto image = detail::d3d12_acquire_swapchain(
            device_->backend_handle(), handle_, commands.handle_);
        return SwapchainImage{image.state, image.native, image.width,
                              image.height, BackendKind::D3d12};
    }
    SDL_GPUTexture* texture = nullptr;
    Uint32 width = 0U;
    Uint32 height = 0U;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(
            static_cast<SDL_GPUCommandBuffer*>(commands.handle_),
            static_cast<SDL_Window*>(window_),
            &texture,
            &width,
            &height)) {
        return {};
    }
    // A null texture with a successful call is the documented "not showing"
    // outcome, not an error: the caller submits the list and skips the frame.
    return SwapchainImage{texture, texture, width, height,
                          BackendKind::SdlGpu};
}

bool Swapchain::present(
    CommandList& commands,
    const SwapchainImage& image,
    const Texture& source,
    Filter filter) noexcept {
    if (device_ == nullptr || !commands || !image || !source) {
        return false;
    }
    if (device_->capabilities().backend == BackendKind::D3d12) {
        return image.backend == BackendKind::D3d12 &&
            detail::d3d12_present_swapchain(
                commands.handle_, handle_, image.handle,
                source.backend_handle(), filter);
    }
    SDL_GPUBlitInfo blit{};
    blit.source.texture = static_cast<SDL_GPUTexture*>(source.handle_);
    blit.source.w = source.desc_.width;
    blit.source.h = source.desc_.height;
    blit.destination.texture = static_cast<SDL_GPUTexture*>(image.handle);
    blit.destination.w = image.width;
    blit.destination.h = image.height;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = as_filter(filter);
    SDL_BlitGPUTexture(static_cast<SDL_GPUCommandBuffer*>(commands.handle_), &blit);
    return true;
}

// ------------------------------------------------------------------ Device

bool Device::available() noexcept {
    return available(BackendKind::SdlGpu);
}

bool Device::available(BackendKind backend) noexcept {
    if (backend == BackendKind::D3d12) {
        return detail::d3d12_available();
    }
    if (backend != BackendKind::SdlGpu) {
        return false;
    }
    if (!ensure_video()) {
        return false;
    }
    return SDL_GPUSupportsShaderFormats(all_shader_formats, nullptr);
}

core::Result<Device, RhiError> Device::create(const DeviceConfig& config) noexcept {
    using Result = core::Result<Device, RhiError>;

    const core::u32 maximum_frames =
        config.backend == BackendKind::D3d12 ? 4U : 3U;
    if (config.frames_in_flight == 0U
        || config.frames_in_flight > maximum_frames) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    if (config.backend == BackendKind::D3d12) {
        auto created = detail::create_d3d12_device(config);
        if (!created) {
            return Result::failure(created.error());
        }
        return Result::success(Device{
            created.value().state,
            created.value().native,
            created.value().capabilities});
    }
    if (config.backend != BackendKind::SdlGpu) {
        return Result::failure(RhiError::Unavailable);
    }
    if (!ensure_video()) {
        return Result::failure(RhiError::Unavailable);
    }

    auto* device = SDL_CreateGPUDevice(all_shader_formats, config.debug, nullptr);
    if (device == nullptr && config.debug) {
        // A machine without the debug layer installed is a normal machine, and
        // losing validation is better than losing the output path.
        device = SDL_CreateGPUDevice(all_shader_formats, false, nullptr);
    }
    if (device == nullptr) {
        return Result::failure(RhiError::Unavailable);
    }
    if (!SDL_SetGPUAllowedFramesInFlight(device, config.frames_in_flight)) {
        SDL_DestroyGPUDevice(device);
        return Result::failure(RhiError::BackendFailure);
    }
    const auto format_supports = [device](
                                      TextureFormat format,
                                      SDL_GPUTextureUsageFlags usage) noexcept {
        return SDL_GPUTextureSupportsFormat(
            device, as_format(format), SDL_GPU_TEXTURETYPE_2D, usage);
    };
    DeviceCapabilities capabilities{};
    capabilities.backend = BackendKind::SdlGpu;
    // SDL remains the Basic renderer backend. Attachment format support does
    // not make the native D3D12 Advanced command path callable through it.
    capabilities.advanced_scene_raster = false;
    capabilities.offscreen = true;
    capabilities.rgba16_float = format_supports(
        TextureFormat::Rgba16Float,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    capabilities.rgba32_float = format_supports(
        TextureFormat::Rgba32Float,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    capabilities.sampled_depth = format_supports(
        TextureFormat::Depth32Float,
        SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET |
            SDL_GPU_TEXTUREUSAGE_SAMPLER);
    capabilities.integer_targets = format_supports(
        TextureFormat::R32Uint,
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER);
    // False, and it has to be: the flag now means bindable and dispatchable,
    // and this backend has no dispatch. That the formats support the usage is
    // exactly the sort of raw potential the flag stopped reporting when the
    // D3D12 path made it mean something callable.
    //
    // SDL_GPU itself has SDL_CreateGPUComputePipeline and friends. Wiring them
    // up is deferred rather than missing: the Advanced renderer is D3D12-gated,
    // so nothing can reach a dispatch through this backend yet, and a second
    // implementation with no caller is a second thing to keep correct. The
    // consequence is stated plainly - the Basic renderer cannot use a compute
    // pass until this lands.
    capabilities.storage_images = false;
    capabilities.compute = false;
    capabilities.indirect_draw = false;
    return Result::success(Device{device, device, capabilities});
}

Device::Device(Device&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)),
      native_(std::exchange(other.native_, nullptr)),
      capabilities_(other.capabilities_) {}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
            if (capabilities_.backend == BackendKind::D3d12) {
                detail::destroy_d3d12_device(handle_);
            } else {
                SDL_DestroyGPUDevice(as_device(handle_));
            }
        }
        handle_ = std::exchange(other.handle_, nullptr);
        native_ = std::exchange(other.native_, nullptr);
        capabilities_ = other.capabilities_;
    }
    return *this;
}

Device::~Device() {
    if (handle_ != nullptr) {
        if (capabilities_.backend == BackendKind::D3d12) {
            detail::destroy_d3d12_device(handle_);
        } else {
            SDL_DestroyGPUDevice(as_device(handle_));
        }
        handle_ = nullptr;
        native_ = nullptr;
    }
}

core::RenderAdapterIdentity Device::adapter_identity() const noexcept {
    return handle_ && capabilities_.backend == BackendKind::D3d12
        ? detail::d3d12_adapter_identity(handle_) : core::RenderAdapterIdentity{};
}

std::string_view Device::driver_name() const noexcept {
    if (handle_ == nullptr) {
        return {};
    }
    if (capabilities_.backend == BackendKind::D3d12) {
        return detail::d3d12_driver_name(handle_);
    }
    const auto* const name = SDL_GetGPUDeviceDriver(as_device(handle_));
    return name == nullptr ? std::string_view{} : std::string_view{name};
}

bool Device::supports(ShaderFormat format) const noexcept {
    if (handle_ == nullptr) {
        return false;
    }
    if (capabilities_.backend == BackendKind::D3d12) {
        return detail::d3d12_supports_shader(format);
    }
    return (SDL_GetGPUShaderFormats(as_device(handle_)) & as_shader_format(format))
        != 0U;
}

bool Device::supports(TextureFormat format) const noexcept {
    if (handle_ == nullptr) {
        return false;
    }
    if (capabilities_.backend == BackendKind::D3d12) {
        return detail::d3d12_supports_texture(handle_, format);
    }
    SDL_GPUTextureUsageFlags usage = is_depth(format)
        ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET
        : SDL_GPU_TEXTUREUSAGE_SAMPLER;
    return SDL_GPUTextureSupportsFormat(
        as_device(handle_), as_format(format), SDL_GPU_TEXTURETYPE_2D, usage);
}

core::Result<Texture, RhiError> Device::create_texture(
    const TextureDesc& desc) noexcept {
    using Result = core::Result<Texture, RhiError>;

    if (handle_ == nullptr) {
        return Result::failure(RhiError::Unavailable);
    }
    if (desc.width == 0U || desc.height == 0U || desc.array_layers == 0U ||
        desc.mip_levels == 0U ||
        (!desc.color_target && !desc.sampled && !desc.depth_target &&
         !desc.storage_read && !desc.storage_write)) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    // A depth format is a depth attachment and nothing else, and a colour
    // format cannot be one. Both mismatches are configuration errors here
    // rather than validation-layer noise later.
    if ((block_compressed(desc.format) && (desc.color_target || desc.depth_target || desc.storage_read || desc.storage_write || desc.streaming_upload)) ||
        desc.depth_target != is_depth(desc.format) ||
        (desc.depth_target &&
         (desc.color_target || desc.storage_read || desc.storage_write)) ||
        (desc.streaming_upload &&
         (desc.depth_target || desc.storage_read || desc.storage_write))) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    if (capabilities_.backend == BackendKind::D3d12) {
        auto created = detail::d3d12_create_texture(handle_, desc);
        if (!created) {
            return Result::failure(created.error());
        }
        return Result::success(Texture{
            *this,
            created.value().state,
            created.value().native,
            BackendKind::D3d12,
            desc});
    }

    SDL_GPUTextureCreateInfo info{};
    info.type = desc.array_layers > 1U ? SDL_GPU_TEXTURETYPE_2D_ARRAY
                                      : SDL_GPU_TEXTURETYPE_2D;
    info.format = as_format(desc.format);
    info.usage = (desc.color_target ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0U)
        | (desc.sampled ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0U)
        | (desc.depth_target ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET : 0U)
        | (desc.storage_read ? SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_READ : 0U)
        | (desc.storage_write ? SDL_GPU_TEXTUREUSAGE_COMPUTE_STORAGE_WRITE : 0U);
    info.width = desc.width;
    info.height = desc.height;
    info.layer_count_or_depth = desc.array_layers;
    info.num_levels = desc.mip_levels;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    if (!SDL_GPUTextureSupportsFormat(
            as_device(handle_), info.format, info.type, info.usage)) {
        return Result::failure(RhiError::InvalidConfiguration);
    }

    auto* const texture = SDL_CreateGPUTexture(as_device(handle_), &info);
    if (texture == nullptr) {
        return Result::failure(RhiError::AllocationFailed);
    }
    return Result::success(
        Texture{*this, texture, texture, BackendKind::SdlGpu, desc});
}

core::Result<StorageBuffer, RhiError> Device::create_storage_buffer(
    core::u32 bytes) noexcept {
    using Result = core::Result<StorageBuffer, RhiError>;

    if (handle_ == nullptr) {
        return Result::failure(RhiError::Unavailable);
    }
    if (bytes == 0U) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    if (capabilities_.backend == BackendKind::D3d12) {
        auto created = detail::d3d12_create_storage_buffer(handle_, bytes);
        if (!created) {
            return Result::failure(created.error());
        }
        return Result::success(StorageBuffer{
            *this,
            created.value().state,
            created.value().native,
            nullptr,
            BackendKind::D3d12,
            bytes});
    }

    auto* const device = as_device(handle_);
    SDL_GPUBufferCreateInfo info{};
    info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    info.size = bytes;
    auto* const buffer = SDL_CreateGPUBuffer(device, &info);
    if (buffer == nullptr) {
        return Result::failure(RhiError::AllocationFailed);
    }

    SDL_GPUTransferBufferCreateInfo staging_info{};
    staging_info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    staging_info.size = bytes;
    auto* const staging = SDL_CreateGPUTransferBuffer(device, &staging_info);
    if (staging == nullptr) {
        SDL_ReleaseGPUBuffer(device, buffer);
        return Result::failure(RhiError::AllocationFailed);
    }
    return Result::success(StorageBuffer{
        *this, buffer, buffer, staging, BackendKind::SdlGpu, bytes});
}

core::Result<Sampler, RhiError> Device::create_sampler(Filter filter) noexcept {
    SamplerDesc desc{};
    desc.min_filter = desc.mag_filter = desc.mip_filter = filter;
    return create_sampler(desc);
}

core::Result<Sampler, RhiError> Device::create_sampler(const SamplerDesc& desc) noexcept {
    using Result = core::Result<Sampler, RhiError>;

    if (desc.min_filter > Filter::Linear || desc.mag_filter > Filter::Linear
        || desc.mip_filter > Filter::Linear || desc.address_u > AddressMode::Mirror
        || desc.address_v > AddressMode::Mirror || desc.address_w > AddressMode::Mirror
        || desc.comparison > SamplerCompare::Always || desc.max_anisotropy < 1U
        || desc.max_anisotropy > 16U || !std::isfinite(desc.lod_bias)
        || desc.lod_bias < -16.0F || desc.lod_bias > 15.99F
        || !std::isfinite(desc.min_lod) || !std::isfinite(desc.max_lod)
        || desc.min_lod < 0.0F || desc.max_lod < desc.min_lod
        || (desc.max_anisotropy > 1U && (desc.min_filter != Filter::Linear
            || desc.mag_filter != Filter::Linear || desc.mip_filter != Filter::Linear))) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    if (handle_ == nullptr) {
        return Result::failure(RhiError::Unavailable);
    }
    if (capabilities_.backend == BackendKind::D3d12) {
        auto created = detail::d3d12_create_sampler(handle_, desc);
        if (!created) {
            return Result::failure(created.error());
        }
        return Result::success(Sampler{*this, created.value()});
    }
    SDL_GPUSamplerCreateInfo info{};
    info.min_filter = as_filter(desc.min_filter);
    info.mag_filter = as_filter(desc.mag_filter);
    info.mipmap_mode = desc.mip_filter == Filter::Linear ? SDL_GPU_SAMPLERMIPMAPMODE_LINEAR
                                                       : SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    const auto address = [](AddressMode mode) {
        switch (mode) {
        case AddressMode::Repeat: return SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
        case AddressMode::Mirror: return SDL_GPU_SAMPLERADDRESSMODE_MIRRORED_REPEAT;
        default: return SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        }
    };
    info.address_mode_u = address(desc.address_u);
    info.address_mode_v = address(desc.address_v);
    info.address_mode_w = address(desc.address_w);
    info.mip_lod_bias = desc.lod_bias;
    info.min_lod = desc.min_lod;
    info.max_lod = desc.max_lod;
    info.enable_anisotropy = desc.max_anisotropy > 1U;
    info.max_anisotropy = static_cast<float>(desc.max_anisotropy);
    info.enable_compare = desc.comparison != SamplerCompare::Disabled;
    constexpr std::array compares{SDL_GPU_COMPAREOP_ALWAYS, SDL_GPU_COMPAREOP_NEVER,
        SDL_GPU_COMPAREOP_LESS, SDL_GPU_COMPAREOP_EQUAL, SDL_GPU_COMPAREOP_LESS_OR_EQUAL,
        SDL_GPU_COMPAREOP_GREATER, SDL_GPU_COMPAREOP_NOT_EQUAL,
        SDL_GPU_COMPAREOP_GREATER_OR_EQUAL, SDL_GPU_COMPAREOP_ALWAYS};
    info.compare_op = compares[static_cast<std::size_t>(desc.comparison)];

    auto* const sampler = SDL_CreateGPUSampler(as_device(handle_), &info);
    if (sampler == nullptr) {
        return Result::failure(RhiError::AllocationFailed);
    }
    return Result::success(Sampler{*this, sampler});
}

core::Result<Readback, RhiError> Device::create_readback(core::u32 bytes) noexcept {
    using Result = core::Result<Readback, RhiError>;

    if (handle_ == nullptr) {
        return Result::failure(RhiError::Unavailable);
    }
    if (bytes == 0U) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    if (capabilities_.backend == BackendKind::D3d12) {
        auto created = detail::d3d12_create_readback(handle_, bytes);
        if (!created) {
            return Result::failure(created.error());
        }
        return Result::success(Readback{*this, created.value(), bytes});
    }
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    info.size = bytes;
    auto* const buffer = SDL_CreateGPUTransferBuffer(as_device(handle_), &info);
    if (buffer == nullptr) {
        return Result::failure(RhiError::AllocationFailed);
    }
    return Result::success(Readback{*this, buffer, bytes});
}

core::Result<GraphicsPipeline, RhiError> Device::create_pipeline(
    const ShaderDesc& vertex,
    const ShaderDesc& fragment,
    TextureFormat target_format,
    ColorBlend blend,
    DepthState depth) noexcept {
    using Result = core::Result<GraphicsPipeline, RhiError>;

    if (handle_ == nullptr) {
        return Result::failure(RhiError::Unavailable);
    }
    if (!valid_blend(blend) || vertex.stage != ShaderStage::Vertex
        || fragment.stage != ShaderStage::Fragment || vertex.bytecode.empty()
        || fragment.bytecode.empty()) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    if (is_depth(target_format)) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    if (!supports(vertex.format) || !supports(fragment.format)) {
        return Result::failure(RhiError::ShaderFormatUnsupported);
    }
    if (capabilities_.backend == BackendKind::D3d12) {
        auto created = detail::d3d12_create_pipeline(
            handle_, vertex, fragment, target_format, blend, depth);
        if (!created) {
            return Result::failure(created.error());
        }
        return Result::success(GraphicsPipeline{*this, created.value()});
    }

    auto* const device = as_device(handle_);
    const auto compile = [device](const ShaderDesc& desc) noexcept -> SDL_GPUShader* {
        SDL_GPUShaderCreateInfo info{};
        info.code = desc.bytecode.data();
        info.code_size = desc.bytecode.size();
        info.entrypoint = desc.entry_point.data();
        info.format = as_shader_format(desc.format);
        info.stage = desc.stage == ShaderStage::Vertex
            ? SDL_GPU_SHADERSTAGE_VERTEX
            : SDL_GPU_SHADERSTAGE_FRAGMENT;
        if (desc.sampled_texture_count != 0U
            && desc.sampled_texture_count != desc.sampler_count) return nullptr;
        info.num_samplers = desc.sampler_count;
        info.num_storage_buffers = desc.storage_buffer_count;
        info.num_storage_textures = 0U;
        info.num_uniform_buffers = desc.uniform_buffer_count;
        return SDL_CreateGPUShader(device, &info);
    };

    auto* const vertex_shader = compile(vertex);
    if (vertex_shader == nullptr) {
        return Result::failure(RhiError::BackendFailure);
    }
    auto* const fragment_shader = compile(fragment);
    if (fragment_shader == nullptr) {
        SDL_ReleaseGPUShader(device, vertex_shader);
        return Result::failure(RhiError::BackendFailure);
    }

    SDL_GPUColorTargetDescription target{};
    target.format = as_format(target_format);
    configure_blend(target.blend_state, blend);

    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vertex_shader;
    info.fragment_shader = fragment_shader;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
    info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1U;
    if (depth.enabled) {
        info.target_info.has_depth_stencil_target = true;
        info.target_info.depth_stencil_format =
            as_format(TextureFormat::Depth32Float);
        info.depth_stencil_state.enable_depth_test = true;
        info.depth_stencil_state.enable_depth_write = depth.write;
        info.depth_stencil_state.compare_op = depth.greater
            ? SDL_GPU_COMPAREOP_GREATER
            : SDL_GPU_COMPAREOP_LESS;
    }

    auto* const pipeline = SDL_CreateGPUGraphicsPipeline(device, &info);
    // The shaders are owned by the pipeline once it exists, so they are
    // released either way.
    SDL_ReleaseGPUShader(device, vertex_shader);
    SDL_ReleaseGPUShader(device, fragment_shader);
    if (pipeline == nullptr) {
        return Result::failure(RhiError::BackendFailure);
    }
    return Result::success(GraphicsPipeline{*this, pipeline});
}

core::Result<GraphicsPipeline, RhiError> Device::create_pipeline(
    const ShaderDesc& vertex,
    const ShaderDesc& fragment,
    TextureFormat target_format,
    bool alpha_blend,
    DepthState depth) noexcept {
    return create_pipeline(
        vertex,
        fragment,
        target_format,
        alpha_blend ? ColorBlend::Normal : ColorBlend::Disabled,
        depth);
}

core::Result<ComputePipeline, RhiError> Device::create_compute_pipeline(
    const ShaderDesc& compute) noexcept {
    using Result = core::Result<ComputePipeline, RhiError>;

    if (handle_ == nullptr) {
        return Result::failure(RhiError::Unavailable);
    }
    // Gated on the capability rather than on the backend kind, so the flag and
    // the entry point cannot drift apart: whatever `compute` reports false
    // refuses here, and the test that flips the flag is the same test that
    // proves a dispatch works.
    if (!capabilities_.compute) {
        return Result::failure(RhiError::OperationUnsupported);
    }
    if (compute.stage != ShaderStage::Compute || compute.bytecode.empty()) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    if (!supports(compute.format)) {
        return Result::failure(RhiError::ShaderFormatUnsupported);
    }
    auto created = detail::d3d12_create_compute_pipeline(handle_, compute);
    if (!created) {
        return Result::failure(created.error());
    }
    return Result::success(ComputePipeline{*this, created.value()});
}

core::Result<Swapchain, RhiError> Device::claim_window(
    void* window,
    PresentMode preferred, TextureFormat format) noexcept {
    using Result = core::Result<Swapchain, RhiError>;

    if (handle_ == nullptr) {
        return Result::failure(RhiError::Unavailable);
    }
    if (window == nullptr) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    if (capabilities_.backend == BackendKind::D3d12) {
        auto* const sdl_window = static_cast<SDL_Window*>(window);
        const SDL_PropertiesID properties =
            SDL_GetWindowProperties(sdl_window);
        void* const native_window = SDL_GetPointerProperty(
            properties, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        if (native_window == nullptr) {
            return Result::failure(RhiError::Unavailable);
        }
        auto created = detail::d3d12_create_swapchain(
            handle_, native_window, preferred, format);
        if (!created) {
            return Result::failure(created.error());
        }
        return Result::success(Swapchain{
            *this, window, created.value().state, created.value().mode, format});
    }

    auto* const device = as_device(handle_);
    auto* const sdl_window = static_cast<SDL_Window*>(window);
    // Deeper managed outputs require the native colour-space contract.
    if (format != TextureFormat::Rgba8Unorm) return Result::failure(RhiError::InvalidConfiguration);
    if (!SDL_ClaimWindowForGPUDevice(device, sdl_window)) {
        return Result::failure(RhiError::BackendFailure);
    }

    // A claimed window starts on vsync. Walk down from the requested mode to
    // the first the platform supports and can be set to; vsync is always
    // available, so this terminates.
    const PresentMode ladder[] = {preferred, PresentMode::Mailbox,
                                  PresentMode::Immediate, PresentMode::Vsync};
    auto chosen = PresentMode::Vsync;
    for (const auto candidate : ladder) {
        const auto mode = as_present_mode(candidate);
        if (!SDL_WindowSupportsGPUPresentMode(device, sdl_window, mode)) {
            continue;
        }
        if (SDL_SetGPUSwapchainParameters(
                device, sdl_window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode)) {
            chosen = candidate;
            break;
        }
    }
    return Result::success(Swapchain{*this, window, window, chosen});
}

core::Result<CommandList, RhiError> Device::acquire_commands() noexcept {
    using Result = core::Result<CommandList, RhiError>;

    if (handle_ == nullptr) {
        return Result::failure(RhiError::Unavailable);
    }
    if (capabilities_.backend == BackendKind::D3d12) {
        auto created = detail::d3d12_acquire_commands(handle_);
        if (!created) {
            return Result::failure(created.error());
        }
        return Result::success(CommandList{
            *this,
            created.value().state,
            created.value().native,
            BackendKind::D3d12});
    }
    auto* const commands = SDL_AcquireGPUCommandBuffer(as_device(handle_));
    if (commands == nullptr) {
        return Result::failure(RhiError::BackendFailure);
    }
    return Result::success(
        CommandList{*this, commands, commands, BackendKind::SdlGpu});
}

core::Result<SubmittedWork, RhiError> Device::submit(CommandList commands) noexcept {
    using Result = core::Result<SubmittedWork, RhiError>;

    if (handle_ == nullptr || !commands || commands.device_ != this) {
        return Result::failure(RhiError::Unavailable);
    }
    if (capabilities_.backend == BackendKind::D3d12) {
        auto* const raw = std::exchange(commands.handle_, nullptr);
        commands.native_ = nullptr;
        commands.device_ = nullptr;
        commands.backend_ = BackendKind::SdlGpu;
        const auto submitted_at = std::chrono::steady_clock::now();
        auto submitted = detail::d3d12_submit(handle_, raw);
        if (!submitted) {
            commands.finish_recording(
                detail::d3d12_submission_accepted(raw)
                    ? CommandRecordingOutcome::Submitted
                    : CommandRecordingOutcome::Canceled);
            return Result::failure(submitted.error());
        }
        commands.finish_recording(CommandRecordingOutcome::Submitted);
        return Result::success(
            SubmittedWork{*this, submitted.value(), submitted_at});
    }
    // Take the raw buffer before the list can cancel it in its destructor: a
    // submitted command buffer must not also be cancelled.
    auto* const raw = static_cast<SDL_GPUCommandBuffer*>(
        std::exchange(commands.handle_, nullptr));
    commands.device_ = nullptr;

    const auto submitted_at = std::chrono::steady_clock::now();
    auto* const fence = SDL_SubmitGPUCommandBufferAndAcquireFence(raw);
    if (fence == nullptr) {
        commands.finish_recording(CommandRecordingOutcome::Canceled);
        return Result::failure(RhiError::BackendFailure);
    }
    commands.finish_recording(CommandRecordingOutcome::Submitted);
    return Result::success(SubmittedWork{*this, fence, submitted_at});
}

PresentBarrierSupport Device::present_barrier_support() const noexcept {
    const PresentBarrierBackendHandle backend =
        acquire_present_barrier_backend();
    if (backend == nullptr || !native_handle()) {
        return PresentBarrierSupport::Unsupported;
    }
    return backend->support(native_handle());
}

std::string_view Device::present_barrier_detail() const noexcept {
    const PresentBarrierBackendHandle backend =
        acquire_present_barrier_backend();
    if (backend == nullptr) {
        if (capabilities_.backend == BackendKind::D3d12) {
            return "native D3D12 swapchain presentation is active, but no "
                   "vendor present-barrier backend is installed";
        }
        // Said plainly rather than as a bare "unsupported": an operator who
        // authored FRAMELOCK needs to know the presentation path, not the
        // display hardware, is what cannot join a barrier.
        return "no native present-barrier backend is installed; SDL_GPU "
               "swapchains present independently";
    }
    return backend->detail();
}

VideoReferenceSample Device::video_reference() const noexcept {
    const PresentBarrierBackendHandle backend =
        acquire_present_barrier_backend();
    if (backend == nullptr || !native_handle()) {
        return {};
    }
    return backend->video_reference(native_handle());
}

void Device::wait_idle() noexcept {
    if (handle_ != nullptr) {
        if (capabilities_.backend == BackendKind::D3d12) {
            detail::d3d12_wait_idle(handle_);
        } else {
            SDL_WaitForGPUIdle(as_device(handle_));
        }
    }
}

} // namespace mediaserver::rhi
