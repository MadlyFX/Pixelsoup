#include <ajfx/effect.hpp>

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
    FieldChecker(const Effect& effect, std::string where) : effect_{effect}, where_{std::move(where)} {}

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

void check_module(const Effect&, const Break& m, FieldChecker& check) {
    check.require(m.threshold > 0.0F, "threshold", "must be positive: a body cannot break on no impact at all");
    check.require(m.scatter >= 0.0F, "scatter", "must not be negative");
    check.require(!m.pieces.empty(), "pieces", "needs at least one piece: a body cannot break into nothing");
    check.require(m.pieces.size() <= max_break_pieces, "pieces",
                  "breaks into more than the " + std::to_string(max_break_pieces) + " pieces one body may make");
    for (std::size_t i = 0U; i < m.pieces.size(); ++i) {
        const std::string at = "pieces[" + std::to_string(i) + "]";
        check.require(m.pieces[i].size > 0.0F, at + ".size", "must be positive");
    }
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
                                     !std::is_same_v<M, Break>) {
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
        const std::uint32_t named = std::visit([](const auto& m) { return m.body; }, module);
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

std::optional<Error> check_emitter(const Effect& effect, const Emitter& emitter) {
    const std::string where = "emitter " + quoted(emitter.name);
    if (emitter.capacity == 0U || emitter.capacity > max_emitter_capacity) {
        return Error{ErrorCode::InvalidEffect,
                     where + " capacity must be between 1 and " + std::to_string(max_emitter_capacity)};
    }

    std::set<ModuleId> ids;
    std::optional<Error> error;
    bool has_lifetime = false;
    bool has_tumble = false;
    std::size_t rigid_bodies = 0U;
    std::size_t rigid_physics = 0U;
    std::size_t spins = 0U;
    std::size_t breaks = 0U;
    std::uint64_t piled = 0U;
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
        } else if constexpr (std::is_same_v<M, InitSpin>) {
            ++spins;
        } else if constexpr (std::is_same_v<M, SpawnPile>) {
            piled += module.count;
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
            if (std::visit([](const auto& m) { return m.body; }, module) != 0U) {
                return Error{ErrorCode::InvalidEffect,
                             where + " module " + quoted(std::visit([](const auto& m) { return kind_of(m); }, module)) +
                                 " spawns a body kind, and the emitter has no 'rigid_physics' module to make "
                                 "bodies"};
            }
        }
    }
    if (breaks > 0U && rigid_physics == 0U) {
        return Error{ErrorCode::InvalidEffect, where +
                                                   " has a 'break' module but no 'rigid_physics' module: only a "
                                                   "body in a physics world can come apart"};
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

    FieldChecker renderer{effect, where + " renderer"};
    SpriteRenderer::fields(emitter.renderer, renderer);
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
    if (renderer.error()) {
        return renderer.error();
    }

    if (!emitter.spawn.empty() && !has_lifetime) {
        return Error{ErrorCode::InvalidEffect, where + " spawns particles but has no 'lifetime' module"};
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
    std::set<std::string_view> names;
    for (const Emitter& emitter : effect.emitters) {
        if (emitter.name.empty()) {
            return Error{ErrorCode::InvalidEffect, "an emitter has no name"};
        }
        if (!names.insert(emitter.name).second) {
            return Error{ErrorCode::InvalidEffect, "emitter " + quoted(emitter.name) + " is declared twice"};
        }
        if (std::optional<Error> error = check_emitter(effect, emitter)) {
            return *std::move(error);
        }
    }
    return true;
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
    for (const UpdateModule& module : emitter.update) {
        std::visit(
            [&](const auto& m) {
                using M = std::decay_t<decltype(m)>;
                if constexpr (std::is_same_v<M, ColorOverLife>) {
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
                                             [](const RigidBodyKind& kind) { return !kind.draw.empty(); });
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
    return part;
}

} // namespace ajfx
