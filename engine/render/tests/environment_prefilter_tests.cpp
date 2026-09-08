#include <mediaserver/render/environment_prefilter.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace core = mediaserver::core;
namespace render = mediaserver::render;
namespace rhi = mediaserver::rhi;

void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{std::string{"line "} + std::to_string(line) +
                                 ": check failed: " + expression};
    }
}

#define IBL_CHECK(...)                                                         \
    check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

[[nodiscard]] core::u16 half_from(float value) noexcept {
    // Only the handful of exact values this test uses, spelled out rather than
    // reimplementing the converter under test's sibling.
    if (value == 0.0F) {
        return 0x0000U;
    }
    if (value == 0.5F) {
        return 0x3800U;
    }
    if (value == 1.0F) {
        return 0x3C00U;
    }
    if (value == 2.0F) {
        return 0x4000U;
    }
    return 0x3C00U;
}

[[nodiscard]] float float_from_half(core::u16 bits) noexcept {
    const core::u32 sign = static_cast<core::u32>(bits & 0x8000U) << 16U;
    const core::u32 exponent = (bits >> 10U) & 0x1FU;
    const core::u32 mantissa = bits & 0x3FFU;
    core::u32 result{};
    if (exponent == 0U) {
        if (mantissa == 0U) {
            result = sign;
        } else {
            // Subnormal half; normalize it into a float.
            core::u32 shifted = mantissa;
            core::u32 shift = 0U;
            while ((shifted & 0x400U) == 0U) {
                shifted <<= 1U;
                ++shift;
            }
            shifted &= 0x3FFU;
            result = sign | ((127U - 15U - shift + 1U) << 23U)
                | (shifted << 13U);
        }
    } else if (exponent == 0x1FU) {
        result = sign | 0x7F80'0000U | (mantissa << 13U);
    } else {
        result = sign | ((exponent + 127U - 15U) << 23U) | (mantissa << 13U);
    }
    float value{};
    std::memcpy(&value, &result, sizeof(value));
    return value;
}

struct Environment final {
    rhi::Device device;
    render::EnvironmentPrefilter prefilter;
};

// A one-texel equirectangular map of a single radiance value. Every direction
// in the sphere therefore carries the same radiance, which is what makes the
// diffuse integral below analytic.
[[nodiscard]] rhi::Texture constant_equirect(
    rhi::Device& device, rhi::CommandList& commands, float value) {
    rhi::TextureDesc desc{};
    desc.width = 4U;
    desc.height = 2U;
    desc.format = rhi::TextureFormat::Rgba16Float;
    desc.sampled = true;
    auto created = device.create_texture(desc);
    check(created.has_value(), "equirect creation", __LINE__);

    const core::u16 half = half_from(value);
    const core::u16 one = half_from(1.0F);
    std::vector<core::u8> pixels(
        static_cast<std::size_t>(desc.width) * desc.height * 4U
        * sizeof(core::u16));
    for (std::size_t texel = 0U;
         texel < static_cast<std::size_t>(desc.width) * desc.height; ++texel) {
        const std::array<core::u16, 4U> rgba{half, half, half, one};
        std::memcpy(pixels.data() + (texel * 4U * sizeof(core::u16)),
                    rgba.data(), rgba.size() * sizeof(core::u16));
    }
    check(commands.upload(created.value(), pixels), "equirect upload",
          __LINE__);
    return std::move(created).value();
}

[[nodiscard]] render::EnvironmentPrefilterLimits small_limits() noexcept {
    render::EnvironmentPrefilterLimits limits{};
    limits.face_size = 32U;
    limits.specular_mips = 3U;
    limits.irradiance_face_size = 32U;
    limits.brdf_extent = 32U;
    limits.specular_samples = 32U;
    limits.brdf_samples = 64U;
    return limits;
}

// The diffuse integral is exact for a constant environment.
//
// An analytic check rather than a golden image, because a golden image proves
// only that nothing changed - it cannot tell a correct convolution from a
// consistently wrong one. For a uniform radiance L over the whole sphere, the
// zeroth spherical-harmonic coefficient is L * Y0 * 4pi, and every other
// coefficient is zero because the basis functions above band zero integrate to
// nothing over a constant.
//
// This is what catches a missing solid-angle weight: without it the corners of
// each cube face are over-counted and the result is several percent high, which
// looks like a slightly bright environment rather than like a bug.
void a_constant_environment_integrates_to_its_own_radiance() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)
        || !render::EnvironmentPrefilter::shaders_available()) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    IBL_CHECK(opened);
    auto device = std::move(opened).value();

    auto created = render::EnvironmentPrefilter::create(device, small_limits());
    IBL_CHECK(created);
    auto prefilter = std::move(created).value();

    constexpr float radiance = 2.0F;
    constexpr core::u32 coefficient_bytes = 9U * 4U * sizeof(float);
    auto readback = device.create_readback(coefficient_bytes);
    IBL_CHECK(readback);

    auto acquired = device.acquire_commands();
    IBL_CHECK(acquired);
    auto commands = std::move(acquired).value();
    const rhi::Texture equirect =
        constant_equirect(device, commands, radiance);
    IBL_CHECK(prefilter.prepare(commands));
    IBL_CHECK(prefilter.record(commands, equirect, 0.0F));
    const render::EnvironmentMaps maps = prefilter.maps();
    IBL_CHECK(maps.complete());
    IBL_CHECK(commands.download(*maps.irradiance, readback.value()));
    auto submitted = device.submit(std::move(commands));
    IBL_CHECK(submitted);
    IBL_CHECK(std::move(submitted).value().wait());

    const auto mapped = readback.value().map();
    IBL_CHECK(mapped.size() >= coefficient_bytes);
    std::array<float, 36U> coefficients{};
    std::memcpy(coefficients.data(), mapped.data(), coefficient_bytes);
    readback.value().unmap();

    constexpr float pi = 3.14159265358979323846F;
    constexpr float y0 = 0.282095F;
    const float expected = radiance * y0 * 4.0F * pi;
    // Loose enough for a 32x32 face and 6144 float additions, tight enough
    // that a missing solid-angle weight - worth several percent - fails.
    IBL_CHECK(std::abs(coefficients[0U] - expected) < expected * 0.01F);
    IBL_CHECK(std::abs(coefficients[1U] - expected) < expected * 0.01F);
    IBL_CHECK(std::abs(coefficients[2U] - expected) < expected * 0.01F);

    // Every band above zero integrates to nothing over a constant environment.
    for (std::size_t slot = 1U; slot < 9U; ++slot) {
        for (std::size_t channel = 0U; channel < 3U; ++channel) {
            IBL_CHECK(std::abs(coefficients[(slot * 4U) + channel])
                      < expected * 0.02F);
        }
    }
}

// The same source prefilters to the same bits, twice, on two instances.
//
// Two nodes each build their own environment from the same map. If the results
// differ at all they light the same subject differently, and the seam appears
// on whichever surface is most reflective - which on a virtual production stage
// is usually the one the shot is about.
void prefiltering_is_byte_identical_across_instances() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)
        || !render::EnvironmentPrefilter::shaders_available()) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    IBL_CHECK(opened);
    auto device = std::move(opened).value();

    const render::EnvironmentPrefilterLimits limits = small_limits();
    const core::u32 face_bytes = limits.face_size * limits.face_size * 8U;

    const auto run = [&](std::vector<core::u8>& into) {
        auto created = render::EnvironmentPrefilter::create(device, limits);
        IBL_CHECK(created);
        auto prefilter = std::move(created).value();
        auto readback = device.create_readback(face_bytes);
        IBL_CHECK(readback);
        auto acquired = device.acquire_commands();
        IBL_CHECK(acquired);
        auto commands = std::move(acquired).value();
        const rhi::Texture equirect =
            constant_equirect(device, commands, 0.5F);
        IBL_CHECK(prefilter.prepare(commands));
        // A non-zero rotation, so the test covers the path an authored
        // environment actually takes rather than only the identity.
        IBL_CHECK(prefilter.record(commands, equirect, 0.125F));
        const render::EnvironmentMaps maps = prefilter.maps();
        IBL_CHECK(maps.complete());
        IBL_CHECK(commands.download(*maps.specular, readback.value()));
        auto submitted = device.submit(std::move(commands));
        IBL_CHECK(submitted);
        IBL_CHECK(std::move(submitted).value().wait());
        const auto mapped = readback.value().map();
        IBL_CHECK(mapped.size() >= face_bytes);
        into.assign(mapped.begin(), mapped.begin() + face_bytes);
        readback.value().unmap();
    };

    std::vector<core::u8> first;
    std::vector<core::u8> second;
    run(first);
    run(second);
    IBL_CHECK(first == second);

    // And the result is the environment, not an empty target: a constant
    // environment convolves to that constant whatever the roughness.
    core::u16 bits{};
    std::memcpy(&bits, first.data(), sizeof(bits));
    IBL_CHECK(std::abs(float_from_half(bits) - 0.5F) < 0.02F);
}

// A configuration this component cannot honour is refused at construction,
// where a caller can still do something about it, rather than at the first
// dispatch of a frame.
void an_impossible_prefilter_configuration_is_refused() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)
        || !render::EnvironmentPrefilter::shaders_available()) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    IBL_CHECK(opened);
    auto device = std::move(opened).value();

    render::EnvironmentPrefilterLimits zero = small_limits();
    zero.face_size = 0U;
    IBL_CHECK(!render::EnvironmentPrefilter::create(device, zero));

    // More mips than the face has levels: the bottom of the chain would name a
    // level with no texels in it.
    render::EnvironmentPrefilterLimits deep = small_limits();
    deep.face_size = 8U;
    deep.specular_mips = 9U;
    const auto refused = render::EnvironmentPrefilter::create(device, deep);
    IBL_CHECK(!refused);
    IBL_CHECK(refused.error() == render::RenderError::InvalidConfiguration);

    render::EnvironmentPrefilterLimits none = small_limits();
    none.specular_samples = 0U;
    IBL_CHECK(!render::EnvironmentPrefilter::create(device, none));
}

// Roughness across the chain is stated once, because the lit shader has to
// invert it to pick a mip and two definitions of that mapping is one too many.
void mip_roughness_spans_mirror_to_rough() {
    IBL_CHECK(render::EnvironmentPrefilter::mip_roughness(0U, 5U) == 0.0F);
    IBL_CHECK(render::EnvironmentPrefilter::mip_roughness(4U, 5U) == 1.0F);
    IBL_CHECK(render::EnvironmentPrefilter::mip_roughness(2U, 5U) == 0.5F);
    // One level is a mirror and nothing else, rather than a division by zero.
    IBL_CHECK(render::EnvironmentPrefilter::mip_roughness(0U, 1U) == 0.0F);
}

// Nothing is offered until everything is recorded. A lit shader handed half a
// prefiltered environment produces a plausible wrong image, which is worse than
// falling back to the hemisphere it had before.
void an_incomplete_environment_offers_nothing() {
    if (!rhi::Device::available(rhi::BackendKind::D3d12)
        || !render::EnvironmentPrefilter::shaders_available()) {
        return;
    }
    rhi::DeviceConfig config{};
    config.backend = rhi::BackendKind::D3d12;
    auto opened = rhi::Device::create(config);
    IBL_CHECK(opened);
    auto device = std::move(opened).value();
    auto created = render::EnvironmentPrefilter::create(device, small_limits());
    IBL_CHECK(created);
    auto prefilter = std::move(created).value();

    IBL_CHECK(!prefilter.maps().complete());
    IBL_CHECK(!prefilter.brdf_ready());

    auto acquired = device.acquire_commands();
    IBL_CHECK(acquired);
    auto commands = std::move(acquired).value();
    IBL_CHECK(prefilter.prepare(commands));
    IBL_CHECK(prefilter.brdf_ready());
    // The table is there; the environment is not.
    IBL_CHECK(!prefilter.maps().complete());

    // A second prepare records nothing and still succeeds: the table depends on
    // nothing that changes.
    IBL_CHECK(prefilter.prepare(commands));

    // And an absent source is refused rather than producing an empty
    // environment that would read as a black room.
    const rhi::Texture missing;
    IBL_CHECK(!prefilter.record(commands, missing, 0.0F));
    IBL_CHECK(!prefilter.maps().complete());
}

struct Case final {
    const char* name;
    void (*function)();
};

constexpr std::array<Case, 5U> cases{{
    {"a constant environment integrates to its own radiance",
     &a_constant_environment_integrates_to_its_own_radiance},
    {"prefiltering is byte identical across instances",
     &prefiltering_is_byte_identical_across_instances},
    {"an impossible prefilter configuration is refused",
     &an_impossible_prefilter_configuration_is_refused},
    {"mip roughness spans mirror to rough",
     &mip_roughness_spans_mirror_to_rough},
    {"an incomplete environment offers nothing",
     &an_incomplete_environment_offers_nothing},
}};

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& entry : cases) {
        try {
            entry.function();
            std::cout << "[pass] " << entry.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[fail] " << entry.name << ": " << error.what()
                      << '\n';
        }
    }
    std::cout << (cases.size() - failures) << '/' << cases.size()
              << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
