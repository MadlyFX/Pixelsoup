#include "coach.hpp"

#include "bake.hpp"
#include "catalog.hpp"
#include "raw_modules.hpp"

#include <algorithm>
#include <cmath>

namespace ajfx::studio {
namespace {

std::size_t count_of(const Layer& layer, std::string_view kind) {
    return static_cast<std::size_t>(std::count_if(layer.blocks.begin(), layer.blocks.end(), [kind](const Block& b) {
        return b.enabled && b.kind == kind;
    }));
}

const Block* first(const Layer& layer, BlockKind kind) {
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
std::function<void(Document&)> add_block(Uid layer, BlockKind kind, bool front = false,
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

// Blocks whose particles are born of the page's solid things.
bool born_of_solids(std::string_view kind) {
    return kind == BlockKind::HitPuffs || kind == BlockKind::HitSparks || kind == BlockKind::Trail;
}

} // namespace

bool has_births(const Layer& layer) {
    return first(layer, BlockKind::Stream) != nullptr || first(layer, BlockKind::Burst) != nullptr || first(layer, BlockKind::Object) != nullptr ||
           std::any_of(layer.blocks.begin(), layer.blocks.end(),
                       [](const Block& b) { return b.enabled && born_of_solids(b.kind); }) ||
           has_raw(layer, "spawn_rate") || has_raw(layer, "spawn_burst") || has_raw(layer, "spawn_pile") ||
           has_raw(layer, "spawn_from_contacts") || has_raw(layer, "spawn_from_bodies");
}

std::vector<Suggestion> starters(const Layer& layer) {
    if (has_births(layer)) {
        return {};
    }
    if (layer.kind != LayerKind::Rigid) {
        // Named for the block each adds; the words under are a few, since the
        // operator learns what a block does by using it.
        return {
            Suggestion{"Stream", Category::Birth, {SuggestedBlock{BlockKind::Stream, {}}}},
            Suggestion{"Burst", Category::Birth, {SuggestedBlock{BlockKind::Burst, {}}}},
        };
    }
    // What falls needs gravity to fall and a ground to land on, and the
    // page gives every layer both (Document::world), so a starter is the one
    // block it is named for.
    return {
        Suggestion{"Pour", Category::Birth, {SuggestedBlock{BlockKind::Stream, {}}}},
        Suggestion{"Burst", Category::Birth, {SuggestedBlock{BlockKind::Burst, {}}}},
        // Standing on the ground, which is half its 0.4 m up.
        Suggestion{"Object", Category::Physics,
                   {SuggestedBlock{BlockKind::Object, {{"motion", choice("fixed")}, {"at", vec3(0.0F, 0.2F, 0.0F)}}}}},
        Suggestion{"Heap", Category::Physics,
                   {SuggestedBlock{BlockKind::Object, {{"model", choice("rock")}, {"arrange", choice("heap")}}}}},
    };
}

Uid add_suggestion(Document& document, Uid layer_uid, const Suggestion& suggestion) {
    Layer* layer = document.layer(layer_uid);
    if (layer == nullptr) {
        return 0U;
    }
    Uid first_added = 0U;
    for (const SuggestedBlock& made : suggestion.blocks) {
        std::optional<Block> block = make_block(document, made.kind);
        if (!block) {
            continue;
        }
        for (const auto& [control, value] : made.values) {
            block->set(control, value);
        }
        const Uid uid = insert_block(*layer, layer->blocks.size(), std::move(*block));
        if (first_added == 0U) {
            first_added = uid;
        }
    }
    return first_added;
}

std::vector<Suggestion> next_steps(const Document& document, const Layer& layer) {
    std::vector<Suggestion> steps;
    // A layer that is off is not advised on, and one with nothing born asks
    // what to start with instead (`starters`).
    if (!layer.enabled || !has_births(layer)) {
        return steps;
    }
    const bool rigid = layer.kind == LayerKind::Rigid;
    const Block* appearance = first(layer, BlockKind::Appearance);
    const std::string material = appearance != nullptr ? choice_of(*appearance, "material") : "";
    const Block* launch = first(layer, BlockKind::Launch);
    const Block* through = first(layer, BlockKind::PassesThrough);
    // The page's gravity pulls the layer by its share, and the page's ground
    // holds it unless it passes through the ground too.
    const bool pulled_by_page = world_share(layer).gravity > 0.0F && document.world.gravity > 0.0F;
    const bool has_gravity =
        first(layer, BlockKind::Gravity) != nullptr || has_raw(layer, "gravity") || pulled_by_page;
    const bool falls = has_gravity || (launch != nullptr && choice_of(*launch, "direction") == "down");
    const bool grounded = first(layer, BlockKind::Ground) != nullptr || has_raw(layer, "plane_collision") ||
                          (document.world.ground && !(through != nullptr && toggle_of(*through, "ground")));
    const SuggestedBlock gravity{BlockKind::Gravity, {}};
    const SuggestedBlock ground{BlockKind::Ground, {}};

    // Solid things: what the world moves, and what only stands there. A layer
    // whose objects all stand or are driven has nothing that falls, so what
    // lands on them is the next thing to add; and pieces that are moved
    // without gravity hang where they are born, which nothing else says.
    bool dropped = false;
    if (rigid) {
        const bool born_pieces = first(layer, BlockKind::Stream) != nullptr || first(layer, BlockKind::Burst) != nullptr ||
                                 has_raw(layer, "spawn_rate") || has_raw(layer, "spawn_burst");
        const bool knocked_about = std::any_of(layer.blocks.begin(), layer.blocks.end(), [](const Block& b) {
            return b.enabled && b.kind == BlockKind::Object && choice_of(b, "motion") == "loose";
        });
        if (!born_pieces && !knocked_about && !has_raw(layer, "spawn_pile")) {
            dropped = true;
            Suggestion drop{"Stream", Category::Birth, {SuggestedBlock{BlockKind::Stream, {}}}};
            // What the Stream needs to fall and land, where the page does not
            // give it.
            if (!has_gravity) {
                drop.blocks.push_back(gravity);
            }
            if (!grounded) {
                drop.blocks.push_back(ground);
            }
            steps.push_back(std::move(drop));
        } else if (!has_gravity) {
            steps.push_back({"Gravity", Category::Motion, {gravity}});
        }
    }

    // The ground the dropped Stream brings, if it is offered, is not offered
    // twice.
    if (!dropped && falls && !grounded && document.backdrop != Backdrop::Void) {
        steps.push_back({"Ground", Category::Contact, {ground}});
    }

    if (appearance == nullptr) {
        // A layer of solid things draws slabs until told otherwise, objects or
        // no objects; the pieces that are the layer's own (not an Object, not
        // a Stream with a model of its own) are the ones this is about.
        const bool own_pieces = std::any_of(layer.blocks.begin(), layer.blocks.end(), [](const Block& b) {
            return b.enabled && (b.kind == BlockKind::Stream || b.kind == BlockKind::Burst) && choice_of(b, "model") == "layer";
        });
        if (!rigid || own_pieces) {
            steps.push_back({"Look", Category::Look, {SuggestedBlock{BlockKind::Appearance, {}}}});
        }
    }
    if (appearance != nullptr && material == "model" && first(layer, BlockKind::Tumble) == nullptr) {
        steps.push_back({"Tumble", Category::Contact, {SuggestedBlock{BlockKind::Tumble, {}}}});
    }
    // An object's pieces lie on long after the loop, so their end is not seen.
    if (first(layer, BlockKind::Fade) == nullptr && !has_raw(layer, "color_over_life") && first(layer, BlockKind::Object) == nullptr &&
        !has_raw(layer, "spawn_pile")) {
        steps.push_back({"Fade", Category::Look, {SuggestedBlock{BlockKind::Fade, {}}}});
    }
    return steps;
}

std::vector<Advice> advise(const Document& document, const Compiled& compiled, const BakeStatus* bake) {
    std::vector<Advice> notes;
    // The page's explosions, in the page's own units: whether one is strong
    // enough to break a thing is a ratio, which scale leaves alone.
    const std::vector<PageBlast> blasts = page_blasts(document, 1.0F);
    for (const Layer& layer : document.layers) {
        if (!layer.enabled) {
            continue;
        }
        // An Object asked to be something it cannot stops the whole layer
        // compiling, and the reason reads better here, on the block at fault
        // and with the fix, than passed through in the runtime's own words.
        bool impossible_object = false;
        for (const Block& block : layer.blocks) {
            if (!block.enabled || block.kind != BlockKind::Object) {
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
                                 "A moved object cannot collide as its own shape.", "Use a crate",
                                 set_value(block.uid, "collides", choice("crate"))});
                impossible_object = true;
            }
            // Standing still, being driven and being hollow are a prop's,
            // and a prop is one authored thing rather than a heap of them.
            if (many && (motion != "loose" || hollow)) {
                notes.push_back({layer.uid, block.uid, AdviceLevel::Error,
                                 "Only a single object can stand still, move or be hollow.", "Just one",
                                 set_value(block.uid, "arrange", choice("one"))});
                impossible_object = true;
            }
        }
        if (impossible_object) {
            continue;
        }

        const LayerReport* report = compiled.layer(layer.uid);
        if (report != nullptr && !report->error.empty()) {
            notes.push_back({layer.uid, report->error_block, AdviceLevel::Error,
                             "Cannot play: " + report->error + ".", {}, {}});
            continue;
        }

        // The page's solid layers are one world. One too full for particles
        // to land on is left out of what they meet, rather than the layers
        // that would meet it refusing to play.
        if (layer.kind == LayerKind::Rigid && report != nullptr && report->included &&
            report->emitter_capacity > ajfx::max_body_colliders) {
            const bool landed_on = std::any_of(document.layers.begin(), document.layers.end(), [](const Layer& other) {
                return other.enabled && other.kind == LayerKind::Particles &&
                       first(other, BlockKind::PassesThrough) == nullptr && has_births(other);
            });
            if (landed_on) {
                notes.push_back({layer.uid, 0U, AdviceLevel::Warning,
                                 "Particles pass through: up to " + std::to_string(report->emitter_capacity) +
                                     " solid pieces, and " + std::to_string(ajfx::max_body_colliders) +
                                     " can be met.",
                                 {},
                                 {}});
            }
        }

        // A solid layer plays in the page's one world, which is drawn the
        // way its first solid layer is: say so where this one's look is not.
        if (layer.kind == LayerKind::Rigid && report != nullptr && report->included &&
            compiled.solids.layers.size() > 1U && compiled.solids.layers.front().layer != layer.uid) {
            const LayerReport* base = compiled.layer(compiled.solids.layers.front().layer);
            const Layer* base_layer = document.layer(compiled.solids.layers.front().layer);
            if (base != nullptr && base_layer != nullptr) {
                // Each layer's models and colours are its own; glowing or
                // solid, sprites or models, and lights are the world's.
                const SpriteRenderer& mine = report->renderer;
                const SpriteRenderer& theirs = base->renderer;
                if (mine.blend != theirs.blend || mine.geometry != theirs.geometry ||
                    mine.light_count != theirs.light_count) {
                    notes.push_back({layer.uid, 0U, AdviceLevel::Tip,
                                     "Drawn as " + base_layer->name + " is: one world.", {}, {}});
                }
            }
        }

        // Born of solid things, with nothing solid on the page to be born of,
        // or reading a solid layer that does not play.
        for (const Block& block : layer.blocks) {
            if (!block.enabled || !born_of_solids(block.kind)) {
                continue;
            }
            if (compiled.solids.emitter.empty()) {
                notes.push_back({layer.uid, block.uid, AdviceLevel::Warning, "No solid layer to come from.",
                                 "Add solid", [](Document& d) {
                                     Layer solid = make_layer(d, LayerWhat::Solid, "Solid");
                                     d.layers.push_back(std::move(solid));
                                 }});
                continue;
            }
            const auto named = static_cast<Uid>(std::max(number_of(block, "solid"), 0.0F));
            if (named != 0U && compiled.solids.member(named) == nullptr) {
                notes.push_back({layer.uid, block.uid, AdviceLevel::Warning,
                                 document.layer(named) == nullptr ? "Its solid layer is gone."
                                                                  : "Its solid layer does not play.",
                                 "All solids", set_value(block.uid, "solid", number(0.0F))});
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
                                 "Breaking needs an Object above it.", "Add object", add_block(layer.uid, BlockKind::Object)});
                break;
            }
        }
        for (std::size_t i = 0U; i < layer.blocks.size(); ++i) {
            if (layer.blocks[i].kind != BlockKind::Object || !layer.blocks[i].enabled) {
                continue;
            }
            const Block& object = layer.blocks[i];
            // What this Object owns: the breaking blocks after it, up to the
            // next Object.
            const Block* its_break = nullptr;
            std::size_t parts = 0U;
            const Block* its_part = nullptr;
            for (std::size_t j = i + 1U; j < layer.blocks.size() && layer.blocks[j].kind != BlockKind::Object; ++j) {
                const Block& block = layer.blocks[j];
                if (!block.enabled) {
                    continue;
                }
                if (block.kind == BlockKind::Break && its_break == nullptr) {
                    its_break = &block;
                } else if (block.kind == BlockKind::Part) {
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
                                 "Only a knocked-about object can break.", "Knocked about",
                                 set_value(object.uid, "motion", choice("loose"))});
                continue;
            }
            // Set to break when blasted, with nothing on the page to blast
            // it, or nothing strong enough where it stands.
            if (its_break != nullptr && number_of(*its_break, "blast") > 0.0F) {
                const float needed = number_of(*its_break, "blast");
                const Value at = get(object, "at");
                const ajfx::Vec3 where{at.x, at.y, at.z};
                float hardest = 0.0F;
                for (const PageBlast& blast : blasts) {
                    hardest = std::max(hardest, ajfx::blast_push(blast.blast, where).speed);
                }
                if (blasts.empty()) {
                    notes.push_back({layer.uid, its_break->uid, AdviceLevel::Tip, "Nothing on the page explodes.",
                                     "Add explosion",
                                     add_block(layer.uid, BlockKind::Explosion, false,
                                               {{"at", vec3(at.x, at.y, at.z)}})});
                } else if (hardest < needed) {
                    notes.push_back({layer.uid, its_break->uid, AdviceLevel::Tip,
                                     hardest > 0.0F ? "No explosion throws it hard enough to break."
                                                    : "No explosion reaches it.",
                                     hardest > 0.0F ? "Break easier" : std::string{},
                                     hardest > 0.0F ? set_value(its_break->uid, "blast",
                                                                number(std::floor(hardest * 9.0F) / 10.0F))
                                                    : std::function<void(Document&)>{}});
                }
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
                                     "Breaking all would make " + std::to_string(count * into) +
                                         " pieces; there is room for " + std::to_string(room->capacity) + ".",
                                     {},
                                     {}});
                }
            }
            if (cutting && its_part != nullptr) {
                notes.push_back({layer.uid, its_part->uid, AdviceLevel::Warning,
                                 "It cuts its model, so these parts are unused.", "Use the parts",
                                 set_value(its_break->uid, "into", choice("parts"))});
            }
            if (its_break != nullptr && its_part == nullptr && !cutting) {
                notes.push_back({layer.uid, its_break->uid, AdviceLevel::Warning,
                                 "Nothing to break into.", "Add part", add_block(layer.uid, BlockKind::Part)});
            }
            if (its_part != nullptr && its_break == nullptr) {
                notes.push_back({layer.uid, its_part->uid, AdviceLevel::Warning,
                                 "No Break, so this part is never used.", "Add break",
                                 add_block(layer.uid, BlockKind::Break)});
            }
            // A part is drawn as its own model only where the layer draws
            // models at all.
            if (its_part != nullptr) {
                const Block* appearance = first(layer, BlockKind::Appearance);
                if (appearance != nullptr && choice_of(*appearance, "material") != "model") {
                    notes.push_back({layer.uid, its_part->uid, AdviceLevel::Tip,
                                     "Parts draw as sprites. Set Appearance to Model.", "Draw as models",
                                     set_value(appearance->uid, "material", choice("model"))});
                }
            }
        }

        // A layer holding one object that only stands there is a stage
        // piece, not an empty layer: it has something to show without
        // anything being born in it. With nothing at all there is nothing to
        // advise on; the page asks what to start with (`starters`) instead.
        // The tips that only add a block (a look, a fade, a ground, a
        // tumble) are `next_steps`, offered as the way on rather than as
        // faults; what is left here is what is wrong or looks cheap.
        const bool born = has_births(layer);
        if (!born) {
            continue;
        }

        const Block* appearance = first(layer, BlockKind::Appearance);
        const std::string material = appearance != nullptr ? choice_of(*appearance, "material") : "";
        if (appearance != nullptr && material == "smoke" && number_of(*appearance, "soft") <= 0.0F) {
            notes.push_back({layer.uid, appearance->uid, AdviceLevel::Tip,
                             "Smoke cuts a hard line at the floor.", "Soften edges",
                             set_value(appearance->uid, "soft", number(0.3F))});
        }

        const Block* color_block = first(layer, BlockKind::Color);
        if (appearance != nullptr && (material == "glow" || material == "trail") && color_block != nullptr &&
            get(*color_block, "brightness").y <= 1.0F) {
            notes.push_back({layer.uid, color_block->uid, AdviceLevel::Tip,
                             "Glow needs brightness above 1 to bloom.", "Brighten",
                             set_value(color_block->uid, "brightness", range(4.0F, 4.0F))});
        }

        const Block* tumble = first(layer, BlockKind::Tumble);
        if (tumble != nullptr && material != "model") {
            notes.push_back({layer.uid, tumble->uid, AdviceLevel::Tip,
                             "Tumble only shows on models.", "Draw as models",
                             appearance != nullptr ? set_value(appearance->uid, "material", choice("model"))
                                                   : add_block(layer.uid, BlockKind::Appearance, false,
                                                               {{"material", choice("model")}})});
        }

        // An automatic loop always covers the effect; a loop set by hand may not.
        if (born && !document.loop_auto && layer.life_max > document.loop_seconds) {
            notes.push_back({layer.uid, 0U, AdviceLevel::Tip,
                             "The loop is shorter than particle life.", "Auto loop",
                             [](Document& d) { d.loop_auto = true; }});
        }

        // An artist's own file that is not picked yet.
        bool glowing_object = false;
        for (const Block& block : layer.blocks) {
            if (!block.enabled) {
                continue;
            }
            // Blocks that can be a model of their own: an Object always, a
            // Stream or Burst once it picks one (its model is "the layer's
            // own" until then, which is no model of its own to draw).
            const bool object = block.kind == BlockKind::Object;
            const bool piece = block.kind == BlockKind::Stream || block.kind == BlockKind::Burst;
            const bool has_model = object || (piece && choice_of(block, "model") != "layer");
            // A layer draws all its pieces one way, and a model is drawn only
            // where the layer draws models. So an object, or a Stream of
            // rocks, in a glowing layer is a glowing dot, and drawing it as
            // its model draws the layer's other pieces as models too: say so
            // once, where the choice is, rather than leave it to be found.
            if (has_model && !glowing_object && appearance != nullptr && material != "model") {
                glowing_object = true;
                notes.push_back({layer.uid, block.uid, AdviceLevel::Tip, "Glow draws this as a glowing dot.",
                                 "Draw as models", set_value(appearance->uid, "material", choice("model"))});
            }
            if (block.kind == BlockKind::MeshCollider && file_of(block, "file").empty()) {
                notes.push_back({layer.uid, block.uid, AdviceLevel::Warning, "Pick a model to collide with.", {},
                                 {}});
            }
            if ((object || piece) && choice_of(block, "model") == "file" && file_of(block, "model_file").empty()) {
                notes.push_back({layer.uid, block.uid, AdviceLevel::Warning,
                                 "Pick a model for this " +
                                     std::string{object ? "object" : block.kind == BlockKind::Burst ? "burst" : "stream"} + ".",
                                 "Use a cube", set_value(block.uid, "model", choice("cube"))});
            }
            if (block.kind == BlockKind::Appearance) {
                const bool solid = choice_of(block, "material") == "model";
                if (!solid && choice_of(block, "texture") == "file" && file_of(block, "texture_file").empty()) {
                    notes.push_back({layer.uid, block.uid, AdviceLevel::Tip, "Pick a picture.", {}, {}});
                }
                if (solid && choice_of(block, "mesh") == "file" && file_of(block, "mesh_file").empty()) {
                    notes.push_back({layer.uid, block.uid, AdviceLevel::Tip, "Pick a model.", {}, {}});
                }
            }
        }

        if (report != nullptr && report->capacity > 250000U) {
            notes.push_back({layer.uid, 0U, AdviceLevel::Warning,
                             "Up to " + thousands(report->capacity) + " particles: heavy for a live show.", {}, {}});
        }

        // Drawing settings, which the host applies as it draws.
        for (const Block& block : layer.blocks) {
            if (!block.enabled || block.kind != BlockKind::Appearance) {
                continue;
            }
            const Value tint = get(block, "tint");
            const bool drawn_differently = tint.x != 1.0F || tint.y != 1.0F || tint.z != 1.0F ||
                                           number_of(block, "brightness") != 1.0F ||
                                           number_of(block, "opacity") != 1.0F ||
                                           number_of(block, "size_scale") != 1.0F;
            if (drawn_differently) {
                notes.push_back({layer.uid, block.uid, AdviceLevel::Tip,
                                 "Not applied yet by the media server or this preview.", {}, {}});
            }
        }

        // A baked layer holds what the simulation reads where it is set.
        const auto make_live = [uid = layer.uid](Document& d) {
            if (Layer* target = d.layer(uid)) {
                target->baked = false;
            }
        };
        if (report != nullptr) {
            for (const LayerReport::Held& held : report->held) {
                const Block* block = document.block(held.block);
                const BlockDef* def = block != nullptr ? find_block_def(block->kind) : nullptr;
                const ControlDef* control = def != nullptr ? find_control(*def, held.control) : nullptr;
                const std::string label = control != nullptr ? std::string{control->label} : held.control;
                notes.push_back({layer.uid, held.block, AdviceLevel::Warning,
                                 label + " is held: the layer is baked.", "Keep live", make_live});
            }
        }
        if (bake != nullptr) {
            if (const LayerBake* state = bake->layer(layer.uid)) {
                const std::string cost =
                    bake->cost.bytes > 0U
                        ? " (about " + bytes_text(bake->cost.bytes) + ", " + bytes_text(bake->cost.bytes_per_second) +
                              " a second" +
                              (bake->cost.seconds >= 0.0
                                   ? ", " + format_number(static_cast<float>(bake->cost.seconds), 1) + " s to bake"
                                   : std::string{}) +
                              ")"
                        : std::string{};
                switch (state->state) {
                case BakeState::Unbaked:
                    if (bake->automatic && state->why.empty()) {
                        // Studio bakes it once the page settles: nothing to do.
                        break;
                    }
                    notes.push_back({layer.uid, 0U, AdviceLevel::Tip,
                                     state->why.empty()
                                         ? "Baked, no cache yet. Press Bake" + cost + "."
                                         : "Baked, but " + state->why + ".",
                                     "Keep live", make_live});
                    break;
                case BakeState::Stale:
                    if (bake->automatic) {
                        // Baked again once the page settles: said quietly.
                        notes.push_back({layer.uid, state->block, AdviceLevel::Tip,
                                         "Bakes again when you pause: " + state->why + ".", "Keep live", make_live});
                        break;
                    }
                    notes.push_back({layer.uid, state->block, AdviceLevel::Warning,
                                     "Cache is stale: " + state->why + ". Bake again" + cost + ".", "Keep live",
                                     make_live});
                    break;
                case BakeState::TooLarge:
                    notes.push_back({layer.uid, 0U, AdviceLevel::Warning, "Cannot bake: " + state->why + ".",
                                     "Keep live", make_live});
                    break;
                case BakeState::Live:
                case BakeState::Current:
                    break;
                }
            }
        }
    }
    return notes;
}

} // namespace ajfx::studio
