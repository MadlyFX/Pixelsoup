#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/math.hpp>
#include <ajfx/package.hpp>
#include <ajfx/result.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Distance fields: a grid of signed distances to a surface, negative inside
// it, that SdfCollision keeps particles out of (or in). A package carries
// each one as an entry "fields/<name>", built when the package is authored
// (Studio bakes them from meshes), so every node collides with identical
// numbers. Entry layout, all little-endian:
//
//     "AJSD"                 4 bytes
//     field version          u32
//     nx, ny, nz             u32 x 3 (2 to max_field_size)
//     origin                 f32 x 3, the first sample's position
//     spacing                f32, between neighbouring samples
//     distances              f32 x nx*ny*nz, x fastest, then y, then z
namespace ajfx {

inline constexpr std::uint32_t field_format_version = 1U;
inline constexpr std::uint32_t max_field_size = 256U;
inline constexpr std::uint64_t max_field_samples = 1U << 22U;

struct DistanceField final {
    std::uint32_t nx{0U};
    std::uint32_t ny{0U};
    std::uint32_t nz{0U};
    Vec3 origin{};
    float spacing{1.0F};
    std::vector<float> distances;

    friend bool operator==(const DistanceField&, const DistanceField&) = default;
};

// A field sampling `distance` at every grid point. Fails for a grid out of
// range or a spacing that is not positive and finite.
[[nodiscard]] Result<DistanceField> make_distance_field(std::uint32_t nx, std::uint32_t ny, std::uint32_t nz,
                                                        Vec3 origin, float spacing,
                                                        const std::function<float(Vec3)>& distance);

struct FieldSample final {
    float distance{};
    // The gradient of the interpolated distance: the way out of the surface.
    Vec3 gradient{};
};

// Trilinear distance and its exact gradient at `p` (the field's own space),
// written operation for operation as the GPU kernels evaluate it; nothing
// outside the grid.
[[nodiscard]] std::optional<FieldSample> sample_field(const DistanceField& field, Vec3 p);

[[nodiscard]] std::vector<std::uint8_t> write_field(const DistanceField& field);
// Fails with MalformedPackage for a bad header, size or sample, and
// UnsupportedVersion for a newer layout.
[[nodiscard]] Result<DistanceField> read_field(std::span<const std::uint8_t> bytes);

// Fields by the name SdfCollision modules use.
using FieldSet = std::map<std::string, DistanceField, std::less<>>;

// "fields/<name>" entries for write_package's `extra`.
[[nodiscard]] std::vector<PackageEntry> field_package_entries(const FieldSet& fields);

// Checks that `fields` holds every field the effect's modules name. Fails
// with MissingField, naming the emitter.
[[nodiscard]] Result<bool> check_fields(const Effect& effect, const FieldSet& fields);

// Every field the effect names, read from its package and checked as above.
[[nodiscard]] Result<FieldSet> read_package_fields(std::span<const std::uint8_t> package, const Effect& effect);

} // namespace ajfx
