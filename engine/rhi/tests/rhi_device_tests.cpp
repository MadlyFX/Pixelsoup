#include <mediaserver/rhi/device.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
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
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 6U> cases{{
    {"an offscreen target clears and reads back exactly",
     &an_offscreen_target_clears_and_reads_back_exactly},
    {"a blit moves pixels between targets without the CPU",
     &a_blit_moves_pixels_between_targets_without_the_cpu},
    {"resource handles release exactly once and survive moves",
     &resource_handles_release_exactly_once_and_survive_moves},
    {"invalid descriptions are refused before the backend sees them",
     &invalid_descriptions_are_refused_before_the_backend_sees_them},
    {"an upload larger than its buffer is refused rather than truncated",
     &an_upload_larger_than_its_buffer_is_refused_rather_than_truncated},
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
