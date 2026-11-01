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

// A unit quaternion turning a particle's own axes into the effect's: the
// vector part, then the scalar. The default turns nothing.
struct Quat final {
    float x{};
    float y{};
    float z{};
    float w{1.0F};

    friend constexpr bool operator==(const Quat&, const Quat&) = default;
};

// One turn after another: `a` applied to whatever `b` already turned.
[[nodiscard]] constexpr Quat times(Quat a, Quat b) noexcept {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// `v` turned by `q`, which is taken to be unit length.
[[nodiscard]] constexpr Vec3 rotate(Quat q, Vec3 v) noexcept {
    const Vec3 u{q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0F;
    return v + t * q.w + cross(u, t);
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
