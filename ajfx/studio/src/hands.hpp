#pragma once

#include "compile.hpp"
#include "document.hpp"
#include "performance.hpp"
#include "stage_pick.hpp"

#include <ajfx/math.hpp>
#include <ajfx/rigid_solver.hpp>
#include <ajfx/simulation.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Hands: what an artist does to a scene while it plays, as acts of its
// performance (performance.hpp). Each tool here is a function of where the
// artist touched and what stood there at the step it is stamped with, and
// writes acts; nothing here draws or reads input, so the viewport, the
// command line and the tests all act through the same calls.
// - A brush lays copies of an Object down over a disc (Paint), takes the
//   bodies under it away (Erase), or pushes them out from its middle or
//   draws them in (Push, Pull). It is dabbed once a step while it is held:
//   one stroke is many dabs.
// - A grab takes hold of a body where the pointer met it and carries it
//   across a plane facing the camera through that point, deeper or nearer
//   with the wheel; letting go throws it with the speed it was carried at.
// - A poke pushes the body it lands on along the pointer, spinning it about
//   its middle as a push off-centre does.
// - A blast throws everything near where it is set off out from there, and
//   breaks what breaks when blasted that hard.
namespace ajfx::studio {

// ------------------------------------------------------------------ brush

enum class BrushMode : std::uint8_t { Paint, Erase, Push, Pull };

struct Brush final {
    BrushMode mode{BrushMode::Paint};
    // The disc it covers, metres.
    float radius{0.5F};
    // Paint: things laid down each dab, at most SolidWorld::drops_per_step.
    std::uint32_t count{3U};
    // Paint: 0 spreads them evenly, 1 gathers them to the middle. Push,
    // Pull: 0 pushes all alike, 1 pushes nothing at the edge.
    float falloff{0.0F};
    PaintTurn turn{PaintTurn::Realistic};
    // Push, Pull: the speed given at the middle, m/s.
    float strength{3.0F};
    // How far above the surface a painted thing's middle starts, beyond half
    // its size, metres: a little, so it settles rather than starts inside.
    float lift{0.02F};
};

struct BrushDab final {
    // The acts made: the dab of paint, or a deletion or push per body.
    std::vector<std::uint32_t> serials;
    // The stroke a paint dab is part of; pass it to the next dab.
    std::uint32_t stroke{0U};
    // The step a dab of paint was put at: the one asked for, or the next
    // with room for its births (SolidWorld::drops_per_step a step).
    std::uint64_t tick{0U};
    // Why nothing was done, in the artist's words.
    std::string refusal;
};

// One dab of `brush` at `point` on a surface whose way out is `normal`, at
// step `tick`. Paint lays copies of `kind` down; Erase, Push and Pull act on
// `bodies` (where the scene's bodies stand at that step, stage_bodies)
// within the brush's radius of the point. `stroke` is what the stroke's
// first dab returned, or 0 for a first dab. Refused, changing nothing, when
// there is nothing to do: a kind that cannot be dropped, a dab that would
// take the scene past its most dropped things, no body under the brush.
[[nodiscard]] BrushDab brush_dab(Document& document, const ActTargets& targets, const Brush& brush, Uid kind,
                                 Vec3 point, Vec3 normal, std::span<const StageBody> bodies, std::uint64_t tick,
                                 std::uint32_t stroke = 0U);

// ------------------------------------------------------------------- grab

// The plane a held body is carried across: through `point`, facing `normal`.
struct GrabPlane final {
    Vec3 point{};
    Vec3 normal{0.0F, 0.0F, 1.0F};
};

// Where the ray from `origin` along `direction` meets the plane; nothing
// when it runs along it or away from it.
[[nodiscard]] std::optional<Vec3> on_plane(const GrabPlane& plane, Vec3 origin, Vec3 direction);

// A hand holding a body: the grab act, the plane it is carried across, and
// where the body's middle is from the point the pointer holds.
struct Hold final {
    std::uint32_t serial{0U};
    GrabPlane plane{};
    Vec3 offset{};
};

struct GrabStart final {
    Hold hold{};
    std::string refusal;
};

// Takes hold of the body `hit` landed on at step `tick`, held turned as
// `held` (how it stands then), carried across the plane through the point
// hit facing `view` (the way the camera looks). Refused for a click on no
// body, or on one acts cannot name (a piece of something broken).
// A `firm` hand holds it exactly where it is carried, through whatever is
// in the way (Act::firm).
[[nodiscard]] GrabStart begin_grab(Document& document, const ActTargets& targets, const StageHit& hit, Quat held,
                                   Vec3 view, std::uint64_t tick,
                                   float gain = grab_gain, float max_speed = grab_max_speed, bool firm = false);

// Carries the held body to where the pointer's ray meets the plane, from
// step `tick` on. Called once a step for as long as it is held, even when
// the pointer is still: the last call's step is where it is let go. False
// when the ray misses the plane or the step is earlier than the last.
bool carry(Document& document, const Hold& hold, Vec3 origin, Vec3 direction, std::uint64_t tick);

// Moves the plane a held body is carried across `metres` further from the
// camera (nearer when negative), as the wheel does.
void push_deeper(Hold& hold, float metres);

// ------------------------------------------------------------ poke, blast

// How hard a poke pushes, m/s, unless told otherwise.
inline constexpr float poke_speed = 4.0F;

// Pushes the body `hit` landed on along `direction` at `speed` m/s, at step
// `tick`. Off its middle (`radius` metres in size, as StageBody has it) the
// push spins it too, as it would a ball. Refused for a click on no body, or
// on one acts cannot name.
[[nodiscard]] StageAct poke(Document& document, const ActTargets& targets, const StageHit& hit, Vec3 direction,
                            float speed, float radius, std::uint64_t tick);

// Sets off a blast at `center` at step `tick`: `power` m/s at its centre,
// nothing beyond `radius` metres. Refused in a scene with no solid world.
[[nodiscard]] StageAct set_off_blast(Document& document, const ActTargets& targets, Vec3 center, float radius,
                                     float power, std::uint64_t tick);

// ------------------------------------------------- playing the performance

// A compiled scene as acts and replays need it, without a window: compiled
// for export, its cuts made. Models from the artist's files are not read
// here; a scene that names one is refused with the file named.
struct Prepared final {
    std::optional<Compiled> compiled;
    ActTargets targets;
    std::string error;
};
[[nodiscard]] Prepared prepare_scene(const Document& document);

// The scene played with its performance from the start to step `to` on the
// CPU, with `seed` as its instance seed.
struct Played final {
    std::optional<Simulation> simulation;
    // The solid world's place among the emitters, when there is one.
    std::optional<std::size_t> world;
    // Acts this compile could not apply (performance_actions).
    std::size_t skipped{0U};
    std::string error;
};
[[nodiscard]] Played play_scene(const Document& document, const Prepared& prepared, const RigidSolver* solver,
                                std::uint32_t seed, std::uint64_t to);
// Plays on from where `played` is to step `to`, with the performance as it
// is now. Acts at steps already played are not applied again.
void play_on(Played& played, const Document& document, const Prepared& prepared, std::uint64_t to);

// A body of the solid world as it stands in a played scene.
struct WorldBody final {
    std::uint32_t spawn_index{};
    // How acts name it; none for a piece of something broken.
    std::optional<BodyRef> ref;
    Vec3 position{};
    Quat orientation{};
    Vec3 velocity{};
    // Its longest side, metres (its kind's scale applied).
    float size{};
    std::uint32_t kind{};
};
[[nodiscard]] std::vector<WorldBody> world_bodies(const Played& played, const Prepared& prepared);
// The one named `ref`, if it stands there.
[[nodiscard]] std::optional<WorldBody> find_body(const Played& played, const Prepared& prepared,
                                                 const BodyRef& ref);
// The bodies as picking and the brush take them (stage_bodies).
[[nodiscard]] std::vector<StageBody> played_stage(const Played& played, const Prepared& prepared);

// A hash of everything every emitter holds at the step it is at: what two
// replays of one performance agree on bit for bit.
[[nodiscard]] std::uint64_t state_digest(const Simulation& simulation);

} // namespace ajfx::studio
