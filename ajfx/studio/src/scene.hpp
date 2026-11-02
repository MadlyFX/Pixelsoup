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
    float radius{0.2F};
    std::vector<ParticleSource> sources;
    bool persistent{false}; // only bounded initial/burst sources may be persistent
    float lifetime{3.0F};
    Vec3 velocity{0.0F, 1.0F, 0.0F};
    float spread{20.0F};
    float gravity{0.0F}; // explicit share of scene gravity, independent of name
    float drag{0.0F};
    float turbulence{0.0F};
    bool collide{true};
    Color tint{1.0F, 1.0F, 1.0F, 1.0F};
    float size{0.05F};
    ParticleLook look{ParticleLook::Glow};
    friend bool operator==(const ParticleSystem &, const ParticleSystem &) = default;
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
    std::variant<Object, ParticleSystem, Group> data;
    friend bool operator==(const SceneItem &, const SceneItem &) = default;
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
[[nodiscard]] std::uint32_t add_source(Document &document, std::uint32_t system, SourceMode mode);
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
