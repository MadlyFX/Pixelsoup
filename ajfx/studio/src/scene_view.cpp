#include "scene_view.hpp"

#include <mediaserver/editor/number_field.hpp>
#include <mediaserver/editor/operator_widgets.hpp>
#include <tinyfiledialogs.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <filesystem>
#include <type_traits>

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

// The decimals a value is shown with: the control's, or as many more (up to
// four) as it takes to show it as it is, so an ember of 0.015 m does not
// read 0.01.
int shown_decimals(float value, int decimals) {
    if (decimals <= 0) {
        return decimals;
    }
    int shown = decimals;
    for (; shown < 4; ++shown) {
        const double scaled = std::fabs(static_cast<double>(value)) * std::pow(10.0, shown);
        if (std::fabs(scaled - std::round(scaled)) < 2.0e-3) {
            break;
        }
    }
    return shown;
}

bool field(editor::NumberField &numbers, gui::UiContext &ctx, std::uint64_t owner, const ControlDef &control,
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
                               {}, value, control.decimals == 0 ? 1.0 : 0.01, out, shown_decimals(value, control.decimals),
                               nullptr, mixed);
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
                             axes[k], *values[k], 0.01, out, shown_decimals(*values[k], control.decimals), nullptr,
                             mixed && (mixed_axes & (1U << k)) != 0U)) {
                *values[k] = std::clamp(static_cast<float>(out), control.hard_min, control.hard_max);
                if (edited_axes) { *edited_axes |= 1U << k; }
                changed = true;
            }
        }
        ctx.tooltip(box, std::string{control.label} + " (" + std::string{control.unit} + ")");
    } else if (control.type == ControlType::Range) {
        // Two values: each particle picks its own between them.
        const float gap = 26.0F;
        const float width = (box.width - gap) * 0.5F;
        float *values[] = {&v.x, &v.y};
        for (std::uint32_t k = 0U; k < 2U; ++k) {
            double out = *values[k];
            const gui::Rect part{box.x + static_cast<float>(k) * (width + gap), box.y, width, box.height};
            if (numbers.draw(ctx, part_id(owner, control.id, k), part, {}, *values[k],
                             control.decimals == 0 ? 1.0 : 0.01, out, shown_decimals(*values[k], control.decimals), nullptr,
                             mixed)) {
                *values[k] = std::clamp(static_cast<float>(out), control.hard_min, control.hard_max);
                changed = true;
            }
        }
        ctx.label_centered(gui::Rect{box.x + width, box.y, gap, box.height}, "to", theme.text_muted);
        ctx.tooltip(box, std::string{control.label} + (control.unit.empty() ? "" : " (" + std::string{control.unit} + ")") +
                             ": each particle picks its own, between the two.");
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
        if ((o->distribution == Distribution::Single || o->distribution == Distribution::Placed) &&
            (p.control.id == "count" || p.control.id == "width" || p.control.id == "depth" ||
             p.control.id == "spacing" || p.control.id == "variation")) {
            return false;
        }
    }
    if (const auto *s = std::get_if<ParticleSystem>(&item.data); s && s->persistent && p.control.id == "lifetime") {
        return false;
    }
    if (const auto *s = std::get_if<ParticleSystem>(&item.data);
        s && !s->casts_light &&
        (p.control.id == "light_count" || p.control.id == "light_strength" || p.control.id == "light_reach")) {
        return false;
    }
    if (const auto *f = std::get_if<Force>(&item.data)) {
        const std::string_view id = p.control.id;
        const bool blast = f->kind == ForceKind::Blast;
        if (id == "position") { return f->kind != ForceKind::Wind; }
        if (id == "strength") { return !blast; }
        if (id == "heading") { return f->kind == ForceKind::Wind; }
        if (id == "axis") { return f->kind == ForceKind::Vortex; }
        if (id == "reach") { return f->kind == ForceKind::Attract; }
        if (id == "height") { return f->kind == ForceKind::DrawIn; }
        if (id == "power" || id == "radius" || id == "time") { return blast; }
        if (id == "all_systems") { return !blast; }
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

bool property_visible(const SceneItem &item, const SceneProperty &p) { return visible_property(item, p); }

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
               {"", "Floor", "A static surface aligned with the grid", true, {}},
               {"Forces", "Wind", "A steady push, on the systems it names", true, {}},
               {"Forces", "Vortex", "Spins particles around an axis", true, {}},
               {"Forces", "Attract", "Pulls to a point, or pushes from it", true, {}},
               {"Forces", "Draw in", "Pulls toward a line, like a fire's draught", true, {}},
               {"Forces", "Blast", "Throws everything out from a point, once", true, {}},
               {"Smoke", "Smoke grid", "A box of air that particle systems fill with smoke and ride", true, {}}},
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
            label += o->distribution == Distribution::Placed ? " (kind)" : " (" + std::to_string(o->count) + ")";
        }
        ctx.label(gui::Rect{row.x + (group ? 30 : 10), row.y, row.width - 34, row.height}, label,
                  item.simulate ? theme.text : theme.text_muted);
        ctx.tooltip(row, group                                          ? "Organizational group"
                         : std::holds_alternative<Object>(item.data)    ? "Mesh object"
                         : std::holds_alternative<Force>(item.data)     ? "Force"
                         : std::holds_alternative<SmokeGrid>(item.data) ? "Smoke grid"
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
    v = number(static_cast<float>(document.scene->physics.max_dropped));
    if (field(numbers_, ctx, 0U,
              numeric("max_dropped", "Most dropped at once", 64, 0, static_cast<float>(max_dropped_limit), {}, 0),
              gui::Rect{x, y, width, row_height}, v)) {
        document.scene->physics.max_dropped = static_cast<std::uint32_t>(
            std::lround(std::clamp(v.x, 0.0F, static_cast<float>(max_dropped_limit))));
    }
    y += 32;
    // What was done while the scene played, kept with it and replayed.
    heading(ctx, "Performance", x, y, width);
    const Performance &performance = document.scene->performance;
    const std::size_t drops = static_cast<std::size_t>(std::count_if(
        performance.acts.begin(), performance.acts.end(), [](const Act &act) { return act.kind == ActKind::Drop; }));
    const std::size_t deletions = performance.acts.size() - drops;
    const std::string summary = performance.acts.empty()
                                    ? std::string{"Nothing yet: drop things while it plays"}
                                    : std::to_string(drops) + (drops == 1U ? " drop, " : " drops, ") +
                                          std::to_string(deletions) + (deletions == 1U ? " deletion" : " deletions");
    ctx.label(gui::Rect{x, y, width - 40, 28}, summary, ctx.theme().text_muted);
    if (!performance.acts.empty() &&
        editor::icon_button(ctx, gui::ui_id("scene.performance.clear"), gui::Rect{x + width - 28.0F, y, 28, 28},
                            gui::Icon::Trash, "Clear the performance: take away every drop and deletion")) {
        document.scene->performance.acts.clear();
    }
    y += 36;
    heading(ctx, "Preview", x, y, width);
    // What the viewport stands the scene in: a lit stage, a dim night for
    // glowing effects, or nothing. Lit particles show the difference.
    ControlDef backdrop;
    backdrop.id = "backdrop";
    backdrop.label = "Backdrop";
    backdrop.type = ControlType::Choice;
    backdrop.choices = {{"studio", "Studio"}, {"night", "Night"}, {"void", "Void"}};
    backdrop.fallback = number(0.0F);
    v = number(static_cast<float>(document.backdrop));
    if (field(numbers_, ctx, 0U, backdrop, gui::Rect{x, y, width, row_height}, v)) {
        document.backdrop = static_cast<Backdrop>(std::clamp(static_cast<int>(v.x), 0, 2));
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
    heading(ctx, "Statistics", x, y, width);
    for (const std::string &line : statistics_) {
        const bool warning = line.rfind("Near a limit.", 0U) == 0U;
        for (const std::string &part : wrap_text(ctx, line, width)) {
            ctx.label(gui::Rect{x, y, width, 22}, part, warning ? ctx.theme().caution : ctx.theme().text_muted);
            y += 22;
        }
        y += 4;
    }
    ctx.tooltip(gui::Rect{x, y - 26.0F * static_cast<float>(std::max<std::size_t>(statistics_.size(), 1U)), width,
                          26.0F * static_cast<float>(std::max<std::size_t>(statistics_.size(), 1U))},
                "Counted from the compiled scene. A limit is named once it is four fifths used.");
    y += 8;
    draw_relationships(ctx, document, nullptr, x, y, width);
}

void SceneView::draw_relationships(gui::UiContext &ctx, Document &document, const SceneItem *item, float x,
                                   float &y, float width) {
    const bool system = item != nullptr && std::holds_alternative<ParticleSystem>(item->data);
    const bool object = item != nullptr && std::holds_alternative<Object>(item->data);
    const bool grid = item != nullptr && std::holds_alternative<SmokeGrid>(item->data);
    if (item != nullptr && !system && !object && !grid) {
        return;
    }
    const auto name_of = [&document](Uid uid) {
        const SceneItem *found = document.scene->item(uid);
        return found ? found->name : std::string{"(missing)"};
    };
    heading(ctx, "Relationships", x, y, width);
    Uid remove{};
    std::size_t shown = 0U;
    for (Relationship &r : document.scene->relationships) {
        const bool from_system = r.kind == RelationKind::TrailFromSystem;
        const bool smoke = !relation_births(r.kind);
        const bool touches = item == nullptr || r.system == item->uid ||
                             (object && !from_system && !smoke && (r.object == item->uid || r.object == 0U)) ||
                             (system && from_system && r.object == item->uid) || (grid && smoke && r.object == item->uid);
        if (!touches) {
            continue;
        }
        ++shown;
        // The rule as a sentence: what the source does, and what the system
        // does about it; for smoke, what the system does to the grid.
        const std::string title =
            smoke ? name_of(r.system) + (r.kind == RelationKind::FeedSmoke ? " / Feeds -> " : " / Rides -> ") +
                        name_of(r.object)
                  : (r.object == 0U ? std::string{"Every object"} : name_of(r.object)) +
                        (r.kind == RelationKind::EmitOnHit ? " / Hit -> " : " / Moving -> ") + name_of(r.system) +
                        (r.kind == RelationKind::EmitOnHit ? " / Emit" : " / Trail");
        ctx.label(gui::Rect{x, y, width - 32.0F, row_height}, title, ctx.theme().text);
        ctx.tooltip(gui::Rect{x, y, width - 32.0F, row_height},
                    r.kind == RelationKind::EmitOnHit ? "Particles are born where the object hits something."
                    : r.kind == RelationKind::FeedSmoke
                        ? "Its particles leave smoke, heat and their motion in the grid's air as they pass through it."
                    : r.kind == RelationKind::RideSmoke ? "Its particles are carried by the grid's air while inside it."
                    : from_system ? "Particles are shed behind that system's particles while they move."
                                  : "Particles are shed behind the object while it moves.");
        if (editor::icon_button(ctx, part_id(r.uid, "relation.remove"), gui::Rect{x + width - 28.0F, y, 28, 28},
                                gui::Icon::Trash, "Remove this relationship")) {
            remove = r.uid;
        }
        y += 30.0F;
        if (!relationship_active(*document.scene, r)) {
            for (const std::string &part :
                 wrap_text(ctx,
                           smoke       ? "Nothing happens: the smoke grid is switched off."
                           : from_system ? "Nothing is born: the system it trails is switched off."
                                         : "Nothing is born: hits and trails come from Dynamic objects.",
                           width)) {
                ctx.label(gui::Rect{x, y, width, 22}, part, ctx.theme().caution);
                y += 22;
            }
            y += 6.0F;
        }
        Value v;
        const auto number_row = [&](const ControlDef &control, float &target) {
            v = number(target);
            if (field(numbers_, ctx, r.uid, control, gui::Rect{x, y, width, row_height}, v)) {
                target = v.x;
            }
            y += 32.0F;
        };
        if (r.kind == RelationKind::FeedSmoke) {
            number_row(numeric("density", "Smoke", 1.0F, 0.0F, 20.0F, "/s"), r.density);
            number_row(numeric("heat", "Heat", 0.5F, 0.0F, 20.0F, "/s"), r.heat);
            number_row(numeric("push", "Push", 1.0F, 0.0F, 10.0F, "x"), r.push);
            y += 4.0F;
            continue;
        }
        if (r.kind == RelationKind::RideSmoke) {
            number_row(numeric("strength", "Strength", 10.0F, 0.0F, 100.0F, "/s", 1), r.strength);
            y += 4.0F;
            continue;
        }
        if (r.kind == RelationKind::EmitOnHit) {
            number_row(numeric("hardness", "Hits from", 1.0F, 0.0F, 10.0F, "m/s", 1), r.hardness);
            number_row(numeric("per_hit", "Per hit", 2.0F, 0.0F, 50.0F, {}, 0), r.per_hit);
            number_row(numeric("per_speed", "Per m/s", 3.0F, 0.0F, 50.0F, {}, 1), r.per_speed);
            number_row(numeric("spread", "Spread", 70.0F, 0.0F, 180.0F, "deg", 0), r.spread);
        } else {
            number_row(numeric("per_speed", "Per m/s", 10.0F, 0.0F, 100.0F, "/s", 1), r.per_speed);
            number_row(numeric("min_speed", "Moving from", 0.5F, 0.0F, 10.0F, "m/s"), r.min_speed);
            number_row(numeric("rate", "Base amount", 0.0F, 0.0F, 100.0F, "/s", 1), r.rate);
            if (!from_system) {
                ControlDef pieces = numeric("pieces", "Broken pieces only", 0.0F, 0.0F, 1.0F);
                pieces.type = ControlType::Toggle;
                pieces.fallback = toggle(false);
                v = toggle(r.pieces);
                if (field(numbers_, ctx, r.uid, pieces, gui::Rect{x, y, width, row_height}, v)) {
                    r.pieces = v.x != 0.0F;
                }
                y += 32.0F;
            }
        }
        v = number(static_cast<float>(r.seed));
        if (field(numbers_, ctx, r.uid, numeric("seed", "Seed", 0.0F, 0.0F, 1000000.0F, {}, 0),
                  gui::Rect{x, y, width, row_height}, v)) {
            r.seed = static_cast<std::uint32_t>(v.x);
        }
        y += 36.0F;
    }
    if (remove) {
        static_cast<void>(remove_relationship(document, remove));
    }
    if (shown == 0U) {
        // Wrapped, not cut short: a narrow inspector still reads all of it.
        for (const std::string &part :
             wrap_text(ctx,
                       item == nullptr ? "None. Add one from an object, a particle system or a smoke grid."
                       : grid          ? "None yet: no particle system feeds or rides it."
                                       : "None yet.",
                       width)) {
            ctx.label(gui::Rect{x, y, width, 22}, part, ctx.theme().text_muted);
            y += 22.0F;
        }
        y += 10.0F;
    }
    if (item == nullptr || document.scene->relationships.size() >= max_relationships) {
        return;
    }
    const bool worded = width >= 300.0F;
    if (grid) {
        // A smoke grid is fed and ridden by particle systems: each is
        // offered under both kinds, less what already binds it.
        if (editor::pill_button(ctx, part_id(item->uid, "relation.add"), gui::Rect{x, y + 4, worded ? 160.0F : 34.0F, 28},
                                gui::Icon::Plus, worded ? "Add relationship" : "",
                                "Have a particle system feed this smoke, or ride it")) {
            relation_target_ = item->uid;
            relation_from_system_ = false;
            relation_choices_.clear();
            std::vector<ListEntry> entries;
            for (const RelationKind kind : {RelationKind::FeedSmoke, RelationKind::RideSmoke}) {
                for (const SceneItem &other : document.scene->items) {
                    if (!std::holds_alternative<ParticleSystem>(other.data)) {
                        continue;
                    }
                    const bool bound = std::any_of(
                        document.scene->relationships.begin(), document.scene->relationships.end(),
                        [&](const Relationship &r) { return r.kind == kind && r.object == item->uid && r.system == other.uid; });
                    relation_choices_.emplace_back(kind, other.uid);
                    entries.push_back({std::string{relation_name(kind)}, other.name,
                                       bound ? std::string{"Already does"}
                                       : kind == RelationKind::FeedSmoke
                                           ? std::string{"Its particles leave smoke and heat in the air"}
                                           : std::string{"Its particles are carried by the air"},
                                       !bound, {}});
                }
            }
            if (entries.empty()) {
                entries.push_back({"", "No particle systems in the scene", "", false, {}});
            }
            relation_add_.open(x, y + 36, "Add relationship", std::move(entries), false);
        }
        y += 36.0F;
        return;
    }
    // Only a Dynamic object hits or moves, so only one can feed a rule: an
    // object that is not one has no button, and a system is offered only
    // the Dynamic objects, the rest shown but not takeable.
    const auto dynamic = [](const SceneItem &candidate) {
        const auto *shape = std::get_if<Object>(&candidate.data);
        return shape != nullptr && shape->motion == ObjectMotion::Dynamic;
    };
    if (object && !dynamic(*item)) {
        ctx.label(gui::Rect{x, y, width, row_height},
                  "Only a Dynamic object can feed one: set Physics > Motion to Dynamic.", ctx.theme().text_muted);
        y += 32.0F;
        return;
    }
    if (object) {
        // One edit: a system with no source of its own, and the rule that feeds it.
        if (editor::pill_button(ctx, part_id(item->uid, "relation.new_hits"), gui::Rect{x, y + 4, worded ? 220.0F : 34.0F, 28},
                                gui::Icon::Plus, worded ? "New system born of its hits" : "",
                                "A particle system with no source of its own: particles only where this object hits")) {
            if (const Uid made = add_bound_system(document, RelationKind::EmitOnHit, item->uid)) {
                select(made);
                return;
            }
        }
        y += 36.0F;
        if (editor::pill_button(ctx, part_id(item->uid, "relation.new_trail"), gui::Rect{x, y + 4, worded ? 220.0F : 34.0F, 28},
                                gui::Icon::Plus, worded ? "New system trailing it" : "",
                                "A particle system with no source of its own: particles only behind this object as it moves")) {
            if (const Uid made = add_bound_system(document, RelationKind::TrailFrom, item->uid)) {
                select(made);
                return;
            }
        }
        y += 36.0F;
    }
    if (system) {
        // One edit: a system with no source of its own, trailing this one's particles.
        if (editor::pill_button(ctx, part_id(item->uid, "relation.new_system_trail"),
                                gui::Rect{x, y + 4, worded ? 260.0F : 34.0F, 28}, gui::Icon::Plus,
                                worded ? "New system trailing its particles" : "",
                                "A particle system with no source of its own: particles only behind this system's "
                                "particles as they move")) {
            if (const Uid made = add_bound_system(document, RelationKind::TrailFromSystem, item->uid)) {
                select(made);
                return;
            }
        }
        y += 36.0F;
    }
    if (editor::pill_button(ctx, part_id(item->uid, "relation.add"), gui::Rect{x, y + 4, worded ? 160.0F : 34.0F, 28},
                            gui::Icon::Plus, worded ? "Add relationship" : "",
                            system ? "Have particles born where a Dynamic object hits, shed behind it, or shed "
                                     "behind another system's particles; or feed a smoke grid, or ride one"
                                   : "Have a particle system react to this object")) {
        relation_target_ = item->uid;
        relation_from_system_ = system;
        relation_choices_.clear();
        std::vector<ListEntry> entries;
        const bool any_dynamic = std::any_of(document.scene->items.begin(), document.scene->items.end(), dynamic);
        for (const RelationKind kind : {RelationKind::EmitOnHit, RelationKind::TrailFrom}) {
            const std::string group{relation_name(kind)};
            if (system) {
                relation_choices_.emplace_back(kind, 0U);
                entries.push_back({group, "Every Dynamic object",
                                   !any_dynamic ? std::string{"None in the scene yet"}
                                   : kind == RelationKind::EmitOnHit ? std::string{"Born wherever any of them hits"}
                                                                     : std::string{"Shed behind each as it moves"},
                                   any_dynamic, {}});
            }
            for (const SceneItem &other : document.scene->items) {
                if (system ? std::holds_alternative<Object>(other.data)
                           : std::holds_alternative<ParticleSystem>(other.data)) {
                    relation_choices_.emplace_back(kind, other.uid);
                    const bool still = system && !dynamic(other);
                    entries.push_back({group, other.name,
                                       still ? std::string{"Not Dynamic: it never hits or moves"}
                                       : kind == RelationKind::EmitOnHit ? std::string{"Born where it hits"}
                                                                         : std::string{"Shed behind it"},
                                       !still, {}});
                }
            }
        }
        if (system) {
            // Another system's particles, moving: never this system's own.
            const std::string group{relation_name(RelationKind::TrailFromSystem)};
            for (const SceneItem &other : document.scene->items) {
                if (other.uid != item->uid && std::holds_alternative<ParticleSystem>(other.data)) {
                    relation_choices_.emplace_back(RelationKind::TrailFromSystem, other.uid);
                    entries.push_back({group, other.name, "Shed behind its particles as they move", true, {}});
                }
            }
            // The scene's smoke grids, to feed and to ride; a system may do
            // both, once each.
            for (const RelationKind kind : {RelationKind::FeedSmoke, RelationKind::RideSmoke}) {
                for (const SceneItem &other : document.scene->items) {
                    if (!std::holds_alternative<SmokeGrid>(other.data)) {
                        continue;
                    }
                    const bool bound = std::any_of(
                        document.scene->relationships.begin(), document.scene->relationships.end(),
                        [&](const Relationship &r) { return r.kind == kind && r.object == other.uid && r.system == item->uid; });
                    relation_choices_.emplace_back(kind, other.uid);
                    entries.push_back({std::string{relation_name(kind)}, other.name,
                                       bound ? std::string{"Already does"}
                                       : kind == RelationKind::FeedSmoke
                                           ? std::string{"Its particles leave smoke and heat in the air"}
                                           : std::string{"Its particles are carried by the air"},
                                       !bound, {}});
                }
            }
        }
        if (entries.empty()) {
            entries.push_back({"", "No particle systems in the scene", "", false, {}});
        }
        relation_add_.open(x, y + 36, "Add relationship", std::move(entries), false);
    }
    y += 36.0F;
}

void SceneView::draw_sources(gui::UiContext &ctx, Document &document, SceneItem &item, float x, float &y, float width) {
    auto *system = std::get_if<ParticleSystem>(&item.data);
    if (!system) {
        return;
    }
    heading(ctx, "Sources", x, y, width);
    const bool bound = std::any_of(document.scene->relationships.begin(), document.scene->relationships.end(),
                                   [&item](const Relationship &r) { return r.system == item.uid; });
    if (system->sources.empty()) {
        for (const std::string &part :
             wrap_text(ctx, bound ? "None of its own: born only of its relationships." : "None: add one, or a relationship.",
                       width)) {
            ctx.label(gui::Rect{x, y, width, 22}, part, bound ? ctx.theme().text_muted : ctx.theme().caution);
            y += 22.0F;
        }
        y += 10.0F;
    }
    Uid remove{};
    std::size_t ordinal = 0U;
    for (ParticleSource &source : system->sources) {
        {
            // Numbered in the list's order and said in words: what it gives
            // and when, never its identity.
            const std::string gives =
                source.mode == SourceMode::Continuous ? format_number(source.rate, 2) + " a second"
                : source.mode == SourceMode::Initial  ? std::to_string(source.count) + " at the start"
                                                      : std::to_string(source.count) + " at " +
                                                           format_number(source.time, 2) + " s";
            ctx.label(gui::Rect{x, y, width - 30, 28}, "Source " + std::to_string(++ordinal) + ": " + gives,
                      ctx.theme().text_muted);
            if (editor::icon_button(ctx, part_id(source.uid, "remove"), gui::Rect{x + width - 28, y, 28, 28},
                                    gui::Icon::Trash,
                                    system->sources.size() == 1U && !bound
                                        ? "Remove source. With none, and no relationship, nothing is born."
                                        : "Remove source")) {
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

void SceneView::draw_grid_cells(gui::UiContext &ctx, const SceneItem &item, float x, float &y, float width) {
    const auto *grid = std::get_if<SmokeGrid>(&item.data);
    if (!grid) {
        return;
    }
    const std::array<std::uint32_t, 3> n = grid_resolution(*grid);
    const std::uint64_t cells = grid_cells(*grid);
    const auto count = [](std::uint64_t value) {
        std::string digits = std::to_string(value);
        for (std::size_t at = digits.size(); at > 3U; at -= 3U) {
            digits.insert(at - 3U, ",");
        }
        return digits;
    };
    const bool too_long = std::max({n[0], n[1], n[2]}) > max_volume_size;
    const bool too_many = cells > max_volume_cells;
    std::string text = std::to_string(n[0]) + " x " + std::to_string(n[1]) + " x " + std::to_string(n[2]) + " cells, " +
                       count(cells) + " of " + count(max_volume_cells);
    if (too_long) {
        text += ". A side may be at most " + std::to_string(max_volume_size) + " cells: raise the cell size.";
    } else if (too_many) {
        text += ". Too many for one grid: raise the cell size or shrink the box.";
    }
    for (const std::string &part : wrap_text(ctx, text, width)) {
        ctx.label(gui::Rect{x, y, width, 22}, part, too_long || too_many ? ctx.theme().caution : ctx.theme().text_muted);
        y += 22;
    }
    ctx.tooltip(gui::Rect{x, y - 22.0F, width, 22.0F},
                "The box cut into cells of the cell size, at least two a side. About 108 bytes a cell on the GPU.");
    y += 6.0F;
}

void SceneView::draw_targets(gui::UiContext &ctx, Document &document, SceneItem &item, float x, float &y,
                             float width) {
    auto *force = std::get_if<Force>(&item.data);
    if (!force || force->kind == ForceKind::Blast || force->all_systems) {
        return;
    }
    // One switch per particle system: on, the force acts on it.
    for (const SceneItem &other : document.scene->items) {
        if (!std::holds_alternative<ParticleSystem>(other.data)) {
            continue;
        }
        const auto at = std::find(force->targets.begin(), force->targets.end(), other.uid);
        bool on = at != force->targets.end();
        ctx.label(gui::Rect{x, y, width - 60.0F, row_height}, other.name, ctx.theme().text_muted);
        if (switch_toggle(ctx, part_id(item.uid, "target", other.uid), gui::Rect{x + width - 68.0F, y + 2.0F, 44.0F, 24.0F},
                          on)) {
            if (on) {
                force->targets.push_back(other.uid);
            } else {
                std::erase(force->targets, other.uid);
            }
        }
        y += 32.0F;
    }
    if (force->targets.empty()) {
        for (const std::string &part : wrap_text(ctx, "Acts on nothing until a system is switched on.", width)) {
            ctx.label(gui::Rect{x, y, width, 22}, part, ctx.theme().warning);
            y += 22.0F;
        }
        y += 10.0F;
    }
}

void SceneView::draw_motion_stack(gui::UiContext &ctx, Document &document, SceneItem &item, float x, float &y,
                                  float width) {
    auto *system = std::get_if<ParticleSystem>(&item.data);
    if (!system) {
        return;
    }
    Uid remove{};
    Uid moved{};
    int by = 0;
    const bool worded = width >= 300.0F;
    for (std::size_t i = 0U; i < system->motion.size(); ++i) {
        MotionBehavior &b = system->motion[i];
        // One row names the behavior and holds its switch, order and removal.
        const float buttons = 28.0F * 3.0F + 8.0F * 2.0F;
        const float switch_w = 44.0F;
        ctx.label(gui::Rect{x, y, width - buttons - switch_w - 16.0F, 28}, behavior_name(b.kind),
                  b.enabled ? ctx.theme().text : ctx.theme().text_muted);
        ctx.tooltip(gui::Rect{x, y, width - buttons - switch_w - 16.0F, 28},
                    b.kind == BehaviorKind::Drag         ? "Slows particles: speed lost per second."
                    : b.kind == BehaviorKind::Turbulence ? "Swirling, natural motion from curl noise."
                    : b.kind == BehaviorKind::Vortex     ? "Spins particles around an axis."
                    : b.kind == BehaviorKind::Wind       ? "A steady sideways push."
                    : b.kind == BehaviorKind::Attract    ? "Pulls particles to a point, or pushes them from it."
                                                         : "Pulls particles toward a line through the center.");
        float bx = x + width - buttons;
        if (editor::icon_button(ctx, part_id(b.uid, "behavior.up"), gui::Rect{bx, y, 28, 28}, gui::Icon::ToStart,
                                "Run earlier in the stack", false, i > 0U)) {
            moved = b.uid;
            by = -1;
        }
        bx += 36.0F;
        if (editor::icon_button(ctx, part_id(b.uid, "behavior.down"), gui::Rect{bx, y, 28, 28}, gui::Icon::ToEnd,
                                "Run later in the stack", false, i + 1U < system->motion.size())) {
            moved = b.uid;
            by = 1;
        }
        bx += 36.0F;
        if (editor::icon_button(ctx, part_id(b.uid, "behavior.remove"), gui::Rect{bx, y, 28, 28}, gui::Icon::Trash,
                                "Remove this behavior")) {
            remove = b.uid;
        }
        bool on = b.enabled;
        if (switch_toggle(ctx, part_id(b.uid, "behavior.enabled"),
                          gui::Rect{x + width - buttons - switch_w - 8.0F, y + 2.0F, switch_w, 24}, on)) {
            b.enabled = on;
        }
        ctx.tooltip(gui::Rect{x + width - buttons - switch_w - 8.0F, y, switch_w, 28},
                    on ? "On. Off keeps its settings but applies no force." : "Off. Its force is not applied.");
        y += 30.0F;
        Value v;
        const auto number_row = [&](const ControlDef &control, float &target) {
            v = number(target);
            if (field(numbers_, ctx, b.uid, control, gui::Rect{x, y, width, row_height}, v)) {
                target = v.x;
            }
            y += 32.0F;
        };
        const auto range_row = [&](std::string_view label, float fallback, float lo, float hi, std::string_view unit,
                                   int decimals) {
            ControlDef control = numeric("strength", label, fallback, lo, hi, unit, decimals);
            control.type = ControlType::Range;
            control.fallback = range(fallback, fallback);
            v = range(b.strength.min, b.strength.max);
            if (field(numbers_, ctx, b.uid, control, gui::Rect{x, y, width, row_height}, v)) {
                b.strength = {v.x, v.y};
            }
            y += 32.0F;
        };
        const auto center_row = [&](std::string_view label, Vec3 fallback) {
            ControlDef center = numeric("center", label, 0.0F, -10000.0F, 10000.0F, "m");
            center.type = ControlType::Vec3;
            center.fallback = vec3(fallback.x, fallback.y, fallback.z);
            v = vec3(b.center.x, b.center.y, b.center.z);
            const float height = property_height(center, width);
            if (field(numbers_, ctx, b.uid, center, gui::Rect{x, y, width, height}, v)) {
                b.center = {v.x, v.y, v.z};
            }
            y += height + 4.0F;
        };
        switch (b.kind) {
        case BehaviorKind::Drag:
            range_row("Amount", 1.0F, 0.0F, 10.0F, {}, 2);
            break;
        case BehaviorKind::Turbulence:
            range_row("Amount", 1.5F, 0.0F, 10.0F, {}, 2);
            number_row(numeric("size", "Swirl size", 0.6F, 0.05F, 5.0F, "m"), b.size);
            number_row(numeric("churn", "Churn", 0.4F, 0.0F, 3.0F), b.churn);
            break;
        case BehaviorKind::Vortex: {
            range_row("Strength", 3.0F, -20.0F, 20.0F, "m/s^2", 1);
            ControlDef axis = numeric("axis", "Axis", 0.0F, 0.0F, 1.0F);
            axis.type = ControlType::Choice;
            axis.choices = {{"Vertical", "Vertical"}, {"Horizontal", "Horizontal"}};
            v = number(static_cast<float>(b.axis));
            if (field(numbers_, ctx, b.uid, axis, gui::Rect{x, y, width, row_height}, v)) {
                b.axis = static_cast<VortexAxis>(static_cast<int>(v.x));
            }
            y += 32.0F;
            center_row("Center", {});
            break;
        }
        case BehaviorKind::Wind:
            range_row("Strength", 1.5F, 0.0F, 20.0F, "m/s^2", 1);
            number_row(numeric("heading", "Heading", 0.0F, 0.0F, 360.0F, "deg", 0), b.heading);
            break;
        case BehaviorKind::Attract:
            range_row("Strength", 4.0F, -30.0F, 30.0F, "m/s^2", 1);
            center_row("Point", {0.0F, 1.0F, 0.0F});
            number_row(numeric("reach", "Reach", 0.0F, 0.0F, 10.0F, "m"), b.reach);
            break;
        case BehaviorKind::DrawIn:
            range_row("Pull", 6.0F, 0.0F, 30.0F, "/s^2", 1);
            number_row(numeric("reach", "Fades by", 1.0F, 0.0F, 5.0F, "m"), b.reach);
            center_row("Center", {});
            break;
        }
        {
            v = number(static_cast<float>(b.seed));
            if (field(numbers_, ctx, b.uid, numeric("seed", "Seed", 0.0F, 0.0F, 1000000.0F, {}, 0),
                      gui::Rect{x, y, width, row_height}, v)) {
                b.seed = static_cast<std::uint32_t>(v.x);
            }
            y += 32.0F;
        }
        y += 4.0F;
    }
    if (remove) {
        std::erase_if(system->motion, [remove](const MotionBehavior &b) { return b.uid == remove; });
    } else if (moved) {
        static_cast<void>(move_behavior(document, item.uid, moved, by));
    }
    if (system->motion.size() < max_behaviors &&
        editor::pill_button(ctx, part_id(item.uid, "behavior.add"), gui::Rect{x, y + 4, worded ? 140.0F : 34.0F, 28},
                            gui::Icon::Plus, worded ? "Add behavior" : "", "Add a force to this system's motion")) {
        behavior_target_ = item.uid;
        behavior_add_.open(x, y + 36, "Add behavior",
                           {{"", "Drag", "Slows particles", true, {}},
                            {"", "Turbulence", "Swirling, natural motion", true, {}},
                            {"", "Vortex", "Spins particles around an axis", true, {}},
                            {"", "Wind", "A steady sideways push", true, {}},
                            {"", "Attract", "Pulls to a point, or pushes from it", true, {}},
                            {"", "Draw in", "Pulls toward a line, like a fire's draught", true, {}}},
                           false);
    }
    y += 36.0F;
}

namespace {
template <class Key> auto sample_keys(const std::vector<Key> &keys, float t) {
    const auto next = std::find_if(keys.begin(), keys.end(), [t](const Key &key) { return key.t >= t; });
    if (next == keys.begin()) {
        return keys.front().value;
    }
    if (next == keys.end()) {
        return keys.back().value;
    }
    const Key &a = *(next - 1);
    const Key &b = *next;
    const float f = b.t > a.t ? (t - a.t) / (b.t - a.t) : 1.0F;
    if constexpr (std::is_same_v<Key, ColorKey>) {
        return Color{a.value.r + (b.value.r - a.value.r) * f, a.value.g + (b.value.g - a.value.g) * f,
                     a.value.b + (b.value.b - a.value.b) * f, a.value.a + (b.value.a - a.value.a) * f};
    } else {
        return a.value + (b.value - a.value) * f;
    }
}
// Keys stay in age order: an edited age is held between its neighbours.
template <class Key> void clamp_age(std::vector<Key> &keys, std::size_t k) {
    const float lo = k == 0U ? 0.0F : keys[k - 1U].t;
    const float hi = k + 1U >= keys.size() ? 1.0F : keys[k + 1U].t;
    keys[k].t = std::clamp(keys[k].t, lo, hi);
}
template <class Key> void add_key_between_last(std::vector<Key> &keys) {
    const Key &a = keys[keys.size() - 2U];
    const Key &b = keys.back();
    Key fresh;
    fresh.t = (a.t + b.t) * 0.5F;
    fresh.value = sample_keys(keys, fresh.t);
    keys.insert(keys.end() - 1, fresh);
}
} // namespace

void SceneView::draw_over_age(gui::UiContext &ctx, SceneItem &item, float x, float &y, float width) {
    auto *system = std::get_if<ParticleSystem>(&item.data);
    if (!system) {
        return;
    }
    // UI identities for keys live beside the item's, never among scene uids.
    const auto owner = [&item](std::size_t slot) {
        return (1ULL << 40U) | (static_cast<std::uint64_t>(item.uid) << 8U) | static_cast<std::uint64_t>(slot);
    };
    const auto driver_row = [&](std::string_view id, std::string_view label, Driver &driver, bool &switched) {
        ControlDef control = numeric(id, label, 0.0F, 0.0F, 1.0F);
        control.type = ControlType::Choice;
        control.choices = {{"Constant", "Constant"}, {"Keys", "Keys"}};
        control.help = "Constant keeps one value for life. Keys follow the particle's age from birth (0) to death (1).";
        Value v = number(static_cast<float>(driver));
        if (field(numbers_, ctx, item.uid, control, gui::Rect{x, y, width, row_height}, v)) {
            const Driver chosen = static_cast<Driver>(static_cast<int>(v.x));
            switched = chosen == Driver::Age && driver != Driver::Age;
            driver = chosen;
        }
        y += 32.0F;
    };
    const auto strip = [&](auto sample_color, auto sample_height) {
        constexpr int bars = 40;
        const gui::Rect area{x, y, width - 24.0F, 36.0F};
        ctx.rounded(area, ctx.theme().track, 6.0F);
        const float bar = area.width / static_cast<float>(bars);
        for (int k = 0; k < bars; ++k) {
            const float t = (static_cast<float>(k) + 0.5F) / static_cast<float>(bars);
            const float h = std::clamp(sample_height(t), 0.0F, 1.0F) * (area.height - 6.0F);
            ctx.rounded(gui::Rect{area.x + bar * static_cast<float>(k) + 1.0F, area.bottom() - 3.0F - h, bar - 2.0F, h},
                        sample_color(t), 2.0F);
        }
        ctx.tooltip(area, "Birth on the left, death on the right.");
        y += area.height + 6.0F;
    };

    bool switched = false;
    driver_row("size_driver", "Size over age", system->size_driver, switched);
    if (switched && system->size_over_age == ParticleSystem{}.size_over_age) {
        system->size_over_age = {{0.0F, 1.0F}, {1.0F, 0.0F}};
    }
    if (system->size_driver == Driver::Age) {
        auto &keys = system->size_over_age;
        float peak = 0.0F;
        for (const CurveKey &key : keys) {
            peak = std::max(peak, key.value);
        }
        const gui::UiColor accent = ctx.theme().accent;
        strip([accent](float) { return accent; },
              [&keys, peak](float t) { return peak > 0.0F ? sample_keys(keys, t) / peak : 0.0F; });
        std::size_t remove = keys.size();
        for (std::size_t k = 0U; k < keys.size(); ++k) {
            const float half = (width - 24.0F - 8.0F) * 0.5F;
            Value v = number(keys[k].t);
            ControlDef age = numeric("age", "Age", k == 0U ? 0.0F : 1.0F, 0.0F, 1.0F);
            if (field(numbers_, ctx, owner(k), age, gui::Rect{x, y, half, row_height}, v)) {
                keys[k].t = v.x;
                clamp_age(keys, k);
            }
            v = number(keys[k].value);
            if (field(numbers_, ctx, owner(k), numeric("scale", "Size", 1.0F, 0.0F, 100.0F, "x"),
                      gui::Rect{x + half + 8.0F, y, half, row_height}, v)) {
                keys[k].value = v.x;
            }
            if (keys.size() > 2U &&
                editor::icon_button(ctx, part_id(owner(k), "key.remove"), gui::Rect{x + width - 24.0F, y, 24, 28},
                                    gui::Icon::Minus, "Remove this key")) {
                remove = k;
            }
            y += 32.0F;
        }
        if (remove < keys.size()) {
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(remove));
        }
        if (keys.size() < max_curve_keys &&
            editor::pill_button(ctx, part_id(item.uid, "size_key.add"), gui::Rect{x, y, 100, 28}, gui::Icon::Plus,
                                "Add key", "Add a key before the last one")) {
            add_key_between_last(keys);
        }
        y += 36.0F;
    }

    switched = false;
    driver_row("tint_driver", "Color over age", system->tint_driver, switched);
    if (switched && system->tint_over_age == ParticleSystem{}.tint_over_age) {
        system->tint_over_age = {{0.0F, {1.0F, 1.0F, 1.0F, 1.0F}}, {1.0F, {1.0F, 1.0F, 1.0F, 0.0F}}};
    }
    if (system->tint_driver == Driver::Age) {
        auto &keys = system->tint_over_age;
        strip(
            [&keys](float t) {
                const Color c = sample_keys(keys, t);
                return gui::UiColor{std::min(c.r, 1.0F), std::min(c.g, 1.0F), std::min(c.b, 1.0F), 1.0F};
            },
            [&keys](float t) { return sample_keys(keys, t).a; });
        std::size_t remove = keys.size();
        for (std::size_t k = 0U; k < keys.size(); ++k) {
            const float half = (width - 24.0F - 8.0F) * 0.5F;
            Value v = number(keys[k].t);
            ControlDef age = numeric("age", "Age", k == 0U ? 0.0F : 1.0F, 0.0F, 1.0F);
            if (field(numbers_, ctx, owner(32U + k), age, gui::Rect{x, y, half, row_height}, v)) {
                keys[k].t = v.x;
                clamp_age(keys, k);
            }
            ControlDef well = numeric("color", "Color", 1.0F, 0.0F, 64.0F);
            well.type = ControlType::Color;
            well.fallback = color(1.0F, 1.0F, 1.0F, 1.0F);
            v = color(keys[k].value.r, keys[k].value.g, keys[k].value.b, keys[k].value.a);
            if (field(numbers_, ctx, owner(32U + k), well, gui::Rect{x + half + 8.0F, y, half, row_height}, v)) {
                keys[k].value = {v.x, v.y, v.z, v.w};
            }
            color_live_ = color_live_ || ctx.color_open(part_id(owner(32U + k), "color"));
            if (keys.size() > 2U &&
                editor::icon_button(ctx, part_id(owner(32U + k), "key.remove"),
                                    gui::Rect{x + width - 24.0F, y, 24, 28}, gui::Icon::Minus, "Remove this key")) {
                remove = k;
            }
            y += 32.0F;
        }
        if (remove < keys.size()) {
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(remove));
        }
        if (keys.size() < max_curve_keys &&
            editor::pill_button(ctx, part_id(item.uid, "tint_key.add"), gui::Rect{x, y, 100, 28}, gui::Icon::Plus,
                                "Add key", "Add a key before the last one")) {
            add_key_between_last(keys);
        }
        y += 36.0F;
    }
}

void SceneView::draw_inspector(gui::UiContext &ctx, const gui::Rect &bounds, Document &document,
                               std::string_view issue) {
    color_live_ = false;
    const auto &theme = ctx.theme();
    ctx.rounded(bounds, theme.panel, theme.radius_large);
    const gui::Rect view{bounds.x + 12, bounds.y + 12, bounds.width - 24, bounds.height - 24};
    // Another selection starts at the top, at its name, rather than where
    // the last one was scrolled to.
    if (selection_ != inspected_) {
        inspected_ = selection_;
        inspector_scroll_.reset();
    }
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
            const std::string fallback = default_item_name(*item);
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
                if (selection_.size() == 1U && section == "Motion") {
                    draw_motion_stack(ctx, document, *item, x, y, width);
                }
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
                        // A name the artist has not chosen follows what
                        // it names: an imported model takes the file's.
                        const std::string was = default_item_name(target);
                        other->write(target, applied);
                        if (target.name == was) {
                            target.name = default_item_name(target);
                        }
                    }
                }
            }
            if (p.control.type == ControlType::Color) {
                color_live_ = color_live_ || ctx.color_open(part_id(item->uid, p.control.id));
            }
            y += height + 4.0F;
            // The key editors follow the last Appearance row, after the light.
            if (selection_.size() == 1U && p.control.id == "shadowed") {
                draw_over_age(ctx, *item, x, y, width);
            }
            // What the box and cell size come to, under the last Smoke row,
            // and the limit when it is past: the number is there so the
            // artist can decide, nothing is thinned to fit.
            if (selection_.size() == 1U && p.control.id == "solve") {
                draw_grid_cells(ctx, *item, x, y, width);
            }
        }
        if (selection_.size() == 1U && section == "Motion") {
            draw_motion_stack(ctx, document, *item, x, y, width);
        }
        if (selection_.size() == 1U) {
            draw_sources(ctx, document, *item, x, y, width);
            draw_targets(ctx, document, *item, x, y, width);
            draw_relationships(ctx, document, item, x, y, width);
        }
    }
    inspector_scroll_.end(ctx, y + offset - view.y + 16);
}
void SceneView::draw_overlays(gui::UiContext &ctx, const gui::Rect &window, Document &document) {
    if (const auto chosen = behavior_add_.draw(ctx, window)) {
        static_cast<void>(add_behavior(document, behavior_target_, static_cast<BehaviorKind>(*chosen)));
    }
    if (const auto chosen = relation_add_.draw(ctx, window)) {
        if (*chosen < relation_choices_.size()) {
            const auto [kind, other] = relation_choices_[*chosen];
            static_cast<void>(relation_from_system_ ? add_relationship(document, kind, other, relation_target_)
                                                    : add_relationship(document, kind, relation_target_, other));
        }
    }
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
        } else if (*chosen == 10U) {
            select(add_smoke_grid(document));
        } else if (*chosen >= 5U) {
            select(add_force(document, static_cast<ForceKind>(*chosen - 5U)));
        } else {
            import_models(document);
        }
    }
}
} // namespace ajfx::studio
