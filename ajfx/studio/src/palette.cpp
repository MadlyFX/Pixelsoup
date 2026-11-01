#include "palette.hpp"

#include "catalog.hpp"
#include "raw_modules.hpp"
#include "recipes.hpp"

#include <mediaserver/editor/operator_widgets.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>

namespace ajfx::studio {
namespace {

namespace editor = mediaserver::editor;

constexpr float entry_height = 28.0F;
constexpr float heading_height = 26.0F;

std::string lower(std::string_view text) {
    std::string out{text};
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

} // namespace

void Palette::build() {
    if (!sections_.empty()) {
        return;
    }
    Section layers{"Layers", {}};
    layers.entries.push_back(Entry{Incoming{Incoming::Kind::Layer, "", "Empty layer", std::nullopt},
                                   "A layer with no blocks, to build from scratch.", "new blank", false});
    for (const LayerPreset& preset : layer_presets()) {
        layers.entries.push_back(Entry{Incoming{Incoming::Kind::Layer, std::string{preset.id}, std::string{preset.title},
                                                std::nullopt},
                                       std::string{preset.description}, "ready-made layer", false});
    }
    sections_.push_back(std::move(layers));
    for (const Category category : {Category::Birth, Category::Motion, Category::Look, Category::Contact,
                                    Category::Physics, Category::Light}) {
        Section section{std::string{category_name(category)}, {}};
        for (const BlockDef& def : block_catalog()) {
            if (def.category == category && def.kind != raw_block_kind) {
                section.entries.push_back(Entry{Incoming{Incoming::Kind::Block, std::string{def.kind},
                                                         std::string{def.title}, def.category},
                                                std::string{def.description}, std::string{def.keywords}, def.unique});
            }
        }
        sections_.push_back(std::move(section));
    }
    Section hood{"Under the hood", {}};
    for (const RawKindDef& def : raw_kinds()) {
        hood.entries.push_back(Entry{Incoming{Incoming::Kind::Module, std::string{def.kind}, std::string{def.title},
                                              Category::Expert},
                                     "One ajfx " + std::string{def.stage} + " module, " + std::string{def.kind} +
                                         ", every field editable.",
                                     "module raw expert", false});
    }
    sections_.push_back(std::move(hood));
}

std::optional<Incoming> Palette::take_clicked() {
    std::optional<Incoming> out = std::move(clicked_);
    clicked_.reset();
    return out;
}

void Palette::draw(gui::UiContext& ctx, const gui::Rect& bounds, const Document& document, Uid working_layer) {
    build();
    const gui::Theme& theme = ctx.theme();
    ctx.rounded(bounds, theme.panel, theme.radius_large);
    if (!open_) {
        // The rail: a way back in, and a hint of what is there.
        const gui::Rect show{bounds.x + (bounds.width - 30.0F) * 0.5F, bounds.y + 10.0F, 30.0F, 30.0F};
        if (editor::icon_button(ctx, gui::ui_id("studio.palette.show"), show, gui::Icon::Plus,
                                "Show the palette: every layer and block, to drag onto the page")) {
            open_ = true;
        }
        float y = show.bottom() + 16.0F;
        for (const Category category : {Category::Birth, Category::Motion, Category::Look, Category::Contact,
                                        Category::Physics, Category::Light}) {
            draw_category_icon(ctx, category, bounds.center_x(), y + 10.0F, gui::fade(theme.text_muted, 0.6F));
            y += 26.0F;
        }
        return;
    }

    // The heading and a way to fold it away.
    const gui::Rect title{bounds.x + 14.0F, bounds.y + 8.0F, bounds.width - 56.0F, 30.0F};
    ctx.label(title, "Palette", theme.text);
    ctx.tooltip(title, "Drag a layer or block onto the page, or click one to add it to the layer you are working on.");
    const gui::Rect hide{bounds.right() - 38.0F, bounds.y + 9.0F, 28.0F, 28.0F};
    if (editor::icon_button(ctx, gui::ui_id("studio.palette.hide"), hide, gui::Icon::Minus, "Fold the palette away")) {
        open_ = false;
        return;
    }
    // Search.
    const gui::Rect search{bounds.x + 10.0F, title.bottom() + 6.0F, bounds.width - 20.0F, 28.0F};
    static_cast<void>(ctx.text_field(gui::ui_id("studio.palette.search"), search, search_));
    if (search_.empty() && ctx.focus() != gui::ui_id("studio.palette.search")) {
        gui::draw_icon(ctx.draw_list(), gui::Icon::Search, search.x + 14.0F, search.center_y(), theme.text_muted, 0.8F);
        ctx.label(gui::Rect{search.x + 28.0F, search.y, search.width - 32.0F, search.height}, "Search",
                  gui::fade(theme.text_muted, 0.8F));
    }
    const std::string needle = lower(search_);

    const Layer* working = document.layer(working_layer);
    const gui::Rect view{bounds.x + 2.0F, search.bottom() + 8.0F, bounds.width - 4.0F,
                         std::max(bounds.bottom() - search.bottom() - 12.0F, 10.0F)};
    const float offset = scroll_.begin(ctx, view);
    float y = view.y - offset;
    const float x = view.x + 8.0F;
    const float w = view.width - 16.0F;
    for (const Section& section : sections_) {
        std::vector<const Entry*> shown;
        for (const Entry& entry : section.entries) {
            if (needle.empty() || lower(entry.incoming.title).find(needle) != std::string::npos ||
                lower(entry.description).find(needle) != std::string::npos ||
                lower(entry.keywords).find(needle) != std::string::npos) {
                shown.push_back(&entry);
            }
        }
        if (shown.empty()) {
            continue;
        }
        // A search shows every match, folded or not.
        const bool folded = needle.empty() && folded_.contains(section.heading);
        const gui::Rect head{x, y, w, heading_height};
        const gui::Interaction head_hit = ctx.grab(part_id(0U, "palette.section." + section.heading), head);
        ctx.cursor_for(head_hit, gui::Cursor::Hand);
        editor::draw_twirl(ctx, head.x + 8.0F, head.center_y(), !folded, theme.text_muted);
        ctx.label(gui::Rect{head.x + 20.0F, head.y, head.width - 20.0F, head.height}, section.heading,
                  head_hit.hovered ? theme.text : theme.text_muted);
        if (head_hit.clicked && needle.empty()) {
            if (folded) {
                folded_.erase(section.heading);
            } else {
                folded_.insert(section.heading);
            }
        }
        y += heading_height;
        if (folded) {
            y += 4.0F;
            continue;
        }
        for (const Entry* entry : shown) {
            const gui::Rect row{x, y, w, entry_height};
            const gui::UiId id = part_id(static_cast<std::uint64_t>(entry->incoming.kind),
                                         "palette.entry." + entry->incoming.id);
            const gui::Interaction hit = ctx.grab(id, row);
            ctx.cursor_for(hit, gui::Cursor::Hand);
            // A block that may be in a layer once, already in the working one.
            const bool there = entry->unique && working != nullptr &&
                               std::any_of(working->blocks.begin(), working->blocks.end(),
                                           [&](const Block& b) { return b.kind == entry->incoming.id; });
            const float glow = ctx.ease(id ^ 0x5A5AU, hit.hovered || hit.held ? 1.0F : 0.0F);
            if (glow > 0.0F) {
                ctx.rounded(row, gui::fade(theme.widget_hover, 0.6F * glow), theme.radius_small);
            }
            const gui::UiColor ink = there ? gui::fade(theme.text_muted, 0.7F) : theme.text;
            if (entry->incoming.category) {
                draw_category_icon(ctx, *entry->incoming.category, row.x + 14.0F, row.center_y(),
                                   gui::fade(theme.text_muted, there ? 0.6F : 1.0F));
            } else {
                gui::draw_icon(ctx.draw_list(), gui::Icon::Plus, row.x + 14.0F, row.center_y(),
                               gui::fade(theme.text_muted, 0.9F), 0.75F);
            }
            ctx.label(gui::Rect{row.x + 30.0F, row.y, row.width - 34.0F, row.height}, entry->incoming.title, ink);
            ctx.tooltip(row, there ? entry->description + " This layer has one already: drag it to another."
                                   : entry->description + " Drag onto the page, or click to add.");
            if (hit.pressed) {
                pressed_ = true;
                dragging_ = false;
                press_x_ = ctx.pointer_x();
                press_y_ = ctx.pointer_y();
                pressed_entry_ = entry->incoming;
            }
            if (hit.clicked && !dragging_) {
                clicked_ = entry->incoming;
            }
            y += entry_height;
        }
        y += 6.0F;
    }
    scroll_.end(ctx, y + offset - view.y + 8.0F);

    if (pressed_ && ctx.pointer_down() && !dragging_ &&
        std::hypot(ctx.pointer_x() - press_x_, ctx.pointer_y() - press_y_) > 5.0F) {
        dragging_ = true;
    }
}

void Palette::end_frame(gui::UiContext& ctx) {
    if (!ctx.pointer_down()) {
        pressed_ = false;
        dragging_ = false;
    }
}

} // namespace ajfx::studio
