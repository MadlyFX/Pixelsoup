#include "compile.hpp"
#include "catalog.hpp"
#include "document_io.hpp"
#include "raw_modules.hpp"

#include <algorithm>
#include <variant>

namespace ajfx::studio {

std::vector<Block> detach_block(Document& document, const Compiled& compiled, Uid uid) {
    std::vector<Block> blocks;
    const std::optional<Document::BlockPlace> place = document.find_block(uid);
    if (!place) {
        return blocks;
    }
    const Block& block = place->layer->blocks[place->index];
    const BlockDef* def = find_block_def(block.kind);
    const LayerReport* report = compiled.layer(place->layer->uid);
    if (def == nullptr || !def->detachable || report == nullptr || !report->error.empty()) {
        return blocks;
    }
    const bool shapes_life = block.kind == BlockKind::Color || block.kind == BlockKind::Size || block.kind == BlockKind::Fade;
    const Layer& layer = *place->layer;
    const std::string original = block_text(block);
    for (const ModuleLine& line : report->modules) {
        bool take = line.origin == uid;
        // The merged over-life curve this block shapes travels with it, so
        // the picture is unchanged the moment it is detached.
        if (!take && shapes_life && line.origin == layer.uid) {
            const auto* update = std::get_if<UpdateModule>(&line.module);
            take = update != nullptr &&
                   ((block.kind == BlockKind::Size && std::holds_alternative<SizeOverLife>(*update)) ||
                    (block.kind != BlockKind::Size && std::holds_alternative<ColorOverLife>(*update)));
        }
        if (take) {
            RawModule module = line.module;
            set_raw_id(module, 0U);
            // What the block published goes with it, so its inputs keep the
            // values they had rather than naming parameters nobody declares.
            visit_fields(module, [&](std::string_view, auto& field) {
                using T = std::decay_t<decltype(field)>;
                if constexpr (std::is_same_v<T, FloatInput> || std::is_same_v<T, Vec3Input> ||
                              std::is_same_v<T, ColorInput>) {
                    if (!field.bound()) {
                        return;
                    }
                    ParameterValue value{};
                    if (compiled.effect) {
                        for (const ParameterDesc& parameter : compiled.effect->parameters) {
                            if (parameter.name == field.parameter) {
                                value = parameter.default_value;
                            }
                        }
                    }
                    if constexpr (std::is_same_v<T, FloatInput>) {
                        field.value = value[0];
                    } else if constexpr (std::is_same_v<T, Vec3Input>) {
                        field.value = Vec3{value[0], value[1], value[2]};
                    } else {
                        field.value = Color{value[0], value[1], value[2], value[3]};
                    }
                    field.parameter.clear();
                }
            });
            Block raw = make_raw_block(document, module);
            raw.detached = Detached{uid, original, std::move(module), 1U};
            blocks.push_back(std::move(raw));
        }
    }
    for (Block& raw : blocks) {
        raw.detached->count = static_cast<std::uint32_t>(blocks.size());
    }
    return blocks;
}

namespace {

// The block a detach came from, as it was.
std::optional<Block> original_of(const Detached& detached) {
    std::string error;
    return block_from_text(detached.original, error);
}

} // namespace

std::optional<DetachedGroup> detached_group(const Document& document, Uid uid) {
    const Block* block = document.block(uid);
    if (block == nullptr || !block->detached) {
        return std::nullopt;
    }
    const std::optional<Block> original = original_of(*block->detached);
    if (!original) {
        return std::nullopt;
    }
    DetachedGroup group;
    group.from = block->detached->from;
    group.title = block_title(*original);
    std::size_t expected = 0U;
    for (const Layer& layer : document.layers) {
        for (const Block& other : layer.blocks) {
            if (!other.detached || other.detached->from != group.from) {
                continue;
            }
            const bool in_layer = std::any_of(layer.blocks.begin(), layer.blocks.end(),
                                              [&](const Block& b) { return b.uid == uid; });
            if (!in_layer) {
                continue;
            }
            ++group.modules;
            RawModule now = other.raw ? *other.raw : other.detached->module;
            set_raw_id(now, 0U);
            if (!other.raw || !(now == other.detached->module) || !other.enabled) {
                group.changed = true;
            }
        }
    }
    expected = block->detached->count;
    if (expected > group.modules) {
        group.changed = true;
    }
    return group;
}

std::optional<Uid> recombine_block(Document& document, Uid uid) {
    const std::optional<Document::BlockPlace> place = document.find_block(uid);
    if (!place) {
        return std::nullopt;
    }
    Layer& layer = *place->layer;
    const Block& block = layer.blocks[place->index];
    if (!block.detached) {
        return std::nullopt;
    }
    std::optional<Block> original = original_of(*block.detached);
    if (!original) {
        return std::nullopt;
    }
    const Uid from = block.detached->from;
    std::size_t at = layer.blocks.size();
    for (std::size_t i = 0U; i < layer.blocks.size(); ++i) {
        if (layer.blocks[i].detached && layer.blocks[i].detached->from == from) {
            at = std::min(at, i);
        }
    }
    std::erase_if(layer.blocks, [&](const Block& b) { return b.detached && b.detached->from == from; });
    // Its own uid back, so its random streams are the ones it had - unless
    // something (a copied layer) has taken it since.
    if (document.block(original->uid) != nullptr || document.layer(original->uid) != nullptr || original->uid == 0U) {
        original->uid = document.allocate();
    }
    const Uid restored = original->uid;
    insert_block(layer, std::min(at, layer.blocks.size()), std::move(*original));
    return restored;
}

} // namespace ajfx::studio
