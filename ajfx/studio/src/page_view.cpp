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

bool PageView::gesture_live() const noexcept {
    return numbers_.live() || drag_block_ != 0U || rename_.active();
}

void PageView::forget() {
    open_.clear();
    details_.clear();
    folded_.clear();
    drag_block_ = 0U;
    dragging_ = false;
    picker_.close();
    menu_block_ = menu_layer_ = menu_kind_ = menu_what_ = 0U;
    rename_.reset();
    scroll_.reset();
    selection_ = {};
    restore_scroll_ = -1.0F;
    incoming_drop_.reset();
}

PageViewState PageView::view_state() const {
    PageViewState view;
    view.folded.assign(folded_.begin(), folded_.end());
    view.open.assign(open_.begin(), open_.end());
    view.details.assign(details_.begin(), details_.end());
    view.focus = selection_.block;
    view.layer = selection_.picked_layer();
    view.scroll = scroll_.offset();
    return view;
}

void PageView::restore(const PageViewState& view) {
    folded_ = std::set<Uid>{view.folded.begin(), view.folded.end()};
    open_ = std::set<Uid>{view.open.begin(), view.open.end()};
    details_ = std::set<Uid>{view.details.begin(), view.details.end()};
    selection_ = Selection{view.focus, view.focus == 0U ? view.layer : 0U, view.layer};
    // Applied on the next frame, before the page is laid out; the page clamps
    // it once it has measured itself.
    restore_scroll_ = view.scroll;
}

Uid PageView::working_layer(const Document& document) const {
    if (selection_.block != 0U) {
        for (const Layer& layer : document.layers) {
            for (const Block& block : layer.blocks) {
                if (block.uid == selection_.block) {
                    return folded_.contains(layer.uid) ? 0U : layer.uid;
                }
            }
        }
    }
    if (selection_.layer != 0U && document.layer(selection_.layer) != nullptr && !folded_.contains(selection_.layer)) {
        return selection_.layer;
    }
    return 0U;
}

Uid PageView::target_layer(const Document& document) const {
    if (const Uid working = working_layer(document); working != 0U) {
        return working;
    }
    if (document.layer(selection_.last_layer) != nullptr) {
        return selection_.last_layer;
    }
    return document.layers.empty() ? 0U : document.layers.back().uid;
}

void PageView::insert(Document& document, const Incoming& incoming, Uid layer_uid, std::size_t index) {
    if (incoming.kind == Incoming::Kind::Layer) {
        Layer layer;
        if (const std::optional<LayerWhat> empty = empty_layer_what(incoming.id)) {
            // Named for what it is, which is what the artist chose it by.
            layer = make_layer(document, *empty,
                               *empty == LayerWhat::Other ? "Layer " + std::to_string(document.layers.size() + 1U)
                                                          : std::string{layer_what_name(*empty)});
        } else if (std::optional<Layer> preset = make_layer_preset(document, incoming.id)) {
            layer = std::move(*preset);
        } else {
            return;
        }
        selection_.pick_layer(layer.uid);
        selection_.block = 0U;
        folded_.erase(layer.uid);
        index = std::min(index, document.layers.size());
        document.layers.insert(document.layers.begin() + static_cast<std::ptrdiff_t>(index), std::move(layer));
        return;
    }
    Layer* layer = document.layer(layer_uid);
    if (layer == nullptr) {
        return;
    }
    // A block outside this layer's kind is never offered, but a drag can
    // still be let go over the wrong layer: nothing lands.
    if (incoming.kind == Incoming::Kind::Block && !block_fits(incoming.id, layer->kind)) {
        return;
    }
    std::optional<Block> block;
    if (incoming.kind == Incoming::Kind::Module) {
        if (const RawKindDef* def = find_raw_kind(incoming.id)) {
            block = make_raw_block(document, def->make());
        }
    } else {
        block = make_block(document, incoming.id);
    }
    if (!block) {
        return;
    }
    const Uid uid = block->uid;
    insert_block(*layer, index, std::move(*block));
    work_on(*layer, uid);
}

void PageView::work_on(const Layer& layer, Uid block) {
    open_.insert(block);
    folded_.erase(layer.uid);
    selection_.work_on_block(layer.uid, block);
}

void PageView::select_block(const Document& document, Uid block) {
    for (const Layer& layer : document.layers) {
        for (const Block& b : layer.blocks) {
            if (b.uid == block) {
                work_on(layer, block);
                return;
            }
        }
    }
}

void PageView::select_layer(Uid layer) {
    folded_.erase(layer);
    selection_.block = 0U;
    selection_.pick_layer(layer);
}

void PageView::take_suggestion(Document& document, Uid layer_uid, const Suggestion& suggestion) {
    const Layer* layer = document.layer(layer_uid);
    const Uid first = add_suggestion(document, layer_uid, suggestion);
    if (layer != nullptr && first != 0U) {
        work_on(*layer, first);
    }
}

std::vector<gui::Rect> PageView::starter_tiles(Uid layer) const {
    std::vector<gui::Rect> tiles;
    for (const auto& [owner, rect] : starter_tiles_) {
        if (owner == layer) {
            tiles.push_back(rect);
        }
    }
    return tiles;
}

std::optional<gui::Rect> PageView::header_chip(Uid layer, std::string_view name) const {
    for (const HeaderChip& chip : header_chips_) {
        if (chip.layer == layer && chip.name == name) {
            return chip.rect;
        }
    }
    return std::nullopt;
}

std::vector<gui::Rect> PageView::next_step_chips(Uid layer) const {
    std::vector<gui::Rect> chips;
    for (const auto& [owner, rect] : next_step_chips_) {
        if (owner == layer) {
            chips.push_back(rect);
        }
    }
    return chips;
}

void PageView::add(Document& document, const Incoming& incoming) {
    if (incoming.kind == Incoming::Kind::Layer) {
        insert(document, incoming, 0U, document.layers.size());
        return;
    }
    Uid target = target_layer(document);
    if (target == 0U) {
        // A block needs a layer to live in.
        insert(document, Incoming{Incoming::Kind::Layer, {}, {}, {}}, 0U, 0U);
        target = document.layers.back().uid;
    }
    const Layer* layer = document.layer(target);
    const BlockDef* def = incoming.kind == Incoming::Kind::Block ? find_block_def(incoming.id) : nullptr;
    if (def != nullptr && layer != nullptr && !fits(def->layers, layer->kind)) {
        return;
    }
    if (def != nullptr && def->unique && layer != nullptr &&
        std::any_of(layer->blocks.begin(), layer->blocks.end(), [&](const Block& b) { return b.kind == def->kind; })) {
        // Already there: open the one it has rather than refuse silently.
        for (const Block& b : layer->blocks) {
            if (b.kind == def->kind) {
                open_.insert(b.uid);
                selection_.block = b.uid;
                folded_.erase(layer->uid);
            }
        }
        return;
    }
    insert(document, incoming, target, layer != nullptr ? layer->blocks.size() : 0U);
}

bool PageView::number(gui::UiContext& ctx, gui::UiId id, const gui::Rect& field, float& value, float speed,
                      int decimals, std::string_view axis, float min, float max) {
    double out = value;
    const bool changed = numbers_.draw(ctx, id, field, axis, value, speed, out, decimals);
    if (max > min) {
        // Where the value sits in its usual range: a thin line under the field.
        const float t = std::clamp((value - min) / (max - min), 0.0F, 1.0F);
        const gui::Rect track{field.x + 4.0F, field.bottom() - 3.0F, field.width - 8.0F, 2.0F};
        ctx.rounded(track, gui::fade(ctx.theme().text_muted, 0.18F), 1.0F);
        if (t > 0.0F) {
            ctx.rounded(gui::Rect{track.x, track.y, track.width * t, track.height},
                        gui::fade(ctx.theme().accent, 0.6F), 1.0F);
        }
    }
    if (changed && std::isfinite(out)) {
        value = static_cast<float>(out);
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ page

bool PageView::popover_covers(float x, float y) const noexcept {
    return picker_.covers(x, y);
}

void PageView::draw(gui::UiContext& ctx, const gui::Rect& bounds, Document& document, const PageInputs& inputs) {
    const gui::Theme& theme = ctx.theme();
    page_ = bounds;
    deferred_.clear();
    ctx.rounded(bounds, theme.panel, theme.radius_large);

    slots_.clear();
    layer_areas_.clear();
    layer_tops_.clear();
    starter_tiles_.clear();
    next_step_chips_.clear();
    header_chips_.clear();
    block_chips_.clear();
    solid_layers_.clear();
    for (const Layer& layer : document.layers) {
        if (layer.kind == LayerKind::Rigid) {
            solid_layers_.emplace_back(layer.uid, layer.name.empty() ? std::string{"Solid"} : layer.name);
        }
    }
    if (selection_.layer != 0U && document.layer(selection_.layer) == nullptr) {
        selection_.layer = 0U;
    }
    if (!ctx.typing() && !picker_.is_open() && ctx.key_pressed(gui::Key::Escape)) {
        // Nothing worked on: the preview's guides and paths step back.
        selection_.clear_working();
    }
    // Once a frame, for every heading to mark.
    target_ = target_layer(document);
    const gui::Rect view = gui::inset(bounds, 2.0F);
    if (restore_scroll_ >= 0.0F) {
        scroll_.scroll_to(restore_scroll_);
        restore_scroll_ = -1.0F;
    }
    const float offset = scroll_.begin(ctx, view);
    const float width = std::min(view.width - 64.0F, page_max_width);
    const float x = view.x + std::max((view.width - width) * 0.5F, 32.0F);
    float y = view.y + 24.0F - offset;
    content_x_ = x;
    content_w_ = width;

    y = draw_header(ctx, document, inputs, x, y, width);
    for (std::size_t i = 0U; i < document.layers.size(); ++i) {
        layer_tops_.emplace_back(document.layers[i].uid, y);
        y = draw_layer(ctx, document, i, inputs, x, y, width);
    }
    layers_end_y_ = y;

    // Adding a layer: what it is, or a ready-made part.
    const gui::Rect add{x, y + 4.0F, 150.0F, 30.0F};
    if (editor::pill_button(ctx, gui::ui_id("studio.add-layer"), add, gui::Icon::Plus, "Add layer",
                            "Asks what it is")) {
        open_picker(PickerMode::Layers, 0U, 0U, add.x, add.bottom() + 6.0F, document);
    }
    if (document.layers.empty()) {
        ctx.label(gui::Rect{add.right() + 12.0F, add.y, width - add.width - 12.0F, add.height},
                  "An effect is made of layers.", theme.text_muted);
    } else {
        ctx.label(gui::Rect{add.right() + 12.0F, add.y, width - add.width - 12.0F, add.height},
                  "/ adds a block", gui::fade(theme.text_muted, 0.7F));
    }
    y += 80.0F;

    scroll_.end(ctx, y + offset - view.y);
    resolve_drag(ctx);
    resolve_incoming(ctx, document);
    for (auto& action : deferred_) {
        action(document);
    }
    deferred_.clear();
    rename_.end_frame(ctx);
}

float PageView::draw_header(gui::UiContext& ctx, Document& document, const PageInputs& inputs, float x, float y,
                            float w) {
    const gui::Theme& theme = ctx.theme();
    // The title: large, and a click away from editing.
    const gui::UiId title_id = gui::ui_id("studio.title");
    const gui::Rect title{x, y, w, 42.0F};
    if (ctx.focus() == title_id) {
        static_cast<void>(ctx.text_field(title_id, gui::Rect{x, y + 6.0F, w, 30.0F}, document.name));
    } else {
        const gui::Interaction interaction = ctx.grab(title_id ^ 1U, title);
        ctx.cursor_for(interaction, gui::Cursor::Text);
        label_sized(ctx, title, document.name.empty() ? "Untitled effect" : document.name,
                    document.name.empty() ? theme.text_muted : theme.text, 1.75F);
        if (interaction.clicked) {
            ctx.focus_text_field_selected(title_id, document.name);
        }
    }
    y += 46.0F;

    // A line about the whole effect.
    std::string about = thousands(document.layers.size()) + (document.layers.size() == 1U ? " layer" : " layers");
    if (inputs.compiled != nullptr) {
        about += ", up to " + thousands(inputs.compiled->total_capacity()) + " particles";
    }
    ctx.label(gui::Rect{x, y, w, 18.0F}, about, theme.text_muted);
    y += 26.0F;

    // The settings every effect has, as one compact strip.
    const auto setting = [&](float at, std::string_view label, gui::UiId id, float& value, float speed, int decimals,
                             std::string_view unit, float min, float max, float hard_min, float hard_max,
                             std::string_view tip) {
        const float label_w = ctx.font_width(label) + 8.0F;
        const gui::Rect label_rect{at, y, label_w, row_height};
        ctx.label(label_rect, label, theme.text_muted);
        ctx.tooltip(label_rect, tip);
        const gui::Rect field{at + label_w, y, 62.0F, row_height};
        float v = value;
        if (number(ctx, id, field, v, speed, decimals, {}, min, max)) {
            value = std::clamp(v, hard_min, hard_max);
        }
        ctx.label(gui::Rect{field.right() + 4.0F, y, 30.0F, row_height}, unit, theme.text_muted);
        return field.right() + 4.0F + ctx.font_width(unit) + 18.0F;
    };
    float at = x;
    at = setting(at, "Size", gui::ui_id("studio.scale"), document.scale, 0.01F, 2, "x", 0.1F, 4.0F, 0.01F, 100.0F,
                 "Scales the whole effect.");
    {
        // The loop: as long as the effect takes to play out, unless set by
        // hand. Dragging or typing a length sets it; Auto gives it back.
        const float label_w = ctx.font_width("Loop") + 8.0F;
        const gui::Rect label_rect{at, y, label_w, row_height};
        ctx.label(label_rect, "Loop", theme.text_muted);
        ctx.tooltip(label_rect, document.loop_auto
                                    ? "Loop length, worked out from the effect."
                                    : "Loop length, as you set it.");
        const gui::Rect field{at + label_w, y, 62.0F, row_height};
        float v = document.loop_auto ? inputs.natural_loop : document.loop_seconds;
        if (number(ctx, gui::ui_id("studio.loop"), field, v, 0.05F, 1, {}, 1.0F, 20.0F)) {
            document.loop_seconds = std::clamp(v, 0.5F, 120.0F);
            document.loop_auto = false;
        }
        ctx.label(gui::Rect{field.right() + 4.0F, y, 30.0F, row_height}, "s", theme.text_muted);
        const gui::Rect auto_chip{field.right() + 4.0F + ctx.font_width("s") + 6.0F, y + 2.0F,
                                  ctx.font_width("Auto") + 18.0F, row_height - 4.0F};
        const gui::Interaction chip = ctx.grab(gui::ui_id("studio.loop.auto"), auto_chip);
        ctx.cursor_for(chip, gui::Cursor::Hand);
        ctx.rounded(auto_chip,
                    document.loop_auto ? gui::fade(theme.accent, 0.22F)
                                       : (chip.hovered ? theme.widget_hover : theme.widget),
                    auto_chip.height * 0.5F);
        ctx.label_centered(auto_chip, "Auto", document.loop_auto ? theme.accent : theme.text_muted);
        ctx.tooltip(auto_chip, document.loop_auto ? "Click to keep this length."
                                                  : "Click to follow the effect.");
        if (chip.clicked) {
            if (document.loop_auto) {
                document.loop_seconds = std::clamp(inputs.natural_loop, 0.5F, 120.0F);
            }
            document.loop_auto = !document.loop_auto;
        }
        at = auto_chip.right() + 18.0F;
    }
    at = setting(at, "Glow", gui::ui_id("studio.glow"), document.glow, 0.01F, 2, "", 0.0F, 2.0F, 0.0F, 8.0F,
                 "Preview bloom. Match the media server's.");
    const std::array<std::string_view, 3> backdrops{"Studio", "Night", "Void"};
    std::size_t backdrop = static_cast<std::size_t>(document.backdrop);
    const float chips_w = std::max(x + w - at, 0.0F);
    if (chips_w > 150.0F) {
        if (choice_chips(ctx, gui::ui_id("studio.backdrop"), gui::Rect{at, y, chips_w, row_height}, backdrops,
                         backdrop)) {
            document.backdrop = static_cast<Backdrop>(backdrop);
        }
        y += row_height + 22.0F;
    } else {
        y += row_height + 6.0F;
        if (choice_chips(ctx, gui::ui_id("studio.backdrop"), gui::Rect{x, y, w, row_height}, backdrops, backdrop)) {
            document.backdrop = static_cast<Backdrop>(backdrop);
        }
        y += row_height + 22.0F;
    }
    y = draw_world(ctx, document, x, y, w);
    y = draw_budget(ctx, document, inputs, x, y, w);
    return draw_parameters(ctx, inputs, x, y, w);
}

// The page's budget: GPU milliseconds a frame, with its reset arrow, and the
// button that scales the page's amounts to fill it (or, once a run found the
// page over, to fit it). What the last run came to reads under the row.
float PageView::draw_budget(gui::UiContext& ctx, Document& document, const PageInputs& inputs, float x, float y,
                            float w) {
    const gui::Theme& theme = ctx.theme();
    const BudgetView none;
    const BudgetView& view = inputs.budget != nullptr ? *inputs.budget : none;
    const float fx = x + label_width;
    const gui::Rect label_rect{x, y, label_width - 8.0F, row_height};
    ctx.label(label_rect, "Budget", theme.text_muted);
    ctx.tooltip(label_rect, "GPU time a frame the effect may take.");
    float budget = document.budget_ms;
    if (number(ctx, gui::ui_id("studio.budget"), gui::Rect{fx, y, 70.0F, row_height}, budget, 0.02F, 1, {}, 0.5F,
               16.0F)) {
        document.budget_ms = std::clamp(budget, min_budget_ms, max_budget_ms);
    }
    float at = fx + 74.0F;
    ctx.label(gui::Rect{at, y, ctx.font_width("ms") + 6.0F, row_height}, "ms", theme.text_muted);
    at += ctx.font_width("ms") + 12.0F;

    const std::string_view text = view.running ? "Stop" : view.over ? "Scale down" : "Maximize budget";
    const std::string_view tip = view.running ? "Stop. The page stays as it is."
                                 : view.over  ? "Fewer particles, to fit the budget."
                                              : "More particles, to fill the budget.";
    const gui::Rect button{at, y, ctx.font_width(text) + 46.0F, row_height};
    budget_button_ = button;
    if (editor::pill_button(ctx, gui::ui_id("studio.budget.fill"), button,
                            view.running ? gui::Icon::Stop : gui::Icon::Target, text, tip)) {
        budget_request_ = BudgetRequest{.stop = view.running, .allow_down = view.over, .options = {keep_look_}};
    }
    at = button.right() + 8.0F;

    // Whether particles shrink as there are more, so the look holds.
    const std::string_view look = "Keep the look";
    const gui::Rect chip{at, y + 2.0F, ctx.font_width(look) + 22.0F, row_height - 4.0F};
    if (chip.right() < x + w - editor::reset_button_width - 6.0F) {
        const gui::Interaction hit = ctx.grab(gui::ui_id("studio.budget.look"), chip);
        ctx.cursor_for(hit, gui::Cursor::Hand);
        ctx.rounded(chip, keep_look_ ? gui::fade(theme.accent, 0.22F) : (hit.hovered ? theme.widget_hover : theme.widget),
                    chip.height * 0.5F);
        ctx.label_centered(chip, look, keep_look_ ? theme.accent : theme.text_muted);
        ctx.tooltip(chip, keep_look_ ? "Smaller particles as there are more." : "Particles keep their size.");
        if (hit.clicked) {
            keep_look_ = !keep_look_;
        }
    }
    if (document.budget_ms != default_budget_ms) {
        const gui::Rect reset{x + w - editor::reset_button_width, y + (row_height - 18.0F) * 0.5F,
                              editor::reset_button_width, 18.0F};
        if (editor::reset_button(ctx, gui::ui_id("studio.budget.reset"), reset,
                                 "Back to " + format_number(default_budget_ms, 1) + " ms")) {
            document.budget_ms = default_budget_ms;
        }
    }
    y += row_height + row_gap;
    if (!view.status.empty()) {
        ctx.label(gui::Rect{fx, y, w - label_width, 18.0F}, view.status,
                  view.over ? theme.caution : theme.text_muted);
        y += 18.0F + row_gap;
    }
    return y + 18.0F;
}

// The world every layer plays in, as three rows: the ground, gravity and the
// wind. Each layer takes them by what it is (world_share), so they are set
// once for the page rather than added to every layer. A value away from its
// default carries a reset arrow.
float PageView::draw_world(gui::UiContext& ctx, Document& document, float x, float y, float w) {
    const gui::Theme& theme = ctx.theme();
    PageWorld& world = document.world;
    const PageWorld defaults;
    const float fx = x + label_width;
    const auto label = [&](std::string_view text, std::string_view tip) {
        const gui::Rect rect{x, y, label_width - 8.0F, row_height};
        ctx.label(rect, text, theme.text_muted);
        ctx.tooltip(rect, tip);
    };
    const auto unit = [&](float at, std::string_view text) {
        ctx.label(gui::Rect{at, y, ctx.font_width(text) + 6.0F, row_height}, text, theme.text_muted);
        return at + ctx.font_width(text) + 10.0F;
    };
    const auto reset = [&](std::string_view name, std::string_view tip) {
        const gui::Rect rect{x + w - editor::reset_button_width, y + (row_height - 18.0F) * 0.5F,
                             editor::reset_button_width, 18.0F};
        return editor::reset_button(ctx, part_id(0U, "studio.world.reset." + std::string{name}), rect, tip);
    };

    // The ground: on or off, and how high.
    label("Ground", "Every layer lands on it.");
    {
        const std::string_view state = world.ground ? "On" : "Off";
        const gui::Rect chip{fx, y + 2.0F, ctx.font_width(state) + 22.0F, row_height - 4.0F};
        const gui::Interaction hit = ctx.grab(gui::ui_id("studio.world.ground"), chip);
        ctx.cursor_for(hit, gui::Cursor::Hand);
        ctx.rounded(chip,
                    world.ground ? gui::fade(theme.accent, 0.22F) : (hit.hovered ? theme.widget_hover : theme.widget),
                    chip.height * 0.5F);
        ctx.label_centered(chip, state, world.ground ? theme.accent : theme.text_muted);
        ctx.tooltip(chip, world.ground ? "Click for no ground." : "Click for a ground.");
        if (hit.clicked) {
            world.ground = !world.ground;
        }
        if (world.ground) {
            const float at = chip.right() + 10.0F;
            float height = world.ground_height;
            if (number(ctx, gui::ui_id("studio.world.height"), gui::Rect{at, y, 70.0F, row_height}, height, 0.01F, 2,
                       {}, -2.0F, 2.0F)) {
                world.ground_height = std::clamp(height, -1000.0F, 1000.0F);
            }
            static_cast<void>(unit(at + 74.0F, "m high"));
        }
        if ((world.ground != defaults.ground || world.ground_height != defaults.ground_height) &&
            reset("ground", "Back to a ground at 0 m")) {
            world.ground = defaults.ground;
            world.ground_height = defaults.ground_height;
        }
    }
    y += row_height + row_gap;

    // Gravity, in g.
    label("Gravity", "Every layer falls by its share. 1 is Earth's.");
    {
        float gravity = world.gravity;
        if (number(ctx, gui::ui_id("studio.world.gravity"), gui::Rect{fx, y, 70.0F, row_height}, gravity, 0.01F, 2, {},
                   0.0F, 2.0F)) {
            world.gravity = std::clamp(gravity, 0.0F, 10.0F);
        }
        static_cast<void>(unit(fx + 74.0F, "g"));
        if (world.gravity != defaults.gravity && reset("gravity", "Back to Earth's: 1 g")) {
            world.gravity = defaults.gravity;
        }
    }
    y += row_height + row_gap;

    // The wind: how hard, and which way.
    label("Wind", "Pushes every layer by its share.");
    {
        float wind = world.wind;
        if (number(ctx, gui::ui_id("studio.world.wind"), gui::Rect{fx, y, 70.0F, row_height}, wind, 0.02F, 1, {}, 0.0F,
                   10.0F)) {
            world.wind = std::clamp(wind, 0.0F, 100.0F);
        }
        float at = unit(fx + 74.0F, "m/s^2");
        if (world.wind > 0.0F) {
            at = unit(at, "toward");
            float heading = world.wind_heading;
            if (number(ctx, gui::ui_id("studio.world.heading"), gui::Rect{at, y, 70.0F, row_height}, heading, 0.5F, 0,
                       {}, 0.0F, 360.0F)) {
                world.wind_heading = std::clamp(heading, -360.0F, 360.0F);
            }
            static_cast<void>(unit(at + 74.0F, "deg"));
        }
        if ((world.wind != defaults.wind || world.wind_heading != defaults.wind_heading) &&
            reset("wind", "Back to no wind")) {
            world.wind = defaults.wind;
            world.wind_heading = defaults.wind_heading;
        }
    }
    return y + row_height + 22.0F;
}

float PageView::draw_parameters(gui::UiContext& ctx, const PageInputs& inputs, float x, float y, float w) {
    if (inputs.compiled == nullptr || !inputs.compiled->effect || inputs.compiled->effect->parameters.empty()) {
        preview_values_.clear();
        return y;
    }
    const gui::Theme& theme = ctx.theme();
    const std::vector<ParameterDesc>& parameters = inputs.compiled->effect->parameters;
    // Values for parameters that are no longer published are forgotten.
    std::erase_if(preview_values_, [&](const auto& entry) {
        return std::none_of(parameters.begin(), parameters.end(),
                            [&](const ParameterDesc& p) { return p.name == entry.first; });
    });
    ctx.label(gui::Rect{x, y, w, 20.0F}, "Published for the media server", theme.text);
    ctx.tooltip(gui::Rect{x, y, w, 20.0F},
                "Operator controls. Values here only change the preview.");
    y += 24.0F;
    for (const ParameterDesc& parameter : parameters) {
        const gui::Rect label{x + 12.0F, y, label_width + 40.0F, row_height};
        ctx.label(label, parameter.name, theme.text_muted);
        ctx.tooltip(label, "From " + format_number(parameter.min, 3) + " to " + format_number(parameter.max, 3) +
                               ", " + format_number(parameter.default_value[0], 3) +
                               (parameter.type == ParameterType::Float ? std::string{} : std::string{"..."}) +
                               " unless an operator sets it");
        const auto found = preview_values_.find(parameter.name);
        ParameterValue value = found != preview_values_.end() ? found->second : parameter.default_value;
        const std::size_t components = parameter.type == ParameterType::Float ? 1U : parameter.type == ParameterType::Vec3 ? 3U : 4U;
        constexpr std::string_view channels[4] = {"R", "G", "B", "A"};
        const float field_x = x + label_width + 56.0F;
        const float box_w = components == 1U ? 150.0F : std::min(72.0F, (w - label_width - 90.0F) / 4.0F - 4.0F);
        const float speed = std::max((parameter.max - parameter.min) / 240.0F, 0.001F);
        bool changed = false;
        for (std::size_t i = 0U; i < components; ++i) {
            float v = value[i];
            if (number(ctx, part_id(i, "studio.parameter." + parameter.name),
                       gui::Rect{field_x + static_cast<float>(i) * (box_w + 4.0F), y, box_w, row_height}, v, speed, 2,
                       components == 1U ? std::string_view{} : channels[i], parameter.min, parameter.max)) {
                value[i] = std::clamp(v, parameter.min, parameter.max);
                changed = true;
            }
        }
        if (changed) {
            preview_values_[parameter.name] = value;
        }
        if (found != preview_values_.end()) {
            const gui::Rect reset{x + w - editor::reset_button_width, y + (row_height - 18.0F) * 0.5F,
                                  editor::reset_button_width, 18.0F};
            if (editor::reset_button(ctx, part_id(0U, "studio.parameter.reset." + parameter.name), reset,
                                     "Back to the effect's own value")) {
                preview_values_.erase(parameter.name);
            }
        }
        y += row_height + row_gap;
    }
    return y + 18.0F;
}

} // namespace ajfx::studio
