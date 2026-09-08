#include <mediaserver/render/display_lut_cache.hpp>
#include <mediaserver/render/display_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace render = mediaserver::render;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define LUT_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] bool near(float a, float b, float tolerance) noexcept {
    return std::abs(a - b) <= tolerance;
}

// An identity look: sampling it returns what it was given.
[[nodiscard]] render::DisplayLut identity_cube(core::u32 size) {
    render::DisplayLut lut{};
    lut.size = size;
    lut.values.assign(
        static_cast<std::size_t>(size) * size * size * 3U, 0.0F);
    const auto steps = static_cast<float>(size - 1U);
    for (core::u32 b = 0U; b < size; ++b) {
        for (core::u32 g = 0U; g < size; ++g) {
            for (core::u32 r = 0U; r < size; ++r) {
                const std::size_t index =
                    ((static_cast<std::size_t>(b) * size + g) * size + r) * 3U;
                lut.values[index] = static_cast<float>(r) / steps;
                lut.values[index + 1U] = static_cast<float>(g) / steps;
                lut.values[index + 2U] = static_cast<float>(b) / steps;
            }
        }
    }
    return lut;
}

// The shaper covers the whole positive range and inverts exactly.
//
// It is the reason the table can represent scene-linear values above one at
// all. A table indexed by raw linear would either clip every highlight or
// spend most of its grid points on values no show produces.
void the_shaper_covers_the_range_and_inverts() {
    LUT_CHECK(render::display_shaper(0.0F) == 0.0F);
    // Negative light is not a colour. Clamped rather than reflected, so a
    // stray negative from a filter cannot index the table backwards.
    LUT_CHECK(render::display_shaper(-5.0F) == 0.0F);
    // Monotonic, which is what makes interpolation between grid points mean
    // anything at all.
    float previous = -1.0F;
    for (int step = 0; step <= 200; ++step) {
        const float linear = static_cast<float>(step) * 0.5F;
        const float shaped = render::display_shaper(linear);
        LUT_CHECK(shaped > previous);
        LUT_CHECK(shaped >= 0.0F && shaped <= 1.0F);
        previous = shaped;
    }
    // Round trips over the range a show actually uses.
    for (const float linear : {0.0F, 0.18F, 1.0F, 4.0F, 16.0F, 100.0F}) {
        const float back =
            render::display_shaper_inverse(render::display_shaper(linear));
        LUT_CHECK(near(back, linear, std::max(linear * 1.0e-4F, 1.0e-5F)));
    }
    // The top grid point stands for everything brighter, and is finite: an
    // infinity there would poison every interpolation that touched it.
    LUT_CHECK(std::isfinite(render::display_shaper_inverse(1.0F)));
    LUT_CHECK(render::display_shaper_inverse(1.0F) > 1000.0F);
}

// Baking is deterministic, and an identity look changes nothing.
void baking_is_deterministic_and_an_identity_look_is_transparent() {
    const render::DisplayLut a =
        render::bake_display_lut(render::DisplayTransform::AcesApproximate, 17U);
    const render::DisplayLut b =
        render::bake_display_lut(render::DisplayTransform::AcesApproximate, 17U);
    LUT_CHECK(a.valid());
    // Byte for byte, because two nodes bake this independently and a table
    // that differed in its last bit would grade the two halves of a wall
    // differently.
    LUT_CHECK(a == b);

    // Composing an identity look leaves the transform alone. The tolerance is
    // the look table's own interpolation error, not slack: the identity is
    // resampled through a 33-point grid on the way.
    const render::DisplayLut look = identity_cube(33U);
    const render::DisplayLut composed = render::bake_display_lut(
        render::DisplayTransform::AcesApproximate, 17U, &look);
    LUT_CHECK(composed.valid());
    for (std::size_t index = 0U; index < a.values.size(); ++index) {
        LUT_CHECK(near(composed.values[index], a.values[index], 2.0e-3F));
    }
}

// A known input through the sRGB transform lands where the maths says.
//
// The sRGB entry exists to be checkable by hand: it is the reference the tone
// curve is judged against, and the fallback when an authored transform fails
// to load.
void a_known_value_lands_where_the_transform_says() {
    const render::DisplayLut lut =
        render::bake_display_lut(render::DisplayTransform::Srgb, 65U);
    LUT_CHECK(lut.valid());

    const auto sampled = [&](float linear) {
        const float shaped = render::display_shaper(linear);
        const float input[3U]{shaped, shaped, shaped};
        float out[3U]{};
        render::sample_display_lut(lut, input, out);
        return out[0U];
    };

    // Black is black, and mid grey encodes to the sRGB value for it.
    LUT_CHECK(near(sampled(0.0F), 0.0F, 1.0e-3F));
    LUT_CHECK(near(sampled(0.18F), 0.4613F, 6.0e-3F));
    LUT_CHECK(near(sampled(1.0F), 1.0F, 6.0e-3F));
    // And it saturates rather than wrapping: a highlight far past one is still
    // white, not black.
    LUT_CHECK(near(sampled(64.0F), 1.0F, 6.0e-3F));

    // The tone curve is a different answer, and darker at the top - that is
    // what a shoulder is for.
    const render::DisplayLut aces =
        render::bake_display_lut(render::DisplayTransform::AcesApproximate, 65U);
    const float shaped = render::display_shaper(1.0F);
    const float input[3U]{shaped, shaped, shaped};
    float toned[3U]{};
    render::sample_display_lut(aces, input, toned);
    LUT_CHECK(toned[0U] < sampled(1.0F));
}

// A `.cube` file parses, and a malformed one is refused rather than half-read.
void a_cube_file_parses_and_a_broken_one_is_refused() {
    const std::string good =
        "# a comment\n"
        "TITLE \"look\"\n"
        "LUT_3D_SIZE 2\n"
        "\n"
        "0.0 0.0 0.0\n"
        "1.0 0.0 0.0\n"
        "0.0 1.0 0.0\n"
        "1.0 1.0 0.0\n"
        "0.0 0.0 1.0\n"
        "1.0 0.0 1.0\n"
        "0.0 1.0 1.0\n"
        "1.0 1.0 1.0\n";
    const render::DisplayLut lut = render::parse_cube_lut(good);
    LUT_CHECK(lut.valid());
    LUT_CHECK(lut.size == 2U);
    // Red varies fastest, which is the `.cube` ordering. Reading it the other
    // way round swaps a look's red and blue and is the kind of mistake that
    // looks deliberate.
    LUT_CHECK(lut.values[0U] == 0.0F);
    LUT_CHECK(lut.values[3U] == 1.0F);
    LUT_CHECK(lut.values[5U] == 0.0F);

    // Too few entries for the declared size, a missing size, and a 1D table
    // are all refused. A 1D table is a different transform, not a smaller
    // version of this one.
    LUT_CHECK(!render::parse_cube_lut("LUT_3D_SIZE 2\n0.0 0.0 0.0\n").valid());
    LUT_CHECK(!render::parse_cube_lut("0.0 0.0 0.0\n").valid());
    LUT_CHECK(!render::parse_cube_lut("LUT_1D_SIZE 2\n0 0 0\n1 1 1\n").valid());
    LUT_CHECK(!render::parse_cube_lut("LUT_3D_SIZE 2\nnonsense\n").valid());
}

// The atlas is the table, rearranged and nothing else.
void the_atlas_holds_every_entry_where_the_shader_looks() {
    const render::DisplayLut lut = identity_cube(9U);
    const std::vector<core::u8> atlas = render::pack_display_lut_atlas(lut);
    const core::u32 width = render::display_lut_atlas_width(9U);
    const core::u32 height = render::display_lut_atlas_height(9U);
    LUT_CHECK(width == 81U);
    LUT_CHECK(height == 9U);
    LUT_CHECK(atlas.size()
              == static_cast<std::size_t>(width) * height * 8U);

    // Slice `b` sits at columns `b * size`, which is the arithmetic the shader
    // performs. Checking one interior texel rather than a corner, because a
    // corner survives most indexing mistakes.
    constexpr core::u32 r = 3U;
    constexpr core::u32 g = 5U;
    constexpr core::u32 b = 7U;
    const std::size_t texel =
        (static_cast<std::size_t>(g) * width + (b * 9U + r)) * 8U;
    core::u16 bits{};
    std::memcpy(&bits, atlas.data() + texel, sizeof(bits));
    // 3/8 in binary16 is exact, so this compares a value rather than a
    // rounding.
    const float expected = 3.0F / 8.0F;
    const core::u32 sign = static_cast<core::u32>(bits & 0x8000U) << 16U;
    const core::u32 exponent = (bits >> 10U) & 0x1FU;
    const core::u32 mantissa = bits & 0x3FFU;
    const core::u32 assembled =
        sign | ((exponent + 112U) << 23U) | (mantissa << 13U);
    float decoded{};
    std::memcpy(&decoded, &assembled, sizeof(decoded));
    LUT_CHECK(near(decoded, expected, 1.0e-6F));
}

// An identity table returns its input, which is what makes "off" expressible.
void sampling_an_identity_table_returns_its_input() {
    const render::DisplayLut lut = identity_cube(33U);
    for (const float value : {0.0F, 0.25F, 0.5F, 0.75F, 1.0F, 0.137F}) {
        const float input[3U]{value, value * 0.5F, 1.0F - value};
        float out[3U]{};
        render::sample_display_lut(lut, input, out);
        LUT_CHECK(near(out[0U], input[0U], 1.0e-5F));
        LUT_CHECK(near(out[1U], input[1U], 1.0e-5F));
        LUT_CHECK(near(out[2U], input[2U], 1.0e-5F));
    }
    // Out of range indices clamp rather than reading past the table.
    const float high[3U]{2.0F, -1.0F, 0.5F};
    float out[3U]{};
    render::sample_display_lut(lut, high, out);
    LUT_CHECK(near(out[0U], 1.0F, 1.0e-5F));
    LUT_CHECK(near(out[1U], 0.0F, 1.0e-5F));
}

// The dither is a pure function of the wall pixel.
//
// Two nodes must produce the same threshold for the same canvas coordinate or
// the seam between them carries a line of differently rounded values. A node
// that seeded from its own target coordinate would pass every other test in
// this file and fail on a wall.
void the_dither_is_a_pure_function_of_the_wall_pixel() {
    std::array<bool, 64U> seen{};
    double sum = 0.0;
    for (core::i64 y = 0; y < 8; ++y) {
        for (core::i64 x = 0; x < 8; ++x) {
            const float threshold = render::output_dither_threshold(x, y);
            LUT_CHECK(threshold >= 0.0F && threshold < 1.0F);
            // Every cell of the matrix is a distinct level; a repeat would be
            // a level of the pattern lost.
            const auto level = static_cast<std::size_t>(
                std::lround(threshold * 64.0F - 0.5F));
            LUT_CHECK(level < seen.size());
            LUT_CHECK(!seen[level]);
            seen[level] = true;
            sum += threshold;
        }
    }
    // Averages exactly one half, so quantising with it is unbiased.
    LUT_CHECK(near(static_cast<float>(sum / 64.0), 0.5F, 1.0e-6F));

    // Periodic in both axes, and across the origin the same way the shader's
    // integer mask wraps.
    LUT_CHECK(render::output_dither_threshold(13, 5)
              == render::output_dither_threshold(13 + 8, 5 + 16));
    LUT_CHECK(render::output_dither_threshold(-1, 3)
              == render::output_dither_threshold(7, 3));
    LUT_CHECK(render::output_dither_threshold(-8, -8)
              == render::output_dither_threshold(0, 0));
    // A node whose slice starts at canvas x=1280 asks for its local pixel 3 as
    // canvas pixel 1283, and gets what the whole-canvas node gets there.
    LUT_CHECK(render::output_dither_threshold(1280 + 3, 40)
              == render::output_dither_threshold(1283, 40));

    // A value exactly on a code value stays there whatever the threshold; one
    // exactly halfway lands above on half the cells and below on the other
    // half, which is what unbiased means for a flat field.
    std::size_t above = 0U;
    for (core::i64 y = 0; y < 8; ++y) {
        for (core::i64 x = 0; x < 8; ++x) {
            const float threshold = render::output_dither_threshold(x, y);
            LUT_CHECK(render::quantize_with_dither(0.0F, threshold, 255U)
                      == 0.0F);
            LUT_CHECK(render::quantize_with_dither(1.0F, threshold, 255U)
                      == 1.0F);
            if (render::quantize_with_dither(0.5F, threshold, 255U)
                > 127.75F / 255.0F) {
                ++above;
            }
        }
    }
    LUT_CHECK(above == 32U);
    // Zero levels is no dither and no quantisation, which is what off means.
    LUT_CHECK(render::quantize_with_dither(0.3F, 0.7F, 0U) == 0.3F);
}

// A smooth ramp quantised with dither has no long flat runs.
//
// The banding check that needs no golden image: on a ramp sixteen pixels per
// code value, rounding alone flat-lines every one of them, and the dither is
// there to break those runs up. Measured on one row of the matrix, which is
// how a horizontal gradient meets it.
void a_ramp_quantised_with_dither_has_no_long_flat_runs() {
    constexpr int width = 4096;
    const auto longest_run = [](bool dithered) {
        int longest = 0;
        int run = 0;
        float previous = -1.0F;
        for (int x = 0; x < width; ++x) {
            const float value =
                static_cast<float>(x) / static_cast<float>(width - 1);
            const float threshold =
                dithered ? render::output_dither_threshold(x, 3) : 0.5F;
            const float quantised =
                render::quantize_with_dither(value, threshold, 255U);
            if (quantised == previous) {
                ++run;
            } else {
                run = 1;
                previous = quantised;
            }
            longest = std::max(longest, run);
        }
        return longest;
    };
    const int plain = longest_run(false);
    const int dithered = longest_run(true);
    LUT_CHECK(plain >= 16);
    LUT_CHECK(dithered < plain);
    // Twelve: three quarters of a band. On this row the smallest threshold is
    // about a third and the largest nearly one, so the last quarter of one band
    // and the first pixel of the next all round the same way, and nothing
    // shorter is reachable without a wider matrix. Measured, and the even rows
    // come in at ten.
    LUT_CHECK(dithered <= 12);
}

// The table is baked by the residency worker, off the frame path, and a look
// file reaches it - or fails to - in a way the caller can see.
//
// `wait_for_test` is the only wait in here and it is the test's, not the
// frame path's: an observation never blocks, and the first one after a new
// request reports Pending with no table rather than the bake it has not done.
void the_display_table_cache_bakes_off_the_frame_path() {
    namespace fs = std::filesystem;
    const fs::path directory =
        fs::temp_directory_path() / "mediaserver-display-lut-tests";
    fs::create_directories(directory);
    const fs::path look_path = directory / "half-red.cube";
    // A look that halves red and leaves the other two channels alone, red
    // varying fastest as the format requires.
    const std::string look_text =
        "LUT_3D_SIZE 2\n"
        "0.0 0.0 0.0\n"
        "0.5 0.0 0.0\n"
        "0.0 1.0 0.0\n"
        "0.5 1.0 0.0\n"
        "0.0 0.0 1.0\n"
        "0.5 0.0 1.0\n"
        "0.0 1.0 1.0\n"
        "0.5 1.0 1.0\n";
    {
        std::ofstream out{look_path, std::ios::binary};
        out << look_text;
    }

    render::DisplayLutCache cache;
    const render::DisplayLutRequest request{
        render::DisplayTransform::Srgb, look_path.string(), 17U};
    const auto first = cache.observe(request);
    LUT_CHECK(first.state == render::DisplayLutState::Pending);
    LUT_CHECK(!first.ready());
    LUT_CHECK(cache.wait_for_test());
    const auto ready = cache.observe(request);
    LUT_CHECK(ready.state == render::DisplayLutState::Ready);
    LUT_CHECK(ready.ready());
    // Byte for byte the composed bake a node would perform itself, because
    // that is what every node does perform.
    const render::DisplayLut look = render::parse_cube_lut(look_text);
    LUT_CHECK(look.valid());
    const render::DisplayLut expected =
        render::bake_display_lut(render::DisplayTransform::Srgb, 17U, &look);
    LUT_CHECK(*ready.table == expected);
    // And the look did something: white through it has half the red.
    const float shaped = render::display_shaper(1.0F);
    const float white[3U]{shaped, shaped, shaped};
    float graded[3U]{};
    render::sample_display_lut(*ready.table, white, graded);
    LUT_CHECK(near(graded[0U], 0.5F, 1.0e-2F));
    LUT_CHECK(near(graded[1U], 1.0F, 1.0e-2F));

    // A path that does not exist: Failed, with the transform alone published
    // so the wall shows a predictable picture while the path is fixed.
    const render::DisplayLutRequest missing{
        render::DisplayTransform::Srgb,
        (directory / "does-not-exist.cube").string(), 17U};
    LUT_CHECK(cache.observe(missing).state == render::DisplayLutState::Pending);
    LUT_CHECK(cache.wait_for_test());
    const auto failed = cache.observe(missing);
    LUT_CHECK(failed.state == render::DisplayLutState::Failed);
    LUT_CHECK(failed.ready());
    LUT_CHECK(*failed.table
              == render::bake_display_lut(render::DisplayTransform::Srgb, 17U));
    LUT_CHECK(cache.stats().failures >= 1U);

    // No look at all bakes without touching a disk.
    const render::DisplayLutRequest plain{
        render::DisplayTransform::AcesApproximate, std::string{}, 17U};
    LUT_CHECK(cache.observe(plain).state == render::DisplayLutState::Pending);
    LUT_CHECK(cache.wait_for_test());
    const auto baked = cache.observe(plain);
    LUT_CHECK(baked.state == render::DisplayLutState::Ready);
    LUT_CHECK(*baked.table
              == render::bake_display_lut(
                  render::DisplayTransform::AcesApproximate, 17U));

    // A refresh moves the generation on, so a caller that adopted the old
    // table adopts the new one rather than assuming nothing changed.
    LUT_CHECK(cache.refresh(plain));
    LUT_CHECK(cache.wait_for_test());
    LUT_CHECK(cache.observe(plain).generation > baked.generation);

    std::error_code ignored;
    fs::remove_all(directory, ignored);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 9U> cases{{
    {"the shaper covers the range and inverts",
     &the_shaper_covers_the_range_and_inverts},
    {"baking is deterministic and an identity look is transparent",
     &baking_is_deterministic_and_an_identity_look_is_transparent},
    {"a known value lands where the transform says",
     &a_known_value_lands_where_the_transform_says},
    {"a cube file parses and a broken one is refused",
     &a_cube_file_parses_and_a_broken_one_is_refused},
    {"the atlas holds every entry where the shader looks",
     &the_atlas_holds_every_entry_where_the_shader_looks},
    {"sampling an identity table returns its input",
     &sampling_an_identity_table_returns_its_input},
    {"the dither is a pure function of the wall pixel",
     &the_dither_is_a_pure_function_of_the_wall_pixel},
    {"a ramp quantised with dither has no long flat runs",
     &a_ramp_quantised_with_dither_has_no_long_flat_runs},
    {"the display table cache bakes off the frame path",
     &the_display_table_cache_bakes_off_the_frame_path},
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
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
