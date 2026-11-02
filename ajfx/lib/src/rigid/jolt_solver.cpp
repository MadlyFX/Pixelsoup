#include <ajfx/rigid.hpp>

// Jolt must come first; every other Jolt header assumes it.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuery.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace ajfx::rigid {
namespace {

// Jolt's allocator, factory and type registry are process-wide; made once
// and kept for the life of the process.
void register_jolt() {
    static std::once_flag once;
    std::call_once(once, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

namespace layers {
constexpr JPH::ObjectLayer still = 0;
constexpr JPH::ObjectLayer moving = 1;
} // namespace layers

// Statics meet moving bodies; moving bodies meet everything.
class PairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == layers::moving || b == layers::moving;
    }
};

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2U; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(layer));
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == JPH::BroadPhaseLayer(layers::moving) ? "moving" : "still";
    }
#endif
};

class LayerVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad) const override {
        return layer == layers::moving || broad == JPH::BroadPhaseLayer(layers::moving);
    }
};

JPH::Vec3 vec(Vec3 v) {
    return JPH::Vec3(v.x, v.y, v.z);
}

Vec3 vec(JPH::Vec3Arg v) {
    return Vec3{v.GetX(), v.GetY(), v.GetZ()};
}

// A world-space point, which is wider than a Vec3 in a double-precision
// build of Jolt and the same thing in this one.
Vec3 vec_of(JPH::RVec3Arg v) {
    return Vec3{static_cast<float>(v.GetX()), static_cast<float>(v.GetY()), static_cast<float>(v.GetZ())};
}

// A body smaller than this is made this size: a shape of no size is not one.
constexpr float smallest_size = 1.0e-3F;
// How far out a plane collider reaches from its centre.
constexpr float plane_extent = 1000.0F;
// How far beyond a removed body's bounds a body counts as touching it.
constexpr float touch_margin = 0.05F;
// The arm a join's twist is measured against, so a turn and a pull can be
// compared as one force: a join holding at a quarter of a unit.
constexpr float turn_arm = 0.25F;
// Sides a generated round prop is built from. Three is the fewest that
// encloses anything; past this the pieces cost more than the roundness.
constexpr std::uint32_t fewest_segments = 3U;
constexpr std::uint32_t most_segments = 64U;

constexpr float pi = 3.14159265358979323846F;

JPH::Vec3 at_least(JPH::Vec3Arg half) {
    return JPH::Vec3(std::max(half.GetX(), smallest_size), std::max(half.GetY(), smallest_size),
                     std::max(half.GetZ(), smallest_size));
}

// A box whose convex radius fits it. Jolt refuses a radius larger than the
// smallest half extent, and a hollow prop's wall is often very thin.
JPH::RefConst<JPH::Shape> box_shape(JPH::Vec3Arg half_in, float density) {
    const JPH::Vec3 half = at_least(half_in);
    const float smallest = std::min(half.GetX(), std::min(half.GetY(), half.GetZ()));
    auto* box = new JPH::BoxShape(half, std::min(JPH::cDefaultConvexRadius, 0.25F * smallest));
    box->SetDensity(density);
    return box;
}

// A cylinder whose convex radius fits it, for the same reason: a bucket's
// bottom is a disc far thinner than Jolt's default rounding.
JPH::RefConst<JPH::Shape> cylinder_shape(float half_height, float radius, float density) {
    const float h = std::max(half_height, smallest_size);
    const float r = std::max(radius, smallest_size);
    auto* cylinder = new JPH::CylinderShape(h, r, std::min(JPH::cDefaultConvexRadius, 0.25F * std::min(h, r)));
    cylinder->SetDensity(density);
    return cylinder;
}

// One convex piece of a generated prop, in the prop's own space.
struct Piece final {
    JPH::RefConst<JPH::Shape> shape;
    JPH::Vec3 at{JPH::Vec3::sZero()};
    JPH::Quat turn{JPH::Quat::sIdentity()};
};

// A ring of upright staves whose faces meet, enclosing `radius` with walls
// `wall` thick and `height` tall, centred on the prop's origin. This is what
// makes a bucket's side: a wall a body can be inside, out of convex pieces,
// since no engine can make one hollow shape move under forces.
void add_staves(std::vector<Piece>& out, float radius, float wall, float height, std::uint32_t segments,
                float centre_y, float density) {
    const float middle = std::max(radius - wall * 0.5F, smallest_size);
    // Half the chord each stave spans, so neighbours touch at their corners.
    const float half_chord = middle * std::tan(pi / static_cast<float>(segments));
    for (std::uint32_t i = 0U; i < segments; ++i) {
        const float angle = 2.0F * pi * static_cast<float>(i) / static_cast<float>(segments);
        out.push_back(Piece{box_shape(JPH::Vec3(half_chord, height * 0.5F, wall * 0.5F), density),
                            JPH::Vec3(std::sin(angle) * middle, centre_y, std::cos(angle) * middle),
                            JPH::Quat::sRotation(JPH::Vec3::sAxisY(), angle)});
    }
}

// The pieces a generated prop is made of, in a fixed order so every node
// builds the same compound. The hollow shapes are open at the top, which is
// what a bucket, a crate or a barrel is.
std::vector<Piece> prop_pieces(const RigidPropDesc& p) {
    const std::uint32_t segments = std::clamp(p.segments, fewest_segments, most_segments);
    const float wall = std::max(p.wall, smallest_size);
    const float radius = std::max(p.radius, smallest_size);
    const float height = std::max(p.height, smallest_size);
    const JPH::Vec3 half = at_least(vec(p.half_size));
    std::vector<Piece> pieces;
    switch (p.shape) {
    case RigidPropDesc::Shape::HollowBox: {
        const float floor = std::min(wall, half.GetY() * 2.0F);
        pieces.push_back(Piece{box_shape(JPH::Vec3(half.GetX(), floor * 0.5F, half.GetZ()), p.density),
                               JPH::Vec3(0.0F, -half.GetY() + floor * 0.5F, 0.0F)});
        const float side = std::max(half.GetY() - floor * 0.5F, smallest_size);
        const float mid = -half.GetY() + floor + side;
        const float inner_x = std::max(half.GetX() - wall, smallest_size);
        for (const float sx : {-1.0F, 1.0F}) {
            pieces.push_back(Piece{box_shape(JPH::Vec3(wall * 0.5F, side, half.GetZ()), p.density),
                                   JPH::Vec3(sx * (half.GetX() - wall * 0.5F), mid, 0.0F)});
        }
        for (const float sz : {-1.0F, 1.0F}) {
            pieces.push_back(Piece{box_shape(JPH::Vec3(inner_x, side, wall * 0.5F), p.density),
                                   JPH::Vec3(0.0F, mid, sz * (half.GetZ() - wall * 0.5F))});
        }
        break;
    }
    case RigidPropDesc::Shape::HollowCylinder: {
        const float floor = std::min(wall, height);
        pieces.push_back(Piece{cylinder_shape(floor * 0.5F, radius, p.density),
                               JPH::Vec3(0.0F, -height * 0.5F + floor * 0.5F, 0.0F)});
        const float side = std::max(height - floor, smallest_size);
        add_staves(pieces, radius, wall, side, segments, -height * 0.5F + floor + side * 0.5F, p.density);
        break;
    }
    case RigidPropDesc::Shape::Pipe:
        add_staves(pieces, radius, wall, height, segments, 0.0F, p.density);
        break;
    default:
        break;
    }
    return pieces;
}

// The shape a prop collides as, at size 1; `size` scales it afterwards.
std::optional<Error> prop_shape(const RigidPropDesc& p, JPH::RefConst<JPH::Shape>& out) {
    const std::uint32_t segments = std::clamp(p.segments, fewest_segments, most_segments);
    if (p.shape == RigidPropDesc::Shape::Mesh && p.motion != RigidPropDesc::Motion::Fixed) {
        // Validation refuses this before the solver sees it. A host driving
        // the seam itself gets the same answer rather than Jolt's: a mesh
        // shape reports no mass, and with assertions off that would become an
        // infinite inverse mass rather than a complaint.
        return Error{ErrorCode::RigidSolverFailure,
                     "collides as its own model and moves; a collision mesh can only stand still"};
    }
    switch (p.shape) {
    case RigidPropDesc::Shape::Mesh: {
        JPH::VertexList vertices;
        vertices.reserve(p.points.size());
        for (const Vec3& point : p.points) {
            vertices.push_back(JPH::Float3(point.x, point.y, point.z));
        }
        JPH::IndexedTriangleList triangles;
        triangles.reserve(p.indices.size() / 3U);
        for (std::size_t i = 0U; i + 2U < p.indices.size(); i += 3U) {
            triangles.push_back(JPH::IndexedTriangle(p.indices[i], p.indices[i + 1U], p.indices[i + 2U]));
        }
        JPH::MeshShapeSettings settings(vertices, triangles);
        const JPH::ShapeSettings::ShapeResult made = settings.Create();
        if (made.HasError()) {
            return Error{ErrorCode::RigidSolverFailure,
                         std::string{"its model makes no collision mesh: "} + made.GetError().c_str()};
        }
        out = made.Get();
        return std::nullopt;
    }
    case RigidPropDesc::Shape::Hull: {
        JPH::Array<JPH::Vec3> points;
        points.reserve(p.points.size());
        for (const Vec3& point : p.points) {
            points.push_back(vec(point));
        }
        JPH::ConvexHullShapeSettings settings(points);
        settings.SetDensity(p.density);
        const JPH::ShapeSettings::ShapeResult made = settings.Create();
        if (made.HasError()) {
            return Error{ErrorCode::RigidSolverFailure,
                         std::string{"its model makes no convex hull: "} + made.GetError().c_str()};
        }
        out = made.Get();
        return std::nullopt;
    }
    case RigidPropDesc::Shape::Sphere: {
        auto* sphere = new JPH::SphereShape(std::max(p.radius, smallest_size));
        sphere->SetDensity(p.density);
        out = sphere;
        return std::nullopt;
    }
    case RigidPropDesc::Shape::Box:
        out = box_shape(vec(p.half_size), p.density);
        return std::nullopt;
    case RigidPropDesc::Shape::Cylinder:
        out = cylinder_shape(p.height * 0.5F, p.radius, p.density);
        return std::nullopt;
    case RigidPropDesc::Shape::Cone: {
        // Jolt has no cone: the hull of an apex over a ring of base points is
        // one, to the roundness `segments` buys.
        const float radius = std::max(p.radius, smallest_size);
        const float half_height = std::max(p.height * 0.5F, smallest_size);
        JPH::Array<JPH::Vec3> points;
        points.push_back(JPH::Vec3(0.0F, half_height, 0.0F));
        for (std::uint32_t i = 0U; i < segments; ++i) {
            const float angle = 2.0F * pi * static_cast<float>(i) / static_cast<float>(segments);
            points.push_back(JPH::Vec3(std::sin(angle) * radius, -half_height, std::cos(angle) * radius));
        }
        JPH::ConvexHullShapeSettings settings(points);
        settings.SetDensity(p.density);
        const JPH::ShapeSettings::ShapeResult made = settings.Create();
        if (made.HasError()) {
            return Error{ErrorCode::RigidSolverFailure,
                         std::string{"its cone makes no shape: "} + made.GetError().c_str()};
        }
        out = made.Get();
        return std::nullopt;
    }
    case RigidPropDesc::Shape::HollowBox:
    case RigidPropDesc::Shape::HollowCylinder:
    case RigidPropDesc::Shape::Pipe: {
        const std::vector<Piece> pieces = prop_pieces(p);
        if (pieces.empty()) {
            return Error{ErrorCode::RigidSolverFailure, "its measurements make no shape"};
        }
        JPH::StaticCompoundShapeSettings settings;
        for (const Piece& piece : pieces) {
            settings.AddShape(piece.at, piece.turn, piece.shape.GetPtr());
        }
        const JPH::ShapeSettings::ShapeResult made = settings.Create();
        if (made.HasError()) {
            return Error{ErrorCode::RigidSolverFailure,
                         std::string{"its hollow shape could not be made: "} + made.GetError().c_str()};
        }
        out = made.Get();
        return std::nullopt;
    }
    }
    return Error{ErrorCode::RigidSolverFailure, "has a shape this solver does not know"};
}

// A world is also its own contact listener, where the desc asked for
// contacts. Jolt calls it from the job threads that find the collisions, so
// what they find is collected under a lock and ordered afterwards: the order
// contacts arrive in is the engine's and the thread count's, and neither may
// reach what a host does with them.
class JoltWorld final : public RigidWorld, private JPH::ContactListener {
public:
    JoltWorld(const RigidWorldDesc& desc, JPH::JobSystem& jobs)
        : desc_{desc},
          jobs_{jobs},
          temp_{4U * 1024U * 1024U + static_cast<JPH::uint>(desc.capacity) * 1024U} {}

    Result<bool> init() {
        const auto statics = static_cast<JPH::uint>(desc_.statics.size());
        const auto props = static_cast<JPH::uint>(desc_.props.size());
        const JPH::uint pairs = std::max<JPH::uint>(4096U, desc_.capacity * 8U);
        system_.Init(desc_.capacity + statics + props, 0U, pairs, pairs, broad_phase_, layer_vs_broad_, pairs_);
        if (desc_.report_contacts) {
            contacts_.reserve(desc_.contact_limit);
            system_.SetContactListener(this);
        }
        // A hull is wrapped once per kind that is one, since every body of a
        // kind scales the same hull.
        hulls_.resize(desc_.bodies.size());
        for (std::size_t k = 0U; k < desc_.bodies.size(); ++k) {
            const RigidWorldDesc::Body& body = desc_.bodies[k];
            if (body.shape != RigidWorldDesc::Shape::Hull) {
                continue;
            }
            JPH::Array<JPH::Vec3> points;
            for (const Vec3& point : body.hull_points) {
                points.push_back(vec(point));
            }
            JPH::ConvexHullShapeSettings settings(points);
            settings.SetDensity(body.density);
            JPH::ShapeSettings::ShapeResult hull = settings.Create();
            if (hull.HasError()) {
                return Error{ErrorCode::RigidSolverFailure, "body kind " + std::to_string(k) +
                                                                "'s mesh makes no convex hull: " +
                                                                hull.GetError().c_str()};
            }
            hulls_[k] = hull.Get();
        }
        JPH::BodyInterface& bodies = system_.GetBodyInterfaceNoLock();
        for (JPH::uint k = 0U; k < statics; ++k) {
            const RigidStatic& s = desc_.statics[k];
            JPH::RefConst<JPH::Shape> shape;
            JPH::RVec3 at = JPH::RVec3::sZero();
            switch (s.kind) {
            case RigidStatic::Kind::Plane:
                shape = new JPH::PlaneShape(JPH::Plane::sFromPointAndNormal(vec(s.center), vec(s.normal)), nullptr,
                                            plane_extent);
                break;
            case RigidStatic::Kind::Sphere:
                shape = new JPH::SphereShape(s.radius);
                at = JPH::RVec3(s.center.x, s.center.y, s.center.z);
                break;
            case RigidStatic::Kind::Box:
                shape = new JPH::BoxShape(vec(s.half_size));
                at = JPH::RVec3(s.center.x, s.center.y, s.center.z);
                break;
            }
            JPH::BodyCreationSettings settings(shape, at, JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                               layers::still);
            settings.mFriction = s.friction;
            settings.mRestitution = s.restitution;
            JPH::Body* body = bodies.CreateBodyWithID(JPH::BodyID(desc_.capacity + k), settings);
            if (body == nullptr) {
                return Error{ErrorCode::RigidSolverFailure, "a collider could not be made"};
            }
            bodies.AddBody(body->GetID(), JPH::EActivation::DontActivate);
        }
        // Then the props, in order, so their ids are a pure function of the
        // desc and every node builds the same world.
        for (std::size_t k = 0U; k < desc_.props.size(); ++k) {
            const RigidPropDesc& p = desc_.props[k];
            const std::string where = "prop " + std::to_string(k) + " ";
            JPH::RefConst<JPH::Shape> shape;
            if (const std::optional<Error> failed = prop_shape(p, shape)) {
                return Error{failed->code, where + failed->message};
            }
            const float size = std::max(p.size, smallest_size);
            if (size != 1.0F) {
                shape = new JPH::ScaledShape(shape, JPH::Vec3::sReplicate(size));
            }
            const bool fixed = p.motion == RigidPropDesc::Motion::Fixed;
            JPH::EMotionType motion = JPH::EMotionType::Static;
            if (p.motion == RigidPropDesc::Motion::Driven) {
                motion = JPH::EMotionType::Kinematic;
            } else if (p.motion == RigidPropDesc::Motion::Loose) {
                motion = JPH::EMotionType::Dynamic;
            }
            const Quat q = p.orientation;
            JPH::BodyCreationSettings settings(shape, JPH::RVec3(p.position.x, p.position.y, p.position.z),
                                               JPH::Quat(q.x, q.y, q.z, q.w).Normalized(), motion,
                                               fixed ? layers::still : layers::moving);
            settings.mFriction = p.friction;
            settings.mRestitution = p.restitution;
            settings.mLinearDamping = desc_.linear_damping;
            settings.mAngularDamping = desc_.angular_damping;
            // A driven prop must not sleep, or it would stop following its
            // move; a loose one rests as the emitter's bodies do.
            settings.mAllowSleeping = p.motion == RigidPropDesc::Motion::Loose && desc_.allow_sleep;
            JPH::Body* body = bodies.CreateBodyWithID(JPH::BodyID(prop_id(k)), settings);
            if (body == nullptr) {
                return Error{ErrorCode::RigidSolverFailure, where + "could not be made"};
            }
            bodies.AddBody(body->GetID(), fixed ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        }
        return true;
    }

    void add(std::uint32_t slot, std::uint32_t kind, float size, const RigidBodyState& state,
             bool asleep) override {
        const std::size_t k = std::min<std::size_t>(kind, desc_.bodies.size() - 1U);
        const RigidWorldDesc::Body& of_kind = desc_.bodies[k];
        const float s = std::max(size, smallest_size);
        JPH::RefConst<JPH::Shape> shape;
        switch (of_kind.shape) {
        case RigidWorldDesc::Shape::Hull:
            shape = new JPH::ScaledShape(hulls_[k], JPH::Vec3::sReplicate(s));
            break;
        case RigidWorldDesc::Shape::Sphere: {
            auto* sphere = new JPH::SphereShape(0.5F * s);
            sphere->SetDensity(of_kind.density);
            shape = sphere;
            break;
        }
        case RigidWorldDesc::Shape::Box: {
            auto* box = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5F * s), std::min(JPH::cDefaultConvexRadius, 0.25F * s));
            box->SetDensity(of_kind.density);
            shape = box;
            break;
        }
        }
        const Quat q = state.orientation;
        JPH::BodyCreationSettings settings(shape, JPH::RVec3(state.position.x, state.position.y, state.position.z),
                                           JPH::Quat(q.x, q.y, q.z, q.w).Normalized(), JPH::EMotionType::Dynamic,
                                           layers::moving);
        settings.mLinearVelocity = vec(state.velocity);
        settings.mAngularVelocity = vec(state.angular_velocity);
        settings.mFriction = of_kind.friction;
        settings.mRestitution = of_kind.restitution;
        settings.mLinearDamping = desc_.linear_damping;
        settings.mAngularDamping = desc_.angular_damping;
        settings.mAllowSleeping = desc_.allow_sleep;
        JPH::BodyInterface& bodies = system_.GetBodyInterfaceNoLock();
        if (JPH::Body* body = bodies.CreateBodyWithID(JPH::BodyID(slot), settings)) {
            bodies.AddBody(body->GetID(), asleep ? JPH::EActivation::DontActivate : JPH::EActivation::Activate);
        }
    }

    void wake(std::span<const std::uint32_t> slots) override {
        std::vector<JPH::BodyID> ids;
        ids.reserve(slots.size());
        for (const std::uint32_t slot : slots) {
            ids.emplace_back(slot);
        }
        if (!ids.empty()) {
            system_.GetBodyInterfaceNoLock().ActivateBodies(ids.data(), static_cast<int>(ids.size()));
        }
    }

    // Jolt leaves a sleeping body asleep when what it rests on goes, so it
    // would hang in the air: everything touching the removed body wakes.
    // The broad phase's bounds depend on its history, which a restored world
    // does not share, so its candidates are kept by their exact bounds and
    // woken in body order, as Jolt's determinism notes ask.
    void remove(std::uint32_t slot) override {
        JPH::BodyInterface& bodies = system_.GetBodyInterfaceNoLock();
        const JPH::BodyID id(slot);
        JPH::AABox reach = bodies.GetTransformedShape(id).GetWorldSpaceBounds();
        reach.ExpandBy(JPH::Vec3::sReplicate(touch_margin));
        JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> near;
        system_.GetBroadPhaseQuery().CollideAABox(reach, near);
        std::vector<JPH::BodyID> touching;
        for (const JPH::BodyID other : near.mHits) {
            if (other != id && bodies.GetObjectLayer(other) == layers::moving &&
                bodies.GetTransformedShape(other).GetWorldSpaceBounds().Overlaps(reach)) {
                touching.push_back(other);
            }
        }
        std::sort(touching.begin(), touching.end());
        bodies.RemoveBody(id);
        bodies.DestroyBody(id);
        if (!touching.empty()) {
            bodies.ActivateBodies(touching.data(), static_cast<int>(touching.size()));
        }
    }

    // Jolt fixes the two where they stand, so a join holds the pose the
    // bodies were put in rather than pulling them to a point.
    void glue(const RigidJoin& join, float strength) override {
        // A world is stepped from one thread at a time, so the bodies are
        // taken without a lock, as everything else here takes them.
        const JPH::BodyLockInterfaceNoLock& bodies = system_.GetBodyLockInterfaceNoLock();
        JPH::Body* first = bodies.TryGetBody(JPH::BodyID(join.a));
        JPH::Body* second = bodies.TryGetBody(JPH::BodyID(join.b));
        if (first == nullptr || second == nullptr) {
            return;
        }
        // Jolt bakes a joint's rest from the world poses the bodies are in
        // when it is made, and offers no setting for it. A world rebuilt
        // from a checkpoint has them somewhere else by then, and baking from
        // there gives numbers a rounding apart, which is a seek that lands
        // beside the step it should land on.
        //
        // So both bodies are put somewhere the join alone decides -- the
        // first at the origin, unturned, the second at the rest the join
        // carries -- the constraint is made there, and both are put back.
        // What Jolt bakes is then a function of the join and nothing else,
        // the same on the step it was made and on every rebuild after.
        JPH::BodyInterface& moving = system_.GetBodyInterfaceNoLock();
        const JPH::RVec3 a_was_at = first->GetPosition();
        const JPH::Quat a_was_turned = first->GetRotation();
        const JPH::RVec3 b_was_at = second->GetPosition();
        const JPH::Quat b_was_turned = second->GetRotation();
        moving.SetPositionAndRotation(JPH::BodyID(join.a), JPH::RVec3::sZero(), JPH::Quat::sIdentity(),
                                      JPH::EActivation::DontActivate);
        moving.SetPositionAndRotation(JPH::BodyID(join.b), JPH::RVec3(join.at.x, join.at.y, join.at.z),
                                      JPH::Quat(join.turn.x, join.turn.y, join.turn.z, join.turn.w).Normalized(),
                                      JPH::EActivation::DontActivate);
        JPH::FixedConstraintSettings settings;
        settings.mAutoDetectPoint = true;
        JPH::Constraint* made = settings.Create(*first, *second);
        moving.SetPositionAndRotation(JPH::BodyID(join.a), a_was_at, a_was_turned, JPH::EActivation::DontActivate);
        moving.SetPositionAndRotation(JPH::BodyID(join.b), b_was_at, b_was_turned, JPH::EActivation::DontActivate);
        if (made == nullptr) {
            return;
        }
        made->AddRef();
        system_.AddConstraint(made);
        joins_.push_back(Join{join, strength, made});
        held_.push_back(join);
    }

    // Jolt moves its last constraint into the gap when one goes, and reads a
    // checkpoint back by that numbering, so the same move is made here: the
    // two lists stay in step and `joins` gives the order a rebuild needs.
    void drop(std::size_t at) {
        system_.RemoveConstraint(joins_[at].constraint);
        joins_[at].constraint->Release();
        joins_[at] = joins_.back();
        held_[at] = held_.back();
        joins_.pop_back();
        held_.pop_back();
    }

    void unglue(const RigidJoin& join) override {
        for (std::size_t i = 0U; i < joins_.size(); ++i) {
            if (joins_[i].join == join) {
                drop(i);
                return;
            }
        }
    }

    std::span<const RigidJoin> joins() const override { return held_; }

    // A driven prop is told where to be, and Jolt gives it the velocity that
    // takes it there over the step, so it shoves what is in its way instead
    // of passing through it.
    void set_prop_pose(std::size_t index, Vec3 position, Quat orientation) override {
        if (index >= desc_.props.size() || desc_.props[index].motion != RigidPropDesc::Motion::Driven) {
            return;
        }
        system_.GetBodyInterfaceNoLock().MoveKinematic(
            JPH::BodyID(prop_id(index)), JPH::RVec3(position.x, position.y, position.z),
            JPH::Quat(orientation.x, orientation.y, orientation.z, orientation.w).Normalized(), desc_.step_seconds);
    }

    void step(Vec3 gravity) override {
        contacts_.clear();
        dropped_ = 0U;
        system_.SetGravity(vec(gravity));
        system_.Update(desc_.step_seconds, 1, &temp_, &jobs_);
        // What the step had to pull through each join to hold it. Walked in
        // the order the joins were made, so which of two joins at the same
        // force goes first is the order they were made in and nothing else.
        // Walked from the end, so dropping one never moves a join not yet
        // looked at: what breaks is decided by the force through it and by
        // nothing about the order it is walked in.
        for (std::size_t i = joins_.size(); i-- > 0U;) {
            const JPH::Vec3 pull = joins_[i].fixed()->GetTotalLambdaPosition();
            const JPH::Vec3 twist = joins_[i].fixed()->GetTotalLambdaRotation();
            // The twist is scaled by a body's reach to compare with the
            // pull: a join an arm's length out takes that much less to turn
            // than to drag.
            const float force = std::sqrt(pull.Dot(pull)) + std::sqrt(twist.Dot(twist)) / turn_arm;
            if (force > joins_[i].strength) {
                drop(i);
            }
        }
        std::sort(contacts_.begin(), contacts_.end(), contact_before);
        if (contacts_.size() > desc_.contact_limit) {
            dropped_ = static_cast<std::uint32_t>(contacts_.size() - desc_.contact_limit);
            contacts_.resize(desc_.contact_limit);
        }
    }

    RigidBodyState body(std::uint32_t slot) const override { return state_of(JPH::BodyID(slot)); }

    RigidBodyState prop(std::size_t index) const override {
        if (index >= desc_.props.size()) {
            return RigidBodyState{};
        }
        return state_of(JPH::BodyID(prop_id(index)));
    }

    std::span<const RigidContact> contacts() const override { return contacts_; }

    std::uint32_t contacts_dropped() const override { return dropped_; }

    std::vector<std::uint8_t> save() const override {
        JPH::StateRecorderImpl recorder;
        system_.SaveState(recorder);
        const std::string data = recorder.GetData();
        return std::vector<std::uint8_t>(data.begin(), data.end());
    }

    bool restore(std::span<const std::uint8_t> bytes) override {
        JPH::StateRecorderImpl recorder;
        recorder.WriteBytes(bytes.data(), bytes.size());
        return system_.RestoreState(recorder) && !recorder.IsFailed();
    }

private:
    void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                        JPH::ContactSettings&) override {
        found(body1, body2, manifold, true);
    }

    void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
                            JPH::ContactSettings&) override {
        found(body1, body2, manifold, false);
    }

    // One manifold, as collision detection found it and before the step
    // resolved it, which is the only moment the speed the pair arrived at is
    // known. Jolt also finds a fast pair on its way in, a step before it
    // strictly touches, so that it cannot pass through: that is the contact
    // carrying the arrival speed, and it is reported as any other.
    void found(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, bool began) {
        const auto points = static_cast<std::uint32_t>(manifold.mRelativeContactPointsOn1.size());
        if (points == 0U) {
            return;
        }
        // The middle of the manifold, so a face resting on a face gives one
        // point between them rather than a corner of itself.
        JPH::RVec3 sum = JPH::RVec3::sZero();
        for (std::uint32_t i = 0U; i < points; ++i) {
            sum += manifold.GetWorldSpaceContactPointOn1(i);
        }
        const JPH::RVec3 point = sum / static_cast<float>(points);
        // Jolt's normal is the way body 2 leaves the collision, so it points
        // from body 1 towards body 2; it is turned round below with the
        // sides, if they are.
        const JPH::Vec3 normal = manifold.mWorldSpaceNormal;
        const JPH::Vec3 closing = body2.GetPointVelocity(point) - body1.GetPointVelocity(point);
        const float speed = std::max(-closing.Dot(normal), 0.0F);
        if (speed < desc_.contact_min_speed) {
            return;
        }
        auto [a_is, a] = side_of(body1.GetID());
        auto [b_is, b] = side_of(body2.GetID());
        RigidContact contact{.a_is = a_is,
                             .a = a,
                             .b_is = b_is,
                             .b = b,
                             .point = vec_of(point),
                             .normal = vec(normal),
                             .speed = speed,
                             .began = began};
        if (std::tie(b_is, b) < std::tie(a_is, a)) {
            contact.a_is = b_is;
            contact.a = b;
            contact.b_is = a_is;
            contact.b = a;
            contact.normal = vec(-normal);
        }
        const std::lock_guard<std::mutex> held{collecting_};
        contacts_.push_back(contact);
    }

    // Which of the world's three kinds of body an id is, and its index among
    // them. Particles hold the slots below capacity, then the statics, then
    // the props, as prop_id lays them out.
    [[nodiscard]] std::pair<RigidSide, std::uint32_t> side_of(JPH::BodyID id) const {
        const std::uint32_t index = id.GetIndex();
        if (index < desc_.capacity) {
            return {RigidSide::Body, index};
        }
        const auto statics = static_cast<std::uint32_t>(desc_.statics.size());
        if (index < desc_.capacity + statics) {
            return {RigidSide::Static, index - desc_.capacity};
        }
        return {RigidSide::Prop, index - desc_.capacity - statics};
    }

    // Particles hold the slots below capacity, then the statics, then the
    // props in the order the desc lists them.
    [[nodiscard]] std::uint32_t prop_id(std::size_t index) const {
        return desc_.capacity + static_cast<std::uint32_t>(desc_.statics.size()) +
               static_cast<std::uint32_t>(index);
    }

    [[nodiscard]] RigidBodyState state_of(JPH::BodyID id) const {
        const JPH::BodyInterface& bodies = system_.GetBodyInterfaceNoLock();
        JPH::RVec3 p;
        JPH::Quat q;
        bodies.GetPositionAndRotation(id, p, q);
        return RigidBodyState{Vec3{static_cast<float>(p.GetX()), static_cast<float>(p.GetY()),
                                   static_cast<float>(p.GetZ())},
                              Quat{q.GetX(), q.GetY(), q.GetZ(), q.GetW()}, vec(bodies.GetLinearVelocity(id)),
                              vec(bodies.GetAngularVelocity(id))};
    }

    RigidWorldDesc desc_;
    JPH::JobSystem& jobs_;
    JPH::TempAllocatorImplWithMallocFallback temp_;
    BroadPhaseLayers broad_phase_;
    LayerVsBroadPhase layer_vs_broad_;
    PairFilter pairs_;
    JPH::PhysicsSystem system_;
    // One wrapped hull per body kind; empty for a kind that is not one.
    std::vector<JPH::RefConst<JPH::Shape>> hulls_;
    // One join the world holds: which bodies, what it can take, and Jolt's
    // constraint. Held in the order they were made.
    struct Join final {
        RigidJoin join;
        float strength{0.0F};
        JPH::Constraint* constraint{nullptr};
        [[nodiscard]] JPH::FixedConstraint* fixed() const {
            return static_cast<JPH::FixedConstraint*>(constraint);
        }
    };
    std::vector<Join> joins_;
    // The same joins, in the same order, as `joins` reports them.
    std::vector<RigidJoin> held_;
    // Contact callbacks arrive on the job threads that find the collisions.
    std::mutex collecting_;
    std::vector<RigidContact> contacts_;
    std::uint32_t dropped_{0U};

public:
    ~JoltWorld() override {
        for (const Join& join : joins_) {
            system_.RemoveConstraint(join.constraint);
            join.constraint->Release();
        }
    }
};

class JoltSolver final : public RigidSolver {
public:
    explicit JoltSolver(JoltOptions options)
        : jobs_{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                static_cast<int>(std::max<std::uint32_t>(options.threads, 1U) - 1U)} {}

    Result<std::unique_ptr<RigidWorld>> create_world(const RigidWorldDesc& desc) const override {
        auto world = std::make_unique<JoltWorld>(desc, jobs_);
        if (Result<bool> made = world->init(); !made) {
            return made.error();
        }
        return std::unique_ptr<RigidWorld>{std::move(world)};
    }

private:
    mutable JPH::JobSystemThreadPool jobs_;
};

} // namespace

std::unique_ptr<RigidSolver> make_jolt_solver(JoltOptions options) {
    register_jolt();
    return std::make_unique<JoltSolver>(options);
}

} // namespace ajfx::rigid
