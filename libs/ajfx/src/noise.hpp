#pragma once

#include <ajfx/math.hpp>
#include <ajfx/random.hpp>

#include <cmath>
#include <cstdint>

// Lattice gradient noise and its curl. The lattice hash is the same integer
// hash particles use, so the field is identical on every machine; only the
// interpolation is floating point, compiled without contraction.
namespace ajfx::detail {

inline float fade(float t) {
    return t * t * t * (t * (t * 6.0F - 15.0F) + 10.0F);
}

inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

// Dot product of one of the twelve cube-edge gradients with (x, y, z).
inline float corner(std::uint32_t hash, float x, float y, float z) {
    switch (hash % 12U) {
    case 0U: return x + y;
    case 1U: return -x + y;
    case 2U: return x - y;
    case 3U: return -x - y;
    case 4U: return x + z;
    case 5U: return -x + z;
    case 6U: return x - z;
    case 7U: return -x - z;
    case 8U: return y + z;
    case 9U: return -y + z;
    case 10U: return y - z;
    default: return -y - z;
    }
}

// Gradient noise in roughly [-1, 1], zero at lattice points.
inline float gradient_noise(Vec3 p, std::uint32_t seed) {
    const float fx = std::floor(p.x);
    const float fy = std::floor(p.y);
    const float fz = std::floor(p.z);
    const auto ix = static_cast<std::uint32_t>(static_cast<std::int32_t>(fx));
    const auto iy = static_cast<std::uint32_t>(static_cast<std::int32_t>(fy));
    const auto iz = static_cast<std::uint32_t>(static_cast<std::int32_t>(fz));
    const float x = p.x - fx;
    const float y = p.y - fy;
    const float z = p.z - fz;

    const auto h = [seed, ix, iy, iz](std::uint32_t dx, std::uint32_t dy, std::uint32_t dz) {
        return hash_mix(hash_mix(hash_mix(seed, ix + dx), iy + dy), iz + dz);
    };
    const float c000 = corner(h(0U, 0U, 0U), x, y, z);
    const float c100 = corner(h(1U, 0U, 0U), x - 1.0F, y, z);
    const float c010 = corner(h(0U, 1U, 0U), x, y - 1.0F, z);
    const float c110 = corner(h(1U, 1U, 0U), x - 1.0F, y - 1.0F, z);
    const float c001 = corner(h(0U, 0U, 1U), x, y, z - 1.0F);
    const float c101 = corner(h(1U, 0U, 1U), x - 1.0F, y, z - 1.0F);
    const float c011 = corner(h(0U, 1U, 1U), x, y - 1.0F, z - 1.0F);
    const float c111 = corner(h(1U, 1U, 1U), x - 1.0F, y - 1.0F, z - 1.0F);

    const float u = fade(x);
    const float v = fade(y);
    const float w = fade(z);
    return lerp(lerp(lerp(c000, c100, u), lerp(c010, c110, u), v),
                lerp(lerp(c001, c101, u), lerp(c011, c111, u), v), w);
}

// Curl of a vector potential made of three independent noise fields, by
// central differences. Divergence-free up to the difference error.
inline Vec3 curl_noise(Vec3 p, std::uint32_t seed) {
    constexpr float e = 1.0e-2F;
    constexpr float inv = 1.0F / (2.0F * e);
    const std::uint32_t s1 = hash_mix(seed, 1U);
    const std::uint32_t s2 = hash_mix(seed, 2U);
    const std::uint32_t s3 = hash_mix(seed, 3U);
    const Vec3 dx{e, 0.0F, 0.0F};
    const Vec3 dy{0.0F, e, 0.0F};
    const Vec3 dz{0.0F, 0.0F, e};

    const float d3_dy = (gradient_noise(p + dy, s3) - gradient_noise(p - dy, s3)) * inv;
    const float d2_dz = (gradient_noise(p + dz, s2) - gradient_noise(p - dz, s2)) * inv;
    const float d1_dz = (gradient_noise(p + dz, s1) - gradient_noise(p - dz, s1)) * inv;
    const float d3_dx = (gradient_noise(p + dx, s3) - gradient_noise(p - dx, s3)) * inv;
    const float d2_dx = (gradient_noise(p + dx, s2) - gradient_noise(p - dx, s2)) * inv;
    const float d1_dy = (gradient_noise(p + dy, s1) - gradient_noise(p - dy, s1)) * inv;
    return {d3_dy - d2_dz, d1_dz - d3_dx, d2_dx - d1_dy};
}

} // namespace ajfx::detail
