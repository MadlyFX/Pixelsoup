#include "catalog_parts.hpp"

namespace ajfx::studio {

using namespace catalog_controls;

// Motion: the forces on them.
void add_motion_blocks(std::vector<BlockDef>& blocks) {
    blocks.push_back(BlockDef{
        .kind = BlockKind::Gravity,
        .title = "Gravity",
        .description = "Its own share of the page's gravity.",
        .category = Category::Motion,
        .controls = {one_strength_in_rigid(strength_control("strength", "Strength", 1.0F, 0.0F, 3.0F, "x", 2,
                                      "1 is the page's gravity.")),
                     seed_control()},
        .keywords = "fall weight down heavy",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::Rise,
        .title = "Rise",
        .description = "Lifts particles, like heat.",
        .category = Category::Motion,
        .controls = {one_strength_in_rigid(strength_control("strength", "Lift", 2.0F, 0.0F, 20.0F, "m/s^2", 1,
                                      "Upward push, between the two.")),
                     particles_only(detail(choice_control("over_life", "Over its life", "steady",
                                           {{"steady", "Steady"}, {"cools", "Eases as it cools"}, {"builds", "Builds up"}},
                                           "How the lift changes over life."))),
                     seed_control()},
        .keywords = "buoyancy heat float up hot",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::Wind,
        .title = "Wind",
        .description = "A steady sideways push.",
        .category = Category::Motion,
        .controls = {one_strength_in_rigid(strength_control("strength", "Strength", 1.5F, 0.0F, 20.0F, "m/s^2", 1,
                                      "Push, between the two.")),
                     number_control("heading", "Heading", 0.0F, 0.0F, 360.0F, "deg", 0,
                                    "0 is +X, 90 is -Z. Drag its arrow.",
                                    0.0F, 360.0F),
                     seed_control()},
        .keywords = "breeze push sideways direction",
    });
    // A box of air the layer's particles fill with smoke and ride: the
    // runtime's grid volume (ajfx::Volume), in the artist's words. Look
    // settings are read when the smoke is drawn, never by the simulation;
    // none of these publishes, since a volume's fields are not inputs.
    blocks.push_back(BlockDef{
        .kind = BlockKind::SmokeGrid,
        .title = "Smoke grid",
        .description = "A box of air that particles fill with smoke and ride.",
        .category = Category::Motion,
        .controls = {vec3_control("center", "Position", vec3(0.0F, 1.5F, 0.0F), "m", "The centre of the box."),
                     vec3_control("size", "Size", vec3(2.0F, 3.0F, 2.0F), "m", "Width, height, depth."),
                     number_control("cell", "Cell size", 0.1F, 0.02F, 1.0F, "m", 3,
                                    "Smaller cells, finer smoke, more to compute.", 0.01F, 100.0F),
                     number_control("rises", "Rises", 1.0F, 0.0F, 10.0F, "", 2, "Lift per unit of heat.", 0.0F, 100.0F),
                     detail(number_control("sinks", "Sinks", 0.0F, 0.0F, 10.0F, "", 2, "Fall per unit of smoke.", 0.0F, 100.0F)),
                     number_control("swirl", "Swirl", 0.0F, 0.0F, 10.0F, "", 2, "Sharpens the eddies; 0 is off.", 0.0F, 100.0F),
                     number_control("fades", "Fades", 0.2F, 0.0F, 10.0F, "/s", 2, "Smoke and heat lost each second.", 0.0F, 100.0F),
                     detail(number_control("calms", "Calms", 0.0F, 0.0F, 10.0F, "/s", 2, "Motion lost each second.", 0.0F, 100.0F)),
                     detail(number_control("solve", "Solve quality", 20.0F, 1.0F, 80.0F, "", 0,
                                           "Pressure iterations: more keeps the air from piling up.", 1.0F,
                                           static_cast<float>(max_volume_pressure_iterations))),
                     number_control("thickness", "Thickness", 1.0F, 0.0F, 10.0F, "", 2,
                                    "How much light a unit of smoke takes.", 0.0F, 100.0F),
                     color_control("color", "Color", color(1.0F, 1.0F, 1.0F, 1.0F), "The smoke's colour."),
                     number_control("brightness", "Brightness", 0.8F, 0.0F, 1.0F, "", 2,
                                    "The share of the light it takes that it scatters back.", 0.0F, 1.0F)},
        .keywords = "smoke air fluid volume grid gas fog plume",
        .detachable = false,
        .layers = in_particles,
        .only_in = "A rigid physics layer's bodies feed no smoke.",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::Turbulence,
        .title = "Turbulence",
        .description = "Swirling, natural motion.",
        .category = Category::Motion,
        .controls = {publishable(strength_control("strength", "Amount", 1.5F, 0.0F, 10.0F, "", 2,
                                                  "Churn, between the two.")),
                     number_control("size", "Swirl size", 0.6F, 0.05F, 5.0F, "m", 2,
                                    "Small swirls jitter, large ones roll.", 0.01F, 1000.0F),
                     detail(publishable(number_control("churn", "Churn", 0.4F, 0.0F, 3.0F, "", 2,
                                           "How fast the swirls change."))),
                     detail(number_control("roll", "Rolls upward", 0.0F, 0.0F, 5.0F, "m/s", 2,
                                           "Swirls rise at this speed.",
                                           0.0F, 100.0F)),
                     detail(choice_control("over_life", "Over its life", "steady",
                                           {{"steady", "Steady"}, {"grows", "Calm, then breaks up"},
                                            {"settles", "Settles down"}},
                                           "Calm at the base, broken up at the tip.")),
                     seed_control()},
        .keywords = "noise curl swirl chaos organic flow rolling eddy plume",
        .layers = in_particles,
        .only_in = "Bodies move whole.",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::Drag,
        .title = "Air resistance",
        .description = "Slows particles.",
        .category = Category::Motion,
        .controls = {publishable(one_strength_in_rigid(strength_control("amount", "Amount", 1.0F, 0.0F, 10.0F, "", 2,
                                                  "Speed lost per second, between the two."))),
                     seed_control()},
        .keywords = "drag friction slow damping",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::Swirl,
        .title = "Swirl",
        .description = "Spins particles around an axis.",
        .category = Category::Motion,
        .controls = {publishable(strength_control("strength", "Strength", 3.0F, -20.0F, 20.0F, "m/s^2", 1,
                                                  "Between the two. Negative turns the other way.",
                                                  -1.0e6F)),
                     choice_control("axis", "Axis", "vertical", {{"vertical", "Vertical"}, {"horizontal", "Horizontal"}},
                                    "The axis they turn around."),
                     detail(vec3_control("center", "Center", vec3(0.0F, 0.0F, 0.0F), "m",
                                         "Where the axis passes. Drag its dot.")),
                     seed_control()},
        .keywords = "vortex tornado spin orbit whirl",
        .layers = in_particles,
        .only_in = "Bodies move whole.",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::DrawIn,
        .title = "Draw in",
        .description = "Pulls particles toward the middle.",
        .category = Category::Motion,
        .controls = {strength_control("strength", "Pull", 6.0F, 0.0F, 30.0F, "/s^2", 1,
                                      "Pull per metre away, between the two."),
                     number_control("height", "Fades by", 1.0F, 0.0F, 5.0F, "m", 2,
                                    "Pull fades out by this height. 0 is none.", 0.0F,
                                    1000.0F),
                     detail(vec3_control("center", "Center", vec3(0.0F, 0.0F, 0.0F), "m",
                                         "Base of the line. Drag its dot.")),
                     seed_control()},
        .keywords = "converge inflow gather pinch tongue entrain fire",
        .unique = true,
        .layers = in_particles,
        .only_in = "Bodies move whole.",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::Attract,
        .title = "Attract",
        .description = "Pulls or pushes from a point.",
        .category = Category::Motion,
        .controls = {publishable(strength_control("strength", "Strength", 4.0F, -30.0F, 30.0F, "m/s^2", 1,
                                                  "Between the two. Negative pushes.",
                                                  -1.0e6F)),
                     vec3_control("point", "Point", vec3(0.0F, 1.0F, 0.0F), "m",
                                  "Where they are pulled to. Drag its dot."),
                     detail(publishable(number_control("reach", "Reach", 0.0F, 0.0F, 10.0F, "m", 2,
                                           "No pull beyond this. 0 reaches everywhere."))),
                     seed_control()},
        .keywords = "attractor magnet pull push repel gather",
        .layers = in_particles,
        .only_in = "Bodies move whole.",
    });
    // One blast for the whole page: whichever layer holds it, it throws
    // every layer's pieces and particles out from where it goes off, and
    // breaks what is set to break when blasted (Break's "Breaks when
    // blasted at"). A layer that passes through is left alone.
    blocks.push_back(BlockDef{
        .kind = BlockKind::Explosion,
        .title = "Explosion",
        .description = "Throws everything near it outward.",
        .category = Category::Motion,
        .controls = {vec3_control("at", "Position", vec3(0.0F, 0.5F, 0.0F), "m",
                                  "Where it goes off. Drag its dot."),
                     number_control("power", "Power", 12.0F, 1.0F, 60.0F, "m/s", 1,
                                    "Speed it throws at its middle.", 0.0F),
                     number_control("radius", "Radius", 3.0F, 0.2F, 20.0F, "m", 2,
                                    "How far it reaches.", 0.01F),
                     number_control("time", "Goes off at", 0.5F, 0.0F, 10.0F, "s", 2,
                                    "When it goes off.")},
        .keywords = "explosion explode blast bomb boom shockwave detonate push throw blow up",
        // It acts on every layer, not on the modules of one: there is
        // nothing of its own to take apart.
        .detachable = false,
    });
    // A shove at one point, one way: what the preview's Push tool writes.
    // It compiles to a small blast set behind its point (page_blasts), so
    // what stands at the point is thrown along "Toward" at the Power given,
    // and whatever is near it a little less.
    blocks.push_back(BlockDef{
        .kind = BlockKind::Push,
        .title = "Push",
        .description = "A short shove at one point.",
        .category = Category::Motion,
        .controls = {vec3_control("at", "Position", vec3(0.0F, 0.5F, 0.0F), "m", "What it pushes. Drag its dot."),
                     vec3_control("toward", "Toward", vec3(0.0F, 0.0F, -1.0F), "", "The way it pushes."),
                     number_control("power", "Power", 4.0F, 0.5F, 30.0F, "m/s", 1, "Speed it gives there.", 0.0F),
                     number_control("radius", "Reach", 0.5F, 0.1F, 5.0F, "m", 2, "How far round it reaches.", 0.01F),
                     number_control("time", "At", 0.5F, 0.0F, 10.0F, "s", 2, "When it pushes.")},
        .keywords = "push shove nudge kick knock poke hit impulse",
        .detachable = false,
    });
    // Smoke pushed aside by a thrown crate, dust dragged behind a rolling
    // rock (ajfx::Wake). Smoke and dust get one by default where the page's
    // solid things move; this block sets it, or stirs another layer.
    blocks.push_back(BlockDef{
        .kind = BlockKind::Stirred,
        .title = "Stirred by moving pieces",
        .description = "Moving solid things push these along.",
        .category = Category::Motion,
        .controls = {solid_layer_control(),
                     publishable(strength_control("strength", "Strength", 4.0F, 0.0F, 20.0F, "", 1,
                                                  "How fast they take up the pieces' speed.")),
                     number_control("reach", "Reach", 4.0F, 0.5F, 10.0F, "x", 1, "In half sizes of a piece.", 0.1F,
                                    100.0F),
                     detail(number_control("min_speed", "Moving from", 0.1F, 0.0F, 5.0F, "m/s", 2,
                                           "Slower pieces stir nothing.", 0.0F, 1.0e4F))},
        .keywords = "wake stir push drag along turbulence moving pieces bodies crate rock smoke dust",
        .unique = true,
        .layers = in_particles,
        .only_in = "Moved by solid things.",
    });
}

} // namespace ajfx::studio
