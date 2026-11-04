#pragma once

#include <ajfx/math.hpp>

#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What an artist did to a running effect, stamped by the step it happened in:
// a body placed or dropped, a push, a body taken away, a body steered after a
// hand. Actions are inputs to the simulation like parameter values are: a
// pure function of the tick, read by the step they are stamped with and by
// nothing else, so playing through and seeking agree, and a performance
// replays exactly from (effect, seed, actions). They live outside the effect
// (SpawnActions in modules.hpp only declares that an emitter takes them and
// how many), so acting never changes the package.
//
// An action at tick t is applied in the step from t to t + 1: a body spawned
// at t stands at t + 1. The one exception is a rigid physics body spawned at
// tick 0, which stands before the first step, as a pile does: that is where
// a stage's initial state goes.
namespace ajfx {

enum class ActionKind : std::uint8_t {
    // A body (rigid physics emitter) or a particle born where the action
    // says, of `body` kind, with `serial` as its spawn count. A body takes
    // the pose and motion exactly; a particle starts at `position`, offset
    // by what its init modules give it, and is thrown by `velocity` beside
    // theirs. A body with `release` past its tick sleeps where it is put
    // until then.
    Spawn,
    // Adds `push` to the velocity and `twist` to the angular velocity of the
    // body whose spawn index is `target`, waking it. Bodies only: a particle
    // emitter takes Spawn alone, on the CPU and the GPU alike.
    Push,
    // Ends the life of the body whose spawn index is `target`: it leaves in
    // this step, as one whose life ran out. Bodies only.
    Remove,
    // Steers the body whose spawn index is `target` towards `position` and
    // `orientation`: its velocity is changed towards the one that would
    // reach there in 1 / `gain` seconds, no faster than `max_speed`, and its
    // spin likewise. The body stays in the world's hands, so it is stopped
    // by what it meets; steering every step is a hand holding it, and not
    // steering is letting go, with whatever speed the last step gave it.
    Steer,
};

struct Action final {
    ActionKind kind{ActionKind::Spawn};
    std::uint64_t tick{0U};
    // Spawn: this particle's count within its emitter's SpawnActions source
    // (spawn_index_of). A provider hands each spawn a serial of its own, never
    // reused, so taking one action away or moving it leaves every other
    // spawn's random draws as they were. Below 2^24.
    std::uint32_t serial{0U};
    // Spawn: which of the emitter's body kinds (0 is its own).
    std::uint32_t body{0U};
    Vec3 position{};
    Quat orientation{};
    Vec3 velocity{};
    Vec3 angular_velocity{};
    // Spawn: the body's size, or 0 for what its init modules give it.
    float size{0.0F};
    // Spawn: the tick a body wakes at; at or before its own tick, it is
    // awake from the start.
    std::uint64_t release{0U};
    // Push, Remove, Steer: the spawn index of the particle acted on.
    std::uint32_t target{0U};
    Vec3 push{};
    Vec3 twist{};
    float gain{8.0F};
    float max_speed{12.0F};

    friend bool operator==(const Action&, const Action&) = default;
};

// Supplies an emitter's actions per step, by the emitter's name. Must be a
// pure function of (emitter, tick): a host that appends while a simulation
// runs hands it a snapshot and tells the simulation's checkpoints which
// ticks changed (CheckpointRing::clear_from, RigidTrack::set_actions). May
// be called from a RigidRunner's thread.
class ActionProvider {
public:
    virtual ~ActionProvider() = default;
    // The actions of `emitter` stamped `tick`, in the order they are to be
    // applied. Good until the provider is changed.
    [[nodiscard]] virtual std::span<const Action> actions_at(std::string_view emitter, std::uint64_t tick) const = 0;
    // A hash of everything `emitter`'s actions are, whatever the tick: what
    // a cache baked with them is keyed by (CacheIdentity).
    [[nodiscard]] virtual std::uint64_t digest(std::string_view emitter) const = 0;
};

// A hash of `actions`, every field of every one, in order.
[[nodiscard]] std::uint64_t action_digest(std::span<const Action> actions, std::uint64_t hash = 0xcbf29ce484222325ULL);

// Actions kept by emitter, each emitter's in tick order and, within a tick,
// the order they were added. A value: copy it to snapshot it.
class ActionList final : public ActionProvider {
public:
    // Adds `action` to `emitter`'s, keeping tick order.
    void add(std::string_view emitter, const Action& action);
    // Every action of `emitter`, in tick order.
    [[nodiscard]] std::span<const Action> actions(std::string_view emitter) const;
    // Removes the actions of `emitter` for which `drop(action)` is true.
    template <class Drop>
    void remove_if(std::string_view emitter, Drop&& drop) {
        const auto found = lists_.find(emitter);
        if (found != lists_.end()) {
            std::erase_if(found->second, drop);
        }
    }
    [[nodiscard]] bool empty() const noexcept { return lists_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept;

    [[nodiscard]] std::span<const Action> actions_at(std::string_view emitter, std::uint64_t tick) const override;
    [[nodiscard]] std::uint64_t digest(std::string_view emitter) const override;

private:
    std::map<std::string, std::vector<Action>, std::less<>> lists_;
};

} // namespace ajfx
