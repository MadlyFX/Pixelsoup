#pragma once

#include <mediaserver/core/result.hpp>
#include <mediaserver/core/types.hpp>
#include <mediaserver/scene/scene.hpp>

#include <string_view>
#include <string>
#include <vector>

// The display transform, baked.
//
// Everything upstream of here makes the render *correct*; this is what makes it
// match on camera, which is the only place correctness is judged. The chain on
// a volume is renderer, output transform, LED processor, panel, lens, sensor,
// camera IDT, editorial - and a wrong link comes back as a colour cast the DP
// notices on the day.
//
// **Baked, not evaluated.** A display pipeline is a graph of transforms and
// evaluating one per pixel is not frame-path work. It is sampled through one
// lookup table built once, off the frame path, so the shader does a lookup and
// nothing else - and so the transform can be replaced by a measured one
// without touching a shader.
namespace mediaserver::render {

// The authored enumeration, not a copy of it. The scene owns the choice
// because it is replicated project state; this library only bakes it. A second
// enum here would need a mapping, and a mapping is a place two nodes can
// disagree.
using DisplayTransform = scene::DisplayTransform;

// Thirty-three points a side is the size a colourist's `.cube` almost always
// is, and the size a measured bake is usually delivered at. Matching it means
// an authored table replaces this one without resampling.
inline constexpr core::u32 default_display_lut_size = 33U;

// A cube of display values, indexed by a shaped scene-linear colour.
//
// Stored as one flat array with red varying fastest, which is the ordering
// `.cube` files use and the ordering the atlas upload below assumes.
struct DisplayLut final {
    core::u32 size{};
    // `size^3 * 3` values.
    std::vector<float> values;

    [[nodiscard]] bool valid() const noexcept {
        return size >= 2U
            && values.size()
                == static_cast<std::size_t>(size) * size * size * 3U;
    }
    bool operator==(const DisplayLut&) const = default;
};

// Where the table spends its resolution.
//
// The shaper is `x / (x + k)`, monotonic over the whole positive range and
// exactly invertible, so the table covers every scene-linear value without a
// separate clamp. `k` is the linear value that lands halfway along the table,
// so half the grid points describe everything below it.
//
// One rather than something larger: at four, four fifths of the table would
// describe highlights above diffuse white and the entire range a shot is
// actually graded in would share the remaining fifth. Diffuse white is the
// value the eye and the camera both care about most, so it sits in the middle.
inline constexpr float display_shaper_pivot = 1.0F;

// Scene-linear to the table's index domain, and back. Both here so a shader
// that forgets one cannot silently disagree with the bake that used the other.
[[nodiscard]] float display_shaper(float linear) noexcept;
[[nodiscard]] float display_shaper_inverse(float shaped) noexcept;

// Builds the table for one transform, optionally composed with an authored
// show LUT applied *after* it.
//
// Composed rather than sampled separately because two lookups is two chances
// for two nodes to disagree, and because the frame path should do one.
[[nodiscard]] DisplayLut bake_display_lut(
    DisplayTransform transform, core::u32 size,
    const DisplayLut* show_lut = nullptr);

// Parses a `.cube` file. Returns an invalid table rather than throwing: a bad
// look file is an authored mistake to report, not a crash.
[[nodiscard]] DisplayLut parse_cube_lut(std::string_view text);

// Samples the table the way the shader does - manual trilinear, fixed
// operation order. Shared so a test can assert the shader's arithmetic without
// a GPU, and so the two cannot drift.
void sample_display_lut(
    const DisplayLut& lut, const float linear[3U], float out[3U]) noexcept;

// The atlas the shader samples: `size` slices of `size` by `size`, laid out
// left to right. A 2D atlas rather than a 3D texture because the filtering has
// to be done by hand anyway - a hardware sampler's precision is not guaranteed
// identical between implementations, and this value feeds a wall that two
// nodes each render half of. With the filtering manual, a 3D texture buys
// nothing here and costs an RHI change.
[[nodiscard]] core::u32 display_lut_atlas_width(core::u32 size) noexcept;
[[nodiscard]] core::u32 display_lut_atlas_height(core::u32 size) noexcept;

// Packs the table into `RGBA16F` atlas texels, ready to upload.
[[nodiscard]] std::vector<core::u8> pack_display_lut_atlas(
    const DisplayLut& lut);

// Dither before quantisation, at the output boundary.
//
// Ten-bit output of a smooth gradient bands visibly on a large wall, and
// eight-bit output bands worse; banding on a wall is one of the cheapest ways
// to make an expensive volume look cheap. The pattern is a fixed 8x8 ordered
// matrix indexed by the **absolute canvas pixel**, never by this node's own
// target coordinate: two nodes must produce the same threshold for the same
// wall pixel or the seam between them carries a visible line of differently
// rounded values.
//
// Static rather than animated. Temporally varying dither hides banding better
// for an eye, but a camera integrating over a shutter does not necessarily
// average it the way an eye does, and a pattern that has to be tested on
// camera before it ships should be an authored option rather than the default.
//
// The threshold is in [0, 1) and averages exactly one half over the matrix, so
// quantising with it is unbiased: a flat field lands on the same code value
// rounding would have produced. `output_dither_period` is the matrix side;
// the shader carries the same matrix, and the GPU output test asserts the two
// agree by comparing pixels against this reference.
inline constexpr core::u32 output_dither_period = 8U;
[[nodiscard]] float output_dither_threshold(core::i64 canvas_x,
                                            core::i64 canvas_y) noexcept;
// One channel, encoded in [0, 1], quantised to `levels` steps with the
// threshold above. `levels` is the output format's own step count - 255 for
// an eight-bit target - because dithering to a depth the swapchain does not
// carry throws away a bit of precision for nothing.
[[nodiscard]] float quantize_with_dither(float encoded, float threshold,
                                         core::u32 levels) noexcept;

} // namespace mediaserver::render
