#include <mediaserver/scene/scene.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace mediaserver::scene {
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

void mix(core::u64& state, const Material& value) noexcept {
    mix(state, value.base_color);
    mix(state, value.roughness);
    mix(state, value.metallic);
    mix(state, value.emissive);
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
    mix(state, value.near_plane);
    mix(state, value.far_plane);
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

    mix(state, scene.id.value());
    mix(state, scene.show_frame);
    mix(state, static_cast<core::u64>(scene.canvas.width));
    mix(state, static_cast<core::u64>(scene.canvas.height));
    mix(state, scene.camera);
    mix(state, scene.environment.sky_color);
    mix(state, scene.environment.ground_color);
    mix(state, scene.environment.ambient);

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
        mix(state, static_cast<core::u64>(light.locked));
    }

    mix(state, static_cast<core::u64>(scene.point_lights.size()));
    for (const auto& light : scene.point_lights) {
        mix(state, light.position);
        mix(state, light.color);
        mix(state, light.intensity);
        mix(state, light.range);
        mix(state, light.source.value());
        mix(state, static_cast<core::u64>(light.locked));
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
