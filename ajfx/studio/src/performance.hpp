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
#include <vector>

// A scene's performance (scene.hpp) as the simulation takes it: acts turned
// into ajfx::Actions on the solid world, the one emitter they act on. Acting
// never changes the package.
// - A drop is a Spawn of the dropped Object's body kind with the drop's
//   serial as its spawn count, so taking an earlier drop away leaves every
//   later dropped body's variation as it was.
// - A paint act is one Spawn for each thing it lays down, each with a serial
//   of its own, spread over the brush and turned as painted_things says.
// - A deletion is a Remove of the body's spawn index, a push a Push of it.
// - A grab is a Steer of the body at every step from its first sample to its
//   last, towards the point along the line between the samples either side;
//   after the last it is let go, with the speed the hand gave it.
// - A blast is a Blast at its centre.
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
// are left out and counted in `skipped`, and their serials listed in
// `unapplied`. Immutable once made: a new performance makes a new one.
[[nodiscard]] std::shared_ptr<const ActionList> performance_actions(const Performance& performance,
                                                                    const ActTargets& targets,
                                                                    std::size_t* skipped = nullptr,
                                                                    std::vector<std::uint32_t>* unapplied = nullptr);

// Records a drop of `object` at `tick` and returns its serial. Acts stay in
// tick order; one at the same tick as others goes after them.
std::uint32_t add_drop(Performance& performance, std::uint64_t tick, Uid object, Vec3 position, Vec3 turn,
                       Vec3 velocity);
std::uint32_t add_delete(Performance& performance, std::uint64_t tick, BodyRef target);
// Adds `push` m/s to the body's velocity and `twist` radians a second to its
// spin, at `tick`.
std::uint32_t add_push(Performance& performance, std::uint64_t tick, BodyRef target, Vec3 push, Vec3 twist);
// Sets off a blast at `center` at `tick`, `power` m/s at its centre falling
// to nothing at `radius` metres.
std::uint32_t add_blast(Performance& performance, std::uint64_t tick, Vec3 center, float radius, float power);

// What a hand holding a body does unless told otherwise: closes on where it
// is steered in a tenth of a second, at up to 20 m/s, fast enough to throw.
inline constexpr float grab_gain = 10.0F;
inline constexpr float grab_max_speed = 20.0F;
// Takes hold of the body `target` at `tick`, steering its middle to `point`
// and holding it turned as `turn` (degrees, as an Object's rotation).
// A `firm` grab holds it exactly there (Act::firm) rather than steering it.
std::uint32_t add_grab(Performance& performance, std::uint64_t tick, BodyRef target, Vec3 point, Vec3 turn,
                       float gain = grab_gain, float max_speed = grab_max_speed, bool firm = false);
// Steers the body the grab `serial` holds to `point` from `tick` on. A
// sample at the grab's last step replaces where that one steers to; an
// earlier one is refused, as is one for an act that is no grab.
bool extend_grab(Performance& performance, std::uint32_t serial, std::uint64_t tick, Vec3 point);

// One dab of a paint stroke: `count` copies of `object` laid down at once
// over a disc of `radius` metres about `position`, across the surface whose
// way out is `normal`. `stroke` names the stroke it is part of: 0 for its
// first dab, which then names the stroke by its own serial.
struct PaintDab final {
    std::uint64_t tick{0U};
    Uid object{};
    Vec3 position{};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    Vec3 turn{};
    Vec3 velocity{};
    float radius{0.5F};
    std::uint32_t count{1U};
    float falloff{0.0F};
    PaintTurn paint_turn{PaintTurn::Realistic};
    std::uint32_t stroke{0U};
};
// Records a dab and returns its serial, the first of the `count` it holds.
std::uint32_t add_paint(Performance& performance, const PaintDab& dab);

// Where each thing a paint act lays down starts, and how it is turned: its
// place is drawn over the brush's disc (gathered to the middle by the
// falloff) and its turn by the paint act's PaintTurn, each from the thing's
// own serial alone. In serial order.
struct PaintedThing final {
    std::uint32_t serial{};
    Vec3 position{};
    Quat orientation{};
};
[[nodiscard]] std::vector<PaintedThing> painted_things(const Act& paint);

// The last step an act acts in: a grab's last sample, any other act's own.
[[nodiscard]] std::uint64_t act_end(const Act& act);
// Whether `act` made the dropped or painted body of serial `drop`.
[[nodiscard]] bool makes_body(const Act& act, std::uint32_t drop);
// The act that made the dropped or painted body of serial `drop`; null when
// there is none.
[[nodiscard]] const Act* maker_of(const Performance& performance, std::uint32_t drop);
// The Object a body is a copy of, or is: what its act's timeline track is.
// 0 for a body made by no act there is.
[[nodiscard]] Uid object_of(const Performance& performance, const BodyRef& body);

// Takes an act away. One that made bodies (a drop, a dab of paint) takes
// with it every act on them, since those named a body no longer made.
// Returns whether there was one.
bool remove_act(Performance& performance, std::uint32_t serial);
// Moves an act to another tick, keeping tick order. A grab moves whole.
bool retime_act(Performance& performance, std::uint32_t serial, std::uint64_t tick);
// The acts of a paint stroke, by its first dab's serial, in tick order.
[[nodiscard]] std::vector<std::uint32_t> stroke_acts(const Performance& performance, std::uint32_t stroke);
// Takes a whole stroke away, or moves it whole so that it starts at `tick`.
bool remove_stroke(Performance& performance, std::uint32_t stroke);
bool retime_stroke(Performance& performance, std::uint32_t stroke, std::uint64_t tick);
// Re-recording from a step: takes away every act from `tick` on, and cuts
// short a grab still holding then so it lets go before `tick`. What came
// before is left as it was, serials included. Returns how many acts were
// taken away or cut short.
std::size_t rerecord_from(Performance& performance, std::uint64_t tick);
// Takes away every act on an Object (one removed from the scene).
void forget_object(Performance& performance, Uid object);
[[nodiscard]] const Act* find_act(const Performance& performance, std::uint32_t serial);

// The first tick at which two performances act differently: where a
// simulation that followed `before` has to go back to for `after`. Nothing
// when they act alike. A grab that only goes on further (a hand still
// holding) differs from the step after its old last sample, so carrying a
// body replays nothing.
[[nodiscard]] std::optional<std::uint64_t> first_difference(const Performance& before, const Performance& after);

// The scene without its performance: what a package is built of, so an act
// rebuilds nothing.
[[nodiscard]] Document without_performance(Document document);

// ------------------------------------------------------- acting in the viewport

// What a click in the viewport does: selects what it lands on, drops a copy
// of the selected Object above where it lands, deletes the body it lands on,
// or places a copy of the selected Object on the stage where it lands, to
// stand there from the first step (stage.hpp).
enum class StageTool : std::uint8_t { Select, Drop, Delete, Place };

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

// What the performance holds, counted by kind in the artist's words: "2
// drops, 1 paint stroke, 3 pushes". Empty for an empty one.
[[nodiscard]] std::string performance_summary(const Performance& performance);

// Dropped and painted things the performance holds at once: those its drops
// and paint make less the deletions of them.
[[nodiscard]] std::size_t dropped_count(const Performance& performance);

struct StageAct final {
    // The act made; 0 when it was refused.
    std::uint32_t serial{0U};
    // Why it was refused, in the artist's words.
    std::string refusal;
};
// Does what `tool` does at `hit` at step `tick` (a Place is always at the
// first step): a Drop of the `selected`
// Object, its bottom drop_height above the point, turned as the Object is;
// a Delete of the body hit. Refused, changing nothing, where it cannot be
// done: nothing selected to drop, an Object that is not loose, a scene that
// takes no drops or holds its most, a click on no body or one that is not
// an Object's or a drop's.
[[nodiscard]] StageAct act_on_stage(Document& document, const ActTargets& targets, StageTool tool, Uid selected,
                                    const StageHit& hit, std::uint64_t tick);

} // namespace ajfx::studio
