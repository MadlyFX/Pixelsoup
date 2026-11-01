#include <ajfx/rigid.hpp>

// Jolt must come first; every other Jolt header assumes it.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/PlaneShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <utility>

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

// A body smaller than this is made this size: a shape of no size is not one.
constexpr float smallest_size = 1.0e-3F;
// How far out a plane collider reaches from its centre.
constexpr float plane_extent = 1000.0F;

class JoltWorld final : public RigidWorld {
public:
    JoltWorld(const RigidWorldDesc& desc, JPH::JobSystem& jobs)
        : desc_{desc},
          jobs_{jobs},
          temp_{4U * 1024U * 1024U + static_cast<JPH::uint>(desc.capacity) * 1024U} {}

    Result<bool> init() {
        const auto statics = static_cast<JPH::uint>(desc_.statics.size());
        const JPH::uint pairs = std::max<JPH::uint>(4096U, desc_.capacity * 8U);
        system_.Init(desc_.capacity + statics, 0U, pairs, pairs, broad_phase_, layer_vs_broad_, pairs_);
        if (desc_.shape == RigidWorldDesc::Shape::Hull) {
            JPH::Array<JPH::Vec3> points;
            for (const Vec3& point : desc_.hull_points) {
                points.push_back(vec(point));
            }
            JPH::ConvexHullShapeSettings settings(points);
            settings.SetDensity(desc_.density);
            JPH::ShapeSettings::ShapeResult hull = settings.Create();
            if (hull.HasError()) {
                return Error{ErrorCode::RigidSolverFailure,
                             std::string{"its mesh makes no convex hull: "} + hull.GetError().c_str()};
            }
            hull_ = hull.Get();
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
        return true;
    }

    void add(std::uint32_t slot, float size, const RigidBodyState& state) override {
        const float s = std::max(size, smallest_size);
        JPH::RefConst<JPH::Shape> shape;
        switch (desc_.shape) {
        case RigidWorldDesc::Shape::Hull:
            shape = new JPH::ScaledShape(hull_, JPH::Vec3::sReplicate(s));
            break;
        case RigidWorldDesc::Shape::Sphere: {
            auto* sphere = new JPH::SphereShape(0.5F * s);
            sphere->SetDensity(desc_.density);
            shape = sphere;
            break;
        }
        case RigidWorldDesc::Shape::Box: {
            auto* box = new JPH::BoxShape(JPH::Vec3::sReplicate(0.5F * s), std::min(JPH::cDefaultConvexRadius, 0.25F * s));
            box->SetDensity(desc_.density);
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
        settings.mFriction = desc_.friction;
        settings.mRestitution = desc_.restitution;
        settings.mLinearDamping = desc_.linear_damping;
        settings.mAngularDamping = desc_.angular_damping;
        settings.mAllowSleeping = desc_.allow_sleep;
        JPH::BodyInterface& bodies = system_.GetBodyInterfaceNoLock();
        if (JPH::Body* body = bodies.CreateBodyWithID(JPH::BodyID(slot), settings)) {
            bodies.AddBody(body->GetID(), JPH::EActivation::Activate);
        }
    }

    void remove(std::uint32_t slot) override {
        JPH::BodyInterface& bodies = system_.GetBodyInterfaceNoLock();
        const JPH::BodyID id(slot);
        bodies.RemoveBody(id);
        bodies.DestroyBody(id);
    }

    void step(Vec3 gravity) override {
        system_.SetGravity(vec(gravity));
        system_.Update(desc_.step_seconds, 1, &temp_, &jobs_);
    }

    RigidBodyState body(std::uint32_t slot) const override {
        const JPH::BodyInterface& bodies = system_.GetBodyInterfaceNoLock();
        const JPH::BodyID id(slot);
        JPH::RVec3 p;
        JPH::Quat q;
        bodies.GetPositionAndRotation(id, p, q);
        return RigidBodyState{Vec3{static_cast<float>(p.GetX()), static_cast<float>(p.GetY()),
                                   static_cast<float>(p.GetZ())},
                              Quat{q.GetX(), q.GetY(), q.GetZ(), q.GetW()}, vec(bodies.GetLinearVelocity(id)),
                              vec(bodies.GetAngularVelocity(id))};
    }

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
    RigidWorldDesc desc_;
    JPH::JobSystem& jobs_;
    JPH::TempAllocatorImplWithMallocFallback temp_;
    BroadPhaseLayers broad_phase_;
    LayerVsBroadPhase layer_vs_broad_;
    PairFilter pairs_;
    JPH::PhysicsSystem system_;
    JPH::RefConst<JPH::Shape> hull_;
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
