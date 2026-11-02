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

float PageView::block_body_height(const Block& block, const BlockDef& def, LayerKind kind) const {
    if (block.kind == BlockKind::Module) {
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
        selection_.last_layer = layer.uid;
    }
    ctx.tooltip(grip, "Drag to move");

    // The header: what it is and what it is set to.
    const gui::Rect head{x, y, w, header_height};
    const gui::Rect menu{x + w - 30.0F, y + 5.0F, 24.0F, 24.0F};
    const gui::Rect eye{menu.x - 28.0F, menu.y, 24.0F, 24.0F};
    // An object in a solid layer can be blown up from its own heading: one
    // click, which cuts it, sets off an explosion at it and adds the fire,
    // sparks, smoke and dust. The room is kept whether the chip shows or
    // not, so the summary does not move under the pointer.
    const bool explodable = layer.kind == LayerKind::Rigid && block.kind == BlockKind::Object;
    const std::string explode_text = "Explode";
    const float explode_w = explodable ? ctx.font_width(explode_text) + 20.0F : 0.0F;
    const float actions_x = explodable ? eye.x - explode_w - 6.0F : eye.x;
    draw_category_icon(ctx, def->category, x + 18.0F, head.center_y(), gui::fade(theme.text_muted, dim));
    const std::string title = block_title(block);
    const float title_w = ctx.font_width(title);
    ctx.label(gui::Rect{x + 36.0F, y, title_w + 4.0F, header_height}, title, gui::fade(theme.text, dim));
    const float summary_x = x + 36.0F + title_w + 12.0F;
    const float summary_w = std::max(actions_x - summary_x - 6.0F, 0.0F);
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
    if (block.kind == BlockKind::Color && summary_w > 40.0F) {
        // The colour itself, beside its words.
        const gui::Rect swatch{eye.x - 26.0F, head.center_y() - 6.0F, 18.0F, 12.0F};
        ctx.rounded(swatch, gui::fade(to_ui(get(block, "color")), dim), theme.radius_small);
    }
    const gui::Interaction head_hit =
        ctx.grab(part_id(uid, "block.head"), gui::Rect{x, y, std::max(actions_x - x, 0.0F), header_height});
    ctx.cursor_for(head_hit, gui::Cursor::Hand);
    if (head_hit.clicked) {
        selection_.last_layer = layer.uid;
        if (open) {
            open_.erase(uid);
            if (selection_.block == uid) {
                // Closed, it is no longer worked on; its layer still is.
                selection_.block = 0U;
                selection_.layer = layer.uid;
            }
        } else {
            open_.insert(uid);
            selection_.block = uid;
            selection_.layer = 0U;
        }
    }
    ctx.tooltip(gui::Rect{x + 6.0F, y, 26.0F, header_height}, def->description);
    if (editor::row_flag_toggle(ctx, part_id(uid, "block.eye"), eye, editor::RowFlag::Visible, block.enabled,
                                hovered)) {
        block.enabled = !block.enabled;
    }
    if (explodable && (hovered || open)) {
        const gui::Rect chip{actions_x, y + (header_height - 22.0F) * 0.5F, explode_w, 22.0F};
        const gui::Interaction hit = ctx.grab(part_id(uid, "block.explode"), chip);
        ctx.cursor_for(hit, gui::Cursor::Hand);
        ctx.rounded(chip,
                    gui::blend(gui::fade(theme.widget, 0.7F * dim), theme.widget_hover, hit.hovered ? 0.35F : 0.0F),
                    theme.radius_small);
        ctx.label(gui::Rect{chip.x + 10.0F, chip.y, chip.width - 20.0F, chip.height}, explode_text,
                  gui::fade(theme.text_muted, dim));
        ctx.tooltip(chip, "Blow it up: cut it, with fire, sparks, smoke and dust");
        block_chips_.push_back(BlockChip{uid, "explode", chip});
        if (hit.clicked) {
            explode(layer.uid, uid);
        }
    }
    if (editor::icon_button(ctx, part_id(uid, "block.menu"), menu, gui::Icon::Details, "Block actions")) {
        menu_block_ = uid;
        selection_.last_layer = layer.uid;
        ctx.open_context_menu(part_id(0U, "block.context"), menu.x, menu.bottom() + 2.0F);
    }

    float cy = y + header_height;
    if (open) {
        if (block.kind == BlockKind::Module) {
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
            selection_.block = block.uid;
            selection_.layer = 0U;
            if (block.kind == BlockKind::Object && control.id == "material") {
                material_changed(block.uid);
            }
        }
        const Published* published = block.publication(control.id);
        if (control.publishable) {
            const gui::Rect link{field.right() + 2.0F, y + 1.0F, 24.0F, 24.0F};
            if (editor::icon_button(ctx, part_id(block.uid, control.id, 98U), link, gui::Icon::Link,
                                    published != nullptr ? "Stop publishing" : "Publish to the media server",
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
                if (block.kind == BlockKind::Object && control.id == "material") {
                    material_changed(block.uid);
                }
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
    case ControlType::SolidLayer: {
        // Every solid layer, then each by name. One named that is gone
        // shows none picked by name; the coach says so.
        std::vector<std::string_view> labels{"All solids"};
        std::size_t selected = 0U;
        const auto held = static_cast<Uid>(std::max(value.x, 0.0F));
        for (std::size_t i = 0U; i < solid_layers_.size(); ++i) {
            labels.push_back(solid_layers_[i].second);
            if (solid_layers_[i].first == held) {
                selected = i + 1U;
            }
        }
        if (choice_chips(ctx, part_id(block.uid, control.id), field, labels, selected)) {
            return commit(studio::number(selected == 0U ? 0.0F : static_cast<float>(solid_layers_[selected - 1U].first)));
        }
        return false;
    }
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
                    ? "Shown as name_min and name_max."
                    : "The name in the media server. Share one to drive both.");
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
                "The values operators may set.");
    return y + row_height + row_gap;
}

// "Explode this", after the page has drawn: it adds layers, which the page
// is part way through drawing. The Explosion it sets off is opened and worked
// on, so its handle is the one the preview offers to drag.
void PageView::explode(Uid layer, Uid object) {
    deferred_.push_back([this, layer, object](Document& document) { explode_now(document, layer, object); });
}

void PageView::explode_now(Document& document, Uid layer, Uid object) {
    if (const std::optional<Exploded> made = explode_this(document, layer, object)) {
        open_.insert(made->explosion);
        selection_.work_on_block(layer, made->explosion);
    }
}

// An Object's "Made of" changed: its material's values go into it and its
// Break, after the page has drawn, since the Break may be drawn further down.
void PageView::material_changed(Uid object) {
    deferred_.push_back([object](Document& document) { static_cast<void>(apply_material(document, object)); });
}

std::optional<gui::Rect> PageView::block_chip(Uid block, std::string_view name) const {
    for (const BlockChip& chip : block_chips_) {
        if (chip.block == block && chip.name == name) {
            return chip.rect;
        }
    }
    return std::nullopt;
}

} // namespace ajfx::studio
