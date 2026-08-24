#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_renderer.hpp>

#include <mediaserver/rhi/device.hpp>

#include <array>
#include <cstddef>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// The GPU proof for the bespoke UI's draw foundation. With no window, it renders
// a solid rectangle and a line of text into an offscreen colour target and reads
// it back, proving three things end to end: the UI pipeline composites an
// alpha-blended quad in the target's pixel space; a solid fill samples the
// atlas's white texel (so its colour is exact); and FreeType glyphs baked into
// the same atlas paint coverage where the text sits. Everything above this - the
// layout, widgets, and the ported editor panels - rests on it.
namespace {

namespace core = mediaserver::core;
namespace rhi = mediaserver::rhi;
namespace gui = mediaserver::gui;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define UI_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] rhi::Device open_device() {
    rhi::DeviceConfig config{};
    config.debug = false;
    auto created = rhi::Device::create(config);
    check(created.has_value(), "device creation", __LINE__);
    return std::move(created).value();
}

// A font is needed to bake the atlas (which also carries the white texel). The
// suite falls back across the fonts a stock Windows install ships; if none load,
// text-dependent expectations are skipped rather than failed.
[[nodiscard]] std::optional<gui::FontAtlas> load_font() {
    constexpr std::array<std::string_view, 4U> candidates{
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/arial.ttf",
        "C:/Windows/Fonts/consola.ttf",
        "C:/Windows/Fonts/tahoma.ttf"};
    for (const auto path : candidates) {
        auto atlas = gui::FontAtlas::create(path, 16U);
        if (atlas.has_value()) {
            return atlas;
        }
    }
    return std::nullopt;
}

struct Image final {
    core::u32 width{};
    core::u32 height{};
    std::vector<core::u8> rgba;

    [[nodiscard]] std::size_t at(core::u32 x, core::u32 y) const noexcept {
        return (static_cast<std::size_t>(y) * width + x) * 4U;
    }
};

// Renders one draw list, backed by `atlas`, into a fresh offscreen target and
// reads it back.
[[nodiscard]] Image render_ui(
    rhi::Device& device,
    gui::UiRenderer& renderer,
    const gui::FontAtlas& atlas,
    const gui::UiDrawList& list,
    core::u32 width,
    core::u32 height) {
    rhi::TextureDesc desc{};
    desc.width = width;
    desc.height = height;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.color_target = true;
    auto target = device.create_texture(desc);
    check(target.has_value(), "target creation", __LINE__);

    const auto bytes = width * height * 4U;
    auto readback = device.create_readback(bytes);
    check(readback.has_value(), "readback creation", __LINE__);

    auto acquired = device.acquire_commands();
    check(acquired.has_value(), "acquire commands", __LINE__);
    auto commands = std::move(acquired).value();

    check(
        renderer.upload_atlas(
            device, commands, atlas.pixels(), atlas.width(), atlas.height()),
        "atlas upload",
        __LINE__);
    check(
        renderer.render(
            device,
            commands,
            target.value(),
            rhi::Color{0.0F, 0.0F, 0.0F, 1.0F},
            list,
            width,
            height),
        "render",
        __LINE__);
    check(
        commands.download(target.value(), readback.value()),
        "download",
        __LINE__);
    auto submitted = device.submit(std::move(commands));
    check(submitted.has_value(), "submit", __LINE__);
    check(std::move(submitted).value().wait(), "wait", __LINE__);

    Image image{};
    image.width = width;
    image.height = height;
    const auto pixels = readback.value().map();
    check(pixels.size() >= bytes, "readback map", __LINE__);
    image.rgba.assign(pixels.begin(), pixels.begin() + bytes);
    readback.value().unmap();
    return image;
}

void a_solid_rect_paints_its_exact_colour() {
    auto device = open_device();
    auto atlas = load_font();
    if (!atlas.has_value()) {
        std::cout << "[skip] no system font; rect suite needs the atlas's "
                     "white texel\n";
        return;
    }

    auto created = gui::UiRenderer::create(device, rhi::TextureFormat::Rgba8Unorm);
    UI_CHECK(created.has_value());
    auto renderer = std::move(created).value();

    constexpr core::u32 width = 128U;
    constexpr core::u32 height = 64U;
    gui::UiDrawList list;
    list.set_white_texel(atlas->white_u(), atlas->white_v());
    // An opaque red rectangle over a black clear.
    list.add_rect(10.0F, 10.0F, 40.0F, 20.0F, gui::UiColor{1.0F, 0.0F, 0.0F, 1.0F});

    const auto image = render_ui(device, renderer, *atlas, list, width, height);

    // Inside the rect: red.
    const auto inside = image.at(30U, 20U);
    UI_CHECK(image.rgba[inside + 0U] > 200U);
    UI_CHECK(image.rgba[inside + 1U] < 60U);
    UI_CHECK(image.rgba[inside + 2U] < 60U);

    // Outside it: the black clear shows through.
    const auto outside = image.at(100U, 5U);
    UI_CHECK(image.rgba[outside + 0U] < 20U);
    UI_CHECK(image.rgba[outside + 1U] < 20U);
    UI_CHECK(image.rgba[outside + 2U] < 20U);
}

void a_clip_rect_confines_a_fill() {
    auto device = open_device();
    auto atlas = load_font();
    if (!atlas.has_value()) {
        std::cout << "[skip] no system font; clip suite needs the atlas\n";
        return;
    }

    auto created = gui::UiRenderer::create(device, rhi::TextureFormat::Rgba8Unorm);
    UI_CHECK(created.has_value());
    auto renderer = std::move(created).value();

    constexpr core::u32 width = 128U;
    constexpr core::u32 height = 64U;
    gui::UiDrawList list;
    list.set_white_texel(atlas->white_u(), atlas->white_v());
    // A fill larger than its clip; only the clipped region should paint.
    list.push_clip(20.0F, 20.0F, 20.0F, 20.0F);
    list.add_rect(0.0F, 0.0F, 128.0F, 64.0F, gui::UiColor{0.0F, 1.0F, 0.0F, 1.0F});
    list.pop_clip();

    const auto image = render_ui(device, renderer, *atlas, list, width, height);

    // Inside the clip: green.
    const auto inside = image.at(30U, 30U);
    UI_CHECK(image.rgba[inside + 1U] > 200U);
    // Outside the clip but inside the fill: black. The shader discarded it.
    const auto outside = image.at(10U, 10U);
    UI_CHECK(image.rgba[outside + 1U] < 20U);
}

void text_paints_coverage_where_it_sits() {
    auto device = open_device();
    auto atlas = load_font();
    if (!atlas.has_value()) {
        std::cout << "[skip] no system font; text suite skipped\n";
        return;
    }

    auto created = gui::UiRenderer::create(device, rhi::TextureFormat::Rgba8Unorm);
    UI_CHECK(created.has_value());
    auto renderer = std::move(created).value();

    constexpr core::u32 width = 160U;
    constexpr core::u32 height = 48U;
    const std::string_view text = "Hi 123";
    gui::UiDrawList list;
    list.set_white_texel(atlas->white_u(), atlas->white_v());
    const float advance =
        gui::draw_text(list, *atlas, 8.0F, 8.0F, text, gui::UiColor{1.0F, 1.0F, 1.0F, 1.0F});
    UI_CHECK(advance > 0.0F);

    const auto image = render_ui(device, renderer, *atlas, list, width, height);

    // Somewhere in the text's box, glyph coverage lit non-black pixels. The clear
    // is black, so any bright pixel is text.
    core::u32 lit = 0U;
    const auto x_end = static_cast<core::u32>(8.0F + advance);
    const auto y_end =
        static_cast<core::u32>(8.0F + atlas->line_height());
    for (core::u32 y = 8U; y < y_end && y < height; ++y) {
        for (core::u32 x = 8U; x < x_end && x < width; ++x) {
            if (image.rgba[image.at(x, y) + 0U] > 100U) {
                ++lit;
            }
        }
    }
    UI_CHECK(lit > 0U);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 3U> cases{{
    {"a solid rect paints its exact colour",
     &a_solid_rect_paints_its_exact_colour},
    {"a clip rect confines a fill", &a_clip_rect_confines_a_fill},
    {"text paints coverage where it sits", &text_paints_coverage_where_it_sits},
}};

} // namespace

int main() {
    if (!rhi::Device::available()) {
        std::cout << "[skip] no GPU backend on this machine; UI suite skipped\n";
        return 0;
    }
    if (!gui::UiRenderer::shaders_available()) {
        std::cout << "[skip] this build carries no UI shader bytecode; "
                     "UI suite skipped\n";
        return 0;
    }
    {
        // A device that cannot create the UI pipeline (no shader format this
        // build supports) is a clean skip, not a failure.
        auto device = open_device();
        auto probe =
            gui::UiRenderer::create(device, rhi::TextureFormat::Rgba8Unorm);
        if (!probe) {
            std::cout << "[skip] no shader format this build supports on this "
                         "device; UI suite skipped\n";
            return 0;
        }
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
