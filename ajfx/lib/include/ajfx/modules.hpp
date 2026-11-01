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
    // Rigid physics emitters: which of the emitter's body kinds
    // (RigidPhysics::bodies) these newborns are. 0 is the emitter's own shape
    // and material, which is every body's unless it is given another.
    std::uint32_t body{0U};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("rate", self.rate);
        visit("body", self.body);
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
    // Rigid physics emitters: which of the emitter's body kinds
    // (RigidPhysics::bodies) these newborns are. 0 is the emitter's own shape
    // and material, which is every body's unless it is given another.
    std::uint32_t body{0U};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("time", self.time);
        visit("count", self.count);
        visit("interval", self.interval);
        visit("cycles", self.cycles);
        visit("body", self.body);
    }
    friend bool operator==(const SpawnBurst&, const SpawnBurst&) = default;
};

// How a pile's bodies are stacked.
enum class PileShape {
    // A cone whose sides slope at `slope` degrees, as rocks or rubble lie.
    Heap,
    // Rows filling a `width` by `depth` footprint, a layer at a time, as a
    // wall of bricks or a stack of crates.
    Box,
};

// `count` bodies already there when the effect starts, stacked on `center`
// and held still until `release` seconds: a pile of rocks that collapses when
// the effect plays, or a wall that stands until something knocks it down.
// - **When:** the pile is part of step 0's state, so a paused effect shows
//   it, and every seek to the start finds it. Its particles come first in
//   birth order, before any other spawn module's.
// - **Where:** bodies sit on a lattice spaced by the largest pile body's
//   reach (the ball its shape turns within) times `spacing`, filled from the
//   bottom up. A heap's layers are staggered, each body over a gap below, and
//   it grows as tall as `count` needs; a box is built as high as it needs.
//   `jitter` moves each body across by up to that share of the spacing, so a
//   heap is not a perfect stack.
// - **How:** init modules set each body up as any newborn (life, colour,
//   size, orientation), but a pile body starts still where the pile puts it:
//   position, velocity and spin are the pile's.
// - **Held:** until `release` the bodies sleep, so nothing moves them unless
//   another body of the emitter knocks into them. At `release` they all wake
//   and fall as they will. A release of 0 lets them go from the first step.
// Rigid physics emitters only: a pile stands on its bodies.
struct SpawnPile final {
    static constexpr std::string_view kind = "spawn_pile";
    ModuleId id{};
    std::uint32_t count{100U};
    PileShape shape{PileShape::Heap};
    Vec3 center{};
    float slope{55.0F};  // heap, degrees from the ground
    float width{1.0F};   // box, along x
    float depth{1.0F};   // box, along z
    float spacing{1.0F};
    float jitter{0.15F};
    float release{0.0F};
    // Rigid physics emitters: which of the emitter's body kinds
    // (RigidPhysics::bodies) these newborns are. 0 is the emitter's own shape
    // and material, which is every body's unless it is given another.
    std::uint32_t body{0U};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("count", self.count);
        visit("shape", self.shape);
        visit("center", self.center);
        visit("slope", self.slope);
        visit("width", self.width);
        visit("depth", self.depth);
        visit("spacing", self.spacing);
        visit("jitter", self.jitter);
        visit("release", self.release);
        visit("body", self.body);
    }
    friend bool operator==(const SpawnPile&, const SpawnPile&) = default;
};

using SpawnModule = std::variant<SpawnRate, SpawnBurst, SpawnPile>;

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

// Where each particle is born. A sphere or disc gives each particle its own
// radius, uniformly between radius_min and radius_max, and places it within
// that radius (on it with `surface_only`): equal ends give a crisp edge, a
// spread a soft one, or a thick shell.
struct ShapeLocation final {
    static constexpr std::string_view kind = "shape_location";
    ModuleId id{};
    Shape shape{Shape::Sphere};
    Vec3Input center{};
    FloatInput radius_min{1.0F};    // sphere and disc
    FloatInput radius_max{1.0F};
    Vec3 size{1.0F, 1.0F, 1.0F};    // box
    bool surface_only{false};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("shape", self.shape);
        visit("center", self.center);
        visit("radius_min", self.radius_min);
        visit("radius_max", self.radius_max);
        visit("size", self.size);
        visit("surface_only", self.surface_only);
    }
    friend bool operator==(const ShapeLocation&, const ShapeLocation&) = default;
};

// Speed between min and max along a direction, scattered uniformly within a
// cone of `spread` degrees half-angle (180 is every direction). A
// `spread_min` above zero hollows the cone: no particle leaves closer to the
// direction than that, as water from a fountain's crown.
struct Velocity final {
    static constexpr std::string_view kind = "velocity";
    ModuleId id{};
    Vec3Input direction{Vec3{0.0F, 1.0F, 0.0F}};
    FloatInput speed_min{1.0F};
    FloatInput speed_max{1.0F};
    FloatInput spread{0.0F};
    FloatInput spread_min{0.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("direction", self.direction);
        visit("speed_min", self.speed_min);
        visit("speed_max", self.speed_max);
        visit("spread", self.spread);
        visit("spread_min", self.spread_min);
    }
    friend bool operator==(const Velocity&, const Velocity&) = default;
};

// A colour between a and b, its red, green and blue then multiplied by a
// brightness between brightness_min and brightness_max, picked apart from
// the colour. Colour-over-life multiplies this.
struct InitColor final {
    static constexpr std::string_view kind = "color";
    ModuleId id{};
    ColorInput a{};
    ColorInput b{};
    FloatInput brightness_min{1.0F};
    FloatInput brightness_max{1.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("a", self.a);
        visit("b", self.b);
        visit("brightness_min", self.brightness_min);
        visit("brightness_max", self.brightness_max);
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

// A rigid particle's starting turn: an orientation uniformly at random (or
// none), and a spin of speed_min to speed_max degrees per second about an
// axis uniformly at random. Needs a RigidBody module, which turns it.
struct InitTumble final {
    static constexpr std::string_view kind = "tumble";
    ModuleId id{};
    FloatInput speed_min{0.0F};
    FloatInput speed_max{360.0F};
    bool random_orientation{true};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("speed_min", self.speed_min);
        visit("speed_max", self.speed_max);
        visit("random_orientation", self.random_orientation);
    }
    friend bool operator==(const InitTumble&, const InitTumble&) = default;
};

// A sprite's spin: speed_min to speed_max degrees per second, either way
// round when `both_ways`, slowing by `drag` of itself per second (as Drag
// slows velocity). The update turns `rotation` by it, where it integrates
// position; at most one per emitter.
struct InitSpin final {
    static constexpr std::string_view kind = "spin";
    ModuleId id{};
    FloatInput speed_min{0.0F};
    FloatInput speed_max{90.0F};
    bool both_ways{true};
    FloatInput drag{0.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("speed_min", self.speed_min);
        visit("speed_max", self.speed_max);
        visit("both_ways", self.both_ways);
        visit("drag", self.drag);
    }
    friend bool operator==(const InitSpin&, const InitSpin&) = default;
};

using InitModule =
    std::variant<Lifetime, ShapeLocation, Velocity, InitColor, InitSize, InitRotation, InitTumble, InitSpin>;

// Keys of a curve or a gradient over a particle's life (t from 0 at birth to
// 1 at death), sampled linearly between keys and held beyond the ends.
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


// ---------------------------------------------------------------- update
//
// Forces (gravity, drag, curl noise, vortex, attractor, converge) change
// velocity and run first, in stack order. Position is then integrated. Turning (rigid
// body), constraints (the colliders) and appearance (colour and size over
// life) run after, in stack order.
//
// Every force acts on each particle times a factor picked for that particle
// between `scale_min` and `scale_max`, once, so it keeps it for life: some
// sparks fall heavier, some smoke drags more. The default 1 to 1 is the force
// as given.

// A constant acceleration, scaled over each particle's life by `over_life`:
// buoyancy that is strongest while a flame is hot, say.
struct Gravity final {
    static constexpr std::string_view kind = "gravity";
    ModuleId id{};
    Vec3Input acceleration{Vec3{0.0F, -9.81F, 0.0F}};
    std::vector<CurveKey> over_life{{0.0F, 1.0F}, {1.0F, 1.0F}};
    FloatInput scale_min{1.0F};
    FloatInput scale_max{1.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("acceleration", self.acceleration);
        visit("over_life", self.over_life);
        visit("scale_min", self.scale_min);
        visit("scale_max", self.scale_max);
    }
    friend bool operator==(const Gravity&, const Gravity&) = default;
};

// Velocity loses `coefficient` of itself per second (exactly: it is divided
// by 1 + coefficient * dt each step, which is stable at any coefficient).
struct Drag final {
    static constexpr std::string_view kind = "drag";
    ModuleId id{};
    FloatInput coefficient{1.0F};
    FloatInput scale_min{1.0F};
    FloatInput scale_max{1.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("coefficient", self.coefficient);
        visit("scale_min", self.scale_min);
        visit("scale_max", self.scale_max);
    }
    friend bool operator==(const Drag&, const Drag&) = default;
};

// Divergence-free turbulence: swirls without bunching particles together.
// `frequency` is in cycles per world unit; `evolution` scrolls the field
// through time in units per second. The field travels at `drift` (world
// units per second), so eddies roll along with a plume moving at about that
// speed instead of standing still. `over_life` scales the strength over each
// particle's life: calm at a flame's base, breaking up towards its tip.
struct CurlNoise final {
    static constexpr std::string_view kind = "curl_noise";
    ModuleId id{};
    FloatInput strength{1.0F};
    FloatInput frequency{1.0F};
    FloatInput evolution{0.0F};
    std::uint32_t seed{0U};
    Vec3Input drift{};
    std::vector<CurveKey> over_life{{0.0F, 1.0F}, {1.0F, 1.0F}};
    FloatInput scale_min{1.0F};
    FloatInput scale_max{1.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("strength", self.strength);
        visit("frequency", self.frequency);
        visit("evolution", self.evolution);
        visit("seed", self.seed);
        visit("drift", self.drift);
        visit("over_life", self.over_life);
        visit("scale_min", self.scale_min);
        visit("scale_max", self.scale_max);
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
    FloatInput scale_min{1.0F};
    FloatInput scale_max{1.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("center", self.center);
        visit("axis", self.axis);
        visit("strength", self.strength);
        visit("scale_min", self.scale_min);
        visit("scale_max", self.scale_max);
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
    FloatInput scale_min{1.0F};
    FloatInput scale_max{1.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("position", self.position);
        visit("strength", self.strength);
        visit("radius", self.radius);
        visit("scale_min", self.scale_min);
        visit("scale_max", self.scale_max);
    }
    friend bool operator==(const Attractor&, const Attractor&) = default;
};

// Acceleration towards the line through `center` along `axis`, `strength`
// per second squared for each unit away from it: the air a fire draws in at
// its base, gathering flames into tongues. It fades linearly to nothing at
// `height` above the centre (along the axis); a height of 0 never fades.
struct Converge final {
    static constexpr std::string_view kind = "converge";
    ModuleId id{};
    Vec3Input center{};
    Vec3Input axis{Vec3{0.0F, 1.0F, 0.0F}};
    FloatInput strength{1.0F};
    FloatInput height{0.0F};
    FloatInput scale_min{1.0F};
    FloatInput scale_max{1.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("center", self.center);
        visit("axis", self.axis);
        visit("strength", self.strength);
        visit("height", self.height);
        visit("scale_min", self.scale_min);
        visit("scale_max", self.scale_max);
    }
    friend bool operator==(const Converge&, const Converge&) = default;
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

// Makes the emitter's particles small rigid bodies, for debris. Each turns
// by its angular velocity, which loses `angular_drag` of itself per second as
// Drag's velocity does. Every collider then meets a particle as a solid ball
// `contact_scale` of its size in radius rather than as a point:
// - one arriving slower than `rest_speed` (units per second) stops rather
//   than bouncing, so a pile settles instead of buzzing;
// - the collider's friction grips the ball's surface. Its grip is limited
//   by the push, as Coulomb friction's is: friction f takes at most
//   f / (1 - f) of the speed the contact pushes back with off the slip, so
//   a resting piece slides to a stop and full friction grips at once;
// - `roundness` (0 to 1) is how readily a gripped piece rolls. At 1 it is a
//   solid ball: sliding becomes rolling and spin throws it along. At 0 it is
//   a flat chip: grip only slows its sliding, and each contact takes its
//   spin away. Between, a contact keeps that share of the spin.
// At most one per emitter.
struct RigidBody final {
    static constexpr std::string_view kind = "rigid_body";
    ModuleId id{};
    FloatInput angular_drag{0.5F};
    float contact_scale{0.5F};
    FloatInput rest_speed{0.2F};
    FloatInput roundness{1.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("angular_drag", self.angular_drag);
        visit("contact_scale", self.contact_scale);
        visit("rest_speed", self.rest_speed);
        visit("roundness", self.roundness);
    }
    friend bool operator==(const RigidBody&, const RigidBody&) = default;
};

// The body a rigid physics particle is: the convex hull of the renderer's
// mesh, a sphere or a cube, each scaled by the particle's size. Auto takes
// the hull for mesh geometry and a sphere otherwise.
enum class RigidShape {
    Auto,
    Hull,
    Sphere,
    Box,
};

// One kind of body a rigid physics emitter's particles can be. The emitter's
// own shape and material are kind 0; every kind listed in
// RigidPhysics::bodies is another, and a spawn module's `body` names the kind
// its newborns are. So a wall of crates, the pieces a crate breaks into and
// the ball that broke them are one world's bodies, each with its own shape
// and how it answers a knock.
// - **Its shape:** as the emitter's, scaled by each particle's size. `mesh`
//   is the model a `hull` kind wraps ("meshes/<name>" in the package); empty
//   wraps the renderer's own mesh, which is what the emitter's shape does.
// - **What it does not change:** the renderer draws one mesh for the whole
//   emitter, so a kind decides what a body collides as and not what it looks
//   like. Damping and sleep are the emitter's.
struct RigidBodyKind final {
    RigidShape shape{RigidShape::Auto};
    std::string mesh;
    float density{1000.0F};
    float friction{0.5F};
    float restitution{0.2F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("shape", self.shape);
        visit("mesh", self.mesh);
        visit("density", self.density);
        visit("friction", self.friction);
        visit("restitution", self.restitution);
    }
    friend bool operator==(const RigidBodyKind&, const RigidBodyKind&) = default;
};

// How a prop moves. Fixed stands still for ever, and is the only kind that
// may collide as its own model. Driven is moved by the effect along the one
// move the prop describes, shoving whatever is in its way and never being
// pushed back, so it can tip a pile over. Loose is an ordinary body: gravity
// and other bodies move it.
enum class PropMotion {
    Fixed,
    Driven,
    Loose,
};

// What a prop collides as.
// - `Mesh` is the model's own triangles, exactly, hollows and all. Only a
//   Fixed prop may be one: a collision mesh has no volume to take mass from
//   and no inside, so an engine cannot move one (Jolt says as much through
//   Shape::MustBeStatic). A Mesh prop that moves is refused rather than
//   quietly approximated, or worse, given an infinite inverse mass.
// - `Hull` is the convex wrapping of the model, tight on a solid lump and
//   wrong on anything hollow (a bucket becomes a tub).
// - The rest are generated from their measurements and need no model: a
//   Loose prop that must be hollow is built from a few convex pieces, which
//   is what a bucket, a crate, a barrel or a pipe nearly is.
enum class PropShape {
    Mesh,
    Hull,
    Sphere,
    Box,
    Cylinder,
    HollowBox,
    HollowCylinder,
    Cone,
    Pipe,
};

// An object an artist placed in a rigid physics emitter's world: a bucket for
// debris to spill out of, a wall to pile against, a crate to knock over.
// Unlike a particle, a prop is authored rather than born. It stands in the
// world from the first step, in the pose given here, and never dies.
// - **Its body:** `shape` at `size`, of `density`, with this `friction` and
//   `restitution`. A model is held in the package with its longest side one
//   unit long, so `size` is that side's length.
// - **Its shape:** `mesh` names the model for `Mesh` and `Hull`. The
//   generated shapes read `half_size`, `radius`, `height`, `wall` and
//   `segments` instead, and name no model at all.
// - **Driven:** the prop stands at `position` and `turn` until `move_from`
//   seconds, moves to `to_position` and `to_turn` over the next `move_over`,
//   and stays there. A `move_over` of 0 puts it there at once.
// - **Determinism:** props enter the world before any particle, in the order
//   they are listed here, and never change slot.
struct RigidProp final {
    std::string mesh;
    PropShape shape{PropShape::Mesh};
    PropMotion motion{PropMotion::Fixed};
    Vec3 position{};
    Vec3 turn{};  // degrees, turning about Y, then X, then Z
    float size{1.0F};
    Vec3 half_size{0.5F, 0.5F, 0.5F};
    float radius{0.5F};
    float height{1.0F};
    float wall{0.05F};
    std::uint32_t segments{12U};
    float density{1000.0F};
    float friction{0.5F};
    float restitution{0.2F};
    Vec3 to_position{};
    Vec3 to_turn{};
    float move_from{0.0F};
    float move_over{0.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("mesh", self.mesh);
        visit("shape", self.shape);
        visit("motion", self.motion);
        visit("position", self.position);
        visit("turn", self.turn);
        visit("size", self.size);
        visit("half_size", self.half_size);
        visit("radius", self.radius);
        visit("height", self.height);
        visit("wall", self.wall);
        visit("segments", self.segments);
        visit("density", self.density);
        visit("friction", self.friction);
        visit("restitution", self.restitution);
        visit("to_position", self.to_position);
        visit("to_turn", self.to_turn);
        visit("move_from", self.move_from);
        visit("move_over", self.move_over);
    }
    friend bool operator==(const RigidProp&, const RigidProp&) = default;
};

// A prop's `turn` as a quaternion: degrees about Y, then X, then Z, so the
// three read as yaw, pitch and roll. One definition, so what an engine does
// and what an editor draws over the picture cannot disagree.
[[nodiscard]] Quat prop_orientation(Vec3 degrees) noexcept;

// Makes the emitter's particles bodies in a rigid-body physics engine on the
// CPU (the host's RigidSolver, see rigid_solver.hpp; ajfx::rigid provides
// Jolt's). Unlike RigidBody, they collide with each other: debris piles up,
// knocks other debris about and comes to rest.
// - **Bodies:** each is `shape` at the particle's size from birth, of
//   `density`, with this `friction` and `restitution`. A sphere is `size`
//   across; a cube is `size` on a side; a hull is the mesh scaled by `size`.
// - **What acts on them:** Gravity (bound or not); Drag, unbound, as linear
//   damping; and the emitter's plane, sphere and box colliders, which become
//   unmoving bodies with their own restitution and friction. Bound collider
//   inputs, `kill`, keeping particles inside a sphere, distance fields,
//   other forces and SizeOverLife are refused: an engine body cannot follow
//   them.
// - **Rest:** with `allow_sleep`, bodies that have come to rest sleep until
//   something knocks them, which costs nothing while they lie still.
// - **More kinds:** `bodies` are the other kinds of body this emitter's
//   particles can be (see RigidBodyKind), each with its own shape and
//   material. A spawn module's `body` names the kind its newborns are, 0
//   being the shape and material here.
// - **Props:** `props` are the objects the artist placed in this world (see
//   RigidProp). They stand in it from the first step and the particles meet
//   them as they meet each other.
// - **Determinism:** props enter the world first, in order, then bodies are
//   created and removed in birth order in fixed slots, and the engine's
//   state travels in checkpoints, so every node and every seek computes the
//   same bits (for the same build of the solver).
// At most one per emitter, and the emitter holds at most max_rigid_capacity
// particles and max_rigid_props props.
struct RigidPhysics final {
    static constexpr std::string_view kind = "rigid_physics";
    ModuleId id{};
    RigidShape shape{RigidShape::Auto};
    float density{1000.0F};
    float friction{0.5F};
    float restitution{0.2F};
    float angular_damping{0.05F};
    bool allow_sleep{true};
    std::vector<RigidBodyKind> bodies;
    std::vector<RigidProp> props;

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("shape", self.shape);
        visit("density", self.density);
        visit("friction", self.friction);
        visit("restitution", self.restitution);
        visit("angular_damping", self.angular_damping);
        visit("allow_sleep", self.allow_sleep);
        visit("bodies", self.bodies);
        visit("props", self.props);
    }
    friend bool operator==(const RigidPhysics&, const RigidPhysics&) = default;
};

inline constexpr std::uint32_t max_rigid_capacity = 65536U;
// Kinds of body beyond the emitter's own. Each is a shape to build and
// another material; a handful is a scene, and a world is not a shop.
inline constexpr std::size_t max_rigid_body_kinds = 15U;
// Props stand in the world for the whole effect, so a world full of them
// costs every step. A few hundred is a stage; thousands are debris.
inline constexpr std::size_t max_rigid_props = 256U;

// A key at normalized age `t` (0 at birth, 1 at death). Keys are sorted by t;
// values are linear between keys and held beyond the ends.
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

using UpdateModule = std::variant<Gravity, Drag, CurlNoise, Vortex, Attractor, Converge, PlaneCollision,
                                  SphereCollision, BoxCollision, SdfCollision, ColorOverLife, SizeOverLife, RigidBody,
                                  RigidPhysics>;

// Collision modules constrain positions after integration rather than
// accelerating particles.
[[nodiscard]] constexpr bool is_collision(const UpdateModule& module) noexcept {
    return std::holds_alternative<PlaneCollision>(module) || std::holds_alternative<SphereCollision>(module) ||
           std::holds_alternative<BoxCollision>(module) || std::holds_alternative<SdfCollision>(module);
}

// Forces run before integration; everything else after it.
[[nodiscard]] constexpr bool is_force(const UpdateModule& module) noexcept {
    return !is_collision(module) && !std::holds_alternative<ColorOverLife>(module) &&
           !std::holds_alternative<SizeOverLife>(module) && !std::holds_alternative<RigidBody>(module) &&
           !std::holds_alternative<RigidPhysics>(module);
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

// What a sprite renderer draws: a quad per particle; one ribbon through the
// emitter's live particles in birth order (oldest first), each end as wide as
// its particle and turned to the camera across the trail; or the emitter's
// mesh per particle (see mesh.hpp), scaled by its size and turned by its
// orientation when it has one (InitTumble, RigidBody); otherwise its +Z
// along its velocity with velocity facing, or spun about Y by its rotation.
// Sprites and ribbons do not show an orientation.
enum class Geometry {
    Sprites,
    Ribbon,
    Mesh,
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
    // the strip); a texture runs along it by each end's life. A mesh
    // ("meshes/<name>" in the package) takes its texture by its own
    // coordinates and is shaded by a light at the camera.
    Geometry geometry{Geometry::Sprites};
    std::string mesh;
    // Particle lights: point lights in the host's lit scene, carried by some
    // of the emitter's particles. A particle carries one for its whole life
    // or not at all: every light_spacing()-th particle by birth does, so that
    // at most `light_count` (up to 64) do at once and every node picks the
    // same ones. Each light's intensity and reach are picked between their
    // min and max for its particle. Its colour is the particle's, or with
    // `light_own_color` the `light_colors` keys over the particle's life (by
    // their alpha), and `light_curve` scales its intensity over that life.
    // Zero gives none. A host without a lit scene ignores them.
    static constexpr std::size_t max_light_keys = 8U;
    std::uint32_t light_count{0U};
    float light_intensity_min{1.0F};
    float light_intensity_max{1.0F};
    float light_range_min{1.0F};
    float light_range_max{1.0F};
    bool light_own_color{false};
    std::vector<ColorKey> light_colors{{0.0F, Color{}}, {1.0F, Color{}}};
    std::vector<CurveKey> light_curve{{0.0F, 1.0F}, {1.0F, 1.0F}};

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
        visit("mesh", self.mesh);
        visit("light_count", self.light_count);
        visit("light_intensity_min", self.light_intensity_min);
        visit("light_intensity_max", self.light_intensity_max);
        visit("light_range_min", self.light_range_min);
        visit("light_range_max", self.light_range_max);
        visit("light_own_color", self.light_own_color);
        visit("light_colors", self.light_colors);
        visit("light_curve", self.light_curve);
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
    static constexpr std::string_view names[] = {"sprites", "ribbon", "mesh"};
};
template <>
struct EnumNames<RigidShape> {
    static constexpr std::string_view names[] = {"auto", "hull", "sphere", "box"};
};
template <>
struct EnumNames<PileShape> {
    static constexpr std::string_view names[] = {"heap", "box"};
};
template <>
struct EnumNames<PropMotion> {
    static constexpr std::string_view names[] = {"fixed", "driven", "loose"};
};
template <>
struct EnumNames<PropShape> {
    static constexpr std::string_view names[] = {"mesh",     "hull",            "sphere", "box", "cylinder",
                                                 "hollow_box", "hollow_cylinder", "cone",   "pipe"};
};

} // namespace ajfx
