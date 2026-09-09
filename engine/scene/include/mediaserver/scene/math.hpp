#pragma once

#include <mediaserver/core/types.hpp>

#include <array>
#include <span>

// Scene arithmetic, with bit-exact reproducibility as the load-bearing
// property rather than as a nice-to-have.
//
// Every node in a cohort evaluates the same scene from the same replicated
// document at the same show frame. If two nodes disagree about where an object
// is by one ulp, the seam between their slices shows it: a hard edge across a
// surface that is supposed to be continuous. So the rule for this header is
// that any two IEEE-754 binary32/binary64 machines must produce identical
// bits, and everything below follows from it.
//
// What that costs:
//
//  * No `std::sin`, `std::cos`, or `std::tan`. The C library's transcendentals
//    are not specified to any particular accuracy and genuinely differ between
//    glibc, Apple's libm, and the UCRT. They are replaced here by polynomial
//    evaluations that use only the operations IEEE-754 *does* pin down.
//  * No fused multiply-add. `a * b + c` and `fma(a, b, c)` round differently,
//    and whether a compiler contracts is a target and flag decision. The build
//    turns contraction off for this target; see
//    `mediaserver_set_deterministic_float_options`.
//  * Fixed operation order everywhere. Nothing here is written as a reduction
//    a compiler is free to reassociate, because at `/fp:precise` and
//    `-ffp-contract=off` it is not free to, and the code should read the way
//    it will execute.
//
// `std::sqrt` is kept: IEEE-754 requires it to be correctly rounded, so it is
// exactly as reproducible as addition. Division likewise.
//
// The build flags are belt; the golden digests in the scene tests are braces.
// A toolchain change that breaks reproducibility fails a test rather than
// producing a seam on a stage six months later.
namespace mediaserver::scene {

// Turns of a circle, not radians or degrees.
//
// The document stores rotations as turns because the values operators actually
// type - a quarter turn, a third of a turn - are exact in binary32 as turns and
// are not as radians. Converting to radians happens once, inside the range
// reduction below, where the extra precision of double absorbs it.
using Turns = float;

namespace deterministic {

struct LinearMotionSample final {
    // Small, relative control-frame offsets; never cast an absolute u64 stamp.
    double time{};
    double value{};
};
struct LinearMotionFit final {
    double present{};
    double velocity{};
};
// Least-squares line evaluated at time zero, with fixed binary64 operation
// order. Caller supplies oldest-to-newest samples and a bounded window.
[[nodiscard]] LinearMotionFit fit_linear_motion(
    std::span<const LinearMotionSample> samples) noexcept;
// Shortest signed turn, including the deterministic half-turn tie (-0.5).
[[nodiscard]] double shortest_turn(double turns) noexcept;

// sin/cos of an angle in turns. Exact to well under binary32 precision, and
// identical on every platform this builds for.
[[nodiscard]] float sin_turns(Turns turns) noexcept;
[[nodiscard]] float cos_turns(Turns turns) noexcept;

// tan of a half-angle given in turns. This is the one form the projection
// needs - `tan(fov_y / 2)` - and naming it that way keeps the caller from
// having to halve an angle in a unit where halving is the caller's problem.
//
// Returns 0 when the half-angle is not strictly inside a quarter turn, which
// is the degenerate field of view the caller must reject anyway. Every field
// of view strictly inside half a turn is supported, including the wide ones a
// short lens on a volume produces.
[[nodiscard]] float tan_half_turns(Turns turns) noexcept;

// The inverse: the angle, in turns, whose tangent is `value`. The result is in
// (-0.25, 0.25) turns - plus or minus a quarter turn - as an arctangent must
// be.
//
// This is what turns a lens into a field of view: half the sensor height over
// the focal length is a tangent, and the angle it subtends is the camera's
// view. Written here, deterministically, rather than reached for from the C
// library, for the same reason as everything else in this header: two machines
// that disagree about the angle by one ulp put the edge of the plate in two
// different places. Non-finite input is zero.
[[nodiscard]] Turns atan_turns(float value) noexcept;

// Converts exposure-compensation stops to a multiplier without calling a
// platform libm exponential. Inputs are saturated to +/-24 stops, a wider
// range than an operator or camera profile can use meaningfully and one that
// keeps every result finite and normal in binary32. Non-finite input is the
// identity.
[[nodiscard]] float stop_multiplier(float stops) noexcept;

} // namespace deterministic

struct Vec3 final {
    float x{};
    float y{};
    float z{};

    bool operator==(const Vec3&) const = default;
};

struct Vec4 final {
    float x{};
    float y{};
    float z{};
    float w{};

    bool operator==(const Vec4&) const = default;
};

[[nodiscard]] constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

[[nodiscard]] constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

[[nodiscard]] constexpr Vec3 operator-(Vec3 v) noexcept {
    return Vec3{-v.x, -v.y, -v.z};
}

[[nodiscard]] constexpr Vec3 operator*(Vec3 v, float scalar) noexcept {
    return Vec3{v.x * scalar, v.y * scalar, v.z * scalar};
}

[[nodiscard]] constexpr float dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return Vec3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x,
    };
}

[[nodiscard]] float length(Vec3 v) noexcept;

// Returns `fallback` for a vector too short to normalise rather than producing
// infinities. A zero-length direction is a document authoring mistake, and the
// renderer should draw something wrong-looking rather than nothing at all.
[[nodiscard]] Vec3 normalize(Vec3 v, Vec3 fallback = Vec3{0.0F, 0.0F, -1.0F}) noexcept;

// A 4x4 matrix in column-major storage: element (row, column) lives at
// `m[column * 4 + row]`.
//
// This is the layout GLM uses and the layout HLSL's default `column_major`
// packing expects in a constant buffer, so a matrix written here is uploaded
// with no transpose and multiplied in the shader as `mul(matrix, vector)`.
// Picking the pair that needs no fixups is worth more than picking a
// convention on aesthetic grounds.
struct Mat4 final {
    std::array<float, 16U> m{};

    bool operator==(const Mat4&) const = default;

    [[nodiscard]] constexpr float at(core::u32 row, core::u32 column) const noexcept {
        return m[static_cast<std::size_t>(column) * 4U + row];
    }

    constexpr void set(core::u32 row, core::u32 column, float value) noexcept {
        m[static_cast<std::size_t>(column) * 4U + row] = value;
    }
};

[[nodiscard]] Mat4 identity() noexcept;

// `a` applied after `b`, the usual reading of a matrix product.
[[nodiscard]] Mat4 multiply(const Mat4& a, const Mat4& b) noexcept;

[[nodiscard]] Vec4 transform(const Mat4& m, Vec4 v) noexcept;

// Transforms a position: the vector is extended with w = 1 and the result is
// truncated. No perspective divide - use `transform` when there might be one.
[[nodiscard]] Vec3 transform_point(const Mat4& m, Vec3 point) noexcept;

[[nodiscard]] Mat4 translation(Vec3 offset) noexcept;
[[nodiscard]] Mat4 scaling(Vec3 factors) noexcept;

// Intrinsic rotations applied in a fixed order: yaw about +Y, then pitch about
// +X, then roll about -Z. Fixed because "the order the document says" is one
// more thing that could differ between two nodes, and there is no reason to
// let it.
[[nodiscard]] Mat4 rotation(Turns yaw, Turns pitch, Turns roll) noexcept;

// Rotates `v` about `axis` by `turns`, by Rodrigues' formula with the three
// terms summed in a fixed order. `axis` is normalised here rather than assumed
// to be, because the caller's axis usually comes from a document property.
[[nodiscard]] Vec3 rotate_around(Vec3 v, Vec3 axis, Turns turns) noexcept;

// Reduces an angle to the half-open interval [0, 1) turns.
//
// A show running for ten hours with something turning once a second reaches
// 36,000 turns. Binary32 spends sixteen bits on the whole part of that and has
// eight left for the fraction, so the rotation would visibly step. Reducing in
// binary64 first and returning the fraction keeps full precision for as long
// as the show runs, and does it identically on every node.
[[nodiscard]] Turns wrap_turns(double turns) noexcept;

// A right-handed view matrix looking from `eye` towards `target`.
//
// Right-handed with -Z forward, matching the projection below and the
// convention every DCC package the content will come from already uses.
[[nodiscard]] Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) noexcept;

// The inverse of a matrix built only from rotation, uniform-or-not scale, and
// translation. Cheaper and more accurate than a general inverse, and the only
// case the scene produces.
//
// Returns identity for a matrix with a degenerate basis, which is what a zero
// scale in the document looks like from here.
[[nodiscard]] Mat4 invert_affine(const Mat4& m) noexcept;

// The general inverse, for a matrix `invert_affine` cannot handle - a
// projection, or a view-projection, whose bottom row is not (0,0,0,1).
//
// Cofactor expansion in a fixed operation order, so two nodes inverting the
// same matrix get the same bits. A pivoting solver would be more accurate on a
// badly conditioned matrix and would also let the pivot choice depend on
// values that differ in their last bit between two machines, which is the one
// thing this must not do.
//
// Returns identity for a singular matrix, which is what a degenerate frustum
// looks like from here.
[[nodiscard]] Mat4 invert(const Mat4& m) noexcept;

// The 3x3 basis that transforms normals: the inverse transpose of the upper
// 3x3, stored in a Mat4 with an identity fourth row and column so the upload
// has one shape. Correct under non-uniform scale, which a plain basis is not.
[[nodiscard]] Mat4 normal_basis(const Mat4& m) noexcept;

} // namespace mediaserver::scene
