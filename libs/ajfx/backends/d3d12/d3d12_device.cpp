#include <ajfx/d3d12.hpp>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <memory>
#include <cstring>
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

constexpr std::uint64_t upload_page_bytes = 1U << 20U;

class Device final : public gpu::Device {
public:
    static Result<std::unique_ptr<gpu::Device>> create(const DeviceOptions& options) {
        auto device = std::unique_ptr<Device>(new Device());
        if (std::optional<Error> error = device->initialise(options)) {
            return *std::move(error);
        }
        return std::unique_ptr<gpu::Device>{std::move(device)};
    }

    ~Device() override {
        if (queue_ && fence_) {
            (void)wait_idle();
        }
        if (event_ != nullptr) {
            CloseHandle(event_);
        }
    }

    [[nodiscard]] gpu::ShaderFormat shader_format() const noexcept override { return gpu::ShaderFormat::Dxil; }
    [[nodiscard]] std::string_view adapter_name() const noexcept override { return adapter_name_; }

    Result<gpu::Buffer> create_buffer(std::uint64_t bytes, std::string_view name) override {
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC desc = buffer_desc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        ComPtr<ID3D12Resource> resource;
        // Committed resources on an implicit heap are zero-filled.
        const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                            D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                            IID_PPV_ARGS(&resource));
        if (FAILED(hr)) {
            return failure(ErrorCode::DeviceFailure, "creating a " + std::to_string(bytes) + "-byte buffer", hr);
        }
        resource->SetName(widen(name).c_str());
        const std::uint32_t id = next_id_++;
        buffers_.emplace(id, BufferRecord{std::move(resource)});
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
        pipeline->SetName(widen(name).c_str());
        const std::uint32_t id = next_id_++;
        pipelines_.emplace(id, std::move(pipeline));
        return gpu::Pipeline{id};
    }

    void destroy_pipeline(gpu::Pipeline pipeline) override {
        const auto found = pipelines_.find(pipeline.id);
        if (found == pipelines_.end()) {
            return;
        }
        retired_pipelines_.push_back(std::move(found->second));
        pipelines_.erase(found);
    }

    void write_buffer(gpu::Buffer buffer, std::uint64_t offset, std::span<const std::uint8_t> bytes) override {
        BufferRecord* record = find(buffer);
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
        BufferRecord* to = find(destination);
        BufferRecord* from = find(source);
        if (to == nullptr || from == nullptr || to == from || bytes == 0U) {
            note(Error{ErrorCode::DeviceFailure, "copy_buffer needs two different, valid buffers"});
            return;
        }
        transition(*from, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(*to, D3D12_RESOURCE_STATE_COPY_DEST);
        list_->CopyBufferRegion(to->resource.Get(), destination_offset, from->resource.Get(), source_offset, bytes);
    }

    void dispatch(const gpu::Dispatch& d) override {
        const auto pipeline = pipelines_.find(d.pipeline.id);
        if (pipeline == pipelines_.end()) {
            note(Error{ErrorCode::DeviceFailure, "dispatch of an unknown pipeline"});
            return;
        }
        BufferRecord* indirect = d.indirect.valid() ? find(d.indirect) : nullptr;
        if (d.indirect.valid() && indirect == nullptr) {
            note(Error{ErrorCode::DeviceFailure, "dispatch with an unknown indirect buffer"});
            return;
        }
        list_->SetComputeRootSignature(root_signature_.Get());
        list_->SetPipelineState(pipeline->second.Get());
        list_->SetComputeRoot32BitConstants(0U, gpu::root_constant_count, d.constants.data(), 0U);
        for (std::uint32_t slot = 0U; slot < gpu::max_bound_buffers; ++slot) {
            BufferRecord* record = d.buffers[slot].valid() ? find(d.buffers[slot]) : &buffers_.at(dummy_.id);
            if (record == nullptr || record == indirect) {
                note(Error{ErrorCode::DeviceFailure, "dispatch binds an unknown buffer or its indirect buffer"});
                return;
            }
            transition(*record, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list_->SetComputeRootUnorderedAccessView(1U + slot, record->resource->GetGPUVirtualAddress());
        }
        if (indirect != nullptr) {
            transition(*indirect, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
            list_->ExecuteIndirect(dispatch_signature_.Get(), 1U, indirect->resource.Get(), d.indirect_offset,
                                   nullptr, 0U);
        } else {
            list_->Dispatch(d.groups_x, d.groups_y, 1U);
        }
    }

    void barrier() override {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        barrier.UAV.pResource = nullptr;
        list_->ResourceBarrier(1U, &barrier);
    }

    Result<bool> flush() override {
        HRESULT hr = list_->Close();
        if (SUCCEEDED(hr)) {
            ID3D12CommandList* lists[] = {list_.Get()};
            queue_->ExecuteCommandLists(1U, lists);
        }
        // The recorded transitions happened only if the list ran.
        for (auto& [id, record] : buffers_) {
            if (SUCCEEDED(hr)) {
                record.executed = record.state;
            } else {
                record.state = record.executed;
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

    Result<std::vector<std::uint8_t>> read_buffer(gpu::Buffer buffer, std::uint64_t offset,
                                                  std::uint64_t bytes) override {
        BufferRecord* record = find(buffer);
        if (record == nullptr) {
            return Error{ErrorCode::DeviceFailure, "read_buffer of an unknown buffer"};
        }
        if (bytes == 0U) {
            return std::vector<std::uint8_t>{};
        }
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        const D3D12_RESOURCE_DESC desc = buffer_desc(bytes, D3D12_RESOURCE_FLAG_NONE);
        ComPtr<ID3D12Resource> readback;
        HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&readback));
        if (FAILED(hr)) {
            return failure(ErrorCode::DeviceFailure, "creating a readback buffer", hr);
        }
        transition(*record, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list_->CopyBufferRegion(readback.Get(), 0U, record->resource.Get(), offset, bytes);
        Result<bool> flushed = flush();
        if (!flushed) {
            return flushed.error();
        }
        void* mapped = nullptr;
        const D3D12_RANGE range{0U, static_cast<SIZE_T>(bytes)};
        hr = readback->Map(0U, &range, &mapped);
        if (FAILED(hr)) {
            return failure(ErrorCode::DeviceFailure, "mapping a readback buffer", hr);
        }
        std::vector<std::uint8_t> out(static_cast<std::size_t>(bytes));
        std::memcpy(out.data(), mapped, out.size());
        const D3D12_RANGE nothing{0U, 0U};
        readback->Unmap(0U, &nothing);
        return out;
    }

private:
    struct BufferRecord final {
        ComPtr<ID3D12Resource> resource;
        // As of the end of the recorded work, and of the last work that ran.
        D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
        D3D12_RESOURCE_STATES executed{D3D12_RESOURCE_STATE_COMMON};
    };

    Device() = default;

    std::optional<Error> initialise(const DeviceOptions& options) {
        if (options.debug_layer) {
            ComPtr<ID3D12Debug> debug;
            if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
                debug->EnableDebugLayer();
            }
        }
        ComPtr<IDXGIFactory6> factory;
        HRESULT hr = CreateDXGIFactory2(0U, IID_PPV_ARGS(&factory));
        if (FAILED(hr)) {
            return failure(ErrorCode::GpuUnavailable, "creating a DXGI factory", hr);
        }

        std::vector<ComPtr<IDXGIAdapter1>> candidates;
        if (options.warp) {
            ComPtr<IDXGIAdapter1> warp;
            if (SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) {
                candidates.push_back(warp);
            }
        } else {
            ComPtr<IDXGIAdapter1> adapter;
            for (UINT i = 0U; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
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
        // descriptor heaps, and every kernel shares one signature.
        D3D12_ROOT_PARAMETER parameters[1U + gpu::max_bound_buffers]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0U, 0U, gpu::root_constant_count};
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

        D3D12_INDIRECT_ARGUMENT_DESC argument{};
        argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC signature{};
        signature.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        signature.NumArgumentDescs = 1U;
        signature.pArgumentDescs = &argument;
        if (FAILED(hr = device_->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&dispatch_signature_)))) {
            return failure(ErrorCode::GpuUnavailable, "creating the dispatch signature", hr);
        }

        Result<gpu::Buffer> dummy = create_buffer(256U, "ajfx unbound slot");
        if (!dummy) {
            return dummy.error();
        }
        dummy_ = dummy.value();
        return std::nullopt;
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

    BufferRecord* find(gpu::Buffer buffer) {
        const auto found = buffers_.find(buffer.id);
        return found == buffers_.end() ? nullptr : &found->second;
    }

    void note(Error error) {
        if (!deferred_) {
            deferred_ = std::move(error);
        }
    }

    void transition(BufferRecord& record, D3D12_RESOURCE_STATES state) {
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

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12InfoQueue> info_queue_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> allocator_;
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    ComPtr<ID3D12RootSignature> root_signature_;
    ComPtr<ID3D12CommandSignature> dispatch_signature_;
    HANDLE event_{nullptr};
    std::uint64_t fence_value_{0U};
    std::string adapter_name_;

    std::unordered_map<std::uint32_t, BufferRecord> buffers_;
    std::unordered_map<std::uint32_t, ComPtr<ID3D12PipelineState>> pipelines_;
    std::uint32_t next_id_{1U};
    gpu::Buffer dummy_;

    std::vector<ComPtr<ID3D12Resource>> uploads_;
    std::uint8_t* upload_mapped_{nullptr};
    std::uint64_t upload_used_{0U};
    std::uint64_t upload_capacity_{0U};

    std::vector<ComPtr<ID3D12Resource>> retired_;
    std::vector<ComPtr<ID3D12PipelineState>> retired_pipelines_;
    std::optional<Error> deferred_;
};

} // namespace

Result<std::unique_ptr<gpu::Device>> create_device(const DeviceOptions& options) {
    return Device::create(options);
}

} // namespace ajfx::d3d12
