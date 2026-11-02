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

// The uid of the layer with this name, for a note that already found it.
Uid layer_named_uid(const Document& document, const std::string& name) {
    for (const Layer& layer : document.layers) {
        if (layer.name == name) {
            return layer.uid;
        }
    }
    return 0U;
}

std::vector<Advice> advise(const Document& document, const Compiled& compiled) {
    std::vector<Advice> notes;
    for (const Layer& layer : document.layers) {
        if (!layer.enabled) {
            continue;
        }
        // An Object asked to be something it cannot stops the whole layer
        // compiling, and the reason reads better here, on the block at fault
        // and with the fix, than passed through in the runtime's own words.
        bool impossible_object = false;
        for (const Block& block : layer.blocks) {
            if (!block.enabled || block.kind != "object") {
                continue;
            }
            const std::string motion = choice_of(block, "motion");
            const std::string collides = choice_of(block, "collides");
            const bool hollow = collides == "bucket" || collides == "crate" || collides == "tube";
            const bool many = choice_of(block, "arrange") != "one";
            // A collision mesh cannot move at all: it has no volume to take
            // mass from and no inside, so contact against a moving one is
            // unreliable. The generated shapes are solid pieces, and move.
            if (motion == "driven" && collides == "own") {
                notes.push_back({layer.uid, block.uid, AdviceLevel::Error,
                                 "An object that is moved cannot collide as its own shape: a shape taken from a "
                                 "model has no inside for the engine to move. Collide as a hollow crate, a "
                                 "bucket or a tube instead, or let it be knocked about.",
                                 "Collide as a crate", set_value(block.uid, "collides", choice("crate"))});
                impossible_object = true;
            }
            // Standing still, being driven and being hollow are a prop's,
            // and a prop is one authored thing rather than a heap of them.
            if (many && (motion != "loose" || hollow)) {
                notes.push_back({layer.uid, block.uid, AdviceLevel::Error,
                                 "Only one object on its own can stand still, be moved or be hollow. Set how "
                                 "many back to just one, or let it be knocked about and collide as its own "
                                 "shape, a ball or a block.",
                                 "Just one", set_value(block.uid, "arrange", choice("one"))});
                impossible_object = true;
            }
        }
        if (impossible_object) {
            continue;
        }

        const LayerReport* report = compiled.layer(layer.uid);
        if (report != nullptr && !report->error.empty()) {
            notes.push_back({layer.uid, report->error_block, AdviceLevel::Error,
                             "This layer cannot play: " + report->error + ".", {}, {}});
            continue;
        }

        // Hitting solid objects meets every rigid layer of the page,
        // wherever it sits, so there has to be one; each has to be small
        // enough to meet every particle, and what they come to together is
        // a cost worth saying out loud.
        if (const Block* lands = first(layer, "pieces")) {
            std::size_t solid_layers = 0U;
            std::uint64_t met = 0U;
            std::string crowded;
            for (const Layer& other : document.layers) {
                if (!other.enabled || other.kind != LayerKind::Rigid) {
                    continue;
                }
                ++solid_layers;
                if (const LayerReport* theirs = compiled.layer(other.uid); theirs != nullptr) {
                    met += theirs->capacity;
                    if (theirs->capacity > ajfx::max_body_colliders && crowded.empty()) {
                        crowded = other.name;
                    }
                }
            }
            if (solid_layers == 0U) {
                notes.push_back({layer.uid, lands->uid, AdviceLevel::Warning,
                                 "Nothing in this effect is solid, so these particles meet nothing and fall "
                                 "through. Add a rigid physics layer with something in it.",
                                 {},
                                 {}});
            } else if (!crowded.empty()) {
                notes.push_back({layer.uid, lands->uid, AdviceLevel::Error,
                                 "The " + crowded + " layer holds up to " +
                                     std::to_string(compiled.layer(layer_named_uid(document, crowded))->capacity) +
                                     " pieces, and meeting them means meeting every one: at most " +
                                     std::to_string(ajfx::max_body_colliders) +
                                     " can be met. Make that layer's pieces fewer.",
                                 {},
                                 {}});
            } else if (met > ajfx::max_body_colliders) {
                // Not refused: each layer is within what a particle may be
                // tested against, and the data grows to fit. It is the cost
                // that is worth knowing, since it is every particle against
                // every body, every step.
                notes.push_back({layer.uid, lands->uid, AdviceLevel::Tip,
                                 "These particles are tested against up to " + std::to_string(met) +
                                     " solid pieces every step, across " + std::to_string(solid_layers) +
                                     " layers. That is every particle against every piece, so it is worth "
                                     "keeping an eye on the GPU time.",
                                 {},
                                 {}});
            }
        }

        // Breaking belongs to the Object above it, so each Object's own
        // Break and Parts are gathered and spoken about together. A breaking
        // block with no Object over it is the first thing to say.
        const Block* breaking = nullptr;
        const Block* part = nullptr;
        for (std::size_t i = 0U; i < layer.blocks.size(); ++i) {
            const Block& block = layer.blocks[i];
            if (!block.enabled || !owned_by_object(block.kind)) {
                continue;
            }
            if (!object_of(layer, i)) {
                notes.push_back({layer.uid, block.uid, AdviceLevel::Error,
                                 "Breaking belongs to an object: this block has none above it, so nothing comes "
                                 "apart. Add an Object above it, and it is that object which breaks.",
                                 "Add an object", add_block(layer.uid, "object")});
                break;
            }
        }
        for (std::size_t i = 0U; i < layer.blocks.size(); ++i) {
            if (layer.blocks[i].kind != "object" || !layer.blocks[i].enabled) {
                continue;
            }
            const Block& object = layer.blocks[i];
            // What this Object owns: the breaking blocks after it, up to the
            // next Object.
            const Block* its_break = nullptr;
            std::size_t parts = 0U;
            const Block* its_part = nullptr;
            for (std::size_t j = i + 1U; j < layer.blocks.size() && layer.blocks[j].kind != "object"; ++j) {
                const Block& block = layer.blocks[j];
                if (!block.enabled) {
                    continue;
                }
                if (block.kind == "break" && its_break == nullptr) {
                    its_break = &block;
                } else if (block.kind == "part") {
                    ++parts;
                    if (its_part == nullptr) {
                        its_part = &block;
                    }
                }
            }
            if (its_break != nullptr && breaking == nullptr) {
                breaking = its_break;
            }
            if (its_part != nullptr && part == nullptr) {
                part = its_part;
            }
            const bool cutting = its_break != nullptr && choice_of(*its_break, "into") == "cut";
            const std::string motion = choice_of(object, "motion");
            const std::string collides = choice_of(object, "collides");
            const bool hollow = collides == "bucket" || collides == "crate" || collides == "tube";
            // Only a piece the world moves can come apart: what stands
            // still, what the effect drives and what is hollow is a prop.
            if (its_break != nullptr && (motion != "loose" || hollow)) {
                notes.push_back({layer.uid, its_break->uid, AdviceLevel::Error,
                                 "Only an object the world moves can come apart. Let it be knocked about, and "
                                 "have it collide as its own shape, a ball or a block.",
                                 "Knocked about", set_value(object.uid, "motion", choice("loose"))});
                continue;
            }
            // A piece that comes apart needs room for what it comes apart
            // into, or the break is refused when it happens and nothing
            // says why. The compiler sizes for it, so this is the bound the
            // layer could not grow to.
            if (its_break != nullptr) {
                const std::size_t into =
                    cutting ? static_cast<std::size_t>(number_of(*its_break, "cells")) : parts;
                const LayerReport* room = compiled.layer(layer.uid);
                const std::size_t count = choice_of(object, "arrange") == "one"
                                              ? 1U
                                              : static_cast<std::size_t>(number_of(object, "count"));
                if (into > 0U && room != nullptr && count * into > room->capacity) {
                    notes.push_back({layer.uid, its_break->uid, AdviceLevel::Warning,
                                     "If every one of these came apart there would be " +
                                         std::to_string(count * into) + " pieces, and this layer holds " +
                                         std::to_string(room->capacity) +
                                         ". The ones that do not fit stay whole. Break into fewer, or start "
                                         "with fewer.",
                                     {},
                                     {}});
                }
            }
            if (cutting && its_part != nullptr) {
                notes.push_back({layer.uid, its_part->uid, AdviceLevel::Warning,
                                 "This object is set to cut its model into pieces, so the parts placed under it "
                                 "are not used. Set it back to parts you place, or remove them.",
                                 "Use the parts", set_value(its_break->uid, "into", choice("parts"))});
            }
            if (its_break != nullptr && its_part == nullptr && !cutting) {
                notes.push_back({layer.uid, its_break->uid, AdviceLevel::Warning,
                                 "Nothing is set to break off yet, so this object stays whole. Add a Part for "
                                 "each panel, plank or shard it comes apart into.",
                                 "Add a part", add_block(layer.uid, "part")});
            }
            if (its_part != nullptr && its_break == nullptr) {
                notes.push_back({layer.uid, its_part->uid, AdviceLevel::Warning,
                                 "Nothing breaks, so this part is never made. Add a Break to say how hard this "
                                 "object has to be hit to come apart.",
                                 "Add break", add_block(layer.uid, "break")});
            }
            // A part is drawn as its own model only where the layer draws
            // models at all.
            if (its_part != nullptr) {
                const Block* appearance = first(layer, "appearance");
                if (appearance != nullptr && choice_of(*appearance, "material") != "model") {
                    notes.push_back({layer.uid, its_part->uid, AdviceLevel::Tip,
                                     "The parts hit things as their own shapes, but the layer draws its pieces "
                                     "as sprites, so they are not drawn as their models. Set Appearance to "
                                     "debris to see them.",
                                     "Draw debris", set_value(appearance->uid, "material", choice("model"))});
                }
            }
        }

        const bool objects = first(layer, "object") != nullptr;
        const bool born = first(layer, "stream") != nullptr || first(layer, "burst") != nullptr || objects ||
                          has_raw(layer, "spawn_rate") || has_raw(layer, "spawn_burst") ||
                          has_raw(layer, "spawn_pile");
        // A layer holding one object that only stands there is a stage
        // piece, not an empty layer: it has something to show without
        // anything being born in it.
        const bool propped = objects;
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

        // An object's pieces lie on long after the loop, so their end is
        // not seen.
        if (born && first(layer, "fade") == nullptr && !has_raw(layer, "color_over_life") && !objects &&
            !has_raw(layer, "spawn_pile")) {
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
        if (tumble != nullptr && material != "model") {
            notes.push_back({layer.uid, tumble->uid, AdviceLevel::Tip,
                             "Tumbling only shows on solid chunks: set Appearance to Debris.", "Use debris",
                             appearance != nullptr ? set_value(appearance->uid, "material", choice("model"))
                                                   : add_block(layer.uid, "appearance", false,
                                                               {{"material", choice("model")}})});
        }
        if (appearance != nullptr && material == "model" && tumble == nullptr) {
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
            if (block.kind == "object" && choice_of(block, "model") == "file" &&
                file_of(block, "model_file").empty()) {
                notes.push_back({layer.uid, block.uid, AdviceLevel::Warning,
                                 "Pick a model for this object. Until then it is drawn as the layer's own "
                                 "pieces are.",
                                 "Draw as a cube", set_value(block.uid, "model", choice("cube"))});
            }
            if (block.kind == "appearance") {
                const bool debris = choice_of(block, "material") == "model";
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
