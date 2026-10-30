#pragma once

#include <ajfx/math.hpp>
#include <ajfx/parameters.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The built-in modules. An emitter runs three stacks of them every simulation
// step: spawn modules decide how many particles are born, init modules set up
// each newborn, and update modules act on every live particle.
//
// Every module describes its fields once, in `fields`, and the package format,
// validation and parameter binding are all written against that list. Adding
// a field to a module is one line there.
//
// `id` names the module instance within its emitter. Random streams are keyed
// by it, so inserting or reordering modules leaves every other module's random
// values where they were. The builder assigns ids; an editor keeps them stable.
namespace ajfx {

using ModuleId = std::uint32_t;

// ---------------------------------------------------------------- spawn

// Continuous emission. Births are spread across the step at their exact times,
// so emission is smooth whatever the simulation rate.
struct SpawnRate final {
    static constexpr std::string_view kind = "spawn_rate";
    ModuleId id{};
    FloatInput rate{10.0F}; // particles per second

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("rate", self.rate);
    }
    friend bool operator==(const SpawnRate&, const SpawnRate&) = default;
};

// `count` particles at `time` seconds, repeated every `interval` seconds
// `cycles` times. An interval of 0 is a single burst; 0 cycles with a
// non-zero interval repeats for ever.
struct SpawnBurst final {
    static constexpr std::string_view kind = "spawn_burst";
    ModuleId id{};
    float time{0.0F};
    std::uint32_t count{10U};
    float interval{0.0F};
    std::uint32_t cycles{1U};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("time", self.time);
        visit("count", self.count);
        visit("interval", self.interval);
        visit("cycles", self.cycles);
    }
    friend bool operator==(const SpawnBurst&, const SpawnBurst&) = default;
};

using SpawnModule = std::variant<SpawnRate, SpawnBurst>;

// ---------------------------------------------------------------- init

// Seconds, uniformly between min and max. Required on any emitter that spawns.
struct Lifetime final {
    static constexpr std::string_view kind = "lifetime";
    ModuleId id{};
    FloatInput min{1.0F};
    FloatInput max{1.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("min", self.min);
        visit("max", self.max);
    }
    friend bool operator==(const Lifetime&, const Lifetime&) = default;
};

enum class Shape {
    Point,
    Sphere,
    Box,
    Disc, // in the XZ plane
};

struct ShapeLocation final {
    static constexpr std::string_view kind = "shape_location";
    ModuleId id{};
    Shape shape{Shape::Sphere};
    Vec3Input center{};
    FloatInput radius{1.0F};        // sphere and disc
    Vec3 size{1.0F, 1.0F, 1.0F};    // box
    bool surface_only{false};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("shape", self.shape);
        visit("center", self.center);
        visit("radius", self.radius);
        visit("size", self.size);
        visit("surface_only", self.surface_only);
    }
    friend bool operator==(const ShapeLocation&, const ShapeLocation&) = default;
};

// Speed between min and max along a direction, scattered uniformly within a
// cone of `spread` degrees half-angle (180 is every direction).
struct Velocity final {
    static constexpr std::string_view kind = "velocity";
    ModuleId id{};
    Vec3Input direction{Vec3{0.0F, 1.0F, 0.0F}};
    FloatInput speed_min{1.0F};
    FloatInput speed_max{1.0F};
    FloatInput spread{0.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("direction", self.direction);
        visit("speed_min", self.speed_min);
        visit("speed_max", self.speed_max);
        visit("spread", self.spread);
    }
    friend bool operator==(const Velocity&, const Velocity&) = default;
};

// A colour between a and b. Colour-over-life multiplies this.
struct InitColor final {
    static constexpr std::string_view kind = "color";
    ModuleId id{};
    ColorInput a{};
    ColorInput b{};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("a", self.a);
        visit("b", self.b);
    }
    friend bool operator==(const InitColor&, const InitColor&) = default;
};

// World-space sprite size. Size-over-life multiplies this.
struct InitSize final {
    static constexpr std::string_view kind = "size";
    ModuleId id{};
    FloatInput min{0.1F};
    FloatInput max{0.1F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("min", self.min);
        visit("max", self.max);
    }
    friend bool operator==(const InitSize&, const InitSize&) = default;
};

// Sprite rotation, degrees.
struct InitRotation final {
    static constexpr std::string_view kind = "rotation";
    ModuleId id{};
    FloatInput min{0.0F};
    FloatInput max{360.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("min", self.min);
        visit("max", self.max);
    }
    friend bool operator==(const InitRotation&, const InitRotation&) = default;
};

using InitModule = std::variant<Lifetime, ShapeLocation, Velocity, InitColor, InitSize, InitRotation>;

// ---------------------------------------------------------------- update
//
// Forces (gravity, drag, curl noise, vortex, attractor) change velocity and
// run first, in stack order. Position is then integrated. Constraints and
// appearance (plane collision, colour and size over life) run after, in stack
// order.

struct Gravity final {
    static constexpr std::string_view kind = "gravity";
    ModuleId id{};
    Vec3Input acceleration{Vec3{0.0F, -9.81F, 0.0F}};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("acceleration", self.acceleration);
    }
    friend bool operator==(const Gravity&, const Gravity&) = default;
};

// Velocity loses `coefficient` of itself per second (exactly: it is divided
// by 1 + coefficient * dt each step, which is stable at any coefficient).
struct Drag final {
    static constexpr std::string_view kind = "drag";
    ModuleId id{};
    FloatInput coefficient{1.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("coefficient", self.coefficient);
    }
    friend bool operator==(const Drag&, const Drag&) = default;
};

// Divergence-free turbulence: swirls without bunching particles together.
// `frequency` is in cycles per world unit; `evolution` scrolls the field
// through time in units per second.
struct CurlNoise final {
    static constexpr std::string_view kind = "curl_noise";
    ModuleId id{};
    FloatInput strength{1.0F};
    FloatInput frequency{1.0F};
    FloatInput evolution{0.0F};
    std::uint32_t seed{0U};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("strength", self.strength);
        visit("frequency", self.frequency);
        visit("evolution", self.evolution);
        visit("seed", self.seed);
    }
    friend bool operator==(const CurlNoise&, const CurlNoise&) = default;
};

// Acceleration around an axis through `center`, counter-clockwise looking
// down the axis for positive strength.
struct Vortex final {
    static constexpr std::string_view kind = "vortex";
    ModuleId id{};
    Vec3Input center{};
    Vec3Input axis{Vec3{0.0F, 1.0F, 0.0F}};
    FloatInput strength{1.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("center", self.center);
        visit("axis", self.axis);
        visit("strength", self.strength);
    }
    friend bool operator==(const Vortex&, const Vortex&) = default;
};

// Acceleration towards `position` (away from it for negative strength),
// fading linearly to nothing at `radius`; a radius of 0 never fades.
struct Attractor final {
    static constexpr std::string_view kind = "attractor";
    ModuleId id{};
    Vec3Input position{};
    FloatInput strength{1.0F};
    FloatInput radius{0.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("position", self.position);
        visit("strength", self.strength);
        visit("radius", self.radius);
    }
    friend bool operator==(const Attractor&, const Attractor&) = default;
};

// The plane is every point p with dot(normal, p) == offset; particles are kept
// on the side the normal points to. Restitution scales the bounce, friction
// takes that fraction off the sliding velocity, and `kill` removes particles
// that cross instead.
struct PlaneCollision final {
    static constexpr std::string_view kind = "plane_collision";
    ModuleId id{};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    float offset{0.0F};
    FloatInput restitution{0.5F};
    FloatInput friction{0.0F};
    bool kill{false};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("normal", self.normal);
        visit("offset", self.offset);
        visit("restitution", self.restitution);
        visit("friction", self.friction);
        visit("kill", self.kill);
    }
    friend bool operator==(const PlaneCollision&, const PlaneCollision&) = default;
};

// Particles kept outside a sphere, or with `inside` within it. The response
// is the plane's: pushed back onto the surface, with restitution, friction
// and `kill` as there.
struct SphereCollision final {
    static constexpr std::string_view kind = "sphere_collision";
    ModuleId id{};
    Vec3Input center{};
    FloatInput radius{1.0F};
    bool inside{false};
    FloatInput restitution{0.5F};
    FloatInput friction{0.0F};
    bool kill{false};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("center", self.center);
        visit("radius", self.radius);
        visit("inside", self.inside);
        visit("restitution", self.restitution);
        visit("friction", self.friction);
        visit("kill", self.kill);
    }
    friend bool operator==(const SphereCollision&, const SphereCollision&) = default;
};

// Particles kept outside an axis-aligned box (in the effect's own space), or
// with `inside` within it; `half_size` is half its extent on each axis. A
// particle that gets inside leaves through the nearest face (x before y
// before z on a tie); one that gets out returns to the nearest point on the
// box. Responds as SphereCollision.
struct BoxCollision final {
    static constexpr std::string_view kind = "box_collision";
    ModuleId id{};
    Vec3Input center{};
    Vec3 half_size{0.5F, 0.5F, 0.5F};
    bool inside{false};
    FloatInput restitution{0.5F};
    FloatInput friction{0.0F};
    bool kill{false};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("center", self.center);
        visit("half_size", self.half_size);
        visit("inside", self.inside);
        visit("restitution", self.restitution);
        visit("friction", self.friction);
        visit("kill", self.kill);
    }
    friend bool operator==(const BoxCollision&, const BoxCollision&) = default;
};

// Particles kept out of the surface a distance field describes (where its
// distance is negative), or with `inside` within it. The field is named
// (package entry "fields/<name>", see field.hpp) and placed at `offset` in
// the effect's space; outside its grid nothing collides. Responds as
// SphereCollision, pushing along the field's gradient.
struct SdfCollision final {
    static constexpr std::string_view kind = "sdf_collision";
    ModuleId id{};
    std::string field;
    Vec3Input offset{};
    bool inside{false};
    FloatInput restitution{0.5F};
    FloatInput friction{0.0F};
    bool kill{false};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("field", self.field);
        visit("offset", self.offset);
        visit("inside", self.inside);
        visit("restitution", self.restitution);
        visit("friction", self.friction);
        visit("kill", self.kill);
    }
    friend bool operator==(const SdfCollision&, const SdfCollision&) = default;
};

// A key at normalized age `t` (0 at birth, 1 at death). Keys are sorted by t;
// values are linear between keys and held beyond the ends.
struct ColorKey final {
    float t{};
    Color value{};
    friend bool operator==(const ColorKey&, const ColorKey&) = default;
};

struct CurveKey final {
    float t{};
    float value{};
    friend bool operator==(const CurveKey&, const CurveKey&) = default;
};

struct ColorOverLife final {
    static constexpr std::string_view kind = "color_over_life";
    ModuleId id{};
    std::vector<ColorKey> keys{{0.0F, Color{}}, {1.0F, Color{1.0F, 1.0F, 1.0F, 0.0F}}};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("keys", self.keys);
    }
    friend bool operator==(const ColorOverLife&, const ColorOverLife&) = default;
};

struct SizeOverLife final {
    static constexpr std::string_view kind = "size_over_life";
    ModuleId id{};
    std::vector<CurveKey> keys{{0.0F, 1.0F}, {1.0F, 1.0F}};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("keys", self.keys);
    }
    friend bool operator==(const SizeOverLife&, const SizeOverLife&) = default;
};

using UpdateModule = std::variant<Gravity, Drag, CurlNoise, Vortex, Attractor, PlaneCollision, SphereCollision,
                                  BoxCollision, SdfCollision, ColorOverLife, SizeOverLife>;

// Collision modules constrain positions after integration rather than
// accelerating particles.
[[nodiscard]] constexpr bool is_collision(const UpdateModule& module) noexcept {
    return std::holds_alternative<PlaneCollision>(module) || std::holds_alternative<SphereCollision>(module) ||
           std::holds_alternative<BoxCollision>(module) || std::holds_alternative<SdfCollision>(module);
}

// ---------------------------------------------------------------- render

enum class BlendMode {
    Alpha,
    Additive,
    Premultiplied,
};

enum class Facing {
    Camera,
    Velocity,
};

// What a sprite renderer draws: a quad per particle, or one ribbon through
// the emitter's live particles in birth order (oldest first), each end as
// wide as its particle and turned to the camera across the trail.
enum class Geometry {
    Sprites,
    Ribbon,
};

// How a host should draw the emitter's particles. The CPU reference only
// carries this; drawing arrives with the GPU runtime.
struct SpriteRenderer final {
    static constexpr std::string_view kind = "sprite";
    BlendMode blend{BlendMode::Alpha};
    Facing facing{Facing::Camera};
    // A texture in the effect package ("textures/<name>"); empty draws a soft
    // disc. With a flipbook, the texture is a grid of cells played left to
    // right, top to bottom over each particle's life; `flipbook_blend`
    // cross-fades each cell into the next.
    std::string texture;
    std::uint32_t flipbook_columns{1U};
    std::uint32_t flipbook_rows{1U};
    bool flipbook_blend{false};
    bool sort{true};
    // With velocity facing, sprites lengthen by the distance travelled in
    // this many seconds: motion streaks for sparks and rain.
    float stretch{0.0F};
    // Soft particles: a sprite fades out over this distance (world units) as
    // it nears the scene behind it, instead of cutting a hard line where it
    // meets a surface. Zero keeps hard edges. A host draws it against its own
    // scene depth; with no scene there is nothing to fade against.
    float soft_distance{0.0F};
    // A ribbon faces the camera and is not depth-sorted (that would break
    // the strip); a texture runs along it by each end's life.
    Geometry geometry{Geometry::Sprites};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("blend", self.blend);
        visit("facing", self.facing);
        visit("texture", self.texture);
        visit("flipbook_columns", self.flipbook_columns);
        visit("flipbook_rows", self.flipbook_rows);
        visit("flipbook_blend", self.flipbook_blend);
        visit("sort", self.sort);
        visit("stretch", self.stretch);
        visit("soft_distance", self.soft_distance);
        visit("geometry", self.geometry);
    }
    friend bool operator==(const SpriteRenderer&, const SpriteRenderer&) = default;
};

// Names for enum fields, as they appear in packages.
template <class E>
struct EnumNames;

template <>
struct EnumNames<Shape> {
    static constexpr std::string_view names[] = {"point", "sphere", "box", "disc"};
};
template <>
struct EnumNames<BlendMode> {
    static constexpr std::string_view names[] = {"alpha", "additive", "premultiplied"};
};
template <>
struct EnumNames<Facing> {
    static constexpr std::string_view names[] = {"camera", "velocity"};
};
template <>
struct EnumNames<Geometry> {
    static constexpr std::string_view names[] = {"sprites", "ribbon"};
};

} // namespace ajfx
