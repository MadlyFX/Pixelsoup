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

const Block* Document::block(Uid uid) const {
    const std::optional<BlockPlace> place = const_cast<Document*>(this)->find_block(uid);
    return place ? &place->layer->blocks[place->index] : nullptr;
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

Block make_raw_block(Document& document, RawModule module) {
    Block block;
    block.uid = document.allocate();
    block.kind = std::string{raw_block_kind};
    block.raw = std::move(module);
    return block;
}

Uid insert_block(Layer& layer, std::size_t index, Block block) {
    const Uid uid = block.uid;
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
