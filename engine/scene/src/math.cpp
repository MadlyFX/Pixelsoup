#include <mediaserver/scene/math.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace mediaserver::scene {
namespace {

// Two pi, correctly rounded to binary64. Written as a literal rather than
// derived from a library constant so the value is visible and fixed.
constexpr double two_pi = 6.283185307179586476925286766559;

// Taylor coefficients rather than a fitted minimax polynomial. Over the
// reduced range |r| <= pi/4 the first omitted term is around 1e-12, which is
// four orders of magnitude below binary32 precision - so the float result is
// the correctly rounded one for every input that matters, and the coefficients
// stay values a reader can verify by inspection.
constexpr double sin_c3 = -1.0 / 6.0;
constexpr double sin_c5 = 1.0 / 120.0;
constexpr double sin_c7 = -1.0 / 5040.0;
constexpr double sin_c9 = 1.0 / 362880.0;
constexpr double sin_c11 = -1.0 / 39916800.0;

constexpr double cos_c2 = -1.0 / 2.0;
constexpr double cos_c4 = 1.0 / 24.0;
constexpr double cos_c6 = -1.0 / 720.0;
constexpr double cos_c8 = 1.0 / 40320.0;
constexpr double cos_c10 = -1.0 / 3628800.0;
constexpr double cos_c12 = 1.0 / 479001600.0;

[[nodiscard]] double poly_sin(double r, double r2) noexcept {
    return r
        * (1.0
           + r2
               * (sin_c3
                  + r2
                      * (sin_c5
                         + r2 * (sin_c7 + r2 * (sin_c9 + r2 * sin_c11)))));
}

[[nodiscard]] double poly_cos(double r2) noexcept {
    return 1.0
        + r2
            * (cos_c2
               + r2
                   * (cos_c4
                      + r2
                          * (cos_c6
                             + r2 * (cos_c8 + r2 * (cos_c10 + r2 * cos_c12)))));
}

// Quadrant reduction in turns rather than radians.
//
// Turns are the right unit to reduce in because the reduction is then exact:
// the quadrant is an integer, a quarter of it is a power-of-two fraction, and
// the subtraction that follows is a correctly rounded binary64 operation.
// Reducing in radians would need a multi-word pi and would still approximate.
//
// Large angles lose precision here exactly as they would anywhere, but they
// lose it identically on every machine, which is the property being bought.
struct Reduced final {
    double remainder{}; // radians, in [-pi/4, pi/4]
    core::i32 quadrant{};
};

[[nodiscard]] Reduced reduce_turns(float turns) noexcept {
    const auto value = static_cast<double>(turns);
    const auto scaled = value * 4.0;
    // nearbyint under the default rounding mode is round-to-nearest-even and
    // is exact for every representable input, so this is a deterministic step
    // rather than a library call whose accuracy would need pinning down.
    const auto quadrant = std::nearbyint(scaled);
    const auto fraction = value - quadrant * 0.25;
    const auto index = static_cast<core::i64>(quadrant) & core::i64{3};
    return Reduced{fraction * two_pi, static_cast<core::i32>(index)};
}

} // namespace

namespace deterministic {

double shortest_turn(double turns) noexcept {
    return turns - std::floor(turns + 0.5);
}

LinearMotionFit fit_linear_motion(
    std::span<const LinearMotionSample> samples) noexcept {
    if (samples.empty()) return {};
    double time_sum = 0.0;
    double value_sum = 0.0;
    // Subtract the newest value before reducing, to preserve small motions
    // far from the tracker origin and avoid cancellation in the covariance.
    const double origin = samples.back().value;
    for (const auto& sample : samples) {
        time_sum += sample.time;
        value_sum += sample.value - origin;
    }
    const double count = static_cast<double>(samples.size());
    const double time_mean = time_sum / count;
    const double value_mean = value_sum / count;
    double variance = 0.0;
    double covariance = 0.0;
    for (const auto& sample : samples) {
        const double dt = sample.time - time_mean;
        variance += dt * dt;
        covariance += dt * ((sample.value - origin) - value_mean);
    }
    const double velocity = variance > 0.0 ? covariance / variance : 0.0;
    return {origin + (value_mean - velocity * time_mean), velocity};
}

float sin_turns(Turns turns) noexcept {
    const auto reduced = reduce_turns(turns);
    const auto r2 = reduced.remainder * reduced.remainder;
    switch (reduced.quadrant) {
    case 1:
        return static_cast<float>(poly_cos(r2));
    case 2:
        return static_cast<float>(-poly_sin(reduced.remainder, r2));
    case 3:
        return static_cast<float>(-poly_cos(r2));
    default:
        break;
    }
    return static_cast<float>(poly_sin(reduced.remainder, r2));
}

float cos_turns(Turns turns) noexcept {
    const auto reduced = reduce_turns(turns);
    const auto r2 = reduced.remainder * reduced.remainder;
    switch (reduced.quadrant) {
    case 1:
        return static_cast<float>(-poly_sin(reduced.remainder, r2));
    case 2:
        return static_cast<float>(-poly_cos(r2));
    case 3:
        return static_cast<float>(poly_sin(reduced.remainder, r2));
    default:
        break;
    }
    return static_cast<float>(poly_cos(r2));
}

float tan_half_turns(Turns turns) noexcept {
    const auto half = static_cast<double>(turns) * 0.5;
    // A half-angle at or past a quarter turn is a field of view of 180 degrees
    // or more: the tangent is infinite or negative and the projection built
    // from it would be nonsense. Reporting zero lets the caller reject the
    // camera during validation instead of propagating an infinity into a
    // matrix and discovering it as a blank output.
    if (!(half > -0.25) || !(half < 0.25)) {
        return 0.0F;
    }
    const auto reduced = reduce_turns(static_cast<float>(half));
    const auto r2 = reduced.remainder * reduced.remainder;
    auto sine = poly_sin(reduced.remainder, r2);
    auto cosine = poly_cos(r2);
    // The quadrant, which this deliberately did not apply until now.
    //
    // A half-angle past an eighth of a turn - a field of view at or past 90
    // degrees - reduces into quadrant 1 or 3, and reading the polynomials
    // straight off produced a *negative* tangent there. `valid()` rejects a
    // non-positive tangent, so every camera that was ever accepted had a
    // quadrant of zero and is bit-for-bit unchanged by this. What changes is
    // that a genuinely wide lens is now usable instead of being refused.
    switch (reduced.quadrant) {
    case 1: {
        const auto swapped = sine;
        sine = cosine;
        cosine = -swapped;
        break;
    }
    case 3: {
        const auto swapped = sine;
        sine = -cosine;
        cosine = swapped;
        break;
    }
    default:
        break;
    }
    return static_cast<float>(sine / cosine);
}

Turns atan_turns(float value) noexcept {
    if (!std::isfinite(value)) {
        return 0.0F;
    }
    const auto magnitude = std::abs(static_cast<double>(value));
    // atan(x) = pi/2 - atan(1/x) for x > 1, which brings every input into
    // [0, 1] before the second reduction below.
    const bool reciprocal = magnitude > 1.0;
    double reduced = reciprocal ? 1.0 / magnitude : magnitude;
    // atan(a) = pi/4 + atan((a-1)/(a+1)) shrinks [0, 1] to [-t, t] where
    // t = tan(pi/8). The series converges far too slowly near 1 to be used
    // over the whole interval, and this is the standard way to avoid it.
    constexpr double tan_eighth = 0.41421356237309504880168872420970;
    constexpr double quarter_pi = 0.78539816339744830961566084581988;
    constexpr double half_pi = 1.5707963267948966192313216916398;
    double offset = 0.0;
    if (reduced > tan_eighth) {
        offset = quarter_pi;
        reduced = (reduced - 1.0) / (reduced + 1.0);
    }

    // The odd Taylor series, in fixed order and evaluated in binary64. Over
    // the reduced interval the first omitted term is below 1e-11, four orders
    // of magnitude under binary32 precision, so the float result is the
    // correctly rounded one for every input that matters.
    const double r2 = reduced * reduced;
    double series = 1.0 / 23.0;
    series = -1.0 / 21.0 + r2 * series;
    series = 1.0 / 19.0 + r2 * series;
    series = -1.0 / 17.0 + r2 * series;
    series = 1.0 / 15.0 + r2 * series;
    series = -1.0 / 13.0 + r2 * series;
    series = 1.0 / 11.0 + r2 * series;
    series = -1.0 / 9.0 + r2 * series;
    series = 1.0 / 7.0 + r2 * series;
    series = -1.0 / 5.0 + r2 * series;
    series = 1.0 / 3.0 + r2 * series;
    series = -1.0 + r2 * series;
    // The Horner chain above accumulates the alternating signs of the series
    // with the leading term inverted, so the result is negated once here.
    double radians = offset - reduced * series;
    if (reciprocal) {
        radians = half_pi - radians;
    }
    constexpr double two_pi_reciprocal =
        0.15915494309189533576888376337251;
    const auto turns = static_cast<float>(radians * two_pi_reciprocal);
    return value < 0.0F ? -turns : turns;
}

float stop_multiplier(float stops) noexcept {
    if (!std::isfinite(stops)) {
        return 1.0F;
    }
    const double value = std::clamp(
        static_cast<double>(stops), -24.0, 24.0);
    const double integral = std::floor(value);
    const double fraction = value - integral;

    // 2^fraction == exp(ln(2) * fraction). Over [0, 1), the degree-12
    // Taylor remainder is below 3e-12, far beneath a binary32 ulp. Fixed
    // Horner order and a literal ln(2) keep the result independent of libm.
    constexpr double ln_two =
        0.693147180559945309417232121458176568;
    const double y = fraction * ln_two;
    double polynomial = 1.0 / 479001600.0; // 1 / 12!
    polynomial = 1.0 / 39916800.0 + y * polynomial;
    polynomial = 1.0 / 3628800.0 + y * polynomial;
    polynomial = 1.0 / 362880.0 + y * polynomial;
    polynomial = 1.0 / 40320.0 + y * polynomial;
    polynomial = 1.0 / 5040.0 + y * polynomial;
    polynomial = 1.0 / 720.0 + y * polynomial;
    polynomial = 1.0 / 120.0 + y * polynomial;
    polynomial = 1.0 / 24.0 + y * polynomial;
    polynomial = 1.0 / 6.0 + y * polynomial;
    polynomial = 0.5 + y * polynomial;
    polynomial = 1.0 + y * polynomial;
    polynomial = 1.0 + y * polynomial;

    const auto exponent = static_cast<core::i32>(integral);
    const auto biased = static_cast<core::u64>(exponent + 1'023);
    const double scale = std::bit_cast<double>(biased << 52U);
    return static_cast<float>(scale * polynomial);
}

} // namespace deterministic

float length(Vec3 v) noexcept {
    return std::sqrt(dot(v, v));
}

Vec3 normalize(Vec3 v, Vec3 fallback) noexcept {
    const auto magnitude = length(v);
    // Not a tolerance on "close to zero" but on "the division still carries a
    // direction". Below this the components are noise.
    if (!(magnitude > 1.0e-20F)) {
        return fallback;
    }
    return Vec3{v.x / magnitude, v.y / magnitude, v.z / magnitude};
}

Mat4 identity() noexcept {
    Mat4 result{};
    result.m[0U] = 1.0F;
    result.m[5U] = 1.0F;
    result.m[10U] = 1.0F;
    result.m[15U] = 1.0F;
    return result;
}

Mat4 multiply(const Mat4& a, const Mat4& b) noexcept {
    Mat4 result{};
    for (core::u32 column = 0U; column < 4U; ++column) {
        for (core::u32 row = 0U; row < 4U; ++row) {
            // The four products are summed left to right and written out as
            // such. The sum order is part of the result, and an accumulator
            // loop would read like a reduction a reader could assume is free
            // to reassociate.
            const auto sum = a.at(row, 0U) * b.at(0U, column)
                + a.at(row, 1U) * b.at(1U, column)
                + a.at(row, 2U) * b.at(2U, column)
                + a.at(row, 3U) * b.at(3U, column);
            result.set(row, column, sum);
        }
    }
    return result;
}

Vec4 transform(const Mat4& m, Vec4 v) noexcept {
    return Vec4{
        m.at(0U, 0U) * v.x + m.at(0U, 1U) * v.y + m.at(0U, 2U) * v.z
            + m.at(0U, 3U) * v.w,
        m.at(1U, 0U) * v.x + m.at(1U, 1U) * v.y + m.at(1U, 2U) * v.z
            + m.at(1U, 3U) * v.w,
        m.at(2U, 0U) * v.x + m.at(2U, 1U) * v.y + m.at(2U, 2U) * v.z
            + m.at(2U, 3U) * v.w,
        m.at(3U, 0U) * v.x + m.at(3U, 1U) * v.y + m.at(3U, 2U) * v.z
            + m.at(3U, 3U) * v.w,
    };
}

Vec3 transform_point(const Mat4& m, Vec3 point) noexcept {
    const auto result = transform(m, Vec4{point.x, point.y, point.z, 1.0F});
    return Vec3{result.x, result.y, result.z};
}

Mat4 translation(Vec3 offset) noexcept {
    auto result = identity();
    result.set(0U, 3U, offset.x);
    result.set(1U, 3U, offset.y);
    result.set(2U, 3U, offset.z);
    return result;
}

Mat4 scaling(Vec3 factors) noexcept {
    Mat4 result{};
    result.set(0U, 0U, factors.x);
    result.set(1U, 1U, factors.y);
    result.set(2U, 2U, factors.z);
    result.set(3U, 3U, 1.0F);
    return result;
}

Mat4 rotation(Turns yaw, Turns pitch, Turns roll) noexcept {
    const auto sin_yaw = deterministic::sin_turns(yaw);
    const auto cos_yaw = deterministic::cos_turns(yaw);
    const auto sin_pitch = deterministic::sin_turns(pitch);
    const auto cos_pitch = deterministic::cos_turns(pitch);
    // Roll is about the view direction, which is -Z here, so a positive roll
    // turns the horizon the way an operator expects it to.
    const auto sin_roll = deterministic::sin_turns(-roll);
    const auto cos_roll = deterministic::cos_turns(-roll);

    auto yaw_matrix = identity();
    yaw_matrix.set(0U, 0U, cos_yaw);
    yaw_matrix.set(0U, 2U, sin_yaw);
    yaw_matrix.set(2U, 0U, -sin_yaw);
    yaw_matrix.set(2U, 2U, cos_yaw);

    auto pitch_matrix = identity();
    pitch_matrix.set(1U, 1U, cos_pitch);
    pitch_matrix.set(1U, 2U, -sin_pitch);
    pitch_matrix.set(2U, 1U, sin_pitch);
    pitch_matrix.set(2U, 2U, cos_pitch);

    auto roll_matrix = identity();
    roll_matrix.set(0U, 0U, cos_roll);
    roll_matrix.set(0U, 1U, -sin_roll);
    roll_matrix.set(1U, 0U, sin_roll);
    roll_matrix.set(1U, 1U, cos_roll);

    return multiply(yaw_matrix, multiply(pitch_matrix, roll_matrix));
}

Vec3 rotate_around(Vec3 v, Vec3 axis, Turns turns) noexcept {
    const auto unit = normalize(axis, Vec3{0.0F, 1.0F, 0.0F});
    const auto sine = deterministic::sin_turns(turns);
    const auto cosine = deterministic::cos_turns(turns);
    const auto perpendicular = cross(unit, v);
    const auto along = dot(unit, v) * (1.0F - cosine);
    return v * cosine + perpendicular * sine + unit * along;
}

Turns wrap_turns(double turns) noexcept {
    if (!std::isfinite(turns)) {
        return 0.0F;
    }
    // floor is exact for every finite double, so the subtraction that follows
    // is the only rounding step and it is correctly rounded.
    const auto fraction = turns - std::floor(turns);
    return static_cast<float>(fraction);
}

Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up) noexcept {
    const auto forward = normalize(target - eye);
    const auto right = normalize(cross(forward, up), Vec3{1.0F, 0.0F, 0.0F});
    // An up vector parallel to the view direction leaves the cross product at
    // zero and the fallback above pointing somewhere arbitrary. Re-deriving up
    // from whichever right was produced keeps the basis orthonormal either
    // way, so a badly authored camera tilts rather than degenerating.
    const auto true_up = cross(right, forward);

    auto result = identity();
    result.set(0U, 0U, right.x);
    result.set(0U, 1U, right.y);
    result.set(0U, 2U, right.z);
    result.set(1U, 0U, true_up.x);
    result.set(1U, 1U, true_up.y);
    result.set(1U, 2U, true_up.z);
    result.set(2U, 0U, -forward.x);
    result.set(2U, 1U, -forward.y);
    result.set(2U, 2U, -forward.z);
    result.set(0U, 3U, -dot(right, eye));
    result.set(1U, 3U, -dot(true_up, eye));
    result.set(2U, 3U, dot(forward, eye));
    return result;
}

namespace {

// Inverse of the upper 3x3 by adjugate over determinant, written into the
// upper 3x3 of an otherwise untouched matrix. False for a singular basis,
// which from the document's point of view is a zero scale.
[[nodiscard]] bool invert_basis(const Mat4& m, Mat4& into) noexcept {
    const auto a = m.at(0U, 0U);
    const auto b = m.at(0U, 1U);
    const auto c = m.at(0U, 2U);
    const auto d = m.at(1U, 0U);
    const auto e = m.at(1U, 1U);
    const auto f = m.at(1U, 2U);
    const auto g = m.at(2U, 0U);
    const auto h = m.at(2U, 1U);
    const auto i = m.at(2U, 2U);

    const auto cofactor_a = e * i - f * h;
    const auto cofactor_b = f * g - d * i;
    const auto cofactor_c = d * h - e * g;

    const auto determinant = a * cofactor_a + b * cofactor_b + c * cofactor_c;
    if (determinant == 0.0F) {
        return false;
    }
    const auto inverse_determinant = 1.0F / determinant;

    into.set(0U, 0U, cofactor_a * inverse_determinant);
    into.set(0U, 1U, (c * h - b * i) * inverse_determinant);
    into.set(0U, 2U, (b * f - c * e) * inverse_determinant);
    into.set(1U, 0U, cofactor_b * inverse_determinant);
    into.set(1U, 1U, (a * i - c * g) * inverse_determinant);
    into.set(1U, 2U, (c * d - a * f) * inverse_determinant);
    into.set(2U, 0U, cofactor_c * inverse_determinant);
    into.set(2U, 1U, (b * g - a * h) * inverse_determinant);
    into.set(2U, 2U, (a * e - b * d) * inverse_determinant);
    return true;
}

} // namespace

Mat4 invert(const Mat4& m) noexcept {
    // Column-major storage, so index (row, column) is m[column * 4 + row].
    // The expansion below is written against a flat column-major array to keep
    // the operation order explicit and identical everywhere.
    const std::array<float, 16U>& a = m.m;
    std::array<float, 16U> inverse{};

    inverse[0U] = a[5U] * a[10U] * a[15U] - a[5U] * a[11U] * a[14U]
        - a[9U] * a[6U] * a[15U] + a[9U] * a[7U] * a[14U]
        + a[13U] * a[6U] * a[11U] - a[13U] * a[7U] * a[10U];
    inverse[4U] = -a[4U] * a[10U] * a[15U] + a[4U] * a[11U] * a[14U]
        + a[8U] * a[6U] * a[15U] - a[8U] * a[7U] * a[14U]
        - a[12U] * a[6U] * a[11U] + a[12U] * a[7U] * a[10U];
    inverse[8U] = a[4U] * a[9U] * a[15U] - a[4U] * a[11U] * a[13U]
        - a[8U] * a[5U] * a[15U] + a[8U] * a[7U] * a[13U]
        + a[12U] * a[5U] * a[11U] - a[12U] * a[7U] * a[9U];
    inverse[12U] = -a[4U] * a[9U] * a[14U] + a[4U] * a[10U] * a[13U]
        + a[8U] * a[5U] * a[14U] - a[8U] * a[6U] * a[13U]
        - a[12U] * a[5U] * a[10U] + a[12U] * a[6U] * a[9U];
    inverse[1U] = -a[1U] * a[10U] * a[15U] + a[1U] * a[11U] * a[14U]
        + a[9U] * a[2U] * a[15U] - a[9U] * a[3U] * a[14U]
        - a[13U] * a[2U] * a[11U] + a[13U] * a[3U] * a[10U];
    inverse[5U] = a[0U] * a[10U] * a[15U] - a[0U] * a[11U] * a[14U]
        - a[8U] * a[2U] * a[15U] + a[8U] * a[3U] * a[14U]
        + a[12U] * a[2U] * a[11U] - a[12U] * a[3U] * a[10U];
    inverse[9U] = -a[0U] * a[9U] * a[15U] + a[0U] * a[11U] * a[13U]
        + a[8U] * a[1U] * a[15U] - a[8U] * a[3U] * a[13U]
        - a[12U] * a[1U] * a[11U] + a[12U] * a[3U] * a[9U];
    inverse[13U] = a[0U] * a[9U] * a[14U] - a[0U] * a[10U] * a[13U]
        - a[8U] * a[1U] * a[14U] + a[8U] * a[2U] * a[13U]
        + a[12U] * a[1U] * a[10U] - a[12U] * a[2U] * a[9U];
    inverse[2U] = a[1U] * a[6U] * a[15U] - a[1U] * a[7U] * a[14U]
        - a[5U] * a[2U] * a[15U] + a[5U] * a[3U] * a[14U]
        + a[13U] * a[2U] * a[7U] - a[13U] * a[3U] * a[6U];
    inverse[6U] = -a[0U] * a[6U] * a[15U] + a[0U] * a[7U] * a[14U]
        + a[4U] * a[2U] * a[15U] - a[4U] * a[3U] * a[14U]
        - a[12U] * a[2U] * a[7U] + a[12U] * a[3U] * a[6U];
    inverse[10U] = a[0U] * a[5U] * a[15U] - a[0U] * a[7U] * a[13U]
        - a[4U] * a[1U] * a[15U] + a[4U] * a[3U] * a[13U]
        + a[12U] * a[1U] * a[7U] - a[12U] * a[3U] * a[5U];
    inverse[14U] = -a[0U] * a[5U] * a[14U] + a[0U] * a[6U] * a[13U]
        + a[4U] * a[1U] * a[14U] - a[4U] * a[2U] * a[13U]
        - a[12U] * a[1U] * a[6U] + a[12U] * a[2U] * a[5U];
    inverse[3U] = -a[1U] * a[6U] * a[11U] + a[1U] * a[7U] * a[10U]
        + a[5U] * a[2U] * a[11U] - a[5U] * a[3U] * a[10U]
        - a[9U] * a[2U] * a[7U] + a[9U] * a[3U] * a[6U];
    inverse[7U] = a[0U] * a[6U] * a[11U] - a[0U] * a[7U] * a[10U]
        - a[4U] * a[2U] * a[11U] + a[4U] * a[3U] * a[10U]
        + a[8U] * a[2U] * a[7U] - a[8U] * a[3U] * a[6U];
    inverse[11U] = -a[0U] * a[5U] * a[11U] + a[0U] * a[7U] * a[9U]
        + a[4U] * a[1U] * a[11U] - a[4U] * a[3U] * a[9U]
        - a[8U] * a[1U] * a[7U] + a[8U] * a[3U] * a[5U];
    inverse[15U] = a[0U] * a[5U] * a[10U] - a[0U] * a[6U] * a[9U]
        - a[4U] * a[1U] * a[10U] + a[4U] * a[2U] * a[9U]
        + a[8U] * a[1U] * a[6U] - a[8U] * a[2U] * a[5U];

    const float determinant = a[0U] * inverse[0U] + a[1U] * inverse[4U]
        + a[2U] * inverse[8U] + a[3U] * inverse[12U];
    if (!std::isfinite(determinant) || std::abs(determinant) < 1.0e-20F) {
        return identity();
    }
    const float reciprocal = 1.0F / determinant;
    Mat4 result{};
    for (std::size_t index = 0U; index < 16U; ++index) {
        result.m[index] = inverse[index] * reciprocal;
    }
    for (const float value : result.m) {
        if (!std::isfinite(value)) {
            return identity();
        }
    }
    return result;
}

Mat4 invert_affine(const Mat4& m) noexcept {
    auto result = identity();
    if (!invert_basis(m, result)) {
        return identity();
    }

    const Vec3 offset{m.at(0U, 3U), m.at(1U, 3U), m.at(2U, 3U)};
    result.set(
        0U,
        3U,
        -(result.at(0U, 0U) * offset.x + result.at(0U, 1U) * offset.y
          + result.at(0U, 2U) * offset.z));
    result.set(
        1U,
        3U,
        -(result.at(1U, 0U) * offset.x + result.at(1U, 1U) * offset.y
          + result.at(1U, 2U) * offset.z));
    result.set(
        2U,
        3U,
        -(result.at(2U, 0U) * offset.x + result.at(2U, 1U) * offset.y
          + result.at(2U, 2U) * offset.z));
    return result;
}

Mat4 normal_basis(const Mat4& m) noexcept {
    auto inverse = identity();
    if (!invert_basis(m, inverse)) {
        return identity();
    }
    auto result = identity();
    for (core::u32 row = 0U; row < 3U; ++row) {
        for (core::u32 column = 0U; column < 3U; ++column) {
            result.set(row, column, inverse.at(column, row));
        }
    }
    return result;
}

} // namespace mediaserver::scene
