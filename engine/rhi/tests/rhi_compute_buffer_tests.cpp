#include <mediaserver/rhi/device.hpp>

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
#include <mediaserver/shaders/buffer_add_probe_comp_dxil.h>
#include <mediaserver/shaders/buffer_scale_probe_comp_dxil.h>
#include <mediaserver/shaders/points_probe_vert_dxil.h>
#include <mediaserver/shaders/solid_frag_dxil.h>
#endif

#include <array>
#include <cstring>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// Writable buffers and indirect arguments: a dispatch writes a buffer and the
// arguments for the work after it, a second dispatch runs with those
// arguments and reads what the first wrote, and a pulled draw takes its
// instance count from the GPU. Every value is predictable, so the checks are
// exact.
namespace {

namespace core = mediaserver::core;
namespace rhi = mediaserver::rhi;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) + ": check failed: " + expression};
    }
}

#define RHI_CHECK(...) check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

template <class T>
[[nodiscard]] std::span<const core::u8> bytes_of(const T& value) {
    return {reinterpret_cast<const core::u8*>(&value), sizeof(T)};
}

template <class T, std::size_t N>
[[nodiscard]] std::span<const core::u8> bytes_of(const std::array<T, N>& values) {
    return {reinterpret_cast<const core::u8*>(values.data()), sizeof(T) * N};
}

[[nodiscard]] std::vector<core::u32> read_uints(rhi::Readback& readback, core::u32 count) {
    const auto mapped = readback.map();
    std::vector<core::u32> values;
    if (mapped.size() >= static_cast<std::size_t>(count) * 4U) {
        values.resize(count);
        std::memcpy(values.data(), mapped.data(), static_cast<std::size_t>(count) * 4U);
    }
    readback.unmap();
    return values;
}

// Debug builds run under the D3D12 debug layer, which fails the submission on
// a missing transition or barrier rather than letting it pass by luck.
[[nodiscard]] rhi::Device open_d3d12() {
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
#if !defined(NDEBUG)
    config.debug = true;
#endif
    auto device = rhi::Device::create(config);
    RHI_CHECK(device);
    return std::move(device).value();
}

void submit_and_wait(rhi::Device& device, rhi::CommandList commands) {
    auto submitted = device.submit(std::move(commands));
    RHI_CHECK(submitted);
    RHI_CHECK(std::move(submitted).value().wait());
}

void capabilities_are_honest_about_backends() {
    // The SDL device is gone before the D3D12 one opens: SDL GPU sits on a
    // D3D12 device without the debug layer, and the runtime refuses to create
    // a debug-layer device while a non-debug one is alive in the process.
    {
        rhi::DeviceConfig config{};
        config.backend = rhi::BackendKind::SdlGpu;
        auto sdl = rhi::Device::create(config);
        if (sdl) {
            RHI_CHECK(!sdl.value().capabilities().writable_buffers);
            RHI_CHECK(!sdl.value().capabilities().indirect_dispatch);
            RHI_CHECK(!sdl.value().capabilities().indirect_draw);
            auto refused = sdl.value().create_writable_buffer(64U);
            RHI_CHECK(!refused && refused.error() == rhi::RhiError::OperationUnsupported);
        }
    }
    if (rhi::Device::available(rhi::BackendKind::D3d12)) {
        auto device = open_d3d12();
        RHI_CHECK(device.capabilities().writable_buffers);
        RHI_CHECK(device.capabilities().indirect_dispatch);
        RHI_CHECK(device.capabilities().indirect_draw);
        auto buffer = device.create_writable_buffer(64U);
        RHI_CHECK(buffer && buffer.value().writable() && buffer.value().ranged());
        // Geometry buffers stay read-only.
        auto geometry = device.create_geometry_buffer(64U);
        RHI_CHECK(geometry && !geometry.value().writable());
    }
}

#if defined(MEDIASERVER_HAS_DXIL_SHADERS)

template <std::size_t N>
[[nodiscard]] std::span<const core::u8> code(const unsigned char (&bytecode)[N]) {
    return {reinterpret_cast<const core::u8*>(bytecode), N};
}

[[nodiscard]] rhi::ShaderDesc compute(std::span<const core::u8> bytecode, core::u32 storage, core::u32 writable) {
    rhi::ShaderDesc desc{};
    desc.stage = rhi::ShaderStage::Compute;
    desc.format = rhi::ShaderFormat::Dxil;
    desc.bytecode = bytecode;
    desc.storage_buffer_count = storage;
    desc.writable_buffer_count = writable;
    desc.uniform_buffer_count = 1U;
    return desc;
}

struct Parameters final {
    core::u32 count{};
    core::u32 value{};
    core::u32 padding[2]{};
};

constexpr core::u32 element_count = 200U; // four groups of 64, the last partial
constexpr core::u32 factor = 3U;
constexpr core::u32 addend = 1000U;

void a_dispatch_writes_buffers_and_the_next_runs_from_its_arguments() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    auto device = open_d3d12();
    auto scale = device.create_compute_pipeline(compute(code(mediaserver_buffer_scale_probe_comp_dxil), 1U, 2U));
    auto add = device.create_compute_pipeline(compute(code(mediaserver_buffer_add_probe_comp_dxil), 0U, 1U));
    RHI_CHECK(scale && add);

    // A device-local source, written by range; it is bound read-only.
    std::array<core::u32, element_count> source{};
    for (core::u32 i = 0U; i < element_count; ++i) {
        source[i] = 7U * i + 11U;
    }
    auto input = device.create_geometry_buffer(element_count * 4U);
    // Room for one element past the count, which must stay untouched (zero).
    auto values = device.create_writable_buffer((element_count + 1U) * 4U);
    auto arguments = device.create_writable_buffer(32U);
    auto readback = device.create_readback((element_count + 1U) * 4U);
    auto argument_readback = device.create_readback(32U);
    RHI_CHECK(input && values && arguments && readback && argument_readback);

    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();
    RHI_CHECK(commands.upload_range(input.value(), 0U, bytes_of(source)));

    const Parameters scale_parameters{element_count, factor, {}};
    const rhi::StorageBuffer* scale_reads[] = {&input.value()};
    const rhi::StorageBuffer* scale_writes[] = {&values.value(), &arguments.value()};
    rhi::ComputeDispatch first{};
    first.pipeline = &scale.value();
    first.storage_buffers = scale_reads;
    first.writable_buffers = scale_writes;
    first.uniform = bytes_of(scale_parameters);
    first.groups_x = (element_count + 63U) / 64U;
    RHI_CHECK(commands.dispatch(first));

    // Sized by the GPU: no group count given here.
    const Parameters add_parameters{element_count, addend, {}};
    const rhi::StorageBuffer* add_writes[] = {&values.value()};
    rhi::ComputeDispatch second{};
    second.pipeline = &add.value();
    second.writable_buffers = add_writes;
    second.uniform = bytes_of(add_parameters);
    second.groups_x = 0U;
    second.indirect = rhi::IndirectArguments{&arguments.value(), 0U};
    RHI_CHECK(commands.dispatch(second));

    RHI_CHECK(commands.download(values.value(), 0U, (element_count + 1U) * 4U, readback.value()));
    RHI_CHECK(commands.download(arguments.value(), 0U, 32U, argument_readback.value()));
    submit_and_wait(device, std::move(commands));

    const std::vector<core::u32> result = read_uints(readback.value(), element_count + 1U);
    RHI_CHECK(result.size() == element_count + 1U);
    for (core::u32 i = 0U; i < element_count; ++i) {
        RHI_CHECK(result[i] == source[i] * factor + i + addend);
    }
    RHI_CHECK(result[element_count] == 0U);
    const std::vector<core::u32> args = read_uints(argument_readback.value(), 8U);
    RHI_CHECK(args.size() == 8U);
    RHI_CHECK(args[0] == 4U && args[1] == 1U && args[2] == 1U);
    RHI_CHECK(args[4] == 6U && args[5] == element_count && args[6] == 0U && args[7] == 0U);
}

void bad_bindings_are_refused_before_anything_is_recorded() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    auto device = open_d3d12();
    auto add = device.create_compute_pipeline(compute(code(mediaserver_buffer_add_probe_comp_dxil), 0U, 1U));
    auto values = device.create_writable_buffer(64U);
    auto geometry = device.create_geometry_buffer(64U);
    RHI_CHECK(add && values && geometry);
    const Parameters parameters{4U, 1U, {}};

    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();
    rhi::ComputeDispatch work{};
    work.pipeline = &add.value();
    work.uniform = bytes_of(parameters);

    // The pipeline declares one writable buffer: none is refused.
    RHI_CHECK(!commands.dispatch(work));
    // A geometry buffer cannot be written.
    const rhi::StorageBuffer* read_only[] = {&geometry.value()};
    work.writable_buffers = read_only;
    RHI_CHECK(!commands.dispatch(work));
    // A buffer cannot be both written and the source of the arguments.
    const rhi::StorageBuffer* writable[] = {&values.value()};
    work.writable_buffers = writable;
    work.indirect = rhi::IndirectArguments{&values.value(), 0U};
    RHI_CHECK(!commands.dispatch(work));
    // Arguments must fit the buffer.
    auto small = device.create_writable_buffer(8U);
    RHI_CHECK(small);
    work.indirect = rhi::IndirectArguments{&small.value(), 0U};
    RHI_CHECK(!commands.dispatch(work));
    // The same work with a direct size runs, so the list was left usable.
    work.indirect = {};
    work.groups_x = 1U;
    RHI_CHECK(commands.dispatch(work));
    submit_and_wait(device, std::move(commands));
}

void an_indirect_draw_takes_its_instance_count_from_the_gpu() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    auto device = open_d3d12();
    rhi::ShaderDesc vertex{};
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.format = rhi::ShaderFormat::Dxil;
    vertex.bytecode = code(mediaserver_points_probe_vert_dxil);
    vertex.storage_buffer_count = 1U;
    rhi::ShaderDesc fragment{};
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.format = rhi::ShaderFormat::Dxil;
    fragment.bytecode = code(mediaserver_solid_frag_dxil);
    auto squares = device.create_pipeline(vertex, fragment, rhi::TextureFormat::Rgba8Unorm, rhi::ColorBlend::Disabled);
    auto scale = device.create_compute_pipeline(compute(code(mediaserver_buffer_scale_probe_comp_dxil), 1U, 2U));
    RHI_CHECK(squares && scale);

    constexpr core::u32 size = 64U;
    rhi::TextureDesc color_desc{};
    color_desc.width = size;
    color_desc.height = size;
    color_desc.format = rhi::TextureFormat::Rgba8Unorm;
    color_desc.color_target = true;
    color_desc.sampled = true;
    rhi::TextureDesc depth_desc{};
    depth_desc.width = size;
    depth_desc.height = size;
    depth_desc.format = rhi::TextureFormat::Depth32Float;
    depth_desc.depth_target = true;
    auto color = device.create_texture(color_desc);
    auto depth = device.create_texture(depth_desc);
    auto pixels = device.create_readback(size * size * 4U);

    // Three points across the middle, but the GPU will ask for two.
    const std::array<float, 6> points{-0.5F, 0.0F, 0.0F, 0.0F, 0.5F, 0.0F};
    auto positions = device.create_writable_buffer(sizeof(points));
    auto unused = device.create_writable_buffer(64U);
    auto arguments = device.create_writable_buffer(32U);
    auto source = device.create_geometry_buffer(64U);
    RHI_CHECK(color && depth && pixels && positions && unused && arguments && source);

    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();
    RHI_CHECK(commands.upload_range(positions.value(), 0U, bytes_of(points)));
    const Parameters parameters{2U, 1U, {}};
    const rhi::StorageBuffer* reads[] = {&source.value()};
    const rhi::StorageBuffer* writes[] = {&unused.value(), &arguments.value()};
    rhi::ComputeDispatch count{};
    count.pipeline = &scale.value();
    count.storage_buffers = reads;
    count.writable_buffers = writes;
    count.uniform = bytes_of(parameters);
    RHI_CHECK(commands.dispatch(count));

    // The batch's counts (three instances) are ignored for the indirect ones.
    const std::array<rhi::DrawBatch, 1U> batches{rhi::DrawBatch{6U, 3U}};
    const rhi::StorageBuffer* vertex_buffers[] = {&positions.value()};
    rhi::PulledDraw draw{};
    draw.pipeline = &squares.value();
    draw.vertex_buffers = vertex_buffers;
    draw.batches = batches;
    draw.indirect = rhi::IndirectArguments{&arguments.value(), 16U};
    RHI_CHECK(commands.draw_pulled(color.value(), rhi::Color{0.0F, 0.0F, 0.0F, 1.0F}, depth.value(), 0.0F, draw));
    RHI_CHECK(commands.download(color.value(), pixels.value()));
    submit_and_wait(device, std::move(commands));

    // Clip x of -0.5, 0 and 0.5 are pixels 16, 32 and 48 across 64.
    const auto mapped = pixels.value().map();
    RHI_CHECK(mapped.size() >= size * size * 4U);
    const auto red = [&](core::u32 x, core::u32 y) { return mapped[(static_cast<std::size_t>(y) * size + x) * 4U]; };
    RHI_CHECK(red(16U, 32U) == 255U);
    RHI_CHECK(red(32U, 32U) == 255U);
    RHI_CHECK(red(48U, 32U) == 0U); // the third instance was not drawn
    RHI_CHECK(red(32U, 8U) == 0U);
    pixels.value().unmap();

    // An indirect draw needs exactly one batch.
    auto again = device.acquire_commands();
    RHI_CHECK(again);
    auto refused = std::move(again).value();
    const std::array<rhi::DrawBatch, 2U> two{rhi::DrawBatch{6U, 1U}, rhi::DrawBatch{6U, 1U}};
    draw.batches = two;
    RHI_CHECK(!refused.draw_pulled(color.value(), rhi::Color{}, depth.value(), 0.0F, draw));
}

void a_copy_moves_one_range_of_a_buffer_into_another() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)) {
        return;
    }
    auto device = open_d3d12();
    auto source = device.create_writable_buffer(16U);
    auto destination = device.create_writable_buffer(16U);
    auto readback = device.create_readback(16U);
    RHI_CHECK(source && destination && readback);
    auto acquired = device.acquire_commands();
    RHI_CHECK(acquired);
    auto commands = std::move(acquired).value();
    const std::array<core::u32, 4U> from{10U, 20U, 30U, 40U};
    const std::array<core::u32, 4U> into{1U, 2U, 3U, 4U};
    RHI_CHECK(commands.upload_range(source.value(), 0U, bytes_of(from)));
    RHI_CHECK(commands.upload_range(destination.value(), 0U, bytes_of(into)));
    // The third word of one into the second of the other, the rest kept.
    RHI_CHECK(commands.copy_buffer(source.value(), 8U, destination.value(), 4U, 4U));
    // Past either end, or unaligned, is refused.
    RHI_CHECK(!commands.copy_buffer(source.value(), 12U, destination.value(), 0U, 8U));
    RHI_CHECK(!commands.copy_buffer(source.value(), 0U, destination.value(), 14U, 4U));
    RHI_CHECK(!commands.copy_buffer(source.value(), 2U, destination.value(), 0U, 4U));
    RHI_CHECK(commands.download(destination.value(), 0U, 16U, readback.value()));
    submit_and_wait(device, std::move(commands));
    RHI_CHECK((read_uints(readback.value(), 4U) == std::vector<core::u32>{1U, 30U, 3U, 4U}));
}

#endif

} // namespace

int main() {
    if (!mediaserver::rhi::Device::available()) {
        std::cout << "[skip] no GPU backend on this machine\n";
        return 0;
    }
    struct Case final {
        const char* name;
        void (*run)();
    };
    const Case cases[] = {
        {"capabilities_are_honest_about_backends", capabilities_are_honest_about_backends},
#if defined(MEDIASERVER_HAS_DXIL_SHADERS)
        {"a_dispatch_writes_buffers_and_the_next_runs_from_its_arguments",
         a_dispatch_writes_buffers_and_the_next_runs_from_its_arguments},
        {"bad_bindings_are_refused_before_anything_is_recorded", bad_bindings_are_refused_before_anything_is_recorded},
        {"an_indirect_draw_takes_its_instance_count_from_the_gpu", an_indirect_draw_takes_its_instance_count_from_the_gpu},
        {"a_copy_moves_one_range_of_a_buffer_into_another", a_copy_moves_one_range_of_a_buffer_into_another},
#endif
    };
    int failures = 0;
    for (const Case& test : cases) {
        try {
            test.run();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    return failures == 0 ? 0 : 1;
}
