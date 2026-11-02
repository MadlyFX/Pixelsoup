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
                                advice.fix_label, "Apply")) {
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

} // namespace ajfx::studio
