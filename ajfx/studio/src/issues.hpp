#pragma once

#include "bake.hpp"
#include "hands.hpp"
#include "scene.hpp"

#include <ajfx/rigid_solver.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What is wrong with a scene, or about to be, in one list: the Issues panel's
// contents, worked out headless so the command line and the tests read the
// same list the panel will draw.
// - Before it plays: the scene does not validate or compile; the solid world
//   is near or over its budgets (body kinds, which every piece of a
//   breakable thing counts against, and props); acts this compile cannot
//   apply; a wind told to blow on things in a scene where nothing moves.
// - As it plays: drops and painted things the world refused, each with its
//   act, its step and why (the scene's most dropped things at once, or too
//   many in one step); particle systems that ran out of room.
// - Its cache: baked items whose cache is stale, missing or cannot be
//   kept, and a cache file that no longer matches the scene.
namespace ajfx::studio {

enum class IssueLevel : std::uint8_t {
    Note,    // worth knowing; nothing is lost
    Warning, // something the artist asked for is not happening, or soon will not
    Problem, // the scene cannot play or deliver as it is
};

enum class IssueKind : std::uint8_t {
    Scene,           // validation or compile
    Unapplied,       // an act this compile has nothing to apply to
    RefusedDrop,     // a dropped or painted thing the world did not take
    DropBudget,      // the scene's most dropped things at once was reached
    ParticlesRefused,// a particle system ran out of room
    KindBudget,      // body kinds of max_rigid_body_kinds
    PropBudget,      // props of max_rigid_props
    WindIdle,        // a wind that blows on things, with nothing to blow on
    CacheStale,      // the cache holds the scene as it was
    CacheUnbaked,    // a baked item no cache holds yet
    CacheTooLarge,   // a baked item too big for a cache
};

struct Issue final {
    IssueLevel level{IssueLevel::Note};
    IssueKind kind{IssueKind::Scene};
    // The item it is about, when one is (0 for the scene).
    Uid item{};
    // The act it is about, when one is (its serial), and the step.
    std::uint32_t act{};
    std::uint64_t tick{};
    // In the artist's words.
    std::string message;
    friend bool operator==(const Issue&, const Issue&) = default;
};

[[nodiscard]] std::string_view issue_level_name(IssueLevel level);
[[nodiscard]] std::string_view issue_kind_name(IssueKind kind);

// Share of a budget at which it is worth a warning.
inline constexpr float budget_warning_share = 0.75F;

// What the scene says before anything plays.
[[nodiscard]] std::vector<Issue> scene_issues(const Document& document, const Prepared& prepared);

// What playing the performance from the start to step `to` shows, with
// `seed` as the instance seed: every refused drop and painted thing, and
// every particle system that ran out of room by then.
[[nodiscard]] std::vector<Issue> play_issues(const Document& document, const Prepared& prepared,
                                             const RigidSolver* solver, std::uint32_t seed, std::uint64_t to);

// The cache's state, from what Studio worked out of its last bake.
[[nodiscard]] std::vector<Issue> bake_issues(const Document& document, const BakeStatus& status);

// Whether the cache in `bytes` still holds the scene: opened against the
// scene compiled now, with its performance and `seed`, as Studio's check
// opens it. Nothing when it does.
[[nodiscard]] std::optional<Issue> cache_issue(const Document& document, const Prepared& prepared,
                                               std::span<const std::uint8_t> bytes, std::uint32_t seed);

// Problems first, then warnings, then notes; within a level by step, then
// as they were found.
void sort_issues(std::vector<Issue>& issues);

// The item an emitter of the compiled effect was compiled from ("item_12"),
// or 0.
[[nodiscard]] Uid emitter_item(std::string_view emitter);

} // namespace ajfx::studio
