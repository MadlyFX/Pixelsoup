#include <mediaserver/render/media_source.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

// The decode seam, checked with no GPU. The GPU media suite proves a clip lands
// on the quad; this proves the layer beneath it - the MediaSource - resolves
// paths and decodes frames, and that decode is frame-exact: any frame is
// reachable directly and the same frame always yields the same pixels.
namespace {

namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define SOURCE_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// A small solid-colour 32-bit uncompressed TGA (top-left origin). stb_image
// reads it, and a solid colour makes a frame's identity obvious after decode.
void write_solid_tga(
    const std::string& path, core::u8 red, core::u8 green, core::u8 blue) {
    constexpr core::u32 width = 4U;
    constexpr core::u32 height = 4U;
    std::array<core::u8, 18U> header{};
    header[2U] = 2U; // uncompressed true-colour
    header[12U] = static_cast<core::u8>(width & 0xffU);
    header[14U] = static_cast<core::u8>(height & 0xffU);
    header[16U] = 32U;   // bits per pixel
    header[17U] = 0x28U; // top-left origin, 8 alpha bits

    std::vector<core::u8> pixels;
    for (core::u32 i = 0U; i < width * height; ++i) {
        pixels.push_back(blue);  // TGA is BGRA
        pixels.push_back(green);
        pixels.push_back(red);
        pixels.push_back(255U);
    }
    std::ofstream out{path, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(header.data()), header.size());
    out.write(
        reinterpret_cast<const char*>(pixels.data()),
        static_cast<std::streamsize>(pixels.size()));
    out.close();
    check(static_cast<bool>(out), "wrote a test frame", __LINE__);
}

[[nodiscard]] std::string temp_path(const std::string& name) {
    return (std::filesystem::temp_directory_path() / name).string();
}

void the_frame_field_is_substituted_not_formatted() {
    // A run of '#' is the zero-padded frame field.
    SOURCE_CHECK(
        render::ImageSequenceSource::resolve_path("plate_####.png", 7U)
        == "plate_0007.png");
    // No field: a still, returned unchanged whatever the frame.
    SOURCE_CHECK(
        render::ImageSequenceSource::resolve_path("still.png", 3U) == "still.png");
    // A number wider than the field is not truncated.
    SOURCE_CHECK(
        render::ImageSequenceSource::resolve_path("f_#.png", 42U) == "f_42.png");
    // Not a printf format: a '%' in the template is a literal, not a directive.
    SOURCE_CHECK(
        render::ImageSequenceSource::resolve_path("100%_#.png", 5U)
        == "100%_5.png");
}

void a_frame_decodes_to_its_pixels() {
    render::ImageSequenceSource source;
    const auto path = temp_path("mediaserver_source_solid.tga");
    write_solid_tga(path, 220U, 30U, 40U); // a red frame

    const auto frame = source.decode(path, 0U);
    SOURCE_CHECK(frame.valid());
    SOURCE_CHECK(frame.width == 4U);
    SOURCE_CHECK(frame.height == 4U);
    SOURCE_CHECK(frame.format == rhi::TextureFormat::Rgba8Unorm);
    SOURCE_CHECK(frame.pixels.size() == 4U * 4U * 4U);
    // Decoded as RGBA: red first, full alpha.
    SOURCE_CHECK(frame.pixels[0U] > 200U);
    SOURCE_CHECK(frame.pixels[1U] < 60U);
    SOURCE_CHECK(frame.pixels[2U] < 60U);
    SOURCE_CHECK(frame.pixels[3U] == 255U);
}

void decode_is_frame_exact_and_reachable_in_any_order() {
    render::ImageSequenceSource source;
    const auto zero = temp_path("mediaserver_source_seq_0.tga");
    const auto one = temp_path("mediaserver_source_seq_1.tga");
    write_solid_tga(zero, 220U, 30U, 40U); // frame 0: red
    write_solid_tga(one, 30U, 210U, 50U);  // frame 1: green
    const std::string sequence =
        (std::filesystem::temp_directory_path() / "mediaserver_source_seq_#.tga")
            .string();

    // Frame 1 is reachable without first decoding frame 0.
    const auto green = source.decode(sequence, 1U);
    SOURCE_CHECK(green.valid());
    SOURCE_CHECK(green.pixels[0U] < 60U && green.pixels[1U] > 200U);

    // Frame 0 decodes to its own pixels, and re-decoding yields the same bytes:
    // decode is a pure function of (source, frame).
    const auto red_a = source.decode(sequence, 0U);
    const auto red_b = source.decode(sequence, 0U);
    SOURCE_CHECK(red_a.valid());
    SOURCE_CHECK(red_a.pixels[0U] > 200U && red_a.pixels[1U] < 60U);
    SOURCE_CHECK(red_a.pixels == red_b.pixels);
    // The two frames are genuinely different pixels.
    SOURCE_CHECK(red_a.pixels != green.pixels);

    // The cache key is stable and distinguishes frames.
    SOURCE_CHECK(source.key(sequence, 0U) != source.key(sequence, 1U));
    SOURCE_CHECK(source.key(sequence, 0U) == source.key(sequence, 0U));
}

void a_frame_that_cannot_be_read_is_invalid() {
    render::ImageSequenceSource source;
    const auto frame = source.decode("mediaserver_no_such_file_####.png", 3U);
    SOURCE_CHECK(!frame.valid());
    // An empty source names nothing.
    SOURCE_CHECK(source.decode("", 0U).valid() == false);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 4U> cases{{
    {"the frame field is substituted not formatted",
     &the_frame_field_is_substituted_not_formatted},
    {"a frame decodes to its pixels", &a_frame_decodes_to_its_pixels},
    {"decode is frame-exact and reachable in any order",
     &decode_is_frame_exact_and_reachable_in_any_order},
    {"a frame that cannot be read is invalid",
     &a_frame_that_cannot_be_read_is_invalid},
}};

} // namespace

int main() {
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
