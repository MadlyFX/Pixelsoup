#pragma once

#include "bake.hpp"
#include "catalog.hpp"
#include "compile.hpp"
#include "page_revision.hpp"
#include "document_io.hpp"
#include "guides.hpp"
#include "library.hpp"
#include "raw_modules.hpp"
#include "recipes.hpp"
#include "session.hpp"
#include "timeline.hpp"
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

// What every area of ajfx Studio's headless tests uses: the checks, a way
// to name a recipe's layers and blocks, and the lookups into a compiled
// effect. Each area's tests are in their own file (`<area>_tests.cpp`) and
// register themselves with `studio_tests.cpp`, which runs them.
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


inline Document recipe(std::string_view id) {
    std::optional<Document> document = make_recipe(id);
    CHECK(document.has_value());
    return std::move(*document);
}

inline Layer& layer_named(Document& document, std::string_view name) {
    for (Layer& layer : document.layers) {
        if (layer.name == name) {
            return layer;
        }
    }
    throw std::runtime_error{"no layer " + std::string{name}};
}

inline Block& block_of(Layer& layer, std::string_view kind) {
    for (Block& block : layer.blocks) {
        if (block.kind == kind) {
            return block;
        }
    }
    throw std::runtime_error{"no block " + std::string{kind}};
}

inline const ajfx::Emitter& emitter_of(const Compiled& compiled, const Layer& layer) {
    const LayerReport* report = compiled.layer(layer.uid);
    CHECK(report != nullptr && compiled.effect.has_value());
    for (const ajfx::Emitter& emitter : compiled.effect->emitters) {
        if (emitter.name == report->emitter) {
            return emitter;
        }
    }
    throw std::runtime_error{"layer not in the effect"};
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

inline bool near(float a, float b) {
    return std::fabs(a - b) <= 1.0e-5F * std::max(1.0F, std::fabs(b));
}

inline bool near(float a, float b, float tolerance) {
    return std::fabs(a - b) <= tolerance;
}

inline std::multiset<std::string> described(const Compiled& compiled, Uid layer) {
    std::multiset<std::string> out;
    for (const ModuleLine& line : compiled.layer(layer)->modules) {
        out.insert(std::string{raw_kind(line.module)} + ": " + describe_module(line.module));
    }
    return out;
}

inline const ajfx::ParameterDesc* parameter_of(const Compiled& compiled, std::string_view name) {
    for (const ajfx::ParameterDesc& parameter : compiled.effect->parameters) {
        if (parameter.name == name) {
            return &parameter;
        }
    }
    return nullptr;
}

// Whether any note says this.
} // namespace studio_test
