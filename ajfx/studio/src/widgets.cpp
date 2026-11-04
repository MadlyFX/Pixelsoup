#include "widgets.hpp"

#include <mediaserver/editor/operator_widgets.hpp>
#include <mediaserver/gui/form_layout.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <initializer_list>
#include <memory>
#include <utility>

namespace ajfx::studio {

gui::Theme studio_theme(bool night) {
    gui::Theme theme;
    if (!night) { return theme; }
    const auto dim = [](gui::UiColor c, float amount) {
        return gui::UiColor{c.r * amount, c.g * amount, c.b * amount, c.a};
    };
    theme.background = dim(theme.background, 0.7F);
    theme.panel = dim(theme.panel, 0.72F);
    theme.header = dim(theme.header, 0.74F);
    theme.row = dim(theme.row, 0.72F);
    theme.row_alt = dim(theme.row_alt, 0.72F);
    theme.widget = dim(theme.widget, 0.76F);
    theme.widget_hover = dim(theme.widget_hover, 0.8F);
    theme.track = dim(theme.track, 0.7F);
    theme.divider = dim(theme.divider, 0.75F);
    theme.border = dim(theme.border, 0.75F);
    theme.text = dim(theme.text, 0.82F);
    theme.text_muted = dim(theme.text_muted, 0.85F);
    theme.glow *= 0.45F;
    return theme;
}

gui::UiId part_id(std::uint64_t owner, std::string_view name, std::uint32_t part) {
    gui::UiId id = gui::ui_id(name);
    id ^= (owner + 0x9E3779B97F4A7C15ULL) * 0xBF58476D1CE4E5B9ULL;
    id ^= (static_cast<std::uint64_t>(part) + 1ULL) * 0x94D049BB133111EBULL;
    return id;
}

void draw_category_icon(gui::UiContext& ctx, Category category, float cx, float cy, gui::UiColor color) {
    gui::Icon icon = gui::Icon::Details;
    switch (category) {
    case Category::Birth: icon = gui::Icon::Target; break;
    case Category::Motion: icon = gui::Icon::MoveTool; break;
    case Category::Look: icon = gui::Icon::Droplet; break;
    case Category::Contact: icon = gui::Icon::Surface; break;
    case Category::Physics: {
        // Three solid pieces come to rest on each other: a pile of bodies.
        constexpr float piece = 5.0F;
        constexpr float gap = 1.4F;
        constexpr float radius = 1.2F;
        constexpr float half = piece + gap * 0.5F;
        ctx.rounded(gui::Rect{cx - piece * 0.5F, cy - half, piece, piece}, color, radius);
        ctx.rounded(gui::Rect{cx - half, cy + gap * 0.5F, piece, piece}, color, radius);
        ctx.rounded(gui::Rect{cx + gap * 0.5F, cy + gap * 0.5F, piece, piece}, color, radius);
        return;
    }
    case Category::Light: icon = gui::Icon::Shading; break;
    case Category::Expert: icon = gui::Icon::Details; break;
    }
    gui::draw_icon(ctx.draw_list(), icon, cx, cy, color, 0.9F);
}

void draw_grip(gui::UiContext& ctx, float cx, float cy, gui::UiColor color) {
    for (int row = -1; row <= 1; ++row) {
        for (int column = 0; column < 2; ++column) {
            const float x = cx + (column == 0 ? -2.5F : 2.5F);
            const float y = cy + static_cast<float>(row) * 5.0F;
            ctx.rounded(gui::Rect{x - 1.25F, y - 1.25F, 2.5F, 2.5F}, color, 1.25F);
        }
    }
}

void draw_dice(gui::UiContext& ctx, float cx, float cy, gui::UiColor color) {
    // A rounded square, five pips.
    ctx.rounded_outline(gui::Rect{cx - 6.5F, cy - 6.5F, 13.0F, 13.0F}, color, 3.0F, 1.5F);
    constexpr float pip = 1.4F;
    for (const auto& [x, y] : {std::pair{-3.0F, -3.0F}, std::pair{3.0F, -3.0F}, std::pair{0.0F, 0.0F},
                               std::pair{-3.0F, 3.0F}, std::pair{3.0F, 3.0F}}) {
        ctx.rounded(gui::Rect{cx + x - pip, cy + y - pip, pip * 2.0F, pip * 2.0F}, color, pip);
    }
}

void draw_burst(gui::UiContext& ctx, float cx, float cy, gui::UiColor color) {
    // Eight rays out from a dot, alternately long and short.
    ctx.rounded(gui::Rect{cx - 2.0F, cy - 2.0F, 4.0F, 4.0F}, color, 2.0F);
    for (int ray = 0; ray < 8; ++ray) {
        const float angle = static_cast<float>(ray) * 0.785398163F;
        const float inner = 3.5F;
        const float outer = ray % 2 == 0 ? 7.0F : 5.5F;
        const float c = std::cos(angle);
        const float s = std::sin(angle);
        ctx.draw_list().add_line(cx + c * inner, cy + s * inner, cx + c * outer, cy + s * outer, 1.5F, color);
    }
}

bool glyph_key(gui::UiContext& ctx, gui::UiId id, const gui::Rect& bounds, GlyphDraw glyph, bool engaged,
               std::string_view tooltip, gui::UiColor engaged_color) {
    const gui::Interaction interaction = ctx.grab(id, bounds);
    ctx.cursor_for(interaction, gui::Cursor::Hand);
    const gui::Theme& theme = ctx.theme();
    const float hover = ctx.ease(id ^ gui::ui_id("studio.glyph-key.hover"), interaction.hovered ? 1.0F : 0.0F);
    const float radius = std::min(bounds.height * 0.5F, theme.radius);
    const bool pressed = interaction.held && !interaction.released;
    ctx.rounded(pressed ? bounds.inflated(-1.0F, -1.0F) : bounds,
                pressed ? theme.widget_active
                        : (engaged ? gui::fade(engaged_color, 0.22F + 0.08F * hover)
                                   : gui::blend(theme.widget, theme.widget_hover, hover)),
                radius);
    glyph(ctx, bounds.center_x(), bounds.center_y(),
          engaged ? engaged_color : gui::blend(theme.text_muted, theme.text, hover));
    ctx.tooltip(bounds, tooltip);
    return interaction.clicked;
}

bool choice_chips(gui::UiContext& ctx, gui::UiId id, const gui::Rect& bounds,
                  std::span<const std::string_view> labels, std::size_t& selected) {
    const gui::Theme& theme = ctx.theme();
    constexpr float pad = 9.0F;
    constexpr float gap = 4.0F;
    float total = 0.0F;
    for (const std::string_view label : labels) {
        total += ctx.font_width(label) + pad * 2.0F;
    }
    total += gap * static_cast<float>(labels.empty() ? 0U : labels.size() - 1U);
    if (total > bounds.width) {
        return ctx.dropdown(id, bounds, labels, selected);
    }
    bool changed = false;
    float x = bounds.x;
    for (std::size_t i = 0U; i < labels.size(); ++i) {
        const float width = ctx.font_width(labels[i]) + pad * 2.0F;
        const gui::Rect chip{x, bounds.y + 1.0F, width, bounds.height - 2.0F};
        const gui::UiId chip_id = id ^ ((static_cast<gui::UiId>(i) + 1U) * 0x9E3779B97F4A7C15ULL);
        const gui::Interaction interaction = ctx.grab(chip_id, chip);
        ctx.cursor_for(interaction, gui::Cursor::Hand);
        const bool on = i == selected;
        const float hover = ctx.ease(chip_id, interaction.hovered && !on ? 1.0F : 0.0F);
        const gui::UiColor body =
            on ? theme.selection : gui::blend(theme.track, theme.widget_hover, hover);
        ctx.rounded(chip, body, chip.height * 0.5F);
        if (on) {
            ctx.rounded_outline(chip, gui::fade(theme.accent, 0.55F), chip.height * 0.5F);
        }
        ctx.label_centered(chip, labels[i], on ? theme.text : theme.text_muted);
        if (interaction.clicked && !on) {
            selected = i;
            changed = true;
        }
        x += width + gap;
    }
    return changed;
}

bool switch_toggle(gui::UiContext& ctx, gui::UiId id, const gui::Rect& bounds, bool& value) {
    const gui::Theme& theme = ctx.theme();
    const gui::Rect track{bounds.x, bounds.center_y() - 9.0F, 34.0F, 18.0F};
    const gui::Interaction interaction = ctx.grab(id, track.inflated(4.0F, 4.0F));
    ctx.cursor_for(interaction, gui::Cursor::Hand);
    bool changed = false;
    if (interaction.clicked) {
        value = !value;
        changed = true;
    }
    const float on = ctx.ease(id, value ? 1.0F : 0.0F);
    ctx.rounded(track, gui::blend(theme.track, gui::fade(theme.accent, 0.55F), on), track.height * 0.5F);
    ctx.rounded_outline(track, theme.border, track.height * 0.5F);
    const float knob_x = track.x + 2.0F + on * (track.width - track.height);
    ctx.rounded(gui::Rect{knob_x, track.y + 2.0F, track.height - 4.0F, track.height - 4.0F},
                gui::blend(theme.text_muted, theme.text, on), (track.height - 4.0F) * 0.5F);
    return changed;
}

std::vector<std::string> wrap_text(const gui::UiContext& ctx, std::string_view text, float width) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t at = 0U;
    while (at < text.size()) {
        std::size_t end = text.find(' ', at);
        if (end == std::string_view::npos) {
            end = text.size();
        }
        const std::string_view word = text.substr(at, end - at);
        const std::string candidate = line.empty() ? std::string{word} : line + " " + std::string{word};
        if (!line.empty() && ctx.font_width(candidate) > width) {
            lines.push_back(std::move(line));
            line = std::string{word};
        } else {
            line = candidate;
        }
        at = end + 1U;
    }
    if (!line.empty()) {
        lines.push_back(std::move(line));
    }
    return lines;
}

void label_sized(gui::UiContext& ctx, const gui::Rect& bounds, std::string_view text, gui::UiColor color,
                 float multiple) {
    if (const std::optional<float> size = ctx.text_size_at_most(multiple)) {
        ctx.label(bounds, text, color, *size);
    } else {
        ctx.label(bounds, text, color);
    }
}

float text_width_sized(const gui::UiContext& ctx, std::string_view text, float multiple) {
    if (const std::optional<float> size = ctx.text_size_at_most(multiple)) {
        return ctx.font_width(text, *size);
    }
    return ctx.font_width(text);
}

std::string thousands(std::uint64_t value) {
    std::string digits = std::to_string(value);
    for (int at = static_cast<int>(digits.size()) - 3; at > 0; at -= 3) {
        digits.insert(static_cast<std::size_t>(at), ",");
    }
    return digits;
}

// ---------------------------------------------------------- popover list

namespace {

std::string lower(std::string_view text) {
    std::string out{text};
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

constexpr float popover_width = 380.0F;
constexpr float entry_height = 46.0F;
constexpr float group_height = 26.0F;

} // namespace

void PopoverList::open(float x, float y, std::string title, std::vector<ListEntry> entries, bool searchable) {
    open_ = true;
    fresh_ = true;
    x_ = x;
    y_ = y;
    title_ = std::move(title);
    entries_ = std::move(entries);
    searchable_ = searchable;
    search_.clear();
    highlight_ = 0U;
    scroll_.reset();
}

std::optional<std::size_t> PopoverList::draw(gui::UiContext& ctx, const gui::Rect& window) {
    if (!open_) {
        return std::nullopt;
    }
    // Open, the card is what the pointer is about: the button that opened it
    // must not explain itself over it. Its own rows may still.
    ctx.drop_tooltip();
    const gui::Theme& theme = ctx.theme();
    const gui::UiId search_id = gui::ui_id("studio.popover.search");

    // What matches the search, in order, with group headings between.
    const std::string needle = lower(search_);
    std::vector<std::size_t> shown;
    for (std::size_t i = 0U; i < entries_.size(); ++i) {
        const ListEntry& entry = entries_[i];
        if (needle.empty() || lower(entry.title).find(needle) != std::string::npos ||
            lower(entry.description).find(needle) != std::string::npos ||
            lower(entry.group).find(needle) != std::string::npos) {
            shown.push_back(i);
        }
    }
    float content = 0.0F;
    std::string group;
    for (const std::size_t i : shown) {
        if (entries_[i].group != group) {
            group = entries_[i].group;
            content += group.empty() ? 0.0F : group_height;
        }
        content += entry_height;
    }
    const float header = 34.0F + (searchable_ ? 36.0F : 0.0F);
    const float height = std::min(header + std::max(content, entry_height) + 10.0F, std::max(window.height - 40.0F, 160.0F));
    bounds_ = gui::Rect{std::clamp(x_, window.x + 8.0F, std::max(window.right() - popover_width - 8.0F, window.x + 8.0F)),
                        std::clamp(y_, window.y + 8.0F, std::max(window.bottom() - height - 8.0F, window.y + 8.0F)),
                        popover_width, height};

    // Keys: move through what is enabled, pick, or back out.
    std::vector<std::size_t> pickable;
    for (const std::size_t i : shown) {
        if (entries_[i].enabled) {
            pickable.push_back(i);
        }
    }
    if (highlight_ >= pickable.size()) {
        highlight_ = 0U;
    }
    std::optional<std::size_t> picked;
    if (ctx.key_pressed(gui::Key::Down) && !pickable.empty()) {
        highlight_ = (highlight_ + 1U) % pickable.size();
    }
    if (ctx.key_pressed(gui::Key::Up) && !pickable.empty()) {
        highlight_ = (highlight_ + pickable.size() - 1U) % pickable.size();
    }
    if (ctx.key_pressed(gui::Key::Enter) && !pickable.empty()) {
        picked = pickable[highlight_];
    }
    if (ctx.key_pressed(gui::Key::Escape)) {
        close();
        ctx.clear_focus();
        return std::nullopt;
    }
    if (!fresh_ && (ctx.pointer_pressed() || ctx.secondary_pointer_pressed()) &&
        !bounds_.contains(ctx.pointer_x(), ctx.pointer_y())) {
        close();
        ctx.clear_focus();
        return std::nullopt;
    }

    ctx.shadow(bounds_, theme.radius_large);
    ctx.rounded(bounds_, theme.panel, theme.radius_large);
    ctx.rounded_outline(bounds_, theme.border, theme.radius_large);
    const gui::Rect inner = gui::inset(bounds_, 10.0F);
    ctx.label(gui::Rect{inner.x + 4.0F, inner.y, inner.width, 24.0F}, title_, theme.text_muted);
    float y = inner.y + 28.0F;
    if (searchable_) {
        const gui::Rect field{inner.x, y, inner.width, 28.0F};
        if (fresh_) {
            ctx.focus_text_field(search_id, search_);
        }
        if (mediaserver::editor::search_box(ctx, search_id, field, search_, "Search")) {
            highlight_ = 0U;
            scroll_.reset();
        }
        y += 36.0F;
    }
    fresh_ = false;

    const gui::Rect list{inner.x, y, inner.width, inner.bottom() - y};
    const float offset = scroll_.begin(ctx, list);
    float row_y = list.y - offset;
    group.clear();
    const std::size_t highlighted = pickable.empty() ? entries_.size() : pickable[highlight_];
    for (const std::size_t i : shown) {
        const ListEntry& entry = entries_[i];
        if (entry.group != group) {
            group = entry.group;
            if (!group.empty()) {
                ctx.label(gui::Rect{list.x + 6.0F, row_y + 4.0F, list.width, group_height - 6.0F}, group,
                          theme.text_muted);
                row_y += group_height;
            }
        }
        const gui::Rect row{list.x, row_y, list.width, entry_height - 2.0F};
        const gui::UiId row_id = part_id(i, "studio.popover.entry");
        const gui::Interaction interaction = entry.enabled ? ctx.grab(row_id, row) : gui::Interaction{};
        if (entry.enabled) {
            ctx.cursor_for(interaction, gui::Cursor::Hand);
        }
        if (interaction.hovered && entry.enabled) {
            for (std::size_t p = 0U; p < pickable.size(); ++p) {
                if (pickable[p] == i) {
                    highlight_ = p;
                }
            }
        }
        if (i == highlighted) {
            ctx.rounded(row, theme.selection, theme.radius);
        }
        const gui::UiColor ink = entry.enabled ? theme.text : theme.text_muted;
        float text_x = row.x + 10.0F;
        if (entry.icon) {
            draw_category_icon(ctx, *entry.icon, row.x + 18.0F, row.center_y(), entry.enabled ? theme.text_muted : gui::fade(theme.text_muted, 0.5F));
            text_x = row.x + 36.0F;
        }
        ctx.label(gui::Rect{text_x, row.y + 4.0F, row.right() - text_x - 6.0F, 18.0F}, entry.title, ink);
        ctx.label(gui::Rect{text_x, row.y + 22.0F, row.right() - text_x - 6.0F, 18.0F}, entry.description,
                  gui::fade(theme.text_muted, entry.enabled ? 1.0F : 0.6F));
        if (interaction.clicked) {
            picked = i;
        }
        row_y += entry_height;
    }
    if (shown.empty()) {
        ctx.label(gui::Rect{list.x + 8.0F, list.y + 6.0F, list.width, 20.0F}, "Nothing matches", theme.text_muted);
    }
    scroll_.end(ctx, content);

    if (picked) {
        close();
        ctx.clear_focus();
    }
    return picked;
}

PopoverShield::PopoverShield(gui::UiContext& ctx, bool covered) {
    if (covered) {
        ctx_ = std::addressof(ctx);
        ctx_->push_pointer_block();
    }
}

void PopoverShield::release() noexcept {
    if (ctx_ != nullptr) {
        ctx_->pop_pointer_block();
        ctx_ = nullptr;
    }
}

} // namespace ajfx::studio
