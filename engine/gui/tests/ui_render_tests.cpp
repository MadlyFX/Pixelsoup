#include <mediaserver/gui/text.hpp>
#include <mediaserver/gui/ui_renderer.hpp>

#include <mediaserver/rhi/device.hpp>

#include <array>
#include <cmath>
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

void hinted_glyph_quads_stay_on_the_pixel_grid() {
    auto atlas = load_font();
    if (!atlas.has_value()) {
        std::cout << "[skip] no system font; glyph alignment suite skipped\n";
        return;
    }

    gui::UiDrawList list;
    list.set_white_texel(atlas->white_u(), atlas->white_v());
    static_cast<void>(gui::draw_text(
        list, *atlas, 8.35F, 7.65F, "SECTION HEADER",
        gui::UiColor{1.0F, 1.0F, 1.0F, 1.0F}));

    UI_CHECK(!list.vertices().empty());
    for (const gui::UiVertex& vertex : list.vertices()) {
        UI_CHECK(std::fabs(vertex.x - std::round(vertex.x)) < 0.001F);
        UI_CHECK(std::fabs(vertex.y - std::round(vertex.y)) < 0.001F);
    }
}

// An atlas baked for a 200% display holds glyphs twice the size, but measures
// and reports its metrics in the same logical units the 100% atlas does - so
// a layout never learns the scale - and its glyphs still land on whole
// physical pixels once the list multiplies them out.
void a_scaled_atlas_measures_in_logical_units() {
    constexpr std::string_view path = "C:/Windows/Fonts/segoeui.ttf";
    auto normal = gui::FontAtlas::create(path, 13U, 1.0F);
    auto dense = gui::FontAtlas::create(path, 13U, 2.0F);
    if (!normal.has_value() || !dense.has_value()) {
        std::cout << "[skip] no Segoe UI; scaled atlas suite skipped\n";
        return;
    }
    UI_CHECK(dense->scale() == 2.0F);
    const float plain = normal->measure("Particle Ribbons");
    const float scaled = dense->measure("Particle Ribbons");
    UI_CHECK(std::fabs(plain - scaled) <= plain * 0.08F);
    UI_CHECK(std::fabs(normal->line_height() - dense->line_height()) <= 1.5F);
    const gui::Glyph* const small = normal->glyph('W');
    const gui::Glyph* const large = dense->glyph('W');
    UI_CHECK(small != nullptr && large != nullptr);
    UI_CHECK(large->height >= small->height * 1.7F);

    gui::UiDrawList list;
    list.set_scale(2.0F);
    list.set_white_texel(dense->white_u(), dense->white_v());
    const float advance = gui::draw_text(
        list, *dense, 8.3F, 7.7F, "Wall", gui::UiColor{1.0F, 1.0F, 1.0F, 1.0F});
    UI_CHECK(std::fabs(advance - dense->measure("Wall")) < 0.6F);
    UI_CHECK(!list.vertices().empty());
    for (const gui::UiVertex& vertex : list.vertices()) {
        UI_CHECK(std::fabs(vertex.x - std::round(vertex.x)) < 0.001F);
        UI_CHECK(std::fabs(vertex.y - std::round(vertex.y)) < 0.001F);
    }
}

struct Case final {
    const char* name;
    void (*function)();
};

// An engine target composited into the UI. The point of the case is that the
// image samples its own texture rather than the glyph atlas, and that the fills
// and text around it still come out right - the draw list has to break into one
// pass per texture and compose them in draw order, not clear between them.
void an_image_samples_its_own_texture() {
    auto device = open_device();
    auto atlas = load_font();
    if (!atlas.has_value()) {
        std::cout << "[skip] no system font; image suite needs the atlas\n";
        return;
    }

    auto created = gui::UiRenderer::create(device, rhi::TextureFormat::Rgba8Unorm);
    UI_CHECK(created.has_value());
    auto renderer = std::move(created).value();

    // A stand-in for a rendered viewport/filmstrip atlas: green on the left
    // and magenta on the right so explicit UV cropping is observable.
    constexpr core::u32 source_size = 16U;
    rhi::TextureDesc desc{};
    desc.width = source_size;
    desc.height = source_size;
    desc.format = rhi::TextureFormat::Rgba8Unorm;
    desc.sampled = true;
    auto source = device.create_texture(desc);
    UI_CHECK(source.has_value());

    std::vector<core::u8> source_pixels(
        static_cast<std::size_t>(source_size) * source_size * 4U, 0U);
    for (core::u32 y = 0U; y < source_size; ++y) {
        for (core::u32 x = 0U; x < source_size; ++x) {
            const std::size_t offset =
                (static_cast<std::size_t>(y) * source_size + x) * 4U;
            if (x < source_size / 2U) {
                source_pixels[offset + 1U] = 255U; // green
            } else {
                source_pixels[offset] = 255U;      // magenta
                source_pixels[offset + 2U] = 255U;
            }
            source_pixels[offset + 3U] = 255U;
        }
    }

    constexpr core::u32 width = 128U;
    constexpr core::u32 height = 64U;
    gui::UiDrawList list;
    list.set_white_texel(atlas->white_u(), atlas->white_v());
    // A red fill, the image over part of it, and a blue fill after: three
    // passes, and the later ones must not wipe the earlier.
    list.add_rect(0.0F, 0.0F, 128.0F, 64.0F, gui::UiColor{1.0F, 0.0F, 0.0F, 1.0F});
    list.add_image(16.0F, 8.0F, 48.0F, 32.0F, source.value());
    list.add_image_region(68.0F, 8.0F, 24.0F, 32.0F, source.value(),
                          0.5F, 0.0F, 1.0F, 1.0F);
    list.add_rect(96.0F, 40.0F, 16.0F, 16.0F, gui::UiColor{0.0F, 0.0F, 1.0F, 1.0F});

    auto acquired = device.acquire_commands();
    UI_CHECK(acquired.has_value());
    auto commands = std::move(acquired).value();
    UI_CHECK(commands.upload(source.value(), source_pixels));

    rhi::TextureDesc target_desc{};
    target_desc.width = width;
    target_desc.height = height;
    target_desc.format = rhi::TextureFormat::Rgba8Unorm;
    target_desc.color_target = true;
    auto target = device.create_texture(target_desc);
    UI_CHECK(target.has_value());

    const auto bytes = width * height * 4U;
    auto readback = device.create_readback(bytes);
    UI_CHECK(readback.has_value());

    UI_CHECK(renderer.upload_atlas(
        device, commands, atlas->pixels(), atlas->width(), atlas->height()));
    UI_CHECK(renderer.render(
        device,
        commands,
        target.value(),
        rhi::Color{0.0F, 0.0F, 0.0F, 1.0F},
        list,
        width,
        height));
    UI_CHECK(commands.download(target.value(), readback.value()));
    auto submitted = device.submit(std::move(commands));
    UI_CHECK(submitted.has_value());
    UI_CHECK(std::move(submitted).value().wait());

    Image out{};
    out.width = width;
    out.height = height;
    const auto pixels = readback.value().map();
    UI_CHECK(pixels.size() >= bytes);
    out.rgba.assign(pixels.begin(), pixels.begin() + bytes);
    readback.value().unmap();

    // Inside the image: the source's green, not the atlas.
    const auto inside = out.at(28U, 24U);
    UI_CHECK(out.rgba[inside + 0U] < 60U);
    UI_CHECK(out.rgba[inside + 1U] > 200U);
    UI_CHECK(out.rgba[inside + 2U] < 60U);

    // The explicit region displays only the atlas's magenta right half.
    const auto cropped = out.at(80U, 24U);
    UI_CHECK(out.rgba[cropped + 0U] > 200U);
    UI_CHECK(out.rgba[cropped + 1U] < 60U);
    UI_CHECK(out.rgba[cropped + 2U] > 200U);

    // The fill drawn before it survived: the image's pass loaded, not cleared.
    const auto before = out.at(8U, 4U);
    UI_CHECK(out.rgba[before + 0U] > 200U);
    UI_CHECK(out.rgba[before + 1U] < 60U);

    // And the fill drawn after it landed on top of the red.
    const auto after = out.at(100U, 48U);
    UI_CHECK(out.rgba[after + 2U] > 200U);
    UI_CHECK(out.rgba[after + 0U] < 60U);
}

// The primitives the operator surfaces need that a rectangle cannot express: a
// keyframe diamond, a twirl arrow, a gizmo axis at an angle. What could be
// wrong silently is winding (a back-facing triangle culls to nothing) and
// coverage (a fan that misses part of its own polygon), so this renders one of
// each and reads the pixels inside and outside them.
void polygons_and_lines_cover_their_own_geometry() {
    auto device = open_device();
    auto atlas = load_font();
    if (!atlas.has_value()) {
        std::cout << "[skip] no system font; polygon suite needs the atlas's "
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

    // A red triangle wound clockwise in pixel space (which is counter-clockwise
    // once y is flipped into clip space), covering (20, 20)-ish.
    list.add_triangle(
        20.0F, 8.0F, 34.0F, 32.0F, 6.0F, 32.0F,
        gui::UiColor{1.0F, 0.0F, 0.0F, 1.0F});

    // A green diamond - the keyframe marker - centred at (64, 20), as four
    // points through add_convex_poly.
    const std::array<gui::UiPoint, 4U> diamond{{
        {64.0F, 12.0F}, {72.0F, 20.0F}, {64.0F, 28.0F}, {56.0F, 20.0F}}};
    list.add_convex_poly(diamond, gui::UiColor{0.0F, 1.0F, 0.0F, 1.0F});

    // A blue diagonal, thick enough that its centre is unambiguous.
    list.add_line(
        96.0F, 8.0F, 120.0F, 32.0F, 4.0F, gui::UiColor{0.0F, 0.0F, 1.0F, 1.0F});

    const auto image = render_ui(device, renderer, *atlas, list, width, height);

    // Well inside the triangle.
    const auto in_triangle = image.at(20U, 26U);
    UI_CHECK(image.rgba[in_triangle + 0U] > 200U);
    // Outside it, but inside its bounding box: the corner a rectangle would
    // have painted and a triangle must not.
    const auto beside_triangle = image.at(7U, 10U);
    UI_CHECK(image.rgba[beside_triangle + 0U] < 20U);

    // The diamond's centre, and a bounding-box corner it must leave black.
    const auto in_diamond = image.at(64U, 20U);
    UI_CHECK(image.rgba[in_diamond + 1U] > 200U);
    const auto diamond_corner = image.at(57U, 13U);
    UI_CHECK(image.rgba[diamond_corner + 1U] < 20U);

    // The line's own centre, at the midpoint of the segment.
    const auto on_line = image.at(108U, 20U);
    UI_CHECK(image.rgba[on_line + 2U] > 200U);
    // And the corner of its bounding box, which a 45-degree segment misses.
    const auto off_line = image.at(118U, 10U);
    UI_CHECK(image.rgba[off_line + 2U] < 20U);
}

constexpr std::array<Case, 7U> cases{{
    {"a scaled atlas measures in logical units",
     &a_scaled_atlas_measures_in_logical_units},
    {"a solid rect paints its exact colour",
     &a_solid_rect_paints_its_exact_colour},
    {"a clip rect confines a fill", &a_clip_rect_confines_a_fill},
    {"text paints coverage where it sits", &text_paints_coverage_where_it_sits},
    {"hinted glyph quads stay on the pixel grid",
     &hinted_glyph_quads_stay_on_the_pixel_grid},
    {"an image samples its own texture", &an_image_samples_its_own_texture},
    {"polygons and lines cover their own geometry",
     &polygons_and_lines_cover_their_own_geometry},
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
