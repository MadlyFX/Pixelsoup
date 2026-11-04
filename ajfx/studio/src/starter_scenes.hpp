#pragma once

#include "document.hpp"

#include <optional>
#include <span>
#include <string_view>

// Whole scenes to start from, by name: what `ajfx-studio-cli new` makes, and
// the scenes the compiler is tested on. Between them they reach every kind of
// scene item, Motion behavior, force, rule and Object setting the compiler
// turns into modules.
namespace ajfx::studio {

struct StarterScene final {
    std::string_view id;
    // A few words on what it is, for the command line's help.
    std::string_view description;
};

[[nodiscard]] std::span<const StarterScene> starter_scenes();

// A new scene of that name; nothing for a name there is none of.
[[nodiscard]] std::optional<Document> make_starter_scene(std::string_view id);

} // namespace ajfx::studio
