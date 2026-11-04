#pragma once

#include "compile.hpp"
#include "scene.hpp"
#include "stage_pick.hpp"

#include <ajfx/actions.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>

// A scene's performance (scene.hpp) as the simulation takes it: drops and
// deletions turned into ajfx::Actions on the solid world, the one emitter
// they act on. Acting never changes the package. A drop is a Spawn of the
// dropped Object's body kind with the drop's serial as its spawn count, so
// taking an earlier drop away leaves every later dropped body's variation as
// it was; a deletion is a Remove of the body's spawn index.
namespace ajfx::studio {

// Where a compiled scene takes acts: its solid world, the source drops are
// born of there, and for each Object that is a loose body, its body kind and
// the spawn module its own bodies come from.
struct ActTargets final {
    std::string emitter;
    std::optional<std::uint32_t> drops;
    std::uint32_t max_dropped{0U};
    // By Object item uid.
    std::map<Uid, std::uint32_t> kinds;
    std::map<Uid, std::uint32_t> own;
    // Which Object a spawn module's bodies are, by its place in the stack.
    std::map<std::uint32_t, Uid> owners;
};
[[nodiscard]] ActTargets act_targets(const Compiled& compiled);

// Whether `object` can be dropped in this compile: a loose Object in a world
// that takes drops.
[[nodiscard]] bool droppable(const ActTargets& targets, Uid object);

// The spawn index the body `ref` has in the solid world, or nothing for a
// body the compile does not make.
[[nodiscard]] std::optional<std::uint32_t> body_index(const ActTargets& targets, const BodyRef& ref);
// The body a spawn index is, or nothing for one acts cannot name (a piece of
// something broken, a body of no Object).
[[nodiscard]] std::optional<BodyRef> body_ref(const ActTargets& targets, std::uint32_t spawn_index);

// The performance as actions, keyed by the solid world's name. Acts on an
// Object this compile has no body kind for, or on a body it does not make,
// are left out and counted in `skipped`. Immutable once made: a new
// performance makes a new one.
[[nodiscard]] std::shared_ptr<const ActionList> performance_actions(const Performance& performance,
                                                                    const ActTargets& targets,
                                                                    std::size_t* skipped = nullptr);

// Records a drop of `object` at `tick` and returns its serial. Acts stay in
// tick order; one at the same tick as others goes after them.
std::uint32_t add_drop(Performance& performance, std::uint64_t tick, Uid object, Vec3 position, Vec3 turn,
                       Vec3 velocity);
std::uint32_t add_delete(Performance& performance, std::uint64_t tick, BodyRef target);
// Takes an act away. A drop takes its deletions with it, since they named a
// body that is no longer dropped. Returns whether there was one.
bool remove_act(Performance& performance, std::uint32_t serial);
// Moves an act to another tick, keeping tick order.
bool retime_act(Performance& performance, std::uint32_t serial, std::uint64_t tick);
// Takes away every act on an Object (one removed from the scene).
void forget_object(Performance& performance, Uid object);
[[nodiscard]] const Act* find_act(const Performance& performance, std::uint32_t serial);

// The first tick at which two performances act differently: where a
// simulation that followed `before` has to go back to for `after`. Nothing
// when they act alike.
[[nodiscard]] std::optional<std::uint64_t> first_difference(const Performance& before, const Performance& after);

// The scene without its performance: what a package is built of, so an act
// rebuilds nothing.
[[nodiscard]] Document without_performance(Document document);

// ------------------------------------------------------- acting in the viewport

// What a click in the viewport does: selects what it lands on, drops a copy
// of the selected Object above where it lands, or deletes the body it lands
// on.
enum class StageTool : std::uint8_t { Select, Drop, Delete };

// How far above where a click lands a dropped thing's bottom starts, metres.
inline constexpr float drop_height = 0.75F;

// The step an act done now is stamped with. Playing, one past what the
// preview's rigid physics may have stepped ahead of the step `shown` (`lead`
// steps; mediaserver::render::acting_tick), so the act lands on a step not
// yet worked out and replays nothing. Paused, the step shown, so it happens
// as the scene plays on from there.
[[nodiscard]] constexpr std::uint64_t act_tick(bool playing, std::uint64_t shown, std::uint32_t lead) noexcept {
    return playing ? shown + lead + 1U : shown;
}

// Dropped things the performance holds at once: its drops less the
// deletions of them.
[[nodiscard]] std::size_t dropped_count(const Performance& performance);

struct StageAct final {
    // The act made; 0 when it was refused.
    std::uint32_t serial{0U};
    // Why it was refused, in the artist's words.
    std::string refusal;
};
// Does what `tool` does at `hit` at step `tick`: a Drop of the `selected`
// Object, its bottom drop_height above the point, turned as the Object is;
// a Delete of the body hit. Refused, changing nothing, where it cannot be
// done: nothing selected to drop, an Object that is not loose, a scene that
// takes no drops or holds its most, a click on no body or one that is not
// an Object's or a drop's.
[[nodiscard]] StageAct act_on_stage(Document& document, const ActTargets& targets, StageTool tool, Uid selected,
                                    const StageHit& hit, std::uint64_t tick);

} // namespace ajfx::studio
