#include "layer_builder.hpp"

#include "build_core.hpp"
#include "catalog.hpp"
#include "library.hpp"
#include "raw_modules.hpp"
#include "rigid_builder.hpp"

#include <ajfx/random.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <set>
#include <span>
#include <tuple>
namespace ajfx::studio {
namespace {
constexpr float earth_gravity = 9.81F;
constexpr std::size_t over_life_keys = 17U;
Vec3 normalized(Vec3 v, Vec3 fallback) {
    const float length = std::sqrt(dot(v, v));
    return length > 1.0e-6F ? v * (1.0F / length) : fallback;
}

float smoothstep(float edge0, float edge1, float x) {
    if (edge0 == edge1) {
        return x < edge0 ? 0.0F : 1.0F;
    }
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

// A piecewise-linear multiplier through `keys` (t, r, g, b).
Color ramp(std::span<const std::array<float, 4>> keys, float t) {
    if (t <= keys.front()[0]) {
        return Color{keys.front()[1], keys.front()[2], keys.front()[3], 1.0F};
    }
    for (std::size_t i = 1U; i < keys.size(); ++i) {
        if (t <= keys[i][0]) {
            const float f = (t - keys[i - 1U][0]) / (keys[i][0] - keys[i - 1U][0]);
            const auto lerp = [&](std::size_t c) { return keys[i - 1U][c] + (keys[i][c] - keys[i - 1U][c]) * f; };
            return Color{lerp(1U), lerp(2U), lerp(3U), 1.0F};
        }
    }
    return Color{keys.back()[1], keys.back()[2], keys.back()[3], 1.0F};
}
} // namespace

BuildCore::BuildCore(const Layer& layer, float scale, std::vector<ParameterDesc>& parameters, PageWorld world,
                     SolidWorld solids, std::vector<PageBlast> blasts)
    : layer_{layer}, s_{scale}, parameters_{parameters}, world_{world}, solids_{std::move(solids)},
      blasts_{std::move(blasts)} {}

// A block's module id. Random streams are keyed by id, so a block's seed
// picks another id to deal its particles other values. Seeded ids have
// the top bit set, apart from every plain uid * 16 + slot, and the rare
// clash of two seeded ones moves on to the next free id.
ModuleId BuildCore::id(const Block& block, std::uint32_t slot) {
    const float seed = number_of(block, "seed");
    if (!(seed >= 1.0F)) {
        return module_id(block.uid, slot);
    }
    constexpr ModuleId seeded = 0x80000000U;
    ModuleId id = seeded | (hash_mix(module_id(block.uid, slot), static_cast<std::uint32_t>(seed)) & ~seeded);
    while (!seeded_.insert(id).second) {
        id = seeded | ((id + 1U) & ~seeded);
    }
    return id;
}

ModuleId BuildCore::meeting_id(Uid solid) {
    constexpr ModuleId seeded = 0x80000000U;
    ModuleId id = seeded | (hash_mix(module_id(layer_.uid, 9U), solid) & ~seeded);
    while (!seeded_.insert(id).second) {
        id = seeded | ((id + 1U) & ~seeded);
    }
    return id;
}

ModuleId BuildCore::blast_id(Uid blast) {
    constexpr ModuleId seeded = 0x80000000U;
    ModuleId id = seeded | (hash_mix(module_id(layer_.uid, 10U), blast) & ~seeded);
    while (!seeded_.insert(id).second) {
        id = seeded | ((id + 1U) & ~seeded);
    }
    return id;
}

ModuleId BuildCore::wake_id() {
    constexpr ModuleId seeded = 0x80000000U;
    ModuleId id = seeded | (hash_mix(module_id(layer_.uid, 11U), 0U) & ~seeded);
    while (!seeded_.insert(id).second) {
        id = seeded | ((id + 1U) & ~seeded);
    }
    return id;
}

// Names the package entry an artist's file goes in, and asks for it.
std::string BuildCore::request(AssetRequest::Kind kind, const Block& block, const std::string& path, float scale,
                               std::uint32_t cells, ColliderSolid solid) {
    std::string options;
    if (kind == AssetRequest::Kind::Field) {
        options = format_number(scale, 6) + "/" + std::to_string(cells) + "/" +
                  std::to_string(static_cast<int>(solid));
    }
    const std::string_view what = kind == AssetRequest::Kind::Texture ? "texture"
                                  : kind == AssetRequest::Kind::Mesh  ? "mesh"
                                                                      : "field";
    AssetRequest wanted{kind, user_asset_name(what, path, options), path, layer_.uid, block.uid, scale, cells, solid};
    const std::string entry = wanted.entry;
    if (std::none_of(requests_.begin(), requests_.end(), [&](const AssetRequest& r) { return r.entry == entry; })) {
        requests_.push_back(std::move(wanted));
    }
    return entry;
}

class LayerBuilder::Impl final : public BuildCore {
public:
    Impl(const Layer& layer, float scale, std::vector<ParameterDesc>& parameters, PageWorld world,
         SolidWorld solids, std::vector<PageBlast> blasts)
        : BuildCore{layer, scale, parameters, world, std::move(solids), std::move(blasts)}, rigid_{*this} {}

    [[nodiscard]] const RigidBuilder& rigid() const noexcept { return rigid_; }

    // Builds the emitter; on failure sets `error` and `error_block`.
    std::optional<Emitter> build(LayerReport& report) {
        // The layer's kind decides, not what blocks it happens to hold: a
        // rigid layer is one world, declared once, and the engine turns its
        // pieces, so Tumble there only sets the spin they are thrown with.
        physics_ = layer_.kind == LayerKind::Rigid;
        if (physics_) {
            rigid_.world();
        } else if (std::any_of(layer_.blocks.begin(), layer_.blocks.end(), [](const Block& b) {
                       const auto* update = b.raw ? std::get_if<UpdateModule>(&*b.raw) : nullptr;
                       return b.enabled && b.kind == BlockKind::Module && update != nullptr &&
                              std::holds_alternative<RigidPhysics>(*update);
                   })) {
            // An expert put the module in by hand with a Module block. The
            // runtime's own validation has the last word on that.
            physics_ = true;
        }
        for (const Block& block : layer_.blocks) {
            if (block.enabled) {
                add(block);
            }
            if (!error_.empty()) {
                report.error = error_;
                report.error_block = block.uid;
                return std::nullopt;
            }
        }
        // The last Object on the page has nothing after it to close it.
        rigid_.finish_object();
        if (!error_.empty()) {
            report.error = error_;
            return std::nullopt;
        }
        // The page's ground, gravity and wind, and its solid things.
        place_world();
        // A layer of solid things draws them, whether or not it has an
        // Appearance block saying so. An Object's model is drawn only where
        // the layer draws models, and having to add a Look block before the
        // model you picked appears is not something to discover. It is the
        // kind that decides, not whether an Object is in it yet: a Stream
        // that is slabs alone must be slabs beside an Object, since the
        // layer draws every piece one way and its pieces' Auto shape follows
        // what is drawn (a ball for sprites, a hull for models).
        if (layer_.kind == LayerKind::Rigid && !saw_appearance_ && renderer_.geometry != Geometry::Mesh) {
            renderer_.geometry = Geometry::Mesh;
            renderer_.mesh = "slab";
            renderer_.texture.clear();
        }
        if (!error_.empty()) {
            report.error = error_;
            return std::nullopt;
        }
        Emitter emitter;
        emitter.seed = layer_.seed == 0U ? layer_.uid : hash_mix(layer_.uid, layer_.seed);
        emitter.capacity = max_emitter_capacity;

        const float life_min = std::max(layer_.life_min, 0.01F);
        place_init(SlotLifetime, layer_.uid,
                   Lifetime{.id = module_id(layer_.uid, 1U), .min = life_min,
                            .max = std::max(layer_.life_max, life_min)});
        finish_over_life();
        // Every Object's size, the kinds of body the layer holds, and the props and
        // bodies that go on its physics world: the rigid builder's (rigid_builder.cpp).
        if (!rigid_.finish(report)) {
            return std::nullopt;
        }
        const auto by_slot = [](const Placed& a, const Placed& b) { return a.slot < b.slot; };
        std::stable_sort(init_.begin(), init_.end(), by_slot);
        std::stable_sort(forces_.begin(), forces_.end(), by_slot);
        std::stable_sort(after_.begin(), after_.end(), by_slot);
        // Each spawn module's particles draw their random values by its place
        // in the spawn stack, so the stack is in the order of the blocks'
        // uids rather than the page's: moving a block, or adding one, leaves
        // every other block's particles where they were.
        std::stable_sort(spawn_.begin(), spawn_.end(),
                         [](const Placed& a, const Placed& b) { return a.line.origin < b.line.origin; });
        for (Placed& placed : spawn_) {
            emitter.spawn.push_back(std::get<SpawnModule>(placed.line.module));
            report.modules.push_back(std::move(placed.line));
        }
        for (Placed& placed : init_) {
            emitter.init.push_back(std::get<InitModule>(placed.line.module));
            report.modules.push_back(std::move(placed.line));
        }
        for (std::vector<Placed>* group : {&forces_, &after_, &life_}) {
            for (Placed& placed : *group) {
                emitter.update.push_back(std::get<UpdateModule>(placed.line.module));
                report.modules.push_back(std::move(placed.line));
            }
        }
        emitter.renderer = renderer_;
        report.renderer = renderer_;
        return emitter;
    }

private:
    // The first block of this kind the layer has switched on, or null.
    [[nodiscard]] const Block* enabled_block(BlockKind kind) const {
        for (const Block& block : layer_.blocks) {
            if (block.enabled && block.kind == kind) {
                return &block;
            }
        }
        return nullptr;
    }

    // Whether a Module block holds a ground plane: a Ground detached into
    // its module is still the layer's own ground.
    [[nodiscard]] bool has_raw_plane() const {
        return std::any_of(layer_.blocks.begin(), layer_.blocks.end(), [](const Block& b) {
            const auto* update = b.enabled && b.raw ? std::get_if<UpdateModule>(&*b.raw) : nullptr;
            return update != nullptr && std::holds_alternative<PlaneCollision>(*update);
        });
    }

    // What the layer does where it meets the ground or something solid: its
    // own Ground block's answer where it has one, else what it is.
    struct Meeting final {
        float restitution{0.0F};
        float friction{0.0F};
        bool kill{false};
    };
    [[nodiscard]] Meeting meeting(const WorldShare& share, const Block* own_ground) const {
        Contact contact = share.contact;
        float bounce = share.bounce;
        float friction = share.friction;
        if (own_ground != nullptr) {
            const std::string response = choice_of(*own_ground, "response");
            contact = response == "bounce"   ? Contact::Bounce
                      : response == "stick"  ? Contact::Stick
                      : response == "vanish" ? Contact::Vanish
                                             : Contact::Slide;
            bounce = number_of(*own_ground, "bounce");
            friction = number_of(*own_ground, "friction");
        }
        // A body leaves the world when its life ends, never on contact.
        if (physics_ && contact == Contact::Vanish) {
            contact = Contact::Stick;
        }
        switch (contact) {
        case Contact::Bounce:
            return Meeting{bounce, friction, false};
        case Contact::Slide:
            return Meeting{0.0F, friction, false};
        case Contact::Stick:
            return Meeting{0.0F, 1.0F, false};
        case Contact::Vanish:
            return Meeting{0.0F, 0.0F, true};
        }
        return Meeting{};
    }

    // The page's world (Document::world): its gravity and wind by the
    // layer's share of them, its ground, and every solid thing on the page,
    // each made from the layer (module ids from its uid) rather than from a
    // block, so they are there without anyone adding them. A layer's own
    // Gravity or Ground block stands in for the page's; Passes through opts
    // out of the solids, and of the ground if it says so.
    void place_world() {
        const WorldShare share = world_share(layer_);
        const Block* own_gravity = enabled_block(BlockKind::Gravity);
        const Block* own_ground = enabled_block(BlockKind::Ground);
        const Block* through = enabled_block(BlockKind::PassesThrough);
        const float s = s_;
        if (own_gravity == nullptr && share.gravity > 0.0F && world_.gravity > 0.0F) {
            place_force(SlotGravity, layer_.uid,
                        Gravity{.id = module_id(layer_.uid, 6U),
                                .acceleration =
                                    Vec3{0.0F, -earth_gravity * world_.gravity * share.gravity * s, 0.0F}});
        }
        if (share.wind > 0.0F && world_.wind > 0.0F) {
            const float heading = world_.wind_heading * 3.14159265F / 180.0F;
            const float push = world_.wind * share.wind * s;
            place_force(SlotWind, layer_.uid,
                        Gravity{.id = module_id(layer_.uid, 7U),
                                .acceleration = Vec3{std::cos(heading) * push, 0.0F, -std::sin(heading) * push}});
        }
        const Meeting met = meeting(share, own_ground);
        const bool through_ground = through != nullptr && toggle_of(*through, "ground");
        if (world_.ground && own_ground == nullptr && !through_ground && !has_raw_plane()) {
            PlaneCollision plane{.id = module_id(layer_.uid, 8U)};
            plane.offset = world_.ground_height * s;
            plane.restitution = FloatInput{met.restitution};
            plane.friction = FloatInput{met.friction};
            plane.kill = met.kill;
            place_after(SlotCollision, layer_.uid, plane);
        }
        // The page's explosions throw every layer's pieces and particles,
        // by the share what it is takes of them, unless it passes through.
        // Made from the layer and keyed by both uids, in the order of the
        // Explosion blocks' uids, so they are there without anyone adding
        // them and stay put when either moves.
        if (through == nullptr) {
            for (const PageBlast& blast : blasts_) {
                Shockwave wave{.id = blast_id(blast.block), .blast = blast.blast.name};
                wave.scale_min = FloatInput{share.blast_min};
                wave.scale_max = FloatInput{std::max(share.blast_max, share.blast_min)};
                place_force(SlotShockwave, layer_.uid, std::move(wave));
            }
        }
        // Particles meet every solid thing on the page, which is one world
        // (SolidWorld) whose emitter steps before any other. A rigid layer's
        // pieces are that world's own bodies.
        if (physics_ || through != nullptr || !solids_.meetable()) {
            return;
        }
        // Smoke and dust are pushed along by the solid things moving through
        // them, unless a Stirred block says how.
        if (solids_.moves && enabled_block(BlockKind::Stirred) == nullptr &&
            (layer_.what == LayerWhat::Smoke || layer_.what == LayerWhat::Dust)) {
            place_force(SlotWake, layer_.uid, Wake{.id = wake_id(), .emitter = solids_.emitter});
        }
        place_after(SlotCollision, layer_.uid,
                    BodyCollision{.id = meeting_id(0U),
                                  .emitter = solids_.emitter,
                                  .restitution = FloatInput{met.restitution},
                                  .friction = FloatInput{met.friction},
                                  .kill = met.kill});
    }

    // The bodies of the page's solid world a block born of solid things
    // reads: the solid layer's it names, or every one (0). Nothing when no
    // solid layer plays, or the one it names does not.
    [[nodiscard]] std::optional<Scope> solid_bodies(const Block& block) const {
        if (solids_.emitter.empty()) {
            return std::nullopt;
        }
        const auto named = static_cast<Uid>(std::max(number_of(block, "solid"), 0.0F));
        if (named == 0U) {
            return Scope{};
        }
        const SolidWorld::Member* member = solids_.member(named);
        if (member == nullptr) {
            return std::nullopt;
        }
        return member->bodies;
    }

    // Puffs or sparks where the page's solid things hit (SpawnFromContacts).
    // Speeds are the page's, so a hit's speed scales as every other does.
    void compile_hits(const Block& block) {
        std::optional<Scope> bodies = solid_bodies(block);
        if (!bodies) {
            return;
        }
        const float s = s_;
        place_spawn(block.uid,
                    SpawnFromContacts{.id = id(block, 1U),
                                      .emitter = solids_.emitter,
                                      .min_speed = std::max(number_of(block, "hardness"), 0.0F) * s,
                                      .new_only = toggle_of(block, "new_only"),
                                      .per_hit = std::max(number_of(block, "per_hit"), 0.0F),
                                      .per_speed = std::max(number_of(block, "per_speed"), 0.0F) / s,
                                      .max_per_step = static_cast<std::uint32_t>(
                                          std::max(1L, std::lround(number_of(block, "most")))),
                                      .along_normal = std::max(number_of(block, "up"), 0.0F),
                                      .along_surface = std::max(number_of(block, "across"), 0.0F),
                                      .spread = std::clamp(number_of(block, "spread"), 0.0F, 180.0F),
                                      .bodies = std::move(*bodies)});
    }

    // Shed behind the page's moving solid things (SpawnFromBodies): every
    // one, or only the pieces broken ones became.
    void compile_trail(const Block& block) {
        std::optional<Scope> bodies = solid_bodies(block);
        if (!bodies) {
            return;
        }
        if (toggle_of(block, "pieces")) {
            bodies->sources = {piece_spawn_source};
        }
        const float s = s_;
        place_spawn(block.uid,
                    SpawnFromBodies{.id = id(block, 1U),
                                    .emitter = solids_.emitter,
                                    .min_speed = std::max(number_of(block, "min_speed"), 0.0F) * s,
                                    .rate = std::max(number_of(block, "rate"), 0.0F),
                                    .per_speed = std::max(number_of(block, "per_speed"), 0.0F) / s,
                                    .max_per_step = static_cast<std::uint32_t>(
                                        std::max(1L, std::lround(number_of(block, "most")))),
                                    .surface = std::clamp(number_of(block, "surface"), 0.0F, 1.0F),
                                    .inherit = std::clamp(number_of(block, "inherit"), 0.0F, 1.0F),
                                    .bodies = std::move(*bodies)});
    }

    // Pushed along by the page's moving solid things (Wake): those of the
    // solid layer it names, or all of them. Nothing when nothing solid plays
    // or the world is too full to be looked through.
    void compile_stirred(const Block& block) {
        std::optional<Scope> bodies = solid_bodies(block);
        if (!bodies || !solids_.meetable()) {
            return;
        }
        Wake wake{.id = id(block, 1U), .emitter = solids_.emitter};
        wake.strength = FloatInput{1.0F};
        scale_by(wake, block, "strength");
        wake.radius = std::max(number_of(block, "reach"), 0.01F);
        wake.min_speed = std::max(number_of(block, "min_speed"), 0.0F) * s_;
        wake.bodies = std::move(*bodies);
        place_force(SlotWake, block.uid, std::move(wake));
    }

    // A glow that cools with age. In a solid layer it may be the broken
    // pieces' alone, so a crate's fragments glow where the crate did not.
    void compile_heat(const Block& block) {
        Heat heat{.id = id(block, 1U),
                  .start = std::max(number_of(block, "start"), 0.0F),
                  .half_life = std::max(number_of(block, "half_life"), 0.01F),
                  .glow = std::max(number_of(block, "glow"), 0.0F)};
        if (physics_ && choice_of(block, "what") == "pieces") {
            heat.only.sources = {piece_spawn_source};
        }
        // With the looks over life: every heat is added after every colour
        // over life, wherever it stands.
        life_.push_back(Placed{2, ModuleLine{block.uid, RawModule{UpdateModule{std::move(heat)}}}});
    }

    // The two ends of a force block's strength, as the factor each particle
    // takes the force by (see the forces in ajfx/modules.hpp).
    template <class Force>
    void scale_by(Force& force, const Block& block, std::string_view control) {
        const Value strength = get(block, control);
        std::tie(force.scale_min, force.scale_max) = range_input(block, control, strength.x, strength.y, 1.0F);
    }

    // A force's strength in a rigid layer. The physics engine applies one
    // gravity to the whole world and one damping to each body, and reads
    // neither a force's per-particle scale nor its curve over life, so the
    // strength has to be in the force itself or it is silently lost. The
    // control is one number there (ControlDef::one_number), so the high end
    // is the strength, and nothing random is promised.
    [[nodiscard]] float world_strength(const Block& block, std::string_view control) const {
        return get(block, control).y;
    }

    void add(const Block& block) {
        const BlockDef* def = find_block_def(block.kind);
        if (def == nullptr) {
            error_ = "Studio does not know the block '" + block.kind + "'";
            return;
        }
        if (def->unique) {
            if (!unique_.insert(block.kind).second) {
                error_ = "A layer can have only one " + std::string{def->title} + " block";
                return;
            }
        }
        // The palette and the picker never offer a block outside the
        // layer's kind, and a drop is refused, so one here came from a
        // hand-edited page. It is reported rather than quietly dropped.
        if (block.kind != BlockKind::Module && !fits(def->layers, layer_.kind)) {
            std::string kind_words{layer_kind_name(layer_.kind)};
            kind_words[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(kind_words[0])));
            error_ = std::string{def->title} + " does not belong in a " + kind_words + " layer";
            return;
        }
        const std::optional<BlockKind> kind = block_kind_from(block.kind);
        if (!kind) {
            return;
        }
        switch (*kind) {
        case BlockKind::Stream:
            compile_stream(block);
            break;
        case BlockKind::Burst:
            compile_burst(block);
            break;
        case BlockKind::Shape:
            compile_shape(block);
            break;
        case BlockKind::Launch:
            compile_launch(block);
            break;
        case BlockKind::HitPuffs:
        case BlockKind::HitSparks:
            compile_hits(block);
            break;
        case BlockKind::Trail:
            compile_trail(block);
            break;
        case BlockKind::Stirred:
            compile_stirred(block);
            break;
        case BlockKind::Heat:
            compile_heat(block);
            break;
        case BlockKind::Gravity:
            compile_gravity(block);
            break;
        case BlockKind::Rise:
            compile_rise(block);
            break;
        case BlockKind::Wind:
            compile_wind(block);
            break;
        case BlockKind::Turbulence:
            compile_turbulence(block);
            break;
        case BlockKind::DrawIn:
            compile_draw_in(block);
            break;
        case BlockKind::Spin:
            compile_spin(block);
            break;
        case BlockKind::Drag:
            compile_drag(block);
            break;
        case BlockKind::Swirl:
            compile_swirl(block);
            break;
        case BlockKind::Attract:
            compile_attract(block);
            break;
        case BlockKind::Appearance:
            compile_appearance(block);
            break;
        case BlockKind::Color:
            compile_color(block);
            break;
        case BlockKind::Size:
            compile_size(block);
            break;
        case BlockKind::Fade:
            compile_fade(block);
            break;
        case BlockKind::Ground:
            compile_ground(block);
            break;
        case BlockKind::PassesThrough:
            // Read where the page's world is placed: it takes away rather
            // than adds.
            break;
        case BlockKind::Explosion:
        case BlockKind::Push:
            // The page's, not the layer's: compile() lists it as a blast and
            // every layer answers it where the page's world is placed.
            break;
        case BlockKind::Sphere:
            compile_sphere(block);
            break;
        case BlockKind::Box:
            compile_box(block);
            break;
        case BlockKind::MeshCollider:
            compile_mesh_collider(block);
            break;
        case BlockKind::Tumble:
            compile_tumble(block);
            break;
        case BlockKind::Object:
            rigid_.object(block);
            break;
        case BlockKind::Break:
            rigid_.breaking(block);
            break;
        case BlockKind::Part:
            rigid_.part(block);
            break;
        case BlockKind::Light:
            compile_light(block);
            break;
        case BlockKind::SmokeGrid:
            // The page's: listed on the effect by the compiler
            // (page_volumes), never a module of this emitter.
            break;
        case BlockKind::Module:
            compile_module(block);
            break;
        case BlockKind::Physics:
            // A rigid layer's world is the layer's own and placed by build(); it is never a
            // block on the list.
            break;
        }
    }

    void compile_stream(const Block& block) {
        const Uid uid = block.uid;
        place_spawn(uid, SpawnRate{.id = id(block, 1U),
                                   .rate = input(block, "rate", number_of(block, "rate"), 1.0F),
                                   .body = rigid_.born_as(block)});
    }

    void compile_burst(const Block& block) {
        const Uid uid = block.uid;
        place_spawn(uid, SpawnBurst{.id = id(block, 1U),
                                    .time = number_of(block, "time"),
                                    .count = static_cast<std::uint32_t>(std::lround(number_of(block, "count"))),
                                    .interval = number_of(block, "every"),
                                    .cycles = number_of(block, "every") > 0.0F
                                                  ? static_cast<std::uint32_t>(std::lround(number_of(block, "repeats")))
                                                  : 1U,
                                    .body = rigid_.born_as(block)});
    }

    void compile_shape(const Block& block) {
        const Uid uid = block.uid;
        const float s = s_;
        const std::string shape = choice_of(block, "shape");
        const Value radius = get(block, "size");
        const Value box = get(block, "box");
        ShapeLocation location{.id = id(block, 1U)};
        location.center = to_vec3(get(block, "offset")) * s;
        std::tie(location.radius_min, location.radius_max) =
            range_input(block, "size", radius.x * s, radius.y * s, s);
        location.shape = shape == "point"                    ? Shape::Point
                         : shape == "box"                    ? Shape::Box
                         : shape == "disc" || shape == "ring" ? Shape::Disc
                                                              : Shape::Sphere;
        location.surface_only = shape == "shell" || shape == "ring";
        location.size = Vec3{std::fabs(box.x), std::fabs(box.y), std::fabs(box.z)} * s;
        place_init(SlotShape, uid, location);
    }

    void compile_launch(const Block& block) {
        const Uid uid = block.uid;
        const float s = s_;
        const std::string direction = choice_of(block, "direction");
        const Value speed = get(block, "speed");
        Velocity velocity{.id = id(block, 1U)};
        const Vec3 up{0.0F, 1.0F, 0.0F};
        velocity.direction = direction == "down"     ? Vec3{0.0F, -1.0F, 0.0F}
                             : direction == "side"   ? Vec3{1.0F, 0.0F, 0.0F}
                             : direction == "custom" ? normalized(to_vec3(get(block, "custom")), up)
                                                     : up;
        if (direction == "custom") {
            if (const Published* published = publication(block, "custom")) {
                velocity.direction = declare(published->name, ParameterType::Vec3, to_value(velocity.direction.value),
                                             published->low, published->high, control_timing(block.kind, "custom"));
            }
        }
        std::tie(velocity.speed_min, velocity.speed_max) = range_input(block, "speed", speed.x * s, speed.y * s, s);
        if (direction == "out") {
            velocity.spread = 180.0F;
        } else {
            const Value spread = get(block, "spread");
            std::tie(velocity.spread_min, velocity.spread) =
                range_input(block, "spread", std::clamp(spread.x, 0.0F, 180.0F), std::clamp(spread.y, 0.0F, 180.0F),
                            1.0F);
        }
        place_init(SlotVelocity, uid, velocity);
    }

    void compile_gravity(const Block& block) {
        const Uid uid = block.uid;
        const float s = s_;
        // A share of the page's gravity: 1 is the page's, whatever that is.
        const float pull = (physics_ ? world_strength(block, "strength") : 1.0F) * world_.gravity;
        Gravity gravity{.id = id(block, 1U), .acceleration = Vec3{0.0F, -earth_gravity * s * pull, 0.0F}};
        if (!physics_) {
            scale_by(gravity, block, "strength");
        }
        place_force(SlotGravity, uid, gravity);
    }

    void compile_rise(const Block& block) {
        const Uid uid = block.uid;
        const float s = s_;
        const std::string over_life = choice_of(block, "over_life");
        const float lift = physics_ ? world_strength(block, "strength") : 1.0F;
        Gravity rise{.id = id(block, 1U),
                     .acceleration = Vec3{0.0F, s * lift, 0.0F},
                     .over_life = over_life == "cools"    ? std::vector<CurveKey>{{0.0F, 1.6F}, {1.0F, 0.2F}}
                                  : over_life == "builds" ? std::vector<CurveKey>{{0.0F, 0.2F}, {1.0F, 1.6F}}
                                                          : std::vector<CurveKey>{{0.0F, 1.0F}, {1.0F, 1.0F}}};
        if (!physics_) {
            scale_by(rise, block, "strength");
        }
        place_force(SlotRise, uid, rise);
    }

    void compile_wind(const Block& block) {
        const Uid uid = block.uid;
        const float s = s_;
        const float heading = number_of(block, "heading") * 3.14159265F / 180.0F;
        const float push = physics_ ? world_strength(block, "strength") : 1.0F;
        Gravity wind{.id = id(block, 1U),
                     .acceleration = Vec3{std::cos(heading) * s * push, 0.0F, -std::sin(heading) * s * push}};
        if (!physics_) {
            scale_by(wind, block, "strength");
        }
        place_force(SlotWind, uid, wind);
    }

    void compile_turbulence(const Block& block) {
        const Uid uid = block.uid;
        const float s = s_;
        const float size = std::max(number_of(block, "size"), 0.01F) * s;
        CurlNoise noise{.id = id(block, 1U),
                        .strength = s,
                        .frequency = 1.0F / size,
                        .evolution = input(block, "churn", number_of(block, "churn") * s, s),
                        .seed = hash_mix(uid, static_cast<std::uint32_t>(number_of(block, "seed"))),
                        .drift = Vec3{0.0F, number_of(block, "roll") * s, 0.0F},
                        .over_life = turbulence_curve(choice_of(block, "over_life"))};
        scale_by(noise, block, "strength");
        place_force(SlotTurbulence, uid, noise);
    }

    void compile_draw_in(const Block& block) {
        const Uid uid = block.uid;
        const float s = s_;
        Converge converge{.id = id(block, 1U),
                          .center = to_vec3(get(block, "center")) * s,
                          .strength = 1.0F,
                          .height = number_of(block, "height") * s};
        scale_by(converge, block, "strength");
        place_force(SlotDrawIn, uid, converge);
    }

    void compile_spin(const Block& block) {
        const Uid uid = block.uid;
        const Value speed = get(block, "speed");
        place_init(SlotSpin, uid,
                   InitSpin{.id = id(block, 1U),
                            .speed_min = std::min(speed.x, speed.y),
                            .speed_max = std::max(speed.x, speed.y),
                            .both_ways = toggle_of(block, "both_ways"),
                            .drag = number_of(block, "slows")});
    }

    void compile_drag(const Block& block) {
        const Uid uid = block.uid;
        // In a rigid layer the amount is the body's linear damping, so
        // it is the coefficient; the engine never reads the scale.
        Drag drag{.id = id(block, 1U),
                  .coefficient = physics_ ? FloatInput{world_strength(block, "amount")} : FloatInput{1.0F}};
        if (!physics_) {
            scale_by(drag, block, "amount");
        }
        place_force(SlotDrag, uid, drag);
    }

    void compile_swirl(const Block& block) {
        const Uid uid = block.uid;
        const float s = s_;
        Vortex vortex{.id = id(block, 1U),
                      .center = to_vec3(get(block, "center")) * s,
                      .axis = choice_of(block, "axis") == "horizontal" ? Vec3{1.0F, 0.0F, 0.0F}
                                                                       : Vec3{0.0F, 1.0F, 0.0F},
                      .strength = s};
        scale_by(vortex, block, "strength");
        place_force(SlotSwirl, uid, vortex);
    }

    void compile_attract(const Block& block) {
        const Uid uid = block.uid;
        const float s = s_;
        Attractor attractor{.id = id(block, 1U),
                            .position = to_vec3(get(block, "point")) * s,
                            .strength = s,
                            .radius = input(block, "reach", number_of(block, "reach") * s, s)};
        scale_by(attractor, block, "strength");
        place_force(SlotAttract, uid, attractor);
    }

    void compile_fade(const Block& block) {
        const Uid uid = block.uid;
        const float in = number_of(block, "in") / 100.0F;
        const float out = number_of(block, "out") / 100.0F;
        color_life_.push_back([in, out](float t) {
            const float a = (in > 0.0F ? smoothstep(0.0F, in, t) : 1.0F) *
                            (out > 0.0F ? 1.0F - smoothstep(1.0F - out, 1.0F, t) : 1.0F);
            return Color{1.0F, 1.0F, 1.0F, a};
        });
        life_origins_.push_back(uid);
    }

    void compile_ground(const Block& block) {
        const Uid uid = block.uid;
        const float s = s_;
        const std::string response = choice_of(block, "response");
        PlaneCollision plane{.id = id(block, 1U)};
        plane.offset = number_of(block, "height") * s;
        plane.restitution = response == "bounce" ? input(block, "bounce", number_of(block, "bounce"), 1.0F) : FloatInput{0.0F};
        plane.friction = response == "stick" ? FloatInput{1.0F} : input(block, "friction", number_of(block, "friction"), 1.0F);
        plane.kill = response == "vanish";
        place_after(SlotCollision, uid, plane);
    }

    void compile_sphere(const Block& block) {
        const Uid uid = block.uid;
        const float s = s_;
        place_after(SlotCollision, uid,
                    SphereCollision{.id = id(block, 1U),
                                    .center = to_vec3(get(block, "center")) * s,
                                    .radius = input(block, "radius", number_of(block, "radius") * s, s),
                                    .inside = toggle_of(block, "inside"),
                                    .restitution = input(block, "bounce", number_of(block, "bounce"), 1.0F),
                                    .friction = input(block, "friction", number_of(block, "friction"), 1.0F),
                                    .kill = toggle_of(block, "vanish")});
    }

    void compile_box(const Block& block) {
        const Uid uid = block.uid;
        const float s = s_;
        const Vec3 extent = to_vec3(get(block, "size"));
        place_after(SlotCollision, uid,
                    BoxCollision{.id = id(block, 1U),
                                 .center = to_vec3(get(block, "center")) * s,
                                 .half_size = Vec3{std::fabs(extent.x), std::fabs(extent.y), std::fabs(extent.z)} *
                                              (0.5F * s),
                                 .inside = toggle_of(block, "inside"),
                                 .restitution = input(block, "bounce", number_of(block, "bounce"), 1.0F),
                                 .friction = input(block, "friction", number_of(block, "friction"), 1.0F),
                                 .kill = toggle_of(block, "vanish")});
    }

    void compile_tumble(const Block& block) {
        const Uid uid = block.uid;
        const float s = s_;
        const Value spin = get(block, "spin");
        InitTumble tumble{.id = id(block, 1U)};
        std::tie(tumble.speed_min, tumble.speed_max) = range_input(block, "spin", spin.x, spin.y, 1.0F);
        place_init(SlotTumble, uid, tumble);
        if (physics_) {
            return;
        }
        place_after(SlotRigid, uid,
                    RigidBody{.id = id(block, 2U),
                              .angular_drag = input(block, "damping", number_of(block, "damping"), 1.0F),
                              .contact_scale = number_of(block, "contact"),
                              .rest_speed = input(block, "settle", number_of(block, "settle") * s, s),
                              .roundness = input(block, "roundness", number_of(block, "roundness"), 1.0F)});
    }

    // The control's value as a runtime input: bound to its published
    // parameter, else the constant. `factor` takes the published range into
    // the units the value is in (page scale for distances).
    // What the control publishes, or null where it publishes nothing: on a
    // baked layer, a bake-time control is held where it is set instead, and
    // the report says so.
    const Published* publication(const Block& block, std::string_view control) {
        const Published* published = block.publication(control);
        if (published == nullptr || !layer_.baked || control_timing(block.kind, control) != ParameterTiming::Bake) {
            return published;
        }
        if (std::none_of(held_.begin(), held_.end(), [&](const LayerReport::Held& h) {
                return h.block == block.uid && h.control == control;
            })) {
            held_.push_back(LayerReport::Held{block.uid, std::string{control}, published->name});
        }
        return nullptr;
    }

    FloatInput input(const Block& block, std::string_view control, float value, float factor) {
        const Published* published = publication(block, control);
        if (published == nullptr) {
            return value;
        }
        return declare(published->name, ParameterType::Float, to_value(value), published->low * factor,
                       published->high * factor, control_timing(block.kind, control));
    }

    std::pair<FloatInput, FloatInput> range_input(const Block& block, std::string_view control, float low, float high,
                                                  float factor) {
        const Published* published = publication(block, control);
        if (published == nullptr) {
            return {low, high};
        }
        const float from = published->low * factor;
        const float to = published->high * factor;
        const ParameterTiming timing = control_timing(block.kind, control);
        return {declare(published->name + "_min", ParameterType::Float, to_value(low), from, to, timing),
                declare(published->name + "_max", ParameterType::Float, to_value(high), from, to, timing)};
    }

    ColorInput color_input(const Block& block, std::string_view control, Color value) {
        const Published* published = publication(block, control);
        if (published == nullptr) {
            return value;
        }
        return declare(published->name, ParameterType::Color, to_value(value), published->low, published->high,
                       control_timing(block.kind, control));
    }

    // Declares (or joins) a parameter of the effect. Two controls may share
    // a name and so one parameter, if they publish the same kind of value at
    // the same time; the range covers both.
    Binding declare(const std::string& name, ParameterType type, ParameterValue value, float low, float high,
                    ParameterTiming timing) {
        if (low > high) {
            std::swap(low, high);
        }
        if (!valid_parameter_name(name)) {
            error_ = "A published control needs a name of letters, digits and underscores";
            return bind(name);
        }
        const std::size_t components = type == ParameterType::Float ? 1U : type == ParameterType::Vec3 ? 3U : 4U;
        for (std::size_t i = 0U; i < components; ++i) {
            value[i] = std::clamp(value[i], low, high);
        }
        if (std::find(published_.begin(), published_.end(), name) == published_.end()) {
            published_.push_back(name);
        }
        for (ParameterDesc& existing : parameters_) {
            if (existing.name != name) {
                continue;
            }
            if (existing.type != type) {
                error_ = "'" + name + "' is published twice as different kinds of value";
                return bind(name);
            }
            if (existing.timing != timing) {
                error_ = "'" + name + "' is published by a control the simulation reads and by one only drawing "
                         "reads; give them different names";
                return bind(name);
            }
            existing.min = std::min(existing.min, low);
            existing.max = std::max(existing.max, high);
            return bind(name);
        }
        parameters_.push_back(ParameterDesc{name, type, value, low, high, timing});
        return bind(name);
    }

    static std::vector<CurveKey> turbulence_curve(const std::string& over_life) {
        if (over_life == "grows") {
            return {{0.0F, 0.15F}, {0.5F, 1.0F}, {1.0F, 1.4F}};
        }
        if (over_life == "settles") {
            return {{0.0F, 1.4F}, {1.0F, 0.2F}};
        }
        return {{0.0F, 1.0F}, {1.0F, 1.0F}};
    }

    // Lights ride on the renderer: Appearance keeps whatever this sets.
    void compile_light(const Block& block) {
        SpriteRenderer& r = renderer_;
        r.light_count = static_cast<std::uint32_t>(std::lround(number_of(block, "count")));
        const Value intensity = get(block, "intensity");
        const Value reach = get(block, "reach");
        r.light_intensity_min = std::max(std::min(intensity.x, intensity.y), 0.0F);
        r.light_intensity_max = std::max(std::max(intensity.x, intensity.y), 0.0F);
        r.light_range_min = std::max(std::min(reach.x, reach.y), 0.01F) * s_;
        r.light_range_max = std::max(std::max(reach.x, reach.y), 0.01F) * s_;
        const std::string over_life = choice_of(block, "over_life");
        if (over_life == "fade") {
            r.light_curve = {{0.0F, 0.0F}, {0.15F, 1.0F}, {0.6F, 1.0F}, {1.0F, 0.0F}};
        } else if (over_life == "flare") {
            r.light_curve = {{0.0F, 0.0F}, {0.04F, 1.0F}, {0.3F, 0.35F}, {1.0F, 0.0F}};
        } else if (over_life == "out") {
            r.light_curve = {{0.0F, 1.0F}, {1.0F, 0.0F}};
        } else {
            r.light_curve = {{0.0F, 1.0F}, {1.0F, 1.0F}};
        }
        const std::string mode = choice_of(block, "color_mode");
        r.light_own_color = mode == "own" || mode == "shift";
        const Value own = get(block, "light_color");
        const Color start{own.x, own.y, own.z, 1.0F};
        Color end = start;
        if (mode == "shift") {
            const Value to = get(block, "end_color");
            end = Color{to.x, to.y, to.z, 1.0F};
        }
        r.light_colors = {{0.0F, start}, {1.0F, end}};
    }

    void compile_appearance(const Block& block) {
        saw_appearance_ = true;
        const std::string material = choice_of(block, "material");
        SpriteRenderer r = renderer_; // keeps the light fields
        r.blend = material == "glow" || material == "trail" ? BlendMode::Additive : BlendMode::Alpha;
        r.sort = (material == "smoke" || material == "solid") && toggle_of(block, "sort");
        r.soft_distance = material == "model" ? 0.0F : number_of(block, "soft") * s_;
        if (material == "model") {
            r.geometry = Geometry::Mesh;
            r.mesh = choice_of(block, "mesh");
            if (r.mesh == "file") {
                const std::string path = file_of(block, "mesh_file");
                // No model picked yet: slabs, until there is.
                r.mesh = path.empty() ? std::string{"slab"} : request(AssetRequest::Kind::Mesh, block, path);
            }
            r.texture.clear();
            r.facing = Facing::Camera;
            r.stretch = 0.0F;
            r.flipbook_columns = r.flipbook_rows = 1U;
            r.flipbook_blend = false;
        } else {
            LibraryTexture own{};
            const LibraryTexture* texture = library_texture(choice_of(block, "texture"));
            std::string own_entry;
            if (choice_of(block, "texture") == "file") {
                texture = nullptr;
                if (const std::string path = file_of(block, "texture_file"); !path.empty()) {
                    own_entry = request(AssetRequest::Kind::Texture, block, path);
                    own.entry = own_entry;
                    own.columns = static_cast<std::uint32_t>(std::clamp(std::lround(number_of(block, "columns")), 1L, 64L));
                    own.rows = static_cast<std::uint32_t>(std::clamp(std::lround(number_of(block, "rows")), 1L, 64L));
                    texture = &own;
                }
            }
            const bool flipbook = texture != nullptr && texture->columns * texture->rows > 1U;
            r.geometry = material == "trail" ? Geometry::Ribbon : Geometry::Sprites;
            r.mesh.clear();
            // A ribbon runs one picture along its length, so it cannot play
            // a flipbook; it keeps the soft disc instead.
            const bool usable = texture != nullptr && !(material == "trail" && flipbook);
            r.texture = usable ? std::string{texture->entry} : std::string{};
            r.flipbook_columns = usable ? texture->columns : 1U;
            r.flipbook_rows = usable ? texture->rows : 1U;
            r.flipbook_blend = usable && flipbook && toggle_of(block, "blend");
            const bool streak = material != "trail" && choice_of(block, "facing") == "motion";
            r.facing = streak ? Facing::Velocity : Facing::Camera;
            r.stretch = streak ? number_of(block, "stretch") : 0.0F;
            if (material != "trail" && toggle_of(block, "spin")) {
                place_init(SlotRotation, block.uid, InitRotation{.id = id(block, 1U)});
            }
        }
        // The scene's light on it. Six-way maps need a picture from a file
        // and its second map; without both, the particles are lit plainly.
        const std::string lighting = choice_of(block, "lighting");
        r.lighting = Lighting::Unlit;
        r.lightmap.clear();
        if (material != "trail" && lighting == "lit") {
            r.lighting = Lighting::Lit;
        } else if (material != "trail" && material != "model" && lighting == "six_way") {
            const std::string map = choice_of(block, "texture") == "file" && !r.texture.empty()
                                        ? file_of(block, "lightmap_file")
                                        : std::string{};
            if (!map.empty()) {
                r.lighting = Lighting::SixWay;
                r.lightmap = request(AssetRequest::Kind::Texture, block, map);
            } else {
                r.lighting = Lighting::Lit;
            }
        }
        r.curvature = std::clamp(number_of(block, "curvature"), 0.0F, 1.0F);
        r.wrap = std::clamp(number_of(block, "wrap"), 0.0F, 1.0F);
        r.ambient = std::max(number_of(block, "ambient"), 0.0F);
        r.shadowed = toggle_of(block, "shadowed");
        // What the host multiplies the layer by as it draws it.
        const Value tint = get(block, "tint");
        r.tint = color_input(block, "tint", Color{tint.x, tint.y, tint.z, 1.0F});
        r.brightness = input(block, "brightness", number_of(block, "brightness"), 1.0F);
        r.opacity = input(block, "opacity", number_of(block, "opacity"), 1.0F);
        r.size_scale = input(block, "size_scale", number_of(block, "size_scale"), 1.0F);
        renderer_ = std::move(r);
    }

    void compile_mesh_collider(const Block& block) {
        const std::string path = file_of(block, "file");
        if (path.empty()) {
            return; // nothing to collide with until a model is picked
        }
        const std::string detail = choice_of(block, "detail");
        const std::uint32_t cells = detail == "coarse" ? 40U : detail == "fine" ? 120U : 72U;
        const std::string mode = choice_of(block, "solid");
        const ColliderSolid solid = mode == "solid"     ? ColliderSolid::Solid
                                    : mode == "surface" ? ColliderSolid::Surface
                                                        : ColliderSolid::Auto;
        const float scale = std::max(number_of(block, "scale"), 0.001F) * s_;
        const std::string field = request(AssetRequest::Kind::Field, block, path, scale, cells, solid);
        place_after(SlotCollision, block.uid,
                    SdfCollision{.id = id(block, 1U),
                                 .field = field,
                                 .offset = to_vec3(get(block, "position")) * s_,
                                 .inside = toggle_of(block, "inside"),
                                 .restitution = input(block, "bounce", number_of(block, "bounce"), 1.0F),
                                 .friction = input(block, "friction", number_of(block, "friction"), 1.0F),
                                 .kill = toggle_of(block, "vanish")});
    }

    void compile_color(const Block& block) {
        const Value base = get(block, "color");
        // The starting colour carries the brightest end; each particle takes
        // its own share of that down to the dimmest.
        const Value bright = get(block, "brightness");
        const float brightness = bright.y;
        const float opacity = number_of(block, "opacity");
        const Color a{base.x * brightness, base.y * brightness, base.z * brightness, opacity};
        Color b = a;
        if (toggle_of(block, "mix")) {
            const Value second = get(block, "second");
            b = Color{second.x * brightness, second.y * brightness, second.z * brightness, opacity};
        }
        // A published colour is the whole starting colour, brightness and
        // opacity included; without a second colour both ends follow it.
        const ColorInput start = color_input(block, "color", a);
        place_init(SlotColor, block.uid,
                   InitColor{.id = id(block, 1U),
                             .a = start,
                             .b = toggle_of(block, "mix") ? ColorInput{b} : start,
                             .brightness_min = brightness > 0.0F ? std::clamp(bright.x / brightness, 0.0F, 1.0F) : 1.0F,
                             .brightness_max = 1.0F});

        const std::string over = choice_of(block, "over_life");
        if (over == "fire") {
            static constexpr std::array<std::array<float, 4>, 6> fire = {{{0.0F, 1.0F, 1.0F, 1.0F},
                                                                          {0.12F, 1.0F, 0.88F, 0.62F},
                                                                          {0.35F, 1.0F, 0.5F, 0.16F},
                                                                          {0.62F, 0.75F, 0.18F, 0.04F},
                                                                          {0.85F, 0.3F, 0.06F, 0.02F},
                                                                          {1.0F, 0.12F, 0.03F, 0.02F}}};
            color_life_.push_back([](float t) { return ramp(fire, t); });
        } else if (over == "cool") {
            static constexpr std::array<std::array<float, 4>, 3> cool = {
                {{0.0F, 1.0F, 1.0F, 1.0F}, {0.35F, 0.7F, 0.82F, 1.0F}, {1.0F, 0.22F, 0.3F, 0.55F}}};
            color_life_.push_back([](float t) { return ramp(cool, t); });
        } else if (over == "darken") {
            color_life_.push_back([](float t) {
                const float k = 1.0F - 0.78F * t;
                return Color{k, k, k, 1.0F};
            });
        } else if (over == "shift") {
            const Value end = get(block, "end_color");
            // A multiplier that takes the start colour to the end colour.
            const auto ratio = [](float to, float from) { return std::min(to / std::max(from, 0.02F), 20.0F); };
            const Color target{ratio(end.x, base.x), ratio(end.y, base.y), ratio(end.z, base.z), 1.0F};
            color_life_.push_back([target](float t) {
                return Color{1.0F + (target.r - 1.0F) * t, 1.0F + (target.g - 1.0F) * t, 1.0F + (target.b - 1.0F) * t,
                             1.0F};
            });
        }
        if (over != "constant") {
            life_origins_.push_back(block.uid);
        }
    }

    void compile_size(const Block& block) {
        const Value size = get(block, "size");
        InitSize init{.id = id(block, 1U)};
        std::tie(init.min, init.max) = range_input(block, "size", size.x * s_, size.y * s_, s_);
        // What an Object's own size is measured against: its kind's scale
        // multiplies the size every body is born at.
        size_min_ = std::max(size.x * s_, 1.0e-4F);
        place_init(SlotSize, block.uid, init);
        const std::string over = choice_of(block, "over_life");
        const float change = number_of(block, "change");
        if (over == "grow") {
            size_life_.push_back([change](float t) { return 1.0F + (change - 1.0F) * (1.0F - (1.0F - t) * (1.0F - t)); });
        } else if (over == "swell") {
            size_life_.push_back([change](float t) { return 1.0F + (change - 1.0F) * smoothstep(0.0F, 1.0F, t); });
        } else if (over == "shrink") {
            size_life_.push_back([](float t) { return std::pow(std::max(1.0F - t, 0.0F), 0.8F); });
        } else if (over == "pop") {
            size_life_.push_back([](float t) { return smoothstep(0.0F, 0.12F, t) * (1.0F - smoothstep(0.35F, 1.0F, t)); });
        }
        if (over != "constant") {
            life_origins_.push_back(block.uid);
        }
    }

    void compile_module(const Block& block) {
        if (!block.raw) {
            error_ = "This Module block holds no module";
            return;
        }
        RawModule module = *block.raw;
        // A raw block with a seed value deals its particles other values,
        // as a typed block's seed does; without one its id is the plain one.
        set_raw_id(module, id(block, 1U));
        if (const auto* spawn = std::get_if<SpawnModule>(&module)) {
            spawn_.push_back(Placed{0, ModuleLine{block.uid, RawModule{*spawn}}});
        } else if (const auto* init = std::get_if<InitModule>(&module)) {
            init_.push_back(Placed{SlotRawInit, ModuleLine{block.uid, RawModule{*init}}});
        } else {
            const UpdateModule& update = std::get<UpdateModule>(module);
            if (std::holds_alternative<SdfCollision>(update)) {
                error_ = "Distance field collision needs a baked field, which Studio cannot make yet";
                return;
            }
            if (std::holds_alternative<ColorOverLife>(update)) {
                raw_color_life_ = ModuleLine{block.uid, module};
            } else if (std::holds_alternative<SizeOverLife>(update)) {
                raw_size_life_ = ModuleLine{block.uid, module};
            } else if (is_force(update)) {
                forces_.push_back(Placed{SlotRawForce, ModuleLine{block.uid, module}});
            } else if (std::holds_alternative<RigidBody>(update) || std::holds_alternative<RigidPhysics>(update)) {
                after_.push_back(Placed{SlotRigid, ModuleLine{block.uid, module}});
            } else {
                after_.push_back(Placed{SlotRawCollision, ModuleLine{block.uid, module}});
            }
        }
    }

    // One colour-over-life and one size-over-life module from everything the
    // blocks asked for; a raw one of either replaces the merged one.
    void finish_over_life() {
        if (raw_color_life_) {
            life_.push_back(Placed{0, std::move(*raw_color_life_)});
        } else if (!color_life_.empty()) {
            ColorOverLife module{.id = module_id(layer_.uid, 2U), .keys = {}};
            for (std::size_t i = 0U; i < over_life_keys; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(over_life_keys - 1U);
                Color product{1.0F, 1.0F, 1.0F, 1.0F};
                for (const auto& contribution : color_life_) {
                    const Color c = contribution(t);
                    product = Color{product.r * c.r, product.g * c.g, product.b * c.b, product.a * c.a};
                }
                module.keys.push_back(ColorKey{t, product});
            }
            life_.push_back(Placed{0, ModuleLine{layer_.uid, RawModule{UpdateModule{std::move(module)}}}});
        }
        if (raw_size_life_) {
            life_.push_back(Placed{1, std::move(*raw_size_life_)});
        } else if (!size_life_.empty()) {
            SizeOverLife module{.id = module_id(layer_.uid, 3U), .keys = {}};
            for (std::size_t i = 0U; i < over_life_keys; ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(over_life_keys - 1U);
                float product = 1.0F;
                for (const auto& contribution : size_life_) {
                    product *= contribution(t);
                }
                module.keys.push_back(CurveKey{t, product});
            }
            life_.push_back(Placed{1, ModuleLine{layer_.uid, RawModule{UpdateModule{std::move(module)}}}});
        }
    }

    RigidBuilder rigid_;
};

LayerBuilder::LayerBuilder(const Layer& layer, float scale, std::vector<ParameterDesc>& parameters, PageWorld world,
                           SolidWorld solids, std::vector<PageBlast> blasts)
    : impl_{std::make_unique<Impl>(layer, scale, parameters, world, std::move(solids), std::move(blasts))} {}

LayerBuilder::~LayerBuilder() = default;

std::optional<Emitter> LayerBuilder::build(LayerReport& report) {
    return impl_->build(report);
}

const std::vector<FractureCut>& LayerBuilder::cuts() const noexcept {
    return impl_->rigid().cuts();
}

std::uint64_t LayerBuilder::extra_for_breaking() const noexcept {
    return impl_->rigid().extra_for_breaking();
}

const std::vector<std::string>& LayerBuilder::published() const noexcept {
    return impl_->published();
}

const std::vector<LayerReport::Held>& LayerBuilder::held() const noexcept {
    return impl_->held();
}

const std::vector<AssetRequest>& LayerBuilder::requests() const noexcept {
    return impl_->requests();
}

} // namespace ajfx::studio
