#pragma once

#include <ajfx/math.hpp>
#include <ajfx/parameters.hpp>

#include <cstddef>
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

// Which of an emitter's particles a module acts on: its `only`. One emitter
// can then hold several kinds of thing that start and look different -- a
// rigid physics world whose crates are brown and whose hot fragments glow, or
// sparks and dust born from one emitter's contacts -- with an init module or
// an appearance curve for each.
// - `sources` names spawn modules by their place in the emitter's spawn stack
//   (spawn_source of a particle's spawn index), and piece_spawn_source
//   (effect.hpp) for the pieces broken bodies become.
// - `kinds` names body kinds of a rigid physics emitter (0 being its own).
// - A particle is in scope when every list that is not empty names it, so an
//   empty scope is every particle, which is what a module without one has
//   always acted on. A particle no Lifetime is in scope of is refused by
//   validation; one no other init module is in scope of keeps that module's
//   default, as it would without the module.
// Scopes are data: the GPU kernels read them each step, so editing one needs
// no kernels compiled again. On the GPU a particle emitter has no body kinds,
// so `kinds` is only for rigid physics emitters, which the CPU simulates.
struct Scope final {
    std::vector<std::uint32_t> sources;
    std::vector<std::uint32_t> kinds;

    [[nodiscard]] bool empty() const noexcept { return sources.empty() && kinds.empty(); }

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("sources", self.sources);
        visit("kinds", self.kinds);
    }
    friend bool operator==(const Scope&, const Scope&) = default;
};

// Entries one scope list may hold: every spawn source an emitter can have,
// pieces included, or every body kind.
inline constexpr std::size_t max_scope_entries = 256U;

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
    // One body whose middle is `center`, as an object placed by hand: a
    // count of 1, with no lattice under it to lift it off the point.
    Single,
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
//   heap is not a perfect stack. A single body is not stacked: its middle
//   is `center` itself.
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
    Vec3 initial_velocity{};
    Vec3 turn{}; // degrees, same convention as RigidProp

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
        visit("initial_velocity", self.initial_velocity);
        visit("turn", self.turn);
    }
    friend bool operator==(const SpawnPile&, const SpawnPile&) = default;
};

// Particles born where another emitter's bodies hit something: dust where
// pieces land, sparks where metal strikes. `emitter` names a rigid physics
// emitter; every step its contacts (RigidContact) are read in their own
// order, after its world has stepped, and each that qualifies bears
// particles at its point.
// - **Which contacts:** those closing at `min_speed` or faster, only those
//   that began this step with `new_only` (a hit rather than a scrape), and
//   with a non-empty `bodies` scope only those where a body of the named
//   emitter in that scope is one of the two sides (its sources and kinds are
//   the named emitter's).
// - **How many:** `per_hit` plus `per_speed` for every unit a second of the
//   contact's speed, rounded up or down by a hash of the step and the
//   contact's place in the list, so a hit twice as hard bears about twice as
//   many. At most `max_per_step` a step, taking contacts in order; the rest
//   bear none. That bound is what validation proves the capacity against.
// - **How they start:** at the contact's point, at age 0, then set up by the
//   init modules as any newborn; a ShapeLocation is then an offset around the
//   point and a Velocity adds to what the contact gives. The contact gives
//   `along_normal` of its speed along its normal, turned to point up (away
//   from a floor), and `along_surface` of it along the surface a random way,
//   the two together scattered within `spread` degrees.
// The world collects contacts as slow as the slowest reader asks for
// (contact_floor), whether the host asked for contacts or not. A particle
// emitter only: bodies are not born of contacts. The named emitter steps
// first, as every rigid physics emitter does, so the contacts are this
// step's.
struct SpawnFromContacts final {
    static constexpr std::string_view kind = "spawn_from_contacts";
    ModuleId id{};
    std::string emitter;
    float min_speed{1.0F};
    bool new_only{true};
    float per_hit{0.0F};
    float per_speed{2.0F};
    std::uint32_t max_per_step{64U};
    float along_normal{0.2F};
    float along_surface{0.1F};
    float spread{30.0F};
    Scope bodies;

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("emitter", self.emitter);
        visit("min_speed", self.min_speed);
        visit("new_only", self.new_only);
        visit("per_hit", self.per_hit);
        visit("per_speed", self.per_speed);
        visit("max_per_step", self.max_per_step);
        visit("along_normal", self.along_normal);
        visit("along_surface", self.along_surface);
        visit("spread", self.spread);
        visit("bodies", self.bodies);
    }
    friend bool operator==(const SpawnFromContacts&, const SpawnFromContacts&) = default;
};

// Particles shed by another emitter's moving bodies: smoke trailing a
// thrown rock, embers off a burning plank. `emitter` names a rigid physics
// emitter; every step each of its live bodies, in its own order, that is in
// the `bodies` scope (its sources and kinds are the named emitter's) and
// moves at `min_speed` or faster sheds particles.
// - **How many:** `rate` plus `per_speed` for every unit a second of the
//   body's speed, per second, rounded up or down by a hash of the body and
//   the step, so a still body sheds nothing and a fast one a dense trail. At
//   most `max_per_step` a step in all, taking bodies in order.
// - **Where:** along the step's stretch of the body's path, each at the age
//   it reached by the end of the step, so a trail is continuous at any
//   speed; `surface` places it from the body's middle (0) out to the ball its
//   size spans (1), a random way.
// - **How they start:** with `inherit` of the body's velocity, then set up by
//   the init modules as any newborn (a ShapeLocation offsets, a Velocity
//   adds). A particle emitter only.
struct SpawnFromBodies final {
    static constexpr std::string_view kind = "spawn_from_bodies";
    ModuleId id{};
    std::string emitter;
    float min_speed{0.5F};
    float rate{0.0F};
    float per_speed{10.0F};
    std::uint32_t max_per_step{64U};
    float surface{1.0F};
    float inherit{0.0F};
    Scope bodies;

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("emitter", self.emitter);
        visit("min_speed", self.min_speed);
        visit("rate", self.rate);
        visit("per_speed", self.per_speed);
        visit("max_per_step", self.max_per_step);
        visit("surface", self.surface);
        visit("inherit", self.inherit);
        visit("bodies", self.bodies);
    }
    friend bool operator==(const SpawnFromBodies&, const SpawnFromBodies&) = default;
};

// Particles shed by another emitter's moving particles: smoke trailing an
// ember, sparks off a comet. `emitter` names a plain particle emitter (one
// with rigid physics has bodies, which SpawnFromBodies sheds from); every
// step each of its live particles, in its own order, that is in the
// `particles` scope (its sources are the named emitter's; it has no kinds)
// and moves at `min_speed` or faster sheds particles.
// - **How many:** `rate` plus `per_speed` for every unit a second of the
//   particle's speed, per second, rounded up or down by a hash of the
//   particle and the step, so a still particle sheds nothing and a fast one
//   a dense trail. At most `max_per_step` a step in all, taking particles
//   in order.
// - **Where:** along the step's stretch of the particle's path, each at the
//   age it reached by the end of the step, so a trail is continuous at any
//   speed.
// - **How they start:** with `inherit` of the particle's velocity, then set
//   up by the init modules as any newborn (a ShapeLocation offsets, a
//   Velocity adds). A particle emitter only.
// - **Order:** the named emitter steps before this one, so what it sheds
//   from is where this step left the particles, newborns included. An
//   emitter may not shed from itself, and a chain of emitters shedding from
//   one another may not close (validation refuses both). Within a step these
//   births come after every other spawn module's, in stack order among
//   themselves.
struct SpawnFromParticles final {
    static constexpr std::string_view kind = "spawn_from_particles";
    ModuleId id{};
    std::string emitter;
    float min_speed{0.5F};
    float rate{0.0F};
    float per_speed{10.0F};
    std::uint32_t max_per_step{64U};
    float inherit{0.0F};
    Scope particles;

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("emitter", self.emitter);
        visit("min_speed", self.min_speed);
        visit("rate", self.rate);
        visit("per_speed", self.per_speed);
        visit("max_per_step", self.max_per_step);
        visit("inherit", self.inherit);
        visit("particles", self.particles);
    }
    friend bool operator==(const SpawnFromParticles&, const SpawnFromParticles&) = default;
};

// Particles born of actions (ajfx/actions.hpp): what an artist placed,
// dropped or painted into the running effect, step by step. The module only
// declares that the emitter takes actions and how many; the actions
// themselves come from an ActionProvider the host gives the simulation, so
// acting never changes the effect. Its births take the module's place in
// the stack as their source, and each action's own serial as its count, so
// an action taken away or moved leaves every other one's particles as they
// were.
// - **Rigid physics emitters:** a Spawn action puts a body exactly where it
//   says, of the kind it names, moving as it says, asleep until its release;
//   Push, Remove and Steer act on any of the emitter's bodies. At most
//   `max_alive` of its births are alive at once: past that a spawn is
//   refused and counted (EmitterState::refused), never squeezed in.
// - **Particle emitters:** a Spawn action bears a particle at its position,
//   offset by the init modules, thrown by its velocity beside theirs. The
//   GPU runtime bears them too. Push, Remove and Steer act on bodies alone.
// - At most `max_per_step` births a step, whatever the actions ask.
// - At most one per emitter.
struct SpawnActions final {
    static constexpr std::string_view kind = "spawn_actions";
    ModuleId id{};
    std::uint32_t max_per_step{64U};
    std::uint32_t max_alive{1024U};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("max_per_step", self.max_per_step);
        visit("max_alive", self.max_alive);
    }
    friend bool operator==(const SpawnActions&, const SpawnActions&) = default;
};

using SpawnModule = std::variant<SpawnRate, SpawnBurst, SpawnPile, SpawnFromContacts, SpawnFromBodies,
                                 SpawnFromParticles, SpawnActions>;

// The body kind a spawn module's newborns are: its `body`, or 0 for a module
// that has none (it bears particles, not bodies).
template <class Module>
[[nodiscard]] constexpr std::uint32_t spawn_body(const Module& module) noexcept {
    if constexpr (requires { module.body; }) {
        return module.body;
    } else {
        static_cast<void>(module);
        return 0U;
    }
}

// ---------------------------------------------------------------- init

// Seconds, uniformly between min and max. Required on any emitter that spawns.
struct Lifetime final {
    static constexpr std::string_view kind = "lifetime";
    ModuleId id{};
    FloatInput min{1.0F};
    FloatInput max{1.0F};

    // The particles it sets up (see Scope); empty is every one.
    Scope only{};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("min", self.min);
        visit("max", self.max);
        visit("only", self.only);
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

    // The particles it sets up (see Scope); empty is every one.
    Scope only{};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("shape", self.shape);
        visit("center", self.center);
        visit("radius_min", self.radius_min);
        visit("radius_max", self.radius_max);
        visit("size", self.size);
        visit("surface_only", self.surface_only);
        visit("only", self.only);
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

    // The particles it sets up (see Scope); empty is every one.
    Scope only{};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("direction", self.direction);
        visit("speed_min", self.speed_min);
        visit("speed_max", self.speed_max);
        visit("spread", self.spread);
        visit("spread_min", self.spread_min);
        visit("only", self.only);
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

    // The particles it sets up (see Scope); empty is every one.
    Scope only{};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("a", self.a);
        visit("b", self.b);
        visit("brightness_min", self.brightness_min);
        visit("brightness_max", self.brightness_max);
        visit("only", self.only);
    }
    friend bool operator==(const InitColor&, const InitColor&) = default;
};

// World-space sprite size. Size-over-life multiplies this.
struct InitSize final {
    static constexpr std::string_view kind = "size";
    ModuleId id{};
    FloatInput min{0.1F};
    FloatInput max{0.1F};

    // The particles it sets up (see Scope); empty is every one.
    Scope only{};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("min", self.min);
        visit("max", self.max);
        visit("only", self.only);
    }
    friend bool operator==(const InitSize&, const InitSize&) = default;
};

// Sprite rotation, degrees.
struct InitRotation final {
    static constexpr std::string_view kind = "rotation";
    ModuleId id{};
    FloatInput min{0.0F};
    FloatInput max{360.0F};

    // The particles it sets up (see Scope); empty is every one.
    Scope only{};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("min", self.min);
        visit("max", self.max);
        visit("only", self.only);
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

    // The particles it sets up (see Scope); empty is every one.
    Scope only{};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("speed_min", self.speed_min);
        visit("speed_max", self.speed_max);
        visit("random_orientation", self.random_orientation);
        visit("only", self.only);
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
// Forces (gravity, drag, curl noise, vortex, attractor, converge, shockwave,
// wake, volume advect) change
// velocity and run first, in stack order. Position is then integrated. Turning (rigid
// body), constraints (the colliders), deposits into grid volumes (volume
// source) and appearance (colour and size over life) run after, in stack
// order.
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

// A blast: at `time` seconds, everything within `radius` of `center` is
// thrown straight out from it, hardest at the centre and fading linearly to
// nothing at the edge. `power` is the speed (units per second) it gives at
// the centre, whatever the mass of what it throws, so a blast reads the same
// on a spark, a crate or a splinter.
// - **Set once:** an effect lists its blasts (Effect::blasts), each by name,
//   and every module that answers one names it: Shockwave throws an
//   emitter's particles, and in a rigid physics emitter its bodies and loose
//   props, and Break::blast_threshold shatters the bodies thrown hardest. A
//   host or an editor sets a blast in one place and the dust, the debris and
//   what breaks all follow it.
// - **When:** in one step, the first that starts at or after `time`
//   (blast_tick), so every node and every seek finds it in the same step.
// - **How hard:** blast_push gives what it does to whatever stands at a
//   point. The CPU reference, the GPU kernels and the rigid world all take
//   it from there.
// Its values are authored rather than bound to parameters: a blast is an
// event of the effect, as a prop is a thing in it.
struct Blast final {
    std::string name;
    Vec3 center{};
    float power{10.0F};
    float radius{3.0F};
    float time{0.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("name", self.name);
        visit("center", self.center);
        visit("power", self.power);
        visit("radius", self.radius);
        visit("time", self.time);
    }
    friend bool operator==(const Blast&, const Blast&) = default;
};

// Blasts one effect may list.
inline constexpr std::size_t max_blasts = 64U;

// A grid volume: a dense box of cells the effect lists (Effect::volumes,
// EffectBuilder::volume) that particles feed (VolumeSource) and are carried
// by (VolumeAdvect). It is an Eulerian smoke grid, stepped once per
// simulation step, holding per cell a density, a temperature and a velocity,
// all three collocated at the cell's centre (no staggered faces). The grid
// stands at `origin` and spans nx by ny by nz cells of `cell_size`.
// - **Each step, in this order:** the step's deposits are injected;
//   buoyancy lifts by `buoyancy` times the temperature and sinks by `weight`
//   times the density (along +Y and -Y); vorticity confinement of
//   `vorticity` (0 is off) sharpens the eddies; velocity and both scalars
//   are advected semi-Lagrangially by the velocity; the velocity is made
//   divergence-free by `pressure_iterations` Jacobi iterations from zero
//   pressure (walls all round); then each field loses its dissipation of
//   itself per second (as Drag loses velocity: divided by 1 + k dt).
// - **Exact:** the arithmetic is fixed, and the GPU kernels run it
//   operation for operation as the CPU reference does, so a grid stepped on
//   either is the same grid, and a seek lands on the grid stepping would
//   have reached. Deposits are summed as fixed-point integers (see
//   VolumeSource), so their sum does not depend on the order particles are
//   taken in.
// - **Budget:** at most max_volume_size cells along any axis and
//   max_volume_cells in all, refused with the memory a larger grid would
//   need. The pressure solve is bounded by max_volume_pressure_iterations.
// - **Drawn:** the reference renderer raymarches the density under the
//   scene's light: `extinction` is how much light a unit of density takes
//   per unit of length, `albedo` the share of it scattered towards the
//   camera, in `color`. The simulation never reads those three.
struct Volume final {
    std::string name;
    Vec3 origin{};
    float cell_size{0.1F};
    std::uint32_t nx{16U};
    std::uint32_t ny{16U};
    std::uint32_t nz{16U};
    float buoyancy{1.0F};
    float weight{0.0F};
    float vorticity{0.0F};
    std::uint32_t pressure_iterations{20U};
    float density_dissipation{0.0F};
    float temperature_dissipation{0.0F};
    float velocity_dissipation{0.0F};
    float extinction{1.0F};
    float albedo{0.8F};
    Vec3 color{1.0F, 1.0F, 1.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("name", self.name);
        visit("origin", self.origin);
        visit("cell_size", self.cell_size);
        visit("nx", self.nx);
        visit("ny", self.ny);
        visit("nz", self.nz);
        visit("buoyancy", self.buoyancy);
        visit("weight", self.weight);
        visit("vorticity", self.vorticity);
        visit("pressure_iterations", self.pressure_iterations);
        visit("density_dissipation", self.density_dissipation);
        visit("temperature_dissipation", self.temperature_dissipation);
        visit("velocity_dissipation", self.velocity_dissipation);
        visit("extinction", self.extinction);
        visit("albedo", self.albedo);
        visit("color", self.color);
    }
    friend bool operator==(const Volume&, const Volume&) = default;
};

// Volumes one effect may list, cells along one axis of a grid, cells in one
// grid in all (a grid's state is 20 bytes a cell on the CPU; the GPU keeps
// about five times that for its working buffers), and Jacobi iterations one
// pressure solve may take.
inline constexpr std::size_t max_volumes = 16U;
inline constexpr std::uint32_t max_volume_size = 256U;
inline constexpr std::uint64_t max_volume_cells = 1ULL << 20U;
inline constexpr std::uint32_t max_volume_pressure_iterations = 256U;

// How many cells a grid has.
[[nodiscard]] constexpr std::uint64_t volume_cells(const Volume& volume) noexcept {
    return std::uint64_t{volume.nx} * volume.ny * volume.nz;
}

// A deposit is quantised to this before it is summed: deposits are added as
// 64-bit fixed-point integers (one unit is 2^-20), so the sum over a cell's
// particles is exact and the same whatever order they are taken in, on the
// CPU and the GPU alike. A particle may deposit up to 2047 units of anything
// in one step; more is clamped.
inline constexpr float volume_deposit_quantum = 1.0F / 1048576.0F;

// What a blast does to whatever stands at `at`: the way it is thrown (out
// from the centre; straight up at the centre itself) and the speed it is
// given there. Nothing beyond the radius.
struct BlastPush final {
    Vec3 direction{};
    float speed{0.0F};

    friend bool operator==(const BlastPush&, const BlastPush&) = default;
};

[[nodiscard]] BlastPush blast_push(const Blast& blast, Vec3 at) noexcept;

// The step a blast goes off in: the first that starts at or after its time.
[[nodiscard]] std::uint64_t blast_tick(const Blast& blast, std::uint32_t simulation_rate) noexcept;

// Throws the emitter's particles out from the blast it names (Effect::blasts)
// in the step it goes off: each takes blast_push's speed at where it stands,
// times its own factor between `scale_min` and `scale_max`, as every force
// takes one. In a rigid physics emitter it throws the bodies and the loose
// props instead (a prop takes the blast as it is, having no factor of its
// own), and a body of a kind whose Break has a `blast_threshold` comes apart
// first if it is thrown that hard.
struct Shockwave final {
    static constexpr std::string_view kind = "shockwave";
    ModuleId id{};
    std::string blast;
    FloatInput scale_min{1.0F};
    FloatInput scale_max{1.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("blast", self.blast);
        visit("scale_min", self.scale_min);
        visit("scale_max", self.scale_max);
    }
    friend bool operator==(const Shockwave&, const Shockwave&) = default;
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

// Carries the emitter's particles along with a grid volume's velocity
// (Effect::volumes, named by `volume`): smoke riding the air it heated,
// embers drawn up a plume. Each step a particle inside the grid takes
// `strength` times the step (at most all) of the way to the velocity the grid
// samples trilinearly at it, times its own factor between `scale_min` and
// `scale_max`, as every force takes one. Outside the grid nothing acts. A
// force, so it runs before integration; it reads the grid as the step began,
// before this step's deposits move it.
struct VolumeAdvect final {
    static constexpr std::string_view kind = "volume_advect";
    ModuleId id{};
    std::string volume;
    FloatInput strength{10.0F};
    FloatInput scale_min{1.0F};
    FloatInput scale_max{1.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("volume", self.volume);
        visit("strength", self.strength);
        visit("scale_min", self.scale_min);
        visit("scale_max", self.scale_max);
    }
    friend bool operator==(const VolumeAdvect&, const VolumeAdvect&) = default;
};

// Feeds a grid volume (Effect::volumes, named by `volume`) from the emitter's
// particles: each step every particle inside the grid deposits, into the
// cell it is in, `density` and `temperature` per second, and `velocity` of
// its own velocity per second as momentum, all three times `over_life` at
// its age. Runs after integration, in stack order, from where the step left
// the particle; the grid takes the step's deposits when it steps, after
// every emitter. Each deposit is quantised (volume_deposit_quantum) and the
// cell's deposits are summed as integers, so the sum is exact and does not
// depend on the order the particles are taken in.
struct VolumeSource final {
    static constexpr std::string_view kind = "volume_source";
    ModuleId id{};
    std::string volume;
    FloatInput density{1.0F};
    FloatInput temperature{0.0F};
    FloatInput velocity{0.0F};
    std::vector<CurveKey> over_life{{0.0F, 1.0F}, {1.0F, 1.0F}};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("volume", self.volume);
        visit("density", self.density);
        visit("temperature", self.temperature);
        visit("velocity", self.velocity);
        visit("over_life", self.over_life);
    }
    friend bool operator==(const VolumeSource&, const VolumeSource&) = default;
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

// Particles kept outside a box (in the effect's own space), or with `inside`
// within it; `half_size` is half its extent on each of its own axes, and
// `turn` stands it as a prop is turned (degrees about Y, then X, then Z,
// prop_orientation), so a log lying across a fire is met lying there. A
// particle that gets inside leaves through the nearest face (x before y
// before z on a tie); one that gets out returns to the nearest point on the
// box. Responds as SphereCollision.
struct BoxCollision final {
    static constexpr std::string_view kind = "box_collision";
    ModuleId id{};
    Vec3Input center{};
    Vec3 half_size{0.5F, 0.5F, 0.5F};
    Vec3 turn{}; // degrees, as RigidProp's
    bool inside{false};
    FloatInput restitution{0.5F};
    FloatInput friction{0.0F};
    bool kill{false};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("center", self.center);
        visit("half_size", self.half_size);
        visit("turn", self.turn);
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

// Particles kept out of the bodies of another emitter's rigid physics world:
// snow that lands on the crates rather than falling through them. The named
// emitter's bodies move and come and go as its own physics decides, and these
// particles meet wherever they are this step.
// - **What a body is met as:** the ball or the box its size describes, turned
//   as the body is -- a box for a body kind that is a box, a ball for one
//   that is a ball or a model's hull. It is the body's own size, not its
//   model, so a thin panel is met as the box its size spans.
// - **Which bodies:** every live body of that emitter, in its own order, up
//   to its capacity, which is held to `max_body_colliders`. Each step the
//   bodies are sorted into a grid of cells about as wide as the largest of
//   them, and a particle tests only the bodies in the cells around it. It
//   still meets them in body order, the same bodies at the same moments as
//   meeting every body in turn would, so the grid changes the cost and not
//   one bit of the result (SimulationOptions::body_broadphase turns it off,
//   for proving that).
// - **Order:** every rigid physics emitter steps before any other
//   (Simulation::step), so the bodies met are this step's wherever the
//   named emitter is listed. An emitter may hold one of these per emitter
//   it meets, so one layer of dust can settle on everything solid in the
//   effect; the bodies are gathered once a step and shared between them.
// - The response is the plane's: pushed back out, with restitution, friction
//   and `kill` as there. Particles are not carried along by a body they rest
//   on: a body that moves out from under its snow leaves it behind.
struct BodyCollision final {
    static constexpr std::string_view kind = "body_collision";
    ModuleId id{};
    // The emitter whose bodies these particles hit.
    std::string emitter;
    FloatInput restitution{0.0F};
    FloatInput friction{0.0F};
    bool kill{false};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("emitter", self.emitter);
        visit("restitution", self.restitution);
        visit("friction", self.friction);
        visit("kill", self.kill);
    }
    friend bool operator==(const BodyCollision&, const BodyCollision&) = default;
};

// Bodies one emitter may hold while another collides with them (BodyCollision)
// or is stirred by them (Wake). The grid they are sorted into each step
// keeps a particle's cost to the bodies near it, so this bounds the memory
// and the sorting, not a product of particles and bodies.
inline constexpr std::uint32_t max_body_colliders = 16384U;

// Another emitter's moving bodies stir these particles: smoke pushed aside
// by a thrown crate, dust dragged along behind a rolling boulder. Each step
// a particle within reach of a moving body of the named rigid physics
// emitter is pulled towards that body's velocity, as Drag pulls towards
// rest:
// - **Reach:** `radius` times the body's half size (the ball or box its size
//   describes), from its middle. Strongest at the middle, fading linearly to
//   nothing at the edge, so a big body stirs a wide wake.
// - **How hard:** `strength` is how much of the difference the particle
//   loses per second at full strength: each step it takes k / (1 + k) of the
//   way to the body's velocity, k being strength times the step times the
//   fade, so any strength is stable. A particle's own factor between
//   `scale_min` and `scale_max` multiplies it, as every force's does.
// - **Which bodies:** those in the `bodies` scope (the named emitter's
//   sources and kinds) moving at `min_speed` or faster: a body at rest
//   leaves the smoke around it alone rather than holding it still.
// - **Overlapping wakes:** the particle follows the strongest wake at it
//   (the earlier body on a tie) rather than all of them added up, so a
//   crowd of bodies stirs no harder than one. That is also what makes the
//   result the same whichever order the bodies are looked at in.
// - Found through the same grid as BodyCollision, so bounded by
//   `max_body_colliders` too. A particle emitter only.
struct Wake final {
    static constexpr std::string_view kind = "wake";
    ModuleId id{};
    std::string emitter;
    FloatInput strength{4.0F};
    float radius{4.0F};
    float min_speed{0.1F};
    Scope bodies;
    FloatInput scale_min{1.0F};
    FloatInput scale_max{1.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("emitter", self.emitter);
        visit("strength", self.strength);
        visit("radius", self.radius);
        visit("min_speed", self.min_speed);
        visit("bodies", self.bodies);
        visit("scale_min", self.scale_min);
        visit("scale_max", self.scale_max);
    }
    friend bool operator==(const Wake&, const Wake&) = default;
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
// - **What it looks like:** `draw` is the model its bodies are drawn as, for
//   a renderer that draws a mesh per particle; empty draws the emitter's own
//   model, as every other body is drawn. A crate's panels collide and draw
//   as panels this way, without being an emitter of their own.
// - **How big it is:** `scale` multiplies the particle's size for bodies of
//   this kind, so a crate and the girder beside it are one world's bodies at
//   sizes of their own. An emitter has one size at birth (InitSize), which is
//   every body's; without this a kind could only be the size every other body
//   is. A body is therefore `shape` at the particle's size times `scale`, and
//   what collides, what is met as a BodyCollision and what is drawn all take
//   it, so the shape and the model cannot come apart. It must be above zero.
//   A piece a body breaks into takes BreakPiece::size of its parent as well,
//   which is a share of that parent rather than of this kind.
// - **What it does not change:** damping and sleep are the emitter's.
struct RigidBodyKind final {
    RigidShape shape{RigidShape::Auto};
    std::string mesh;
    std::string draw;
    float scale{1.0F};
    float density{1000.0F};
    float friction{0.5F};
    float restitution{0.2F};
    // Draw-time surface assignment. Never enters the solver or motion cache.
    Color draw_tint{};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("shape", self.shape);
        visit("mesh", self.mesh);
        visit("draw", self.draw);
        visit("scale", self.scale);
        visit("density", self.density);
        visit("friction", self.friction);
        visit("restitution", self.restitution);
        visit("draw_tint", self.draw_tint);
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
    Color draw_tint{};

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
        visit("draw_tint", self.draw_tint);
    }
    friend bool operator==(const RigidProp&, const RigidProp&) = default;
};

// A prop's `turn` as a quaternion: degrees about Y, then X, then Z, so the
// three read as yaw, pitch and roll. One definition, so what an engine does
// and what an editor draws over the picture cannot disagree.
[[nodiscard]] Quat prop_orientation(Vec3 degrees) noexcept;

// Holds the emitter's bodies to each other where they touch, so a pile of
// bricks is a wall rather than a heap waiting to happen. A join gives way
// when enough force goes through it, and the rest hold: hit a wall in the
// middle and the middle goes while the ends stand.
// - **What is joined:** every pair of bodies born in the same step whose
//   surfaces are within `range` of each other, in birth order. Bodies born
//   later are not joined to what is already there -- a wall is built at
//   once, and what lands on it afterwards is not part of it.
// - **What breaks one:** `strength` is the impulse a join carries before it
//   gives way, in kilogram-units a second. A join between heavy bodies
//   carries their weight, so a tall wall wants more than a low one.
// - **With breaking:** a body that comes apart (Break) takes its joins with
//   it, and the pieces it leaves are joined to nothing. The crack is in the
//   joins; the rubble is in the pieces.
// - Rigid physics emitters only, and at most one per emitter.
struct Glue final {
    static constexpr std::string_view kind = "glue";
    ModuleId id{};
    float range{0.02F};
    float strength{400.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("range", self.range);
        visit("strength", self.strength);
    }
    friend bool operator==(const Glue&, const Glue&) = default;
};

// Joins one emitter's world may hold. Every pair of bodies born together is
// considered, so this is what bounds both the looking and the holding.
inline constexpr std::size_t max_rigid_joins = 8192U;

// One piece a body comes apart into, placed in the parent's own frame.
// - **What it is:** a body of kind `body` (RigidPhysics::bodies, 0 being the
//   emitter's own), at `size` of the parent's size. A piece shaped like a
//   piece is a kind that collides as a shard model's hull.
// - **Where it is:** `offset` is a share of the parent's size, turned by the
//   parent's own turn, so a panel sits where it sat in the crate; `turn` is
//   the piece's own, in degrees about Y, then X, then Z, as a prop's is.
struct BreakPiece final {
    std::uint32_t body{0U};
    Vec3 offset{};
    Vec3 turn{};
    float size{0.5F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("body", self.body);
        visit("offset", self.offset);
        visit("turn", self.turn);
        visit("size", self.size);
    }
    friend bool operator==(const BreakPiece&, const BreakPiece&) = default;
};

// Makes one kind of the emitter's bodies breakable: a body of kind `body`
// that is hit at `threshold` or harder comes apart into `pieces`.
// - **When:** every step reports what its bodies hit and how hard (see
//   RigidContact). A body whose hardest contact that step closed at
//   `threshold` or more breaks at the end of it, before the step's births.
//   The threshold is fixed, not bound to a parameter: it is also what the
//   world filters contacts by, which is decided when the world is built.
// - **What the pieces take:** the parent's pose, with each piece's offset and
//   turn within it; the parent's velocity, and `scatter` units a second more,
//   outward along where the piece sat, with `spin` degrees a second about an
//   axis of the piece's own; and the parent's colour, age and lifetime, so
//   the pieces of a crate are pieces of that crate and not newborns. The
//   emitter's init modules do not run on them.
// - **Again:** a piece breaks in its turn if its own kind has a Break module
//   of its own. A cycle of kinds is refused: breaking has to end.
// - **Room:** a body breaks only if every one of its pieces fits the
//   emitter's capacity. One that does not stays whole, and the pieces it
//   could not bear are counted in EmitterState::refused rather than half of
//   it breaking.
// - **Blasted:** with a `blast_threshold` above zero, a body of the kind
//   that a Shockwave of the emitter throws at that speed or more (its
//   blast_push speed at the body, times the body's own factor) comes apart
//   at the start of the blast's step, before the blast throws anything, so
//   its pieces are thrown from where each of them sits. Bodies farther out,
//   thrown more gently, are only pushed. Zero leaves blasts to push.
// - Rigid physics emitters only, and at most one Break for a kind.
struct Break final {
    static constexpr std::string_view kind = "break";
    ModuleId id{};
    std::uint32_t body{0U};
    float threshold{3.0F};
    float blast_threshold{0.0F};
    float scatter{0.5F};
    float spin{90.0F};
    std::vector<BreakPiece> pieces;

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("body", self.body);
        visit("threshold", self.threshold);
        visit("blast_threshold", self.blast_threshold);
        visit("scatter", self.scatter);
        visit("spin", self.spin);
        visit("pieces", self.pieces);
    }
    friend bool operator==(const Break&, const Break&) = default;
};

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
// Pieces one body may come apart into. A crate is a few panels and slivers;
// a hundred of them is a simulation of its own. A model cut into cells makes
// one of these per cell, which is what sets the number.
inline constexpr std::size_t max_break_pieces = 64U;
// Kinds of body beyond the emitter's own. Each is a shape to build and
// another material; a handful is a scene, and a world is not a shop. A model
// cut into cells makes one per cell, so this holds as many as a body may
// come apart into.
inline constexpr std::size_t max_rigid_body_kinds = 256U;
// Props stand in the world for the whole effect, so a world full of them
// costs every step. A few hundred is a stage; thousands are debris.
inline constexpr std::size_t max_rigid_props = 256U;

// A key at normalized age `t` (0 at birth, 1 at death). Keys are sorted by t;
// values are linear between keys and held beyond the ends.
struct ColorOverLife final {
    static constexpr std::string_view kind = "color_over_life";
    ModuleId id{};
    std::vector<ColorKey> keys{{0.0F, Color{}}, {1.0F, Color{1.0F, 1.0F, 1.0F, 0.0F}}};

    // The particles it sets up (see Scope); empty is every one.
    Scope only{};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("keys", self.keys);
        visit("only", self.only);
    }
    friend bool operator==(const ColorOverLife&, const ColorOverLife&) = default;
};

struct SizeOverLife final {
    static constexpr std::string_view kind = "size_over_life";
    ModuleId id{};
    std::vector<CurveKey> keys{{0.0F, 1.0F}, {1.0F, 1.0F}};

    // The particles it sets up (see Scope); empty is every one.
    Scope only{};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("keys", self.keys);
        visit("only", self.only);
    }
    friend bool operator==(const SizeOverLife&, const SizeOverLife&) = default;
};

// A glow that cools: hot sparks, a fragment fresh from a blast, embers. Each
// particle in scope has a heat of `start` when it is born, halving every
// `half_life` seconds of its age, and glows by it on top of its colour.
// - **The glow:** a black-body-like ramp of the heat (0 to 1) -- black, deep
//   red, orange, yellow, white -- times `glow`, added to the particle's red,
//   green and blue (alpha untouched). Above 1 the glow stays white until the
//   heat falls through 1, so `start` above 1 holds a white heat for a while.
//   Colours above 1 are emission: what a host's renderer draws as light.
// - **Pure in age:** the heat at age a is start * 2^(-a / half_life), worked
//   out afresh each step rather than carried, so a seek, a cache and the GPU
//   all find it from the age alone, and it falls monotonically. A piece a
//   body breaks into takes its parent's age, so it is as hot as the parent
//   was; a fragment that should start hot is born hot (its own spawn module
//   or kind, scoped).
// - **With colour over life:** every heat is added after every colour over
//   life, wherever the modules stand in the stack, so the glow is on the
//   colour of that age.
// - In a rigid physics emitter too, scoped by body kind, so hot fragments and
//   cold crates are one world's bodies.
struct Heat final {
    static constexpr std::string_view kind = "heat";
    ModuleId id{};
    float start{1.0F};
    float half_life{0.5F};
    float glow{4.0F};
    Scope only{};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("start", self.start);
        visit("half_life", self.half_life);
        visit("glow", self.glow);
        visit("only", self.only);
    }
    friend bool operator==(const Heat&, const Heat&) = default;
};

// Wind on a rigid physics emitter's bodies: air moving at `velocity` (m/s)
// drags each body towards its own speed, as a sail is pushed.
// - **How hard:** `drag` (kg/m^3: half the air's density times a drag
//   coefficient; 0.6 is about a box in air) times the body's area (the
//   square of its size, its kind's scale applied) times the air's speed past
//   it, squared, along the way the air moves past it. The world divides that
//   force by the body's mass, so a light body (a cardboard box, a sheet) is
//   carried off where a heavy one hardly moves.
// - **Gusts:** with `gust` above 0 the wind's speed rises and falls by up to
//   that share of itself, smoothly, over about `gust_seconds`, the same for
//   every body: a function of the step and the module's id, never a clock.
// - **Resting bodies:** a sleeping body is woken only when the push could
//   slide it: more than its kind's friction times its weight, worked out
//   from its kind's density and its size. Heavy things at rest stay asleep
//   and cost nothing; awake ones are pushed whatever they weigh.
// Rigid physics emitters only; their props are left alone.
struct BodyWind final {
    static constexpr std::string_view kind = "body_wind";
    ModuleId id{};
    Vec3 velocity{4.0F, 0.0F, 0.0F};
    float drag{0.6F};
    float gust{0.0F};
    float gust_seconds{2.0F};

    template <class Self, class Visitor>
    static void fields(Self& self, Visitor&& visit) {
        visit("velocity", self.velocity);
        visit("drag", self.drag);
        visit("gust", self.gust);
        visit("gust_seconds", self.gust_seconds);
    }
    friend bool operator==(const BodyWind&, const BodyWind&) = default;
};

// Winds one rigid physics emitter may blow its bodies with.
inline constexpr std::size_t max_body_winds = 16U;

// The air's velocity a BodyWind blows with in step `tick` at
// `simulation_rate`: its velocity, times its gust at that step.
[[nodiscard]] Vec3 body_wind_at(const BodyWind& wind, std::uint64_t tick, std::uint32_t simulation_rate) noexcept;

using UpdateModule = std::variant<Gravity, Drag, CurlNoise, Vortex, Attractor, Converge, PlaneCollision,
                                  SphereCollision, BoxCollision, SdfCollision, BodyCollision, ColorOverLife,
                                  SizeOverLife, RigidBody, RigidPhysics, Break, Glue, Shockwave, Heat, Wake,
                                  VolumeAdvect, VolumeSource, BodyWind>;

// Collision modules constrain positions after integration rather than
// accelerating particles.
[[nodiscard]] constexpr bool is_collision(const UpdateModule& module) noexcept {
    return std::holds_alternative<PlaneCollision>(module) || std::holds_alternative<SphereCollision>(module) ||
           std::holds_alternative<BoxCollision>(module) || std::holds_alternative<SdfCollision>(module) ||
           std::holds_alternative<BodyCollision>(module);
}

// Forces run before integration; everything else after it.
[[nodiscard]] constexpr bool is_force(const UpdateModule& module) noexcept {
    return !is_collision(module) && !std::holds_alternative<ColorOverLife>(module) &&
           !std::holds_alternative<SizeOverLife>(module) && !std::holds_alternative<RigidBody>(module) &&
           !std::holds_alternative<RigidPhysics>(module) && !std::holds_alternative<Break>(module) &&
           !std::holds_alternative<Glue>(module) && !std::holds_alternative<Heat>(module) &&
           !std::holds_alternative<VolumeSource>(module) && !std::holds_alternative<BodyWind>(module);
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

// How the scene's light reaches an emitter's particles. The sprite's frame
// is its own: +X right, +Y up, +Z towards the camera.
enum class Lighting {
    // The particle's colour as it is: glows, sparks, anything emissive.
    Unlit,
    // Lit by the scene's lights as a rounded surface: a sprite takes a
    // normal that turns from facing the camera at its centre towards its
    // edge by `curvature`, a mesh keeps its own normals. Light arrives
    // through six directions (right, left, up, down, front, behind), the
    // same basis six-way maps use, so the two match in a mixed effect.
    Lit,
    // Lit through six-way lightmaps, for smoke and clouds rendered offline:
    // `texture` holds in RGB how much light each texel takes from the
    // right, from above and from the front (the camera), and `lightmap`
    // holds the same from the left, from below and from behind. The
    // texture's alpha is the coverage; the lightmap's alpha is unused.
    // Both share the flipbook grid. Sprites only.
    SixWay,
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
    // The scene's light on the particles (see Lighting). A lit particle is
    // its colour times the light it receives: with `Lit`, through a normal
    // that turns from the camera at the centre to `curvature` of the way
    // round a sphere at the edge (0 flat, 1 a full sphere); `wrap` carries
    // light past the terminator so a thin puff lit from behind still glows
    // at its rim (0 stops there, 1 reaches all the way round, at half
    // strength where the light is square to it); `ambient` scales
    // the sky and ground hemisphere light it takes; with `shadowed` it
    // takes the scene's directional shadows. Unlit ignores them all. A host
    // without a lit scene draws a lit particle under one light at the
    // camera, as the reference renderer does under its own light.
    Lighting lighting{Lighting::Unlit};
    std::string lightmap;
    float curvature{0.6F};
    float wrap{0.25F};
    float ambient{1.0F};
    bool shadowed{true};
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
    // Draw inputs: what a host multiplies every particle by as it draws it,
    // after the simulation (and after a cache) has had its say. Colour by
    // `tint` times `brightness`, alpha by `tint`'s alpha times `opacity`,
    // size by `size_scale`. The simulation never reads them, which is what
    // lets them follow playback-time parameters (ParameterTiming) that a
    // cache honours for free; `draw_adjustment` resolves them.
    ColorInput tint{Color{}};
    FloatInput brightness{1.0F};
    FloatInput opacity{1.0F};
    FloatInput size_scale{1.0F};

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
        visit("lighting", self.lighting);
        visit("lightmap", self.lightmap);
        visit("curvature", self.curvature);
        visit("wrap", self.wrap);
        visit("ambient", self.ambient);
        visit("shadowed", self.shadowed);
        visit("light_count", self.light_count);
        visit("light_intensity_min", self.light_intensity_min);
        visit("light_intensity_max", self.light_intensity_max);
        visit("light_range_min", self.light_range_min);
        visit("light_range_max", self.light_range_max);
        visit("light_own_color", self.light_own_color);
        visit("light_colors", self.light_colors);
        visit("light_curve", self.light_curve);
        visit("tint", self.tint);
        visit("brightness", self.brightness);
        visit("opacity", self.opacity);
        visit("size_scale", self.size_scale);
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
struct EnumNames<Lighting> {
    static constexpr std::string_view names[] = {"unlit", "lit", "six_way"};
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
    static constexpr std::string_view names[] = {"heap", "box", "single"};
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
