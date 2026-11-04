#pragma once

#include <ajfx/effect.hpp>
#include <ajfx/mesh.hpp>
#include <ajfx/result.hpp>
#include <ajfx/rigid_solver.hpp>
#include <ajfx/simulation.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

// Rigid physics off the frame path. A host that records a frame at a time
// (the media server) cannot run a seek's worth of engine steps, or save the
// engine's state for a checkpoint, while it records. A RigidRunner steps the
// rigid physics emitters of every track on its own thread instead: ahead of
// the latest tick asked of it, from its own checkpoints when a seek goes
// back. The frame path only picks up the particles of a finished tick, or
// learns that the tick is not ready and holds.
//
// The runner computes exactly what Simulation does: the same bits, whatever
// order ticks are asked in and however often the runner turns between tracks.
namespace ajfx {

struct RigidRunnerOptions final {
    // Steps a track runs ahead of the latest tick asked of it, so playback
    // finds each tick ready. Each step held costs one copy of the particles.
    std::uint32_t lead{30U};
    // A checkpoint every this many steps; the latest `checkpoint_count` are
    // kept, so a seek back resimulates at most about one interval.
    std::uint64_t checkpoint_interval{120U};
    std::size_t checkpoint_count{8U};
    // Steps one track takes before the runner turns to the next.
    std::uint32_t turn{8U};
    // Contacts a host wants reported beside what the effect reads of them
    // (SimulationOptions::contacts); none asks for only those.
    std::optional<ContactOptions> contacts;
};

// A track's rigid physics emitters at one tick, as Simulation holds them
// (without solver state), indexed as the effect's emitters. The others are
// empty.
// - `contacts` are each emitter's contacts from the step that reached this
//   tick, as Simulation::contacts gives them: what the effect reads of them
//   (Break, SpawnFromContacts) and whatever the runner's options asked for
//   besides. Empty at tick 0. A frame at a tick the runner reached by a seek
//   carries them too: the runner always steps into a tick it hands out,
//   never restores straight onto it.
// - GpuSimulation::step takes a frame so its particle emitters meet these
//   bodies and are born of these contacts.
struct RigidFrame final {
    std::uint64_t tick{0U};
    std::vector<EmitterState> emitters;
    std::vector<std::vector<RigidContact>> contacts;
    std::vector<std::uint32_t> contacts_dropped;
};

class RigidTrack;

class RigidRunner final {
public:
    // Starts the runner's thread. `solver` must outlive the runner, and only
    // the runner may step worlds it makes. The solver's own threads (see
    // ajfx::rigid::JoltOptions) are its thread budget beyond this one.
    explicit RigidRunner(const RigidSolver& solver, RigidRunnerOptions options = {});
    RigidRunner(const RigidRunner&) = delete;
    RigidRunner& operator=(const RigidRunner&) = delete;
    // Stops and joins the thread. Tracks may outlive it; they get no frames.
    ~RigidRunner();

    // A track of `effect`'s rigid physics emitters with `instance_seed`, the
    // hull meshes in `meshes`, and `parameters` (none for the defaults),
    // which must be a pure function of the tick and callable from the
    // runner's thread. Its worlds are built on that thread when a tick is
    // first asked for; a failure there is reported by RigidTrack::error.
    [[nodiscard]] std::unique_ptr<RigidTrack> track(const Effect& effect, std::uint32_t instance_seed, MeshSet meshes,
                                                    std::shared_ptr<const ParameterProvider> parameters = nullptr);

    // Blocks until every track has the tick it last asked for and its lead
    // (or has failed). For tests and tools.
    void wait_idle();

    struct Shared;
    struct Job;

private:
    std::shared_ptr<Shared> shared_;
};

// One effect instance's rigid physics on a runner. Its calls take a lock
// briefly and never wait for a step. Destroying it lets the runner drop the
// work.
class RigidTrack final {
public:
    RigidTrack(const RigidTrack&) = delete;
    RigidTrack& operator=(const RigidTrack&) = delete;
    ~RigidTrack();

    // The particles at `tick` if the runner has them. Otherwise none, and
    // the runner works towards `tick`, from a checkpoint when it is behind
    // where the runner stands. Asking is also where the runner looks ahead
    // from: ticks before it are let go.
    [[nodiscard]] std::shared_ptr<const RigidFrame> frame(std::uint64_t tick);
    // Follow `parameters` from now on. What the runner holds describes the
    // old parameters' past, so it is dropped and worked out again, unless the
    // track's emitters read no parameter: then their past is the same.
    void set_parameters(std::shared_ptr<const ParameterProvider> parameters);
    // Why the runner could not build this track's worlds, once it has tried.
    [[nodiscard]] std::optional<Error> error() const;

private:
    friend class RigidRunner;
    RigidTrack(std::shared_ptr<RigidRunner::Shared> shared, std::shared_ptr<RigidRunner::Job> job);

    std::shared_ptr<RigidRunner::Shared> shared_;
    std::shared_ptr<RigidRunner::Job> job_;
};

} // namespace ajfx
