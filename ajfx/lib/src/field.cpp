#include <ajfx/field.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>

namespace ajfx {
namespace {

constexpr char field_magic[4] = {'A', 'J', 'S', 'D'};
constexpr std::size_t header_bytes = 36U;
constexpr std::string_view field_prefix = "fields/";

std::string quoted(std::string_view text) {
    return "'" + std::string{text} + "'";
}

bool valid_grid(std::uint32_t nx, std::uint32_t ny, std::uint32_t nz) {
    const auto in_range = [](std::uint32_t n) { return n >= 2U && n <= max_field_size; };
    return in_range(nx) && in_range(ny) && in_range(nz) &&
           std::uint64_t{nx} * ny * nz <= max_field_samples;
}

bool finite(Vec3 v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (std::uint32_t i = 0U; i < 4U; ++i) {
        out.push_back(static_cast<std::uint8_t>(value >> (8U * i)));
    }
}

void put_f32(std::vector<std::uint8_t>& out, float value) {
    std::uint32_t bits = 0U;
    std::memcpy(&bits, &value, 4U);
    put_u32(out, bits);
}

std::uint32_t get_u32(std::span<const std::uint8_t> bytes, std::size_t at) {
    std::uint32_t value = 0U;
    for (std::uint32_t i = 0U; i < 4U; ++i) {
        value |= static_cast<std::uint32_t>(bytes[at + i]) << (8U * i);
    }
    return value;
}

float get_f32(std::span<const std::uint8_t> bytes, std::size_t at) {
    const std::uint32_t bits = get_u32(bytes, at);
    float value = 0.0F;
    std::memcpy(&value, &bits, 4U);
    return value;
}

Error malformed(const std::string& problem) {
    return Error{ErrorCode::MalformedPackage, "distance field " + problem};
}

// The field names an emitter's SdfCollision modules use.
template <class Visit>
void for_each_field_name(const Emitter& emitter, Visit&& visit) {
    for (const UpdateModule& module : emitter.update) {
        if (const auto* collision = std::get_if<SdfCollision>(&module)) {
            visit(collision->field);
        }
    }
}

} // namespace

Result<DistanceField> make_distance_field(std::uint32_t nx, std::uint32_t ny, std::uint32_t nz, Vec3 origin,
                                          float spacing, const std::function<float(Vec3)>& distance) {
    if (!valid_grid(nx, ny, nz)) {
        return Error{ErrorCode::InvalidEffect, "a distance field must be 2 to " + std::to_string(max_field_size) +
                                                   " samples on each axis and at most " +
                                                   std::to_string(max_field_samples) + " in all"};
    }
    if (!(spacing > 0.0F) || !std::isfinite(spacing) || !finite(origin)) {
        return Error{ErrorCode::InvalidEffect, "a distance field needs a finite origin and a positive spacing"};
    }
    DistanceField field;
    field.nx = nx;
    field.ny = ny;
    field.nz = nz;
    field.origin = origin;
    field.spacing = spacing;
    field.distances.reserve(std::size_t{nx} * ny * nz);
    for (std::uint32_t z = 0U; z < nz; ++z) {
        for (std::uint32_t y = 0U; y < ny; ++y) {
            for (std::uint32_t x = 0U; x < nx; ++x) {
                const Vec3 p{origin.x + static_cast<float>(x) * spacing, origin.y + static_cast<float>(y) * spacing,
                             origin.z + static_cast<float>(z) * spacing};
                const float d = distance(p);
                if (!std::isfinite(d)) {
                    return Error{ErrorCode::InvalidEffect, "a distance field sample is not finite"};
                }
                field.distances.push_back(d);
            }
        }
    }
    return field;
}

std::optional<FieldSample> sample_field(const DistanceField& f, Vec3 p) {
    const float gx = (p.x - f.origin.x) / f.spacing;
    const float gy = (p.y - f.origin.y) / f.spacing;
    const float gz = (p.z - f.origin.z) / f.spacing;
    if (!(gx >= 0.0F && gy >= 0.0F && gz >= 0.0F && gx <= static_cast<float>(f.nx - 1U) &&
          gy <= static_cast<float>(f.ny - 1U) && gz <= static_cast<float>(f.nz - 1U))) {
        return std::nullopt;
    }
    const std::uint32_t ix = std::min(static_cast<std::uint32_t>(std::floor(gx)), f.nx - 2U);
    const std::uint32_t iy = std::min(static_cast<std::uint32_t>(std::floor(gy)), f.ny - 2U);
    const std::uint32_t iz = std::min(static_cast<std::uint32_t>(std::floor(gz)), f.nz - 2U);
    const float fx = gx - static_cast<float>(ix);
    const float fy = gy - static_cast<float>(iy);
    const float fz = gz - static_cast<float>(iz);
    const auto at = [&f](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return f.distances[(std::size_t{z} * f.ny + y) * f.nx + x];
    };
    const float c000 = at(ix, iy, iz);
    const float c100 = at(ix + 1U, iy, iz);
    const float c010 = at(ix, iy + 1U, iz);
    const float c110 = at(ix + 1U, iy + 1U, iz);
    const float c001 = at(ix, iy, iz + 1U);
    const float c101 = at(ix + 1U, iy, iz + 1U);
    const float c011 = at(ix, iy + 1U, iz + 1U);
    const float c111 = at(ix + 1U, iy + 1U, iz + 1U);
    // Along x, then y, then z; the gradient is the derivative of exactly
    // this interpolation.
    const float c00 = c000 + (c100 - c000) * fx;
    const float c10 = c010 + (c110 - c010) * fx;
    const float c01 = c001 + (c101 - c001) * fx;
    const float c11 = c011 + (c111 - c011) * fx;
    const float c0 = c00 + (c10 - c00) * fy;
    const float c1 = c01 + (c11 - c01) * fy;
    const float e00 = c100 - c000;
    const float e10 = c110 - c010;
    const float e01 = c101 - c001;
    const float e11 = c111 - c011;
    const float ex0 = e00 + (e10 - e00) * fy;
    const float ex1 = e01 + (e11 - e01) * fy;
    const float dy0 = c10 - c00;
    const float dy1 = c11 - c01;
    FieldSample sample;
    sample.distance = c0 + (c1 - c0) * fz;
    sample.gradient = Vec3{(ex0 + (ex1 - ex0) * fz) / f.spacing, (dy0 + (dy1 - dy0) * fz) / f.spacing,
                           (c1 - c0) / f.spacing};
    return sample;
}

std::vector<std::uint8_t> write_field(const DistanceField& field) {
    std::vector<std::uint8_t> out(std::begin(field_magic), std::end(field_magic));
    put_u32(out, field_format_version);
    put_u32(out, field.nx);
    put_u32(out, field.ny);
    put_u32(out, field.nz);
    put_f32(out, field.origin.x);
    put_f32(out, field.origin.y);
    put_f32(out, field.origin.z);
    put_f32(out, field.spacing);
    for (const float d : field.distances) {
        put_f32(out, d);
    }
    return out;
}

Result<DistanceField> read_field(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < header_bytes || std::memcmp(bytes.data(), field_magic, 4U) != 0) {
        return malformed("has no AJSD header");
    }
    const std::uint32_t version = get_u32(bytes, 4U);
    if (version > field_format_version) {
        return Error{ErrorCode::UnsupportedVersion,
                     "distance field version " + std::to_string(version) + " is newer than this library reads"};
    }
    DistanceField field;
    field.nx = get_u32(bytes, 8U);
    field.ny = get_u32(bytes, 12U);
    field.nz = get_u32(bytes, 16U);
    field.origin = Vec3{get_f32(bytes, 20U), get_f32(bytes, 24U), get_f32(bytes, 28U)};
    field.spacing = get_f32(bytes, 32U);
    if (!valid_grid(field.nx, field.ny, field.nz)) {
        return malformed("grid " + std::to_string(field.nx) + "x" + std::to_string(field.ny) + "x" +
                         std::to_string(field.nz) + " is out of range");
    }
    if (!(field.spacing > 0.0F) || !std::isfinite(field.spacing) || !finite(field.origin)) {
        return malformed("has a bad origin or spacing");
    }
    const std::size_t samples = std::size_t{field.nx} * field.ny * field.nz;
    if (bytes.size() != header_bytes + samples * 4U) {
        return malformed("is not as long as its grid");
    }
    field.distances.resize(samples);
    for (std::size_t i = 0U; i < samples; ++i) {
        field.distances[i] = get_f32(bytes, header_bytes + i * 4U);
        if (!std::isfinite(field.distances[i])) {
            return malformed("has a sample that is not finite");
        }
    }
    return field;
}

std::vector<PackageEntry> field_package_entries(const FieldSet& fields) {
    std::vector<PackageEntry> entries;
    for (const auto& [name, field] : fields) {
        entries.push_back({std::string{field_prefix} + name, write_field(field)});
    }
    return entries;
}

Result<bool> check_fields(const Effect& effect, const FieldSet& fields) {
    for (const Emitter& emitter : effect.emitters) {
        std::optional<Error> missing;
        for_each_field_name(emitter, [&](const std::string& name) {
            if (!missing && !fields.contains(name)) {
                missing = Error{ErrorCode::MissingField, "emitter " + quoted(emitter.name) + " collides with field " +
                                                             quoted(name) + ", which the package does not hold"};
            }
        });
        if (missing) {
            return *std::move(missing);
        }
    }
    return true;
}

Result<FieldSet> read_package_fields(std::span<const std::uint8_t> package, const Effect& effect) {
    Result<std::vector<PackageEntry>> entries = read_container(package);
    if (!entries) {
        return entries.error();
    }
    FieldSet fields;
    std::optional<Error> error;
    for (const Emitter& emitter : effect.emitters) {
        for_each_field_name(emitter, [&](const std::string& name) {
            if (error || fields.contains(name)) {
                return;
            }
            const std::string entry_name = std::string{field_prefix} + name;
            const auto entry = std::find_if(entries.value().begin(), entries.value().end(),
                                            [&](const PackageEntry& e) { return e.name == entry_name; });
            if (entry == entries.value().end()) {
                return; // check_fields names the emitter
            }
            Result<DistanceField> field = read_field(entry->data);
            if (!field) {
                error = Error{field.error().code, quoted(entry_name) + ": " + field.error().message};
                return;
            }
            fields.emplace(name, std::move(field).value());
        });
    }
    if (error) {
        return *std::move(error);
    }
    if (Result<bool> checked = check_fields(effect, fields); !checked) {
        return checked.error();
    }
    return fields;
}

} // namespace ajfx
