#include <mediaserver/scene/canvas.hpp>
#include <mediaserver/scene/math.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>

namespace {

namespace core = mediaserver::core;
namespace scene = mediaserver::scene;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{
            std::string{"line "} + std::to_string(line)
            + ": check failed: " + expression};
    }
}

#define SCENE_CHECK(...) \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] bool near(float actual, float expected, float tolerance) noexcept {
    return std::abs(actual - expected) <= tolerance;
}

// FNV-1a over the bit patterns of a running sequence of results. Used to pin
// the arithmetic itself rather than any one value: if a toolchain change moves
// a single ulp anywhere in the sequence, this moves.
class BitDigest final {
public:
    void operator()(float value) noexcept {
        core::u32 bits{};
        std::memcpy(&bits, &value, sizeof(bits));
        for (core::u32 shift = 0U; shift < 32U; shift += 8U) {
            state_ ^= (bits >> shift) & 0xffU;
            state_ *= 1'099'511'628'211ULL;
        }
    }

    void operator()(const scene::Mat4& matrix) noexcept {
        for (const auto element : matrix.m) {
            (*this)(element);
        }
    }

    [[nodiscard]] core::u64 value() const noexcept { return state_; }

private:
    core::u64 state_{14'695'981'039'346'656'037ULL};
};

void the_deterministic_trig_agrees_with_the_library_it_replaces() {
    // The point of the replacement is reproducibility, not a different answer.
    // Anything further than this from the platform's own sine would mean the
    // polynomial is wrong rather than merely pinned.
    for (int step = -1'000; step <= 1'000; ++step) {
        const auto turns = static_cast<float>(step) * 0.003F;
        const auto radians = static_cast<double>(turns) * 6.283185307179586;
        SCENE_CHECK(near(
            scene::deterministic::sin_turns(turns),
            static_cast<float>(std::sin(radians)),
            2.0e-6F));
        SCENE_CHECK(near(
            scene::deterministic::cos_turns(turns),
            static_cast<float>(std::cos(radians)),
            2.0e-6F));
    }
}

void the_quadrant_boundaries_are_exact_where_they_should_be() {
    SCENE_CHECK(scene::deterministic::sin_turns(0.0F) == 0.0F);
    SCENE_CHECK(scene::deterministic::cos_turns(0.0F) == 1.0F);
    // A quarter turn reduces to a remainder of exactly zero, so these come out
    // of the polynomial as exact 1 and 0 rather than as approximations.
    SCENE_CHECK(scene::deterministic::sin_turns(0.25F) == 1.0F);
    SCENE_CHECK(scene::deterministic::cos_turns(0.25F) == 0.0F);
    SCENE_CHECK(scene::deterministic::sin_turns(0.5F) == 0.0F);
    SCENE_CHECK(scene::deterministic::cos_turns(0.5F) == -1.0F);
    SCENE_CHECK(scene::deterministic::sin_turns(0.75F) == -1.0F);
    SCENE_CHECK(scene::deterministic::cos_turns(0.75F) == 0.0F);

    // Periodicity is exact for integral turns, which is what keeps a long run
    // from drifting away from where it started.
    SCENE_CHECK(scene::deterministic::sin_turns(4.0F) == 0.0F);
    SCENE_CHECK(scene::deterministic::cos_turns(-3.0F) == 1.0F);
    SCENE_CHECK(scene::deterministic::cos_turns(-3.5F) == -1.0F);
}

void a_field_of_view_outside_the_representable_range_reports_zero() {
    // Half a turn is a 180 degree field of view: the near-plane window is
    // infinite. Anything at or past it has to be refused rather than turned
    // into an infinity inside a matrix.
    SCENE_CHECK(scene::deterministic::tan_half_turns(0.5F) == 0.0F);
    SCENE_CHECK(scene::deterministic::tan_half_turns(0.75F) == 0.0F);
    SCENE_CHECK(scene::deterministic::tan_half_turns(0.0F) == 0.0F);
    // 90 degrees: the half-angle is 45, whose tangent is one.
    SCENE_CHECK(near(scene::deterministic::tan_half_turns(0.25F), 1.0F, 1.0e-6F));

    // Every field of view strictly inside half a turn, against the library.
    //
    // This range used to stop being correct at 90 degrees: past that the
    // half-angle reduces into a different quadrant, and reading the sine and
    // cosine polynomials straight off produced a *negative* tangent. Nothing
    // ever rendered with one - `valid()` rejects a non-positive tangent, so
    // such a camera was refused outright - but a short lens on a volume
    // genuinely reaches those angles, so it is now computed rather than
    // refused. Everything below 90 degrees is untouched.
    for (int step = 1; step < 200; ++step) {
        const auto turns = static_cast<float>(step) * 0.0025F;
        const auto computed = scene::deterministic::tan_half_turns(turns);
        const auto expected = static_cast<float>(
            std::tan(std::numbers::pi_v<double> * static_cast<double>(turns)));
        SCENE_CHECK(near(computed, expected,
                         std::abs(expected) * 1.0e-5F + 1.0e-6F));
        // A usable field of view is a positive tangent, all the way up.
        SCENE_CHECK(computed > 0.0F);
    }

    // And the camera that follows from a wide one is now buildable, which is
    // the whole operator-visible consequence.
    scene::Camera wide{};
    wide.field_of_view = 0.375F; // 135 degrees
    SCENE_CHECK(scene::valid(wide));
}

void an_arctangent_inverts_the_tangent_it_is_paired_with() {
    // This is what turns a lens into a field of view: half the sensor height
    // over the focal length is a tangent, and its angle is what the camera
    // sees. It has to agree with the tangent used to build the projection, or
    // an authored lens and the frustum drawn from it would describe different
    // cameras.
    for (int step = -2'000; step <= 2'000; ++step) {
        const auto value = static_cast<float>(step) * 0.05F;
        const auto turns = scene::deterministic::atan_turns(value);
        const auto expected = static_cast<float>(
            std::atan(static_cast<double>(value))
            / (2.0 * std::numbers::pi_v<double>));
        SCENE_CHECK(near(turns, expected, 1.0e-7F));
        // The result is an angle in the open quarter turn an arctangent must
        // land in, whatever it was handed.
        SCENE_CHECK(turns > -0.25F && turns < 0.25F);
    }

    SCENE_CHECK(scene::deterministic::atan_turns(0.0F) == 0.0F);
    // Exactly an eighth of a turn - 45 degrees - is where the reduction hands
    // over, so it is the one value worth naming.
    SCENE_CHECK(near(scene::deterministic::atan_turns(1.0F), 0.125F, 1.0e-7F));
    SCENE_CHECK(near(scene::deterministic::atan_turns(-1.0F), -0.125F, 1.0e-7F));
    // Round tripping through the projection's own tangent closes the loop.
    for (int step = 1; step < 100; ++step) {
        const auto turns = static_cast<float>(step) * 0.0045F;
        const auto tangent = scene::deterministic::tan_half_turns(turns);
        const auto recovered = scene::deterministic::atan_turns(tangent);
        SCENE_CHECK(near(recovered * 2.0F, turns, 1.0e-5F));
    }

    SCENE_CHECK(scene::deterministic::atan_turns(
                    std::numeric_limits<float>::quiet_NaN()) == 0.0F);
    SCENE_CHECK(scene::deterministic::atan_turns(
                    std::numeric_limits<float>::infinity()) == 0.0F);
}

void exposure_stops_are_deterministic_powers_of_two() {
    SCENE_CHECK(scene::deterministic::stop_multiplier(-2.0F) == 0.25F);
    SCENE_CHECK(scene::deterministic::stop_multiplier(-1.0F) == 0.5F);
    SCENE_CHECK(scene::deterministic::stop_multiplier(0.0F) == 1.0F);
    SCENE_CHECK(scene::deterministic::stop_multiplier(1.0F) == 2.0F);
    SCENE_CHECK(scene::deterministic::stop_multiplier(8.0F) == 256.0F);
    SCENE_CHECK(near(
        scene::deterministic::stop_multiplier(0.5F),
        1.41421356237F, 2.0e-7F));
    SCENE_CHECK(scene::deterministic::stop_multiplier(100.0F) == 0x1p24F);
    SCENE_CHECK(scene::deterministic::stop_multiplier(-100.0F) == 0x1p-24F);
    SCENE_CHECK(scene::deterministic::stop_multiplier(
                    std::numeric_limits<float>::quiet_NaN()) == 1.0F);
}

void wrapping_keeps_precision_over_a_run_no_show_will_reach() {
    // One turn per second for a hundred hours. Held as a raw float the
    // fractional part would be gone; wrapped, it is exact to the same
    // precision as the first second.
    const auto turns = scene::wrap_turns(360'000.25);
    SCENE_CHECK(turns == 0.25F);
    SCENE_CHECK(scene::wrap_turns(-0.25) == 0.75F);
    SCENE_CHECK(scene::wrap_turns(0.0) == 0.0F);
    // A wrapped angle is always inside one turn, whatever it started as.
    for (int step = 0; step < 200; ++step) {
        const auto raw = static_cast<double>(step) * 1'234.567;
        const auto wrapped = scene::wrap_turns(raw);
        SCENE_CHECK(wrapped >= 0.0F && wrapped < 1.0F);
    }
}

void matrix_products_behave_like_the_transforms_they_encode() {
    const auto move = scene::translation(scene::Vec3{1.0F, 2.0F, 3.0F});
    const auto grow = scene::scaling(scene::Vec3{2.0F, 2.0F, 2.0F});
    const auto composed = scene::multiply(move, grow);

    // Scale first, then translate: the composition applies right to left.
    const auto point = scene::transform_point(composed, scene::Vec3{1.0F, 1.0F, 1.0F});
    SCENE_CHECK(point == (scene::Vec3{3.0F, 4.0F, 5.0F}));

    SCENE_CHECK(scene::multiply(scene::identity(), move) == move);
    SCENE_CHECK(scene::multiply(move, scene::identity()) == move);
}

void an_affine_inverse_round_trips_a_point() {
    const auto world = scene::multiply(
        scene::translation(scene::Vec3{-4.0F, 7.5F, 2.25F}),
        scene::multiply(
            scene::rotation(0.125F, 0.0625F, 0.375F),
            scene::scaling(scene::Vec3{2.0F, 0.5F, 3.0F})));
    const auto inverse = scene::invert_affine(world);

    const scene::Vec3 original{1.5F, -2.0F, 0.75F};
    const auto there = scene::transform_point(world, original);
    const auto back = scene::transform_point(inverse, there);
    SCENE_CHECK(near(back.x, original.x, 1.0e-4F));
    SCENE_CHECK(near(back.y, original.y, 1.0e-4F));
    SCENE_CHECK(near(back.z, original.z, 1.0e-4F));

    // A singular basis is a zero scale in the document. Identity is the
    // documented answer; an infinity would propagate into a frame.
    SCENE_CHECK(
        scene::invert_affine(scene::scaling(scene::Vec3{}))
        == scene::identity());
}

void a_normal_stays_perpendicular_under_non_uniform_scale() {
    // The case a plain basis gets wrong. A tangent and a normal that start
    // perpendicular must stay perpendicular once the object is squashed.
    const auto world = scene::multiply(
        scene::rotation(0.2F, 0.1F, 0.0F),
        scene::scaling(scene::Vec3{4.0F, 0.25F, 1.0F}));
    const auto normals = scene::normal_basis(world);

    // Deliberately off the principal axes. A tangent aligned with one of the
    // scale axes stays perpendicular even under the naive transform, so it
    // would not discriminate between the two.
    const auto tangent = scene::normalize(scene::Vec3{1.0F, 1.0F, 0.0F});
    const auto normal = scene::normalize(scene::Vec3{1.0F, -1.0F, 0.0F});
    SCENE_CHECK(near(scene::dot(tangent, normal), 0.0F, 1.0e-6F));

    const auto moved_tangent = scene::transform_point(world, tangent);
    const auto moved_normal = scene::normalize(
        scene::transform_point(normals, normal));
    SCENE_CHECK(near(
        scene::dot(scene::normalize(moved_tangent), moved_normal), 0.0F, 1.0e-4F));

    // The same test through the basis alone, which is what gets it wrong: this
    // is here so the assertion above is known to be discriminating.
    const auto naive = scene::normalize(scene::transform_point(world, normal));
    SCENE_CHECK(
        !near(scene::dot(scene::normalize(moved_tangent), naive), 0.0F, 1.0e-4F));
}

void rotating_about_an_axis_preserves_length_and_the_axis() {
    const scene::Vec3 axis{0.0F, 1.0F, 0.0F};
    const scene::Vec3 v{3.0F, 1.0F, 0.0F};
    const auto turned = scene::rotate_around(v, axis, 0.25F);
    SCENE_CHECK(near(scene::length(turned), scene::length(v), 1.0e-5F));
    // A quarter turn about +Y takes +X to -Z, and leaves the Y component be.
    SCENE_CHECK(near(turned.x, 0.0F, 1.0e-5F));
    SCENE_CHECK(near(turned.y, 1.0F, 1.0e-6F));
    SCENE_CHECK(near(turned.z, -3.0F, 1.0e-5F));

    // A full turn is the identity, exactly, because the quadrant reduction is.
    SCENE_CHECK(scene::rotate_around(v, axis, 1.0F) == v);
}

void a_view_matrix_puts_the_eye_at_the_origin_looking_down_minus_z() {
    const scene::Vec3 eye{4.0F, 3.0F, 5.0F};
    const scene::Vec3 target{0.0F, 1.0F, 0.0F};
    const auto view = scene::look_at(eye, target, scene::Vec3{0.0F, 1.0F, 0.0F});

    const auto at_eye = scene::transform_point(view, eye);
    SCENE_CHECK(near(at_eye.x, 0.0F, 1.0e-5F));
    SCENE_CHECK(near(at_eye.y, 0.0F, 1.0e-5F));
    SCENE_CHECK(near(at_eye.z, 0.0F, 1.0e-5F));

    const auto at_target = scene::transform_point(view, target);
    SCENE_CHECK(near(at_target.x, 0.0F, 1.0e-4F));
    SCENE_CHECK(near(at_target.y, 0.0F, 1.0e-4F));
    // In front of the camera is negative Z, at the distance between them.
    SCENE_CHECK(near(at_target.z, -scene::length(target - eye), 1.0e-4F));
}

// The golden. Every result below is folded into one digest, so a toolchain,
// flag, or platform that computes anything differently fails here rather than
// producing a seam on a wall months later.
//
// If this fails after an intentional change to the arithmetic, the fix is to
// re-record the constant *and to understand what moved*. If it fails without
// one, the build has stopped being reproducible and no scene should be trusted
// across nodes until it is. It is never correct to record a second value per
// platform: two platforms disagreeing is the fault, not the measurement.
//
// Recorded on MSVC 19.44 x64, and confirmed identical at /Od and at
// /O2 /arch:AVX2 - which is the check that matters, since AVX2 is where a
// compiler free to contract would start emitting fused multiply-adds.
//
// Re-recorded when `tan_half_turns` began applying its quadrant. The sweep
// above runs to a 0.4 turn field of view, and past 0.25 turns the old result
// was a negative tangent that `valid()` refused, so nothing that ever rendered
// moved. The test above proves that directly against the library.
constexpr core::u64 golden_arithmetic_digest = 0xe33a40b80963fdeeULL;

void the_arithmetic_matches_its_recorded_digest() {
    BitDigest digest;
    for (int step = -400; step <= 400; ++step) {
        const auto turns = static_cast<float>(step) * 0.0125F;
        digest(scene::deterministic::sin_turns(turns));
        digest(scene::deterministic::cos_turns(turns));
    }
    for (int step = 1; step <= 40; ++step) {
        digest(scene::deterministic::tan_half_turns(
            static_cast<float>(step) * 0.01F));
    }
    for (int step = -96; step <= 96; ++step) {
        digest(scene::deterministic::stop_multiplier(
            static_cast<float>(step) * 0.25F));
    }
    for (int step = 0; step < 24; ++step) {
        scene::Camera camera{};
        camera.aperture_f_number =
            1.4F + static_cast<float>(step) * 0.125F;
        camera.shutter_seconds =
            static_cast<float>(step + 1) / 240.0F;
        camera.iso = 50.0F * static_cast<float>(1 + step % 8);
        camera.exposure_compensation_ev =
            -3.0F + static_cast<float>(step) * 0.25F;
        digest(scene::camera_exposure_multiplier(camera));
    }
    scene::Camera clamped_camera{};
    clamped_camera.aperture_f_number = 0.01F;
    clamped_camera.shutter_seconds = 1'000.0F;
    clamped_camera.iso = 1'000'000.0F;
    clamped_camera.exposure_compensation_ev = 100.0F;
    digest(scene::camera_exposure_multiplier(clamped_camera));
    for (int step = 0; step < 32; ++step) {
        const auto phase = static_cast<float>(step) * 0.03125F;
        const auto world = scene::multiply(
            scene::translation(scene::Vec3{phase, phase * 2.0F, -phase}),
            scene::multiply(
                scene::rotation(phase, phase * 0.5F, phase * 0.25F),
                scene::scaling(
                    scene::Vec3{1.0F + phase, 2.0F - phase, 0.5F + phase})));
        digest(world);
        digest(scene::normal_basis(world));
        digest(scene::invert_affine(world));
        digest(scene::look_at(
            scene::Vec3{phase, 3.0F, 8.0F - phase},
            scene::Vec3{0.0F, 1.0F, 0.0F},
            scene::Vec3{0.0F, 1.0F, 0.0F}));
    }

    if (digest.value() != golden_arithmetic_digest) {
        // Printed rather than merely asserted so that re-recording an
        // intentional change does not require a debugger.
        std::cout << "  computed 0x" << std::hex << digest.value() << std::dec
                  << '\n';
    }
    SCENE_CHECK(digest.value() == golden_arithmetic_digest);
}

struct Case final {
    const char* name;
    void (*function)();
};

// The general inverse round-trips a perspective view-projection.
//
// `invert_affine` cannot do this one: a projection's bottom row is not
// (0,0,0,1), which is exactly the assumption that makes the affine version
// cheaper. The sky pass needs the general answer to turn a pixel back into a
// world-space ray.
void a_general_inverse_round_trips_a_projection() {
    scene::Camera camera{};
    camera.eye = scene::Vec3{2.0F, 3.0F, 4.0F};
    camera.target = scene::Vec3{0.0F, 0.0F, 0.0F};
    camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    const scene::Canvas canvas{1920U, 1080U};
    // An off-centre slice, so the projection is genuinely asymmetric. A
    // centred one is the case a wrong inverse still passes.
    const scene::CanvasRect slice{0U, 0U, 640U, 1080U};
    const scene::ViewMatrices matrices =
        scene::view_for_slice(camera, canvas, slice);
    const scene::Mat4 view_projection = matrices.view_projection;
    const scene::Mat4 inverted = scene::invert(view_projection);
    const scene::Mat4 identity = scene::multiply(view_projection, inverted);

    for (core::u32 row = 0U; row < 4U; ++row) {
        for (core::u32 column = 0U; column < 4U; ++column) {
            const float expected = row == column ? 1.0F : 0.0F;
            SCENE_CHECK(std::abs(identity.at(row, column) - expected) < 1.0e-3F);
        }
    }

    // A singular matrix is identity rather than a set of infinities that would
    // turn a sky ray into a NaN and every pixel it touches into black.
    scene::Mat4 singular{};
    SCENE_CHECK(scene::invert(singular) == scene::identity());
}

// The jitter sequence is a function of the show frame and nothing else.
//
// Every property here is a multi-node contract rather than an image-quality
// one. Two nodes rendering the same show frame must offset the projection
// identically, a repeated frame must reuse its offset rather than take the
// next sample, and the offsets must stay inside the pixel or the apron that
// step 05 sizes for them is the wrong size.
void the_jitter_sequence_is_a_function_of_the_show_frame() {
    // Halton's radical inverse, against values worked out by hand: the first
    // few terms of base two are 1/2, 1/4, 3/4, 1/8, and of base three 1/3,
    // 2/3, 1/9. These are exact in binary32 except the thirds, which are not.
    SCENE_CHECK(scene::halton(1U, 2U) == 0.5F);
    SCENE_CHECK(scene::halton(2U, 2U) == 0.25F);
    SCENE_CHECK(scene::halton(3U, 2U) == 0.75F);
    SCENE_CHECK(scene::halton(4U, 2U) == 0.125F);
    SCENE_CHECK(near(scene::halton(1U, 3U), 1.0F / 3.0F, 1.0e-7F));
    SCENE_CHECK(near(scene::halton(2U, 3U), 2.0F / 3.0F, 1.0e-7F));
    SCENE_CHECK(near(scene::halton(3U, 3U), 1.0F / 9.0F, 1.0e-7F));
    // Index zero is the sequence's degenerate sample, and a base below two
    // would otherwise divide forever.
    SCENE_CHECK(scene::halton(0U, 2U) == 0.0F);
    SCENE_CHECK(scene::halton(5U, 1U) == 0.0F);
    SCENE_CHECK(scene::halton(5U, 0U) == 0.0F);

    // The sequence starts at Halton's first sample, not its degenerate zero:
    // base two's is a half, which lands exactly on the pixel centre in x, and
    // base three's is a third, which does not. No sample is the exact centre,
    // and a project with temporal work off applies no jitter at all rather
    // than leaning on one that happens to be.
    SCENE_CHECK(scene::jitter_for_frame(0U, 8U).x == 0.0F);
    SCENE_CHECK(near(scene::jitter_for_frame(0U, 8U).y,
                     (1.0F / 3.0F) - 0.5F, 1.0F / 512.0F));

    // Every offset lands exactly on a 1/256 grid, and this is the property the
    // renderer depends on rather than the Halton value itself.
    //
    // The offset is added to a pixel index before being divided by the canvas
    // extent. A third does not survive that addition: `x + 1/3` rounds one way
    // for one node's pixel index and another way for its neighbour's, the two
    // disagree by a fraction of a pixel about where the sample sits, and a
    // temporal history integrates the disagreement rather than averaging it
    // out. Measured before the snapping went in: three pixels of a 128x96
    // canvas differing on the very first frame, fifty-five by the eighth. On
    // this grid the addition is exact for any canvas a wall will have.
    //
    // Held to the exact value, not a tolerance, because a tolerance is
    // precisely what this cannot afford.
    for (core::u64 frame = 0U; frame < 64U; ++frame) {
        const scene::SubpixelJitter jitter =
            scene::jitter_for_frame(frame, 8U);
        SCENE_CHECK(jitter.x * 256.0F
                    == static_cast<float>(static_cast<core::i32>(
                           jitter.x * 256.0F)));
        SCENE_CHECK(jitter.y * 256.0F
                    == static_cast<float>(static_cast<core::i32>(
                           jitter.y * 256.0F)));
    }

    // Inside the pixel, always. The apron step 05 reserves for temporal work
    // is sized on the assumption that the offset never leaves it.
    for (core::u64 frame = 0U; frame < 64U; ++frame) {
        const scene::SubpixelJitter jitter =
            scene::jitter_for_frame(frame, 8U);
        SCENE_CHECK(jitter.x >= -0.5F && jitter.x < 0.5F);
        SCENE_CHECK(jitter.y >= -0.5F && jitter.y < 0.5F);
    }

    // The sequence repeats with its authored period, and two lengths are
    // genuinely different sequences rather than the same one resampled.
    for (core::u64 frame = 0U; frame < 32U; ++frame) {
        SCENE_CHECK(scene::jitter_for_frame(frame, 8U)
                    == scene::jitter_for_frame(frame + 8U, 8U));
        SCENE_CHECK(scene::jitter_for_frame(frame, 16U)
                    == scene::jitter_for_frame(frame + 16U, 16U));
    }
    SCENE_CHECK(!(scene::jitter_for_frame(9U, 8U)
                  == scene::jitter_for_frame(9U, 16U)));

    // A repeated show frame is a hold, and a hold reuses the offset. This
    // holds by construction rather than by a branch - the offset is a function
    // of the frame number, so asking twice cannot answer differently.
    SCENE_CHECK(scene::jitter_for_frame(1234U, 8U)
                == scene::jitter_for_frame(1234U, 8U));

    // A zero length is clamped rather than dividing by zero.
    SCENE_CHECK(scene::jitter_for_frame(7U, 0U)
                == scene::jitter_for_frame(0U, 1U));

    // Far into a run no show will reach, the sequence still only depends on
    // the frame modulo its length.
    constexpr core::u64 huge = 1'000'000'000'000ULL;
    SCENE_CHECK(scene::jitter_for_frame(huge, 8U)
                == scene::jitter_for_frame(huge % 8U, 8U));
}

// A jittered slice lands where the jittered whole canvas would have put it.
//
// The reason the offset is expressed in canvas pixels: a node holding one
// slice has to sample the same sub-pixel position the full-canvas render
// would, or two nodes accumulate different images and the seam between them
// moves every frame.
void a_jittered_slice_agrees_with_the_jittered_canvas() {
    scene::Camera camera{};
    camera.eye = scene::Vec3{0.0F, 0.0F, 4.0F};
    camera.target = scene::Vec3{};
    camera.up = scene::Vec3{0.0F, 1.0F, 0.0F};
    camera.field_of_view = 0.125F;
    camera.near_plane = 0.1F;
    camera.far_plane = 100.0F;
    const scene::Canvas canvas{1920U, 1080U};
    const scene::SubpixelJitter jitter = scene::jitter_for_frame(3U, 8U);

    const scene::FrustumWindow whole =
        scene::window_for_slice(camera, canvas, scene::whole(canvas));
    const scene::FrustumWindow whole_jittered =
        scene::window_for_slice(camera, canvas, scene::whole(canvas), jitter);

    // The window shifted rather than reshaped: both edges of an axis moved by
    // the same amount, so the span is untouched.
    //
    // Tolerances are sized for binary32 cancellation, not for the property:
    // both windows sit around a tenth, so differencing them loses about eight
    // decimal places before the quantity under test - a pixel offset of a few
    // times ten to the minus five - even enters it.
    SCENE_CHECK(near(whole_jittered.right - whole_jittered.left,
                     whole.right - whole.left, 1.0e-7F));
    SCENE_CHECK(near(whole_jittered.top - whole_jittered.bottom,
                     whole.top - whole.bottom, 1.0e-7F));
    // And by one canvas pixel scaled by the offset - the check that the units
    // are pixels rather than spans or projection units.
    const float pixel_x = (whole.right - whole.left)
        / static_cast<float>(canvas.width);
    SCENE_CHECK(near(whole_jittered.left - whole.left,
                     jitter.x * pixel_x, 1.0e-7F));

    // Two adjacent slices still share a bit-identical boundary under jitter.
    // This is the seam property step 05 depends on, and adding a per-axis
    // offset to both edges is what preserves it.
    const scene::CanvasRect left_half{0U, 0U, 960U, 1080U};
    const scene::CanvasRect right_half{960U, 0U, 960U, 1080U};
    const scene::FrustumWindow a =
        scene::window_for_slice(camera, canvas, left_half, jitter);
    const scene::FrustumWindow b =
        scene::window_for_slice(camera, canvas, right_half, jitter);
    SCENE_CHECK(a.right == b.left);

    // A zero offset reproduces the unjittered window exactly, so a project
    // with temporal work switched off is bit for bit what it always was.
    SCENE_CHECK(scene::window_for_slice(camera, canvas, left_half,
                                        scene::SubpixelJitter{})
                == scene::window_for_slice(camera, canvas, left_half));

    // The matrix form agrees with the window form.
    //
    // Two paths reach a jittered projection - a canvas slice rebuilds its
    // window, and an output that already resolved an asymmetric view has only
    // a matrix left - and they have to move the frustum the same way and in
    // the same direction. A sign error here is invisible in a still frame and
    // shows up as temporal antialiasing that sharpens nothing, because every
    // sample lands on the same side of the pixel it should be straddling.
    const scene::ViewMatrices from_window =
        scene::view_for_slice(camera, canvas, scene::whole(canvas), jitter);
    const scene::ViewMatrices from_matrix = scene::jittered(
        scene::view_for_slice(camera, canvas, scene::whole(canvas)), jitter,
        canvas.width, canvas.height);
    for (core::u32 index = 0U; index < 16U; ++index) {
        SCENE_CHECK(near(from_matrix.projection.m[index],
                         from_window.projection.m[index], 1.0e-6F));
    }

    // A world point lands on the same canvas pixel through either, which is
    // the property that actually matters: the two forms are one projection.
    const scene::Vec3 probe{0.4F, -0.3F, 0.0F};
    const scene::CanvasProjection window_hit =
        scene::project_to_canvas(from_window, canvas, probe);
    const scene::CanvasProjection matrix_hit =
        scene::project_to_canvas(from_matrix, canvas, probe);
    SCENE_CHECK(near(window_hit.x, matrix_hit.x, 1.0e-3F));
    SCENE_CHECK(near(window_hit.y, matrix_hit.y, 1.0e-3F));

    // And it is genuinely offset from the unjittered projection, by the
    // sub-pixel amount asked for rather than by nothing or by a whole pixel.
    //
    // The image slides opposite the frustum on x: moving both vertical edges
    // right means the camera looks further right, so a fixed world point lands
    // further left. On y the two negations cancel - the frustum moves up and
    // screen y grows downward - so that axis keeps its sign. Asserting the
    // directions rather than just the magnitudes is what would catch a jitter
    // that swapped its axes, which no still frame would show.
    const scene::CanvasProjection plain_hit = scene::project_to_canvas(
        scene::view_for_slice(camera, canvas, scene::whole(canvas)), canvas,
        probe);
    SCENE_CHECK(near(window_hit.x - plain_hit.x, -jitter.x, 1.0e-2F));
    SCENE_CHECK(near(window_hit.y - plain_hit.y, jitter.y, 1.0e-2F));

    // Zero jitter leaves a resolved viewpoint untouched, and a zero extent is
    // refused rather than dividing.
    SCENE_CHECK(scene::jittered(from_window, scene::SubpixelJitter{},
                                canvas.width, canvas.height)
                == from_window);
    SCENE_CHECK(scene::jittered(from_window, jitter, 0U, canvas.height)
                == from_window);
}

constexpr std::array<Case, 15U> cases{{
    {"a general inverse round trips a projection",
     &a_general_inverse_round_trips_a_projection},
    {"the jitter sequence is a function of the show frame",
     &the_jitter_sequence_is_a_function_of_the_show_frame},
    {"a jittered slice agrees with the jittered canvas",
     &a_jittered_slice_agrees_with_the_jittered_canvas},
    {"the deterministic trig agrees with the library it replaces",
     &the_deterministic_trig_agrees_with_the_library_it_replaces},
    {"the quadrant boundaries are exact where they should be",
     &the_quadrant_boundaries_are_exact_where_they_should_be},
    {"an arctangent inverts the tangent it is paired with",
     &an_arctangent_inverts_the_tangent_it_is_paired_with},
    {"a field of view outside the representable range reports zero",
     &a_field_of_view_outside_the_representable_range_reports_zero},
    {"exposure stops are deterministic powers of two",
     &exposure_stops_are_deterministic_powers_of_two},
    {"wrapping keeps precision over a run no show will reach",
     &wrapping_keeps_precision_over_a_run_no_show_will_reach},
    {"matrix products behave like the transforms they encode",
     &matrix_products_behave_like_the_transforms_they_encode},
    {"an affine inverse round trips a point",
     &an_affine_inverse_round_trips_a_point},
    {"a normal stays perpendicular under non-uniform scale",
     &a_normal_stays_perpendicular_under_non_uniform_scale},
    {"rotating about an axis preserves length and the axis",
     &rotating_about_an_axis_preserves_length_and_the_axis},
    {"a view matrix puts the eye at the origin looking down minus z",
     &a_view_matrix_puts_the_eye_at_the_origin_looking_down_minus_z},
    {"the arithmetic matches its recorded digest",
     &the_arithmetic_matches_its_recorded_digest},
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
