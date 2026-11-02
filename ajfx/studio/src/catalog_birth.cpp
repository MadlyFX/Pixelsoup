#include "catalog_parts.hpp"

namespace ajfx::studio {

using namespace catalog_controls;

// Birth: how many particles, where they start and how they leave.
void add_birth_blocks(std::vector<BlockDef>& blocks) {
    blocks.push_back(BlockDef{
        .kind = BlockKind::Stream,
        .title = "Stream",
        .description = "Particles born steadily.",
        .category = Category::Birth,
        .controls = with_controls({publishable(number_control("rate", "Amount", 30.0F, 0.0F, 500.0F, "/s", 0,
                                                              "Particles per second.", 0.0F, 1.0e6F))},
                                  piece_model_controls()),
        .keywords = "emit spawn rate continuous flow",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::Burst,
        .title = "Burst",
        .description = "Many particles at once.",
        .category = Category::Birth,
        .controls = with_controls(
            {number_control("count", "Count", 60.0F, 1.0F, 1000.0F, "", 0, "Particles per burst.", 1.0F, 1.0e6F),
             number_control("time", "At", 0.0F, 0.0F, 10.0F, "s", 2, "When the first burst goes off."),
             number_control("every", "Repeat every", 0.0F, 0.0F, 10.0F, "s", 2,
                            "Seconds between bursts. 0 is once."),
             detail(number_control("repeats", "Bursts", 0.0F, 0.0F, 20.0F, "", 0,
                                   "Bursts when repeating. 0 is for ever.", 0.0F, 1.0e6F))},
            piece_model_controls()),
        .keywords = "emit spawn explosion pop once",
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::Shape,
        .title = "Spawn shape",
        .description = "Where particles are born.",
        .category = Category::Birth,
        .controls = {choice_control("shape", "Shape", "ball",
                                    {{"point", "Point"},
                                     {"ball", "Ball"},
                                     {"shell", "Shell"},
                                     {"box", "Box"},
                                     {"disc", "Disc"},
                                     {"ring", "Ring"}},
                                    "Shell and Ring use the edge only."),
                     when(publishable(range_control("size", "Radius", 0.2F, 0.2F, 0.0F, 3.0F, "m", 2,
                                                    "Each particle's radius, between the two.")),
                          "shape", {"point", "box"}, true),
                     when(vec3_control("box", "Box size", vec3(0.4F, 0.4F, 0.4F), "m", "Width, height, depth."),
                          "shape", {"box"}),
                     detail(vec3_control("offset", "Offset", vec3(0.0F, 0.0F, 0.0F), "m",
                                         "Moves the shape. Drag its dot.")),
                     seed_control()},
        .keywords = "location position emitter area volume sphere circle",
        .unique = true,
    });
    blocks.push_back(BlockDef{
        .kind = BlockKind::Launch,
        .title = "Launch",
        .description = "Speed and direction at birth.",
        .category = Category::Birth,
        .controls = {choice_control("direction", "Direction", "up",
                                    {{"up", "Up"}, {"out", "Outward"}, {"down", "Down"}, {"side", "Sideways"},
                                     {"custom", "Custom"}},
                                    "Out goes every way."),
                     publishable(range_control("speed", "Speed", 1.0F, 2.0F, 0.0F, 20.0F, "m/s", 2,
                                   "Each particle's speed, between the two.")),
                     when(spread_control(), "direction", {"out"}, true),
                     when(detail(vec3_control("custom", "Aim", vec3(0.0F, 1.0F, 0.0F), "", "The direction to aim.")),
                          "direction", {"custom"}),
                     seed_control()},
        .keywords = "velocity speed throw shoot initial direction cone",
        .unique = true,
    });
    // Born where the page's solid things hit: soft puffs that hang near the
    // hit, or sparks thrown off it. One runtime module, two starting points.
    const auto hits = [](BlockKind kind, std::string_view title, std::string_view description, float hardness,
                         float per_hit, float per_speed, float spread, float up, float across,
                         std::string_view keywords) {
        return BlockDef{
            .kind = kind,
            .title = title,
            .description = description,
            .category = Category::Birth,
            .controls = {solid_layer_control(),
                         number_control("hardness", "Hits from", hardness, 0.0F, 10.0F, "m/s", 1,
                                        "Softer hits make none.", 0.0F, 1.0e4F),
                         number_control("per_hit", "Per hit", per_hit, 0.0F, 50.0F, "", 0, "Born at every hit.",
                                        0.0F, 1.0e5F),
                         number_control("per_speed", "Per m/s", per_speed, 0.0F, 50.0F, "", 1,
                                        "More for harder hits.", 0.0F, 1.0e5F),
                         number_control("spread", "Spread", spread, 0.0F, 180.0F, "deg", 0,
                                        "Scatter around the bounce.", 0.0F, 180.0F),
                         detail(number_control("up", "Thrown up", up, 0.0F, 2.0F, "x", 2,
                                               "Share of the hit's speed, away from it.", 0.0F, 100.0F)),
                         detail(number_control("across", "Thrown across", across, 0.0F, 2.0F, "x", 2,
                                               "Share of the hit's speed, along it.", 0.0F, 100.0F)),
                         detail(toggle_control("new_only", "New hits only", true, "Off: sliding makes them too.")),
                         detail(number_control("most", "Most a step", 64.0F, 1.0F, 512.0F, "", 0,
                                               "Caps a big crash.", 1.0F, 1.0e5F)),
                         seed_control()},
            .keywords = keywords,
            .layers = in_particles,
            .only_in = "Born of solid things.",
        };
    };
    blocks.push_back(hits(BlockKind::HitPuffs, "Puffs where things hit", "Dust where solid things land.", 1.0F,
                          2.0F, 3.0F, 70.0F, 0.15F, 0.3F,
                          "contact impact collision landing dust puff smoke debris hit"));
    blocks.push_back(hits(BlockKind::HitSparks, "Sparks where things hit", "Sparks where solid things strike.",
                          2.0F, 4.0F, 6.0F, 35.0F, 0.6F, 0.4F,
                          "contact impact collision strike metal sparks hit"));
    blocks.push_back(BlockDef{
        .kind = BlockKind::Trail,
        .title = "Trail",
        .description = "Shed behind moving solid things.",
        .category = Category::Birth,
        .controls = {solid_layer_control(),
                     toggle_control("pieces", "Broken pieces only", false, "Only what broke off."),
                     number_control("per_speed", "Per m/s", 10.0F, 0.0F, 100.0F, "/s", 1, "More behind faster ones.",
                                    0.0F, 1.0e5F),
                     detail(number_control("min_speed", "Moving from", 0.5F, 0.0F, 10.0F, "m/s", 2,
                                           "Slower ones shed none.", 0.0F, 1.0e4F)),
                     detail(number_control("rate", "Base amount", 0.0F, 0.0F, 100.0F, "/s", 1,
                                           "Per second, while moving.", 0.0F, 1.0e5F)),
                     detail(number_control("surface", "From the surface", 1.0F, 0.0F, 1.0F, "", 2,
                                           "0 is the middle.", 0.0F, 1.0F)),
                     detail(number_control("inherit", "Keeps their speed", 0.0F, 0.0F, 1.0F, "x", 2,
                                           "Share of the speed they leave at.", 0.0F, 1.0F)),
                     detail(number_control("most", "Most a step", 64.0F, 1.0F, 512.0F, "", 0, "Caps a big crash.",
                                           1.0F, 1.0e5F)),
                     seed_control()},
        .keywords = "trail smoke embers moving bodies pieces fragments wake streak behind",
        .layers = in_particles,
        .only_in = "Born of solid things.",
    });
}

} // namespace ajfx::studio
