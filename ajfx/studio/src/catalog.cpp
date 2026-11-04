#include "catalog.hpp"

#include "catalog_parts.hpp"
#include "raw_modules.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace ajfx::studio {
namespace {

std::vector<BlockDef> make_catalog() {
    std::vector<BlockDef> blocks;
    add_birth_blocks(blocks);
    add_motion_blocks(blocks);
    add_look_blocks(blocks);
    add_contact_blocks(blocks);
    add_rigid_blocks(blocks);
    add_expert_blocks(blocks);
    return blocks;
}

} // namespace

std::string_view category_name(Category category) noexcept {
    switch (category) {
    case Category::Birth: return "Birth";
    case Category::Motion: return "Motion";
    case Category::Look: return "Look";
    case Category::Contact: return "Contact";
    case Category::Physics: return "Rigid physics";
    case Category::Light: return "Light";
    case Category::Expert: return "Under the hood";
    }
    return "";
}

std::string_view rigid_scope_name(RigidScope scope) noexcept {
    switch (scope) {
    case RigidScope::Objects: return "What is in it";
    case RigidScope::Pieces: return "The pieces";
    case RigidScope::Breaking: return "When they break";
    case RigidScope::World: return "The world";
    case RigidScope::Hood: return "Under the hood";
    }
    return "";
}

std::string_view rigid_scope_help(RigidScope scope) noexcept {
    switch (scope) {
    case RigidScope::Objects:
        return "Things standing in this layer.";
    case RigidScope::Pieces:
        return "The layer's own pieces.";
    case RigidScope::Breaking:
        return "What happens when a piece is hit hard.";
    case RigidScope::World:
        return "What every piece shares.";
    case RigidScope::Hood:
        return "Raw modules.";
    }
    return "";
}

RigidScope rigid_scope(BlockKind kind) noexcept {
    switch (kind) {
    case BlockKind::Object:
        return RigidScope::Objects;
    // Breaking belongs to the Object above it, so the page draws these
    // under their Object rather than under a heading of their own. The
    // scope is what a Break with no Object over it reads under, which the
    // coach then asks about.
    case BlockKind::Break:
    case BlockKind::Part:
        return RigidScope::Breaking;
    // A force in a rigid layer is the world's gravity or a body's damping,
    // and a collider becomes an unmoving body in the world, so both are the
    // world's rather than each piece's (see the rigid builder's
    // world_strength). The world block is what every piece is made of, so
    // it reads here too.
    case BlockKind::Gravity:
    case BlockKind::Rise:
    case BlockKind::Wind:
    case BlockKind::Drag:
    case BlockKind::Ground:
    case BlockKind::Sphere:
    case BlockKind::Box:
    case BlockKind::MeshCollider:
    case BlockKind::PassesThrough:
    case BlockKind::Physics:
    // An explosion throws everything in every world, so it reads with the
    // world rather than with any one piece.
    case BlockKind::Explosion:
    case BlockKind::Push:
    case BlockKind::SmokeGrid:
        return RigidScope::World;
    case BlockKind::Module:
        return RigidScope::Hood;
    // Spawning, placing, launching, tumbling and every look: the pieces'.
    case BlockKind::Stream:
    case BlockKind::Burst:
    case BlockKind::Shape:
    case BlockKind::Launch:
    case BlockKind::Stirred:
    case BlockKind::HitPuffs:
    case BlockKind::HitSparks:
    case BlockKind::Trail:
    case BlockKind::Turbulence:
    case BlockKind::Swirl:
    case BlockKind::DrawIn:
    case BlockKind::Attract:
    case BlockKind::Appearance:
    case BlockKind::Color:
    case BlockKind::Size:
    case BlockKind::Spin:
    case BlockKind::Fade:
    case BlockKind::Heat:
    case BlockKind::Tumble:
    case BlockKind::Light:
        return RigidScope::Pieces;
    }
    return RigidScope::Pieces;
}

RigidScope rigid_scope(std::string_view block_kind) noexcept {
    const std::optional<BlockKind> kind = block_kind_from(block_kind);
    return kind ? rigid_scope(*kind) : RigidScope::Pieces;
}

const std::vector<BlockDef>& block_catalog() {
    static const std::vector<BlockDef> catalog = make_catalog();
    return catalog;
}

const BlockDef* find_block_def(BlockKind kind) {
    // Indexed by kind, so a lookup is not a search of the catalog.
    static const std::array<const BlockDef*, block_kind_count> index = [] {
        std::array<const BlockDef*, block_kind_count> found{};
        for (const BlockDef& def : block_catalog()) {
            found[static_cast<std::size_t>(def.kind)] = &def;
        }
        return found;
    }();
    return index[static_cast<std::size_t>(kind)];
}

const BlockDef* find_block_def(std::string_view kind) {
    const std::optional<BlockKind> known = block_kind_from(kind);
    return known ? find_block_def(*known) : nullptr;
}

const ControlDef* find_control(const BlockDef& def, std::string_view id) {
    for (const ControlDef& control : def.controls) {
        if (control.id == id) {
            return &control;
        }
    }
    return nullptr;
}

ParameterTiming control_timing(std::string_view block_kind, std::string_view control) {
    if (const BlockDef* def = find_block_def(block_kind)) {
        if (const ControlDef* found = find_control(*def, control)) {
            return found->timing;
        }
    }
    return ParameterTiming::Bake;
}

Value value_of(const Block& block, const ControlDef& control) {
    const Value* stored = block.find(control.id);
    return stored != nullptr ? *stored : control.fallback;
}

Value get(const Block& block, std::string_view control) {
    const BlockDef* def = find_block_def(block.kind);
    const ControlDef* found = def != nullptr ? find_control(*def, control) : nullptr;
    if (found == nullptr) {
        const Value* stored = block.find(control);
        return stored != nullptr ? *stored : Value{};
    }
    return value_of(block, *found);
}

float number_of(const Block& block, std::string_view control) {
    return get(block, control).x;
}

std::string choice_of(const Block& block, std::string_view control) {
    return get(block, control).text;
}

bool toggle_of(const Block& block, std::string_view control) {
    return get(block, control).x != 0.0F;
}

Value sanitize(const ControlDef& control, Value value) {
    const auto clamp = [&](float v) {
        return std::isfinite(v) ? std::clamp(v, control.hard_min, control.hard_max) : control.fallback.x;
    };
    switch (control.type) {
    case ControlType::Number:
        return number(clamp(value.x));
    case ControlType::Range: {
        const float low = clamp(value.x);
        return range(low, std::max(low, clamp(value.y)));
    }
    case ControlType::Vec3:
        return vec3(clamp(value.x), clamp(value.y), clamp(value.z));
    case ControlType::Color:
        return color(std::clamp(value.x, 0.0F, 1.0F), std::clamp(value.y, 0.0F, 1.0F),
                     std::clamp(value.z, 0.0F, 1.0F), std::clamp(value.w, 0.0F, 1.0F));
    case ControlType::Choice:
        for (const ChoiceDef& option : control.choices) {
            if (option.id == value.text) {
                return choice(std::move(value.text));
            }
        }
        return control.fallback;
    case ControlType::Toggle:
        return toggle(value.x != 0.0F);
    case ControlType::File:
        return choice(std::move(value.text));
    case ControlType::SolidLayer:
        // A uid, whole; 0 is every solid layer.
        return number(std::isfinite(value.x) ? std::max(std::round(value.x), 0.0F) : 0.0F);
    }
    return value;
}

std::string file_of(const Block& block, std::string_view control) {
    return get(block, control).text;
}

std::string file_label(std::string_view path) {
    if (path.empty()) {
        return "no file";
    }
    return std::filesystem::path{std::string{path}}.filename().string();
}

bool block_fits(BlockKind block_kind, LayerKind layer) noexcept {
    const BlockDef* def = find_block_def(block_kind);
    return def != nullptr && fits(def->layers, layer);
}

bool block_fits(std::string_view block_kind, LayerKind layer) noexcept {
    const BlockDef* def = find_block_def(block_kind);
    return def != nullptr && fits(def->layers, layer);
}

std::vector<ChoiceDef> choices_in(const ControlDef& control, LayerKind layer) {
    std::vector<ChoiceDef> out;
    for (const ChoiceDef& option : control.choices) {
        if (fits(option.layers, layer)) {
            out.push_back(option);
        }
    }
    return out;
}

void conform_block(Block& block, LayerKind layer) {
    const BlockDef* def = find_block_def(block.kind);
    if (def == nullptr) {
        return;
    }
    // An Appearance in a rigid physics layer draws models unless the artist
    // asks for Glow. The layer is made of solid things, and an Object's
    // model is drawn only where the layer draws models at all, which is not
    // something to have to discover. It is set here rather than left to the
    // generic fallback below, which would land on the first material on the
    // list and draw a crate as a glowing dot.
    if (block.kind == BlockKind::Appearance && layer == LayerKind::Rigid) {
        const Value* held = block.find("material");
        if (held == nullptr || (held->text != "glow" && held->text != "model")) {
            block.set("material", choice("model"));
        }
    }
    for (const ControlDef& control : def->controls) {
        if (!fits(control.layers, layer)) {
            block.clear(control.id);
            block.unpublish(control.id);
            continue;
        }
        if (control.type == ControlType::Choice) {
            // A choice standing on an option this kind does not offer falls
            // back, as it does for an option the catalog has dropped.
            const std::vector<ChoiceDef> offered = choices_in(control, layer);
            const Value held = value_of(block, control);
            const bool on_list = std::any_of(offered.begin(), offered.end(),
                                             [&](const ChoiceDef& option) { return option.id == held.text; });
            if (!on_list) {
                block.clear(control.id);
                if (!offered.empty() &&
                    std::none_of(offered.begin(), offered.end(),
                                 [&](const ChoiceDef& option) { return option.id == value_of(block, control).text; })) {
                    block.set(control.id, choice(std::string{offered.front().id}));
                }
            }
        }
        if (control.type == ControlType::Range && fits(control.one_number, layer)) {
            // One number here: the high end is what the artist last typed,
            // so both ends take it and the reading tells the truth.
            const Value held = value_of(block, control);
            if (held.x != held.y) {
                block.set(control.id, range(held.y, held.y));
            }
        }
    }
}

bool control_visible(const Block& block, const BlockDef& def, const ControlDef& control, LayerKind layer) {
    // A setting this kind of layer has no use for is not shown at all: the
    // runtime would ignore it or refuse the layer over it.
    if (!fits(control.layers, layer)) {
        return false;
    }
    if (!control.shown_if) {
        return true;
    }
    const Condition& condition = *control.shown_if;
    const ControlDef* other = find_control(def, condition.control);
    if (other == nullptr) {
        return true;
    }
    // A control hidden by its own condition hides what depends on it.
    if (!control_visible(block, def, *other, layer)) {
        return false;
    }
    const Value value = value_of(block, *other);
    const std::string reading = other->type == ControlType::Toggle ? (value.x != 0.0F ? "on" : "off") : value.text;
    bool matched = false;
    for (std::string_view candidate : condition.values) {
        matched = matched || (!candidate.empty() && candidate == reading);
    }
    return matched != condition.negate;
}

std::string format_range(const Value& range, int decimals, std::string_view unit) {
    std::string text = format_number(range.x, decimals);
    if (format_number(range.y, decimals) != text) {
        text += " to " + format_number(range.y, decimals);
    }
    if (!unit.empty()) {
        if (unit != "x" && unit != "/s" && unit != "%") {
            text += ' ';
        }
        text += unit;
    }
    return text;
}

std::string format_number(float value, int decimals) {
    char buffer[48];
    std::snprintf(buffer, sizeof(buffer), "%.*f", std::max(decimals, 0), static_cast<double>(value));
    std::string text = buffer;
    // Trailing zeros say nothing: "1.50" reads "1.5", "2.00" reads "2".
    if (text.find('.') != std::string::npos) {
        while (!text.empty() && text.back() == '0') {
            text.pop_back();
        }
        if (!text.empty() && text.back() == '.') {
            text.pop_back();
        }
    }
    return text == "-0" ? std::string{"0"} : text;
}

namespace {

std::string with_unit(float value, int decimals, std::string_view unit) {
    std::string text = format_number(value, decimals);
    if (!unit.empty()) {
        if (unit != "x" && unit != "/s" && unit != "%") {
            text += ' ';
        }
        text += unit;
    }
    return text;
}

std::string choice_label(const Block& block, std::string_view control) {
    const BlockDef* def = find_block_def(block.kind);
    const ControlDef* found = def != nullptr ? find_control(*def, control) : nullptr;
    if (found == nullptr) {
        return {};
    }
    const std::string id = value_of(block, *found).text;
    for (const ChoiceDef& option : found->choices) {
        if (option.id == id) {
            return std::string{option.label};
        }
    }
    return id;
}

// ", Rock" after a Stream's or Burst's summary when its pieces are a model of
// their own; nothing while they are the layer's.
std::string piece_model_suffix(const Block& block) {
    const std::string model = choice_of(block, "model");
    if (model == "layer") {
        return {};
    }
    if (model == "file") {
        const std::string label = file_label(file_of(block, "model_file"));
        return ", " + (label.empty() ? std::string{"no model yet"} : label);
    }
    return ", " + choice_label(block, "model");
}

// "Glass" -> "glass", to sit inside a reading.
std::string lower_first(std::string text) {
    if (!text.empty()) {
        text[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[0])));
    }
    return text;
}

} // namespace

std::string block_title(const Block& block) {
    if (block.kind == BlockKind::Module && block.raw) {
        return std::string{raw_title(*block.raw)};
    }
    const BlockDef* def = find_block_def(block.kind);
    return def != nullptr ? std::string{def->title} : block.kind;
}

std::string block_summary(const Block& block) {
    const std::string_view kind = block.kind;
    if (kind == BlockKind::Stream) {
        return format_number(number_of(block, "rate"), 0) + " a second" + piece_model_suffix(block);
    }
    if (kind == BlockKind::Burst) {
        const std::string count = format_number(number_of(block, "count"), 0);
        const float every = number_of(block, "every");
        return (every > 0.0F ? count + " every " + with_unit(every, 2, "s") : count + " at once") +
               piece_model_suffix(block);
    }
    if (kind == BlockKind::Shape) {
        const std::string shape = choice_label(block, "shape");
        const std::string id = choice_of(block, "shape");
        if (id == "point") {
            return shape;
        }
        if (id == "box") {
            const Value box = get(block, "box");
            return shape + ", " + format_number(box.x, 2) + " x " + format_number(box.y, 2) + " x " +
                   with_unit(box.z, 2, "m");
        }
        return shape + ", " + format_range(get(block, "size"), 2, "m");
    }
    if (kind == BlockKind::HitPuffs || kind == BlockKind::HitSparks) {
        std::string text;
        if (number_of(block, "per_hit") > 0.0F) {
            text = format_number(number_of(block, "per_hit"), 0) + " a hit + ";
        }
        return text + format_number(number_of(block, "per_speed"), 1) + " per m/s, over " +
               with_unit(number_of(block, "hardness"), 1, "m/s");
    }
    if (kind == BlockKind::Stirred) {
        return format_range(get(block, "strength"), 1, "") + ", reaching " +
               format_number(number_of(block, "reach"), 1) + " half sizes";
    }
    if (kind == BlockKind::Trail) {
        return format_number(number_of(block, "per_speed"), 1) + " per m/s" +
               (toggle_of(block, "pieces") ? ", broken pieces" : "");
    }
    if (kind == BlockKind::Heat) {
        return "Heat " + format_number(number_of(block, "start"), 2) + ", halves in " +
               with_unit(number_of(block, "half_life"), 2, "s") +
               (choice_of(block, "what") == "pieces" ? ", broken pieces" : "");
    }
    if (kind == BlockKind::Launch) {
        std::string text = choice_label(block, "direction") + " at " + format_range(get(block, "speed"), 2, "m/s");
        if (choice_of(block, "direction") != "out") {
            text += ", " + format_range(get(block, "spread"), 0, "deg") + " spread";
        }
        return text;
    }
    if (kind == BlockKind::Gravity) {
        return format_range(get(block, "strength"), 2, "x") + " the page's";
    }
    if (kind == BlockKind::Rise) {
        return format_range(get(block, "strength"), 1, "m/s^2");
    }
    if (kind == BlockKind::Wind) {
        return format_range(get(block, "strength"), 1, "m/s^2") + " toward " +
               with_unit(number_of(block, "heading"), 0, "deg");
    }
    if (kind == BlockKind::Turbulence) {
        std::string text = "Amount " + format_range(get(block, "strength"), 2, "") + ", swirls " +
                           with_unit(number_of(block, "size"), 2, "m");
        if (number_of(block, "roll") > 0.0F) {
            text += ", rolling";
        }
        return text;
    }
    if (kind == BlockKind::DrawIn) {
        std::string text = "Pull " + format_range(get(block, "strength"), 1, "");
        if (number_of(block, "height") > 0.0F) {
            text += ", fades by " + with_unit(number_of(block, "height"), 2, "m");
        }
        return text;
    }
    if (kind == BlockKind::Spin) {
        const Value speed = get(block, "speed");
        return format_number(speed.x, 0) + " to " + with_unit(speed.y, 0, "deg/s");
    }
    if (kind == BlockKind::Drag) {
        return format_range(get(block, "amount"), 2, "");
    }
    if (kind == BlockKind::Swirl) {
        return choice_label(block, "axis") + ", " + format_range(get(block, "strength"), 1, "m/s^2");
    }
    if (kind == BlockKind::Attract) {
        const Value strength = get(block, "strength");
        if (strength.y <= 0.0F) {
            return "Push " + format_range(range(-strength.y, -strength.x), 1, "m/s^2");
        }
        return (strength.x < 0.0F ? "Pull or push " : "Pull ") + format_range(strength, 1, "m/s^2");
    }
    if (kind == BlockKind::Appearance) {
        const std::string material = choice_of(block, "material");
        if (material == "model") {
            return "Model, " + (choice_of(block, "mesh") == "file" ? file_label(file_of(block, "mesh_file"))
                                                                  : choice_label(block, "mesh"));
        }
        return choice_label(block, "material") + ", " +
               (choice_of(block, "texture") == "file" ? file_label(file_of(block, "texture_file"))
                                                      : choice_label(block, "texture"));
    }
    if (kind == BlockKind::Color) {
        std::string text = choice_label(block, "over_life");
        const Value brightness = get(block, "brightness");
        if (brightness.x != 1.0F || brightness.y != 1.0F) {
            text += ", brightness " + format_range(brightness, 1, "");
        }
        return text;
    }
    if (kind == BlockKind::Size) {
        const Value size = get(block, "size");
        return format_number(size.x, 3) + " to " + with_unit(size.y, 3, "m") + ", " + choice_label(block, "over_life");
    }
    if (kind == BlockKind::Fade) {
        return "In " + format_number(number_of(block, "in"), 0) + "%, out " + format_number(number_of(block, "out"), 0) +
               "%";
    }
    if (kind == BlockKind::Ground) {
        const std::string response = choice_of(block, "response");
        return response == "bounce" ? "Bounce " + format_number(number_of(block, "bounce"), 2)
                                    : choice_label(block, "response");
    }
    if (kind == BlockKind::Sphere) {
        return (toggle_of(block, "inside") ? "Inside, " : "Outside, ") + with_unit(number_of(block, "radius"), 2, "m");
    }
    if (kind == BlockKind::Box) {
        return toggle_of(block, "inside") ? "Keep inside" : "Keep out";
    }
    if (kind == BlockKind::MeshCollider) {
        return file_label(file_of(block, "file")) + (toggle_of(block, "inside") ? ", keep inside" : ", keep out");
    }
    if (kind == BlockKind::Tumble) {
        const Value spin = get(block, "spin");
        if (spin.y <= 0.0F) {
            return "Turned every way, no spin";
        }
        return format_number(spin.x, 0) + " to " + with_unit(spin.y, 0, "deg/s");
    }
    if (kind == BlockKind::Physics) {
        return choice_label(block, "shape") + ", friction " + format_number(number_of(block, "friction"), 2) +
               ", bounce " + format_number(number_of(block, "bounce"), 2);
    }
    if (kind == BlockKind::Break) {
        std::string hit = "over " + with_unit(number_of(block, "force"), 1, "m/s");
        if (number_of(block, "blast") > 0.0F) {
            hit += ", blasted over " + with_unit(number_of(block, "blast"), 1, "m/s");
        }
        if (choice_of(block, "into") == "cut") {
            return "Cut into " + format_number(number_of(block, "cells"), 0) + ", " + hit;
        }
        return "Into the parts below, " + hit;
    }
    if (kind == BlockKind::Object) {
        const std::string model =
            choice_of(block, "model") == "file" ? file_label(file_of(block, "model_file")) : choice_label(block, "model");
        const std::string arrange = choice_of(block, "arrange");
        std::string text = (model.empty() ? std::string{"No model"} : model) + ", " +
                           (arrange == "one"      ? std::string{"one"}
                            : arrange == "placed" ? std::string{"placed by hand"}
                                             : choice_label(block, "arrange") + " of " +
                                                   format_number(number_of(block, "count"), 0)) +
                           ", " + with_unit(number_of(block, "size"), 2, "m");
        if (choice_of(block, "material") != "layer") {
            text += ", " + lower_first(choice_label(block, "material"));
        }
        if (arrange == "one" && number_of(block, "appear") > 0.0F) {
            text += ", appears at " + with_unit(number_of(block, "appear"), 2, "s");
        }
        return text;
    }
    if (kind == BlockKind::Explosion) {
        return with_unit(number_of(block, "power"), 1, "m/s") + ", reaching " +
               with_unit(number_of(block, "radius"), 2, "m") + ", at " + with_unit(number_of(block, "time"), 2, "s");
    }
    if (kind == BlockKind::Push) {
        return with_unit(number_of(block, "power"), 1, "m/s") + ", reaching " +
               with_unit(number_of(block, "radius"), 2, "m") + ", at " + with_unit(number_of(block, "time"), 2, "s");
    }
    if (kind == BlockKind::Part) {
        const std::string model =
            choice_of(block, "model") == "file" ? file_label(file_of(block, "model_file")) : choice_label(block, "model");
        return (model.empty() ? std::string{"No model"} : model) + ", " +
               format_number(number_of(block, "size") * 100.0F, 0) + "% of the piece";
    }
    if (kind == BlockKind::PassesThrough) {
        return toggle_of(block, "ground") ? "Solid things and the ground" : "Solid things";
    }
    if (kind == BlockKind::Light) {
        const Value reach = get(block, "reach");
        return format_number(number_of(block, "count"), 0) + " lights, reach " + format_number(reach.x, 1) +
               " to " + with_unit(reach.y, 1, "m");
    }
    if (kind == BlockKind::Module && block.raw) {
        return std::string{raw_stage(*block.raw)} + ": " + std::string{raw_kind(*block.raw)};
    }
    return {};
}

} // namespace ajfx::studio
