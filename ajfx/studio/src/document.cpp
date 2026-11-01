#include "document.hpp"

#include "catalog.hpp"

#include <algorithm>
#include <cctype>

namespace ajfx::studio {

const Value* Block::find(std::string_view control) const {
    for (const auto& [name, value] : values) {
        if (name == control) {
            return &value;
        }
    }
    return nullptr;
}

void Block::set(std::string_view control, Value value) {
    // Kept sorted by name, so two blocks with the same values compare equal
    // however they were set.
    const auto at = std::lower_bound(values.begin(), values.end(), control,
                                     [](const auto& entry, std::string_view name) { return entry.first < name; });
    if (at != values.end() && at->first == control) {
        at->second = std::move(value);
        return;
    }
    values.emplace(at, std::string{control}, std::move(value));
}

void Block::clear(std::string_view control) {
    std::erase_if(values, [&](const auto& entry) { return entry.first == control; });
}

const Published* Block::publication(std::string_view control) const {
    for (const Published& entry : published) {
        if (entry.control == control) {
            return &entry;
        }
    }
    return nullptr;
}

void Block::publish(Published publication) {
    const auto at = std::lower_bound(published.begin(), published.end(), publication.control,
                                     [](const Published& entry, const std::string& name) { return entry.control < name; });
    if (at != published.end() && at->control == publication.control) {
        *at = std::move(publication);
        return;
    }
    published.insert(at, std::move(publication));
}

void Block::unpublish(std::string_view control) {
    std::erase_if(published, [&](const Published& entry) { return entry.control == control; });
}

std::string parameter_name(std::string_view text) {
    std::string name;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0) {
            name += static_cast<char>(std::tolower(u));
        } else if (!name.empty() && name.back() != '_') {
            name += '_';
        }
        if (name.size() >= 48U) {
            break;
        }
    }
    while (!name.empty() && name.back() == '_') {
        name.pop_back();
    }
    return name;
}

std::string typed_parameter_name(std::string_view text) {
    std::string name;
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (std::isalnum(u) != 0) {
            name += static_cast<char>(std::tolower(u));
        } else if (c == '_' || c == ' ' || c == '-') {
            name += '_';
        }
        if (name.size() >= 48U) {
            break;
        }
    }
    return name;
}

bool valid_parameter_name(std::string_view name) noexcept {
    if (name.empty() || name.size() > 48U) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

std::string_view layer_kind_name(LayerKind kind) noexcept {
    switch (kind) {
    case LayerKind::Particles: return "Particles";
    case LayerKind::Rigid: return "Rigid physics";
    }
    return "Particles";
}

std::string_view layer_kind_description(LayerKind kind) noexcept {
    switch (kind) {
    case LayerKind::Particles:
        return "Points moved by forces: smoke, sparks, flames, rain. They pass through each other.";
    case LayerKind::Rigid:
        return "Solid pieces moved by a physics engine: they knock into each other, pile up and come to rest.";
    }
    return {};
}

std::string_view layer_kind_id(LayerKind kind) noexcept {
    switch (kind) {
    case LayerKind::Particles: return "particles";
    case LayerKind::Rigid: return "rigid";
    }
    return "particles";
}

LayerKind layer_kind_from(std::string_view id) noexcept {
    return id == "rigid" ? LayerKind::Rigid : LayerKind::Particles;
}

namespace {
constexpr std::string_view empty_layer_prefix = "empty:";
} // namespace

std::string empty_layer_id(LayerKind kind) {
    return std::string{empty_layer_prefix} + std::string{layer_kind_id(kind)};
}

std::optional<LayerKind> empty_layer_kind(std::string_view id) noexcept {
    // A bare "" is the empty layer as pages and pickers named it before
    // layers had kinds; it is the particle layer.
    if (id.empty()) {
        return LayerKind::Particles;
    }
    if (!id.starts_with(empty_layer_prefix)) {
        return std::nullopt;
    }
    const std::string_view rest = id.substr(empty_layer_prefix.size());
    for (const LayerKind kind : {LayerKind::Particles, LayerKind::Rigid}) {
        if (layer_kind_id(kind) == rest) {
            return kind;
        }
    }
    return std::nullopt;
}

Layer* Document::layer(Uid uid) {
    for (Layer& candidate : layers) {
        if (candidate.uid == uid) {
            return &candidate;
        }
    }
    return nullptr;
}

const Layer* Document::layer(Uid uid) const {
    return const_cast<Document*>(this)->layer(uid);
}

std::optional<Document::BlockPlace> Document::find_block(Uid uid) {
    for (Layer& candidate : layers) {
        for (std::size_t i = 0U; i < candidate.blocks.size(); ++i) {
            if (candidate.blocks[i].uid == uid) {
                return BlockPlace{&candidate, i};
            }
        }
    }
    return std::nullopt;
}

Block* Document::block(Uid uid) {
    if (const std::optional<BlockPlace> place = find_block(uid)) {
        return &place->layer->blocks[place->index];
    }
    if (Layer* owner = world_of(uid)) {
        return &*owner->world;
    }
    return nullptr;
}

const Block* Document::block(Uid uid) const {
    return const_cast<Document*>(this)->block(uid);
}

Layer* Document::world_of(Uid block) {
    for (Layer& candidate : layers) {
        if (candidate.world && candidate.world->uid == block) {
            return &candidate;
        }
    }
    return nullptr;
}

std::optional<Block> make_block(Document& document, std::string_view kind) {
    if (find_block_def(kind) == nullptr || kind == raw_block_kind) {
        return std::nullopt;
    }
    Block block;
    block.uid = document.allocate();
    block.kind = std::string{kind};
    return block;
}

Layer make_layer(Document& document, LayerKind kind, std::string name) {
    Layer layer;
    layer.uid = document.allocate();
    layer.name = std::move(name);
    layer.kind = kind;
    if (kind == LayerKind::Rigid) {
        Block world;
        world.uid = document.allocate();
        world.kind = std::string{world_block_kind};
        layer.world = std::move(world);
        // Pieces in a physics world lie where they fall: they are watched
        // long after a particle's life would have ended.
        layer.life_min = layer.life_max = 60.0F;
    }
    return layer;
}

std::vector<std::string> blocks_against_kind(const Layer& layer, LayerKind kind) {
    std::vector<std::string> titles;
    for (const Block& block : layer.blocks) {
        if (block.kind == raw_block_kind || block_fits(block.kind, kind)) {
            continue;
        }
        if (const BlockDef* def = find_block_def(block.kind)) {
            titles.emplace_back(def->title);
        }
    }
    return titles;
}

bool set_layer_kind(Document& document, Layer& layer, LayerKind kind) {
    if (layer.kind == kind) {
        return true;
    }
    if (!blocks_against_kind(layer, kind).empty()) {
        return false;
    }
    layer.kind = kind;
    if (kind == LayerKind::Rigid) {
        if (!layer.world) {
            Block world;
            world.uid = document.allocate();
            world.kind = std::string{world_block_kind};
            layer.world = std::move(world);
        }
    } else {
        // The world is dropped with the kind: a particle layer has none,
        // and keeping a hidden one would be a setting nothing can reach.
        layer.world.reset();
    }
    for (Block& block : layer.blocks) {
        conform_block(block, kind);
    }
    return true;
}

Block make_raw_block(Document& document, RawModule module) {
    Block block;
    block.uid = document.allocate();
    block.kind = std::string{raw_block_kind};
    block.raw = std::move(module);
    return block;
}

Uid insert_block(Layer& layer, std::size_t index, Block block) {
    const Uid uid = block.uid;
    // Whatever it carried, it holds only what this layer's kind can act on.
    conform_block(block, layer.kind);
    index = std::min(index, layer.blocks.size());
    layer.blocks.insert(layer.blocks.begin() + static_cast<std::ptrdiff_t>(index), std::move(block));
    return uid;
}

bool move_block(Document& document, Uid uid, Uid to_layer, std::size_t index) {
    const std::optional<Document::BlockPlace> place = document.find_block(uid);
    Layer* const target = document.layer(to_layer);
    if (!place || target == nullptr) {
        return false;
    }
    // A block the target's kind does not have stays where it is: the drop
    // is refused rather than landing something the layer cannot play.
    if (place->layer->blocks[place->index].kind != raw_block_kind &&
        !block_fits(place->layer->blocks[place->index].kind, target->kind)) {
        return false;
    }
    Block block = std::move(place->layer->blocks[place->index]);
    place->layer->blocks.erase(place->layer->blocks.begin() + static_cast<std::ptrdiff_t>(place->index));
    insert_block(*target, index, std::move(block));
    return true;
}

bool remove_block(Document& document, Uid uid) {
    const std::optional<Document::BlockPlace> place = document.find_block(uid);
    if (!place) {
        return false;
    }
    place->layer->blocks.erase(place->layer->blocks.begin() + static_cast<std::ptrdiff_t>(place->index));
    return true;
}

std::optional<Uid> duplicate_block(Document& document, Uid uid) {
    const std::optional<Document::BlockPlace> place = document.find_block(uid);
    if (!place) {
        return std::nullopt;
    }
    Block copy = place->layer->blocks[place->index];
    copy.uid = document.allocate();
    // A copy is a module of its own, not part of what was detached.
    copy.detached.reset();
    return insert_block(*place->layer, place->index + 1U, std::move(copy));
}

Layer copy_layer(Document& document, const Layer& layer) {
    Layer copy = layer;
    copy.uid = document.allocate();
    copy.solo = false;
    if (copy.world) {
        copy.world->uid = document.allocate();
    }
    for (Block& block : copy.blocks) {
        block.uid = document.allocate();
    }
    return copy;
}

bool remove_layer(Document& document, Uid uid) {
    const auto before = document.layers.size();
    std::erase_if(document.layers, [&](const Layer& layer) { return layer.uid == uid; });
    return document.layers.size() != before;
}

bool move_layer(Document& document, Uid uid, std::size_t index) {
    const auto found = std::find_if(document.layers.begin(), document.layers.end(),
                                    [&](const Layer& layer) { return layer.uid == uid; });
    if (found == document.layers.end()) {
        return false;
    }
    Layer layer = std::move(*found);
    document.layers.erase(found);
    index = std::min(index, document.layers.size());
    document.layers.insert(document.layers.begin() + static_cast<std::ptrdiff_t>(index), std::move(layer));
    return true;
}

} // namespace ajfx::studio
