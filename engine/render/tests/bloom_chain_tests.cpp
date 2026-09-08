#include <mediaserver/render/bloom_chain.hpp>
#include <mediaserver/render/slice_apron.hpp>

#include <array>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define BLOOM_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// The depth is a function of the canvas and nothing else.
//
// This is the property the whole file exists for. A chain length taken from the
// local target would give a node holding a narrow slice a shorter chain than
// its neighbour, and the two halves of the wall would bloom to different radii
// - a difference no apron can repair, because it is not a boundary artefact but
// two different filters.
void the_depth_belongs_to_the_canvas() {
    const scene::Canvas canvas{1920U, 1080U};
    const core::u32 depth = render::bloom_chain_depth(canvas);
    BLOOM_CHECK(depth == render::max_bloom_levels);

    // Every slice of it, however narrow, reports the same.
    const core::u32 stride = 1U << depth;
    for (core::u32 x = 0U; x + stride <= canvas.width; x += stride) {
        const auto plan = render::plan_bloom_chain(
            canvas, scene::CanvasRect{x, 0U, stride, canvas.height});
        BLOOM_CHECK(plan.has_value());
        BLOOM_CHECK(plan.value().depth() == depth);
    }

    // A small canvas halves fewer times, and stops before a level stops
    // carrying anything a blur would spread.
    BLOOM_CHECK(render::bloom_chain_depth(scene::Canvas{256U, 128U}) == 4U);
    BLOOM_CHECK(render::bloom_chain_depth(scene::Canvas{16U, 16U}) == 1U);
    BLOOM_CHECK(render::bloom_chain_depth(scene::Canvas{8U, 8U}) == 0U);
    // A canvas that cannot be halved usefully has no chain, which is a chain of
    // nothing rather than a refusal - a tiny preview surface is not an error.
    const auto tiny = render::plan_bloom_chain(
        scene::Canvas{8U, 8U}, scene::CanvasRect{0U, 0U, 8U, 8U});
    BLOOM_CHECK(tiny.has_value());
    BLOOM_CHECK(tiny.value().levels.empty());
    BLOOM_CHECK(tiny.value().depth() == 0U);
}

// Two slices partition the canvas exactly as one whole-canvas render does.
//
// Asserted on the grid itself rather than on rendered pixels, because that is
// where the property lives: if the block boundaries agree the averages agree,
// and if they do not, no comparison of output will explain why.
void two_slices_and_the_whole_share_one_grid() {
    const scene::Canvas canvas{1024U, 512U};
    const core::u32 depth = render::bloom_chain_depth(canvas);
    const auto whole = render::plan_bloom_chain(
        canvas, scene::CanvasRect{0U, 0U, canvas.width, canvas.height});
    BLOOM_CHECK(whole.has_value());

    const core::u32 half = canvas.width / 2U;
    const auto left = render::plan_bloom_chain(
        canvas, scene::CanvasRect{0U, 0U, half, canvas.height});
    const auto right = render::plan_bloom_chain(
        canvas, scene::CanvasRect{half, 0U, half, canvas.height});
    BLOOM_CHECK(left.has_value());
    BLOOM_CHECK(right.has_value());
    BLOOM_CHECK(left.value().depth() == whole.value().depth());
    BLOOM_CHECK(right.value().depth() == whole.value().depth());

    // At every level the halves cover the whole exactly: the same texel count
    // end to end, and each half abuts its neighbour without overlapping it.
    for (core::u32 level = 0U; level <= depth; ++level) {
        const render::BloomLevel& all = whole.value().levels[level];
        const render::BloomLevel& a = left.value().levels[level];
        const render::BloomLevel& b = right.value().levels[level];
        BLOOM_CHECK(a.width + b.width == all.width);
        BLOOM_CHECK(a.height == all.height);
        BLOOM_CHECK(b.height == all.height);
        BLOOM_CHECK(a.canvas.x + a.canvas.width == b.canvas.x);
    }
}

// A rectangle off the coarsest level's grid is refused, not resampled.
void an_unaligned_rectangle_is_refused() {
    const scene::Canvas canvas{1024U, 512U};
    const core::u32 stride = 1U << render::bloom_chain_depth(canvas);
    BLOOM_CHECK(stride == 64U);

    // One pixel off is off. This is the case that would otherwise produce a
    // perfectly plausible chain whose every level averages a different set of
    // canvas pixels from its neighbour.
    const auto skewed = render::plan_bloom_chain(
        canvas, scene::CanvasRect{stride + 1U, 0U, stride, canvas.height});
    BLOOM_CHECK(!skewed);
    BLOOM_CHECK(skewed.error() == render::BloomChainError::Unaligned);

    const auto short_end = render::plan_bloom_chain(
        canvas, scene::CanvasRect{0U, 0U, stride - 1U, canvas.height});
    BLOOM_CHECK(!short_end);
    BLOOM_CHECK(short_end.error() == render::BloomChainError::Unaligned);

    // A rectangle running to the canvas edge is aligned there whatever its
    // width, because the short last block is short for every node.
    const scene::Canvas ragged{1000U, 500U};
    const core::u32 ragged_stride = 1U << render::bloom_chain_depth(ragged);
    const auto tail = render::plan_bloom_chain(
        ragged,
        scene::CanvasRect{ragged_stride, 0U, ragged.width - ragged_stride,
                          ragged.height});
    BLOOM_CHECK(tail.has_value());
    const auto ragged_whole = render::plan_bloom_chain(
        ragged, scene::CanvasRect{0U, 0U, ragged.width, ragged.height});
    BLOOM_CHECK(ragged_whole.has_value());
    // The ragged edge produces the same texels either way: the leading block
    // plus the tail is the whole canvas.
    for (core::u32 level = 0U; level <= ragged_whole.value().depth(); ++level) {
        BLOOM_CHECK(tail.value().levels[level].width
                    <= ragged_whole.value().levels[level].width);
        BLOOM_CHECK(tail.value().levels[level].height
                    == ragged_whole.value().levels[level].height);
    }
}

// The apron is the chain's real reach, stated in canvas pixels.
void the_apron_covers_the_whole_chain() {
    BLOOM_CHECK(render::bloom_chain_apron(0U) == 0U);
    BLOOM_CHECK(render::bloom_chain_apron(1U) == 4U);
    // 252 is the kernels' actual reach; 256 is that rounded up to four whole
    // blocks, which is what keeps a padded rectangle on the grid.
    BLOOM_CHECK(render::bloom_chain_apron(6U) == 256U);

    // Every depth lands on a whole number of its own blocks. This is the
    // property, not the individual numbers above.
    for (core::u32 depth = 1U; depth <= render::max_bloom_levels; ++depth) {
        const core::u32 stride = 1U << depth;
        BLOOM_CHECK(render::bloom_chain_apron(depth) % stride == 0U);
        BLOOM_CHECK(render::bloom_chain_apron(depth) >= 4U * (stride - 1U));
    }

    // It grows with the chain, and the default apron budget pays for exactly
    // the deepest chain this file will build. The two constants are related on
    // purpose; if either moves without the other, this fails.
    BLOOM_CHECK(render::bloom_chain_apron(render::max_bloom_levels)
                <= scene::RendererSettings{}.slice_apron_limit);
    for (core::u32 depth = 1U; depth <= render::max_bloom_levels; ++depth) {
        BLOOM_CHECK(render::bloom_chain_apron(depth)
                    > render::bloom_chain_apron(depth - 1U));
    }
    BLOOM_CHECK(render::bloom_chain_alignment(scene::Canvas{2560U, 720U})
                == 64U);
    BLOOM_CHECK(render::bloom_chain_alignment(scene::Canvas{8U, 8U}) == 0U);
}

// The geometry the shipping two-node demo actually uses.
//
// `scripts/run-scene-demo.ps1` splits a 2560x720 canvas into two 1280x720
// slices, which is the closest thing this repository has to a real wall. The
// grid tests above use small canvases with obliging numbers; this one exists
// because a chain that only aligns on obliging numbers is a chain that refuses
// every frame of the demo, and nothing else here would have caught that.
//
// The padded rectangle is what has to align, not the authored slice - so the
// apron has to be a whole number of coarsest-level blocks, and that is a
// property of `apron_requirements` rather than of the planner.
void the_two_node_demo_geometry_plans() {
    const scene::Canvas canvas{2560U, 720U};
    const core::u32 depth = render::bloom_chain_depth(canvas);
    BLOOM_CHECK(depth == render::max_bloom_levels);

    scene::Scene value{};
    value.canvas = canvas;
    value.renderer_settings.backend = scene::RendererBackend::Advanced;
    value.renderer_settings.bloom_strength = 0.5F;

    for (core::u32 node = 0U; node < 2U; ++node) {
        const scene::CanvasRect slice{node * 1280U, 0U, 1280U, 720U};
        const auto padded =
            render::padded_slice_for(value, slice, slice.width, slice.height,
                                     true);
        BLOOM_CHECK(padded.has_value());
        const auto plan =
            render::plan_bloom_chain(canvas, padded.value().render);
        BLOOM_CHECK(plan.has_value());
        BLOOM_CHECK(plan.value().depth() == depth);
    }

    // The apron is a whole number of blocks, which is what made the above
    // work. Stated separately so a change to either number fails here rather
    // than somewhere far away.
    const core::u32 stride = 1U << depth;
    BLOOM_CHECK(render::bloom_chain_apron(depth) % stride == 0U);
    BLOOM_CHECK(render::bloom_chain_apron(depth)
                <= scene::RendererSettings{}.slice_apron_limit);
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 5U> cases{{
    {"the depth belongs to the canvas", &the_depth_belongs_to_the_canvas},
    {"two slices and the whole share one grid",
     &two_slices_and_the_whole_share_one_grid},
    {"an unaligned rectangle is refused", &an_unaligned_rectangle_is_refused},
    {"the apron covers the whole chain", &the_apron_covers_the_whole_chain},
    {"the two node demo geometry plans", &the_two_node_demo_geometry_plans},
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
