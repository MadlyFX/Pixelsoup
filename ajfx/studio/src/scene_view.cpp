#include "scene_view.hpp"

#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/editor/operator_widgets.hpp>
#include <tinyfiledialogs.h>

#include <algorithm>
#include <array>
#include <filesystem>

namespace ajfx::studio {
namespace {
namespace editor = mediaserver::editor;
constexpr float row_height = 28.0F;

// Below this an axis field cannot show a value such as "-12.50".
constexpr float min_axis_width = 76.0F;

// A vector sits beside its label when there is room, under it when not, and
// one axis to a row when even that leaves its values unreadable.
float property_height(const ControlDef &control, float width) {
    if (control.type != ControlType::Vec3) {
        return row_height;
    }
    const float field_width = width - std::min(124.0F, width * 0.42F) - 24.0F;
    if (field_width >= 228.0F) {
        return row_height;
    }
    return (width - 24.0F - 8.0F) / 3.0F >= min_axis_width ? 2.0F * row_height : 4.0F * row_height + 6.0F;
}

bool field(editor::NumberField &numbers, gui::UiContext &ctx, Uid owner, const ControlDef &control,
           const gui::Rect &row, Value &v, bool mixed = false, unsigned mixed_axes = 7U,
           unsigned *edited_axes = nullptr, bool *reset_value = nullptr) {
    if (edited_axes) { *edited_axes = 0U; }
    if (reset_value) { *reset_value = false; }
    const auto &theme = ctx.theme();
    const float label_w = std::min(124.0F, row.width * 0.42F);
    const bool stacked = control.type == ControlType::Vec3 && row.height > row_height;
    const bool axis_rows = control.type == ControlType::Vec3 && row.height > 2.0F * row_height;
    const gui::Rect box{stacked ? row.x : row.x + label_w, stacked ? row.y + row_height : row.y,
                        std::max(row.width - (stacked ? 0.0F : label_w) - 24.0F, 20.0F), row_height};
    const gui::Rect reset{row.right() - 20.0F, row.y + 4.0F, 18.0F, 20.0F};
    const std::string label = std::string{control.label} + (mixed ? " *" : "");
    ctx.label(gui::Rect{row.x, row.y, stacked ? row.width - 24.0F : label_w - 5.0F, row_height}, label,
              theme.text_muted);
    ctx.tooltip(row, mixed ? "Mixed values. Editing applies this value to the selection." : control.help);
    bool changed = false;
    if (control.type == ControlType::Number) {
        float value = v.x;
        const float unit_w = control.unit.empty() ? 0.0F : ctx.font_width(control.unit) + 5.0F;
        double out = value;
        changed = numbers.draw(ctx, part_id(owner, control.id), gui::Rect{box.x, box.y, box.width - unit_w, box.height},
                               {}, value, control.decimals == 0 ? 1.0 : 0.01, out, control.decimals, nullptr, mixed);
        if (changed) {
            v.x = std::clamp(static_cast<float>(out), control.hard_min, control.hard_max);
        }
        if (unit_w > 0) {
            ctx.label(gui::Rect{box.right() - unit_w + 3.0F, box.y, unit_w, box.height}, control.unit,
                      theme.text_muted);
        }
    } else if (control.type == ControlType::Vec3) {
        const float width = (box.width - 8.0F) / 3.0F;
        float *values[] = {&v.x, &v.y, &v.z};
        constexpr std::string_view axes[] = {"X", "Y", "Z"};
        for (std::uint32_t k = 0U; k < 3U; ++k) {
            double out = *values[k];
            const gui::Rect axis_box =
                axis_rows ? gui::Rect{box.x, box.y + static_cast<float>(k) * (row_height + 2.0F), box.width, box.height}
                          : gui::Rect{box.x + static_cast<float>(k) * (width + 4.0F), box.y, width, box.height};
            if (numbers.draw(ctx, part_id(owner, control.id, k), axis_box,
                             axes[k], *values[k], 0.01, out, control.decimals, nullptr,
                             mixed && (mixed_axes & (1U << k)) != 0U)) {
                *values[k] = std::clamp(static_cast<float>(out), control.hard_min, control.hard_max);
                if (edited_axes) { *edited_axes |= 1U << k; }
                changed = true;
            }
        }
        ctx.tooltip(box, std::string{control.label} + " (" + std::string{control.unit} + ")");
    } else if (control.type == ControlType::Color) {
        gui::UiColor tint{v.x, v.y, v.z, v.w};
        changed = ctx.color_well(part_id(owner, control.id), box, tint);
        if (changed) {
            v = color(tint.r, tint.g, tint.b, tint.a);
        }
    } else if (control.type == ControlType::Toggle) {
        bool on = v.x != 0;
        changed = switch_toggle(ctx, part_id(owner, control.id), box, on);
        if (changed) {
            v = toggle(on);
        }
    } else if (control.type == ControlType::Choice) {
        std::vector<std::string_view> labels;
        for (const ChoiceDef &c : control.choices) {
            labels.push_back(c.label);
        }
        std::size_t at = mixed ? labels.size() : static_cast<std::size_t>(v.x);
        changed = choice_chips(ctx, part_id(owner, control.id), box, labels, at);
        if (changed) {
            v = number(static_cast<float>(at));
        }
    } else if (control.type == ControlType::File) {
        if (editor::pill_button(ctx, part_id(owner, control.id), box, gui::Icon::Import,
                                std::filesystem::path{v.text}.filename().string().c_str(), "Choose a model")) {
            const char *filters[] = {"*.obj", "*.gltf", "*.glb"};
            if (const char *path = tinyfd_openFileDialog("Choose a model", "", 3, filters, "3D models", 0)) {
                v.text = std::filesystem::path{path}.generic_string();
                changed = true;
            }
        }
    }
    if ((mixed || v != control.fallback) &&
        editor::reset_button(ctx, part_id(owner, control.id, 8U), reset, "Reset to default")) {
        v = control.fallback;
        if (edited_axes) { *edited_axes = 7U; }
        if (reset_value) { *reset_value = true; }
        changed = true;
    }
    return changed;
}
ControlDef numeric(std::string_view id, std::string_view label, float fallback, float lo, float hi,
                   std::string_view unit = {}, int decimals = 2) {
    ControlDef c;
    c.id = id;
    c.label = label;
    c.fallback = number(fallback);
    c.min = c.hard_min = lo;
    c.max = c.hard_max = hi;
    c.unit = unit;
    c.decimals = decimals;
    return c;
}
void heading(gui::UiContext &ctx, std::string_view text, float x, float &y, float width) {
    y += 12.0F;
    ctx.label(gui::Rect{x, y, width, row_height}, text, ctx.theme().text);
    y += row_height + 2.0F;
}
bool visible_property(const SceneItem &item, const SceneProperty &p) {
    if (const auto *o = std::get_if<Object>(&item.data)) {
        if (p.section == "Animation" && o->motion != ObjectMotion::Animated) {
            return false;
        }
        if (p.section == "Fracture" && p.control.id != "fracture" && !o->fracture) {
            return false;
        }
        if (o->distribution == Distribution::Single &&
            (p.control.id == "count" || p.control.id == "width" || p.control.id == "depth" ||
             p.control.id == "spacing" || p.control.id == "variation")) {
            return false;
        }
    }
    if (const auto *s = std::get_if<ParticleSystem>(&item.data); s && s->persistent && p.control.id == "lifetime") {
        return false;
    }
    if (const auto *s = std::get_if<ParticleSystem>(&item.data);
        s && p.control.id == "persistent" &&
        std::any_of(s->sources.begin(), s->sources.end(),
                    [](const ParticleSource &source) { return source.mode == SourceMode::Continuous; })) {
        return false;
    }
    return true;
}
} // namespace

void SceneView::forget() {
    selection_.clear();
    folded_.clear();
    add_.close();
    tree_scroll_.reset();
    inspector_scroll_.reset();
    color_live_ = false;
}
void SceneView::select(Uid uid, bool extend) {
    if (!extend) {
        selection_.clear();
    }
    const auto at = std::find(selection_.begin(), selection_.end(), uid);
    if (at == selection_.end() && uid != 0U) {
        selection_.push_back(uid);
    } else if (at != selection_.end() && extend) {
        selection_.erase(at);
    }
}
void SceneView::open_add(float x, float y) {
    add_.open(x, y, "Add",
              {{"", "Object", "A visible model with editable physics", true, {}},
               {"", "Particle system", "A source, motion and appearance", true, {}},
               {"", "Group", "Organize without changing simulation", true, {}},
               {"", "Import models", "Place OBJ, glTF or GLB files", true, {}},
               {"", "Floor", "A static surface aligned with the grid", true, {}}},
              false);
}
void SceneView::import_models(Document &document) {
    const char *filters[] = {"*.obj", "*.gltf", "*.glb"};
    if (const char *paths = tinyfd_openFileDialog("Import models", "", 3, filters, "3D models", 1)) {
        selection_.clear();
        const std::string files{paths};
        for (std::size_t at = 0U; at < files.size();) {
            const auto end = files.find('|', at);
            const std::string file = files.substr(at, end == std::string::npos ? end : end - at);
            const Uid uid = add_object(document, std::filesystem::path{file}.generic_string());
            if (uid != 0U) {
                selection_.push_back(uid);
            }
            if (end == std::string::npos) {
                break;
            }
            at = end + 1U;
        }
    }
}
void SceneView::draw_tree(gui::UiContext &ctx, const gui::Rect &bounds, Document &document) {
    const auto &theme = ctx.theme();
    ctx.rounded(bounds, theme.panel, theme.radius_large);
    // Import keeps its word while both fit; Add, the one most reached for,
    // always does.
    const bool roomy = bounds.width >= 190.0F;
    const gui::Rect add{bounds.x + 10, bounds.y + 10, 72, 30};
    if (editor::pill_button(ctx, gui::ui_id("scene.add"), add, gui::Icon::Plus, "Add", "Create an item")) {
        open_add(add.x, add.bottom() + 4);
    }
    if (editor::pill_button(ctx, gui::ui_id("scene.import"), gui::Rect{add.right() + 6, add.y, roomy ? 92.0F : 34.0F, 30},
                            gui::Icon::Import, roomy ? "Import" : "", "Import models")) {
        import_models(document);
    }
    const gui::Rect view{bounds.x + 8, bounds.y + 50, bounds.width - 16, bounds.height - 58};
    const float offset = tree_scroll_.begin(ctx, view);
    float y = view.y - offset;
    const gui::Rect root{view.x, y, view.width, 32};
    const auto hit = ctx.grab(gui::ui_id("scene.root"), root);
    ctx.rounded(root, selection_.empty() ? theme.selection : hit.hovered ? theme.row : theme.panel, theme.radius);
    ctx.label(gui::Rect{root.x + 8, root.y, root.width - 16, root.height}, "Scene settings", theme.text);
    if (hit.clicked) {
        select(0U);
    }
    y += 38;
    if (document.scene) {
        tree_children(ctx, document, 0U, view.x, y, view.width, 0U);
    }
    if (document.scene && document.scene->items.empty()) {
        ctx.label(gui::Rect{view.x + 8, y + 10, view.width - 16, 28}, "Import models or add a system.",
                  theme.text_muted);
        y += 48;
    }
    tree_scroll_.end(ctx, y + offset - view.y);
}
void SceneView::tree_children(gui::UiContext &ctx, Document &document, Uid parent, float x, float &y, float width,
                              unsigned depth) {
    if (depth > 64U) {
        return;
    }
    const auto &theme = ctx.theme();
    for (const SceneItem &item : document.scene->items) {
        if (item.parent != parent) {
            continue;
        }
        const float indent = std::min(static_cast<float>(depth) * 14.0F, width * 0.5F);
        const gui::Rect row{x + indent, y, width - indent, 32};
        const bool selected = std::find(selection_.begin(), selection_.end(), item.uid) != selection_.end();
        const bool group = std::holds_alternative<Group>(item.data);
        const auto hit = ctx.grab(part_id(item.uid, "tree.row"), row);
        const float hover = ctx.ease(part_id(item.uid, "tree.hover"), hit.hovered ? 1.0F : 0.0F);
        ctx.rounded(row, selected ? theme.selection : gui::blend(theme.panel, theme.row, hover), theme.radius);
        if (group) {
            const gui::Rect fold{row.x + 2, row.y + 2, 26, 28};
            editor::draw_twirl(ctx, fold.center_x(), fold.center_y(), !folded_.contains(item.uid), theme.text_muted);
            if (hit.clicked && ctx.pointer_x() < fold.right()) {
                if (!folded_.erase(item.uid)) {
                    folded_.insert(item.uid);
                }
            }
        }
        std::string label = item.name;
        if (const auto *o = std::get_if<Object>(&item.data); o && o->distribution != Distribution::Single) {
            label += " (" + std::to_string(o->count) + ")";
        }
        ctx.label(gui::Rect{row.x + (group ? 30 : 10), row.y, row.width - 34, row.height}, label,
                  item.simulate ? theme.text : theme.text_muted);
        ctx.tooltip(row, group                                       ? "Organizational group"
                         : std::holds_alternative<Object>(item.data) ? "Mesh object"
                                                                     : "Particle system");
        if (hit.clicked && (!group || ctx.pointer_x() >= row.x + 28)) {
            select(item.uid, ctx.ctrl_held());
        }
        y += 35;
        if (group && !folded_.contains(item.uid)) {
            tree_children(ctx, document, item.uid, x, y, width, depth + 1U);
        }
    }
}

void SceneView::draw_settings(gui::UiContext &ctx, Document &document, float x, float &y, float width) {
    static_cast<void>(ctx.text_field(gui::ui_id("scene.name"), gui::Rect{x, y, width - 24, 30}, document.name));
    if (document.name != "Untitled scene" &&
        editor::reset_button(ctx, gui::ui_id("scene.name.reset"), gui::Rect{x + width - 20, y + 4, 18, 22},
                             "Reset to Untitled scene")) {
        document.name = "Untitled scene";
    }
    y += 38;
    heading(ctx, "Simulation", x, y, width);
    Value v = number(document.scene->physics.gravity);
    if (field(numbers_, ctx, 0U, numeric("gravity", "Gravity", 1, 0, 10, "g"), gui::Rect{x, y, width, row_height}, v)) {
        document.scene->physics.gravity = v.x;
    }
    y += 32;
    v = number(document.scene->physics.angular_damping);
    if (field(numbers_, ctx, 0U, numeric("damping", "Angular damping", 0.05F, 0, 1), gui::Rect{x, y, width, row_height},
              v)) {
        document.scene->physics.angular_damping = v.x;
    }
    y += 32;
    ControlDef sleep;
    sleep.id = "sleep";
    sleep.label = "Sleep when still";
    sleep.type = ControlType::Toggle;
    sleep.fallback = toggle(true);
    v = toggle(document.scene->physics.allow_sleep);
    if (field(numbers_, ctx, 0U, sleep, gui::Rect{x, y, width, row_height}, v)) {
        document.scene->physics.allow_sleep = v.x != 0;
    }
    y += 32;
    heading(ctx, "Capture range", x, y, width);
    v = number(document.loop_seconds);
    if (field(numbers_, ctx, 0U, numeric("range", "Duration", 6, 0.5F, 3600, "s"), gui::Rect{x, y, width, row_height},
              v)) {
        document.loop_seconds = v.x;
    }
    y += 40;
    ctx.label(gui::Rect{x, y, width, 28}, "Fixed step: 60 Hz", ctx.theme().text_muted);
    y += 32;
    const std::size_t items = document.scene->items.size();
    ctx.label(gui::Rect{x, y, width, 28}, std::to_string(items) + (items == 1U ? " scene item" : " scene items"),
              ctx.theme().text_muted);
    y += 32;
}

void SceneView::draw_sources(gui::UiContext &ctx, Document &document, SceneItem &item, float x, float &y, float width) {
    auto *system = std::get_if<ParticleSystem>(&item.data);
    if (!system) {
        return;
    }
    heading(ctx, "Sources", x, y, width);
    Uid remove{};
    for (ParticleSource &source : system->sources) {
        if (system->sources.size() > 1U) {
            ctx.label(gui::Rect{x, y, width - 30, 28}, "Source " + std::to_string(source.uid), ctx.theme().text_muted);
            if (editor::icon_button(ctx, part_id(source.uid, "remove"), gui::Rect{x + width - 28, y, 28, 28},
                                    gui::Icon::Trash, "Remove source")) {
                remove = source.uid;
            }
            y += 30;
        }
        ControlDef mode = numeric("mode", "Source", 1, 0, 2);
        mode.type = ControlType::Choice;
        mode.choices =
            system->persistent
                ? std::vector<ChoiceDef>{{"Initial", "Initial"}, {"Burst", "Burst"}}
                : std::vector<ChoiceDef>{{"Initial", "Initial"}, {"Continuous", "Continuous"}, {"Burst", "Burst"}};
        mode.fallback = number(system->persistent ? 0.0F : 1.0F);
        Value v = number(system->persistent ? (source.mode == SourceMode::Initial ? 0.0F : 1.0F)
                                            : static_cast<float>(source.mode));
        if (field(numbers_, ctx, source.uid, mode, gui::Rect{x, y, width, row_height}, v)) {
            source.mode = system->persistent ? (v.x == 0 ? SourceMode::Initial : SourceMode::Burst)
                                             : static_cast<SourceMode>(static_cast<int>(v.x));
        }
        y += 32;
        if (source.mode == SourceMode::Continuous) {
            v = number(source.rate);
            if (field(numbers_, ctx, source.uid, numeric("rate", "Rate", 40, 0, 1000000, "/s", 0),
                      gui::Rect{x, y, width, row_height}, v)) {
                source.rate = v.x;
            }
        } else {
            v = number(static_cast<float>(source.count));
            if (field(numbers_, ctx, source.uid, numeric("count", "Count", 100, 1, 1000000, {}, 0),
                      gui::Rect{x, y, width, row_height}, v)) {
                source.count = static_cast<std::uint32_t>(v.x);
            }
        }
        y += 32;
        if (source.mode == SourceMode::Burst) {
            v = number(source.time);
            if (field(numbers_, ctx, source.uid, numeric("time", "Scene time", 0, 0, 3600, "s"),
                      gui::Rect{x, y, width, row_height}, v)) {
                source.time = v.x;
            }
            y += 32;
        }
    }
    if (remove) {
        std::erase_if(system->sources, [remove](const ParticleSource &s) { return s.uid == remove; });
    }
    if (system->sources.size() < 32U &&
        editor::pill_button(ctx, part_id(item.uid, "source.add"), gui::Rect{x, y + 4, 120, 28}, gui::Icon::Plus,
                            "Add source", "Add another source to this population")) {
        static_cast<void>(
            add_source(document, item.uid, system->persistent ? SourceMode::Initial : SourceMode::Continuous));
    }
    y += 36;
}

void SceneView::draw_inspector(gui::UiContext &ctx, const gui::Rect &bounds, Document &document,
                               std::string_view issue) {
    color_live_ = false;
    const auto &theme = ctx.theme();
    ctx.rounded(bounds, theme.panel, theme.radius_large);
    const gui::Rect view{bounds.x + 12, bounds.y + 12, bounds.width - 24, bounds.height - 24};
    const float offset = inspector_scroll_.begin(ctx, view);
    const float x = view.x;
    const float width = view.width;
    float y = view.y - offset;
    std::erase_if(selection_, [&](Uid uid) { return document.scene->item(uid) == nullptr; });
    SceneItem *item = document.scene->item(selected());
    if (!issue.empty()) {
        heading(ctx, "Issues", x, y, width);
        for (const std::string &line : wrap_text(ctx, issue, width)) {
            ctx.label(gui::Rect{x, y, width, 22}, line, theme.warning);
            y += 22;
        }
        y += 12;
    }
    if (!item) {
        draw_settings(ctx, document, x, y, width);
    } else {
        if (selection_.size() == 1U) {
            static_cast<void>(ctx.text_field(part_id(item->uid, "name"), gui::Rect{x, y, width - 24, 30}, item->name));
            const auto *object = std::get_if<Object>(&item->data);
            const std::string fallback =
                object ? (object->mesh == "cube" ? "Object" : std::filesystem::path{object->mesh}.stem().string())
                : std::holds_alternative<Group>(item->data) ? "Group"
                                                            : "Particle system";
            if (item->name != fallback &&
                editor::reset_button(ctx, part_id(item->uid, "name.reset"), gui::Rect{x + width - 20, y + 4, 18, 22},
                                     "Reset name to " + fallback)) {
                item->name = fallback;
            }
        } else {
            ctx.label(gui::Rect{x, y, width, 30}, std::to_string(selection_.size()) + " items selected", theme.text);
        }
        y += 38;
        const bool worded = width >= 200.0F;
        const float duplicate_w = worded ? 100.0F : 34.0F;
        if (editor::pill_button(ctx, gui::ui_id("scene.duplicate"), gui::Rect{x, y, duplicate_w, 28}, gui::Icon::Plus,
                                worded ? "Duplicate" : "", "Duplicate selection")) {
            const std::vector<Uid> selected = selection_;
            selection_.clear();
            for (Uid uid : selected) {
                const Uid fresh = duplicate_item(document, uid);
                if (fresh) {
                    selection_.push_back(fresh);
                }
            }
            inspector_scroll_.end(ctx, y + offset - view.y + 40);
            return;
        }
        if (editor::pill_button(ctx, gui::ui_id("scene.delete"),
                                gui::Rect{x + duplicate_w + 8, y, worded ? 86.0F : 34.0F, 28}, gui::Icon::Trash,
                                worded ? "Delete" : "", "Remove selection", editor::ButtonTone::Danger)) {
            for (Uid uid : selection_) {
                remove_item(document, uid);
            }
            selection_.clear();
            inspector_scroll_.end(ctx, y + offset - view.y + 40);
            return;
        }
        y += 36;
        // Parent is organizational only; it never transforms the selection.
        std::vector<Uid> parents{0U};
        ControlDef parent = numeric("parent", "Group", 0, 0, 4096);
        parent.type = ControlType::Choice;
        parent.choices.push_back({"Scene", "Scene"});
        for (const SceneItem &candidate : document.scene->items) {
            bool descendant = false;
            for (const SceneItem *at = &candidate; at != nullptr; at = document.scene->item(at->parent)) {
                if (std::find(selection_.begin(), selection_.end(), at->uid) != selection_.end()) {
                    descendant = true;
                    break;
                }
                if (!at->parent) {
                    break;
                }
            }
            if (std::holds_alternative<Group>(candidate.data) && !descendant) {
                parents.push_back(candidate.uid);
                parent.choices.push_back({candidate.name, candidate.name});
            }
        }
        const auto current = std::find(parents.begin(), parents.end(), item->parent);
        Value parent_value = number(static_cast<float>(current == parents.end() ? 0 : current - parents.begin()));
        const bool mixed_parent = std::any_of(selection_.begin(), selection_.end(), [&](Uid uid) {
            return document.scene->item(uid)->parent != item->parent;
        });
        if (field(numbers_, ctx, item->uid, parent, gui::Rect{x, y, width, row_height}, parent_value, mixed_parent)) {
            for (Uid uid : selection_) {
                static_cast<void>(group_item(document, uid, parents[static_cast<std::size_t>(parent_value.x)]));
            }
        }
        y += 32;
        if (!std::holds_alternative<Group>(item->data)) {
            Value seed = number(static_cast<float>(item->seed));
            const bool mixed_seed = std::any_of(selection_.begin(), selection_.end(), [&](Uid uid) {
                return document.scene->item(uid)->seed != item->seed;
            });
            bool reset_seed = false;
            if (field(numbers_, ctx, item->uid,
                      numeric("seed", "Seed", static_cast<float>(item->uid), 0, 1000000, {}, 0),
                      gui::Rect{x, y, width, row_height}, seed, mixed_seed, 7U, nullptr, &reset_seed)) {
                for (Uid uid : selection_) {
                    document.scene->item(uid)->seed = reset_seed ? uid : static_cast<std::uint32_t>(seed.x);
                }
            }
            y += 32;
        }
        std::string_view section;
        for (const SceneProperty &p : scene_properties(*item)) {
            if (!visible_property(*item, p)) {
                continue;
            }
            Value v = p.read(*item);
            bool common = true;
            bool mixed = false;
            unsigned mixed_axes = 0U;
            for (Uid uid : selection_) {
                const SceneItem &target = *document.scene->item(uid);
                const auto props = scene_properties(target);
                const auto other = std::find_if(props.begin(), props.end(), [&](const SceneProperty &a) {
                    return a.control.id == p.control.id && a.control.type == p.control.type;
                });
                if (other == props.end()) {
                    common = false;
                    break;
                }
                mixed = mixed || other->read(target) != v;
                const Value other_value = other->read(target);
                if (other_value.x != v.x) { mixed_axes |= 1U; }
                if (other_value.y != v.y) { mixed_axes |= 2U; }
                if (other_value.z != v.z) { mixed_axes |= 4U; }
            }
            if (!common) {
                continue;
            }
            if (section != p.section) {
                section = p.section;
                heading(ctx, section, x, y, width);
            }
            unsigned edited_axes = 0U;
            const float height = property_height(p.control, width);
            if (field(numbers_, ctx, item->uid, p.control, gui::Rect{x, y, width, height}, v, mixed,
                      mixed_axes, &edited_axes)) {
                for (Uid uid : selection_) {
                    SceneItem &target = *document.scene->item(uid);
                    const auto props = scene_properties(target);
                    const auto other = std::find_if(props.begin(), props.end(), [&](const SceneProperty &a) {
                        return a.control.id == p.control.id && a.control.type == p.control.type;
                    });
                    if (other != props.end()) {
                        Value applied = v;
                        if (p.control.type == ControlType::Vec3) {
                            const Value before = other->read(target);
                            if ((edited_axes & 1U) == 0U) { applied.x = before.x; }
                            if ((edited_axes & 2U) == 0U) { applied.y = before.y; }
                            if ((edited_axes & 4U) == 0U) { applied.z = before.z; }
                        }
                        other->write(target, applied);
                    }
                }
            }
            if (p.control.type == ControlType::Color) {
                color_live_ = color_live_ || ctx.color_open(part_id(item->uid, p.control.id));
            }
            y += height + 4.0F;
        }
        if (selection_.size() == 1U) {
            draw_sources(ctx, document, *item, x, y, width);
        }
    }
    inspector_scroll_.end(ctx, y + offset - view.y + 16);
}
void SceneView::draw_overlays(gui::UiContext &ctx, const gui::Rect &window, Document &document) {
    if (const auto chosen = add_.draw(ctx, window)) {
        if (*chosen == 0U) {
            select(add_object(document));
        } else if (*chosen == 1U) {
            select(add_particle_system(document));
        } else if (*chosen == 2U) {
            const auto contents = selection_;
            const Uid group = add_group(document);
            for (Uid uid : contents) {
                static_cast<void>(group_item(document, uid, group));
            }
            select(group);
        } else if (*chosen == 4U) {
            select(add_floor(document));
        } else {
            import_models(document);
        }
    }
}
} // namespace ajfx::studio
