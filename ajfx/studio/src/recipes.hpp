#pragma once

#include "document.hpp"

#include <optional>
#include <string_view>
#include <vector>

// Starting points. A recipe is a whole effect; a layer preset is one part of
// one ("Embers", "Smoke") that can be added to any page. Both are ordinary
// blocks with tuned values, so everything they do can be seen and changed.
namespace ajfx::studio {

struct Recipe final {
    std::string_view id;
    std::string_view title;
    std::string_view description;
};

struct LayerPreset final {
    std::string_view id;
    std::string_view title;
    std::string_view description;
    // What moves it: the preset's blocks are the ones that kind can hold.
    LayerKind kind{LayerKind::Particles};
    // What it is, which the layer it makes says, and which decides how the
    // page's ground, gravity and wind act on it.
    LayerWhat what{LayerWhat::Other};
};

[[nodiscard]] const std::vector<Recipe>& recipes();
[[nodiscard]] const std::vector<LayerPreset>& layer_presets();

// A new page from the recipe, or nothing for an unknown id.
[[nodiscard]] std::optional<Document> make_recipe(std::string_view id);
// A new layer from the preset, with uids from `document`.
[[nodiscard]] std::optional<Layer> make_layer_preset(Document& document, std::string_view id);

} // namespace ajfx::studio
