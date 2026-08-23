#include <mediaserver/scene/math.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <iostream>
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
constexpr core::u64 golden_arithmetic_digest = 0x293d074212d89394ULL;

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

constexpr std::array<Case, 10U> cases{{
    {"the deterministic trig agrees with the library it replaces",
     &the_deterministic_trig_agrees_with_the_library_it_replaces},
    {"the quadrant boundaries are exact where they should be",
     &the_quadrant_boundaries_are_exact_where_they_should_be},
    {"a field of view outside the representable range reports zero",
     &a_field_of_view_outside_the_representable_range_reports_zero},
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
