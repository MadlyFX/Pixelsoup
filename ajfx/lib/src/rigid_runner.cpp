#include <ajfx/rigid_runner.hpp>

#include <ajfx/cache.hpp>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
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
    // What the whole effect reads of each of the part's emitters' contacts.
    std::vector<std::optional<float>> contact_floors;
    // Whether any of the part's emitters reads a parameter.
    bool reads_any_parameter{false};
    // Played from a cache rather than simulated: each frame is what the
    // cache holds of every emitter it caches, and nothing is stepped.
    std::shared_ptr<const Cache> cache;
    // Which of the whole effect's emitters the cache holds.
    std::vector<std::size_t> cached;

    // Under Shared::mutex.
    std::uint64_t wanted{0U};
    bool asked{false};
    std::uint64_t generation{0U};
    std::shared_ptr<const ParameterProvider> parameters;
    std::shared_ptr<const ActionProvider> actions;
    // Set by set_actions: the first tick whose actions changed since the
    // generation the simulation follows, so the next turn keeps what stands
    // before it rather than starting over.
    std::optional<std::uint64_t> revise_from;
    std::optional<std::uint32_t> lead;
    // Ascending ticks, all from `generation`, none before `wanted`.
    std::deque<std::shared_ptr<const RigidFrame>> frames;
    std::optional<Error> error;
    bool dropped{false};
    bool busy{false};

    // Only the runner's thread.
    std::optional<Simulation> simulation;
    std::optional<std::uint64_t> built; // the generation `simulation` follows
    std::vector<SimulationState> checkpoints;
    // A cached track's emitters as decoded at `decoded_tick`, to move on
    // from a frame at a time (Cache::advance).
    std::vector<EmitterState> decoded;
    std::optional<std::uint64_t> decoded_tick;
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

    [[nodiscard]] std::uint32_t lead_of(const Job& job) const { return job.lead.value_or(options.lead); }

    [[nodiscard]] bool wants_work(const Job& job) const {
        return job.asked && !job.dropped && !job.error && first_missing(job) <= job.wanted + lead_of(job);
    }

    // The frame path is waiting on it.
    [[nodiscard]] static bool urgent(const Job& job) { return first_missing(job) == job.wanted; }

    [[nodiscard]] bool settled() const {
        return std::none_of(jobs.begin(), jobs.end(),
                            [this](const std::shared_ptr<Job>& job) { return job->busy || wants_work(*job); });
    }

    void run();
    struct Turn final {
        std::uint64_t from{0U};
        std::uint64_t wanted{0U};
        std::uint64_t generation{0U};
        std::uint32_t lead{0U};
        std::optional<std::uint64_t> revise_from;
        const ParameterProvider* parameters{nullptr};
        const ActionProvider* actions{nullptr};
    };
    [[nodiscard]] std::optional<Error> turn(Job& job, const Turn& take,
                                            std::vector<std::shared_ptr<const RigidFrame>>& produced) const;
    // A cached track's turn: reads rather than steps.
    void read_turn(Job& job, const Turn& take, std::vector<std::shared_ptr<const RigidFrame>>& produced) const;
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
    // Which kind each body is: a host draws a kind by its own model.
    out.body_kind = s.body_kind;
    // Which bodies wait asleep for an action's release: the state a frame is
    // compared with holds it.
    out.held_until = s.held_until;
    // The props' poses: a host draws them, so they travel with the particles.
    out.props = s.props;
    out.spawned = s.spawned;
    out.refused = s.refused;
    out.rate_carry = s.rate_carry;
    out.spawn_counts = s.spawn_counts;
    return out;
}

} // namespace

void RigidRunner::Shared::read_turn(Job& job, const Turn& take,
                                    std::vector<std::shared_ptr<const RigidFrame>>& produced) const {
    const Cache& cache = *job.cache;
    const std::uint64_t horizon = take.wanted + take.lead;
    // Bodies a frame at a time are cheap; a particle layer is decoded from
    // its keyframe on a jump and moved on a frame at a time after that.
    const std::uint32_t most = std::max(options.turn, 1U) * 8U;
    if (job.decoded.size() != job.emitters) {
        job.decoded.assign(job.emitters, EmitterState{});
        job.decoded_tick.reset();
    }
    std::uint32_t n = 0U;
    for (std::uint64_t tick = take.from; tick <= horizon && n < most; ++tick, ++n) {
        const bool onward = job.decoded_tick && *job.decoded_tick + 1U == tick;
        for (const std::size_t e : job.cached) {
            if (onward) {
                cache.advance(e, tick, job.decoded[e]);
            } else {
                job.decoded[e] = cache.emitter_at(e, tick);
            }
        }
        job.decoded_tick = tick;
        auto frame = std::make_shared<RigidFrame>();
        frame->tick = tick;
        frame->emitters.resize(job.emitters);
        frame->contacts.resize(job.emitters);
        frame->contacts_dropped.resize(job.emitters, 0U);
        for (const std::size_t e : job.cached) {
            frame->emitters[e] = job.decoded[e];
        }
        produced.push_back(std::move(frame));
    }
}

std::optional<Error> RigidRunner::Shared::turn(Job& job, const Turn& take,
                                               std::vector<std::shared_ptr<const RigidFrame>>& produced) const {
    if (job.cache) {
        read_turn(job, take, produced);
        return std::nullopt;
    }
    const std::uint64_t from = take.from;
    const std::uint64_t wanted = take.wanted;
    const std::uint64_t generation = take.generation;
    const ParameterProvider* parameters = take.parameters;
    const ActionProvider* actions = take.actions;
    if (!job.simulation) {
        Result<Simulation> made = Simulation::create(job.part, job.seed,
                                                     SimulationOptions{.meshes = job.meshes,
                                                                       .rigid = solver,
                                                                       .contacts = options.contacts,
                                                                       .contact_floors = job.contact_floors});
        if (!made) {
            return made.error();
        }
        job.simulation.emplace(std::move(made).value());
    }
    Simulation& simulation = *job.simulation;
    // New parameters (or none followed yet): what is held describes another
    // past. New actions from a tick on: what is held up to that tick stands,
    // and a simulation past it has to come back to a checkpoint before it.
    bool stale = false;
    if (job.built != generation) {
        if (take.revise_from && job.built) {
            const std::uint64_t revised = *take.revise_from;
            std::erase_if(job.checkpoints,
                          [revised](const SimulationState& c) { return c.tick > revised || revised == 0U; });
            stale = simulation.tick() > revised || revised == 0U;
        } else {
            job.checkpoints.clear();
            simulation.reset(parameters, actions);
        }
        job.built = generation;
    }

    // Behind where it stands: back to the latest checkpoint before the
    // first missing tick. Ahead: on from a checkpoint already past it. A
    // frame carries the contacts of the step that reached it, which a
    // restore has not taken, so a checkpoint is only ever stepped on from:
    // one at the first missing tick itself would hand that tick out with
    // none.
    const SimulationState* best = nullptr;
    for (const SimulationState& checkpoint : job.checkpoints) {
        const bool before = checkpoint.tick < from || (from == 0U && checkpoint.tick == 0U);
        if (before && (best == nullptr || checkpoint.tick > best->tick)) {
            best = &checkpoint;
        }
    }
    if (simulation.tick() > from || stale) {
        if (best != nullptr) {
            simulation.restore(*best);
        } else {
            simulation.reset(parameters, actions);
        }
    } else if (best != nullptr && best->tick > simulation.tick()) {
        simulation.restore(*best);
    }
    simulation.prepare(parameters, actions);

    const std::uint64_t horizon = wanted + take.lead;
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
            frame->contacts.resize(job.emitters);
            frame->contacts_dropped.resize(job.emitters, 0U);
            for (std::size_t k = 0U; k < job.emitter_of.size(); ++k) {
                frame->emitters[job.emitter_of[k]] = particles_of(simulation.emitter(k));
                const std::span<const RigidContact> contacts = simulation.contacts(k);
                frame->contacts[job.emitter_of[k]].assign(contacts.begin(), contacts.end());
                frame->contacts_dropped[job.emitter_of[k]] = simulation.contacts_dropped(k);
            }
            produced.push_back(std::move(frame));
        }
        if (tick >= horizon || steps == std::max(options.turn, 1U)) {
            break;
        }
        simulation.step(parameters, actions);
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
        Turn take;
        take.wanted = pick->wanted;
        take.from = first_missing(*pick);
        take.generation = pick->generation;
        take.lead = lead_of(*pick);
        take.revise_from = pick->revise_from;
        const std::shared_ptr<const ParameterProvider> parameters = pick->parameters;
        const std::shared_ptr<const ActionProvider> actions = pick->actions;
        take.parameters = parameters.get();
        take.actions = actions.get();
        lock.unlock();

        std::vector<std::shared_ptr<const RigidFrame>> produced;
        std::optional<Error> failed = turn(*pick, take, produced);

        lock.lock();
        pick->busy = false;
        if (failed) {
            pick->error = std::move(failed);
        }
        if (pick->generation == take.generation) {
            pick->revise_from.reset();
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
                                               std::shared_ptr<const ParameterProvider> parameters,
                                               std::shared_ptr<const ActionProvider> actions,
                                               std::shared_ptr<const Cache> cache) {
    auto job = std::make_shared<Job>();
    job->part = rigid_physics_part(effect);
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        if (rigid_physics(effect.emitters[i]) != nullptr) {
            job->emitter_of.push_back(i);
            job->contact_floors.push_back(contact_floor(effect, i));
        }
    }
    job->emitters = effect.emitters.size();
    job->reads_any_parameter = std::any_of(job->part.emitters.begin(), job->part.emitters.end(),
                                        [&](const Emitter& emitter) { return reads_parameters(effect, emitter); });
    job->seed = instance_seed;
    job->meshes = std::move(meshes);
    job->parameters = std::move(parameters);
    job->actions = std::move(actions);
    if (cache) {
        // Every rigid physics emitter is read from it, so the track needs no
        // solver; one it plays live would need the bodies it does not hold.
        for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
            if (cache->mode(i) != CacheMode::Live) {
                job->cached.push_back(i);
            } else if (rigid_physics(effect.emitters[i]) != nullptr && !job->error) {
                job->error = Error{ErrorCode::StaleCache, "the cache leaves the rigid physics of emitter '" +
                                                              effect.emitters[i].name +
                                                              "' live: a cached track plays only cached bodies, "
                                                              "so bake them too"};
            }
        }
        job->cache = std::move(cache);
    }
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
        // Bodies that read no parameter have the same past under any.
        if (job_->reads_any_parameter) {
            ++job_->generation;
            job_->frames.clear();
        }
    }
    shared_->wake.notify_all();
}

void RigidTrack::set_actions(std::shared_ptr<const ActionProvider> actions, std::uint64_t from) {
    {
        const std::lock_guard lock{shared_->mutex};
        RigidRunner::Job& job = *job_;
        job.actions = std::move(actions);
        // Frames from `from` on, and whatever a turn under way produces,
        // describe the old actions. The turn after sees where to keep from.
        std::erase_if(job.frames,
                      [from](const std::shared_ptr<const RigidFrame>& f) { return f->tick > from || from == 0U; });
        ++job.generation;
        if (job.built) {
            job.revise_from = job.revise_from ? std::min(*job.revise_from, from) : from;
        }
    }
    shared_->wake.notify_all();
}

void RigidTrack::set_lead(std::uint32_t lead) {
    {
        const std::lock_guard lock{shared_->mutex};
        job_->lead = lead;
    }
    shared_->wake.notify_all();
}

std::optional<Error> RigidTrack::error() const {
    const std::lock_guard lock{shared_->mutex};
    return job_->error;
}

} // namespace ajfx
