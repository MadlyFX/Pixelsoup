#pragma once

#include <mediaserver/scene/lighting.hpp>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace mediaserver::render {

// One-degree Type C web, including both azimuth endpoints. Every GPU uses
// these same samples and four explicit loads, never hardware filtering.
inline constexpr core::u32 ies_vertical_samples = 181U;
inline constexpr core::u32 ies_horizontal_samples = 361U;
inline constexpr core::u32 max_ies_profiles = 16U;
inline constexpr std::size_t max_ies_source_bytes = 1024U;
inline constexpr std::size_t ies_profile_texels =
    ies_vertical_samples * ies_horizontal_samples;

struct IesProfile final {
    std::array<float, ies_profile_texels> values{};
    std::array<float, ies_vertical_samples> azimuth_mean{};
    // Worker-built quadrature. Packing a lumen light needs no angular math
    // beyond its two authored cone limits, and point flux is a single lookup.
    std::array<float, 721U> ring_mean{}, ring_cosine{};
    float solid_angle{};
};

enum class IesError : core::u8 {
    None, Read, Format, Tilt, PhotometricType, Angles, Capacity, Dark
};
[[nodiscard]] const char* ies_error_label(IesError error) noexcept;
struct IesResult final {
    std::shared_ptr<const IesProfile> profile;
    IesError error{IesError::None};
};
// Worker/offline only: bounded text, locale-independent numbers, finite and
// strictly increasing angle tables. Measured intensities become peak-one.
[[nodiscard]] IesResult parse_ies_profile(std::string_view text);
[[nodiscard]] IesResult load_ies_profile(const std::string& source);
[[nodiscard]] float sample_ies_profile(const IesProfile& profile,
                                      float vertical_degrees,
                                      float horizontal_degrees) noexcept;
[[nodiscard]] float ies_weight(const IesProfile& profile,
                               const scene::PointLight& light,
                               scene::Vec3 from_light) noexcept;
[[nodiscard]] scene::Vec3 ies_tangent(const scene::PointLight& light) noexcept;
// Candela is peak intensity; lumens is total flux after IES and spot clipping.
[[nodiscard]] scene::LightEmission ies_light_emission(
    const scene::PointLight& light, float scale, const IesProfile* profile) noexcept;

enum class IesCacheState : core::u8 { Unseen, Pending, Ready, Failed, Busy, Capacity };
struct IesObservation final {
    std::shared_ptr<const IesProfile> profile;
    IesCacheState state{IesCacheState::Unseen};
    IesError error{IesError::None};
    core::u64 generation{}, publication_epoch{};
};
struct IesCacheStats final {
    core::u64 admissions{}, failures{}, late_results{}, busy{}, refused{}, overwrites{};
};
class IesCache final {
public:
    IesCache();
    [[nodiscard]] IesObservation observe(std::string_view source,
        core::u64 fence = 0U, core::u64 authored_generation = 0U) noexcept;
    [[nodiscard]] bool refresh(std::string_view source) noexcept;
    [[nodiscard]] core::u64 publication_epoch() const noexcept;
    [[nodiscard]] IesCacheStats stats() const noexcept;
    void wait_for_test() const;
    void set_next_load_gate_for_test(std::shared_ptr<const std::function<void()>> gate);
private:
    struct State;
    std::shared_ptr<State> state_;
};

} // namespace mediaserver::render
