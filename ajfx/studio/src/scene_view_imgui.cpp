#include "scene_view.hpp"

#include "im_controls.hpp"
#include "performance.hpp"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <type_traits>
#include <vector>

// The scene tree and the inspector in Dear ImGui: the same selection, the
// same registry of properties and the same edits to the scene as the panels
// in scene_view.cpp, drawn as ImGui windows.
namespace ajfx::studio {
namespace {

ControlDef numeric_def(std::string_view id, std::string_view label, float fallback, float lo, float hi,
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

ImVec4 caution() { return ImVec4{1.0F, 0.72F, 0.25F, 1.0F}; }

void wrapped(const std::string& text, const ImVec4* tint = nullptr) {
    if (tint != nullptr) {
        ImGui::PushStyleColor(ImGuiCol_Text, *tint);
    }
    ImGui::TextWrapped("%s", text.c_str());
    if (tint != nullptr) {
        ImGui::PopStyleColor();
    }
}

template <class Key> auto sample_keys(const std::vector<Key>& keys, float t) {
    const auto next = std::find_if(keys.begin(), keys.end(), [t](const Key& key) { return key.t >= t; });
    if (next == keys.begin()) {
        return keys.front().value;
    }
    if (next == keys.end()) {
        return keys.back().value;
    }
    const Key& a = *(next - 1);
    const Key& b = *next;
    const float f = b.t > a.t ? (t - a.t) / (b.t - a.t) : 1.0F;
    if constexpr (std::is_same_v<Key, ColorKey>) {
        return Color{a.value.r + (b.value.r - a.value.r) * f, a.value.g + (b.value.g - a.value.g) * f,
                     a.value.b + (b.value.b - a.value.b) * f, a.value.a + (b.value.a - a.value.a) * f};
    } else {
        return a.value + (b.value - a.value) * f;
    }
}
template <class Key> void clamp_age(std::vector<Key>& keys, std::size_t k) {
    const float lo = k == 0U ? 0.0F : keys[k - 1U].t;
    const float hi = k + 1U >= keys.size() ? 1.0F : keys[k + 1U].t;
    keys[k].t = std::clamp(keys[k].t, lo, hi);
}
template <class Key> void add_key_between_last(std::vector<Key>& keys) {
    const Key& a = keys[keys.size() - 2U];
    const Key& b = keys.back();
    Key fresh;
    fresh.t = (a.t + b.t) * 0.5F;
    fresh.value = sample_keys(keys, fresh.t);
    keys.insert(keys.end() - 1, fresh);
}

// One entry of the relationship menu.
struct Offer final {
    std::string group;
    std::string label;
    std::string hint;
    bool enabled{true};
    RelationKind kind{};
    Uid other{};
};

} // namespace

// ------------------------------------------------------------------ tree

void SceneView::tree_panel(Document& document) {
    if (!ImGui::Begin("Scene")) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("+ Add") || add_requested_) {
        add_requested_ = false;
        ImGui::OpenPopup("add_item");
    }
    ImGui::SameLine();
    if (ImGui::Button("Import")) {
        import_models(document);
    }
    if (ImGui::BeginPopup("add_item")) {
        const auto item = [](const char* label, const char* tip) {
            const bool chosen = ImGui::MenuItem(label);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", tip);
            }
            return chosen;
        };
        if (item("Object", "A visible model with editable physics")) {
            select(add_object(document));
        }
        if (item("Particle system", "A source, motion and appearance")) {
            select(add_particle_system(document));
        }
        if (item("Group", "Organize without changing simulation")) {
            const auto contents = selection_;
            const Uid group = add_group(document);
            for (const Uid uid : contents) {
                static_cast<void>(group_item(document, uid, group));
            }
            select(group);
        }
        if (item("Floor", "A static surface aligned with the grid")) {
            select(add_floor(document));
        }
        ImGui::SeparatorText("Forces");
        const std::array<std::pair<const char*, const char*>, 5> forces{
            {{"Wind", "A steady push, on the systems it names"},
             {"Vortex", "Spins particles around an axis"},
             {"Attract", "Pulls to a point, or pushes from it"},
             {"Draw in", "Pulls toward a line, like a fire's draught"},
             {"Blast", "Throws everything out from a point, once"}}};
        for (std::size_t k = 0U; k < forces.size(); ++k) {
            if (item(forces[k].first, forces[k].second)) {
                select(add_force(document, static_cast<ForceKind>(k)));
            }
        }
        ImGui::SeparatorText("Smoke");
        if (item("Smoke grid", "A box of air that particle systems fill with smoke and ride")) {
            select(add_smoke_grid(document));
        }
        ImGui::EndPopup();
    }
    ImGui::Separator();
    tree_rows_.clear();
    if (ImGui::Selectable("Scene settings", selection_.empty())) {
        select(0U);
    }
    if (document.scene) {
        const auto children = [&](auto&& self, Uid parent, unsigned depth) -> void {
            if (depth > 64U) {
                return;
            }
            for (const SceneItem& item : document.scene->items) {
                if (item.parent != parent) {
                    continue;
                }
                ImGui::PushID(static_cast<int>(item.uid));
                tree_rows_.emplace_back(item.uid, ImVec4{});
                const bool selected = std::find(selection_.begin(), selection_.end(), item.uid) != selection_.end();
                const bool group = std::holds_alternative<Group>(item.data);
                std::string label = item.name;
                if (const auto* o = std::get_if<Object>(&item.data); o && o->distribution != Distribution::Single) {
                    label += o->distribution == Distribution::Placed ? " (kind)" : " (" + std::to_string(o->count) + ")";
                }
                ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoAutoOpenOnLog;
                if (selected) {
                    flags |= ImGuiTreeNodeFlags_Selected;
                }
                if (!item.simulate) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                }
                bool open = false;
                if (group) {
                    ImGui::SetNextItemOpen(!folded_.contains(item.uid), ImGuiCond_Always);
                    open = ImGui::TreeNodeEx("##node", flags | ImGuiTreeNodeFlags_OpenOnArrow, "%s", label.c_str());
                    if (ImGui::IsItemToggledOpen() && !folded_.erase(item.uid)) {
                        folded_.insert(item.uid);
                    }
                } else {
                    ImGui::TreeNodeEx("##node", flags | ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen,
                                      "%s", label.c_str());
                }
                if (!item.simulate) {
                    ImGui::PopStyleColor();
                }
                tree_rows_.back().second = ImVec4{ImGui::GetItemRectMin().x, ImGui::GetItemRectMin().y,
                                                  ImGui::GetItemRectMax().x, ImGui::GetItemRectMax().y};
                if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                    select(item.uid, ImGui::GetIO().KeyCtrl);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", group                                          ? "Organizational group"
                                            : std::holds_alternative<Object>(item.data)    ? "Mesh object"
                                            : std::holds_alternative<Force>(item.data)     ? "Force"
                                            : std::holds_alternative<SmokeGrid>(item.data) ? "Smoke grid"
                                                                                           : "Particle system");
                }
                if (group && open) {
                    self(self, item.uid, depth + 1U);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        };
        children(children, 0U, 0U);
        if (document.scene->items.empty()) {
            ImGui::TextDisabled("Import models or add a system.");
        }
    }
    ImGui::End();
}

// ------------------------------------------------------------- inspector

void SceneView::im_settings(Document& document) {
    ImGui::SetNextItemWidth(-ImGui::GetFrameHeight() - ImGui::GetStyle().ItemSpacing.x);
    ImGui::InputText("##scene_name", &document.name);
    if (document.name != "Untitled scene") {
        ImGui::SameLine();
        if (ImGui::Button("\xE2\x86\x90##name_reset")) {
            document.name = "Untitled scene";
        }
    }
    Scene& scene = *document.scene;
    im::heading("Simulation");
    im::number_row("gravity", "Gravity", scene.physics.gravity, 1.0F, 0.0F, 10.0F, "g");
    im::number_row("damping", "Angular damping", scene.physics.angular_damping, 0.05F, 0.0F, 1.0F);
    im::toggle_row("sleep", "Sleep when still", scene.physics.allow_sleep, true);
    float most = static_cast<float>(scene.physics.max_dropped);
    if (im::number_row("max_dropped", "Most dropped at once", most, 64.0F, 0.0F, static_cast<float>(max_dropped_limit),
                       {}, 0)) {
        scene.physics.max_dropped = static_cast<std::uint32_t>(
            std::lround(std::clamp(most, 0.0F, static_cast<float>(max_dropped_limit))));
    }
    im::heading("Performance");
    const Performance& performance = scene.performance;
    ImGui::TextDisabled("%s", performance.acts.empty() ? "Nothing yet: drop things while it plays"
                                                       : performance_summary(performance).c_str());
    if (!performance.acts.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear")) {
            scene.performance.acts.clear();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Clear the performance: take away everything done while it played");
        }
    }
    im::heading("Preview");
    int backdrop = static_cast<int>(document.backdrop);
    if (im::choice_row("backdrop", "Backdrop", backdrop, 0, {"Studio", "Night", "Void"})) {
        document.backdrop = static_cast<Backdrop>(std::clamp(backdrop, 0, 2));
    }
    im::heading("Capture range");
    im::number_row("range", "Duration", document.loop_seconds, 6.0F, 0.5F, 3600.0F, "s");
    ImGui::TextDisabled("Fixed step: 60 Hz");
    im::heading("Statistics");
    const ImVec4 warn = caution();
    for (const std::string& line : statistics_) {
        const bool warning = line.rfind("Near a limit.", 0U) == 0U;
        ImGui::PushStyleColor(ImGuiCol_Text, warning ? warn : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", line.c_str());
        ImGui::PopStyleColor();
    }
    im_relationships(document, nullptr);
}

void SceneView::im_relationships(Document& document, const SceneItem* item) {
    const bool system = item != nullptr && std::holds_alternative<ParticleSystem>(item->data);
    const bool object = item != nullptr && std::holds_alternative<Object>(item->data);
    const bool grid = item != nullptr && std::holds_alternative<SmokeGrid>(item->data);
    if (item != nullptr && !system && !object && !grid) {
        return;
    }
    const auto name_of = [&document](Uid uid) {
        const SceneItem* found = document.scene->item(uid);
        return found ? found->name : std::string{"(missing)"};
    };
    im::heading("Relationships");
    Uid remove{};
    std::size_t shown = 0U;
    const ImVec4 warn = caution();
    for (Relationship& r : document.scene->relationships) {
        const bool from_system = r.kind == RelationKind::TrailFromSystem;
        const bool smoke = !relation_births(r.kind);
        const bool touches = item == nullptr || r.system == item->uid ||
                             (object && !from_system && !smoke && (r.object == item->uid || r.object == 0U)) ||
                             (system && from_system && r.object == item->uid) || (grid && smoke && r.object == item->uid);
        if (!touches) {
            continue;
        }
        ++shown;
        ImGui::PushID(static_cast<int>(r.uid));
        const std::string title =
            smoke ? name_of(r.system) + (r.kind == RelationKind::FeedSmoke ? " / Feeds -> " : " / Rides -> ") +
                        name_of(r.object)
                  : (r.object == 0U ? std::string{"Every object"} : name_of(r.object)) +
                        (r.kind == RelationKind::EmitOnHit ? " / Hit -> " : " / Moving -> ") + name_of(r.system) +
                        (r.kind == RelationKind::EmitOnHit ? " / Emit" : " / Trail");
        ImGui::TextUnformatted(title.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", r.kind == RelationKind::EmitOnHit ? "Particles are born where the object hits something."
                                    : r.kind == RelationKind::FeedSmoke
                                        ? "Its particles leave smoke, heat and their motion in the grid's air as they pass through it."
                                    : r.kind == RelationKind::RideSmoke ? "Its particles are carried by the grid's air while inside it."
                                    : from_system ? "Particles are shed behind that system's particles while they move."
                                                  : "Particles are shed behind the object while it moves.");
        }
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
        if (ImGui::Button("x")) {
            remove = r.uid;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove this relationship");
        }
        if (!relationship_active(*document.scene, r)) {
            wrapped(smoke         ? "Nothing happens: the smoke grid is switched off."
                    : from_system ? "Nothing is born: the system it trails is switched off."
                                  : "Nothing is born: hits and trails come from Dynamic objects.",
                    &warn);
        }
        if (r.kind == RelationKind::FeedSmoke) {
            im::number_row("density", "Smoke", r.density, 1.0F, 0.0F, 20.0F, "/s");
            im::number_row("heat", "Heat", r.heat, 0.5F, 0.0F, 20.0F, "/s");
            im::number_row("push", "Push", r.push, 1.0F, 0.0F, 10.0F, "x");
        } else if (r.kind == RelationKind::RideSmoke) {
            im::number_row("strength", "Strength", r.strength, 10.0F, 0.0F, 100.0F, "/s", 1);
        } else {
            if (r.kind == RelationKind::EmitOnHit) {
                im::number_row("hardness", "Hits from", r.hardness, 1.0F, 0.0F, 10.0F, "m/s", 1);
                im::number_row("per_hit", "Per hit", r.per_hit, 2.0F, 0.0F, 50.0F, {}, 0);
                im::number_row("per_speed", "Per m/s", r.per_speed, 3.0F, 0.0F, 50.0F, {}, 1);
                im::number_row("spread", "Spread", r.spread, 70.0F, 0.0F, 180.0F, "deg", 0);
            } else {
                im::number_row("per_speed", "Per m/s", r.per_speed, 10.0F, 0.0F, 100.0F, "/s", 1);
                im::number_row("min_speed", "Moving from", r.min_speed, 0.5F, 0.0F, 10.0F, "m/s");
                im::number_row("rate", "Base amount", r.rate, 0.0F, 0.0F, 100.0F, "/s", 1);
                if (!from_system) {
                    im::toggle_row("pieces", "Broken pieces only", r.pieces, false);
                }
            }
            float seed = static_cast<float>(r.seed);
            if (im::number_row("seed", "Seed", seed, 0.0F, 0.0F, 1000000.0F, {}, 0)) {
                r.seed = static_cast<std::uint32_t>(seed);
            }
        }
        ImGui::Spacing();
        ImGui::PopID();
    }
    if (remove) {
        static_cast<void>(remove_relationship(document, remove));
    }
    if (shown == 0U) {
        ImGui::TextDisabled("%s", item == nullptr ? "None. Add one from an object, a particle system or a smoke grid."
                                  : grid          ? "None yet: no particle system feeds or rides it."
                                                  : "None yet.");
    }
    if (item == nullptr || document.scene->relationships.size() >= max_relationships) {
        return;
    }
    const auto dynamic = [](const SceneItem& candidate) {
        const auto* shape = std::get_if<Object>(&candidate.data);
        return shape != nullptr && shape->motion == ObjectMotion::Dynamic;
    };
    if (object && !dynamic(*item)) {
        ImGui::TextDisabled("Only a Dynamic object can feed one: set Physics > Motion to Dynamic.");
        return;
    }
    if (object) {
        if (ImGui::Button("New system born of its hits")) {
            if (const Uid made = add_bound_system(document, RelationKind::EmitOnHit, item->uid)) {
                select(made);
                return;
            }
        }
        if (ImGui::Button("New system trailing it")) {
            if (const Uid made = add_bound_system(document, RelationKind::TrailFrom, item->uid)) {
                select(made);
                return;
            }
        }
    }
    if (system && ImGui::Button("New system trailing its particles")) {
        if (const Uid made = add_bound_system(document, RelationKind::TrailFromSystem, item->uid)) {
            select(made);
            return;
        }
    }
    if (ImGui::Button("+ Add relationship")) {
        ImGui::OpenPopup("relation_add");
    }
    if (!ImGui::BeginPopup("relation_add")) {
        return;
    }
    std::vector<Offer> offers;
    const auto bound = [&](RelationKind kind, Uid source, Uid owner) {
        return std::any_of(document.scene->relationships.begin(), document.scene->relationships.end(),
                           [&](const Relationship& r) { return r.kind == kind && r.object == source && r.system == owner; });
    };
    const auto smoke_hint = [](RelationKind kind, bool already) {
        return already                           ? std::string{"Already does"}
               : kind == RelationKind::FeedSmoke ? std::string{"Its particles leave smoke and heat in the air"}
                                                 : std::string{"Its particles are carried by the air"};
    };
    if (grid) {
        for (const RelationKind kind : {RelationKind::FeedSmoke, RelationKind::RideSmoke}) {
            for (const SceneItem& other : document.scene->items) {
                if (std::holds_alternative<ParticleSystem>(other.data)) {
                    const bool already = bound(kind, item->uid, other.uid);
                    offers.push_back({std::string{relation_name(kind)}, other.name, smoke_hint(kind, already), !already,
                                      kind, other.uid});
                }
            }
        }
    } else {
        const bool any_dynamic = std::any_of(document.scene->items.begin(), document.scene->items.end(), dynamic);
        for (const RelationKind kind : {RelationKind::EmitOnHit, RelationKind::TrailFrom}) {
            const std::string group{relation_name(kind)};
            if (system) {
                offers.push_back({group, "Every Dynamic object",
                                  !any_dynamic ? std::string{"None in the scene yet"}
                                  : kind == RelationKind::EmitOnHit ? std::string{"Born wherever any of them hits"}
                                                                    : std::string{"Shed behind each as it moves"},
                                  any_dynamic, kind, 0U});
            }
            for (const SceneItem& other : document.scene->items) {
                if (system ? std::holds_alternative<Object>(other.data)
                           : std::holds_alternative<ParticleSystem>(other.data)) {
                    const bool still = system && !dynamic(other);
                    offers.push_back({group, other.name,
                                      still ? std::string{"Not Dynamic: it never hits or moves"}
                                      : kind == RelationKind::EmitOnHit ? std::string{"Born where it hits"}
                                                                        : std::string{"Shed behind it"},
                                      !still, kind, other.uid});
                }
            }
        }
        if (system) {
            for (const SceneItem& other : document.scene->items) {
                if (other.uid != item->uid && std::holds_alternative<ParticleSystem>(other.data)) {
                    offers.push_back({std::string{relation_name(RelationKind::TrailFromSystem)}, other.name,
                                      "Shed behind its particles as they move", true, RelationKind::TrailFromSystem,
                                      other.uid});
                }
            }
            for (const RelationKind kind : {RelationKind::FeedSmoke, RelationKind::RideSmoke}) {
                for (const SceneItem& other : document.scene->items) {
                    if (std::holds_alternative<SmokeGrid>(other.data)) {
                        const bool already = bound(kind, other.uid, item->uid);
                        offers.push_back({std::string{relation_name(kind)}, other.name, smoke_hint(kind, already),
                                          !already, kind, other.uid});
                    }
                }
            }
        }
    }
    if (offers.empty()) {
        ImGui::TextDisabled("No particle systems in the scene");
    }
    std::string group;
    for (const Offer& offer : offers) {
        if (offer.group != group) {
            group = offer.group;
            ImGui::SeparatorText(group.c_str());
        }
        if (ImGui::MenuItem(offer.label.c_str(), nullptr, false, offer.enabled)) {
            static_cast<void>(system ? add_relationship(document, offer.kind, offer.other, item->uid)
                             : add_relationship(document, offer.kind, item->uid, offer.other));
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", offer.hint.c_str());
        }
    }
    ImGui::EndPopup();
}

void SceneView::im_sources(Document& document, SceneItem& item) {
    auto* system = std::get_if<ParticleSystem>(&item.data);
    if (!system) {
        return;
    }
    im::heading("Sources");
    const bool bound = std::any_of(document.scene->relationships.begin(), document.scene->relationships.end(),
                                   [&item](const Relationship& r) { return r.system == item.uid; });
    if (system->sources.empty()) {
        const ImVec4 warn = caution();
        wrapped(bound ? "None of its own: born only of its relationships." : "None: add one, or a relationship.",
                bound ? nullptr : &warn);
    }
    Uid remove{};
    std::size_t ordinal = 0U;
    for (ParticleSource& source : system->sources) {
        ImGui::PushID(static_cast<int>(source.uid));
        const std::string gives =
            source.mode == SourceMode::Continuous ? format_number(source.rate, 2) + " a second"
            : source.mode == SourceMode::Initial  ? std::to_string(source.count) + " at the start"
                                                  : std::to_string(source.count) + " at " +
                                                        format_number(source.time, 2) + " s";
        ImGui::TextDisabled("Source %zu: %s", ++ordinal, gives.c_str());
        ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight());
        if (ImGui::Button("x")) {
            remove = source.uid;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", system->sources.size() == 1U && !bound
                                        ? "Remove source. With none, and no relationship, nothing is born."
                                        : "Remove source");
        }
        int mode = system->persistent ? (source.mode == SourceMode::Initial ? 0 : 1) : static_cast<int>(source.mode);
        const bool picked = system->persistent ? im::choice_row("mode", "Source", mode, 0, {"Initial", "Burst"})
                                               : im::choice_row("mode", "Source", mode, 1, {"Initial", "Continuous", "Burst"});
        if (picked) {
            source.mode = system->persistent ? (mode == 0 ? SourceMode::Initial : SourceMode::Burst)
                                             : static_cast<SourceMode>(mode);
        }
        if (source.mode == SourceMode::Continuous) {
            im::number_row("rate", "Rate", source.rate, 40.0F, 0.0F, 1000000.0F, "/s", 0);
        } else {
            float count = static_cast<float>(source.count);
            if (im::number_row("count", "Count", count, 100.0F, 1.0F, 1000000.0F, {}, 0)) {
                source.count = static_cast<std::uint32_t>(count);
            }
        }
        if (source.mode == SourceMode::Burst) {
            im::number_row("time", "Scene time", source.time, 0.0F, 0.0F, 3600.0F, "s");
        }
        ImGui::PopID();
    }
    if (remove) {
        std::erase_if(system->sources, [remove](const ParticleSource& s) { return s.uid == remove; });
    }
    if (system->sources.size() < 32U && ImGui::Button("+ Add source")) {
        static_cast<void>(
            add_source(document, item.uid, system->persistent ? SourceMode::Initial : SourceMode::Continuous));
    }
}

void SceneView::im_grid_cells(const SceneItem& item) {
    const auto* grid = std::get_if<SmokeGrid>(&item.data);
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
    const ImVec4 warn = caution();
    const ImVec4 quiet = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
    const ImVec4& tint = too_long || too_many ? warn : quiet;
    wrapped(text, &tint);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("The box cut into cells of the cell size, at least two a side. About 108 bytes a cell on the GPU.");
    }
}

void SceneView::im_targets(Document& document, SceneItem& item) {
    auto* force = std::get_if<Force>(&item.data);
    if (!force || force->kind == ForceKind::Blast || force->all_systems) {
        return;
    }
    for (const SceneItem& other : document.scene->items) {
        if (!std::holds_alternative<ParticleSystem>(other.data)) {
            continue;
        }
        const auto at = std::find(force->targets.begin(), force->targets.end(), other.uid);
        bool on = at != force->targets.end();
        ImGui::PushID(static_cast<int>(other.uid));
        if (ImGui::Checkbox(other.name.c_str(), &on)) {
            if (on) {
                force->targets.push_back(other.uid);
            } else {
                std::erase(force->targets, other.uid);
            }
        }
        ImGui::PopID();
    }
    if (force->targets.empty()) {
        const ImVec4 warn = caution();
        wrapped("Acts on nothing until a system is switched on.", &warn);
    }
}

void SceneView::im_motion_stack(Document& document, SceneItem& item) {
    auto* system = std::get_if<ParticleSystem>(&item.data);
    if (!system) {
        return;
    }
    Uid remove{};
    Uid moved{};
    int by = 0;
    for (std::size_t i = 0U; i < system->motion.size(); ++i) {
        MotionBehavior& b = system->motion[i];
        ImGui::PushID(static_cast<int>(b.uid));
        ImGui::Checkbox("##on", &b.enabled);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", b.enabled ? "On. Off keeps its settings but applies no force."
                                              : "Off. Its force is not applied.");
        }
        ImGui::SameLine();
        ImGui::TextUnformatted(std::string{behavior_name(b.kind)}.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", b.kind == BehaviorKind::Drag         ? "Slows particles: speed lost per second."
                                    : b.kind == BehaviorKind::Turbulence ? "Swirling, natural motion from curl noise."
                                    : b.kind == BehaviorKind::Vortex     ? "Spins particles around an axis."
                                    : b.kind == BehaviorKind::Wind       ? "A steady sideways push."
                                    : b.kind == BehaviorKind::Attract    ? "Pulls particles to a point, or pushes them from it."
                                                                         : "Pulls particles toward a line through the center.");
        }
        const float button = ImGui::GetFrameHeight();
        ImGui::SameLine(ImGui::GetContentRegionMax().x - button * 3.0F - ImGui::GetStyle().ItemSpacing.x * 2.0F);
        ImGui::BeginDisabled(i == 0U);
        if (ImGui::Button("^")) {
            moved = b.uid;
            by = -1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(i + 1U >= system->motion.size());
        if (ImGui::Button("v")) {
            moved = b.uid;
            by = 1;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("x")) {
            remove = b.uid;
        }
        const auto strength = [&](std::string_view label, float fallback, float lo, float hi, std::string_view unit,
                                  int decimals) {
            ControlDef control = numeric_def("strength", label, fallback, lo, hi, unit, decimals);
            control.type = ControlType::Range;
            control.fallback = range(fallback, fallback);
            Value v = range(b.strength.min, b.strength.max);
            if (im::row(control, v).changed) {
                b.strength = {v.x, v.y};
            }
        };
        const auto center = [&](std::string_view label, Vec3 fallback) {
            ControlDef control = numeric_def("center", label, 0.0F, -10000.0F, 10000.0F, "m");
            control.type = ControlType::Vec3;
            control.fallback = vec3(fallback.x, fallback.y, fallback.z);
            Value v = vec3(b.center.x, b.center.y, b.center.z);
            if (im::row(control, v).changed) {
                b.center = {v.x, v.y, v.z};
            }
        };
        switch (b.kind) {
        case BehaviorKind::Drag:
            strength("Amount", 1.0F, 0.0F, 10.0F, {}, 2);
            break;
        case BehaviorKind::Turbulence:
            strength("Amount", 1.5F, 0.0F, 10.0F, {}, 2);
            im::number_row("size", "Swirl size", b.size, 0.6F, 0.05F, 5.0F, "m");
            im::number_row("churn", "Churn", b.churn, 0.4F, 0.0F, 3.0F);
            break;
        case BehaviorKind::Vortex: {
            strength("Strength", 3.0F, -20.0F, 20.0F, "m/s^2", 1);
            int axis = static_cast<int>(b.axis);
            if (im::choice_row("axis", "Axis", axis, 0, {"Vertical", "Horizontal"})) {
                b.axis = static_cast<VortexAxis>(axis);
            }
            center("Center", {});
            break;
        }
        case BehaviorKind::Wind:
            strength("Strength", 1.5F, 0.0F, 20.0F, "m/s^2", 1);
            im::number_row("heading", "Heading", b.heading, 0.0F, 0.0F, 360.0F, "deg", 0);
            break;
        case BehaviorKind::Attract:
            strength("Strength", 4.0F, -30.0F, 30.0F, "m/s^2", 1);
            center("Point", {0.0F, 1.0F, 0.0F});
            im::number_row("reach", "Reach", b.reach, 0.0F, 0.0F, 10.0F, "m");
            break;
        case BehaviorKind::DrawIn:
            strength("Pull", 6.0F, 0.0F, 30.0F, "/s^2", 1);
            im::number_row("reach", "Fades by", b.reach, 1.0F, 0.0F, 5.0F, "m");
            center("Center", {});
            break;
        }
        float seed = static_cast<float>(b.seed);
        if (im::number_row("seed", "Seed", seed, 0.0F, 0.0F, 1000000.0F, {}, 0)) {
            b.seed = static_cast<std::uint32_t>(seed);
        }
        ImGui::Spacing();
        ImGui::PopID();
    }
    if (remove) {
        std::erase_if(system->motion, [remove](const MotionBehavior& b) { return b.uid == remove; });
    } else if (moved) {
        static_cast<void>(move_behavior(document, item.uid, moved, by));
    }
    if (system->motion.size() < max_behaviors && ImGui::Button("+ Add behavior")) {
        ImGui::OpenPopup("behavior_add");
    }
    if (ImGui::BeginPopup("behavior_add")) {
        const std::array<std::pair<const char*, const char*>, 6> kinds{
            {{"Drag", "Slows particles"},
             {"Turbulence", "Swirling, natural motion"},
             {"Vortex", "Spins particles around an axis"},
             {"Wind", "A steady sideways push"},
             {"Attract", "Pulls to a point, or pushes from it"},
             {"Draw in", "Pulls toward a line, like a fire's draught"}}};
        for (std::size_t k = 0U; k < kinds.size(); ++k) {
            if (ImGui::MenuItem(kinds[k].first)) {
                static_cast<void>(add_behavior(document, item.uid, static_cast<BehaviorKind>(k)));
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", kinds[k].second);
            }
        }
        ImGui::EndPopup();
    }
}

void SceneView::im_over_age(SceneItem& item) {
    auto* system = std::get_if<ParticleSystem>(&item.data);
    if (!system) {
        return;
    }
    const auto strip = [](auto sample_color, auto sample_height) {
        constexpr int bars = 40;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = ImGui::GetFrameHeight() * 1.4F;
        ImDrawList* list = ImGui::GetWindowDrawList();
        list->AddRectFilled(origin, ImVec2{origin.x + width, origin.y + height},
                            ImGui::GetColorU32(ImGuiCol_FrameBg), 4.0F);
        const float bar = width / static_cast<float>(bars);
        for (int k = 0; k < bars; ++k) {
            const float t = (static_cast<float>(k) + 0.5F) / static_cast<float>(bars);
            const float h = std::clamp(sample_height(t), 0.0F, 1.0F) * (height - 6.0F);
            const ImU32 c = sample_color(t);
            list->AddRectFilled(ImVec2{origin.x + bar * static_cast<float>(k) + 1.0F, origin.y + height - 3.0F - h},
                                ImVec2{origin.x + bar * static_cast<float>(k + 1) - 1.0F, origin.y + height - 3.0F}, c,
                                2.0F);
        }
        ImGui::Dummy(ImVec2{width, height});
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Birth on the left, death on the right.");
        }
    };
    const auto driver_row = [&](std::string_view id, Driver& driver, std::string_view label, bool& switched) {
        int chosen = static_cast<int>(driver);
        if (im::choice_row(id, label, chosen, 0, {"Constant", "Keys"})) {
            switched = chosen == static_cast<int>(Driver::Age) && driver != Driver::Age;
            driver = static_cast<Driver>(chosen);
        }
    };

    bool switched = false;
    driver_row("size_driver", system->size_driver, "Size over age", switched);
    if (switched && system->size_over_age == ParticleSystem{}.size_over_age) {
        system->size_over_age = {{0.0F, 1.0F}, {1.0F, 0.0F}};
    }
    if (system->size_driver == Driver::Age) {
        auto& keys = system->size_over_age;
        float peak = 0.0F;
        for (const CurveKey& key : keys) {
            peak = std::max(peak, key.value);
        }
        const ImU32 accent = ImGui::GetColorU32(ImGuiCol_ButtonActive);
        strip([accent](float) { return accent; },
              [&keys, peak](float t) { return peak > 0.0F ? sample_keys(keys, t) / peak : 0.0F; });
        std::size_t remove = keys.size();
        for (std::size_t k = 0U; k < keys.size(); ++k) {
            ImGui::PushID(static_cast<int>(k));
            im::number_row("age", "Age", keys[k].t, k == 0U ? 0.0F : 1.0F, 0.0F, 1.0F);
            clamp_age(keys, k);
            im::number_row("scale", "Size", keys[k].value, 1.0F, 0.0F, 100.0F, "x");
            if (keys.size() > 2U && ImGui::SmallButton("Remove key")) {
                remove = k;
            }
            ImGui::PopID();
        }
        if (remove < keys.size()) {
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(remove));
        }
        if (keys.size() < max_curve_keys && ImGui::Button("+ Add size key")) {
            add_key_between_last(keys);
        }
    }

    switched = false;
    driver_row("tint_driver", system->tint_driver, "Color over age", switched);
    if (switched && system->tint_over_age == ParticleSystem{}.tint_over_age) {
        system->tint_over_age = {{0.0F, {1.0F, 1.0F, 1.0F, 1.0F}}, {1.0F, {1.0F, 1.0F, 1.0F, 0.0F}}};
    }
    if (system->tint_driver == Driver::Age) {
        auto& keys = system->tint_over_age;
        strip(
            [&keys](float t) {
                const Color c = sample_keys(keys, t);
                return ImGui::ColorConvertFloat4ToU32(
                    ImVec4{std::min(c.r, 1.0F), std::min(c.g, 1.0F), std::min(c.b, 1.0F), 1.0F});
            },
            [&keys](float t) { return sample_keys(keys, t).a; });
        std::size_t remove = keys.size();
        for (std::size_t k = 0U; k < keys.size(); ++k) {
            ImGui::PushID(static_cast<int>(32U + k));
            im::number_row("age", "Age", keys[k].t, k == 0U ? 0.0F : 1.0F, 0.0F, 1.0F);
            clamp_age(keys, k);
            ControlDef well = numeric_def("color", "Color", 1.0F, 0.0F, 64.0F);
            well.type = ControlType::Color;
            well.fallback = color(1.0F, 1.0F, 1.0F, 1.0F);
            Value v = color(keys[k].value.r, keys[k].value.g, keys[k].value.b, keys[k].value.a);
            if (im::row(well, v).changed) {
                keys[k].value = {v.x, v.y, v.z, v.w};
            }
            if (keys.size() > 2U && ImGui::SmallButton("Remove key")) {
                remove = k;
            }
            ImGui::PopID();
        }
        if (remove < keys.size()) {
            keys.erase(keys.begin() + static_cast<std::ptrdiff_t>(remove));
        }
        if (keys.size() < max_curve_keys && ImGui::Button("+ Add color key")) {
            add_key_between_last(keys);
        }
    }
}

void SceneView::inspector_panel(Document& document, std::string_view issue) {
    if (!ImGui::Begin("Inspector")) {
        ImGui::End();
        return;
    }
    im::forget_drawn();
    if (selection_ != inspected_) {
        inspected_ = selection_;
        ImGui::SetScrollY(0.0F);
    }
    std::erase_if(selection_, [&](Uid uid) { return document.scene->item(uid) == nullptr; });
    SceneItem* item = document.scene->item(selected());
    const ImVec4 warn = caution();
    if (!issue.empty()) {
        im::heading("Issues");
        wrapped(std::string{issue}, &warn);
        ImGui::Spacing();
    }
    if (!item) {
        im_settings(document);
        im_live_ = ImGui::IsAnyItemActive();
        ImGui::End();
        return;
    }
    ImGui::PushID(static_cast<int>(item->uid));
    const float frame = ImGui::GetFrameHeight();
    if (selection_.size() == 1U) {
        ImGui::SetNextItemWidth(-frame - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##name", &item->name);
        const std::string fallback = default_item_name(*item);
        if (item->name != fallback) {
            ImGui::SameLine();
            if (ImGui::Button("\xE2\x86\x90##name_reset")) {
                item->name = fallback;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Reset name to %s", fallback.c_str());
            }
        }
    } else {
        ImGui::Text("%zu items selected", selection_.size());
    }
    if (ImGui::Button("Duplicate")) {
        const std::vector<Uid> selected = selection_;
        selection_.clear();
        for (const Uid uid : selected) {
            const Uid fresh = duplicate_item(document, uid);
            if (fresh) {
                selection_.push_back(fresh);
            }
        }
        ImGui::PopID();
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{0.55F, 0.16F, 0.16F, 1.0F});
    const bool deleted = ImGui::Button("Delete");
    ImGui::PopStyleColor();
    if (deleted) {
        for (const Uid uid : selection_) {
            remove_item(document, uid);
        }
        selection_.clear();
        ImGui::PopID();
        ImGui::End();
        return;
    }
    // Parent is organizational only; it never transforms the selection.
    std::vector<Uid> parents{0U};
    ControlDef parent = numeric_def("parent", "Group", 0, 0, 4096);
    parent.type = ControlType::Choice;
    parent.choices.push_back({"Scene", "Scene"});
    for (const SceneItem& candidate : document.scene->items) {
        bool descendant = false;
        for (const SceneItem* at = &candidate; at != nullptr; at = document.scene->item(at->parent)) {
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
    if (im::row(parent, parent_value, mixed_parent).changed) {
        for (const Uid uid : selection_) {
            static_cast<void>(group_item(document, uid, parents[static_cast<std::size_t>(parent_value.x)]));
        }
    }
    if (!std::holds_alternative<Group>(item->data)) {
        Value seed = number(static_cast<float>(item->seed));
        const bool mixed_seed = std::any_of(selection_.begin(), selection_.end(), [&](Uid uid) {
            return document.scene->item(uid)->seed != item->seed;
        });
        const im::Edit edit =
            im::row(numeric_def("seed", "Seed", static_cast<float>(item->uid), 0, 1000000, {}, 0), seed, mixed_seed);
        if (edit.changed) {
            for (const Uid uid : selection_) {
                document.scene->item(uid)->seed = edit.reset ? uid : static_cast<std::uint32_t>(seed.x);
            }
        }
    }
    std::string_view section;
    for (const SceneProperty& p : scene_properties(*item)) {
        if (!property_visible(*item, p)) {
            continue;
        }
        Value v = p.read(*item);
        bool common = true;
        bool mixed = false;
        unsigned mixed_axes = 0U;
        for (const Uid uid : selection_) {
            const SceneItem& target = *document.scene->item(uid);
            const auto props = scene_properties(target);
            const auto other = std::find_if(props.begin(), props.end(), [&](const SceneProperty& a) {
                return a.control.id == p.control.id && a.control.type == p.control.type;
            });
            if (other == props.end()) {
                common = false;
                break;
            }
            const Value other_value = other->read(target);
            mixed = mixed || other_value != v;
            if (other_value.x != v.x) { mixed_axes |= 1U; }
            if (other_value.y != v.y) { mixed_axes |= 2U; }
            if (other_value.z != v.z) { mixed_axes |= 4U; }
        }
        if (!common) {
            continue;
        }
        if (section != p.section) {
            if (selection_.size() == 1U && section == "Motion") {
                im_motion_stack(document, *item);
            }
            section = p.section;
            im::heading(section);
        }
        const im::Edit edit = im::row(p.control, v, mixed, mixed_axes);
        if (edit.changed) {
            for (const Uid uid : selection_) {
                SceneItem& target = *document.scene->item(uid);
                const auto props = scene_properties(target);
                const auto other = std::find_if(props.begin(), props.end(), [&](const SceneProperty& a) {
                    return a.control.id == p.control.id && a.control.type == p.control.type;
                });
                if (other != props.end()) {
                    Value applied = v;
                    if (p.control.type == ControlType::Vec3) {
                        const Value before = other->read(target);
                        if ((edit.axes & 1U) == 0U) { applied.x = before.x; }
                        if ((edit.axes & 2U) == 0U) { applied.y = before.y; }
                        if ((edit.axes & 4U) == 0U) { applied.z = before.z; }
                    }
                    // A name the artist has not chosen follows what it names.
                    const std::string was = default_item_name(target);
                    other->write(target, applied);
                    if (target.name == was) {
                        target.name = default_item_name(target);
                    }
                }
            }
        }
        if (selection_.size() == 1U && p.control.id == "shadowed") {
            im_over_age(*item);
        }
        if (selection_.size() == 1U && p.control.id == "solve") {
            im_grid_cells(*item);
        }
    }
    if (selection_.size() == 1U && section == "Motion") {
        im_motion_stack(document, *item);
    }
    if (selection_.size() == 1U) {
        im_sources(document, *item);
        im_targets(document, *item);
        im_relationships(document, item);
    }
    ImGui::PopID();
    im_live_ = ImGui::IsAnyItemActive();
    ImGui::End();
}

} // namespace ajfx::studio
