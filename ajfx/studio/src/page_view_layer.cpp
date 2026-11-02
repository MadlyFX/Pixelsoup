#include "page_view.hpp"

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

namespace {

// What a layer's heading says of its budget, and what that means.
struct BudgetNote final {
    std::string text;
    std::string tip;
};

BudgetNote budget_note(const Layer& layer, const LayerReport* report, const Analysis* analysis) {
    // A layer holding only objects that stand there has no particles to
    // budget: it says what it holds instead, since "0 of 64 max" would only
    // puzzle. An object the world moves is a body and does have a budget.
    const std::size_t standing = static_cast<std::size_t>(
        std::count_if(layer.blocks.begin(), layer.blocks.end(), [](const Block& b) {
            return b.enabled && b.kind == BlockKind::Object && choice_of(b, "motion") != "loose";
        }));
    const bool births = std::any_of(layer.blocks.begin(), layer.blocks.end(), [](const Block& b) {
        return b.enabled && (b.kind == BlockKind::Stream || b.kind == BlockKind::Burst ||
                             (b.kind == BlockKind::Object && choice_of(b, "motion") == "loose"));
    });
    BudgetNote note;
    note.tip = standing > 0U && !births ? "Objects standing in this layer."
                                        : "Most alive at once, of the most it can hold.";
    // Nothing is said of a layer with nothing born in it: "0 of 64 max" tells
    // no one anything, and the starters are what it offers.
    if (report != nullptr && report->error.empty() && has_births(layer)) {
        if (standing > 0U && !births) {
            note.text = std::to_string(standing) + (standing == 1U ? " object" : " objects");
        } else {
            note.text = thousands(report->capacity) + " max";
            if (analysis != nullptr) {
                if (const auto peak = analysis->peak_alive.find(layer.uid); peak != analysis->peak_alive.end()) {
                    note.text = thousands(peak->second) + " of " + note.text;
                }
            }
        }
    }
    return note;
}

// What the Baked / Live chip says when the pointer rests on it: what it is,
// what a click does, and how the cache stands.
std::string bake_tooltip(bool baked, const LayerBake* state) {
    std::string tip = baked ? "Baked: played from a cache. Click for live."
                            : "Live: simulated as it plays. Click to bake.";
    if (state != nullptr && state->state == BakeState::Current) {
        tip += " Cache is current.";
    } else if (state != nullptr && state->state == BakeState::Stale) {
        tip += " Cache is stale: " + state->why + ".";
    } else if (state != nullptr && state->state == BakeState::Unbaked) {
        tip += state->why.empty() ? std::string{" No cache yet."} : " " + state->why + ".";
    } else if (state != nullptr && state->state == BakeState::TooLarge) {
        tip += " Cannot bake: " + state->why + ".";
    }
    return tip;
}

} // namespace

float PageView::draw_layer(gui::UiContext& ctx, Document& document, std::size_t layer_index, const PageInputs& inputs,
                           float x, float y, float w) {
    const gui::Theme& theme = ctx.theme();
    Layer& layer = document.layers[layer_index];
    const Uid uid = layer.uid;
    const float top = y;
    layer_name_ = layer.name;

    y = draw_layer_header(ctx, document, layer, inputs, x, y, w);
    if (folded_.contains(uid)) {
        layer_areas_.push_back(
            LayerArea{uid, gui::Rect{x, top, w, y - top}, layer.blocks.size(), y, layer.kind == LayerKind::Rigid});
        return y + 14.0F;
    }
    y = draw_layer_settings(ctx, layer, x, y, w);
    y = draw_layer_blocks(ctx, document, layer, inputs, x, y, w);
    y = draw_starters(ctx, layer, x, y, w);
    y = draw_next_steps(ctx, document, layer, x, y, w);

    // The coach's notes about the layer as a whole.
    if (inputs.advice != nullptr) {
        for (std::size_t i = 0U; i < inputs.advice->size(); ++i) {
            const Advice& advice = (*inputs.advice)[i];
            if (advice.layer == uid && (advice.block == 0U || document.block(advice.block) == nullptr)) {
                y = draw_advice(ctx, advice, i, x, y, w);
            }
        }
    }
    y = draw_hood(ctx, hood_lines_for(inputs, uid, uid, true), x, y, w);

    // Adding a block, at the foot of the layer.
    const float add_y = y;
    y = draw_add_block(ctx, document, layer, x, y, w);
    layer_areas_.push_back(LayerArea{uid, gui::Rect{x, top, w, y - top}, layer.blocks.size(), add_y - 3.0F,
                                     layer.kind == LayerKind::Rigid});

    // A rule before the next layer.
    ctx.panel(gui::Rect{x, y + 10.0F, w, 1.0F}, theme.divider);
    return y + 26.0F;
}

// The heading: a twirl, the name, its budget and its switches. Returns where
// the layer's rows begin.
float PageView::draw_layer_header(gui::UiContext& ctx, Document& document, Layer& layer, const PageInputs& inputs,
                                  float x, float y, float w) {
    const gui::Theme& theme = ctx.theme();
    const Uid uid = layer.uid;
    const LayerReport* report = inputs.compiled != nullptr ? inputs.compiled->layer(uid) : nullptr;
    const bool folded = folded_.contains(uid);

    const gui::Rect head{x - 8.0F, y, w + 16.0F, layer_header};
    const bool hovered = head.contains(ctx.pointer_x(), ctx.pointer_y());
    const gui::Rect twirl{x - 6.0F, y + 6.0F, 22.0F, 24.0F};
    const gui::Interaction twirl_hit = ctx.grab(part_id(uid, "layer.twirl"), twirl);
    ctx.cursor_for(twirl_hit, gui::Cursor::Hand);
    editor::draw_twirl(ctx, twirl.center_x(), twirl.center_y(), !folded, theme.text_muted);
    if (twirl_hit.clicked) {
        if (folded) {
            folded_.erase(uid);
        } else {
            folded_.insert(uid);
        }
    }
    // The layer a click adds to takes the quiet selection tint, so what is
    // marked is what is added to. The accent bar below is the layer being
    // worked on, whose guides and paths the preview shows; the target is that
    // one, or the last one worked on once Esc or a fold has left none.
    if (target_ == uid) {
        ctx.rounded(head, theme.selection, theme.radius);
    }
    // The layer being worked on, marked in the margin: the preview's guides
    // and paths are its.
    if (working_layer(document) == uid) {
        const gui::Rect mark{x - 14.0F, y + 9.0F, 3.0F, layer_header - 18.0F};
        ctx.rounded(mark, theme.accent, 1.5F);
        ctx.tooltip(gui::Rect{mark.x - 4.0F, mark.y, 10.0F, mark.height},
                    "Working layer. Esc clears.");
    }

    const float buttons = 3.0F * 28.0F;
    const gui::Rect menu{x + w - 26.0F, y + 6.0F, 24.0F, 24.0F};
    const gui::Rect solo{menu.x - 28.0F, menu.y, 24.0F, 24.0F};
    const gui::Rect eye{solo.x - 28.0F, menu.y, 24.0F, 24.0F};
    const BudgetNote budget = budget_note(layer, report, inputs.analysis);
    const float budget_w = budget.text.empty() ? 0.0F : ctx.font_width(budget.text) + 12.0F;
    // What the layer is, said on the layer itself: it decides what moves it
    // and how the page's ground, gravity and wind act on it. What moves it
    // (the kind) follows from that, and is its own chip only in the expert
    // view, under Advanced.
    const std::string what_text{layer_what_name(layer.what)};
    const float what_w = ctx.font_width(what_text) + 20.0F;
    const std::string kind_text{layer_kind_name(layer.kind)};
    const float kind_w = inputs.expert ? ctx.font_width(kind_text) + 20.0F + 4.0F : 0.0F;
    // Baked or live, said and switched on the layer itself.
    const std::string bake_text = layer.baked ? "Baked" : "Live";
    const float bake_w = ctx.font_width(bake_text) + 20.0F;
    const gui::Rect name{x + 20.0F, y,
                         std::max(w - 20.0F - buttons - budget_w - what_w - kind_w - bake_w - 20.0F, 40.0F),
                         layer_header};
    if (rename_.editing(uid)) {
        if (std::optional<std::string> renamed = rename_.draw(ctx, gui::Rect{name.x, y + 5.0F, name.width, 26.0F})) {
            layer.name = std::move(*renamed);
        }
    } else {
        const gui::Interaction name_hit = ctx.grab(part_id(uid, "layer.name"), name);
        label_sized(ctx, name, layer.name.empty() ? "Layer" : layer.name,
                    gui::fade(theme.text, layer.enabled ? 1.0F : hidden_dim), 1.2F);
        if (name_hit.clicked) {
            // Working on the layer: its guides and paths, and no other's.
            selection_.pick_layer(uid);
            if (const std::optional<Document::BlockPlace> place = document.find_block(selection_.block);
                !place || place->layer->uid != uid) {
                selection_.block = 0U;
            }
            if (clicks_.double_click(uid, ctx.time_seconds())) {
                rename_.begin(ctx, uid, layer.name);
            }
        }
        ctx.tooltip(name, "Click to select, double-click to rename");
    }
    {
        const float dim = layer.enabled ? 1.0F : hidden_dim;
        // A heading chip that is a button, as it looks: it opens a menu.
        const auto menu_chip = [&](const gui::Rect& at, std::string_view part, const std::string& text,
                                   std::string_view tip) {
            const gui::Interaction hit = ctx.grab(part_id(uid, part), at);
            ctx.cursor_for(hit, gui::Cursor::Hand);
            ctx.rounded(at,
                        gui::blend(gui::fade(theme.widget, 0.7F * dim), theme.widget_hover, hit.hovered ? 0.35F : 0.0F),
                        theme.radius_small);
            ctx.label(gui::Rect{at.x + 10.0F, at.y, at.width - 20.0F, at.height}, text, gui::fade(theme.text_muted, dim));
            ctx.tooltip(at, tip);
            return hit.clicked;
        };
        // What it is: a menu of everything a layer can be.
        const gui::Rect what_chip{eye.x - budget_w - what_w - 8.0F, y + (layer_header - 22.0F) * 0.5F, what_w, 22.0F};
        header_chips_.push_back(HeaderChip{uid, "what", what_chip});
        if (menu_chip(what_chip, "layer.what", what_text, layer_what_description(layer.what))) {
            menu_what_ = uid;
            ctx.open_context_menu(part_id(0U, "layer.what"), what_chip.x, what_chip.bottom() + 2.0F);
        }
        // What moves it, under Advanced: both kinds.
        gui::Rect chip = what_chip;
        if (inputs.expert) {
            chip = gui::Rect{what_chip.x - kind_w, what_chip.y, kind_w - 4.0F, what_chip.height};
            header_chips_.push_back(HeaderChip{uid, "kind", chip});
            if (menu_chip(chip, "layer.kind", kind_text, layer_kind_description(layer.kind))) {
                menu_kind_ = uid;
                ctx.open_context_menu(part_id(0U, "layer.kind"), chip.x, chip.bottom() + 2.0F);
            }
        }

        // Live is what every layer is, and says nothing: it shows on the
        // target, or while the pointer is over the heading. Baked always says
        // so. The room is kept either way, so the name does not move.
        if (layer.baked || hovered || target_ == uid) {
            const gui::Rect baked{chip.x - bake_w - 4.0F, chip.y, bake_w, chip.height};
            const gui::Interaction bake_hit = ctx.grab(part_id(uid, "layer.bake"), baked);
            ctx.cursor_for(bake_hit, gui::Cursor::Hand);
            const LayerBake* state = inputs.bake != nullptr ? inputs.bake->layer(uid) : nullptr;
            const bool stale =
                state != nullptr && state->state != BakeState::Current && state->state != BakeState::Live;
            const gui::UiColor fill = !layer.baked ? gui::fade(theme.widget, 0.7F * dim)
                                      : stale      ? gui::fade(theme.caution, 0.45F * dim)
                                                   : gui::fade(theme.good, 0.45F * dim);
            ctx.rounded(baked, bake_hit.hovered ? gui::blend(fill, theme.widget_hover, 0.35F) : fill,
                        theme.radius_small);
            ctx.label(gui::Rect{baked.x + 10.0F, baked.y, baked.width - 20.0F, baked.height}, bake_text,
                      gui::fade(layer.baked ? theme.text : theme.text_muted, dim));
            const std::string bake_tip = bake_tooltip(layer.baked, state);
            ctx.tooltip(baked, bake_tip);
            header_chips_.push_back(HeaderChip{uid, "bake", baked});
            if (bake_hit.clicked) {
                layer.baked = !layer.baked;
            }
        }
    }
    if (!budget.text.empty()) {
        const gui::Rect budget_rect{eye.x - budget_w - 4.0F, y, budget_w, layer_header};
        ctx.label(budget_rect, budget.text, gui::fade(theme.text_muted, layer.enabled ? 1.0F : hidden_dim));
        ctx.tooltip(budget_rect, budget.tip);
    }
    if (editor::row_flag_toggle(ctx, part_id(uid, "layer.eye"), eye, editor::RowFlag::Visible, layer.enabled, hovered)) {
        layer.enabled = !layer.enabled;
    }
    if (editor::icon_button(ctx, part_id(uid, "layer.solo"), solo, gui::Icon::Solo,
                            layer.solo ? "Stop playing this layer alone" : "Play this layer alone (preview only)",
                            layer.solo)) {
        layer.solo = !layer.solo;
    }
    if (editor::icon_button(ctx, part_id(uid, "layer.menu"), menu, gui::Icon::Details, "Layer actions")) {
        menu_layer_ = uid;
        ctx.open_context_menu(part_id(0U, "layer.context"), menu.x, menu.bottom() + 2.0F);
    }
    y += layer_header + 2.0F;
    return y;
}

// How long each layer's particles live, and the seed that deals the layer's
// random values afresh.
float PageView::draw_layer_settings(gui::UiContext& ctx, Layer& layer, float x, float y, float w) {
    const gui::Theme& theme = ctx.theme();
    const Uid uid = layer.uid;
    // How long each particle lives: every layer has one.
    {
        const gui::Rect row{x + 12.0F, y, w - 12.0F, row_height};
        const gui::Rect label{row.x, y, label_width, row_height};
        // A rigid piece is not born to die: it lies where it lands, so it
        // stays rather than lives.
        ctx.label(label, layer.kind == LayerKind::Rigid ? "Stays for" : "Each lives", theme.text_muted);
        ctx.tooltip(label, "Seconds each lives, between the two.");
        float low = layer.life_min;
        float high = layer.life_max;
        const float fx = row.x + label_width;
        if (number(ctx, part_id(uid, "layer.life", 0U), gui::Rect{fx, y, 70.0F, row_height}, low, 0.01F, 2, {}, 0.0F, 6.0F)) {
            layer.life_min = std::clamp(low, 0.01F, 600.0F);
            layer.life_max = std::max(layer.life_max, layer.life_min);
        }
        ctx.label(gui::Rect{fx + 74.0F, y, 14.0F, row_height}, "to", theme.text_muted);
        if (number(ctx, part_id(uid, "layer.life", 1U), gui::Rect{fx + 88.0F, y, 70.0F, row_height}, high, 0.01F, 2, {}, 0.0F, 6.0F)) {
            layer.life_max = std::clamp(high, layer.life_min, 600.0F);
        }
        ctx.label(gui::Rect{fx + 162.0F, y, 20.0F, row_height}, "s", theme.text_muted);
        // The layer's seed, at the row's end: every block's random values at
        // once, where a block's own seed changes only its own.
        const float seed_x = row.right() - editor::reset_button_width - 4.0F - 60.0F;
        if (seed_x > fx + 190.0F + ctx.font_width("Seed")) {
            const gui::Rect seed_label{seed_x - ctx.font_width("Seed") - 8.0F, y, ctx.font_width("Seed") + 4.0F, row_height};
            ctx.label(seed_label, "Seed", theme.text_muted);
            ctx.tooltip(seed_label, "Another random look. 0 is the default.");
            float seed = static_cast<float>(layer.seed);
            if (number(ctx, part_id(uid, "layer.seed"), gui::Rect{seed_x, y, 60.0F, row_height}, seed, 0.2F, 0, {}, 0.0F,
                       100.0F)) {
                layer.seed = static_cast<std::uint32_t>(std::lround(std::clamp(seed, 0.0F, 1.0e6F)));
            }
            if (layer.seed != 0U) {
                const gui::Rect reset{row.right() - editor::reset_button_width, y + (row_height - 18.0F) * 0.5F,
                                      editor::reset_button_width, 18.0F};
                if (editor::reset_button(ctx, part_id(uid, "layer.seed", 99U), reset, "Back to the layer's own: 0")) {
                    layer.seed = 0U;
                }
            }
        }
        if (!layer.enabled) {
            grey_out(ctx, row);
        }
        y += row_height + 8.0F;
    }

    return y;
}

// The foot of the layer: a row that opens the block picker.
float PageView::draw_add_block(gui::UiContext& ctx, const Document& document, Layer& layer, float x, float y,
                               float w) {
    const gui::Theme& theme = ctx.theme();
    const Uid uid = layer.uid;
    // Adding a block, at the foot of the layer.
    const gui::Rect add{x, y, w, 28.0F};
    const gui::Interaction add_hit = ctx.grab(part_id(uid, "layer.add"), add);
    ctx.cursor_for(add_hit, gui::Cursor::Hand);
    const float glow = ctx.ease(part_id(uid, "layer.add.hover"), add_hit.hovered ? 1.0F : 0.0F);
    if (glow > 0.0F) {
        ctx.rounded(add, gui::fade(theme.widget_hover, 0.5F * glow), theme.radius);
    }
    const gui::UiColor ink = gui::blend(gui::fade(theme.text_muted, 0.75F), theme.text, glow);
    gui::draw_icon(ctx.draw_list(), gui::Icon::Plus, add.x + 16.0F, add.center_y(), ink, 0.8F);
    ctx.label(gui::Rect{add.x + 32.0F, add.y, add.width - 32.0F, add.height}, "Add block", ink);
    if (add_hit.clicked) {
        selection_.last_layer = uid;
        open_picker(PickerMode::Blocks, uid, layer.blocks.size(), add.x + 20.0F, add.bottom() + 4.0F, document);
    }
    y += 32.0F;
    return y;
}

// A rigid layer holds blocks for four different things at once -- its own
// pieces, what they break into, the props beside them and the world they
// share -- and a flat list says nothing about which a block is for. So its
// blocks read under headings, each block under the heading its kind belongs
// to, while `Layer::blocks` stays one list in the order it was authored:
// block order is for reading, never for the effect (see compile.cpp), so
// drawing it grouped changes nothing but the reading.
//
// A particle layer has one scope and keeps its flat list.
float PageView::draw_layer_blocks(gui::UiContext& ctx, Document& document, Layer& layer, const PageInputs& inputs,
                                  float x, float y, float w) {
    if (layer.kind != LayerKind::Rigid) {
        for (std::size_t i = 0U; i < layer.blocks.size(); ++i) {
            y = draw_block(ctx, document, layer, i, inputs, x, y, w);
        }
        return y;
    }
    for (const RigidScope scope : rigid_scopes) {
        // The world row is the layer's own, not a block on its list, so it
        // gives The world a heading even where no block does.
        const bool world_row = scope == RigidScope::World && layer.world.has_value();
        std::vector<std::size_t> members;
        for (std::size_t i = 0U; i < layer.blocks.size(); ++i) {
            if (rigid_scope(layer.blocks[i].kind) != scope) {
                continue;
            }
            // Breaking belongs to the Object above it, and is drawn there.
            // What is left under this heading is a block with no Object over
            // it, which the coach asks about.
            if (scope == RigidScope::Breaking && object_of(layer, i)) {
                continue;
            }
            members.push_back(i);
        }
        if (members.empty() && !world_row) {
            continue;
        }
        y = draw_scope_heading(ctx, scope, x, y, w);
        if (world_row) {
            y = draw_world_row(ctx, layer, x, y, w);
        }
        for (const std::size_t index : members) {
            y = draw_block(ctx, document, layer, index, inputs, x, y, w);
            if (scope != RigidScope::Objects) {
                continue;
            }
            // What this Object owns, indented under it: reading the page is
            // then reading each thing in the layer and what becomes of it.
            std::size_t end = index + 1U;
            for (std::size_t j = index + 1U;
                 j < layer.blocks.size() && layer.blocks[j].kind != BlockKind::Object; ++j) {
                if (!owned_by_object(layer.blocks[j].kind)) {
                    continue;
                }
                y = draw_block(ctx, document, layer, j, inputs, x + object_indent, y, w - object_indent);
                end = j + 1U;
            }
            // A gap at the group's foot, so a Break dragged onto this Object
            // lands in it rather than at the end of the layer.
            slots_.push_back(Slot{layer.uid, end, gui::Rect{x + object_indent, y, w - object_indent, 0.0F},
                                  RigidScope::Breaking, true});
        }
        // The gap at the heading's foot: a block of this scope dropped
        // below everything under it lands after the last of them, which is
        // one past that block in the list.
        const std::size_t end = members.empty() ? layer.blocks.size() : members.back() + 1U;
        slots_.push_back(Slot{layer.uid, end, gui::Rect{x, y, w, 0.0F}, scope, true});
        y += scope_gap;
    }
    return y;
}

float PageView::draw_scope_heading(gui::UiContext& ctx, RigidScope scope, float x, float y, float w) {
    const gui::Theme& theme = ctx.theme();
    const std::string_view text = rigid_scope_name(scope);
    const float text_w = ctx.font_width(text);
    ctx.label(gui::Rect{x + 2.0F, y, text_w + 6.0F, scope_heading}, text, gui::fade(theme.text_muted, 0.85F));
    ctx.tooltip(gui::Rect{x, y, text_w + 10.0F, scope_heading}, rigid_scope_help(scope));
    // A hairline on from the words, so the heading reads as a band over its
    // blocks rather than as another row of them.
    const float rule_x = x + text_w + 14.0F;
    if (x + w - rule_x > 24.0F) {
        ctx.panel(gui::Rect{rule_x, y + scope_heading * 0.5F, x + w - rule_x, 1.0F}, theme.divider);
    }
    return y + scope_heading;
}

// A rigid layer's world: what every piece in it is made of and how it
// behaves on contact. It belongs to the layer, like how long a particle
// lives, so it reads as a row of The world rather than as a block that
// could be dragged away or removed.
float PageView::draw_world_row(gui::UiContext& ctx, Layer& layer, float x, float y, float w) {
    const gui::Theme& theme = ctx.theme();
    const BlockDef* world_def = find_block_def(BlockKind::Physics);
    const Uid world_uid = layer.world->uid;
    const bool shown = open_.contains(world_uid);
    const gui::Rect world_row{x + 12.0F, y, w - 12.0F, row_height};
    const gui::Interaction hit = ctx.grab(part_id(world_uid, "layer.world"), world_row);
    ctx.cursor_for(hit, gui::Cursor::Hand);
    editor::draw_twirl(ctx, world_row.x + 8.0F, world_row.center_y(), shown, theme.text_muted);
    ctx.label(gui::Rect{world_row.x + 22.0F, y, label_width, row_height}, "Bodies",
              hit.hovered ? theme.text : theme.text_muted);
    ctx.label(gui::Rect{world_row.x + label_width + 12.0F, y, world_row.width - label_width - 20.0F, row_height},
              block_summary(*layer.world), gui::fade(theme.text_muted, 0.85F));
    ctx.tooltip(world_row, "What the pieces are made of.");
    if (hit.clicked) {
        if (shown) {
            open_.erase(world_uid);
        } else {
            open_.insert(world_uid);
        }
        selection_.pick_layer(layer.uid);
    }
    y += row_height + 2.0F;
    if (shown && world_def != nullptr) {
        y = draw_controls(ctx, *layer.world, *world_def, layer.kind, x + 24.0F, y, w - 36.0F);
    }
    if (!layer.enabled) {
        grey_out(ctx, gui::Rect{world_row.x, world_row.y, world_row.width, y - world_row.y});
    }
    return y + 8.0F;
}

// What a layer with nothing born in it can start with, as tiles to click. The
// question and the tiles go as soon as something is born: they are for the
// first block, and the way on from there is the coach's notes.
float PageView::draw_starters(gui::UiContext& ctx, const Layer& layer, float x, float y, float w) {
    const std::vector<Suggestion> choices = starters(layer);
    if (choices.empty()) {
        return y;
    }
    const gui::Theme& theme = ctx.theme();
    const float top = y;
    ctx.label(gui::Rect{x + 2.0F, y, w - 4.0F, row_height}, "Start with", theme.text_muted);
    y += row_height + 2.0F;

    // Two to a row where they fit, one where they do not.
    const std::size_t columns = w >= starter_two_columns ? 2U : 1U;
    const float tile_w = (w - starter_gap * static_cast<float>(columns - 1U)) / static_cast<float>(columns);
    const float text_w = tile_w - starter_icon_room - 14.0F;
    for (std::size_t first = 0U; first < choices.size(); first += columns) {
        const std::size_t end = std::min(first + columns, choices.size());
        for (std::size_t i = first; i < end; ++i) {
            const Suggestion& choice = choices[i];
            const gui::Rect tile{x + static_cast<float>(i - first) * (tile_w + starter_gap), y, tile_w, starter_height};
            const gui::UiId id = part_id(layer.uid, "layer.starter", static_cast<std::uint32_t>(i));
            const gui::Interaction hit = ctx.grab(id, tile);
            ctx.cursor_for(hit, gui::Cursor::Hand);
            const float lift = ctx.ease(id, hit.hovered ? 1.0F : 0.0F);
            ctx.rounded(tile, gui::blend(theme.widget, theme.widget_hover, lift), theme.radius);
            if (lift > 0.0F) {
                ctx.rounded_outline(tile, gui::fade(theme.accent, 0.6F * lift), theme.radius);
            }
            draw_category_icon(ctx, choice.icon, tile.x + 24.0F, tile.center_y(),
                               gui::blend(theme.text_muted, theme.text, lift));
            ctx.label(gui::Rect{tile.x + starter_icon_room, tile.y, text_w, tile.height}, choice.title, theme.text);
            ctx.tooltip(tile, suggestion_adds(choice));
            starter_tiles_.emplace_back(layer.uid, tile);
            if (hit.clicked) {
                deferred_.push_back([this, owner = layer.uid, chosen = choice](Document& document) {
                    take_suggestion(document, owner, chosen);
                });
            }
        }
        y += starter_height + starter_gap;
    }
    if (!layer.enabled) {
        grey_out(ctx, gui::Rect{x, top, w, y - starter_gap - top});
    }
    return y + 4.0F;
}

// What a layer with something born in it offers to add next: a row of chips
// under its blocks, wrapping where a row is full. Each is named for the block
// it adds ("Look"), with what a click adds in its tooltip, where the coach used
// to word the same as a fault in an amber card. They go as they are taken.
float PageView::draw_next_steps(gui::UiContext& ctx, const Document& document, const Layer& layer, float x, float y,
                                float w) {
    const std::vector<Suggestion> steps = next_steps(document, layer);
    if (steps.empty()) {
        return y;
    }
    const gui::Theme& theme = ctx.theme();
    y += 2.0F;
    const float label_w = ctx.font_width("Next") + 14.0F;
    ctx.label(gui::Rect{x + 2.0F, y, label_w, next_chip_height}, "Next", theme.text_muted);
    const float start = x + 2.0F + label_w;
    float cx = start;
    for (std::size_t i = 0U; i < steps.size(); ++i) {
        const Suggestion& step = steps[i];
        const float chip_w = ctx.font_width(step.title) + next_chip_room;
        // A row that is full goes on below, lined up under the first chip.
        if (cx > start && cx + chip_w > x + w) {
            cx = start;
            y += next_chip_height + next_chip_gap;
        }
        const gui::Rect chip{cx, y, chip_w, next_chip_height};
        next_step_chips_.emplace_back(layer.uid, chip);
        if (editor::pill_button(ctx, part_id(layer.uid, "layer.next", static_cast<std::uint32_t>(i)), chip,
                                gui::Icon::Plus, step.title, suggestion_adds(step))) {
            deferred_.push_back([this, owner = layer.uid, chosen = step](Document& edited) {
                take_suggestion(edited, owner, chosen);
            });
        }
        cx += chip_w + next_chip_gap;
    }
    return y + next_chip_height + 8.0F;
}

} // namespace ajfx::studio
