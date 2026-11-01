#include <ajfx/rigid_runner.hpp>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace ajfx {

struct RigidRunner::Job final {
    // Fixed when the track is made; after that only the runner's thread
    // reads them.
    Effect part;
    // Each of the part's emitters' index in the whole effect.
    std::vector<std::size_t> emitter_of;
    std::size_t emitters{0U};
    std::uint32_t seed{0U};
    MeshSet meshes;

    // Under Shared::mutex.
    std::uint64_t wanted{0U};
    bool asked{false};
    std::uint64_t generation{0U};
    std::shared_ptr<const ParameterProvider> parameters;
    // Ascending ticks, all from `generation`, none before `wanted`.
    std::deque<std::shared_ptr<const RigidFrame>> frames;
    std::optional<Error> error;
    bool dropped{false};
    bool busy{false};

    // Only the runner's thread.
    std::optional<Simulation> simulation;
    std::optional<std::uint64_t> built; // the generation `simulation` follows
    std::vector<SimulationState> checkpoints;
};

struct RigidRunner::Shared final {
    const RigidSolver* solver{nullptr};
    RigidRunnerOptions options;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::vector<std::shared_ptr<Job>> jobs;
    std::size_t next{0U};
    bool stopping{false};
    std::thread thread;

    // The first tick from `wanted` on that the job holds no frame for: past
    // the run of frames starting at `wanted`, or `wanted` itself.
    [[nodiscard]] static std::uint64_t first_missing(const Job& job) {
        std::uint64_t tick = job.wanted;
        for (const std::shared_ptr<const RigidFrame>& frame : job.frames) {
            if (frame->tick == tick) {
                ++tick;
            } else if (frame->tick > tick) {
                break;
            }
        }
        return tick;
    }

    [[nodiscard]] bool wants_work(const Job& job) const {
        return job.asked && !job.dropped && !job.error && first_missing(job) <= job.wanted + options.lead;
    }

    // The frame path is waiting on it.
    [[nodiscard]] static bool urgent(const Job& job) { return first_missing(job) == job.wanted; }

    [[nodiscard]] bool settled() const {
        return std::none_of(jobs.begin(), jobs.end(),
                            [this](const std::shared_ptr<Job>& job) { return job->busy || wants_work(*job); });
    }

    void run();
    [[nodiscard]] std::optional<Error> turn(Job& job, std::uint64_t from, std::uint64_t wanted,
                                            std::uint64_t generation, const ParameterProvider* parameters,
                                            std::vector<std::shared_ptr<const RigidFrame>>& produced) const;
};

namespace {

// A particle state without the engine's own bytes, which the frame path
// never needs.
EmitterState particles_of(const EmitterState& s) {
    EmitterState out;
    out.position = s.position;
    out.velocity = s.velocity;
    out.age = s.age;
    out.lifetime = s.lifetime;
    out.base_color = s.base_color;
    out.color = s.color;
    out.base_size = s.base_size;
    out.size = s.size;
    out.rotation = s.rotation;
    out.orientation = s.orientation;
    out.angular_velocity = s.angular_velocity;
    out.spin = s.spin;
    out.spawn_index = s.spawn_index;
    out.body = s.body;
    // The props' poses: a host draws them, so they travel with the particles.
    out.props = s.props;
    out.spawned = s.spawned;
    out.refused = s.refused;
    out.rate_carry = s.rate_carry;
    return out;
}

} // namespace

std::optional<Error> RigidRunner::Shared::turn(Job& job, std::uint64_t from, std::uint64_t wanted,
                                               std::uint64_t generation, const ParameterProvider* parameters,
                                               std::vector<std::shared_ptr<const RigidFrame>>& produced) const {
    if (!job.simulation) {
        Result<Simulation> made =
            Simulation::create(job.part, job.seed, SimulationOptions{.meshes = job.meshes, .rigid = solver});
        if (!made) {
            return made.error();
        }
        job.simulation.emplace(std::move(made).value());
    }
    Simulation& simulation = *job.simulation;
    // New parameters (or none followed yet): what is held describes another
    // past.
    if (job.built != generation) {
        job.checkpoints.clear();
        simulation.reset(parameters);
        job.built = generation;
    }

    // Behind where it stands: back to the latest checkpoint at or before the
    // first missing tick. Ahead: on from a checkpoint already past it.
    const SimulationState* best = nullptr;
    for (const SimulationState& checkpoint : job.checkpoints) {
        if (checkpoint.tick <= from && (best == nullptr || checkpoint.tick > best->tick)) {
            best = &checkpoint;
        }
    }
    if (simulation.tick() > from) {
        if (best != nullptr) {
            simulation.restore(*best);
        } else {
            simulation.reset(parameters);
        }
    } else if (best != nullptr && best->tick > simulation.tick()) {
        simulation.restore(*best);
    }

    const std::uint64_t horizon = wanted + options.lead;
    const std::uint64_t interval = std::max<std::uint64_t>(options.checkpoint_interval, 1U);
    const std::size_t keep = std::max<std::size_t>(options.checkpoint_count, 1U);
    for (std::uint32_t steps = 0U;; ++steps) {
        const std::uint64_t tick = simulation.tick();
        if (tick % interval == 0U && std::none_of(job.checkpoints.begin(), job.checkpoints.end(),
                                                  [tick](const SimulationState& c) { return c.tick == tick; })) {
            if (job.checkpoints.size() == keep) {
                job.checkpoints.erase(job.checkpoints.begin());
            }
            job.checkpoints.push_back(simulation.state());
        }
        if (tick >= from) {
            auto frame = std::make_shared<RigidFrame>();
            frame->tick = tick;
            frame->emitters.resize(job.emitters);
            for (std::size_t k = 0U; k < job.emitter_of.size(); ++k) {
                frame->emitters[job.emitter_of[k]] = particles_of(simulation.emitter(k));
            }
            produced.push_back(std::move(frame));
        }
        if (tick >= horizon || steps == std::max(options.turn, 1U)) {
            break;
        }
        simulation.step(parameters);
    }
    return std::nullopt;
}

void RigidRunner::Shared::run() {
    std::unique_lock lock{mutex};
    while (true) {
        // A dropped track's worlds are freed outside the lock, so the frame
        // path never waits on them.
        std::vector<std::shared_ptr<Job>> dropped;
        for (std::shared_ptr<Job>& job : jobs) {
            if (job->dropped) {
                dropped.push_back(std::move(job));
            }
        }
        if (!dropped.empty()) {
            std::erase(jobs, nullptr);
            lock.unlock();
            dropped.clear();
            lock.lock();
            continue;
        }
        if (stopping) {
            return;
        }
        // A track the frame path waits on first, then any short of its lead,
        // each in turn.
        std::shared_ptr<Job> pick;
        for (const bool urgent_only : {true, false}) {
            for (std::size_t k = 0U; k < jobs.size() && !pick; ++k) {
                const std::size_t at = (next + k) % jobs.size();
                if (wants_work(*jobs[at]) && (!urgent_only || urgent(*jobs[at]))) {
                    pick = jobs[at];
                    next = (at + 1U) % jobs.size();
                }
            }
        }
        if (!pick) {
            idle.notify_all();
            wake.wait(lock);
            continue;
        }

        pick->busy = true;
        const std::uint64_t wanted = pick->wanted;
        const std::uint64_t from = first_missing(*pick);
        const std::uint64_t generation = pick->generation;
        const std::shared_ptr<const ParameterProvider> parameters = pick->parameters;
        lock.unlock();

        std::vector<std::shared_ptr<const RigidFrame>> produced;
        std::optional<Error> failed = turn(*pick, from, wanted, generation, parameters.get(), produced);

        lock.lock();
        pick->busy = false;
        if (failed) {
            pick->error = std::move(failed);
        }
        if (pick->generation == generation) {
            for (std::shared_ptr<const RigidFrame>& frame : produced) {
                if (frame->tick < pick->wanted) {
                    continue;
                }
                const auto at = std::lower_bound(pick->frames.begin(), pick->frames.end(), frame->tick,
                                                 [](const std::shared_ptr<const RigidFrame>& held, std::uint64_t tick) {
                                                     return held->tick < tick;
                                                 });
                if (at == pick->frames.end() || (*at)->tick != frame->tick) {
                    pick->frames.insert(at, std::move(frame));
                }
            }
        }
    }
}

RigidRunner::RigidRunner(const RigidSolver& solver, RigidRunnerOptions options) : shared_{std::make_shared<Shared>()} {
    shared_->solver = &solver;
    shared_->options = options;
    shared_->thread = std::thread{[shared = shared_.get()] { shared->run(); }};
}

RigidRunner::~RigidRunner() {
    {
        const std::lock_guard lock{shared_->mutex};
        shared_->stopping = true;
    }
    shared_->wake.notify_all();
    shared_->thread.join();
    // The worlds go now, while their solver surely lives; a track kept
    // longer only ever answers "not ready".
    const std::lock_guard lock{shared_->mutex};
    for (const std::shared_ptr<Job>& job : shared_->jobs) {
        job->simulation.reset();
        job->checkpoints.clear();
        job->frames.clear();
    }
    shared_->jobs.clear();
}

std::unique_ptr<RigidTrack> RigidRunner::track(const Effect& effect, std::uint32_t instance_seed, MeshSet meshes,
                                               std::shared_ptr<const ParameterProvider> parameters) {
    auto job = std::make_shared<Job>();
    job->part = rigid_physics_part(effect);
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        if (rigid_physics(effect.emitters[i]) != nullptr) {
            job->emitter_of.push_back(i);
        }
    }
    job->emitters = effect.emitters.size();
    job->seed = instance_seed;
    job->meshes = std::move(meshes);
    job->parameters = std::move(parameters);
    {
        const std::lock_guard lock{shared_->mutex};
        if (!shared_->stopping) {
            shared_->jobs.push_back(job);
        }
    }
    return std::unique_ptr<RigidTrack>{new RigidTrack{shared_, std::move(job)}};
}

void RigidRunner::wait_idle() {
    std::unique_lock lock{shared_->mutex};
    shared_->idle.wait(lock, [this] { return shared_->stopping || shared_->settled(); });
}

RigidTrack::RigidTrack(std::shared_ptr<RigidRunner::Shared> shared, std::shared_ptr<RigidRunner::Job> job)
    : shared_{std::move(shared)}, job_{std::move(job)} {}

RigidTrack::~RigidTrack() {
    {
        const std::lock_guard lock{shared_->mutex};
        job_->dropped = true;
        job_->frames.clear();
    }
    shared_->wake.notify_all();
}

std::shared_ptr<const RigidFrame> RigidTrack::frame(std::uint64_t tick) {
    std::shared_ptr<const RigidFrame> found;
    bool work = false;
    {
        const std::lock_guard lock{shared_->mutex};
        RigidRunner::Job& job = *job_;
        job.wanted = tick;
        job.asked = true;
        while (!job.frames.empty() && job.frames.front()->tick < tick) {
            job.frames.pop_front();
        }
        if (!job.frames.empty() && job.frames.front()->tick == tick) {
            found = job.frames.front();
        }
        work = shared_->wants_work(job);
    }
    if (work) {
        shared_->wake.notify_all();
    }
    return found;
}

void RigidTrack::set_parameters(std::shared_ptr<const ParameterProvider> parameters) {
    {
        const std::lock_guard lock{shared_->mutex};
        job_->parameters = std::move(parameters);
        ++job_->generation;
        job_->frames.clear();
    }
    shared_->wake.notify_all();
}

std::optional<Error> RigidTrack::error() const {
    const std::lock_guard lock{shared_->mutex};
    return job_->error;
}

} // namespace ajfx
