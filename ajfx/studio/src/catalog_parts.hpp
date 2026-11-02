#pragma once

#include "catalog.hpp"

#include <array>
#include <string_view>
#include <utility>
#include <vector>

// The pieces the block catalog is assembled from (catalog.cpp): the builders
// each block's controls are written with, and one function a category of
// blocks is defined by. Internal to the catalog; nothing else includes this.
namespace ajfx::studio {
namespace catalog_controls {

inline ControlDef number_control(std::string_view id, std::string_view label, float fallback, float min, float max,
                          std::string_view unit, int decimals, std::string_view help, float hard_min = 0.0F,
                          float hard_max = 1.0e9F) {
    ControlDef control;
    control.id = id;
    control.label = label;
    control.type = ControlType::Number;
    control.fallback = number(fallback);
    control.min = min;
    control.max = max;
    control.hard_min = hard_min;
    control.hard_max = hard_max;
    control.unit = unit;
    control.decimals = decimals;
    control.help = help;
    return control;
}

inline ControlDef range_control(std::string_view id, std::string_view label, float low, float high, float min, float max,
                         std::string_view unit, int decimals, std::string_view help) {
    ControlDef control = number_control(id, label, 0.0F, min, max, unit, decimals, help);
    control.type = ControlType::Range;
    control.fallback = range(low, high);
    return control;
}

inline ControlDef vec3_control(std::string_view id, std::string_view label, Value fallback, std::string_view unit,
                        std::string_view help) {
    ControlDef control = number_control(id, label, 0.0F, -5.0F, 5.0F, unit, 2, help, -1.0e6F, 1.0e6F);
    control.type = ControlType::Vec3;
    control.fallback = std::move(fallback);
    return control;
}

// Three turns in degrees, round the upright axis, then tipped, then rolled.
// Whole degrees: a tenth of one is not a thing an artist means.
inline ControlDef angles_control(std::string_view id, std::string_view label, std::string_view help) {
    ControlDef control = number_control(id, label, 0.0F, -180.0F, 180.0F, "deg", 0, help, -3600.0F, 3600.0F);
    control.type = ControlType::Vec3;
    control.fallback = vec3(0.0F, 0.0F, 0.0F);
    return control;
}

inline ControlDef color_control(std::string_view id, std::string_view label, Value fallback, std::string_view help) {
    ControlDef control;
    control.id = id;
    control.label = label;
    control.type = ControlType::Color;
    control.fallback = std::move(fallback);
    control.hard_min = 0.0F;
    control.hard_max = 1.0F;
    control.help = help;
    return control;
}

inline ControlDef choice_control(std::string_view id, std::string_view label, std::string_view fallback,
                          std::vector<ChoiceDef> choices, std::string_view help) {
    ControlDef control;
    control.id = id;
    control.label = label;
    control.type = ControlType::Choice;
    control.fallback = choice(std::string{fallback});
    control.choices = std::move(choices);
    control.help = help;
    return control;
}

inline ControlDef toggle_control(std::string_view id, std::string_view label, bool fallback, std::string_view help) {
    ControlDef control;
    control.id = id;
    control.label = label;
    control.type = ControlType::Toggle;
    control.fallback = toggle(fallback);
    control.help = help;
    return control;
}

inline ControlDef file_control(std::string_view id, std::string_view label, FileKind files, std::string_view help) {
    ControlDef control;
    control.id = id;
    control.label = label;
    control.type = ControlType::File;
    control.fallback = choice({});
    control.files = files;
    control.help = help;
    return control;
}

// A control a particle layer has and a rigid layer does not: the physics
// engine turns and sizes its own bodies, and refuses what would kill or
// hold a particle, so these are not shown there and not stored (see
// conform_block).
inline ControlDef particles_only(ControlDef control) {
    control.layers = in_particles;
    return control;
}

// A force's strength in a rigid layer is the world's, not each body's: one
// number, with both ends together.
inline ControlDef one_strength_in_rigid(ControlDef control) {
    control.one_number = in_rigid;
    return control;
}

inline ControlDef publishable(ControlDef control) {
    control.publishable = true;
    return control;
}

// Published for a host to set as particles are drawn: the simulation never
// reads it, so a baked layer honours it as a live one does.
inline ControlDef publishable_at_playback(ControlDef control) {
    control.publishable = true;
    control.timing = ParameterTiming::Playback;
    return control;
}

inline ControlDef detail(ControlDef control) {
    control.level = Level::Detail;
    return control;
}

inline ControlDef when(ControlDef control, std::string_view other, std::array<std::string_view, 3> values,
                bool negate = false) {
    control.shown_if = Condition{other, values, negate};
    return control;
}

// A force's strength: each particle feels its own, picked between the two
// and kept for life, so some fall heavier or drift further than others.
inline ControlDef strength_control(std::string_view id, std::string_view label, float fallback, float min, float max,
                            std::string_view unit, int decimals, std::string_view help, float hard_min = 0.0F) {
    ControlDef control = range_control(id, label, fallback, fallback, min, max, unit, decimals, help);
    control.hard_min = hard_min;
    return control;
}

// Under More settings on every block that picks values at random: another
// number deals each particle different values, for a different look with
// the same settings.
inline ControlDef seed_control() {
    return detail(number_control("seed", "Seed", 0.0F, 0.0F, 100.0F, "", 0,
                                 "Another random look. 0 is the default.",
                                 0.0F, 1.0e6F));
}

inline ControlDef spread_control() {
    ControlDef control = publishable(range_control(
        "spread", "Spread", 0.0F, 20.0F, 0.0F, 180.0F, "deg", 0,
        "Angle from the direction, between the two."));
    control.hard_max = 180.0F;
    control.number_is_high = true;
    return control;
}

// What a Stream's or Burst's pieces are in a rigid layer, if not the layer's
// own: a model each, which they look like and hit things as, the way an
// Object's is. Only a rigid layer has bodies to be a model of, so a particle
// layer neither shows these nor keeps them.
inline std::vector<ControlDef> piece_model_controls() {
    ControlDef model = choice_control("model", "Model", "layer",
                                      {{"layer", "The layer's own"},
                                       {"cube", "Cube"},
                                       {"rock", "Rock"},
                                       {"slab", "Slab"},
                                       {"shard", "Shard"},
                                       {"file", "From a file"}},
                                      "Shape of each piece. The layer's own uses Appearance.");
    model.layers = in_rigid;
    ControlDef file = when(file_control("model_file", "Model file", FileKind::Model,
                                        "OBJ, glTF or GLB, fitted to 1 m."),
                           "model", {"file"});
    file.layers = in_rigid;
    return {std::move(model), std::move(file)};
}

// Which of the page's solid layers a block born of solid things reads: one
// by name, or every one (0), which is where it starts.
inline ControlDef solid_layer_control() {
    ControlDef control;
    control.id = "solid";
    control.label = "From";
    control.type = ControlType::SolidLayer;
    control.fallback = number(0.0F);
    control.hard_min = 0.0F;
    control.hard_max = 1.0e9F;
    control.help = "Which solid layer. All by default.";
    return control;
}

// A block's own controls, then the rest.
inline std::vector<ControlDef> with_controls(std::vector<ControlDef> controls, std::vector<ControlDef> more) {
    for (ControlDef& control : more) {
        controls.push_back(std::move(control));
    }
    return controls;
}

} // namespace catalog_controls

// Each appends its category's blocks, in the order the palette lists them.
void add_birth_blocks(std::vector<BlockDef>& blocks);
void add_motion_blocks(std::vector<BlockDef>& blocks);
void add_look_blocks(std::vector<BlockDef>& blocks);
void add_contact_blocks(std::vector<BlockDef>& blocks);
void add_rigid_blocks(std::vector<BlockDef>& blocks);
void add_expert_blocks(std::vector<BlockDef>& blocks);

} // namespace ajfx::studio
