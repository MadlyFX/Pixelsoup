#pragma once

#include "performance.hpp"

#include <ajfx/math.hpp>
#include <ajfx/rigid_solver.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

// The stage: what stands in the scene before it plays. A thing on the stage
// is a drop at step 0 (a Place), so there is one kind of record for what an
// artist puts down and what they drop later, and both replay the same. The
// setup tools work on those: put a thing somewhere by hand, jitter them so
// a neat stack loosens, settle them under gravity until they rest and keep
// where they came to rest, and reset them to where they were put.
namespace ajfx::studio {

// Whether an act is a thing on the stage.
[[nodiscard]] constexpr bool on_stage(const Act& act) noexcept {
    return act.kind == ActKind::Drop && act.tick == 0U;
}
[[nodiscard]] std::size_t stage_count(const Performance& performance);

// The degrees (x, y, z as an Object's rotation) that prop_orientation turns
// into `q`; the inverse, up to the several turns that make one pose.
[[nodiscard]] Vec3 turn_of(Quat q) noexcept;

// A pose as a 4x4 matrix, column-major (translation in the last column), as a
// gizmo takes it: the rotation prop_orientation makes of `turn`, at `position`.
using PoseMatrix = std::array<float, 16>;
[[nodiscard]] PoseMatrix pose_matrix(Vec3 position, Vec3 turn) noexcept;
// The position and turn (as an Object's rotation, in degrees) a rigid pose
// matrix has. Any scale in it is taken out first.
void pose_from_matrix(const PoseMatrix& matrix, Vec3& position, Vec3& turn) noexcept;

// Makes a kind placed by hand of each model file in `files` (an OBJ, glTF or
// GLB, whatever the case of its extension; anything else is left alone) and
// returns the kinds in the order of the files. Dynamic, as a placed thing has
// to be.
[[nodiscard]] std::vector<Uid> add_dropped_models(Document& document, std::span<const std::string> files);

// Puts a copy of the Object `kind` on the stage with its middle at
// `position`. Refused, changing nothing, for an Object that is not a kind
// that can be dropped, a scene that takes no drops or holds its most.
[[nodiscard]] StageAct place(Document& document, const ActTargets& targets, Uid kind, Vec3 position, Vec3 turn);

// Puts a thing already on the stage somewhere by hand. Where it is put is
// its home. False for an act that is no thing on the stage.
bool move_placement(Performance& performance, std::uint32_t serial, Vec3 position, Vec3 turn);

// Shifts every thing on the stage by up to `shift` metres each way and
// tilts it by up to `tilt` degrees about each axis, as `seed` and the
// thing's own serial draw: the same seed does the same every time, and one
// thing being taken away moves no other. Returns how many were moved.
std::size_t jitter_stage(Performance& performance, std::uint32_t seed, float shift, float tilt);

// Puts everything the setup tools moved back where it was put. Returns how
// many were moved back.
std::size_t reset_stage(Performance& performance);

struct SettleOptions final {
    std::uint32_t instance_seed{0U};
    // The most simulated time given to come to rest, seconds.
    double most_seconds{8.0};
    // How long everything has to be still for, seconds, and how still:
    // metres a second, and radians a second.
    double still_for{0.5};
    float still_speed{0.03F};
    float still_spin{0.15F};
    // Hears the share done and may stop it by returning false.
    std::function<bool(double)> progress;
};

struct SettleResult final {
    // Things moved to where they came to rest.
    std::size_t settled{0U};
    // Things still moving when time ran out, left where they were put.
    std::size_t moving{0U};
    // Steps simulated.
    std::uint64_t steps{0U};
    // Why nothing was done, in the artist's words.
    std::string refusal;
};

// Lets the things on the stage fall and come to rest, with the scene as it
// stands (its own piles and props too), and keeps each one where it came to
// rest. `compiled` is the scene compiled for export with its files read and
// its cuts made. The performance after the stage is not played.
[[nodiscard]] SettleResult settle_stage(Document& document, const Compiled& compiled, const RigidSolver* solver,
                                        const SettleOptions& options = {});

} // namespace ajfx::studio
