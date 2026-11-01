#include "coach.hpp"

#include "catalog.hpp"
#include "raw_modules.hpp"

#include <algorithm>

namespace ajfx::studio {
namespace {

const Block* first(const Layer& layer, std::string_view kind) {
    for (const Block& block : layer.blocks) {
        if (block.enabled && block.kind == kind) {
            return &block;
        }
    }
    return nullptr;
}

bool has_raw(const Layer& layer, std::string_view module_kind) {
    return std::any_of(layer.blocks.begin(), layer.blocks.end(), [&](const Block& block) {
        return block.enabled && block.raw && raw_kind(*block.raw) == module_kind;
    });
}

// Adds a block of `kind` to the layer, at the end or (with `front`) first.
std::function<void(Document&)> add_block(Uid layer, std::string kind, bool front = false,
                                         std::vector<std::pair<std::string, Value>> values = {}) {
    return [=](Document& document) {
        Layer* target = document.layer(layer);
        std::optional<Block> block = make_block(document, kind);
        if (target == nullptr || !block) {
            return;
        }
        for (const auto& [control, value] : values) {
            block->set(control, value);
        }
        insert_block(*target, front ? 0U : target->blocks.size(), std::move(*block));
    };
}

std::function<void(Document&)> set_value(Uid block, std::string control, Value value) {
    return [=](Document& document) {
        if (std::optional<Document::BlockPlace> place = document.find_block(block)) {
            place->layer->blocks[place->index].set(control, value);
        }
    };
}

std::string thousands(std::uint64_t value) {
    std::string digits = std::to_string(value);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<std::size_t>(at), ",");
    }
    return digits;
}

} // namespace

std::vector<Advice> advise(const Document& document, const Compiled& compiled) {
    std::vector<Advice> notes;
    for (const Layer& layer : document.layers) {
        if (!layer.enabled) {
            continue;
        }
        const LayerReport* report = compiled.layer(layer.uid);
        if (report != nullptr && !report->error.empty()) {
            notes.push_back({layer.uid, report->error_block, AdviceLevel::Error,
                             "This layer cannot play: " + report->error + ".", {}, {}});
            continue;
        }

        const bool piled = first(layer, "pile") != nullptr || has_raw(layer, "spawn_pile");
        const bool born = first(layer, "stream") != nullptr || first(layer, "burst") != nullptr || piled ||
                          has_raw(layer, "spawn_rate") || has_raw(layer, "spawn_burst");
        if (!born) {
            notes.push_back({layer.uid, 0U, AdviceLevel::Warning, "Nothing is born in this layer yet.",
                             "Add a stream", add_block(layer.uid, "stream", true)});
            continue;
        }

        const Block* appearance = first(layer, "appearance");
        const std::string material = appearance != nullptr ? choice_of(*appearance, "material") : "";
        if (appearance == nullptr) {
            notes.push_back({layer.uid, 0U, AdviceLevel::Tip,
                             "Particles draw as plain soft dots. Appearance gives them a look: glow, smoke, debris.",
                             "Add appearance", add_block(layer.uid, "appearance")});
        }

        // A pile's pieces lie on long after the loop, so their end is not seen.
        if (first(layer, "fade") == nullptr && !has_raw(layer, "color_over_life") && !piled) {
            notes.push_back({layer.uid, 0U, AdviceLevel::Tip,
                             "Particles vanish suddenly at the end of their life. A fade hides the pop.", "Add fade",
                             add_block(layer.uid, "fade")});
        }

        if (appearance != nullptr && material == "smoke" && number_of(*appearance, "soft") <= 0.0F) {
            notes.push_back({layer.uid, appearance->uid, AdviceLevel::Tip,
                             "Smoke cuts a hard line where it meets the floor.", "Soften edges",
                             set_value(appearance->uid, "soft", number(0.3F))});
        }

        const Block* color_block = first(layer, "color");
        if (appearance != nullptr && (material == "glow" || material == "trail") && color_block != nullptr &&
            get(*color_block, "brightness").y <= 1.0F) {
            notes.push_back({layer.uid, color_block->uid, AdviceLevel::Tip,
                             "Glow looks flat at brightness 1 or less. Brighter than 1, it blooms like real light.",
                             "Brighten", set_value(color_block->uid, "brightness", range(4.0F, 4.0F))});
        }

        const Block* launch = first(layer, "launch");
        const bool falls = first(layer, "gravity") != nullptr ||
                           (launch != nullptr && choice_of(*launch, "direction") == "down");
        const bool grounded = first(layer, "ground") != nullptr || has_raw(layer, "plane_collision");
        if (falls && !grounded && document.backdrop != Backdrop::Void) {
            notes.push_back({layer.uid, 0U, AdviceLevel::Tip, "Particles fall through the floor.", "Add ground",
                             add_block(layer.uid, "ground")});
        }

        const Block* tumble = first(layer, "tumble");
        if (tumble != nullptr && material != "debris") {
            notes.push_back({layer.uid, tumble->uid, AdviceLevel::Tip,
                             "Tumbling only shows on solid chunks: set Appearance to Debris.", "Use debris",
                             appearance != nullptr ? set_value(appearance->uid, "material", choice("debris"))
                                                   : add_block(layer.uid, "appearance", false,
                                                               {{"material", choice("debris")}})});
        }
        if (appearance != nullptr && material == "debris" && tumble == nullptr) {
            notes.push_back({layer.uid, appearance->uid, AdviceLevel::Tip,
                             "Debris reads as solid when it spins and rolls to rest.", "Add tumble",
                             add_block(layer.uid, "tumble")});
        }

        // An automatic loop always covers the effect; a loop set by hand may not.
        if (!document.loop_auto && layer.life_max > document.loop_seconds) {
            notes.push_back({layer.uid, 0U, AdviceLevel::Tip,
                             "Particles live longer than the preview loop, so it cuts them off.", "Loop automatically",
                             [](Document& d) { d.loop_auto = true; }});
        }

        // An artist's own file that is not picked yet.
        for (const Block& block : layer.blocks) {
            if (!block.enabled) {
                continue;
            }
            if (block.kind == "mesh_collider" && file_of(block, "file").empty()) {
                notes.push_back({layer.uid, block.uid, AdviceLevel::Warning,
                                 "Pick a model for particles to bounce off. Until then nothing collides.", {}, {}});
            }
            if (block.kind == "appearance") {
                const bool debris = choice_of(block, "material") == "debris";
                if (!debris && choice_of(block, "texture") == "file" && file_of(block, "texture_file").empty()) {
                    notes.push_back({layer.uid, block.uid, AdviceLevel::Tip,
                                     "Pick a picture. Until then particles draw as soft dots.", {}, {}});
                }
                if (debris && choice_of(block, "mesh") == "file" && file_of(block, "mesh_file").empty()) {
                    notes.push_back({layer.uid, block.uid, AdviceLevel::Tip,
                                     "Pick a model. Until then debris draws as chips.", {}, {}});
                }
            }
        }

        if (report != nullptr && report->capacity > 250000U) {
            notes.push_back({layer.uid, 0U, AdviceLevel::Warning,
                             "This layer can hold up to " + thousands(report->capacity) +
                                 " particles: heavy for a live show. Fewer, larger particles often look as good.",
                             {}, {}});
        }
    }
    return notes;
}

} // namespace ajfx::studio
