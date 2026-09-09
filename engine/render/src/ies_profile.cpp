#include <mediaserver/render/ies_profile.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <vector>

namespace mediaserver::render {
namespace {
constexpr float pi = 3.14159265358979323846F;
constexpr std::size_t max_bytes = 8U * 1024U * 1024U;
struct Numbers {
    std::string_view text;
    bool number(float& value) {
        const auto start = text.find_first_not_of(" \t\r\n,");
        if (start == std::string_view::npos) return false;
        text.remove_prefix(start);
        if (text.front() == '+') text.remove_prefix(1);
        const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
        if (result.ec != std::errc{} || !std::isfinite(value)) return false;
        text.remove_prefix(static_cast<std::size_t>(result.ptr - text.data()));
        return text.empty() || text.find_first_of(" \t\r\n,") == 0;
    }
};
float interpolate(float a, float b, float t) noexcept { return a + (b - a) * t; }
struct Interval { std::size_t a{}, b{}; float t{}; };
Interval interval(const std::vector<float>& angles, float angle) {
    const auto it = std::lower_bound(angles.begin(), angles.end(), angle);
    const auto b = static_cast<std::size_t>(it - angles.begin());
    if (b == 0) return {};
    if (b == angles.size()) return {b - 1, b - 1, 0};
    return {b - 1, b, (angle - angles[b - 1]) / (angles[b] - angles[b - 1])};
}
} // namespace

const char* ies_error_label(IesError error) noexcept {
    switch (error) {
    case IesError::None: return "IES profile ready";
    case IesError::Read: return "IES file could not be read (8 MiB maximum)";
    case IesError::Format: return "IES file is malformed or has non-finite values";
    case IesError::Tilt: return "IES requires TILT=NONE";
    case IesError::PhotometricType: return "IES requires Type C photometry";
    case IesError::Angles: return "IES angle coverage or ordering is unsupported";
    case IesError::Capacity: return "IES profile or path exceeds fixed capacity";
    case IesError::Dark: return "IES profile contains no emitted light";
    }
    return "IES profile failed";
}

IesResult parse_ies_profile(std::string_view text) {
    if (text.size() > max_bytes) return {{}, IesError::Capacity};
    // Accept the published 1991, 1995, 2002 and 2019 identifiers and the
    // unlabelled 1986 form. TILT must begin a line, never occur in a label.
    std::size_t data = std::string_view::npos;
    while (!text.empty()) {
        const auto end = text.find('\n');
        auto line = text.substr(0, end);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.starts_with("TILT=")) {
            if (line != "TILT=NONE") return {{}, IesError::Tilt};
            data = end;
            break;
        }
        if (end == std::string_view::npos) break;
        text.remove_prefix(end + 1);
    }
    if (data == std::string_view::npos) return {{}, IesError::Format};
    Numbers input{text.substr(data + 1)};
    std::array<float, 13> header{};
    for (auto& n : header) if (!input.number(n)) return {{}, IesError::Format};
    if (header[5] != 1) return {{}, IesError::PhotometricType};
    if (header[3] < 2 || header[3] > 721 || header[4] < 1 || header[4] > 1441)
        return {{}, IesError::Capacity};
    if (std::floor(header[3]) != header[3] || std::floor(header[4]) != header[4]
        || header[0] < 1 || std::floor(header[0]) != header[0]
        || header[2] <= 0 || (header[6] != 1 && header[6] != 2)
        || header[10] <= 0 || header[11] <= 0 || header[12] < 0)
        return {{}, IesError::Format};
    const auto nv = static_cast<std::size_t>(header[3]);
    const auto nh = static_cast<std::size_t>(header[4]);
    std::vector<float> vertical(nv), horizontal(nh), values(nv * nh);
    for (auto* angles : {&vertical, &horizontal}) {
        for (auto& angle : *angles) if (!input.number(angle)) return {{}, IesError::Format};
        for (std::size_t i = 1; i < angles->size(); ++i)
            if ((*angles)[i] <= (*angles)[i - 1]) return {{}, IesError::Angles};
    }
    const bool vertical_ok = (vertical.front() == 0 && (vertical.back() == 90 || vertical.back() == 180))
        || (vertical.front() == 90 && vertical.back() == 180);
    const bool horizontal_ok = (horizontal.front() == 0 &&
        ((nh == 1) || horizontal.back() == 90 || horizontal.back() == 180 || horizontal.back() == 360))
        || (horizontal.front() == 90 && horizontal.back() == 270);
    if (!vertical_ok || !horizontal_ok) return {{}, IesError::Angles};
    float peak = 0;
    for (auto& v : values) {
        if (!input.number(v) || v < 0) return {{}, IesError::Format};
        peak = std::max(peak, v);
    }
    if (input.text.find_first_not_of(" \t\r\n,") != std::string_view::npos)
        return {{}, IesError::Format};
    if (peak <= 0) return {{}, IesError::Dark};
    // Common candela/ballast multipliers cancel in peak normalization. Lamp
    // lumens and watts are metadata: authored intensity controls emission.
    for (auto& v : values) v /= peak;
    auto profile = std::make_shared<IesProfile>();
    for (core::u32 h = 0; h < ies_horizontal_samples; ++h) {
        float phi = static_cast<float>(h == 360 ? 0 : h);
        if (nh == 1) phi = 0;
        else if (horizontal.front() == 90) {
            if (phi < 90) phi = 180 - phi;
            if (phi > 270) phi = 540 - phi;
        } else if (horizontal.back() == 90) {
            if (phi > 180) phi = 360 - phi;
            if (phi > 90) phi = 180 - phi;
        } else if (horizontal.back() == 180 && phi > 180) phi = 360 - phi;
        const auto hi = interval(horizontal, phi);
        for (core::u32 v = 0; v < ies_vertical_samples; ++v) {
            const float theta = static_cast<float>(v);
            float sample = 0;
            if (theta >= vertical.front() && theta <= vertical.back()) {
                const auto vi = interval(vertical, theta);
                sample = interpolate(interpolate(values[hi.a * nv + vi.a], values[hi.a * nv + vi.b], vi.t),
                    interpolate(values[hi.b * nv + vi.a], values[hi.b * nv + vi.b], vi.t), hi.t);
            }
            profile->values[h * ies_vertical_samples + v] = sample;
            if (h < 360) profile->azimuth_mean[v] += sample / 360.0F;
        }
    }
    for (core::u32 i = 0; i <= 720U; ++i) {
        const float degrees = static_cast<float>(i) * 0.25F;
        const auto row = std::min(i / 4U, 179U);
        profile->ring_mean[i] = interpolate(profile->azimuth_mean[row], profile->azimuth_mean[row + 1U],
                                            degrees - static_cast<float>(row));
        profile->ring_cosine[i] = scene::deterministic::cos_turns(degrees / 360.0F);
        if (i != 0U)
            profile->solid_angle += (profile->ring_mean[i - 1U] + profile->ring_mean[i]) * 0.5F
                * (profile->ring_cosine[i - 1U] - profile->ring_cosine[i]) * (2 * pi);
    }
    return {std::move(profile), IesError::None};
}

IesResult load_ies_profile(const std::string& source) {
    try {
        std::ifstream file{std::filesystem::path{std::u8string(source.begin(), source.end())},
                           std::ios::binary | std::ios::ate};
        if (!file) return {{}, IesError::Read};
        const auto size = file.tellg();
        if (size <= 0 || static_cast<std::uintmax_t>(size) > max_bytes) return {{}, IesError::Read};
        std::string text(static_cast<std::size_t>(size), '\0');
        file.seekg(0);
        if (!file.read(text.data(), static_cast<std::streamsize>(text.size()))) return {{}, IesError::Read};
        return parse_ies_profile(text);
    } catch (...) { return {{}, IesError::Read}; }
}

float sample_ies_profile(const IesProfile& p, float theta, float phi) noexcept {
    if (!std::isfinite(theta) || !std::isfinite(phi)) return 0;
    theta = std::clamp(theta, 0.0F, 180.0F);
    phi -= std::floor(phi / 360.0F) * 360.0F;
    const auto x = std::min(static_cast<core::u32>(theta), 179U);
    const auto y = std::min(static_cast<core::u32>(phi), 359U);
    const float fx = theta - static_cast<float>(x), fy = phi - static_cast<float>(y);
    const auto at = y * ies_vertical_samples + x;
    return interpolate(interpolate(p.values[at], p.values[at + 1], fx),
        interpolate(p.values[at + ies_vertical_samples], p.values[at + ies_vertical_samples + 1], fx), fy);
}

scene::Vec3 ies_tangent(const scene::PointLight& light) noexcept {
    const auto axis = scene::normalize(light.direction, {0, -1, 0});
    const auto reference = std::abs(axis.x) < 0.99F ? scene::Vec3{1, 0, 0} : scene::Vec3{0, 0, 1};
    const auto tangent = scene::normalize(reference - axis * scene::dot(reference, axis), {1, 0, 0});
    const auto bitangent = scene::cross(axis, tangent);
    return tangent * scene::deterministic::cos_turns(light.ies_rotation)
        + bitangent * scene::deterministic::sin_turns(light.ies_rotation);
}

float ies_weight(const IesProfile& profile, const scene::PointLight& light, scene::Vec3 from_light) noexcept {
    const auto d = scene::normalize(from_light, {0, -1, 0});
    const auto axis = scene::normalize(light.direction, {0, -1, 0});
    const auto tangent = ies_tangent(light);
    const float theta = std::acos(std::clamp(scene::dot(d, axis), -1.0F, 1.0F)) * (180 / pi);
    const float phi = std::atan2(scene::dot(d, scene::cross(axis, tangent)), scene::dot(d, tangent)) * (180 / pi);
    return sample_ies_profile(profile, theta, phi);
}

scene::LightEmission ies_light_emission(const scene::PointLight& light, float scale,
                                       const IesProfile* profile) noexcept {
    auto result = scene::light_emission(light, scale);
    if (!profile || light.unit != scene::LightUnit::Lumens) return result;
    float integral = profile->solid_angle;
    if (light.kind == scene::LocalLightKind::Spot) {
        integral = 0;
        const float inner = scene::deterministic::cos_turns(light.inner_cone);
        const float outer = scene::deterministic::cos_turns(light.outer_cone);
        // Integrate clipped rings in cosine (uniform solid angle). The web is
        // piecewise linear over quarter-degree rings. Split at both authored
        // cone limits so even a sub-degree hard cone preserves its flux.
        for (core::u32 i = 0; i < 720U && profile->ring_cosine[i] > outer; ++i) {
            const float high = profile->ring_cosine[i], low = profile->ring_cosine[i + 1U];
            if (high <= low) continue;
            const auto mean_at = [&](float mu) {
                return interpolate(profile->ring_mean[i + 1U], profile->ring_mean[i], (mu - low) / (high - low));
            };
            const float cap_low = std::max(low, inner);
            if (high > cap_low)
                integral += (mean_at(high) + mean_at(cap_low)) * 0.5F * (high - cap_low);
            const float a = std::max(low, outer), b = std::min(high, inner);
            if (b <= a) continue;
            // Three-point Gauss quadrature exactly integrates this linear web
            // times the shader's cubic smoothstep. Normalized t avoids the
            // cancellation of subtracting two near-equal antiderivatives.
            const float ta = (a - outer) / (inner - outer), tb = (b - outer) / (inner - outer);
            const float ia = mean_at(a), ib = mean_at(b);
            const auto f = [&](float x) {
                const float t = interpolate(ta, tb, x);
                return interpolate(ia, ib, x) * t * t * (3.0F - 2.0F * t);
            };
            integral += (b - a) * ((5.0F / 18.0F) * (f(0.1127016654F) + f(0.8872983346F)) + (4.0F / 9.0F) * f(0.5F));
        }
        integral *= 2 * pi;
    }
    result.intensity = integral > 1e-8F ? light.intensity * scale / integral : 0.0F;
    return result;
}
} // namespace mediaserver::render
