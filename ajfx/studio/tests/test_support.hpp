#pragma once

#include "bake.hpp"
#include "compile.hpp"
#include "page_revision.hpp"
#include "document_io.hpp"
#include "guides.hpp"
#include "library.hpp"
#include "session.hpp"
#include "starter_scenes.hpp"
#include "timeline.hpp"
#include "scene_stats.hpp"
#include "user_assets.hpp"

#include <ajfx/cache.hpp>
#include <ajfx/package.hpp>
#include <ajfx/rigid.hpp>
#include <ajfx/simulation.hpp>

#if AJFX_STUDIO_TEST_KERNELS
#include <ajfx/d3d12.hpp>
#include <ajfx/gpu/kernels.hpp>
#endif

#include <algorithm>
#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#define CHECK(...) studio_test::check(static_cast<bool>((__VA_ARGS__)), #__VA_ARGS__, __LINE__)

// What every area of ajfx Studio's headless tests uses: the checks, the
// starter scenes by name, and the lookups into a compiled effect. Each area's
// tests are in their own file (`<area>_tests.cpp`) and register themselves
// with `studio_tests.cpp`, which runs them.
namespace studio_test {

using namespace ajfx::studio;

struct Case final {
    const char* name;
    void (*run)();
};

inline void check(bool condition, const char* expression, int line) {
    if (!condition) {
        throw std::runtime_error{"line " + std::to_string(line) + ": check failed: " + expression};
    }
}

// A starter scene (starter_scenes.hpp) by name.
inline Document starter(std::string_view id) {
    std::optional<Document> document = make_starter_scene(id);
    CHECK(document.has_value());
    return std::move(*document);
}

// The first item of the scene with this name.
inline Uid item_named(const Document& document, std::string_view name) {
    for (const SceneItem& item : document.scene->items) {
        if (item.name == name) {
            return item.uid;
        }
    }
    throw std::runtime_error{"no item " + std::string{name}};
}

// The emitter an item plays in.
inline const ajfx::Emitter& emitter_of(const Compiled& compiled, Uid item) {
    const ItemReport* report = compiled.item(item);
    CHECK(report != nullptr && compiled.effect.has_value());
    for (const ajfx::Emitter& emitter : compiled.effect->emitters) {
        if (emitter.name == report->emitter) {
            return emitter;
        }
    }
    throw std::runtime_error{"item not in the effect"};
}

template <class T, class V>
struct in_variant;
template <class T, class... Ts>
struct in_variant<T, std::variant<Ts...>> : std::bool_constant<(std::is_same_v<T, Ts> || ...)> {};

template <class M>
inline const M* find_module(const ajfx::Emitter& emitter) {
    if constexpr (in_variant<M, ajfx::SpawnModule>::value) {
        for (const ajfx::SpawnModule& module : emitter.spawn) {
            if (const M* found = std::get_if<M>(&module)) {
                return found;
            }
        }
    } else if constexpr (in_variant<M, ajfx::InitModule>::value) {
        for (const ajfx::InitModule& module : emitter.init) {
            if (const M* found = std::get_if<M>(&module)) {
                return found;
            }
        }
    } else {
        for (const ajfx::UpdateModule& module : emitter.update) {
            if (const M* found = std::get_if<M>(&module)) {
                return found;
            }
        }
    }
    return nullptr;
}

// Every module of this type the emitter has, in its stack's order.
template <class M>
inline std::vector<const M*> modules_of(const ajfx::Emitter& emitter) {
    std::vector<const M*> out;
    const auto take = [&out](const auto& stack) {
        for (const auto& module : stack) {
            if (const M* found = std::get_if<M>(&module)) {
                out.push_back(found);
            }
        }
    };
    if constexpr (in_variant<M, ajfx::SpawnModule>::value) {
        take(emitter.spawn);
    } else if constexpr (in_variant<M, ajfx::InitModule>::value) {
        take(emitter.init);
    } else {
        take(emitter.update);
    }
    return out;
}

// The update stack's module kinds, in order (ajfx module `kind` names).
inline std::vector<std::string> update_kinds(const ajfx::Emitter& emitter) {
    std::vector<std::string> out;
    for (const ajfx::UpdateModule& module : emitter.update) {
        out.push_back(std::visit([](const auto& m) { return std::string{std::decay_t<decltype(m)>::kind}; }, module));
    }
    return out;
}

inline bool near(float a, float b) {
    return std::fabs(a - b) <= 1.0e-5F * std::max(1.0F, std::fabs(b));
}

inline bool near(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

inline const ajfx::ParameterDesc* parameter_of(const Compiled& compiled, std::string_view name) {
    for (const ajfx::ParameterDesc& parameter : compiled.effect->parameters) {
        if (parameter.name == name) {
            return &parameter;
        }
    }
    return nullptr;
}

} // namespace studio_test
