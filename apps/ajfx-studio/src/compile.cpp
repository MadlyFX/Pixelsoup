#include "compile.hpp"

#include "catalog.hpp"
#include "document_io.hpp"
#include "library.hpp"
#include "raw_modules.hpp"

#include <ajfx/gpu/kernels.hpp>
#include <ajfx/random.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <span>
#include <tuple>

namespace ajfx::studio {
namespace {

constexpr float earth_gravity = 9.81F;
constexpr std::size_t over_life_keys = 17U;

// Module ids: sixteen a uid, so each block (and each layer, for what the
// compiler makes from it) owns its own run of ids.
constexpr ModuleId module_id(Uid uid, std::uint32_t slot) noexcept {
    return uid * 16U + slot;
}

// Where a module goes within its stage. Lower first; ties keep page order.
enum Slot : int {
    // init
    SlotLifetime = 0,
    SlotShape,
    SlotVelocity,
    SlotColor,
    SlotSize,
    SlotRotation,
    SlotSpin,
    SlotTumble,
    SlotRawInit,
    // update: forces
    SlotGravity = 0,
    SlotRise,
    SlotWind,
    SlotSwirl,
    SlotAttract,
    SlotDrawIn,
    SlotTurbulence,
    SlotRawForce,
    SlotDrag, // last: it slows what the forces gave
    // update: after integration
    SlotRigid = 0,
    SlotCollision,
    SlotRawCollision,
};

struct Placed final {
    int slot{};
    ModuleLine line;
};

Vec3 to_vec3(const Value& value) {
    return Vec3{value.x, value.y, value.z};
}

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

std::string emitter_name(const Layer& layer, std::set<std::string>& taken) {
    std::string base;
    for (const char c : layer.name) {
        const auto u = static_cast<unsigned char>(c);
        base += std::isalnum(u) != 0 ? static_cast<char>(std::tolower(u)) : '_';
    }
    if (base.empty()) {
        base = "layer";
    }
    std::string name = base;
    for (int n = 2; !taken.insert(name).second; ++n) {
        name = base + "_" + std::to_string(n);
    }
    return name;
}

class LayerBuilder final {
public:
    LayerBuilder(const Layer& layer, float scale, std::vector<ParameterDesc>& parameters)
        : layer_{layer}, s_{scale}, parameters_{parameters} {}

    // The parameters this layer's blocks publish.
    [[nodiscard]] const std::vector<std::string>& published() const noexcept { return published_; }
    // The artist's files the layer's blocks name.
    [[nodiscard]] const std::vector<AssetRequest>& requests() const noexcept { return requests_; }

    // Builds the emitter; on failure sets `error` and `error_block`.
    std::optional<Emitter> build(LayerReport& report) {
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
        Emitter emitter;
        emitter.seed = layer_.seed == 0U ? layer_.uid : hash_mix(layer_.uid, layer_.seed);
        emitter.capacity = max_emitter_capacity;

        const float life_min = std::max(layer_.life_min, 0.01F);
        place_init(SlotLifetime, layer_.uid,
                   Lifetime{.id = module_id(layer_.uid, 1U), .min = life_min,
                            .max = std::max(layer_.life_max, life_min)});
        finish_over_life();

        const auto by_slot = [](const Placed& a, const Placed& b) { return a.slot < b.slot; };
        std::stable_sort(init_.begin(), init_.end(), by_slot);
        std::stable_sort(forces_.begin(), forces_.end(), by_slot);
        std::stable_sort(after_.begin(), after_.end(), by_slot);
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
    // A block's module id. Random streams are keyed by id, so a block's seed
    // picks another id to deal its particles other values. Seeded ids have
    // the top bit set, apart from every plain uid * 16 + slot, and the rare
    // clash of two seeded ones moves on to the next free id.
    ModuleId id(const Block& block, std::uint32_t slot) {
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

    // The two ends of a force block's strength, as the factor each particle
    // takes the force by (see the forces in ajfx/modules.hpp).
    template <class Force>
    void scale_by(Force& force, const Block& block, std::string_view control) {
        const Value strength = get(block, control);
        std::tie(force.scale_min, force.scale_max) = range_input(block, control, strength.x, strength.y, 1.0F);
    }

    template <class M>
    void place_init(int slot, Uid origin, M module) {
        init_.push_back(Placed{slot, ModuleLine{origin, RawModule{InitModule{std::move(module)}}}});
    }
    template <class M>
    void place_force(int slot, Uid origin, M module) {
        forces_.push_back(Placed{slot, ModuleLine{origin, RawModule{UpdateModule{std::move(module)}}}});
    }
    template <class M>
    void place_after(int slot, Uid origin, M module) {
        after_.push_back(Placed{slot, ModuleLine{origin, RawModule{UpdateModule{std::move(module)}}}});
    }
    template <class M>
    void place_spawn(Uid origin, M module) {
        spawn_.push_back(Placed{0, ModuleLine{origin, RawModule{SpawnModule{std::move(module)}}}});
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
        const Uid uid = block.uid;
        const std::string_view kind = block.kind;
        const float s = s_;

        if (kind == "stream") {
            place_spawn(uid, SpawnRate{.id = id(block, 1U), .rate = input(block, "rate", number_of(block, "rate"), 1.0F)});
        } else if (kind == "burst") {
            place_spawn(uid, SpawnBurst{.id = id(block, 1U),
                                        .time = number_of(block, "time"),
                                        .count = static_cast<std::uint32_t>(std::lround(number_of(block, "count"))),
                                        .interval = number_of(block, "every"),
                                        .cycles = number_of(block, "every") > 0.0F
                                                      ? static_cast<std::uint32_t>(std::lround(number_of(block, "repeats")))
                                                      : 1U});
        } else if (kind == "shape") {
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
        } else if (kind == "launch") {
            const std::string direction = choice_of(block, "direction");
            const Value speed = get(block, "speed");
            Velocity velocity{.id = id(block, 1U)};
            const Vec3 up{0.0F, 1.0F, 0.0F};
            velocity.direction = direction == "down"     ? Vec3{0.0F, -1.0F, 0.0F}
                                 : direction == "side"   ? Vec3{1.0F, 0.0F, 0.0F}
                                 : direction == "custom" ? normalized(to_vec3(get(block, "custom")), up)
                                                         : up;
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
        } else if (kind == "gravity") {
            Gravity gravity{.id = id(block, 1U), .acceleration = Vec3{0.0F, -earth_gravity * s, 0.0F}};
            scale_by(gravity, block, "strength");
            place_force(SlotGravity, uid, gravity);
        } else if (kind == "rise") {
            const std::string over_life = choice_of(block, "over_life");
            Gravity rise{.id = id(block, 1U),
                         .acceleration = Vec3{0.0F, s, 0.0F},
                         .over_life = over_life == "cools"    ? std::vector<CurveKey>{{0.0F, 1.6F}, {1.0F, 0.2F}}
                                      : over_life == "builds" ? std::vector<CurveKey>{{0.0F, 0.2F}, {1.0F, 1.6F}}
                                                              : std::vector<CurveKey>{{0.0F, 1.0F}, {1.0F, 1.0F}}};
            scale_by(rise, block, "strength");
            place_force(SlotRise, uid, rise);
        } else if (kind == "wind") {
            const float heading = number_of(block, "heading") * 3.14159265F / 180.0F;
            Gravity wind{.id = id(block, 1U), .acceleration = Vec3{std::cos(heading) * s, 0.0F, -std::sin(heading) * s}};
            scale_by(wind, block, "strength");
            place_force(SlotWind, uid, wind);
        } else if (kind == "turbulence") {
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
        } else if (kind == "draw_in") {
            Converge converge{.id = id(block, 1U),
                              .center = to_vec3(get(block, "center")) * s,
                              .strength = 1.0F,
                              .height = number_of(block, "height") * s};
            scale_by(converge, block, "strength");
            place_force(SlotDrawIn, uid, converge);
        } else if (kind == "spin") {
            const Value speed = get(block, "speed");
            place_init(SlotSpin, uid,
                       InitSpin{.id = id(block, 1U),
                                .speed_min = std::min(speed.x, speed.y),
                                .speed_max = std::max(speed.x, speed.y),
                                .both_ways = toggle_of(block, "both_ways"),
                                .drag = number_of(block, "slows")});
        } else if (kind == "drag") {
            Drag drag{.id = id(block, 1U), .coefficient = 1.0F};
            scale_by(drag, block, "amount");
            place_force(SlotDrag, uid, drag);
        } else if (kind == "swirl") {
            Vortex vortex{.id = id(block, 1U),
                          .center = to_vec3(get(block, "center")) * s,
                          .axis = choice_of(block, "axis") == "horizontal" ? Vec3{1.0F, 0.0F, 0.0F}
                                                                           : Vec3{0.0F, 1.0F, 0.0F},
                          .strength = s};
            scale_by(vortex, block, "strength");
            place_force(SlotSwirl, uid, vortex);
        } else if (kind == "attract") {
            Attractor attractor{.id = id(block, 1U),
                                .position = to_vec3(get(block, "point")) * s,
                                .strength = s,
                                .radius = input(block, "reach", number_of(block, "reach") * s, s)};
            scale_by(attractor, block, "strength");
            place_force(SlotAttract, uid, attractor);
        } else if (kind == "appearance") {
            appearance(block);
        } else if (kind == "color") {
            color(block);
        } else if (kind == "size") {
            size(block);
        } else if (kind == "fade") {
            const float in = number_of(block, "in") / 100.0F;
            const float out = number_of(block, "out") / 100.0F;
            color_life_.push_back([in, out](float t) {
                const float a = (in > 0.0F ? smoothstep(0.0F, in, t) : 1.0F) *
                                (out > 0.0F ? 1.0F - smoothstep(1.0F - out, 1.0F, t) : 1.0F);
                return Color{1.0F, 1.0F, 1.0F, a};
            });
            life_origins_.push_back(uid);
        } else if (kind == "ground") {
            const std::string response = choice_of(block, "response");
            PlaneCollision plane{.id = id(block, 1U)};
            plane.offset = number_of(block, "height") * s;
            plane.restitution = response == "bounce" ? input(block, "bounce", number_of(block, "bounce"), 1.0F) : FloatInput{0.0F};
            plane.friction = response == "stick" ? FloatInput{1.0F} : input(block, "friction", number_of(block, "friction"), 1.0F);
            plane.kill = response == "vanish";
            place_after(SlotCollision, uid, plane);
        } else if (kind == "sphere") {
            place_after(SlotCollision, uid,
                        SphereCollision{.id = id(block, 1U),
                                        .center = to_vec3(get(block, "center")) * s,
                                        .radius = input(block, "radius", number_of(block, "radius") * s, s),
                                        .inside = toggle_of(block, "inside"),
                                        .restitution = input(block, "bounce", number_of(block, "bounce"), 1.0F),
                                        .friction = input(block, "friction", number_of(block, "friction"), 1.0F),
                                        .kill = toggle_of(block, "vanish")});
        } else if (kind == "box") {
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
        } else if (kind == "mesh_collider") {
            mesh_collider(block);
        } else if (kind == "tumble") {
            const Value spin = get(block, "spin");
            InitTumble tumble{.id = id(block, 1U)};
            std::tie(tumble.speed_min, tumble.speed_max) = range_input(block, "spin", spin.x, spin.y, 1.0F);
            place_init(SlotTumble, uid, tumble);
            place_after(SlotRigid, uid,
                        RigidBody{.id = id(block, 2U),
                                  .angular_drag = input(block, "damping", number_of(block, "damping"), 1.0F),
                                  .contact_scale = number_of(block, "contact"),
                                  .rest_speed = input(block, "settle", number_of(block, "settle") * s, s),
                                  .roundness = input(block, "roundness", number_of(block, "roundness"), 1.0F)});
        } else if (kind == "light") {
            light(block);
        } else if (kind == raw_block_kind) {
            raw(block);
        }
    }

    // The control's value as a runtime input: bound to its published
    // parameter, else the constant. `factor` takes the published range into
    // the units the value is in (page scale for distances).
    FloatInput input(const Block& block, std::string_view control, float value, float factor) {
        const Published* published = block.publication(control);
        if (published == nullptr) {
            return value;
        }
        return declare(published->name, ParameterType::Float, to_value(value), published->low * factor,
                       published->high * factor);
    }

    std::pair<FloatInput, FloatInput> range_input(const Block& block, std::string_view control, float low, float high,
                                                  float factor) {
        const Published* published = block.publication(control);
        if (published == nullptr) {
            return {low, high};
        }
        const float from = published->low * factor;
        const float to = published->high * factor;
        return {declare(published->name + "_min", ParameterType::Float, to_value(low), from, to),
                declare(published->name + "_max", ParameterType::Float, to_value(high), from, to)};
    }

    ColorInput color_input(const Block& block, std::string_view control, Color value) {
        const Published* published = block.publication(control);
        if (published == nullptr) {
            return value;
        }
        return declare(published->name, ParameterType::Color, to_value(value), published->low, published->high);
    }

    // Declares (or joins) a parameter of the effect. Two controls may share
    // a name and so one parameter, if they publish the same kind of value;
    // the range covers both.
    Binding declare(const std::string& name, ParameterType type, ParameterValue value, float low, float high) {
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
            existing.min = std::min(existing.min, low);
            existing.max = std::max(existing.max, high);
            return bind(name);
        }
        parameters_.push_back(ParameterDesc{name, type, value, low, high});
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
    void light(const Block& block) {
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

    void appearance(const Block& block) {
        const std::string material = choice_of(block, "material");
        SpriteRenderer r = renderer_; // keeps the light fields
        r.blend = material == "glow" || material == "trail" ? BlendMode::Additive : BlendMode::Alpha;
        r.sort = (material == "smoke" || material == "solid") && toggle_of(block, "sort");
        r.soft_distance = material == "debris" ? 0.0F : number_of(block, "soft") * s_;
        if (material == "debris") {
            r.geometry = Geometry::Mesh;
            r.mesh = choice_of(block, "mesh");
            if (r.mesh == "file") {
                const std::string path = file_of(block, "mesh_file");
                // No model picked yet: chips, until there is.
                r.mesh = path.empty() ? std::string{"chip"} : request(AssetRequest::Kind::Mesh, block, path);
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
        renderer_ = std::move(r);
    }

    // Names the package entry an artist's file goes in, and asks for it.
    std::string request(AssetRequest::Kind kind, const Block& block, const std::string& path, float scale = 1.0F,
                        std::uint32_t cells = 0U, ColliderSolid solid = ColliderSolid::Auto) {
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

    void mesh_collider(const Block& block) {
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

    void color(const Block& block) {
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

    void size(const Block& block) {
        const Value size = get(block, "size");
        InitSize init{.id = id(block, 1U)};
        std::tie(init.min, init.max) = range_input(block, "size", size.x * s_, size.y * s_, s_);
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

    void raw(const Block& block) {
        if (!block.raw) {
            error_ = "This Module block holds no module";
            return;
        }
        RawModule module = *block.raw;
        set_raw_id(module, module_id(block.uid, 1U));
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
            } else if (std::holds_alternative<RigidBody>(update)) {
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

    const Layer& layer_;
    float s_;
    std::vector<ParameterDesc>& parameters_;
    std::vector<std::string> published_;
    std::vector<AssetRequest> requests_;
    std::string error_;
    std::set<std::string> unique_;
    std::set<ModuleId> seeded_;
    std::vector<Placed> spawn_;
    std::vector<Placed> init_;
    std::vector<Placed> forces_;
    std::vector<Placed> after_;
    std::vector<Placed> life_;
    std::vector<std::function<Color(float)>> color_life_;
    std::vector<std::function<float(float)>> size_life_;
    std::vector<Uid> life_origins_;
    std::optional<ModuleLine> raw_color_life_;
    std::optional<ModuleLine> raw_size_life_;
    SpriteRenderer renderer_{};
};

// The block a validation message names ("... module 'drag' #37 ..."), if it
// belongs to the layer.
Uid block_named_in(const std::string& message, const Layer& layer) {
    const std::size_t hash = message.find(" #");
    if (hash == std::string::npos) {
        return 0U;
    }
    std::uint64_t id = 0U;
    for (std::size_t i = hash + 2U; i < message.size() && std::isdigit(static_cast<unsigned char>(message[i])) != 0;
         ++i) {
        id = id * 10U + static_cast<std::uint64_t>(message[i] - '0');
    }
    const Uid uid = static_cast<Uid>(id / 16U);
    for (const Block& block : layer.blocks) {
        if (block.uid == uid) {
            return uid;
        }
    }
    return 0U;
}

} // namespace

const LayerReport* Compiled::layer(Uid uid) const {
    for (const LayerReport& report : layers) {
        if (report.layer == uid) {
            return &report;
        }
    }
    return nullptr;
}

std::uint32_t Compiled::total_capacity() const {
    std::uint32_t total = 0U;
    for (const LayerReport& report : layers) {
        total += report.included ? report.capacity : 0U;
    }
    return total;
}

Compiled compile(const Document& document, Purpose purpose) {
    Compiled compiled;
    Effect effect;
    effect.name = document.name.empty() ? std::string{"effect"} : document.name;
    effect.simulation_rate = 60U;
    const float scale = std::isfinite(document.scale) && document.scale > 0.0F ? document.scale : 1.0F;
    const bool any_solo =
        purpose == Purpose::Preview &&
        std::any_of(document.layers.begin(), document.layers.end(), [](const Layer& l) { return l.solo && l.enabled; });

    const auto wanted = [&](const Layer& layer) { return layer.enabled && (!any_solo || layer.solo); };

    // Every published range first: a parameter two layers share has to be
    // sized for both before either layer's budget is worked out.
    std::vector<ParameterDesc> parameters;
    for (const Layer& layer : document.layers) {
        if (wanted(layer)) {
            LayerReport scratch;
            static_cast<void>(LayerBuilder{layer, scale, parameters}.build(scratch));
        }
    }
    std::set<std::string> used;

    std::set<std::string> names;
    for (const Layer& layer : document.layers) {
        LayerReport report;
        report.layer = layer.uid;
        report.emitter = emitter_name(layer, names);
        // A layer that will not play still compiles, against its own copy,
        // so what it publishes never widens what the playing layers see.
        std::vector<ParameterDesc> own = parameters;
        LayerBuilder builder{layer, scale, wanted(layer) ? parameters : own};
        std::optional<Emitter> emitter = builder.build(report);
        report.parameters = builder.published();
        if (emitter) {
            emitter->name = report.emitter;
            // The most particles it can have alive, with every published
            // parameter anywhere in its range; then its capacity is exactly
            // that, rounded up.
            Effect alone;
            alone.name = effect.name;
            alone.simulation_rate = effect.simulation_rate;
            alone.parameters = wanted(layer) ? parameters : own;
            const std::uint64_t bound = particle_bound(alone, *emitter);
            if (bound > max_emitter_capacity) {
                report.error = "This layer could have more particles alive than any layer may (" +
                               std::to_string(max_emitter_capacity) + "): lower its amount or life";
                emitter.reset();
            } else {
                const std::uint64_t rounded = std::max<std::uint64_t>(64U, (bound + 63U) / 64U * 64U);
                emitter->capacity = static_cast<std::uint32_t>(std::min<std::uint64_t>(rounded, max_emitter_capacity));
                report.capacity = emitter->capacity;
                report.bytes = static_cast<std::uint64_t>(gpu::particle_layout(*emitter).stride) * emitter->capacity;
                alone.emitters.push_back(*emitter);
                if (Result<bool> valid = validate(alone); !valid) {
                    report.error = valid.error().message;
                    report.error_block = block_named_in(report.error, layer);
                    emitter.reset();
                }
            }
        }
        if (emitter && wanted(layer)) {
            report.included = true;
            effect.emitters.push_back(std::move(*emitter));
            used.insert(report.parameters.begin(), report.parameters.end());
            for (const AssetRequest& request : builder.requests()) {
                if (std::none_of(compiled.requests.begin(), compiled.requests.end(),
                                 [&](const AssetRequest& r) { return r.entry == request.entry; })) {
                    compiled.requests.push_back(request);
                }
            }
        }
        if (!report.error.empty() && wanted(layer) && compiled.error.empty()) {
            compiled.error = layer.name + ": " + report.error;
        }
        compiled.layers.push_back(std::move(report));
    }

    for (const Emitter& emitter : effect.emitters) {
        if (!emitter.renderer.texture.empty() && !emitter.renderer.texture.starts_with("user.")) {
            if (const TextureData* texture = library_texture_data(emitter.renderer.texture)) {
                compiled.textures.emplace(emitter.renderer.texture, *texture);
            }
        }
        if (!emitter.renderer.mesh.empty() && !emitter.renderer.mesh.starts_with("user.")) {
            if (const MeshData* mesh = library_mesh(emitter.renderer.mesh)) {
                compiled.meshes.emplace(emitter.renderer.mesh, *mesh);
            }
        }
    }
    // Only what a playing layer reads is offered to the host.
    for (const ParameterDesc& parameter : parameters) {
        if (used.contains(parameter.name)) {
            effect.parameters.push_back(parameter);
        }
    }
    if (!effect.emitters.empty()) {
        compiled.effect = std::move(effect);
    }
    return compiled;
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

namespace {

// A FloatInput's most: its value, or its parameter's top end when bound.
float most(const Effect& effect, const FloatInput& input) {
    if (!input.bound()) {
        return input.value;
    }
    for (const ParameterDesc& parameter : effect.parameters) {
        if (parameter.name == input.parameter) {
            return parameter.max;
        }
    }
    return input.value;
}

} // namespace

float natural_loop_seconds(const Compiled& compiled) {
    float end = 0.0F;
    if (compiled.effect) {
        const Effect& effect = *compiled.effect;
        for (const Emitter& emitter : effect.emitters) {
            float life = 0.0F;
            for (const InitModule& module : emitter.init) {
                if (const auto* lifetime = std::get_if<Lifetime>(&module)) {
                    life = std::max({life, most(effect, lifetime->min), most(effect, lifetime->max)});
                }
            }
            for (const SpawnModule& module : emitter.spawn) {
                if (std::holds_alternative<SpawnRate>(module)) {
                    // Building up, then a whole generation at its full.
                    end = std::max(end, life * 2.0F);
                } else if (const auto* burst = std::get_if<SpawnBurst>(&module)) {
                    const float start = std::max(burst->time, 0.0F);
                    if (burst->interval > 0.0F && burst->cycles == 0U) {
                        // Repeating for ever: until bursts overlap as they will from then on.
                        end = std::max(end, start + std::max(burst->interval, life) + life);
                    } else {
                        const float last = burst->interval > 0.0F
                                               ? start + burst->interval * static_cast<float>(std::max(burst->cycles, 1U) - 1U)
                                               : start;
                        end = std::max(end, last + life);
                    }
                }
            }
        }
    }
    if (!(end > 0.0F) || !std::isfinite(end)) {
        return 2.0F;
    }
    // A moment after the last particle dies, in tenths.
    return std::clamp(std::ceil((end + 0.2F) * 10.0F) / 10.0F, 1.0F, 120.0F);
}

float loop_seconds(const Document& document, const Compiled& compiled) {
    if (!document.loop_auto) {
        return std::clamp(document.loop_seconds, 0.5F, 120.0F);
    }
    return natural_loop_seconds(compiled);
}

std::vector<Block> detach_block(Document& document, const Compiled& compiled, Uid uid) {
    std::vector<Block> blocks;
    const std::optional<Document::BlockPlace> place = document.find_block(uid);
    if (!place) {
        return blocks;
    }
    const Block& block = place->layer->blocks[place->index];
    const BlockDef* def = find_block_def(block.kind);
    const LayerReport* report = compiled.layer(place->layer->uid);
    if (def == nullptr || !def->detachable || report == nullptr || !report->error.empty()) {
        return blocks;
    }
    const bool shapes_life = block.kind == "color" || block.kind == "size" || block.kind == "fade";
    const Layer& layer = *place->layer;
    const std::string original = block_text(block);
    for (const ModuleLine& line : report->modules) {
        bool take = line.origin == uid;
        // The merged over-life curve this block shapes travels with it, so
        // the picture is unchanged the moment it is detached.
        if (!take && shapes_life && line.origin == layer.uid) {
            const auto* update = std::get_if<UpdateModule>(&line.module);
            take = update != nullptr &&
                   ((block.kind == "size" && std::holds_alternative<SizeOverLife>(*update)) ||
                    (block.kind != "size" && std::holds_alternative<ColorOverLife>(*update)));
        }
        if (take) {
            RawModule module = line.module;
            set_raw_id(module, 0U);
            // What the block published goes with it, so its inputs keep the
            // values they had rather than naming parameters nobody declares.
            visit_fields(module, [&](std::string_view, auto& field) {
                using T = std::decay_t<decltype(field)>;
                if constexpr (std::is_same_v<T, FloatInput> || std::is_same_v<T, Vec3Input> ||
                              std::is_same_v<T, ColorInput>) {
                    if (!field.bound()) {
                        return;
                    }
                    ParameterValue value{};
                    if (compiled.effect) {
                        for (const ParameterDesc& parameter : compiled.effect->parameters) {
                            if (parameter.name == field.parameter) {
                                value = parameter.default_value;
                            }
                        }
                    }
                    if constexpr (std::is_same_v<T, FloatInput>) {
                        field.value = value[0];
                    } else if constexpr (std::is_same_v<T, Vec3Input>) {
                        field.value = Vec3{value[0], value[1], value[2]};
                    } else {
                        field.value = Color{value[0], value[1], value[2], value[3]};
                    }
                    field.parameter.clear();
                }
            });
            Block raw = make_raw_block(document, module);
            raw.detached = Detached{uid, original, std::move(module), 1U};
            blocks.push_back(std::move(raw));
        }
    }
    for (Block& raw : blocks) {
        raw.detached->count = static_cast<std::uint32_t>(blocks.size());
    }
    return blocks;
}

namespace {

// The block a detach came from, as it was.
std::optional<Block> original_of(const Detached& detached) {
    std::string error;
    return block_from_text(detached.original, error);
}

} // namespace

std::optional<DetachedGroup> detached_group(const Document& document, Uid uid) {
    const Block* block = document.block(uid);
    if (block == nullptr || !block->detached) {
        return std::nullopt;
    }
    const std::optional<Block> original = original_of(*block->detached);
    if (!original) {
        return std::nullopt;
    }
    DetachedGroup group;
    group.from = block->detached->from;
    group.title = block_title(*original);
    std::size_t expected = 0U;
    for (const Layer& layer : document.layers) {
        for (const Block& other : layer.blocks) {
            if (!other.detached || other.detached->from != group.from) {
                continue;
            }
            const bool in_layer = std::any_of(layer.blocks.begin(), layer.blocks.end(),
                                              [&](const Block& b) { return b.uid == uid; });
            if (!in_layer) {
                continue;
            }
            ++group.modules;
            RawModule now = other.raw ? *other.raw : other.detached->module;
            set_raw_id(now, 0U);
            if (!other.raw || !(now == other.detached->module) || !other.enabled) {
                group.changed = true;
            }
        }
    }
    expected = block->detached->count;
    if (expected > group.modules) {
        group.changed = true;
    }
    return group;
}

std::optional<Uid> recombine_block(Document& document, Uid uid) {
    const std::optional<Document::BlockPlace> place = document.find_block(uid);
    if (!place) {
        return std::nullopt;
    }
    Layer& layer = *place->layer;
    const Block& block = layer.blocks[place->index];
    if (!block.detached) {
        return std::nullopt;
    }
    std::optional<Block> original = original_of(*block.detached);
    if (!original) {
        return std::nullopt;
    }
    const Uid from = block.detached->from;
    std::size_t at = layer.blocks.size();
    for (std::size_t i = 0U; i < layer.blocks.size(); ++i) {
        if (layer.blocks[i].detached && layer.blocks[i].detached->from == from) {
            at = std::min(at, i);
        }
    }
    std::erase_if(layer.blocks, [&](const Block& b) { return b.detached && b.detached->from == from; });
    // Its own uid back, so its random streams are the ones it had - unless
    // something (a copied layer) has taken it since.
    if (document.block(original->uid) != nullptr || document.layer(original->uid) != nullptr || original->uid == 0U) {
        original->uid = document.allocate();
    }
    const Uid restored = original->uid;
    insert_block(layer, std::min(at, layer.blocks.size()), std::move(*original));
    return restored;
}

} // namespace ajfx::studio
