#include "volume.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace ajfx::detail {
namespace {

constexpr float largest_quantized = 2147483520.0F; // the largest float below 2^31

// A cell's place in the grid, x fastest.
inline std::uint32_t at(const VolumeStep& g, std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept {
    return (z * g.ny + y) * g.nx + x;
}

// The velocity of the cell `d` along from (x, y, z) on one axis, or nothing
// where that is outside the grid: the walls do not move.
inline Vec3 velocity_or_still(const VolumeStep& g, const VolumeState& s, std::int64_t x, std::int64_t y,
                              std::int64_t z) noexcept {
    if (x < 0 || y < 0 || z < 0 || x >= g.nx || y >= g.ny || z >= g.nz) {
        return Vec3{};
    }
    return s.velocity[at(g, static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y),
                         static_cast<std::uint32_t>(z))];
}

// A scalar of the cell along one axis, clamped to the grid's edge.
inline float scalar_clamped(const VolumeStep& g, const std::vector<float>& field, std::int64_t x, std::int64_t y,
                            std::int64_t z) noexcept {
    const auto cx = static_cast<std::uint32_t>(std::clamp<std::int64_t>(x, 0, g.nx - 1U));
    const auto cy = static_cast<std::uint32_t>(std::clamp<std::int64_t>(y, 0, g.ny - 1U));
    const auto cz = static_cast<std::uint32_t>(std::clamp<std::int64_t>(z, 0, g.nz - 1U));
    return field[at(g, cx, cy, cz)];
}

// Where a point in grid units (the lattice of cell centres) samples from:
// the lower cell on each axis and the fraction towards the next, clamped to
// the edge. As ajfx_volume_taps works it out.
struct Taps final {
    std::uint32_t ix, iy, iz;
    float fx, fy, fz;
};

inline Taps taps(const VolumeStep& g, Vec3 p) noexcept {
    const float gx = std::min(std::max(p.x, 0.0F), static_cast<float>(g.nx - 1U));
    const float gy = std::min(std::max(p.y, 0.0F), static_cast<float>(g.ny - 1U));
    const float gz = std::min(std::max(p.z, 0.0F), static_cast<float>(g.nz - 1U));
    Taps t{};
    t.ix = std::min(static_cast<std::uint32_t>(std::floor(gx)), g.nx - 2U);
    t.iy = std::min(static_cast<std::uint32_t>(std::floor(gy)), g.ny - 2U);
    t.iz = std::min(static_cast<std::uint32_t>(std::floor(gz)), g.nz - 2U);
    t.fx = gx - static_cast<float>(t.ix);
    t.fy = gy - static_cast<float>(t.iy);
    t.fz = gz - static_cast<float>(t.iz);
    return t;
}

// Trilinear between eight values, in the order ajfx_volume_trilinear adds
// them.
inline float trilinear(float c000, float c100, float c010, float c110, float c001, float c101, float c011,
                       float c111, const Taps& t) noexcept {
    const float c00 = c000 + (c100 - c000) * t.fx;
    const float c10 = c010 + (c110 - c010) * t.fx;
    const float c01 = c001 + (c101 - c001) * t.fx;
    const float c11 = c011 + (c111 - c011) * t.fx;
    const float c0 = c00 + (c10 - c00) * t.fy;
    const float c1 = c01 + (c11 - c01) * t.fy;
    return c0 + (c1 - c0) * t.fz;
}

inline float sample_scalar(const VolumeStep& g, const std::vector<float>& f, const Taps& t) noexcept {
    return trilinear(f[at(g, t.ix, t.iy, t.iz)], f[at(g, t.ix + 1U, t.iy, t.iz)], f[at(g, t.ix, t.iy + 1U, t.iz)],
                     f[at(g, t.ix + 1U, t.iy + 1U, t.iz)], f[at(g, t.ix, t.iy, t.iz + 1U)],
                     f[at(g, t.ix + 1U, t.iy, t.iz + 1U)], f[at(g, t.ix, t.iy + 1U, t.iz + 1U)],
                     f[at(g, t.ix + 1U, t.iy + 1U, t.iz + 1U)], t);
}

inline Vec3 sample_velocity(const VolumeStep& g, const std::vector<Vec3>& v, const Taps& t) noexcept {
    const Vec3 c000 = v[at(g, t.ix, t.iy, t.iz)];
    const Vec3 c100 = v[at(g, t.ix + 1U, t.iy, t.iz)];
    const Vec3 c010 = v[at(g, t.ix, t.iy + 1U, t.iz)];
    const Vec3 c110 = v[at(g, t.ix + 1U, t.iy + 1U, t.iz)];
    const Vec3 c001 = v[at(g, t.ix, t.iy, t.iz + 1U)];
    const Vec3 c101 = v[at(g, t.ix + 1U, t.iy, t.iz + 1U)];
    const Vec3 c011 = v[at(g, t.ix, t.iy + 1U, t.iz + 1U)];
    const Vec3 c111 = v[at(g, t.ix + 1U, t.iy + 1U, t.iz + 1U)];
    return Vec3{trilinear(c000.x, c100.x, c010.x, c110.x, c001.x, c101.x, c011.x, c111.x, t),
                trilinear(c000.y, c100.y, c010.y, c110.y, c001.y, c101.y, c011.y, c111.y, t),
                trilinear(c000.z, c100.z, c010.z, c110.z, c001.z, c101.z, c011.z, c111.z, t)};
}

void size_like(VolumeState& s, const VolumeState& like) {
    s.nx = like.nx;
    s.ny = like.ny;
    s.nz = like.nz;
    s.density.resize(like.density.size());
    s.temperature.resize(like.temperature.size());
    s.velocity.resize(like.velocity.size());
}

} // namespace

VolumeStep volume_step(const Volume& volume, float dt) {
    VolumeStep g;
    g.nx = volume.nx;
    g.ny = volume.ny;
    g.nz = volume.nz;
    g.cells = static_cast<std::uint32_t>(volume_cells(volume));
    g.origin = volume.origin;
    const float h = volume.cell_size;
    g.inv_cell = 1.0F / h;
    g.buoyancy_dt = volume.buoyancy * dt;
    g.weight_dt = volume.weight * dt;
    g.confinement = volume.vorticity * h * dt;
    g.inv_two_cells = 1.0F / (2.0F * h);
    g.cell_squared = h * h;
    g.advect_scale = dt * g.inv_cell;
    g.keep_density = 1.0F / (1.0F + volume.density_dissipation * dt);
    g.keep_temperature = 1.0F / (1.0F + volume.temperature_dissipation * dt);
    g.keep_velocity = 1.0F / (1.0F + volume.velocity_dissipation * dt);
    g.pressure_iterations = volume.pressure_iterations;
    return g;
}

VolumeState empty_volume(const Volume& volume) {
    VolumeState s;
    s.nx = volume.nx;
    s.ny = volume.ny;
    s.nz = volume.nz;
    const auto cells = static_cast<std::size_t>(volume_cells(volume));
    s.density.assign(cells, 0.0F);
    s.temperature.assign(cells, 0.0F);
    s.velocity.assign(cells, Vec3{});
    return s;
}

void VolumeDeposits::clear() noexcept {
    std::fill(sums.begin(), sums.end(), std::int64_t{0});
    deposited = false;
}

std::optional<std::uint32_t> volume_cell(const VolumeStep& g, Vec3 p) noexcept {
    const float ux = (p.x - g.origin.x) * g.inv_cell;
    const float uy = (p.y - g.origin.y) * g.inv_cell;
    const float uz = (p.z - g.origin.z) * g.inv_cell;
    if (!(ux >= 0.0F && uy >= 0.0F && uz >= 0.0F && ux < static_cast<float>(g.nx) && uy < static_cast<float>(g.ny) &&
          uz < static_cast<float>(g.nz))) {
        return std::nullopt;
    }
    return at(g, static_cast<std::uint32_t>(std::floor(ux)), static_cast<std::uint32_t>(std::floor(uy)),
              static_cast<std::uint32_t>(std::floor(uz)));
}

std::int32_t quantize_deposit(float amount) noexcept {
    float scaled = amount * 1048576.0F;
    scaled = std::min(std::max(scaled, -largest_quantized), largest_quantized);
    return static_cast<std::int32_t>(std::floor(scaled + 0.5F));
}

float decode_deposit(std::int64_t sum) noexcept {
    const auto lo = static_cast<std::uint32_t>(static_cast<std::uint64_t>(sum));
    const auto hi = static_cast<std::int32_t>(sum >> 32);
    return (static_cast<float>(hi) * 4294967296.0F + static_cast<float>(lo >> 16U) * 65536.0F) +
           static_cast<float>(lo & 0xffffU);
}

void deposit(VolumeDeposits& d, std::uint32_t cell, float density, float temperature, Vec3 momentum) {
    std::int64_t* sums = d.sums.data() + static_cast<std::size_t>(cell) * 5U;
    sums[0] += quantize_deposit(density);
    sums[1] += quantize_deposit(temperature);
    sums[2] += quantize_deposit(momentum.x);
    sums[3] += quantize_deposit(momentum.y);
    sums[4] += quantize_deposit(momentum.z);
    d.deposited = true;
}

std::optional<Vec3> sample_volume_velocity(const VolumeStep& g, const VolumeState& s, Vec3 p) {
    const float ux = (p.x - g.origin.x) * g.inv_cell;
    const float uy = (p.y - g.origin.y) * g.inv_cell;
    const float uz = (p.z - g.origin.z) * g.inv_cell;
    if (!(ux >= 0.0F && uy >= 0.0F && uz >= 0.0F && ux <= static_cast<float>(g.nx) &&
          uy <= static_cast<float>(g.ny) && uz <= static_cast<float>(g.nz))) {
        return std::nullopt;
    }
    return sample_velocity(g, s.velocity, taps(g, Vec3{ux - 0.5F, uy - 0.5F, uz - 0.5F}));
}

void step_volume(const VolumeStep& g, VolumeDeposits& deposits, VolumeState& s, VolumeScratch& w) {
    const std::uint32_t n = g.cells;
    size_like(w.next, s);
    w.curl.resize(n);
    w.magnitude.resize(n);
    w.divergence.resize(n);
    w.pressure[0].resize(n);
    w.pressure[1].resize(n);
    if (deposits.sums.size() != static_cast<std::size_t>(n) * 5U) {
        deposits.sums.assign(static_cast<std::size_t>(n) * 5U, 0);
    }

    // Inject and buoyancy, each cell on its own.
    for (std::uint32_t c = 0U; c < n; ++c) {
        const std::int64_t* sums = deposits.sums.data() + static_cast<std::size_t>(c) * 5U;
        const float d = s.density[c] + decode_deposit(sums[0]) * volume_deposit_quantum;
        const float t = s.temperature[c] + decode_deposit(sums[1]) * volume_deposit_quantum;
        Vec3 v = s.velocity[c];
        v.x = v.x + decode_deposit(sums[2]) * volume_deposit_quantum;
        v.y = v.y + decode_deposit(sums[3]) * volume_deposit_quantum;
        v.z = v.z + decode_deposit(sums[4]) * volume_deposit_quantum;
        v.y = v.y + (t * g.buoyancy_dt - d * g.weight_dt);
        s.density[c] = d;
        s.temperature[c] = t;
        s.velocity[c] = v;
    }
    deposits.clear();

    // Vorticity confinement: the curl and its size at every cell, then a
    // push along the curl, across the way its size grows.
    if (g.confinement > 0.0F) {
        for (std::uint32_t z = 0U; z < g.nz; ++z) {
            for (std::uint32_t y = 0U; y < g.ny; ++y) {
                for (std::uint32_t x = 0U; x < g.nx; ++x) {
                    const Vec3 xp = velocity_or_still(g, s, x + 1, y, z);
                    const Vec3 xm = velocity_or_still(g, s, static_cast<std::int64_t>(x) - 1, y, z);
                    const Vec3 yp = velocity_or_still(g, s, x, y + 1, z);
                    const Vec3 ym = velocity_or_still(g, s, x, static_cast<std::int64_t>(y) - 1, z);
                    const Vec3 zp = velocity_or_still(g, s, x, y, z + 1);
                    const Vec3 zm = velocity_or_still(g, s, x, y, static_cast<std::int64_t>(z) - 1);
                    const Vec3 curl{(yp.z - ym.z) * g.inv_two_cells - (zp.y - zm.y) * g.inv_two_cells,
                                    (zp.x - zm.x) * g.inv_two_cells - (xp.z - xm.z) * g.inv_two_cells,
                                    (xp.y - xm.y) * g.inv_two_cells - (yp.x - ym.x) * g.inv_two_cells};
                    const std::uint32_t c = at(g, x, y, z);
                    w.curl[c] = curl;
                    w.magnitude[c] = std::sqrt(dot(curl, curl));
                }
            }
        }
        for (std::uint32_t z = 0U; z < g.nz; ++z) {
            for (std::uint32_t y = 0U; y < g.ny; ++y) {
                for (std::uint32_t x = 0U; x < g.nx; ++x) {
                    const std::int64_t ix = x;
                    const std::int64_t iy = y;
                    const std::int64_t iz = z;
                    const Vec3 grow{
                        (scalar_clamped(g, w.magnitude, ix + 1, iy, iz) - scalar_clamped(g, w.magnitude, ix - 1, iy, iz)) *
                            g.inv_two_cells,
                        (scalar_clamped(g, w.magnitude, ix, iy + 1, iz) - scalar_clamped(g, w.magnitude, ix, iy - 1, iz)) *
                            g.inv_two_cells,
                        (scalar_clamped(g, w.magnitude, ix, iy, iz + 1) - scalar_clamped(g, w.magnitude, ix, iy, iz - 1)) *
                            g.inv_two_cells};
                    const float length = std::sqrt(dot(grow, grow));
                    if (length > 1.0e-6F) {
                        const std::uint32_t c = at(g, x, y, z);
                        const Vec3 towards = grow * (1.0F / length);
                        s.velocity[c] += cross(towards, w.curl[c]) * g.confinement;
                    }
                }
            }
        }
    }

    // Semi-Lagrangian advection: each cell takes what the velocity carried
    // to its centre, from the grid as it stands.
    for (std::uint32_t z = 0U; z < g.nz; ++z) {
        for (std::uint32_t y = 0U; y < g.ny; ++y) {
            for (std::uint32_t x = 0U; x < g.nx; ++x) {
                const std::uint32_t c = at(g, x, y, z);
                const Vec3 v = s.velocity[c];
                const Taps t = taps(g, Vec3{static_cast<float>(x) - v.x * g.advect_scale,
                                            static_cast<float>(y) - v.y * g.advect_scale,
                                            static_cast<float>(z) - v.z * g.advect_scale});
                w.next.density[c] = sample_scalar(g, s.density, t);
                w.next.temperature[c] = sample_scalar(g, s.temperature, t);
                w.next.velocity[c] = sample_velocity(g, s.velocity, t);
            }
        }
    }
    std::swap(s.density, w.next.density);
    std::swap(s.temperature, w.next.temperature);
    std::swap(s.velocity, w.next.velocity);

    // Pressure: the divergence, Jacobi from zero, and the gradient taken off.
    for (std::uint32_t z = 0U; z < g.nz; ++z) {
        for (std::uint32_t y = 0U; y < g.ny; ++y) {
            for (std::uint32_t x = 0U; x < g.nx; ++x) {
                const Vec3 xp = velocity_or_still(g, s, x + 1, y, z);
                const Vec3 xm = velocity_or_still(g, s, static_cast<std::int64_t>(x) - 1, y, z);
                const Vec3 yp = velocity_or_still(g, s, x, y + 1, z);
                const Vec3 ym = velocity_or_still(g, s, x, static_cast<std::int64_t>(y) - 1, z);
                const Vec3 zp = velocity_or_still(g, s, x, y, z + 1);
                const Vec3 zm = velocity_or_still(g, s, x, y, static_cast<std::int64_t>(z) - 1);
                const std::uint32_t c = at(g, x, y, z);
                w.divergence[c] = (((xp.x - xm.x) + (yp.y - ym.y)) + (zp.z - zm.z)) * g.inv_two_cells;
                w.pressure[0][c] = 0.0F;
            }
        }
    }
    std::uint32_t from = 0U;
    for (std::uint32_t iteration = 0U; iteration < g.pressure_iterations; ++iteration) {
        const std::vector<float>& in = w.pressure[from];
        std::vector<float>& out = w.pressure[1U - from];
        for (std::uint32_t z = 0U; z < g.nz; ++z) {
            for (std::uint32_t y = 0U; y < g.ny; ++y) {
                for (std::uint32_t x = 0U; x < g.nx; ++x) {
                    const std::int64_t ix = x;
                    const std::int64_t iy = y;
                    const std::int64_t iz = z;
                    const std::uint32_t c = at(g, x, y, z);
                    const float sum = ((((scalar_clamped(g, in, ix - 1, iy, iz) + scalar_clamped(g, in, ix + 1, iy, iz)) +
                                         scalar_clamped(g, in, ix, iy - 1, iz)) +
                                        scalar_clamped(g, in, ix, iy + 1, iz)) +
                                       scalar_clamped(g, in, ix, iy, iz - 1)) +
                                      scalar_clamped(g, in, ix, iy, iz + 1);
                    out[c] = (sum - w.divergence[c] * g.cell_squared) * (1.0F / 6.0F);
                }
            }
        }
        from = 1U - from;
    }
    const std::vector<float>& p = w.pressure[from];
    for (std::uint32_t z = 0U; z < g.nz; ++z) {
        for (std::uint32_t y = 0U; y < g.ny; ++y) {
            for (std::uint32_t x = 0U; x < g.nx; ++x) {
                const std::int64_t ix = x;
                const std::int64_t iy = y;
                const std::int64_t iz = z;
                const std::uint32_t c = at(g, x, y, z);
                Vec3 v = s.velocity[c];
                v.x = v.x - (scalar_clamped(g, p, ix + 1, iy, iz) - scalar_clamped(g, p, ix - 1, iy, iz)) * g.inv_two_cells;
                v.y = v.y - (scalar_clamped(g, p, ix, iy + 1, iz) - scalar_clamped(g, p, ix, iy - 1, iz)) * g.inv_two_cells;
                v.z = v.z - (scalar_clamped(g, p, ix, iy, iz + 1) - scalar_clamped(g, p, ix, iy, iz - 1)) * g.inv_two_cells;
                s.velocity[c] = v * g.keep_velocity;
                s.density[c] = s.density[c] * g.keep_density;
                s.temperature[c] = s.temperature[c] * g.keep_temperature;
            }
        }
    }
}

} // namespace ajfx::detail
