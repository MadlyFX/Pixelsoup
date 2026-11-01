#pragma once

#include <ajfx/math.hpp>

#include <array>
#include <string>
#include <utility>

// Published parameters: the values an effect lets its host set and animate
// (spawn rate, tint, wind...). A module input is either a constant baked into
// the effect or bound to one of these by name.
namespace ajfx {

enum class ParameterType {
    Float,
    Vec3,
    Color,
};

// Every parameter value is four floats; Float uses the first, Vec3 the first
// three. One shape keeps the host interface and the GPU constant layout flat.
using ParameterValue = std::array<float, 4>;

[[nodiscard]] constexpr ParameterValue to_value(float value) noexcept { return {value, 0.0F, 0.0F, 0.0F}; }
[[nodiscard]] constexpr ParameterValue to_value(Vec3 value) noexcept { return {value.x, value.y, value.z, 0.0F}; }
[[nodiscard]] constexpr ParameterValue to_value(Color value) noexcept { return {value.r, value.g, value.b, value.a}; }

struct ParameterDesc final {
    std::string name;
    ParameterType type{ParameterType::Float};
    ParameterValue default_value{};
    // Applied to every component. Values a host supplies are clamped into the
    // range, which is what lets an effect's particle budget be checked once,
    // when it is built, instead of failing mid-show.
    float min{0.0F};
    float max{1.0F};

    friend bool operator==(const ParameterDesc&, const ParameterDesc&) = default;
};

struct Binding final {
    std::string parameter;
};

// `ajfx::bind("rate")` in place of a constant binds a module input.
[[nodiscard]] inline Binding bind(std::string parameter) {
    return Binding{std::move(parameter)};
}

template <class T>
struct Input final {
    T value{};
    // Empty for a constant.
    std::string parameter;

    Input() = default;
    Input(T constant) : value{constant} {}
    Input(Binding binding) : parameter{std::move(binding.parameter)} {}

    [[nodiscard]] bool bound() const noexcept { return !parameter.empty(); }

    friend bool operator==(const Input&, const Input&) = default;
};

using FloatInput = Input<float>;
using Vec3Input = Input<Vec3>;
using ColorInput = Input<Color>;

} // namespace ajfx
