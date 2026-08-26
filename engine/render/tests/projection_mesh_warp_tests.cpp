#include <mediaserver/render/projection_mesh_warp.hpp>
#include <mediaserver/render/projection_mesh_loader.hpp>

#include <mediaserver/rhi/device.hpp>

#include <array>
#include <cstddef>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace output = mediaserver::output;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} +
                                 std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define WARP_CHECK(...)                                                       \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

class ImportedPatchFixture final {
public:
    ImportedPatchFixture() {
        const auto stamp = std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count();
        path_ = std::filesystem::temp_directory_path()
            / (std::string{"mediaserver-warp-patch-"}
               + std::to_string(stamp) + ".obj");
        std::ofstream stream{path_, std::ios::binary | std::ios::trunc};
        stream
            << "v -4 2.25 -4\n"
               "v -4 -2.25 -4\n"
               "v 4 2.25 -4\n"
               "v 4 -2.25 -4\n"
               "vt 0 1\n"
               "vt 0 0\n"
               "vt 1 1\n"
               "vt 1 0\n"
               "f 1/1 2/2 3/3\n"
               "f 3/3 2/2 4/4\n";
        stream.close();
        WARP_CHECK(stream.good());
    }

    ImportedPatchFixture(const ImportedPatchFixture&) = delete;
    ImportedPatchFixture& operator=(const ImportedPatchFixture&) = delete;

    ~ImportedPatchFixture() {
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove(path_, ignored));
    }

    [[nodiscard]] std::string source() const { return path_.string(); }

private:
    std::filesystem::path path_;
};

[[nodiscard]] rhi::Device open_device() {
    auto created = rhi::Device::create(rhi::DeviceConfig{});
    check(created.has_value(), "device creation", __LINE__);
    return std::move(created).value();
}

[[nodiscard]] std::vector<core::u8> gradient(core::u32 width,
                                              core::u32 height) {
    std::vector<core::u8> rgba(
        static_cast<std::size_t>(width) * height * 4U, 0U);
    for (core::u32 y = 0U; y < height; ++y) {
        for (core::u32 x = 0U; x < width; ++x) {
            const std::size_t at =
                (static_cast<std::size_t>(y) * width + x) * 4U;
            rgba[at] = static_cast<core::u8>((x * 255U) / (width - 1U));
            rgba[at + 1U] =
                static_cast<core::u8>((y * 255U) / (height - 1U));
            rgba[at + 2U] = 32U;
            rgba[at + 3U] = 255U;
        }
    }
    return rgba;
}

[[nodiscard]] rhi::Texture upload_source(
    rhi::Device& device, core::u32 width, core::u32 height,
    const std::vector<core::u8>& rgba) {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    auto texture = device.create_texture(desc);
    check(texture.has_value(), "source texture", __LINE__);
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "upload commands", __LINE__);
    auto commands = std::move(acquired).value();
    check(commands.upload(texture.value(), rgba), "source upload", __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "source submit", __LINE__);
    check(std::move(submitted).value().wait(), "source wait", __LINE__);
    return std::move(texture).value();
}

[[nodiscard]] output::ProjectionMesh planar_mesh() {
    output::ProjectionMesh mesh{};
    mesh.vertices = {
        {{-4.0F, 2.25F, -4.0F}, 0.0F, 0.0F},
        {{4.0F, 2.25F, -4.0F}, 1.0F, 0.0F},
        {{-4.0F, -2.25F, -4.0F}, 0.0F, 1.0F},
        {{4.0F, -2.25F, -4.0F}, 1.0F, 1.0F},
    };
    mesh.indices = {0U, 2U, 1U, 1U, 2U, 3U};
    return mesh;
}

[[nodiscard]] output::ProjectionMesh curved_mesh() {
    // Four columns over a 120-degree, radius-four arc. This is the same
    // friendly surface generator persisted by Feed Mapping, so this test
    // proves the generated contract reaches the GPU warp rather than only
    // exercising an unrelated hand-authored mesh fixture.
    output::PhysicalSurface surface{};
    surface.center = {0.0F, 0.0F, -4.0F};
    surface.width = 8.3775804F;
    surface.height = 3.0F;
    surface.raster = {96U, 64U};
    auto generated = output::horizontal_arc_mesh(
        surface, 1.0F / 3.0F, 4U);
    check(generated.has_value(), "horizontal arc generation", __LINE__);
    return std::move(generated).value();
}

struct Image final {
    core::u32 width{};
    core::u32 height{};
    std::vector<core::u8> rgba;

    [[nodiscard]] std::size_t at(core::u32 x, core::u32 y) const noexcept {
        return (static_cast<std::size_t>(y) * width + x) * 4U;
    }
};

[[nodiscard]] Image run(rhi::Device& device, const rhi::Texture& source,
                        const output::ProjectionMesh& mesh,
                        core::u32 width, core::u32 height) {
    auto view = output::mesh_perspective_view_for(
        mesh, scene::Vec3{}, 0.1F, 100.0F);
    check(view.has_value(), "mesh view", __LINE__);
    auto created = render::ProjectionMeshWarp::create(
        device, width, height,
        render::ProjectionMeshWarpLimits{64U, 192U});
    check(created.has_value(), "warp creation", __LINE__);
    auto warp = std::move(created).value();
    auto readback = device.create_readback(width * height * 4U);
    check(readback.has_value(), "readback creation", __LINE__);
    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "draw commands", __LINE__);
    auto commands = std::move(acquired).value();
    check(warp.render(commands, source, mesh, view.value()), "warp render",
          __LINE__);
    check(commands.download(warp.target(), readback.value()), "warp download",
          __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "warp submit", __LINE__);
    check(std::move(submitted).value().wait(), "warp wait", __LINE__);

    const auto pixels = readback.value().map();
    check(pixels.size() >= static_cast<std::size_t>(width) * height * 4U,
          "warp map", __LINE__);
    Image image{width, height, {}};
    image.rgba.assign(
        pixels.begin(),
        pixels.begin() + static_cast<std::size_t>(width) * height * 4U);
    readback.value().unmap();
    return image;
}

[[nodiscard]] int difference(core::u8 a, core::u8 b) noexcept {
    return a > b ? static_cast<int>(a - b) : static_cast<int>(b - a);
}

void a_planar_projection_mesh_is_an_identity_warp() {
    auto device = open_device();
    constexpr core::u32 width = 96U;
    constexpr core::u32 height = 64U;
    const auto reference = gradient(width, height);
    const auto source = upload_source(device, width, height, reference);
    const Image image = run(device, source, planar_mesh(), width, height);

    // Ignore the one-pixel raster boundary; every interior sample should be
    // the matching source texel apart from linear-sampler rounding.
    for (core::u32 y = 2U; y + 2U < height; ++y) {
        for (core::u32 x = 2U; x + 2U < width; ++x) {
            const std::size_t at = image.at(x, y);
            WARP_CHECK(difference(image.rgba[at], reference[at]) <= 2);
            WARP_CHECK(difference(image.rgba[at + 1U],
                                  reference[at + 1U]) <= 2);
            WARP_CHECK(image.rgba[at + 3U] == 255U);
        }
    }
}

void a_curved_mesh_projectively_remaps_one_scene_render() {
    auto device = open_device();
    constexpr core::u32 width = 96U;
    constexpr core::u32 height = 64U;
    const auto reference = gradient(width, height);
    const auto source = upload_source(device, width, height, reference);
    const Image planar = run(device, source, planar_mesh(), width, height);
    const Image curved = run(device, source, curved_mesh(), width, height);

    const core::u32 y = height / 2U;
    const core::u32 quarter = width / 4U;
    const std::size_t planar_at = planar.at(quarter, y);
    const std::size_t curved_at = curved.at(quarter, y);
    WARP_CHECK(planar.rgba[planar_at + 3U] == 255U);
    WARP_CHECK(curved.rgba[curved_at + 3U] == 255U);
    // The curved quarter rib projects closer to the centre of the fitted scene
    // view than its flat destination UV. A fullscreen copy cannot satisfy it.
    WARP_CHECK(curved.rgba[curved_at] > planar.rgba[planar_at] + 10U);

    const std::size_t center = curved.at(width / 2U, y);
    WARP_CHECK(difference(curved.rgba[center], planar.rgba[center]) <= 3);
}

void an_imported_uv_patch_runs_through_the_gpu_warp() {
    ImportedPatchFixture fixture;
    output::PhysicalSurface calibration{};
    calibration.center = {};
    calibration.width = 8.0F;
    calibration.height = 4.5F;
    calibration.raster = {96U, 64U};
    const auto imported = render::load_projection_mesh(
        fixture.source(), calibration);
    WARP_CHECK(imported.has_value());

    auto device = open_device();
    constexpr core::u32 width = 96U;
    constexpr core::u32 height = 64U;
    const auto reference = gradient(width, height);
    const auto source = upload_source(device, width, height, reference);
    const Image image = run(device, source, imported.value(), width, height);
    for (core::u32 y = 2U; y + 2U < height; ++y) {
        for (core::u32 x = 2U; x + 2U < width; ++x) {
            const std::size_t at = image.at(x, y);
            WARP_CHECK(difference(image.rgba[at], reference[at]) <= 2);
            WARP_CHECK(difference(image.rgba[at + 1U],
                                  reference[at + 1U]) <= 2);
            WARP_CHECK(image.rgba[at + 3U] == 255U);
        }
    }
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 3U> cases{{
    {"a planar projection mesh is an identity warp",
     &a_planar_projection_mesh_is_an_identity_warp},
    {"a curved mesh projectively remaps one scene render",
     &a_curved_mesh_projectively_remaps_one_scene_render},
    {"an imported UV patch runs through the GPU warp",
     &an_imported_uv_patch_runs_through_the_gpu_warp},
}};

} // namespace

int main() {
    if (!rhi::Device::available() ||
        !render::ProjectionMeshWarp::shaders_available()) {
        std::cout << "[skip] projection mesh warp unavailable\n";
        return 0;
    }
    {
        auto device = open_device();
        const auto probe = render::ProjectionMeshWarp::create(
            device, 4U, 4U, render::ProjectionMeshWarpLimits{4U, 6U});
        if (!probe &&
            probe.error() == render::RenderError::ShaderFormatUnsupported) {
            std::cout << "[skip] projection mesh shader format unsupported\n";
            return 0;
        }
        WARP_CHECK(probe.has_value());
    }

    std::size_t failures{};
    for (const Case& test : cases) {
        try {
            test.function();
            std::cout << "[pass] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << test.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << cases.size() - failures << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
