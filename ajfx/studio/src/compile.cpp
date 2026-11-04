#include "compile.hpp"

#include "library.hpp"
#include "user_assets.hpp"

#include <ajfx/fracture.hpp>
#include <ajfx/gpu/kernels.hpp>
#include <ajfx/random.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
#include <variant>

// The scene compiler: a Scene straight to an ajfx::Effect.
//
// Every simulating particle system is an emitter of its own; every
// simulating Object is part of the scene's one solid world, a rigid physics
// emitter; every simulating smoke grid is a volume of the effect, and every
// simulating blast a blast. Module ids and seeds are keyed by the item's
// reserved identities (item uid + a component number, scene.cpp), so adding,
// moving or editing one item leaves every other's random streams where they
// were, and the effect is the same whatever order the tree lists items in.
namespace ajfx::studio {
namespace {

constexpr float earth_gravity = 9.81F;
constexpr float pi = 3.14159265F;
constexpr ModuleId seeded_bit = 0x80000000U;

// Each identity owns sixteen module ids.
constexpr ModuleId module_id(Uid uid, std::uint32_t slot) noexcept {
    return uid * 16U + slot;
}

// An item's identities, by what each keys (scene.cpp reserves 32 a item).
enum Part : Uid {
    PartWorld = 1U,    // an Object's material and world settings; a blast; a smoke grid
    PartObject = 2U,   // an Object's bodies
    PartColor = 3U,    // a system's colour; a Static object as a collider
    PartLook = 4U,     // how it is drawn
    PartSize = 5U,     // a system's size
    PartLaunch = 6U,   // how it leaves
    PartShape = 7U,    // where a system's particles are born
    PartGravity = 8U,  // a system's share of gravity
    PartLight = 9U,    // the light a system's particles give
    PartBreak = 11U,   // how an Object comes apart
    PartThrough = 12U, // a system that passes through solid things
    PartSizeAge = 13U, // a system's size over its age
    PartTintAge = 14U, // a system's colour over its age
    PartSpin = 15U,    // a system's spin
    PartForces = 16U,  // the forces acting on a system, one identity each
};

// Seeds and counts are carried through float by the scene's values, so a
// seed past 2^24 keys as its nearest float does.
std::uint32_t through_float(std::uint32_t value) {
    return static_cast<std::uint32_t>(static_cast<float>(value));
}

Vec3 normalized(Vec3 v, Vec3 fallback) {
    const float length = std::sqrt(dot(v, v));
    return length > 1.0e-6F ? v * (1.0F / length) : fallback;
}

std::string format_number(float value, int decimals) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.*f", std::max(decimals, 0), static_cast<double>(value));
    std::string text = buffer;
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text == "-0" ? std::string{"0"} : text;
}

bool library_shape(const std::string& mesh) {
    return mesh == "cube" || mesh == "slab" || mesh == "rock" || mesh == "shard" || mesh == "floor";
}

// Where a module goes within its stage. Lower first; ties keep the order
// they were placed in.
enum InitSlot : int { InitLifetime, InitShape, InitVelocity, InitColorSlot, InitSizeSlot, InitRotationSlot, InitSpinSlot };
enum ForceSlot : int {
    ForceGravity = 0,
    ForceWind = 2,
    ForceSwirl = 3,
    ForceAttract = 4,
    ForceDrawIn = 5,
    ForceTurbulence = 6,
    ForceShockwave = 7,
    ForceRaw = 9,
    ForceDrag = 10,
};
enum AfterSlot : int { AfterRigid = 0, AfterCollision = 2, AfterRaw = 3 };

struct Placed final {
    int slot{};
    // The identity it came from, which orders the spawn stack.
    Uid origin{};
    std::variant<SpawnModule, InitModule, UpdateModule> module;
};

// One emitter's modules as they are placed, and the ids they are given.
class Modules final {
public:
    explicit Modules(Uid owner) : owner_{owner} {}

    // A module id. Random streams are keyed by id, so a seed picks another
    // id to deal its particles other values. Seeded ids have the top bit
    // set, apart from every plain uid * 16 + slot, and the rare clash of two
    // seeded ones moves on to the next free id.
    ModuleId id(Uid uid, std::uint32_t slot, std::uint32_t seed = 0U) {
        if (seed == 0U) {
            return module_id(uid, slot);
        }
        return claim(seeded_bit | (hash_mix(module_id(uid, slot), through_float(seed)) & ~seeded_bit));
    }
    // An id the emitter keys by its owner and something else it answers:
    // a blast, or the solid world it meets.
    ModuleId keyed(std::uint32_t slot, Uid key) {
        return claim(seeded_bit | (hash_mix(module_id(owner_, slot), key) & ~seeded_bit));
    }

    template <class M>
    void spawn(Uid origin, M module) {
        spawn_.push_back(Placed{0, origin, SpawnModule{std::move(module)}});
    }
    template <class M>
    void init(int slot, Uid origin, M module) {
        init_.push_back(Placed{slot, origin, InitModule{std::move(module)}});
    }
    template <class M>
    void force(int slot, Uid origin, M module) {
        forces_.push_back(Placed{slot, origin, UpdateModule{std::move(module)}});
    }
    template <class M>
    void after(int slot, Uid origin, M module) {
        after_.push_back(Placed{slot, origin, UpdateModule{std::move(module)}});
    }
    template <class M>
    void life(Uid origin, M module) {
        life_.push_back(Placed{0, origin, UpdateModule{std::move(module)}});
    }

    // The emitter's stacks: spawn modules in the order of the identities
    // they came from (each draws its random values by its place in the
    // stack), then init and update by slot.
    void fill(Emitter& emitter) {
        const auto by_slot = [](const Placed& a, const Placed& b) { return a.slot < b.slot; };
        std::stable_sort(init_.begin(), init_.end(), by_slot);
        std::stable_sort(forces_.begin(), forces_.end(), by_slot);
        std::stable_sort(after_.begin(), after_.end(), by_slot);
        std::stable_sort(spawn_.begin(), spawn_.end(),
                         [](const Placed& a, const Placed& b) { return a.origin < b.origin; });
        for (Placed& placed : spawn_) {
            emitter.spawn.push_back(std::get<SpawnModule>(std::move(placed.module)));
        }
        for (Placed& placed : init_) {
            emitter.init.push_back(std::get<InitModule>(std::move(placed.module)));
        }
        for (std::vector<Placed>* group : {&forces_, &after_, &life_}) {
            for (Placed& placed : *group) {
                emitter.update.push_back(std::get<UpdateModule>(std::move(placed.module)));
            }
        }
    }

private:
    ModuleId claim(ModuleId id) {
        while (!seeded_.insert(id).second) {
            id = seeded_bit | ((id + 1U) & ~seeded_bit);
        }
        return id;
    }

    Uid owner_;
    std::set<ModuleId> seeded_;
    std::vector<Placed> spawn_;
    std::vector<Placed> init_;
    std::vector<Placed> forces_;
    std::vector<Placed> after_;
    std::vector<Placed> life_;
};

// The parameters a live system publishes (live_parameters), declared as
// the effect's.
class Parameters final {
public:
    explicit Parameters(std::vector<ParameterDesc>& all) : all_{all} {}

    Binding declare(const std::string& name, ParameterType type, ParameterValue value, float low, float high) {
        if (low > high) {
            std::swap(low, high);
        }
        const std::size_t components = type == ParameterType::Float ? 1U : type == ParameterType::Vec3 ? 3U : 4U;
        for (std::size_t i = 0U; i < components; ++i) {
            value[i] = std::clamp(value[i], low, high);
        }
        if (std::find(names_.begin(), names_.end(), name) == names_.end()) {
            names_.push_back(name);
        }
        for (ParameterDesc& existing : all_) {
            if (existing.name == name) {
                existing.min = std::min(existing.min, low);
                existing.max = std::max(existing.max, high);
                return bind(name);
            }
        }
        all_.push_back(ParameterDesc{name, type, value, low, high, ParameterTiming::Bake});
        return bind(name);
    }
    // Both ends of a range as `name_min` and `name_max`, or the constants
    // where nothing is published.
    std::pair<FloatInput, FloatInput> range(const std::string* name, float low, float high, float from, float to) {
        if (name == nullptr) {
            return {low, high};
        }
        return {declare(*name + "_min", ParameterType::Float, to_value(low), from, to),
                declare(*name + "_max", ParameterType::Float, to_value(high), from, to)};
    }
    [[nodiscard]] const std::vector<std::string>& names() const noexcept { return names_; }

private:
    std::vector<ParameterDesc>& all_;
    std::vector<std::string> names_;
};

std::string live_name(Uid system, const char* what) {
    return "live" + std::to_string(system) + "_" + what;
}

// A behavior's strength range when it is live: the inspector's, so a value
// it allows is never clamped.
std::pair<float, float> behavior_limits(BehaviorKind kind) {
    switch (kind) {
    case BehaviorKind::Drag:
    case BehaviorKind::Turbulence: return {0.0F, 100.0F};
    case BehaviorKind::Vortex:
    case BehaviorKind::Attract: return {-1000.0F, 1000.0F};
    case BehaviorKind::Wind:
    case BehaviorKind::DrawIn: return {0.0F, 1000.0F};
    }
    return {0.0F, 1000.0F};
}

// The scene's blasts and smoke grids, which every emitter sees.
struct SceneBlast final {
    Uid identity{};
    Blast blast;
};

struct Shared final {
    const Document& document;
    const Scene& scene;
    float s{1.0F};
    // In identity order.
    std::vector<const SceneItem*> ordered;
    std::vector<SceneBlast> blasts;
    std::vector<Volume> volumes;
    std::vector<ParameterDesc> parameters;
};

std::string emitter_name(Uid item) {
    return "item_" + std::to_string(item);
}

// ------------------------------------------------------------ the solid world

// One Object, as it stands in the solid world.
struct ObjectPart final {
    const SceneItem* item{};
    const Object* object{};
    std::string error;
    // Its own bodies (a SpawnPile), or none for a thing that cannot move.
    std::optional<SpawnPile> pile;
    std::optional<RigidBodyKind> kind;
    std::optional<RigidProp> prop;
    std::optional<FractureCut> cut;
    std::uint32_t count{0U};
    std::uint64_t extra{0U};
    Lifetime lifetime;
    Velocity velocity;
    std::optional<InitSize> size;
    std::optional<Gravity> gravity;
    std::vector<Shockwave> shockwaves;
    std::vector<AssetRequest> requests;
    SpriteRenderer renderer;
};

// How a solid thing is drawn: as its own model, with the look's defaults.
SpriteRenderer solid_renderer() {
    SpriteRenderer r;
    r.blend = BlendMode::Alpha;
    r.sort = false;
    r.soft_distance = 0.0F;
    r.geometry = Geometry::Mesh;
    r.mesh = "slab";
    r.texture.clear();
    r.facing = Facing::Camera;
    r.stretch = 0.0F;
    r.flipbook_columns = r.flipbook_rows = 1U;
    r.flipbook_blend = false;
    r.lighting = Lighting::Unlit;
    r.lightmap.clear();
    r.curvature = 0.6F;
    r.wrap = 0.25F;
    r.ambient = 1.0F;
    r.shadowed = true;
    r.tint = ColorInput{Color{1.0F, 1.0F, 1.0F, 1.0F}};
    r.brightness = FloatInput{1.0F};
    r.opacity = FloatInput{1.0F};
    r.size_scale = FloatInput{1.0F};
    return r;
}

// A particle system's renderer, from its look.
SpriteRenderer system_renderer(const ParticleSystem& system, float s) {
    SpriteRenderer r;
    const ParticleLook look = system.look;
    r.blend = look == ParticleLook::Glow ? BlendMode::Additive : BlendMode::Alpha;
    r.sort = look != ParticleLook::Glow;
    r.soft_distance = 0.15F * s;
    const LibraryTexture* texture = library_texture("dot");
    const bool flipbook = texture != nullptr && texture->columns * texture->rows > 1U;
    r.geometry = Geometry::Sprites;
    r.mesh.clear();
    r.texture = texture != nullptr ? std::string{texture->entry} : std::string{};
    r.flipbook_columns = texture != nullptr ? texture->columns : 1U;
    r.flipbook_rows = texture != nullptr ? texture->rows : 1U;
    r.flipbook_blend = texture != nullptr && flipbook;
    r.facing = Facing::Camera;
    r.stretch = 0.0F;
    r.lighting = system.lighting == ParticleLighting::Lit ? Lighting::Lit : Lighting::Unlit;
    r.lightmap.clear();
    r.curvature = std::clamp(system.curvature, 0.0F, 1.0F);
    r.wrap = std::clamp(system.wrap, 0.0F, 1.0F);
    r.ambient = std::max(system.ambient, 0.0F);
    r.shadowed = system.shadowed;
    r.tint = ColorInput{Color{system.tint.r, system.tint.g, system.tint.b, 1.0F}};
    r.brightness = FloatInput{1.0F};
    r.opacity = FloatInput{1.0F};
    r.size_scale = FloatInput{1.0F};
    if (system.casts_light) {
        // In the particle's own colour, fading in and out with its life, so
        // a flame's light goes as the flame does.
        r.light_count = static_cast<std::uint32_t>(std::lround(static_cast<float>(system.light_count)));
        const Range intensity = system.light_strength;
        const Range reach = system.light_reach;
        r.light_intensity_min = std::max(std::min(intensity.min, intensity.max), 0.0F);
        r.light_intensity_max = std::max(std::max(intensity.min, intensity.max), 0.0F);
        r.light_range_min = std::max(std::min(reach.min, reach.max), 0.01F) * s;
        r.light_range_max = std::max(std::max(reach.min, reach.max), 0.01F) * s;
        r.light_curve = {{0.0F, 0.0F}, {0.15F, 1.0F}, {0.6F, 1.0F}, {1.0F, 0.0F}};
        r.light_own_color = false;
        const Color unused{1.0F, 0.6F, 0.3F, 1.0F};
        r.light_colors = {{0.0F, unused}, {1.0F, unused}};
    }
    return r;
}

std::string request_entry(AssetRequest::Kind kind, const std::string& path, float scale, std::uint32_t cells,
                          ColliderSolid solid, bool fit, Vec3 turn) {
    std::string options;
    if (kind == AssetRequest::Kind::Field) {
        options = format_number(scale, 6) + "/" + std::to_string(cells) + "/" + std::to_string(static_cast<int>(solid));
        if (fit || turn != Vec3{}) {
            options += "/" + std::string{fit ? "fit" : "as-is"} + "/" + format_number(turn.x, 4) + "," +
                       format_number(turn.y, 4) + "," + format_number(turn.z, 4);
        }
    }
    const std::string_view what = kind == AssetRequest::Kind::Mesh ? "mesh" : "field";
    return user_asset_name(what, path, options);
}

ObjectPart object_part(const Shared& shared, const SceneItem& item, const Object& object) {
    ObjectPart out;
    out.item = &item;
    out.object = &object;
    const float s = shared.s;
    const Uid uid = item.uid;
    Modules ids{uid};
    out.renderer = solid_renderer();

    // Its model: a library shape by name, or the artist's file.
    std::string model = object.mesh;
    if (!library_shape(object.mesh)) {
        model = object.mesh.empty() ? std::string{"cube"}
                                    : request_entry(AssetRequest::Kind::Mesh, object.mesh, 1.0F, 0U,
                                                    ColliderSolid::Auto, false, Vec3{});
        if (!object.mesh.empty()) {
            out.requests.push_back(AssetRequest{.kind = AssetRequest::Kind::Mesh, .entry = model, .path = object.mesh, .item = uid});
        }
    }
    const float size = std::max(object.size * s, 1.0e-4F);
    const bool placed = object.distribution == Distribution::Placed;
    out.count = placed ? 0U
                : object.distribution == Distribution::Single
                    ? 1U
                    : static_cast<std::uint32_t>(std::max(1L, std::lround(static_cast<float>(object.count))));

    if (object.motion == ObjectMotion::Dynamic) {
        RigidBodyKind kind;
        kind.shape = object.collider == ObjectCollider::Sphere ? RigidShape::Sphere
                     : object.collider == ObjectCollider::Box  ? RigidShape::Box
                                                               : RigidShape::Hull;
        if (kind.shape == RigidShape::Hull) {
            kind.mesh = model;
        }
        kind.draw = model;
        kind.density = object.density;
        kind.friction = object.friction;
        kind.restitution = object.restitution;
        // Its size is the kind's scale against the size every body is born
        // at, which a solid world states outright.
        kind.scale = size / 0.1F;
        out.kind = std::move(kind);
        out.pile = SpawnPile{.id = ids.id(uid + PartObject, 1U, item.seed),
                             .count = out.count,
                             .shape = object.distribution == Distribution::Single ? PileShape::Single
                                      : object.distribution == Distribution::Grid ? PileShape::Box
                                                                                  : PileShape::Heap,
                             .center = object.position * s,
                             .slope = 55.0F,
                             .width = object.width * s,
                             .depth = object.depth * s,
                             .spacing = object.spacing,
                             .jitter = object.distribution == Distribution::Single ? 0.0F : object.variation,
                             .release = object.release,
                             .body = 1U};
    } else {
        if (placed) {
            out.error = "A thing placed by hand has to be knocked about: let it move, or give it a place of its own";
            return out;
        }
        if (out.count > 1U) {
            out.error = "Only one can stand still or be moved: set how many back to just one, or let it be "
                        "knocked about";
            return out;
        }
        if (object.motion == ObjectMotion::Animated && object.collider == ObjectCollider::Hull) {
            out.error = "An object that is moved cannot collide as its own shape: have it collide as a ball or a "
                        "box instead";
            return out;
        }
        RigidProp prop;
        prop.motion = object.motion == ObjectMotion::Animated ? PropMotion::Driven : PropMotion::Fixed;
        prop.shape = object.collider == ObjectCollider::Hull  ? PropShape::Hull
                     : object.collider == ObjectCollider::Box ? PropShape::Box
                                                              : PropShape::Sphere;
        prop.mesh = model;
        prop.position = object.position * s;
        prop.turn = object.rotation;
        prop.size = size;
        prop.half_size = Vec3{object.size, object.size, object.size} * (0.5F * s);
        prop.radius = object.size * 0.5F * s;
        prop.height = 0.8F * s;
        prop.wall = 0.05F * s;
        prop.segments = 12U;
        prop.density = object.density;
        prop.friction = object.friction;
        prop.restitution = object.restitution;
        prop.to_position = object.move_to * s;
        prop.to_turn = object.turn_to;
        prop.move_from = object.move_at;
        prop.move_over = object.move_over;
        prop.draw_tint = object.tint;
        out.prop = std::move(prop);
    }
    if (object.fracture) {
        if (out.prop) {
            out.error = "Only a piece the world moves can come apart: let this object be knocked about";
            return out;
        }
        const std::uint32_t cells =
            static_cast<std::uint32_t>(std::max(1L, std::lround(static_cast<float>(object.fragments))));
        out.cut = FractureCut{.item = uid,
                              .model = model,
                              .body = 1U,
                              .id = ids.id(uid + PartBreak, 1U),
                              .cells = cells,
                              .seed = static_cast<std::uint32_t>(std::lround(static_cast<float>(item.seed))),
                              .spread = 0.0F,
                              .threshold = object.fracture_speed * s,
                              // Thrown by a blast as hard as a hit that breaks it.
                              .blast_threshold = std::max(object.fracture_speed, 0.0F) * s,
                              .scatter = 1.0F * s,
                              .spin = 180.0F};
        out.extra = static_cast<std::uint64_t>(out.count) * (cells - 1U);
        if (1U + cells > max_rigid_body_kinds) {
            out.error = "It needs " + std::to_string(1U + cells) + " kinds of body, and a world holds " +
                        std::to_string(max_rigid_body_kinds) + ": cut fewer pieces";
            return out;
        }
    }
    const float speed = std::sqrt(dot(object.velocity, object.velocity));
    out.velocity = Velocity{.id = ids.id(uid + PartLaunch, 1U)};
    out.velocity.direction = normalized(object.velocity, Vec3{0.0F, 1.0F, 0.0F});
    out.velocity.speed_min = FloatInput{speed * s};
    out.velocity.speed_max = FloatInput{speed * s};
    out.velocity.spread_min = FloatInput{0.0F};
    out.velocity.spread = FloatInput{0.0F};
    // Things stand until they break or the world lets them leave.
    const float forever = std::numeric_limits<float>::max();
    out.lifetime = Lifetime{.id = module_id(uid, 1U), .min = forever, .max = forever};
    if (out.kind) {
        out.size = InitSize{.id = module_id(uid, 5U), .min = 0.1F, .max = 0.1F};
    }
    if (shared.scene.physics.gravity > 0.0F) {
        out.gravity = Gravity{.id = module_id(uid, 6U),
                              .acceleration = Vec3{0.0F, -earth_gravity * shared.scene.physics.gravity * s, 0.0F}};
    }
    for (const SceneBlast& blast : shared.blasts) {
        Shockwave wave{.id = ids.keyed(10U, blast.identity), .blast = blast.blast.name};
        wave.scale_min = FloatInput{1.0F};
        wave.scale_max = FloatInput{1.0F};
        out.shockwaves.push_back(std::move(wave));
    }
    return out;
}

// What the world is built of, and what each Object is in it.
struct BuiltWorld final {
    std::optional<Emitter> emitter;
    std::uint64_t extra{0U};
    std::vector<FractureCut> cuts;
    // By object, in identity order: its body kinds in the world (empty for
    // a world of one object).
    std::vector<Scope> scopes;
    std::string error;
};

std::uint64_t rounded_capacity(std::uint64_t bound) {
    const std::uint64_t rounded = std::max<std::uint64_t>(64U, (bound + 63U) / 64U * 64U);
    return std::min<std::uint64_t>(rounded, max_emitter_capacity);
}

// The scene's Objects as one physics world, its first Object's: its name,
// seed, material and world settings. Every Object's bodies are a kind of
// it, so loose things of one Object knock into another's; its looks and
// launch are scoped to its own kinds.
BuiltWorld build_world(const Shared& shared, const std::vector<ObjectPart>& parts) {
    BuiltWorld out;
    if (parts.empty()) {
        return out;
    }
    const ObjectPart& base = parts.front();
    const bool alone = parts.size() == 1U;
    const ScenePhysics& physics = shared.scene.physics;
    RigidPhysics world{.id = module_id(base.item->uid + PartWorld, 1U),
                       .shape = RigidShape::Auto,
                       .density = base.object->density,
                       .friction = base.object->friction,
                       .restitution = base.object->restitution,
                       .angular_damping = physics.angular_damping,
                       .allow_sleep = physics.allow_sleep};

    // Each Object's kinds by the world's numbers. Every Object after the
    // first also holds a kind of its own material that nothing is born as,
    // so the world's kind numbers stay as caches know them.
    std::vector<std::vector<std::uint32_t>> kinds(parts.size());
    for (std::size_t b = 0U; b < parts.size(); ++b) {
        const ObjectPart& part = parts[b];
        if (b == 0U) {
            kinds[b].push_back(0U);
        } else {
            RigidBodyKind own{.shape = RigidShape::Hull,
                              .mesh = "slab",
                              .draw = "slab",
                              .scale = 1.0F,
                              .density = part.object->density,
                              .friction = part.object->friction,
                              .restitution = part.object->restitution};
            own.draw_tint = part.object->tint;
            world.bodies.push_back(std::move(own));
            kinds[b].push_back(static_cast<std::uint32_t>(world.bodies.size()));
        }
        if (part.kind) {
            RigidBodyKind kind = *part.kind;
            kind.draw_tint = part.object->tint;
            world.bodies.push_back(std::move(kind));
            kinds[b].push_back(static_cast<std::uint32_t>(world.bodies.size()));
        }
        if (part.prop) {
            world.props.push_back(*part.prop);
        }
    }
    std::size_t wanted = world.bodies.size() + 1U;
    for (const ObjectPart& part : parts) {
        if (part.cut) {
            wanted += part.cut->cells;
        }
    }
    if (wanted > max_rigid_body_kinds + 1U) {
        out.error = "the scene's things need " + std::to_string(wanted - 1U) + " kinds of body, and their one world " +
                    "holds " + std::to_string(max_rigid_body_kinds);
        return out;
    }
    if (world.props.size() > max_rigid_props) {
        out.error = "the scene holds " + std::to_string(world.props.size()) + " standing things, and its world " +
                    std::to_string(max_rigid_props);
        return out;
    }

    Emitter emitter;
    emitter.name = emitter_name(base.item->uid);
    emitter.seed = hash_mix(base.item->uid, 1U);
    emitter.capacity = max_emitter_capacity;
    emitter.renderer = base.renderer;
    // The spawn stack, in identity order, each Object's pile bearing its
    // kind.
    for (std::size_t b = 0U; b < parts.size(); ++b) {
        if (parts[b].pile) {
            SpawnPile pile = *parts[b].pile;
            pile.initial_velocity = parts[b].object->velocity * shared.s;
            pile.turn = parts[b].object->rotation;
            pile.body = kinds[b].back();
            emitter.spawn.emplace_back(std::move(pile));
        }
    }
    if (emitter.spawn.size() > max_spawn_modules) {
        out.error = "the scene's things are made in " + std::to_string(emitter.spawn.size()) +
                    " ways, and their one world takes " + std::to_string(max_spawn_modules);
        return out;
    }
    for (std::size_t b = 0U; b < parts.size(); ++b) {
        out.scopes.push_back(alone ? Scope{} : Scope{.sources = {}, .kinds = kinds[b]});
        const Scope only = out.scopes.back();
        const ObjectPart& part = parts[b];
        Lifetime lifetime = part.lifetime;
        lifetime.only = only;
        emitter.init.emplace_back(std::move(lifetime));
        Velocity velocity = part.velocity;
        velocity.only = only;
        emitter.init.emplace_back(std::move(velocity));
        if (part.size) {
            InitSize size = *part.size;
            size.only = only;
            emitter.init.emplace_back(std::move(size));
        }
    }
    // One gravity and one answer to each blast: the first Object's.
    if (base.gravity) {
        emitter.update.emplace_back(*base.gravity);
    }
    for (const Shockwave& wave : base.shockwaves) {
        emitter.update.emplace_back(wave);
    }
    emitter.update.emplace_back(std::move(world));

    for (std::size_t b = 0U; b < parts.size(); ++b) {
        out.extra += parts[b].extra;
        if (parts[b].cut) {
            FractureCut cut = *parts[b].cut;
            cut.emitter = emitter.name;
            cut.body = kinds[b].back();
            out.cuts.push_back(std::move(cut));
        }
    }
    out.emitter = std::move(emitter);
    return out;
}

// ------------------------------------------------------------ particle systems

struct SystemBuild final {
    std::optional<Emitter> emitter;
    std::vector<std::string> parameters;
    std::vector<AssetRequest> requests;
    std::string error;
};

// What a system's particles do where they meet a solid thing: what the
// first Static floor says, else slide.
struct Meeting final {
    float restitution{0.0F};
    float friction{0.2F};
};

// The forces that act on a system, in identity order.
std::vector<const SceneItem*> forces_on(const Shared& shared, Uid system) {
    std::vector<const SceneItem*> out;
    for (const SceneItem* item : shared.ordered) {
        const auto* force = std::get_if<Force>(&item->data);
        if (force && force->kind != ForceKind::Blast && item->simulate && force_targets(*force, system)) {
            out.push_back(item);
        }
    }
    return out;
}

SystemBuild build_system(Shared& shared, const SceneItem& item, const ParticleSystem& system, bool live,
                         const SolidWorld& solids) {
    SystemBuild out;
    const float s = shared.s;
    const Uid uid = item.uid;
    Modules m{uid};
    Parameters published{shared.parameters};
    const auto name = [&](const char* what) -> std::optional<std::string> {
        return live ? std::optional<std::string>{live_name(uid, what)} : std::nullopt;
    };
    const auto ptr = [](const std::optional<std::string>& n) { return n ? &*n : nullptr; };

    // Births: each source, in identity order.
    std::vector<ParticleSource> sources = system.sources;
    std::sort(sources.begin(), sources.end(),
              [](const ParticleSource& a, const ParticleSource& b) { return a.uid < b.uid; });
    for (const ParticleSource& source : sources) {
        if (source.mode == SourceMode::Continuous) {
            m.spawn(source.uid, SpawnRate{.id = m.id(source.uid, 1U), .rate = FloatInput{source.rate}, .body = 0U});
        } else {
            m.spawn(source.uid,
                    SpawnBurst{.id = m.id(source.uid, 1U),
                               .time = source.mode == SourceMode::Initial ? 0.0F : source.time,
                               .count = static_cast<std::uint32_t>(std::lround(static_cast<float>(source.count))),
                               .interval = 0.0F,
                               .cycles = 1U,
                               .body = 0U});
        }
    }

    // Where they are born: a ball round its position.
    {
        ShapeLocation location{.id = m.id(uid + PartShape, 1U)};
        location.center = system.position * s;
        const std::optional<std::string> radius = name("radius");
        std::tie(location.radius_min, location.radius_max) =
            published.range(ptr(radius), system.radius.min * s, system.radius.max * s, 0.0F * s, 1000.0F * s);
        location.shape = Shape::Sphere;
        location.surface_only = false;
        location.size = Vec3{0.4F, 0.4F, 0.4F} * s;
        m.init(InitShape, uid + PartShape, location);
    }

    // How they leave.
    {
        Velocity velocity{.id = m.id(uid + PartLaunch, 1U)};
        velocity.direction = normalized(system.direction, Vec3{0.0F, 1.0F, 0.0F});
        if (const std::optional<std::string> direction = name("direction")) {
            velocity.direction =
                published.declare(*direction, ParameterType::Vec3, to_value(velocity.direction.value), -1.0F, 1.0F);
        }
        const std::optional<std::string> speed = name("speed");
        std::tie(velocity.speed_min, velocity.speed_max) =
            published.range(ptr(speed), system.speed.min * s, system.speed.max * s, 0.0F * s, 10000.0F * s);
        const std::optional<std::string> spread = name("spread");
        std::tie(velocity.spread_min, velocity.spread) =
            published.range(ptr(spread), 0.0F, std::clamp(system.spread, 0.0F, 180.0F), 0.0F, 180.0F);
        m.init(InitVelocity, uid + PartLaunch, velocity);
    }

    // Its own share of the scene's gravity.
    {
        Gravity gravity{.id = m.id(uid + PartGravity, 1U),
                        .acceleration = Vec3{0.0F, -earth_gravity * s * shared.scene.physics.gravity, 0.0F}};
        const std::optional<std::string> share = name("gravity");
        std::tie(gravity.scale_min, gravity.scale_max) =
            published.range(ptr(share), system.gravity, system.gravity, 0.0F, 10.0F);
        m.force(ForceGravity, uid + PartGravity, gravity);
    }

    // One force: a Motion behavior of the system's, or a Force item acting
    // on it. `strength` is each particle's share; `live` names it for a
    // live preview.
    struct Push final {
        Uid identity{};
        std::uint32_t seed{0U};
        Range strength{};
        const std::string* live{nullptr};
        std::pair<float, float> limits{};
    };
    const auto strength = [&](auto& force, const Push& push) {
        std::tie(force.scale_min, force.scale_max) =
            published.range(push.live, push.strength.min, push.strength.max, push.limits.first, push.limits.second);
    };
    const auto wind = [&](const Push& push, float heading_degrees) {
        const float heading = heading_degrees * pi / 180.0F;
        Gravity force{.id = m.id(push.identity, 1U, push.seed),
                      .acceleration = Vec3{std::cos(heading) * s, 0.0F, -std::sin(heading) * s}};
        strength(force, push);
        m.force(ForceWind, push.identity, force);
    };
    const auto swirl = [&](const Push& push, VortexAxis axis, Vec3 center) {
        Vortex force{.id = m.id(push.identity, 1U, push.seed),
                     .center = center * s,
                     .axis = axis == VortexAxis::Horizontal ? Vec3{1.0F, 0.0F, 0.0F} : Vec3{0.0F, 1.0F, 0.0F},
                     .strength = s};
        strength(force, push);
        m.force(ForceSwirl, push.identity, force);
    };
    const auto attract = [&](const Push& push, Vec3 point, float reach) {
        Attractor force{.id = m.id(push.identity, 1U, push.seed),
                        .position = point * s,
                        .strength = s,
                        .radius = FloatInput{reach * s}};
        strength(force, push);
        m.force(ForceAttract, push.identity, force);
    };
    const auto draw_in = [&](const Push& push, Vec3 center, float height) {
        Converge force{.id = m.id(push.identity, 1U, push.seed), .center = center * s, .strength = 1.0F, .height = height * s};
        strength(force, push);
        m.force(ForceDrawIn, push.identity, force);
    };

    // The Motion stack, in its order.
    std::vector<std::string> behavior_names;
    behavior_names.reserve(system.motion.size());
    for (const MotionBehavior& b : system.motion) {
        behavior_names.push_back(live_name(uid, "m") + std::to_string(b.uid));
    }
    for (std::size_t k = 0U; k < system.motion.size(); ++k) {
        const MotionBehavior& b = system.motion[k];
        if (!b.enabled) {
            continue;
        }
        const Push push{.identity = b.uid,
                        .seed = b.seed,
                        .strength = b.strength,
                        .live = live ? &behavior_names[k] : nullptr,
                        .limits = behavior_limits(b.kind)};
        switch (b.kind) {
        case BehaviorKind::Drag: {
            Drag drag{.id = m.id(b.uid, 1U, b.seed), .coefficient = FloatInput{1.0F}};
            strength(drag, push);
            m.force(ForceDrag, b.uid, drag);
            break;
        }
        case BehaviorKind::Turbulence: {
            const float size = std::max(b.size, 0.01F) * s;
            CurlNoise noise{.id = m.id(b.uid, 1U, b.seed),
                            .strength = s,
                            .frequency = 1.0F / size,
                            .evolution = FloatInput{b.churn * s},
                            .seed = hash_mix(b.uid, through_float(b.seed)),
                            .drift = Vec3{0.0F, 0.0F * s, 0.0F},
                            .over_life = {{0.0F, 1.0F}, {1.0F, 1.0F}}};
            strength(noise, push);
            m.force(ForceTurbulence, b.uid, noise);
            break;
        }
        case BehaviorKind::Vortex: swirl(push, b.axis, b.center); break;
        case BehaviorKind::Wind: wind(push, b.heading); break;
        case BehaviorKind::Attract: attract(push, b.center, b.reach); break;
        case BehaviorKind::DrawIn: draw_in(push, b.center, b.reach); break;
        }
    }

    // How it is drawn, and how it starts.
    SpriteRenderer renderer = system_renderer(system, s);
    {
        const float brightness = system.brightness.max;
        const Color start{brightness, brightness, brightness, 1.0F};
        m.init(InitColorSlot, uid + PartColor,
               InitColor{.id = m.id(uid + PartColor, 1U),
                         .a = ColorInput{start},
                         .b = ColorInput{start},
                         .brightness_min =
                             brightness > 0.0F ? std::clamp(system.brightness.min / brightness, 0.0F, 1.0F) : 1.0F,
                         .brightness_max = 1.0F});
    }
    m.init(InitSizeSlot, uid + PartSize,
           InitSize{.id = m.id(uid + PartSize, 1U), .min = FloatInput{system.size.min * s},
                    .max = FloatInput{system.size.max * s}});
    if (system.spin.max > 0.0F || system.spin.min > 0.0F) {
        m.init(InitSpinSlot, uid + PartSpin,
               InitSpin{.id = m.id(uid + PartSpin, 1U),
                        .speed_min = std::min(system.spin.min, system.spin.max),
                        .speed_max = std::max(system.spin.min, system.spin.max),
                        .both_ways = true,
                        .drag = 0.5F});
    }

    // What it meets: every Static object as its own shape, unless it
    // passes through.
    std::optional<Meeting> floor;
    if (system.collide) {
        for (const SceneItem* other : shared.ordered) {
            const auto* still = std::get_if<Object>(&other->data);
            if (still == nullptr || !other->simulate || still->motion != ObjectMotion::Static) {
                continue;
            }
            const Uid id_of = other->uid + PartColor;
            if (still->mesh == "floor") {
                float top = 0.0F;
                if (const MeshData* mesh = library_mesh("floor")) {
                    for (const auto& vertex : mesh->vertices) {
                        top = std::max(top, vertex.position.y);
                    }
                }
                PlaneCollision plane{.id = m.id(id_of, 1U)};
                plane.offset = (still->position.y + top * still->size) * s;
                plane.restitution = FloatInput{still->restitution};
                plane.friction = FloatInput{still->friction};
                plane.kill = false;
                m.after(AfterCollision, id_of, plane);
                // The first floor says how its particles meet bodies too.
                if (!floor) {
                    floor = Meeting{still->restitution, still->friction};
                }
            } else if (still->collider == ObjectCollider::Sphere) {
                m.after(AfterCollision, id_of,
                        SphereCollision{.id = m.id(id_of, 1U),
                                        .center = still->position * s,
                                        .radius = FloatInput{still->size * 0.5F * s},
                                        .inside = false,
                                        .restitution = FloatInput{still->restitution},
                                        .friction = FloatInput{still->friction},
                                        .kill = false});
            } else if (const MeshData* primitive = library_mesh(still->mesh);
                       primitive == nullptr && still->collider == ObjectCollider::Hull) {
                // The artist's model as its own surface: a distance field
                // fitted and turned as the model is drawn. Nothing to meet
                // until a model is picked.
                if (still->mesh.empty()) {
                    continue;
                }
                const float scale = std::max(still->size, 0.001F) * s;
                const std::string field = request_entry(AssetRequest::Kind::Field, still->mesh, scale, 72U,
                                                        ColliderSolid::Auto, true, still->rotation);
                if (std::none_of(out.requests.begin(), out.requests.end(),
                                 [&](const AssetRequest& r) { return r.entry == field; })) {
                    out.requests.push_back(AssetRequest{.kind = AssetRequest::Kind::Field,
                                                        .entry = field,
                                                        .path = still->mesh,
                                                        .item = other->uid,
                                                        .scale = scale,
                                                        .cells = 72U,
                                                        .solid = ColliderSolid::Auto,
                                                        .fit = true,
                                                        .turn = still->rotation});
                }
                m.after(AfterCollision, id_of,
                        SdfCollision{.id = m.id(id_of, 1U),
                                     .field = field,
                                     .offset = still->position * s,
                                     .inside = false,
                                     .restitution = FloatInput{still->restitution},
                                     .friction = FloatInput{still->friction},
                                     .kill = false});
            } else {
                // A box: a primitive's own bounds, or a model's longest side.
                Vec3 low{-0.5F, -0.5F, -0.5F};
                Vec3 high{0.5F, 0.5F, 0.5F};
                if (primitive != nullptr && !primitive->vertices.empty()) {
                    low = high = primitive->vertices.front().position;
                    for (const auto& vertex : primitive->vertices) {
                        const Vec3 v = vertex.position;
                        low = Vec3{std::min(low.x, v.x), std::min(low.y, v.y), std::min(low.z, v.z)};
                        high = Vec3{std::max(high.x, v.x), std::max(high.y, v.y), std::max(high.z, v.z)};
                    }
                }
                const Vec3 middle = rotate(prop_orientation(still->rotation), (low + high) * (0.5F * still->size));
                const Vec3 extent = (high - low) * still->size;
                m.after(AfterCollision, id_of,
                        BoxCollision{.id = m.id(id_of, 1U),
                                     .center = (still->position + middle) * s,
                                     .half_size = Vec3{std::fabs(extent.x), std::fabs(extent.y), std::fabs(extent.z)} *
                                                  (0.5F * s),
                                     .turn = still->rotation,
                                     .inside = false,
                                     .restitution = FloatInput{still->restitution},
                                     .friction = FloatInput{still->friction},
                                     .kill = false});
            }
        }
    }

    // The forces acting on it, each with an identity of the system's own.
    const std::vector<const SceneItem*> acting = forces_on(shared, uid);
    for (std::size_t k = 0U; k < acting.size() && k < max_forces_per_system; ++k) {
        const SceneItem& on = *acting[k];
        const Force& f = std::get<Force>(on.data);
        const Push push{.identity = uid + PartForces + static_cast<Uid>(k),
                        .seed = on.seed,
                        .strength = f.strength,
                        .live = nullptr,
                        .limits = {}};
        switch (f.kind) {
        case ForceKind::Wind: wind(push, f.heading); break;
        case ForceKind::Vortex: swirl(push, f.axis, f.position); break;
        case ForceKind::Attract: attract(push, f.position, f.reach); break;
        case ForceKind::DrawIn: draw_in(push, f.position, f.height); break;
        case ForceKind::Blast: break;
        }
    }

    // The rules that give it particles or bind them to a smoke grid, in
    // identity order.
    std::vector<const Relationship*> rules;
    for (const Relationship& r : shared.scene.relationships) {
        if (r.system == uid) {
            rules.push_back(&r);
        }
    }
    std::sort(rules.begin(), rules.end(), [](const Relationship* a, const Relationship* b) { return a->uid < b->uid; });
    // The bodies of the solid world a rule reads: one Object's, or every
    // one (0). Nothing when no world plays, or the Object does not.
    const auto bodies = [&](Uid object) -> std::optional<Scope> {
        if (solids.emitter.empty()) {
            return std::nullopt;
        }
        if (object == 0U) {
            return Scope{};
        }
        const SolidWorld::Member* member = solids.member(object);
        if (member == nullptr) {
            return std::nullopt;
        }
        return member->bodies;
    };
    for (const Relationship* r : rules) {
        switch (r->kind) {
        case RelationKind::EmitOnHit: {
            std::optional<Scope> from = bodies(r->object);
            if (!from) {
                break;
            }
            m.spawn(r->uid, SpawnFromContacts{.id = m.id(r->uid, 1U, r->seed),
                                              .emitter = solids.emitter,
                                              .min_speed = std::max(r->hardness, 0.0F) * s,
                                              .new_only = true,
                                              .per_hit = std::max(r->per_hit, 0.0F),
                                              .per_speed = std::max(r->per_speed, 0.0F) / s,
                                              .max_per_step = 64U,
                                              .along_normal = 0.15F,
                                              .along_surface = 0.3F,
                                              .spread = std::clamp(r->spread, 0.0F, 180.0F),
                                              .bodies = std::move(*from)});
            break;
        }
        case RelationKind::TrailFrom: {
            std::optional<Scope> from = bodies(r->object);
            if (!from) {
                break;
            }
            if (r->pieces) {
                from->sources = {piece_spawn_source};
            }
            m.spawn(r->uid, SpawnFromBodies{.id = m.id(r->uid, 1U, r->seed),
                                            .emitter = solids.emitter,
                                            .min_speed = std::max(r->min_speed, 0.0F) * s,
                                            .rate = std::max(r->rate, 0.0F),
                                            .per_speed = std::max(r->per_speed, 0.0F) / s,
                                            .max_per_step = 64U,
                                            .surface = 1.0F,
                                            .inherit = 0.0F,
                                            .bodies = std::move(*from)});
            break;
        }
        case RelationKind::TrailFromSystem:
            // Shed behind another system's particles, naming its emitter.
            m.spawn(r->uid, SpawnFromParticles{.id = m.id(r->uid, 1U, r->seed),
                                               .emitter = emitter_name(r->object),
                                               .min_speed = r->min_speed * s,
                                               .rate = r->rate,
                                               .per_speed = r->per_speed / s,
                                               .max_per_step = 64U,
                                               .inherit = 0.0F});
            break;
        case RelationKind::FeedSmoke:
        case RelationKind::RideSmoke: {
            // A grid that is switched off is not on the effect, so a rule on
            // it makes nothing rather than name a volume there is none of.
            // The amounts are rates and shares, which scale leaves alone.
            if (!relationship_active(shared.scene, *r)) {
                break;
            }
            const std::string volume = smoke_grid_volume(r->object);
            if (r->kind == RelationKind::FeedSmoke) {
                m.after(AfterRaw, r->uid,
                        VolumeSource{.id = m.id(r->uid, 1U),
                                     .volume = volume,
                                     .density = FloatInput{r->density},
                                     .temperature = FloatInput{r->heat},
                                     .velocity = FloatInput{r->push}});
            } else {
                m.force(ForceRaw, r->uid,
                        VolumeAdvect{.id = m.id(r->uid, 1U), .volume = volume, .strength = FloatInput{r->strength}});
            }
            break;
        }
        }
    }

    // The scene's blasts throw it, unless it passes through; and it meets
    // the solid world's bodies.
    if (system.collide) {
        for (const SceneBlast& blast : shared.blasts) {
            Shockwave wave{.id = m.keyed(10U, blast.identity), .blast = blast.blast.name};
            wave.scale_min = FloatInput{0.5F};
            wave.scale_max = FloatInput{1.0F};
            m.force(ForceShockwave, uid, std::move(wave));
        }
        if (solids.meetable()) {
            const Meeting met = floor.value_or(Meeting{});
            m.after(AfterCollision, uid,
                    BodyCollision{.id = m.keyed(9U, 0U),
                                  .emitter = solids.emitter,
                                  .restitution = FloatInput{met.restitution},
                                  .friction = FloatInput{met.friction},
                                  .kill = false});
        }
    }

    // Each lives its lifetime, or for ever.
    const float forever = std::numeric_limits<float>::max();
    const float life_min = std::max(system.persistent ? forever : system.lifetime.min, 0.01F);
    const float life_max = std::max(system.persistent ? forever : system.lifetime.max, life_min);
    m.init(InitLifetime, uid, Lifetime{.id = module_id(uid, 1U), .min = life_min, .max = life_max});
    // Its colour and size over its age.
    if (system.tint_driver == Driver::Age) {
        m.life(uid + PartTintAge,
               ColorOverLife{.id = m.id(uid + PartTintAge, 1U), .keys = system.tint_over_age});
    }
    if (system.size_driver == Driver::Age) {
        m.life(uid + PartSizeAge, SizeOverLife{.id = m.id(uid + PartSizeAge, 1U), .keys = system.size_over_age});
    }

    Emitter emitter;
    emitter.name = emitter_name(uid);
    emitter.seed = item.seed == 0U ? uid : hash_mix(uid, item.seed);
    emitter.capacity = max_emitter_capacity;
    m.fill(emitter);
    emitter.renderer = std::move(renderer);
    out.parameters = published.names();
    out.emitter = std::move(emitter);
    return out;
}

} // namespace

Compiled compile(const Document& document, std::uint32_t live) {
    Compiled compiled;
    if (!document.scene) {
        compiled.error = "There is no scene";
        return compiled;
    }
    if (const std::string error = validate_scene(document); !error.empty()) {
        compiled.error = error;
        return compiled;
    }
    const Scene& scene = *document.scene;
    Shared shared{.document = document,
                  .scene = scene,
                  .s = std::isfinite(document.scale) && document.scale > 0.0F ? document.scale : 1.0F,
                  .ordered = {},
                  .blasts = {},
                  .volumes = {},
                  .parameters = {}};
    const float s = shared.s;
    for (const SceneItem& item : scene.items) {
        shared.ordered.push_back(&item);
    }
    std::sort(shared.ordered.begin(), shared.ordered.end(),
              [](const SceneItem* a, const SceneItem* b) { return a->uid < b->uid; });

    Effect effect;
    effect.name = document.name.empty() ? std::string{"effect"} : document.name;
    effect.simulation_rate = 60U;

    // The scene's blasts, while anything plays to be blasted. Each is
    // named by its identity, so moving or adding items renames nothing.
    const bool any_plays = std::any_of(shared.ordered.begin(), shared.ordered.end(), [](const SceneItem* item) {
        return item->simulate &&
               (std::holds_alternative<Object>(item->data) || std::holds_alternative<ParticleSystem>(item->data));
    });
    if (any_plays) {
        for (const SceneItem* item : shared.ordered) {
            const auto* force = std::get_if<Force>(&item->data);
            if (force == nullptr || force->kind != ForceKind::Blast || !item->simulate) {
                continue;
            }
            const Uid identity = item->uid + PartWorld;
            shared.blasts.push_back(SceneBlast{identity,
                                               Blast{.name = blast_name(identity),
                                                     .center = force->position * s,
                                                     .power = std::max(force->power, 0.0F) * s,
                                                     .radius = std::max(force->radius, 0.01F) * s,
                                                     .time = std::max(force->time, 0.0F)}});
        }
        if (shared.blasts.size() > max_blasts) {
            shared.blasts.resize(max_blasts);
        }
    }
    for (const SceneBlast& blast : shared.blasts) {
        effect.blasts.push_back(blast.blast);
    }
    // Its smoke grids, which particle systems feed or ride by name. A box
    // that is not a whole number of cells grows evenly by less than one.
    for (const SceneItem* item : shared.ordered) {
        const auto* grid = std::get_if<SmokeGrid>(&item->data);
        if (grid == nullptr || !item->simulate) {
            continue;
        }
        const float cell = std::max(grid->cell, 0.0001F);
        const auto along = [cell](float extent) {
            const double count =
                std::ceil(static_cast<double>(std::fabs(extent)) / static_cast<double>(cell) - 1.0e-4);
            return static_cast<std::uint32_t>(std::clamp(count, 2.0, 1.0e9));
        };
        Volume volume;
        volume.name = smoke_grid_volume(item->uid);
        volume.nx = along(grid->size.x);
        volume.ny = along(grid->size.y);
        volume.nz = along(grid->size.z);
        volume.cell_size = cell * s;
        volume.origin = Vec3{grid->position.x * s - 0.5F * static_cast<float>(volume.nx) * volume.cell_size,
                             grid->position.y * s - 0.5F * static_cast<float>(volume.ny) * volume.cell_size,
                             grid->position.z * s - 0.5F * static_cast<float>(volume.nz) * volume.cell_size};
        volume.buoyancy = grid->rises;
        volume.weight = grid->sinks;
        volume.vorticity = grid->swirl;
        volume.pressure_iterations =
            static_cast<std::uint32_t>(std::lround(std::max(static_cast<float>(grid->solve), 0.0F)));
        volume.density_dissipation = grid->fades;
        volume.temperature_dissipation = grid->fades;
        volume.velocity_dissipation = grid->calms;
        volume.extinction = grid->thickness;
        volume.albedo = grid->brightness;
        volume.color = Vec3{grid->color.r, grid->color.g, grid->color.b};
        effect.volumes.push_back(std::move(volume));
    }

    // Reports for every item that plays, in identity order.
    std::map<Uid, ItemReport> reports;
    const auto report_of = [&](const SceneItem& item) -> ItemReport& {
        auto [at, inserted] = reports.try_emplace(item.uid, ItemReport{.item = item.uid});
        return at->second;
    };
    const auto fail = [&](const SceneItem& item, const std::string& why) {
        ItemReport& report = report_of(item);
        report.included = false;
        report.error = why;
        // The runtime refuses a grid by its volume's name, from whichever
        // emitter it was checked beside: the item at fault is the grid.
        const SceneItem* at_fault = &item;
        if (const Uid grid = volume_named_in(why); grid != 0U) {
            if (const SceneItem* owner = scene.item(grid - 1U)) {
                at_fault = owner;
                report_of(*owner).error = why;
            }
        }
        if (compiled.error.empty()) {
            compiled.error = at_fault->name + ": " + why;
        }
    };
    // Checks an emitter beside those already in, as a host loads them, and
    // sizes it to the most particles it can have alive.
    std::vector<Emitter> so_far;
    const auto sized = [&](Emitter& emitter, std::uint64_t extra, std::string& why) {
        Effect alone;
        alone.name = effect.name;
        alone.simulation_rate = effect.simulation_rate;
        alone.blasts = effect.blasts;
        alone.volumes = effect.volumes;
        alone.parameters = shared.parameters;
        alone.emitters = so_far;
        const std::uint64_t bound = particle_bound(alone, emitter) + extra;
        if (bound > max_emitter_capacity) {
            why = "It could have more alive than any emitter may (" + std::to_string(max_emitter_capacity) + ")";
            return false;
        }
        emitter.capacity = static_cast<std::uint32_t>(rounded_capacity(bound));
        alone.emitters.push_back(emitter);
        if (Result<bool> valid = validate(alone); !valid) {
            why = valid.error().message;
            return false;
        }
        return true;
    };

    // The solid world first: what particles meet and are born of.
    std::vector<ObjectPart> parts;
    for (const SceneItem* item : shared.ordered) {
        const auto* object = std::get_if<Object>(&item->data);
        if (object == nullptr) {
            continue;
        }
        ItemReport& report = report_of(*item);
        report.baked = true;
        if (!item->simulate) {
            continue;
        }
        ObjectPart part = object_part(shared, *item, *object);
        if (!part.error.empty()) {
            fail(*item, part.error);
            continue;
        }
        parts.push_back(std::move(part));
    }
    std::vector<Emitter> emitters;
    std::map<Uid, std::size_t> emitter_at;
    SolidWorld solids;
    if (!parts.empty() && compiled.error.empty()) {
        BuiltWorld world = build_world(shared, parts);
        std::string why = world.error;
        if (why.empty()) {
            Emitter& emitter = *world.emitter;
            // Dropped things are born in it, at most the scene's budget at once.
            const std::uint32_t drops = scene.physics.max_dropped;
            if (drops > 0U) {
                emitter.spawn.push_back(SpawnActions{.id = module_id(parts.front().item->uid, 12U),
                                                     .max_per_step = SolidWorld::drops_per_step,
                                                     .max_alive = drops});
            }
            // Wind that blows on things blows on every body of it.
            for (const SceneItem* item : shared.ordered) {
                const auto* force = std::get_if<Force>(&item->data);
                if (force == nullptr || force->kind != ForceKind::Wind || !force->bodies || !item->simulate ||
                    !(force->air_speed > 0.0F)) {
                    continue;
                }
                const float heading = force->heading * (3.14159265358979F / 180.0F);
                emitter.update.emplace_back(BodyWind{
                    .id = item->uid * 16U,
                    .velocity = Vec3{std::cos(heading), 0.0F, -std::sin(heading)} * force->air_speed * s,
                    .drag = body_wind_drag,
                    .gust = std::clamp(force->gust, 0.0F, 1.0F),
                    .gust_seconds = body_wind_gust_seconds,
                });
            }
            if (sized(emitter, world.extra, why)) {
                solids.emitter = emitter.name;
                solids.capacity = emitter.capacity;
                solids.moves = !emitter.spawn.empty();
                if (drops > 0U) {
                    solids.drops = static_cast<std::uint32_t>(emitter.spawn.size() - 1U);
                    solids.max_dropped = drops;
                }
                const std::uint32_t stride = gpu::particle_layout(emitter).stride;
                for (std::size_t b = 0U; b < parts.size(); ++b) {
                    const SceneItem& item = *parts[b].item;
                    solids.items.push_back(SolidWorld::Member{item.uid, world.scopes[b]});
                    ItemReport& report = report_of(item);
                    report.included = true;
                    report.emitter = emitter.name;
                    report.scope = world.scopes[b];
                    report.emitter_capacity = emitter.capacity;
                    // Its own share: its bodies and what they break into.
                    report.capacity = static_cast<std::uint32_t>(rounded_capacity(parts[b].count + parts[b].extra));
                    report.bytes = static_cast<std::uint64_t>(stride) * report.capacity;
                    report.prop = parts[b].prop.has_value();
                    if (parts[b].pile) {
                        report.spawns.push_back(parts[b].pile->id);
                    }
                    for (const AssetRequest& request : parts[b].requests) {
                        if (std::none_of(compiled.requests.begin(), compiled.requests.end(),
                                         [&](const AssetRequest& r) { return r.entry == request.entry; })) {
                            compiled.requests.push_back(request);
                        }
                    }
                }
                compiled.cuts = std::move(world.cuts);
                so_far.push_back(emitter);
                emitter_at[parts.front().item->uid] = emitters.size();
                emitters.push_back(std::move(emitter));
            }
        }
        if (!why.empty()) {
            for (const ObjectPart& part : parts) {
                fail(*part.item, "It does not fit in the scene's one solid world: " + why);
            }
        }
    }

    // Then each particle system, beside what is in already.
    std::set<std::string> used;
    for (const SceneItem* item : shared.ordered) {
        const auto* system = std::get_if<ParticleSystem>(&item->data);
        if (system == nullptr) {
            continue;
        }
        ItemReport& report = report_of(*item);
        report.emitter = emitter_name(item->uid);
        report.baked = item->uid != live;
        if (!item->simulate) {
            continue;
        }
        SystemBuild build = build_system(shared, *item, *system, item->uid == live, solids);
        if (!build.error.empty()) {
            fail(*item, build.error);
            continue;
        }
        Emitter& emitter = *build.emitter;
        std::string why;
        if (!sized(emitter, 0U, why)) {
            fail(*item, why);
            continue;
        }
        report.included = true;
        report.capacity = emitter.capacity;
        report.emitter_capacity = emitter.capacity;
        report.bytes = static_cast<std::uint64_t>(gpu::particle_layout(emitter).stride) * emitter.capacity;
        for (const SpawnModule& spawn : emitter.spawn) {
            report.spawns.push_back(std::visit([](const auto& typed) { return typed.id; }, spawn));
        }
        used.insert(build.parameters.begin(), build.parameters.end());
        for (const AssetRequest& request : build.requests) {
            if (std::none_of(compiled.requests.begin(), compiled.requests.end(),
                             [&](const AssetRequest& r) { return r.entry == request.entry; })) {
                compiled.requests.push_back(request);
            }
        }
        so_far.push_back(emitter);
        emitter_at[item->uid] = emitters.size();
        emitters.push_back(std::move(emitter));
    }
    // Smoke grids build no emitter; they play while they simulate.
    for (const SceneItem* item : shared.ordered) {
        if (std::holds_alternative<SmokeGrid>(item->data)) {
            ItemReport& report = report_of(*item);
            report.included = item->simulate;
            report.baked = false;
        }
    }

    for (auto& [uid, report] : reports) {
        compiled.items.push_back(std::move(report));
    }
    compiled.solids = std::move(solids);
    if (!compiled.error.empty()) {
        // A scene that cannot play whole does not play: never replace a
        // verified preview with a plausible incomplete one.
        return compiled;
    }
    // In identity order, which is the order they are drawn in.
    for (const auto& [uid, at] : emitter_at) {
        effect.emitters.push_back(std::move(emitters[at]));
    }

    for (const Emitter& emitter : effect.emitters) {
        if (!emitter.renderer.texture.empty() && !emitter.renderer.texture.starts_with("user.")) {
            if (const TextureData* texture = library_texture_data(emitter.renderer.texture)) {
                compiled.textures.emplace(emitter.renderer.texture, *texture);
            }
        }
        // The models it draws, its body kinds collide and draw as, and its
        // props are shaped by.
        std::vector<std::string> models{emitter.renderer.mesh};
        if (const RigidPhysics* physics = rigid_physics(emitter)) {
            for (const RigidBodyKind& of : physics->bodies) {
                models.push_back(of.mesh);
                models.push_back(of.draw);
            }
            for (const RigidProp& prop : physics->props) {
                models.push_back(prop.mesh);
            }
        }
        for (const std::string& model : models) {
            if (model.empty() || model.starts_with("user.")) {
                continue;
            }
            if (const MeshData* mesh = library_mesh(model)) {
                compiled.meshes.emplace(model, *mesh);
            }
        }
    }
    for (const ParameterDesc& parameter : shared.parameters) {
        if (used.contains(parameter.name)) {
            effect.parameters.push_back(parameter);
        }
    }
    if (!effect.emitters.empty()) {
        compiled.effect = std::move(effect);
    }
    return compiled;
}


const ItemReport* Compiled::item(Uid uid) const {
    for (const ItemReport& report : items) {
        if (report.item == uid) {
            return &report;
        }
    }
    return nullptr;
}

std::uint32_t Compiled::total_capacity() const {
    std::uint32_t total = 0U;
    for (const ItemReport& report : items) {
        total += report.included ? report.capacity : 0U;
    }
    return total;
}

std::string blast_name(Uid identity) {
    return "blast_" + std::to_string(identity);
}

std::string volume_name(Uid identity) {
    return "grid_" + std::to_string(identity);
}

Uid volume_identity(std::string_view name) {
    constexpr std::string_view prefix = "grid_";
    if (!name.starts_with(prefix) || name.size() == prefix.size()) {
        return 0U;
    }
    Uid identity = 0U;
    for (const char c : name.substr(prefix.size())) {
        if (c < '0' || c > '9' || identity > 0x0fffffffU) {
            return 0U;
        }
        identity = identity * 10U + static_cast<Uid>(c - '0');
    }
    return identity;
}

Uid volume_named_in(std::string_view message) {
    // The runtime quotes names in single quotes (ajfx::validate).
    constexpr std::string_view lead = "volume '";
    const std::size_t at = message.find(lead);
    if (at == std::string_view::npos) {
        return 0U;
    }
    const std::size_t from = at + lead.size();
    const std::size_t end = message.find('\'', from);
    if (end == std::string_view::npos) {
        return 0U;
    }
    return volume_identity(message.substr(from, end - from));
}

namespace {

void widen(Scope& scope, std::uint32_t parent, std::uint32_t first, std::uint32_t count) {
    if (scope.kinds.empty() || std::find(scope.kinds.begin(), scope.kinds.end(), parent) == scope.kinds.end()) {
        return;
    }
    for (std::uint32_t k = first; k < first + count && scope.kinds.size() < max_scope_entries; ++k) {
        if (std::find(scope.kinds.begin(), scope.kinds.end(), k) == scope.kinds.end()) {
            scope.kinds.push_back(k);
        }
    }
}

// Widens every scope naming body kind `parent` of `emitter` to the `count`
// kinds from `first`, which a cut made of it: a piece is in every scope its
// parent was. Particle emitters born of that emitter's bodies (hits and
// trails) and the reports and world that name it follow too.
void widen_scopes(Compiled& compiled, const std::string& emitter, std::uint32_t parent, std::uint32_t first,
                  std::uint32_t count) {
    const auto widen_only = [&](auto& typed) {
        if constexpr (requires { typed.only; }) {
            widen(typed.only, parent, first, count);
        }
    };
    if (compiled.effect) {
        for (Emitter& e : compiled.effect->emitters) {
            if (e.name == emitter) {
                for (InitModule& module : e.init) {
                    std::visit(widen_only, module);
                }
                for (UpdateModule& module : e.update) {
                    std::visit(widen_only, module);
                }
                continue;
            }
            for (SpawnModule& module : e.spawn) {
                if (auto* hits = std::get_if<SpawnFromContacts>(&module); hits != nullptr && hits->emitter == emitter) {
                    widen(hits->bodies, parent, first, count);
                } else if (auto* moving = std::get_if<SpawnFromBodies>(&module);
                           moving != nullptr && moving->emitter == emitter) {
                    widen(moving->bodies, parent, first, count);
                }
            }
        }
    }
    for (ItemReport& report : compiled.items) {
        if (report.emitter == emitter) {
            widen(report.scope, parent, first, count);
        }
    }
    if (compiled.solids.emitter == emitter) {
        for (SolidWorld::Member& member : compiled.solids.items) {
            widen(member.bodies, parent, first, count);
        }
    }
}

} // namespace

// Cuts the models the compiled effect's breaking Objects are cut from, now
// that the models are read. Each cut adds a body kind and a mesh per cell and
// the Break module that scatters them, so the pieces are the piece rather
// than models laid beside it. A cut that cannot be made leaves the Object
// playing unbroken and says why.
void make_cuts(Compiled& compiled, std::string& issue) {
    if (!compiled.effect) {
        return;
    }
    for (const FractureCut& cut : compiled.cuts) {
        const auto named = [&](const std::string& why) {
            if (issue.empty()) {
                issue = why;
            }
        };
        Emitter* emitter = nullptr;
        for (Emitter& candidate : compiled.effect->emitters) {
            if (candidate.name == cut.emitter) {
                emitter = &candidate;
            }
        }
        const auto model = compiled.meshes.find(cut.model);
        if (emitter == nullptr || model == compiled.meshes.end()) {
            named("the model to cut into pieces could not be read");
            continue;
        }
        RigidPhysics* physics = nullptr;
        for (UpdateModule& module : emitter->update) {
            if (auto* found = std::get_if<RigidPhysics>(&module)) {
                physics = found;
            }
        }
        if (physics == nullptr) {
            named("an object that comes apart needs a physics world");
            continue;
        }
        const FractureOptions options{.count = cut.cells,
                                      .seed = cut.seed,
                                      .focus = Vec3{},
                                      .focus_bias = std::clamp(cut.spread, 0.0F, 1.0F)};
        Result<FracturedPieces> made = cells_as_pieces(
            model->second, cut.model, options, static_cast<std::uint32_t>(physics->bodies.size()) + 1U);
        if (!made) {
            named(made.error().message);
            continue;
        }
        const auto first_cell = static_cast<std::uint32_t>(physics->bodies.size()) + 1U;
        const auto cell_count = static_cast<std::uint32_t>(made.value().bodies.size());
        // An Object's size is its kind's scale, so the cells cut from it
        // keep that scale and are as big as the thing they came from.
        const bool has_parent = cut.body >= 1U && cut.body <= physics->bodies.size();
        const RigidBodyKind parent = has_parent ? physics->bodies[cut.body - 1U] : RigidBodyKind{};
        const float parent_scale = has_parent ? parent.scale : 1.0F;
        for (RigidBodyKind& kind : made.value().bodies) {
            kind.scale *= parent_scale;
            kind.draw_tint = parent.draw_tint;
            kind.density = parent.density;
            kind.friction = parent.friction;
            kind.restitution = parent.restitution;
            physics->bodies.push_back(std::move(kind));
        }
        // A cell is in every scope the thing it was cut from is in.
        widen_scopes(compiled, cut.emitter, cut.body, first_cell, cell_count);
        for (auto& [name, mesh] : made.value().meshes) {
            compiled.meshes.emplace(name, std::move(mesh));
        }
        emitter->update.push_back(Break{.id = cut.id,
                                        .body = cut.body,
                                        .threshold = cut.threshold,
                                        .blast_threshold = cut.blast_threshold,
                                        .scatter = cut.scatter,
                                        .spin = cut.spin,
                                        .pieces = std::move(made).value().pieces});
    }
    if (compiled.cuts.empty()) {
        return;
    }
    // The cuts went in after the compiler validated, so what they made is
    // held to the same rules here rather than reaching a host unchecked.
    if (Result<bool> valid = validate(*compiled.effect); !valid) {
        if (issue.empty()) {
            issue = valid.error().message;
        }
        compiled.effect.reset();
    }
}

std::vector<PackageEntry> asset_entries(const Compiled& compiled) {
    std::vector<PackageEntry> entries = texture_package_entries(compiled.textures);
    for (PackageEntry& entry : mesh_package_entries(compiled.meshes)) {
        entries.push_back(std::move(entry));
    }
    for (PackageEntry& entry : field_package_entries(compiled.fields)) {
        entries.push_back(std::move(entry));
    }
    return entries;
}

float loop_seconds(const Document& document) {
    return std::clamp(document.loop_seconds, 0.5F, 3600.0F);
}

} // namespace ajfx::studio
