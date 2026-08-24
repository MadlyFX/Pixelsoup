#include <mediaserver/rhi/device.hpp>

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <utility>

namespace mediaserver::rhi {
namespace {

[[nodiscard]] SDL_GPUDevice* as_device(void* handle) noexcept {
    return static_cast<SDL_GPUDevice*>(handle);
}

[[nodiscard]] SDL_GPUTextureFormat as_format(TextureFormat format) noexcept {
    switch (format) {
    case TextureFormat::Bgra8Unorm:
        return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    case TextureFormat::Depth32Float:
        return SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    case TextureFormat::Rgba8Unorm:
        break;
    }
    return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
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

// ----------------------------------------------------------------- Texture

Texture::Texture(Texture&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      handle_(std::exchange(other.handle_, nullptr)),
      desc_(other.desc_) {}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
        desc_ = other.desc_;
    }
    return *this;
}

Texture::~Texture() {
    release();
}

void Texture::release() noexcept {
    if (device_ != nullptr && handle_ != nullptr) {
        SDL_ReleaseGPUTexture(
            as_device(device_->backend_handle()),
            static_cast<SDL_GPUTexture*>(handle_));
    }
    device_ = nullptr;
    handle_ = nullptr;
}

// ----------------------------------------------------------- StorageBuffer

StorageBuffer::StorageBuffer(StorageBuffer&& other) noexcept
    : device_(std::exchange(other.device_, nullptr)),
      handle_(std::exchange(other.handle_, nullptr)),
      staging_(std::exchange(other.staging_, nullptr)),
      bytes_(std::exchange(other.bytes_, 0U)) {}

StorageBuffer& StorageBuffer::operator=(StorageBuffer&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
        staging_ = std::exchange(other.staging_, nullptr);
        bytes_ = std::exchange(other.bytes_, 0U);
    }
    return *this;
}

StorageBuffer::~StorageBuffer() {
    release();
}

void StorageBuffer::release() noexcept {
    if (device_ != nullptr) {
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
    staging_ = nullptr;
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
        SDL_ReleaseGPUGraphicsPipeline(
            as_device(device_->backend_handle()),
            static_cast<SDL_GPUGraphicsPipeline*>(handle_));
    }
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
    SDL_UnmapGPUTransferBuffer(
        as_device(device_->backend_handle()),
        static_cast<SDL_GPUTransferBuffer*>(handle_));
    mapped_ = false;
}

void Readback::release() noexcept {
    unmap();
    if (device_ != nullptr && handle_ != nullptr) {
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
      elapsed_(other.elapsed_) {}

SubmittedWork& SubmittedWork::operator=(SubmittedWork&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
        submitted_at_ = other.submitted_at_;
        elapsed_ = other.elapsed_;
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
    auto* fence = static_cast<SDL_GPUFence*>(handle_);
    const auto waited =
        SDL_WaitForGPUFences(as_device(device_->backend_handle()), true, &fence, 1U);
    elapsed_ = std::chrono::steady_clock::now() - submitted_at_;
    return waited;
}

void SubmittedWork::release() noexcept {
    if (device_ != nullptr && handle_ != nullptr) {
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
      handle_(std::exchange(other.handle_, nullptr)) {}

CommandList& CommandList::operator=(CommandList&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, nullptr);
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

CommandList::~CommandList() {
    release();
}

void CommandList::release() noexcept {
    if (device_ != nullptr && handle_ != nullptr) {
        // Cancelling is the only correct disposal for a list nobody submitted:
        // the backend otherwise holds the buffer for the life of the device.
        static_cast<void>(SDL_CancelGPUCommandBuffer(
            static_cast<SDL_GPUCommandBuffer*>(handle_)));
    }
    device_ = nullptr;
    handle_ = nullptr;
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

    auto* const device = as_device(device_->backend_handle());
    auto* const staging = static_cast<SDL_GPUTransferBuffer*>(buffer.staging_);
    auto* const mapped = SDL_MapGPUTransferBuffer(device, staging, false);
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
    SDL_UploadToGPUBuffer(pass, &source, &destination, false);
    SDL_EndGPUCopyPass(pass);
    return true;
}

bool CommandList::upload(
    Texture& texture,
    std::span<const core::u8> rgba) noexcept {
    if (device_ == nullptr || handle_ == nullptr || !texture) {
        return false;
    }
    const auto required = static_cast<std::size_t>(texture.desc_.width)
        * texture.desc_.height * 4U;
    if (rgba.size() != required || required == 0U) {
        return false;
    }

    auto* const device = as_device(device_->backend_handle());
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    info.size = static_cast<Uint32>(required);
    auto* const staging = SDL_CreateGPUTransferBuffer(device, &info);
    if (staging == nullptr) {
        return false;
    }

    auto* const mapped = SDL_MapGPUTransferBuffer(device, staging, false);
    if (mapped == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device, staging);
        return false;
    }
    std::copy(rgba.begin(), rgba.end(), static_cast<core::u8*>(mapped));
    SDL_UnmapGPUTransferBuffer(device, staging);

    auto* const pass =
        SDL_BeginGPUCopyPass(static_cast<SDL_GPUCommandBuffer*>(handle_));
    if (pass == nullptr) {
        SDL_ReleaseGPUTransferBuffer(device, staging);
        return false;
    }
    SDL_GPUTextureTransferInfo source{};
    source.transfer_buffer = staging;
    source.pixels_per_row = texture.desc_.width;
    source.rows_per_layer = texture.desc_.height;
    SDL_GPUTextureRegion region{};
    region.texture = static_cast<SDL_GPUTexture*>(texture.handle_);
    region.w = texture.desc_.width;
    region.h = texture.desc_.height;
    region.d = 1U;
    SDL_UploadToGPUTexture(pass, &source, &region, false);
    SDL_EndGPUCopyPass(pass);

    // Releasing a transfer buffer is deferred by the backend until the work
    // referencing it retires, so this is safe before submission.
    SDL_ReleaseGPUTransferBuffer(device, staging);
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
    if (!depth.desc().depth_target
        || depth.width() != color.width() || depth.height() != color.height()) {
        // A depth buffer of a different size than its colour target is a
        // resize that half happened. Refusing beats rendering a frame that is
        // depth-tested against the wrong pixels.
        return false;
    }
    // A stride of zero with batches present would bind the same record to
    // every one of them, which is never what a caller means.
    const auto stride = static_cast<std::size_t>(draw.batch_uniform_stride);
    if (!draw.batches.empty()
        && (stride == 0U || draw.batch_uniforms.size() < draw.batches.size() * stride)) {
        return false;
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
        if (draw.texture != nullptr && draw.sampler != nullptr
            && *draw.texture && *draw.sampler) {
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
            SDL_PushGPUVertexUniformData(
                commands,
                1U,
                draw.batch_uniforms.data() + index * stride,
                draw.batch_uniform_stride);
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
    const Texture& source,
    const Sampler& sampler,
    std::span<const core::u8> fragment_uniform) noexcept {
    if (device_ == nullptr || handle_ == nullptr || !target || !pipeline
        || !source || !sampler) {
        return false;
    }

    auto* const commands = static_cast<SDL_GPUCommandBuffer*>(handle_);

    SDL_GPUColorTargetInfo colour{};
    colour.texture = static_cast<SDL_GPUTexture*>(target.handle_);
    colour.clear_color = SDL_FColor{clear.r, clear.g, clear.b, clear.a};
    colour.load_op = SDL_GPU_LOADOP_CLEAR;
    colour.store_op = SDL_GPU_STOREOP_STORE;

    auto* const pass = SDL_BeginGPURenderPass(commands, &colour, 1U, nullptr);
    if (pass == nullptr) {
        return false;
    }

    SDL_BindGPUGraphicsPipeline(
        pass, static_cast<SDL_GPUGraphicsPipeline*>(pipeline.handle_));
    const SDL_GPUTextureSamplerBinding binding{
        static_cast<SDL_GPUTexture*>(source.handle_),
        static_cast<SDL_GPUSampler*>(sampler.handle_),
    };
    SDL_BindGPUFragmentSamplers(pass, 0U, &binding, 1U);
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

bool CommandList::clear(Texture& target, Color colour) noexcept {
    if (device_ == nullptr || handle_ == nullptr || !target) {
        return false;
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

bool CommandList::blit(
    const Texture& source,
    Texture& destination,
    Filter filter) noexcept {
    if (device_ == nullptr || handle_ == nullptr || !source || !destination) {
        return false;
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

bool CommandList::download(const Texture& source, Readback& into) noexcept {
    if (device_ == nullptr || handle_ == nullptr || !source || !into) {
        return false;
    }
    const auto required =
        static_cast<std::size_t>(source.desc_.width) * source.desc_.height * 4U;
    if (required > static_cast<std::size_t>(into.bytes_)) {
        return false;
    }

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
      mode_(other.mode_) {}

Swapchain& Swapchain::operator=(Swapchain&& other) noexcept {
    if (this != &other) {
        release();
        device_ = std::exchange(other.device_, nullptr);
        window_ = std::exchange(other.window_, nullptr);
        mode_ = other.mode_;
    }
    return *this;
}

Swapchain::~Swapchain() {
    release();
}

void Swapchain::release() noexcept {
    if (device_ != nullptr && window_ != nullptr) {
        SDL_ReleaseWindowFromGPUDevice(
            as_device(device_->backend_handle()),
            static_cast<SDL_Window*>(window_));
    }
    device_ = nullptr;
    window_ = nullptr;
}

SwapchainImage Swapchain::acquire(CommandList& commands) noexcept {
    if (device_ == nullptr || window_ == nullptr || !commands) {
        return {};
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
    return SwapchainImage{texture, width, height};
}

bool Swapchain::present(
    CommandList& commands,
    const SwapchainImage& image,
    const Texture& source,
    Filter filter) noexcept {
    if (device_ == nullptr || !commands || !image || !source) {
        return false;
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
    if (!ensure_video()) {
        return false;
    }
    return SDL_GPUSupportsShaderFormats(all_shader_formats, nullptr);
}

core::Result<Device, RhiError> Device::create(const DeviceConfig& config) noexcept {
    using Result = core::Result<Device, RhiError>;

    if (config.frames_in_flight == 0U || config.frames_in_flight > 3U) {
        return Result::failure(RhiError::InvalidConfiguration);
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
    return Result::success(Device{device});
}

Device::Device(Device&& other) noexcept
    : handle_(std::exchange(other.handle_, nullptr)) {}

Device& Device::operator=(Device&& other) noexcept {
    if (this != &other) {
        if (handle_ != nullptr) {
            SDL_DestroyGPUDevice(as_device(handle_));
        }
        handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
}

Device::~Device() {
    if (handle_ != nullptr) {
        SDL_DestroyGPUDevice(as_device(handle_));
        handle_ = nullptr;
    }
}

std::string_view Device::driver_name() const noexcept {
    if (handle_ == nullptr) {
        return {};
    }
    const auto* const name = SDL_GetGPUDeviceDriver(as_device(handle_));
    return name == nullptr ? std::string_view{} : std::string_view{name};
}

bool Device::supports(ShaderFormat format) const noexcept {
    if (handle_ == nullptr) {
        return false;
    }
    return (SDL_GetGPUShaderFormats(as_device(handle_)) & as_shader_format(format))
        != 0U;
}

core::Result<Texture, RhiError> Device::create_texture(
    const TextureDesc& desc) noexcept {
    using Result = core::Result<Texture, RhiError>;

    if (handle_ == nullptr) {
        return Result::failure(RhiError::Unavailable);
    }
    if (desc.width == 0U || desc.height == 0U
        || (!desc.color_target && !desc.sampled && !desc.depth_target)) {
        return Result::failure(RhiError::InvalidConfiguration);
    }
    // A depth format is a depth attachment and nothing else, and a colour
    // format cannot be one. Both mismatches are configuration errors here
    // rather than validation-layer noise later.
    if (desc.depth_target != is_depth(desc.format)
        || (desc.depth_target && (desc.color_target || desc.sampled))) {
        return Result::failure(RhiError::InvalidConfiguration);
    }

    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = as_format(desc.format);
    info.usage = (desc.color_target ? SDL_GPU_TEXTUREUSAGE_COLOR_TARGET : 0U)
        | (desc.sampled ? SDL_GPU_TEXTUREUSAGE_SAMPLER : 0U)
        | (desc.depth_target ? SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET : 0U);
    info.width = desc.width;
    info.height = desc.height;
    info.layer_count_or_depth = 1U;
    info.num_levels = 1U;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;

    auto* const texture = SDL_CreateGPUTexture(as_device(handle_), &info);
    if (texture == nullptr) {
        return Result::failure(RhiError::AllocationFailed);
    }
    return Result::success(Texture{*this, texture, desc});
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
    return Result::success(StorageBuffer{*this, buffer, staging, bytes});
}

core::Result<Sampler, RhiError> Device::create_sampler(Filter filter) noexcept {
    using Result = core::Result<Sampler, RhiError>;

    if (handle_ == nullptr) {
        return Result::failure(RhiError::Unavailable);
    }
    SDL_GPUSamplerCreateInfo info{};
    info.min_filter = as_filter(filter);
    info.mag_filter = as_filter(filter);
    info.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    info.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    info.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;

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
    bool alpha_blend,
    DepthState depth) noexcept {
    using Result = core::Result<GraphicsPipeline, RhiError>;

    if (handle_ == nullptr) {
        return Result::failure(RhiError::Unavailable);
    }
    if (vertex.stage != ShaderStage::Vertex
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

    auto* const device = as_device(handle_);
    const auto compile = [device](const ShaderDesc& desc) noexcept {
        SDL_GPUShaderCreateInfo info{};
        info.code = desc.bytecode.data();
        info.code_size = desc.bytecode.size();
        info.entrypoint = desc.entry_point.data();
        info.format = as_shader_format(desc.format);
        info.stage = desc.stage == ShaderStage::Vertex
            ? SDL_GPU_SHADERSTAGE_VERTEX
            : SDL_GPU_SHADERSTAGE_FRAGMENT;
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
    if (alpha_blend) {
        target.blend_state.enable_blend = true;
        target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        target.blend_state.dst_color_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        target.blend_state.dst_alpha_blendfactor =
            SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }

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

core::Result<Swapchain, RhiError> Device::claim_window(
    void* window,
    PresentMode preferred) noexcept {
    using Result = core::Result<Swapchain, RhiError>;

    if (handle_ == nullptr) {
        return Result::failure(RhiError::Unavailable);
    }
    if (window == nullptr) {
        return Result::failure(RhiError::InvalidConfiguration);
    }

    auto* const device = as_device(handle_);
    auto* const sdl_window = static_cast<SDL_Window*>(window);
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
    return Result::success(Swapchain{*this, window, chosen});
}

core::Result<CommandList, RhiError> Device::acquire_commands() noexcept {
    using Result = core::Result<CommandList, RhiError>;

    if (handle_ == nullptr) {
        return Result::failure(RhiError::Unavailable);
    }
    auto* const commands = SDL_AcquireGPUCommandBuffer(as_device(handle_));
    if (commands == nullptr) {
        return Result::failure(RhiError::BackendFailure);
    }
    return Result::success(CommandList{*this, commands});
}

core::Result<SubmittedWork, RhiError> Device::submit(CommandList commands) noexcept {
    using Result = core::Result<SubmittedWork, RhiError>;

    if (handle_ == nullptr || !commands) {
        return Result::failure(RhiError::Unavailable);
    }
    // Take the raw buffer before the list can cancel it in its destructor: a
    // submitted command buffer must not also be cancelled.
    auto* const raw = static_cast<SDL_GPUCommandBuffer*>(
        std::exchange(commands.handle_, nullptr));
    commands.device_ = nullptr;

    const auto submitted_at = std::chrono::steady_clock::now();
    auto* const fence = SDL_SubmitGPUCommandBufferAndAcquireFence(raw);
    if (fence == nullptr) {
        return Result::failure(RhiError::BackendFailure);
    }
    return Result::success(SubmittedWork{*this, fence, submitted_at});
}

void Device::wait_idle() noexcept {
    if (handle_ != nullptr) {
        SDL_WaitForGPUIdle(as_device(handle_));
    }
}

} // namespace mediaserver::rhi
