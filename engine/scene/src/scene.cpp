#include <mediaserver/scene/scene.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mediaserver::scene {

ReflectionProbe clamped(const ReflectionProbe& value) noexcept {
    auto result = value;
    const auto bounded = [](float v, float lo, float hi, float fallback) {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    result.position = {bounded(value.position.x, -10000, 10000, 0), bounded(value.position.y, -10000, 10000, 1), bounded(value.position.z, -10000, 10000, 0)};
    result.extent = {bounded(value.extent.x, 0.01F, 10000, 5), bounded(value.extent.y, 0.01F, 10000, 3), bounded(value.extent.z, 0.01F, 10000, 5)};
    result.blend = bounded(value.blend, 0.001F, 1, 0.25F);
    result.intensity = bounded(value.intensity, 0, 16, 1);
    return result;
}
PlanarReflection clamped(const PlanarReflection& value) noexcept {
    auto result = value;
    const auto bounded = [](float v) { return std::isfinite(v) ? std::clamp(v, -10000.0F, 10000.0F) : 0.0F; };
    result.point = {bounded(value.point.x), bounded(value.point.y), bounded(value.point.z)};
    result.normal = normalize({bounded(value.normal.x), bounded(value.normal.y), bounded(value.normal.z)}, {0, 1, 0});
    result.strength = std::isfinite(value.strength) ? std::clamp(value.strength, 0.0F, 1.0F) : 1.0F;
    return result;
}
Mat4 reflection_matrix(const PlanarReflection& plane) noexcept {
    const auto n = normalize(plane.normal, {0, 1, 0});
    const float a[3] = {n.x, n.y, n.z};
    auto result = identity();
    for (core::u32 row = 0; row < 3; ++row) {
        for (core::u32 column = 0; column < 3; ++column)
            result.set(row, column, (row == column ? 1.0F : 0.0F) - 2 * a[row] * a[column]);
        result.set(row, 3, 2 * dot(n, plane.point) * a[row]);
    }
    return result;
}

BakedLighting clamped(const BakedLighting& value) {
    BakedLighting result = value;
    const BakedLighting defaults{};
    auto coordinate = [](float v, float fallback) {
        return std::isfinite(v) ? std::clamp(v, -10000.0F, 10000.0F) : fallback;
    };
    result.minimum = {coordinate(value.minimum.x, defaults.minimum.x), coordinate(value.minimum.y, defaults.minimum.y), coordinate(value.minimum.z, defaults.minimum.z)};
    result.maximum = {std::max(coordinate(value.maximum.x, defaults.maximum.x), result.minimum.x + 0.01F), std::max(coordinate(value.maximum.y, defaults.maximum.y), result.minimum.y + 0.01F), std::max(coordinate(value.maximum.z, defaults.maximum.z), result.minimum.z + 0.01F)};
    result.count_x = std::clamp(value.count_x, 2U, max_baked_probe_axis);
    result.count_y = std::clamp(value.count_y, 2U, max_baked_probe_axis);
    result.count_z = std::clamp(value.count_z, 2U, max_baked_probe_axis);
    result.samples = std::clamp(value.samples, 64U, 65536U);
    result.bounces = std::clamp(value.bounces, 1U, 16U);
    result.strength = std::isfinite(value.strength) ? std::clamp(value.strength, 0.0F, 16.0F) : 1.0F;
    for (auto& probe : result.reflections) probe = clamped(probe);
    return result;
}

namespace {

// One ramp's contribution: fully out at zero frames elapsed and fully in after
// `length`. Written as one division of two exactly representable integers so
// that every node lands on the same float rather than on the sum of a repeated
// per-frame step.
[[nodiscard]] float ramp(core::i64 elapsed, core::i64 length) noexcept {
    if (length <= 0) {
        return 1.0F;
    }
    if (elapsed <= 0) {
        return 0.0F;
    }
    if (elapsed >= length) {
        return 1.0F;
    }
    return static_cast<float>(static_cast<double>(elapsed) /
                              static_cast<double>(length));
}

constexpr core::u64 fnv_offset_basis = 14'695'981'039'346'656'037ULL;
constexpr core::u64 fnv_prime = 1'099'511'628'211ULL;

struct Point2d final {
    double x{};
    double y{};
};

using Matrix3d = std::array<double, 9U>;

[[nodiscard]] Matrix3d multiply(const Matrix3d& left,
                                const Matrix3d& right) noexcept {
    Matrix3d result{};
    for (std::size_t row = 0U; row < 3U; ++row) {
        for (std::size_t column = 0U; column < 3U; ++column) {
            for (std::size_t inner = 0U; inner < 3U; ++inner) {
                result[row * 3U + column] +=
                    left[row * 3U + inner] *
                    right[inner * 3U + column];
            }
        }
    }
    return result;
}

[[nodiscard]] std::array<double, 3U> multiply(
    const Matrix3d& matrix,
    const std::array<double, 3U>& vector) noexcept {
    std::array<double, 3U> result{};
    for (std::size_t row = 0U; row < 3U; ++row) {
        for (std::size_t column = 0U; column < 3U; ++column) {
            result[row] += matrix[row * 3U + column] * vector[column];
        }
    }
    return result;
}

// CIE 1931 Planckian-locus approximation for 1667..25000 K, evaluated only
// over White Balance's narrower 2000..12000 K canonical range. These fixed
// coefficients are followed by a fixed CIE 1960 UCS tint displacement.
[[nodiscard]] Point2d planckian_xy(double temperature) noexcept {
    const double inverse = 1.0 / temperature;
    const double inverse2 = inverse * inverse;
    const double inverse3 = inverse2 * inverse;
    const double x = temperature <= 4'000.0
        ? -0.2661239e9 * inverse3 - 0.2343580e6 * inverse2
              + 0.8776956e3 * inverse + 0.179910
        : -3.0258469e9 * inverse3 + 2.1070379e6 * inverse2
              + 0.2226347e3 * inverse + 0.240390;
    const double x2 = x * x;
    const double x3 = x2 * x;
    const double y = temperature <= 2'222.0
        ? -1.1063814 * x3 - 1.34811020 * x2 + 2.18555832 * x
              - 0.20219683
        : temperature <= 4'000.0
        ? -0.9549476 * x3 - 1.37418593 * x2 + 2.09137015 * x
              - 0.16748867
        : 3.0817580 * x3 - 5.87338670 * x2 + 3.75112997 * x
              - 0.37001483;
    return Point2d{x, y};
}

[[nodiscard]] Point2d xy_to_cie1960_uv(Point2d xy) noexcept {
    const double denominator = -2.0 * xy.x + 12.0 * xy.y + 3.0;
    return Point2d{4.0 * xy.x / denominator,
                   6.0 * xy.y / denominator};
}

[[nodiscard]] Point2d cie1960_uv_to_xy(Point2d uv) noexcept {
    const double denominator = 2.0 * uv.x - 8.0 * uv.y + 4.0;
    return Point2d{3.0 * uv.x / denominator,
                   2.0 * uv.y / denominator};
}

[[nodiscard]] std::array<float, 9U> white_balance_matrix(
    double temperature, double tint) noexcept {
    // The Planckian approximation does not land exactly on the standardized
    // D65 chromaticity. Translate the whole locus in CIE 1960 UCS so 6500 K
    // maps exactly to D65 (x=0.3127, y=0.3290), then apply tint along v.
    constexpr Point2d d65_xy{0.3127, 0.3290};
    const Point2d d65_uv = xy_to_cie1960_uv(d65_xy);
    const Point2d planck_d65_uv = xy_to_cie1960_uv(planckian_xy(6'500.0));
    Point2d target_uv = xy_to_cie1960_uv(planckian_xy(temperature));
    target_uv.x += d65_uv.x - planck_d65_uv.x;
    target_uv.y += d65_uv.y - planck_d65_uv.y
        - tint * static_cast<double>(white_balance_tint_uv_scale);
    const Point2d target_xy = cie1960_uv_to_xy(target_uv);

    constexpr std::array<double, 3U> d65_xyz{0.95047, 1.0, 1.08883};
    const std::array<double, 3U> target_xyz{
        target_xy.x / target_xy.y,
        1.0,
        (1.0 - target_xy.x - target_xy.y) / target_xy.y};

    // Bradford cone-response matrices, followed by IEC 61966-2-1 linear-sRGB
    // D65 RGB<->XYZ matrices. Multiplication order maps D65-referred RGB to
    // the authored destination white: X2R * B^-1 * D * B * R2X.
    constexpr Matrix3d bradford{
        0.8951, 0.2664, -0.1614,
        -0.7502, 1.7135, 0.0367,
        0.0389, -0.0685, 1.0296};
    constexpr Matrix3d inverse_bradford{
        0.9869929, -0.1470543, 0.1599627,
        0.4323053, 0.5183603, 0.0492912,
        -0.0085287, 0.0400428, 0.9684867};
    constexpr Matrix3d rgb_to_xyz{
        0.4124564, 0.3575761, 0.1804375,
        0.2126729, 0.7151522, 0.0721750,
        0.0193339, 0.1191920, 0.9503041};
    constexpr Matrix3d xyz_to_rgb{
        3.2404542, -1.5371385, -0.4985314,
        -0.9692660, 1.8760108, 0.0415560,
        0.0556434, -0.2040259, 1.0572252};

    const auto source_cones = multiply(bradford, d65_xyz);
    const auto target_cones = multiply(bradford, target_xyz);
    const Matrix3d cone_scale{
        target_cones[0U] / source_cones[0U], 0.0, 0.0,
        0.0, target_cones[1U] / source_cones[1U], 0.0,
        0.0, 0.0, target_cones[2U] / source_cones[2U]};
    const Matrix3d adaptation = multiply(
        inverse_bradford, multiply(cone_scale, bradford));
    const Matrix3d rgb_adaptation = multiply(
        xyz_to_rgb, multiply(adaptation, rgb_to_xyz));
    std::array<float, 9U> result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        result[index] = static_cast<float>(rgb_adaptation[index]);
    }
    return result;
}

[[nodiscard]] bool same_sign(double left, double right) noexcept {
    return (left > 0.0 && right > 0.0) || (left < 0.0 && right < 0.0);
}

[[nodiscard]] bool valid_curve_channel(
    const CurveChannelParameters& channel) noexcept {
    if (channel.points.size() < min_curve_control_points
        || channel.points.size() > max_curve_control_points) {
        return false;
    }
    for (std::size_t index = 0U; index < channel.points.size(); ++index) {
        const CurveControlPoint point = channel.points[index];
        if (!std::isfinite(point.x) || !std::isfinite(point.y)
            || point.x < 0.0F || point.x > 1.0F
            || point.y < 0.0F || point.y > 1.0F
            || (index != 0U
                && point.x <= channel.points[index - 1U].x)) {
            return false;
        }
    }
    return channel.points.front() == CurveControlPoint{0.0F, 0.0F}
        && channel.points.back() == CurveControlPoint{1.0F, 1.0F};
}

// Fritsch-Carlson/PCHIP tangents. The weighted harmonic mean at interior
// points and the shape-preserving one-sided endpoint rule keep every segment
// monotone whenever its two authored ordinates are monotone, without imposing
// monotonicity across an authored local extremum.
[[nodiscard]] std::array<float, curve_lut_entries> sample_curve(
    const CurveChannelParameters& channel) noexcept {
    const std::size_t count = channel.points.size();
    std::array<double, max_curve_control_points> widths{};
    std::array<double, max_curve_control_points> secants{};
    std::array<double, max_curve_control_points> tangents{};
    for (std::size_t index = 0U; index + 1U < count; ++index) {
        widths[index] = static_cast<double>(channel.points[index + 1U].x)
            - static_cast<double>(channel.points[index].x);
        secants[index] =
            (static_cast<double>(channel.points[index + 1U].y)
             - static_cast<double>(channel.points[index].y))
            / widths[index];
    }
    if (count == 2U) {
        tangents[0U] = secants[0U];
        tangents[1U] = secants[0U];
    } else {
        const auto endpoint = [](double h0, double h1, double d0,
                                 double d1) noexcept {
            double tangent = ((2.0 * h0 + h1) * d0 - h0 * d1)
                / (h0 + h1);
            if (!same_sign(tangent, d0)) {
                tangent = 0.0;
            } else if (!same_sign(d0, d1)
                       && std::abs(tangent) > 3.0 * std::abs(d0)) {
                tangent = 3.0 * d0;
            }
            return tangent;
        };
        tangents[0U] = endpoint(
            widths[0U], widths[1U], secants[0U], secants[1U]);
        tangents[count - 1U] = endpoint(
            widths[count - 2U], widths[count - 3U], secants[count - 2U],
            secants[count - 3U]);
        for (std::size_t index = 1U; index + 1U < count; ++index) {
            const double before = secants[index - 1U];
            const double after = secants[index];
            if (!same_sign(before, after)) {
                tangents[index] = 0.0;
                continue;
            }
            const double first_weight = 2.0 * widths[index]
                + widths[index - 1U];
            const double second_weight = widths[index]
                + 2.0 * widths[index - 1U];
            tangents[index] = (first_weight + second_weight)
                / (first_weight / before + second_weight / after);
        }
    }

    std::array<float, curve_lut_entries> result{};
    std::size_t segment{};
    for (core::u32 index = 0U; index < curve_lut_entries; ++index) {
        const double x = static_cast<double>(index)
            / static_cast<double>(curve_lut_entries - 1U);
        while (segment + 2U < count
               && x > static_cast<double>(channel.points[segment + 1U].x)) {
            ++segment;
        }
        const double left_x = channel.points[segment].x;
        const double width = widths[segment];
        const double t = (x - left_x) / width;
        const double t2 = t * t;
        const double t3 = t2 * t;
        const double left_y = channel.points[segment].y;
        const double right_y = channel.points[segment + 1U].y;
        const double y = (2.0 * t3 - 3.0 * t2 + 1.0) * left_y
            + (t3 - 2.0 * t2 + t) * width * tangents[segment]
            + (-2.0 * t3 + 3.0 * t2) * right_y
            + (t3 - t2) * width * tangents[segment + 1U];
        result[index] = static_cast<float>(std::clamp(y, 0.0, 1.0));
    }
    // Preserve the authored endpoint bits exactly rather than relying on the
    // final Hermite arithmetic to reconstruct them.
    result.front() = 0.0F;
    result.back() = 1.0F;
    return result;
}

void mix(core::u64& state, core::u64 value) noexcept {
    for (core::u32 shift = 0U; shift < 64U; shift += 8U) {
        state ^= (value >> shift) & 0xffULL;
        state *= fnv_prime;
    }
}

void mix(core::u64& state, float value) noexcept {
    // The bit pattern, not the value: two floats one ulp apart must not hash
    // equal. std::bit_cast would say this more directly, but memcpy says it in
    // C++20 without pulling in <bit> for one call.
    core::u32 bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    // Negative zero is the one case where distinct bit patterns are the same
    // number, and it arises constantly from negating a zero component. Folding
    // it here keeps the digest from reporting a divergence that no renderer
    // could observe.
    if (bits == 0x8000'0000U) {
        bits = 0U;
    }
    mix(state, static_cast<core::u64>(bits));
}

void mix(core::u64& state, double value) noexcept {
    core::u64 bits{};
    std::memcpy(&bits, &value, sizeof(bits));
    if (bits == 0x8000'0000'0000'0000ULL) {
        bits = 0U;
    }
    mix(state, bits);
}

void mix(core::u64& state, Vec3 value) noexcept {
    mix(state, value.x);
    mix(state, value.y);
    mix(state, value.z);
}

void mix(core::u64& state, const Mat4& value) noexcept {
    for (const auto element : value.m) {
        mix(state, element);
    }
}

void mix(core::u64& state, const std::string& value) noexcept;

void mix(core::u64& state, const Material& value) noexcept {
    mix(state, value.base_color);
    mix(state, value.roughness);
    mix(state, value.metallic);
    mix(state, value.emissive);
    mix(state, value.emissive_strength);
    mix(state, value.normal_scale);
    mix(state, value.occlusion_strength);
    mix(state, static_cast<core::u64>(value.alpha_mode));
    mix(state, value.alpha_cutoff);
    mix(state, static_cast<core::u64>(value.double_sided));
    mix(state, value.base_color_texture);
    mix(state, value.metallic_roughness_texture);
    mix(state, value.normal_texture);
    mix(state, value.occlusion_texture);
    mix(state, value.emissive_texture);
    mix(state, value.base_alpha);
    mix(state, value.clearcoat);
    mix(state, value.clearcoat_roughness);
    mix(state, value.clearcoat_normal_scale);
    mix(state, value.sheen_roughness);
    mix(state, value.transmission);
    mix(state, value.ior);
    mix(state, value.specular);
    mix(state, value.anisotropy_strength);
    mix(state, value.anisotropy_rotation);
    mix(state, value.sheen_color);
    mix(state, value.specular_color);
    mix(state, value.clearcoat_texture);
    mix(state, value.clearcoat_roughness_texture);
    mix(state, value.clearcoat_normal_texture);
    mix(state, value.sheen_color_texture);
    mix(state, value.sheen_roughness_texture);
    mix(state, value.transmission_texture);
    mix(state, value.specular_texture);
    mix(state, value.specular_color_texture);
    mix(state, value.anisotropy_texture);

}

void mix(core::u64& state,
         const LevelsChannelParameters& value) noexcept {
    mix(state, value.input_black);
    mix(state, value.input_white);
    mix(state, value.gamma);
    mix(state, value.output_black);
    mix(state, value.output_white);
}

void mix(core::u64& state, const LevelsParameters& value) noexcept {
    // This declaration order is also the execution order: master, R, G, B.
    mix(state, value.master);
    mix(state, value.red);
    mix(state, value.green);
    mix(state, value.blue);
}

void mix(core::u64& state, const LiftGammaGainParameters& value) noexcept {
    mix(state, value.lift);
    mix(state, value.gamma);
    mix(state, value.gain);
}

void mix(core::u64& state, const ContrastPivotParameters& value) noexcept {
    mix(state, value.contrast);
    mix(state, value.pivot);
}

void mix(core::u64& state, const ColorMatrixParameters& value) noexcept {
    for (const float coefficient : value.matrix) {
        mix(state, coefficient);
    }
    mix(state, value.offset);
}

void mix(core::u64& state, const WhiteBalanceParameters& value) noexcept {
    mix(state, value.temperature_kelvin);
    mix(state, value.tint);
    for (const float coefficient : value.matrix) {
        mix(state, coefficient);
    }
}

void mix(core::u64& state, const CurvesParameters& value) noexcept {
    const auto mix_channel = [&state](
                                 const CurveChannelParameters& channel) noexcept {
        mix(state, static_cast<core::u64>(channel.points.size()));
        for (const CurveControlPoint point : channel.points) {
            mix(state, point.x);
            mix(state, point.y);
        }
    };
    mix_channel(value.master);
    for (const auto* const channel : {&value.red, &value.green, &value.blue}) {
        mix(state, static_cast<core::u64>(channel->has_value()));
        if (channel->has_value()) {
            mix_channel(**channel);
        }
    }
}

void mix(core::u64& state, const FilmGrainParameters& value) noexcept {
    mix(state, value.amount);
    mix(state, value.pixel_scale);
    mix(state, value.chroma_ratio);
    mix(state, static_cast<core::u64>(value.seed));
    mix(state, static_cast<core::u64>(value.mode));
}

void mix(core::u64& state, const BoxBlurParameters& value) noexcept {
    mix(state, static_cast<core::u64>(value.radius_px));
    mix(state, static_cast<core::u64>(value.passes));
}

void mix(core::u64& state,
         const DirectionalBlurParameters& value) noexcept {
    mix(state, value.length_px);
    mix(state, value.angle_turns);
}

void mix(core::u64& state, const ChromaKeyParameters& value) noexcept {
    mix(state, value.key_color);
    mix(state, value.tolerance);
    mix(state, value.softness);
    mix(state, static_cast<core::u64>(
                   static_cast<core::i64>(value.morphology_px)));
    mix(state, value.spill_suppression);
    mix(state, value.mix);
}

void mix(core::u64& state, const LumaKeyParameters& value) noexcept {
    mix(state, value.low_threshold);
    mix(state, value.high_threshold);
    mix(state, value.low_softness);
    mix(state, value.high_softness);
    mix(state, static_cast<core::u64>(value.invert));
    mix(state, value.mix);
}

void mix(core::u64& state, const std::string& value) noexcept;

void mix(core::u64& state, const MaskBounds& value) noexcept {
    mix(state, value.left);
    mix(state, value.top);
    mix(state, value.right);
    mix(state, value.bottom);
}

void mix(core::u64& state, const MaskPoint& value) noexcept {
    mix(state, value.source.value());
    mix(state, value.scene.value());
    mix(state, value.mask.value());
    mix(state, static_cast<core::u64>(value.order));
    mix(state, value.x);
    mix(state, value.y);
    mix(state, value.in_x);
    mix(state, value.in_y);
    mix(state, value.out_x);
    mix(state, value.out_y);
}

void mix(core::u64& state, const MaskInstance& value) noexcept {
    mix(state, value.source.value());
    mix(state, value.scene.value());
    mix(state, value.name);
    mix(state, value.target.value());
    mix(state, static_cast<core::u64>(value.kind));
    mix(state, static_cast<core::u64>(value.combine));
    mix(state, static_cast<core::u64>(value.order));
    mix(state, static_cast<core::u64>(value.enabled));
    mix(state, value.feather_canvas_px);
    mix(state, value.expansion_canvas_px);
    mix(state, value.opacity);
    mix(state, static_cast<core::u64>(value.invert));
    mix(state, value.bounds);
    mix(state, value.rotation_turns);
    mix(state, static_cast<core::u64>(value.points.size()));
    for (const MaskPoint& point : value.points) {
        mix(state, point);
    }
}

void mix(core::u64& state, const LayerStyle& value) noexcept {
    mix(state, value.opacity);
    mix(state, static_cast<core::u64>(value.blend));
    mix(state, value.horizontal_blend);
    mix(state, static_cast<core::u64>(value.effects.brightness_enabled));
    mix(state, value.effects.brightness);
    mix(state,
        static_cast<core::u64>(value.effects.brightness_product_valid));
    if (value.effects.brightness_product_valid) {
        mix(state, value.effects.brightness_product);
    }
    mix(state, static_cast<core::u64>(value.effects.saturation_enabled));
    mix(state, value.effects.saturation);
    mix(state,
        static_cast<core::u64>(value.effects.saturation_product_valid));
    if (value.effects.saturation_product_valid) {
        mix(state, value.effects.saturation_product);
    }
    mix(state, static_cast<core::u64>(value.effects.blur_enabled));
    mix(state, value.effects.blur_radius);
    if (!value.effects.inline_operations.empty()) {
        mix(state, static_cast<core::u64>(
                       value.effects.inline_operations.size()));
        for (const InlineEffectOperation& operation :
             value.effects.inline_operations) {
            mix(state, static_cast<core::u64>(operation.kind));
            mix(state, operation.value);
            if (operation.kind == EffectKind::Levels) {
                mix(state, operation.levels);
            } else if (operation.kind == EffectKind::LiftGammaGain) {
                mix(state, operation.lift_gamma_gain);
            } else if (operation.kind == EffectKind::ContrastPivot) {
                mix(state, operation.contrast_pivot);
            } else if (operation.kind == EffectKind::ColorMatrix) {
                mix(state, operation.color_matrix);
            } else if (operation.kind == EffectKind::WhiteBalance) {
                mix(state, operation.white_balance);
            } else if (operation.kind == EffectKind::Curves) {
                mix(state, operation.curves);
            } else if (operation.kind == EffectKind::FilmGrain) {
                mix(state, operation.film_grain);
            }
        }
    }
    if (!value.effects.spatial_operations.empty()) {
        mix(state, static_cast<core::u64>(
                       value.effects.spatial_operations.size()));
        for (const SpatialEffectOperation& operation :
             value.effects.spatial_operations) {
            mix(state, static_cast<core::u64>(operation.kind));
            mix(state, operation.value);
            if (operation.kind == EffectKind::BoxBlur) {
                mix(state, operation.box_blur);
            } else if (operation.kind == EffectKind::DirectionalBlur) {
                mix(state, operation.directional_blur);
            } else if (operation.kind == EffectKind::ChromaKey) {
                mix(state, operation.chroma_key);
            } else if (operation.kind == EffectKind::LumaKey) {
                mix(state, operation.luma_key);
            }
        }
    }
}

void mix(core::u64& state, const Camera& value) noexcept {
    mix(state, value.eye);
    mix(state, value.target);
    mix(state, value.up);
    mix(state, value.field_of_view);
    // Conditional so every existing turn-based camera retains its historical
    // digest. Once authored, all calibration data participates because it
    // changes the tracked projection and entrance-pupil position.
    if (value.lens_profile.has_value()) {
        const LensProfile profile = clamped(*value.lens_profile);
        mix(state, 0x4C45'4E53'5052'4F46ULL); // "LENSPROF"
        mix(state, profile.sensor_width_mm);
        mix(state, profile.sensor_height_mm);
        mix(state, profile.focal_length_mm);
        mix(state, profile.entrance_pupil_offset_m);
        mix(state, profile.distortion.k1);
        mix(state, profile.distortion.k2);
        mix(state, profile.distortion.k3);
        mix(state, profile.distortion.p1);
        mix(state, profile.distortion.p2);
        mix(state, static_cast<core::u64>(profile.calibration_count));
        for (core::u32 index = 0U; index < profile.calibration_count; ++index) {
            const LensCalibrationPoint& point = profile.calibration[index];
            mix(state, point.zoom_encoder);
            mix(state, point.focus_encoder);
            mix(state, point.focal_length_mm);
            mix(state, point.entrance_pupil_offset_m);
            mix(state, point.distortion.k1);
            mix(state, point.distortion.k2);
            mix(state, point.distortion.k3);
            mix(state, point.distortion.p1);
            mix(state, point.distortion.p2);
        }
    }
    mix(state, value.near_plane);
    mix(state, value.far_plane);
    mix(state, value.focus_distance_m);
    mix(state, value.aperture_f_number);
    mix(state, value.shutter_seconds);
    mix(state, value.iso);
    mix(state, value.exposure_compensation_ev);
}

void mix(core::u64& state, const TrackedCamera& value) noexcept {
    mix(state, value.position);
    mix(state, value.yaw);
    mix(state, value.pitch);
    mix(state, value.roll);
    mix(state, value.field_of_view);
    mix(state, value.aspect_ratio);
    mix(state, value.focal_length_mm);
    mix(state, value.focus_distance_m);
    mix(state, value.aperture_f_number);
    mix(state, value.control_frame);
    mix(state, value.sequence);
    mix(state, static_cast<core::u64>(value.valid));
    mix(state, static_cast<core::u64>(value.held));
}

void mix(core::u64& state, const IcvfxCameraSettings& value) noexcept {
    const IcvfxCameraSettings bounded = clamped(value);
    mix(state, static_cast<core::u64>(bounded.inner_frustum));
    mix(state, static_cast<core::u64>(bounded.margin_px));
    mix(state, static_cast<core::u64>(bounded.soft_edge_px));
    mix(state, static_cast<core::u64>(bounded.resolution_numerator));
    mix(state, static_cast<core::u64>(bounded.resolution_denominator));
    mix(state, static_cast<core::u64>(
                   static_cast<core::i64>(bounded.render_order)));
}

void mix(core::u64& state, const LensEncoders& value) noexcept {
    const LensEncoders bounded = clamped(value);
    mix(state, bounded.zoom);
    mix(state, bounded.focus);
    mix(state, bounded.iris);
    mix(state, static_cast<core::u64>(bounded.zoom_valid));
    mix(state, static_cast<core::u64>(bounded.focus_valid));
    mix(state, static_cast<core::u64>(bounded.iris_valid));
}

void mix(core::u64& state, CanvasRect value) noexcept {
    mix(state, static_cast<core::u64>(value.x));
    mix(state, static_cast<core::u64>(value.y));
    mix(state, static_cast<core::u64>(value.width));
    mix(state, static_cast<core::u64>(value.height));
}

void mix(core::u64& state, MeshKey value) noexcept {
    mix(state, static_cast<core::u64>(value.primitive));
    mix(state, static_cast<core::u64>(value.segments));
}

void mix(core::u64& state, const std::string& value) noexcept {
    // Length before bytes, so one string that is a prefix of another cannot
    // hash equal.
    mix(state, static_cast<core::u64>(value.size()));
    for (const auto byte : value) {
        mix(state, static_cast<core::u64>(static_cast<unsigned char>(byte)));
    }
}

} // namespace

// Defined beside `clamped(ColorSettings)` below; declared here because the
// scene digest above it needs the same mix the profile digest uses.
void mix_color(core::u64& state, const ColorSettings& value) noexcept;

double SceneTime::seconds() const noexcept {
    if (rate_numerator == 0U) {
        return 0.0;
    }
    return static_cast<double>(frame) * static_cast<double>(rate_denominator)
        / static_cast<double>(rate_numerator);
}

float fold_mask_coverage(float accumulated, float coverage,
                         MaskCombine combine) noexcept {
    switch (combine) {
    case MaskCombine::Add:
        return std::max(accumulated, coverage);
    case MaskCombine::Subtract:
        return accumulated * (1.0F - coverage);
    case MaskCombine::Intersect:
        return std::min(accumulated, coverage);
    case MaskCombine::Difference:
        return std::abs(accumulated - coverage);
    }
    return accumulated;
}

const CanvasRect* Scene::slice_for(core::u32 node_index) const noexcept {
    for (const auto& binding : slices) {
        if (binding.node_index == node_index) {
            return &binding.rect;
        }
    }
    return nullptr;
}

core::u64 digest(const Scene& scene) noexcept {
    auto state = fnv_offset_basis;
    if (!scene.wall_colors.empty()) mix(state, asset_set_digest(scene));

    mix(state, scene.id.value());
    if (scene.document_version.value() != 0U) {
        mix(state, 0x444F'4356'4552'7631ULL); // "DOCVERv1"
        mix(state, scene.document_version.value());
    }
    mix(state, scene.show_frame);
    if (scene.temporal_document_digest) {
        mix(state, 0x54454D50444F4301ULL);
        mix(state, *scene.temporal_document_digest);
    }
    mix(state, static_cast<core::u64>(scene.canvas.width));
    mix(state, static_cast<core::u64>(scene.canvas.height));
    mix(state, scene.camera);
    // Preserve historical hand-built scene digests while making an authored
    // camera cut part of replicated scene identity.
    if (scene.camera_source.value() != 0U) {
        mix(state, 0x4341'4D45'5241'4944ULL); // "CAMERAID"
        mix(state, scene.camera_source.value());
    }
    // Conditional, so a project that binds no camera digests exactly as it did
    // before the inner frustum existed. Once cameras are bound the whole stack
    // participates - which camera, which feed, how each plate is drawn, in the
    // order they are composited - because all of it decides the picture and
    // two nodes have to agree on every part of it.
    //
    // The order matters as much as the contents: two nodes that composited the
    // same plates in different orders would disagree wherever two overlap, so
    // the sequence is digested as a sequence rather than as a set.
    if (!scene.tracked_cameras.empty()) {
        mix(state, 0x5452'4B53'5441'434BULL); // "TRKSTACK"
        mix(state, static_cast<core::u64>(scene.tracked_cameras.size()));
        for (const TrackedCameraView& view : scene.tracked_cameras) {
            mix(state, view.camera.value());
            mix(state, view.source.value());
            mix(state, view.authored);
            mix(state, view.icvfx);
            if (view.pose.valid) {
                mix(state, 0x5452'4143'4B43'414DULL); // "TRACKCAM"
                mix(state, view.pose);
                // Encoders alter the image only when a calibrated lens exists.
                // Keeping this conditional means a tracked turn-based camera
                // digests without them while any lens disagreement is still
                // caught.
                if (view.authored.lens_profile.has_value()) {
                    mix(state, 0x4C45'4E53'454E'434FULL); // "LENSENCO"
                    mix(state, view.encoders);
                }
            }
        }
    }
    if (scene.environment.baked != BakedLighting{}) {
        mix(state, core::u64{0x42414b4544474931ULL});
        mix(state, static_cast<core::u64>(scene.environment.baked.enabled));
        mix(state, scene.environment.baked.minimum.x);
        mix(state, scene.environment.baked.minimum.y);
        mix(state, scene.environment.baked.minimum.z);
        mix(state, scene.environment.baked.maximum.x);
        mix(state, scene.environment.baked.maximum.y);
        mix(state, scene.environment.baked.maximum.z);
        mix(state, static_cast<core::u64>(scene.environment.baked.count_x));
        mix(state, static_cast<core::u64>(scene.environment.baked.count_y));
        mix(state, static_cast<core::u64>(scene.environment.baked.count_z));
        mix(state, static_cast<core::u64>(scene.environment.baked.samples));
        mix(state, static_cast<core::u64>(scene.environment.baked.bounces));
        mix(state, scene.environment.baked.strength);
        mix(state, scene.environment.baked.source);
        mix(state, scene.environment.baked.content);
        for (const auto& probe : scene.environment.baked.reflections) {
            if (probe == ReflectionProbe{}) continue;
            mix(state, probe.source.value());
            mix(state, static_cast<core::u64>(probe.enabled));
            mix(state, static_cast<core::u64>(probe.sphere));
            mix(state, probe.position); mix(state, probe.extent);
            mix(state, probe.blend); mix(state, probe.intensity);
        }
    }
    if (scene.environment.planar != PlanarReflection{}) {
        const auto& p = scene.environment.planar;
        mix(state, core::u64{0x504c414e4152});
        mix(state, static_cast<core::u64>(p.enabled)); mix(state, p.receiver.value());
        mix(state, p.point); mix(state, p.normal); mix(state, p.strength);
    }
    if (scene.asset_refresh_generation != 0) { mix(state, core::u64{0x415353455447454eULL}); mix(state, scene.asset_refresh_generation); }
    if (scene.asset_commit.digest != 0) {
        mix(state, scene.asset_commit.digest);
        mix(state, scene.asset_commit.control_frame);
        mix(state, scene.asset_commit.show_frame);
    }
    mix(state, scene.environment.sky_color);
    mix(state, scene.environment.ground_color);
    mix(state, scene.environment.ambient);
    mix(state, scene.environment.map_source);
    mix(state, scene.environment.map_intensity);
    mix(state, scene.environment.map_rotation);
    if (scene.environment.physical_sky != PhysicalSky{}) {
        const auto& sky = scene.environment.physical_sky;
        mix(state, core::u64{0x504859534b593031ULL});
        mix(state, static_cast<core::u64>(sky.enabled));
        mix(state, sky.time_of_day); mix(state, sky.latitude);
        mix(state, sky.north_rotation); mix(state, sky.haze);
        mix(state, sky.solar_illuminance); mix(state, sky.angular_radius);
        mix(state, sky.ground_albedo);
    }
    mix(state, scene.environment.fog_density);
    mix(state, scene.environment.fog_color);
    mix(state, scene.environment.fog_anisotropy);
    // Conditional so a scene that authored uniform air digests exactly as it
    // did before height falloff existed.
    if (scene.environment.fog_height != 0.0F
        || scene.environment.fog_height_falloff != 0.0F) {
        mix(state, core::u64{0x464F'4748'4549'4748ULL}); // "FOGHEIGH"
        mix(state, scene.environment.fog_height);
        mix(state, scene.environment.fog_height_falloff);
    }
    if (scene.environment.fog_background_distance != 0.0F) {
        mix(state, core::u64{0x464F'4742'4744'4953ULL}); // "FOGBGDIS"
        mix(state, scene.environment.fog_background_distance);
    }

    // Keep the historical digest of a group-free scene stable. Once hierarchy
    // exists, every authored/effective group value participates so checkpoint
    // and streamed projections compare the same complete carrier.
    if (!scene.groups.empty() || !scene.group_diagnostics.empty()) {
        // Domain marker: the conditional extension preserves old group-free
        // digests without making an invalid-hierarchy payload ambiguous with
        // the next legacy vector count.
        mix(state, core::u64{0x4752'4F55'5053'7631ULL}); // "GROUPSv1"
        mix(state, static_cast<core::u64>(scene.groups.size()));
        for (const SceneGroup& group : scene.groups) {
            mix(state, group.node.value());
            mix(state, group.scene.value());
            mix(state, group.name);
            mix(state, static_cast<core::u64>(
                           group.parent_group.has_value()));
            if (group.parent_group.has_value()) {
                mix(state, group.parent_group->value());
            }
            mix(state, static_cast<core::u64>(group.group_order));
            mix(state, group.position);
            mix(state, group.yaw);
            mix(state, group.pitch);
            mix(state, group.roll);
            mix(state, group.scale);
            mix(state, group.world);
            mix(state, static_cast<core::u64>(group.locked));
            mix(state, static_cast<core::u64>(group.visible));
            mix(state, static_cast<core::u64>(group.effective_locked));
            mix(state, static_cast<core::u64>(group.effective_visible));
        }
        mix(state,
            static_cast<core::u64>(scene.group_diagnostics.size()));
        for (const GroupDiagnostic& diagnostic :
             scene.group_diagnostics) {
            mix(state, static_cast<core::u64>(diagnostic.kind));
            mix(state, diagnostic.subject.value());
            mix(state, diagnostic.related.value());
        }
    }

    // Screens extend the digest only when a document has tracks, keeping the
    // digest of every project authored before they existed exactly as it was.
    if (!scene.screens.empty()) {
        mix(state, core::u64{0x5343'5245'454E'7631ULL}); // "SCREENv1"
        mix(state, static_cast<core::u64>(scene.screens.size()));
        for (const SceneScreen& screen : scene.screens) {
            mix(state, screen.node.value());
            mix(state, screen.scene.value());
            mix(state, screen.name);
            mix(state, static_cast<core::u64>(screen.order));
            mix(state, screen.world);
            mix(state, static_cast<core::u64>(screen.raster_width));
            mix(state, static_cast<core::u64>(screen.raster_height));
            mix(state, static_cast<core::u64>(screen.aspect_locked));
            mix(state, screen.style.opacity);
            mix(state, static_cast<core::u64>(screen.style.blend));
            mix(state, static_cast<core::u64>(screen.muted));
            mix(state, static_cast<core::u64>(screen.locked));
        }
    }

    if (!scene.masks.empty() || !scene.mask_programs.empty()
        || !scene.mask_diagnostics.empty()) {
        mix(state, core::u64{0x4D41'534B'5376'3031ULL}); // "MASKSv01"
        mix(state, static_cast<core::u64>(scene.masks.size()));
        for (const MaskInstance& mask : scene.masks) {
            mix(state, mask);
        }
        mix(state, static_cast<core::u64>(scene.mask_programs.size()));
        for (const MaskProgram& program : scene.mask_programs) {
            mix(state, program.target.value());
            mix(state, static_cast<core::u64>(program.coordinate_space));
            mix(state, static_cast<core::u64>(program.operations.size()));
            for (const MaskInstance& operation : program.operations) {
                mix(state, operation);
            }
        }
        mix(state, static_cast<core::u64>(
                       scene.mask_diagnostics.size()));
        for (const MaskDiagnostic& diagnostic : scene.mask_diagnostics) {
            mix(state, static_cast<core::u64>(diagnostic.kind));
            mix(state, diagnostic.subject.value());
            mix(state, diagnostic.related.value());
        }
    }

    // Counts are hashed before their contents so that a scene which is a
    // prefix of another cannot collide with it.
    mix(state, static_cast<core::u64>(scene.instances.size()));
    for (const auto& instance : scene.instances) {
        mix(state, static_cast<core::u64>(instance.mesh.primitive));
        mix(state, static_cast<core::u64>(instance.mesh.segments));
        mix(state, instance.world);
        mix(state, instance.normal);
        mix(state, instance.material);
        mix(state, instance.source.value());
        mix(state, instance.style);
        if (!instance.bake_static) mix(state, core::u64{0x42414b454f4646ULL});
        mix(state, static_cast<core::u64>(instance.locked));
        if (instance.parent_group.has_value()
            || instance.group_order != 0) {
            mix(state, static_cast<core::u64>(
                           instance.parent_group.has_value()));
            if (instance.parent_group.has_value()) {
                mix(state, instance.parent_group->value());
            }
            mix(state, static_cast<core::u64>(instance.group_order));
        }
    }

    mix(state, static_cast<core::u64>(scene.media.size()));
    for (const auto& clip : scene.media) {
        mix(state, clip.mesh);
        mix(state, clip.world);
        mix(state, clip.normal);
        mix(state, clip.source);
        mix(state, static_cast<core::u64>(clip.frame_index));
        mix(state, clip.asset.value());
        mix(state, clip.source_node.value());
        mix(state, clip.timeline_lane.value());
        mix(state, clip.compositing_order);
        mix(state, clip.style);
        mix(state, static_cast<core::u64>(clip.locked));
        if (clip.parent_group.has_value() || clip.group_order != 0) {
            mix(state, static_cast<core::u64>(
                           clip.parent_group.has_value()));
            if (clip.parent_group.has_value()) {
                mix(state, clip.parent_group->value());
            }
            mix(state, static_cast<core::u64>(clip.group_order));
        }
    }

    mix(state, static_cast<core::u64>(scene.models.size()));
    for (const auto& model : scene.models) {
        mix(state, model.world);
        mix(state, model.normal);
        mix(state, model.source);
        mix(state, model.source_node.value());
        mix(state, model.material);
        mix(state, model.style);
        if (!model.bake_static) mix(state, core::u64{0x42414b454f4646ULL});
        if (model.animation != ModelAnimation{}) {
            mix(state,core::u64{0x414e494d415445ULL});
            mix(state,static_cast<core::u64>(model.animation.enabled));
            mix(state,static_cast<core::u64>(model.animation.clip));
            mix(state,static_cast<core::u64>(model.animation.loop));
            mix(state,model.animation.start_frame); mix(state,model.animation.wind);
            mix(state,static_cast<core::u64>(model.animation.wind_period_frames));
        }
        mix(state, static_cast<core::u64>(model.locked));
        if (model.parent_group.has_value() || model.group_order != 0) {
            mix(state, static_cast<core::u64>(
                           model.parent_group.has_value()));
            if (model.parent_group.has_value()) {
                mix(state, model.parent_group->value());
            }
            mix(state, static_cast<core::u64>(model.group_order));
        }
    }

    mix(state, static_cast<core::u64>(scene.effects.size()));
    for (const auto& effect : scene.effects) {
        mix(state, effect.source.value());
        mix(state, effect.target.value());
        mix(state, static_cast<core::u64>(effect.kind));
        mix(state, static_cast<core::u64>(effect.order));
        mix(state, static_cast<core::u64>(effect.enabled));
        mix(state, effect.value);
        if (effect.kind == EffectKind::Levels) {
            mix(state, effect.levels);
        } else if (effect.kind == EffectKind::LiftGammaGain) {
            mix(state, effect.lift_gamma_gain);
        } else if (effect.kind == EffectKind::ContrastPivot) {
            mix(state, effect.contrast_pivot);
        } else if (effect.kind == EffectKind::ColorMatrix) {
            mix(state, effect.color_matrix);
        } else if (effect.kind == EffectKind::WhiteBalance) {
            mix(state, effect.white_balance);
        } else if (effect.kind == EffectKind::Curves) {
            mix(state, effect.curves);
        } else if (effect.kind == EffectKind::FilmGrain) {
            mix(state, effect.film_grain);
        } else if (effect.kind == EffectKind::BoxBlur) {
            mix(state, effect.box_blur);
        } else if (effect.kind == EffectKind::DirectionalBlur) {
            mix(state, effect.directional_blur);
        } else if (effect.kind == EffectKind::ChromaKey) {
            mix(state, effect.chroma_key);
        } else if (effect.kind == EffectKind::LumaKey) {
            mix(state, effect.luma_key);
        }
    }

    mix(state, static_cast<core::u64>(scene.directional_lights.size()));
    for (const auto& light : scene.directional_lights) {
        mix(state, light.direction);
        mix(state, light.color);
        mix(state, light.intensity);
        mix(state, light.source.value());
        if (!light.bake_indirect) mix(state, core::u64{0x42414b454f4646ULL});
        mix(state, static_cast<core::u64>(light.locked));
        mix(state, static_cast<core::u64>(light.unit));
        mix(state, light.angular_radius);
        mix(state, light.color_temperature_kelvin);
        mix(state, static_cast<core::u64>(light.use_color_temperature));
    }

    mix(state, static_cast<core::u64>(scene.point_lights.size()));
    for (const auto& light : scene.point_lights) {
        mix(state, light.ies_source);
        mix(state, light.ies_rotation);
        mix(state, light.position);
        mix(state, light.color);
        mix(state, light.intensity);
        mix(state, light.range);
        mix(state, light.source.value());
        if (!light.bake_indirect) mix(state, core::u64{0x42414b454f4646ULL});
        mix(state, static_cast<core::u64>(light.locked));
        mix(state, static_cast<core::u64>(light.kind));
        mix(state, static_cast<core::u64>(light.unit));
        mix(state, light.direction);
        mix(state, light.inner_cone);
        mix(state, light.outer_cone);
        mix(state, light.width);
        mix(state, light.height);
        mix(state, static_cast<core::u64>(light.two_sided));
        mix(state, light.radius);
        mix(state, light.area_rotation);
        mix(state, light.color_temperature_kelvin);
        mix(state, static_cast<core::u64>(light.use_color_temperature));
    }

    mix(state, static_cast<core::u64>(scene.slices.size()));
    for (const auto& binding : scene.slices) {
        mix(state, static_cast<core::u64>(binding.node_index));
        mix(state, binding.rect);
    }

    mix(state, static_cast<core::u64>(scene.effect_settings.blur_taps));
    mix(state, scene.effect_settings.blur_sigma_ratio);
    mix(state, static_cast<core::u64>(scene.effect_settings.blur_passes));
    mix(state, static_cast<core::u64>(
                   scene.effect_settings.directional_blur_taps));
    mix(state, static_cast<core::u64>(
                   static_cast<core::u8>(
                       scene.effect_settings.brightness_curve)));
    mix(state, scene.effect_settings.brightness_gamma);

    if (scene.renderer_settings != RendererSettings{}) {
        mix(state, core::u64{0x5245'4E44'4552'7631ULL}); // "RENDERv1"
        mix(state, static_cast<core::u64>(scene.renderer_settings.backend));
        mix(state, static_cast<core::u64>(scene.renderer_settings.required));
        mix(state, static_cast<core::u64>(scene.renderer_settings.quality));
        mix(state,
            static_cast<core::u64>(scene.renderer_settings.ray_tracing));
        mix(state, static_cast<core::u64>(scene.renderer_settings.ddgi));
        mix(state, static_cast<core::u64>(scene.renderer_settings.restir));
        mix(state, static_cast<core::u64>(scene.renderer_settings.denoising));
        mix(state, static_cast<core::u64>(scene.renderer_settings.taau));
        mix(state,
            static_cast<core::u64>(scene.renderer_settings.motion_blur));
        mix(state, static_cast<core::u64>(scene.renderer_settings.motion_blur_radius));
        mix(state, static_cast<core::u64>(scene.renderer_settings.plate_depth_of_field));
        mix(state, static_cast<core::u64>(scene.renderer_settings.plate_dof_radius));
        if (scene.renderer_settings.motion_blur) {
            mix(state, static_cast<core::u64>(scene.show_rate.numerator));
            mix(state, static_cast<core::u64>(scene.show_rate.denominator));
        }
        mix(state, static_cast<core::u64>(
                       scene.renderer_settings.spatial_antialiasing));
        mix(state, scene.renderer_settings.shadow_blend_band);
        mix(state, scene.renderer_settings.shadow_filter_radius);
        mix(state, scene.renderer_settings.contact_shadow_length);
        mix(state, scene.renderer_settings.contact_shadow_thickness);
        mix(state, scene.renderer_settings.occlusion_radius);
        mix(state, scene.renderer_settings.occlusion_strength);
        mix(state, scene.renderer_settings.bloom_strength);
        if (scene.renderer_settings.photometric_scale != RendererSettings{}.photometric_scale)
            mix(state, scene.renderer_settings.photometric_scale);
        mix(state, static_cast<core::u64>(
                       scene.renderer_settings.taa_sequence_length));
        mix(state, static_cast<core::u64>(
                       scene.renderer_settings.ddgi_probe_count_x));
        mix(state, static_cast<core::u64>(
                       scene.renderer_settings.ddgi_probe_count_y));
        mix(state, static_cast<core::u64>(
                       scene.renderer_settings.ddgi_probe_count_z));
        mix(state, static_cast<core::u64>(
                       scene.renderer_settings.ddgi_rays_per_probe));
        mix(state, static_cast<core::u64>(
                       scene.renderer_settings.slice_apron_limit));
        mix(state, static_cast<core::u64>(
                       scene.renderer_settings.shadow_cascades));
        mix(state, scene.renderer_settings.gi_strength);
    if (!scene.renderer_settings.frustum_culling || !scene.renderer_settings.geometry_instancing || !scene.renderer_settings.model_lod) {
        mix(state, core::u64{0x42414b4544474931ULL});
    mix(state, core::u64{0x4745'4F4D'4554'5259ULL});
        mix(state, static_cast<core::u64>(scene.renderer_settings.frustum_culling));
        mix(state, static_cast<core::u64>(scene.renderer_settings.geometry_instancing));
        mix(state, static_cast<core::u64>(scene.renderer_settings.model_lod));
    }
        mix(state, static_cast<core::u64>(scene.renderer_settings.texture_streaming));
        mix(state, static_cast<core::u64>(scene.renderer_settings.texture_budget_mib));
        if (scene.renderer_settings.frame_budget_us != 0U) {
            mix(state, 0x425544474554ULL);
            mix(state, static_cast<core::u64>(scene.renderer_settings.frame_budget_us));
        }
        mix(state, static_cast<core::u64>(scene.renderer_settings.texture_mip_bias));
        mix(state, core::u64{0x5341'4D50'4C45'0001ULL}); // Sampling contract v1.
    mix(state, core::u64{0x5445'4D50'4F52'0002ULL}); // Depth-aware temporal contract v2.
        mix(state, static_cast<core::u64>(scene.renderer_settings.texture_anisotropy));
        mix(state, scene.renderer_settings.texture_lod_bias);
        // Conditional, like the profile digest: a project that never touched
        // colour keeps the digest it had before the output stage was authored.
        if (scene.renderer_settings.color != ColorSettings{}) {
            mix_color(state, scene.renderer_settings.color);
        }
    }

    return state;
}

Camera tracked_view_camera(
    const Camera& authored, const TrackedCamera& tracked) noexcept {
    return tracked_view_camera(authored, tracked, LensEncoders{});
}

Camera tracked_view_camera(const Camera& authored,
                           const TrackedCamera& tracked,
                           const LensEncoders& encoders) noexcept {
    if (!tracked.valid) {
        return authored;
    }
    // Near/far and every exposure control stay authored: they are lens and
    // shading policy, and a tracker has no opinion about them.
    Camera result = authored;
    result.eye = tracked.position;
    // The tracker reports orientation, not a look-at target, so the target is
    // reconstructed one unit along the rotated forward axis. Composed by the
    // scene's own fixed-order rotation so no consumer picks an Euler
    // convention, and normalised to keep the eye-to-target distance from
    // affecting anything downstream.
    const Mat4 basis = rotation(tracked.yaw, tracked.pitch, tracked.roll);
    const Vec3 forward =
        normalize(transform_point(basis, Vec3{0.0F, 0.0F, -1.0F}));
    const Vec3 up = normalize(transform_point(basis, Vec3{0.0F, 1.0F, 0.0F}),
                              Vec3{0.0F, 1.0F, 0.0F});
    // The tracker names a marker on the camera body. Rays originate at the
    // lens entrance pupil, which can shift with zoom/focus. The profile is
    // authored state and the encoder input is frame-stamped, so every node
    // reaches this exact point without any local interpolation or cache.
    float pupil_offset = 0.0F;
    if (authored.lens_profile.has_value()) {
        pupil_offset = evaluate_lens(*authored.lens_profile, encoders)
                           .entrance_pupil_offset_m;
    }
    result.eye = tracked.position + forward * pupil_offset;
    result.target = result.eye + forward;
    result.up = up;
    result.field_of_view = tracked.field_of_view;
    return result;
}

Vec3 stage_position_from_tracker(
    Vec3 reported, const TrackerCalibration& calibration) noexcept {
    const TrackerCalibration bounded = clamped(calibration);
    // Axis map first, then scale, then origin. Fixed order, because three
    // operations that commute in one arrangement and not another is one more
    // thing two nodes could disagree about.
    const Vec3 mapped = bounded.axes == TrackerAxes::ZUpRightHanded
        // The tracker's +Z is stage up and its +Y is stage forward, which is
        // stage -Z. A rotation of a quarter turn about +X, written out rather
        // than composed, so the sign of every term is visible.
        ? Vec3{reported.x, reported.z, -reported.y}
        : reported;
    return Vec3{
        mapped.x * bounded.scale + bounded.origin.x,
        mapped.y * bounded.scale + bounded.origin.y,
        mapped.z * bounded.scale + bounded.origin.z,
    };
}

Turns stage_pan_from_tracker(
    Turns reported, const TrackerCalibration& calibration) noexcept {
    // The axis map above preserves the sense of rotation in the horizontal
    // plane, so heading is the whole orientation correction. Wrapped, so a
    // heading authored near a turn boundary cannot drift the angle out of the
    // range every consumer expects.
    return wrap_turns(
        reported + clamped(calibration).heading);
}

TrackerCalibration clamped(const TrackerCalibration& value) noexcept {
    TrackerCalibration result{};
    result.axes = value.axes == TrackerAxes::ZUpRightHanded
        ? TrackerAxes::ZUpRightHanded
        : TrackerAxes::StageNative;
    const auto bounded = [](float candidate, float limit,
                            float fallback) noexcept {
        return std::isfinite(candidate) ? std::clamp(candidate, -limit, limit)
                                        : fallback;
    };
    result.origin = Vec3{
        bounded(value.origin.x, max_tracker_origin_m, 0.0F),
        bounded(value.origin.y, max_tracker_origin_m, 0.0F),
        bounded(value.origin.z, max_tracker_origin_m, 0.0F),
    };
    result.heading = std::isfinite(value.heading)
        ? wrap_turns(value.heading)
        : 0.0F;
    result.scale = std::isfinite(value.scale)
        ? std::clamp(value.scale, min_tracker_scale, max_tracker_scale)
        : 1.0F;
    return result;
}

TrackingSourceSettings clamped(const TrackingSourceSettings& value) noexcept {
    TrackingSourceSettings result = value;
    result.protocol = value.protocol == TrackerProtocolKind::OpenTrackIO
        ? TrackerProtocolKind::OpenTrackIO
        : TrackerProtocolKind::FreeD;
    result.port = std::min(value.port, max_tracking_port);
    const auto bounded = [](float candidate, float low, float high,
                            float fallback) noexcept {
        return std::isfinite(candidate) ? std::clamp(candidate, low, high)
                                        : fallback;
    };
    result.sensor_height_mm =
        bounded(value.sensor_height_mm, min_sensor_extent_mm,
                max_sensor_extent_mm, TrackingSourceSettings{}.sensor_height_mm);
    result.sensor_aspect_ratio =
        bounded(value.sensor_aspect_ratio, 0.1F, 10.0F,
                TrackingSourceSettings{}.sensor_aspect_ratio);
    result.fallback_focal_length_mm = bounded(
        value.fallback_focal_length_mm, min_focal_length_mm,
        max_focal_length_mm,
        TrackingSourceSettings{}.fallback_focal_length_mm);
    result.calibration = clamped(value.calibration);
    // A source with no port cannot be listening, whatever the operator ticked.
    // Saying so here means no downstream reader has to guard the same thing.
    result.enabled = value.enabled && result.port != 0U;
    return result;
}

Turns tracked_field_of_view(
    const TrackingSourceSettings& source, float focal_length_mm) noexcept {
    const TrackingSourceSettings bounded = clamped(source);
    const float focal = std::isfinite(focal_length_mm)
            && focal_length_mm >= min_focal_length_mm
            && focal_length_mm <= max_focal_length_mm
        ? focal_length_mm
        : bounded.fallback_focal_length_mm;
    // Half the sensor height over the focal length is a tangent; twice its
    // arctangent is the vertical angle the camera sees. The clamps above bound
    // the ratio at 100, whose angle is under half a turn, so this can never
    // produce a field of view a frustum cannot be built from.
    const float half_height = bounded.sensor_height_mm * 0.5F;
    const Turns half_angle = deterministic::atan_turns(half_height / focal);
    return half_angle + half_angle;
}

IcvfxCameraSettings clamped(const IcvfxCameraSettings& value) noexcept {
    IcvfxCameraSettings result = value;
    result.margin_px = std::min(value.margin_px, max_icvfx_margin_px);
    result.soft_edge_px = std::min(value.soft_edge_px, max_icvfx_soft_edge_px);
    result.resolution_numerator =
        std::clamp(value.resolution_numerator, 1U, max_icvfx_resolution_scale);
    result.resolution_denominator = std::clamp(
        value.resolution_denominator, 1U, max_icvfx_resolution_scale);
    // A feather wider than the margin would reach outside the rectangle the
    // margin bought, so the outer frustum would show through the plate edge.
    result.soft_edge_px = std::min(result.soft_edge_px, result.margin_px);
    return result;
}

IcvfxSettings clamped(const IcvfxSettings& value) noexcept {
    IcvfxSettings result = value;
    result.latency_frames =
        std::min(value.latency_frames, max_icvfx_latency_frames);
    result.prediction_window = std::clamp(value.prediction_window, 2U,
                                         max_pose_prediction_window);
    result.jitter_filter_percent = std::min(value.jitter_filter_percent, 100U);
    result.camera_defaults = clamped(value.camera_defaults);
    return result;
}

ColorSettings clamped(const ColorSettings& value) noexcept {
    ColorSettings result = value;
    // An enumerator this build does not know falls back to the reference
    // transform rather than indexing a table that does not exist. The parser
    // refuses unknown names before they get here; this guards the in-memory
    // path the same way.
    result.display_transform =
        value.display_transform == DisplayTransform::Srgb
        ? DisplayTransform::Srgb
        : value.display_transform == DisplayTransform::Aces13 ? DisplayTransform::Aces13
        : DisplayTransform::AcesApproximate;
    return result;
}

void mix_color(core::u64& state, const ColorSettings& source) noexcept {
    const ColorSettings value = clamped(source);
    mix(state, core::u64{0x434F'4C4F'5250'4F4CULL}); // "COLORPOL"
    mix(state, static_cast<core::u64>(value.display_transform));
    mix(state, value.show_lut_source);
    mix(state, static_cast<core::u64>(value.dither));
}

WallColorSettings clamped(const WallColorSettings& value) noexcept {
    WallColorSettings result = value;
    for (std::size_t i = 0; i < result.primaries.size(); ++i) {
        result.primaries[i] = std::isfinite(value.primaries[i])
            ? std::clamp(value.primaries[i], -16.0F, 16.0F)
            : (i % 4U == 0U ? 1.0F : 0.0F);
    }
    return result;
}

RendererSettings clamped(const RendererSettings& value) noexcept {
    RendererSettings result = value;
    result.frame_budget_us = std::min(value.frame_budget_us, 1'000'000U);
    result.texture_budget_mib = std::clamp(value.texture_budget_mib, 1U, 512U);
    result.texture_mip_bias = std::min(value.texture_mip_bias, 12U);
    result.texture_anisotropy = value.texture_anisotropy >= 16U ? 16U
        : value.texture_anisotropy >= 8U ? 8U : value.texture_anisotropy >= 4U ? 4U
        : value.texture_anisotropy >= 2U ? 2U : 1U;
    result.plate_dof_radius = std::min(value.plate_dof_radius, max_plate_dof_radius);
    result.motion_blur_radius = std::min(value.motion_blur_radius, max_motion_blur_radius);
    result.texture_lod_bias = std::isfinite(value.texture_lod_bias)
        ? std::clamp(value.texture_lod_bias, -4.0F, 4.0F) : 0.0F;
    result.photometric_scale = std::isfinite(value.photometric_scale)
        ? std::clamp(value.photometric_scale, 0.000001F, 1.0F)
        : RendererSettings{}.photometric_scale;
    result.icvfx = clamped(value.icvfx);
    result.color = clamped(value.color);
    result.backend = value.backend == RendererBackend::Advanced
        ? RendererBackend::Advanced
        : RendererBackend::Basic;
    result.quality = value.quality == RendererQuality::Interactive
        ? RendererQuality::Interactive
        : value.quality == RendererQuality::Cinematic
        ? RendererQuality::Cinematic
        : RendererQuality::Balanced;
    result.ddgi_probe_count_x =
        std::clamp(value.ddgi_probe_count_x, 1U, max_ddgi_probe_axis);
    result.ddgi_probe_count_y =
        std::clamp(value.ddgi_probe_count_y, 1U, max_ddgi_probe_axis);
    result.ddgi_probe_count_z =
        std::clamp(value.ddgi_probe_count_z, 1U, max_ddgi_probe_axis);
    result.ddgi_rays_per_probe =
        std::clamp(value.ddgi_rays_per_probe, 1U, max_ddgi_rays_per_probe);
    result.slice_apron_limit =
        std::clamp(value.slice_apron_limit, 0U, max_slice_apron);
    result.shadow_blend_band = std::isfinite(value.shadow_blend_band)
        ? std::clamp(value.shadow_blend_band, 0.0F, 0.25F) : 0.1F;
    result.shadow_filter_radius = std::isfinite(value.shadow_filter_radius)
        ? std::clamp(value.shadow_filter_radius, 0.0F, 4.0F) : 2.0F;
    result.contact_shadow_length = std::isfinite(value.contact_shadow_length)
        ? std::clamp(value.contact_shadow_length, 0.0F, 10.0F) : 0.0F;
    result.contact_shadow_thickness = std::isfinite(value.contact_shadow_thickness)
        ? std::clamp(value.contact_shadow_thickness, 0.0F, 1.0F) : 0.02F;
    result.shadow_cascades =
        std::clamp(value.shadow_cascades, 0U, max_shadow_cascades);
    // Clamped rather than defaulted, so an out-of-range authored period still
    // lands on the same value on every node instead of one node clamping and
    // another falling back.
    result.taa_sequence_length = std::clamp(
        value.taa_sequence_length, min_taa_sequence_length,
        max_taa_sequence_length);
    // A non-finite authored strength is the project default rather than a
    // renderer that has to guard the same number again.
    result.gi_strength = std::isfinite(value.gi_strength)
        ? std::clamp(value.gi_strength, 0.0F, max_gi_strength)
        : RendererSettings{}.gi_strength;
    result.occlusion_radius = std::isfinite(value.occlusion_radius)
        ? std::clamp(value.occlusion_radius, 0.0F, max_occlusion_radius)
        : 0.0F;
    result.occlusion_strength = std::isfinite(value.occlusion_strength)
        ? std::clamp(value.occlusion_strength, 0.0F, max_occlusion_strength)
        : RendererSettings{}.occlusion_strength;
    result.bloom_strength = std::isfinite(value.bloom_strength)
        ? std::clamp(value.bloom_strength, 0.0F, max_bloom_strength)
        : 0.0F;
    return result;
}

core::u64 asset_generation_key(const Scene& source) noexcept {
    if(source.asset_manifest_digest == 0 && source.asset_refresh_generation == 0) return 0;
    core::u64 state = fnv_offset_basis;
    mix(state,source.asset_manifest_digest); mix(state,source.asset_refresh_generation); return state;
}

core::u64 asset_set_digest(const Scene& source) noexcept {
    core::u64 state = fnv_offset_basis;
    bool any = source.renderer_settings.backend == RendererBackend::Advanced;
    if (asset_generation_key(source) != 0) {
        any = true;
        mix(state, asset_generation_key(source));
    }
    if (any) mix(state, renderer_settings_digest(source.renderer_settings));
    const auto reference = [&](const std::string& path, core::u64 kind) {
        if (path.empty()) return;
        any = true; mix(state, kind); mix(state, path);
    };
    const auto material = [&](const Material& m) {
        core::u64 kind = 100;
        for (const auto* path : {&m.base_color_texture, &m.metallic_roughness_texture, &m.normal_texture,
            &m.occlusion_texture, &m.emissive_texture, &m.clearcoat_texture, &m.clearcoat_roughness_texture,
            &m.clearcoat_normal_texture, &m.sheen_color_texture, &m.sheen_roughness_texture,
            &m.transmission_texture, &m.specular_texture, &m.specular_color_texture, &m.anisotropy_texture}) reference(*path, kind++);
    };
    for (const auto& model : source.models) { reference(model.source, 1); material(model.material); }
    for (const auto& instance : source.instances) material(instance.material);
    for (const auto& media : source.media) {
        // A local proxy changes its path while retaining the Library identity.
        // The replicated asset ID remains the cohort's source identity.
        if (media.asset.value() != 0) { any = true; mix(state, core::u64{2}); mix(state, media.asset.value()); }
        else reference(media.source, 2);
    }
    for (const auto& light : source.point_lights) reference(light.ies_source, 6);
    if (!source.environment.physical_sky.enabled) {
        reference(source.environment.map_source, 3);
        if (!source.environment.map_source.empty()) mix(state, source.environment.map_rotation);
    }
    reference(source.renderer_settings.color.show_lut_source, 4);
    for (const auto& [id, authored] : source.wall_colors) {
        const auto wall = clamped(authored);
        any = true; mix(state, core::u64{0x57414c4c434f4c52ULL}); mix(state, id.value());
        mix(state, static_cast<core::u64>(wall.enabled));
        for (float c : wall.primaries) mix(state, c);
        mix(state, wall.camera_inverse_lut_source);
    }
    if (source.environment.baked.enabled || std::any_of(source.environment.baked.reflections.begin(), source.environment.baked.reflections.end(), [](const auto& p) { return p.enabled; })) {
        any = true; mix(state, core::u64{5}); mix(state, source.environment.baked.source); mix(state, source.environment.baked.content);
    }
    return any ? state : 0;
}

core::u64 renderer_settings_digest(const RendererSettings& source) noexcept {
    const RendererSettings value = clamped(source);
    core::u64 state = 0x5245'4E44'504F'4C31ULL; // "RENDPOL1"
    mix(state, static_cast<core::u64>(value.backend));
    mix(state, static_cast<core::u64>(value.required));
    mix(state, static_cast<core::u64>(value.quality));
    mix(state, static_cast<core::u64>(value.ray_tracing));
    mix(state, static_cast<core::u64>(value.ddgi));
    mix(state, static_cast<core::u64>(value.restir));
    mix(state, static_cast<core::u64>(value.denoising));
    mix(state, static_cast<core::u64>(value.taau));
    mix(state, static_cast<core::u64>(value.motion_blur));
    mix(state, static_cast<core::u64>(value.motion_blur_radius));
    mix(state, static_cast<core::u64>(value.plate_depth_of_field));
    mix(state, static_cast<core::u64>(value.plate_dof_radius));
    // Object-only velocity, fixed shutter reconstruction and its apron contract.
    mix(state, core::u64{0x4F42'4A4D'4F54'0001ULL});
    mix(state, static_cast<core::u64>(value.spatial_antialiasing));
    mix(state, value.shadow_blend_band);
    mix(state, value.shadow_filter_radius);
    mix(state, value.contact_shadow_length);
    mix(state, value.contact_shadow_thickness);
    mix(state, value.occlusion_radius);
    mix(state, value.occlusion_strength);
    mix(state, value.bloom_strength);
    mix(state, static_cast<core::u64>(value.taa_sequence_length));
    mix(state, static_cast<core::u64>(value.ddgi_probe_count_x));
    mix(state, static_cast<core::u64>(value.ddgi_probe_count_y));
    mix(state, static_cast<core::u64>(value.ddgi_probe_count_z));
    mix(state, static_cast<core::u64>(value.ddgi_rays_per_probe));
    mix(state, static_cast<core::u64>(value.slice_apron_limit));
    mix(state, static_cast<core::u64>(value.shadow_cascades));
    mix(state, value.gi_strength);
    mix(state, core::u64{0x4C54'4341'5245'0001ULL}); // LTC area emission, pinned GGX fit v1.
    mix(state, core::u64{0x4945'5353'554E'0001ULL}); // IES and sun-disc contract v1.
    mix(state, core::u64{0x434F'4C4F'5552'0001ULL}); // OCIO ACES 1.3, EXR and per-wall colour v1.
    // A node built before photometric conversion cannot satisfy this profile.
    mix(state, core::u64{0x5048'4F54'4F4D'3031ULL}); // "PHOTOM01"
    mix(state, value.photometric_scale);
    // Geometry policy defaults enable authored LOD companions. This version
    // tag is unconditional: an older build that cannot read those levels must
    // not satisfy readiness evidence for this renderer, even at defaults.
    mix(state, core::u64{0x4745'4F4D'4554'5259ULL});
    mix(state, static_cast<core::u64>(value.frustum_culling));
    mix(state, static_cast<core::u64>(value.geometry_instancing));
    mix(state, static_cast<core::u64>(value.model_lod));
    mix(state, core::u64{0x5445'584D'4950'3031ULL});
    mix(state, static_cast<core::u64>(value.texture_streaming));
    mix(state, static_cast<core::u64>(value.texture_budget_mib));
    if (value.frame_budget_us != 0U) {
        mix(state, 0x425544474554ULL);
        mix(state, static_cast<core::u64>(value.frame_budget_us));
    }
    mix(state, static_cast<core::u64>(value.texture_mip_bias));
    mix(state, core::u64{0x5341'4D50'4C45'0001ULL}); // Sampling contract v1.
    mix(state, core::u64{0x5445'4D50'4F52'0002ULL}); // Depth-aware temporal contract v2.
    mix(state, static_cast<core::u64>(value.texture_anisotropy));
    mix(state, value.texture_lod_bias);
    // Extended material maps, immutable packages and sequential filter support
    // change the renderer contract even for imported models at scene defaults.
    mix(state, core::u64{0x4D41'5445'5249'414CULL}); // "MATERIAL"
    // Prediction also drives tracked cameras when the plate switch is off.
    // Always cover its authored fields, including an inactive fit window.
    mix(state, core::u64{0x504F'5345'5052'4431ULL}); // "POSEPRD1"
    mix(state, static_cast<core::u64>(value.icvfx.pose_prediction));
    mix(state, static_cast<core::u64>(value.icvfx.prediction_window));
    mix(state, static_cast<core::u64>(value.icvfx.jitter_filter_percent));
    mix(state, static_cast<core::u64>(value.icvfx.latency_frames));
    // Plate defaults affect the render profile while plates are enabled.
    if (value.icvfx.inner_frustum) {
        mix(state, 0x4943'5646'5850'4F4CULL); // "ICVFXPOL"
        // The per-camera defaults are in the profile because a node that
        // gathered capability evidence under one plate density was answering
        // for a different picture than one gathered under another.
        mix(state, value.icvfx.camera_defaults);
    }
    // Colour joins the profile once anything about it is authored, for the
    // same reason as the inner frustum above: a node that gathered evidence
    // under one display transform answered for a different picture than one
    // under another - and until then the profile digest is unchanged.
    if (value.color != ColorSettings{}) {
        mix_color(state, value.color);
    }
    return state;
}

EffectSettings clamped(const EffectSettings& value) noexcept {
    EffectSettings result{};
    // An even tap count has no centre sample, so the kernel would be
    // asymmetric about the pixel it is filtering. Round up to the next odd.
    const core::u32 taps =
        std::clamp(value.blur_taps, min_blur_taps, max_blur_taps);
    result.blur_taps = (taps % 2U) == 0U ? taps + 1U : taps;
    result.blur_sigma_ratio =
        std::isfinite(value.blur_sigma_ratio)
            ? std::clamp(value.blur_sigma_ratio, min_blur_sigma_ratio,
                         max_blur_sigma_ratio)
            : EffectSettings{}.blur_sigma_ratio;
    result.blur_passes = std::clamp(value.blur_passes, 1U, max_blur_passes);
    const core::u32 directional_taps = std::clamp(
        value.directional_blur_taps, min_directional_blur_taps,
        max_directional_blur_taps);
    result.directional_blur_taps = (directional_taps & 1U) == 0U
        ? std::min(directional_taps + 1U, max_directional_blur_taps)
        : directional_taps;
    result.brightness_curve = value.brightness_curve == BrightnessCurve::Gain
        ? BrightnessCurve::Gain
        : BrightnessCurve::Exposure;
    result.brightness_gamma =
        std::isfinite(value.brightness_gamma)
            ? std::clamp(value.brightness_gamma, min_brightness_gamma,
                         max_brightness_gamma)
            : 1.0F;
    return result;
}

std::optional<BoxBlurParameters>
normalized_box_blur(const BoxBlurParameters& value) noexcept {
    if (value.radius_px > max_box_blur_radius_px || value.passes == 0U ||
        value.passes > max_box_blur_passes) {
        return std::nullopt;
    }
    return value;
}

bool box_blur_is_identity(const BoxBlurParameters& value) noexcept {
    const auto normalized = normalized_box_blur(value);
    return !normalized.has_value() || normalized->radius_px == 0U;
}

EffectBoundsExpansion
box_blur_bounds(const BoxBlurParameters& value) noexcept {
    const auto normalized = normalized_box_blur(value);
    if (!normalized.has_value() || normalized->radius_px == 0U) {
        return {};
    }
    const float extent = static_cast<float>(
        normalized->radius_px * normalized->passes);
    return EffectBoundsExpansion{extent, extent};
}

std::optional<DirectionalBlurParameters> normalized_directional_blur(
    const DirectionalBlurParameters& value) noexcept {
    if (!std::isfinite(value.length_px) || value.length_px < 0.0F ||
        value.length_px > max_directional_blur_length_px ||
        !std::isfinite(value.angle_turns)) {
        return std::nullopt;
    }
    return DirectionalBlurParameters{
        value.length_px, wrap_turns(value.angle_turns)};
}

bool directional_blur_is_identity(
    const DirectionalBlurParameters& value) noexcept {
    const auto normalized = normalized_directional_blur(value);
    return !normalized.has_value() || normalized->length_px <= 0.0F;
}

EffectBoundsExpansion directional_blur_bounds(
    const DirectionalBlurParameters& value) noexcept {
    const auto normalized = normalized_directional_blur(value);
    if (!normalized.has_value() || normalized->length_px <= 0.0F) {
        return {};
    }
    const float half_length = normalized->length_px * 0.5F;
    return EffectBoundsExpansion{
        std::abs(deterministic::cos_turns(normalized->angle_turns)) *
            half_length,
        std::abs(deterministic::sin_turns(normalized->angle_turns)) *
            half_length};
}

std::optional<ChromaKeyParameters> normalized_chroma_key(
    const ChromaKeyParameters& value) noexcept {
    const auto in_unit = [](float component) noexcept {
        return std::isfinite(component) && component >= 0.0F &&
            component <= 1.0F;
    };
    if (!in_unit(value.mix)) {
        return std::nullopt;
    }
    if (value.mix == 0.0F) {
        ChromaKeyParameters identity{};
        identity.mix = 0.0F;
        return identity;
    }
    if (!in_unit(value.key_color.x) || !in_unit(value.key_color.y) ||
        !in_unit(value.key_color.z) || !in_unit(value.tolerance) ||
        !in_unit(value.softness) ||
        value.morphology_px < -max_key_morphology_radius_px ||
        value.morphology_px > max_key_morphology_radius_px ||
        !in_unit(value.spill_suppression)) {
        return std::nullopt;
    }
    return value;
}

bool chroma_key_is_identity(const ChromaKeyParameters& value) noexcept {
    const auto normalized = normalized_chroma_key(value);
    return normalized.has_value() && normalized->mix == 0.0F;
}

std::optional<LumaKeyParameters> normalized_luma_key(
    const LumaKeyParameters& value) noexcept {
    const auto in_unit = [](float component) noexcept {
        return std::isfinite(component) && component >= 0.0F &&
            component <= 1.0F;
    };
    if (!in_unit(value.mix)) {
        return std::nullopt;
    }
    if (value.mix == 0.0F) {
        LumaKeyParameters identity{};
        identity.mix = 0.0F;
        return identity;
    }
    if (!in_unit(value.low_threshold) || !in_unit(value.high_threshold) ||
        value.low_threshold > value.high_threshold ||
        !in_unit(value.low_softness) || !in_unit(value.high_softness)) {
        return std::nullopt;
    }
    return value;
}

bool luma_key_is_identity(const LumaKeyParameters& value) noexcept {
    const auto normalized = normalized_luma_key(value);
    return normalized.has_value() && normalized->mix == 0.0F;
}

float normalized_saturation(float value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= max_saturation
        ? value
        : 1.0F;
}

std::optional<LevelsParameters>
normalized_levels(const LevelsParameters& value) noexcept {
    const auto normalize_channel = [](const LevelsChannelParameters& input,
                                      LevelsChannelParameters& output) noexcept {
        if (!std::isfinite(input.input_black)
            || !std::isfinite(input.input_white)
            || !std::isfinite(input.gamma)
            || !std::isfinite(input.output_black)
            || !std::isfinite(input.output_white)) {
            return false;
        }
        output.input_black = std::clamp(input.input_black, 0.0F, 1.0F);
        output.input_white = std::clamp(input.input_white, 0.0F, 1.0F);
        output.gamma =
            std::clamp(input.gamma, min_levels_gamma, max_levels_gamma);
        output.output_black = std::clamp(input.output_black, 0.0F, 1.0F);
        output.output_white = std::clamp(input.output_white, 0.0F, 1.0F);
        return output.input_black < output.input_white;
    };

    LevelsParameters result{};
    if (!normalize_channel(value.master, result.master)
        || !normalize_channel(value.red, result.red)
        || !normalize_channel(value.green, result.green)
        || !normalize_channel(value.blue, result.blue)) {
        return std::nullopt;
    }
    return result;
}

bool levels_is_identity(const LevelsParameters& value) noexcept {
    const auto normalized = normalized_levels(value);
    return normalized.has_value() && *normalized == LevelsParameters{};
}

std::optional<LiftGammaGainParameters>
normalized_lift_gamma_gain(const LiftGammaGainParameters& value) noexcept {
    const auto finite = [](Vec3 vector) noexcept {
        return std::isfinite(vector.x) && std::isfinite(vector.y)
            && std::isfinite(vector.z);
    };
    if (!finite(value.lift) || !finite(value.gamma) || !finite(value.gain)) {
        return std::nullopt;
    }
    const auto clamp_vector = [](Vec3 vector, float minimum,
                                 float maximum) noexcept {
        return Vec3{std::clamp(vector.x, minimum, maximum),
                    std::clamp(vector.y, minimum, maximum),
                    std::clamp(vector.z, minimum, maximum)};
    };
    return LiftGammaGainParameters{
        clamp_vector(value.lift, min_lift_gamma_gain_lift,
                     max_lift_gamma_gain_lift),
        clamp_vector(value.gamma, min_lift_gamma_gain_gamma,
                     max_lift_gamma_gain_gamma),
        clamp_vector(value.gain, min_lift_gamma_gain_gain,
                     max_lift_gamma_gain_gain)};
}

bool lift_gamma_gain_is_identity(
    const LiftGammaGainParameters& value) noexcept {
    const auto normalized = normalized_lift_gamma_gain(value);
    return normalized.has_value()
        && *normalized == LiftGammaGainParameters{};
}

std::optional<ContrastPivotParameters>
normalized_contrast_pivot(const ContrastPivotParameters& value) noexcept {
    if (!std::isfinite(value.contrast) || !std::isfinite(value.pivot)) {
        return std::nullopt;
    }
    return ContrastPivotParameters{
        std::clamp(value.contrast, min_contrast_pivot_contrast,
                   max_contrast_pivot_contrast),
        std::clamp(value.pivot, min_contrast_pivot_pivot,
                   max_contrast_pivot_pivot)};
}

bool contrast_pivot_is_identity(
    const ContrastPivotParameters& value) noexcept {
    const auto normalized = normalized_contrast_pivot(value);
    return normalized.has_value() && normalized->contrast == 1.0F;
}

std::optional<ColorMatrixParameters>
normalized_color_matrix(const ColorMatrixParameters& value) noexcept {
    ColorMatrixParameters result{};
    for (std::size_t index = 0U; index < value.matrix.size(); ++index) {
        if (!std::isfinite(value.matrix[index])) {
            return std::nullopt;
        }
        result.matrix[index] = std::clamp(
            value.matrix[index], min_color_matrix_coefficient,
            max_color_matrix_coefficient);
    }
    if (!std::isfinite(value.offset.x) || !std::isfinite(value.offset.y)
        || !std::isfinite(value.offset.z)) {
        return std::nullopt;
    }
    result.offset = Vec3{
        std::clamp(value.offset.x, min_color_matrix_coefficient,
                   max_color_matrix_coefficient),
        std::clamp(value.offset.y, min_color_matrix_coefficient,
                   max_color_matrix_coefficient),
        std::clamp(value.offset.z, min_color_matrix_coefficient,
                   max_color_matrix_coefficient)};
    return result;
}

bool color_matrix_is_identity(const ColorMatrixParameters& value) noexcept {
    const auto normalized = normalized_color_matrix(value);
    return normalized.has_value() && *normalized == ColorMatrixParameters{};
}

std::optional<WhiteBalanceParameters>
normalized_white_balance(const WhiteBalanceParameters& value) noexcept {
    if (!std::isfinite(value.temperature_kelvin)
        || !std::isfinite(value.tint)) {
        return std::nullopt;
    }
    WhiteBalanceParameters result{};
    result.temperature_kelvin = std::clamp(
        value.temperature_kelvin, min_white_balance_temperature_kelvin,
        max_white_balance_temperature_kelvin);
    result.tint = std::clamp(
        value.tint, min_white_balance_tint, max_white_balance_tint);
    if (result.temperature_kelvin ==
            d65_white_balance_temperature_kelvin
        && result.tint == 0.0F) {
        return result;
    }
    result.matrix = white_balance_matrix(
        static_cast<double>(result.temperature_kelvin),
        static_cast<double>(result.tint));
    return result;
}

bool white_balance_is_identity(
    const WhiteBalanceParameters& value) noexcept {
    const auto normalized = normalized_white_balance(value);
    return normalized.has_value()
        && normalized->temperature_kelvin ==
            d65_white_balance_temperature_kelvin
        && normalized->tint == 0.0F;
}

std::optional<CurvesParameters>
normalized_curves(const CurvesParameters& value) noexcept {
    if (!valid_curve_channel(value.master)
        || (value.red.has_value() && !valid_curve_channel(*value.red))
        || (value.green.has_value() && !valid_curve_channel(*value.green))
        || (value.blue.has_value() && !valid_curve_channel(*value.blue))) {
        return std::nullopt;
    }

    CurvesParameters result{};
    result.master = value.master;
    result.red = value.red;
    result.green = value.green;
    result.blue = value.blue;
    const auto master = sample_curve(result.master);
    const auto identity = sample_curve(CurveChannelParameters{});
    const auto red = result.red.has_value() ? sample_curve(*result.red)
                                            : identity;
    const auto green = result.green.has_value() ? sample_curve(*result.green)
                                                : identity;
    const auto blue = result.blue.has_value() ? sample_curve(*result.blue)
                                              : identity;
    result.lut.resize(curve_lut_entries);
    for (core::u32 index = 0U; index < curve_lut_entries; ++index) {
        result.lut[index] = Vec4{
            master[index], red[index], green[index], blue[index]};
    }
    return result;
}

bool curves_is_identity(const CurvesParameters& value) noexcept {
    if (!valid_curve_channel(value.master)
        || (value.red.has_value() && !valid_curve_channel(*value.red))
        || (value.green.has_value() && !valid_curve_channel(*value.green))
        || (value.blue.has_value() && !valid_curve_channel(*value.blue))) {
        return false;
    }
    const auto channel_identity = [](const CurveChannelParameters& channel) {
        return std::all_of(
            channel.points.begin(), channel.points.end(),
            [](CurveControlPoint point) noexcept { return point.x == point.y; });
    };
    return channel_identity(value.master)
        && (!value.red.has_value() || channel_identity(*value.red))
        && (!value.green.has_value() || channel_identity(*value.green))
        && (!value.blue.has_value() || channel_identity(*value.blue));
}

bool curves_lut_is_canonical(const CurvesParameters& value) noexcept {
    if (value.lut.size() != curve_lut_entries
        || !valid_curve_channel(value.master)
        || (value.red.has_value() && !valid_curve_channel(*value.red))
        || (value.green.has_value() && !valid_curve_channel(*value.green))
        || (value.blue.has_value() && !valid_curve_channel(*value.blue))) {
        return false;
    }
    const auto master = sample_curve(value.master);
    const auto identity = sample_curve(CurveChannelParameters{});
    const auto red = value.red.has_value() ? sample_curve(*value.red)
                                          : identity;
    const auto green = value.green.has_value() ? sample_curve(*value.green)
                                              : identity;
    const auto blue = value.blue.has_value() ? sample_curve(*value.blue)
                                            : identity;
    for (core::u32 index = 0U; index < curve_lut_entries; ++index) {
        if (value.lut[index]
            != Vec4{master[index], red[index], green[index], blue[index]}) {
            return false;
        }
    }
    return true;
}

std::optional<FilmGrainParameters>
normalized_film_grain(const FilmGrainParameters& value) noexcept {
    if (!std::isfinite(value.amount) || !std::isfinite(value.pixel_scale)
        || !std::isfinite(value.chroma_ratio)) {
        return std::nullopt;
    }
    switch (value.mode) {
    case FilmGrainMode::Monochrome:
    case FilmGrainMode::RGB:
        break;
    default:
        return std::nullopt;
    }
    return FilmGrainParameters{
        std::clamp(value.amount, min_film_grain_amount,
                   max_film_grain_amount),
        std::clamp(value.pixel_scale, min_film_grain_pixel_scale,
                   max_film_grain_pixel_scale),
        std::clamp(value.chroma_ratio, min_film_grain_chroma_ratio,
                   max_film_grain_chroma_ratio),
        value.seed,
        value.mode};
}

bool film_grain_is_identity(const FilmGrainParameters& value) noexcept {
    const auto normalized = normalized_film_grain(value);
    return normalized.has_value() && normalized->amount == 0.0F;
}

double composed_brightness_product(double left, double right) noexcept {
    return composed_saturation_product(left, right);
}

float brightness_from_product(double product, float response_gamma) noexcept {
    if (std::isnan(product) || product < 0.0) {
        product = 1.0;
    }
    const double bounded = std::isfinite(product)
        ? std::clamp(product, 0.0, 16.0)
        : 16.0;
    const float gamma = std::isfinite(response_gamma)
            && response_gamma >= min_brightness_gamma
            && response_gamma <= max_brightness_gamma
        ? response_gamma
        : 1.0F;
    if (gamma == 1.0F || bounded <= 0.0) {
        return static_cast<float>(bounded);
    }
    return static_cast<float>(std::clamp(
        std::pow(bounded, static_cast<double>(gamma)), 0.0, 16.0));
}

double composed_saturation_product(double left, double right) noexcept {
    const auto normalized_factor = [](double value) noexcept {
        return (std::isfinite(value) || value > 0.0) && value >= 0.0
            ? value
            : 1.0;
    };
    left = normalized_factor(left);
    right = normalized_factor(right);
    if (left == 0.0 || right == 0.0) {
        return 0.0;
    }
    return left * right;
}

float saturation_from_product(double product) noexcept {
    if (std::isnan(product) || product < 0.0) {
        return 1.0F;
    }
    if (!std::isfinite(product)) {
        return max_saturation;
    }
    return static_cast<float>(std::clamp(
        product, 0.0, static_cast<double>(max_saturation)));
}

bool clip_visible(const ClipTiming& timing, core::i64 frame) noexcept {
    if (!timing.placed()) {
        return true;
    }
    return frame >= timing.start && frame <= timing.last_frame();
}

bool clip_starts_within(const ClipTiming& timing, core::i64 frame,
                        core::i64 lead) noexcept {
    if (!timing.placed() || lead <= 0 || timing.start <= frame) {
        return false;
    }
    return timing.start - frame <= lead;
}

float clip_fade(const ClipTiming& timing, core::i64 frame) noexcept {
    if (!timing.placed()) {
        return 1.0F;
    }
    if (!clip_visible(timing, frame)) {
        return 0.0F;
    }
    // A head ramp of n frames is fully out on the clip's first frame and fully
    // in n frames later, so a one-frame ramp is the shortest visible dissolve
    // rather than a no-op. The tail is the mirror of it.
    const float head = ramp(frame - timing.start, timing.fade_in);
    const float tail = ramp(timing.last_frame() - frame, timing.fade_out);
    return std::min(head, tail);
}

Vec3 screen_extent(const Vec3& scale, core::u32 raster_width,
                   core::u32 raster_height, bool aspect_locked) noexcept {
    if (!aspect_locked || raster_width == 0U || raster_height == 0U) {
        return scale;
    }
    // Width drives height rather than the reverse, because width is what a
    // scale gizmo and a screen's physical span are both quoted in. Computed in
    // double and rounded once, so every node derives the same bits from the
    // same three integers.
    const auto aspect = static_cast<double>(raster_height)
        / static_cast<double>(raster_width);
    const auto height = static_cast<double>(scale.x) * aspect;
    if (!std::isfinite(height)) {
        return scale;
    }
    return Vec3{scale.x, static_cast<float>(height), scale.z};
}

} // namespace mediaserver::scene
