#include <ajfx/d3d12.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ajfx::d3d12 {
namespace {

using Microsoft::WRL::ComPtr;

std::string hresult(HRESULT hr) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08lX", static_cast<unsigned long>(hr));
    return text;
}

Error failure(ErrorCode code, const std::string& what, HRESULT hr) {
    return Error{code, what + " failed (" + hresult(hr) + ")"};
}

std::string narrow(const wchar_t* text) {
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring widen(std::string_view text) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}

D3D12_RESOURCE_DESC buffer_desc(std::uint64_t bytes, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<std::uint64_t>((bytes + 3U) & ~std::uint64_t{3U}, 4U);
    desc.Height = 1U;
    desc.DepthOrArraySize = 1U;
    desc.MipLevels = 1U;
    desc.SampleDesc.Count = 1U;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    return desc;
}

DXGI_FORMAT dxgi_format(gpu::TextureFormat format) {
    return format == gpu::TextureFormat::Rgba16Float ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
}

std::size_t bytes_per_pixel(gpu::TextureFormat format) {
    return format == gpu::TextureFormat::Rgba16Float ? 8U : 4U;
}

constexpr std::uint64_t upload_page_bytes = 1U << 20U;
constexpr std::uint32_t max_render_targets = 256U;

class DeviceImpl final : public Device {
public:
    static Result<std::unique_ptr<Device>> create(const DeviceOptions& options) {
        auto device = std::unique_ptr<DeviceImpl>(new DeviceImpl());
        if (std::optional<Error> error = device->initialise(options)) {
            return *std::move(error);
        }
        return std::unique_ptr<Device>{std::move(device)};
    }

    ~DeviceImpl() override {
        if (queue_ && fence_) {
            (void)wait_idle();
        }
        swap_chains_.clear();
        if (event_ != nullptr) {
            CloseHandle(event_);
        }
    }

    [[nodiscard]] gpu::ShaderFormat shader_format() const noexcept override { return gpu::ShaderFormat::Dxil; }
    [[nodiscard]] std::string_view adapter_name() const noexcept override { return adapter_name_; }

    // ------------------------------------------------------------ buffers

    Result<gpu::Buffer> create_buffer(std::uint64_t bytes, std::string_view name) override {
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC desc = buffer_desc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        Record record;
        // Committed resources on an implicit heap are zero-filled.
        const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                            IID_PPV_ARGS(&record.resource));
        if (FAILED(hr)) {
            return failure(ErrorCode::DeviceFailure, "creating a " + std::to_string(bytes) + "-byte buffer", hr);
        }
        record.resource->SetName(widen(name).c_str());
        const std::uint32_t id = next_id_++;
        buffers_.emplace(id, std::move(record));
        return gpu::Buffer{id};
    }

    void destroy_buffer(gpu::Buffer buffer) override {
        const auto found = buffers_.find(buffer.id);
        if (found == buffers_.end()) {
            return;
        }
        // Recorded work may still use it; keep it alive until the next flush.
        retired_.push_back(std::move(found->second.resource));
        buffers_.erase(found);
    }

    void write_buffer(gpu::Buffer buffer, std::uint64_t offset, std::span<const std::uint8_t> bytes) override {
        Record* record = find(buffer);
        if (record == nullptr || bytes.empty()) {
            return;
        }
        const std::optional<std::pair<ID3D12Resource*, std::uint64_t>> staging = stage(bytes);
        if (!staging) {
            return;
        }
        transition(*record, D3D12_RESOURCE_STATE_COPY_DEST);
        list_->CopyBufferRegion(record->resource.Get(), offset, staging->first, staging->second, bytes.size());
    }

    void copy_buffer(gpu::Buffer destination, std::uint64_t destination_offset, gpu::Buffer source,
                     std::uint64_t source_offset, std::uint64_t bytes) override {
        Record* to = find(destination);
        Record* from = find(source);
        if (to == nullptr || from == nullptr || to == from || bytes == 0U) {
            note(Error{ErrorCode::DeviceFailure, "copy_buffer needs two different, valid buffers"});
            return;
        }
        transition(*from, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(*to, D3D12_RESOURCE_STATE_COPY_DEST);
        list_->CopyBufferRegion(to->resource.Get(), destination_offset, from->resource.Get(), source_offset, bytes);
    }

    Result<std::vector<std::uint8_t>> read_buffer(gpu::Buffer buffer, std::uint64_t offset,
                                                  std::uint64_t bytes) override {
        Record* record = find(buffer);
        if (record == nullptr) {
            return Error{ErrorCode::DeviceFailure, "read_buffer of an unknown buffer"};
        }
        if (bytes == 0U) {
            return std::vector<std::uint8_t>{};
        }
        Result<ComPtr<ID3D12Resource>> readback = create_readback(bytes);
        if (!readback) {
            return readback.error();
        }
        transition(*record, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list_->CopyBufferRegion(readback.value().Get(), 0U, record->resource.Get(), offset, bytes);
        return finish_readback(readback.value().Get(), bytes, [bytes](const std::uint8_t* mapped) {
            return std::vector<std::uint8_t>(mapped, mapped + bytes);
        });
    }

    // ------------------------------------------------------------ pipelines

    Result<gpu::Pipeline> create_compute_pipeline(std::span<const std::uint8_t> bytecode,
                                                  std::string_view name) override {
        D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root_signature_.Get();
        desc.CS = {bytecode.data(), bytecode.size()};
        ComPtr<ID3D12PipelineState> pipeline;
        const HRESULT hr = device_->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(hr)) {
            return failure(ErrorCode::DeviceFailure, "creating pipeline '" + std::string{name} + "'", hr);
        }
        return add_pipeline(std::move(pipeline), name);
    }

    Result<gpu::Pipeline> create_graphics_pipeline(const gpu::GraphicsPipelineDesc& g, std::string_view name) override {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root_signature_.Get();
        desc.VS = {g.vertex.data(), g.vertex.size()};
        desc.PS = {g.pixel.data(), g.pixel.size()};
        // Pixel shaders output premultiplied colour.
        D3D12_RENDER_TARGET_BLEND_DESC& blend = desc.BlendState.RenderTarget[0];
        blend.BlendEnable = g.blend != gpu::Blend::Opaque;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = g.blend == gpu::Blend::Additive ? D3D12_BLEND_ONE : D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = blend.DestBlend;
        blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.LogicOp = D3D12_LOGIC_OP_NOOP;
        blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1U;
        desc.RTVFormats[0] = dxgi_format(g.format);
        desc.SampleDesc.Count = 1U;
        ComPtr<ID3D12PipelineState> pipeline;
        const HRESULT hr = device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
        if (FAILED(hr)) {
            return failure(ErrorCode::DeviceFailure, "creating pipeline '" + std::string{name} + "'", hr);
        }
        return add_pipeline(std::move(pipeline), name);
    }

    void destroy_pipeline(gpu::Pipeline pipeline) override {
        const auto found = pipelines_.find(pipeline.id);
        if (found == pipelines_.end()) {
            return;
        }
        retired_pipelines_.push_back(std::move(found->second));
        pipelines_.erase(found);
    }

    // ------------------------------------------------------------ textures

    Result<gpu::Texture> create_render_target(std::uint32_t width, std::uint32_t height, gpu::TextureFormat format,
                                              std::string_view name) override {
        if (width == 0U || height == 0U || width > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            height > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            return Error{ErrorCode::DeviceFailure, "render target size " + std::to_string(width) + "x" +
                                                       std::to_string(height) + " is not supported"};
        }
        if (free_rtv_slots_.empty()) {
            return Error{ErrorCode::DeviceFailure, "too many render targets"};
        }
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1U;
        desc.MipLevels = 1U;
        desc.Format = dxgi_format(format);
        desc.SampleDesc.Count = 1U;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        TextureRecord record;
        const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                            IID_PPV_ARGS(&record.resource));
        if (FAILED(hr)) {
            return failure(ErrorCode::DeviceFailure, "creating a render target", hr);
        }
        record.resource->SetName(widen(name).c_str());
        record.rtv_slot = free_rtv_slots_.back();
        free_rtv_slots_.pop_back();
        record.rtv = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        record.rtv.ptr += static_cast<SIZE_T>(record.rtv_slot) * rtv_size_;
        device_->CreateRenderTargetView(record.resource.Get(), nullptr, record.rtv);
        record.width = width;
        record.height = height;
        record.format = format;
        const std::uint32_t id = next_id_++;
        textures_.emplace(id, std::move(record));
        return gpu::Texture{id};
    }

    void destroy_texture(gpu::Texture texture) override {
        const auto found = textures_.find(texture.id);
        if (found == textures_.end()) {
            return;
        }
        retired_.push_back(std::move(found->second.resource));
        retired_rtv_slots_.push_back(found->second.rtv_slot);
        textures_.erase(found);
    }

    Result<std::vector<std::uint8_t>> read_texture(gpu::Texture texture) override {
        const auto found = textures_.find(texture.id);
        if (found == textures_.end()) {
            return Error{ErrorCode::DeviceFailure, "read_texture of an unknown texture"};
        }
        TextureRecord& record = found->second;
        const D3D12_RESOURCE_DESC desc = record.resource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 total = 0U;
        device_->GetCopyableFootprints(&desc, 0U, 1U, 0U, &footprint, nullptr, nullptr, &total);
        Result<ComPtr<ID3D12Resource>> readback = create_readback(total);
        if (!readback) {
            return readback.error();
        }
        transition(record, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = readback.value().Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.pResource = record.resource.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list_->CopyTextureRegion(&to, 0U, 0U, 0U, &from, nullptr);
        const std::size_t row = static_cast<std::size_t>(record.width) * bytes_per_pixel(record.format);
        const std::uint32_t height = record.height;
        return finish_readback(readback.value().Get(), total, [&](const std::uint8_t* mapped) {
            std::vector<std::uint8_t> out(row * height);
            for (std::uint32_t y = 0U; y < height; ++y) {
                std::memcpy(out.data() + y * row,
                            mapped + footprint.Offset + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch,
                            row);
            }
            return out;
        });
    }

    // ------------------------------------------------------------ recording

    void dispatch(const gpu::Dispatch& d) override {
        const auto pipeline = pipelines_.find(d.pipeline.id);
        if (pipeline == pipelines_.end()) {
            note(Error{ErrorCode::DeviceFailure, "dispatch of an unknown pipeline"});
            return;
        }
        if (rendering_ != nullptr) {
            note(Error{ErrorCode::DeviceFailure, "dispatch inside begin_rendering/end_rendering"});
            return;
        }
        Record* indirect = d.indirect.valid() ? find(d.indirect) : nullptr;
        if (d.indirect.valid() && indirect == nullptr) {
            note(Error{ErrorCode::DeviceFailure, "dispatch with an unknown indirect buffer"});
            return;
        }
        std::array<D3D12_GPU_VIRTUAL_ADDRESS, gpu::max_bound_buffers> addresses{};
        if (!prepare_buffers(d.buffers, indirect, addresses)) {
            return;
        }
        list_->SetComputeRootSignature(root_signature_.Get());
        list_->SetPipelineState(pipeline->second.Get());
        list_->SetComputeRoot32BitConstants(0U, gpu::root_constant_count, d.constants.data(), 0U);
        for (std::uint32_t slot = 0U; slot < gpu::max_bound_buffers; ++slot) {
            list_->SetComputeRootUnorderedAccessView(1U + slot, addresses[slot]);
        }
        if (indirect != nullptr) {
            transition(*indirect, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            list_->ExecuteIndirect(dispatch_signature_.Get(), 1U, indirect->resource.Get(), d.indirect_offset,
                                   nullptr, 0U);
        } else {
            list_->Dispatch(d.groups_x, d.groups_y, 1U);
        }
    }

    void begin_rendering(gpu::Texture target, const std::array<float, 4>* clear_color) override {
        const auto found = textures_.find(target.id);
        if (found == textures_.end() || rendering_ != nullptr) {
            note(Error{ErrorCode::DeviceFailure, "begin_rendering needs a valid target and no open rendering"});
            return;
        }
        TextureRecord& record = found->second;
        transition(record, D3D12_RESOURCE_STATE_RENDER_TARGET);
        list_->OMSetRenderTargets(1U, &record.rtv, FALSE, nullptr);
        const D3D12_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(record.width), static_cast<float>(record.height),
                                      0.0F, 1.0F};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(record.width), static_cast<LONG>(record.height)};
        list_->RSSetViewports(1U, &viewport);
        list_->RSSetScissorRects(1U, &scissor);
        if (clear_color != nullptr) {
            list_->ClearRenderTargetView(record.rtv, clear_color->data(), 0U, nullptr);
        }
        rendering_ = &record;
    }

    void draw(const gpu::Draw& d) override {
        const auto pipeline = pipelines_.find(d.pipeline.id);
        if (rendering_ == nullptr || pipeline == pipelines_.end()) {
            note(Error{ErrorCode::DeviceFailure, "draw needs a known pipeline inside begin_rendering"});
            return;
        }
        Record* indirect = d.indirect.valid() ? find(d.indirect) : nullptr;
        if (d.indirect.valid() && indirect == nullptr) {
            note(Error{ErrorCode::DeviceFailure, "draw with an unknown indirect buffer"});
            return;
        }
        std::array<D3D12_GPU_VIRTUAL_ADDRESS, gpu::max_bound_buffers> addresses{};
        if (!prepare_buffers(d.buffers, indirect, addresses)) {
            return;
        }
        list_->SetGraphicsRootSignature(root_signature_.Get());
        list_->SetPipelineState(pipeline->second.Get());
        list_->SetGraphicsRoot32BitConstants(0U, gpu::root_constant_count, d.constants.data(), 0U);
        for (std::uint32_t slot = 0U; slot < gpu::max_bound_buffers; ++slot) {
            list_->SetGraphicsRootUnorderedAccessView(1U + slot, addresses[slot]);
        }
        list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if (indirect != nullptr) {
            transition(*indirect, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            list_->ExecuteIndirect(draw_signature_.Get(), 1U, indirect->resource.Get(), d.indirect_offset, nullptr,
                                   0U);
        } else if (d.vertex_count > 0U && d.instance_count > 0U) {
            list_->DrawInstanced(d.vertex_count, d.instance_count, 0U, 0U);
        }
    }

    void end_rendering() override {
        rendering_ = nullptr;
    }

    void barrier() override {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = nullptr;
        list_->ResourceBarrier(1U, &barrier);
    }

    // ------------------------------------------------------------ submission

    Result<bool> flush() override {
        rendering_ = nullptr;
        HRESULT hr = list_->Close();
        if (SUCCEEDED(hr)) {
            ID3D12CommandList* lists[] = {list_.Get()};
            queue_->ExecuteCommandLists(1U, lists);
        }
        // The recorded transitions happened only if the list ran.
        const auto settle = [hr](Record& record) {
            if (SUCCEEDED(hr)) {
                record.executed = record.state;
            } else {
                record.state = record.executed;
            }
        };
        for (auto& [id, record] : buffers_) {
            settle(record);
        }
        for (auto& [id, record] : textures_) {
            settle(record);
        }
        for (auto& [window, swap] : swap_chains_) {
            for (Record& buffer : swap.buffers) {
                settle(buffer);
            }
        }
        const Result<bool> waited = wait_idle();
        HRESULT reset = allocator_->Reset();
        if (SUCCEEDED(reset)) {
            reset = list_->Reset(allocator_.Get(), nullptr);
        }
        if (FAILED(reset)) {
            // A list that failed to close cannot be reset; start a fresh one
            // so the device stays usable after reporting the failure.
            reset = recreate_command_list();
        }
        uploads_.clear();
        upload_used_ = upload_capacity_;
        retired_.clear();
        retired_pipelines_.clear();
        free_rtv_slots_.insert(free_rtv_slots_.end(), retired_rtv_slots_.begin(), retired_rtv_slots_.end());
        retired_rtv_slots_.clear();

        if (std::optional<Error> error = std::exchange(deferred_, std::nullopt)) {
            return *std::move(error);
        }
        if (std::optional<Error> error = debug_layer_errors()) {
            return *std::move(error);
        }
        if (FAILED(hr)) {
            return failure(ErrorCode::DeviceFailure, "closing the command list", hr);
        }
        if (!waited) {
            return waited.error();
        }
        if (FAILED(reset)) {
            return failure(ErrorCode::DeviceFailure, "resetting the command list", reset);
        }
        return true;
    }

    Result<bool> present(void* window, gpu::Texture texture) override {
        const auto found = textures_.find(texture.id);
        if (window == nullptr || found == textures_.end() || found->second.format != gpu::TextureFormat::Rgba8Unorm) {
            return Error{ErrorCode::DeviceFailure, "present needs a window and an Rgba8Unorm render target"};
        }
        TextureRecord& record = found->second;
        SwapChain& swap = swap_chains_[window];
        if (!swap.chain || swap.width != record.width || swap.height != record.height) {
            if (std::optional<Error> error = prepare_swap_chain(window, swap, record.width, record.height)) {
                swap_chains_.erase(window);
                return *std::move(error);
            }
        }
        Record& back = swap.buffers[swap.chain->GetCurrentBackBufferIndex()];
        transition(back, D3D12_RESOURCE_STATE_COPY_DEST);
        transition(record, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list_->CopyResource(back.resource.Get(), record.resource.Get());
        transition(back, D3D12_RESOURCE_STATE_PRESENT);
        Result<bool> flushed = flush();
        if (!flushed) {
            return flushed;
        }
        const HRESULT hr = swap.chain->Present(1U, 0U);
        if (FAILED(hr)) {
            return failure(ErrorCode::DeviceFailure, "presenting", hr);
        }
        return true;
    }

private:
    // A buffer or texture with its state as of the end of the recorded work,
    // and as of the last work that ran.
    struct Record {
        ComPtr<ID3D12Resource> resource;
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
        D3D12_RESOURCE_STATES executed{D3D12_RESOURCE_STATE_COMMON};
    };

    struct TextureRecord final : Record {
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        std::uint32_t rtv_slot{0U};
        std::uint32_t width{0U};
        std::uint32_t height{0U};
        gpu::TextureFormat format{gpu::TextureFormat::Rgba8Unorm};
    };

    struct SwapChain final {
        ComPtr<IDXGISwapChain3> chain;
        std::vector<Record> buffers;
        std::uint32_t width{0U};
        std::uint32_t height{0U};
    };

    DeviceImpl() = default;

    std::optional<Error> initialise(const DeviceOptions& options) {
        if (options.debug_layer) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
            }
        }
        HRESULT hr = CreateDXGIFactory2(0U, IID_PPV_ARGS(&factory_));
        if (FAILED(hr)) {
            return failure(ErrorCode::GpuUnavailable, "creating a DXGI factory", hr);
        }

        std::vector<ComPtr<IDXGIAdapter1>> candidates;
        if (options.warp) {
            ComPtr<IDXGIAdapter1> warp;
            if (SUCCEEDED(factory_->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
                candidates.push_back(warp);
            }
        } else {
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0U; factory_->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                                   IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND;
                 ++i) {
                DXGI_ADAPTER_DESC1 desc{};
                adapter->GetDesc1(&desc);
                if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0U) {
                    candidates.push_back(adapter);
                }
            }
        }
        for (const ComPtr<IDXGIAdapter1>& adapter : candidates) {
            ComPtr<ID3D12Device> device;
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
                continue;
            }
            D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_2};
            if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) ||
                model.HighestShaderModel < D3D_SHADER_MODEL_6_2) {
                continue;
            }
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            adapter_name_ = narrow(desc.Description);
            device_ = device;
            if (options.debug_layer) {
                (void)device_.As(&info_queue_);
            }
            break;
        }
        if (!device_) {
            return Error{ErrorCode::GpuUnavailable, options.warp ? "WARP with shader model 6.2 is not available"
                                                                 : "no hardware adapter with shader model 6.2"};
        }

        const D3D12_COMMAND_QUEUE_DESC queue{D3D12_COMMAND_LIST_TYPE_DIRECT};
        if (FAILED(hr = device_->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue_))) ||
            FAILED(hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_))) ||
            FAILED(hr = device_->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                                                   IID_PPV_ARGS(&list_))) ||
            FAILED(hr = device_->CreateFence(0U, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) {
            return failure(ErrorCode::GpuUnavailable, "creating the command queue", hr);
        }
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (event_ == nullptr) {
            return Error{ErrorCode::GpuUnavailable, "creating a fence event failed"};
        }

        // Sixteen root constants at b0 and eight root UAVs at u0..u7: no
        // descriptor heaps, and every kernel and draw shares one signature.
        D3D12_ROOT_PARAMETER parameters[1U + gpu::max_bound_buffers]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0U, 1U, gpu::root_constant_count}; // b0, space 1
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        for (std::uint32_t slot = 0U; slot < gpu::max_bound_buffers; ++slot) {
            parameters[1U + slot].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            parameters[1U + slot].Descriptor = {slot, 0U};
            parameters[1U + slot].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_ROOT_SIGNATURE_DESC root{};
        root.NumParameters = static_cast<UINT>(std::size(parameters));
        root.pParameters = parameters;
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> errors;
        if (FAILED(hr = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)) ||
            FAILED(hr = device_->CreateRootSignature(0U, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                     IID_PPV_ARGS(&root_signature_)))) {
            return failure(ErrorCode::GpuUnavailable, "creating the root signature", hr);
        }

        if (std::optional<Error> error = create_signature(D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH,
                                                          sizeof(D3D12_DISPATCH_ARGUMENTS), dispatch_signature_)) {
            return error;
        }
        if (std::optional<Error> error =
                create_signature(D3D12_INDIRECT_ARGUMENT_TYPE_DRAW, sizeof(D3D12_DRAW_ARGUMENTS), draw_signature_)) {
            return error;
        }

        D3D12_DESCRIPTOR_HEAP_DESC rtv_heap{};
        rtv_heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_heap.NumDescriptors = max_render_targets;
        if (FAILED(hr = device_->CreateDescriptorHeap(&rtv_heap, IID_PPV_ARGS(&rtv_heap_)))) {
            return failure(ErrorCode::GpuUnavailable, "creating the render target heap", hr);
        }
        rtv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        for (std::uint32_t slot = max_render_targets; slot > 0U; --slot) {
            free_rtv_slots_.push_back(slot - 1U);
        }

        Result<gpu::Buffer> dummy = create_buffer(256U, "ajfx unbound slot");
        if (!dummy) {
            return dummy.error();
        }
        dummy_ = dummy.value();
        return std::nullopt;
    }

    std::optional<Error> create_signature(D3D12_INDIRECT_ARGUMENT_TYPE type, UINT stride,
                                          ComPtr<ID3D12CommandSignature>& out) {
        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type = type;
        D3D12_COMMAND_SIGNATURE_DESC signature{};
        signature.ByteStride = stride;
        signature.NumArgumentDescs = 1U;
        signature.pArgumentDescs = &argument;
        const HRESULT hr = device_->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&out));
        if (FAILED(hr)) {
            return failure(ErrorCode::GpuUnavailable, "creating an indirect argument signature", hr);
        }
        return std::nullopt;
    }

    gpu::Pipeline add_pipeline(ComPtr<ID3D12PipelineState> pipeline, std::string_view name) {
        pipeline->SetName(widen(name).c_str());
        const std::uint32_t id = next_id_++;
        pipelines_.emplace(id, std::move(pipeline));
        return gpu::Pipeline{id};
    }

    HRESULT recreate_command_list() {
        list_.Reset();
        allocator_.Reset();
        HRESULT hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator_));
        if (SUCCEEDED(hr)) {
            hr = device_->CreateCommandList(0U, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                                            IID_PPV_ARGS(&list_));
        }
        return hr;
    }

    // With the debug layer on, any error or corruption message it stored
    // since the last flush fails the flush, so tests catch invalid use.
    std::optional<Error> debug_layer_errors() {
        if (!info_queue_) {
            return std::nullopt;
        }
        std::optional<Error> error;
        const UINT64 count = info_queue_->GetNumStoredMessages();
        for (UINT64 i = 0U; i < count && !error; ++i) {
            SIZE_T length = 0U;
            info_queue_->GetMessage(i, nullptr, &length);
            std::vector<std::uint8_t> storage(length);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(info_queue_->GetMessage(i, message, &length)) &&
                (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                 message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)) {
                error = Error{ErrorCode::DeviceFailure,
                              "D3D12 debug layer: " + std::string{message->pDescription, message->DescriptionByteLength}};
            }
        }
        info_queue_->ClearStoredMessages();
        return error;
    }

    Record* find(gpu::Buffer buffer) {
        const auto found = buffers_.find(buffer.id);
        return found == buffers_.end() ? nullptr : &found->second;
    }

    void note(Error error) {
        if (!deferred_) {
            deferred_ = std::move(error);
        }
    }

    void transition(Record& record, D3D12_RESOURCE_STATES state) {
        if (record.state == state) {
            return;
        }
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = record.resource.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = record.state;
        barrier.Transition.StateAfter = state;
        list_->ResourceBarrier(1U, &barrier);
        record.state = state;
    }

    // Transitions every bound buffer for read-write access and resolves its
    // address; unbound slots get a small placeholder. False (with a deferred
    // error) if a buffer is unknown or is also the indirect argument buffer.
    bool prepare_buffers(const std::array<gpu::Buffer, gpu::max_bound_buffers>& buffers, const Record* indirect,
                         std::array<D3D12_GPU_VIRTUAL_ADDRESS, gpu::max_bound_buffers>& addresses) {
        for (std::uint32_t slot = 0U; slot < gpu::max_bound_buffers; ++slot) {
            Record* record = buffers[slot].valid() ? find(buffers[slot]) : &buffers_.at(dummy_.id);
            if (record == nullptr || record == indirect) {
                note(Error{ErrorCode::DeviceFailure, "binding an unknown buffer or the indirect argument buffer"});
                return false;
            }
            transition(*record, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            addresses[slot] = record->resource->GetGPUVirtualAddress();
        }
        return true;
    }

    Result<ComPtr<ID3D12Resource>> create_readback(std::uint64_t bytes) {
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        const D3D12_RESOURCE_DESC desc = buffer_desc(bytes, D3D12_RESOURCE_FLAG_NONE);
        ComPtr<ID3D12Resource> readback;
        const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&readback));
        if (FAILED(hr)) {
            return failure(ErrorCode::DeviceFailure, "creating a readback buffer", hr);
        }
        return readback;
    }

    // Flushes, maps the readback buffer and lets `extract` copy out of it.
    template <class Extract>
    Result<std::vector<std::uint8_t>> finish_readback(ID3D12Resource* readback, std::uint64_t bytes, Extract extract) {
        Result<bool> flushed = flush();
        if (!flushed) {
            return flushed.error();
        }
        void* mapped = nullptr;
        const D3D12_RANGE range{0U, static_cast<SIZE_T>(bytes)};
        const HRESULT hr = readback->Map(0U, &range, &mapped);
        if (FAILED(hr)) {
            return failure(ErrorCode::DeviceFailure, "mapping a readback buffer", hr);
        }
        std::vector<std::uint8_t> out = extract(static_cast<const std::uint8_t*>(mapped));
        const D3D12_RANGE nothing{0U, 0U};
        readback->Unmap(0U, &nothing);
        return out;
    }

    std::optional<Error> prepare_swap_chain(void* window, SwapChain& swap, std::uint32_t width, std::uint32_t height) {
        // Nothing recorded may still refer to the old buffers.
        if (Result<bool> flushed = flush(); !flushed) {
            return flushed.error();
        }
        swap.buffers.clear();
        HRESULT hr = S_OK;
        if (swap.chain) {
            hr = swap.chain->ResizeBuffers(0U, width, height, DXGI_FORMAT_UNKNOWN, 0U);
        } else {
            DXGI_SWAP_CHAIN_DESC1 desc{};
            desc.Width = width;
            desc.Height = height;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1U;
            desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            desc.BufferCount = 2U;
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            ComPtr<IDXGISwapChain1> chain;
            hr = factory_->CreateSwapChainForHwnd(queue_.Get(), static_cast<HWND>(window), &desc, nullptr, nullptr,
                                                  &chain);
            if (SUCCEEDED(hr)) {
                (void)factory_->MakeWindowAssociation(static_cast<HWND>(window), DXGI_MWA_NO_ALT_ENTER);
                hr = chain.As(&swap.chain);
            }
        }
        for (UINT i = 0U; SUCCEEDED(hr) && i < 2U; ++i) {
            Record& buffer = swap.buffers.emplace_back();
            hr = swap.chain->GetBuffer(i, IID_PPV_ARGS(&buffer.resource));
            buffer.state = D3D12_RESOURCE_STATE_PRESENT;
            buffer.executed = D3D12_RESOURCE_STATE_PRESENT;
        }
        if (FAILED(hr)) {
            return failure(ErrorCode::DeviceFailure, "creating the window's swap chain", hr);
        }
        swap.width = width;
        swap.height = height;
        return std::nullopt;
    }

    // Copies bytes into upload memory that lives until the next flush.
    std::optional<std::pair<ID3D12Resource*, std::uint64_t>> stage(std::span<const std::uint8_t> bytes) {
        const std::uint64_t size = (bytes.size() + 15U) & ~std::uint64_t{15U};
        if (upload_used_ + size > upload_capacity_) {
            upload_capacity_ = std::max(upload_page_bytes, size);
            const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
            const D3D12_RESOURCE_DESC desc = buffer_desc(upload_capacity_, D3D12_RESOURCE_FLAG_NONE);
            ComPtr<ID3D12Resource> page;
            HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                          D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                          IID_PPV_ARGS(&page));
            void* mapped = nullptr;
            if (SUCCEEDED(hr)) {
                hr = page->Map(0U, nullptr, &mapped);
            }
            if (FAILED(hr)) {
                note(failure(ErrorCode::DeviceFailure, "creating upload memory", hr));
                upload_used_ = upload_capacity_;
                return std::nullopt;
            }
            uploads_.push_back(std::move(page));
            upload_mapped_ = static_cast<std::uint8_t*>(mapped);
            upload_used_ = 0U;
        }
        std::memcpy(upload_mapped_ + upload_used_, bytes.data(), bytes.size());
        const std::uint64_t offset = upload_used_;
        upload_used_ += size;
        return std::pair{uploads_.back().Get(), offset};
    }

    Result<bool> wait_idle() {
        const std::uint64_t value = ++fence_value_;
        HRESULT hr = queue_->Signal(fence_.Get(), value);
        if (SUCCEEDED(hr) && fence_->GetCompletedValue() < value) {
            hr = fence_->SetEventOnCompletion(value, event_);
            if (SUCCEEDED(hr)) {
                WaitForSingleObject(event_, INFINITE);
            }
        }
        if (FAILED(hr)) {
            return failure(ErrorCode::DeviceFailure, "waiting for the GPU", hr);
        }
        const HRESULT removed = device_->GetDeviceRemovedReason();
        if (FAILED(removed)) {
            return failure(ErrorCode::DeviceFailure, "the device was removed", removed);
        }
        return true;
    }

    ComPtr<IDXGIFactory6> factory_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12InfoQueue> info_queue_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12RootSignature> root_signature_;
    ComPtr<ID3D12CommandSignature> dispatch_signature_;
    ComPtr<ID3D12CommandSignature> draw_signature_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    std::uint32_t rtv_size_{0U};
    std::vector<std::uint32_t> free_rtv_slots_;
    HANDLE event_{nullptr};
    std::uint64_t fence_value_{0U};
    std::string adapter_name_;

    std::unordered_map<std::uint32_t, Record> buffers_;
    std::unordered_map<std::uint32_t, ComPtr<ID3D12PipelineState>> pipelines_;
    std::unordered_map<std::uint32_t, TextureRecord> textures_;
    std::unordered_map<void*, SwapChain> swap_chains_;
    TextureRecord* rendering_{nullptr};
    std::uint32_t next_id_{1U};
    gpu::Buffer dummy_;

    std::vector<ComPtr<ID3D12Resource>> uploads_;
    std::uint8_t* upload_mapped_{nullptr};
    std::uint64_t upload_used_{0U};
    std::uint64_t upload_capacity_{0U};

    std::vector<ComPtr<ID3D12Resource>> retired_;
    std::vector<ComPtr<ID3D12PipelineState>> retired_pipelines_;
    std::vector<std::uint32_t> retired_rtv_slots_;
    std::optional<Error> deferred_;
};

} // namespace

Result<std::unique_ptr<Device>> create_device(const DeviceOptions& options) {
    return DeviceImpl::create(options);
}

} // namespace ajfx::d3d12
