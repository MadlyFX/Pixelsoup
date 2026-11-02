#include "page_view.hpp"

#include "blast.hpp"
#include "page_view_support.hpp"
#include "raw_modules.hpp"
#include "recipes.hpp"

#include <tinyfiledialogs.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <type_traits>

namespace ajfx::studio {

using namespace page_ui;

void PageView::open_picker(PickerMode mode, Uid layer, std::size_t index, float x, float y, const Document& document) {
    picker_mode_ = mode;
    picker_layer_ = layer;
    picker_index_ = index;
    picker_ids_.clear();
    std::vector<ListEntry> entries;
    std::string title;
    if (mode == PickerMode::Blocks) {
        const Layer* target = document.layer(layer);
        const LayerKind kind = target != nullptr ? target->kind : LayerKind::Particles;
        title = "Add to " + (target != nullptr && !target->name.empty() ? target->name : std::string{"layer"});
        for (const BlockDef& def : block_catalog()) {
            // Only what this kind of layer can play: a block the runtime
            // would refuse is not on the list to be chosen in the first
            // place.
            if (!fits(def.layers, kind)) {
                continue;
            }
            bool present = false;
            if (target != nullptr && def.unique) {
                present = std::any_of(target->blocks.begin(), target->blocks.end(),
                                      [&](const Block& b) { return b.kind == def.kind; });
            }
            entries.push_back(ListEntry{std::string{category_name(def.category)}, std::string{def.title},
                                        present ? std::string{"Already in this layer"} : std::string{def.description},
                                        !present, def.category});
            picker_ids_.emplace_back(block_kind_id(def.kind));
        }
    } else if (mode == PickerMode::Modules) {
        title = "Add a module";
        for (const RawKindDef& def : raw_kinds()) {
            std::string stage{def.stage};
            stage[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(stage[0])));
            entries.push_back(ListEntry{stage, std::string{def.title}, std::string{def.kind}, true, Category::Expert});
            picker_ids_.emplace_back(def.kind);
        }
    } else {
        // What it is comes first: that decides what moves it and how the
        // page's world acts on it, and the kind follows. Then the ready-made
        // layers, each of which is something too.
        title = "What is it?";
        for (const LayerWhat what : layer_whats) {
            entries.push_back(ListEntry{"What is it?", std::string{layer_what_name(what)},
                                        std::string{layer_what_description(what)}, true, {}});
            picker_ids_.emplace_back(empty_layer_id(what));
        }
        for (const LayerPreset& preset : layer_presets()) {
            entries.push_back(ListEntry{"Ready-made", std::string{preset.title}, std::string{preset.description}, true,
                                        {}});
            picker_ids_.emplace_back(preset.id);
        }
    }
    picker_.open(x, y, std::move(title), std::move(entries));
}

void PageView::open_block_picker(const Document& document) {
    const Layer* layer = document.layer(target_layer(document));
    if (layer == nullptr) {
        open_picker(PickerMode::Layers, 0U, 0U, page_.x + 40.0F, page_.y + 80.0F, document);
        return;
    }
    float y = page_.y + 80.0F;
    for (const LayerArea& area : layer_areas_) {
        if (area.layer == layer->uid) {
            y = area.end_y;
        }
    }
    open_picker(PickerMode::Blocks, layer->uid, layer->blocks.size(), page_.x + 60.0F, y, document);
}

void PageView::resolve_drag(gui::UiContext& ctx) {
    if (drag_block_ == 0U) {
        return;
    }
    const bool down = ctx.pointer_down();
    if (!dragging_ && down && std::fabs(ctx.pointer_y() - drag_press_y_) > 4.0F) {
        dragging_ = true;
    }
    drop_.reset();
    if (dragging_) {
        const float py = ctx.pointer_y();
        // The layer under the pointer, else the nearest.
        const LayerArea* area = nullptr;
        float best = 1.0e9F;
        for (const LayerArea& candidate : layer_areas_) {
            const float distance = py < candidate.rect.y ? candidate.rect.y - py
                                   : py > candidate.rect.bottom() ? py - candidate.rect.bottom()
                                                                  : 0.0F;
            if (distance < best) {
                best = distance;
                area = &candidate;
            }
        }
        if (area != nullptr) {
            const std::optional<std::pair<std::size_t, float>> gap = drop_gap(*area, drag_scope_, py);
            drop_x_ = area->rect.x;
            drop_w_ = area->rect.width;
            drop_y_ = gap ? gap->second : area->end_y;
            drop_ = std::make_pair(area->layer, gap ? gap->first : area->count);
        }
        ctx.set_cursor(gui::Cursor::Move);
    }
    if (!down) {
        if (dragging_ && drop_) {
            const Uid block = drag_block_;
            Uid to_layer = drop_->first;
            std::size_t index = drop_->second;
            deferred_.push_back([block, to_layer, index](Document& document) {
                std::size_t target = index;
                if (const std::optional<Document::BlockPlace> place = document.find_block(block)) {
                    // Indices count the block's old place; it leaves first.
                    if (place->layer->uid == to_layer && place->index < target) {
                        --target;
                    }
                }
                move_block(document, block, to_layer, target);
            });
        }
        drag_block_ = 0U;
        dragging_ = false;
        drop_.reset();
    }
}

void PageView::resolve_incoming(gui::UiContext& ctx, const Document& document) {
    incoming_drop_.reset();
    if (incoming_ == nullptr) {
        return;
    }
    const float px = ctx.pointer_x();
    const float py = ctx.pointer_y();
    if (!page_.contains(px, py)) {
        return;
    }
    if (incoming_->kind == Incoming::Kind::Layer) {
        // Between layers: before the first whose middle is below the pointer.
        std::size_t index = document.layers.size();
        incoming_y_ = layers_end_y_;
        for (std::size_t i = 0U; i < layer_tops_.size(); ++i) {
            const float top = layer_tops_[i].second;
            const float bottom = i + 1U < layer_tops_.size() ? layer_tops_[i + 1U].second : layers_end_y_;
            if (py < (top + bottom) * 0.5F) {
                index = i;
                incoming_y_ = top - 4.0F;
                break;
            }
        }
        incoming_drop_ = std::make_pair(Uid{0U}, index);
    } else {
        const BlockDef* def = incoming_->kind == Incoming::Kind::Block ? find_block_def(incoming_->id) : nullptr;
        // The layer under the pointer, else the nearest, that can take it.
        const LayerArea* area = nullptr;
        float best = 1.0e9F;
        for (const LayerArea& candidate : layer_areas_) {
            const Layer* layer = document.layer(candidate.layer);
            if (layer == nullptr || folded_.contains(candidate.layer)) {
                continue;
            }
            if (def != nullptr && def->unique &&
                std::any_of(layer->blocks.begin(), layer->blocks.end(),
                            [&](const Block& b) { return b.kind == def->kind; })) {
                continue;
            }
            const float distance = py < candidate.rect.y ? candidate.rect.y - py
                                   : py > candidate.rect.bottom() ? py - candidate.rect.bottom()
                                                                  : 0.0F;
            if (distance < best) {
                best = distance;
                area = &candidate;
            }
        }
        if (area == nullptr) {
            return;
        }
        // A palette entry lands under the heading its kind belongs to, as a
        // dragged block does: a Module block under the hood, a Prop among
        // the props, a force in the world.
        const RigidScope scope = incoming_->kind == Incoming::Kind::Module ? RigidScope::Hood
                                                                          : rigid_scope(incoming_->id);
        const std::optional<std::pair<std::size_t, float>> gap = drop_gap(*area, scope, py);
        drop_x_ = area->rect.x;
        drop_w_ = area->rect.width;
        incoming_y_ = gap ? gap->second : area->end_y;
        incoming_drop_ = std::make_pair(area->layer, gap ? gap->first : area->count);
    }
    ctx.set_cursor(gui::Cursor::Move);
    if (!ctx.pointer_down()) {
        // Let go over the page: it lands there.
        const Incoming incoming = *incoming_;
        const Uid layer = incoming_drop_->first;
        const std::size_t index = incoming_drop_->second;
        deferred_.push_back([this, incoming, layer, index](Document& target) { insert(target, incoming, layer, index); });
        incoming_drop_.reset();
    }
}

void PageView::draw_overlays(gui::UiContext& ctx, const gui::Rect& window, Document& document, const PageInputs& inputs) {
    const gui::Theme& theme = ctx.theme();
    if (dragging_ && drop_) {
        ctx.rounded(gui::Rect{drop_x_, drop_y_ - 1.0F, drop_w_, 3.0F}, theme.accent, 1.5F);
        if (const Block* block = document.block(drag_block_)) {
            const std::string title = block_title(*block);
            const gui::Rect ghost{ctx.pointer_x() + 14.0F, ctx.pointer_y() - 14.0F, ctx.font_width(title) + 48.0F, 28.0F};
            ctx.shadow(ghost, theme.radius);
            ctx.rounded(ghost, theme.widget, theme.radius);
            if (const BlockDef* def = find_block_def(block->kind)) {
                draw_category_icon(ctx, def->category, ghost.x + 16.0F, ghost.center_y(), theme.text_muted);
            }
            ctx.label(gui::Rect{ghost.x + 32.0F, ghost.y, ghost.width - 32.0F, ghost.height}, title, theme.text);
        }
    }

    if (incoming_ != nullptr && incoming_drop_) {
        // Where a palette entry would land, and the entry under the pointer.
        if (incoming_->kind == Incoming::Kind::Layer) {
            ctx.rounded(gui::Rect{content_x_, incoming_y_ - 1.0F, content_w_, 3.0F}, theme.accent, 1.5F);
        } else {
            ctx.rounded(gui::Rect{drop_x_, incoming_y_ - 1.0F, drop_w_, 3.0F}, theme.accent, 1.5F);
        }
    }
    if (incoming_ != nullptr) {
        const gui::Rect ghost{ctx.pointer_x() + 14.0F, ctx.pointer_y() - 14.0F, ctx.font_width(incoming_->title) + 48.0F,
                              28.0F};
        ctx.shadow(ghost, theme.radius);
        ctx.rounded(ghost, incoming_drop_ ? theme.widget : gui::fade(theme.widget, 0.7F), theme.radius);
        if (incoming_->category) {
            draw_category_icon(ctx, *incoming_->category, ghost.x + 16.0F, ghost.center_y(), theme.text_muted);
        } else {
            gui::draw_icon(ctx.draw_list(), gui::Icon::Plus, ghost.x + 16.0F, ghost.center_y(), theme.text_muted, 0.8F);
        }
        ctx.label(gui::Rect{ghost.x + 32.0F, ghost.y, ghost.width - 32.0F, ghost.height}, incoming_->title,
                  incoming_drop_ ? theme.text : theme.text_muted);
    }

    if (std::optional<std::size_t> picked = picker_.draw(ctx, window)) {
        const std::string id = *picked < picker_ids_.size() ? picker_ids_[*picked] : std::string{};
        if (picker_mode_ == PickerMode::Blocks) {
            if (id == BlockKind::Module) {
                const gui::Rect at = picker_.bounds();
                open_picker(PickerMode::Modules, picker_layer_, picker_index_, at.x, at.y, document);
            } else if (std::optional<Block> block = make_block(document, id)) {
                const Uid uid = block->uid;
                if (Layer* layer = document.layer(picker_layer_)) {
                    insert_block(*layer, picker_index_, std::move(*block));
                    open_.insert(uid);
                }
            }
        } else if (picker_mode_ == PickerMode::Modules) {
            if (const RawKindDef* def = find_raw_kind(id)) {
                Block block = make_raw_block(document, def->make());
                const Uid uid = block.uid;
                if (Layer* layer = document.layer(picker_layer_)) {
                    insert_block(*layer, picker_index_, std::move(block));
                    open_.insert(uid);
                }
            }
        } else {
            // Through insert(), which is the one place that knows how to
            // build a layer of any kind: a second copy here once pushed a
            // layer with no uid when it did not recognise the id, and every
            // widget on it then shared the uid of every other such layer.
            insert(document, Incoming{Incoming::Kind::Layer, id, {}, {}}, 0U, document.layers.size());
        }
    }

    // The block's actions.
    if (menu_block_ != 0U) {
        const gui::UiId id = part_id(0U, "block.context");
        const Block* block = document.block(menu_block_);
        const BlockDef* def = block != nullptr ? find_block_def(block->kind) : nullptr;
        const bool detachable = def != nullptr && def->detachable && inputs.compiled != nullptr;
        std::vector<std::string_view> options{"Duplicate", "Move up", "Move down"};
        if (detachable) {
            options.push_back("Detach into modules");
        }
        // Modules a block was detached into go back together into it.
        std::string recombine;
        if (const std::optional<DetachedGroup> group = detached_group(document, menu_block_)) {
            recombine = "Recombine into " + group->title + (group->changed ? " (drops module edits)" : "");
            options.push_back(recombine);
        }
        // An object in a solid layer can be blown up.
        const std::optional<Document::BlockPlace> in_layer = document.find_block(menu_block_);
        const Uid explodes = in_layer && block != nullptr && block->kind == BlockKind::Object
                                 ? explodable_object(document, in_layer->layer->uid, menu_block_)
                                 : 0U;
        if (explodes != 0U) {
            options.push_back("Explode this");
        }
        options.push_back("Delete");
        if (const std::optional<std::size_t> chosen = ctx.context_menu(id, options)) {
            const std::string_view action = options[*chosen];
            const Uid uid = menu_block_;
            if (action == "Explode this" && in_layer) {
                explode_now(document, in_layer->layer->uid, uid);
            } else if (action == "Duplicate") {
                if (std::optional<Uid> copy = duplicate_block(document, uid)) {
                    if (open_.contains(uid)) {
                        open_.insert(*copy);
                    }
                }
            } else if (action == "Move up" || action == "Move down") {
                if (std::optional<Document::BlockPlace> place = document.find_block(uid)) {
                    const bool up = action == "Move up";
                    if ((up && place->index > 0U) || (!up && place->index + 1U < place->layer->blocks.size())) {
                        move_block(document, uid, place->layer->uid, up ? place->index - 1U : place->index + 1U);
                    }
                }
            } else if (action == "Detach into modules") {
                std::vector<Block> raw = detach_block(document, *inputs.compiled, uid);
                if (std::optional<Document::BlockPlace> place = document.find_block(uid); place && !raw.empty()) {
                    Layer& layer = *place->layer;
                    const std::size_t at = place->index;
                    remove_block(document, uid);
                    for (std::size_t i = 0U; i < raw.size(); ++i) {
                        open_.insert(raw[i].uid);
                        insert_block(layer, at + i, std::move(raw[i]));
                    }
                }
            } else if (!recombine.empty() && action == recombine) {
                if (const std::optional<Uid> restored = recombine_block(document, uid)) {
                    open_.insert(*restored);
                    selection_.block = *restored;
                    selection_.layer = 0U;
                }
            } else if (action == "Delete") {
                remove_block(document, uid);
            }
            menu_block_ = 0U;
        } else if (!ctx.menu_open(id)) {
            menu_block_ = 0U;
        }
    }

    // The layer's actions.
    if (menu_layer_ != 0U) {
        const gui::UiId id = part_id(0U, "layer.context");
        std::vector<std::string_view> options{"Rename", "Duplicate", "Move up", "Move down", "Fold all others"};
        // A solid layer with something in it can be blown up: its first
        // object.
        if (explodable_object(document, menu_layer_) != 0U) {
            options.push_back("Explode this");
        }
        options.push_back("Delete layer");
        if (const std::optional<std::size_t> chosen = ctx.context_menu(id, options)) {
            const Uid uid = menu_layer_;
            const std::string_view action = options[*chosen];
            const auto found = std::find_if(document.layers.begin(), document.layers.end(),
                                            [&](const Layer& l) { return l.uid == uid; });
            if (found != document.layers.end()) {
                const std::size_t at = static_cast<std::size_t>(found - document.layers.begin());
                if (action == "Rename") {
                    rename_.begin(ctx, uid, found->name);
                } else if (action == "Duplicate") {
                    Layer copy = copy_layer(document, *found);
                    copy.name += " copy";
                    document.layers.insert(document.layers.begin() + static_cast<std::ptrdiff_t>(at + 1U), std::move(copy));
                } else if (action == "Move up") {
                    if (at > 0U) {
                        move_layer(document, uid, at - 1U);
                    }
                } else if (action == "Move down") {
                    move_layer(document, uid, at + 1U);
                } else if (action == "Fold all others") {
                    for (const Layer& fold : document.layers) {
                        if (fold.uid != uid) {
                            folded_.insert(fold.uid);
                        }
                    }
                    folded_.erase(uid);
                } else if (action == "Explode this") {
                    explode_now(document, uid, 0U);
                } else if (action == "Delete layer") {
                    remove_layer(document, uid);
                }
            }
            menu_layer_ = 0U;
        } else if (!ctx.menu_open(id)) {
            menu_layer_ = 0U;
        }
    }

    // What the layer is, from its chip: everything a layer can be. One that
    // needs the other kind, with a block in the way, names the block and
    // changes nothing, as the kind's own menu does.
    if (menu_what_ != 0U) {
        const gui::UiId id = part_id(0U, "layer.what");
        Layer* layer = document.layer(menu_what_);
        std::array<std::string, layer_whats.size()> labels;
        std::array<bool, layer_whats.size()> open_to_it{};
        for (std::size_t i = 0U; i < layer_whats.size(); ++i) {
            const LayerWhat what = layer_whats[i];
            labels[i] = std::string{layer_what_name(what)};
            open_to_it[i] = true;
            if (layer != nullptr && layer->kind != layer_what_kind(what)) {
                const std::vector<std::string> blocking = blocks_against_kind(*layer, layer_what_kind(what));
                open_to_it[i] = blocking.empty();
                for (std::size_t b = 0U; b < blocking.size(); ++b) {
                    labels[i] += (b == 0U ? ": " : ", ") + blocking[b];
                }
            }
        }
        std::array<std::string_view, layer_whats.size()> options;
        for (std::size_t i = 0U; i < layer_whats.size(); ++i) {
            options[i] = labels[i];
        }
        if (const std::optional<std::size_t> chosen = ctx.context_menu(id, options)) {
            if (layer != nullptr && layer->what != layer_whats[*chosen] && open_to_it[*chosen]) {
                const Uid uid = layer->uid;
                set_layer_what(document, *layer, layer_whats[*chosen]);
                selection_.block = 0U;
                selection_.pick_layer(uid);
            }
            menu_what_ = 0U;
        } else if (!ctx.menu_open(id)) {
            menu_what_ = 0U;
        }
    }

    // The layer's kind, from its chip: both kinds, the one it is not as an
    // action when nothing is in the way and as the reason when something is.
    // A block the other kind does not have is never dropped behind an
    // artist's back: it is named, and the layer stays as it is until they
    // deal with it.
    if (menu_kind_ != 0U) {
        const gui::UiId id = part_id(0U, "layer.kind");
        Layer* layer = document.layer(menu_kind_);
        constexpr std::array<LayerKind, 2> kinds{LayerKind::Particles, LayerKind::Rigid};
        std::array<std::string, 2> labels;
        std::array<bool, 2> open_to_it{};
        for (std::size_t i = 0U; i < kinds.size(); ++i) {
            labels[i] = std::string{layer_kind_name(kinds[i])};
            open_to_it[i] = layer == nullptr || layer->kind == kinds[i];
            if (layer != nullptr && layer->kind != kinds[i]) {
                const std::vector<std::string> blocking = blocks_against_kind(*layer, kinds[i]);
                open_to_it[i] = blocking.empty();
                for (std::size_t b = 0U; b < blocking.size(); ++b) {
                    labels[i] += (b == 0U ? ": " : ", ") + blocking[b];
                }
            }
        }
        const std::array<std::string_view, 2> options{labels[0], labels[1]};
        if (const std::optional<std::size_t> chosen = ctx.context_menu(id, options)) {
            // The kind it already is, or one with a block in the way, changes
            // nothing: the entry read as the reason, not as something to do.
            if (layer != nullptr && layer->kind != kinds[*chosen] && open_to_it[*chosen]) {
                const Uid uid = layer->uid;
                set_layer_kind(document, *layer, kinds[*chosen]);
                selection_.block = 0U;
                selection_.pick_layer(uid);
            }
            menu_kind_ = 0U;
        } else if (!ctx.menu_open(id)) {
            menu_kind_ = 0U;
        }
    }
}

} // namespace ajfx::studio
