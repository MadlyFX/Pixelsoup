#include <mediaserver/rhi/device.hpp>

#if defined(_WIN32)
#include <d3d12.h>
#include <wrl/client.h>
#ifdef DeviceCapabilities
#undef DeviceCapabilities
#endif
#endif

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/compute_fill_comp_dxil.h>
#include <mediaserver/shaders/solid_frag_dxil.h>
#include <mediaserver/shaders/solid_vert_dxil.h>
#include <mediaserver/shaders/compute_gather_comp_dxil.h>
#endif

#include <array>
#include <cstddef>
#include <cstring>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace rhi = mediaserver::rhi;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define RHI_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] rhi::Device open_device() {
    rhi::DeviceConfig config{};
    config.debug = false;
    auto created = rhi::Device::create(config);
    check(created.has_value(), "device creation", __LINE__);
    return std::move(created).value();
}

[[nodiscard]] rhi::TextureDesc target_desc(core::u32 width, core::u32 height) {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.color_target = true;
    desc.sampled = true;
    return desc;
}

// A clear is the one render pass that needs no pipeline, which makes it the
// right first proof: the device, the queue, the fence, and the readback path
// are all exercised without a single byte of shader bytecode.
void an_offscreen_target_clears_and_reads_back_exactly() {
    auto device = open_device();
    RHI_CHECK(!device.driver_name().empty());

    constexpr core::u32 width = 64U;
    constexpr core::u32 height = 32U;
    auto texture = device.create_texture(target_desc(width, height));
    RHI_CHECK(texture);
    auto readback = device.create_readback(width * height * 4U);
    RHI_CHECK(readback);

    auto commands = device.acquire_commands();
    RHI_CHECK(commands);
    auto list = std::move(commands).value();
    RHI_CHECK(list.clear(texture.value(), rhi::Color{0.25F, 0.5F, 0.75F, 1.0F}));
    RHI_CHECK(list.download(texture.value(), readback.value()));

    auto submitted = device.submit(std::move(list));
    RHI_CHECK(submitted);
    auto work = std::move(submitted).value();
    RHI_CHECK(work.wait());
    RHI_CHECK(work.submit_to_complete().count() > 0);

    const auto pixels = readback.value().map();
    RHI_CHECK(pixels.size() == static_cast<std::size_t>(width) * height * 4U);
    // Exact UNORM quantisation of the clear colour, so this is an equality
    // check rather than a tolerance: a wrong channel order fails it.
    RHI_CHECK(pixels[0] == 64U);
    RHI_CHECK(pixels[1] == 127U);
    RHI_CHECK(pixels[2] == 191U);
    RHI_CHECK(pixels[3] == 255U);
    // Every pixel, not just the first: a partial clear would pass a corner
    // check and fail the wall.
    for (std::size_t index = 0U; index < pixels.size(); index += 4U) {
        RHI_CHECK(pixels[index] == 64U);
        RHI_CHECK(pixels[index + 3U] == 255U);
    }
    readback.value().unmap();
}

// The presentation step is a GPU-to-GPU blit. Proving it against an offscreen
// destination means the path is covered on a machine with no display.
void a_blit_moves_pixels_between_targets_without_the_cpu() {
    auto device = open_device();

    auto source = device.create_texture(target_desc(32U, 16U));
    auto destination = device.create_texture(target_desc(64U, 32U));
    RHI_CHECK(source);
    RHI_CHECK(destination);
    auto readback = device.create_readback(64U * 32U * 4U);
    RHI_CHECK(readback);

    auto list = std::move(device.acquire_commands()).value();
    RHI_CHECK(list.clear(source.value(), rhi::Color{1.0F, 0.0F, 0.0F, 1.0F}));
    RHI_CHECK(list.clear(destination.value(), rhi::Color{0.0F, 0.0F, 1.0F, 1.0F}));
    // Upscaling as well as copying, because presentation blits an offscreen
    // target onto a swapchain image that need not match its size.
    RHI_CHECK(
        list.blit(source.value(), destination.value(), rhi::Filter::Nearest));
    RHI_CHECK(list.download(destination.value(), readback.value()));

    auto work = std::move(device.submit(std::move(list))).value();
    RHI_CHECK(work.wait());

    const auto pixels = readback.value().map();
    RHI_CHECK(pixels.size() == 64U * 32U * 4U);
    RHI_CHECK(pixels[0] == 255U);
    RHI_CHECK(pixels[1] == 0U);
    RHI_CHECK(pixels[2] == 0U);
    readback.value().unmap();
}

void resource_handles_release_exactly_once_and_survive_moves() {
    auto device = open_device();

    auto first = device.create_texture(target_desc(8U, 8U));
    RHI_CHECK(first);
    auto moved = std::move(first).value();
    RHI_CHECK(moved);

    rhi::Texture empty;
    RHI_CHECK(!empty);
    empty = std::move(moved);
    RHI_CHECK(empty);
    RHI_CHECK(empty.width() == 8U);

    // Self-move must not release the handle it is about to keep.
    auto& alias = empty;
    empty = std::move(alias);
    RHI_CHECK(empty);

    auto buffer = device.create_storage_buffer(256U);
    RHI_CHECK(buffer);
    rhi::StorageBuffer taken = std::move(buffer).value();
    RHI_CHECK(taken);
    RHI_CHECK(taken.capacity_bytes() == 256U);

    auto sampler = device.create_sampler(rhi::Filter::Nearest);
    RHI_CHECK(sampler);
    device.wait_idle();
}

void invalid_descriptions_are_refused_before_the_backend_sees_them() {
    auto device = open_device();

    auto zero_size = device.create_texture(target_desc(0U, 16U));
    RHI_CHECK(!zero_size);
    RHI_CHECK(zero_size.error() == rhi::RhiError::InvalidConfiguration);

    // A texture that is neither a target nor sampled has no use, and refusing
    // it is better than allocating one nothing can bind.
    rhi::TextureDesc useless{};
    useless.width = 8U;
    useless.height = 8U;
    auto refused = device.create_texture(useless);
    RHI_CHECK(!refused);
    RHI_CHECK(refused.error() == rhi::RhiError::InvalidConfiguration);

    auto empty_buffer = device.create_storage_buffer(0U);
    RHI_CHECK(!empty_buffer);
    RHI_CHECK(empty_buffer.error() == rhi::RhiError::InvalidConfiguration);

    rhi::DeviceConfig bad{};
    bad.frames_in_flight = 0U;
    auto bad_device = rhi::Device::create(bad);
    RHI_CHECK(!bad_device);
    RHI_CHECK(bad_device.error() == rhi::RhiError::InvalidConfiguration);

    auto invalid_layers = target_desc(8U, 8U);
    invalid_layers.array_layers = 0U;
    RHI_CHECK(!device.create_texture(invalid_layers));
    auto invalid_mips = target_desc(8U, 8U);
    invalid_mips.mip_levels = 0U;
    RHI_CHECK(!device.create_texture(invalid_mips));
}

void capabilities_and_native_handles_are_typed_and_immutable() {
    auto device = open_device();
    const rhi::DeviceCapabilities capabilities = device.capabilities();
    RHI_CHECK(capabilities.backend == rhi::BackendKind::SdlGpu);
    RHI_CHECK(!capabilities.advanced_scene_raster);
    RHI_CHECK(capabilities.offscreen);
    RHI_CHECK(!capabilities.advanced_raster());
    RHI_CHECK(!capabilities.advanced_ray_tracing());
    // The default backend is SDL_GPU, which exposes no dispatch. Both flags
    // therefore report false, including storage_images: it means bindable and
    // dispatchable, not merely constructible.
    RHI_CHECK(!capabilities.compute);
    RHI_CHECK(!capabilities.storage_images);
    RHI_CHECK(!capabilities.bindless);
    RHI_CHECK(!capabilities.explicit_barriers);
    RHI_CHECK(!capabilities.async_compute);
    RHI_CHECK(!capabilities.indirect_draw);
    RHI_CHECK(!capabilities.acceleration_structures);
    RHI_CHECK(!capabilities.ray_queries);
    RHI_CHECK(capabilities.rgba16_float ==
              device.supports(rhi::TextureFormat::Rgba16Float));
    RHI_CHECK(capabilities.rgba32_float ==
              device.supports(rhi::TextureFormat::Rgba32Float));
    RHI_CHECK(capabilities.integer_targets ==
              device.supports(rhi::TextureFormat::R32Uint));

    const rhi::NativeHandle device_handle = device.native_handle();
    RHI_CHECK(device_handle);
    RHI_CHECK(device_handle.backend == rhi::BackendKind::SdlGpu);
    RHI_CHECK(device_handle.object == rhi::NativeObjectKind::Device);

    auto texture = device.create_texture(target_desc(8U, 8U));
    RHI_CHECK(texture);
    const rhi::NativeHandle texture_handle = texture.value().native_handle();
    RHI_CHECK(texture_handle);
    RHI_CHECK(texture_handle.backend == rhi::BackendKind::SdlGpu);
    RHI_CHECK(texture_handle.object == rhi::NativeObjectKind::Texture);
    RHI_CHECK(texture_handle.value == texture.value().backend_handle());

}

// A device the driver removes says so, and why; a working one says nothing.
void native_d3d12_reports_a_lost_device() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto created = rhi::Device::create(config);
    RHI_CHECK(created);
    auto device = std::move(created).value();
    RHI_CHECK(device.lost_reason().empty());
#if defined(_WIN32)
    // D3D12's own way to make a device lost, as a hang or a fault would.
    auto* native = static_cast<ID3D12Device*>(device.native_handle().value);
    Microsoft::WRL::ComPtr<ID3D12Device5> removable;
    if (native == nullptr || FAILED(native->QueryInterface(IID_PPV_ARGS(&removable)))) {
        return;
    }
    removable->RemoveDevice();
    RHI_CHECK(!device.lost_reason().empty());
#endif
}

void native_d3d12_is_nonblocking_and_preserves_linear_float_pixels() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    config.frames_in_flight = 4U;
    auto created = rhi::Device::create(config);
    RHI_CHECK(created);
    auto device = std::move(created).value();
    const rhi::DeviceCapabilities capabilities = device.capabilities();
    RHI_CHECK(capabilities.advanced_scene_raster);
    RHI_CHECK(capabilities.offscreen);
    RHI_CHECK(capabilities.rgba16_float);
    RHI_CHECK(capabilities.sampled_depth);
    RHI_CHECK(capabilities.integer_targets);
    RHI_CHECK(capabilities.advanced_raster());
    // Compute is callable on this backend now; async compute still is not, and
    // the two are separate flags precisely so this line can say that.
    RHI_CHECK(capabilities.compute);
    RHI_CHECK(capabilities.storage_images);
    RHI_CHECK(!capabilities.bindless);
    // Explicit barriers are callable now; async compute still is not.
    RHI_CHECK(capabilities.explicit_barriers);
    RHI_CHECK(!capabilities.async_compute);
    // Writable buffers and indirect dispatch and draw are callable; see
    // rhi_compute_buffer_tests.cpp.
    RHI_CHECK(capabilities.writable_buffers);
    RHI_CHECK(capabilities.indirect_dispatch);
    RHI_CHECK(capabilities.indirect_draw);
    RHI_CHECK(!capabilities.acceleration_structures);
    RHI_CHECK(!capabilities.ray_queries);
    RHI_CHECK(!capabilities.advanced_ray_tracing());
    RHI_CHECK(device.supports(rhi::ShaderFormat::Dxil));
    RHI_CHECK(!device.supports(rhi::ShaderFormat::SpirV));
    RHI_CHECK(device.native_handle().backend == rhi::BackendKind::D3d12);

    {
        rhi::TextureDesc desc{};
        desc.width = 7U; // Deliberately exercises D3D12's padded row pitch.
        desc.height = 3U;
        desc.format = rhi::TextureFormat::Rgba16Float;
        desc.color_target = true;
        desc.sampled = true;
        auto target = device.create_texture(desc);
        RHI_CHECK(target);
        RHI_CHECK(target.value().native_handle().value
                  != target.value().backend_handle());
        auto readback = device.create_readback(7U * 3U * 8U);
        RHI_CHECK(readback);
        auto commands = device.acquire_commands();
        RHI_CHECK(commands);
        auto list = std::move(commands).value();
        RHI_CHECK(list.clear(
            target.value(), rhi::Color{0.25F, 0.5F, 0.75F, 1.0F}));
        RHI_CHECK(list.download(target.value(), readback.value()));
        auto submitted = device.submit(std::move(list));
        RHI_CHECK(submitted);
        RHI_CHECK(submitted.value().wait());
        RHI_CHECK(submitted.value().gpu_execution().count() > 0);
        const auto pixels = readback.value().map();
        RHI_CHECK(pixels.size() == 7U * 3U * 8U);
        // IEEE-754 binary16, little endian: .25, .5, .75, 1.0.
        const std::array<core::u8, 8U> expected{
            0x00U, 0x34U, 0x00U, 0x38U, 0x00U, 0x3AU, 0x00U, 0x3CU};
        for (std::size_t pixel = 0U; pixel < 21U; ++pixel) {
            for (std::size_t channel_byte = 0U;
                 channel_byte < expected.size(); ++channel_byte) {
                RHI_CHECK(pixels[pixel * expected.size() + channel_byte]
                          == expected[channel_byte]);
            }
        }
        readback.value().unmap();
    }

    // Operators rebuild shows continuously. Resource churn must return CPU
    // descriptor slots instead of eventually exhausting a monotonic heap.
    for (core::u32 index = 0U; index < 2'500U; ++index) {
        auto sampler = device.create_sampler(
            (index & 1U) == 0U ? rhi::Filter::Linear
                               : rhi::Filter::Nearest);
        RHI_CHECK(sampler);
    }

    // Command acquisition is a poll, never a hidden queue wait.
    std::array<rhi::CommandList, 4U> held{};
    for (auto& command : held) {
        auto acquired = device.acquire_commands();
        RHI_CHECK(acquired);
        command = std::move(acquired).value();
    }
    auto exhausted = device.acquire_commands();
    RHI_CHECK(!exhausted);
    RHI_CHECK(exhausted.error() == rhi::RhiError::Unavailable);
}

[[nodiscard]] core::u64 byte_digest(std::span<const core::u8> bytes) noexcept {
    core::u64 state = 14'695'981'039'346'656'037ULL;
    for (const core::u8 byte : bytes) {
        state ^= byte;
        state *= 1'099'511'628'211ULL;
    }
    return state;
}

void fill_plate(
    std::span<core::u8> pixels,
    core::u32 width,
    core::u32 height,
    core::u8 seed) {
    RHI_CHECK(pixels.size() ==
              static_cast<std::size_t>(width) * height * 4U);
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * width + x) * 4U;
            pixels[offset] = static_cast<core::u8>(
                static_cast<core::u32>(seed) + x * 17U + y * 13U);
            pixels[offset + 1U] = static_cast<core::u8>(
                static_cast<core::u32>(seed) * 3U + x * 5U + y * 29U);
            pixels[offset + 2U] = static_cast<core::u8>(
                static_cast<core::u32>(seed) * 7U + x + y * 11U);
            pixels[offset + 3U] = 255U;
        }
    }
}

void native_d3d12_upload_pages_span_multiple_4k_plates_and_reuse() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
#if !defined(NDEBUG)
    config.debug = true;
#endif
    // One context makes the second iteration prove that the completed
    // context's pages are reset and reused rather than replaced by another
    // frame slot.
    config.frames_in_flight = 1U;
    auto created_device = rhi::Device::create(config);
    RHI_CHECK(created_device);
    auto device = std::move(created_device).value();

    // DCI 4K is deliberately wider than UHD and each RGBA8 payload is more
    // than twice the old fixed 16 MiB command upload arena.
    constexpr core::u32 width = 4'096U;
    constexpr core::u32 height = 2'160U;
    constexpr core::u32 bytes = width * height * 4U;
    constexpr std::size_t plate_count = 2U;
    static_assert(bytes > 16U * 1024U * 1024U);

    auto desc = target_desc(width, height);
    desc.streaming_upload = true;
    std::array<rhi::Texture, plate_count> plates{};
    std::array<rhi::Readback, plate_count> readbacks{};
    for (std::size_t index = 0U; index < plate_count; ++index) {
        auto created = device.create_texture(desc);
        RHI_CHECK(created);
        plates[index] = std::move(created).value();
        auto readback = device.create_readback(bytes);
        RHI_CHECK(readback);
        readbacks[index] = std::move(readback).value();
    }

    std::vector<core::u8> pixels(bytes);
    for (core::u32 generation = 0U; generation < 2U; ++generation) {
        auto acquired = device.acquire_commands();
        RHI_CHECK(acquired);
        auto list = std::move(acquired).value();
        std::array<core::u64, plate_count> expected{};
        for (std::size_t index = 0U; index < plate_count; ++index) {
            const auto seed = static_cast<core::u8>(
                19U + generation * 71U + static_cast<core::u32>(index) * 37U);
            fill_plate(pixels, width, height, seed);
            expected[index] = byte_digest(pixels);
            RHI_CHECK(list.upload(plates[index], pixels));
            RHI_CHECK(list.download(plates[index], readbacks[index]));
        }
        // The per-context budget is deliberately finite. A third DCI 4K
        // payload exceeds 80 MiB and must be refused without invalidating the
        // two uploads already recorded on this command list.
        fill_plate(pixels, width, height,
                   static_cast<core::u8>(223U - generation));
        RHI_CHECK(!list.upload(plates[0U], pixels));
        {
            auto submitted = device.submit(std::move(list));
            RHI_CHECK(submitted);
            auto work = std::move(submitted).value();
            RHI_CHECK(work.wait());
        }
        for (std::size_t index = 0U; index < plate_count; ++index) {
            const auto copied = readbacks[index].map();
            RHI_CHECK(copied.size() == bytes);
            RHI_CHECK(byte_digest(copied) == expected[index]);
            readbacks[index].unmap();
        }
    }
}

void native_d3d12_cancelled_lists_restore_texture_state() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    config.frames_in_flight = 2U;
#if !defined(NDEBUG)
    config.debug = true;
#endif
    auto created_device = rhi::Device::create(config);
    RHI_CHECK(created_device);
    auto device = std::move(created_device).value();

#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> messages;
    auto* native_device =
        static_cast<ID3D12Device*>(device.native_handle().value);
    if (native_device != nullptr) {
        static_cast<void>(native_device->QueryInterface(
            IID_PPV_ARGS(messages.ReleaseAndGetAddressOf())));
    }
#endif

    constexpr core::u32 width = 64U;
    constexpr core::u32 height = 32U;
    constexpr core::u32 bytes = width * height * 4U;
    auto desc = target_desc(width, height);
    desc.streaming_upload = true;
    auto created_texture = device.create_texture(desc);
    RHI_CHECK(created_texture);
    auto texture = std::move(created_texture).value();
    auto created_readback = device.create_readback(bytes);
    RHI_CHECK(created_readback);
    auto readback = std::move(created_readback).value();

    std::vector<core::u8> pixels(bytes);
    fill_plate(pixels, width, height, 41U);
    {
        auto acquired = device.acquire_commands();
        RHI_CHECK(acquired);
        auto cancelled = std::move(acquired).value();
        RHI_CHECK(cancelled.upload(texture, pixels));
        auto contender_acquired = device.acquire_commands();
        RHI_CHECK(contender_acquired);
        auto contender = std::move(contender_acquired).value();
        RHI_CHECK(!contender.upload(texture, pixels));
        // Destruction deliberately cancels this list. Its speculative
        // COMMON -> COPY_DEST -> PIXEL_SHADER_RESOURCE state must not leak
        // into the next recording because none of those barriers executed.
        // A second recording context is refused while the first owns the
        // texture, rather than inheriting state for barriers that may vanish.
    }

#if defined(_WIN32)
    if (messages) {
        messages->ClearStoredMessages();
    }
#endif

    fill_plate(pixels, width, height, 173U);
    const core::u64 expected = byte_digest(pixels);
    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto list = std::move(acquired).value();
    RHI_CHECK(list.upload(texture, pixels));
    RHI_CHECK(list.download(texture, readback));
    auto submitted = device.submit(std::move(list));
    RHI_CHECK(submitted);
    RHI_CHECK(submitted.value().wait());
    const auto copied = readback.map();
    RHI_CHECK(copied.size() == bytes);
    RHI_CHECK(byte_digest(copied) == expected);
    readback.unmap();

#if defined(_WIN32)
    if (messages) {
        core::u64 serious_messages{};
        const UINT64 count = messages->GetNumStoredMessages();
        for (UINT64 index = 0U; index < count; ++index) {
            SIZE_T size{};
            if (FAILED(messages->GetMessage(index, nullptr, &size)) ||
                size < sizeof(D3D12_MESSAGE)) {
                continue;
            }
            std::vector<core::u8> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(messages->GetMessage(index, message, &size))) {
                continue;
            }
            if (message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION ||
                message->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
                ++serious_messages;
            }
        }
        RHI_CHECK(serious_messages == 0U);
    }
#endif
}

void an_upload_larger_than_its_buffer_is_refused_rather_than_truncated() {
    auto device = open_device();

    auto buffer = device.create_storage_buffer(64U);
    RHI_CHECK(buffer);
    auto list = std::move(device.acquire_commands()).value();

    const std::vector<core::u8> oversized(128U, core::u8{7});
    RHI_CHECK(!list.upload(buffer.value(), oversized));

    const std::vector<core::u8> fits(64U, core::u8{7});
    RHI_CHECK(list.upload(buffer.value(), fits));

    auto work = std::move(device.submit(std::move(list))).value();
    RHI_CHECK(work.wait());
}

void a_streaming_texture_cycles_reusable_upload_storage() {
    auto device = open_device();

    constexpr core::u32 width = 16U;
    constexpr core::u32 height = 8U;
    constexpr core::u32 bytes = width * height * 4U;
    constexpr std::size_t frame_count = 3U;

    auto desc = target_desc(width, height);
    desc.streaming_upload = true;
    auto created = device.create_texture(desc);
    RHI_CHECK(created);
    rhi::Texture texture = std::move(created).value();
    RHI_CHECK(texture.desc().streaming_upload);

    std::array<rhi::Readback, frame_count> readbacks{};
    std::array<rhi::SubmittedWork, frame_count> submissions{};

    // Queue more than one replacement without waiting. The retained logical
    // transfer buffer must cycle instead of letting the next map overwrite
    // bytes an earlier submission has not consumed yet.
    for (std::size_t frame = 0U; frame < frame_count; ++frame) {
        auto readback = device.create_readback(bytes);
        RHI_CHECK(readback);
        readbacks[frame] = std::move(readback).value();

        std::vector<core::u8> pixels(bytes);
        const auto red = static_cast<core::u8>(31U + frame * 47U);
        const auto green = static_cast<core::u8>(201U - frame * 53U);
        for (std::size_t index = 0U; index < pixels.size(); index += 4U) {
            pixels[index] = red;
            pixels[index + 1U] = green;
            pixels[index + 2U] = static_cast<core::u8>(frame);
            pixels[index + 3U] = 255U;
        }

        auto commands = device.acquire_commands();
        RHI_CHECK(commands);
        auto list = std::move(commands).value();
        RHI_CHECK(list.upload(texture, pixels));
        RHI_CHECK(list.download(texture, readbacks[frame]));
        auto submitted = device.submit(std::move(list));
        RHI_CHECK(submitted);
        submissions[frame] = std::move(submitted).value();
    }

    // Moving after the staging resource has been allocated transfers both
    // owned handles; destruction of the moved-from texture must release none.
    rhi::Texture moved = std::move(texture);
    RHI_CHECK(moved);
    RHI_CHECK(!texture);

    for (std::size_t frame = 0U; frame < frame_count; ++frame) {
        RHI_CHECK(submissions[frame].wait());
        const auto pixels = readbacks[frame].map();
        RHI_CHECK(pixels.size() == bytes);
        const auto red = static_cast<core::u8>(31U + frame * 47U);
        const auto green = static_cast<core::u8>(201U - frame * 53U);
        for (std::size_t index = 0U; index < pixels.size(); index += 4U) {
            RHI_CHECK(pixels[index] == red);
            RHI_CHECK(pixels[index + 1U] == green);
            RHI_CHECK(pixels[index + 2U] == static_cast<core::u8>(frame));
            RHI_CHECK(pixels[index + 3U] == 255U);
        }
        readbacks[frame].unmap();
    }

    // Move-assignment releases the destination's old texture/staging pair and
    // takes ownership of the streaming pair exactly once.
    auto replacement = device.create_texture(target_desc(4U, 4U));
    RHI_CHECK(replacement);
    rhi::Texture assigned = std::move(replacement).value();
    assigned = std::move(moved);
    RHI_CHECK(assigned);
    RHI_CHECK(assigned.desc().streaming_upload);
    RHI_CHECK(!moved);
    device.wait_idle();
}

// A pipeline needs bytecode this build may not have for the device it found.
// The seam has to say which of those two it is, because "no DXIL compiler on
// the build machine" and "the GPU refused a valid shader" are different
// problems with different fixes.
void a_pipeline_reports_an_unsupported_shader_format_distinctly() {
    auto device = open_device();

    const std::array<core::u8, 4U> nonsense{core::u8{1}, core::u8{2}, core::u8{3}, core::u8{4}};

    rhi::ShaderFormat unsupported = rhi::ShaderFormat::Msl;
    for (const auto candidate :
         {rhi::ShaderFormat::Msl,
          rhi::ShaderFormat::SpirV,
          rhi::ShaderFormat::Dxil,
          rhi::ShaderFormat::Dxbc}) {
        if (!device.supports(candidate)) {
            unsupported = candidate;
            break;
        }
    }
    RHI_CHECK(!device.supports(unsupported));

    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = unsupported;
    vertex.bytecode = nonsense;
    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = unsupported;
    fragment.bytecode = nonsense;

    auto pipeline = device.create_pipeline(
        vertex, fragment, rhi::TextureFormat::Rgba8Unorm, true);
    RHI_CHECK(!pipeline);
    RHI_CHECK(pipeline.error() == rhi::RhiError::ShaderFormatUnsupported);

    // Empty bytecode is a caller mistake, not a device capability question.
    rhi::ShaderDesc empty_vertex = vertex;
    empty_vertex.format = rhi::ShaderFormat::Dxil;
    empty_vertex.bytecode = {};
    auto invalid = device.create_pipeline(
        empty_vertex, fragment, rhi::TextureFormat::Rgba8Unorm, true);
    RHI_CHECK(!invalid);
    RHI_CHECK(invalid.error() == rhi::RhiError::InvalidConfiguration);

    // Blend equations are an enum now, not an alpha-blend bit. Unknown values
    // are rejected before a backend ever sees partially initialised factors.
    auto invalid_blend = device.create_pipeline(
        vertex,
        fragment,
        rhi::TextureFormat::Rgba8Unorm,
        static_cast<rhi::ColorBlend>(255U));
    RHI_CHECK(!invalid_blend);
    RHI_CHECK(invalid_blend.error() == rhi::RhiError::InvalidConfiguration);
}

void one_asset_publication_epoch_is_latched_per_recording() {
    auto device = open_device();
    {
        auto acquired = device.acquire_commands();
        RHI_CHECK(acquired.has_value());
        auto commands = std::move(acquired).value();
        // Zero is the explicit unlatched sentinel and cannot become a fence.
        RHI_CHECK(commands.latch_asset_publication_epoch(0U) == 0U);
        RHI_CHECK(commands.latch_asset_publication_epoch(41U) == 41U);
        // A later renderer in this same command list keeps the first epoch.
        RHI_CHECK(commands.latch_asset_publication_epoch(99U) == 41U);
        RHI_CHECK(commands.latch_asset_publication_epoch(3U, rhi::AssetPublicationDomain::Environments) == 3U);
        RHI_CHECK(commands.latch_asset_publication_epoch(17U, rhi::AssetPublicationDomain::Probes) == 17U);
        RHI_CHECK(commands.latch_asset_publication_epoch(100U, rhi::AssetPublicationDomain::Probes) == 17U);

        rhi::CommandList moved = std::move(commands);
        RHI_CHECK(moved.latch_asset_publication_epoch(123U) == 41U);
        RHI_CHECK(moved.latch_asset_publication_epoch(9U, rhi::AssetPublicationDomain::Environments) == 3U);
        RHI_CHECK(moved.latch_asset_publication_epoch(9U, rhi::AssetPublicationDomain::Probes) == 17U);
        RHI_CHECK(commands.latch_asset_publication_epoch(7U) == 7U);
    }

    // A newly acquired recording starts unlatched and accepts current state.
    auto next = device.acquire_commands();
    RHI_CHECK(next.has_value());
    auto commands = std::move(next).value();
    RHI_CHECK(commands.latch_asset_publication_epoch(99U) == 99U);
}

void recording_tokens_resolve_submit_or_cancel_without_callbacks() {
    auto device = open_device();
    rhi::CommandRecordingToken canceled;
    {
        auto acquired = device.acquire_commands();
        RHI_CHECK(acquired.has_value());
        auto commands = std::move(acquired).value();
        canceled = commands.recording_token();
        RHI_CHECK(canceled);
        RHI_CHECK(
            canceled.outcome() == rhi::CommandRecordingOutcome::Pending);

        rhi::CommandList moved = std::move(commands);
        const auto same = moved.recording_token();
        RHI_CHECK(same.same_recording(canceled));
        // Scope exit cancels the still-live recording. Both retained values
        // remain queryable without pointing back into the CommandList.
    }
    RHI_CHECK(
        canceled.outcome() == rhi::CommandRecordingOutcome::Canceled);

    auto target = device.create_texture(target_desc(4U, 4U));
    RHI_CHECK(target);
    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    auto submitted_token = commands.recording_token();
    RHI_CHECK(commands.clear(
        target.value(), rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}));
    auto submitted = device.submit(std::move(commands));
    RHI_CHECK(submitted);
    RHI_CHECK(
        submitted_token.outcome()
        == rhi::CommandRecordingOutcome::Submitted);
    RHI_CHECK(std::move(submitted).value().wait());
    canceled = {};
    submitted_token = {};

    // More than the fixed ledger capacity, sequentially. A slot becomes
    // reusable only after CommandList and the final copied receipt both
    // release it; otherwise this loop deterministically exhausts the ledger.
    for (core::u32 recording = 0U; recording < 1'100U; ++recording) {
        rhi::CommandRecordingToken receipt;
        {
            auto next = device.acquire_commands();
            RHI_CHECK(next.has_value());
            auto list = std::move(next).value();
            receipt = list.recording_token();
            RHI_CHECK(receipt);
        }
        RHI_CHECK(
            receipt.outcome() == rhi::CommandRecordingOutcome::Canceled);
        receipt = {};
    }

    // Exhaust the process ledger with completed receipts. Once this command
    // observes exhaustion it must stay receipt-less for its whole recording,
    // even if another slot becomes free between mapped views.
    std::array<rhi::CommandRecordingToken, 1'024U> held{};
    for (auto& receipt : held) {
        auto next = device.acquire_commands();
        RHI_CHECK(next.has_value());
        auto list = std::move(next).value();
        receipt = list.recording_token();
        RHI_CHECK(receipt);
    }
    auto exhausted = device.acquire_commands();
    RHI_CHECK(exhausted.has_value());
    auto exhausted_list = std::move(exhausted).value();
    RHI_CHECK(!exhausted_list.recording_token());
    held.back() = {};
    RHI_CHECK(!exhausted_list.recording_token());
    exhausted_list = {};

    auto fresh = device.acquire_commands();
    RHI_CHECK(fresh.has_value());
    auto fresh_list = std::move(fresh).value();
    RHI_CHECK(fresh_list.recording_token());
}

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)

[[nodiscard]] rhi::ShaderDesc compute_shader(
    std::span<const core::u8> bytecode,
    core::u32 samplers,
    core::u32 storage_buffers,
    core::u32 storage_textures,
    core::u32 uniforms) {
    rhi::ShaderDesc desc{};
    desc.stage = rhi::ShaderStage::Compute;
    desc.format = rhi::ShaderFormat::Dxil;
    desc.bytecode = bytecode;
    desc.sampler_count = samplers;
    desc.storage_buffer_count = storage_buffers;
    desc.storage_texture_count = storage_textures;
    desc.uniform_buffer_count = uniforms;
    return desc;
}

[[nodiscard]] std::span<const core::u8> fill_bytecode() {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_compute_fill_comp_dxil),
        sizeof(mediaserver_compute_fill_comp_dxil)};
}

[[nodiscard]] std::span<const core::u8> gather_bytecode() {
    return std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_compute_gather_comp_dxil),
        sizeof(mediaserver_compute_gather_comp_dxil)};
}

// A storage texture the fill shader can write and the gather shader can read.
// R32Uint on purpose: an integer format makes every expected value exact, so
// the assertions below compare the whole readback rather than a tolerance.
[[nodiscard]] rhi::TextureDesc storage_desc(
    core::u32 width, core::u32 height) {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::R32Uint;
    desc.storage_write = true;
    desc.storage_read = true;
    return desc;
}

struct FillParameters final {
    core::u32 width{};
    core::u32 height{};
    core::u32 seed{};
    core::u32 padding{};
};

struct GatherParameters final {
    core::u32 width{};
    core::u32 height{};
    core::u32 factor{};
    core::u32 padding{};
};

[[nodiscard]] std::span<const core::u8> as_bytes(const void* value,
                                                 std::size_t bytes) {
    return std::span<const core::u8>{
        static_cast<const core::u8*>(value), bytes};
}

// Whole thread groups covering `extent`, matching the shaders' numthreads(8,8).
// The shaders discard the threads this rounds up past the edge.
[[nodiscard]] core::u32 groups_for(core::u32 extent) {
    return (extent + 7U) / 8U;
}

[[nodiscard]] std::vector<core::u32> read_uints(
    rhi::Readback& readback, core::u32 count) {
    const auto mapped = readback.map();
    std::vector<core::u32> values;
    if (mapped.size() < static_cast<std::size_t>(count) * 4U) {
        readback.unmap();
        return values;
    }
    values.resize(count);
    std::memcpy(values.data(), mapped.data(),
                static_cast<std::size_t>(count) * 4U);
    readback.unmap();
    return values;
}

// The first proof that a dispatch runs at all: a known pattern in, the same
// pattern out, byte for byte. A capability flag that claims compute has to be
// backed by exactly this, which is why it is asserted here rather than
// inferred from a pipeline that merely created.
void native_d3d12_a_dispatch_writes_a_storage_texture_exactly() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
#if !defined(NDEBUG)
    config.debug = true;
#endif
    auto created_device = rhi::Device::create(config);
    RHI_CHECK(created_device);
    auto device = std::move(created_device).value();
    RHI_CHECK(device.capabilities().compute);
    RHI_CHECK(device.capabilities().storage_images);

    constexpr core::u32 width = 16U;
    constexpr core::u32 height = 8U;
    constexpr core::u32 texels = width * height;
    constexpr core::u32 seed = 1'000U;

    auto pipeline = device.create_compute_pipeline(
        compute_shader(fill_bytecode(), 0U, 0U, 1U, 1U));
    RHI_CHECK(pipeline);
    auto target = device.create_texture(storage_desc(width, height));
    RHI_CHECK(target);
    auto readback = device.create_readback(texels * 4U);
    RHI_CHECK(readback);

    const FillParameters parameters{width, height, seed, 0U};
    const std::array<rhi::StorageTexture, 1U> bindings{
        rhi::StorageTexture{&target.value(), 0U, 0U, true}};

    rhi::ComputeDispatch work{};
    work.pipeline = &pipeline.value();
    work.storage_textures = bindings;
    work.uniform = as_bytes(&parameters, sizeof(parameters));
    work.groups_x = groups_for(width);
    work.groups_y = groups_for(height);

    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();
    RHI_CHECK(commands.dispatch(work));
    RHI_CHECK(commands.download(target.value(), readback.value()));
    auto submitted = device.submit(std::move(commands));
    RHI_CHECK(submitted);
    RHI_CHECK(std::move(submitted).value().wait());

    const auto values = read_uints(readback.value(), texels);
    RHI_CHECK(values.size() == texels);
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            RHI_CHECK(values[(y * width) + x] == seed + (y * width) + x);
        }
    }

    // Identical work twice must produce identical bits. Two render nodes run
    // the same dispatch on the same replicated state and their walls abut, so
    // a result that varied between runs would seam.
    auto repeat_readback = device.create_readback(texels * 4U);
    RHI_CHECK(repeat_readback);
    auto repeat_acquired = device.acquire_commands();
    RHI_CHECK(repeat_acquired);
    auto repeat = std::move(repeat_acquired).value();
    RHI_CHECK(repeat.dispatch(work));
    RHI_CHECK(repeat.download(target.value(), repeat_readback.value()));
    auto repeat_submitted = device.submit(std::move(repeat));
    RHI_CHECK(repeat_submitted);
    RHI_CHECK(std::move(repeat_submitted).value().wait());
    RHI_CHECK(read_uints(repeat_readback.value(), texels) == values);
}

// Two dispatches on one list where the second reads what the first wrote.
// Without the UAV barrier between them the second reads whatever the first had
// reached, which on a real GPU is neither empty nor correct - and would pass a
// test that only checked the values were not zero.
void native_d3d12_a_dependent_dispatch_reads_what_the_first_wrote() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
#if !defined(NDEBUG)
    config.debug = true;
#endif
    auto created_device = rhi::Device::create(config);
    RHI_CHECK(created_device);
    auto device = std::move(created_device).value();

    constexpr core::u32 width = 16U;
    constexpr core::u32 height = 8U;
    constexpr core::u32 texels = width * height;
    constexpr core::u32 seed = 7U;
    constexpr core::u32 factor = 3U;

    auto fill = device.create_compute_pipeline(
        compute_shader(fill_bytecode(), 0U, 0U, 1U, 1U));
    RHI_CHECK(fill);
    // One sampler, one storage buffer, two storage textures, one uniform: the
    // whole binding surface in a single root signature.
    auto gather = device.create_compute_pipeline(
        compute_shader(gather_bytecode(), 1U, 1U, 2U, 1U));
    RHI_CHECK(gather);

    auto first = device.create_texture(storage_desc(width, height));
    RHI_CHECK(first);
    auto second = device.create_texture(storage_desc(width, height));
    RHI_CHECK(second);
    auto readback = device.create_readback(texels * 4U);
    RHI_CHECK(readback);

    // A sampled source, uploaded rather than computed, so its contribution is
    // known independently of anything a dispatch did.
    auto sampled_desc = target_desc(width, height);
    auto sampled = device.create_texture(sampled_desc);
    RHI_CHECK(sampled);
    std::vector<core::u8> pixels(
        static_cast<std::size_t>(texels) * 4U, static_cast<core::u8>(0U));
    for (core::u32 index = 0U; index < texels; ++index) {
        // Red only; the shader reads .r and rounds the unorm back to an int.
        pixels[static_cast<std::size_t>(index) * 4U] =
            static_cast<core::u8>(index % 256U);
        pixels[(static_cast<std::size_t>(index) * 4U) + 3U] =
            static_cast<core::u8>(255U);
    }
    auto sampler = device.create_sampler(rhi::Filter::Nearest);
    RHI_CHECK(sampler);

    std::vector<core::u8> weight_bytes(
        static_cast<std::size_t>(texels) * 4U);
    std::vector<core::u32> weights(texels);
    for (core::u32 index = 0U; index < texels; ++index) {
        weights[index] = index * 2U;
    }
    std::memcpy(weight_bytes.data(), weights.data(), weight_bytes.size());
    auto storage = device.create_storage_buffer(
        static_cast<core::u32>(weight_bytes.size()));
    RHI_CHECK(storage);

    const FillParameters fill_parameters{width, height, seed, 0U};
    const GatherParameters gather_parameters{width, height, factor, 0U};
    const std::array<rhi::StorageTexture, 1U> fill_bindings{
        rhi::StorageTexture{&first.value(), 0U, 0U, true}};
    // The first texture is read here and written there, in one recording. That
    // is the dependency the barrier exists for, and declaring it read-only is
    // what the backend checks against the texture's own storage flags.
    const std::array<rhi::StorageTexture, 2U> gather_bindings{
        rhi::StorageTexture{&first.value(), 0U, 0U, false},
        rhi::StorageTexture{&second.value(), 0U, 0U, true}};
    const std::array<rhi::SampledTexture, 1U> gather_sources{
        rhi::SampledTexture{&sampled.value(), &sampler.value()}};
    const std::array<const rhi::StorageBuffer*, 1U> gather_buffers{
        &storage.value()};

    rhi::ComputeDispatch fill_work{};
    fill_work.pipeline = &fill.value();
    fill_work.storage_textures = fill_bindings;
    fill_work.uniform = as_bytes(&fill_parameters, sizeof(fill_parameters));
    fill_work.groups_x = groups_for(width);
    fill_work.groups_y = groups_for(height);

    rhi::ComputeDispatch gather_work{};
    gather_work.pipeline = &gather.value();
    gather_work.storage_textures = gather_bindings;
    gather_work.sampled_textures = gather_sources;
    gather_work.storage_buffers = gather_buffers;
    gather_work.uniform =
        as_bytes(&gather_parameters, sizeof(gather_parameters));
    gather_work.groups_x = groups_for(width);
    gather_work.groups_y = groups_for(height);

    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();
    RHI_CHECK(commands.upload(sampled.value(), pixels));
    RHI_CHECK(commands.upload(storage.value(), weight_bytes));
    RHI_CHECK(commands.dispatch(fill_work));
    RHI_CHECK(commands.dispatch(gather_work));
    RHI_CHECK(commands.download(second.value(), readback.value()));
    auto submitted = device.submit(std::move(commands));
    RHI_CHECK(submitted);
    RHI_CHECK(std::move(submitted).value().wait());

    const auto values = read_uints(readback.value(), texels);
    RHI_CHECK(values.size() == texels);
    for (core::u32 index = 0U; index < texels; ++index) {
        const core::u32 written = seed + index;
        const core::u32 expected =
            (written * factor) + weights[index] + (index % 256U);
        RHI_CHECK(values[index] == expected);
    }
}

// The journal covers dispatches exactly as it covers draws. A cancelled list
// that transitioned a texture to UNORDERED_ACCESS must leave it as it found
// it, or the next recording issues a barrier from a state the resource is not
// in - which the debug layer reports and a release build does not.
void native_d3d12_a_cancelled_dispatch_restores_texture_state() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    config.frames_in_flight = 2U;
#if !defined(NDEBUG)
    config.debug = true;
#endif
    auto created_device = rhi::Device::create(config);
    RHI_CHECK(created_device);
    auto device = std::move(created_device).value();

#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> messages;
    auto* native_device =
        static_cast<ID3D12Device*>(device.native_handle().value);
    if (native_device != nullptr) {
        static_cast<void>(native_device->QueryInterface(
            IID_PPV_ARGS(messages.ReleaseAndGetAddressOf())));
    }
#endif

    constexpr core::u32 width = 16U;
    constexpr core::u32 height = 8U;
    constexpr core::u32 texels = width * height;
    constexpr core::u32 seed = 500U;

    auto pipeline = device.create_compute_pipeline(
        compute_shader(fill_bytecode(), 0U, 0U, 1U, 1U));
    RHI_CHECK(pipeline);
    auto target = device.create_texture(storage_desc(width, height));
    RHI_CHECK(target);
    auto readback = device.create_readback(texels * 4U);
    RHI_CHECK(readback);

    const FillParameters parameters{width, height, seed, 0U};
    const std::array<rhi::StorageTexture, 1U> bindings{
        rhi::StorageTexture{&target.value(), 0U, 0U, true}};
    rhi::ComputeDispatch work{};
    work.pipeline = &pipeline.value();
    work.storage_textures = bindings;
    work.uniform = as_bytes(&parameters, sizeof(parameters));
    work.groups_x = groups_for(width);
    work.groups_y = groups_for(height);

    {
        auto acquired = device.acquire_commands();
        RHI_CHECK(acquired);
        auto cancelled = std::move(acquired).value();
        RHI_CHECK(cancelled.dispatch(work));
        // A second recording may not touch a texture the first has claimed.
        auto contender_acquired = device.acquire_commands();
        RHI_CHECK(contender_acquired);
        auto contender = std::move(contender_acquired).value();
        RHI_CHECK(!contender.dispatch(work));
        // Scope exit cancels both. The speculative
        // COMMON -> UNORDERED_ACCESS must not survive them.
    }

    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();
    RHI_CHECK(commands.dispatch(work));
    RHI_CHECK(commands.download(target.value(), readback.value()));
    auto submitted = device.submit(std::move(commands));
    RHI_CHECK(submitted);
    RHI_CHECK(std::move(submitted).value().wait());

    const auto values = read_uints(readback.value(), texels);
    RHI_CHECK(values.size() == texels);
    for (core::u32 index = 0U; index < texels; ++index) {
        RHI_CHECK(values[index] == seed + index);
    }

#if defined(_WIN32)
    if (messages) {
        RHI_CHECK(messages->GetNumStoredMessages() == 0U);
    }
#endif
}

// Every refusal a dispatch can make, and none of them a wait or an allocation.
// The frame path polls; a compute pass that blocked on a full descriptor heap
// would miss a boundary rather than skip a frame, which is worse.
void native_d3d12_a_malformed_dispatch_is_refused_rather_than_waited_on() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
#if !defined(NDEBUG)
    config.debug = true;
#endif
    auto created_device = rhi::Device::create(config);
    RHI_CHECK(created_device);
    auto device = std::move(created_device).value();

    constexpr core::u32 width = 8U;
    constexpr core::u32 height = 8U;

    // A vertex shader is not a compute shader, whatever its bytecode says.
    auto wrong_stage = device.create_compute_pipeline(
        compute_shader(fill_bytecode(), 0U, 0U, 1U, 1U));
    RHI_CHECK(wrong_stage);
    auto misnamed = compute_shader(fill_bytecode(), 0U, 0U, 1U, 1U);
    misnamed.stage = rhi::ShaderStage::Vertex;
    auto refused_stage = device.create_compute_pipeline(misnamed);
    RHI_CHECK(!refused_stage);
    RHI_CHECK(refused_stage.error() == rhi::RhiError::InvalidConfiguration);

    auto empty = compute_shader({}, 0U, 0U, 1U, 1U);
    auto refused_empty = device.create_compute_pipeline(empty);
    RHI_CHECK(!refused_empty);
    RHI_CHECK(refused_empty.error() == rhi::RhiError::InvalidConfiguration);

    auto target = device.create_texture(storage_desc(width, height));
    RHI_CHECK(target);
    const FillParameters parameters{width, height, 1U, 0U};
    const std::array<rhi::StorageTexture, 1U> bindings{
        rhi::StorageTexture{&target.value(), 0U, 0U, true}};

    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();

    rhi::ComputeDispatch work{};
    work.pipeline = &wrong_stage.value();
    work.storage_textures = bindings;
    work.uniform = as_bytes(&parameters, sizeof(parameters));
    work.groups_x = groups_for(width);
    work.groups_y = groups_for(height);

    // No pipeline at all.
    rhi::ComputeDispatch headless = work;
    headless.pipeline = nullptr;
    RHI_CHECK(!commands.dispatch(headless));

    // A group count of zero is a caller that computed it wrong, not a request
    // to do nothing.
    rhi::ComputeDispatch empty_groups = work;
    empty_groups.groups_x = 0U;
    RHI_CHECK(!commands.dispatch(empty_groups));

    // Fewer bindings than the root signature declares.
    rhi::ComputeDispatch unbound = work;
    unbound.storage_textures = {};
    RHI_CHECK(!commands.dispatch(unbound));

    // More than the resource has.
    const std::array<rhi::StorageTexture, 1U> past_the_end{
        rhi::StorageTexture{&target.value(), 4U, 0U, true}};
    rhi::ComputeDispatch out_of_range = work;
    out_of_range.storage_textures = past_the_end;
    RHI_CHECK(!commands.dispatch(out_of_range));

    const std::array<rhi::StorageTexture, 1U> past_the_layers{
        rhi::StorageTexture{&target.value(), 0U, 3U, true}};
    rhi::ComputeDispatch out_of_layers = work;
    out_of_layers.storage_textures = past_the_layers;
    RHI_CHECK(!commands.dispatch(out_of_layers));

    // Writing a texture that never asked to be written.
    auto read_only = device.create_texture(target_desc(width, height));
    RHI_CHECK(read_only);
    const std::array<rhi::StorageTexture, 1U> not_writable{
        rhi::StorageTexture{&read_only.value(), 0U, 0U, true}};
    rhi::ComputeDispatch unwritable = work;
    unwritable.storage_textures = not_writable;
    RHI_CHECK(!commands.dispatch(unwritable));

    // A refusal on the *second* binding must not have transitioned the first.
    // This is the case that proves the check runs before any barrier is
    // written rather than merely before the offending one: the good binding
    // comes first, so a per-binding check would already have recorded it.
    auto second = device.create_texture(storage_desc(width, height));
    RHI_CHECK(second);
    auto pair_pipeline = device.create_compute_pipeline(
        compute_shader(gather_bytecode(), 1U, 1U, 2U, 1U));
    RHI_CHECK(pair_pipeline);
    const std::array<rhi::StorageTexture, 2U> good_then_bad{
        rhi::StorageTexture{&second.value(), 0U, 0U, true},
        rhi::StorageTexture{&read_only.value(), 0U, 0U, true}};
    rhi::ComputeDispatch late_refusal{};
    late_refusal.pipeline = &pair_pipeline.value();
    late_refusal.storage_textures = good_then_bad;
    late_refusal.uniform = as_bytes(&parameters, sizeof(parameters));
    late_refusal.groups_x = groups_for(width);
    late_refusal.groups_y = groups_for(height);
    RHI_CHECK(!commands.dispatch(late_refusal));

    // And after all of that the list is still clean enough to record and
    // submit. A refused dispatch that had left a half-written barrier behind
    // would fail here rather than at the call that was refused, which is the
    // failure this contract exists to prevent.
    RHI_CHECK(commands.dispatch(work));
    auto submitted = device.submit(std::move(commands));
    RHI_CHECK(submitted);
    RHI_CHECK(std::move(submitted).value().wait());
}

// The recording descriptor heap is fixed, and running it out refuses rather
// than waiting for a submission to free it. This is the frame-path rule the
// whole RHI keeps: a compute pass that blocked on a full heap would miss a
// show boundary, which on a wall is worse than skipping the pass.
void native_d3d12_dispatch_descriptor_exhaustion_is_refused() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto created_device = rhi::Device::create(config);
    RHI_CHECK(created_device);
    auto device = std::move(created_device).value();

    constexpr core::u32 width = 8U;
    constexpr core::u32 height = 8U;

    auto pipeline = device.create_compute_pipeline(
        compute_shader(fill_bytecode(), 0U, 0U, 1U, 1U));
    RHI_CHECK(pipeline);
    auto target = device.create_texture(storage_desc(width, height));
    RHI_CHECK(target);

    const FillParameters parameters{width, height, 0U, 0U};
    const std::array<rhi::StorageTexture, 1U> bindings{
        rhi::StorageTexture{&target.value(), 0U, 0U, true}};
    rhi::ComputeDispatch work{};
    work.pipeline = &pipeline.value();
    work.storage_textures = bindings;
    work.uniform = as_bytes(&parameters, sizeof(parameters));
    work.groups_x = groups_for(width);
    work.groups_y = groups_for(height);

    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();

    // One descriptor per dispatch. Well past the heap either way, and the
    // loop is bounded so a heap that never filled fails the test rather than
    // running forever - which is the failure mode a "keep going until it
    // refuses" loop would hide.
    bool refused = false;
    for (core::u32 attempt = 0U; attempt < 200'000U; ++attempt) {
        if (!commands.dispatch(work)) {
            refused = true;
            break;
        }
    }
    RHI_CHECK(refused);
    // Refused, not waited on: the list is still live and still refuses, having
    // neither blocked nor grown a heap under the caller.
    RHI_CHECK(!commands.dispatch(work));

    // A fresh recording has its own heap and works normally, which is what
    // makes the refusal a bounded local condition rather than a broken device.
    commands = {};
    auto next = device.acquire_commands();
    RHI_CHECK(next);
    auto fresh = std::move(next).value();
    RHI_CHECK(fresh.dispatch(work));
    auto submitted = device.submit(std::move(fresh));
    RHI_CHECK(submitted);
    RHI_CHECK(std::move(submitted).value().wait());
}

// A dispatch addressing one mip of one array layer. This is the binding shape
// image-based lighting needs - each prefiltered roughness level is its own mip
// of its own cube face - and a whole-resource UAV cannot express it, which is
// why the subresource is part of the binding rather than a later addition.
void native_d3d12_a_dispatch_addresses_one_mip_of_one_layer() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
#if !defined(NDEBUG)
    config.debug = true;
#endif
    auto created_device = rhi::Device::create(config);
    RHI_CHECK(created_device);
    auto device = std::move(created_device).value();

    constexpr core::u32 width = 16U;
    constexpr core::u32 height = 16U;
    constexpr core::u32 layers = 3U;
    constexpr core::u32 levels = 3U;

    auto desc = storage_desc(width, height);
    desc.array_layers = layers;
    desc.mip_levels = levels;
    auto target = device.create_texture(desc);
    RHI_CHECK(target);

    auto pipeline = device.create_compute_pipeline(
        compute_shader(fill_bytecode(), 0U, 0U, 1U, 1U));
    RHI_CHECK(pipeline);

    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();

    // Every subresource in turn, each its own dispatch sized to that level.
    for (core::u32 layer = 0U; layer < layers; ++layer) {
        for (core::u32 level = 0U; level < levels; ++level) {
            const core::u32 level_width = width >> level;
            const core::u32 level_height = height >> level;
            const FillParameters parameters{
                level_width, level_height, (layer * levels) + level, 0U};
            const std::array<rhi::StorageTexture, 1U> bindings{
                rhi::StorageTexture{&target.value(), level, layer, true}};
            rhi::ComputeDispatch work{};
            work.pipeline = &pipeline.value();
            work.storage_textures = bindings;
            work.uniform = as_bytes(&parameters, sizeof(parameters));
            work.groups_x = groups_for(level_width);
            work.groups_y = groups_for(level_height);
            RHI_CHECK(commands.dispatch(work));
        }
    }
    auto submitted = device.submit(std::move(commands));
    RHI_CHECK(submitted);
    RHI_CHECK(std::move(submitted).value().wait());
}

#else

void native_d3d12_a_dispatch_writes_a_storage_texture_exactly() {}
void native_d3d12_a_dependent_dispatch_reads_what_the_first_wrote() {}
void native_d3d12_a_cancelled_dispatch_restores_texture_state() {}
void native_d3d12_a_malformed_dispatch_is_refused_rather_than_waited_on() {}
void native_d3d12_a_dispatch_addresses_one_mip_of_one_layer() {}
void native_d3d12_dispatch_descriptor_exhaustion_is_refused() {}

#endif

// A backend that cannot dispatch says so distinctly, and its capability flag
// agrees with its entry point. The two are asserted together because a flag
// that drifted from what is callable is exactly the lie this seam exists to
// avoid - and the SDL backend is where that drift would happen first, since
// SDL_GPU itself has a compute API this RHI does not expose.
void a_backend_without_compute_refuses_distinctly() {
    auto device = open_device();
    if (device.capabilities().compute) {
        return;
    }
    // Whatever it can or cannot construct, it reports no storage images
    // either: the flag means bindable and dispatchable now.
    RHI_CHECK(!device.capabilities().storage_images);
    rhi::ShaderDesc desc{};
    desc.stage = rhi::ShaderStage::Compute;
    desc.format = rhi::ShaderFormat::Dxil;
    const std::array<core::u8, 4U> bytes{{1U, 2U, 3U, 4U}};
    desc.bytecode = bytes;
    auto refused = device.create_compute_pipeline(desc);
    RHI_CHECK(!refused);
    // Not InvalidConfiguration: no correction to the request would help.
    RHI_CHECK(refused.error() == rhi::RhiError::OperationUnsupported);
}

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)

// A pipeline that fills whatever it is allowed to rasterize with white.
[[nodiscard]] core::Result<rhi::GraphicsPipeline, rhi::RhiError> solid_pipeline(
    rhi::Device& device, rhi::TextureFormat format) {
    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = rhi::ShaderFormat::Dxil;
    vertex.bytecode = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_solid_vert_dxil),
        sizeof(mediaserver_solid_vert_dxil)};
    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = rhi::ShaderFormat::Dxil;
    fragment.bytecode = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_solid_frag_dxil),
        sizeof(mediaserver_solid_frag_dxil)};
    return device.create_pipeline(
        vertex, fragment, format, rhi::ColorBlend::Disabled);
}

// A viewport rasterizes into its own rectangle and nowhere else, and the clear
// still covers the whole target.
//
// Both halves matter. The first is what lets a padded slice render pay only for
// the apron the scene authored rather than for the largest one the project
// permits. The second is the trap: a caller who assumed the clear was scoped to
// the viewport would find the rest of the target wiped, so the behaviour is
// asserted rather than described.
void a_viewport_draw_covers_its_rectangle_and_nothing_else() {
    auto device = open_device();
    auto pipeline = solid_pipeline(device, rhi::TextureFormat::Rgba8Unorm);
    if (!pipeline) {
        return;
    }

    constexpr core::u32 width = 16U;
    constexpr core::u32 height = 16U;
    constexpr core::u32 bytes = width * height * 4U;

    auto color = device.create_texture(target_desc(width, height));
    RHI_CHECK(color);
    rhi::TextureDesc depth_desc{};
    depth_desc.width = width;
    depth_desc.height = height;
    depth_desc.format = rhi::TextureFormat::Depth32Float;
    depth_desc.depth_target = true;
    auto depth = device.create_texture(depth_desc);
    RHI_CHECK(depth);
    auto readback = device.create_readback(bytes);
    RHI_CHECK(readback);

    const std::array<rhi::DrawBatch, 1U> batches{rhi::DrawBatch{3U, 1U}};
    rhi::PulledDraw draw{};
    draw.pipeline = &pipeline.value();
    draw.batches = batches;
    draw.viewport = rhi::ViewportRect{4U, 2U, 8U, 6U};

    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();
    RHI_CHECK(commands.draw_pulled(
        color.value(), rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, depth.value(),
        0.0F, draw));
    RHI_CHECK(commands.download(color.value(), readback.value()));
    auto submitted = device.submit(std::move(commands));
    RHI_CHECK(submitted);
    RHI_CHECK(std::move(submitted).value().wait());

    const auto pixels = readback.value().map();
    RHI_CHECK(pixels.size() >= bytes);
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            const bool inside = x >= draw.viewport.x
                && x < draw.viewport.x + draw.viewport.width
                && y >= draw.viewport.y
                && y < draw.viewport.y + draw.viewport.height;
            const core::u8 red =
                pixels[((static_cast<std::size_t>(y) * width) + x) * 4U];
            // White inside, and the clear value outside - not last frame's
            // contents, and not white bleeding past the scissor.
            RHI_CHECK(red == (inside ? core::u8{255U} : core::u8{0U}));
        }
    }
    readback.value().unmap();

    // No viewport is the whole target, which is every caller that predates
    // this field and must keep behaving as it did.
    rhi::PulledDraw whole = draw;
    whole.viewport = {};
    auto second_readback = device.create_readback(bytes);
    RHI_CHECK(second_readback);
    auto next = device.acquire_commands();
    RHI_CHECK(next);
    auto more = std::move(next).value();
    RHI_CHECK(more.draw_pulled(
        color.value(), rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, depth.value(),
        0.0F, whole));
    RHI_CHECK(more.download(color.value(), second_readback.value()));
    auto second_submitted = device.submit(std::move(more));
    RHI_CHECK(second_submitted);
    RHI_CHECK(std::move(second_submitted).value().wait());
    const auto covered = second_readback.value().map();
    RHI_CHECK(covered.size() >= bytes);
    for (core::u32 index = 0U; index < width * height; ++index) {
        RHI_CHECK(covered[static_cast<std::size_t>(index) * 4U] == 255U);
    }
    second_readback.value().unmap();
}

// A viewport the target cannot hold is refused, never clamped. Clamping would
// rasterize a different region than the caller computed, and the caller
// computed it from a slice geometry two nodes have to agree on pixel for pixel.
void a_viewport_larger_than_its_target_is_refused() {
    auto device = open_device();
    auto pipeline = solid_pipeline(device, rhi::TextureFormat::Rgba8Unorm);
    if (!pipeline) {
        return;
    }

    constexpr core::u32 width = 16U;
    constexpr core::u32 height = 16U;
    auto color = device.create_texture(target_desc(width, height));
    RHI_CHECK(color);
    rhi::TextureDesc depth_desc{};
    depth_desc.width = width;
    depth_desc.height = height;
    depth_desc.format = rhi::TextureFormat::Depth32Float;
    depth_desc.depth_target = true;
    auto depth = device.create_texture(depth_desc);
    RHI_CHECK(depth);

    const std::array<rhi::DrawBatch, 1U> batches{rhi::DrawBatch{3U, 1U}};
    rhi::PulledDraw draw{};
    draw.pipeline = &pipeline.value();
    draw.batches = batches;

    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();

    const std::array<rhi::ViewportRect, 4U> impossible{{
        // Wider than the target.
        {0U, 0U, width + 1U, height},
        // Taller than the target.
        {0U, 0U, width, height + 1U},
        // Fits, but offset past the edge.
        {1U, 0U, width, height},
        {0U, 1U, width, height},
    }};
    for (const rhi::ViewportRect& region : impossible) {
        rhi::PulledDraw refused = draw;
        refused.viewport = region;
        RHI_CHECK(!commands.draw_pulled(
            color.value(), rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, depth.value(),
            0.0F, refused));
    }
    // Exactly the target is not too large.
    rhi::PulledDraw exact = draw;
    exact.viewport = rhi::ViewportRect{0U, 0U, width, height};
    RHI_CHECK(commands.draw_pulled(
        color.value(), rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, depth.value(),
        0.0F, exact));
    auto submitted = device.submit(std::move(commands));
    RHI_CHECK(submitted);
    RHI_CHECK(std::move(submitted).value().wait());
}


// A wireframe pipeline rasterizes triangle edges and leaves their interiors
// alone. The fullscreen triangle's edges lie on and outside the target's
// border, so a wireframe draw of it covers the middle of the target with
// nothing, where the same draw filled solid covers every pixel.
void a_wireframe_pipeline_draws_edges_and_not_interiors() {
    auto device = open_device();
    if (!device.capabilities().wireframe_fill) {
        return;
    }
    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = rhi::ShaderFormat::Dxil;
    vertex.bytecode = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_solid_vert_dxil),
        sizeof(mediaserver_solid_vert_dxil)};
    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = rhi::ShaderFormat::Dxil;
    fragment.bytecode = std::span<const core::u8>{
        reinterpret_cast<const core::u8*>(mediaserver_solid_frag_dxil),
        sizeof(mediaserver_solid_frag_dxil)};
    auto wireframe = device.create_pipeline(
        vertex, fragment, rhi::TextureFormat::Rgba8Unorm,
        rhi::ColorBlend::Disabled, rhi::DepthState{},
        rhi::RasterState{rhi::FillMode::Wireframe});
    if (!wireframe && wireframe.error() == rhi::RhiError::ShaderFormatUnsupported) {
        return;
    }
    RHI_CHECK(wireframe);
    auto solid = solid_pipeline(device, rhi::TextureFormat::Rgba8Unorm);
    RHI_CHECK(solid);

    constexpr core::u32 width = 16U;
    constexpr core::u32 height = 16U;
    constexpr core::u32 bytes = width * height * 4U;
    auto color = device.create_texture(target_desc(width, height));
    RHI_CHECK(color);
    rhi::TextureDesc depth_desc{};
    depth_desc.width = width;
    depth_desc.height = height;
    depth_desc.format = rhi::TextureFormat::Depth32Float;
    depth_desc.depth_target = true;
    auto depth = device.create_texture(depth_desc);
    RHI_CHECK(depth);
    const std::array<rhi::DrawBatch, 1U> batches{rhi::DrawBatch{3U, 1U}};

    const auto covered = [&](const rhi::GraphicsPipeline& pipeline) {
        auto readback = device.create_readback(bytes);
        RHI_CHECK(readback);
        rhi::PulledDraw draw{};
        draw.pipeline = &pipeline;
        draw.batches = batches;
        auto acquired = device.acquire_commands();
        RHI_CHECK(acquired);
        auto commands = std::move(acquired).value();
        RHI_CHECK(commands.draw_pulled(
            color.value(), rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, depth.value(),
            0.0F, draw));
        RHI_CHECK(commands.download(color.value(), readback.value()));
        auto submitted = device.submit(std::move(commands));
        RHI_CHECK(submitted);
        RHI_CHECK(std::move(submitted).value().wait());
        const auto pixels = readback.value().map();
        RHI_CHECK(pixels.size() >= bytes);
        std::size_t lit = 0U;
        bool middle = false;
        for (core::u32 y = 1U; y + 1U < height; ++y) {
            for (core::u32 x = 1U; x + 1U < width; ++x) {
                const bool white =
                    pixels[((static_cast<std::size_t>(y) * width) + x) * 4U] ==
                    255U;
                lit += white ? 1U : 0U;
                middle = middle || (white && x == width / 2U &&
                                    y == height / 2U);
            }
        }
        readback.value().unmap();
        return std::pair<std::size_t, bool>{lit, middle};
    };
    const auto filled = covered(solid.value());
    RHI_CHECK(filled.first == (width - 2U) * (height - 2U) && filled.second);
    // Inside the border the only edge is the diagonal reaching the far
    // corner: at most a pixel or two, never the interior.
    const auto edges = covered(wireframe.value());
    RHI_CHECK(!edges.second);
    RHI_CHECK(edges.first <= 2U);
}

#else

void a_viewport_draw_covers_its_rectangle_and_nothing_else() {}
void a_viewport_larger_than_its_target_is_refused() {}
void a_wireframe_pipeline_draws_edges_and_not_interiors() {}

#endif

// The crop primitive: an exact sub-rectangle copy, no filter and no scale.
// A padded slice render ends with one of these, and it has to move the pixels
// unchanged - resampling a rectangle already at the right pitch would alter
// the values the whole padded path exists to keep identical to a full-canvas
// render.
void a_region_copy_moves_exact_pixels_and_refuses_the_rest() {
    auto device = open_device();

    constexpr core::u32 width = 8U;
    constexpr core::u32 height = 8U;
    constexpr core::u32 crop = 4U;

    auto source = device.create_texture(target_desc(width, height));
    RHI_CHECK(source);
    auto destination = device.create_texture(target_desc(crop, crop));
    RHI_CHECK(destination);
    auto readback = device.create_readback(crop * crop * 4U);
    RHI_CHECK(readback);

    std::vector<core::u8> pixels(
        static_cast<std::size_t>(width) * height * 4U);
    for (core::u32 index = 0U; index < width * height; ++index) {
        const std::size_t base = static_cast<std::size_t>(index) * 4U;
        pixels[base] = static_cast<core::u8>(index);
        pixels[base + 1U] = static_cast<core::u8>(index * 2U);
        pixels[base + 2U] = static_cast<core::u8>(index * 3U);
        pixels[base + 3U] = 255U;
    }

    const rhi::ViewportRect region{2U, 3U, crop, crop};
    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();
    RHI_CHECK(commands.upload(source.value(), pixels));

    // Everything a crop can get wrong, refused rather than adjusted.
    auto too_small = device.create_texture(target_desc(crop - 1U, crop));
    RHI_CHECK(too_small);
    RHI_CHECK(!commands.copy_region(
        source.value(), region, too_small.value()));
    RHI_CHECK(!commands.copy_region(
        source.value(), rhi::ViewportRect{}, destination.value()));
    RHI_CHECK(!commands.copy_region(
        source.value(), rhi::ViewportRect{width - 1U, 0U, crop, crop},
        destination.value()));
    rhi::TextureDesc other_format = target_desc(crop, crop);
    other_format.format = rhi::TextureFormat::R32Uint;
    auto mismatched = device.create_texture(other_format);
    if (mismatched) {
        RHI_CHECK(!commands.copy_region(
            source.value(), region, mismatched.value()));
    }

    RHI_CHECK(commands.copy_region(
        source.value(), region, destination.value()));
    RHI_CHECK(commands.download(destination.value(), readback.value()));
    auto submitted = device.submit(std::move(commands));
    RHI_CHECK(submitted);
    RHI_CHECK(std::move(submitted).value().wait());

    const auto copied = readback.value().map();
    RHI_CHECK(copied.size() >= static_cast<std::size_t>(crop) * crop * 4U);
    for (core::u32 y = 0U; y < crop; ++y) {
        for (core::u32 x = 0U; x < crop; ++x) {
            const core::u32 source_index =
                ((y + region.y) * width) + (x + region.x);
            const std::size_t base =
                (static_cast<std::size_t>(y) * crop + x) * 4U;
            RHI_CHECK(copied[base] == static_cast<core::u8>(source_index));
            RHI_CHECK(
                copied[base + 1U] == static_cast<core::u8>(source_index * 2U));
            RHI_CHECK(
                copied[base + 2U] == static_cast<core::u8>(source_index * 3U));
        }
    }
    readback.value().unmap();
}

// An explicitly requested transition goes through the same journal every
// implicit one uses.
//
// That is the whole contract: a caller planning a frame ahead gets to own its
// barriers without opening a second path to resource state. A second path is
// how the journal stops being complete, and an incomplete journal means a
// cancelled list leaves a texture in a state nothing recorded.
void native_d3d12_an_explicit_transition_is_journalled_and_rolls_back() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    config.frames_in_flight = 2U;
#if !defined(NDEBUG)
    config.debug = true;
#endif
    auto created_device = rhi::Device::create(config);
    RHI_CHECK(created_device);
    auto device = std::move(created_device).value();
    RHI_CHECK(device.capabilities().explicit_barriers);

#if defined(_WIN32)
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> messages;
    auto* native_device =
        static_cast<ID3D12Device*>(device.native_handle().value);
    if (native_device != nullptr) {
        static_cast<void>(native_device->QueryInterface(
            IID_PPV_ARGS(messages.ReleaseAndGetAddressOf())));
    }
#endif

    constexpr core::u32 width = 16U;
    constexpr core::u32 height = 16U;
    constexpr core::u32 bytes = width * height * 4U;
    auto target = device.create_texture(target_desc(width, height));
    RHI_CHECK(target);
    auto readback = device.create_readback(bytes);
    RHI_CHECK(readback);

    {
        auto acquired = device.acquire_commands();
        RHI_CHECK(acquired);
        auto cancelled = std::move(acquired).value();
        RHI_CHECK(cancelled.transition(
            target.value(), rhi::ResourceState::CopySource));
        // Claimed by that recording, so a second one may not touch it - the
        // same fencing an implicit transition gets.
        auto contender_acquired = device.acquire_commands();
        RHI_CHECK(contender_acquired);
        auto contender = std::move(contender_acquired).value();
        RHI_CHECK(!contender.transition(
            target.value(), rhi::ResourceState::ColorTarget));
        // Scope exit cancels both. The speculative COPY_SOURCE must not
        // survive them, or the next recording issues a barrier from a state
        // the resource is not in.
    }

    // If the rollback did not happen, this clear transitions from a state the
    // texture does not hold and the debug layer says so.
    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();
    RHI_CHECK(commands.clear(
        target.value(), rhi::Color{0.25F, 0.5F, 0.75F, 1.0F}));
    RHI_CHECK(commands.download(target.value(), readback.value()));
    auto submitted = device.submit(std::move(commands));
    RHI_CHECK(submitted);
    RHI_CHECK(std::move(submitted).value().wait());

    const auto pixels = readback.value().map();
    RHI_CHECK(pixels.size() >= bytes);
    RHI_CHECK(pixels[0U] == 64U);
    readback.value().unmap();

#if defined(_WIN32)
    // Errors and corruption only. The debug layer also emits performance
    // advice - a clear whose colour differs from the one the texture was
    // created with is "typically slower", and this test picks a distinctive
    // colour on purpose so the readback proves the clear happened. Failing on
    // that would make the test about the clear value rather than about the
    // rollback it exists to check.
    if (messages) {
        UINT64 serious = 0U;
        const auto stored = messages->GetNumStoredMessages();
        for (UINT64 index = 0U; index < stored; ++index) {
            SIZE_T length = 0U;
            static_cast<void>(messages->GetMessage(index, nullptr, &length));
            std::vector<char> buffer(length);
            auto* record = reinterpret_cast<D3D12_MESSAGE*>(buffer.data());
            if (FAILED(messages->GetMessage(index, record, &length))) {
                continue;
            }
            if (record->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION
                || record->Severity == D3D12_MESSAGE_SEVERITY_ERROR) {
                std::cerr << "d3d12: " << record->pDescription << '\n';
                ++serious;
            }
        }
        RHI_CHECK(serious == 0U);
    }
#endif
}

// Asking for the state a texture already holds records nothing and succeeds,
// and an explicit transition composes with the implicit one a draw does.
//
// Both matter to a render graph. A compiled plan transitions a resource before
// the pass that uses it, and then the pass body transitions it again on its own
// account; if the second were an error, or if the pair produced a different
// image than the pass alone, no plan could be executed over the existing draw
// entry points.
void native_d3d12_a_redundant_transition_is_a_no_op() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
#if !defined(NDEBUG)
    config.debug = true;
#endif
    auto created_device = rhi::Device::create(config);
    RHI_CHECK(created_device);
    auto device = std::move(created_device).value();

    constexpr core::u32 width = 8U;
    constexpr core::u32 height = 8U;
    constexpr core::u32 bytes = width * height * 4U;
    auto target = device.create_texture(target_desc(width, height));
    RHI_CHECK(target);
    auto readback = device.create_readback(bytes);
    RHI_CHECK(readback);

    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();
    // Three times into the same state, then a clear that wants that state
    // anyway.
    RHI_CHECK(commands.transition(
        target.value(), rhi::ResourceState::ColorTarget));
    RHI_CHECK(commands.transition(
        target.value(), rhi::ResourceState::ColorTarget));
    RHI_CHECK(commands.transition(
        target.value(), rhi::ResourceState::ColorTarget));
    RHI_CHECK(commands.clear(
        target.value(), rhi::Color{1.0F, 0.0F, 0.0F, 1.0F}));
    RHI_CHECK(commands.download(target.value(), readback.value()));
    auto submitted = device.submit(std::move(commands));
    RHI_CHECK(submitted);
    RHI_CHECK(std::move(submitted).value().wait());

    const auto pixels = readback.value().map();
    RHI_CHECK(pixels.size() >= bytes);
    RHI_CHECK(pixels[0U] == 255U);
    RHI_CHECK(pixels[1U] == 0U);
    readback.value().unmap();
}

// A backend that owns its barriers refuses to hand them over, and its
// capability flag agrees with its entry point.
void a_backend_without_explicit_barriers_refuses() {
    auto device = open_device();
    if (device.capabilities().explicit_barriers) {
        return;
    }
    auto target = device.create_texture(target_desc(8U, 8U));
    RHI_CHECK(target);
    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();
    RHI_CHECK(!commands.transition(
        target.value(), rhi::ResourceState::ColorTarget));
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 28U> cases{{
    {"an explicit transition is journalled and rolls back",
     &native_d3d12_an_explicit_transition_is_journalled_and_rolls_back},
    {"a redundant transition is a no-op",
     &native_d3d12_a_redundant_transition_is_a_no_op},
    {"a backend without explicit barriers refuses",
     &a_backend_without_explicit_barriers_refuses},
    {"a viewport draw covers its rectangle and nothing else",
     &a_viewport_draw_covers_its_rectangle_and_nothing_else},
    {"a wireframe pipeline draws edges and not interiors",
     &a_wireframe_pipeline_draws_edges_and_not_interiors},
    {"a viewport larger than its target is refused",
     &a_viewport_larger_than_its_target_is_refused},
    {"a region copy moves exact pixels and refuses the rest",
     &a_region_copy_moves_exact_pixels_and_refuses_the_rest},
    {"dispatch descriptor exhaustion is refused",
     &native_d3d12_dispatch_descriptor_exhaustion_is_refused},
    {"a dispatch writes a storage texture exactly",
     &native_d3d12_a_dispatch_writes_a_storage_texture_exactly},
    {"a dependent dispatch reads what the first wrote",
     &native_d3d12_a_dependent_dispatch_reads_what_the_first_wrote},
    {"a cancelled dispatch restores texture state",
     &native_d3d12_a_cancelled_dispatch_restores_texture_state},
    {"a malformed dispatch is refused rather than waited on",
     &native_d3d12_a_malformed_dispatch_is_refused_rather_than_waited_on},
    {"a dispatch addresses one mip of one layer",
     &native_d3d12_a_dispatch_addresses_one_mip_of_one_layer},
    {"a backend without compute refuses distinctly",
     &a_backend_without_compute_refuses_distinctly},
    {"an offscreen target clears and reads back exactly",
     &an_offscreen_target_clears_and_reads_back_exactly},
    {"a blit moves pixels between targets without the CPU",
     &a_blit_moves_pixels_between_targets_without_the_cpu},
    {"resource handles release exactly once and survive moves",
     &resource_handles_release_exactly_once_and_survive_moves},
    {"invalid descriptions are refused before the backend sees them",
     &invalid_descriptions_are_refused_before_the_backend_sees_them},
    {"capabilities and native handles are typed and immutable",
     &capabilities_and_native_handles_are_typed_and_immutable},
    {"native D3D12 is nonblocking and preserves linear float pixels",
     &native_d3d12_is_nonblocking_and_preserves_linear_float_pixels},
    {"native D3D12 reports a lost device", &native_d3d12_reports_a_lost_device},
    {"native D3D12 upload pages span multiple 4K plates and reuse",
     &native_d3d12_upload_pages_span_multiple_4k_plates_and_reuse},
    {"native D3D12 cancelled lists restore texture state",
     &native_d3d12_cancelled_lists_restore_texture_state},
    {"an upload larger than its buffer is refused rather than truncated",
     &an_upload_larger_than_its_buffer_is_refused_rather_than_truncated},
    {"a streaming texture cycles reusable upload storage",
     &a_streaming_texture_cycles_reusable_upload_storage},
    {"one asset publication epoch is latched per recording",
     &one_asset_publication_epoch_is_latched_per_recording},
    {"recording tokens resolve submit or cancel without callbacks",
     &recording_tokens_resolve_submit_or_cancel_without_callbacks},
    {"a pipeline reports an unsupported shader format distinctly",
     &a_pipeline_reports_an_unsupported_shader_format_distinctly},
}};

} // namespace

int main() {
    // A build machine with no GPU is a normal build machine. Skipping loudly
    // is honest; failing would train people to ignore this suite.
    if (!rhi::Device::available()) {
        std::cout << "[skip] no GPU backend on this machine; rhi suite skipped\n";
        return 0;
    }

    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
