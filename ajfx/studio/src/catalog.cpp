#include "catalog.hpp"

#include "raw_modules.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace ajfx::studio {
namespace {

ControlDef number_control(std::string_view id, std::string_view label, float fallback, float min, float max,
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

ControlDef range_control(std::string_view id, std::string_view label, float low, float high, float min, float max,
                         std::string_view unit, int decimals, std::string_view help) {
    ControlDef control = number_control(id, label, 0.0F, min, max, unit, decimals, help);
    control.type = ControlType::Range;
    control.fallback = range(low, high);
    return control;
}

ControlDef vec3_control(std::string_view id, std::string_view label, Value fallback, std::string_view unit,
                        std::string_view help) {
    ControlDef control = number_control(id, label, 0.0F, -5.0F, 5.0F, unit, 2, help, -1.0e6F, 1.0e6F);
    control.type = ControlType::Vec3;
    control.fallback = std::move(fallback);
    return control;
}

// Three turns in degrees, round the upright axis, then tipped, then rolled.
// Whole degrees: a tenth of one is not a thing an artist means.
ControlDef angles_control(std::string_view id, std::string_view label, std::string_view help) {
    ControlDef control = number_control(id, label, 0.0F, -180.0F, 180.0F, "deg", 0, help, -3600.0F, 3600.0F);
    control.type = ControlType::Vec3;
    control.fallback = vec3(0.0F, 0.0F, 0.0F);
    return control;
}

ControlDef color_control(std::string_view id, std::string_view label, Value fallback, std::string_view help) {
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

ControlDef choice_control(std::string_view id, std::string_view label, std::string_view fallback,
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

ControlDef toggle_control(std::string_view id, std::string_view label, bool fallback, std::string_view help) {
    ControlDef control;
    control.id = id;
    control.label = label;
    control.type = ControlType::Toggle;
    control.fallback = toggle(fallback);
    control.help = help;
    return control;
}

ControlDef file_control(std::string_view id, std::string_view label, FileKind files, std::string_view help) {
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
ControlDef particles_only(ControlDef control) {
    control.layers = in_particles;
    return control;
}

// A force's strength in a rigid layer is the world's, not each body's: one
// number, with both ends together.
ControlDef one_strength_in_rigid(ControlDef control) {
    control.one_number = in_rigid;
    return control;
}

ControlDef publishable(ControlDef control) {
    control.publishable = true;
    return control;
}

ControlDef detail(ControlDef control) {
    control.level = Level::Detail;
    return control;
}

ControlDef when(ControlDef control, std::string_view other, std::array<std::string_view, 3> values,
                bool negate = false) {
    control.shown_if = Condition{other, values, negate};
    return control;
}

// A force's strength: each particle feels its own, picked between the two
// and kept for life, so some fall heavier or drift further than others.
ControlDef strength_control(std::string_view id, std::string_view label, float fallback, float min, float max,
                            std::string_view unit, int decimals, std::string_view help, float hard_min = 0.0F) {
    ControlDef control = range_control(id, label, fallback, fallback, min, max, unit, decimals, help);
    control.hard_min = hard_min;
    return control;
}

// Under More settings on every block that picks values at random: another
// number deals each particle different values, for a different look with
// the same settings.
ControlDef seed_control() {
    return detail(number_control("seed", "Seed", 0.0F, 0.0F, 100.0F, "", 0,
                                 "Deals out different random values: the same settings, another look. 0 is the "
                                 "block's own.",
                                 0.0F, 1.0e6F));
}

ControlDef spread_control() {
    ControlDef control = publishable(range_control(
        "spread", "Spread", 0.0F, 20.0F, 0.0F, 180.0F, "deg", 0,
        "How far from the direction each particle goes, between the two: 0 to 20 is a narrow cone, 0 is a beam, "
        "180 every way. Raising the first hollows the cone, as a fountain's crown."));
    control.hard_max = 180.0F;
    control.number_is_high = true;
    return control;
}

std::vector<BlockDef> make_catalog() {
    std::vector<BlockDef> blocks;

    // ---------------------------------------------------------- birth
    blocks.push_back(BlockDef{
        .kind = "stream",
        .title = "Stream",
        .description = "A steady flow of new particles, so many each second.",
        .category = Category::Birth,
        .controls = {publishable(number_control("rate", "Amount", 30.0F, 0.0F, 500.0F, "/s", 0, "Particles born each second.",
                                    0.0F, 1.0e6F))},
        .keywords = "emit spawn rate continuous flow",
    });
    blocks.push_back(BlockDef{
        .kind = "burst",
        .title = "Burst",
        .description = "Many particles at once, once or again and again.",
        .category = Category::Birth,
        .controls =
            {number_control("count", "Count", 60.0F, 1.0F, 1000.0F, "", 0, "Particles in each burst.", 1.0F, 1.0e6F),
             number_control("time", "At", 0.0F, 0.0F, 10.0F, "s", 2, "When the first burst goes off."),
             number_control("every", "Repeat every", 0.0F, 0.0F, 10.0F, "s", 2,
                            "Seconds between bursts; 0 bursts once."),
             detail(number_control("repeats", "Bursts", 0.0F, 0.0F, 20.0F, "", 0,
                                   "How many bursts when repeating; 0 keeps going for ever.", 0.0F, 1.0e6F))},
        .keywords = "emit spawn explosion pop once",
    });
    blocks.push_back(BlockDef{
        .kind = "shape",
        .title = "Spawn shape",
        .description = "Where particles are born: a point, inside a ball, on a ring...",
        .category = Category::Birth,
        .controls = {choice_control("shape", "Shape", "ball",
                                    {{"point", "Point"},
                                     {"ball", "Ball"},
                                     {"shell", "Shell"},
                                     {"box", "Box"},
                                     {"disc", "Disc"},
                                     {"ring", "Ring"}},
                                    "Ball and Disc fill the shape; Shell and Ring use only its edge."),
                     when(publishable(range_control("size", "Radius", 0.2F, 0.2F, 0.0F, 3.0F, "m", 2,
                                                    "Each particle's own radius, picked between the two. Equal "
                                                    "ends give a crisp edge; apart, a ball's edge goes soft and a "
                                                    "shell or ring grows thick.")),
                          "shape", {"point", "box"}, true),
                     when(vec3_control("box", "Box size", vec3(0.4F, 0.4F, 0.4F), "m", "Width, height and depth."),
                          "shape", {"box"}),
                     detail(vec3_control("offset", "Offset", vec3(0.0F, 0.0F, 0.0F), "m",
                                         "Moves the shape away from the effect's origin. Drag its dot in the "
                                         "preview.")),
                     seed_control()},
        .keywords = "location position emitter area volume sphere circle",
        .unique = true,
    });
    blocks.push_back(BlockDef{
        .kind = "launch",
        .title = "Launch",
        .description = "The speed and direction particles set off with.",
        .category = Category::Birth,
        .controls = {choice_control("direction", "Direction", "up",
                                    {{"up", "Up"}, {"out", "Outward"}, {"down", "Down"}, {"side", "Sideways"},
                                     {"custom", "Custom"}},
                                    "Outward throws particles every way from where they are born."),
                     publishable(range_control("speed", "Speed", 1.0F, 2.0F, 0.0F, 20.0F, "m/s", 2,
                                   "Each particle's speed, between the two.")),
                     when(spread_control(), "direction", {"out"}, true),
                     when(detail(vec3_control("custom", "Aim", vec3(0.0F, 1.0F, 0.0F), "", "The direction to aim.")),
                          "direction", {"custom"}),
                     seed_control()},
        .keywords = "velocity speed throw shoot initial direction cone",
        .unique = true,
    });

    // --------------------------------------------------------- motion
    blocks.push_back(BlockDef{
        .kind = "gravity",
        .title = "Gravity",
        .description = "Pulls particles down, as heavy things fall.",
        .category = Category::Motion,
        .controls = {one_strength_in_rigid(strength_control("strength", "Strength", 1.0F, 0.0F, 3.0F, "g", 2,
                                      "1 is Earth's gravity; less for floaty things. Apart, some particles fall "
                                      "heavier than others.")),
                     seed_control()},
        .keywords = "fall weight down heavy",
    });
    blocks.push_back(BlockDef{
        .kind = "rise",
        .title = "Rise",
        .description = "Lifts particles, as heat carries flames and smoke upwards.",
        .category = Category::Motion,
        .controls = {one_strength_in_rigid(strength_control("strength", "Lift", 2.0F, 0.0F, 20.0F, "m/s^2", 1,
                                      "How hard heat pushes up, each particle between the two.")),
                     particles_only(detail(choice_control("over_life", "Over its life", "steady",
                                           {{"steady", "Steady"}, {"cools", "Eases as it cools"}, {"builds", "Builds up"}},
                                           "Hot flames shoot up and slow at the tip as they cool."))),
                     seed_control()},
        .keywords = "buoyancy heat float up hot",
    });
    blocks.push_back(BlockDef{
        .kind = "wind",
        .title = "Wind",
        .description = "A steady push to one side.",
        .category = Category::Motion,
        .controls = {one_strength_in_rigid(strength_control("strength", "Strength", 1.5F, 0.0F, 20.0F, "m/s^2", 1,
                                      "How hard it pushes, each particle between the two: light ones blow further.")),
                     number_control("heading", "Heading", 0.0F, 0.0F, 360.0F, "deg", 0,
                                    "Where it blows: 0 is along +X, 90 along -Z. Drag its arrow in the preview.",
                                    0.0F, 360.0F),
                     seed_control()},
        .keywords = "breeze push sideways direction",
    });
    blocks.push_back(BlockDef{
        .kind = "turbulence",
        .title = "Turbulence",
        .description = "Swirling, natural motion, like air stirring.",
        .category = Category::Motion,
        .controls = {publishable(strength_control("strength", "Amount", 1.5F, 0.0F, 10.0F, "", 2,
                                                  "How strongly the air churns, each particle between the two.")),
                     number_control("size", "Swirl size", 0.6F, 0.05F, 5.0F, "m", 2,
                                    "Small swirls make jittery motion; large ones slow rolling.", 0.01F, 1000.0F),
                     detail(publishable(number_control("churn", "Churn", 0.4F, 0.0F, 3.0F, "", 2,
                                           "How quickly the swirls themselves change."))),
                     detail(number_control("roll", "Rolls upward", 0.0F, 0.0F, 5.0F, "m/s", 2,
                                           "The swirls travel up at this speed, so particles rising about as fast "
                                           "ride the same eddy and curl, as in a real plume.",
                                           0.0F, 100.0F)),
                     detail(choice_control("over_life", "Over its life", "steady",
                                           {{"steady", "Steady"}, {"grows", "Calm, then breaks up"},
                                            {"settles", "Settles down"}},
                                           "Flames are smooth at the base and break up towards the tip.")),
                     seed_control()},
        .keywords = "noise curl swirl chaos organic flow rolling eddy plume",
        .layers = in_particles,
        .only_in = "Turbulence stirs each particle on its own; a physics engine moves whole bodies, so a rigid layer has no place for it.",
    });
    blocks.push_back(BlockDef{
        .kind = "drag",
        .title = "Air resistance",
        .description = "Slows particles down over time.",
        .category = Category::Motion,
        .controls = {publishable(one_strength_in_rigid(strength_control("amount", "Amount", 1.0F, 0.0F, 10.0F, "", 2,
                                                  "The share of speed lost each second, each particle between the "
                                                  "two: light ones slow sooner."))),
                     seed_control()},
        .keywords = "drag friction slow damping",
    });
    blocks.push_back(BlockDef{
        .kind = "swirl",
        .title = "Swirl",
        .description = "Spins particles around an axis, like a whirlwind.",
        .category = Category::Motion,
        .controls = {publishable(strength_control("strength", "Strength", 3.0F, -20.0F, 20.0F, "m/s^2", 1,
                                                  "Each particle between the two. Negative spins the other way.",
                                                  -1.0e6F)),
                     choice_control("axis", "Axis", "vertical", {{"vertical", "Vertical"}, {"horizontal", "Horizontal"}},
                                    "The axis particles turn around."),
                     detail(vec3_control("center", "Center", vec3(0.0F, 0.0F, 0.0F), "m",
                                         "Where the axis passes through. Drag its dot in the preview.")),
                     seed_control()},
        .keywords = "vortex tornado spin orbit whirl",
        .layers = in_particles,
        .only_in = "Swirl pulls each particle round a line; a physics engine moves whole bodies, so a rigid layer has no place for it.",
    });
    blocks.push_back(BlockDef{
        .kind = "draw_in",
        .title = "Draw in",
        .description = "Pulls particles in towards the middle near the base, as a fire draws in air, so flames "
                       "gather into tongues.",
        .category = Category::Motion,
        .controls = {strength_control("strength", "Pull", 6.0F, 0.0F, 30.0F, "/s^2", 1,
                                      "How hard particles are pulled towards the middle, for each metre away; each "
                                      "particle between the two."),
                     number_control("height", "Fades by", 1.0F, 0.0F, 5.0F, "m", 2,
                                    "The pull fades to nothing by this height. 0 pulls at every height.", 0.0F,
                                    1000.0F),
                     detail(vec3_control("center", "Center", vec3(0.0F, 0.0F, 0.0F), "m",
                                         "The base of the line particles are pulled to. Drag its dot in the "
                                         "preview.")),
                     seed_control()},
        .keywords = "converge inflow gather pinch tongue entrain fire",
        .unique = true,
        .layers = in_particles,
        .only_in = "Draw in gathers each particle towards a line; a physics engine moves whole bodies, so a rigid layer has no place for it.",
    });
    blocks.push_back(BlockDef{
        .kind = "attract",
        .title = "Attract",
        .description = "Pulls particles towards a point, or pushes them away.",
        .category = Category::Motion,
        .controls = {publishable(strength_control("strength", "Strength", 4.0F, -30.0F, 30.0F, "m/s^2", 1,
                                                  "Each particle between the two. Negative pushes particles away.",
                                                  -1.0e6F)),
                     vec3_control("point", "Point", vec3(0.0F, 1.0F, 0.0F), "m",
                                  "Where they are pulled to. Drag its dot in the preview."),
                     detail(publishable(number_control("reach", "Reach", 0.0F, 0.0F, 10.0F, "m", 2,
                                           "Beyond this distance there is no pull; 0 reaches everywhere."))),
                     seed_control()},
        .keywords = "attractor magnet pull push repel gather",
        .layers = in_particles,
        .only_in = "Attract pulls each particle to a point; a physics engine moves whole bodies, so a rigid layer has no place for it.",
    });

    // ----------------------------------------------------------- look
    blocks.push_back(BlockDef{
        .kind = "appearance",
        .title = "Appearance",
        .description = "What each particle looks like: glowing, smoky, solid or a chunk of debris.",
        .category = Category::Look,
        .controls =
            {choice_control("material", "Material", "glow",
                            {{"glow", "Glow"}, {"smoke", "Smoke"}, {"solid", "Solid"}, {"debris", "Debris"},
                             {"trail", "Trail"}},
                            "Glow adds light (fire, sparks, magic). Smoke and Solid cover what is behind. Debris "
                            "draws solid chunks. Trail draws one ribbon through the layer's particles."),
             when(choice_control("texture", "Texture", "dot",
                                 {{"dot", "Soft dot"},
                                  {"glow", "Hot glow"},
                                  {"spark", "Spark"},
                                  {"flame", "Flame"},
                                  {"smoke", "Smoke puff"},
                                  {"wisp", "Wisp"},
                                  {"star", "Star"},
                                  {"ring", "Ring"},
                                  {"file", "From a file"}},
                                 "The picture each particle carries. Flame and Smoke puff animate over its life. "
                                 "From a file uses a picture of your own."),
                  "material", {"debris"}, true),
             when(file_control("texture_file", "Picture", FileKind::Image,
                               "A PNG, JPEG, TGA or BMP. Its alpha shapes each particle; the export carries the "
                               "picture, so the media server needs no copy of the file."),
                  "texture", {"file"}),
             when(number_control("columns", "Frames across", 1.0F, 1.0F, 16.0F, "", 0,
                                 "For a flipbook: how many frames the picture holds side by side. They play left "
                                 "to right, top to bottom over each particle's life.",
                                 1.0F, 64.0F),
                  "texture", {"file"}),
             when(number_control("rows", "Frames down", 1.0F, 1.0F, 16.0F, "", 0,
                                 "For a flipbook: how many rows of frames the picture holds.", 1.0F, 64.0F),
                  "texture", {"file"}),
             when(choice_control("mesh", "Chunk", "chip",
                                 {{"chip", "Chip"}, {"shard", "Shard"}, {"rock", "Rock"}, {"cube", "Cube"},
                                  {"file", "From a file"}},
                                 "The solid shape each piece of debris has. From a file uses a model of your own."),
                  "material", {"debris"}),
             when(file_control("mesh_file", "Model", FileKind::Model,
                               "An OBJ, glTF or GLB. It is centred and scaled to fit a 1 m box, so Size sets how "
                               "big each piece is."),
                  "mesh", {"file"}),
             when(choice_control("facing", "Facing", "camera",
                                 {{"camera", "Face camera"}, {"motion", "Stretch along motion"}},
                                 "Stretching along motion turns sparks and rain into streaks."),
                  "material", {"debris", "trail"}, true),
             when(number_control("stretch", "Stretch", 0.04F, 0.0F, 0.3F, "s", 3,
                                 "How long a streak is: the distance travelled in this many seconds."),
                  "facing", {"motion"}),
             particles_only(when(toggle_control("spin", "Random spin", false,
                                                "Turns each particle's picture to a random angle."),
                                 "material", {"debris", "trail"}, true)),
             detail(when(number_control("soft", "Soft edges", 0.15F, 0.0F, 1.0F, "m", 2,
                                        "Fades particles out as they meet the floor or objects, instead of a hard "
                                        "line."),
                         "material", {"debris"}, true)),
             detail(when(toggle_control("sort", "Depth sort", true,
                                        "Draws smoke and solid particles back to front. Glow never needs it."),
                         "material", {"smoke", "solid"})),
             detail(when(toggle_control("blend", "Smooth animation", true,
                                        "Cross-fades between a flipbook's frames."),
                         "texture", {"flame", "smoke", "file"})),
             when(seed_control(), "spin", {"on"})},
        .keywords = "material texture sprite render look blend mesh ribbon trail additive",
        .unique = true,
        .detachable = false,
    });
    blocks.push_back(BlockDef{
        .kind = "color",
        .title = "Color",
        .description = "The colour and brightness particles have, and how it changes as they age.",
        .category = Category::Look,
        .controls =
            {publishable(color_control("color", "Color", color(1.0F, 0.55F, 0.2F), "The colour each particle starts with.")),
             range_control("brightness", "Brightness", 1.0F, 1.0F, 0.0F, 20.0F, "x", 1,
                           "Each particle's, picked between the two. Above 1, glowing particles bloom and light up "
                           "their surroundings."),
             number_control("opacity", "Opacity", 1.0F, 0.0F, 1.0F, "", 2, "How solid each particle is.", 0.0F, 1.0F),
             choice_control("over_life", "As it ages", "constant",
                            {{"constant", "Stays"},
                             {"fire", "Burns out"},
                             {"cool", "Cools"},
                             {"darken", "Darkens"},
                             {"shift", "Shifts"}},
                            "Burns out runs hot white to yellow, orange and a dark ember."),
             when(color_control("end_color", "Becomes", color(0.35F, 0.1F, 0.8F), "The colour it shifts to."),
                  "over_life", {"shift"}),
             detail(toggle_control("mix", "Two colors", false, "Each particle picks a colour between two.")),
             detail(when(color_control("second", "Second color", color(1.0F, 0.8F, 0.35F),
                                       "The other end of the mix."),
                         "mix", {"on"})),
             seed_control()},
        .keywords = "colour tint hue brightness emissive gradient over life hdr",
        .unique = true,
    });
    blocks.push_back(BlockDef{
        .kind = "size",
        .title = "Size",
        .description = "How big particles are, and whether they grow or shrink.",
        .category = Category::Look,
        .controls = {publishable(range_control("size", "Size", 0.05F, 0.1F, 0.0F, 2.0F, "m", 3,
                                   "Each particle's size across, between the two.")),
                     particles_only(choice_control("over_life", "As it ages", "constant",
                                    {{"constant", "Stays"},
                                     {"grow", "Grows"},
                                     {"shrink", "Shrinks"},
                                     {"pop", "Pops"},
                                     {"swell", "Swells"}},
                                    "Pops grows quickly then shrinks away; Swells grows slowly.")),
                     particles_only(when(number_control("change", "Grows to", 3.0F, 1.0F, 10.0F, "x", 1,
                                         "How much bigger it ends than it starts.", 1.0F, 1000.0F),
                          "over_life", {"grow", "swell"})),
                     seed_control()},
        .keywords = "scale scale over life grow shrink big small",
        .unique = true,
    });
    blocks.push_back(BlockDef{
        .kind = "spin",
        .title = "Spin",
        .description = "Turns each particle's picture as it moves, slowing as it goes: flames twist, smoke rolls.",
        .category = Category::Look,
        .controls = {range_control("speed", "Speed", 30.0F, 120.0F, 0.0F, 720.0F, "deg/s", 0,
                                   "How fast each particle turns, picked between the two."),
                     detail(toggle_control("both_ways", "Both ways", true,
                                           "Half turn one way and half the other.")),
                     detail(number_control("slows", "Slows", 0.5F, 0.0F, 5.0F, "", 2,
                                           "The share of its spin each particle loses per second.")),
                     seed_control()},
        .keywords = "rotate rotation twist turn roll angular",
        .unique = true,
        .layers = in_particles,
        .only_in = "Spin turns flat sprites; a solid body turns as it tumbles, so a rigid layer has no place for it.",
    });
    blocks.push_back(BlockDef{
        .kind = "fade",
        .title = "Fade",
        .description = "Fades particles in when born and out before they die, so none pop.",
        .category = Category::Look,
        .controls = {number_control("in", "Fade in", 10.0F, 0.0F, 50.0F, "% of life", 0, "", 0.0F, 100.0F),
                     number_control("out", "Fade out", 40.0F, 0.0F, 100.0F, "% of life", 0, "", 0.0F, 100.0F)},
        .keywords = "alpha opacity transparency dissolve",
        .unique = true,
    });

    // -------------------------------------------------------- contact
    blocks.push_back(BlockDef{
        .kind = "ground",
        .title = "Ground",
        .description = "Particles hit the ground and bounce, slide, stick or vanish.",
        .category = Category::Contact,
        .controls = {choice_control("response", "On contact", "bounce",
                                    {{"bounce", "Bounce"}, {"slide", "Slide"}, {"stick", "Stick"},
                                     // A body leaves the world when its life ends, never on contact.
                                     {"vanish", "Vanish", in_particles}},
                                    "What a particle does when it reaches the ground."),
                     when(publishable(number_control("bounce", "Bounciness", 0.4F, 0.0F, 1.0F, "", 2,
                                         "1 bounces back as fast as it arrived.")),
                          "response", {"bounce"}),
                     when(publishable(number_control("friction", "Friction", 0.2F, 0.0F, 1.0F, "", 2,
                                         "How quickly sliding comes to a stop.", 0.0F, 1.0F)),
                          "response", {"bounce", "slide"}),
                     detail(number_control("height", "Height", 0.0F, -2.0F, 2.0F, "m", 2,
                                           "Where the ground is.", -1.0e6F))},
        .keywords = "floor collide collision plane bounce land",
    });
    blocks.push_back(BlockDef{
        .kind = "sphere",
        .title = "Sphere collider",
        .description = "Keeps particles out of a ball, or inside it.",
        .category = Category::Contact,
        .controls = {vec3_control("center", "Center", vec3(0.0F, 1.0F, 0.0F), "m", ""),
                     publishable(number_control("radius", "Radius", 0.5F, 0.0F, 5.0F, "m", 2, "")),
                     particles_only(toggle_control("inside", "Keep inside", false,
                                    "Holds particles within the ball instead.")),
                     publishable(number_control("bounce", "Bounciness", 0.4F, 0.0F, 1.0F, "", 2, "")),
                     detail(publishable(number_control("friction", "Friction", 0.1F, 0.0F, 1.0F, "", 2, "", 0.0F, 1.0F))),
                     particles_only(detail(toggle_control("vanish", "Vanish on contact", false, "")))},
        .keywords = "collide collision ball obstacle",
    });
    blocks.push_back(BlockDef{
        .kind = "box",
        .title = "Box collider",
        .description = "Keeps particles out of a box, or inside it.",
        .category = Category::Contact,
        .controls = {vec3_control("center", "Center", vec3(0.0F, 0.5F, 0.0F), "m", ""),
                     vec3_control("size", "Size", vec3(1.0F, 1.0F, 1.0F), "m", "Width, height and depth."),
                     particles_only(toggle_control("inside", "Keep inside", false,
                                    "Holds particles within the box instead.")),
                     publishable(number_control("bounce", "Bounciness", 0.4F, 0.0F, 1.0F, "", 2, "")),
                     detail(publishable(number_control("friction", "Friction", 0.1F, 0.0F, 1.0F, "", 2, "", 0.0F, 1.0F))),
                     particles_only(detail(toggle_control("vanish", "Vanish on contact", false, "")))},
        .keywords = "collide collision cube wall obstacle container",
    });
    blocks.push_back(BlockDef{
        .kind = "mesh_collider",
        .title = "Mesh collider",
        .description = "Particles bounce off a 3D model of your own: a table, rocks, a wall.",
        .category = Category::Contact,
        .controls = {file_control("file", "Model", FileKind::Model,
                                  "An OBJ, glTF or GLB. Only its shape is used: Studio turns it into a distance field "
                                  "the export carries, so the model itself is not in the effect."),
                     vec3_control("position", "Position", vec3(0.0F, 0.0F, 0.0F), "m",
                                  "Where the model's origin sits."),
                     number_control("scale", "Scale", 1.0F, 0.1F, 4.0F, "x", 2,
                                    "Multiplies the model's own size. Models in centimetres want 0.01.", 0.001F,
                                    1000.0F),
                     choice_control("solid", "Treat as", "auto",
                                    {{"auto", "Automatic"}, {"solid", "Solid"}, {"surface", "Thin surface"}},
                                    "Solid keeps particles out of the model's inside. A thin surface (a floor, "
                                    "terrain, an open shape) keeps them off both of its sides. Automatic is solid "
                                    "when the model is closed."),
                     toggle_control("inside", "Keep inside", false, "Holds particles within the model instead."),
                     publishable(number_control("bounce", "Bounciness", 0.4F, 0.0F, 1.0F, "", 2, "")),
                     detail(publishable(number_control("friction", "Friction", 0.1F, 0.0F, 1.0F, "", 2, "", 0.0F, 1.0F))),
                     detail(choice_control("detail", "Detail", "medium",
                                           {{"coarse", "Coarse"}, {"medium", "Medium"}, {"fine", "Fine"}},
                                           "How closely the field follows the model. Finer catches small parts and "
                                           "makes a larger export.")),
                     detail(toggle_control("vanish", "Vanish on contact", false, ""))},
        .keywords = "collide collision mesh model import obj gltf glb obstacle terrain sdf",
        // Its distance field is baked from the file; a Module could not name it.
        .detachable = false,
        .layers = in_particles,
        .only_in = "A rigid layer collides with a model by placing it as a prop, which is solid and exact.",
    });
    blocks.push_back(BlockDef{
        .kind = "tumble",
        .title = "Tumble",
        .description = "Particles become small solid bodies that spin, roll and come to rest.",
        .category = Category::Contact,
        .controls = {publishable(range_control("spin", "Spin", 180.0F, 720.0F, 0.0F, 2000.0F, "deg/s", 0,
                                   "How fast each piece spins when thrown.")),
                     particles_only(publishable(number_control("roundness", "Roundness", 0.1F, 0.0F, 1.0F, "", 2,
                                                "0 for flat chips that slide to a stop, 1 for stones that roll.",
                                                0.0F, 1.0F))),
                     particles_only(detail(publishable(number_control("damping", "Spin damping", 1.0F, 0.0F, 10.0F, "", 2,
                                           "How quickly spinning slows.")))),
                     particles_only(detail(publishable(number_control("settle", "Settle below", 0.2F, 0.0F, 2.0F, "m/s", 2,
                                           "Slower contacts stop instead of bouncing, so piles settle.")))),
                     particles_only(detail(number_control("contact", "Contact size", 0.4F, 0.05F, 1.0F, "", 2,
                                           "The ball each piece collides as, as a share of its size."))),
                     seed_control()},
        .keywords = "rigid body debris physics rotate rolling",
        .unique = true,
    });
    // --------------------------------------------------- rigid physics
    blocks.push_back(BlockDef{
        .kind = "prop",
        .title = "Prop",
        .description = "An object you place in the scene for the pieces to land on, fall out of or knock over.",
        .category = Category::Physics,
        .controls =
            {file_control("model", "Model", FileKind::Model,
                          "The model to place. Its longest side becomes its size, so a bucket and a crate are "
                          "set up the same way."),
             choice_control("motion", "It can", "fixed",
                            {{"fixed", "Stay put"}, {"driven", "Be moved"}, {"loose", "Be knocked about"}},
                            "Stay put for a floor, a wall or a bucket that only holds; only a prop that stays "
                            "put can collide as its own shape. Be moved to tip or swing it yourself, shoving "
                            "whatever is in the way. Be knocked about to let the pieces move it."),
             vec3_control("at", "Position", vec3(0.0F, 0.0F, 0.0F), "m", "Where the middle of the prop sits."),
             angles_control("turn", "Turn", "How the prop is turned where it stands."),
             number_control("size", "Size", 1.0F, 0.1F, 5.0F, "m", 2,
                            "The prop's longest side. The model is fitted to it, so one model serves at any "
                            "size.",
                            0.01F),
             choice_control("shape", "Collides as", "model",
                            {{"model", "Its own shape"},
                             {"bucket", "A bucket"},
                             {"crate", "A crate"},
                             {"tube", "A tube"},
                             {"ball", "A ball"},
                             {"block", "A block"}},
                            "What the pieces actually hit. Its own shape follows the model exactly, hollows and "
                            "all, but only a prop that stays put can use it. The rest are worked out from their "
                            "measurements and can move, and a bucket, crate or tube is hollow, so pieces sit "
                            "inside it."),
             when(number_control("wall", "Wall thickness", 0.05F, 0.01F, 0.3F, "m", 2,
                                 "How thick the sides and bottom are. Thinner holds more; thicker survives a "
                                 "harder knock.",
                                 0.001F),
                  "shape", {"bucket", "crate", "tube"}),
             when(number_control("radius", "Radius", 0.5F, 0.05F, 2.0F, "m", 2, "How wide across it is.", 0.01F),
                  "shape", {"bucket", "tube", "ball"}),
             when(number_control("height", "Height", 0.8F, 0.05F, 3.0F, "m", 2, "How tall it is.", 0.01F),
                  "shape", {"bucket", "tube"}),
             when(vec3_control("extent", "Size across", vec3(0.6F, 0.6F, 0.6F), "m",
                               "How wide, tall and deep it is."),
                  "shape", {"crate", "block"}),
             when(vec3_control("move_to", "Moves to", vec3(0.0F, 0.0F, 0.0F), "m",
                               "Where it ends up. The same position leaves it where it is, and only the turn "
                               "changes."),
                  "motion", {"driven"}),
             when(angles_control("turn_to", "Turns to", "How it is turned when the move is done. Past 90 degrees "
                                                        "a bucket is upside down and whatever it held falls out."),
                  "motion", {"driven"}),
             when(number_control("move_at", "Starts moving at", 0.5F, 0.0F, 10.0F, "s", 2,
                                 "When the move begins."),
                  "motion", {"driven"}),
             when(number_control("move_over", "Taking", 0.5F, 0.0F, 10.0F, "s", 2,
                                 "How long the move takes. 0 puts it there at once."),
                  "motion", {"driven"}),
             detail(number_control("friction", "Friction", 0.6F, 0.0F, 1.0F, "", 2,
                                   "How much the pieces grip it.", 0.0F, 1.0F)),
             detail(number_control("bounce", "Bounciness", 0.1F, 0.0F, 1.0F, "", 2,
                                   "How hard the pieces bounce off it.", 0.0F, 1.0F)),
             detail(when(number_control("density", "Density", 600.0F, 100.0F, 8000.0F, "kg/m^3", 0,
                                        "How heavy it is for its size. Wood is about 600, stone 2400.", 1.0F),
                         "motion", {"loose"})),
             detail(when(number_control("sides", "Sides", 12.0F, 3.0F, 48.0F, "", 0,
                                        "How many flat sides a round shape is built from. More is rounder and "
                                        "costs a little more.",
                                        3.0F, 64.0F),
                         "shape", {"bucket", "tube"}))},
        .keywords = "prop object bucket crate wall barrel pipe model import obj gltf glb place static kinematic "
                    "knock over tip spill scenery",
        // Its shape and pose are the physics world's, not a module's.
        .detachable = false,
        .layers = in_rigid,
        .only_in = "A prop is an object in a physics world, so it needs a rigid physics layer.",
    });
    blocks.push_back(BlockDef{
        .kind = "pile",
        .title = "Pile",
        .description = "Solid pieces already stacked when the effect starts, that collapse when it plays.",
        .category = Category::Physics,
        .controls =
            {number_control("count", "Pieces", 150.0F, 1.0F, 1000.0F, "", 0, "How many pieces the pile holds.", 1.0F,
                            20000.0F),
             choice_control("shape", "Stacked as", "heap", {{"heap", "Heap"}, {"stack", "Stack"}},
                            "A heap is a cone of pieces, as rocks or rubble lie. A stack fills a footprint layer "
                            "by layer, as a wall of bricks or a pile of crates."),
             when(number_control("slope", "Slope", 55.0F, 15.0F, 80.0F, "deg", 0,
                                 "How steep the heap's sides are. Steeper than the pieces can hold, and it slumps "
                                 "when let go.",
                                 5.0F, 85.0F),
                  "shape", {"heap"}),
             when(number_control("width", "Width", 1.0F, 0.1F, 5.0F, "m", 2, "The stack's footprint across.", 0.01F),
                  "shape", {"stack"}),
             when(number_control("depth", "Depth", 0.4F, 0.1F, 5.0F, "m", 2, "The stack's footprint front to back.",
                                 0.01F),
                  "shape", {"stack"}),
             number_control("release", "Let go at", 0.0F, 0.0F, 10.0F, "s", 2,
                            "When the pile is let go. Until then it stands still, unless something knocks into "
                            "it; 0 lets it fall as soon as the effect plays."),
             detail(vec3_control("at", "Position", vec3(0.0F, 0.0F, 0.0F), "m",
                                 "Where the middle of the pile's base sits.")),
             detail(number_control("spacing", "Spacing", 1.0F, 0.5F, 2.0F, "x", 2,
                                   "How far apart the pieces start, as a share of the largest piece's reach. Below "
                                   "1 they start packed closer, and may overlap until they fall.",
                                   0.5F, 4.0F)),
             detail(number_control("jitter", "Untidiness", 0.15F, 0.0F, 1.0F, "", 2,
                                   "How far each piece is nudged off its place, so the pile is not a perfect "
                                   "stack.",
                                   0.0F, 1.0F)),
             seed_control()},
        .keywords = "prespawn pre-spawn stack heap rubble rocks wall bricks collapse topple preplaced",
        .layers = in_rigid,
        .only_in = "A pile is stacked solid pieces resting on each other, so it needs a rigid physics layer.",
    });
    blocks.push_back(BlockDef{
        .kind = "physics",
        .title = "Bodies",
        .description = "What every piece in this layer is made of, and how it behaves when it touches another.",
        .category = Category::Physics,
        .controls = {choice_control("shape", "Body", "auto",
                                    {{"auto", "Its own shape"}, {"ball", "Ball"}, {"cube", "Cube"}},
                                    "What each piece collides as. Its own shape wraps a debris chunk tightly; a "
                                    "ball rolls."),
                     number_control("friction", "Friction", 0.6F, 0.0F, 1.0F, "", 2,
                                    "How much pieces grip each other and the ground.", 0.0F, 1.0F),
                     number_control("bounce", "Bounciness", 0.15F, 0.0F, 1.0F, "", 2,
                                    "1 bounces back as fast as it arrived.", 0.0F, 1.0F),
                     detail(number_control("density", "Density", 2400.0F, 100.0F, 8000.0F, "kg/m^3", 0,
                                           "How heavy each piece is for its size. Stone is about 2400.", 1.0F)),
                     detail(number_control("damping", "Spin damping", 0.05F, 0.0F, 2.0F, "", 2,
                                           "How quickly spinning slows in the air.")),
                     detail(toggle_control("rest", "Rest when still", true,
                                           "Pieces that come to rest sleep until something knocks them, which "
                                           "costs nothing while they lie still."))},
        .keywords = "rigid body physics collide stack pile topple jolt solid",
        .unique = true,
        // Part of the layer, so there is nothing to take it apart into.
        .detachable = false,
        // A rigid layer's world, held on the layer (Layer::world), not a
        // block an artist adds: there is one world per rigid layer and no
        // way to be without it. Never offered by the palette or the picker.
        .layers = in_no_layer,
    });

    // ---------------------------------------------------------- light
    blocks.push_back(BlockDef{
        .kind = "light",
        .title = "Cast light",
        .description = "Some particles light the scene around them, each light staying with its particle.",
        .category = Category::Light,
        .controls = {number_control("count", "Lights", 6.0F, 1.0F, 64.0F, "", 0,
                                    "The most lights at once. Particles are picked through the layer, and each "
                                    "keeps its light for its whole life.",
                                    1.0F, 64.0F),
                     range_control("intensity", "Intensity", 1.5F, 2.5F, 0.0F, 20.0F, "", 1,
                                   "Each light's brightness, picked between the two."),
                     range_control("reach", "Reach", 1.2F, 2.0F, 0.05F, 10.0F, "m", 2,
                                   "How far each light carries, picked between the two."),
                     detail(choice_control("over_life", "Over its life", "fade",
                                           {{"steady", "Steady"},
                                            {"fade", "Fades in and out"},
                                            {"flare", "Flares, then fades"},
                                            {"out", "Fades out"}},
                                           "How bright each light is as its particle ages. Fading lets lights come "
                                           "and go without popping.")),
                     detail(choice_control("color_mode", "Light color", "particle",
                                           {{"particle", "The particle's"}, {"own", "Its own"}, {"shift", "Shifts"}},
                                           "Follow the particle's colour, or give the light a colour of its own.")),
                     detail(when(color_control("light_color", "Color", color(1.0F, 0.6F, 0.3F),
                                               "The light's own colour."),
                                 "color_mode", {"own", "shift"})),
                     detail(when(color_control("end_color", "Becomes", color(0.8F, 0.2F, 0.05F),
                                               "The colour the light shifts to by the end of its particle's life."),
                                 "color_mode", {"shift"}))},
        .keywords = "illuminate glow point light lighting",
        .unique = true,
        .detachable = false,
    });

    // --------------------------------------------------------- expert
    blocks.push_back(BlockDef{
        .kind = raw_block_kind,
        .title = "Module",
        .description = "One ajfx module, exactly as the runtime runs it, every field editable.",
        .category = Category::Expert,
        .controls = {},
        .keywords = "raw expert advanced module custom",
        .detachable = false,
    });
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

const std::vector<BlockDef>& block_catalog() {
    static const std::vector<BlockDef> catalog = make_catalog();
    return catalog;
}

const BlockDef* find_block_def(std::string_view kind) {
    for (const BlockDef& def : block_catalog()) {
        if (def.kind == kind) {
            return &def;
        }
    }
    return nullptr;
}

const ControlDef* find_control(const BlockDef& def, std::string_view id) {
    for (const ControlDef& control : def.controls) {
        if (control.id == id) {
            return &control;
        }
    }
    return nullptr;
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

} // namespace

std::string block_title(const Block& block) {
    if (block.kind == raw_block_kind && block.raw) {
        return std::string{raw_title(*block.raw)};
    }
    const BlockDef* def = find_block_def(block.kind);
    return def != nullptr ? std::string{def->title} : block.kind;
}

std::string block_summary(const Block& block) {
    const std::string_view kind = block.kind;
    if (kind == "stream") {
        return format_number(number_of(block, "rate"), 0) + " a second";
    }
    if (kind == "prop") {
        const std::string model = file_label(file_of(block, "model"));
        std::string text = model.empty() ? "no model yet" : model;
        const std::string shape = choice_of(block, "shape");
        if (shape == "bucket") {
            text += ", a bucket";
        } else if (shape == "crate") {
            text += ", a crate";
        } else if (shape == "tube") {
            text += ", a tube";
        } else if (shape == "ball") {
            text += ", a ball";
        } else if (shape == "block") {
            text += ", a block";
        }
        const std::string motion = choice_of(block, "motion");
        if (motion == "driven") {
            text += ", moved";
        } else if (motion == "loose") {
            text += ", loose";
        }
        return text;
    }
    if (kind == "burst") {
        const std::string count = format_number(number_of(block, "count"), 0);
        const float every = number_of(block, "every");
        return every > 0.0F ? count + " every " + with_unit(every, 2, "s") : count + " at once";
    }
    if (kind == "shape") {
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
    if (kind == "launch") {
        std::string text = choice_label(block, "direction") + " at " + format_range(get(block, "speed"), 2, "m/s");
        if (choice_of(block, "direction") != "out") {
            text += ", " + format_range(get(block, "spread"), 0, "deg") + " spread";
        }
        return text;
    }
    if (kind == "gravity") {
        return format_range(get(block, "strength"), 2, "g");
    }
    if (kind == "rise") {
        return format_range(get(block, "strength"), 1, "m/s^2");
    }
    if (kind == "wind") {
        return format_range(get(block, "strength"), 1, "m/s^2") + " toward " +
               with_unit(number_of(block, "heading"), 0, "deg");
    }
    if (kind == "turbulence") {
        std::string text = "Amount " + format_range(get(block, "strength"), 2, "") + ", swirls " +
                           with_unit(number_of(block, "size"), 2, "m");
        if (number_of(block, "roll") > 0.0F) {
            text += ", rolling";
        }
        return text;
    }
    if (kind == "draw_in") {
        std::string text = "Pull " + format_range(get(block, "strength"), 1, "");
        if (number_of(block, "height") > 0.0F) {
            text += ", fades by " + with_unit(number_of(block, "height"), 2, "m");
        }
        return text;
    }
    if (kind == "spin") {
        const Value speed = get(block, "speed");
        return format_number(speed.x, 0) + " to " + with_unit(speed.y, 0, "deg/s");
    }
    if (kind == "drag") {
        return format_range(get(block, "amount"), 2, "");
    }
    if (kind == "swirl") {
        return choice_label(block, "axis") + ", " + format_range(get(block, "strength"), 1, "m/s^2");
    }
    if (kind == "attract") {
        const Value strength = get(block, "strength");
        if (strength.y <= 0.0F) {
            return "Push " + format_range(range(-strength.y, -strength.x), 1, "m/s^2");
        }
        return (strength.x < 0.0F ? "Pull or push " : "Pull ") + format_range(strength, 1, "m/s^2");
    }
    if (kind == "appearance") {
        const std::string material = choice_of(block, "material");
        if (material == "debris") {
            return "Debris, " + (choice_of(block, "mesh") == "file" ? file_label(file_of(block, "mesh_file"))
                                                                   : choice_label(block, "mesh"));
        }
        return choice_label(block, "material") + ", " +
               (choice_of(block, "texture") == "file" ? file_label(file_of(block, "texture_file"))
                                                      : choice_label(block, "texture"));
    }
    if (kind == "color") {
        std::string text = choice_label(block, "over_life");
        const Value brightness = get(block, "brightness");
        if (brightness.x != 1.0F || brightness.y != 1.0F) {
            text += ", brightness " + format_range(brightness, 1, "");
        }
        return text;
    }
    if (kind == "size") {
        const Value size = get(block, "size");
        return format_number(size.x, 3) + " to " + with_unit(size.y, 3, "m") + ", " + choice_label(block, "over_life");
    }
    if (kind == "fade") {
        return "In " + format_number(number_of(block, "in"), 0) + "%, out " + format_number(number_of(block, "out"), 0) +
               "%";
    }
    if (kind == "ground") {
        const std::string response = choice_of(block, "response");
        return response == "bounce" ? "Bounce " + format_number(number_of(block, "bounce"), 2)
                                    : choice_label(block, "response");
    }
    if (kind == "sphere") {
        return (toggle_of(block, "inside") ? "Inside, " : "Outside, ") + with_unit(number_of(block, "radius"), 2, "m");
    }
    if (kind == "box") {
        return toggle_of(block, "inside") ? "Keep inside" : "Keep out";
    }
    if (kind == "mesh_collider") {
        return file_label(file_of(block, "file")) + (toggle_of(block, "inside") ? ", keep inside" : ", keep out");
    }
    if (kind == "tumble") {
        const Value spin = get(block, "spin");
        if (spin.y <= 0.0F) {
            return "Turned every way, no spin";
        }
        return format_number(spin.x, 0) + " to " + with_unit(spin.y, 0, "deg/s");
    }
    if (kind == world_block_kind) {
        return choice_label(block, "shape") + ", friction " + format_number(number_of(block, "friction"), 2) +
               ", bounce " + format_number(number_of(block, "bounce"), 2);
    }
    if (kind == "pile") {
        std::string text = choice_label(block, "shape") + " of " + format_number(number_of(block, "count"), 0);
        const float release = number_of(block, "release");
        return text + (release > 0.0F ? ", let go at " + with_unit(release, 2, "s") : ", falls at once");
    }
    if (kind == "light") {
        const Value reach = get(block, "reach");
        return format_number(number_of(block, "count"), 0) + " lights, reach " + format_number(reach.x, 1) +
               " to " + with_unit(reach.y, 1, "m");
    }
    if (kind == raw_block_kind && block.raw) {
        return std::string{raw_stage(*block.raw)} + ": " + std::string{raw_kind(*block.raw)};
    }
    return {};
}

} // namespace ajfx::studio
