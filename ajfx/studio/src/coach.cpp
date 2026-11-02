#include "coach.hpp"

#include "catalog.hpp"
#include "raw_modules.hpp"

#include <algorithm>

namespace ajfx::studio {
namespace {

std::size_t count_of(const Layer& layer, std::string_view kind) {
    return static_cast<std::size_t>(std::count_if(layer.blocks.begin(), layer.blocks.end(), [kind](const Block& b) {
        return b.enabled && b.kind == kind;
    }));
}

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
        // A prop asked to be something it cannot stops the whole layer
        // compiling, and the reason reads better here, on the block at fault
        // and with the fix, than passed through in the runtime's own words.
        bool impossible_prop = false;
        for (const Block& block : layer.blocks) {
            if (!block.enabled || block.kind != "prop") {
                continue;
            }
            // A collision mesh cannot move at all: it has no volume to take
            // mass from and no inside, so contact against a moving one is
            // unreliable. The simple shapes are solid pieces, and those move.
            if (choice_of(block, "shape") == "model" && choice_of(block, "motion") != "fixed") {
                notes.push_back({layer.uid, block.uid, AdviceLevel::Error,
                                 "A prop that moves cannot collide as its own shape: only a prop that stays put "
                                 "can. Collide as a bucket, crate, tube, ball or block instead, or set it to "
                                 "stay put.",
                                 "Collide as a bucket", set_value(block.uid, "shape", choice("bucket"))});
                impossible_prop = true;
            }
        }
        if (impossible_prop) {
            continue;
        }

        const LayerReport* report = compiled.layer(layer.uid);
        if (report != nullptr && !report->error.empty()) {
            notes.push_back({layer.uid, report->error_block, AdviceLevel::Error,
                             "This layer cannot play: " + report->error + ".", {}, {}});
            continue;
        }

        // Landing on pieces lands on the rigid layer above, so there has to
        // be one, and it has to be small enough to meet every particle.
        if (const Block* lands = first(layer, "pieces")) {
            const Layer* above = nullptr;
            for (const Layer& other : document.layers) {
                if (other.uid == layer.uid) {
                    break;
                }
                if (other.enabled && other.kind == LayerKind::Rigid) {
                    above = &other;
                }
            }
            if (above == nullptr) {
                notes.push_back({layer.uid, lands->uid, AdviceLevel::Warning,
                                 "There is no physics layer above this one, so there are no pieces to land on "
                                 "and these particles fall through. Move a rigid layer above it, or start one.",
                                 {},
                                 {}});
            } else if (const LayerReport* theirs = compiled.layer(above->uid);
                       theirs != nullptr && theirs->capacity > ajfx::max_body_colliders) {
                notes.push_back({layer.uid, lands->uid, AdviceLevel::Error,
                                 "The layer above holds up to " + std::to_string(theirs->capacity) +
                                     " pieces, and landing on them means meeting every one: at most " +
                                     std::to_string(ajfx::max_body_colliders) +
                                     " can be landed on. Make that layer's pieces fewer.",
                                 {},
                                 {}});
            }
        }

        // Breaking is two halves: when a piece comes apart, and what it
        // comes apart into. Either alone does nothing, so say which is
        // missing on the block that is there.
        const Block* breaking = first(layer, "break");
        const Block* part = first(layer, "part");
        const bool cutting = breaking != nullptr && choice_of(*breaking, "into") == "cut";
        // A piece that comes apart needs room for what it comes apart into,
        // or the break is refused when it happens and nothing says why.
        if (breaking != nullptr) {
            const std::size_t into =
                cutting ? static_cast<std::size_t>(number_of(*breaking, "cells")) : count_of(layer, "part");
            const Block* pile = first(layer, "pile");
            const LayerReport* room = compiled.layer(layer.uid);
            const std::size_t pieces = pile != nullptr ? static_cast<std::size_t>(number_of(*pile, "count")) : 0U;
            if (into > 0U && pieces > 0U && room != nullptr && pieces * into > room->capacity) {
                notes.push_back({layer.uid, breaking->uid, AdviceLevel::Warning,
                                 "If every piece came apart there would be " + std::to_string(pieces * into) +
                                     " of them, and this layer holds " + std::to_string(room->capacity) +
                                     ". The ones that do not fit stay whole. Break into fewer, or start with "
                                     "fewer pieces.",
                                 {},
                                 {}});
            }
        }
        if (cutting && part != nullptr) {
            notes.push_back({layer.uid, part->uid, AdviceLevel::Warning,
                             "Breaking is set to cut the model into pieces, so the parts you place here are not "
                             "used. Set it back to parts you place, or remove them.",
                             "Use the parts", set_value(breaking->uid, "into", choice("parts"))});
        }
        if (breaking != nullptr && part == nullptr && !cutting) {
            notes.push_back({layer.uid, breaking->uid, AdviceLevel::Warning,
                             "Nothing is set to break off yet, so the pieces stay whole. Add a Part for each "
                             "panel, plank or shard a piece comes apart into.",
                             "Add a part", add_block(layer.uid, "part")});
        }
        if (part != nullptr && breaking == nullptr) {
            notes.push_back({layer.uid, part->uid, AdviceLevel::Warning,
                             "Nothing breaks, so this part is never made. Add a Break to say how hard a piece "
                             "has to be hit to come apart.",
                             "Add break", add_block(layer.uid, "break")});
        }
        // A part is drawn as its own model only where the layer draws models.
        if (part != nullptr) {
            const Block* appearance = first(layer, "appearance");
            if (appearance != nullptr && choice_of(*appearance, "material") != "debris") {
                notes.push_back({layer.uid, part->uid, AdviceLevel::Tip,
                                 "The parts hit things as their own shapes, but the layer draws its pieces as "
                                 "sprites, so they are not drawn as their models. Set Appearance to debris to "
                                 "see them.",
                                 "Draw debris", set_value(appearance->uid, "material", choice("debris"))});
            }
        }

        const bool piled = first(layer, "pile") != nullptr || has_raw(layer, "spawn_pile");
        const bool born = first(layer, "stream") != nullptr || first(layer, "burst") != nullptr || piled ||
                          has_raw(layer, "spawn_rate") || has_raw(layer, "spawn_burst");
        // A layer of props alone is a stage piece, not an empty layer: it has
        // something to show without anything being born in it.
        const bool propped = first(layer, "prop") != nullptr;
        if (!born && !propped) {
            notes.push_back({layer.uid, 0U, AdviceLevel::Warning, "Nothing is born in this layer yet.",
                             "Add a stream", add_block(layer.uid, "stream", true)});
            continue;
        }

        const Block* appearance = first(layer, "appearance");
        const std::string material = appearance != nullptr ? choice_of(*appearance, "material") : "";
        if (appearance == nullptr && born) {
            notes.push_back({layer.uid, 0U, AdviceLevel::Tip,
                             "Particles draw as plain soft dots. Appearance gives them a look: glow, smoke, debris.",
                             "Add appearance", add_block(layer.uid, "appearance")});
        }

        // A pile's pieces lie on long after the loop, so their end is not seen.
        if (born && first(layer, "fade") == nullptr && !has_raw(layer, "color_over_life") && !piled) {
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
        if (born && !document.loop_auto && layer.life_max > document.loop_seconds) {
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
            if (block.kind == "prop" && file_of(block, "model").empty()) {
                notes.push_back({layer.uid, block.uid, AdviceLevel::Warning,
                                 "Pick a model for this prop. Until then it stands in as a plain block and "
                                 "nothing is drawn.",
                                 {},
                                 {}});
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
