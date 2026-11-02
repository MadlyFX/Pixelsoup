#include <ajfx/cache.hpp>

#include <ajfx/package.hpp>
#include <ajfx/mesh.hpp>

#include "curves.hpp"
#include "step_inputs.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <numbers>
#include <string>
#include <utility>
#include <variant>

namespace ajfx {
namespace {

constexpr std::uint8_t state_magic[4] = {'A', 'J', 'F', 'S'};
constexpr std::uint8_t cache_magic[4] = {'A', 'J', 'F', 'C'};

constexpr std::uint64_t fnv_basis = 0xcbf29ce484222325ULL;

std::uint64_t fnv1a(std::span<const std::uint8_t> bytes, std::uint64_t hash = fnv_basis) {
    for (const std::uint8_t byte : bytes) {
        hash ^= byte;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

std::uint64_t fnv_text(std::string_view text, std::uint64_t hash = fnv_basis) {
    return fnv1a(std::span{reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}, hash);
}

std::uint64_t fnv_u64(std::uint64_t value, std::uint64_t hash) {
    std::uint8_t bytes[8];
    for (std::uint32_t i = 0U; i < 8U; ++i) {
        bytes[i] = static_cast<std::uint8_t>(value >> (8U * i));
    }
    return fnv1a(bytes, hash);
}

std::string quoted(std::string_view text) {
    return "'" + std::string{text} + "'";
}

Error malformed(const std::string& what) {
    return Error{ErrorCode::MalformedCache, what};
}

// ------------------------------------------------------------ bytes

// Little-endian, floats by their bits, so equal values give equal bytes on
// every machine.
class Writer final {
public:
    std::vector<std::uint8_t> out;

    void u8(std::uint8_t v) { out.push_back(v); }
    void u16(std::uint16_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8U));
    }
    void u32(std::uint32_t v) {
        for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
            out.push_back(static_cast<std::uint8_t>(v >> shift));
        }
    }
    void u64(std::uint64_t v) {
        for (std::uint32_t shift = 0U; shift < 64U; shift += 8U) {
            out.push_back(static_cast<std::uint8_t>(v >> shift));
        }
    }
    void i16(std::int16_t v) { u16(static_cast<std::uint16_t>(v)); }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    void f64(double v) { u64(std::bit_cast<std::uint64_t>(v)); }
    void vec3(Vec3 v) {
        f32(v.x);
        f32(v.y);
        f32(v.z);
    }
    void quat(Quat q) {
        f32(q.x);
        f32(q.y);
        f32(q.z);
        f32(q.w);
    }
    void color(Color c) {
        f32(c.r);
        f32(c.g);
        f32(c.b);
        f32(c.a);
    }
    void str(std::string_view s) {
        u32(static_cast<std::uint32_t>(s.size()));
        out.insert(out.end(), s.begin(), s.end());
    }
    void bytes(std::span<const std::uint8_t> b) { out.insert(out.end(), b.begin(), b.end()); }
};

// Every read is checked; the first that runs out marks the reader failed and
// every later read fails too, so a caller checks once at the end.
class Reader final {
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_{bytes} {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return ok_ ? bytes_.size() - at_ : 0U; }
    // Whether `count` items of `size` bytes could still be read, so a count
    // read from a damaged file cannot ask for an absurd allocation.
    [[nodiscard]] bool fits(std::uint64_t count, std::size_t size) {
        if (!ok_ || (size != 0U && count > remaining() / size)) {
            ok_ = false;
        }
        return ok_;
    }

    std::uint8_t u8() {
        std::uint8_t v = 0U;
        take(&v, 1U);
        return v;
    }
    std::uint16_t u16() {
        std::uint8_t b[2]{};
        take(b, 2U);
        return static_cast<std::uint16_t>(b[0] | (b[1] << 8U));
    }
    std::uint32_t u32() {
        std::uint8_t b[4]{};
        take(b, 4U);
        std::uint32_t v = 0U;
        for (std::uint32_t i = 0U; i < 4U; ++i) {
            v |= static_cast<std::uint32_t>(b[i]) << (8U * i);
        }
        return v;
    }
    std::uint64_t u64() {
        std::uint8_t b[8]{};
        take(b, 8U);
        std::uint64_t v = 0U;
        for (std::uint32_t i = 0U; i < 8U; ++i) {
            v |= static_cast<std::uint64_t>(b[i]) << (8U * i);
        }
        return v;
    }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    float f32() { return std::bit_cast<float>(u32()); }
    double f64() { return std::bit_cast<double>(u64()); }
    Vec3 vec3() {
        const float x = f32();
        const float y = f32();
        const float z = f32();
        return {x, y, z};
    }
    Quat quat() {
        const float x = f32();
        const float y = f32();
        const float z = f32();
        const float w = f32();
        return {x, y, z, w};
    }
    Color color() {
        const float r = f32();
        const float g = f32();
        const float b = f32();
        const float a = f32();
        return {r, g, b, a};
    }
    std::string str() {
        const std::uint32_t n = u32();
        if (!fits(n, 1U)) {
            return {};
        }
        std::string s(reinterpret_cast<const char*>(bytes_.data() + at_), n);
        at_ += n;
        return s;
    }
    std::span<const std::uint8_t> span(std::uint64_t n) {
        if (!fits(n, 1U)) {
            return {};
        }
        const std::span<const std::uint8_t> s = bytes_.subspan(at_, static_cast<std::size_t>(n));
        at_ += static_cast<std::size_t>(n);
        return s;
    }

private:
    void take(std::uint8_t* to, std::size_t n) {
        if (!ok_ || bytes_.size() - at_ < n) {
            ok_ = false;
            std::memset(to, 0, n);
            return;
        }
        std::memcpy(to, bytes_.data() + at_, n);
        at_ += n;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t at_{0U};
    bool ok_{true};
};

// Half precision, rounded to nearest even, for a cached particle's velocity.
std::uint16_t to_half(float value) {
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(value);
    const std::uint32_t sign = (bits >> 16U) & 0x8000U;
    const std::uint32_t exponent = (bits >> 23U) & 0xffU;
    std::uint32_t mantissa = bits & 0x7fffffU;
    if (exponent == 0xffU) {
        return static_cast<std::uint16_t>(sign | 0x7c00U | (mantissa != 0U ? 0x200U : 0U));
    }
    const std::int32_t e = static_cast<std::int32_t>(exponent) - 127 + 15;
    if (e >= 31) {
        return static_cast<std::uint16_t>(sign | 0x7bffU); // the largest finite half, not infinity
    }
    if (e <= 0) {
        if (e < -10) {
            return static_cast<std::uint16_t>(sign);
        }
        mantissa |= 0x800000U;
        const std::uint32_t shift = static_cast<std::uint32_t>(14 - e);
        std::uint32_t half = mantissa >> shift;
        const std::uint32_t rest = mantissa & ((1U << shift) - 1U);
        const std::uint32_t midway = 1U << (shift - 1U);
        if (rest > midway || (rest == midway && (half & 1U) != 0U)) {
            ++half;
        }
        return static_cast<std::uint16_t>(sign | half);
    }
    std::uint32_t half = (static_cast<std::uint32_t>(e) << 10U) | (mantissa >> 13U);
    const std::uint32_t rest = mantissa & 0x1fffU;
    if (rest > 0x1000U || (rest == 0x1000U && (half & 1U) != 0U)) {
        ++half; // may carry into the exponent, which is the right answer
    }
    if (half >= 0x7c00U) {
        half = 0x7bffU;
    }
    return static_cast<std::uint16_t>(sign | half);
}

float from_half(std::uint16_t half) {
    const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000U) << 16U;
    const std::uint32_t exponent = (half >> 10U) & 0x1fU;
    std::uint32_t mantissa = half & 0x3ffU;
    if (exponent == 0U) {
        if (mantissa == 0U) {
            return std::bit_cast<float>(sign);
        }
        std::int32_t e = -1;
        do {
            ++e;
            mantissa <<= 1U;
        } while ((mantissa & 0x400U) == 0U);
        const std::uint32_t bits = sign | (static_cast<std::uint32_t>(127 - 15 - e) << 23U) | ((mantissa & 0x3ffU) << 13U);
        return std::bit_cast<float>(bits);
    }
    if (exponent == 31U) {
        return std::bit_cast<float>(sign | 0x7f800000U | (mantissa << 13U));
    }
    return std::bit_cast<float>(sign | ((exponent - 15U + 127U) << 23U) | (mantissa << 13U));
}

// ------------------------------------------------------------ state

void write_particle(Writer& w, const EmitterState& s, std::size_t i);

void write_emitter(Writer& w, const EmitterState& s) {
    const std::size_t n = s.alive();
    w.u32(static_cast<std::uint32_t>(n));
    for (std::size_t i = 0U; i < n; ++i) {
        write_particle(w, s, i);
    }
    w.u32(static_cast<std::uint32_t>(s.body.size()));
    for (const std::uint32_t body : s.body) {
        w.u32(body);
    }
    w.u32(static_cast<std::uint32_t>(s.body_kind.size()));
    for (const std::uint32_t kind : s.body_kind) {
        w.u32(kind);
    }
    w.u32(static_cast<std::uint32_t>(s.joins.size()));
    for (const RigidJoin& join : s.joins) {
        w.u32(join.a);
        w.u32(join.b);
        w.vec3(join.at);
        w.quat(join.turn);
    }
    w.u32(static_cast<std::uint32_t>(s.solver_state.size()));
    w.bytes(s.solver_state);
    w.u32(static_cast<std::uint32_t>(s.props.size()));
    for (const RigidBodyState& prop : s.props) {
        w.vec3(prop.position);
        w.quat(prop.orientation);
        w.vec3(prop.velocity);
        w.vec3(prop.angular_velocity);
    }
    w.u64(s.spawned);
    w.u64(s.refused);
    w.u32(static_cast<std::uint32_t>(s.rate_carry.size()));
    for (const double carry : s.rate_carry) {
        w.f64(carry);
    }
    w.u32(static_cast<std::uint32_t>(s.spawn_counts.size()));
    for (const std::uint64_t count : s.spawn_counts) {
        w.u64(count);
    }
}

// One particle's whole record, the order write_emitter uses.
constexpr std::size_t particle_record_bytes = 4U * (3U + 3U + 1U + 1U + 4U + 4U + 1U + 1U + 1U + 4U + 3U + 1U + 1U);

void resize(EmitterState& s, std::size_t n) {
    s.position.resize(n);
    s.velocity.resize(n);
    s.age.resize(n);
    s.lifetime.resize(n);
    s.base_color.resize(n);
    s.color.resize(n);
    s.base_size.resize(n);
    s.size.resize(n);
    s.rotation.resize(n);
    s.orientation.resize(n);
    s.angular_velocity.resize(n);
    s.spin.resize(n);
    s.spawn_index.resize(n);
}

void read_particle(Reader& r, EmitterState& s, std::size_t i) {
    s.position[i] = r.vec3();
    s.velocity[i] = r.vec3();
    s.age[i] = r.f32();
    s.lifetime[i] = r.f32();
    s.base_color[i] = r.color();
    s.color[i] = r.color();
    s.base_size[i] = r.f32();
    s.size[i] = r.f32();
    s.rotation[i] = r.f32();
    s.orientation[i] = r.quat();
    s.angular_velocity[i] = r.vec3();
    s.spin[i] = r.f32();
    s.spawn_index[i] = r.u32();
}

void write_particle(Writer& w, const EmitterState& s, std::size_t i) {
    w.vec3(s.position[i]);
    w.vec3(s.velocity[i]);
    w.f32(s.age[i]);
    w.f32(s.lifetime[i]);
    w.color(s.base_color[i]);
    w.color(s.color[i]);
    w.f32(s.base_size[i]);
    w.f32(s.size[i]);
    w.f32(s.rotation[i]);
    w.quat(s.orientation[i]);
    w.vec3(s.angular_velocity[i]);
    w.f32(s.spin[i]);
    w.u32(s.spawn_index[i]);
}

bool read_emitter(Reader& r, EmitterState& s) {
    const std::uint32_t n = r.u32();
    if (n > max_emitter_capacity || !r.fits(n, particle_record_bytes)) {
        return false;
    }
    resize(s, n);
    for (std::size_t i = 0U; i < n; ++i) {
        read_particle(r, s, i);
    }
    const std::uint32_t bodies = r.u32();
    if ((bodies != 0U && bodies != n) || !r.fits(bodies, 4U)) {
        return false;
    }
    s.body.resize(bodies);
    for (std::uint32_t& body : s.body) {
        body = r.u32();
    }
    const std::uint32_t kinds = r.u32();
    if ((kinds != 0U && kinds != n) || !r.fits(kinds, 4U)) {
        return false;
    }
    s.body_kind.resize(kinds);
    for (std::uint32_t& kind : s.body_kind) {
        kind = r.u32();
    }
    const std::uint32_t joins = r.u32();
    if (joins > max_rigid_joins || !r.fits(joins, 36U)) {
        return false;
    }
    s.joins.resize(joins);
    for (RigidJoin& join : s.joins) {
        join.a = r.u32();
        join.b = r.u32();
        join.at = r.vec3();
        join.turn = r.quat();
    }
    const std::span<const std::uint8_t> solver = r.span(r.u32());
    s.solver_state.assign(solver.begin(), solver.end());
    const std::uint32_t props = r.u32();
    if (props > max_rigid_props || !r.fits(props, 52U)) {
        return false;
    }
    s.props.resize(props);
    for (RigidBodyState& prop : s.props) {
        prop.position = r.vec3();
        prop.orientation = r.quat();
        prop.velocity = r.vec3();
        prop.angular_velocity = r.vec3();
    }
    s.spawned = r.u64();
    s.refused = r.u64();
    const std::uint32_t carries = r.u32();
    if (!r.fits(carries, 8U)) {
        return false;
    }
    s.rate_carry.resize(carries);
    for (double& carry : s.rate_carry) {
        carry = r.f64();
    }
    const std::uint32_t sources = r.u32();
    if (sources > max_spawn_modules + 1U || !r.fits(sources, 8U)) {
        return false;
    }
    s.spawn_counts.resize(sources);
    for (std::uint64_t& count : s.spawn_counts) {
        count = r.u64();
    }
    return r.ok();
}

void write_state_into(Writer& w, const SimulationState& state) {
    w.bytes(state_magic);
    w.u32(state_format_version);
    w.u64(state.tick);
    w.u32(static_cast<std::uint32_t>(state.emitters.size()));
    for (const EmitterState& emitter : state.emitters) {
        write_emitter(w, emitter);
    }
}

// ------------------------------------------------------------ what a layer keeps

// What a cached particle layer stores of a particle that lives on, beyond
// its position: only what is drawn and cannot be worked out again.
constexpr std::uint8_t keeps_velocity = 1U << 0U;
constexpr std::uint8_t keeps_rotation = 1U << 1U;
constexpr std::uint8_t keeps_orientation = 1U << 2U;

std::uint8_t particle_keeps(const Emitter& emitter) {
    const std::uint32_t mask = required_attributes(emitter);
    std::uint8_t keeps = 0U;
    if (emitter.renderer.facing == Facing::Velocity && has_attribute(mask, Attribute::Velocity)) {
        keeps |= keeps_velocity;
    }
    if (has_attribute(mask, Attribute::Rotation)) {
        keeps |= keeps_rotation;
    }
    if (has_attribute(mask, Attribute::Orientation)) {
        keeps |= keeps_orientation;
    }
    return keeps;
}

// A cached particle's colour and size over its life, worked out again from
// its age exactly as the simulation's update pass does.
void particle_appearance(const Emitter& emitter, EmitterState& s, std::size_t first, std::size_t last) {
    for (const UpdateModule& module : emitter.update) {
        if (const auto* color = std::get_if<ColorOverLife>(&module)) {
            detail::color_over_life(*color, s, first, last);
        } else if (const auto* size = std::get_if<SizeOverLife>(&module)) {
            detail::size_over_life(*size, s, first, last);
        }
    }
    detail::glow(emitter, s, first, last);
}

// A rigid layer's bodies take their colour over life where the engine moves
// them (move_rigid), and no size over life: the body is the size it was born.
void body_appearance(const Emitter& emitter, EmitterState& s) {
    for (const UpdateModule& module : emitter.update) {
        if (const auto* color = std::get_if<ColorOverLife>(&module)) {
            detail::color_over_life(*color, s, 0U, s.alive());
        }
    }
    detail::glow(emitter, s, 0U, s.alive());
}

// What a body is for its whole life, kept once rather than every step.
struct BodyRecord final {
    std::uint32_t spawn_index{0U};
    std::uint32_t kind{0U};
    float size{0.0F};
    float base_size{0.0F};
    float lifetime{0.0F};
    Color base_color{};
    Color color{};

};

constexpr std::size_t body_record_bytes = 4U + 4U + 4U + 4U + 4U + 16U + 16U;
constexpr std::size_t body_frame_bytes = 4U + 12U + 16U + 4U;

constexpr std::uint8_t frame_colors = 1U << 0U;
constexpr std::uint8_t frame_sizes = 1U << 1U;

void write_body_frame(Writer& w, const EmitterState& s, std::uint8_t flags) {
    w.u8(flags);
    w.u64(s.spawned);
    w.u64(s.refused);
    w.u32(static_cast<std::uint32_t>(s.alive()));
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        w.u32(s.spawn_index[i]);
        w.vec3(s.position[i]);
        w.quat(s.orientation[i]);
        w.f32(s.age[i]);
    }
    if ((flags & frame_colors) != 0U) {
        for (const Color& color : s.color) {
            w.color(color);
        }
    }
    if ((flags & frame_sizes) != 0U) {
        for (const float size : s.size) {
            w.f32(size);
        }
    }
    w.u32(static_cast<std::uint32_t>(s.props.size()));
    for (const RigidBodyState& prop : s.props) {
        w.vec3(prop.position);
        w.quat(prop.orientation);
    }
}

const BodyRecord* find_body(const std::vector<BodyRecord>& table, std::uint32_t spawn_index) {
    const auto at = std::lower_bound(table.begin(), table.end(), spawn_index,
                                     [](const BodyRecord& r, std::uint32_t index) { return r.spawn_index < index; });
    return at != table.end() && at->spawn_index == spawn_index ? &*at : nullptr;
}

bool read_body_frame(Reader& r, const Emitter& emitter, const std::vector<BodyRecord>& table, EmitterState& s) {
    const std::uint8_t flags = r.u8();
    const std::uint64_t spawned = r.u64();
    const std::uint64_t refused = r.u64();
    const std::uint32_t n = r.u32();
    if (n > max_rigid_capacity || !r.fits(n, body_frame_bytes)) {
        return false;
    }
    s = EmitterState{};
    resize(s, n);
    s.body_kind.resize(n);
    s.spawned = spawned;
    s.refused = refused;
    for (std::size_t i = 0U; i < n; ++i) {
        s.spawn_index[i] = r.u32();
        s.position[i] = r.vec3();
        s.orientation[i] = r.quat();
        s.age[i] = r.f32();
        const BodyRecord* body = find_body(table, s.spawn_index[i]);
        if (body == nullptr) {
            return false;
        }
        s.body_kind[i] = body->kind;
        s.size[i] = body->size;
        s.base_size[i] = body->base_size;
        s.lifetime[i] = body->lifetime;
        s.base_color[i] = body->base_color;
        s.color[i] = body->color;
    }
    if ((flags & frame_colors) != 0U) {
        for (Color& color : s.color) {
            color = r.color();
        }
    } else {
        body_appearance(emitter, s);
    }
    if ((flags & frame_sizes) != 0U) {
        for (float& size : s.size) {
            size = r.f32();
        }
    }
    const std::uint32_t props = r.u32();
    if (props > max_rigid_props || !r.fits(props, 28U)) {
        return false;
    }
    s.props.resize(props);
    for (RigidBodyState& prop : s.props) {
        prop.position = r.vec3();
        prop.orientation = r.quat();
    }
    return r.ok();
}

// Whether a decoded rigid frame is exactly what was simulated, in everything
// a cached body promises: what another emitter collides with and what is
// drawn.
bool same_bodies(const EmitterState& decoded, const EmitterState& s, bool colors, bool sizes) {
    if (decoded.alive() != s.alive() || decoded.spawned != s.spawned || decoded.refused != s.refused ||
        decoded.props.size() != s.props.size()) {
        return false;
    }
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        const std::uint32_t kind = i < s.body_kind.size() ? s.body_kind[i] : 0U;
        if (decoded.spawn_index[i] != s.spawn_index[i] || decoded.position[i] != s.position[i] ||
            decoded.orientation[i] != s.orientation[i] || decoded.age[i] != s.age[i] ||
            decoded.body_kind[i] != kind || decoded.lifetime[i] != s.lifetime[i] ||
            decoded.base_color[i] != s.base_color[i] || decoded.base_size[i] != s.base_size[i]) {
            return false;
        }
        if ((colors && decoded.color[i] != s.color[i]) || (sizes && decoded.size[i] != s.size[i])) {
            return false;
        }
    }
    for (std::size_t p = 0U; p < s.props.size(); ++p) {
        if (decoded.props[p].position != s.props[p].position ||
            decoded.props[p].orientation != s.props[p].orientation) {
            return false;
        }
    }
    return true;
}

// ------------------------------------------------------------ particle frames

constexpr std::uint8_t frame_delta = 0U;
constexpr std::uint8_t frame_key = 1U;
constexpr std::int16_t position_escape = std::numeric_limits<std::int16_t>::min();
constexpr float tau = 2.0F * std::numbers::pi_v<float>;

std::uint16_t turn_bits(float radians) {
    const float turns = radians / tau;
    const float fraction = turns - std::floor(turns);
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(std::lround(fraction * 65536.0F)) & 0xffffU);
}

float turn_radians(std::uint16_t bits) {
    return static_cast<float>(bits) * (tau / 65536.0F);
}

std::int16_t snorm(float v) {
    return static_cast<std::int16_t>(std::lround(std::clamp(v, -1.0F, 1.0F) * 32767.0F));
}

Quat unsnorm(std::int16_t x, std::int16_t y, std::int16_t z, std::int16_t w) {
    const float fx = static_cast<float>(x) / 32767.0F;
    const float fy = static_cast<float>(y) / 32767.0F;
    const float fz = static_cast<float>(z) / 32767.0F;
    const float fw = static_cast<float>(w) / 32767.0F;
    const float length = std::sqrt(fx * fx + fy * fy + fz * fz + fw * fw);
    if (!(length > 0.0F)) {
        return Quat{};
    }
    const float inverse = 1.0F / length;
    return Quat{fx * inverse, fy * inverse, fz * inverse, fw * inverse};
}

struct ParticleCodec final {
    const Emitter& emitter;
    float dt;
    float step;
    std::uint8_t keeps;
};

// A newborn as a delta frame keeps it: exact, but only what it uses and
// what cannot be worked out again. Its colour and size are its base ones put
// through the curves at its age, as its birth did.
std::size_t newborn_bytes(const Emitter& emitter) {
    const std::uint32_t mask = required_attributes(emitter);
    std::size_t bytes = 12U + 12U + 4U + 4U + 16U + 4U + 4U;
    if (has_attribute(mask, Attribute::Rotation)) {
        bytes += 8U;
    }
    if (has_attribute(mask, Attribute::Orientation)) {
        bytes += 28U;
    }
    return bytes;
}

void write_newborn(Writer& w, const Emitter& emitter, const EmitterState& s, std::size_t i) {
    const std::uint32_t mask = required_attributes(emitter);
    w.vec3(s.position[i]);
    w.vec3(s.velocity[i]);
    w.f32(s.age[i]);
    w.f32(s.lifetime[i]);
    w.color(s.base_color[i]);
    w.f32(s.base_size[i]);
    w.u32(s.spawn_index[i]);
    if (has_attribute(mask, Attribute::Rotation)) {
        w.f32(s.rotation[i]);
        w.f32(s.spin[i]);
    }
    if (has_attribute(mask, Attribute::Orientation)) {
        w.quat(s.orientation[i]);
        w.vec3(s.angular_velocity[i]);
    }
}

void read_newborn(Reader& r, const Emitter& emitter, EmitterState& s, std::size_t i) {
    const std::uint32_t mask = required_attributes(emitter);
    s.position[i] = r.vec3();
    s.velocity[i] = r.vec3();
    s.age[i] = r.f32();
    s.lifetime[i] = r.f32();
    s.base_color[i] = r.color();
    s.color[i] = s.base_color[i];
    s.base_size[i] = r.f32();
    s.size[i] = s.base_size[i];
    s.spawn_index[i] = r.u32();
    s.rotation[i] = 0.0F;
    s.spin[i] = 0.0F;
    s.orientation[i] = Quat{};
    s.angular_velocity[i] = Vec3{};
    if (has_attribute(mask, Attribute::Rotation)) {
        s.rotation[i] = r.f32();
        s.spin[i] = r.f32();
    }
    if (has_attribute(mask, Attribute::Orientation)) {
        s.orientation[i] = r.quat();
        s.angular_velocity[i] = r.vec3();
    }
}

// Applies one particle frame to `s`, which holds the frame before it as this
// codec decoded it. False when the bytes do not fit that frame.
bool read_particle_frame(Reader& r, const ParticleCodec& codec, EmitterState& s) {
    const std::uint8_t type = r.u8();
    if (type == frame_key) {
        EmitterState key;
        if (!read_emitter(r, key)) {
            return false;
        }
        s = std::move(key);
        return true;
    }
    if (type != frame_delta) {
        return false;
    }
    const std::uint64_t spawned = r.u64();
    const std::uint64_t refused = r.u64();
    const std::uint32_t previous = r.u32();
    if (previous != s.alive()) {
        return false;
    }
    const std::span<const std::uint8_t> died = r.span((static_cast<std::uint64_t>(previous) + 7U) / 8U);
    if (!r.ok()) {
        return false;
    }
    // The survivors, oldest first as the simulation keeps them.
    std::size_t kept = 0U;
    for (std::size_t i = 0U; i < previous; ++i) {
        if ((died[i / 8U] & (1U << (i % 8U))) != 0U) {
            continue;
        }
        if (kept != i) {
            s.position[kept] = s.position[i];
            s.velocity[kept] = s.velocity[i];
            s.age[kept] = s.age[i];
            s.lifetime[kept] = s.lifetime[i];
            s.base_color[kept] = s.base_color[i];
            s.color[kept] = s.color[i];
            s.base_size[kept] = s.base_size[i];
            s.size[kept] = s.size[i];
            s.rotation[kept] = s.rotation[i];
            s.orientation[kept] = s.orientation[i];
            s.angular_velocity[kept] = s.angular_velocity[i];
            s.spin[kept] = s.spin[i];
            s.spawn_index[kept] = s.spawn_index[i];
        }
        ++kept;
    }
    const std::uint32_t born = r.u32();
    if (kept + born > max_emitter_capacity || !r.fits(born, newborn_bytes(codec.emitter))) {
        return false;
    }
    resize(s, kept + born);
    for (std::size_t i = kept; i < kept + born; ++i) {
        read_newborn(r, codec.emitter, s, i);
    }
    particle_appearance(codec.emitter, s, kept, kept + born);
    for (std::size_t i = 0U; i < kept; ++i) {
        s.age[i] += codec.dt;
        const std::int16_t dx = r.i16();
        if (dx == position_escape) {
            s.position[i] = r.vec3();
        } else {
            const std::int16_t dy = r.i16();
            const std::int16_t dz = r.i16();
            s.position[i] = s.position[i] + Vec3{static_cast<float>(dx) * codec.step,
                                                 static_cast<float>(dy) * codec.step,
                                                 static_cast<float>(dz) * codec.step};
        }
        if ((codec.keeps & keeps_velocity) != 0U) {
            const float x = from_half(r.u16());
            const float y = from_half(r.u16());
            const float z = from_half(r.u16());
            s.velocity[i] = Vec3{x, y, z};
        }
        if ((codec.keeps & keeps_rotation) != 0U) {
            s.rotation[i] = turn_radians(r.u16());
        }
        if ((codec.keeps & keeps_orientation) != 0U) {
            const std::int16_t x = r.i16();
            const std::int16_t y = r.i16();
            const std::int16_t z = r.i16();
            const std::int16_t w = r.i16();
            s.orientation[i] = unsnorm(x, y, z, w);
        }
    }
    particle_appearance(codec.emitter, s, 0U, kept);
    s.spawned = spawned;
    s.refused = refused;
    return r.ok();
}

// The delta from `before` (decoded) to `now` (simulated), or nothing when
// `now` is not `before` with some particles gone and others appended, in
// which case the caller writes a keyframe.
std::optional<std::vector<std::uint8_t>> particle_delta(const ParticleCodec& codec, const EmitterState& before,
                                                        const EmitterState& now) {
    const std::size_t previous = before.alive();
    std::vector<std::uint8_t> died((previous + 7U) / 8U, 0U);
    std::vector<std::size_t> survivor_of; // index in `before` of each survivor in `now`
    std::size_t j = 0U;
    for (std::size_t i = 0U; i < previous; ++i) {
        if (j < now.alive() && now.spawn_index[j] == before.spawn_index[i]) {
            survivor_of.push_back(i);
            ++j;
        } else {
            died[i / 8U] = static_cast<std::uint8_t>(died[i / 8U] | (1U << (i % 8U)));
        }
    }
    const std::size_t kept = j;
    Writer w;
    w.u8(frame_delta);
    w.u64(now.spawned);
    w.u64(now.refused);
    w.u32(static_cast<std::uint32_t>(previous));
    w.bytes(died);
    w.u32(static_cast<std::uint32_t>(now.alive() - kept));
    for (std::size_t i = kept; i < now.alive(); ++i) {
        write_newborn(w, codec.emitter, now, i);
    }
    for (std::size_t k = 0U; k < kept; ++k) {
        const Vec3 from = before.position[survivor_of[k]];
        const Vec3 to = now.position[k];
        const auto steps = [&codec](float d) { return std::round(d / codec.step); };
        const float qx = steps(to.x - from.x);
        const float qy = steps(to.y - from.y);
        const float qz = steps(to.z - from.z);
        const auto fits = [](float q) { return std::isfinite(q) && q >= -32767.0F && q <= 32767.0F; };
        if (fits(qx) && fits(qy) && fits(qz)) {
            w.i16(static_cast<std::int16_t>(qx));
            w.i16(static_cast<std::int16_t>(qy));
            w.i16(static_cast<std::int16_t>(qz));
        } else {
            w.i16(position_escape);
            w.vec3(to);
        }
        if ((codec.keeps & keeps_velocity) != 0U) {
            w.u16(to_half(now.velocity[k].x));
            w.u16(to_half(now.velocity[k].y));
            w.u16(to_half(now.velocity[k].z));
        }
        if ((codec.keeps & keeps_rotation) != 0U) {
            w.u16(turn_bits(now.rotation[k]));
        }
        if ((codec.keeps & keeps_orientation) != 0U) {
            w.i16(snorm(now.orientation[k].x));
            w.i16(snorm(now.orientation[k].y));
            w.i16(snorm(now.orientation[k].z));
            w.i16(snorm(now.orientation[k].w));
        }
    }
    return std::move(w.out);
}

// Whether a decoded particle frame keeps what a cached particle promises
// exactly: who is alive, their ages, lives, colours and sizes.
bool same_particles(const EmitterState& decoded, const EmitterState& s) {
    if (decoded.alive() != s.alive() || decoded.spawned != s.spawned || decoded.refused != s.refused) {
        return false;
    }
    for (std::size_t i = 0U; i < s.alive(); ++i) {
        if (decoded.spawn_index[i] != s.spawn_index[i] || decoded.age[i] != s.age[i] ||
            decoded.lifetime[i] != s.lifetime[i] || decoded.color[i] != s.color[i] ||
            decoded.size[i] != s.size[i] || decoded.base_color[i] != s.base_color[i] ||
            decoded.base_size[i] != s.base_size[i]) {
            return false;
        }
    }
    return true;
}

// One set of values for every step: what a bake runs with.
class FixedValues final : public ParameterProvider {
public:
    explicit FixedValues(std::vector<ParameterValue> values) : values_{std::move(values)} {}
    void values_at(std::uint64_t, std::span<ParameterValue> values) const override {
        std::copy_n(values_.begin(), std::min(values_.size(), values.size()), values.begin());
    }

private:
    std::vector<ParameterValue> values_;
};

// `values` filled out with defaults and clamped, as a step would see them.
std::vector<ParameterValue> complete(const Effect& effect, std::span<const ParameterValue> values) {
    std::vector<ParameterValue> out(effect.parameters.size());
    for (std::size_t i = 0U; i < out.size(); ++i) {
        const ParameterDesc& parameter = effect.parameters[i];
        out[i] = i < values.size() ? values[i] : parameter.default_value;
        for (float& component : out[i]) {
            component = std::clamp(component, parameter.min, parameter.max);
        }
    }
    return out;
}

// Marks the parameters a module's simulated inputs bind.
class BoundParameters final {
public:
    BoundParameters(const Effect& effect, std::vector<bool>& read) : effect_{effect}, read_{read} {}

    template <class T>
    void operator()(std::string_view, const Input<T>& input) {
        if (!input.bound()) {
            return;
        }
        for (std::size_t i = 0U; i < effect_.parameters.size(); ++i) {
            if (effect_.parameters[i].name == input.parameter) {
                read_[i] = true;
            }
        }
    }
    void operator()(std::string_view, const std::vector<BreakPiece>& pieces) {
        for (const BreakPiece& piece : pieces) {
            BreakPiece::fields(piece, *this);
        }
    }
    void operator()(std::string_view, const std::vector<RigidBodyKind>& bodies) {
        for (const RigidBodyKind& body : bodies) {
            RigidBodyKind::fields(body, *this);
        }
    }
    void operator()(std::string_view, const std::vector<RigidProp>& props) {
        for (const RigidProp& prop : props) {
            RigidProp::fields(prop, *this);
        }
    }
    template <class T>
    void operator()(std::string_view, const T&) {}

private:
    const Effect& effect_;
    std::vector<bool>& read_;
};

void mark_reads(const Effect& effect, const Emitter& emitter, std::vector<bool>& read) {
    BoundParameters walk{effect, read};
    const auto visit = [&walk](const auto& module) { std::decay_t<decltype(module)>::fields(module, walk); };
    for (const SpawnModule& module : emitter.spawn) {
        std::visit(visit, module);
    }
    for (const InitModule& module : emitter.init) {
        std::visit(visit, module);
    }
    for (const UpdateModule& module : emitter.update) {
        std::visit(visit, module);
    }
}

// Every name a module or renderer gives: meshes and fields among them.
class Names final {
public:
    std::vector<std::string> found;

    void operator()(std::string_view, const std::string& name) {
        if (!name.empty()) {
            found.push_back(name);
        }
    }
    void operator()(std::string_view, const std::vector<BreakPiece>& pieces) {
        for (const BreakPiece& piece : pieces) {
            BreakPiece::fields(piece, *this);
        }
    }
    void operator()(std::string_view, const std::vector<RigidBodyKind>& bodies) {
        for (const RigidBodyKind& body : bodies) {
            RigidBodyKind::fields(body, *this);
        }
    }
    void operator()(std::string_view, const std::vector<RigidProp>& props) {
        for (const RigidProp& prop : props) {
            RigidProp::fields(prop, *this);
        }
    }
    template <class T>
    void operator()(std::string_view, const T&) {}
};

// One emitter's own digest: everything in emitter_digest but the bodies it
// meets.
std::uint64_t own_digest(const Effect& effect, std::size_t index, const MeshSet& meshes, const FieldSet& fields) {
    const Emitter& emitter = effect.emitters[index];
    std::vector<bool> read(effect.parameters.size(), false);
    mark_reads(effect, emitter, read);
    Effect one;
    one.name = "emitter";
    one.simulation_rate = effect.simulation_rate;
    for (std::size_t p = 0U; p < effect.parameters.size(); ++p) {
        if (read[p] && effect.parameters[p].timing == ParameterTiming::Bake) {
            one.parameters.push_back(effect.parameters[p]);
        }
    }
    // The blasts it answers are part of what it does.
    for (const UpdateModule& module : emitter.update) {
        if (const auto* wave = std::get_if<Shockwave>(&module)) {
            const Blast* blast = find_blast(effect, wave->blast);
            const bool listed = std::any_of(one.blasts.begin(), one.blasts.end(),
                                            [&](const Blast& b) { return b.name == wave->blast; });
            if (blast != nullptr && !listed) {
                one.blasts.push_back(*blast);
            }
        }
    }
    Emitter simulated = emitter;
    // Only what the simulation reads of the renderer: rigid hulls wrap its
    // mesh, and its geometry decides how a body is met.
    simulated.renderer = SpriteRenderer{};
    simulated.renderer.geometry = emitter.renderer.geometry;
    simulated.renderer.mesh = emitter.renderer.mesh;
    for (UpdateModule& module : simulated.update) {
        if (auto* physics = std::get_if<RigidPhysics>(&module)) {
            for (RigidBodyKind& body : physics->bodies) { body.draw_tint = Color{}; }
            for (RigidProp& prop : physics->props) { prop.draw_tint = Color{}; }
        }
    }
    one.emitters.push_back(std::move(simulated));
    std::uint64_t hash = fnv_text(effect_to_json(one));
    Names names;
    const auto visit = [&names](const auto& module) { std::decay_t<decltype(module)>::fields(module, names); };
    for (const SpawnModule& module : emitter.spawn) {
        std::visit(visit, module);
    }
    for (const InitModule& module : emitter.init) {
        std::visit(visit, module);
    }
    for (const UpdateModule& module : emitter.update) {
        std::visit(visit, module);
    }
    names.found.push_back(emitter.renderer.mesh);
    std::sort(names.found.begin(), names.found.end());
    names.found.erase(std::unique(names.found.begin(), names.found.end()), names.found.end());
    for (const std::string& name : names.found) {
        if (const auto mesh = meshes.find(name); mesh != meshes.end()) {
            hash = fnv_text(name, hash);
            hash = fnv1a(write_mesh(mesh->second), hash);
        }
        if (const auto field = fields.find(name); field != fields.end()) {
            const DistanceField& f = field->second;
            hash = fnv_text(name, hash);
            hash = fnv_u64(f.nx, hash);
            hash = fnv_u64(f.ny, hash);
            hash = fnv_u64(f.nz, hash);
            for (const float v : {f.origin.x, f.origin.y, f.origin.z, f.spacing}) {
                hash = fnv_u64(std::bit_cast<std::uint32_t>(v), hash);
            }
            for (const float d : f.distances) {
                hash = fnv_u64(std::bit_cast<std::uint32_t>(d), hash);
            }
        }
    }
    return hash;
}

// Which parameters the cached emitters depend on.
std::vector<bool> cached_reads(const Effect& effect, std::span<const CacheMode> modes) {
    std::vector<bool> read(effect.parameters.size(), false);
    for (std::size_t i = 0U; i < effect.emitters.size() && i < modes.size(); ++i) {
        if (modes[i] == CacheMode::Live) {
            continue;
        }
        for (const std::size_t p : parameters_read(effect, i)) {
            read[p] = true;
        }
    }
    return read;
}

std::string describe(const ParameterValue& value, std::size_t components) {
    std::string out;
    for (std::size_t i = 0U; i < components; ++i) {
        char text[32];
        std::snprintf(text, sizeof(text), "%g", static_cast<double>(value[i]));
        out += (i == 0U ? "" : ", ") + std::string{text};
    }
    return components == 1U ? out : "(" + out + ")";
}

std::size_t components_of(const Effect& effect, std::string_view name) {
    for (const ParameterDesc& parameter : effect.parameters) {
        if (parameter.name == name) {
            return parameter.type == ParameterType::Float ? 1U : parameter.type == ParameterType::Vec3 ? 3U : 4U;
        }
    }
    return 4U;
}

// Why `have` is not `want`, in a sentence, or nothing when it is: first
// where it was placed, then which emitters, then what it was baked with.
std::optional<std::string> origin_difference(const CacheIdentity& have, const CacheIdentity& want) {
    if (have.instance_seed != want.instance_seed) {
        return "it was baked with instance seed " + std::to_string(have.instance_seed) + ", not " +
               std::to_string(want.instance_seed);
    }
    if (have.simulation_rate != want.simulation_rate) {
        return "it was baked at " + std::to_string(have.simulation_rate) + " steps a second, not " +
               std::to_string(want.simulation_rate);
    }
    return std::nullopt;
}

std::optional<std::string> emitter_difference(const CacheIdentity& have, const CacheIdentity& want) {
    for (const CachedEmitter& baked : have.emitters) {
        const auto now = std::find_if(want.emitters.begin(), want.emitters.end(),
                                      [&baked](const CachedEmitter& e) { return e.emitter == baked.emitter; });
        if (now == want.emitters.end()) {
            return "emitter " + quoted(baked.emitter) + ", which it holds, is not in the effect";
        }
        if (now->mode != baked.mode || now->digest != baked.digest) {
            return "emitter " + quoted(baked.emitter) + " has changed since the bake";
        }
    }
    return std::nullopt;
}

std::optional<std::string> value_difference(const Effect& effect, const CacheIdentity& have,
                                            const CacheIdentity& want) {
    for (std::size_t i = 0U; i < std::max(have.bake.size(), want.bake.size()); ++i) {
        if (i >= have.bake.size()) {
            return "bake-time parameter " + quoted(want.bake[i].name) + " was not part of the bake";
        }
        if (i >= want.bake.size() || have.bake[i].name != want.bake[i].name) {
            return "it was baked with bake-time parameter " + quoted(have.bake[i].name) +
                   ", which the effect no longer has in that place";
        }
        if (have.bake[i].value != want.bake[i].value) {
            const std::size_t n = components_of(effect, have.bake[i].name);
            return "bake-time parameter " + quoted(have.bake[i].name) + " was baked at " +
                   describe(have.bake[i].value, n) + " and is now " + describe(want.bake[i].value, n);
        }
    }
    return std::nullopt;
}

// The most bytes any window of `window` consecutive frames adds up to.
std::uint64_t peak_window(const std::vector<std::uint64_t>& per_frame, std::uint64_t window) {
    std::uint64_t sum = 0U;
    std::uint64_t peak = 0U;
    const std::size_t w = static_cast<std::size_t>(std::max<std::uint64_t>(window, 1U));
    for (std::size_t i = 0U; i < per_frame.size(); ++i) {
        sum += per_frame[i];
        if (i >= w) {
            sum -= per_frame[i - w];
        }
        peak = std::max(peak, sum);
    }
    return peak;
}

struct Slice final {
    std::uint64_t offset{0U};
    std::uint32_t size{0U};
};

} // namespace

namespace {

// The emitters whose bodies or contacts emitter `index` bears particles
// from (SpawnFromContacts, SpawnFromBodies), or whose bodies' speeds stir it
// (Wake): what a cache of bodies does not keep.
std::vector<std::size_t> born_of(const Effect& effect, std::size_t index) {
    std::vector<std::size_t> out;
    for (const UpdateModule& module : effect.emitters[index].update) {
        const auto* stirred = std::get_if<Wake>(&module);
        for (std::size_t other = 0U; stirred != nullptr && other < effect.emitters.size(); ++other) {
            if (other != index && effect.emitters[other].name == stirred->emitter) {
                out.push_back(other);
            }
        }
    }
    for (const SpawnModule& module : effect.emitters[index].spawn) {
        const std::string* named = nullptr;
        if (const auto* contacts = std::get_if<SpawnFromContacts>(&module)) {
            named = &contacts->emitter;
        } else if (const auto* shed = std::get_if<SpawnFromBodies>(&module)) {
            named = &shed->emitter;
        }
        for (std::size_t other = 0U; named != nullptr && other < effect.emitters.size(); ++other) {
            if (other != index && effect.emitters[other].name == *named) {
                out.push_back(other);
            }
        }
    }
    return out;
}

} // namespace

// ------------------------------------------------------------ public: state

std::vector<std::uint8_t> write_state(const SimulationState& state) {
    Writer w;
    write_state_into(w, state);
    w.u64(fnv1a(w.out));
    return std::move(w.out);
}

Result<SimulationState> read_state(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < sizeof(state_magic) + 4U + 8U + 4U + 8U ||
        !std::equal(std::begin(state_magic), std::end(state_magic), bytes.begin())) {
        return malformed("is not saved ajfx simulation state");
    }
    const std::span<const std::uint8_t> body = bytes.first(bytes.size() - 8U);
    Reader trailer{bytes.last(8U)};
    if (fnv1a(body) != trailer.u64()) {
        return malformed("saved simulation state is damaged: its checksum does not match");
    }
    Reader r{body.subspan(sizeof(state_magic))};
    const std::uint32_t version = r.u32();
    if (version != state_format_version) {
        return Error{ErrorCode::UnsupportedVersion, "saved simulation state version " + std::to_string(version) +
                                                        " is not supported; this library reads version " +
                                                        std::to_string(state_format_version)};
    }
    SimulationState state;
    state.tick = r.u64();
    const std::uint32_t emitters = r.u32();
    if (!r.fits(emitters, 4U)) {
        return malformed("saved simulation state is truncated");
    }
    state.emitters.resize(emitters);
    for (std::size_t i = 0U; i < emitters; ++i) {
        if (!read_emitter(r, state.emitters[i])) {
            return malformed("saved simulation state's emitter " + std::to_string(i) +
                             " is truncated or inconsistent");
        }
    }
    if (r.remaining() != 0U) {
        return malformed("saved simulation state has bytes after its last emitter");
    }
    return state;
}

// ------------------------------------------------------------ public: identity

std::uint64_t emitter_digest(const Effect& effect, std::size_t index, const MeshSet& meshes, const FieldSet& fields) {
    if (index >= effect.emitters.size()) {
        return 0U;
    }
    std::uint64_t hash = own_digest(effect, index, meshes, fields);
    const auto depend_on = [&](const std::string& name) {
        for (std::size_t other = 0U; other < effect.emitters.size(); ++other) {
            if (other != index && effect.emitters[other].name == name) {
                hash = fnv_u64(own_digest(effect, other, meshes, fields), hash);
            }
        }
    };
    // The bodies it meets are part of what it does, and so are the bodies
    // and contacts it bears particles from.
    for (const UpdateModule& module : effect.emitters[index].update) {
        if (const auto* meets = std::get_if<BodyCollision>(&module)) {
            depend_on(meets->emitter);
        } else if (const auto* stirred = std::get_if<Wake>(&module)) {
            depend_on(stirred->emitter);
        }
    }
    for (const SpawnModule& module : effect.emitters[index].spawn) {
        if (const auto* contacts = std::get_if<SpawnFromContacts>(&module)) {
            depend_on(contacts->emitter);
        } else if (const auto* shed = std::get_if<SpawnFromBodies>(&module)) {
            depend_on(shed->emitter);
        }
    }
    return hash;
}

std::uint64_t simulation_digest(const Effect& effect, const MeshSet& meshes, const FieldSet& fields) {
    Effect declared;
    declared.name = "effect";
    declared.simulation_rate = effect.simulation_rate;
    for (const ParameterDesc& parameter : effect.parameters) {
        if (parameter.timing == ParameterTiming::Bake) {
            declared.parameters.push_back(parameter);
        }
    }
    std::uint64_t hash = fnv_text(effect_to_json(declared));
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        hash = fnv_text(effect.emitters[i].name, hash);
        hash = fnv_u64(emitter_digest(effect, i, meshes, fields), hash);
    }
    return hash;
}

std::vector<CacheMode> default_cache_modes(const Effect& effect) {
    std::vector<CacheMode> modes(effect.emitters.size(), CacheMode::Live);
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        if (rigid_physics(effect.emitters[i]) != nullptr) {
            modes[i] = CacheMode::Rigid;
        }
    }
    // An emitter bearing particles from cached bodies cannot stay live: a
    // cache keeps bodies, not their contacts or speeds. It is cached with
    // them where a particle layer may hold it, and otherwise the bodies stay
    // live too.
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        for (const std::size_t other : born_of(effect, i)) {
            if (modes[other] == CacheMode::Live) {
                continue;
            }
            if (effect.emitters[i].capacity <= max_cached_particles) {
                modes[i] = CacheMode::Particles;
            } else {
                modes[other] = CacheMode::Live;
            }
        }
    }
    return modes;
}

std::vector<std::size_t> parameters_read(const Effect& effect, std::size_t index) {
    std::vector<bool> read(effect.parameters.size(), false);
    if (index >= effect.emitters.size()) {
        return {};
    }
    const Emitter& emitter = effect.emitters[index];
    mark_reads(effect, emitter, read);
    // The bodies it meets move as their own emitter's parameters say.
    for (const UpdateModule& module : emitter.update) {
        const std::string* named = nullptr;
        if (const auto* meets = std::get_if<BodyCollision>(&module)) {
            named = &meets->emitter;
        } else if (const auto* stirred = std::get_if<Wake>(&module)) {
            named = &stirred->emitter;
        }
        if (named != nullptr) {
            for (const Emitter& other : effect.emitters) {
                if (other.name == *named && rigid_physics(other) != nullptr) {
                    mark_reads(effect, other, read);
                }
            }
        }
    }
    std::vector<std::size_t> out;
    for (std::size_t i = 0U; i < read.size(); ++i) {
        if (read[i]) {
            out.push_back(i);
        }
    }
    return out;
}

CacheIdentity cache_identity(const Effect& effect, std::span<const CacheMode> modes, std::uint32_t instance_seed,
                             std::span<const ParameterValue> values, const MeshSet& meshes, const FieldSet& fields) {
    const std::vector<CacheMode> defaults = modes.empty() ? default_cache_modes(effect) : std::vector<CacheMode>{};
    const std::span<const CacheMode> chosen = modes.empty() ? std::span<const CacheMode>{defaults} : modes;
    const std::vector<bool> read = cached_reads(effect, chosen);
    const std::vector<ParameterValue> all = complete(effect, values);
    CacheIdentity identity{instance_seed, effect.simulation_rate, {}, {}};
    for (std::size_t i = 0U; i < effect.emitters.size() && i < chosen.size(); ++i) {
        if (chosen[i] != CacheMode::Live) {
            identity.emitters.push_back(
                CachedEmitter{effect.emitters[i].name, chosen[i], emitter_digest(effect, i, meshes, fields)});
        }
    }
    for (std::size_t i = 0U; i < effect.parameters.size(); ++i) {
        if (effect.parameters[i].timing == ParameterTiming::Bake && read[i]) {
            identity.bake.push_back(BakeValue{effect.parameters[i].name, all[i]});
        }
    }
    return identity;
}

std::string_view cache_mode_name(CacheMode mode) noexcept {
    switch (mode) {
    case CacheMode::Live:
        return "live";
    case CacheMode::Rigid:
        return "rigid";
    case CacheMode::Particles:
        return "particles";
    }
    return "live";
}

// ------------------------------------------------------------ the cache

struct Cache::Data final {
    std::vector<std::uint8_t> bytes;
    // The effect it plays, which may differ from the one baked in its live
    // emitters.
    Effect effect;
    CacheIdentity identity;
    std::uint64_t frames{0U};
    std::uint64_t keyframe_interval{0U};
    float position_step{0.0F};
    float dt{0.0F};
    BakeReport report;
    // The baked effect's emitters, in its order.
    struct Layer final {
        std::string name;
        CacheMode mode{CacheMode::Live};
        std::uint8_t keeps{0U};
        std::vector<BodyRecord> bodies;
        std::vector<Slice> frames;
        // The ticks whose frames are keyframes, ascending (particle layers).
        std::vector<std::uint64_t> keys;
    };
    std::vector<Layer> layers;
    // For each of the playing effect's emitters, the cached layer it is, or
    // none for one that plays live.
    std::vector<const Layer*> of;
    struct Checkpoint final {
        std::uint64_t tick{0U};
        Slice slice;
    };
    // Empty unless they describe the playing effect.
    std::vector<Checkpoint> checkpoints;
    // Where the frames and checkpoints are, within `bytes`.
    std::size_t blob{0U};
    std::size_t blob_size{0U};

    [[nodiscard]] std::span<const std::uint8_t> at(const Slice& slice) const {
        return std::span{bytes}.subspan(blob + static_cast<std::size_t>(slice.offset), slice.size);
    }
    [[nodiscard]] const Layer* layer(std::size_t index) const noexcept {
        return index < of.size() ? of[index] : nullptr;
    }
};

Cache::Cache(std::unique_ptr<Data> data) : data_{std::move(data)} {}
Cache::Cache(Cache&&) noexcept = default;
Cache& Cache::operator=(Cache&&) noexcept = default;
Cache::~Cache() = default;

const CacheIdentity& Cache::identity() const noexcept {
    return data_->identity;
}
const Effect& Cache::effect() const noexcept {
    return data_->effect;
}
std::uint64_t Cache::frames() const noexcept {
    return data_->frames;
}
std::uint64_t Cache::bytes() const noexcept {
    return data_->bytes.size();
}
const BakeReport& Cache::report() const noexcept {
    return data_->report;
}
CacheMode Cache::mode(std::size_t index) const noexcept {
    const Data::Layer* layer = data_->layer(index);
    return layer != nullptr ? layer->mode : CacheMode::Live;
}
bool Cache::has_checkpoints() const noexcept {
    return !data_->checkpoints.empty();
}
bool Cache::caches_anything() const noexcept {
    return std::any_of(data_->of.begin(), data_->of.end(), [](const Data::Layer* layer) { return layer != nullptr; });
}

EmitterState Cache::emitter_at(std::size_t index, std::uint64_t tick) const {
    const Data::Layer* layer = data_->layer(index);
    if (layer == nullptr || data_->frames == 0U) {
        return {};
    }
    const Emitter& emitter = data_->effect.emitters[index];
    tick = std::min(tick, data_->frames - 1U);
    EmitterState state;
    if (layer->mode == CacheMode::Rigid) {
        Reader r{data_->at(layer->frames[tick])};
        if (!read_body_frame(r, emitter, layer->bodies, state)) {
            return {};
        }
    } else if (layer->mode == CacheMode::Particles) {
        const auto key = std::upper_bound(layer->keys.begin(), layer->keys.end(), tick);
        const std::uint64_t from = key == layer->keys.begin() ? 0U : *(key - 1);
        const ParticleCodec codec{emitter, data_->dt, data_->position_step, layer->keeps};
        for (std::uint64_t t = from; t <= tick; ++t) {
            Reader r{data_->at(layer->frames[t])};
            if (!read_particle_frame(r, codec, state)) {
                return {};
            }
        }
    }
    return state;
}

void Cache::advance(std::size_t index, std::uint64_t tick, EmitterState& state) const {
    const Data::Layer* layer = data_->layer(index);
    if (layer == nullptr || data_->frames == 0U) {
        return;
    }
    if (tick >= data_->frames) {
        // Past the end the last frame holds: `state` already is it.
        return;
    }
    if (layer->mode == CacheMode::Particles && tick > 0U) {
        const ParticleCodec codec{data_->effect.emitters[index], data_->dt, data_->position_step, layer->keeps};
        EmitterState next = state;
        Reader r{data_->at(layer->frames[tick])};
        if (read_particle_frame(r, codec, next)) {
            state = std::move(next);
            return;
        }
    }
    state = emitter_at(index, tick);
}

std::optional<std::uint64_t> Cache::checkpoint_tick(std::uint64_t tick) const {
    std::optional<std::uint64_t> best;
    for (const Data::Checkpoint& checkpoint : data_->checkpoints) {
        if (checkpoint.tick <= tick && (!best || checkpoint.tick > *best)) {
            best = checkpoint.tick;
        }
    }
    return best;
}

std::optional<SimulationState> Cache::checkpoint_before(std::uint64_t tick) const {
    const Data::Checkpoint* best = nullptr;
    for (const Data::Checkpoint& checkpoint : data_->checkpoints) {
        if (checkpoint.tick <= tick && (best == nullptr || checkpoint.tick > best->tick)) {
            best = &checkpoint;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    Result<SimulationState> state = read_state(data_->at(best->slice));
    if (!state) {
        return std::nullopt;
    }
    return std::move(state).value();
}

Result<Cache> Cache::open(std::vector<std::uint8_t> bytes, const Effect& effect, std::uint32_t instance_seed,
                          std::span<const ParameterValue> values, const MeshSet& meshes, const FieldSet& fields) {
    if (bytes.size() < sizeof(cache_magic) + 4U + 8U ||
        !std::equal(std::begin(cache_magic), std::end(cache_magic), bytes.begin())) {
        return malformed("is not an ajfx cache");
    }
    {
        Reader trailer{std::span{bytes}.last(8U)};
        if (fnv1a(std::span{bytes}.first(bytes.size() - 8U)) != trailer.u64()) {
            return malformed("the cache is damaged: its checksum does not match");
        }
    }
    auto data = std::make_unique<Data>();
    Reader r{std::span{bytes}.subspan(sizeof(cache_magic), bytes.size() - sizeof(cache_magic) - 8U)};
    const std::uint32_t version = r.u32();
    if (version != cache_format_version) {
        return Error{ErrorCode::UnsupportedVersion, "cache version " + std::to_string(version) +
                                                        " is not supported; this library reads version " +
                                                        std::to_string(cache_format_version)};
    }
    CacheIdentity& identity = data->identity;
    identity.instance_seed = r.u32();
    identity.simulation_rate = r.u32();
    const std::uint32_t cached = r.u32();
    if (!r.fits(cached, 13U)) {
        return malformed("the cache's identity is truncated");
    }
    for (std::uint32_t i = 0U; i < cached; ++i) {
        CachedEmitter& emitter = identity.emitters.emplace_back();
        emitter.emitter = r.str();
        const std::uint8_t mode = r.u8();
        emitter.digest = r.u64();
        if (mode == 0U || mode > static_cast<std::uint8_t>(CacheMode::Particles)) {
            return malformed("the cache's identity is malformed");
        }
        emitter.mode = static_cast<CacheMode>(mode);
    }
    const std::uint32_t bakes = r.u32();
    if (!r.fits(bakes, 20U)) {
        return malformed("the cache's identity is truncated");
    }
    for (std::uint32_t i = 0U; i < bakes; ++i) {
        BakeValue& value = identity.bake.emplace_back();
        value.name = r.str();
        for (float& component : value.value) {
            component = r.f32();
        }
    }
    if (!r.ok()) {
        return malformed("the cache's identity is truncated");
    }
    if (std::optional<std::string> why =
            origin_difference(identity, CacheIdentity{instance_seed, effect.simulation_rate, {}, {}})) {
        return Error{ErrorCode::StaleCache, "the cache does not belong to this effect: " + *why};
    }
    const std::uint64_t baked_whole = r.u64();
    data->frames = r.u64();
    data->keyframe_interval = r.u64();
    r.u64(); // checkpoint interval, for tools that print it
    data->position_step = r.f32();
    const std::uint32_t layers = r.u32();
    if (!r.fits(layers, 23U)) {
        return malformed("the cache's emitters are truncated");
    }
    data->layers.resize(layers);
    data->report.layers.resize(layers);
    for (std::size_t i = 0U; i < layers; ++i) {
        Data::Layer& layer = data->layers[i];
        CacheLayer& report = data->report.layers[i];
        layer.name = r.str();
        report.emitter = layer.name;
        const std::uint8_t mode = r.u8();
        layer.keeps = r.u8();
        report.bytes = r.u64();
        report.peak_bytes_per_second = r.u64();
        report.peak_count = r.u32();
        if (!r.ok() || mode > static_cast<std::uint8_t>(CacheMode::Particles)) {
            return malformed("the cache's emitter " + std::to_string(i) + " is malformed");
        }
        layer.mode = static_cast<CacheMode>(mode);
        report.mode = layer.mode;
        if (layer.mode == CacheMode::Live) {
            continue;
        }
        const std::uint32_t bodies = r.u32();
        if (!r.fits(bodies, body_record_bytes)) {
            return malformed("the cache's emitter " + quoted(layer.name) + " is truncated");
        }
        layer.bodies.resize(bodies);
        for (BodyRecord& body : layer.bodies) {
            body.spawn_index = r.u32();
            body.kind = r.u32();
            body.size = r.f32();
            body.base_size = r.f32();
            body.lifetime = r.f32();
            body.base_color = r.color();
            body.color = r.color();
        }
        if (!r.fits(data->frames, 12U)) {
            return malformed("the cache's emitter " + quoted(layer.name) + " is truncated");
        }
        layer.frames.resize(static_cast<std::size_t>(data->frames));
        for (Slice& slice : layer.frames) {
            slice.offset = r.u64();
            slice.size = r.u32();
        }
    }
    const std::uint32_t checkpoints = r.u32();
    if (!r.fits(checkpoints, 20U)) {
        return malformed("the cache's checkpoints are truncated");
    }
    std::vector<Data::Checkpoint> stored(checkpoints);
    for (Data::Checkpoint& checkpoint : stored) {
        checkpoint.tick = r.u64();
        checkpoint.slice.offset = r.u64();
        checkpoint.slice.size = r.u32();
    }
    data->report.peak_bytes_per_second = r.u64();
    data->report.checkpoint_bytes = r.u64();
    const std::uint64_t blob = r.u64();
    const std::size_t blob_at = bytes.size() - 8U - r.remaining();
    if (!r.ok() || blob != r.remaining()) {
        return malformed("the cache's frames are truncated");
    }
    data->blob = blob_at;
    data->blob_size = static_cast<std::size_t>(blob);
    const auto inside = [&data](const Slice& slice) {
        return slice.offset <= data->blob_size && slice.size <= data->blob_size - slice.offset;
    };
    for (const Data::Layer& layer : data->layers) {
        for (const Slice& slice : layer.frames) {
            if (!inside(slice) || slice.size == 0U) {
                return malformed("the cache's frame index points outside its frames");
            }
        }
    }
    for (const Data::Checkpoint& checkpoint : stored) {
        if (!inside(checkpoint.slice)) {
            return malformed("the cache's checkpoint index points outside its frames");
        }
    }

    // The playing effect's emitters, matched to the cached ones by name.
    data->of.assign(effect.emitters.size(), nullptr);
    std::vector<CacheMode> modes(effect.emitters.size(), CacheMode::Live);
    for (const Data::Layer& layer : data->layers) {
        if (layer.mode == CacheMode::Live) {
            continue;
        }
        bool found = false;
        for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
            if (effect.emitters[i].name == layer.name) {
                data->of[i] = &layer;
                modes[i] = layer.mode;
                found = true;
            }
        }
        if (!found) {
            return Error{ErrorCode::StaleCache, "the cache does not belong to this effect: emitter " +
                                                    quoted(layer.name) + ", which it holds, is not in the effect"};
        }
    }
    // What it was baked from and with, against what it is to be played with.
    const CacheIdentity expected = cache_identity(effect, modes, instance_seed, values, meshes, fields);
    if (std::optional<std::string> why = emitter_difference(identity, expected)) {
        return Error{ErrorCode::StaleCache, "the cache does not belong to this effect: " + *why};
    }
    if (std::optional<std::string> why = value_difference(effect, identity, expected)) {
        return Error{ErrorCode::StaleCache, "the cache does not belong to this effect: " + *why};
    }
    // Its checkpoints hold every emitter, so they serve only the effect
    // that was baked, live emitters and all.
    bool same_effect = baked_whole == simulation_digest(effect, meshes, fields) && layers == effect.emitters.size();
    for (std::size_t i = 0U; same_effect && i < layers; ++i) {
        same_effect = data->layers[i].name == effect.emitters[i].name;
    }
    if (same_effect) {
        data->checkpoints = std::move(stored);
    }
    data->bytes = std::move(bytes);
    for (Data::Layer& layer : data->layers) {
        if (layer.mode != CacheMode::Particles) {
            continue;
        }
        for (std::size_t t = 0U; t < layer.frames.size(); ++t) {
            if (data->at(layer.frames[t])[0] == frame_key) {
                layer.keys.push_back(t);
            }
        }
        if (layer.keys.empty() || layer.keys.front() != 0U) {
            return malformed("the cache's particle layer " + quoted(layer.name) + " does not start with a keyframe");
        }
    }
    data->report.bytes = data->bytes.size();
    data->report.identity = data->identity;
    data->effect = effect;
    data->dt = 1.0F / static_cast<float>(effect.simulation_rate);
    return Cache{std::move(data)};
}

// ------------------------------------------------------------ baking

Result<Baked> bake(const Effect& effect, std::uint32_t instance_seed, SimulationOptions options,
                   const BakeSettings& settings) {
    if (Result<bool> valid = validate(effect); !valid) {
        return valid.error();
    }
    if (settings.frames == 0U) {
        return Error{ErrorCode::InvalidEffect, "a bake needs at least one step"};
    }
    const std::size_t count = effect.emitters.size();
    const std::vector<CacheMode> modes = settings.modes.empty() ? default_cache_modes(effect) : settings.modes;
    if (modes.size() != count) {
        return Error{ErrorCode::InvalidEffect, "a bake needs one mode per emitter"};
    }
    const std::vector<ParameterValue> baked_values = complete(effect, settings.values);
    const CacheIdentity pinned =
        cache_identity(effect, modes, instance_seed, baked_values, options.meshes, options.fields);
    const std::uint64_t whole = simulation_digest(effect, options.meshes, options.fields);
    // Checkpoints hold the live emitters' state too, which is only the
    // cache's to give if nothing outside the identity steers them.
    bool checkpoints_hold = true;
    {
        const std::vector<bool> held = cached_reads(effect, modes);
        for (std::size_t i = 0U; i < count; ++i) {
            if (modes[i] != CacheMode::Live) {
                continue;
            }
            for (const std::size_t p : parameters_read(effect, i)) {
                if (effect.parameters[p].timing == ParameterTiming::Bake && !held[p]) {
                    checkpoints_hold = false;
                }
            }
        }
    }
    const std::uint64_t checkpoint_interval = checkpoints_hold ? settings.checkpoint_interval : 0U;
    const float step = settings.position_step;
    if (!(step > 0.0F) || !std::isfinite(step)) {
        return Error{ErrorCode::InvalidEffect, "a bake's position step must be positive"};
    }
    for (std::size_t i = 0U; i < count; ++i) {
        const Emitter& emitter = effect.emitters[i];
        const bool rigid = rigid_physics(emitter) != nullptr;
        for (const std::size_t other : born_of(effect, i)) {
            if (modes[i] == CacheMode::Live && modes[other] != CacheMode::Live) {
                return Error{ErrorCode::InvalidEffect,
                             "emitter " + quoted(emitter.name) + " bears particles from, or is stirred by, the bodies of " +
                                 quoted(effect.emitters[other].name) +
                                 ", which this bake caches; a cache keeps bodies, not their contacts or speeds, so "
                                 "cache " + quoted(emitter.name) + " too or leave " +
                                 quoted(effect.emitters[other].name) + " live"};
            }
        }
        if (modes[i] == CacheMode::Rigid && !rigid) {
            return Error{ErrorCode::InvalidEffect, "emitter " + quoted(emitter.name) +
                                                       " has no rigid physics, so its bodies cannot be cached"};
        }
        if (modes[i] == CacheMode::Particles && rigid) {
            return Error{ErrorCode::InvalidEffect, "emitter " + quoted(emitter.name) +
                                                       " has rigid physics: cache its bodies instead"};
        }
        if (modes[i] == CacheMode::Rigid && emitter.capacity > max_cached_bodies) {
            return Error{ErrorCode::BudgetExceeded, "emitter " + quoted(emitter.name) + " holds up to " +
                                                        std::to_string(emitter.capacity) +
                                                        " bodies; a cached layer holds at most " +
                                                        std::to_string(max_cached_bodies)};
        }
        if (modes[i] == CacheMode::Particles && emitter.capacity > max_cached_particles) {
            return Error{ErrorCode::BudgetExceeded, "emitter " + quoted(emitter.name) + " holds up to " +
                                                        std::to_string(emitter.capacity) +
                                                        " particles; a cached layer holds at most " +
                                                        std::to_string(max_cached_particles) +
                                                        ", and one that large is cheaper to simulate than to stream"};
        }
    }

    Result<Simulation> created = Simulation::create(effect, instance_seed, std::move(options));
    if (!created) {
        return created.error();
    }
    Simulation simulation = std::move(created).value();
    const FixedValues values{baked_values};
    simulation.reset(&values);

    const float dt = simulation.step_seconds();
    struct Layer final {
        std::vector<std::uint8_t> bytes;
        std::vector<Slice> frames;
        std::vector<std::uint64_t> per_frame;
        std::vector<BodyRecord> bodies;
        EmitterState decoded;
        std::uint8_t keeps{0U};
        std::uint32_t peak{0U};
    };
    std::vector<Layer> layers(count);
    for (std::size_t i = 0U; i < count; ++i) {
        if (modes[i] == CacheMode::Particles) {
            layers[i].keeps = particle_keeps(effect.emitters[i]);
        }
    }
    std::vector<std::uint8_t> checkpoint_bytes;
    std::vector<std::pair<std::uint64_t, Slice>> checkpoints;
    std::vector<std::uint64_t> checkpoint_per_frame(static_cast<std::size_t>(settings.frames), 0U);
    std::uint64_t total = 0U;

    for (std::uint64_t tick = 0U; tick < settings.frames; ++tick) {
        if (tick > 0U) {
            simulation.step(&values);
        }
        for (std::size_t i = 0U; i < count; ++i) {
            if (modes[i] == CacheMode::Live) {
                continue;
            }
            Layer& layer = layers[i];
            const Emitter& emitter = effect.emitters[i];
            const EmitterState& now = simulation.emitter(i);
            layer.peak = std::max(layer.peak, static_cast<std::uint32_t>(now.alive()));
            std::vector<std::uint8_t> frame;
            if (modes[i] == CacheMode::Rigid) {
                // Each body is recorded once, as it first appears.
                for (std::size_t p = 0U; p < now.alive(); ++p) {
                    const BodyRecord record{now.spawn_index[p], p < now.body_kind.size() ? now.body_kind[p] : 0U,
                                            now.size[p],       now.base_size[p],
                                            now.lifetime[p],   now.base_color[p],
                                            now.color[p]};
                    const auto at = std::lower_bound(
                        layer.bodies.begin(), layer.bodies.end(), record.spawn_index,
                        [](const BodyRecord& r, std::uint32_t index) { return r.spawn_index < index; });
                    if (at == layer.bodies.end() || at->spawn_index != record.spawn_index) {
                        layer.bodies.insert(at, record);
                    }
                }
                std::uint8_t flags = 0U;
                for (;;) {
                    Writer w;
                    write_body_frame(w, now, flags);
                    EmitterState decoded;
                    Reader r{w.out};
                    if (!read_body_frame(r, emitter, layer.bodies, decoded)) {
                        return malformed("could not read back a frame of " + quoted(emitter.name));
                    }
                    if (same_bodies(decoded, now, true, true)) {
                        frame = std::move(w.out);
                        layer.decoded = std::move(decoded);
                        break;
                    }
                    if (flags == (frame_colors | frame_sizes)) {
                        return malformed("emitter " + quoted(emitter.name) +
                                         "'s bodies change in a way a rigid cache cannot hold");
                    }
                    // Colour or size went another way than the curves say
                    // (it should not): keep them outright from here.
                    flags = static_cast<std::uint8_t>(flags | frame_colors | frame_sizes);
                }
            } else {
                const ParticleCodec codec{emitter, dt, step, layer.keeps};
                const bool key = tick == 0U || (settings.keyframe_interval > 0U && tick % settings.keyframe_interval == 0U);
                if (!key) {
                    if (std::optional<std::vector<std::uint8_t>> delta = particle_delta(codec, layer.decoded, now)) {
                        EmitterState decoded = layer.decoded;
                        Reader r{*delta};
                        if (read_particle_frame(r, codec, decoded) && same_particles(decoded, now)) {
                            frame = std::move(*delta);
                            layer.decoded = std::move(decoded);
                        }
                    }
                }
                if (frame.empty()) {
                    Writer w;
                    w.u8(frame_key);
                    write_emitter(w, now);
                    frame = std::move(w.out);
                    layer.decoded = now;
                }
            }
            layer.frames.push_back(Slice{layer.bytes.size(), static_cast<std::uint32_t>(frame.size())});
            layer.per_frame.push_back(frame.size());
            layer.bytes.insert(layer.bytes.end(), frame.begin(), frame.end());
            total += frame.size();
        }
        if (checkpoint_interval > 0U && tick % checkpoint_interval == 0U) {
            SimulationState state = simulation.state();
            for (std::size_t i = 0U; i < count; ++i) {
                if (modes[i] != CacheMode::Live) {
                    state.emitters[i] = layers[i].decoded;
                }
            }
            const std::vector<std::uint8_t> saved = write_state(state);
            checkpoints.emplace_back(tick, Slice{checkpoint_bytes.size(), static_cast<std::uint32_t>(saved.size())});
            checkpoint_bytes.insert(checkpoint_bytes.end(), saved.begin(), saved.end());
            checkpoint_per_frame[static_cast<std::size_t>(tick)] = saved.size();
            total += saved.size();
        }
        if (total > settings.budget.bytes) {
            std::size_t heaviest = 0U;
            for (std::size_t i = 1U; i < count; ++i) {
                if (layers[i].bytes.size() > layers[heaviest].bytes.size()) {
                    heaviest = i;
                }
            }
            return Error{ErrorCode::BudgetExceeded,
                         "the cache passed its budget of " + std::to_string(settings.budget.bytes) + " bytes at step " +
                             std::to_string(tick) + "; emitter " + quoted(effect.emitters[heaviest].name) +
                             " weighs most, at " + std::to_string(layers[heaviest].bytes.size()) + " bytes"};
        }
        if (settings.progress && !settings.progress(tick)) {
            return Error{ErrorCode::InvalidEffect, "the bake was stopped at step " + std::to_string(tick)};
        }
    }

    // What playing it costs, per emitter and in all.
    BakeReport report;
    std::vector<std::uint64_t> all = checkpoint_per_frame;
    const std::uint64_t second = effect.simulation_rate;
    for (std::size_t i = 0U; i < count; ++i) {
        CacheLayer& out = report.layers.emplace_back();
        out.emitter = effect.emitters[i].name;
        out.mode = modes[i];
        out.bytes = layers[i].bytes.size();
        out.peak_bytes_per_second = peak_window(layers[i].per_frame, second);
        out.peak_count = layers[i].peak;
        for (std::size_t t = 0U; t < layers[i].per_frame.size(); ++t) {
            all[t] += layers[i].per_frame[t];
        }
    }
    report.peak_bytes_per_second = peak_window(all, second);
    report.checkpoint_bytes = checkpoint_bytes.size();
    if (report.peak_bytes_per_second > settings.budget.bytes_per_second) {
        std::size_t heaviest = 0U;
        for (std::size_t i = 1U; i < count; ++i) {
            if (report.layers[i].peak_bytes_per_second > report.layers[heaviest].peak_bytes_per_second) {
                heaviest = i;
            }
        }
        return Error{ErrorCode::BudgetExceeded,
                     "playing the cache would read up to " + std::to_string(report.peak_bytes_per_second) +
                         " bytes a second, over its budget of " + std::to_string(settings.budget.bytes_per_second) +
                         "; emitter " + quoted(effect.emitters[heaviest].name) + " reads most, up to " +
                         std::to_string(report.layers[heaviest].peak_bytes_per_second)};
    }

    // The file: header, indices, then every frame and checkpoint.
    Writer w;
    w.bytes(cache_magic);
    w.u32(cache_format_version);
    w.u32(pinned.instance_seed);
    w.u32(pinned.simulation_rate);
    w.u32(static_cast<std::uint32_t>(pinned.emitters.size()));
    for (const CachedEmitter& emitter : pinned.emitters) {
        w.str(emitter.emitter);
        w.u8(static_cast<std::uint8_t>(emitter.mode));
        w.u64(emitter.digest);
    }
    w.u32(static_cast<std::uint32_t>(pinned.bake.size()));
    for (const BakeValue& value : pinned.bake) {
        w.str(value.name);
        for (const float component : value.value) {
            w.f32(component);
        }
    }
    w.u64(whole);
    w.u64(settings.frames);
    w.u64(settings.keyframe_interval);
    w.u64(checkpoint_interval);
    w.f32(step);
    w.u32(static_cast<std::uint32_t>(count));
    std::uint64_t offset = 0U;
    for (std::size_t i = 0U; i < count; ++i) {
        const CacheLayer& out = report.layers[i];
        w.str(out.emitter);
        w.u8(static_cast<std::uint8_t>(out.mode));
        w.u8(layers[i].keeps);
        w.u64(out.bytes);
        w.u64(out.peak_bytes_per_second);
        w.u32(out.peak_count);
        if (out.mode == CacheMode::Live) {
            continue;
        }
        w.u32(static_cast<std::uint32_t>(layers[i].bodies.size()));
        for (const BodyRecord& body : layers[i].bodies) {
            w.u32(body.spawn_index);
            w.u32(body.kind);
            w.f32(body.size);
            w.f32(body.base_size);
            w.f32(body.lifetime);
            w.color(body.base_color);
            w.color(body.color);
        }
        for (const Slice& slice : layers[i].frames) {
            w.u64(offset + slice.offset);
            w.u32(slice.size);
        }
        offset += layers[i].bytes.size();
    }
    w.u32(static_cast<std::uint32_t>(checkpoints.size()));
    for (const auto& [tick, slice] : checkpoints) {
        w.u64(tick);
        w.u64(offset + slice.offset);
        w.u32(slice.size);
    }
    w.u64(report.peak_bytes_per_second);
    w.u64(report.checkpoint_bytes);
    w.u64(offset + checkpoint_bytes.size());
    for (const Layer& layer : layers) {
        w.bytes(layer.bytes);
    }
    w.bytes(checkpoint_bytes);
    w.u64(fnv1a(w.out));
    report.bytes = w.out.size();
    report.identity = pinned;
    if (report.bytes > settings.budget.bytes) {
        return Error{ErrorCode::BudgetExceeded, "the cache comes to " + std::to_string(report.bytes) +
                                                    " bytes, over its budget of " +
                                                    std::to_string(settings.budget.bytes)};
    }
    return Baked{std::move(w.out), std::move(report)};
}

} // namespace ajfx
