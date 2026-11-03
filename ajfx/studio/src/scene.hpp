#pragma once

#include <ajfx/modules.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace ajfx::studio {

// These are authored owners, independent of the compiler's emitter packing.
// UIDs reserve 32 internal component IDs, so lowering never allocates identities
// according to tree order. Organization is deliberately not transform parenting.
enum class ObjectMotion { Static, Dynamic, Animated };
enum class ObjectCollider { Hull, Sphere, Box };
enum class Distribution { Single, Grid, Pile };
enum class SourceMode { Initial, Continuous, Burst };
enum class ParticleLook { Glow, Smoke, Solid };
// A behavior in a system's Motion stack. Each is a typed owner of one runtime
// force; the stack's order is authored and is the order the forces run in.
// A value each particle picks for itself, between the two, once at birth.
struct Range final {
    float min{};
    float max{};
    friend bool operator==(const Range &, const Range &) = default;
};
enum class BehaviorKind { Drag, Turbulence, Vortex, Wind, Attract, DrawIn };
enum class VortexAxis { Vertical, Horizontal };
// What sets a value over a particle's life: a constant, or keys over its
// normalized age (0 at birth, 1 at death).
enum class Driver { Constant, Age };

struct MotionBehavior final {
    std::uint32_t uid{};
    BehaviorKind kind{BehaviorKind::Drag};
    bool enabled{true};
    // Each particle's share, between the two. Drag: speed lost per second;
    // Turbulence: amount; Vortex, Wind, Attract: m/s^2 (negative turns or
    // pushes the other way); Draw in: pull per metre from the line.
    Range strength{1.0F, 1.0F};
    float size{0.6F};     // Turbulence: swirl size, metres
    float churn{0.4F};    // Turbulence: how fast the swirls change
    Vec3 center{};        // Vortex: where its axis passes; Attract: the point; Draw in: base of the line
    VortexAxis axis{VortexAxis::Vertical};
    float heading{0.0F};  // Wind: degrees, 0 is +X and 90 is -Z
    float reach{0.0F};    // Attract: no pull beyond this (0 is everywhere); Draw in: fades out by this height (0 is never)
    std::uint32_t seed{0U}; // another random pick of shares, and another noise field; 0 is the default
    friend bool operator==(const MotionBehavior &, const MotionBehavior &) = default;
};

struct Object final {
    std::string mesh{"cube"}; // primitive name or the artist's model file
    Vec3 position{0.0F, 1.0F, 0.0F};
    Vec3 rotation{};  // degrees
    float size{1.0F}; // longest side, metres
    Color tint{1.0F, 1.0F, 1.0F, 1.0F};
    ObjectMotion motion{ObjectMotion::Dynamic};
    ObjectCollider collider{ObjectCollider::Hull};
    float density{1000.0F};
    float friction{0.6F};
    float restitution{0.15F};
    Vec3 velocity{};
    float release{0.0F};
    Vec3 move_to{};
    Vec3 turn_to{};
    float move_at{0.0F};
    float move_over{1.0F};
    Distribution distribution{Distribution::Single};
    std::uint32_t count{9U};
    float width{2.0F};
    float depth{2.0F};
    float spacing{1.05F};
    float variation{0.0F};
    bool fracture{false};
    std::uint32_t fragments{8U};
    float fracture_speed{4.0F};
    friend bool operator==(const Object &, const Object &) = default;
};

struct ParticleSource final {
    std::uint32_t uid{};
    SourceMode mode{SourceMode::Continuous};
    std::uint32_t count{100U};
    float rate{40.0F};
    float time{0.0F};
    friend bool operator==(const ParticleSource &, const ParticleSource &) = default;
};

struct ParticleSystem final {
    Vec3 position{0.0F, 1.0F, 0.0F};
    Range radius{0.0F, 0.2F}; // born between the two distances from the position
    std::vector<ParticleSource> sources;
    bool persistent{false}; // only bounded initial/burst sources may be persistent
    Range lifetime{3.0F, 3.0F};
    Vec3 direction{0.0F, 1.0F, 0.0F};
    Range speed{1.0F, 1.0F}; // m/s along the direction
    float spread{20.0F};
    float gravity{0.0F}; // explicit share of scene gravity, independent of name
    std::vector<MotionBehavior> motion; // in stack order
    bool collide{true};
    Color tint{1.0F, 1.0F, 1.0F, 1.0F};
    Range brightness{1.0F, 1.0F}; // above 1 a particle blooms
    Range size{0.05F, 0.05F};
    Range spin{0.0F, 0.0F}; // degrees per second, either way
    ParticleLook look{ParticleLook::Glow};
    // Size and colour over age multiply the base size and the colour. Keys
    // are sorted by age; values hold beyond the ends.
    Driver size_driver{Driver::Constant};
    std::vector<CurveKey> size_over_age{{0.0F, 1.0F}, {1.0F, 1.0F}};
    Driver tint_driver{Driver::Constant};
    std::vector<ColorKey> tint_over_age{{0.0F, {1.0F, 1.0F, 1.0F, 1.0F}}, {1.0F, {1.0F, 1.0F, 1.0F, 1.0F}}};
    friend bool operator==(const ParticleSystem &, const ParticleSystem &) = default;
};

// A placed influence on particle systems: a scene item with handles, acting
// on every particle system or on the ones it names. Its seed is the item's.
enum class ForceKind { Wind, Vortex, Attract, DrawIn, Blast };
struct Force final {
    ForceKind kind{ForceKind::Wind};
    Vec3 position{0.0F, 1.0F, 0.0F}; // Vortex axis, Attract point, Draw in base, Blast centre; Wind has none
    Range strength{1.5F, 1.5F};      // each particle's share: Wind, Vortex, Attract m/s^2; Draw in per metre
    float heading{0.0F};             // Wind: degrees, 0 is +X and 90 is -Z
    VortexAxis axis{VortexAxis::Vertical};
    float reach{0.0F};               // Attract: no pull beyond this; 0 is everywhere
    float height{1.0F};              // Draw in: fades out by this height; 0 is never
    float power{12.0F};              // Blast: speed given at the centre, m/s
    float radius{3.0F};              // Blast: nothing beyond this
    float time{0.5F};                // Blast: when it goes off, seconds
    bool all_systems{true};          // otherwise `targets` names the systems it acts on
    std::vector<std::uint32_t> targets;
    friend bool operator==(const Force &, const Force &) = default;
};

struct Group final {
    friend bool operator==(const Group &, const Group &) = default;
};

struct SceneItem final {
    std::uint32_t uid{};
    std::uint32_t parent{}; // organizational group only; 0 is the scene root
    std::string name;
    std::uint32_t seed{1U};
    bool simulate{true};
    std::variant<Object, ParticleSystem, Group, Force> data;
    friend bool operator==(const SceneItem &, const SceneItem &) = default;
};

// One rule binding an object to a particle system: particles born where the
// object hits something, or shed behind it while it moves. It lives on the
// scene, is shown from either endpoint, and is stored once.
enum class RelationKind { EmitOnHit, TrailFrom };
struct Relationship final {
    std::uint32_t uid{};
    RelationKind kind{RelationKind::EmitOnHit};
    std::uint32_t object{}; // 0 is every object
    std::uint32_t system{};
    float hardness{1.0F};  // Emit on hit: softer hits make none, m/s
    float per_hit{2.0F};   // Emit on hit: born at every hit
    float per_speed{3.0F}; // Emit on hit: more per m/s of the hit; Trail: per m/s, each second
    float spread{70.0F};   // Emit on hit: scatter around the bounce, degrees
    float min_speed{0.5F}; // Trail: slower objects shed none, m/s
    float rate{0.0F};      // Trail: per second while moving, whatever the speed
    bool pieces{false};    // Trail: only the pieces a broken object became
    std::uint32_t seed{0U};
    friend bool operator==(const Relationship &, const Relationship &) = default;
};

struct ScenePhysics final {
    float gravity{1.0F}; // g, along -Y
    float angular_damping{0.05F};
    bool allow_sleep{true};
    friend bool operator==(const ScenePhysics &, const ScenePhysics &) = default;
};

struct Scene final {
    ScenePhysics physics;
    std::vector<SceneItem> items;
    std::vector<Relationship> relationships;
    [[nodiscard]] SceneItem *item(std::uint32_t uid);
    [[nodiscard]] const SceneItem *item(std::uint32_t uid) const;
    friend bool operator==(const Scene &, const Scene &) = default;
};

struct Document;
struct Compiled;
enum class Purpose : std::uint8_t;

[[nodiscard]] Document empty_scene();
// Startup/New defaults, distinct from loading an existing scene's exact contents.
[[nodiscard]] Document new_scene();
[[nodiscard]] std::uint32_t add_floor(Document &document);
[[nodiscard]] std::uint32_t add_object(Document &document, std::string mesh = "cube");
[[nodiscard]] std::uint32_t add_particle_system(Document &document);
[[nodiscard]] std::uint32_t add_group(Document &document);
[[nodiscard]] std::uint32_t add_force(Document &document, ForceKind kind);
// Binds an object (0 for every object) to a particle system.
[[nodiscard]] std::uint32_t add_relationship(Document &document, RelationKind kind, std::uint32_t object,
                                             std::uint32_t system);
bool remove_relationship(Document &document, std::uint32_t uid);
// Whether a rule can ever fire: hits and trails come from Dynamic objects,
// so a rule on a Static or Animated one, or on every object when none is
// Dynamic, is born nothing.
[[nodiscard]] bool relationship_active(const Scene &scene, const Relationship &relationship);
[[nodiscard]] std::string_view relation_name(RelationKind kind);
inline constexpr std::size_t max_relationships = 256U;
[[nodiscard]] std::string_view force_name(ForceKind kind);
// Whether a force acts on a particle system: every one, or one it names.
[[nodiscard]] bool force_targets(const Force &force, std::uint32_t system);
// A blast throws everything; the others each lower into the systems they
// act on, at most this many per system.
inline constexpr std::size_t max_forces_per_system = 16U;
[[nodiscard]] std::uint32_t add_source(Document &document, std::uint32_t system, SourceMode mode);
// Appends a behavior to a system's Motion stack with that kind's defaults.
[[nodiscard]] std::uint32_t add_behavior(Document &document, std::uint32_t system, BehaviorKind kind);
// Moves a behavior one place up (-1) or down (+1) its stack.
bool move_behavior(Document &document, std::uint32_t system, std::uint32_t behavior, int by);
[[nodiscard]] std::string_view behavior_name(BehaviorKind kind);
inline constexpr std::size_t max_behaviors = 16U;
inline constexpr std::size_t max_curve_keys = 16U;
bool remove_item(Document &document, std::uint32_t uid);
bool group_item(Document &document, std::uint32_t uid, std::uint32_t parent);
[[nodiscard]] std::uint32_t duplicate_item(Document &document, std::uint32_t uid);
// Storage validation permits editable capability errors; playback additionally
// checks combinations and budgets. Saving an unfinished setup must stay usable.
[[nodiscard]] std::string validate_scene(const Document &document, bool for_playback = true);
[[nodiscard]] bool same_scene_motion(const Document &a, const Document &b);
// Internal compiler IR only. Never saved, edited or cached as authored state.
[[nodiscard]] Document lower_scene(const Document &document);
[[nodiscard]] Compiled compile_scene(const Document &document, Purpose purpose);
// The block of `lowered` that viewport handles drag for an item: its Object,
// or its particle system's spawn shape. 0 for a group.
[[nodiscard]] std::uint32_t handle_block(const Document &document, std::uint32_t item);
// Writes what a handle drag changed in `lowered` (lower_scene of `document`)
// back to the item that owns it, so the authored scene stays the one owner.
// Returns whether the item changed.
bool adopt_handle_edit(Document &document, const Document &lowered, std::uint32_t item);

} // namespace ajfx::studio
