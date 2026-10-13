#include <mediaserver/scene/canvas.hpp>

#include <algorithm>
#include <cmath>

namespace mediaserver::scene {
namespace {

// A canvas larger than this is not a wall, it is a mistake in a document.
// Sixteen 4K outputs across is 61,440 pixels, so the bound is loose enough to
// be irrelevant in practice and tight enough that the pixel arithmetic below
// stays exact in binary32.
constexpr core::u64 max_canvas_extent = 65'535U;

[[nodiscard]] ViewMatrices identity_view() noexcept {
    return ViewMatrices{identity(), identity(), identity()};
}

[[nodiscard]] bool valid_camera_body(const Camera& camera) noexcept {
    if (!std::isfinite(camera.near_plane) || !std::isfinite(camera.far_plane)) {
        return false;
    }
    if (!std::isfinite(camera.aperture_f_number)
        || !std::isfinite(camera.shutter_seconds)
        || !std::isfinite(camera.iso)
        || !std::isfinite(camera.exposure_compensation_ev)
        || !(camera.aperture_f_number > 0.0F)
        || !(camera.shutter_seconds > 0.0F) || !(camera.iso > 0.0F)) {
        return false;
    }
    if (!(camera.near_plane > 0.0F) || !(camera.far_plane > camera.near_plane)) {
        return false;
    }
    return length(camera.target - camera.eye) > 0.0F;
}

[[nodiscard]] bool valid_window(const FrustumWindow& value) noexcept {
    return std::isfinite(value.left) && std::isfinite(value.right)
        && std::isfinite(value.bottom) && std::isfinite(value.top)
        && std::isfinite(value.near_plane) && std::isfinite(value.far_plane)
        && value.right > value.left && value.top > value.bottom
        && value.near_plane > 0.0F && value.far_plane > value.near_plane;
}

[[nodiscard]] FrustumWindow slice_of_window(const FrustumWindow& full,
                                             Canvas canvas,
                                             CanvasRect slice) noexcept {
    if (!valid_window(full) || !contains(canvas, slice)) {
        return FrustumWindow{};
    }
    const auto canvas_width = static_cast<float>(canvas.width);
    const auto canvas_height = static_cast<float>(canvas.height);
    const auto span_x = full.right - full.left;
    const auto span_y = full.top - full.bottom;
    const auto edge = [](core::u32 origin, core::u32 extent) noexcept {
        return static_cast<float>(
            static_cast<core::u64>(origin) + static_cast<core::u64>(extent));
    };
    return FrustumWindow{
        full.left + span_x * (static_cast<float>(slice.x) / canvas_width),
        full.left + span_x * (edge(slice.x, slice.width) / canvas_width),
        full.top - span_y * (edge(slice.y, slice.height) / canvas_height),
        full.top - span_y * (static_cast<float>(slice.y) / canvas_height),
        full.near_plane,
        full.far_plane,
    };
}

[[nodiscard]] float bounded(float value, float low, float high,
                            float fallback) noexcept {
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}

[[nodiscard]] BrownConradyDistortion bounded(
    const BrownConradyDistortion& value) noexcept {
    const BrownConradyDistortion defaults{};
    return BrownConradyDistortion{
        bounded(value.k1, -max_lens_distortion_coefficient,
                max_lens_distortion_coefficient, defaults.k1),
        bounded(value.k2, -max_lens_distortion_coefficient,
                max_lens_distortion_coefficient, defaults.k2),
        bounded(value.k3, -max_lens_distortion_coefficient,
                max_lens_distortion_coefficient, defaults.k3),
        bounded(value.p1, -max_lens_distortion_coefficient,
                max_lens_distortion_coefficient, defaults.p1),
        bounded(value.p2, -max_lens_distortion_coefficient,
                max_lens_distortion_coefficient, defaults.p2),
    };
}

// The one place a projection matrix is built. Both the per-slice canvas view
// and the explicit-aspect view go through it, so an off-axis slice and a
// tracked camera cannot end up using two subtly different constructions.
[[nodiscard]] ViewMatrices view_from_window(
    const Camera& camera, const FrustumWindow& frustum) noexcept {
    const auto width = frustum.right - frustum.left;
    const auto height = frustum.top - frustum.bottom;
    const auto depth_span = frustum.far_plane - frustum.near_plane;
    if (!(width > 0.0F) || !(height > 0.0F) || !(depth_span > 0.0F)) {
        return identity_view();
    }

    const auto twice_near = frustum.near_plane + frustum.near_plane;

    Mat4 projection{};
    projection.set(0U, 0U, twice_near / width);
    projection.set(0U, 2U, (frustum.right + frustum.left) / width);
    projection.set(1U, 1U, twice_near / height);
    projection.set(1U, 2U, (frustum.top + frustum.bottom) / height);
    // Reversed depth: near maps to 1, far to 0. See the header.
    projection.set(2U, 2U, frustum.near_plane / depth_span);
    projection.set(2U, 3U, (frustum.near_plane * frustum.far_plane) / depth_span);
    projection.set(3U, 2U, -1.0F);

    const auto view = look_at(camera.eye, camera.target, camera.up);
    return ViewMatrices{view, projection, multiply(projection, view)};
}

// The perspective divide and the map from clip space back into canvas pixels.
// Shared so the whole-canvas path and the per-slice path cannot drift.
[[nodiscard]] CanvasProjection project(
    const Mat4& view_projection,
    CanvasRect rect,
    Vec3 world_point) noexcept {
    const auto clip = transform(
        view_projection, Vec4{world_point.x, world_point.y, world_point.z, 1.0F});
    const auto ndc_x = clip.w != 0.0F ? clip.x / clip.w : 0.0F;
    const auto ndc_y = clip.w != 0.0F ? clip.y / clip.w : 0.0F;
    const auto depth = clip.w != 0.0F ? clip.z / clip.w : 0.0F;

    const auto x = static_cast<float>(rect.x)
        + (ndc_x * 0.5F + 0.5F) * static_cast<float>(rect.width);
    const auto y = static_cast<float>(rect.y)
        + (0.5F - ndc_y * 0.5F) * static_cast<float>(rect.height);
    return CanvasProjection{x, y, depth, clip.w > 0.0F};
}

} // namespace

void place_camera(Camera& camera, Vec3 position, Turns yaw, Turns pitch,
                  Turns roll) noexcept {
    const Mat4 turned = rotation(yaw, pitch, roll);
    // The rotated -Z and +Y axes: the columns of the rotation.
    const Vec3 forward{-turned.at(0U, 2U), -turned.at(1U, 2U),
                       -turned.at(2U, 2U)};
    camera.eye = position;
    camera.target = position + forward * camera_aim_distance_m;
    camera.up = Vec3{turned.at(0U, 1U), turned.at(1U, 1U), turned.at(2U, 1U)};
}

Vec3 camera_rotation(const Camera& camera) noexcept {
    constexpr double turn = 6.283185307179586;
    const Vec3 forward = normalize(camera.target - camera.eye, Vec3{});
    if (length(forward) == 0.0F) {
        return Vec3{};
    }
    // rotation() sends -Z to (-sin(yaw)cos(pitch), sin(pitch),
    // -cos(yaw)cos(pitch)).
    const double pitch =
        std::asin(std::clamp(static_cast<double>(forward.y), -1.0, 1.0));
    const double yaw = std::atan2(-static_cast<double>(forward.x),
                                  -static_cast<double>(forward.z));
    // Roll is the turn of the authored up about the view direction, measured
    // against the up that yaw and pitch alone would give.
    const Mat4 unrolled = rotation(static_cast<float>(yaw / turn),
                                   static_cast<float>(pitch / turn), 0.0F);
    const Vec3 right{unrolled.at(0U, 0U), unrolled.at(1U, 0U),
                     unrolled.at(2U, 0U)};
    const Vec3 level_up{unrolled.at(0U, 1U), unrolled.at(1U, 1U),
                        unrolled.at(2U, 1U)};
    const Vec3 up = camera.up - forward * dot(camera.up, forward);
    const double roll =
        length(up) > 0.0F ? std::atan2(static_cast<double>(dot(up, right)),
                                       static_cast<double>(dot(up, level_up)))
                          : 0.0;
    return Vec3{static_cast<float>(yaw / turn), static_cast<float>(pitch / turn),
                static_cast<float>(roll / turn)};
}

bool valid(Canvas canvas) noexcept {
    return canvas.width != 0U && canvas.height != 0U
        && static_cast<core::u64>(canvas.width) <= max_canvas_extent
        && static_cast<core::u64>(canvas.height) <= max_canvas_extent;
}

bool contains(Canvas canvas, CanvasRect slice) noexcept {
    if (!valid(canvas) || slice.width == 0U || slice.height == 0U) {
        return false;
    }
    const auto right = static_cast<core::u64>(slice.x)
        + static_cast<core::u64>(slice.width);
    const auto bottom = static_cast<core::u64>(slice.y)
        + static_cast<core::u64>(slice.height);
    return right <= static_cast<core::u64>(canvas.width)
        && bottom <= static_cast<core::u64>(canvas.height);
}

bool valid(const Camera& camera) noexcept {
    if (!valid_camera_body(camera)) {
        return false;
    }
    if (camera.lens_profile.has_value() && !valid(*camera.lens_profile)) {
        return false;
    }
    // Zero is what the deterministic tangent reports for a field of view at or
    // past half a turn, so this one check covers both the degenerate and the
    // out-of-range camera.
    if (!(deterministic::tan_half_turns(camera.field_of_view) > 0.0F)) {
        return false;
    }
    return true;
}

bool valid(const LensProfile& value) noexcept {
    if (!std::isfinite(value.sensor_width_mm)
        || !std::isfinite(value.sensor_height_mm)
        || !std::isfinite(value.focal_length_mm)
        || !std::isfinite(value.entrance_pupil_offset_m)
        || value.sensor_width_mm < min_lens_sensor_extent_mm
        || value.sensor_width_mm > max_lens_sensor_extent_mm
        || value.sensor_height_mm < min_lens_sensor_extent_mm
        || value.sensor_height_mm > max_lens_sensor_extent_mm
        || value.focal_length_mm < min_lens_focal_length_mm
        || value.focal_length_mm > max_lens_focal_length_mm
        || std::abs(value.entrance_pupil_offset_m) > max_lens_pupil_offset_m
        || value.calibration_count > max_lens_calibration_points) {
        return false;
    }
    const auto valid_distortion = [](const BrownConradyDistortion& value) {
        return std::isfinite(value.k1) && std::isfinite(value.k2)
            && std::isfinite(value.k3) && std::isfinite(value.p1)
            && std::isfinite(value.p2)
            && std::abs(value.k1) <= max_lens_distortion_coefficient
            && std::abs(value.k2) <= max_lens_distortion_coefficient
            && std::abs(value.k3) <= max_lens_distortion_coefficient
            && std::abs(value.p1) <= max_lens_distortion_coefficient
            && std::abs(value.p2) <= max_lens_distortion_coefficient;
    };
    if (!valid_distortion(value.distortion)) {
        return false;
    }
    for (core::u32 index = 0U; index < value.calibration_count; ++index) {
        const LensCalibrationPoint& point = value.calibration[index];
        if (!std::isfinite(point.zoom_encoder)
            || !std::isfinite(point.focus_encoder)
            || !std::isfinite(point.focal_length_mm)
            || !std::isfinite(point.entrance_pupil_offset_m)
            || point.zoom_encoder < 0.0F
            || point.zoom_encoder > max_lens_encoder_value
            || point.focus_encoder < 0.0F
            || point.focus_encoder > max_lens_encoder_value
            || point.focal_length_mm < min_lens_focal_length_mm
            || point.focal_length_mm > max_lens_focal_length_mm
            || std::abs(point.entrance_pupil_offset_m)
                > max_lens_pupil_offset_m
            || !valid_distortion(point.distortion)) {
            return false;
        }
    }
    return true;
}

bool valid(const LensEncoders& value) noexcept {
    const auto valid_component = [](float component) noexcept {
        return std::isfinite(component) && component >= 0.0F
            && component <= max_lens_encoder_value;
    };
    return valid_component(value.zoom) && valid_component(value.focus)
        && valid_component(value.iris);
}

LensProfile clamped(const LensProfile& value) noexcept {
    LensProfile result{};
    result.sensor_width_mm = bounded(value.sensor_width_mm,
                                     min_lens_sensor_extent_mm,
                                     max_lens_sensor_extent_mm,
                                     LensProfile{}.sensor_width_mm);
    result.sensor_height_mm = bounded(value.sensor_height_mm,
                                      min_lens_sensor_extent_mm,
                                      max_lens_sensor_extent_mm,
                                      LensProfile{}.sensor_height_mm);
    result.focal_length_mm = bounded(value.focal_length_mm,
                                     min_lens_focal_length_mm,
                                     max_lens_focal_length_mm,
                                     LensProfile{}.focal_length_mm);
    result.entrance_pupil_offset_m = bounded(
        value.entrance_pupil_offset_m, -max_lens_pupil_offset_m,
        max_lens_pupil_offset_m, LensProfile{}.entrance_pupil_offset_m);
    result.distortion = bounded(value.distortion);
    result.calibration_count = std::min(value.calibration_count,
                                        max_lens_calibration_points);
    for (core::u32 index = 0U; index < result.calibration_count; ++index) {
        const LensCalibrationPoint& source = value.calibration[index];
        LensCalibrationPoint& point = result.calibration[index];
        point.zoom_encoder = bounded(source.zoom_encoder, 0.0F,
                                     max_lens_encoder_value, 0.0F);
        point.focus_encoder = bounded(source.focus_encoder, 0.0F,
                                      max_lens_encoder_value, 0.0F);
        point.focal_length_mm = bounded(source.focal_length_mm,
                                        min_lens_focal_length_mm,
                                        max_lens_focal_length_mm,
                                        result.focal_length_mm);
        point.entrance_pupil_offset_m = bounded(
            source.entrance_pupil_offset_m, -max_lens_pupil_offset_m,
            max_lens_pupil_offset_m, result.entrance_pupil_offset_m);
        point.distortion = bounded(source.distortion);
    }
    return result;
}

LensEncoders clamped(const LensEncoders& value) noexcept {
    LensEncoders result{};
    const auto component = [](float source, bool supplied, float& target,
                              bool& valid_flag) noexcept {
        valid_flag = supplied && std::isfinite(source) && source >= 0.0F
            && source <= max_lens_encoder_value;
        target = valid_flag ? source : 0.0F;
    };
    component(value.zoom, value.zoom_valid, result.zoom, result.zoom_valid);
    component(value.focus, value.focus_valid, result.focus,
              result.focus_valid);
    component(value.iris, value.iris_valid, result.iris, result.iris_valid);
    return result;
}

EvaluatedLens evaluate_lens(const LensProfile& profile,
                            const LensEncoders& encoders) noexcept {
    const LensProfile bounded_profile = clamped(profile);
    const LensEncoders bounded_encoders = clamped(encoders);
    EvaluatedLens result{
        bounded_profile.sensor_width_mm,
        bounded_profile.sensor_height_mm,
        bounded_profile.focal_length_mm,
        bounded_profile.entrance_pupil_offset_m,
        bounded_profile.distortion,
        false,
    };
    if (bounded_profile.calibration_count == 0U
        || !bounded_encoders.zoom_valid || !bounded_encoders.focus_valid) {
        return result;
    }

    const auto nearest_axis = [](const LensProfile& value, float encoder,
                                 bool zoom, bool lower, float& result_axis)
        noexcept {
        bool found = false;
        for (core::u32 index = 0U; index < value.calibration_count; ++index) {
            const LensCalibrationPoint& point = value.calibration[index];
            const float candidate = zoom ? point.zoom_encoder
                                         : point.focus_encoder;
            const bool eligible = lower ? candidate <= encoder
                                        : candidate >= encoder;
            if (!eligible || (found && (lower ? candidate <= result_axis
                                               : candidate >= result_axis))) {
                continue;
            }
            result_axis = candidate;
            found = true;
        }
        if (found) {
            return true;
        }
        // A sample outside the calibrated range holds the nearest measured
        // edge; it never extrapolates a physical lens curve.
        for (core::u32 index = 0U; index < value.calibration_count; ++index) {
            const LensCalibrationPoint& point = value.calibration[index];
            const float candidate = zoom ? point.zoom_encoder
                                         : point.focus_encoder;
            if (!found || (lower ? candidate < result_axis
                                 : candidate > result_axis)) {
                result_axis = candidate;
                found = true;
            }
        }
        return found;
    };

    float zoom_low{};
    float zoom_high{};
    float focus_low{};
    float focus_high{};
    if (!nearest_axis(bounded_profile, bounded_encoders.zoom, true, true,
                      zoom_low)
        || !nearest_axis(bounded_profile, bounded_encoders.zoom, true, false,
                         zoom_high)
        || !nearest_axis(bounded_profile, bounded_encoders.focus, false, true,
                         focus_low)
        || !nearest_axis(bounded_profile, bounded_encoders.focus, false, false,
                         focus_high)) {
        return result;
    }

    const auto at = [&bounded_profile](float zoom, float focus)
        -> const LensCalibrationPoint* {
        for (core::u32 index = 0U;
             index < bounded_profile.calibration_count; ++index) {
            const LensCalibrationPoint& point =
                bounded_profile.calibration[index];
            if (point.zoom_encoder == zoom && point.focus_encoder == focus) {
                return &point;
            }
        }
        return nullptr;
    };
    const LensCalibrationPoint* const low_low = at(zoom_low, focus_low);
    const LensCalibrationPoint* const high_low = at(zoom_high, focus_low);
    const LensCalibrationPoint* const low_high = at(zoom_low, focus_high);
    const LensCalibrationPoint* const high_high = at(zoom_high, focus_high);

    if (low_low != nullptr && high_low != nullptr && low_high != nullptr
        && high_high != nullptr) {
        const float tx = zoom_high > zoom_low
            ? (bounded_encoders.zoom - zoom_low) / (zoom_high - zoom_low)
            : 0.0F;
        const float ty = focus_high > focus_low
            ? (bounded_encoders.focus - focus_low) / (focus_high - focus_low)
            : 0.0F;
        const auto bilinear = [tx, ty](float a, float b, float c,
                                       float d) noexcept {
            const float lower = a + (b - a) * tx;
            const float upper = c + (d - c) * tx;
            return lower + (upper - lower) * ty;
        };
        result.focal_length_mm = bilinear(low_low->focal_length_mm,
                                          high_low->focal_length_mm,
                                          low_high->focal_length_mm,
                                          high_high->focal_length_mm);
        result.entrance_pupil_offset_m = bilinear(
            low_low->entrance_pupil_offset_m,
            high_low->entrance_pupil_offset_m,
            low_high->entrance_pupil_offset_m,
            high_high->entrance_pupil_offset_m);
        result.distortion = BrownConradyDistortion{
            bilinear(low_low->distortion.k1, high_low->distortion.k1,
                     low_high->distortion.k1, high_high->distortion.k1),
            bilinear(low_low->distortion.k2, high_low->distortion.k2,
                     low_high->distortion.k2, high_high->distortion.k2),
            bilinear(low_low->distortion.k3, high_low->distortion.k3,
                     low_high->distortion.k3, high_high->distortion.k3),
            bilinear(low_low->distortion.p1, high_low->distortion.p1,
                     low_high->distortion.p1, high_high->distortion.p1),
            bilinear(low_low->distortion.p2, high_low->distortion.p2,
                     low_high->distortion.p2, high_high->distortion.p2),
        };
        result.calibrated = true;
        return result;
    }

    // A sparse imported table is still useful. Pick its nearest measured point
    // with a fixed index tie-break rather than fabricating an extrapolation.
    const LensCalibrationPoint* nearest = nullptr;
    float nearest_distance{};
    for (core::u32 index = 0U; index < bounded_profile.calibration_count;
         ++index) {
        const LensCalibrationPoint& point = bounded_profile.calibration[index];
        const float dx = point.zoom_encoder - bounded_encoders.zoom;
        const float dy = point.focus_encoder - bounded_encoders.focus;
        const float distance = dx * dx + dy * dy;
        if (nearest == nullptr || distance < nearest_distance) {
            nearest = &point;
            nearest_distance = distance;
        }
    }
    if (nearest != nullptr) {
        result.focal_length_mm = nearest->focal_length_mm;
        result.entrance_pupil_offset_m = nearest->entrance_pupil_offset_m;
        result.distortion = nearest->distortion;
        result.calibrated = true;
    }
    return result;
}

LensCoordinate distort_brown_conrady(
    LensCoordinate undistorted,
    const BrownConradyDistortion& distortion) noexcept {
    if (!std::isfinite(undistorted.x) || !std::isfinite(undistorted.y)) {
        return LensCoordinate{};
    }
    const BrownConradyDistortion bounded_distortion = bounded(distortion);
    const float x = undistorted.x;
    const float y = undistorted.y;
    const float x2 = x * x;
    const float y2 = y * y;
    const float r2 = x2 + y2;
    const float r4 = r2 * r2;
    const float r6 = r4 * r2;
    const float radial = 1.0F + bounded_distortion.k1 * r2
        + bounded_distortion.k2 * r4 + bounded_distortion.k3 * r6;
    return LensCoordinate{
        x * radial + 2.0F * bounded_distortion.p1 * x * y
            + bounded_distortion.p2 * (r2 + 2.0F * x2),
        y * radial + bounded_distortion.p1 * (r2 + 2.0F * y2)
            + 2.0F * bounded_distortion.p2 * x * y,
    };
}

DistortedLensExtent distorted_lens_extent(
    const BrownConradyDistortion& distortion) noexcept {
    DistortedLensExtent result{};
    constexpr core::u32 grid = 8U;
    bool any = false;
    for (core::u32 y = 0U; y <= grid; ++y) {
        for (core::u32 x = 0U; x <= grid; ++x) {
            const LensCoordinate sample = distort_brown_conrady(
                LensCoordinate{
                    -1.0F + 2.0F * static_cast<float>(x)
                        / static_cast<float>(grid),
                    -1.0F + 2.0F * static_cast<float>(y)
                        / static_cast<float>(grid)},
                distortion);
            if (!std::isfinite(sample.x) || !std::isfinite(sample.y)) {
                return result;
            }
            if (!any) {
                result.left = result.right = sample.x;
                result.bottom = result.top = sample.y;
                any = true;
            } else {
                result.left = std::min(result.left, sample.x);
                result.right = std::max(result.right, sample.x);
                result.bottom = std::min(result.bottom, sample.y);
                result.top = std::max(result.top, sample.y);
            }
        }
    }
    // A sampled grid catches the normal calibration shape closely, but it
    // cannot prove that a high-order curve has no extremum between samples.
    // Coverage must fail outward, never inward, so also fold in a closed-form
    // bound over [-1, 1]^2. r^2 is at most two, r^4 at most four, and r^6 at
    // most eight; the tangential terms have the corresponding bounds below.
    // This can over-render for a pathological imported profile, which is the
    // safe trade for never exposing an outer-frustum hole at the camera edge.
    const BrownConradyDistortion bounded_distortion = bounded(distortion);
    const float radial_bound = 1.0F
        + 2.0F * std::abs(bounded_distortion.k1)
        + 4.0F * std::abs(bounded_distortion.k2)
        + 8.0F * std::abs(bounded_distortion.k3);
    const float horizontal_bound = radial_bound
        + 2.0F * std::abs(bounded_distortion.p1)
        + 4.0F * std::abs(bounded_distortion.p2);
    const float vertical_bound = radial_bound
        + 4.0F * std::abs(bounded_distortion.p1)
        + 2.0F * std::abs(bounded_distortion.p2);
    result.left = std::min(result.left, -horizontal_bound);
    result.right = std::max(result.right, horizontal_bound);
    result.bottom = std::min(result.bottom, -vertical_bound);
    result.top = std::max(result.top, vertical_bound);
    result.valid = any && result.right > result.left && result.top > result.bottom;
    return result;
}

float camera_exposure_multiplier(const Camera& camera) noexcept {
    if (!std::isfinite(camera.aperture_f_number)
        || !std::isfinite(camera.shutter_seconds)
        || !std::isfinite(camera.iso)
        || !std::isfinite(camera.exposure_compensation_ev)
        || !(camera.aperture_f_number > 0.0F)
        || !(camera.shutter_seconds > 0.0F) || !(camera.iso > 0.0F)) {
        return 1.0F;
    }

    constexpr float neutral_aperture = 2.8F;
    constexpr float neutral_shutter = 1.0F / 48.0F;
    constexpr float neutral_iso = 100.0F;
    constexpr double minimum = 0x1p-24;
    constexpr double maximum = 0x1p24;

    const double aperture_ratio = static_cast<double>(neutral_aperture)
        / static_cast<double>(camera.aperture_f_number);
    double exposure = aperture_ratio * aperture_ratio;
    exposure *= static_cast<double>(camera.shutter_seconds)
        / static_cast<double>(neutral_shutter);
    exposure *= static_cast<double>(camera.iso)
        / static_cast<double>(neutral_iso);
    exposure *= static_cast<double>(
        deterministic::stop_multiplier(camera.exposure_compensation_ev));
    return static_cast<float>(std::clamp(exposure, minimum, maximum));
}

CanvasRect whole(Canvas canvas) noexcept {
    return CanvasRect{0U, 0U, canvas.width, canvas.height};
}

float halton(core::u64 index, core::u32 base) noexcept {
    if (base < 2U) {
        return 0.0F;
    }
    const auto radix = static_cast<float>(base);
    float result = 0.0F;
    float fraction = 1.0F;
    core::u64 remaining = index;
    while (remaining > 0U) {
        fraction /= radix;
        result += fraction * static_cast<float>(remaining % base);
        remaining /= base;
    }
    return result;
}

SubpixelJitter jitter_for_frame(
    core::u64 show_frame, core::u32 sequence_length) noexcept {
    const core::u64 length = sequence_length == 0U
        ? 1U
        : static_cast<core::u64>(sequence_length);
    // Offset by one so index zero is Halton's first sample rather than its
    // degenerate zero, which would place the first frame of every sequence at
    // the corner of the pixel instead of inside it.
    const core::u64 index = (show_frame % length) + 1U;
    // Snapped to a 1/256 grid, and that is a determinism requirement rather
    // than a tidiness one.
    //
    // The offset is added to a pixel index before being divided by the canvas
    // extent. Halton in base two lands on exact binary fractions and survives
    // that addition unchanged; base three lands on thirds, which do not, so
    // `x + jitter` rounds differently for one node's pixel index than for
    // another's and the two disagree on where the sample sits. On a 1/256 grid
    // the addition is exact for any canvas a wall will ever have, and every
    // node's arithmetic is the same arithmetic.
    //
    // A 1/256 grid is far finer than the sub-pixel structure this is chasing -
    // the sequence exists to move samples across a pixel, not across a
    // two-hundred-and-fifty-sixth of one.
    const auto snapped = [](float value) noexcept {
        // Truncation toward zero is exactly specified, so this rounds the same
        // way everywhere without reaching for a library rounding mode.
        const float scaled = value * 256.0F;
        const auto rounded = static_cast<core::i32>(
            scaled < 0.0F ? scaled - 0.5F : scaled + 0.5F);
        return static_cast<float>(rounded) / 256.0F;
    };
    return SubpixelJitter{snapped(halton(index, 2U) - 0.5F),
                          snapped(halton(index, 3U) - 0.5F)};
}

FrustumWindow window_for_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice) noexcept {
    if (!valid(camera) || !contains(canvas, slice)) {
        return FrustumWindow{};
    }

    const auto near_plane = camera.near_plane;
    const auto canvas_width = static_cast<float>(canvas.width);
    const auto canvas_height = static_cast<float>(canvas.height);

    // The full near-plane window the camera sees, before the slice narrows it.
    const auto top = near_plane
        * deterministic::tan_half_turns(camera.field_of_view);
    const auto bottom = -top;
    const auto right = top * (canvas_width / canvas_height);
    const auto left = -right;
    const auto span_x = right - left;
    const auto span_y = top - bottom;

    // The slice's window, as fractions of the canvas.
    //
    // Both edges of a shared boundary are the same expression over the same
    // integers - node A's `x + width` is node B's `x` - so two adjacent slices
    // compute a bit-identical plane and the seam cannot open. This is the one
    // place in the projection where that matters, and it is why the fractions
    // are formed from the pixel counts rather than accumulated per slice.
    const auto edge = [](core::u32 origin, core::u32 extent) noexcept {
        return static_cast<float>(
            static_cast<core::u64>(origin) + static_cast<core::u64>(extent));
    };

    return FrustumWindow{
        left + span_x * (static_cast<float>(slice.x) / canvas_width),
        left + span_x * (edge(slice.x, slice.width) / canvas_width),
        top - span_y * (edge(slice.y, slice.height) / canvas_height),
        top - span_y * (static_cast<float>(slice.y) / canvas_height),
        near_plane,
        camera.far_plane,
    };
}

ViewMatrices view_for_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice) noexcept {
    return view_from_window(camera, window_for_slice(camera, canvas, slice));
}

FrustumWindow window_for_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice,
    SubpixelJitter jitter) noexcept {
    const FrustumWindow window = window_for_slice(camera, canvas, slice);
    if (window == FrustumWindow{}) {
        return window;
    }
    // The jitter goes into the numerator of the same canvas fraction the
    // unjittered edges are formed from, not onto the edges afterwards.
    //
    // Both forms shift the window by the same distance in exact arithmetic.
    // They do not round the same way: an edge built as `left + span * (n / W)`
    // and then displaced is a different sequence of operations from one built
    // as `left + span * ((n + j) / W)`, and `view_from_window` divides by
    // `right - left` immediately afterwards, so the difference lands in the
    // projection. Two nodes then disagree on where a jittered sample sits by a
    // fraction of a pixel, and the temporal history integrates that
    // disagreement rather than averaging it away. Measured: three pixels of a
    // 128x96 canvas on the very first frame, spreading to fifty-five by the
    // eighth.
    //
    // The seam property survives because the numerator is still the same
    // expression over the same integers on both sides of a shared boundary -
    // node A's `x + width + jitter` is node B's `x + jitter`.
    const auto canvas_width = static_cast<float>(canvas.width);
    const auto canvas_height = static_cast<float>(canvas.height);
    const auto top = camera.near_plane
        * deterministic::tan_half_turns(camera.field_of_view);
    const auto bottom = -top;
    const auto right = top * (canvas_width / canvas_height);
    const auto left = -right;
    const auto span_x = right - left;
    const auto span_y = top - bottom;
    const auto edge = [](core::u32 origin, core::u32 extent) noexcept {
        return static_cast<float>(
            static_cast<core::u64>(origin) + static_cast<core::u64>(extent));
    };
    return FrustumWindow{
        left + span_x * ((static_cast<float>(slice.x) + jitter.x)
                         / canvas_width),
        left + span_x * ((edge(slice.x, slice.width) + jitter.x)
                         / canvas_width),
        top - span_y * ((edge(slice.y, slice.height) - jitter.y)
                        / canvas_height),
        top - span_y * ((static_cast<float>(slice.y) - jitter.y)
                        / canvas_height),
        window.near_plane,
        window.far_plane,
    };
}

ViewMatrices view_for_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice,
    SubpixelJitter jitter) noexcept {
    return view_from_window(camera,
                            window_for_slice(camera, canvas, slice, jitter));
}

ViewMatrices spot_light_view(
    Vec3 position,
    Vec3 direction,
    Turns outer_cone,
    float near_plane,
    float far_plane) noexcept {
    if (!(near_plane > 0.0F) || !(far_plane > near_plane)
        || !(outer_cone > 0.0F) || !(outer_cone < 0.25F)) {
        return identity_view();
    }
    const Vec3 axis = normalize(direction, Vec3{0.0F, -1.0F, 0.0F});
    // An up vector that cannot be parallel to the axis. Chosen by a threshold
    // on the axis rather than by a cross product magnitude, so the choice
    // flips at the same place on every node instead of wherever rounding puts
    // it - a light pointing straight down must not get one basis here and a
    // different one next door.
    const Vec3 reference = axis.y > 0.9F || axis.y < -0.9F
        ? Vec3{0.0F, 0.0F, 1.0F}
        : Vec3{0.0F, 1.0F, 0.0F};
    // `outer_cone` is the half-angle, and `tan_half_turns` halves what it is
    // given, so the full cone angle goes in and the half-extent comes out.
    const auto half_extent =
        near_plane * deterministic::tan_half_turns(outer_cone + outer_cone);
    const FrustumWindow window{-half_extent, half_extent, -half_extent,
                               half_extent, near_plane, far_plane};
    Camera camera{};
    camera.eye = position;
    camera.target = position + axis;
    camera.up = reference;
    camera.near_plane = near_plane;
    camera.far_plane = far_plane;
    return view_from_window(camera, window);
}

ViewMatrices jittered(
    const ViewMatrices& matrices,
    SubpixelJitter jitter,
    core::u32 width,
    core::u32 height) noexcept {
    if (width == 0U || height == 0U) {
        return matrices;
    }
    // The projection's asymmetry terms. Shifting them is the same operation
    // the window form performs on `left`/`right` and `bottom`/`top`: the
    // frustum slides without changing shape.
    //
    // Both axes add. Moving both edges of an axis by `d` moves the asymmetry
    // term by `2d/span`, and one target pixel is `span/extent` wide, so the
    // whole conversion is two over the extent - with no negation anywhere,
    // because this mirrors the window rather than the screen. The equivalence
    // is asserted against the window form in the scene math suite; a sign
    // guessed from device-space handedness here was wrong on y.
    ViewMatrices result = matrices;
    const auto offset_x = jitter.x * 2.0F / static_cast<float>(width);
    const auto offset_y = jitter.y * 2.0F / static_cast<float>(height);
    result.projection.set(0U, 2U, matrices.projection.at(0U, 2U) + offset_x);
    result.projection.set(1U, 2U, matrices.projection.at(1U, 2U) + offset_y);
    result.view_projection = multiply(result.projection, result.view);
    return result;
}

FrustumWindow window_for_lens(const Camera& camera,
                              const LensProfile& profile,
                              const LensEncoders& encoders) noexcept {
    if (!valid_camera_body(camera)) {
        return FrustumWindow{};
    }
    const EvaluatedLens lens = evaluate_lens(profile, encoders);
    if (!(lens.sensor_width_mm > 0.0F) || !(lens.sensor_height_mm > 0.0F)
        || !(lens.focal_length_mm > 0.0F)) {
        return FrustumWindow{};
    }
    const float half_width = camera.near_plane
        * (lens.sensor_width_mm * 0.5F) / lens.focal_length_mm;
    const float half_height = camera.near_plane
        * (lens.sensor_height_mm * 0.5F) / lens.focal_length_mm;
    const FrustumWindow result{-half_width, half_width, -half_height,
                               half_height, camera.near_plane,
                               camera.far_plane};
    return valid_window(result) ? result : FrustumWindow{};
}

FrustumWindow distorted_window_for_lens(
    const Camera& camera,
    const LensProfile& profile,
    const LensEncoders& encoders) noexcept {
    const FrustumWindow undistorted = window_for_lens(camera, profile, encoders);
    if (!valid_window(undistorted)) {
        return FrustumWindow{};
    }
    const EvaluatedLens lens = evaluate_lens(profile, encoders);
    const DistortedLensExtent extent = distorted_lens_extent(lens.distortion);
    if (!extent.valid) {
        return undistorted;
    }
    const float half_width = (undistorted.right - undistorted.left) * 0.5F;
    const float half_height = (undistorted.top - undistorted.bottom) * 0.5F;
    const FrustumWindow result{half_width * extent.left,
                               half_width * extent.right,
                               half_height * extent.bottom,
                               half_height * extent.top,
                               undistorted.near_plane,
                               undistorted.far_plane};
    return valid_window(result) ? result : undistorted;
}

FrustumWindow window_for_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice,
    const LensProfile& profile,
    const LensEncoders& encoders) noexcept {
    return slice_of_window(window_for_lens(camera, profile, encoders), canvas,
                           slice);
}

ViewMatrices view_for_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice,
    const LensProfile& profile,
    const LensEncoders& encoders) noexcept {
    return view_from_window(camera,
                            window_for_slice(camera, canvas, slice, profile,
                                             encoders));
}

ViewMatrices view_for_lens(const Camera& camera,
                           const LensProfile& profile,
                           const LensEncoders& encoders) noexcept {
    return view_from_window(camera, window_for_lens(camera, profile, encoders));
}

ViewMatrices view_for_distorted_lens(
    const Camera& camera,
    const LensProfile& profile,
    const LensEncoders& encoders) noexcept {
    return view_from_window(camera,
                            distorted_window_for_lens(camera, profile,
                                                      encoders));
}

FrustumWindow window_for_aspect(
    const Camera& camera, float aspect_ratio) noexcept {
    if (!valid(camera) || !std::isfinite(aspect_ratio)
        || !(aspect_ratio > 0.0F)) {
        return FrustumWindow{};
    }
    const auto top =
        camera.near_plane * deterministic::tan_half_turns(camera.field_of_view);
    const auto right = top * aspect_ratio;
    return FrustumWindow{
        -right, right, -top, top, camera.near_plane, camera.far_plane};
}

ViewMatrices view_for_aspect(
    const Camera& camera, float aspect_ratio) noexcept {
    return view_from_window(camera, window_for_aspect(camera, aspect_ratio));
}

CanvasProjection project_to_canvas(
    const Camera& camera,
    Canvas canvas,
    Vec3 world_point) noexcept {
    const auto rect = whole(canvas);
    const auto matrices = view_for_slice(camera, canvas, rect);
    return project(matrices.view_projection, rect, world_point);
}

CanvasProjection project_to_canvas(
    const ViewMatrices& matrices,
    Canvas canvas,
    Vec3 world_point) noexcept {
    return project(matrices.view_projection, whole(canvas), world_point);
}

CanvasProjection project_through_slice(
    const Camera& camera,
    Canvas canvas,
    CanvasRect slice,
    Vec3 world_point) noexcept {
    const auto matrices = view_for_slice(camera, canvas, slice);
    return project(matrices.view_projection, slice, world_point);
}

bool intersects(CanvasRect a, CanvasRect b) noexcept {
    if (a.width == 0U || a.height == 0U || b.width == 0U || b.height == 0U) {
        return false;
    }
    const auto a_right = static_cast<core::u64>(a.x) + a.width;
    const auto a_bottom = static_cast<core::u64>(a.y) + a.height;
    const auto b_right = static_cast<core::u64>(b.x) + b.width;
    const auto b_bottom = static_cast<core::u64>(b.y) + b.height;
    return static_cast<core::u64>(a.x) < b_right
        && static_cast<core::u64>(b.x) < a_right
        && static_cast<core::u64>(a.y) < b_bottom
        && static_cast<core::u64>(b.y) < a_bottom;
}

CanvasRect intersection(CanvasRect a, CanvasRect b) noexcept {
    if (!intersects(a, b)) {
        return CanvasRect{};
    }
    const auto left = std::max(static_cast<core::u64>(a.x),
                               static_cast<core::u64>(b.x));
    const auto top = std::max(static_cast<core::u64>(a.y),
                              static_cast<core::u64>(b.y));
    const auto right = std::min(static_cast<core::u64>(a.x) + a.width,
                                static_cast<core::u64>(b.x) + b.width);
    const auto bottom = std::min(static_cast<core::u64>(a.y) + a.height,
                                 static_cast<core::u64>(b.y) + b.height);
    return CanvasRect{
        static_cast<core::u32>(left),
        static_cast<core::u32>(top),
        static_cast<core::u32>(right - left),
        static_cast<core::u32>(bottom - top),
    };
}

FrustumCorners frustum_corners(
    const Camera& camera, float aspect_ratio) noexcept {
    if (!valid(camera) || !std::isfinite(aspect_ratio)
        || !(aspect_ratio > 0.0F)) {
        return FrustumCorners{};
    }

    // The basis the view matrix is built from, formed here by the same
    // expressions `look_at` uses so the corners and the projection cannot
    // describe two slightly different cameras.
    const auto forward = normalize(camera.target - camera.eye);
    const auto right = normalize(cross(forward, camera.up),
                                 Vec3{1.0F, 0.0F, 0.0F});
    const auto up = cross(right, forward);

    const auto tangent = deterministic::tan_half_turns(camera.field_of_view);
    const auto near_half_height = camera.near_plane * tangent;
    const auto near_half_width = near_half_height * aspect_ratio;
    const auto far_half_height = camera.far_plane * tangent;
    const auto far_half_width = far_half_height * aspect_ratio;

    const auto corner = [&](float distance, float half_width,
                            float half_height, float x_sign,
                            float y_sign) noexcept {
        const auto centre = camera.eye + forward * distance;
        return centre + right * (half_width * x_sign)
            + up * (half_height * y_sign);
    };

    FrustumCorners result{};
    result.points[0] = corner(camera.near_plane, near_half_width,
                              near_half_height, -1.0F, -1.0F);
    result.points[1] = corner(camera.near_plane, near_half_width,
                              near_half_height, 1.0F, -1.0F);
    result.points[2] = corner(camera.near_plane, near_half_width,
                              near_half_height, -1.0F, 1.0F);
    result.points[3] = corner(camera.near_plane, near_half_width,
                              near_half_height, 1.0F, 1.0F);
    result.points[4] = corner(camera.far_plane, far_half_width,
                              far_half_height, -1.0F, -1.0F);
    result.points[5] = corner(camera.far_plane, far_half_width,
                              far_half_height, 1.0F, -1.0F);
    result.points[6] = corner(camera.far_plane, far_half_width,
                              far_half_height, -1.0F, 1.0F);
    result.points[7] = corner(camera.far_plane, far_half_width,
                              far_half_height, 1.0F, 1.0F);
    result.valid = true;
    return result;
}

FrustumCorners frustum_corners(const Camera& camera,
                               const FrustumWindow& window) noexcept {
    if (!valid_camera_body(camera) || !valid_window(window)) {
        return FrustumCorners{};
    }
    const auto forward = normalize(camera.target - camera.eye);
    const auto right = normalize(cross(forward, camera.up),
                                 Vec3{1.0F, 0.0F, 0.0F});
    const auto up = cross(right, forward);
    const auto corner = [&](float distance, float horizontal,
                            float vertical) noexcept {
        const float scale = distance / window.near_plane;
        const Vec3 centre = camera.eye + forward * distance;
        return centre + right * (horizontal * scale)
            + up * (vertical * scale);
    };

    FrustumCorners result{};
    result.points[0] =
        corner(window.near_plane, window.left, window.bottom);
    result.points[1] =
        corner(window.near_plane, window.right, window.bottom);
    result.points[2] = corner(window.near_plane, window.left, window.top);
    result.points[3] = corner(window.near_plane, window.right, window.top);
    result.points[4] = corner(window.far_plane, window.left, window.bottom);
    result.points[5] = corner(window.far_plane, window.right, window.bottom);
    result.points[6] = corner(window.far_plane, window.left, window.top);
    result.points[7] = corner(window.far_plane, window.right, window.top);
    result.valid = true;
    return result;
}

FrustumCorners frustum_corners_for_lens(
    const Camera& camera,
    const LensProfile& profile,
    const LensEncoders& encoders,
    bool distorted_coverage) noexcept {
    const FrustumWindow window = distorted_coverage
        ? distorted_window_for_lens(camera, profile, encoders)
        : window_for_lens(camera, profile, encoders);
    return frustum_corners(camera, window);
}

InnerFrustumRegion inner_frustum_rect(
    Canvas canvas,
    std::span<const CanvasProjection> projected,
    core::u32 margin_px) noexcept {
    if (!valid(canvas)) {
        return InnerFrustumRegion{};
    }

    bool any = false;
    float min_x = 0.0F;
    float max_x = 0.0F;
    float min_y = 0.0F;
    float max_y = 0.0F;
    for (const CanvasProjection& point : projected) {
        if (!point.visible || !std::isfinite(point.x)
            || !std::isfinite(point.y)) {
            continue;
        }
        if (!any) {
            min_x = point.x;
            max_x = point.x;
            min_y = point.y;
            max_y = point.y;
            any = true;
            continue;
        }
        min_x = std::min(min_x, point.x);
        max_x = std::max(max_x, point.x);
        min_y = std::min(min_y, point.y);
        max_y = std::max(max_y, point.y);
    }
    if (!any) {
        return InnerFrustumRegion{};
    }

    // Outward on both sides, in the one expression every node evaluates.
    // std::floor and std::ceil are exact operations on a binary float, so two
    // nodes given the same bits produce the same integer; there is nothing
    // here for a platform to round differently.
    const auto extent = static_cast<double>(max_canvas_extent) + 1.0;
    const auto low = [extent](float value, core::u32 margin) noexcept {
        const auto floored =
            std::floor(static_cast<double>(value)) - static_cast<double>(margin);
        return static_cast<core::i64>(std::clamp(floored, -extent, extent));
    };
    const auto high = [extent](float value, core::u32 margin) noexcept {
        const auto ceiled =
            std::ceil(static_cast<double>(value)) + static_cast<double>(margin);
        return static_cast<core::i64>(std::clamp(ceiled, -extent, extent));
    };

    const core::i64 left = low(min_x, margin_px);
    const core::i64 top = low(min_y, margin_px);
    const core::i64 right = high(max_x, margin_px);
    const core::i64 bottom = high(max_y, margin_px);

    const auto canvas_width = static_cast<core::i64>(canvas.width);
    const auto canvas_height = static_cast<core::i64>(canvas.height);
    const bool clamped = left < 0 || top < 0 || right > canvas_width
        || bottom > canvas_height;

    const core::i64 clipped_left = std::clamp(left, core::i64{0}, canvas_width);
    const core::i64 clipped_top = std::clamp(top, core::i64{0}, canvas_height);
    const core::i64 clipped_right =
        std::clamp(right, core::i64{0}, canvas_width);
    const core::i64 clipped_bottom =
        std::clamp(bottom, core::i64{0}, canvas_height);
    if (clipped_right <= clipped_left || clipped_bottom <= clipped_top) {
        // The camera sees the world somewhere, but not on this canvas.
        return InnerFrustumRegion{CanvasRect{}, false, true};
    }

    return InnerFrustumRegion{
        CanvasRect{
            static_cast<core::u32>(clipped_left),
            static_cast<core::u32>(clipped_top),
            static_cast<core::u32>(clipped_right - clipped_left),
            static_cast<core::u32>(clipped_bottom - clipped_top),
        },
        true,
        clamped,
    };
}

} // namespace mediaserver::scene
