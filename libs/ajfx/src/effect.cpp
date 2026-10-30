#include <ajfx/effect.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <limits>
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

void check_module(const Effect& e, const SpawnRate& m, FieldChecker& check) {
    check.require(range(e, m.rate).lo >= 0.0F, "rate", "must not be negative");
}

void check_module(const Effect& e, const Lifetime& m, FieldChecker& check) {
    check.require(range(e, m.min).lo > 0.0F, "min", "must be greater than zero");
    check.require(range(e, m.max).lo > 0.0F, "max", "must be greater than zero");
}

void check_module(const Effect& e, const ShapeLocation& m, FieldChecker& check) {
    check.require(range(e, m.radius).lo >= 0.0F, "radius", "must not be negative");
    check.require(m.size.x >= 0.0F && m.size.y >= 0.0F && m.size.z >= 0.0F, "size", "must not be negative");
}

void check_module(const Effect& e, const Velocity& m, FieldChecker& check) {
    const Range spread = range(e, m.spread);
    check.require(spread.lo >= 0.0F && spread.hi <= 180.0F, "spread", "must be between 0 and 180 degrees");
}

void check_module(const Effect& e, const InitSize& m, FieldChecker& check) {
    check.require(range(e, m.min).lo >= 0.0F, "min", "must not be negative");
    check.require(range(e, m.max).lo >= 0.0F, "max", "must not be negative");
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
    const auto visit_module = [&](const auto& module) {
        if (error) {
            return;
        }
        if (module.id == 0U || !ids.insert(module.id).second) {
            error = Error{ErrorCode::InvalidEffect, where + " module " + quoted(kind_of(module)) +
                                                        " needs an id that is non-zero and unique in its emitter"};
            return;
        }
        if constexpr (std::is_same_v<std::decay_t<decltype(module)>, Lifetime>) {
            has_lifetime = true;
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

    FieldChecker renderer{effect, where + " renderer"};
    SpriteRenderer::fields(emitter.renderer, renderer);
    renderer.require(emitter.renderer.flipbook_columns >= 1U && emitter.renderer.flipbook_rows >= 1U, "flipbook",
                     "needs at least one column and row");
    renderer.require(emitter.renderer.stretch >= 0.0F, "stretch", "must not be negative");
    renderer.require(emitter.renderer.soft_distance >= 0.0F, "soft_distance", "must not be negative");
    if (emitter.renderer.geometry == Geometry::Mesh) {
        renderer.require(valid_texture_name(emitter.renderer.mesh), "mesh",
                         "must be 1 to 64 characters of a-z, 0-9, '_', '-' and '.'");
        renderer.require(emitter.renderer.stretch == 0.0F && emitter.renderer.flipbook_columns == 1U &&
                             emitter.renderer.flipbook_rows == 1U && !emitter.renderer.flipbook_blend,
                         "geometry", "mesh takes its texture by its own coordinates: no stretch or flipbook");
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
        }
    }
    if (!(bound < 1.8e19)) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return static_cast<std::uint64_t>(bound);
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

} // namespace ajfx
