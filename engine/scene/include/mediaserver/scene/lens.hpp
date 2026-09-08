#pragma once

#include <mediaserver/core/types.hpp>

#include <array>

// Lens calibration values shared by document evaluation, tracking, and
// projection construction.  The types deliberately contain no assets and no
// mutable cache: every render node evaluates the same bounded data from the
// replicated document and the same frame-stamped encoder values.
namespace mediaserver::scene {

// Brown-Conrady's radial/tangential camera model. Coordinates are normalised
// against the sensor half-extents: (-1,-1) and (1,1) are the undistorted sensor
// corners. A positive radial coefficient expands the field at the corners.
struct BrownConradyDistortion final {
    float k1{};
    float k2{};
    float k3{};
    float p1{};
    float p2{};

    bool operator==(const BrownConradyDistortion&) const = default;
};

struct LensCoordinate final {
    float x{};
    float y{};

    bool operator==(const LensCoordinate&) const = default;
};

// One measured calibration point. Encoder values retain the units supplied by
// the tracking source: FreeD values are its exact 24-bit counts, while a source
// that reports calibrated physical controls may use those units consistently
// in both the profile and the frame channel. The table is bounded so evaluating
// a lens never allocates or observes an asset.
struct LensCalibrationPoint final {
    float zoom_encoder{};
    float focus_encoder{};
    float focal_length_mm{35.0F};
    // Positive moves from the tracked marker toward the camera's forward axis.
    float entrance_pupil_offset_m{};
    BrownConradyDistortion distortion{};

    bool operator==(const LensCalibrationPoint&) const = default;
};

// Sixteen points are enough for the common 4x4 zoom/focus calibration grid,
// while keeping the complete authored value small, fixed-size, and cheap to
// copy into an evaluated Scene.
inline constexpr core::u32 max_lens_calibration_points = 16U;
inline constexpr float min_lens_sensor_extent_mm = 1.0F;
inline constexpr float max_lens_sensor_extent_mm = 200.0F;
inline constexpr float min_lens_focal_length_mm = 1.0F;
inline constexpr float max_lens_focal_length_mm = 2'000.0F;
inline constexpr float max_lens_pupil_offset_m = 2.0F;
inline constexpr float max_lens_distortion_coefficient = 8.0F;
inline constexpr float max_lens_encoder_value = 16'777'215.0F;

// Authored camera calibration. `focal_length_mm`, `entrance_pupil_offset_m`,
// and `distortion` are the one-point profile and the safe fallback whenever a
// tracker supplies no zoom/focus pair. Supplying calibration points makes the
// lookup bilinear over the nearest complete rectangle and uses the nearest
// measured point if the authored table has a hole.
struct LensProfile final {
    float sensor_width_mm{36.0F};
    float sensor_height_mm{24.0F};
    float focal_length_mm{35.0F};
    float entrance_pupil_offset_m{};
    BrownConradyDistortion distortion{};
    std::array<LensCalibrationPoint, max_lens_calibration_points>
        calibration{};
    core::u32 calibration_count{};

    bool operator==(const LensProfile&) const = default;
};

// Frame control data. Zero is a valid encoder position, so each component has
// an explicit validity bit. A held pose holds this value with it; no node
// estimates or interpolates a missing encoder locally.
struct LensEncoders final {
    float zoom{};
    float focus{};
    float iris{};
    bool zoom_valid{};
    bool focus_valid{};
    bool iris_valid{};

    bool operator==(const LensEncoders&) const = default;
};

// The pure result of applying one profile to one encoder sample.
struct EvaluatedLens final {
    float sensor_width_mm{};
    float sensor_height_mm{};
    float focal_length_mm{};
    float entrance_pupil_offset_m{};
    BrownConradyDistortion distortion{};
    bool calibrated{};

    bool operator==(const EvaluatedLens&) const = default;
};

// A conservative normalised distorted-field extent. It may be asymmetric
// because tangential terms can shift one edge more than its opposite edge; its
// analytic bound intentionally permits additional coverage rather than ever
// omitting a photographed corner.
struct DistortedLensExtent final {
    float left{-1.0F};
    float right{1.0F};
    float bottom{-1.0F};
    float top{1.0F};
    bool valid{};

    bool operator==(const DistortedLensExtent&) const = default;
};

[[nodiscard]] bool valid(const LensProfile& value) noexcept;
[[nodiscard]] bool valid(const LensEncoders& value) noexcept;
[[nodiscard]] LensProfile clamped(const LensProfile& value) noexcept;
[[nodiscard]] LensEncoders clamped(const LensEncoders& value) noexcept;

// Pure, bounded lookup of profile + encoders. With no complete zoom/focus pair
// it returns the profile's static calibration; it never remembers a previous
// answer.
[[nodiscard]] EvaluatedLens evaluate_lens(
    const LensProfile& profile, const LensEncoders& encoders) noexcept;

[[nodiscard]] LensCoordinate distort_brown_conrady(
    LensCoordinate undistorted,
    const BrownConradyDistortion& distortion) noexcept;

// Conservative extent over a fixed sensor grid. It is used only for coverage
// sizing; render projection itself always remains undistorted.
[[nodiscard]] DistortedLensExtent distorted_lens_extent(
    const BrownConradyDistortion& distortion) noexcept;

} // namespace mediaserver::scene
