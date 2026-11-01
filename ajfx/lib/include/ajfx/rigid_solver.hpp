#pragma once

#include <ajfx/math.hpp>
#include <ajfx/result.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
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

// What every body of one emitter's world is made of.
struct RigidWorldDesc final {
    enum class Shape {
        Hull,
        Sphere,
        Box,
    };
    // Bodies alive at once; slots run from 0 to capacity - 1.
    std::uint32_t capacity{0U};
    float step_seconds{0.0F};
    Shape shape{Shape::Sphere};
    // For Hull: the points the hull wraps, at size 1.
    std::vector<Vec3> hull_points;
    float density{1000.0F};
    float friction{0.5F};
    float restitution{0.2F};
    float linear_damping{0.0F};
    float angular_damping{0.05F};
    bool allow_sleep{true};
    std::vector<RigidStatic> statics;
    // The artist's props, which enter the world before any body, in order.
    std::vector<RigidPropDesc> props;
};

// A body as a particle is born, and as the solver reports it after a step.
struct RigidBodyState final {
    Vec3 position{};
    Quat orientation{};
    Vec3 velocity{};
    Vec3 angular_velocity{};

    friend bool operator==(const RigidBodyState&, const RigidBodyState&) = default;
};

class RigidWorld {
public:
    virtual ~RigidWorld() = default;

    // Adds a body of the world's shape at `size` in `slot`, which is free.
    // An `asleep` body stays where it is, without falling, until it is woken
    // or another body knocks into it.
    virtual void add(std::uint32_t slot, float size, const RigidBodyState& state, bool asleep) = 0;
    // Wakes the bodies in `slots`, given in ascending order.
    virtual void wake(std::span<const std::uint32_t> slots) = 0;
    // Removes the body in `slot`.
    virtual void remove(std::uint32_t slot) = 0;
    // Puts the Driven prop in `index` at `position` and `orientation`. Called
    // before step, which then moves whatever the prop has moved into. Fixed
    // and Loose props are never given a pose.
    virtual void set_prop_pose(std::size_t index, Vec3 position, Quat orientation) = 0;
    // Advances by the world's step under `gravity`.
    virtual void step(Vec3 gravity) = 0;
    [[nodiscard]] virtual RigidBodyState body(std::uint32_t slot) const = 0;
    // Where the prop in `index` stands, for a host to draw it by.
    [[nodiscard]] virtual RigidBodyState prop(std::size_t index) const = 0;
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
