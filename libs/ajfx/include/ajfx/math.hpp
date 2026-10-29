#pragma once

// Plain value types. ajfx does not adopt a host's math library; hosts convert
// at the boundary, which is a handful of floats.
namespace ajfx {

struct Vec3 final {
    float x{};
    float y{};
    float z{};

    friend constexpr bool operator==(const Vec3&, const Vec3&) = default;
    friend constexpr Vec3 operator+(Vec3 a, Vec3 b) noexcept { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
    friend constexpr Vec3 operator-(Vec3 a, Vec3 b) noexcept { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
    friend constexpr Vec3 operator*(Vec3 a, float s) noexcept { return {a.x * s, a.y * s, a.z * s}; }
    constexpr Vec3& operator+=(Vec3 b) noexcept { return *this = *this + b; }
    constexpr Vec3& operator-=(Vec3 b) noexcept { return *this = *this - b; }
};

[[nodiscard]] constexpr float dot(Vec3 a, Vec3 b) noexcept {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

[[nodiscard]] constexpr Vec3 cross(Vec3 a, Vec3 b) noexcept {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

// Linear, unpremultiplied RGBA. Values above 1 are allowed (HDR emission).
struct Color final {
    float r{1.0F};
    float g{1.0F};
    float b{1.0F};
    float a{1.0F};

    friend constexpr bool operator==(const Color&, const Color&) = default;
};

} // namespace ajfx
