#include "page_view.hpp"

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
namespace {

namespace editor = mediaserver::editor;

constexpr float page_max_width = 720.0F;
constexpr float header_height = 34.0F;
constexpr float row_height = 26.0F;
constexpr float row_gap = 4.0F;
constexpr float label_width = 112.0F;
constexpr float hood_line = 17.0F;
constexpr float layer_header = 36.0F;
// A scope heading over a rigid layer's blocks, and the air after its group.
constexpr float scope_heading = 24.0F;
constexpr float scope_gap = 8.0F;
// How far an Object's own blocks sit in from it, so a page reads as each
// thing in the layer and what becomes of it.
constexpr float object_indent = 16.0F;
// How much of a hidden row's ink is left.
constexpr float hidden_dim = 0.45F;

gui::UiColor to_ui(const Value& v) {
    return gui::UiColor{v.x, v.y, v.z, v.w};
}

// Greys out a hidden row's controls: they stay editable under a veil of the
// page's own colour.
void grey_out(gui::UiContext& ctx, const gui::Rect& area) {
    ctx.rounded(area, gui::fade(ctx.theme().panel, 1.0F - hidden_dim), ctx.theme().radius);
}

// "Rigid physics" -> "rigid physics", to sit inside a sentence.
std::string lower_words(std::string_view text) {
    std::string out{text};
    if (!out.empty()) {
        out[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(out[0])));
    }
    return out;
}

std::string readable(std::string_view field) {
    std::string out{field};
    std::replace(out.begin(), out.end(), '_', ' ');
    if (!out.empty()) {
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    }
    return out;
}

std::string default_text(const ControlDef& control) {
    const Value& v = control.fallback;
    switch (control.type) {
    case ControlType::Number: return format_number(v.x, control.decimals) + std::string{control.unit.empty() ? "" : " "} + std::string{control.unit};
    case ControlType::Range: return format_number(v.x, control.decimals) + " to " + format_number(v.y, control.decimals);
    case ControlType::Vec3:
        return format_number(v.x, 2) + ", " + format_number(v.y, 2) + ", " + format_number(v.z, 2);
    case ControlType::Color: return "the starting colour";
    case ControlType::File: return "no file";
    case ControlType::Toggle: return v.x != 0.0F ? "on" : "off";
    case ControlType::Choice:
        for (const ChoiceDef& option : control.choices) {
            if (option.id == v.text) {
                return std::string{option.label};
            }
        }
        return v.text;
    }
    return {};
}

std::vector<std::string> hood_lines_for(const PageInputs& inputs, Uid layer, Uid origin, bool with_renderer) {
    std::vector<std::string> lines;
    if (!inputs.expert || inputs.compiled == nullptr) {
        return lines;
    }
    const LayerReport* report = inputs.compiled->layer(layer);
    if (report == nullptr) {
        return lines;
    }
    for (const ModuleLine& line : report->modules) {
        if (line.origin == origin) {
            lines.push_back(std::string{raw_stage(line.module)} + ": " + std::string{raw_kind(line.module)} + "   " +
                            describe_module(line.module));
        }
    }
    if (with_renderer && report->error.empty()) {
        lines.push_back("draw: " + describe_renderer(report->renderer));
        lines.push_back("budget: " + thousands(report->capacity) + " particles, " +
                        thousands((report->bytes + 1023U) / 1024U) + " KB of state");
    }
    return lines;
}

// Rows the Module block's fields take.
struct RawRows final {
    float rows{0.0F};
    template <class T>
    void operator()(std::string_view, const T& field) {
        if constexpr (std::is_same_v<T, std::vector<ColorKey>> || std::is_same_v<T, std::vector<CurveKey>>) {
            rows += static_cast<float>(field.size()) + 1.0F;
        } else {
            rows += 1.0F;
        }
    }
};

} // namespace

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
    menu_block_ = menu_layer_ = 0U;
    rename_.reset();
    scroll_.reset();
    last_layer_ = 0U;
    focus_ = 0U;
    layer_focus_ = 0U;
    restore_scroll_ = -1.0F;
    incoming_drop_.reset();
}

PageViewState PageView::view_state() const {
    PageViewState view;
    view.folded.assign(folded_.begin(), folded_.end());
    view.open.assign(open_.begin(), open_.end());
    view.details.assign(details_.begin(), details_.end());
    view.focus = focus_;
    view.layer = layer_focus_ != 0U ? layer_focus_ : last_layer_;
    view.scroll = scroll_.offset();
    return view;
}

void PageView::restore(const PageViewState& view) {
    folded_ = std::set<Uid>{view.folded.begin(), view.folded.end()};
    open_ = std::set<Uid>{view.open.begin(), view.open.end()};
    details_ = std::set<Uid>{view.details.begin(), view.details.end()};
    focus_ = view.focus;
    layer_focus_ = view.focus == 0U ? view.layer : 0U;
    last_layer_ = view.layer;
    // Applied on the next frame, before the page is laid out; the page clamps
    // it once it has measured itself.
    restore_scroll_ = view.scroll;
}

Uid PageView::working_layer(const Document& document) const {
    if (focus_ != 0U) {
        for (const Layer& layer : document.layers) {
            for (const Block& block : layer.blocks) {
                if (block.uid == focus_) {
                    return folded_.contains(layer.uid) ? 0U : layer.uid;
                }
            }
        }
    }
    if (layer_focus_ != 0U && document.layer(layer_focus_) != nullptr && !folded_.contains(layer_focus_)) {
        return layer_focus_;
    }
    return 0U;
}

void PageView::insert(Document& document, const Incoming& incoming, Uid layer_uid, std::size_t index) {
    if (incoming.kind == Incoming::Kind::Layer) {
        Layer layer;
        if (const std::optional<LayerKind> empty = empty_layer_kind(incoming.id)) {
            layer = make_layer(document, *empty, "Layer " + std::to_string(document.layers.size() + 1U));
        } else if (std::optional<Layer> preset = make_layer_preset(document, incoming.id)) {
            layer = std::move(*preset);
        } else {
            return;
        }
        last_layer_ = layer_focus_ = layer.uid;
        focus_ = 0U;
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
    open_.insert(uid);
    folded_.erase(layer->uid);
    focus_ = uid;
    layer_focus_ = 0U;
    last_layer_ = layer->uid;
}

void PageView::add(Document& document, const Incoming& incoming) {
    if (incoming.kind == Incoming::Kind::Layer) {
        insert(document, incoming, 0U, document.layers.size());
        return;
    }
    Uid target = working_layer(document);
    if (target == 0U && document.layer(last_layer_) != nullptr) {
        target = last_layer_;
    }
    if (target == 0U) {
        if (document.layers.empty()) {
            // A block needs a layer to live in.
            insert(document, Incoming{Incoming::Kind::Layer, {}, {}, {}}, 0U, 0U);
        }
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
                focus_ = b.uid;
                folded_.erase(layer->uid);
            }
        }
        return;
    }
    insert(document, incoming, target, layer != nullptr ? layer->blocks.size() : 0U);
}

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
        title = "Add a block to this " + lower_words(layer_kind_name(kind)) + " layer";
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
            picker_ids_.emplace_back(def.kind);
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
        title = "Add a layer";
        // The kind comes first, because it decides what the layer can be
        // built from. Each empty layer says what it is for.
        for (const LayerKind empty : {LayerKind::Particles, LayerKind::Rigid}) {
            entries.push_back(ListEntry{"Start empty", "Empty " + lower_words(layer_kind_name(empty)) + " layer",
                                        std::string{layer_kind_description(empty)}, true, {}});
            picker_ids_.emplace_back(empty_layer_id(empty));
        }
        for (const LayerPreset& preset : layer_presets()) {
            entries.push_back(ListEntry{std::string{layer_kind_name(preset.kind)}, std::string{preset.title},
                                        std::string{preset.description}, true, {}});
            picker_ids_.emplace_back(preset.id);
        }
    }
    picker_.open(x, y, std::move(title), std::move(entries));
}

void PageView::open_block_picker(const Document& document) {
    if (document.layers.empty()) {
        open_picker(PickerMode::Layers, 0U, 0U, page_.x + 40.0F, page_.y + 80.0F, document);
        return;
    }
    const Layer* layer = document.layer(last_layer_);
    if (layer == nullptr) {
        layer = &document.layers.back();
    }
    float y = page_.y + 80.0F;
    for (const LayerArea& area : layer_areas_) {
        if (area.layer == layer->uid) {
            y = area.end_y;
        }
    }
    open_picker(PickerMode::Blocks, layer->uid, layer->blocks.size(), page_.x + 60.0F, y, document);
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
    if (layer_focus_ != 0U && document.layer(layer_focus_) == nullptr) {
        layer_focus_ = 0U;
    }
    if (!ctx.typing() && !picker_.is_open() && ctx.key_pressed(gui::Key::Escape)) {
        // Nothing worked on: the preview's guides and paths step back.
        focus_ = 0U;
        layer_focus_ = 0U;
    }
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

    // Adding a layer: a ready-made part, or an empty one.
    const gui::Rect add{x, y + 4.0F, 150.0F, 30.0F};
    if (editor::pill_button(ctx, gui::ui_id("studio.add-layer"), add, gui::Icon::Plus, "Add layer",
                            "Add a layer: flames, smoke, sparks, debris... or an empty one")) {
        open_picker(PickerMode::Layers, 0U, 0U, add.x, add.bottom() + 6.0F, document);
    }
    if (document.layers.empty()) {
        ctx.label(gui::Rect{add.right() + 12.0F, add.y, width - add.width - 12.0F, add.height},
                  "Every effect is made of layers: flames, embers, smoke...", theme.text_muted);
    } else {
        ctx.label(gui::Rect{add.right() + 12.0F, add.y, width - add.width - 12.0F, add.height},
                  "Press / to add a block to the layer you are working on", gui::fade(theme.text_muted, 0.7F));
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
                 "Grows or shrinks the whole effect, keeping its look: distances, sizes, speeds and forces.");
    {
        // The loop: as long as the effect takes to play out, unless set by
        // hand. Dragging or typing a length sets it; Auto gives it back.
        const float label_w = ctx.font_width("Loop") + 8.0F;
        const gui::Rect label_rect{at, y, label_w, row_height};
        ctx.label(label_rect, "Loop", theme.text_muted);
        ctx.tooltip(label_rect, document.loop_auto
                                    ? "How long the preview plays before starting again: worked out from the effect, "
                                      "until its last particle has gone. Set a length to choose your own."
                                    : "How long the preview plays before starting again, as you set it.");
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
        ctx.tooltip(auto_chip, document.loop_auto ? "The loop follows the effect. Click to keep this length instead."
                                                  : "Loop as long as the effect takes to play out");
        if (chip.clicked) {
            if (document.loop_auto) {
                document.loop_seconds = std::clamp(inputs.natural_loop, 0.5F, 120.0F);
            }
            document.loop_auto = !document.loop_auto;
        }
        at = auto_chip.right() + 18.0F;
    }
    at = setting(at, "Glow", gui::ui_id("studio.glow"), document.glow, 0.01F, 2, "", 0.0F, 2.0F, 0.0F, 8.0F,
                 "Bloom in the preview: how far bright light spills. Set the same on the media server's project.");
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
    return draw_parameters(ctx, inputs, x, y, w);
}

float PageView::draw_layer(gui::UiContext& ctx, Document& document, std::size_t layer_index, const PageInputs& inputs,
                           float x, float y, float w) {
    const gui::Theme& theme = ctx.theme();
    Layer& layer = document.layers[layer_index];
    const Uid uid = layer.uid;
    const float top = y;
    layer_name_ = layer.name;
    const LayerReport* report = inputs.compiled != nullptr ? inputs.compiled->layer(uid) : nullptr;
    const bool folded = folded_.contains(uid);

    // The heading: a twirl, the name, its budget and its switches.
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
    // The layer being worked on, marked in the margin: the preview's guides
    // and paths are its.
    if (working_layer(document) == uid) {
        const gui::Rect mark{x - 14.0F, y + 9.0F, 3.0F, layer_header - 18.0F};
        ctx.rounded(mark, theme.accent, 1.5F);
        ctx.tooltip(gui::Rect{mark.x - 4.0F, mark.y, 10.0F, mark.height},
                    "The layer being worked on: the preview shows its guides and paths. Esc to clear.");
    }

    const float buttons = 3.0F * 28.0F;
    const gui::Rect menu{x + w - 26.0F, y + 6.0F, 24.0F, 24.0F};
    const gui::Rect solo{menu.x - 28.0F, menu.y, 24.0F, 24.0F};
    const gui::Rect eye{solo.x - 28.0F, menu.y, 24.0F, 24.0F};
    // A layer holding only objects that stand there has no particles to
    // budget: it says what it holds instead, since "0 of 64 max" would only
    // puzzle. An object the world moves is a body and does have a budget.
    const std::size_t standing = static_cast<std::size_t>(
        std::count_if(layer.blocks.begin(), layer.blocks.end(), [](const Block& b) {
            return b.enabled && b.kind == "object" && choice_of(b, "motion") != "loose";
        }));
    const bool births = std::any_of(layer.blocks.begin(), layer.blocks.end(), [](const Block& b) {
        return b.enabled && (b.kind == "stream" || b.kind == "burst" ||
                             (b.kind == "object" && choice_of(b, "motion") == "loose"));
    });
    std::string budget;
    if (report != nullptr && report->error.empty()) {
        if (standing > 0U && !births) {
            budget = std::to_string(standing) + (standing == 1U ? " object" : " objects");
        } else {
            budget = thousands(report->capacity) + " max";
            if (inputs.analysis != nullptr) {
                if (const auto peak = inputs.analysis->peak_alive.find(uid);
                    peak != inputs.analysis->peak_alive.end()) {
                    budget = thousands(peak->second) + " of " + budget;
                }
            }
        }
    }
    const float budget_w = budget.empty() ? 0.0F : ctx.font_width(budget) + 12.0F;
    // What moves this layer, said on the layer itself. It is the one thing
    // that decides which blocks the layer can hold, so it is read at a
    // glance rather than worked out from what is on the list.
    const std::string kind_text{layer_kind_name(layer.kind)};
    const float kind_w = ctx.font_width(kind_text) + 20.0F;
    const gui::Rect name{x + 20.0F, y, std::max(w - 20.0F - buttons - budget_w - kind_w - 16.0F, 40.0F), layer_header};
    if (rename_.editing(uid)) {
        if (std::optional<std::string> renamed = rename_.draw(ctx, gui::Rect{name.x, y + 5.0F, name.width, 26.0F})) {
            layer.name = std::move(*renamed);
        }
    } else {
        const gui::Interaction name_hit = ctx.grab(part_id(uid, "layer.name"), name);
        label_sized(ctx, name, layer.name.empty() ? "Layer" : layer.name,
                    gui::fade(theme.text, layer.enabled ? 1.0F : hidden_dim), 1.2F);
        if (name_hit.clicked) {
            last_layer_ = uid;
            // Working on the layer: its guides and paths, and no other's.
            layer_focus_ = uid;
            if (const std::optional<Document::BlockPlace> place = document.find_block(focus_);
                !place || place->layer->uid != uid) {
                focus_ = 0U;
            }
            if (clicks_.double_click(uid, ctx.time_seconds())) {
                rename_.begin(ctx, uid, layer.name);
            }
        }
        ctx.tooltip(name, "Click to work on this layer, double-click to rename");
    }
    {
        const gui::Rect chip{eye.x - budget_w - kind_w - 8.0F, y + (layer_header - 22.0F) * 0.5F, kind_w, 22.0F};
        const float dim = layer.enabled ? 1.0F : hidden_dim;
        ctx.rounded(chip, gui::fade(theme.widget, 0.7F * dim), theme.radius_small);
        ctx.label(gui::Rect{chip.x + 10.0F, chip.y, chip.width - 20.0F, chip.height}, kind_text,
                  gui::fade(theme.text_muted, dim));
        ctx.tooltip(chip, std::string{layer_kind_description(layer.kind)} +
                              " The palette and the block picker offer what this kind of layer can play, and "
                              "nothing else. Change it under Layer actions.");
    }
    if (!budget.empty()) {
        const gui::Rect budget_rect{eye.x - budget_w - 4.0F, y, budget_w, layer_header};
        ctx.label(budget_rect, budget, gui::fade(theme.text_muted, layer.enabled ? 1.0F : hidden_dim));
        ctx.tooltip(budget_rect,
                    standing > 0U && !births
                        ? "The objects this layer stands in its world. One that only stands there is not a "
                          "particle: it is there for the whole effect and takes no part of a budget."
                        : "Most particles alive at once in the preview loop, of the most this layer can ever "
                          "hold. The budget is worked out from its blocks; nobody sets it by hand.");
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

    if (folded) {
        layer_areas_.push_back(
            LayerArea{uid, gui::Rect{x, top, w, y - top}, layer.blocks.size(), y, layer.kind == LayerKind::Rigid});
        return y + 14.0F;
    }

    // How long each particle lives: every layer has one.
    {
        const gui::Rect row{x + 12.0F, y, w - 12.0F, row_height};
        const gui::Rect label{row.x, y, label_width, row_height};
        ctx.label(label, "Each lives", theme.text_muted);
        ctx.tooltip(label, "Seconds each particle lives, chosen between the two. Shorter is lighter and snappier.");
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
            ctx.tooltip(seed_label, "Deals the whole layer different random values: the same settings, another look. "
                                    "0 is the layer's own.");
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

    y = draw_layer_blocks(ctx, document, layer, inputs, x, y, w);

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
        last_layer_ = uid;
        open_picker(PickerMode::Blocks, uid, layer.blocks.size(), add.x + 20.0F, add.bottom() + 4.0F, document);
    }
    y += 32.0F;
    layer_areas_.push_back(LayerArea{uid, gui::Rect{x, top, w, y - top}, layer.blocks.size(), add.y - 3.0F,
                                     layer.kind == LayerKind::Rigid});

    // A rule before the next layer.
    ctx.panel(gui::Rect{x, y + 10.0F, w, 1.0F}, theme.divider);
    return y + 26.0F;
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
                 j < layer.blocks.size() && layer.blocks[j].kind != "object"; ++j) {
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
    const BlockDef* world_def = find_block_def(world_block_kind);
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
    ctx.tooltip(world_row, "What the pieces in this layer are made of, and how they behave when they touch. "
                           "Every piece in a rigid physics layer is one of these.");
    if (hit.clicked) {
        if (shown) {
            open_.erase(world_uid);
        } else {
            open_.insert(world_uid);
        }
        last_layer_ = layer.uid;
        layer_focus_ = layer.uid;
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

float PageView::block_body_height(const Block& block, const BlockDef& def, LayerKind kind) const {
    if (block.kind == raw_block_kind) {
        RawRows rows;
        if (block.raw) {
            visit_fields(*block.raw, rows);
        }
        return rows.rows * (row_height + row_gap) + 10.0F;
    }
    float rows = 0.0F;
    bool any_detail = false;
    for (const ControlDef& control : def.controls) {
        if (!control_visible(block, def, control, kind)) {
            continue;
        }
        if (control.level == Level::Detail) {
            any_detail = true;
            if (!details_.contains(block.uid)) {
                continue;
            }
        }
        rows += block.publication(control.id) != nullptr ? 2.0F : 1.0F;
    }
    return rows * (row_height + row_gap) + (any_detail ? 24.0F : 0.0F) + 10.0F;
}

float PageView::draw_block(gui::UiContext& ctx, Document& document, Layer& layer, std::size_t index,
                           const PageInputs& inputs, float x, float y, float w) {
    const gui::Theme& theme = ctx.theme();
    Block& block = layer.blocks[index];
    const Uid uid = block.uid;
    const BlockDef* def = find_block_def(block.kind);
    if (def == nullptr) {
        return y;
    }
    // Which heading this block reads under, and so which gaps may take it
    // when it is dragged. It is the block's kind that decides, not where it
    // happens to be drawn, so a block dragged out of a particle layer aims
    // at the right heading of the rigid layer it is let go over.
    const RigidScope scope = rigid_scope(block.kind);
    const bool open = open_.contains(uid);
    const std::vector<std::string> hood = hood_lines_for(inputs, layer.uid, uid, false);
    const float body = open ? block_body_height(block, *def, layer.kind) : 0.0F;
    const float hood_h = hood.empty() ? 0.0F : static_cast<float>(hood.size()) * hood_line + 8.0F;
    const gui::Rect card{x, y, w, header_height + body + hood_h};
    const bool being_dragged = dragging_ && drag_block_ == uid;
    const bool hovered = !being_dragged && card.inflated(28.0F, 0.0F).contains(ctx.pointer_x(), ctx.pointer_y()) &&
                         !ctx.pointer_blocked();
    const LayerReport* report = inputs.compiled != nullptr ? inputs.compiled->layer(layer.uid) : nullptr;
    const bool faulted = report != nullptr && report->error_block == uid;

    const float hover = ctx.ease(part_id(uid, "block.hover"), hovered ? 1.0F : 0.0F);
    if (being_dragged) {
        ctx.rounded_outline(card, gui::fade(theme.accent, 0.5F), theme.radius);
        slots_.push_back(Slot{layer.uid, index, card, scope, false});
        return card.bottom() + 6.0F;
    }
    ctx.rounded(card, gui::blend(theme.row, theme.widget, open ? 0.35F + 0.25F * hover : 0.6F * hover), theme.radius);
    if (faulted) {
        ctx.rounded_outline(card, gui::fade(theme.warning, 0.7F), theme.radius);
    }
    // Hidden, by its own eye or its layer's: greyed, but still editable.
    const bool shown = block.enabled && layer.enabled;
    const float dim = shown ? 1.0F : hidden_dim;

    // The grip, in the margin, while the pointer is near.
    const gui::Rect grip{x - 24.0F, y + 6.0F, 18.0F, 22.0F};
    const gui::Interaction grip_hit = ctx.grab(part_id(uid, "block.grip"), grip);
    ctx.cursor_for(grip_hit, gui::Cursor::Move);
    if (hover > 0.0F || grip_hit.held) {
        draw_grip(ctx, grip.center_x(), grip.center_y(), gui::fade(theme.text_muted, std::max(hover, 0.4F)));
    }
    if (grip_hit.pressed) {
        drag_block_ = uid;
        drag_scope_ = scope;
        drag_press_y_ = ctx.pointer_press_y();
        dragging_ = false;
        last_layer_ = layer.uid;
    }
    ctx.tooltip(grip, "Drag to move");

    // The header: what it is and what it is set to.
    const gui::Rect head{x, y, w, header_height};
    const gui::Rect menu{x + w - 30.0F, y + 5.0F, 24.0F, 24.0F};
    const gui::Rect eye{menu.x - 28.0F, menu.y, 24.0F, 24.0F};
    draw_category_icon(ctx, def->category, x + 18.0F, head.center_y(), gui::fade(theme.text_muted, dim));
    const std::string title = block_title(block);
    const float title_w = ctx.font_width(title);
    ctx.label(gui::Rect{x + 36.0F, y, title_w + 4.0F, header_height}, title, gui::fade(theme.text, dim));
    const float summary_x = x + 36.0F + title_w + 12.0F;
    const float summary_w = std::max(eye.x - summary_x - 6.0F, 0.0F);
    if (summary_w > 20.0F) {
        std::string summary = block_summary(block);
        if (block.detached) {
            // Where it came from, so it can be put back together.
            if (const std::optional<DetachedGroup> group = detached_group(document, uid)) {
                summary += ", from " + group->title;
            }
        }
        ctx.label(gui::Rect{summary_x, y, summary_w, header_height}, summary, gui::fade(theme.text_muted, dim));
    }
    if (block.kind == "color" && summary_w > 40.0F) {
        // The colour itself, beside its words.
        const gui::Rect swatch{eye.x - 26.0F, head.center_y() - 6.0F, 18.0F, 12.0F};
        ctx.rounded(swatch, gui::fade(to_ui(get(block, "color")), dim), theme.radius_small);
    }
    const gui::Interaction head_hit = ctx.grab(part_id(uid, "block.head"), gui::Rect{x, y, std::max(eye.x - x, 0.0F), header_height});
    ctx.cursor_for(head_hit, gui::Cursor::Hand);
    if (head_hit.clicked) {
        last_layer_ = layer.uid;
        if (open) {
            open_.erase(uid);
            if (focus_ == uid) {
                // Closed, it is no longer worked on; its layer still is.
                focus_ = 0U;
                layer_focus_ = layer.uid;
            }
        } else {
            open_.insert(uid);
            focus_ = uid;
            layer_focus_ = 0U;
        }
    }
    ctx.tooltip(gui::Rect{x + 6.0F, y, 26.0F, header_height}, def->description);
    if (editor::row_flag_toggle(ctx, part_id(uid, "block.eye"), eye, editor::RowFlag::Visible, block.enabled,
                                hovered)) {
        block.enabled = !block.enabled;
    }
    if (editor::icon_button(ctx, part_id(uid, "block.menu"), menu, gui::Icon::Details, "Block actions")) {
        menu_block_ = uid;
        last_layer_ = layer.uid;
        ctx.open_context_menu(part_id(0U, "block.context"), menu.x, menu.bottom() + 2.0F);
    }

    float cy = y + header_height;
    if (open) {
        if (block.kind == raw_block_kind) {
            cy = draw_raw_fields(ctx, block, x + 12.0F, cy, w - 24.0F);
        } else {
            cy = draw_controls(ctx, block, *def, layer.kind, x + 12.0F, cy, w - 24.0F);
        }
        cy = y + header_height + body;
    }
    if (!hood.empty()) {
        draw_hood(ctx, hood, x + 12.0F, cy, w - 24.0F);
    }
    if (!shown && card.height > header_height) {
        grey_out(ctx, gui::Rect{x, y + header_height, w, card.height - header_height});
    }
    slots_.push_back(Slot{layer.uid, index, card, scope, false});
    float bottom = card.bottom() + 6.0F;

    // The coach's notes about this block, under it.
    if (inputs.advice != nullptr) {
        for (std::size_t i = 0U; i < inputs.advice->size(); ++i) {
            const Advice& advice = (*inputs.advice)[i];
            if (advice.block == uid) {
                bottom = draw_advice(ctx, advice, i, x + 16.0F, bottom, w - 16.0F);
            }
        }
    }
    return bottom;
}

float PageView::draw_controls(gui::UiContext& ctx, Block& block, const BlockDef& def, LayerKind kind, float x,
                              float y, float w) {
    const gui::Theme& theme = ctx.theme();
    y += 4.0F;
    bool any_detail = false;
    const bool details = details_.contains(block.uid);
    for (const ControlDef& control : def.controls) {
        if (!control_visible(block, def, control, kind)) {
            continue;
        }
        if (control.level == Level::Detail) {
            any_detail = true;
            if (!details) {
                continue;
            }
        }
        const gui::Rect row{x, y, w, row_height};
        const gui::Rect label{x, y, label_width, row_height};
        ctx.label(label, control.label, theme.text_muted);
        if (!control.help.empty()) {
            ctx.tooltip(label, control.help);
        }
        const float link_w = control.publishable ? 28.0F : 0.0F;
        const gui::Rect field{x + label_width, y, w - label_width - editor::reset_button_width - 4.0F - link_w, row_height};
        if (draw_control(ctx, block, control, kind, field)) {
            focus_ = block.uid;
            layer_focus_ = 0U;
        }
        const Published* published = block.publication(control.id);
        if (control.publishable) {
            const gui::Rect link{field.right() + 2.0F, y + 1.0F, 24.0F, 24.0F};
            if (editor::icon_button(ctx, part_id(block.uid, control.id, 98U), link, gui::Icon::Link,
                                    published != nullptr ? "Stop publishing this to the media server"
                                                         : "Publish to the media server, for operators to set and animate",
                                    published != nullptr)) {
                if (published != nullptr) {
                    block.unpublish(control.id);
                } else {
                    block.publish(default_publication(block, control));
                }
                published = block.publication(control.id);
            }
        }
        if (value_of(block, control) != control.fallback) {
            const gui::Rect reset{row.right() - editor::reset_button_width, y + (row_height - 18.0F) * 0.5F,
                                  editor::reset_button_width, 18.0F};
            if (editor::reset_button(ctx, part_id(block.uid, control.id, 99U), reset,
                                     "Back to the default: " + default_text(control))) {
                block.clear(control.id);
            }
        }
        y += row_height + row_gap;
        if (published != nullptr) {
            y = draw_publication(ctx, block, control, *published, x, y, w);
        }
    }
    if (any_detail) {
        const gui::Rect more{x, y, 120.0F, 22.0F};
        const gui::Interaction hit = ctx.grab(part_id(block.uid, "block.more"), more);
        ctx.cursor_for(hit, gui::Cursor::Hand);
        editor::draw_twirl(ctx, more.x + 8.0F, more.center_y(), details, theme.text_muted);
        ctx.label(gui::Rect{more.x + 20.0F, more.y, more.width - 20.0F, more.height},
                  details ? "Fewer settings" : "More settings", hit.hovered ? theme.text : theme.text_muted);
        if (hit.clicked) {
            if (details) {
                details_.erase(block.uid);
            } else {
                details_.insert(block.uid);
            }
        }
        y += 24.0F;
    }
    return y;
}

bool PageView::draw_control(gui::UiContext& ctx, Block& block, const ControlDef& control, LayerKind kind,
                            const gui::Rect& field) {
    const gui::Theme& theme = ctx.theme();
    Value value = value_of(block, control);
    const auto commit = [&](Value changed) {
        block.set(control.id, sanitize(control, std::move(changed)));
        return true;
    };
    const float speed = std::max((control.max - control.min) / 240.0F, 0.001F);
    const auto unit_label = [&](float at) {
        if (!control.unit.empty()) {
            ctx.label(gui::Rect{at + 6.0F, field.y, 80.0F, field.height}, control.unit, theme.text_muted);
        }
    };
    switch (control.type) {
    case ControlType::Number: {
        const gui::Rect box{field.x, field.y, std::min(150.0F, field.width - 60.0F), field.height};
        float v = value.x;
        const bool changed = number(ctx, part_id(block.uid, control.id), box, v, speed, control.decimals, {},
                                    control.min, control.max);
        unit_label(box.right());
        return changed && commit(studio::number(v));
    }
    case ControlType::Range: {
        if (fits(control.one_number, kind)) {
            // One value here, not a spread: this layer's runtime applies it
            // to everything alike, so two ends would promise a variation it
            // does not deliver. Both ends move together.
            const gui::Rect box{field.x, field.y, std::min(150.0F, field.width - 60.0F), field.height};
            float v = value.y;
            const bool changed = number(ctx, part_id(block.uid, control.id), box, v, speed, control.decimals, {},
                                        control.min, control.max);
            unit_label(box.right());
            return changed && commit(range(v, v));
        }
        const float box_w = std::min(90.0F, (field.width - 70.0F) * 0.5F);
        float low = value.x;
        float high = value.y;
        bool changed = number(ctx, part_id(block.uid, control.id, 0U), gui::Rect{field.x, field.y, box_w, field.height},
                              low, speed, control.decimals, {}, control.min, control.max);
        ctx.label(gui::Rect{field.x + box_w + 2.0F, field.y, 18.0F, field.height}, "to", theme.text_muted);
        if (changed) {
            high = std::max(high, low);
        }
        if (number(ctx, part_id(block.uid, control.id, 1U),
                   gui::Rect{field.x + box_w + 22.0F, field.y, box_w, field.height}, high, speed, control.decimals, {},
                   control.min, control.max)) {
            low = std::min(low, high);
            changed = true;
        }
        unit_label(field.x + box_w * 2.0F + 22.0F);
        return changed && commit(range(low, high));
    }
    case ControlType::Vec3: {
        const float box_w = std::min(84.0F, (field.width - 40.0F) / 3.0F);
        float parts[3] = {value.x, value.y, value.z};
        bool changed = false;
        constexpr std::string_view axes[3] = {"X", "Y", "Z"};
        // A drag moves the smallest digit the control shows: hundredths of a
        // metre, or whole degrees.
        const float step = control.decimals >= 2 ? 0.01F : (control.decimals == 1 ? 0.1F : 1.0F);
        for (std::uint32_t i = 0U; i < 3U; ++i) {
            changed = number(ctx, part_id(block.uid, control.id, i),
                             gui::Rect{field.x + static_cast<float>(i) * (box_w + 4.0F), field.y, box_w, field.height},
                             parts[i], step, control.decimals, axes[i]) ||
                      changed;
        }
        unit_label(field.x + box_w * 3.0F + 8.0F);
        return changed && commit(vec3(parts[0], parts[1], parts[2]));
    }
    case ControlType::Color: {
        gui::UiColor ui = to_ui(value);
        if (ctx.color_well(part_id(block.uid, control.id), gui::Rect{field.x, field.y + 1.0F, 64.0F, field.height - 2.0F}, ui)) {
            return commit(color(ui.r, ui.g, ui.b, value.w));
        }
        return false;
    }
    case ControlType::Choice: {
        // Only the options this kind of layer can answer for.
        const std::vector<ChoiceDef> offered = choices_in(control, kind);
        std::vector<std::string_view> labels;
        std::size_t selected = 0U;
        for (std::size_t i = 0U; i < offered.size(); ++i) {
            labels.push_back(offered[i].label);
            if (offered[i].id == value.text) {
                selected = i;
            }
        }
        if (!offered.empty() && choice_chips(ctx, part_id(block.uid, control.id), field, labels, selected)) {
            return commit(choice(std::string{offered[selected].id}));
        }
        return false;
    }
    case ControlType::Toggle: {
        bool on = value.x != 0.0F;
        if (switch_toggle(ctx, part_id(block.uid, control.id), field, on)) {
            return commit(toggle(on));
        }
        return false;
    }
    case ControlType::File: return draw_file(ctx, block, control, field);
    }
    return false;
}

// A file: its name on a button that opens the system's file picker, or a
// file dropped on it from the desktop.
bool PageView::draw_file(gui::UiContext& ctx, Block& block, const ControlDef& control, const gui::Rect& field) {
    const gui::Theme& theme = ctx.theme();
    const std::string path = value_of(block, control).text;
    const bool images = control.files == FileKind::Image;
    const gui::Rect button{field.x, field.y, std::min(field.width, 240.0F), field.height};
    const gui::Interaction hit = ctx.grab(part_id(block.uid, control.id), button);
    ctx.cursor_for(hit, gui::Cursor::Hand);
    const bool dropping = ctx.file_drag_over() && button.contains(ctx.file_drag_x(), ctx.file_drag_y());
    ctx.rounded(button, dropping ? gui::fade(theme.accent, 0.25F) : hit.hovered ? theme.widget_hover : theme.widget,
                theme.radius_small);
    gui::draw_icon(ctx.draw_list(), gui::Icon::Import, button.x + 14.0F, button.center_y(),
                   path.empty() ? theme.accent : theme.text_muted, 0.8F);
    ctx.label(gui::Rect{button.x + 28.0F, button.y, button.width - 34.0F, button.height},
              path.empty() ? (images ? std::string{"Choose a picture..."} : std::string{"Choose a model..."})
                           : file_label(path),
              path.empty() ? theme.text_muted : theme.text);
    ctx.tooltip(button, path.empty() ? (images ? "Choose a PNG, JPEG, TGA or BMP, or drop one here"
                                               : "Choose an OBJ, glTF or GLB, or drop one here")
                                     : path + ". Click to choose another, or drop one here.");
    std::string chosen;
    if (hit.clicked) {
        const char* image_filters[] = {"*.png", "*.jpg", "*.jpeg", "*.tga", "*.bmp"};
        const char* model_filters[] = {"*.obj", "*.gltf", "*.glb"};
        const std::string start = path.empty() ? std::string{} : path;
        if (const char* picked = images ? tinyfd_openFileDialog("Choose a picture", start.c_str(), 5, image_filters,
                                                                "Pictures", 0)
                                        : tinyfd_openFileDialog("Choose a model", start.c_str(), 3, model_filters,
                                                                "3D models", 0)) {
            chosen = picked;
        }
    }
    for (const std::string& dropped : ctx.dropped_files()) {
        if (button.contains(ctx.drop_x(), ctx.drop_y())) {
            chosen = dropped;
        }
    }
    if (chosen.empty() || chosen == path) {
        return false;
    }
    block.set(control.id, choice(std::filesystem::path{chosen}.generic_string()));
    return true;
}

Published PageView::default_publication(const Block& block, const ControlDef& control) const {
    Published published;
    published.control = std::string{control.id};
    published.name = parameter_name(layer_name_ + "_" + std::string{control.id});
    if (published.name.empty()) {
        published.name = parameter_name(control.id);
    }
    // The control's usual range, stretched to hold its value now. A colour
    // runs to its brightness, with room above.
    const Value value = value_of(block, control);
    if (control.type == ControlType::Color) {
        published.low = 0.0F;
        published.high = std::ceil(std::max(1.0F, get(block, "brightness").y) * 1.5F);
    } else {
        const float high_value = control.type == ControlType::Range ? value.y : value.x;
        published.low = std::min(control.min, value.x);
        published.high = std::max(control.max, high_value);
    }
    return published;
}

float PageView::draw_publication(gui::UiContext& ctx, Block& block, const ControlDef& control,
                                 const Published& published, float x, float y, float w) {
    const gui::Theme& theme = ctx.theme();
    const float indent = x + label_width;
    ctx.label(gui::Rect{x + 12.0F, y, label_width - 12.0F, row_height}, "Published as", gui::fade(theme.accent, 0.85F));
    std::string name = published.name;
    const gui::UiId name_id = part_id(block.uid, control.id, 97U);
    const float name_w = std::min(170.0F, std::max(w - label_width - 190.0F, 80.0F));
    if (ctx.text_field(name_id, gui::Rect{indent, y, name_w, row_height}, name)) {
        Published renamed = published;
        renamed.name = typed_parameter_name(name);
        if (!renamed.name.empty()) {
            block.publish(renamed);
        }
    }
    ctx.tooltip(gui::Rect{indent, y, name_w, row_height},
                control.type == ControlType::Range
                    ? "The media server shows two parameters, this name with _min and _max."
                    : "The name operators see in the media server's Inspector. Give two controls one name to drive "
                      "both with one value.");
    float at = indent + name_w + 8.0F;
    ctx.label(gui::Rect{at, y, 40.0F, row_height}, "from", theme.text_muted);
    at += ctx.font_width("from") + 6.0F;
    const float speed = std::max((control.max - control.min) / 240.0F, 0.001F);
    float low = published.low;
    float high = published.high;
    if (number(ctx, part_id(block.uid, control.id, 96U), gui::Rect{at, y, 64.0F, row_height}, low, speed,
               control.decimals)) {
        Published changed = published;
        changed.low = std::min(low, published.high);
        block.publish(changed);
    }
    at += 70.0F;
    ctx.label(gui::Rect{at, y, 20.0F, row_height}, "to", theme.text_muted);
    at += ctx.font_width("to") + 6.0F;
    if (number(ctx, part_id(block.uid, control.id, 95U), gui::Rect{at, y, 64.0F, row_height}, high, speed,
               control.decimals)) {
        Published changed = *block.publication(control.id);
        changed.high = std::max(high, changed.low);
        block.publish(changed);
    }
    ctx.tooltip(gui::Rect{indent + name_w, y, 160.0F, row_height},
                "The values operators may set. The budget is worked out for the whole range, so none of them can "
                "overrun it.");
    return y + row_height + row_gap;
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
                "What operators can set and animate on each placement. Try values here; they change only the "
                "preview, not the effect.");
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

float PageView::draw_raw_fields(gui::UiContext& ctx, Block& block, float x, float y, float w) {
    if (!block.raw) {
        return y;
    }
    const gui::Theme& theme = ctx.theme();
    y += 4.0F;
    std::uint32_t part = 0U;
    const Uid uid = block.uid;
    PageView& self = *this;
    const auto label_row = [&](std::string_view name) {
        ctx.label(gui::Rect{x, y, label_width, row_height}, readable(name), theme.text_muted);
        return gui::Rect{x + label_width, y, w - label_width, row_height};
    };
    const auto field_id = [&](std::string_view name) { return part_id(uid, name, part++); };
    const auto vec_fields = [&](std::string_view name, const gui::Rect& field, float* values, std::size_t count) {
        const float box_w = std::min(76.0F, (field.width - 8.0F) / static_cast<float>(count) - 4.0F);
        constexpr std::string_view axes[4] = {"X", "Y", "Z", "A"};
        constexpr std::string_view channels[4] = {"R", "G", "B", "A"};
        bool changed = false;
        for (std::size_t i = 0U; i < count; ++i) {
            changed = self.number(ctx, field_id(name), gui::Rect{field.x + static_cast<float>(i) * (box_w + 4.0F), field.y, box_w, field.height},
                                  values[i], 0.01F, 3, count == 4U ? channels[i] : axes[i]) ||
                      changed;
        }
        return changed;
    };
    const auto keys_editor = [&](std::string_view name, auto& keys) {
        using Key = typename std::decay_t<decltype(keys)>::value_type;
        std::optional<std::size_t> remove;
        bool changed = false;
        for (std::size_t k = 0U; k < keys.size(); ++k) {
            const gui::Rect field = label_row(k == 0U ? name : std::string_view{});
            float t = keys[k].t;
            changed = self.number(ctx, field_id(name), gui::Rect{field.x, field.y, 60.0F, field.height}, t, 0.005F, 3, "t") ||
                      changed;
            keys[k].t = std::clamp(t, 0.0F, 1.0F);
            const gui::Rect rest{field.x + 64.0F, field.y, field.width - 64.0F - 28.0F, field.height};
            if constexpr (std::is_same_v<Key, ColorKey>) {
                float c[4] = {keys[k].value.r, keys[k].value.g, keys[k].value.b, keys[k].value.a};
                if (vec_fields(name, rest, c, 4U)) {
                    keys[k].value = Color{c[0], c[1], c[2], c[3]};
                    changed = true;
                }
            } else {
                changed = self.number(ctx, field_id(name), gui::Rect{rest.x, rest.y, 90.0F, rest.height}, keys[k].value,
                                      0.01F, 3) ||
                          changed;
            }
            if (editor::icon_button(ctx, field_id(name), gui::Rect{field.right() - 24.0F, field.y + 1.0F, 24.0F, 24.0F},
                                    gui::Icon::Close, "Remove this key")) {
                remove = k;
            }
            y += row_height + row_gap;
        }
        const gui::Rect add = label_row(keys.empty() ? name : std::string_view{});
        if (editor::pill_button(ctx, field_id(name), gui::Rect{add.x, add.y, 100.0F, add.height}, gui::Icon::Plus,
                                "Add key", "Add a key at the end of life")) {
            Key key = keys.empty() ? Key{} : keys.back();
            key.t = 1.0F;
            keys.push_back(key);
            changed = true;
        }
        y += row_height + row_gap;
        if (remove) {
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(*remove));
            changed = true;
        }
        if (changed) {
            std::stable_sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.t < b.t; });
        }
    };
    visit_fields(*block.raw, [&](std::string_view name, auto& field) {
        using T = std::decay_t<decltype(field)>;
        if constexpr (std::is_same_v<T, std::vector<ColorKey>> || std::is_same_v<T, std::vector<CurveKey>>) {
            keys_editor(name, field);
            return;
        } else {
            const gui::Rect row = label_row(name);
            if constexpr (std::is_same_v<T, float>) {
                self.number(ctx, field_id(name), gui::Rect{row.x, row.y, 150.0F, row.height}, field, 0.01F, 3);
            } else if constexpr (std::is_same_v<T, std::uint32_t>) {
                float v = static_cast<float>(field);
                if (self.number(ctx, field_id(name), gui::Rect{row.x, row.y, 150.0F, row.height}, v, 0.25F, 0)) {
                    field = static_cast<std::uint32_t>(std::clamp(v, 0.0F, 4.0e9F));
                }
            } else if constexpr (std::is_same_v<T, bool>) {
                static_cast<void>(switch_toggle(ctx, field_id(name), row, field));
            } else if constexpr (std::is_same_v<T, Vec3>) {
                float v[3] = {field.x, field.y, field.z};
                if (vec_fields(name, row, v, 3U)) {
                    field = Vec3{v[0], v[1], v[2]};
                }
            } else if constexpr (std::is_same_v<T, std::string>) {
                static_cast<void>(ctx.text_field(field_id(name), gui::Rect{row.x, row.y, std::min(row.width, 220.0F), row.height}, field));
            } else if constexpr (NamedEnum<T>) {
                std::vector<std::string_view> labels(std::begin(EnumNames<T>::names), std::end(EnumNames<T>::names));
                std::size_t selected = static_cast<std::size_t>(field);
                if (choice_chips(ctx, field_id(name), row, labels, selected)) {
                    field = static_cast<T>(selected);
                }
            } else if constexpr (std::is_same_v<T, FloatInput> || std::is_same_v<T, Vec3Input> ||
                                 std::is_same_v<T, ColorInput>) {
                if (field.bound()) {
                    ctx.label(gui::Rect{row.x, row.y, row.width - 90.0F, row.height}, "Bound to '" + field.parameter + "'",
                              theme.text_muted);
                    if (editor::pill_button(ctx, field_id(name), gui::Rect{row.right() - 84.0F, row.y, 84.0F, row.height},
                                            std::nullopt, "Unbind", "Use a fixed value instead")) {
                        field.parameter.clear();
                    }
                } else if constexpr (std::is_same_v<T, FloatInput>) {
                    self.number(ctx, field_id(name), gui::Rect{row.x, row.y, 150.0F, row.height}, field.value, 0.01F, 3);
                } else if constexpr (std::is_same_v<T, Vec3Input>) {
                    float v[3] = {field.value.x, field.value.y, field.value.z};
                    if (vec_fields(name, row, v, 3U)) {
                        field.value = Vec3{v[0], v[1], v[2]};
                    }
                } else {
                    float v[4] = {field.value.r, field.value.g, field.value.b, field.value.a};
                    if (vec_fields(name, row, v, 4U)) {
                        field.value = Color{v[0], v[1], v[2], v[3]};
                    }
                }
            }
            y += row_height + row_gap;
        }
    });
    return y;
}

float PageView::draw_hood(gui::UiContext& ctx, const std::vector<std::string>& lines, float x, float y, float w) {
    if (lines.empty()) {
        return y;
    }
    const gui::Theme& theme = ctx.theme();
    const gui::Rect box{x, y + 2.0F, w, static_cast<float>(lines.size()) * hood_line + 4.0F};
    ctx.rounded(box, theme.track, theme.radius_small);
    float ly = box.y + 2.0F;
    for (const std::string& line : lines) {
        const gui::Rect rect{box.x + 8.0F, ly, box.width - 16.0F, hood_line};
        if (const std::optional<float> size = ctx.text_size_at_most(0.86F)) {
            ctx.label(rect, line, gui::fade(theme.keyframe, 0.85F), *size);
        } else {
            ctx.label(rect, line, gui::fade(theme.keyframe, 0.85F));
        }
        ctx.tooltip(rect, line);
        ly += hood_line;
    }
    return box.bottom() + 6.0F;
}

float PageView::draw_advice(gui::UiContext& ctx, const Advice& advice, std::size_t index, float x, float y, float w) {
    const gui::Theme& theme = ctx.theme();
    const gui::UiColor tone = advice.level == AdviceLevel::Error ? theme.warning : theme.caution;
    const float fix_w = advice.fix_label.empty() ? 0.0F : ctx.font_width(advice.fix_label) + 34.0F;
    const std::vector<std::string> lines = wrap_text(ctx, advice.message, w - 40.0F - fix_w);
    const float height = std::max(34.0F, static_cast<float>(lines.size()) * 18.0F + 14.0F);
    const gui::Rect card{x, y, w, height};
    ctx.rounded(card, gui::fade(tone, 0.07F), theme.radius);
    ctx.rounded_outline(card, gui::fade(tone, 0.3F), theme.radius);
    ctx.rounded(gui::Rect{card.x + 12.0F, card.y + 13.0F, 7.0F, 7.0F}, tone, 3.5F);
    float ly = card.y + 7.0F;
    for (const std::string& line : lines) {
        ctx.label(gui::Rect{card.x + 28.0F, ly, w - 40.0F - fix_w, 18.0F}, line, theme.text);
        ly += 18.0F;
    }
    if (!advice.fix_label.empty() && advice.fix) {
        const gui::Rect fix{card.right() - fix_w - 6.0F, card.center_y() - 13.0F, fix_w, 26.0F};
        if (editor::pill_button(ctx, part_id(index, "advice.fix", advice.layer ^ advice.block), fix, std::nullopt,
                                advice.fix_label, "Apply this fix (it can be undone)")) {
            deferred_.push_back(advice.fix);
        }
    }
    return card.bottom() + 6.0F;
}

// Where a block of `scope` would land in `area` with the pointer at `py`:
// the index it takes in the layer's list, and where to draw the line.
//
// The gaps are walked in the order they were drawn, so the first whose
// middle is below the pointer is the gap the pointer is in. A grouped
// layer draws its blocks out of list order, so only the gaps of the
// block's own heading can take it -- its kind decides where it is drawn,
// and a line at any other heading would promise a place it cannot land.
// With the pointer past them all, the heading's own foot takes it. Nothing
// matching leaves the layer's end, which is what an ungrouped layer always
// uses.
std::optional<std::pair<std::size_t, float>> PageView::drop_gap(const LayerArea& area, RigidScope scope,
                                                                float py) const {
    std::optional<std::pair<std::size_t, float>> foot;
    for (const Slot& slot : slots_) {
        if (slot.layer != area.layer) {
            continue;
        }
        if (area.grouped && slot.scope != scope) {
            continue;
        }
        if (py < slot.rect.center_y()) {
            return std::make_pair(slot.index, slot.rect.y - 3.0F);
        }
        if (area.grouped && slot.tail) {
            foot = std::make_pair(slot.index, slot.rect.y - 3.0F);
        }
    }
    return foot;
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
            if (id == raw_block_kind) {
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
        options.push_back("Delete");
        if (const std::optional<std::size_t> chosen = ctx.context_menu(id, options)) {
            const std::string_view action = options[*chosen];
            const Uid uid = menu_block_;
            if (action == "Duplicate") {
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
                    focus_ = *restored;
                    layer_focus_ = 0U;
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
        // The kind it could be instead, as an action when nothing is in the
        // way and as the reason when something is. A block the other kind
        // does not have is never dropped behind an artist's back: it is
        // named, and the layer stays as it is until they deal with it.
        const Layer* menu_target = document.layer(menu_layer_);
        const LayerKind other = menu_target != nullptr && menu_target->kind == LayerKind::Particles
                                    ? LayerKind::Rigid
                                    : LayerKind::Particles;
        std::string switch_entry;
        bool can_switch = false;
        if (menu_target != nullptr) {
            const std::vector<std::string> blocking = blocks_against_kind(*menu_target, other);
            can_switch = blocking.empty();
            if (can_switch) {
                switch_entry = "Make it a " + lower_words(layer_kind_name(other)) + " layer";
            } else {
                switch_entry = "Cannot be " + lower_words(layer_kind_name(other)) + ": ";
                for (std::size_t i = 0U; i < blocking.size(); ++i) {
                    switch_entry += (i == 0U ? "" : ", ") + blocking[i];
                }
            }
        }
        const std::array<std::string_view, 7> options{"Rename",           "Duplicate",   "Move up",
                                                      "Move down",        switch_entry,  "Fold all others",
                                                      "Delete layer"};
        if (const std::optional<std::size_t> chosen = ctx.context_menu(id, options)) {
            const Uid uid = menu_layer_;
            const auto found = std::find_if(document.layers.begin(), document.layers.end(),
                                            [&](const Layer& l) { return l.uid == uid; });
            if (found != document.layers.end()) {
                const std::size_t at = static_cast<std::size_t>(found - document.layers.begin());
                switch (*chosen) {
                case 0U: rename_.begin(ctx, uid, found->name); break;
                case 1U: {
                    Layer copy = copy_layer(document, *found);
                    copy.name += " copy";
                    document.layers.insert(document.layers.begin() + static_cast<std::ptrdiff_t>(at + 1U), std::move(copy));
                    break;
                }
                case 2U:
                    if (at > 0U) {
                        move_layer(document, uid, at - 1U);
                    }
                    break;
                case 3U: move_layer(document, uid, at + 1U); break;
                case 4U:
                    // Nothing happens when a block is in the way: the entry
                    // read as the reason, not as something to do.
                    if (can_switch) {
                        set_layer_kind(document, *found, other);
                        focus_ = 0U;
                        layer_focus_ = uid;
                        last_layer_ = uid;
                    }
                    break;
                case 5U:
                    for (const Layer& fold : document.layers) {
                        if (fold.uid != uid) {
                            folded_.insert(fold.uid);
                        }
                    }
                    folded_.erase(uid);
                    break;
                case 6U: remove_layer(document, uid); break;
                default: break;
                }
            }
            menu_layer_ = 0U;
        } else if (!ctx.menu_open(id)) {
            menu_layer_ = 0U;
        }
    }
}

} // namespace ajfx::studio
