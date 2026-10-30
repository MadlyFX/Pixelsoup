#include <mediaserver/render/particle_device.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <utility>

namespace mediaserver::render {
namespace {

ajfx::Error device_error(std::string message) {
    return ajfx::Error{ajfx::ErrorCode::DeviceFailure, std::move(message)};
}

// The renderer draws particles itself; ajfx's reference drawing path is not
// offered here.
ajfx::Error no_drawing() {
    return ajfx::Error{ajfx::ErrorCode::GpuUnavailable,
                       "the media server draws particles with its own render passes"};
}

} // namespace

ParticleDevice::ParticleDevice(rhi::Device& device) noexcept : device_{&device} {}

ParticleDevice::~ParticleDevice() {
    // An open list of our own was never submitted; dropping it cancels it.
    own_.reset();
}

core::Result<std::unique_ptr<ParticleDevice>, rhi::RhiError> ParticleDevice::create(rhi::Device& device) {
    using Result = core::Result<std::unique_ptr<ParticleDevice>, rhi::RhiError>;
    const rhi::DeviceCapabilities& capabilities = device.capabilities();
    if (!capabilities.compute || !capabilities.writable_buffers || !capabilities.indirect_dispatch ||
        !device.supports(rhi::ShaderFormat::Dxil)) {
        return Result::failure(rhi::RhiError::OperationUnsupported);
    }
    auto particles = std::unique_ptr<ParticleDevice>(new ParticleDevice(device));
    for (rhi::StorageBuffer& placeholder : particles->placeholders_) {
        auto created = device.create_writable_buffer(16U);
        if (!created) {
            return Result::failure(created.error());
        }
        placeholder = std::move(created).value();
    }
    return Result::success(std::move(particles));
}

void ParticleDevice::begin_recording(rhi::CommandList& commands) noexcept {
    collect();
    borrowed_ = &commands;
}

bool ParticleDevice::witness_safe(std::uint32_t witness) const noexcept {
    if (witness == 0U) {
        return true;
    }
    if ((witness & placeholder_witness) != 0U) {
        return placeholders_[witness & ~placeholder_witness].safe_to_release();
    }
    if (const auto live = buffers_.find(witness); live != buffers_.end()) {
        return live->second.safe_to_release();
    }
    const auto retired = std::ranges::find(retired_buffers_, witness, &std::pair<std::uint32_t, rhi::StorageBuffer>::first);
    // A witness already freed was safe, so its kernel is too.
    return retired == retired_buffers_.end() || retired->second.safe_to_release();
}

void ParticleDevice::collect() noexcept {
    // Kernels first: their witnesses may be among the buffers freed below.
    std::erase_if(retired_pipelines_, [this](const RetiredPipeline& retired) { return witness_safe(retired.witness); });
    std::erase_if(retired_buffers_, [](const auto& retired) { return retired.second.safe_to_release(); });
}

void ParticleDevice::end_recording() noexcept {
    borrowed_ = nullptr;
}

const rhi::StorageBuffer* ParticleDevice::buffer(ajfx::gpu::Buffer handle) const noexcept {
    const auto found = buffers_.find(handle.id);
    return found == buffers_.end() ? nullptr : &found->second;
}

std::optional<ajfx::Error> ParticleDevice::take_error() noexcept {
    return std::exchange(error_, std::nullopt);
}

void ParticleDevice::note(std::string message) noexcept {
    if (!error_) {
        error_ = device_error(std::move(message));
    }
}

rhi::CommandList* ParticleDevice::commands() noexcept {
    if (borrowed_ != nullptr) {
        return borrowed_;
    }
    if (!own_) {
        auto acquired = device_->acquire_commands();
        if (!acquired) {
            note("no command list could be acquired for particle work");
            return nullptr;
        }
        own_.emplace(std::move(acquired).value());
    }
    return &*own_;
}

ajfx::gpu::ShaderFormat ParticleDevice::shader_format() const noexcept {
    return ajfx::gpu::ShaderFormat::Dxil;
}

std::string_view ParticleDevice::adapter_name() const noexcept {
    return device_->driver_name();
}

ajfx::Result<ajfx::gpu::Buffer> ParticleDevice::create_buffer(std::uint64_t bytes, std::string_view name) {
    collect();
    // The RHI sizes buffers in 32 bits; a larger particle pool is refused
    // rather than truncated.
    if (bytes > std::numeric_limits<core::u32>::max()) {
        return device_error("particle buffer '" + std::string{name} + "' is larger than 4 GiB");
    }
    auto created = device_->create_writable_buffer(static_cast<core::u32>(std::max<std::uint64_t>(bytes, 4U)));
    if (!created) {
        return device_error("creating particle buffer '" + std::string{name} + "' failed");
    }
    const std::uint32_t id = next_id_++;
    buffers_.emplace(id, std::move(created).value());
    return ajfx::gpu::Buffer{id};
}

void ParticleDevice::destroy_buffer(ajfx::gpu::Buffer buffer) {
    const auto found = buffers_.find(buffer.id);
    if (found == buffers_.end()) {
        return;
    }
    retired_buffers_.emplace_back(found->first, std::move(found->second));
    buffers_.erase(found);
    collect();
}

ajfx::Result<ajfx::gpu::Pipeline> ParticleDevice::create_compute_pipeline(std::span<const std::uint8_t> bytecode,
                                                                          std::string_view name) {
    rhi::ShaderDesc desc{};
    desc.stage = rhi::ShaderStage::Compute;
    desc.format = rhi::ShaderFormat::Dxil;
    desc.bytecode = std::span<const core::u8>{bytecode.data(), bytecode.size()};
    desc.writable_buffer_count = ajfx::gpu::max_bound_buffers;
    desc.uniform_buffer_count = 1U;
    auto created = device_->create_compute_pipeline(desc);
    if (!created) {
        return device_error("creating particle kernel '" + std::string{name} + "' failed");
    }
    const std::uint32_t id = next_id_++;
    pipelines_.emplace(id, std::move(created).value());
    return ajfx::gpu::Pipeline{id};
}

ajfx::Result<ajfx::gpu::Pipeline> ParticleDevice::create_graphics_pipeline(const ajfx::gpu::GraphicsPipelineDesc&,
                                                                           std::string_view) {
    return no_drawing();
}

void ParticleDevice::destroy_pipeline(ajfx::gpu::Pipeline pipeline) {
    const auto found = pipelines_.find(pipeline.id);
    if (found == pipelines_.end()) {
        return;
    }
    const auto witness = witnesses_.find(pipeline.id);
    retired_pipelines_.push_back({std::move(found->second), witness != witnesses_.end() ? witness->second : 0U});
    if (witness != witnesses_.end()) {
        witnesses_.erase(witness);
    }
    pipelines_.erase(found);
    collect();
}

ajfx::Result<ajfx::gpu::Texture> ParticleDevice::create_render_target(std::uint32_t, std::uint32_t,
                                                                      ajfx::gpu::TextureFormat, std::string_view) {
    return no_drawing();
}

ajfx::Result<ajfx::gpu::Texture> ParticleDevice::create_texture(const ajfx::TextureData&, std::string_view) {
    return no_drawing();
}

void ParticleDevice::destroy_texture(ajfx::gpu::Texture) {}

void ParticleDevice::write_buffer(ajfx::gpu::Buffer buffer, std::uint64_t offset,
                                  std::span<const std::uint8_t> bytes) {
    const auto found = buffers_.find(buffer.id);
    rhi::CommandList* list = commands();
    if (found == buffers_.end() || list == nullptr || offset > std::numeric_limits<core::u32>::max() ||
        !list->upload_range(found->second, static_cast<core::u32>(offset),
                            std::span<const core::u8>{bytes.data(), bytes.size()})) {
        note("writing a particle buffer failed");
    }
}

void ParticleDevice::copy_buffer(ajfx::gpu::Buffer destination, std::uint64_t destination_offset,
                                 ajfx::gpu::Buffer source, std::uint64_t source_offset, std::uint64_t bytes) {
    const auto to = buffers_.find(destination.id);
    const auto from = buffers_.find(source.id);
    rhi::CommandList* list = commands();
    // The RHI copies whole prefixes; ajfx's checkpoints copy from offset zero.
    if (to == buffers_.end() || from == buffers_.end() || list == nullptr || destination_offset != 0U ||
        source_offset != 0U || bytes > std::numeric_limits<core::u32>::max() ||
        !list->copy_buffer(from->second, to->second, static_cast<core::u32>(bytes))) {
        note("copying a particle buffer failed");
    }
}

void ParticleDevice::dispatch(const ajfx::gpu::Dispatch& d) {
    const auto pipeline = pipelines_.find(d.pipeline.id);
    rhi::CommandList* list = commands();
    if (pipeline == pipelines_.end() || list == nullptr) {
        note("dispatch of an unknown particle kernel");
        return;
    }
    std::array<const rhi::StorageBuffer*, ajfx::gpu::max_bound_buffers> writable{};
    for (std::size_t slot = 0U; slot < writable.size(); ++slot) {
        const rhi::StorageBuffer* bound = d.buffers[slot].valid() ? buffer(d.buffers[slot]) : &placeholders_[slot];
        if (bound == nullptr) {
            note("dispatch binds an unknown particle buffer");
            return;
        }
        writable[slot] = bound;
    }
    rhi::ComputeDispatch work{};
    work.pipeline = &pipeline->second;
    work.writable_buffers = writable;
    work.uniform = std::span<const core::u8>{reinterpret_cast<const core::u8*>(d.constants.data()),
                                             d.constants.size() * sizeof(std::uint32_t)};
    if (d.indirect.valid()) {
        const rhi::StorageBuffer* arguments = buffer(d.indirect);
        if (arguments == nullptr || d.indirect_offset > std::numeric_limits<core::u32>::max()) {
            note("dispatch reads unknown indirect arguments");
            return;
        }
        work.indirect = rhi::IndirectArguments{arguments, static_cast<core::u32>(d.indirect_offset)};
    } else {
        work.groups_x = d.groups_x;
        work.groups_y = d.groups_y;
    }
    if (!list->dispatch(work)) {
        note("a particle dispatch was refused");
        return;
    }
    witnesses_[d.pipeline.id] = d.buffers[0].valid() ? d.buffers[0].id : (placeholder_witness | 0U);
}

void ParticleDevice::begin_rendering(ajfx::gpu::Texture, const std::array<float, 4>*) {
    note(no_drawing().message);
}

void ParticleDevice::draw(const ajfx::gpu::Draw&) {
    note(no_drawing().message);
}

void ParticleDevice::end_rendering() {}

void ParticleDevice::barrier() {
    // The RHI places a UAV barrier between dispatches that share a writable
    // buffer, and a transition before a buffer is read any other way.
}

ajfx::Result<bool> ParticleDevice::flush() {
    if (borrowed_ != nullptr) {
        return device_error("particle work is being recorded into the renderer's list, which the renderer submits");
    }
    if (own_) {
        auto submitted = device_->submit(std::move(*own_));
        own_.reset();
        if (!submitted || !std::move(submitted).value().wait()) {
            note("submitting particle work failed");
        }
    }
    if (std::optional<ajfx::Error> error = take_error()) {
        return *std::move(error);
    }
    return true;
}

ajfx::Result<std::vector<std::uint8_t>> ParticleDevice::read_buffer(ajfx::gpu::Buffer handle, std::uint64_t offset,
                                                                   std::uint64_t bytes) {
    if (bytes == 0U) {
        return std::vector<std::uint8_t>{};
    }
    const rhi::StorageBuffer* source = buffer(handle);
    if (source == nullptr || borrowed_ != nullptr || offset > std::numeric_limits<core::u32>::max() ||
        bytes > std::numeric_limits<core::u32>::max()) {
        return device_error("read_buffer needs a known buffer and no borrowed command list");
    }
    auto readback = device_->create_readback(static_cast<core::u32>(bytes));
    rhi::CommandList* list = commands();
    if (!readback || list == nullptr ||
        !list->download(*source, static_cast<core::u32>(offset), static_cast<core::u32>(bytes), readback.value())) {
        return device_error("reading a particle buffer back failed");
    }
    ajfx::Result<bool> flushed = flush();
    if (!flushed) {
        return flushed.error();
    }
    const std::span<const core::u8> mapped = readback.value().map();
    if (mapped.size() < bytes) {
        readback.value().unmap();
        return device_error("mapping a particle readback failed");
    }
    std::vector<std::uint8_t> out(mapped.begin(), mapped.begin() + static_cast<std::ptrdiff_t>(bytes));
    readback.value().unmap();
    return out;
}

ajfx::Result<std::vector<std::uint8_t>> ParticleDevice::read_texture(ajfx::gpu::Texture) {
    return no_drawing();
}

} // namespace mediaserver::render
