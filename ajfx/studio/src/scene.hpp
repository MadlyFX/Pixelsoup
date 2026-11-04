#pragma once

#include <ajfx/modules.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <variant>
#include <vector>

namespace ajfx::studio {

// These are authored owners, independent of the compiler's emitter packing.
// UIDs reserve 32 internal component IDs, so lowering never allocates identities
// according to tree order. Organization is deliberately not transform parenting.
enum class ObjectMotion { Static, Dynamic, Animated };
enum class ObjectCollider { Hull, Sphere, Box };
// Placed: the Object is a kind only, with no body of its own; every body of it
// is put on the stage by hand (a Place at the start) or dropped while it plays.
enum class Distribution { Single, Grid, Pile, Placed };
enum class SourceMode { Initial, Continuous, Burst };
enum class ParticleLook { Glow, Smoke, Solid };
// Glows: the particle's colour as it is. Lit: the scene's lights and shadows
// fall on it, as a rounded puff (see ajfx::Lighting::Lit).
enum class ParticleLighting { Glows, Lit };
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
    // How the scene's light reaches it (SpriteRenderer::lighting, curvature,
    // wrap, ambient and shadowed).
    ParticleLighting lighting{ParticleLighting::Glows};
    float curvature{0.6F}; // 0 flat, 1 a full ball
    float wrap{0.25F};     // light reaching round the dark side
    float ambient{1.0F};   // its share of the sky and ground light
    bool shadowed{true};
    // Light the particles give off, in their own colour, onto the scene and
    // its smoke (SpriteRenderer's particle lights): at most `light_count`
    // carry one at once, each its strength and reach between the two.
    bool casts_light{false};
    std::uint32_t light_count{6U};
    Range light_strength{1.5F, 2.5F};
    Range light_reach{1.2F, 2.0F};
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

// A box of air (a grid volume, ajfx::Volume) that particle systems fill with
// smoke and ride: a placed scene item with a position and a size in metres,
// cut into cells of `cell` metres. Its fluid settings are in the artist's
// words; the runtime's names are beside them.
struct SmokeGrid final {
    Vec3 position{0.0F, 1.5F, 0.0F}; // the centre of the box
    Vec3 size{2.0F, 3.0F, 2.0F};     // width, height, depth, metres
    float cell{0.1F};                // metres a cell; the resolution follows (grid_resolution)
    float rises{1.0F};               // buoyancy: lift per unit of heat
    float sinks{0.0F};               // weight: fall per unit of smoke
    float swirl{0.0F};               // vorticity confinement; 0 is off
    float fades{0.2F};               // smoke and heat lost per second (dissipation)
    float calms{0.0F};               // motion lost per second (velocity dissipation)
    std::uint32_t solve{20U};        // pressure iterations
    float thickness{1.0F};           // extinction: how much light a unit of smoke takes
    Color color{1.0F, 1.0F, 1.0F, 1.0F};
    float brightness{0.8F};          // albedo: the share of that light scattered to the camera
    friend bool operator==(const SmokeGrid &, const SmokeGrid &) = default;
};

struct SceneItem final {
    std::uint32_t uid{};
    std::uint32_t parent{}; // organizational group only; 0 is the scene root
    std::string name;
    std::uint32_t seed{1U};
    bool simulate{true};
    std::variant<Object, ParticleSystem, Group, Force, SmokeGrid> data;
    friend bool operator==(const SceneItem &, const SceneItem &) = default;
};

// One rule binding a particle system to something else on the scene: born
// where an object hits something, shed behind it while it moves, shed
// behind another particle system's particles while they move, or feeding or
// riding a smoke grid. It lives on the scene, is shown from either endpoint,
// and is stored once.
enum class RelationKind { EmitOnHit, TrailFrom, TrailFromSystem, FeedSmoke, RideSmoke };
struct Relationship final {
    std::uint32_t uid{};
    RelationKind kind{RelationKind::EmitOnHit};
    // The source: an object (0 is every object), for Trail from system the
    // particle system whose particles are trailed, or for Feed smoke and
    // Ride smoke the smoke grid. Never `system`.
    std::uint32_t object{};
    std::uint32_t system{};
    float hardness{1.0F};  // Emit on hit: softer hits make none, m/s
    float per_hit{2.0F};   // Emit on hit: born at every hit
    float per_speed{3.0F}; // Emit on hit: more per m/s of the hit; Trail: per m/s, each second
    float spread{70.0F};   // Emit on hit: scatter around the bounce, degrees
    float min_speed{0.5F}; // Trails: slower objects or particles shed none, m/s
    float rate{0.0F};      // Trails: per second while moving, whatever the speed
    bool pieces{false};    // Trail: only the pieces a broken object became
    float density{1.0F};   // Feed smoke: smoke each particle adds per second
    float heat{0.5F};      // Feed smoke: heat each particle adds per second
    float push{1.0F};      // Feed smoke: the share of its own velocity each particle gives the air per second
    float strength{10.0F}; // Ride smoke: how quickly a particle takes up the air's motion, per second
    std::uint32_t seed{0U};
    friend bool operator==(const Relationship &, const Relationship &) = default;
};

struct ScenePhysics final {
    float gravity{1.0F}; // g, along -Y
    float angular_damping{0.05F};
    bool allow_sleep{true};
    // The most dropped things the solid world holds at once. A budget fixed
    // up front: a drop past it is refused and said, never made room for.
    // 0 takes no drops.
    std::uint32_t max_dropped{64U};
    friend bool operator==(const ScenePhysics &, const ScenePhysics &) = default;
};

// What the artist did while the scene played: drops and deletions, each at
// the simulation step it happened in. Saved with the scene and replayed
// exactly, since the simulation takes them as inputs (ajfx::Action) beside
// its own; changing them changes no package. See performance.hpp.
enum class ActKind : std::uint8_t { Drop, Delete };
// A body an act is done to: one of an Object's own (the ones it stands with
// at the start), by its count among them, or one a drop made, by the drop's
// serial.
struct BodyRef final {
    std::uint32_t object{}; // the Object item; 0 for a dropped body
    std::uint32_t count{};  // its place among the Object's own
    std::uint32_t drop{};   // the drop's serial; 0 for an Object's own body
    friend bool operator==(const BodyRef &, const BodyRef &) = default;
};
struct Act final {
    // Never reused, even once the act is taken away: a dropped body's
    // random variation is drawn by its serial, so removing or moving one act
    // leaves every other body as it was.
    std::uint32_t serial{};
    ActKind kind{ActKind::Drop};
    std::uint64_t tick{};      // simulation step it is applied in
    std::uint32_t object{};    // Drop: the Object whose copy is dropped
    Vec3 position{};           // Drop: where, metres in the scene
    Vec3 turn{};               // Drop: degrees, as an Object's rotation
    Vec3 velocity{};           // Drop: m/s
    // Drop at step 0 is a thing on the stage. A setup tool (Settle, Jitter)
    // moves it from where the artist put it; `home` is where that was, so
    // Reset can put it back. Putting it somewhere by hand makes that its home.
    bool touched{false};
    Vec3 home{};
    Vec3 home_turn{};
    BodyRef target{};          // Delete: the body taken away
    friend bool operator==(const Act &, const Act &) = default;
};
struct Performance final {
    std::vector<Act> acts; // by tick, then as they were done
    std::uint32_t next_serial{1U};
    friend bool operator==(const Performance &, const Performance &) = default;
};
inline constexpr std::uint32_t max_dropped_limit = 4096U;
inline constexpr std::size_t max_acts = 65536U;
struct Scene;
// Empty when the performance is well formed: acts in tick order, serials
// unique and below next_serial, drops of Objects in the scene, deletions of
// a body. Says what is wrong otherwise.
[[nodiscard]] std::string validate_performance(const Scene &scene);

struct Scene final {
    ScenePhysics physics;
    std::vector<SceneItem> items;
    std::vector<Relationship> relationships;
    Performance performance;
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
[[nodiscard]] std::uint32_t add_smoke_grid(Document &document);
// How many cells a smoke grid has along each side at its cell size: its
// size divided by the cell, rounded up, at least 2. Whether that fits the
// runtime's budget (ajfx::max_volume_size a side, ajfx::max_volume_cells in
// all) is validate_scene's to say.
[[nodiscard]] std::array<std::uint32_t, 3> grid_resolution(const SmokeGrid &grid);
[[nodiscard]] std::uint64_t grid_cells(const SmokeGrid &grid);
// The name a smoke grid's volume has in the effect, from its identity.
[[nodiscard]] std::string smoke_grid_volume(std::uint32_t item);
inline constexpr std::size_t max_smoke_grids = max_volumes;
// Binds a source to a particle system: an object (0 for every object), for
// Trail from system another particle system, or for Feed smoke and Ride
// smoke a smoke grid. 0 when the endpoints are not what the kind takes, a
// system would feed itself, or the system already feeds or rides that grid.
[[nodiscard]] std::uint32_t add_relationship(Document &document, RelationKind kind, std::uint32_t object,
                                             std::uint32_t system);
// Whether a rule of this kind gives a system particles (hits and trails),
// rather than binding the particles it has to a smoke grid.
[[nodiscard]] bool relation_births(RelationKind kind);
bool remove_relationship(Document &document, std::uint32_t uid);
// A particle system that lives only on a rule from `object` (an object, or
// for Trail from system a particle system): no source of its own, so nothing
// plays until the source hits or moves. Returns the system, named after the
// source.
[[nodiscard]] std::uint32_t add_bound_system(Document &document, RelationKind kind, std::uint32_t object);
// Whether a rule can ever fire: hits and trails come from Dynamic objects,
// so a rule on a Static or Animated one, or on every object when none is
// Dynamic, is born nothing; a trail from a system that does not simulate is
// born nothing; a smoke rule on a grid that does not simulate does nothing.
[[nodiscard]] bool relationship_active(const Scene &scene, const Relationship &relationship);
[[nodiscard]] std::string_view relation_name(RelationKind kind);
inline constexpr std::size_t max_relationships = 256U;
[[nodiscard]] std::string_view force_name(ForceKind kind);
// What an item is called until the artist names it: "Object" for a cube,
// "Ground" for the floor, the file's name for a model, the kind otherwise.
[[nodiscard]] std::string default_item_name(const SceneItem &item);
// Resolves each object's model file against the folder `from` (where a
// relative one was relative to) and writes it relative to the folder `to`,
// with forward slashes; `to` empty leaves it whole. Library shapes are
// names, not files, and stay as they are.
void rebase_models(Document &document, const std::filesystem::path &from, const std::filesystem::path &to);
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
// `live` names a particle system whose Motion controls (live_parameters)
// are lowered as published parameters, for a preview; 0 for none.
[[nodiscard]] Document lower_scene(const Document &document, std::uint32_t live = 0U);
[[nodiscard]] Compiled compile_scene(const Document &document, Purpose purpose, std::uint32_t live = 0U);

// A preview compiled with a live system reads that system's Motion values --
// speed, spread, direction, radius, gravity share and each behavior's
// strength -- from its placement's parameters rather than from the package.
// Changing one is then a new parameter value, which the renderer takes the
// next frame, instead of a new package. By the parameter's name, in the
// units the runtime takes.
struct LiveParameter final {
    std::string name;
    ParameterValue value{};
    std::uint32_t components{1U};
};
[[nodiscard]] std::vector<LiveParameter> live_parameters(const Document &document, std::uint32_t system);
// Whether `a` and `b` are the same scene but for `system`'s live values: a
// preview built of one with `system` live shows the other through
// live_parameters alone.
[[nodiscard]] bool same_but_live(const Document &a, const Document &b, std::uint32_t system);
// The scene as `system` alone needs it to play: every other particle system
// is left out unless the system's particles are born of it (a trail rule, or
// one feeding that). Objects, forces and grids stay. The system's own
// particles do exactly what they do in the whole scene, so its paths can be
// traced from this far sooner.
[[nodiscard]] Document focus_scene(const Document &document, std::uint32_t system);
// The block of `lowered` that viewport handles drag for an item: its Object,
// or its particle system's spawn shape. 0 for a group.
[[nodiscard]] std::uint32_t handle_block(const Document &document, std::uint32_t item);
// Writes what a handle drag changed in `lowered` (lower_scene of `document`)
// back to the item that owns it, so the authored scene stays the one owner.
// Returns whether the item changed.
bool adopt_handle_edit(Document &document, const Document &lowered, std::uint32_t item);

} // namespace ajfx::studio
