#pragma once

#include "document.hpp"
#include "json.hpp"

#include <ajfx/modules.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

// Raw ajfx modules, for the Module block and for showing what a page compiles
// to. Everything here is written against each module's own `fields` list, so
// a field added to a module in ajfx appears in Studio without a change here.
namespace ajfx::studio {

struct RawKindDef final {
    std::string_view kind;
    std::string_view title;
    std::string_view stage; // "spawn", "init" or "update"
    RawModule (*make)();
};

// Every built-in module, by stage.
[[nodiscard]] const std::vector<RawKindDef>& raw_kinds();
[[nodiscard]] const RawKindDef* find_raw_kind(std::string_view kind);

[[nodiscard]] std::string_view raw_kind(const RawModule& module);
[[nodiscard]] std::string_view raw_stage(const RawModule& module);
[[nodiscard]] std::string_view raw_title(const RawModule& module);
[[nodiscard]] ModuleId raw_id(const RawModule& module);
void set_raw_id(RawModule& module, ModuleId id);

// "rate 30 · count 4", every field, for reading under the hood.
[[nodiscard]] std::string describe_module(const RawModule& module);
[[nodiscard]] std::string describe_renderer(const SpriteRenderer& renderer);

[[nodiscard]] json::Value raw_to_json(const RawModule& module);
[[nodiscard]] std::optional<RawModule> raw_from_json(const json::Value& value, std::string& error);

// Calls visit(name, field&) for each field of the module.
template <class Visitor>
void visit_fields(RawModule& module, Visitor&& visit) {
    std::visit(
        [&](auto& stage) {
            std::visit([&](auto& typed) { std::decay_t<decltype(typed)>::fields(typed, visit); }, stage);
        },
        module);
}

template <class Visitor>
void visit_fields(const RawModule& module, Visitor&& visit) {
    std::visit(
        [&](const auto& stage) {
            std::visit([&](const auto& typed) { std::decay_t<decltype(typed)>::fields(typed, visit); }, stage);
        },
        module);
}

template <class E>
concept NamedEnum = std::is_enum_v<E> && requires { EnumNames<E>::names; };

} // namespace ajfx::studio
