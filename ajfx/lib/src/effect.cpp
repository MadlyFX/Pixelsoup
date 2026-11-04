#include <ajfx/effect.hpp>
#include <ajfx/random.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace ajfx {
namespace {

// Texture names become package entry names: short, lower case, no paths.
bool valid_texture_name(std::string_view name) {
    if (name.empty() || name.size() > 64U || name.front() == '.') {
        return false;
    }
    for (const char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) {
            return false;
        }
    }
    return true;
}

std::string quoted(std::string_view text) {
    return "'" + std::string{text} + "'";
}

const ParameterDesc* find_parameter(const Effect& effect, std::string_view name) {
    for (const ParameterDesc& parameter : effect.parameters) {
        if (parameter.name == name) {
            return &parameter;
        }
    }
    return nullptr;
}

std::size_t component_count(ParameterType type) {
    switch (type) {
    case ParameterType::Float:
        return 1U;
    case ParameterType::Vec3:
        return 3U;
    case ParameterType::Color:
        return 4U;
    }
    return 4U;
}

template <class T>
constexpr ParameterType parameter_type_of() {
    if constexpr (std::is_same_v<T, float>) {
        return ParameterType::Float;
    } else if constexpr (std::is_same_v<T, Vec3>) {
        return ParameterType::Vec3;
    } else {
        return ParameterType::Color;
    }
}

// The values a float input can take: itself if constant, the parameter's
// range if bound. Only meaningful after bindings have been checked.
struct Range final {
    float lo{};
    float hi{};
};

Range range(const Effect& effect, const FloatInput& input) {
    if (!input.bound()) {
        return {input.value, input.value};
    }
    const ParameterDesc* parameter = find_parameter(effect, input.parameter);
    return parameter == nullptr ? Range{} : Range{parameter->min, parameter->max};
}

bool finite(float value) {
    return std::isfinite(value);
}

bool finite(Vec3 value) {
    return finite(value.x) && finite(value.y) && finite(value.z);
}

bool finite(Color value) {
    return finite(value.r) && finite(value.g) && finite(value.b) && finite(value.a);
}

template <class Module>
std::string_view kind_of(const Module&) {
    return Module::kind;
}

// Walks a module's fields checking the rules every field of a given type
// shares: finite numbers, bindings that resolve to a parameter of the right
// type, sorted keys, enum values in range.
class FieldChecker final {
public:
    // `draw` checks the renderer's draw inputs, which only playback-time
    // parameters may bind; everything else is read by the simulation, which
    // only bake-time parameters may reach (ParameterTiming).
    FieldChecker(const Effect& effect, std::string where, bool draw = false)
        : effect_{effect}, where_{std::move(where)}, draw_{draw} {}

    template <class T>
    void operator()(std::string_view field, const Input<T>& input) {
        if (!input.bound()) {
            if (!finite(input.value)) {
                fail(ErrorCode::InvalidEffect, field, "is not a finite number");
            }
            return;
        }
        const ParameterDesc* parameter = find_parameter(effect_, input.parameter);
        if (parameter == nullptr) {
            fail(ErrorCode::UnknownParameter, field, "is bound to unknown parameter " + quoted(input.parameter));
        } else if (parameter->type != parameter_type_of<T>()) {
            fail(ErrorCode::ParameterTypeMismatch, field,
                 "is bound to parameter " + quoted(input.parameter) + " of a different type");
        } else if (draw_ && parameter->timing != ParameterTiming::Playback) {
            fail(ErrorCode::InvalidEffect, field,
                 "is a draw input bound to bake-time parameter " + quoted(input.parameter) +
                     "; only drawing reads it, so make the parameter playback-time");
        } else if (!draw_ && parameter->timing == ParameterTiming::Playback) {
            fail(ErrorCode::InvalidEffect, field,
                 "is read by the simulation and bound to playback-time parameter " + quoted(input.parameter) +
                     ", which a cache could not honour; make the parameter bake-time");
        }
    }

    void operator()(std::string_view field, const float& value) {
        if (!finite(value)) {
            fail(ErrorCode::InvalidEffect, field, "is not a finite number");
        }
    }

    void operator()(std::string_view field, const Vec3& value) {
        if (!finite(value)) {
            fail(ErrorCode::InvalidEffect, field, "is not a finite vector");
        }
    }
    void operator()(std::string_view field, const Color& value) {
        if (!finite(value)) { fail(ErrorCode::InvalidEffect, field, "is not a finite color"); }
    }

    template <class Key>
        requires std::is_same_v<Key, ColorKey> || std::is_same_v<Key, CurveKey>
    void operator()(std::string_view field, const std::vector<Key>& keys) {
        if (keys.empty()) {
            fail(ErrorCode::InvalidEffect, field, "needs at least one key");
            return;
        }
        float previous = 0.0F;
        for (const Key& key : keys) {
            if (!finite(key.t) || !finite(key.value) || key.t < previous || key.t > 1.0F) {
                fail(ErrorCode::InvalidEffect, field, "keys must be finite with t rising from 0 to 1");
                return;
            }
            previous = key.t;
        }
    }

    template <class E>
        requires std::is_enum_v<E>
    void operator()(std::string_view field, const E& value) {
        if (static_cast<std::size_t>(value) >= std::size(EnumNames<E>::names)) {
            fail(ErrorCode::InvalidEffect, field, "has an unknown value");
        }
    }

    // A break piece's own fields, held to what any other module's are.
    void operator()(std::string_view field, const std::vector<BreakPiece>& pieces) {
        for (std::size_t i = 0U; i < pieces.size(); ++i) {
            const std::string where = std::string{field} + "[" + std::to_string(i) + "]";
            BreakPiece::fields(pieces[i], [&](std::string_view name, const auto& value) {
                (*this)(where + "." + std::string{name}, value);
            });
        }
    }

    // A body kind's own fields, held to what any other module's numbers are.
    void operator()(std::string_view field, const std::vector<RigidBodyKind>& bodies) {
        for (std::size_t i = 0U; i < bodies.size(); ++i) {
            const std::string where = std::string{field} + "[" + std::to_string(i) + "]";
            RigidBodyKind::fields(bodies[i], [&](std::string_view name, const auto& value) {
                (*this)(where + "." + std::string{name}, value);
            });
        }
    }

    // A prop's own fields, so every number in one is held to what any other
    // module's number is. What a prop may be is check_module's business.
    void operator()(std::string_view field, const std::vector<RigidProp>& props) {
        for (std::size_t i = 0U; i < props.size(); ++i) {
            const std::string where = std::string{field} + "[" + std::to_string(i) + "]";
            RigidProp::fields(props[i], [&](std::string_view name, const auto& value) {
                (*this)(where + "." + std::string{name}, value);
            });
        }
    }

    // What a scope names is checked against its emitter (check_scope); here
    // only how much it may name.
    void operator()(std::string_view field, const Scope& scope) {
        if (scope.sources.size() > max_scope_entries || scope.kinds.size() > max_scope_entries) {
            fail(ErrorCode::InvalidEffect, field,
                 "names more than the " + std::to_string(max_scope_entries) + " entries a scope may hold");
        }
    }

    void operator()(std::string_view, const std::uint32_t&) {}
    void operator()(std::string_view, const bool&) {}
    void operator()(std::string_view, const std::string&) {}

    void fail(ErrorCode code, std::string_view field, const std::string& problem) {
        if (!error_) {
            error_ = Error{code, where_ + " field " + quoted(field) + " " + problem};
        }
    }

    // A module-level rule, reported in the same form.
    void require(bool condition, std::string_view field, const std::string& problem) {
        if (!condition) {
            fail(ErrorCode::InvalidEffect, field, problem);
        }
    }

    [[nodiscard]] const std::optional<Error>& error() const noexcept { return error_; }

private:
    const Effect& effect_;
    std::string where_;
    bool draw_{false};
    std::optional<Error> error_;
};

// Rules particular to one module, run after its fields passed.
void check_module(const Effect&, const SpawnBurst& m, FieldChecker& check) {
    check.require(m.time >= 0.0F, "time", "must not be negative");
    check.require(m.interval >= 0.0F, "interval", "must not be negative");
}

void check_module(const Effect&, const SpawnPile& m, FieldChecker& check) {
    check.require(m.slope >= 5.0F && m.slope <= 85.0F, "slope", "must be between 5 and 85 degrees");
    check.require(m.width > 0.0F, "width", "must be positive");
    check.require(m.depth > 0.0F, "depth", "must be positive");
    check.require(m.spacing >= 0.5F && m.spacing <= 4.0F, "spacing", "must be between 0.5 and 4");
    check.require(m.jitter >= 0.0F && m.jitter <= 1.0F, "jitter", "must be between 0 and 1");
    check.require(m.release >= 0.0F, "release", "must not be negative");
    check.require(m.shape != PileShape::Single || m.count == 1U, "count", "must be 1 for a single body");
}

void check_module(const Effect&, const SpawnActions& m, FieldChecker& check) {
    check.require(m.max_per_step >= 1U, "max_per_step", "must be at least 1");
    check.require(m.max_alive >= 1U, "max_alive", "must be at least 1");
}

void check_module(const Effect& e, const SpawnRate& m, FieldChecker& check) {
    check.require(range(e, m.rate).lo >= 0.0F, "rate", "must not be negative");
}

void check_module(const Effect& e, const Lifetime& m, FieldChecker& check) {
    check.require(range(e, m.min).lo > 0.0F, "min", "must be greater than zero");
    check.require(range(e, m.max).lo > 0.0F, "max", "must be greater than zero");
}

void check_module(const Effect& e, const ShapeLocation& m, FieldChecker& check) {
    check.require(range(e, m.radius_min).lo >= 0.0F, "radius_min", "must not be negative");
    check.require(range(e, m.radius_max).lo >= 0.0F, "radius_max", "must not be negative");
    check.require(m.size.x >= 0.0F && m.size.y >= 0.0F && m.size.z >= 0.0F, "size", "must not be negative");
}

void check_module(const Effect& e, const Velocity& m, FieldChecker& check) {
    const Range spread = range(e, m.spread);
    check.require(spread.lo >= 0.0F && spread.hi <= 180.0F, "spread", "must be between 0 and 180 degrees");
    const Range inner = range(e, m.spread_min);
    check.require(inner.lo >= 0.0F && inner.hi <= 180.0F, "spread_min", "must be between 0 and 180 degrees");
}

void check_module(const Effect& e, const InitSize& m, FieldChecker& check) {
    check.require(range(e, m.min).lo >= 0.0F, "min", "must not be negative");
    check.require(range(e, m.max).lo >= 0.0F, "max", "must not be negative");
}

void check_module(const Effect& e, const RigidBody& m, FieldChecker& check) {
    check.require(range(e, m.angular_drag).lo >= 0.0F, "angular_drag", "must not be negative");
    check.require(m.contact_scale >= 0.0F, "contact_scale", "must not be negative");
    check.require(range(e, m.rest_speed).lo >= 0.0F, "rest_speed", "must not be negative");
    const Range roundness = range(e, m.roundness);
    check.require(roundness.lo >= 0.0F && roundness.hi <= 1.0F, "roundness", "must be between 0 and 1");
}

// The emitter whose bodies these particles meet has to exist, hold bodies,
// and come first, so that what they meet is this step's.
void check_body_collision(const Effect& effect, std::size_t index, const BodyCollision& m, FieldChecker& check) {
    std::optional<std::size_t> at;
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        if (effect.emitters[i].name == m.emitter) {
            at = i;
            break;
        }
    }
    if (!at) {
        check.require(false, "emitter", "names emitter " + quoted(m.emitter) + ", which this effect has none of");
        return;
    }
    const Emitter& other = effect.emitters[*at];
    const RigidPhysics* physics = rigid_physics(other);
    check.require(physics != nullptr, "emitter",
                  "names emitter " + quoted(m.emitter) +
                      ", which has no 'rigid_physics' module, so it holds no bodies to hit");
    check.require(*at != index, "emitter", "names its own emitter; particles meet the bodies of another");
    // Where the named emitter is listed no longer matters: every rigid
    // physics emitter steps before any other (Simulation::step), so the
    // bodies met have already moved this step whichever way round they are
    // written.
    check.require(other.capacity <= max_body_colliders, "emitter",
                  "names emitter " + quoted(m.emitter) + ", which holds up to " + std::to_string(other.capacity) +
                      " bodies; at most " + std::to_string(max_body_colliders) +
                      " may be collided with, the room their grid is given");
}

void check_module(const Effect& e, const Wake& m, FieldChecker& check) {
    check.require(range(e, m.strength).lo >= 0.0F, "strength", "must not be negative");
    check.require(m.radius > 0.0F && std::isfinite(m.radius), "radius",
                  "must be positive: it is how many half sizes of a body its wake reaches");
    check.require(m.min_speed >= 0.0F && std::isfinite(m.min_speed), "min_speed", "must not be negative");
}

void check_module(const Effect&, const Break& m, FieldChecker& check) {
    check.require(m.threshold > 0.0F, "threshold", "must be positive: a body cannot break on no impact at all");
    check.require(m.blast_threshold >= 0.0F, "blast_threshold",
                  "must not be negative: zero leaves blasts to push, above zero is how hard one must be to break");
    check.require(m.scatter >= 0.0F, "scatter", "must not be negative");
    check.require(!m.pieces.empty(), "pieces", "needs at least one piece: a body cannot break into nothing");
    check.require(m.pieces.size() <= max_break_pieces, "pieces",
                  "breaks into more than the " + std::to_string(max_break_pieces) + " pieces one body may make");
    for (std::size_t i = 0U; i < m.pieces.size(); ++i) {
        const std::string at = "pieces[" + std::to_string(i) + "]";
        check.require(m.pieces[i].size > 0.0F, at + ".size", "must be positive");
    }
}

void check_module(const Effect&, const SpawnFromContacts& m, FieldChecker& check) {
    check.require(m.min_speed > 0.0F, "min_speed",
                  "must be positive: bodies at rest touch at no speed, and every touch would bear particles");
    check.require(m.per_hit >= 0.0F, "per_hit", "must not be negative");
    check.require(m.per_speed >= 0.0F, "per_speed", "must not be negative");
    check.require(m.max_per_step >= 1U && m.max_per_step <= max_emitter_capacity, "max_per_step",
                  "must be between 1 and " + std::to_string(max_emitter_capacity));
    check.require(m.spread >= 0.0F && m.spread <= 180.0F, "spread", "must be between 0 and 180 degrees");
}

void check_module(const Effect&, const SpawnFromBodies& m, FieldChecker& check) {
    check.require(m.min_speed >= 0.0F, "min_speed", "must not be negative");
    check.require(m.rate >= 0.0F, "rate", "must not be negative");
    check.require(m.per_speed >= 0.0F, "per_speed", "must not be negative");
    check.require(m.max_per_step >= 1U && m.max_per_step <= max_emitter_capacity, "max_per_step",
                  "must be between 1 and " + std::to_string(max_emitter_capacity));
    check.require(m.surface >= 0.0F && m.surface <= 1.0F, "surface", "must be between 0 and 1");
}

void check_module(const Effect&, const SpawnFromParticles& m, FieldChecker& check) {
    check.require(m.min_speed >= 0.0F, "min_speed", "must not be negative");
    check.require(m.rate >= 0.0F, "rate", "must not be negative");
    check.require(m.per_speed >= 0.0F, "per_speed", "must not be negative");
    check.require(m.max_per_step >= 1U && m.max_per_step <= max_emitter_capacity, "max_per_step",
                  "must be between 1 and " + std::to_string(max_emitter_capacity));
}

void check_module(const Effect&, const Heat& m, FieldChecker& check) {
    check.require(m.start >= 0.0F, "start", "must not be negative");
    check.require(m.half_life > 0.0F, "half_life", "must be positive: a heat that never cools is a colour");
    check.require(m.glow >= 0.0F, "glow", "must not be negative");
}

// A shockwave answers a blast the effect lists.
void check_module(const Effect& e, const Shockwave& m, FieldChecker& check) {
    check.require(find_blast(e, m.blast) != nullptr, "blast",
                  "names blast " + quoted(m.blast) + ", which this effect has none of");
}

// Both grid modules name a volume the effect lists.
void check_module(const Effect& e, const VolumeAdvect& m, FieldChecker& check) {
    check.require(find_volume(e, m.volume) != nullptr, "volume",
                  "names volume " + quoted(m.volume) + ", which this effect has none of");
    check.require(range(e, m.strength).lo >= 0.0F, "strength", "must not be negative");
}

void check_module(const Effect& e, const VolumeSource& m, FieldChecker& check) {
    check.require(find_volume(e, m.volume) != nullptr, "volume",
                  "names volume " + quoted(m.volume) + ", which this effect has none of");
    check.require(range(e, m.density).lo >= 0.0F, "density", "must not be negative");
    check.require(range(e, m.temperature).lo >= 0.0F, "temperature", "must not be negative");
    check.require(range(e, m.velocity).lo >= 0.0F, "velocity", "must not be negative");
}

void check_module(const Effect&, const BodyWind& m, FieldChecker& check) {
    check.require(m.drag >= 0.0F, "drag", "must not be negative");
    check.require(m.gust >= 0.0F && m.gust <= 1.0F, "gust",
                  "must be between 0 and 1: the share of its speed the wind rises and falls by");
    check.require(m.gust_seconds > 0.0F, "gust_seconds", "must be positive");
}

void check_module(const Effect&, const Glue& m, FieldChecker& check) {
    check.require(m.range >= 0.0F, "range", "must not be negative");
    check.require(m.strength > 0.0F, "strength",
                  "must be positive: a join that takes no force is not a join");
}

void check_module(const Effect&, const RigidPhysics& m, FieldChecker& check) {
    check.require(m.density > 0.0F, "density", "must be positive");
    check.require(m.friction >= 0.0F && m.friction <= 1.0F, "friction", "must be between 0 and 1");
    check.require(m.restitution >= 0.0F && m.restitution <= 1.0F, "restitution", "must be between 0 and 1");
    check.require(m.angular_damping >= 0.0F, "angular_damping", "must not be negative");
    check.require(m.bodies.size() <= max_rigid_body_kinds, "bodies",
                  "holds more kinds of body than the " + std::to_string(max_rigid_body_kinds) +
                      " an emitter may have beyond its own");
    for (std::size_t i = 0U; i < m.bodies.size(); ++i) {
        const RigidBodyKind& body = m.bodies[i];
        const std::string at = "bodies[" + std::to_string(i) + "]";
        check.require(body.mesh.empty() || valid_texture_name(body.mesh), at + ".mesh",
                      "must be 1 to 64 characters of a-z, 0-9, '_', '-' and '.'");
        check.require(body.draw.empty() || valid_texture_name(body.draw), at + ".draw",
                      "must be 1 to 64 characters of a-z, 0-9, '_', '-' and '.'");
        check.require(std::isfinite(body.draw_tint.r) && std::isfinite(body.draw_tint.g) && std::isfinite(body.draw_tint.b) &&
                      std::isfinite(body.draw_tint.a) && body.draw_tint.r >= 0 && body.draw_tint.g >= 0 && body.draw_tint.b >= 0 &&
                      body.draw_tint.a >= 0 && body.draw_tint.a <= 1, at + ".draw_tint", "must be finite nonnegative RGB and alpha between 0 and 1");
        // A kind at no size is a body with no shape to build, which an
        // engine answers for in its own way rather than complaining, so it
        // is refused here with the kind that asked for it.
        check.require(body.scale > 0.0F && std::isfinite(body.scale), at + ".scale", "must be above zero");
        check.require(body.density > 0.0F, at + ".density", "must be positive");
        check.require(body.friction >= 0.0F && body.friction <= 1.0F, at + ".friction",
                      "must be between 0 and 1");
        check.require(body.restitution >= 0.0F && body.restitution <= 1.0F, at + ".restitution",
                      "must be between 0 and 1");
    }
    check.require(m.props.size() <= max_rigid_props, "props",
                  "holds more props than the " + std::to_string(max_rigid_props) + " that may stand in one world");
    for (std::size_t i = 0U; i < m.props.size(); ++i) {
        const RigidProp& p = m.props[i];
        const std::string at = "props[" + std::to_string(i) + "]";
        check.require(std::isfinite(p.draw_tint.r) && std::isfinite(p.draw_tint.g) && std::isfinite(p.draw_tint.b) &&
                      std::isfinite(p.draw_tint.a) && p.draw_tint.r >= 0 && p.draw_tint.g >= 0 && p.draw_tint.b >= 0 &&
                      p.draw_tint.a >= 0 && p.draw_tint.a <= 1, at + ".draw_tint", "must be finite nonnegative RGB and alpha between 0 and 1");
        const bool from_model = p.shape == PropShape::Mesh || p.shape == PropShape::Hull;
        const bool round = p.shape == PropShape::Sphere || p.shape == PropShape::Cylinder ||
                           p.shape == PropShape::Cone || p.shape == PropShape::HollowCylinder ||
                           p.shape == PropShape::Pipe;
        const bool tall = p.shape == PropShape::Cylinder || p.shape == PropShape::Cone ||
                          p.shape == PropShape::HollowCylinder || p.shape == PropShape::Pipe;
        const bool boxy = p.shape == PropShape::Box || p.shape == PropShape::HollowBox;
        const bool hollow = p.shape == PropShape::HollowBox || p.shape == PropShape::HollowCylinder ||
                            p.shape == PropShape::Pipe;
        check.require(!from_model || valid_texture_name(p.mesh), at + ".mesh",
                      "must be 1 to 64 characters of a-z, 0-9, '_', '-' and '.'");
        // A collision mesh cannot move at all: it has no volume to take mass
        // from, and no inside, so contact against a moving one is unreliable
        // (Jolt's Shape::MustBeStatic). The generated hollow shapes are
        // compounds of convex pieces, and those move properly.
        check.require(p.shape != PropShape::Mesh || p.motion == PropMotion::Fixed, at + ".motion",
                      "must be 'fixed' while the prop collides as its own model: a collision mesh cannot move "
                      "(collide as 'hollow_box', 'hollow_cylinder' or 'pipe' instead)");
        check.require(p.size > 0.0F, at + ".size", "must be positive");
        check.require(p.density > 0.0F, at + ".density", "must be positive");
        check.require(p.friction >= 0.0F && p.friction <= 1.0F, at + ".friction", "must be between 0 and 1");
        check.require(p.restitution >= 0.0F && p.restitution <= 1.0F, at + ".restitution",
                      "must be between 0 and 1");
        check.require(p.move_from >= 0.0F, at + ".move_from", "must not be negative");
        check.require(p.move_over >= 0.0F, at + ".move_over", "must not be negative");
        check.require(!round || p.radius > 0.0F, at + ".radius", "must be positive for this shape");
        check.require(!tall || p.height > 0.0F, at + ".height", "must be positive for this shape");
        check.require(!boxy || (p.half_size.x > 0.0F && p.half_size.y > 0.0F && p.half_size.z > 0.0F),
                      at + ".half_size", "must be positive on every axis for this shape");
        check.require(!round || (p.segments >= 3U && p.segments <= 64U), at + ".segments",
                      "must be between 3 and 64 for this shape");
        check.require(!hollow || p.wall > 0.0F, at + ".wall", "must be positive for a hollow shape");
        // A wall thicker than the room inside leaves nothing hollow.
        if (p.shape == PropShape::HollowCylinder || p.shape == PropShape::Pipe) {
            check.require(p.wall < p.radius, at + ".wall", "must be thinner than the prop's radius");
        } else if (p.shape == PropShape::HollowBox) {
            check.require(p.wall < std::min(p.half_size.x, p.half_size.z), at + ".wall",
                          "must be thinner than half the prop's width and depth");
        }
    }
}

void check_module(const Effect& e, const Drag& m, FieldChecker& check) {
    check.require(range(e, m.coefficient).lo >= 0.0F, "coefficient", "must not be negative");
}

void check_module(const Effect& e, const CurlNoise& m, FieldChecker& check) {
    check.require(range(e, m.frequency).lo >= 0.0F, "frequency", "must not be negative");
}

void check_module(const Effect& e, const Attractor& m, FieldChecker& check) {
    check.require(range(e, m.radius).lo >= 0.0F, "radius", "must not be negative");
}

void check_module(const Effect& e, const Converge& m, FieldChecker& check) {
    check.require(range(e, m.height).lo >= 0.0F, "height", "must not be negative");
}

void check_module(const Effect& e, const InitSpin& m, FieldChecker& check) {
    check.require(range(e, m.drag).lo >= 0.0F, "drag", "must not be negative");
}

void check_module(const Effect& e, const PlaneCollision& m, FieldChecker& check) {
    check.require(dot(m.normal, m.normal) > 1.0e-12F, "normal", "must not be zero");
    check.require(range(e, m.restitution).lo >= 0.0F, "restitution", "must not be negative");
    const Range friction = range(e, m.friction);
    check.require(friction.lo >= 0.0F && friction.hi <= 1.0F, "friction", "must be between 0 and 1");
}

// Restitution and friction, as every collider takes them.
template <class Module>
void check_response(const Effect& e, const Module& m, FieldChecker& check) {
    check.require(range(e, m.restitution).lo >= 0.0F, "restitution", "must not be negative");
    const Range friction = range(e, m.friction);
    check.require(friction.lo >= 0.0F && friction.hi <= 1.0F, "friction", "must be between 0 and 1");
}

void check_module(const Effect& e, const SphereCollision& m, FieldChecker& check) {
    check.require(range(e, m.radius).lo > 0.0F, "radius", "must be positive");
    check_response(e, m, check);
}

void check_module(const Effect& e, const SdfCollision& m, FieldChecker& check) {
    check.require(valid_texture_name(m.field), "field", "must be 1 to 64 characters of a-z, 0-9, '_', '-' and '.'");
    check_response(e, m, check);
}

void check_module(const Effect& e, const BoxCollision& m, FieldChecker& check) {
    check.require(m.half_size.x > 0.0F && m.half_size.y > 0.0F && m.half_size.z > 0.0F, "half_size",
                  "must be positive on every axis");
    check_response(e, m, check);
}

template <class Module>
void check_module(const Effect&, const Module&, FieldChecker&) {}

template <class Module>
std::optional<Error> check_module_fields(const Effect& effect, const Emitter& emitter, const Module& module) {
    FieldChecker checker{effect, "emitter " + quoted(emitter.name) + " module " + quoted(kind_of(module)) + " #" +
                                     std::to_string(module.id)};
    Module::fields(module, checker);
    if (!checker.error()) {
        check_module(effect, module, checker);
    }
    return checker.error();
}

// What a rigid physics emitter may hold: the engine owns its particles'
// motion, so only what can become a body, a force on every body or an
// unmoving collider is allowed.
std::optional<Error> check_rigid_physics(const Emitter& emitter, const std::string& where) {
    if (emitter.capacity > max_rigid_capacity) {
        return Error{ErrorCode::InvalidEffect, where + " has rigid physics, which holds at most " +
                                                    std::to_string(max_rigid_capacity) + " particles"};
    }
    std::optional<Error> error;
    const auto refuse = [&](const auto& module, const std::string& why) {
        if (!error) {
            error = Error{ErrorCode::InvalidEffect, where + " module " + quoted(kind_of(module)) + " #" +
                                                         std::to_string(module.id) + " " + why};
        }
    };
    for (const SpawnModule& module : emitter.spawn) {
        std::visit(
            [&](const auto& m) {
                using M = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<M, SpawnFromContacts> || std::is_same_v<M, SpawnFromBodies>) {
                    refuse(m, "bears particles from another emitter's bodies; a rigid physics emitter's bodies "
                              "are not born that way (give the particles an emitter of their own)");
                } else if constexpr (std::is_same_v<M, SpawnFromParticles>) {
                    refuse(m, "bears particles from another emitter's particles; a rigid physics emitter's bodies "
                              "are not born that way (give the particles an emitter of their own)");
                }
            },
            module);
    }
    for (const InitModule& module : emitter.init) {
        std::visit(
            [&](const auto& m) {
                using M = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<M, InitRotation> || std::is_same_v<M, InitSpin>) {
                    refuse(m, "turns sprites; a rigid physics body turns by its orientation (use 'tumble')");
                }
            },
            module);
    }
    const RigidPhysics* physics = nullptr;
    for (const UpdateModule& module : emitter.update) {
        std::visit(
            [&](const auto& m) {
                using M = std::decay_t<decltype(m)>;
                const auto unmoving = [&](const auto& collider, bool bound) {
                    if (bound) {
                        refuse(collider, "is bound to a parameter; rigid physics colliders do not move or change yet");
                    } else if (collider.kill) {
                        refuse(collider, "kills; rigid physics bodies are only removed when their life ends");
                    }
                };
                if constexpr (std::is_same_v<M, RigidPhysics>) {
                    physics = &m;
                } else if constexpr (std::is_same_v<M, Drag>) {
                    if (m.coefficient.bound()) {
                        refuse(m, "is bound to a parameter; rigid physics takes drag as a fixed damping");
                    }
                } else if constexpr (std::is_same_v<M, PlaneCollision>) {
                    unmoving(m, m.restitution.bound() || m.friction.bound());
                } else if constexpr (std::is_same_v<M, SphereCollision>) {
                    if (m.inside) {
                        refuse(m, "keeps particles inside; rigid physics colliders keep them out");
                    }
                    unmoving(m, m.center.bound() || m.radius.bound() || m.restitution.bound() || m.friction.bound());
                } else if constexpr (std::is_same_v<M, BoxCollision>) {
                    unmoving(m, m.center.bound() || m.restitution.bound() || m.friction.bound());
                } else if constexpr (!std::is_same_v<M, Gravity> && !std::is_same_v<M, ColorOverLife> &&
                                     !std::is_same_v<M, Break> && !std::is_same_v<M, Glue> &&
                                     !std::is_same_v<M, Shockwave> && !std::is_same_v<M, Heat> &&
                                     !std::is_same_v<M, BodyWind>) {
                    refuse(m, "cannot act on rigid physics particles; the physics engine moves them");
                }
            },
            module);
    }
    if (error) {
        return error;
    }
    // A spawn module may only name a kind the emitter has: its own, or one
    // of the kinds rigid physics lists.
    const auto kinds = static_cast<std::uint32_t>(physics->bodies.size() + 1U);
    for (const SpawnModule& module : emitter.spawn) {
        const std::uint32_t named = std::visit([](const auto& m) { return spawn_body(m); }, module);
        if (named >= kinds) {
            return Error{ErrorCode::InvalidEffect,
                         where + " module " + quoted(std::visit([](const auto& m) { return kind_of(m); }, module)) + " spawns body kind " +
                             std::to_string(named) + ", and rigid physics has " + std::to_string(kinds) +
                             " (0 is the emitter's own)"};
        }
    }
    // Every kind a break names must exist, no kind may break two ways, and
    // the pieces must stop breaking: a cycle would come apart for ever.
    std::vector<const Break*> breaks;
    for (const UpdateModule& module : emitter.update) {
        if (const auto* breaking = std::get_if<Break>(&module)) {
            breaks.push_back(breaking);
        }
    }
    std::vector<const Break*> breaks_kind(kinds, nullptr);
    for (const Break* breaking : breaks) {
        const std::string which = " module 'break' #" + std::to_string(breaking->id);
        if (breaking->body >= kinds) {
            return Error{ErrorCode::InvalidEffect, where + which + " breaks body kind " +
                                                       std::to_string(breaking->body) + ", and rigid physics has " +
                                                       std::to_string(kinds) + " (0 is the emitter's own)"};
        }
        if (breaks_kind[breaking->body] != nullptr) {
            return Error{ErrorCode::InvalidEffect, where + which + " breaks body kind " +
                                                       std::to_string(breaking->body) +
                                                       ", which another 'break' module already breaks"};
        }
        breaks_kind[breaking->body] = breaking;
        for (std::size_t i = 0U; i < breaking->pieces.size(); ++i) {
            if (breaking->pieces[i].body >= kinds) {
                return Error{ErrorCode::InvalidEffect,
                             where + which + " piece " + std::to_string(i) + " is body kind " +
                                 std::to_string(breaking->pieces[i].body) + ", and rigid physics has " +
                                 std::to_string(kinds) + " (0 is the emitter's own)"};
            }
        }
    }
    // A kind breaks into kinds that break into kinds: walk it and refuse a
    // loop, naming the kind the trail came back to.
    for (std::uint32_t start = 0U; start < kinds; ++start) {
        if (breaks_kind[start] == nullptr) {
            continue;
        }
        std::vector<bool> seen(kinds, false);
        std::vector<std::uint32_t> trail{start};
        while (!trail.empty()) {
            const std::uint32_t kind = trail.back();
            trail.pop_back();
            if (seen[kind]) {
                continue;
            }
            seen[kind] = true;
            if (breaks_kind[kind] == nullptr) {
                continue;
            }
            for (const BreakPiece& piece : breaks_kind[kind]->pieces) {
                if (piece.body == start) {
                    return Error{ErrorCode::InvalidEffect,
                                 where + " body kind " + std::to_string(start) +
                                     " breaks into pieces that break back into it; breaking has to end"};
                }
                trail.push_back(piece.body);
            }
        }
    }
    const RigidShape shape = physics->shape;
    if (shape == RigidShape::Hull && emitter.renderer.geometry != Geometry::Mesh) {
        return Error{ErrorCode::InvalidEffect,
                     where + " rigid physics shape 'hull' wraps the renderer's mesh, and the renderer draws none"};
    }
    for (std::size_t i = 0U; i < physics->bodies.size(); ++i) {
        if (!physics->bodies[i].draw.empty() && emitter.renderer.geometry != Geometry::Mesh) {
            return Error{ErrorCode::InvalidEffect,
                         where + " rigid physics body kind " + std::to_string(i + 1U) + " is drawn as model " +
                             quoted(physics->bodies[i].draw) + ", and the renderer draws sprites, not meshes"};
        }
    }
    for (std::size_t i = 0U; i < physics->bodies.size(); ++i) {
        const RigidBodyKind& body = physics->bodies[i];
        const bool hull = body.shape == RigidShape::Hull ||
                          (body.shape == RigidShape::Auto && emitter.renderer.geometry == Geometry::Mesh);
        if (hull && body.mesh.empty() && emitter.renderer.geometry != Geometry::Mesh) {
            return Error{ErrorCode::InvalidEffect,
                         where + " rigid physics body kind " + std::to_string(i + 1U) +
                             " collides as a hull and names no model, and the renderer draws none"};
        }
    }
    if (emitter.renderer.geometry == Geometry::Mesh && emitter.renderer.facing == Facing::Velocity) {
        return Error{ErrorCode::InvalidEffect,
                     where + " renderer field 'facing' rigid mesh particles turn by their orientation: no velocity facing"};
    }
    return std::nullopt;
}

std::optional<Error> check_parameters(const Effect& effect) {
    std::set<std::string_view> names;
    for (const ParameterDesc& parameter : effect.parameters) {
        const std::string where = "parameter " + quoted(parameter.name);
        if (parameter.name.empty()) {
            return Error{ErrorCode::InvalidEffect, "a parameter has no name"};
        }
        if (!names.insert(parameter.name).second) {
            return Error{ErrorCode::InvalidEffect, where + " is declared twice"};
        }
        if (!finite(parameter.min) || !finite(parameter.max) || parameter.min > parameter.max) {
            return Error{ErrorCode::InvalidEffect, where + " needs a finite range with min <= max"};
        }
        for (std::size_t i = 0U; i < component_count(parameter.type); ++i) {
            const float value = parameter.default_value[i];
            if (!finite(value) || value < parameter.min || value > parameter.max) {
                return Error{ErrorCode::InvalidEffect, where + " default is outside its range"};
            }
        }
    }
    return std::nullopt;
}

// Every blast is named once, stands somewhere, reaches somewhere and goes
// off at a time the effect reaches.
std::optional<Error> check_blasts(const Effect& effect) {
    if (effect.blasts.size() > max_blasts) {
        return Error{ErrorCode::InvalidEffect, "the effect lists " + std::to_string(effect.blasts.size()) +
                                                   " blasts, more than the " + std::to_string(max_blasts) +
                                                   " one effect may have"};
    }
    std::set<std::string_view> names;
    for (const Blast& blast : effect.blasts) {
        if (blast.name.empty()) {
            return Error{ErrorCode::InvalidEffect, "a blast has no name"};
        }
        if (!names.insert(blast.name).second) {
            return Error{ErrorCode::InvalidEffect, "blast " + quoted(blast.name) + " is declared twice"};
        }
        FieldChecker check{effect, "blast " + quoted(blast.name)};
        Blast::fields(blast, check);
        if (!check.error()) {
            check.require(blast.power >= 0.0F, "power", "must not be negative: a blast throws outwards");
            check.require(blast.radius > 0.0F, "radius", "must be positive: a blast reaches somewhere");
            check.require(blast.time >= 0.0F, "time", "must not be negative");
        }
        if (check.error()) {
            return check.error();
        }
    }
    return std::nullopt;
}

// Every volume is named once, has cells of some size, and fits its budget:
// a grid past the cap is refused with the memory it would need.
std::optional<Error> check_volumes(const Effect& effect) {
    if (effect.volumes.size() > max_volumes) {
        return Error{ErrorCode::InvalidEffect, "the effect lists " + std::to_string(effect.volumes.size()) +
                                                   " volumes, more than the " + std::to_string(max_volumes) +
                                                   " one effect may have"};
    }
    std::set<std::string_view> names;
    for (const Volume& volume : effect.volumes) {
        if (!valid_texture_name(volume.name)) {
            return Error{ErrorCode::InvalidEffect,
                         "a volume's name must be 1 to 64 characters of a-z, 0-9, '_', '-' and '.'"};
        }
        if (!names.insert(volume.name).second) {
            return Error{ErrorCode::InvalidEffect, "volume " + quoted(volume.name) + " is declared twice"};
        }
        const std::string where = "volume " + quoted(volume.name);
        FieldChecker check{effect, where};
        Volume::fields(volume, check);
        if (!check.error()) {
            check.require(volume.cell_size > 0.0F, "cell_size", "must be positive");
            check.require(volume.nx >= 2U && volume.ny >= 2U && volume.nz >= 2U, "nx",
                          "and ny and nz must be at least 2: a grid is at least two cells each way");
            check.require(volume.nx <= max_volume_size && volume.ny <= max_volume_size &&
                              volume.nz <= max_volume_size,
                          "nx", "and ny and nz must be at most " + std::to_string(max_volume_size) + " cells");
            check.require(volume.vorticity >= 0.0F, "vorticity", "must not be negative");
            check.require(volume.pressure_iterations <= max_volume_pressure_iterations, "pressure_iterations",
                          "must be at most " + std::to_string(max_volume_pressure_iterations));
            check.require(volume.density_dissipation >= 0.0F, "density_dissipation", "must not be negative");
            check.require(volume.temperature_dissipation >= 0.0F, "temperature_dissipation",
                          "must not be negative");
            check.require(volume.velocity_dissipation >= 0.0F, "velocity_dissipation", "must not be negative");
            check.require(volume.extinction >= 0.0F, "extinction", "must not be negative");
            check.require(volume.albedo >= 0.0F && volume.albedo <= 1.0F, "albedo", "must be between 0 and 1");
        }
        if (check.error()) {
            return check.error();
        }
        const std::uint64_t cells = volume_cells(volume);
        if (cells > max_volume_cells) {
            // Five floats a cell: density, temperature and the velocity.
            const std::uint64_t megabytes = (cells * 20U + ((1U << 20U) - 1U)) >> 20U;
            return Error{ErrorCode::BudgetExceeded,
                         where + " has " + std::to_string(cells) + " cells (" + std::to_string(volume.nx) + "x" +
                             std::to_string(volume.ny) + "x" + std::to_string(volume.nz) + "), more than the " +
                             std::to_string(max_volume_cells) + " one grid may hold; its state alone would need " +
                             std::to_string(megabytes) + " MB"};
        }
    }
    return std::nullopt;
}

// What a scope (Scope) may name in `emitter`: its spawn modules, the pieces
// of broken bodies where it has rigid physics, and its body kinds there. The
// problem in words, or nothing.
std::optional<std::string> scope_problem(const Emitter& emitter, const Scope& scope) {
    const RigidPhysics* physics = rigid_physics(emitter);
    for (const std::uint32_t source : scope.sources) {
        if (source == piece_spawn_source) {
            if (physics == nullptr) {
                return "names the pieces of broken bodies (" + std::to_string(piece_spawn_source) +
                       "), and emitter " + quoted(emitter.name) + " has no rigid physics to break";
            }
            continue;
        }
        if (source >= emitter.spawn.size()) {
            return "names spawn source " + std::to_string(source) + ", and emitter " + quoted(emitter.name) + " has " +
                   std::to_string(emitter.spawn.size()) + " spawn module(s), counted from 0";
        }
    }
    if (!scope.kinds.empty() && physics == nullptr) {
        return "names body kinds, and emitter " + quoted(emitter.name) +
               " has no rigid physics, so its particles are no bodies";
    }
    const std::size_t kinds = physics != nullptr ? physics->bodies.size() + 1U : 1U;
    for (const std::uint32_t kind : scope.kinds) {
        if (kind >= kinds) {
            return "names body kind " + std::to_string(kind) + ", and emitter " + quoted(emitter.name) + " has " +
                   std::to_string(kinds) + " (0 is its own)";
        }
    }
    return std::nullopt;
}

// The emitter a spawn module bears particles from must exist, be another and
// hold bodies, and what its `bodies` scope names must be that emitter's.
void check_body_source(const Effect& effect, std::size_t index, const std::string& name, const Scope& bodies,
                       FieldChecker& check) {
    std::optional<std::size_t> at;
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        if (effect.emitters[i].name == name) {
            at = i;
            break;
        }
    }
    if (!at) {
        check.require(false, "emitter", "names emitter " + quoted(name) + ", which this effect has none of");
        return;
    }
    check.require(*at != index, "emitter", "names its own emitter; particles are born of another's bodies");
    const Emitter& other = effect.emitters[*at];
    check.require(rigid_physics(other) != nullptr, "emitter",
                  "names emitter " + quoted(name) +
                      ", which has no 'rigid_physics' module, so it has no bodies to bear particles from");
    if (rigid_physics(other) != nullptr) {
        if (std::optional<std::string> problem = scope_problem(other, bodies)) {
            check.require(false, "bodies", *problem);
        }
    }
}

// The emitter a SpawnFromParticles sheds from must exist, be another and be
// a plain particle emitter, and what its `particles` scope names must be
// that emitter's spawn sources. A chain closing on itself is refused by
// check_particle_cycles, once every emitter's modules have passed.
void check_particle_source(const Effect& effect, std::size_t index, const SpawnFromParticles& m,
                           FieldChecker& check) {
    std::optional<std::size_t> at;
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        if (effect.emitters[i].name == m.emitter) {
            at = i;
            break;
        }
    }
    if (!at) {
        check.require(false, "emitter", "names emitter " + quoted(m.emitter) + ", which this effect has none of");
        return;
    }
    check.require(*at != index, "emitter",
                  "names its own emitter; particles are shed by another emitter's particles");
    const Emitter& other = effect.emitters[*at];
    check.require(rigid_physics(other) == nullptr, "emitter",
                  "names emitter " + quoted(m.emitter) +
                      ", which has a 'rigid_physics' module: its particles are bodies, which 'spawn_from_bodies' "
                      "sheds from");
    check.require(m.particles.kinds.empty(), "particles",
                  "names body kinds, and emitter " + quoted(m.emitter) + "'s particles are no bodies");
    if (rigid_physics(other) == nullptr && m.particles.kinds.empty()) {
        if (std::optional<std::string> problem = scope_problem(other, m.particles)) {
            check.require(false, "particles", *problem);
        }
    }
}

// The emitters emitter `index` sheds particles from (SpawnFromParticles),
// as indices, each once, in the order its spawn stack names them.
std::vector<std::size_t> particle_sources(const Effect& effect, std::size_t index) {
    std::vector<std::size_t> out;
    for (const SpawnModule& module : effect.emitters[index].spawn) {
        const auto* shed = std::get_if<SpawnFromParticles>(&module);
        for (std::size_t other = 0U; shed != nullptr && other < effect.emitters.size(); ++other) {
            if (effect.emitters[other].name == shed->emitter && std::find(out.begin(), out.end(), other) == out.end()) {
                out.push_back(other);
            }
        }
    }
    return out;
}

// Emitters shedding particles from one another in a ring could never agree
// which steps first, so the ring is refused, named from the emitter that
// closes it.
std::optional<Error> check_particle_cycles(const Effect& effect) {
    const std::size_t count = effect.emitters.size();
    // 0 unseen, 1 on the current path, 2 done.
    std::vector<std::uint8_t> mark(count, 0U);
    std::vector<std::size_t> path;
    std::optional<Error> error;
    const auto visit = [&](auto& self, std::size_t at) -> void {
        if (error || mark[at] == 2U) {
            return;
        }
        if (mark[at] == 1U) {
            std::string chain = quoted(effect.emitters[at].name);
            const auto start = std::find(path.begin(), path.end(), at);
            for (auto it = start + 1; it != path.end(); ++it) {
                chain += ", which sheds from " + quoted(effect.emitters[*it].name);
            }
            error = Error{ErrorCode::InvalidEffect,
                          "emitter " + chain + ", which sheds from " + quoted(effect.emitters[at].name) +
                              ": emitters shedding particles from one another in a ring cannot each step after "
                              "their source ('spawn_from_particles' field 'emitter')"};
            return;
        }
        mark[at] = 1U;
        path.push_back(at);
        for (const std::size_t source : particle_sources(effect, at)) {
            self(self, source);
        }
        path.pop_back();
        mark[at] = 2U;
    };
    for (std::size_t i = 0U; i < count && !error; ++i) {
        visit(visit, i);
    }
    return error;
}

// The bodies a wake follows: another emitter's, of no more than the grid
// they are found through holds.
void check_wake(const Effect& effect, std::size_t index, const Wake& m, FieldChecker& check) {
    std::optional<std::size_t> at;
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        if (effect.emitters[i].name == m.emitter) {
            at = i;
            break;
        }
    }
    if (!at) {
        check.require(false, "emitter", "names emitter " + quoted(m.emitter) + ", which this effect has none of");
        return;
    }
    const Emitter& other = effect.emitters[*at];
    const RigidPhysics* physics = rigid_physics(other);
    check.require(physics != nullptr, "emitter",
                  "names emitter " + quoted(m.emitter) +
                      ", which has no 'rigid_physics' module, so it has no bodies to stir anything");
    check.require(*at != index, "emitter", "names its own emitter; particles are stirred by another's bodies");
    check.require(other.capacity <= max_body_colliders, "emitter",
                  "names emitter " + quoted(m.emitter) + ", which holds up to " + std::to_string(other.capacity) +
                      " bodies; at most " + std::to_string(max_body_colliders) +
                      " may stir particles, the room their grid is given");
    if (physics != nullptr) {
        if (std::optional<std::string> problem = scope_problem(other, m.bodies)) {
            check.require(false, "bodies", *problem);
        }
    }
}

std::optional<Error> check_emitter(const Effect& effect, const Emitter& emitter, std::size_t index) {
    const std::string where = "emitter " + quoted(emitter.name);
    if (emitter.capacity == 0U || emitter.capacity > max_emitter_capacity) {
        return Error{ErrorCode::InvalidEffect,
                     where + " capacity must be between 1 and " + std::to_string(max_emitter_capacity)};
    }

    if (emitter.spawn.size() > max_spawn_modules) {
        return Error{ErrorCode::InvalidEffect, where + " has " + std::to_string(emitter.spawn.size()) +
                                                    " spawn modules; a spawn index has room for " +
                                                    std::to_string(max_spawn_modules)};
    }

    std::set<ModuleId> ids;
    std::optional<Error> error;
    bool has_lifetime = false;
    bool has_tumble = false;
    std::size_t rigid_bodies = 0U;
    std::size_t rigid_physics = 0U;
    std::size_t spins = 0U;
    std::size_t breaks = 0U;
    std::size_t glues = 0U;
    std::size_t body_winds = 0U;
    std::uint64_t piled = 0U;
    std::size_t action_sources = 0U;
    std::uint32_t action_alive = 0U;
    const auto visit_module = [&](const auto& module) {
        if (error) {
            return;
        }
        if (module.id == 0U || !ids.insert(module.id).second) {
            error = Error{ErrorCode::InvalidEffect, where + " module " + quoted(kind_of(module)) +
                                                        " needs an id that is non-zero and unique in its emitter"};
            return;
        }
        using M = std::decay_t<decltype(module)>;
        if constexpr (std::is_same_v<M, Lifetime>) {
            has_lifetime = true;
        } else if constexpr (std::is_same_v<M, InitTumble>) {
            has_tumble = true;
        } else if constexpr (std::is_same_v<M, RigidBody>) {
            ++rigid_bodies;
        } else if constexpr (std::is_same_v<M, RigidPhysics>) {
            ++rigid_physics;
        } else if constexpr (std::is_same_v<M, Break>) {
            ++breaks;
        } else if constexpr (std::is_same_v<M, Glue>) {
            ++glues;
        } else if constexpr (std::is_same_v<M, BodyWind>) {
            ++body_winds;
        } else if constexpr (std::is_same_v<M, InitSpin>) {
            ++spins;
        } else if constexpr (std::is_same_v<M, SpawnPile>) {
            piled += module.count;
        } else if constexpr (std::is_same_v<M, SpawnActions>) {
            ++action_sources;
            action_alive = module.max_alive;
        }
        error = check_module_fields(effect, emitter, module);
    };
    for (const SpawnModule& module : emitter.spawn) {
        std::visit(visit_module, module);
    }
    for (const InitModule& module : emitter.init) {
        std::visit(visit_module, module);
    }
    for (const UpdateModule& module : emitter.update) {
        std::visit(visit_module, module);
    }
    if (error) {
        return error;
    }
    if (rigid_bodies > 1U) {
        return Error{ErrorCode::InvalidEffect, where + " has more than one 'rigid_body' module"};
    }
    if (spins > 1U) {
        return Error{ErrorCode::InvalidEffect, where + " has more than one 'spin' module"};
    }
    if (rigid_physics > 1U) {
        return Error{ErrorCode::InvalidEffect, where + " has more than one 'rigid_physics' module"};
    }
    if (rigid_physics == 1U && rigid_bodies > 0U) {
        return Error{ErrorCode::InvalidEffect,
                     where + " has both 'rigid_body' and 'rigid_physics'; choose the GPU tumble or the physics engine"};
    }
    if (has_tumble && rigid_bodies == 0U && rigid_physics == 0U) {
        return Error{ErrorCode::InvalidEffect,
                     where + " has a 'tumble' module but no 'rigid_body' or 'rigid_physics' module to turn it"};
    }
    if (rigid_physics == 0U) {
        for (const SpawnModule& module : emitter.spawn) {
            if (std::visit([](const auto& m) { return spawn_body(m); }, module) != 0U) {
                return Error{ErrorCode::InvalidEffect,
                             where + " module " + quoted(std::visit([](const auto& m) { return kind_of(m); }, module)) +
                                 " spawns a body kind, and the emitter has no 'rigid_physics' module to make "
                                 "bodies"};
            }
        }
    }
    for (const UpdateModule& module : emitter.update) {
        if (const auto* meets = std::get_if<BodyCollision>(&module)) {
            FieldChecker checker{effect, "emitter " + quoted(emitter.name) + " module 'body_collision' #" +
                                             std::to_string(meets->id)};
            check_body_collision(effect, index, *meets, checker);
            if (checker.error()) {
                return checker.error();
            }
        }
        if (const auto* stirred = std::get_if<Wake>(&module)) {
            FieldChecker checker{effect, "emitter " + quoted(emitter.name) + " module 'wake' #" +
                                             std::to_string(stirred->id)};
            check_wake(effect, index, *stirred, checker);
            if (checker.error()) {
                return checker.error();
            }
        }
    }
    if (glues > 1U) {
        return Error{ErrorCode::InvalidEffect, where + " has more than one 'glue' module"};
    }
    if (glues > 0U && rigid_physics == 0U) {
        return Error{ErrorCode::InvalidEffect,
                     where + " has a 'glue' module but no 'rigid_physics' module: only bodies in a physics "
                             "world can be held to each other"};
    }
    if (body_winds > 0U && rigid_physics == 0U) {
        return Error{ErrorCode::InvalidEffect,
                     where + " has a 'body_wind' module but no 'rigid_physics' module: it blows on bodies in a "
                             "physics world; a particle emitter takes wind as a gravity or a drag"};
    }
    if (body_winds > max_body_winds) {
        return Error{ErrorCode::InvalidEffect, where + " has more than " + std::to_string(max_body_winds) +
                                                   " 'body_wind' modules"};
    }
    if (breaks > 0U && rigid_physics == 0U) {
        return Error{ErrorCode::InvalidEffect, where +
                                                   " has a 'break' module but no 'rigid_physics' module: only a "
                                                   "body in a physics world can come apart"};
    }
    if (action_sources > 1U) {
        return Error{ErrorCode::InvalidEffect,
                     where + " has more than one 'spawn_actions' module: an emitter takes its actions through one"};
    }
    if (action_sources == 1U && rigid_physics == 1U && action_alive > emitter.capacity) {
        return Error{ErrorCode::InvalidEffect, where + " module 'spawn_actions' keeps up to " +
                                                   std::to_string(action_alive) +
                                                   " bodies alive, more than its capacity of " +
                                                   std::to_string(emitter.capacity)};
    }
    if (piled > 0U && rigid_physics == 0U) {
        return Error{ErrorCode::InvalidEffect,
                     where + " has a 'spawn_pile' module but no 'rigid_physics' module: a pile stands on its bodies"};
    }
    if (piled > emitter.capacity) {
        return Error{ErrorCode::InvalidEffect, where + " piles " + std::to_string(piled) +
                                                    " bodies, more than its capacity of " +
                                                    std::to_string(emitter.capacity)};
    }
    if (rigid_physics == 1U) {
        if (std::optional<Error> rigid = check_rigid_physics(emitter, where)) {
            return rigid;
        }
    }
    // What every scope names, and the emitters spawn modules bear from.
    std::optional<Error> scoped;
    const auto check_scoped = [&](const auto& m) {
        using M = std::decay_t<decltype(m)>;
        static_cast<void>(m);
        if (scoped) {
            return;
        }
        if constexpr (requires { m.only; }) {
            if (std::optional<std::string> problem = scope_problem(emitter, m.only)) {
                scoped = Error{ErrorCode::InvalidEffect, where + " module " + quoted(kind_of(m)) + " #" +
                                                             std::to_string(m.id) + " field 'only' " + *problem};
                return;
            }
        }
        if constexpr (std::is_same_v<M, SpawnFromContacts> || std::is_same_v<M, SpawnFromBodies>) {
            FieldChecker checker{effect, where + " module " + quoted(kind_of(m)) + " #" + std::to_string(m.id)};
            check_body_source(effect, index, m.emitter, m.bodies, checker);
            if (checker.error()) {
                scoped = checker.error();
            }
        }
        if constexpr (std::is_same_v<M, SpawnFromParticles>) {
            FieldChecker checker{effect, where + " module " + quoted(kind_of(m)) + " #" + std::to_string(m.id)};
            check_particle_source(effect, index, m, checker);
            if (checker.error()) {
                scoped = checker.error();
            }
        }
    };
    for (const SpawnModule& module : emitter.spawn) {
        std::visit(check_scoped, module);
    }
    for (const InitModule& module : emitter.init) {
        std::visit(check_scoped, module);
    }
    for (const UpdateModule& module : emitter.update) {
        std::visit(check_scoped, module);
    }
    if (scoped) {
        return scoped;
    }

    FieldChecker renderer{effect, where + " renderer", true};
    SpriteRenderer::fields(emitter.renderer, renderer);
    for (const auto& [field, input] : {std::pair{"brightness", &emitter.renderer.brightness},
                                       std::pair{"opacity", &emitter.renderer.opacity},
                                       std::pair{"size_scale", &emitter.renderer.size_scale}}) {
        renderer.require(range(effect, *input).lo >= 0.0F, field, "must not be negative");
    }
    renderer.require(emitter.renderer.flipbook_columns >= 1U && emitter.renderer.flipbook_rows >= 1U, "flipbook",
                     "needs at least one column and row");
    renderer.require(emitter.renderer.stretch >= 0.0F, "stretch", "must not be negative");
    renderer.require(emitter.renderer.soft_distance >= 0.0F, "soft_distance", "must not be negative");
    const SpriteRenderer& sprite = emitter.renderer;
    renderer.require(sprite.light_count <= 64U, "light_count", "must be at most 64");
    renderer.require(sprite.light_intensity_min >= 0.0F && sprite.light_intensity_max >= sprite.light_intensity_min,
                     "light_intensity", "must not be negative, with max at least min");
    renderer.require(sprite.light_count == 0U ||
                         (sprite.light_range_min > 0.0F && sprite.light_range_max >= sprite.light_range_min),
                     "light_range", "must be positive, with max at least min");
    renderer.require(sprite.light_colors.size() <= SpriteRenderer::max_light_keys, "light_colors",
                     "has more than " + std::to_string(SpriteRenderer::max_light_keys) + " keys");
    renderer.require(sprite.light_curve.size() <= SpriteRenderer::max_light_keys, "light_curve",
                     "has more than " + std::to_string(SpriteRenderer::max_light_keys) + " keys");
    for (const CurveKey& key : sprite.light_curve) {
        renderer.require(key.value >= 0.0F, "light_curve", "must not go negative");
    }
    if (emitter.renderer.geometry == Geometry::Mesh) {
        renderer.require(valid_texture_name(emitter.renderer.mesh), "mesh",
                         "must be 1 to 64 characters of a-z, 0-9, '_', '-' and '.'");
        renderer.require(emitter.renderer.stretch == 0.0F && emitter.renderer.flipbook_columns == 1U &&
                             emitter.renderer.flipbook_rows == 1U && !emitter.renderer.flipbook_blend,
                         "geometry", "mesh takes its texture by its own coordinates: no stretch or flipbook");
        renderer.require(rigid_bodies == 0U || emitter.renderer.facing == Facing::Camera, "facing",
                         "rigid mesh particles turn by their orientation: no velocity facing");
    } else {
        renderer.require(emitter.renderer.mesh.empty(), "mesh", "is only for mesh geometry");
    }
    if (emitter.renderer.geometry == Geometry::Ribbon) {
        renderer.require(emitter.renderer.facing == Facing::Camera && emitter.renderer.stretch == 0.0F, "geometry",
                         "ribbon faces the camera: no velocity facing or stretch");
        renderer.require(emitter.renderer.flipbook_columns == 1U && emitter.renderer.flipbook_rows == 1U &&
                             !emitter.renderer.flipbook_blend,
                         "geometry", "ribbon runs its texture along its length: no flipbook");
    }
    renderer.require(emitter.renderer.texture.empty() || valid_texture_name(emitter.renderer.texture), "texture",
                     "must be 1 to 64 characters of a-z, 0-9, '_', '-' and '.'");
    renderer.require(!emitter.renderer.texture.empty() ||
                         (emitter.renderer.flipbook_columns == 1U && emitter.renderer.flipbook_rows == 1U &&
                          !emitter.renderer.flipbook_blend),
                     "flipbook", "needs a texture");
    // Lighting: a rounded sprite or a lit mesh; six-way maps are sprites
    // with both textures. A ribbon has no frame to light in.
    renderer.require(sprite.curvature >= 0.0F && sprite.curvature <= 1.0F, "curvature", "must be 0 to 1");
    renderer.require(sprite.wrap >= 0.0F && sprite.wrap <= 1.0F, "wrap", "must be 0 to 1");
    renderer.require(sprite.ambient >= 0.0F, "ambient", "must not be negative");
    renderer.require(sprite.lighting == Lighting::Unlit || sprite.geometry != Geometry::Ribbon, "lighting",
                     "a ribbon is unlit");
    if (sprite.lighting == Lighting::SixWay) {
        renderer.require(sprite.geometry == Geometry::Sprites, "lighting", "six-way maps light sprites only");
        renderer.require(!sprite.texture.empty(), "lighting", "six-way maps need a texture (right, up, front)");
        renderer.require(!sprite.lightmap.empty() && valid_texture_name(sprite.lightmap), "lightmap",
                         "six-way maps need a second texture (left, down, behind) named like any texture");
    } else {
        renderer.require(sprite.lightmap.empty(), "lightmap", "is only for six-way lighting");
    }
    if (renderer.error()) {
        return renderer.error();
    }

    if (!emitter.spawn.empty() && !has_lifetime) {
        return Error{ErrorCode::InvalidEffect, where + " spawns particles but has no 'lifetime' module"};
    }
    // Every spawn module's particles need a life: some Lifetime in scope of
    // them, unscoped or naming their source and kind.
    for (std::size_t slot = 0U; slot < emitter.spawn.size(); ++slot) {
        const SpawnModule& spawn = emitter.spawn[slot];
        const std::uint32_t kind = std::visit([](const auto& m) { return spawn_body(m); }, spawn);
        const std::uint32_t first = spawn_index_of(static_cast<std::uint32_t>(slot), 0U);
        const bool lives = std::any_of(emitter.init.begin(), emitter.init.end(), [&](const InitModule& module) {
            const auto* lifetime = std::get_if<Lifetime>(&module);
            return lifetime != nullptr && in_scope(lifetime->only, first, kind);
        });
        if (!lives) {
            return Error{ErrorCode::InvalidEffect,
                         where + " module " + quoted(std::visit([](const auto& m) { return kind_of(m); }, spawn)) +
                             " #" + std::to_string(std::visit([](const auto& m) { return m.id; }, spawn)) +
                             " bears particles that no 'lifetime' module is in scope of"};
        }
    }

    const std::uint64_t bound = particle_bound(effect, emitter);
    if (bound > emitter.capacity) {
        const std::string needed =
            bound == std::numeric_limits<std::uint64_t>::max() ? std::string{"an unbounded number of"} : std::to_string(bound);
        return Error{ErrorCode::BudgetExceeded, where + " can have up to " + needed +
                                                    " particles alive but its capacity is " +
                                                    std::to_string(emitter.capacity)};
    }
    return std::nullopt;
}

} // namespace

std::uint64_t particle_bound(const Effect& effect, const Emitter& emitter) {
    // Survivors of a step have age below their lifetime; newborns have age
    // below one step. Every live particle was therefore born within the last
    // max(lifetime, step) seconds.
    double longest_life = 0.0;
    for (const InitModule& module : emitter.init) {
        if (const Lifetime* lifetime = std::get_if<Lifetime>(&module)) {
            longest_life = std::max({longest_life, static_cast<double>(range(effect, lifetime->min).hi),
                                     static_cast<double>(range(effect, lifetime->max).hi)});
        }
    }
    const double step = 1.0 / static_cast<double>(std::max(effect.simulation_rate, 1U));
    const double window = std::max(longest_life, step);

    double bound = 0.0;
    for (const SpawnModule& module : emitter.spawn) {
        if (const SpawnRate* rate = std::get_if<SpawnRate>(&module)) {
            // A rate emitter carries less than one birth between steps, and
            // float rounding at the window's edges can add one more.
            bound += std::ceil(static_cast<double>(range(effect, rate->rate).hi) * window) + 2.0;
        } else if (const SpawnBurst* burst = std::get_if<SpawnBurst>(&module)) {
            double occurrences = 1.0;
            if (burst->interval > 0.0F && burst->cycles != 1U) {
                occurrences = std::floor(window / static_cast<double>(burst->interval)) + 1.0;
                if (burst->cycles != 0U) {
                    occurrences = std::min(occurrences, static_cast<double>(burst->cycles));
                }
            }
            bound += occurrences * static_cast<double>(burst->count);
        } else if (const SpawnPile* pile = std::get_if<SpawnPile>(&module)) {
            bound += static_cast<double>(pile->count);
        } else if (const SpawnActions* actions = std::get_if<SpawnActions>(&module);
                   actions != nullptr && rigid_physics(emitter) != nullptr) {
            // Bodies born of actions are refused past max_alive.
            bound += static_cast<double>(actions->max_alive);
        } else {
            // Contacts and moving bodies bear at most their cap a step, in
            // every step the window holds and the one it starts in.
            const std::uint32_t cap = std::visit(
                [](const auto& m) -> std::uint32_t {
                    if constexpr (requires { m.max_per_step; }) {
                        return m.max_per_step;
                    } else {
                        static_cast<void>(m);
                        return 0U;
                    }
                },
                module);
            const double steps = std::ceil(window * static_cast<double>(std::max(effect.simulation_rate, 1U))) + 1.0;
            bound += steps * static_cast<double>(cap);
        }
    }
    if (!(bound < 1.8e19)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(bound);
}

std::uint32_t light_spacing(const Effect& effect, const Emitter& emitter) {
    const std::uint64_t lights = emitter.renderer.light_count;
    if (lights == 0U) {
        return 1U;
    }
    const std::uint64_t alive = std::min<std::uint64_t>(particle_bound(effect, emitter), emitter.capacity);
    return static_cast<std::uint32_t>(std::max<std::uint64_t>((alive + lights - 1U) / lights, 1U));
}

Result<bool> validate(const Effect& effect) {
    if (effect.name.empty()) {
        return Error{ErrorCode::InvalidEffect, "the effect has no name"};
    }
    if (effect.simulation_rate == 0U || effect.simulation_rate > max_simulation_rate) {
        return Error{ErrorCode::InvalidEffect,
                     "simulation rate must be between 1 and " + std::to_string(max_simulation_rate) + " steps per second"};
    }
    if (std::optional<Error> error = check_parameters(effect)) {
        return *std::move(error);
    }
    if (std::optional<Error> error = check_blasts(effect)) {
        return *std::move(error);
    }
    if (std::optional<Error> error = check_volumes(effect)) {
        return *std::move(error);
    }
    std::set<std::string_view> names;
    for (const Emitter& emitter : effect.emitters) {
        if (emitter.name.empty()) {
            return Error{ErrorCode::InvalidEffect, "an emitter has no name"};
        }
        if (!names.insert(emitter.name).second) {
            return Error{ErrorCode::InvalidEffect, "emitter " + quoted(emitter.name) + " is declared twice"};
        }
        if (std::optional<Error> error =
                check_emitter(effect, emitter, static_cast<std::size_t>(&emitter - effect.emitters.data()))) {
            return *std::move(error);
        }
    }
    if (std::optional<Error> error = check_particle_cycles(effect)) {
        return *std::move(error);
    }
    return true;
}

std::vector<std::size_t> step_order(const Effect& effect) {
    const std::size_t count = effect.emitters.size();
    std::vector<std::size_t> order;
    std::vector<bool> placed(count, false);
    for (std::size_t i = 0U; i < count; ++i) {
        if (rigid_physics(effect.emitters[i]) != nullptr) {
            order.push_back(i);
            placed[i] = true;
        }
    }
    // The lowest index whose particle sources have all stepped, until none
    // is left or the rest wait on one another.
    while (order.size() < count) {
        std::optional<std::size_t> next;
        for (std::size_t i = 0U; i < count && !next; ++i) {
            if (placed[i]) {
                continue;
            }
            const std::vector<std::size_t> sources = particle_sources(effect, i);
            if (std::all_of(sources.begin(), sources.end(), [&](std::size_t s) { return placed[s] || s == i; })) {
                next = i;
            }
        }
        if (!next) {
            break;
        }
        order.push_back(*next);
        placed[*next] = true;
    }
    return order;
}

std::uint32_t required_attributes(const Emitter& emitter) {
    std::uint32_t mask = static_cast<std::uint32_t>(Attribute::Position) |
                         static_cast<std::uint32_t>(Attribute::Age) | static_cast<std::uint32_t>(Attribute::Lifetime);
    const auto add = [&mask](Attribute attribute) { mask |= static_cast<std::uint32_t>(attribute); };
    for (const InitModule& module : emitter.init) {
        std::visit(
            [&](const auto& m) {
                using M = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<M, Velocity>) {
                    add(Attribute::Velocity);
                } else if constexpr (std::is_same_v<M, InitColor>) {
                    add(Attribute::Color);
                } else if constexpr (std::is_same_v<M, InitSize>) {
                    add(Attribute::Size);
                } else if constexpr (std::is_same_v<M, InitRotation>) {
                    add(Attribute::Rotation);
                } else if constexpr (std::is_same_v<M, InitTumble>) {
                    add(Attribute::Orientation);
                    add(Attribute::AngularVelocity);
                } else if constexpr (std::is_same_v<M, InitSpin>) {
                    add(Attribute::Rotation);
                    add(Attribute::Spin);
                }
            },
            module);
    }
    for (const SpawnModule& module : emitter.spawn) {
        // A particle born of a contact, a moving body or a moving particle
        // may be thrown by it.
        if (std::holds_alternative<SpawnFromContacts>(module) || std::holds_alternative<SpawnFromBodies>(module) ||
            std::holds_alternative<SpawnFromParticles>(module) || std::holds_alternative<SpawnActions>(module)) {
            add(Attribute::Velocity);
        }
    }
    for (const UpdateModule& module : emitter.update) {
        std::visit(
            [&](const auto& m) {
                using M = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<M, ColorOverLife> || std::is_same_v<M, Heat>) {
                    add(Attribute::Color);
                } else if constexpr (std::is_same_v<M, SizeOverLife>) {
                    add(Attribute::Size);
                } else if constexpr (std::is_same_v<M, PlaneCollision> || std::is_same_v<M, SphereCollision> ||
                                     std::is_same_v<M, BoxCollision> || std::is_same_v<M, SdfCollision>) {
                    // Collision only reads velocity; it has none to bounce
                    // unless something else gave it one.
                } else if constexpr (std::is_same_v<M, RigidBody> || std::is_same_v<M, RigidPhysics>) {
                    add(Attribute::Velocity);
                    add(Attribute::Orientation);
                    add(Attribute::AngularVelocity);
                } else {
                    add(Attribute::Velocity);
                }
            },
            module);
    }
    if (emitter.renderer.facing == Facing::Velocity) {
        add(Attribute::Velocity);
    }
    return mask;
}

Quat prop_orientation(Vec3 degrees) noexcept {
    constexpr float half_degree = std::numbers::pi_v<float> / 360.0F;
    const Quat yaw{0.0F, std::sin(degrees.y * half_degree), 0.0F, std::cos(degrees.y * half_degree)};
    const Quat pitch{std::sin(degrees.x * half_degree), 0.0F, 0.0F, std::cos(degrees.x * half_degree)};
    const Quat roll{0.0F, 0.0F, std::sin(degrees.z * half_degree), std::cos(degrees.z * half_degree)};
    return times(roll, times(pitch, yaw));
}

const Blast* find_blast(const Effect& effect, std::string_view name) noexcept {
    for (const Blast& blast : effect.blasts) {
        if (blast.name == name) {
            return &blast;
        }
    }
    return nullptr;
}

// Operation for operation as ajfx_shockwave (shaders/ajfx_particles.hlsli)
// works it out, so the GPU and the reference throw alike.
BlastPush blast_push(const Blast& blast, Vec3 at) noexcept {
    const Vec3 d = at - blast.center;
    const float distance = std::sqrt(dot(d, d));
    if (!(distance < blast.radius)) {
        return {};
    }
    const Vec3 out = distance > 1.0e-6F ? d * (1.0F / distance) : Vec3{0.0F, 1.0F, 0.0F};
    return BlastPush{out, blast.power * (1.0F - distance / blast.radius)};
}

std::uint64_t blast_tick(const Blast& blast, std::uint32_t simulation_rate) noexcept {
    return static_cast<std::uint64_t>(std::ceil(static_cast<double>(blast.time) * simulation_rate));
}

Vec3 body_wind_at(const BodyWind& wind, std::uint64_t tick, std::uint32_t simulation_rate) noexcept {
    if (!(wind.gust > 0.0F) || !(wind.gust_seconds > 0.0F) || simulation_rate == 0U) {
        return wind.velocity;
    }
    // Smooth value noise over time, a lattice point every gust_seconds, in
    // [-1, 1]: worked out from the step alone, in doubles to the lattice and
    // floats after it, so every node and every seek blows alike.
    const double at = static_cast<double>(tick) / (static_cast<double>(simulation_rate) * wind.gust_seconds);
    const double whole = std::floor(at);
    const auto lattice = static_cast<std::uint32_t>(static_cast<std::uint64_t>(whole) & 0xffffffffULL);
    const auto point = [&](std::uint32_t k) { return hash_unit(hash_mix(hash_mix(0x9e3779b9U, wind.id), k)); };
    const float t = static_cast<float>(at - whole);
    const float s = t * t * (3.0F - 2.0F * t);
    const float a = point(lattice);
    const float b = point(lattice + 1U);
    const float noise = (a + (b - a) * s) * 2.0F - 1.0F;
    return wind.velocity * (1.0F + wind.gust * noise);
}

const Volume* find_volume(const Effect& effect, std::string_view name) noexcept {
    for (const Volume& volume : effect.volumes) {
        if (volume.name == name) {
            return &volume;
        }
    }
    return nullptr;
}

std::optional<std::size_t> volume_index(const Effect& effect, std::string_view name) noexcept {
    for (std::size_t i = 0U; i < effect.volumes.size(); ++i) {
        if (effect.volumes[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

bool uses_volumes(const Emitter& emitter) noexcept {
    return std::any_of(emitter.update.begin(), emitter.update.end(), [](const UpdateModule& module) {
        return std::holds_alternative<VolumeAdvect>(module) || std::holds_alternative<VolumeSource>(module);
    });
}

std::optional<float> contact_floor(const Effect& effect, std::size_t index) {
    if (index >= effect.emitters.size()) {
        return std::nullopt;
    }
    std::optional<float> slowest;
    const auto lower = [&slowest](float speed) { slowest = slowest ? std::min(*slowest, speed) : speed; };
    for (const UpdateModule& module : effect.emitters[index].update) {
        if (const auto* breaking = std::get_if<Break>(&module)) {
            lower(breaking->threshold);
        }
    }
    const std::string& name = effect.emitters[index].name;
    for (std::size_t i = 0U; i < effect.emitters.size(); ++i) {
        if (i == index) {
            continue;
        }
        for (const SpawnModule& module : effect.emitters[i].spawn) {
            if (const auto* reads = std::get_if<SpawnFromContacts>(&module); reads != nullptr && reads->emitter == name) {
                lower(reads->min_speed);
            }
        }
    }
    return slowest;
}

const RigidPhysics* rigid_physics(const Emitter& emitter) noexcept {
    for (const UpdateModule& module : emitter.update) {
        if (const auto* physics = std::get_if<RigidPhysics>(&module)) {
            return physics;
        }
    }
    return nullptr;
}

bool models_by_kind(const Emitter& emitter) noexcept {
    if (emitter.renderer.geometry != Geometry::Mesh) {
        return false;
    }
    const RigidPhysics* physics = rigid_physics(emitter);
    return physics != nullptr && std::any_of(physics->bodies.begin(), physics->bodies.end(),
                                             [](const RigidBodyKind& kind) { return !kind.draw.empty() || kind.scale != 1.0F || kind.draw_tint != Color{}; });
}

bool has_rigid_physics(const Effect& effect) noexcept {
    return std::any_of(effect.emitters.begin(), effect.emitters.end(),
                       [](const Emitter& emitter) { return rigid_physics(emitter) != nullptr; });
}

Effect rigid_physics_part(const Effect& effect) {
    Effect part = effect;
    part.emitters.clear();
    for (const Emitter& emitter : effect.emitters) {
        if (rigid_physics(emitter) != nullptr) {
            part.emitters.push_back(emitter);
        }
    }
    // Bodies neither feed nor follow a grid, so the part carries none.
    part.volumes.clear();
    return part;
}

} // namespace ajfx
