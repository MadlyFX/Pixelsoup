#pragma once

#include <ajfx/math.hpp>
#include <ajfx/result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <tuple>
#include <vector>

// The seam between ajfx and a rigid-body engine. ajfx itself depends on none:
// a host that wants RigidPhysics emitters hands Simulation and GpuSimulation
// a RigidSolver, such as ajfx::rigid::make_jolt_solver (ajfx/rigid.hpp).
//
// A world is built with its statics and its props, in the order the desc
// lists them and before any body exists. ajfx then drives it in a fixed order
// every step: removals oldest first, wakes, the driven props' new poses, one
// step, then additions in birth order into the lowest free slots. A solver
// must compute the same bits for the same calls on every machine that runs
// the same build, and `restore` into a world rebuilt with the same bodies
// must continue exactly as the saved world would have.
namespace ajfx {

// A static collider, resolved from the emitter's collision modules.
struct RigidStatic final {
    enum class Kind {
        // Everything on the far side of the plane through `center` facing
        // `normal` is solid.
        Plane,
        // A ball of `radius` at `center`.
        Sphere,
        // An axis-aligned box of `half_size` at `center`.
        Box,
    };
    Kind kind{Kind::Plane};
    Vec3 center{};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    Vec3 half_size{};
    float radius{0.0F};
    float friction{0.0F};
    float restitution{0.0F};
};

// What one side of a contact is: one of the emitter's bodies, one of the
// artist's props, or one of the statics its collision modules became. The
// index is that body's slot, or the prop's or static's place in the desc.
enum class RigidSide {
    Body,
    Prop,
    Static,
};

// A pair the step found touching, as collision detection saw them before the
// step resolved them, so `speed` is how hard they arrived rather than what is
// left of it afterwards.
// - **Its sides:** `a` is the lower of the two by (what it is, its index),
//   always, so a pair reads the same however the engine happened to order
//   it, and `normal` points from a towards b.
// - **Its order:** a step's contacts are sorted by `contact_before`. The
//   order is ajfx's own: an engine's internal order, and the thread count it
//   ran on, must not reach what is done with them.
// - **How many:** only pairs closing at `RigidWorldDesc::contact_min_speed`
//   or faster are reported, which is what keeps a pile at rest from
//   reporting at all.
struct RigidContact final {
    RigidSide a_is{RigidSide::Body};
    std::uint32_t a{0U};
    RigidSide b_is{RigidSide::Body};
    std::uint32_t b{0U};
    // Where they met, in the world, and the direction from a towards b.
    Vec3 point{};
    Vec3 normal{};
    // How fast they were closing along the normal when they were found: the
    // force of a hit, as far as ajfx reports one. Never below zero, since a
    // pair drifting apart is not closing.
    float speed{0.0F};
    // Whether this pair met this step, rather than having lain together
    // since an earlier one. A pile at rest keeps its contacts.
    bool began{false};

    friend bool operator==(const RigidContact&, const RigidContact&) = default;
};

// The order a step's contacts are reported in: by their sides, then where
// and which way they met, then how hard. Total, so sorting by it gives one
// answer whatever order the contacts were found in.
[[nodiscard]] inline bool contact_before(const RigidContact& l, const RigidContact& r) noexcept {
    const auto key = [](const RigidContact& c) {
        return std::tuple{c.a_is,    c.a,        c.b_is,     c.b,        c.point.x, c.point.y,
                          c.point.z, c.normal.x, c.normal.y, c.normal.z, c.speed};
    };
    return key(l) < key(r);
}

// An object the artist placed, resolved from the emitter's RigidProp list:
// the shape is chosen and measured, and the model, where one is used, is
// already here. A Driven prop is moved by the caller through set_prop_pose
// and is never moved by anything it hits.
struct RigidPropDesc final {
    enum class Shape {
        // The model's own triangles, hollows and all: `points` and
        // `indices`, three indices a triangle. Never Loose - a hollow shape
        // cannot move under forces.
        Mesh,
        // The convex wrapping of `points`.
        Hull,
        // Generated from the measurements below, and needing no model. The
        // hollow ones are compounds of convex pieces.
        Sphere,
        Box,
        Cylinder,
        HollowBox,
        HollowCylinder,
        Cone,
        Pipe,
    };
    enum class Motion {
        Fixed,
        Driven,
        Loose,
    };
    Shape shape{Shape::Hull};
    Motion motion{Motion::Fixed};
    // Mesh and Hull: the model at size 1, which `size` then scales.
    std::vector<Vec3> points;
    std::vector<std::uint32_t> indices;
    // The generated shapes' measurements, which `size` also scales.
    Vec3 half_size{};
    float radius{0.0F};
    float height{0.0F};
    float wall{0.0F};
    std::uint32_t segments{12U};
    float size{1.0F};
    // Where it stands before the first step.
    Vec3 position{};
    Quat orientation{};
    float density{1000.0F};
    float friction{0.5F};
    float restitution{0.2F};
};

// What the bodies of one emitter's world are made of.
struct RigidWorldDesc final {
    enum class Shape {
        Hull,
        Sphere,
        Box,
    };
    // One kind of body the emitter's particles are, chosen per body by
    // `add`, so that one world can hold crates, the pieces they break into
    // and the ball that broke them. Damping and sleep are the world's; shape
    // and material are a kind's.
    struct Body final {
        Shape shape{Shape::Sphere};
        // For Hull: the points the hull wraps, at size 1.
        std::vector<Vec3> hull_points;
        // Multiplies the size `add` is given, so a kind can be a size of its
        // own where the emitter has one size at birth for every body. A
        // solver applies it once, where it builds the body's shape, so every
        // way a body is made takes it. Above zero.
        float scale{1.0F};
        float density{1000.0F};
        float friction{0.5F};
        float restitution{0.2F};
    };
    // Bodies alive at once; slots run from 0 to capacity - 1.
    std::uint32_t capacity{0U};
    float step_seconds{0.0F};
    // The kinds, by their index. Never empty: kind 0 is the emitter's own,
    // and the kind every body is unless it was given another.
    std::vector<Body> bodies{Body{}};
    float linear_damping{0.0F};
    float angular_damping{0.05F};
    bool allow_sleep{true};
    std::vector<RigidStatic> statics;
    // The artist's props, which enter the world before any body, in order.
    std::vector<RigidPropDesc> props;
    // Whether each step reports the contacts it found (RigidWorld::contacts).
    // Finding them out of an engine costs, so a world reports none unless it
    // is asked to.
    bool report_contacts{false};
    // Only pairs closing at least this fast (units per second) are reported.
    // This is what bounds how many there are: bodies at rest close at
    // nothing, so a settled pile reports none however large it is.
    float contact_min_speed{0.0F};
    // At most this many are reported. What will not fit is counted
    // (RigidWorld::contacts_dropped) rather than quietly left out.
    std::uint32_t contact_limit{256U};
};

// Two bodies held to each other, by their slots, with `a` the lower of the
// two so a pair reads one way only. Which bodies are joined is ajfx's
// business; a world holds them until the force through one passes what it
// was given to hold.
// - **Its rest:** `at` and `turn` are where b sits in a's own frame at the
//   moment they were joined, and the join holds them that way. An engine
//   bakes a joint's rest from the poses the bodies are in when it is made,
//   so a world rebuilt from a checkpoint would otherwise bake a different
//   one and carry on differently: the rest travels with the join instead.
struct RigidJoin final {
    std::uint32_t a{0U};
    std::uint32_t b{0U};
    Vec3 at{};
    Quat turn{};

    friend bool operator==(const RigidJoin&, const RigidJoin&) = default;
};

// A body as a particle is born, and as the solver reports it after a step.
struct RigidBodyState final {
    Vec3 position{};
    Quat orientation{};
    Vec3 velocity{};
    Vec3 angular_velocity{};

    friend bool operator==(const RigidBodyState&, const RigidBodyState&) = default;
};

// Where b sits in a's own frame, which is what a join holds them at.
[[nodiscard]] constexpr RigidJoin rest_of(std::uint32_t a, std::uint32_t b, const RigidBodyState& first,
                                          const RigidBodyState& second) noexcept {
    const Quat back{-first.orientation.x, -first.orientation.y, -first.orientation.z, first.orientation.w};
    return RigidJoin{a, b, rotate(back, second.position - first.position), times(back, second.orientation)};
}

class RigidWorld {
public:
    virtual ~RigidWorld() = default;

    // Adds a body of `kind` (an index into the desc's bodies, clamped to
    // what it holds) at `size` in `slot`, which is free. An `asleep` body
    // stays where it is, without falling, until it is woken or another body
    // knocks into it.
    virtual void add(std::uint32_t slot, std::uint32_t kind, float size, const RigidBodyState& state,
                     bool asleep) = 0;
    // Wakes the bodies in `slots`, given in ascending order.
    virtual void wake(std::span<const std::uint32_t> slots) = 0;
    // Removes the body in `slot`.
    virtual void remove(std::uint32_t slot) = 0;
    // Holds `join`'s two bodies in the pose they are in now, until the
    // impulse through the join passes `strength`. Joins are held in the
    // order they are made and never reordered, since what a solver computes
    // depends on the order it holds them in.
    virtual void glue(const RigidJoin& join, float strength) = 0;
    // Drops that join, if it is held. A body leaving the world takes its
    // joins with it, so the caller ungles them before it removes the body.
    virtual void unglue(const RigidJoin& join) = 0;
    // The joins the world holds, in the world's own order, which is the
    // order they must be made again in to rebuild it. An engine keeps its
    // joints in an order of its own and renumbers them as they go, and it
    // reads a checkpoint back by that numbering, so the caller takes this
    // order rather than keeping one of its own.
    [[nodiscard]] virtual std::span<const RigidJoin> joins() const = 0;
    // Puts the Driven prop in `index` at `position` and `orientation`. Called
    // before step, which then moves whatever the prop has moved into. Fixed
    // and Loose props are never given a pose.
    virtual void set_prop_pose(std::size_t index, Vec3 position, Quat orientation) = 0;
    // Advances by the world's step under `gravity`.
    virtual void step(Vec3 gravity) = 0;
    [[nodiscard]] virtual RigidBodyState body(std::uint32_t slot) const = 0;
    // Where the prop in `index` stands, for a host to draw it by.
    [[nodiscard]] virtual RigidBodyState prop(std::size_t index) const = 0;
    // The contacts the last step found, in `contact_before`'s order. Empty
    // until a step has run, and for any world whose desc did not ask for
    // them. What it returns is good until the next step.
    [[nodiscard]] virtual std::span<const RigidContact> contacts() const = 0;
    // How many more the last step found and did not report, the desc's
    // contact_limit being full.
    [[nodiscard]] virtual std::uint32_t contacts_dropped() const = 0;
    // Everything the step depends on beyond the bodies' existence: poses,
    // velocities, sleep, contacts. Allocates; for checkpoints, not every step.
    [[nodiscard]] virtual std::vector<std::uint8_t> save() const = 0;
    // Puts back what `save` returned, into a world holding the same bodies
    // in the same slots. False if the bytes do not fit it.
    [[nodiscard]] virtual bool restore(std::span<const std::uint8_t> bytes) = 0;
};

class RigidSolver {
public:
    virtual ~RigidSolver() = default;

    // A world with `desc`'s statics and no bodies. Worlds of one solver may
    // share its threads, so step them from one thread at a time.
    [[nodiscard]] virtual Result<std::unique_ptr<RigidWorld>> create_world(const RigidWorldDesc& desc) const = 0;
};

} // namespace ajfx
